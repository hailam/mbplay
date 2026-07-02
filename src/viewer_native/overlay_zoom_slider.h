#ifndef MB_OVERLAY_ZOOM_SLIDER_H
#define MB_OVERLAY_ZOOM_SLIDER_H

#import <Cocoa/Cocoa.h>
#include "../config.h"

// =============================================================================
// MBZoomSliderOverlayDelegate Protocol
// =============================================================================

@class MBZoomSliderOverlay;

@protocol MBZoomSliderOverlayDelegate <NSObject>

/**
 * Called when user drags the zoom slider.
 * @param slider The zoom slider overlay
 * @param zoomLog10 The requested zoom level as log10 (0 to MB_ZOOM_LOG10_MAX).
 *                  Log form: 10^4000 does not fit in a double.
 */
- (void)zoomSliderOverlay:(MBZoomSliderOverlay *)slider didChangeZoomLog10:(double)zoomLog10;

/**
 * Called when user scrolls over the zoom slider for fine adjustment.
 * @param slider The zoom slider overlay
 * @param delta Scroll delta (positive = zoom in, negative = zoom out)
 */
- (void)zoomSliderOverlay:(MBZoomSliderOverlay *)slider didScrollWithDelta:(CGFloat)delta;

@end

// =============================================================================
// MBZoomSliderOverlay - Auto-hiding Vertical Zoom Slider
// =============================================================================

/**
 * Slider dimensions
 */
#define MB_ZOOM_SLIDER_WIDTH 40.0
#define MB_ZOOM_SLIDER_HEIGHT 250.0
#define MB_ZOOM_SLIDER_MARGIN 15.0

/**
 * Zoom range (logarithmic, from config.h — single source of truth)
 */
#define MB_ZOOM_LOG_MIN 0.0
#define MB_ZOOM_LOG_MAX MB_ZOOM_LOG10_MAX

@interface MBZoomSliderOverlay : NSView

/**
 * Delegate for zoom slider actions
 */
@property (nonatomic, assign) id<MBZoomSliderOverlayDelegate> delegate;

/**
 * Hide delay in seconds (default 3.0)
 */
@property (nonatomic, assign) NSTimeInterval hideDelay;

/**
 * Whether the slider is currently visible
 */
@property (nonatomic, readonly) BOOL isVisible;

/**
 * Initialize with parent view to anchor to
 */
- (instancetype)initWithFrame:(NSRect)frameRect;

/**
 * Update zoom display from external source (log10 of the zoom level).
 * Does NOT trigger delegate callback.
 */
- (void)updateZoomLog10:(double)zoomLog10;

/**
 * Show the slider with fade-in animation.
 * Resets the hide timer.
 */
- (void)showWithFade;

/**
 * Schedule auto-hide after hideDelay seconds.
 */
- (void)scheduleHide;

/**
 * Cancel any pending hide timer.
 */
- (void)cancelHide;

/**
 * Force immediate hide (no animation).
 */
- (void)hideImmediately;

@end

#endif // MB_OVERLAY_ZOOM_SLIDER_H
