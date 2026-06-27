/* In-compiler compilation cache (cache the assembly .s).

   Public interface.  Built as a back-end object (gcc/compile-cache.o in
   OBJS) so toplev.cc's references resolve in every *1 compiler; the cache
   is a fast no-op for non-C/C++ front-ends.

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

#ifndef GCC_COMPILE_CACHE_H
#define GCC_COMPILE_CACHE_H

/* ---------------------------------------------------------------------------
   Overview
   ---------------------------------------------------------------------------

   This is an in-process, content-addressed cache of the *assembly* output
   of a single C/C++ translation unit.  It lives entirely inside the
   compiler; there is NO driver protocol change.  The driver forwards
   "-fcompile-cache=DIR" through its existing "%{f*}" wildcard, and
   cc1/cc1plus do all the work (the dir can equally come from the
   GCC_COMPILE_CACHE_DIR environment variable).

   The cache key is a SHA-1 over everything that can change the generated
   assembly for this TU:
     - the compiler binary's own checksum (executable_checksum[16]);
     - the source language and dialect (lang_hooks.name);
     - the full preprocessed source closure (every file libcpp stacked,
       identified by path + exact bytes), gathered after parsing through the
       libcpp accessor cpp_foreach_included_file ();
     - the codegen/ABI-relevant subset of the command line.

   Call sites (see the edits in c-opts.cc / toplev.cc):

     1. compile_cache_init_determinism (pch_active)
          Called once, BEFORE parsing.  Records whether a PCH pipeline is
          active (a C/C++-specific fact the back-end object cannot see for
          itself) and, if the user did not pass -frandom-seed, pins a
          deterministic, TU-unique seed so codegen name generation is
          reproducible across runs -- otherwise two runs of the same TU
          would emit byte-different .s and never hit the cache.

     2. compile_cache_try_serve (pfile)
          Called AFTER the parse loop has populated the include closure,
          with parse_in.  Computes the key; on a hit, replaces the open
          asm_out_file with the cached bytes and returns true (caller skips
          the back-end via compile_cache_hit_p ()).  On a miss it stashes
          the key for a later compile_cache_store ().

     3. compile_cache_store ()
          Called from toplev.cc on a miss, AFTER finalize () has closed
          asm_out_file, so the freshly produced .s on disk is complete.
          Atomically publishes it into the cache under the stashed key.
   --------------------------------------------------------------------------- */

struct cpp_reader;

/* True if caching is configured and this compilation is eligible (right
   language, single input, not -E/-fsyntax-only/LTO/PCH/coverage/plugins/...).
   Cheap; the result is cached internally.  */
extern bool compile_cache_enabled_p (void);

/* Record PCH state (PCH_ACTIVE true when creating or consuming a PCH) and,
   if the user did not set -frandom-seed, pin it to a value derived from
   {main input path, codegen-relevant flags, executable_checksum} so codegen
   is reproducible and the cache can ever hit.  No-op when caching is
   disabled or the user already passed -frandom-seed.  Call BEFORE parsing,
   from the C/C++ front end.  */
extern void compile_cache_init_determinism (bool pch_active);

/* Compute the cache key from the now-complete include closure (walked via
   PFILE) + options.  On a hit, write the cached assembly into the open
   asm_out_file and return true (caller should skip the back-end).  On a miss
   (or when disabled) return false; the key is stashed for compile_cache_store
   ().  */
extern bool compile_cache_try_serve (cpp_reader *pfile);

/* Store the freshly produced assembly (asm_file_name's contents) into the
   cache under the key from the preceding compile_cache_try_serve ().  Call
   on a miss, AFTER asm_out_file has been closed by finalize ().  No-op when
   disabled, on error, or if no key was computed.  */
extern void compile_cache_store (void);

/* True once compile_cache_try_serve () has reported a hit, i.e. the back-end
   was (or should be) skipped for this TU.  Used by the compile_file seam and
   to decide whether compile_cache_store () has anything to do.  */
extern bool compile_cache_hit_p (void);

#endif /* GCC_COMPILE_CACHE_H */
