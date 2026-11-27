// FST-X Arrow stream, row-list batches (rows given: a filter's matches or a list of rows). MIT licence.
// Part of r_arrow.cpp: included once, inside its anonymous namespace, after the helpers it uses (array_init,
// make_utf8, Validity, ...). Kept in its own file so that the range path (r_arrow.cpp) and this path can change
// apart. Not a header of its own: it has no include guard and compiles only inside r_arrow.cpp (and so in the fuzz
// target, which includes r_arrow.cpp).
//
// A batch is read through the engine's rows plan (fstx_plan_create_rows + fstx_read): only the pages that hold
// requested rows are read, pages without a codec decode only the 1,024-row vectors needed, and the pages of all
// columns decode side by side on the engine pool. Fixed-width values land in the Arrow buffer (or in a buffer of the
// same width that is converted in place), strings are gathered per page by the workers and appended here in row
// order. Columns that leave as dictionaries (every page dictionary-coded) take their codes from a column source.

struct RowsCol {
  size_t j = 0;               // stream column
  ArrowArray* out = nullptr;  // its child array
  void* vals = nullptr;       // fixed width: where fstx_read writes the stored values (n of them)
  std::vector<uint8_t> tmp;   // ... when they cannot go to the Arrow buffer as they are
  // strings
  void* off = nullptr;  // int32 or int64 offsets, n + 1
  bool big = false;
  std::vector<char> bytes;
  uint64_t tot = 0;
  std::unique_ptr<Validity> v;
};

struct RowsHost {
  const Stream* S = nullptr;
  uint64_t n = 0;
  std::vector<RowsCol>* cols = nullptr;  // in plan order
  std::string error;
};

void* rows_alloc(void* ctx, uint32_t k, uint32_t, int phys, uint64_t n) {
  RowsHost& H = *(RowsHost*)ctx;
  RowsCol& R = (*H.cols)[k];
  const ColSpec& c = H.S->cols[R.j];
  try {
    if (phys == FSTX_STRING) {
      array_init(R.out, (int64_t)n, 3);
      R.big = c.out == O_LARGE_UTF8;
      R.off = array_buf(R.out, 1, (size_t)(n + 1) * (R.big ? 8 : 4));
      if (R.big) ((int64_t*)R.off)[0] = 0;
      else ((int32_t*)R.off)[0] = 0;
      R.v.reset(new Validity{R.out, n});
      return R.off;
    }
    array_init(R.out, (int64_t)n, 2);
    const size_t w = phys == FSTX_FLOAT64 ? 8 : 4;
    // the Arrow buffer takes the stored values as they are (int32, double, int Date, factor codes shifted in place,
    // POSIXct seconds converted in place to microseconds); logical and double Dates change width: a scratch buffer
    if (c.out == O_BOOL || (c.out == O_DATE && phys == FSTX_FLOAT64)) {
      R.tmp.resize((size_t)n * w);
      R.vals = R.tmp.data();
    } else {
      R.vals = array_buf(R.out, 1, (size_t)n * w);
    }
    if (c.out == O_FACTOR) {
      ArrayHold* hh = (ArrayHold*)R.out->private_data;
      hh->dict = new_child();
      R.out->dictionary = hh->dict;
      make_utf8(hh->dict, c.lev_lens.size(), c.lev_lens.data(), c.lev_bytes.data());
    }
    return R.vals;
  } catch (const std::exception& x) {
    H.error = x.what();
    return nullptr;
  }
}

int rows_strings(void* ctx, uint32_t k, uint64_t dest, uint32_t cnt, const int32_t* lens, const char* bytes) {
  RowsHost& H = *(RowsHost*)ctx;
  RowsCol& R = (*H.cols)[k];
  try {
    uint64_t add = 0;
    for (uint32_t i = 0; i < cnt; ++i) {
      const int32_t l = lens[i];
      if (l < 0) R.v->null_at(dest + i);
      else add += (uint64_t)l;
      const uint64_t t = R.tot + add;
      if (R.big) {
        ((int64_t*)R.off)[dest + i + 1] = (int64_t)t;
      } else {
        if (t > (uint64_t)INT32_MAX)
          throw Fail("column '" + H.S->cols[R.j].name + "': a batch holds more than 2 GB of text; use a smaller batch_rows");
        ((int32_t*)R.off)[dest + i + 1] = (int32_t)t;
      }
    }
    if (add) R.bytes.insert(R.bytes.end(), bytes, bytes + add);
    R.tot += add;
    return 0;
  } catch (const std::exception& x) {
    H.error = x.what();
    return 1;
  }
}

