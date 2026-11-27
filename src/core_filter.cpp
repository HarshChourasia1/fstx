// FST-X core engine: filters (M5). Pages are skipped by their footer statistics (min / max / NA count) and,
// for dictionary string chunks, by the dictionary itself; the rows left are then tested exactly, page by page
// in parallel, into a row bitmap. Several predicates are ANDed. No R headers.
#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <memory>
#include <string_view>
#include <unordered_set>

#include "core_file.h"
#include "hwy/base.h"  // hwy::PopCount: portable (no compiler extension)

namespace fb = fstx::fb;

struct fstx_selection {
  std::vector<uint64_t> rows;
};

namespace fstx {
// core_reader.cpp: a page body copied out of the file instead of touched in the mapping
const uint8_t* page_body_read(const fstx_file& F, uint32_t col, const fb::Page* p, bool verify, CodecContext& cc,
                              std::vector<uint8_t>& buf, std::vector<uint8_t>& stored);

namespace {

const int32_t kNaI = std::numeric_limits<int32_t>::min();

inline bool r_na_real(double x) {
  uint64_t b;
  std::memcpy(&b, &x, 8);
  return (uint32_t)(b & 0xffffffffu) == 1954u;
}

struct Pred {
  uint32_t col = 0;
  int op = FSTX_OP_NONE;
  int phys = 0;
  double v = 0;
  std::vector<double> set;  // sorted, no NA / NaN
  bool set_na = false, set_nan = false;
  std::vector<std::string> strs;
  std::unordered_set<std::string_view> sset;
};

// one dictionary string chunk: which entries match, and whether any / all do
struct DictMatch {
  std::vector<uint8_t> m;
  bool any = false, all = true;
};

struct PageRef {
  const fb::Page* p;
  uint64_t r0, r1;    // rows of the window in this page
  int32_t dict = -1;  // index into Filter::dm (string dictionary chunks)
  bool all = false;   // the statistics prove every row matches: no decoding needed
};

enum Verdict { V_SKIP, V_MAYBE, V_ALL };

bool str_match(const Pred& q, const char* s, int32_t len) {
  if (len < 0) return q.op == FSTX_OP_ISNA || (q.op == FSTX_OP_IN && q.set_na);
  switch (q.op) {
    case FSTX_OP_NOTNA: return true;
    case FSTX_OP_ISNA: return false;
    case FSTX_OP_EQ:
    case FSTX_OP_NE: {
      const std::string& t = q.strs[0];
      const bool eq = (size_t)len == t.size() && (len == 0 || std::memcmp(s, t.data(), (size_t)len) == 0);
      return q.op == FSTX_OP_EQ ? eq : !eq;
    }
    case FSTX_OP_IN: return q.sset.count(std::string_view(s, (size_t)len)) > 0;
    default: return false;
  }
}

inline bool in_set(const Pred& q, double x) { return std::binary_search(q.set.begin(), q.set.end(), x); }

inline bool num_match(const Pred& q, double x) {
  switch (q.op) {
    case FSTX_OP_EQ: return x == q.v;
    case FSTX_OP_NE: return x != q.v;
    case FSTX_OP_LT: return x < q.v;
    case FSTX_OP_LE: return x <= q.v;
    case FSTX_OP_GT: return x > q.v;
    case FSTX_OP_GE: return x >= q.v;
    case FSTX_OP_IN: return in_set(q, x);
    case FSTX_OP_NOTNA: return true;
    default: return false;
  }
}

inline bool int_match(const Pred& q, int32_t x) {
  if (x == kNaI) return q.op == FSTX_OP_ISNA || (q.op == FSTX_OP_IN && q.set_na);
  return num_match(q, (double)x);
}

inline bool dbl_match(const Pred& q, double x) {
  if (std::isnan(x)) {
    if (q.op == FSTX_OP_ISNA) return true;
    if (q.op == FSTX_OP_IN) return r_na_real(x) ? q.set_na : q.set_nan;
    return false;
  }
  return num_match(q, x);
}

// does any set element fall in [mn, mx]?
bool set_hits(const Pred& q, double mn, double mx) {
  auto it = std::lower_bound(q.set.begin(), q.set.end(), mn);
  return it != q.set.end() && *it <= mx;
}

// known = false (FSTX_VERIFY_NULLS): null_count is checked only when the page is decoded, so no verdict rests on it
Verdict num_verdict(const Pred& q, const fb::Page* p, bool dbl, bool known) {
  const uint32_t n = p->row_count(), nc = p->null_count();
  const bool mm = (p->flags() & kFlagMinMax) != 0;
  const double mn = p->min(), mx = p->max();
  // doubles: NaN is in no statistic (min / max skip it, null_count counts R's NA only)
  switch (q.op) {
    case FSTX_OP_NONE: return V_SKIP;
    case FSTX_OP_ISNA:
      if (dbl) return mm ? V_MAYBE : V_ALL;
      if (!known) return V_MAYBE;
      return nc == 0 ? V_SKIP : nc == n ? V_ALL : V_MAYBE;
    case FSTX_OP_NOTNA:
      if (!mm) return V_SKIP;
      return (!dbl && known && nc == 0) ? V_ALL : V_MAYBE;
    case FSTX_OP_IN: {
      const bool hit = mm && set_hits(q, mn, mx);
      if (dbl && q.set_nan) return V_MAYBE;
      if (q.set_na && (nc > 0 || !known)) return (!dbl && !mm && known) ? V_ALL : V_MAYBE;
      if (!hit) return V_SKIP;
      if (!dbl && known && nc == 0 && mn == mx) return V_ALL;
      return V_MAYBE;
    }
    default: break;
  }
  if (!mm) return V_SKIP;  // only NA / NaN: no comparison is true
  const double v = q.v;
  bool skip = false, all = false;
  switch (q.op) {
    case FSTX_OP_EQ: skip = v < mn || v > mx; all = mn == v && mx == v; break;
    case FSTX_OP_NE: skip = mn == v && mx == v; all = v < mn || v > mx; break;
    case FSTX_OP_LT: skip = mn >= v; all = mx < v; break;
    case FSTX_OP_LE: skip = mn > v; all = mx <= v; break;
    case FSTX_OP_GT: skip = mx <= v; all = mn > v; break;
    case FSTX_OP_GE: skip = mx < v; all = mn >= v; break;
    default: break;
  }
  if (skip) return V_SKIP;
  return (all && known && nc == 0 && !dbl) ? V_ALL : V_MAYBE;
}

Verdict str_verdict(const Pred& q, const fb::Page* p, const DictMatch* d, bool known) {
  const uint32_t n = p->row_count(), nc = p->null_count();
  const bool na_ok = q.op == FSTX_OP_ISNA || (q.op == FSTX_OP_IN && q.set_na);
  if (q.op == FSTX_OP_NONE) return V_SKIP;
  if (!known && (q.op == FSTX_OP_ISNA || q.op == FSTX_OP_NOTNA)) return V_MAYBE;
  if (q.op == FSTX_OP_ISNA) return nc == 0 ? V_SKIP : nc == n ? V_ALL : V_MAYBE;
  if (q.op == FSTX_OP_NOTNA) return nc == n ? V_SKIP : nc == 0 ? V_ALL : V_MAYBE;
  const bool some_na = !known || nc > 0, some_val = !known || nc < n;
  if (d) {
    const bool can = (some_val && d->any) || (some_na && na_ok);
    if (!can) return V_SKIP;
    const bool every = (!some_val || d->all) && (!some_na || na_ok);
    return every ? V_ALL : V_MAYBE;
  }
  if (!some_val && !na_ok) return V_SKIP;
  return V_MAYBE;
}

struct Filter {
  const fstx_file& F;
  std::vector<Pred> preds;
  uint64_t from, to;
  int threads;
  bool verify, nulls;  // FSTX_VERIFY_HASH, FSTX_VERIFY_NULLS
  std::vector<DictMatch> dm;
  std::vector<std::vector<PageRef>> keep;  // [pred]: pages not skipped
  std::vector<std::atomic<uint64_t>> bits;
  fstx_filter_stats st{};

