// FST-X core engine: column sources. One column over a row range, decoded a page at a time on demand (lazy
// columns) or all pages at once in parallel (deferred strings, factor codes, materialising a lazy column).
// The host never sees file offsets; it asks for page p and gets decoded values. No R headers.
#include <algorithm>
#include <limits>
#include <memory>
#include <mutex>
#include <new>

#include "arrow_abi.h"
#include "core_file.h"
#include "vendor/zstd/zstd.h"

namespace fb = fstx::fb;

namespace {
struct SrcPage {
  const fb::Page* page = nullptr;  // null for memory sources and after detach
  uint64_t first = 0;              // first output row
  uint32_t n = 0;                  // rows in the stored page
  uint32_t skip = 0, take = 0;     // rows skipped at the page start, rows delivered
  int32_t dict = -1;               // dictionary of this page's chunk (string sources), -1 = none
  uint32_t nulls = 0;
};
struct SrcDict {
  const fb::Page* page = nullptr;
  uint32_t n = 0;
  bool ready = false;
  std::vector<int32_t> lens;
  std::vector<uint8_t> own;  // the entries' bytes, back to back
  std::vector<int32_t> remap;  // codes mode: entry -> 1-based level
};
struct PageData {
  bool ready = false;
  std::vector<uint8_t> vals;  // fixed / codes: decoded values of the delivered rows
  std::vector<int32_t> lens;  // strings: delivered rows (-1 = NA), or dictionary codes
  std::vector<uint8_t> own;   // strings: owned bytes
  const char* bytes = nullptr;
  uint64_t mem = 0;
  uint64_t tick = 0;
};
}  // namespace

// Buffers of decoded string pages kept for the next batch of a stream (arrow_abi.h: fstx_bufpool_*), so that a
// read does not map, fault in and free the same amount of memory once per batch.
struct fstx_bufpool {
  std::mutex m;
  std::vector<std::vector<uint8_t>> bytes;
  std::vector<std::vector<int32_t>> ints;
};

struct fstx_colsrc {
  const fstx_file* F = nullptr;
  uint32_t col = 0;
  int phys = FSTX_INT32;
  int logical = FSTX_LT_NONE;
  uint32_t n_levels = 0;
  int mode = FSTX_SRC_FIXED;
  bool verify = true;  // FSTX_VERIFY_HASH
  bool nulls = false;  // FSTX_VERIFY_NULLS
  bool own_bytes = true;
  uint64_t rows = 0;
  std::vector<SrcPage> pages;
  std::vector<SrcDict> dicts;
  std::vector<PageData> data;
  std::vector<uint64_t> firsts;
  uint64_t cache_bytes = 0;
  uint64_t cache_cap = 64ull << 20;  // decoded fixed pages kept per column before the oldest are dropped
  uint64_t tick = 0;
  uint32_t ready_count = 0;
  // M4: lazy fixed columns reuse the buffers of dropped pages, and a sequential scan (3 pages in a row)
  // decodes the next pages on `threads` workers at once
  int threads = 1;
  uint32_t last_req = UINT32_MAX, streak = 0;
  std::vector<std::vector<uint8_t>> pool;
  std::unique_ptr<fstx::CodecContext> cc{new fstx::CodecContext()};
  std::vector<uint8_t> scratch, scratch2;
  std::mutex count_m;  // the counters above, when pages are loaded from several threads (fstx_colsrc_load_page)
  fstx_bufpool* bufs = nullptr;  // string page buffers come from here and go back here (not owned)
  bool read = false;  // pages are copied out of the file (page_body_read), not touched in the mapping
  std::string name() const { return F ? F->col_name(col) : std::string("(in memory)"); }
};

// One worker's decode state for fstx_run_tasks / fstx_colsrc_load_page (arrow_abi.h).
struct fstx_scratch {
  fstx::CodecContext* cc;
  std::vector<uint8_t>* a;
  std::vector<uint8_t>* b;
  std::vector<uint8_t>* host;  // the caller's buffer (fstx_scratch_buffer): kept with a scratch set, else per task
};

// Decode state per worker kept between fstx_run_tasks calls (a stream's batches): codec contexts and the page
// buffers they decompress into are made once, not once per batch.
struct fstx_scratchset {
  std::vector<std::unique_ptr<fstx::CodecContext>> cc;
  std::vector<std::vector<uint8_t>> a, b, host;
  void ensure(size_t k) {  // before the workers start
    while (cc.size() < k) cc.emplace_back(new fstx::CodecContext());
    if (a.size() < k) a.resize(k);
    if (b.size() < k) b.resize(k);
    if (host.size() < k) host.resize(k);
  }
};

