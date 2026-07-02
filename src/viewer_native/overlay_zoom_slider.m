#import "overlay_zoom_slider.h"
#include <math.h>

// =============================================================================
// Constants
// =============================================================================

#define FADE_IN_DURATION 0.2
#define FADE_OUT_DURATION 0.3
#define SLIDER_PADDING 8.0
#define LABEL_HEIGHT 16.0
#define BUTTON_SIZE 24.0

// =============================================================================
// MBZoomSliderOverlay Implementation
// =============================================================================

@interface MBZoomSliderOverlay ()

@property (nonatomic, retain) NSSlider *slider;
@property (nonatomic, retain) NSTextField *zoomLabel;
@property (nonatomic, retain) NSButton *zoomInButton;
@property (nonatomic, retain) NSButton *zoomOutButton;
@property (nonatomic, assign) NSTimer *hideTimer;  // Not retained - runloop owns it
@property (nonatomic, retain) NSTrackingArea *trackingArea;
@property (nonatomic, assign) BOOL mouseInside;
@property (nonatomic, readwrite) BOOL isVisible;

@end

@implementation MBZoomSliderOverlay

- (instancetype)initWithFrame:(NSRect)frameRect {
    self = [super initWithFrame:frameRect];
    if (self) {
        _hideDelay = 3.0;
        _mouseInside = NO;
        _isVisible = YES;  // Start visible, then auto-hide

        [self setupUI];
        [self enableMouseTracking];

        // Schedule initial auto-hide
        [self scheduleHide];
    }
    return self;
}

- (void)dealloc {
    if (_hideTimer) {
        [_hideTimer invalidate];
        // Don't release - we didn't retain it, runloop owns it
    }
    if (_trackingArea) {
        [self removeTrackingArea:_trackingArea];
        [_trackingArea release];
    }
    [_slider release];
    [_zoomLabel release];
    [_zoomInButton release];
    [_zoomOutButton release];
    [super dealloc];
}

- (void)setupUI {
    // Use visual effect view for translucent background
    self.wantsLayer = YES;
    self.layer.backgroundColor = [[NSColor colorWithWhite:0.1 alpha:0.85] CGColor];
    self.layer.cornerRadius = 8.0;

    CGFloat contentWidth = self.bounds.size.width - SLIDER_PADDING * 2;
    CGFloat y = self.bounds.size.height - SLIDER_PADDING;

    // Zoom in button at top
    y -= BUTTON_SIZE;
    _zoomInButton = [[NSButton alloc] initWithFrame:NSMakeRect(
        (self.bounds.size.width - BUTTON_SIZE) / 2, y, BUTTON_SIZE, BUTTON_SIZE)];
    [_zoomInButton setButtonType:NSButtonTypeMomentaryPushIn];
    [_zoomInButton setBezelStyle:NSBezelStyleCircular];
    [_zoomInButton setTitle:@"+"];
    [_zoomInButton setTarget:self];
    [_zoomInButton setAction:@selector(zoomInClicked:)];
    [self addSubview:_zoomInButton];

    y -= SLIDER_PADDING;

    // Vertical slider (most of the height)
    CGFloat sliderHeight = self.bounds.size.height - BUTTON_SIZE * 2 - LABEL_HEIGHT - SLIDER_PADDING * 5;
    y -= sliderHeight;

    _slider = [[NSSlider alloc] initWithFrame:NSMakeRect(
        (self.bounds.size.width - 20) / 2, y, 20, sliderHeight)];
    [_slider setSliderType:NSSliderTypeLinear];
    [_slider setVertical:YES];
    [_slider setMinValue:MB_ZOOM_LOG_MIN];
    [_slider setMaxValue:MB_ZOOM_LOG_MAX];
    [_slider setDoubleValue:0.0];
    [_slider setContinuous:YES];
    [_slider setTarget:self];
    [_slider setAction:@selector(sliderChanged:)];
    [self addSubview:_slider];

    y -= SLIDER_PADDING;

    // Zoom out button
    y -= BUTTON_SIZE;
    _zoomOutButton = [[NSButton alloc] initWithFrame:NSMakeRect(
        (self.bounds.size.width - BUTTON_SIZE) / 2, y, BUTTON_SIZE, BUTTON_SIZE)];
    [_zoomOutButton setButtonType:NSButtonTypeMomentaryPushIn];
    [_zoomOutButton setBezelStyle:NSBezelStyleCircular];
    [_zoomOutButton setTitle:@"-"];
    [_zoomOutButton setTarget:self];
    [_zoomOutButton setAction:@selector(zoomOutClicked:)];
    [self addSubview:_zoomOutButton];

    y -= SLIDER_PADDING;

    // Zoom level label at bottom
    y -= LABEL_HEIGHT;
    _zoomLabel = [[NSTextField alloc] initWithFrame:NSMakeRect(
        SLIDER_PADDING, y, contentWidth, LABEL_HEIGHT)];
    [_zoomLabel setStringValue:@"1.0x"];
    [_zoomLabel setBezeled:NO];
    [_zoomLabel setDrawsBackground:NO];
    [_zoomLabel setEditable:NO];
    [_zoomLabel setSelectable:NO];
    [_zoomLabel setTextColor:[NSColor whiteColor]];
    [_zoomLabel setFont:[NSFont monospacedSystemFontOfSize:9 weight:NSFontWeightRegular]];
    [_zoomLabel setAlignment:NSTextAlignmentCenter];
    [self addSubview:_zoomLabel];
}

