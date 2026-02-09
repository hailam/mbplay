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

// =============================================================================
// Image Dimensions
// =============================================================================

#define MB_WIDTH 19200
#define MB_HEIGHT 10800
#define MB_MAX_ITER 1000

// =============================================================================
// Tile-based Rendering
// =============================================================================

#define MB_TILE_SIZE 64
#define MB_NUM_TILES_X ((MB_WIDTH + MB_TILE_SIZE - 1) / MB_TILE_SIZE)
#define MB_NUM_TILES_Y ((MB_HEIGHT + MB_TILE_SIZE - 1) / MB_TILE_SIZE)
#define MB_TOTAL_TILES (MB_NUM_TILES_X * MB_NUM_TILES_Y)

// =============================================================================
// Pre-computed Scale Constants
// =============================================================================

#define MB_CX_SCALE (3.5 / MB_WIDTH)
#define MB_CY_SCALE (2.0 / MB_HEIGHT)
#define MB_WIDTH_HALF (MB_WIDTH / 2.0)
#define MB_HEIGHT_HALF (MB_HEIGHT / 2.0)

// =============================================================================
// Pixel Color Type
// =============================================================================

typedef struct {
  unsigned char r, g, b;
} PixelColor;

#endif // MB_CONFIG_H
