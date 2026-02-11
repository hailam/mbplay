#include "mp_complex.h"

// =============================================================================
// MPComplex Implementation
// =============================================================================

void mp_complex_init(MPComplex *z, mpfr_prec_t prec) {
    mp_real_init(&z->re, prec);
    mp_real_init(&z->im, prec);
}

void mp_complex_init_set_str(MPComplex *z, const char *re_str, const char *im_str,
                              mpfr_prec_t prec) {
    mp_real_init_set_str(&z->re, re_str, prec);
    mp_real_init_set_str(&z->im, im_str, prec);
}

void mp_complex_init_set_d(MPComplex *z, double re, double im, mpfr_prec_t prec) {
    mp_real_init_set_d(&z->re, re, prec);
    mp_real_init_set_d(&z->im, im, prec);
}

void mp_complex_set_d(MPComplex *z, double re, double im) {
    mp_real_set_d(&z->re, re);
    mp_real_set_d(&z->im, im);
}

void mp_complex_set(MPComplex *dst, const MPComplex *src) {
    mp_real_set(&dst->re, &src->re);
    mp_real_set(&dst->im, &src->im);
}

double mp_complex_get_re_d(const MPComplex *z) {
    return mp_real_get_d(&z->re);
}

double mp_complex_get_im_d(const MPComplex *z) {
    return mp_real_get_d(&z->im);
}

void mp_complex_add(MPComplex *r, const MPComplex *a, const MPComplex *b) {
    mp_real_add(&r->re, &a->re, &b->re);
    mp_real_add(&r->im, &a->im, &b->im);
}

void mp_complex_sub(MPComplex *r, const MPComplex *a, const MPComplex *b) {
    mp_real_sub(&r->re, &a->re, &b->re);
    mp_real_sub(&r->im, &a->im, &b->im);
}

void mp_complex_mul(MPComplex *r, const MPComplex *a, const MPComplex *b) {
    // (a.re + a.im*i) * (b.re + b.im*i)
    // = (a.re*b.re - a.im*b.im) + (a.re*b.im + a.im*b.re)*i

    // Need temporaries to handle case where r aliases a or b
    MPReal tmp_re, tmp_im, t1, t2;
    mpfr_prec_t prec = a->re.precision;

    mp_real_init(&tmp_re, prec);
    mp_real_init(&tmp_im, prec);
    mp_real_init(&t1, prec);
    mp_real_init(&t2, prec);

    // tmp_re = a.re * b.re - a.im * b.im
    mp_real_mul(&t1, &a->re, &b->re);
    mp_real_mul(&t2, &a->im, &b->im);
    mp_real_sub(&tmp_re, &t1, &t2);

    // tmp_im = a.re * b.im + a.im * b.re
    mp_real_mul(&t1, &a->re, &b->im);
    mp_real_mul(&t2, &a->im, &b->re);
    mp_real_add(&tmp_im, &t1, &t2);

    mp_real_set(&r->re, &tmp_re);
    mp_real_set(&r->im, &tmp_im);

    mp_real_clear(&tmp_re);
    mp_real_clear(&tmp_im);
    mp_real_clear(&t1);
    mp_real_clear(&t2);
}

void mp_complex_sqr(MPComplex *r, const MPComplex *z) {
    // (re + im*i)^2 = (re^2 - im^2) + (2*re*im)*i

    MPReal tmp_re, tmp_im, re_sqr, im_sqr;
    mpfr_prec_t prec = z->re.precision;

    mp_real_init(&tmp_re, prec);
    mp_real_init(&tmp_im, prec);
    mp_real_init(&re_sqr, prec);
    mp_real_init(&im_sqr, prec);

    // re_sqr = re^2
    mp_real_sqr(&re_sqr, &z->re);

    // im_sqr = im^2
    mp_real_sqr(&im_sqr, &z->im);

    // tmp_re = re^2 - im^2
    mp_real_sub(&tmp_re, &re_sqr, &im_sqr);

    // tmp_im = 2 * re * im
    mp_real_mul(&tmp_im, &z->re, &z->im);
    mp_real_add(&tmp_im, &tmp_im, &tmp_im);  // tmp_im *= 2

    mp_real_set(&r->re, &tmp_re);
    mp_real_set(&r->im, &tmp_im);

    mp_real_clear(&tmp_re);
    mp_real_clear(&tmp_im);
    mp_real_clear(&re_sqr);
    mp_real_clear(&im_sqr);
}

void mp_complex_norm_sqr(MPReal *r, const MPComplex *z) {
    // |z|^2 = re^2 + im^2
    MPReal re_sqr, im_sqr;
    mpfr_prec_t prec = z->re.precision;

    mp_real_init(&re_sqr, prec);
    mp_real_init(&im_sqr, prec);

    mp_real_sqr(&re_sqr, &z->re);
    mp_real_sqr(&im_sqr, &z->im);
    mp_real_add(r, &re_sqr, &im_sqr);

    mp_real_clear(&re_sqr);
    mp_real_clear(&im_sqr);
}

double mp_complex_norm_sqr_d(const MPComplex *z) {
    double re = mp_real_get_d(&z->re);
    double im = mp_real_get_d(&z->im);
    return re * re + im * im;
}

void mp_complex_clear(MPComplex *z) {
    mp_real_clear(&z->re);
    mp_real_clear(&z->im);
}

// =============================================================================
// Mandelbrot-specific Operations
// =============================================================================

void mp_complex_mandelbrot_iter(MPComplex *z_new, const MPComplex *z, const MPComplex *c) {
    // z_new = z^2 + c
    mp_complex_sqr(z_new, z);
    mp_complex_add(z_new, z_new, c);
}

bool mp_complex_escaped(const MPComplex *z, double escape_radius_sqr) {
    double norm_sqr = mp_complex_norm_sqr_d(z);
    return norm_sqr >= escape_radius_sqr;
}
