#import "control_panel.h"
#import <Cocoa/Cocoa.h>
#include <math.h>

// =============================================================================
// Constants
// =============================================================================

#define PANEL_PADDING 12.0
#define CONTROL_HEIGHT 24.0
#define CONTROL_SPACING 8.0
#define LABEL_HEIGHT 14.0
#define SLIDER_HEIGHT 20.0

// Slider ranges
#define ANIM_SPEED_MIN 0.3
#define ANIM_SPEED_MAX 10.0
#define COLOR_CYCLE_MIN 8.0
#define COLOR_CYCLE_MAX 512.0
#define ZOOM_LOG_MAX MB_ZOOM_LOG10_MAX  // single source of truth in config.h

// =============================================================================
// MBControlPanel Implementation
// =============================================================================

@interface MBControlPanel ()

// Coordinate input
@property (nonatomic, strong) NSTextField *realLabel;
@property (nonatomic, strong) NSTextField *realField;
@property (nonatomic, strong) NSTextField *imagLabel;
@property (nonatomic, strong) NSTextField *imagField;
@property (nonatomic, strong) NSButton *jumpButton;
@property (nonatomic, strong) NSButton *coordCopyButton;

// Sliders
@property (nonatomic, strong) NSTextField *zoomLabel;
@property (nonatomic, strong) NSSlider *zoomSlider;
@property (nonatomic, strong) NSTextField *zoomValueLabel;

@property (nonatomic, strong) NSTextField *animSpeedLabel;
@property (nonatomic, strong) NSSlider *animSpeedSlider;
@property (nonatomic, strong) NSTextField *animSpeedValueLabel;

@property (nonatomic, strong) NSTextField *colorCycleLabel;
@property (nonatomic, strong) NSSlider *colorCycleSlider;
@property (nonatomic, strong) NSTextField *colorCycleValueLabel;

// Collapse toggle
@property (nonatomic, strong) NSButton *collapseButton;
@property (nonatomic, readwrite) BOOL isCollapsed;

// Number formatters
@property (nonatomic, strong) NSNumberFormatter *coordFormatter;

@end

@implementation MBControlPanel

- (instancetype)initWithFrame:(NSRect)frameRect {
    self = [super initWithFrame:frameRect];
    if (self) {
        _isCollapsed = YES;  // Start collapsed by default

        // Create number formatter for coordinate input
        _coordFormatter = [[NSNumberFormatter alloc] init];
        [_coordFormatter setNumberStyle:NSNumberFormatterDecimalStyle];
        [_coordFormatter setMaximumFractionDigits:17];
        [_coordFormatter setMinimumFractionDigits:0];
        [_coordFormatter setUsesGroupingSeparator:NO];

        [self setupUI];
    }
    return self;
}

- (void)dealloc {
    [_coordFormatter release];
    [_realLabel release];
    [_realField release];
    [_imagLabel release];
    [_imagField release];
    [_jumpButton release];
    [_coordCopyButton release];
    [_zoomLabel release];
    [_zoomSlider release];
    [_zoomValueLabel release];
    [_animSpeedLabel release];
    [_animSpeedSlider release];
    [_animSpeedValueLabel release];
    [_colorCycleLabel release];
    [_colorCycleSlider release];
    [_colorCycleValueLabel release];
    [_collapseButton release];
    [super dealloc];
}

