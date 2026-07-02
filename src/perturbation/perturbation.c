#include "perturbation.h"
#include "../precision/mp_complex.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

int ref_orbit_init(ReferenceOrbit *orbit, uint32_t max_iter) {
    memset(orbit, 0, sizeof(ReferenceOrbit));

    orbit->z_real = (double *)malloc(max_iter * sizeof(double));
    orbit->z_imag = (double *)malloc(max_iter * sizeof(double));

    if (!orbit->z_real || !orbit->z_imag) {
        ref_orbit_cleanup(orbit);
        return -1;
    }

    orbit->max_iter = max_iter;
    orbit->valid = false;

    return 0;
}

void ref_orbit_compute(ReferenceOrbit *orbit, double cx, double cy) {
    if (!orbit->z_real || !orbit->z_imag) {
        return;
    }

    orbit->ref_cx = cx;
    orbit->ref_cy = cy;

    double zx = 0.0, zy = 0.0;
    double zx2 = 0.0, zy2 = 0.0;
    uint32_t iteration = 0;

    // Store Z_ref[n] at each iteration
    while (iteration < orbit->max_iter) {
        orbit->z_real[iteration] = zx;
        orbit->z_imag[iteration] = zy;

        // Check escape
        if (zx2 + zy2 >= 4.0) {
            break;
        }

        // z = z^2 + c
        double new_zy = 2.0 * zx * zy + cy;
        double new_zx = zx2 - zy2 + cx;

        zx = new_zx;
        zy = new_zy;
        zx2 = zx * zx;
        zy2 = zy * zy;

        iteration++;
    }

    orbit->escape_iter = iteration;
    orbit->valid = true;
}

void ref_orbit_cleanup(ReferenceOrbit *orbit) {
    if (orbit->z_real) {
        free(orbit->z_real);
        orbit->z_real = NULL;
    }
    if (orbit->z_imag) {
        free(orbit->z_imag);
        orbit->z_imag = NULL;
    }
    orbit->valid = false;
}

// =============================================================================
// High-Precision Reference Orbit Implementation
// =============================================================================

int ref_orbit_hp_init(ReferenceOrbitHP *orbit, uint32_t max_iter, uint32_t precision) {
    memset(orbit, 0, sizeof(ReferenceOrbitHP));

    orbit->z_real = (double *)malloc(max_iter * sizeof(double));
    orbit->z_imag = (double *)malloc(max_iter * sizeof(double));

    if (!orbit->z_real || !orbit->z_imag) {
        ref_orbit_hp_cleanup(orbit);
        return -1;
    }

    orbit->max_iter = max_iter;
    orbit->precision = precision;
    orbit->valid = false;

    return 0;
}

void ref_orbit_hp_compute(ReferenceOrbitHP *orbit, const char *cx_str, const char *cy_str) {
    ref_orbit_hp_compute_cancellable(orbit, cx_str, cy_str, NULL, NULL);
}

static void hp_drop_cont_state(ReferenceOrbitHP *orbit) {
    if (orbit->cont_z) {
        mp_complex_clear(orbit->cont_z);
        free(orbit->cont_z);
        orbit->cont_z = NULL;
    }
}

// The hot loop, shared by fresh computes and continuations. zr/zi hold the
// current z, zr2/zi2 its component squares (maintained incrementally so the
// escape test reuses the iteration's squarings — the naive form squared z a
// second time for the norm and copied z twice per step, ~40% more MPFR work).
// On a non-escaped, non-aborted run the final z is kept as continuation state.
static void hp_orbit_run(ReferenceOrbitHP *orbit,
                         mpfr_t zr, mpfr_t zi, mpfr_t zr2, mpfr_t zi2,
                         mpfr_t cr, mpfr_t ci, mpfr_t tmp,
                         uint32_t start_iter,
                         bool (*should_abort)(void *ctx), void *ctx) {
    uint32_t iteration = start_iter;
    bool aborted = false;

    while (iteration < orbit->max_iter) {
        orbit->z_real[iteration] = mpfr_get_d(zr, MPFR_RNDN);
        orbit->z_imag[iteration] = mpfr_get_d(zi, MPFR_RNDN);

        // Escape: |z|^2 = zr2 + zi2 >= 4
        mpfr_add(tmp, zr2, zi2, MPFR_RNDN);
        if (mpfr_cmp_ui(tmp, 4) >= 0) {
            break;
        }

        // z' = z^2 + c, reusing the maintained squares
        mpfr_mul(tmp, zr, zi, MPFR_RNDN);       // zr*zi (old values)
        mpfr_sub(zr, zr2, zi2, MPFR_RNDN);
        mpfr_add(zr, zr, cr, MPFR_RNDN);
        mpfr_mul_2ui(zi, tmp, 1, MPFR_RNDN);
        mpfr_add(zi, zi, ci, MPFR_RNDN);
        mpfr_sqr(zr2, zr, MPFR_RNDN);
        mpfr_sqr(zi2, zi, MPFR_RNDN);

        iteration++;

        if (should_abort && (iteration & 0xFF) == 0 && should_abort(ctx)) {
            aborted = true;
            break;
        }
    }

    orbit->escape_iter = iteration;
    orbit->valid = !aborted;

    hp_drop_cont_state(orbit);
    if (!aborted && iteration >= orbit->max_iter) {
        // Non-escaped: keep the exact final z so a deeper budget can resume
        orbit->cont_z = (MPComplex *)malloc(sizeof(MPComplex));
        if (orbit->cont_z) {
            mp_complex_init(orbit->cont_z, orbit->precision);
            mpfr_set(orbit->cont_z->re.value, zr, MPFR_RNDN);
            mpfr_set(orbit->cont_z->im.value, zi, MPFR_RNDN);
        }
    }
}

