#ifndef MB_MP_COMPLEX_H
#define MB_MP_COMPLEX_H

#include "mp_real.h"
#include <stdbool.h>

// =============================================================================
// Arbitrary-Precision Complex Numbers
// =============================================================================

typedef struct {
    MPReal re;  // Real part
    MPReal im;  // Imaginary part
} MPComplex;

/**
 * Initialize a complex number with given precision.
 * @param z The complex to initialize
 * @param prec Precision in bits
 */
void mp_complex_init(MPComplex *z, mpfr_prec_t prec);

/**
 * Initialize from two decimal strings.
 * @param z The complex to initialize
 * @param re_str Real part as decimal string
 * @param im_str Imaginary part as decimal string
 * @param prec Precision in bits
 */
void mp_complex_init_set_str(MPComplex *z, const char *re_str, const char *im_str,
                              mpfr_prec_t prec);

/**
 * Initialize from two doubles.
 * @param z The complex to initialize
 * @param re Real part
 * @param im Imaginary part
 * @param prec Precision in bits
 */
void mp_complex_init_set_d(MPComplex *z, double re, double im, mpfr_prec_t prec);

/**
 * Set from two doubles (already initialized).
 * @param z The complex to set
 * @param re Real part
 * @param im Imaginary part
 */
void mp_complex_set_d(MPComplex *z, double re, double im);

/**
 * Copy from another complex.
 * @param dst Destination (must be initialized)
 * @param src Source
 */
void mp_complex_set(MPComplex *dst, const MPComplex *src);

/**
 * Get real part as double.
 */
double mp_complex_get_re_d(const MPComplex *z);

/**
 * Get imaginary part as double.
 */
double mp_complex_get_im_d(const MPComplex *z);

/**
 * Add: r = a + b
 */
void mp_complex_add(MPComplex *r, const MPComplex *a, const MPComplex *b);

/**
 * Subtract: r = a - b
 */
void mp_complex_sub(MPComplex *r, const MPComplex *a, const MPComplex *b);

/**
 * Multiply: r = a * b
 * (a.re + a.im*i) * (b.re + b.im*i)
 * = (a.re*b.re - a.im*b.im) + (a.re*b.im + a.im*b.re)*i
 */
void mp_complex_mul(MPComplex *r, const MPComplex *a, const MPComplex *b);

/**
 * Square: r = z^2
 * (re + im*i)^2 = (re^2 - im^2) + (2*re*im)*i
 */
void mp_complex_sqr(MPComplex *r, const MPComplex *z);

/**
 * Get squared magnitude: |z|^2 = re^2 + im^2
 * Result stored in r (MPReal)
 */
void mp_complex_norm_sqr(MPReal *r, const MPComplex *z);

/**
 * Get squared magnitude as double.
 */
double mp_complex_norm_sqr_d(const MPComplex *z);

/**
 * Clear/free memory.
 */
void mp_complex_clear(MPComplex *z);

// =============================================================================
// Mandelbrot-specific Operations
// =============================================================================

/**
 * Mandelbrot iteration: z_new = z^2 + c
 * @param z_new Result (must be initialized)
 * @param z Current Z value
 * @param c The C constant
 */
void mp_complex_mandelbrot_iter(MPComplex *z_new, const MPComplex *z, const MPComplex *c);

/**
 * Check if |z|^2 >= escape_radius^2
 * @param z Complex number
 * @param escape_radius_sqr Squared escape radius (typically 4.0)
 * @return true if escaped
 */
bool mp_complex_escaped(const MPComplex *z, double escape_radius_sqr);

#endif // MB_MP_COMPLEX_H
