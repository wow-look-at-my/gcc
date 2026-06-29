#!/usr/bin/env node
// Benchmark the in-compiler compile cache of a freshly built GCC tree.
//
// Usage:  node ci-bench-cache.mjs <gcc-build-dir>
//         GCC_BUILD_DIR=<dir> node ci-bench-cache.mjs
//
// Drives the freshly built xg++ over a small fixed C++ workload several ways and
// reports wall-clock medians plus the headline ratios (warm-vs-base speedup and
// cold-vs-base overhead) for our in-compiler cache and, when available, for
// ccache wrapping the SAME xg++ as a comparison baseline:
//
//   T_base       - compile the batch with NO -fcompile-cache (default flags)
//   T_cold       - our cache (-fcompile-cache -fintegrated-as), fresh empty dir,
//                  compile the batch (all miss + store)
//   T_warm       - our cache, compile again against the now-full dir (all hits;
//                  each served pre-parse, logged as "manifest-hit")
//   T_ccache_cold - ccache wrapping xg++ (no -fcompile-cache), fresh empty
//                   CCACHE_DIR after `ccache -C -z` (all miss)
//   T_ccache_warm - ccache wrapping xg++, compile again against the populated
//                   CCACHE_DIR (all hits)
//
// The ccache rows are skipped gracefully (not a failure) when `ccache` is not on
// PATH. Results are written to the log and, when set, appended to the file named
// by $GITHUB_STEP_SUMMARY (Markdown) so they show up in the run UI. The workload
// is deliberately small (a handful of heavy-STL TUs) so CI stays reasonable.
//
// Node ESM, standard-library only (no external deps). Exits 0 on success, or
// non-zero with a clear message if a compile fails or hit/miss counts are
// wrong (which would indicate the cache is broken).

import { spawnSync } from 'node:child_process';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';

function usage() {
  process.stderr.write(
    'usage: node ci-bench-cache.mjs <gcc-build-dir>\n' +
    '       (or set GCC_BUILD_DIR)\n'
  );
  process.exit(2);
}

const buildDir = process.argv[2] || process.env.GCC_BUILD_DIR;
if (!buildDir) usage();

const gccDir = path.join(buildDir, 'gcc');
const XGPP = path.join(gccDir, 'xg++');
const B = '-B' + gccDir;

if (!fs.existsSync(XGPP)) {
  process.stderr.write('error: driver not found: ' + XGPP + '\n');
  process.exit(2);
}

function fail(msg) {
  process.stderr.write('CACHE BENCH FAILED: ' + msg + '\n');
  process.exit(1);
}

// Locate the ccache binary on PATH (used for a comparison baseline). Returns
// the absolute path, or null if ccache isn't installed -- in which case the
// ccache rows are skipped gracefully rather than failing the benchmark.
function findCcache() {
  const probe = spawnSync('ccache', ['--version'], { encoding: 'utf8' });
  if (probe.error || probe.status !== 0) return null;
  const which = spawnSync('command', ['-v', 'ccache'], {
    shell: true,
    encoding: 'utf8',
  });
  const p = (which.stdout || '').trim();
  return p || 'ccache';
}

const CCACHE = findCcache();
const haveCcache = CCACHE !== null;