  Filter(const fstx_file& f, uint64_t a, uint64_t b, int t, int v)
      : F(f), from(a), to(b), threads(t), verify((v & FSTX_VERIFY_HASH) != 0), nulls((v & FSTX_VERIFY_NULLS) != 0) {}

  void load_dict(const Pred& q, uint32_t col, const fb::Page* p, CodecContext& cc, DictMatch& d) {
    std::vector<uint8_t> buf, fbuf, stored;
    std::vector<int32_t> lens;
    const char* bytes = nullptr;
    try {
      const uint8_t* b = page_body_read(F, col, p, verify, cc, buf, stored);
      enc::decode_str_page(b, p->raw_len(), p->chain(), p->row_count(), enc::K_DICT, lens, fbuf, &bytes);
    } catch (...) {
      rethrow_for_page(F, col, p);
    }
    const uint32_t n = p->row_count();
    d.m.assign(n, 0);
    d.any = false;
    d.all = true;
    uint64_t off = 0;
    for (uint32_t k = 0; k < n; ++k) {
      const int32_t l = lens[k];
      const bool m = str_match(q, bytes + off, l);
      if (l > 0) off += (uint64_t)l;
      d.m[k] = m;
      d.any = d.any || m;
      d.all = d.all && m;
    }
  }

  // ---- pass 1: page statistics
  void skip_pass() {
    const auto* rgs = F.footer->row_groups();
    const uint32_t nrg = rgs ? rgs->size() : 0;
    keep.assign(preds.size(), {});
    CodecContext cc;
    for (size_t i = 0; i < preds.size(); ++i) {
      const Pred& q = preds[i];
      const bool dbl = q.phys == FSTX_FLOAT64;
      for (uint32_t g = 0; g < nrg; ++g) {
        const fb::RowGroup* rg = rgs->Get(g);
        if (rg->first_row() >= to || rg->first_row() + rg->row_count() <= from) continue;
        const fb::ColumnChunk* ch = rg->columns()->Get(q.col);
        int32_t di = -1;
        if (is_str(q.phys) && ch->dictionary() && q.op != FSTX_OP_NONE) {  // code pages need it too
          di = (int32_t)dm.size();
          dm.emplace_back();
          load_dict(q, q.col, ch->dictionary(), cc, dm.back());
        }
        for (const fb::Page* p : *ch->pages()) {
          const uint64_t fr = p->first_row(), e = fr + p->row_count();
          if (fr >= to || e <= from) continue;
          ++st.pages_total;
          const Verdict v = is_str(q.phys) ? str_verdict(q, p, di >= 0 ? &dm[(size_t)di] : nullptr, !nulls)
                                                  : num_verdict(q, p, dbl, !nulls);
          if (v == V_SKIP) {
            ++st.pages_skipped;
            continue;
          }
          keep[i].push_back(PageRef{p, std::max(from, fr), std::min(to, e), di, v == V_ALL});
        }
      }
    }
  }

