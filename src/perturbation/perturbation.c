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
    if (!orbit->z_real || !orbit->z_imag) {
        return;
    }

    // Store reference point strings
    strncpy(orbit->ref_cx_str, cx_str, MB_HP_COORD_STR_LEN - 1);
    orbit->ref_cx_str[MB_HP_COORD_STR_LEN - 1] = '\0';
    strncpy(orbit->ref_cy_str, cy_str, MB_HP_COORD_STR_LEN - 1);
    orbit->ref_cy_str[MB_HP_COORD_STR_LEN - 1] = '\0';

    mpfr_prec_t prec = orbit->precision;

    // Initialize HP complex numbers
    MPComplex c, z, z_new;
    MPReal norm_sqr;

    mp_complex_init_set_str(&c, cx_str, cy_str, prec);
    mp_complex_init(&z, prec);
    mp_complex_init(&z_new, prec);
    mp_real_init(&norm_sqr, prec);

    // Store double approximation of reference C
    orbit->ref_cx = mp_complex_get_re_d(&c);
    orbit->ref_cy = mp_complex_get_im_d(&c);

    // z = 0
    mp_complex_set_d(&z, 0.0, 0.0);

    uint32_t iteration = 0;

    // Mandelbrot iteration: z = z^2 + c
    while (iteration < orbit->max_iter) {
        // Store current z as double for GPU upload
        orbit->z_real[iteration] = mp_complex_get_re_d(&z);
        orbit->z_imag[iteration] = mp_complex_get_im_d(&z);

        // Check escape: |z|^2 >= 4
        mp_complex_norm_sqr(&norm_sqr, &z);
        if (mp_real_cmp_d(&norm_sqr, 4.0) >= 0) {
            break;
        }

        // z_new = z^2 + c
        mp_complex_mandelbrot_iter(&z_new, &z, &c);
        mp_complex_set(&z, &z_new);

        iteration++;
    }

    orbit->escape_iter = iteration;
    orbit->valid = true;

    // Cleanup MPFR resources
    mp_complex_clear(&c);
    mp_complex_clear(&z);
    mp_complex_clear(&z_new);
    mp_real_clear(&norm_sqr);
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
