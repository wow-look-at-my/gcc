#!/usr/bin/env node
// Corpus validation for the fork's driver-level compile cache: build a real
// project three times with the installed dist compiler --
//
//   off   cache disabled            (proves the fork builds the project)
//   cold  -fcompile-cache, empty    (populates the cache; normal compiles)
//   warm  -fcompile-cache, warm     (fresh build dir; must SERVE from cache)
//
// and assert (a) all three builds succeed, (b) the project's quick test
// passes on the off and warm builds, (c) every warm object is BYTE-IDENTICAL
// to its cold counterpart, and (d) the warm build actually hit the cache
// (GCC_COMPILE_CACHE_DEBUG "manifest-hit"/"hit" line count >= a per-project
// floor) -- byte identity alone would also hold for a deterministic compiler
// that silently missed, so (d) is what proves the serve path ran.
//
// cold and warm use SIBLING build directories at the same depth. For the
// non-debug projects the cache keys are path-independent, so cross-dir warm
// hits need nothing special; the llama.cpp leg builds RelWithDebInfo (-g)
// with -ffile-prefix-map=<builddir>=. and so exercises the B2 prefix-mapped
// -g keys: DW_AT_comp_dir maps to "." in both dirs (GCC's DW_AT_producer
// deliberately omits *-prefix-map options), making cross-directory serves
// legal AND byte-identical.
//
// Usage:  node ci-corpus.mjs <openssl|zlib-ng|sqlite|fmt|llama.cpp>
// Env:
//   GCC_DIST     extracted dist root (default $RUNNER_TEMP/gcc-dist);
//                compilers are $GCC_DIST/bin/{gcc,g++}
//   CORPUS_CC / CORPUS_CXX
//                override the compiler command, possibly multi-word (local
//                testing with an uninstalled tree: ".../xgcc -B.../gcc")
//   CORPUS_WORK  scratch dir (default $RUNNER_TEMP/corpus)
//
// Node ESM, standard-library only (matches ci-verify-cache.mjs conventions).

import { spawnSync } from 'node:child_process';
import crypto from 'node:crypto';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';

const PROJECTS = ['openssl', 'zlib-ng', 'sqlite', 'fmt', 'llama.cpp'];
const project = process.argv[2];
if (!PROJECTS.includes(project)) {
  process.stderr.write(`usage: node ci-corpus.mjs <${PROJECTS.join('|')}>\n`);
  process.exit(2);
}

// Pinned upstream sources (tags verified to exist at authoring time).
const PINS = {
  openssl: { repo: 'https://github.com/openssl/openssl', ref: 'openssl-3.3.1' },
  'zlib-ng': { repo: 'https://github.com/zlib-ng/zlib-ng', ref: '2.2.1' },
  fmt: { repo: 'https://github.com/fmtlib/fmt', ref: '11.0.2' },
  'llama.cpp': { repo: 'https://github.com/ggml-org/llama.cpp', ref: 'b9891' },
  sqlite: { url: 'https://sqlite.org/2024/sqlite-amalgamation-3460100.zip', dir: 'sqlite-amalgamation-3460100' },
};

// Minimum warm cache-hit line count per project (conservative: well under
// the project's cacheable TU count -- llama.cpp b9891 has ~420 compile
// edges, openssl 3.3 ~1800, so a shortfall means the serve path broke, not
// normal drift).
const HIT_FLOOR = { openssl: 400, 'zlib-ng': 30, sqlite: 2, fmt: 20, 'llama.cpp': 300 };

const RT = process.env.RUNNER_TEMP || os.tmpdir();
const dist = process.env.GCC_DIST || path.join(RT, 'gcc-dist');
const CC = process.env.CORPUS_CC || path.join(dist, 'bin', 'gcc');
const CXX = process.env.CORPUS_CXX || path.join(dist, 'bin', 'g++');
const work = path.join(process.env.CORPUS_WORK || path.join(RT, 'corpus'), project);
const srcDir = path.join(work, 'src');
const cacheDir = path.join(work, 'cc-cache');
const logDir = path.join(work, 'logs');
const nproc = String(os.availableParallelism ? os.availableParallelism() : os.cpus().length);

