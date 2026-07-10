/* In-compiler compilation cache: driver-usable serve unit (no libcpp).

   Implements compile_cache_serve_object (), the SHARED warm-hit path used by
   BOTH the driver (gcc.cc) and the compiler (cc1/cc1plus).  It computes the
   manifest key, reads the manifest + object, re-resolves the recorded headers
   by absolute path, and places the cached .o -- using only sha1 (libiberty),
   file I/O, and the on-disk format (compile-cache-format.h).  No libcpp, no
   tree/rtl/langhooks, so it links into the driver as cleanly as into cc1plus.

   The libcpp-dependent STORE path (the include-closure walk that WRITES the
   manifest/object on a miss) stays in compile-cache.cc.

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
#include "options.h"		/* OPT_* enumerators */
#include "opts.h"		/* cl_decoded_option, cl_options[], cl_options_count */
#include "sha1.h"
#include "compile-cache-format.h"
#include "compile-cache-serve.h"

#include <sys/stat.h>
/* For the opportunistic reflink rung of cc_place_object().  Guarded so the
   feature compiles in only where the kernel headers expose FICLONE (Linux);
   elsewhere cc_place_object falls back to hardlink/copy.  */
#if defined (__linux__)
# include <sys/ioctl.h>
# if defined (__has_include)
#  if __has_include (<linux/fs.h>)
#   include <linux/fs.h>
#  endif
# endif
#endif
#if defined (HAVE_MMAP_FILE) || defined (HAVE_SYS_MMAN_H) || defined (__linux__)
# include <sys/mman.h>
# define CCS_HAVE_MMAP 1
#endif

/* The v3 per-object metadata lives in an xattr on the cache .o; fall back to
   the .bin sidecar where xattrs are unavailable.  */
#if defined (__has_include)
# if __has_include (<sys/xattr.h>)
#  include <sys/xattr.h>
#  define CCS_HAVE_XATTR 1
# endif
#endif

/* ------------------------------------------------------------------------ */
/* Small helpers (self-contained: no compiler globals)                      */
/* ------------------------------------------------------------------------ */

/* Feed a 1-byte tag, an 8-byte little-endian length, then the bytes, into
   CTX.  The tag+length framing makes the key unambiguous.  Identical framing
   to compile-cache.cc's cc_hash_component, so a key computed here matches one
   computed there over the same components.  */
static void
ccs_hash_component (struct sha1_ctx *ctx, unsigned char tag,
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
ccs_hash_str (struct sha1_ctx *ctx, unsigned char tag, const char *s)
{
  ccs_hash_component (ctx, tag, s ? s : "", s ? strlen (s) : 0);
}

/* Read the whole file at PATH into a freshly xmalloc'd buffer; return it and
   set *LEN.  Returns NULL on any error.  A trailing NUL is appended.  */
static unsigned char *
ccs_read_file (const char *path, size_t *len)
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

/* True when the environment requests the airtight serve mode; see the
   declaration in compile-cache-serve.h.  */
bool
cc_verify_hash_env_p (void)
{
  const char *v = getenv ("GCC_COMPILE_CACHE_VERIFY");
  if (v && !strcmp (v, "hash"))
    return true;
  v = getenv ("GCC_COMPILE_CACHE_PARANOID");
  return v && v[0] && strcmp (v, "0") != 0;
}

/* Last-used bump for LRU eviction; see compile-cache-serve.h.  utimensat is
   POSIX.1-2008; hosts without it simply skip the bump (eviction still works
   off store-time mtimes, just without hit refreshes).  Owner-set explicit
   times work on the 0444 cache objects.  */
void
cc_touch_entry (const char *path)
{
#if defined (AT_FDCWD) && defined (UTIME_NOW)
  struct stat st;
  if (stat (path, &st) != 0)
    return;
  time_t now = time (NULL);
  if (st.st_mtime >= now - 3600)
    return;			/* fresh enough: skip the inode write */
  struct timespec ts[2];
  ts[0].tv_sec = 0;
  ts[0].tv_nsec = UTIME_NOW;
  ts[1].tv_sec = 0;
  ts[1].tv_nsec = UTIME_NOW;
  (void) utimensat (AT_FDCWD, path, ts, 0);
#else
  (void) path;
#endif
}

/* Emit one debug line:  "compile-cache: <action> <key12> <output>".  */
static void
ccs_debug_line (const cc_serve_ctx *ctx, const char *action, const char *key,
		const char *out)
{
  if (!ctx->debug)
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
	   out ? out : "-");
  fflush (stderr);
}

/* Validate the CC_MAN_HDR_REC_SIZE records of one manifest entry against the
   filesystem: every recorded file must still exist with the recorded FULL
   stat identity -- size, mtime sec+nsec, ctime sec+nsec, dev, ino (the stat
   shortcut, ccache's inode-cache bar; only trusted when the record carries
   CC_MHR_FLAG_HAS_STATID) -- or, failing that, re-hash to the recorded SHA-1.
   A same-size rewrite that restores mtime (touch -d) still advances ctime,
   so it falls through to the re-hash and is caught.  MAN/MLEN is the whole
   manifest buffer; RECS_OFF/HDR_COUNT locate the entry's records.
   VERIFY_HASH forces the content re-hash.  Shared by the warm .o serve and
   the auto-PCH probe.  */
static bool
ccs_records_match (const unsigned char *man, size_t mlen, uint64_t recs_off,
		   uint32_t hdr_count, bool verify_hash)
{
  for (uint32_t hi = 0; hi < hdr_count; hi++)
    {
      const unsigned char *rec = man + recs_off
				 + (uint64_t) hi * CC_MAN_HDR_REC_SIZE;
      uint32_t path_off = cc_get_u32 (rec + CC_MHR_OFF_PATH);
      uint32_t rflags = cc_get_u32 (rec + CC_MHR_OFF_FLAGS);
      uint64_t want_size = cc_get_u64 (rec + CC_MHR_OFF_SIZE);
      const unsigned char *want_hash = rec + CC_MHR_OFF_HASH;

      if (rflags & ~CC_MHR_FLAG_KNOWN_MASK)
	return false;		/* written by a future format: never match */

      if (path_off + 4 > mlen)
	return false;
      uint32_t plen = cc_get_u32 (man + path_off);
      if ((uint64_t) path_off + 4 + plen + 1 > mlen)
	return false;
      const char *hpath = (const char *) (man + path_off + 4);

      struct stat stt;
      if (stat (hpath, &stt) != 0)
	return false;

      if (!verify_hash && (rflags & CC_MHR_FLAG_HAS_STATID))
	{
	  struct cc_statid id;
	  cc_statid_from_stat (&stt, &id);
	  if (id.size == want_size
	      && id.mtime_s == cc_get_u64 (rec + CC_MHR_OFF_MTIME)
	      && id.ctime_s == cc_get_u64 (rec + CC_MHR_OFF_CTIME)
	      && id.dev == cc_get_u64 (rec + CC_MHR_OFF_DEV)
	      && id.ino == cc_get_u64 (rec + CC_MHR_OFF_INO)
	      && id.mtime_ns == cc_get_u32 (rec + CC_MHR_OFF_MTIME_NSEC)
	      && id.ctime_ns == cc_get_u32 (rec + CC_MHR_OFF_CTIME_NSEC))
	    continue;
	}

      size_t got_len = 0;
      unsigned char *body = ccs_read_file (hpath, &got_len);
      if (!body || (uint64_t) got_len != want_size)
	{
	  free (body);
	  return false;
	}
      unsigned char got_hash[20];
      struct sha1_ctx fctx;
      sha1_init_ctx (&fctx);
      if (got_len)
	sha1_process_bytes (body, got_len, &fctx);
      sha1_finish_ctx (&fctx, got_hash);
      free (body);
      if (memcmp (got_hash, want_hash, 20) != 0)
	return false;
    }
  return true;
}