- (void)enableMouseTracking {
    if (_trackingArea) {
        [self removeTrackingArea:_trackingArea];
        [_trackingArea release];
    }

    _trackingArea = [[NSTrackingArea alloc]
        initWithRect:self.bounds
             options:(NSTrackingMouseEnteredAndExited | NSTrackingActiveInKeyWindow)
               owner:self
            userInfo:nil];
    [self addTrackingArea:_trackingArea];
}

- (void)updateTrackingAreas {
    [super updateTrackingAreas];
    [self enableMouseTracking];
}

// =============================================================================
// Mouse Events
// =============================================================================

- (void)mouseEntered:(NSEvent *)event {
    (void)event;
    _mouseInside = YES;
    [self cancelHide];
    [self showWithFade];
}

- (void)mouseExited:(NSEvent *)event {
    (void)event;
    _mouseInside = NO;
    [self scheduleHide];
}

- (void)scrollWheel:(NSEvent *)event {
    // Fine zoom adjustment via scroll wheel
    CGFloat delta = [event scrollingDeltaY];
    if ([event hasPreciseScrollingDeltas]) {
        delta *= 0.1;  // Trackpad sensitivity
    }

    id<MBZoomSliderOverlayDelegate> delegate = _delegate;
    if (delegate && [delegate respondsToSelector:@selector(zoomSliderOverlay:didScrollWithDelta:)]) {
        [delegate zoomSliderOverlay:self didScrollWithDelta:delta];
    }

    // Reset hide timer on interaction
    [self scheduleHide];
}

// =============================================================================
// Button Actions
// =============================================================================

- (void)zoomInClicked:(id)sender {
    (void)sender;
    // Increase zoom by ~1.5x (0.176 in log10 scale)
    double currentLog = [_slider doubleValue];
    double newLog = currentLog + 0.176;
    if (newLog > MB_ZOOM_LOG_MAX) newLog = MB_ZOOM_LOG_MAX;

    [_slider setDoubleValue:newLog];
    [self sliderChanged:_slider];
    [self scheduleHide];
}

- (void)zoomOutClicked:(id)sender {
    (void)sender;
    // Decrease zoom by ~1.5x
    double currentLog = [_slider doubleValue];
    double newLog = currentLog - 0.176;
    if (newLog < MB_ZOOM_LOG_MIN) newLog = MB_ZOOM_LOG_MIN;

    [_slider setDoubleValue:newLog];
    [self sliderChanged:_slider];
    [self scheduleHide];
}

