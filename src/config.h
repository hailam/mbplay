#ifndef MB_CONFIG_H
#define MB_CONFIG_H

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

#endif // MB_CONFIG_H
