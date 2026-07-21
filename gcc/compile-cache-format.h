/* In-compiler compilation cache: on-disk binary format (shared, no deps).

   The byte layout of the cache's object and manifest files, plus the
   little-endian load/store helpers, factored out so BOTH the libcpp-dependent
   store path (compile-cache.cc, linked only into cc1/cc1plus) and the
   driver-usable serve path (compile-cache-serve.cc, linked into the driver too)
   describe the same bytes.  This header pulls in nothing beyond <stdint.h> /
   <string.h> (via system.h, already included by every TU that uses it), so it
   is safe to include from the driver.

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

#ifndef GCC_COMPILE_CACHE_FORMAT_H
#define GCC_COMPILE_CACHE_FORMAT_H

/* ------------------------------------------------------------------------ */
/* Object binary format                                                     */
/* ------------------------------------------------------------------------ */

#define CC_MAGIC      "GCCCACHE"		/* 8 bytes, no NUL stored */
#define CC_MAGIC_LEN  8
/* Version 3: the per-object ".bin" metadata sidecar is gone.  The cache object
   is now the produced ".o" itself, hardlinked (reflink->hardlink->copy) into
   the content-addressed path "DIR/<2hex>/<rest>.o".  The few fields a hit needs
   (format version, flags, back-end warning/error counts, and the recorded
   diagnostics blob) live in an extended attribute "user.gcc_cc.meta" on that
   .o; the small "user.gcc_cc.v" xattr carries the format version on its own for
   a cheap probe.  The old .bin's inputs table and header strings were
   write-only at serve time (the object key is a full-closure SHA-1 content
   address and the manifest keeps its own include set), so they were dropped.
   A minimal .bin sidecar is still written ONLY as a per-entry fallback when the
   filesystem rejects user xattrs (ENOTSUP / E2BIG); see CC_META_* below.  */
#define CC_FORMAT_VERSION  3u
#define CC_HEADER_SIZE     128u

/* Extended-attribute names for the v3 metadata carried on the cache .o.  The
   "user." namespace is the only one a normal (non-root, non-trusted) process
   may set on a regular file.  */
#define CC_XATTR_VERSION  "user.gcc_cc.v"	/* u16 LE format version */
#define CC_XATTR_META     "user.gcc_cc.meta"	/* CC_META_* record + diag blob */

/* Compact metadata record stored in the CC_XATTR_META xattr (and, on the
   xattr-unsupported fallback path, as the body of a ".bin" sidecar).  It holds
   exactly the fields a served hit consumes; the diagnostics blob (if any) is
   appended immediately after the fixed record.  Kept tiny so it fits in a
   single inode xattr (ext4 packs all of an inode's xattrs into ~one 4 KB
   block).  */
#define CC_META_MAGIC      "GCCCMETA"		/* 8 bytes, no NUL stored */
#define CC_META_REC_SIZE   24u
enum cc_meta_off
{
  CC_META_OFF_MAGIC    = 0,	/* char[8]  "GCCCMETA"              */
  CC_META_OFF_VERSION  = 8,	/* u16      format_version           */
  CC_META_OFF_FLAGS    = 10,	/* u16      flags (CC_FLAG_*)        */
  CC_META_OFF_WARNINGS = 12,	/* u32      back-end warning count   */
  CC_META_OFF_ERRORS   = 16,	/* u32      back-end error count     */
  CC_META_OFF_DIAG_LEN = 20	/* u32      diagnostics blob length  */
  /* diagnostics blob (DIAG_LEN bytes) follows at CC_META_REC_SIZE.  */
};

