#import <Cocoa/Cocoa.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <CoreText/CoreText.h>
#include "native_viewer.h"
#include "control_panel.h"
#include "overlay_zoom_slider.h"
#include "coordinate_marker.h"
#include "../compute/compute_scheduler.h"
#include "../tile_map/tile_map.h"
#include "../tile_cache/disk_cache.h"
#include "../color/color.h"
#include "../color/palettes.h"
#include "../mandelbrot/mandelbrot.h"
#include "../perturbation/deep_render.h"
#include "../precision/view_hp.h"
#include "../precision/mp_real.h"
#include "../cinematic/director.h"
#include <stdlib.h>
#include <math.h>
#include <stdatomic.h>

// How long the view must be stable before deep tiles are computed. While the
// user is actively zooming/panning, only the cheap reprojected placeholder is
// drawn; expensive perturbation tiles start once they stop.
#define MB_DEEP_SETTLE_SECONDS 0.15

// =============================================================================
// Debug Logging (disabled in release builds)
// =============================================================================

#ifdef DEBUG
#define MB_DEBUG_LOG(fmt, ...) NSLog(fmt, ##__VA_ARGS__)
#else
#define MB_DEBUG_LOG(fmt, ...) ((void)0)
#endif

// Default cache paths
#define MB_CACHE_ENV_VAR "MB_CACHE_DIR"
#define MB_CACHE_DEFAULT_MACOS "~/Library/Caches/Mandelbrot/tiles"
#define MB_CACHE_FALLBACK "~/.mandelbrot/tiles"

// Get cache path from environment or default
static NSString* getCachePath(void) {
    const char *envPath = getenv(MB_CACHE_ENV_VAR);
    if (envPath && envPath[0] != '\0') {
        return [NSString stringWithUTF8String:envPath];
    }
    // macOS standard cache location
    return [@MB_CACHE_DEFAULT_MACOS stringByExpandingTildeInPath];
}

// =============================================================================
// Constants
// =============================================================================

#define TILE_SIZE MB_INTERACTIVE_TILE_SIZE
#define MAX_ITER 1000

// Minimap dimensions and settings
#define MINIMAP_WIDTH 150
#define MINIMAP_HEIGHT 120
#define MIN_INDICATOR_SIZE 20       // Minimum viewport indicator size in pixels
#define MINIMAP_MAX_ITER 500        // Lower iteration limit for minimap (performance)

// Mandelbrot set bounds (standard view encompasses the full set)
#define MB_FULL_MIN_CX (-2.5)
#define MB_FULL_MAX_CX (1.0)
#define MB_FULL_MIN_CY (-1.2)
#define MB_FULL_MAX_CY (1.2)
#define MB_FULL_WIDTH (MB_FULL_MAX_CX - MB_FULL_MIN_CX)   // 3.5
#define MB_FULL_HEIGHT (MB_FULL_MAX_CY - MB_FULL_MIN_CY)  // 2.4

// Zoom factors
#define MB_ZOOM_IN_FACTOR 1.5
#define MB_ZOOM_OUT_FACTOR 0.67

// Scroll sensitivity
#define MB_SCROLL_SENSITIVITY 0.05

// Preset locations for quick navigation
typedef struct {
    const char *name;
    double center_x;
    double center_y;
    double zoom;
    char key;
} PresetLocation;

// =============================================================================
// Key Handler Dispatch Table
// =============================================================================

// Forward declaration for MandelbrotView
@class MandelbrotView;

// Key handler function type
typedef void (*KeyHandlerFunc)(MandelbrotView *self);

// Key binding entry
typedef struct {
    unichar key;
    unichar altKey;           // Secondary key (e.g., 'R' for 'r'), 0 if none
    KeyHandlerFunc handler;
    BOOL closesPresetMenu;    // If YES, also closes preset menu when triggered
} KeyBinding;

// Handler function declarations
static void handleResetView(MandelbrotView *self);
static void handleZoomIn(MandelbrotView *self);
static void handleZoomOut(MandelbrotView *self);
static void handleToggleHUD(MandelbrotView *self);
static void handleToggleMinimap(MandelbrotView *self);
static void handleToggleCoordinates(MandelbrotView *self);
static void handleTogglePresetMenu(MandelbrotView *self);
static void handleEscape(MandelbrotView *self);
static void handlePanUp(MandelbrotView *self);
static void handlePanDown(MandelbrotView *self);
static void handlePanLeft(MandelbrotView *self);
static void handlePanRight(MandelbrotView *self);
static void handlePresetKey(MandelbrotView *self, unichar key);
static void handleClearCache(MandelbrotView *self);
static void handleCyclePalette(MandelbrotView *self);
static void handleToggleSmoothColoring(MandelbrotView *self);
static void handleToggleAntialiasing(MandelbrotView *self);
static void handleDecreaseColorCycleScale(MandelbrotView *self);
static void handleIncreaseColorCycleScale(MandelbrotView *self);
static void handleToggleControlPanel(MandelbrotView *self);
static void handleToggleCoordinatePicker(MandelbrotView *self);
static void handleToggleCinematic(MandelbrotView *self);

// Static key bindings table (non-preset keys)
static const KeyBinding kKeyBindings[] = {
    { 'r', 'R', handleResetView,          YES },
    { '=', '+', handleZoomIn,             YES },
    { '-', 0,   handleZoomOut,            YES },
    { 'i', 'I', handleToggleHUD,          NO  },
    { 'm', 'M', handleToggleMinimap,      NO  },
    { 'c', 'C', handleToggleCoordinates,  NO  },
    { 'p', 'P', handleTogglePresetMenu,   NO  },
    { 27,  0,   handleEscape,             NO  },  // Escape key
    { 'x', 'X', handleClearCache,         YES },  // Clear all caches
    { 'n', 'N', handleCyclePalette,       YES },  // Cycle color palette
    { 's', 'S', handleToggleSmoothColoring, YES }, // Toggle smooth coloring
    { 'a', 'A', handleToggleAntialiasing, YES }, // Toggle antialiasing (MSAA)
    { '[', '{', handleDecreaseColorCycleScale, YES }, // More color bands (tighter cycling)
    { ']', '}', handleIncreaseColorCycleScale, YES }, // Fewer color bands (smoother)
    { '\t', 0, handleToggleControlPanel, NO }, // Tab: Toggle control panel
    { 'g', 'G', handleToggleCoordinatePicker, NO }, // G: Toggle coordinate picker
    { 'v', 'V', handleToggleCinematic, YES }, // V: Cinematic autopilot zoom
};
static const int kKeyBindingsCount = sizeof(kKeyBindings) / sizeof(kKeyBindings[0]);

// =============================================================================
// Preset Locations
// =============================================================================

static const PresetLocation kPresetLocations[] = {
    // === Classic Locations ===
    {"Seahorse Valley",      -0.745,              0.113,              5e3,   '1'},
    {"Elephant Valley",       0.275,              0.006,              200,   '2'},
    {"Double Spiral",        -0.7436438870372,    0.1318259043,       1e6,   '3'},
    {"Mini Mandelbrot",      -1.768778833,       -0.001738996,        1e8,   '4'},
    {"Spiral Arms",          -0.761574,          -0.0847596,          5e4,   '5'},

    // === Deep Zoom Locations ===
    {"Seahorse Tail",        -0.74364085,         0.13182733,         1e8,   '6'},
    {"Lightning",            -1.315180982097868,  0.073481649996795,  1e10,  '7'},
    {"Starfish",             -0.374004139,        0.659792175,        5e5,   '8'},
    {"Dendrite",              0.2501,              0.0,                1e4,   '9'},

    // === Spirals & Patterns (menu-only, no hotkey) ===
    {"Julia Island",         -1.768,              0.0,                 200,   0},
    {"Quad Spiral",          -0.745428,           0.113009,           5e6,   0},
    {"Tendrils",             -0.22815,           -1.11514,            5e3,   0},
    {"Scepter Valley",       -1.25066,            0.02012,            1e4,   0},
    {"Period-3 Bulb",        -0.122,              0.745,              500,   0},
    {"Period-4 Bulb",         0.282,             -0.01,               500,   0},
};
static const int kPresetCount = sizeof(kPresetLocations) / sizeof(kPresetLocations[0]);

// =============================================================================
// MandelbrotView - Custom NSView with Metal rendering
// =============================================================================

@interface MandelbrotView : NSView <MBControlPanelDelegate, MBZoomSliderOverlayDelegate, MBCoordinateMarkerDelegate> {
    CAMetalLayer *_metalLayer;
    id<MTLDevice> _device;
    id<MTLCommandQueue> _commandQueue;
    id<MTLTexture> _texture;
    id<MTLRenderPipelineState> _pipelineState;

    MBViewState _viewState;
    ComputeScheduler _scheduler;
    PixelColor *_framebuffer;

    BOOL _needsRedraw;
    NSTimer *_displayTimer;

    // For smooth animation
    double _targetCenterX, _targetCenterY;
    double _targetZoom;
    BOOL _animating;

    // For mouse drag panning
    NSPoint _lastDragPoint;

    // Map tile buffers
    PixelColor *_mapTileBuf;        // 256x256 tile buffer
    PixelColor *_scaledTileBuf;     // For upscaling low-res tiles

    // Async tile computation
    dispatch_queue_t _tileQueue;
    NSMutableSet<NSString *> *_pendingTiles;
    NSMutableDictionary<NSString *, NSData *> *_asyncTileCache;
    NSLock *_asyncCacheLock;

    // Parent tile fallback buffer
    PixelColor *_parentTileBuf;

    // HUD display
    BOOL _showHUD;

    // High-precision mode tracking
    BOOL _highPrecisionMode;
    uint32_t _currentPrecision;

    // Animation system (zoom values stored as log10)
    double _animStartCenterX, _animStartCenterY, _animStartZoom;
    double _animTargetCenterX, _animTargetCenterY, _animTargetZoom;
    double _animProgress;           // 0.0 to 1.0
    double _animDuration;           // seconds (default 0.3)
    NSDate *_animStartTime;

    // Minimap
    PixelColor *_minimapBuffer;     // Pre-rendered thumbnail (150x120)
    PixelColor *_minimapZoomedBuffer;  // Cached zoomed minimap region
    double _minimapCachedCenterX;      // Center X of cached zoomed view
    double _minimapCachedCenterY;      // Center Y of cached zoomed view
    double _minimapCachedZoomLevel;    // Minimap zoom level (1.0 = full view)
    BOOL _showMinimap;

    // Coordinate readout
    NSPoint _mousePosition;
    BOOL _mouseInView;
    BOOL _showCoordinates;
    NSTrackingArea *_trackingArea;
    CFAbsoluteTime _cineHudActivity;   // last mouse activity: hover-HUD timer
    BOOL _cineHudPinned;               // I key pins the cinematic HUD

    // Preset menu
    BOOL _showPresetMenu;
    int _selectedPresetIdx;

    // Render settings (smooth coloring, palette)
    MBRenderSettings _renderSettings;

    // Control panel reference (not retained - managed externally)
    MBControlPanel *_controlPanel;

    // Overlay controls
    MBZoomSliderOverlay *_zoomSlider;
    MBCoordinateMarker *_coordinateMarker;

    // Deep-zoom rendering (screen-space perturbation tiles)
    MBDeepRenderer *_deepRenderer;
    // Bumped on every view change (main thread); read by tile workers to
    // drop queued jobs whose view is already gone.
    _Atomic(uint64_t) _viewGeneration;
    // Last time the view changed; tile queuing waits for the view to settle
    // so rapid zooming does not compute tiles the user immediately discards.
    CFAbsoluteTime _lastViewChangeTime;
    BOOL _deepNeedsPoll;            // re-render while waiting for settle
    // Current map-tile pyramid level; map workers skip tiles queued for a
    // level far from what is being displayed now.
    _Atomic(int) _currentTargetZoom;

    // Last fully rendered deep frame, reprojected as a placeholder while
    // tiles of the current generation are still computing.
    PixelColor *_lastDeepFrame;
    int _lastDeepFrameW, _lastDeepFrameH;
    MBViewState _lastDeepFrameView;
    BOOL _lastDeepFrameValid;

    // Cinematic mode (autopilot zoom; see docs/CINEMATIC_DESIGN.md)
    _Atomic(bool) _cinematicMode;
    MBDirector *_cineDirector;
    dispatch_queue_t _cineQueue;     // serial: one keyframe render at a time
    _Atomic(int) _cinePumping;
    double _cineZ;                   // playback zoom (log10)
    double _cineSpeed;               // target speed, decades/second
    double _cineCurSpeed;            // eased actual speed
    CFAbsoluteTime _cineLastTick;
}

- (void)enterCinematic;
- (void)exitCinematic;
- (void)setCinematicSpeed:(double)decadesPerSecond;

- (instancetype)initWithFrame:(NSRect)frameRect;
- (void)invalidate;
- (MBViewState *)viewState;
- (void)loadRenderSettings;
- (void)saveRenderSettings;
- (void)setControlPanel:(MBControlPanel *)panel;
- (void)syncControlPanel;

@end

@implementation MandelbrotView

- (instancetype)initWithFrame:(NSRect)frameRect {
    MB_DEBUG_LOG(@"MandelbrotView initWithFrame: starting, size=%fx%f", frameRect.size.width, frameRect.size.height);
    self = [super initWithFrame:frameRect];
    if (self) {
        // Initialize view state FIRST (needed for makeBackingLayer)
        mb_view_state_init(&_viewState, (int)frameRect.size.width, (int)frameRect.size.height);

        // Initialize Metal (before wantsLayer triggers makeBackingLayer)
        _device = MTLCreateSystemDefaultDevice();
        if (!_device) {
            MB_DEBUG_LOG(@"Metal is not supported on this device");
            return nil;
        }
        MB_DEBUG_LOG(@"MandelbrotView: got Metal device: %@", _device.name);
        _commandQueue = [_device newCommandQueue];

        // Now enable layer-backing (this calls makeBackingLayer)
        self.wantsLayer = YES;

        // Ensure the layer has our device and correct size
        if (_metalLayer) {
            _metalLayer.device = _device;
            _metalLayer.drawableSize = CGSizeMake(frameRect.size.width, frameRect.size.height);
            MB_DEBUG_LOG(@"MandelbrotView: Metal layer configured with size %fx%f", frameRect.size.width, frameRect.size.height);
        } else {
            MB_DEBUG_LOG(@"MandelbrotView: WARNING - no metal layer after wantsLayer=YES");
        }

        // Initialize scheduler
        MB_DEBUG_LOG(@"MandelbrotView: initializing scheduler");
        if (scheduler_init(&_scheduler, TILE_SIZE, MAX_ITER) != 0) {
            MB_DEBUG_LOG(@"Failed to initialize compute scheduler");
            return nil;
        }
        MB_DEBUG_LOG(@"MandelbrotView: scheduler initialized");

        // Initialize disk cache (check MB_CACHE_DIR env var first)
        NSString *cachePath = getCachePath();
        if (scheduler_init_disk_cache(&_scheduler, [cachePath UTF8String], 0) != 0) {
            MB_DEBUG_LOG(@"Warning: Failed to initialize disk cache, tiles won't persist");
        } else {
            MB_DEBUG_LOG(@"MandelbrotView: disk cache initialized at %@", cachePath);
        }

        // Allocate map tile buffers (256x256)
        _mapTileBuf = malloc(MB_MAP_TILE_SIZE * MB_MAP_TILE_SIZE * sizeof(PixelColor));
        _scaledTileBuf = malloc(MB_MAP_TILE_SIZE * MB_MAP_TILE_SIZE * sizeof(PixelColor));
        if (!_mapTileBuf || !_scaledTileBuf) {
            MB_DEBUG_LOG(@"Failed to allocate map tile buffers");
            if (_mapTileBuf) free(_mapTileBuf);
            if (_scaledTileBuf) free(_scaledTileBuf);
            scheduler_cleanup(&_scheduler);
            return nil;
        }

        // Allocate framebuffer
        size_t pixels = (size_t)_viewState.viewport_width * _viewState.viewport_height;
        _framebuffer = malloc(pixels * sizeof(PixelColor));
        if (!_framebuffer) {
            free(_mapTileBuf);
            free(_scaledTileBuf);
            scheduler_cleanup(&_scheduler);
            return nil;
        }

        _needsRedraw = YES;
        _animating = NO;

        // Initialize async tile computation
        _tileQueue = dispatch_queue_create("com.mandelbrot.tiles", DISPATCH_QUEUE_CONCURRENT);
        _pendingTiles = [[NSMutableSet alloc] init];
        _asyncTileCache = [[NSMutableDictionary alloc] init];
        _asyncCacheLock = [[NSLock alloc] init];

        // Parent tile fallback buffer
        _parentTileBuf = malloc(MB_MAP_TILE_SIZE * MB_MAP_TILE_SIZE * sizeof(PixelColor));
        if (!_parentTileBuf) {
            MB_DEBUG_LOG(@"Failed to allocate parent tile buffer");
            free(_mapTileBuf);
            free(_scaledTileBuf);
            free(_framebuffer);
            scheduler_cleanup(&_scheduler);
            return nil;
        }

        // HUD enabled by default
        _showHUD = YES;

        // High-precision mode tracking
        _highPrecisionMode = NO;
        _currentPrecision = 64;

        // Animation system defaults
        _animProgress = 1.0;  // Not animating
        _animDuration = 0.3;  // 300ms default
        _animStartTime = nil;

        // Minimap setup
        _showMinimap = YES;
        _minimapBuffer = malloc(MINIMAP_WIDTH * MINIMAP_HEIGHT * sizeof(PixelColor));
        _minimapZoomedBuffer = malloc(MINIMAP_WIDTH * MINIMAP_HEIGHT * sizeof(PixelColor));
        _minimapCachedCenterX = 0.0;
        _minimapCachedCenterY = 0.0;
        _minimapCachedZoomLevel = 1.0;
        if (_minimapBuffer) {
            [self renderMinimapOnce];
        }

        // Coordinate readout setup
        _showCoordinates = YES;
        _mouseInView = NO;
        _mousePosition = NSMakePoint(0, 0);
        [self enableMouseTracking];

        // Create zoom slider overlay (right edge, vertically centered)
        CGFloat sliderY = (frameRect.size.height - MB_ZOOM_SLIDER_HEIGHT) / 2;
        NSRect sliderFrame = NSMakeRect(
            frameRect.size.width - MB_ZOOM_SLIDER_WIDTH - MB_ZOOM_SLIDER_MARGIN,
            sliderY,
            MB_ZOOM_SLIDER_WIDTH,
            MB_ZOOM_SLIDER_HEIGHT);
        _zoomSlider = [[MBZoomSliderOverlay alloc] initWithFrame:sliderFrame];
        _zoomSlider.delegate = self;
        _zoomSlider.autoresizingMask = NSViewMinXMargin | NSViewMinYMargin | NSViewMaxYMargin;
        [self addSubview:_zoomSlider];

        // Create coordinate marker overlay (covers entire view)
        _coordinateMarker = [[MBCoordinateMarker alloc] initWithFrame:self.bounds];
        _coordinateMarker.delegate = self;
        _coordinateMarker.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
        [self addSubview:_coordinateMarker];

        // Deep-zoom renderer (screen-space perturbation tiles)
        _deepRenderer = mb_deep_renderer_create();
        _viewGeneration = 1;
        _lastDeepFrame = NULL;
        _lastDeepFrameValid = NO;

        // Cinematic mode
        _cinematicMode = false;
        _cineDirector = NULL;
        _cineQueue = dispatch_queue_create("com.mandelbrot.cinematic", DISPATCH_QUEUE_SERIAL);
        // Slow-motion default: reads like screen-recording B-roll, and the
        // prefetch pipeline stays far ahead at this pace.
        _cineSpeed = 0.12;  // decades per second: one doubling every
                            // ~2.5s — Apple-screensaver slow by default
                            // (override with --speed)

        // Load render settings from user defaults or use defaults
        [self loadRenderSettings];

        // Disk tiles are stored post-colored; key them by the settings
        [self updateDiskCacheVariant];

        // Set up display timer for 60fps (simpler than CVDisplayLink, avoids threading issues)
        _displayTimer = [NSTimer scheduledTimerWithTimeInterval:1.0/60.0
                                                         target:self
                                                       selector:@selector(displayTimerFired:)
                                                       userInfo:nil
                                                        repeats:YES];
        [_displayTimer retain];  // Retain for non-ARC
        // Add to common run loop modes so it fires during tracking (mouse drag, etc.)
        [[NSRunLoop currentRunLoop] addTimer:_displayTimer forMode:NSEventTrackingRunLoopMode];
    }
    return self;
}

