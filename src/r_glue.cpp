// FST-X R layer (cpp11). The only file that sees both R and the engine.
// R errors raised inside engine callbacks are caught with cpp11::unwind_protect, turned into an error code
// for the engine, and re-raised only after the engine has unwound and returned, so no longjmp ever crosses
// engine C++ frames.
#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "r_lazy.h"

namespace {

struct PlanFree {
  void operator()(fstx_plan* p) const { fstx_plan_free(p); }
};
using FilePtr = std::unique_ptr<fstx_file, FileCloser>;
using PlanPtr = std::unique_ptr<fstx_plan, PlanFree>;

std::string utf8_of(SEXP s) {  // s: a CHARSXP
  return std::string(Rf_translateCharUTF8(s));
}

std::string path_of(SEXP path) {
  if (TYPEOF(path) != STRSXP || Rf_xlength(path) != 1 || STRING_ELT(path, 0) == NA_STRING)
    cpp11::stop("fstx: 'path' must be a single string");
  return utf8_of(STRING_ELT(path, 0));
}

[[noreturn]] void stop_err(const fstx_error& e) { cpp11::stop("fstx: %s", e.message); }

FilePtr open_or_stop(const std::string& path) {
  fstx_file* f = nullptr;
  fstx_error err;
  if (fstx_open(path.c_str(), &f, &err) != FSTX_OK) stop_err(err);
  return FilePtr(f);
}

FileRef open_shared_or_stop(const std::string& path) {
  fstx_file* f = nullptr;
  fstx_error err;
  if (fstx_open(path.c_str(), &f, &err) != FSTX_OK) stop_err(err);
  return FileRef(f, FileCloser());
}

// ---------------------------------------------------------------- writing
struct StrSrc {
  const SEXP* p;
  SEXP na;
  bool utf8_locale;
};

inline bool is_ascii(const char* c, int n) {
  for (int i = 0; i < n; ++i)
    if ((unsigned char)c[i] > 127) return false;
  return true;
}

// Runs on worker threads: only reads CHARSXP memory and flags (CHAR, LENGTH, getCharCE never allocate).
int get_strings(void* ctx, uint64_t row, uint32_t n, const char** ptrs, int32_t* lens) {
  const StrSrc* s = (const StrSrc*)ctx;
  for (uint32_t i = 0; i < n; ++i) {
    SEXP x = s->p[row + i];
    if (x == s->na) {
      ptrs[i] = nullptr;
      lens[i] = -1;
      continue;
    }
    const char* c = CHAR(x);
    int len = LENGTH(x);
    ptrs[i] = c;
    lens[i] = len;
    cetype_t ce = Rf_getCharCE(x);
    if (ce == CE_UTF8) continue;
    if (ce == CE_BYTES) return 2;
    if (ce == CE_LATIN1) return 1;
    if (!s->utf8_locale && !is_ascii(c, len)) return 1;
  }
  return 0;
}

}  // namespace

