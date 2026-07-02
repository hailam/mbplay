#include "native_viewer.h"
#include <stdio.h>
#include <stdbool.h>
#include <string.h>

#include <stdlib.h>

int main(int argc, char *argv[]) {
    // Parse command-line arguments
    bool clear_cache = false;
    bool cinematic = false;
    double cine_speed = 0.5;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--clear-cache") == 0 || strcmp(argv[i], "-c") == 0) {
            clear_cache = true;
        } else if (strcmp(argv[i], "--cinematic") == 0 || strcmp(argv[i], "-C") == 0) {
            cinematic = true;
        } else if (strcmp(argv[i], "--speed") == 0 && i + 1 < argc) {
            cine_speed = atof(argv[++i]);
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("Usage: %s [options]\n", argv[0]);
            printf("Options:\n");
            printf("  --clear-cache, -c  Clear tile cache on startup\n");
            printf("  --cinematic, -C    Start the autopilot zoom (V toggles in-app)\n");
            printf("  --speed S          Cinematic zoom speed in decades/second (default 0.5)\n");
            printf("  --help, -h         Show this help message\n");
            return 0;
        }
    }

    printf("Mandelbrot Interactive Viewer\n");
    printf("Controls:\n");
    printf("  Pinch/Scroll: Zoom in/out\n");
    printf("  Two-finger scroll: Pan\n");
    printf("  Click: Recenter on point\n");
    printf("  Right-click: Zoom out\n");
    printf("  +/-: Zoom in/out\n");
    printf("  Arrow keys: Pan\n");
    printf("  R: Reset view\n");
    printf("  X: Clear all caches\n");
    printf("  Esc: Quit\n");
    printf("\n");

    if (native_viewer_init("Mandelbrot Explorer", 1280, 800, clear_cache) != 0) {
        fprintf(stderr, "Failed to initialize viewer\n");
        return 1;
    }

    if (cinematic) {
        native_viewer_start_cinematic(cine_speed);
    }

    native_viewer_run();
    native_viewer_shutdown();

    return 0;
}
