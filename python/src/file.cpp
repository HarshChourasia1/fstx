// fstx Python binding: opening a file and its metadata (no column data). MIT licence.
#include "binding.h"

#include <nanobind/stl/string.h>

#include <cstring>

namespace fstxpy {

// The names R and the golden sidecars use (DESIGN.md section 4: phys integer / double / logical / character,
// logical none / factor / Date / POSIXct).
static const char* phys_name(int p) {
  switch (p) {
    case FSTX_INT32: return "integer";
    case FSTX_FLOAT64: return "double";
    case FSTX_LOGICAL: return "logical";
    case FSTX_STRING: return "character";
    default: return "unknown";
  }
}
static const char* logical_name(int l) {
  switch (l) {
    case FSTX_LT_NONE: return "none";
    case FSTX_LT_FACTOR: return "factor";
    case FSTX_LT_DATE: return "Date";
    case FSTX_LT_POSIXCT: return "POSIXct";
    default: return "unknown";
  }
}
static const char* profile_name(int p) {
  switch (p) {
    case FSTX_PROFILE_FAST: return "fast";
    case FSTX_PROFILE_BALANCED: return "balanced";
    case FSTX_PROFILE_SMALL: return "small";
    default: return "unknown";
  }
}
static nb::object bytes_or_none(const uint8_t* p, size_t n) {
  if (!p) return nb::none();
  return nb::bytes((const char*)p, n);
}

static File* open_file(const std::string& path) {
  fstx_file* f = nullptr;
  fstx_error err;
  std::memset(&err, 0, sizeof(err));
  int rc;
  {
    nb::gil_scoped_release nogil;  // maps the file and checks header, footer and page directory
    rc = fstx_open(path.c_str(), &f, &err);
  }
  check(rc, err);
  File* out = new File();
  out->f = f;
  out->path = path;
  return out;
}

static nb::dict file_info(const File& F) {
  fstx_file_info fi;
  fstx_file_info_get(F.get(), &fi);
  nb::dict d;
  d["path"] = F.path;
  d["nrow"] = fi.n_rows;
  d["ncol"] = fi.n_cols;
  d["row_groups"] = fi.n_row_groups;
  d["pages"] = fi.n_pages;
  d["file_size"] = fi.file_size;
  d["row_group_size"] = fi.row_group_size;
  d["format_version"] = fi.format_version;
  d["profile"] = profile_name(fi.profile);
  d["table_attrs"] = bytes_or_none(fi.table_attrs, fi.table_attrs_len);  // fstx attribute blob (spec 9.4)
  return d;
}

static nb::dict column_info(const File& F, uint32_t col) {
  fstx_file* f = F.get();
  fstx_column_info ci;
  if (fstx_column_info_get(f, col, &ci) != FSTX_OK) throw nb::index_error("column index out of range");
  nb::dict d;
  d["index"] = col;
  d["name"] = nb::str(ci.name ? ci.name : "", ci.name_len);
  d["phys"] = phys_name(ci.phys);
  d["logical"] = logical_name(ci.logical);
  d["ordered"] = ci.ordered != 0;
  d["num_levels"] = ci.n_levels;
  if (ci.has_tzone) d["tzone"] = nb::str(ci.tzone ? ci.tzone : "", ci.tzone_len);
  else d["tzone"] = nb::none();
  d["attrs"] = bytes_or_none(ci.attrs, ci.attrs_len);
  d["compressed_bytes"] = ci.compressed_bytes;
  d["raw_bytes"] = ci.raw_bytes;
  d["pages"] = ci.n_pages;
  return d;
}

static nb::list columns(const File& F) {
  fstx_file_info fi;
  fstx_file_info_get(F.get(), &fi);
  nb::list out;
  for (uint32_t c = 0; c < fi.n_cols; ++c) out.append(column_info(F, c));
  return out;
}

// The levels of a factor column, in order (an empty list for other columns).
static nb::list levels(const File& F, uint32_t col) {
  fstx_file* f = F.get();
  fstx_column_info ci;
  if (fstx_column_info_get(f, col, &ci) != FSTX_OK) throw nb::index_error("column index out of range");
  nb::list out;
  for (uint32_t i = 0; i < ci.n_levels; ++i) {
    const char* s = nullptr;
    uint32_t len = 0;
    if (fstx_level_get(f, col, i, &s, &len) != FSTX_OK) throw nb::index_error("level index out of range");
    out.append(nb::str(s ? s : "", len));
  }
  return out;
}

void init_file(nb::module_& m) {
  nb::class_<File>(m, "File", "An open fstx file (private: use fstx.open).")
      .def_prop_ro("path", [](const File& F) { return F.path; })
      .def_prop_ro("closed", [](const File& F) { return F.f == nullptr; })
      .def("close", &File::close, "Closes the file (unmaps it). Safe to call twice.")
      .def("info", &file_info, "File-level metadata as a dict.")
      .def("columns", &columns, "One dict per column, in file order.")
      .def("column", &column_info, nb::arg("index"), "One column's metadata.")
      .def("levels", &levels, nb::arg("index"), "The levels of a factor column, in order.")
      .def("find_column",
           [](const File& F, const std::string& name) {
             return (int64_t)fstx_find_column(F.get(), name.data(), name.size());
           },
           nb::arg("name"), "Index of the column with this name, or -1.");
  m.def("open", &open_file, nb::arg("path"), nb::rv_policy::take_ownership,
        "Opens an fstx file (path as str, UTF-8 inside).");
}

}  // namespace fstxpy
