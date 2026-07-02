#ifndef MB_MP_REAL_H
#define MB_MP_REAL_H

#include <mpfr.h>
#include <stddef.h>
#include <stdint.h>

// =============================================================================
// MPFR Wrapper for Arbitrary-Precision Real Numbers
// =============================================================================

typedef struct {
    mpfr_t value;
    mpfr_prec_t precision;
} MPReal;

/**
 * Initialize an MPReal with given precision.
 * @param x The MPReal to initialize
 * @param prec Precision in bits
 */
void mp_real_init(MPReal *x, mpfr_prec_t prec);

/**
 * Initialize and set from a decimal string.
 * @param x The MPReal to initialize
 * @param str Decimal string representation (e.g., "-1.23456789012345678901234567890")
 * @param prec Precision in bits
 */
void mp_real_init_set_str(MPReal *x, const char *str, mpfr_prec_t prec);

/**
 * Initialize and set from a double.
 * @param x The MPReal to initialize
 * @param d The double value
 * @param prec Precision in bits
 */
void mp_real_init_set_d(MPReal *x, double d, mpfr_prec_t prec);

/**
 * Set from a double (already initialized).
 * @param x The MPReal to set
 * @param d The double value
 */
void mp_real_set_d(MPReal *x, double d);

/**
 * Set from a decimal string (already initialized).
 * @param x The MPReal to set
 * @param str Decimal string representation
 */
void mp_real_set_str(MPReal *x, const char *str);

/**
 * Copy from another MPReal.
 * @param dst Destination (must be initialized)
 * @param src Source MPReal
 */
void mp_real_set(MPReal *dst, const MPReal *src);

/**
 * Get as double (may lose precision).
 * @param x The MPReal
 * @return Double approximation
 */
double mp_real_get_d(const MPReal *x);

/**
 * Add: r = a + b
 * @param r Result (must be initialized)
 * @param a First operand
 * @param b Second operand
 */
void mp_real_add(MPReal *r, const MPReal *a, const MPReal *b);

/**
 * Add double: r = a + d
 * @param r Result (must be initialized)
 * @param a First operand
 * @param d Double to add
 */
void mp_real_add_d(MPReal *r, const MPReal *a, double d);

/**
 * Subtract: r = a - b
 * @param r Result (must be initialized)
 * @param a First operand
 * @param b Second operand
 */
void mp_real_sub(MPReal *r, const MPReal *a, const MPReal *b);

/**
 * Subtract double: r = a - d
 * @param r Result (must be initialized)
 * @param a First operand
 * @param d Double to subtract
 */
void mp_real_sub_d(MPReal *r, const MPReal *a, double d);

/**
 * Multiply: r = a * b
 * @param r Result (must be initialized)
 * @param a First operand
 * @param b Second operand
 */
void mp_real_mul(MPReal *r, const MPReal *a, const MPReal *b);

/**
 * Multiply by double: r = a * d
 * @param r Result (must be initialized)
 * @param a First operand
 * @param d Double multiplier
 */
void mp_real_mul_d(MPReal *r, const MPReal *a, double d);

/**
 * Square: r = a^2
 * @param r Result (must be initialized)
 * @param a Operand
 */
void mp_real_sqr(MPReal *r, const MPReal *a);

/**
 * Negate: r = -a
 * @param r Result (must be initialized)
 * @param a Operand
 */
void mp_real_neg(MPReal *r, const MPReal *a);

/**
 * Compare to double.
 * @param x The MPReal
 * @param d Double to compare
 * @return negative if x < d, 0 if x == d, positive if x > d
 */
int mp_real_cmp_d(const MPReal *x, double d);

/**
 * Format as decimal string.
 * @param buf Output buffer
 * @param n Buffer size
 * @param x The MPReal
 * @param digits Number of significant digits
 * @return Number of characters written (not including null terminator)
 */
int mp_real_snprintf(char *buf, size_t n, const MPReal *x, int digits);

/**
 * Add an extended-exponent value: r = a + (m * 2^e).
 * Exact regardless of exponent (unlike mp_real_add_d, which cannot receive
 * values outside double range).
 */
void mp_real_add_fx(MPReal *r, const MPReal *a, double fx_m, int64_t fx_e);

/**
 * Get as extended-exponent value: *fx_m in +-[0.5,1) (or 0), value = m*2^e.
 * Never overflows/underflows, unlike mp_real_get_d.
 */
void mp_real_get_fx(const MPReal *x, double *fx_m, int64_t *fx_e);

/**
 * Clear/free memory used by MPReal.
 * @param x The MPReal to clear
 */
void mp_real_clear(MPReal *x);

// =============================================================================
// Precision Helpers
// =============================================================================

/**
 * Calculate required precision for a given zoom level.
 * Based on scale = 2.0 / (viewport_height * zoom)
 * At zoom 10^N, we need approximately 3.32 * N bits of precision.
 * @param zoom_level The zoom level
 * @return Required precision in bits (minimum 64)
 */
mpfr_prec_t mp_required_precision(double zoom_level);

/**
 * Same, from log10 of the zoom level — usable beyond double range
 * (zoom 10^4000 needs ~13k bits; a double cannot hold 10^4000 itself).
 */
mpfr_prec_t mp_required_precision_log10(double zoom_log10);

#endif // MB_MP_REAL_H