- (void)loadRenderSettings {
    NSUserDefaults *defaults = [NSUserDefaults standardUserDefaults];
    // Check if settings exist (first run detection)
    if ([defaults objectForKey:@"mb_color_mode"]) {
        _renderSettings.color_mode = (MBColorMode)[defaults integerForKey:@"mb_color_mode"];
        _renderSettings.palette_id = (MBPaletteId)[defaults integerForKey:@"mb_palette_id"];
        _renderSettings.antialiasing_enabled = [defaults boolForKey:@"mb_antialiasing"];
        _renderSettings.color_cycle_scale = [defaults floatForKey:@"mb_color_cycle_scale"];

        // Validate palette_id in case app was downgraded
        if (_renderSettings.palette_id >= MB_PALETTE_COUNT) {
            _renderSettings.palette_id = MB_PALETTE_CLASSIC;
        }
        // Validate color_cycle_scale range
        if (_renderSettings.color_cycle_scale < 8.0f || _renderSettings.color_cycle_scale > 512.0f) {
            _renderSettings.color_cycle_scale = 64.0f;
        }
    } else {
        // First run defaults
        _renderSettings.color_mode = MB_COLOR_MODE_CLASSIC;
        _renderSettings.palette_id = MB_PALETTE_CLASSIC;
        _renderSettings.antialiasing_enabled = NO;
        _renderSettings.color_cycle_scale = 64.0f;
    }
}

- (void)saveRenderSettings {
    NSUserDefaults *defaults = [NSUserDefaults standardUserDefaults];
    [defaults setInteger:_renderSettings.color_mode forKey:@"mb_color_mode"];
    [defaults setInteger:_renderSettings.palette_id forKey:@"mb_palette_id"];
    [defaults setBool:_renderSettings.antialiasing_enabled forKey:@"mb_antialiasing"];
    [defaults setFloat:_renderSettings.color_cycle_scale forKey:@"mb_color_cycle_scale"];
}

// =============================================================================
// Control Panel Integration
// =============================================================================

- (void)setControlPanel:(MBControlPanel *)panel {
    _controlPanel = panel;
    panel.delegate = self;
    [self syncControlPanel];
}

- (void)syncControlPanel {
    if (_controlPanel) {
        [_controlPanel updateCoordinatesFromViewState:&_viewState];
        [_controlPanel updateZoomDisplayLog10:mb_view_zoom_log10(&_viewState)];
        [_controlPanel setAnimationSpeed:_animDuration];
        [_controlPanel setColorCycleScale:_renderSettings.color_cycle_scale];
    }

    // Also update overlay controls
    if (_zoomSlider) {
        [_zoomSlider updateZoomLog10:mb_view_zoom_log10(&_viewState)];
    }
    if (_coordinateMarker) {
        [_coordinateMarker updateViewState:&_viewState];
    }
}

// =============================================================================
// MBControlPanelDelegate Protocol
// =============================================================================

- (void)controlPanel:(MBControlPanel *)panel didRequestJumpToRealString:(NSString *)realStr
          imagString:(NSString *)imagStr {
    (void)panel;

    // Always set the center from the raw strings in MPFR: a user may paste a
    // deep target while still zoomed out and zoom in afterwards, so the full
    // precision must be captured regardless of the current zoom. (Jumps do
    // not animate — the double-based animation path would truncate it.)
    if (mb_view_hp_set_center(&_viewState,
                              realStr.UTF8String, imagStr.UTF8String) == 0) {
        [self updateHPMode];
        [self invalidateCache];
        [self syncControlPanel];
    } else {
        NSBeep();
    }
}

- (void)controlPanel:(MBControlPanel *)panel didChangeAnimationSpeed:(double)seconds {
    (void)panel;
    _animDuration = seconds;
}

- (void)controlPanel:(MBControlPanel *)panel didChangeColorCycleScale:(float)scale {
    (void)panel;
    _renderSettings.color_cycle_scale = scale;
    [self invalidateRenderCache];
    [self saveRenderSettings];
}

- (void)controlPanelDidRequestCopyCoordinates:(MBControlPanel *)panel {
    (void)panel;

    // Format coordinates as "real + imag*i" or "real - abs(imag)*i".
    // In HP mode the strings carry the full precision.
    NSString *coordStr;
    if (_viewState.high_precision_mode) {
        NSString *re = [NSString stringWithUTF8String:_viewState.center_x_str];
        NSString *im = [NSString stringWithUTF8String:_viewState.center_y_str];
        if ([im hasPrefix:@"-"]) {
            coordStr = [NSString stringWithFormat:@"%@ - %@i", re, [im substringFromIndex:1]];
        } else {
            coordStr = [NSString stringWithFormat:@"%@ + %@i", re, im];
        }
    } else if (_viewState.center_y >= 0) {
        coordStr = [NSString stringWithFormat:@"%.17g + %.17gi",
                    _viewState.center_x, _viewState.center_y];
    } else {
        coordStr = [NSString stringWithFormat:@"%.17g - %.17gi",
                    _viewState.center_x, -_viewState.center_y];
    }

    // Copy to clipboard
    NSPasteboard *pb = [NSPasteboard generalPasteboard];
    [pb clearContents];
    [pb setString:coordStr forType:NSPasteboardTypeString];
}

// =============================================================================
// MBZoomSliderOverlayDelegate Protocol
// =============================================================================

- (void)zoomSliderOverlay:(MBZoomSliderOverlay *)slider didChangeZoomLog10:(double)zoomLog10 {
    (void)slider;
    // Animate to the new zoom level, keeping center position
    [self startAnimationToX:_viewState.center_x y:_viewState.center_y zoomLog10:zoomLog10];
}

- (void)zoomSliderOverlay:(MBZoomSliderOverlay *)slider didScrollWithDelta:(CGFloat)delta {
    (void)slider;
    // Fine zoom adjustment via scroll wheel on slider. Clamp the per-event
    // factor: a momentum flick can otherwise drive it to zero or negative,
    // teleporting the view to the zoom clamp boundary.
    double factor = 1.0 + delta * MB_SCROLL_SENSITIVITY;
    if (factor < 0.2) factor = 0.2;
    if (factor > 5.0) factor = 5.0;
    double newL10 = mb_clamp_zoom_log10(mb_view_zoom_log10(&_viewState) + log10(factor));
    [self startAnimationToX:_viewState.center_x y:_viewState.center_y zoomLog10:newL10];
}

// =============================================================================
// MBCoordinateMarkerDelegate Protocol
// =============================================================================

- (void)coordinateMarker:(MBCoordinateMarker *)marker didMoveToReal:(double)real imag:(double)imag {
    (void)marker;
    // Update control panel text fields to show marker coordinates
    if (_controlPanel) {
        [_controlPanel setCoordinateReal:real imag:imag];
    }
}

- (void)coordinateMarkerDidRequestNavigation:(MBCoordinateMarker *)marker {
    // Navigate to marker position, keeping current zoom
    [self startAnimationToX:marker.coordinateReal y:marker.coordinateImag
                  zoomLog10:mb_view_zoom_log10(&_viewState)];
}

- (void)coordinateMarker:(MBCoordinateMarker *)marker pickerModeChanged:(BOOL)active {
    (void)marker;
    (void)active;
    // Could update UI to indicate picker mode is active
    _needsRedraw = YES;
}

- (void)dealloc {
    if (_displayTimer) {
        [_displayTimer invalidate];
        [_displayTimer release];
        _displayTimer = nil;
    }
    if (_framebuffer) {
        free(_framebuffer);
    }
    if (_mapTileBuf) {
        free(_mapTileBuf);
    }
    if (_scaledTileBuf) {
        free(_scaledTileBuf);
    }
    if (_parentTileBuf) {
        free(_parentTileBuf);
    }
    if (_minimapBuffer) {
        free(_minimapBuffer);
    }
    if (_minimapZoomedBuffer) {
        free(_minimapZoomedBuffer);
    }
    if (_trackingArea) {
        [self removeTrackingArea:_trackingArea];
        [_trackingArea release];
    }
    if (_animStartTime) {
        [_animStartTime release];
    }
    scheduler_cleanup(&_scheduler);
    if (_texture) {
        [_texture release];
        _texture = nil;
    }
    if (_commandQueue) {
        [_commandQueue release];
        _commandQueue = nil;
    }
    if (_device) {
        [_device release];
        _device = nil;
    }
    // Release async tile resources
    if (_tileQueue) {
        dispatch_release(_tileQueue);
        _tileQueue = nil;
    }
    if (_pendingTiles) {
        [_pendingTiles release];
        _pendingTiles = nil;
    }
    if (_asyncTileCache) {
        [_asyncTileCache release];
        _asyncTileCache = nil;
    }
    if (_asyncCacheLock) {
        [_asyncCacheLock release];
        _asyncCacheLock = nil;
    }
    if (_zoomSlider) {
        [_zoomSlider release];
        _zoomSlider = nil;
    }
    if (_coordinateMarker) {
        [_coordinateMarker release];
        _coordinateMarker = nil;
    }
    if (_deepRenderer) {
        mb_deep_renderer_destroy(_deepRenderer);
        _deepRenderer = NULL;
    }
    if (_lastDeepFrame) {
        free(_lastDeepFrame);
        _lastDeepFrame = NULL;
    }
    atomic_store(&_cinematicMode, false);
    if (_cineQueue) {
        // Drain the worker (it exits after its current keyframe) before
        // destroying the director it renders into.
        dispatch_sync(_cineQueue, ^{});
        if (_cineDirector) {
            mb_director_destroy(_cineDirector);
            _cineDirector = NULL;
        }
        dispatch_release(_cineQueue);
        _cineQueue = nil;
    }
    [super dealloc];
}

- (CALayer *)makeBackingLayer {
    _metalLayer = [[CAMetalLayer alloc] init];
    _metalLayer.device = _device;
    _metalLayer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    _metalLayer.framebufferOnly = NO;
    _metalLayer.drawableSize = CGSizeMake(_viewState.viewport_width, _viewState.viewport_height);
    return _metalLayer;
}

- (void)setFrameSize:(NSSize)newSize {
    [super setFrameSize:newSize];

    // Keyframes bake the viewport size; leave cinematic mode and retire the
    // director (a fresh one is created at the new size on the next entry).
    // The destroy job runs on the serial queue AFTER the pump loop exits.
    if (atomic_load(&_cinematicMode)) {
        [self exitCinematic];
    }
    if (_cineDirector &&
        ((int)newSize.width != _viewState.viewport_width ||
         (int)newSize.height != _viewState.viewport_height)) {
        MBDirector *old = _cineDirector;
        _cineDirector = NULL;
        dispatch_async(_cineQueue, ^{
            mb_director_destroy(old);
        });
    }

    int oldWidth = _viewState.viewport_width;
    int oldHeight = _viewState.viewport_height;

    // Update viewport
    _viewState.viewport_width = (int)newSize.width;
    _viewState.viewport_height = (int)newSize.height;

    // Update Metal layer drawable size (use logical pixels, not scaled)
    if (_metalLayer) {
        _metalLayer.drawableSize = CGSizeMake(newSize.width, newSize.height);
    }

    // Resize framebuffer (keep old buffer if realloc fails)
    size_t pixels = (size_t)_viewState.viewport_width * _viewState.viewport_height;
    PixelColor *newBuf = realloc(_framebuffer, pixels * sizeof(PixelColor));
    if (newBuf) {
        _framebuffer = newBuf;
    } else {
        // Realloc failed — the framebuffer still has the old size, so the
        // viewport dimensions must be restored or the render loop would
        // write past the end of the old allocation.
        _viewState.viewport_width = oldWidth;
        _viewState.viewport_height = oldHeight;
        return;
    }

    // Resize texture
    [self createTexture];

    // Invalidate caches on resize. Deep tiles bake the viewport geometry
    // into their pixels (deltas are relative to the viewport center), so the
    // view generation must advance or stale tiles would be blitted with the
    // wrong mapping.
    [self invalidateCache];
}

- (void)createTexture {
    // Release old texture if exists
    if (_texture) {
        [_texture release];
        _texture = nil;
    }

    // Safety check for Metal device
    if (!_device) {
        return;
    }

    MTLTextureDescriptor *desc = [[MTLTextureDescriptor alloc] init];
    desc.pixelFormat = MTLPixelFormatBGRA8Unorm;  // Match CAMetalLayer format
    desc.width = _viewState.viewport_width;
    desc.height = _viewState.viewport_height;
    desc.usage = MTLTextureUsageShaderRead;
    _texture = [_device newTextureWithDescriptor:desc];  // newTextureWithDescriptor returns retained object
    [desc release];  // Release the descriptor
}

- (void)viewDidMoveToWindow {
    [super viewDidMoveToWindow];
    if (self.window) {
        // Use logical pixels for drawable size (matches texture size)
        _metalLayer.drawableSize = CGSizeMake(self.bounds.size.width, self.bounds.size.height);
        // Update viewport to match bounds
        _viewState.viewport_width = (int)self.bounds.size.width;
        _viewState.viewport_height = (int)self.bounds.size.height;
        [self createTexture];
        _needsRedraw = YES;
        // Trigger initial render
        [self renderFrame];
    }
}

- (BOOL)acceptsFirstResponder {
    return YES;
}

// =============================================================================
// High-Precision Mode Helpers
// =============================================================================

- (void)updateHPMode {
    BOOL needsHP = _viewState.zoom_level >= MB_HP_ZOOM_THRESHOLD;

    _highPrecisionMode = needsHP;
    _viewState.high_precision_mode = needsHP;
    _currentPrecision = needsHP
        ? (uint32_t)mp_required_precision_log10(mb_view_zoom_log10(&_viewState))
        : 64;
}

// =============================================================================
// Animation System
// =============================================================================

- (double)easeInOutCubic:(double)t {
    return t < 0.5 ? 4.0 * t * t * t : 1.0 - pow(-2.0 * t + 2.0, 3.0) / 2.0;
}

- (void)startAnimationToX:(double)x y:(double)y zoomLog10:(double)zoomL10 {
    zoomL10 = mb_clamp_zoom_log10(zoomL10);
    double curL10 = mb_view_zoom_log10(&_viewState);

    // Deep zoom: jump directly. The animation interpolates the center in
    // double precision, which would truncate an HP center; and when only the
    // zoom changes we must not touch the center strings at all.
    if (curL10 >= MB_DEEP_ZOOM_LOG10 || zoomL10 >= MB_DEEP_ZOOM_LOG10) {
        BOOL centerChanged = (x != _viewState.center_x || y != _viewState.center_y);
        mb_view_set_zoom_log10(&_viewState, zoomL10);
        if (centerChanged) {
            // Target is given as doubles (marker/preset); precision is
            // limited to double by the caller anyway.
            _viewState.center_x = x;
            _viewState.center_y = y;
            mb_view_hp_sync_from_doubles(&_viewState);
        }
        _animating = NO;
        [self updateHPMode];
        [self invalidateCache];
        [self syncControlPanel];
        return;
    }

    // Store starting state (zoom in log10)
    _animStartCenterX = _viewState.center_x;
    _animStartCenterY = _viewState.center_y;
    _animStartZoom = curL10;

    // Store target state
    _animTargetCenterX = x;
    _animTargetCenterY = y;
    _animTargetZoom = zoomL10;

    // Start animation
    _animProgress = 0.0;
    if (_animStartTime) {
        [_animStartTime release];
    }
    _animStartTime = [[NSDate date] retain];
    _animating = YES;
    _needsRedraw = YES;
}