int rows_no_dict(void* ctx, uint32_t, uint32_t, const int32_t*, const char*) {
  ((RowsHost*)ctx)->error = "internal error: a rows plan delivered a dictionary";
  return 1;
}
int rows_no_codes(void* ctx, uint32_t, uint64_t, uint32_t, const int32_t*) {
  ((RowsHost*)ctx)->error = "internal error: a rows plan delivered dictionary codes";
  return 1;
}
int rows_no_at(void* ctx, uint32_t, uint32_t, const uint64_t*, const int32_t*, const char*) {
  ((RowsHost*)ctx)->error = "internal error: rows out of order";
  return 1;
}

// Stored values -> Arrow values in place (or from the scratch buffer), with the validity bitmap.
void rows_finish_fixed(const ColSpec& c, RowsCol& R, uint64_t n) {
  Validity v{R.out, n};
  const int32_t na = std::numeric_limits<int32_t>::min();
  if (c.out == O_BOOL) {
    const int32_t* x = (const int32_t*)R.vals;
    uint8_t* o = (uint8_t*)array_buf(R.out, 1, (size_t)((n + 7) / 8));
    std::memset(o, 0, (size_t)((n + 7) / 8));
    for (uint64_t i = 0; i < n; ++i) {
      if (x[i] == na) v.null_at(i);
      else if (x[i]) o[i >> 3] |= (uint8_t)(1u << (i & 7));
    }
  } else if (c.phys == FSTX_FLOAT64) {
    double* x = (double*)R.vals;
    if (c.out == O_FLOAT64) {  // R's NA_real_: a NaN whose low word is 1954; other NaN stay NaN
      const uint64_t* u = (const uint64_t*)x;
      for (uint64_t i = 0; i < n; ++i)
        if ((u[i] & 0xFFFFFFFFull) == 1954 && (u[i] & 0x7FF0000000000000ull) == 0x7FF0000000000000ull) v.null_at(i);
    } else if (c.out == O_DATE) {  // whole days, rounded down (as R prints a fractional Date)
      int32_t* o = (int32_t*)array_buf(R.out, 1, (size_t)n * 4);
      for (uint64_t i = 0; i < n; ++i) {
        const double d = std::floor(x[i]);
        if (!std::isfinite(d)) {
          o[i] = 0;
          v.null_at(i);
        } else if (d < (double)INT32_MIN || d > (double)INT32_MAX) {
          throw Fail("column '" + c.name + "' has a Date outside the range of Arrow date32");
        } else {
          o[i] = (int32_t)d;
        }
      }
    } else {  // O_TS: seconds -> microseconds, rounded to the nearest, in place
      int64_t* o = (int64_t*)R.vals;
      for (uint64_t i = 0; i < n; ++i) {
        const double us = std::nearbyint(x[i] * 1e6);
        if (!std::isfinite(us)) {
          o[i] = 0;
          v.null_at(i);
        } else if (us < -9.2e18 || us > 9.2e18) {
          throw Fail("column '" + c.name + "' has a time outside the range of Arrow microsecond timestamps");
        } else {
          o[i] = (int64_t)us;
        }
      }
    }
  } else {  // int32, int Date, factor codes (1-based -> 0-based dictionary indexes)
    int32_t* x = (int32_t*)R.vals;
    const int32_t shift = c.out == O_FACTOR ? 1 : 0;
    if (shift) {
      for (uint64_t i = 0; i < n; ++i) {
        if (x[i] == na) {
          x[i] = 0;
          v.null_at(i);
        } else {
          x[i] -= shift;
        }
      }
    } else {
      for (uint64_t i = 0; i < n; ++i)
        if (x[i] == na) v.null_at(i);
    }
  }
  v.done();
}

void rows_finish_strings(RowsCol& R) {
  char* d = (char*)array_buf(R.out, 2, (size_t)R.tot);
  if (R.tot) std::memcpy(d, R.bytes.data(), (size_t)R.tot);
  std::vector<char>().swap(R.bytes);
  R.v->done();
}

