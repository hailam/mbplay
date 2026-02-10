#ifndef MB_TILE_CACHE_H
#define MB_TILE_CACHE_H

#include "../config.h"
#include <stdint.h>
#include <stdbool.h>

// =============================================================================
// Tile Cache API
// =============================================================================

// Tile identifier (combines position and view generation for invalidation)
typedef struct {
    int32_t tile_x;      // Tile X index (in tile units, not pixels)
    int32_t tile_y;      // Tile Y index (in tile units, not pixels)
    uint32_t generation; // View generation (incremented on zoom/pan)
} TileKey;

// Cached tile with pixel data
typedef struct {
    TileKey key;
    PixelColor *pixels;  // MB_INTERACTIVE_TILE_SIZE^2 pixels
    uint64_t last_used;  // For LRU eviction
    bool valid;
} CachedTile;

// Tile cache state
typedef struct {
    CachedTile *tiles;          // Array of cached tiles
    int capacity;               // Max number of tiles (MB_MAX_CACHED_TILES)
    int count;                  // Current number of valid tiles
    uint64_t access_counter;    // Monotonic counter for LRU
    uint32_t current_generation; // Current view generation
    void *mutex;                // pthread_mutex_t* for thread safety
} TileCache;

/**
 * Initialize the tile cache.
 * @param cache Cache to initialize
 * @return 0 on success, -1 on failure
 */
int tile_cache_init(TileCache *cache);

/**
 * Look up a tile in the cache.
 * @param cache The tile cache
 * @param key Tile key to look up
 * @return Pointer to cached tile if found and valid, NULL otherwise
 */
CachedTile* tile_cache_get(TileCache *cache, const TileKey *key);

/**
 * Add or update a tile in the cache.
 * May evict the least-recently-used tile if cache is full.
 * @param cache The tile cache
 * @param key Tile key
 * @param pixels Pixel data (will be copied)
 * @return 0 on success, -1 on failure
 */
int tile_cache_put(TileCache *cache, const TileKey *key, const PixelColor *pixels);

/**
 * Increment the view generation, invalidating all cached tiles.
 * Call this when zoom level changes significantly.
 * @param cache The tile cache
 */
void tile_cache_new_generation(TileCache *cache);

/**
 * Get current generation number.
 * @param cache The tile cache
 * @return Current generation number
 */
uint32_t tile_cache_get_generation(const TileCache *cache);

/**
 * Clear all cached tiles.
 * @param cache The tile cache
 */
void tile_cache_clear(TileCache *cache);

/**
 * Clean up tile cache resources.
 * @param cache The tile cache
 */
void tile_cache_cleanup(TileCache *cache);

/**
 * Get cache statistics.
 * @param cache The tile cache
 * @param hits Output: number of cache hits
 * @param misses Output: number of cache misses
 */
void tile_cache_stats(const TileCache *cache, uint64_t *hits, uint64_t *misses);

#endif // MB_TILE_CACHE_H
