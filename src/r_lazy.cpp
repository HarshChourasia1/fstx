// FST-X R layer: ALTREP columns. Strings ("deferred": decoded in parallel at read time, R strings made per page
// on first use) and lazy columns of every type (pages decoded from the memory-mapped file on first use).
//
// The ALTREP methods are called by R directly, outside any cpp11 frame: they hold only plain-data locals, call
// the engine through its C interface (which never throws) and raise R errors with Rf_error, so a longjmp never
// skips a C++ destructor.
#include "r_lazy.h"

#include <algorithm>
#include <climits>
#include <cstring>
#include <limits>
#include <new>
#include <vector>

extern "C" {
#include <R_ext/Altrep.h>
}

namespace {

R_altrep_class_t cls_str, cls_int, cls_dbl, cls_lgl;
R_altrep_class_t cls_view;  // 1.0: rows of an unfinished string column, R strings made on first use

void holder_finalize(SEXP xp) {
  Holder* h = (Holder*)R_ExternalPtrAddr(xp);
  if (h) {
    R_ClearExternalPtr(xp);
    delete h;
  }
}

inline Holder* hold(SEXP x) { return (Holder*)R_ExternalPtrAddr(R_altrep_data1(x)); }

[[noreturn]] void raise_err(const fstx_error* e) { Rf_error("fstx: %s", e->message); }

// ---------------------------------------------------------------- strings
// data2 = list(full STRSXP or NULL, first-page STRSXP or NULL, dictionary cache: list of STRSXP)
//
// M6s: R strings are made page by page straight into one full STRSXP, which becomes the column once every page
// is made (no per-page vectors copied at the end). The first page used is made alone into its own small vector,
// so head() or one element of a huge column does not allocate the whole column. R calls Elt and Dataptr with
// its garbage collector switched off, so making strings here triggers no collection (the heap just grows);
// that is most of the speed-up over making the same strings in a .Call, where every few MB start a collection
// that walks R's whole string cache.
inline SEXP st_full(SEXP x) { return VECTOR_ELT(R_altrep_data2(x), 0); }
inline SEXP st_p0(SEXP x) { return VECTOR_ELT(R_altrep_data2(x), 1); }

SEXP str_dict(SEXP x, Holder* h, int32_t d) {
  SEXP dc = VECTOR_ELT(R_altrep_data2(x), 2);
  SEXP v = VECTOR_ELT(dc, d);
  if (v != R_NilValue) return v;
  uint32_t n;
  const int32_t* lens;
  const char* bytes;
  fstx_error err;
  if (fstx_colsrc_dict(h->src, (uint32_t)d, &n, &lens, &bytes, &err) != FSTX_OK) raise_err(&err);
  v = PROTECT(Rf_allocVector(STRSXP, (R_xlen_t)n));
  for (uint32_t i = 0; i < n; ++i) {
    SET_STRING_ELT(v, (R_xlen_t)i, Rf_mkCharLenCE(bytes, lens[i], CE_UTF8));
    bytes += lens[i];
  }
  SET_VECTOR_ELT(dc, d, v);
  UNPROTECT(1);
  return v;
}

// Writes page p's strings into dst[off, off + rows of p).
void str_fill(SEXP x, Holder* h, uint32_t p, SEXP dst, R_xlen_t off) {
  const int32_t* lens;
  const char* bytes;
  int32_t dict;
  fstx_error err;
  if (fstx_colsrc_str_page(h->src, p, &lens, &bytes, &dict, &err) != FSTX_OK) raise_err(&err);
  uint64_t first;
  uint32_t n;
  fstx_colsrc_page_span(h->src, p, &first, &n);
  if (dict >= 0) {
    SEXP dv = str_dict(x, h, dict);  // kept alive by x's dictionary cache
    const SEXP* dp = STRING_PTR_RO(dv);
    for (uint32_t i = 0; i < n; ++i) SET_STRING_ELT(dst, off + (R_xlen_t)i, lens[i] < 0 ? NA_STRING : dp[lens[i]]);
  } else {
    for (uint32_t i = 0; i < n; ++i) {
      if (lens[i] < 0) {
        SET_STRING_ELT(dst, off + (R_xlen_t)i, NA_STRING);
      } else {
        SET_STRING_ELT(dst, off + (R_xlen_t)i, Rf_mkCharLenCE(bytes, lens[i], CE_UTF8));
        bytes += lens[i];
      }
    }
  }
}

// Every page is made: the full vector is the column; the bytes and (lazy columns) the file are released.
void str_finish(SEXP x, Holder* h) {
  SEXP st = R_altrep_data2(x);
  SET_VECTOR_ELT(st, 1, R_NilValue);
  SET_VECTOR_ELT(st, 2, R_NilValue);
  h->s_fp = STRING_PTR_RO(VECTOR_ELT(st, 0));  // stays valid: x keeps the full vector, R never moves it
  h->s_done = true;
  h->p0 = UINT32_MAX;
  h->release();
}

// The full vector, allocated on first need; the page made alone (if any) moves into it.
SEXP str_full_vec(SEXP x, Holder* h) {
  SEXP st = R_altrep_data2(x);
  SEXP full = VECTOR_ELT(st, 0);
  if (full != R_NilValue) return full;
  full = Rf_allocVector(STRSXP, (R_xlen_t)h->rows);
  SET_VECTOR_ELT(st, 0, full);  // protected through x from here on
  if (h->p0 != UINT32_MAX) {
    SEXP pv = VECTOR_ELT(st, 1);
    uint64_t first;
    uint32_t n;
    fstx_colsrc_page_span(h->src, h->p0, &first, &n);
    for (uint32_t i = 0; i < n; ++i) SET_STRING_ELT(full, (R_xlen_t)(first + i), STRING_ELT(pv, i));
    h->made[h->p0] = 1;
    ++h->s_made;
    SET_VECTOR_ELT(st, 1, R_NilValue);
    h->p0 = UINT32_MAX;
    h->c_n = 0;
  }
  return full;
}

// Makes page p into the full vector (finishing the column when it was the last page).
void str_make_into_full(SEXP x, Holder* h, uint32_t p) {
  SEXP full = str_full_vec(x, h);
  if (h->made[p]) return;  // it was the page made alone
  uint64_t first;
  uint32_t n;
  fstx_colsrc_page_span(h->src, p, &first, &n);
  str_fill(x, h, p, full, (R_xlen_t)first);
  h->made[p] = 1;
  ++h->s_made;
  fstx_colsrc_drop(h->src, p);  // the R strings hold the data now
  if (p < h->offs.size()) std::vector<uint64_t>().swap(h->offs[p]);
  if (h->s_made == (uint32_t)h->made.size()) str_finish(x, h);
}

// 1.0: an all-dictionary column with nothing made yet, made whole in one pass straight from the codes. Every
// dictionary is made first and the full vector is allocated last; nothing is allocated while the rows are written,
// so the full vector is the newest object on R's heap and plain pointer stores need no write barrier (R's own
// duplicate() copies string vectors the same way). Called from Dataptr, so R's collector is also off.
void str_dict_fill(SEXP x, Holder* h, const fstx_colsrc_info& ci) {
  for (uint32_t d = 0; d < ci.n_dicts; ++d) str_dict(x, h, (int32_t)d);
  SEXP dc = VECTOR_ELT(R_altrep_data2(x), 2);
  SEXP full = str_full_vec(x, h);  // allocated last
  SEXP* o = (SEXP*)STRING_PTR_RO(full);
  fstx_error err;
  for (uint32_t p = 0; p < ci.n_pages; ++p) {
    const int32_t* codes;
    const char* bytes;
    int32_t d;
    if (fstx_colsrc_str_page(h->src, p, &codes, &bytes, &d, &err) != FSTX_OK) raise_err(&err);
    uint64_t first;
    uint32_t n;
    fstx_colsrc_page_span(h->src, p, &first, &n);
    const SEXP* dp = STRING_PTR_RO(VECTOR_ELT(dc, d));  // codes are checked against the dictionary at decode
    SEXP* to = o + first;
    for (uint32_t i = 0; i < n; ++i) to[i] = codes[i] < 0 ? NA_STRING : dp[codes[i]];
    h->made[p] = 1;
    fstx_colsrc_drop(h->src, p);
    if (p < h->offs.size()) std::vector<uint64_t>().swap(h->offs[p]);
  }
  h->s_made = ci.n_pages;
  str_finish(x, h);
}

// Makes every remaining page; returns the full vector.
SEXP str_complete(SEXP x) {
  Holder* h = hold(x);
  if (h->s_done) return st_full(x);
  fstx_colsrc_info ci;
  fstx_colsrc_info_get(h->src, &ci);
  fstx_error err;
  if (!ci.detached && ci.pages_ready + h->s_made + (h->p0 != UINT32_MAX ? 1u : 0u) < ci.n_pages) {
    // lazy column: decode the pages not made yet in parallel first
    if (fstx_colsrc_load(h->src, nullptr, h->threads, &err) != FSTX_OK) raise_err(&err);
  }
  if (ci.all_dict && h->s_made == 0 && h->p0 == UINT32_MAX && ci.n_pages > 0) {
    str_dict_fill(x, h, ci);
    return st_full(x);
  }
  str_full_vec(x, h);
  for (uint32_t p = 0; p < ci.n_pages && !h->s_done; ++p)
    if (!h->made[p]) str_make_into_full(x, h, p);
  if (!h->s_done) str_finish(x, h);  // zero pages (zero rows)
  return st_full(x);
}

R_xlen_t alt_length(SEXP x) { return (R_xlen_t)hold(x)->rows; }

SEXP str_elt_slow(SEXP x, Holder* h, R_xlen_t i) {
  const uint64_t r = (uint64_t)i;
  const uint32_t p = fstx_colsrc_page_of(h->src, r);
  uint64_t first;
  uint32_t n;
  fstx_colsrc_page_span(h->src, p, &first, &n);
  if (p == h->p0) {
    h->c_first = first;
    h->c_n = n;
    h->c_in_full = false;
    return STRING_ELT(st_p0(x), (R_xlen_t)(r - first));
  }
  if (!h->made[p]) {
    if (h->s_made == 0 && h->p0 == UINT32_MAX && h->made.size() > 1) {  // first page used: alone
      SEXP pv = PROTECT(Rf_allocVector(STRSXP, (R_xlen_t)n));
      str_fill(x, h, p, pv, 0);
      SET_VECTOR_ELT(R_altrep_data2(x), 1, pv);
      UNPROTECT(1);
      h->p0 = p;
      fstx_colsrc_drop(h->src, p);
      if (p < h->offs.size()) std::vector<uint64_t>().swap(h->offs[p]);
      h->c_first = first;
      h->c_n = n;
      h->c_in_full = false;
      return STRING_ELT(pv, (R_xlen_t)(r - first));
    }
    str_make_into_full(x, h, p);
    if (h->s_done) return STRING_ELT(st_full(x), i);
  }
  h->c_first = first;
  h->c_n = n;
  h->c_in_full = true;
  return STRING_ELT(st_full(x), i);
}

SEXP str_elt(SEXP x, R_xlen_t i) {
  Holder* h = hold(x);
  if (h->s_done) return h->s_fp[i];
  const uint64_t r = (uint64_t)i;
  if (h->c_n && r - h->c_first < h->c_n)
    return h->c_in_full ? STRING_ELT(st_full(x), i) : STRING_ELT(st_p0(x), (R_xlen_t)(r - h->c_first));
  return str_elt_slow(x, h, i);
}

void str_set_elt(SEXP x, R_xlen_t i, SEXP v) {
  PROTECT(v);
  SEXP full = str_complete(x);
  SET_STRING_ELT(full, i, v);
  UNPROTECT(1);
}

void* str_dataptr(SEXP x, Rboolean) { return (void*)STRING_PTR_RO(str_complete(x)); }

const void* str_dataptr_or_null(SEXP x) {
  return hold(x)->s_done ? (const void*)STRING_PTR_RO(st_full(x)) : nullptr;
}

// Fills out[j] with row indx[j] of the unfinished string column x (indices already checked: valid 1-based rows).
// Pages already made are read from the full vector or the first-page vector, dictionary pages from their
// dictionary, plain pages from their decoded bytes through per-page byte offsets kept in the holder.
void str_subset_fill(SEXP x, SEXP indx, SEXP out) {
  Holder* h = hold(x);
  const bool is_int = TYPEOF(indx) == INTSXP;
  const R_xlen_t k = Rf_xlength(indx);
  const int* ii = is_int ? INTEGER(indx) : nullptr;
  const double* di = is_int ? nullptr : REAL(indx);
  fstx_colsrc_info ci;
  fstx_colsrc_info_get(h->src, &ci);
  fstx_error err;
  for (R_xlen_t j = 0; j < k; ++j) {
    const uint64_t r = is_int ? (uint64_t)ii[j] - 1 : (uint64_t)di[j] - 1;
    const uint32_t p = fstx_colsrc_page_of(h->src, r);
    uint64_t first;
    uint32_t pn;
    fstx_colsrc_page_span(h->src, p, &first, &pn);
    SEXP s;
    if (h->made[p]) {
      s = STRING_ELT(st_full(x), (R_xlen_t)r);
    } else if (p == h->p0) {
      s = STRING_ELT(st_p0(x), (R_xlen_t)(r - first));
    } else {
      const int32_t* lens;
      const char* bytes;
      int32_t dict;
      if (fstx_colsrc_str_page(h->src, p, &lens, &bytes, &dict, &err) != FSTX_OK) raise_err(&err);
      const int32_t l = lens[r - first];
      if (l < 0) {
        s = NA_STRING;
      } else if (dict >= 0) {
        s = STRING_ELT(str_dict(x, h, dict), l);
      } else {
        const uint64_t* o = nullptr;
        bool oom = false;
        try {  // byte offset of each row in the page, made once per page and kept until the page is made
          if (h->offs.size() != ci.n_pages) h->offs.resize(ci.n_pages);
          std::vector<uint64_t>& v = h->offs[p];
          if (v.empty()) {
            v.resize(pn);
            uint64_t acc = 0;
            for (uint32_t i = 0; i < pn; ++i) {
              v[i] = acc;
              if (lens[i] > 0) acc += (uint64_t)lens[i];
            }
          }
          o = v.data();
        } catch (const std::bad_alloc&) {
          oom = true;
        }
        if (oom) Rf_error("fstx: out of memory");  // no C++ object alive here
        s = Rf_mkCharLenCE(bytes + o[r - first], l, CE_UTF8);
      }
    }
    SET_STRING_ELT(out, j, s);
  }
}

// x[indx] read from the R strings fp[0, nx) with R's own subset rule (NA or out-of-range index -> NA).
SEXP subset_of_ptr(const SEXP* fp, R_xlen_t nx, SEXP indx) {
  const bool is_int = TYPEOF(indx) == INTSXP;
  const R_xlen_t k = Rf_xlength(indx);
  const int* ii = is_int ? INTEGER(indx) : nullptr;
  const double* di = is_int ? nullptr : REAL(indx);
  SEXP out = PROTECT(Rf_allocVector(STRSXP, k));
  for (R_xlen_t j = 0; j < k; ++j) {  // same tests as R's ExtractSubset (subset.c)
    R_xlen_t r;
    if (is_int) {
      r = ii[j] > 0 && (R_xlen_t)ii[j] <= nx ? (R_xlen_t)ii[j] - 1 : -1;
    } else {
      const double d = di[j];  // R: (R_xlen_t)(d - 1) in [0, nx), tested before the cast
      r = R_FINITE(d) && d > 0 && d < (double)nx + 1 ? (R_xlen_t)(d - 1) : -1;
    }
    SET_STRING_ELT(out, j, r < 0 ? NA_STRING : fp[r]);
  }
  UNPROTECT(1);
  return out;
}

// True when every index is a whole row number in [1, n] (no NA, nothing out of range).
bool rows_valid(SEXP indx, double n) {
  const bool is_int = TYPEOF(indx) == INTSXP;
  const R_xlen_t k = Rf_xlength(indx);
  const int* ii = is_int ? INTEGER(indx) : nullptr;
  const double* di = is_int ? nullptr : REAL(indx);
  for (R_xlen_t j = 0; j < k; ++j) {
    if (is_int ? (ii[j] == NA_INTEGER || ii[j] < 1 || (double)ii[j] > n)
               : (ISNAN(di[j]) || di[j] < 1 || di[j] > n || di[j] != std::floor(di[j])))
      return false;
  }
  return true;
}

// ---------------------------------------------------------------- views (1.0)
// A subset of an unfinished string column (dplyr::filter, x[i], vec_slice) that makes no R string until it is
// used: filter() slices every column, but later steps often use only a few of them. On first use (Elt,
// Dataptr, serialize) every row of the view is made at once, inside R's Elt / Dataptr call (collector off), and
// the view lets go of its column.
// data1 = the column (an fstx_str vector, kept alive by the view; R_NilValue once made),
// data2 = list(row numbers: 1-based INTSXP, or REALSXP past INT_MAX; the made STRSXP or NULL).
inline SEXP vw_rows(SEXP v) { return VECTOR_ELT(R_altrep_data2(v), 0); }
inline SEXP vw_made(SEXP v) { return VECTOR_ELT(R_altrep_data2(v), 1); }

// A new view of column x (unfinished, holder with a source) over the valid row numbers in rows (copied).
SEXP view_new(SEXP x, SEXP rows) {
  Holder* h = hold(x);
  const R_xlen_t k = Rf_xlength(rows);
  const bool small = h->rows <= (uint64_t)INT_MAX;
  SEXP r = PROTECT(Rf_allocVector(small ? INTSXP : REALSXP, k));
  if (TYPEOF(rows) == INTSXP) {
    const int* s = INTEGER(rows);
    if (small) std::memcpy(INTEGER(r), s, (size_t)k * sizeof(int));
    else for (R_xlen_t j = 0; j < k; ++j) REAL(r)[j] = (double)s[j];
  } else {
    const double* s = REAL(rows);
    if (small) for (R_xlen_t j = 0; j < k; ++j) INTEGER(r)[j] = (int)s[j];
    else std::memcpy(REAL(r), s, (size_t)k * sizeof(double));
  }
  SEXP st = PROTECT(Rf_allocVector(VECSXP, 2));
  SET_VECTOR_ELT(st, 0, r);
  // The view reads x's rows later: R code that changes x must copy it first (x is shared from here on).
  MARK_NOT_MUTABLE(x);
  SEXP v = R_new_altrep(cls_view, x, st);
  UNPROTECT(2);
  return v;
}

// Makes every row of view v (once) and returns the STRSXP that holds them. R allocation: called by R inside Elt /
// Dataptr (collector off) or from a method that may allocate.
SEXP view_make(SEXP v) {
  SEXP made = vw_made(v);
  if (made != R_NilValue) return made;
  SEXP x = R_altrep_data1(v), rows = vw_rows(v);
  Holder* h = hold(x);
  if (h->s_done) {
    made = subset_of_ptr(h->s_fp, (R_xlen_t)h->rows, rows);
  } else {
    if (!h->src) Rf_error("fstx: internal error, column source is missing");
    made = PROTECT(Rf_allocVector(STRSXP, Rf_xlength(rows)));
    str_subset_fill(x, rows, made);
    UNPROTECT(1);
  }
  SEXP st = R_altrep_data2(v);
  SET_VECTOR_ELT(st, 1, made);  // protected through v from here on
  SET_VECTOR_ELT(st, 0, R_NilValue);
  R_set_altrep_data1(v, R_NilValue);  // the column may go now
  return made;
}

R_xlen_t vw_length(SEXP v) {
  SEXP made = vw_made(v);
  return Rf_xlength(made != R_NilValue ? made : vw_rows(v));
}
SEXP vw_elt(SEXP v, R_xlen_t i) {
  SEXP made = vw_made(v);
  return STRING_ELT(made != R_NilValue ? made : view_make(v), i);
}
void vw_set_elt(SEXP v, R_xlen_t i, SEXP s) {
  PROTECT(s);
  SET_STRING_ELT(view_make(v), i, s);
  UNPROTECT(1);
}
void* vw_dataptr(SEXP v, Rboolean) { return (void*)STRING_PTR_RO(view_make(v)); }
const void* vw_dataptr_or_null(SEXP v) {
  SEXP made = vw_made(v);
  return made != R_NilValue ? (const void*)STRING_PTR_RO(made) : nullptr;
}
// A subset of a view: a new view of the same column while nothing is made (filter then arrange), else R's rule.
SEXP vw_extract_subset(SEXP v, SEXP indx, SEXP) {
  if (TYPEOF(indx) != INTSXP && TYPEOF(indx) != REALSXP) return nullptr;
  SEXP made = vw_made(v);
  if (made == R_NilValue && hold(R_altrep_data1(v))->s_done) made = view_make(v);  // the column is made: cheap
  if (made != R_NilValue) return subset_of_ptr(STRING_PTR_RO(made), Rf_xlength(made), indx);
  SEXP rows = vw_rows(v);
  const R_xlen_t k = Rf_xlength(indx);
  if (k < 2 || !rows_valid(indx, (double)Rf_xlength(rows))) return nullptr;
  SEXP x = R_altrep_data1(v);
  const bool is_int = TYPEOF(indx) == INTSXP, r_int = TYPEOF(rows) == INTSXP;
  SEXP comp = PROTECT(Rf_allocVector(r_int ? INTSXP : REALSXP, k));  // column row numbers of the subset
  for (R_xlen_t j = 0; j < k; ++j) {
    const R_xlen_t q = is_int ? (R_xlen_t)INTEGER(indx)[j] - 1 : (R_xlen_t)REAL(indx)[j] - 1;
    if (r_int) INTEGER(comp)[j] = INTEGER(rows)[q];
    else REAL(comp)[j] = REAL(rows)[q];
  }
  SEXP out = view_new(x, comp);
  UNPROTECT(1);
  return out;
}
Rboolean vw_inspect(SEXP v, int, int, int, void (*)(SEXP, int, int, int)) {
  SEXP made = vw_made(v);
  if (made != R_NilValue) Rprintf(" fstx string subset, %.0f rows, made\n", (double)Rf_xlength(made));
  else Rprintf(" fstx string subset, %.0f rows of a %.0f-row column, not made yet\n", (double)Rf_xlength(vw_rows(v)),
               (double)hold(R_altrep_data1(v))->rows);
  return TRUE;
}

// x[i], vctrs::vec_slice (dplyr::filter, arrange, joins, rbind).
// - Complete column: R's own subset rule (NA or out-of-range index -> NA) read straight from the full vector, so a
//   finished column subsets at plain-vector speed instead of one Elt call per element.
// - Unfinished column: a subset of 2 to half the rows becomes a view (above): its R strings are made only if it
//   is used, straight from the decoded bytes, without making the pages it touches (half: past that, making the
//   whole column is the better deal, since later ops find it made). Anything else (NA or out-of-range indices,
//   larger subsets) goes to R's default, which uses Elt / Dataptr.
SEXP str_extract_subset(SEXP x, SEXP indx, SEXP) {
  Holder* h = hold(x);
  if (TYPEOF(indx) != INTSXP && TYPEOF(indx) != REALSXP) return nullptr;
  if (h->s_done) return subset_of_ptr(h->s_fp, (R_xlen_t)h->rows, indx);
  if (!h->src) return nullptr;
  // one element (x[i], often in a loop): R's default asks Elt, which makes and keeps the page, so the next
  // calls on that page cost what they cost on a plain vector. Zero elements: nothing to make.
  const R_xlen_t k = Rf_xlength(indx);
  if (k < 2) return nullptr;
  const double n = (double)h->rows;
  if ((double)k > n / 2 || !rows_valid(indx, n)) return nullptr;
  return view_new(x, indx);
}

int str_no_na(SEXP x) { return hold(x)->nulls == 0 ? 1 : 0; }

// Compact state: list(lengths (-1 = NA), all bytes back to back). R strings are not made for it.
// strings = "auto" columns and fully made columns use R's standard format instead (readable without fstx).
SEXP str_serialized_state(SEXP x) {
  Holder* h = hold(x);
  if (h->s_done || h->portable) return nullptr;
  SEXP opt = Rf_GetOption1(Rf_install("fstx.portable_serialize"));
  if (opt != R_NilValue && Rf_asLogical(opt) == TRUE) return nullptr;
  fstx_colsrc_info ci;
  fstx_colsrc_info_get(h->src, &ci);
  SEXP full = st_full(x), pv0 = st_p0(x);
  fstx_error err;
  SEXP lens_out = PROTECT(Rf_allocVector(INTSXP, (R_xlen_t)h->rows));
  int* lo = INTEGER(lens_out);
  uint64_t total = 0;
  for (int pass = 0; pass < 2; ++pass) {
    unsigned char* bo = nullptr;
    SEXP bytes_out = R_NilValue;
    if (pass == 1) {
      bytes_out = PROTECT(Rf_allocVector(RAWSXP, (R_xlen_t)total));
      bo = RAW(bytes_out);
    }
    for (uint32_t p = 0; p < ci.n_pages; ++p) {
      uint64_t first;
      uint32_t n;
      fstx_colsrc_page_span(h->src, p, &first, &n);
      if (h->made[p] || p == h->p0) {  // already R strings
        for (uint32_t i = 0; i < n; ++i) {
          SEXP s = h->made[p] ? STRING_ELT(full, (R_xlen_t)(first + i)) : STRING_ELT(pv0, (R_xlen_t)i);
          const int l = s == NA_STRING ? -1 : LENGTH(s);
          if (pass == 0) {
            lo[first + i] = l;
            if (l > 0) total += (uint64_t)l;
          } else if (l > 0) {
            std::memcpy(bo, CHAR(s), (size_t)l);
            bo += l;
          }
        }
        continue;
      }
      const int32_t* lens;
      const char* bytes;
      int32_t dict;
      if (fstx_colsrc_str_page(h->src, p, &lens, &bytes, &dict, &err) != FSTX_OK) raise_err(&err);
      const SEXP* dp = nullptr;
      if (dict >= 0) dp = STRING_PTR_RO(str_dict(x, h, dict));  // dictionary strings only (kept by x)
      for (uint32_t i = 0; i < n; ++i) {
        int32_t l;
        const char* s;
        if (dict >= 0) {
          const int32_t k = lens[i];
          l = k < 0 ? -1 : LENGTH(dp[k]);
          s = k < 0 ? nullptr : CHAR(dp[k]);
        } else {
          l = lens[i];
          s = bytes;
          if (l > 0) bytes += l;
        }
        if (pass == 0) {
          lo[first + i] = l;
          if (l > 0) total += (uint64_t)l;
        } else if (l > 0) {
          std::memcpy(bo, s, (size_t)l);
          bo += l;
        }
      }
    }
    if (pass == 1) {
      SEXP st = PROTECT(Rf_allocVector(VECSXP, 2));
      SET_VECTOR_ELT(st, 0, lens_out);
      SET_VECTOR_ELT(st, 1, bytes_out);
      if (!ci.detached) fstx_colsrc_drop(h->src, UINT32_MAX);  // lazy: the file still has them
      UNPROTECT(3);
      return st;
    }
  }
  UNPROTECT(1);
  return nullptr;
}

SEXP make_str_altrep(SEXP xp, Holder* h);

SEXP str_unserialize(SEXP, SEXP st) {
  if (TYPEOF(st) != VECSXP || Rf_xlength(st) != 2 || TYPEOF(VECTOR_ELT(st, 0)) != INTSXP ||
      TYPEOF(VECTOR_ELT(st, 1)) != RAWSXP)
    Rf_error("fstx: invalid serialized string column");
  SEXP lens = VECTOR_ELT(st, 0), bytes = VECTOR_ELT(st, 1);
  const R_xlen_t n = Rf_xlength(lens);
  const int* l = INTEGER(lens);
  uint64_t total = 0;
  for (R_xlen_t i = 0; i < n; ++i) {
    if (l[i] < -1) Rf_error("fstx: invalid serialized string column");
    if (l[i] > 0) total += (uint64_t)l[i];
  }
  if (total != (uint64_t)Rf_xlength(bytes)) Rf_error("fstx: invalid serialized string column");
  SEXP xp = PROTECT(fstx_holder_xp());
  Holder* h = new (std::nothrow) Holder();
  if (!h) Rf_error("fstx: out of memory");
  R_SetExternalPtrAddr(xp, h);
  fstx_error err;
  if (fstx_colsrc_from_strings((uint64_t)n, l, (const char*)RAW(bytes), &h->src, &err) != FSTX_OK) raise_err(&err);
  fstx_holder_sync(h);
  SEXP out = fstx_altrep_new(xp);
  UNPROTECT(1);
  return out;
}

Rboolean alt_inspect(SEXP x, int, int, int, void (*)(SEXP, int, int, int)) {
  Holder* h = hold(x);
  const char* what = h->phys == FSTX_STRING && h->mode == FSTX_SRC_STRINGS ? "character"
                     : h->phys == FSTX_FLOAT64                             ? "double"
                     : h->phys == FSTX_LOGICAL                             ? "logical"
                                                                           : "integer";
  if (!h->src) {
    Rprintf(" fstx %s %s column, %.0f rows, materialised\n", h->lazy ? "lazy" : "deferred", what, (double)h->rows);
  } else {
    fstx_colsrc_info ci;
    fstx_colsrc_info_get(h->src, &ci);
    Rprintf(" fstx %s %s column, %.0f rows, %u of %u pages decoded (%.1f MB)%s\n", h->lazy ? "lazy" : "deferred",
            what, (double)h->rows, ci.pages_ready, ci.n_pages, ci.cached_bytes / 1048576.0,
            ci.detached ? "" : ", reading from the file");
  }
  return TRUE;
}

// ---------------------------------------------------------------- fixed width (integer, double, logical)
// data2 = the full vector once materialised, else NULL.
template <typename T>
T* full_ptr(SEXP full);
template <>
int* full_ptr<int>(SEXP full) {
  return TYPEOF(full) == REALSXP ? nullptr : (TYPEOF(full) == LGLSXP ? LOGICAL(full) : INTEGER(full));
}
template <>
double* full_ptr<double>(SEXP full) {
  return REAL(full);
}

SEXP fixed_full(SEXP x) {
  SEXP full = R_altrep_data2(x);
  if (full != R_NilValue) return full;
  Holder* h = hold(x);
  const SEXPTYPE t = h->mode == FSTX_SRC_CODES ? INTSXP
                     : h->phys == FSTX_FLOAT64 ? REALSXP
                     : h->phys == FSTX_LOGICAL ? LGLSXP
                                               : INTSXP;
  full = PROTECT(Rf_allocVector(t, (R_xlen_t)h->rows));
  void* dst = t == REALSXP ? (void*)REAL(full) : t == LGLSXP ? (void*)LOGICAL(full) : (void*)INTEGER(full);
  fstx_error err;
  if (fstx_colsrc_load(h->src, dst, h->threads, &err) != FSTX_OK) raise_err(&err);
  R_set_altrep_data2(x, full);
  h->release();
  UNPROTECT(1);
  return full;
}

template <typename T>
T fixed_elt(SEXP x, R_xlen_t i) {
  SEXP full = R_altrep_data2(x);
  if (full != R_NilValue) return full_ptr<T>(full)[i];
  Holder* h = hold(x);
  const uint64_t row = (uint64_t)i;
  if (h->c_vals && row >= h->c_first && row - h->c_first < h->c_n) return ((const T*)h->c_vals)[row - h->c_first];
  const uint32_t p = fstx_colsrc_page_of(h->src, row);
  uint64_t first;
  uint32_t n;
  fstx_colsrc_page_span(h->src, p, &first, &n);
  const void* vals;
  fstx_error err;
  h->c_vals = nullptr;
  if (fstx_colsrc_fixed_page(h->src, p, &vals, &err) != FSTX_OK) raise_err(&err);
  h->c_first = first;  // valid until the next fixed_page call (which may drop older pages)
  h->c_n = n;
  h->c_vals = vals;
  return ((const T*)vals)[row - first];
}

template <typename T>
R_xlen_t fixed_region(SEXP x, R_xlen_t i, R_xlen_t n, T* buf) {
  Holder* h = hold(x);
  const R_xlen_t len = (R_xlen_t)h->rows;
  if (i >= len) return 0;
  if (n > len - i) n = len - i;
  SEXP full = R_altrep_data2(x);
  if (full != R_NilValue) {
    std::memcpy(buf, full_ptr<T>(full) + i, (size_t)n * sizeof(T));
    return n;
  }
  R_xlen_t done = 0;
  fstx_error err;
  while (done < n) {
    const uint64_t row = (uint64_t)(i + done);
    const uint32_t p = fstx_colsrc_page_of(h->src, row);
    uint64_t first;
    uint32_t pn;
    fstx_colsrc_page_span(h->src, p, &first, &pn);
    const void* vals;
    h->c_vals = nullptr;
    if (fstx_colsrc_fixed_page(h->src, p, &vals, &err) != FSTX_OK) raise_err(&err);
    h->c_first = first;
    h->c_n = pn;
    h->c_vals = vals;
    R_xlen_t k = (R_xlen_t)(first + pn - row);
    if (k > n - done) k = n - done;
    std::memcpy(buf + done, (const T*)vals + (row - first), (size_t)k * sizeof(T));
    done += k;
  }
  return n;
}

int int_elt(SEXP x, R_xlen_t i) { return fixed_elt<int>(x, i); }
double dbl_elt(SEXP x, R_xlen_t i) { return fixed_elt<double>(x, i); }
R_xlen_t int_region(SEXP x, R_xlen_t i, R_xlen_t n, int* buf) { return fixed_region<int>(x, i, n, buf); }
R_xlen_t dbl_region(SEXP x, R_xlen_t i, R_xlen_t n, double* buf) { return fixed_region<double>(x, i, n, buf); }

void* fixed_dataptr(SEXP x, Rboolean) {
  SEXP full = fixed_full(x);
  return TYPEOF(full) == REALSXP ? (void*)REAL(full) : TYPEOF(full) == LGLSXP ? (void*)LOGICAL(full) : (void*)INTEGER(full);
}

const void* fixed_dataptr_or_null(SEXP x) {
  SEXP full = R_altrep_data2(x);
  if (full == R_NilValue) return nullptr;
  return TYPEOF(full) == REALSXP ? (const void*)REAL(full)
         : TYPEOF(full) == LGLSXP ? (const void*)LOGICAL(full)
                                  : (const void*)INTEGER(full);
}

int fixed_no_na(SEXP x) {
  Holder* h = hold(x);
  return h->nulls == 0 && h->phys != FSTX_FLOAT64 ? 1 : 0;  // a double page's null count does not cover NaN
}

SEXP make_str_altrep(SEXP xp, Holder* h) {
  fstx_colsrc_info ci;
  fstx_colsrc_info_get(h->src, &ci);
  h->made.assign(ci.n_pages, 0);
  h->s_made = 0;
  h->s_done = false;
  h->p0 = UINT32_MAX;
  h->c_n = 0;
  SEXP st = PROTECT(Rf_allocVector(VECSXP, 3));
  SET_VECTOR_ELT(st, 2, Rf_allocVector(VECSXP, (R_xlen_t)ci.n_dicts));
  SEXP out = R_new_altrep(cls_str, xp, st);
  UNPROTECT(1);
  return out;
}

}  // namespace

