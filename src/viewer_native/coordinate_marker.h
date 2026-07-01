#ifndef MB_COORDINATE_MARKER_H
#define MB_COORDINATE_MARKER_H

#import <Cocoa/Cocoa.h>

// =============================================================================
// MBCoordinateMarkerDelegate Protocol
// =============================================================================

@class MBCoordinateMarker;

@protocol MBCoordinateMarkerDelegate <NSObject>

/**
 * Called when the marker is placed or dragged to a new position.
 * @param marker The coordinate marker
 * @param real Real component of the complex coordinate
 * @param imag Imaginary component of the complex coordinate
 */
- (void)coordinateMarker:(MBCoordinateMarker *)marker didMoveToReal:(double)real imag:(double)imag;

/**
 * Called when user requests navigation to the marker position.
 * @param marker The coordinate marker
 */
- (void)coordinateMarkerDidRequestNavigation:(MBCoordinateMarker *)marker;

@optional

/**
 * Called when coordinate picker mode is toggled.
 * @param marker The coordinate marker
 * @param active YES if picker mode is now active
 */
- (void)coordinateMarker:(MBCoordinateMarker *)marker pickerModeChanged:(BOOL)active;

@end

// =============================================================================
// MBCoordinateMarker - Draggable Coordinate Crosshair Overlay
// =============================================================================

/**
 * Marker visual settings
 */
#define MB_MARKER_SIZE 20.0
#define MB_MARKER_LINE_WIDTH 2.0

/**
 * Coordinate marker states
 */
typedef NS_ENUM(NSInteger, MBMarkerState) {
    MBMarkerStateHidden = 0,    // No marker visible
    MBMarkerStateTracking,      // Marker follows mouse
    MBMarkerStatePlaced,        // Marker at fixed position, can be dragged
};

@interface MBCoordinateMarker : NSView

/**
 * Delegate for marker actions
 */
@property (nonatomic, assign) id<MBCoordinateMarkerDelegate> delegate;

/**
 * Current state of the marker
 */
@property (nonatomic, readonly) MBMarkerState state;

/**
 * Whether coordinate picker mode is active
 */
@property (nonatomic, readonly) BOOL pickerModeActive;

/**
 * Current marker position in view coordinates
 */
@property (nonatomic, readonly) NSPoint markerPosition;

/**
 * Current complex coordinate (real, imag)
 */
@property (nonatomic, readonly) double coordinateReal;
@property (nonatomic, readonly) double coordinateImag;

/**
 * Initialize with frame
 */
- (instancetype)initWithFrame:(NSRect)frameRect;

/**
 * Toggle coordinate picker mode.
 * When active, mouse clicks place the marker.
 */
- (void)togglePickerMode;

/**
 * Set picker mode directly
 */
- (void)setPickerMode:(BOOL)active;

/**
 * Place marker at specific view coordinates.
 * Converts to complex coordinates and notifies delegate.
 * @param point View coordinates
 * @param viewState Current view state for coordinate conversion
 */
- (void)placeMarkerAtPoint:(NSPoint)point withViewState:(const void *)viewState;

/**
 * Place marker at specific complex coordinates.
 * @param real Real component
 * @param imag Imaginary component
 * @param viewState Current view state for coordinate conversion
 */
- (void)placeMarkerAtReal:(double)real imag:(double)imag withViewState:(const void *)viewState;

/**
 * Update marker position from external coordinate change.
 * Used when text field editing moves the marker.
 */
- (void)updateMarkerFromReal:(double)real imag:(double)imag withViewState:(const void *)viewState;

/**
 * Update view state reference (call when view pans/zooms).
 * Updates screen position of placed marker.
 */
- (void)updateViewState:(const void *)viewState;

/**
 * Hide the marker
 */
- (void)hideMarker;

/**
 * Show coordinate label near marker
 */
@property (nonatomic, assign) BOOL showCoordinateLabel;

@end

#endif // MB_COORDINATE_MARKER_H
