#!/usr/bin/env node
// Verify the in-compiler compile cache of a built GCC tree.
//
// Usage:  node ci-verify-cache.mjs <gcc-build-dir>
//         GCC_BUILD_DIR=<dir> node ci-verify-cache.mjs
//
// <gcc-build-dir> is the out-of-tree GCC build directory (the one that
// contains the "gcc" subdir with xgcc/xg++). The script drives those
// freshly built compilers, exercises the compile cache, and exits 0 with
// "CACHE VERIFY OK" on success or 1 with a clear message on any violation.
//
// Node ESM, standard-library only (no external deps).

import { spawnSync } from 'node:child_process';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';

function usage() {
  process.stderr.write(
    'usage: node ci-verify-cache.mjs <gcc-build-dir>\n' +
    '       (or set GCC_BUILD_DIR)\n'
  );
  process.exit(2);
}

const buildDir = process.argv[2] || process.env.GCC_BUILD_DIR;
if (!buildDir) usage();

const gccDir = path.join(buildDir, 'gcc');
const XGCC = path.join(gccDir, 'xgcc');
const XGPP = path.join(gccDir, 'xg++');
const B = '-B' + gccDir;

for (const drv of [XGCC, XGPP]) {
  if (!fs.existsSync(drv)) {
    process.stderr.write('error: driver not found: ' + drv + '\n');
    process.exit(2);
  }
}

// Fail the whole verification with a clear message.
function fail(msg) {
  process.stderr.write('CACHE VERIFY FAILED: ' + msg + '\n');
  process.exit(1);
}

// Parse every "compile-cache: <action> <key12> <output>" debug line from
// stderr into [{ action, key }, ...]. action is miss/store/hit, key is the
// first 12 hex chars (or "-").
//
// Stage 5 caches the in-process-assembled .o and serves it BEFORE parse via a
// ccache-style direct-mode manifest, so a warm compile logs "manifest-hit"
// (the pre-parse object serve) instead of the post-parse "hit". Both carry the
// same content key (OK) and place a byte-identical object, so for these checks
// a "manifest-hit" IS a hit -- normalize it. The manifest-layer "manifest-miss"
// / "manifest-store" lines (keyed on the separate manifest key MK) are NOT
// folded into miss/store: on a cold compile they appear ALONGSIDE the real
// content-level miss/store, and the checks below assert on the content key, so
// they are deliberately left unmatched by this regex.
function parseKeys(stderr) {
  const re = /compile-cache: (manifest-hit|miss|store|hit) ([0-9a-f]+|-)/g;
  const out = [];
  let m;
  while ((m = re.exec(stderr)) !== null) {
    const action = m[1] === 'manifest-hit' ? 'hit' : m[1];
    out.push({ action, key: m[2] });
  }
  return out;
}

const debugEnv = { ...process.env, GCC_COMPILE_CACHE_DEBUG: '1' };

// Write a header file and advance its mtime so a content change is reliably
// detected by the manifest fast-path. The pre-parse manifest serve records
// each header's size+mtime and accepts a candidate set on a size+mtime stat
// match WITHOUT re-hashing (a deliberate, documented ccache-style shortcut;
// GCC_COMPILE_CACHE_VERIFY=hash forces a full re-hash instead). A real edit
// always advances mtime, but an instantaneous same-size rewrite in a test may
// land in the same coarse mtime tick, which would let the shortcut correctly
// accept the OLD content and serve a stale object. Bumping mtime mirrors a
// real edit and keeps the invalidation check exercising the DEFAULT fast path
// deterministically (rather than only the airtight verify-hash mode).
let mtimeNudgeSecs = 0;
function writeHeader(p, content) {
  fs.writeFileSync(p, content);
  mtimeNudgeSecs += 2;
  const t = new Date(Date.now() + mtimeNudgeSecs * 1000);
  fs.utimesSync(p, t, t);
}

