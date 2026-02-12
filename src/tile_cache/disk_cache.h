#ifndef MB_DISK_CACHE_H
#define MB_DISK_CACHE_H

#include "../tile_map/tile_map.h"
#include "../config.h"
#include <stdint.h>

// =============================================================================
// Disk Cache for Persistent Tile Storage
// =============================================================================
//
// Storage layout:
//   {base_path}/{z}/{x}/{y}.qoi
//
// Zoom 0-20: Disk cached
// Zoom 21+:  Memory only (too many potential tiles)
//
// Uses QOI format for fast encoding/decoding.

#define MB_DISK_CACHE_MAX_ZOOM 20
#define MB_DISK_CACHE_DEFAULT_SIZE (1024LL * 1024 * 1024)  // 1GB default
#define MB_MAX_TILE_FILE_SIZE (1024 * 1024)  // 1MB max tile file (safety limit)

typedef struct DiskCache DiskCache;

/**
 * Initialize disk cache.
 * @param base_path Base directory for tile storage (e.g., ~/.mandelbrot/tiles)
 * @param max_size_bytes Maximum cache size in bytes (0 for default 1GB)
 * @return Initialized cache, or NULL on failure
 */
DiskCache *disk_cache_init(const char *base_path, int64_t max_size_bytes);

/**
 * Get a tile from disk cache.
 * @param cache The disk cache
 * @param tile Tile identifier
 * @param pixels Output buffer (MB_TILE_SIZE * MB_TILE_SIZE pixels)
 * @return 0 on success (cache hit), -1 on miss or error
 */
int disk_cache_get(DiskCache *cache, const MapTile *tile, PixelColor *pixels);

/**
 * Store a tile to disk cache.
 * May trigger LRU eviction if cache is full.
 * @param cache The disk cache
 * @param tile Tile identifier
 * @param pixels Pixel data to store
 * @return 0 on success, -1 on error
 */
int disk_cache_put(DiskCache *cache, const MapTile *tile, const PixelColor *pixels);

/**
 * Check if a tile exists in disk cache.
 * @param cache The disk cache
 * @param tile Tile identifier
 * @return 1 if exists, 0 if not
 */
int disk_cache_exists(DiskCache *cache, const MapTile *tile);

/**
 * Get current cache size.
 * @param cache The disk cache
 * @return Current cache size in bytes
 */
int64_t disk_cache_size(const DiskCache *cache);

/**
 * Clear all cached tiles.
 * @param cache The disk cache
 */
void disk_cache_clear(DiskCache *cache);

/**
 * Clean up disk cache resources.
 * @param cache The disk cache
 */
void disk_cache_cleanup(DiskCache *cache);

#endif // MB_DISK_CACHE_H
