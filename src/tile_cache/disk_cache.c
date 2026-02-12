#include "disk_cache.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>

#define QOI_IMPLEMENTATION
#define QOI_NO_STDIO
#include <qoi.h>

// =============================================================================
// Disk Cache Implementation
// =============================================================================

// LRU entry for tracking file access times
typedef struct LRUEntry {
    char path[512];
    int64_t size;
    time_t last_access;
    struct LRUEntry *next;
    struct LRUEntry *prev;
} LRUEntry;

struct DiskCache {
    char base_path[512];
    int64_t max_size;
    int64_t current_size;
    LRUEntry *lru_head;  // Most recently used
    LRUEntry *lru_tail;  // Least recently used
    pthread_mutex_t mutex;
};

// =============================================================================
// Helper Functions
// =============================================================================

static int ensure_directory(const char *path) {
    struct stat st;
    if (stat(path, &st) == 0) {
        return S_ISDIR(st.st_mode) ? 0 : -1;
    }

    // Create directory with parents
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s", path);
    size_t len = strlen(tmp);

    for (size_t i = 1; i < len; i++) {
        if (tmp[i] == '/') {
            tmp[i] = '\0';
            if (stat(tmp, &st) != 0) {
                if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
                    return -1;
                }
            }
            tmp[i] = '/';
        }
    }

    if (stat(path, &st) != 0) {
        if (mkdir(path, 0755) != 0 && errno != EEXIST) {
            return -1;
        }
    }

    return 0;
}

static void get_tile_path(const DiskCache *cache, const MapTile *tile, char *path, size_t path_size) {
    snprintf(path, path_size, "%s/%d/%llu/%llu.qoi",
             cache->base_path, tile->zoom,
             (unsigned long long)tile->x,
             (unsigned long long)tile->y);
}

static void get_tile_dir(const DiskCache *cache, const MapTile *tile, char *path, size_t path_size) {
    snprintf(path, path_size, "%s/%d/%llu",
             cache->base_path, tile->zoom,
             (unsigned long long)tile->x);
}

// Convert PixelColor (RGB) to RGBA for QOI
static void rgb_to_rgba(const PixelColor *src, unsigned char *dst, size_t count) {
    for (size_t i = 0; i < count; i++) {
        dst[i * 4 + 0] = src[i].r;
        dst[i * 4 + 1] = src[i].g;
        dst[i * 4 + 2] = src[i].b;
        dst[i * 4 + 3] = 255;
    }
}

// Convert RGBA to PixelColor (RGB)
static void rgba_to_rgb(const unsigned char *src, PixelColor *dst, size_t count) {
    for (size_t i = 0; i < count; i++) {
        dst[i].r = src[i * 4 + 0];
        dst[i].g = src[i * 4 + 1];
        dst[i].b = src[i * 4 + 2];
    }
}

// =============================================================================
// LRU Management
// =============================================================================

static LRUEntry *lru_find(DiskCache *cache, const char *path) {
    LRUEntry *entry = cache->lru_head;
    while (entry) {
        if (strcmp(entry->path, path) == 0) {
            return entry;
        }
        entry = entry->next;
    }
    return NULL;
}

static void lru_remove(DiskCache *cache, LRUEntry *entry) {
    if (entry->prev) {
        entry->prev->next = entry->next;
    } else {
        cache->lru_head = entry->next;
    }

    if (entry->next) {
        entry->next->prev = entry->prev;
    } else {
        cache->lru_tail = entry->prev;
    }
}

static void lru_add_front(DiskCache *cache, LRUEntry *entry) {
    entry->prev = NULL;
    entry->next = cache->lru_head;

    if (cache->lru_head) {
        cache->lru_head->prev = entry;
    } else {
        cache->lru_tail = entry;
    }

    cache->lru_head = entry;
}

static void lru_touch(DiskCache *cache, const char *path) {
    LRUEntry *entry = lru_find(cache, path);
    if (entry) {
        // Move to front
        lru_remove(cache, entry);
        lru_add_front(cache, entry);
        entry->last_access = time(NULL);
    }
}

static void lru_evict_oldest(DiskCache *cache) {
    while (cache->current_size > cache->max_size && cache->lru_tail) {
        LRUEntry *oldest = cache->lru_tail;

        // Remove file
        unlink(oldest->path);

        // Update size
        cache->current_size -= oldest->size;

        // Remove from list
        lru_remove(cache, oldest);
        free(oldest);
    }
}

// =============================================================================
// Public API
// =============================================================================

DiskCache *disk_cache_init(const char *base_path, int64_t max_size_bytes) {
    DiskCache *cache = (DiskCache *)calloc(1, sizeof(DiskCache));
    if (!cache) return NULL;

    strncpy(cache->base_path, base_path, sizeof(cache->base_path) - 1);
    cache->max_size = max_size_bytes > 0 ? max_size_bytes : MB_DISK_CACHE_DEFAULT_SIZE;
    cache->current_size = 0;
    cache->lru_head = NULL;
    cache->lru_tail = NULL;

    pthread_mutex_init(&cache->mutex, NULL);

    // Ensure base directory exists
    if (ensure_directory(base_path) != 0) {
        free(cache);
        return NULL;
    }

    return cache;
}

