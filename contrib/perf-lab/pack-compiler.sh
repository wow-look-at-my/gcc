#!/bin/bash
# perf-lab: package the minimal runnable subset of a build-gcc.sh tree.
#
#   pack-compiler.sh BUILD OUT.tar.zst
#
# The measurement shards run the compiler on a DIFFERENT runner than the one
# that built it, so the build job ships exactly what the run-shard.sh
# invocation lines reference -- nothing else from the ~10 GB build tree:
#
#   $PKG/gcc/xg++            the driver        (xg++ -B$PKG/gcc ...)
#   $PKG/gcc/cc1plus         the compiler proper (found via -B; also driven
#                            directly for .gch builds in the pch cells)
#   $PKG/gcc/specs           driver specs, if the build generated one (the
#                            driver loads a `specs` file found in a -B dir)
#   $PKG/gcc/as              gas, if present (the fork compiles single-process
#                            via embedded gas, but ship it so a driver
#                            fallback path can never fail on a shard)
#   $PKG/gcc/include{,-fixed}       cc1plus -isystem dirs (pch cells) and the
#                                   driver's -B include dirs
#   $PKG/$T/libstdc++-v3/include    built libstdc++ headers   (-I, 2 dirs)
#   $PKG/src/libstdc++-v3/libsupc++ source-tree supc++ headers (-I)
#   $PKG/.perf-lab-src       set to the RELATIVE path `src` -- run-shard.sh
#                            resolves non-absolute values against the package
#                            root, so the package works wherever it lands
#   $PKG/.perf-lab-sha       provenance (git SHA of the packaged compiler)
#
# Before tarring, the staged package must compile a <vector> smoke TU on its
# own (driver + -B + header set, no build tree in sight); the tarball is only
# produced if that passes.
set -euo pipefail

BUILD=${1:?usage: pack-compiler.sh BUILD OUT.tar.zst}
OUTTAR=${2:?usage: pack-compiler.sh BUILD OUT.tar.zst}

test -x "$BUILD/gcc/xg++"    || { echo "::error::$BUILD/gcc/xg++ missing"; exit 1; }
test -x "$BUILD/gcc/cc1plus" || { echo "::error::$BUILD/gcc/cc1plus missing"; exit 1; }
test -f "$BUILD/.perf-lab-src" || { echo "::error::$BUILD/.perf-lab-src missing (not a build-gcc.sh tree?)"; exit 1; }

SRC=$(cat "$BUILD/.perf-lab-src")
# glob on */libstdc++-v3/include so a src/libstdc++-v3/libsupc++ stub (e.g.
# when re-packing an unpacked package) can never shadow the real triplet dir
T=$(basename "$(dirname "$(dirname "$(ls -d "$BUILD"/*/libstdc++-v3/include | head -n1)")")")
test -d "$BUILD/$T/libstdc++-v3/include" || {
  echo "::error::$BUILD/$T/libstdc++-v3/include missing"; exit 1; }
test -d "$SRC/libstdc++-v3/libsupc++" || {
  echo "::error::$SRC/libstdc++-v3/libsupc++ missing"; exit 1; }

STAGE=$(mktemp -d)
trap 'rm -rf "$STAGE"' EXIT
PKG=$STAGE/pkg

mkdir -p "$PKG/gcc" "$PKG/$T/libstdc++-v3" "$PKG/src/libstdc++-v3"
cp -a "$BUILD/gcc/xg++" "$BUILD/gcc/cc1plus" "$PKG/gcc/"
[ -f "$BUILD/gcc/specs" ] && cp -a "$BUILD/gcc/specs" "$PKG/gcc/"
[ -f "$BUILD/gcc/as" ]    && cp -a "$BUILD/gcc/as"    "$PKG/gcc/"
# Header trees are copied with -L (dereference): the build tree's libstdc++
# include dir is ~800 SYMLINKS into the SOURCE tree. The source tree exists
# on the build runner, so shipped links resolve THERE -- but they dangle on
# the measurement shards and every compile dies with "vector: No such file
# or directory" (quick run 28704282166 failed exactly this way).
cp -RL "$BUILD/gcc/include" "$PKG/gcc/include"
[ -d "$BUILD/gcc/include-fixed" ] && cp -RL "$BUILD/gcc/include-fixed" "$PKG/gcc/include-fixed"
cp -RL "$BUILD/$T/libstdc++-v3/include" "$PKG/$T/libstdc++-v3/include"
cp -RL "$SRC/libstdc++-v3/libsupc++" "$PKG/src/libstdc++-v3/libsupc++"
printf 'src\n' > "$PKG/.perf-lab-src"
cp "$BUILD/.perf-lab-sha" "$PKG/.perf-lab-sha"

# ---- machine-independence guard ----------------------------------------------
# No symlink may ship: a link that resolves HERE (source tree present) can
# dangle on the shard runner, and the smoke compile below cannot see that.
NLINKS=$(find "$PKG" -type l | wc -l | tr -d ' ')
if [ "$NLINKS" -ne 0 ]; then
  echo "::error::$NLINKS symlink(s) in staged package -- they would dangle on the shard runner:"
  find "$PKG" -type l | head -20
  exit 1
fi

# ---- prove the staged package is self-sufficient ----------------------------
printf '#include <vector>\nint f(){ std::vector<int> v{1,2,3}; return (int)v.size(); }\n' \
  > "$STAGE/smoke.cpp"
"$PKG/gcc/xg++" -B"$PKG/gcc" -nostdinc++ \
  -I"$PKG/$T/libstdc++-v3/include/$T" \
  -I"$PKG/$T/libstdc++-v3/include" \
  -I"$PKG/src/libstdc++-v3/libsupc++" \
  -O2 -c "$STAGE/smoke.cpp" -o "$STAGE/smoke.o"
test -s "$STAGE/smoke.o"
echo "package smoke test OK (xg++ -c <vector> TU from staged package)"

# ---- tar + report ------------------------------------------------------------
export ZSTD_CLEVEL=9 ZSTD_NBTHREADS=0
tar --zstd -cf "$OUTTAR" -C "$PKG" .

UNPACKED=$(du -sh "$PKG" | cut -f1)
PACKED=$(du -h "$OUTTAR" | cut -f1)
echo "packaged $BUILD -> $OUTTAR"
echo "  contents: $(find "$PKG" -type f | wc -l) files, $UNPACKED unpacked, $PACKED compressed (zstd -$ZSTD_CLEVEL)"
echo "  sha: $(cat "$PKG/.perf-lab-sha")"
if [ -n "${GITHUB_STEP_SUMMARY:-}" ]; then
  {
    echo "- compiler package \`$(basename "$OUTTAR")\`: $PACKED compressed / $UNPACKED unpacked, sha \`$(cat "$PKG/.perf-lab-sha")\`"
  } >> "$GITHUB_STEP_SUMMARY"
fi