namespace fstx {
namespace {

// As page_body, but the stored bytes are copied out of the file (MappedFile::read_at) instead of touched in the
// mapping: the stored bytes go to buf (no codec) or to in and the body to buf (codec). Only the caller's buffers and
// codec context: these run on threads that may end (a stream pulled from a thread that is not R's).
const uint8_t* page_body_copy(const fstx_colsrc& S, const fb::Page* p, CodecContext& cc, std::vector<uint8_t>& buf,
                              std::vector<uint8_t>& in) {
  const fstx_file& F = *S.F;
  const size_t cl = (size_t)p->comp_len();
  const bool none = p->codec() == fb::Codec::None;
  std::vector<uint8_t>& got = none ? buf : in;
  if (got.size() < cl) got.resize(cl);
  F.map.read_at(p->offset(), got.data(), cl);
  if (S.verify && hash64(got.data(), cl) != p->hash())
    fail(FSTX_ERR_CHECKSUM, "page checksum mismatch in column '%s' (rows %llu to %llu): the file is corrupt",
         F.col_name(S.col).c_str(), (unsigned long long)p->first_row() + 1,
         (unsigned long long)(p->first_row() + p->row_count()));
  if (none) return buf.data();
  const size_t raw = p->raw_len();
  if (p->codec() == fb::Codec::ZSTD && ZSTD_getFrameContentSize(in.data(), cl) != raw)
    fail(FSTX_ERR_CORRUPT, "column '%s' (rows %llu to %llu): the page size does not match its ZSTD frame: the file is "
         "corrupt", F.col_name(S.col).c_str(), (unsigned long long)p->first_row() + 1,
         (unsigned long long)(p->first_row() + p->row_count()));
  if (buf.size() < raw) buf.resize(raw);
  decompress_page(cc, (int)p->codec(), in.data(), cl, buf.data(), raw);
  return buf.data();
}

const uint8_t* body_of(const fstx_colsrc& S, const fb::Page* p, CodecContext& cc, std::vector<uint8_t>& buf,
                       std::vector<uint8_t>& in) {
  return S.read ? page_body_copy(S, p, cc, buf, in) : page_body(*S.F, S.col, p, S.verify, cc, buf);
}

int width_mode(const fstx_colsrc& S) { return S.mode == FSTX_SRC_CODES ? 4 : width_of(S.phys); }

// String page buffers: from the source's pool when it has one (any contents, any size), else new.
template <typename T>
std::vector<T> take_buf(fstx_bufpool* P, std::vector<std::vector<T>> fstx_bufpool::*which) {
  std::vector<T> v;
  if (P) {
    std::lock_guard<std::mutex> g(P->m);
    std::vector<std::vector<T>>& l = P->*which;
    if (!l.empty()) {
      v.swap(l.back());
      l.pop_back();
    }
  }
  return v;
}
template <typename T>
void give_buf(fstx_bufpool* P, std::vector<std::vector<T>> fstx_bufpool::*which, std::vector<T>& v) {
  if (!P || !v.capacity()) return;
  std::lock_guard<std::mutex> g(P->m);
  (P->*which).push_back(std::move(v));
  v = std::vector<T>();
}

void free_page(fstx_colsrc& S, uint32_t p) {
  PageData& d = S.data[p];
  if (!d.ready) return;
  S.cache_bytes -= d.mem;
  if (S.mode == FSTX_SRC_FIXED && d.vals.capacity() && S.pool.size() < 64) {
    S.pool.push_back(std::move(d.vals));
    d.vals = std::vector<uint8_t>();
  } else {
    std::vector<uint8_t>().swap(d.vals);
  }
  give_buf(S.bufs, &fstx_bufpool::ints, d.lens);
  give_buf(S.bufs, &fstx_bufpool::bytes, d.own);
  std::vector<int32_t>().swap(d.lens);
  std::vector<uint8_t>().swap(d.own);
  d.bytes = nullptr;
  d.mem = 0;
  d.ready = false;
  --S.ready_count;
}

void decode_dict(const fstx_colsrc& S, SrcDict& D, CodecContext& cc, std::vector<uint8_t>& buf) {
  if (D.ready) return;
  if (!S.F || !D.page) fail(FSTX_ERR_INTERNAL, "dictionary is not loaded");
  try {
    std::vector<uint8_t> in;
    const uint8_t* b = body_of(S, D.page, cc, buf, in);
    std::vector<uint8_t> fbuf;
    const char* bytes = nullptr;
    enc::decode_str_page(b, D.page->raw_len(), D.page->chain(), D.n, enc::K_DICT, D.lens, fbuf, &bytes);
    uint64_t tot = 0;
    for (uint32_t i = 0; i < D.n; ++i) tot += (uint64_t)std::max(0, D.lens[i]);
    check_no_nul(bytes, D.lens.data(), D.n);
    D.own.assign((const uint8_t*)bytes, (const uint8_t*)bytes + tot);
    if (!S.read) S.F->map.release(D.page->offset(), D.page->comp_len());
  } catch (...) {
    rethrow_for_page(*S.F, S.col, D.page);
  }
  D.ready = true;
}

// Decodes page p into d (strings / codes into d's buffers; fixed values into dst when given, else into d.vals).
// Thread-safe for distinct pages as long as dictionaries were decoded beforehand (codes / dictionary pages).
void decode_page(const fstx_colsrc& S, uint32_t p, PageData& d, void* dst, CodecContext& cc, std::vector<uint8_t>& buf,
                 std::vector<uint8_t>& tmp) {
  const SrcPage& sp = S.pages[p];
  if (!S.F || !sp.page) fail(FSTX_ERR_INTERNAL, "page is not loaded and the source has no file");
  const fb::Page* pg = sp.page;
  const uint32_t n = sp.n;
  try {
    const uint8_t* b = body_of(S, pg, cc, buf, tmp);
    const size_t len = pg->raw_len();
    const uint32_t chain = pg->chain();
    if (S.mode == FSTX_SRC_FIXED) {
      const size_t w = (size_t)width_of(S.phys);
      const bool whole = sp.skip == 0 && sp.take == n;
      uint8_t* to;
      if (dst && whole) {
        to = (uint8_t*)dst;
      } else if (!dst && whole && d.vals.size() == (size_t)n * w) {  // buffer set up by prep_vals
        to = d.vals.data();
      } else {
        if (tmp.size() < (size_t)n * w) tmp.resize((size_t)n * w);
        to = tmp.data();
      }
      if (S.phys == FSTX_FLOAT64)
        enc::decode_dbl_page(b, len, chain, n, (double*)to);
      else
        enc::decode_int_page(b, len, chain, n, kind_of(S.phys), std::numeric_limits<int32_t>::min(), (int32_t*)to);
      if (S.nulls) {
        if (S.phys == FSTX_FLOAT64) check_nulls(pg, (const double*)to, n);
        else check_nulls(pg, (const int32_t*)to, n, std::numeric_limits<int32_t>::min());
      }
      const uint8_t* got = to + (size_t)sp.skip * w;
      if (S.logical == FSTX_LT_FACTOR) {
        const int32_t* v = (const int32_t*)got;
        const int32_t nl = (int32_t)std::min<uint32_t>(S.n_levels, 0x7fffffff);
        for (uint32_t i = 0; i < sp.take; ++i)
          if (v[i] != std::numeric_limits<int32_t>::min() && (v[i] < 1 || v[i] > nl))
            enc::corrupt("factor codes outside the levels");
      }
      if (dst) {
        if (!whole) std::memcpy(dst, got, (size_t)sp.take * w);
      } else {
        if (to != d.vals.data()) d.vals.assign(got, got + (size_t)sp.take * w);
        d.mem = d.vals.size();
      }
    } else if (sp.dict >= 0) {  // dictionary codes: strings mode keeps codes, codes mode maps them to levels
      const SrcDict& D = S.dicts[(size_t)sp.dict];
      std::vector<int32_t> codes = take_buf(S.bufs, &fstx_bufpool::ints);
      codes.resize(n);
      enc::decode_int_page(b, len, chain, n, enc::K_CODES, -1, codes.data());
      if (S.nulls) check_nulls(pg, codes.data(), n, -1);
      const int32_t nd = (int32_t)D.n;
      for (uint32_t i = 0; i < n; ++i)
        if (codes[i] < -1 || codes[i] >= nd) enc::corrupt("dictionary code out of range");
      if (S.mode == FSTX_SRC_CODES) {
        if (D.remap.size() != D.n) fail(FSTX_ERR_INTERNAL, "factor levels are not set");
        int32_t* out;
        if (dst) {
          out = (int32_t*)dst;
        } else {
          d.vals.resize((size_t)sp.take * 4);
          out = (int32_t*)d.vals.data();
          d.mem = d.vals.size();
        }
        for (uint32_t i = 0; i < sp.take; ++i) {
          const int32_t k = codes[sp.skip + i];
          out[i] = k < 0 ? std::numeric_limits<int32_t>::min() : D.remap[(size_t)k];
        }
      } else {
        if (sp.skip == 0 && sp.take == n) {
          d.lens.swap(codes);
        } else {
          d.lens = take_buf(S.bufs, &fstx_bufpool::ints);
          d.lens.assign(codes.begin() + sp.skip, codes.begin() + sp.skip + sp.take);
        }
        d.bytes = nullptr;
        d.mem = d.lens.size() * 4;
      }
      give_buf(S.bufs, &fstx_bufpool::ints, codes);
    } else {  // plain or FSST strings
      if (S.mode != FSTX_SRC_STRINGS) fail(FSTX_ERR_INTERNAL, "string page in a non-string source");
      std::vector<int32_t> lv = take_buf(S.bufs, &fstx_bufpool::ints);
      std::vector<uint8_t> fbuf = take_buf(S.bufs, &fstx_bufpool::bytes);
      const char* bytes = nullptr;
      enc::decode_str_page(b, len, chain, n, enc::K_STR, lv, fbuf, &bytes);
      if (S.nulls) check_nulls(pg, lv.data(), n, -1);
      uint64_t skip_bytes = 0, take_bytes = 0;
      for (uint32_t i = 0; i < sp.skip; ++i) skip_bytes += (uint64_t)std::max(0, lv[i]);
      for (uint32_t i = 0; i < sp.take; ++i) take_bytes += (uint64_t)std::max(0, lv[sp.skip + i]);
      const char* first = bytes + skip_bytes;
      if (sp.skip == 0 && sp.take == n) {
        d.lens.swap(lv);
      } else {
        d.lens = take_buf(S.bufs, &fstx_bufpool::ints);
        d.lens.assign(lv.begin() + sp.skip, lv.begin() + sp.skip + sp.take);
      }
      give_buf(S.bufs, &fstx_bufpool::ints, lv);
      check_no_nul(first, d.lens.data(), sp.take);
      const bool in_map = (const uint8_t*)bytes >= S.F->map.data() &&
                          (const uint8_t*)bytes < S.F->map.data() + S.F->map.size();
      if (in_map && !S.own_bytes) {
        d.bytes = first;  // lazy plain page without a codec: the strings stay in the mapped file
      } else if (!fbuf.empty() && (const uint8_t*)bytes == fbuf.data()) {
        d.own.swap(fbuf);
        d.bytes = (const char*)d.own.data() + skip_bytes;
      } else {
        d.own = take_buf(S.bufs, &fstx_bufpool::bytes);
        d.own.assign((const uint8_t*)first, (const uint8_t*)first + take_bytes);
        d.bytes = (const char*)d.own.data();
      }
      give_buf(S.bufs, &fstx_bufpool::bytes, fbuf);  // empty when it became the page's own bytes
      d.mem = d.lens.size() * 4 + d.own.size();
    }
    if (!S.read && !(S.mode == FSTX_SRC_STRINGS && !S.own_bytes && sp.dict < 0 && pg->codec() == fb::Codec::None))
      S.F->map.release(pg->offset(), pg->comp_len());
  } catch (...) {
    rethrow_for_page(*S.F, S.col, pg);
  }
}

// A whole fixed page decodes straight into d.vals: size it now, reusing a dropped page's buffer when there is one.
void prep_vals(fstx_colsrc& S, uint32_t p) {
  const SrcPage& sp = S.pages[p];
  if (S.mode != FSTX_SRC_FIXED || sp.skip != 0 || sp.take != sp.n) return;
  PageData& d = S.data[p];
  const size_t need = (size_t)sp.n * (size_t)width_of(S.phys);
  if (d.vals.size() == need) return;
  if (d.vals.capacity() < need && !S.pool.empty()) {
    d.vals = std::move(S.pool.back());
    S.pool.pop_back();
  }
  d.vals.resize(need);
}

// drop the oldest decoded fixed pages (not those in [keep_lo, keep_hi)) until the cache is half full
void trim_cache(fstx_colsrc& S, uint32_t keep_lo, uint32_t keep_hi) {
  if (S.mode == FSTX_SRC_STRINGS || S.cache_bytes <= S.cache_cap) return;
  while (S.cache_bytes > S.cache_cap / 2) {
    uint32_t old = UINT32_MAX;
    uint64_t best = UINT64_MAX;
    for (uint32_t q = 0; q < S.data.size(); ++q)
      if ((q < keep_lo || q >= keep_hi) && S.data[q].ready && S.data[q].tick < best) {
        best = S.data[q].tick;
        old = q;
      }
    if (old == UINT32_MAX) break;
    free_page(S, old);
  }
}

void make_ready(fstx_colsrc& S, uint32_t p) {
  PageData& d = S.data[p];
  if (d.ready) {
    d.tick = ++S.tick;
    return;
  }
  const SrcPage& sp = S.pages[p];
  FSTX_PROF_SCOPE(R_LAZY_PAGE);
  if (sp.dict >= 0 && S.mode == FSTX_SRC_STRINGS) decode_dict(S, S.dicts[(size_t)sp.dict], *S.cc, S.scratch);
  prep_vals(S, p);
  decode_page(S, p, d, nullptr, *S.cc, S.scratch, S.scratch2);
  d.ready = true;
  d.tick = ++S.tick;
  ++S.ready_count;
  S.cache_bytes += d.mem;
  trim_cache(S, p, p + 1);
}

// Runs fn(i) for i in [0, n) on up to `threads` workers (on this thread when the pool is busy). ss: the workers'
// decode state to use (else made for this call).
void parallel_for(int64_t n, int threads, const std::function<void(int64_t, CodecContext&, std::vector<uint8_t>&,
                                                                    std::vector<uint8_t>&)>& fn,
                  fstx_scratchset* ss = nullptr) {
  if (n <= 0) return;
  int k = (int)std::min<int64_t>(resolve_threads(threads), n);
  ThreadPool& pool = ThreadPool::instance();
  if (k <= 1 || pool.busy()) {
    if (ss) {
      ss->ensure(1);
      for (int64_t i = 0; i < n; ++i) fn(i, *ss->cc[0], ss->a[0], ss->b[0]);
      return;
    }
    CodecContext cc;
    std::vector<uint8_t> a, b;
    for (int64_t i = 0; i < n; ++i) fn(i, cc, a, b);
    return;
  }
  std::atomic<int64_t> next(0);
  std::atomic<bool> stop(false);
  std::vector<std::unique_ptr<CodecContext>> ctxs;
  if (ss) ss->ensure((size_t)k);
  else
    for (int i = 0; i < k; ++i) ctxs.emplace_back(new CodecContext());
  pool.start(k, [&](int id) {
    std::vector<uint8_t> la, lb;
    std::vector<uint8_t>& a = ss ? ss->a[(size_t)id] : la;
    std::vector<uint8_t>& b = ss ? ss->b[(size_t)id] : lb;
    CodecContext& cc = ss ? *ss->cc[(size_t)id] : *ctxs[(size_t)id];
    for (;;) {
      if (stop.load()) return;
      const int64_t i = next.fetch_add(1);
      if (i >= n) return;
      try {
        fn(i, cc, a, b);
      } catch (...) {
        stop.store(true);
        throw;
      }
    }
  });
  pool.wait();
}

// Sequential scan of a lazy fixed column reached page p (not decoded yet): decode p and the next pages in
// parallel, up to 2 per worker and a quarter of the cache.
void read_ahead(fstx_colsrc& S, uint32_t p) {
  FSTX_PROF_SCOPE(R_LAZY_PAGE);
  const int k = resolve_threads(S.threads);
  const SrcPage& sp = S.pages[p];
  const uint64_t pb = std::max<uint64_t>(1, (uint64_t)sp.n * (uint64_t)width_of(S.phys));
  uint64_t cnt = std::min<uint64_t>((uint64_t)2 * (uint64_t)k, std::max<uint64_t>(1, (S.cache_cap / 4) / pb));
  cnt = std::min<uint64_t>(cnt, S.pages.size() - p);
  std::vector<uint32_t> todo;
  for (uint32_t q = p; q < p + (uint32_t)cnt; ++q)
    if (!S.data[q].ready) {
      prep_vals(S, q);
      todo.push_back(q);
    }
  parallel_for((int64_t)todo.size(), k,
               [&](int64_t i, CodecContext& cc, std::vector<uint8_t>& a, std::vector<uint8_t>& b) {
                 const uint32_t q = todo[(size_t)i];
                 decode_page(S, q, S.data[q], nullptr, cc, a, b);
               });
  for (uint32_t q : todo) {
    PageData& d = S.data[q];
    d.ready = true;
    d.tick = ++S.tick;
    ++S.ready_count;
    S.cache_bytes += d.mem;
  }
  trim_cache(S, p, p + (uint32_t)cnt);
}

}  // namespace
}  // namespace fstx

