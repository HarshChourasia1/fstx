// Registration code in the form cpp11::cpp_register() generates (written by hand: the 'decor'
// package that cpp_register needs is not installed on the build machine).

#include "cpp11/declarations.hpp"
#include <R_ext/Visibility.h>
#include <R_ext/Rdynload.h>

void fstx_init_altrep(DllInfo* dll);

// r_glue.cpp
int fstx_write_impl(SEXP cols, SEXP path, SEXP names, SEXP phys, SEXP logical, SEXP ordered, SEXP levels, SEXP tzone, SEXP attrs, SEXP table_attrs, double nrow, int codec, int level, int threads, bool utf8_locale, int profile, int keep_pct, int force_encoding, bool append, int test_crash);
extern "C" SEXP _fstx_fstx_write_impl(SEXP cols, SEXP path, SEXP names, SEXP phys, SEXP logical, SEXP ordered, SEXP levels, SEXP tzone, SEXP attrs, SEXP table_attrs, SEXP nrow, SEXP codec, SEXP level, SEXP threads, SEXP utf8_locale, SEXP profile, SEXP keep_pct, SEXP force_encoding, SEXP append, SEXP test_crash) {
  BEGIN_CPP11
    return cpp11::as_sexp(fstx_write_impl(cpp11::as_cpp<cpp11::decay_t<SEXP>>(cols), cpp11::as_cpp<cpp11::decay_t<SEXP>>(path), cpp11::as_cpp<cpp11::decay_t<SEXP>>(names), cpp11::as_cpp<cpp11::decay_t<SEXP>>(phys), cpp11::as_cpp<cpp11::decay_t<SEXP>>(logical), cpp11::as_cpp<cpp11::decay_t<SEXP>>(ordered), cpp11::as_cpp<cpp11::decay_t<SEXP>>(levels), cpp11::as_cpp<cpp11::decay_t<SEXP>>(tzone), cpp11::as_cpp<cpp11::decay_t<SEXP>>(attrs), cpp11::as_cpp<cpp11::decay_t<SEXP>>(table_attrs), cpp11::as_cpp<cpp11::decay_t<double>>(nrow), cpp11::as_cpp<cpp11::decay_t<int>>(codec), cpp11::as_cpp<cpp11::decay_t<int>>(level), cpp11::as_cpp<cpp11::decay_t<int>>(threads), cpp11::as_cpp<cpp11::decay_t<bool>>(utf8_locale), cpp11::as_cpp<cpp11::decay_t<int>>(profile), cpp11::as_cpp<cpp11::decay_t<int>>(keep_pct), cpp11::as_cpp<cpp11::decay_t<int>>(force_encoding), cpp11::as_cpp<cpp11::decay_t<bool>>(append), cpp11::as_cpp<cpp11::decay_t<int>>(test_crash)));
  END_CPP11
}
SEXP fstx_read_impl(SEXP path, SEXP columns, double from, double to, int threads, bool verify, int mode, SEXP as_factor, SEXP presize, SEXP rows);
extern "C" SEXP _fstx_fstx_read_impl(SEXP path, SEXP columns, SEXP from, SEXP to, SEXP threads, SEXP verify, SEXP mode, SEXP as_factor, SEXP presize, SEXP rows) {
  BEGIN_CPP11
    return cpp11::as_sexp(fstx_read_impl(cpp11::as_cpp<cpp11::decay_t<SEXP>>(path), cpp11::as_cpp<cpp11::decay_t<SEXP>>(columns), cpp11::as_cpp<cpp11::decay_t<double>>(from), cpp11::as_cpp<cpp11::decay_t<double>>(to), cpp11::as_cpp<cpp11::decay_t<int>>(threads), cpp11::as_cpp<cpp11::decay_t<bool>>(verify), cpp11::as_cpp<cpp11::decay_t<int>>(mode), cpp11::as_cpp<cpp11::decay_t<SEXP>>(as_factor), cpp11::as_cpp<cpp11::decay_t<SEXP>>(presize), cpp11::as_cpp<cpp11::decay_t<SEXP>>(rows)));
  END_CPP11
}
// r_lazy.cpp
SEXP fstx_codes_impl(SEXP xp, SEXP remaps, SEXP levels, bool ordered, bool lazy);
extern "C" SEXP _fstx_fstx_codes_impl(SEXP xp, SEXP remaps, SEXP levels, SEXP ordered, SEXP lazy) {
  BEGIN_CPP11
    return cpp11::as_sexp(fstx_codes_impl(cpp11::as_cpp<cpp11::decay_t<SEXP>>(xp), cpp11::as_cpp<cpp11::decay_t<SEXP>>(remaps), cpp11::as_cpp<cpp11::decay_t<SEXP>>(levels), cpp11::as_cpp<cpp11::decay_t<bool>>(ordered), cpp11::as_cpp<cpp11::decay_t<bool>>(lazy)));
  END_CPP11
}
void fstx_presize_impl(double nodes, double bytes);
extern "C" SEXP _fstx_fstx_presize_impl(SEXP nodes, SEXP bytes) {
  BEGIN_CPP11
    fstx_presize_impl(cpp11::as_cpp<cpp11::decay_t<double>>(nodes), cpp11::as_cpp<cpp11::decay_t<double>>(bytes));
    return R_NilValue;
  END_CPP11
}
SEXP fstx_col_state_impl(SEXP x);
extern "C" SEXP _fstx_fstx_col_state_impl(SEXP x) {
  BEGIN_CPP11
    return cpp11::as_sexp(fstx_col_state_impl(cpp11::as_cpp<cpp11::decay_t<SEXP>>(x)));
  END_CPP11
}
SEXP fstx_meta_impl(SEXP path);
extern "C" SEXP _fstx_fstx_meta_impl(SEXP path) {
  BEGIN_CPP11
    return cpp11::as_sexp(fstx_meta_impl(cpp11::as_cpp<cpp11::decay_t<SEXP>>(path)));
  END_CPP11
}
SEXP fstx_pages_impl(SEXP path);
extern "C" SEXP _fstx_fstx_pages_impl(SEXP path) {
  BEGIN_CPP11
    return cpp11::as_sexp(fstx_pages_impl(cpp11::as_cpp<cpp11::decay_t<SEXP>>(path)));
  END_CPP11
}
SEXP fstx_filter_impl(SEXP path, SEXP preds, double from, double to, int threads, bool verify, bool plan_only);
extern "C" SEXP _fstx_fstx_filter_impl(SEXP path, SEXP preds, SEXP from, SEXP to, SEXP threads, SEXP verify, SEXP plan_only) {
  BEGIN_CPP11
    return cpp11::as_sexp(fstx_filter_impl(cpp11::as_cpp<cpp11::decay_t<SEXP>>(path), cpp11::as_cpp<cpp11::decay_t<SEXP>>(preds), cpp11::as_cpp<cpp11::decay_t<double>>(from), cpp11::as_cpp<cpp11::decay_t<double>>(to), cpp11::as_cpp<cpp11::decay_t<int>>(threads), cpp11::as_cpp<cpp11::decay_t<bool>>(verify), cpp11::as_cpp<cpp11::decay_t<bool>>(plan_only)));
  END_CPP11
}
void fstx_compact_impl(SEXP path, SEXP tmp);
extern "C" SEXP _fstx_fstx_compact_impl(SEXP path, SEXP tmp) {
  BEGIN_CPP11
    fstx_compact_impl(cpp11::as_cpp<cpp11::decay_t<SEXP>>(path), cpp11::as_cpp<cpp11::decay_t<SEXP>>(tmp));
    return R_NilValue;
  END_CPP11
}
SEXP fstx_sizes_impl(SEXP path);
extern "C" SEXP _fstx_fstx_sizes_impl(SEXP path) {
  BEGIN_CPP11
    return cpp11::as_sexp(fstx_sizes_impl(cpp11::as_cpp<cpp11::decay_t<SEXP>>(path)));
  END_CPP11
}
int fstx_hw_threads();
extern "C" SEXP _fstx_fstx_hw_threads() {
  BEGIN_CPP11
    return cpp11::as_sexp(fstx_hw_threads());
  END_CPP11
}