  // ---- candidate rows: the intersection of every predicate's kept pages
  void candidates() {
    const uint64_t n = to - from, nw = (n + 63) / 64;
    std::vector<std::atomic<uint64_t>> b(nw);
    bits.swap(b);
    std::vector<uint64_t> cur(nw), k(nw);
    std::fill(cur.begin(), cur.end(), ~0ull);
    if (nw && (n & 63)) cur[nw - 1] = (1ull << (n & 63)) - 1;
    auto set_range = [&](std::vector<uint64_t>& w, uint64_t a, uint64_t e) {  // bits [a, e) relative to from
      while (a < e) {
        const uint64_t wi = a / 64, bi = a & 63, m = std::min<uint64_t>(64 - bi, e - a);
        w[wi] |= (m == 64 ? ~0ull : ((1ull << m) - 1)) << bi;
        a += m;
      }
    };
    for (size_t i = 0; i < preds.size(); ++i) {
      std::fill(k.begin(), k.end(), 0ull);
      for (const PageRef& r : keep[i]) set_range(k, r.r0 - from, r.r1 - from);
      for (uint64_t w = 0; w < nw; ++w) cur[w] &= k[w];
    }
    uint64_t c = 0;
    for (uint64_t w = 0; w < nw; ++w) {
      bits[w].store(cur[w], std::memory_order_relaxed);
      c += (uint64_t)hwy::PopCount(cur[w]);
    }
    st.rows_candidate = c;
  }

