// fstx Python binding: what the binding's .cpp files share. MIT licence.
// Layout: one .cpp per area (file.cpp = open + metadata; read.cpp = Arrow stream + filters; write.cpp to come), each exporting
// init_<area>(nb::module_&), called from NB_MODULE in module.cpp and listed in ../CMakeLists.txt.
// The engine is used only through its C interface (fstx_core.h, arrow_abi.h); simd.h / kern_test.h are the
// two C++ engine headers module.cpp reads for build information and the kernel self-test.
#ifndef FSTX_PY_BINDING_H
#define FSTX_PY_BINDING_H

#include <nanobind/nanobind.h>

#include <string>

#include "fstx_core.h"

namespace fstxpy {
namespace nb = nanobind;

// Raises fstx.FstxError (set up in module.cpp) from an engine error: args are (message, code, column),
// column is None when the error names no column. Call with the GIL held. Never returns.
[[noreturn]] void raise_error(const fstx_error& err);
inline void check(int rc, const fstx_error& err) {
  if (rc != FSTX_OK) raise_error(err);
}

// An open file. Closed by close(), by the context manager or when the object is collected. Engine objects
// made from it (plans, column sources) need the file open: keep the Python File alive while they live
// (nb::keep_alive or a nb::object reference) and call File::get() to refuse a closed one.
struct File {
  fstx_file* f = nullptr;
  std::string path;

  File() = default;
  File(const File&) = delete;
  File& operator=(const File&) = delete;
  ~File() { close(); }
  void close() {
    if (f) fstx_close(f);
    f = nullptr;
  }
  // the engine handle; raises ValueError when the file was closed
  fstx_file* get() const {
    if (!f) throw nb::value_error("the fstx file is closed");
    return f;
  }
};

void init_file(nb::module_& m);
void init_write(nb::module_& m);
void init_read(nb::module_& m);

}  // namespace fstxpy

#endif