// Compile SRC -> OBJ with -O2 -c -fcompile-cache=CACHEDIR (plus optional
// EXTRA driver args). Returns { status, stderr, keys } where keys is the
// parsed list of cache actions. EXTRA is genuinely optional.
//
// -fintegrated-as is REQUIRED: Stage 5 caches and serves the in-process
// (integrated-as) assembled object, and gates itself off entirely without it
// (compile_cache_enabled_p() -> false, so NO miss/store/hit lines are emitted).
// It is not the default on this target, so every cache-exercising compile must
// pass it explicitly or the cache stays disabled and every check sees nothing.
function compile(driver, src, obj, cacheDir, extra) {
  const args = [
    '-O2', '-c', src, '-o', obj,
    '-fcompile-cache=' + cacheDir, '-fintegrated-as', B,
  ].concat(extra || []);
  const res = spawnSync(driver, args, {
    env: debugEnv,
    encoding: 'utf8',
  });
  if (res.error) {
    fail('failed to spawn ' + driver + ': ' + res.error.message);
  }
  const stderr = res.stderr || '';
  if (res.status !== 0) {
    fail(
      'compile failed (' + path.basename(driver) + '): exit ' + res.status +
      '\n--- args ---\n' + args.join(' ') +
      '\n--- stderr ---\n' + stderr
    );
  }
  return { status: res.status, stderr, keys: parseKeys(stderr) };
}

// Find the (first) key for a given action in a parsed key list.
function keyFor(keys, action) {
  const e = keys.find((k) => k.action === action);
  return e ? e.key : null;
}

// Link OBJ into EXE with the freshly built driver and run it; return the
// program's exit status. EXTRA_LINK_ARGS (optional) are appended to the link
// command; RUN_ENV (optional) is the environment for the run (defaults to the
// current process env).
function linkAndRun(driver, obj, exe, extraLinkArgs, runEnv) {
  const args = [obj, '-o', exe, B].concat(extraLinkArgs || []);
  const link = spawnSync(driver, args, {
    env: process.env,
    encoding: 'utf8',
  });
  if (link.error) fail('failed to spawn linker ' + driver + ': ' + link.error.message);
  if (link.status !== 0) {
    fail(
      'link failed (' + path.basename(driver) + '): exit ' + link.status +
      '\n' + (link.stderr || '')
    );
  }
  const run = spawnSync(exe, [], {
    encoding: 'utf8',
    env: runEnv || process.env,
  });
  if (run.error) fail('failed to run ' + exe + ': ' + run.error.message);
  return run.status;
}

function readObj(p) {
  return fs.readFileSync(p);
}

// Locate the directory holding the freshly built libstdc++.so inside the
// out-of-tree build (e.g. <build>/<target>/libstdc++-v3/src/.libs). An
// uninstalled in-tree xg++ does not know where its own libstdc++ lives, so a
// C++ link needs this on -L and the run needs it on LD_LIBRARY_PATH. Returns
// the dir, or null if not found.
function findLibstdcxxDir(root) {
  const target = path.join(root, '*', 'libstdc++-v3', 'src', '.libs', 'libstdc++.so');
  let hits = [];
  try {
    hits = fs.globSync(target);
  } catch {
    hits = [];
  }
  if (hits.length === 0) {
    // Fallback: a broad recursive search for libstdc++.so under the build dir.
    const stack = [root];
    while (stack.length && hits.length === 0) {
      const dir = stack.pop();
      let ents;
      try {
        ents = fs.readdirSync(dir, { withFileTypes: true });
      } catch {
        continue;
      }
      for (const ent of ents) {
        const full = path.join(dir, ent.name);
        if (ent.isDirectory()) {
          stack.push(full);
        } else if (ent.name === 'libstdc++.so') {
          hits.push(full);
          break;
        }
      }
    }
  }
  return hits.length ? path.dirname(hits[0]) : null;
}

// --- structured binary cache-object format -------------------------------
// One little-endian binary file per entry at DIR/<2hex>/<rest>.bin.
const CC_MAGIC = 'GCCCACHE';
// Object-header format version. Bumped to 2 when the assembled-object payload
// moved out of the .bin and into a sibling ".o" sidecar (Stage 5 .o cache):
// the .bin now carries only metadata + diagnostics, and its asm section is
// empty (asm_len == 0). Field offsets are unchanged from v1.
const CC_FORMAT_VERSION = 2;
const CC_MANIFEST_MAGIC = 'CCMANIFS';
const CC_HEADER_SIZE = 128;
// Fixed-header field byte offsets (must match gcc/compile-cache.cc).
const H = {
  MAGIC: 0, FORMAT_VER: 8, FLAGS: 10, INPUT_COUNT: 12, CREATED: 16,
  ASM_OFF: 24, ASM_LEN: 32, DIAG_OFF: 40, DIAG_LEN: 48, INPUTS_OFF: 56,
  WARNINGS: 64, ERRORS: 68, KEY: 72, CHECKSUM: 92,
  SOURCE_OFF: 108, CWD_OFF: 112, TARGET_OFF: 116, LANGUAGE_OFF: 120,
  OPTIONS_OFF: 124,
};
const CC_INPUT_REC_SIZE = 32;
const IN = { HASH: 0, SIZE: 20, PATH: 28 };

