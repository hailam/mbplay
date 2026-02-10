#ifndef MB_TILE_MAP_H
#define MB_TILE_MAP_H

#include <stdint.h>

// =============================================================================
// Map-style Tile Coordinate System
// =============================================================================
//
// Coordinate system similar to web map tiles (z/x/y):
//   Zoom level Z -> 2^Z x 2^Z tiles, each 256x256 pixels
//
// Complex plane bounds (matching default Mandelbrot view):
//   Real:      [-2.5, 1.0]  width = 3.5
//   Imaginary: [-1.5, 1.5]  height = 3.0
//
// Max zoom: 60 (double precision limit), 70+ with perturbation

#define MB_MAP_TILE_SIZE 256
#define MB_REAL_MIN -2.5
#define MB_REAL_MAX 1.0
#define MB_REAL_WIDTH 3.5
#define MB_IMAG_MIN -1.5
#define MB_IMAG_MAX 1.5
#define MB_IMAG_HEIGHT 3.0
#define MB_MAX_ZOOM 60

/**
 * Map tile identifier using z/x/y coordinates.
 */
typedef struct {
    int32_t zoom;   // Zoom level (0 = single tile, higher = more tiles)
    uint64_t x, y;  // Tile coordinates at this zoom level
} MapTile;

/**
 * Convert complex plane coordinates to map tile.
 * @param cx Complex real coordinate
 * @param cy Complex imaginary coordinate
 * @param zoom Zoom level
 * @return MapTile containing the point
 */
MapTile mb_complex_to_tile(double cx, double cy, int zoom);

/**
 * Get the complex plane bounds for a map tile.
 * @param tile The map tile
 * @param min_cx Output: minimum real coordinate
 * @param max_cx Output: maximum real coordinate
 * @param min_cy Output: minimum imaginary coordinate
 * @param max_cy Output: maximum imaginary coordinate
 */
void mb_tile_to_bounds(const MapTile *tile, double *min_cx, double *max_cx,
                       double *min_cy, double *max_cy);

/**
 * Get the center of a map tile in complex coordinates.
 * @param tile The map tile
 * @param cx Output: center real coordinate
 * @param cy Output: center imaginary coordinate
 */
void mb_tile_center(const MapTile *tile, double *cx, double *cy);

/**
 * Convert pixel within a tile to complex coordinates.
 * @param tile The map tile
 * @param px Pixel X within tile (0 to MB_TILE_SIZE-1)
 * @param py Pixel Y within tile (0 to MB_TILE_SIZE-1)
 * @param cx Output: complex real coordinate
 * @param cy Output: complex imaginary coordinate
 */
void mb_tile_pixel_to_complex(const MapTile *tile, int px, int py,
                              double *cx, double *cy);

/**
 * Get list of tiles visible in a viewport.
 * @param center_x Viewport center real coordinate
 * @param center_y Viewport center imaginary coordinate
 * @param zoom Zoom level
 * @param viewport_w Viewport width in pixels
 * @param viewport_h Viewport height in pixels
 * @param out_tiles Output array for visible tiles
 * @param max_tiles Maximum tiles to return
 * @return Number of tiles written to out_tiles
 */
int mb_get_visible_tiles(double center_x, double center_y, int zoom,
                         int viewport_w, int viewport_h,
                         MapTile *out_tiles, int max_tiles);

/**
 * Get the scale (complex units per pixel) for a zoom level.
 * @param zoom Zoom level
 * @return Scale in complex units per pixel
 */
double mb_tile_zoom_to_scale(int zoom);

/**
 * Convert a scale to the closest zoom level.
 * @param scale Complex units per pixel
 * @return Closest zoom level
 */
int mb_scale_to_tile_zoom(double scale);

#endif // MB_TILE_MAP_H