// An uninstalled in-tree xg++ does not know where its own freshly built
// libstdc++ headers are, so a TU that #includes <vector> fails with
// "fatal error: vector: No such file or directory" unless we point it at the
// build-tree headers. Discover the include dirs the libstdc++ build itself
// uses: the generic <build>/<target>/libstdc++-v3/include, its target
// subdir, and the libsupc++ source dir (which holds <new>, <typeinfo>, ...).
// Returns an array of -nostdinc++/-I args usable for compiling STL code, or
// null if the dirs can't be found (caller then uses a self-contained
// workload). Robust to the exact target triple via a glob.
function discoverStdcxxIncludes() {
  let genericInc = null;
  const direct = path.join(
    buildDir, 'x86_64-pc-linux-gnu', 'libstdc++-v3', 'include'
  );
  if (fs.existsSync(path.join(direct, 'vector'))) {
    genericInc = direct;
  } else {
    let hits = [];
    try {
      hits = fs.globSync(
        path.join(buildDir, '*', 'libstdc++-v3', 'include', 'vector')
      );
    } catch {
      hits = [];
    }
    if (hits.length) genericInc = path.dirname(hits[0]);
  }
  if (!genericInc) return null;

  // The target subdir under the generic include holds bits/c++config.h.
  let targetInc = null;
  try {
    const ents = fs.readdirSync(genericInc, { withFileTypes: true });
    for (const e of ents) {
      if (
        e.isDirectory() &&
        fs.existsSync(path.join(genericInc, e.name, 'bits', 'c++config.h'))
      ) {
        targetInc = path.join(genericInc, e.name);
        break;
      }
    }
  } catch {
    /* ignore */
  }
  if (!targetInc) return null;

  // libsupc++ lives in the *source* tree, not the build tree. Prefer
  // $GITHUB_WORKSPACE (where the checkout is), then look relative to common
  // build layouts; require it to actually contain <new>.
  const supcCandidates = [];
  if (process.env.GITHUB_WORKSPACE) {
    supcCandidates.push(
      path.join(process.env.GITHUB_WORKSPACE, 'libstdc++-v3', 'libsupc++')
    );
  }
  // Sometimes the build dir sits beside or under the source tree.
  supcCandidates.push(path.join(buildDir, '..', 'libstdc++-v3', 'libsupc++'));
  let supcInc = null;
  for (const c of supcCandidates) {
    if (fs.existsSync(path.join(c, 'new'))) {
      supcInc = c;
      break;
    }
  }
  if (!supcInc) return null;

  return ['-nostdinc++', '-I' + targetInc, '-I' + genericInc, '-I' + supcInc];
}

const stdcxxIncludes = discoverStdcxxIncludes();
const useStl = stdcxxIncludes !== null;

// Number of translation units in the workload. Each is a distinct, heavy-STL
// C++ TU (a macro is varied per TU so the cache keys differ). Kept modest so
// CI wall-time stays reasonable.
const TU_COUNT = Number(process.env.CC_BENCH_TUS || '12');
// How many timed repetitions per measurement; the median is reported.
const REPS = Number(process.env.CC_BENCH_REPS || '3');

const work = fs.mkdtempSync(path.join(os.tmpdir(), 'cc-bench-'));
const srcDir = path.join(work, 'src');
const objDir = path.join(work, 'obj');
fs.mkdirSync(srcDir);
fs.mkdirSync(objDir);

// Generate TU_COUNT distinct heavy-STL C++ source files. Each pulls in several
// standard headers and instantiates a moderately heavy template plus a few
// functions; UNIT is unique per file so every key is distinct.
const STL_TEMPLATE = (n) => `
#include <vector>
#include <map>
#include <string>
#include <algorithm>
#include <memory>
#include <functional>

#define UNIT ${n}

namespace bench_${n} {

template <typename K, typename V>
struct Table {
  std::map<K, V> m;
  std::vector<std::pair<K, V>> log;
  void put(const K& k, const V& v) { m[k] = v; log.emplace_back(k, v); }
  V get(const K& k) const {
    auto it = m.find(k);
    return it == m.end() ? V{} : it->second;
  }
  std::size_t size() const { return m.size(); }
};

template <typename T>
T accumulate_sorted(std::vector<T> xs) {
  std::sort(xs.begin(), xs.end());
  T acc{};
  for (const auto& x : xs) acc = acc + x;
  return acc;
}

inline int build(int seed) {
  Table<std::string, int> t;
  std::vector<int> v;
  for (int i = 0; i < 64 + (UNIT % 7); ++i) {
    t.put("k" + std::to_string(i + seed), i * (UNIT + 1));
    v.push_back(i ^ seed);
  }
  std::function<int(int)> f = [&](int x) { return x + t.get("k0"); };
  auto sp = std::make_shared<std::vector<int>>(v);
  return f(accumulate_sorted(*sp)) + static_cast<int>(t.size());
}

} // namespace bench_${n}

int entry_${n}() { return bench_${n}::build(UNIT); }
`;

