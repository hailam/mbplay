#include "viewer.h"
#include <SDL.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

// =============================================================================
// Viewer State
// =============================================================================

static SDL_Window *window = NULL;
static SDL_Renderer *renderer = NULL;
static SDL_Texture *texture = NULL;

static PixelColor *display_buffer = NULL;
static int display_width = 0;
static int display_height = 0;
static int full_width = 0;
static int full_height = 0;
static float scale_factor = 1.0f;

static SDL_mutex *buffer_mutex = NULL;
static bool running = false;

// Maximum display dimensions
#define MAX_DISPLAY_WIDTH 1920
#define MAX_DISPLAY_HEIGHT 1080

// =============================================================================
// Viewer API Implementation
// =============================================================================

int viewer_init(const char *title, int fw, int fh) {
    full_width = fw;
    full_height = fh;

    // Calculate scale to fit in display bounds
    float scale_w = (float)MAX_DISPLAY_WIDTH / fw;
    float scale_h = (float)MAX_DISPLAY_HEIGHT / fh;
    float scale = fminf(scale_w, scale_h);
    if (scale > 1.0f) scale = 1.0f;  // Don't upscale

    scale_factor = 1.0f / scale;  // Inverse for full->display mapping
    display_width = (int)(fw * scale);
    display_height = (int)(fh * scale);

    // Initialize SDL
    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        fprintf(stderr, "SDL init failed: %s\n", SDL_GetError());
        return -1;
    }

    // Create window
    window = SDL_CreateWindow(
        title,
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        display_width, display_height,
        SDL_WINDOW_SHOWN | SDL_WINDOW_ALLOW_HIGHDPI
    );
    if (!window) {
        fprintf(stderr, "Window creation failed: %s\n", SDL_GetError());
        SDL_Quit();
        return -1;
    }

    // Create renderer (hardware accelerated)
    renderer = SDL_CreateRenderer(window, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!renderer) {
        fprintf(stderr, "Renderer creation failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return -1;
    }

    // Create streaming texture
    texture = SDL_CreateTexture(renderer,
        SDL_PIXELFORMAT_RGB24,
        SDL_TEXTUREACCESS_STREAMING,
        display_width, display_height);
    if (!texture) {
        fprintf(stderr, "Texture creation failed: %s\n", SDL_GetError());
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return -1;
    }

    // Allocate display buffer (initialized to black)
    display_buffer = calloc((size_t)display_width * display_height, sizeof(PixelColor));
    if (!display_buffer) {
        SDL_DestroyTexture(texture);
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return -1;
    }

    // Create mutex for thread-safe updates
    buffer_mutex = SDL_CreateMutex();
    if (!buffer_mutex) {
        free(display_buffer);
        SDL_DestroyTexture(texture);
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return -1;
    }

    running = true;
    return 0;
}

void viewer_update_row(int y, const PixelColor *row, int width) {
    if (!running || !display_buffer) return;

    // Map full-res Y to display Y
    int dy = (int)(y / scale_factor);
    if (dy >= display_height) return;

    SDL_LockMutex(buffer_mutex);

    // Downsample row into display buffer
    for (int x = 0; x < width; x++) {
        int dx = (int)(x / scale_factor);
        if (dx >= display_width) continue;

        // Simple point sampling (could use area averaging for quality)
        display_buffer[dy * display_width + dx] = row[x];
    }

    SDL_UnlockMutex(buffer_mutex);
}

bool viewer_present(void) {
    if (!running) return false;

    // Process events
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        switch (event.type) {
            case SDL_QUIT:
                running = false;
                return false;
            case SDL_KEYDOWN:
                if (event.key.keysym.sym == SDLK_ESCAPE ||
                    event.key.keysym.sym == SDLK_q) {
                    running = false;
                    return false;
                }
                break;
        }
    }

    // Update texture from display buffer
    SDL_LockMutex(buffer_mutex);
    SDL_UpdateTexture(texture, NULL, display_buffer,
                      display_width * sizeof(PixelColor));
    SDL_UnlockMutex(buffer_mutex);

    // Render
    SDL_RenderClear(renderer);
    SDL_RenderCopy(renderer, texture, NULL, NULL);
    SDL_RenderPresent(renderer);

    return true;
}

bool viewer_is_running(void) {
    return running;
}

float viewer_get_scale(void) {
    return scale_factor;
}

void viewer_shutdown(void) {
    running = false;

    if (buffer_mutex) {
        SDL_DestroyMutex(buffer_mutex);
        buffer_mutex = NULL;
    }
    if (display_buffer) {
        free(display_buffer);
        display_buffer = NULL;
    }
    if (texture) {
        SDL_DestroyTexture(texture);
        texture = NULL;
    }
    if (renderer) {
        SDL_DestroyRenderer(renderer);
        renderer = NULL;
    }
    if (window) {
        SDL_DestroyWindow(window);
        window = NULL;
    }
    SDL_Quit();
}
