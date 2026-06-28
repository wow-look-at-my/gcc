/* embed.h - in-process GAS assembler entry point for cc1plus.
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

#ifndef GAS_EMBED_H
#define GAS_EMBED_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Assemble the GAS-syntax assembly text in [asm_buf, asm_buf + len) and write
   an ELF object file to OUT_OBJ.  Targets x86-64 ELF.  Performs no subprocess
   and never exits the host process: returns 0 on success, non-zero on any
   assembler error (a fatal assembler condition is caught and turned into a
   returned error rather than killing the caller).

   One-shot per process: GAS keeps file-scope state that is not re-zeroed, so
   call this at most once in a process (cc1plus forks a fresh process per TU).  */
int gas_assemble_buffer (const char *asm_buf, size_t len, const char *out_obj);

#ifdef __cplusplus
}
#endif

#endif /* GAS_EMBED_H */