- (void)setupUI {
    // Use dark appearance for panel background
    self.wantsLayer = YES;
    self.layer.backgroundColor = [[NSColor colorWithWhite:0.15 alpha:1.0] CGColor];

    CGFloat y = self.bounds.size.height - PANEL_PADDING;
    CGFloat contentWidth = MB_CONTROL_PANEL_WIDTH - PANEL_PADDING * 2;

    // --- Collapse button ---
    y -= CONTROL_HEIGHT;
    _collapseButton = [[NSButton alloc] initWithFrame:NSMakeRect(PANEL_PADDING, y, contentWidth, CONTROL_HEIGHT)];
    [_collapseButton setButtonType:NSButtonTypeMomentaryPushIn];
    [_collapseButton setBezelStyle:NSBezelStyleRounded];
    [_collapseButton setTitle:@"> Show Controls"];  // Starts collapsed
    [_collapseButton setTarget:self];
    [_collapseButton setAction:@selector(collapseButtonClicked:)];
    [self addSubview:_collapseButton];

    y -= CONTROL_SPACING * 2;

    // --- Coordinates Section ---
    // Real label
    y -= LABEL_HEIGHT;
    _realLabel = [self createLabelWithText:@"Real:" frame:NSMakeRect(PANEL_PADDING, y, contentWidth, LABEL_HEIGHT)];
    [self addSubview:_realLabel];

    // Real input field. No NSNumberFormatter: it would round the text to a
    // double-backed NSNumber on end-of-editing, destroying pasted
    // high-precision coordinates (and rejecting scientific notation).
    // Validation happens in jumpButtonClicked instead.
    y -= CONTROL_HEIGHT;
    _realField = [self createTextFieldWithFrame:NSMakeRect(PANEL_PADDING, y, contentWidth, CONTROL_HEIGHT)];
    [_realField setPlaceholderString:@"-0.745"];
    [self addSubview:_realField];

    y -= CONTROL_SPACING;

    // Imaginary label
    y -= LABEL_HEIGHT;
    _imagLabel = [self createLabelWithText:@"Imaginary:" frame:NSMakeRect(PANEL_PADDING, y, contentWidth, LABEL_HEIGHT)];
    [self addSubview:_imagLabel];

    // Imaginary input field (no formatter — see above)
    y -= CONTROL_HEIGHT;
    _imagField = [self createTextFieldWithFrame:NSMakeRect(PANEL_PADDING, y, contentWidth, CONTROL_HEIGHT)];
    [_imagField setPlaceholderString:@"0.113"];
    [self addSubview:_imagField];

    y -= CONTROL_SPACING;

    // Jump and Copy buttons (side by side)
    y -= CONTROL_HEIGHT;
    CGFloat buttonWidth = (contentWidth - CONTROL_SPACING) / 2;

    _jumpButton = [[NSButton alloc] initWithFrame:NSMakeRect(PANEL_PADDING, y, buttonWidth, CONTROL_HEIGHT)];
    [_jumpButton setButtonType:NSButtonTypeMomentaryPushIn];
    [_jumpButton setBezelStyle:NSBezelStyleRounded];
    [_jumpButton setTitle:@"Jump"];
    [_jumpButton setTarget:self];
    [_jumpButton setAction:@selector(jumpButtonClicked:)];
    [self addSubview:_jumpButton];

    _coordCopyButton = [[NSButton alloc] initWithFrame:NSMakeRect(PANEL_PADDING + buttonWidth + CONTROL_SPACING, y, buttonWidth, CONTROL_HEIGHT)];
    [_coordCopyButton setButtonType:NSButtonTypeMomentaryPushIn];
    [_coordCopyButton setBezelStyle:NSBezelStyleRounded];
    [_coordCopyButton setTitle:@"Copy"];
    [_coordCopyButton setTarget:self];
    [_coordCopyButton setAction:@selector(copyButtonClicked:)];
    [self addSubview:_coordCopyButton];

    y -= CONTROL_SPACING * 2;

    // --- Zoom Slider (read-only display) ---
    y -= LABEL_HEIGHT;
    _zoomLabel = [self createLabelWithText:@"Zoom Level:" frame:NSMakeRect(PANEL_PADDING, y, contentWidth - 60, LABEL_HEIGHT)];
    [self addSubview:_zoomLabel];

    _zoomValueLabel = [self createLabelWithText:@"1.0x" frame:NSMakeRect(PANEL_PADDING + contentWidth - 60, y, 60, LABEL_HEIGHT)];
    [_zoomValueLabel setAlignment:NSTextAlignmentRight];
    [self addSubview:_zoomValueLabel];

    y -= SLIDER_HEIGHT;
    _zoomSlider = [[NSSlider alloc] initWithFrame:NSMakeRect(PANEL_PADDING, y, contentWidth, SLIDER_HEIGHT)];
    [_zoomSlider setMinValue:0.0];
    [_zoomSlider setMaxValue:1.0];
    [_zoomSlider setDoubleValue:0.0];
    [_zoomSlider setEnabled:NO];  // Read-only display
    [_zoomSlider setTarget:self];
    [_zoomSlider setAction:@selector(zoomSliderChanged:)];
    [self addSubview:_zoomSlider];

    y -= CONTROL_SPACING * 2;

    // --- Animation Speed Slider ---
    y -= LABEL_HEIGHT;
    _animSpeedLabel = [self createLabelWithText:@"Animation Speed:" frame:NSMakeRect(PANEL_PADDING, y, contentWidth - 50, LABEL_HEIGHT)];
    [self addSubview:_animSpeedLabel];

    _animSpeedValueLabel = [self createLabelWithText:@"0.3s" frame:NSMakeRect(PANEL_PADDING + contentWidth - 50, y, 50, LABEL_HEIGHT)];
    [_animSpeedValueLabel setAlignment:NSTextAlignmentRight];
    [self addSubview:_animSpeedValueLabel];

    y -= SLIDER_HEIGHT;
    _animSpeedSlider = [[NSSlider alloc] initWithFrame:NSMakeRect(PANEL_PADDING, y, contentWidth, SLIDER_HEIGHT)];
    [_animSpeedSlider setMinValue:ANIM_SPEED_MIN];
    [_animSpeedSlider setMaxValue:ANIM_SPEED_MAX];
    [_animSpeedSlider setDoubleValue:0.3];
    [_animSpeedSlider setContinuous:YES];
    [_animSpeedSlider setTarget:self];
    [_animSpeedSlider setAction:@selector(animSpeedSliderChanged:)];
    [self addSubview:_animSpeedSlider];

    y -= CONTROL_SPACING * 2;

    // --- Color Cycle Scale Slider ---
    y -= LABEL_HEIGHT;
    _colorCycleLabel = [self createLabelWithText:@"Color Cycle:" frame:NSMakeRect(PANEL_PADDING, y, contentWidth - 50, LABEL_HEIGHT)];
    [self addSubview:_colorCycleLabel];

    _colorCycleValueLabel = [self createLabelWithText:@"64" frame:NSMakeRect(PANEL_PADDING + contentWidth - 50, y, 50, LABEL_HEIGHT)];
    [_colorCycleValueLabel setAlignment:NSTextAlignmentRight];
    [self addSubview:_colorCycleValueLabel];

    y -= SLIDER_HEIGHT;
    _colorCycleSlider = [[NSSlider alloc] initWithFrame:NSMakeRect(PANEL_PADDING, y, contentWidth, SLIDER_HEIGHT)];
    [_colorCycleSlider setMinValue:0.0];  // Logarithmic: 0 = 8, 1 = 512
    [_colorCycleSlider setMaxValue:1.0];
    [_colorCycleSlider setDoubleValue:0.5];  // Default: 64 (middle of log scale)
    [_colorCycleSlider setContinuous:YES];
    [_colorCycleSlider setTarget:self];
    [_colorCycleSlider setAction:@selector(colorCycleSliderChanged:)];
    [self addSubview:_colorCycleSlider];

    y -= CONTROL_SPACING * 2;

    // --- Help text at bottom ---
    y -= LABEL_HEIGHT * 2;
    NSTextField *helpLabel = [self createLabelWithText:@"Tab: toggle panel\nArrows/drag: pan" frame:NSMakeRect(PANEL_PADDING, y, contentWidth, LABEL_HEIGHT * 2)];
    [helpLabel setTextColor:[NSColor colorWithWhite:0.5 alpha:1.0]];
    [helpLabel setFont:[NSFont systemFontOfSize:10]];
    [self addSubview:helpLabel];
}