/* Manifest object (ccache-style "direct mode" index).
   Version 2: each entry carries, after its header records, the TU's recorded
   __has_include probes (see CC_MAN_ENT_* / CC_MAN_PROBE_*), so TUs that probe
   (i.e. any real C++ TU, via bits/c++config.h) are servable pre-parse instead
   of being disqualified wholesale; the entry head grew 32 -> 40 bytes to gain
   the probe count and an entry-flags word.
   Version 3: each header record carries the file's FULL stat identity
   (size, mtime sec+nsec, ctime sec+nsec, dev, ino -- see CC_MHR_*) instead of
   size + mtime-seconds only, so the serve-time stat shortcut matches ccache's
   safety bar: a same-second same-size rewrite that restores mtime (touch -d)
   still advances ctime and is caught without hashing the bytes.  The record
   grew 40 -> 80 bytes and gained a flags word (CC_MHR_FLAG_*).
   Version 4: same layout; two new header-record flag bits.  SYSHDR marks
   records excluded from a user-only (-MM/-MMD) dependency list, letting the
   serve tiers synthesize such lists and so serve openssl-style -MMD builds
   instead of declining them.  MAIN_SOURCE marks the TU's own source file:
   the manifest key already commits the CURRENT main source's bytes, so the
   serve side validates that record implicitly by key equality instead of by
   its stored absolute path -- which for cmake try_compile probes points at
   an ephemeral CMakeScratch dir that is deleted after the probe and made
   every probe manifest permanently self-invalidating.  Each version bump
   makes older manifests unreadable (and vice versa) -- the intended clean
   invalidation.  */
#define CC_MANIFEST_MAGIC      "CCMANIFS"	/* 8 bytes, no NUL stored */
#define CC_MANIFEST_VERSION    4u
#define CC_MANIFEST_HEADER_SIZE  32u

/* Compiler-id sidecar: a tiny file the compiler proper writes on a
   miss-store so the driver can form the manifest key without linking the
   compiler's checksum object or knowing lang_hooks.name.  Layout: 8-byte magic
   + u16 version + u16 reserved + u8[16] executable_checksum + u32 lang_len +
   lang bytes (no NUL).

   The sidecar is PER LANGUAGE: "DIR/compiler-id-<prog>" where <prog> is the
   compiler proper's own program name ("cc1", "cc1plus", ...) -- exactly the
   token the driver knows when it assembles the command it is about to spawn.
   cc1 and cc1plus share one cache dir, and the original single
   "DIR/compiler-id" made the LAST-storing language win: the driver then
   computed a wrong MK for every TU of the other language and its no-spawn
   tier never hit (never a wrong serve -- MK folds checksum+lang, so a
   mismatched id can only miss -- but every C TU of a C++-heavy build paid a
   full cc1 exec on every warm build).  The legacy single name is still
   WRITTEN (an older driver sharing the cache dir reads only it; tiny, and
   last-language-wins is the status quo it already had) and still READ as a
   fallback (a cache populated before the split holds only it).  */
#define CC_COMPILER_ID_MAGIC    "CCCOMPID"	/* 8 bytes, no NUL stored */
#define CC_COMPILER_ID_VERSION  1u
#define CC_COMPILER_ID_NAME     "compiler-id"	/* legacy single-file name */
#define CC_COMPILER_ID_PREFIX   "compiler-id-"	/* + <prog>: per-language */

/* Header flag bits (CC_OFF_FLAGS).  */
#define CC_FLAG_HAD_FE_DIAG  0x1u	/* TU emitted front-end diagnostics. */

