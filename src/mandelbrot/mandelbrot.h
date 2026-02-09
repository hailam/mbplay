#ifndef MB_MANDELBROT_H
#define MB_MANDELBROT_H

#include "../config.h"

// =============================================================================
// Mandelbrot Iteration API
// =============================================================================

/**
 * Check if a point is in the main cardioid or period-2 bulb.
 * These points are known to be in the Mandelbrot set, so we can skip iteration.
 *
 * @param cx Real part of complex coordinate
 * @param cy Imaginary part of complex coordinate
 * @return 1 if point is in cardioid/bulb (definitely in set), 0 otherwise
 */
int mb_is_in_cardioid_or_bulb(double cx, double cy);

/**
 * Compute iteration count for a single point using scalar arithmetic.
 *
 * @param cx Real part of complex coordinate
 * @param cy Imaginary part of complex coordinate
 * @return Number of iterations before escape, or MB_MAX_ITER if in set
 */
unsigned int mb_compute_iteration_scalar(double cx, double cy);

/**
 * Compute iteration count for two points simultaneously using SIMD.
 * Falls back to scalar if one point is in cardioid/bulb.
 *
 * @param cx0 Real part of first point
 * @param cy0 Imaginary part of first point
 * @param cx1 Real part of second point
 * @param cy1 Imaginary part of second point
 * @param iter0 Output: iteration count for first point
 * @param iter1 Output: iteration count for second point
 */
void mb_compute_iteration_simd2(double cx0, double cy0, double cx1, double cy1,
                                unsigned int *iter0, unsigned int *iter1);

#endif // MB_MANDELBROT_H
