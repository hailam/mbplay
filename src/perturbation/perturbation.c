#include "perturbation.h"
#include <stdlib.h>
#include <string.h>

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
