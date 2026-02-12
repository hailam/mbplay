#ifndef MB_COLOR_H
#define MB_COLOR_H

#include "../config.h"
#include "palettes.h"

// =============================================================================
// Color Mapping (Header-only, inline)
// =============================================================================

/**
 * Map iteration count to RGB color (classic mode).
 * Points in the set (iteration == MAX_ITER) are black.
 * Escaped points use a simple bit-mixing color scheme.
 *
 * This is the original coloring function, preserved for backward compatibility.
 */
static inline void color_from_iteration(PixelColor *pixel,
                                        unsigned int iteration) {
  if (iteration == MB_MAX_ITER) {
    pixel->r = 0;
    pixel->g = 0;
    pixel->b = 0;
  } else {
    // Bitwise AND instead of modulo for efficiency
    pixel->r = (unsigned char)((iteration * 9) & 0xFF);
    pixel->g = (unsigned char)((iteration * 7) & 0xFF);
    pixel->b = (unsigned char)((iteration * 5) & 0xFF);
  }
}

/**
 * Map iteration count to RGB color with full render settings.
 * Supports smooth coloring and multiple palettes.
 *
 * @param pixel Output pixel color
 * @param iteration Integer iteration count
 * @param final_z2 Final |z|^2 value (required for smooth mode)
 * @param max_iter Maximum iteration limit
 * @param settings Render settings (color mode, palette)
 */
static inline void color_from_iteration_ex(PixelColor *pixel,
                                           unsigned int iteration,
                                           float final_z2,
                                           unsigned int max_iter,
                                           const MBRenderSettings *settings) {
    if (settings->color_mode == MB_COLOR_MODE_SMOOTH) {
        // Smooth coloring with palette
        float smooth = mb_compute_smooth_iteration(iteration, final_z2, max_iter);
        color_from_smooth_iteration(pixel, smooth, max_iter, settings->palette_id);
    } else {
        // Classic integer coloring
        if (settings->palette_id == MB_PALETTE_CLASSIC) {
            // Original bit-mixing algorithm
            color_from_iteration_classic(pixel, iteration, max_iter);
        } else {
            // Other palette with discrete steps
            color_from_iteration_palette(pixel, iteration, max_iter, settings->palette_id);
        }
    }
}

#endif // MB_COLOR_H