SEXP fstx_holder_xp() {
  SEXP xp = PROTECT(R_MakeExternalPtr(nullptr, R_NilValue, R_NilValue));
  R_RegisterCFinalizerEx(xp, holder_finalize, TRUE);
  UNPROTECT(1);
  return xp;
}

Holder* fstx_holder_of(SEXP xp) {
  if (TYPEOF(xp) != EXTPTRSXP) return nullptr;
  return (Holder*)R_ExternalPtrAddr(xp);
}

void fstx_holder_sync(Holder* h) {
  fstx_colsrc_info ci;
  fstx_colsrc_info_get(h->src, &ci);
  h->rows = ci.rows;
  h->nulls = ci.null_count;
  h->phys = ci.phys;
  h->mode = ci.mode;
}

SEXP fstx_altrep_new(SEXP xp) {
  Holder* h = fstx_holder_of(xp);
  if (!h || !h->src) Rf_error("fstx: internal error, column source is missing");
  if (h->mode == FSTX_SRC_STRINGS) return make_str_altrep(xp, h);
  R_altrep_class_t c = h->mode == FSTX_SRC_CODES     ? cls_int
                       : h->phys == FSTX_FLOAT64     ? cls_dbl
                       : h->phys == FSTX_LOGICAL     ? cls_lgl
                                                     : cls_int;
  return R_new_altrep(c, xp, R_NilValue);
}