using namespace fstx;

extern "C" int fstx_colsrc_open(const fstx_file* f, uint32_t col, uint64_t from, uint64_t to, int verify, int own_bytes,
                                fstx_colsrc** out, fstx_error* err) {
  set_error(err, FSTX_OK, "");
  *out = nullptr;
  fstx_colsrc* s = nullptr;
  try {
    if (!f || col >= f->n_cols) fail(FSTX_ERR_ARG, "column index out of range");
    if (from > to || to > f->n_rows) fail(FSTX_ERR_ARG, "row range is outside the table");
    s = new fstx_colsrc();
    s->F = f;
    s->col = col;
    const fb::Column* x = f->col(col);
    s->phys = (int)x->phys();
    s->logical = (int)x->logical();
    s->n_levels = x->levels() ? x->levels()->size() : 0;
    s->mode = s->phys == FSTX_STRING ? FSTX_SRC_STRINGS : FSTX_SRC_FIXED;
    s->verify = (verify & FSTX_VERIFY_HASH) != 0;
    s->nulls = (verify & FSTX_VERIFY_NULLS) != 0;
    s->own_bytes = own_bytes != 0;
    s->rows = to - from;
    const auto* rgs = f->footer->row_groups();
    const uint32_t nrg = rgs ? rgs->size() : 0;
    if (from < to) {
      for (uint32_t g = 0; g < nrg; ++g) {
        const fb::RowGroup* rg = rgs->Get(g);
        if (rg->first_row() + rg->row_count() <= from) continue;
        if (rg->first_row() >= to) break;
        const fb::ColumnChunk* ch = rg->columns()->Get(col);
        const fb::Page* dp = ch->dictionary();
        int32_t di = -1;
        if (dp) {
          SrcDict D;
          D.page = dp;
          D.n = dp->row_count();
          s->dicts.push_back(std::move(D));
          di = (int32_t)s->dicts.size() - 1;
        }
        const auto* pages = ch->pages();
        for (uint32_t i = 0; i < pages->size(); ++i) {
          const fb::Page* p = pages->Get(i);
          if (p->first_row() + p->row_count() <= from) continue;
          if (p->first_row() >= to) break;
          const uint64_t a = std::max(from, p->first_row()), e = std::min(to, p->first_row() + p->row_count());
          SrcPage sp;
          sp.page = p;
          sp.first = a - from;
          sp.n = p->row_count();
          sp.skip = (uint32_t)(a - p->first_row());
          sp.take = (uint32_t)(e - a);
          sp.dict = di;
          sp.nulls = (sp.skip == 0 && sp.take == sp.n) ? p->null_count() : (p->null_count() ? 1u : 0u);
          s->pages.push_back(sp);
          s->firsts.push_back(sp.first);
        }
      }
    }
    s->data.resize(s->pages.size());
    *out = s;
    return FSTX_OK;
  } catch (...) {
    delete s;
    return to_error(err);
  }
}