- (NSTextField *)createLabelWithText:(NSString *)text frame:(NSRect)frame {
    NSTextField *label = [[NSTextField alloc] initWithFrame:frame];
    [label setStringValue:text];
    [label setBezeled:NO];
    [label setDrawsBackground:NO];
    [label setEditable:NO];
    [label setSelectable:NO];
    [label setTextColor:[NSColor colorWithWhite:0.8 alpha:1.0]];
    [label setFont:[NSFont systemFontOfSize:11]];
    return label;
}

- (NSTextField *)createTextFieldWithFrame:(NSRect)frame {
    NSTextField *field = [[NSTextField alloc] initWithFrame:frame];
    [field setBezeled:YES];
    [field setBezelStyle:NSTextFieldSquareBezel];
    [field setDrawsBackground:YES];
    [field setBackgroundColor:[NSColor colorWithWhite:0.2 alpha:1.0]];
    [field setTextColor:[NSColor whiteColor]];
    [field setFont:[NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightRegular]];
    [field setEditable:YES];
    [field setSelectable:YES];
    [field setAllowsEditingTextAttributes:NO];
    return field;
}

// =============================================================================
// Button Actions
// =============================================================================

- (void)collapseButtonClicked:(id)sender {
    (void)sender;
    [self toggleCollapsed];
}