- (void)sliderChanged:(id)sender {
    (void)sender;

    // The slider value IS log10 of the zoom (pow(10, x) would overflow a
    // double past x = 308).
    double logValue = [_slider doubleValue];

    // Update label
    [self updateLabelForZoomLog10:logValue];

    // Notify delegate
    id<MBZoomSliderOverlayDelegate> delegate = _delegate;
    if (delegate && [delegate respondsToSelector:@selector(zoomSliderOverlay:didChangeZoomLog10:)]) {
        [delegate zoomSliderOverlay:self didChangeZoomLog10:logValue];
    }

    // Reset hide timer
    [self scheduleHide];
}

// =============================================================================
// Public Methods
// =============================================================================

- (void)updateZoomLog10:(double)zoomLog10 {
    double logValue = zoomLog10;
    if (logValue < MB_ZOOM_LOG_MIN) logValue = MB_ZOOM_LOG_MIN;
    if (logValue > MB_ZOOM_LOG_MAX) logValue = MB_ZOOM_LOG_MAX;

    // Grow the slider range with the current depth instead of always
    // spanning the full 0..4000: a fixed full-range track would move ~25
    // decades per pixel, making the knob useless. With a range of ~1.5x the
    // current depth (min 60 decades), one pixel is well under a decade at
    // typical depths.
    double range = logValue * 1.5 + 30.0;
    if (range < 60.0) range = 60.0;
    if (range > MB_ZOOM_LOG_MAX) range = MB_ZOOM_LOG_MAX;
    [_slider setMaxValue:range];

    [_slider setDoubleValue:logValue];
    [self updateLabelForZoomLog10:logValue];
}

- (void)updateLabelForZoomLog10:(double)zoomLog10 {
    NSString *label;
    if (zoomLog10 >= 6.0) {
        label = [NSString stringWithFormat:@"1e%.0fx", zoomLog10];
    } else if (zoomLog10 >= 3.0) {
        label = [NSString stringWithFormat:@"%.0fx", pow(10.0, zoomLog10)];
    } else {
        label = [NSString stringWithFormat:@"%.1fx", pow(10.0, zoomLog10)];
    }
    [_zoomLabel setStringValue:label];
}

- (void)showWithFade {
    if (_isVisible && self.alphaValue >= 1.0) return;

    _isVisible = YES;
    [self cancelHide];

    [NSAnimationContext runAnimationGroup:^(NSAnimationContext *ctx) {
        ctx.duration = FADE_IN_DURATION;
        self.animator.alphaValue = 1.0;
    } completionHandler:nil];
}

- (void)scheduleHide {
    [self cancelHide];

    // Don't retain - scheduledTimerWithTimeInterval adds to runloop which retains it
    _hideTimer = [NSTimer scheduledTimerWithTimeInterval:_hideDelay
                                                  target:self
                                                selector:@selector(fadeOut)
                                                userInfo:nil
                                                 repeats:NO];
}

- (void)cancelHide {
    if (_hideTimer) {
        [_hideTimer invalidate];
        // Don't release - invalidate removes from runloop which releases it
        _hideTimer = nil;
    }
}

- (void)fadeOut {
    // The one-shot timer that got us here is invalidated by the run loop
    // after firing; clear the assign-property now so a later cancelHide
    // does not message a deallocated timer.
    _hideTimer = nil;

    if (_mouseInside) return;

    [NSAnimationContext runAnimationGroup:^(NSAnimationContext *ctx) {
        ctx.duration = FADE_OUT_DURATION;
        self.animator.alphaValue = 0.0;
    } completionHandler:^{
        self->_isVisible = NO;
    }];
}

- (void)hideImmediately {
    [self cancelHide];
    self.alphaValue = 0.0;
    _isVisible = NO;
}

// =============================================================================
// Drawing
// =============================================================================

- (BOOL)isFlipped {
    return YES;
}

@end