extern "C" int fstx_colsrc_from_strings(uint64_t n, const int32_t* lens, const char* bytes, fstx_colsrc** out,
                                        fstx_error* err) {
  set_error(err, FSTX_OK, "");
  *out = nullptr;
  fstx_colsrc* s = nullptr;
  try {
    s = new fstx_colsrc();
    s->phys = FSTX_STRING;
    s->mode = FSTX_SRC_STRINGS;
    s->rows = n;
    const uint64_t step = 65536;
    const char* q = bytes;
    for (uint64_t r = 0; r < n; r += step) {
      const uint32_t k = (uint32_t)std::min<uint64_t>(step, n - r);
      SrcPage sp;
      sp.first = r;
      sp.n = sp.take = k;
      PageData d;
      d.lens.assign(lens + r, lens + r + k);
      uint64_t tot = 0;
      for (uint32_t i = 0; i < k; ++i) {
        if (d.lens[i] < -1) fail(FSTX_ERR_CORRUPT, "invalid string length in serialized data");
        if (d.lens[i] < 0) ++sp.nulls;
        else tot += (uint64_t)d.lens[i];
      }
      d.own.assign((const uint8_t*)q, (const uint8_t*)q + tot);
      q += tot;
      d.bytes = (const char*)d.own.data();
      d.mem = d.lens.size() * 4 + d.own.size();
      d.ready = true;
      s->cache_bytes += d.mem;
      ++s->ready_count;
      s->pages.push_back(sp);
      s->firsts.push_back(sp.first);
      s->data.push_back(std::move(d));
    }
    *out = s;
    return FSTX_OK;
  } catch (...) {
    delete s;
    return to_error(err);
  }
}

