# Why the fork's compile-cache "direct mode" (manifest serve) never hits on llama.cpp

## Cause

The original hypothesis (manifest key incorporates build-dir-specific paths) is **wrong**.
The manifest key (MK) is deliberately build-dir-independent for `-g0` builds, and that
works. The real cause: **no manifest is ever stored for any TU that evaluates
`__has_include`** — and every real C++ TU does, because libstdc++'s root config header
`bits/c++config.h` (included by every standard header) evaluates
`__has_include(<pstl/pstl_config.h>)` and `__has_include(<tbb/tbb.h>)`
(lines 872/878 of the installed header).

`cc_store_manifest` (`gcc/compile-cache.cc:1750`) bails with
`manifest-skip-has-include` when `cc_tu_used_has_include` is set (the libcpp-side flag
`used_has_include`, `libcpp/internal.h:462`, read via `cpp_used_has_include()`). Rationale
in the code: a header probed by `__has_include` but never included leaves no trace in the
include closure, so the manifest's stat/hash re-check could not notice the probe result
flipping (a file *appearing* can't be detected by re-verifying recorded files) — serving
would be unsound. Sound decision, catastrophic reach: it disables the entire pre-parse
fast path for ~any TU that includes any C++ standard header. Warm rebuilds therefore pay
full driver + cc1plus startup + preprocess + closure hashing on every TU (deep post-preprocess
cache hits replay the object, skipping only the back end): 156.8 s wall / 437 s user for
llama.cpp vs ccache's 73.6 s / 112 s, whose direct mode does serve these TUs.

For the record, MK = schema version + domain tag + optional `GCC_COMPILE_CACHE_SALT` +
compiler checksum + lang name + (only under `-g`: main source path + cwd) +
output-affecting options + search-path option **values** (`-I` etc., anti-shadowing) +
main source bytes (`ccs_compute_manifest_key`, `gcc/compile-cache-serve.cc:272`; twin
`cc_compute_manifest_key`, `compile-cache.cc:858`). In the llama.cpp benchmark none of
these differ across build dirs: Release has no `-g` (so cwd/src-path are excluded), the
`-I` values are absolute *source-tree* paths identical in both dirs, and `-o`
/`-dumpbase`/`-dumpdir` are explicitly excluded.

## Evidence

1. Benchmark cache (`bench/gcc-cache2`) holds ~397 deep `.o` entries but only **25**
   manifests — all from cmake configure-probe TUs (`CMakeCXXCompilerId.cpp`, OpenSSL
   config checks) that don't touch libstdc++.
2. Probe, tiny TU with **no** includes (`int f(){return 42;}`): 1st compile →
   `manifest-store`; 2nd compile same cwd → `manifest-hit`; 3rd compile from a
   **different cwd** → `manifest-hit`. Cross-build-dir manifest serving works when a
   manifest exists.
3. Probe, `#include <string>` only: 1st compile → `store` + `manifest-skip-has-include`;
   2nd compile **same cwd** → `manifest-miss` + deep `hit`. Exactly the benchmark
   symptom, reproduced without changing directory — this is not a cross-dir issue at all.
4. Real benchmark TU (`common/common.cpp`, exact benchmark flags, salted to force a full
   compile) → `manifest-skip-has-include`.
5. Re-running the earlier agent's exact probe command from a **different cwd** against the
   benchmark cache computes the byte-identical MK `c84fa7b713f3` and the identical deep
   key `05fcd0259f53` — direct proof the manifest key does not incorporate the build dir.
6. `grep __has_include` over `<string>`'s 140-header closure → only `bits/c++config.h`
   (+ `<bit>`); c++config.h is in every libstdc++ header's closure.

## Suggested fix

Make `__has_include` sound instead of disqualifying — record probes in the manifest and
re-verify them at serve time:

- **Positive probes** (`__has_include` returned true): record the resolved file as an
  ordinary manifest header record (path + size + mtime + hash). The existing serve-time
  re-check then validates it; if the file vanishes or changes, the entry misses. Probes
  whose file was subsequently `#include`d are already covered by the closure.
- **Negative probes** (returned false): record the candidate paths libcpp tried
  (`_cpp_find_file` knows them) as "must-still-be-absent" records; serve verifies each
  with a failed `stat()`. A file appearing → miss → full compile. Conservative and sound.
- libcpp already centralizes evaluation in `builtin_has_include` (`libcpp/macro.cc`), the
  same place that sets `used_has_include` — hook the recording there.

This restores the manifest fast path for effectively all C++ TUs at the cost of a few
extra stat()s per serve, and should close most of the 156.8 s → 73.6 s gap to ccache
(whose direct mode demonstrably serves these same TUs). An interim smaller win with the
same machinery: keep skipping only TUs that had a *negative* probe, storing positive-probe
files as header records.
