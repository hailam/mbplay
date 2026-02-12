#ifndef MB_GPU_H
#define MB_GPU_H

#include "../config.h"
#include "../perturbation/perturbation.h"
#include <stdbool.h>
#include <stdint.h>

// =============================================================================
// GPU Compute API (Metal via CMT)
// =============================================================================
//
// This module provides GPU-accelerated Mandelbrot computation via Metal.
// There are several APIs depending on use case and zoom level:
//
// 1. BASIC API (gpu_compute_row, gpu_compute_full)
//    - Float precision, direct computation
//    - Use for batch rendering at standard zoom levels (zoom < 1e6)
//
// 2. TILE API (gpu_compute_tile)
//    - Float precision, tile-based
//    - Use for interactive viewer at standard zoom levels
//
// 3. PERTURBATION V1 API (gpu_compute_tile_perturb)
//    - Uses perturbation theory with float deltas
//    - Deprecated: use V2 for better precision at deep zoom
//
// 4. PERTURBATION V2 API (gpu_compute_tile_perturb_v2)
//    - Uses perturbation theory with double-precision deltas
//    - Pre-computes deltas on CPU, runs iteration on GPU
//    - Use for deep zoom (1e6 < zoom < 1e12)
//
// 5. HIGH-PRECISION API (gpu_precompute_deltas_hp + V2 compute)
//    - Uses arbitrary precision for delta computation
//    - Supports zoom levels beyond double precision limits
//    - Use for extreme deep zoom (zoom > 1e12)
//

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

// =============================================================================
// Tile-based Compute API (for interactive viewer)
// =============================================================================

/**
 * Parameters for tile-based GPU computation.
 */
typedef struct {
    float center_x;     // View center X in complex plane
    float center_y;     // View center Y in complex plane
    float scale;        // Complex units per pixel
    uint32_t tile_x;    // Tile X offset in pixels
    uint32_t tile_y;    // Tile Y offset in pixels
    uint32_t tile_size; // Tile dimension (typically 256)
    uint32_t max_iter;  // Maximum iterations
    uint32_t vp_half_w; // Half viewport width
    uint32_t vp_half_h; // Half viewport height
} GPUTileParams;

/**
 * Initialize GPU for tile-based rendering.
 * @param tile_size Size of tiles (typically 256)
 * @param max_iter Maximum iterations
 * @return 0 on success, -1 on failure
 */
int gpu_init_tiles(int tile_size, int max_iter);

/**
 * Compute a single tile on GPU using float precision.
 * @param params Tile parameters
 * @param output Output buffer (tile_size * tile_size pixels)
 */
void gpu_compute_tile(const GPUTileParams *params, PixelColor *output);

/**
 * Check if GPU tile compute is initialized.
 * @return true if initialized
 */
bool gpu_tiles_initialized(void);

// =============================================================================
// Perturbation-based Compute API (for deep zoom)
// =============================================================================

/**
 * Parameters for perturbation-based GPU computation.
 */
typedef struct {
    float center_x;     // View center X in complex plane
    float center_y;     // View center Y in complex plane
    float scale;        // Complex units per pixel
    float ref_cx;       // Reference point C real
    float ref_cy;       // Reference point C imaginary
    uint32_t tile_x;    // Tile X offset in pixels
    uint32_t tile_y;    // Tile Y offset in pixels
    uint32_t tile_size; // Tile dimension (typically 256)
    uint32_t max_iter;  // Maximum iterations
    uint32_t ref_escape_iter;  // When reference orbit escaped
    uint32_t vp_half_w; // Half viewport width
    uint32_t vp_half_h; // Half viewport height
} GPUPerturbTileParams;

/**
 * Initialize GPU for perturbation-based rendering.
 * @param max_iter Maximum iterations (for reference orbit buffer)
 * @return 0 on success, -1 on failure
 */
int gpu_init_perturbation(uint32_t max_iter);

