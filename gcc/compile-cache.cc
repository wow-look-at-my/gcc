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
#include "../libcpp/include/mkdeps.h"  /* deps_add_dep: complete the .d on a
					  pre-parse manifest hit */
#include "sha1.h"
#include "compile-cache-format.h"	/* shared on-disk format + LE helpers */
#include "compile-cache-serve.h"	/* shared driver-usable serve unit */
#include "compile-cache.h"

/* The option predicates that build the manifest key are defined ONCE in the
   shared serve unit (compile-cache-serve.cc) so the driver and cc1plus compute
   an identical key.  Declared here (extern) for the store path's use.  */
extern bool cc_option_affects_output_p (const cl_decoded_option *decoded);
extern bool cc_option_is_search_path_p (const cl_decoded_option *decoded);
/* Likewise the search-path normalization the MK twins share (ccache
   base_dir parity; see its definition for the collision-bar discussion).  */
extern const char *cc_mk_search_path_relative (const char *path,
					       const char *cwd);

#include <sys/stat.h>
#include <dirent.h>		/* shard scans for B1 eviction */
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

/* PCH consumption state (set from c_common_read_pch via
   compile_cache_note_pch_read, DURING parsing -- after enabled_p gating).
   A consumed FOREIGN pch (anything but the auto-PCH stub this compile was
   handed via -fauto-pch-ref) hides its baked-in include closure from the
   cpp_foreach_included_file walk, so neither a trustworthy key nor a
   closure-complete manifest can be built: serve/store must bail.  Our own
   stub is fine -- its recorded closure is merged back in from the gch
   manifest by cc_merge_gch_manifest ().  */
static bool cc_pch_consumed = false;
static bool cc_pch_consumed_ours = false;

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

/* True if this TU evaluated __has_include_next (captured from the cpp_reader
   at post-parse time).  Its result depends on the include-stack position of
   the probing file, which the pre-parse serve cannot reconstruct, so
   compile_cache_store () writes NO manifest entry for such a TU
   ("manifest-skip-has-include-next").  Plain __has_include no longer
   disqualifies: its evaluations are recorded per probe (cc_meta.probes),
   folded into the object key, stored in the manifest, and re-verified at
   serve time.  The post-parse object cache still applies either way.  */
static bool cc_tu_used_has_include_next = false;

/* True if this TU evaluated at least one __has_include probe whose search
   the serve side cannot faithfully re-run from records (relative quote form,
   -remap, header-map directories -- anything libcpp did not mark
   CPP_HI_PROBE_VERIFIABLE), or consumed a gch whose manifest carries the
   CC_MAN_EFLAG_UNVERIFIED_PROBES flag.  Set while collecting probes in
   cc_compute_key; makes compile_cache_store () skip the manifest
   ("manifest-skip-has-include") -- the status quo for such TUs.  */
static bool cc_tu_has_unverifiable_probe = false;

/* ------------------------------------------------------------------------ */
/* Object metadata, gathered during key computation                         */
/* ------------------------------------------------------------------------ */

/* One included input file's identity for the inputs table.  */
struct cc_input
{
  char *path;			/* xstrdup'd path */
  uint64_t size;		/* byte count (of the hashed content) */
  struct cc_statid id;		/* full stat identity for the stat shortcut */
  uint32_t mhr_flags;		/* CC_MHR_FLAG_* (HAS_STATID when ID holds) */
  unsigned char hash[20];	/* raw SHA-1 of the bytes */
};

/* One recorded __has_include evaluation (mirrors libcpp's record; see
   cpp_foreach_has_include_probe).  FLAGS is the CPP_HI_PROBE_* mask.  */
struct cc_probe
{
  char *name;			/* operand spelling, post macro expansion */
  char *resolved;		/* found: resolved path, else NULL */
  char **candidates;		/* paths the search proved absent */
  unsigned n_candidates;
  unsigned flags;
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
  cc_probe *probes;		/* recorded __has_include evaluations */
  unsigned probe_count;
  unsigned probe_cap;
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
  for (unsigned i = 0; i < cc_meta.probe_count; i++)
    {
      free (cc_meta.probes[i].name);
      free (cc_meta.probes[i].resolved);
      for (unsigned k = 0; k < cc_meta.probes[i].n_candidates; k++)
	free (cc_meta.probes[i].candidates[k]);
      free (cc_meta.probes[i].candidates);
    }
  free (cc_meta.probes);
  memset (&cc_meta, 0, sizeof (cc_meta));
}

static void
cc_meta_add_input (const char *path, uint64_t size,
		   const struct cc_statid *id, uint32_t mhr_flags,
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
  in->id = *id;
  in->mhr_flags = mhr_flags;
  memcpy (in->hash, hash, 20);
}

/* Second of the compile's start, for the stat-identity "too new" guard
   below.  Set (once) in compile_cache_init_determinism (), which runs at the
   very start of c_common_parse_file -- before the preprocessor reads any
   header.  */
static time_t cc_compile_start_time = 0;

/* Capture PATH's full stat identity into *ID for a header record whose
   hashed content was READ_SIZE bytes.  Returns CC_MHR_FLAG_HAS_STATID when
   the identity can be TRUSTED to describe those hashed bytes, else 0 (with
   *ID zeroed):

     - the stat must succeed and report exactly READ_SIZE bytes (a differing
       size means the file changed between cpp reading it and now, so the
       identity describes different content than the hash);
     - the file's mtime and ctime must predate this compile's start second
       (ccache's bar): a file written during or after the compile started
       could be rewritten again with an indistinguishable same-second stamp,
       so its identity cannot vouch for its content yet.  The next store
       refreshes it; until then a serve simply re-hashes that one file.  */
static uint32_t
cc_capture_statid (const char *path, uint64_t read_size,
		   struct cc_statid *id)
{
  memset (id, 0, sizeof (*id));
  struct stat stt;
  if (stat (path, &stt) != 0)
    return 0;
  cc_statid_from_stat (&stt, id);
  if (id->size != read_size)
    {
      memset (id, 0, sizeof (*id));
      return 0;
    }
  if (cc_compile_start_time > 0
      && ((time_t) id->mtime_s >= cc_compile_start_time
	  || (time_t) id->ctime_s >= cc_compile_start_time))
    {
      memset (id, 0, sizeof (*id));
      return 0;
    }
  return CC_MHR_FLAG_HAS_STATID;
}

/* Append one probe record (deep-copying every string) to the growable vec
   *V/*N/*CAP.  Shared by the TU metadata (cc_meta.probes) and the auto-PCH
   manifest writer's local collection.  */
static void
cc_probe_vec_add (cc_probe **v, unsigned *n, unsigned *cap,
		  const char *name, unsigned flags, const char *resolved,
		  const char *const *candidates, unsigned n_candidates)
{
  if (*n == *cap)
    {
      unsigned ncap = *cap ? *cap * 2 : 8;
      *v = XRESIZEVEC (cc_probe, *v, ncap);
      *cap = ncap;
    }
  cc_probe *p = &(*v)[(*n)++];
  p->name = xstrdup (name);
  p->resolved = resolved ? xstrdup (resolved) : NULL;
  p->n_candidates = n_candidates;
  p->candidates = n_candidates ? XNEWVEC (char *, n_candidates) : NULL;
  for (unsigned k = 0; k < n_candidates; k++)
    p->candidates[k] = xstrdup (candidates[k]);
  p->flags = flags;
}

static void
cc_meta_add_probe (const char *name, unsigned flags, const char *resolved,
		   const char *const *candidates, unsigned n_candidates)
{
  cc_probe_vec_add (&cc_meta.probes, &cc_meta.probe_count,
		    &cc_meta.probe_cap, name, flags, resolved, candidates,
		    n_candidates);
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
compile_cache_configured_p (void)
{
  /* Deliberately does NOT touch cc_enabled / cc_dir: this runs early (from
     c_common_post_options, before asm_file_name and friends exist), and
     latching the tri-state here would freeze compile_cache_enabled_p () on
     incomplete state.  */
  const char *dir = compile_cache_dir;
  if (!dir || !dir[0])
    dir = getenv ("GCC_COMPILE_CACHE_DIR");
  return dir && dir[0];
}

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
     gas_assemble_buffer).  With -fno-integrated-as (the restored external-as
     pipeline, also auto-selected by the driver when -Wa,/-Xassembler options
     are present) cc1plus emits only text and the external as's object is not
     guaranteed byte-identical to an in-process-assembled one -- so such
     compiles are ineligible for BOTH serve and store, by design: soundness
     over speed.  The driver tier skips them for the same reason (its own
     skip-no-integrated-as tag in driver_try_serve_from_cache).  */
  if (!flag_integrated_as)
    {
      cc_debug_line ("skip-no-integrated-as", NULL);
      return false;
    }

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

/* B2: the prefix maps active while cc_compute_key runs -- shared by the
   closure-walk callback (cc_hash_one_file) and the auto-PCH gch merge
   (cc_merge_gch_manifest), both of which hash file PATHS under -g and must
   hash the same REWRITTEN strings DWARF will contain.  Populated/freed by
   cc_compute_key; empty (count == 0, remap == identity) without -g or
   without map options.  cc1 processes one TU per process, so file-scope
   state is safe here (matches cc_meta et al.).  */
static struct cc_prefix_maps cc_key_pm;