// r_m4.cpp
SEXP fstx_prof_impl(bool reset);
extern "C" SEXP _fstx_fstx_prof_impl(SEXP reset) {
  BEGIN_CPP11
    return cpp11::as_sexp(fstx_prof_impl(cpp11::as_cpp<cpp11::decay_t<bool>>(reset)));
  END_CPP11
}

SEXP fstx_simd_info_impl();
extern "C" SEXP _fstx_fstx_simd_info_impl() {
  BEGIN_CPP11
    return cpp11::as_sexp(fstx_simd_info_impl());
  END_CPP11
}
std::string fstx_simd_set_impl(std::string target);
extern "C" SEXP _fstx_fstx_simd_set_impl(SEXP target) {
  BEGIN_CPP11
    return cpp11::as_sexp(fstx_simd_set_impl(cpp11::as_cpp<cpp11::decay_t<std::string>>(target)));
  END_CPP11
}
SEXP fstx_kernel_selftest_impl(double seed, int iters);
extern "C" SEXP _fstx_fstx_kernel_selftest_impl(SEXP seed, SEXP iters) {
  BEGIN_CPP11
    return cpp11::as_sexp(fstx_kernel_selftest_impl(cpp11::as_cpp<cpp11::decay_t<double>>(seed), cpp11::as_cpp<cpp11::decay_t<int>>(iters)));
  END_CPP11
}
SEXP fstx_kernel_bench_impl(double min_time);
extern "C" SEXP _fstx_fstx_kernel_bench_impl(SEXP min_time) {
  BEGIN_CPP11
    return cpp11::as_sexp(fstx_kernel_bench_impl(cpp11::as_cpp<cpp11::decay_t<double>>(min_time)));
  END_CPP11
}

