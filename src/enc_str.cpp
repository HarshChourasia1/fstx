// FST-X string encodings (file format 2): row-group dictionaries (hashing the host's string pointers),
// FSST pages (vendored cwida/fsst, one symbol table per column chunk, stored in every page) and plain pages
// (lengths through the integer chain, then the bytes). MIT licence. No R headers.
#include <algorithm>
#include <limits>

#include "enc.h"
#include "fsst.h"

namespace fstx {
namespace enc {

FsstTable::~FsstTable() {
  if (enc) fsst_destroy((fsst_encoder_t*)enc);
}

namespace {

constexpr uint32_t kV = 1024;
constexpr uint32_t kStrSampleVectors = 8;
constexpr uint64_t kFsstMaxPageBytes = 1ull << 26;  // bigger pages stay plain (FSST lengths are int32)

inline int bits_of(uint64_t r) { return r ? 64 - __builtin_clzll(r) : 0; }

// Open-addressing table from (pointer, length) to dictionary code. The host gives the same pointer for
// equal strings (R's global string cache); a host that does not only gets a bigger dictionary.
struct PtrDict {
  std::vector<int32_t> slot;
  uint32_t mask = 0;
  int lg = 0;
  std::vector<const char*>* ptr;
  std::vector<int32_t>* len;
  void init(std::vector<const char*>* p, std::vector<int32_t>* l) {
    ptr = p;
    len = l;
    lg = 12;
    slot.assign(1u << lg, -1);
    mask = (1u << lg) - 1;
  }
  uint32_t hash(const char* p, int32_t l) const {
    return (uint32_t)((((uint64_t)(uintptr_t)p >> 3) ^ ((uint64_t)(uint32_t)l << 40)) * 0x9E3779B97F4A7C15ULL >> (64 - lg));
  }
  void grow() {
    ++lg;
    slot.assign(1u << lg, -1);
    mask = (1u << lg) - 1;
    for (size_t k = 0; k < ptr->size(); ++k) {
      uint32_t h = hash((*ptr)[k], (*len)[k]);
      while (slot[h] >= 0) h = (h + 1) & mask;
      slot[h] = (int32_t)k;
    }
  }
  int32_t code(const char* p, int32_t l, uint64_t& bytes) {
    uint32_t h = hash(p, l);
    for (;;) {
      const int32_t s = slot[h];
      if (s < 0) break;
      if ((*ptr)[(size_t)s] == p && (*len)[(size_t)s] == l) return s;
      h = (h + 1) & mask;
    }
    const int32_t c = (int32_t)ptr->size();
    slot[h] = c;
    ptr->push_back(p);
    len->push_back(l);
    bytes += (uint64_t)l;
    if (ptr->size() * 2 > slot.size()) grow();
    return c;
  }
};

void check_str_header(const uint8_t* body, size_t len, uint32_t chain, uint32_t n, int kind) {
  check_aligned(body);
  if (len < 8 || (len & 7u)) corrupt("page size");
  if (body[0] != kind || chain_kind(chain) != kind) corrupt("page kind does not match the column");
  if (body[1] != chain_na(chain) || body[1] > NA_RUNS || (kind == K_DICT && body[1] != NA_NONE)) corrupt("NA mode");
  if (get_u32(body + 4) != n) corrupt("row count");
}

// FSST symbol tables are checked before fsst_import touches them.
bool fsst_table_ok(const uint8_t* p, size_t len) {
  if (len < 17) return false;
  if (p[8] & 1) return false;  // zero-terminated tables are never written
  uint32_t total = 0;
  size_t need = 17;
  for (int l = 1; l <= 8; ++l) {
    const uint32_t c = p[9 + (l & 7)];
    total += c;
    need += (size_t)c * (size_t)((l & 7) + 1);
  }
  return total <= 255 && need <= len;
}

}  // namespace

int plan_str_chunk(const fstx_column_data& d, uint64_t r0, uint64_t r1, const EncCtx& cx, int force, StrChunk& sc) {
  const uint64_t n = r1 - r0;
  const uint64_t nv = (n + kV - 1) / kV;
  sc.mode = T_PLAIN;
  sc.codes.clear();
  sc.dptr.clear();
  sc.dlen.clear();
  sc.fsst.reset();
  sc.vsizes.assign(nv, 0);

  bool dict = force == 0 || force == T_DICT;
  PtrDict pd;
  uint64_t dict_bytes = 0;
  if (dict) {
    sc.codes.resize(n);
    pd.init(&sc.dptr, &sc.dlen);
  }
  std::vector<uint64_t> svec;  // sample vectors, ascending
  if (nv <= kStrSampleVectors) {
    for (uint64_t v = 0; v < nv; ++v) svec.push_back(v);
  } else {
    for (uint64_t k = 0; k < kStrSampleVectors; ++k) svec.push_back(k * (nv - 1) / (kStrSampleVectors - 1));
  }
  size_t si = 0;
  std::vector<const unsigned char*> sp;
  std::vector<size_t> sl;
  uint64_t sbytes = 0, plain_bytes = 0;
  const char* ptrs[kV];
  int32_t lens[kV];

  for (uint64_t v = 0; v < nv; ++v) {
    const uint32_t m = (uint32_t)std::min<uint64_t>(kV, n - v * kV);
    const int rc = d.get_strings(d.ctx, r0 + v * kV, m, ptrs, lens);
    if (rc != 0) return rc;
    uint64_t s = 4ull * m;
    for (uint32_t i = 0; i < m; ++i)
      if (lens[i] > 0) s += (uint64_t)lens[i];
    sc.vsizes[v] = s;
    plain_bytes += s - 4ull * m;
    if (dict) {
      int32_t* codes = sc.codes.data() + v * kV;
      for (uint32_t i = 0; i < m; ++i) codes[i] = lens[i] < 0 ? -1 : pd.code(ptrs[i], lens[i], dict_bytes);
      const uint64_t seen = v * kV + m;
      // Checked from 65,536 rows on: earlier checks gave up on medium-cardinality columns (50k product codes
      // with skewed popularity pass rows / 2 distinct values in the first 16k rows).
      if ((force != T_DICT && seen >= 65536 && sc.dptr.size() * 2 > seen) || sc.dptr.size() > (1u << 24) ||
          dict_bytes + 8 * sc.dptr.size() > kMaxRawPage / 2) {
        dict = false;  // too many distinct strings: stop paying for the hashing
        std::vector<int32_t>().swap(sc.codes);
        std::vector<const char*>().swap(sc.dptr);
        std::vector<int32_t>().swap(sc.dlen);
      }
    }
    if (si < svec.size() && svec[si] == v) {
      ++si;
      for (uint32_t i = 0; i < m; ++i)
        if (lens[i] >= 0) {
          sp.push_back((const unsigned char*)(lens[i] > 0 ? ptrs[i] : ""));
          sl.push_back((size_t)lens[i]);
          sbytes += (uint64_t)lens[i];
        }
    }
  }

  if (dict && !sc.dptr.empty()) {
    const uint64_t nd = sc.dptr.size();
    const double dict_raw = (double)dict_bytes + (double)nd + (double)n * bits_of(nd - 1) / 8.0;
    const double plain_raw = (double)plain_bytes + (double)n;
    if (force == T_DICT || dict_raw < plain_raw) {
      sc.mode = T_DICT;
      return 0;
    }
  }
  std::vector<int32_t>().swap(sc.codes);
  std::vector<const char*>().swap(sc.dptr);
  std::vector<int32_t>().swap(sc.dlen);

  if (force == T_PLAIN || sp.empty() || sbytes == 0) return 0;
  if (force != T_FSST && sbytes < 4096) return 0;  // too little text to be worth a symbol table

  std::unique_ptr<FsstTable> ft(new FsstTable());
  ft->enc = fsst_create(sp.size(), sl.data(), sp.data(), 0);
  if (!ft->enc) fail(FSTX_ERR_MEMORY, "cannot create an FSST symbol table");
  bool use = force == T_FSST;
  if (!use) {
    const size_t cap = 2 * sbytes + 8 * (sp.size() + sbytes / 511 + 2) + 64;
    std::vector<unsigned char> fo(cap);
    std::vector<size_t> lo(sp.size());
    std::vector<unsigned char*> po(sp.size());
    const size_t done = fsst_compress((fsst_encoder_t*)ft->enc, sp.size(), sl.data(), sp.data(), cap, fo.data(),
                                      lo.data(), po.data());
    if (done == sp.size()) {
      size_t fbytes = 0;
      for (size_t k = 0; k < done; ++k) fbytes += lo[k];
      fbytes += FSST_MAXHEADER / 2;  // the symbol table goes into every page
      if (cx.codec == FSTX_CODEC_NONE) {
        use = fbytes * 100 < sbytes * 85;
      } else {
        std::vector<uint8_t> cat;
        cat.reserve(sbytes);
        for (size_t k = 0; k < sp.size(); ++k) cat.insert(cat.end(), sp[k], sp[k] + sl[k]);
        const size_t zp = zstd_est(cx, cat.data(), cat.size());
        // FSST output is contiguous in fo
        const size_t zf = std::min(fbytes, zstd_est(cx, fo.data(), fbytes - FSST_MAXHEADER / 2) + FSST_MAXHEADER / 2);
        use = zf < zp;
      }
    }
  }
  if (use) {
    unsigned char buf[FSST_MAXHEADER];
    const unsigned int len = fsst_export((fsst_encoder_t*)ft->enc, buf);
    ft->blob.assign(buf, buf + len);
    sc.fsst = std::move(ft);
    sc.mode = T_FSST;
  }
  return 0;
}

uint32_t encode_str_page(int kind, const char* const* ptrs, const int32_t* lens, uint32_t n, int mode,
                         const FsstTable* fs, const EncCtx& cx0, Buf& out) {
  EncCtx cx = cx0;
  cx.force = 0;
  uint64_t total = 0;
  std::vector<int32_t> l0(n);
  bool any_na = false;
  for (uint32_t i = 0; i < n; ++i) {
    if (lens[i] < 0) {
      l0[i] = 0;
      any_na = true;
    } else {
      l0[i] = lens[i];
      total += (uint64_t)lens[i];
    }
  }
  if (mode == T_FSST && (!fs || !fs->enc || total > kFsstMaxPageBytes)) mode = T_PLAIN;
  out.clear();
  out.v.reserve((size_t)(mode == T_FSST ? 2 * total : total) + 4ull * n + 4096);
  {
    uint8_t* h = out.grow(8);
    h[0] = (uint8_t)kind;
    put_u32(h + 4, n);
  }
  int na = NA_NONE;
  if (any_na) {
    na = na_write_i32(lens, n, -1, out);
    out.v[1] = (uint8_t)na;
  }
  uint32_t tags;
  if (mode == T_FSST) {
    size_t h = out.open(T_FSST);
    put_u64(out.grow(8), total);
    size_t tb = out.open(T_PLAIN);
    out.put(fs->blob.data(), fs->blob.size());
    out.close(tb);
    std::vector<size_t> li(n);
    std::vector<const unsigned char*> pi(n);
    for (uint32_t i = 0; i < n; ++i) {
      li[i] = (size_t)l0[i];
      pi[i] = (const unsigned char*)(l0[i] > 0 ? ptrs[i] : "");
    }
    std::vector<size_t> lo(n);
    std::vector<unsigned char*> po(n);
    const size_t cap = 2 * (size_t)total + 8 * ((size_t)n + (size_t)total / 511 + 2) + 64;
    size_t bb = out.open(T_PLAIN);
    const size_t pos = out.size();
    out.grow(cap);
    unsigned char* base = out.v.data() + pos;
    const size_t done = fsst_compress((fsst_encoder_t*)fs->enc, n, li.data(), pi.data(), cap, base, lo.data(), po.data());
    if (done != n) fail(FSTX_ERR_INTERNAL, "FSST output buffer too small");
    size_t clen = 0;
    bool contiguous = true;
    for (uint32_t i = 0; i < n; ++i) {
      if (po[i] != base + clen) contiguous = false;
      clen += lo[i];
    }
    if (!contiguous) {
      std::vector<unsigned char> tmp(clen);
      size_t q = 0;
      for (uint32_t i = 0; i < n; ++i) {
        if (lo[i]) std::memcpy(tmp.data() + q, po[i], lo[i]);
        q += lo[i];
      }
      if (clen) std::memcpy(base, tmp.data(), clen);
    }
    out.v.resize(pos + clen);
    out.close(bb);
    std::vector<int32_t> cl(n);
    for (uint32_t i = 0; i < n; ++i) cl[i] = (int32_t)lo[i];
    const uint32_t c1 = encode_int32_auto(cl.data(), n, 1, cx, out) & 255u;
    out.close(h);
    tags = T_FSST | c1 << 8;
  } else {
    size_t h = out.open(T_PLAIN);
    const uint32_t c1 = encode_int32_auto(l0.data(), n, 1, cx, out) & 255u;
    size_t bb = out.open(T_PLAIN);
    uint8_t* dst = out.grow((size_t)total);
    for (uint32_t i = 0; i < n; ++i)
      if (l0[i] > 0) {
        std::memcpy(dst, ptrs[i], (size_t)l0[i]);
        dst += l0[i];
      }
    out.close(bb);
    out.close(h);
    tags = T_PLAIN | c1 << 8 | (uint32_t)T_PLAIN << 16;
  }
  return make_chain(kind, na, tags);
}

void decode_str_page(const uint8_t* body, size_t len, uint32_t chain, uint32_t n, int kind,
                     std::vector<int32_t>& lens, std::vector<uint8_t>& buf, const char** bytes) {
  check_str_header(body, len, chain, n, kind);
  Rd rd{body + 8, body + len};
  Blk nb{};
  const int na = body[1];
  if (na) nb = rd.next();
  if (rd.peek() != chain_tag(chain, 0)) corrupt("encoding does not match the page directory");
  Blk vb = rd.next();
  if (!rd.done()) corrupt("trailing bytes");
  lens.resize(n);
  if (vb.tag == T_PLAIN) {
    Cur c(vb);
    Rd in = c.rest();
    decode_int32_blk(in, lens.data(), n, 1);
    Blk bb = in.next();
    if (bb.tag != T_PLAIN) corrupt("string bytes");
    uint64_t tot = 0;
    for (uint32_t i = 0; i < n; ++i) {
      if (lens[i] < 0) corrupt("string length");
      tot += (uint64_t)lens[i];
    }
    if (tot != bb.len) corrupt("string lengths do not match the bytes");
    *bytes = (const char*)bb.p;
  } else if (vb.tag == T_FSST) {
    Cur c(vb);
    const uint64_t total = get_u64(c.take(8));
    Rd in = c.rest();
    Blk tb = in.next();
    if (tb.tag != T_PLAIN || !fsst_table_ok(tb.p, tb.len)) corrupt("FSST symbol table");
    fsst_decoder_t dec;
    if (fsst_import(&dec, tb.p) == 0) corrupt("FSST symbol table");
    Blk bb = in.next();
    if (bb.tag != T_PLAIN) corrupt("FSST bytes");
    decode_int32_blk(in, lens.data(), n, 1);
    uint64_t csum = 0;
    for (uint32_t i = 0; i < n; ++i) {
      if (lens[i] < 0) corrupt("FSST length");
      csum += (uint64_t)lens[i];
    }
    if (csum != bb.len || total > 8 * csum || total > kMaxRawPage) corrupt("FSST sizes");
    if (buf.size() < total + 64) buf.resize((size_t)total + 64);
    const unsigned char* src = bb.p;
    uint64_t pos = 0;
    for (uint32_t i = 0; i < n; ++i) {
      const size_t cl = (size_t)lens[i];
      size_t got = 0;
      if (cl) {
        got = fsst_decompress(&dec, cl, src, (size_t)(total - pos) + 32, buf.data() + pos);
        if (got > total - pos) corrupt("FSST string longer than the page");
      }
      lens[i] = (int32_t)got;
      pos += got;
      src += cl;
    }
    if (pos != total) corrupt("FSST total length");
    *bytes = (const char*)buf.data();
  } else {
    corrupt("unknown string encoding");
  }
  if (na) {
    const int32_t zero = 0;
    na_apply_i32(nb, na, n, lens.data(), -1, &zero);
  }
}

}  // namespace enc
}  // namespace fstx
