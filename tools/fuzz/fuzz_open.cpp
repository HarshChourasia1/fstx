// fstx fuzz target "open": header, tail, footer and page directory checks, and every accessor that reads them.
#include "fuzz_common.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  if (size < fz::kCtl) return 0;
  fz::Ctl c(data);
  std::vector<uint8_t> b = fz::file_bytes(data, size, c);
  static const std::string path = fz::tmp_path("open");
  if (!fz::write_file(path, b.data(), b.size())) return 0;
  uint64_t committed, physical, valid_end;
  fstx_error err;
  fstx_file_sizes(path.c_str(), &committed, &physical, &valid_end, &err);
  fstx_file* f = nullptr;
  if (fstx_open(path.c_str(), &f, &err) != FSTX_OK) {
    if (f) fz::fail("open failed but returned a file");
    if (err.code == FSTX_OK) fz::fail("open failed with code 0");
    if (!std::memchr(err.message, 0, sizeof err.message)) fz::fail("error message not terminated");
    return 0;
  }
  // format rules: an opened file has no unknown required bit; format 2 has no feature fields; format 3 profile
  // 3-255 is read as is
  const fz::Facts x = fz::facts_of(b);
  if (x.ok) {
    if (x.req & ~fstx::features_known()) fz::fail("a file with an unknown required bit was opened");
    if (x.version == 2 && (x.req || x.opt || x.n_names || x.profile > 2)) fz::fail("format 2 rule broken at open");
  }
  fstx_file_info fi;
  fstx_file_info_get(f, &fi);
  if (x.ok && fi.profile != x.profile) fz::fail("profile changed at open");
  for (uint32_t col = 0; col < fi.n_cols && col < 4096; ++col) {
    fstx_column_info ci;
    if (fstx_column_info_get(f, col, &ci) != FSTX_OK) continue;
    if (ci.name && fstx_find_column(f, ci.name, ci.name_len) < 0) fz::fail("column name not found");
    volatile size_t sink = 0;
    for (uint32_t i = 0; i < ci.n_levels && i < 4096; ++i) {
      const char* s;
      uint32_t len;
      if (fstx_level_get(f, col, i, &s, &len) == FSTX_OK && len) sink += (unsigned char)s[len - 1];
    }
    if (ci.has_tzone && ci.tzone_len) sink += (unsigned char)ci.tzone[ci.tzone_len - 1];
    if (ci.attrs_len) sink += ci.attrs[ci.attrs_len - 1];
  }
  fstx_column_info ci;
  fstx_column_info_get(f, fi.n_cols, &ci);  // out of range: must fail cleanly
  const char* s;
  uint32_t len;
  fstx_level_get(f, 0, 1u << 30, &s, &len);
  const int64_t np = fstx_pages_get(f, nullptr, 0);
  if (np < 0 || (uint64_t)np != fi.n_pages) fz::fail("page count differs");
  std::vector<fstx_page_info> pi((size_t)std::min<int64_t>(np, 1 << 16));
  fstx_pages_get(f, pi.data(), pi.size());
  for (const auto& p : pi) {
    fstx_encoding_name(p.enc[0]);
    fstx_encoding_name(p.enc[1]);
    fstx_encoding_name(p.enc[2]);
    fstx_kind_name(p.kind);
  }
  fstx_close(f);
  return 0;
}
