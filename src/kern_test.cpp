// FST-X M4: kernel self-test (every SIMD kernel against its plain version, bit for bit, on random and edge data)
// and kernel micro-benchmark (GB/s of decoded output, plain vs SIMD). Both use the SIMD target active now.
// No R headers. MIT licence.
#include <algorithm>
#include <chrono>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#include "kern_test.h"
#include "simd.h"

namespace fstx {
namespace ktest {
namespace {

struct Rng {
  uint64_t s;
  explicit Rng(uint64_t seed) : s(seed) {}
  uint64_t next() {  // splitmix64
    uint64_t z = (s += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
  }
  uint32_t below(uint32_t n) { return n ? (uint32_t)(next() % n) : 0; }
};

template <class T>
bool same(const T* a, const T* b, size_t n) {
  return n == 0 || std::memcmp(a, b, n * sizeof(T)) == 0;  // n == 0: data() may be null
}

struct Acc {
  std::vector<KResult>& out;
  KResult cur;
  Acc(std::vector<KResult>& o, const char* name) : out(o) { cur.kernel = name; }
  void add(bool ok) {
    ++cur.cases;
    if (!ok) ++cur.mismatches;
  }
  ~Acc() { out.push_back(cur); }
};

template <class U>
void test_unpack(Rng& r, int iters, std::vector<KResult>& res, const char* nm_unpack, const char* nm_pack) {
  constexpr int B = (int)sizeof(U) * 8;
  std::vector<U> in(1024 + 8), a(1024), b(1024), vals(1024), pk1(1024 + 8), pk2(1024 + 8);
  Acc au(res, nm_unpack), ap(res, nm_pack);
  for (int it = 0; it < iters; ++it)
    for (int w = 0; w <= B; ++w) {
      for (auto& x : in) x = (U)r.next();
      U base;
      switch (it % 4) {
        case 0: base = 0; break;
        case 1: base = (U)~U(0); break;
        case 2: base = (U)((U)1 << (B - 1)); break;
        default: base = (U)r.next();
      }
      std::fill(a.begin(), a.end(), U(7));
      std::fill(b.begin(), b.end(), U(9));
      kern::unpack<U>(w, in.data(), a.data(), base);
      if constexpr (sizeof(U) == 4) simd::unpack32(w, (const uint32_t*)in.data(), (uint32_t*)b.data(), (uint32_t)base);
      else simd::unpack64(w, (const uint64_t*)in.data(), (uint64_t*)b.data(), (uint64_t)base);
      au.add(same(a.data(), b.data(), 1024));
      // pack: values that fit w bits (edges: all zero, all max)
      const U mask = w == B ? (U)~U(0) : (U)(((U)1 << (w % B)) - 1);
      for (int i = 0; i < 1024; ++i) {
        U v = (U)r.next() & mask;
        if (it % 5 == 1) v = 0;
        if (it % 5 == 2) v = mask;
        vals[i] = v;
      }
      std::fill(pk1.begin(), pk1.end(), U(3));
      std::fill(pk2.begin(), pk2.end(), U(3));
      kern::pack<U>(w, vals.data(), pk1.data());
      if constexpr (sizeof(U) == 4) simd::pack32(w, (const uint32_t*)vals.data(), (uint32_t*)pk2.data());
      else simd::pack64(w, (const uint64_t*)vals.data(), (uint64_t*)pk2.data());
      bool ok = same(pk1.data(), pk2.data(), pk1.size());
      // round trip through the SIMD unpack
      if constexpr (sizeof(U) == 4) simd::unpack32(w, (const uint32_t*)pk2.data(), (uint32_t*)b.data(), 0u);
      else simd::unpack64(w, (const uint64_t*)pk2.data(), (uint64_t*)b.data(), 0u);
      ok = ok && same(vals.data(), b.data(), 1024);
      ap.add(ok);
    }
}

uint32_t pick_len(Rng& r, int it, uint32_t maxn) {
  static const uint32_t edges[] = {0, 1, 2, 3, 4, 5, 7, 8, 9, 15, 16, 17, 31, 32, 33, 63, 64, 65, 127, 128, 1023, 1024};
  const uint32_t ne = sizeof(edges) / sizeof(edges[0]);
  if ((uint32_t)it < ne) return std::min(edges[it], maxn);
  return r.below(maxn + 1);
}

template <class U>
void test_delta(Rng& r, int iters, std::vector<KResult>& res, const char* nm) {
  Acc ac(res, nm);
  std::vector<U> a(1024), b(1024);
  for (int it = 0; it < iters; ++it) {
    uint32_t m = pick_len(r, it, 1024);
    if (m == 0) m = 1;
    for (uint32_t i = 0; i < m; ++i) a[i] = (it % 3 == 0) ? (U)r.below(100) : (U)r.next();
    b = a;
    const U first = (U)r.next();
    kern::delta_sum<U>(a.data(), m, first);
    if constexpr (sizeof(U) == 4) simd::delta32((uint32_t*)b.data(), m, (uint32_t)first);
    else simd::delta64((uint64_t*)b.data(), m, (uint64_t)first);
    ac.add(same(a.data(), b.data(), 1024));
  }
}

template <class T>
void test_minmax(Rng& r, int iters, std::vector<KResult>& res, const char* nm) {
  Acc ac(res, nm);
  std::vector<T> x(1024);
  for (int it = 0; it < iters; ++it) {
    uint32_t m = pick_len(r, it, 1024);
    if (m == 0) m = 1;
    for (uint32_t i = 0; i < m; ++i) {
      x[i] = (T)r.next();
      if (it % 4 == 1) x[i] = (T)(r.below(1000)) - 500;
    }
    if (it % 7 == 3) x[r.below(m)] = std::numeric_limits<T>::min();
    if (it % 7 == 4) x[r.below(m)] = std::numeric_limits<T>::max();
    T a0, a1, b0, b1;
    kern::minmax<T>(x.data(), m, a0, a1);
    if constexpr (sizeof(T) == 4) simd::minmax32((const int32_t*)x.data(), m, (int32_t&)b0, (int32_t&)b1);
    else simd::minmax64((const int64_t*)x.data(), m, (int64_t&)b0, (int64_t&)b1);
    ac.add(a0 == b0 && a1 == b1);
  }
}

void test_alp(Rng& r, int iters, std::vector<KResult>& res) {
  Acc ac(res, "alp_decode");
  std::vector<double> a(1100), b(1100);
  const int64_t two51 = (int64_t)1 << 51;
  const int64_t edge[] = {0, 1, -1, two51 - 1, -two51, two51, -two51 - 1, std::numeric_limits<int64_t>::max(),
                          std::numeric_limits<int64_t>::min(), (int64_t)1 << 53, -((int64_t)1 << 53) + 1};
  for (int it = 0; it < iters; ++it) {
    const uint32_t n = pick_len(r, it, 1100);
    const int e = (int)r.below(19), f = (int)r.below((uint32_t)e + 1);
    for (uint32_t i = 0; i < n; ++i) {
      int64_t q;
      switch (r.below(4)) {
        case 0: q = (int64_t)r.below(2000000) - 1000000; break;
        case 1: q = (int64_t)(r.next() % (uint64_t)two51) - (it % 2 ? two51 / 2 : 0); break;
        case 2: q = edge[r.below(sizeof(edge) / sizeof(edge[0]))]; break;
        default: q = (it % 9 == 0) ? (int64_t)r.next() : (int64_t)r.below(1u << 30);
      }
      std::memcpy(&a[i], &q, 8);
    }
    b = a;
    kern::alp_decode(a.data(), n, e, f);
    simd::alp_decode(b.data(), n, e, f);
    ac.add(same(a.data(), b.data(), a.size()));
  }
}

template <int S>
void test_unsplit(Rng& r, int iters, std::vector<KResult>& res, const char* nm) {
  Acc ac(res, nm);
  for (int it = 0; it < iters; ++it) {
    const uint32_t n = it < 22 ? pick_len(r, it, 5000) : r.below(5000);
    std::vector<uint8_t> src((size_t)n * S), a((size_t)n * S + 1, 5), b((size_t)n * S + 1, 5);
    for (auto& x : src) x = (uint8_t)r.next();
    kern::unsplit<S>(src.data(), a.data(), n);
    if (S == 4) simd::unsplit4(src.data(), b.data(), n);
    else simd::unsplit8(src.data(), b.data(), n);
    ac.add(a == b);
  }
}

template <class T>
void test_na(Rng& r, int iters, std::vector<KResult>& res, const char* nm) {
  Acc ac(res, nm);
  for (int it = 0; it < iters; ++it) {
    const size_t nw = 1 + r.below(40);
    std::vector<uint64_t> w(nw);
    const int dens = it % 5;
    for (auto& x : w) {
      switch (dens) {
        case 0: x = 0; break;
        case 1: x = ~0ull; break;
        case 2: x = r.next() & r.next() & r.next(); break;
        case 3: x = r.next(); break;
        default: x = r.below(3) == 0 ? 0 : (1ull << r.below(64));
      }
    }
    std::vector<T> a(nw * 64);
    for (auto& x : a) {
      uint64_t v = r.next();
      std::memcpy(&x, &v, sizeof(T));
    }
    std::vector<T> b = a;
    T na;
    const uint64_t nab = sizeof(T) == 8 ? 0x7FF00000000007A2ULL : 0x80000000ULL;
    std::memcpy(&na, &nab, sizeof(T));
    kern::na_bitmap<T>(w.data(), nw, a.data(), na);
    if constexpr (sizeof(T) == 4) simd::na_bitmap32(w.data(), nw, (int32_t*)b.data(), (int32_t)na);
    else simd::na_bitmap64(w.data(), nw, (uint64_t*)b.data(), (uint64_t)nab);
    ac.add(same(a.data(), b.data(), a.size()));
  }
}

template <class T>
void test_dict(Rng& r, int iters, std::vector<KResult>& res, const char* nm) {
  Acc ac(res, nm);
  for (int it = 0; it < iters; ++it) {
    const uint32_t n = pick_len(r, it, 3000);
    const uint32_t nd = 1 + r.below(it % 3 == 0 ? 4 : 5000);
    std::vector<T> vals(nd);
    for (auto& v : vals) v = (T)r.next();
    std::vector<int32_t> codes(n);
    for (auto& c : codes) c = (int32_t)r.below(nd);
    const bool bad = n > 0 && it % 6 == 5;
    if (bad) codes[r.below(n)] = (it % 12 == 5) ? -1 - (int32_t)r.below(3) : (int32_t)(nd + r.below(3));
    std::vector<T> a(n), b(n);
    const bool ka = kern::dict_gather<T>(codes.data(), n, vals.data(), nd, a.data());
    bool kb;
    if constexpr (sizeof(T) == 4) {
      if (it % 2) {  // in place, as get_dict does for int32
        std::vector<int32_t> io = codes;
        kb = simd::dict32(io.data(), n, (const int32_t*)vals.data(), nd, io.data());
        std::memcpy(b.data(), io.data(), (size_t)n * 4);
      } else {
        kb = simd::dict32(codes.data(), n, (const int32_t*)vals.data(), nd, (int32_t*)b.data());
      }
    } else {
      kb = simd::dict64(codes.data(), n, (const int64_t*)vals.data(), nd, (int64_t*)b.data());
    }
    ac.add(ka == kb && ka == !bad && (!ka || same(a.data(), b.data(), n)));
  }
}

}  // namespace

void selftest(uint64_t seed, int iters, std::vector<KResult>& res) {
  Rng r(seed);
  test_unpack<uint32_t>(r, iters, res, "unpack32", "pack32");
  test_unpack<uint64_t>(r, iters, res, "unpack64", "pack64");
  test_delta<uint32_t>(r, iters * 8, res, "delta32");
  test_delta<uint64_t>(r, iters * 8, res, "delta64");
  test_minmax<int32_t>(r, iters * 8, res, "minmax32");
  test_minmax<int64_t>(r, iters * 8, res, "minmax64");
  test_alp(r, iters * 8, res);
  test_unsplit<4>(r, iters * 4, res, "unsplit4");
  test_unsplit<8>(r, iters * 4, res, "unsplit8");
  test_na<int32_t>(r, iters * 8, res, "na_bitmap32");
  test_na<double>(r, iters * 8, res, "na_bitmap64");
  test_dict<int32_t>(r, iters * 8, res, "dict32");
  test_dict<int64_t>(r, iters * 8, res, "dict64");
}

// ---------------------------------------------------------------- micro-benchmark
namespace {

double now_s() {
  return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// best of `rounds` timings of fn() (each call handles `bytes` of output); GB/s
template <class F>
double gbps(F fn, double bytes, double min_time) {
  fn();  // warm up
  double best = 1e30;
  for (int round = 0; round < 5; ++round) {
    int reps = 0;
    const double t0 = now_s();
    double t = 0;
    do {
      fn();
      ++reps;
      t = now_s() - t0;
    } while (t < min_time / 5);
    best = std::min(best, t / reps);
  }
  return bytes / best / 1e9;
}

volatile uint64_t g_sink = 0;

}  // namespace

void bench(double min_time, std::vector<KBench>& res) {
  Rng r(42);
  const uint32_t nvec = 256;  // 256 vectors of 1,024 values: 1 MB (u32) / 2 MB (u64) of output
  auto add = [&](const std::string& k, double s, double v) { res.push_back(KBench{k, s, v}); };

  for (int w : {1, 2, 4, 8, 13, 20, 32}) {
    std::vector<uint32_t> in((size_t)nvec * 1024), out((size_t)nvec * 1024);
    for (auto& x : in) x = (uint32_t)r.next();
    const double bytes = (double)nvec * 1024 * 4;
    auto f_s = [&] {
      for (uint32_t v = 0; v < nvec; ++v) kern::unpack<uint32_t>(w, in.data() + (size_t)v * 32 * w, out.data() + (size_t)v * 1024, 7u);
    };
    auto f_v = [&] {
      for (uint32_t v = 0; v < nvec; ++v) simd::unpack32(w, in.data() + (size_t)v * 32 * w, out.data() + (size_t)v * 1024, 7u);
    };
    add("unpack32 w" + std::to_string(w), gbps(f_s, bytes, min_time), gbps(f_v, bytes, min_time));
  }
  for (int w : {8, 20, 40, 64}) {
    std::vector<uint64_t> in((size_t)nvec * 1024), out((size_t)nvec * 1024);
    for (auto& x : in) x = r.next();
    const double bytes = (double)nvec * 1024 * 8;
    auto f_s = [&] {
      for (uint32_t v = 0; v < nvec; ++v) kern::unpack<uint64_t>(w, in.data() + (size_t)v * 16 * w, out.data() + (size_t)v * 1024, 7u);
    };
    auto f_v = [&] {
      for (uint32_t v = 0; v < nvec; ++v) simd::unpack64(w, in.data() + (size_t)v * 16 * w, out.data() + (size_t)v * 1024, 7u);
    };
    add("unpack64 w" + std::to_string(w), gbps(f_s, bytes, min_time), gbps(f_v, bytes, min_time));
  }
  for (int w : {2, 13, 32}) {
    std::vector<uint32_t> in((size_t)nvec * 1024), out((size_t)nvec * 1024);
    for (auto& x : in) x = (uint32_t)r.next() & (w == 32 ? ~0u : ((1u << w) - 1));
    const double bytes = (double)nvec * 1024 * 4;
    auto f_s = [&] {
      for (uint32_t v = 0; v < nvec; ++v) kern::pack<uint32_t>(w, in.data() + (size_t)v * 1024, out.data() + (size_t)v * 32 * w);
    };
    auto f_v = [&] {
      for (uint32_t v = 0; v < nvec; ++v) simd::pack32(w, in.data() + (size_t)v * 1024, out.data() + (size_t)v * 32 * w);
    };
    add("pack32 w" + std::to_string(w) + " (input GB/s)", gbps(f_s, bytes, min_time), gbps(f_v, bytes, min_time));
  }
  {
    std::vector<uint32_t> a((size_t)nvec * 1024);
    for (auto& x : a) x = (uint32_t)r.below(100);
    const double bytes = (double)a.size() * 4;
    auto f_s = [&] {
      for (uint32_t v = 0; v < nvec; ++v) kern::delta_sum<uint32_t>(a.data() + (size_t)v * 1024, 1024, 5u);
    };
    auto f_v = [&] {
      for (uint32_t v = 0; v < nvec; ++v) simd::delta32(a.data() + (size_t)v * 1024, 1024, 5u);
    };
    add("delta32", gbps(f_s, bytes, min_time), gbps(f_v, bytes, min_time));
    std::vector<uint64_t> b((size_t)nvec * 1024);
    for (auto& x : b) x = r.below(100);
    auto g_s = [&] {
      for (uint32_t v = 0; v < nvec; ++v) kern::delta_sum<uint64_t>(b.data() + (size_t)v * 1024, 1024, 5u);
    };
    auto g_v = [&] {
      for (uint32_t v = 0; v < nvec; ++v) simd::delta64(b.data() + (size_t)v * 1024, 1024, 5u);
    };
    add("delta64", gbps(g_s, bytes * 2, min_time), gbps(g_v, bytes * 2, min_time));
  }
  {
    std::vector<int32_t> a((size_t)nvec * 1024);
    for (auto& x : a) x = (int32_t)r.next();
    std::vector<int64_t> b((size_t)nvec * 1024);
    for (auto& x : b) x = (int64_t)r.next();
    auto f_s = [&] {
      int32_t lo, hi;
      for (uint32_t v = 0; v < nvec; ++v) {
        kern::minmax<int32_t>(a.data() + (size_t)v * 1024, 1024, lo, hi);
        g_sink += (uint64_t)lo;
      }
    };
    auto f_v = [&] {
      int32_t lo, hi;
      for (uint32_t v = 0; v < nvec; ++v) {
        simd::minmax32(a.data() + (size_t)v * 1024, 1024, lo, hi);
        g_sink += (uint64_t)lo;
      }
    };
    add("minmax32 (input GB/s)", gbps(f_s, (double)a.size() * 4, min_time), gbps(f_v, (double)a.size() * 4, min_time));
    auto g_s = [&] {
      int64_t lo, hi;
      for (uint32_t v = 0; v < nvec; ++v) {
        kern::minmax<int64_t>(b.data() + (size_t)v * 1024, 1024, lo, hi);
        g_sink += (uint64_t)lo;
      }
    };
    auto g_v = [&] {
      int64_t lo, hi;
      for (uint32_t v = 0; v < nvec; ++v) {
        simd::minmax64(b.data() + (size_t)v * 1024, 1024, lo, hi);
        g_sink += (uint64_t)lo;
      }
    };
    add("minmax64 (input GB/s)", gbps(g_s, (double)b.size() * 8, min_time), gbps(g_v, (double)b.size() * 8, min_time));
  }
  {
    const size_t n = (size_t)nvec * 1024;
    std::vector<int64_t> q(n);
    for (auto& x : q) x = (int64_t)r.below(100000000);
    std::vector<double> buf(n);
    auto f_s = [&] {
      std::memcpy(buf.data(), q.data(), n * 8);
      kern::alp_decode(buf.data(), (uint32_t)n, 4, 2);
    };
    auto f_v = [&] {
      std::memcpy(buf.data(), q.data(), n * 8);
      simd::alp_decode(buf.data(), (uint32_t)n, 4, 2);
    };
    add("alp_decode (incl. 8 B/value copy)", gbps(f_s, (double)n * 8, min_time), gbps(f_v, (double)n * 8, min_time));
  }
  {
    const uint32_t n = 65536;
    std::vector<uint8_t> src((size_t)n * 8), out((size_t)n * 8);
    for (auto& x : src) x = (uint8_t)r.next();
    auto f4s = [&] { kern::unsplit<4>(src.data(), out.data(), n); };
    auto f4v = [&] { simd::unsplit4(src.data(), out.data(), n); };
    add("unsplit4", gbps(f4s, (double)n * 4, min_time), gbps(f4v, (double)n * 4, min_time));
    auto f8s = [&] { kern::unsplit<8>(src.data(), out.data(), n); };
    auto f8v = [&] { simd::unsplit8(src.data(), out.data(), n); };
    add("unsplit8", gbps(f8s, (double)n * 8, min_time), gbps(f8v, (double)n * 8, min_time));
  }
  for (int dens : {2, 50}) {  // percent of rows NA
    const size_t nw = 1024;
    std::vector<uint64_t> w(nw);
    for (auto& x : w) {
      uint64_t m = 0;
      for (int b = 0; b < 64; ++b)
        if ((int)r.below(100) < dens) m |= 1ull << b;
      x = m;
    }
    std::vector<int32_t> a(nw * 64);
    std::vector<uint64_t> b(nw * 64);
    const double nb4 = (double)nw * 64 * 4, nb8 = nb4 * 2;
    auto f_s = [&] { kern::na_bitmap<int32_t>(w.data(), nw, a.data(), (int32_t)0x80000000); };
    auto f_v = [&] { simd::na_bitmap32(w.data(), nw, a.data(), (int32_t)0x80000000); };
    add("na_bitmap32 " + std::to_string(dens) + "% NA", gbps(f_s, nb4, min_time), gbps(f_v, nb4, min_time));
    auto g_s = [&] { kern::na_bitmap<uint64_t>(w.data(), nw, b.data(), 0x7FF00000000007A2ULL); };
    auto g_v = [&] { simd::na_bitmap64(w.data(), nw, b.data(), 0x7FF00000000007A2ULL); };
    add("na_bitmap64 " + std::to_string(dens) + "% NA", gbps(g_s, nb8, min_time), gbps(g_v, nb8, min_time));
  }
  {
    const uint32_t n = 65536, nd = 1000;
    std::vector<int32_t> codes(n), v32(nd), o32(n);
    std::vector<int64_t> v64(nd), o64(n);
    for (auto& c : codes) c = (int32_t)r.below(nd);
    for (auto& x : v32) x = (int32_t)r.next();
    for (auto& x : v64) x = (int64_t)r.next();
    auto f_s = [&] { g_sink += kern::dict_gather<int32_t>(codes.data(), n, v32.data(), nd, o32.data()); };
    auto f_v = [&] { g_sink += simd::dict32(codes.data(), n, v32.data(), nd, o32.data()); };
    add("dict32", gbps(f_s, (double)n * 4, min_time), gbps(f_v, (double)n * 4, min_time));
    auto g_s = [&] { g_sink += kern::dict_gather<int64_t>(codes.data(), n, v64.data(), nd, o64.data()); };
    auto g_v = [&] { g_sink += simd::dict64(codes.data(), n, v64.data(), nd, o64.data()); };
    add("dict64", gbps(g_s, (double)n * 8, min_time), gbps(g_v, (double)n * 8, min_time));
  }
}

}  // namespace ktest
}  // namespace fstx
