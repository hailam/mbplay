#ifndef MB_CONFIG_H
#define MB_CONFIG_H

#include <stdbool.h>
#include <stdio.h>

// =============================================================================
// Platform Detection
// =============================================================================

#if defined(__ARM_NEON)
#define MB_SIMD_NEON 1
#elif defined(__AVX2__)
#define MB_SIMD_AVX2 1
#elif defined(__SSE2__)
#define MB_SIMD_SSE2 1
#else
#define MB_SIMD_NONE 1
#endif

// GPU detection (Metal on macOS)
#if defined(__APPLE__) && defined(__MACH__)
#define MB_GPU_METAL 1
#endif

// =============================================================================
// Default Image Dimensions (can be overridden at runtime)
// =============================================================================

#define MB_DEFAULT_WIDTH 1920
#define MB_DEFAULT_HEIGHT 1080
#define MB_DEFAULT_MAX_ITER 1000

// Legacy defines for backward compatibility with existing code
#ifndef MB_WIDTH
#define MB_WIDTH MB_DEFAULT_WIDTH
#endif
#ifndef MB_HEIGHT
#define MB_HEIGHT MB_DEFAULT_HEIGHT
#endif
#ifndef MB_MAX_ITER
#define MB_MAX_ITER MB_DEFAULT_MAX_ITER
#endif

// =============================================================================
// Tile-based Rendering
// =============================================================================

#define MB_TILE_SIZE 64
#define MB_NUM_TILES_X ((MB_WIDTH + MB_TILE_SIZE - 1) / MB_TILE_SIZE)
#define MB_NUM_TILES_Y ((MB_HEIGHT + MB_TILE_SIZE - 1) / MB_TILE_SIZE)
#define MB_TOTAL_TILES (MB_NUM_TILES_X * MB_NUM_TILES_Y)

// =============================================================================
// Pre-computed Scale Constants (for default dimensions)
// =============================================================================

#define MB_CX_SCALE (3.5 / MB_WIDTH)
#define MB_CY_SCALE (2.0 / MB_HEIGHT)
#define MB_WIDTH_HALF (MB_WIDTH / 2.0)
#define MB_HEIGHT_HALF (MB_HEIGHT / 2.0)

// =============================================================================
// Runtime Configuration
// =============================================================================

typedef struct {
    int width;
    int height;
    int max_iter;
    double cx_scale;
    double cy_scale;
    double width_half;
    double height_half;
} MBConfig;

static inline void mb_config_init(MBConfig *cfg, int width, int height, int max_iter) {
    cfg->width = width;
    cfg->height = height;
    cfg->max_iter = max_iter;
    cfg->cx_scale = 3.5 / width;
    cfg->cy_scale = 2.0 / height;
    cfg->width_half = width / 2.0;
    cfg->height_half = height / 2.0;
}

// =============================================================================
// Pixel Color Type
// =============================================================================

typedef struct {
    unsigned char r, g, b;
} PixelColor;

// =============================================================================
// Perturbation Theory Configuration
// =============================================================================

#define MB_PERTURBATION_ENABLED 1
#define MB_REF_ORBIT_MAX_ITER 4096
#define MB_GLITCH_MARKER 0xFFFFFFFE

// Legacy: Switch to CPU double precision above this zoom level
// With perturbation enabled, GPU can render at any zoom level
#define MB_FLOAT_ZOOM_LIMIT 1e15  // Effectively disabled with perturbation

// =============================================================================
// High-Precision (Arbitrary Precision) Configuration
// =============================================================================

// Zoom threshold to switch to high-precision mode
#define MB_HP_ZOOM_THRESHOLD 1e12

// Precision tiers (bits) based on zoom level
#define MB_PREC_TIER_1 128   // zoom 1e12 - 1e30
#define MB_PREC_TIER_2 256   // zoom 1e30 - 1e60
#define MB_PREC_TIER_3 512   // zoom 1e60 - 1e120
#define MB_PREC_TIER_4 1024  // zoom > 1e120

// Maximum string length for HP coordinates
#define MB_HP_COORD_STR_LEN 512

// Maximum zoom level with HP mode (log10 of zoom)
#define MB_MAX_ZOOM_HP 200

// Tile size for interactive rendering (256x256 pixels, ~64MB for 256 tiles)
#define MB_INTERACTIVE_TILE_SIZE 256
#define MB_MAX_CACHED_TILES 256

// =============================================================================
// Interactive Viewer State
// =============================================================================

typedef struct {
    double center_x, center_y;  // Complex plane center (double precision, for display/UI)
    double zoom_level;          // 1.0 = default view, higher = zoomed in
    int viewport_width;
    int viewport_height;

    // High-precision center coordinates (decimal strings)
    char center_x_str[MB_HP_COORD_STR_LEN];
    char center_y_str[MB_HP_COORD_STR_LEN];
    bool high_precision_mode;   // True when zoom exceeds HP threshold
} MBViewState;

static inline void mb_view_state_init(MBViewState *view, int width, int height) {
    view->center_x = -0.5;
    view->center_y = 0.0;
    view->zoom_level = 1.0;
    view->viewport_width = width;
    view->viewport_height = height;

    // Initialize HP center strings
    snprintf(view->center_x_str, MB_HP_COORD_STR_LEN, "-0.5");
    snprintf(view->center_y_str, MB_HP_COORD_STR_LEN, "0.0");
    view->high_precision_mode = false;
}

// Pixel to complex plane coordinate mapping
static inline void mb_pixel_to_complex(
    const MBViewState *view, int px, int py,
    double *cx, double *cy) {
    // Scale based on viewport height to maintain aspect ratio
    double scale = (2.0 / view->viewport_height) / view->zoom_level;
    *cx = view->center_x + (px - view->viewport_width / 2.0) * scale;
    *cy = view->center_y + (py - view->viewport_height / 2.0) * scale;
}

// Complex to pixel coordinate mapping
static inline void mb_complex_to_pixel(
    const MBViewState *view, double cx, double cy,
    int *px, int *py) {
    double scale = (2.0 / view->viewport_height) / view->zoom_level;
    *px = (int)((cx - view->center_x) / scale + view->viewport_width / 2.0);
    *py = (int)((cy - view->center_y) / scale + view->viewport_height / 2.0);
}

// Get the current scale (complex units per pixel)
static inline double mb_view_get_scale(const MBViewState *view) {
    return (2.0 / view->viewport_height) / view->zoom_level;
}

// Check if we need double precision at current zoom level
static inline int mb_view_needs_double(const MBViewState *view) {
    return view->zoom_level >= MB_FLOAT_ZOOM_LIMIT;
}

// Check if we need high-precision mode at current zoom level
static inline bool mb_view_needs_high_precision(const MBViewState *view) {
    return view->zoom_level >= MB_HP_ZOOM_THRESHOLD;
}

// Set the HP center from doubles (syncs string representations)
static inline void mb_view_set_center_hp(MBViewState *view, double cx, double cy) {
    view->center_x = cx;
    view->center_y = cy;
    // Store as high-precision string with maximum precision for double
    snprintf(view->center_x_str, MB_HP_COORD_STR_LEN, "%.17g", cx);
    snprintf(view->center_y_str, MB_HP_COORD_STR_LEN, "%.17g", cy);
}

#endif // MB_CONFIG_H