// Returns 0 on success, or the 1-based index of a character column that must be converted to UTF-8 first.
[[cpp11::register]]
int fstx_write_impl(SEXP cols, SEXP path, SEXP names, SEXP phys, SEXP logical, SEXP ordered, SEXP levels,
                    SEXP tzone, SEXP attrs, SEXP table_attrs, double nrow, int codec, int level, int threads,
                    bool utf8_locale, int profile, int keep_pct, int force_encoding, bool append,
                    int test_crash) {
  const std::string p = path_of(path);
  const R_xlen_t nc = Rf_xlength(cols);
  if (Rf_xlength(names) != nc || Rf_xlength(phys) != nc || Rf_xlength(logical) != nc || Rf_xlength(ordered) != nc ||
      Rf_xlength(levels) != nc || Rf_xlength(tzone) != nc || Rf_xlength(attrs) != nc)
    cpp11::stop("fstx: internal error, column spec lengths differ");
  if (!(nrow >= 0)) cpp11::stop("fstx: bad row count");
  const uint64_t n = (uint64_t)nrow;

  // Column definitions. All R-allocating work happens here, before the engine starts.
  std::vector<std::string> col_names((size_t)nc);
  std::vector<std::vector<std::string>> lv((size_t)nc);
  std::vector<std::vector<const char*>> lv_ptr((size_t)nc);
  std::vector<std::vector<uint32_t>> lv_len((size_t)nc);
  std::vector<std::string> tz((size_t)nc);
  std::vector<fstx_write_column> defs((size_t)nc);
  std::vector<fstx_column_data> data((size_t)nc);
  std::vector<StrSrc> srcs((size_t)nc);
  for (R_xlen_t j = 0; j < nc; ++j) {
    fstx_write_column& d = defs[(size_t)j];
    std::memset(&d, 0, sizeof(d));
    col_names[(size_t)j] = utf8_of(STRING_ELT(names, j));
    d.name = col_names[(size_t)j].c_str();
    d.name_len = (uint32_t)col_names[(size_t)j].size();
    d.phys = INTEGER(phys)[j];
    d.logical = INTEGER(logical)[j];
    d.ordered = LOGICAL(ordered)[j] == TRUE;
    SEXP l = VECTOR_ELT(levels, j);
    if (l != R_NilValue) {
      R_xlen_t nl = Rf_xlength(l);
      for (R_xlen_t i = 0; i < nl; ++i) lv[(size_t)j].push_back(utf8_of(STRING_ELT(l, i)));
      for (const std::string& s : lv[(size_t)j]) {
        lv_ptr[(size_t)j].push_back(s.data());
        lv_len[(size_t)j].push_back((uint32_t)s.size());
      }
      d.n_levels = (uint32_t)nl;
      d.levels = lv_ptr[(size_t)j].data();
      d.level_lens = lv_len[(size_t)j].data();
    }
    SEXP t = VECTOR_ELT(tzone, j);
    if (t != R_NilValue) {
      tz[(size_t)j] = utf8_of(STRING_ELT(t, 0));
      d.has_tzone = 1;
      d.tzone = tz[(size_t)j].c_str();
      d.tzone_len = (uint32_t)tz[(size_t)j].size();
    }
    SEXP a = VECTOR_ELT(attrs, j);
    if (a != R_NilValue) {
      d.attrs = RAW(a);
      d.attrs_len = (size_t)Rf_xlength(a);
    }
    SEXP x = VECTOR_ELT(cols, j);
    if ((uint64_t)Rf_xlength(x) != n) cpp11::stop("fstx: column %d has the wrong length", (int)j + 1);
    fstx_column_data& cd = data[(size_t)j];
    std::memset(&cd, 0, sizeof(cd));
    switch (TYPEOF(x)) {
      case INTSXP:
        if (d.phys != FSTX_INT32) cpp11::stop("fstx: internal type mismatch");
        cd.data = INTEGER_RO(x);  // ALTREP sequences are expanded here, on the main thread
        break;
      case LGLSXP:
        if (d.phys != FSTX_LOGICAL) cpp11::stop("fstx: internal type mismatch");
        cd.data = LOGICAL_RO(x);
        break;
      case REALSXP:
        if (d.phys != FSTX_FLOAT64) cpp11::stop("fstx: internal type mismatch");
        cd.data = REAL_RO(x);
        break;
      case STRSXP:
        if (d.phys != FSTX_STRING) cpp11::stop("fstx: internal type mismatch");
        srcs[(size_t)j] = StrSrc{STRING_PTR_RO(x), NA_STRING, utf8_locale};
        cd.get_strings = get_strings;
        cd.ctx = &srcs[(size_t)j];
        break;
      default:
        cpp11::stop("fstx: column %d has an unsupported type", (int)j + 1);
    }
  }
  const uint8_t* ta = nullptr;
  size_t ta_len = 0;
  if (table_attrs != R_NilValue) {
    ta = RAW(table_attrs);
    ta_len = (size_t)Rf_xlength(table_attrs);
  }

  fstx_write_options opt;
  std::memset(&opt, 0, sizeof(opt));
  opt.codec = codec;
  opt.level = level;
  opt.threads = threads;
  opt.profile = profile;
  opt.keep_pct = keep_pct;
  opt.force_encoding = force_encoding;
  opt.test_crash = test_crash;

  // From here on no R API calls until the engine is done.
  fstx_error err;
  fstx_writer* w = nullptr;
  const int orc = append ? fstx_writer_open_append(p.c_str(), defs.data(), (uint32_t)nc, ta, ta_len, &opt, &w, &err)
                         : fstx_writer_open(p.c_str(), defs.data(), (uint32_t)nc, ta, ta_len, &opt, &w, &err);
  if (orc != FSTX_OK) stop_err(err);
  if (fstx_writer_append(w, data.data(), n, &err) != FSTX_OK) {
    fstx_writer_abort(w);
    if (err.code == FSTX_ERR_TRANSLATE && err.column >= 0) return (int)err.column + 1;
    stop_err(err);
  }
  if (fstx_writer_close(w, &err) != FSTX_OK) stop_err(err);
  return 0;
}