// Recursively list every regular file under DIR.
function listFiles(dir) {
  const out = [];
  const stack = [dir];
  while (stack.length) {
    const d = stack.pop();
    let ents;
    try {
      ents = fs.readdirSync(d, { withFileTypes: true });
    } catch {
      continue;
    }
    for (const e of ents) {
      const full = path.join(d, e.name);
      if (e.isDirectory()) stack.push(full);
      else if (e.isFile()) out.push(full);
    }
  }
  return out;
}

// Read a length-prefixed + NUL string referenced by a u32 file offset that
// points at the u32 length prefix. Returns the string content (no NUL).
function readRefString(buf, off, label) {
  if (off + 4 > buf.length) {
    fail('binary: ' + label + ' offset ' + off + ' out of range');
  }
  const slen = buf.readUInt32LE(off);
  const start = off + 4;
  const end = start + slen;
  if (end + 1 > buf.length) {
    fail('binary: ' + label + ' string [' + start + ',' + end + ') out of range');
  }
  if (buf[end] !== 0) {
    fail('binary: ' + label + ' string is not NUL-terminated at ' + end);
  }
  return buf.toString('utf8', start, end);
}

// Parse + validate ONE .bin cache object. Returns a small summary object.
// Verbose-prints the header hexdump, the decoded source path, and one decoded
// input path.
function parseAndValidateObject(binPath) {
  const buf = fs.readFileSync(binPath);
  if (buf.length < CC_HEADER_SIZE) {
    fail('binary: ' + binPath + ' is shorter than the 128-byte header');
  }
  const magic = buf.toString('latin1', H.MAGIC, H.MAGIC + 8);
  if (magic !== CC_MAGIC) {
    fail('binary: bad magic ' + JSON.stringify(magic) + ' (want "GCCCACHE")');
  }
  const ver = buf.readUInt16LE(H.FORMAT_VER);
  if (ver !== CC_FORMAT_VERSION) {
    fail('binary: format_version ' + ver + ' != ' + CC_FORMAT_VERSION);
  }
  const inputCount = buf.readUInt32LE(H.INPUT_COUNT);
  const asmOff = Number(buf.readBigUInt64LE(H.ASM_OFF));
  const asmLen = Number(buf.readBigUInt64LE(H.ASM_LEN));
  const diagOff = Number(buf.readBigUInt64LE(H.DIAG_OFF));
  const diagLen = Number(buf.readBigUInt64LE(H.DIAG_LEN));
  const inputsOff = Number(buf.readBigUInt64LE(H.INPUTS_OFF));

  // Section spans must fit inside the file.
  if (asmOff + asmLen > buf.length) {
    fail('binary: asm span [' + asmOff + ',' + (asmOff + asmLen) +
      ') exceeds file size ' + buf.length);
  }
  if (diagOff + diagLen > buf.length) {
    fail('binary: diag span [' + diagOff + ',' + (diagOff + diagLen) +
      ') exceeds file size ' + buf.length);
  }
  if (inputsOff + inputCount * CC_INPUT_REC_SIZE > buf.length) {
    fail('binary: inputs table exceeds file size');
  }

  // Follow source_off -> length-prefixed + NUL string.
  const sourceOff = buf.readUInt32LE(H.SOURCE_OFF);
  const source = readRefString(buf, sourceOff, 'source_off');

  // Walk the inputs table and decode one path via its path_off.
  let firstInputPath = null;
  for (let i = 0; i < inputCount; i++) {
    const rec = inputsOff + i * CC_INPUT_REC_SIZE;
    const pathOff = buf.readUInt32LE(rec + IN.PATH);
    const p = readRefString(buf, pathOff, 'input[' + i + '].path_off');
    if (i === 0) firstInputPath = p;
  }

  return {
    buf, magic, ver, inputCount, asmOff, asmLen, diagOff, diagLen, inputsOff,
    sourceOff, source, firstInputPath,
    warnings: buf.readUInt32LE(H.WARNINGS),
    errors: buf.readUInt32LE(H.ERRORS),
  };
}

// ---------------------------------------------------------------------------
const work = fs.mkdtempSync(path.join(os.tmpdir(), 'cc-verify-'));

const incH = path.join(work, 'inc.h');
const aC = path.join(work, 'a.c');
const tCpp = path.join(work, 't.cpp');