/**
 * Upload reference orbit to GPU.
 * @param orbit The computed reference orbit
 */
void gpu_update_reference_orbit(const ReferenceOrbit *orbit);

/**
 * Compute a single tile using perturbation theory.
 * Output iterations buffer may contain 0xFFFFFFFE for glitched pixels.
 * @param params Tile parameters including reference info
 * @param output Output buffer (tile_size * tile_size pixels)
 * @param iterations Optional: raw iteration output (for glitch detection)
 */
void gpu_compute_tile_perturb(const GPUPerturbTileParams *params,
                              PixelColor *output, uint32_t *iterations);

/**
 * Check if perturbation compute is initialized.
 * @return true if initialized
 */
bool gpu_perturbation_initialized(void);

// =============================================================================
// Perturbation V2 API (Pre-computed deltas for precision fix)
// =============================================================================

/**
 * Parameters for perturbation V2 computation (deltas pre-computed on CPU).
 */
typedef struct {
    uint32_t tile_size;      // Tile dimension (typically 256)
    uint32_t max_iter;       // Maximum iterations
    uint32_t ref_escape_iter; // When reference orbit escaped
} GPUPerturbParamsV2;

/**
 * Pre-compute deltas on CPU in double precision.
 * Deltas are stored in double precision for GPU computation.
 *
 * @param center_x View center X (double)
 * @param center_y View center Y (double)
 * @param scale Complex units per pixel (double)
 * @param ref_cx Reference point C real (double)
 * @param ref_cy Reference point C imaginary (double)
 * @param tile_size Tile dimension
 * @param vp_half_w Half viewport width
 * @param vp_half_h Half viewport height
 * @param tile_px Tile X offset in pixels
 * @param tile_py Tile Y offset in pixels
 * @param delta_buffer Output buffer for double2 deltas (tile_size^2 * 2 doubles)
 */
void gpu_precompute_deltas(double center_x, double center_y, double scale,
                           double ref_cx, double ref_cy,
                           uint32_t tile_size, int vp_half_w, int vp_half_h,
                           uint32_t tile_px, uint32_t tile_py,
                           double *delta_buffer);

/**
 * Compute a tile using perturbation V2 with pre-computed deltas.
 * @param params V2 tile parameters
 * @param deltas Pre-computed delta buffer (from gpu_precompute_deltas), double precision
 * @param output Output pixel buffer
 * @param iterations Optional: raw iteration output (for glitch detection)
 */
void gpu_compute_tile_perturb_v2(const GPUPerturbParamsV2 *params,
                                  const double *deltas,
                                  PixelColor *output, uint32_t *iterations);

// =============================================================================
// High-Precision Delta Computation API
// =============================================================================

/**
 * Pre-compute deltas on CPU using arbitrary precision math.
 * This is for deep zoom levels where double precision is insufficient
 * for accurate center coordinate representation.
 *
 * @param center_x_str View center X as decimal string
 * @param center_y_str View center Y as decimal string
 * @param ref_cx_str Reference point C real as decimal string
 * @param ref_cy_str Reference point C imaginary as decimal string
 * @param precision Bits of precision to use (128, 256, 512, etc.)
 * @param scale Complex units per pixel (double precision OK)
 * @param tile_size Tile dimension
 * @param vp_half_w Half viewport width
 * @param vp_half_h Half viewport height
 * @param tile_px Tile X offset in pixels
 * @param tile_py Tile Y offset in pixels
 * @param delta_buffer Output buffer for double2 deltas (tile_size^2 * 2 doubles)
 */
void gpu_precompute_deltas_hp(
    const char *center_x_str, const char *center_y_str,
    const char *ref_cx_str, const char *ref_cy_str,
    uint32_t precision,
    double scale,
    uint32_t tile_size, int vp_half_w, int vp_half_h,
    uint32_t tile_px, uint32_t tile_py,
    double *delta_buffer);

#endif // MB_GPU_H
