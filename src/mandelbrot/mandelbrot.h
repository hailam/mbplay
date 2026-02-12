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
 * Compute iteration count for a single point with custom max iteration limit.
 * Includes cardioid/bulb early-out optimization.
 *
 * @param cx Real part of complex coordinate
 * @param cy Imaginary part of complex coordinate
 * @param max_iter Maximum iterations before considering point in set
 * @return Number of iterations before escape, or max_iter if in set
 */
unsigned int mb_compute_point(double cx, double cy, unsigned int max_iter);

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

// =============================================================================
// Tile-based Computation (for interactive viewer)
// =============================================================================

/**
 * Compute a tile using double precision (CPU).
 * Used for deep zoom when float precision is insufficient.
 *
 * @param center_x View center X in complex plane
 * @param center_y View center Y in complex plane
 * @param scale Complex units per pixel
 * @param tile_x Tile X offset in pixels
 * @param tile_y Tile Y offset in pixels
 * @param tile_size Tile dimension
 * @param vp_half_w Half viewport width
 * @param vp_half_h Half viewport height
 * @param max_iter Maximum iterations
 * @param output Output buffer (tile_size * tile_size pixels)
 */
void mb_compute_tile_double(double center_x, double center_y, double scale,
                            int tile_x, int tile_y, int tile_size,
                            int vp_half_w, int vp_half_h,
                            int max_iter, PixelColor *output);

// =============================================================================
// Smooth Coloring Support
// =============================================================================

/**
 * Compute iteration count and final |z|^2 for smooth coloring.
 *
 * @param cx Real part of complex coordinate
 * @param cy Imaginary part of complex coordinate
 * @param max_iter Maximum iterations
 * @param final_z2 Output: final |z|^2 at escape (for smooth coloring)
 * @return Number of iterations before escape
 */
unsigned int mb_compute_point_smooth(double cx, double cy, unsigned int max_iter,
                                     float *final_z2);

/**
 * Compute a tile using double precision with smooth coloring support.
 *
 * @param center_x View center X in complex plane
 * @param center_y View center Y in complex plane
 * @param scale Complex units per pixel
 * @param tile_x Tile X offset in pixels
 * @param tile_y Tile Y offset in pixels
 * @param tile_size Tile dimension
 * @param vp_half_w Half viewport width
 * @param vp_half_h Half viewport height
 * @param max_iter Maximum iterations
 * @param output Output buffer (tile_size * tile_size pixels)
 * @param settings Render settings (color mode, palette)
 */
void mb_compute_tile_double_smooth(double center_x, double center_y, double scale,
                                   int tile_x, int tile_y, int tile_size,
                                   int vp_half_w, int vp_half_h,
                                   int max_iter, PixelColor *output,
                                   const MBRenderSettings *settings);

#endif // MB_MANDELBROT_H
