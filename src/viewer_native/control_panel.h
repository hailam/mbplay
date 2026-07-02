#ifndef MB_CONTROL_PANEL_H
#define MB_CONTROL_PANEL_H

#import <Cocoa/Cocoa.h>
#include "../config.h"

// =============================================================================
// MBControlPanelDelegate Protocol
// =============================================================================

@class MBControlPanel;

@protocol MBControlPanelDelegate <NSObject>

/**
 * Called when user requests navigation to coordinates.
 * Coordinates are passed as the raw decimal strings so deep-zoom targets
 * keep their full precision (a double only holds ~17 digits).
 */
- (void)controlPanel:(MBControlPanel *)panel didRequestJumpToRealString:(NSString *)realStr
          imagString:(NSString *)imagStr;

/**
 * Called when animation speed slider changes.
 * @param seconds Animation duration in seconds (0.3 - 10.0)
 */
- (void)controlPanel:(MBControlPanel *)panel didChangeAnimationSpeed:(double)seconds;

/**
 * Called when color cycle scale slider changes.
 * @param scale Color band density (8.0 - 512.0)
 */
- (void)controlPanel:(MBControlPanel *)panel didChangeColorCycleScale:(float)scale;

/**
 * Called when user requests coordinates copied to clipboard.
 */
- (void)controlPanelDidRequestCopyCoordinates:(MBControlPanel *)panel;

@end

// =============================================================================
// MBControlPanel - Native macOS Control Panel
// =============================================================================

/**
 * Panel width when expanded (220pt as per spec)
 */
#define MB_CONTROL_PANEL_WIDTH 220.0

@interface MBControlPanel : NSView

/**
 * Delegate for control panel actions (assign, not weak, for non-ARC)
 */
@property (nonatomic, assign) id<MBControlPanelDelegate> delegate;

/**
 * Whether the panel is collapsed (hidden)
 */
@property (nonatomic, readonly) BOOL isCollapsed;

/**
 * Initialize with frame rect
 */
- (instancetype)initWithFrame:(NSRect)frameRect;

/**
 * Update coordinate display from view state.
 * Call this when the view navigates.
 */
- (void)updateCoordinatesFromViewState:(const MBViewState *)viewState;

/**
 * Update coordinate text fields directly.
 * Used by coordinate marker to sync marker position with panel.
 */
- (void)setCoordinateReal:(double)real imag:(double)imag;

/**
 * Update zoom display slider (log10 of the zoom level — 10^4000 does not
 * fit in a double). Call this when zoom level changes.
 */
- (void)updateZoomDisplayLog10:(double)zoomLog10;

/**
 * Update animation speed slider value (for initial sync)
 */
- (void)setAnimationSpeed:(double)seconds;

/**
 * Update color cycle scale slider value (for initial sync)
 */
- (void)setColorCycleScale:(float)scale;

/**
 * Toggle panel collapsed state
 */
- (void)toggleCollapsed;

/**
 * Set collapsed state directly
 */
- (void)setCollapsed:(BOOL)collapsed animated:(BOOL)animated;

@end

#endif // MB_CONTROL_PANEL_H
