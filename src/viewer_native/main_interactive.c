#include "native_viewer.h"
#include <stdio.h>

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

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

    if (native_viewer_init("Mandelbrot Explorer", 1280, 800) != 0) {
        fprintf(stderr, "Failed to initialize viewer\n");
        return 1;
    }

    native_viewer_run();
    native_viewer_shutdown();

    return 0;
}
