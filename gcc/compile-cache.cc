/* In-compiler compilation cache (structured binary cache objects).

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

#include "config.h"
#include "system.h"
#include "coretypes.h"
#include "backend.h"		/* prerequisite for tree.h / rtl.h */
#include "target.h"
#include "rtl.h"
#include "tree.h"		/* enums needed by langhooks.h / output.h */
#include "options.h"		/* generated option vars + OPT_* enumerators
				   (incl. the compile_cache_dir macro) */
#include "opts.h"		/* cl_decoded_option, cl_options[], CL_* flags */
#include "flags.h"
#include "toplev.h"		/* save_decoded_options, set_random_seed,
				   get_src_pwd */
#include "output.h"		/* asm_out_file */
#include "langhooks.h"		/* lang_hooks.name */
#include "plugin.h"		/* plugins_active_p, flag_plugin_added */
#include "diagnostic.h"		/* global_dc, diagnostic_count, pp_buffer */
#include "diagnostic-core.h"	/* seen_error, fatal_error */
#include "../libcpp/include/cpplib.h"  /* cpp_foreach_included_file */
#include "sha1.h"
#include "compile-cache-format.h"	/* shared on-disk format + LE helpers */
#include "compile-cache-serve.h"	/* shared driver-usable serve unit */
#include "compile-cache.h"

/* The option predicates that build the manifest key are defined ONCE in the
   shared serve unit (compile-cache-serve.cc) so the driver and cc1plus compute
   an identical key.  Declared here (extern) for the store path's use.  */
extern bool cc_option_affects_output_p (const cl_decoded_option *decoded);
extern bool cc_option_is_search_path_p (const cl_decoded_option *decoded);

#include <sys/stat.h>
/* For the opportunistic reflink rung of cc_place_object().  Guarded so the
   feature compiles in only where the kernel headers expose FICLONE (Linux);
   elsewhere cc_place_object falls back to hardlink/copy.  The reflink block in
   cc_place_object is itself further guarded by "#ifdef FICLONE".  */
#if defined (__linux__)
# include <sys/ioctl.h>
# if defined (__has_include)
#  if __has_include (<linux/fs.h>)
#   include <linux/fs.h>
#  endif
# endif
#endif

/* Extended attributes carry the v3 per-object metadata on the cache .o.  When
   the platform lacks <sys/xattr.h> the store falls back to the minimal .bin
   sidecar and the serve reads it; CC_HAVE_XATTR gates the fast path.  */
#if defined (__has_include)
# if __has_include (<sys/xattr.h>)
#  include <sys/xattr.h>
#  define CC_HAVE_XATTR 1
# endif
#endif

/* The compiler binary's own 16-byte fingerprint.  Each front-end compiler is
   linked with its own generated <binary>-checksum.o (cc1-checksum.o,
   cc1plus-checksum.o, ...) defining a strong "executable_checksum".  But this
   object lives in the shared backend OBJS, which is also linked into binaries
   that have NO checksum object (e.g. lto1, lto-dump).  Provide a weak
   all-zero fallback so those still link; the strong per-binary definition
   overrides it in cc1/cc1plus, which are the only places the cache runs.
   Declared/defined here so this back-end object needs no c-family header.
   The explicit "extern" forces external linkage (a namespace-scope const in
   C++ is otherwise internal, which "weak" rejects).  */
extern const unsigned char executable_checksum[16]
  __attribute__ ((weak)) = { 0 };

/* ------------------------------------------------------------------------ */
/* Binary object + manifest format                                          */
/* ------------------------------------------------------------------------ */

/* The on-disk byte layout (object header/sections, inputs table, manifest
   header/entries, the compiler-id sidecar) and the little-endian load/store
   helpers + cc_hex live in compile-cache-format.h, shared with the driver-side
   serve unit so both describe identical bytes.  Each cache OBJECT is one
   little-endian "DIR/<2hex>/<rest>.bin" (header -> inputs table -> string area
   -> empty asm -> diagnostics) with its payload .o in a sidecar
   "DIR/<2hex>/<rest>.o".  A MANIFEST (ccache-style direct-mode index) is a
   separate object keyed by the manifest key MK that lists, per include set, the
   object key OK and the headers it depended on.  */

/* Forward declarations (defined later, used earlier).  */
static unsigned char *cc_read_file (const char *path, size_t *len);
static bool cc_place_object (const char *cached_o, const char *dst);

/* ------------------------------------------------------------------------ */
/* Configuration / state                                                    */
/* ------------------------------------------------------------------------ */

/* The cache directory comes from -fcompile-cache=DIR (Var(compile_cache_dir)
   in common.opt, surfaced as a macro by the generated options.h) or the
   GCC_COMPILE_CACHE_DIR environment variable.  Do NOT redeclare
   compile_cache_dir here -- options.h #defines it to a global_options field,
   which would turn a redeclaration into a syntax error.  */

/* Tri-state cache of compile_cache_enabled_p()'s answer:
   -1 = not yet computed, 0 = disabled, 1 = enabled.  */
static int cc_enabled = -1;

/* Resolved cache directory (compile_cache_dir, else env), or NULL.  */
static const char *cc_dir = NULL;

/* True when a PCH pipeline is active for this TU.  Set from the C/C++ front
   end (which can see flag_pch_preprocess / pch_file) before any gating.  */
static bool cc_pch_active = false;

/* The 40-char lowercase-hex SHA-1 key for this TU, computed by
   compile_cache_try_serve() and reused by compile_cache_store().  */
static char cc_key_hex[41];
/* The same key as 20 raw bytes (stored in the object header).  */
static unsigned char cc_key_raw[20];
static bool cc_key_valid = false;

/* Set once compile_cache_try_serve() reports a hit.  */
static bool cc_hit = false;

/* The 40-char lowercase-hex manifest key (MK) for this TU, computed by the
   pre-parse fast-path and reused by the miss-path manifest store.  */
static char cc_manifest_key_hex[41];
static bool cc_manifest_key_valid = false;

/* Number of {DK_WARNING, DK_WERROR} diagnostics already counted when the
   back-end capture started (i.e. emitted by the front end / parse).  Used to
   set CC_FLAG_HAD_FE_DIAG so the manifest fast-path never serves an object
   whose front-end diagnostics it would silently drop.  */
static int cc_fe_warnings = 0;
static int cc_fe_werrors = 0;

/* True if this TU evaluated __has_include / __has_include_next (captured from
   the cpp_reader at post-parse time).  When set, compile_cache_store () writes
   NO manifest entry, so the pre-parse fast-path never has a manifest to serve
   for this TU (its include closure is not a sound predictor -- a probed-but-
   not-included header can flip absent<->present without changing the closure).
   The post-parse object cache still applies.  */
static bool cc_tu_used_has_include = false;

/* ------------------------------------------------------------------------ */
/* Object metadata, gathered during key computation                         */
/* ------------------------------------------------------------------------ */

/* One included input file's identity for the inputs table.  */
struct cc_input
{
  char *path;			/* xstrdup'd path */
  uint64_t size;		/* byte count */
  uint64_t mtime;		/* st_mtime (seconds) for the stat shortcut */
  unsigned char hash[20];	/* raw SHA-1 of the bytes */
};

/* Metadata for the object being built on a miss.  All strings xstrdup'd /
   xmalloc'd and freed in cc_meta_clear().  */
struct cc_metadata
{
  char *source;			/* main input path */
  char *cwd;			/* current working directory */
  char *target;			/* target triple */
  char *language;		/* lang_hooks.name */
  char *options;		/* space-joined codegen/ABI option tokens */
  cc_input *inputs;		/* inputs table */
  unsigned input_count;
  unsigned input_cap;
};

static cc_metadata cc_meta;

static void
cc_meta_clear (void)
{
  free (cc_meta.source);
  free (cc_meta.cwd);
  free (cc_meta.target);
  free (cc_meta.language);
  free (cc_meta.options);
  for (unsigned i = 0; i < cc_meta.input_count; i++)
    free (cc_meta.inputs[i].path);
  free (cc_meta.inputs);
  memset (&cc_meta, 0, sizeof (cc_meta));
}

static void
cc_meta_add_input (const char *path, uint64_t size, uint64_t mtime,
		   const unsigned char hash[20])
{
  if (cc_meta.input_count == cc_meta.input_cap)
    {
      unsigned n = cc_meta.input_cap ? cc_meta.input_cap * 2 : 8;
      cc_meta.inputs
	= XRESIZEVEC (cc_input, cc_meta.inputs, n);
      cc_meta.input_cap = n;
    }
  cc_input *in = &cc_meta.inputs[cc_meta.input_count++];
  in->path = xstrdup (path);
  in->size = size;
  in->mtime = mtime;
  memcpy (in->hash, hash, 20);
}

/* ------------------------------------------------------------------------ */
/* Back-end diagnostic capture                                              */
/* ------------------------------------------------------------------------ */

/* While capturing, we point the diagnostic pretty-printer's output stream at
   a temp file so exactly the back-end diagnostic bytes are recorded.  At
   store time the bytes are echoed to stderr (so they still print on the miss)
   and serialized.  We also snapshot the warning/werror counts so the stored
   counts reflect only the back-end phase.

   We track DK_WARNING (plain warnings) and DK_WERROR (warnings promoted to
   errors by -Werror).  Real DK_ERRORs are not tracked here: a real back-end
   error makes seen_error() true, and compile_cache_store() bails on that, so
   a failed compile is never cached.  Restoring exactly {warningcount,
   werrorcount} on a hit reproduces both the "N warnings"/"M errors" summary
   line and the -Werror exit code (toplev exits non-zero on werrorcount).  */
static FILE *cc_diag_capture = NULL;	/* tmpfile() while capturing */
static FILE *cc_diag_saved_stream = NULL;	/* the printer's real stream */
static int cc_diag_base_warnings = 0;
static int cc_diag_base_werrors = 0;
static bool cc_capturing = false;

/* Install the capturing stream on global_dc's printer.  Called on a miss,
   right before the back-end phase runs.  */