// ---------------------------------------------------------------- reading
namespace {

struct ReadCtx {
  SEXP out;    // protected list of output vectors (all selected columns)
  SEXP dicts;  // protected list: the current string dictionary of each planned column
  std::vector<uint32_t> map;  // planned column -> position in out
  bool pending = false;
  SEXP token = R_NilValue;
};

void* alloc_column(void* ctx, uint32_t j, uint32_t, int phys, uint64_t n) {
  ReadCtx* c = (ReadCtx*)ctx;
  SEXPTYPE t = phys == FSTX_FLOAT64 ? REALSXP : phys == FSTX_LOGICAL ? LGLSXP : phys == FSTX_STRING ? STRSXP : INTSXP;
  try {
    SEXP v = cpp11::unwind_protect([&] {
      SEXP x = Rf_allocVector(t, (R_xlen_t)n);
      SET_VECTOR_ELT(c->out, c->map[j], x);
      return x;
    });
    switch (t) {
      case REALSXP: return n ? (void*)REAL(v) : (void*)c;
      case LGLSXP: return n ? (void*)LOGICAL(v) : (void*)c;
      case INTSXP: return n ? (void*)INTEGER(v) : (void*)c;
      default: return (void*)c;  // strings arrive through on_strings
    }
  } catch (const cpp11::unwind_exception& e) {
    c->pending = true;
    c->token = e.token;
    return nullptr;
  }
}

int on_strings(void* ctx, uint32_t j, uint64_t dest, uint32_t n, const int32_t* lens, const char* bytes) {
  ReadCtx* c = (ReadCtx*)ctx;
  try {
    cpp11::unwind_protect([&] {
      SEXP v = VECTOR_ELT(c->out, c->map[j]);
      const char* p = bytes;
      for (uint32_t i = 0; i < n; ++i) {
        int32_t l = lens[i];
        if (l < 0) {
          SET_STRING_ELT(v, (R_xlen_t)(dest + i), NA_STRING);
        } else {
          SET_STRING_ELT(v, (R_xlen_t)(dest + i), Rf_mkCharLenCE(p, l, CE_UTF8));
          p += l;
        }
      }
    });
    return 0;
  } catch (const cpp11::unwind_exception& e) {
    c->pending = true;
    c->token = e.token;
    return 1;
  }
}

int on_strings_at(void* ctx, uint32_t j, uint32_t n, const uint64_t* dest, const int32_t* lens, const char* bytes) {
  ReadCtx* c = (ReadCtx*)ctx;
  try {
    cpp11::unwind_protect([&] {
      SEXP v = VECTOR_ELT(c->out, c->map[j]);
      const char* p = bytes;
      for (uint32_t i = 0; i < n; ++i) {
        int32_t l = lens[i];
        if (l < 0) {
          SET_STRING_ELT(v, (R_xlen_t)dest[i], NA_STRING);
        } else {
          SET_STRING_ELT(v, (R_xlen_t)dest[i], Rf_mkCharLenCE(p, l, CE_UTF8));
          p += l;
        }
      }
    });
    return 0;
  } catch (const cpp11::unwind_exception& e) {
    c->pending = true;
    c->token = e.token;
    return 1;
  }
}

int on_dict(void* ctx, uint32_t j, uint32_t n, const int32_t* lens, const char* bytes) {
  ReadCtx* c = (ReadCtx*)ctx;
  try {
    cpp11::unwind_protect([&] {
      SEXP d = Rf_allocVector(STRSXP, (R_xlen_t)n);
      SET_VECTOR_ELT(c->dicts, j, d);  // protected from here on
      const char* p = bytes;
      for (uint32_t i = 0; i < n; ++i) {
        const int32_t l = lens[i] < 0 ? 0 : lens[i];
        SET_STRING_ELT(d, (R_xlen_t)i, Rf_mkCharLenCE(p, l, CE_UTF8));
        p += l;
      }
    });
    return 0;
  } catch (const cpp11::unwind_exception& e) {
    c->pending = true;
    c->token = e.token;
    return 1;
  }
}

// No allocation here: every output string is a CHARSXP the dictionary already holds.
int on_codes(void* ctx, uint32_t j, uint64_t dest, uint32_t n, const int32_t* codes) {
  ReadCtx* c = (ReadCtx*)ctx;
  SEXP v = VECTOR_ELT(c->out, c->map[j]);
  SEXP d = VECTOR_ELT(c->dicts, j);
  if (TYPEOF(d) != STRSXP) return 1;
  const R_xlen_t nd = Rf_xlength(d);
  const SEXP* dp = STRING_PTR_RO(d);
  for (uint32_t i = 0; i < n; ++i) {
    const int32_t k = codes[i];
    if (k >= nd) return 1;
    SET_STRING_ELT(v, (R_xlen_t)(dest + i), k < 0 ? NA_STRING : dp[k]);
  }
  return 0;
}

SEXP raw_of(const uint8_t* p, size_t n) {
  if (!p || !n) return R_NilValue;
  cpp11::writable::raws r((R_xlen_t)n);
  std::memcpy(RAW(r), p, n);
  return r;
}

SEXP str1(const char* p, uint32_t n) { return cpp11::safe[Rf_mkCharLenCE](p, (int)n, CE_UTF8); }

}  // namespace

