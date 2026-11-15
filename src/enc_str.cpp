// FST-X string encodings (file format 2): row-group dictionaries (hashing the host's string pointers),
// FSST pages (vendored cwida/fsst, one symbol table per column chunk, stored in every page) and plain pages
// (lengths through the integer chain, then the bytes). MIT licence. No R headers.
#include <algorithm>
#include <limits>

#include "enc.h"
#include "fsst.h"
#include "libfsst.hpp"

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

// FSST encoding of many strings, byte for byte what libfsst's scalar compressBulk writes (the vendored build has no
// AVX-512 path): each string in chunks of up to 511 bytes, each chunk followed by the table's terminator byte, then a
// greedy parse (long symbol from the hash table, else the shortCodes entry). compressBulk walks one string at a time
// and waits on each lookup; here 4 lanes walk 4 runs of strings at once and each step is branch-free. Lanes copy up to
// kFsstStage bytes of chunks (terminator after each) into a stage and record where the output stood at every input
// position, so chunk outputs can be cut out afterwards. Multi-byte symbols never contain the terminator byte, so no
// match crosses a chunk end and the parse at the terminator is one step; its output is dropped.
constexpr size_t kFsstLanes = 4;
constexpr size_t kFsstStage = 4096;
constexpr size_t kFsstStageCap = kFsstStage + 512 + 16;  // a chunk (511 + terminator) may start just under kFsstStage

struct FsstLane {
  size_t line = 0, end_line = 0, off = 0;  // next input: line, byte offset in it
  uint8_t* stage = nullptr;
  uint8_t* ob = nullptr;    // output of one round, terminator codes included
  uint16_t* pm = nullptr;   // pm[i]: low 16 bits of the output pointer when the parse reached stage position i
  uint16_t* tpos = nullptr; // terminator position of each chunk staged this round
  uint32_t* cline = nullptr;
  size_t nch = 0;
  const uint8_t* c = nullptr;
  const uint8_t* e = nullptr;
  uint8_t* o = nullptr;
  uint8_t* w0 = nullptr;    // where this lane's output starts
  uint8_t* w = nullptr;     // and where the next chunk output goes
};

// Per-thread scratch kept between pages (the pool threads live on), up to kKeepScratch bytes; bigger asks are freed after
// use. No zero fill: every byte is written before it is read.
constexpr size_t kKeepScratch = 4u << 20;
struct Scratch {
  std::unique_ptr<uint8_t[]> keep;
  size_t cap = 0;
  std::unique_ptr<uint8_t[]> big;
  uint8_t* get(size_t n) {
    if (n > kKeepScratch) {
      big.reset(new uint8_t[n]);
      return big.get();
    }
    big.reset();
    if (cap < n) {
      keep.reset(new uint8_t[kKeepScratch]);
      cap = kKeepScratch;
    }
    return keep.get();
  }
};
thread_local Scratch tl_lanes, tl_out;

static_assert(libfsst::SymbolTable::hashTabSize == 1024, "the 32-bit hash below keeps bits 0-24 of FSST_HASH only");

