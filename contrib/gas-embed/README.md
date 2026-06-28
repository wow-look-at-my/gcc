# gas-embed: in-process integrated assembler for GCC 14 (`-fintegrated-as`)

This directory implements an in-process **integrated assembler** for GCC 14 by
embedding the GNU assembler (GAS, from binutils 2.42) into the compiler. With
`-fintegrated-as`, cc1plus assembles its own output in memory and writes the
object file directly, so `g++ -c foo.cc -o foo.o` runs as a **single process**.

## Before / after

- **Before:** GCC forks a separate `as` process for every translation unit --
  cc1plus emits text assembly (`.s`), and the driver `exec`s the standalone
  binutils `as` to turn that into the `.o`.
- **After (`-fintegrated-as`):** cc1plus captures its own assembly output in an
  in-memory buffer (`open_memstream` in `gcc/toplev.cc`) and hands it to an
  embedded GAS via `gas_assemble_buffer()` (linked in as `libgas.a`), which
  writes the `.o` in-process. The driver's spec no longer invokes `as`.

The flag is **off by default**; without it GCC behaves exactly as before. The
in-process object output is **byte-identical** to the conventional
`cc1plus | as --64` pipeline (verified by sha256 across trivial, heavy-C++,
`-g`, and inline-`asm()` translation units).

The embedded assembler entry point `gas_assemble_buffer()` turns an in-memory
GAS-syntax assembly buffer into an ELF `.o` with no subprocess, and never exits
the host process: an assembler error is caught and returned as a non-zero
status instead of terminating cc1plus.

## How it fits together

- `gcc/` side (this fork): a `-fintegrated-as` flag in `gcc/common.opt`, the
  `open_memstream` capture + `gas_assemble_buffer()` call in `gcc/toplev.cc`
  (declared in `gcc/gas-embed.h`), a 2-hunk spec change in `gcc/gcc.cc` that
  drops the `as` invocation when the flag is set, and `Make-lang.in` changes
  (`gcc/{c,cp,lto}/Make-lang.in`) to link `libgas.a` into cc1/cc1plus/lto1.
- binutils side (NOT in this repo): the GAS source edits live in
  `gas-integrated-as.patch`, applied to a pristine binutils 2.42 `gas/` tree.
  binutils itself is reconstructible from the upstream 2.42 release plus that
  patch, so only the patch is committed here.

## What's in this directory

| File | What it is |
|------|------------|
| `gas-integrated-as.patch` | **The comprehensive binutils gas patch.** Reproduces the full set of gas edits (the 5 modified files below) plus the two new files `embed.c`/`embed.h`. Apply with `patch -p1` from a pristine binutils 2.42 `gas/` directory. This is the canonical, recoverable form of the binutils-side work. |
| `embed.c` | The new GAS entry point `gas_assemble_buffer()` (full source). Standalone copy of `gas/embed.c` (also contained in the patch above). |
| `embed.h` | Its public declaration. Standalone copy of `gas/embed.h` (also in the patch). |
| `gas_embed_test.c` | Standalone harness exercising `gas_assemble_buffer()` end to end. |
| `input-file.c.patch` | (Per-file patch, superseded by `gas-integrated-as.patch`.) Buffer-mode input (read assembly from memory, scrubber path preserved). |
| `input-file.h.patch` | (Per-file.) Declares `input_file_set_buffer()`. |
| `messages.c.patch` | (Per-file.) De-fatalize: `as_fatal`/`as_abort` `longjmp` instead of `xexit` in embed mode. |
| `as.h.patch` | (Per-file.) `#include <setjmp.h>` + `extern jmp_buf gas_fatal_jmp; extern int gas_in_embed;`. |
| `tc-i386.c.patch` | (Per-file.) `i386_set_default_arch_64()` runtime setter (the `--64` effect). |
| `build-libgas.sh` | Rebuilds `libgas.a` and the standalone test harness from the built tree. |

## The entry point

```c
/* gas/embed.h */
int gas_assemble_buffer (const char *asm_buf, size_t len, const char *out_obj);
```

Returns 0 on success, non-zero on any assembler error. Never exits the host
process: an `as_fatal`/`as_abort` condition is caught (via `setjmp`/`longjmp`)
and turned into a returned error. One-shot per process (GAS keeps file-scope
state that is not re-zeroed; cc1plus forks a fresh process per TU, so this is
fine).

Its body is `gas/as.c:main()` minus argv parsing and minus the `xexit()`s. The
two `static` helpers `main()` calls -- `gas_init()` and
`perform_an_assembly_pass()` -- are replicated verbatim in `embed.c` (they are
`static` in `as.c`, so they cannot be called; their bodies are copied). Keep
`embed.c` in sync with `as.c` if that init/pass/finalize sequence changes.

### How the COMMON globals are provided

`as.c` is the single translation unit where GAS's `COMMON` globals
(`text_section`, `now_seg`, `out_file_name`, all the `flag_*`, ...) and a few
directly-initialised ones (`debug_type`, `max_macro_nest`, ...) are *defined*.
We cannot link `as.o` because it defines `main` (clashes with cc1plus's
`gcc/main.cc`). Instead of hand-redeclaring ~45 globals (error-prone, drifts
with binutils), we **recompile `as.c` with `-Dmain=gas_unused_main`** into
`as-embed.o` and link THAT. This keeps every global definition with its exact
initializer, automatically in sync with binutils; the renamed `main` is dead
code that is never called. `embed.o` provides `gas_assemble_buffer`.