/* Fixed object-header field byte offsets.  */
enum cc_hdr_off
{
  CC_OFF_MAGIC        = 0,	/* char[8]  "GCCCACHE"            */
  CC_OFF_FORMAT_VER   = 8,	/* u16      format_version         */
  CC_OFF_FLAGS        = 10,	/* u16      flags                  */
  CC_OFF_INPUT_COUNT  = 12,	/* u32      number of inputs       */
  CC_OFF_CREATED      = 16,	/* u64      time(NULL)             */
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

/* Each inputs-table record is 32 bytes.  */
#define CC_INPUT_REC_SIZE  32u
enum cc_input_off
{
  CC_IN_OFF_HASH = 0,		/* u8[20]  raw SHA-1 of the file */
  CC_IN_OFF_SIZE = 20,		/* u64     file size in bytes    */
  CC_IN_OFF_PATH = 28		/* u32 -> path string            */
};

/* Manifest header field byte offsets.  */
enum cc_manifest_hdr_off
{
  CC_MAN_OFF_MAGIC       = 0,	/* char[8]  "CCMANIFS"          */
  CC_MAN_OFF_VERSION     = 8,	/* u16      manifest version     */
  CC_MAN_OFF_FLAGS       = 10,	/* u16      reserved             */
  CC_MAN_OFF_ENTRY_COUNT = 12,	/* u32      number of entries    */
  CC_MAN_OFF_ENTRIES_OFF = 16,	/* u64      entries section off   */
  CC_MAN_OFF_RESERVED    = 24	/* u64      reserved             */
};

/* Fixed head of one manifest entry (v2): 40 bytes, followed by HDR_COUNT
   40-byte header records (CC_MHR_*), then PROBE_COUNT variable-length probe
   records (CC_MPR_*).  */
#define CC_MAN_ENT_HEAD_SIZE  40u
enum cc_man_ent_off
{
  CC_MENT_OFF_OK          = 0,	/* u8[20] object key OK              */
  CC_MENT_OFF_WARNINGS    = 20,	/* u32    stored warning count       */
  CC_MENT_OFF_WERRORS     = 24,	/* u32    stored werror count        */
  CC_MENT_OFF_FLAGS       = 28,	/* u32    CC_MAN_EFLAG_*             */
  CC_MENT_OFF_HDR_COUNT   = 32,	/* u32    number of header records   */
  CC_MENT_OFF_PROBE_COUNT = 36	/* u32    number of probe records    */
};

/* Entry flag bits (CC_MENT_OFF_FLAGS).  UNVERIFIED_PROBES marks an entry
   whose TU evaluated __has_include probes that are NOT covered by its probe
   records (quote-form/-remap/header-map/_next probes the serve side cannot
   re-verify).  The .o manifest store refuses to write such entries at all;
   only the auto-PCH gch manifest writes them, and its consumers must then
   not trust probe stability (the driver answers NEGATIVE -- compile without
   the PCH; the gch-merge distrusts the TU key).  An entry with unknown flag
   bits must be treated as never matching.  */
#define CC_MAN_EFLAG_UNVERIFIED_PROBES  0x1u
#define CC_MAN_EFLAG_KNOWN_MASK         0x1u

/* Per-header record inside a manifest entry: 80 bytes (v3).  SIZE is the
   byte count of the content the HASH was computed over (what cpp read); the
   stat-identity fields describe the on-disk file at store time and are only
   meaningful when CC_MHR_FLAG_HAS_STATID is set (the store side clears it
   when the identity could not be proven to describe the hashed bytes: stat
   failure, a stat size differing from the read size, or a file so recently
   written that a same-stamp rewrite could hide behind it).  Without the
   flag -- or on any identity mismatch -- the serve side falls back to a full
   content re-hash, exactly as before.  */
#define CC_MAN_HDR_REC_SIZE  80u
enum cc_man_hdr_rec_off
{
  CC_MHR_OFF_PATH = 0,		/* u32 -> resolved abs path string */
  CC_MHR_OFF_FLAGS = 4,		/* u32  CC_MHR_FLAG_*              */
  CC_MHR_OFF_SIZE = 8,		/* u64  content size (hashed bytes) */
  CC_MHR_OFF_MTIME = 16,	/* u64  st_mtime (seconds)         */
  CC_MHR_OFF_CTIME = 24,	/* u64  st_ctime (seconds)         */
  CC_MHR_OFF_DEV = 32,		/* u64  st_dev                     */
  CC_MHR_OFF_INO = 40,		/* u64  st_ino                     */
  CC_MHR_OFF_MTIME_NSEC = 48,	/* u32  st_mtim.tv_nsec (0 if N/A) */
  CC_MHR_OFF_CTIME_NSEC = 52,	/* u32  st_ctim.tv_nsec (0 if N/A) */
  CC_MHR_OFF_HASH = 56		/* u8[20] raw SHA-1                */
  /* Bytes 76..79 reserved (written zero).  */
};

/* Header-record flag bits (CC_MHR_OFF_FLAGS).  A record with unknown flag
   bits must be treated as never matching (same policy as entry/probe
   flags).  SYSHDR is the deps-relevant disposition libcpp saw at the file's
   first stacking (CPP_INCLUDED_FILE_SYSP): set means a user-only (-MM/-MMD)
   dependency list excludes the file.  MAIN_SOURCE marks the TU's main
   source record (CPP_INCLUDED_FILE_MAIN): the serve side skips its
   path-based stat/hash re-validation -- the manifest key already commits
   the current main source's bytes, so key equality IS its validation -- and
   dependency synthesis substitutes the serve-time source path for the
   stored one.  */
#define CC_MHR_FLAG_HAS_STATID  0x1u	/* stat-identity fields are trusted */
#define CC_MHR_FLAG_SYSHDR      0x2u	/* excluded from -MM/-MMD deps */
#define CC_MHR_FLAG_MAIN_SOURCE 0x4u	/* the TU's own source file */
#define CC_MHR_FLAG_KNOWN_MASK  0x7u

/* Per-probe record inside a manifest entry: one recorded __has_include
   evaluation.  16-byte fixed part, then N_CANDIDATES u32 string offsets --
   the fully joined paths the store-side search proved ABSENT, in search
   order.  A serve re-verifies the probe with pure stat() logic: every
   candidate must still be absent, and a FOUND probe's resolved path must
   still exist (and not be a directory -- cpp treats those as ENOENT).  Only
   verifiable probes are ever written (CPP_HI_PROBE_VERIFIABLE upstream).  */
#define CC_MAN_PROBE_REC_FIXED_SIZE  16u
enum cc_man_probe_rec_off
{
  CC_MPR_OFF_FLAGS    = 0,	/* u32  CC_MPR_FLAG_*                    */
  CC_MPR_OFF_NAME     = 4,	/* u32 -> operand spelling string        */
  CC_MPR_OFF_RESOLVED = 8,	/* u32 -> resolved path string (FOUND
				   only; 0 -- never a valid string
				   offset -- otherwise)                  */
  CC_MPR_OFF_NCAND    = 12	/* u32  candidate count                  */
  /* N_CANDIDATES u32 string offsets follow at
     CC_MAN_PROBE_REC_FIXED_SIZE.  */
};

/* Probe record flag bits (CC_MPR_OFF_FLAGS).  A record with unknown flag
   bits must be treated as never matching.  */
#define CC_MPR_FLAG_FOUND       0x1u	/* the probe returned 1 */
#define CC_MPR_FLAG_BRACKET     0x2u	/* <...> operand form   */
#define CC_MPR_FLAG_KNOWN_MASK  0x3u

/* Component tags (one byte each), participating in the keys.  */
enum cc_tag
{
  CC_TAG_CHECKSUM = 1,	/* executable_checksum[16] */
  CC_TAG_LANG = 2,	/* lang_hooks.name */
  CC_TAG_STD = 3,	/* reserved */
  CC_TAG_FILE_PATH = 4,	/* one included file's path */
  CC_TAG_FILE_BODY = 5,	/* that file's bytes */
  CC_TAG_OPT = 6,	/* one canonicalized command-line option token */
  CC_TAG_MAIN_INPUT = 7,/* main_input_filename */
  CC_TAG_CWD = 8,	/* current working directory */
  CC_TAG_VERSION = 9,	/* key-schema version */
  CC_TAG_SALT = 10,	/* GCC_COMPILE_CACHE_SALT */
  CC_TAG_SRC_BODY = 11,	/* main source file bytes (manifest key only) */
  CC_TAG_SEARCH_PATH = 12, /* an include-search-path value (manifest key only) */
  CC_TAG_HAS_INCLUDE = 13 /* one __has_include probe's result (object key) */
};

/* Key-schema version (shared by the object key OK and the manifest key MK).
   Bumped 5 -> 6: the object key now commits every evaluated __has_include /
   __has_include_next probe's RESULT (operand spelling + form + found bit,
   under CC_TAG_HAS_INCLUDE; deliberately NOT the resolved path, which would
   break the non-debug key's build-tree independence).  A probe can flip
   absent<->present without changing the include closure (it usually only
   changes a #define), so a closure-only key would keep serving the stale
   object -- and, since a hit stores nothing, would also never refresh the
   manifest that now records probes.  The bump cleanly invalidates
   pre-existing entries.
   Bumped 6 -> 7: the dependency-output options (-MD/-MMD/-MF/-MT/-MQ/-MP/
   -M/-MM/-MG/-Mmodules/-fdeps-*) no longer participate in the keys -- they
   shape only the .d side channel, which every serve regenerates from the
   live command line, and the driver spec derives the cc1-level -MD argument
   from -o, so keying them made the key vary with the output path even
   though the object bytes do not.  The classifier change alters key
   material, so the bump keeps old and new binaries from half-sharing a
   cache.
   Bumped 7 -> 8: prefix-map-aware -g keys (B2).  Under -g the main source
   path, the cwd, and (object key only) every closure path are hashed AFTER
   applying the user's -ffile-prefix-map/-fdebug-prefix-map rewrites
   (replicating gcc/file-prefix-map.cc's semantics; shared helpers in
   compile-cache-serve.cc keep the driver and cc1plus twins identical), and
   map options whose OLD prefix matches the raw source path or cwd are
   excluded from the option walk -- their entire effect on those hashed
   strings is the mapping itself, so two build directories that map
   themselves to one canonical prefix (-ffile-prefix-map=$PWD=.) share keys
   and, since their DWARF is rewritten identically by construction, the
   cached bytes.  Map options matching neither string stay hashed raw
   (conservative: they may rewrite OTHER paths inside DWARF).  With no map
   options the hashed material is byte-identical to v7, but the version
   bump keeps old and new binaries from half-sharing a cache.  */
#define CC_KEY_SCHEMA_VERSION 8u

/* Little-endian store helpers.  */
static inline void
cc_put_u16 (unsigned char *p, uint16_t v)
{
  p[0] = (unsigned char) (v & 0xff);
  p[1] = (unsigned char) ((v >> 8) & 0xff);
}

static inline void
cc_put_u32 (unsigned char *p, uint32_t v)
{
  for (int i = 0; i < 4; i++)
    p[i] = (unsigned char) ((v >> (8 * i)) & 0xff);
}

static inline void
cc_put_u64 (unsigned char *p, uint64_t v)
{
  for (int i = 0; i < 8; i++)
    p[i] = (unsigned char) ((v >> (8 * i)) & 0xff);
}

/* Little-endian load helpers.  */
static inline uint16_t
cc_get_u16 (const unsigned char *p)
{
  return (uint16_t) (p[0] | ((uint16_t) p[1] << 8));
}

static inline uint32_t
cc_get_u32 (const unsigned char *p)
{
  uint32_t v = 0;
  for (int i = 0; i < 4; i++)
    v |= (uint32_t) p[i] << (8 * i);
  return v;
}

static inline uint64_t
cc_get_u64 (const unsigned char *p)
{
  uint64_t v = 0;
  for (int i = 0; i < 8; i++)
    v |= (uint64_t) p[i] << (8 * i);
  return v;
}

/* Render 20 raw SHA-1 bytes into OUT[41] as lowercase hex + NUL.  */
static inline void
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
/* Fast 128-bit content fingerprint (for the MANIFEST key's bulk source body) */
/* ------------------------------------------------------------------------ */

/* The manifest key MK must hash the entire (possibly multi-megabyte,
   preprocessed) source body, and it is computed BOTH by cc1plus (store +
   pre-parse serve) and by the driver (no-spawn serve) -- so a warm hit's wall
   time is dominated by hashing that body, and SHA-1 (~0.5 GB/s here) is the
   bottleneck.  MK is only a LOOKUP INDEX: a collision causes a manifest miss
   (fall through to a real compile), never a wrong answer -- the object key OK
   is still a full SHA-1 content address and every recorded header is verified
   on a hit.  So the body may be fingerprinted with a fast non-cryptographic
   hash and that fingerprint folded into MK's SHA-1, keeping MK 160-bit and
   collision-safe while hashing the bulk bytes several times faster.

   cc_fast128 is a single-pass 128-bit hash (two independent xxHash-style
   64-bit lanes, avalanche-finalized).  It MUST be byte-for-byte identical on
   the driver and in cc1plus -- it lives here, static inline, so both compile
   the same code.  Do not "optimize" one copy.  */

static inline uint64_t
cc_f128_rotl (uint64_t x, int r)
{
  return (x << r) | (x >> (64 - r));
}

static inline uint64_t
cc_f128_read64 (const unsigned char *p)
{
  /* memcpy compiles to a single unaligned 64-bit load; the byte-order is
     native, which is fine because cc_fast128 is only ever compared against
     itself (and MK already folds in the compiler checksum, so a cache is never
     shared across architectures).  Both the driver and cc1plus compile this
     identical inline, so they agree on the same machine.  */
  uint64_t v;
  memcpy (&v, p, sizeof (v));
  return v;
}

static inline uint64_t
cc_f128_mix (uint64_t h, uint64_t k)
{
  const uint64_t P1 = 0x9E3779B185EBCA87ULL;
  const uint64_t P2 = 0xC2B2AE3D27D4EB4FULL;
  k *= P2;
  k = cc_f128_rotl (k, 31);
  k *= P1;
  h ^= k;
  h = cc_f128_rotl (h, 27);
  h = h * 5 + 0x52DCE729ULL;
  return h;
}

static inline uint64_t
cc_f128_avalanche (uint64_t h)
{
  h ^= h >> 33;
  h *= 0xFF51AFD7ED558CCDULL;
  h ^= h >> 33;
  h *= 0xC4CEB9FE1A85EC53ULL;
  h ^= h >> 33;
  return h;
}

/* Fingerprint LEN bytes at DATA into OUT[16] (little-endian: lane0 then
   lane1).  */
static inline void
cc_fast128 (const void *data, size_t len, unsigned char out[16])
{
  const unsigned char *p = (const unsigned char *) data;
  const unsigned char *end = p + len;
  uint64_t h1 = 0x9E3779B185EBCA87ULL ^ (uint64_t) len;
  uint64_t h2 = 0xC2B2AE3D27D4EB4FULL + (uint64_t) len;

  while (end - p >= 16)
    {
      h1 = cc_f128_mix (h1, cc_f128_read64 (p));
      h2 = cc_f128_mix (h2, cc_f128_read64 (p + 8));
      p += 16;
    }
  if (end - p >= 8)
    {
      h1 = cc_f128_mix (h1, cc_f128_read64 (p));
      p += 8;
    }
  /* Tail (< 8 bytes): pack remaining bytes into a word, fold into h2.  */
  {
    uint64_t t = 0;
    int shift = 0;
    while (p < end)
      {
	t |= (uint64_t) *p++ << shift;
	shift += 8;
      }
    h2 = cc_f128_mix (h2, t);
  }

  h1 += h2;
  h2 += h1;
  h1 = cc_f128_avalanche (h1);
  h2 = cc_f128_avalanche (h2);

  for (int i = 0; i < 8; i++)
    out[i] = (unsigned char) (h1 >> (8 * i));
  for (int i = 0; i < 8; i++)
    out[8 + i] = (unsigned char) (h2 >> (8 * i));
}

#endif /* GCC_COMPILE_CACHE_FORMAT_H */