// Self-contained fallback used when the build-tree libstdc++ headers can't be
// located. No standard-library includes, but still moderately heavy on the
// front end + optimizer at -O2 (deep template recursion plus several
// functions). UNIT is unique per file so every key differs.
const SELF_TEMPLATE = (n) => `
#define UNIT ${n}

namespace bench_${n} {

template <int N> struct Fib { enum { value = Fib<N-1>::value + Fib<N-2>::value }; };
template <> struct Fib<0> { enum { value = 0 }; };
template <> struct Fib<1> { enum { value = 1 }; };

template <typename T, int N>
struct Poly {
  T c[N];
  T eval(T x) const {
    T acc = T();
    for (int i = N - 1; i >= 0; --i) acc = acc * x + c[i];
    return acc;
  }
};

template <typename T>
T accumulate(const T* xs, int n) {
  T acc = T();
  for (int i = 0; i < n; ++i) acc = acc + xs[i] * (i ^ (UNIT + 1));
  return acc;
}

template <typename T>
struct Matrix {
  T a[8][8];
  void identity() {
    for (int i = 0; i < 8; ++i)
      for (int j = 0; j < 8; ++j) a[i][j] = (i == j) ? T(1) : T();
  }
  Matrix mul(const Matrix& o) const {
    Matrix r;
    for (int i = 0; i < 8; ++i)
      for (int j = 0; j < 8; ++j) {
        T s = T();
        for (int k = 0; k < 8; ++k) s = s + a[i][k] * o.a[k][j];
        r.a[i][j] = s;
      }
    return r;
  }
};

inline long build(long seed) {
  long data[64];
  for (int i = 0; i < 64; ++i) data[i] = (i + seed) * (UNIT + 3);
  Poly<long, 6> p{ {1, 2, 3, 4, 5, 6} };
  Matrix<long> m, n;
  m.identity();
  n.identity();
  Matrix<long> r = m.mul(n);
  return accumulate(data, 64) + p.eval(seed) + Fib<20>::value + r.a[3][3];
}

} // namespace bench_${n}

long entry_${n}() { return bench_${n}::build(UNIT); }
`;

const SRC_TEMPLATE = useStl ? STL_TEMPLATE : SELF_TEMPLATE;

const files = [];
for (let i = 0; i < TU_COUNT; i++) {
  const p = path.join(srcDir, `tu_${i}.cc`);
  fs.writeFileSync(p, SRC_TEMPLATE(i));
  files.push(p);
}

const debugEnv = { ...process.env, GCC_COMPILE_CACHE_DEBUG: '1' };

// Compile ONE TU at -O2. cacheDir==null -> no -fcompile-cache (the no-cache
// baseline). captureDebug turns on the debug env so the returned stderr
// carries the cache action lines. Returns { stderr }.
//
// When a cacheDir is given we ALSO pass -fintegrated-as: Stage 5 caches and
// serves the in-process (integrated-as) assembled object, and gates the cache
// off entirely without it (compile_cache_enabled_p() -> false, so NO
// miss/store/hit lines are emitted and nothing is ever stored). It is NOT the
// default on this target, so every cache-exercising compile must request it
// explicitly or the cache stays disabled -- which is exactly why the warm pass
// would otherwise see zero hits. The null-cache baseline (T_base) is left at
// the compiler's default flags on purpose, so it measures a real plain build.
function compileOne(src, cacheDir, captureDebug) {
  const b = path.basename(src, '.cc');
  const args = ['-O2', '-c', src, '-o', path.join(objDir, b + '.o')];
  if (useStl) args.push(...stdcxxIncludes);
  args.push(B);
  if (cacheDir) {
    args.splice(
      args.length - 1, 0,
      '-fcompile-cache=' + cacheDir, '-fintegrated-as'
    );
  }
  const res = spawnSync(XGPP, args, {
    env: captureDebug ? debugEnv : process.env,
    encoding: 'utf8',
  });
  if (res.error) fail('failed to spawn xg++: ' + res.error.message);
  if (res.status !== 0) {
    fail(
      'compile failed: exit ' + res.status + '\n--- args ---\n' +
      args.join(' ') + '\n--- stderr ---\n' + (res.stderr || '')
    );
  }
  return { stderr: res.stderr || '' };
}

