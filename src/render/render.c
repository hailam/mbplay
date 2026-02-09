#include "render.h"
#include "../mandelbrot/mandelbrot.h"
#include "../color/color.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

// =============================================================================
// Internal State
// =============================================================================

// Atomic counter for tile-based work stealing
static atomic_int next_tile = 0;

typedef struct {
    PixelColor *image_data;
} ThreadArgs;

// Thread pool storage
static pthread_t *threads = NULL;
static ThreadArgs *thread_args = NULL;

// =============================================================================
// Tile Rendering Thread
// =============================================================================

static void *render_thread(void *arg) {
    ThreadArgs *args = (ThreadArgs *)arg;

    int tile_index;
    while ((tile_index = atomic_fetch_add(&next_tile, 1)) < MB_TOTAL_TILES) {
        int tile_x = tile_index % MB_NUM_TILES_X;
        int tile_y = tile_index / MB_NUM_TILES_X;

        int start_x = tile_x * MB_TILE_SIZE;
        int start_y = tile_y * MB_TILE_SIZE;
        int end_x = start_x + MB_TILE_SIZE;
        int end_y = start_y + MB_TILE_SIZE;

        // Clamp to image bounds
        if (end_x > MB_WIDTH)
            end_x = MB_WIDTH;
        if (end_y > MB_HEIGHT)
            end_y = MB_HEIGHT;

        for (int y = start_y; y < end_y; y++) {
            double cy = (y - MB_HEIGHT_HALF) * MB_CY_SCALE;
            int x = start_x;

#if defined(MB_SIMD_NEON) || defined(MB_SIMD_AVX2) || defined(MB_SIMD_SSE2)
            // Process pairs of pixels with SIMD
            for (; x + 1 < end_x; x += 2) {
                double cx0 = (x - MB_WIDTH_HALF) * MB_CX_SCALE;
                double cx1 = (x + 1 - MB_WIDTH_HALF) * MB_CX_SCALE;

                unsigned int iter0, iter1;
                mb_compute_iteration_simd2(cx0, cy, cx1, cy, &iter0, &iter1);

                int idx0 = y * MB_WIDTH + x;
                int idx1 = y * MB_WIDTH + x + 1;

                color_from_iteration(&args->image_data[idx0], iter0);
                color_from_iteration(&args->image_data[idx1], iter1);
            }
#endif

            // Handle remaining pixels with scalar code
            for (; x < end_x; x++) {
                double cx = (x - MB_WIDTH_HALF) * MB_CX_SCALE;
                unsigned int iteration = mb_compute_iteration_scalar(cx, cy);

                int pixel_index = y * MB_WIDTH + x;
                color_from_iteration(&args->image_data[pixel_index], iteration);
            }
        }
    }

    return NULL;
}

// =============================================================================
// Public API
// =============================================================================

int render_init(RenderContext *ctx, PixelColor *image_data, int num_threads) {
    if (num_threads <= 0) {
        num_threads = (int)sysconf(_SC_NPROCESSORS_ONLN);
        if (num_threads < 1)
            num_threads = 4; // Fallback
    }

    ctx->image_data = image_data;
    ctx->num_threads = num_threads;

    threads = malloc((size_t)num_threads * sizeof(pthread_t));
    thread_args = malloc((size_t)num_threads * sizeof(ThreadArgs));

    if (!threads || !thread_args) {
        free(threads);
        free(thread_args);
        threads = NULL;
        thread_args = NULL;
        return -1;
    }

    return 0;
}

void render_execute(RenderContext *ctx) {
    // Reset atomic counter
    atomic_store(&next_tile, 0);

    // Start worker threads
    for (int i = 0; i < ctx->num_threads; i++) {
        thread_args[i].image_data = ctx->image_data;
        pthread_create(&threads[i], NULL, render_thread, &thread_args[i]);
    }

    // Wait for all threads to complete
    for (int i = 0; i < ctx->num_threads; i++) {
        pthread_join(threads[i], NULL);
    }
}

void render_cleanup(RenderContext *ctx) {
    (void)ctx; // Unused for now
    free(threads);
    free(thread_args);
    threads = NULL;
    thread_args = NULL;
}

void render_print_simd_status(void) {
#if defined(MB_SIMD_NEON)
    printf("SIMD: ARM NEON enabled\n");
#elif defined(MB_SIMD_AVX2)
    printf("SIMD: x86 AVX2 enabled\n");
#elif defined(MB_SIMD_SSE2)
    printf("SIMD: x86 SSE2 enabled\n");
#else
    printf("SIMD: None (scalar fallback)\n");
#endif
}
