#include "stream.h"
#include <png.h>
#include <stdlib.h>
#include <stdio.h>
#include <setjmp.h>

// =============================================================================
// Stream Context
// =============================================================================

struct StreamContext {
    FILE *fp;
    png_structp png;
    png_infop info;
    int width;
    int height;
    int current_row;
};

// =============================================================================
// Streaming API Implementation
// =============================================================================

StreamContext *stream_init(const char *filename, int width, int height) {
    StreamContext *ctx = malloc(sizeof(StreamContext));
    if (!ctx) {
        return NULL;
    }

    ctx->width = width;
    ctx->height = height;
    ctx->current_row = 0;

    // Open output file
    ctx->fp = fopen(filename, "wb");
    if (!ctx->fp) {
        free(ctx);
        return NULL;
    }

    // Create PNG write struct
    ctx->png = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (!ctx->png) {
        fclose(ctx->fp);
        free(ctx);
        return NULL;
    }

    // Create PNG info struct
    ctx->info = png_create_info_struct(ctx->png);
    if (!ctx->info) {
        png_destroy_write_struct(&ctx->png, NULL);
        fclose(ctx->fp);
        free(ctx);
        return NULL;
    }

    // Error handling
    if (setjmp(png_jmpbuf(ctx->png))) {
        png_destroy_write_struct(&ctx->png, &ctx->info);
        fclose(ctx->fp);
        free(ctx);
        return NULL;
    }

    // Set up I/O
    png_init_io(ctx->png, ctx->fp);

    // Write header
    png_set_IHDR(ctx->png, ctx->info,
                 (png_uint_32)width, (png_uint_32)height,
                 8,                          // bit depth
                 PNG_COLOR_TYPE_RGB,         // RGB (no alpha)
                 PNG_INTERLACE_NONE,
                 PNG_COMPRESSION_TYPE_DEFAULT,
                 PNG_FILTER_TYPE_DEFAULT);

    // Use faster compression for streaming (less CPU overhead)
    png_set_compression_level(ctx->png, 1);

    png_write_info(ctx->png, ctx->info);

    return ctx;
}

bool stream_write_row(StreamContext *ctx, const PixelColor *row) {
    if (!ctx || ctx->current_row >= ctx->height) {
        return false;
    }

    // Error handling
    if (setjmp(png_jmpbuf(ctx->png))) {
        return false;
    }

    // Write row (PixelColor is already RGB packed)
    png_write_row(ctx->png, (png_const_bytep)row);
    ctx->current_row++;

    return true;
}

int stream_get_current_row(StreamContext *ctx) {
    return ctx ? ctx->current_row : 0;
}

bool stream_is_complete(StreamContext *ctx) {
    return ctx && ctx->current_row >= ctx->height;
}

void stream_finish(StreamContext *ctx) {
    if (!ctx) {
        return;
    }

    // Error handling for cleanup
    if (setjmp(png_jmpbuf(ctx->png))) {
        // Just clean up on error
        png_destroy_write_struct(&ctx->png, &ctx->info);
        fclose(ctx->fp);
        free(ctx);
        return;
    }

    // Write PNG footer
    png_write_end(ctx->png, NULL);

    // Clean up
    png_destroy_write_struct(&ctx->png, &ctx->info);
    fclose(ctx->fp);
    free(ctx);
}
