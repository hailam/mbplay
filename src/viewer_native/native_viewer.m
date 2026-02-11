#import <Cocoa/Cocoa.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#include "native_viewer.h"
#include "../compute/compute_scheduler.h"
#include "../tile_map/tile_map.h"
#include "../tile_cache/disk_cache.h"
#include "../color/color.h"
#include "../mandelbrot/mandelbrot.h"
#include <stdlib.h>
#include <math.h>

#define CACHE_PATH "~/.mandelbrot/tiles"

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

    // Map tile buffers
    PixelColor *_mapTileBuf;        // 256x256 tile buffer
    PixelColor *_scaledTileBuf;     // For upscaling low-res tiles

    // Async tile computation
    dispatch_queue_t _tileQueue;
    NSMutableSet<NSString *> *_pendingTiles;
    NSMutableDictionary<NSString *, NSData *> *_asyncTileCache;
    NSLock *_asyncCacheLock;
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

        // Initialize disk cache
        NSString *cachePath = [@CACHE_PATH stringByExpandingTildeInPath];
        if (scheduler_init_disk_cache(&_scheduler, [cachePath UTF8String], 0) != 0) {
            NSLog(@"Warning: Failed to initialize disk cache, tiles won't persist");
        } else {
            NSLog(@"MandelbrotView: disk cache initialized at %@", cachePath);
        }

        // Allocate map tile buffers (256x256)
        _mapTileBuf = malloc(MB_MAP_TILE_SIZE * MB_MAP_TILE_SIZE * sizeof(PixelColor));
        _scaledTileBuf = malloc(MB_MAP_TILE_SIZE * MB_MAP_TILE_SIZE * sizeof(PixelColor));
        if (!_mapTileBuf || !_scaledTileBuf) {
            NSLog(@"Failed to allocate map tile buffers");
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
    double mouse_cy = _viewState.center_y - (point.y - _viewState.viewport_height / 2.0) * scale;

    // Apply zoom
    _viewState.zoom_level *= delta;

    // Clamp zoom level
    if (_viewState.zoom_level < 0.1) _viewState.zoom_level = 0.1;
    if (_viewState.zoom_level > 1e15) _viewState.zoom_level = 1e15;

    // Recalculate center so mouse point stays fixed
    double new_scale = mb_view_get_scale(&_viewState);
    _viewState.center_x = mouse_cx - (point.x - _viewState.viewport_width / 2.0) * new_scale;
    _viewState.center_y = mouse_cy + (point.y - _viewState.viewport_height / 2.0) * new_scale;
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
    // Clear async tile cache (view changed, old tiles no longer valid for rendering)
    // But we keep the computed tiles since they'll be saved to disk cache
    _needsRedraw = YES;
}

- (void)displayTimerFired:(NSTimer *)timer {
    (void)timer;
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

    for (int dy = dy0; dy < dy1; dy++) {
        double sy = (dstRect.size.height - 1 - (dy - dstRect.origin.y)) * scaleY;
        int syi = (int)sy;
        if (syi >= srcSize) syi = srcSize - 1;
        if (syi < 0) syi = 0;

        for (int dx = dx0; dx < dx1; dx++) {
            double sx = (dx - dstRect.origin.x) * scaleX;
            int sxi = (int)sx;
            if (sxi >= srcSize) sxi = srcSize - 1;
            if (sxi < 0) sxi = 0;

            _framebuffer[dy * _viewState.viewport_width + dx] =
                src[syi * srcSize + sxi];
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

                unsigned int iteration;
                if (mb_is_in_cardioid_or_bulb(cx, cy)) {
                    iteration = MAX_ITER;
                } else {
                    double zx = 0.0, zy = 0.0;
                    double zx2 = 0.0, zy2 = 0.0;
                    iteration = 0;

                    while (zx2 + zy2 < 4.0 && iteration < MAX_ITER) {
                        zy = 2.0 * zx * zy + cy;
                        zx = zx2 - zy2 + cx;
                        zx2 = zx * zx;
                        zy2 = zy * zy;
                        iteration++;
                    }
                }

                color_from_iteration(&tileBuf[ly * MB_MAP_TILE_SIZE + lx], iteration);
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

    // 3. If still not ready, queue async computation
    if (!tileReady) {
        MapTile tileCopy = *tile;
        [self queueAsyncTileComputation:tileCopy];
        return;  // Don't render - placeholder tiles will show instead
    }

    // Get tile bounds in complex plane
    double min_cx, max_cx, min_cy, max_cy;
    mb_tile_to_bounds(tile, &min_cx, &max_cx, &min_cy, &max_cy);

    // Convert to screen coordinates using VIEW scale (not tile scale!)
    int vp_half_w = _viewState.viewport_width / 2;
    int vp_half_h = _viewState.viewport_height / 2;

    int screen_x0 = (int)((min_cx - _viewState.center_x) / viewScale + vp_half_w);
    int screen_y0 = (int)((_viewState.center_y - max_cy) / viewScale + vp_half_h);  // Y inverted: max_cy → top of screen
    int screen_x1 = (int)((max_cx - _viewState.center_x) / viewScale + vp_half_w);
    int screen_y1 = (int)((_viewState.center_y - min_cy) / viewScale + vp_half_h);  // Y inverted: min_cy → bottom of screen

    [self blitTile:_mapTileBuf
          fromSize:MB_MAP_TILE_SIZE
            toRect:NSMakeRect(screen_x0, screen_y0,
                              screen_x1 - screen_x0,
                              screen_y1 - screen_y0)];
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

    // 7. Upload and present
    [self uploadAndPresent];
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

int native_viewer_init(const char *title, int width, int height, bool clear_cache) {
    NSLog(@"native_viewer_init: starting");

    // Clear disk cache if requested
    if (clear_cache) {
        NSString *cachePath = [@CACHE_PATH stringByExpandingTildeInPath];
        NSError *error = nil;
        if ([[NSFileManager defaultManager] removeItemAtPath:cachePath error:&error]) {
            NSLog(@"Cleared disk cache at %@", cachePath);
        } else if (error && error.code != NSFileNoSuchFileError) {
            NSLog(@"Warning: Failed to clear cache at %@: %@", cachePath, error.localizedDescription);
        }
    }

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
