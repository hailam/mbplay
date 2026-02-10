#ifndef MB_VIEWER_H
#define MB_VIEWER_H

#include "../config.h"
#include <stdbool.h>

// =============================================================================
// Progressive Viewer API (SDL2)
// =============================================================================

/**
 * Initialize the viewer window.
 * Creates an SDL2 window scaled to fit the screen.
 *
 * @param title Window title
 * @param full_width Full image width
 * @param full_height Full image height
 * @return 0 on success, -1 on failure
 */
int viewer_init(const char *title, int full_width, int full_height);

/**
 * Update a row in the viewer (downsampled to display resolution).
 * Thread-safe - can be called from compute threads.
 *
 * @param y Row index in full resolution
 * @param row Row pixel data
 * @param width Row width (full resolution)
 */
void viewer_update_row(int y, const PixelColor *row, int width);

/**
 * Process events and present the frame.
 * Should be called regularly from main loop.
 *
 * @return false if window was closed, true otherwise
 */
bool viewer_present(void);

/**
 * Check if the viewer is still running.
 *
 * @return true if window is open
 */
bool viewer_is_running(void);

/**
 * Get display scale factor.
 *
 * @return Scale factor (full_size / display_size)
 */
float viewer_get_scale(void);

/**
 * Shutdown the viewer and release resources.
 */
void viewer_shutdown(void);

#endif // MB_VIEWER_H
