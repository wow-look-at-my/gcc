# perf-lab -- benchmark rig (GitHub-hosted runners, dispatch-only)

Compile-time benchmarking for this fork's performance work (include-path dir
index, GGC 512M cap + `MADV_HUGEPAGE`, static tcmalloc, readlink skip, cache
hash-on-read; merged as PR #13 / `f7a3fd96a`). The integration sandbox those
wins were first measured in could not express the THP win at all
(`thp_fault_alloc=0` system-wide), so the headline MADV_HUGEPAGE number --
minor faults -92%, wall -10.6% on the original hugetlb-proxy measurement --
needs a THP-capable host to reproduce. The `census` suite reports whether the
hosted runner environment is one (its THP mode is the first datum every run
collects).

Runs on GitHub-hosted runners (`ubuntu-latest`) via
`.github/workflows/perf-lab.yml`, and only on manual `workflow_dispatch` --
there is no push trigger, and no job targets self-hosted hardware. Runs may
execute **concurrently** by design: each run's jobs get their own isolated
hosted VMs, so parallel runs cannot perturb each other, and no run ever
cancels another.

## Pipeline

```
census -> build (matrix: base?/tip) -> shards (matrix: one cell each) -> aggregate
```

- **build** compiles the tip compiler (and, for the A/B suites, the pinned
  pre-optimization base) in parallel matrix legs, using the exact ci.yml
  combined-tree recipe, then packages the minimal runnable subset with
  `pack-compiler.sh` and uploads it as artifact `compiler-{base,tip}`
  (zstd tar, roughly 100-150 MB compressed / ~370 MB unpacked per side).
- **shards** fans the suite out to one job per measurement cell. Each shard
  downloads the compiler artifact(s), regenerates its bench inputs
  deterministically, and runs the complete interleaved A/B protocol for its
  one cell on one pinned core -- compiles are never co-run; the matrix
  supplies all parallelism. Each shard uploads a `shard-<cell>` artifact
  (summary row + raw `/usr/bin/time -v` outputs + its extras) and writes its
  own row to the run's step summary.
- **aggregate** merges all shard rows into the final table
  (cell | base | tip | ratio | spread | verdict), checks every confident
  result against the expected direction of the merged wins (big/huge/stl/
  realism cells: tip faster; light-O0: control, no change; pch cells: PCH
  speedup; realism: tip syscall drop), flags contradictions as SURPRISE, and
  publishes the one-stop `perf-lab-results` artifact.

Wall-time budget on hosted runners: census ~1 min, the two build legs run in
parallel (~13 min compile + ~1 min packaging each, measured), then every cell
measures simultaneously, so a suite costs roughly `build + slowest shard`
(huge-O2, median of 5 per side, is the long pole) + ~1 min aggregate --
~30-35 min for `composed`, and `full` costs about the same as `composed`
because the pch shards ride the same fan-out instead of extending a serial
tail. The old single sequential job measured 43m24s for `composed` (two
serial ~12.5-min builds + 16m54s suite, run 28703247565) and was budgeted up
to 4-5 h/350-min timeout back when a hosted build was estimated at ~1 h; the
sharded shape also isolates cell failures (`fail-fast: false`) and gives
every cell its own 60-min timeout.

## One-click usage

Actions -> perf-lab -> Run workflow:

| input | meaning |
|---|---|
| `suite` | `census` (environment probe only), `quick` (tip-only stl-O2 + big-O2 shards), `composed` (base-vs-tip A/B: light-O0, stl-O0, stl-O2, big-O2, realism-O0, huge-O2, huge-O2-never), `pch` (PCH ceiling: pch-stl-O0/O0g/O2, pch-json-O0/O2, pch-big-O2, pch-anchor-O0, tip only), `full` (composed + pch shards) |
| `bench_ref` | ref whose compiler is benchmarked ("tip"), default `develop-matt/v14` |

