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
function parseKeys(stderr) {
  const re = /compile-cache: (miss|store|hit) ([0-9a-f]+|-)/g;
  const out = [];
  let m;
  while ((m = re.exec(stderr)) !== null) {
    out.push({ action: m[1], key: m[2] });
  }
  return out;
}

const debugEnv = { ...process.env, GCC_COMPILE_CACHE_DEBUG: '1' };

// Compile SRC -> OBJ with -O2 -c -fcompile-cache=CACHEDIR (plus optional
// EXTRA driver args). Returns { status, stderr, keys } where keys is the
// parsed list of cache actions. EXTRA is genuinely optional.
function compile(driver, src, obj, cacheDir, extra) {
  const args = [
    '-O2', '-c', src, '-o', obj,
    '-fcompile-cache=' + cacheDir, B,
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
  fs.writeFileSync(incH, '#define VAL 43\nint helper(void);\n');
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
  // Restore VAL=42 for the functional check.
  fs.writeFileSync(incH, '#define VAL 42\nint helper(void);\n');
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

// ---- cleanup + success ---------------------------------------------------
try {
  fs.rmSync(work, { recursive: true, force: true });
} catch {
  // best-effort cleanup; not a verification failure
}

process.stdout.write('CACHE VERIFY OK\n');
process.exit(0);