fs.mkdirSync(logDir, { recursive: true });

function fail(msg) {
  process.stderr.write(`CORPUS ${project} FAILED: ${msg}\n`);
  process.exit(1);
}

// Base env for every child: pinning SOURCE_DATE_EPOCH keeps projects that
// embed a build date (openssl's buildinf.h) deterministic across the cold
// and warm configures, which the byte-identity assertion requires.
function childEnv(extra = {}) {
  return { ...process.env, CC, CXX, SOURCE_DATE_EPOCH: '1719792000', ...extra };
}

// Run a command; stdout streams through unless quietStdout pipes it. stderr
// -- and piped stdout too -- is appended to the phase log so cache-debug
// lines are countable afterwards: ninja relays each edge's captured output
// (including the compiler's stderr `compile-cache:` lines) on its OWN
// stdout, so a quiet build's hit/store lines only exist in r.stdout. Fails
// hard on nonzero exit unless allowFail.
function run(phase, argv, { cwd, env, input, allowFail = false, quietStdout = false } = {}) {
  const logFile = path.join(logDir, `${phase}.stderr.log`);
  const r = spawnSync(argv[0], argv.slice(1), {
    cwd, env: env || childEnv(), input,
    stdio: [input === undefined ? 'ignore' : 'pipe', quietStdout ? 'pipe' : 'inherit', 'pipe'],
    maxBuffer: 512 * 1024 * 1024, encoding: 'utf8',
  });
  fs.appendFileSync(logFile,
    `\n### ${argv.join(' ')} (exit ${r.status})\n${r.stderr || ''}` +
    (r.stdout ? `\n### stdout\n${r.stdout}` : ''));
  if (r.error) fail(`${argv[0]}: ${r.error.message}`);
  if (r.status !== 0 && !allowFail) {
    const tail = (r.stderr || '').split('\n').slice(-100).join('\n');
    process.stderr.write(`--- stderr tail of failing command ---\n${tail}\n`);
    fail(`[${phase}] '${argv.join(' ')}' exited ${r.status}`);
  }
  return r;
}

const splitCmd = (s) => s.split(/\s+/).filter(Boolean);

// ---------------------------------------------------------------------------
// Object collection + byte-identity
// ---------------------------------------------------------------------------

// Relative-path -> sha256 of every *.o under dir, skipping CMake's probe
// droppings (CMakeFiles/<version>/CompilerId*, CMakeScratch, CMakeTmp),
// which belong to configure, not to the project build.
function collectObjects(dir) {
  const map = new Map();
  const skip = /CMakeFiles\/[0-9]+\.[0-9]|CMakeScratch|CMakeTmp/;
  (function walk(d) {
    for (const ent of fs.readdirSync(d, { withFileTypes: true })) {
      const p = path.join(d, ent.name);
      if (ent.isSymbolicLink()) continue;
      if (ent.isDirectory()) walk(p);
      else if (ent.name.endsWith('.o')) {
        const rel = path.relative(dir, p);
        if (skip.test(rel)) continue;
        map.set(rel, crypto.createHash('sha256').update(fs.readFileSync(p)).digest('hex'));
      }
    }
  })(dir);
  return map;
}

function writeObjhash(name, map) {
  const lines = [...map.entries()].sort(([a], [b]) => a.localeCompare(b))
    .map(([rel, h]) => `${h}  ${rel}`);
  fs.writeFileSync(path.join(logDir, `objhash-${name}.txt`), lines.join('\n') + '\n');
}

function assertIdentical(cold, warm) {
  const problems = [];
  for (const [rel, h] of cold)
    if (!warm.has(rel)) problems.push(`only in cold: ${rel}`);
    else if (warm.get(rel) !== h) problems.push(`byte mismatch: ${rel}`);
  for (const rel of warm.keys())
    if (!cold.has(rel)) problems.push(`only in warm: ${rel}`);
  if (problems.length)
    fail(`cold/warm object sets differ (${problems.length} problem(s)):\n  ` +
         problems.slice(0, 20).join('\n  ') +
         (problems.length > 20 ? `\n  ... (${problems.length - 20} more)` : '') +
         `\n  full manifests: logs/objhash-{cold,warm}.txt`);
}

