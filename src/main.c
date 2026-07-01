#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "config.h"
#include "gpu/gpu.h"
#include "stream/stream.h"
#include "viewer/viewer.h"
#include "mandelbrot/mandelbrot.h"
#include "color/color.h"

// =============================================================================
// CPU Row Compute (fallback when GPU not available)
// =============================================================================

static void cpu_compute_row(const MBConfig *cfg, int y, PixelColor *output) {
    double cy = (y - cfg->height_half) * cfg->cy_scale;

    for (int x = 0; x < cfg->width; x++) {
        double cx = (x - cfg->width_half) * cfg->cx_scale;
        unsigned int iteration = mb_compute_point(cx, cy, (unsigned int)cfg->max_iter);
        color_from_iteration_classic(&output[x], iteration, (unsigned int)cfg->max_iter);
    }
}

// =============================================================================
// Usage
// =============================================================================

static void print_usage(const char *progname) {
    fprintf(stderr, "Usage: %s [options]\n", progname);
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "  -w WIDTH    Image width (default: %d)\n", MB_DEFAULT_WIDTH);
    fprintf(stderr, "  -h HEIGHT   Image height (default: %d)\n", MB_DEFAULT_HEIGHT);
    fprintf(stderr, "  -i ITER     Max iterations (default: %d)\n", MB_DEFAULT_MAX_ITER);
    fprintf(stderr, "  -o FILE     Output file (default: mandelbrot.png)\n");
    fprintf(stderr, "  -n          No viewer (headless mode)\n");
    fprintf(stderr, "  -c          Force CPU rendering (no GPU)\n");
    fprintf(stderr, "  --help      Show this help\n");
}

// =============================================================================
// Main
// =============================================================================

int main(int argc, char **argv) {
    // Default configuration
    int width = MB_DEFAULT_WIDTH;
    int height = MB_DEFAULT_HEIGHT;
    int max_iter = MB_DEFAULT_MAX_ITER;
    const char *output_file = "mandelbrot.png";
    bool show_viewer = true;
    bool force_cpu = false;

    // Parse arguments
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-w") == 0 && i + 1 < argc) {
            width = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-h") == 0 && i + 1 < argc) {
            height = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-i") == 0 && i + 1 < argc) {
            max_iter = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            output_file = argv[++i];
        } else if (strcmp(argv[i], "-n") == 0) {
            show_viewer = false;
        } else if (strcmp(argv[i], "-c") == 0) {
            force_cpu = true;
        } else if (strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        }
    }

    // Validate dimensions
    if (width <= 0 || height <= 0 || max_iter <= 0) {
        fprintf(stderr, "Error: Invalid dimensions or iteration count\n");
        return 1;
    }

    // Initialize runtime config
    MBConfig cfg;
    mb_config_init(&cfg, width, height, max_iter);

    printf("Mandelbrot Generator\n");
    printf("  Image: %d x %d\n", width, height);
    printf("  Max iterations: %d\n", max_iter);
    printf("  Output: %s\n", output_file);

    // Check GPU availability
    bool use_gpu = !force_cpu && gpu_is_available();
    if (use_gpu) {
        if (gpu_init(&cfg) != 0) {
            printf("  GPU init failed, falling back to CPU\n");
            use_gpu = false;
        } else {
            printf("  Compute: Metal GPU\n");
        }
    } else {
        printf("  Compute: CPU%s\n", force_cpu ? " (forced)" : "");
    }

    // Initialize viewer (optional)
    if (show_viewer) {
        if (viewer_init("Mandelbrot", width, height) != 0) {
            fprintf(stderr, "Warning: Could not initialize viewer, running headless\n");
            show_viewer = false;
        }
    }

    // Initialize streaming output
    StreamContext *stream = stream_init(output_file, width, height);
    if (!stream) {
        fprintf(stderr, "Error: Could not open output file: %s\n", output_file);
        if (show_viewer) viewer_shutdown();
        if (use_gpu) gpu_cleanup();
        return 1;
    }

    // Allocate single row buffer (bounded memory!)
    PixelColor *row = malloc((size_t)width * sizeof(PixelColor));
    if (!row) {
        fprintf(stderr, "Error: Could not allocate row buffer\n");
        stream_finish(stream);
        if (show_viewer) viewer_shutdown();
        if (use_gpu) gpu_cleanup();
        return 1;
    }

    printf("Computing...\n");

    // Process row by row (streaming)
    int last_progress = -1;
    for (int y = 0; y < height; y++) {
        // Check if viewer was closed
        if (show_viewer && !viewer_is_running()) {
            printf("\nViewer closed, aborting...\n");
            break;
        }

        // Compute row
        if (use_gpu) {
            gpu_compute_row(y, row);
        } else {
            cpu_compute_row(&cfg, y, row);
        }

        // Write to file immediately
        if (!stream_write_row(stream, row)) {
            fprintf(stderr, "Error: Failed to write row %d\n", y);
            break;
        }

        // Update viewer periodically
        if (show_viewer) {
            viewer_update_row(y, row, width);
            // Present every 10 rows to reduce overhead
            if (y % 10 == 0) {
                viewer_present();
            }
        }

        // Progress indicator (64-bit product: y*100 can overflow int for
        // very tall images)
        int progress = (int)(((int64_t)y * 100) / height);
        if (progress != last_progress && progress % 10 == 0) {
            printf("  %d%%\n", progress);
            last_progress = progress;
        }
    }

    // Finish streaming
    bool complete = stream_is_complete(stream);
    stream_finish(stream);
    free(row);

    if (use_gpu) {
        gpu_cleanup();
    }

    if (complete) {
        printf("Saved %dx%d image to %s\n", width, height, output_file);
    }

    // Keep viewer open until closed
    if (show_viewer && viewer_is_running()) {
        printf("Press Q or ESC to close viewer...\n");
        while (viewer_is_running()) {
            viewer_present();
            // Small delay to reduce CPU usage
            usleep(16000);  // ~60fps
        }
    }

    if (show_viewer) {
        viewer_shutdown();
    }

    return complete ? 0 : 1;
}
