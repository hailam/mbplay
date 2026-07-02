#include "view_hp.h"
#include "mp_real.h"
#include "floatexp.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

int mb_view_hp_digits(double zoom_level) {
    // Legacy entry point (zoom as a saturating double); prefer the log10
    // variant for deep zoom.
    double l10 = zoom_level > 1.0 ? log10(zoom_level) : 0.0;
    return mb_view_hp_digits_log10(l10);
}

int mb_view_hp_digits_log10(double zoom_log10) {
    // Pixels at zoom 10^N have size ~2/(height*10^N); addressing one takes
    // N + ~4 digits. Add guard digits for accumulated round trips.
    int digits = 17;
    if (zoom_log10 > 0.0) {
        digits = (int)ceil(zoom_log10) + 12;
        if (digits < 17) digits = 17;
    }
    // Leave room for sign, decimal point and exponent in the fixed buffer.
    if (digits > MB_HP_COORD_STR_LEN - 16) digits = MB_HP_COORD_STR_LEN - 16;
    return digits;
}

void mb_view_hp_sync_from_doubles(MBViewState *view) {
    snprintf(view->center_x_str, MB_HP_COORD_STR_LEN, "%.17g", view->center_x);
    snprintf(view->center_y_str, MB_HP_COORD_STR_LEN, "%.17g", view->center_y);
    view->high_precision_mode = mb_view_needs_high_precision(view);
}

// Count significant decimal digits in a coordinate string (skips sign,
// decimal point, exponent, and leading zeros; keeps trailing zeros since
// they were emitted as significant).
static int count_sig_digits(const char *s) {
    int n = 0;
    int seen_nonzero = 0;
    for (; *s && *s != 'e' && *s != 'E'; s++) {
        if (*s >= '0' && *s <= '9') {
            if (*s != '0') seen_nonzero = 1;
            if (seen_nonzero) n++;
        }
    }
    return n;
}

// Digits to keep when re-serializing the center: never fewer than the
// current strings already carry, so a target pasted at higher precision
// than the current zoom survives panning/zooming until the zoom catches up.
static int preserve_digits(const MBViewState *view) {
    int digits = mb_view_hp_digits_log10(mb_view_zoom_log10(view));
    int have = count_sig_digits(view->center_x_str);
    int have_y = count_sig_digits(view->center_y_str);
    if (have_y > have) have = have_y;
    if (have > digits) digits = have;
    if (digits > MB_HP_COORD_STR_LEN - 16) digits = MB_HP_COORD_STR_LEN - 16;
    return digits;
}

static mpfr_prec_t prec_for(const MBViewState *view, int digits) {
    mpfr_prec_t prec = (mpfr_prec_t)(digits * 3.33) + 64;
    mpfr_prec_t zoom_prec = mp_required_precision_log10(mb_view_zoom_log10(view)) + 64;
    if (zoom_prec > prec) prec = zoom_prec;
    return prec;
}

int mb_view_hp_set_center(MBViewState *view, const char *re_str, const char *im_str) {
    if (!re_str || !im_str) return -1;

    // Validate: strtod accepts the same decimal/scientific forms MPFR does.
    char *end = NULL;
    double re_d = strtod(re_str, &end);
    if (end == re_str) return -1;
    double im_d = strtod(im_str, &end);
    if (end == im_str) return -1;
    if (isnan(re_d) || isnan(im_d) || isinf(re_d) || isinf(im_d)) return -1;

    // Never drop digits the caller provided: a user may paste a deep target
    // while still zoomed out and only zoom in afterwards.
    int digits = mb_view_hp_digits_log10(mb_view_zoom_log10(view));
    int in_len = (int)strlen(re_str);
    int im_len = (int)strlen(im_str);
    if (im_len > in_len) in_len = im_len;
    if (in_len > digits) digits = in_len;
    if (digits > MB_HP_COORD_STR_LEN - 16) digits = MB_HP_COORD_STR_LEN - 16;

    mpfr_prec_t prec = prec_for(view, digits);

    MPReal c;
    mp_real_init_set_str(&c, re_str, prec);
    mp_real_snprintf(view->center_x_str, MB_HP_COORD_STR_LEN, &c, digits);
    view->center_x = mp_real_get_d(&c);
    mp_real_clear(&c);

    mp_real_init_set_str(&c, im_str, prec);
    mp_real_snprintf(view->center_y_str, MB_HP_COORD_STR_LEN, &c, digits);
    view->center_y = mp_real_get_d(&c);
    mp_real_clear(&c);

    view->high_precision_mode = mb_view_needs_high_precision(view);
    return 0;
}

