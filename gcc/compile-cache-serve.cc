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

  /* Under -g the source path + cwd bake into DWARF.  */
  if (ctx->paths_affect_output)
    {
      ccs_hash_str (&ctx_sha, CC_TAG_MAIN_INPUT, src_path);
      ccs_hash_str (&ctx_sha, CC_TAG_CWD, ctx->cwd ? ctx->cwd : "");
    }

  /* (4) Output-affecting options + (anti-shadow) search-path VALUES, in
     command-line order.  decoded[0] is the program-name slot; skip it.  */
  for (unsigned i = 1; i < ctx->decoded_count; i++)
    {
      const cl_decoded_option *o = &ctx->decoded[i];
      bool affects = cc_option_affects_output_p (o);
      bool search = cc_option_is_search_path_p (o);
      if (!affects && !search)
	continue;
      for (size_t k = 0; k < o->canonical_option_num_elements; k++)
	ccs_hash_str (&ctx_sha, search ? CC_TAG_SEARCH_PATH : CC_TAG_OPT,
		      o->canonical_option[k]);
    }

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

  /* Walk each entry (candidate header set).  */
  uint64_t cur = entries_off;
  for (uint32_t ei = 0; ei < entry_count; ei++)
    {
      if (cur + 20 + 4 + 4 + 4 > mlen)
	break;
      const unsigned char *ent = man + cur;
      unsigned char ok_raw[20];
      memcpy (ok_raw, ent + 0, 20);
      uint32_t hdr_count = cc_get_u32 (ent + 28);
      uint64_t recs_off = cur + 32;
      uint64_t recs_len = (uint64_t) hdr_count * CC_MAN_HDR_REC_SIZE;
      if (recs_off + recs_len > mlen)
	break;

      bool all_match = true;
      for (uint32_t hi = 0; hi < hdr_count && all_match; hi++)
	{
	  const unsigned char *rec = man + recs_off
				     + (uint64_t) hi * CC_MAN_HDR_REC_SIZE;
	  uint32_t path_off = cc_get_u32 (rec + CC_MHR_OFF_PATH);
	  uint64_t want_size = cc_get_u64 (rec + CC_MHR_OFF_SIZE);
	  uint64_t want_mtime = cc_get_u64 (rec + CC_MHR_OFF_MTIME);
	  const unsigned char *want_hash = rec + CC_MHR_OFF_HASH;

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
	      all_match = false;
	      break;
	    }

	  if (!ctx->verify_hash
	      && (uint64_t) stt.st_size == want_size
	      && (uint64_t) stt.st_mtime == want_mtime)
	    continue;

	  size_t got_len = 0;
	  unsigned char *body = ccs_read_file (hpath, &got_len);
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
	  char ok_hex[41];
	  cc_hex (ok_raw, ok_hex);
	  if (ccs_serve_from_bin (ctx, ok_hex, out_path, "manifest-hit"))
	    {
	      free (man);
	      return true;
	    }
	}

      cur = recs_off + recs_len;
    }

  free (man);
  ccs_debug_line (ctx, "manifest-miss", mk_hex, out_path);
  return false;
}