SEXP fstx_str_plain(SEXP x) {
  if (!ALTREP(x) || !R_altrep_inherits(x, cls_str)) Rf_error("fstx: internal error, not an fstx string column");
  (void)STRING_PTR_RO(x);  // Dataptr: every page made, with R's collector off
  return st_full(x);
}

void fstx_init_altrep(DllInfo* dll) {
  cls_str = R_make_altstring_class("fstx_str", "fstx", dll);
  R_set_altrep_Length_method(cls_str, alt_length);
  R_set_altrep_Inspect_method(cls_str, alt_inspect);
  R_set_altrep_Serialized_state_method(cls_str, str_serialized_state);
  R_set_altrep_Unserialize_method(cls_str, str_unserialize);
  R_set_altvec_Dataptr_method(cls_str, str_dataptr);
  R_set_altvec_Dataptr_or_null_method(cls_str, str_dataptr_or_null);
  R_set_altstring_Elt_method(cls_str, str_elt);
  R_set_altstring_Set_elt_method(cls_str, str_set_elt);
  R_set_altstring_No_NA_method(cls_str, str_no_na);
  R_set_altvec_Extract_subset_method(cls_str, str_extract_subset);
  cls_view = R_make_altstring_class("fstx_view", "fstx", dll);
  R_set_altrep_Length_method(cls_view, vw_length);
  R_set_altrep_Inspect_method(cls_view, vw_inspect);
  R_set_altvec_Dataptr_method(cls_view, vw_dataptr);
  R_set_altvec_Dataptr_or_null_method(cls_view, vw_dataptr_or_null);
  R_set_altvec_Extract_subset_method(cls_view, vw_extract_subset);
  R_set_altstring_Elt_method(cls_view, vw_elt);
  R_set_altstring_Set_elt_method(cls_view, vw_set_elt);

  cls_int = R_make_altinteger_class("fstx_int", "fstx", dll);
  cls_dbl = R_make_altreal_class("fstx_dbl", "fstx", dll);
  cls_lgl = R_make_altlogical_class("fstx_lgl", "fstx", dll);
  for (R_altrep_class_t* c : {&cls_int, &cls_dbl, &cls_lgl}) {
    R_set_altrep_Length_method(*c, alt_length);
    R_set_altrep_Inspect_method(*c, alt_inspect);
    R_set_altvec_Dataptr_method(*c, fixed_dataptr);
    R_set_altvec_Dataptr_or_null_method(*c, fixed_dataptr_or_null);
  }
  R_set_altinteger_Elt_method(cls_int, int_elt);
  R_set_altinteger_Get_region_method(cls_int, int_region);
  R_set_altinteger_No_NA_method(cls_int, fixed_no_na);
  R_set_altreal_Elt_method(cls_dbl, dbl_elt);
  R_set_altreal_Get_region_method(cls_dbl, dbl_region);
  R_set_altlogical_Elt_method(cls_lgl, int_elt);
  R_set_altlogical_Get_region_method(cls_lgl, int_region);
  R_set_altlogical_No_NA_method(cls_lgl, fixed_no_na);
}