void mb_view_hp_translate_fx(MBViewState *view, FloatExp d_re, FloatExp d_im) {
    int digits = preserve_digits(view);
    mpfr_prec_t prec = prec_for(view, digits);

    MPReal c;
    mp_real_init_set_str(&c, view->center_x_str, prec);
    if (!fx_is_zero(d_re)) mp_real_add_fx(&c, &c, d_re.m, d_re.e);
    mp_real_snprintf(view->center_x_str, MB_HP_COORD_STR_LEN, &c, digits);
    view->center_x = mp_real_get_d(&c);
    mp_real_clear(&c);

    mp_real_init_set_str(&c, view->center_y_str, prec);
    if (!fx_is_zero(d_im)) mp_real_add_fx(&c, &c, d_im.m, d_im.e);
    mp_real_snprintf(view->center_y_str, MB_HP_COORD_STR_LEN, &c, digits);
    view->center_y = mp_real_get_d(&c);
    mp_real_clear(&c);

    view->high_precision_mode = mb_view_needs_high_precision(view);
}

void mb_view_hp_translate(MBViewState *view, double d_re, double d_im) {
    mb_view_hp_translate_fx(view, fx_from_d(d_re), fx_from_d(d_im));
}

void mb_view_hp_center_delta_fx(const MBViewState *from, const MBViewState *to,
                                FloatExp *d_re, FloatExp *d_im) {
    double l10 = mb_view_zoom_log10(from);
    double l10_to = mb_view_zoom_log10(to);
    if (l10_to > l10) l10 = l10_to;
    mpfr_prec_t prec = mp_required_precision_log10(l10) + 64;

    MPReal a, b, d;
    mp_real_init_set_str(&a, to->center_x_str, prec);
    mp_real_init_set_str(&b, from->center_x_str, prec);
    mp_real_init(&d, prec);
    mp_real_sub(&d, &a, &b);
    mp_real_get_fx(&d, &d_re->m, &d_re->e);

    mp_real_set_str(&a, to->center_y_str);
    mp_real_set_str(&b, from->center_y_str);
    mp_real_sub(&d, &a, &b);
    mp_real_get_fx(&d, &d_im->m, &d_im->e);

    mp_real_clear(&a);
    mp_real_clear(&b);
    mp_real_clear(&d);
}

void mb_view_hp_center_delta(const MBViewState *from, const MBViewState *to,
                             double *d_re, double *d_im) {
    FloatExp fre, fim;
    mb_view_hp_center_delta_fx(from, to, &fre, &fim);
    *d_re = fx_to_d(fre);
    *d_im = fx_to_d(fim);
}

void mb_view_hp_zoom_towards(MBViewState *view, double off_x_px, double off_y_up_px,
                             double factor) {
    if (factor <= 0.0) return;

    FloatExp old_scale = mb_view_get_scale_fx(view);

    mb_view_set_zoom_log10(view, mb_view_zoom_log10(view) + log10(factor));

    FloatExp new_scale = mb_view_get_scale_fx(view);

    // The point under the cursor is center + off*old_scale; keeping it fixed
    // means center' = center + off*(old_scale - new_scale). Computed in
    // extended-exponent arithmetic so it stays exact at any zoom depth.
    FloatExp diff = fx_sub(old_scale, new_scale);
    FloatExp d_re = fx_mul_d(diff, off_x_px);
    FloatExp d_im = fx_mul_d(diff, off_y_up_px);

    mb_view_hp_translate_fx(view, d_re, d_im);
}