extern "C" {
static const R_CallMethodDef CallEntries[] = {
    {"_fstx_fstx_prof_impl", (DL_FUNC) &_fstx_fstx_prof_impl, 1},
    {"_fstx_fstx_simd_info_impl", (DL_FUNC) &_fstx_fstx_simd_info_impl, 0},
    {"_fstx_fstx_simd_set_impl", (DL_FUNC) &_fstx_fstx_simd_set_impl, 1},
    {"_fstx_fstx_kernel_selftest_impl", (DL_FUNC) &_fstx_fstx_kernel_selftest_impl, 2},
    {"_fstx_fstx_kernel_bench_impl", (DL_FUNC) &_fstx_fstx_kernel_bench_impl, 1},
    {"_fstx_fstx_write_impl", (DL_FUNC) &_fstx_fstx_write_impl, 20},
    {"_fstx_fstx_pages_impl", (DL_FUNC) &_fstx_fstx_pages_impl, 1},
    {"_fstx_fstx_read_impl", (DL_FUNC) &_fstx_fstx_read_impl, 10},
    {"_fstx_fstx_codes_impl", (DL_FUNC) &_fstx_fstx_codes_impl, 5},
    {"_fstx_fstx_presize_impl", (DL_FUNC) &_fstx_fstx_presize_impl, 2},
    {"_fstx_fstx_col_state_impl", (DL_FUNC) &_fstx_fstx_col_state_impl, 1},
    {"_fstx_fstx_meta_impl", (DL_FUNC) &_fstx_fstx_meta_impl, 1},
    {"_fstx_fstx_hw_threads", (DL_FUNC) &_fstx_fstx_hw_threads, 0},
    {"_fstx_fstx_filter_impl", (DL_FUNC) &_fstx_fstx_filter_impl, 7},
    {"_fstx_fstx_compact_impl", (DL_FUNC) &_fstx_fstx_compact_impl, 2},
    {"_fstx_fstx_sizes_impl", (DL_FUNC) &_fstx_fstx_sizes_impl, 1},
    {NULL, NULL, 0}
};
}

extern "C" attribute_visible void R_init_fstx(DllInfo* dll){
  R_registerRoutines(dll, NULL, CallEntries, NULL, NULL);
  R_useDynamicSymbols(dll, FALSE);
  R_forceSymbols(dll, TRUE);
  fstx_init_altrep(dll);
}
