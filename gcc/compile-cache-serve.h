/* In-compiler compilation cache: driver-usable serve unit (no libcpp).

   Public interface for the SHARED serve code that answers a warm cache HIT.
   It is deliberately free of any libcpp / front-end / back-end dependency so
   the SAME object compiles into BOTH:

     - the GCC driver (gcc.cc -> xgcc / xg++), where a hit is answered WITHOUT
       ever spawning cc1/cc1plus or as; and
     - the C/C++ compiler (cc1/cc1plus), as the implementation the post-parse
       object-key path and the pre-parse manifest path call.

   The libcpp-dependent STORE path (the include-closure walk that builds the
   manifest and object on a miss) stays in compile-cache.cc.  This file only
   reads what that path wrote.

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

#ifndef GCC_COMPILE_CACHE_SERVE_H
#define GCC_COMPILE_CACHE_SERVE_H

struct cl_decoded_option;

/* Everything the serve unit needs, supplied by the caller from its own world.
   The driver and cc1plus each fill this from their own globals; the serve code
   itself touches no global compiler state, so the manifest key it computes is
   determined ENTIRELY by these fields -- which is what lets the driver and
   cc1plus agree on the key (the #1 correctness requirement).  */
struct cc_serve_ctx
{
  /* The resolved cache directory (-fcompile-cache=DIR / GCC_COMPILE_CACHE_DIR).  */
  const char *cache_dir;

  /* The 16-byte compiler fingerprint that the OBJECT was stored under.  cc1plus
     passes its own executable_checksum; the driver reads it from the cache's
     "compiler-id" sidecar (written by cc1plus on a miss-store).  Folded into
     the manifest key so a recompiled compiler invalidates the cache.  */
  const unsigned char *checksum;	/* 16 bytes */

  /* lang_hooks.name ("GNU C23" / "GNU C++23").  cc1plus passes its own; the
     driver reads it from the "compiler-id" sidecar.  Folded into the key.  */
  const char *lang_name;

  /* The decoded command-line options (the SAME cl_decoded_option array both
     sides build by running decode_cmdline_options_to_array over the cc1 argv).
     decoded[0] is the program-name slot and is skipped.  */
  const cl_decoded_option *decoded;
  unsigned int decoded_count;

  /* True when the produced bytes depend on source PATHS (i.e. -g is active, so
     paths bake into DWARF).  When true the main source path + cwd fold into the
     manifest key.  cc1plus passes debug_info_level > DINFO_LEVEL_NONE; the
     driver detects -g among the decoded options.  */
  bool paths_affect_output;

  /* Current working directory, used only when paths_affect_output.  */
  const char *cwd;

  /* GCC_COMPILE_CACHE_VERIFY=hash forces a full per-header content re-hash on a
     hit instead of the size+mtime stat shortcut.  */
  bool verify_hash;

  /* GCC_COMPILE_CACHE_DEBUG: emit "compile-cache: <action> <key12> <out>".  */
  bool debug;
};

/* Try to answer a warm hit for source SRC_PATH, placing the cached object at
   OUT_PATH, using CTX.  Computes the manifest key MK from the source bytes +
   CTX (identically on the driver and in cc1plus), reads the manifest, and for
   each recorded include set re-resolves every header by its stored absolute
   path (stat shortcut, else content re-hash) WITHOUT preprocessing or parsing.
   On the first set that fully matches AND whose object exists and carries no
   front-end diagnostics, places the cached .o at OUT_PATH (reflink->hardlink->
   copy ladder) and, if the object recorded any back-end diagnostics, replays
   them to stderr.

   Returns true on a served hit (caller must then NOT run the compiler).  On
   any miss / mismatch / unreadable input returns false (caller proceeds with a
   normal compile).  The caller is responsible for any warning/error-count
   bookkeeping it needs; on a driver hit there is no compiler process, so a hit
   simply means the .o is in place and the diagnostics (if any) were echoed.  */
extern bool compile_cache_serve_object (const cc_serve_ctx *ctx,
					const char *src_path,
					const char *out_path);

#endif /* GCC_COMPILE_CACHE_SERVE_H */
