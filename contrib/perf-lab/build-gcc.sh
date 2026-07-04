#!/bin/bash
# perf-lab: build this fork's compiler out-of-tree, combined-tree style.
#
#   build-gcc.sh SRC BUILD [DLCACHE]
#
# Replicates .github/workflows/ci.yml's known-good recipe exactly (binutils
# 2.42 combined tree + gas integrated-as patch + libgas.a, then all-gcc and
# all-target-libstdc++-v3), with paths parameterized so the same script can
# build both the bench ref ("tip") and the pre-optimization base compiler.
# Each SRC tree gets its OWN binutils extraction patched with its OWN
# contrib/gas-embed patch (the gas-embed dir differs between base and tip).
#
# DLCACHE (default ./dl) persists the binutils tarball across runs on a
# self-hosted runner; the tarball is sha256-pinned.
set -euo pipefail

SRC=${1:?usage: build-gcc.sh SRC BUILD [DLCACHE]}
BUILD=${2:?usage: build-gcc.sh SRC BUILD [DLCACHE]}
DL=${3:-$PWD/dl}

BINUTILS_V=2.42
TARBALL=$DL/binutils-$BINUTILS_V.tar.xz
# sha256 of the official binutils-2.42.tar.xz (verified against ftp.gnu.org)
BINUTILS_SHA=f6e4d41fd5fc778b06b7891457b3620da5ecea1006c6a4a41ae998109f85a800

test -f "$SRC/configure" || { echo "::error::$SRC is not a GCC source tree"; exit 1; }
test -f "$SRC/contrib/gas-embed/gas-integrated-as.patch" || {
  echo "::error::$SRC lacks contrib/gas-embed (not this fork?)"; exit 1; }

# ---- binutils tarball (cached, sha256-pinned, mirror fallback) -------------
mkdir -p "$DL"
tarball_ok() {
  [ -f "$TARBALL" ] && echo "$BINUTILS_SHA  $TARBALL" | sha256sum -c - >/dev/null 2>&1
}
if ! tarball_ok; then
  for url in \
    "https://ftp.gnu.org/gnu/binutils/binutils-$BINUTILS_V.tar.xz" \
    "https://sourceware.org/pub/binutils/releases/binutils-$BINUTILS_V.tar.xz" \
    "https://ftpmirror.gnu.org/gnu/binutils/binutils-$BINUTILS_V.tar.xz"; do
    echo "fetching $url"
    if curl -fL --connect-timeout 20 -o "$TARBALL.part" "$url"; then
      mv "$TARBALL.part" "$TARBALL"
      tarball_ok && break
    fi
  done
fi
tarball_ok || { echo "::error::could not fetch binutils-$BINUTILS_V.tar.xz (sha256 $BINUTILS_SHA)"; exit 1; }

# ---- combined tree in $SRC (mirrors ci.yml step for step) ------------------
rm -rf "$SRC/binutils-$BINUTILS_V"
tar -C "$SRC" -xf "$TARBALL"
test -d "$SRC/binutils-$BINUTILS_V/gas"

BU=$SRC/binutils-$BINUTILS_V
for d in bfd opcodes gas libsframe libctf; do
  ln -sfn "$BU/$d" "$SRC/$d"
done
for entry in "$BU"/include/*; do
  name=$(basename "$entry")
  case "$name" in
    ChangeLog*|MAINTAINERS|COPYING*) continue ;;
  esac
  if [ ! -e "$SRC/include/$name" ]; then
    ln -sfn "$entry" "$SRC/include/$name"
  fi
done

patch -p1 -d "$BU/gas" < "$SRC/contrib/gas-embed/gas-integrated-as.patch"
test -f "$BU/gas/embed.c"

# ---- configure + build (exact ci.yml flags) --------------------------------
rm -rf "$BUILD"
mkdir -p "$BUILD"
cd "$BUILD"
"$SRC/configure" \
  --disable-bootstrap --enable-languages=c,c++ --disable-multilib \
  --with-system-zlib --disable-nls --disable-werror \
  MAKEINFO=true

J=$(nproc)
make -j"$J" all-libiberty MAKEINFO=true
make -j"$J" all-bfd all-opcodes all-gas MAKEINFO=true

export GAS_SRC="$SRC/gas"
export GAS_EMBED_DIR="$SRC/contrib/gas-embed"
bash "$SRC/contrib/gas-embed/build-libgas.sh" "$BUILD"
test -f "$BUILD/libgas.a"

make -j"$J" all-gcc MAKEINFO=true
make -j"$J" all-target-libstdc++-v3 MAKEINFO=true

# ---- markers + smoke test ---------------------------------------------------
printf '%s\n' "$SRC" > "$BUILD/.perf-lab-src"
(git -C "$SRC" rev-parse HEAD 2>/dev/null || echo unknown) > "$BUILD/.perf-lab-sha"

T=$(basename "$(dirname "$(ls -d "$BUILD"/*/libstdc++-v3 | head -n1)")")
printf '#include <vector>\nint f(){ std::vector<int> v{1,2,3}; return (int)v.size(); }\n' \
  > "$BUILD/.perf-lab-smoke.cpp"
"$BUILD/gcc/xg++" -B"$BUILD/gcc" -nostdinc++ \
  -I"$BUILD/$T/libstdc++-v3/include/$T" \
  -I"$BUILD/$T/libstdc++-v3/include" \
  -I"$SRC/libstdc++-v3/libsupc++" \
  -O2 -c "$BUILD/.perf-lab-smoke.cpp" -o "$BUILD/.perf-lab-smoke.o"

echo "build OK: $BUILD (triplet $T, source $(cat "$BUILD/.perf-lab-sha"))"
