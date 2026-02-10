#ifndef MB_COMPUTE_SCHEDULER_H
#define MB_COMPUTE_SCHEDULER_H

#include "../config.h"
#include "../tile_cache/tile_cache.h"
#include "../perturbation/perturbation.h"
#include <stdbool.h>

// =============================================================================
// Compute Scheduler API
// =============================================================================

typedef struct {
    TileCache cache;
    int tile_size;
    int max_iter;
    bool gpu_available;
    bool using_double;  // True when zoom >= MB_FLOAT_ZOOM_LIMIT (legacy, unused with perturbation)

    // Perturbation state
    ReferenceOrbit ref_orbit;
    bool perturbation_enabled;
    double last_ref_cx, last_ref_cy;
    double last_scale;
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
 * Get tiles visible in current view.
 * @param view Current view state
 * @param tile_size Size of tiles
 * @param out_start_x Output: first visible tile X
 * @param out_start_y Output: first visible tile Y
 * @param out_end_x Output: last visible tile X (exclusive)
 * @param out_end_y Output: last visible tile Y (exclusive)
 */
void scheduler_get_visible_tiles(const MBViewState *view, int tile_size,
                                 int *out_start_x, int *out_start_y,
                                 int *out_end_x, int *out_end_y);

/**
 * Check if currently using double precision.
 * @param sched The scheduler
 * @return true if using CPU double precision
 */
bool scheduler_using_double(const ComputeScheduler *sched);

/**
 * Clean up scheduler resources.
 * @param sched The scheduler
 */
void scheduler_cleanup(ComputeScheduler *sched);

#endif // MB_COMPUTE_SCHEDULER_H
