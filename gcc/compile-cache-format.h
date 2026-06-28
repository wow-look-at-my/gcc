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
#define CC_FORMAT_VERSION  2u
#define CC_HEADER_SIZE     128u

/* Manifest object (ccache-style "direct mode" index).  */
#define CC_MANIFEST_MAGIC      "CCMANIFS"	/* 8 bytes, no NUL stored */
#define CC_MANIFEST_VERSION    1u
#define CC_MANIFEST_HEADER_SIZE  32u

/* Compiler-id sidecar: a tiny file at "DIR/compiler-id" that cc1plus writes on
   a miss-store so the driver can form the manifest key without linking the
   compiler's checksum object or knowing lang_hooks.name.  Layout: 8-byte magic
   + u16 version + u16 reserved + u8[16] executable_checksum + u32 lang_len +
   lang bytes (no NUL).  */
#define CC_COMPILER_ID_MAGIC    "CCCOMPID"	/* 8 bytes, no NUL stored */
#define CC_COMPILER_ID_VERSION  1u
#define CC_COMPILER_ID_NAME      "compiler-id"

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

/* Per-header record inside a manifest entry: 40 bytes.  */
#define CC_MAN_HDR_REC_SIZE  40u
enum cc_man_hdr_rec_off
{
  CC_MHR_OFF_PATH = 0,		/* u32 -> resolved abs path string */
  CC_MHR_OFF_SIZE = 4,		/* u64  file size                  */
  CC_MHR_OFF_MTIME = 12,	/* u64  st_mtime (seconds)         */
  CC_MHR_OFF_HASH = 20		/* u8[20] raw SHA-1                */
};

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
  CC_TAG_SEARCH_PATH = 12 /* an include-search-path value (manifest key only) */
};

/* Key-schema version (shared by the object key OK and the manifest key MK).  */
#define CC_KEY_SCHEMA_VERSION 4u

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

#endif /* GCC_COMPILE_CACHE_FORMAT_H */
