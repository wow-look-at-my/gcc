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
// -fintegrated-as is the DEFAULT on this target (common.opt Init(1) with
// libgas linked in) and Stage 5 caches/serves only the in-process assembled
// object (-fno-integrated-as compiles are cache-ineligible by design:
// skip-no-integrated-as).  It is still passed explicitly here so the checks
// keep meaning "the integrated-as cache" even if the default ever changes.
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
// v3 cache layout under DIR:
//   <2hex>/<rest>.o    the cache OBJECT itself (the produced .o, hardlinked
//                      into its content-addressed slot). Per-object metadata
//                      (format version, flags, back-end warning/error counts,
//                      diagnostics blob) lives in the "user.gcc_cc.meta" xattr
//                      on this file -- there is no per-object .bin.
//   <2hex>/<rest>.bin  a MANIFEST (magic "CCMANIFS"), the direct-mode index;
//                      OR, only where the filesystem rejects user xattrs, a
//                      per-object meta-fallback (magic "GCCCMETA").
//   compiler-id        the driver's pre-parse compiler-identity sidecar.
const CC_MAGIC = 'GCCCACHE';          // legacy v2 object magic (must NOT appear)
// Object format version. Bumped to 3 when the per-object .bin metadata sidecar
// was removed: the cache object is now the produced .o, hardlinked into the
// content-addressed slot, with its (tiny) metadata stored in the
// "user.gcc_cc.meta" xattr. The old inputs table + header strings were
// write-only at serve time and were dropped.
const CC_FORMAT_VERSION = 3;
const CC_MANIFEST_MAGIC = 'CCMANIFS';
const CC_HEADER_SIZE = 128;

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

