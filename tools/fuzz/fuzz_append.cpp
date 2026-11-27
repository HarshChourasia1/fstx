// fstx fuzz target "append": fstx_writer_open_append on a damaged file with its own column definitions, then a few
// synthetic rows. Checks: an unknown optional bit is refused; a finished append keeps version, profile and feature
// fields, the old rows read as before and the new rows read as written; an interrupted (test_crash) or aborted
// append leaves the old table.
#include <limits>

#include "fuzz_common.h"

namespace {

const int32_t kNaI = std::numeric_limits<int32_t>::min();

struct NewCol {
  int phys = 0;
  std::vector<int32_t> i;
  std::vector<double> d;
  std::vector<int64_t> l;
  std::vector<std::string> s;
  std::vector<uint8_t> na;
};

int get_strings(void* ctx, uint64_t row, uint32_t n, const char** ptrs, int32_t* lens) {
  const NewCol* c = (const NewCol*)ctx;
  for (uint32_t k = 0; k < n; ++k) {
    const uint64_t r = row + k;
    ptrs[k] = c->s[r].data();
    lens[k] = c->na[r] ? -1 : (int32_t)c->s[r].size();
  }
  return 0;
}

double dbl_bits(uint64_t b) {
  double x;
  std::memcpy(&x, &b, 8);
  return x;
}

struct Def {
  std::string name, tz;
  int phys = 0, logical = 0, ordered = 0, has_tz = 0;
  std::vector<std::string> levels;
  std::vector<uint8_t> attrs;
};

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  if (size < fz::kCtl) return 0;
  fz::Ctl c(data);
  std::vector<uint8_t> b = fz::file_bytes(data, size, c);
  static const std::string path = fz::tmp_path("append");
  if (!fz::write_file(path, b.data(), b.size())) return 0;
  fstx_file* f = nullptr;
  fstx_error err;
  if (fstx_open(path.c_str(), &f, &err) != FSTX_OK) return 0;
  fstx_file_info fi;
  fstx_file_info_get(f, &fi);
  if (!fz::table_small(f) || fi.n_cols == 0 || fi.n_cols > 16) {
    fstx_close(f);
    return 0;
  }
  // the file's own column definitions (copied: the file is closed before the append)
  std::vector<Def> defs(fi.n_cols);
  for (uint32_t j = 0; j < fi.n_cols; ++j) {
    fstx_column_info ci;
    fstx_column_info_get(f, j, &ci);
    Def& d = defs[j];
    d.name.assign(ci.name, ci.name_len);
    d.phys = ci.phys;
    d.logical = ci.logical;
    d.ordered = ci.ordered;
    d.has_tz = ci.has_tzone;
    if (ci.has_tzone) d.tz.assign(ci.tzone, ci.tzone_len);
    if (ci.attrs_len) d.attrs.assign(ci.attrs, ci.attrs + ci.attrs_len);
    for (uint32_t i = 0; i < ci.n_levels && i < 100000; ++i) {
      const char* s;
      uint32_t len;
      if (fstx_level_get(f, j, i, &s, &len) == FSTX_OK) d.levels.emplace_back(s, len);
    }
  }
  std::vector<uint8_t> tattrs(fi.table_attrs, fi.table_attrs + fi.table_attrs_len);
  fz::Table a;
  const bool have_a = fz::read_range(f, 0, fi.n_rows, a, 1, 0) == FSTX_OK;
  const uint64_t n_old = fi.n_rows;
  fstx_close(f);
  const fz::Facts before = fz::facts_of(b);