### x86-64 selection

`gas_assemble_buffer` calls `i386_set_default_arch_64()` (added in
`tc-i386.c`), which runs exactly the `OPTION_64` (`--64`) code path:
`default_arch = "x86_64"` after validating `bfd_target_list()`. This must run
before `output_file_create` (which invokes `TARGET_FORMAT` =
`i386_target_format()` -> `"elf64-x86-64"` + 64-bit code).

## Combined-tree setup recipe (from Stage 1)

GCC-14's top-level `Makefile.def` already lists `bfd`, `opcodes`, `binutils`,
`gas`, `libctf`, `libsframe` as host modules, so they build automatically once
their dirs are present. Setup is two sets of symlinks into the GCC tree
(`$GCC` = `/home/user/gcc-14`, `$BU` = `/home/user/binutils-2.42`):

1. Top-level module dirs (binutils-only; GCC lacks them):
   ```sh
   for d in bfd opcodes gas libsframe libctf; do ln -s "$BU/$d" "$GCC/$d"; done
   ```
   Do NOT symlink `config/`, `include/`, `libiberty/`, `zlib/` -- the combined
   tree must share GCC's copies. (`--with-system-zlib` avoids needing zlib/.)

2. The skew fix: binutils-only headers symlinked into GCC's shared `include/`.
   bfd/opcodes/gas `#include` ~30 binutils headers GCC-14's `include/` does not
   ship (`bfdlink.h`, `dis-asm.h`, `sframe.h`, `sframe-api.h`, `ctf-api.h`,
   `diagnostics.h`, `alloca-conf.h`, `binary-io.h`, the `fopen-*.h`, and the
   subdirs `elf/ opcode/ coff/ cgen/ mach-o/ aout/ som/ vms/ sim/`, ...). The
   exact set is "every entry in `comm -13 <(ls $GCC/include) <(ls $BU/include)`
   minus ChangeLog*/MAINTAINERS". The *overlapping* headers (`libiberty.h`,
   `demangle.h`, `ansidecl.h`, ...) are byte-identical between GCC-14 and
   binutils 2.42, so GCC's stay authoritative -- no libiberty-interface skew.

Configure + build (out-of-tree, `$BUILD` = `/home/user/gcc-build-gas`):
```sh
mkdir -p "$BUILD" && cd "$BUILD"
"$GCC"/configure --disable-bootstrap --enable-languages=c,c++ --disable-multilib \
  --with-system-zlib --disable-nls --disable-werror \
  --target=x86_64-linux-gnu --host=x86_64-linux-gnu --build=x86_64-linux-gnu \
  MAKEINFO=true
make all-libiberty -j"$(nproc)" MAKEINFO=true
make all-bfd all-opcodes all-gas -j"$(nproc)" MAKEINFO=true
```

## Applying the binutils gas source changes

From a pristine binutils 2.42 `gas/` directory, apply the single comprehensive
patch (it modifies `as.h`, `input-file.c`, `input-file.h`, `messages.c`,
`config/tc-i386.c` and creates `embed.c`/`embed.h`):
```sh
cd <binutils>/gas
patch -p1 < .../gas-integrated-as.patch
```
(The older per-file `*.patch` files are kept for reference and apply the same
edits piecemeal; the standalone `embed.c`/`embed.h` copies in this directory are
identical to the ones the comprehensive patch creates.)

Then rebuild: `cd $BUILD && make all-gas -j"$(nproc)" MAKEINFO=true`, and build
`embed.o` + `libgas.a` with `build-libgas.sh` (it captures the exact compile
flags from the gas Makefile).

## Building libgas.a and the test harness

See `build-libgas.sh`. In short, from `$BUILD/gas`:
- compile `embed.o` and `as-embed.o` (= `as.c` with `-Dmain=gas_unused_main`)
  using the same flags `make` uses for the other `gas/*.o`;
- `ar rcs $BUILD/libgas.a <all gas *.o except as.o, plus as-embed.o, plus
  config/{tc-i386,obj-elf,atof-ieee}.o>`.

Link a consumer (the Stage 2 harness, and Stage 3's cc1plus) with:
```
<consumer>.o \
  $BUILD/libgas.a \
  $BUILD/bfd/.libs/libbfd.a \
  $BUILD/opcodes/libopcodes.a \
  $BUILD/libsframe/.libs/libsframe.a \
  $BUILD/libiberty/libiberty.a \
  -lz
```
(`-lzstd` is NOT needed for this config -- zstd was not detected/used.)

## Proof

Two layers of evidence:

1. **Embedded assembler in isolation.** For trivial / heavy-C++ / `-g` /
   inline-`asm()` TUs, the `.o` produced by `gas_assemble_buffer` is
   sha256-identical to `as --64`'s output. A deliberately broken TU (bad
   register, bogus directive, `.abort` -> `as_fatal`) makes
   `gas_assemble_buffer` return non-zero **without killing the process**.
2. **End to end through cc1plus.** With `-fintegrated-as`, cc1plus assembles
   in-process and writes the `.o` directly (no `as` subprocess), and the
   resulting object is byte-identical to the conventional cc1plus + `as`
   pipeline for the same inputs.