/* ------------------------------------------------------------------------ */
/* Shared manifest-entry access (the ONE interpreter of the v2 entry bytes)  */
/* ------------------------------------------------------------------------ */

/* Fetch a length-prefixed + NUL string from the manifest string area with
   bounds checks; NULL if OFF is out of range.  */
const char *
cc_man_string (const unsigned char *man, size_t mlen, uint32_t off)
{
  if ((uint64_t) off + 4 > mlen)
    return NULL;
  uint32_t slen = cc_get_u32 (man + off);
  if ((uint64_t) off + 4 + slen + 1 > mlen)
    return NULL;
  return (const char *) (man + off + 4);
}

/* Parse + bounds-check the manifest entry at OFF into *OUT.  The probe
   records are variable-length, so finding NEXT_OFF walks them (checking each
   fits); the header records are fixed-size.  Returns false on truncation.  */
bool
cc_man_entry_parse (const unsigned char *man, size_t mlen, uint64_t off,
		    struct cc_man_entry *out)
{
  if (off + CC_MAN_ENT_HEAD_SIZE > mlen)
    return false;
  const unsigned char *ent = man + off;
  out->ok_raw = ent + CC_MENT_OFF_OK;
  out->warnings = cc_get_u32 (ent + CC_MENT_OFF_WARNINGS);
  out->werrors = cc_get_u32 (ent + CC_MENT_OFF_WERRORS);
  out->eflags = cc_get_u32 (ent + CC_MENT_OFF_FLAGS);
  out->hdr_count = cc_get_u32 (ent + CC_MENT_OFF_HDR_COUNT);
  out->probe_count = cc_get_u32 (ent + CC_MENT_OFF_PROBE_COUNT);

  out->hdr_recs_off = off + CC_MAN_ENT_HEAD_SIZE;
  uint64_t recs_len = (uint64_t) out->hdr_count * CC_MAN_HDR_REC_SIZE;
  if (out->hdr_recs_off + recs_len > mlen)
    return false;
  out->probe_recs_off = out->hdr_recs_off + recs_len;

  uint64_t cur = out->probe_recs_off;
  for (uint32_t pi = 0; pi < out->probe_count; pi++)
    {
      if (cur + CC_MAN_PROBE_REC_FIXED_SIZE > mlen)
	return false;
      uint32_t ncand = cc_get_u32 (man + cur + CC_MPR_OFF_NCAND);
      uint64_t rec_len
	= CC_MAN_PROBE_REC_FIXED_SIZE + (uint64_t) ncand * 4;
      if (cur + rec_len > mlen)
	return false;
      cur += rec_len;
    }
  out->next_off = cur;
  return true;
}

/* Re-verify ENT's probe records against the filesystem with pure stat()
   logic; the candidates are pre-joined paths, so no include-search
   reconstruction happens here.  Conservative on every edge: any state the
   store side did not prove (a candidate now existing, a resolved path gone
   or turned into a directory, unknown flag bits) rejects the entry -- the
   caller then falls back to a real compile, which recomputes the truth.  */
static bool
ccs_probes_match (const unsigned char *man, size_t mlen,
		  const struct cc_man_entry *ent)
{
  uint64_t cur = ent->probe_recs_off;
  for (uint32_t pi = 0; pi < ent->probe_count; pi++)
    {
      const unsigned char *rec = man + cur;
      uint32_t pflags = cc_get_u32 (rec + CC_MPR_OFF_FLAGS);
      uint32_t ncand = cc_get_u32 (rec + CC_MPR_OFF_NCAND);
      cur += CC_MAN_PROBE_REC_FIXED_SIZE + (uint64_t) ncand * 4;

      if (pflags & ~CC_MPR_FLAG_KNOWN_MASK)
	return false;		/* written by a future format: never match */

      /* Every candidate the store-side search proved absent must still be
	 absent -- a file appearing there would change the resolution (or
	 flip a negative probe to found).  */
      for (uint32_t ci = 0; ci < ncand; ci++)
	{
	  uint32_t coff = cc_get_u32 (rec + CC_MAN_PROBE_REC_FIXED_SIZE
				      + ci * 4);
	  const char *cand = cc_man_string (man, mlen, coff);
	  if (!cand)
	    return false;
	  struct stat stt;
	  if (stat (cand, &stt) == 0)
	    return false;	/* appeared -> stale */
	}

      /* A FOUND probe's resolved path must still exist; a directory does
	 not count (cpp's open_file treats those as ENOENT).  */
      if (pflags & CC_MPR_FLAG_FOUND)
	{
	  const char *resolved
	    = cc_man_string (man, mlen, cc_get_u32 (rec + CC_MPR_OFF_RESOLVED));
	  struct stat stt;
	  if (!resolved || stat (resolved, &stt) != 0
	      || S_ISDIR (stt.st_mode))
	    return false;
	}
    }
  return true;
}

/* Header records + probe records; see compile-cache-serve.h.  */
bool
cc_man_entry_records_valid (const unsigned char *man, size_t mlen,
			    const struct cc_man_entry *ent, bool verify_hash)
{
  if (ent->eflags & ~CC_MAN_EFLAG_KNOWN_MASK)
    return false;		/* written by a future format: never match */
  return ccs_records_match (man, mlen, ent->hdr_recs_off, ent->hdr_count,
			    verify_hash)
	 && ccs_probes_match (man, mlen, ent);
}

/* ------------------------------------------------------------------------ */
/* Option predicates (SHARED with compile-cache.cc -- keep in lockstep)     */
/* ------------------------------------------------------------------------ */

/* Return true if DECODED can change the generated object / ABI and so must
   participate in the key.  This MUST stay byte-for-byte equivalent to
   compile-cache.cc's cc_option_affects_output_p () -- the driver and cc1plus
   both call it (on the same decoded cc1 argv) to build the manifest key, and
   any divergence means the driver computes a different key and never hits.  */
bool
cc_option_affects_output_p (const cl_decoded_option *decoded)
{
  size_t idx = decoded->opt_index;

  if (idx >= cl_options_count)
    return false;

  switch (idx)
    {
    case OPT_o:
    case OPT_dumpbase:
    case OPT_dumpbase_ext:
    case OPT_dumpdir:
    case OPT_fcompile_cache_:
    /* The auto-PCH plumbing options do not change the produced bytes: the
       injected -include (which does, and is keyed) carries the content
       commitment, and the consumed PCH's closure is folded into the key
       explicitly by the store-side merge.  Excluding these keeps a TU's key
       stable across -fauto-pch on/off when no PCH ends up injected.  */
    case OPT_fauto_pch:
    case OPT_fauto_pch_store_:
    case OPT_fauto_pch_ref_:
    /* Dependency-output plumbing: shapes only the .d side channel, never
       the object bytes, and every serve regenerates the .d from the LIVE
       command line (driver synthesis / libcpp's own deps machinery) -- so
       keying these would only split identical objects across entries.
       Concretely poisonous: the driver spec derives the cc1-level -MD
       argument from -o, so keying it would make the key vary with the
       output path even though the object does not.  -M/-MM/-MG are
       dependency-only modes (no object is produced) and never reach a
       cacheable compile; excluded for consistency.  ccache excludes the
       same family from its hash.  */
    case OPT_MD:
    case OPT_MMD:
    case OPT_MF:
    case OPT_MT:
    case OPT_MQ:
    case OPT_MP:
    case OPT_M:
    case OPT_MM:
    case OPT_MG:
    case OPT_Mmodules:
    case OPT_Mno_modules:
    case OPT_fdeps_file_:
    case OPT_fdeps_format_:
    case OPT_fdeps_target_:
      return false;
    default:
      break;
    }

  const struct cl_option *opt = &cl_options[idx];
  unsigned int f = opt->flags;

  const unsigned int CC_CL_ANY_LANG = (CL_MIN_OPTION_CLASS - 1U);

  if (f & CL_WARNING)
    return false;

  if ((f & CL_DRIVER) && !(f & (CL_COMMON | CL_TARGET | CC_CL_ANY_LANG)))
    return false;

  if (f & (CL_OPTIMIZATION | CL_TARGET))
    return true;

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

  switch (idx)
    {
    case OPT_D:
    case OPT_U:
    case OPT_A:
    case OPT_include:
    case OPT_imacros:
    case OPT_nostdinc:
    case OPT_undef:
    case OPT_ansi:
    case OPT_g:
    case OPT_ggdb:
    case OPT_gdwarf:
    case OPT_gdwarf_:
    case OPT_ffile_prefix_map_:
    case OPT_fdebug_prefix_map_:
    case OPT_fmacro_prefix_map_:
    case OPT_fprofile_prefix_map_:
      return true;
    default:
      break;
    }

  if (f & (CL_COMMON | CC_CL_ANY_LANG))
    return true;

  return false;
}

