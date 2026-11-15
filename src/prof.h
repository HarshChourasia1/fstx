// FST-X profiling counters (M4). Compiled out unless the package is built with -DFSTX_PROF: then FSTX_PROF_SCOPE
// adds the time spent in a scope (CPU timestamp counter) to a per-thread counter, and fstx_prof_impl() in R
// sums them over all threads. Times are inclusive (a scope inside another counts in both). No R headers.
#ifndef FSTX_PROF_H
#define FSTX_PROF_H

#include <cstdint>

namespace fstx {
namespace prof {

enum Ev : int {
  // read
  R_TOTAL, R_ALLOC, R_HASH, R_DECOMP, R_DEC_INT, R_DEC_DBL, R_FOR32, R_FOR64, R_DELTA_SUM, R_ALP, R_ALPRD,
  R_SPLIT, R_RLE, R_DICT, R_NA, R_STR_DEC, R_STR_DELIVER, R_CODES_CHECK, R_FACTOR_CHECK, R_COPY, R_RELEASE,
  R_LAZY_PAGE,
  // write
  W_TOTAL, W_PREP_STR, W_STATS, W_NA, W_INT_TOP, W_DBL_TOP, W_CHOOSE, W_ALP_SEARCH, W_ALPRD_SEARCH, W_FORPLAN,
  W_PACK, W_DELTA_MAKE, W_ALP_ENC, W_SPLIT, W_DICT_BUILD, W_ZSTD_EST, W_STR_ENC, W_COMPRESS, W_HASH, W_PUT,
  W_FSST_TABLE, W_FSST_ENC, W_STR_GET, W_STR_CHOOSE,
  N_EV
};

const char* name(int e);
bool enabled();
void reset();
// seconds and call counts per event, summed over threads; false when profiling is compiled out
bool snapshot(double* secs, double* counts);

#ifdef FSTX_PROF
uint64_t now();
void add(int e, uint64_t ticks);
struct Scope {
  int e;
  uint64_t t0;
  bool on = true;
  explicit Scope(int ev) : e(ev), t0(now()) {}
  void stop() {
    if (on) add(e, now() - t0);
    on = false;
  }
  ~Scope() { stop(); }
};
#define FSTX_PROF_CAT2(a, b) a##b
#define FSTX_PROF_CAT(a, b) FSTX_PROF_CAT2(a, b)
#define FSTX_PROF_SCOPE(ev) ::fstx::prof::Scope FSTX_PROF_CAT(fstx_prof_, __LINE__)(::fstx::prof::ev)
#define FSTX_PROF_BEGIN(var, ev) ::fstx::prof::Scope var(::fstx::prof::ev)
#define FSTX_PROF_BEGIN_E(var, ev) ::fstx::prof::Scope var(ev)
#define FSTX_PROF_END(var) var.stop()
#else
#define FSTX_PROF_SCOPE(ev) ((void)0)
#define FSTX_PROF_BEGIN(var, ev) ((void)0)
#define FSTX_PROF_BEGIN_E(var, ev) ((void)0)
#define FSTX_PROF_END(var) ((void)0)
#endif

}  // namespace prof
}  // namespace fstx

#endif
