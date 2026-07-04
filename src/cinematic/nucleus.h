#ifndef MB_CINE_NUCLEUS_H
#define MB_CINE_NUCLEUS_H

#include "../config.h"
#include <stdint.h>
#include <stdbool.h>

// =============================================================================
// Minibrot nucleus locator — the autopilot's deep-dive anchor
// =============================================================================
//
// Field-following steering chases what the probe can see, and at depth the
// deep structure is a hairline that recedes at the zoom rate: a greedy
// follower eventually loses it (verified repeatedly on live dives). The
// robust architecture — the one production zoomers use — is to LOCK a
// target: detect the near-period of the orbit at a promising point (the
// index of the orbit's |z| minimum), then Newton-solve z_p(c) = 0 for the
// superstable nucleus of the minibrot that owns that neighborhood. The
// nucleus is an exact, fixed coordinate: a camera flying toward it can
// never be outrun.

/**
 * Find the minibrot nucleus near a guess coordinate.
 *
 * @param guess_x/guess_y  HP decimal strings for the guess point
 * @param zoom_log10       current zoom depth (sets precision and tolerances)
 * @param min_period       ignore |z| dips at or below this index — used to
 *                         skip past an already-locked parent minibrot and
 *                         find the embedded one beyond it (0 = no floor)
 * @param max_period       orbit length to scan for the near-period
 * @param out_x/out_y      nucleus coordinate (MB_HP_COORD_STR_LEN buffers)
 * @param out_period       detected period
 * @return true on Newton convergence within ~one frame span of the guess
 */
bool mb_nucleus_find(const char *guess_x, const char *guess_y,
                     double zoom_log10,
                     uint32_t min_period, uint32_t max_period,
                     char out_x[MB_HP_COORD_STR_LEN],
                     char out_y[MB_HP_COORD_STR_LEN],
                     uint32_t *out_period);

#endif // MB_CINE_NUCLEUS_H
