#ifndef MB_VIEW_HP_H
#define MB_VIEW_HP_H

#include "../config.h"

// =============================================================================
// High-Precision View Center Operations
// =============================================================================
//
// Beyond zoom ~1e13 a double cannot even *identify* the view center: two
// adjacent pixels map to the same double. These helpers make the decimal
// strings in MBViewState (center_x_str / center_y_str) the source of truth,
// updating them with MPFR at a precision that scales with the zoom level.
// The double fields center_x / center_y are kept in sync as (lossy) display
// approximations. Beyond zoom ~1e300, offsets are passed as FloatExp because
// they no longer fit a double at all.
//
// Every mutation of the view center in the app must go through these
// functions; writing center_x/center_y directly and re-serializing with
// %.17g silently truncates the center back to double precision.

/**
 * Number of significant decimal digits needed to address a pixel at the
 * given zoom (plus guard digits). The log10 variant works at any depth.
 */
int mb_view_hp_digits(double zoom_level);
int mb_view_hp_digits_log10(double zoom_log10);

/**
 * Normalize the state so strings and doubles agree. Call once after
 * mb_view_state_init or whenever the strings may be stale.
 */
void mb_view_hp_sync_from_doubles(MBViewState *view);

/**
 * Set the center from decimal strings (e.g. user input). Doubles are synced.
 * All digits of the input are preserved even if the current zoom would need
 * fewer. Returns 0 on success, -1 if a string does not parse.
 */
int mb_view_hp_set_center(MBViewState *view, const char *re_str, const char *im_str);

/**
 * Translate the center by small complex offsets (typically pixels * scale).
 * The FloatExp variant is exact at any zoom depth; the double variant is a
 * convenience for shallow-zoom callers.
 */
void mb_view_hp_translate_fx(MBViewState *view, FloatExp d_re, FloatExp d_im);
void mb_view_hp_translate(MBViewState *view, double d_re, double d_im);

/**
 * Zoom by `factor` keeping the complex point under the given viewport offset
 * fixed on screen. Offsets are in pixels relative to the viewport center,
 * with off_y_up positive toward the top of the screen (imaginary axis up).
 * The zoom is updated in log space and clamped to
 * [MB_ZOOM_LOG10_MIN, MB_ZOOM_LOG10_MAX].
 */
void mb_view_hp_zoom_towards(MBViewState *view, double off_x_px, double off_y_up_px,
                             double factor);

/**
 * Compute (to.center - from.center) in high precision. The FloatExp variant
 * is exact at any depth; the double variant saturates outside double range.
 * Used e.g. to reproject the previous frame as a placeholder.
 */
void mb_view_hp_center_delta_fx(const MBViewState *from, const MBViewState *to,
                                FloatExp *d_re, FloatExp *d_im);
void mb_view_hp_center_delta(const MBViewState *from, const MBViewState *to,
                             double *d_re, double *d_im);

#endif // MB_VIEW_HP_H