static void
cc_begin_backend_capture (void)
{
  if (cc_capturing)
    return;
  if (!global_dc || !global_dc->printer)
    return;

  output_buffer *buf = pp_buffer (global_dc->printer);
  if (!buf)
    return;

  cc_diag_capture = tmpfile ();
  if (!cc_diag_capture)
    return;			/* no capture; back-end still prints to stderr */

  cc_diag_saved_stream = buf->stream;
  buf->stream = cc_diag_capture;
  cc_diag_base_warnings = global_dc->diagnostic_count (DK_WARNING);
  cc_diag_base_werrors = global_dc->diagnostic_count (DK_WERROR);
  /* Whatever warnings/werrors are already counted were emitted by the front
     end / parse (this hook runs after the parse loop).  Remember them so the
     store can flag the object as having front-end diagnostics -- the manifest
     fast-path must never serve such an object (it skips the parse that would
     re-emit them).  */
  cc_fe_warnings = cc_diag_base_warnings;
  cc_fe_werrors = cc_diag_base_werrors;
  cc_capturing = true;
}

/* Tear down the capturing stream, restoring the printer's real stream and
   echoing the captured diagnostics to stderr (so they still appear on the
   miss).  Returns the captured bytes in *OUT (xmalloc'd, caller frees) and
   the length in *OUT_LEN; the back-end DK_WARNING delta in *WARNINGS and the
   DK_WERROR (warnings promoted by -Werror) delta in *WERRORS.  On failure
   *OUT is NULL and *OUT_LEN 0 (counts are still reported).  Safe to call when
   capture never started.  */
static void
cc_end_backend_capture (unsigned char **out, size_t *out_len,
			uint32_t *warnings, uint32_t *werrors)
{
  *out = NULL;
  *out_len = 0;
  *warnings = 0;
  *werrors = 0;

  if (!cc_capturing)
    return;

  /* Restore the real stream first so any later diagnostics go to stderr.  */
  output_buffer *buf
    = (global_dc && global_dc->printer) ? pp_buffer (global_dc->printer) : NULL;
  if (buf)
    buf->stream = cc_diag_saved_stream;

  int w = (global_dc ? global_dc->diagnostic_count (DK_WARNING) : 0)
	  - cc_diag_base_warnings;
  int we = (global_dc ? global_dc->diagnostic_count (DK_WERROR) : 0)
	   - cc_diag_base_werrors;
  *warnings = w > 0 ? (uint32_t) w : 0;
  *werrors = we > 0 ? (uint32_t) we : 0;

  FILE *cap = cc_diag_capture;
  cc_diag_capture = NULL;
  cc_diag_saved_stream = NULL;
  cc_capturing = false;

  if (!cap)
    return;

  /* Slurp the captured bytes.  */
  if (fflush (cap) != 0 || fseek (cap, 0L, SEEK_END) != 0)
    {
      fclose (cap);
      return;
    }
  long n = ftell (cap);
  if (n < 0 || fseek (cap, 0L, SEEK_SET) != 0)
    {
      fclose (cap);
      return;
    }

  unsigned char *bytes = NULL;
  size_t len = 0;
  if (n > 0)
    {
      bytes = (unsigned char *) xmalloc ((size_t) n);
      len = fread (bytes, 1, (size_t) n, cap);
    }
  fclose (cap);

  /* Echo to stderr so the diagnostics still print on this (miss) compile.  */
  if (len)
    {
      fwrite (bytes, 1, len, stderr);
      fflush (stderr);
    }

  *out = bytes;
  *out_len = len;
}

/* ------------------------------------------------------------------------ */
/* Debug tracing                                                            */
/* ------------------------------------------------------------------------ */

/* True if GCC_COMPILE_CACHE_DEBUG is set in the environment (lazily probed).
   -1 = unknown, 0 = off, 1 = on.  */
static int cc_debug = -1;

static bool
cc_debug_p (void)
{
  if (cc_debug == -1)
    {
      const char *e = getenv ("GCC_COMPILE_CACHE_DEBUG");
      cc_debug = (e && e[0]) ? 1 : 0;
    }
  return cc_debug == 1;
}

/* Emit one debug line:  "compile-cache: <action> <key12> <output>".  ACTION
   is "hit", "miss" or "store"; KEY may be NULL (printed as "-").  */
static void
cc_debug_line (const char *action, const char *key)
{
  if (!cc_debug_p ())
    return;
  char k12[13];
  if (key)
    {
      memcpy (k12, key, 12);
      k12[12] = '\0';
    }
  else
    strcpy (k12, "-");
  fprintf (stderr, "compile-cache: %s %s %s\n", action, k12,
	   asm_file_name ? asm_file_name : "-");
  fflush (stderr);
}

/* ------------------------------------------------------------------------ */
/* Small helpers                                                            */
/* ------------------------------------------------------------------------ */

/* The component tags (enum cc_tag), the key-schema version
   (CC_KEY_SCHEMA_VERSION), the LE load/store helpers, and cc_hex live in
   compile-cache-format.h (shared with the serve unit).  The two sha1-feeding
   wrappers below stay here -- they take a sha1_ctx and are used only by the
   libcpp-dependent key walk.  */

/* Feed a 1-byte tag, an 8-byte little-endian length, then the bytes, into
   CTX.  The tag+length framing makes the key unambiguous.  Must match the
   framing in compile-cache-serve.cc (ccs_hash_component).  */
static void
cc_hash_component (struct sha1_ctx *ctx, unsigned char tag,
		   const void *data, size_t len)
{
  unsigned char hdr[9];
  hdr[0] = tag;
  for (int i = 0; i < 8; i++)
    hdr[1 + i] = (unsigned char) ((uint64_t) len >> (8 * i));
  sha1_process_bytes (hdr, sizeof (hdr), ctx);
  if (len)
    sha1_process_bytes (data, len, ctx);
}

/* Hash a NUL-terminated string as one component.  */
static void
cc_hash_str (struct sha1_ctx *ctx, unsigned char tag, const char *s)
{
  cc_hash_component (ctx, tag, s ? s : "", s ? strlen (s) : 0);
}

/* ------------------------------------------------------------------------ */
/* Option filtering                                                         */
/* ------------------------------------------------------------------------ */

/* Return true if DECODED is a command-line option that can change the
   generated assembly / ABI and therefore must participate in the key.

   Policy: conservative-but-broad.  We would rather over-include an option
   (an unnecessary miss) than under-include one (a WRONG hit serving stale
   assembly).  When in doubt, INCLUDE.

   THE DEFINITION LIVES IN compile-cache-serve.cc (declared extern at the top
   of this file) so the driver and cc1plus compute an identical manifest key.
   This block is the documentation of the policy; the code is shared.  */
#if 0
static bool
cc_option_affects_output_p (const cl_decoded_option *decoded)
{
  size_t idx = decoded->opt_index;

  /* Non-options (input files, "--", unknown).  The main input file is hashed
     separately; additional inputs make us ineligible (num_in_fnames != 1).  */
  if (idx >= cl_options_count)
    return false;

  /* Options that name the OUTPUT location or DUMP paths, or configure the
     cache itself, must be excluded even though they are flagged CL_COMMON:
     they carry run-varying paths (the driver hands cc1 a fresh temporary
     "-o /tmp/ccXXXX.s" each invocation) that do NOT change the assembly
     *content*.  Including them would make the key differ on every run and the
     cache could never hit.  */
  switch (idx)
    {
    case OPT_o:			/* -o <file> (Var asm_file_name) */
    case OPT_dumpbase:		/* -dumpbase <name> */
    case OPT_dumpbase_ext:	/* -dumpbase-ext <ext> */
    case OPT_dumpdir:		/* -dumpdir <dir> */
    case OPT_fcompile_cache_:	/* -fcompile-cache=<dir> (this feature) */
      return false;
    default:
      break;
    }

  const struct cl_option *opt = &cl_options[idx];
  unsigned int f = opt->flags;

  /* Mask of all per-language class bits (one bit per front-end, below
     CL_MIN_OPTION_CLASS).  */
  const unsigned int CC_CL_ANY_LANG = (CL_MIN_OPTION_CLASS - 1U);

  /* Warnings and diagnostics never affect emitted code.  */
  if (f & CL_WARNING)
    return false;

  /* Pure-driver options never reach codegen.  Some are dual
     CL_DRIVER|CL_COMMON; only exclude pure-driver ones.  */
  if ((f & CL_DRIVER) && !(f & (CL_COMMON | CL_TARGET | CC_CL_ANY_LANG)))
    return false;

  /* The big inclusive buckets: optimization and target machine options.  */
  if (f & (CL_OPTIMIZATION | CL_TARGET))
    return true;

  /* The include-search-path options name DIRECTORIES, not produced bytes.
     Their VALUES are deliberately excluded from the key: what they affect is
     *which* files end up in the include closure, and the closure walk already
     hashes the resolved files' exact CONTENTS (see cc_hash_one_file).  So the
     path strings are redundant -- two builds that resolve the same headers
     from differently named -I/-isystem trees produce identical assembly and
     must share a key.  Folding the path strings in defeats cross-build-dir /
     cross-machine hits (an -isystem dir contributing zero files was observed
     to flip the key).  We return false here rather than letting the generic
     CL_COMMON/lang catch-all at the bottom include them.

     Note this drops only the search-path *values*.  -D/-U/-A/-include/
     -imacros below DO affect the produced bytes (they change the
     preprocessed source) and stay in the key; nostdinc/undef/ansi change the
     set of predefined macros / search behaviour and stay too.  */
  switch (idx)
    {
    case OPT_I:
    case OPT_iquote:
    case OPT_isystem:
    case OPT_idirafter:
    case OPT_iprefix:
    case OPT_iwithprefix:
    case OPT_iwithprefixbefore:
    case OPT_isysroot:
      return false;
    default:
      break;
    }

  /* Force-include specific indices that change produced bytes but are not in
     CL_OPTIMIZATION.  Intentionally broad.  */
  switch (idx)
    {
    /* Preprocessor state baked into the source closure semantics.  */
    case OPT_D:
    case OPT_U:
    case OPT_A:
    case OPT_include:
    case OPT_imacros:
    case OPT_nostdinc:
    case OPT_undef:
    case OPT_ansi:
    /* Debug info kind/level changes the emitted assembly directives.  */
    case OPT_g:
    case OPT_ggdb:
    case OPT_gdwarf:
    case OPT_gdwarf_:
    /* Path remapping changes the bytes baked into debug info / strings.  */
    case OPT_ffile_prefix_map_:
    case OPT_fdebug_prefix_map_:
    case OPT_fmacro_prefix_map_:
    case OPT_fprofile_prefix_map_:
      return true;
    default:
      break;
    }

  /* Anything else that is a language or common option and is not obviously
     diagnostic-only: include it.  Over-inclusion is safe.  */
  if (f & (CL_COMMON | CC_CL_ANY_LANG))
    return true;

  return false;
}
#endif /* 0 -- cc_option_affects_output_p now lives in compile-cache-serve.cc */

