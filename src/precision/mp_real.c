#include "mp_real.h"
#include <stdio.h>
#include <math.h>

// =============================================================================
// MPReal Implementation
// =============================================================================

void mp_real_init(MPReal *x, mpfr_prec_t prec) {
    mpfr_init2(x->value, prec);
    x->precision = prec;
}

void mp_real_init_set_str(MPReal *x, const char *str, mpfr_prec_t prec) {
    mpfr_init2(x->value, prec);
    x->precision = prec;
    mpfr_set_str(x->value, str, 10, MPFR_RNDN);
}

void mp_real_init_set_d(MPReal *x, double d, mpfr_prec_t prec) {
    mpfr_init2(x->value, prec);
    x->precision = prec;
    mpfr_set_d(x->value, d, MPFR_RNDN);
}

void mp_real_set_d(MPReal *x, double d) {
    mpfr_set_d(x->value, d, MPFR_RNDN);
}

void mp_real_set_str(MPReal *x, const char *str) {
    mpfr_set_str(x->value, str, 10, MPFR_RNDN);
}

void mp_real_set(MPReal *dst, const MPReal *src) {
    mpfr_set(dst->value, src->value, MPFR_RNDN);
}

double mp_real_get_d(const MPReal *x) {
    return mpfr_get_d(x->value, MPFR_RNDN);
}

void mp_real_add(MPReal *r, const MPReal *a, const MPReal *b) {
    mpfr_add(r->value, a->value, b->value, MPFR_RNDN);
}

void mp_real_add_d(MPReal *r, const MPReal *a, double d) {
    mpfr_add_d(r->value, a->value, d, MPFR_RNDN);
}

void mp_real_sub(MPReal *r, const MPReal *a, const MPReal *b) {
    mpfr_sub(r->value, a->value, b->value, MPFR_RNDN);
}

void mp_real_sub_d(MPReal *r, const MPReal *a, double d) {
    mpfr_sub_d(r->value, a->value, d, MPFR_RNDN);
}

void mp_real_mul(MPReal *r, const MPReal *a, const MPReal *b) {
    mpfr_mul(r->value, a->value, b->value, MPFR_RNDN);
}

void mp_real_mul_d(MPReal *r, const MPReal *a, double d) {
    mpfr_mul_d(r->value, a->value, d, MPFR_RNDN);
}

void mp_real_sqr(MPReal *r, const MPReal *a) {
    mpfr_sqr(r->value, a->value, MPFR_RNDN);
}

void mp_real_neg(MPReal *r, const MPReal *a) {
    mpfr_neg(r->value, a->value, MPFR_RNDN);
}

int mp_real_cmp_d(const MPReal *x, double d) {
    return mpfr_cmp_d(x->value, d);
}

int mp_real_snprintf(char *buf, size_t n, const MPReal *x, int digits) {
    return mpfr_snprintf(buf, n, "%.*Rg", digits, x->value);
}

void mp_real_clear(MPReal *x) {
    mpfr_clear(x->value);
    x->precision = 0;
}

// =============================================================================
// Precision Helpers
// =============================================================================

mpfr_prec_t mp_required_precision(double zoom_level) {
    // At zoom level Z, scale = 2.0 / (height * Z)
    // To represent this precisely, we need log2(Z) + some margin bits
    // Formula: bits = 3.32 * log10(zoom) + 64 (margin)
    // Or: bits = log2(zoom) + 64

    if (zoom_level <= 1.0) {
        return 64;  // Double precision sufficient
    }

    double log_zoom = log2(zoom_level);
    mpfr_prec_t bits = (mpfr_prec_t)(log_zoom + 80);  // 80 bits margin for safety

    // Round up to nice boundaries
    if (bits <= 64) return 64;
    if (bits <= 128) return 128;
    if (bits <= 256) return 256;
    if (bits <= 512) return 512;
    if (bits <= 1024) return 1024;
    if (bits <= 2048) return 2048;
    return 4096;  // Max reasonable precision
}

mpfr_prec_t mp_required_precision_log10(double zoom_log10) {
    if (zoom_log10 <= 0.0) {
        return 64;
    }

    // bits = log2(10^zoom_log10) + margin, rounded up to a multiple of 256.
    // (Power-of-two rounding wasted up to 2x precision — and MPFR multiply
    // cost — right below each boundary; the 80-bit guard already spans far
    // more zoom decades than one 256-bit quantum, so tier reuse is safe.)
    double bits_needed = zoom_log10 * 3.321928094887362 + 80.0;
    if (bits_needed <= 64.0) return 64;
    if (bits_needed > (double)(1 << 20)) return (mpfr_prec_t)(1 << 20);

    mpfr_prec_t bits = ((mpfr_prec_t)bits_needed + 255) & ~(mpfr_prec_t)255;
    if (bits < 128) bits = 128;
    return bits;
}

// =============================================================================
// Extended-Exponent Bridges
// =============================================================================

void mp_real_add_fx(MPReal *r, const MPReal *a, double fx_m, int64_t fx_e) {
    if (fx_m == 0.0) {
        mpfr_set(r->value, a->value, MPFR_RNDN);
        return;
    }
    MPReal t;
    mp_real_init(&t, r->precision);
    mpfr_set_d(t.value, fx_m, MPFR_RNDN);
    mpfr_mul_2si(t.value, t.value, (long)fx_e, MPFR_RNDN);
    mpfr_add(r->value, a->value, t.value, MPFR_RNDN);
    mp_real_clear(&t);
}

void mp_real_get_fx(const MPReal *x, double *fx_m, int64_t *fx_e) {
    if (mpfr_zero_p(x->value)) {
        *fx_m = 0.0;
        *fx_e = 0;
        return;
    }
    long e = 0;
    // Returns d in +-[0.5, 1) with value = d * 2^e
    double d = mpfr_get_d_2exp(&e, x->value, MPFR_RNDN);
    *fx_m = d;
    *fx_e = (int64_t)e;
}
