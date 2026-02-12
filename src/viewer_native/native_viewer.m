#import <Cocoa/Cocoa.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <CoreText/CoreText.h>
#include "native_viewer.h"
#include "../compute/compute_scheduler.h"
#include "../tile_map/tile_map.h"
#include "../tile_cache/disk_cache.h"
#include "../color/color.h"
#include "../color/palettes.h"
#include "../mandelbrot/mandelbrot.h"
#include <stdlib.h>
#include <math.h>

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

@interface MandelbrotView : NSView {
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

    // Animation system
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

    // Preset menu
    BOOL _showPresetMenu;
    int _selectedPresetIdx;

    // Render settings (smooth coloring, palette)
    MBRenderSettings _renderSettings;
}

- (instancetype)initWithFrame:(NSRect)frameRect;
- (void)invalidate;
- (MBViewState *)viewState;

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

        // Render settings defaults
        _renderSettings.color_mode = MB_COLOR_MODE_CLASSIC;
        _renderSettings.palette_id = MB_PALETTE_CLASSIC;
        _renderSettings.antialiasing_enabled = false;

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
        // Realloc failed - keep old buffer and restore dimensions
        return;
    }

    // Resize texture
    [self createTexture];

    // Invalidate cache on resize
    tile_cache_new_generation(&_scheduler.cache);
    _needsRedraw = YES;
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

    if (needsHP != _highPrecisionMode) {
        _highPrecisionMode = needsHP;
        _viewState.high_precision_mode = needsHP;

        if (needsHP) {
            // Calculate precision tier
            double log_zoom = log10(_viewState.zoom_level);
            if (log_zoom < 30) _currentPrecision = MB_PREC_TIER_1;
            else if (log_zoom < 60) _currentPrecision = MB_PREC_TIER_2;
            else if (log_zoom < 120) _currentPrecision = MB_PREC_TIER_3;
            else _currentPrecision = MB_PREC_TIER_4;
        } else {
            _currentPrecision = 64;
        }
    }
}

- (void)syncHPCenterStrings {
    // Sync HP center strings from double values
    // This is used when transitioning into HP mode or after pan operations
    snprintf(_viewState.center_x_str, MB_HP_COORD_STR_LEN, "%.17g", _viewState.center_x);
    snprintf(_viewState.center_y_str, MB_HP_COORD_STR_LEN, "%.17g", _viewState.center_y);
}

// =============================================================================
// Animation System
// =============================================================================

- (double)easeInOutCubic:(double)t {
    return t < 0.5 ? 4.0 * t * t * t : 1.0 - pow(-2.0 * t + 2.0, 3.0) / 2.0;
}

- (void)startAnimationToX:(double)x y:(double)y zoom:(double)zoom {
    // Store starting state
    _animStartCenterX = _viewState.center_x;
    _animStartCenterY = _viewState.center_y;
    _animStartZoom = _viewState.zoom_level;

    // Store target state
    _animTargetCenterX = x;
    _animTargetCenterY = y;
    _animTargetZoom = zoom;

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
        _viewState.zoom_level = _animTargetZoom;
        _animating = NO;
        [_animStartTime release];
        _animStartTime = nil;
    } else {
        // Interpolate with easing
        double t = [self easeInOutCubic:_animProgress];

        _viewState.center_x = _animStartCenterX + (_animTargetCenterX - _animStartCenterX) * t;
        _viewState.center_y = _animStartCenterY + (_animTargetCenterY - _animStartCenterY) * t;

        // Logarithmic interpolation for zoom (for smooth feeling across large ranges)
        double logStartZoom = log(_animStartZoom);
        double logTargetZoom = log(_animTargetZoom);
        _viewState.zoom_level = exp(logStartZoom + (logTargetZoom - logStartZoom) * t);
    }

    // Update HP mode and sync strings
    [self updateHPMode];
    [self syncHPCenterStrings];
    [self invalidateCache];
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
            color_from_iteration(&_minimapBuffer[y * MINIMAP_WIDTH + x], iteration);
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
            color_from_iteration(&_minimapZoomedBuffer[y * MINIMAP_WIDTH + x], iteration);
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
    _needsRedraw = YES;
}