/* True if DECODED names an include search path whose VALUE folds into the
   MANIFEST key (anti-shadowing).  MUST match compile-cache.cc's
   cc_option_is_search_path_p ().  */
bool
cc_option_is_search_path_p (const cl_decoded_option *decoded)
{
  size_t idx = decoded->opt_index;
  if (idx >= cl_options_count)
    return false;
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
    case OPT_nostdinc:
      return true;
    default:
      return false;
    }
}

/* Relativize one search-path VALUE against CWD for MANIFEST-key hashing
   (ccache base_dir parity): a search dir INSIDE the current (build)
   directory hashes as its cwd-relative form, so two build trees differing
   only in their absolute location -- e.g. cmake's absolute -I<builddir>/sub
   for generated headers -- produce the same MK and share manifest entries.
   This lowers only the MK collision bar (MK is a lookup index): every serve
   still re-verifies the recorded per-header identities and the object key
   stays a full-closure content address, so a candidate whose headers do not
   match simply falls through to a real compile.  A value outside CWD, or a
   relative one, hashes unchanged, preserving the anti-shadowing bar for
   every directory the build tree does not own.  See "Deliberate QoI trades"
   in .github/TESTSUITE.md.

   Returns PATH itself, a suffix of PATH, or a literal "." -- never an
   allocation.  Non-path elements of a search option (the "-I" token itself)
   pass through untouched (they never begin with CWD).  Both MK twins
   (ccs_compute_manifest_key here / cc_compute_manifest_key in
   compile-cache.cc) MUST call this with their side's cwd -- the same value
   CC_TAG_CWD hashes under -g (driver: ctx->cwd; cc1plus: get_src_pwd ()),
   which is what keeps the twins byte-identical.  */
const char *
cc_mk_search_path_relative (const char *path, const char *cwd)
{
  if (!path || !cwd || !cwd[0] || !IS_ABSOLUTE_PATH (path))
    return path;
  size_t cwd_len = strlen (cwd);
  /* Ignore trailing separators on CWD; a bare root ("/") never relativizes
     (cwd_len stays 1 and the separator check below cannot pass).  */
  while (cwd_len > 1 && IS_DIR_SEPARATOR (cwd[cwd_len - 1]))
    cwd_len--;
  if (cwd_len <= 1 || filename_ncmp (path, cwd, cwd_len) != 0)
    return path;
  if (path[cwd_len] == '\0')
    return ".";			/* the search dir IS the cwd */
  if (!IS_DIR_SEPARATOR (path[cwd_len]))
    return path;		/* /foo vs /foobar: not inside */
  const char *rel = path + cwd_len;
  while (IS_DIR_SEPARATOR (*rel))
    rel++;
  return rel[0] ? rel : ".";	/* "/cwd///" collapses to "." */
}

/* ------------------------------------------------------------------------ */
/* B2: prefix-map-aware -g keys (see compile-cache-serve.h)                 */
/* ------------------------------------------------------------------------ */

void
cc_pmaps_collect (struct cc_prefix_maps *pm, const cl_decoded_option *decoded,
		  unsigned decoded_count)
{
  pm->ents = NULL;
  pm->count = 0;
  pm->dropped = NULL;
  pm->dropped_n = 0;

  unsigned cap = 0;
  bool canon = false;		/* -fcanon-prefix-map is Init(0) */
  for (unsigned i = 1; i < decoded_count; i++)
    {
      const cl_decoded_option *o = &decoded[i];
      if (o->opt_index == OPT_fcanon_prefix_map)
	{
	  canon = (o->value != 0);
	  continue;
	}
      if (o->opt_index != OPT_ffile_prefix_map_
	  && o->opt_index != OPT_fdebug_prefix_map_)
	continue;
      const char *arg = o->arg;
      const char *p = arg ? strrchr (arg, '=') : NULL;
      if (!p)
	continue;	/* malformed (cc1 errors); stays hashed raw */
      if (pm->count == cap)
	{
	  cap = cap ? cap * 2 : 4;
	  pm->ents = XRESIZEVEC (struct cc_pmap_ent, pm->ents, cap);
	}
      struct cc_pmap_ent *e = &pm->ents[pm->count++];
      e->canonicalize = canon;
      e->old_prefix = xstrndup (arg, p - arg);
      e->old_len = (size_t) (p - arg);
      if (e->canonicalize)
	{
	  char *realname = lrealpath (e->old_prefix);
	  free (e->old_prefix);
	  e->old_prefix = realname;
	  e->old_len = strlen (realname);
	}
      e->new_prefix = xstrdup (p + 1);
      e->new_len = strlen (e->new_prefix);
      e->opt_i = i;
    }
}

/* Does E's OLD prefix match FILENAME (per E's canonicalize rule)?  The
   exact comparison remap_filename performs when it walks the list.  */
static bool
cc_pmap_ent_matches (const struct cc_pmap_ent *e, const char *filename)
{
  if (!filename)
    return false;
  if (!e->canonicalize)
    return filename_ncmp (filename, e->old_prefix, e->old_len) == 0;
  const char *realname;
  bool realname_alloc = false;
  if (lbasename (filename) == filename)
    realname = filename;
  else
    {
      realname = lrealpath (filename);
      realname_alloc = true;
    }
  bool m = filename_ncmp (realname, e->old_prefix, e->old_len) == 0;
  if (realname_alloc)
    free (const_cast <char *> (realname));
  return m;
}

void
cc_pmaps_mark_dropped (struct cc_prefix_maps *pm, unsigned decoded_count,
		       const char *src_path, const char *cwd)
{
  if (pm->count == 0)
    return;
  pm->dropped = XCNEWVEC (bool, decoded_count);
  pm->dropped_n = decoded_count;
  for (unsigned k = 0; k < pm->count; k++)
    {
      const struct cc_pmap_ent *e = &pm->ents[k];
      if (cc_pmap_ent_matches (e, src_path) || cc_pmap_ent_matches (e, cwd))
	pm->dropped[e->opt_i] = true;
    }
}

