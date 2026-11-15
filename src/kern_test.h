// FST-X M4: kernel self-test and micro-benchmark (kern_test.cpp). No R headers. MIT licence.
#ifndef FSTX_KERN_TEST_H
#define FSTX_KERN_TEST_H

#include <cstdint>
#include <string>
#include <vector>

namespace fstx {
namespace ktest {

struct KResult {
  std::string kernel;
  int cases = 0, mismatches = 0;
};
struct KBench {
  std::string kernel;
  double scalar_gbps, simd_gbps;
};

void selftest(uint64_t seed, int iters, std::vector<KResult>& res);
void bench(double min_time, std::vector<KBench>& res);

}  // namespace ktest
}  // namespace fstx

#endif
