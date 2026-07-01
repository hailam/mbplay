#import "coordinate_marker.h"
#import "../config.h"
#include <math.h>

// =============================================================================
// Constants
// =============================================================================

#define CROSSHAIR_LENGTH 10.0
#define CENTER_DOT_RADIUS 3.0
#define LABEL_OFFSET_Y 25.0
#define LABEL_PADDING 4.0
#define DRAG_THRESHOLD 5.0

// =============================================================================
// MBCoordinateMarker Implementation
// =============================================================================

@interface MBCoordinateMarker ()

@property (nonatomic, readwrite) MBMarkerState state;
@property (nonatomic, readwrite) BOOL pickerModeActive;
@property (nonatomic, readwrite) NSPoint markerPosition;
@property (nonatomic, readwrite) double coordinateReal;
@property (nonatomic, readwrite) double coordinateImag;

@property (nonatomic, retain) NSTextField *coordLabel;
@property (nonatomic, retain) NSTrackingArea *trackingArea;
@property (nonatomic, assign) BOOL isDragging;
@property (nonatomic, assign) NSPoint dragStartPoint;
@property (nonatomic, assign) MBViewState cachedViewState;

@end

@implementation MBCoordinateMarker

- (instancetype)initWithFrame:(NSRect)frameRect {
    self = [super initWithFrame:frameRect];
    if (self) {
        _state = MBMarkerStateHidden;
        _pickerModeActive = NO;
        _markerPosition = NSMakePoint(0, 0);
        _coordinateReal = 0.0;
        _coordinateImag = 0.0;
        _showCoordinateLabel = YES;
        _isDragging = NO;

        // This view doesn't draw background, only the marker
        self.wantsLayer = YES;
        self.layer.backgroundColor = [[NSColor clearColor] CGColor];

        // Create coordinate label
        _coordLabel = [[NSTextField alloc] initWithFrame:NSMakeRect(0, 0, 200, 18)];
        [_coordLabel setStringValue:@""];
        [_coordLabel setBezeled:NO];
        [_coordLabel setDrawsBackground:YES];
        [_coordLabel setBackgroundColor:[NSColor colorWithWhite:0.0 alpha:0.7]];
        [_coordLabel setTextColor:[NSColor whiteColor]];
        [_coordLabel setFont:[NSFont monospacedSystemFontOfSize:10 weight:NSFontWeightRegular]];
        [_coordLabel setEditable:NO];
        [_coordLabel setSelectable:NO];
        [_coordLabel setAlignment:NSTextAlignmentCenter];
        [_coordLabel setHidden:YES];
        [_coordLabel.cell setTruncatesLastVisibleLine:YES];
        [self addSubview:_coordLabel];

        [self enableMouseTracking];
    }
    return self;
}

- (void)dealloc {
    // Clean up cursor stack if picker was active
    if (_pickerModeActive) {
        [NSCursor pop];
    }
    if (_trackingArea) {
        [self removeTrackingArea:_trackingArea];
        [_trackingArea release];
    }
    [_coordLabel release];
    [super dealloc];
}

- (void)enableMouseTracking {
    if (_trackingArea) {
        [self removeTrackingArea:_trackingArea];
        [_trackingArea release];
    }

    _trackingArea = [[NSTrackingArea alloc]
        initWithRect:self.bounds
             options:(NSTrackingMouseMoved | NSTrackingMouseEnteredAndExited |
                      NSTrackingActiveInKeyWindow | NSTrackingInVisibleRect)
               owner:self
            userInfo:nil];
    [self addTrackingArea:_trackingArea];
}

- (void)updateTrackingAreas {
    [super updateTrackingAreas];
    [self enableMouseTracking];
}

// =============================================================================
// Public Methods
// =============================================================================

- (void)togglePickerMode {
    [self setPickerMode:!_pickerModeActive];
}

- (void)setPickerMode:(BOOL)active {
    if (_pickerModeActive == active) return;

    _pickerModeActive = active;

    if (active) {
        _state = MBMarkerStateTracking;
        // Change cursor to crosshair
        [[NSCursor crosshairCursor] push];
    } else {
        if (_state == MBMarkerStateTracking) {
            _state = MBMarkerStateHidden;
        }
        [NSCursor pop];
    }

    [self setNeedsDisplay:YES];

    // Notify delegate
    id<MBCoordinateMarkerDelegate> delegate = _delegate;
    if (delegate && [delegate respondsToSelector:@selector(coordinateMarker:pickerModeChanged:)]) {
        [delegate coordinateMarker:self pickerModeChanged:active];
    }
}