- (void)updateAnimation {
    if (_animProgress >= 1.0 || !_animStartTime) {
        _animating = NO;
        return;
    }

    // Calculate elapsed time
    NSTimeInterval elapsed = -[_animStartTime timeIntervalSinceNow];
    _animProgress = elapsed / _animDuration;

    if (_animProgress >= 1.0) {
        // Animation complete
        _animProgress = 1.0;
        _viewState.center_x = _animTargetCenterX;
        _viewState.center_y = _animTargetCenterY;
        mb_view_set_zoom_log10(&_viewState, _animTargetZoom);
        _animating = NO;
        [_animStartTime release];
        _animStartTime = nil;
    } else {
        // Interpolate with easing
        double t = [self easeInOutCubic:_animProgress];

        _viewState.center_x = _animStartCenterX + (_animTargetCenterX - _animStartCenterX) * t;
        _viewState.center_y = _animStartCenterY + (_animTargetCenterY - _animStartCenterY) * t;

        // Zoom values are log10, so a linear lerp is logarithmic in zoom
        // (smooth feeling across large ranges)
        mb_view_set_zoom_log10(&_viewState,
                               _animStartZoom + (_animTargetZoom - _animStartZoom) * t);
    }

    // Update HP mode and sync strings (shallow zoom only: the deep path
    // never animates, so double precision is exact here)
    [self updateHPMode];
    mb_view_hp_sync_from_doubles(&_viewState);
    [self invalidateCache];

    // Update control panel displays
    [self syncControlPanel];
}

// =============================================================================
// Text Blitting Helper
// =============================================================================

- (void)blitTextBitmap:(uint8_t *)bitmap
                 width:(int)bmpWidth
                height:(int)bmpHeight
                   toX:(int)dstX
                     y:(int)dstY
{
    for (int ty = 0; ty < bmpHeight; ty++) {
        int srcY = bmpHeight - 1 - ty;  // Flip Y for CG coordinates
        int screenY = dstY + ty;
        if (screenY < 0 || screenY >= _viewState.viewport_height) continue;

        for (int tx = 0; tx < bmpWidth; tx++) {
            int screenX = dstX + tx;
            if (screenX < 0 || screenX >= _viewState.viewport_width) continue;

            int srcIdx = srcY * bmpWidth + tx;
            int dstIdx = screenY * _viewState.viewport_width + screenX;

            uint8_t sr = bitmap[srcIdx * 4 + 0];
            uint8_t sg = bitmap[srcIdx * 4 + 1];
            uint8_t sb = bitmap[srcIdx * 4 + 2];
            uint8_t sa = bitmap[srcIdx * 4 + 3];

            if (sa > 0) {
                float alpha = sa / 255.0f;
                _framebuffer[dstIdx].r = (uint8_t)(sr * alpha + _framebuffer[dstIdx].r * (1 - alpha));
                _framebuffer[dstIdx].g = (uint8_t)(sg * alpha + _framebuffer[dstIdx].g * (1 - alpha));
                _framebuffer[dstIdx].b = (uint8_t)(sb * alpha + _framebuffer[dstIdx].b * (1 - alpha));
            }
        }
    }
}

// =============================================================================
// Minimap
// =============================================================================

- (void)renderMinimapOnce {
    if (!_minimapBuffer) return;

    // Render full Mandelbrot set at minimap resolution
    double scaleX = MB_FULL_WIDTH / MINIMAP_WIDTH;
    double scaleY = MB_FULL_HEIGHT / MINIMAP_HEIGHT;

    for (int y = 0; y < MINIMAP_HEIGHT; y++) {
        double cy = MB_FULL_MAX_CY - (y + 0.5) * scaleY;  // Top-down
        for (int x = 0; x < MINIMAP_WIDTH; x++) {
            double cx = MB_FULL_MIN_CX + (x + 0.5) * scaleX;
            unsigned int iteration = mb_compute_point(cx, cy, MINIMAP_MAX_ITER);
            // color_from_iteration hardcodes MB_MAX_ITER as the interior
            // check; the minimap iterates to MINIMAP_MAX_ITER, so interior
            // pixels must be detected against that limit instead.
            color_from_iteration_classic(&_minimapBuffer[y * MINIMAP_WIDTH + x],
                                         iteration, MINIMAP_MAX_ITER);
        }
    }
}

- (void)renderMinimapZoomed:(double)zoom centerX:(double)cx centerY:(double)cy {
    if (!_minimapZoomedBuffer) return;

    // Calculate zoomed region bounds
    double zoomedWidth = MB_FULL_WIDTH / zoom;
    double zoomedHeight = MB_FULL_HEIGHT / zoom;

    double minCx = cx - zoomedWidth / 2.0;
    double maxCx = cx + zoomedWidth / 2.0;
    double minCy = cy - zoomedHeight / 2.0;
    double maxCy = cy + zoomedHeight / 2.0;

    // Clamp to full Mandelbrot bounds (so we don't render outside the set)
    if (minCx < MB_FULL_MIN_CX) {
        minCx = MB_FULL_MIN_CX;
        maxCx = minCx + zoomedWidth;
    }
    if (maxCx > MB_FULL_MAX_CX) {
        maxCx = MB_FULL_MAX_CX;
        minCx = maxCx - zoomedWidth;
    }
    if (minCy < MB_FULL_MIN_CY) {
        minCy = MB_FULL_MIN_CY;
        maxCy = minCy + zoomedHeight;
    }
    if (maxCy > MB_FULL_MAX_CY) {
        maxCy = MB_FULL_MAX_CY;
        minCy = maxCy - zoomedHeight;
    }

    double scaleX = (maxCx - minCx) / MINIMAP_WIDTH;
    double scaleY = (maxCy - minCy) / MINIMAP_HEIGHT;

    for (int y = 0; y < MINIMAP_HEIGHT; y++) {
        double py = maxCy - (y + 0.5) * scaleY;  // Top-down
        for (int x = 0; x < MINIMAP_WIDTH; x++) {
            double px = minCx + (x + 0.5) * scaleX;
            unsigned int iteration = mb_compute_point(px, py, MINIMAP_MAX_ITER);
            color_from_iteration_classic(&_minimapZoomedBuffer[y * MINIMAP_WIDTH + x],
                                         iteration, MINIMAP_MAX_ITER);
        }
    }

    // Update cache tracking
    _minimapCachedCenterX = cx;
    _minimapCachedCenterY = cy;
    _minimapCachedZoomLevel = zoom;
}

- (void)drawMinimapToFramebuffer {
    if (!_showMinimap || !_minimapBuffer) return;

    int padding = 10;
    int minimapX = padding;
    int minimapY = _viewState.viewport_height - MINIMAP_HEIGHT - padding;

    // Calculate viewport size in full minimap pixels
    double viewScale = mb_view_get_scale(&_viewState);
    double halfW = (_viewState.viewport_width / 2.0) * viewScale;
    double halfH = (_viewState.viewport_height / 2.0) * viewScale;

    double viewMinX = _viewState.center_x - halfW;
    double viewMaxX = _viewState.center_x + halfW;
    double viewMinY = _viewState.center_y - halfH;
    double viewMaxY = _viewState.center_y + halfH;
    double viewWidth = viewMaxX - viewMinX;

    // Calculate indicator size on full minimap
    double fullMapScaleX = MINIMAP_WIDTH / MB_FULL_WIDTH;
    double indicatorWidth = viewWidth * fullMapScaleX;

    // Determine if we need to zoom the minimap
    double minimapZoom = 1.0;
    BOOL useZoomedMinimap = NO;

    if (indicatorWidth < MIN_INDICATOR_SIZE && _minimapZoomedBuffer) {
        // Calculate zoom needed to make indicator visible
        minimapZoom = MIN_INDICATOR_SIZE / indicatorWidth;
        // The zoomed minimap renders in plain double precision; past ~1e9
        // it would be garbage, so cap it (the indicator then simply marks
        // the neighborhood rather than the exact viewport).
        if (minimapZoom > 1e9) minimapZoom = 1e9;
        useZoomedMinimap = YES;

        // Check if cached zoomed buffer is still valid:
        // - Zoom level within 2x of current?
        // - Center still within 50% of cached view bounds?
        BOOL cacheValid = NO;
        if (_minimapCachedZoomLevel > 0) {
            double zoomRatio = minimapZoom / _minimapCachedZoomLevel;
            if (zoomRatio >= 0.5 && zoomRatio <= 2.0) {
                // Check if center is still within cached bounds
                double cachedWidth = MB_FULL_WIDTH / _minimapCachedZoomLevel;
                double cachedHeight = MB_FULL_HEIGHT / _minimapCachedZoomLevel;
                double dx = fabs(_viewState.center_x - _minimapCachedCenterX);
                double dy = fabs(_viewState.center_y - _minimapCachedCenterY);
                // Valid if center moved less than 25% of cached view size
                if (dx < cachedWidth * 0.25 && dy < cachedHeight * 0.25) {
                    cacheValid = YES;
                }
            }
        }

        // Re-render zoomed minimap if cache is invalid
        if (!cacheValid) {
            [self renderMinimapZoomed:minimapZoom centerX:_viewState.center_x centerY:_viewState.center_y];
        }
    }

    // Select which buffer to use
    PixelColor *sourceBuffer = useZoomedMinimap ? _minimapZoomedBuffer : _minimapBuffer;

    // Draw semi-transparent border
    int borderWidth = 2;
    for (int y = minimapY - borderWidth; y < minimapY + MINIMAP_HEIGHT + borderWidth; y++) {
        if (y < 0 || y >= _viewState.viewport_height) continue;
        for (int x = minimapX - borderWidth; x < minimapX + MINIMAP_WIDTH + borderWidth; x++) {
            if (x < 0 || x >= _viewState.viewport_width) continue;

            // Check if in border region
            BOOL inBorder = (x < minimapX || x >= minimapX + MINIMAP_WIDTH ||
                            y < minimapY || y >= minimapY + MINIMAP_HEIGHT);
            if (inBorder) {
                int idx = y * _viewState.viewport_width + x;
                _framebuffer[idx].r = 100;
                _framebuffer[idx].g = 100;
                _framebuffer[idx].b = 100;
            }
        }
    }

    // Blit minimap (using selected buffer)
    for (int y = 0; y < MINIMAP_HEIGHT; y++) {
        int dstY = minimapY + y;
        if (dstY < 0 || dstY >= _viewState.viewport_height) continue;

        for (int x = 0; x < MINIMAP_WIDTH; x++) {
            int dstX = minimapX + x;
            if (dstX < 0 || dstX >= _viewState.viewport_width) continue;

            int dstIdx = dstY * _viewState.viewport_width + dstX;
            int srcIdx = y * MINIMAP_WIDTH + x;
            _framebuffer[dstIdx] = sourceBuffer[srcIdx];
        }
    }

    // Calculate the bounds of what's currently shown on the minimap
    double mapMinCx, mapMaxCx, mapMinCy, mapMaxCy;
    if (useZoomedMinimap) {
        // Zoomed minimap - use cached bounds
        double zoomedWidth = MB_FULL_WIDTH / _minimapCachedZoomLevel;
        double zoomedHeight = MB_FULL_HEIGHT / _minimapCachedZoomLevel;
        mapMinCx = _minimapCachedCenterX - zoomedWidth / 2.0;
        mapMaxCx = _minimapCachedCenterX + zoomedWidth / 2.0;
        mapMinCy = _minimapCachedCenterY - zoomedHeight / 2.0;
        mapMaxCy = _minimapCachedCenterY + zoomedHeight / 2.0;

        // Clamp to full Mandelbrot bounds (matching renderMinimapZoomed)
        if (mapMinCx < MB_FULL_MIN_CX) {
            mapMinCx = MB_FULL_MIN_CX;
            mapMaxCx = mapMinCx + zoomedWidth;
        }
        if (mapMaxCx > MB_FULL_MAX_CX) {
            mapMaxCx = MB_FULL_MAX_CX;
            mapMinCx = mapMaxCx - zoomedWidth;
        }
        if (mapMinCy < MB_FULL_MIN_CY) {
            mapMinCy = MB_FULL_MIN_CY;
            mapMaxCy = mapMinCy + zoomedHeight;
        }
        if (mapMaxCy > MB_FULL_MAX_CY) {
            mapMaxCy = MB_FULL_MAX_CY;
            mapMinCy = mapMaxCy - zoomedHeight;
        }
    } else {
        // Full minimap
        mapMinCx = MB_FULL_MIN_CX;
        mapMaxCx = MB_FULL_MAX_CX;
        mapMinCy = MB_FULL_MIN_CY;
        mapMaxCy = MB_FULL_MAX_CY;
    }

    // Draw viewport rectangle (red)
    double mapScaleX = MINIMAP_WIDTH / (mapMaxCx - mapMinCx);
    double mapScaleY = MINIMAP_HEIGHT / (mapMaxCy - mapMinCy);

    // Convert viewport to minimap pixel coords
    int rectX0 = minimapX + (int)((viewMinX - mapMinCx) * mapScaleX);
    int rectX1 = minimapX + (int)((viewMaxX - mapMinCx) * mapScaleX);
    int rectY0 = minimapY + (int)((mapMaxCy - viewMaxY) * mapScaleY);
    int rectY1 = minimapY + (int)((mapMaxCy - viewMinY) * mapScaleY);

    // Clamp to minimap bounds
    rectX0 = MAX(minimapX, MIN(minimapX + MINIMAP_WIDTH - 1, rectX0));
    rectX1 = MAX(minimapX, MIN(minimapX + MINIMAP_WIDTH - 1, rectX1));
    rectY0 = MAX(minimapY, MIN(minimapY + MINIMAP_HEIGHT - 1, rectY0));
    rectY1 = MAX(minimapY, MIN(minimapY + MINIMAP_HEIGHT - 1, rectY1));

    // Ensure minimum indicator size (at least 2 pixels if within bounds)
    if (rectX1 - rectX0 < 2 && rectX0 >= minimapX && rectX1 <= minimapX + MINIMAP_WIDTH - 2) {
        rectX1 = rectX0 + 2;
    }
    if (rectY1 - rectY0 < 2 && rectY0 >= minimapY && rectY1 <= minimapY + MINIMAP_HEIGHT - 2) {
        rectY1 = rectY0 + 2;
    }

    // Draw rectangle outline (red)
    PixelColor red = {255, 0, 0};

    // Top edge
    for (int x = rectX0; x <= rectX1; x++) {
        if (rectY0 >= 0 && rectY0 < _viewState.viewport_height && x >= 0 && x < _viewState.viewport_width) {
            _framebuffer[rectY0 * _viewState.viewport_width + x] = red;
        }
    }
    // Bottom edge
    for (int x = rectX0; x <= rectX1; x++) {
        if (rectY1 >= 0 && rectY1 < _viewState.viewport_height && x >= 0 && x < _viewState.viewport_width) {
            _framebuffer[rectY1 * _viewState.viewport_width + x] = red;
        }
    }
    // Left edge
    for (int y = rectY0; y <= rectY1; y++) {
        if (y >= 0 && y < _viewState.viewport_height && rectX0 >= 0 && rectX0 < _viewState.viewport_width) {
            _framebuffer[y * _viewState.viewport_width + rectX0] = red;
        }
    }
    // Right edge
    for (int y = rectY0; y <= rectY1; y++) {
        if (y >= 0 && y < _viewState.viewport_height && rectX1 >= 0 && rectX1 < _viewState.viewport_width) {
            _framebuffer[y * _viewState.viewport_width + rectX1] = red;
        }
    }
}

// =============================================================================
// Mouse Tracking & Coordinate Readout
// =============================================================================

- (void)enableMouseTracking {
    _trackingArea = [[NSTrackingArea alloc] initWithRect:self.bounds
                                                 options:(NSTrackingMouseMoved |
                                                         NSTrackingMouseEnteredAndExited |
                                                         NSTrackingActiveInKeyWindow |
                                                         NSTrackingInVisibleRect)
                                                   owner:self
                                                userInfo:nil];
    [self addTrackingArea:_trackingArea];
}

- (void)mouseMoved:(NSEvent *)event {
    NSPoint loc = [self convertPoint:event.locationInWindow fromView:nil];
    loc.y = self.bounds.size.height - loc.y;  // Flip Y
    _mousePosition = loc;
    _cineHudActivity = CFAbsoluteTimeGetCurrent();
    _needsRedraw = YES;
}

- (void)mouseEntered:(NSEvent *)event {
    _mouseInView = YES;
    _cineHudActivity = CFAbsoluteTimeGetCurrent();
    // Re-grab keyboard focus when the cursor returns to the fractal view:
    // clicking panel controls moves first responder there and keyboard
    // shortcuts silently die. Don't steal from an active text editor.
    NSResponder *fr = self.window.firstResponder;
    if (fr != self && ![fr isKindOfClass:[NSTextView class]]) {
        [self.window makeFirstResponder:self];
    }
    _needsRedraw = YES;
}

- (void)mouseExited:(NSEvent *)event {
    _mouseInView = NO;
    _needsRedraw = YES;
}