/* Callback: fold one included file's path + content DIGEST into the TU key,
   and record its identity (path + size + per-file SHA-1) for the inputs table.
   Both the key and the object metadata are produced from this single walk.

   CONTENT_SHA1, when non-NULL, is the 20-byte SHA-1 of the file's raw on-disk
   bytes that libcpp computed once when the compiler first read the file.  We
   feed that stored digest straight into the key (and reuse it as the inputs-
   table per-file hash) -- so a cache MISS never re-opens or re-reads the
   include closure just to hash it.  Only when no digest is available (a file
   that did not go through read_file_guts) does libcpp hand us BUFFER/SIZE and
   we hash the bytes ourselves, preserving correctness on that fallback path.

   Note this folds the per-file 20-byte digest into the key in dedup'd
   all_files order, in place of the file's raw bytes -- the same content
   commitment (a SHA-1 over the raw on-disk bytes), in the same walk order, but
   it changes the key VALUE, which is why CC_KEY_SCHEMA_VERSION is bumped.  */
static bool
cc_hash_one_file (const char *path, const unsigned char *buffer,
		  size_t size, const unsigned char *content_sha1,
		  unsigned file_flags, void *user)
{
  struct cc_closure_state *st = (struct cc_closure_state *) user;

  /* The file's CONTENTS are always part of the key; its PATH only when the
     produced bytes depend on it (under -g -- see cc_paths_affect_output_p).
     Dropping the path off the non-debug key is what makes an identical header
     reached via a differently named include tree hash the same.  The inputs
     table below still records the real path for diagnostics either way.
     B2: DWARF contains the path AFTER the -f{file,debug}-prefix-map
     rewrites, so the key hashes the mapped string (identity without maps;
     this also closes the deep-tier hole where two differently-mapped
     compiles of headers under a dropped prefix would otherwise collide).  */
  if (cc_paths_affect_output_p ())
    {
      char *mp = (cc_key_pm.count
		  ? cc_pmaps_remap_alloc (&cc_key_pm, path) : NULL);
      cc_hash_str (st->ctx, CC_TAG_FILE_PATH, mp ? mp : path);
      free (mp);
    }

  /* Per-file SHA-1 (also the inputs-table per-file hash).  Prefer the stored
     raw-bytes digest from libcpp (no re-read); otherwise hash the bytes we
     were handed.  */
  unsigned char fh[20];
  if (content_sha1)
    memcpy (fh, content_sha1, 20);
  else
    {
      struct sha1_ctx fctx;
      sha1_init_ctx (&fctx);
      if (size)
	sha1_process_bytes (buffer, size, &fctx);
      sha1_finish_ctx (&fctx, fh);
    }

  /* Commit the file's content to the TU key via its 20-byte digest.  Framed
     under CC_TAG_FILE_BODY (a 1-byte tag + 8-byte length + the 20 digest
     bytes) -- the closure is walked in the unchanged all_files order.  */
  cc_hash_component (st->ctx, CC_TAG_FILE_BODY, fh, 20);

  /* Capture the file's full stat identity for the manifest stat-shortcut (an
     identity match accepts a header on a hit without re-reading it).
     Best-effort: an untrusted identity (stat failure, changed size, too-new
     stamps) just forces a content re-hash on the next hit.  Carry libcpp's
     per-file facts into the record flags: the -MM/-MMD exclusion bit and the
     main-source mark (see compile-cache-format.h CC_MHR_FLAG_*).  */
  struct cc_statid id;
  uint32_t mflags = cc_capture_statid (path, (uint64_t) size, &id);
  if (file_flags & CPP_INCLUDED_FILE_SYSP)
    mflags |= CC_MHR_FLAG_SYSHDR;
  if (file_flags & CPP_INCLUDED_FILE_MAIN)
    mflags |= CC_MHR_FLAG_MAIN_SOURCE;
  cc_meta_add_input (path, (uint64_t) size, &id, mflags, fh);
  return true;			/* keep walking */
}

/* Fold one __has_include probe's RESULT into the key: the operand spelling
   plus its output-affecting facts (form + found bit).  Deliberately NOT the
   resolved path or the candidates -- paths would break the non-debug key's
   build-tree independence, and only the boolean result (per spelling and
   form) reaches the preprocessed tokens.  A flipped probe usually changes
   the output WITHOUT changing the include closure (it just changes a
   #define), so without this component the closure-content key would keep
   serving the stale object -- and, a hit storing nothing, would never
   refresh the manifest either.  */
static void
cc_hash_probe_result (struct sha1_ctx *ctx, const char *name, unsigned flags)
{
  unsigned char fb
    = (unsigned char) (flags & (CPP_HI_PROBE_FOUND | CPP_HI_PROBE_BRACKET
				| CPP_HI_PROBE_NEXT));
  cc_hash_component (ctx, CC_TAG_HAS_INCLUDE, &fb, 1);
  cc_hash_str (ctx, CC_TAG_HAS_INCLUDE, name);
}

/* cpp_has_include_probe_cb: fold one recorded probe into the TU key and
   capture it in cc_meta.probes for the manifest store.  An unverifiable
   probe still keys (its result affects the output) but flags the TU as
   manifest-ineligible.  */
