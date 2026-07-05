# Results (corrected data set, `results2.jsonl`)

Full `ninja` build of llama.cpp @ 7f4cced (Release, `-DGGML_CCACHE=OFF`,
`LLAMA_CURL=OFF`, 549 edges / 418 objects), 4-core VM, ninja `-j6`.
Compilers: distro gcc 13.3.0 (Ubuntu 24.04) vs this fork's PGO build
(`xgcc (GCC) 14.4.1 20260626`, CI run 28681611255, commit f7a3fd96a).
Recorded 2026-07-04; see `ENV.md` for the full environment and
`README.md` for the protocol and guards.

| config | wall (s) | user (s) |
|---|---|---|
| sys-true-nocache (run 1 / 2) | 297.0 / 275.2 | 999 / 972 |
| fork-pgo-true-nocache (run 1 / 2) | 284.0 / 278.9 | 992 / 977 |
| fork-cache-cold | 283.1 | 993 |
| fork-cache-warm | 156.8 | 437 |
| ccache-fork-cold | 301.1 | 1043 |
| ccache-fork-warm | 73.6 | 112 |

("warm" = populated cache but a fresh, re-configured build directory.)

## Findings

1. **Cold-build throughput is within run-to-run noise of distro GCC 13.3**
   (284.0/278.9 vs 297.0/275.2 wall). The fork's extra features cost
   nothing measurable on a real workload.

2. **The in-compiler cache's store overhead is ~0**: fork-cache-cold 283.1
   vs 284.0/278.9 no-cache — inside noise. ccache's store overhead on the
   very same compiler is ~+6% (301.1).

3. **Warm speedup: fork cache 1.8x vs ccache 3.9x** (156.8 s vs 73.6 s
   against the ~283 s no-cache baseline), on the same compiler and the
   same workload. Cause: `cc_store_manifest` skips the manifest store
   for any TU that evaluated `__has_include`, and libstdc++'s
   `bits/c++config.h` (in every standard header's closure) always does —
   so no manifest is ever stored for a real C++ TU, the pre-parse fast
   path never fires, and every TU falls back to the full preprocess+hash
   path before finding its deep (post-preprocess) hit. (Not a
   build-dir/cwd keying issue — the manifest key is build-dir-independent
   for `-g0` builds; see `DIAGNOSIS.md` for the full analysis.)
   Probe evidence (`GCC_COMPILE_CACHE_DEBUG=1` recompile of a real TU
   against the warm cache):

   ```
   compile-cache: manifest-miss c84fa7b713f3 common/CMakeFiles/llama-common.dir/common.cpp.o
   compile-cache: hit          05fcd0259f53 common/CMakeFiles/llama-common.dir/common.cpp.o
   ```

   The residual per-TU work is visible in user time: 437 s (fork warm) vs
   112 s (ccache warm), and in the warm ninja log only 83 of 418 object
   edges finish in <500 ms. Recording `__has_include` probes in the
   manifest and re-verifying them at serve time (see `DIAGNOSIS.md`) is
   the obvious next lever.

4. **Correctness: all 418 objects are byte-identical** (sha256) across
   fork no-cache, fork-cache-cold and fork-cache-warm
   (`objhash-verdict.txt`: IDENTICAL, 418 objects).

5. **The warm floor is ~70 s and it is not compile time**: the
   `llama-ui-assets` generation step is a 66.6 s serial edge (plus a
   15.5 s `ui.cpp.o` behind it) on the critical path. ccache-warm's
   73.6 s sits essentially on that floor; no compiler cache can go lower
   on this workload.