/* ------------------------------------------------------------------------ */
/* Gating                                                                   */
/* ------------------------------------------------------------------------ */

bool
compile_cache_enabled_p (void)
{
  if (cc_enabled != -1)
    return cc_enabled == 1;

  cc_enabled = 0;		/* assume disabled until all checks pass */

  /* 1. Cache directory must be configured.  */
  cc_dir = compile_cache_dir;
  if (!cc_dir || !cc_dir[0])
    cc_dir = getenv ("GCC_COMPILE_CACHE_DIR");
  if (!cc_dir || !cc_dir[0])
    return false;

  /* 2. Only C and C++.  This object links into every *1 compiler, so this is
     the fast bail-out for Fortran/Ada/etc.  lang_hooks.name is like
     "GNU C23" / "GNU C++23".  (ObjC/ObjC++ are "GNU Objective-C..." and fall
     out here, deliberately -- only the two we support deterministically.)  */
  {
    const char *ln = lang_hooks.name;
    if (!ln || !startswith (ln, "GNU C"))
      return false;
  }

  /* 3. Modes that don't produce a normal single .s for one TU.  */
  if (flag_preprocess_only)		/* -E */
    return false;
  if (flag_syntax_only)			/* -fsyntax-only */
    return false;

  /* 4. LTO in any form -- the artifact is GIMPLE bytecode, not reusable
     assembly, and WPA/ltrans have their own pipeline.  */
  if (flag_lto || flag_generate_lto || in_lto_p || flag_wpa)
    return false;

  /* 5. Coverage / profiling instrumentation makes output path/run dependent
     and adds counters keyed to file identity.  */
  if (profile_flag || profile_arc_flag || flag_test_coverage
      || flag_branch_probabilities || flag_profile_use)
    return false;

  /* 6. Plugins can mutate the IR arbitrarily; not captured by our key.  */
  if (flag_plugin_added || plugins_active_p ())
    return false;

  /* 7. Precompiled headers: creating or consuming a PCH is a different
     pipeline and not byte-reproducible here.  (Set by the C/C++ front end.)  */
  if (cc_pch_active)
    return false;

  /* 8. Exactly one input translation unit.  */
  if (num_in_fnames != 1)
    return false;

  /* 9. Output must be a real, seekable file we can copy-from and copy-to.  */
  if (asm_out_file == stdout)
    return false;
  if (!asm_file_name
      || !strcmp (asm_file_name, "-")
      || !strcmp (asm_file_name, HOST_BIT_BUCKET))
    return false;

  /* 10. Stage 5 caches and serves the in-process-assembled OBJECT (.o), which
     only exists under -fintegrated-as (cc1plus produced the .o itself via
     gas_assemble_buffer).  Without integrated-as there is no in-process .o to
     cache, and the whole pre-parse serve premise ("the compiler already made
     the object") does not hold -- so the cache disables itself rather than
     fall back to the older .s behaviour.  asm_file_name == integ_obj_path in
     that mode (the real .o), which is what we place on a hit.  */
  if (!flag_integrated_as)
    return false;

  cc_enabled = 1;
  return true;
}

/* ------------------------------------------------------------------------ */
/* Key computation                                                          */
/* ------------------------------------------------------------------------ */

/* True when the emitted assembly actually depends on source PATH strings, so
   they must participate in the key.  The only thing that bakes a source path
   into the produced bytes is debug info: under -g the main input filename,
   the compilation directory, and each included file's path are emitted into
   DWARF (DW_AT_name / DW_AT_comp_dir / the .debug_line file table).  Without
   any -g the assembly is path-independent (the same content compiled from any
   directory, via any -I spelling, is byte-identical), so leaving the paths
   OUT of the key is what lets it hit across build directories and machines.

   -f*-prefix-map already canonicalizes the paths that land in debug info and
   is itself in the key (cc_option_affects_output_p), so a -g build that
   remaps to a stable prefix still hits; a -g build with raw absolute paths
   correctly keys on them and so will NOT cross-pollinate between trees.

   Caveat (deliberate, documented in invoke.texi): a non-debug TU that expands
   __FILE__ / __BASE_FILE__ bakes a path into the assembly even though
   debug_info_level is NONE.  libcpp does not cheaply expose whether those
   built-ins were expanded this TU (built-in macros, unlike user macros, never
   set NODE_USED), so we accept the same optimistic sloppiness as __TIME__:
   such a TU may get a hit carrying the first compiler's path string.  Use -g,
   a prefix-map, or the salt if exact __FILE__ bytes matter without debug.  */
static bool
cc_paths_affect_output_p (void)
{
  return debug_info_level > DINFO_LEVEL_NONE;
}

/* Closure-walk state shared with the cpp_foreach_included_file callback.  */
struct cc_closure_state
{
  struct sha1_ctx *ctx;
};

/* Callback: hash one included file's path + exact bytes into the digest, and
   record its identity (path + size + per-file SHA-1) for the inputs table.
   Both the key and the object metadata are produced from this single walk.  */
static bool
cc_hash_one_file (const char *path, const unsigned char *buffer,
		  size_t size, void *user)
{
  struct cc_closure_state *st = (struct cc_closure_state *) user;
  /* The file's CONTENTS are always part of the key; its PATH only when the
     produced bytes depend on it (under -g -- see cc_paths_affect_output_p).
     Dropping the path off the non-debug key is what makes an identical header
     reached via a differently named include tree hash the same.  The inputs
     table below still records the real path for diagnostics either way.  */
  if (cc_paths_affect_output_p ())
    cc_hash_str (st->ctx, CC_TAG_FILE_PATH, path);
  cc_hash_component (st->ctx, CC_TAG_FILE_BODY, buffer, size);

  /* Per-file SHA-1 for the inputs table (independent of the TU key digest).  */
  unsigned char fh[20];
  struct sha1_ctx fctx;
  sha1_init_ctx (&fctx);
  if (size)
    sha1_process_bytes (buffer, size, &fctx);
  sha1_finish_ctx (&fctx, fh);

  /* Capture st_mtime for the manifest stat-shortcut (size+mtime match accepts
     a header on a hit without re-reading it).  Best-effort: a failed stat
     records mtime 0, which simply forces a content re-hash on the next hit.  */
  uint64_t mtime = 0;
  {
    struct stat stt;
    if (stat (path, &stt) == 0)
      mtime = (uint64_t) stt.st_mtime;
  }
  cc_meta_add_input (path, (uint64_t) size, mtime, fh);
  return true;			/* keep walking */
}

/* Append one option token to the metadata "options" string (space-joined),
   reusing the codegen/ABI option set chosen for the key.  */
static void
cc_options_append (char **opts, const char *tok)
{
  if (!tok)
    return;
  if (!*opts)
    {
      *opts = xstrdup (tok);
      return;
    }
  char *joined = concat (*opts, " ", tok, NULL);
  free (*opts);
  *opts = joined;
}

/* Compute the SHA-1 key for this TU into cc_key_hex / cc_key_raw /
   cc_key_valid, using PFILE for the include closure, and gather the object
   metadata (source/cwd/target/language/options + inputs table) along the way.
   Returns true on success.  */
static bool
cc_compute_key (cpp_reader *pfile)
{
  struct sha1_ctx ctx;
  sha1_init_ctx (&ctx);

  cc_meta_clear ();

  /* Metadata scalars.  */
  cc_meta.source = xstrdup (main_input_filename ? main_input_filename : "");
  {
    const char *pwd = get_src_pwd ();
    cc_meta.cwd = xstrdup (pwd ? pwd : "");
  }
  cc_meta.target = xstrdup (TARGET_NAME);
  cc_meta.language = xstrdup (lang_hooks.name ? lang_hooks.name : "");

  /* (0) Key schema version, so a future format change self-invalidates.  */
  {
    unsigned char v[4];
    unsigned ver = CC_KEY_SCHEMA_VERSION;
    for (int i = 0; i < 4; i++)
      v[i] = (unsigned char) (ver >> (8 * i));
    cc_hash_component (&ctx, CC_TAG_VERSION, v, sizeof (v));
  }

  /* (0b) Optional user salt (GCC_COMPILE_CACHE_SALT).  When set and non-empty
     it is mixed into the key, so changing it remaps every TU to a fresh key and
     makes the previously cached entries unreachable -- a non-destructive logical
     cache reset / cache-bust.  When unset or empty it is not hashed at all, so
     existing keys are unaffected for users who never set it.  */
  {
    const char *salt = getenv ("GCC_COMPILE_CACHE_SALT");
    if (salt && salt[0])
      cc_hash_str (&ctx, CC_TAG_SALT, salt);
  }

  /* (1) The compiler binary's own fingerprint.  */
  cc_hash_component (&ctx, CC_TAG_CHECKSUM, executable_checksum, 16);

  /* (2) Language + dialect (lang_hooks.name encodes both, e.g. "GNU C++23").
     CC_TAG_STD is reserved/empty -- the -std=... decoded option is included
     by cc_option_affects_output_p in the option walk below.  */
  cc_hash_str (&ctx, CC_TAG_LANG, lang_hooks.name);
  cc_hash_str (&ctx, CC_TAG_STD, "");

  /* Main input path + cwd.  These bake into the produced bytes only as debug
     info (DW_AT_name / DW_AT_comp_dir), so they are hashed only under -g; see
     cc_paths_affect_output_p.  Leaving them out of the non-debug key is what
     lets the same source compiled from two different directories (different
     cwd, different -o) share one cache entry.  They are still recorded in the
     object metadata (cc_meta.source / cc_meta.cwd) for diagnostics.  */
  if (cc_paths_affect_output_p ())
    {
      cc_hash_str (&ctx, CC_TAG_MAIN_INPUT, main_input_filename);
      cc_hash_str (&ctx, CC_TAG_CWD, cc_meta.cwd);
    }

  /* (4) Canonicalized codegen/ABI-relevant command-line options, in order.
     The same token set is recorded in the metadata "options" string (minus
     -o / dump temp paths, which cc_option_affects_output_p already drops).  */
  for (unsigned i = 1; i < save_decoded_options_count; i++)
    {
      const cl_decoded_option *o = &save_decoded_options[i];
      if (!cc_option_affects_output_p (o))
	continue;
      for (size_t k = 0; k < o->canonical_option_num_elements; k++)
	{
	  cc_hash_str (&ctx, CC_TAG_OPT, o->canonical_option[k]);
	  cc_options_append (&cc_meta.options, o->canonical_option[k]);
	}
    }
  if (!cc_meta.options)
    cc_meta.options = xstrdup ("");

  /* (3) The source closure: paths + exact bytes of every stacked file.  This
     same walk fills the inputs table via cc_meta_add_input.  */
  struct cc_closure_state st;
  st.ctx = &ctx;
  if (!cpp_foreach_included_file (pfile, cc_hash_one_file, &st))
    return false;		/* a file could not be re-read; don't trust key */

  sha1_finish_ctx (&ctx, cc_key_raw);
  cc_hex (cc_key_raw, cc_key_hex);
  cc_key_valid = true;
  return true;
}

