#!/bin/sh
# Rebuild libgas.a and the standalone test harness for the embedded GAS
# assembler (Stage 2).  Assumes the combined tree is already built (see
# README.md): bfd/opcodes/gas objects exist under $BUILD.
#
# Usage: build-libgas.sh [BUILD_DIR]
set -e

BUILD="${1:-/home/user/gcc-build-gas}"
# GAS_SRC is the gas source dir (a symlink -> binutils-2.42/gas in the combined
# tree).  Defaults to the local checkout; override via $GAS_SRC for CI, where the
# GCC checkout (and its gas symlink) live under $GITHUB_WORKSPACE.
GAS_SRC="${GAS_SRC:-/home/user/gcc-14/gas}"
# GAS_EMBED_DIR is contrib/gas-embed (holds gas_embed_test.c).  Override for CI.
GAS_EMBED_DIR="${GAS_EMBED_DIR:-/home/user/gcc-14/contrib/gas-embed}"
GASB="$BUILD/gas"
# Compiler and extra flags for the objects and the test harness built by THIS
# script.  Instrumented/PGO tree builds must pass flags matching the tree's
# CFLAGS (e.g. EXTRA_CFLAGS='-fprofile-generate'): the harness links the
# combined-tree archives, and if those were compiled with -fprofile-generate
# they reference __gcov_* symbols that only resolve when the harness link
# uses the same flag.  Defaults preserve the historical plain build.
CC="${CC:-x86_64-linux-gnu-gcc}"
EXTRA_CFLAGS="${EXTRA_CFLAGS:-}"

# The exact compile flags the gas Makefile uses for gas/*.o (run from $GASB).
# (Captured from `make V=1 messages.o`.)
CFLAGS_GAS="-DHAVE_CONFIG_H -I. -I$GAS_SRC -I../bfd -I$GAS_SRC/config \
  -I$GAS_SRC/../include -I$GAS_SRC/.. -I$GAS_SRC/../bfd \
  -DLOCALEDIR=\"/usr/local/share/locale\" \
  -W -Wall -Wstrict-prototypes -Wmissing-prototypes -Wshadow \
  -Wstack-usage=262144 -Wwrite-strings -g -O2"

cd "$GASB"

echo ">>> compiling embed.o"
# shellcheck disable=SC2086
$CC $CFLAGS_GAS $EXTRA_CFLAGS -c -o embed.o "$GAS_SRC/embed.c"

echo ">>> compiling as-embed.o (as.c with main renamed)"
# shellcheck disable=SC2086
$CC $CFLAGS_GAS $EXTRA_CFLAGS -Dmain=gas_unused_main -c -o as-embed.o "$GAS_SRC/as.c"

echo ">>> building libgas.a"
# All top-level gas *.o EXCEPT the original as.o (it has the real main); this
# glob already includes embed.o and as-embed.o.  Plus the 3 config objects.
CORE_OBJS=$(ls *.o | grep -vE '^as\.o$' | tr '\n' ' ')
CONFIG_OBJS="config/tc-i386.o config/obj-elf.o config/atof-ieee.o"
rm -f "$BUILD/libgas.a"
# shellcheck disable=SC2086
ar rcs "$BUILD/libgas.a" $CORE_OBJS $CONFIG_OBJS
echo "$CORE_OBJS $CONFIG_OBJS" > "$BUILD/libgas.objlist"

# Symbol-clash fix for linking into cc1plus (Stage 3a).  GAS's as-embed.o
# strongly defines text_section/data_section/bss_section (the COMMON-globals
# TU); GCC's varasm.o ALSO strongly defines globals of those exact names
# (as 'section *').  Two strong defs => multiple-definition link error.  Rename
# the GAS copies (definition AND every cross-TU reference, uniformly across all
# archive members) so they no longer collide with GCC's.  These are the ONLY
# three clashes (verified by a full nm intersection of libgas vs GCC's
# cc1plus-linked TUs; 'verbose' also intersects but lives only in collect2/
# lto-wrapper objects, which cc1plus does not link).
REDEFS=$(mktemp)
cat > "$REDEFS" <<'EOF'
text_section gas_text_section
data_section gas_data_section
bss_section gas_bss_section
EOF
objcopy --redefine-syms="$REDEFS" "$BUILD/libgas.a"
rm -f "$REDEFS"
# Sanity: the clashing names must be gone, the renamed ones present.
if nm "$BUILD/libgas.a" 2>/dev/null | grep -qwE 'B (text_section|data_section|bss_section)'; then
  echo "!!! ERROR: libgas.a still strongly defines a clashing *_section symbol" >&2; exit 1
fi
echo ">>> libgas.a built: $(ls -l "$BUILD/libgas.a" | awk '{print $5}') bytes (section syms renamed gas_*)"

# Sanity: must export gas_assemble_buffer and must NOT define main.
if nm "$BUILD/libgas.a" | grep -q ' T gas_assemble_buffer$'; then
  echo ">>> OK: libgas.a exports gas_assemble_buffer"
else
  echo "!!! ERROR: libgas.a missing gas_assemble_buffer" >&2; exit 1
fi
if nm "$BUILD/libgas.a" | grep -q ' T main$'; then
  echo "!!! ERROR: libgas.a still defines main" >&2; exit 1
fi

# binutils' configure auto-detects libzstd: if it was present when bfd/gas were
# built, libbfd.a/libgas.a reference ZSTD_* and the link needs -lzstd; if it was
# absent (e.g. no libzstd-dev), they do not and -lzstd would fail to resolve.
# Detect from the actual archives so the same script works in both environments.
ZSTD_LIB=
if nm "$BUILD/bfd/.libs/libbfd.a" "$BUILD/libgas.a" 2>/dev/null \
     | grep -qE '^ *U +ZSTD_'; then
  ZSTD_LIB=-lzstd
  echo ">>> zstd detected in binutils libs; linking with -lzstd"
fi

echo ">>> building harness gas_embed_test"
# shellcheck disable=SC2086
$CC $EXTRA_CFLAGS "$GAS_EMBED_DIR/gas_embed_test.c" \
  "$BUILD/libgas.a" \
  "$BUILD/bfd/.libs/libbfd.a" \
  "$BUILD/opcodes/libopcodes.a" \
  "$BUILD/libsframe/.libs/libsframe.a" \
  "$BUILD/libiberty/libiberty.a" \
  -lz $ZSTD_LIB \
  -o "$BUILD/gas_embed_test"
echo ">>> done: $BUILD/gas_embed_test"
