#include "compute_scheduler.h"
#include "../gpu/gpu.h"
#include "../mandelbrot/mandelbrot.h"
#include "../color/color.h"
#include "../tile_map/tile_map.h"
#include "../tile_cache/disk_cache.h"
#include <string.h>
#include <stdlib.h>
#include <math.h>

// Glitch marker value
#define MB_GLITCH_MARKER 0xFFFFFFFE

// =============================================================================
// Scheduler Implementation
// =============================================================================

int scheduler_init(ComputeScheduler *sched, int tile_size, int max_iter) {
    memset(sched, 0, sizeof(ComputeScheduler));

    sched->tile_size = tile_size;
    sched->max_iter = max_iter;
    sched->using_double = false;
    sched->perturbation_enabled = false;
    sched->last_ref_cx = 0.0;
    sched->last_ref_cy = 0.0;
    sched->last_scale = 0.0;

    // Initialize tile cache
    if (tile_cache_init(&sched->cache) != 0) {
        return -1;
    }

    // Allocate reusable buffers
    size_t tilePixels = (size_t)tile_size * tile_size;
    sched->delta_buffer = (float *)malloc(tilePixels * 2 * sizeof(float));
    sched->iter_buffer = (uint32_t *)malloc(tilePixels * sizeof(uint32_t));
    if (!sched->delta_buffer || !sched->iter_buffer) {
        scheduler_cleanup(sched);
        return -1;
    }

    // Try to initialize GPU
    sched->gpu_available = (gpu_init_tiles(tile_size, max_iter) == 0);

    // Initialize perturbation if GPU is available
    if (sched->gpu_available) {
        if (ref_orbit_init(&sched->ref_orbit, MB_REF_ORBIT_MAX_ITER) == 0) {
            if (gpu_init_perturbation(MB_REF_ORBIT_MAX_ITER) == 0) {
                sched->perturbation_enabled = true;
            }
        }
    }

    return 0;
}

void scheduler_update_view(ComputeScheduler *sched, const MBViewState *view) {
    double scale = mb_view_get_scale(view);

    // Check if reference orbit needs recomputation
    // Recompute if: center moved significantly, or scale changed significantly
    if (sched->perturbation_enabled) {
        double center_dist = fabs(view->center_x - sched->last_ref_cx) +
                            fabs(view->center_y - sched->last_ref_cy);
        double scale_ratio = sched->last_scale > 0 ? scale / sched->last_scale : 0;

        // Recompute if center moved more than 50% of viewport, or scale changed >50%
        // Relaxed threshold - V2 API handles precision correctly so we don't need
        // to recompute reference as often
        bool need_recompute = !sched->ref_orbit.valid ||
                             center_dist > scale * view->viewport_width * 0.5 ||
                             scale_ratio < 0.5 || scale_ratio > 2.0;

        if (need_recompute) {
            // Compute reference at view center
            ref_orbit_compute(&sched->ref_orbit, view->center_x, view->center_y);
            gpu_update_reference_orbit(&sched->ref_orbit);

            sched->last_ref_cx = view->center_x;
            sched->last_ref_cy = view->center_y;
            sched->last_scale = scale;

            // NOTE: No cache invalidation on reference change.
            // With V2 API, deltas are computed per-tile with the current
            // reference, so cached tiles remain valid.
        }
    }

    // Legacy double precision check (kept for fallback)
    bool needs_double = mb_view_needs_double(view);
    if (needs_double != sched->using_double) {
        if (!sched->perturbation_enabled) {
            tile_cache_new_generation(&sched->cache);
        }
        sched->using_double = needs_double;
    }
}

