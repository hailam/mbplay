#include "mandelbrot.h"

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
    // Skip known interior points
    if (mb_is_in_cardioid_or_bulb(cx, cy)) {
        return MB_MAX_ITER;
    }

    double zx = 0.0, zy = 0.0;
    double zx2 = 0.0, zy2 = 0.0;
    unsigned int iteration = 0;

    while (zx2 + zy2 < 4.0 && iteration < MB_MAX_ITER) {
        zy = 2.0 * zx * zy + cy;
        zx = zx2 - zy2 + cx;
        zx2 = zx * zx;
        zy2 = zy * zy;
        iteration++;
    }

    return iteration;
}