// ---- 7. on-disk layout: content-addressed .o + xattr meta + manifest + id --
{
  // cacheA holds (at least) the VAL=42 and VAL=43 entries. The v3 .o cache
  // layout under a cache dir is:
  //   <2hex>/<rest>.o    -- the cache OBJECT itself: the produced .o, hardlinked
  //                         (reflink/hardlink/copy) into the content-addressed
  //                         slot. Its metadata (format version, flags, back-end
  //                         warning/error counts, diag blob) lives in the
  //                         "user.gcc_cc.meta" xattr (+ "user.gcc_cc.v" probe).
  //                         There is NO per-object .bin anymore.
  //   <2hex>/<rest>.bin  -- a MANIFEST (magic "CCMANIFS"): the direct-mode
  //                         index; has NO .o object. (A per-object .bin appears
  //                         ONLY on the xattr-unsupported fallback path, holding
  //                         the CC_META record; see metaOf below.)
  //   compiler-id-<prog> -- the driver's pre-parse compiler-identity sidecar
  //                         for one compiler proper (cc1/cc1plus; the driver
  //                         reads the one matching the command it assembled).
  //   compiler-id        -- the legacy single-file sidecar, still written for
  //                         older drivers sharing the cache dir (and read as
  //                         the fallback for pre-split caches).
  const files = listFiles(cacheA);
  if (files.length === 0) {
    fail('check 7: no cache files found under ' + cacheA);
  }

  // Read a file's 8-byte magic (latin1), or '' if too short.
  const magicOf = (f) => {
    const b = fs.readFileSync(f);
    return b.length >= 8 ? b.toString('latin1', 0, 8) : '';
  };

  // Read an extended attribute as a Buffer (or null if absent/unsupported).
  // Node has no xattr API, so shell out to python3's os.getxattr, which is the
  // same syscall the compiler uses. Returns null when the attr is missing.
  function getxattr(file, name) {
    const py =
      'import os,sys\n' +
      'try:\n' +
      '  sys.stdout.buffer.write(os.getxattr(sys.argv[1], sys.argv[2]))\n' +
      'except OSError:\n' +
      '  sys.exit(3)\n';
    const r = spawnSync('python3', ['-c', py, file, name],
      { encoding: 'buffer' });
    if (r.status === 0) return r.stdout;
    return null;
  }

  // Decode the CC_META record carried by an object: the xattr, or (fallback)
  // the .bin sidecar body. Returns { ver, flags, warnings, errors, diagLen }
  // or fails. METAREC layout: magic[8] ver(u16) flags(u16) warn(u32) err(u32)
  // diag_len(u32), then diag_len bytes.
  const CC_META_MAGIC = 'GCCCMETA';
  const CC_META_REC_SIZE = 24;
  function decodeMeta(buf, src) {
    if (buf.length < CC_META_REC_SIZE) {
      fail('check 7: meta record too short (' + buf.length + ' B) from ' + src);
    }
    const mg = buf.toString('latin1', 0, 8);
    if (mg !== CC_META_MAGIC) {
      fail('check 7: meta magic ' + JSON.stringify(mg) + ' (want "' +
        CC_META_MAGIC + '") from ' + src);
    }
    const ver = buf.readUInt16LE(8);
    if (ver !== CC_FORMAT_VERSION) {
      fail('check 7: meta format_version ' + ver + ' != ' + CC_FORMAT_VERSION +
        ' from ' + src);
    }
    const diagLen = buf.readUInt32LE(20);
    if (CC_META_REC_SIZE + diagLen > buf.length) {
      fail('check 7: meta diag blob exceeds record from ' + src);
    }
    return {
      ver, flags: buf.readUInt16LE(10),
      warnings: buf.readUInt32LE(12), errors: buf.readUInt32LE(16), diagLen,
    };
  }
  // Read an object's metadata: prefer the xattr, fall back to the .bin sidecar.
  function metaOf(objPath) {
    const x = getxattr(objPath, 'user.gcc_cc.meta');
    if (x) return { meta: decodeMeta(x, objPath + ' [xattr]'), via: 'xattr' };
    const binPath = objPath.replace(/\.o$/, '.bin');
    if (fs.existsSync(binPath)) {
      return { meta: decodeMeta(fs.readFileSync(binPath), binPath),
        via: 'bin-fallback' };
    }
    fail('check 7: object .o has neither user.gcc_cc.meta xattr nor a .bin ' +
      'fallback: ' + objPath);
    return null; // unreached
  }

  const isIdFile = (f) => path.basename(f) === 'compiler-id' ||
    path.basename(f).startsWith('compiler-id-');
  const binFiles = files.filter((f) => f.endsWith('.bin'));
  const oFiles = files.filter((f) => f.endsWith('.o'));
  const idFiles = files.filter(isIdFile);
  const unexpected = files.filter(
    (f) => !f.endsWith('.bin') && !f.endsWith('.o') && !isIdFile(f));
  if (unexpected.length) {
    fail('check 7: found unexpected cache file(s) (not .bin/.o/compiler-id*):\n  ' +
      unexpected.join('\n  '));
  }

  // The driver's compiler-id sidecars must exist: the per-language one for
  // the (only) compiler that stored here (cc1 -- cacheA saw C compiles) plus
  // the legacy single name kept for older drivers.
  const idNames = idFiles.map((f) => path.basename(f)).sort();
  if (idNames.join(',') !== 'compiler-id,compiler-id-cc1') {
    fail('check 7: expected compiler-id + compiler-id-cc1 sidecars, got [' +
      idNames.join(', ') + ']');
  }

  // Classify .bin files: manifests (CCMANIFS) vs per-object meta fallbacks
  // (GCCCMETA). A GCCCACHE .bin is the OLD v2 object format and must NOT appear.
  const manBins = [];
  for (const f of binFiles) {
    const mg = magicOf(f);
    if (mg === CC_MANIFEST_MAGIC) manBins.push(f);
    else if (mg === CC_META_MAGIC) {
      // A meta fallback .bin must sit beside its object .o.
      if (!fs.existsSync(f.replace(/\.bin$/, '.o'))) {
        fail('check 7: meta-fallback .bin without its object .o: ' + f);
      }
    } else if (mg === CC_MAGIC) {
      fail('check 7: found a legacy v2 object .bin (GCCCACHE); v3 stores the ' +
        'object as the content-addressed .o with xattr metadata: ' + f);
    } else {
      fail('check 7: .bin with unknown magic ' + JSON.stringify(mg) + ': ' + f);
    }
  }
  // The cache objects are the .o files (excluding any that is purely a manifest
  // sibling -- manifests have no .o, so every .o here is a real object).
  const objFiles = oFiles;
  if (objFiles.length < 2) {
    fail('check 7: expected >=2 cache object .o entries (VAL=42 and VAL=43), ' +
      'got ' + objFiles.length);
  }
  if (manBins.length < 1) {
    fail('check 7: expected at least one manifest (.bin, CCMANIFS), got ' +
      manBins.length);
  }

  // Every cache object .o must carry valid v3 metadata (xattr or .bin fallback).
  let viaXattr = 0, viaBin = 0;
  let sample = null;
  for (const o of objFiles) {
    const { meta, via } = metaOf(o);
    if (via === 'xattr') viaXattr++; else viaBin++;
    if (!sample) sample = { o, meta, via };
  }

  process.stdout.write(
    'check 7 OK: ' + objFiles.length + ' cache object(s) (.o) + ' +
    manBins.length + ' manifest(s), compiler-id + compiler-id-cc1 present; ' +
    'no legacy v2 .bin;\n' +
    '           metadata via xattr=' + viaXattr + ' bin-fallback=' + viaBin +
    '\n' +
    '           sample ' + path.basename(sample.o) + ' [' + sample.via +
    ']: version=' + sample.meta.ver + ' flags=' + sample.meta.flags +
    ' warnings=' + sample.meta.warnings + ' errors=' + sample.meta.errors +
    ' diag_len=' + sample.meta.diagLen + '\n');
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

// ---- checks 11-13: transparent auto-PCH (-fauto-pch / GCC_AUTO_PCH) -------
//
// The driver detects a TU whose leading lines are only comments/blanks and
// #include <...> directives, builds a PCH for that prelude once under
// CACHE/pch/<key>/, and injects "-include <stub>" into later compiles that
// share the (normalized) prelude + flag cell. These checks assert the three
// load-bearing behaviors: exactly-once generation + reuse with byte-identical
// objects (11), include-closure invalidation on a header edit -- no stale PCH
// (12), and the any-stderr-means-negative-entry rule that keeps diagnostics
// byte-identical (13).

// Parse "auto-pch: <what> ..." decision lines from stderr.
function parsePch(stderr) {
  const re = /auto-pch: ([a-z-]+)/g;
  const out = [];
  let m;
  while ((m = re.exec(stderr)) !== null) out.push(m[1]);
  return out;
}

// compile() with auto-PCH enabled (env) + decision logging.
function compilePch(driver, src, obj, cacheDir, extra) {
  const args = [
    '-O2', '-c', src, '-o', obj,
    '-fcompile-cache=' + cacheDir, '-fintegrated-as', B,
  ].concat(extra || []);
  const res = spawnSync(driver, args, {
    env: { ...debugEnv, GCC_AUTO_PCH: '1', GCC_AUTO_PCH_DEBUG: '1' },
    encoding: 'utf8',
  });
  if (res.error) fail('failed to spawn ' + driver + ': ' + res.error.message);
  return { status: res.status, stderr: res.stderr || '', pch: parsePch(res.stderr || '') };
}

{
  const dir = path.join(work, 'apch');
  const inc = path.join(dir, 'inc');
  fs.mkdirSync(inc, { recursive: true });
  const cache = path.join(dir, 'cache');
  fs.mkdirSync(cache);
  writeHeader(path.join(inc, 'ap1.h'),
    '#ifndef AP1_H\n#define AP1_H\ninline int ap1() { return 10; }\n#endif\n');
  writeHeader(path.join(inc, 'ap2.h'),
    '#ifndef AP2_H\n#define AP2_H\ninline int ap2() { return 20; }\n#endif\n');
  writeHeader(path.join(inc, 'ap3.h'),
    '#ifndef AP3_H\n#define AP3_H\ninline int ap3() { return 30; }\n#endif\n');
  const prelude = '#include <ap1.h>\n#include <ap2.h>\n#include <ap3.h>\n';
  const tu1 = path.join(dir, 'tu1.cpp');
  const tu2 = path.join(dir, 'tu2.cpp');
  fs.writeFileSync(tu1, '// banner one\n' + prelude +
    'int use1() { return ap1() + ap2() + ap3(); }\n');
  fs.writeFileSync(tu2, '/* different banner, same line count */\n' + prelude +
    'int use2() { return ap1() * ap2() - ap3(); }\n');
  const I = '-I' + inc;

  // check 11: seed compiler-id, then gen+inject, then share; byte-identical.
  // The seed must be a DIFFERENT source: probe/inject runs before the .o
  // serve, and a warm .o hit (same source, same flags) correctly skips PCH
  // generation entirely -- so re-compiling the seed TU would only ever
  // manifest-hit, never gen.
  const tu0 = path.join(dir, 'tu0.cpp');
  fs.writeFileSync(tu0, '// seed banner\n' + prelude +
    'int use0() { return ap1(); }\n');
  compile(XGPP, tu0, path.join(dir, 'seed.o'), cache, [I]); // writes compiler-id
  let r = compilePch(XGPP, tu1, path.join(dir, 't1a.o'), cache, [I]);
  if (r.status !== 0) fail('check 11: compile failed\n' + r.stderr);
  if (!r.pch.includes('gen-ok') || !r.pch.includes('inject'))
    fail('check 11: expected gen-ok + inject, got: ' + r.pch.join(',') + '\n' + r.stderr);
  r = compilePch(XGPP, tu2, path.join(dir, 't2a.o'), cache, [I]);
  if (!r.pch.includes('inject') || r.pch.includes('gen-ok'))
    fail('check 11: expected shared-prelude inject without regen, got: '
         + r.pch.join(',') + '\n' + r.stderr);
  // feature-off reference: a plain driver run (no cache, no auto-pch).
  const off = spawnSync(XGPP, ['-O2', '-c', tu2, '-o', path.join(dir, 't2plain.o'),
    '-fintegrated-as', B, I], { encoding: 'utf8' });
  if (off.status !== 0) fail('check 11: plain compile failed\n' + off.stderr);
  if (Buffer.compare(readObj(path.join(dir, 't2a.o')),
		     readObj(path.join(dir, 't2plain.o'))) !== 0)
    fail('check 11: injected object differs from plain compile');
  const gchs = fs.globSync(path.join(cache, 'pch', '*', '*', 'stub.h.gch'));
  if (gchs.length !== 1)
    fail('check 11: expected exactly 1 stub.h.gch, found ' + gchs.length);
  process.stdout.write('check 11 OK: auto-PCH gen once + shared inject, byte-identical object\n');

  // check 12: edit a prelude header -> manifest invalidates -> regen, fresh .o.
  writeHeader(path.join(inc, 'ap2.h'),
    '#ifndef AP2_H\n#define AP2_H\ninline int ap2() { return 21; }\n#endif\n');
  r = compilePch(XGPP, tu1, path.join(dir, 't1b.o'), cache, [I]);
  if (!r.pch.includes('gen-ok'))
    fail('check 12: expected regeneration after header edit, got: '
         + r.pch.join(',') + '\n' + r.stderr);
  const off2 = spawnSync(XGPP, ['-O2', '-c', tu1, '-o', path.join(dir, 't1plain.o'),
    '-fintegrated-as', B, I], { encoding: 'utf8' });
  if (off2.status !== 0) fail('check 12: plain compile failed\n' + off2.stderr);
  if (Buffer.compare(readObj(path.join(dir, 't1b.o')),
		     readObj(path.join(dir, 't1plain.o'))) !== 0)
    fail('check 12: STALE PCH: post-edit object differs from plain compile');
  process.stdout.write('check 12 OK: prelude-header edit regenerates the PCH, no stale object\n');

  // check 13: a prelude that WARNS is negative-cached; diagnostics identical.
  writeHeader(path.join(inc, 'apw.h'),
    '#ifndef APW_H\n#define APW_H\n#warning "apw"\ninline int apw() { return 1; }\n#endif\n');
  const tuw = path.join(dir, 'tuw.cpp');
  fs.writeFileSync(tuw, '#include <ap1.h>\n#include <ap2.h>\n#include <apw.h>\n'
    + 'int usew() { return apw(); }\n');
  r = compilePch(XGPP, tuw, path.join(dir, 'tw1.o'), cache, [I]);
  if (!r.pch.includes('gen-fail') || !r.pch.includes('negative-store'))
    fail('check 13: expected gen-fail + negative-store for a warning prelude, got: '
         + r.pch.join(',') + '\n' + r.stderr);
  // Second compile: negative entry honored; stderr must equal a plain compile.
  const on2 = spawnSync(XGPP, ['-O2', '-c', tuw, '-o', path.join(dir, 'tw2.o'),
    '-fcompile-cache=' + cache, '-fintegrated-as', B, I],
    { env: { ...process.env, GCC_AUTO_PCH: '1' }, encoding: 'utf8' });
  const offw = spawnSync(XGPP, ['-O2', '-c', tuw, '-o', path.join(dir, 'twoff.o'),
    '-fintegrated-as', B, I], { encoding: 'utf8' });
  if (on2.status !== 0 || offw.status !== 0)
    fail('check 13: compiles failed\n' + on2.stderr + offw.stderr);
  if (on2.stderr !== offw.stderr)
    fail('check 13: stderr differs between auto-pch-on (negative) and plain:\n--- on ---\n'
         + on2.stderr + '--- off ---\n' + offw.stderr);
  process.stdout.write('check 13 OK: warning prelude -> negative entry, stderr byte-identical\n');
}

// ---- check 14: PCH object bytes -- determinism + manual-PCH parity --------
//
// Checks 11/12 byte-compare a PCH-consuming object against a NO-pch compile.
// That identity holds for preludes like theirs but is NOT guaranteed in
// general: loading a PCH restores shared trees for header-defined entities,
// so varasm emits one merged constant pool for anonymous string/numeric
// literals, where a fresh parse materializes per-use duplicates in
// per-function .rodata.*.str1.* SHF_MERGE sections with .LC labels (observed
// on {fmt}; global symbols, section inventory of named data, and the
// normalized instruction stream are identical -- the linker merges the
// literals anyway). This is stock-PCH-inherent: a hand-built -x c++-header
// PCH diverges from the plain compile in exactly the same way. See the
// -fauto-pch section of gcc/doc/invoke.texi.
//
// The invariants that DO hold universally, asserted here on a
// string-literal-bearing prelude:
//   (a) determinism: two independent caches produce byte-identical
//       PCH-consuming objects for the same TU + options;
//   (b) manual parity: the auto-PCH object equals the object compiled
//       against a hand-built PCH (driver -x c++-header, consumed via
//       -include) -- which also gives the driver's c++-header spec path
//       (the -o/--output-pch collision fix) its own CI coverage.
// Deliberately NO assertion against the no-PCH object, in either direction.
{
  const dir = path.join(work, 'apch14');
  const inc = path.join(dir, 'inc');
  fs.mkdirSync(inc, { recursive: true });
  writeHeader(path.join(inc, 'as1.h'),
    '#ifndef AS1_H\n#define AS1_H\ninline const char *as1() '
    + '{ return "as1: a mergeable literal shared by prelude and body"; }\n#endif\n');
  writeHeader(path.join(inc, 'as2.h'),
    '#ifndef AS2_H\n#define AS2_H\ninline const char *as2() '
    + '{ return "as2: another mergeable string literal"; }\n#endif\n');
  writeHeader(path.join(inc, 'as3.h'),
    '#ifndef AS3_H\n#define AS3_H\n#include <as1.h>\ninline const char *as3() '
    + '{ return as1() + 5; }\n#endif\n');
  const prelude = '#include <as1.h>\n#include <as2.h>\n#include <as3.h>\n';
  const I = '-I' + inc;
  const seed = path.join(dir, 'seed.cpp');
  fs.writeFileSync(seed, '// seed banner\n' + prelude + 'int s14() { return 14; }\n');
  const tu = path.join(dir, 'tu.cpp');
  fs.writeFileSync(tu, '// tu banner\n' + prelude
    + 'static const char *own = "tu-own literal";\n'
    + 'unsigned long u14() { return (unsigned long)(as1()[0] + as2()[1] + as3()[2] + own[3]); }\n');

  // (a) determinism across independent caches.
  const pchObjs = [];
  for (const tag of ['a', 'b']) {
    const cache = path.join(dir, 'cache_' + tag);
    fs.mkdirSync(cache);
    // Seed the compiler-id sidecar with a DIFFERENT TU (same reason as check 11).
    compile(XGPP, seed, path.join(dir, 'seed_' + tag + '.o'), cache, [I]);
    const obj = path.join(dir, 'tu_' + tag + '.o');
    const r = compilePch(XGPP, tu, obj, cache, [I]);
    if (r.status !== 0) fail('check 14: compile (' + tag + ') failed\n' + r.stderr);
    if (!r.pch.includes('gen-ok') || !r.pch.includes('inject'))
      fail('check 14: expected gen-ok + inject in cache ' + tag + ', got: '
           + r.pch.join(',') + '\n' + r.stderr);
    pchObjs.push(obj);
  }
  if (Buffer.compare(readObj(pchObjs[0]), readObj(pchObjs[1])) !== 0)
    fail('check 14: PCH-consuming objects differ between independent caches');

  // (b) manual parity: driver-built stock PCH, consumed via -include.
  const prel = path.join(dir, 'prel.h');
  fs.writeFileSync(prel, prelude);
  const gch = spawnSync(XGPP, ['-O2', '-x', 'c++-header', prel,
    '-o', prel + '.gch', '-fintegrated-as', B, I], { encoding: 'utf8' });
  if (gch.status !== 0)
    fail('check 14: driver -x c++-header PCH build failed (spec regression?)\n'
         + (gch.stderr || ''));
  // Prove the .gch is actually consumed before trusting the byte-compare.
  const probe = spawnSync(XGPP, ['-O2', '-include', prel, '-H', '-fsyntax-only',
    tu, B, I], { encoding: 'utf8' });
  if (!(probe.stderr || '').split('\n').some((l) => l.startsWith('! ')))
    fail('check 14: manual PCH not consumed (-H shows no "!" line):\n'
         + (probe.stderr || ''));
  const mobj = path.join(dir, 'manual.o');
  const man = spawnSync(XGPP, ['-O2', '-c', tu, '-o', mobj, '-include', prel,
    '-fintegrated-as', B, I], { encoding: 'utf8' });
  if (man.status !== 0)
    fail('check 14: manual-PCH compile failed\n' + (man.stderr || ''));
  if (man.stderr) fail('check 14: manual-PCH compile warned\n' + man.stderr);
  if (Buffer.compare(readObj(pchObjs[0]), readObj(mobj)) !== 0)
    fail('check 14: auto-PCH object differs from manual stock-PCH object');
  process.stdout.write(
    'check 14 OK: PCH objects deterministic across caches + identical to manual -x c++-header PCH\n');
}

// ---- checks 15-18: __has_include probes in the manifest fast path ---------
// A TU that evaluates __has_include used to be disqualified from the manifest
// ("manifest-skip-has-include") -- which was every real C++ TU, because
// libstdc++'s bits/c++config.h probes <pstl/pstl_config.h> and <tbb/tbb.h>.
// Probes are now RECORDED in the manifest entry (operand + result + the
// candidate paths the search proved absent) and re-verified at serve time,
// and their results fold into the object key. These checks pin:
//   15. a real std-header C++ TU stores a manifest and gets a pre-parse
//       manifest-hit on the warm compile;
//   16. a NEGATIVE probe (header absent) does not serve stale after the
//       probed header APPEARS -- full recompile, new key, new behavior;
//   17. a POSITIVE probe does not serve stale after the header is DELETED
//       (and the multi-entry manifest serves the matching original state);
//   18. the carve-outs still disqualify: __has_include_next
//       ("manifest-skip-has-include-next") and relative quote-form probes
//       ("manifest-skip-has-include"), while the deep post-parse cache still
//       hits for them.

// ---- 15. C++ std-header TU: manifest stored + pre-parse manifest-hit ------
{
  // The in-tree xg++ has no default C++ include path; point it at the
  // build-tree libstdc++ headers (and the in-srcdir libsupc++), like a
  // build-tree caller would.
  function findLibstdcxxIncludes(root) {
    let incs = [];
    try {
      incs = fs.globSync(path.join(root, '*', 'libstdc++-v3', 'include'));
    } catch {
      incs = [];
    }
    if (incs.length === 0) {
      // Node without fs.globSync: scan one level of build subdirs.
      let ents = [];
      try {
        ents = fs.readdirSync(root, { withFileTypes: true });
      } catch {
        ents = [];
      }
      for (const e of ents) {
        if (!e.isDirectory()) continue;
        const p = path.join(root, e.name, 'libstdc++-v3', 'include');
        if (fs.existsSync(p)) incs.push(p);
      }
    }
    for (const inc of incs) {
      let ents = [];
      try {
        ents = fs.readdirSync(inc, { withFileTypes: true });
      } catch {
        continue;
      }
      for (const e of ents) {
        if (e.isDirectory() &&
            fs.existsSync(path.join(inc, e.name, 'bits', 'c++config.h'))) {
          return { inc, tgtInc: path.join(inc, e.name) };
        }
      }
    }
    return null;
  }
  function findSrcdir(root) {
    let mk;
    try {
      mk = fs.readFileSync(path.join(root, 'Makefile'), 'utf8');
    } catch {
      return null;
    }
    const m = mk.match(/^srcdir\s*=\s*(.+)\s*$/m);
    return m ? path.resolve(root, m[1].trim()) : null;
  }
  const hdrs = findLibstdcxxIncludes(buildDir);
  if (!hdrs) fail('check 15: could not locate build-tree libstdc++ headers under ' + buildDir);
  const srcdir = findSrcdir(buildDir);
  const supDir = srcdir ? path.join(srcdir, 'libstdc++-v3', 'libsupc++') : null;
  const XI = ['-nostdinc++', '-I' + hdrs.tgtInc, '-I' + hdrs.inc]
    .concat(supDir && fs.existsSync(supDir) ? ['-I' + supDir] : []);

  const dir = path.join(work, 'probe-std');
  fs.mkdirSync(dir);
  const src = path.join(dir, 'stdtu.cpp');
  fs.writeFileSync(
    src,
    '#include <string>\n' +
    'int main(){ std::string s("hello"); return (int) s.size(); }\n'
  );
  const cache = path.join(dir, 'cache');
  const o1 = path.join(dir, 's1.o');
  const o2 = path.join(dir, 's2.o');

  const r1 = compile(XGPP, src, o1, cache, XI);
  if (/compile-cache: manifest-skip-has-include/.test(r1.stderr)) {
    fail('check 15: std-header TU still disqualified from the manifest ' +
         '(manifest-skip-has-include)\n' + r1.stderr);
  }
  if (!/compile-cache: manifest-store /.test(r1.stderr)) {
    fail('check 15: expected a manifest-store on the cold std-header compile\n' + r1.stderr);
  }
  const r2 = compile(XGPP, src, o2, cache, XI);
  if (!/compile-cache: manifest-hit /.test(r2.stderr)) {
    fail('check 15: expected a pre-parse manifest-hit on the warm std-header compile\n' + r2.stderr);
  }
  if (Buffer.compare(readObj(o1), readObj(o2)) !== 0) {
    fail('check 15: manifest-served std-header object is not byte-identical');
  }
  process.stdout.write(
    'check 15 OK: std-header C++ TU stores a manifest and manifest-hits warm, byte-identical\n');
}

// ---- 16+17. probe soundness: header appears / disappears ------------------
{
  const dir = path.join(work, 'probe-flip');
  const idir = path.join(dir, 'inc');
  fs.mkdirSync(idir, { recursive: true });
  const src = path.join(dir, 'p.c');
  // Deliberately NO #include of the probed header: its appearance then flips
  // the probe result WITHOUT changing the include closure, which is exactly
  // the case a closure-only key cannot see -- without the probe-result key
  // component, r3 below would be a stale deep "hit".
  fs.writeFileSync(
    src,
    '#if __has_include(<maybe_probe.h>)\n' +
    '#define PROBE_VAL 42\n' +
    '#else\n' +
    '#define PROBE_VAL 1\n' +
    '#endif\n' +
    'int main(void){ return PROBE_VAL; }\n'
  );
  const cache = path.join(dir, 'cache');
  const XI = ['-I' + idir];
  const o = (n) => path.join(dir, 'p' + n + '.o');

  // Cold: negative probe recorded, manifest stored.
  const r1 = compile(XGCC, src, o(1), cache, XI);
  if (!/compile-cache: manifest-store /.test(r1.stderr)) {
    fail('check 16: expected manifest-store on the cold negative-probe compile\n' + r1.stderr);
  }
  const key1 = keyFor(r1.keys, 'miss');
  // Warm: pre-parse manifest serve.
  const r2 = compile(XGCC, src, o(2), cache, XI);
  if (!/compile-cache: manifest-hit /.test(r2.stderr)) {
    fail('check 16: expected manifest-hit on the warm negative-probe compile\n' + r2.stderr);
  }
  // The probed header APPEARS: serving the old object would be stale.
  writeHeader(path.join(idir, 'maybe_probe.h'), '#define PROBE_VAL 42\n');
  const r3 = compile(XGCC, src, o(3), cache, XI);
  if (/compile-cache: manifest-hit /.test(r3.stderr)) {
    fail('check 16: STALE manifest-hit after the probed header appeared\n' + r3.stderr);
  }
  const key3 = keyFor(r3.keys, 'miss');
  if (!key3) fail('check 16: expected a full recompile (miss) after the probed header appeared\n' + r3.stderr);
  if (key3 === key1) {
    fail('check 16: object key unchanged although the probe result flipped ' +
         '(probe results must fold into the key)');
  }
  const exe = path.join(dir, 'p.out');
  let code = linkAndRun(XGCC, o(3), exe);
  if (code !== 42) fail('check 16: post-appearance program returned ' + code + ', expected 42');
  process.stdout.write(
    'check 16 OK: negative probe invalidates when the header appears (no stale serve; new key; runs 42)\n');

  // Warm again with the header present (positive-probe entry now cached).
  const r4 = compile(XGCC, src, o(4), cache, XI);
  if (!/compile-cache: manifest-hit /.test(r4.stderr)) {
    fail('check 17: expected manifest-hit on the warm positive-probe compile\n' + r4.stderr);
  }
  // The probed header DISAPPEARS: the positive entry must not serve; the
  // ORIGINAL negative entry is valid again and legitimately serves the
  // original object (multi-entry manifest round-trip).
  fs.rmSync(path.join(idir, 'maybe_probe.h'));
  const r5 = compile(XGCC, src, o(5), cache, XI);
  const hit5 = keyFor(r5.keys, 'hit');
  if (!hit5 || hit5 !== key1) {
    fail('check 17: after deleting the probed header, expected the ORIGINAL ' +
         'entry (' + key1 + ') to serve; got ' + hit5 + '\n' + r5.stderr);
  }
  if (Buffer.compare(readObj(o(5)), readObj(o(1))) !== 0) {
    fail('check 17: round-trip object differs from the original');
  }
  code = linkAndRun(XGCC, o(5), exe);
  if (code !== 1) fail('check 17: post-deletion program returned ' + code + ', expected 1');
  process.stdout.write(
    'check 17 OK: positive probe invalidates when the header disappears; original entry serves again\n');
}

// ---- 18. carve-outs: __has_include_next / quote form still disqualify -----
{
  const dir = path.join(work, 'probe-skip');
  const dA = path.join(dir, 'a');
  const dB = path.join(dir, 'b');
  fs.mkdirSync(dA, { recursive: true });
  fs.mkdirSync(dB, { recursive: true });
  fs.writeFileSync(
    path.join(dA, 'x_next.h'),
    '#if __has_include_next(<x_next.h>)\n#define HAVE_NEXT 1\n#else\n#define HAVE_NEXT 0\n#endif\n'
  );
  fs.writeFileSync(path.join(dB, 'x_next.h'), '#define UNUSED_B 1\n');
  const srcNext = path.join(dir, 'n.c');
  fs.writeFileSync(srcNext, '#include <x_next.h>\nint main(void){ return HAVE_NEXT; }\n');
  const cache = path.join(dir, 'cache');

  const rn1 = compile(XGCC, srcNext, path.join(dir, 'n1.o'), cache,
                      ['-I' + dA, '-I' + dB]);
  if (!/compile-cache: manifest-skip-has-include-next /.test(rn1.stderr)) {
    fail('check 18: expected manifest-skip-has-include-next for a __has_include_next TU\n' + rn1.stderr);
  }
  if (/compile-cache: manifest-store /.test(rn1.stderr)) {
    fail('check 18: __has_include_next TU must not store a manifest\n' + rn1.stderr);
  }
  const rn2 = compile(XGCC, srcNext, path.join(dir, 'n2.o'), cache,
                      ['-I' + dA, '-I' + dB]);
  if (/compile-cache: manifest-hit /.test(rn2.stderr)) {
    fail('check 18: __has_include_next TU must not manifest-hit\n' + rn2.stderr);
  }
  if (!keyFor(rn2.keys, 'hit')) {
    fail('check 18: __has_include_next TU should still deep-hit post-parse\n' + rn2.stderr);
  }

  // Relative quote-form probe: VERIFIABLE from records (the candidates are
  // pre-joined at probe time against the exact chain cpp walked, probing
  // file's directory included), so it stores a manifest and serves.  glibc's
  // own bits/statx.h / bits/unistd_ext.h probe "linux/stat.h" /
  // "linux/close_range.h", so this decides whether every POSIX-touching TU
  // manifest-serves or none of them do.  Full appear/disappear lifecycle in
  // check 19.
  fs.writeFileSync(path.join(dir, 'qprobe.h'), '#define HAVE_Q 1\n');
  const srcQ = path.join(dir, 'q.c');
  fs.writeFileSync(
    srcQ,
    '#if __has_include("qprobe.h")\n#define QV 3\n#else\n#define QV 0\n#endif\n' +
    'int main(void){ return QV; }\n'
  );
  const rq = compile(XGCC, srcQ, path.join(dir, 'q1.o'), cache);
  if (!/compile-cache: manifest-store /.test(rq.stderr)) {
    fail('check 18: quote-form probe TU must store a manifest now\n' + rq.stderr);
  }
  const rq2 = compile(XGCC, srcQ, path.join(dir, 'q2.o'), cache);
  if (!/compile-cache: manifest-hit /.test(rq2.stderr)) {
    fail('check 18: quote-form probe TU must manifest-hit warm\n' + rq2.stderr);
  }
  process.stdout.write(
    'check 18 OK: __has_include_next still skips the manifest; quote-form probes store and serve\n');
}

// Synchronous sleep (seconds, fractional ok) for the timestamp-sensitive
// checks below: the store only trusts a file's stat identity when its
// stamps predate the compile's start second (the "too new" guard), so a
// just-written header must rest for over a second before the storing
// compile for the stat shortcut to engage at all.
function sleepSecs(s) {
  spawnSync('sleep', [String(s)]);
}

// ---- 19. quote-form probe lifecycle: flip by appearance, heal by removal --
// A NOT-FOUND quote probe ("qf_probe.h" absent everywhere) must flip the TU
// when a file appears at the probe's first candidate -- the SOURCE'S OWN
// DIRECTORY, the part of the quote chain no search-path option describes --
// and the original entry must serve again once the file is removed
// (multi-entry manifest).  Functional proof via exit codes, like checks
// 16/17 do for the angle forms.
{
  const dir = path.join(work, 'quote-flip');
  fs.mkdirSync(dir, { recursive: true });
  const src = path.join(dir, 'qf.c');
  fs.writeFileSync(
    src,
    '#if __has_include("qf_probe.h")\n#define QV 3\n#else\n#define QV 0\n#endif\n' +
    'int main(void){ return QV; }\n'
  );
  const cache = path.join(dir, 'cache');
  const exe = path.join(dir, 'qf');
  const o = (n) => path.join(dir, 'qf' + n + '.o');

  const r1 = compile(XGCC, src, o(1), cache);
  if (!/compile-cache: manifest-store /.test(r1.stderr)) {
    fail('check 19: expected manifest-store for the not-found quote probe TU\n' + r1.stderr);
  }
  let code = linkAndRun(XGCC, o(1), exe);
  if (code !== 0) fail('check 19: initial program returned ' + code + ', expected 0');

  const r2 = compile(XGCC, src, o(2), cache);
  if (!/compile-cache: manifest-hit /.test(r2.stderr)) {
    fail('check 19: expected manifest-hit while the probe file is absent\n' + r2.stderr);
  }

  // The probed file APPEARS next to the source: the recorded candidate
  // exists now, so the entry must be rejected (no stale 0-return serve) and
  // the recompile must see QV=3.
  writeHeader(path.join(dir, 'qf_probe.h'), '/* appeared */\n');
  const r3 = compile(XGCC, src, o(3), cache);
  if (/compile-cache: manifest-hit /.test(r3.stderr)) {
    fail('check 19: stale manifest-hit after the quoted probe file appeared\n' + r3.stderr);
  }
  code = linkAndRun(XGCC, o(3), exe);
  if (code !== 3) fail('check 19: post-appearance program returned ' + code + ', expected 3');

  // Removal restores the original state: the first entry must serve again.
  fs.rmSync(path.join(dir, 'qf_probe.h'));
  const r4 = compile(XGCC, src, o(4), cache);
  if (!/compile-cache: manifest-hit /.test(r4.stderr)) {
    fail('check 19: expected manifest-hit again after the probe file was removed\n' + r4.stderr);
  }
  code = linkAndRun(XGCC, o(4), exe);
  if (code !== 0) fail('check 19: post-removal program returned ' + code + ', expected 0');

  process.stdout.write(
    'check 19 OK: quote-form probe flips on appearance in the source dir and heals on removal\n');
}

// ---- 20. stat identity: same-size rewrite with ns-exact restored mtime ----
// The manifest stat shortcut must NOT accept a header whose content changed
// even when the rewrite preserves the size AND restores the mtime to the
// exact nanosecond (touch -r): st_ctime necessarily advances, the identity
// mismatch forces the content re-hash, and the changed hash rejects the
// entry.  This is precisely the rewrite the old size+mtime-seconds shortcut
// could not see.
{
  const dir = path.join(work, 'statid');
  fs.mkdirSync(dir, { recursive: true });
  const hdr = path.join(dir, 'sh.h');
  const src = path.join(dir, 's.c');
  const stamp = path.join(dir, 'stamp');
  fs.writeFileSync(hdr, '#define SVAL 42\n');
  fs.writeFileSync(
    src, '#include "sh.h"\nint main(void){ return SVAL; }\n');
  const cache = path.join(dir, 'cache');
  const exe = path.join(dir, 's');
  const o = (n) => path.join(dir, 's' + n + '.o');

  // Let the header come to rest so the store trusts its stat identity (the
  // "too new" guard compares its stamps against the compile's start second).
  sleepSecs(1.3);

  const r1 = compile(XGCC, src, o(1), cache, ['-I' + dir]);
  if (!/compile-cache: manifest-store /.test(r1.stderr)) {
    fail('check 20: expected manifest-store on the cold compile\n' + r1.stderr);
  }
  let code = linkAndRun(XGCC, o(1), exe);
  if (code !== 42) fail('check 20: initial program returned ' + code + ', expected 42');

  // Sanity: the shortcut itself hits while nothing changed.
  const r2 = compile(XGCC, src, o(2), cache, ['-I' + dir]);
  if (!/compile-cache: manifest-hit /.test(r2.stderr)) {
    fail('check 20: expected manifest-hit before the rewrite\n' + r2.stderr);
  }

  // Capture sh.h's full-resolution timestamps on a stamp file, rewrite the
  // header SAME-SIZE with different content, then restore the timestamps
  // ns-exactly from the stamp.  Size, mtime (s+ns), dev, ino all match the
  // record afterwards; only ctime differs.
  let tr = spawnSync('touch', ['-r', hdr, stamp], { encoding: 'utf8' });
  if (tr.status !== 0) fail('check 20: touch -r (capture) failed: ' + (tr.stderr || ''));
  fs.writeFileSync(hdr, '#define SVAL 43\n');   // same byte count as 42
  tr = spawnSync('touch', ['-r', stamp, hdr], { encoding: 'utf8' });
  if (tr.status !== 0) fail('check 20: touch -r (restore) failed: ' + (tr.stderr || ''));
  const stNew = fs.statSync(hdr, { bigint: true });

  const r3 = compile(XGCC, src, o(3), cache, ['-I' + dir]);
  if (/compile-cache: manifest-hit /.test(r3.stderr)) {
    fail('check 20: STALE manifest-hit after a same-size mtime-restored rewrite\n' +
         'header stat: mtimeNs=' + String(stNew.mtimeNs) +
         ' ctimeNs=' + String(stNew.ctimeNs) + '\n' + r3.stderr);
  }
  code = linkAndRun(XGCC, o(3), exe);
  if (code !== 43) fail('check 20: post-rewrite program returned ' + code + ', expected 43 (stale 42 served?)');

  process.stdout.write(
    'check 20 OK: same-size content rewrite with ns-exact restored mtime is caught (ctime mismatch -> re-hash -> stale)\n');
}

// ---- 21. GCC_COMPILE_CACHE_PARANOID=1 forces the re-hash path -------------
// The paranoid mode must still serve (correct hits survive full content
// verification) -- it just never takes the stat shortcut.  Serve parity is
// asserted via the debug tag, byte-identical objects, and program behavior.
{
  const dir = path.join(work, 'paranoid');
  fs.mkdirSync(dir, { recursive: true });
  const hdr = path.join(dir, 'ph.h');
  const src = path.join(dir, 'p.c');
  fs.writeFileSync(hdr, '#define PVAL 7\n');
  fs.writeFileSync(src, '#include "ph.h"\nint main(void){ return PVAL; }\n');
  const cache = path.join(dir, 'cache');
  const exe = path.join(dir, 'p');
  const o = (n) => path.join(dir, 'p' + n + '.o');

  const r1 = compile(XGCC, src, o(1), cache, ['-I' + dir]);
  if (!/compile-cache: manifest-store /.test(r1.stderr)) {
    fail('check 21: expected manifest-store on the cold compile\n' + r1.stderr);
  }

  const paranoidEnv = { ...debugEnv, GCC_COMPILE_CACHE_PARANOID: '1' };
  const res = spawnSync(
    XGCC,
    ['-O2', '-c', src, '-o', o(2), '-I' + dir,
      '-fcompile-cache=' + cache, '-fintegrated-as', B],
    { env: paranoidEnv, encoding: 'utf8' });
  if (res.status !== 0) fail('check 21: paranoid compile failed\n' + (res.stderr || ''));
  if (!/compile-cache: manifest-hit /.test(res.stderr || '')) {
    fail('check 21: expected manifest-hit under GCC_COMPILE_CACHE_PARANOID=1\n' + (res.stderr || ''));
  }
  if (!readObj(o(1)).equals(readObj(o(2)))) {
    fail('check 21: paranoid hit produced different object bytes');
  }
  const code = linkAndRun(XGCC, o(2), exe);
  if (code !== 7) fail('check 21: paranoid-served program returned ' + code + ', expected 7');

  process.stdout.write(
    'check 21 OK: PARANOID=1 (forced content re-hash) still serves byte-identical objects\n');
}

// ---- 22. dependency files on manifest hits (-MD/-MMD contract) ------------
// A served hit must leave the SAME dependency information a real compile
// leaves: (a) driver-tier hit (-MD -MT -MF present) synthesizes the .d from
// the manifest records; (b) -MD without an explicit -MT (the driver spec
// then adds -MQ <output> to the cc1 line; whichever tier serves must leave
// the complete file); (c) -MMD (user-only deps: the openssl make shape,
// every TU compiled -MMD -MF -MT) is stored AND served too -- each record's
// CC_MHR_FLAG_SYSHDR bit carries libcpp's own first-stacking exclusion
// verdict, so the synthesized .d equals cpp's user-only output.  Before
// these fixes a driver-tier hit wrote NOTHING (silently erasing ninja's
// recorded header dependencies: deps = gcc treats a missing depfile as
// empty), and -MMD TUs never even stored a manifest, keeping every openssl
// warm build on the deep per-TU compiler-exec path.
{
  const dir = path.join(work, 'depsynth');
  fs.mkdirSync(dir, { recursive: true });
  const hdr = path.join(dir, 'dh.h');
  const src = path.join(dir, 'd.c');
  fs.writeFileSync(hdr, '#define DV 5\n');
  fs.writeFileSync(src, '#include "dh.h"\n#include <stdint.h>\nint main(void){ return DV; }\n');
  const cache = path.join(dir, 'cache');
  const o = (n) => path.join(dir, 'd' + n + '.o');
  const d = (n) => path.join(dir, 'd' + n + '.d');

  // Parse a make depfile into its sorted SET of prerequisites.  Duplicates
  // collapse: make/ninja ignore multiplicity, and the manifest legitimately
  // records the same path twice when cpp reached it via two search anchors
  // (e.g. the preincluded stdc-predef.h).
  const depSet = (p) => {
    const txt = fs.readFileSync(p, 'utf8');
    const ci = txt.indexOf(':');
    const body = ci >= 0 ? txt.slice(ci + 1) : txt;
    return [...new Set(
      body
        .replace(/\\\n/g, ' ')
        .split(/\s+/)
        .filter((s) => s && !s.endsWith(':'))
    )].sort().join('\n');
  };

  // NOTE: the M-family option values are EXCLUDED from the keys (they shape
  // only the .d side channel, regenerated from the live command line on
  // every serve).  The pairs below still repeat the identical command line
  // and delete the .d in between -- the point under test is that the hit
  // RECREATES the file, not cross-flag key sharing (check 20/21 cover key
  // behavior).

  // (a) driver-tier form: -MD -MT -MF all explicit.
  const argsA = ['-I' + dir, '-MD', '-MT', 'fixed-target.o', '-MF', d(1)];
  const r1 = compile(XGCC, src, o(1), cache, argsA);
  if (!/compile-cache: manifest-store /.test(r1.stderr)) {
    fail('check 22: expected manifest-store on the cold -MD compile\n' + r1.stderr);
  }
  const realSetA = depSet(d(1));
  fs.rmSync(d(1));
  const r2 = compile(XGCC, src, o(1), cache, argsA);
  if (!/compile-cache: manifest-hit /.test(r2.stderr)) {
    fail('check 22: expected manifest-hit on the warm -MD compile\n' + r2.stderr);
  }
  if (!fs.existsSync(d(1))) {
    fail('check 22: manifest hit left NO dependency file (the -MD contract)\n' + r2.stderr);
  }
  if (realSetA !== depSet(d(1))) {
    fail('check 22: hit-synthesized deps differ from the real compile\n--- real ---\n' +
         realSetA + '\n--- synthesized ---\n' + depSet(d(1)));
  }

  // (b) -MD without -MT (the driver spec adds -MQ <output> to the cc1 line;
  // the serving tier must still leave the complete file).  Because the
  // M-family options are excluded from the keys, this DIFFERENT dependency
  // spelling must hit entry (a) directly -- cross-flag key sharing -- and
  // still synthesize its own .d.
  const argsB = ['-I' + dir, '-MD', '-MF', d(2)];
  const r3 = compile(XGCC, src, o(2), cache, argsB);
  if (!/compile-cache: manifest-hit /.test(r3.stderr)) {
    fail('check 22: expected a manifest-hit for -MD without -MT (M-opts are unkeyed)\n' + r3.stderr);
  }
  if (!fs.existsSync(d(2))) {
    fail('check 22: -MD without -MT left no dependency file on a hit');
  }
  if (realSetA !== depSet(d(2))) {
    fail('check 22: -MD-without-MT deps differ from the real compile\n--- real ---\n' +
         realSetA + '\n--- got ---\n' + depSet(d(2)));
  }
  const realSetB = depSet(d(2));
  fs.rmSync(d(2));
  const r4 = compile(XGCC, src, o(2), cache, argsB);
  if (!/compile-cache: manifest-hit /.test(r4.stderr)) {
    fail('check 22: expected a manifest-hit on the repeat -MD-no-MT compile\n' + r4.stderr);
  }
  if (!fs.existsSync(d(2))) {
    fail('check 22: repeat hit did not recreate the deleted .d');
  }
  if (realSetB !== depSet(d(2))) {
    fail('check 22: recreated deps differ\n--- before ---\n' +
         realSetB + '\n--- after ---\n' + depSet(d(2)));
  }

  // (c) -MMD (user-only deps): a dedicated cache dir makes the cold pass
  // explicit -- cold = real compile + manifest-store + cpp's OWN user-only
  // .d (the reference); warm = manifest-hit that re-synthesizes the deleted
  // .d with the IDENTICAL user-only set (system headers excluded, the TU's
  // own files present).
  const cacheC = path.join(dir, 'cache-mmd');
  const argsC = ['-I' + dir, '-MMD', '-MT', 'mmd-target.o', '-MF', d(3)];
  const r5 = compile(XGCC, src, o(3), cacheC, argsC);
  if (!/compile-cache: manifest-store /.test(r5.stderr)) {
    fail('check 22: expected manifest-store on the cold -MMD compile\n' + r5.stderr);
  }
  const realSetC = depSet(d(3));
  if (/stdint\.h/.test(realSetC)) {
    fail("check 22: sanity: cpp's own -MMD deps include a system header\n" + realSetC);
  }
  if (!/dh\.h/.test(realSetC) || !/d\.c/.test(realSetC)) {
    fail('check 22: sanity: -MMD reference deps lost the user header or main source\n' + realSetC);
  }
  fs.rmSync(d(3));
  const r6 = compile(XGCC, src, o(3), cacheC, argsC);
  if (!/compile-cache: manifest-hit /.test(r6.stderr)) {
    fail('check 22: expected manifest-hit on the warm -MMD compile\n' + r6.stderr);
  }
  if (!fs.existsSync(d(3))) {
    fail('check 22: -MMD manifest hit left no dependency file');
  }
  if (realSetC !== depSet(d(3))) {
    fail("check 22: -MMD hit-synthesized deps differ from cpp's own user-only set\n--- real ---\n" +
         realSetC + '\n--- synthesized ---\n' + depSet(d(3)));
  }

  process.stdout.write(
    'check 22 OK: hits honor -MD (explicit-MT and spec-MQ forms) and -MMD (user-only set == cpp\'s own)\n');
}

// ---- check 23: -Wa options are never silently dropped (A2) ---------------
// gas_assemble_buffer() takes no options, so a compile with a non-empty
// -Wa,... must auto-fall back to the external-as pipeline: the option's
// effect must be visible in the object (here --defsym plants a symbol whose
// name must appear in the object's string table), the compile cache must
// skip both serve and store (skip-no-integrated-as), and an EXPLICIT
// -fintegrated-as combined with -Wa must be a hard driver error.
{
  const dir = path.join(work, 'c23');
  fs.mkdirSync(dir);
  const src = path.join(dir, 'wa.c');
  fs.writeFileSync(src, 'int wa_f(int x) { return x * 3; }\n');
  const cache = path.join(dir, 'cache');
  fs.mkdirSync(cache);
  const obj = path.join(dir, 'wa.o');

  // (a) plain compile (no -Wa): symbol absent, cache stores normally.
  let r = spawnSync(XGCC, ['-O2', '-c', src, '-o', obj,
                           '-fcompile-cache=' + cache, B],
                    { env: debugEnv, encoding: 'utf8' });
  if (r.status !== 0) fail('check 23: plain compile failed\n' + r.stderr);
  if (readObj(obj).includes('CCVERIFYWA'))
    fail('check 23: control object unexpectedly contains the defsym name');
  if (!/compile-cache: store /.test(r.stderr))
    fail('check 23: control compile did not store\n' + r.stderr);

  // (b) -Wa,--defsym: fallback runs external as; symbol lands in the .o;
  // cache is skipped with the tag; nothing is served or stored.
  r = spawnSync(XGCC, ['-O2', '-c', src, '-o', obj,
                       '-fcompile-cache=' + cache,
                       '-Wa,--defsym,CCVERIFYWA=41', B],
                { env: debugEnv, encoding: 'utf8' });
  if (r.status !== 0) fail('check 23: -Wa compile failed\n' + r.stderr);
  if (!readObj(obj).includes('CCVERIFYWA'))
    fail('check 23: -Wa,--defsym did not reach the assembler (symbol ' +
         'missing from the object -- the option was dropped)');
  if (!/skip-no-integrated-as/.test(r.stderr))
    fail('check 23: -Wa compile did not tag skip-no-integrated-as\n' + r.stderr);
  if (/compile-cache: (store|hit|manifest-hit) /.test(r.stderr))
    fail('check 23: -Wa compile must neither store nor serve\n' + r.stderr);

  // (c) -Xassembler spelling takes the same path.
  r = spawnSync(XGCC, ['-O2', '-c', src, '-o', obj,
                       '-Xassembler', '--defsym', '-Xassembler', 'CCVERIFYXA=7',
                       B],
                { env: debugEnv, encoding: 'utf8' });
  if (r.status !== 0) fail('check 23: -Xassembler compile failed\n' + r.stderr);
  if (!readObj(obj).includes('CCVERIFYXA'))
    fail('check 23: -Xassembler options were dropped');

  // (d) explicit -fintegrated-as + -Wa: loud driver error, never silence.
  r = spawnSync(XGCC, ['-O2', '-c', src, '-o', obj,
                       '-Wa,--defsym,CCVERIFYWA=41', '-fintegrated-as', B],
                { env: debugEnv, encoding: 'utf8' });
  if (r.status === 0)
    fail('check 23: -fintegrated-as + -Wa must be an error, got success');
  if (!/cannot be passed to the integrated assembler/.test(r.stderr))
    fail('check 23: expected the integrated-assembler conflict error\n' + r.stderr);

  process.stdout.write(
    'check 23 OK: -Wa/-Xassembler reach the external as (defsym present), ' +
    'cache skipped with tag, explicit conflict errors\n');
}

// ---- check 24: -fno-integrated-as pipeline (A1) ---------------------------
// The restored escape hatch: cc1 writes text, the external as produces the
// object.  The object must be byte-identical to the integrated one (same
// binutils 2.42 bits in-process and out), and the compile must be
// cache-ineligible in BOTH directions: it neither stores nor is served
// from a warm cache entry stored by an integrated compile.
{
  const dir = path.join(work, 'c24');
  fs.mkdirSync(dir);
  const src = path.join(dir, 'a1.c');
  fs.writeFileSync(src,
    '#include <stdint.h>\n' +
    'uint32_t a1_f(uint32_t x) { return (x << 3) ^ 0x5a5a5a5au; }\n');
  const cache = path.join(dir, 'cache');
  fs.mkdirSync(cache);
  const objI = path.join(dir, 'a1-int.o');
  const objE = path.join(dir, 'a1-ext.o');

  // Integrated compile, stores into the cache.
  let r = compile(XGCC, src, objI, cache);
  if (!keyFor(r.keys, 'store'))
    fail('check 24: integrated compile did not store\n' + r.stderr);

  // External compile on the WARM cache: byte-identical object, no serve,
  // no store, tagged skip.
  r = spawnSync(XGCC, ['-O2', '-c', src, '-o', objE,
                       '-fcompile-cache=' + cache, '-fno-integrated-as', B],
                { env: debugEnv, encoding: 'utf8' });
  if (r.status !== 0)
    fail('check 24: -fno-integrated-as compile failed\n' + r.stderr);
  if (!readObj(objE).equals(readObj(objI)))
    fail('check 24: external-as object differs from the integrated one');
  if (!/skip-no-integrated-as/.test(r.stderr))
    fail('check 24: expected the skip-no-integrated-as tag\n' + r.stderr);
  if (/compile-cache: (store|hit|manifest-hit) /.test(r.stderr))
    fail('check 24: -fno-integrated-as must neither store nor serve\n' + r.stderr);

  // And it must genuinely be the two-process pipeline: -### shows an as
  // invocation for the external form, none for the integrated default.
  const dashes = (args) => spawnSync(XGCC, args.concat('-###'),
                                     { encoding: 'utf8' }).stderr || '';
  const extCmds = dashes(['-O2', '-c', src, '-o', objE, '-fno-integrated-as', B]);
  if (!/\bas\b[^\n]*--64/.test(extCmds))
    fail('check 24: -### shows no external as command for -fno-integrated-as\n'
         + extCmds);
  const intCmds = dashes(['-O2', '-c', src, '-o', objI, B]);
  if (/\n[^\n]*\bas\b[^\n]*--64/.test(intCmds))
    fail('check 24: integrated default unexpectedly spawns as\n' + intCmds);

  process.stdout.write(
    'check 24 OK: -fno-integrated-as compiles via external as, ' +
    'byte-identical object, cache-skipped both ways\n');
}

// ---- check 25: GCC_COMPILE_CACHE_MAX_SIZE eviction (B1) -------------------
// Tiny cap, many stores: shards over their cap/256 budget sweep their
// least-recently-used entries; a later compile of an evicted TU is a clean
// miss+store; total cache size stays bounded; a serve hit refreshes a stale
// entry's mtime (the LRU clock); the PARANOID mode is unaffected.
{
  const dir = path.join(work, 'c25');
  fs.mkdirSync(dir);
  const cache = path.join(dir, 'cache');
  fs.mkdirSync(cache);
  const capEnv = { ...debugEnv, GCC_COMPILE_CACHE_MAX_SIZE: '1M' };

  // 80 distinct TUs with ~2.9K objects (measured: pad[1500] -> 2976 B at
  // -O2): one object fits a shard's 4096-byte budget (1M/256) even next to
  // its ~300 B manifest, but any shard receiving TWO objects exceeds it and
  // must sweep the older one.  With 80 objects over 256 shards the chance
  // that no shard ever gets two is < 1e-5.
  const N = 80;
  const storeKey = [];   // per-TU object key (12 hex) in store order
  for (let i = 0; i < N; i++) {
    const s = path.join(dir, 't' + i + '.c');
    fs.writeFileSync(s,
      'static const char pad' + i + '[1500] = {1,2,3};\n' +
      'const char *f' + i + '(int k) { return pad' + i + ' + (k % 1500); }\n');
    const r = spawnSync(XGCC, ['-O2', '-c', s, '-o', path.join(dir, 't' + i + '.o'),
                               '-fcompile-cache=' + cache, '-fintegrated-as', B],
                        { env: capEnv, encoding: 'utf8' });
    if (r.status !== 0)
      fail('check 25: store #' + i + ' failed\n' + r.stderr);
    const k = keyFor(parseKeys(r.stderr), 'store');
    if (!k) fail('check 25: store #' + i + ' logged no store\n' + r.stderr);
    storeKey.push(k);
  }

  // Object entry present?  key12 = shard(2) + 10 more hex of the basename.
  const objPresent = (k) => {
    const shard = path.join(cache, k.slice(0, 2));
    let names = [];
    try { names = fs.readdirSync(shard); } catch { return false; }
    return names.some((n) => n.startsWith(k.slice(2)) && n.endsWith('.o'));
  };

  const evicted = storeKey.filter((k) => !objPresent(k));
  if (evicted.length === 0)
    fail('check 25: no object was evicted under a 1M cap after ' + N +
         ' stores (eviction never ran?)');

  // LRU order: in every shard that holds exactly two of our stores with
  // exactly one survivor, the survivor must be the LATER store.
  const byShard = new Map();
  storeKey.forEach((k, i) => {
    const s = k.slice(0, 2);
    if (!byShard.has(s)) byShard.set(s, []);
    byShard.get(s).push(i);
  });
  for (const [, idxs] of byShard) {
    if (idxs.length !== 2) continue;
    const alive = idxs.filter((i) => objPresent(storeKey[i]));
    if (alive.length === 1 && alive[0] !== Math.max(idxs[0], idxs[1]))
      fail('check 25: LRU violated: older store ' + storeKey[alive[0]] +
           ' survived while newer ' +
           storeKey[Math.max(idxs[0], idxs[1])] + ' was evicted');
  }

  // Global bound: per-shard sweeps imply a total cap (+ transient slack).
  let total = 0;
  const stack = [cache];
  while (stack.length) {
    const d = stack.pop();
    for (const ent of fs.readdirSync(d, { withFileTypes: true })) {
      const full = path.join(d, ent.name);
      if (ent.isDirectory()) stack.push(full);
      else total += fs.statSync(full).size;
    }
  }
  if (total > 1.25 * 1024 * 1024)
    fail('check 25: cache size ' + total + ' exceeds the 1M cap (+slack)');

  // Recompiling an evicted TU is a clean miss+store, not an error.
  const ei = storeKey.indexOf(evicted[0]);
  const r2 = spawnSync(XGCC, ['-O2', '-c', path.join(dir, 't' + ei + '.c'),
                              '-o', path.join(dir, 're.o'),
                              '-fcompile-cache=' + cache, '-fintegrated-as', B],
                       { env: capEnv, encoding: 'utf8' });
  if (r2.status !== 0)
    fail('check 25: recompile of evicted TU failed\n' + r2.stderr);
  if (keyFor(parseKeys(r2.stderr), 'hit'))
    fail('check 25: evicted TU somehow served a hit\n' + r2.stderr);
  if (keyFor(parseKeys(r2.stderr), 'store') !== evicted[0])
    fail('check 25: evicted TU did not re-store under its key\n' + r2.stderr);
  if (!readObj(path.join(dir, 're.o'))
        .equals(readObj(path.join(dir, 't' + ei + '.o'))))
    fail('check 25: re-stored object differs from the original');

  // Serve-hit mtime bump: backdate a surviving entry 2h, hit it, and the
  // cache file must be fresh again (so LRU spares hot entries).
  const sk = storeKey.find((k) => objPresent(k));
  if (!sk)
    fail('check 25: every object was evicted -- the per-shard budget ' +
         'cannot even hold one ~2.9K object (cap arithmetic broken?)');
  const shardDir = path.join(cache, sk.slice(0, 2));
  const objName = fs.readdirSync(shardDir)
    .find((n) => n.startsWith(sk.slice(2)) && n.endsWith('.o'));
  const objPath = path.join(shardDir, objName);
  const old = new Date(Date.now() - 2 * 3600 * 1000);
  fs.utimesSync(objPath, old, old);
  const si = storeKey.indexOf(sk);
  const r3 = spawnSync(XGCC, ['-O2', '-c', path.join(dir, 't' + si + '.c'),
                              '-o', path.join(dir, 'hb.o'),
                              '-fcompile-cache=' + cache, '-fintegrated-as', B],
                       { env: capEnv, encoding: 'utf8' });
  if (r3.status !== 0 || !keyFor(parseKeys(r3.stderr), 'hit'))
    fail('check 25: warm hit for the bump test did not happen\n' + r3.stderr);
  if (fs.statSync(objPath).mtimeMs < Date.now() - 300 * 1000)
    fail('check 25: serve hit did not bump the cache entry mtime');

  // PARANOID pass still hits under a cap.
  const r4 = spawnSync(XGCC, ['-O2', '-c', path.join(dir, 't' + si + '.c'),
                              '-o', path.join(dir, 'pp.o'),
                              '-fcompile-cache=' + cache, '-fintegrated-as', B],
                       { env: { ...capEnv, GCC_COMPILE_CACHE_PARANOID: '1' },
                         encoding: 'utf8' });
  if (r4.status !== 0 || !keyFor(parseKeys(r4.stderr), 'hit'))
    fail('check 25: PARANOID warm hit failed under the cap\n' + r4.stderr);

  process.stdout.write(
    'check 25 OK: 1M cap evicted ' + evicted.length + '/' + N +
    ' LRU entries, size bounded, evicted TU re-misses cleanly, ' +
    'hits bump mtime, PARANOID unaffected\n');
}

// ---- check 26: prefix-map-aware -g keys (B2) ------------------------------
// Two build dirs that each map THEMSELVES to "." share manifest keys under
// -g and serve byte-identical objects across dirs; without maps the -g keys
// still embed the raw cwd and must miss; and a map that matches neither the
// source nor the cwd stays IN the key (differing values must miss -- the
// soundness edge ccache's blanket exclusion gets wrong).
{
  const dir = path.join(work, 'c26');
  const srcDir = path.join(dir, 'src');
  const dA = path.join(dir, 'dA');
  const dB = path.join(dir, 'dB');
  for (const d of [dir, srcDir, dA, dB]) fs.mkdirSync(d);
  fs.writeFileSync(path.join(srcDir, 'h.h'), '#define HVAL 5\n');
  const src = path.join(srcDir, 'm.c');
  fs.writeFileSync(src,
    '#include "h.h"\nint b2_f(int x) { return x + HVAL; }\n');
  const cache = path.join(dir, 'cache');
  fs.mkdirSync(cache);

  const gc = (cwd, out, extra) => spawnSync(
    XGCC,
    ['-O2', '-g', '-c', src, '-o', out, '-I' + srcDir,
     '-fcompile-cache=' + cache, '-fintegrated-as', B].concat(extra),
    { cwd, env: debugEnv, encoding: 'utf8' });

  // (a) dA cold store with -ffile-prefix-map=$dA=. ...
  let r = gc(dA, path.join(dA, 'm.o'), ['-ffile-prefix-map=' + dA + '=.']);
  if (r.status !== 0) fail('check 26: dA compile failed\n' + r.stderr);
  if (!/compile-cache: manifest-store /.test(r.stderr))
    fail('check 26: dA did not store a manifest\n' + r.stderr);

  // ... (b) dB with -ffile-prefix-map=$dB=. must manifest-hit and place a
  // byte-identical object.
  r = gc(dB, path.join(dB, 'm.o'), ['-ffile-prefix-map=' + dB + '=.']);
  if (r.status !== 0) fail('check 26: dB compile failed\n' + r.stderr);
  if (!/compile-cache: manifest-hit /.test(r.stderr))
    fail('check 26: cross-dir -g manifest hit missing (prefix maps not ' +
         'folded into the key?)\n' + r.stderr);
  if (!readObj(path.join(dA, 'm.o')).equals(readObj(path.join(dB, 'm.o'))))
    fail('check 26: cross-dir served object is not byte-identical');

  // (c) negative: -g WITHOUT maps across dirs must still miss (raw cwd in
  // both the key and the DWARF).
  const cache2 = path.join(dir, 'cache2');
  fs.mkdirSync(cache2);
  const gc2 = (cwd, out) => spawnSync(
    XGCC,
    ['-O2', '-g', '-c', src, '-o', out, '-I' + srcDir,
     '-fcompile-cache=' + cache2, '-fintegrated-as', B],
    { cwd, env: debugEnv, encoding: 'utf8' });
  r = gc2(dA, path.join(dA, 'n.o'));
  if (r.status !== 0) fail('check 26: negative dA compile failed\n' + r.stderr);
  r = gc2(dB, path.join(dB, 'n.o'));
  if (r.status !== 0) fail('check 26: negative dB compile failed\n' + r.stderr);
  if (/compile-cache: (manifest-hit|hit) /.test(r.stderr))
    fail('check 26: -g without maps must not hit across dirs\n' + r.stderr);
  if (readObj(path.join(dA, 'n.o')).equals(readObj(path.join(dB, 'n.o'))))
    fail('check 26: -g objects from different dirs unexpectedly identical');

  // (d) soundness: two compiles differing ONLY in a map that matches
  // neither the source nor the cwd must key apart (no serve).
  const cache3 = path.join(dir, 'cache3');
  fs.mkdirSync(cache3);
  const gc3 = (map, out) => spawnSync(
    XGCC,
    ['-O2', '-g', '-c', src, '-o', out, '-I' + srcDir,
     '-fcompile-cache=' + cache3, '-fintegrated-as',
     '-ffile-prefix-map=' + map, B],
    { cwd: dA, env: debugEnv, encoding: 'utf8' });
  r = gc3('/ccverify-xx1=y', path.join(dA, 's1.o'));
  if (r.status !== 0) fail('check 26: soundness compile 1 failed\n' + r.stderr);
  r = gc3('/ccverify-xx2=y', path.join(dA, 's2.o'));
  if (r.status !== 0) fail('check 26: soundness compile 2 failed\n' + r.stderr);
  if (/compile-cache: (manifest-hit|hit) /.test(r.stderr))
    fail('check 26: a differing unrelated prefix-map must MISS (it can ' +
         'rewrite other DWARF paths)\n' + r.stderr);

  process.stdout.write(
    'check 26 OK: cross-dir -g prefix-map manifest-hit + byte-identical .o; ' +
    'no-map cross-dir still misses; unrelated-map difference still keys apart\n');
}

// ---- check 27: -save-temps / -gsplit-dwarf auto-fallback ------------------
// Both options promise artifacts only the external-as pipeline produces
// (-save-temps the on-disk .s/.i, -gsplit-dwarf the objcopy-extracted .dwo),
// so the driver must auto-inject -fno-integrated-as for them: the artifacts
// must exist, the fallback note must name the trigger, the compile must be
// cache-skipped (skip-no-integrated-as) in both directions, and the object
// must stay byte-identical to the integrated one.  An EXPLICIT
// -fintegrated-as wins (no fallback, no error -- unlike -Wa nothing the
// user passed is dropped).
{
  const dir = path.join(work, 'c27');
  fs.mkdirSync(dir);
  const src = path.join(dir, 'st.c');
  fs.writeFileSync(src, 'int st_f(int x) { return x - 9; }\n');
  const cache = path.join(dir, 'cache');
  fs.mkdirSync(cache);
  const objI = path.join(dir, 'st-int.o');

  // Control: integrated compile stores.
  let r = compile(XGCC, src, objI, cache);
  if (!keyFor(r.keys, 'store'))
    fail('check 27: integrated control compile did not store\n' + r.stderr);

  // (a) -save-temps on the WARM cache: fallback note, .i/.s on disk,
  // skip tag, no serve/store, byte-identical object.
  const objS = path.join(dir, 'st-save.o');
  r = spawnSync(XGCC, ['-O2', '-c', src, '-o', objS,
                       '-fcompile-cache=' + cache, '-save-temps', B],
                { cwd: dir, env: debugEnv, encoding: 'utf8' });
  if (r.status !== 0) fail('check 27: -save-temps compile failed\n' + r.stderr);
  if (!/note: -save-temps: using the external assembler/.test(r.stderr))
    fail('check 27: missing -save-temps fallback note\n' + r.stderr);
  // -save-temps names the kept temps after the output: -o st-save.o
  // keeps st-save.i / st-save.s (see gcc.misc-tests/outputs.exp
  // "obj savetmp named0").
  for (const t of ['st-save.i', 'st-save.s'])
    if (!fs.existsSync(path.join(dir, t)))
      fail('check 27: -save-temps did not keep ' + t);
  if (!/skip-no-integrated-as/.test(r.stderr))
    fail('check 27: -save-temps compile not tagged skip\n' + r.stderr);
  if (/compile-cache: (store|hit|manifest-hit) /.test(r.stderr))
    fail('check 27: -save-temps must neither store nor serve\n' + r.stderr);
  if (!readObj(objS).equals(readObj(objI)))
    fail('check 27: -save-temps object differs from the integrated one');

  // (b) -gsplit-dwarf: fallback note names it; ASM_FINAL_SPEC's objcopy
  // pass leaves the .dwo beside the object and strips the .o.
  const objD = path.join(dir, 'st-dwo.o');
  r = spawnSync(XGCC, ['-O2', '-g', '-gsplit-dwarf', '-c', src, '-o', objD,
                       '-fcompile-cache=' + cache, B],
                { cwd: dir, env: debugEnv, encoding: 'utf8' });
  if (r.status !== 0) fail('check 27: -gsplit-dwarf compile failed\n' + r.stderr);
  if (!/note: -gsplit-dwarf: using the external assembler/.test(r.stderr))
    fail('check 27: missing -gsplit-dwarf fallback note\n' + r.stderr);
  const dwo = path.join(dir, 'st-dwo.dwo');
  if (!fs.existsSync(dwo))
    fail('check 27: -gsplit-dwarf produced no .dwo');
  if (!readObj(dwo).includes('.debug_info.dwo'))
    fail('check 27: .dwo lacks the .debug_info.dwo section');
  if (readObj(objD).includes('.debug_info.dwo'))
    fail('check 27: .o still carries .debug_info.dwo (objcopy --strip-dwo ' +
         'did not run)');
  if (!/skip-no-integrated-as/.test(r.stderr))
    fail('check 27: -gsplit-dwarf compile not tagged skip\n' + r.stderr);

  // (c) explicit -fintegrated-as + -save-temps: the user's choice wins --
  // no fallback (and no .s: the in-process assembler has none to save).
  const d2 = path.join(dir, 'explicit');
  fs.mkdirSync(d2);
  const objX = path.join(d2, 'st-x.o');
  r = spawnSync(XGCC, ['-O2', '-c', src, '-o', objX,
                       '-save-temps', '-fintegrated-as', B],
                { cwd: d2, env: debugEnv, encoding: 'utf8' });
  if (r.status !== 0)
    fail('check 27: -fintegrated-as -save-temps must not error\n' + r.stderr);
  if (/using the external assembler/.test(r.stderr))
    fail('check 27: explicit -fintegrated-as must suppress the fallback\n'
         + r.stderr);
  if (fs.existsSync(path.join(d2, 'st-x.s')))
    fail('check 27: explicit integrated compile unexpectedly wrote a .s');

  process.stdout.write(
    'check 27 OK: -save-temps keeps .i/.s and -gsplit-dwarf splits the .dwo ' +
    'via the external-as fallback (noted, cache-skipped); explicit ' +
    '-fintegrated-as wins\n');
}

// ---- check 28: duplicate -o is last-one-wins ------------------------------
// The integrated arm hands %W{o*} to cc1, which rejects a duplicate -o
// ("output filename specified twice") -- the external as and ld just took
// the last one.  The driver now kills the earlier -o (check_live_switch),
// so a duplicate -o compiles cleanly, writes ONLY the last name, stays
// integrated and cache-eligible, and the link path behaves the same.
{
  const dir = path.join(work, 'c28');
  fs.mkdirSync(dir);
  const src = path.join(dir, 'oo.c');
  fs.writeFileSync(src, 'int main(void) { return 42; }\n');
  const cache = path.join(dir, 'cache');
  fs.mkdirSync(cache);
  const dead = path.join(dir, 'dead.o');
  const live = path.join(dir, 'live.o');

  // (a) -c with two -o: last wins, first never created, still a normal
  // integrated miss+store.
  let r = spawnSync(XGCC, ['-O2', '-c', src, '-o', dead, '-o', live,
                           '-fcompile-cache=' + cache, B],
                    { cwd: dir, env: debugEnv, encoding: 'utf8' });
  if (r.status !== 0)
    fail('check 28: duplicate -o compile failed\n' + r.stderr);
  if (!fs.existsSync(live)) fail('check 28: last -o was not written');
  if (fs.existsSync(dead)) fail('check 28: earlier (dead) -o was written');
  if (!/compile-cache: store /.test(r.stderr))
    fail('check 28: duplicate -o compile must stay cache-eligible\n'
         + r.stderr);

  // (b) warm repeat serves, byte-identical to a plain single -o compile.
  r = spawnSync(XGCC, ['-O2', '-c', src, '-o', dead, '-o', live,
                       '-fcompile-cache=' + cache, B],
                { cwd: dir, env: debugEnv, encoding: 'utf8' });
  if (r.status !== 0 || !/compile-cache: (manifest-hit|hit) /.test(r.stderr))
    fail('check 28: warm duplicate -o compile did not serve\n' + r.stderr);
  const single = path.join(dir, 'single.o');
  r = spawnSync(XGCC, ['-O2', '-c', src, '-o', single, B],
                { cwd: dir, env: debugEnv, encoding: 'utf8' });
  if (r.status !== 0) fail('check 28: single -o compile failed\n' + r.stderr);
  if (!readObj(live).equals(readObj(single)))
    fail('check 28: duplicate -o object differs from single -o object');

  // (c) the PR shape that used to error: a dump flag plus -o /dev/null
  // before the real -c -o.
  const outO = path.join(dir, 'devnull-shape.o');
  r = spawnSync(XGCC, ['-fdump-ipa-clones', '-o', '/dev/null', '-c',
                       '-o', outO, src, B],
                { cwd: dir, env: debugEnv, encoding: 'utf8' });
  if (r.status !== 0)
    fail('check 28: -fdump-ipa-clones -o /dev/null -c -o out.o errored\n'
         + r.stderr);
  if (!fs.existsSync(outO))
    fail('check 28: devnull-dump shape did not write the real -o');

  // (d) link path: ld saw the full list before and took the last; with the
  // dedup it still produces exactly the last name.
  const deadExe = path.join(dir, 'dead.exe');
  const liveExe = path.join(dir, 'live.exe');
  r = spawnSync(XGCC, [live, '-o', deadExe, '-o', liveExe, B],
                { cwd: dir, encoding: 'utf8' });
  if (r.status !== 0) fail('check 28: duplicate -o link failed\n' + r.stderr);
  if (!fs.existsSync(liveExe) || fs.existsSync(deadExe))
    fail('check 28: link must write only the last -o');
  const run = spawnSync(liveExe, [], { encoding: 'utf8' });
  if (run.status !== 42)
    fail('check 28: linked program returned ' + run.status + ', wanted 42');

  process.stdout.write(
    'check 28 OK: duplicate -o compiles clean (last wins, first absent), ' +
    'stays integrated + cached, devnull-dump shape ok, link dedups too\n');
}

// ---- check 29: MK search-path normalization (ccache base_dir parity) ------
// The 16-TU llama.cpp class: a TU whose -I points INSIDE the build dir
// (cmake passes generated-header include dirs as absolute paths), no -g.
// The absolute -I value used to fold into the manifest key raw, so a fresh
// build dir re-keyed the TU: pre-parse manifest miss, full parse, post-parse
// object hit (~2.5 s each; ~12.3 s of the 13.2 s fork-vs-ccache warm gap).
// Search-path values inside the cwd now hash cwd-relative, so build dir B
// manifest-hits the entry stored from build dir A -- while values OUTSIDE
// the cwd still hash raw (the anti-shadowing bar is unchanged for
// directories the build tree does not own).
{
  const dir = path.join(work, 'c29');
  const srcDir = path.join(dir, 'src');
  const dA = path.join(dir, 'dA');
  const dB = path.join(dir, 'dB');
  for (const d of [dir, srcDir, dA, dB]) fs.mkdirSync(d);
  // Per-build-dir "generated" header, identical content in both build dirs
  // (the same configure step produced it).
  for (const d of [dA, dB]) {
    fs.mkdirSync(path.join(d, 'gen'));
    fs.writeFileSync(path.join(d, 'gen', 'conf.h'), '#define GENV 9\n');
  }
  const src = path.join(srcDir, 'm.cpp');
  fs.writeFileSync(src,
    '#include "conf.h"\nint c29_f(int x) { return x * GENV; }\n');
  const cache = path.join(dir, 'cache');
  fs.mkdirSync(cache);

  const gpp = (cwd, inc, out) => spawnSync(
    XGPP,
    ['-O2', '-c', src, '-o', out, '-I' + inc,
     '-fcompile-cache=' + cache, '-fintegrated-as', B],
    { cwd, env: debugEnv, encoding: 'utf8' });

  // (a) build dir A: cold miss + store, manifest recorded.
  let r = gpp(dA, path.join(dA, 'gen'), path.join(dA, 'm.o'));
  if (r.status !== 0) fail('check 29: dA compile failed\n' + r.stderr);
  if (!/compile-cache: manifest-store /m.test(r.stderr))
    fail('check 29: dA did not store a manifest\n' + r.stderr);

  // (b) build dir B, fresh dir, same relative layout: the -I differs only
  // in the build-dir prefix, which the MK relativizes away -> PRE-PARSE
  // manifest hit (not a deep hit, not a recompile), byte-identical object.
  r = gpp(dB, path.join(dB, 'gen'), path.join(dB, 'm.o'));
  if (r.status !== 0) fail('check 29: dB compile failed\n' + r.stderr);
  if (!/compile-cache: manifest-hit /m.test(r.stderr))
    fail('check 29: cross-build-dir manifest hit missing (build-dir -I not ' +
         'relativized in the MK?)\n' + r.stderr);
  if (/compile-cache: (hit|miss) /m.test(r.stderr))
    fail('check 29: dB compile fell past the pre-parse tier\n' + r.stderr);
  if (!readObj(path.join(dA, 'm.o')).equals(readObj(path.join(dB, 'm.o'))))
    fail('check 29: cross-build-dir served object is not byte-identical');

  // (c) anti-shadowing preserved: the SAME header content reached via -I
  // dirs OUTSIDE the build dir must still key by the raw absolute value --
  // two different out-of-tree dirs may not share a manifest entry. (The
  // object still deep-hits: the include closure content is identical, and
  // OK deliberately drops search paths.)
  const outX = path.join(dir, 'outX');
  const outY = path.join(dir, 'outY');
  for (const d of [outX, outY]) {
    fs.mkdirSync(d);
    fs.writeFileSync(path.join(d, 'conf.h'), '#define GENV 9\n');
  }
  r = gpp(dA, outX, path.join(dA, 'x.o'));
  if (r.status !== 0) fail('check 29: outX compile failed\n' + r.stderr);
  if (!/compile-cache: manifest-miss /m.test(r.stderr))
    fail('check 29: out-of-tree -I outX must MISS the manifest tier (was ' +
         'the value wrongly relativized?)\n' + r.stderr);
  r = gpp(dA, outY, path.join(dA, 'y.o'));
  if (r.status !== 0) fail('check 29: outY compile failed\n' + r.stderr);
  if (!/compile-cache: manifest-miss /m.test(r.stderr))
    fail('check 29: differing out-of-tree -I dirs must key apart\n' +
         r.stderr);

  process.stdout.write(
    'check 29 OK: build-dir -I manifest-hits across build dirs ' +
    '(byte-identical, pre-parse); out-of-tree -I values still key raw\n');
}

// ---- check 30: a deep hit stores the manifest (manifest-store-on-hit) -----
// The 4-TU llama.cpp class (per-target get-model.cpp twins): a TU can
// OBJECT-hit without a manifest ever existing under its own MK -- extra -I
// dirs give it a distinct MK, while the include closure (and thus the
// content-addressed object key) matches a twin that already stored. The
// deep-hit path used to store nothing (compile_cache_store no-ops on
// cc_hit), so such a TU re-paid the full parse on EVERY warm build. Now the
// deep hit stores/refreshes the manifest from the just-computed closure and
// the next compile serves pre-parse.
{
  const dir = path.join(work, 'c30');
  fs.mkdirSync(dir);
  const src = path.join(dir, 'gm.cpp');
  fs.writeFileSync(src, 'int c30_f(int x) { return x - 3; }\n');
  const cache = path.join(dir, 'cache');
  fs.mkdirSync(cache);
  // Exists but holds no headers: changes the MK (search-path VALUE) without
  // changing the include closure -> the OK stays the twin's.
  const extraInc = path.join(dir, 'extra-inc');
  fs.mkdirSync(extraInc);

  const gpp = (out, extra) => spawnSync(
    XGPP,
    ['-O2', '-c', src, '-o', out,
     '-fcompile-cache=' + cache, '-fintegrated-as', B].concat(extra),
    { cwd: dir, env: debugEnv, encoding: 'utf8' });

  // (a) the "twin": plain compile, miss + store (+ manifest under ITS MK).
  let r = gpp(path.join(dir, 'gm1.o'), []);
  if (r.status !== 0) fail('check 30: twin compile failed\n' + r.stderr);
  if (!/compile-cache: store /m.test(r.stderr))
    fail('check 30: twin compile did not store\n' + r.stderr);

  // (b) same TU + an extra empty -I: distinct MK (pre-parse manifest miss),
  // same OK (post-parse deep hit) -- and the deep hit must now store the
  // manifest under THIS MK.
  r = gpp(path.join(dir, 'gm2.o'), ['-I' + extraInc]);
  if (r.status !== 0) fail('check 30: deep-hit compile failed\n' + r.stderr);
  if (!/compile-cache: manifest-miss /m.test(r.stderr))
    fail('check 30: expected a manifest miss for the new MK\n' + r.stderr);
  if (!/compile-cache: hit /m.test(r.stderr))
    fail('check 30: expected a post-parse object hit\n' + r.stderr);
  if (!/compile-cache: manifest-store-on-hit /m.test(r.stderr))
    fail('check 30: deep hit did not store the manifest ' +
         '(manifest-store-on-hit missing)\n' + r.stderr);
  if (/compile-cache: (store|manifest-store) /m.test(r.stderr))
    fail('check 30: deep hit must not re-store the object or take the ' +
         'miss-path manifest store\n' + r.stderr);

  // (c) the exact same command again: the manifest stored in (b) now serves
  // PRE-PARSE -- no deep hit, no parse.
  r = gpp(path.join(dir, 'gm3.o'), ['-I' + extraInc]);
  if (r.status !== 0) fail('check 30: converged compile failed\n' + r.stderr);
  if (!/compile-cache: manifest-hit /m.test(r.stderr))
    fail('check 30: the on-hit-stored manifest did not serve pre-parse\n' +
         r.stderr);
  if (/compile-cache: (hit|miss) /m.test(r.stderr))
    fail('check 30: converged compile still fell past the pre-parse tier\n' +
         r.stderr);
  const g1 = readObj(path.join(dir, 'gm1.o'));
  if (!g1.equals(readObj(path.join(dir, 'gm2.o'))) ||
      !g1.equals(readObj(path.join(dir, 'gm3.o'))))
    fail('check 30: served objects are not byte-identical');

  process.stdout.write(
    'check 30 OK: deep hit stores the manifest (manifest-store-on-hit); ' +
    'the next compile manifest-hits pre-parse; objects byte-identical\n');
}

// ---- check 31: per-language compiler-id -> C serves at the driver tier ----
// cc1 and cc1plus share one cache dir, and the single compiler-id sidecar
// was last-store-wins: after any C++ store the driver keyed C TUs with
// cc1plus's checksum+lang -- wrong MK, no driver-tier serve, and every warm
// C compile still spawned cc1 (all 11 C TUs of the llama.cpp warm build).
// With per-language sidecars the driver reads compiler-id-cc1 for a cc1
// command even when a C++ TU stored last. -v proves the no-spawn: the
// driver prints every command it executes, so a served compile must show
// the manifest-hit line and NO cc1/cc1plus invocation.
{
  const dir = path.join(work, 'c31');
  fs.mkdirSync(dir);
  const cSrc = path.join(dir, 'cf.c');
  const cppSrc = path.join(dir, 'cxx.cpp');
  fs.writeFileSync(cSrc, 'int c31_f(int x) { return x + 31; }\n');
  fs.writeFileSync(cppSrc,
    'template <typename T> T c31_t(T a) { return a * 2; }\n' +
    'int c31_g(int x) { return c31_t(x); }\n');
  const cache = path.join(dir, 'cache');
  fs.mkdirSync(cache);

  // Every compile in this check runs the IDENTICAL argv (including -v, which
  // reaches the cc1 command line and folds into the key material like any
  // other option) -- only then must the warm run's manifest key match the
  // populate run's. -v makes the driver print every command it executes, so
  // a served compile is proven spawn-free by the ABSENCE of the
  // ".../cc1 -quiet ..." line (a path token ending in /cc1 or /cc1plus).
  const vRun = (drv, s, o) => spawnSync(
    drv,
    ['-O2', '-c', s, '-o', o, '-fcompile-cache=' + cache,
     '-fintegrated-as', '-v', B],
    { cwd: dir, env: debugEnv, encoding: 'utf8' });
  const spawnedCc1 = (t) => /\/cc1\s/.test(t);
  const spawnedCc1plus = (t) => /\/cc1plus\s/.test(t);

  // (a) populate: C first, C++ SECOND -- so the legacy single sidecar (still
  // written for old drivers) ends up holding cc1plus's identity, the exact
  // aliasing that used to break the C driver tier.
  let r = vRun(XGCC, cSrc, path.join(dir, 'cf1.o'));
  if (r.status !== 0) fail('check 31: C populate failed\n' + r.stderr);
  if (!/compile-cache: store /m.test(r.stderr) ||
      !/compile-cache: manifest-store /m.test(r.stderr))
    fail('check 31: C populate did not store object + manifest\n' + r.stderr);
  if (!spawnedCc1(r.stderr))
    fail('check 31: sanity: the cold C compile must show the cc1 spawn ' +
         'under -v (spawn detector broken?)\n' + r.stderr);
  r = vRun(XGPP, cppSrc, path.join(dir, 'cxx1.o'));
  if (r.status !== 0) fail('check 31: C++ populate failed\n' + r.stderr);
  if (!/compile-cache: store /m.test(r.stderr))
    fail('check 31: C++ populate did not store\n' + r.stderr);
  if (!spawnedCc1plus(r.stderr))
    fail('check 31: sanity: the cold C++ compile must show the cc1plus ' +
         'spawn under -v\n' + r.stderr);
  for (const id of ['compiler-id', 'compiler-id-cc1', 'compiler-id-cc1plus'])
    if (!fs.existsSync(path.join(cache, id)))
      fail('check 31: missing sidecar ' + id + ' after C + C++ stores');

  // (b) warm C compile: pre-parse manifest hit AT THE DRIVER TIER -- no cc1
  // exec, even though the legacy sidecar now names cc1plus.
  r = vRun(XGCC, cSrc, path.join(dir, 'cf2.o'));
  if (r.status !== 0) fail('check 31: warm C compile failed\n' + r.stderr);
  if (!/compile-cache: manifest-hit /m.test(r.stderr))
    fail('check 31: warm C compile did not manifest-hit (aliased ' +
         'compiler-id? per-language sidecar not read?)\n' + r.stderr);
  if (spawnedCc1(r.stderr))
    fail('check 31: warm C compile still spawned cc1 (driver tier ' +
         'declined)\n' + r.stderr);

  // (c) and the C++ TU serves spawn-free at the driver tier too.
  r = vRun(XGPP, cppSrc, path.join(dir, 'cxx2.o'));
  if (r.status !== 0) fail('check 31: warm C++ compile failed\n' + r.stderr);
  if (!/compile-cache: manifest-hit /m.test(r.stderr))
    fail('check 31: warm C++ compile did not manifest-hit\n' + r.stderr);
  if (spawnedCc1plus(r.stderr))
    fail('check 31: warm C++ compile still spawned cc1plus\n' + r.stderr);

  if (!readObj(path.join(dir, 'cf1.o'))
        .equals(readObj(path.join(dir, 'cf2.o'))))
    fail('check 31: driver-served C object is not byte-identical');
  if (!readObj(path.join(dir, 'cxx1.o'))
        .equals(readObj(path.join(dir, 'cxx2.o'))))
    fail('check 31: driver-served C++ object is not byte-identical');

  process.stdout.write(
    'check 31 OK: per-language compiler-id sidecars; warm C and C++ TUs ' +
    'serve at the driver tier with zero compiler-proper execs\n');
}

// ---- check 32: ephemeral main source (cmake try_compile shape) ------------
// cmake configure probes compile the same tiny source from a freshly
// created scratch dir and DELETE the whole dir right after; the next
// configure repeats that from a NEW random scratch path.  The manifest's
// main-source record is validated by key equality -- the manifest key
// already commits the CURRENT source's bytes -- not by its stored (now
// dead) absolute path, so probe manifests keep serving at the driver tier.
// Regression shape for the zlib-ng warm-configure gap: path-validating the
// main-source record made every probe manifest permanently
// self-invalidating, so every configure re-paid a full compiler exec per
// probe (measured: 4.6 s of a 4.8 s warm leg was configure).
{
  const dir = path.join(work, 'c32');
  fs.mkdirSync(dir);
  const cache = path.join(dir, 'cache');
  const body = 'int probe_main(void) { return 42; }\n';
  const spawnedCc1 = (t) => /\/cc1\s/.test(t);

  // Cold probe from scratch dir #1, then delete the dir like cmake does.
  // -v is on BOTH probe compiles: it reaches the cc1 line and folds into
  // the key material like any other option (see check 31), so the pair must
  // agree on it -- and it is what proves the warm serve spawn-free.
  const s1 = path.join(dir, 'TryCompile-aaa');
  fs.mkdirSync(s1);
  fs.writeFileSync(path.join(s1, 'src.c'), body);
  const r1 = compile(XGCC, path.join(s1, 'src.c'), path.join(s1, 'src.o'),
                     cache, ['-v']);
  if (!/compile-cache: manifest-store /.test(r1.stderr)) {
    fail('check 32: expected manifest-store on the probe cold compile\n' + r1.stderr);
  }
  if (!spawnedCc1(r1.stderr)) {
    fail('check 32: sanity: the cold probe compile must show the cc1 spawn '
         + 'under -v (spawn detector broken?)\n' + r1.stderr);
  }
  fs.copyFileSync(path.join(s1, 'src.o'), path.join(dir, 'probe1.o'));
  fs.rmSync(s1, { recursive: true, force: true });

  // Same content from scratch dir #2: must serve spawn-free.
  const s2 = path.join(dir, 'TryCompile-bbb');
  fs.mkdirSync(s2);
  fs.writeFileSync(path.join(s2, 'src.c'), body);
  const r2 = compile(XGCC, path.join(s2, 'src.c'), path.join(s2, 'src.o'),
                     cache, ['-v']);
  if (!/compile-cache: manifest-hit /.test(r2.stderr)) {
    fail('check 32: probe recompile from a new scratch dir must manifest-hit '
         + '(dead main-source path re-validated?)\n' + r2.stderr);
  }
  if (spawnedCc1(r2.stderr)) {
    fail('check 32: probe recompile spawned cc1 (driver tier declined)\n' + r2.stderr);
  }
  if (!fs.readFileSync(path.join(s2, 'src.o'))
        .equals(fs.readFileSync(path.join(dir, 'probe1.o')))) {
    fail('check 32: probe objects are not byte-identical across scratch dirs');
  }

  // Negative control: changed probe content must MISS -- the key really
  // does commit the current source bytes, so skipping the record's
  // path-validation gives up nothing.
  const s3 = path.join(dir, 'TryCompile-ccc');
  fs.mkdirSync(s3);
  fs.writeFileSync(path.join(s3, 'src.c'),
                   'int probe_main(void) { return 43; }\n');
  const r3 = compile(XGCC, path.join(s3, 'src.c'), path.join(s3, 'src.o'), cache);
  if (/compile-cache: (manifest-hit|hit) /.test(r3.stderr)) {
    fail('check 32: changed probe content must not hit\n' + r3.stderr);
  }

  process.stdout.write(
    'check 32 OK: probe-style TUs (deleted scratch source dirs) keep serving '
    + 'spawn-free; changed content still misses\n');
}

// ---- cleanup + success ---------------------------------------------------
try {
  fs.rmSync(work, { recursive: true, force: true });
} catch {
  // best-effort cleanup; not a verification failure
}

process.stdout.write('CACHE VERIFY OK\n');
process.exit(0);
