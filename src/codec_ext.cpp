// FST-X core engine: the page codec switch (moved here from core_util.cpp in 1.0, WS1; WS7 adds codecs).
// MIT licence. No R headers.
#include <algorithm>

#include "core_internal.h"
#include "vendor/lz4/lz4.h"
#include "vendor/zstd/zstd.h"

namespace fstx {

CodecContext::~CodecContext() {
  if (zc) ZSTD_freeCCtx((ZSTD_CCtx*)zc);
  if (zd) ZSTD_freeDCtx((ZSTD_DCtx*)zd);
}

bool codec_supported(int codec) { return codec >= FSTX_CODEC_NONE && codec <= FSTX_CODEC_ZSTD; }

const char* codec_name(int codec) {
  switch (codec) {
    case FSTX_CODEC_NONE: return "none";
    case FSTX_CODEC_LZ4: return "lz4";
    case FSTX_CODEC_ZSTD: return "zstd";
    case kCodecZstdDict: return "zstd with a dictionary";
    case kCodecPcodec: return "Pcodec";
    default: return nullptr;
  }
}

uint64_t codec_feature_bit(int codec) {
  switch (codec) {
    case kCodecZstdDict: return 1ull << kFeatZstdDict;
    case kCodecPcodec: return 1ull << kFeatPcodec;
    default: return codec >= 128 && codec <= 255 ? 1ull << kFeatExperimental : 0;
  }
}

uint64_t codec_feature_bits() { return 0; }  // no codec of this build needs a feature bit

int zstd_level_min() { return ZSTD_minCLevel(); }
int zstd_level_max() { return ZSTD_maxCLevel(); }

size_t compress_bound(int codec, size_t n) {
  switch (codec) {
    case FSTX_CODEC_LZ4: return (size_t)LZ4_compressBound((int)n);
    case FSTX_CODEC_ZSTD: return ZSTD_compressBound(n);
    default: return n;
  }
}

size_t compress_page(CodecContext& ctx, int codec, int level, const void* src, size_t n, void* dst, size_t cap) {
  if (codec == FSTX_CODEC_ZSTD) {
    if (!ctx.zc) {
      ctx.zc = ZSTD_createCCtx();
      if (!ctx.zc) fail(FSTX_ERR_MEMORY, "cannot create a ZSTD context");
    }
    size_t r = ZSTD_compressCCtx((ZSTD_CCtx*)ctx.zc, dst, cap, src, n, level);
    if (ZSTD_isError(r)) fail(FSTX_ERR_INTERNAL, "ZSTD compression failed: %s", ZSTD_getErrorName(r));
    return r;
  }
  if (codec == FSTX_CODEC_LZ4) {
    if (n > (size_t)LZ4_MAX_INPUT_SIZE) fail(FSTX_ERR_INTERNAL, "page too large for LZ4");
    int r = LZ4_compress_default((const char*)src, (char*)dst, (int)n, (int)std::min<size_t>(cap, 0x7fffffff));
    if (r <= 0) fail(FSTX_ERR_INTERNAL, "LZ4 compression failed");
    return (size_t)r;
  }
  fail(FSTX_ERR_INTERNAL, "unknown codec %d", codec);
}

void decompress_page(CodecContext& ctx, int codec, const void* src, size_t comp_len, void* dst, size_t raw_len) {
  if (codec == FSTX_CODEC_ZSTD) {
    if (!ctx.zd) {
      ctx.zd = ZSTD_createDCtx();
      if (!ctx.zd) fail(FSTX_ERR_MEMORY, "cannot create a ZSTD context");
    }
    size_t r = ZSTD_decompressDCtx((ZSTD_DCtx*)ctx.zd, dst, raw_len, src, comp_len);
    if (ZSTD_isError(r)) fail(FSTX_ERR_CORRUPT, "corrupt page: ZSTD error '%s'", ZSTD_getErrorName(r));
    if (r != raw_len) fail(FSTX_ERR_CORRUPT, "corrupt page: ZSTD gave %llu bytes, expected %llu", (unsigned long long)r, (unsigned long long)raw_len);
    return;
  }
  if (codec == FSTX_CODEC_LZ4) {
    if (comp_len > 0x7fffffff || raw_len > 0x7fffffff) fail(FSTX_ERR_CORRUPT, "corrupt page: LZ4 sizes out of range");
    // safe decoder only: never reads past src+comp_len or writes past dst+raw_len
    int r = LZ4_decompress_safe((const char*)src, (char*)dst, (int)comp_len, (int)raw_len);
    if (r < 0 || (size_t)r != raw_len) fail(FSTX_ERR_CORRUPT, "corrupt page: LZ4 decoding failed");
    return;
  }
  fail(FSTX_ERR_CORRUPT, "corrupt page: unknown codec %d", codec);
}

}  // namespace fstx
