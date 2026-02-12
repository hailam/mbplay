#include "compute_scheduler.h"
#include "../gpu/gpu.h"
#include "../mandelbrot/mandelbrot.h"
#include "../color/color.h"
#include "../tile_map/tile_map.h"
#include "../tile_cache/disk_cache.h"
#include "../precision/mp_real.h"
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
    sched->perturbation_enabled = false;
    sched->high_precision_mode = false;
    sched->current_precision = 64;
    sched->last_ref_cx = 0.0;
    sched->last_ref_cy = 0.0;
    sched->last_scale = 0.0;
    sched->last_ref_cx_str[0] = '\0';
    sched->last_ref_cy_str[0] = '\0';

    // Initialize tile cache
    if (tile_cache_init(&sched->cache) != 0) {
        return -1;
    }

    // Allocate reusable buffers
    size_t tilePixels = (size_t)tile_size * tile_size;
    sched->delta_buffer = (double *)malloc(tilePixels * 2 * sizeof(double));
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

        // Initialize HP reference orbit with default precision
        // It will be re-initialized with higher precision as needed
        if (ref_orbit_hp_init(&sched->ref_orbit_hp, MB_REF_ORBIT_MAX_ITER, MB_PREC_TIER_1) == 0) {
            // HP orbit initialized successfully
        }
    }

    return 0;
}

// Calculate required precision tier based on zoom level
static uint32_t get_precision_tier(double zoom_level) {
    // log10(zoom) determines number of decimal digits needed
    // Each decimal digit needs ~3.32 bits
    if (zoom_level < MB_HP_ZOOM_THRESHOLD) {
        return 64;  // Standard double precision
    }

    double log_zoom = log10(zoom_level);
    if (log_zoom < 30) return MB_PREC_TIER_1;      // 128 bits
    if (log_zoom < 60) return MB_PREC_TIER_2;      // 256 bits
    if (log_zoom < 120) return MB_PREC_TIER_3;     // 512 bits
    return MB_PREC_TIER_4;                          // 1024 bits
}

void scheduler_update_view(ComputeScheduler *sched, const MBViewState *view) {
    double scale = mb_view_get_scale(view);
    bool needs_hp = mb_view_needs_high_precision(view);

    // Check if we're transitioning between HP and standard mode
    if (needs_hp != sched->high_precision_mode) {
        sched->high_precision_mode = needs_hp;
        // Invalidate cache on mode transition
        tile_cache_new_generation(&sched->cache);
    }

    if (needs_hp) {
        // High-precision mode
        uint32_t required_prec = get_precision_tier(view->zoom_level);

        // Check if we need to upgrade precision
        if (required_prec > sched->current_precision) {
            // Reinitialize HP orbit with higher precision
            ref_orbit_hp_cleanup(&sched->ref_orbit_hp);
            ref_orbit_hp_init(&sched->ref_orbit_hp, MB_REF_ORBIT_MAX_ITER, required_prec);
            sched->current_precision = required_prec;
            sched->ref_orbit_hp.valid = false;
        }

        // Check if HP reference orbit needs recomputation
        bool need_recompute = !sched->ref_orbit_hp.valid ||
                             strcmp(view->center_x_str, sched->last_ref_cx_str) != 0 ||
                             strcmp(view->center_y_str, sched->last_ref_cy_str) != 0;

        if (need_recompute) {
            // Compute HP reference at view center
            ref_orbit_hp_compute(&sched->ref_orbit_hp,
                                 view->center_x_str, view->center_y_str);

            // Copy HP orbit data to standard ref_orbit struct for GPU upload
            // (ref_orbit owns its arrays, we copy data into them)
            sched->ref_orbit.ref_cx = sched->ref_orbit_hp.ref_cx;
            sched->ref_orbit.ref_cy = sched->ref_orbit_hp.ref_cy;
            sched->ref_orbit.escape_iter = sched->ref_orbit_hp.escape_iter;

            // Copy the orbit history (computed in HP, stored as doubles)
            uint32_t copy_count = sched->ref_orbit_hp.escape_iter;
            if (copy_count > sched->ref_orbit.max_iter) {
                copy_count = sched->ref_orbit.max_iter;
            }
            for (uint32_t i = 0; i < copy_count; i++) {
                sched->ref_orbit.z_real[i] = sched->ref_orbit_hp.z_real[i];
                sched->ref_orbit.z_imag[i] = sched->ref_orbit_hp.z_imag[i];
            }
            sched->ref_orbit.valid = sched->ref_orbit_hp.valid;

            gpu_update_reference_orbit(&sched->ref_orbit);

            strncpy(sched->last_ref_cx_str, view->center_x_str, MB_HP_COORD_STR_LEN - 1);
            sched->last_ref_cx_str[MB_HP_COORD_STR_LEN - 1] = '\0';
            strncpy(sched->last_ref_cy_str, view->center_y_str, MB_HP_COORD_STR_LEN - 1);
            sched->last_ref_cy_str[MB_HP_COORD_STR_LEN - 1] = '\0';
            sched->last_ref_cx = sched->ref_orbit_hp.ref_cx;
            sched->last_ref_cy = sched->ref_orbit_hp.ref_cy;
            sched->last_scale = scale;
        }
    } else if (sched->perturbation_enabled) {
        // Standard double precision mode
        double center_dist = fabs(view->center_x - sched->last_ref_cx) +
                            fabs(view->center_y - sched->last_ref_cy);
        double scale_ratio = sched->last_scale > 0 ? scale / sched->last_scale : 0;

        // Recompute if center moved more than 50% of viewport, or scale changed >50%
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
        }
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

    // tile_cache_get copies pixels under lock (thread-safe)
    if (tile_cache_get(&sched->cache, &key, output) == 0) {
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
        // Pre-compute deltas on CPU
        if (sched->high_precision_mode && sched->ref_orbit_hp.valid) {
            // High-precision mode: use MPFR for accurate delta computation
            gpu_precompute_deltas_hp(
                view->center_x_str, view->center_y_str,
                sched->ref_orbit_hp.ref_cx_str, sched->ref_orbit_hp.ref_cy_str,
                sched->current_precision, scale,
                (uint32_t)sched->tile_size, vp_half_w, vp_half_h,
                (uint32_t)px, (uint32_t)py, sched->delta_buffer);
        } else {
            // Standard double precision delta computation
            gpu_precompute_deltas(view->center_x, view->center_y, scale,
                                  sched->ref_orbit.ref_cx, sched->ref_orbit.ref_cy,
                                  (uint32_t)sched->tile_size, vp_half_w, vp_half_h,
                                  (uint32_t)px, (uint32_t)py, sched->delta_buffer);
        }

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

void scheduler_cleanup(ComputeScheduler *sched) {
    tile_cache_cleanup(&sched->cache);
    ref_orbit_cleanup(&sched->ref_orbit);
    ref_orbit_hp_cleanup(&sched->ref_orbit_hp);
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
// Disk Cache API Implementation
// =============================================================================

int scheduler_init_disk_cache(ComputeScheduler *sched, const char *cache_path,
                              int64_t max_size_bytes) {
    if (!sched || !cache_path) return -1;

    sched->disk_cache = disk_cache_init(cache_path, max_size_bytes);
    return sched->disk_cache ? 0 : -1;
}
