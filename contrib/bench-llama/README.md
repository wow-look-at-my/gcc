# bench-llama: llama.cpp compile-time benchmark harness

Measures wall/user/sys time for a full `ninja` build of llama.cpp
(`wow-look-at-my/llama.cpp` @ 7f4cced, Release, `-G Ninja`, `LLAMA_CURL=OFF`)
under this fork's compiler and cache vs the distro compiler and ccache.
`RESULTS.md` has the numbers and findings; this file is the protocol.

## Files

- `run_headline.sh` — first benchmark script. Its numbers (`results.jsonl`)
  are **INVALID** (see the pitfall below); both are kept as the archival
  record of what was run and why it was redone.
- `run_corrected.sh` — corrected script; `results2.jsonl` is the
  authoritative data set (one JSON line per run).
- `ENV.md` — exact machine/toolchain inventory for the recorded runs.
- `RESULTS.md` — corrected results table + findings.

The scripts are archival records of the recorded runs: they hard-code the
session's scratchpad paths in `BENCH=`/`LLAMA=` and expect the CI build tree
layout described below. Adjust those variables to rerun.

## Configs

| config | compiler | cache |
|---|---|---|
| `sys-true-nocache` | distro gcc/g++ 13.3 | none |
| `fork-pgo-true-nocache` | fork xg++ (PGO build) | none |
| `fork-cache-cold` / `-warm` | fork xg++ (PGO build) | fork in-compiler cache (`-fcompile-cache=DIR`), wiped / kept |
| `ccache-fork-cold` / `-warm` | fork xg++ (PGO build) | ccache 4.9.1 in front of the fork wrapper |

"cold" = empty cache, measures store overhead. "warm" = populated cache,
**fresh build directory** (deleted and re-configured), measures real
second-build speedup — not a no-op ninja rerun.

## THE pitfall: GGML_CCACHE defaults ON

llama.cpp's `GGML_CCACHE` cmake option defaults ON: if ccache is on PATH,
ggml silently sets a global `RULE_LAUNCH_COMPILE=ccache` and every compile
of every "nocache" config gets cached to `~/.cache/ccache`. That poisoned
the entire first data set (`results.jsonl` — note the impossible ~70 s
"nocache" repeats). Every config in `run_corrected.sh` passes
`-DGGML_CCACHE=OFF` and asserts, per run, before timing:

- the configure log does NOT say "compilation results will be cached";
- `ninja -t commands | grep -c ccache` == 0 for non-ccache configs
  (and > 0 for the ccache configs);
- `-fcompile-cache` appears in the commands only for `fork-cache-*`.

If you rerun this benchmark and skip those guards, your numbers are wrong.

## Measurement discipline

- Runs execute under `nohup nice -n 19 ionice -c3` on an otherwise idle
  4-core VM; each timed run additionally waits for loadavg < 0.6
  (`wait_idle`, 180 s timeout, noted in the JSON if it times out).
- cmake configure is untimed; `/usr/bin/time -v ninja` (default `-j` =
  nproc+2 = 6) is the timed region.
- Every run gets a fresh `build2-<config>-<n>` dir; the previous run's dir
  is deleted first (disk headroom + no incremental reuse).
- Object-level evidence captured per run: sha256 of every `.o`
  (cache-correctness check), the `.ninja_log` (per-edge times), ccache
  stats, and an untimed `GCC_COMPILE_CACHE_DEBUG=1` probe recompile of one
  real TU to classify hit level.

## Reproducing with the CI artifact

The recorded runs used the `gccbuild-pgo` GHA artifact of workflow run
28681611255 (commit f7a3fd96a) extracted verbatim: a configured **build
tree**, not an installed compiler. Its `xg++` therefore needs the
build-tree libstdc++ spelled out, which is what the benchmark's wrapper
scripts did (they were the CMake `CMAKE_C(XX)_COMPILER`):

```sh
# compile (BUILD = extracted gccbuild/, T = x86_64-pc-linux-gnu,
# SRC = gcc checkout at the artifact's commit)
$BUILD/gcc/xg++ -B$BUILD/gcc -nostdinc++ \
  -I$BUILD/$T/libstdc++-v3/include/$T -I$BUILD/$T/libstdc++-v3/include \
  -I$SRC/libstdc++-v3/libsupc++ ...
# and when linking, additionally:
  -L$BUILD/$T/libstdc++-v3/src/.libs -Wl,-rpath,$BUILD/$T/libstdc++-v3/src/.libs
```

The build tree's libstdc++ `include/` entries are symlinks into the
runner's `$GITHUB_WORKSPACE`, so recreate
`/home/runner/work/gcc/gcc -> <checkout>` before using it.

Since then CI also publishes a small **installed dist**
(`https://dl.pazer.build/gcc?branch=<branch>&os=linux&arch=amd64`, a
`gcc-dist/` prefix, stripped, tar.zst). That one needs none of the above:
`gcc-dist/bin/g++` works from any extraction path with no `-B`/`-I` flags
(headers, cc1plus, libstdc++ all resolve relative to the driver). Binaries
it links use the system `libstdc++.so.6` at runtime; set
`LD_LIBRARY_PATH=<dist>/lib64` if yours is older than GCC 14's.

## Raw data

Full build logs, `.ninja_log` copies, and per-object sha256 lists are NOT
committed (bulky); they live in the recording session's scratchpad
(`bench/{logs,ninjalogs,objhash}/`). Per-edge timing claims in RESULTS.md
come from the ninja logs; quoted probe lines come from
`logs/probe-fork-cache.log`.
