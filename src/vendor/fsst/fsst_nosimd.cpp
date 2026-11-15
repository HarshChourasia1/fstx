// FST-X: replaces upstream fsst_avx512.cpp. FST-X builds FSST without SIMD intrinsics (they come in
// milestone M4), so the AVX-512 bulk compressor is never used: the runtime check always says "no".
#include "libfsst.hpp"
namespace libfsst {
bool fsst_hasAVX512() { return false; }
size_t fsst_compressAVX512(SymbolTable&, u8*, u8*, SIMDjob*, SIMDjob*, size_t, size_t) { return 0; }
}  // namespace libfsst
