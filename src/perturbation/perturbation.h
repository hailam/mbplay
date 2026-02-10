#ifndef MB_PERTURBATION_H
#define MB_PERTURBATION_H

#include "../config.h"
#include <stdint.h>
#include <stdbool.h>

#define MB_REF_ORBIT_MAX_ITER 4096

typedef struct {
    double ref_cx, ref_cy;      // Reference point C
    double *z_real, *z_imag;    // Z_ref[n] for each iteration
    uint32_t escape_iter;       // When reference escaped
    uint32_t max_iter;
    bool valid;
} ReferenceOrbit;

/**
 * Initialize a reference orbit structure.
 * Allocates z_real/z_imag arrays.
 * @param orbit The orbit to initialize
 * @param max_iter Maximum iterations to store
 * @return 0 on success, -1 on failure
 */
int ref_orbit_init(ReferenceOrbit *orbit, uint32_t max_iter);

/**
 * Compute the reference orbit for a given center point.
 * Performs standard Mandelbrot iteration, storing Z_ref[n] at each step.
 * @param orbit The orbit structure (must be initialized)
 * @param cx Real part of center point
 * @param cy Imaginary part of center point
 */
void ref_orbit_compute(ReferenceOrbit *orbit, double cx, double cy);

/**
 * Free orbit arrays.
 * @param orbit The orbit to cleanup
 */
void ref_orbit_cleanup(ReferenceOrbit *orbit);

#endif // MB_PERTURBATION_H
