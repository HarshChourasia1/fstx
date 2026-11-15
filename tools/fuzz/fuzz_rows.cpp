// fstx fuzz target "rows": fstx_plan_create_rows with rows in any order (repeats allowed); the result must equal
// the whole read at those rows whenever both succeed.
#include <algorithm>

#include "fuzz_common.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  if (size < fz::kCtl) return 0;
  fz::Ctl c(data);
  std::vector<uint8_t> b = fz::file_bytes(data, size, c);
  static const std::string path = fz::tmp_path("rows");
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
  const int ra = fz::read_range(f, 0, fi.n_rows, a, 1, 0);
  std::vector<uint32_t> cols;
  for (uint32_t j = 0; j < fi.n_cols && j < fz::kMaxCols; ++j) cols.push_back(j);
  std::vector<uint64_t> rows;
  const uint32_t nr = (uint32_t)c.below(200);
  const bool sorted = c.bit(1, 3);
  for (uint32_t i = 0; i < nr && fi.n_rows; ++i) {
    uint64_t r = c.below(fi.n_rows);
    if (c.bit(1, 4) && !rows.empty() && (c.next() & 3) == 0) r = rows.back();  // repeats
    rows.push_back(r);
  }
  if (sorted) std::sort(rows.begin(), rows.end());
  if (c.bit(1, 5) && fi.n_rows) rows.push_back(fi.n_rows);  // one row past the end: must be refused
  fstx_plan* p = nullptr;
  const int rc = fstx_plan_create_rows(f, cols.data(), (uint32_t)cols.size(), rows.data(), rows.size(), &p, &err);
  if (rc == FSTX_OK) {
    if (c.bit(1, 5) && fi.n_rows) fz::fail("a row past the end was accepted");
    fz::Table t;
    const int rt = fz::run_plan(p, (uint32_t)cols.size(), t, c.bit(1, 1) ? 2 : 1, c.bit(1, 0) ? 1 : 0);
    if (rt == FSTX_OK && ra == FSTX_OK) {
      if (t.n != rows.size()) fz::fail("rows plan row count");
      for (size_t j = 0; j < cols.size(); ++j)
        for (uint64_t i = 0; i < t.n; ++i)
          if (!fz::same_cell(t, j, i, a, j, rows[i])) fz::fail("rows read differs from the whole read");
    }
    fstx_plan_free(p);
  }
  fstx_close(f);
  return 0;
}
