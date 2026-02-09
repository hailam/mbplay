#ifndef MB_SIMD_NEON_H
#define MB_SIMD_NEON_H

#include <arm_neon.h>

// =============================================================================
// ARM NEON SIMD Abstraction (64-bit float, 2 lanes)
// =============================================================================

typedef float64x2_t simd_f64x2;

// Load/Store operations
static inline simd_f64x2 simd_set_f64(double a, double b) {
  simd_f64x2 result = {a, b};
  return result;
}

static inline simd_f64x2 simd_splat_f64(double val) { return vdupq_n_f64(val); }

static inline void simd_store_f64(double *dest, simd_f64x2 vec) {
  vst1q_f64(dest, vec);
}

// Arithmetic operations
static inline simd_f64x2 simd_add_f64(simd_f64x2 a, simd_f64x2 b) {
  return vaddq_f64(a, b);
}

static inline simd_f64x2 simd_sub_f64(simd_f64x2 a, simd_f64x2 b) {
  return vsubq_f64(a, b);
}

static inline simd_f64x2 simd_mul_f64(simd_f64x2 a, simd_f64x2 b) {
  return vmulq_f64(a, b);
}

// Fused multiply-add: a + b * c
static inline simd_f64x2 simd_fma_f64(simd_f64x2 a, simd_f64x2 b,
                                      simd_f64x2 c) {
  return vfmaq_f64(a, b, c);
}

#endif // MB_SIMD_NEON_H
