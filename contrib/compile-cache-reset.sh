#!/bin/sh
# compile-cache-reset.sh - reset GCC's in-compiler compilation cache.
#
# The in-compiler cache (-fcompile-cache=DIR, see "Developer Options" in the
# GCC manual) can be reset three ways:
#
#   1. Logical reset (non-destructive): set GCC_COMPILE_CACHE_SALT to a new
#      value.  Every cache key then changes, so old entries are no longer
#      looked up; nothing is deleted, and reverting the salt restores access.
#
#   2. Physical reset (destructive): delete the cache directory (rm -rf DIR).
#
#   3. CI reset: run the GCC CI workflow via workflow_dispatch with the
#      boolean input reset_cache=true (handled inside .github/workflows/ci.yml,
#      not by this script).
#
# This helper performs method 1 or 2 locally.
#
# Usage:
#   contrib/compile-cache-reset.sh --salt [VALUE]   # logical reset (print a salt)
#   contrib/compile-cache-reset.sh --purge [DIR]    # physical reset (delete dir)
#
# With --salt and no VALUE, a fresh salt (the current epoch seconds) is
# printed; export it so the next builds miss and re-populate:
#
#   eval "$(contrib/compile-cache-reset.sh --salt)"
#
# With --purge and no DIR, DIR defaults to $GCC_COMPILE_CACHE_DIR.

set -eu

usage () {
  sed -n '2,/^set -eu$/p' "$0" | sed 's/^# \{0,1\}//; s/^#$//'
  exit "${1:-2}"
}

case "${1:-}" in
  --salt)
    salt="${2:-$(date +%s)}"
    # Print an export statement so callers can `eval` it.
    printf 'export GCC_COMPILE_CACHE_SALT=%s\n' "$salt"
    printf '# logical reset: new salt mixed into every key (old entries unreachable, not deleted)\n' >&2
    ;;
  --purge)
    dir="${2:-${GCC_COMPILE_CACHE_DIR:-}}"
    if [ -z "$dir" ]; then
      printf 'error: no cache directory given and GCC_COMPILE_CACHE_DIR is unset\n' >&2
      exit 1
    fi
    if [ -d "$dir" ]; then
      rm -rf -- "$dir"
      printf 'physical reset: removed cache directory %s\n' "$dir" >&2
    else
      printf 'nothing to do: %s does not exist\n' "$dir" >&2
    fi
    ;;
  -h|--help|'')
    usage 0
    ;;
  *)
    printf 'error: unknown option %s\n\n' "$1" >&2
    usage 2
    ;;
esac
