// Build-performance results matrix for this fork, maintained one cell at a
// time by .github/workflows/profiling.yml via the
// wow-look-at-my/profiling-results-matrix action. Rendered live at:
//
//   https://github.com/wow-look-at-my/gcc/wiki/Build-Performance-Matrix
//
// Each cell is the WALL SECONDS (integer, lower is better) of one timed
// -j$(nproc) build of the row's pinned corpus workload with the column's
// toolchain configuration, measured on a GitHub-hosted ubuntu-latest runner
// (4 vCPU). The timed section covers exactly the project build (configure +
// compile + link); source fetch, the llama.cpp webui pre-build, cache
// populate passes (for the warm columns) and post-build smoke tests are
// excluded. Workload pins and per-column toolchain setup live in
// .github/scripts/ci-corpus.mjs (CORPUS_TIMING_LEG mode) and the workflow.
//
// The "fork dist" columns use the latest published dist of develop-matt/v14
// (https://dl.pazer.build/gcc?branch=develop-matt/v14&os=linux&arch=amd64).
// When that dist meaningfully changes (new optimization stage lands), bump
// `epoch` below and commit: pushes touching this file re-run the whole
// matrix (see profiling.yml's push trigger), and old numbers render struck
// through until re-measured.
//
// Config contract: default-export one MatrixConfig-shaped object; the action
// validates it at runtime (erasable-syntax TypeScript only).

const config = {
  id: 'gcc-fork-build-perf',
  title: 'Build performance matrix (corpus workloads x toolchain configs)',
  page: 'Build-Performance-Matrix',
  // The gcc wiki git repo exists (bootstrapped by hand), so pin it: a probe
  // failure should fail loudly rather than quietly fall back to a branch.
  storage: 'wiki',
  epoch: 1,
  unit: 's',
  // Cells are marked in-flight when their job STARTS (not when queued); a
  // leg is timeboxed to 60 min, so anything in-flight past 90 min is a
  // hard-died runner and renders as lost.
  inFlightTtlMinutes: 90,
  rows: [
    { key: 'llama-release', label: 'llama.cpp b9891 (Release)' },
    { key: 'llama-relwithdebinfo', label: 'llama.cpp b9891 (RelWithDebInfo, `-g`)' },
    { key: 'fmt', label: 'fmt 11.0.2 (Release, tests on)' },
    { key: 'sqlite', label: 'sqlite 3.46.1 amalgamation (`-O2`)' },
    { key: 'zlib-ng', label: 'zlib-ng 2.2.1 (Release)' },
    { key: 'openssl', label: 'openssl 3.3.1 (`no-asm`, static)' },
  ],
  cols: [
    { key: 'stock', label: 'Ubuntu `gcc-13` (stock)' },
    { key: 'fork', label: 'fork dist' },
    { key: 'fork-cache-cold', label: 'fork + `-fcompile-cache` (cold)' },
    { key: 'fork-cache-warm', label: 'fork + `-fcompile-cache` (warm)' },
    { key: 'ccache-fork', label: '`ccache` (warm) + fork dist' },
  ],
};

export default config;
