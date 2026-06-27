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
#include "compile-cache.h"

#include <sys/stat.h>

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
/* Binary object format                                                     */
/* ------------------------------------------------------------------------ */

/* Each cache entry is ONE little-endian binary file at
   "DIR/<2hex>/<rest>.bin" (no sidecars), laid out as:

       header (128 bytes)  ->  inputs table  ->  string area
                           ->  assembly      ->  diagnostics

   Strings are stored length-prefixed: a u32 byte count (NOT counting the
   trailing NUL) + the content bytes + one '\0'.  A string is *referenced* by
   a u32 file offset that points at its u32 length prefix, so C reads the
   length at off and the chars at off+4 (which is a NUL-terminated char*).  */

#define CC_MAGIC      "GCCCACHE"	/* 8 bytes, no NUL stored */
#define CC_MAGIC_LEN  8
#define CC_FORMAT_VERSION  1u
#define CC_HEADER_SIZE     128u

/* Fixed-header field byte offsets.  */
enum cc_hdr_off
{
  CC_OFF_MAGIC        = 0,	/* char[8]  "GCCCACHE"            */
  CC_OFF_FORMAT_VER   = 8,	/* u16      format_version = 1     */
  CC_OFF_FLAGS        = 10,	/* u16      flags = 0              */
  CC_OFF_INPUT_COUNT  = 12,	/* u32      number of inputs       */
  CC_OFF_CREATED      = 16,	/* u64      time(NULL), Unix s UTC */
  CC_OFF_ASM_OFF      = 24,	/* u64      assembly section off    */
  CC_OFF_ASM_LEN      = 32,	/* u64      assembly section len    */
  CC_OFF_DIAG_OFF     = 40,	/* u64      diagnostics section off */
  CC_OFF_DIAG_LEN     = 48,	/* u64      diagnostics section len */
  CC_OFF_INPUTS_OFF   = 56,	/* u64      inputs table off        */
  CC_OFF_WARNINGS     = 64,	/* u32      back-end warning count  */
  CC_OFF_ERRORS       = 68,	/* u32      back-end error count    */
  CC_OFF_KEY          = 72,	/* u8[20]   raw SHA-1 key           */
  CC_OFF_CHECKSUM     = 92,	/* u8[16]   executable_checksum     */
  CC_OFF_SOURCE_OFF   = 108,	/* u32 -> source string             */
  CC_OFF_CWD_OFF      = 112,	/* u32 -> cwd string                */
  CC_OFF_TARGET_OFF   = 116,	/* u32 -> target string             */
  CC_OFF_LANGUAGE_OFF = 120,	/* u32 -> language string           */
  CC_OFF_OPTIONS_OFF  = 124	/* u32 -> options string            */
};

/* Each inputs-table record is 32 bytes: raw SHA-1 (20) + size (u64) +
   path_off (u32).  */
#define CC_INPUT_REC_SIZE  32u
enum cc_input_off
{
  CC_IN_OFF_HASH = 0,		/* u8[20]  raw SHA-1 of the file */
  CC_IN_OFF_SIZE = 20,		/* u64     file size in bytes    */
  CC_IN_OFF_PATH = 28		/* u32 -> path string            */
};

/* Little-endian store helpers (do NOT rely on host endianness / packing).  */
static void
cc_put_u16 (unsigned char *p, uint16_t v)
{
  p[0] = (unsigned char) (v & 0xff);
  p[1] = (unsigned char) ((v >> 8) & 0xff);
}

static void
cc_put_u32 (unsigned char *p, uint32_t v)
{
  for (int i = 0; i < 4; i++)
    p[i] = (unsigned char) ((v >> (8 * i)) & 0xff);
}

static void
cc_put_u64 (unsigned char *p, uint64_t v)
{
  for (int i = 0; i < 8; i++)
    p[i] = (unsigned char) ((v >> (8 * i)) & 0xff);
}

/* Little-endian load helpers (used by the serve path).  */
static uint16_t
cc_get_u16 (const unsigned char *p)
{
  return (uint16_t) (p[0] | ((uint16_t) p[1] << 8));
}

static uint32_t
cc_get_u32 (const unsigned char *p)
{
  uint32_t v = 0;
  for (int i = 0; i < 4; i++)
    v |= (uint32_t) p[i] << (8 * i);
  return v;
}

static uint64_t
cc_get_u64 (const unsigned char *p)
{
  uint64_t v = 0;
  for (int i = 0; i < 8; i++)
    v |= (uint64_t) p[i] << (8 * i);
  return v;
}

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

/* ------------------------------------------------------------------------ */
/* Object metadata, gathered during key computation                         */
/* ------------------------------------------------------------------------ */

