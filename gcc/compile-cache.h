/* In-compiler compilation cache (structured binary cache objects).

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
   of a single C/C++ translation unit, plus the back-end diagnostics emitted
   while producing it.  It lives entirely inside the compiler; there is NO
   driver protocol change.  The driver forwards "-fcompile-cache=DIR" through
   its existing "%{f*}" wildcard, and cc1/cc1plus do all the work (the dir
   can equally come from the GCC_COMPILE_CACHE_DIR environment variable).

   On a miss the produced assembly and the captured back-end diagnostics are
   written as ONE little-endian binary object per entry, at
   "DIR/<2hex>/<rest>.bin" (no sidecar files).  The object has a fixed 128-byte
   header (magic "GCCCACHE", format_version, section offsets/lengths, the raw
   SHA-1 key, the executable checksum, warning/error counts, and offsets to a
   set of metadata strings), an inputs table (one record per included file:
   raw SHA-1 + size + path offset), a string area (offset-referenced,
   length-prefixed + NUL strings), the assembly bytes, and the diagnostic
   bytes.  On a hit the object is read once; if the magic/format_version do not
   match it is ignored (treated as a miss -- self-healing).  The stored
   assembly is written verbatim to the .o-bound asm output (so "as" sees
   byte-identical input), and the stored diagnostics are replayed to stderr
   with the warning/error counts folded into the global counters for -Werror
   and exit-status parity.

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
          itself) and, if the user did not pass -frandom-seed, pins a single
          fixed -frandom-seed so codegen name generation is reproducible
          across runs -- otherwise two runs of the same TU would emit
          byte-different assembly and never hit the cache.

     2. compile_cache_try_serve (pfile)
          Called AFTER the parse loop has populated the include closure,
          with parse_in.  Computes the key (and gathers the object metadata
          from the same include-closure walk).  On a hit, writes the cached
          assembly into the open asm_out_file, replays the cached back-end
          diagnostics to stderr, folds the stored warning/error counts into
          the global counters, and returns true (caller skips the back-end
          via compile_cache_hit_p ()).  On a miss it stashes the key + metadata
          and installs a capturing+teeing sink on global_dc so the back-end
          diagnostics are both printed (as usual) and recorded for storage.

     3. compile_cache_store ()
          Called from toplev.cc on a miss, AFTER finalize () has closed
          asm_out_file, so the freshly produced .s on disk is complete.
          Removes the capturing sink, serializes {metadata, assembly,
          captured diagnostics} into one binary object, and atomically
          publishes it into the cache under the stashed key.
   --------------------------------------------------------------------------- */

struct cpp_reader;

/* True if caching is configured and this compilation is eligible (right
   language, single input, not -E/-fsyntax-only/LTO/PCH/coverage/plugins/...).
   Cheap; the result is cached internally.  */
extern bool compile_cache_enabled_p (void);

/* True if a cache directory is configured at all (-fcompile-cache= or
   GCC_COMPILE_CACHE_DIR), without any eligibility checks and without
   latching compile_cache_enabled_p ()'s internal tri-state.  Safe to call
   early (right after option decoding), when the full gating state
   (lang hooks, asm_file_name, PCH mode, ...) does not exist yet.  Used to
   decide whether libcpp should record per-file content digests at read
   time (cpp_opts->hash_file_contents).  */
extern bool compile_cache_configured_p (void);

/* Record PCH state (PCH_ACTIVE true when creating or consuming a PCH) and,
   if the user did not set -frandom-seed, pin it to a single fixed value so
   codegen is reproducible and the cache can ever hit.  No-op when caching is
   disabled or the user already passed -frandom-seed.  Call BEFORE parsing,
   from the C/C++ front end.  */
extern void compile_cache_init_determinism (bool pch_active);

/* Compute the cache key from the now-complete include closure (walked via
   PFILE) + options, gathering the object metadata along the way.  On a hit,
   write the cached assembly into the open asm_out_file, replay the cached
   back-end diagnostics, fold the stored counts into the global counters, and
   return true (caller should skip the back-end).  On a miss (or when disabled)
   return false; the key + metadata are stashed and a capturing sink is
   installed on global_dc so the back-end diagnostics are recorded for
   compile_cache_store ().  */
extern bool compile_cache_try_serve (cpp_reader *pfile);

/* Pre-parse manifest fast-path (Stage 5).  Call BEFORE the parse loop with the
   main source path.  Hashes the source + output-affecting options + include
   search paths into a manifest key, looks up the recorded include set(s) for
   that key, and re-resolves each header by its stored absolute path WITHOUT
   preprocessing or parsing (size+mtime stat shortcut, else content re-hash).
   If a recorded set fully matches and its object exists (and carries no
   front-end diagnostics), places the cached .o at the output, replays
   diagnostics, sets the hit flag, and returns true -- the caller MUST then
   skip the parse and the back-end entirely.  Returns false on any miss /
   mismatch / bypass (a TU using __has_include is always bypassed); the caller
   proceeds with a normal compile, which records/updates the manifest on the
   miss path.  No-op (returns false) when caching is disabled.  */
extern bool compile_cache_try_serve_manifest (cpp_reader *pfile,
					      const char *src_path);

/* Serialize {metadata, freshly produced assembly (asm_file_name's contents),
   captured back-end diagnostics} into one binary object and atomically publish
   it under the key from the preceding compile_cache_try_serve ().  Call on a
   miss, AFTER asm_out_file has been closed by finalize ().  Also removes the
   capturing sink installed on a miss.  No-op when disabled, on error, or if no
   key was computed.  */
extern void compile_cache_store (void);

/* True once compile_cache_try_serve () has reported a hit, i.e. the back-end
   was (or should be) skipped for this TU.  Used by the compile_file seam and
   to decide whether compile_cache_store () has anything to do.  */
extern bool compile_cache_hit_p (void);

#endif /* GCC_COMPILE_CACHE_H */