  bool any_bits(uint64_t a, uint64_t e) const {  // [a, e) relative to from
    while (a < e) {
      const uint64_t wi = a / 64, bi = a & 63, m = std::min<uint64_t>(64 - bi, e - a);
      const uint64_t mask = (m == 64 ? ~0ull : ((1ull << m) - 1)) << bi;
      if (bits[wi].load(std::memory_order_relaxed) & mask) return true;
      a += m;
    }
    return false;
  }

  // a worker's scratch, leased from Kept<Scratch> for the call (kept between calls by the process, not by a
  // thread: see Kept in core_internal.h)
  struct Scratch {
    CodecContext cc;
    std::vector<uint8_t> buf, fbuf, vals, stored;
    std::vector<int32_t> lens;
    std::vector<uint64_t> words;
    void trim() {
      trim_kept(buf);
      trim_kept(fbuf);
      trim_kept(vals);
      trim_kept(stored);
      trim_kept(lens);
      trim_kept(words);
    }
  };

  // decodes one page of predicate q and ANDs its result into the bitmap
  void eval_page(const Pred& q, const PageRef& r, Scratch& S) {
    const fb::Page* p = r.p;
    const uint32_t n = p->row_count();
    const uint64_t fr = p->first_row();
    const uint64_t a = r.r0 - from, e = r.r1 - from;
    const uint64_t w0 = a / 64, w1 = (e - 1) / 64;
    S.words.assign(w1 - w0 + 1, ~0ull);
    auto clear = [&](uint64_t row) {  // row relative to from
      S.words[row / 64 - w0] &= ~(1ull << (row & 63));
    };
    try {
      const uint8_t* b = page_body_read(F, q.col, p, verify, S.cc, S.buf, S.stored);
      const size_t len = p->raw_len();
      if (q.phys == FSTX_FLOAT64) {
        if (S.vals.size() < (size_t)n * 8) S.vals.resize((size_t)n * 8);
        double* x = (double*)S.vals.data();
        enc::decode_dbl_page(b, len, p->chain(), n, x);
        if (nulls) check_nulls(p, x, n);
        for (uint64_t row = r.r0; row < r.r1; ++row)
          if (!dbl_match(q, x[row - fr])) clear(row - from);
      } else if (!is_str(q.phys)) {
        if (S.vals.size() < (size_t)n * 4) S.vals.resize((size_t)n * 4);
        int32_t* x = (int32_t*)S.vals.data();
        enc::decode_int_page(b, len, p->chain(), n, kind_of(q.phys), kNaI, x);
        if (nulls) check_nulls(p, x, n, kNaI);
        for (uint64_t row = r.r0; row < r.r1; ++row)
          if (!int_match(q, x[row - fr])) clear(row - from);
      } else if (r.dict >= 0) {
        const DictMatch& d = dm[(size_t)r.dict];
        if (S.lens.size() < n) S.lens.resize(n);
        enc::decode_int_page(b, len, p->chain(), n, enc::K_CODES, -1, S.lens.data());
        if (nulls) check_nulls(p, S.lens.data(), n, -1);
        const int32_t nd = (int32_t)d.m.size();
        const bool na_ok = q.op == FSTX_OP_ISNA || (q.op == FSTX_OP_IN && q.set_na);
        for (uint64_t row = r.r0; row < r.r1; ++row) {
          const int32_t k = S.lens[row - fr];
          if (k < -1 || k >= nd) enc::corrupt("dictionary code out of range");
          if (!(k < 0 ? na_ok : d.m[(size_t)k] != 0)) clear(row - from);
        }
      } else {
        const char* bytes = nullptr;
        enc::decode_str_page(b, len, p->chain(), n, enc::K_STR, S.lens, S.fbuf, &bytes);
        if (nulls) check_nulls(p, S.lens.data(), n, -1);
        uint64_t off = 0;
        for (uint64_t i = 0; i < r.r0 - fr; ++i)
          if (S.lens[i] > 0) off += (uint64_t)S.lens[i];
        for (uint64_t row = r.r0; row < r.r1; ++row) {
          const int32_t l = S.lens[row - fr];
          if (!str_match(q, bytes + off, l)) clear(row - from);
          if (l > 0) off += (uint64_t)l;
        }
      }
    } catch (...) {
      rethrow_for_page(F, q.col, p);
    }
    for (uint64_t w = w0; w <= w1; ++w)
      if (S.words[w - w0] != ~0ull) bits[w].fetch_and(S.words[w - w0], std::memory_order_relaxed);
  }

