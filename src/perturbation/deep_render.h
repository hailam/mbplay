#ifndef MB_DEEP_RENDER_H
#define MB_DEEP_RENDER_H

#include "../config.h"
#include <stdint.h>

// =============================================================================
// Deep-Zoom Screen-Tile Renderer
// =============================================================================
//
// Past MB_DEEP_ZOOM_THRESHOLD the z/x/y map-tile pyramid cannot address
// tile bounds in double precision, so the viewer switches to rendering
// screen-space tiles with perturbation:
//
//   - One reference orbit per view generation, computed with MPFR at
//     mp_required_precision(zoom) bits from the high-precision center
//     strings, stored as doubles.
//   - Per-pixel deltas are exact doubles (pixel offsets times scale, plus a
//     once-per-generation MPFR-computed offset between the view center and
//     the reference point, which lets the orbit be reused across pans).
//   - Delta iteration runs in double with rebasing (see perturb_cpu.h), so
//     there are no glitches to repair.
//
// The renderer is safe to call from concurrent GCD queues: the orbit is a
// refcounted immutable snapshot, rebuilt under a mutex when a newer view
// generation arrives or the reference drifts too far from the view center.

typedef struct MBDeepRenderer MBDeepRenderer;

/** Create a renderer. Returns NULL on allocation failure. */
MBDeepRenderer *mb_deep_renderer_create(void);

/** Destroy the renderer and its cached orbit. */
void mb_deep_renderer_destroy(MBDeepRenderer *r);

/**
 * Tell the renderer the view has moved on to `generation`. Cheap (one atomic
 * store), callable from the UI thread on every view change. Tiles rendering
 * for an older generation abort at their next row boundary instead of
 * finishing work nobody will display.
 */
void mb_deep_renderer_note_generation(MBDeepRenderer *r, uint64_t generation);

/** Result codes for mb_deep_renderer_render_tile. */
#define MB_DEEP_OK 0
#define MB_DEEP_STALE 1
#define MB_DEEP_ERROR (-1)

/**
 * Render one screen-space tile.
 *
 * @param r          The renderer.
 * @param view       Snapshot of the view state this job was queued for.
 *                   center_x_str/center_y_str are the authoritative center.
 * @param generation Monotonically increasing view-change counter. Jobs older
 *                   than the newest generation seen are skipped (MB_DEEP_STALE).
 * @param tile_px    Tile origin X in viewport pixels.
 * @param tile_py    Tile origin Y in viewport pixels (screen-down).
 * @param tile_size  Tile dimension in pixels.
 * @param max_iter   Iteration budget (use mb_max_iter_for_zoom).
 * @param settings   Render settings for coloring.
 * @param out        Output buffer, tile_size * tile_size pixels.
 * @return MB_DEEP_OK, MB_DEEP_STALE, or MB_DEEP_ERROR.
 */
int mb_deep_renderer_render_tile(MBDeepRenderer *r,
                                 const MBViewState *view, uint64_t generation,
                                 int tile_px, int tile_py, int tile_size,
                                 uint32_t max_iter,
                                 const MBRenderSettings *settings,
                                 PixelColor *out);

/**
 * Strided variant for progressive refinement: computes every stride-th pixel
 * (sampling block centers), writing a (tile_size/stride)^2 buffer. A coarse
 * stride-4 pass costs 1/16th of the full tile and is upscaled as a preview
 * while the full-resolution pass runs. stride must divide tile_size.
 */
int mb_deep_renderer_render_tile_strided(MBDeepRenderer *r,
                                         const MBViewState *view, uint64_t generation,
                                         int tile_px, int tile_py, int tile_size,
                                         int stride,
                                         uint32_t max_iter,
                                         const MBRenderSettings *settings,
                                         PixelColor *out);

/**
 * Probe variant: raw iteration counts instead of colors, for interest
 * scoring (e.g. the cinematic autopilot steering toward the boundary).
 * Writes a (tile_size/stride)^2 buffer of iteration counts; max_iter means
 * "did not escape".
 */
int mb_deep_renderer_probe_strided(MBDeepRenderer *r,
                                   const MBViewState *view, uint64_t generation,
                                   int tile_px, int tile_py, int tile_size,
                                   int stride,
                                   uint32_t max_iter,
                                   uint32_t *iters_out);

#endif // MB_DEEP_RENDER_H