fs.writeFileSync(incH, '#define VAL 42\nint helper(void);\n');
fs.writeFileSync(
  aC,
  '#include "inc.h"\n' +
  'static int s(void){return VAL;}\n' +
  'int helper(void){return s();}\n' +
  'int main(void){return helper();}\n'
);
fs.writeFileSync(
  tCpp,
  'namespace {\n' +
  '  int anon_fn(int x){ return x + 1; }\n' +
  '}\n' +
  'static int file_static(int x){ return x * 2; }\n' +
  'template <typename T> T tadd(T a, T b){ return a + b; }\n' +
  'int main(){\n' +
  '  int v = anon_fn(2) + file_static(1) + tadd<int>(1, 1);\n' +
  '  return v == 7 ? 7 : 1;\n' +
  '}\n'
);

// ---- 1. miss + store ------------------------------------------------------
const cacheA = path.join(work, 'cacheA');
const o1 = path.join(work, 'a1.o');
{
  const r = compile(XGCC, aC, o1, cacheA);
  const missIdx = r.keys.findIndex((k) => k.action === 'miss');
  const storeIdx = r.keys.findIndex((k) => k.action === 'store');
  if (missIdx === -1) fail('check 1: expected a "miss" on first compile\n' + r.stderr);
  if (storeIdx === -1) fail('check 1: expected a "store" on first compile\n' + r.stderr);
  if (!(missIdx < storeIdx)) {
    fail('check 1: expected "miss" to precede "store"\n' + r.stderr);
  }
  const missKey = r.keys[missIdx].key;
  const storeKey = r.keys[storeIdx].key;
  if (!/^[0-9a-f]{12}$/.test(missKey)) {
    fail('check 1: miss key is not 12 hex chars: ' + missKey);
  }
  if (missKey !== storeKey) {
    fail('check 1: store key ' + storeKey + ' != miss key ' + missKey);
  }
  process.stdout.write('check 1 OK: miss+store, key=' + missKey + '\n');
}

// The content key for the VAL=42 build, established by check 1's miss/store.
const KEY42 = (() => {
  const probe = compile(XGCC, aC, path.join(work, 'aprobe.o'), cacheA);
  // cacheA is already populated, so this is a hit; its key is the content key.
  return keyFor(probe.keys, 'hit');
})();
if (!/^[0-9a-f]{12}$/.test(KEY42 || '')) {
  fail('could not determine stable key for VAL=42 build: ' + KEY42);
}

// ---- 2. hit + byte-identical ---------------------------------------------
const o2 = path.join(work, 'a2.o');
{
  const r = compile(XGCC, aC, o2, cacheA);
  const hitKey = keyFor(r.keys, 'hit');
  if (!hitKey) fail('check 2: expected a "hit" on re-compile\n' + r.stderr);
  if (hitKey !== KEY42) {
    fail('check 2: hit key ' + hitKey + ' != original key ' + KEY42);
  }
  if (Buffer.compare(readObj(o1), readObj(o2)) !== 0) {
    fail('check 2: cache hit produced a non-identical object');
  }
  process.stdout.write('check 2 OK: hit + byte-identical object\n');
}

// ---- 3. determinism: fresh empty cache -> miss, identical object ----------
const cacheFresh = path.join(work, 'cacheFresh');
const o3 = path.join(work, 'a3.o');
{
  const r = compile(XGCC, aC, o3, cacheFresh);
  if (!keyFor(r.keys, 'miss')) {
    fail('check 3: expected a "miss" in a fresh empty cache\n' + r.stderr);
  }
  if (Buffer.compare(readObj(o1), readObj(o3)) !== 0) {
    fail('check 3: non-deterministic object across fresh caches');
  }
  process.stdout.write('check 3 OK: deterministic across a fresh empty cache\n');
}

// ---- 4. invalidation: change inc.h -> different key -----------------------
{
  // writeHeader bumps mtime so the manifest stat-shortcut sees the change.
  writeHeader(incH, '#define VAL 43\nint helper(void);\n');
  const o4 = path.join(work, 'a4.o');
  const r = compile(XGCC, aC, o4, cacheA);
  const missKey = keyFor(r.keys, 'miss');
  if (!missKey) {
    fail('check 4: expected a "miss" after editing inc.h\n' + r.stderr);
  }
  if (missKey === KEY42) {
    fail('check 4: key did not change after inc.h was edited (' +
      KEY42 + ' vs ' + missKey + ')');
  }
  process.stdout.write('check 4 OK: key changed on input change: ' +
    KEY42 + ' -> ' + missKey + '\n');
  // Restore VAL=42 for the functional check (mtime-bumped so the next compile
  // resolves the VAL=42 content again rather than a stale VAL=43 stat match).
  writeHeader(incH, '#define VAL 42\nint helper(void);\n');
}

