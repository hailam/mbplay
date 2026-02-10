#ifndef MB_STREAM_H
#define MB_STREAM_H

#include "../config.h"
#include <stdbool.h>

// =============================================================================
// Streaming Output API (PNG)
// =============================================================================

typedef struct StreamContext StreamContext;

/**
 * Initialize streaming output.
 * Opens file and writes PNG header.
 *
 * @param filename Output file path (should end in .png)
 * @param width Image width in pixels
 * @param height Image height in pixels
 * @return Stream context or NULL on failure
 */
StreamContext *stream_init(const char *filename, int width, int height);

/**
 * Write a single row of pixels to the output.
 * Rows must be written in order from top to bottom.
 *
 * @param ctx Stream context
 * @param row Pointer to row data (width pixels)
 * @return true on success, false on failure
 */
bool stream_write_row(StreamContext *ctx, const PixelColor *row);

/**
 * Get current row index (number of rows written so far).
 *
 * @param ctx Stream context
 * @return Current row index
 */
int stream_get_current_row(StreamContext *ctx);

/**
 * Check if stream is complete (all rows written).
 *
 * @param ctx Stream context
 * @return true if all rows have been written
 */
bool stream_is_complete(StreamContext *ctx);

/**
 * Finalize and close the stream.
 * Writes PNG footer and releases resources.
 *
 * @param ctx Stream context (freed after this call)
 */
void stream_finish(StreamContext *ctx);

#endif // MB_STREAM_H
