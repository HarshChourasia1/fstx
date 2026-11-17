// fstx Python binding: the module fstx._core, its exception and build information. MIT licence.
#include "binding.h"

#include <nanobind/stl/string.h>

#include <cstdint>
#include <cstring>
#include <vector>

#include "kern_test.h"
#include "simd.h"

#define FSTX_PY_STR2(x) #x
#define FSTX_PY_STR(x) FSTX_PY_STR2(x)

namespace fstxpy {

static PyObject* g_error = nullptr;  // fstx.FstxError; the module keeps it alive

void raise_error(const fstx_error& err) {
  // message is a fixed buffer the engine fills with UTF-8; be safe about the terminator
  const void* z = std::memchr(err.message, 0, sizeof(err.message));
  const size_t n = z ? (size_t)((const char*)z - err.message) : sizeof(err.message);
  nb::object col = err.column >= 0 ? nb::object(nb::int_(err.column)) : nb::object(nb::none());
  nb::object args = nb::make_tuple(nb::str(err.message, n), err.code, col);
  PyErr_SetObject(g_error, args.ptr());
  throw nb::python_error();
}

static nb::list target_names(int64_t mask) {
  nb::list out;
  for (int b = 0; b < 63; ++b)
    if (mask & (1LL << b)) out.append(nb::str(fstx::simd::target_name(1LL << b)));
  return out;
}

// What this build is: for bug reports and for checking a new compiler or platform.
static nb::dict build_info() {
  nb::dict d;
  d["engine_version"] = FSTX_PY_STR(FSTX_VERSION);
  d["compiler"] = FSTX_PY_COMPILER;
#ifdef Py_LIMITED_API
  d["abi3"] = true;
#else
  d["abi3"] = false;
#endif
  d["simd"] = fstx::simd::on();
  const int64_t a = fstx::simd::active_target();
  d["simd_target"] = a ? fstx::simd::target_name(a) : "scalar";
  d["simd_dispatch"] = fstx::simd::target_name(fstx::simd::dispatched_target());
  d["simd_cpu"] = target_names(fstx::simd::cpu_targets());
  d["simd_compiled"] = target_names(fstx::simd::compiled_targets());
  d["hardware_threads"] = fstx_hardware_threads();
  return d;
}

// target: "off" (plain loops), "auto" (best the CPU runs) or one compiled target's name, any case
// ("avx2", "sse4", "sse2", "neon", ...). Returns the target now in use ("scalar" when off).
static std::string set_simd(const std::string& target) {
  std::string t;
  for (char c : target) t += (char)((c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c);
  bool ok = false;
  if (t == "off" || t == "scalar") {
    ok = fstx::simd::set_mode(0, 0);
  } else if (t == "auto") {
    ok = fstx::simd::set_mode(1, 0);
  } else {
    const int64_t comp = fstx::simd::compiled_targets();
    int64_t bit = 0;
    for (int b = 0; b < 63; ++b) {
      if (!(comp & (1LL << b))) continue;
      std::string name;
      for (const char* p = fstx::simd::target_name(1LL << b); *p; ++p)
        name += (char)((*p >= 'A' && *p <= 'Z') ? *p - 'A' + 'a' : *p);
      if (name == t) bit = 1LL << b;
    }
    if (!bit) throw nb::value_error(("SIMD target '" + target + "' is not compiled into this build").c_str());
    ok = fstx::simd::set_mode(2, bit);
  }
  if (!ok) throw nb::value_error(("this CPU cannot run SIMD target '" + target + "'").c_str());
  const int64_t a = fstx::simd::active_target();
  return a ? fstx::simd::target_name(a) : "scalar";
}

// The engine's kernel self-test: every SIMD kernel against its plain version on random input, under the
// SIMD target in use. Returns [(kernel, cases, mismatches)].
static nb::list kernel_selftest(uint64_t seed, int iters) {
  std::vector<fstx::ktest::KResult> res;
  {
    nb::gil_scoped_release nogil;
    fstx::ktest::selftest(seed, iters, res);
  }
  nb::list out;
  for (const auto& r : res) out.append(nb::make_tuple(r.kernel, r.cases, r.mismatches));
  return out;
}

}  // namespace fstxpy

NB_MODULE(_core, m) {
  using namespace fstxpy;
  m.doc() = "fstx engine binding (private: use the fstx package)";

  // FstxError(message, code, column): fstx/__init__.py gives it .code and .column properties.
  g_error = PyErr_NewException("fstx.FstxError", PyExc_Exception, nullptr);
  if (!g_error) throw nb::python_error();
  m.attr("FstxError") = nb::borrow(g_error);

  m.attr("ERR_IO") = (int)FSTX_ERR_IO;
  m.attr("ERR_FORMAT") = (int)FSTX_ERR_FORMAT;
  m.attr("ERR_CORRUPT") = (int)FSTX_ERR_CORRUPT;
  m.attr("ERR_CHECKSUM") = (int)FSTX_ERR_CHECKSUM;
  m.attr("ERR_ARG") = (int)FSTX_ERR_ARG;
  m.attr("ERR_MEMORY") = (int)FSTX_ERR_MEMORY;
  m.attr("ERR_HOST") = (int)FSTX_ERR_HOST;
  m.attr("ERR_TRANSLATE") = (int)FSTX_ERR_TRANSLATE;
  m.attr("ERR_INTERNAL") = (int)FSTX_ERR_INTERNAL;

  m.def("build_info", &build_info, "What this build is: engine version, compiler, SIMD targets.");
  m.def("set_simd", &set_simd, nb::arg("target"),
        "Choose the SIMD kernels: 'off', 'auto' or a compiled target name. Returns the target in use.");
  m.def("kernel_selftest", &kernel_selftest, nb::arg("seed") = 1, nb::arg("iters") = 20,
        "Engine kernel self-test under the SIMD target in use: [(kernel, cases, mismatches)].");
  // Stops the engine's worker threads; fstx/__init__.py registers it with atexit (threads must not be
  // alive when the interpreter unloads the module, on Windows above all).
  m.def("shutdown", []() {
    nb::gil_scoped_release nogil;
    fstx_shutdown();
  });

  init_file(m);
  init_write(m);
}