- (void)drawCoordinateReadout {
    if (!_showCoordinates || !_mouseInView) return;

    // Calculate complex coordinates at mouse position
    double scale = mb_view_get_scale(&_viewState);
    double cx = _viewState.center_x + (_mousePosition.x - _viewState.viewport_width / 2.0) * scale;
    double cy = _viewState.center_y - (_mousePosition.y - _viewState.viewport_height / 2.0) * scale;

    // Format coordinate string
    NSString *coordStr;
    if (cy >= 0) {
        coordStr = [NSString stringWithFormat:@"%.10f + %.10fi", cx, cy];
    } else {
        coordStr = [NSString stringWithFormat:@"%.10f - %.10fi", cx, -cy];
    }

    // Calculate position for text (offset from cursor)
    int textX = (int)_mousePosition.x + 15;
    int textY = (int)_mousePosition.y - 10;

    // Keep on screen
    int textWidth = 280;
    int textHeight = 20;
    if (textX + textWidth > _viewState.viewport_width) {
        textX = (int)_mousePosition.x - textWidth - 10;
    }
    if (textY < 0) {
        textY = (int)_mousePosition.y + 20;
    }
    if (textY + textHeight > _viewState.viewport_height) {
        textY = _viewState.viewport_height - textHeight - 5;
    }

    // Draw semi-transparent background
    int padding = 4;
    for (int y = textY - padding; y < textY + textHeight + padding && y < _viewState.viewport_height; y++) {
        if (y < 0) continue;
        for (int x = textX - padding; x < textX + textWidth + padding && x < _viewState.viewport_width; x++) {
            if (x < 0) continue;
            int idx = y * _viewState.viewport_width + x;
            _framebuffer[idx].r = _framebuffer[idx].r / 2;
            _framebuffer[idx].g = _framebuffer[idx].g / 2;
            _framebuffer[idx].b = _framebuffer[idx].b / 2;
        }
    }

    // Render text using Core Graphics
    uint8_t *textBitmap = calloc(textWidth * textHeight * 4, 1);
    if (!textBitmap) return;

    CGColorSpaceRef colorSpace = CGColorSpaceCreateDeviceRGB();
    CGContextRef ctx = CGBitmapContextCreate(textBitmap, textWidth, textHeight, 8,
                                              textWidth * 4, colorSpace,
                                              (CGBitmapInfo)kCGImageAlphaPremultipliedLast);
    CGColorSpaceRelease(colorSpace);
    if (!ctx) {
        free(textBitmap);
        return;
    }

    NSFont *font = [NSFont fontWithName:@"Menlo" size:11.0];
    if (!font) font = [NSFont monospacedSystemFontOfSize:11.0 weight:NSFontWeightRegular];
    NSColor *textColor = [NSColor colorWithRed:0.0 green:1.0 blue:0.8 alpha:1.0];  // Cyan-ish

    NSDictionary *attributes = @{
        NSFontAttributeName: font,
        NSForegroundColorAttributeName: textColor
    };

    NSAttributedString *attrStr = [[NSAttributedString alloc] initWithString:coordStr attributes:attributes];
    NSGraphicsContext *nsContext = [NSGraphicsContext graphicsContextWithCGContext:ctx flipped:YES];
    [NSGraphicsContext saveGraphicsState];
    [NSGraphicsContext setCurrentContext:nsContext];
    [attrStr drawInRect:NSMakeRect(0, 0, textWidth, textHeight)];
    [NSGraphicsContext restoreGraphicsState];

    // Blit text to framebuffer
    [self blitTextBitmap:textBitmap width:textWidth height:textHeight toX:textX y:textY];

    [attrStr release];
    CGContextRelease(ctx);
    free(textBitmap);
}

- (void)drawPresetMenuToFramebuffer {
    if (!_showPresetMenu) return;

    // Menu dimensions
    int menuWidth = 420;
    int lineHeight = 20;
    int padding = 15;
    int menuHeight = (kPresetCount + 3) * lineHeight + padding * 2;  // +3 for title, blank, and help

    // Center on screen
    int menuX = (_viewState.viewport_width - menuWidth) / 2;
    int menuY = (_viewState.viewport_height - menuHeight) / 2;

    // Clamp to screen bounds
    if (menuX < 10) menuX = 10;
    if (menuY < 10) menuY = 10;
    if (menuX + menuWidth > _viewState.viewport_width - 10) {
        menuX = _viewState.viewport_width - menuWidth - 10;
    }
    if (menuY + menuHeight > _viewState.viewport_height - 10) {
        menuY = _viewState.viewport_height - menuHeight - 10;
    }

    // 1. Draw semi-transparent background (70% opacity black)
    for (int y = menuY; y < menuY + menuHeight && y < _viewState.viewport_height; y++) {
        if (y < 0) continue;
        for (int x = menuX; x < menuX + menuWidth && x < _viewState.viewport_width; x++) {
            if (x < 0) continue;
            int idx = y * _viewState.viewport_width + x;
            _framebuffer[idx].r = (uint8_t)(_framebuffer[idx].r * 0.3);
            _framebuffer[idx].g = (uint8_t)(_framebuffer[idx].g * 0.3);
            _framebuffer[idx].b = (uint8_t)(_framebuffer[idx].b * 0.3);
        }
    }

    // Draw border
    for (int y = menuY; y < menuY + menuHeight && y < _viewState.viewport_height; y++) {
        if (y < 0) continue;
        // Left border
        if (menuX >= 0 && menuX < _viewState.viewport_width) {
            int idx = y * _viewState.viewport_width + menuX;
            _framebuffer[idx].r = 100; _framebuffer[idx].g = 100; _framebuffer[idx].b = 100;
        }
        // Right border
        int rx = menuX + menuWidth - 1;
        if (rx >= 0 && rx < _viewState.viewport_width) {
            int idx = y * _viewState.viewport_width + rx;
            _framebuffer[idx].r = 100; _framebuffer[idx].g = 100; _framebuffer[idx].b = 100;
        }
    }
    for (int x = menuX; x < menuX + menuWidth && x < _viewState.viewport_width; x++) {
        if (x < 0) continue;
        // Top border
        if (menuY >= 0 && menuY < _viewState.viewport_height) {
            int idx = menuY * _viewState.viewport_width + x;
            _framebuffer[idx].r = 100; _framebuffer[idx].g = 100; _framebuffer[idx].b = 100;
        }
        // Bottom border
        int by = menuY + menuHeight - 1;
        if (by >= 0 && by < _viewState.viewport_height) {
            int idx = by * _viewState.viewport_width + x;
            _framebuffer[idx].r = 100; _framebuffer[idx].g = 100; _framebuffer[idx].b = 100;
        }
    }

    // 2. Build menu text
    NSMutableString *menuText = [NSMutableString string];
    [menuText appendString:@"══════════ PRESETS ══════════\n\n"];

    for (int i = 0; i < kPresetCount; i++) {
        char keyStr[4] = "   ";
        if (kPresetLocations[i].key != 0) {
            keyStr[0] = '[';
            keyStr[1] = kPresetLocations[i].key;
            keyStr[2] = ']';
        }

        NSString *marker = (i == _selectedPresetIdx) ? @"> " : @"  ";
        [menuText appendFormat:@"%@%s %-18s  (%.1e)\n",
            marker, keyStr, kPresetLocations[i].name,
            kPresetLocations[i].zoom];
    }

    [menuText appendString:@"\n  Up/Down Navigate  Enter Select  P/Esc Close"];

    // 3. Render text using Core Graphics
    int textWidth = menuWidth - padding * 2;
    int textHeight = menuHeight - padding;
    uint8_t *textBitmap = calloc(textWidth * textHeight * 4, 1);
    if (!textBitmap) return;

    CGColorSpaceRef colorSpace = CGColorSpaceCreateDeviceRGB();
    CGContextRef ctx = CGBitmapContextCreate(textBitmap, textWidth, textHeight, 8,
                                              textWidth * 4, colorSpace,
                                              (CGBitmapInfo)kCGImageAlphaPremultipliedLast);
    CGColorSpaceRelease(colorSpace);
    if (!ctx) {
        free(textBitmap);
        return;
    }

    NSFont *font = [NSFont fontWithName:@"Menlo" size:12.0];
    if (!font) font = [NSFont monospacedSystemFontOfSize:12.0 weight:NSFontWeightRegular];

    // Create attributed string with different colors for selected item
    NSMutableAttributedString *attrStr = [[NSMutableAttributedString alloc] init];

    // Title
    NSDictionary *titleAttrs = @{
        NSFontAttributeName: font,
        NSForegroundColorAttributeName: [NSColor colorWithRed:0.3 green:0.8 blue:1.0 alpha:1.0]
    };
    [attrStr appendAttributedString:[[NSAttributedString alloc]
        initWithString:@"══════════ PRESETS ══════════\n\n" attributes:titleAttrs]];

    // Preset items
    NSDictionary *normalAttrs = @{
        NSFontAttributeName: font,
        NSForegroundColorAttributeName: [NSColor colorWithRed:0.8 green:0.8 blue:0.8 alpha:1.0]
    };
    NSDictionary *selectedAttrs = @{
        NSFontAttributeName: font,
        NSForegroundColorAttributeName: [NSColor colorWithRed:1.0 green:1.0 blue:0.2 alpha:1.0]
    };
    NSDictionary *hotkeyAttrs = @{
        NSFontAttributeName: font,
        NSForegroundColorAttributeName: [NSColor colorWithRed:0.5 green:0.8 blue:0.5 alpha:1.0]
    };

    for (int i = 0; i < kPresetCount; i++) {
        NSDictionary *lineAttrs = (i == _selectedPresetIdx) ? selectedAttrs : normalAttrs;
        NSString *marker = (i == _selectedPresetIdx) ? @"> " : @"  ";

        [attrStr appendAttributedString:[[NSAttributedString alloc]
            initWithString:marker attributes:lineAttrs]];

        if (kPresetLocations[i].key != 0) {
            NSString *keyStr = [NSString stringWithFormat:@"[%c] ", kPresetLocations[i].key];
            [attrStr appendAttributedString:[[NSAttributedString alloc]
                initWithString:keyStr attributes:hotkeyAttrs]];
        } else {
            [attrStr appendAttributedString:[[NSAttributedString alloc]
                initWithString:@"    " attributes:lineAttrs]];
        }

        NSString *nameAndZoom = [NSString stringWithFormat:@"%-18s  (%.1e)\n",
            kPresetLocations[i].name, kPresetLocations[i].zoom];
        [attrStr appendAttributedString:[[NSAttributedString alloc]
            initWithString:nameAndZoom attributes:lineAttrs]];
    }

    // Help line
    NSDictionary *helpAttrs = @{
        NSFontAttributeName: font,
        NSForegroundColorAttributeName: [NSColor colorWithRed:0.6 green:0.6 blue:0.6 alpha:1.0]
    };
    [attrStr appendAttributedString:[[NSAttributedString alloc]
        initWithString:@"\n  Up/Down Navigate  Enter Select  P/Esc Close" attributes:helpAttrs]];

    NSGraphicsContext *nsContext = [NSGraphicsContext graphicsContextWithCGContext:ctx flipped:YES];
    [NSGraphicsContext saveGraphicsState];
    [NSGraphicsContext setCurrentContext:nsContext];
    [attrStr drawInRect:NSMakeRect(0, 0, textWidth, textHeight)];
    [NSGraphicsContext restoreGraphicsState];

    // Blit text to framebuffer
    int textStartX = menuX + padding;
    int textStartY = menuY + padding / 2;
    [self blitTextBitmap:textBitmap width:textWidth height:textHeight toX:textStartX y:textStartY];

    [attrStr release];
    CGContextRelease(ctx);
    free(textBitmap);
}

// =============================================================================
// Zoom towards point math
// =============================================================================

- (void)zoomTowardsPoint:(NSPoint)point delta:(double)delta {
    // Clamp the per-event factor: momentum scrolls can deliver deltas that
    // would make the factor zero or negative, which would slam the zoom to
    // the clamp boundary (e.g. from 1e50 straight to 1x).
    if (delta < 0.2) delta = 0.2;
    if (delta > 5.0) delta = 5.0;

    // point.y is already flipped to screen-down by the callers; the
    // imaginary axis grows upward. All center math happens in MPFR so the
    // center stays meaningful beyond double precision (zoom > ~1e13).
    double offX = point.x - _viewState.viewport_width / 2.0;
    double offYUp = _viewState.viewport_height / 2.0 - point.y;

    mb_view_hp_zoom_towards(&_viewState, offX, offYUp, delta);

    // Update HP mode tracking
    [self updateHPMode];

    // Update control panel displays
    [self syncControlPanel];

    // Show zoom slider overlay briefly
    if (_zoomSlider) {
        [_zoomSlider showWithFade];
    }
}

// =============================================================================
// Event Handlers
// =============================================================================

- (void)magnifyWithEvent:(NSEvent *)event {
    if (atomic_load(&_cinematicMode)) return;   // autopilot owns the camera
    // Pinch-to-zoom
    double zoom = 1.0 + event.magnification;
    NSPoint loc = [self convertPoint:event.locationInWindow fromView:nil];
    // Flip Y coordinate (AppKit uses bottom-left origin)
    loc.y = self.bounds.size.height - loc.y;

    [self zoomTowardsPoint:loc delta:zoom];
    [self invalidateCache];
}

- (void)scrollWheel:(NSEvent *)event {
    if (atomic_load(&_cinematicMode)) return;   // autopilot owns the camera
    if (event.modifierFlags & NSEventModifierFlagCommand) {
        // Command+scroll = zoom
        double delta = 1.0 + event.scrollingDeltaY * MB_SCROLL_SENSITIVITY;
        NSPoint loc = [self convertPoint:event.locationInWindow fromView:nil];
        loc.y = self.bounds.size.height - loc.y;
        [self zoomTowardsPoint:loc delta:delta];
    } else if (_viewState.zoom_level > 1.0) {
        // Regular scroll = pan (only when zoomed in). FloatExp scale: a
        // plain double underflows past zoom ~1e300.
        FloatExp scale = mb_view_get_scale_fx(&_viewState);
        mb_view_hp_translate_fx(&_viewState,
                                fx_mul_d(scale, -event.scrollingDeltaX),
                                fx_mul_d(scale, event.scrollingDeltaY));

        // Update control panel
        [self syncControlPanel];
    }
    [self invalidateCache];
}

- (void)mouseDown:(NSEvent *)event {
    // Store initial position for drag
    _lastDragPoint = [self convertPoint:event.locationInWindow fromView:nil];
    _lastDragPoint.y = self.bounds.size.height - _lastDragPoint.y;
}

- (void)mouseDragged:(NSEvent *)event {
    if (atomic_load(&_cinematicMode)) return;   // autopilot owns the camera
    // No panning at 1x zoom - nothing useful outside default bounds
    if (_viewState.zoom_level <= 1.0) return;

    // Pan by dragging
    NSPoint loc = [self convertPoint:event.locationInWindow fromView:nil];
    loc.y = self.bounds.size.height - loc.y;

    FloatExp scale = mb_view_get_scale_fx(&_viewState);
    mb_view_hp_translate_fx(&_viewState,
                            fx_mul_d(scale, -(loc.x - _lastDragPoint.x)),
                            fx_mul_d(scale, loc.y - _lastDragPoint.y));

    // Update control panel
    [self syncControlPanel];

    _lastDragPoint = loc;
    [self invalidateCache];
}

- (void)rightMouseDown:(NSEvent *)event {
    if (atomic_load(&_cinematicMode)) return;   // autopilot owns the camera
    // Right-click to zoom out
    NSPoint loc = [self convertPoint:event.locationInWindow fromView:nil];
    loc.y = self.bounds.size.height - loc.y;
    [self zoomTowardsPoint:loc delta:0.5];
    [self invalidateCache];
}

// =============================================================================
// Key Handler Implementations
// =============================================================================

static void handleResetView(MandelbrotView *self) {
    [self startAnimationToX:-0.5 y:0.0 zoomLog10:0.0];
    self->_highPrecisionMode = NO;
    self->_currentPrecision = 64;
}

static void handleZoomIn(MandelbrotView *self) {
    [self zoomTowardsPoint:NSMakePoint(self->_viewState.viewport_width/2,
                                        self->_viewState.viewport_height/2)
                     delta:MB_ZOOM_IN_FACTOR];
    [self invalidateCache];
}

static void handleZoomOut(MandelbrotView *self) {
    [self zoomTowardsPoint:NSMakePoint(self->_viewState.viewport_width/2,
                                        self->_viewState.viewport_height/2)
                     delta:MB_ZOOM_OUT_FACTOR];
    [self invalidateCache];
}

static void handleToggleHUD(MandelbrotView *self) {
    self->_showHUD = !self->_showHUD;
    self->_needsRedraw = YES;
}

static void handleToggleMinimap(MandelbrotView *self) {
    self->_showMinimap = !self->_showMinimap;
    self->_needsRedraw = YES;
}

static void handleToggleCoordinates(MandelbrotView *self) {
    self->_showCoordinates = !self->_showCoordinates;
    self->_needsRedraw = YES;
}

static void handleTogglePresetMenu(MandelbrotView *self) {
    self->_showPresetMenu = !self->_showPresetMenu;
    self->_needsRedraw = YES;
}

static void handleEscape(MandelbrotView *self) {
    // First close coordinate picker if active
    if (self->_coordinateMarker && self->_coordinateMarker.pickerModeActive) {
        [self->_coordinateMarker setPickerMode:NO];
        return;
    }
    // Also hide marker if placed
    if (self->_coordinateMarker && self->_coordinateMarker.state != MBMarkerStateHidden) {
        [self->_coordinateMarker hideMarker];
        return;
    }
    // Otherwise close window
    [self.window close];
}

static void handlePanUp(MandelbrotView *self) {
    if (self->_viewState.zoom_level <= 1.0) return;
    FloatExp pan = fx_mul_d(mb_view_get_scale_fx(&self->_viewState), 50.0);
    mb_view_hp_translate_fx(&self->_viewState, fx_zero(), pan);
    [self syncControlPanel];
    [self invalidateCache];
}

static void handlePanDown(MandelbrotView *self) {
    if (self->_viewState.zoom_level <= 1.0) return;
    FloatExp pan = fx_mul_d(mb_view_get_scale_fx(&self->_viewState), 50.0);
    mb_view_hp_translate_fx(&self->_viewState, fx_zero(), fx_neg(pan));
    [self syncControlPanel];
    [self invalidateCache];
}

