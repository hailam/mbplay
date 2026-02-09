#ifndef MB_COLOR_H
#define MB_COLOR_H

#include "../config.h"

// =============================================================================
// Color Mapping (Header-only, inline)
// =============================================================================

/**
 * Map iteration count to RGB color.
 * Points in the set (iteration == MAX_ITER) are black.
 * Escaped points use a simple bit-mixing color scheme.
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

#endif // MB_COLOR_H
