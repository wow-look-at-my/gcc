# GCC fork: compile-time performance work

This is `wow-look-at-my/gcc`, a fork of GCC 14 (branched from upstream
`releases/gcc-14` right after the 14.4.0 release) whose development branch
`develop-matt/v14` accumulates compile-*time* optimizations: making the
compiler itself faster and cheaper to run. Upstream's original readme is
untouched at [`README`](README); fork-specific design notes live in
[`single-process-compile.md`](single-process-compile.md) and
[`contrib/perf-lab/`](contrib/perf-lab/).

## Compile-time performance work

Every optimization stage of the fork, in true landing order (git
first-parent / PR-branch commit order), with the measured cost of
**that stage's compiler building the same pinned GCC-source workload**
("self-build", workload v1). Cells marked `?` have not been measured yet —
each one is fillable with a single workflow dispatch (see the measurement
protocol below the table).

| Stage (optimization added) | Landed in | SHA / recipe | Self-build wall (workload v1, hosted 4-vCPU, cold cache) | Δ vs previous stage | perf-lab run | Notes |
|---|---|---|---|---|---|---|
| stock upstream GCC (fork point) | upstream `releases/gcc-14`, 14.4.1-prerelease | `820ff02b98af` (plain recipe) | ? | — | [28727557771](https://github.com/wow-look-at-my/gcc/actions/runs/28727557771) (in flight) | last upstream commit before any fork work; all rows below are cumulative on top of this |
| `.o` compile cache + single-process (no-spawn) driver | [#2](https://github.com/wow-look-at-my/gcc/pull/2), merged 2026-06-29 | `91cd3c559eed` (combined-tree recipe from here on) | ? | ? | ? | cache is opt-in (`-fcompile-cache=`); this table measures the cache-off cold path |
| unconditional integrated assembler | [#5](https://github.com/wow-look-at-my/gcc/pull/5), merged 2026-06-29 | `625cae8cf3a8` | ? | ? | ? | #5 landed before #4 (git first-parent order) |
| compile cache: hardlinked objects + xattr metadata | [#4](https://github.com/wow-look-at-my/gcc/pull/4), merged 2026-06-29 | `24352cdc537b` | ? | ? | ? | |
| compile cache: no-sidecar rework | [#7](https://github.com/wow-look-at-my/gcc/pull/7), merged 2026-06-30 | `5226232bbe76` | ? | ? | [28727562357](https://github.com/wow-look-at-my/gcc/actions/runs/28727562357) (in flight) | pre-#13 anchor; the composed-A/B "base" side |
| GGC THP advise (`MADV_HUGEPAGE` on the GC heap) | [#13](https://github.com/wow-look-at-my/gcc/pull/13) c1/12 | `cfb70b36d96e` † | ? | ? | ? | needs a THP-capable host to express; hosted runners run THP `[always]` |
| include-path dir index (single-component) | [#13](https://github.com/wow-look-at-my/gcc/pull/13) c2/12 | `9cc7fbcd7250` † | ? | ? | ? | |
| dir-index stdin/pseudo-file fix | [#13](https://github.com/wow-look-at-my/gcc/pull/13) c3/12 | `0a335499e693` † | ? | ? | ? | correctness fix for the row above |
| cache hash-on-read (kill the miss re-read) | [#13](https://github.com/wow-look-at-my/gcc/pull/13) c4/12 | `ea09abc1c7db` † | ? | ? | ? | hashes every header read unconditionally until the #14 gate row below |
| dir index: multi-component includes | [#13](https://github.com/wow-look-at-my/gcc/pull/13) c5/12 | `a1aab0d31a66` † | ? | ? | ? | |
| template coerce-skip (raw-args spec probe) | [#13](https://github.com/wow-look-at-my/gcc/pull/13) c6/12 | `2ba24464a0fb` † | ? | ? | ? | |
| GGC heap-size cap 128M → 512M | [#13](https://github.com/wow-look-at-my/gcc/pull/13) c7/12 | `8ce8f4268eee` † | ? | ? | ? | trades RSS for fewer collections |
| static tcmalloc_minimal link into cc1/cc1plus | [#13](https://github.com/wow-look-at-my/gcc/pull/13) c8/12 | `b0a16bb30686` † | ? | ? | ? | link is conditional on libgoogle-perftools-dev at build time (perf-lab installs it) |
| readlink canonicalization skip | [#13](https://github.com/wow-look-at-my/gcc/pull/13) c11/12 | `229039c2a1a3` † | ? | ? | ? | tree ≡ the #13 squash merge `f7a3fd96a` modulo one doc commit; bench-corpus A/B vs pre-#13: −2.3…−7.9% wall by TU class (runs [28703247565](https://github.com/wow-look-at-my/gcc/actions/runs/28703247565), [28723328576](https://github.com/wow-look-at-my/gcc/actions/runs/28723328576) — different workload, do not paste here) |
| driver PCH spec fix (`-x c++-header`) | [#14](https://github.com/wow-look-at-my/gcc/pull/14) c3/14 | `8d60f8b239de` † | ? | ? | ? | correctness only, no perf claim |
| SHA-1 hash-on-read gate (hash only with cache configured) | [#14](https://github.com/wow-look-at-my/gcc/pull/14) c5/14 | `2c8db5e713ee` † | ? | ? | ? | gives back the unconditional hashing cost of the #13 hash-on-read row (−1.86% instr on fmt −O0, cache off) |
| transparent auto-PCH (`-fauto-pch`, opt-in) | [#14](https://github.com/wow-look-at-my/gcc/pull/14) c6/14 | `4e2e8ea470b2` † | ? | ? | ? | off by default, so the cold path this table measures is unchanged by design; warm-cache wins (1.29–1.38× at −O0) live in the [#14 tables](https://github.com/wow-look-at-my/gcc/pull/14) |
| fork tip (= #14 merge) | [#14](https://github.com/wow-look-at-my/gcc/pull/14), merged 2026-07-05 | `5c426dfddb81` | ? | ? | [28727587950](https://github.com/wow-look-at-my/gcc/actions/runs/28727587950) (in flight) | current `develop-matt/v14` |
| PGO-built compiler (same source as tip) | ci.yml `package-pgo` job | build-recipe variant, not a commit | ? | ? | ? | not yet dispatchable via selfbuild (needs a recipe=pgo suite variant); bench-corpus: −2.7…−9.4% vs the plain tip build |

† These SHAs sit inside squash-merged PRs, so they are on no branch — but
they remain fetchable via `refs/pull/13/head` / `refs/pull/14/head`
(verified: both chains are linear, and the PR-tip trees are byte-identical
to the squash merges `f7a3fd96a` / `5c426dfdd`). A perf-lab `selfbuild`
dispatch accepts them as `stage_ref` directly. Should GitHub ever garbage-
collect them, each stage is recomposable from the PR #10/#11/#12 heads plus
cherry-picks in the order above.

### Measurement protocol (how a `?` becomes a number)

- **Workload v1** = the 41 files in
  [`contrib/perf-lab/selfbuild-workload-v1.txt`](contrib/perf-lab/selfbuild-workload-v1.txt)
  (all 16 `libcpp/*.cc` + 25 heavyweight `gcc/` files), with **source
  content pinned at the fork point** `820ff02b9` — the workload never
  varies with the stage under test. Files are compiled with the pinned
  argv embedded in
  [`contrib/perf-lab/selfbuild-suite.sh`](contrib/perf-lab/selfbuild-suite.sh)
  (the real build's flags: `-g -O2`, `-fno-exceptions -fno-rtti`, the
  tree's own `-I` set), against the stage compiler's own in-tree libstdc++
  headers (the fork never touches libstdc++, so that input is constant).
- **Two metrics per cell**: `S s serial-sum / J s -j4`. The headline is
  the **serial sum** — every file compiled `-j1` pinned to one core, the
  per-file walls summed; 1 warmup pass (parallel), then median of 3 timed
  passes. The `-j4` wall is one full-parallelism pass (single-shot, what a
  parallel build feels like; noisier). Δ vs previous stage is computed on
  the serial sum.
- **Runner class**: GitHub-hosted `ubuntu-latest` (4 vCPU). Rows measured
  on this class are comparable to each other; numbers from any other
  machine class do not belong in this table.
- **Cold cache rule**: the compiler's own caches are OFF — no
  `-fcompile-cache=`, no `-fauto-pch`, cache/auto-PCH environment stripped.
  The table tracks the compiler's cold path; warm-cache regimes (.o-cache
  hits, auto-PCH injection) are measured separately in the PR #13/#14
  tables. The OS page cache is deliberately warm (that is what the warmup
  pass is for).
- **To fill a row**: Actions → perf-lab → Run workflow →
  `suite=selfbuild`, `stage_ref=<the row's SHA>` (optional
  `stage_label`). The run's step summary and `selfbuild-results` artifact
  contain a ready-to-paste README row; compute Δ against the previous
  filled row. ~60–100 min per stage; runs are independent and may be
  dispatched in parallel.
- **Every filled row must cite its perf-lab run** in the "perf-lab run"
  column (linked run ID). Rows dispatched but not yet concluded carry the
  linked run ID plus "(in flight)" so the data location survives even if
  the operator's session dies.
- **Honest caveats**: hosted runners differ VM-to-VM (CPU model varies;
  expect a few percent run-to-run variance on walls — the serial sum is
  the steadier metric; the secondary build walls below are context, not
  headlines). Each selfbuild job also records two secondary walls: the
  stage compiler's own build time (system g++ builds the stage source —
  source-size-confounded, later stages carry more code) and the pinned
  workload's `all-gcc` prep time (constant input — a per-run runner-speed
  calibration). Per-optimization micro-benchmarks (syscall counts,
  fault counts, bench-corpus walls) live in the constituent commit
  messages and the PR #13/#14 bodies; those are a **different workload**
  and are linked in Notes, never pasted into the cells.
