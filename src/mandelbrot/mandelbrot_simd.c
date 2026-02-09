#include "mandelbrot.h"
#include "../simd/simd.h"

// =============================================================================
// SIMD Iteration (2 pixels at once)
// =============================================================================

#if defined(MB_SIMD_NEON)

void mb_compute_iteration_simd2(double cx0, double cy0,
                                double cx1, double cy1,
                                unsigned int *iter0, unsigned int *iter1) {
    // Check cardioid/bulb for both pixels first
    int skip0 = mb_is_in_cardioid_or_bulb(cx0, cy0);
    int skip1 = mb_is_in_cardioid_or_bulb(cx1, cy1);

    if (skip0 && skip1) {
        *iter0 = MB_MAX_ITER;
        *iter1 = MB_MAX_ITER;
        return;
    }

    if (skip0) {
        *iter0 = MB_MAX_ITER;
        *iter1 = mb_compute_iteration_scalar(cx1, cy1);
        return;
    }

    if (skip1) {
        *iter0 = mb_compute_iteration_scalar(cx0, cy0);
        *iter1 = MB_MAX_ITER;
        return;
    }

    // SIMD path: process both pixels together
    simd_f64x2 cx = simd_set_f64(cx0, cx1);
    simd_f64x2 cy = simd_set_f64(cy0, cy1);
    simd_f64x2 zx = simd_splat_f64(0.0);
    simd_f64x2 zy = simd_splat_f64(0.0);
    simd_f64x2 zx2 = simd_splat_f64(0.0);
    simd_f64x2 zy2 = simd_splat_f64(0.0);
    simd_f64x2 four = simd_splat_f64(4.0);
    simd_f64x2 two = simd_splat_f64(2.0);

    unsigned int iterations[2] = {0, 0};
    int done[2] = {0, 0};

    for (unsigned int i = 0; i < MB_MAX_ITER && !(done[0] && done[1]); i++) {
        // zy = 2 * zx * zy + cy
        simd_f64x2 zxy = simd_mul_f64(zx, zy);
        zy = simd_fma_f64(cy, two, zxy);

        // zx = zx2 - zy2 + cx
        zx = simd_add_f64(simd_sub_f64(zx2, zy2), cx);

        // Update squares
        zx2 = simd_mul_f64(zx, zx);
        zy2 = simd_mul_f64(zy, zy);

        // Check escape condition
        simd_f64x2 mag2 = simd_add_f64(zx2, zy2);

        // Extract and check each lane
        double mag2_arr[2];
        simd_store_f64(mag2_arr, mag2);

        if (!done[0]) {
            if (mag2_arr[0] >= 4.0) {
                iterations[0] = i + 1;
                done[0] = 1;
            }
        }
        if (!done[1]) {
            if (mag2_arr[1] >= 4.0) {
                iterations[1] = i + 1;
                done[1] = 1;
            }
        }
    }

    *iter0 = done[0] ? iterations[0] : MB_MAX_ITER;
    *iter1 = done[1] ? iterations[1] : MB_MAX_ITER;
}

#else

// Fallback: use scalar implementation for both pixels
void mb_compute_iteration_simd2(double cx0, double cy0,
                                double cx1, double cy1,
                                unsigned int *iter0, unsigned int *iter1) {
    *iter0 = mb_compute_iteration_scalar(cx0, cy0);
    *iter1 = mb_compute_iteration_scalar(cx1, cy1);
}

#endif
