#include "tile_map.h"
#include <math.h>

// =============================================================================
// Tile Coordinate Conversions
// =============================================================================

MapTile mb_complex_to_tile(double cx, double cy, int zoom) {
    MapTile tile;
    tile.zoom = zoom;

    // Number of tiles at this zoom level
    double num_tiles = pow(2.0, zoom);

    // Tile width and height in complex plane
    double tile_width = MB_REAL_WIDTH / num_tiles;
    double tile_height = MB_IMAG_HEIGHT / num_tiles;

    // Convert complex coordinates to tile coordinates
    // x = floor((cx - MB_REAL_MIN) / tile_width)
    // y = floor((cy - MB_IMAG_MIN) / tile_height)
    double x = (cx - MB_REAL_MIN) / tile_width;
    double y = (cy - MB_IMAG_MIN) / tile_height;

    // Clamp to valid range
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x >= num_tiles) x = num_tiles - 1;
    if (y >= num_tiles) y = num_tiles - 1;

    tile.x = (uint64_t)floor(x);
    tile.y = (uint64_t)floor(y);

    return tile;
}

void mb_tile_to_bounds(const MapTile *tile, double *min_cx, double *max_cx,
                       double *min_cy, double *max_cy) {
    // Number of tiles at this zoom level
    double num_tiles = pow(2.0, tile->zoom);

    // Tile width and height in complex plane
    double tile_width = MB_REAL_WIDTH / num_tiles;
    double tile_height = MB_IMAG_HEIGHT / num_tiles;

    *min_cx = MB_REAL_MIN + tile->x * tile_width;
    *max_cx = *min_cx + tile_width;
    *min_cy = MB_IMAG_MIN + tile->y * tile_height;
    *max_cy = *min_cy + tile_height;
}

void mb_tile_center(const MapTile *tile, double *cx, double *cy) {
    double min_cx, max_cx, min_cy, max_cy;
    mb_tile_to_bounds(tile, &min_cx, &max_cx, &min_cy, &max_cy);
    *cx = (min_cx + max_cx) / 2.0;
    *cy = (min_cy + max_cy) / 2.0;
}

void mb_tile_pixel_to_complex(const MapTile *tile, int px, int py,
                              double *cx, double *cy) {
    double min_cx, max_cx, min_cy, max_cy;
    mb_tile_to_bounds(tile, &min_cx, &max_cx, &min_cy, &max_cy);

    // Linear interpolation within tile
    double tile_width = max_cx - min_cx;
    double tile_height = max_cy - min_cy;

    *cx = min_cx + (px + 0.5) * tile_width / MB_MAP_TILE_SIZE;
    *cy = min_cy + (py + 0.5) * tile_height / MB_MAP_TILE_SIZE;
}

int mb_get_visible_tiles(double center_x, double center_y, int zoom,
                         int viewport_w, int viewport_h,
                         MapTile *out_tiles, int max_tiles) {
    if (max_tiles <= 0 || !out_tiles) return 0;

    // Get scale for this zoom level
    double scale = mb_tile_zoom_to_scale(zoom);

    // Calculate viewport bounds in complex plane
    double half_w = (viewport_w / 2.0) * scale;
    double half_h = (viewport_h / 2.0) * scale;

    double view_min_cx = center_x - half_w;
    double view_max_cx = center_x + half_w;
    double view_min_cy = center_y - half_h;
    double view_max_cy = center_y + half_h;

    // Get corner tiles
    MapTile top_left = mb_complex_to_tile(view_min_cx, view_min_cy, zoom);
    MapTile bottom_right = mb_complex_to_tile(view_max_cx, view_max_cy, zoom);

    // Expand by 1 tile for smooth scrolling
    uint64_t start_x = top_left.x > 0 ? top_left.x - 1 : 0;
    uint64_t start_y = top_left.y > 0 ? top_left.y - 1 : 0;
    uint64_t end_x = bottom_right.x + 2;
    uint64_t end_y = bottom_right.y + 2;

    // Clamp to valid range
    uint64_t max_tile = (uint64_t)1 << zoom;
    if (end_x > max_tile) end_x = max_tile;
    if (end_y > max_tile) end_y = max_tile;

    // Collect tiles
    int count = 0;
    for (uint64_t y = start_y; y < end_y && count < max_tiles; y++) {
        for (uint64_t x = start_x; x < end_x && count < max_tiles; x++) {
            out_tiles[count].zoom = zoom;
            out_tiles[count].x = x;
            out_tiles[count].y = y;
            count++;
        }
    }

    return count;
}

double mb_tile_zoom_to_scale(int zoom) {
    // At zoom 0, one tile covers the full complex plane
    // Tile size is MB_MAP_TILE_SIZE pixels
    // So scale at zoom 0 = MB_REAL_WIDTH / MB_MAP_TILE_SIZE
    // At zoom Z, scale = (MB_REAL_WIDTH / MB_MAP_TILE_SIZE) / 2^Z
    double num_tiles = pow(2.0, zoom);
    return MB_REAL_WIDTH / (MB_MAP_TILE_SIZE * num_tiles);
}

int mb_scale_to_tile_zoom(double scale) {
    // Inverse of mb_tile_zoom_to_scale:
    // scale = MB_REAL_WIDTH / (MB_MAP_TILE_SIZE * 2^Z)
    // 2^Z = MB_REAL_WIDTH / (MB_MAP_TILE_SIZE * scale)
    // Z = log2(MB_REAL_WIDTH / (MB_MAP_TILE_SIZE * scale))

    if (scale <= 0) return 0;

    double ratio = MB_REAL_WIDTH / (MB_MAP_TILE_SIZE * scale);
    if (ratio < 1) return 0;

    int zoom = (int)round(log2(ratio));

    // Clamp to valid range
    if (zoom < 0) zoom = 0;
    if (zoom > MB_MAX_ZOOM) zoom = MB_MAX_ZOOM;

    return zoom;
}