// Compile the whole batch once. Returns elapsed seconds (wall clock).
function compileBatch(cacheDir) {
  const start = process.hrtime.bigint();
  for (const f of files) compileOne(f, cacheDir, false);
  const end = process.hrtime.bigint();
  return Number(end - start) / 1e9;
}

// Compile ONE TU at -O2 via `ccache xg++ ...` (no -fcompile-cache; ccache is
// the cache here). CCACHE_DIR points ccache at an isolated dir so it doesn't
// touch the user's real cache. Returns nothing; fails on a compile error.
function compileOneCcache(src, cacheDir) {
  const b = path.basename(src, '.cc');
  const args = [XGPP, '-O2', '-c', src, '-o', path.join(objDir, b + '.o')];
  if (useStl) args.push(...stdcxxIncludes);
  args.push(B);
  const res = spawnSync(CCACHE, args, {
    env: { ...process.env, CCACHE_DIR: cacheDir },
    encoding: 'utf8',
  });
  if (res.error) fail('failed to spawn ccache: ' + res.error.message);
  if (res.status !== 0) {
    fail(
      'ccache compile failed: exit ' + res.status + '\n--- args ---\n' +
      CCACHE + ' ' + args.join(' ') + '\n--- stderr ---\n' + (res.stderr || '')
    );
  }
}

// Compile the whole batch once via ccache. Returns elapsed seconds.
function compileBatchCcache(cacheDir) {
  const start = process.hrtime.bigint();
  for (const f of files) compileOneCcache(f, cacheDir);
  const end = process.hrtime.bigint();
  return Number(end - start) / 1e9;
}

// Read ccache's hit/miss counters for a given CCACHE_DIR. Uses the stable
// machine-readable `ccache --print-stats` (key<TAB>value lines). Returns
// { hit, miss } summing direct+preprocessed hits; missing keys count as 0.
function ccacheStats(cacheDir) {
  const res = spawnSync(CCACHE, ['--print-stats'], {
    env: { ...process.env, CCACHE_DIR: cacheDir },
    encoding: 'utf8',
  });
  const out = res.stdout || '';
  const get = (key) => {
    const m = out.match(new RegExp('^' + key + '\\t(\\d+)', 'm'));
    return m ? Number(m[1]) : 0;
  };
  const hit = get('direct_cache_hit') + get('preprocessed_cache_hit');
  const miss = get('cache_miss');
  return { hit, miss };
}

// Reset a CCACHE_DIR to empty and zero its stats (`-C` clears, `-z` zeroes).
function ccacheReset(cacheDir) {
  spawnSync(CCACHE, ['-C', '-z'], {
    env: { ...process.env, CCACHE_DIR: cacheDir },
    encoding: 'utf8',
  });
}

// Zero a CCACHE_DIR's stats only (`-z`) without clearing cached objects, so the
// next pass against an already-populated dir registers as hits.
function ccacheZeroStats(cacheDir) {
  spawnSync(CCACHE, ['-z'], {
    env: { ...process.env, CCACHE_DIR: cacheDir },
    encoding: 'utf8',
  });
}

function median(xs) {
  const s = [...xs].sort((a, b) => a - b);
  const mid = Math.floor(s.length / 2);
  return s.length % 2 ? s[mid] : (s[mid - 1] + s[mid]) / 2;
}

// Count "compile-cache: <action> ..." debug lines for a given action
// (miss/store/hit), with the Stage 5 normalization the verify script uses:
//
//   * A warm compile serves the in-process-assembled object BEFORE parse via a
//     ccache-style direct-mode manifest, so it logs "manifest-hit" (the
//     pre-parse object serve) instead of the post-parse "hit". Both carry the
//     same content key and place a byte-identical object, so for accounting a
//     "manifest-hit" IS a hit -- fold it in when counting 'hit'.
//   * The manifest LAYER also logs "manifest-miss" / "manifest-store" (keyed on
//     the separate manifest key) on a cold compile, ALONGSIDE the real
//     content-level "miss" / "store". Those must NOT be counted as miss/store
//     here -- the single anchored regex below matches the bare action token
//     right after "compile-cache: ", so "manifest-miss"/"manifest-store" are
//     left unmatched (only "manifest-hit" is explicitly folded into 'hit').
function countAction(stderr, action) {
  const re = /compile-cache: (manifest-hit|miss|store|hit) (?:[0-9a-f]+|-)/g;
  let n = 0;
  let m;
  while ((m = re.exec(stderr)) !== null) {
    const folded = m[1] === 'manifest-hit' ? 'hit' : m[1];
    if (folded === action) n++;
  }
  return n;
}