// ---- 5. functional: link and run, expect exit 42 -------------------------
{
  const oFunc = path.join(work, 'afunc.o');
  compile(XGCC, aC, oFunc, cacheA); // hit (VAL=42 restored)
  const exe = path.join(work, 'a.out');
  const code = linkAndRun(XGCC, oFunc, exe);
  if (code !== 42) {
    fail('check 5: linked program exit code was ' + code + ', expected 42');
  }
  process.stdout.write('check 5 OK: linked program returned 42\n');
}

// ---- 6. C++ miss -> hit -> byte-identical, then link + run -> 7 -----------
{
  const cppCache = path.join(work, 'cppcache');
  const co1 = path.join(work, 't1.o');
  const co2 = path.join(work, 't2.o');

  const r1 = compile(XGPP, tCpp, co1, cppCache);
  const cppMiss = r1.keys.findIndex((k) => k.action === 'miss');
  const cppStore = r1.keys.findIndex((k) => k.action === 'store');
  if (cppMiss === -1) fail('check 6: C++ expected a "miss" on first compile\n' + r1.stderr);
  if (cppStore === -1) fail('check 6: C++ expected a "store" on first compile\n' + r1.stderr);
  const cppKey = r1.keys[cppMiss].key;

  const r2 = compile(XGPP, tCpp, co2, cppCache);
  const cppHit = keyFor(r2.keys, 'hit');
  if (!cppHit) fail('check 6: C++ expected a "hit" on second compile\n' + r2.stderr);
  if (cppHit !== cppKey) {
    fail('check 6: C++ hit key ' + cppHit + ' != miss key ' + cppKey);
  }
  if (Buffer.compare(readObj(co1), readObj(co2)) !== 0) {
    fail('check 6: C++ cache hit produced a non-identical object');
  }
  // The in-tree xg++ does not know where its own (uninstalled) libstdc++ is.
  const libDir = findLibstdcxxDir(buildDir);
  if (!libDir) {
    fail('check 6: could not locate libstdc++.so under ' + buildDir);
  }
  const runEnv = {
    ...process.env,
    LD_LIBRARY_PATH: process.env.LD_LIBRARY_PATH
      ? libDir + ':' + process.env.LD_LIBRARY_PATH
      : libDir,
  };
  const exe = path.join(work, 't.out');
  const code = linkAndRun(XGPP, co1, exe, ['-L' + libDir], runEnv);
  if (code !== 7) {
    fail('check 6: C++ linked program exit code was ' + code + ', expected 7');
  }
  process.stdout.write('check 6 OK: C++ miss->hit, byte-identical, program returned 7\n');
}

