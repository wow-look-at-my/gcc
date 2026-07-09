/* embed.c - in-process GAS assembler entry point for cc1plus.
   Copyright (C) 1987-2024 Free Software Foundation, Inc.

   This file is part of GAS, the GNU Assembler.

   GAS is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 3, or (at your option)
   any later version.

   GAS is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with GAS; see the file COPYING.  If not, write to the Free
   Software Foundation, 51 Franklin Street - Fifth Floor, Boston, MA
   02110-1301, USA.  */

/* gas_assemble_buffer() drives the assembler over an in-memory buffer and
   writes an ELF object, with no subprocess and no process exit.  Its body is
   gas/as.c:main() minus argv parsing and minus the xexit()s; the two static
   helpers main() calls (gas_init, perform_an_assembly_pass) are static in
   as.c, so their bodies are replicated here verbatim from as.c.  Keep this in
   sync with as.c if that init/pass/finalize sequence changes.  */

/* NOTE: do NOT #define COMMON here.  as.c owns the single definition of the
   COMMON globals (it #defines COMMON to empty); this file references them as
   extern (as.h sets COMMON to "extern").  Linking embed.o together with the
   other gas objects (minus as.o) therefore resolves them against as.c's...
   no, against the COMMON tentative definitions in as.o -- which is exactly
   why as.o is dropped from libgas.a and the COMMONs are provided here.  See
   the matching tentative definitions block below.  */

#include "as.h"
#include "subsegs.h"
#include "output-file.h"
#include "sb.h"
#include "macro.h"
#include "dwarf2dbg.h"
#include "dw2gencfi.h"
#include "codeview.h"
#include "bfdver.h"
#include "write.h"
#include "ginsn.h"
#include "input-file.h"
#include "embed.h"

#ifdef HAVE_ITBL_CPU
#include "itbl-ops.h"
#else
#define itbl_init()
#endif

/* Runtime x86-64 (64-bit ELF) target selection, defined in config/tc-i386.c.
   Equivalent to the assembler's "--64" command-line option; must run before
   output_file_create (which invokes TARGET_FORMAT).  */
extern void i386_set_default_arch_64 (void);

