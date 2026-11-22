// Minimal miniz replacement for the FidelityFX SDK's blob decompressor.
//
// The SDK's ffx_blob_decompress.h includes <miniz.h> and calls mz_uncompress(). The SDK build normally vendors
// miniz; the published source tree does not. zlib is ubiquitous, so this header maps the one function used onto
// zlib's uncompress().
#pragma once

#include <zlib.h>

typedef unsigned char mz_uint8;
typedef unsigned long mz_ulong;

#define MZ_OK 0
#define MZ_STREAM_END 1
#define MZ_ERRNO (-1)
#define MZ_STREAM_ERROR (-2)
#define MZ_DATA_ERROR (-3)
#define MZ_MEM_ERROR (-4)
#define MZ_BUF_ERROR (-5)
#define MZ_VERSION_ERROR (-6)

static inline int mz_uncompress(unsigned char* dest, mz_ulong* destLen, const unsigned char* source, mz_ulong sourceLen)
{
    uLongf length = (uLongf)*destLen;
    int result = uncompress((Bytef*)dest, &length, (const Bytef*)source, (uLong)sourceLen);
    *destLen = (mz_ulong)length;
    switch (result)
    {
    case Z_OK: return MZ_OK;
    case Z_BUF_ERROR: return MZ_BUF_ERROR;
    case Z_MEM_ERROR: return MZ_MEM_ERROR;
    case Z_DATA_ERROR: return MZ_DATA_ERROR;
    default: return MZ_STREAM_ERROR;
    }
}