// ---------------------------------------------------------------- R entry points

// Factor codes for an all-dictionary string column: remaps[[d]] gives the 1-based level of dictionary d's
// entries. Eager: an integer vector; lazy: an ALTREP integer vector decoding codes page by page.
[[cpp11::register]]
SEXP fstx_codes_impl(SEXP xp, SEXP remaps, SEXP levels, bool ordered, bool lazy) {
  Holder* h = fstx_holder_of(xp);
  if (!h || !h->src) cpp11::stop("fstx: internal error, column source is missing");
  fstx_colsrc_info ci;
  fstx_colsrc_info_get(h->src, &ci);
  if (TYPEOF(remaps) != VECSXP || (uint32_t)Rf_xlength(remaps) != ci.n_dicts)
    cpp11::stop("fstx: internal error, wrong number of dictionaries");
  const R_xlen_t nl = Rf_xlength(levels);
  std::vector<const int32_t*> rp(ci.n_dicts);
  for (uint32_t d = 0; d < ci.n_dicts; ++d) {
    SEXP r = VECTOR_ELT(remaps, d);
    if (TYPEOF(r) != INTSXP) cpp11::stop("fstx: internal error, remap is not integer");
    const int* v = INTEGER(r);
    for (R_xlen_t k = 0; k < Rf_xlength(r); ++k)
      if (v[k] == NA_INTEGER || v[k] < 1 || v[k] > nl) cpp11::stop("fstx: internal error, remap out of range");
    rp[d] = v;
  }
  fstx_error err;
  if (fstx_colsrc_set_codes(h->src, rp.data(), &err) != FSTX_OK) cpp11::stop("fstx: %s", err.message);
  fstx_holder_sync(h);
  cpp11::sexp out;
  if (lazy) {
    out = cpp11::safe[fstx_altrep_new](xp);
  } else {
    out = cpp11::safe[Rf_allocVector](INTSXP, (R_xlen_t)h->rows);
    if (fstx_colsrc_load(h->src, INTEGER(out), h->threads, &err) != FSTX_OK) cpp11::stop("fstx: %s", err.message);
    h->release();
  }
  cpp11::safe[Rf_setAttrib](out, R_LevelsSymbol, levels);
  cpp11::writable::strings cl = ordered ? cpp11::writable::strings({"ordered", "factor"})
                                        : cpp11::writable::strings({"factor"});
  cpp11::safe[Rf_setAttrib](out, R_ClassSymbol, (SEXP)cl);
  return out;
}

