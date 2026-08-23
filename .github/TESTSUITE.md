# GCC testsuite gate + corpus validation

How this fork proves it is a **drop-in replacement** for upstream GCC 14.4.1:
a dejagnu-testsuite diff gate against a vanilla baseline, plus a corpus of
real projects built with the shipping compiler. Everything lives in
`.github/workflows/ci.yml`.

## The pieces

| job | trigger | what it does |
|---|---|---|
| `testsuite-baseline` | dispatch with `build_baseline=true` | Builds **pristine upstream GCC at the fork point** (commit `820ff02b9` "Bump BASE-VER.", releases/gcc-14, 14.4.1 -- no fork patches, no binutils/libgas overlay) and runs `check-gcc`, `check-g++` and the libstdc++ suite in two shards. |
| `testsuite-baseline-publish` | same dispatch | Tars the three baseline `.sum` files (+ `BASELINE-INFO.txt` provenance) and publishes them to buildhost project `gcc/testsuite-baseline`. |
| `testsuite` | pushes to `develop-matt/v14`, or dispatch with `run_testsuite=true` | Runs the same three suites against the fork compiler (the `gccbuild` artifact from `build`), downloads the latest published baseline, and **fails on any new FAIL or XPASS** (see gate semantics below). Not on every push: the suites are multi-hour, a deliberate deviation from the org's plain `on: push` default. |
| `corpus` | pushes to `develop-matt/v14`, or dispatch with `run_corpus=true` | Builds openssl / zlib-ng / sqlite / fmt / llama.cpp with the shipping `gcc-dist`: cache OFF, cache ON cold, cache ON warm in a fresh build dir. Asserts builds + quick tests pass, **warm objects are byte-identical to cold**, and the warm build really served from the cache (per-project hit floors; the llama.cpp RelWithDebInfo leg requires >= 300 cross-directory manifest hits through `-ffile-prefix-map=<builddir>=.`, exercising the prefix-mapped `-g` keys). |

## Gate semantics (`.github/scripts/testsuite-diff.mjs`)

For each `.sum` pair the job runs `contrib/compare_tests` (embedded in the
report, informational) and gates on its own parse:

- **new FAIL** -- a test that FAILs now but did not FAIL in the baseline.
  Mirrors compare_tests normalization: `XFAIL` and `ERROR` count as FAIL on
  both sides, so `PASS -> XFAIL` gates and `XFAIL -> FAIL` does not; a test
  absent from the baseline that FAILs now gates too.
- **new XPASS** -- a test XPASSing now that did not XPASS in the baseline
  (compare_tests folds XPASS into PASS, so this class is gated here).

Progressions (new PASSes, fixed FAILs) and disappeared tests never gate.
The fork's `.sum` files plus the diff report are uploaded as the
`testsuite-sums-<shard>` artifact on every run, pass or fail.

## Generating / regenerating the baseline

The `testsuite` job **cannot pass until a baseline has been published once**
(it fails fast, before running any suite, with a pointer here). Publish one
with:

```
gh workflow run ci.yml -R wow-look-at-my/gcc --ref <branch> -f build_baseline=true
```

Result: `https://dl.pazer.build/gcc/testsuite-baseline?os=linux&arch=amd64`
(anonymous download; the publish tags the release with the publishing
branch, so the gate tries the current ref's branch first, then
`develop-matt/v14`, then bare latest -- the baseline is a property of the
fork point, so any published copy is acceptable). Regenerate when:

- the fork rebases onto a new upstream commit (update the fork-point sha in
  `ci.yml` and here first), or
- the runner image moves enough to shift environment-dependent tests --
  skew shows up as unexplained paired diffs in the gate report.

## Waiving known-benign diffs

`.github/testsuite-known-diffs.txt` is consulted before failing. One entry
per line, `#` comments ignored:

```
<test text>              # waives the test in every .sum
<sum>:<test text>        # one .sum only, e.g.:
g++.sum:g++.dg/opt/pr12345.C  -std=c++17 (test for excess errors)
```

The text must match the `.sum` line after `FAIL: `/`XPASS: ` exactly
(trimmed). Take entries verbatim from the gate's `GATE new FAIL:`/`GATE new
XPASS:` log lines or the report artifact, and annotate each with a comment
saying why it is benign. The gate logs unused entries so stale waivers can
be pruned.

## Deliberate QoI trades

Waiver-ledger-style entries for on-purpose behavior deltas that are not
testsuite diffs. The spec-only policy still applies: each entry must say
exactly which bar moved and why, and every serve must remain verified.

- **Manifest-key search-path normalization (ccache `base_dir` parity,
  2026-07).** `cc_mk_search_path_relative` (compile-cache-serve.cc, called
  by both MK twins) hashes an include search-path *value* lying inside the
  compile cwd in its cwd-relative form, so two build dirs differing only in
  absolute location (cmake's absolute `-I<builddir>/sub` for generated
  headers) produce one MK. This lowers **only the MK collision bar** (MK is
  a lookup index, never an authority): compiles from different build dirs
  that formerly keyed apart can now select the same manifest, and a
  candidate re-verifies against its *recorded* absolute header paths --
  possibly the other build dir's (still existing, still content-matching)
  copies rather than this dir's same-named ones. Object serving stays
  verified end to end (per-header stat/hash records + the full-closure
  content-addressed object key), which is exactly ccache
  `base_dir`+`CCACHE_NOHASHDIR`'s bar; if the recorded headers changed or
  vanished, the records fail and the TU recompiles. Search-path values
  *outside* the cwd still hash raw, preserving the anti-shadowing bar for
  directories the build tree does not own. Why: measured on the llama.cpp
  Release warm leg, build-dir `-I` values re-keyed 16 TUs per fresh build
  dir at ~2.5 s each -- ~12.3 s of the 13.2 s fork-vs-ccache warm gap
  (hitpath measurement run 2026-07-09; regression-guarded by
  ci-verify-cache check 29).

## Corpus notes

- Sources are pinned (openssl-3.3.1, zlib-ng 2.2.1, sqlite amalgamation
  3.46.1, fmt 11.0.2, llama.cpp b9891) in `.github/scripts/ci-corpus.mjs`.
- `SOURCE_DATE_EPOCH` is pinned so openssl's `buildinf.h` timestamp cannot
  break the cold/warm byte-identity assertion.
- Byte identity is only asserted cold-vs-warm (both compiled with
  `-fcompile-cache`): the cache-OFF build differs legitimately under `-g`
  because `DW_AT_producer` records the extra flag.
- The hit floors exist because byte identity alone cannot distinguish "warm
  serve" from "deterministic recompile"; a floor shortfall means the serve
  path regressed. Failure evidence (per-phase cache-debug logs, object-hash
  manifests) lands in the `corpus-logs-<project>` artifact.

## Follow-ups surfaced by corpus validation

- libgas should accept the ubiquitous benign `-Wa,--noexecstack` (fold it
  into the cache key) so openssl-class builds retain caching. Today any
  `-Wa,` option takes the sound external-as fallback and the build loses
  the cache entirely (the corpus openssl leg works around this with
  `no-asm`).
- Perf-only: the driver-twin and cc1-twin manifest keys diverge for some
  TUs once an auto-PCH `.gch` exists -- the cc1 twin still serves and
  byte-identity holds, but the driver fast path is defeated. Investigate
  folding the gch record into the driver-side key.