static bool
cc_collect_one_probe (const char *name, unsigned flags, const char *resolved,
		      const char *const *candidates, unsigned n_candidates,
		      void *user)
{
  struct sha1_ctx *ctx = (struct sha1_ctx *) user;

  cc_hash_probe_result (ctx, name, flags);
  cc_meta_add_probe (name, flags, resolved, candidates, n_candidates);
  if (!(flags & CPP_HI_PROBE_VERIFIABLE))
    cc_tu_has_unverifiable_probe = true;
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

/* Auto-PCH: fold the consumed .gch's recorded include closure (the gch
   manifest next to the -fauto-pch-ref stub) into the TU key and the inputs
   table, and its recorded __has_include probes into the key and
   cc_meta.probes.  With a PCH loaded, libcpp never stacks the prelude
   headers NOR re-evaluates their probes, so the cpp walks cannot see either
   -- without this merge the stored manifest would validate a TU whose
   prelude headers (or probe results) changed and serve a stale object.  The
   merged records use the digests captured when the .gch was built, which
   the driver's probe re-validated against the filesystem before injecting.
   Returns false on any parse problem OR on a gch flagged
   CC_MAN_EFLAG_UNVERIFIED_PROBES -- its probe results are not fully on
   record, so no trustworthy key exists (the driver refuses to inject such a
   PCH in the first place; this is the belt-and-braces).  The caller must
   then distrust the key.  */
static bool
cc_merge_gch_manifest (struct sha1_ctx *ctx)
{
  const char *stub = flag_auto_pch_ref;
  if (!stub || !stub[0])
    return false;

  /* dirname (stub) + "/manifest".  */
  const char *slash = NULL;
  for (const char *p = stub; *p; p++)
    if (IS_DIR_SEPARATOR (*p))
      slash = p;
  if (!slash)
    return false;
  char *man_path = (char *) xmalloc ((slash - stub) + sizeof ("/manifest"));
  memcpy (man_path, stub, slash - stub);
  strcpy (man_path + (slash - stub), "/manifest");

  size_t mlen = 0;
  unsigned char *man = cc_read_file (man_path, &mlen);
  free (man_path);

  bool ok = false;
  struct cc_man_entry ent;
  if (man
      && mlen >= CC_MANIFEST_HEADER_SIZE
      && memcmp (man + CC_MAN_OFF_MAGIC, CC_MANIFEST_MAGIC, CC_MAGIC_LEN) == 0
      && cc_get_u16 (man + CC_MAN_OFF_VERSION) == CC_MANIFEST_VERSION
      && cc_get_u32 (man + CC_MAN_OFF_ENTRY_COUNT) == 1
      && cc_man_entry_parse (man, mlen,
			     cc_get_u64 (man + CC_MAN_OFF_ENTRIES_OFF), &ent)
      && ent.eflags == 0)
    {
      ok = true;

      /* (a) The prelude's include closure.  */
      for (uint32_t hi = 0; hi < ent.hdr_count && ok; hi++)
	{
	  const unsigned char *rec = man + ent.hdr_recs_off
				     + (uint64_t) hi * CC_MAN_HDR_REC_SIZE;
	  const char *hpath
	    = cc_man_string (man, mlen, cc_get_u32 (rec + CC_MHR_OFF_PATH));
	  uint32_t mflags = cc_get_u32 (rec + CC_MHR_OFF_FLAGS);
	  if (!hpath || (mflags & ~CC_MHR_FLAG_KNOWN_MASK))
	    {
	      ok = false;	/* unknown flag bits: future format */
	      break;
	    }
	  uint64_t sz = cc_get_u64 (rec + CC_MHR_OFF_SIZE);
	  struct cc_statid id;
	  id.size = sz;
	  id.mtime_s = cc_get_u64 (rec + CC_MHR_OFF_MTIME);
	  id.ctime_s = cc_get_u64 (rec + CC_MHR_OFF_CTIME);
	  id.dev = cc_get_u64 (rec + CC_MHR_OFF_DEV);
	  id.ino = cc_get_u64 (rec + CC_MHR_OFF_INO);
	  id.mtime_ns = cc_get_u32 (rec + CC_MHR_OFF_MTIME_NSEC);
	  id.ctime_ns = cc_get_u32 (rec + CC_MHR_OFF_CTIME_NSEC);
	  const unsigned char *h = rec + CC_MHR_OFF_HASH;

	  if (cc_paths_affect_output_p ())
	    {
	      /* B2: hash the prefix-mapped path, like cc_hash_one_file.  */
	      char *mp = (cc_key_pm.count
			  ? cc_pmaps_remap_alloc (&cc_key_pm, hpath) : NULL);
	      cc_hash_str (ctx, CC_TAG_FILE_PATH, mp ? mp : hpath);
	      free (mp);
	    }
	  cc_hash_component (ctx, CC_TAG_FILE_BODY, h, 20);
	  cc_meta_add_input (hpath, sz, &id, mflags, h);
	}

      /* (b) The prelude's recorded probes: fold the results into the key
	 and import the records so this TU's own manifest re-verifies them.
	 Everything the gch recorded is verifiable (unverifiable probes set
	 the entry flag rejected above).  */
      uint64_t cur = ent.probe_recs_off;
      for (uint32_t pi = 0; pi < ent.probe_count && ok; pi++)
	{
	  const unsigned char *rec = man + cur;
	  uint32_t pflags = cc_get_u32 (rec + CC_MPR_OFF_FLAGS);
	  uint32_t ncand = cc_get_u32 (rec + CC_MPR_OFF_NCAND);
	  cur += CC_MAN_PROBE_REC_FIXED_SIZE + (uint64_t) ncand * 4;

	  const char *name
	    = cc_man_string (man, mlen, cc_get_u32 (rec + CC_MPR_OFF_NAME));
	  const char *resolved = NULL;
	  if (pflags & CC_MPR_FLAG_FOUND)
	    resolved = cc_man_string (man, mlen,
				      cc_get_u32 (rec + CC_MPR_OFF_RESOLVED));
	  if (!name || (pflags & ~CC_MPR_FLAG_KNOWN_MASK)
	      || ((pflags & CC_MPR_FLAG_FOUND) && !resolved))
	    {
	      ok = false;
	      break;
	    }

	  unsigned flags = CPP_HI_PROBE_VERIFIABLE;
	  if (pflags & CC_MPR_FLAG_FOUND)
	    flags |= CPP_HI_PROBE_FOUND;
	  if (pflags & CC_MPR_FLAG_BRACKET)
	    flags |= CPP_HI_PROBE_BRACKET;

	  const char **cands
	    = ncand ? XNEWVEC (const char *, ncand) : NULL;
	  for (uint32_t ci = 0; ci < ncand && ok; ci++)
	    {
	      cands[ci]
		= cc_man_string (man, mlen,
				 cc_get_u32 (rec + CC_MAN_PROBE_REC_FIXED_SIZE
					     + ci * 4));
	      if (!cands[ci])
		ok = false;
	    }
	  if (ok)
	    {
	      cc_hash_probe_result (ctx, name, flags);
	      cc_meta_add_probe (name, flags, resolved, cands, ncand);
	    }
	  free (cands);
	}
    }
  free (man);
  return ok;
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
     object metadata (cc_meta.source / cc_meta.cwd) for diagnostics -- RAW:
     only the key hashes the B2 prefix-mapped strings (DWARF contains the
     paths only after the user's -f{file,debug}-prefix-map rewrites; two
     build dirs mapped to one canonical prefix therefore share the key AND
     the bytes).  */
  cc_pmaps_free (&cc_key_pm);	/* drop any stale per-TU state */
  if (cc_paths_affect_output_p ())
    {
      cc_pmaps_collect (&cc_key_pm, save_decoded_options,
			save_decoded_options_count);
      cc_pmaps_mark_dropped (&cc_key_pm, save_decoded_options_count,
			     main_input_filename, cc_meta.cwd);
      char *msrc = cc_pmaps_remap_alloc (&cc_key_pm, main_input_filename);
      char *mcwd = cc_pmaps_remap_alloc (&cc_key_pm, cc_meta.cwd);
      cc_hash_str (&ctx, CC_TAG_MAIN_INPUT, msrc);
      cc_hash_str (&ctx, CC_TAG_CWD, mcwd);
      free (msrc);
      free (mcwd);
    }

  /* (4) Canonicalized codegen/ABI-relevant command-line options, in order.
     The same token set is recorded in the metadata "options" string (minus
     -o / dump temp paths, which cc_option_affects_output_p already drops;
     the B2-dropped map options ARE still recorded there -- the metadata
     describes the compile, the hash describes the output).  */
  for (unsigned i = 1; i < save_decoded_options_count; i++)
    {
      const cl_decoded_option *o = &save_decoded_options[i];
      if (!cc_option_affects_output_p (o))
	continue;
      for (size_t k = 0; k < o->canonical_option_num_elements; k++)
	{
	  if (!cc_pmaps_opt_dropped_p (&cc_key_pm, i))
	    cc_hash_str (&ctx, CC_TAG_OPT, o->canonical_option[k]);
	  cc_options_append (&cc_meta.options, o->canonical_option[k]);
	}
    }
  if (!cc_meta.options)
    cc_meta.options = xstrdup ("");

  /* (3) The source closure: paths + exact bytes of every stacked file.  This
     same walk fills the inputs table via cc_meta_add_input.  (Paths hash
     prefix-mapped under -g via cc_key_pm; see cc_hash_one_file.)  */
  struct cc_closure_state st;
  st.ctx = &ctx;
  if (!cpp_foreach_included_file (pfile, cc_hash_one_file, &st))
    {
      cc_pmaps_free (&cc_key_pm);
      return false;		/* a file could not be re-read; don't trust key */
    }

  /* (3b) Every evaluated __has_include probe's RESULT (a probe can flip the
     output without changing the closure hashed above), captured into
     cc_meta.probes for the manifest along the way.  */
  cc_tu_has_unverifiable_probe = false;
  cpp_foreach_has_include_probe (pfile, cc_collect_one_probe, &ctx);

  /* (3c) Auto-PCH: the consumed .gch replaced the prelude headers in the
     walk above (and pre-answered their probes); commit their recorded
     identities from the gch manifest so the key + manifest stay
     closure-complete.  */
  if (cc_pch_consumed_ours && !cc_merge_gch_manifest (&ctx))
    {
      cc_pmaps_free (&cc_key_pm);
      return false;
    }

  cc_pmaps_free (&cc_key_pm);
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

  /* The cwd: CC_TAG_CWD material under -g below, and the base the
     search-path values relativize against either way.  The driver twin's
     ctx->cwd resolves to the same string (same process directory; under -g
     the -fworking-directory the specs inject IS that directory).  */
  const char *pwd = get_src_pwd ();

  /* Under -g the source path + cwd bake into DWARF -- but only after the
     user's -f{file,debug}-prefix-map rewrites, so hash the MAPPED strings
     and drop the map options the mapping consumed (B2; identity + no drops
     when no map matches).  MUST stay byte-for-byte parallel with the
     driver's ccs_compute_manifest_key.  */
  struct cc_prefix_maps pm;
  memset (&pm, 0, sizeof (pm));
  if (cc_paths_affect_output_p ())
    {
      cc_pmaps_collect (&pm, save_decoded_options, save_decoded_options_count);
      cc_pmaps_mark_dropped (&pm, save_decoded_options_count, src_path,
			     pwd ? pwd : "");
      char *msrc = cc_pmaps_remap_alloc (&pm, src_path);
      char *mcwd = cc_pmaps_remap_alloc (&pm, pwd ? pwd : "");
      cc_hash_str (&ctx, CC_TAG_MAIN_INPUT, msrc);
      cc_hash_str (&ctx, CC_TAG_CWD, mcwd);
      free (msrc);
      free (mcwd);
    }

  /* (4) Output-affecting options + (anti-shadow) search-path VALUES.  Walked
     in command-line order so option ordering is part of the key.  Map
     options whose effect is already captured by the mapped src/cwd above
     are excluded (cc_pmaps_opt_dropped_p; never set without -g).
     Search-path values inside the build dir hash cwd-relative so relocated
     build trees share manifests (cc_mk_search_path_relative; ccache base_dir
     parity -- MK is a lookup index, every serve is still record-verified).  */
  for (unsigned i = 1; i < save_decoded_options_count; i++)
    {
      const cl_decoded_option *o = &save_decoded_options[i];
      if (cc_pmaps_opt_dropped_p (&pm, i))
	continue;
      bool affects = cc_option_affects_output_p (o);
      bool search = cc_option_is_search_path_p (o);
      if (!affects && !search)
	continue;
      for (size_t k = 0; k < o->canonical_option_num_elements; k++)
	{
	  const char *val = o->canonical_option[k];
	  if (search)
	    val = cc_mk_search_path_relative (val, pwd ? pwd : "");
	  cc_hash_str (&ctx, search ? CC_TAG_SEARCH_PATH : CC_TAG_OPT, val);
	}
    }
  cc_pmaps_free (&pm);

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

  /* Anchor the stat-identity "too new" guard (cc_capture_statid) at the
     compile's start, before the preprocessor reads any file.  Deliberately
     ahead of the enabled_p gate: the auto-PCH gch store also captures
     identities, and it runs in PCH-build compiles where the .o cache is
     disabled.  */
  if (cc_compile_start_time == 0)
    cc_compile_start_time = time (NULL);

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
   debug line ("hit" / "manifest-hit").  On a hit the object's stored
   diagnostic counts are copied to *STORED_WARNINGS / *STORED_WERRORS when
   non-NULL (the post-parse path forwards them to the on-hit manifest store,
   mirroring the counts the miss-path store records).  */
static bool
cc_serve_from_bin (const char *ok_hex, bool require_no_fe_diag,
		   const char *debug_action,
		   uint32_t *stored_warnings, uint32_t *stored_werrors)
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
  if (placed)
    /* Mark the entry recently-used so LRU eviction spares it (B1).  */
    cc_touch_entry (obj_path);
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
  if (stored_warnings)
    *stored_warnings = warnings;
  if (stored_werrors)
    *stored_werrors = errors;

  free (meta);
  cc_hit = true;
  cc_debug_line (debug_action, ok_hex);
  return true;
}

/* Defined below with the rest of the store path; the post-parse serve calls
   it to record the manifest on a deep hit.  */
static void cc_store_manifest (uint32_t warnings, uint32_t werrors,
			       const char *debug_action);

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

  /* A consumed foreign PCH hides its closure from the walk below: neither
     the key nor the manifest would cover the prelude headers, so a later
     header edit could serve a stale object.  Bail (leaving cc_key_valid
     false, which also disables the store).  The auto-PCH stub is exempt:
     cc_compute_key merges its recorded closure back in.  */
  if (cc_pch_consumed && !cc_pch_consumed_ours)
    {
      cc_debug_line ("skip-foreign-pch", NULL);
      return false;
    }

  /* Record whether the parse used __has_include_next: its result depends on
     include-stack position, which no record can let the pre-parse serve
     re-verify, so the store must skip the manifest for such a TU.  (Plain
     __has_include is handled by the per-probe records collected in
     cc_compute_key.)  */
  cc_tu_used_has_include_next = cpp_used_has_include_next (pfile);

  if (!cc_compute_key (pfile))
    return false;		/* key untrustworthy -> behave as a miss */

  /* Try to serve by the object key OK.  Front-end diagnostics are allowed here
     (they re-emit on the re-parse that already happened), so pass
     require_no_fe_diag = false.  */
  uint32_t stored_warnings = 0, stored_werrors = 0;
  if (cc_serve_from_bin (cc_key_hex, /*require_no_fe_diag=*/false, "hit",
			 &stored_warnings, &stored_werrors))
    {
      /* A deep (post-parse) hit means the pre-parse manifest lookup could
	 NOT serve this TU: no manifest under its MK, or no entry whose
	 records still verify.  Store/refresh the manifest NOW from the
	 metadata cc_compute_key just gathered (include closure + probes) --
	 the exact material the miss-path store records -- so the NEXT
	 compile serves pre-parse.  Without this, a TU whose first-ever
	 compile object-hit a twin's store (parallel-populate race) never
	 acquired a manifest at all and re-paid the full parse on EVERY warm
	 build (measured: 4 llama.cpp TUs at ~2.5 s each, "manifest-miss,
	 hit" forever).  ccache's equivalent (direct-mode manifest update
	 after a preprocessed-mode hit) behaves the same way.  The counts
	 are the object's stored ones, i.e. what the miss path passed;
	 the skip-guards are the miss path's too: seen_error () mirrored
	 here, MK validity + unverifiable-probe forms inside
	 cc_store_manifest itself.  */
      if (!seen_error ())
	cc_store_manifest (stored_warnings, stored_werrors,
			   "manifest-store-on-hit");
      return true;
    }

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

  /* Dependency output.  A pre-parse hit skips preprocessing, so libcpp's
     deps hold only the main file and the -MT/-MQ targets; on a hit below the
     recorded closure is fed into the deps object (deps_add_dep) so
     c_common_finish still writes a complete .d -- the -MD contract.  For the
     user-only styles (-MM/-MMD: openssl-shaped make builds) the records'
     CC_MHR_FLAG_SYSHDR bit reproduces libcpp's exclusion, so they are served
     too, skipping flagged records below.  Forms the records cannot reproduce
     decline the manifest serve instead (the deep path recomputes exact
     deps): -MG (explicit opt-in) adds missing-file entries; -fdeps-*
     (explicit opt-in) emits structured P1689 output.
     deps.modules is deliberately NOT a decline condition: c-family
     initialization defaults it to TRUE on every compile (c-opts.cc
     c_common_init_options) -- it only means "IF module dependencies exist,
     also list them" -- so treating it as a form signal would kill the
     manifest for every ordinary -MD build, i.e. the entire ninja/cmake hot
     path.  TUs that actually import modules are outside the cache's
     supported territory regardless of dependency output (their .gcm inputs
     are invisible to the include-closure walk).  */
  class mkdeps *mdeps = NULL;
  bool deps_user_only = false;
  {
    const cpp_options *copts = cpp_get_options (pfile);
    if (copts->deps.style != DEPS_NONE
	|| copts->deps.fdeps_format != FDEPS_FMT_NONE)
      {
	if (copts->deps.fdeps_format != FDEPS_FMT_NONE
	    || copts->deps.missing_files)
	  {
	    cc_debug_line ("manifest-skip-deps-form", NULL);
	    return false;
	  }
	deps_user_only = (copts->deps.style == DEPS_USER);
	mdeps = cpp_get_deps (pfile);
      }
  }

  /* Note on __has_include: probes never enter the include closure, so the
     header records alone cannot notice one flipping absent<->present between
     runs.  Each manifest entry therefore carries the probe records its TU
     evaluated (operand + result + the candidate paths the search proved
     absent), and cc_man_entry_records_valid re-verifies them below alongside
     the headers.  TUs whose probes cannot be re-verified from records
     (__has_include_next, relative quote form, -remap, header maps) never get
     a manifest -- compile_cache_store () skips them -- so this lookup simply
     misses for those and a full compile re-resolves everything.  Safe by
     construction.  (void pfile.)  */
  (void) pfile;

  /* Optional airtight mode: GCC_COMPILE_CACHE_VERIFY=hash (alias:
     GCC_COMPILE_CACHE_PARANOID=1) forces a full content re-hash of every
     header on a hit instead of the stat-identity shortcut.  Even this still
     skips parse/codegen/assemble.  */
  bool verify_hash = cc_verify_hash_env_p ();

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

  /* Walk each entry (candidate header set + its recorded probes),
     re-validating with the SAME shared code the driver-level serve uses
     (headers: stat shortcut else re-hash; probes: candidates still absent,
     found paths still present).  */
  uint64_t cur = entries_off;
  for (uint32_t ei = 0; ei < entry_count; ei++)
    {
      struct cc_man_entry ent;
      if (!cc_man_entry_parse (man, mlen, cur, &ent))
	break;			/* truncated manifest -> stop */

      if (ent.eflags == 0
	  && cc_man_entry_records_valid (man, mlen, &ent, verify_hash))
	{
	  /* Every record matched.  Resolve the object under this set's OK and
	     serve it -- but only if it carries no front-end diagnostics (we are
	     about to skip the parse).  */
	  char ok_hex[41];
	  cc_hex (ent.ok_raw, ok_hex);
	  if (cc_serve_from_bin (ok_hex, /*require_no_fe_diag=*/true,
				 "manifest-hit", NULL, NULL))
	    {
	      /* Complete the dependency info from the entry's records (the
		 include closure).  The main file is already in the deps --
		 libcpp added it when the main buffer was stacked -- so skip
		 its record (by flag, and by path for safety) to avoid a
		 duplicate.  Under -MM/-MMD also skip the records libcpp's
		 exclusion would have skipped (CC_MHR_FLAG_SYSHDR).  */
	      if (mdeps)
		for (uint32_t hi = 0; hi < ent.hdr_count; hi++)
		  {
		    const unsigned char *rec
		      = man + ent.hdr_recs_off
			+ (uint64_t) hi * CC_MAN_HDR_REC_SIZE;
		    uint32_t rflags = cc_get_u32 (rec + CC_MHR_OFF_FLAGS);
		    if (rflags & CC_MHR_FLAG_MAIN_SOURCE)
		      continue;
		    if (deps_user_only && (rflags & CC_MHR_FLAG_SYSHDR))
		      continue;
		    const char *hpath
		      = cc_man_string (man, mlen,
				       cc_get_u32 (rec + CC_MHR_OFF_PATH));
		    if (hpath && strcmp (hpath, src_path) != 0)
		      deps_add_dep (mdeps, hpath);
		  }
	      /* Bump the manifest too: it answered this hit (B1).  */
	      {
		char *mp = cc_entry_path (cc_manifest_key_hex,
					  /*make_dirs=*/false);
		cc_touch_entry (mp);
		free (mp);
	      }
	      free (man);
	      return true;
	    }
	  /* Object missing or refused (front-end diag): try the next set, then
	     fall through to a real compile.  */
	}

      cur = ent.next_off;
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
/* B1: cache size cap + LRU eviction (store side)                           */
/* ------------------------------------------------------------------------ */

/* GCC_COMPILE_CACHE_MAX_SIZE=N[K|M|G|T][B] caps the total cache size; unset,
   0, or unparsable means unlimited -- no stats are maintained and no
   eviction runs, so the un-capped hot path pays nothing.  Suffixes are
   powers of 1024, case-insensitive, optional trailing B ("512M", "2g",
   "1024KB", plain bytes).

   Accounting is ccache-style per shard: each 2-hex subdir owns cap/256.
   Every store updates a tiny per-shard stats file ("DIR/xx/shard-stats",
   torn-tolerant text, atomic tmp+rename); when the shard's recorded bytes
   exceed its budget, the shard is swept: entries are ranked by mtime
   (last-use -- serve hits bump it via cc_touch_entry) and the oldest are
   unlinked until the shard is under ~90% of its budget.  Concurrent stores
   can lose a stats update (read-modify-write race); that only delays a
   sweep by one store, and every sweep rewrites exact numbers from its own
   directory scan, so the stats self-heal.  Hardlink-served user objects
   share the inode with the cache entry, so evicting (unlinking) the cache
   path never harms an already-served .o.  Evicting an object a manifest
   still points to degrades to a clean miss at serve time (the serve treats
   a missing object as a miss, never an error); an evicted manifest is
   rebuilt by the next store.  The auto-PCH subtree (DIR/pch/...) is not
   governed by this cap (its entries are directories with their own
   lifecycle); only the 2-hex object/manifest shards are.  */

#define CC_SHARD_STATS_NAME "shard-stats"

/* Parse the cap once.  0 = unlimited.  */
static uint64_t
cc_max_size_bytes (void)
{
  static int parsed = 0;
  static uint64_t cap = 0;
  if (parsed)
    return cap;
  parsed = 1;
  const char *e = getenv ("GCC_COMPILE_CACHE_MAX_SIZE");
  if (!e || !e[0])
    return cap;
  char *end = NULL;
  errno = 0;
  unsigned long long v = strtoull (e, &end, 10);
  if (errno != 0 || end == e)
    return cap;
  uint64_t mult = 1;
  if (*end == 'k' || *end == 'K')
    mult = 1024ULL, end++;
  else if (*end == 'm' || *end == 'M')
    mult = 1024ULL * 1024, end++;
  else if (*end == 'g' || *end == 'G')
    mult = 1024ULL * 1024 * 1024, end++;
  else if (*end == 't' || *end == 'T')
    mult = 1024ULL * 1024 * 1024 * 1024, end++;
  if (*end == 'b' || *end == 'B')
    end++;
  if (*end != '\0')
    return cap;			/* trailing junk: treat as unset */
  cap = (uint64_t) v * mult;
  return cap;
}

/* Read "DIR/xx/shard-stats" ("ccstats1 <bytes> <files>\n").  Any parse
   failure returns false (caller recounts).  */
static bool
cc_shard_stats_read (const char *sdir, uint64_t *bytes, uint64_t *files)
{
  char *p = concat (sdir, "/", CC_SHARD_STATS_NAME, NULL);
  FILE *f = fopen (p, "r");
  free (p);
  if (!f)
    return false;
  char tag[16];
  unsigned long long b = 0, n = 0;
  bool ok = (fscanf (f, "%15s %llu %llu", tag, &b, &n) == 3
	     && strcmp (tag, "ccstats1") == 0);
  fclose (f);
  if (!ok)
    return false;
  *bytes = b;
  *files = n;
  return true;
}

/* Atomically (tmp+rename) publish the shard stats.  Best-effort.  */
static void
cc_shard_stats_write (const char *sdir, uint64_t bytes, uint64_t files)
{
  char *fin = concat (sdir, "/", CC_SHARD_STATS_NAME, NULL);
  char *tmp = concat (fin, ".tmpXXXXXX", NULL);
  int fd = mkstemp (tmp);
  if (fd >= 0)
    {
      FILE *f = fdopen (fd, "w");
      if (f)
	{
	  bool ok = (fprintf (f, "ccstats1 %llu %llu\n",
			      (unsigned long long) bytes,
			      (unsigned long long) files) > 0);
	  if (fclose (f) != 0)
	    ok = false;
	  if (!ok || rename (tmp, fin) != 0)
	    unlink (tmp);
	}
      else
	{
	  close (fd);
	  unlink (tmp);
	}
    }
  free (tmp);
  free (fin);
}

/* One scanned shard entry, for the sweep's LRU ranking.  */
struct cc_shard_ent
{
  char *name;			/* entry basename (xmalloc'd) */
  uint64_t size;
  uint64_t mtime_s;
  uint32_t mtime_ns;
};

/* Oldest (least recently used) first; basename tie-break for determinism
   within one mtime tick.  */
static int
cc_shard_ent_cmp (const void *pa, const void *pb)
{
  const struct cc_shard_ent *a = (const struct cc_shard_ent *) pa;
  const struct cc_shard_ent *b = (const struct cc_shard_ent *) pb;
  if (a->mtime_s != b->mtime_s)
    return a->mtime_s < b->mtime_s ? -1 : 1;
  if (a->mtime_ns != b->mtime_ns)
    return a->mtime_ns < b->mtime_ns ? -1 : 1;
  return strcmp (a->name, b->name);
}

/* Scan SDIR into a freshly xmalloc'd entry array (caller frees each name +
   the array); returns the count and total byte size.  Skips ".", "..", the
   stats file, and in-flight "*.tmp*" temporaries (mkstemp names in this
   cache always contain ".tmp").  */
static struct cc_shard_ent *
cc_shard_scan (const char *sdir, unsigned *count_out, uint64_t *bytes_out)
{
  *count_out = 0;
  *bytes_out = 0;
  DIR *d = opendir (sdir);
  if (!d)
    return NULL;
  unsigned cap = 0, n = 0;
  struct cc_shard_ent *ents = NULL;
  struct dirent *de;
  while ((de = readdir (d)) != NULL)
    {
      const char *nm = de->d_name;
      if (nm[0] == '.' && (nm[1] == '\0' || (nm[1] == '.' && nm[2] == '\0')))
	continue;
      if (strcmp (nm, CC_SHARD_STATS_NAME) == 0)
	continue;
      if (strstr (nm, ".tmp") != NULL)
	continue;		/* in-flight temp of a concurrent store */
      char *full = concat (sdir, "/", nm, NULL);
      struct stat st;
      if (stat (full, &st) != 0 || !S_ISREG (st.st_mode))
	{
	  free (full);
	  continue;
	}
      free (full);
      if (n == cap)
	{
	  cap = cap ? cap * 2 : 32;
	  ents = XRESIZEVEC (struct cc_shard_ent, ents, cap);
	}
      struct cc_statid id;
      cc_statid_from_stat (&st, &id);
      ents[n].name = xstrdup (nm);
      ents[n].size = (uint64_t) st.st_size;
      ents[n].mtime_s = id.mtime_s;
      ents[n].mtime_ns = id.mtime_ns;
      n++;
      *bytes_out += (uint64_t) st.st_size;
    }
  closedir (d);
  *count_out = n;
  return ents;
}

/* Sweep SDIR down to ~90% of BUDGET by unlinking least-recently-used
   entries, then publish exact stats from this scan.  */
static void
cc_shard_sweep (const char *sdir, uint64_t budget)
{
  unsigned n = 0;
  uint64_t total = 0;
  struct cc_shard_ent *ents = cc_shard_scan (sdir, &n, &total);
  uint64_t files = n;

  uint64_t target = budget - budget / 10;	/* ~90% */
  if (total > budget && ents)
    {
      qsort (ents, n, sizeof (*ents), cc_shard_ent_cmp);
      for (unsigned i = 0; i < n && total > target; i++)
	{
	  char *full = concat (sdir, "/", ents[i].name, NULL);
	  if (unlink (full) == 0)
	    {
	      total -= ents[i].size;
	      files--;
	      if (cc_debug_p ())
		{
		  fprintf (stderr, "compile-cache: evict - %s\n", full);
		  fflush (stderr);
		}
	    }
	  free (full);
	}
    }

  cc_shard_stats_write (sdir, total, files);
  for (unsigned i = 0; i < n; i++)
    free (ents[i].name);
  free (ents);
}

/* Account a completed store into KEY_HEX's shard: DELTA_BYTES bytes
   (negative when a rewrite shrank the file) and NEW_FILES newly created
   files.  Sweeps the shard when it exceeds its cap/256 budget.  No-op
   without a configured cap.  */
static void
cc_evict_note_store (const char *key_hex, int64_t delta_bytes, int new_files)
{
  uint64_t cap = cc_max_size_bytes ();
  if (cap == 0)
    return;
  uint64_t budget = cap / 256;
  char shard[3] = { key_hex[0], key_hex[1], '\0' };
  char *sdir = concat (cc_dir, "/", shard, NULL);

  uint64_t bytes = 0, files = 0;
  if (cc_shard_stats_read (sdir, &bytes, &files))
    {
      /* Apply the delta (clamped: drifted stats must not wrap).  */
      if (delta_bytes >= 0)
	bytes += (uint64_t) delta_bytes;
      else if (bytes > (uint64_t) -delta_bytes)
	bytes -= (uint64_t) -delta_bytes;
      else
	bytes = 0;
      files += new_files;
    }
  else
    {
      /* Missing/torn stats (fresh shard, or a cap newly applied to an
	 existing cache): recount from the directory.  The scan already
	 includes the just-stored files, so the delta is NOT re-applied.  */
      unsigned cnt = 0;
      struct cc_shard_ent *ents = cc_shard_scan (sdir, &cnt, &bytes);
      files = cnt;
      for (unsigned i = 0; i < cnt; i++)
	free (ents[i].name);
      free (ents);
    }

  if (bytes > budget)
    cc_shard_sweep (sdir, budget);	/* rewrites exact stats itself */
  else
    cc_shard_stats_write (sdir, bytes, files);
  free (sdir);
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

/* Emit one v3 header record for IN into ENTRIES.  PATH_OFF_ABS is the
   already-final absolute file offset of IN's path string.  Shared by the .o
   manifest store and the auto-PCH gch manifest store so both write identical
   record bytes.  */
static void
cc_emit_hdr_rec (cc_blob *entries, uint32_t path_off_abs, const cc_input *in)
{
  unsigned char nrec[CC_MAN_HDR_REC_SIZE];
  memset (nrec, 0, sizeof (nrec));
  cc_put_u32 (nrec + CC_MHR_OFF_PATH, path_off_abs);
  cc_put_u32 (nrec + CC_MHR_OFF_FLAGS, in->mhr_flags);
  cc_put_u64 (nrec + CC_MHR_OFF_SIZE, in->size);
  cc_put_u64 (nrec + CC_MHR_OFF_MTIME, in->id.mtime_s);
  cc_put_u64 (nrec + CC_MHR_OFF_CTIME, in->id.ctime_s);
  cc_put_u64 (nrec + CC_MHR_OFF_DEV, in->id.dev);
  cc_put_u64 (nrec + CC_MHR_OFF_INO, in->id.ino);
  cc_put_u32 (nrec + CC_MHR_OFF_MTIME_NSEC, in->id.mtime_ns);
  cc_put_u32 (nrec + CC_MHR_OFF_CTIME_NSEC, in->id.ctime_ns);
  memcpy (nrec + CC_MHR_OFF_HASH, in->hash, 20);
  cc_blob_append (entries, nrec, CC_MAN_HDR_REC_SIZE);
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

/* Publish one compiler-id sidecar at PATH (atomic tmp+rename; no-op on
   failure).  */
static void
cc_write_compiler_id_file (const char *path, const unsigned char *buf,
			   size_t total)
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
  free (tmp);
}

/* Write the cache's compiler-id sidecars so the DRIVER can form the manifest
   key without linking this compiler's checksum object or knowing
   lang_hooks.name: they record executable_checksum + lang_hooks.name, the
   two key components the driver cannot derive on its own.  Idempotent and
   cheap; written on every miss-store (a recompiled compiler -> new checksum ->
   the driver's MK changes in lockstep, so a stale id can never cause a wrong
   hit -- it would simply differ from the object's stored checksum-keyed MK).

   Two names are published (see compile-cache-format.h): the per-language
   "DIR/compiler-id-<prog>" keyed by THIS compiler's program name (progname:
   the lbasename of argv[0], "cc1"/"cc1plus"/... -- the very token the driver
   matches when it intercepts the command), and the legacy single
   "DIR/compiler-id" for older drivers sharing the cache dir.  Without the
   split, C and C++ TUs in one cache fought over the single file and the
   losing language's TUs never served at the driver tier.  */
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

  if (progname && progname[0])
    {
      char *lpath = concat (cc_dir, "/", CC_COMPILER_ID_PREFIX, progname,
			    NULL);
      cc_write_compiler_id_file (lpath, buf, total);
      free (lpath);
    }

  char *path = concat (cc_dir, "/", CC_COMPILER_ID_NAME, NULL);
  cc_write_compiler_id_file (path, buf, total);
  free (path);
  free (buf);
}

/* Append/refresh this TU's entry in the manifest object keyed by MK.  The
   manifest lists, per include set, the object key OK, the headers that set
   depended on (path + size + mtime + hash, from cc_meta.inputs), and the
   __has_include probes the TU evaluated (operand + result + the candidate
   paths the search proved absent, from cc_meta.probes).  Existing entries
   with a DIFFERENT OK are preserved (the same source+flags can reach
   different header sets via conditional includes / -I ordering); an entry
   with the SAME OK is replaced (refreshes mtimes after a touch).
   WARNINGS/WERRORS are the object's stored counts (carried so a future
   manifest reader could short-circuit; the authoritative copy is in the
   object header).  Atomic publish.  No-op unless the manifest key is valid,
   and skipped entirely for TUs whose probes a pre-parse serve could not
   re-verify (__has_include_next; unverifiable probe forms).  DEBUG_ACTION
   labels the success debug line: "manifest-store" from the miss path,
   "manifest-store-on-hit" from the post-parse-hit path.  */
static void
cc_store_manifest (uint32_t warnings, uint32_t werrors,
		   const char *debug_action)
{
  if (!cc_manifest_key_valid)
    return;

  /* __has_include_next resolves relative to the probing file's position on
     the include stack, which no record lets a pre-parse serve re-run.  Do
     NOT record a manifest (the post-parse object cache still applies).  */
  if (cc_tu_used_has_include_next)
    {
      cc_debug_line ("manifest-skip-has-include-next", cc_manifest_key_hex);
      return;
    }

  /* Likewise for any probe libcpp could not make re-verifiable from records
     (relative quote form, -remap, header-map dirs) -- the status quo for
     such TUs.  Plain angle probes (the ones every C++ TU makes via
     bits/c++config.h) are recorded in the entry and re-verified at serve
     time instead of disqualifying the TU.  */
  if (cc_tu_has_unverifiable_probe)
    {
      cc_debug_line ("manifest-skip-has-include", cc_manifest_key_hex);
      return;
    }

  char *man_path = cc_entry_path (cc_manifest_key_hex, /*make_dirs=*/true);

  /* Read any existing manifest so we can preserve its other entries.  */
  size_t old_len = 0;
  unsigned char *old = cc_read_file (man_path, &old_len);
  /* For B1 accounting below: whether a file is being replaced (rename over
     it) and how big it was -- the stats track on-disk deltas.  */
  bool had_old = (old != NULL);
  bool old_valid = (old
		    && old_len >= CC_MANIFEST_HEADER_SIZE
		    && memcmp (old + CC_MAN_OFF_MAGIC, CC_MANIFEST_MAGIC,
			       CC_MAGIC_LEN) == 0
		    && cc_get_u16 (old + CC_MAN_OFF_VERSION)
			 == CC_MANIFEST_VERSION);

  /* Pass 1: pick the old entries to preserve (parseable, all strings
     resolvable, OK differing from ours -- the fresh entry supersedes a same-
     OK one) and size the whole entries section, so pass 2 can write every
     string offset as its final ABSOLUTE file offset up front (the string
     area starts right after the entries).  */
  uint64_t keep_off[64];
  unsigned keep_count = 0;
  uint64_t entries_total = 0;
  if (old_valid)
    {
      uint32_t ocount = cc_get_u32 (old + CC_MAN_OFF_ENTRY_COUNT);
      uint64_t ocur = cc_get_u64 (old + CC_MAN_OFF_ENTRIES_OFF);
      for (uint32_t ei = 0; ei < ocount; ei++)
	{
	  struct cc_man_entry ent;
	  if (!cc_man_entry_parse (old, old_len, ocur, &ent))
	    break;		/* truncated -> drop the rest */
	  uint64_t esize = ent.next_off - ocur;
	  uint64_t eoff = ocur;
	  ocur = ent.next_off;

	  if (memcmp (ent.ok_raw, cc_key_raw, 20) == 0)
	    continue;		/* superseded by the fresh entry below */
	  if (keep_count == sizeof (keep_off) / sizeof (keep_off[0]))
	    continue;		/* pathological entry pile-up: drop extras */

	  /* Every string must resolve, or the entry is dropped whole (a
	     truncated string area cannot be re-emitted faithfully).  */
	  bool strings_ok = true;
	  for (uint32_t hi = 0; hi < ent.hdr_count && strings_ok; hi++)
	    {
	      const unsigned char *rec = old + ent.hdr_recs_off
					 + (uint64_t) hi * CC_MAN_HDR_REC_SIZE;
	      strings_ok = (cc_man_string (old, old_len,
					   cc_get_u32 (rec + CC_MHR_OFF_PATH))
			    != NULL);
	    }
	  uint64_t pcur = ent.probe_recs_off;
	  for (uint32_t pi = 0; pi < ent.probe_count && strings_ok; pi++)
	    {
	      const unsigned char *rec = old + pcur;
	      uint32_t pflags = cc_get_u32 (rec + CC_MPR_OFF_FLAGS);
	      uint32_t ncand = cc_get_u32 (rec + CC_MPR_OFF_NCAND);
	      pcur += CC_MAN_PROBE_REC_FIXED_SIZE + (uint64_t) ncand * 4;
	      strings_ok = (cc_man_string (old, old_len,
					   cc_get_u32 (rec + CC_MPR_OFF_NAME))
			    != NULL);
	      if (strings_ok && (pflags & CC_MPR_FLAG_FOUND))
		strings_ok
		  = (cc_man_string (old, old_len,
				    cc_get_u32 (rec + CC_MPR_OFF_RESOLVED))
		     != NULL);
	      for (uint32_t ci = 0; ci < ncand && strings_ok; ci++)
		strings_ok
		  = (cc_man_string (old, old_len,
				    cc_get_u32 (rec
						+ CC_MAN_PROBE_REC_FIXED_SIZE
						+ ci * 4))
		     != NULL);
	    }
	  if (!strings_ok)
	    continue;

	  keep_off[keep_count++] = eoff;
	  entries_total += esize;	/* re-emitted at identical size */
	}
    }
  entries_total += CC_MAN_ENT_HEAD_SIZE
		   + (uint64_t) cc_meta.input_count * CC_MAN_HDR_REC_SIZE;
  for (unsigned i = 0; i < cc_meta.probe_count; i++)
    entries_total += CC_MAN_PROBE_REC_FIXED_SIZE
		     + (uint64_t) cc_meta.probes[i].n_candidates * 4;

  uint64_t entries_off = CC_MANIFEST_HEADER_SIZE;
  uint64_t string_area_off = entries_off + entries_total;
  uint32_t entry_count = 0;

  /* Pass 2: emit.  Strings land in the strings blob; the stored offsets are
     string_area_off + the blob-relative offset, i.e. final.  */
  cc_blob strings = { NULL, 0, 0 };
  cc_blob entries = { NULL, 0, 0 };

  /* (a) Preserved entries, re-emitted with their strings re-added.  */
  for (unsigned ki = 0; ki < keep_count; ki++)
    {
      struct cc_man_entry ent;
      /* Parsed successfully in pass 1; parse again for the offsets.  */
      cc_man_entry_parse (old, old_len, keep_off[ki], &ent);

      unsigned char head[CC_MAN_ENT_HEAD_SIZE];
      memcpy (head + CC_MENT_OFF_OK, ent.ok_raw, 20);
      cc_put_u32 (head + CC_MENT_OFF_WARNINGS, ent.warnings);
      cc_put_u32 (head + CC_MENT_OFF_WERRORS, ent.werrors);
      cc_put_u32 (head + CC_MENT_OFF_FLAGS, ent.eflags);
      cc_put_u32 (head + CC_MENT_OFF_HDR_COUNT, ent.hdr_count);
      cc_put_u32 (head + CC_MENT_OFF_PROBE_COUNT, ent.probe_count);
      cc_blob_append (&entries, head, CC_MAN_ENT_HEAD_SIZE);

      for (uint32_t hi = 0; hi < ent.hdr_count; hi++)
	{
	  const unsigned char *rec = old + ent.hdr_recs_off
				     + (uint64_t) hi * CC_MAN_HDR_REC_SIZE;
	  const char *hpath
	    = cc_man_string (old, old_len,
			     cc_get_u32 (rec + CC_MHR_OFF_PATH));
	  /* Copy the record verbatim (flags + size + stat identity + hash);
	     only the path offset is buffer-relative and must be re-aimed at
	     the new string area.  */
	  unsigned char nrec[CC_MAN_HDR_REC_SIZE];
	  memcpy (nrec, rec, CC_MAN_HDR_REC_SIZE);
	  cc_put_u32 (nrec + CC_MHR_OFF_PATH,
		      (uint32_t) string_area_off
		      + cc_blob_add_string (&strings, hpath));
	  cc_blob_append (&entries, nrec, CC_MAN_HDR_REC_SIZE);
	}

      uint64_t pcur = ent.probe_recs_off;
      for (uint32_t pi = 0; pi < ent.probe_count; pi++)
	{
	  const unsigned char *rec = old + pcur;
	  uint32_t pflags = cc_get_u32 (rec + CC_MPR_OFF_FLAGS);
	  uint32_t ncand = cc_get_u32 (rec + CC_MPR_OFF_NCAND);
	  pcur += CC_MAN_PROBE_REC_FIXED_SIZE + (uint64_t) ncand * 4;

	  unsigned char fixed[CC_MAN_PROBE_REC_FIXED_SIZE];
	  cc_put_u32 (fixed + CC_MPR_OFF_FLAGS, pflags);
	  cc_put_u32 (fixed + CC_MPR_OFF_NAME,
		      (uint32_t) string_area_off
		      + cc_blob_add_string
			  (&strings,
			   cc_man_string (old, old_len,
					  cc_get_u32 (rec
						      + CC_MPR_OFF_NAME))));
	  uint32_t nresolved = 0;
	  if (pflags & CC_MPR_FLAG_FOUND)
	    nresolved
	      = (uint32_t) string_area_off
		+ cc_blob_add_string
		    (&strings,
		     cc_man_string (old, old_len,
				    cc_get_u32 (rec + CC_MPR_OFF_RESOLVED)));
	  cc_put_u32 (fixed + CC_MPR_OFF_RESOLVED, nresolved);
	  cc_put_u32 (fixed + CC_MPR_OFF_NCAND, ncand);
	  cc_blob_append (&entries, fixed, CC_MAN_PROBE_REC_FIXED_SIZE);
	  for (uint32_t ci = 0; ci < ncand; ci++)
	    {
	      const char *cand
		= cc_man_string (old, old_len,
				 cc_get_u32 (rec + CC_MAN_PROBE_REC_FIXED_SIZE
					     + ci * 4));
	      unsigned char co[4];
	      cc_put_u32 (co, (uint32_t) string_area_off
			      + cc_blob_add_string (&strings, cand));
	      cc_blob_append (&entries, co, 4);
	    }
	}
      entry_count++;
    }
  free (old);

  /* (b) This TU's entry: OK + counts + the recorded include set + probes.  */
  {
    unsigned char head[CC_MAN_ENT_HEAD_SIZE];
    memcpy (head + CC_MENT_OFF_OK, cc_key_raw, 20);
    cc_put_u32 (head + CC_MENT_OFF_WARNINGS, warnings);
    cc_put_u32 (head + CC_MENT_OFF_WERRORS, werrors);
    cc_put_u32 (head + CC_MENT_OFF_FLAGS, 0);
    cc_put_u32 (head + CC_MENT_OFF_HDR_COUNT, cc_meta.input_count);
    cc_put_u32 (head + CC_MENT_OFF_PROBE_COUNT, cc_meta.probe_count);
    cc_blob_append (&entries, head, CC_MAN_ENT_HEAD_SIZE);

    for (unsigned i = 0; i < cc_meta.input_count; i++)
      cc_emit_hdr_rec (&entries,
		       (uint32_t) string_area_off
		       + cc_blob_add_string (&strings,
					     cc_meta.inputs[i].path),
		       &cc_meta.inputs[i]);

    for (unsigned i = 0; i < cc_meta.probe_count; i++)
      {
	const cc_probe *p = &cc_meta.probes[i];
	uint32_t pflags = 0;
	if (p->flags & CPP_HI_PROBE_FOUND)
	  pflags |= CC_MPR_FLAG_FOUND;
	if (p->flags & CPP_HI_PROBE_BRACKET)
	  pflags |= CC_MPR_FLAG_BRACKET;

	unsigned char fixed[CC_MAN_PROBE_REC_FIXED_SIZE];
	cc_put_u32 (fixed + CC_MPR_OFF_FLAGS, pflags);
	cc_put_u32 (fixed + CC_MPR_OFF_NAME,
		    (uint32_t) string_area_off
		    + cc_blob_add_string (&strings, p->name));
	cc_put_u32 (fixed + CC_MPR_OFF_RESOLVED,
		    p->resolved
		    ? (uint32_t) string_area_off
		      + cc_blob_add_string (&strings, p->resolved)
		    : 0);
	cc_put_u32 (fixed + CC_MPR_OFF_NCAND, p->n_candidates);
	cc_blob_append (&entries, fixed, CC_MAN_PROBE_REC_FIXED_SIZE);
	for (unsigned k = 0; k < p->n_candidates; k++)
	  {
	    unsigned char co[4];
	    cc_put_u32 (co, (uint32_t) string_area_off
			    + cc_blob_add_string (&strings,
						  p->candidates[k]));
	    cc_blob_append (&entries, co, 4);
	  }
      }
    entry_count++;
  }

  /* Layout: header -> entries -> string area.  Pass 1 sized the entries
     section, so the string offsets emitted above are already absolute.  */
  gcc_checking_assert (entries.len == entries_total);

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
	{
	  cc_debug_line (debug_action, cc_manifest_key_hex);
	  /* B1: account the manifest write (delta vs the replaced file) in
	     its shard and evict if over budget.  */
	  cc_evict_note_store (cc_manifest_key_hex,
			       (int64_t) total
			       - (int64_t) (had_old ? old_len : 0),
			       had_old ? 0 : 1);
	}
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
  bool wrote_bin_meta = false;
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
	      wrote_bin_meta = ok;
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
      cc_store_manifest (warnings, errors, "manifest-store");
      /* Publish the compiler-id sidecar so the DRIVER can form the same
	 manifest key (it needs this compiler's checksum + lang name).  */
      cc_write_compiler_id ();
      /* B1: account the freshly stored object (and the .bin meta sidecar on
	 the xattr-less fallback) in its shard, evicting LRU entries if the
	 shard is now over its share of GCC_COMPILE_CACHE_MAX_SIZE.  A store
	 only happens on an OK miss, so these files are new.  */
      {
	int64_t added = 0;
	int nfiles = 0;
	struct stat ost;
	if (stat (obj_path, &ost) == 0)
	  {
	    added += (int64_t) ost.st_size;
	    nfiles++;
	  }
	if (wrote_bin_meta && stat (bin_path, &ost) == 0)
	  {
	    added += (int64_t) ost.st_size;
	    nfiles++;
	  }
	if (nfiles)
	  cc_evict_note_store (cc_key_hex, added, nfiles);
      }
    }
  else
    unlink (obj_path);		/* clean up a placed-but-unannotated object */

  free (meta);
  free (diag_bytes);
  free (obj_path);
  free (bin_path);
  cc_meta_clear ();
}

/* ------------------------------------------------------------------------ */
/* Transparent auto-PCH (compiler side)                                     */
/* ------------------------------------------------------------------------ */

/* Called from c_common_read_pch after a PCH was successfully consumed.
   ORIG_NAME is the header name the PCH was found for.  Records whether the
   consumed PCH is the auto-PCH stub this compile was handed
   (-fauto-pch-ref) or a foreign one; see cc_pch_consumed[_ours].  */
void
compile_cache_note_pch_read (const char *orig_name)
{
  cc_pch_consumed = true;
  if (flag_auto_pch_ref && orig_name
      && filename_cmp (orig_name, flag_auto_pch_ref) == 0)
    cc_pch_consumed_ours = true;
}

/* Auto-PCH manifest collection: one included file's identity.  */
struct cc_apch_collect_state
{
  cc_input *v;
  unsigned n, cap;
  bool oom_fail;
};

/* cpp_included_file_cb: record PATH + size + mtime + per-file SHA-1, using
   the digest libcpp stored at read time when present (the gch build runs
   with the cache configured, so hash-on-read was active), else hashing the
   handed bytes -- the exact contract cc_hash_one_file uses.  */
static bool
cc_apch_collect_one (const char *path, const unsigned char *buffer,
		     size_t size, const unsigned char *content_sha1,
		     unsigned /* file_flags */, void *user)
{
  /* The gch manifest keeps every record path-validated (its "main source"
     is the synthesized prelude stub, not a key-committed input), so the
     per-file facts are deliberately not folded in here.  */
  struct cc_apch_collect_state *st = (struct cc_apch_collect_state *) user;

  unsigned char fh[20];
  if (content_sha1)
    memcpy (fh, content_sha1, 20);
  else
    {
      struct sha1_ctx fctx;
      sha1_init_ctx (&fctx);
      if (size)
	sha1_process_bytes (buffer, size, &fctx);
      sha1_finish_ctx (&fctx, fh);
    }

  struct cc_statid id;
  uint32_t mflags = cc_capture_statid (path, (uint64_t) size, &id);

  if (st->n == st->cap)
    {
      unsigned ncap = st->cap ? st->cap * 2 : 32;
      st->v = XRESIZEVEC (cc_input, st->v, ncap);
      st->cap = ncap;
    }
  cc_input *in = &st->v[st->n++];
  in->path = xstrdup (path);
  in->size = size;
  in->id = id;
  in->mhr_flags = mflags;
  memcpy (in->hash, fh, 20);
  return true;
}

/* Auto-PCH probe collection: the gch build's evaluated __has_include probes.
   Verifiable ones are recorded in the manifest entry so the driver's probe
   re-validates them before injecting the PCH; any unverifiable one instead
   sets the entry's CC_MAN_EFLAG_UNVERIFIED_PROBES flag, which makes the
   driver answer NEGATIVE (compile without the PCH -- the TU then evaluates
   its probes itself) and the gch-merge distrust the key.  */
struct cc_apch_probe_state
{
  cc_probe *v;
  unsigned n, cap;
  bool unverifiable;
};

static bool
cc_apch_collect_probe (const char *name, unsigned flags, const char *resolved,
		       const char *const *candidates, unsigned n_candidates,
		       void *user)
{
  struct cc_apch_probe_state *st = (struct cc_apch_probe_state *) user;
  if (!(flags & CPP_HI_PROBE_VERIFIABLE))
    st->unverifiable = true;
  else
    cc_probe_vec_add (&st->v, &st->n, &st->cap, name, flags, resolved,
		      candidates, n_candidates);
  return true;
}

/* Called from c_common_write_pch after the .gch was fully written.  When
   the driver requested it (-fauto-pch-store=PATH on this PCH build), write
   the include closure this .gch baked in -- plus the __has_include probes it
   evaluated -- as a single-entry manifest (the same on-disk format the .o
   cache's manifests use, so the driver validates it with the same code) to
   PATH.  PATH is a driver-owned temp; the driver renames it into the entry
   on success, so no atomicity is needed here.  Any failure leaves PATH
   absent/short, which the driver treats as a failed generation (negative
   entry).  */
void
compile_cache_auto_pch_store (cpp_reader *pfile)
{
  const char *out = flag_auto_pch_store;
  if (!out || !out[0] || !pfile)
    return;

  struct cc_apch_collect_state st;
  st.v = NULL;
  st.n = 0;
  st.cap = 0;
  st.oom_fail = false;

  struct cc_apch_probe_state ps;
  ps.v = NULL;
  ps.n = 0;
  ps.cap = 0;
  ps.unverifiable = false;

  bool ok = cpp_foreach_included_file (pfile, cc_apch_collect_one, &st);

  if (ok)
    {
      cpp_foreach_has_include_probe (pfile, cc_apch_collect_probe, &ps);

      cc_blob strings = { NULL, 0, 0 };
      cc_blob entries = { NULL, 0, 0 };

      /* Size the (single) entry up front so string offsets are emitted
	 absolute (the string area follows the entries section directly).  */
      uint64_t entries_total
	= CC_MAN_ENT_HEAD_SIZE + (uint64_t) st.n * CC_MAN_HDR_REC_SIZE;
      for (unsigned i = 0; i < ps.n; i++)
	entries_total += CC_MAN_PROBE_REC_FIXED_SIZE
			 + (uint64_t) ps.v[i].n_candidates * 4;
      uint64_t entries_off = CC_MANIFEST_HEADER_SIZE;
      uint64_t string_area_off = entries_off + entries_total;

      unsigned char head[CC_MAN_ENT_HEAD_SIZE];
      memset (head, 0, sizeof (head));	/* OK slot unused: 20 zero bytes */
      cc_put_u32 (head + CC_MENT_OFF_FLAGS,
		  ps.unverifiable ? CC_MAN_EFLAG_UNVERIFIED_PROBES : 0);
      cc_put_u32 (head + CC_MENT_OFF_HDR_COUNT, st.n);
      cc_put_u32 (head + CC_MENT_OFF_PROBE_COUNT, ps.n);
      cc_blob_append (&entries, head, CC_MAN_ENT_HEAD_SIZE);

      for (unsigned i = 0; i < st.n; i++)
	cc_emit_hdr_rec (&entries,
			 (uint32_t) string_area_off
			 + cc_blob_add_string (&strings, st.v[i].path),
			 &st.v[i]);

      for (unsigned i = 0; i < ps.n; i++)
	{
	  const cc_probe *p = &ps.v[i];
	  uint32_t pflags = 0;
	  if (p->flags & CPP_HI_PROBE_FOUND)
	    pflags |= CC_MPR_FLAG_FOUND;
	  if (p->flags & CPP_HI_PROBE_BRACKET)
	    pflags |= CC_MPR_FLAG_BRACKET;

	  unsigned char fixed[CC_MAN_PROBE_REC_FIXED_SIZE];
	  cc_put_u32 (fixed + CC_MPR_OFF_FLAGS, pflags);
	  cc_put_u32 (fixed + CC_MPR_OFF_NAME,
		      (uint32_t) string_area_off
		      + cc_blob_add_string (&strings, p->name));
	  cc_put_u32 (fixed + CC_MPR_OFF_RESOLVED,
		      p->resolved
		      ? (uint32_t) string_area_off
			+ cc_blob_add_string (&strings, p->resolved)
		      : 0);
	  cc_put_u32 (fixed + CC_MPR_OFF_NCAND, p->n_candidates);
	  cc_blob_append (&entries, fixed, CC_MAN_PROBE_REC_FIXED_SIZE);
	  for (unsigned k = 0; k < p->n_candidates; k++)
	    {
	      unsigned char co[4];
	      cc_put_u32 (co, (uint32_t) string_area_off
			      + cc_blob_add_string (&strings,
						    p->candidates[k]));
	      cc_blob_append (&entries, co, 4);
	    }
	}

      gcc_checking_assert (entries.len == entries_total);

      unsigned char mhdr[CC_MANIFEST_HEADER_SIZE];
      memset (mhdr, 0, sizeof (mhdr));
      memcpy (mhdr + CC_MAN_OFF_MAGIC, CC_MANIFEST_MAGIC, CC_MAGIC_LEN);
      cc_put_u16 (mhdr + CC_MAN_OFF_VERSION, (uint16_t) CC_MANIFEST_VERSION);
      cc_put_u32 (mhdr + CC_MAN_OFF_ENTRY_COUNT, 1);
      cc_put_u64 (mhdr + CC_MAN_OFF_ENTRIES_OFF, entries_off);

      FILE *f = fopen (out, "wb");
      ok = (f != NULL);
      if (ok)
	{
	  ok = (fwrite (mhdr, 1, sizeof (mhdr), f) == sizeof (mhdr));
	  if (ok && entries.len)
	    ok = (fwrite (entries.data, 1, entries.len, f) == entries.len);
	  if (ok && strings.len)
	    ok = (fwrite (strings.data, 1, strings.len, f) == strings.len);
	  if (fclose (f) != 0)
	    ok = false;
	}
      if (!ok)
	unlink (out);		/* leave no half-written manifest behind */

      free (entries.data);
      free (strings.data);
    }

  cc_debug_line (ok ? "auto-pch-manifest-store" : "auto-pch-manifest-FAIL",
		 NULL);

  for (unsigned i = 0; i < st.n; i++)
    free (st.v[i].path);
  free (st.v);
  for (unsigned i = 0; i < ps.n; i++)
    {
      free (ps.v[i].name);
      free (ps.v[i].resolved);
      for (unsigned k = 0; k < ps.v[i].n_candidates; k++)
	free (ps.v[i].candidates[k]);
      free (ps.v[i].candidates);
    }
  free (ps.v);
}