- (void)placeMarkerAtPoint:(NSPoint)point withViewState:(const void *)viewState {
    if (!viewState) return;

    const MBViewState *vs = (const MBViewState *)viewState;
    _cachedViewState = *vs;

    _markerPosition = point;
    _state = MBMarkerStatePlaced;

    // Convert to complex coordinates
    double cx, cy;
    mb_pixel_to_complex(vs, (int)point.x, (int)point.y, &cx, &cy);
    _coordinateReal = cx;
    _coordinateImag = cy;

    [self updateLabelPosition];
    [self setNeedsDisplay:YES];

    // Notify delegate
    id<MBCoordinateMarkerDelegate> delegate = _delegate;
    if (delegate && [delegate respondsToSelector:@selector(coordinateMarker:didMoveToReal:imag:)]) {
        [delegate coordinateMarker:self didMoveToReal:cx imag:cy];
    }
}

- (void)placeMarkerAtReal:(double)real imag:(double)imag withViewState:(const void *)viewState {
    if (!viewState) return;

    const MBViewState *vs = (const MBViewState *)viewState;
    _cachedViewState = *vs;

    _coordinateReal = real;
    _coordinateImag = imag;
    _state = MBMarkerStatePlaced;

    // Convert to screen coordinates
    int px, py;
    mb_complex_to_pixel(vs, real, imag, &px, &py);
    _markerPosition = NSMakePoint(px, py);

    [self updateLabelPosition];
    [self setNeedsDisplay:YES];

    // Notify delegate
    id<MBCoordinateMarkerDelegate> delegate = _delegate;
    if (delegate && [delegate respondsToSelector:@selector(coordinateMarker:didMoveToReal:imag:)]) {
        [delegate coordinateMarker:self didMoveToReal:real imag:imag];
    }
}

- (void)updateMarkerFromReal:(double)real imag:(double)imag withViewState:(const void *)viewState {
    if (!viewState) return;
    if (_state != MBMarkerStatePlaced) return;

    const MBViewState *vs = (const MBViewState *)viewState;
    _cachedViewState = *vs;

    _coordinateReal = real;
    _coordinateImag = imag;

    // Convert to screen coordinates
    int px, py;
    mb_complex_to_pixel(vs, real, imag, &px, &py);
    _markerPosition = NSMakePoint(px, py);

    [self updateLabelPosition];
    [self setNeedsDisplay:YES];
}

- (void)updateViewState:(const void *)viewState {
    if (!viewState) return;

    // Always cache the view state: picker-mode clicks convert through it,
    // and before the first marker placement it would otherwise still be the
    // zero-initialized struct (viewport 0x0 -> division by zero -> the view
    // would navigate to NaN).
    const MBViewState *vs = (const MBViewState *)viewState;
    _cachedViewState = *vs;

    if (_state != MBMarkerStatePlaced) return;

    // Recalculate screen position from stored coordinates
    int px, py;
    mb_complex_to_pixel(vs, _coordinateReal, _coordinateImag, &px, &py);
    _markerPosition = NSMakePoint(px, py);

    [self updateLabelPosition];
    [self setNeedsDisplay:YES];
}

- (void)hideMarker {
    _state = MBMarkerStateHidden;
    [_coordLabel setHidden:YES];
    [self setNeedsDisplay:YES];
}

- (void)updateLabelPosition {
    if (!_showCoordinateLabel || _state == MBMarkerStateHidden) {
        [_coordLabel setHidden:YES];
        return;
    }

    // Format coordinate string
    NSString *coordStr;
    if (_coordinateImag >= 0) {
        coordStr = [NSString stringWithFormat:@"%.10g + %.10gi", _coordinateReal, _coordinateImag];
    } else {
        coordStr = [NSString stringWithFormat:@"%.10g - %.10gi", _coordinateReal, -_coordinateImag];
    }
    [_coordLabel setStringValue:coordStr];
    [_coordLabel sizeToFit];

    // Position label below marker
    CGFloat labelWidth = _coordLabel.frame.size.width + LABEL_PADDING * 2;
    CGFloat labelX = _markerPosition.x - labelWidth / 2;
    CGFloat labelY = _markerPosition.y + LABEL_OFFSET_Y;

    // Clamp to view bounds
    if (labelX < 0) labelX = 0;
    if (labelX + labelWidth > self.bounds.size.width) {
        labelX = self.bounds.size.width - labelWidth;
    }
    if (labelY + 18 > self.bounds.size.height) {
        labelY = _markerPosition.y - LABEL_OFFSET_Y - 18;  // Above marker
    }

    [_coordLabel setFrame:NSMakeRect(labelX, labelY, labelWidth, 18)];
    [_coordLabel setHidden:NO];
}

// =============================================================================
// Mouse Events
// =============================================================================

- (void)mouseMoved:(NSEvent *)event {
    if (_state != MBMarkerStateTracking) return;

    NSPoint point = [self convertPoint:[event locationInWindow] fromView:nil];
    _markerPosition = point;

    // Update coordinates for display
    double cx, cy;
    mb_pixel_to_complex(&_cachedViewState, (int)point.x, (int)point.y, &cx, &cy);
    _coordinateReal = cx;
    _coordinateImag = cy;

    [self updateLabelPosition];
    [self setNeedsDisplay:YES];
}

