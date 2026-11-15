// FST-X R layer: column holders and the ALTREP classes over them (r_lazy.cpp), used by r_glue.cpp.
#ifndef FSTX_R_LAZY_H
#define FSTX_R_LAZY_H

#include <cpp11.hpp>
#include <R_ext/Rdynload.h>

#include <memory>
#include <vector>

#include "fstx_core.h"

struct FileCloser {
  void operator()(fstx_file* f) const { fstx_close(f); }
};
// An open file shared by the lazy columns that read from it; the last one to go closes it.
using FileRef = std::shared_ptr<fstx_file>;

// One column's engine source. Owned by an R external pointer (its finaliser deletes it).
struct Holder {
  FileRef file;                // empty once the source is detached or released
  fstx_colsrc* src = nullptr;  // null once the column is fully in R memory
  int threads = 1;
  uint64_t rows = 0;
  uint64_t nulls = 0;
  int phys = FSTX_INT32;
  int mode = FSTX_SRC_FIXED;
  bool lazy = false;
  // M6s strings: R strings go into one full STRSXP page by page (the first page used alone goes into its own
  // small vector, so touching one element of a huge column does not allocate the whole column).
  bool portable = false;            // serialise in R's standard format (strings = "auto"), not the compact state
  bool s_done = false;              // every page made: the full vector is the column
  bool c_in_full = false;           // the cached span below lives in the full vector (else in the first-page vector)
  uint32_t s_made = 0;              // pages made into the full vector
  uint32_t p0 = UINT32_MAX;         // the page made alone into its own vector, if any
  std::vector<uint8_t> made;        // per page: made into the full vector
  const SEXP* s_fp = nullptr;       // M6.1: the full vector's data once s_done (Elt without further R calls)
  // M6.1: per page not made yet, the byte offset of each row in its decoded bytes (built on the first small subset
  // that needs it, kept until the page is made), so x[i] in a loop does not rescan the page on every call
  std::vector<std::vector<uint64_t>> offs;
  // the last page used (element access walks pages in order): rows [c_first, c_first + c_n)
  uint64_t c_first = 0;
  uint32_t c_n = 0;
  const void* c_vals = nullptr;
  void release() {
    c_n = 0;
    c_vals = nullptr;
    std::vector<uint8_t>().swap(made);
    std::vector<std::vector<uint64_t>>().swap(offs);
    if (src) fstx_colsrc_free(src);
    src = nullptr;
    file.reset();
  }
  ~Holder() { release(); }
};

// An external pointer with a finaliser and no address yet (R allocation: call through cpp11::safe).
SEXP fstx_holder_xp();
// Fills the holder's row / NA / mode fields from its source.
void fstx_holder_sync(Holder* h);
Holder* fstx_holder_of(SEXP xp);
// The ALTREP vector over xp's holder (string, integer, double or logical by the source). R allocation.
SEXP fstx_altrep_new(SEXP xp);
// M6s: makes every R string of an fstx string ALTREP vector and returns the plain STRSXP that holds them. The work
// runs inside R's Dataptr call, where R switches the garbage collector off. R allocation.
SEXP fstx_str_plain(SEXP x);
// 1.0: the plain STRSXP of an all-dictionary string column whose pages are all decoded (one R string per dictionary
// entry, a pointer per row). R allocation; raises R errors.
SEXP fstx_dict_plain(SEXP xp);
void fstx_init_altrep(DllInfo* dll);

#endif
