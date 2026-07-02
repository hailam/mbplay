#ifndef MB_CONFIG_H
#define MB_CONFIG_H

#include <stdbool.h>
#include <stdio.h>
#include <math.h>
#include "precision/floatexp.h"

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

// Maximum string length for HP coordinates.
// Must hold zoom_log10 + ~28 significant digits plus sign/point/exponent;
// 4096 covers the MB_ZOOM_LOG10_MAX = 4000 ceiling.
#define MB_HP_COORD_STR_LEN 4096

// =============================================================================
// Zoom Limits (single source of truth for every clamp in the app)
// =============================================================================
//
// The zoom level lives in LOG10 SPACE (MBViewState.zoom_log10): a plain
// double zoom would cap at ~1e304 where scale = (2/height)/zoom underflows.
// Scales and per-pixel deltas beyond double range use extended-exponent
// arithmetic (FloatExp, precision/floatexp.h), and the perturbation loop has
// a floatexp phase for deltas below double range.
//
// The 10^4000 ceiling is pragmatic, not architectural: the center strings
// need ~zoom_log10 + 12 digits (MB_HP_COORD_STR_LEN bounds them), MPFR
// precision grows to ~13.4k bits, and iteration budgets at that depth are
// what actually limit exploration. Raise MB_ZOOM_LOG10_MAX and
// MB_HP_COORD_STR_LEN together if deeper is ever wanted.
//
// zoom_level remains as a SATURATING double mirror (capped at 1e300) for
// shallow-zoom code paths and display; all deep logic derives from
// zoom_log10 via the helpers below.

#define MB_ZOOM_LOG10_MIN 0.0
#define MB_ZOOM_LOG10_MAX 4000.0
#define MB_ZOOM_MIRROR_MAX 1e300

static inline double mb_clamp_zoom_log10(double l10) {
    if (l10 < MB_ZOOM_LOG10_MIN) return MB_ZOOM_LOG10_MIN;
    if (l10 > MB_ZOOM_LOG10_MAX) return MB_ZOOM_LOG10_MAX;
    return l10;
}

// Above this view zoom the z/x/y map-tile pyramid is abandoned for
// screen-space perturbation tiles: map tile bounds are computed in double,
// and at tile zoom ~41 (view zoom ~1.5e11) the per-pixel step inside a tile
// drops below ~32 ulps of the coordinates, quantizing pixels.
#define MB_DEEP_ZOOM_THRESHOLD 1e11
#define MB_DEEP_ZOOM_LOG10 11.0

// Iteration budget grows with zoom depth; boundary detail at zoom 10^N needs
// roughly O(N) more iterations than the default view. The cap is what BLA
// iteration-skipping makes affordable.
static inline unsigned int mb_max_iter_for_zoom_log10(double zoom_log10) {
    if (zoom_log10 <= 0.0) return MB_DEFAULT_MAX_ITER;
    double iters = (double)MB_DEFAULT_MAX_ITER + 200.0 * zoom_log10;
    if (iters > 500000.0) iters = 500000.0;
    return (unsigned int)iters;
}

static inline unsigned int mb_max_iter_for_zoom(double zoom) {
    if (zoom <= 1.0) return MB_DEFAULT_MAX_ITER;
    return mb_max_iter_for_zoom_log10(log10(zoom));
}

// Same budget for a map tile, derived from the view zoom whose pixel scale
// matches that tile zoom: view_zoom ~= 2^(tile_zoom - 2.9) for a ~1080px
// viewport, i.e. log10(view_zoom) ~= 0.301 * tile_zoom - 0.87.
static inline unsigned int mb_max_iter_for_tile_zoom(int tile_zoom) {
    double log_zoom = 0.301 * (double)tile_zoom - 0.87;
    if (log_zoom < 0.0) log_zoom = 0.0;
    double iters = (double)MB_DEFAULT_MAX_ITER + 200.0 * log_zoom;
    if (iters > 150000.0) iters = 150000.0;
    return (unsigned int)iters;
}

// Tile size for interactive rendering (256x256 pixels, ~64MB for 256 tiles)
#define MB_INTERACTIVE_TILE_SIZE 256
#define MB_MAX_CACHED_TILES 256

// =============================================================================
// Rendering Mode Configuration
// =============================================================================

// Coloring modes
typedef enum {
    MB_COLOR_MODE_CLASSIC = 0,  // Integer iteration (classic banding)
    MB_COLOR_MODE_SMOOTH = 1    // Smooth coloring using log2(log2(|z|))
} MBColorMode;

