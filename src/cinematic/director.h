#ifndef MB_CINE_DIRECTOR_H
#define MB_CINE_DIRECTOR_H

#include "../config.h"
#include <stdint.h>
#include <stdbool.h>

// =============================================================================
// Cinematic Director — keyframe pipeline for the autopilot zoom
// =============================================================================
//
// Per-frame rendering cannot hold a smooth cinematic zoom; instead the
// director renders KEYFRAMES (full offscreen frames, one per zoom doubling
// along the dive path) ahead of the camera, and playback composites the two
// keyframes bracketing the current zoom (shallower one upscaled <=2x, deeper
// one as the sharp central inset). See docs/CINEMATIC_DESIGN.md.
//
// Steering: before each keyframe the director probes iteration counts around
// the path center and nudges the center toward the highest *finite* count —
// the closest approach to the boundary — keeping the dive interesting
// forever (the boundary is infinitely deep everywhere).
//
// Threading: rendering happens in mb_director_render_next (call from ONE
// background thread/queue); playback reads via the lock/unlock pair from the
// UI thread. All other calls are UI-thread-only.

typedef struct MBDirector MBDirector;

typedef struct {
    PixelColor *pixels;   // width x height, screen layout
    MBViewState view;     // snapshot the keyframe was rendered with
    int index;            // keyframe index k: zoom_log10 = k * log10(2)
    bool ready;
} MBCineKeyframe;

// Zoom step between keyframes (one doubling)
#define MB_CINE_STEP 0.30102999566398120   // log10(2)
// Keyframes kept/rendered ahead of playback
#define MB_CINE_SLOTS 8
#define MB_CINE_AHEAD 5

/** Create a director for a fixed viewport size. NULL on failure. */
MBDirector *mb_director_create(int width, int height,
                               const MBRenderSettings *settings);

void mb_director_destroy(MBDirector *d);

/**
 * (Re)start the dive from a view (center strings + zoom). Clears keyframes.
 */
void mb_director_start(MBDirector *d, const MBViewState *seed);

/**
 * Render the next keyframe (blocking; up to seconds at extreme depth).
 * Returns the keyframe index rendered, or -1 when the pipeline is already
 * MB_CINE_AHEAD frames past playback (nothing to do) or on error.
 * Call repeatedly from one background queue while the mode is active.
 */
int mb_director_render_next(MBDirector *d);

/**
 * Playback: fetch the keyframes bracketing zoom 10^z (lo at floor, hi at
 * floor+1; either may be NULL if not rendered yet). The director stays
 * locked until mb_director_unlock_frames — composite, then unlock promptly.
 * Also records the playback position for eviction/scheduling.
 */
void mb_director_lock_frames(MBDirector *d, double zoom_log10,
                             const MBCineKeyframe **lo, const MBCineKeyframe **hi);

void mb_director_unlock_frames(MBDirector *d);

/** Highest zoom_log10 the pipeline can currently play back smoothly. */
double mb_director_ready_log10(MBDirector *d);

#endif // MB_CINE_DIRECTOR_H