- (void)mouseEntered:(NSEvent *)event {
    _mouseInView = YES;
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
    // Get complex coords at mouse BEFORE zoom
    double scale = mb_view_get_scale(&_viewState);
    double mouse_cx = _viewState.center_x + (point.x - _viewState.viewport_width / 2.0) * scale;
    double mouse_cy = _viewState.center_y - (point.y - _viewState.viewport_height / 2.0) * scale;

    // Apply zoom
    _viewState.zoom_level *= delta;

    // Clamp zoom level (no upper limit - MPFR handles arbitrary precision)
    if (_viewState.zoom_level < 0.1) _viewState.zoom_level = 0.1;

    // Recalculate center so mouse point stays fixed
    double new_scale = mb_view_get_scale(&_viewState);
    _viewState.center_x = mouse_cx - (point.x - _viewState.viewport_width / 2.0) * new_scale;
    _viewState.center_y = mouse_cy + (point.y - _viewState.viewport_height / 2.0) * new_scale;

    // Update HP mode and sync strings
    [self updateHPMode];
    [self syncHPCenterStrings];
}

// =============================================================================
// Event Handlers
// =============================================================================

- (void)magnifyWithEvent:(NSEvent *)event {
    // Pinch-to-zoom
    double zoom = 1.0 + event.magnification;
    NSPoint loc = [self convertPoint:event.locationInWindow fromView:nil];
    // Flip Y coordinate (AppKit uses bottom-left origin)
    loc.y = self.bounds.size.height - loc.y;

    [self zoomTowardsPoint:loc delta:zoom];
    [self invalidateCache];
}

- (void)scrollWheel:(NSEvent *)event {
    if (event.modifierFlags & NSEventModifierFlagCommand) {
        // Command+scroll = zoom
        double delta = 1.0 + event.scrollingDeltaY * MB_SCROLL_SENSITIVITY;
        NSPoint loc = [self convertPoint:event.locationInWindow fromView:nil];
        loc.y = self.bounds.size.height - loc.y;
        [self zoomTowardsPoint:loc delta:delta];
    } else {
        // Regular scroll = pan
        double scale = mb_view_get_scale(&_viewState);
        _viewState.center_x -= event.scrollingDeltaX * scale;
        _viewState.center_y += event.scrollingDeltaY * scale;

        // Sync HP strings if in HP mode
        if (_highPrecisionMode) {
            [self syncHPCenterStrings];
        }
    }
    [self invalidateCache];
}

- (void)mouseDown:(NSEvent *)event {
    // Store initial position for drag
    _lastDragPoint = [self convertPoint:event.locationInWindow fromView:nil];
    _lastDragPoint.y = self.bounds.size.height - _lastDragPoint.y;
}

- (void)mouseDragged:(NSEvent *)event {
    // Pan by dragging
    NSPoint loc = [self convertPoint:event.locationInWindow fromView:nil];
    loc.y = self.bounds.size.height - loc.y;

    double scale = mb_view_get_scale(&_viewState);
    _viewState.center_x -= (loc.x - _lastDragPoint.x) * scale;
    _viewState.center_y += (loc.y - _lastDragPoint.y) * scale;

    // Sync HP strings if in HP mode
    if (_highPrecisionMode) {
        [self syncHPCenterStrings];
    }

    _lastDragPoint = loc;
    [self invalidateCache];
}

- (void)rightMouseDown:(NSEvent *)event {
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
    [self startAnimationToX:-0.5 y:0.0 zoom:1.0];
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
    [self.window close];
}

static void handlePanUp(MandelbrotView *self) {
    double panAmount = 50.0 * mb_view_get_scale(&self->_viewState);
    self->_viewState.center_y -= panAmount;
    if (self->_highPrecisionMode) [self syncHPCenterStrings];
    [self invalidateCache];
}

static void handlePanDown(MandelbrotView *self) {
    double panAmount = 50.0 * mb_view_get_scale(&self->_viewState);
    self->_viewState.center_y += panAmount;
    if (self->_highPrecisionMode) [self syncHPCenterStrings];
    [self invalidateCache];
}

