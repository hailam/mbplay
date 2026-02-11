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