  void eval_pass() {
    const int hw = resolve_threads(threads);
    Lease<Scratch> scr((size_t)std::max(hw, 1));
    for (size_t i = 0; i < preds.size(); ++i) {
      const Pred& q = preds[i];
      std::vector<const PageRef*> tasks;
      for (const PageRef& r : keep[i]) {
        if (!any_bits(r.r0 - from, r.r1 - from)) {
          ++st.pages_pruned;
          continue;
        }
        if (r.all) {  // the statistics prove every row matches: nothing to clear
          ++st.pages_all;
          continue;
        }
        tasks.push_back(&r);
      }
      st.pages_read += tasks.size();
      if (tasks.empty()) continue;
      const int k = (int)std::min<size_t>((size_t)std::max(hw, 1), tasks.size());
      if (k <= 1) {
        for (const PageRef* r : tasks) eval_page(q, *r, scr[0]);
        continue;
      }
      std::atomic<size_t> next(0);
      std::mutex m;
      std::exception_ptr err;
      ThreadPool::instance().start(k, [&](int id) {
        Scratch& S = scr[(size_t)id];
        for (;;) {
          const size_t t = next.fetch_add(1);
          if (t >= tasks.size()) return;
          try {
            eval_page(q, *tasks[t], S);
          } catch (...) {
            std::lock_guard<std::mutex> g(m);
            if (!err) err = std::current_exception();
            next.store(tasks.size());
            return;
          }
        }
      });
      ThreadPool::instance().wait();
      if (err) std::rethrow_exception(err);
    }
  }

  void collect(std::vector<uint64_t>& rows) {
    const uint64_t nw = bits.size();
    uint64_t c = 0;
    for (uint64_t w = 0; w < nw; ++w) c += (uint64_t)hwy::PopCount(bits[w].load(std::memory_order_relaxed));
    rows.resize(c);
    uint64_t k = 0;
    for (uint64_t w = 0; w < nw; ++w) {
      uint64_t x = bits[w].load(std::memory_order_relaxed);
      while (x) {
        rows[k++] = from + w * 64 + (uint64_t)__builtin_ctzll(x);
        x &= x - 1;
      }
    }
    st.rows_matched = c;
  }
};

}  // namespace
}  // namespace fstx