const workloadDesc = useStl
  ? 'heavy-STL C++ TUs (<vector>/<map>/<string>/<algorithm>/<memory>/<functional>)'
  : 'heavy-template C++ TUs (self-contained; build-tree libstdc++ headers not found)';

process.stdout.write(
  `Compile-cache benchmark: ${TU_COUNT} ${workloadDesc}, -O2, serial, ` +
  `median of ${REPS} reps\n`
);

// Warm the filesystem / page cache first (throwaway, untimed).
compileBatch(null);

// T_base: no cache.
const baseTimes = [];
for (let r = 0; r < REPS; r++) baseTimes.push(compileBatch(null));
const tBase = median(baseTimes);

// T_cold: fresh empty cache each rep -> all miss + store.
const coldCache = path.join(work, 'cold');
const coldTimes = [];
for (let r = 0; r < REPS; r++) {
  fs.rmSync(coldCache, { recursive: true, force: true });
  fs.mkdirSync(coldCache);
  coldTimes.push(compileBatch(coldCache));
}
const tCold = median(coldTimes);

// T_warm: a populated cache -> all hits. Populate once, then time.
const warmCache = path.join(work, 'warm');
fs.mkdirSync(warmCache);
compileBatch(warmCache); // populate (miss + store)
const warmTimes = [];
for (let r = 0; r < REPS; r++) warmTimes.push(compileBatch(warmCache));
const tWarm = median(warmTimes);

// ccache comparison rows (only if ccache is on PATH). Wrap the SAME xg++ with
// ccache (no -fcompile-cache) and measure cold (fresh dir, cleared+zeroed each
// rep -> all miss) and warm (populated dir -> all hits), same workload/flags.
let tCcacheCold = null, tCcacheWarm = null;
let ccacheColdHit = 0, ccacheColdMiss = 0;
let ccacheWarmHit = 0, ccacheWarmMiss = 0;
if (haveCcache) {
  const ccacheCacheDir = path.join(work, 'ccache');
  fs.mkdirSync(ccacheCacheDir);

  // T_ccache_cold: clear+zero before each timed rep so every TU is a miss.
  const ccacheColdTimes = [];
  for (let r = 0; r < REPS; r++) {
    ccacheReset(ccacheCacheDir);
    ccacheColdTimes.push(compileBatchCcache(ccacheCacheDir));
  }
  tCcacheCold = median(ccacheColdTimes);

  // T_ccache_warm: the dir is already populated from the last cold rep -> all
  // hits. Time without clearing.
  const ccacheWarmTimes = [];
  for (let r = 0; r < REPS; r++) ccacheWarmTimes.push(compileBatchCcache(ccacheCacheDir));
  tCcacheWarm = median(ccacheWarmTimes);

  // Hit/miss accounting on isolated fresh dirs (one cold pass, one warm pass).
  const ccacheAcct = path.join(work, 'ccache-acct');
  fs.mkdirSync(ccacheAcct);
  ccacheReset(ccacheAcct); // clear + zero -> truly cold
  compileBatchCcache(ccacheAcct); // cold: all miss (populates the dir)
  ({ hit: ccacheColdHit, miss: ccacheColdMiss } = ccacheStats(ccacheAcct));
  ccacheZeroStats(ccacheAcct); // zero stats only; populated dir stays warm
  compileBatchCcache(ccacheAcct); // warm: all hits
  ({ hit: ccacheWarmHit, miss: ccacheWarmMiss } = ccacheStats(ccacheAcct));
}