/* ------------------------------------------------------------------------ */
/* Manifest key (MK) computation                                            */
/* ------------------------------------------------------------------------ */

/* cc_option_is_search_path_p () -- the anti-shadowing search-path predicate --
   is defined in compile-cache-serve.cc (declared extern at the top of this
   file) so the store path here and the driver lookup use the identical set.
   The OBJECT key OK deliberately drops these (it is content-addressed and
   portable); the MANIFEST key folds them in (a same-named header newly
   appearing on an earlier -I would resolve differently, yet the old absolute
   path the manifest stored still matches -- so any search-config change must
   yield a different MK -> manifest miss -> a real compile).  */

/* Compute the manifest key MK into cc_manifest_key_hex / cc_manifest_key_valid.
   MK = schema ver + salt + checksum + lang + output-affecting options +
   search-path values + the MAIN SOURCE FILE's bytes (read once from disk; no
   preprocess, no parse), and -- under -g only -- the main source path + cwd.
   MK deliberately excludes header contents: headers are what the manifest
   *discovers*.  Returns false (and leaves cc_manifest_key_valid false) if the
   source cannot be read.  */
static bool
cc_compute_manifest_key (const char *src_path)
{
  cc_manifest_key_valid = false;

  if (!src_path || !src_path[0])
    return false;

  /* Read the main source bytes once.  */
  size_t src_len = 0;
  unsigned char *src = cc_read_file (src_path, &src_len);
  if (!src)
    return false;

  struct sha1_ctx ctx;
  sha1_init_ctx (&ctx);

  /* (0) Schema version (shared with OK so a format bump invalidates both).  */
  {
    unsigned char v[4];
    unsigned ver = CC_KEY_SCHEMA_VERSION;
    for (int i = 0; i < 4; i++)
      v[i] = (unsigned char) (ver >> (8 * i));
    cc_hash_component (&ctx, CC_TAG_VERSION, v, sizeof (v));
  }
  /* A distinct domain tag so an MK can never collide with an OK that happened
     to hash the same components (manifest objects live under MK, objects under
     OK, in the same sharded namespace).  */
  cc_hash_str (&ctx, CC_TAG_LANG, "compile-cache-manifest-key");

  /* (0b) Salt.  */
  {
    const char *salt = getenv ("GCC_COMPILE_CACHE_SALT");
    if (salt && salt[0])
      cc_hash_str (&ctx, CC_TAG_SALT, salt);
  }

  /* (1) Compiler fingerprint + (2) language/dialect.  */
  cc_hash_component (&ctx, CC_TAG_CHECKSUM, executable_checksum, 16);
  cc_hash_str (&ctx, CC_TAG_LANG, lang_hooks.name);

  /* Under -g the source path + cwd bake into DWARF, so fold them in.  */
  if (cc_paths_affect_output_p ())
    {
      cc_hash_str (&ctx, CC_TAG_MAIN_INPUT, src_path);
      const char *pwd = get_src_pwd ();
      cc_hash_str (&ctx, CC_TAG_CWD, pwd ? pwd : "");
    }

  /* (4) Output-affecting options + (anti-shadow) search-path VALUES.  Walked
     in command-line order so option ordering is part of the key.  */
  for (unsigned i = 1; i < save_decoded_options_count; i++)
    {
      const cl_decoded_option *o = &save_decoded_options[i];
      bool affects = cc_option_affects_output_p (o);
      bool search = cc_option_is_search_path_p (o);
      if (!affects && !search)
	continue;
      for (size_t k = 0; k < o->canonical_option_num_elements; k++)
	cc_hash_str (&ctx, search ? CC_TAG_SEARCH_PATH : CC_TAG_OPT,
		     o->canonical_option[k]);
    }

  /* (S) The main source file's exact bytes -- the heart of MK.  Fingerprinted
     with the fast 128-bit cc_fast128 and folded into MK's SHA-1 (see the long
     note in compile-cache-format.h): MK is a lookup index, so a fingerprint
     collision only yields a manifest miss, never a wrong serve.  This MUST stay
     byte-identical to the driver's ccs_compute_manifest_key, or the driver-
     level no-spawn serve never hits.  */
  {
    unsigned char fp[16];
    cc_fast128 (src, src_len, fp);
    cc_hash_component (&ctx, CC_TAG_SRC_BODY, fp, sizeof (fp));
  }
  free (src);

  unsigned char raw[20];
  sha1_finish_ctx (&ctx, raw);
  cc_hex (raw, cc_manifest_key_hex);
  cc_manifest_key_valid = true;
  return true;
}

/* ------------------------------------------------------------------------ */
/* Path construction                                                        */
/* ------------------------------------------------------------------------ */

/* mkdir a single level; treat EEXIST as success.  */
static bool
cc_ensure_dir (const char *path)
{
  if (mkdir (path, 0777) == 0)
    return true;
  return errno == EEXIST;
}

/* Build "DIR/ab/cdef...rest.bin" into a freshly xmalloc'd string (caller frees
   with free()).  KEY is the 40-char hex.  Makes DIR and DIR/ab if MAKE_DIRS.  */
static char *
cc_entry_path (const char *key, bool make_dirs)
{
  if (make_dirs)
    cc_ensure_dir (cc_dir);

  char shard[3] = { key[0], key[1], '\0' };
  char *shard_dir = concat (cc_dir, "/", shard, NULL);
  if (make_dirs)
    cc_ensure_dir (shard_dir);

  char *full = concat (shard_dir, "/", key + 2, ".bin", NULL);
  free (shard_dir);
  return full;
}

/* ------------------------------------------------------------------------ */
/* Public: determinism                                                      */
/* ------------------------------------------------------------------------ */

void
compile_cache_init_determinism (bool pch_active)
{
  /* Record PCH state before any gating decision is cached.  */
  cc_pch_active = pch_active;

  if (!compile_cache_enabled_p ())
    return;

  /* If the user already supplied -frandom-seed (or -fno-random-seed), do not
     override it.  flag_random_seed is file-static in toplev.cc, so detect the
     user's intent by scanning the decoded options for OPT_frandom_seed /
     OPT_frandom_seed_ (the indices handle_common_deferred_options uses).  */
  for (unsigned i = 1; i < save_decoded_options_count; i++)
    {
      size_t idx = save_decoded_options[i].opt_index;
      if (idx == OPT_frandom_seed || idx == OPT_frandom_seed_)
	return;			/* user pinned it; leave as-is */
    }

  /* Pin a single FIXED seed for full determinism.  We deliberately do NOT
     derive a per-TU seed from the input: a constant seed makes two compiles
     of the same TU byte-identical (so the cache can hit), and on ELF the
     -frandom-seed value does not feed symbol naming, so a shared constant
     across TUs is safe and cannot collide on generated symbol names.  The
     cache KEY still hashes the full source closure + flags + executable
     checksum, so distinct TUs remain distinct cache entries.  A user-supplied
     -frandom-seed is still respected (handled by the early return above).  */
  set_random_seed ("1234");
}

/* ------------------------------------------------------------------------ */
/* Public: serve                                                            */
/* ------------------------------------------------------------------------ */

/* Read the whole file at PATH into a freshly xmalloc'd buffer; return it and
   set *LEN.  Returns NULL on any error.  */
static unsigned char *
cc_read_file (const char *path, size_t *len)
{
  *len = 0;
  FILE *f = fopen (path, "rb");
  if (!f)
    return NULL;
  if (fseek (f, 0L, SEEK_END) != 0)
    {
      fclose (f);
      return NULL;
    }
  long n = ftell (f);
  if (n < 0 || fseek (f, 0L, SEEK_SET) != 0)
    {
      fclose (f);
      return NULL;
    }
  unsigned char *buf = (unsigned char *) xmalloc ((size_t) n + 1);
  size_t got = (n > 0) ? fread (buf, 1, (size_t) n, f) : 0;
  fclose (f);
  if (got != (size_t) n)
    {
      free (buf);
      return NULL;
    }
  buf[got] = '\0';
  *len = got;
  return buf;
}

/* The cached object's bytes are stored in a sidecar file next to the .bin
   metadata object so that placement can hardlink it (O(1)) instead of copying.
   For "DIR/ab/rest.bin" the sidecar is "DIR/ab/rest.o".  Caller frees.  */
static char *
cc_object_sidecar_path (const char *entry_bin_path)
{
  size_t n = strlen (entry_bin_path);
  /* Replace a trailing ".bin" with ".o"; otherwise just append ".o".  */
  if (n >= 4 && strcmp (entry_bin_path + n - 4, ".bin") == 0)
    {
      char *p = (char *) xmalloc (n - 4 + 2 + 1);
      memcpy (p, entry_bin_path, n - 4);
      memcpy (p + (n - 4), ".o", 3);	/* ".o\0" */
      return p;
    }
  return concat (entry_bin_path, ".o", NULL);
}