// One greedy step. pmc + 2 * c addresses pm[c - stage]; it gets the low 16 bits of the output pointer (a round writes
// under 64 KB). The hash uses a 32-bit product: bits 0-9 and 15-24 of FSST_HASH(w & 0xFFFFFF), the same index.
__attribute__((always_inline)) inline void fsst_step(const libfsst::SymbolTable& st, const uint8_t*& c, uint8_t*& o,
                                                     uintptr_t pmc) {
  using libfsst::u32;  // FSST_ICL_FREE uses it
  *(uint16_t*)(pmc + 2 * (uintptr_t)c) = (uint16_t)(uintptr_t)o;
  const uint64_t w = libfsst::fsst_unaligned_load(c);
  const uint64_t sc = st.shortCodes[w & 0xFFFF];
  const uint32_t hp = ((uint32_t)w & 0xFFFFFFu) * (uint32_t)FSST_HASH_PRIME;
  const libfsst::Symbol& s = st.hashTab[(hp ^ (hp >> FSST_SHIFT)) & (libfsst::SymbolTable::hashTabSize - 1)];
  const uint64_t icl = s.icl;
  const uint64_t hit = (uint64_t)((uint32_t)icl < (uint32_t)FSST_ICL_FREE) &
                       (uint64_t)(s.load_num() == (w & (0xFFFFFFFFFFFFFFFFull >> (uint8_t)icl)));
  // After finalize() a hash-table symbol has a code under 256, so icl >> 16 has the shortCodes layout: code in bits 0-8
  // (bit 8: escape), length from bit FSST_LEN_BITS. One masked select (masks keep the compiler from branching).
  const uint64_t m = 0 - hit;  // all ones on a long-symbol match
  const uint64_t v = ((icl >> 16) & m) | (sc & ~m);
  o[1] = (uint8_t)w;           // the escaped byte, kept only after an escape code
  o[0] = (uint8_t)v;
  o += 1 + ((v >> 8) & 1);
  c += v >> FSST_LEN_BITS;
}