[[cpp11::register]]
// mode: 0 eager, 1 deferred strings, 2 lazy (every column decoded from the mapped file on use), 3 auto (M6s:
// deferred strings that serialise in R's standard format).
// as_factor: NULL, TRUE (all-dictionary string columns) or column names. presize: an R function(nodes, bytes)
// called before an eager read that builds many R strings, or NULL.
SEXP fstx_read_impl(SEXP path, SEXP columns, double from, double to, int threads, bool verify, int mode,
                    SEXP as_factor, SEXP presize, SEXP rows) {
  FileRef f = open_shared_or_stop(path_of(path));
  fstx_file_info info;
  fstx_file_info_get(f.get(), &info);

  // rows (M5): NULL, or 1-based row numbers in any order (repeats allowed)
  const bool by_rows = rows != R_NilValue;
  std::vector<uint64_t> rv;
  if (by_rows) {
    if (TYPEOF(rows) != INTSXP && TYPEOF(rows) != REALSXP) cpp11::stop("fstx: 'rows' must be a numeric vector");
    const R_xlen_t n = Rf_xlength(rows);
    rv.resize((size_t)n);
    const double nrow = (double)info.n_rows;
    if (TYPEOF(rows) == INTSXP) {
      const int* x = INTEGER(rows);
      for (R_xlen_t i = 0; i < n; ++i) {
        if (x[i] == NA_INTEGER) cpp11::stop("fstx: 'rows' must not contain NA (element %.0f)", (double)i + 1);
        if (x[i] < 1 || (double)x[i] > nrow)
          cpp11::stop("fstx: row %d is out of range (the file has %.0f rows)", x[i], nrow);
        rv[(size_t)i] = (uint64_t)x[i] - 1;
      }
    } else {
      const double* x = REAL(rows);
      for (R_xlen_t i = 0; i < n; ++i) {
        if (ISNAN(x[i])) cpp11::stop("fstx: 'rows' must not contain NA (element %.0f)", (double)i + 1);
        if (x[i] != std::floor(x[i])) cpp11::stop("fstx: 'rows' must be whole numbers (element %.0f is %g)", (double)i + 1, x[i]);
        if (x[i] < 1 || x[i] > nrow) cpp11::stop("fstx: row %.0f is out of range (the file has %.0f rows)", x[i], nrow);
        rv[(size_t)i] = (uint64_t)x[i] - 1;
      }
    }
    mode = 0;  // lazy and deferred columns read a contiguous range: a row list is read eagerly
  }

  // columns: NULL (all), names, or 1-based positions
  std::vector<uint32_t> idx;
  if (columns == R_NilValue) {
    for (uint32_t c = 0; c < info.n_cols; ++c) idx.push_back(c);
  } else if (TYPEOF(columns) == STRSXP) {
    for (R_xlen_t i = 0; i < Rf_xlength(columns); ++i) {
      SEXP s = STRING_ELT(columns, i);
      if (s == NA_STRING) cpp11::stop("fstx: column names must not be NA");
      std::string nm = utf8_of(s);
      int64_t c = fstx_find_column(f.get(), nm.data(), nm.size());
      if (c < 0) cpp11::stop("fstx: column '%s' is not in the file", nm.c_str());
      idx.push_back((uint32_t)c);
    }
  } else if (TYPEOF(columns) == INTSXP || TYPEOF(columns) == REALSXP) {
    for (R_xlen_t i = 0; i < Rf_xlength(columns); ++i) {
      double v = TYPEOF(columns) == INTSXP ? (INTEGER(columns)[i] == NA_INTEGER ? NA_REAL : INTEGER(columns)[i])
                                           : REAL(columns)[i];
      if (ISNAN(v) || v < 1 || v > info.n_cols || v != (double)(int64_t)v)
        cpp11::stop("fstx: column position %g is out of range (the file has %u columns)", v, info.n_cols);
      idx.push_back((uint32_t)(v - 1));
    }
  } else {
    cpp11::stop("fstx: 'columns' must be NULL, a character vector of names or column positions");
  }

  // rows: from/to are 1-based and inclusive; to = NA means the last row; to past the end is clamped
  const double nr = (double)info.n_rows;
  if (ISNAN(from) || from < 1 || from != (double)(int64_t)from) cpp11::stop("fstx: 'from' must be a whole number >= 1");
  double t = ISNAN(to) ? nr : to;
  if (t != (double)(int64_t)t && !ISNAN(to)) cpp11::stop("fstx: 'to' must be a whole number");
  if (t > nr) t = nr;
  if (from > nr + 1 || (from > nr && info.n_rows > 0)) cpp11::stop("fstx: 'from' (%.0f) is beyond the last row (%.0f)", from, nr);
  if (t < from - 1) cpp11::stop("fstx: 'to' must not be smaller than 'from'");
  const uint64_t r0 = (uint64_t)(from - 1), r1 = (uint64_t)t;
  const uint64_t out_rows = by_rows ? (uint64_t)rv.size() : r1 - r0;

  const R_xlen_t nc = (R_xlen_t)idx.size();
  std::vector<int> phys(idx.size());
  std::vector<std::string> cname(idx.size());
  for (size_t j = 0; j < idx.size(); ++j) {
    fstx_column_info ci;
    fstx_column_info_get(f.get(), idx[j], &ci);
    phys[j] = ci.phys;
    cname[j].assign(ci.name, ci.name_len);
  }

  // as_factor: 1 = convert if every page is dictionary-coded, 2 = named (falls back to factor() in R)
  std::vector<int> fflag(idx.size(), 0);
  if (TYPEOF(as_factor) == LGLSXP && Rf_xlength(as_factor) == 1 && LOGICAL(as_factor)[0] == TRUE) {
    for (size_t j = 0; j < idx.size(); ++j)
      if (phys[j] == FSTX_STRING) fflag[j] = 1;
  } else if (TYPEOF(as_factor) == STRSXP) {
    for (R_xlen_t i = 0; i < Rf_xlength(as_factor); ++i) {
      SEXP s = STRING_ELT(as_factor, i);
      if (s == NA_STRING) cpp11::stop("fstx: 'as_factor' names must not be NA");
      const std::string want = utf8_of(s);
      bool found = false;
      for (size_t j = 0; j < idx.size(); ++j) {
        if (cname[j] != want) continue;
        found = true;
        if (phys[j] != FSTX_STRING)
          cpp11::stop("fstx: 'as_factor' names column '%s', which is not a character column", want.c_str());
        fflag[j] = 2;
      }
      if (!found) cpp11::stop("fstx: 'as_factor' names column '%s', which is not among the columns read", want.c_str());
    }
  } else if (as_factor != R_NilValue && !(TYPEOF(as_factor) == LGLSXP && Rf_xlength(as_factor) == 1)) {
    cpp11::stop("fstx: 'as_factor' must be TRUE, FALSE or column names");
  }

  cpp11::writable::list out(nc);
  cpp11::writable::list fdicts(nc);
  cpp11::writable::logicals fallback(nc);
  for (R_xlen_t j = 0; j < nc; ++j) fallback[j] = FALSE;
  fstx_error err;

  // Wraps an engine source into an external pointer that owns it (and, for lazy columns, the open file).
  auto make_xp = [&](fstx_colsrc* src, bool lazy, bool keep_file, bool portable = false) -> SEXP {
    std::unique_ptr<Holder> h(new Holder());
    h->src = src;
    h->portable = portable;
    if (keep_file) h->file = f;
    h->threads = threads;
    h->lazy = lazy;
    if (lazy) fstx_colsrc_set_threads(src, threads);
    fstx_holder_sync(h.get());
    SEXP xp = cpp11::safe[fstx_holder_xp]();
    R_SetExternalPtrAddr(xp, h.release());  // no allocation: the external pointer owns the holder from here
    return xp;
  };

  // how: 0 eager (planned read), 1 deferred strings, 2 lazy, 3 factor codes from dictionaries,
  // 4 eager strings (M6s: decoded like deferred strings, then every R string made inside R's Dataptr call)
  std::vector<int> how(idx.size(), 0);
  for (size_t j = 0; j < idx.size(); ++j) {
    if (fflag[j] && by_rows) {  // dictionary codes need the whole range: factor() in R instead
      fallback[(R_xlen_t)j] = TRUE;
    } else if (fflag[j]) {
      fstx_colsrc* src = nullptr;
      if (fstx_colsrc_open(f.get(), idx[j], r0, r1, verify ? 1 : 0, 1, &src, &err) != FSTX_OK) stop_err(err);
      fstx_colsrc_info si;
      fstx_colsrc_info_get(src, &si);
      if (si.rows == 0 || !si.all_dict) {
        fstx_colsrc_free(src);
        if (fflag[j] == 2 || si.rows == 0) fallback[(R_xlen_t)j] = TRUE;
      } else {
        how[j] = 3;
        cpp11::sexp xp = make_xp(src, mode == 2, true);  // the codes are decoded after R has the levels
        Holder* h = fstx_holder_of(xp);
        cpp11::writable::list dl((R_xlen_t)si.n_dicts);
        for (uint32_t d = 0; d < si.n_dicts; ++d) {
          uint32_t n;
          const int32_t* lens;
          const char* bytes;
          if (fstx_colsrc_dict(h->src, d, &n, &lens, &bytes, &err) != FSTX_OK) stop_err(err);
          cpp11::writable::strings dv((R_xlen_t)n);
          for (uint32_t i = 0; i < n; ++i) {
            SET_STRING_ELT(dv, (R_xlen_t)i, str1(bytes, (uint32_t)lens[i]));
            bytes += lens[i];
          }
          dl[(R_xlen_t)d] = dv;
        }
        out[(R_xlen_t)j] = xp;
        fdicts[(R_xlen_t)j] = dl;
        continue;
      }
    }
    if (mode == 2) how[j] = 2;
    else if ((mode == 1 || mode == 3) && phys[j] == FSTX_STRING) how[j] = 1;
    else if (mode == 0 && !by_rows && phys[j] == FSTX_STRING) how[j] = 4;
  }

  // eager columns: one planned parallel read
  std::vector<uint32_t> pidx, pmap;
  for (size_t j = 0; j < idx.size(); ++j)
    if (how[j] == 0) {
      pidx.push_back(idx[j]);
      pmap.push_back((uint32_t)j);
    }
  if (!pidx.empty()) {
    fstx_plan* praw = nullptr;
    const int prc = by_rows ? fstx_plan_create_rows(f.get(), pidx.data(), (uint32_t)pidx.size(), rv.data(),
                                                    (uint64_t)rv.size(), &praw, &err)
                            : fstx_plan_create(f.get(), pidx.data(), (uint32_t)pidx.size(), r0, r1, &praw, &err);
    if (prc != FSTX_OK) stop_err(err);
    PlanPtr plan(praw);
    if (presize != R_NilValue) {
      uint64_t prow = 0, pbytes = 0;
      fstx_plan_string_stats(plan.get(), &prow, &pbytes);
      if (prow >= 100000) {
        cpp11::function pf(presize);
        pf((double)prow, (double)pbytes);
      }
    }
    cpp11::writable::list dicts((R_xlen_t)pidx.size());
    ReadCtx ctx;
    ctx.out = out;
    ctx.dicts = dicts;
    ctx.map = pmap;
    fstx_read_host host;
    std::memset(&host, 0, sizeof(host));
    host.ctx = &ctx;
    host.alloc_column = alloc_column;
    host.on_strings = on_strings;
    host.on_dict = on_dict;
    host.on_codes = on_codes;
    host.on_strings_at = on_strings_at;
    int rc = fstx_read(plan.get(), &host, threads, verify ? 1 : 0, &err);
    if (ctx.pending) throw cpp11::unwind_exception(ctx.token);  // an R error inside a callback: re-raise it now
    if (rc != FSTX_OK) stop_err(err);
  }

  // deferred strings (decoded in parallel now, R strings made on use), lazy columns and eager strings
  for (size_t j = 0; j < idx.size(); ++j) {
    if (how[j] != 1 && how[j] != 2 && how[j] != 4) continue;
    const bool lazy = how[j] == 2;
    fstx_colsrc* src = nullptr;
    if (fstx_colsrc_open(f.get(), idx[j], r0, r1, verify ? 1 : 0, lazy ? 0 : 1, &src, &err) != FSTX_OK) stop_err(err);
    cpp11::sexp xp = make_xp(src, lazy, lazy, mode == 3);
    bool plain = how[j] == 4;
    if (!lazy) {
      Holder* h = fstx_holder_of(xp);
      if (fstx_colsrc_load(h->src, nullptr, threads, &err) != FSTX_OK) stop_err(err);
      // M6.1: strings = "auto" returns an all-dictionary (low-cardinality) column as a plain vector at once. It
      // costs one R string per dictionary entry plus a pointer per row, and later ops (table, factor, split,
      // grouping) then skip the ALTREP layer, which costs them about 2x on a finished column.
      // 1.0: such a column (auto or eager) is made in one pass straight from the codes (str_dict_fill in
      // r_lazy.cpp, inside Dataptr with R's collector off); it skips the detach copy, as it is made at once.
      fstx_colsrc_info si;
      fstx_colsrc_info_get(h->src, &si);
      if (mode == 3 && si.all_dict) plain = true;
      if (how[j] == 1 && !plain && fstx_colsrc_detach(h->src, &err) != FSTX_OK) stop_err(err);
    }
    cpp11::sexp a = cpp11::safe[fstx_altrep_new](xp);
    if (plain) {
      out[(R_xlen_t)j] = cpp11::safe[fstx_str_plain](a);  // a plain STRSXP; the ALTREP shell is dropped
    } else {
      out[(R_xlen_t)j] = a;
    }
  }

  // R types on top: factor levels/class, Date, POSIXct + tzone. Other attributes come back as blobs.
  cpp11::writable::strings nm(nc);
  cpp11::writable::list col_attrs(nc);
  for (R_xlen_t j = 0; j < nc; ++j) {
    fstx_column_info ci;
    fstx_column_info_get(f.get(), idx[(size_t)j], &ci);
    nm[j] = cpp11::r_string(str1(ci.name, ci.name_len));
    col_attrs[j] = raw_of(ci.attrs, ci.attrs_len);
    if (how[(size_t)j] == 3) continue;  // factor from dictionaries: finished in R
    SEXP v = out[j];
    if (ci.logical == FSTX_LT_FACTOR) {
      cpp11::writable::strings lv((R_xlen_t)ci.n_levels);
      for (uint32_t i = 0; i < ci.n_levels; ++i) {
        const char* s;
        uint32_t len;
        fstx_level_get(f.get(), idx[(size_t)j], i, &s, &len);
        SET_STRING_ELT(lv, i, str1(s, len));
      }
      cpp11::safe[Rf_setAttrib](v, R_LevelsSymbol, (SEXP)lv);
      if (ci.ordered) {
        cpp11::writable::strings cl({"ordered", "factor"});
        cpp11::safe[Rf_setAttrib](v, R_ClassSymbol, (SEXP)cl);
      } else {
        cpp11::writable::strings cl({"factor"});
        cpp11::safe[Rf_setAttrib](v, R_ClassSymbol, (SEXP)cl);
      }
    } else if (ci.logical == FSTX_LT_DATE) {
      cpp11::writable::strings cl({"Date"});
      cpp11::safe[Rf_setAttrib](v, R_ClassSymbol, (SEXP)cl);
    } else if (ci.logical == FSTX_LT_POSIXCT) {
      cpp11::writable::strings cl({"POSIXct", "POSIXt"});
      cpp11::safe[Rf_setAttrib](v, R_ClassSymbol, (SEXP)cl);
      if (ci.has_tzone) {
        cpp11::writable::strings tzs(1);
        SET_STRING_ELT(tzs, 0, str1(ci.tzone, ci.tzone_len));
        cpp11::safe[Rf_setAttrib](v, cpp11::safe[Rf_install]("tzone"), (SEXP)tzs);
      }
    }
  }
  cpp11::sexp ta = raw_of(info.table_attrs, info.table_attrs_len);
  using namespace cpp11::literals;
  return cpp11::writable::list({"columns"_nm = out, "names"_nm = nm, "col_attrs"_nm = col_attrs, "table_attrs"_nm = ta,
                                "nrow"_nm = cpp11::as_sexp((double)out_rows), "all_columns"_nm = cpp11::as_sexp(columns == R_NilValue),
                                "file_rows"_nm = cpp11::as_sexp(nr),
                                "factor_dicts"_nm = fdicts, "factor_fallback"_nm = fallback});
}