int disk_cache_get(DiskCache *cache, const MapTile *tile, PixelColor *pixels) {
    if (!cache || !tile || !pixels) return -1;
    if (tile->zoom > MB_DISK_CACHE_MAX_ZOOM) return -1;

    char path[512];
    get_tile_path(cache, tile, path, sizeof(path));

    pthread_mutex_lock(&cache->mutex);

    // Check if file exists
    FILE *f = fopen(path, "rb");
    if (!f) {
        pthread_mutex_unlock(&cache->mutex);
        return -1;
    }

    // Get file size with error checking
    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    if (file_size < 0 || file_size > MB_MAX_TILE_FILE_SIZE) {
        fclose(f);
        pthread_mutex_unlock(&cache->mutex);
        return -1;
    }
    fseek(f, 0, SEEK_SET);

    // Read file contents
    unsigned char *data = (unsigned char *)malloc((size_t)file_size);
    if (!data) {
        fclose(f);
        pthread_mutex_unlock(&cache->mutex);
        return -1;
    }

    if (fread(data, 1, file_size, f) != (size_t)file_size) {
        free(data);
        fclose(f);
        pthread_mutex_unlock(&cache->mutex);
        return -1;
    }
    fclose(f);

    // Decode QOI
    qoi_desc desc;
    unsigned char *rgba = qoi_decode(data, file_size, &desc, 4);
    free(data);

    if (!rgba || desc.width != MB_MAP_TILE_SIZE || desc.height != MB_MAP_TILE_SIZE) {
        if (rgba) free(rgba);
        pthread_mutex_unlock(&cache->mutex);
        return -1;
    }

    // Convert to RGB
    rgba_to_rgb(rgba, pixels, MB_MAP_TILE_SIZE * MB_MAP_TILE_SIZE);
    free(rgba);

    // Touch LRU
    lru_touch(cache, path);

    pthread_mutex_unlock(&cache->mutex);
    return 0;
}

int disk_cache_put(DiskCache *cache, const MapTile *tile, const PixelColor *pixels) {
    if (!cache || !tile || !pixels) return -1;
    if (tile->zoom > MB_DISK_CACHE_MAX_ZOOM) return -1;

    char path[512];
    char dir[512];
    get_tile_path(cache, tile, path, sizeof(path));
    get_tile_dir(cache, tile, dir, sizeof(dir));

    pthread_mutex_lock(&cache->mutex);

    // Ensure directory exists
    if (ensure_directory(dir) != 0) {
        pthread_mutex_unlock(&cache->mutex);
        return -1;
    }

    // Convert to RGBA
    size_t pixel_count = MB_MAP_TILE_SIZE * MB_MAP_TILE_SIZE;
    unsigned char *rgba = (unsigned char *)malloc(pixel_count * 4);
    if (!rgba) {
        pthread_mutex_unlock(&cache->mutex);
        return -1;
    }

    rgb_to_rgba(pixels, rgba, pixel_count);

    // Encode to QOI
    qoi_desc desc = {
        .width = MB_MAP_TILE_SIZE,
        .height = MB_MAP_TILE_SIZE,
        .channels = 4,
        .colorspace = QOI_SRGB
    };

    int encoded_size;
    unsigned char *encoded = qoi_encode(rgba, &desc, &encoded_size);
    free(rgba);

    if (!encoded) {
        pthread_mutex_unlock(&cache->mutex);
        return -1;
    }

    // Write to file
    FILE *f = fopen(path, "wb");
    if (!f) {
        free(encoded);
        pthread_mutex_unlock(&cache->mutex);
        return -1;
    }

    size_t written = fwrite(encoded, 1, encoded_size, f);
    fclose(f);
    free(encoded);

    if (written != (size_t)encoded_size) {
        unlink(path);
        pthread_mutex_unlock(&cache->mutex);
        return -1;
    }

    // Update LRU
    LRUEntry *entry = lru_find(cache, path);
    if (entry) {
        // Update existing entry
        cache->current_size -= entry->size;
        entry->size = encoded_size;
        cache->current_size += encoded_size;
        lru_remove(cache, entry);
        lru_add_front(cache, entry);
    } else {
        // Create new entry
        entry = (LRUEntry *)calloc(1, sizeof(LRUEntry));
        if (!entry) {
            // Allocation failed - file was written but won't be tracked in LRU
            // This is acceptable; the file will still be accessible via disk
            pthread_mutex_unlock(&cache->mutex);
            return 0;  // Still return success since file was written
        }
        strncpy(entry->path, path, sizeof(entry->path) - 1);
        entry->size = encoded_size;
        entry->last_access = time(NULL);
        cache->current_size += encoded_size;
        lru_add_front(cache, entry);
    }

    // Evict if necessary
    lru_evict_oldest(cache);

    pthread_mutex_unlock(&cache->mutex);
    return 0;
}

int disk_cache_exists(DiskCache *cache, const MapTile *tile) {
    if (!cache || !tile) return 0;
    if (tile->zoom > MB_DISK_CACHE_MAX_ZOOM) return 0;

    char path[512];
    get_tile_path(cache, tile, path, sizeof(path));

    struct stat st;
    return stat(path, &st) == 0 ? 1 : 0;
}

int64_t disk_cache_size(const DiskCache *cache) {
    if (!cache) return 0;
    return cache->current_size;
}

void disk_cache_clear(DiskCache *cache) {
    if (!cache) return;

    pthread_mutex_lock(&cache->mutex);

    // Free LRU entries and delete files
    LRUEntry *entry = cache->lru_head;
    while (entry) {
        LRUEntry *next = entry->next;
        unlink(entry->path);
        free(entry);
        entry = next;
    }

    cache->lru_head = NULL;
    cache->lru_tail = NULL;
    cache->current_size = 0;

    pthread_mutex_unlock(&cache->mutex);
}

void disk_cache_cleanup(DiskCache *cache) {
    if (!cache) return;

    pthread_mutex_lock(&cache->mutex);

    // Free LRU entries (don't delete files)
    LRUEntry *entry = cache->lru_head;
    while (entry) {
        LRUEntry *next = entry->next;
        free(entry);
        entry = next;
    }

    pthread_mutex_unlock(&cache->mutex);
    pthread_mutex_destroy(&cache->mutex);

    free(cache);
}