extern "C" void fstx_colsrc_free(fstx_colsrc* s) {
  if (s && s->bufs)
    for (PageData& d : s->data) {
      give_buf(s->bufs, &fstx_bufpool::bytes, d.own);
      give_buf(s->bufs, &fstx_bufpool::ints, d.lens);
    }
  delete s;
}

extern "C" fstx_bufpool* fstx_bufpool_new(void) { return new (std::nothrow) fstx_bufpool(); }
extern "C" void fstx_bufpool_free(fstx_bufpool* p) { delete p; }
extern "C" void fstx_colsrc_set_bufpool(fstx_colsrc* s, fstx_bufpool* p) { s->bufs = p; }
extern "C" void fstx_colsrc_set_read(fstx_colsrc* s, int read) { s->read = read != 0; }

extern "C" void fstx_colsrc_info_get(const fstx_colsrc* s, fstx_colsrc_info* o) {
  std::memset(o, 0, sizeof(*o));
  o->rows = s->rows;
  o->phys = s->phys;
  o->logical = s->logical;
  o->mode = s->mode;
  o->n_pages = (uint32_t)s->pages.size();
  o->n_dicts = (uint32_t)s->dicts.size();
  bool all = !s->pages.empty();
  uint64_t nulls = 0;
  for (const SrcPage& p : s->pages) {
    if (p.dict < 0) all = false;
    nulls += p.nulls;
  }
  o->all_dict = all ? 1 : 0;
  // FSTX_VERIFY_NULLS: the stored counts are checked only when a page is decoded, so "none" is not promised before
  if (s->nulls && nulls == 0 && !s->pages.empty()) nulls = 1;
  o->null_count = nulls;
  o->pages_ready = s->ready_count;
  o->cached_bytes = s->cache_bytes;
  o->detached = s->F ? 0 : 1;
}