char *
cc_pmaps_remap_alloc (const struct cc_prefix_maps *pm, const char *filename)
{
  if (!filename)
    return xstrdup ("");
  const char *realname = NULL;
  bool realname_alloc = false;
  const struct cc_pmap_ent *hit = NULL;

  /* Walk BACKWARDS: file-prefix-map.cc prepends each option, so its list
     head -- the first match candidate -- is the LAST option given.  */
  for (unsigned k = pm->count; k-- > 0;)
    {
      const struct cc_pmap_ent *e = &pm->ents[k];
      if (e->canonicalize)
	{
	  if (realname == NULL)
	    {
	      if (lbasename (filename) == filename)
		realname = filename;
	      else
		{
		  realname = lrealpath (filename);
		  realname_alloc = true;
		}
	    }
	  if (filename_ncmp (realname, e->old_prefix, e->old_len) == 0)
	    {
	      hit = e;
	      break;
	    }
	}
      else if (filename_ncmp (filename, e->old_prefix, e->old_len) == 0)
	{
	  hit = e;
	  break;
	}
    }

  char *result;
  if (!hit)
    result = xstrdup (filename);
  else
    {
      const char *name
	= (hit->canonicalize ? realname : filename) + hit->old_len;
      size_t name_len = strlen (name) + 1;
      result = (char *) xmalloc (hit->new_len + name_len);
      memcpy (result, hit->new_prefix, hit->new_len);
      memcpy (result + hit->new_len, name, name_len);
    }
  if (realname_alloc)
    free (const_cast <char *> (realname));
  return result;
}

void
cc_pmaps_free (struct cc_prefix_maps *pm)
{
  for (unsigned k = 0; k < pm->count; k++)
    {
      free (pm->ents[k].old_prefix);
      free (pm->ents[k].new_prefix);
    }
  free (pm->ents);
  free (pm->dropped);
  pm->ents = NULL;
  pm->count = 0;
  pm->dropped = NULL;
  pm->dropped_n = 0;
}

/* ------------------------------------------------------------------------ */
/* Manifest key (MK) computation -- the function both sides MUST share       */
/* ------------------------------------------------------------------------ */

/* Compute the manifest key MK into MK_HEX[41] from CTX + the SRC bytes.
   MK = schema ver + a manifest domain tag + salt + checksum + lang +
   output-affecting options + search-path values + the main source bytes, and
   -- under -g only -- the main source path + cwd.  This is the byte-for-byte
   twin of compile-cache.cc's cc_compute_manifest_key (), differing only in
   that its inputs come from CTX instead of compiler globals.  Returns true on
   success.  */
static bool
ccs_compute_manifest_key (const cc_serve_ctx *ctx, const char *src_path,
			  const unsigned char *src, size_t src_len,
			  char mk_hex[41])
{
  struct sha1_ctx ctx_sha;
  sha1_init_ctx (&ctx_sha);

  /* (0) Schema version.  */
  {
    unsigned char v[4];
    unsigned ver = CC_KEY_SCHEMA_VERSION;
    for (int i = 0; i < 4; i++)
      v[i] = (unsigned char) (ver >> (8 * i));
    ccs_hash_component (&ctx_sha, CC_TAG_VERSION, v, sizeof (v));
  }
  /* Distinct domain tag (matches compile-cache.cc).  */
  ccs_hash_str (&ctx_sha, CC_TAG_LANG, "compile-cache-manifest-key");

  /* (0b) Salt.  */
  {
    const char *salt = getenv ("GCC_COMPILE_CACHE_SALT");
    if (salt && salt[0])
      ccs_hash_str (&ctx_sha, CC_TAG_SALT, salt);
  }

  /* (1) Compiler fingerprint + (2) language/dialect.  */
  ccs_hash_component (&ctx_sha, CC_TAG_CHECKSUM, ctx->checksum, 16);
  ccs_hash_str (&ctx_sha, CC_TAG_LANG, ctx->lang_name);

  /* Under -g the source path + cwd bake into DWARF -- but only after the
     user's -f{file,debug}-prefix-map rewrites, so hash the MAPPED strings
     and drop the map options the mapping consumed (B2; identity + no drops
     when no map matches).  MUST stay byte-for-byte parallel with cc1plus's
     cc_compute_manifest_key.  */
  struct cc_prefix_maps pm;
  memset (&pm, 0, sizeof (pm));
  if (ctx->paths_affect_output)
    {
      cc_pmaps_collect (&pm, ctx->decoded, ctx->decoded_count);
      cc_pmaps_mark_dropped (&pm, ctx->decoded_count, src_path,
			     ctx->cwd ? ctx->cwd : "");
      char *msrc = cc_pmaps_remap_alloc (&pm, src_path);
      char *mcwd = cc_pmaps_remap_alloc (&pm, ctx->cwd ? ctx->cwd : "");
      ccs_hash_str (&ctx_sha, CC_TAG_MAIN_INPUT, msrc);
      ccs_hash_str (&ctx_sha, CC_TAG_CWD, mcwd);
      free (msrc);
      free (mcwd);
    }

  /* (4) Output-affecting options + (anti-shadow) search-path VALUES, in
     command-line order.  decoded[0] is the program-name slot; skip it.
     Map options whose effect is already captured by the mapped src/cwd
     above are excluded (cc_pmaps_opt_dropped_p; never set without -g).
     Search-path values inside the build dir hash cwd-relative so relocated
     build trees share manifests (cc_mk_search_path_relative; ccache base_dir
     parity -- MK is a lookup index, every serve is still record-verified).  */
  for (unsigned i = 1; i < ctx->decoded_count; i++)
    {
      const cl_decoded_option *o = &ctx->decoded[i];
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
	    val = cc_mk_search_path_relative (val,
					      ctx->cwd ? ctx->cwd : "");
	  ccs_hash_str (&ctx_sha, search ? CC_TAG_SEARCH_PATH : CC_TAG_OPT,
			val);
	}
    }
  cc_pmaps_free (&pm);

  /* (S) The main source file's exact bytes.  Fingerprinted with the fast
     128-bit hash (cc_fast128) and the fingerprint folded into MK's SHA-1, so
     the multi-megabyte body is hashed once at several GB/s instead of through
     SHA-1.  MUST match cc1plus's cc_compute_manifest_key.  */
  {
    unsigned char fp[16];
    cc_fast128 (src, src_len, fp);
    ccs_hash_component (&ctx_sha, CC_TAG_SRC_BODY, fp, sizeof (fp));
  }

  unsigned char raw[20];
  sha1_finish_ctx (&ctx_sha, raw);
  cc_hex (raw, mk_hex);
  return true;
}

/* ------------------------------------------------------------------------ */
/* Path construction / object placement                                     */
/* ------------------------------------------------------------------------ */

/* Build "DIR/ab/cdef...rest.bin" into a freshly xmalloc'd string.  KEY is the
   40-char hex.  This serve path never creates directories (it only reads).  */
static char *
ccs_entry_path (const char *cache_dir, const char *key)
{
  char shard[3] = { key[0], key[1], '\0' };
  char *shard_dir = concat (cache_dir, "/", shard, NULL);
  char *full = concat (shard_dir, "/", key + 2, ".bin", NULL);
  free (shard_dir);
  return full;
}

/* For "DIR/ab/rest.bin" the sidecar object is "DIR/ab/rest.o".  Caller frees.  */
static char *
ccs_object_sidecar_path (const char *entry_bin_path)
{
  size_t n = strlen (entry_bin_path);
  if (n >= 4 && strcmp (entry_bin_path + n - 4, ".bin") == 0)
    {
      char *p = (char *) xmalloc (n - 4 + 2 + 1);
      memcpy (p, entry_bin_path, n - 4);
      memcpy (p + (n - 4), ".o", 3);	/* ".o\0" */
      return p;
    }
  return concat (entry_bin_path, ".o", NULL);
}

