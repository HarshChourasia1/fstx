// fstx fuzz target "arrow" (written by WS2, taken over by WS6 on 15 Nov 2026). build.sh builds it only when
// src/r_arrow.cpp has the no-R build (FSTX_ARROW_NO_R, WS2's branch).
// The Arrow stream's C entry, fstx_arrow_stream_open() in src/r_arrow.cpp, built here without R: r_arrow.cpp is
// included with FSTX_ARROW_NO_R, so build.sh needs no change (its engine list leaves out the r_* files).
//
// Two passes over the fuzzed file, each pulling and releasing every batch and then the stream:
//   1. all columns (at most kMaxCols), every row, one batch per row group (or batch_rows from control byte 2);
//   2. rows mode: ascending rows with repeats, small batch_rows (1-8).
// Oracles: no crash / sanitizer report / hang; the schema has one child per column; every batch and child has
// the batch's length, buffers that are there, offsets that rise and stay inside the data buffer, dictionary
// indexes inside the dictionary; values equal fuzz_read's whole read (fz::read_range) whenever both succeed;
// after everything is released, no stream, schema or array holder is left (g_live back to 0).
#define FSTX_ARROW_NO_R 1
#include "r_arrow.cpp"

#include "fuzz_common.h"

namespace {

bool vbit(const ArrowArray* a, int64_t i) {  // validity bit (true = value present)
  const uint8_t* v = (const uint8_t*)a->buffers[0];
  if (!v) return true;
  const int64_t k = a->offset + i;
  return (v[k >> 3] >> (k & 7) & 1) != 0;
}

int64_t count_nulls(const ArrowArray* a) {
  int64_t n = 0;
  for (int64_t i = 0; i < a->length; ++i) n += !vbit(a, i);
  return n;
}

// string i of a utf8 ("u") or large_utf8 ("U") array (binary "z" / "Z" have the same layout)
std::string str_at(const ArrowArray* a, bool large, int64_t i) {
  const char* d = (const char*)a->buffers[2];
  int64_t s, e;
  if (large) {
    s = ((const int64_t*)a->buffers[1])[a->offset + i];
    e = ((const int64_t*)a->buffers[1])[a->offset + i + 1];
  } else {
    s = ((const int32_t*)a->buffers[1])[a->offset + i];
    e = ((const int32_t*)a->buffers[1])[a->offset + i + 1];
  }
  return std::string(d + s, (size_t)(e - s));
}

void check_strings(const ArrowArray* a, bool large, const char* what) {
  if (a->n_buffers != 3 || !a->buffers[1]) fz::fail(what, "(buffers)");
  int64_t prev = -1;
  for (int64_t i = 0; i <= a->length; ++i) {
    const int64_t o = large ? ((const int64_t*)a->buffers[1])[a->offset + i]
                            : ((const int32_t*)a->buffers[1])[a->offset + i];
    if (o < 0 || o < prev) fz::fail(what, "(offsets fall)");
    prev = o;
  }
  if (prev > 0 && !a->buffers[2]) fz::fail(what, "(no data buffer)");
  // ASan catches a data buffer shorter than the last offset when the bytes are read here
  for (int64_t i = 0; i < a->length; ++i) (void)str_at(a, large, i);
}

// Checks one column of one batch against the whole read t (column j, file rows rows[0..len)), when t is there.
void check_child(const ArrowSchema* sc, const ArrowArray* a, int64_t len, const fz::Table* t, size_t j,
                 const uint64_t* rows) {
  if (!a || !a->release) fz::fail("child array missing or released");
  if (a->length != len) fz::fail("child length differs from the batch length");
  if (a->null_count != -1 && a->null_count != count_nulls(a)) fz::fail("null_count differs from the validity bitmap");
  const std::string f = sc->format;
  const fz::Col* col = t ? &t->cols[j] : nullptr;
  const int32_t NA = std::numeric_limits<int32_t>::min();
  if (sc->dictionary) {  // factor (levels) or dictionary-coded strings (utf8 values) or binary (binary values)
    const std::string df = sc->dictionary->format;
    if (f != "i" || !a->dictionary || (df != "u" && df != "z")) fz::fail("dictionary shape");
    if (col && (df == "z") != (col->phys == FSTX_BINARY)) fz::fail("dictionary value type differs from the column");
    const ArrowArray* d = a->dictionary;
    check_strings(d, false, "dictionary values");
    const int32_t* x = (const int32_t*)a->buffers[1];
    if (len && !x) fz::fail("dictionary indexes missing");
    for (int64_t i = 0; i < len; ++i) {
      if (!vbit(a, i)) continue;
      const int32_t k = x[a->offset + i];
      if (k < 0 || k >= d->length) fz::fail("dictionary index outside the dictionary");
      if (!col) continue;
      const uint64_t r = rows[i];
      if (fz::is_strlike(col->phys)) {
        if (col->na[r] || col->s[r] != str_at(d, false, k)) fz::fail("dictionary string differs from the whole read");
      } else if (col->i[r] != k + 1) {
        fz::fail("factor code differs from the whole read");
      }
    }
    if (col)
      for (int64_t i = 0; i < len; ++i)
        if (!vbit(a, i) && (fz::is_strlike(col->phys) ? col->na[rows[i]] != 1 : col->i[rows[i]] != NA))
          fz::fail("null where the whole read has a value");
    return;
  }
  if (f == "u" || f == "U" || f == "z" || f == "Z") {
    const bool large = f == "U" || f == "Z";
    if (col && (f == "z" || f == "Z") != (col->phys == FSTX_BINARY)) fz::fail("binary / utf8 type differs from the column");
    check_strings(a, large, "string or binary column");
    if (col)
      for (int64_t i = 0; i < len; ++i) {
        const uint64_t r = rows[i];
        if (vbit(a, i) == (col->na[r] == 1)) fz::fail("string null differs from the whole read");
        if (vbit(a, i) && col->s[r] != str_at(a, large, i)) fz::fail("string differs from the whole read");
      }
    return;
  }
  if (a->n_buffers != 2 || (len && !a->buffers[1])) fz::fail("fixed-width buffers");
  if (!col) {  // touch every value so ASan sees a short buffer
    const size_t w = f == "b" ? 0 : (f == "g" || f == "l" || f.compare(0, 3, "tsu") == 0) ? 8 : 4;
    volatile uint8_t sink = 0;
    const size_t bytes = w ? (size_t)(a->offset + len) * w : (size_t)((a->offset + len + 7) / 8);
    for (size_t b = 0; b < bytes; ++b) sink = sink ^ ((const uint8_t*)a->buffers[1])[b];
    for (int64_t i = 0; i < len; ++i) sink = sink ^ (uint8_t)vbit(a, i);
    return;
  }
  for (int64_t i = 0; i < len; ++i) {
    const uint64_t r = rows[i];
    const int64_t k = a->offset + i;
    const bool present = vbit(a, i);
    // Spec 6.2: on a page whose null_count is 0 the stream does not look for the missing-value pattern and passes
    // it on as a value, while the whole read gives NA. Only a crafted file gets there; accepted for 1.0 (16 Nov),
    // so a present value that is exactly the pattern where the whole read has NA is not a wrong result.
    if (f == "i") {
      const int32_t x = ((const int32_t*)a->buffers[1])[k];
      if (present && x == NA && col->i[r] == NA) continue;
      if (present != (col->i[r] != NA) || (present && x != col->i[r]))
        fz::fail("int32 differs from the whole read");
    } else if (f == "l") {  // int64 (fstx 1.2): INT64_MIN is NA
      const int64_t x = ((const int64_t*)a->buffers[1])[k];
      if (col->phys != FSTX_INT64) fz::fail("int64 child for a column that is not int64");
      if (present && x == INT64_MIN && col->l[r] == INT64_MIN) continue;  // see above
      if (present != (col->l[r] != INT64_MIN) || (present && x != col->l[r]))
        fz::fail("int64 differs from the whole read");
    } else if (f == "b") {
      const bool bit = (((const uint8_t*)a->buffers[1])[k >> 3] >> (k & 7) & 1) != 0;
      if (present != (col->i[r] != NA) || (present && bit != (col->i[r] != 0)))
        fz::fail("bool differs from the whole read");
    } else if (f == "g") {
      uint64_t u;
      std::memcpy(&u, &col->d[r], 8);
      const bool na = std::isnan(col->d[r]) && (u & 0xFFFFFFFFull) == 1954;
      if (present && na && fz::same_dbl(((const double*)a->buffers[1])[k], col->d[r])) continue;  // see above
      if (present == na || (present && !fz::same_dbl(((const double*)a->buffers[1])[k], col->d[r])))
        fz::fail("float64 differs from the whole read");
    } else if (f == "tdD") {
      const int32_t v = ((const int32_t*)a->buffers[1])[k];
      if (col->phys == FSTX_INT32) {
        if (present && v == NA && col->i[r] == NA) continue;  // see above
        if (present != (col->i[r] != NA) || (present && v != col->i[r])) fz::fail("date32 differs (int Date)");
      } else {
        const double d = std::floor(col->d[r]);
        if (present != std::isfinite(d) || (present && (double)v != d)) fz::fail("date32 differs (double Date)");
      }
    } else if (f.compare(0, 4, "tsu:") == 0) {
      const int64_t v = ((const int64_t*)a->buffers[1])[k];
      const double us = std::nearbyint(col->d[r] * 1e6);
      if (present != std::isfinite(us) || (present && (double)v != us)) fz::fail("timestamp differs");
    } else {
      fz::fail("unexpected child format", f.c_str());
    }
  }
}

// One stream over the file: schema, then every batch, then release. rows: the file rows the stream reads, in
// order (used to line batches up with the whole read t).
void run_stream(const std::string& path, const std::vector<uint32_t>& cols, const fstx_arrow_args& A,
                const fz::Table* t, const std::vector<uint64_t>& rows) {
  std::vector<std::string> nm(cols.size());
  std::vector<const char*> np(cols.size());
  for (size_t j = 0; j < cols.size(); ++j) {
    nm[j] = "c" + std::to_string(j);
    np[j] = nm[j].c_str();
  }
  fstx_arrow_args a = A;
  a.cols = cols.data();
  a.names = np.data();
  a.n_cols = (uint32_t)cols.size();
  ArrowArrayStream st;
  std::memset(&st, 0, sizeof st);
  fstx_error err;
  if (fstx_arrow_stream_open(path.c_str(), &a, &st, &err) != FSTX_OK) {
    if (st.release) fz::fail("open failed but filled the stream");
    return;
  }
  ArrowSchema sc;
  std::memset(&sc, 0, sizeof sc);
  if (st.get_schema(&st, &sc) == 0) {
    if (!sc.release || std::string(sc.format) != "+s" || sc.n_children != (int64_t)cols.size())
      fz::fail("schema shape");
    uint64_t seen = 0;
    for (int guard = 0; guard < 1 << 20; ++guard) {
      ArrowArray b;
      std::memset(&b, 0, sizeof b);
      if (st.get_next(&st, &b) != 0) {
        if (!st.get_last_error(&st)) fz::fail("get_next failed without an error message");
        if (b.release) fz::fail("get_next failed but filled the array");
        break;
      }
      if (!b.release) break;  // end of stream
      if (b.length <= 0) fz::fail("empty batch before the end");
      if (b.n_children != sc.n_children) fz::fail("batch has the wrong number of columns");
      if (seen + (uint64_t)b.length > rows.size()) fz::fail("stream gives more rows than asked for");
      for (int64_t j = 0; j < b.n_children; ++j)
        check_child(sc.children[j], b.children[j], b.length, t, (size_t)j, rows.data() + seen);
      if (a.batch_rows && (uint64_t)b.length > a.batch_rows) fz::fail("batch longer than batch_rows");
      seen += (uint64_t)b.length;
      b.release(&b);
      if (b.release) fz::fail("array release left its release callback");
    }
    sc.release(&sc);
    if (sc.release) fz::fail("schema release left its release callback");
  } else if (!st.get_last_error(&st)) {
    fz::fail("get_schema failed without an error message");
  }
  st.release(&st);
  if (st.release) fz::fail("stream release left its release callback");
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  if (size < fz::kCtl) return 0;
  fz::Ctl c(data);
  std::vector<uint8_t> b = fz::file_bytes(data, size, c);
  static const std::string path = fz::tmp_path("arrow");
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
  fz::Table whole;
  const bool have = fz::read_range(f, 0, fi.n_rows, whole, 1, 0) == FSTX_OK;
  fstx_close(f);
  std::vector<uint32_t> cols;
  for (uint32_t j = 0; j < fi.n_cols && j < fz::kMaxCols; ++j) cols.push_back(j);
  if (c.bit(2, 7) && cols.size() > 1) std::swap(cols.front(), cols.back());  // order is free
  fstx_arrow_args A;
  std::memset(&A, 0, sizeof A);
  A.threads = c.bit(2, 6) ? 2 : 1;
  A.verify = c.bit(2, 5) ? 1 : 0;
  // the whole read t is by file column; line it up with cols
  fz::Table t;
  if (have) {
    t.n = whole.n;
    for (uint32_t j : cols) t.cols.push_back(whole.cols[j]);
  }
  // pass 1: every row
  std::vector<uint64_t> all(fi.n_rows);
  for (uint64_t r = 0; r < fi.n_rows; ++r) all[r] = r;
  A.from = 0;
  A.to = fi.n_rows;
  A.batch_rows = c.b[2] & 0x0F;  // 0 = one batch per row group
  run_stream(path, cols, A, have ? &t : nullptr, all);
  // pass 2: rows mode, ascending with repeats, small batches
  std::vector<uint64_t> rows;
  const uint32_t nr = (uint32_t)c.below(300);
  for (uint32_t i = 0; i < nr && fi.n_rows; ++i) rows.push_back(c.below(fi.n_rows));
  std::sort(rows.begin(), rows.end());
  if (c.bit(2, 4) && !rows.empty()) rows.push_back(rows.back());
  static const uint64_t no_row = 0;
  A.rows = rows.empty() ? &no_row : rows.data();  // a non-NULL pointer is what asks for rows mode
  A.n_rows = rows.size();
  A.batch_rows = 1 + c.below(8);
  run_stream(path, cols, A, have ? &t : nullptr, rows);
  if (g_live.load() != 0) fz::fail("a stream, schema or array holder was left after release");
  return 0;
}
