// FST-X R layer, M4: profiling counters, SIMD control, kernel self-test and micro-benchmark. MIT licence.
#include <cpp11.hpp>

#include <cctype>
#include <string>

#include "kern_test.h"
#include "prof.h"
#include "simd.h"

namespace {
cpp11::writable::strings target_names(int64_t mask) {
  cpp11::writable::strings out;
  for (int b = 0; b < 63; ++b)
    if (mask & (1LL << b)) out.push_back(fstx::simd::target_name(1LL << b));
  return out;
}
std::string lower(std::string s) {
  for (char& c : s) c = (char)std::tolower((unsigned char)c);
  return s;
}
}  // namespace

// Seconds and call counts per profiled event, summed over threads (NULL when the package was built without
// -DFSTX_PROF). reset = TRUE zeroes the counters afterwards.
[[cpp11::register]]
SEXP fstx_prof_impl(bool reset) {
  using namespace fstx::prof;
  if (!enabled()) return R_NilValue;
  double secs[N_EV], counts[N_EV];
  snapshot(secs, counts);
  if (reset) fstx::prof::reset();
  cpp11::writable::doubles s(N_EV), c(N_EV);
  cpp11::writable::strings nm(N_EV);
  for (int i = 0; i < N_EV; ++i) {
    s[i] = secs[i];
    c[i] = counts[i];
    nm[i] = name(i);
  }
  using namespace cpp11::literals;
  return cpp11::writable::list({"event"_nm = nm, "seconds"_nm = s, "calls"_nm = c});
}

// SIMD state: whether the kernels are on, the target in use, the targets this CPU runs and those compiled.
[[cpp11::register]]
SEXP fstx_simd_info_impl() {
  using namespace cpp11::literals;
  const int64_t a = fstx::simd::active_target();
  cpp11::writable::strings act(1);
  act[0] = a ? fstx::simd::target_name(a) : "scalar";
  cpp11::writable::strings disp(1);
  disp[0] = fstx::simd::target_name(fstx::simd::dispatched_target());
  return cpp11::writable::list({"simd"_nm = cpp11::as_sexp(fstx::simd::on()), "target"_nm = act, "dispatch"_nm = disp,
                                "cpu"_nm = target_names(fstx::simd::cpu_targets()),
                                "compiled"_nm = target_names(fstx::simd::compiled_targets())});
}

// target: "scalar" (plain loops), "auto" (best target the CPU runs), or a target name (sse2, sse4, avx2, avx3).
// Returns the target now in use; an error when the CPU cannot run the named target.
[[cpp11::register]]
std::string fstx_simd_set_impl(std::string target) {
  const std::string t = lower(target);
  bool ok;
  if (t == "scalar" || t == "off" || t == "none") {
    ok = fstx::simd::set_mode(0, 0);
  } else if (t == "auto" || t == "on" || t == "best") {
    ok = fstx::simd::set_mode(1, 0);
  } else {
    int64_t bit = 0;
    const int64_t comp = fstx::simd::compiled_targets();
    for (int b = 0; b < 63; ++b)
      if ((comp & (1LL << b)) && lower(fstx::simd::target_name(1LL << b)) == (t == "avx512" ? "avx3" : t)) bit = 1LL << b;
    if (!bit) cpp11::stop("fstx: SIMD target '%s' is not compiled in", target.c_str());
    ok = fstx::simd::set_mode(2, bit);
    if (!ok) cpp11::stop("fstx: this CPU cannot run SIMD target '%s'", target.c_str());
  }
  const int64_t a = fstx::simd::active_target();
  return a ? fstx::simd::target_name(a) : "scalar";
}

// Every SIMD kernel against its plain version on random and edge data, with the target in use now.
[[cpp11::register]]
SEXP fstx_kernel_selftest_impl(double seed, int iters) {
  std::vector<fstx::ktest::KResult> res;
  fstx::ktest::selftest((uint64_t)seed, iters, res);
  cpp11::writable::strings k((R_xlen_t)res.size());
  cpp11::writable::integers c((R_xlen_t)res.size()), m((R_xlen_t)res.size());
  for (size_t i = 0; i < res.size(); ++i) {
    k[(R_xlen_t)i] = res[i].kernel;
    c[(R_xlen_t)i] = res[i].cases;
    m[(R_xlen_t)i] = res[i].mismatches;
  }
  using namespace cpp11::literals;
  return cpp11::writable::list({"kernel"_nm = k, "cases"_nm = c, "mismatches"_nm = m});
}

// GB/s of decoded output (or input where named), plain loop vs SIMD kernel, with the target in use now.
[[cpp11::register]]
SEXP fstx_kernel_bench_impl(double min_time) {
  std::vector<fstx::ktest::KBench> res;
  fstx::ktest::bench(min_time, res);
  cpp11::writable::strings k((R_xlen_t)res.size());
  cpp11::writable::doubles s((R_xlen_t)res.size()), v((R_xlen_t)res.size());
  for (size_t i = 0; i < res.size(); ++i) {
    k[(R_xlen_t)i] = res[i].kernel;
    s[(R_xlen_t)i] = res[i].scalar_gbps;
    v[(R_xlen_t)i] = res[i].simd_gbps;
  }
  using namespace cpp11::literals;
  return cpp11::writable::list({"kernel"_nm = k, "scalar_gbps"_nm = s, "simd_gbps"_nm = v});
}