/* Place the cached object file CACHED_O at DST.  Ladder (each rung falls
   through to the next on failure):
     1. reflink (FICLONE)  -- CoW, O(1); unavailable on ext4 (this env), the
        win on btrfs/XFS/ZFS.
     2. hardlink (link())  -- O(1), no byte copy, same-fs.  The caller
        (compile_cache_store) chmods the cache object 0444 AFTER a successful
        placement, so an accidental in-place edit (objcopy/strip --in-place)
        fails loudly rather than mutating the cache.  Because a same-fs hardlink
        shares one inode, the output .o becomes 0444 too -- the documented
        "fail loudly" tradeoff, intended on the store side.
     3. copy               -- always-correct fallback (read once, write to a
        temp in DST's dir, then rename() atomically -- a killed/ENOSPC write
        never leaves a truncated object at DST).
   GCC_COMPILE_CACHE_LINK = copy|hardlink|reflink|auto (default auto) selects
   the highest rung to start at.  DST is unlinked first so link()/open() see a
   clean target (the driver hands us a fresh -o path, but be defensive).
   This function itself leaves DST writable (mkstemp/reflink/link defaults);
   the 0444 enforcement is the store caller's job (see compile_cache_store).
   Returns true on success.  */
static bool
cc_place_object (const char *cached_o, const char *dst)
{
  enum { LINK_AUTO, LINK_COPY, LINK_HARDLINK, LINK_REFLINK } mode = LINK_AUTO;
  const char *e = getenv ("GCC_COMPILE_CACHE_LINK");
  if (e && e[0])
    {
      if (!strcmp (e, "copy"))
	mode = LINK_COPY;
      else if (!strcmp (e, "hardlink"))
	mode = LINK_HARDLINK;
      else if (!strcmp (e, "reflink"))
	mode = LINK_REFLINK;
    }

  /* Clear any existing target so link()/rename see a clean slot.  */
  unlink (dst);

#ifdef FICLONE
  if (mode == LINK_AUTO || mode == LINK_REFLINK)
    {
      int sfd = open (cached_o, O_RDONLY);
      if (sfd >= 0)
	{
	  int dfd = open (dst, O_WRONLY | O_CREAT | O_TRUNC, 0666);
	  if (dfd >= 0)
	    {
	      int rc = ioctl (dfd, FICLONE, sfd);
	      close (dfd);
	      if (rc == 0)
		{
		  close (sfd);
		  return true;
		}
	      unlink (dst);		/* reflink failed; try next rung */
	    }
	  close (sfd);
	}
      if (mode == LINK_REFLINK)
	return false;
    }
#else
  if (mode == LINK_REFLINK)
    mode = LINK_HARDLINK;		/* no reflink support; degrade */
#endif

  if (mode == LINK_AUTO || mode == LINK_HARDLINK)
    {
      if (link (cached_o, dst) == 0)
	return true;
      if (mode == LINK_HARDLINK)
	return false;
      /* AUTO: fall through to copy (e.g. cross-device link -> EXDEV).  */
    }

  /* Copy fallback: read the cached object once, write it to a temp file in the
     SAME directory as DST, then rename() it into place atomically.  A killed
     process or ENOSPC mid-write leaves only the temp (unlinked here) -- never a
     truncated object at DST that would later look valid.  */
  size_t len = 0;
  unsigned char *bytes = cc_read_file (cached_o, &len);
  if (!bytes)
    return false;
  char *tmp = concat (dst, ".tmpXXXXXX", NULL);
  int fd = mkstemp (tmp);
  FILE *out = (fd >= 0) ? fdopen (fd, "wb") : NULL;
  if (!out)
    {
      if (fd >= 0)
	{
	  close (fd);
	  unlink (tmp);
	}
      free (tmp);
      free (bytes);
      return false;
    }
  bool ok = (len == 0) || (fwrite (bytes, 1, len, out) == len);
  if (fclose (out) != 0)
    ok = false;
  free (bytes);
  if (ok && rename (tmp, dst) != 0)
    ok = false;
  if (!ok)
    unlink (tmp);
  free (tmp);
  return ok;
}

/* ------------------------------------------------------------------------ */
/* v3 metadata: xattr on the cache .o, with a minimal .bin fallback         */
/* ------------------------------------------------------------------------ */

/* Build the compact CC_META record (CC_META_REC_SIZE bytes) followed by the
   DIAG_LEN-byte diagnostics blob into a freshly xmalloc'd buffer; set *OUT_LEN.
   Caller frees.  This identical byte sequence is stored either in the
   CC_XATTR_META xattr or, on the fallback path, as the body of the .bin
   sidecar.  */
static unsigned char *
cc_build_meta (uint16_t flags, uint32_t warnings, uint32_t errors,
	       const unsigned char *diag_bytes, size_t diag_len,
	       size_t *out_len)
{
  size_t total = CC_META_REC_SIZE + diag_len;
  unsigned char *buf = (unsigned char *) xmalloc (total ? total : 1);
  memcpy (buf + CC_META_OFF_MAGIC, CC_META_MAGIC, CC_MAGIC_LEN);
  cc_put_u16 (buf + CC_META_OFF_VERSION, (uint16_t) CC_FORMAT_VERSION);
  cc_put_u16 (buf + CC_META_OFF_FLAGS, flags);
  cc_put_u32 (buf + CC_META_OFF_WARNINGS, warnings);
  cc_put_u32 (buf + CC_META_OFF_ERRORS, errors);
  cc_put_u32 (buf + CC_META_OFF_DIAG_LEN, (uint32_t) diag_len);
  if (diag_len)
    memcpy (buf + CC_META_REC_SIZE, diag_bytes, diag_len);
  *out_len = total;
  return buf;
}

/* Decode a CC_META record + diag blob (META, MLEN bytes) into the fields a hit
   needs.  Returns true if the record is structurally valid and is our version;
   on success *DIAG points into META (no copy) and *DIAG_LEN is its length.  */
static bool
cc_parse_meta (const unsigned char *meta, size_t mlen, uint16_t *flags,
	       uint32_t *warnings, uint32_t *errors,
	       const unsigned char **diag, size_t *diag_len)
{
  if (mlen < CC_META_REC_SIZE
      || memcmp (meta + CC_META_OFF_MAGIC, CC_META_MAGIC, CC_MAGIC_LEN) != 0
      || cc_get_u16 (meta + CC_META_OFF_VERSION) != CC_FORMAT_VERSION)
    return false;
  uint32_t dlen = cc_get_u32 (meta + CC_META_OFF_DIAG_LEN);
  if ((uint64_t) dlen > mlen - CC_META_REC_SIZE)
    return false;			/* truncated diag blob -> reject */
  *flags = cc_get_u16 (meta + CC_META_OFF_FLAGS);
  *warnings = cc_get_u32 (meta + CC_META_OFF_WARNINGS);
  *errors = cc_get_u32 (meta + CC_META_OFF_ERRORS);
  *diag = dlen ? meta + CC_META_REC_SIZE : NULL;
  *diag_len = dlen;
  return true;
}

/* Attach the meta record (BUF, LEN bytes) to the cache object at OBJ_PATH as
   the CC_XATTR_META xattr (plus a tiny CC_XATTR_VERSION probe).  Returns true
   on success.  Sets *UNSUPPORTED true when the failure is "this filesystem
   does not support user xattrs / the record does not fit" (ENOTSUP / E2BIG /
   ENOSPC / EDQUOT), so the caller can fall back to a .bin sidecar for THIS
   entry only.  Any other error is a hard failure (returns false, *UNSUPPORTED
   false).  When built without <sys/xattr.h>, reports unsupported.  */
static bool
cc_set_meta_xattr (const char *obj_path, const unsigned char *buf, size_t len,
		   bool *unsupported)
{
  *unsupported = false;
#ifdef CC_HAVE_XATTR
  unsigned char vbuf[2];
  cc_put_u16 (vbuf, (uint16_t) CC_FORMAT_VERSION);
  if (setxattr (obj_path, CC_XATTR_META, buf, len, 0) != 0
      || setxattr (obj_path, CC_XATTR_VERSION, vbuf, sizeof (vbuf), 0) != 0)
    {
      if (errno == ENOTSUP || errno == EOPNOTSUPP || errno == E2BIG
	  || errno == ENOSPC || errno == EDQUOT || errno == ERANGE)
	*unsupported = true;
      /* Clear a partial set so a later reader never sees a stale half-record.  */
      removexattr (obj_path, CC_XATTR_META);
      removexattr (obj_path, CC_XATTR_VERSION);
      return false;
    }
  return true;
#else
  (void) obj_path; (void) buf; (void) len;
  *unsupported = true;
  return false;
#endif
}

/* Read the CC_XATTR_META xattr from OBJ_PATH into a freshly xmalloc'd buffer;
   set *LEN and return it (caller frees).  Returns NULL if the attribute is
   absent/unreadable or xattrs are unavailable -- the caller then tries the
   .bin fallback.  */
static unsigned char *
cc_get_meta_xattr (const char *obj_path, size_t *len)
{
  *len = 0;
#ifdef CC_HAVE_XATTR
  ssize_t n = getxattr (obj_path, CC_XATTR_META, NULL, 0);
  if (n < 0)
    return NULL;
  unsigned char *buf = (unsigned char *) xmalloc ((size_t) n + 1);
  ssize_t got = getxattr (obj_path, CC_XATTR_META, buf, (size_t) n);
  if (got < 0)
    {
      free (buf);
      return NULL;
    }
  buf[got] = '\0';
  *len = (size_t) got;
  return buf;
#else
  (void) obj_path;
  return NULL;
#endif
}

/* Core serve routine shared by the post-parse object-key path
   (compile_cache_try_serve) and the pre-parse manifest path
   (compile_cache_try_serve_manifest).  Given the object's hex key OK_HEX:

     - read and validate the v3 metadata for the object keyed by OK_HEX: the
       CC_XATTR_META xattr on the cache object "DIR/ab/rest.o", or the minimal
       ".bin" sidecar written on the xattr-unsupported fallback path;
     - if REQUIRE_NO_FE_DIAG and the object carries front-end diagnostics
       (CC_FLAG_HAD_FE_DIAG), refuse to serve (return false) -- the pre-parse
       path skips the parse that would re-emit them, so it must fall through;
     - place the cache object "DIR/ab/rest.o" at asm_file_name via the
       reflink->hardlink->copy ladder (cc_place_object);
     - close the in-memory asm stream (asm_out_file) WITHOUT assembling -- on a
       hit the memstream is empty and finalize() must not run gas on it (it is
       gated off by compile_cache_hit_p ());
     - replay the cached diagnostics and fold the stored warning/werror counts;
     - set cc_hit.

   Returns true on a served hit, false on miss/refusal (caller decides whether
   to install the back-end capture and fall through).  DEBUG_ACTION labels the
   debug line ("hit" / "manifest-hit").  */