// Handle glitched pixels with CPU double-precision fallback
static void handle_glitches(ComputeScheduler *sched, const MBViewState *view,
                           uint32_t *iterations, int tile_x_px, int tile_y_px,
                           PixelColor *output) {
    double scale = mb_view_get_scale(view);
    int vp_half_w = view->viewport_width / 2;
    int vp_half_h = view->viewport_height / 2;

    for (int ly = 0; ly < sched->tile_size; ly++) {
        for (int lx = 0; lx < sched->tile_size; lx++) {
            size_t idx = (size_t)ly * sched->tile_size + lx;
            if (iterations[idx] == MB_GLITCH_MARKER) {
                // Compute with CPU double precision
                double px = (double)(tile_x_px + lx);
                double py = (double)(tile_y_px + ly);
                double cx = view->center_x + (px - vp_half_w) * scale;
                double cy = view->center_y + (py - vp_half_h) * scale;

                // Full iteration (no cardioid check since we're at deep zoom)
                double zx = 0.0, zy = 0.0;
                double zx2 = 0.0, zy2 = 0.0;
                unsigned int iteration = 0;

                while (zx2 + zy2 < 4.0 && iteration < (unsigned int)sched->max_iter) {
                    zy = 2.0 * zx * zy + cy;
                    zx = zx2 - zy2 + cx;
                    zx2 = zx * zx;
                    zy2 = zy * zy;
                    iteration++;
                }

                color_from_iteration(&output[idx], iteration);
            }
        }
    }
}

bool scheduler_get_tile(ComputeScheduler *sched, const MBViewState *view,
                        int tile_x, int tile_y, PixelColor *output) {
    uint32_t gen = tile_cache_get_generation(&sched->cache);

    // Check cache first
    TileKey key = {
        .tile_x = tile_x,
        .tile_y = tile_y,
        .generation = gen
    };

    CachedTile *cached = tile_cache_get(&sched->cache, &key);
    if (cached) {
        // Cache hit - copy pixels
        size_t pixels = (size_t)sched->tile_size * sched->tile_size;
        memcpy(output, cached->pixels, pixels * sizeof(PixelColor));
        return true;
    }

    // Cache miss - compute tile
    double scale = mb_view_get_scale(view);
    int vp_half_w = view->viewport_width / 2;
    int vp_half_h = view->viewport_height / 2;

    // Pixel offset for this tile
    int px = tile_x * sched->tile_size;
    int py = tile_y * sched->tile_size;

    // Use perturbation V2 if enabled and reference is valid
    if (sched->perturbation_enabled && sched->ref_orbit.valid) {
        // Pre-compute deltas on CPU (double precision)
        gpu_precompute_deltas(view->center_x, view->center_y, scale,
                              sched->ref_orbit.ref_cx, sched->ref_orbit.ref_cy,
                              (uint32_t)sched->tile_size, vp_half_w, vp_half_h,
                              (uint32_t)px, (uint32_t)py, sched->delta_buffer);

        // Compute with pre-computed deltas (V2 API)
        GPUPerturbParamsV2 params = {
            .tile_size = (uint32_t)sched->tile_size,
            .max_iter = (uint32_t)sched->max_iter,
            .ref_escape_iter = sched->ref_orbit.escape_iter
        };

        gpu_compute_tile_perturb_v2(&params, sched->delta_buffer,
                                    output, sched->iter_buffer);

        // Handle glitched pixels with CPU fallback
        handle_glitches(sched, view, sched->iter_buffer, px, py, output);
    } else if (!sched->gpu_available) {
        // Use CPU double precision
        mb_compute_tile_double(
            view->center_x, view->center_y, scale,
            px, py, sched->tile_size,
            vp_half_w, vp_half_h,
            sched->max_iter, output
        );
    } else {
        // Use standard GPU float precision (low zoom, perturbation not needed)
        GPUTileParams params = {
            .center_x = (float)view->center_x,
            .center_y = (float)view->center_y,
            .scale = (float)scale,
            .tile_x = (uint32_t)px,
            .tile_y = (uint32_t)py,
            .tile_size = (uint32_t)sched->tile_size,
            .max_iter = (uint32_t)sched->max_iter,
            .vp_half_w = (uint32_t)vp_half_w,
            .vp_half_h = (uint32_t)vp_half_h
        };
        gpu_compute_tile(&params, output);
    }

    // Store in cache
    tile_cache_put(&sched->cache, &key, output);

    return true;
}