/* One included input file's identity for the inputs table.  */
struct cc_input
{
  char *path;			/* xstrdup'd path */
  uint64_t size;		/* byte count */
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
cc_meta_add_input (const char *path, uint64_t size,
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

/* Component tags (one byte each), participating in the key.  */
enum cc_tag
{
  CC_TAG_CHECKSUM = 1,	/* executable_checksum[16] */
  CC_TAG_LANG = 2,	/* lang_hooks.name */
  CC_TAG_STD = 3,	/* reserved (dialect is folded into lang + options) */
  CC_TAG_FILE_PATH = 4,	/* one included file's path */
  CC_TAG_FILE_BODY = 5,	/* that file's bytes */
  CC_TAG_OPT = 6,	/* one canonicalized command-line option token */
  CC_TAG_MAIN_INPUT = 7,/* main_input_filename */
  CC_TAG_CWD = 8,	/* current working directory */
  CC_TAG_VERSION = 9,	/* key-schema version */
  CC_TAG_SALT = 10	/* GCC_COMPILE_CACHE_SALT (logical cache reset) */
};

/* Bump when the key construction or cached payload format changes, to
   invalidate stale entries written by an older compiler.  Bumped to 2 with
   the structured binary object format + diagnostic capture (was 1 for the
   raw-.s entries).  Bumped to 3 when the key was made content-addressed:
   search-path option *values* (-I/-isystem/...) and the main input path /
   cwd / per-include-file paths were dropped from the key (the resolved file
   *contents* already cover them), so the key no longer folds in build-dir
   path strings and hits across different build directories and machines.
   Path components are now hashed only when output truly depends on them
   (under -g, where paths are baked into DWARF).  */
#define CC_KEY_SCHEMA_VERSION 3u

/* Feed a 1-byte tag, an 8-byte little-endian length, then the bytes, into
   CTX.  The tag+length framing makes the key unambiguous.  */
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

/* Render 20 raw SHA-1 bytes into OUT[41] as lowercase hex + NUL.  */
static void
cc_hex (const unsigned char raw[20], char out[41])
{
  static const char hexd[] = "0123456789abcdef";
  for (int i = 0; i < 20; i++)
    {
      out[2 * i] = hexd[(raw[i] >> 4) & 0xf];
      out[2 * i + 1] = hexd[raw[i] & 0xf];
    }
  out[40] = '\0';
}

/* ------------------------------------------------------------------------ */
/* Option filtering                                                         */
/* ------------------------------------------------------------------------ */

/* Return true if DECODED is a command-line option that can change the
   generated assembly / ABI and therefore must participate in the key.

   Policy: conservative-but-broad.  We would rather over-include an option
   (an unnecessary miss) than under-include one (a WRONG hit serving stale
   assembly).  When in doubt, INCLUDE.  */
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
  cc_meta_add_input (path, (uint64_t) size, fh);
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

bool
compile_cache_try_serve (cpp_reader *pfile)
{
  if (!compile_cache_enabled_p ())
    return false;
  if (!pfile)
    return false;

  if (!cc_compute_key (pfile))
    return false;		/* key untrustworthy -> behave as a miss */

  char *path = cc_entry_path (cc_key_hex, /*make_dirs=*/false);

  size_t flen = 0;
  unsigned char *file = cc_read_file (path, &flen);
  free (path);

  /* MISS: no object, too small, wrong magic, or wrong format version are all
     treated as a miss (the wrong-magic/version case is self-healing: we will
     overwrite the stale object on store).  Install the back-end diagnostic
     capture so the store can record them.  */
  if (!file
      || flen < CC_HEADER_SIZE
      || memcmp (file + CC_OFF_MAGIC, CC_MAGIC, CC_MAGIC_LEN) != 0
      || cc_get_u16 (file + CC_OFF_FORMAT_VER) != CC_FORMAT_VERSION)
    {
      free (file);
      cc_debug_line ("miss", cc_key_hex);
      cc_begin_backend_capture ();
      return false;
    }

  /* Parse the section table and bounds-check every span against the file.  */
  uint64_t asm_off = cc_get_u64 (file + CC_OFF_ASM_OFF);
  uint64_t asm_len = cc_get_u64 (file + CC_OFF_ASM_LEN);
  uint64_t diag_off = cc_get_u64 (file + CC_OFF_DIAG_OFF);
  uint64_t diag_len = cc_get_u64 (file + CC_OFF_DIAG_LEN);
  uint32_t warnings = cc_get_u32 (file + CC_OFF_WARNINGS);
  uint32_t errors = cc_get_u32 (file + CC_OFF_ERRORS);

  if (asm_off > flen || asm_len > flen - asm_off
      || diag_off > flen || diag_len > flen - diag_off)
    {
      /* Structurally corrupt: ignore and recompute (self-healing).  */
      free (file);
      cc_debug_line ("miss", cc_key_hex);
      cc_begin_backend_capture ();
      return false;
    }

  /* Hit: replace asm_out_file's contents with the cached assembly.
     init_asm_output() has already written the target preamble into
     asm_out_file; the cached assembly is the *complete* final assembly (it has
     its own preamble), so we substitute the stored bytes wholesale.  Close and
     reopen with "wb" (which truncates) -- GCC's config does not probe
     ftruncate.  finalize() will close this fresh handle as usual.
     asm_file_name is a real, seekable path here (gated above).  Once we have
     committed to truncating the back-end's output, a reopen failure is fatal:
     there is no correct output to fall back to.  */
  fclose (asm_out_file);
  asm_out_file = fopen (asm_file_name, "wb");
  if (!asm_out_file)
    {
      free (file);
      fatal_error (input_location,
		   "compilation cache: cannot reopen %qs", asm_file_name);
    }

  bool ok = true;
  if (asm_len)
    ok = (fwrite (file + asm_off, 1, (size_t) asm_len, asm_out_file)
	  == (size_t) asm_len);

  if (!ok)
    {
      /* Partial write of the assembly: we have already truncated the file, so
	 there is no correct fallback -- but treat it as a miss so the caller
	 runs the back-end and regenerates correct output.  */
      free (file);
      cc_debug_line ("miss", cc_key_hex);
      cc_begin_backend_capture ();
      return false;
    }
  fflush (asm_out_file);

  /* Replay the cached back-end diagnostics to stderr and fold the stored
     counts into the global counters so the "N warnings"/"M errors" summary
     and the -Werror exit status match a fresh compile.  Front-end/parse
     diagnostics re-emit naturally on the re-parse, so we only replay (and
     only counted) the back-end phase here.  The warnings field restores
     DK_WARNING (plain warnings); the errors field carries DK_WERROR (warnings
     promoted by -Werror) and restores to werrorcount, which is what drives
     the non-zero exit under -Werror.  (A real back-end DK_ERROR is never
     cached: compile_cache_store() bails when seen_error() is true.)  */
  if (diag_len)
    {
      fflush (stdout);
      fwrite (file + diag_off, 1, (size_t) diag_len, stderr);
      fflush (stderr);
    }
  if (global_dc)
    {
      global_dc->diagnostic_count (DK_WARNING) += (int) warnings;
      global_dc->diagnostic_count (DK_WERROR) += (int) errors;
    }

  free (file);
  cc_hit = true;
  cc_debug_line ("hit", cc_key_hex);
  return true;
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

  /* The freshly produced assembly lives at asm_file_name and has been closed
     by finalize() before we are called.  Slurp it.  */
  size_t asm_len = 0;
  unsigned char *asm_bytes = cc_read_file (asm_file_name, &asm_len);
  if (!asm_bytes)
    {
      free (diag_bytes);
      cc_meta_clear ();
      return;
    }

  /* Build the string area: the 5 header strings, then every input path.  The
     per-input path offsets are captured for the inputs table.  */
  cc_blob strings = { NULL, 0, 0 };
  uint32_t source_off = cc_blob_add_string (&strings, cc_meta.source);
  uint32_t cwd_off = cc_blob_add_string (&strings, cc_meta.cwd);
  uint32_t target_off = cc_blob_add_string (&strings, cc_meta.target);
  uint32_t language_off = cc_blob_add_string (&strings, cc_meta.language);
  uint32_t options_off = cc_blob_add_string (&strings, cc_meta.options);

  uint32_t *path_offs = NULL;
  if (cc_meta.input_count)
    path_offs = XNEWVEC (uint32_t, cc_meta.input_count);
  for (unsigned i = 0; i < cc_meta.input_count; i++)
    path_offs[i] = cc_blob_add_string (&strings, cc_meta.inputs[i].path);

  /* Section layout: header -> inputs table -> string area -> asm -> diag.
     The string offsets above are relative to the string area; rebase them to
     absolute file offsets now that the area's position is known.  */
  uint64_t inputs_off = CC_HEADER_SIZE;
  uint64_t inputs_size = (uint64_t) cc_meta.input_count * CC_INPUT_REC_SIZE;
  uint64_t string_area_off = inputs_off + inputs_size;
  uint64_t asm_off = string_area_off + strings.len;
  uint64_t diag_off = asm_off + asm_len;

  source_off += (uint32_t) string_area_off;
  cwd_off += (uint32_t) string_area_off;
  target_off += (uint32_t) string_area_off;
  language_off += (uint32_t) string_area_off;
  options_off += (uint32_t) string_area_off;
  for (unsigned i = 0; i < cc_meta.input_count; i++)
    path_offs[i] += (uint32_t) string_area_off;

  /* Compose the fixed header.  */
  unsigned char hdr[CC_HEADER_SIZE];
  memset (hdr, 0, sizeof (hdr));
  memcpy (hdr + CC_OFF_MAGIC, CC_MAGIC, CC_MAGIC_LEN);
  cc_put_u16 (hdr + CC_OFF_FORMAT_VER, (uint16_t) CC_FORMAT_VERSION);
  cc_put_u16 (hdr + CC_OFF_FLAGS, 0);
  cc_put_u32 (hdr + CC_OFF_INPUT_COUNT, cc_meta.input_count);
  cc_put_u64 (hdr + CC_OFF_CREATED, (uint64_t) time (NULL));
  cc_put_u64 (hdr + CC_OFF_ASM_OFF, asm_off);
  cc_put_u64 (hdr + CC_OFF_ASM_LEN, (uint64_t) asm_len);
  cc_put_u64 (hdr + CC_OFF_DIAG_OFF, diag_off);
  cc_put_u64 (hdr + CC_OFF_DIAG_LEN, (uint64_t) diag_len);
  cc_put_u64 (hdr + CC_OFF_INPUTS_OFF, inputs_off);
  cc_put_u32 (hdr + CC_OFF_WARNINGS, warnings);
  cc_put_u32 (hdr + CC_OFF_ERRORS, errors);
  memcpy (hdr + CC_OFF_KEY, cc_key_raw, 20);
  memcpy (hdr + CC_OFF_CHECKSUM, executable_checksum, 16);
  cc_put_u32 (hdr + CC_OFF_SOURCE_OFF, source_off);
  cc_put_u32 (hdr + CC_OFF_CWD_OFF, cwd_off);
  cc_put_u32 (hdr + CC_OFF_TARGET_OFF, target_off);
  cc_put_u32 (hdr + CC_OFF_LANGUAGE_OFF, language_off);
  cc_put_u32 (hdr + CC_OFF_OPTIONS_OFF, options_off);

  /* Compose the inputs table (32 bytes each).  */
  unsigned char *intab = NULL;
  if (inputs_size)
    {
      intab = (unsigned char *) xmalloc ((size_t) inputs_size);
      for (unsigned i = 0; i < cc_meta.input_count; i++)
	{
	  unsigned char *rec = intab + (size_t) i * CC_INPUT_REC_SIZE;
	  memcpy (rec + CC_IN_OFF_HASH, cc_meta.inputs[i].hash, 20);
	  cc_put_u64 (rec + CC_IN_OFF_SIZE, cc_meta.inputs[i].size);
	  cc_put_u32 (rec + CC_IN_OFF_PATH, path_offs[i]);
	}
    }

  /* Write everything to a temp file in the shard dir, then atomic-rename.  */
  char *final_path = cc_entry_path (cc_key_hex, /*make_dirs=*/true);
  char *tmp_path = concat (final_path, ".tmpXXXXXX", NULL);
  int tfd = mkstemp (tmp_path);
  bool ok = (tfd >= 0);
  FILE *dst = ok ? fdopen (tfd, "wb") : NULL;
  if (!dst && tfd >= 0)
    {
      close (tfd);
      ok = false;
    }

  if (ok)
    {
      ok = (fwrite (hdr, 1, sizeof (hdr), dst) == sizeof (hdr));
      if (ok && inputs_size)
	ok = (fwrite (intab, 1, (size_t) inputs_size, dst)
	      == (size_t) inputs_size);
      if (ok && strings.len)
	ok = (fwrite (strings.data, 1, strings.len, dst) == strings.len);
      if (ok && asm_len)
	ok = (fwrite (asm_bytes, 1, asm_len, dst) == asm_len);
      if (ok && diag_len)
	ok = (fwrite (diag_bytes, 1, diag_len, dst) == diag_len);
      if (fclose (dst) != 0)
	ok = false;
    }

  if (ok)
    {
      /* Atomic publish.  If another process won the race the rename simply
	 replaces an identical-keyed entry; harmless.  */
      if (rename (tmp_path, final_path) != 0)
	unlink (tmp_path);
      else
	cc_debug_line ("store", cc_key_hex);
    }
  else if (tfd >= 0)
    unlink (tmp_path);

  free (intab);
  free (path_offs);
  free (strings.data);
  free (asm_bytes);
  free (diag_bytes);
  free (tmp_path);
  free (final_path);
  cc_meta_clear ();
}
