#ifndef MB_RENDER_H
#define MB_RENDER_H

#include "../config.h"

// =============================================================================
// Rendering Context
// =============================================================================

typedef struct {
  PixelColor *image_data;
  int num_threads;
} RenderContext;

// =============================================================================
// Rendering API
// =============================================================================

/**
 * Initialize the rendering system.
 * Allocates internal resources for tile-based threaded rendering.
 *
 * @param ctx Render context to initialize
 * @param image_data Pointer to image buffer (WIDTH * HEIGHT pixels)
 * @param num_threads Number of worker threads (0 = auto-detect from CPU cores)
 * @return 0 on success, non-zero on error
 */
int render_init(RenderContext *ctx, PixelColor *image_data, int num_threads);

/**
 * Execute the rendering.
 * Distributes tiles across worker threads using atomic work-stealing.
 *
 * @param ctx Initialized render context
 */
void render_execute(RenderContext *ctx);

/**
 * Clean up the rendering system.
 * Frees internal resources allocated by render_init.
 *
 * @param ctx Render context to clean up
 */
void render_cleanup(RenderContext *ctx);

/**
 * Print SIMD status message to stdout.
 */
void render_print_simd_status(void);

#endif // MB_RENDER_H
