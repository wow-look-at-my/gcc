/* gas-embed.h - declaration of the in-process GAS assembler entry point.
   Copyright (C) 2024 Free Software Foundation, Inc.

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

#ifndef GCC_GAS_EMBED_H
#define GCC_GAS_EMBED_H

/* gas_assemble_buffer() is a C function compiled into libgas.a (gas/embed.c)
   and linked into cc1plus.  It assembles the GAS-syntax assembly text in
   [buf, buf + len) and writes an ELF object file to OUT_OBJ, with no
   subprocess and without ever exiting the host process.  Returns 0 on
   success, non-zero on any assembler error.  One-shot per process.  */

extern "C" int gas_assemble_buffer (const char *buf, size_t len,
				    const char *out_obj);

#endif /* GCC_GAS_EMBED_H */