extern "C" uint32_t fstx_colsrc_page_of(const fstx_colsrc* s, uint64_t row) {
  auto it = std::upper_bound(s->firsts.begin(), s->firsts.end(), row);
  return it == s->firsts.begin() ? 0u : (uint32_t)(it - s->firsts.begin() - 1);
}

extern "C" void fstx_colsrc_page_span(const fstx_colsrc* s, uint32_t p, uint64_t* first, uint32_t* n) {
  *first = s->pages[p].first;
  *n = s->pages[p].take;
}

extern "C" int fstx_colsrc_dict(fstx_colsrc* s, uint32_t d, uint32_t* n, const int32_t** lens, const char** bytes,
                                fstx_error* err) {
  set_error(err, FSTX_OK, "");
  try {
    if (d >= s->dicts.size()) fail(FSTX_ERR_ARG, "dictionary index out of range");
    SrcDict& D = s->dicts[d];
    decode_dict(*s, D, *s->cc, s->scratch);
    *n = D.n;
    *lens = D.lens.data();
    *bytes = (const char*)D.own.data();
    return FSTX_OK;
  } catch (...) {
    return to_error(err);
  }
}

extern "C" int fstx_colsrc_set_codes(fstx_colsrc* s, const int32_t* const* remap, fstx_error* err) {
  set_error(err, FSTX_OK, "");
  try {
    if (s->mode != FSTX_SRC_STRINGS) fail(FSTX_ERR_ARG, "only string columns become factor codes");
    for (const SrcPage& p : s->pages)
      if (p.dict < 0) fail(FSTX_ERR_ARG, "column is not dictionary-encoded in every row group");
    for (uint32_t i = 0; i < s->data.size(); ++i) free_page(*s, i);
    for (size_t d = 0; d < s->dicts.size(); ++d) {
      SrcDict& D = s->dicts[d];
      D.remap.assign(remap[d], remap[d] + D.n);
      std::vector<int32_t>().swap(D.lens);  // the strings are not needed any more
      std::vector<uint8_t>().swap(D.own);
      D.ready = false;
    }
    s->mode = FSTX_SRC_CODES;
    return FSTX_OK;
  } catch (...) {
    return to_error(err);
  }
}

