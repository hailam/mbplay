#include <metal_stdlib>
using namespace metal;

struct ComputeParams {
    float cx_scale;
    float cy_scale;
    float width_half;
    float height_half;
    uint max_iter;
    uint width;
    uint height;
};

static bool is_in_cardioid_or_bulb(float cx, float cy) {
    float cx_shifted = cx - 0.25f;
    float cy2 = cy * cy;
    float q = cx_shifted * cx_shifted + cy2;
    if (q * (q + cx_shifted) <= 0.25f * cy2) return true;
    float cx_plus1 = cx + 1.0f;
    if (cx_plus1 * cx_plus1 + cy2 <= 0.0625f) return true;
    return false;
}

kernel void mandelbrot_compute(
    device uint *iterations [[buffer(0)]],
    constant ComputeParams &params [[buffer(1)]],
    uint2 gid [[thread_position_in_grid]]
) {
    if (gid.x >= params.width || gid.y >= params.height) return;
    float cx = (float(gid.x) - params.width_half) * params.cx_scale;
    float cy = (float(gid.y) - params.height_half) * params.cy_scale;
    uint iteration = params.max_iter;
    if (!is_in_cardioid_or_bulb(cx, cy)) {
        float zx = 0.0f, zy = 0.0f, zx2 = 0.0f, zy2 = 0.0f;
        iteration = 0;
        while (zx2 + zy2 < 4.0f && iteration < params.max_iter) {
            zy = 2.0f * zx * zy + cy;
            zx = zx2 - zy2 + cx;
            zx2 = zx * zx;
            zy2 = zy * zy;
            iteration++;
        }
    }
    iterations[gid.y * params.width + gid.x] = iteration;
}

kernel void mandelbrot_compute_row(
    device uint *iterations [[buffer(0)]],
    constant ComputeParams &params [[buffer(1)]],
    constant uint &row_y [[buffer(2)]],
    uint gid [[thread_position_in_grid]]
) {
    if (gid >= params.width) return;
    float cx = (float(gid) - params.width_half) * params.cx_scale;
    float cy = (float(row_y) - params.height_half) * params.cy_scale;
    uint iteration = params.max_iter;
    if (!is_in_cardioid_or_bulb(cx, cy)) {
        float zx = 0.0f, zy = 0.0f, zx2 = 0.0f, zy2 = 0.0f;
        iteration = 0;
        while (zx2 + zy2 < 4.0f && iteration < params.max_iter) {
            zy = 2.0f * zx * zy + cy;
            zx = zx2 - zy2 + cx;
            zx2 = zx * zx;
            zy2 = zy * zy;
            iteration++;
        }
    }
    iterations[gid] = iteration;
}

struct TileParams {
    float center_x;
    float center_y;
    float scale;
    uint tile_x;
    uint tile_y;
    uint tile_size;
    uint max_iter;
    uint vp_half_w;
    uint vp_half_h;
};

kernel void mandelbrot_compute_tile(
    device uint *iterations [[buffer(0)]],
    constant TileParams &params [[buffer(1)]],
    uint2 gid [[thread_position_in_grid]]
) {
    if (gid.x >= params.tile_size || gid.y >= params.tile_size) return;
    float px = float(params.tile_x + gid.x);
    float py = float(params.tile_y + gid.y);
    float cx = params.center_x + (px - float(params.vp_half_w)) * params.scale;
    float cy = params.center_y + (py - float(params.vp_half_h)) * params.scale;
    uint iteration = params.max_iter;
    if (!is_in_cardioid_or_bulb(cx, cy)) {
        float zx = 0.0f, zy = 0.0f, zx2 = 0.0f, zy2 = 0.0f;
        iteration = 0;
        while (zx2 + zy2 < 4.0f && iteration < params.max_iter) {
            zy = 2.0f * zx * zy + cy;
            zx = zx2 - zy2 + cx;
            zx2 = zx * zx;
            zy2 = zy * zy;
            iteration++;
        }
    }
    iterations[gid.y * params.tile_size + gid.x] = iteration;
}

struct PerturbParams {
    float center_x, center_y, scale;
    float ref_cx, ref_cy;
    uint tile_x, tile_y, tile_size, max_iter;
    uint ref_escape_iter, vp_half_w, vp_half_h;
};