// A column that leaves as dictionary indexes into its row group's dictionary: codes of the pages that hold rows.
void rows_dict_column(const Stream& S, const ColSpec& c, uint64_t a, uint64_t b, const uint64_t* sel, uint64_t n,
                      ArrowArray* out) {
  SrcPtr s = open_src(S, c, a, b);
  fstx_error e;
  fstx_colsrc_info si;
  fstx_colsrc_info_get(s.get(), &si);
  if (si.n_dicts != 1) throw Fail("internal error: a batch spans more than one dictionary");
  array_init(out, (int64_t)n, 2);
  Validity v{out, n};
  int32_t* o = (int32_t*)array_buf(out, 1, (size_t)n * 4);
  uint32_t p = 0;
  uint64_t first = 0;
  uint32_t k = 0;
  const int32_t* pl = nullptr;
  bool have = false;
  for (uint64_t i = 0; i < n; ++i) {
    const uint64_t r = sel[i];
    while (!have || r >= first + k) {
      if (have) {
        fstx_colsrc_drop(s.get(), p);
        ++p;
      }
      fstx_colsrc_page_span(s.get(), p, &first, &k);
      have = true;
      if (r < first + k) {
        const char* pb = nullptr;
        int32_t d = -1;
        check(fstx_colsrc_str_page(s.get(), p, &pl, &pb, &d, &e), e);
        if (d < 0) throw Fail("internal error: dictionary and plain pages mixed in column '" + c.name + "'");
      }
    }
    const int32_t code = pl[r - first];
    o[i] = code < 0 ? 0 : code;
    if (code < 0) v.null_at(i);
  }
  v.done();
  uint32_t dn = 0;
  const int32_t* dl = nullptr;
  const char* db = nullptr;
  check(fstx_colsrc_dict(s.get(), 0, &dn, &dl, &db, &e), e);
  ArrayHold* h = (ArrayHold*)out->private_data;
  h->dict = new_child();
  out->dictionary = h->dict;
  make_utf8(h->dict, dn, dl, db);
}

// One batch of rows: file rows a + sel[i] for i in [0, n) (sel ascending, repeats allowed, sel[n - 1] = b - a - 1),
// all inside one row group. h is the batch's struct array with one child per column.
void rows_batch(Stream& S, ArrayHold* h, uint64_t a, uint64_t b, const uint64_t* sel, uint64_t n) {
  const size_t nc = S.cols.size();
  std::vector<RowsCol> rc;
  std::vector<uint32_t> pcols;
  for (size_t j = 0; j < nc; ++j) {
    if (S.cols[j].out == O_DICT_STR) {
      rows_dict_column(S, S.cols[j], a, b, sel, n, h->kids[j]);
      continue;
    }
    RowsCol R;
    R.j = j;
    R.out = h->kids[j];
    rc.push_back(std::move(R));
    pcols.push_back(S.cols[j].col);
  }
  if (rc.empty()) return;
  std::vector<uint64_t> rows((size_t)n);
  for (uint64_t i = 0; i < n; ++i) rows[(size_t)i] = a + sel[i];
  fstx_plan* plan = nullptr;
  fstx_error e;
  check(fstx_plan_create_rows(S.f, pcols.data(), (uint32_t)pcols.size(), rows.data(), n, &plan, &e), e);
  std::unique_ptr<fstx_plan, void (*)(fstx_plan*)> pg(plan, fstx_plan_free);
  RowsHost H;
  H.S = &S;
  H.n = n;
  H.cols = &rc;
  fstx_read_host host;
  std::memset(&host, 0, sizeof(host));
  host.ctx = &H;
  host.alloc_column = rows_alloc;
  host.on_strings = rows_strings;
  host.on_dict = rows_no_dict;
  host.on_codes = rows_no_codes;
  host.on_strings_at = rows_no_at;
  if (fstx_read(plan, &host, S.threads, S.verify, &e) != FSTX_OK) {
    if (!H.error.empty()) throw Fail(H.error);
    throw Fail(e.message);
  }
  for (RowsCol& R : rc) {
    const ColSpec& c = S.cols[R.j];
    if (c.phys == FSTX_STRING) rows_finish_strings(R);
    else rows_finish_fixed(c, R, n);
  }
}
