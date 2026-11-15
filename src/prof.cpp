// FST-X profiling counters (see prof.h). MIT licence. No R headers.
#include "prof.h"

#include <chrono>
#include <mutex>
#include <vector>

#if defined(FSTX_PROF) && (defined(__x86_64__) || defined(_M_X64) || defined(__i386__))
#include <x86intrin.h>
#define FSTX_PROF_TSC 1
#endif

namespace fstx {
namespace prof {

static const char* kNames[N_EV] = {
    "r_total", "r_alloc", "r_hash", "r_decomp", "r_dec_int", "r_dec_dbl", "r_for32", "r_for64", "r_delta_sum",
    "r_alp", "r_alprd", "r_split", "r_rle", "r_dict", "r_na", "r_str_dec", "r_str_deliver", "r_codes_check",
    "r_factor_check", "r_copy", "r_release", "r_lazy_page",
    "w_total", "w_prep_str", "w_stats", "w_na", "w_int_top", "w_dbl_top", "w_choose", "w_alp_search",
    "w_alprd_search", "w_forplan", "w_pack", "w_delta_make", "w_alp_enc", "w_split", "w_dict_build", "w_zstd_est",
    "w_str_enc", "w_compress", "w_hash", "w_put", "w_fsst_table", "w_fsst_enc", "w_str_get", "w_str_choose"};

const char* name(int e) { return (e >= 0 && e < N_EV) ? kNames[e] : ""; }

#ifdef FSTX_PROF
namespace {
struct Tls {
  uint64_t t[N_EV];
  uint64_t c[N_EV];
};
std::mutex& mu() {
  static std::mutex m;
  return m;
}
std::vector<Tls*>& all() {
  static std::vector<Tls*> v;  // never freed: pool threads live as long as the DLL
  return v;
}
struct Clock0 {
  uint64_t tsc;
  std::chrono::steady_clock::time_point wall;
};
Clock0& c0() {
  static Clock0 c{now(), std::chrono::steady_clock::now()};
  return c;
}
Tls* tls() {
  thread_local Tls* p = nullptr;
  if (!p) {
    p = new Tls();
    for (int i = 0; i < N_EV; ++i) p->t[i] = p->c[i] = 0;
    std::lock_guard<std::mutex> g(mu());
    all().push_back(p);
  }
  return p;
}
}  // namespace

uint64_t now() {
#ifdef FSTX_PROF_TSC
  return __rdtsc();
#else
  return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch()).count();
#endif
}

void add(int e, uint64_t ticks) {
  Tls* p = tls();
  p->t[e] += ticks;
  p->c[e] += 1;
}

bool enabled() { return true; }

void reset() {
  std::lock_guard<std::mutex> g(mu());
  for (Tls* p : all())
    for (int i = 0; i < N_EV; ++i) p->t[i] = p->c[i] = 0;
  c0();
}

bool snapshot(double* secs, double* counts) {
  // ticks per second from the timestamp counter against the steady clock since the first use
  const Clock0& z = c0();
  const uint64_t t1 = now();
  const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - z.wall).count();
  double tps = 1e9;
#ifdef FSTX_PROF_TSC
  if (wall > 0.01 && t1 > z.tsc) tps = (double)(t1 - z.tsc) / wall;
#else
  (void)t1;
#endif
  std::lock_guard<std::mutex> g(mu());
  for (int i = 0; i < N_EV; ++i) secs[i] = counts[i] = 0;
  for (Tls* p : all())
    for (int i = 0; i < N_EV; ++i) {
      secs[i] += (double)p->t[i] / tps;
      counts[i] += (double)p->c[i];
    }
  return true;
}
#else
bool enabled() { return false; }
void reset() {}
bool snapshot(double*, double*) { return false; }
#endif

}  // namespace prof
}  // namespace fstx
