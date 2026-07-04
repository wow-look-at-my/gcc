# perf-lab -- self-hosted benchmark rig

Real-hardware compile-time benchmarking for this fork's performance work
(include-path dir index, GGC 512M cap + `MADV_HUGEPAGE`, static tcmalloc,
readlink skip, cache hash-on-read; merged as PR #13 / `f7a3fd96a`). The
integration sandbox those wins were first measured in could not express the
THP win at all (`thp_fault_alloc=0` system-wide), so the headline
MADV_HUGEPAGE number -- minor faults -92%, wall -10.6% on the original
hugetlb-proxy measurement -- needs a THP-capable host to reproduce. That is
what this rig is for.

Runs on the repo's self-hosted runner (`unraid-gpu-runner`: Ryzen 3800X,
128 GB RAM, Unraid) via `.github/workflows/perf-lab.yml`.

## One-click usage

Actions -> perf-lab -> Run workflow:

| input | meaning |
|---|---|
| `suite` | `census` (environment probe only), `quick` (~10 min measurement: bench_stl+big_tu -O2, tip only), `composed` (base-vs-tip A/B incl. THP fault deltas and the never-collect GC cell), `pch` (PCH ceiling A/B), `full` (composed+pch) |
| `bench_ref` | ref whose compiler is benchmarked ("tip"), default `develop-matt/v14` |

The composed base is pinned to `5226232bb` (the PR #13 merge base) in the
workflow env. Every push to the bootstrap branch also runs the `census` job
only -- a near-instant probe of what the runner actually is (toolchain, THP
mode, disk); the bench job is dispatch-only.

Results land in three places: the run's `$GITHUB_STEP_SUMMARY` (markdown
tables), the `perf-lab-results` artifact (raw TSV, per-run `/usr/bin/time -v`
output, strace and THP counter files, PCH verification), and the job log.

## Files

| file | role |
|---|---|
| `install-deps.sh` | defensive dependency install (apt as root, `sudo -n` fallback, or verify a preinstalled toolchain; fails fast with a clear summary message otherwise) |
| `build-gcc.sh SRC BUILD [DLCACHE]` | build one compiler exactly like `.github/workflows/ci.yml`: binutils 2.42 combined tree (sha256-pinned tarball, cached in DLCACHE), gas integrated-as patch from that tree's own `contrib/gas-embed`, `--disable-bootstrap --enable-languages=c,c++ --disable-multilib --with-system-zlib --disable-nls --disable-werror`, libgas.a, `all-gcc` + `all-target-libstdc++-v3` |
| `gen-benches.sh OUTDIR` | deterministic bench inputs: committed TUs copied verbatim, nlohmann json.hpp v3.11.3 downloaded (sha256-pinned; override with `PERF_LAB_JSON_HPP`), huge_tu generated (big_tu + 24x120 instantiation storm, >1 GiB GGC), 620-header/20-package realism fixture generated |
| `run-suite.sh SUITE TIP [BASE]` | the measurement protocol (below) |
| `tu/` | small TUs + PCH preludes, byte-identical to the ones the merged numbers were measured with |

## Measurement protocol

Ported unchanged from the branch's original measurement methodology:

- CPU-pinned (`taskset -c`, default CPU 2, `PIN_CPU` to override; skipped
  gracefully if taskset is unavailable or the cpuset forbids it)
- 2 warmup runs, then median of 7 (median of 5 for huge_tu)
- base/tip interleaved within each round to neutralize load drift
- every run under `/usr/bin/time -v`: wall + max RSS + **minor faults** (the
  THP signal), rc recorded per run, non-zero-rc runs excluded from medians
- `/proc/vmstat` `thp_fault_alloc`/`thp_fault_fallback`/`thp_collapse_alloc`
  deltas captured around a huge_tu compile per side -- direct evidence of
  whether the GGC MADV_HUGEPAGE advise is being serviced
- `strace -c` on the realism TU per side where strace exists (dir-index /
  readlink syscall wins)
- PCH: the fork's driver breaks `-x c++-header` (duplicate `-o` from the
  integrated-as single-process change -- known, pre-existing), so `.gch`
  files are built by driving `cc1plus` directly; PCH *use* goes through the
  driver. Each cell is verified via `-H` (the `!` marker) and by `.o`
  byte-identity against the no-PCH compile.

## Assumptions / requirements

- x86-64 Linux runner (the PCH cc1plus invocation passes
  `-march=x86-64 -mtune=generic -imultiarch x86_64-linux-gnu`)
- bash >= 4.4, curl, GNU tar + xz; `/usr/bin/time` (installed by
  `install-deps.sh`)
- outbound HTTPS to fetch binutils 2.42 (ftp.gnu.org + two mirror fallbacks,
  cached across runs in `dl/`) and json.hpp (raw.githubusercontent.com)
- ~15 GB free disk per built compiler (two for composed/full)

First runs are environment discovery: if the runner turns out to lack apt or
a toolchain, `install-deps.sh` fails fast with the facts in the step summary,
and the fix (container job or vendored toolchain) is decided from census
data, not guessed preemptively.
