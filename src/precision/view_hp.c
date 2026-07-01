#include "view_hp.h"
#include "mp_real.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

int mb_view_hp_digits(double zoom_level) {
    // Pixels at zoom Z have size ~2/(height*Z); addressing one takes
    // log10(Z) + ~4 digits. Add guard digits for accumulated round trips.
    int digits = 17;
    if (zoom_level > 1.0) {
        digits = (int)ceil(log10(zoom_level)) + 12;
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

int mb_view_hp_set_center(MBViewState *view, const char *re_str, const char *im_str) {
    if (!re_str || !im_str) return -1;

    // Validate: strtod accepts the same decimal/scientific forms MPFR does.
    char *end = NULL;
    double re_d = strtod(re_str, &end);
    if (end == re_str) return -1;
    double im_d = strtod(im_str, &end);
    if (end == im_str) return -1;
    if (isnan(re_d) || isnan(im_d) || isinf(re_d) || isinf(im_d)) return -1;

    // Never drop digits the caller provided: a user may paste a 100-digit
    // target while still zoomed out and only zoom in afterwards.
    int digits = mb_view_hp_digits(view->zoom_level);
    int in_len = (int)strlen(re_str);
    int im_len = (int)strlen(im_str);
    if (im_len > in_len) in_len = im_len;
    if (in_len > digits) digits = in_len;
    if (digits > MB_HP_COORD_STR_LEN - 16) digits = MB_HP_COORD_STR_LEN - 16;

    mpfr_prec_t prec = (mpfr_prec_t)(digits * 3.33) + 64;
    {
        mpfr_prec_t zoom_prec = mp_required_precision(view->zoom_level) + 64;
        if (zoom_prec > prec) prec = zoom_prec;
    }

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
    int digits = mb_view_hp_digits(view->zoom_level);
    int have = count_sig_digits(view->center_x_str);
    int have_y = count_sig_digits(view->center_y_str);
    if (have_y > have) have = have_y;
    if (have > digits) digits = have;
    if (digits > MB_HP_COORD_STR_LEN - 16) digits = MB_HP_COORD_STR_LEN - 16;
    return digits;
}

void mb_view_hp_translate(MBViewState *view, double d_re, double d_im) {
    int digits = preserve_digits(view);
    mpfr_prec_t prec = (mpfr_prec_t)(digits * 3.33) + 64;
    {
        mpfr_prec_t zoom_prec = mp_required_precision(view->zoom_level) + 64;
        if (zoom_prec > prec) prec = zoom_prec;
    }

    MPReal c;
    mp_real_init_set_str(&c, view->center_x_str, prec);
    if (d_re != 0.0) mp_real_add_d(&c, &c, d_re);
    mp_real_snprintf(view->center_x_str, MB_HP_COORD_STR_LEN, &c, digits);
    view->center_x = mp_real_get_d(&c);
    mp_real_clear(&c);

    mp_real_init_set_str(&c, view->center_y_str, prec);
    if (d_im != 0.0) mp_real_add_d(&c, &c, d_im);
    mp_real_snprintf(view->center_y_str, MB_HP_COORD_STR_LEN, &c, digits);
    view->center_y = mp_real_get_d(&c);
    mp_real_clear(&c);

    view->high_precision_mode = mb_view_needs_high_precision(view);
}

void mb_view_hp_center_delta(const MBViewState *from, const MBViewState *to,
                             double *d_re, double *d_im) {
    double zoom = from->zoom_level > to->zoom_level ? from->zoom_level : to->zoom_level;
    mpfr_prec_t prec = mp_required_precision(zoom) + 64;

    MPReal a, b, d;
    mp_real_init_set_str(&a, to->center_x_str, prec);
    mp_real_init_set_str(&b, from->center_x_str, prec);
    mp_real_init(&d, prec);
    mp_real_sub(&d, &a, &b);
    *d_re = mp_real_get_d(&d);

    mp_real_set_str(&a, to->center_y_str);
    mp_real_set_str(&b, from->center_y_str);
    mp_real_sub(&d, &a, &b);
    *d_im = mp_real_get_d(&d);

    mp_real_clear(&a);
    mp_real_clear(&b);
    mp_real_clear(&d);
}

void mb_view_hp_zoom_towards(MBViewState *view, double off_x_px, double off_y_up_px,
                             double factor) {
    double old_scale = mb_view_get_scale(view);

    view->zoom_level = mb_clamp_zoom(view->zoom_level * factor);

    double new_scale = mb_view_get_scale(view);

    // The point under the cursor is center + off*old_scale; keeping it fixed
    // means center' = center + off*(old_scale - new_scale). The move is a
    // small relative offset, so computing it in double is exact enough at any
    // zoom depth (both scales are normal doubles by the MB_ZOOM_MAX budget).
    double d_re = off_x_px * (old_scale - new_scale);
    double d_im = off_y_up_px * (old_scale - new_scale);

    mb_view_hp_translate(view, d_re, d_im);
}
