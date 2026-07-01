#ifndef MB_PALETTES_H
#define MB_PALETTES_H

#include "../config.h"
#include <math.h>

// =============================================================================
// Palette System
// =============================================================================
//
// This module provides multiple color palettes for Mandelbrot visualization.
// Palettes use the cosine palette formula for smooth, customizable gradients:
//   color = a + b * cos(2*PI * (c*t + d))
// where t is the normalized iteration value in [0,1].
//
// Each palette is defined by 4 RGB vectors: a, b, c, d

// Cosine palette parameters (RGB triplets)
typedef struct {
    float a[3];  // Base color
    float b[3];  // Amplitude
    float c[3];  // Frequency
    float d[3];  // Phase
} CosinePalette;

// Predefined palette parameters
static const CosinePalette kPalettes[] = {
    // CLASSIC - Approximation of original bit-mixing (warm earth tones)
    // Tuned: Increased saturation for richer colors
    {{0.5f, 0.5f, 0.5f}, {0.6f, 0.6f, 0.6f}, {1.0f, 1.0f, 1.0f}, {0.0f, 0.1f, 0.2f}},

    // FIRE - Black -> Red -> Orange -> Yellow -> White
    // Tuned: Adjusted phase for later white, better progression
    {{0.5f, 0.2f, 0.0f}, {0.5f, 0.5f, 0.4f}, {1.0f, 0.8f, 0.5f}, {0.0f, 0.1f, 0.25f}},

    // OCEAN - Deep blue -> Cyan -> White
    // Tuned: More cyan transition, deeper blues
    {{0.1f, 0.3f, 0.6f}, {0.4f, 0.5f, 0.4f}, {1.0f, 1.2f, 0.6f}, {0.0f, 0.15f, 0.25f}},

    // PLASMA - Purple -> Pink -> Orange
    // Tuned: Better balance between purple and orange
    {{0.5f, 0.3f, 0.5f}, {0.5f, 0.5f, 0.5f}, {1.0f, 0.8f, 1.0f}, {0.7f, 0.85f, 0.25f}},

    // GRAYSCALE - For analysis/export
    {{0.5f, 0.5f, 0.5f}, {0.5f, 0.5f, 0.5f}, {1.0f, 1.0f, 1.0f}, {0.0f, 0.0f, 0.0f}},

    // ELECTRIC - Cyan -> Blue -> Purple -> Magenta (neon, high contrast)
    {{0.5f, 0.3f, 0.7f}, {0.5f, 0.5f, 0.5f}, {1.0f, 1.0f, 1.0f}, {0.5f, 0.7f, 0.9f}},

    // SUNSET - Deep red -> Orange -> Pink -> Purple (warm gradient)
    {{0.6f, 0.3f, 0.4f}, {0.5f, 0.4f, 0.4f}, {0.8f, 0.6f, 1.0f}, {0.0f, 0.1f, 0.5f}},

    // RAINBOW - Full spectrum cycling (equal frequency RGB)
    {{0.5f, 0.5f, 0.5f}, {0.5f, 0.5f, 0.5f}, {1.0f, 1.0f, 1.0f}, {0.0f, 0.33f, 0.67f}},

    // FOREST - Dark green -> Lime -> Yellow -> Brown (nature tones)
    {{0.4f, 0.5f, 0.3f}, {0.3f, 0.5f, 0.3f}, {1.0f, 0.8f, 0.6f}, {0.2f, 0.0f, 0.3f}},
};

// Palette names for HUD display
static const char* const kPaletteNames[] = {
    "Classic",
    "Fire",
    "Ocean",
    "Plasma",
    "Grayscale",
    "Electric",
    "Sunset",
    "Rainbow",
    "Forest"
};

// =============================================================================
// Smooth Iteration Computation
// =============================================================================

/**
 * Compute smooth iteration count from integer iteration and final |z|^2.
 * Uses the renormalization formula: smooth = iter + 1 - log2(log2(|z|))
 *
 * @param iteration Integer iteration count
 * @param final_z2 Final |z|^2 value at escape (zx*zx + zy*zy)
 * @param max_iter Maximum iteration limit
 * @return Fractional iteration count for smooth coloring
 */
static inline float mb_compute_smooth_iteration(unsigned int iteration,
                                                 float final_z2,
                                                 unsigned int max_iter) {
    // Interior points stay at max_iter
    if (iteration >= max_iter) {
        return (float)max_iter;
    }

    // Smooth coloring formula: iter + 1 - log2(log2(|z|))
    // Since |z|^2 = final_z2, we have |z| = sqrt(final_z2)
    // log2(|z|) = log2(sqrt(final_z2)) = 0.5 * log2(final_z2)
    // So: iter + 1 - log2(0.5 * log2(final_z2))
    //   = iter + 1 - log2(log2(final_z2)) + 1
    //   = iter + 2 - log2(log2(final_z2))
    // Or equivalently using the standard formula with escape radius 2:
    // smooth = iter + 1 - log2(log(final_z2) / log(4)) / log(2)

    if (final_z2 <= 4.0f) {
        // Edge case: z didn't actually escape properly
        return (float)iteration;
    }

    // log2(x) = log(x) / log(2)
    float log_z2 = logf(final_z2);        // log(|z|^2) = 2*log(|z|)
    float log_log_z = logf(log_z2 * 0.5f); // log(log(|z|))
    float log2_log_z = log_log_z / 0.693147f;  // Convert to log2

    float smooth = (float)iteration + 1.0f - log2_log_z;

    // Clamp to valid range
    if (smooth < 0.0f) smooth = 0.0f;

    return smooth;
}