static bool
cc_serve_from_bin (const char *ok_hex, bool require_no_fe_diag,
		   const char *debug_action)
{
  char *bin_path = cc_entry_path (ok_hex, /*make_dirs=*/false);
  char *obj_path = cc_object_sidecar_path (bin_path);

  /* The metadata lives in an xattr on the cache .o; fall back to the .bin
     sidecar written when the filesystem rejected the xattr.  Either way it is
     the same CC_META record + diag blob.  */
  size_t mlen = 0;
  unsigned char *meta = cc_get_meta_xattr (obj_path, &mlen);
  if (!meta)
    meta = cc_read_file (bin_path, &mlen);

  uint16_t flags = 0;
  uint32_t warnings = 0, errors = 0;
  const unsigned char *diag = NULL;
  size_t diag_len = 0;
  if (!meta
      || !cc_parse_meta (meta, mlen, &flags, &warnings, &errors,
			 &diag, &diag_len))
    {
      free (meta);
      free (obj_path);
      free (bin_path);
      return false;
    }

  /* The pre-parse fast-path must not serve an object whose front-end
     diagnostics it would silently drop (it skipped the parse).  */
  if (require_no_fe_diag && (flags & CC_FLAG_HAD_FE_DIAG))
    {
      free (meta);
      free (obj_path);
      free (bin_path);
      return false;
    }

  /* Place the cache .o at the output.  The cache only runs under
     -fintegrated-as (gating check #10), so asm_file_name is the real .o path
     and asm_out_file is the (still-open, empty-on-a-hit) memstream.  */
  bool placed = cc_place_object (obj_path, asm_file_name);
  free (obj_path);
  free (bin_path);
  if (!placed)
    {
      /* Object missing/unreadable: treat as a miss and recompute.  */
      free (meta);
      return false;
    }

  /* Close the in-memory asm stream without assembling it.  finalize() gates
     gas_assemble_buffer on !compile_cache_hit_p (), so on a hit it just frees
     the (empty) buffer.  We close it here so finalize()'s integrated branch
     sees asm_out_file == NULL and skips both the close and the assemble.  */
  if (asm_out_file && asm_out_file != stdout)
    fclose (asm_out_file);
  asm_out_file = NULL;

  /* Replay the cached diagnostics and fold the stored counts so the
     "N warnings"/"M errors" summary and the -Werror exit status match a fresh
     compile.  For the post-parse path these are the back-end diagnostics only
     (front-end ones re-emit on the re-parse); for the manifest path the object
     is guaranteed to have NO front-end diagnostics (refused above), so the
     back-end set is the complete set.  */
  if (diag_len)
    {
      fflush (stdout);
      fwrite (diag, 1, diag_len, stderr);
      fflush (stderr);
    }
  if (global_dc)
    {
      global_dc->diagnostic_count (DK_WARNING) += (int) warnings;
      global_dc->diagnostic_count (DK_WERROR) += (int) errors;
    }

  free (meta);
  cc_hit = true;
  cc_debug_line (debug_action, ok_hex);
  return true;
}

bool
compile_cache_try_serve (cpp_reader *pfile)
{
  if (!compile_cache_enabled_p ())
    return false;
  if (!pfile)
    return false;

  /* A pre-parse manifest hit already served and set cc_hit; nothing to do.  */
  if (cc_hit)
    return true;

  /* Record whether the parse used __has_include so the store can decide
     whether a manifest entry is sound for this TU (it isn't, if it did).  */
  cc_tu_used_has_include = cpp_used_has_include (pfile);

  if (!cc_compute_key (pfile))
    return false;		/* key untrustworthy -> behave as a miss */

  /* Try to serve by the object key OK.  Front-end diagnostics are allowed here
     (they re-emit on the re-parse that already happened), so pass
     require_no_fe_diag = false.  */
  if (cc_serve_from_bin (cc_key_hex, /*require_no_fe_diag=*/false, "hit"))
    return true;

  /* MISS: install the back-end diagnostic capture so the store can record
     them, and fall through to the back-end.  */
  cc_debug_line ("miss", cc_key_hex);
  cc_begin_backend_capture ();
  return false;
}

/* Pre-parse manifest fast-path.  Called BEFORE the parse loop with the main
   source path.  Computes the manifest key MK, reads the manifest object under
   MK, and for each recorded include set re-resolves every header by its stored
   absolute path -- accepting on a size+mtime stat match, else a content
   re-hash -- WITHOUT preprocessing or parsing.  On the first set that fully
   matches AND whose object (under that set's OK) exists and is servable, places
   the cached .o, replays diagnostics, sets cc_hit, and returns true so the
   front end can skip the parse and back-end entirely.  Otherwise returns false
   and the caller proceeds to a normal compile (which records/updates the
   manifest + object on the miss path).  */
bool
compile_cache_try_serve_manifest (cpp_reader *pfile, const char *src_path)
{
  if (!compile_cache_enabled_p ())
    return false;

  /* Note on __has_include: a TU that probes __has_include / __has_include_next
     cannot be soundly served from the manifest (such a probe never enters the
     include closure, so the manifest cannot notice it flipping absent<->present
     between runs).  The bit that records this (cpp_reader::used_has_include) is
     only set DURING the parse, so it is not yet known here at pre-parse time.
     The protection therefore lives on the STORE side: compile_cache_store ()
     refuses to write a manifest for a TU that used __has_include, so no
     manifest ever exists for such a TU and this lookup simply misses -> a full
     compile re-resolves everything.  Safe by construction.  (void pfile.)  */
  (void) pfile;

  /* Optional airtight mode: GCC_COMPILE_CACHE_VERIFY=hash forces a full
     content re-hash of every header on a hit instead of the size+mtime stat
     shortcut.  Even this still skips parse/codegen/assemble.  */
  bool verify_hash = false;
  {
    const char *v = getenv ("GCC_COMPILE_CACHE_VERIFY");
    if (v && !strcmp (v, "hash"))
      verify_hash = true;
  }

  if (!cc_compute_manifest_key (src_path))
    return false;

  char *man_path = cc_entry_path (cc_manifest_key_hex, /*make_dirs=*/false);
  size_t mlen = 0;
  unsigned char *man = cc_read_file (man_path, &mlen);
  free (man_path);

  if (!man
      || mlen < CC_MANIFEST_HEADER_SIZE
      || memcmp (man + CC_MAN_OFF_MAGIC, CC_MANIFEST_MAGIC, CC_MAGIC_LEN) != 0
      || cc_get_u16 (man + CC_MAN_OFF_VERSION) != CC_MANIFEST_VERSION)
    {
      free (man);
      cc_debug_line ("manifest-miss", cc_manifest_key_hex);
      return false;
    }

  uint32_t entry_count = cc_get_u32 (man + CC_MAN_OFF_ENTRY_COUNT);
  uint64_t entries_off = cc_get_u64 (man + CC_MAN_OFF_ENTRIES_OFF);
  if (entries_off > mlen)
    {
      free (man);
      cc_debug_line ("manifest-miss", cc_manifest_key_hex);
      return false;
    }

  /* Walk each entry (candidate header set).  */
  uint64_t cur = entries_off;
  for (uint32_t ei = 0; ei < entry_count; ei++)
    {
      /* Entry fixed part: OK[20] warnings(4) werrors(4) header_count(4).  */
      if (cur + 20 + 4 + 4 + 4 > mlen)
	break;			/* truncated manifest -> stop */
      const unsigned char *ent = man + cur;
      unsigned char ok_raw[20];
      memcpy (ok_raw, ent + 0, 20);
      uint32_t hdr_count = cc_get_u32 (ent + 28);
      uint64_t recs_off = cur + 32;
      uint64_t recs_len = (uint64_t) hdr_count * CC_MAN_HDR_REC_SIZE;
      if (recs_off + recs_len > mlen)
	break;			/* truncated -> stop */

      /* Verify every header in this set resolves and matches.  */
      bool all_match = true;
      for (uint32_t hi = 0; hi < hdr_count && all_match; hi++)
	{
	  const unsigned char *rec = man + recs_off
				     + (uint64_t) hi * CC_MAN_HDR_REC_SIZE;
	  uint32_t path_off = cc_get_u32 (rec + CC_MHR_OFF_PATH);
	  uint64_t want_size = cc_get_u64 (rec + CC_MHR_OFF_SIZE);
	  uint64_t want_mtime = cc_get_u64 (rec + CC_MHR_OFF_MTIME);
	  const unsigned char *want_hash = rec + CC_MHR_OFF_HASH;

	  /* The path string lives in the string area (length-prefixed + NUL).
	     Bounds-check before dereferencing.  */
	  if (path_off + 4 > mlen)
	    {
	      all_match = false;
	      break;
	    }
	  uint32_t plen = cc_get_u32 (man + path_off);
	  if ((uint64_t) path_off + 4 + plen + 1 > mlen)
	    {
	      all_match = false;
	      break;
	    }
	  const char *hpath = (const char *) (man + path_off + 4);

	  struct stat stt;
	  if (stat (hpath, &stt) != 0)
	    {
	      all_match = false;	/* header moved/deleted -> stale set */
	      break;
	    }

	  /* Stat shortcut: size + mtime match accepts without reading, unless
	     the airtight verify-hash mode is on.  */
	  if (!verify_hash
	      && (uint64_t) stt.st_size == want_size
	      && (uint64_t) stt.st_mtime == want_mtime)
	    continue;

	  /* Otherwise read + content-hash and compare.  */
	  size_t got_len = 0;
	  unsigned char *body = cc_read_file (hpath, &got_len);
	  if (!body || (uint64_t) got_len != want_size)
	    {
	      free (body);
	      all_match = false;
	      break;
	    }
	  unsigned char got_hash[20];
	  struct sha1_ctx fctx;
	  sha1_init_ctx (&fctx);
	  if (got_len)
	    sha1_process_bytes (body, got_len, &fctx);
	  sha1_finish_ctx (&fctx, got_hash);
	  free (body);
	  if (memcmp (got_hash, want_hash, 20) != 0)
	    {
	      all_match = false;
	      break;
	    }
	}

      if (all_match)
	{
	  /* Every header matched.  Resolve the object under this set's OK and
	     serve it -- but only if it carries no front-end diagnostics (we are
	     about to skip the parse).  */
	  char ok_hex[41];
	  cc_hex (ok_raw, ok_hex);
	  if (cc_serve_from_bin (ok_hex, /*require_no_fe_diag=*/true,
				 "manifest-hit"))
	    {
	      free (man);
	      return true;
	    }
	  /* Object missing or refused (front-end diag): try the next set, then
	     fall through to a real compile.  */
	}

      cur = recs_off + recs_len;
    }

  free (man);
  cc_debug_line ("manifest-miss", cc_manifest_key_hex);
  return false;
}

