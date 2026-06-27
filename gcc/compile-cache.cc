/* In-compiler compilation cache (cache the assembly .s).

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
static bool cc_key_valid = false;

/* Set once compile_cache_try_serve() reports a hit.  */
static bool cc_hit = false;

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
  CC_TAG_VERSION = 9	/* key-schema version */
};

/* Bump when the key construction or cached payload format changes, to
   invalidate stale entries written by an older compiler.  */
#define CC_KEY_SCHEMA_VERSION 1u

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
    case OPT_I:
    case OPT_iquote:
    case OPT_isystem:
    case OPT_idirafter:
    case OPT_iprefix:
    case OPT_iwithprefix:
    case OPT_iwithprefixbefore:
    case OPT_isysroot:
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

/* Closure-walk state shared with the cpp_foreach_included_file callback.  */
struct cc_closure_state
{
  struct sha1_ctx *ctx;
};

/* Callback: hash one included file's path + exact bytes into the digest.  */
static bool
cc_hash_one_file (const char *path, const unsigned char *buffer,
		  size_t size, void *user)
{
  struct cc_closure_state *st = (struct cc_closure_state *) user;
  cc_hash_str (st->ctx, CC_TAG_FILE_PATH, path);
  cc_hash_component (st->ctx, CC_TAG_FILE_BODY, buffer, size);
  return true;			/* keep walking */
}

/* Compute the SHA-1 key for this TU into cc_key_hex / cc_key_valid, using
   PFILE for the include closure.  Returns true on success.  */
static bool
cc_compute_key (cpp_reader *pfile)
{
  struct sha1_ctx ctx;
  sha1_init_ctx (&ctx);

  /* (0) Key schema version, so a future format change self-invalidates.  */
  {
    unsigned char v[4];
    unsigned ver = CC_KEY_SCHEMA_VERSION;
    for (int i = 0; i < 4; i++)
      v[i] = (unsigned char) (ver >> (8 * i));
    cc_hash_component (&ctx, CC_TAG_VERSION, v, sizeof (v));
  }

  /* (1) The compiler binary's own fingerprint.  */
  cc_hash_component (&ctx, CC_TAG_CHECKSUM, executable_checksum, 16);

  /* (2) Language + dialect (lang_hooks.name encodes both, e.g. "GNU C++23").
     CC_TAG_STD is reserved/empty -- the -std=... decoded option is included
     by cc_option_affects_output_p in the option walk below.  */
  cc_hash_str (&ctx, CC_TAG_LANG, lang_hooks.name);
  cc_hash_str (&ctx, CC_TAG_STD, "");

  /* Main input path + cwd (they also feed debug info / __FILE__).  */
  cc_hash_str (&ctx, CC_TAG_MAIN_INPUT, main_input_filename);
  {
    const char *pwd = get_src_pwd ();
    cc_hash_str (&ctx, CC_TAG_CWD, pwd ? pwd : "");
  }

  /* (4) Canonicalized codegen/ABI-relevant command-line options, in order.  */
  for (unsigned i = 1; i < save_decoded_options_count; i++)
    {
      const cl_decoded_option *o = &save_decoded_options[i];
      if (!cc_option_affects_output_p (o))
	continue;
      for (size_t k = 0; k < o->canonical_option_num_elements; k++)
	cc_hash_str (&ctx, CC_TAG_OPT, o->canonical_option[k]);
    }

  /* (3) The source closure: paths + exact bytes of every stacked file.  */
  struct cc_closure_state st;
  st.ctx = &ctx;
  if (!cpp_foreach_included_file (pfile, cc_hash_one_file, &st))
    return false;		/* a file could not be re-read; don't trust key */

  unsigned char raw[20];
  sha1_finish_ctx (&ctx, raw);
  cc_hex (raw, cc_key_hex);
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

/* Build "DIR/ab/cdef...rest.s" into a freshly xmalloc'd string (caller frees
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

  char *full = concat (shard_dir, "/", key + 2, ".s", NULL);
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

  /* Derive a TU-unique seed from a parse-independent subset: the compiler
     checksum, the main input path + cwd, and the codegen-relevant flags.  We
     do NOT use the include closure here (it isn't known pre-parse, and the
     seed only needs to be unique per main file so two different TUs don't
     collide on generated symbol names).  Determinism of the *output* across
     runs is what enables a cache hit.  */
  struct sha1_ctx ctx;
  sha1_init_ctx (&ctx);
  cc_hash_component (&ctx, CC_TAG_CHECKSUM, executable_checksum, 16);
  cc_hash_str (&ctx, CC_TAG_MAIN_INPUT, main_input_filename);
  {
    const char *pwd = get_src_pwd ();
    cc_hash_str (&ctx, CC_TAG_CWD, pwd ? pwd : "");
  }
  for (unsigned i = 1; i < save_decoded_options_count; i++)
    {
      const cl_decoded_option *o = &save_decoded_options[i];
      if (!cc_option_affects_output_p (o))
	continue;
      for (size_t k = 0; k < o->canonical_option_num_elements; k++)
	cc_hash_str (&ctx, CC_TAG_OPT, o->canonical_option[k]);
    }

  unsigned char raw[20];
  sha1_finish_ctx (&ctx, raw);

  /* set_random_seed() uses a pure-hex string verbatim (strtoul base 0).  Emit
     "0x" + the first 16 hex digits (64 bits) of the digest.  */
  char seed[3 + 16 + 1];
  static const char hexd[] = "0123456789abcdef";
  seed[0] = '0';
  seed[1] = 'x';
  for (int i = 0; i < 8; i++)
    {
      seed[2 + 2 * i] = hexd[(raw[i] >> 4) & 0xf];
      seed[2 + 2 * i + 1] = hexd[raw[i] & 0xf];
    }
  seed[18] = '\0';
  set_random_seed (seed);
}