- (void)mouseDown:(NSEvent *)event {
    NSPoint point = [self convertPoint:[event locationInWindow] fromView:nil];

    if (_state == MBMarkerStatePlaced) {
        // Check if clicking near marker for drag
        CGFloat dx = point.x - _markerPosition.x;
        CGFloat dy = point.y - _markerPosition.y;
        CGFloat distance = sqrt(dx * dx + dy * dy);

        if (distance < MB_MARKER_SIZE) {
            _isDragging = YES;
            _dragStartPoint = point;
            return;
        }
    }

    if (_pickerModeActive) {
        // Place marker at click location
        [self placeMarkerAtPoint:point withViewState:&_cachedViewState];

        // Request navigation
        id<MBCoordinateMarkerDelegate> delegate = _delegate;
        if (delegate && [delegate respondsToSelector:@selector(coordinateMarkerDidRequestNavigation:)]) {
            [delegate coordinateMarkerDidRequestNavigation:self];
        }
    }
}

- (void)mouseDragged:(NSEvent *)event {
    if (!_isDragging) return;

    NSPoint point = [self convertPoint:[event locationInWindow] fromView:nil];

    // Move marker
    _markerPosition = point;

    // Update coordinates
    double cx, cy;
    mb_pixel_to_complex(&_cachedViewState, (int)point.x, (int)point.y, &cx, &cy);
    _coordinateReal = cx;
    _coordinateImag = cy;

    [self updateLabelPosition];
    [self setNeedsDisplay:YES];

    // Notify delegate
    id<MBCoordinateMarkerDelegate> delegate = _delegate;
    if (delegate && [delegate respondsToSelector:@selector(coordinateMarker:didMoveToReal:imag:)]) {
        [delegate coordinateMarker:self didMoveToReal:cx imag:cy];
    }
}

- (void)mouseUp:(NSEvent *)event {
    (void)event;
    _isDragging = NO;
}

- (BOOL)acceptsFirstResponder {
    return NO;  // Don't steal focus from main view
}

// =============================================================================
// Drawing
// =============================================================================

- (void)drawRect:(NSRect)dirtyRect {
    (void)dirtyRect;

    if (_state == MBMarkerStateHidden) return;

    NSPoint pos = _markerPosition;

    // Draw crosshair
    [[NSColor colorWithRed:1.0 green:0.3 blue:0.3 alpha:1.0] setStroke];
    NSBezierPath *crosshair = [NSBezierPath bezierPath];
    [crosshair setLineWidth:MB_MARKER_LINE_WIDTH];

    // Horizontal line
    [crosshair moveToPoint:NSMakePoint(pos.x - CROSSHAIR_LENGTH, pos.y)];
    [crosshair lineToPoint:NSMakePoint(pos.x + CROSSHAIR_LENGTH, pos.y)];

    // Vertical line
    [crosshair moveToPoint:NSMakePoint(pos.x, pos.y - CROSSHAIR_LENGTH)];
    [crosshair lineToPoint:NSMakePoint(pos.x, pos.y + CROSSHAIR_LENGTH)];

    [crosshair stroke];

    // Draw center dot
    [[NSColor colorWithRed:1.0 green:0.3 blue:0.3 alpha:1.0] setFill];
    NSBezierPath *dot = [NSBezierPath bezierPathWithOvalInRect:
        NSMakeRect(pos.x - CENTER_DOT_RADIUS, pos.y - CENTER_DOT_RADIUS,
                   CENTER_DOT_RADIUS * 2, CENTER_DOT_RADIUS * 2)];
    [dot fill];

    // Draw white outline for visibility on dark backgrounds
    [[NSColor colorWithWhite:1.0 alpha:0.5] setStroke];
    NSBezierPath *outline = [NSBezierPath bezierPath];
    [outline setLineWidth:1.0];
    [outline moveToPoint:NSMakePoint(pos.x - CROSSHAIR_LENGTH - 1, pos.y)];
    [outline lineToPoint:NSMakePoint(pos.x + CROSSHAIR_LENGTH + 1, pos.y)];
    [outline moveToPoint:NSMakePoint(pos.x, pos.y - CROSSHAIR_LENGTH - 1)];
    [outline lineToPoint:NSMakePoint(pos.x, pos.y + CROSSHAIR_LENGTH + 1)];
    [outline stroke];
}

- (BOOL)isFlipped {
    return YES;  // Match parent view coordinate system
}

// Allow click-through except when picker mode active or dragging marker
- (NSView *)hitTest:(NSPoint)point {
    if (_pickerModeActive) {
        return [super hitTest:point];
    }

    if (_state == MBMarkerStatePlaced) {
        // Check if near marker. hitTest: receives the point in the
        // superview's coordinate system, not window coordinates.
        NSPoint localPoint = [self convertPoint:point fromView:self.superview];
        CGFloat dx = localPoint.x - _markerPosition.x;
        CGFloat dy = localPoint.y - _markerPosition.y;
        CGFloat distance = sqrt(dx * dx + dy * dy);

        if (distance < MB_MARKER_SIZE) {
            return [super hitTest:point];
        }
    }

    return nil;  // Click-through
}

@end