bool
compile_cache_hit_p (void)
{
  return cc_hit;
}

/* ------------------------------------------------------------------------ */
/* Public: store                                                            */
/* ------------------------------------------------------------------------ */

/* A growable byte buffer used to assemble the string area.  */
struct cc_blob
{
  unsigned char *data;
  size_t len;
  size_t cap;
};

static void
cc_blob_reserve (cc_blob *b, size_t extra)
{
  if (b->len + extra <= b->cap)
    return;
  size_t n = b->cap ? b->cap * 2 : 256;
  while (n < b->len + extra)
    n *= 2;
  b->data = (unsigned char *) xrealloc (b->data, n);
  b->cap = n;
}

static void
cc_blob_append (cc_blob *b, const void *p, size_t n)
{
  if (!n)
    return;
  cc_blob_reserve (b, n);
  memcpy (b->data + b->len, p, n);
  b->len += n;
}

/* Append a length-prefixed + NUL string to the string-area blob and return the
   offset (relative to the blob start) at which its u32 length prefix begins.  */
static uint32_t
cc_blob_add_string (cc_blob *b, const char *s)
{
  if (!s)
    s = "";
  size_t slen = strlen (s);
  uint32_t off = (uint32_t) b->len;
  unsigned char lp[4];
  cc_put_u32 (lp, (uint32_t) slen);
  cc_blob_append (b, lp, 4);
  cc_blob_append (b, s, slen);
  unsigned char nul = 0;
  cc_blob_append (b, &nul, 1);
  return off;
}

/* Write BYTES (LEN bytes) to PATH atomically (temp + rename), then make PATH
   read-only (0444).  PATH lives in an already-created shard dir.  The 0444
   mode guards a hardlink-placed output against accidental in-place mutation
   (objcopy/strip --in-place would fail loudly rather than corrupt the cache).
   Returns true on success.  */
static bool
cc_write_atomic_readonly (const char *path, const unsigned char *bytes,
			  size_t len)
{
  char *tmp = concat (path, ".tmpXXXXXX", NULL);
  int fd = mkstemp (tmp);
  bool ok = (fd >= 0);
  FILE *dst = ok ? fdopen (fd, "wb") : NULL;
  if (!dst && fd >= 0)
    {
      close (fd);
      ok = false;
    }
  if (ok)
    {
      ok = (len == 0) || (fwrite (bytes, 1, len, dst) == len);
      if (fclose (dst) != 0)
	ok = false;
    }
  if (ok)
    {
      chmod (tmp, 0444);
      if (rename (tmp, path) != 0)
	{
	  unlink (tmp);
	  ok = false;
	}
    }
  else if (fd >= 0)
    unlink (tmp);
  free (tmp);
  return ok;
}

/* Write the cache's "compiler-id" sidecar (DIR/compiler-id) so the DRIVER can
   form the manifest key without linking this compiler's checksum object or
   knowing lang_hooks.name: it records executable_checksum + lang_hooks.name,
   the two key components the driver cannot derive on its own.  Idempotent and
   cheap; written on every miss-store (a recompiled compiler -> new checksum ->
   the driver's MK changes in lockstep, so a stale id can never cause a wrong
   hit -- it would simply differ from the object's stored checksum-keyed MK).
   Atomic publish.  No-op on failure.  */
static void
cc_write_compiler_id (void)
{
  if (!cc_dir || !cc_dir[0])
    return;
  cc_ensure_dir (cc_dir);

  const char *lang = lang_hooks.name ? lang_hooks.name : "";
  uint32_t llen = (uint32_t) strlen (lang);

  size_t total = 8 + 2 + 2 + 16 + 4 + (size_t) llen;
  unsigned char *buf = (unsigned char *) xmalloc (total);
  memcpy (buf, CC_COMPILER_ID_MAGIC, CC_MAGIC_LEN);
  cc_put_u16 (buf + 8, (uint16_t) CC_COMPILER_ID_VERSION);
  cc_put_u16 (buf + 10, 0);
  memcpy (buf + 12, executable_checksum, 16);
  cc_put_u32 (buf + 28, llen);
  if (llen)
    memcpy (buf + 32, lang, llen);

  char *path = concat (cc_dir, "/", CC_COMPILER_ID_NAME, NULL);
  char *tmp = concat (path, ".tmpXXXXXX", NULL);
  int fd = mkstemp (tmp);
  bool ok = (fd >= 0);
  FILE *dst = ok ? fdopen (fd, "wb") : NULL;
  if (!dst && fd >= 0)
    {
      close (fd);
      ok = false;
    }
  if (ok)
    {
      ok = (fwrite (buf, 1, total, dst) == total);
      if (fclose (dst) != 0)
	ok = false;
    }
  if (ok)
    {
      if (rename (tmp, path) != 0)
	unlink (tmp);
    }
  else if (fd >= 0)
    unlink (tmp);
  free (buf);
  free (tmp);
  free (path);
}

/* Append/refresh this TU's entry in the manifest object keyed by MK.  The
   manifest lists, per include set, the object key OK and the headers that set
   depended on (path + size + mtime + hash, from cc_meta.inputs).  Existing
   entries with a DIFFERENT OK are preserved (the same source+flags can reach
   different header sets via conditional includes / -I ordering); an entry with
   the SAME OK is replaced (refreshes mtimes after a touch).  WARNINGS/WERRORS
   are the object's stored counts (carried so a future manifest reader could
   short-circuit; the authoritative copy is in the object header).  Atomic
   publish.  No-op unless the manifest key is valid.  */
