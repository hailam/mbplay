#ifndef MB_FLOATEXP_H
#define MB_FLOATEXP_H

#include <math.h>
#include <stdint.h>
#include <stdbool.h>

// =============================================================================
// Extended-Exponent Floating Point ("floatexp")
// =============================================================================
//
// value = m * 2^e with m in +-[0.5, 1) (frexp normalization) or m == 0.
//
// A double's exponent range dies at ~1e+-308, which is what capped the zoom
// level: scale = (2/height)/zoom and the per-pixel deltas underflow past
// zoom ~1e304. FloatExp keeps a double mantissa (52-bit precision — plenty:
// deltas only need ~16 significant digits) with a 64-bit exponent, giving an
// effective range of ~10^(+-2.7e18). All operations are branch-light inline
// functions; used in the perturbation inner loops, so keep them lean.

typedef struct {
    double m;    // mantissa in +-[0.5, 1), or 0.0
    int64_t e;   // binary exponent
} FloatExp;

static inline FloatExp fx_zero(void) {
    FloatExp r = {0.0, 0};
    return r;
}

static inline bool fx_is_zero(FloatExp a) {
    return a.m == 0.0;
}

// Renormalize after an operation left m outside +-[0.5, 1).
static inline FloatExp fx_norm(double m, int64_t e) {
    if (m == 0.0) return fx_zero();
    int ex;
    m = frexp(m, &ex);
    FloatExp r = {m, e + ex};
    return r;
}

static inline FloatExp fx_from_d(double d) {
    return fx_norm(d, 0);
}

// 10^l10 as a FloatExp (l10 may be far outside double range).
static inline FloatExp fx_from_log10(double l10) {
    double e2 = l10 * 3.321928094887362;  // log2(10)
    double ei = floor(e2);
    double m = exp2(e2 - ei);             // in [1, 2)
    return fx_norm(m, (int64_t)ei);
}

// Convert to double; saturates to +-DBL_MAX on overflow, flushes to 0 on
// underflow (including the subnormal range — callers treat that as "too
// small to matter in double math").
static inline double fx_to_d(FloatExp a) {
    if (a.m == 0.0) return 0.0;
    if (a.e > 1023) return a.m > 0 ? 1.7976931348623157e308 : -1.7976931348623157e308;
    if (a.e < -1021) return 0.0;
    return ldexp(a.m, (int)a.e);
}

static inline FloatExp fx_neg(FloatExp a) {
    FloatExp r = {-a.m, a.e};
    return r;
}

static inline FloatExp fx_mul(FloatExp a, FloatExp b) {
    if (a.m == 0.0 || b.m == 0.0) return fx_zero();
    // m in +-[0.25, 1): renormalization adjusts by at most 1
    return fx_norm(a.m * b.m, a.e + b.e);
}

static inline FloatExp fx_mul_d(FloatExp a, double d) {
    return fx_mul(a, fx_from_d(d));
}

static inline FloatExp fx_add(FloatExp a, FloatExp b) {
    if (a.m == 0.0) return b;
    if (b.m == 0.0) return a;
    int64_t diff = a.e - b.e;
    if (diff > 60) return a;      // b is below a's precision
    if (diff < -60) return b;
    // Align b to a's exponent; |diff| <= 60 so ldexp is exact
    double m = a.m + ldexp(b.m, (int)(-diff));
    return fx_norm(m, a.e);
}

static inline FloatExp fx_sub(FloatExp a, FloatExp b) {
    return fx_add(a, fx_neg(b));
}

static inline FloatExp fx_div(FloatExp a, FloatExp b) {
    if (a.m == 0.0) return fx_zero();
    // b must be nonzero; m in +-(0.5, 2)
    return fx_norm(a.m / b.m, a.e - b.e);
}

// Compare |a| vs |b|: negative, zero, positive like memcmp.
static inline int fx_cmp_abs(FloatExp a, FloatExp b) {
    if (a.m == 0.0) return b.m == 0.0 ? 0 : -1;
    if (b.m == 0.0) return 1;
    if (a.e != b.e) return a.e > b.e ? 1 : -1;
    double fa = fabs(a.m), fb = fabs(b.m);
    if (fa == fb) return 0;
    return fa > fb ? 1 : -1;
}

// value(a) >= d, for non-negative d (used for escape/threshold checks)
static inline bool fx_ge_d(FloatExp a, double d) {
    if (a.m < 0.0) return false;
    if (a.m == 0.0) return d <= 0.0;
    if (d <= 0.0) return true;
    return fx_cmp_abs(a, fx_from_d(d)) >= 0;
}

// 2^k * a
static inline FloatExp fx_scale2(FloatExp a, int64_t k) {
    if (a.m == 0.0) return a;
    FloatExp r = {a.m, a.e + k};
    return r;
}

// |a|
static inline FloatExp fx_abs(FloatExp a) {
    FloatExp r = {fabs(a.m), a.e};
    return r;
}

// log10(|a|); -INFINITY for zero (for display / budget decisions only)
static inline double fx_log10(FloatExp a) {
    if (a.m == 0.0) return -INFINITY;
    return log10(fabs(a.m)) + (double)a.e * 0.3010299956639812;  // log10(2)
}

// =============================================================================
// Complex helpers for the perturbation recurrence
// =============================================================================

typedef struct {
    FloatExp re, im;
} FloatExpC;

static inline FloatExpC fxc_zero(void) {
    FloatExpC r = {fx_zero(), fx_zero()};
    return r;
}

static inline FloatExpC fxc_from_d(double re, double im) {
    FloatExpC r = {fx_from_d(re), fx_from_d(im)};
    return r;
}

static inline FloatExpC fxc_add(FloatExpC a, FloatExpC b) {
    FloatExpC r = {fx_add(a.re, b.re), fx_add(a.im, b.im)};
    return r;
}

// (a.re + i a.im) * (b.re + i b.im)
static inline FloatExpC fxc_mul(FloatExpC a, FloatExpC b) {
    FloatExpC r = {
        fx_sub(fx_mul(a.re, b.re), fx_mul(a.im, b.im)),
        fx_add(fx_mul(a.re, b.im), fx_mul(a.im, b.re)),
    };
    return r;
}

// |a|^2 as FloatExp
static inline FloatExp fxc_norm_sqr(FloatExpC a) {
    return fx_add(fx_mul(a.re, a.re), fx_mul(a.im, a.im));
}

#endif // MB_FLOATEXP_H
