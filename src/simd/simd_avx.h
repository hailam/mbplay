#ifndef MB_SIMD_AVX_H
#define MB_SIMD_AVX_H

// =============================================================================
// x86 AVX SIMD Abstraction (stub for future implementation)
// =============================================================================

#ifdef __AVX2__
#include <immintrin.h>

typedef __m256d simd_f64x4;

// TODO: Implement AVX wrappers when x86 support is needed
#endif

#endif // MB_SIMD_AVX_H