function countHits(phase) {
  const logFile = path.join(logDir, `${phase}.stderr.log`);
  const text = fs.existsSync(logFile) ? fs.readFileSync(logFile, 'utf8') : '';
  const count = (re) => (text.match(re) || []).length;
  return {
    manifestHit: count(/^compile-cache: manifest-hit /gm),
    hit: count(/^compile-cache: hit /gm),
    miss: count(/^compile-cache: miss /gm),
    store: count(/^compile-cache: store /gm),
  };
}

// ---------------------------------------------------------------------------
// Per-project recipes
// ---------------------------------------------------------------------------

function fetchSource() {
  if (fs.existsSync(srcDir)) return;
  const pin = PINS[project];
  if (pin.repo) {
    run('fetch', ['git', 'clone', '--depth', '1', '--branch', pin.ref, pin.repo, srcDir]);
  } else {
    fs.mkdirSync(work, { recursive: true });
    const zip = path.join(work, 'src.zip');
    run('fetch', ['curl', '-fsSL', '--retry', '4', '-o', zip, pin.url]);
    run('fetch', ['unzip', '-q', '-o', zip, '-d', work]);
    fs.renameSync(path.join(work, pin.dir), srcDir);
  }
  // b9891's webui defaults kit.version to Date.now(), making the generated
  // ui.cpp nondeterministic across builds -- pin it so byte-identity is
  // testable (same class as SOURCE_DATE_EPOCH for openssl).
  if (project === 'llama.cpp') {
    const cfg = path.join(srcDir, 'tools', 'ui', 'svelte.config.js');
    const s = fs.readFileSync(cfg, 'utf8');
    if (!/\bversion\s*:/.test(s)) {
      const p = s.replace(/(\bkit:\s*\{)/, `$1\n\t\tversion: { name: 'corpus' },`);
      if (p === s) fail('could not pin SvelteKit version in tools/ui/svelte.config.js');
      fs.writeFileSync(cfg, p);
    }
  }
}

// Extra compile flags for a build in DIR with/without the cache. llama.cpp
// builds with -g (RelWithDebInfo), so it also prefix-maps the build dir --
// the flag whose cache-key handling (B2) this leg exists to exercise.
function extraFlags(dir, cache) {
  const flags = [];
  if (project === 'llama.cpp') flags.push(`-ffile-prefix-map=${dir}=.`);
  if (cache) flags.push(`-fcompile-cache=${cacheDir}`);
  return flags;
}

// Configure + build the project in DIR. `phase` names the stderr log.
function build(phase, dir, cache) {
  fs.mkdirSync(dir, { recursive: true });
  const env = childEnv(cache ? { GCC_COMPILE_CACHE_DEBUG: '1' } : {});
  const flags = extraFlags(dir, cache);
  const cmake = (args, flagVar) =>
    run(phase, ['cmake', '-S', srcDir, '-B', dir, '-G', 'Ninja',
                ...(flags.length ? [`-D${flagVar}=${flags.join(' ')}`] : []), ...args],
        { env, quietStdout: true });

  switch (project) {
    case 'openssl':
      // Small config: static-only, no tests/docs. Unrecognized dash-args are
      // appended to CFLAGS by Configure. no-asm matters: when asm is enabled
      // Configure appends -Wa,--noexecstack to the compile flags, and any
      // -Wa, option triggers the fork's external-as fallback, which disables
      // the compile cache by design -- so the leg would silently validate
      // nothing. no-asm keeps every TU on the integrated assembler and thus
      // cache-covered. (Follow-up idea: teach libgas to accept --noexecstack
      // so asm-enabled openssl builds become cacheable too.)
      run(phase, ['perl', path.join(srcDir, 'Configure'), 'linux-x86_64',
                  'no-asm', 'no-shared', 'no-tests', 'no-docs', ...flags],
          { cwd: dir, env, quietStdout: true });
      run(phase, ['make', '-s', `-j${nproc}`], { cwd: dir, env, quietStdout: true });
      break;
    case 'zlib-ng':
      // Compat API, tests off (gtest would be fetched from the network);
      // the quick test below is a hermetic compress/uncompress roundtrip.
      cmake(['-DCMAKE_BUILD_TYPE=Release', '-DZLIB_COMPAT=ON',
             '-DZLIB_ENABLE_TESTS=OFF', '-DZLIBNG_ENABLE_TESTS=OFF', '-DWITH_GTEST=OFF'],
            'CMAKE_C_FLAGS');
      run(phase, ['cmake', '--build', dir, '-j', nproc], { env, quietStdout: true });
      break;
    case 'sqlite': {
      // The amalgamation: one huge TU + the shell, linked into sqlite3.
      const cc = [...splitCmd(CC), '-O2', ...flags];
      run(phase, [...cc, '-c', path.join(srcDir, 'sqlite3.c'), '-o', path.join(dir, 'sqlite3.o')], { env });
      run(phase, [...cc, '-I', srcDir, '-c', path.join(srcDir, 'shell.c'), '-o', path.join(dir, 'shell.o')], { env });
      run(phase, [...splitCmd(CC), path.join(dir, 'sqlite3.o'), path.join(dir, 'shell.o'),
                  '-o', path.join(dir, 'sqlite3'), '-lm', '-lpthread', '-ldl'], { env });
      break;
    }
    case 'fmt':
      cmake(['-DCMAKE_BUILD_TYPE=Release', '-DFMT_TEST=ON', '-DFMT_DOC=OFF'], 'CMAKE_CXX_FLAGS');
      run(phase, ['cmake', '--build', dir, '-j', nproc], { env, quietStdout: true });
      break;
    case 'llama.cpp':
      // RelWithDebInfo (-O2 -g) exercises the prefix-mapped -g keys; CURL
      // off (no libcurl-dev on the runner), ccache off (GGML_CCACHE=OFF --
      // the fork's cache is the one under test).
      run(phase, ['cmake', '-S', srcDir, '-B', dir, '-G', 'Ninja',
                  '-DCMAKE_BUILD_TYPE=RelWithDebInfo', '-DLLAMA_CURL=OFF', '-DGGML_CCACHE=OFF',
                  `-DCMAKE_C_FLAGS=${flags.join(' ')}`, `-DCMAKE_CXX_FLAGS=${flags.join(' ')}`],
          { env, quietStdout: true });
      run(phase, ['cmake', '--build', dir, '-j', nproc], { env, quietStdout: true });
      break;
  }
}

// Cheap end-to-end check that the build's artifacts actually work; run on
// the off and warm builds (warm = cache-served objects linked and executed).
function quickTest(phase, dir) {
  switch (project) {
    case 'openssl': {
      const r = run(phase, [path.join(dir, 'apps', 'openssl'), 'version'], { quietStdout: true });
      if (!/OpenSSL 3\.3\.1/.test(r.stdout || '')) fail(`[${phase}] unexpected 'openssl version': ${r.stdout}`);
      process.stdout.write(`[${phase}] ${String(r.stdout).trim()}\n`);
      break;
    }
    case 'zlib-ng': {
      const prog = path.join(dir, 'roundtrip.c');
      fs.writeFileSync(prog, `
#include <stdio.h>
#include <string.h>
#include <zlib.h>
int main(void) {
  const char *msg = "corpus-roundtrip-payload corpus-roundtrip-payload";
  unsigned char comp[512], decomp[512];
  uLongf clen = sizeof comp, dlen = sizeof decomp;
  if (compress2(comp, &clen, (const unsigned char *)msg, strlen(msg) + 1, 9) != Z_OK) return 1;
  if (uncompress(decomp, &dlen, comp, clen) != Z_OK) return 2;
  if (strcmp((const char *)decomp, msg) != 0) return 3;
  printf("zlib %s roundtrip ok\\n", zlibVersion());
  return 0;
}
`);
      const lib = ['libz.a', 'libz-ng.a'].map((n) => path.join(dir, n)).find(fs.existsSync);
      const args = lib ? [lib] : ['-L' + dir, '-lz', `-Wl,-rpath,${dir}`];
      run(phase, [...splitCmd(CC), prog, '-I', dir, '-I', srcDir, '-o', path.join(dir, 'roundtrip'), ...args]);
      run(phase, [path.join(dir, 'roundtrip')]);
      break;
    }
    case 'sqlite': {
      const r = run(phase, [path.join(dir, 'sqlite3')],
                    { input: 'CREATE TABLE t(a INT); INSERT INTO t VALUES(1),(2),(41); SELECT sum(a) FROM t;\n', quietStdout: true });
      if (String(r.stdout).trim() !== '44') fail(`[${phase}] sqlite quick check: expected 44, got '${String(r.stdout).trim()}'`);
      process.stdout.write(`[${phase}] sqlite quick check ok (sum=44)\n`);
      break;
    }
    case 'fmt':
      // --no-tests=error: a misconfigured build discovering zero tests must
      // fail the leg, not vacuously pass it.
      run(phase, ['ctest', '--test-dir', dir, '--no-tests=error',
                  '--output-on-failure', '--timeout', '120', '-j', nproc],
          { quietStdout: true });
      process.stdout.write(`[${phase}] fmt ctest suite ok\n`);
      break;
    case 'llama.cpp':
      run(phase, [path.join(dir, 'bin', 'llama-cli'), '--version'], { quietStdout: true });
      process.stdout.write(`[${phase}] llama-cli --version ok\n`);
      break;
  }
}

// ---------------------------------------------------------------------------
// The off -> cold -> warm sequence
// ---------------------------------------------------------------------------

run('setup', [...splitCmd(CC), '--version'], { quietStdout: true });
fetchSource();
fs.mkdirSync(cacheDir, { recursive: true });

const phases = [
  { name: 'off', cache: false },
  { name: 'cold', cache: true },
  { name: 'warm', cache: true },
];
const timings = {};
const objects = {};
for (const { name, cache } of phases) {
  const dir = path.join(work, `build-${name}`);
  const t0 = Date.now();
  build(name, dir, cache);
  timings[name] = ((Date.now() - t0) / 1000).toFixed(1);
  if (cache) {
    objects[name] = collectObjects(dir);
    writeObjhash(name, objects[name]);
  }
  if (name !== 'cold') quickTest(name, dir);
  process.stdout.write(`[${name}] build ok in ${timings[name]}s` +
    (cache ? `, ${objects[name].size} objects` : '') + '\n');
}

if (objects.cold.size === 0) fail('cold build produced no object files (collection bug?)');
assertIdentical(objects.cold, objects.warm);

const cold = countHits('cold');
const warm = countHits('warm');
const warmHits = warm.manifestHit + warm.hit;
const floor = HIT_FLOOR[project];
process.stdout.write(
  `cache lines -- cold: ${JSON.stringify(cold)}\n` +
  `cache lines -- warm: ${JSON.stringify(warm)}\n`);
if (cold.store === 0) fail('cold build stored nothing in the cache');
if (warmHits < floor)
  fail(`warm build hit the cache ${warmHits} time(s), floor is ${floor} -- ` +
       `the serve path regressed (byte identity alone cannot catch a silent miss)`);

const summary =
  `## corpus: ${project}\n\n` +
  `| phase | wall (s) | objects | manifest-hit | hit | miss | store |\n|---|---:|---:|---:|---:|---:|---:|\n` +
  `| off | ${timings.off} | - | - | - | - | - |\n` +
  `| cold | ${timings.cold} | ${objects.cold.size} | ${cold.manifestHit} | ${cold.hit} | ${cold.miss} | ${cold.store} |\n` +
  `| warm | ${timings.warm} | ${objects.warm.size} | ${warm.manifestHit} | ${warm.hit} | ${warm.miss} | ${warm.store} |\n\n` +
  `${objects.warm.size} warm objects byte-identical to cold; ` +
  `${warmHits} warm cache hits (floor ${floor}).\n`;
if (process.env.GITHUB_STEP_SUMMARY) fs.appendFileSync(process.env.GITHUB_STEP_SUMMARY, summary);
process.stdout.write('\n' + summary + `\nCORPUS ${project} OK\n`);