kernel void mandelbrot_compute_perturb(
    device uint *iterations [[buffer(0)]],
    constant PerturbParams &params [[buffer(1)]],
    constant float2 *ref_orbit [[buffer(2)]],
    uint2 gid [[thread_position_in_grid]]
) {
    if (gid.x >= params.tile_size || gid.y >= params.tile_size) return;

    float px = float(params.tile_x + gid.x);
    float py = float(params.tile_y + gid.y);

    // Delta from reference: dC = C_pixel - C_ref
    float delta_cx = params.center_x + (px - float(params.vp_half_w)) * params.scale - params.ref_cx;
    float delta_cy = params.center_y + (py - float(params.vp_half_h)) * params.scale - params.ref_cy;

    float delta_x = 0.0f, delta_y = 0.0f;
    uint iteration = 0;
    uint max_safe = min(params.max_iter, params.ref_escape_iter);

    while (iteration < max_safe) {
        float2 z_ref = ref_orbit[iteration];
        float zx = z_ref.x + delta_x;
        float zy = z_ref.y + delta_y;

        if (zx*zx + zy*zy >= 4.0f) break;

        // d_n+1 = 2*Z_ref*d + d^2 + dC
        float new_dx = 2.0f*(z_ref.x*delta_x - z_ref.y*delta_y)
                     + delta_x*delta_x - delta_y*delta_y + delta_cx;
        float new_dy = 2.0f*(z_ref.x*delta_y + z_ref.y*delta_x)
                     + 2.0f*delta_x*delta_y + delta_cy;

        // Glitch detection: delta magnitude too large relative to reference
        float delta_mag = new_dx*new_dx + new_dy*new_dy;
        float ref_mag = z_ref.x*z_ref.x + z_ref.y*z_ref.y;
        if (delta_mag > ref_mag * 1e6f && ref_mag > 1e-10f) {
            iterations[gid.y * params.tile_size + gid.x] = 0xFFFFFFFE;
            return;
        }

        delta_x = new_dx;
        delta_y = new_dy;
        iteration++;
    }

    // Mark glitch if pixel needs more iterations than reference
    if (iteration >= params.ref_escape_iter && iteration < params.max_iter) {
        iterations[gid.y * params.tile_size + gid.x] = 0xFFFFFFFE;
        return;
    }

    iterations[gid.y * params.tile_size + gid.x] = iteration;
}

struct PerturbParamsV2 {
    uint tile_size, max_iter, ref_escape_iter;
};

kernel void mandelbrot_compute_perturb_v2(
    device uint *iterations [[buffer(0)]],
    constant PerturbParamsV2 &params [[buffer(1)]],
    constant double2 *ref_orbit [[buffer(2)]],
    constant double2 *pixel_deltas [[buffer(3)]],
    uint2 gid [[thread_position_in_grid]]
) {
    if (gid.x >= params.tile_size || gid.y >= params.tile_size) return;

    uint idx = gid.y * params.tile_size + gid.x;
    double2 delta_c = pixel_deltas[idx];  // Pre-computed on CPU in double precision!

    double delta_x = 0.0, delta_y = 0.0;
    uint iteration = 0;
    uint max_safe = min(params.max_iter, params.ref_escape_iter);

    while (iteration < max_safe) {
        double2 z_ref = ref_orbit[iteration];
        double zx = z_ref.x + delta_x;
        double zy = z_ref.y + delta_y;

        if (zx*zx + zy*zy >= 4.0) break;

        // d_n+1 = 2*Z_ref*d + d^2 + dC
        double new_dx = 2.0*(z_ref.x*delta_x - z_ref.y*delta_y)
                     + delta_x*delta_x - delta_y*delta_y + delta_c.x;
        double new_dy = 2.0*(z_ref.x*delta_y + z_ref.y*delta_x)
                     + 2.0*delta_x*delta_y + delta_c.y;

        // Glitch detection: delta magnitude too large relative to reference
        double delta_mag = new_dx*new_dx + new_dy*new_dy;
        double ref_mag = z_ref.x*z_ref.x + z_ref.y*z_ref.y;
        if (delta_mag > ref_mag * 1e6 && ref_mag > 1e-10) {
            iterations[idx] = 0xFFFFFFFE;
            return;
        }

        delta_x = new_dx;
        delta_y = new_dy;
        iteration++;
    }

    // Mark glitch if pixel needs more iterations than reference
    if (iteration >= params.ref_escape_iter && iteration < params.max_iter) {
        iterations[idx] = 0xFFFFFFFE;
        return;
    }

    iterations[idx] = iteration;
}