static void handlePanLeft(MandelbrotView *self) {
    if (self->_viewState.zoom_level <= 1.0) return;
    FloatExp pan = fx_mul_d(mb_view_get_scale_fx(&self->_viewState), 50.0);
    mb_view_hp_translate_fx(&self->_viewState, fx_neg(pan), fx_zero());
    [self syncControlPanel];
    [self invalidateCache];
}

static void handlePanRight(MandelbrotView *self) {
    if (self->_viewState.zoom_level <= 1.0) return;
    FloatExp pan = fx_mul_d(mb_view_get_scale_fx(&self->_viewState), 50.0);
    mb_view_hp_translate_fx(&self->_viewState, pan, fx_zero());
    [self syncControlPanel];
    [self invalidateCache];
}

static void handleClearCache(MandelbrotView *self) {
    // Clear in-memory tile cache
    tile_cache_clear(&self->_scheduler.cache);
    // Clear disk cache
    if (self->_scheduler.disk_cache) {
        disk_cache_clear(self->_scheduler.disk_cache);
    }
    self->_needsRedraw = YES;
}

static void handleCyclePalette(MandelbrotView *self) {
    // Cycle to next palette
    self->_renderSettings.palette_id = (self->_renderSettings.palette_id + 1) % MB_PALETTE_COUNT;
    // Invalidate render cache since colors will change
    [self invalidateRenderCache];
    [self saveRenderSettings];
}

static void handleToggleSmoothColoring(MandelbrotView *self) {
    // Toggle between classic and smooth coloring
    if (self->_renderSettings.color_mode == MB_COLOR_MODE_CLASSIC) {
        self->_renderSettings.color_mode = MB_COLOR_MODE_SMOOTH;
    } else {
        self->_renderSettings.color_mode = MB_COLOR_MODE_CLASSIC;
    }
    // Invalidate render cache since colors will change
    [self invalidateRenderCache];
    [self saveRenderSettings];
}

static void handleToggleAntialiasing(MandelbrotView *self) {
    // Toggle antialiasing (2x2 supersampling)
    self->_renderSettings.antialiasing_enabled = !self->_renderSettings.antialiasing_enabled;
    // Invalidate render cache since rendering changes
    [self invalidateRenderCache];
    [self saveRenderSettings];
}

static void handleDecreaseColorCycleScale(MandelbrotView *self) {
    // Decrease cycle scale = more bands (tighter cycling)
    self->_renderSettings.color_cycle_scale *= 0.5f;
    if (self->_renderSettings.color_cycle_scale < 8.0f) {
        self->_renderSettings.color_cycle_scale = 8.0f;
    }
    [self invalidateRenderCache];
    [self saveRenderSettings];
}

static void handleIncreaseColorCycleScale(MandelbrotView *self) {
    // Increase cycle scale = fewer bands (smoother gradient)
    self->_renderSettings.color_cycle_scale *= 2.0f;
    if (self->_renderSettings.color_cycle_scale > 512.0f) {
        self->_renderSettings.color_cycle_scale = 512.0f;
    }
    [self invalidateRenderCache];
    [self saveRenderSettings];
}

static void handleToggleControlPanel(MandelbrotView *self) {
    if (self->_controlPanel) {
        [self->_controlPanel toggleCollapsed];
    }
}

static void handleToggleCoordinatePicker(MandelbrotView *self) {
    if (self->_coordinateMarker) {
        [self->_coordinateMarker togglePickerMode];
        // Update the cached view state for coordinate conversion
        [self->_coordinateMarker updateViewState:&self->_viewState];
    }
}

static void handleToggleCinematic(MandelbrotView *self) {
    [self enterCinematic];   // exit is handled by the cinematic key intercept
}

static void handlePresetKey(MandelbrotView *self, unichar key) {
    for (int i = 0; i < kPresetCount; i++) {
        if (kPresetLocations[i].key == key) {
            [self startAnimationToX:kPresetLocations[i].center_x
                                  y:kPresetLocations[i].center_y
                          zoomLog10:log10(kPresetLocations[i].zoom)];
            break;
        }
    }
}

- (void)keyDown:(NSEvent *)event {
    NSString *chars = [event charactersIgnoringModifiers];
    if (!chars || [chars length] == 0) {
        [super keyDown:event];
        return;
    }
    unichar key = [chars characterAtIndex:0];

    // Cinematic mode: V/Escape exits (back to interactive at the current
    // location), I toggles the HUD, everything else is the movie's problem.
    if (atomic_load(&_cinematicMode)) {
        if (key == 'v' || key == 'V' || key == 27) {
            [self exitCinematic];
        } else if (key == 'i' || key == 'I') {
            // Cinematic has its own HUD pin: the interactive _showHUD
            // defaults ON, and reusing it would defeat hover auto-hide.
            _cineHudPinned = !_cineHudPinned;
        }
        return;
    }

    // Handle preset menu navigation when menu is open
    if (_showPresetMenu) {
        switch (key) {
            case NSUpArrowFunctionKey:
                _selectedPresetIdx--;
                if (_selectedPresetIdx < 0) _selectedPresetIdx = kPresetCount - 1;
                _needsRedraw = YES;
                return;
            case NSDownArrowFunctionKey:
                _selectedPresetIdx++;
                if (_selectedPresetIdx >= kPresetCount) _selectedPresetIdx = 0;
                _needsRedraw = YES;
                return;
            case 13: // Enter/Return
                [self startAnimationToX:kPresetLocations[_selectedPresetIdx].center_x
                                      y:kPresetLocations[_selectedPresetIdx].center_y
                              zoomLog10:log10(kPresetLocations[_selectedPresetIdx].zoom)];
                _showPresetMenu = NO;
                _needsRedraw = YES;
                return;
            case 27: // Escape
            case 'p':
            case 'P':
                _showPresetMenu = NO;
                _needsRedraw = YES;
                return;
        }
    }

    // Handle arrow keys for panning (not in dispatch table due to special preset menu handling)
    switch (key) {
        case NSUpArrowFunctionKey:
            handlePanUp(self);
            return;
        case NSDownArrowFunctionKey:
            handlePanDown(self);
            return;
        case NSLeftArrowFunctionKey:
            handlePanLeft(self);
            return;
        case NSRightArrowFunctionKey:
            handlePanRight(self);
            return;
    }

    // Check dispatch table for key bindings
    for (int i = 0; i < kKeyBindingsCount; i++) {
        if (key == kKeyBindings[i].key || key == kKeyBindings[i].altKey) {
            if (_showPresetMenu && kKeyBindings[i].closesPresetMenu) {
                _showPresetMenu = NO;
            }
            kKeyBindings[i].handler(self);
            return;
        }
    }

    // Handle preset hotkeys (1-9)
    if (key >= '1' && key <= '9') {
        handlePresetKey(self, key);
        return;
    }

    // Pass unhandled keys to super
    [super keyDown:event];
}

// =============================================================================
// Rendering
// =============================================================================

- (void)invalidate {
    // Only invalidate cache when view parameters change significantly
    // For now just mark as needing redraw
    _needsRedraw = YES;
}

- (void)invalidateCache {
    tile_cache_new_generation(&_scheduler.cache);
    uint64_t gen = atomic_fetch_add(&_viewGeneration, 1) + 1;
    _lastViewChangeTime = CFAbsoluteTimeGetCurrent();
    if (_deepRenderer) {
        // Let in-flight deep tiles abort mid-render
        mb_deep_renderer_note_generation(_deepRenderer, gen);
    }
    _needsRedraw = YES;
}

// Fingerprint of every render setting that changes tile pixels. Bump the
// version constant when the coloring or iteration formula changes so old
// disk tiles are not served for new renders.
- (uint32_t)renderSettingsFingerprint {
    uint32_t fp = 2166136261u;  // FNV-1a
    uint32_t values[4] = {
        (uint32_t)_renderSettings.color_mode,
        (uint32_t)_renderSettings.palette_id,
        (uint32_t)lroundf(_renderSettings.color_cycle_scale * 100.0f),
        2u,  // formula version (bumped: zoom-scaled iterations)
    };
    for (int i = 0; i < 4; i++) {
        fp = (fp ^ values[i]) * 16777619u;
    }
    return fp;
}

- (void)updateDiskCacheVariant {
    if (_scheduler.disk_cache) {
        disk_cache_set_variant(_scheduler.disk_cache, [self renderSettingsFingerprint]);
    }
}

- (void)invalidateRenderCache {
    // Clear async tile cache (render settings changed)
    [_asyncCacheLock lock];
    [_asyncTileCache removeAllObjects];
    [_pendingTiles removeAllObjects];
    [_asyncCacheLock unlock];

    // Also clear scheduler cache
    tile_cache_new_generation(&_scheduler.cache);

    // Disk tiles are keyed by render settings; repoint the cache
    [self updateDiskCacheVariant];

    // Deep tiles are colored too — force recompute
    uint64_t gen = atomic_fetch_add(&_viewGeneration, 1) + 1;
    _lastDeepFrameValid = NO;
    if (_deepRenderer) {
        mb_deep_renderer_note_generation(_deepRenderer, gen);
    }

    _needsRedraw = YES;
}

- (void)displayTimerFired:(NSTimer *)timer {
    (void)timer;

    if (atomic_load(&_cinematicMode)) {
        [self cinematicTick];
        return;
    }

    // Update animation state
    if (_animating) {
        [self updateAnimation];
    }

    if (_needsRedraw || _deepNeedsPoll) {
        [self renderFrame];
        _needsRedraw = NO;
    }
}

- (void)blitTile:(PixelColor *)src fromSize:(int)srcSize toRect:(NSRect)dstRect {
    // Bilinear scale from src (srcSize x srcSize) to dstRect in _framebuffer
    int dx0 = MAX(0, (int)dstRect.origin.x);
    int dy0 = MAX(0, (int)dstRect.origin.y);
    int dx1 = MIN(_viewState.viewport_width, (int)(dstRect.origin.x + dstRect.size.width));
    int dy1 = MIN(_viewState.viewport_height, (int)(dstRect.origin.y + dstRect.size.height));

    if (dx1 <= dx0 || dy1 <= dy0 || dstRect.size.width <= 0 || dstRect.size.height <= 0) {
        return;
    }

    double scaleX = (double)srcSize / dstRect.size.width;
    double scaleY = (double)srcSize / dstRect.size.height;

    // Use bilinear interpolation when upscaling (scale < 1.0)
    BOOL useLinear = (scaleX < 1.0 || scaleY < 1.0);

    for (int dy = dy0; dy < dy1; dy++) {
        double sy = (dstRect.size.height - 1 - (dy - dstRect.origin.y)) * scaleY;

        for (int dx = dx0; dx < dx1; dx++) {
            double sx = (dx - dstRect.origin.x) * scaleX;

            if (useLinear) {
                // Bilinear interpolation
                int sxi0 = (int)floor(sx);
                int syi0 = (int)floor(sy);
                int sxi1 = sxi0 + 1;
                int syi1 = syi0 + 1;

                // Clamp to valid range
                if (sxi0 < 0) sxi0 = 0;
                if (syi0 < 0) syi0 = 0;
                if (sxi1 >= srcSize) sxi1 = srcSize - 1;
                if (syi1 >= srcSize) syi1 = srcSize - 1;
                if (sxi0 >= srcSize) sxi0 = srcSize - 1;
                if (syi0 >= srcSize) syi0 = srcSize - 1;

                // Fractional position
                double fx = sx - floor(sx);
                double fy = sy - floor(sy);

                // Sample 4 neighbors
                PixelColor p00 = src[syi0 * srcSize + sxi0];
                PixelColor p10 = src[syi0 * srcSize + sxi1];
                PixelColor p01 = src[syi1 * srcSize + sxi0];
                PixelColor p11 = src[syi1 * srcSize + sxi1];

                // Bilinear blend: (1-fx)(1-fy)*p00 + fx*(1-fy)*p10 + (1-fx)*fy*p01 + fx*fy*p11
                double w00 = (1.0 - fx) * (1.0 - fy);
                double w10 = fx * (1.0 - fy);
                double w01 = (1.0 - fx) * fy;
                double w11 = fx * fy;

                PixelColor result;
                result.r = (unsigned char)(w00 * p00.r + w10 * p10.r + w01 * p01.r + w11 * p11.r);
                result.g = (unsigned char)(w00 * p00.g + w10 * p10.g + w01 * p01.g + w11 * p11.g);
                result.b = (unsigned char)(w00 * p00.b + w10 * p10.b + w01 * p01.b + w11 * p11.b);

                _framebuffer[dy * _viewState.viewport_width + dx] = result;
            } else {
                // Point sampling for downscaling
                int sxi = (int)sx;
                int syi = (int)sy;
                if (sxi >= srcSize) sxi = srcSize - 1;
                if (sxi < 0) sxi = 0;
                if (syi >= srcSize) syi = srcSize - 1;
                if (syi < 0) syi = 0;

                _framebuffer[dy * _viewState.viewport_width + dx] =
                    src[syi * srcSize + sxi];
            }
        }
    }
}

- (NSString *)tileKeyForTile:(const MapTile *)tile {
    // Include the render-settings fingerprint: tiles are stored post-colored,
    // and a worker that started before a palette change must not have its
    // stale-colored result served (or promoted to disk) afterwards.
    return [NSString stringWithFormat:@"%08x_%d_%llu_%llu",
            [self renderSettingsFingerprint], tile->zoom, tile->x, tile->y];
}

// Evict cache entries while holding _asyncCacheLock, sparing the current
// deep generation: wiping everything would make a deep frame that needs many
// tiles impossible to complete (each pass would evict the previous tiles).
- (void)purgeAsyncCacheLocked {
    if (_asyncTileCache.count < 256) return;

    NSString *keepPrefix = [NSString stringWithFormat:@"deep_%llu_",
                            (unsigned long long)atomic_load(&_viewGeneration)];
    NSMutableArray *toRemove = [NSMutableArray array];
    for (NSString *k in _asyncTileCache) {
        if (![k hasPrefix:keepPrefix]) {
            [toRemove addObject:k];
        }
    }
    [_asyncTileCache removeObjectsForKeys:toRemove];
    // If the current generation alone exceeds the cap, keep it anyway —
    // a temporarily oversized cache beats a frame that can never finish.
}

- (BOOL)getTileFromAsyncCache:(const MapTile *)tile output:(PixelColor *)output {
    NSString *key = [self tileKeyForTile:tile];
    BOOL found = NO;

    // Copy under the lock: a worker thread hitting the cache-size purge can
    // release the NSData the moment the lock is dropped.
    [_asyncCacheLock lock];
    NSData *data = _asyncTileCache[key];
    if (data && data.length == MB_MAP_TILE_SIZE * MB_MAP_TILE_SIZE * sizeof(PixelColor)) {
        memcpy(output, data.bytes, data.length);
        found = YES;
    }
    [_asyncCacheLock unlock];

    return found;
}

- (void)queueAsyncTileComputation:(MapTile)tile {
    NSString *key = [self tileKeyForTile:&tile];

    [_asyncCacheLock lock];
    // Check if already pending or cached
    if ([_pendingTiles containsObject:key] || _asyncTileCache[key] != nil) {
        [_asyncCacheLock unlock];
        return;
    }
    [_pendingTiles addObject:key];
    [_asyncCacheLock unlock];

    // Capture render settings by value for thread safety
    MBRenderSettings capturedSettings = _renderSettings;

    // Queue async computation
    dispatch_async(_tileQueue, ^{
        // Skip tiles queued for a pyramid level far from what is displayed
        // now (the user zoomed past this level before the job started).
        // Coarser levels within reach stay useful as ancestor placeholders.
        int curZoom = atomic_load(&self->_currentTargetZoom);
        if (tile.zoom > curZoom + 2 || tile.zoom < curZoom - 6) {
            [_asyncCacheLock lock];
            [_pendingTiles removeObject:key];
            [_asyncCacheLock unlock];
            return;
        }

        // Allocate buffer for this tile
        PixelColor *tileBuf = malloc(MB_MAP_TILE_SIZE * MB_MAP_TILE_SIZE * sizeof(PixelColor));
        if (!tileBuf) {
            [_asyncCacheLock lock];
            [_pendingTiles removeObject:key];
            [_asyncCacheLock unlock];
            return;
        }

        // Compute tile (this is the expensive operation)
        // Note: We compute directly here instead of going through the
        // scheduler because the scheduler is not thread-safe
        double min_cx, max_cx, min_cy, max_cy;
        mb_tile_to_bounds(&tile, &min_cx, &max_cx, &min_cy, &max_cy);

        double scale_x = (max_cx - min_cx) / MB_MAP_TILE_SIZE;
        double scale_y = (max_cy - min_cy) / MB_MAP_TILE_SIZE;

        // Iteration budget scales with tile depth so deep tiles keep detail
        unsigned int tileMaxIter = mb_max_iter_for_tile_zoom(tile.zoom);

        for (int ly = 0; ly < MB_MAP_TILE_SIZE; ly++) {
            double cy = min_cy + (ly + 0.5) * scale_y;
            // Two pixels per step (NEON lanes on arm64); tile size is even
            for (int lx = 0; lx < MB_MAP_TILE_SIZE; lx += 2) {
                double cx0 = min_cx + (lx + 0.5) * scale_x;
                double cx1 = min_cx + (lx + 1.5) * scale_x;
                unsigned int it0, it1;
                float z0, z1;
                mb_compute_pair_smooth(cx0, cy, cx1, cy, tileMaxIter,
                                       &it0, &it1, &z0, &z1);
                color_from_iteration_ex(&tileBuf[ly * MB_MAP_TILE_SIZE + lx],
                                        it0, z0, tileMaxIter, &capturedSettings);
                color_from_iteration_ex(&tileBuf[ly * MB_MAP_TILE_SIZE + lx + 1],
                                        it1, z1, tileMaxIter, &capturedSettings);
            }
        }

        // Store in async cache (with memory limit). alloc/init + release:
        // GCD worker threads drain autorelease pools at unspecified times,
        // so autoreleased 256KB buffers could pile up during a long zoom.
        NSData *tileData = [[NSData alloc] initWithBytes:tileBuf
                                                  length:MB_MAP_TILE_SIZE * MB_MAP_TILE_SIZE * sizeof(PixelColor)];

        [_asyncCacheLock lock];
        // Limit cache size (~64MB) to prevent unbounded memory growth
        [self purgeAsyncCacheLocked];
        _asyncTileCache[key] = tileData;
        [_pendingTiles removeObject:key];
        [_asyncCacheLock unlock];
        [tileData release];

        free(tileBuf);

        // Trigger redraw on main thread
        dispatch_async(dispatch_get_main_queue(), ^{
            _needsRedraw = YES;
        });
    });
}