// ---- 7. on-disk layout: object .bin + .o sidecar + manifest + compiler-id --
{
  // cacheA holds (at least) the VAL=42 and VAL=43 entries. The Stage 5 .o
  // cache layout under a cache dir is:
  //   <2hex>/<rest>.bin  -- an OBJECT (magic "GCCCACHE"): metadata + diags,
  //                         asm section empty (payload externalized);
  //   <2hex>/<rest>.o    -- that object's payload SIDECAR (placed on a hit via
  //                         reflink/hardlink/copy);
  //   <2hex>/<rest>.bin  -- a MANIFEST (magic "CCMANIFS"): the direct-mode
  //                         index; has NO .o sidecar;
  //   compiler-id        -- the driver's pre-parse compiler-identity sidecar.
  // The .o sidecars and compiler-id are REQUIRED artifacts of this design, not
  // forbidden extras, so this check validates those invariants rather than
  // rejecting non-.bin files (which the pre-Stage-5 .s cache did).
  const files = listFiles(cacheA);
  if (files.length === 0) {
    fail('check 7: no cache files found under ' + cacheA);
  }

  // Read a file's 8-byte magic (latin1), or '' if too short.
  const magicOf = (f) => {
    const b = fs.readFileSync(f);
    return b.length >= 8 ? b.toString('latin1', 0, 8) : '';
  };

  const binFiles = files.filter((f) => f.endsWith('.bin'));
  const oFiles = files.filter((f) => f.endsWith('.o'));
  const idFiles = files.filter((f) => path.basename(f) === 'compiler-id');
  const unexpected = files.filter(
    (f) => !f.endsWith('.bin') && !f.endsWith('.o') &&
      path.basename(f) !== 'compiler-id');
  if (unexpected.length) {
    fail('check 7: found unexpected cache file(s) (not .bin/.o/compiler-id):\n  ' +
      unexpected.join('\n  '));
  }

  // The driver's compiler-id sidecar must exist.
  if (idFiles.length !== 1) {
    fail('check 7: expected exactly one compiler-id sidecar, got ' +
      idFiles.length);
  }

  // Split .bin files into objects (GCCCACHE) and manifests (CCMANIFS).
  const objBins = [];
  const manBins = [];
  for (const f of binFiles) {
    const mg = magicOf(f);
    if (mg === CC_MAGIC) objBins.push(f);
    else if (mg === CC_MANIFEST_MAGIC) manBins.push(f);
    else fail('check 7: .bin with unknown magic ' + JSON.stringify(mg) +
      ': ' + f);
  }
  if (objBins.length < 2) {
    fail('check 7: expected >=2 object .bin entries (VAL=42 and VAL=43), got ' +
      objBins.length);
  }
  if (manBins.length < 1) {
    fail('check 7: expected at least one manifest (.bin, CCMANIFS), got ' +
      manBins.length);
  }

  // Every OBJECT .bin must have a sibling .o payload sidecar; every MANIFEST
  // .bin must NOT (the manifest is an index, it carries no object payload).
  const sidecarOf = (binPath) => binPath.replace(/\.bin$/, '.o');
  for (const ob of objBins) {
    if (!fs.existsSync(sidecarOf(ob))) {
      fail('check 7: object .bin missing its .o payload sidecar: ' + ob);
    }
  }
  for (const mb of manBins) {
    if (fs.existsSync(sidecarOf(mb))) {
      fail('check 7: manifest .bin must not have a .o sidecar: ' + mb);
    }
  }
  // Each .o must belong to an object .bin (no orphan sidecars).
  const objBinSet = new Set(objBins);
  for (const o of oFiles) {
    if (!objBinSet.has(o.replace(/\.o$/, '.bin'))) {
      fail('check 7: orphan .o sidecar with no object .bin: ' + o);
    }
  }

  // Parse + validate one full OBJECT (not a manifest).
  const obj = parseAndValidateObject(objBins[0]);
  if (obj.inputCount < 1) {
    fail('check 7: expected at least one input record, got ' + obj.inputCount);
  }
  if (!obj.firstInputPath) {
    fail('check 7: could not decode any input path');
  }
  // The asm payload now lives in the .o sidecar, so the in-.bin asm span is
  // empty. Assert that explicitly (it characterizes the v2 .o-cache format).
  if (obj.asmLen !== 0) {
    fail('check 7: expected empty in-.bin asm span (payload is the .o ' +
      'sidecar), got asm_len=' + obj.asmLen);
  }
  process.stdout.write(
    'check 7 OK: ' + objBins.length + ' object(s) + ' + manBins.length +
    ' manifest(s), each object has a .o sidecar, compiler-id present;\n' +
    '           magic=GCCCACHE version=' + obj.ver + ' inputs=' +
    obj.inputCount + ' asm_len=' + obj.asmLen + '\n' +
    '           source=' + obj.source + '\n' +
    '           input[0]=' + obj.firstInputPath + '\n' +
    '           diag=[' + obj.diagOff + ',' + (obj.diagOff + obj.diagLen) +
    ')\n');
}

