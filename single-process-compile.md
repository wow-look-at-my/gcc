# Single-Process Compilation for GCC

## Goal

One OS process per compile. `g++ foo.cc -c` should be a single process from source
to `.o`, not the current three (driver -> cc1plus -> as). The motivation is not the
fork/exec cost; it is that each process boundary forces a **lossy serialization** --
today, textual assembly -- that discards structured data the next stage must
reconstruct. Collapsing to one process is the enabling change for a class of
optimizations that are impossible today because the relevant data lives on the wrong
side of a text pipe.

## Today's architecture (from source)

`wow-look-at-my/gcc` @ `develop-matt/v14`. A single `g++ foo.cc -c` is three processes:

1. **driver** (`g++` -> `gcc/gcc.cc`) -- interprets spec strings, forks the stages.
2. **cc1plus** -- the actual C++ compiler; emits **textual** assembly.
3. **as** (GNU binutils) -- re-parses that text into a `.o`.

(Linking adds `collect2`/`ld`, but that is once per binary, not per compile.)

How the driver wires it:

- The pipeline is **spec-driven**, not hardcoded. The `@c++` spec
  (`gcc/cp/lang-specs.h:102`) expands to `cc1plus ... %(invoke_as)`.
- `invoke_as` (`gcc/gcc.cc:1310`) appends `-o %|.s |\n as %(asm_options) ...` -- the
  literal ` | ` is the second pipeline stage.
- `execute()` (`gcc/gcc.cc:3249`) splits the argbuffer on `|` into N commands and
  spawns each: `pex_init`/`pex_run` (`gcc.cc:3419`/`3431`) ->
  `pex_unix_exec_child` (`libiberty/pex-unix.c:735`) -> `vfork()` (`pex-unix.c:790`)
  -> `execvp`/`execv` (`pex-unix.c:857`/`862`).
- Default (no `-pipe`): cc1plus writes a temp `.s` via `make_temp_file`
  (`gcc.cc:6492`); `as` reads it back. `-pipe` (`common.opt:3700`) swaps the temp file
  for an OS pipe (`gcc.cc:6316`) but **keeps two processes**.

**GCC has no integrated assembler.** A whole-tree search for `integrated-as` /
`fno-integrated-as` / `integrated_assembler` / `cc1as` returns zero hits; the only
"integrated" option is `-no-integrated-cpp` (the preprocessor). `as` is
unconditionally an external program (`gcc.cc:1492`). Clang, by contrast, has emitted
object code in-process via its MC layer since ~2010.

## Why the cc1plus->as boundary is the one that matters

By the time `final_scan_insn` runs in `gcc/final.cc`, cc1plus already holds the
**fully resolved** instruction:

- `insn_code_number` -- the exact `.md` pattern chosen,
- `recog_data.operand[]` -- the register-allocated operands,
- `which_alternative` -- the selected asm alternative.

That is precisely what an encoder needs to emit bytes. Instead, `get_insn_template`
(`final.cc:2021`) fetches a printf-style template string (literal asm like
`"mov{<imodesuffix>}\t{%1, %0|%0, %1}"`, `gcc/config/i386/i386.md:3359`) and
`output_asm_insn` (`final.cc:3411`) renders it to ASCII -- one
`putc (c, asm_out_file)` per character -- into a `FILE *` (`gcc/output.h:321`). Then
`as` lexes that text, parses it against its own instruction tables, re-resolves every
register and immediate, and re-derives the encoding.

**The instruction is selected twice**, and the structured form is destroyed in
between. The text is pure serialization tax -- and, more importantly, a one-way wall:
nothing structured crosses it.

## What one process unlocks (the actual prize)

These are capabilities, not microseconds. Each is impossible today because the data
is on the wrong side of the text boundary:

1. **Exact instruction sizes feed back into the compiler.** Branch shortening,
   alignment/padding, `-Os` size decisions, and jump-range selection run today on
   *estimated* sizes because the real encoded sizes only exist after `as` relaxes.
   In-process, `final.cc` (and `shorten_branches`) can use the encoder's real sizes
   -> tighter code, correct-first-time branch forms.
2. **Section layout and relaxation are visible to codegen.** The assembler's fragment
   layout / relaxation decisions (today entirely inside `as`) become available to the
   compiler, enabling co-optimization (e.g. choosing instruction forms from real
   offsets).
3. **One symbol table, one string pool.** Today every identifier is interned in
   cc1plus, serialized to text, and re-interned in `as`. In-process they are shared --
   the compiler's `line_table`/symtab *is* the assembler's.