// Hit/miss accounting via the debug lines, on a separate fresh cache.
const acctCache = path.join(work, 'acct');
fs.mkdirSync(acctCache);
let coldMiss = 0, coldStore = 0, coldHit = 0;
for (const f of files) {
  const { stderr } = compileOne(f, acctCache, true);
  coldMiss += countAction(stderr, 'miss');
  coldStore += countAction(stderr, 'store');
  coldHit += countAction(stderr, 'hit');
}
let warmMiss = 0, warmStore = 0, warmHit = 0;
for (const f of files) {
  const { stderr } = compileOne(f, acctCache, true);
  warmMiss += countAction(stderr, 'miss');
  warmStore += countAction(stderr, 'store');
  warmHit += countAction(stderr, 'hit');
}

const speedup = tBase / tWarm;
const overheadPct = ((tCold - tBase) / tBase) * 100;
const ccacheSpeedup = haveCcache ? tBase / tCcacheWarm : null;
const ccacheOverheadPct = haveCcache ? ((tCcacheCold - tBase) / tBase) * 100 : null;
const f2 = (x) => x.toFixed(2);

// Sanity: the cache must actually behave. A broken cache (0 hits warm, or
// hits on the cold pass) should fail CI rather than report meaningless numbers.
if (coldHit !== 0) fail(`cold pass had ${coldHit} unexpected hit(s)`);
if (warmHit !== TU_COUNT) {
  fail(`warm pass had ${warmHit} hit(s), expected ${TU_COUNT}`);
}

const tableRows = [
  '| Measurement | Wall-clock (s) |',
  '| --- | --- |',
  `| T_base (no cache) | ${f2(tBase)} |`,
  `| T_cold (ours, empty cache: all miss+store) | ${f2(tCold)} |`,
  `| T_warm (ours, full cache: all hits) | ${f2(tWarm)} |`,
];
if (haveCcache) {
  tableRows.push(
    `| T_ccache_cold (ccache, empty cache: all miss) | ${f2(tCcacheCold)} |`,
    `| T_ccache_warm (ccache, full cache: all hits) | ${f2(tCcacheWarm)} |`
  );
}

const lines = [
  '### Compile-cache benchmark',
  '',
  `Workload: **${TU_COUNT}** ${workloadDesc}, \`-O2\`, serial, median of ${REPS} reps.`,
  '',
  ...tableRows,
  '',
  `**Ours warm-vs-base speedup:** ${f2(speedup)}x  (\`T_base / T_warm\`)`,
  '',
  `**Ours cold-vs-base overhead:** ${overheadPct.toFixed(1)}%  (\`(T_cold - T_base) / T_base\`)`,
  '',
];
if (haveCcache) {
  lines.push(
    `**ccache warm-vs-base speedup:** ${f2(ccacheSpeedup)}x  (\`T_base / T_ccache_warm\`)`,
    '',
    `**ccache cold-vs-base overhead:** ${ccacheOverheadPct.toFixed(1)}%  (\`(T_ccache_cold - T_base) / T_base\`)`,
    ''
  );
} else {
  lines.push('_ccache not found on PATH — ccache rows skipped._', '');
}
lines.push(
  `Hit/miss accounting — ours cold: ${coldMiss} miss, ${coldStore} store, ${coldHit} hit; ` +
  `ours warm: ${warmMiss} miss, ${warmStore} store, ${warmHit} hit.`,
  ''
);
if (haveCcache) {
  lines.push(
    `ccache accounting — cold: ${ccacheColdMiss} miss, ${ccacheColdHit} hit; ` +
    `warm: ${ccacheWarmMiss} miss, ${ccacheWarmHit} hit.`,
    ''
  );
}
const summary = lines.join('\n') + '\n';

// Always log it.
process.stdout.write('\n' + summary);

// Append to the GitHub step summary if available.
const stepSummary = process.env.GITHUB_STEP_SUMMARY;
if (stepSummary) {
  try {
    fs.appendFileSync(stepSummary, summary);
    process.stdout.write('(wrote benchmark to $GITHUB_STEP_SUMMARY)\n');
  } catch (e) {
    process.stdout.write('(could not write $GITHUB_STEP_SUMMARY: ' + e.message + ')\n');
  }
}

// Best-effort cleanup.
try {
  fs.rmSync(work, { recursive: true, force: true });
} catch {
  // not a failure
}

process.stdout.write('CACHE BENCH OK\n');
process.exit(0);
