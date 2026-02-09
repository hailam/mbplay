#ifndef MB_SIMD_H
#define MB_SIMD_H

#include "../config.h"

// =============================================================================
// Platform-agnostic SIMD Abstraction
// =============================================================================
//
// This header includes the appropriate platform-specific SIMD implementation
// based on compile-time platform detection in config.h.
//
// All SIMD wrappers are static inline for zero overhead.
//

#if defined(MB_SIMD_NEON)
#include "simd_neon.h"
#elif defined(MB_SIMD_AVX2)
#include "simd_avx.h"
#elif defined(MB_SIMD_SSE2)
#include "simd_sse.h"
#endif

#endif // MB_SIMD_H
