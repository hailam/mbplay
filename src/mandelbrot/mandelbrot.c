#include "mandelbrot.h"
#include "../color/color.h"

// =============================================================================
// Cardioid and Period-2 Bulb Detection
// =============================================================================

int mb_is_in_cardioid_or_bulb(double cx, double cy) {
    // Main cardioid check
    double cx_shifted = cx - 0.25;
    double cy2 = cy * cy;
    double q = cx_shifted * cx_shifted + cy2;
    if (q * (q + cx_shifted) <= 0.25 * cy2) {
        return 1;
    }

    // Period-2 bulb check
    double cx_plus1 = cx + 1.0;
    if (cx_plus1 * cx_plus1 + cy2 <= 0.0625) {
        return 1;
    }

    return 0;
}

// =============================================================================
// Scalar Iteration
// =============================================================================

unsigned int mb_compute_iteration_scalar(double cx, double cy) {
    return mb_compute_point(cx, cy, MB_MAX_ITER);
}

unsigned int mb_compute_point(double cx, double cy, unsigned int max_iter) {
    // Skip known interior points
    if (mb_is_in_cardioid_or_bulb(cx, cy)) {
        return max_iter;
    }

    double zx = 0.0, zy = 0.0;
    double zx2 = 0.0, zy2 = 0.0;
    unsigned int iteration = 0;

    // Brent-style periodicity detection: interior points fall into a cycle;
    // once the orbit revisits a saved value (to ~1 ulp), it can never escape,
    // so the full budget need not be burned. Saves are at power-of-two
    // iterations so any cycle length is eventually bracketed.
    double sx = 0.0, sy = 0.0;
    unsigned int next_save = 8;

    while (zx2 + zy2 < 4.0 && iteration < max_iter) {
        zy = 2.0 * zx * zy + cy;
        zx = zx2 - zy2 + cx;
        zx2 = zx * zx;
        zy2 = zy * zy;
        iteration++;

        double tol = 1e-15 * (fabs(zx) + fabs(zy)) + 1e-300;
        if (fabs(zx - sx) < tol && fabs(zy - sy) < tol) {
            return max_iter;  // periodic: interior
        }
        if (iteration == next_save) {
            sx = zx;
            sy = zy;
            next_save *= 2;
        }
    }

    return iteration;
}

// =============================================================================
// Double-Precision Tile Computation
// =============================================================================

void mb_compute_tile_double(double center_x, double center_y, double scale,
                            int tile_x, int tile_y, int tile_size,
                            int vp_half_w, int vp_half_h,
                            int max_iter, PixelColor *output) {
    for (int ly = 0; ly < tile_size; ly++) {
        for (int lx = 0; lx < tile_size; lx++) {
            // Global pixel coordinates
            double px = (double)(tile_x + lx);
            double py = (double)(tile_y + ly);

            // Map to complex plane
            double cx = center_x + (px - vp_half_w) * scale;
            double cy = center_y + (py - vp_half_h) * scale;

            // Compute iteration with cardioid/bulb skip
            unsigned int iteration = mb_compute_point(cx, cy, (unsigned int)max_iter);

            // Convert to color
            color_from_iteration_classic(&output[ly * tile_size + lx], iteration,
                                         (unsigned int)max_iter);
        }
    }
}

// =============================================================================
// Smooth Iteration Point Computation
// =============================================================================

/**
 * Compute iteration count and final |z|^2 for smooth coloring.
 * Returns both iteration count and the escape magnitude for color interpolation.
 */
unsigned int mb_compute_point_smooth(double cx, double cy, unsigned int max_iter,
                                     float *final_z2) {
    *final_z2 = 0.0f;

    // Skip known interior points
    if (mb_is_in_cardioid_or_bulb(cx, cy)) {
        return max_iter;
    }

    double zx = 0.0, zy = 0.0;
    double zx2 = 0.0, zy2 = 0.0;
    unsigned int iteration = 0;

    // Brent-style periodicity detection (see mb_compute_point)
    double sx = 0.0, sy = 0.0;
    unsigned int next_save = 8;

    while (zx2 + zy2 < 4.0 && iteration < max_iter) {
        zy = 2.0 * zx * zy + cy;
        zx = zx2 - zy2 + cx;
        zx2 = zx * zx;
        zy2 = zy * zy;
        iteration++;

        double tol = 1e-15 * (fabs(zx) + fabs(zy)) + 1e-300;
        if (fabs(zx - sx) < tol && fabs(zy - sy) < tol) {
            *final_z2 = 0.0f;
            return max_iter;  // periodic: interior
        }
        if (iteration == next_save) {
            sx = zx;
            sy = zy;
            next_save *= 2;
        }
    }

    *final_z2 = (float)(zx2 + zy2);
    return iteration;
}

/**
 * Compute a tile using double precision with smooth coloring support.
 */
void mb_compute_tile_double_smooth(double center_x, double center_y, double scale,
                                   int tile_x, int tile_y, int tile_size,
                                   int vp_half_w, int vp_half_h,
                                   int max_iter, PixelColor *output,
                                   const MBRenderSettings *settings) {
    for (int ly = 0; ly < tile_size; ly++) {
        double py = (double)(tile_y + ly);
        double cy = center_y + (py - vp_half_h) * scale;

        int lx = 0;
        // Two pixels per step (NEON lanes on arm64)
        for (; lx + 1 < tile_size; lx += 2) {
            double cx0 = center_x + ((double)(tile_x + lx) - vp_half_w) * scale;
            double cx1 = center_x + ((double)(tile_x + lx + 1) - vp_half_w) * scale;

            unsigned int it0, it1;
            float z0, z1;
            mb_compute_pair_smooth(cx0, cy, cx1, cy, (unsigned int)max_iter,
                                   &it0, &it1, &z0, &z1);

            color_from_iteration_ex(&output[ly * tile_size + lx], it0, z0,
                                    (unsigned int)max_iter, settings);
            color_from_iteration_ex(&output[ly * tile_size + lx + 1], it1, z1,
                                    (unsigned int)max_iter, settings);
        }
        for (; lx < tile_size; lx++) {
            double cx = center_x + ((double)(tile_x + lx) - vp_half_w) * scale;
            float final_z2;
            unsigned int iteration = mb_compute_point_smooth(cx, cy, (unsigned int)max_iter, &final_z2);
            color_from_iteration_ex(&output[ly * tile_size + lx], iteration, final_z2,
                                    (unsigned int)max_iter, settings);
        }
    }
}