- (void)jumpButtonClicked:(id)sender {
    (void)sender;

    // Parse coordinate values
    NSString *realStr = [_realField stringValue];
    NSString *imagStr = [_imagField stringValue];

    if ([realStr length] == 0 || [imagStr length] == 0) {
        NSBeep();
        return;
    }

    // Validate as numbers (supports scientific notation)
    double real = [self parseCoordinate:realStr];
    double imag = [self parseCoordinate:imagStr];

    if (isnan(real) || isnan(imag)) {
        NSBeep();
        return;
    }

    // Notify delegate with the raw strings (full precision for deep zoom)
    id<MBControlPanelDelegate> delegate = _delegate;
    if (delegate && [delegate respondsToSelector:@selector(controlPanel:didRequestJumpToRealString:imagString:)]) {
        [delegate controlPanel:self didRequestJumpToRealString:realStr imagString:imagStr];
    }
}

- (void)copyButtonClicked:(id)sender {
    (void)sender;

    id<MBControlPanelDelegate> delegate = _delegate;
    if (delegate && [delegate respondsToSelector:@selector(controlPanelDidRequestCopyCoordinates:)]) {
        [delegate controlPanelDidRequestCopyCoordinates:self];
    }
}

- (double)parseCoordinate:(NSString *)str {
    // Handle scientific notation and regular decimals
    NSScanner *scanner = [NSScanner scannerWithString:str];
    double value;
    if ([scanner scanDouble:&value] && [scanner isAtEnd]) {
        return value;
    }
    return NAN;
}

// =============================================================================
// Slider Actions
// =============================================================================

- (void)zoomSliderChanged:(id)sender {
    (void)sender;
    // Zoom slider is read-only, but in case it becomes editable:
    // double sliderValue = [_zoomSlider doubleValue];
    // double zoom = pow(10.0, sliderValue * ZOOM_LOG_MAX);
}

- (void)animSpeedSliderChanged:(id)sender {
    (void)sender;
    double seconds = [_animSpeedSlider doubleValue];

    // Update value label
    [_animSpeedValueLabel setStringValue:[NSString stringWithFormat:@"%.1fs", seconds]];

    // Notify delegate
    id<MBControlPanelDelegate> delegate = _delegate;
    if (delegate && [delegate respondsToSelector:@selector(controlPanel:didChangeAnimationSpeed:)]) {
        [delegate controlPanel:self didChangeAnimationSpeed:seconds];
    }
}

- (void)colorCycleSliderChanged:(id)sender {
    (void)sender;

    // Convert from linear [0,1] to logarithmic [8, 512]
    // 0.0 -> 8, 0.5 -> ~64, 1.0 -> 512
    double sliderValue = [_colorCycleSlider doubleValue];
    double logMin = log(COLOR_CYCLE_MIN);
    double logMax = log(COLOR_CYCLE_MAX);
    float scale = (float)exp(logMin + sliderValue * (logMax - logMin));

    // Update value label
    [_colorCycleValueLabel setStringValue:[NSString stringWithFormat:@"%.0f", scale]];

    // Notify delegate
    id<MBControlPanelDelegate> delegate = _delegate;
    if (delegate && [delegate respondsToSelector:@selector(controlPanel:didChangeColorCycleScale:)]) {
        [delegate controlPanel:self didChangeColorCycleScale:scale];
    }
}

// =============================================================================
// Public Methods
// =============================================================================

- (void)updateCoordinatesFromViewState:(const MBViewState *)viewState {
    if (!viewState) return;

    if (viewState->high_precision_mode) {
        // Beyond double precision the strings are the authoritative center
        [_realField setStringValue:[NSString stringWithUTF8String:viewState->center_x_str]];
        [_imagField setStringValue:[NSString stringWithUTF8String:viewState->center_y_str]];
    } else {
        [_realField setStringValue:[NSString stringWithFormat:@"%.15g", viewState->center_x]];
        [_imagField setStringValue:[NSString stringWithFormat:@"%.15g", viewState->center_y]];
    }
}

- (void)setCoordinateReal:(double)real imag:(double)imag {
    [_realField setStringValue:[NSString stringWithFormat:@"%.15g", real]];
    [_imagField setStringValue:[NSString stringWithFormat:@"%.15g", imag]];
}

