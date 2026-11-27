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
// order. Columns that leave as dictionaries (every page dictionary-coded) get their codes and the row group's dictionary
// from the same plan (fstx_plan_rows_codes).

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
  int32_t* codes = nullptr;  // dictionary columns: the indexes
  bool has_dict = false;
};

struct RowsHost {
  const Stream* S = nullptr;
  uint64_t n = 0;
  std::vector<RowsCol>* cols = nullptr;  // in plan order
  std::string error;
};

// Sets up child array R.out for n string rows (offsets, validity on the first NA); bytes come later.
void rows_str_init(const ColSpec& c, RowsCol& R, uint64_t n) {
  array_init(R.out, (int64_t)n, 3);
  R.big = c.out == O_LARGE_UTF8;
  R.off = array_buf(R.out, 1, (size_t)(n + 1) * (R.big ? 8 : 4));
  if (R.big) ((int64_t*)R.off)[0] = 0;
  else ((int32_t*)R.off)[0] = 0;
  R.v.reset(new Validity{R.out, n});
}

// Sets up child array R.out for n fixed-width rows; returns where the n stored values (width of phys) go.
void* rows_fixed_init(const ColSpec& c, RowsCol& R, uint64_t n) {
  array_init(R.out, (int64_t)n, 2);
  const size_t w = c.phys == FSTX_FLOAT64 ? 8 : 4;
  // the Arrow buffer takes the stored values as they are (int32, double, int Date, factor codes shifted in place,
  // POSIXct seconds converted in place to microseconds); logical and double Dates change width: a scratch buffer
  if (c.out == O_BOOL || (c.out == O_DATE && c.phys == FSTX_FLOAT64)) {
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
}

void* rows_alloc(void* ctx, uint32_t k, uint32_t, int phys, uint64_t n) {
  RowsHost& H = *(RowsHost*)ctx;
  RowsCol& R = (*H.cols)[k];
  const ColSpec& c = H.S->cols[R.j];
  try {
    if (c.out == O_DICT_STR) {
      array_init(R.out, (int64_t)n, 2);
      R.codes = (int32_t*)array_buf(R.out, 1, (size_t)n * 4);
      R.v.reset(new Validity{R.out, n});
      return R.codes;
    }
    if (phys == FSTX_STRING) {
      rows_str_init(c, R, n);
      return R.off;
    }
    return rows_fixed_init(c, R, n);
  } catch (const std::exception& x) {
    H.error = x.what();
    return nullptr;
  }
}

int rows_strings(void* ctx, uint32_t k, uint64_t dest, uint32_t cnt, const int32_t* lens, const char* bytes) {
  RowsHost& H = *(RowsHost*)ctx;
  RowsCol& R = (*H.cols)[k];
  try {
    if (R.codes) throw Fail("internal error: dictionary and plain pages mixed in column '" + H.S->cols[R.j].name + "'");
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

// A dictionary child for array out (n entries laid out like on_strings).
void set_dict(ArrowArray* out, uint32_t n, const int32_t* lens, const char* bytes) {
  ArrayHold* hh = (ArrayHold*)out->private_data;
  hh->dict = new_child();
  out->dictionary = hh->dict;
  make_utf8(hh->dict, n, lens, bytes);
}

int rows_dict(void* ctx, uint32_t k, uint32_t n, const int32_t* lens, const char* bytes) {
  RowsHost& H = *(RowsHost*)ctx;
  RowsCol& R = (*H.cols)[k];
  try {
    if (R.has_dict) throw Fail("internal error: a batch spans more than one dictionary");
    set_dict(R.out, n, lens, bytes);
    R.has_dict = true;
    return 0;
  } catch (const std::exception& x) {
    H.error = x.what();
    return 1;
  }
}

int rows_codes(void* ctx, uint32_t k, uint64_t dest, uint32_t cnt, const int32_t* codes) {
  RowsHost& H = *(RowsHost*)ctx;
  RowsCol& R = (*H.cols)[k];
  if (!R.codes) {
    H.error = "internal error: codes for a column that is not a dictionary";
    return 1;
  }
  for (uint32_t i = 0; i < cnt; ++i) {
    const int32_t x = codes[i];
    R.codes[dest + i] = x < 0 ? 0 : x;
    if (x < 0) R.v->null_at(dest + i);
  }
  return 0;
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

// A selection that spans several batches but is not big (kRowsAllMax rows at most) is decoded by one rows plan for
// all of its rows when the first batch is asked for, and the batches are then cut from it: one parallel decode
// instead of one per row group (each with its own start, tail and wait).
constexpr uint64_t kRowsAllMax = 1u << 20;

struct StageDict {
  uint64_t start = UINT64_MAX;  // first staged row that uses it
  std::vector<int32_t> lens;
  std::string bytes;
};
struct StageCol {
  size_t j = 0;
  bool str = false, dict = false;
  std::vector<uint8_t> vals;  // fixed width: stored values; dictionary columns: int32 codes
  std::vector<uint64_t> off;  // strings: n + 1 offsets into bytes
  std::vector<uint8_t> na;
  std::vector<char> bytes;
  std::vector<StageDict> dicts;  // dictionary columns, in row order
};
struct RowsStage {
  const Stream* S = nullptr;
  std::vector<StageCol> cols;  // one per stream column
  std::string error;
};

void* stage_alloc(void* ctx, uint32_t k, uint32_t, int phys, uint64_t n) {
  RowsStage& G = *(RowsStage*)ctx;
  StageCol& C = G.cols[k];
  try {
    if (G.S->cols[C.j].out == O_DICT_STR) {
      C.dict = true;
      C.vals.resize((size_t)n * 4);
      return C.vals.data();
    }
    if (phys == FSTX_STRING) {
      C.str = true;
      C.off.assign((size_t)n + 1, 0);
      C.na.assign((size_t)n, 0);
      return C.off.data();
    }
    C.vals.resize((size_t)n * (phys == FSTX_FLOAT64 ? 8 : 4));
    return C.vals.data();
  } catch (const std::exception& x) {
    G.error = x.what();
    return nullptr;
  }
}

int stage_strings(void* ctx, uint32_t k, uint64_t dest, uint32_t cnt, const int32_t* lens, const char* bytes) {
  RowsStage& G = *(RowsStage*)ctx;
  StageCol& C = G.cols[k];
  try {
    if (C.dict) throw Fail("internal error: dictionary and plain pages mixed in column '" + G.S->cols[C.j].name + "'");
    uint64_t add = 0;
    for (uint32_t i = 0; i < cnt; ++i) {
      const int32_t l = lens[i];
      C.na[dest + i] = l < 0;
      if (l > 0) add += (uint64_t)l;
      C.off[dest + i + 1] = C.off[dest] + add;
    }
    if (add) C.bytes.insert(C.bytes.end(), bytes, bytes + add);
    return 0;
  } catch (const std::exception& x) {
    G.error = x.what();
    return 1;
  }
}

int stage_dict(void* ctx, uint32_t k, uint32_t n, const int32_t* lens, const char* bytes) {
  RowsStage& G = *(RowsStage*)ctx;
  StageCol& C = G.cols[k];
  try {
    StageDict D;
    D.lens.assign(lens, lens + n);
    uint64_t tot = 0;
    for (uint32_t i = 0; i < n; ++i) tot += (uint64_t)std::max(0, lens[i]);
    D.bytes.assign(bytes, (size_t)tot);
    C.dicts.push_back(std::move(D));
    return 0;
  } catch (const std::exception& x) {
    G.error = x.what();
    return 1;
  }
}

int stage_codes(void* ctx, uint32_t k, uint64_t dest, uint32_t cnt, const int32_t* codes) {
  RowsStage& G = *(RowsStage*)ctx;
  StageCol& C = G.cols[k];
  if (!C.dict || C.dicts.empty()) {
    G.error = "internal error: codes without a dictionary";
    return 1;
  }
  if (C.dicts.back().start == UINT64_MAX) C.dicts.back().start = dest;
  std::memcpy(C.vals.data() + (size_t)dest * 4, codes, (size_t)cnt * 4);
  return 0;
}

// Runs one rows plan for all stream columns over file rows `rows` with `host`; dictionary columns as codes.
void rows_read(const Stream& S, const uint64_t* rows, uint64_t n, fstx_read_host& host, const std::string& host_error) {
  std::vector<uint32_t> pcols;
  for (const ColSpec& c : S.cols) pcols.push_back(c.col);
  fstx_plan* plan = nullptr;
  fstx_error e;
  check(fstx_plan_create_rows(S.f, pcols.data(), (uint32_t)pcols.size(), rows, n, &plan, &e), e);
  std::unique_ptr<fstx_plan, void (*)(fstx_plan*)> pg(plan, fstx_plan_free);
  for (size_t j = 0; j < S.cols.size(); ++j)
    if (S.cols[j].out == O_DICT_STR) check(fstx_plan_rows_codes(plan, (uint32_t)j, &e), e);
  host.on_strings_at = rows_no_at;
  if (fstx_read(plan, &host, S.threads, S.verify, &e) != FSTX_OK) {
    if (!host_error.empty()) throw Fail(host_error);
    throw Fail(e.message);
  }
}

RowsStage* rows_stage(Stream& S) {
  std::unique_ptr<RowsStage> G(new RowsStage());
  G->S = &S;
  G->cols.resize(S.cols.size());
  for (size_t j = 0; j < S.cols.size(); ++j) G->cols[j].j = j;
  fstx_read_host host;
  std::memset(&host, 0, sizeof(host));
  host.ctx = G.get();
  host.alloc_column = stage_alloc;
  host.on_strings = stage_strings;
  host.on_dict = stage_dict;
  host.on_codes = stage_codes;
  rows_read(S, S.rows.data(), S.rows.size(), host, G->error);
  RowsStage* g = G.release();
  S.rows_all = std::shared_ptr<void>(g, [](void* p) { delete (RowsStage*)p; });
  return g;
}

// The batch's rows [i0, i0 + n) of the staged selection, into child array R.out.
void rows_from_stage(const ColSpec& c, const StageCol& C, uint64_t i0, uint64_t n, RowsCol& R) {
  if (C.dict) {
    const StageDict* D = nullptr;  // the batch's row group's dictionary: the last one that starts at or before i0
    for (const StageDict& d : C.dicts)
      if (d.start != UINT64_MAX && d.start <= i0) D = &d;
    if (!D) throw Fail("internal error: no dictionary for a batch of column '" + c.name + "'");
    array_init(R.out, (int64_t)n, 2);
    Validity v{R.out, n};
    int32_t* o = (int32_t*)array_buf(R.out, 1, (size_t)n * 4);
    const int32_t* x = (const int32_t*)C.vals.data() + i0;
    for (uint64_t i = 0; i < n; ++i) {
      o[i] = x[i] < 0 ? 0 : x[i];
      if (x[i] < 0) v.null_at(i);
    }
    v.done();
    set_dict(R.out, (uint32_t)D->lens.size(), D->lens.data(), D->bytes.data());
    return;
  }
  if (!C.str) {
    const size_t w = c.phys == FSTX_FLOAT64 ? 8 : 4;
    void* d = rows_fixed_init(c, R, n);
    if (n) std::memcpy(d, C.vals.data() + (size_t)i0 * w, (size_t)n * w);
    rows_finish_fixed(c, R, n);
    return;
  }
  rows_str_init(c, R, n);
  const uint64_t base = C.off[(size_t)i0];
  for (uint64_t i = 0; i < n; ++i) {
    if (C.na[(size_t)(i0 + i)]) R.v->null_at(i);
    const uint64_t t = C.off[(size_t)(i0 + i + 1)] - base;
    if (R.big) {
      ((int64_t*)R.off)[i + 1] = (int64_t)t;
    } else {
      if (t > (uint64_t)INT32_MAX)
        throw Fail("column '" + c.name + "': a batch holds more than 2 GB of text; use a smaller batch_rows");
      ((int32_t*)R.off)[i + 1] = (int32_t)t;
    }
  }
  R.tot = C.off[(size_t)(i0 + n)] - base;
  char* d = (char*)array_buf(R.out, 2, (size_t)R.tot);
  if (R.tot) std::memcpy(d, C.bytes.data() + base, (size_t)R.tot);
  R.v->done();
}

// One batch of rows: file rows a + sel[i] for i in [0, n) (sel ascending, repeats allowed), all inside one row
// group; they are S.rows[S.pos, S.pos + n). h is the batch's struct array with one child per column.
void rows_batch(Stream& S, ArrayHold* h, uint64_t a, const uint64_t* sel, uint64_t n) {
  const size_t nc = S.cols.size();
  if (!nc) return;
  const uint64_t i0 = S.pos;
  RowsStage* G = (RowsStage*)S.rows_all.get();
  if (!G && i0 == 0 && n < S.rows.size() && S.rows.size() <= kRowsAllMax) G = rows_stage(S);
  if (G) {
    for (size_t j = 0; j < nc; ++j) {
      RowsCol R;
      R.j = j;
      R.out = h->kids[j];
      rows_from_stage(S.cols[j], G->cols[j], i0, n, R);
    }
    if (i0 + n >= S.rows.size()) S.rows_all.reset();  // the last batch: the staged rows go
    return;
  }
  std::vector<RowsCol> rc(nc);
  for (size_t j = 0; j < nc; ++j) {
    rc[j].j = j;
    rc[j].out = h->kids[j];
  }
  std::vector<uint64_t> rows((size_t)n);
  for (uint64_t i = 0; i < n; ++i) rows[(size_t)i] = a + sel[i];
  RowsHost H;
  H.S = &S;
  H.n = n;
  H.cols = &rc;
  fstx_read_host host;
  std::memset(&host, 0, sizeof(host));
  host.ctx = &H;
  host.alloc_column = rows_alloc;
  host.on_strings = rows_strings;
  host.on_dict = rows_dict;
  host.on_codes = rows_codes;
  rows_read(S, rows.data(), n, host, H.error);
  for (RowsCol& R : rc) {
    const ColSpec& c = S.cols[R.j];
    if (c.out == O_DICT_STR) {
      if (!R.has_dict) throw Fail("internal error: no dictionary for column '" + c.name + "'");
      R.v->done();
    } else if (c.phys == FSTX_STRING) {
      rows_finish_strings(R);
    } else {
      rows_finish_fixed(c, R, n);
    }
  }
}
