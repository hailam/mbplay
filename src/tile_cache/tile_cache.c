#include "tile_cache.h"
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

// =============================================================================
// Internal State
// =============================================================================

static uint64_t cache_hits = 0;
static uint64_t cache_misses = 0;

// =============================================================================
// Helper Functions
// =============================================================================

static inline bool tile_key_equals(const TileKey *a, const TileKey *b) {
    return a->tile_x == b->tile_x &&
           a->tile_y == b->tile_y &&
           a->generation == b->generation;
}

static int find_lru_slot(TileCache *cache) {
    uint64_t min_time = UINT64_MAX;
    int min_idx = 0;

    for (int i = 0; i < cache->capacity; i++) {
        if (!cache->tiles[i].valid) {
            return i;  // Found an empty slot
        }
        if (cache->tiles[i].last_used < min_time) {
            min_time = cache->tiles[i].last_used;
            min_idx = i;
        }
    }

    return min_idx;  // Evict LRU tile
}

// =============================================================================
// Public API Implementation
// =============================================================================

int tile_cache_init(TileCache *cache) {
    cache->capacity = MB_MAX_CACHED_TILES;
    cache->count = 0;
    cache->access_counter = 0;
    cache->current_generation = 0;

    // Allocate tile array
    cache->tiles = calloc(cache->capacity, sizeof(CachedTile));
    if (!cache->tiles) {
        return -1;
    }

    // Allocate pixel buffers for each tile slot
    size_t tile_pixels = MB_INTERACTIVE_TILE_SIZE * MB_INTERACTIVE_TILE_SIZE;
    for (int i = 0; i < cache->capacity; i++) {
        cache->tiles[i].pixels = malloc(tile_pixels * sizeof(PixelColor));
        if (!cache->tiles[i].pixels) {
            // Clean up already allocated buffers
            for (int j = 0; j < i; j++) {
                free(cache->tiles[j].pixels);
            }
            free(cache->tiles);
            return -1;
        }
        cache->tiles[i].valid = false;
    }

    // Create mutex
    pthread_mutex_t *mutex = malloc(sizeof(pthread_mutex_t));
    if (!mutex) {
        for (int i = 0; i < cache->capacity; i++) {
            free(cache->tiles[i].pixels);
        }
        free(cache->tiles);
        return -1;
    }
    pthread_mutex_init(mutex, NULL);
    cache->mutex = mutex;

    return 0;
}

int tile_cache_get(TileCache *cache, const TileKey *key, PixelColor *output) {
    pthread_mutex_t *mutex = (pthread_mutex_t*)cache->mutex;
    pthread_mutex_lock(mutex);

    for (int i = 0; i < cache->capacity; i++) {
        if (cache->tiles[i].valid && tile_key_equals(&cache->tiles[i].key, key)) {
            cache->tiles[i].last_used = ++cache->access_counter;
            cache_hits++;
            // Copy pixels while holding the lock to prevent use-after-free
            size_t tile_pixels = MB_INTERACTIVE_TILE_SIZE * MB_INTERACTIVE_TILE_SIZE;
            memcpy(output, cache->tiles[i].pixels, tile_pixels * sizeof(PixelColor));
            pthread_mutex_unlock(mutex);
            return 0;
        }
    }

    cache_misses++;
    pthread_mutex_unlock(mutex);
    return -1;
}

int tile_cache_put(TileCache *cache, const TileKey *key, const PixelColor *pixels) {
    pthread_mutex_t *mutex = (pthread_mutex_t*)cache->mutex;
    pthread_mutex_lock(mutex);

    // Check if tile already exists
    for (int i = 0; i < cache->capacity; i++) {
        if (cache->tiles[i].valid && tile_key_equals(&cache->tiles[i].key, key)) {
            // Update existing tile
            size_t tile_pixels = MB_INTERACTIVE_TILE_SIZE * MB_INTERACTIVE_TILE_SIZE;
            memcpy(cache->tiles[i].pixels, pixels, tile_pixels * sizeof(PixelColor));
            cache->tiles[i].last_used = ++cache->access_counter;
            pthread_mutex_unlock(mutex);
            return 0;
        }
    }

    // Find slot for new tile (LRU eviction if needed)
    int slot = find_lru_slot(cache);

    if (!cache->tiles[slot].valid) {
        cache->count++;
    }

    cache->tiles[slot].key = *key;
    cache->tiles[slot].valid = true;
    cache->tiles[slot].last_used = ++cache->access_counter;

    size_t tile_pixels = MB_INTERACTIVE_TILE_SIZE * MB_INTERACTIVE_TILE_SIZE;
    memcpy(cache->tiles[slot].pixels, pixels, tile_pixels * sizeof(PixelColor));

    pthread_mutex_unlock(mutex);
    return 0;
}

void tile_cache_new_generation(TileCache *cache) {
    pthread_mutex_t *mutex = (pthread_mutex_t*)cache->mutex;
    pthread_mutex_lock(mutex);

    cache->current_generation++;

    pthread_mutex_unlock(mutex);
}

uint32_t tile_cache_get_generation(const TileCache *cache) {
    pthread_mutex_t *mutex = (pthread_mutex_t*)cache->mutex;
    pthread_mutex_lock(mutex);
    uint32_t gen = cache->current_generation;
    pthread_mutex_unlock(mutex);
    return gen;
}

void tile_cache_clear(TileCache *cache) {
    pthread_mutex_t *mutex = (pthread_mutex_t*)cache->mutex;
    pthread_mutex_lock(mutex);

    for (int i = 0; i < cache->capacity; i++) {
        cache->tiles[i].valid = false;
    }
    cache->count = 0;
    cache->current_generation++;

    pthread_mutex_unlock(mutex);
}

void tile_cache_cleanup(TileCache *cache) {
    if (cache->mutex) {
        pthread_mutex_t *mutex = (pthread_mutex_t*)cache->mutex;
        pthread_mutex_destroy(mutex);
        free(mutex);
        cache->mutex = NULL;
    }

    if (cache->tiles) {
        for (int i = 0; i < cache->capacity; i++) {
            if (cache->tiles[i].pixels) {
                free(cache->tiles[i].pixels);
            }
        }
        free(cache->tiles);
        cache->tiles = NULL;
    }

    cache->count = 0;
    cache->capacity = 0;
}

void tile_cache_stats(const TileCache *cache, uint64_t *hits, uint64_t *misses) {
    (void)cache;
    if (hits) *hits = cache_hits;
    if (misses) *misses = cache_misses;
}
