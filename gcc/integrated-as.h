/* Declaration of the in-process ("integrated") assembler entry point.
   This folds the GNU assembler (gas) into cc1plus so that a compile to an
   object file is a single process: no forked `as`, no temporary .s file.
   Copyright (C) 2026 Free Software Foundation, Inc.

This file is part of GCC.

GCC is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free
Software Foundation; either version 3, or (at your option) any later
version.

GCC is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or
FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
for more details.

You should have received a copy of the GNU General Public License
along with GCC; see the file COPYING3.  If not see
<http://www.gnu.org/licenses/>.  */

#ifndef GCC_INTEGRATED_AS_H
#define GCC_INTEGRATED_AS_H

/* Assemble LEN bytes of assembly text at ASM_TEXT, writing the resulting
   relocatable object file to OUT_OBJ_PATH.  Implemented in libgas (carved
   from GNU as); see binutils gas/as-lib.{h,c}.  Runs the minimal gas
   init/assemble/write subset of as's main() exactly once per process and
   relies on process exit for teardown of all gas globals.  TARGET_ARGS is
   reserved for future per-call target selection and may be NULL.  Returns 0
   on success, non-zero on an assembly error or fatal/abort condition.

   This must match gas/as-lib.h's declaration exactly.  It is declared
   extern "C" because libgas is C and cc1plus is C++.  */

extern "C" int gas_assemble_buffer (const char *asm_text, size_t len,
				    const char *out_obj_path,
				    const char *const *target_args);

#endif /* GCC_INTEGRATED_AS_H */
