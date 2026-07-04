#!/bin/bash
# perf-lab: install build + measurement dependencies on the runner.
#
# Defensive: written to cope with any Debian-ish userland, not just the
# GitHub-hosted ubuntu image. Strategy:
#   1. apt-get exists and we are root            -> plain apt-get
#   2. apt-get exists, not root, passwordless sudo -> sudo -n apt-get
#   3. no apt-get (or no way to run it)          -> check whether a usable
#      toolchain is already present; fail fast with a clear message if not.
set -euo pipefail

# build-essential: gcc/g++/make/libc headers.  gmp/mpfr/mpc/isl: GCC prereqs.
# zlib1g-dev: --with-system-zlib.  libzstd-dev: LTO section compression.
# libgoogle-perftools-dev: libtcmalloc_minimal.a, statically linked by the
# tip compiler when present (base ignores it -- that asymmetry is part of the
# measured win).  time: /usr/bin/time -v.  strace: syscall counting (optional
# but cheap).  util-linux: taskset.
PKGS=(
  build-essential flex bison m4 patch
  libgmp-dev libmpfr-dev libmpc-dev libisl-dev
  zlib1g-dev libzstd-dev libgoogle-perftools-dev
  curl ca-certificates xz-utils time strace util-linux git
)

summary() {
  if [ -n "${GITHUB_STEP_SUMMARY:-}" ]; then
    printf '%s\n' "$*" >> "$GITHUB_STEP_SUMMARY"
  fi
}

if command -v apt-get >/dev/null 2>&1; then
  APT=(env DEBIAN_FRONTEND=noninteractive apt-get)
  if [ "$(id -u)" -ne 0 ]; then
    if command -v sudo >/dev/null 2>&1 && sudo -n true 2>/dev/null; then
      APT=(sudo -n env DEBIAN_FRONTEND=noninteractive apt-get)
    else
      echo "::error::apt-get is present but we are uid $(id -u) with no passwordless sudo"
      summary '## perf-lab: dependency install FAILED (apt-get present, no root/sudo)'
      exit 1
    fi
  fi
  "${APT[@]}" update
  "${APT[@]}" install -y --no-install-recommends "${PKGS[@]}"
  echo "deps installed via apt-get"
  exit 0
fi

echo "::warning::no apt-get on this runner; probing for a preinstalled toolchain"
missing=0
for c in gcc g++ make flex bison patch curl tar xz /usr/bin/time; do
  if ! command -v "$c" >/dev/null 2>&1; then
    echo "::error::required tool missing: $c"
    missing=1
  fi
done
for h in gmp.h mpfr.h mpc.h zlib.h; do
  if [ ! -e "/usr/include/$h" ] && [ ! -e "/usr/local/include/$h" ]; then
    echo "::warning::header not found in /usr/include or /usr/local/include: $h (configure will be the judge)"
  fi
done

if [ "$missing" -ne 0 ]; then
  summary '## perf-lab: dependency bootstrap FAILED'
  summary ''
  summary 'This runner has **no apt-get** and its preinstalled toolchain is incomplete'
  summary '(missing tools listed in the job log). Iteration-2 options, pending census'
  summary 'facts: run the bench job in a `container:` (needs docker on the runner) or'
  summary 'vendor a toolchain. Not implemented preemptively -- see contrib/perf-lab/README.md.'
  exit 1
fi
echo "usable preinstalled toolchain found; proceeding without package installs"