/* Place the cached object file CACHED_O at DST.  reflink->hardlink->copy
   ladder, selectable via GCC_COMPILE_CACHE_LINK.  Twin of compile-cache.cc's
   cc_place_object ().  The copy rung writes to a temp in DST's dir then
   rename()s atomically, so a killed/ENOSPC write never leaves a truncated .o
   at the user's -o path.  On a same-fs hardlink to the cache object (stored
   0444 by the store side) the served DST is read-only but fully readable and
   correct; reflink/copy leave it writable.  Either way DST is usable -- the
   caller additionally ensures the owner read bit is set.  Returns true on
   success.  */
static bool
ccs_place_object (const char *cached_o, const char *dst)
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
	      unlink (dst);
	    }
	  close (sfd);
	}
      if (mode == LINK_REFLINK)
	return false;
    }
#else
  if (mode == LINK_REFLINK)
    mode = LINK_HARDLINK;
#endif

  if (mode == LINK_AUTO || mode == LINK_HARDLINK)
    {
      if (link (cached_o, dst) == 0)
	return true;
      if (mode == LINK_HARDLINK)
	return false;
    }

  /* Copy fallback: write to a temp file in the SAME directory as DST, then
     rename() it into place atomically.  A killed process or ENOSPC mid-write
     leaves only the temp (unlinked here) -- never a truncated object at the
     user's -o path that would later look valid.  */
  size_t len = 0;
  unsigned char *bytes = ccs_read_file (cached_o, &len);
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
/* v3 metadata read (xattr on the cache .o, .bin sidecar fallback)          */
/* ------------------------------------------------------------------------ */

/* Read the CC_XATTR_META xattr from OBJ_PATH into a freshly xmalloc'd buffer;
   set *LEN.  Returns NULL if absent/unreadable or xattrs are unavailable.
   Byte-twin of compile-cache.cc's cc_get_meta_xattr ().  */