- (void)updateZoomDisplay:(double)zoomLevel {
    // Convert zoom to slider position (logarithmic)
    double sliderValue = 0.0;
    if (zoomLevel > 1.0) {
        sliderValue = log10(zoomLevel) / ZOOM_LOG_MAX;
        if (sliderValue > 1.0) sliderValue = 1.0;
    }
    [_zoomSlider setDoubleValue:sliderValue];

    // Update value label
    if (zoomLevel >= 1e15) {
        [_zoomValueLabel setStringValue:[NSString stringWithFormat:@"%.1e", zoomLevel]];
    } else if (zoomLevel >= 1e6) {
        [_zoomValueLabel setStringValue:[NSString stringWithFormat:@"%.2eX", zoomLevel]];
    } else {
        [_zoomValueLabel setStringValue:[NSString stringWithFormat:@"%.1fx", zoomLevel]];
    }
}

- (void)setAnimationSpeed:(double)seconds {
    // Clamp to valid range
    if (seconds < ANIM_SPEED_MIN) seconds = ANIM_SPEED_MIN;
    if (seconds > ANIM_SPEED_MAX) seconds = ANIM_SPEED_MAX;

    [_animSpeedSlider setDoubleValue:seconds];
    [_animSpeedValueLabel setStringValue:[NSString stringWithFormat:@"%.1fs", seconds]];
}

- (void)setColorCycleScale:(float)scale {
    // Clamp to valid range
    if (scale < COLOR_CYCLE_MIN) scale = (float)COLOR_CYCLE_MIN;
    if (scale > COLOR_CYCLE_MAX) scale = (float)COLOR_CYCLE_MAX;

    // Convert to slider position (logarithmic)
    double logMin = log(COLOR_CYCLE_MIN);
    double logMax = log(COLOR_CYCLE_MAX);
    double sliderValue = (log(scale) - logMin) / (logMax - logMin);

    [_colorCycleSlider setDoubleValue:sliderValue];
    [_colorCycleValueLabel setStringValue:[NSString stringWithFormat:@"%.0f", scale]];
}

- (void)toggleCollapsed {
    [self setCollapsed:!_isCollapsed animated:YES];
}

- (void)setCollapsed:(BOOL)collapsed animated:(BOOL)animated {
    if (_isCollapsed == collapsed) return;
    _isCollapsed = collapsed;

    // Update button title
    [_collapseButton setTitle:collapsed ? @"> Show Controls" : @"< Hide Controls"];

    // Find the split view and collapse/expand
    NSSplitView *splitView = (NSSplitView *)self.superview;
    if (![splitView isKindOfClass:[NSSplitView class]]) {
        // Direct parent might not be the split view
        splitView = (NSSplitView *)self.superview.superview;
        if (![splitView isKindOfClass:[NSSplitView class]]) {
            return;
        }
    }

    NSView *firstView = [[splitView subviews] objectAtIndex:0];

    if (animated) {
        [NSAnimationContext runAnimationGroup:^(NSAnimationContext *context) {
            context.duration = 0.2;
            if (collapsed) {
                [splitView setPosition:0.0 ofDividerAtIndex:0];
            } else {
                [splitView setPosition:MB_CONTROL_PANEL_WIDTH ofDividerAtIndex:0];
            }
        } completionHandler:nil];
    } else {
        if (collapsed) {
            [firstView setFrameSize:NSMakeSize(0, firstView.frame.size.height)];
        } else {
            [firstView setFrameSize:NSMakeSize(MB_CONTROL_PANEL_WIDTH, firstView.frame.size.height)];
        }
        [splitView adjustSubviews];
    }
}

// =============================================================================
// Drawing
// =============================================================================

- (BOOL)isFlipped {
    // Use top-left origin for easier layout
    return YES;
}

- (void)drawRect:(NSRect)dirtyRect {
    [super drawRect:dirtyRect];

    // Draw separator line on right edge
    [[NSColor colorWithWhite:0.3 alpha:1.0] setStroke];
    NSBezierPath *line = [NSBezierPath bezierPath];
    [line moveToPoint:NSMakePoint(self.bounds.size.width - 0.5, 0)];
    [line lineToPoint:NSMakePoint(self.bounds.size.width - 0.5, self.bounds.size.height)];
    [line setLineWidth:1.0];
    [line stroke];
}

@end