extern "C" int fstx_colsrc_fixed_page(fstx_colsrc* s, uint32_t p, const void** vals, fstx_error* err) {
  set_error(err, FSTX_OK, "");
  try {
    if (p >= s->pages.size()) fail(FSTX_ERR_ARG, "page index out of range");
    if (s->mode == FSTX_SRC_STRINGS) fail(FSTX_ERR_ARG, "not a fixed-width source");
    if (p != s->last_req) {
      s->streak = (s->last_req != UINT32_MAX && p == s->last_req + 1) ? s->streak + 1 : 0;
      s->last_req = p;
    }
    if (!s->data[p].ready && s->mode == FSTX_SRC_FIXED && s->F && s->threads != 1 && s->streak >= 2)
      read_ahead(*s, p);
    make_ready(*s, p);
    *vals = s->data[p].vals.data();
    return FSTX_OK;
  } catch (...) {
    return to_error(err);
  }
}

extern "C" int fstx_colsrc_str_page(fstx_colsrc* s, uint32_t p, const int32_t** lens, const char** bytes, int32_t* dict,
                                    fstx_error* err) {
  set_error(err, FSTX_OK, "");
  try {
    if (p >= s->pages.size()) fail(FSTX_ERR_ARG, "page index out of range");
    if (s->mode != FSTX_SRC_STRINGS) fail(FSTX_ERR_ARG, "not a string source");
    make_ready(*s, p);
    *lens = s->data[p].lens.data();
    *bytes = s->data[p].bytes;
    *dict = s->pages[p].dict;
    return FSTX_OK;
  } catch (...) {
    return to_error(err);
  }
}

// Thread-safe for distinct pages of one source, once its dictionaries are decoded (fstx_colsrc_dict).
extern "C" int fstx_colsrc_load_page(fstx_colsrc* s, uint32_t p, fstx_scratch* w, const int32_t** lens,
                                     const char** bytes, int32_t* dict, fstx_error* err) {
  set_error(err, FSTX_OK, "");
  try {
    if (p >= s->pages.size()) fail(FSTX_ERR_ARG, "page index out of range");
    if (s->mode != FSTX_SRC_STRINGS) fail(FSTX_ERR_ARG, "not a string source");
    const SrcPage& sp = s->pages[p];
    if (sp.dict >= 0 && !s->dicts[(size_t)sp.dict].ready) fail(FSTX_ERR_INTERNAL, "dictionary is not decoded");
    PageData& d = s->data[p];
    if (!d.ready) {
      if (w) {
        decode_page(*s, p, d, nullptr, *w->cc, *w->a, *w->b);
      } else {
        CodecContext cc;
        std::vector<uint8_t> a, b;
        decode_page(*s, p, d, nullptr, cc, a, b);
      }
      std::lock_guard<std::mutex> g(s->count_m);
      d.ready = true;
      d.tick = ++s->tick;
      ++s->ready_count;
      s->cache_bytes += d.mem;
    }
    *lens = d.lens.data();
    *bytes = d.bytes;
    *dict = sp.dict;
    return FSTX_OK;
  } catch (...) {
    return to_error(err);
  }
}

// Thread-safe for distinct pages of one source: the page's buffers are freed outside the counters' lock.
extern "C" void fstx_colsrc_drop_page(fstx_colsrc* s, uint32_t p) {
  if (p >= s->data.size()) return;
  PageData& d = s->data[p];
  if (!d.ready) return;
  std::vector<uint8_t> own, vals;
  std::vector<int32_t> lens;
  own.swap(d.own);
  vals.swap(d.vals);
  lens.swap(d.lens);
  give_buf(s->bufs, &fstx_bufpool::bytes, own);
  give_buf(s->bufs, &fstx_bufpool::ints, lens);
  const uint64_t mem = d.mem;
  d.bytes = nullptr;
  d.mem = 0;
  std::lock_guard<std::mutex> g(s->count_m);
  d.ready = false;
  s->cache_bytes -= mem;
  --s->ready_count;
}

extern "C" int fstx_run_tasks(int64_t n, int threads, fstx_task_fn fn, void* ctx, fstx_scratchset* ss,
                              fstx_error* err) {
  set_error(err, FSTX_OK, "");
  try {
    parallel_for(
        n, threads,
        [&](int64_t i, CodecContext& cc, std::vector<uint8_t>& a, std::vector<uint8_t>& b) {
          // the worker's own buffer from the scratch set (a is that worker's slot), else one for this task
          std::vector<uint8_t> own;
          std::vector<uint8_t>* host = &own;
          if (ss && !ss->a.empty() && &a >= ss->a.data() && &a < ss->a.data() + ss->a.size())
            host = &ss->host[(size_t)(&a - ss->a.data())];
          fstx_scratch w{&cc, &a, &b, host};
          if (fn(ctx, i, &w) != 0) fail(FSTX_ERR_INTERNAL, "a task failed");
        },
        ss);
    return FSTX_OK;
  } catch (...) {
    return to_error(err);
  }
}

extern "C" void fstx_colsrc_set_threads(fstx_colsrc* s, int threads) { s->threads = threads; }