extern "C" int fstx_filter(const fstx_file* f, const fstx_pred* preds, uint32_t n_preds, uint64_t from, uint64_t to,
                           int threads, int verify, int plan_only, fstx_selection** out, fstx_filter_stats* stats,
                           fstx_error* err) {
  fstx::set_error(err, FSTX_OK, "");
  if (out) *out = nullptr;
  fstx_selection* sel = nullptr;
  try {
    if (!f || (n_preds && !preds)) fstx::fail(FSTX_ERR_ARG, "missing file or predicates");
    if (from > to || to > f->n_rows) fstx::fail(FSTX_ERR_ARG, "row window out of range");
    fstx::Filter X(*f, from, to, threads, verify);
    X.preds.reserve(n_preds);  // Pred::sset points into Pred::strs: no moves after it is built
    for (uint32_t i = 0; i < n_preds; ++i) {
      const fstx_pred& s = preds[i];
      if (s.col >= f->n_cols) fstx::fail(FSTX_ERR_ARG, "predicate %u: column index out of range", i + 1);
      if (s.op < FSTX_OP_NONE || s.op > FSTX_OP_NOTNA) fstx::fail(FSTX_ERR_ARG, "predicate %u: unknown operator", i + 1);
      fstx::Pred q;
      q.col = s.col;
      q.op = s.op;
      q.phys = (int)f->col(s.col)->phys();
      q.v = s.value;
      q.set_na = s.set_na != 0;
      q.set_nan = s.set_nan != 0;
      if (q.phys == FSTX_INT64)  // fstx 1.2 drop 2: exact int64 filters
        fstx::fail(FSTX_ERR_ARG, "column '%s' holds 64-bit integers: filters on it are not supported yet",
                   f->col_name(s.col).c_str());
      if (fstx::is_str(q.phys)) {
        if (q.op >= FSTX_OP_LT && q.op <= FSTX_OP_GE)
          fstx::fail(FSTX_ERR_ARG, "column '%s' holds %s: only ==, !=, %%in%% and is.na() are supported",
                     f->col_name(s.col).c_str(), q.phys == FSTX_BINARY ? "binary values" : "strings");
        if ((q.op == FSTX_OP_EQ || q.op == FSTX_OP_NE) && s.n_strs != 1)
          fstx::fail(FSTX_ERR_ARG, "predicate %u: one string expected", i + 1);
        for (uint64_t k = 0; k < s.n_strs; ++k) q.strs.emplace_back(s.strs[k], (size_t)std::max(0, s.str_lens[k]));
      } else {
        if ((q.op >= FSTX_OP_EQ && q.op <= FSTX_OP_GE) && std::isnan(q.v)) q.op = FSTX_OP_NONE;
        for (uint64_t k = 0; k < s.n_set; ++k)
          if (!std::isnan(s.set[k])) q.set.push_back(s.set[k]);
        std::sort(q.set.begin(), q.set.end());
        q.set.erase(std::unique(q.set.begin(), q.set.end()), q.set.end());
      }
      X.preds.push_back(std::move(q));
      fstx::Pred& b = X.preds.back();
      if (b.op == FSTX_OP_IN)
        for (const std::string& t : b.strs) b.sset.insert(std::string_view(t));
    }
    X.st.rows_window = to - from;
    if (to == from) {
      X.st.rows_candidate = 0;
    } else {
      X.skip_pass();
      X.candidates();
    }
    sel = new fstx_selection();
    if (!plan_only && to > from) {
      X.eval_pass();
      X.collect(sel->rows);
    }
    if (stats) *stats = X.st;
    if (out) *out = sel;
    else delete sel;
    return FSTX_OK;
  } catch (...) {
    delete sel;
    return fstx::to_error(err);
  }
}

extern "C" const uint64_t* fstx_selection_rows(const fstx_selection* s, uint64_t* n) {
  *n = s ? (uint64_t)s->rows.size() : 0;
  return s && !s->rows.empty() ? s->rows.data() : nullptr;
}

extern "C" void fstx_selection_free(fstx_selection* s) { delete s; }