// ---- 8. warning parity: -Wmaybe-uninitialized on MISS and HIT -------------
{
  const wCache = path.join(work, 'wcache');
  const wSrc = path.join(work, 'w.c');
  // A conditional definition followed by an UNCONDITIONAL use; at -O2 -Wall
  // this reliably triggers the middle-end/back-end warning
  // -Wmaybe-uninitialized, so it exercises the back-end diagnostic
  // capture-on-MISS / replay-on-HIT path. (The terser
  // "int f(int c){int x; if(c) x=1; return x;}" does not fire on this
  // compiler -- the optimizer proves the garbage value never matters -- so it
  // would make this parity check vacuous; the unconditional use below forces
  // the warning to actually appear.)
  fs.writeFileSync(
    wSrc,
    'int g(int);\n' +
    'int f(int c){\n' +
    '  int x;\n' +
    '  if (c > 5)\n' +
    '    x = g(c);\n' +
    '  return x + c;\n' +
    '}\n'
  );
  const wObj1 = path.join(work, 'w1.o');
  const wObj2 = path.join(work, 'w2.o');

  // -Wall is needed to enable -Wmaybe-uninitialized; pass it as an extra arg.
  // -fintegrated-as is required to engage the cache (see compile() above).
  function compileW(obj) {
    const args = [
      '-O2', '-Wall', '-c', wSrc, '-o', obj,
      '-fcompile-cache=' + wCache, '-fintegrated-as', B,
    ];
    const res = spawnSync(XGCC, args, { env: debugEnv, encoding: 'utf8' });
    if (res.error) fail('check 8: failed to spawn xgcc: ' + res.error.message);
    return { status: res.status, stderr: res.stderr || '', keys: parseKeys(res.stderr || '') };
  }

  const miss = compileW(wObj1);
  if (!keyFor(miss.keys, 'miss')) {
    fail('check 8: expected a "miss" on first -Wall compile\n' + miss.stderr);
  }
  if (!/-Wmaybe-uninitialized/.test(miss.stderr)) {
    fail('check 8: expected -Wmaybe-uninitialized on MISS\n' + miss.stderr);
  }

  const hit = compileW(wObj2);
  if (!keyFor(hit.keys, 'hit')) {
    fail('check 8: expected a "hit" on second -Wall compile\n' + hit.stderr);
  }
  if (!/-Wmaybe-uninitialized/.test(hit.stderr)) {
    fail('check 8: expected -Wmaybe-uninitialized replayed on HIT\n' + hit.stderr);
  }
  if (miss.status !== hit.status) {
    fail('check 8: exit codes differ MISS=' + miss.status + ' HIT=' + hit.status);
  }
  process.stdout.write(
    'check 8 OK: -Wmaybe-uninitialized on MISS and HIT; exit codes match (' +
    miss.status + ')\n');
}

// ---- 9. cross-dir HIT without -g (content-addressed key) ------------------
// The headline property of the content-addressed key: the SAME source content,
// compiled from two DIFFERENT working directories, with a DIFFERENT -I<dir>
// spelling and a DIFFERENT -o, and WITHOUT -g, must hit on the second compile
// (the key folds in file CONTENTS, not paths). Before this fix the key folded
// in the main input path, the cwd, the per-include paths, and the -I/-isystem
// values, so this scenario always missed -- a shared cache got zero hits on
// real builds. Compile into the SAME cache dir from both trees and require
// miss+store then hit, with identical keys.
{
  const xdCache = path.join(work, 'xdcache');
  const dirA = path.join(work, 'xdA', 'inc');
  const dirB = path.join(work, 'xdB', 'inc');
  fs.mkdirSync(dirA, { recursive: true });
  fs.mkdirSync(dirB, { recursive: true });

  const SRC =
    '#include "h.h"\n' +
    'int helper(void);\n' +
    'static int s(void){return VAL;}\n' +
    'int helper(void){return s();}\n' +
    'int main(void){return helper();}\n';
  const HDR = '#define VAL 42\nint helper(void);\n';

  // Identical content under two differently named trees + differently named
  // source files. The header is reached via "-Iinc" in each tree, so the
  // resolved include path differs between the two compiles.
  const aSrc = path.join(work, 'xdA', 'a.c');
  const bSrc = path.join(work, 'xdB', 'b.c');
  fs.writeFileSync(aSrc, SRC);
  fs.writeFileSync(path.join(dirA, 'h.h'), HDR);
  fs.writeFileSync(bSrc, SRC);
  fs.writeFileSync(path.join(dirB, 'h.h'), HDR);

  const objA = path.join(work, 'xdA.o');
  const objB = path.join(work, 'xdB.o');

  // Compile #1 from xdA/ : cwd=xdA, -Iinc, -o xdA.o, NO -g.
  const r1 = spawnSync(
    XGCC,
    ['-O2', '-c', 'a.c', '-Iinc', '-o', objA,
      '-fcompile-cache=' + xdCache, '-fintegrated-as', B],
    { cwd: path.join(work, 'xdA'), env: debugEnv, encoding: 'utf8' });
  if (r1.error) fail('check 9: failed to spawn xgcc (dirA): ' + r1.error.message);
  if (r1.status !== 0) fail('check 9: dirA compile failed: ' + (r1.stderr || ''));
  const k1 = parseKeys(r1.stderr || '');
  const miss1 = keyFor(k1, 'miss');
  if (!miss1) fail('check 9: expected a "miss" on first cross-dir compile\n' + r1.stderr);
  if (!keyFor(k1, 'store')) fail('check 9: expected a "store" on first cross-dir compile\n' + r1.stderr);

  // Compile #2 from xdB/ : cwd=xdB, -Iinc, -o xdB.o, NO -g -- different cwd,
  // different source filename, different resolved -I path, different output.
  const r2 = spawnSync(
    XGCC,
    ['-O2', '-c', 'b.c', '-Iinc', '-o', objB,
      '-fcompile-cache=' + xdCache, '-fintegrated-as', B],
    { cwd: path.join(work, 'xdB'), env: debugEnv, encoding: 'utf8' });
  if (r2.error) fail('check 9: failed to spawn xgcc (dirB): ' + r2.error.message);
  if (r2.status !== 0) fail('check 9: dirB compile failed: ' + (r2.stderr || ''));
  const k2 = parseKeys(r2.stderr || '');
  const hit2 = keyFor(k2, 'hit');
  if (!hit2) {
    fail('check 9: expected a "hit" on the SAME content from a different ' +
      'build dir (the content-addressed key must ignore paths without -g)\n' +
      r2.stderr);
  }
  if (hit2 !== miss1) {
    fail('check 9: cross-dir hit key ' + hit2 + ' != original key ' + miss1 +
      ' (key still depends on a path component without -g)');
  }
  if (Buffer.compare(readObj(objA), readObj(objB)) !== 0) {
    fail('check 9: cross-dir cache hit produced a non-identical object');
  }
  process.stdout.write(
    'check 9 OK: cross-dir HIT without -g, key=' + miss1 +
    ' (same content, different cwd/-I/-o -> same key)\n');
}