void ref_orbit_hp_compute_cancellable(ReferenceOrbitHP *orbit,
                                      const char *cx_str, const char *cy_str,
                                      bool (*should_abort)(void *ctx), void *ctx) {
    if (!orbit->z_real || !orbit->z_imag) {
        return;
    }

    // Store reference point strings
    strncpy(orbit->ref_cx_str, cx_str, MB_HP_COORD_STR_LEN - 1);
    orbit->ref_cx_str[MB_HP_COORD_STR_LEN - 1] = '\0';
    strncpy(orbit->ref_cy_str, cy_str, MB_HP_COORD_STR_LEN - 1);
    orbit->ref_cy_str[MB_HP_COORD_STR_LEN - 1] = '\0';

    mpfr_prec_t prec = orbit->precision;

    mpfr_t zr, zi, zr2, zi2, cr, ci, tmp;
    mpfr_inits2(prec, zr, zi, zr2, zi2, cr, ci, tmp, (mpfr_ptr)0);

    mpfr_set_str(cr, cx_str, 10, MPFR_RNDN);
    mpfr_set_str(ci, cy_str, 10, MPFR_RNDN);

    // Store double approximation of reference C
    orbit->ref_cx = mpfr_get_d(cr, MPFR_RNDN);
    orbit->ref_cy = mpfr_get_d(ci, MPFR_RNDN);

    // z = 0 (squares included)
    mpfr_set_ui(zr, 0, MPFR_RNDN);
    mpfr_set_ui(zi, 0, MPFR_RNDN);
    mpfr_set_ui(zr2, 0, MPFR_RNDN);
    mpfr_set_ui(zi2, 0, MPFR_RNDN);

    hp_orbit_run(orbit, zr, zi, zr2, zi2, cr, ci, tmp, 0, should_abort, ctx);

    mpfr_clears(zr, zi, zr2, zi2, cr, ci, tmp, (mpfr_ptr)0);
}

int ref_orbit_hp_continue(ReferenceOrbitHP *dst, const ReferenceOrbitHP *src,
                          bool (*should_abort)(void *ctx), void *ctx) {
    if (!dst || !src || !dst->z_real || !dst->z_imag) return -1;
    if (!src->valid || !src->cont_z) return -1;
    if (src->escape_iter < src->max_iter) return -1;   // escaped: nothing to resume
    if (dst->precision != src->precision) return -1;
    if (dst->max_iter <= src->max_iter) return -1;

    // Carry the reference identity and the already-computed iterations
    memcpy(dst->ref_cx_str, src->ref_cx_str, MB_HP_COORD_STR_LEN);
    memcpy(dst->ref_cy_str, src->ref_cy_str, MB_HP_COORD_STR_LEN);
    dst->ref_cx = src->ref_cx;
    dst->ref_cy = src->ref_cy;
    memcpy(dst->z_real, src->z_real, (size_t)src->max_iter * sizeof(double));
    memcpy(dst->z_imag, src->z_imag, (size_t)src->max_iter * sizeof(double));

    mpfr_prec_t prec = dst->precision;

    mpfr_t zr, zi, zr2, zi2, cr, ci, tmp;
    mpfr_inits2(prec, zr, zi, zr2, zi2, cr, ci, tmp, (mpfr_ptr)0);

    // Same strings at the same precision parse to the identical value the
    // original run used.
    mpfr_set_str(cr, src->ref_cx_str, 10, MPFR_RNDN);
    mpfr_set_str(ci, src->ref_cy_str, 10, MPFR_RNDN);

    mpfr_set(zr, src->cont_z->re.value, MPFR_RNDN);
    mpfr_set(zi, src->cont_z->im.value, MPFR_RNDN);
    mpfr_sqr(zr2, zr, MPFR_RNDN);
    mpfr_sqr(zi2, zi, MPFR_RNDN);

    hp_orbit_run(dst, zr, zi, zr2, zi2, cr, ci, tmp,
                 src->max_iter, should_abort, ctx);

    mpfr_clears(zr, zi, zr2, zi2, cr, ci, tmp, (mpfr_ptr)0);

    return dst->valid ? 0 : -1;
}

void ref_orbit_hp_cleanup(ReferenceOrbitHP *orbit) {
    if (orbit->z_real) {
        free(orbit->z_real);
        orbit->z_real = NULL;
    }
    if (orbit->z_imag) {
        free(orbit->z_imag);
        orbit->z_imag = NULL;
    }
    hp_drop_cont_state(orbit);
    orbit->valid = false;
}

bool ref_orbit_hp_is_valid_for(const ReferenceOrbitHP *orbit,
                               const char *cx_str, const char *cy_str,
                               double tolerance) {
    if (!orbit->valid) {
        return false;
    }

    // Quick check: compare strings exactly
    if (strcmp(orbit->ref_cx_str, cx_str) == 0 &&
        strcmp(orbit->ref_cy_str, cy_str) == 0) {
        return true;
    }

    // Compute distance in double precision (good enough for tolerance check)
    // Parse the strings as doubles for a quick distance check
    double orbit_cx = orbit->ref_cx;
    double orbit_cy = orbit->ref_cy;
    double new_cx = strtod(cx_str, NULL);
    double new_cy = strtod(cy_str, NULL);

    double dx = fabs(orbit_cx - new_cx);
    double dy = fabs(orbit_cy - new_cy);

    return (dx + dy) <= tolerance;
}