  // new rows
  const uint64_t m = 1 + c.below(2100);
  std::vector<NewCol> nc(defs.size());
  for (size_t j = 0; j < defs.size(); ++j) {
    NewCol& x = nc[j];
    x.phys = defs[j].phys;
    for (uint64_t r = 0; r < m; ++r) {
      const uint32_t z = c.next();
      const bool na = (z & 15) == 0;
      switch (x.phys) {
        case FSTX_FLOAT64:
          x.d.push_back(na ? dbl_bits(0x7FF00000000007A2ull)
                           : (z & 31) == 1 ? dbl_bits(0x7FF8000000000000ull) : (double)(int32_t)(z >> 8) / 100.0);
          break;
        case FSTX_STRING:
          x.na.push_back(na ? 1 : 0);
          x.s.push_back(na ? std::string() : "v" + std::to_string(z % 97));
          break;
        case FSTX_BINARY: {  // any bytes, NUL included, some repeated (dictionary candidates)
          x.na.push_back(na ? 1 : 0);
          std::string v;
          if (!na)
            for (uint32_t k = 0, len = (z >> 4) % 9; k < len; ++k) v.push_back((char)(k ? c.next() : z >> 8 & 3));
          x.s.push_back(v);
          break;
        }
        case FSTX_INT64: {  // small values, values past 2^53 and the extremes
          const uint64_t hi = c.next();
          const int64_t v = (z & 7) == 1 ? INT64_MAX - (int64_t)(hi & 7)
                            : (z & 7) == 2 ? INT64_MIN + 1 + (int64_t)(hi & 7)
                            : (z & 7) == 3 ? (int64_t)(hi << 32 | z)
                                           : (int64_t)(z % 2001) - 1000;
          x.l.push_back(na ? INT64_MIN : v);
          break;
        }
        case FSTX_LOGICAL: x.i.push_back(na ? kNaI : (int32_t)(z >> 4 & 1)); break;
        default:
          if (defs[j].logical == FSTX_LT_FACTOR)
            x.i.push_back(na || defs[j].levels.empty() ? kNaI : 1 + (int32_t)(z % defs[j].levels.size()));
          else
            x.i.push_back(na ? kNaI : (int32_t)(z % 2001) - 1000);
      }
    }
  }
  std::vector<std::vector<const char*>> lv(defs.size());
  std::vector<std::vector<uint32_t>> ll(defs.size());
  std::vector<fstx_write_column> wc(defs.size());
  std::vector<fstx_column_data> cd(defs.size());
  for (size_t j = 0; j < defs.size(); ++j) {
    const Def& d = defs[j];
    for (const auto& s : d.levels) {
      lv[j].push_back(s.data());
      ll[j].push_back((uint32_t)s.size());
    }
    fstx_write_column& w = wc[j];
    std::memset(&w, 0, sizeof w);
    w.name = d.name.data();
    w.name_len = (uint32_t)d.name.size();
    w.phys = d.phys;
    w.logical = d.logical;
    w.ordered = d.ordered;
    w.n_levels = (uint32_t)d.levels.size();
    w.levels = lv[j].data();
    w.level_lens = ll[j].data();
    w.has_tzone = d.has_tz;
    w.tzone = d.tz.data();
    w.tzone_len = (uint32_t)d.tz.size();
    w.attrs = d.attrs.empty() ? nullptr : d.attrs.data();
    w.attrs_len = d.attrs.size();
    NewCol& x = nc[j];
    cd[j].data = x.phys == FSTX_FLOAT64 ? (const void*)x.d.data()
                 : x.phys == FSTX_INT64 ? (const void*)x.l.data()
                 : fz::is_strlike(x.phys) ? nullptr : (const void*)x.i.data();
    cd[j].get_strings = get_strings;
    cd[j].ctx = &x;
  }
  fstx_write_options o;
  std::memset(&o, 0, sizeof o);
  o.codec = (int)c.below(3);
  o.level = 1;
  o.threads = c.bit(4, 0) ? 2 : 1;
  o.profile = 1;
  o.keep_pct = 10;
  static const int forced[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 6 | 256, 6 | 512, 1 | 1024};
  o.force_encoding = c.bit(4, 1) ? forced[c.below(sizeof forced / sizeof forced[0])] : 0;
  const int mode = (int)c.below(4);  // 0, 1 = finish; 2 = test_crash; 3 = abort
  o.test_crash = mode == 2 ? 1 : 0;
  fstx_writer* w = nullptr;
  int rc = fstx_writer_open_append(path.c_str(), wc.data(), (uint32_t)defs.size(),
                                   tattrs.empty() ? nullptr : tattrs.data(), tattrs.size(), &o, &w, &err);
  if (rc == FSTX_OK && before.ok && (before.opt & ~fstx::features_known()))
    fz::fail("append accepted a file with an unknown optional bit");
  if (rc != FSTX_OK) return 0;
  rc = fstx_writer_append(w, cd.data(), m, &err);
  if (mode == 3) {
    fstx_writer_abort(w);
  } else {
    const int rc2 = fstx_writer_close(w, &err);
    if (rc == FSTX_OK) rc = rc2;
  }
  // read back
  std::vector<uint8_t> nb = fz::read_file(path);
  if (fstx_open(path.c_str(), &f, &err) != FSTX_OK) {
    if (have_a) fz::fail("file unreadable after an append to a readable file", err.message);
    return 0;
  }
  fstx_file_info_get(f, &fi);
  const bool finished = rc == FSTX_OK && mode < 2;
  const fz::Facts after = fz::facts_of(nb);
  if (before.ok && after.ok &&
      (after.version != before.version || after.profile != before.profile || after.req != before.req ||
       after.opt != before.opt))
    fz::fail("append changed version, profile or feature fields");
  if (have_a && fi.n_rows != n_old + (finished ? m : 0)) fz::fail("row count after append");
  fz::Table t;
  if (fz::read_range(f, 0, fi.n_rows, t, 1, 0) == FSTX_OK) {
    if (have_a)
      for (size_t j = 0; j < a.cols.size(); ++j)
        for (uint64_t r = 0; r < n_old && r < t.n; ++r)
          if (!fz::same_cell(a, j, r, t, j, r)) fz::fail("old rows changed by an append");
    if (finished && have_a)
      for (size_t j = 0; j < defs.size() && j < t.cols.size(); ++j) {
        const fz::Col& tc = t.cols[j];
        const NewCol& x = nc[j];
        for (uint64_t r = 0; r < m; ++r) {
          const uint64_t q = n_old + r;
          bool ok;
          if (x.phys == FSTX_FLOAT64) ok = fz::same_dbl(tc.d[q], x.d[r]);
          else if (x.phys == FSTX_INT64) ok = tc.l[q] == x.l[r];
          else if (fz::is_strlike(x.phys)) ok = (tc.na[q] == 1) == (x.na[r] == 1) && (x.na[r] || tc.s[q] == x.s[r]);
          else ok = tc.i[q] == x.i[r];
          if (!ok) fz::fail("appended rows read back differently");
        }
      }
  } else if (finished && have_a) {
    fz::fail("table unreadable after an append to a readable table");
  }
  fstx_close(f);
  return 0;
}