void scheduler_get_visible_tiles(const MBViewState *view, int tile_size,
                                 int *out_start_x, int *out_start_y,
                                 int *out_end_x, int *out_end_y) {
    // Get bounds in tile coordinates
    // We compute one extra tile on each edge for smooth scrolling
    int start_x = -1;  // Start before viewport for smooth scrolling
    int start_y = -1;
    int end_x = (view->viewport_width + tile_size - 1) / tile_size + 1;
    int end_y = (view->viewport_height + tile_size - 1) / tile_size + 1;

    *out_start_x = start_x;
    *out_start_y = start_y;
    *out_end_x = end_x;
    *out_end_y = end_y;
}

bool scheduler_using_double(const ComputeScheduler *sched) {
    return sched->using_double;
}

void scheduler_cleanup(ComputeScheduler *sched) {
    tile_cache_cleanup(&sched->cache);
    ref_orbit_cleanup(&sched->ref_orbit);
    if (sched->gpu_available) {
        gpu_cleanup();
    }
    if (sched->delta_buffer) {
        free(sched->delta_buffer);
        sched->delta_buffer = NULL;
    }
    if (sched->iter_buffer) {
        free(sched->iter_buffer);
        sched->iter_buffer = NULL;
    }
    if (sched->disk_cache) {
        disk_cache_cleanup(sched->disk_cache);
        sched->disk_cache = NULL;
    }
    memset(sched, 0, sizeof(ComputeScheduler));
}

// =============================================================================
// Map Tile API Implementation
// =============================================================================

int scheduler_init_disk_cache(ComputeScheduler *sched, const char *cache_path,
                              int64_t max_size_bytes) {
    if (!sched || !cache_path) return -1;

    sched->disk_cache = disk_cache_init(cache_path, max_size_bytes);
    return sched->disk_cache ? 0 : -1;
}

bool scheduler_get_map_tile(ComputeScheduler *sched, const MapTile *tile,
                            PixelColor *output) {
    if (!sched || !tile || !output) return false;

    // 1. Check disk cache first (if zoom <= 20)
    if (tile->zoom <= MB_DISK_CACHE_MAX_ZOOM && sched->disk_cache) {
        if (disk_cache_get(sched->disk_cache, tile, output) == 0) {
            return true;  // Cache hit!
        }
    }

    // 2. Compute tile bounds
    double min_cx, max_cx, min_cy, max_cy;
    mb_tile_to_bounds(tile, &min_cx, &max_cx, &min_cy, &max_cy);

    // 3. Create view state for this tile
    MBViewState tile_view;
    tile_view.center_x = (min_cx + max_cx) / 2.0;
    tile_view.center_y = (min_cy + max_cy) / 2.0;
    tile_view.viewport_width = MB_MAP_TILE_SIZE;
    tile_view.viewport_height = MB_MAP_TILE_SIZE;

    // Zoom level = 1 / scale (units per pixel)
    // scale = tile_width / tile_pixels
    double tile_width = max_cx - min_cx;
    double scale = tile_width / MB_MAP_TILE_SIZE;
    tile_view.zoom_level = (2.0 / tile_view.viewport_height) / scale;

    // 4. Update reference if needed
    scheduler_update_view(sched, &tile_view);

    // 5. Compute tile using existing infrastructure
    // The tile is at position (0,0) in tile coordinates for this view
    scheduler_get_tile(sched, &tile_view, 0, 0, output);

    // 6. Save to disk cache
    if (tile->zoom <= MB_DISK_CACHE_MAX_ZOOM && sched->disk_cache) {
        disk_cache_put(sched->disk_cache, tile, output);
    }

    return true;
}
