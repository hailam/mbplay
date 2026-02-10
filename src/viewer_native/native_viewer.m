#import <Cocoa/Cocoa.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#include "native_viewer.h"
#include "../compute/compute_scheduler.h"
#include "../color/color.h"
#include <stdlib.h>

// =============================================================================
// Constants
// =============================================================================

#define TILE_SIZE MB_INTERACTIVE_TILE_SIZE
#define MAX_ITER 1000

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
}

- (instancetype)initWithFrame:(NSRect)frameRect;
- (void)invalidate;
- (MBViewState *)viewState;

@end

@implementation MandelbrotView

- (instancetype)initWithFrame:(NSRect)frameRect {
    NSLog(@"MandelbrotView initWithFrame: starting, size=%fx%f", frameRect.size.width, frameRect.size.height);
    self = [super initWithFrame:frameRect];
    if (self) {
        // Initialize view state FIRST (needed for makeBackingLayer)
        mb_view_state_init(&_viewState, (int)frameRect.size.width, (int)frameRect.size.height);

        // Initialize Metal (before wantsLayer triggers makeBackingLayer)
        _device = MTLCreateSystemDefaultDevice();
        if (!_device) {
            NSLog(@"Metal is not supported on this device");
            return nil;
        }
        NSLog(@"MandelbrotView: got Metal device: %@", _device.name);
        _commandQueue = [_device newCommandQueue];

        // Now enable layer-backing (this calls makeBackingLayer)
        self.wantsLayer = YES;

        // Ensure the layer has our device and correct size
        if (_metalLayer) {
            _metalLayer.device = _device;
            _metalLayer.drawableSize = CGSizeMake(frameRect.size.width, frameRect.size.height);
            NSLog(@"MandelbrotView: Metal layer configured with size %fx%f", frameRect.size.width, frameRect.size.height);
        } else {
            NSLog(@"MandelbrotView: WARNING - no metal layer after wantsLayer=YES");
        }

        // Initialize scheduler
        NSLog(@"MandelbrotView: initializing scheduler");
        if (scheduler_init(&_scheduler, TILE_SIZE, MAX_ITER) != 0) {
            NSLog(@"Failed to initialize compute scheduler");
            return nil;
        }
        NSLog(@"MandelbrotView: scheduler initialized");

        // Allocate framebuffer
        size_t pixels = (size_t)_viewState.viewport_width * _viewState.viewport_height;
        _framebuffer = malloc(pixels * sizeof(PixelColor));
        if (!_framebuffer) {
            scheduler_cleanup(&_scheduler);
            return nil;
        }

        _needsRedraw = YES;
        _animating = NO;

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

    // Resize framebuffer
    size_t pixels = (size_t)_viewState.viewport_width * _viewState.viewport_height;
    PixelColor *newBuf = realloc(_framebuffer, pixels * sizeof(PixelColor));
    if (newBuf) {
        _framebuffer = newBuf;
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
// Zoom towards point math
// =============================================================================

- (void)zoomTowardsPoint:(NSPoint)point delta:(double)delta {
    // Get complex coords at mouse BEFORE zoom
    double scale = mb_view_get_scale(&_viewState);
    double mouse_cx = _viewState.center_x + (point.x - _viewState.viewport_width / 2.0) * scale;
    double mouse_cy = _viewState.center_y + (point.y - _viewState.viewport_height / 2.0) * scale;

    // Apply zoom
    _viewState.zoom_level *= delta;

    // Clamp zoom level
    if (_viewState.zoom_level < 0.1) _viewState.zoom_level = 0.1;
    if (_viewState.zoom_level > 1e15) _viewState.zoom_level = 1e15;

    // Recalculate center so mouse point stays fixed
    double new_scale = mb_view_get_scale(&_viewState);
    _viewState.center_x = mouse_cx - (point.x - _viewState.viewport_width / 2.0) * new_scale;
    _viewState.center_y = mouse_cy - (point.y - _viewState.viewport_height / 2.0) * new_scale;
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
        double delta = 1.0 + event.scrollingDeltaY * 0.05;
        NSPoint loc = [self convertPoint:event.locationInWindow fromView:nil];
        loc.y = self.bounds.size.height - loc.y;
        [self zoomTowardsPoint:loc delta:delta];
    } else {
        // Regular scroll = pan
        double scale = mb_view_get_scale(&_viewState);
        _viewState.center_x -= event.scrollingDeltaX * scale;
        _viewState.center_y += event.scrollingDeltaY * scale;
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
    _viewState.center_y -= (loc.y - _lastDragPoint.y) * scale;

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

- (void)keyDown:(NSEvent *)event {
    NSString *chars = [event charactersIgnoringModifiers];
    if (!chars || [chars length] == 0) {
        [super keyDown:event];
        return;
    }
    unichar key = [chars characterAtIndex:0];

    double panAmount = 50.0 * mb_view_get_scale(&_viewState);

    switch (key) {
        case 'r':
        case 'R':
            // Reset view
            mb_view_state_init(&_viewState, _viewState.viewport_width, _viewState.viewport_height);
            [self invalidateCache];
            return;
        case '=':
        case '+':
            // Zoom in
            [self zoomTowardsPoint:NSMakePoint(_viewState.viewport_width/2, _viewState.viewport_height/2) delta:1.5];
            [self invalidateCache];
            return;
        case '-':
            // Zoom out
            [self zoomTowardsPoint:NSMakePoint(_viewState.viewport_width/2, _viewState.viewport_height/2) delta:0.67];
            [self invalidateCache];
            return;
        case NSUpArrowFunctionKey:
            _viewState.center_y -= panAmount;
            [self invalidateCache];
            return;
        case NSDownArrowFunctionKey:
            _viewState.center_y += panAmount;
            [self invalidateCache];
            return;
        case NSLeftArrowFunctionKey:
            _viewState.center_x -= panAmount;
            [self invalidateCache];
            return;
        case NSRightArrowFunctionKey:
            _viewState.center_x += panAmount;
            [self invalidateCache];
            return;
        case 27: // Escape
            [self.window close];
            return;
        default:
            [super keyDown:event];
            return;
    }
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

- (void)displayTimerFired:(NSTimer *)timer {
    (void)timer;
    if (_needsRedraw) {
        [self renderFrame];
        _needsRedraw = NO;
    }
}

- (void)renderFrame {
    // Safety checks
    if (!_framebuffer || _viewState.viewport_width <= 0 || _viewState.viewport_height <= 0) {
        return;
    }

    // Update scheduler with current view state
    scheduler_update_view(&_scheduler, &_viewState);

    // Calculate visible tiles
    int start_tx, start_ty, end_tx, end_ty;
    scheduler_get_visible_tiles(&_viewState, TILE_SIZE,
                                &start_tx, &start_ty, &end_tx, &end_ty);

    // Temporary buffer for a single tile
    PixelColor *tileBuf = malloc(TILE_SIZE * TILE_SIZE * sizeof(PixelColor));
    if (!tileBuf) {
        NSLog(@"renderFrame: failed to allocate tile buffer");
        return;
    }

    // Clear framebuffer
    memset(_framebuffer, 0, (size_t)_viewState.viewport_width * _viewState.viewport_height * sizeof(PixelColor));

    // Render visible tiles
    for (int ty = start_ty; ty < end_ty; ty++) {
        for (int tx = start_tx; tx < end_tx; tx++) {
            // Get or compute tile
            if (scheduler_get_tile(&_scheduler, &_viewState, tx, ty, tileBuf)) {
                // Copy tile to framebuffer
                int px = tx * TILE_SIZE;
                int py = ty * TILE_SIZE;

                for (int ly = 0; ly < TILE_SIZE; ly++) {
                    int fy = py + ly;
                    if (fy < 0 || fy >= _viewState.viewport_height) continue;

                    for (int lx = 0; lx < TILE_SIZE; lx++) {
                        int fx = px + lx;
                        if (fx < 0 || fx >= _viewState.viewport_width) continue;

                        _framebuffer[fy * _viewState.viewport_width + fx] =
                            tileBuf[ly * TILE_SIZE + lx];
                    }
                }
            }
        }
    }

    free(tileBuf);

    // Convert RGB to BGRA for Metal texture (CAMetalLayer uses BGRA)
    size_t pixels = (size_t)_viewState.viewport_width * _viewState.viewport_height;
    uint8_t *bgra = malloc(pixels * 4);
    if (!bgra) return;

    for (size_t i = 0; i < pixels; i++) {
        bgra[i * 4 + 0] = _framebuffer[i].b;  // B
        bgra[i * 4 + 1] = _framebuffer[i].g;  // G
        bgra[i * 4 + 2] = _framebuffer[i].r;  // R
        bgra[i * 4 + 3] = 255;                 // A
    }

    // Upload to texture
    if (_texture) {
        MTLRegion region = MTLRegionMake2D(0, 0, _viewState.viewport_width, _viewState.viewport_height);
        [_texture replaceRegion:region mipmapLevel:0 withBytes:bgra bytesPerRow:_viewState.viewport_width * 4];
    }

    free(bgra);

    // Render texture to screen using Metal
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
    NSLog(@"applicationDidFinishLaunching");
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

int native_viewer_init(const char *title, int width, int height) {
    NSLog(@"native_viewer_init: starting");

    // Initialize application
    [NSApplication sharedApplication];
    NSLog(@"native_viewer_init: got NSApp");
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];

    // Set app delegate for proper termination handling
    g_appDelegate = [[MandelbrotAppDelegate alloc] init];
    [g_appDelegate retain];
    [NSApp setDelegate:g_appDelegate];
    NSLog(@"native_viewer_init: set activation policy");

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
    NSLog(@"native_viewer_init: creating view");
    g_view = [[MandelbrotView alloc] initWithFrame:frame];
    if (!g_view) {
        NSLog(@"native_viewer_init: view creation FAILED");
        [g_window release];
        return -1;
    }
    [g_view retain];  // Retain for non-ARC
    NSLog(@"native_viewer_init: view created");

    [g_window setContentView:g_view];

    // Create window controller
    g_windowController = [[MandelbrotWindowController alloc] initWithWindow:g_window];
    [g_windowController retain];  // Retain for non-ARC
    g_windowController.mandelbrotView = g_view;
    [g_window setDelegate:g_windowController];

    // Show window
    NSLog(@"native_viewer_init: showing window");
    [g_window setIsVisible:YES];
    [g_window makeKeyAndOrderFront:nil];
    [g_window orderFrontRegardless];
    [g_window makeFirstResponder:g_view];
    [g_window center];

    [NSApp activateIgnoringOtherApps:YES];
    NSLog(@"native_viewer_init: window visible=%d, frame=%@",
          [g_window isVisible], NSStringFromRect([g_window frame]));
    NSLog(@"native_viewer_init: done, returning 0");

    return 0;
}

void native_viewer_run(void) {
    NSLog(@"native_viewer_run: starting run loop");
    [NSApp finishLaunching];
    NSLog(@"native_viewer_run: finishLaunching done, calling run");
    [NSApp run];
    NSLog(@"native_viewer_run: run returned");
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
