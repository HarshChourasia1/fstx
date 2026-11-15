// fstx fuzz target "colsrc": column sources (the lazy columns), page by page and loaded in one go, string sources
// turned into factor codes, and detached sources. Every value that comes back must equal the whole read.
#include "fuzz_common.h"

namespace {

void check_fixed(const void* vals, int phys, const fz::Table& a, uint32_t col, uint64_t row0, uint32_t n) {
  for (uint32_t i = 0; i < n; ++i) {
    const uint64_t r = row0 + i;
    if (phys == FSTX_FLOAT64) {
      if (!fz::same_dbl(((const double*)vals)[i], a.cols[col].d[r])) fz::fail("colsrc double differs");
    } else if (((const int32_t*)vals)[i] != a.cols[col].i[r]) {
      fz::fail("colsrc int differs");
    }
  }
}

void one_column(fstx_file* f, uint32_t col, const fz::Table& a, bool have_a, uint64_t from, uint64_t to, fz::Ctl& c) {
  fstx_colsrc* s = nullptr;
  fstx_error err;
  if (fstx_colsrc_open(f, col, from, to, c.bit(1, 0) ? 1 : 0, c.bit(2, 0) ? 1 : 0, &s, &err) != FSTX_OK) return;
  fstx_colsrc_info in;
  fstx_colsrc_info_get(s, &in);
  if (in.rows != to - from) fz::fail("colsrc rows");
  if (c.bit(2, 1)) fstx_colsrc_set_threads(s, 2);
  for (uint32_t k = 0; k < 4 && in.rows; ++k) {  // page_of must give a page that holds the row
    const uint64_t r = c.below(in.rows);
    const uint32_t p = fstx_colsrc_page_of(s, r);
    uint64_t first;
    uint32_t n;
    fstx_colsrc_page_span(s, p, &first, &n);
    if (r < first || r >= first + n) fz::fail("colsrc page_of");
  }
  std::vector<std::vector<std::string>> dicts(in.n_dicts);
  std::vector<uint8_t> dict_ok(in.n_dicts, 0);
  for (uint32_t p = 0; p < in.n_pages; ++p) {
    uint64_t first;
    uint32_t n;
    fstx_colsrc_page_span(s, p, &first, &n);
    if (in.mode == FSTX_SRC_FIXED) {
      const void* vals;
      if (fstx_colsrc_fixed_page(s, p, &vals, &err) == FSTX_OK && have_a) check_fixed(vals, in.phys, a, col, from + first, n);
    } else {
      const int32_t* lens;
      const char* bytes;
      int32_t d;
      if (fstx_colsrc_str_page(s, p, &lens, &bytes, &d, &err) != FSTX_OK) continue;
      if (d >= 0) {
        if ((uint32_t)d >= in.n_dicts) fz::fail("colsrc dictionary index out of range");
        if (!dict_ok[d]) {
          uint32_t dn;
          const int32_t* dl;
          const char* db;
          if (fstx_colsrc_dict(s, (uint32_t)d, &dn, &dl, &db, &err) != FSTX_OK) continue;
          for (uint32_t i = 0; i < dn; ++i) {
            if (dl[i] < 0) fz::fail("NA in a colsrc dictionary");
            dicts[d].emplace_back(db, (size_t)dl[i]);
            db += dl[i];
          }
          dict_ok[d] = 1;
        }
        for (uint32_t i = 0; i < n; ++i) {
          if (lens[i] < -1 || lens[i] >= (int32_t)dicts[d].size()) fz::fail("colsrc code out of range");
          if (!have_a) continue;
          const uint64_t r = from + first + i;
          const fz::Col& ac = a.cols[col];
          if ((lens[i] < 0) != (ac.na[r] == 1) || (lens[i] >= 0 && dicts[d][lens[i]] != ac.s[r]))
            fz::fail("colsrc dictionary string differs");
        }
      } else {
        for (uint32_t i = 0; i < n; ++i) {
          if (lens[i] < -1) fz::fail("colsrc string length below -1");
          if (have_a) {
            const uint64_t r = from + first + i;
            const fz::Col& ac = a.cols[col];
            if ((lens[i] < 0) != (ac.na[r] == 1) ||
                (lens[i] >= 0 && (ac.s[r].size() != (size_t)lens[i] || std::memcmp(ac.s[r].data(), bytes, lens[i]))))
              fz::fail("colsrc string differs");
          }
          if (lens[i] > 0) bytes += lens[i];
        }
      }
    }
    if (c.bit(2, 2) && (c.next() & 1)) fstx_colsrc_drop(s, p);
  }
  // load all pages at once
  const int threads = c.bit(2, 3) ? 2 : 1;
  if (in.mode == FSTX_SRC_FIXED) {
    std::vector<uint8_t> dst((size_t)in.rows * (in.phys == FSTX_FLOAT64 ? 8 : 4));
    if (fstx_colsrc_load(s, dst.data(), threads, &err) == FSTX_OK && have_a) check_fixed(dst.data(), in.phys, a, col, from, (uint32_t)in.rows);
  } else if (fstx_colsrc_load(s, nullptr, threads, &err) == FSTX_OK) {
    if (c.bit(2, 4)) fstx_colsrc_detach(s, &err);
    if (in.all_dict && c.bit(2, 5)) {  // factor codes: entry k of dictionary d becomes level base[d] + k + 1
      std::vector<std::vector<int32_t>> remap(in.n_dicts);
      std::vector<const int32_t*> rp(in.n_dicts);
      std::vector<std::string> levels;
      bool ok = true;
      for (uint32_t d = 0; d < in.n_dicts && ok; ++d) {
        uint32_t dn;
        const int32_t* dl;
        const char* db;
        if (fstx_colsrc_dict(s, d, &dn, &dl, &db, &err) != FSTX_OK) {
          ok = false;
          break;
        }
        for (uint32_t i = 0; i < dn; ++i) {
          remap[d].push_back((int32_t)levels.size() + 1);
          levels.emplace_back(db, (size_t)dl[i]);
          db += dl[i];
        }
        rp[d] = remap[d].data();
      }
      if (ok && fstx_colsrc_set_codes(s, rp.data(), &err) == FSTX_OK) {
        std::vector<int32_t> codes(in.rows);
        if (fstx_colsrc_load(s, codes.data(), threads, &err) == FSTX_OK && have_a) {
          const fz::Col& ac = a.cols[col];
          for (uint64_t r = 0; r < in.rows; ++r) {
            const int32_t k = codes[r];
            const bool na = k == INT32_MIN;
            if (!na && (k < 1 || (size_t)k > levels.size())) fz::fail("colsrc factor code out of range");
            if (na != (ac.na[from + r] == 1) || (!na && levels[(size_t)k - 1] != ac.s[from + r]))
              fz::fail("colsrc factor code differs");
          }
        }
      }
    }
  }
  fstx_colsrc_free(s);
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  if (size < fz::kCtl) return 0;
  fz::Ctl c(data);
  std::vector<uint8_t> b = fz::file_bytes(data, size, c);
  static const std::string path = fz::tmp_path("colsrc");
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
  fz::Table a;
  const bool have_a = fz::read_range(f, 0, fi.n_rows, a, 1, 0) == FSTX_OK;
  const uint64_t from = c.bit(2, 6) ? c.below(fi.n_rows + 1) : 0;
  const uint64_t to = c.bit(2, 7) ? from + c.below(fi.n_rows - from + 1) : fi.n_rows;
  for (uint32_t col = 0; col < fi.n_cols && col < 16; ++col) one_column(f, col, a, have_a, from, to, c);
  fstx_close(f);
  return 0;
}