4. **Object bytes straight to memory.** The `.o` can go directly to an in-process
   linker, to the existing `-fcompile-cache`, or to an LTO pipeline without touching
   disk or text. The compile cache can key/store **structured** object data instead of
   `.s` text, and a cache hit skips the assembler entirely.
5. **Diagnostics share source locations through emission.** Assembler-level errors
   (today decoupled) can map back through the compiler's `line_table`.
6. **Inline asm shares the assembler context** -- symbol references can be checked
   against the live symtab.

The driver->cc1plus fork is the *lesser* boundary: it mainly repeats startup
(`general_init`/`backend_init` in `gcc/toplev.cc` rebuild GC, string pool, line
tables, the pass manager, the instruction recognizer, IRA tables every TU). It is
worth folding too, but it does not gate the optimizations above -- the assembler seam
does.

## Path to one process

### Stage A -- in-process assembler (the tractable first step)

Fold the assemble step into cc1plus so no second process is spawned. cc1plus still
emits text, but to an in-memory buffer that an in-process assembler consumes,
producing the `.o` directly.

- **The reentrancy objection mostly dissolves.** The standard reason "you can't call
  gas as a library" is that gas is a `main()` built on process-global mutable state
  (symbol table, frags, current section, parser state). But with one TU per process,
  those globals only need to be valid for a **single** assemble, after which the
  process exits -- so no reset/re-entrancy is required. You need gas callable *once*,
  not repeatedly.
- **Seam to change:** replace the `%(invoke_as)` text-pipe in `invoke_as`
  (`gcc.cc:1310`) / the `execute()` pipeline split (`gcc.cc:3249`) with an in-process
  call when the input came from our own cc1plus. Concretely: after `compile_file()`
  finishes writing `asm_out_file`, hand the buffer to an
  `assemble_in_process(asm_text, out_obj_path)` entry point instead of closing the
  file and letting the driver fork `as`.
- **Build implication:** link the assembler's object-producing core (a `libgas`-style
  extraction of binutils `gas/` + `libbfd`/`libopcodes`) into cc1plus. This is the
  real work of Stage A: carving a callable `as_main(buffer) -> object` out of gas
  without dragging in its CLI/`exit` assumptions.
- **What it buys:** literally one process per compile; no fork/exec of `as`; no temp
  `.s`; no `/tmp` pressure. **What it does not buy:** the text round-trip and double
  instruction selection remain (cc1plus formats ASCII, the in-process gas re-parses
  it). It is the structural foothold, not the destination.

### Stage B -- direct object emission (the destination)

A GCC-side MC layer: `final.cc` emits object bytes from the structured insn data, with
no text in between. This is the clang model (MCInst -> MCCodeEmitter -> object writer).

- Requires per-target instruction encoders -- the byte/ModRM/REX knowledge that today
  exists only inside `as`. This is the large, multi-target cost, and the reason GCC
  never built it.
- Could be staged target-by-target (x86-64 first), with a fallback to the Stage-A text
  path for un-converted targets -- mirroring clang's `-fno-integrated-as` fallback.
- This is what delivers the "what one process unlocks" list in full: no second
  selection, real sizes available pre-emission, structured object in memory.

### The driver->cc1plus fork (separate axis)

Folding the driver into cc1plus (or persistent/zygote cc1plus) removes the per-TU
startup repetition. It is complementary but independent of the assembler seam; it does
not unlock the codegen optimizations. Ties into the existing zygote/startup
investigation.

## Recommendation / sequencing

1. **Stage A prototype** on x86-64 Linux: extract a one-shot `libgas` and call it
   in-process from cc1plus, gated behind a flag (e.g. `-fintegrated-as`), falling back
   to the fork path otherwise. This proves "one process per compile" end-to-end and is
   the load-bearing engineering risk (carving gas out of its global/CLI assumptions).
2. **Wire the compile cache** to the in-process object output so a hit skips assembly.
3. **Stage B** incrementally: replace the text->gas path with direct encoding, target
   by target, reusing the Stage-A integration points and fallback.

## Open questions

- Carving gas: how much of `gas/` + `libbfd` global state can be tolerated for a
  single-shot call vs. must be encapsulated? (One-shot lifetime is the lever.)
- Build coupling: gas and gcc are both GPLv3 (same project), so linking is not a
  license issue, but the binutils/gcc build coupling needs a plan (submodule? vendored
  subset?).
- Flag/UX: mirror clang's `-fintegrated-as` / `-fno-integrated-as` for opt-out and
  target gating.
- Stage B encoder source of truth: hand-written per-target tables vs. generating from
  the `.md` + a new encoding description.