// Available palettes
typedef enum {
    MB_PALETTE_CLASSIC = 0,     // Original bit-mixing palette
    MB_PALETTE_FIRE = 1,        // Black → Red → Orange → Yellow → White
    MB_PALETTE_OCEAN = 2,       // Deep blue → Cyan → White
    MB_PALETTE_PLASMA = 3,      // Purple → Pink → Orange (cosine)
    MB_PALETTE_GRAYSCALE = 4,   // For analysis/export
    MB_PALETTE_ELECTRIC = 5,    // Cyan → Blue → Purple → Magenta (neon)
    MB_PALETTE_SUNSET = 6,      // Deep red → Orange → Pink → Purple
    MB_PALETTE_RAINBOW = 7,     // Full spectrum cycling
    MB_PALETTE_FOREST = 8,      // Dark green → Lime → Yellow → Brown
    MB_PALETTE_COUNT = 9
} MBPaletteId;

// Rendering state
typedef struct {
    MBColorMode color_mode;
    MBPaletteId palette_id;
    bool antialiasing_enabled;   // 2x2 supersampling
    float color_cycle_scale;     // Color band density, default 64.0f, range [8.0 - 512.0]
} MBRenderSettings;

// =============================================================================
// Interactive Viewer State
// =============================================================================

typedef struct {
    double center_x, center_y;  // Complex plane center (double precision, for display/UI)
    double zoom_level;          // SATURATING mirror of 10^zoom_log10 (caps at 1e300)
    double zoom_log10;          // Source of truth for the zoom level (log10)
    int viewport_width;
    int viewport_height;

    // High-precision center coordinates (decimal strings)
    char center_x_str[MB_HP_COORD_STR_LEN];
    char center_y_str[MB_HP_COORD_STR_LEN];
    bool high_precision_mode;   // True when zoom exceeds HP threshold
} MBViewState;

// Read the log10 zoom; falls back to the double mirror for code (and tests)
// that set zoom_level directly.
static inline double mb_view_zoom_log10(const MBViewState *view) {
    if (view->zoom_log10 > 0.0) return view->zoom_log10;
    if (view->zoom_level > 1.0) return log10(view->zoom_level);
    return 0.0;
}

// The only sanctioned way to change the zoom level.
static inline void mb_view_set_zoom_log10(MBViewState *view, double l10) {
    l10 = mb_clamp_zoom_log10(l10);
    view->zoom_log10 = l10;
    view->zoom_level = l10 >= 300.0 ? MB_ZOOM_MIRROR_MAX : pow(10.0, l10);
}

static inline void mb_view_state_init(MBViewState *view, int width, int height) {
    view->center_x = -0.5;
    view->center_y = 0.0;
    view->zoom_level = 1.0;
    view->zoom_log10 = 0.0;
    view->viewport_width = width;
    view->viewport_height = height;

    // Initialize HP center strings
    snprintf(view->center_x_str, MB_HP_COORD_STR_LEN, "-0.5");
    snprintf(view->center_y_str, MB_HP_COORD_STR_LEN, "0.0");
    view->high_precision_mode = false;
}

// Pixel to complex plane coordinate mapping.
// Screen convention matches the viewer: py grows downward, imaginary axis
// grows upward, so the top of the screen is center_y + half-height.
static inline void mb_pixel_to_complex(
    const MBViewState *view, int px, int py,
    double *cx, double *cy) {
    // Scale based on viewport height to maintain aspect ratio
    double scale = (2.0 / view->viewport_height) / view->zoom_level;
    *cx = view->center_x + (px - view->viewport_width / 2.0) * scale;
    *cy = view->center_y - (py - view->viewport_height / 2.0) * scale;
}

// Complex to pixel coordinate mapping (inverse of mb_pixel_to_complex)
static inline void mb_complex_to_pixel(
    const MBViewState *view, double cx, double cy,
    int *px, int *py) {
    double scale = (2.0 / view->viewport_height) / view->zoom_level;
    double fx = (cx - view->center_x) / scale + view->viewport_width / 2.0;
    double fy = (view->center_y - cy) / scale + view->viewport_height / 2.0;
    // Clamp before casting: at deep zoom an off-screen point can be
    // astronomically many pixels away, and out-of-range double-to-int
    // conversion is undefined behavior.
    if (fx < -1e9) fx = -1e9;
    if (fx > 1e9) fx = 1e9;
    if (fy < -1e9) fy = -1e9;
    if (fy > 1e9) fy = 1e9;
    *px = (int)fx;
    *py = (int)fy;
}

// Get the current scale (complex units per pixel) as a double.
// SATURATES below ~1e-303: only valid for shallow-zoom code paths
// (zoom < MB_DEEP_ZOOM_THRESHOLD); deep paths must use the FloatExp variant.
static inline double mb_view_get_scale(const MBViewState *view) {
    return (2.0 / view->viewport_height) / view->zoom_level;
}

// Scale as extended-exponent value — exact at any zoom depth.
static inline FloatExp mb_view_get_scale_fx(const MBViewState *view) {
    FloatExp inv_zoom = fx_from_log10(-mb_view_zoom_log10(view));
    return fx_mul_d(inv_zoom, 2.0 / view->viewport_height);
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
