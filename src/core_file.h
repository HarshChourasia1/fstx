// FST-X core engine: the open-file structure shared by the reader (core_reader.cpp) and the column sources
// (core_colsrc.cpp). No R headers.
#ifndef FSTX_CORE_FILE_H
#define FSTX_CORE_FILE_H

#include <string>
#include <unordered_map>
#include <vector>

#include "core_internal.h"
#include "enc.h"
#include "fstx_footer_generated.h"

namespace fstx {
struct ColSummary {
  uint64_t comp = 0, raw = 0, pages = 0;
};
inline int width_of(int phys) { return phys == FSTX_FLOAT64 ? 8 : (phys == FSTX_STRING ? 0 : 4); }
inline int kind_of(int phys) {
  return phys == FSTX_FLOAT64 ? enc::K_DBL : phys == FSTX_LOGICAL ? enc::K_LGL : phys == FSTX_INT32 ? enc::K_INT : enc::K_STR;
}
}  // namespace fstx

struct fstx_file {
  fstx::MappedFile map;
  std::string path;
  const fstx::fb::Footer* footer = nullptr;
  uint64_t footer_off = 0;
  uint64_t size = 0;  // committed size: the file size, or the header's committed length during an append
  uint64_t n_rows = 0;
  uint32_t n_cols = 0;
  uint64_t n_pages = 0;
  uint32_t version = 0;
  std::vector<fstx::ColSummary> sums;
  int profile = 1;
  std::unordered_map<std::string, uint32_t> names;

  const fstx::fb::Column* col(uint32_t c) const { return footer->columns()->Get(c); }
  std::string col_name(uint32_t c) const {
    const fstx::fb::Column* x = col(c);
    return x->name() ? x->name()->str() : std::string();
  }
};

namespace fstx {
// Maps the file and checks header, tail, footer and page directory (core_reader.cpp).
void open_file(fstx_file& F, const std::string& path);
// The page's stored bytes, with the checksum checked when verify is set (FSTX_ERR_CHECKSUM names the column
// and rows).
const uint8_t* page_src(const fstx_file& F, uint32_t col, const fb::Page* p, bool verify);
// The page body with the codec removed (the mapped file itself when there is no codec).
const uint8_t* page_body(const fstx_file& F, uint32_t col, const fb::Page* p, bool verify, CodecContext& cc,
                         std::vector<uint8_t>& buf);
// Re-throws the current exception; a FSTX_ERR_CORRUPT one gets the column and the page's rows added.
[[noreturn]] void rethrow_for_page(const fstx_file& F, uint32_t col, const fb::Page* p);
// Throws FSTX_ERR_CORRUPT if a decoded string run holds a NUL byte (R strings cannot).
void check_no_nul(const char* bytes, const int32_t* lens, uint32_t n);
// FSTX_VERIFY_NULLS: throws FSTX_ERR_CORRUPT unless the whole decoded page (n values) holds exactly the page's
// null_count missing values. na is the int32 pattern: NA_integer_, or -1 for code pages and string lengths.
// Doubles count R's NA_real_ as the writer does: a NaN whose low word is 1954.
void check_nulls(const fb::Page* p, const int32_t* x, uint32_t n, int32_t na);
void check_nulls(const fb::Page* p, const double* x, uint32_t n);

// Tests only: copies src to dst through compaction with fields overridden, to make files a newer fstx could
// write (unknown feature bits, reserved numbers). -1 = keep the field. Page fields act on column 0's first page.
struct Forge {
  int hdr_flags = -1, version = -1, phys0 = -1, logical0 = -1, codec0 = -1, kind0 = -1, tag0 = -1, na0 = -1,
      profile = -1, nulls0 = -1;
  uint64_t req = 0, opt = 0;
  std::vector<std::string> names;
};
void forge_copy(const std::string& src, const std::string& dst, const Forge& f);
}  // namespace fstx

#endif
