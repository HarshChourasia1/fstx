// fstx fuzz target "read": the whole table through fstx_read, then the same rows again with 2 threads, with SIMD
// off, and a row window over some columns. Any two reads that succeed must agree.
#include "fuzz_common.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  if (size < fz::kCtl) return 0;
  fz::Ctl c(data);
  std::vector<uint8_t> b = fz::file_bytes(data, size, c);
  static const std::string path = fz::tmp_path("read");
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
  const int verify = c.bit(1, 0) ? 1 : 0;
  fz::Table a;
  const int ra = fz::read_range(f, 0, fi.n_rows, a, 1, verify);
  if (c.bit(1, 1)) {  // 2 threads
    fz::Table t;
    if (fz::read_range(f, 0, fi.n_rows, t, 2, verify) == FSTX_OK && ra == FSTX_OK) fz::must_equal(a, t, "threads 1 vs 2");
  }
  if (c.bit(1, 2)) {  // SIMD off
    fstx::simd::set_mode(0, 0);
    fz::Table t;
    const int rt = fz::read_range(f, 0, fi.n_rows, t, 1, verify);
    fstx::simd::set_mode(1, 0);
    if (rt == FSTX_OK && ra == FSTX_OK) fz::must_equal(a, t, "SIMD on vs off");
  }
  // a row window over a column subset (repeats allowed)
  std::vector<uint32_t> cols;
  const uint32_t k = 1 + (uint32_t)c.below(4);
  for (uint32_t i = 0; i < k && fi.n_cols; ++i) cols.push_back((uint32_t)c.below(fi.n_cols));
  const uint64_t from = c.below(fi.n_rows + 1), to = from + c.below(fi.n_rows - from + 1);
  if (!cols.empty()) {
    fz::Table w;
    if (fz::read_range(f, from, to, w, 1, verify, &cols) == FSTX_OK && ra == FSTX_OK) {
      if (w.n != to - from) fz::fail("window row count");
      for (size_t j = 0; j < cols.size(); ++j)
        for (uint64_t r = 0; r < w.n; ++r)
          if (!fz::same_cell(w, j, r, a, cols[j], from + r)) fz::fail("window differs from the whole read");
    }
  }
  fstx_close(f);
  return 0;
}