static void
cc_store_manifest (uint32_t warnings, uint32_t werrors)
{
  if (!cc_manifest_key_valid)
    return;

  /* A TU that probed __has_include / __has_include_next has an include closure
     that is not a sound predictor for a pre-parse serve (a probed-but-not-
     included header can appear without changing the closure).  Do NOT record a
     manifest for it, so the fast-path never serves it; the object cache still
     hits post-parse.  */
  if (cc_tu_used_has_include)
    {
      cc_debug_line ("manifest-skip-has-include", cc_manifest_key_hex);
      return;
    }

  char *man_path = cc_entry_path (cc_manifest_key_hex, /*make_dirs=*/true);

  /* Read any existing manifest so we can preserve its other entries.  */
  size_t old_len = 0;
  unsigned char *old = cc_read_file (man_path, &old_len);
  bool old_valid = (old
		    && old_len >= CC_MANIFEST_HEADER_SIZE
		    && memcmp (old + CC_MAN_OFF_MAGIC, CC_MANIFEST_MAGIC,
			       CC_MAGIC_LEN) == 0
		    && cc_get_u16 (old + CC_MAN_OFF_VERSION)
			 == CC_MANIFEST_VERSION);

  /* Build the new manifest: a fresh string area + entries blob.  We re-emit
     preserved entries (rewriting their header paths into the new string area)
     plus this TU's entry.  */
  cc_blob strings = { NULL, 0, 0 };
  cc_blob entries = { NULL, 0, 0 };
  uint32_t entry_count = 0;

  /* Helper lambda-style emit of one entry given OK + per-header arrays.  Done
     inline (C++ here has no convenient closure over the blobs without a struct)
     so we keep two code paths: preserved entries and the new entry.  */

  /* (a) Preserve existing entries whose OK differs from ours.  */
  if (old_valid)
    {
      uint32_t ocount = cc_get_u32 (old + CC_MAN_OFF_ENTRY_COUNT);
      uint64_t ooff = cc_get_u64 (old + CC_MAN_OFF_ENTRIES_OFF);
      uint64_t ocur = ooff;
      for (uint32_t ei = 0; ei < ocount && ocur + 32 <= old_len; ei++)
	{
	  const unsigned char *ent = old + ocur;
	  unsigned char ok_raw[20];
	  memcpy (ok_raw, ent, 20);
	  uint32_t ow = cc_get_u32 (ent + 20);
	  uint32_t owe = cc_get_u32 (ent + 24);
	  uint32_t hc = cc_get_u32 (ent + 28);
	  uint64_t recs = ocur + 32;
	  uint64_t recs_len = (uint64_t) hc * CC_MAN_HDR_REC_SIZE;
	  if (recs + recs_len > old_len)
	    break;
	  /* Skip our own OK -- the fresh entry below supersedes it.  */
	  if (memcmp (ok_raw, cc_key_raw, 20) == 0)
	    {
	      ocur = recs + recs_len;
	      continue;
	    }
	  /* Re-emit this entry into the new blobs.  */
	  unsigned char head[32];
	  memcpy (head, ok_raw, 20);
	  cc_put_u32 (head + 20, ow);
	  cc_put_u32 (head + 24, owe);
	  cc_put_u32 (head + 28, hc);
	  cc_blob_append (&entries, head, 32);
	  for (uint32_t hi = 0; hi < hc; hi++)
	    {
	      const unsigned char *rec = old + recs
					 + (uint64_t) hi * CC_MAN_HDR_REC_SIZE;
	      uint32_t opath = cc_get_u32 (rec + CC_MHR_OFF_PATH);
	      const char *hpath = "";
	      if ((uint64_t) opath + 4 <= old_len)
		{
		  uint32_t plen = cc_get_u32 (old + opath);
		  if ((uint64_t) opath + 4 + plen + 1 <= old_len)
		    hpath = (const char *) (old + opath + 4);
		}
	      uint32_t npath = cc_blob_add_string (&strings, hpath);
	      unsigned char nrec[CC_MAN_HDR_REC_SIZE];
	      cc_put_u32 (nrec + CC_MHR_OFF_PATH, npath);	/* rebased later */
	      memcpy (nrec + CC_MHR_OFF_SIZE, rec + CC_MHR_OFF_SIZE, 8);
	      memcpy (nrec + CC_MHR_OFF_MTIME, rec + CC_MHR_OFF_MTIME, 8);
	      memcpy (nrec + CC_MHR_OFF_HASH, rec + CC_MHR_OFF_HASH, 20);
	      cc_blob_append (&entries, nrec, CC_MAN_HDR_REC_SIZE);
	    }
	  entry_count++;
	  ocur = recs + recs_len;
	}
    }
  free (old);

  /* (b) This TU's entry: OK + counts + the recorded include set.  */
  {
    unsigned char head[32];
    memcpy (head, cc_key_raw, 20);
    cc_put_u32 (head + 20, warnings);
    cc_put_u32 (head + 24, werrors);
    cc_put_u32 (head + 28, cc_meta.input_count);
    cc_blob_append (&entries, head, 32);
    for (unsigned i = 0; i < cc_meta.input_count; i++)
      {
	uint32_t npath = cc_blob_add_string (&strings, cc_meta.inputs[i].path);
	unsigned char nrec[CC_MAN_HDR_REC_SIZE];
	cc_put_u32 (nrec + CC_MHR_OFF_PATH, npath);		/* rebased later */
	cc_put_u64 (nrec + CC_MHR_OFF_SIZE, cc_meta.inputs[i].size);
	cc_put_u64 (nrec + CC_MHR_OFF_MTIME, cc_meta.inputs[i].mtime);
	memcpy (nrec + CC_MHR_OFF_HASH, cc_meta.inputs[i].hash, 20);
	cc_blob_append (&entries, nrec, CC_MAN_HDR_REC_SIZE);
      }
    entry_count++;
  }

  /* Layout: header -> entries -> string area.  Rebase every header record's
     path offset (currently relative to the string area) to an absolute file
     offset.  Walk the entries blob in lockstep with how we built it.  */
  uint64_t entries_off = CC_MANIFEST_HEADER_SIZE;
  uint64_t string_area_off = entries_off + entries.len;
  {
    uint64_t cur = 0;
    for (uint32_t ei = 0; ei < entry_count; ei++)
      {
	uint32_t hc = cc_get_u32 (entries.data + cur + 28);
	uint64_t recs = cur + 32;
	for (uint32_t hi = 0; hi < hc; hi++)
	  {
	    unsigned char *rec = entries.data + recs
				 + (uint64_t) hi * CC_MAN_HDR_REC_SIZE;
	    uint32_t rel = cc_get_u32 (rec + CC_MHR_OFF_PATH);
	    cc_put_u32 (rec + CC_MHR_OFF_PATH,
			rel + (uint32_t) string_area_off);
	  }
	cur = recs + (uint64_t) hc * CC_MAN_HDR_REC_SIZE;
      }
  }

  unsigned char mhdr[CC_MANIFEST_HEADER_SIZE];
  memset (mhdr, 0, sizeof (mhdr));
  memcpy (mhdr + CC_MAN_OFF_MAGIC, CC_MANIFEST_MAGIC, CC_MAGIC_LEN);
  cc_put_u16 (mhdr + CC_MAN_OFF_VERSION, (uint16_t) CC_MANIFEST_VERSION);
  cc_put_u16 (mhdr + CC_MAN_OFF_FLAGS, 0);
  cc_put_u32 (mhdr + CC_MAN_OFF_ENTRY_COUNT, entry_count);
  cc_put_u64 (mhdr + CC_MAN_OFF_ENTRIES_OFF, entries_off);

  /* Assemble the whole manifest into one buffer and publish atomically.  The
     manifest is NOT made read-only (it is rewritten as new sets appear).  */
  size_t total = (size_t) string_area_off + strings.len;
  unsigned char *buf = (unsigned char *) xmalloc (total ? total : 1);
  memcpy (buf, mhdr, sizeof (mhdr));
  if (entries.len)
    memcpy (buf + entries_off, entries.data, entries.len);
  if (strings.len)
    memcpy (buf + string_area_off, strings.data, strings.len);

  char *tmp = concat (man_path, ".tmpXXXXXX", NULL);
  int fd = mkstemp (tmp);
  bool ok = (fd >= 0);
  FILE *dst = ok ? fdopen (fd, "wb") : NULL;
  if (!dst && fd >= 0)
    {
      close (fd);
      ok = false;
    }
  if (ok)
    {
      ok = (total == 0) || (fwrite (buf, 1, total, dst) == total);
      if (fclose (dst) != 0)
	ok = false;
    }
  if (ok)
    {
      if (rename (tmp, man_path) != 0)
	unlink (tmp);
      else
	cc_debug_line ("manifest-store", cc_manifest_key_hex);
    }
  else if (fd >= 0)
    unlink (tmp);

  free (buf);
  free (tmp);
  free (entries.data);
  free (strings.data);
  free (man_path);
}

void
compile_cache_store (void)
{
  /* Always tear down the capture if it is active, even on the early-outs
     below, so the diagnostics still print and the printer stream is restored.
     The captured bytes/counts are only USED when we actually write an object.  */
  unsigned char *diag_bytes = NULL;
  size_t diag_len = 0;
  /* "errors" holds the DK_WERROR (warnings promoted by -Werror) delta; it goes
     into the header's errors field and is restored to werrorcount on a hit.
     Real DK_ERRORs are never stored (the seen_error() guard below bails).  */
  uint32_t warnings = 0, errors = 0;
  cc_end_backend_capture (&diag_bytes, &diag_len, &warnings, &errors);

  if (!compile_cache_enabled_p ()
      || !cc_key_valid		/* serve never ran / key untrusted */
      || cc_hit			/* nothing new to store on a hit */
      || seen_error ())		/* don't cache a failed compile */
    {
      free (diag_bytes);
      cc_meta_clear ();
      return;
    }

  /* The freshly produced OBJECT (.o) lives at asm_file_name: under
     -fintegrated-as (required, gating check #10) finalize() ran
     gas_assemble_buffer (integ_obj_path == asm_file_name) before we are
     called, so asm_file_name holds the finished .o.  In v3 we do NOT slurp it:
     the cache object IS that .o, hardlinked (reflink->hardlink->copy) into the
     content-addressed cache path so the store costs O(1) instead of a
     read-into-memory + rewrite that scaled with object size.  */

  /* Did the front end / parse emit any diagnostics?  If so, flag the object so
     the pre-parse manifest fast-path never serves it (it would skip the parse
     that re-emits those).  cc_fe_* were snapshotted when the back-end capture
     started, i.e. after the parse.  */
  bool had_fe_diag = (cc_fe_warnings > 0 || cc_fe_werrors > 0);
  uint16_t flags = (uint16_t) (had_fe_diag ? CC_FLAG_HAD_FE_DIAG : 0);

  /* Build the compact metadata record (the only fields a hit consumes) plus
     the diagnostics blob.  No inputs table, no header strings: those were
     write-only at serve time (the object key is a full-closure SHA-1 content
     address; the manifest carries its own include set), so v3 drops them.  */
  size_t meta_len = 0;
  unsigned char *meta = cc_build_meta (flags, warnings, errors,
				       diag_bytes, diag_len, &meta_len);

  /* Place the produced .o into the content-addressed cache slot
     (DIR/<2hex>/<rest>.o) via reflink->hardlink->copy.  cc_entry_path makes the
     shard dirs; the .o path is derived from it.  */
  char *bin_path = cc_entry_path (cc_key_hex, /*make_dirs=*/true);
  char *obj_path = cc_object_sidecar_path (bin_path);
  bool ok = cc_place_object (asm_file_name, obj_path);

  /* Attach the metadata as an xattr on the cache .o.  This MUST happen while the
     object is still writable: on Linux, setting a user.* xattr requires WRITE
     permission on the inode, so doing it after the 0444 chmod below would fail
     with EACCES on a non-root runner (root's CAP_DAC_OVERRIDE masks this, which
     is why it only surfaced in CI).  If the filesystem rejects user xattrs
     (ENOTSUP) or the record will not fit (E2BIG/ENOSPC/...), fall back to a
     minimal .bin sidecar carrying the same bytes -- for THIS entry only -- so
     functionality is preserved everywhere.  Logged once.  */
  if (ok)
    {
      bool unsupported = false;
      if (!cc_set_meta_xattr (obj_path, meta, meta_len, &unsupported))
	{
	  if (unsupported)
	    {
	      static bool warned = false;
	      if (!warned)
		{
		  warned = true;
		  cc_debug_line ("xattr-fallback", cc_key_hex);
		}
	      ok = cc_write_atomic_readonly (bin_path, meta, meta_len);
	    }
	  else
	    ok = false;		/* hard xattr error -> abandon this entry */
	}
    }

  /* Make the cache object read-only (0444) as the docstring promises: an
     accidental in-place rewrite of the cache (objcopy/strip --in-place) then
     fails loudly instead of silently corrupting it.  When the object was
     hardlinked to the output .o (same-fs store), the output .o becomes 0444
     too -- the documented "fail loudly" tradeoff, intended on the store side.
     This is the LAST step before publishing: the metadata xattr above needed a
     writable inode, so the chmod cannot precede it.  */
  if (ok)
    chmod (obj_path, 0444);

  if (ok)
    {
      cc_debug_line ("store", cc_key_hex);
      /* Record/refresh the manifest entry so a future run of the SAME source
	 can serve this object BEFORE parsing.  */
      cc_store_manifest (warnings, errors);
      /* Publish the compiler-id sidecar so the DRIVER can form the same
	 manifest key (it needs this compiler's checksum + lang name).  */
      cc_write_compiler_id ();
    }
  else
    unlink (obj_path);		/* clean up a placed-but-unannotated object */

  free (meta);
  free (diag_bytes);
  free (obj_path);
  free (bin_path);
  cc_meta_clear ();
}
