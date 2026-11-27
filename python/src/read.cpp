// fstx Python binding: reading. The Arrow C stream of the engine (arrow_abi.h) handed to Python as a
// PyCapsule, and filters (fstx_filter). MIT licence.
// Column data never passes through Python objects here: pyarrow imports the stream and pulls the batches
// (it releases the GIL around each pull); the stream's callbacks are plain C++ and touch no Python object.
#include "binding.h"

#include <nanobind/stl/string.h>
#include <nanobind/stl/vector.h>

#include <cstring>
#include <vector>

#include "arrow_abi.h"

namespace fstxpy {

// The capsule owns the ArrowArrayStream struct, and after it the UTF-8 flags the stream sets per batch (one byte a
// column, FSTX_ARROW_CHECK_UTF8): keep the capsule while the stream is pulled. A consumer (pyarrow) moves the
// stream out and marks the struct released; a capsule nobody consumed releases the stream (and with it the open
// file) here.
struct StreamBox {
  ArrowArrayStream st;  // first: the capsule's pointer is &st
  uint8_t* bad;
  size_t n;
};

static void stream_capsule_free(void* p) noexcept {
  StreamBox* b = (StreamBox*)p;
  if (b->st.release) b->st.release(&b->st);
  delete[] b->bad;
  delete b;
}

// rows: None for the range [from, to), or bytes holding uint64 row numbers (0-based, ascending, repeats
// allowed). raw: Date / POSIXct columns as stored. The stream opens the file itself and keeps it open until
// it is released, so it does not depend on a File object.
static nb::capsule stream_open(const std::string& path, const std::vector<uint32_t>& cols,
                               const std::vector<std::string>& names, uint64_t from, uint64_t to,
                               nb::object rows, int threads, uint64_t batch_rows, int verify, bool raw, int flags) {
  if (cols.size() != names.size()) throw nb::value_error("cols and names differ in length");
  std::vector<const char*> np;
  for (const std::string& s : names) np.push_back(s.c_str());
  std::vector<uint64_t> rv;
  const bool by_rows = !rows.is_none();
  if (by_rows) {
    nb::bytes b = nb::cast<nb::bytes>(rows);
    if (b.size() % 8) throw nb::value_error("rows must hold 8-byte row numbers");
    rv.resize(b.size() / 8);
    if (!rv.empty()) std::memcpy(rv.data(), b.c_str(), b.size());
  }
  static const uint64_t no_row = 0;
  fstx_arrow_args A;
  std::memset(&A, 0, sizeof(A));
  A.cols = cols.data();
  A.names = np.data();
  A.n_cols = (uint32_t)cols.size();
  A.from = from;
  A.to = to;
  A.rows = by_rows ? (rv.empty() ? &no_row : rv.data()) : nullptr;  // an empty selection: no batches
  A.n_rows = rv.size();
  A.threads = threads;
  A.batch_rows = batch_rows;
  A.verify = verify;
  A.raw = raw ? 1 : 0;
  A.flags = flags;
  StreamBox* box = new StreamBox();
  std::memset(&box->st, 0, sizeof(box->st));
  box->n = cols.size();
  box->bad = new uint8_t[box->n + 1]();
  A.utf8_bad = box->bad;
  fstx_error err;
  std::memset(&err, 0, sizeof(err));
  int rc;
  {
    nb::gil_scoped_release nogil;  // opens and maps the file, checks header, footer and page directory
    rc = fstx_arrow_stream_open(path.c_str(), &A, &box->st, &err);
  }
  if (rc != FSTX_OK) {
    delete[] box->bad;
    delete box;
    raise_error(err);
  }
  return nb::capsule(&box->st, "arrow_array_stream", stream_capsule_free);
}

// The columns (positions in the stream) whose text in the batch pulled last is not valid UTF-8.
static nb::list stream_utf8_bad(nb::capsule c) {
  const StreamBox* b = (const StreamBox*)c.data();
  if (!b) throw nb::value_error("not an fstx stream capsule");
  nb::list out;
  for (size_t j = 0; j < b->n; ++j)
    if (b->bad[j]) out.append(j);
  return out;
}

// preds: one dict per condition (all must hold): col (index), op (FSTX_OP_*), value (float), set (list of
// float), set_na, set_nan, strs (list of bytes, UTF-8). Returns (rows, stats): rows = bytes of uint64
// matching rows of [from, to), ascending and 0-based (empty when plan_only), stats = the engine's counts.
static nb::tuple file_filter(const File& F, nb::list preds, uint64_t from, uint64_t to, int threads, int verify,
                             bool plan_only) {
  fstx_file* f = F.get();
  const size_t np = nb::len(preds);
  std::vector<fstx_pred> ps(np);
  std::vector<std::vector<double>> sets(np);
  std::vector<std::vector<std::string>> strs(np);
  std::vector<std::vector<const char*>> sp(np);
  std::vector<std::vector<int32_t>> sl(np);
  for (size_t i = 0; i < np; ++i) {
    nb::dict q = nb::cast<nb::dict>(preds[i]);
    fstx_pred& d = ps[i];
    std::memset(&d, 0, sizeof(d));
    d.col = nb::cast<uint32_t>(q["col"]);
    d.op = nb::cast<int>(q["op"]);
    if (q.contains("value")) d.value = nb::cast<double>(q["value"]);
    if (q.contains("set")) {
      sets[i] = nb::cast<std::vector<double>>(q["set"]);
      d.set = sets[i].data();
      d.n_set = sets[i].size();
    }
    d.set_na = q.contains("set_na") && nb::cast<bool>(q["set_na"]);
    d.set_nan = q.contains("set_nan") && nb::cast<bool>(q["set_nan"]);
    if (q.contains("strs")) {
      for (nb::handle h : nb::cast<nb::list>(q["strs"])) {
        nb::bytes b = nb::cast<nb::bytes>(h);
        strs[i].emplace_back(b.c_str(), b.size());
      }
      for (const std::string& x : strs[i]) {
        sp[i].push_back(x.data());
        sl[i].push_back((int32_t)x.size());
      }
      d.strs = sp[i].data();
      d.str_lens = sl[i].data();
      d.n_strs = strs[i].size();
    }
  }
  fstx_selection* sel = nullptr;
  fstx_filter_stats st;
  std::memset(&st, 0, sizeof(st));
  fstx_error err;
  std::memset(&err, 0, sizeof(err));
  int rc;
  {
    nb::gil_scoped_release nogil;
    rc = fstx_filter(f, ps.data(), (uint32_t)np, from, to, threads, verify, plan_only ? 1 : 0, &sel, &st, &err);
  }
  check(rc, err);
  uint64_t n = 0;
  const uint64_t* r = sel ? fstx_selection_rows(sel, &n) : nullptr;
  nb::object rows;
  try {
    rows = nb::bytes((const char*)r, (size_t)n * 8);
  } catch (...) {
    fstx_selection_free(sel);
    throw;
  }
  fstx_selection_free(sel);
  nb::dict s;
  s["pages_total"] = st.pages_total;
  s["pages_skipped"] = st.pages_skipped;
  s["pages_pruned"] = st.pages_pruned;
  s["pages_all"] = st.pages_all;
  s["pages_read"] = st.pages_read;
  s["rows_window"] = st.rows_window;
  s["rows_candidate"] = st.rows_candidate;
  s["rows_matched"] = st.rows_matched;
  return nb::make_tuple(rows, s);
}

void init_read(nb::module_& m) {
  m.def("stream", &stream_open, nb::arg("path"), nb::arg("cols"), nb::arg("names"), nb::arg("start"),
        nb::arg("stop"), nb::arg("rows").none(), nb::arg("threads"), nb::arg("batch_rows"), nb::arg("verify"),
        nb::arg("raw"), nb::arg("flags") = 0, "An Arrow C stream over the file, as an 'arrow_array_stream' PyCapsule.");
  m.def("stream_utf8_bad", &stream_utf8_bad, nb::arg("capsule"),
        "Columns of the last batch of a stream opened with ARROW_CHECK_UTF8 whose text is not valid UTF-8.");
  m.attr("ARROW_PLAIN_STRINGS") = (int)FSTX_ARROW_PLAIN_STRINGS;
  m.attr("ARROW_CHECK_UTF8") = (int)FSTX_ARROW_CHECK_UTF8;
  m.def("filter", &file_filter, nb::arg("file"), nb::arg("preds"), nb::arg("start"), nb::arg("stop"),
        nb::arg("threads"), nb::arg("verify"), nb::arg("plan_only"),
        "Rows of [start, stop) that match every condition: (uint64 bytes, stats).");
  m.attr("OP_NONE") = (int)FSTX_OP_NONE;
  m.attr("OP_EQ") = (int)FSTX_OP_EQ;
  m.attr("OP_NE") = (int)FSTX_OP_NE;
  m.attr("OP_LT") = (int)FSTX_OP_LT;
  m.attr("OP_LE") = (int)FSTX_OP_LE;
  m.attr("OP_GT") = (int)FSTX_OP_GT;
  m.attr("OP_GE") = (int)FSTX_OP_GE;
  m.attr("OP_IN") = (int)FSTX_OP_IN;
  m.attr("OP_ISNA") = (int)FSTX_OP_ISNA;
  m.attr("OP_NOTNA") = (int)FSTX_OP_NOTNA;
  m.attr("VERIFY_HASH") = (int)FSTX_VERIFY_HASH;
  m.attr("VERIFY_NULLS") = (int)FSTX_VERIFY_NULLS;
}

}  // namespace fstxpy
