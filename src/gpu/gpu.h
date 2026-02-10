#ifndef MB_GPU_H
#define MB_GPU_H

#include "../config.h"
#include <stdbool.h>

// =============================================================================
// GPU Compute API (Metal via CMT)
// =============================================================================

/**
 * Check if Metal GPU is available on this system.
 * @return true if GPU compute is available
 */
bool gpu_is_available(void);

/**
 * Initialize GPU compute resources.
 * Creates Metal device, command queue, compute pipeline, and buffers.
 *
 * @param cfg Runtime configuration with dimensions
 * @return 0 on success, -1 on failure
 */
int gpu_init(const MBConfig *cfg);

/**
 * Compute a single row of the Mandelbrot set on GPU.
 * For streaming output - computes one row at a time.
 *
 * @param y Row index
 * @param output Output buffer for one row (width pixels)
 */
void gpu_compute_row(int y, PixelColor *output);

/**
 * Compute entire Mandelbrot image on GPU.
 * For non-streaming output - computes full image at once.
 *
 * @param output Output buffer (width * height pixels)
 */
void gpu_compute_full(PixelColor *output);

/**
 * Clean up GPU resources.
 */
void gpu_cleanup(void);

#endif // MB_GPU_H