/* ------------------------------------------------------------------------ */
/* Public: serve                                                            */
/* ------------------------------------------------------------------------ */

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

  FILE *in = fopen (path, "rb");
  if (!in)
    {
      free (path);
      cc_debug_line ("miss", cc_key_hex);
      return false;		/* miss */
    }

  /* Hit: replace asm_out_file's contents with the cached assembly.
     init_asm_output() has already written the target preamble into
     asm_out_file; the cached .s is the *complete* final assembly (it has its
     own preamble), so we must substitute the stored bytes wholesale.  Close
     and reopen the file with "wb" (which truncates) rather than rely on
     ftruncate(), which GCC's config does not probe.  finalize() will close
     this fresh handle as usual.  asm_file_name is a real, seekable path here
     (gated in compile_cache_enabled_p).  Once we have committed to truncating
     the back-end's output, a reopen failure is fatal: there is no correct
     output to fall back to.  */
  fclose (asm_out_file);
  asm_out_file = fopen (asm_file_name, "wb");
  if (!asm_out_file)
    {
      fclose (in);
      fatal_error (input_location,
		   "compilation cache: cannot reopen %qs", asm_file_name);
    }

  char buf[65536];
  size_t got;
  bool ok = true;
  while ((got = fread (buf, 1, sizeof (buf), in)) > 0)
    if (fwrite (buf, 1, got, asm_out_file) != got)
      {
	ok = false;
	break;
      }
  if (ferror (in))
    ok = false;
  fclose (in);
  free (path);

  if (!ok)
    {
      cc_debug_line ("miss", cc_key_hex);
      return false;		/* treat partial copy as a miss */
    }

  fflush (asm_out_file);
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

void
compile_cache_store (void)
{
  if (!compile_cache_enabled_p ())
    return;
  if (!cc_key_valid)
    return;			/* serve was never run / key untrusted */
  if (cc_hit)
    return;			/* nothing new to store on a hit */
  if (seen_error ())
    return;			/* don't cache the output of a failed compile */

  /* The freshly produced assembly lives at asm_file_name and has been closed
     by finalize() before we are called.  Copy it into a temp file in the
     shard dir and atomically rename into place.  */
  FILE *src = fopen (asm_file_name, "rb");
  if (!src)
    return;

  char *final_path = cc_entry_path (cc_key_hex, /*make_dirs=*/true);

  /* Temp file in the same directory so rename() is atomic on the same fs.  */
  char *tmp_path = concat (final_path, ".tmpXXXXXX", NULL);
  int tfd = mkstemp (tmp_path);
  if (tfd < 0)
    {
      fclose (src);
      free (tmp_path);
      free (final_path);
      return;
    }
  FILE *dst = fdopen (tfd, "wb");
  if (!dst)
    {
      close (tfd);
      unlink (tmp_path);
      fclose (src);
      free (tmp_path);
      free (final_path);
      return;
    }

  char buf[65536];
  size_t got;
  bool ok = true;
  while ((got = fread (buf, 1, sizeof (buf), src)) > 0)
    if (fwrite (buf, 1, got, dst) != got)
      {
	ok = false;
	break;
      }
  if (ferror (src))
    ok = false;
  fclose (src);
  if (fclose (dst) != 0)
    ok = false;

  if (ok)
    {
      /* Atomic publish.  If another process won the race the rename simply
	 replaces an identical-keyed entry; harmless.  */
      if (rename (tmp_path, final_path) != 0)
	unlink (tmp_path);
      else
	cc_debug_line ("store", cc_key_hex);
    }
  else
    unlink (tmp_path);

  free (tmp_path);
  free (final_path);
}