// ---- 10. cross-dir MISS with -g, plus same-dir -g HIT control -------------
// The complement of check 9: under -g the source path IS baked into the DWARF
// (DW_AT_name / DW_AT_comp_dir / the line table), so it MUST be part of the
// key. The same content compiled from two different dirs with -g must produce
// two DIFFERENT keys (a second miss), and recompiling the FIRST tree again
// with -g must hit (same path -> same key). This proves the path is in the
// key exactly when output depends on it.
{
  const gCache = path.join(work, 'gxdcache');
  // Reuse the trees created in check 9.
  const aDir = path.join(work, 'xdA');
  const bDir = path.join(work, 'xdB');
  const objAg = path.join(work, 'xdAg.o');
  const objBg = path.join(work, 'xdBg.o');
  const objAg2 = path.join(work, 'xdAg2.o');

  function compileG(cwd, src, obj) {
    const res = spawnSync(
      XGCC,
      ['-O2', '-g', '-c', src, '-Iinc', '-o', obj,
        '-fcompile-cache=' + gCache, '-fintegrated-as', B],
      { cwd, env: debugEnv, encoding: 'utf8' });
    if (res.error) fail('check 10: failed to spawn xgcc: ' + res.error.message);
    if (res.status !== 0) fail('check 10: -g compile failed: ' + (res.stderr || ''));
    return parseKeys(res.stderr || '');
  }

  const kA = compileG(aDir, 'a.c', objAg);
  const missA = keyFor(kA, 'miss');
  if (!missA) fail('check 10: expected a "miss" on first -g compile (dirA)');

  const kB = compileG(bDir, 'b.c', objBg);
  const missB = keyFor(kB, 'miss');
  if (!missB) {
    fail('check 10: expected a "miss" compiling the same content from a ' +
      'different dir WITH -g (the path must be in the key under -g)');
  }
  if (missB === missA) {
    fail('check 10: -g keys from different build dirs are equal (' + missA +
      '); the source path is NOT in the key under -g but must be');
  }

  // Control: recompiling the FIRST tree with -g must hit (identical path).
  const kA2 = compileG(aDir, 'a.c', objAg2);
  const hitA2 = keyFor(kA2, 'hit');
  if (!hitA2) {
    fail('check 10: expected a "hit" recompiling the SAME -g tree (dirA)\n' +
      'a -g build must still hit when nothing changed');
  }
  if (hitA2 !== missA) {
    fail('check 10: -g re-hit key ' + hitA2 + ' != original -g key ' + missA);
  }
  process.stdout.write(
    'check 10 OK: cross-dir MISS with -g (' + missA + ' != ' + missB +
    '); same-dir -g re-compile HIT (' + hitA2 + ')\n');
}

// ---- cleanup + success ---------------------------------------------------
try {
  fs.rmSync(work, { recursive: true, force: true });
} catch {
  // best-effort cleanup; not a verification failure
}

process.stdout.write('CACHE VERIFY OK\n');
process.exit(0);