[[cpp11::register]]
SEXP fstx_meta_impl(SEXP path) {
  FilePtr f = open_or_stop(path_of(path));
  fstx_file_info info;
  fstx_file_info_get(f.get(), &info);
  const R_xlen_t nc = info.n_cols;
  static const char* phys_names[] = {"integer", "double", "logical", "character"};
  static const char* lt_names[] = {"", "factor", "Date", "POSIXct"};
  cpp11::writable::strings name(nc), phys(nc), logical(nc), tz(nc);
  cpp11::writable::doubles comp(nc), raw(nc), pages(nc), nlev(nc);
  cpp11::writable::list attrs(nc);
  for (R_xlen_t j = 0; j < nc; ++j) {
    fstx_column_info ci;
    fstx_column_info_get(f.get(), (uint32_t)j, &ci);
    name[j] = cpp11::r_string(str1(ci.name, ci.name_len));
    phys[j] = cpp11::r_string(phys_names[ci.phys]);
    logical[j] = cpp11::r_string(lt_names[ci.logical]);
    if (ci.has_tzone) tz[j] = cpp11::r_string(str1(ci.tzone, ci.tzone_len));
    else tz[j] = cpp11::r_string(NA_STRING);
    comp[j] = (double)ci.compressed_bytes;
    raw[j] = (double)ci.raw_bytes;
    pages[j] = (double)ci.n_pages;
    nlev[j] = (double)ci.n_levels;
    attrs[j] = raw_of(ci.attrs, ci.attrs_len);
  }
  using namespace cpp11::literals;
  return cpp11::writable::list(
      {"nrow"_nm = cpp11::as_sexp((double)info.n_rows), "ncol"_nm = cpp11::as_sexp((double)info.n_cols),
       "file_size"_nm = cpp11::as_sexp((double)info.file_size), "row_groups"_nm = cpp11::as_sexp((double)info.n_row_groups),
       "pages"_nm = cpp11::as_sexp((double)info.n_pages), "row_group_size"_nm = cpp11::as_sexp((double)info.row_group_size),
       "format_version"_nm = cpp11::as_sexp((double)info.format_version),
       "profile"_nm = cpp11::as_sexp(info.profile == FSTX_PROFILE_FAST       ? "fast"
                                     : info.profile == FSTX_PROFILE_SMALL    ? "small"
                                     : info.profile == FSTX_PROFILE_BALANCED ? "balanced"
                                                                             : "unknown"),
       "name"_nm = name, "phys"_nm = phys,
       "logical"_nm = logical, "tzone"_nm = tz, "compressed_bytes"_nm = comp, "raw_bytes"_nm = raw,
       "col_pages"_nm = pages, "n_levels"_nm = nlev, "col_attrs"_nm = attrs,
       "table_attrs"_nm = raw_of(info.table_attrs, info.table_attrs_len)});
}

