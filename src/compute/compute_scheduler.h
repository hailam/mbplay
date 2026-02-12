#ifndef MB_COMPUTE_SCHEDULER_H
#define MB_COMPUTE_SCHEDULER_H

#include "../config.h"
#include "../tile_cache/tile_cache.h"
#include "../tile_cache/disk_cache.h"
#include "../tile_map/tile_map.h"
#include "../perturbation/perturbation.h"
#include <stdbool.h>

// =============================================================================
// Compute Scheduler API
// =============================================================================

typedef struct {
    TileCache cache;
    DiskCache *disk_cache;   // Persistent storage for map tiles
    int tile_size;
    int max_iter;
    bool gpu_available;

    // Perturbation state (standard double precision)
    ReferenceOrbit ref_orbit;
    bool perturbation_enabled;
    double last_ref_cx, last_ref_cy;
    double last_scale;

    // High-precision perturbation state
    ReferenceOrbitHP ref_orbit_hp;
    bool high_precision_mode;
    uint32_t current_precision;          // Current precision tier (bits)
    char last_ref_cx_str[MB_HP_COORD_STR_LEN];
    char last_ref_cy_str[MB_HP_COORD_STR_LEN];

    // Reusable buffers (avoid malloc per tile)
    double *delta_buffer;    // Pre-computed deltas for V2 API (double precision)
    uint32_t *iter_buffer;   // Iteration output for glitch detection
} ComputeScheduler;

/**
 * Initialize the compute scheduler.
 * @param sched Scheduler to initialize
 * @param tile_size Size of tiles
 * @param max_iter Maximum iterations
 * @return 0 on success, -1 on failure
 */
int scheduler_init(ComputeScheduler *sched, int tile_size, int max_iter);

/**
 * Update scheduler for new view state.
 * Invalidates cache if zoom level crosses precision threshold.
 * @param sched The scheduler
 * @param view Current view state
 */
void scheduler_update_view(ComputeScheduler *sched, const MBViewState *view);

/**
 * Request a tile to be computed.
 * Returns cached tile if available, otherwise computes it.
 * @param sched The scheduler
 * @param view Current view state
 * @param tile_x Tile X index (in tile units)
 * @param tile_y Tile Y index (in tile units)
 * @param output Output buffer (tile_size * tile_size pixels)
 * @return true if tile was retrieved/computed, false on error
 */
bool scheduler_get_tile(ComputeScheduler *sched, const MBViewState *view,
                        int tile_x, int tile_y, PixelColor *output);

/**
 * Clean up scheduler resources.
 * @param sched The scheduler
 */
void scheduler_cleanup(ComputeScheduler *sched);

// =============================================================================
// Disk Cache API
// =============================================================================

/**
 * Initialize disk cache for persistent map tile storage.
 * @param sched The scheduler
 * @param cache_path Base path for tile cache (e.g., ~/.mandelbrot/tiles)
 * @param max_size_bytes Maximum cache size (0 for default 1GB)
 * @return 0 on success, -1 on failure
 */
int scheduler_init_disk_cache(ComputeScheduler *sched, const char *cache_path,
                              int64_t max_size_bytes);

#endif // MB_COMPUTE_SCHEDULER_H