// Walk up the tile pyramid looking for a cached ancestor. Fills `output`
// and returns how many levels up it was found, or -1 if none. Capped at 8
// levels: beyond that an ancestor contributes less than one source pixel
// per tile (and blitTileQuadrant's source region would collapse to zero).
- (int)findAncestorTileData:(const MapTile *)tile into:(PixelColor *)output {
    MapTile cur = *tile;
    int offset = 0;

    while (cur.zoom > 0 && offset < 8) {
        cur.zoom -= 1;
        cur.x /= 2;
        cur.y /= 2;
        offset += 1;

        if ([self getTileFromAsyncCache:&cur output:output]) return offset;
        if (cur.zoom <= MB_DISK_CACHE_MAX_ZOOM && _scheduler.disk_cache &&
            disk_cache_get(_scheduler.disk_cache, &cur, output) == 0) return offset;
    }
    return -1;
}

- (void)blitTileQuadrant:(PixelColor *)src
               quadrantX:(int)qx quadrantY:(int)qy
               zoomOffset:(int)zoomOffset
                  toRect:(NSRect)destRect {
    // Blit a portion of the source tile (quadrant determined by zoomOffset) to destRect
    // zoomOffset = 1 means blit 1/2 of tile (128x128 region)
    // zoomOffset = 2 means blit 1/4 of tile (64x64 region)
    // etc.

    int dx0 = MAX(0, (int)destRect.origin.x);
    int dy0 = MAX(0, (int)destRect.origin.y);
    int dx1 = MIN(_viewState.viewport_width, (int)(destRect.origin.x + destRect.size.width));
    int dy1 = MIN(_viewState.viewport_height, (int)(destRect.origin.y + destRect.size.height));

    if (dx1 <= dx0 || dy1 <= dy0 || destRect.size.width <= 0 || destRect.size.height <= 0) {
        return;
    }

    // Calculate source region within the parent tile
    int divisor = 1 << zoomOffset;  // 2 for zoomOffset=1, 4 for zoomOffset=2, etc.
    int srcRegionSize = MB_MAP_TILE_SIZE / divisor;
    int srcX0 = qx * srcRegionSize;
    int srcY0 = qy * srcRegionSize;

    double scaleX = (double)srcRegionSize / destRect.size.width;
    double scaleY = (double)srcRegionSize / destRect.size.height;

    for (int dy = dy0; dy < dy1; dy++) {
        // Y is inverted in screen space
        double sy = srcY0 + (destRect.size.height - 1 - (dy - destRect.origin.y)) * scaleY;
        int syi = (int)sy;
        if (syi >= srcY0 + srcRegionSize) syi = srcY0 + srcRegionSize - 1;
        if (syi < srcY0) syi = srcY0;

        for (int dx = dx0; dx < dx1; dx++) {
            double sx = srcX0 + (dx - destRect.origin.x) * scaleX;
            int sxi = (int)sx;
            if (sxi >= srcX0 + srcRegionSize) sxi = srcX0 + srcRegionSize - 1;
            if (sxi < srcX0) sxi = srcX0;

            _framebuffer[dy * _viewState.viewport_width + dx] =
                src[syi * MB_MAP_TILE_SIZE + sxi];
        }
    }
}

- (void)renderMapTile:(const MapTile *)tile withViewScale:(double)viewScale {
    BOOL tileReady = NO;

    // 1. Check disk cache first (fast path)
    if (tile->zoom <= MB_DISK_CACHE_MAX_ZOOM && _scheduler.disk_cache) {
        if (disk_cache_get(_scheduler.disk_cache, tile, _mapTileBuf) == 0) {
            tileReady = YES;
        }
    }

    // 2. If not in disk cache, check async memory cache
    if (!tileReady) {
        if ([self getTileFromAsyncCache:tile output:_mapTileBuf]) {
            tileReady = YES;
            // Save to disk cache for future use
            if (tile->zoom <= MB_DISK_CACHE_MAX_ZOOM && _scheduler.disk_cache) {
                disk_cache_put(_scheduler.disk_cache, tile, _mapTileBuf);
            }
        }
    }

    // Get tile bounds in complex plane (needed for screen coords calculation)
    double min_cx, max_cx, min_cy, max_cy;
    mb_tile_to_bounds(tile, &min_cx, &max_cx, &min_cy, &max_cy);

    // Convert to screen coordinates using VIEW scale (not tile scale!)
    int vp_half_w = _viewState.viewport_width / 2;
    int vp_half_h = _viewState.viewport_height / 2;

    double sx0 = (min_cx - _viewState.center_x) / viewScale + vp_half_w;
    double sy0 = (_viewState.center_y - max_cy) / viewScale + vp_half_h;  // Y inverted: max_cy → top of screen
    double sx1 = (max_cx - _viewState.center_x) / viewScale + vp_half_w;
    double sy1 = (_viewState.center_y - min_cy) / viewScale + vp_half_h;  // Y inverted: min_cy → bottom of screen

    // Casting a double outside int range is undefined behavior; a heavily
    // stretched tile can exceed it. Skip tiles that far off/large — they
    // could not be blitted meaningfully anyway.
    const double kMaxScreenCoord = 1e7;
    if (fabs(sx0) > kMaxScreenCoord || fabs(sy0) > kMaxScreenCoord ||
        fabs(sx1) > kMaxScreenCoord || fabs(sy1) > kMaxScreenCoord) {
        return;
    }

    int screen_x0 = (int)sx0;
    int screen_y0 = (int)sy0;
    int screen_x1 = (int)sx1;
    int screen_y1 = (int)sy1;

    NSRect screenRect = NSMakeRect(screen_x0, screen_y0, screen_x1 - screen_x0, screen_y1 - screen_y0);

    // 3. If still not ready, try ancestor tile fallback, then queue async computation
    if (!tileReady) {
        MapTile tileCopy = *tile;
        [self queueAsyncTileComputation:tileCopy];

        // Render the nearest cached ancestor as a placeholder
        int zoomOffset = [self findAncestorTileData:tile into:_parentTileBuf];
        if (zoomOffset > 0) {
            // Which sub-region of the ancestor corresponds to this tile
            uint64_t mask = ((uint64_t)1 << zoomOffset) - 1;
            int qx = (int)(tile->x & mask);
            int qy = (int)(tile->y & mask);

            [self blitTileQuadrant:_parentTileBuf quadrantX:qx quadrantY:qy zoomOffset:zoomOffset toRect:screenRect];
        }
        return;
    }

    [self blitTile:_mapTileBuf fromSize:MB_MAP_TILE_SIZE toRect:screenRect];
}

- (void)renderPlaceholderTilesAtZoom:(int)zoom withViewScale:(double)viewScale {
    double halfW = (_viewState.viewport_width / 2.0) * viewScale;
    double halfH = (_viewState.viewport_height / 2.0) * viewScale;

    MapTile topLeft = mb_complex_to_tile(_viewState.center_x - halfW,
                                          _viewState.center_y - halfH, zoom);
    MapTile bottomRight = mb_complex_to_tile(_viewState.center_x + halfW,
                                              _viewState.center_y + halfH, zoom);

    for (uint64_t ty = topLeft.y; ty <= bottomRight.y; ty++) {
        for (uint64_t tx = topLeft.x; tx <= bottomRight.x; tx++) {
            MapTile tile = { .zoom = zoom, .x = tx, .y = ty };
            if (_scheduler.disk_cache && disk_cache_exists(_scheduler.disk_cache, &tile)) {
                [self renderMapTile:&tile withViewScale:viewScale];
            }
        }
    }
}

- (void)renderMapFrame {
    // 1. Calculate target zoom from view scale
    //    We want tiles at ~1:1 pixel scale (256 tile pixels ≈ 256 screen pixels)
    double viewScale = mb_view_get_scale(&_viewState);
    double tileWidthNeeded = viewScale * MB_MAP_TILE_SIZE;  // complex width of 256 screen pixels
    int targetZoom = (int)round(log2(MB_REAL_WIDTH / tileWidthNeeded));
    if (targetZoom < 0) targetZoom = 0;
    if (targetZoom > MB_MAX_ZOOM) targetZoom = MB_MAX_ZOOM;
    atomic_store(&_currentTargetZoom, targetZoom);

    // 2. Calculate visible region in complex plane using VIEW scale
    double halfW = (_viewState.viewport_width / 2.0) * viewScale;
    double halfH = (_viewState.viewport_height / 2.0) * viewScale;
    double viewMinCx = _viewState.center_x - halfW;
    double viewMaxCx = _viewState.center_x + halfW;
    double viewMinCy = _viewState.center_y - halfH;
    double viewMaxCy = _viewState.center_y + halfH;

    // 3. Find which tiles intersect this region
    MapTile topLeft = mb_complex_to_tile(viewMinCx, viewMinCy, targetZoom);
    MapTile bottomRight = mb_complex_to_tile(viewMaxCx, viewMaxCy, targetZoom);

    // Expand by 1 tile for smooth scrolling
    int64_t startX = (int64_t)topLeft.x - 1;
    int64_t startY = (int64_t)topLeft.y - 1;
    int64_t endX = (int64_t)bottomRight.x + 2;
    int64_t endY = (int64_t)bottomRight.y + 2;
    if (startX < 0) startX = 0;
    if (startY < 0) startY = 0;
    int64_t maxTile = (int64_t)1 << targetZoom;
    if (endX > maxTile) endX = maxTile;
    if (endY > maxTile) endY = maxTile;

    // 4. Clear framebuffer
    size_t pixels = (size_t)_viewState.viewport_width * _viewState.viewport_height;
    memset(_framebuffer, 0, pixels * sizeof(PixelColor));

    // 5. First pass: placeholder tiles (lower zoom, cached only)
    int placeholderZoom = targetZoom > 2 ? targetZoom - 2 : 0;
    [self renderPlaceholderTilesAtZoom:placeholderZoom withViewScale:viewScale];

    // 6. Second pass: render actual tiles
    for (int64_t ty = startY; ty < endY; ty++) {
        for (int64_t tx = startX; tx < endX; tx++) {
            MapTile tile = { .zoom = targetZoom, .x = (uint64_t)tx, .y = (uint64_t)ty };
            [self renderMapTile:&tile withViewScale:viewScale];
        }
    }
}

// =============================================================================
// Deep-Zoom Rendering (screen-space perturbation tiles)
// =============================================================================

// Coarse preview tiles use stride-4 sampling: 1/16th of the compute, shown
// upscaled while the full-resolution pass runs.
#define MB_DEEP_COARSE_STRIDE 4

- (NSString *)deepTileKeyForGen:(uint64_t)gen tx:(int)tx ty:(int)ty coarse:(BOOL)coarse {
    // Both variants share the "deep_<gen>_" prefix so the cache purge can
    // spare the whole current generation with one prefix check.
    return [NSString stringWithFormat:@"deep_%llu_%s%d_%d",
            (unsigned long long)gen, coarse ? "c_" : "", tx, ty];
}

- (void)queueDeepTileGen:(uint64_t)gen tx:(int)tx ty:(int)ty coarse:(BOOL)coarse {
    NSString *key = [self deepTileKeyForGen:gen tx:tx ty:ty coarse:coarse];

    [_asyncCacheLock lock];
    if ([_pendingTiles containsObject:key] || _asyncTileCache[key] != nil) {
        [_asyncCacheLock unlock];
        return;
    }
    [_pendingTiles addObject:key];
    [_asyncCacheLock unlock];

    // Snapshot everything the worker needs (plain structs, copied by value)
    MBViewState viewCopy = _viewState;
    MBRenderSettings settingsCopy = _renderSettings;
    // Quantize the iteration budget so small zoom changes reuse the shared
    // reference orbit (rebuilding it for every wheel tick would stall all
    // tile workers behind the renderer mutex).
    uint32_t maxIter = mb_max_iter_for_zoom_log10(mb_view_zoom_log10(&viewCopy));
    maxIter = (maxIter + 2047u) & ~2047u;
    int tileSize = MB_MAP_TILE_SIZE;
    int stride = coarse ? MB_DEEP_COARSE_STRIDE : 1;

    dispatch_async(_tileQueue, ^{
        // The queue may hold jobs for view states the user has zoomed past;
        // drop them before doing any work.
        if (atomic_load(&self->_viewGeneration) != gen) {
            [_asyncCacheLock lock];
            [_pendingTiles removeObject:key];
            [_asyncCacheLock unlock];
            return;
        }

        int outDim = tileSize / stride;
        size_t tileBytes = (size_t)outDim * outDim * sizeof(PixelColor);
        PixelColor *buf = malloc(tileBytes);
        int rc = MB_DEEP_ERROR;
        if (buf) {
            rc = mb_deep_renderer_render_tile_strided(_deepRenderer, &viewCopy, gen,
                                                      tx * tileSize, ty * tileSize,
                                                      tileSize, stride, maxIter,
                                                      &settingsCopy, buf);
        }

        [_asyncCacheLock lock];
        if (rc == MB_DEEP_OK) {
            [self purgeAsyncCacheLocked];
            // alloc/init + release below: avoid autoreleased buffers piling
            // up on GCD worker threads with unspecified pool drains
            NSData *tileData = [[NSData alloc] initWithBytes:buf length:tileBytes];
            _asyncTileCache[key] = tileData;
            [tileData release];
        }
        [_pendingTiles removeObject:key];
        [_asyncCacheLock unlock];

        free(buf);

        if (rc == MB_DEEP_OK) {
            dispatch_async(dispatch_get_main_queue(), ^{
                _needsRedraw = YES;
            });
        }
    });
}

// Nearest-neighbor upscale of a coarse (stride-sampled) deep tile. No Y
// flip: deep tiles are screen-space.
- (void)blitCoarseDeepTileData:(NSData *)data tx:(int)tx ty:(int)ty {
    const PixelColor *src = (const PixelColor *)data.bytes;
    int ts = MB_MAP_TILE_SIZE;
    int stride = MB_DEEP_COARSE_STRIDE;
    int dim = ts / stride;
    int w = _viewState.viewport_width;
    int h = _viewState.viewport_height;
    int x0 = tx * ts;
    int y0 = ty * ts;
    int copyW = MIN(ts, w - x0);
    int copyH = MIN(ts, h - y0);
    if (copyW <= 0 || copyH <= 0) return;

    for (int y = 0; y < copyH; y++) {
        const PixelColor *srcRow = &src[(size_t)(y / stride) * dim];
        PixelColor *dstRow = &_framebuffer[(size_t)(y0 + y) * w + x0];
        for (int x = 0; x < copyW; x++) {
            dstRow[x] = srcRow[x / stride];
        }
    }
}

- (void)blitDeepTileData:(NSData *)data tx:(int)tx ty:(int)ty {
    const PixelColor *src = (const PixelColor *)data.bytes;
    int ts = MB_MAP_TILE_SIZE;
    int w = _viewState.viewport_width;
    int h = _viewState.viewport_height;
    int x0 = tx * ts;
    int y0 = ty * ts;
    int copyW = MIN(ts, w - x0);
    int copyH = MIN(ts, h - y0);
    if (copyW <= 0 || copyH <= 0) return;

    for (int y = 0; y < copyH; y++) {
        memcpy(&_framebuffer[(size_t)(y0 + y) * w + x0],
               &src[(size_t)y * ts],
               (size_t)copyW * sizeof(PixelColor));
    }
}

// Reproject the last completed deep frame under the current view as a
// placeholder while this generation's tiles are still computing.
- (void)drawDeepPlaceholder {
    int w = _viewState.viewport_width;
    int h = _viewState.viewport_height;
    size_t pixels = (size_t)w * h;

    if (!_lastDeepFrameValid || !_lastDeepFrame) {
        memset(_framebuffer, 0, pixels * sizeof(PixelColor));
        return;
    }

    // All reprojection math in FloatExp: doubles saturate past zoom ~1e300.
    FloatExp dReFx, dImFx;
    mb_view_hp_center_delta_fx(&_lastDeepFrameView, &_viewState, &dReFx, &dImFx);

    FloatExp curScale = mb_view_get_scale_fx(&_viewState);
    FloatExp oldScale = mb_view_get_scale_fx(&_lastDeepFrameView);
    double ratio = fx_to_d(fx_div(curScale, oldScale));

    // If the zoom moved so far that the old frame maps to nothing useful,
    // fall back to black rather than degenerate math.
    if (!(ratio > 1e-8) || !(ratio < 1e8)) {
        memset(_framebuffer, 0, pixels * sizeof(PixelColor));
        return;
    }

    // Center offset in old-frame pixels; bounded by the ref-drift limit so
    // the double conversion is safe.
    double offXpx = fx_to_d(fx_div(dReFx, oldScale));
    double offYpx = fx_to_d(fx_div(dImFx, oldScale));

    int lw = _lastDeepFrameW;
    int lh = _lastDeepFrameH;
    double baseX = lw / 2.0 + offXpx - (w / 2.0) * ratio;
    double baseY = lh / 2.0 - offYpx - (h / 2.0) * ratio;

    for (int py = 0; py < h; py++) {
        double sy = baseY + py * ratio;
        // Validate in double space BEFORE casting: out-of-range
        // double-to-int conversion is undefined behavior.
        BOOL rowValid = (sy >= 0.0 && sy < (double)lh);
        int syi = rowValid ? (int)sy : 0;
        for (int px = 0; px < w; px++) {
            double sx = baseX + px * ratio;
            PixelColor c = {0, 0, 0};
            if (rowValid && sx >= 0.0 && sx < (double)lw) {
                int sxi = (int)sx;
                c = _lastDeepFrame[(size_t)syi * lw + sxi];
            }
            _framebuffer[(size_t)py * w + px] = c;
        }
    }
}

