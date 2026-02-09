#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define QOI_IMPLEMENTATION
#include "qoi.h"

#include "config.h"
#include "render/render.h"

int main(int argc, char **argv) {
    const char *output_file = "mandelbrot.qoi";
    if (argc > 1) {
        output_file = argv[1];
    }

    char cwd[1024];
    if (getcwd(cwd, sizeof(cwd)) != NULL) {
        printf("Current working dir: %s\n", cwd);
    }

    // Allocate image buffer
    size_t total_pixels = (size_t)MB_WIDTH * MB_HEIGHT;
    PixelColor *buffer = malloc(total_pixels * sizeof(PixelColor));
    if (!buffer) {
        fprintf(stderr, "Error: Could not allocate image buffer\n");
        return 1;
    }

    // Initialize renderer
    RenderContext ctx;
    if (render_init(&ctx, buffer, 0) != 0) {
        fprintf(stderr, "Error: Could not initialize renderer\n");
        free(buffer);
        return 1;
    }

    printf("Computing fractal with %d threads (%d tiles of %dx%d)...\n",
           ctx.num_threads, MB_TOTAL_TILES, MB_TILE_SIZE, MB_TILE_SIZE);
    render_print_simd_status();

    // Render the fractal
    render_execute(&ctx);

    // Clean up renderer
    render_cleanup(&ctx);

    // Write output file
    printf("Writing QOI file...\n");

    qoi_desc desc = {
        .width = MB_WIDTH,
        .height = MB_HEIGHT,
        .channels = 3,
        .colorspace = QOI_SRGB
    };

    int result = qoi_write(output_file, buffer, &desc);
    free(buffer);

    if (!result) {
        fprintf(stderr, "Error: Failed to write QOI file\n");
        return 1;
    }

    printf("Mandelbrot set image saved to %s\n", output_file);
    return 0;
}