// dst needs 2 * (total bytes) + 16. Writes the codes of all strings back to back; lo[i] = code bytes of string i.
size_t fsst_encode(const void* enc, size_t n, const size_t* len, const unsigned char* const* str, uint8_t* dst,
                   size_t* lo) {
  const libfsst::SymbolTable& st = *((const libfsst::Encoder*)enc)->symbolTable;
  const uint8_t term = (uint8_t)st.terminator;
  size_t total = 0;
  for (size_t i = 0; i < n; ++i) {
    lo[i] = 0;
    total += len[i];
  }
  if (total == 0) return 0;
  const size_t nl = total >= 4 * kFsstStage ? kFsstLanes : 1;
  // per lane: stage, round output (2x), pm (uint16), tpos (uint16), cline (uint32); 8-byte aligned pieces
  constexpr size_t o_pm = 3 * kFsstStageCap, o_tp = 5 * kFsstStageCap;
  constexpr size_t o_cl = (o_tp + 2 * (kFsstStage + 1) + 7) & ~(size_t)7;
  constexpr size_t per = (o_cl + 4 * (kFsstStage + 1) + 7) & ~(size_t)7;
  FsstLane ln[kFsstLanes];
  {
    uint8_t* m = tl_lanes.get(nl * per);
    size_t i = 0, acc = 0;
    for (size_t k = 0; k < nl; ++k) {
      FsstLane& L = ln[k];
      L.stage = m;
      L.ob = m + kFsstStageCap;
      L.pm = (uint16_t*)(m + o_pm);
      L.tpos = (uint16_t*)(m + o_tp);
      L.cline = (uint32_t*)(m + o_cl);
      m += per;
      L.line = i;
      L.w0 = L.w = dst + 2 * acc;
      const size_t want = (k + 1) * total / nl;  // lanes get about equal bytes
      while (i < n && (k + 1 == nl || acc < want)) acc += len[i++];
      L.end_line = i;
    }
  }
  auto fill = [&](FsstLane& L) {
    const uint16_t o16 = (uint16_t)(uintptr_t)L.ob;
    for (size_t j = 0; j < L.nch; ++j) {  // cut the last round's chunk outputs out
      const size_t a = j ? (uint16_t)(L.pm[L.tpos[j - 1] + 1u] - o16) : 0, b = (uint16_t)(L.pm[L.tpos[j]] - o16);
      std::memcpy(L.w, L.ob + a, b - a);
      L.w += b - a;
      lo[L.cline[j]] += b - a;
    }
    size_t s = 0, k = 0;
    while (L.line < L.end_line && s < kFsstStage) {
      const size_t l = len[L.line];
      if (l == 0) {
        ++L.line;
        continue;
      }
      const size_t ch = std::min<size_t>(511, l - L.off);
      std::memcpy(L.stage + s, str[L.line] + L.off, ch);
      s += ch;
      L.stage[s] = term;
      L.tpos[k] = (uint16_t)s++;
      L.cline[k++] = (uint32_t)L.line;
      L.off += ch;
      if (L.off == l) {
        L.off = 0;
        ++L.line;
      }
    }
    L.nch = k;
    L.c = L.stage;
    L.e = L.stage + s;
    L.o = L.ob;
    return k > 0;
  };
  bool live[kFsstLanes];
  for (size_t k = 0; k < nl; ++k) live[k] = fill(ln[k]);
  for (;;) {
    size_t id[kFsstLanes], m = 0;
    for (size_t k = 0; k < nl; ++k)
      if (live[k]) id[m++] = k;
    if (m == 0) break;
    // A step eats 1 to 8 bytes and never passes a terminator, so min(e - c) / 8 steps need no end checks.
    if (m == 4) {
      FsstLane &A = ln[id[0]], &B = ln[id[1]], &C = ln[id[2]], &D = ln[id[3]];
      const uint8_t *ca = A.c, *cb = B.c, *cc = C.c, *cd = D.c;
      const uint8_t *ea = A.e, *eb = B.e, *ec = C.e, *ed = D.e;
      uint8_t *oa = A.o, *ob = B.o, *oc = C.o, *od = D.o;
      const uintptr_t xa = (uintptr_t)A.pm - 2 * (uintptr_t)A.stage, xb = (uintptr_t)B.pm - 2 * (uintptr_t)B.stage,
                      xc = (uintptr_t)C.pm - 2 * (uintptr_t)C.stage, xd = (uintptr_t)D.pm - 2 * (uintptr_t)D.stage;
      for (;;) {
        size_t k = std::min(std::min((size_t)(ea - ca), (size_t)(eb - cb)), std::min((size_t)(ec - cc), (size_t)(ed - cd))) >> 3;
        if (k == 0) break;
        do {
          fsst_step(st, ca, oa, xa);
          fsst_step(st, cb, ob, xb);
          fsst_step(st, cc, oc, xc);
          fsst_step(st, cd, od, xd);
        } while (--k);
      }
      while (ca < ea && cb < eb && cc < ec && cd < ed) {
        fsst_step(st, ca, oa, xa);
        fsst_step(st, cb, ob, xb);
        fsst_step(st, cc, oc, xc);
        fsst_step(st, cd, od, xd);
      }
      A.c = ca, B.c = cb, C.c = cc, D.c = cd;
      A.o = oa, B.o = ob, C.o = oc, D.o = od;
    } else if (m >= 2) {
      FsstLane &A = ln[id[0]], &B = ln[id[1]];
      const uint8_t *ca = A.c, *cb = B.c, *ea = A.e, *eb = B.e;
      uint8_t *oa = A.o, *ob = B.o;
      const uintptr_t xa = (uintptr_t)A.pm - 2 * (uintptr_t)A.stage, xb = (uintptr_t)B.pm - 2 * (uintptr_t)B.stage;
      for (;;) {
        size_t k = std::min((size_t)(ea - ca), (size_t)(eb - cb)) >> 3;
        if (k == 0) break;
        do {
          fsst_step(st, ca, oa, xa);
          fsst_step(st, cb, ob, xb);
        } while (--k);
      }
      while (ca < ea && cb < eb) {
        fsst_step(st, ca, oa, xa);
        fsst_step(st, cb, ob, xb);
      }
      A.c = ca, B.c = cb;
      A.o = oa, B.o = ob;
    } else {
      FsstLane& A = ln[id[0]];
      const uint8_t *ca = A.c, *ea = A.e;
      uint8_t* oa = A.o;
      const uintptr_t xa = (uintptr_t)A.pm - 2 * (uintptr_t)A.stage;
      while (ca < ea) fsst_step(st, ca, oa, xa);
      A.c = ca;
      A.o = oa;
    }
    for (size_t j = 0; j < m; ++j) {
      FsstLane& L = ln[id[j]];
      if (L.c >= L.e) live[id[j]] = fill(L);
    }
  }
  // lane k wrote from dst + 2 * (bytes before its strings); move the lanes together
  size_t q = 0;
  for (size_t k = 0; k < nl; ++k) {
    const size_t b = (size_t)(ln[k].w - ln[k].w0);
    if (b && ln[k].w0 != dst + q) std::memmove(dst + q, ln[k].w0, b);
    q += b;
  }
  return q;
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
  const bool dict_fsst = (force & F_DICT_FSST) != 0;  // tests: FSST-code the dictionary page
  force &= 0xff;

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
      if (dict_fsst) {
        std::vector<const unsigned char*> dp(nd);
        std::vector<size_t> dl(nd);
        for (uint64_t k = 0; k < nd; ++k) {
          dp[k] = (const unsigned char*)(sc.dlen[k] > 0 ? sc.dptr[k] : "");
          dl[k] = (size_t)sc.dlen[k];
        }
        std::unique_ptr<FsstTable> ft(new FsstTable());
        ft->enc = fsst_create(nd, dl.data(), dp.data(), 0);
        if (!ft->enc) fail(FSTX_ERR_MEMORY, "cannot create an FSST symbol table");
        unsigned char buf[FSST_MAXHEADER];
        const unsigned int len = fsst_export((fsst_encoder_t*)ft->enc, buf);
        ft->blob.assign(buf, buf + len);
        sc.fsst = std::move(ft);
      }
      return 0;
    }
  }
  std::vector<int32_t>().swap(sc.codes);
  std::vector<const char*>().swap(sc.dptr);
  std::vector<int32_t>().swap(sc.dlen);

  if (force == T_PLAIN || sp.empty() || sbytes == 0) return 0;
  if (force != T_FSST && sbytes < 4096) return 0;  // too little text to be worth a symbol table

  std::unique_ptr<FsstTable> ft(new FsstTable());
  FSTX_PROF_BEGIN(prof_tab, W_FSST_TABLE);
  ft->enc = fsst_create(sp.size(), sl.data(), sp.data(), 0);
  FSTX_PROF_END(prof_tab);
  if (!ft->enc) fail(FSTX_ERR_MEMORY, "cannot create an FSST symbol table");
  bool use = force == T_FSST;
  if (!use) {
    FSTX_PROF_SCOPE(W_STR_CHOOSE);
    std::unique_ptr<uint8_t[]> fo(new uint8_t[2 * sbytes + 16]);
    std::vector<size_t> lo(sp.size());
    {
      size_t fbytes = fsst_encode(ft->enc, sp.size(), sl.data(), sp.data(), fo.get(), lo.data());
      fbytes += FSST_MAXHEADER / 2;  // the symbol table goes into every page
      if (cx.codec == FSTX_CODEC_NONE) {
        use = fbytes * 100 < sbytes * 85;
      } else {
        std::vector<uint8_t> cat;
        cat.reserve(sbytes);
        for (size_t k = 0; k < sp.size(); ++k) cat.insert(cat.end(), sp[k], sp[k] + sl[k]);
        const size_t zp = zstd_est(cx, cat.data(), cat.size());
        // FSST output is contiguous in fo
        const size_t zf = std::min(fbytes, zstd_est(cx, fo.get(), fbytes - FSST_MAXHEADER / 2) + FSST_MAXHEADER / 2);
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
    size_t bb = out.open(T_PLAIN);
    uint8_t* tmp = tl_out.get(2 * (size_t)total + 16);
    FSTX_PROF_BEGIN(prof_fe, W_FSST_ENC);
    const size_t clen = fsst_encode(fs->enc, n, li.data(), pi.data(), tmp, lo.data());
    FSTX_PROF_END(prof_fe);
    out.put(tmp, clen);
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
        // A code string never ends in a lone escape (255): the decoder would read symbol[255], past its table.
        // Only a string ending in 255 is looked at: the trailing run of 255s starts at a code boundary, so an odd
        // run means a lone escape.
        if (src[cl - 1] == 255) {
          size_t esc = 1;
          while (esc < cl && src[cl - 1 - esc] == 255) ++esc;
          if (esc & 1) corrupt("FSST escape at the end of a string");
        }
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