static void handlePanLeft(MandelbrotView *self) {
    double panAmount = 50.0 * mb_view_get_scale(&self->_viewState);
    self->_viewState.center_x -= panAmount;
    if (self->_highPrecisionMode) [self syncHPCenterStrings];
    [self invalidateCache];
}

static void handlePanRight(MandelbrotView *self) {
    double panAmount = 50.0 * mb_view_get_scale(&self->_viewState);
    self->_viewState.center_x += panAmount;
    if (self->_highPrecisionMode) [self syncHPCenterStrings];
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
}

static void handleToggleAntialiasing(MandelbrotView *self) {
    // Toggle antialiasing (2x2 supersampling)
    self->_renderSettings.antialiasing_enabled = !self->_renderSettings.antialiasing_enabled;
    // Invalidate render cache since rendering changes
    [self invalidateRenderCache];
}

static void handlePresetKey(MandelbrotView *self, unichar key) {
    for (int i = 0; i < kPresetCount; i++) {
        if (kPresetLocations[i].key == key) {
            [self startAnimationToX:kPresetLocations[i].center_x
                                  y:kPresetLocations[i].center_y
                               zoom:kPresetLocations[i].zoom];
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
                                   zoom:kPresetLocations[_selectedPresetIdx].zoom];
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
    _needsRedraw = YES;
}

- (void)invalidateRenderCache {
    // Clear async tile cache (render settings changed)
    [_asyncCacheLock lock];
    [_asyncTileCache removeAllObjects];
    [_pendingTiles removeAllObjects];
    [_asyncCacheLock unlock];

    // Also clear scheduler cache
    tile_cache_new_generation(&_scheduler.cache);

    _needsRedraw = YES;
}

- (void)displayTimerFired:(NSTimer *)timer {
    (void)timer;

    // Update animation state
    if (_animating) {
        [self updateAnimation];
    }

    if (_needsRedraw) {
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
    return [NSString stringWithFormat:@"%d_%llu_%llu", tile->zoom, tile->x, tile->y];
}

- (BOOL)getTileFromAsyncCache:(const MapTile *)tile output:(PixelColor *)output {
    NSString *key = [self tileKeyForTile:tile];
    [_asyncCacheLock lock];
    NSData *data = _asyncTileCache[key];
    [_asyncCacheLock unlock];

    if (data && data.length == MB_MAP_TILE_SIZE * MB_MAP_TILE_SIZE * sizeof(PixelColor)) {
        memcpy(output, data.bytes, data.length);
        return YES;
    }
    return NO;
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
        // Allocate buffer for this tile
        PixelColor *tileBuf = malloc(MB_MAP_TILE_SIZE * MB_MAP_TILE_SIZE * sizeof(PixelColor));
        if (!tileBuf) {
            [_asyncCacheLock lock];
            [_pendingTiles removeObject:key];
            [_asyncCacheLock unlock];
            return;
        }

        // Compute tile (this is the expensive operation)
        // Note: We compute directly here instead of using scheduler_get_map_tile
        // because the scheduler is not thread-safe
        double min_cx, max_cx, min_cy, max_cy;
        mb_tile_to_bounds(&tile, &min_cx, &max_cx, &min_cy, &max_cy);

        double scale_x = (max_cx - min_cx) / MB_MAP_TILE_SIZE;
        double scale_y = (max_cy - min_cy) / MB_MAP_TILE_SIZE;

        for (int ly = 0; ly < MB_MAP_TILE_SIZE; ly++) {
            double cy = min_cy + (ly + 0.5) * scale_y;
            for (int lx = 0; lx < MB_MAP_TILE_SIZE; lx++) {
                double cx = min_cx + (lx + 0.5) * scale_x;
                float final_z2;
                unsigned int iteration = mb_compute_point_smooth(cx, cy, MAX_ITER, &final_z2);
                color_from_iteration_ex(&tileBuf[ly * MB_MAP_TILE_SIZE + lx],
                                        iteration, final_z2, MAX_ITER, &capturedSettings);
            }
        }

        // Store in async cache (with memory limit)
        NSData *tileData = [NSData dataWithBytes:tileBuf length:MB_MAP_TILE_SIZE * MB_MAP_TILE_SIZE * sizeof(PixelColor)];

        [_asyncCacheLock lock];
        // Limit cache to 256 tiles (~64MB) to prevent unbounded memory growth
        if (_asyncTileCache.count >= 256) {
            // Clear oldest entries (simple strategy: remove all when limit hit)
            [_asyncTileCache removeAllObjects];
        }
        _asyncTileCache[key] = tileData;
        [_pendingTiles removeObject:key];
        [_asyncCacheLock unlock];

        free(tileBuf);

        // Trigger redraw on main thread
        dispatch_async(dispatch_get_main_queue(), ^{
            _needsRedraw = YES;
        });
    });
}

