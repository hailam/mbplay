#include "native_viewer.h"
#include <stdio.h>
#include <stdbool.h>
#include <string.h>

int main(int argc, char *argv[]) {
    // Parse command-line arguments
    bool clear_cache = false;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--clear-cache") == 0 || strcmp(argv[i], "-c") == 0) {
            clear_cache = true;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("Usage: %s [options]\n", argv[0]);
            printf("Options:\n");
            printf("  --clear-cache, -c  Clear tile cache on startup\n");
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
    printf("  Esc: Quit\n");
    printf("\n");

    if (native_viewer_init("Mandelbrot Explorer", 1280, 800, clear_cache) != 0) {
        fprintf(stderr, "Failed to initialize viewer\n");
        return 1;
    }

    native_viewer_run();
    native_viewer_shutdown();

    return 0;
}
