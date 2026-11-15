// FST-X core engine: the page codec switch (moved here from core_util.cpp in 1.0, WS1). A new codec is added
// in this file and codec_ext.cpp only: its number, name, the feature bit its files need, and the three calls
// below. MIT licence. No R headers.
#ifndef FSTX_CODEC_EXT_H
#define FSTX_CODEC_EXT_H

#include <cstddef>
#include <cstdint>

namespace fstx {

// Codec numbers (Page.codec in the footer). 0-2 are read and written. 3 = zstd with a dictionary and
// 4 = Pcodec are reserved, 5-127 are kept for later codecs, 128-255 for experiments that a release never writes.
// A reader refuses any codec it does not have.
constexpr int kCodecZstdDict = 3;
constexpr int kCodecPcodec = 4;
constexpr int kCodecExperimental = 128;

// One per worker, reused across pages (thread-local contexts).
struct CodecContext {
  void* zc = nullptr;  // ZSTD_CCtx*
  void* zd = nullptr;  // ZSTD_DCtx*
  ~CodecContext();
};

bool codec_supported(int codec);  // this build reads and writes it
// "none", "lz4", "zstd", the reserved names, or nullptr for a number without a name
const char* codec_name(int codec);
// Required-feature bit a file needs when one of its pages uses this codec (0 = none), and all such bits this
// build's codecs can read.
uint64_t codec_feature_bit(int codec);
uint64_t codec_feature_bits();

size_t compress_bound(int codec, size_t n);
// Returns the compressed size; throws on codec failure.
size_t compress_page(CodecContext& ctx, int codec, int level, const void* src, size_t n, void* dst, size_t cap);
// Decompresses exactly raw_len bytes; throws FSTX_ERR_CORRUPT on any decoder error or size mismatch.
void decompress_page(CodecContext& ctx, int codec, const void* src, size_t comp_len, void* dst, size_t raw_len);
int zstd_level_min();
int zstd_level_max();

}  // namespace fstx

#endif
