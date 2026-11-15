// fstx fuzz target "compact": page-copy compaction (fstx_compact_copy) of a damaged file. Checks: an unknown
// optional bit is refused; the copy keeps version, profile and feature fields; the copy reads the same as the
// original whenever both reads succeed.
#include "fuzz_common.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  if (size < fz::kCtl) return 0;
  fz::Ctl c(data);
  std::vector<uint8_t> b = fz::file_bytes(data, size, c);
  static const std::string path = fz::tmp_path("compact"), out = fz::tmp_path("compact_out");
  if (!fz::write_file(path, b.data(), b.size())) return 0;
  ::unlink(out.c_str());
  fstx_error err;
  const fz::Facts before = fz::facts_of(b);
  if (fstx_compact_copy(path.c_str(), out.c_str(), &err) != FSTX_OK) return 0;
  if (before.ok && (before.opt & ~fstx::features_known())) fz::fail("compaction accepted an unknown optional bit");
  std::vector<uint8_t> nb = fz::read_file(out);
  const fz::Facts after = fz::facts_of(nb);
  if (before.ok && (!after.ok || after.version != before.version || after.profile != before.profile ||
                    after.req != before.req || after.opt != before.opt))
    fz::fail("compaction changed version, profile or feature fields");
  fstx_file *f = nullptr, *g = nullptr;
  if (fstx_open(path.c_str(), &f, &err) != FSTX_OK) fz::fail("compacted a file that does not open");
  if (fstx_open(out.c_str(), &g, &err) != FSTX_OK) fz::fail("the compacted copy does not open", err.message);
  fstx_file_info fi, gi;
  fstx_file_info_get(f, &fi);
  fstx_file_info_get(g, &gi);
  if (fi.n_rows != gi.n_rows || fi.n_cols != gi.n_cols || fi.n_pages != gi.n_pages) fz::fail("compaction changed the shape");
  if (fz::table_small(f)) {
    fz::Table a, t;
    const int ra = fz::read_range(f, 0, fi.n_rows, a, 1, 0);
    const int rt = fz::read_range(g, 0, gi.n_rows, t, 1, c.bit(1, 0) ? 1 : 0);
    if (ra == FSTX_OK && rt == FSTX_OK) fz::must_equal(a, t, "original vs compacted copy");
    if (ra == FSTX_OK && rt != FSTX_OK) fz::fail("the compacted copy fails where the original reads", "");
  }
  fstx_close(f);
  fstx_close(g);
  ::unlink(out.c_str());
  return 0;
}
