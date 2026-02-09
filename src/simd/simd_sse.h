#ifndef MB_SIMD_SSE_H
#define MB_SIMD_SSE_H

// =============================================================================
// x86 SSE2 SIMD Abstraction (stub for future implementation)
// =============================================================================

#ifdef __SSE2__
#include <emmintrin.h>

typedef __m128d simd_f64x2;

// TODO: Implement SSE2 wrappers when x86 support is needed
#endif

#endif // MB_SIMD_SSE_H