- (void)snapshotDeepFrame {
    int w = _viewState.viewport_width;
    int h = _viewState.viewport_height;
    size_t pixels = (size_t)w * h;

    PixelColor *copy = realloc(_lastDeepFrame, pixels * sizeof(PixelColor));
    if (!copy) return;
    _lastDeepFrame = copy;
    memcpy(_lastDeepFrame, _framebuffer, pixels * sizeof(PixelColor));
    _lastDeepFrameW = w;
    _lastDeepFrameH = h;
    _lastDeepFrameView = _viewState;
    _lastDeepFrameValid = YES;
}

- (void)renderDeepFrame {
    uint64_t gen = atomic_load(&_viewGeneration);
    int ts = MB_MAP_TILE_SIZE;
    int w = _viewState.viewport_width;
    int h = _viewState.viewport_height;

    [self drawDeepPlaceholder];

    // While the user is actively zooming/panning, don't start expensive
    // perturbation tiles for views that will be gone in the next event —
    // show only the placeholder until the view settles. (Skip the wait when
    // there is nothing to show at all.)
    BOOL settled = (CFAbsoluteTimeGetCurrent() - _lastViewChangeTime) >= MB_DEEP_SETTLE_SECONDS
                   || !_lastDeepFrameValid;

    int tilesX = (w + ts - 1) / ts;
    int tilesY = (h + ts - 1) / ts;
    size_t tileBytes = (size_t)ts * ts * sizeof(PixelColor);
    int coarseDim = ts / MB_DEEP_COARSE_STRIDE;
    size_t coarseBytes = (size_t)coarseDim * coarseDim * sizeof(PixelColor);
    BOOL allDone = YES;

    BOOL needCoarse[64] = {NO};  // tilesX*tilesY bounded well below this
    int tileCount = tilesX * tilesY;
    if (tileCount > 64) tileCount = 64;

    for (int ty = 0; ty < tilesY; ty++) {
        for (int tx = 0; tx < tilesX; tx++) {
            NSString *key = [self deepTileKeyForGen:gen tx:tx ty:ty coarse:NO];

            [_asyncCacheLock lock];
            NSData *data = [[_asyncTileCache[key] retain] autorelease];
            NSData *coarse = nil;
            if (!(data && data.length == tileBytes)) {
                NSString *ckey = [self deepTileKeyForGen:gen tx:tx ty:ty coarse:YES];
                coarse = [[_asyncTileCache[ckey] retain] autorelease];
            }
            [_asyncCacheLock unlock];

            if (data && data.length == tileBytes) {
                [self blitDeepTileData:data tx:tx ty:ty];
            } else {
                allDone = NO;
                if (coarse && coarse.length == coarseBytes) {
                    // Preview while the full-resolution pass computes
                    [self blitCoarseDeepTileData:coarse tx:tx ty:ty];
                    if (settled) {
                        [self queueDeepTileGen:gen tx:tx ty:ty coarse:NO];
                    }
                } else if (settled) {
                    int idx = ty * tilesX + tx;
                    if (idx < 64) needCoarse[idx] = YES;
                }
            }
        }
    }

    // Queue ALL coarse passes before any full pass: the dispatch queue is
    // FIFO, so interleaving would let expensive full tiles hog the workers
    // while other tiles still have no preview at all.
    if (settled) {
        for (int idx = 0; idx < tileCount; idx++) {
            if (needCoarse[idx]) {
                [self queueDeepTileGen:gen tx:idx % tilesX ty:idx / tilesX coarse:YES];
            }
        }
        for (int idx = 0; idx < tileCount; idx++) {
            if (needCoarse[idx]) {
                [self queueDeepTileGen:gen tx:idx % tilesX ty:idx / tilesX coarse:NO];
            }
        }
    }

    // If queuing was deferred, keep polling until the view settles
    _deepNeedsPoll = (!allDone && !settled);

    // Keep a copy of complete frames (before HUD overlays) for reprojection
    if (allDone) {
        [self snapshotDeepFrame];
    }
}

// =============================================================================
// Cinematic Mode (autopilot zoom — keyframe compositing)
// =============================================================================

- (void)setCinematicSpeed:(double)decadesPerSecond {
    if (decadesPerSecond < 0.05) decadesPerSecond = 0.05;
    if (decadesPerSecond > 4.0) decadesPerSecond = 4.0;
    _cineSpeed = decadesPerSecond;
}

- (void)enterCinematic {
    if (atomic_load(&_cinematicMode)) return;

    if (!_cineDirector) {
        _cineDirector = mb_director_create(_viewState.viewport_width,
                                           _viewState.viewport_height,
                                           &_renderSettings);
        if (!_cineDirector) return;
    }

    // From a (near-)default view, jitter the seed so every dive explores
    // a fresh path; a deliberately framed deep view is used exactly as-is.
    MBViewState seed = _viewState;
    if (mb_view_zoom_log10(&seed) < 1.5) {
        cine_jitter_seed(&seed, 12.0);
    }
    mb_director_start(_cineDirector, &seed);
    _cineZ = mb_view_zoom_log10(&_viewState);
    _cineCurSpeed = 0.0;
    _cineLastTick = CFAbsoluteTimeGetCurrent();
    atomic_store(&_cinematicMode, true);
    [self pumpCinematic];
}

- (void)exitCinematic {
    if (!atomic_load(&_cinematicMode)) return;
    atomic_store(&_cinematicMode, false);

    // Land the interactive view exactly where the movie stopped
    const MBCineKeyframe *lo = NULL, *hi = NULL;
    mb_director_lock_frames(_cineDirector, _cineZ, &lo, &hi);
    const MBCineKeyframe *ref = lo ? lo : hi;
    char cx[MB_HP_COORD_STR_LEN], cy[MB_HP_COORD_STR_LEN];
    bool have = ref != NULL;
    if (have) {
        memcpy(cx, ref->view.center_x_str, MB_HP_COORD_STR_LEN);
        memcpy(cy, ref->view.center_y_str, MB_HP_COORD_STR_LEN);
    }
    mb_director_unlock_frames(_cineDirector);

    if (have) {
        mb_view_hp_set_center(&_viewState, cx, cy);
    }
    mb_view_set_zoom_log10(&_viewState, _cineZ);
    [self updateHPMode];
    [self invalidateCache];
    [self syncControlPanel];
}

// Keep exactly one background loop rendering keyframes ahead of playback
- (void)pumpCinematic {
    if (!atomic_load(&_cinematicMode) || !_cineDirector) return;
    int expected = 0;
    if (!atomic_compare_exchange_strong(&_cinePumping, &expected, 1)) return;

    MBDirector *director = _cineDirector;
    dispatch_async(_cineQueue, ^{
        while (atomic_load(&self->_cinematicMode)) {
            int rendered = mb_director_render_next(director);
            if (rendered < 0) break;  // pipeline full (or error): tick re-pumps
        }
        atomic_store(&self->_cinePumping, 0);
    });
}

// Bilinear draw of a keyframe under the playback transform:
// src_x = w/2 + offX + (dst_x - w/2) * ratio (same height mapping).
// Bilinear matters for cinematic motion: nearest sampling quantizes the
// continuous zoom into per-pixel jumps that read as crawling/shimmer.
// When `clear` is set, out-of-range destination pixels are blacked (base
// layer); otherwise they are left untouched (sharp inset layer).
- (void)blitCineFrame:(const PixelColor *)src ratio:(double)ratio
                 offX:(double)offX offY:(double)offY clear:(BOOL)clear {
    int w = _viewState.viewport_width;
    int h = _viewState.viewport_height;
    double baseX = w / 2.0 + offX - (w / 2.0) * ratio - 0.5;
    double baseY = h / 2.0 + offY - (h / 2.0) * ratio - 0.5;

    for (int dy = 0; dy < h; dy++) {
        double sy = baseY + dy * ratio;
        BOOL rowValid = (sy >= 0.0 && sy <= (double)(h - 1));
        int sy0 = 0, sy1 = 0;
        double fy = 0.0;
        if (rowValid) {
            sy0 = (int)sy;
            sy1 = sy0 + 1 < h ? sy0 + 1 : sy0;
            fy = sy - sy0;
        }
        PixelColor *dst = &_framebuffer[(size_t)dy * w];
        const PixelColor *row0 = &src[(size_t)sy0 * w];
        const PixelColor *row1 = &src[(size_t)sy1 * w];

        for (int dx = 0; dx < w; dx++) {
            double sx = baseX + dx * ratio;
            if (rowValid && sx >= 0.0 && sx <= (double)(w - 1)) {
                int sx0 = (int)sx;
                int sx1 = sx0 + 1 < w ? sx0 + 1 : sx0;
                double fx = sx - sx0;

                double w00 = (1.0 - fx) * (1.0 - fy);
                double w10 = fx * (1.0 - fy);
                double w01 = (1.0 - fx) * fy;
                double w11 = fx * fy;

                PixelColor p00 = row0[sx0], p10 = row0[sx1];
                PixelColor p01 = row1[sx0], p11 = row1[sx1];
                PixelColor out = {
                    (unsigned char)(w00 * p00.r + w10 * p10.r + w01 * p01.r + w11 * p11.r + 0.5),
                    (unsigned char)(w00 * p00.g + w10 * p10.g + w01 * p01.g + w11 * p11.g + 0.5),
                    (unsigned char)(w00 * p00.b + w10 * p10.b + w01 * p01.b + w11 * p11.b + 0.5),
                };
                dst[dx] = out;
            } else if (clear) {
                PixelColor black = {0, 0, 0};
                dst[dx] = black;
            }
        }
    }
}

// Random pixel-scale jitter: the steering is chaotic in the dynamical
// sense, so a few pixels at the seed diverge into a completely different
// descent within ~10 keyframes — endless variety, and the seed invariant
// (set in frame) is untouched.
static void cine_jitter_seed(MBViewState *seed, double pixels) {
    double jx = ((double)arc4random() / UINT32_MAX - 0.5) * 2.0 * pixels;
    double jy = ((double)arc4random() / UINT32_MAX - 0.5) * 2.0 * pixels;
    FloatExp scale = mb_view_get_scale_fx(seed);
    mb_view_hp_translate_fx(seed, fx_mul_d(scale, jx), fx_mul_d(scale, jy));
}

- (void)cinematicRestartFromPreset {
    MBViewState seed;
    mb_view_state_init(&seed, _viewState.viewport_width, _viewState.viewport_height);
    const PresetLocation *p =
        &kPresetLocations[arc4random_uniform((uint32_t)kPresetCount)];
    seed.center_x = p->center_x;
    seed.center_y = p->center_y;
    mb_view_hp_sync_from_doubles(&seed);
    cine_jitter_seed(&seed, 40.0);
    mb_director_start(_cineDirector, &seed);
    _cineZ = 0.0;
    _cineCurSpeed = 0.0;
}

- (void)cinematicTick {
    if (!_cineDirector || !_framebuffer) return;

    CFAbsoluteTime now = CFAbsoluteTimeGetCurrent();
    double dt = now - _cineLastTick;
    _cineLastTick = now;
    if (dt < 0.0) dt = 0.0;
    if (dt > 0.1) dt = 0.1;

    // Advance the zoom, easing the speed by pipeline health: never outrun
    // the rendered keyframes — slow down instead of stuttering. Speed is
    // PROPORTIONAL to pipeline headroom (full at 4 keyframes ahead, zero
    // at 1) so playback pre-slows while deep keyframes render, rather
    // than sprinting into starvation and slamming to a halt. The slow
    // blend makes speed changes read as camera work, not corrections
    // (~1.25s time constant, Apple-style slow-motion ramps).
    double ready = mb_director_ready_log10(_cineDirector);
    double headroom = (ready - _cineZ - MB_CINE_STEP) / (3.0 * MB_CINE_STEP);
    if (headroom < 0.0) headroom = 0.0;
    if (headroom > 1.0) headroom = 1.0;
    double target = _cineSpeed * headroom;
    double blend = dt * 0.5;   // ~2s speed-ramp time constant
    if (blend > 1.0) blend = 1.0;
    _cineCurSpeed += (target - _cineCurSpeed) * blend;
    _cineZ += _cineCurSpeed * dt;
    if (ready > 0.05 && _cineZ > ready - 0.05) {
        _cineZ = ready - 0.05;   // hard floor against playing unrendered depth
        if (_cineZ < 0.0) _cineZ = 0.0;
    }

    // The dive is endless: restart from a fresh preset at the ceiling
    if (_cineZ >= MB_ZOOM_LOG10_MAX - 2.0 * MB_CINE_STEP) {
        [self cinematicRestartFromPreset];
    }

    [self pumpCinematic];

    // ---- Composite the two bracketing keyframes ----
    const MBCineKeyframe *lo = NULL, *hi = NULL;
    mb_director_lock_frames(_cineDirector, _cineZ, &lo, &hi);

    int w = _viewState.viewport_width;
    int h = _viewState.viewport_height;
    size_t pixels = (size_t)w * h;

    if (!lo && !hi) {
        memset(_framebuffer, 0, pixels * sizeof(PixelColor));
    } else {
        int kIdx = (int)floor(_cineZ / MB_CINE_STEP);
        double t = _cineZ / MB_CINE_STEP - kIdx;

        // Camera path: Catmull-Rom spline through the keyframe centers
        // around the playback position. A plain lerp is C0 only — the pan
        // velocity snaps at every keyframe boundary, which is exactly what
        // reads as "not cinematic". Positions are in lo-frame pixels,
        // derived from FloatExp center deltas (valid at any depth).
        double p0x = 0, p0y = 0, p2x = 0, p2y = 0, p3x = 0, p3y = 0;
        bool have2 = false;

        FloatExp sLo = lo ? mb_view_get_scale_fx(&lo->view) : fx_zero();

        if (lo && hi) {
            FloatExp dRe, dIm;
            mb_view_hp_center_delta_fx(&lo->view, &hi->view, &dRe, &dIm);
            p2x = fx_to_d(fx_div(dRe, sLo));
            p2y = -fx_to_d(fx_div(dIm, sLo));
            have2 = true;

            const MBCineKeyframe *prev = mb_director_frame_at(_cineDirector, kIdx - 1);
            if (prev) {
                mb_view_hp_center_delta_fx(&lo->view, &prev->view, &dRe, &dIm);
                p0x = fx_to_d(fx_div(dRe, sLo));
                p0y = -fx_to_d(fx_div(dIm, sLo));
            } else {
                p0x = -p2x;   // linear extrapolation
                p0y = -p2y;
            }

            const MBCineKeyframe *next = mb_director_frame_at(_cineDirector, kIdx + 2);
            if (next) {
                mb_view_hp_center_delta_fx(&lo->view, &next->view, &dRe, &dIm);
                p3x = fx_to_d(fx_div(dRe, sLo));
                p3y = -fx_to_d(fx_div(dIm, sLo));
            } else {
                p3x = 2.0 * p2x;
                p3y = 2.0 * p2y;
            }
        }

        // Catmull-Rom (p1 = lo center = origin of this coordinate frame)
        double t2 = t * t, t3 = t2 * t;
        double pcx = 0.5 * ((-p0x + p2x) * t +
                            (2.0 * p0x - 5.0 * 0 + 4.0 * p2x - p3x) * t2 +
                            (-p0x + 3.0 * 0 - 3.0 * p2x + p3x) * t3);
        double pcy = 0.5 * ((-p0y + p2y) * t +
                            (2.0 * p0y - 5.0 * 0 + 4.0 * p2y - p3y) * t2 +
                            (-p0y + 3.0 * 0 - 3.0 * p2y + p3y) * t3);
        if (!have2) {
            pcx = 0.0;
            pcy = 0.0;
        }

        if (lo) {
            [self blitCineFrame:lo->pixels ratio:pow(2.0, -t)
                           offX:pcx offY:pcy clear:YES];
        } else {
            memset(_framebuffer, 0, pixels * sizeof(PixelColor));
        }

        if (hi) {
            // Offsets relative to the hi center, in hi-frame pixels (its
            // scale is half of lo's, so lo-frame pixels double).
            double offX = (pcx - p2x) * 2.0;
            double offY = (pcy - p2y) * 2.0;
            [self blitCineFrame:hi->pixels ratio:pow(2.0, 1.0 - t)
                           offX:offX offY:offY clear:NO];
        }
    }

    mb_director_unlock_frames(_cineDirector);

    // HUD: hover-transient (auto-hides 2.5s after the mouse goes idle)
    // or pinned with the I key. Deliberately independent of the
    // interactive-mode _showHUD, which defaults ON.
    mb_view_set_zoom_log10(&_viewState, _cineZ);
    BOOL hoverHUD = _mouseInView &&
        (CFAbsoluteTimeGetCurrent() - _cineHudActivity) < 2.5;
    if (_cineHudPinned || hoverHUD) {
        [self drawHUDToFramebuffer];
    }

    [self uploadAndPresent];
}

- (void)renderFrame {
    // Safety checks
    if (!_framebuffer || _viewState.viewport_width <= 0 || _viewState.viewport_height <= 0) {
        return;
    }

    if (_viewState.zoom_level >= MB_DEEP_ZOOM_THRESHOLD) {
        // Past double precision for the map-tile pyramid: render
        // screen-space perturbation tiles instead.
        [self renderDeepFrame];
    } else {
        _deepNeedsPoll = NO;
        [self renderMapFrame];
    }

    // 7. Draw HUD if enabled
    if (_showHUD) {
        [self drawHUDToFramebuffer];
    }

    // 8. Draw minimap
    [self drawMinimapToFramebuffer];

    // 9. Draw coordinate readout
    [self drawCoordinateReadout];

    // 10. Draw preset menu (if visible)
    if (_showPresetMenu) {
        [self drawPresetMenuToFramebuffer];
    }

    // 11. Upload and present
    [self uploadAndPresent];
}

