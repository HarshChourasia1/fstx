// fstx fuzz target "filter": fstx_filter with 1-3 predicates built from the table's own values. The selection
// must be ascending and inside the window. When every page of the predicate columns has statistics that are true
// for its decoded values (min / max, NA count), the selection must equal a row-by-row check of the whole read.
#include <algorithm>
#include <limits>

#include "fuzz_common.h"

namespace {

const int32_t kNaI = std::numeric_limits<int32_t>::min();
bool r_na_real(double x) {
  uint64_t b;
  std::memcpy(&b, &x, 8);
  return std::isnan(x) && (uint32_t)(b & 0xffffffffu) == 1954u;
}

struct P {
  fstx_pred p{};
  std::vector<double> set;
  std::vector<int64_t> iset;
  std::vector<std::string> strs;
  std::vector<const char*> sp;
  std::vector<int32_t> sl;
};

bool num_match(int op, double x, double v, const std::vector<double>& set) {
  switch (op) {
    case FSTX_OP_EQ: return x == v;
    case FSTX_OP_NE: return x != v;
    case FSTX_OP_LT: return x < v;
    case FSTX_OP_LE: return x <= v;
    case FSTX_OP_GT: return x > v;
    case FSTX_OP_GE: return x >= v;
    case FSTX_OP_IN:
      for (double s : set)
        if (!std::isnan(s) && s == x) return true;
      return false;
    case FSTX_OP_NOTNA: return true;
    default: return false;
  }
}

// int64 rows (fstx 1.2): exact, against an int64 literal / set (has_i64) or a double compared as a real number
bool match_i64(const P& q, int64_t x) {
  const int op = q.p.op;
  if (x == INT64_MIN) return op == FSTX_OP_ISNA || (op == FSTX_OP_IN && q.p.set_na);
  if (op == FSTX_OP_NOTNA) return true;
  if (op == FSTX_OP_IN) {
    if (q.p.has_i64) return std::find(q.iset.begin(), q.iset.end(), x) != q.iset.end();
    for (double d : q.set)
      if (!std::isnan(d) && fstx::cmp_d_i64(d, x) == 0) return true;
    return false;
  }
  if (op < FSTX_OP_EQ || op > FSTX_OP_GE) return false;
  if (q.p.has_i64 ? q.p.i64 == INT64_MIN : std::isnan(q.p.value)) return false;
  const int k = q.p.has_i64 ? (x < q.p.i64 ? -1 : x > q.p.i64 ? 1 : 0) : -fstx::cmp_d_i64(q.p.value, x);
  switch (op) {
    case FSTX_OP_EQ: return k == 0;
    case FSTX_OP_NE: return k != 0;
    case FSTX_OP_LT: return k < 0;
    case FSTX_OP_LE: return k <= 0;
    case FSTX_OP_GT: return k > 0;
    default: return k >= 0;
  }
}

bool match(const P& q, const fz::Col& c, uint64_t r) {
  const int op = q.p.op;
  if (c.phys == FSTX_INT64) return match_i64(q, c.l[r]);
  if (fz::is_strlike(c.phys)) {
    if (c.na[r] == 1) return op == FSTX_OP_ISNA || (op == FSTX_OP_IN && q.p.set_na);
    const std::string& s = c.s[r];
    switch (op) {
      case FSTX_OP_NOTNA: return true;
      case FSTX_OP_EQ: return s == q.strs[0];
      case FSTX_OP_NE: return s != q.strs[0];
      case FSTX_OP_IN: return std::find(q.strs.begin(), q.strs.end(), s) != q.strs.end();
      default: return false;
    }
  }
  if (c.phys == FSTX_FLOAT64) {
    const double x = c.d[r];
    if (std::isnan(x)) {
      if (op == FSTX_OP_ISNA) return true;
      if (op == FSTX_OP_IN) return r_na_real(x) ? q.p.set_na : q.p.set_nan;
      return false;
    }
    if (op == FSTX_OP_ISNA) return false;
    if (op >= FSTX_OP_EQ && op <= FSTX_OP_GE && std::isnan(q.p.value)) return false;
    return num_match(op, x, q.p.value, q.set);
  }
  const int32_t x = c.i[r];
  if (x == kNaI) return op == FSTX_OP_ISNA || (op == FSTX_OP_IN && q.p.set_na);
  if (op == FSTX_OP_ISNA) return false;
  if (op >= FSTX_OP_EQ && op <= FSTX_OP_GE && std::isnan(q.p.value)) return false;
  return num_match(op, (double)x, q.p.value, q.set);
}

// every page of column col: footer statistics true for the decoded values?
bool stats_true(const std::vector<uint8_t>& b, uint32_t col, const fz::Col& c, uint64_t n_rows) {
  const auto* ft = fz::footer_of(b.data(), b.size());
  if (!ft) return false;
  bool ok = true;
  fz::each_page(ft, [&](uint32_t cc, const fstx::fb::Page* p, bool dict) {
    if (cc != col || dict || !ok) return;
    const uint64_t r0 = p->first_row(), n = p->row_count();
    if (r0 > n_rows || n > n_rows - r0) {
      ok = false;
      return;
    }
    const bool mm = (p->flags() & 1) != 0;
    uint64_t na = 0;
    bool any = false;
    for (uint64_t r = r0; r < r0 + n; ++r) {
      double x;
      if (c.phys == FSTX_INT64) {  // rounded outward: min <= x <= max as real numbers
        const int64_t v = c.l[r];
        if (v == INT64_MIN) {
          ++na;
          continue;
        }
        any = true;
        if (mm && (std::isnan(p->min()) || std::isnan(p->max()) || fstx::cmp_d_i64(p->min(), v) > 0 ||
                   fstx::cmp_d_i64(p->max(), v) < 0))
          ok = false;
        continue;
      }
      if (fz::is_strlike(c.phys)) {
        if (c.na[r] == 1) ++na;
        continue;
      }
      if (c.phys == FSTX_FLOAT64) {
        x = c.d[r];
        if (r_na_real(x)) ++na;
        if (std::isnan(x)) continue;
      } else {
        if (c.i[r] == kNaI) {
          ++na;
          continue;
        }
        x = c.i[r];
      }
      any = true;
      if (mm && !(x >= p->min() && x <= p->max())) ok = false;
    }
    if (na != p->null_count() || (any && !mm)) ok = false;
  });
  return ok;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  if (size < fz::kCtl) return 0;
  fz::Ctl c(data);
  std::vector<uint8_t> b = fz::file_bytes(data, size, c);
  static const std::string path = fz::tmp_path("filter");
  if (!fz::write_file(path, b.data(), b.size())) return 0;
  fstx_file* f = nullptr;
  fstx_error err;
  if (fstx_open(path.c_str(), &f, &err) != FSTX_OK) return 0;
  if (!fz::table_small(f)) {
    fstx_close(f);
    return 0;
  }
  fstx_file_info fi;
  fstx_file_info_get(f, &fi);
  if (fi.n_cols == 0) {
    fstx_close(f);
    return 0;
  }
  fz::Table a;
  const bool have_a = fz::read_range(f, 0, fi.n_rows, a, 1, 0) == FSTX_OK;
  const uint32_t np = 1 + (uint32_t)c.below(3);
  std::vector<P> ps(np);
  for (P& q : ps) {
    const uint32_t col = (uint32_t)c.below(std::min<uint32_t>(fi.n_cols, fz::kMaxCols));
    fstx_column_info ci;
    fstx_column_info_get(f, col, &ci);
    q.p.col = col;
    q.p.op = (int)c.below(10);
    // a value from the table (or NaN / a random one)
    double v = (double)(int32_t)c.next();
    int64_t iv = (int64_t)((uint64_t)c.next() << 32 | c.next());
    std::string sv = "x";
    if (have_a && a.n) {
      const uint64_t r = c.below(a.n);
      const fz::Col& ac = a.cols[col];
      if (ac.phys == FSTX_FLOAT64) v = ac.d[r];
      else if (fz::is_strlike(ac.phys)) sv = ac.na[r] == 1 ? std::string() : ac.s[r];
      else if (ac.phys == FSTX_INT64) {
        iv = ac.l[r];
        v = iv == INT64_MIN ? NAN : (double)iv;  // rounded: may sit between two rows' values
      } else v = ac.i[r] == kNaI ? NAN : ac.i[r];
    }
    if ((c.next() & 7) == 0) v = NAN;
    if (ci.phys == FSTX_INT64) {  // an int64 literal (exact) half the time, near the picked value
      q.p.has_i64 = (int)(c.next() & 1);
      const uint32_t z = c.next() & 15;
      if (z == 1) iv = INT64_MIN;
      else if (z == 2 && iv < INT64_MAX) ++iv;
      else if (z == 3 && iv > INT64_MIN + 1) --iv;
      q.p.i64 = iv;
      q.iset = {iv, iv ^ 1, INT64_MIN, INT64_MAX};
      q.p.i64_set = q.iset.data();
      q.p.n_i64_set = q.iset.size();
      if ((c.next() & 3) == 0) v += 0.5;
    }
    q.p.value = v;
    q.p.set_na = (int)(c.next() & 1);
    q.p.set_nan = (int)(c.next() & 1);
    q.set = {v, v + 1, -v};
    q.p.set = q.set.data();
    q.p.n_set = q.set.size();
    q.strs = {sv, sv + "a", std::string()};
    if (fz::is_strlike(ci.phys) && (q.p.op == FSTX_OP_EQ || q.p.op == FSTX_OP_NE)) q.strs.resize(1);
    for (const auto& s : q.strs) {
      q.sp.push_back(s.data());
      q.sl.push_back((int32_t)s.size());
    }
    q.p.strs = q.sp.data();
    q.p.str_lens = q.sl.data();
    q.p.n_strs = q.strs.size();
  }
  std::vector<fstx_pred> pp;
  for (const P& q : ps) pp.push_back(q.p);
  const uint64_t from = c.bit(3, 0) ? c.below(fi.n_rows + 1) : 0;
  const uint64_t to = c.bit(3, 1) ? from + c.below(fi.n_rows - from + 1) : fi.n_rows;
  const int plan_only = c.bit(3, 2) ? 1 : 0;
  fstx_selection* sel = nullptr;
  fstx_filter_stats st;
  const int rc = fstx_filter(f, pp.data(), (uint32_t)pp.size(), from, to, c.bit(3, 3) ? 2 : 1, c.bit(1, 0) ? 1 : 0,
                             plan_only, &sel, &st, &err);
  if (rc == FSTX_OK) {
    uint64_t n = 0;
    const uint64_t* rows = fstx_selection_rows(sel, &n);
    if (n != st.rows_matched) fz::fail("filter count differs from its stats");
    for (uint64_t i = 0; i < n; ++i) {
      if (rows[i] < from || rows[i] >= to) fz::fail("filter row outside the window");
      if (i && rows[i] <= rows[i - 1]) fz::fail("filter rows not ascending");
    }
    if (plan_only && n) fz::fail("plan_only returned rows");
    bool strict = have_a && !plan_only;
    for (const P& q : ps)
      if (strict) strict = stats_true(b, q.p.col, a.cols[q.p.col], a.n);
    if (strict) {
      uint64_t k = 0;
      for (uint64_t r = from; r < to; ++r) {
        bool m = true;
        for (const P& q : ps) m = m && match(q, a.cols[q.p.col], r);
        if (m) {
          if (k >= n || rows[k] != r) fz::fail("filter differs from a row-by-row check");
          ++k;
        }
      }
      if (k != n) fz::fail("filter returned rows a row-by-row check does not");
    }
    fstx_selection_free(sel);
  } else if (sel) {
    fz::fail("filter failed but returned a selection");
  }
  fstx_close(f);
  return 0;
}
