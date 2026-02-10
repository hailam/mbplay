#ifndef MB_NATIVE_VIEWER_H
#define MB_NATIVE_VIEWER_H

#include "../config.h"

// =============================================================================
// Native macOS Viewer API
// =============================================================================

/**
 * Initialize the native viewer window.
 * Creates an NSWindow with a custom NSView backed by CAMetalLayer.
 *
 * @param title Window title
 * @param width Initial viewport width
 * @param height Initial viewport height
 * @return 0 on success, -1 on failure
 */
int native_viewer_init(const char *title, int width, int height);

/**
 * Run the native event loop.
 * This is a blocking call that handles user input and rendering.
 * Returns when the window is closed.
 */
void native_viewer_run(void);

/**
 * Shut down the native viewer and release resources.
 */
void native_viewer_shutdown(void);

/**
 * Get the current view state.
 * @return Pointer to current view state
 */
MBViewState* native_viewer_get_state(void);

#endif // MB_NATIVE_VIEWER_H