- (BOOL)getParentTileData:(const MapTile *)tile buffer:(PixelColor *)output {
    if (tile->zoom <= 0) return NO;

    MapTile parent = {
        .zoom = tile->zoom - 1,
        .x = tile->x / 2,
        .y = tile->y / 2
    };

    // Check async cache first
    if ([self getTileFromAsyncCache:&parent output:output]) return YES;

    // Then check disk cache
    if (parent.zoom <= MB_DISK_CACHE_MAX_ZOOM && _scheduler.disk_cache &&
        disk_cache_get(_scheduler.disk_cache, &parent, output) == 0) return YES;

    // Recursively try grandparent
    return [self getParentTileData:&parent buffer:output];
}

- (int)getParentZoomOffset:(const MapTile *)tile {
    // Returns how many zoom levels up we had to go to find a cached parent
    if (tile->zoom <= 0) return -1;

    MapTile parent = {
        .zoom = tile->zoom - 1,
        .x = tile->x / 2,
        .y = tile->y / 2
    };

    // Check async cache first
    if ([self getTileFromAsyncCache:&parent output:_parentTileBuf]) return 1;

    // Then check disk cache
    if (parent.zoom <= MB_DISK_CACHE_MAX_ZOOM && _scheduler.disk_cache &&
        disk_cache_get(_scheduler.disk_cache, &parent, _parentTileBuf) == 0) return 1;

    // Recursively try grandparent
    int parentOffset = [self getParentZoomOffset:&parent];
    if (parentOffset > 0) return parentOffset + 1;

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

    int screen_x0 = (int)((min_cx - _viewState.center_x) / viewScale + vp_half_w);
    int screen_y0 = (int)((_viewState.center_y - max_cy) / viewScale + vp_half_h);  // Y inverted: max_cy → top of screen
    int screen_x1 = (int)((max_cx - _viewState.center_x) / viewScale + vp_half_w);
    int screen_y1 = (int)((_viewState.center_y - min_cy) / viewScale + vp_half_h);  // Y inverted: min_cy → bottom of screen

    NSRect screenRect = NSMakeRect(screen_x0, screen_y0, screen_x1 - screen_x0, screen_y1 - screen_y0);

    // 3. If still not ready, try parent tile fallback, then queue async computation
    if (!tileReady) {
        MapTile tileCopy = *tile;
        [self queueAsyncTileComputation:tileCopy];

        // Try to render parent tile as placeholder
        if ([self getParentTileData:tile buffer:_parentTileBuf]) {
            // Calculate how many zoom levels up we went
            int zoomOffset = 1;
            MapTile parent = { .zoom = tile->zoom - 1, .x = tile->x / 2, .y = tile->y / 2 };

            // Check if this parent was the one that matched
            if (![self getTileFromAsyncCache:&parent output:_mapTileBuf] &&
                (parent.zoom > MB_DISK_CACHE_MAX_ZOOM || !_scheduler.disk_cache ||
                 disk_cache_get(_scheduler.disk_cache, &parent, _mapTileBuf) != 0)) {
                // Parent didn't match, must be grandparent or further up
                zoomOffset = 2;
                MapTile grandparent = { .zoom = tile->zoom - 2, .x = tile->x / 4, .y = tile->y / 4 };
                if (![self getTileFromAsyncCache:&grandparent output:_mapTileBuf] &&
                    (grandparent.zoom > MB_DISK_CACHE_MAX_ZOOM || !_scheduler.disk_cache ||
                     disk_cache_get(_scheduler.disk_cache, &grandparent, _mapTileBuf) != 0)) {
                    zoomOffset = 3;
                }
            }

            // Calculate which sub-region of the ancestor corresponds to this tile
            int mask = (1 << zoomOffset) - 1;
            int qx = tile->x & mask;
            int qy = tile->y & mask;

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

- (void)renderFrame {
    // Safety checks
    if (!_framebuffer || _viewState.viewport_width <= 0 || _viewState.viewport_height <= 0) {
        return;
    }

    scheduler_update_view(&_scheduler, &_viewState);

    // 1. Calculate target zoom from view scale
    //    We want tiles at ~1:1 pixel scale (256 tile pixels ≈ 256 screen pixels)
    double viewScale = mb_view_get_scale(&_viewState);
    double tileWidthNeeded = viewScale * MB_MAP_TILE_SIZE;  // complex width of 256 screen pixels
    int targetZoom = (int)round(log2(MB_REAL_WIDTH / tileWidthNeeded));
    if (targetZoom < 0) targetZoom = 0;
    if (targetZoom > MB_MAX_ZOOM) targetZoom = MB_MAX_ZOOM;

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
    double scale = mb_view_get_scale(&_viewState);

    // Count pending tiles
    [_asyncCacheLock lock];
    NSUInteger pendingCount = _pendingTiles.count;
    NSUInteger cachedCount = _asyncTileCache.count;
    [_asyncCacheLock unlock];

    // Build HUD text
    NSString *zoomLine;
    if (_viewState.zoom_level >= 1e15) {
        // Use scientific notation for extreme zoom
        zoomLine = [NSString stringWithFormat:@"Zoom: %.2e", _viewState.zoom_level];
    } else {
        zoomLine = [NSString stringWithFormat:@"Zoom: %.2fx", _viewState.zoom_level];
    }

    NSString *centerLine;
    if (_viewState.center_y >= 0) {
        centerLine = [NSString stringWithFormat:@"Center: %.6f + %.6fi", _viewState.center_x, _viewState.center_y];
    } else {
        centerLine = [NSString stringWithFormat:@"Center: %.6f - %.6fi", _viewState.center_x, -_viewState.center_y];
    }
    NSString *scaleLine = [NSString stringWithFormat:@"Scale: %.2e", scale];

    NSString *precLine;
    if (_highPrecisionMode) {
        precLine = [NSString stringWithFormat:@"Precision: %u bits (HP)", _currentPrecision];
    } else {
        precLine = @"Precision: 64 bits (double)";
    }

    NSString *tilesLine = [NSString stringWithFormat:@"Tiles: %lu cached, %lu pending",
                          (unsigned long)cachedCount, (unsigned long)pendingCount];

    // Render settings info
    NSString *colorModeName = (_renderSettings.color_mode == MB_COLOR_MODE_SMOOTH) ? @"Smooth" : @"Classic";
    NSString *paletteName = [NSString stringWithUTF8String:kPaletteNames[_renderSettings.palette_id]];
    NSString *aaStatus = _renderSettings.antialiasing_enabled ? @"AA:ON" : @"AA:OFF";
    NSString *renderLine = [NSString stringWithFormat:@"%@ | %@ | %@", colorModeName, paletteName, aaStatus];

    NSString *helpLine = @"Keys: I=HUD S=smooth N=palette A=AA P=presets R=reset";

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

    // Create view
    MB_DEBUG_LOG(@"native_viewer_init: creating view");
    g_view = [[MandelbrotView alloc] initWithFrame:frame];
    if (!g_view) {
        MB_DEBUG_LOG(@"native_viewer_init: view creation FAILED");
        [g_window release];
        return -1;
    }
    [g_view retain];  // Retain for non-ARC
    MB_DEBUG_LOG(@"native_viewer_init: view created");

    [g_window setContentView:g_view];

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