static unsigned char *
ccs_get_meta_xattr (const char *obj_path, size_t *len)
{
  *len = 0;
#ifdef CCS_HAVE_XATTR
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

/* Decode a CC_META record + diag blob.  Byte-twin of cc_parse_meta ().  */
static bool
ccs_parse_meta (const unsigned char *meta, size_t mlen, uint16_t *flags,
		const unsigned char **diag, size_t *diag_len)
{
  if (mlen < CC_META_REC_SIZE
      || memcmp (meta + CC_META_OFF_MAGIC, CC_META_MAGIC, CC_MAGIC_LEN) != 0
      || cc_get_u16 (meta + CC_META_OFF_VERSION) != CC_FORMAT_VERSION)
    return false;
  uint32_t dlen = cc_get_u32 (meta + CC_META_OFF_DIAG_LEN);
  if ((uint64_t) dlen > mlen - CC_META_REC_SIZE)
    return false;
  *flags = cc_get_u16 (meta + CC_META_OFF_FLAGS);
  *diag = dlen ? meta + CC_META_REC_SIZE : NULL;
  *diag_len = dlen;
  return true;
}

/* ------------------------------------------------------------------------ */
/* Object serve                                                             */
/* ------------------------------------------------------------------------ */

/* Read + validate the v3 metadata for the object keyed by OK_HEX (the
   CC_XATTR_META xattr on the cache .o, or the minimal .bin sidecar fallback),
   refuse it if it carries front-end diagnostics (the serve path skips the parse
   that would re-emit them), place the cache .o at OUT_PATH, and replay any
   recorded back-end diagnostics to stderr.  Returns true on a served hit.  This
   is the driver-safe twin of compile-cache.cc's cc_serve_from_bin (), minus the
   global_dc count folding (the serve unit has no diagnostic context; counts
   live in the metadata and are irrelevant to a process that emits no new
   diagnostics and exits 0 -- a TU with -Werror promotions had front-end diags
   and is refused above, and a back-end-only warning count does not change exit
   status on a hit with no errors).  */
static bool
ccs_serve_from_bin (const cc_serve_ctx *ctx, const char *ok_hex,
		    const char *out_path, const char *debug_action)
{
  char *bin_path = ccs_entry_path (ctx->cache_dir, ok_hex);
  char *obj_path = ccs_object_sidecar_path (bin_path);

  size_t mlen = 0;
  unsigned char *meta = ccs_get_meta_xattr (obj_path, &mlen);
  if (!meta)
    meta = ccs_read_file (bin_path, &mlen);

  uint16_t flags = 0;
  const unsigned char *diag = NULL;
  size_t diag_len = 0;
  if (!meta || !ccs_parse_meta (meta, mlen, &flags, &diag, &diag_len))
    {
      free (meta);
      free (obj_path);
      free (bin_path);
      return false;
    }

  /* The serve path must not serve an object whose front-end diagnostics it
     would silently drop.  */
  if (flags & CC_FLAG_HAD_FE_DIAG)
    {
      free (meta);
      free (obj_path);
      free (bin_path);
      return false;
    }

  bool placed = ccs_place_object (obj_path, out_path);
  if (placed)
    /* Mark the entry recently-used so LRU eviction spares it (B1).  */
    cc_touch_entry (obj_path);
  free (obj_path);
  free (bin_path);
  if (!placed)
    {
      free (meta);
      return false;
    }

  /* The served output must be usable by the user.  A same-fs hardlink to the
     0444 cache object lands DST at 0444 (read-only but readable -- acceptable
     per the design); guarantee at least the owner read bit so the served .o is
     never left unreadable.  reflink/copy already leave it writable.  */
  {
    struct stat st;
    if (stat (out_path, &st) == 0 && !(st.st_mode & S_IRUSR))
      chmod (out_path, (st.st_mode & 07777) | S_IRUSR);
  }

  /* Replay the cached back-end diagnostics so a warm hit prints exactly what a
     fresh compile would.  */
  if (diag_len)
    {
      fflush (stdout);
      fwrite (diag, 1, diag_len, stderr);
      fflush (stderr);
    }

  free (meta);
  ccs_debug_line (ctx, debug_action, ok_hex, out_path);
  return true;
}

/* ------------------------------------------------------------------------ */
/* Dependency-file synthesis (driver-tier manifest hit)                     */
/* ------------------------------------------------------------------------ */

/* Write NAME to F with Make quoting, byte-compatible with libcpp's
   mkdeps.cc munge(): backslashes directly preceding a space/tab double, the
   space/tab itself is backslash-escaped, '#' is backslash-escaped, '$'
   doubles.  */
static bool
ccs_deps_munge (FILE *f, const char *name)
{
  for (const char *p = name; *p; p++)
    {
      switch (*p)
	{
	case ' ':
	case '\t':
	  for (const char *q = p - 1; q >= name && *q == '\\'; q--)
	    if (putc ('\\', f) == EOF)
	      return false;
	  if (putc ('\\', f) == EOF)
	    return false;
	  break;
	case '#':
	  if (putc ('\\', f) == EOF)
	    return false;
	  break;
	case '$':
	  if (putc ('$', f) == EOF)
	    return false;
	  break;
	default:
	  break;
	}
      if (putc (*p, f) == EOF)
	return false;
    }
  return true;
}

/* Write the make-style dependency file CTX requested (ctx->deps_path) for a
   manifest hit on ENT: the -MT/-MQ targets, then every header-record path of
   the entry -- which IS the TU's include closure, main source included, i.e.
   exactly the set -MD would have produced (system headers and all).  Under
   -MP (ctx->deps_phony) a phony target follows for every dependency except
   the main source SRC_PATH, matching cpp.  Layout is one logical rule (no
   column wrapping); consumers parse it identically.  Returns false on any
   write failure, after which the caller must treat the serve as a miss (the
   real compile then writes its own file).  */
static bool
ccs_write_deps (const cc_serve_ctx *ctx, const unsigned char *man,
		size_t mlen, const struct cc_man_entry *ent,
		const char *src_path)
{
  FILE *f = fopen (ctx->deps_path, "w");
  if (!f)
    return false;

  bool ok = true;
  for (unsigned i = 0; ok && i < ctx->deps_target_count; i++)
    {
      if (i && putc (' ', f) == EOF)
	ok = false;
      if (!ok)
	break;
      if (ctx->deps_target_quoted && ctx->deps_target_quoted[i])
	ok = ccs_deps_munge (f, ctx->deps_targets[i]);
      else
	ok = (fputs (ctx->deps_targets[i], f) != EOF);
    }
  if (ok)
    ok = (putc (':', f) != EOF);

  for (uint32_t hi = 0; ok && hi < ent->hdr_count; hi++)
    {
      const unsigned char *rec = man + ent->hdr_recs_off
				 + (uint64_t) hi * CC_MAN_HDR_REC_SIZE;
      const char *hpath
	= cc_man_string (man, mlen, cc_get_u32 (rec + CC_MHR_OFF_PATH));
      if (!hpath)
	{
	  ok = false;
	  break;
	}
      ok = (putc (' ', f) != EOF) && ccs_deps_munge (f, hpath);
    }
  if (ok)
    ok = (putc ('\n', f) != EOF);

  if (ok && ctx->deps_phony)
    for (uint32_t hi = 0; ok && hi < ent->hdr_count; hi++)
      {
	const unsigned char *rec = man + ent->hdr_recs_off
				   + (uint64_t) hi * CC_MAN_HDR_REC_SIZE;
	const char *hpath
	  = cc_man_string (man, mlen, cc_get_u32 (rec + CC_MHR_OFF_PATH));
	if (!hpath)
	  {
	    ok = false;
	    break;
	  }
	if (strcmp (hpath, src_path) == 0)
	  continue;		/* cpp emits no phony rule for the source */
	ok = ccs_deps_munge (f, hpath) && (fputs (":\n", f) != EOF);
      }

  if (fclose (f) != 0)
    ok = false;
  if (!ok)
    unlink (ctx->deps_path);	/* no torn file: miss -> real compile */
  return ok;
}

/* ------------------------------------------------------------------------ */
/* Public entry point                                                       */
/* ------------------------------------------------------------------------ */

bool
compile_cache_serve_object (const cc_serve_ctx *ctx, const char *src_path,
			    const char *out_path)
{
  if (!ctx || !ctx->cache_dir || !ctx->cache_dir[0]
      || !ctx->checksum || !ctx->lang_name
      || !src_path || !src_path[0]
      || !out_path || !out_path[0])
    return false;

  /* Map (or read) the main source bytes once for hashing -- no preprocess, no
     parse.  mmap avoids a multi-megabyte malloc+copy of a preprocessed .ii and
     is the bulk of a warm hit's cost; cc_fast128 then fingerprints it at
     several GB/s (see ccs_compute_manifest_key).  */
  size_t src_len = 0;
  unsigned char *src = NULL;
  bool src_mapped = false;
#ifdef CCS_HAVE_MMAP
  {
    int sfd = open (src_path, O_RDONLY);
    if (sfd >= 0)
      {
	struct stat st;
	if (fstat (sfd, &st) == 0 && S_ISREG (st.st_mode) && st.st_size > 0)
	  {
	    void *m = mmap (NULL, (size_t) st.st_size, PROT_READ, MAP_PRIVATE,
			    sfd, 0);
	    if (m != MAP_FAILED)
	      {
		src = (unsigned char *) m;
		src_len = (size_t) st.st_size;
		src_mapped = true;
	      }
	  }
	close (sfd);
      }
  }
#endif
  if (!src)
    {
      /* Fallback (mmap unavailable, or an empty source -- ccs_read_file yields
	 a 1-byte buffer with len 0 for an empty file, NULL only on error).  */
      src = ccs_read_file (src_path, &src_len);
      if (!src)
	return false;
    }

  char mk_hex[41];
  bool mk_ok = ccs_compute_manifest_key (ctx, src_path, src, src_len, mk_hex);
#ifdef CCS_HAVE_MMAP
  if (src_mapped)
    munmap (src, src_len);
  else
#endif
    free (src);
  if (!mk_ok)
    return false;

  char *man_path = ccs_entry_path (ctx->cache_dir, mk_hex);
  size_t mlen = 0;
  unsigned char *man = ccs_read_file (man_path, &mlen);
  free (man_path);

  if (!man
      || mlen < CC_MANIFEST_HEADER_SIZE
      || memcmp (man + CC_MAN_OFF_MAGIC, CC_MANIFEST_MAGIC, CC_MAGIC_LEN) != 0
      || cc_get_u16 (man + CC_MAN_OFF_VERSION) != CC_MANIFEST_VERSION)
    {
      free (man);
      ccs_debug_line (ctx, "manifest-miss", mk_hex, out_path);
      return false;
    }

  uint32_t entry_count = cc_get_u32 (man + CC_MAN_OFF_ENTRY_COUNT);
  uint64_t entries_off = cc_get_u64 (man + CC_MAN_OFF_ENTRIES_OFF);
  if (entries_off > mlen)
    {
      free (man);
      ccs_debug_line (ctx, "manifest-miss", mk_hex, out_path);
      return false;
    }

  /* Walk each entry (candidate header set + its recorded probes).  */
  uint64_t cur = entries_off;
  for (uint32_t ei = 0; ei < entry_count; ei++)
    {
      struct cc_man_entry ent;
      if (!cc_man_entry_parse (man, mlen, cur, &ent))
	break;			/* truncated manifest -> stop */

      /* Only flag-free entries are servable objects (the auto-PCH gch
	 manifest is the sole writer of flagged entries, and it lives under
	 its own path, but stay strict).  */
      if (ent.eflags == 0
	  && cc_man_entry_records_valid (man, mlen, &ent, ctx->verify_hash))
	{
	  char ok_hex[41];
	  cc_hex (ent.ok_raw, ok_hex);
	  if (ccs_serve_from_bin (ctx, ok_hex, out_path, "manifest-hit")
	      /* A requested dependency file is part of the contract: the
		 entry's records are the closure, so write it here.  On a
		 write failure fall through as a miss -- the real compile
		 overwrites the placed .o and emits its own file.  */
	      && (!ctx->deps_path
		  || ccs_write_deps (ctx, man, mlen, &ent, src_path)))
	    {
	      /* Bump the manifest too: it answered this hit, so it is as
		 recently-used as the object it pointed at (B1).  */
	      char *mp = ccs_entry_path (ctx->cache_dir, mk_hex);
	      cc_touch_entry (mp);
	      free (mp);
	      free (man);
	      return true;
	    }
	}

      cur = ent.next_off;
    }

  free (man);
  ccs_debug_line (ctx, "manifest-miss", mk_hex, out_path);
  return false;
}

/* ------------------------------------------------------------------------ */
/* Transparent auto-PCH (driver side)                                       */
/* ------------------------------------------------------------------------ */

/* The driver detects a TU whose leading lines are a "prelude" -- nothing but
   comments, blank lines, and #include <...> directives -- and transparently
   builds + reuses a precompiled header for that prelude, keyed by the prelude
   BYTES + the flag cell + the compiler id, validated on every use against the
   include closure recorded when the .gch was built.  The functions here are
   the pure/probing parts (no spawning, no argv editing -- that lives in
   gcc.cc): the prelude scanner, the entry-key/path derivation, and the probe.
   Everything is driver-linkable: no libcpp, no backend globals.  */

/* Scan SRC/LEN (raw source bytes) for a leading include-only prelude and
   produce the NORMALIZED stub for it.  Accepts, from the top (skipping a
   UTF-8 BOM): horizontal/vertical whitespace, // and (multi-line) block
   comments, and #include <...> logical lines (backslash-newline splices
   honored; anything else after the closing '>' except whitespace/comments
   stops the scan; the '#' must also be the first non-blank byte on its
   physical line).  Stops at the first other construct.

   The normalized stub preserves the LINE STRUCTURE of the prefix -- every
   physical line up to the last accepted include keeps its position -- but
   the bytes of an include logical line are copied VERBATIM while every
   other (comment/blank) line becomes an empty line.  Two TUs whose leading
   comments differ in TEXT but not in LINE COUNT therefore normalize to the
   same stub and share one PCH; and because line numbers agree with the TU,
   diagnostics that point into the stub (include-chain roots, carets on
   include lines) match a plain compile byte-for-byte.  The TU itself is
   still compiled whole, so its real comments are re-lexed and comment
   diagnostics (-Wcomment etc.) fire identically either way.

   On success sets *NORM/*NORM_LEN to an xmalloc'd normalized stub and
   *INCLUDE_COUNT to the number of accepted includes; with no accepted
   include, *NORM is NULL and the count 0.  Returns true (scan is always
   usable).  */

/* Skip a backslash-newline splice at P (also \r\n).  Returns bytes skipped.  */
static size_t
ccp_splice_len (const unsigned char *p, const unsigned char *end)
{
  if (p < end && *p == '\\')
    {
      if (p + 1 < end && p[1] == '\n')
	return 2;
      if (p + 2 < end && p[1] == '\r' && p[2] == '\n')
	return 3;
      if (p + 1 < end && p[1] == '\r')
	return 2;
    }
  return 0;
}

bool
cc_auto_pch_scan_prelude (const unsigned char *src, size_t len,
			  unsigned char **norm, size_t *norm_len,
			  unsigned *include_count)
{
  const unsigned char *p = src;
  const unsigned char *end = src + len;
  const unsigned char *accepted_end = NULL;	/* after last include's NL */
  unsigned count = 0;

  /* Byte ranges of accepted include LOGICAL lines (start of the physical
     line the directive starts on, through the newline that ends the logical
     line), for the normalization pass below.  */
  struct range { size_t lo, hi; };
  range *incs = NULL;
  unsigned nincs = 0, cincs = 0;

  *norm = NULL;
  *norm_len = 0;
  *include_count = 0;

  /* UTF-8 BOM.  */
  if (len >= 3 && p[0] == 0xEF && p[1] == 0xBB && p[2] == 0xBF)
    p += 3;

  const unsigned char *line_start = p;	/* current physical line start */
  bool line_blank_so_far = true;	/* only ws seen on this line */

  while (p < end)
    {
      size_t sp;

      if (*p == '\n' || *p == '\r')
	{
	  p++;
	  if (p <= end && p[-1] == '\r' && p < end && *p == '\n')
	    p++;
	  line_start = p;
	  line_blank_so_far = true;
	  continue;
	}
      if (*p == ' ' || *p == '\t' || *p == '\v' || *p == '\f')
	{
	  p++;
	  continue;
	}
      if ((sp = ccp_splice_len (p, end)) != 0)
	{
	  p += sp;
	  line_start = p;
	  continue;
	}

      /* Comments count as whitespace between constructs, but any comment
	 makes the line no longer blank-prefixed for a following '#'.  */
      if (*p == '/' && p + 1 < end && p[1] == '/')
	{
	  line_blank_so_far = false;
	  p += 2;
	  while (p < end && *p != '\n')
	    {
	      if ((sp = ccp_splice_len (p, end)) != 0)
		p += sp;	/* spliced line continues the // comment */
	      else
		p++;
	    }
	  continue;
	}
      if (*p == '/' && p + 1 < end && p[1] == '*')
	{
	  line_blank_so_far = false;
	  p += 2;
	  while (p + 1 < end && !(*p == '*' && p[1] == '/'))
	    p++;
	  if (p + 1 >= end)
	    break;		/* unterminated comment: stop scan */
	  p += 2;
	  continue;
	}

      /* Directive?  The '#' must be the first non-blank byte on its line so
	 the whole physical line can be copied verbatim into the stub.  */
      if (*p != '#' || !line_blank_so_far)
	break;
      const unsigned char *inc_lo = line_start;
      p++;
      while (p < end && (*p == ' ' || *p == '\t'
			 || (sp = ccp_splice_len (p, end)) != 0))
	p += (*p == ' ' || *p == '\t') ? 1 : sp;
      if ((size_t) (end - p) < 7 || memcmp (p, "include", 7) != 0)
	break;
      p += 7;
      /* Next must not be an identifier char ("include_next" etc).  */
      if (p < end && (ISALNUM (*p) || *p == '_'))
	break;
      while (p < end && (*p == ' ' || *p == '\t'
			 || (sp = ccp_splice_len (p, end)) != 0))
	p += (*p == ' ' || *p == '\t') ? 1 : sp;
      if (p >= end || *p != '<')
	break;			/* v1: angle-bracket includes only */
      p++;
      while (p < end && *p != '>' && *p != '\n')
	{
	  if ((sp = ccp_splice_len (p, end)) != 0)
	    p += sp;
	  else
	    p++;
	}
      if (p >= end || *p != '>')
	break;			/* newline/EOF before '>': malformed */
      p++;

      /* Rest of the logical line: whitespace and comments only.  */
      bool line_ok = true;
      bool at_eol = false;
      while (p < end && !at_eol)
	{
	  if (*p == '\n')
	    {
	      p++;
	      at_eol = true;
	    }
	  else if (*p == '\r')
	    p++;
	  else if (*p == ' ' || *p == '\t' || *p == '\v' || *p == '\f')
	    p++;
	  else if ((sp = ccp_splice_len (p, end)) != 0)
	    p += sp;
	  else if (*p == '/' && p + 1 < end && p[1] == '/')
	    {
	      p += 2;
	      while (p < end && *p != '\n')
		{
		  if ((sp = ccp_splice_len (p, end)) != 0)
		    p += sp;
		  else
		    p++;
		}
	    }
	  else if (*p == '/' && p + 1 < end && p[1] == '*')
	    {
	      p += 2;
	      while (p + 1 < end && !(*p == '*' && p[1] == '/'))
		p++;
	      if (p + 1 >= end)
		{
		  line_ok = false;
		  break;
		}
	      p += 2;
	    }
	  else
	    {
	      line_ok = false;
	      break;
	    }
	}
      if (!line_ok)
	break;

      if (nincs == cincs)
	{
	  cincs = cincs ? cincs * 2 : 16;
	  incs = XRESIZEVEC (range, incs, cincs);
	}
      incs[nincs].lo = (size_t) (inc_lo - src);
      incs[nincs].hi = (size_t) (p - src);
      nincs++;
      count++;
      accepted_end = p;		/* after the include line's newline (or EOF) */
      line_start = p;
      line_blank_so_far = true;
    }

  if (accepted_end && count)
    {
      /* Normalization pass: include ranges verbatim, everything between
	 them line-count-preserving blank lines.  */
      size_t plen = (size_t) (accepted_end - src);
      unsigned char *out = (unsigned char *) xmalloc (plen + 1);
      size_t o = 0;
      size_t pos = 0;
      for (unsigned i = 0; i < nincs; i++)
	{
	  for (size_t b = pos; b < incs[i].lo; b++)
	    if (src[b] == '\n')
	      out[o++] = '\n';
	  memcpy (out + o, src + incs[i].lo, incs[i].hi - incs[i].lo);
	  o += incs[i].hi - incs[i].lo;
	  pos = incs[i].hi;
	}
      out[o] = 0;
      *norm = out;
      *norm_len = o;
      *include_count = count;
    }
  free (incs);
  return true;
}

/* Compute the auto-PCH entry base path for CTX + the prelude bytes:
   "<cache_dir>/pch/<2hex>/<38hex>" (no trailing slash; caller appends
   "/stub.h" etc. and creates directories).  Key = schema tag + key schema
   version + salt + compiler checksum + lang + every output-affecting
   canonicalized option (the SAME classifier as the manifest key, minus -o /
   dump paths by construction) + the prelude bytes.  Deliberately NOT keyed:
   the TU's path or name (preludes are shared across TUs).  Returns a freshly
   xmalloc'd string.  */
char *
cc_auto_pch_entry_base (const cc_serve_ctx *ctx,
			const unsigned char *prelude, size_t plen)
{
  struct sha1_ctx sctx;
  sha1_init_ctx (&sctx);

  ccs_hash_str (&sctx, CC_TAG_LANG, "gcc-auto-pch-v1");
  {
    unsigned char v[4];
    unsigned ver = CC_KEY_SCHEMA_VERSION;
    for (int i = 0; i < 4; i++)
      v[i] = (unsigned char) (ver >> (8 * i));
    ccs_hash_component (&sctx, CC_TAG_VERSION, v, sizeof (v));
  }
  {
    const char *salt = getenv ("GCC_COMPILE_CACHE_SALT");
    if (salt && salt[0])
      ccs_hash_str (&sctx, CC_TAG_SALT, salt);
  }
  ccs_hash_component (&sctx, CC_TAG_CHECKSUM, ctx->checksum, 16);
  ccs_hash_str (&sctx, CC_TAG_LANG, ctx->lang_name);

  for (unsigned i = 1; i < ctx->decoded_count; i++)
    {
      const cl_decoded_option *o = &ctx->decoded[i];
      if (o->opt_index == OPT_SPECIAL_input_file)
	continue;
      if (!cc_option_affects_output_p (o))
	continue;
      for (size_t k = 0; k < o->canonical_option_num_elements; k++)
	ccs_hash_str (&sctx, CC_TAG_OPT, o->canonical_option[k]);
    }

  ccs_hash_component (&sctx, CC_TAG_FILE_BODY, prelude, plen);

  unsigned char raw[20];
  sha1_finish_ctx (&sctx, raw);
  char hex[41];
  cc_hex (raw, hex);

  size_t n = strlen (ctx->cache_dir) + strlen ("/pch/") + 2 + 1 + 38 + 1;
  char *base = (char *) xmalloc (n);
  snprintf (base, n, "%s/pch/%c%c/%s", ctx->cache_dir, hex[0], hex[1],
	    hex + 2);
  return base;
}

/* Probe the entry at BASE.  Returns:
     CC_APCH_USABLE   -- stub + gch + manifest all present and valid: inject.
     CC_APCH_NEGATIVE -- a valid "do not use" marker: compile normally, do
			 not regenerate (its manifest still matched).
     CC_APCH_ABSENT   -- nothing usable (or a stale entry whose closure no
			 longer matches): candidate for (re)generation.
   Validity = the manifest's records still match the filesystem (stat
   shortcut / re-hash, exactly like a warm .o hit) AND the stub bytes equal
   the TU's prelude bytes.  A negative entry with a manifest revalidates the
   same way, so a header edit lifts the negative automatically; a negative
   without a manifest expires after 10 minutes (mtime).  */
enum cc_auto_pch_probe_result
cc_auto_pch_probe (const cc_serve_ctx *ctx, const char *base,
		   const unsigned char *prelude, size_t plen)
{
  size_t bl = strlen (base);
  char *pbuf = (char *) xmalloc (bl + 32);

  /* Manifest first: both positive and negative entries carry one.  */
  snprintf (pbuf, bl + 32, "%s/manifest", base);
  size_t mlen = 0;
  unsigned char *man = ccs_read_file (pbuf, &mlen);
  bool man_ok = false;
  bool man_unverified_probes = false;
  if (man
      && mlen >= CC_MANIFEST_HEADER_SIZE
      && memcmp (man + CC_MAN_OFF_MAGIC, CC_MANIFEST_MAGIC, CC_MAGIC_LEN) == 0
      && cc_get_u16 (man + CC_MAN_OFF_VERSION) == CC_MANIFEST_VERSION
      && cc_get_u32 (man + CC_MAN_OFF_ENTRY_COUNT) == 1)
    {
      struct cc_man_entry ent;
      if (cc_man_entry_parse (man, mlen,
			      cc_get_u64 (man + CC_MAN_OFF_ENTRIES_OFF), &ent)
	  && (ent.eflags & ~CC_MAN_EFLAG_KNOWN_MASK) == 0)
	{
	  man_unverified_probes
	    = (ent.eflags & CC_MAN_EFLAG_UNVERIFIED_PROBES) != 0;
	  man_ok = cc_man_entry_records_valid (man, mlen, &ent,
					       ctx->verify_hash);
	}
    }
  free (man);

  /* Negative marker?  */
  snprintf (pbuf, bl + 32, "%s/negative", base);
  struct stat nst;
  if (stat (pbuf, &nst) == 0)
    {
      if (man_ok)
	{
	  free (pbuf);
	  return CC_APCH_NEGATIVE;	/* still-valid "don't use" */
	}
      /* Closure changed (or no manifest): retry, but rate-limit the
	 no-manifest case to one regeneration attempt per 10 minutes.  */
      if (mlen == 0 && time (NULL) - nst.st_mtime < 600)
	{
	  free (pbuf);
	  return CC_APCH_NEGATIVE;
	}
      free (pbuf);
      return CC_APCH_ABSENT;
    }

  if (!man_ok)
    {
      free (pbuf);
      return CC_APCH_ABSENT;
    }

  /* The gch build evaluated __has_include probes its manifest cannot
     re-verify (quote form / -remap / header maps / _next): such a probe
     flipping would make the .gch stale with no record noticing, so the PCH
     must not be injected.  The entry is otherwise intact, so treat it as a
     standing "do not use" (the TU compiles normally and evaluates its
     probes itself); a header edit still lifts it via the manifest
     mismatch -> ABSENT -> regeneration path above.  */
  if (man_unverified_probes)
    {
      free (pbuf);
      return CC_APCH_NEGATIVE;
    }

  /* Stub must byte-equal the prelude (key preimage check + completeness).  */
  snprintf (pbuf, bl + 32, "%s/stub.h", base);
  size_t slen = 0;
  unsigned char *stub = ccs_read_file (pbuf, &slen);
  bool stub_ok = (stub && slen == plen && memcmp (stub, prelude, plen) == 0);
  free (stub);
  if (!stub_ok)
    {
      free (pbuf);
      return CC_APCH_ABSENT;
    }

  /* The .gch itself (renamed last at gen time = entry-complete marker).  */
  snprintf (pbuf, bl + 32, "%s/stub.h.gch", base);
  struct stat gst;
  bool gch_ok = (stat (pbuf, &gst) == 0 && gst.st_size > 0);
  free (pbuf);
  return gch_ok ? CC_APCH_USABLE : CC_APCH_ABSENT;
}