- (void)drawHUDToFramebuffer {
    // Format info strings
    double zoomL10 = mb_view_zoom_log10(&_viewState);
    double scaleL10 = fx_log10(mb_view_get_scale_fx(&_viewState));

    // Count pending tiles
    [_asyncCacheLock lock];
    NSUInteger pendingCount = _pendingTiles.count;
    NSUInteger cachedCount = _asyncTileCache.count;
    [_asyncCacheLock unlock];

    // Build HUD text
    NSString *zoomLine;
    if (zoomL10 >= 15.0) {
        // Log form for extreme zoom (a double cannot even hold 10^400)
        zoomLine = [NSString stringWithFormat:@"Zoom: 10^%.2f", zoomL10];
    } else {
        zoomLine = [NSString stringWithFormat:@"Zoom: %.2fx", _viewState.zoom_level];
    }

    NSString *centerLine;
    if (_viewState.center_y >= 0) {
        centerLine = [NSString stringWithFormat:@"Center: %.6f + %.6fi", _viewState.center_x, _viewState.center_y];
    } else {
        centerLine = [NSString stringWithFormat:@"Center: %.6f - %.6fi", _viewState.center_x, -_viewState.center_y];
    }
    NSString *scaleLine = [NSString stringWithFormat:@"Scale: 10^%.1f", scaleL10];

    NSString *precLine;
    if (_viewState.zoom_level >= MB_DEEP_ZOOM_THRESHOLD) {
        precLine = [NSString stringWithFormat:@"Perturbation: %u-bit ref, %u iter",
                    (unsigned)mp_required_precision_log10(zoomL10),
                    mb_max_iter_for_zoom_log10(zoomL10)];
    } else {
        precLine = [NSString stringWithFormat:@"Precision: 64 bits (double), %u iter",
                    mb_max_iter_for_zoom_log10(zoomL10)];
    }

    NSString *tilesLine = [NSString stringWithFormat:@"Tiles: %lu cached, %lu pending",
                          (unsigned long)cachedCount, (unsigned long)pendingCount];

    // Render settings info
    NSString *colorModeName = (_renderSettings.color_mode == MB_COLOR_MODE_SMOOTH) ? @"Smooth" : @"Classic";
    NSString *paletteName = [NSString stringWithUTF8String:kPaletteNames[_renderSettings.palette_id]];
    NSString *aaStatus = _renderSettings.antialiasing_enabled ? @"AA:ON" : @"AA:OFF";
    NSString *cycleStr = [NSString stringWithFormat:@"Cycle:%.0f", _renderSettings.color_cycle_scale];
    NSString *renderLine = [NSString stringWithFormat:@"%@ | %@ | %@ | %@", colorModeName, paletteName, aaStatus, cycleStr];

    NSString *helpLine = @"Keys: I=HUD S=smooth N=palette A=AA [/]=cycle";

    NSString *hudText = [NSString stringWithFormat:@"%@\n%@\n%@\n%@\n%@\n%@\n%@",
                        zoomLine, centerLine, scaleLine, precLine, tilesLine, renderLine, helpLine];

    // HUD dimensions
    int padding = 10;
    int lineHeight = 16;
    int hudWidth = 340;  // Wider for longer help line
    int hudHeight = lineHeight * 7 + padding * 2;  // 7 lines (added render settings)

    // Position in top-right corner
    int hudX = _viewState.viewport_width - hudWidth - padding;
    int hudY = padding;

    // Draw semi-transparent background (50% alpha black)
    for (int y = hudY; y < hudY + hudHeight && y < _viewState.viewport_height; y++) {
        for (int x = hudX; x < hudX + hudWidth && x < _viewState.viewport_width; x++) {
            if (x >= 0) {
                int idx = y * _viewState.viewport_width + x;
                // Alpha blend: dst = src * alpha + dst * (1 - alpha)
                // With 50% alpha black: dst = dst * 0.5
                _framebuffer[idx].r = _framebuffer[idx].r / 2;
                _framebuffer[idx].g = _framebuffer[idx].g / 2;
                _framebuffer[idx].b = _framebuffer[idx].b / 2;
            }
        }
    }

    // Create bitmap context for text rendering
    int textWidth = hudWidth - padding * 2;
    int textHeight = hudHeight - padding;
    size_t textPixels = textWidth * textHeight;
    uint8_t *textBitmap = calloc(textPixels * 4, 1);
    if (!textBitmap) return;

    CGColorSpaceRef colorSpace = CGColorSpaceCreateDeviceRGB();
    CGContextRef ctx = CGBitmapContextCreate(textBitmap, textWidth, textHeight, 8,
                                              textWidth * 4, colorSpace,
                                              (CGBitmapInfo)kCGImageAlphaPremultipliedLast);
    CGColorSpaceRelease(colorSpace);
    if (!ctx) {
        free(textBitmap);
        return;
    }

    // Set up text attributes using NSFont for NSAttributedString drawing
    NSFont *font = [NSFont fontWithName:@"Menlo" size:12.0];
    if (!font) font = [NSFont monospacedSystemFontOfSize:12.0 weight:NSFontWeightRegular];
    NSColor *textColor = [NSColor whiteColor];

    NSDictionary *attributes = @{
        NSFontAttributeName: font,
        NSForegroundColorAttributeName: textColor
    };

    NSAttributedString *attrStr = [[NSAttributedString alloc] initWithString:hudText
                                                                  attributes:attributes];

    // Use NSGraphicsContext for proper text drawing with flipped coordinates
    NSGraphicsContext *nsContext = [NSGraphicsContext graphicsContextWithCGContext:ctx flipped:YES];
    [NSGraphicsContext saveGraphicsState];
    [NSGraphicsContext setCurrentContext:nsContext];

    // Draw text
    [attrStr drawInRect:NSMakeRect(0, 0, textWidth, textHeight)];

    [NSGraphicsContext restoreGraphicsState];

    // Copy text bitmap to framebuffer with alpha blending
    int textStartX = hudX + padding;
    int textStartY = hudY + padding / 2;
    [self blitTextBitmap:textBitmap width:textWidth height:textHeight toX:textStartX y:textStartY];

    // Cleanup
    [attrStr release];
    CGContextRelease(ctx);
    free(textBitmap);
}

- (void)uploadAndPresent {
    size_t pixels = (size_t)_viewState.viewport_width * _viewState.viewport_height;
    uint8_t *bgra = malloc(pixels * 4);
    if (!bgra) return;

    for (size_t i = 0; i < pixels; i++) {
        bgra[i * 4 + 0] = _framebuffer[i].b;
        bgra[i * 4 + 1] = _framebuffer[i].g;
        bgra[i * 4 + 2] = _framebuffer[i].r;
        bgra[i * 4 + 3] = 255;
    }

    if (_texture) {
        MTLRegion region = MTLRegionMake2D(0, 0, _viewState.viewport_width, _viewState.viewport_height);
        [_texture replaceRegion:region mipmapLevel:0 withBytes:bgra bytesPerRow:_viewState.viewport_width * 4];
    }

    free(bgra);
    [self presentTexture];
}

- (void)presentTexture {
    if (!_metalLayer || !_commandQueue || !_texture) return;

    id<CAMetalDrawable> drawable = [_metalLayer nextDrawable];
    if (!drawable) return;

    id<MTLTexture> dstTexture = drawable.texture;
    if (!dstTexture) return;

    // Create a simple blit command to copy texture to drawable
    id<MTLCommandBuffer> commandBuffer = [_commandQueue commandBuffer];
    if (!commandBuffer) return;

    // Use blit encoder to copy texture
    id<MTLBlitCommandEncoder> blitEncoder = [commandBuffer blitCommandEncoder];
    if (!blitEncoder) {
        return;
    }

    // Calculate source and destination sizes
    NSUInteger srcWidth = _texture.width;
    NSUInteger srcHeight = _texture.height;
    NSUInteger dstWidth = dstTexture.width;
    NSUInteger dstHeight = dstTexture.height;

    NSUInteger copyWidth = MIN(srcWidth, dstWidth);
    NSUInteger copyHeight = MIN(srcHeight, dstHeight);

    if (copyWidth == 0 || copyHeight == 0) {
        [blitEncoder endEncoding];
        return;
    }

    [blitEncoder copyFromTexture:_texture
                     sourceSlice:0
                     sourceLevel:0
                    sourceOrigin:MTLOriginMake(0, 0, 0)
                      sourceSize:MTLSizeMake(copyWidth, copyHeight, 1)
                       toTexture:dstTexture
                destinationSlice:0
                destinationLevel:0
               destinationOrigin:MTLOriginMake(0, 0, 0)];

    [blitEncoder endEncoding];

    [commandBuffer presentDrawable:drawable];
    [commandBuffer commit];
}

- (void)drawRect:(NSRect)dirtyRect {
    // Metal rendering is handled by display link
}

- (MBViewState *)viewState {
    return &_viewState;
}

@end

// =============================================================================
// Window Controller
// =============================================================================

@interface MandelbrotWindowController : NSWindowController<NSWindowDelegate>
@property (nonatomic, assign) MandelbrotView *mandelbrotView;
@end

@implementation MandelbrotWindowController

- (void)windowWillClose:(NSNotification *)notification {
    [NSApp stop:nil];
    // Post a dummy event to wake up the run loop
    NSEvent *event = [NSEvent otherEventWithType:NSEventTypeApplicationDefined
                                        location:NSMakePoint(0, 0)
                                   modifierFlags:0
                                       timestamp:0
                                    windowNumber:0
                                         context:nil
                                         subtype:0
                                           data1:0
                                           data2:0];
    [NSApp postEvent:event atStart:YES];
}

@end

// =============================================================================
// Application Delegate
// =============================================================================

@interface MandelbrotAppDelegate : NSObject<NSApplicationDelegate>
@end

@implementation MandelbrotAppDelegate

- (void)applicationDidFinishLaunching:(NSNotification *)notification {
    MB_DEBUG_LOG(@"applicationDidFinishLaunching");
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)sender {
    return YES;
}

@end

static MandelbrotAppDelegate *g_appDelegate = nil;

// =============================================================================
// Global State (retained manually for non-ARC)
// =============================================================================

static NSWindow *g_window = nil;
static MandelbrotWindowController *g_windowController = nil;
static MandelbrotView *g_view = nil;
static NSSplitView *g_splitView = nil;
static MBControlPanel *g_controlPanel = nil;

// =============================================================================
// Public API
// =============================================================================

int native_viewer_init(const char *title, int width, int height, bool clear_cache) {
    MB_DEBUG_LOG(@"native_viewer_init: starting");

    // Clear disk cache if requested
    if (clear_cache) {
        NSString *cachePath = getCachePath();
        NSError *error = nil;
        if ([[NSFileManager defaultManager] removeItemAtPath:cachePath error:&error]) {
            MB_DEBUG_LOG(@"Cleared disk cache at %@", cachePath);
        } else if (error && error.code != NSFileNoSuchFileError) {
            MB_DEBUG_LOG(@"Warning: Failed to clear cache at %@: %@", cachePath, error.localizedDescription);
        }
    }

    // Initialize application
    [NSApplication sharedApplication];
    MB_DEBUG_LOG(@"native_viewer_init: got NSApp");
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];

    // Set app delegate for proper termination handling
    g_appDelegate = [[MandelbrotAppDelegate alloc] init];
    [g_appDelegate retain];
    [NSApp setDelegate:g_appDelegate];
    MB_DEBUG_LOG(@"native_viewer_init: set activation policy");

    // Create menu bar (required for proper app activation)
    NSMenu *menuBar = [[NSMenu alloc] init];
    NSMenuItem *appMenuItem = [[NSMenuItem alloc] init];
    [menuBar addItem:appMenuItem];
    [NSApp setMainMenu:menuBar];

    NSMenu *appMenu = [[NSMenu alloc] init];
    NSMenuItem *quitItem = [[NSMenuItem alloc] initWithTitle:@"Quit"
                                                      action:@selector(terminate:)
                                               keyEquivalent:@"q"];
    [appMenu addItem:quitItem];
    [appMenuItem setSubmenu:appMenu];

    // Create window
    NSRect frame = NSMakeRect(100, 100, width, height);
    NSUInteger style = NSWindowStyleMaskTitled |
                      NSWindowStyleMaskClosable |
                      NSWindowStyleMaskMiniaturizable |
                      NSWindowStyleMaskResizable;

    g_window = [[NSWindow alloc] initWithContentRect:frame
                                           styleMask:style
                                             backing:NSBackingStoreBuffered
                                               defer:NO];
    [g_window retain];  // Retain for non-ARC

    NSString *titleStr = [NSString stringWithUTF8String:title];
    [g_window setTitle:titleStr];
    [g_window setMinSize:NSMakeSize(400, 300)];

    // Create split view to hold control panel and Mandelbrot view
    MB_DEBUG_LOG(@"native_viewer_init: creating split view");
    g_splitView = [[NSSplitView alloc] initWithFrame:frame];
    [g_splitView retain];  // Retain for non-ARC
    [g_splitView setVertical:YES];  // Side-by-side layout
    [g_splitView setDividerStyle:NSSplitViewDividerStyleThin];

    // Create control panel (left side)
    NSRect panelFrame = NSMakeRect(0, 0, MB_CONTROL_PANEL_WIDTH, height);
    g_controlPanel = [[MBControlPanel alloc] initWithFrame:panelFrame];
    [g_controlPanel retain];  // Retain for non-ARC

    // Create Mandelbrot view (right side - takes remaining space)
    NSRect viewFrame = NSMakeRect(0, 0, width - MB_CONTROL_PANEL_WIDTH, height);
    MB_DEBUG_LOG(@"native_viewer_init: creating Mandelbrot view");
    g_view = [[MandelbrotView alloc] initWithFrame:viewFrame];
    if (!g_view) {
        MB_DEBUG_LOG(@"native_viewer_init: view creation FAILED");
        [g_splitView release];
        [g_controlPanel release];
        [g_window release];
        return -1;
    }
    [g_view retain];  // Retain for non-ARC
    MB_DEBUG_LOG(@"native_viewer_init: view created");

    // Add subviews to split view (order matters: first = left, second = right)
    [g_splitView addSubview:g_controlPanel];
    [g_splitView addSubview:g_view];

    // Set holding priority so control panel keeps its size when window is resized
    [g_splitView setHoldingPriority:NSLayoutPriorityDefaultHigh forSubviewAtIndex:0];
    [g_splitView setHoldingPriority:NSLayoutPriorityDefaultLow forSubviewAtIndex:1];

    // Connect control panel to view
    [g_view setControlPanel:g_controlPanel];

    [g_window setContentView:g_splitView];

    // Create window controller
    g_windowController = [[MandelbrotWindowController alloc] initWithWindow:g_window];
    [g_windowController retain];  // Retain for non-ARC
    g_windowController.mandelbrotView = g_view;
    [g_window setDelegate:g_windowController];

    // Show window
    MB_DEBUG_LOG(@"native_viewer_init: showing window");
    [g_window setIsVisible:YES];
    [g_window makeKeyAndOrderFront:nil];
    [g_window orderFrontRegardless];
    [g_window makeFirstResponder:g_view];
    [g_window center];

    // Collapse the control panel AFTER the window's first layout: a
    // divider position set pre-display gets re-proportioned by the split
    // view's initial adjustSubviews, leaving an empty strip on the left.
    [g_splitView layoutSubtreeIfNeeded];
    [g_splitView setPosition:0.0 ofDividerAtIndex:0];

    if (getenv("MB_UI_DEBUG")) {
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(1.0 * NSEC_PER_SEC)),
                       dispatch_get_main_queue(), ^{
            fprintf(stderr, "[ui] content=%s split=%s panel=%s view=%s\n",
                    NSStringFromRect([[g_window contentView] frame]).UTF8String,
                    NSStringFromRect(g_splitView.frame).UTF8String,
                    NSStringFromRect(g_controlPanel.frame).UTF8String,
                    NSStringFromRect(g_view.frame).UTF8String);
            fprintf(stderr, "[ui] view bounds=%s layer=%s\n",
                    NSStringFromRect(g_view.bounds).UTF8String,
                    NSStringFromRect(g_view.layer.frame).UTF8String);
        });
    }

    [NSApp activateIgnoringOtherApps:YES];
    MB_DEBUG_LOG(@"native_viewer_init: window visible=%d, frame=%@",
          [g_window isVisible], NSStringFromRect([g_window frame]));
    MB_DEBUG_LOG(@"native_viewer_init: done, returning 0");

    return 0;
}

void native_viewer_run(void) {
    MB_DEBUG_LOG(@"native_viewer_run: starting run loop");
    [NSApp finishLaunching];
    MB_DEBUG_LOG(@"native_viewer_run: finishLaunching done, calling run");
    [NSApp run];
    MB_DEBUG_LOG(@"native_viewer_run: run returned");
}

void native_viewer_shutdown(void) {
    if (g_view) {
        [g_view release];
        g_view = nil;
    }
    if (g_controlPanel) {
        [g_controlPanel release];
        g_controlPanel = nil;
    }
    if (g_splitView) {
        [g_splitView release];
        g_splitView = nil;
    }
    if (g_windowController) {
        [g_windowController release];
        g_windowController = nil;
    }
    if (g_window) {
        [g_window release];
        g_window = nil;
    }
    if (g_appDelegate) {
        [g_appDelegate release];
        g_appDelegate = nil;
    }
}

MBViewState* native_viewer_get_state(void) {
    if (g_view) {
        return [g_view viewState];
    }
    return NULL;
}

void native_viewer_start_cinematic(double decades_per_second) {
    if (g_view) {
        [g_view setCinematicSpeed:decades_per_second];
        [g_view enterCinematic];
    }
}