int
gas_assemble_buffer (const char *asm_buf, size_t len, const char *out_obj)
{
  /* Modified between setjmp and a possible longjmp, so it must be volatile.
     0 = fault would occur before/while building the object (clean it up);
     1 = we have reached explicit teardown (do not re-close on a fault).  */
  volatile int phase = 0;
#ifndef OBJ_MACH_O
  flagword applicable;
#endif

  /* --- gas_early_init() (as.c:1276), MINUS signal_init (cc1plus owns the
     signal handlers), MINUS expandargv (no argv) and the locale setup (it
     affects only diagnostic text, not the emitted object).  --- */
  hex_init ();
  if (bfd_init () != BFD_INIT_MAGIC)
    return -1;				/* was as_fatal ("libbfd ABI mismatch") */

  obstack_begin (&notes, chunksize);
  xmalloc_set_program_name ("gas-embed");
  bfd_set_error_program_name ("gas-embed");

  init_include_dir ();

  /* --- target: x86-64 ELF, the runtime "--64" effect (tc-i386.c).  Must
     precede output_file_create.  --- */
  i386_set_default_arch_64 ();

  out_file_name = out_obj;		/* as.h:357 */

  /* From here on a fatal assembler condition must come back to us rather than
     kill the host process.  Arm the landing pad BEFORE any work that can call
     as_fatal/as_abort (output_file_create, md_begin, the parse, write...).  */
  gas_in_embed = 1;
  if (setjmp (gas_fatal_jmp))
    {
      /* as_fatal / as_abort fired and longjmp'd here.  The diagnostic has
	 already been printed.  Disable embed mode so any further fatal (e.g.
	 from the cleanup below) xexits rather than re-entering this landing
	 pad.  */
      gas_in_embed = 0;
      if (phase == 0 && stdoutput != NULL)
	{
	  keep_it = 0;
	  output_file_close ();		/* flush/free; unlinks the bad object */
	}
      return -1;
    }

  /* --- gas_init() (as.c:1324), EXCEPT the xatexit(output_file_close): we
     call teardown explicitly below instead of via the atexit chain.  --- */
  symbol_begin ();
  frag_init ();
  subsegs_begin ();
  read_begin ();
  input_scrub_begin ();
  expr_begin ();
  eh_begin ();

  macro_init ();

  dwarf2_init ();

  local_symbol_make (".gasversion.", absolute_section,
		     &predefined_address_frag, BFD_VERSION / 10000UL);

  output_file_create (out_file_name);
  gas_assert (stdoutput != 0);

  dot_symbol_init ();

#ifdef tc_init_after_args
  tc_init_after_args ();
#endif

  itbl_init ();

  /* --- input: feed the in-memory buffer through the normal file path
     (preprocess + do_scrub_chars), so #APP/#NO_APP behave identically to the
     standalone assembler.  --- */
  input_file_set_buffer (asm_buf, len);

  /* --- perform_an_assembly_pass() (as.c:1202), single buffer "file".  --- */
  need_pass_2 = 0;

#ifndef OBJ_MACH_O
  /* Create the standard sections, and those the assembler uses internally.  */
  text_section = subseg_new (TEXT_SECTION_NAME, 0);
  data_section = subseg_new (DATA_SECTION_NAME, 0);
  bss_section = subseg_new (BSS_SECTION_NAME, 0);
  /* @@ FIXME -- we're setting the RELOC flag so that sections are assumed
     to have relocs, otherwise we don't find out in time.  */
  applicable = bfd_applicable_section_flags (stdoutput);
  bfd_set_section_flags (text_section,
			 applicable & (SEC_ALLOC | SEC_LOAD | SEC_RELOC
				       | SEC_CODE | SEC_READONLY));
  bfd_set_section_flags (data_section,
			 applicable & (SEC_ALLOC | SEC_LOAD | SEC_RELOC
				       | SEC_DATA));
  bfd_set_section_flags (bss_section, applicable & SEC_ALLOC);
  seg_info (bss_section)->bss = 1;
#endif
  subseg_new (BFD_ABS_SECTION_NAME, 0);
  subseg_new (BFD_UND_SECTION_NAME, 0);
  reg_section = subseg_new ("*GAS `reg' section*", 0);
  expr_section = subseg_new ("*GAS `expr' section*", 0);

#ifndef OBJ_MACH_O
  subseg_set (text_section, 0);
#endif

  /* This may add symbol table entries, which requires having an open BFD,
     and sections already created.  */
  md_begin ();

#ifdef obj_begin
  obj_begin ();
#endif

  read_a_source_file ("<gas-embed>");

  /* --- finalize (as.c:1441-1468).  --- */
  cond_finish_check (-1);

#ifdef md_finish
  md_finish ();
#endif

#if defined OBJ_ELF || defined OBJ_MAYBE_ELF
  if ((flag_execstack || flag_noexecstack)
      && OUTPUT_FLAVOR == bfd_target_elf_flavour)
    {
      segT gnustack;

      gnustack = subseg_new (".note.GNU-stack", 0);
      bfd_set_section_flags (gnustack,
			     SEC_READONLY | (flag_execstack ? SEC_CODE : 0));

    }
#endif

  codeview_finish ();

  /* If we've been collecting dwarf2 .debug_line info, either for assembly
     debugging or on behalf of the compiler, emit it now.  */
  dwarf2_finish ();

  /* If we constructed dwarf2 .eh_frame info, either via .cfi directives from
     the user or by the backend, emit it now.  */
  cfi_finish ();

  keep_it = 0;
  if (seen_at_least_1_file ())
    {
      write_object_file ();		/* write.c:2126 -- THE OUTPUT */

      if (had_errors () == 0)
	keep_it = 1;
      else if (flag_always_generate_output)
	keep_it = 1;
    }

  /* --- teardown.  output_file_close() does bfd_close(stdoutput) (which
     flushes the .o), nulls now_seg/stdoutput, unlinks the file if !keep_it,
     and runs md_end/obj_end/macro_end/expr_end/read_end/symbol_end/
     subsegs_end.  It cannot be skipped.  --- */
  phase = 1;

  input_scrub_end ();

  output_file_close ();

  gas_in_embed = 0;
  return had_errors () ? -1 : 0;
}