extern "C" void fstx_colsrc_drop(fstx_colsrc* s, uint32_t p) {
  if (p == UINT32_MAX) {
    for (uint32_t i = 0; i < s->data.size(); ++i) free_page(*s, i);
  } else if (p < s->data.size()) {
    free_page(*s, p);
  }
}

// fstx_colsrc_load on `threads` workers, or on this thread with a worker's decode state w.
static int colsrc_load(fstx_colsrc* s, void* dst, int threads, fstx_scratch* w, fstx_error* err) {
  set_error(err, FSTX_OK, "");
  using Fn = std::function<void(int64_t, CodecContext&, std::vector<uint8_t>&, std::vector<uint8_t>&)>;
  auto parallel_for = [w](int64_t n, int threads, const Fn& fn) {
    if (!w) return fstx::parallel_for(n, threads, fn);
    for (int64_t i = 0; i < n; ++i) fn(i, *w->cc, *w->a, *w->b);
  };
  try {
    if (s->mode != FSTX_SRC_STRINGS && !dst) fail(FSTX_ERR_ARG, "fixed-width sources load into a buffer");
    if (s->mode == FSTX_SRC_CODES) {
      for (const SrcDict& D : s->dicts)
        if (D.remap.size() != D.n) fail(FSTX_ERR_ARG, "factor levels are not set");
    } else {
      // dictionaries first (they are small); pages that use them decode in parallel afterwards
      parallel_for((int64_t)s->dicts.size(), threads,
                   [&](int64_t d, CodecContext& cc, std::vector<uint8_t>& a, std::vector<uint8_t>&) {
                     decode_dict(*s, s->dicts[(size_t)d], cc, a);
                   });
    }
    const size_t w = (size_t)width_mode(*s);
    std::vector<uint32_t> todo;
    for (uint32_t p = 0; p < s->pages.size(); ++p)
      if (dst || !s->data[p].ready) todo.push_back(p);
    parallel_for((int64_t)todo.size(), threads,
                 [&](int64_t i, CodecContext& cc, std::vector<uint8_t>& a, std::vector<uint8_t>& b) {
                   const uint32_t p = todo[(size_t)i];
                   const SrcPage& sp = s->pages[p];
                   if (dst) {
                     PageData& d = s->data[p];
                     if (d.ready && s->mode != FSTX_SRC_STRINGS) {  // already decoded: copy
                       std::memcpy((uint8_t*)dst + sp.first * w, d.vals.data(), (size_t)sp.take * w);
                     } else {
                       PageData tmp;
                       decode_page(*s, p, tmp, (uint8_t*)dst + sp.first * w, cc, a, b);
                     }
                   } else {
                     decode_page(*s, p, s->data[p], nullptr, cc, a, b);
                   }
                 });
    if (!dst) {
      for (uint32_t p : todo) {
        PageData& d = s->data[p];
        d.ready = true;
        d.tick = ++s->tick;
        ++s->ready_count;
        s->cache_bytes += d.mem;
      }
    }
    return FSTX_OK;
  } catch (...) {
    return to_error(err);
  }
}

extern "C" int fstx_colsrc_load(fstx_colsrc* s, void* dst, int threads, fstx_error* err) {
  return colsrc_load(s, dst, threads, nullptr, err);
}

extern "C" int fstx_colsrc_load_with(fstx_colsrc* s, void* dst, fstx_scratch* w, fstx_error* err) {
  return colsrc_load(s, dst, 1, w, err);
}

extern "C" void* fstx_scratch_buffer(fstx_scratch* w, size_t bytes) {
  try {
    if (w->host->size() < bytes) w->host->resize(bytes);
    return w->host->data();
  } catch (...) {
    return nullptr;
  }
}

extern "C" fstx_scratchset* fstx_scratchset_new(void) { return new (std::nothrow) fstx_scratchset(); }
extern "C" void fstx_scratchset_free(fstx_scratchset* ss) { delete ss; }

extern "C" int fstx_colsrc_detach(fstx_colsrc* s, fstx_error* err) {
  set_error(err, FSTX_OK, "");
  try {
    if (s->mode != FSTX_SRC_STRINGS) fail(FSTX_ERR_ARG, "only string sources can be detached");
    if (s->F) {
      for (SrcDict& D : s->dicts)
        if (!D.ready) fail(FSTX_ERR_INTERNAL, "detach before every dictionary is decoded");
      for (uint32_t p = 0; p < s->pages.size(); ++p) {
        PageData& d = s->data[p];
        if (!d.ready) fail(FSTX_ERR_INTERNAL, "detach before every page is decoded");
        if (s->pages[p].dict < 0 && d.own.empty() && d.bytes) {  // bytes still in the mapped file: copy them
          uint64_t tot = 0;
          for (int32_t l : d.lens) tot += (uint64_t)std::max(0, l);
          d.own.assign((const uint8_t*)d.bytes, (const uint8_t*)d.bytes + tot);
          d.bytes = (const char*)d.own.data();
          s->cache_bytes += tot;
          d.mem += tot;
        }
      }
    }
    for (SrcPage& p : s->pages) p.page = nullptr;
    for (SrcDict& D : s->dicts) D.page = nullptr;
    s->F = nullptr;
    return FSTX_OK;
  } catch (...) {
    return to_error(err);
  }
}