The composed base is pinned to `5226232bb` (the PR #13 merge base) in the
workflow env. Every dispatch first runs the near-instant `census` job -- a
probe of what the runner environment actually is (toolchain, THP mode,
disk) -- and, unless `suite=census`, then the build/shards/aggregate chain.

Results land in three places: the run's `$GITHUB_STEP_SUMMARY` (one section
per shard plus the aggregate table), the `perf-lab-results` artifact (merged
TSV + every shard's raw files), and the per-shard `shard-<cell>` artifacts.

## Files

| file | role |
|---|---|
| `install-deps.sh` | defensive dependency install (apt as root, `sudo -n` fallback, or verify a preinstalled toolchain; fails fast with a clear summary message otherwise) |
| `build-gcc.sh SRC BUILD [DLCACHE]` | build one compiler exactly like `.github/workflows/ci.yml`: binutils 2.42 combined tree (sha256-pinned tarball, cached in DLCACHE), gas integrated-as patch from that tree's own `contrib/gas-embed`, `--disable-bootstrap --enable-languages=c,c++ --disable-multilib --with-system-zlib --disable-nls --disable-werror`, libgas.a, `all-gcc` + `all-target-libstdc++-v3` |
| `pack-compiler.sh BUILD OUT.tar.zst` | package the minimal runnable subset of a build tree for the shards: `gcc/xg++`, `gcc/cc1plus`, `gcc/specs`+`gcc/as` if present, `gcc/include{,-fixed}`, the built libstdc++ headers, and the source tree's `libsupc++` headers; smoke-compiles a `<vector>` TU from the staged package before tarring |
| `gen-benches.sh OUTDIR` | deterministic bench inputs: committed TUs copied verbatim, nlohmann json.hpp v3.11.3 downloaded (sha256-pinned; override with `PERF_LAB_JSON_HPP`), huge_tu generated (big_tu + 24x120 instantiation storm, >1 GiB GGC), 620-header/20-package realism fixture generated |
| `run-shard.sh CELL TIP [BASE]` | ONE cell's complete measurement (the CI path): warmups + interleaved median-of-N A/B on a pinned core, only that cell's extras (THP deltas / strace / PCH build+verify), writes `shard-summary.tsv` and self-reports NOISY when its two sides' ranges overlap |
| `aggregate-shards.sh SHARDS_IN OUT` | merge shard summaries into the final table, expected-direction checks, SURPRISE flags, one-stop artifact |
| `run-suite.sh SUITE TIP [BASE]` | the original one-box protocol (all cells sequentially in one process); kept for local runs and as the protocol reference -- the shards run the same per-cell protocol |
| `tu/` | small TUs + PCH preludes, byte-identical to the ones the merged numbers were measured with |

## Measurement protocol

Ported unchanged from the branch's original measurement methodology; the
sharding changes WHERE cells run (one hosted VM per cell), not HOW:

- CPU-pinned (`taskset -c`, default CPU 2, `PIN_CPU` to override; skipped
  gracefully if taskset is unavailable or the cpuset forbids it)
- 2 warmup runs, then median of 7 (1 warmup + median of 5 for huge_tu)
- base/tip interleaved run-by-run within each round, on the same core, to
  neutralize load drift -- this A/B stays on one VM; only cell-to-cell
  comparisons cross machines (they were never the measurement)
- every run under `/usr/bin/time -v`: wall + max RSS + **minor faults** (the
  THP signal), rc recorded per run, non-zero-rc runs excluded from medians
- honesty rule: a shard whose two sides' min..max wall ranges overlap
  reports `NOISY` instead of claiming a direction
- `/proc/vmstat` `thp_fault_alloc`/`thp_fault_fallback`/`thp_collapse_alloc`
  deltas captured around a huge_tu compile per side (huge-O2 shard) --
  direct evidence of whether the GGC MADV_HUGEPAGE advise is being serviced
- `strace -c` on the realism TU per side (realism-O0 shard; dir-index /
  readlink syscall wins)
- PCH: the fork's driver breaks `-x c++-header` (duplicate `-o` from the
  integrated-as single-process change -- known, pre-existing), so `.gch`
  files are built by driving `cc1plus` directly; PCH *use* goes through the
  driver. Each pch shard verifies its cell via `-H` (the `!` marker) and by
  `.o` byte-identity against the no-PCH compile (except anchor, by design).

## Assumptions / requirements

- x86-64 Linux runner (the PCH cc1plus invocation passes
  `-march=x86-64 -mtune=generic -imultiarch x86_64-linux-gnu`)
- bash >= 4.4, curl, GNU tar + xz + zstd; `/usr/bin/time` (installed by
  `install-deps.sh`)
- outbound HTTPS to fetch binutils 2.42 (ftp.gnu.org + two mirror fallbacks;
  each build leg fetches its own) and json.hpp (raw.githubusercontent.com,
  per shard)
- ~15 GB free disk on a build leg; shards need only ~1 GB for the unpacked
  compiler package(s) + bench inputs