// One row per stored page (dictionary pages included), for fstx_pages().
[[cpp11::register]]
SEXP fstx_pages_impl(SEXP path) {
  FilePtr f = open_or_stop(path_of(path));
  const int64_t np = fstx_pages_get(f.get(), nullptr, 0);
  std::vector<fstx_page_info> pi((size_t)np);
  if (np) fstx_pages_get(f.get(), pi.data(), (uint64_t)np);
  const R_xlen_t n = (R_xlen_t)np;
  static const char* codec_names[] = {"none", "lz4", "zstd"};
  static const char* na_names[] = {"none", "bitmap", "runs"};
  cpp11::writable::doubles rg(n), col(n), first(n), rows(n), nulls(n), off(n), comp(n), raw(n);
  cpp11::writable::logicals dict(n);
  cpp11::writable::strings codec(n), kind(n), na(n), e0(n), e1(n), e2(n);
  for (R_xlen_t i = 0; i < n; ++i) {
    const fstx_page_info& p = pi[(size_t)i];
    rg[i] = p.row_group + 1.0;
    col[i] = p.column + 1.0;
    dict[i] = p.is_dictionary ? TRUE : FALSE;
    first[i] = (double)p.first_row + 1.0;
    rows[i] = p.row_count;
    nulls[i] = p.null_count;
    off[i] = (double)p.offset;
    comp[i] = p.comp_len;
    raw[i] = p.raw_len;
    codec[i] = cpp11::r_string(p.codec >= 0 && p.codec <= 2 ? codec_names[p.codec] : "?");
    kind[i] = cpp11::r_string(fstx_kind_name(p.kind));
    na[i] = cpp11::r_string(p.na_mode >= 0 && p.na_mode <= 2 ? na_names[p.na_mode] : "?");
    e0[i] = cpp11::r_string(fstx_encoding_name(p.enc[0]));
    e1[i] = cpp11::r_string(fstx_encoding_name(p.enc[1]));
    e2[i] = cpp11::r_string(fstx_encoding_name(p.enc[2]));
  }
  using namespace cpp11::literals;
  return cpp11::writable::list({"row_group"_nm = rg, "column"_nm = col, "dictionary"_nm = dict, "first_row"_nm = first,
                                "rows"_nm = rows, "nulls"_nm = nulls, "offset"_nm = off, "comp_bytes"_nm = comp,
                                "raw_bytes"_nm = raw, "codec"_nm = codec, "kind"_nm = kind, "na"_nm = na,
                                "encoding"_nm = e0, "child1"_nm = e1, "child2"_nm = e2});
}

