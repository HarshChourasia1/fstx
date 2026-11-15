// FST-X encodings (file format 2). Clean-room code written from the published ideas (FastLanes-style
// 1,024-value vectors, frame-of-reference, delta, run-length, dictionary, ALP, ALP-RD, byte-split); FSST is
// the vendored cwida library. Plain loops only, no SIMD intrinsics. MIT licence. No R headers.
//
// Page body layout (what the codec compresses; every part is 8-byte aligned):
//   header   8 bytes: kind u8 | NA mode u8 | 0 u16 | row count u32
//   NA block (only when NA mode != 0)
//   value block
// Block: 8-byte header [tag u8, param u8 x3, payload length u32], then the payload padded to 8 bytes.
// Blocks nest: RLE, DICT, ALP, ALP-RD, FSST and plain strings carry child blocks inside their payload.
#ifndef FSTX_ENC_H
#define FSTX_ENC_H

#include <memory>
#include <vector>

#include "core_internal.h"

namespace fstx {
namespace enc {

// encoding tags: one byte in every block header and in a page's directory chain
enum : uint8_t {
  T_PLAIN = 1,
  T_CONST = 2,
  T_FOR = 3,     // frame-of-reference + bit-packing, per 1,024-value vector
  T_DELTA = 4,   // deltas inside each vector, then FOR + bit-packing
  T_RLE = 5,     // run values + run lengths (child chains)
  T_DICT = 6,    // dictionary values + codes (child chains)
  T_SPLIT = 7,   // byte-split with the type's own stride (4 or 8), meant for a codec on top
  T_ALP = 8,     // decimal doubles as integers (child int64 chain) + exceptions
  T_ALPRD = 9,   // real doubles: left bits through an 8-entry dictionary, right bits through an int64 chain
  T_FSST = 10,   // strings: FSST symbol table + compressed bytes + compressed lengths
  T_NA_BITMAP = 0x81,
  T_NA_RUNS = 0x82
};
constexpr int kMaxTag = 10;

// page kinds (low nibble of chain byte 0, and byte 0 of the page body)
enum : uint8_t { K_INT = 1, K_DBL = 2, K_LGL = 3, K_STR = 4, K_CODES = 5, K_DICT = 6 };
enum : uint8_t { NA_NONE = 0, NA_BITMAP = 1, NA_RUNS = 2 };
enum { P_FAST = 0, P_BALANCED = 1, P_SMALL = 2 };

const char* tag_name(int tag);
const char* kind_name(int kind);

// chain (Page.chain in the footer): byte 0 = kind | NA mode << 4, byte 1 = top encoding,
// bytes 2 and 3 = top encodings of the first and second child blocks (0 when none)
inline uint32_t make_chain(int kind, int na, uint32_t tags3) { return (uint32_t)(kind | (na << 4)) | (tags3 << 8); }
inline int chain_kind(uint32_t c) { return (int)(c & 15u); }
inline int chain_na(uint32_t c) { return (int)((c >> 4) & 15u); }
inline int chain_tag(uint32_t c, int i) { return (int)((c >> (8 * (i + 1))) & 255u); }

struct Buf {
  std::vector<uint8_t> v;
  size_t size() const { return v.size(); }
  void clear() { v.clear(); }
  uint8_t* grow(size_t n) {
    size_t s = v.size();
    v.resize(s + n);
    return v.data() + s;
  }
  void put(const void* p, size_t n) {
    if (n) std::memcpy(grow(n), p, n);
  }
  void pad8() {
    size_t r = v.size() & 7u;
    if (r) grow(8 - r);
  }
  size_t open(uint8_t tag, uint8_t a = 0, uint8_t b = 0, uint8_t c = 0) {
    pad8();
    size_t h = v.size();
    uint8_t* p = grow(8);
    p[0] = tag;
    p[1] = a;
    p[2] = b;
    p[3] = c;
    return h;
  }
  void close(size_t h) {
    uint64_t len = v.size() - h - 8;
    if (len > 0xffffffffull) fail(FSTX_ERR_INTERNAL, "encoded block over 4 GB");
    put_u32(v.data() + h + 4, (uint32_t)len);
    pad8();
  }
};

// ---- reading blocks (bounds-checked; every failure is FSTX_ERR_CORRUPT)
[[noreturn]] void corrupt(const char* what);
inline size_t a8(size_t x) { return (x + 7) & ~(size_t)7; }

struct Blk {
  uint8_t tag, a, b, c;
  const uint8_t* p;
  size_t len;
};

struct Rd {  // a run of blocks
  const uint8_t* p;
  const uint8_t* e;
  bool done() const { return p >= e; }
  int peek() const {
    if (e - p < 8) corrupt("block header");
    return p[0];
  }
  Blk next() {
    if (e - p < 8) corrupt("block header");
    Blk k;
    k.tag = p[0];
    k.a = p[1];
    k.b = p[2];
    k.c = p[3];
    k.len = get_u32(p + 4);
    k.p = p + 8;
    if ((size_t)(e - k.p) < a8(k.len)) corrupt("block length");
    p = k.p + a8(k.len);
    return k;
  }
};

struct Cur {  // inside one block's payload
  const uint8_t* base;
  const uint8_t* p;
  const uint8_t* e;
  explicit Cur(const Blk& b) : base(b.p), p(b.p), e(b.p + b.len) {}
  const uint8_t* take(size_t n) {
    if ((size_t)(e - p) < n) corrupt("payload too short");
    const uint8_t* r = p;
    p += n;
    return r;
  }
  void pad() {
    size_t off = (size_t)(p - base) & 7u;
    if (off) p = ((size_t)(e - p) < 8 - off) ? e : p + (8 - off);
  }
  Rd rest() const { return Rd{p, e}; }
};

// Settings for encoding one page.
struct EncCtx {
  int profile = P_BALANCED;
  int force = 0;                 // top-level encoding forced by the host (tests); 0 = choose
  int codec = FSTX_CODEC_ZSTD;   // codec the page will get afterwards (NONE: never try SPLIT)
  CodecContext* cc = nullptr;    // for the ZSTD -1 estimates
};

// Fixed-width pages. The return value is the page's chain. x is read in place, never changed.
// na_value marks missing values (NA_integer_ for INT/LGL, -1 for string codes).
uint32_t encode_int_page(const int32_t* x, uint32_t n, int kind, int32_t na_value, const EncCtx& cx, Buf& out);
uint32_t encode_dbl_page(const double* x, uint32_t n, const EncCtx& cx, Buf& out);
// Decode a whole page into out (n values). Checks the body against chain and n; throws FSTX_ERR_CORRUPT.
void decode_int_page(const uint8_t* body, size_t len, uint32_t chain, uint32_t n, int kind, int32_t na_value,
                     int32_t* out);
void decode_dbl_page(const uint8_t* body, size_t len, uint32_t chain, uint32_t n, double* out);
// Partial decode (M5): vm holds one byte per 1,024-value vector of the page (nonzero = needed); only those
// vectors of out are written. Encodings without vector access decode the whole page. vm = NULL: whole page.
void decode_int_page_part(const uint8_t* body, size_t len, uint32_t chain, uint32_t n, int kind, int32_t na_value,
                          const uint8_t* vm, int32_t* out);
void decode_dbl_page_part(const uint8_t* body, size_t len, uint32_t chain, uint32_t n, const uint8_t* vm,
                          double* out);

// ---- shared by enc_num.cpp and enc_str.cpp
// int32 child chain (chooses its own encoding); returns its tags (top tag in the low byte)
uint32_t encode_int32_auto(const int32_t* x, uint32_t n, int depth, const EncCtx& cx, Buf& out);
int decode_int32_blk(Rd& rd, int32_t* out, uint32_t n, int depth);
// Writes the NA block when x holds na_value; returns the NA mode.
int na_write_i32(const int32_t* x, uint32_t n, int32_t na_value, Buf& out);
// Sets out[i] = na for rows marked in the NA block. If must_be is not null, those rows must hold *must_be.
void na_apply_i32(const Blk& b, int mode, uint32_t n, int32_t* out, int32_t na, const int32_t* must_be);
// Size of buf after ZSTD -1 (the chooser's codec estimate)
size_t zstd_est(const EncCtx& cx, const uint8_t* p, size_t n);
void check_aligned(const uint8_t* body);

// ---- strings
struct FsstTable {
  void* enc = nullptr;           // fsst_encoder_t*, shared read-only by workers (scalar path)
  std::vector<uint8_t> blob;     // exported symbol table, copied into every page
  ~FsstTable();
};

// Plan for one column chunk (one string column in one row group), made before its pages are encoded.
struct StrChunk {
  int mode = T_PLAIN;                    // T_PLAIN, T_FSST or T_DICT
  std::vector<int32_t> codes;            // T_DICT: one per row, -1 = NA
  std::vector<const char*> dptr;         // T_DICT: dictionary entries, first-appearance order
  std::vector<int32_t> dlen;
  std::unique_ptr<FsstTable> fsst;       // T_FSST
  std::vector<uint64_t> vsizes;          // per 1,024-row vector: 4 x rows + bytes (page planning)
};

// Reads rows [r0, r1) through the host callback, picks dictionary / FSST / plain, builds what the pages
// need. force: 0, T_DICT, T_FSST or T_PLAIN. Returns the host's error code (0 = fine).
int plan_str_chunk(const fstx_column_data& d, uint64_t r0, uint64_t r1, const EncCtx& cx, int force, StrChunk& sc);

// kind K_STR (may hold NA) or K_DICT (dictionary page, no NA). mode T_PLAIN or T_FSST.
uint32_t encode_str_page(int kind, const char* const* ptrs, const int32_t* lens, uint32_t n, int mode,
                         const FsstTable* fsst, const EncCtx& cx, Buf& out);
// Decodes a K_STR / K_DICT page: lens gets n entries (-1 = NA), bytes points at the strings, one after
// another (into the body itself for plain pages, into buf for FSST pages).
void decode_str_page(const uint8_t* body, size_t len, uint32_t chain, uint32_t n, int kind,
                     std::vector<int32_t>& lens, std::vector<uint8_t>& buf, const char** bytes);

}  // namespace enc
}  // namespace fstx

#endif
