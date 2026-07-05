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

/* ---- Stat identity (manifest v3 stat shortcut) ---- */

/* The full stat identity of one recorded file, as compared by the serve-time
   stat shortcut (ccache's inode-cache bar: size + mtime sec/nsec + ctime
   sec/nsec + dev + ino all matching lets a hit skip re-hashing the bytes; any
   difference falls back to the content re-hash).  Extracted by
   cc_statid_from_stat below, which the STORE side (compile-cache.cc) and the
   SERVE side (compile-cache-serve.cc) both inline -- one definition, so both
   agree byte-for-byte on every platform, including the nsec availability
   #if.  */
struct cc_statid
{
  uint64_t size;
  uint64_t mtime_s;
  uint64_t ctime_s;
  uint64_t dev;
  uint64_t ino;
  uint32_t mtime_ns;
  uint32_t ctime_ns;
};

/* Fill *ID from *ST.  struct stat is visible here via system.h (every
   includer pulls it in first).  Hosts without st_mtim/st_ctim nanosecond
   fields record 0 on both sides, which compares consistently.  */
static inline void
cc_statid_from_stat (const struct stat *st, struct cc_statid *id)
{
  id->size = (uint64_t) st->st_size;
  id->mtime_s = (uint64_t) st->st_mtime;
  id->ctime_s = (uint64_t) st->st_ctime;
  id->dev = (uint64_t) st->st_dev;
  id->ino = (uint64_t) st->st_ino;
#if defined (__linux__) || defined (__GLIBC__) || defined (__FreeBSD__) \
    || defined (__NetBSD__) || defined (__OpenBSD__) || defined (__sun)
  id->mtime_ns = (uint32_t) st->st_mtim.tv_nsec;
  id->ctime_ns = (uint32_t) st->st_ctim.tv_nsec;
#elif defined (__APPLE__)
  id->mtime_ns = (uint32_t) st->st_mtimespec.tv_nsec;
  id->ctime_ns = (uint32_t) st->st_ctimespec.tv_nsec;
#else
  id->mtime_ns = 0;
  id->ctime_ns = 0;
#endif
}

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

  /* GCC_COMPILE_CACHE_VERIFY=hash (alias GCC_COMPILE_CACHE_PARANOID=1) forces
     a full per-header content re-hash on a hit instead of the stat-identity
     shortcut; see cc_verify_hash_env_p ().  */
  bool verify_hash;

  /* GCC_COMPILE_CACHE_DEBUG: emit "compile-cache: <action> <key12> <out>".  */
  bool debug;

  /* Driver-tier dependency-file synthesis.  When DEPS_PATH is non-NULL, a
     served manifest hit must also write a make-style dependency file there
     honoring the -MD contract (every file of the include closure, system
     headers included) -- the recorded header set IS that closure, so the
     driver can emit the .d without running the preprocessor.  The driver
     only requests this for the forms it can reproduce exactly (-MD with an
     explicit dependency file and at least one -MT/-MQ target, optional
     -MP); for anything else it declines the serve instead and cc1plus's
     tier takes over.  All three pointers are borrowed.  A hit that fails to
     write the file reports a miss (the object may already be placed; the
     ensuing real compile simply overwrites it and writes its own .d).  */
  const char *deps_path;		/* dependency file (-MF / -MD arg) */
  const char *const *deps_targets;	/* target names, command-line order */
  const bool *deps_target_quoted;	/* per-target: munge like -MQ */
  unsigned deps_target_count;
  bool deps_phony;			/* -MP: phony target per header */
};

/* ---- Shared manifest-entry access (one interpreter for the v2 bytes) ---- */

/* Parsed byte locations of one manifest entry (v3 layout: 40-byte head,
   HDR_COUNT 80-byte header records, PROBE_COUNT variable-length probe
   records; see compile-cache-format.h).  Offsets are absolute within the
   manifest buffer.  */
struct cc_man_entry
{
  const unsigned char *ok_raw;	/* the entry's 20-byte object key */
  uint32_t warnings;
  uint32_t werrors;
  uint32_t eflags;		/* CC_MAN_EFLAG_* */
  uint32_t hdr_count;
  uint32_t probe_count;
  uint64_t hdr_recs_off;
  uint64_t probe_recs_off;
  uint64_t next_off;		/* one past this entry's last record */
};

/* Parse + bounds-check the entry at OFF in MAN/MLEN into *OUT (walking its
   variable-length probe records to find NEXT_OFF).  Returns false on any
   truncation / malformation, after which the caller must stop walking the
   manifest.  */
extern bool cc_man_entry_parse (const unsigned char *man, size_t mlen,
				uint64_t off, struct cc_man_entry *out);

/* Fetch a length-prefixed string from the manifest string area with bounds
   checks; NULL if OFF is out of range.  */
extern const char *cc_man_string (const unsigned char *man, size_t mlen,
				  uint32_t off);

/* True when the environment requests the airtight serve mode
   (GCC_COMPILE_CACHE_VERIFY=hash, or its alias GCC_COMPILE_CACHE_PARANOID
   set non-empty and not "0"): every header content re-hashes on a hit
   instead of the stat-identity shortcut.  One definition (in the serve
   unit) so the driver and cc1plus honor identical spellings.  */
extern bool cc_verify_hash_env_p (void);

/* Re-validate ENT against the filesystem: every header record must still
   resolve (full stat-identity shortcut, else content re-hash; VERIFY_HASH
   forces the re-hash) and every probe record must still reproduce (each
   candidate path still absent; a FOUND probe's resolved path still present
   and not a directory).  Entries with unknown flag bits never validate.
   Shared by the driver serve, the cc1plus pre-parse serve, and the auto-PCH
   probe, so all three accept exactly the same states.  */
extern bool cc_man_entry_records_valid (const unsigned char *man, size_t mlen,
					const struct cc_man_entry *ent,
					bool verify_hash);

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

/* ---- Transparent auto-PCH (driver side; see the section in the .cc) ---- */

/* Scan raw source bytes for a leading include-only prelude and produce the
   NORMALIZED stub for it: include logical lines verbatim, all other lines
   blanked, line positions preserved (see the .cc).  Sets *NORM (xmalloc'd;
   NULL when no include was accepted), *NORM_LEN, and *INCLUDE_COUNT.  */
extern bool cc_auto_pch_scan_prelude (const unsigned char *src, size_t len,
				      unsigned char **norm, size_t *norm_len,
				      unsigned *include_count);

/* Derive the cache entry base path "<dir>/pch/<2hex>/<38hex>" for this
   prelude under CTX's flag cell + compiler id.  xmalloc'd.  */
extern char *cc_auto_pch_entry_base (const cc_serve_ctx *ctx,
				     const unsigned char *prelude,
				     size_t plen);

/* Probe result for cc_auto_pch_probe.  */
enum cc_auto_pch_probe_result
{
  CC_APCH_USABLE,	/* entry valid: inject -include <base>/stub.h */
  CC_APCH_ABSENT,	/* no (valid) entry: candidate for generation */
  CC_APCH_NEGATIVE	/* valid do-not-use marker: compile normally */
};

extern enum cc_auto_pch_probe_result
cc_auto_pch_probe (const cc_serve_ctx *ctx, const char *base,
		   const unsigned char *prelude, size_t plen);

#endif /* GCC_COMPILE_CACHE_SERVE_H */