// preds: list of list(col = name, op = int, value = double, set = double, set_na, set_nan, strs = character).
// Returns list(rows = 1-based matching rows (double), stats = named doubles).
[[cpp11::register]]
SEXP fstx_filter_impl(SEXP path, SEXP preds, double from, double to, int threads, bool verify, bool plan_only) {
  FilePtr f = open_or_stop(path_of(path));
  fstx_file_info info;
  fstx_file_info_get(f.get(), &info);
  const R_xlen_t np = Rf_xlength(preds);
  std::vector<fstx_pred> ps((size_t)np);
  std::vector<std::vector<std::string>> strs((size_t)np);
  std::vector<std::vector<const char*>> sp((size_t)np);
  std::vector<std::vector<int32_t>> sl((size_t)np);
  auto field = [](SEXP l, const char* nm) -> SEXP {
    SEXP names = Rf_getAttrib(l, R_NamesSymbol);
    for (R_xlen_t i = 0; i < Rf_xlength(l); ++i)
      if (std::strcmp(CHAR(STRING_ELT(names, i)), nm) == 0) return VECTOR_ELT(l, i);
    return R_NilValue;
  };
  for (R_xlen_t i = 0; i < np; ++i) {
    SEXP q = VECTOR_ELT(preds, i);
    fstx_pred& d = ps[(size_t)i];
    std::memset(&d, 0, sizeof(d));
    SEXP col = field(q, "col");
    if (TYPEOF(col) != STRSXP || Rf_xlength(col) != 1) cpp11::stop("fstx: internal error, bad filter column");
    const std::string nm = utf8_of(STRING_ELT(col, 0));
    const int64_t c = fstx_find_column(f.get(), nm.data(), nm.size());
    if (c < 0) cpp11::stop("fstx: column '%s' is not in the file", nm.c_str());
    d.col = (uint32_t)c;
    d.op = Rf_asInteger(field(q, "op"));
    SEXP v = field(q, "value");
    d.value = v == R_NilValue ? NA_REAL : Rf_asReal(v);
    SEXP set = field(q, "set");
    if (set != R_NilValue) {
      d.set = REAL(set);
      d.n_set = (uint64_t)Rf_xlength(set);
    }
    SEXP sna = field(q, "set_na"), snan = field(q, "set_nan");
    d.set_na = sna != R_NilValue && Rf_asLogical(sna) == TRUE;
    d.set_nan = snan != R_NilValue && Rf_asLogical(snan) == TRUE;
    SEXP st = field(q, "strs");
    if (st != R_NilValue) {
      for (R_xlen_t k = 0; k < Rf_xlength(st); ++k) {
        SEXP e = STRING_ELT(st, k);
        if (e == NA_STRING) continue;  // NA in a set is set_na
        strs[(size_t)i].push_back(utf8_of(e));
      }
      for (const std::string& x : strs[(size_t)i]) {
        sp[(size_t)i].push_back(x.data());
        sl[(size_t)i].push_back((int32_t)x.size());
      }
      d.strs = sp[(size_t)i].data();
      d.str_lens = sl[(size_t)i].data();
      d.n_strs = (uint64_t)strs[(size_t)i].size();
    }
  }
  const double nr = (double)info.n_rows;
  double t = ISNAN(to) ? nr : std::min(to, nr);
  if (ISNAN(from) || from < 1) from = 1;
  if (t < from - 1) t = from - 1;
  fstx_selection* sel = nullptr;
  fstx_filter_stats stt;
  fstx_error err;
  if (fstx_filter(f.get(), ps.data(), (uint32_t)np, (uint64_t)(from - 1), (uint64_t)t, threads, verify ? 1 : 0,
                  plan_only ? 1 : 0, &sel, &stt, &err) != FSTX_OK)
    stop_err(err);
  uint64_t n = 0;
  const uint64_t* r = fstx_selection_rows(sel, &n);
  SEXP out;
  try {
    out = cpp11::safe[Rf_allocVector](REALSXP, (R_xlen_t)n);
  } catch (...) {
    fstx_selection_free(sel);
    throw;
  }
  cpp11::sexp keep(out);
  double* o = REAL(out);
  for (uint64_t i = 0; i < n; ++i) o[i] = (double)r[i] + 1.0;
  fstx_selection_free(sel);
  using namespace cpp11::literals;
  cpp11::writable::doubles stats({(double)stt.pages_total, (double)stt.pages_skipped, (double)stt.pages_pruned,
                                  (double)stt.pages_all, (double)stt.pages_read, (double)stt.rows_window,
                                  (double)stt.rows_candidate, (double)stt.rows_matched});
  stats.names() = {"pages_total", "pages_skipped", "pages_pruned", "pages_all", "pages_read", "rows_window",
                   "rows_candidate", "rows_matched"};
  return cpp11::writable::list({"rows"_nm = keep, "stats"_nm = stats});
}

[[cpp11::register]]
void fstx_compact_impl(SEXP path, SEXP tmp) {
  const std::string p = path_of(path), t = path_of(tmp);
  fstx_error err;
  if (fstx_compact_copy(p.c_str(), t.c_str(), &err) != FSTX_OK) stop_err(err);
}

[[cpp11::register]]
SEXP fstx_sizes_impl(SEXP path) {
  const std::string p = path_of(path);
  uint64_t a = 0, b = 0, c = 0;
  fstx_error err;
  if (fstx_file_sizes(p.c_str(), &a, &b, &c, &err) != FSTX_OK) stop_err(err);
  cpp11::writable::doubles r({(double)a, (double)b, (double)c});
  r.names() = {"committed", "physical", "valid_end"};
  return r;
}

[[cpp11::register]]
int fstx_hw_threads() { return (int)fstx_hardware_threads(); }

extern "C" void R_unload_fstx(DllInfo*) { fstx_shutdown(); }