// Grows R's heap ahead of a read that will make many R strings: `nodes` live list cells plus one `bytes`
// raw vector, dropped at once. R keeps its larger collection thresholds, so building the strings triggers
// far fewer full collections (each of which walks R's global string cache).
[[cpp11::register]]
void fstx_presize_impl(double nodes, double bytes) {
  cpp11::unwind_protect([&] {
    SEXP a = PROTECT(nodes >= 1 ? Rf_allocList((int)std::min(nodes, 2e9)) : R_NilValue);
    SEXP b = PROTECT(bytes >= 1 ? Rf_allocVector(RAWSXP, (R_xlen_t)bytes) : R_NilValue);
    UNPROTECT(2);
    (void)a;
    (void)b;
  });
}

// State of an fstx ALTREP column (NULL for any other vector), for tests and diagnostics.
[[cpp11::register]]
SEXP fstx_col_state_impl(SEXP x) {
  if (!ALTREP(x)) return R_NilValue;
  if (!R_altrep_inherits(x, cls_str) && !R_altrep_inherits(x, cls_int) && !R_altrep_inherits(x, cls_dbl) &&
      !R_altrep_inherits(x, cls_lgl))
    return R_NilValue;
  Holder* h = hold(x);
  fstx_colsrc_info ci;
  std::memset(&ci, 0, sizeof(ci));
  if (h->src) fstx_colsrc_info_get(h->src, &ci);
  using namespace cpp11::literals;
  return cpp11::writable::list({"lazy"_nm = cpp11::as_sexp(h->lazy), "materialised"_nm = cpp11::as_sexp(h->src == nullptr),
                                "file_open"_nm = cpp11::as_sexp((bool)h->file), "pages"_nm = cpp11::as_sexp((double)ci.n_pages),
                                "pages_ready"_nm = cpp11::as_sexp((double)ci.pages_ready),
                                "cached_bytes"_nm = cpp11::as_sexp((double)ci.cached_bytes)});
}