// =============================================================================
// Palette Color Functions
// =============================================================================

/**
 * Get color from cosine palette using normalized value t in [0,1].
 */
static inline void palette_get_color(const CosinePalette *palette,
                                     float t,
                                     PixelColor *pixel) {
    // Cosine palette formula: color = a + b * cos(2*PI * (c*t + d))
    float pi2 = 6.283185f;

    float r = palette->a[0] + palette->b[0] * cosf(pi2 * (palette->c[0] * t + palette->d[0]));
    float g = palette->a[1] + palette->b[1] * cosf(pi2 * (palette->c[1] * t + palette->d[1]));
    float b = palette->a[2] + palette->b[2] * cosf(pi2 * (palette->c[2] * t + palette->d[2]));

    // Clamp to [0, 1]
    if (r < 0.0f) r = 0.0f; if (r > 1.0f) r = 1.0f;
    if (g < 0.0f) g = 0.0f; if (g > 1.0f) g = 1.0f;
    if (b < 0.0f) b = 0.0f; if (b > 1.0f) b = 1.0f;

    pixel->r = (unsigned char)(r * 255.0f);
    pixel->g = (unsigned char)(g * 255.0f);
    pixel->b = (unsigned char)(b * 255.0f);
}

/**
 * Map smooth iteration to color using specified palette.
 * Points in the set (iteration >= max_iter) are black.
 *
 * @param pixel Output pixel color
 * @param smooth_iter Fractional iteration count
 * @param max_iter Maximum iteration limit (for black detection)
 * @param palette_id Palette to use
 * @param color_cycle_scale Color band density (default 64.0f, lower = more bands)
 */
static inline void color_from_smooth_iteration(PixelColor *pixel,
                                                float smooth_iter,
                                                unsigned int max_iter,
                                                MBPaletteId palette_id,
                                                float color_cycle_scale) {
    // Interior points are black
    if (smooth_iter >= (float)max_iter - 0.5f) {
        pixel->r = 0;
        pixel->g = 0;
        pixel->b = 0;
        return;
    }

    // Normalize to [0, 1] with wrapping for color cycling
    // Use log scale for better distribution at high iteration counts
    float t = smooth_iter / color_cycle_scale;  // Color cycle every N iterations
    t = t - floorf(t);  // Wrap to [0, 1]

    if (palette_id >= MB_PALETTE_COUNT) {
        palette_id = MB_PALETTE_CLASSIC;
    }

    palette_get_color(&kPalettes[palette_id], t, pixel);
}

/**
 * Classic color mapping using original bit-mixing algorithm.
 * Preserved for backward compatibility.
 */
static inline void color_from_iteration_classic(PixelColor *pixel,
                                                 unsigned int iteration,
                                                 unsigned int max_iter) {
    if (iteration >= max_iter) {
        pixel->r = 0;
        pixel->g = 0;
        pixel->b = 0;
    } else {
        pixel->r = (unsigned char)((iteration * 9) & 0xFF);
        pixel->g = (unsigned char)((iteration * 7) & 0xFF);
        pixel->b = (unsigned char)((iteration * 5) & 0xFF);
    }
}

/**
 * Apply palette to integer iteration count (no smooth coloring).
 * Uses palette color but with discrete steps.
 *
 * @param pixel Output pixel color
 * @param iteration Integer iteration count
 * @param max_iter Maximum iteration limit
 * @param palette_id Palette to use
 * @param color_cycle_scale Color band density (default 64.0f, lower = more bands)
 */
static inline void color_from_iteration_palette(PixelColor *pixel,
                                                 unsigned int iteration,
                                                 unsigned int max_iter,
                                                 MBPaletteId palette_id,
                                                 float color_cycle_scale) {
    if (iteration >= max_iter) {
        pixel->r = 0;
        pixel->g = 0;
        pixel->b = 0;
        return;
    }

    // For non-smooth mode with palette, just use integer iteration
    float t = (float)iteration / color_cycle_scale;
    t = t - floorf(t);

    if (palette_id >= MB_PALETTE_COUNT) {
        palette_id = MB_PALETTE_CLASSIC;
    }

    palette_get_color(&kPalettes[palette_id], t, pixel);
}

#endif // MB_PALETTES_H
