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

// Tile computation with final |z|^2 output for smooth coloring
kernel void mandelbrot_compute_tile_smooth(
    device uint *iterations [[buffer(0)]],
    device float *final_z2 [[buffer(2)]],
    constant TileParams &params [[buffer(1)]],
    uint2 gid [[thread_position_in_grid]]
) {
    if (gid.x >= params.tile_size || gid.y >= params.tile_size) return;
    float px = float(params.tile_x + gid.x);
    float py = float(params.tile_y + gid.y);
    float cx = params.center_x + (px - float(params.vp_half_w)) * params.scale;
    float cy = params.center_y + (py - float(params.vp_half_h)) * params.scale;

    uint idx = gid.y * params.tile_size + gid.x;
    uint iteration = params.max_iter;
    float z2 = 0.0f;

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
        z2 = zx2 + zy2;
    }

    iterations[idx] = iteration;
    final_z2[idx] = z2;
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

// NOTE: The former "perturbation V2" kernels used the double/double2 types,
// which Metal does not support on any Apple GPU. They could never compile,
// which silently disabled the entire GPU library (newLibraryWithSource fails
// for the whole source) and with it every float kernel below. V2 perturbation
// now runs on the CPU in src/perturbation/perturb_cpu.c, which has real
// 64-bit doubles and needs no glitch markers thanks to rebasing.

// =============================================================================
// Float-float ("df") perturbation — GPU deep zoom without doubles
// =============================================================================
//
// A value is a pair (hi, lo) of floats with hi+lo ~ 48-bit precision
// (Dekker/Knuth error-free transformations, FMA-based). The float EXPONENT
// range limits |dc| to >= ~1e-32, i.e. zoom up to ~1e30: within that window
// this kernel matches the CPU double iteration to within +-1 count on
// boundary pixels. Deeper zooms stay on the CPU (floatexp + BLA).
//
// Rebasing (Zhuoran) replaces glitch detection, exactly like the CPU loops.

static float2 df_add(float2 a, float2 b) {
    float s = a.x + b.x;
    float bb = s - a.x;
    float err = (a.x - (s - bb)) + (b.x - bb);
    err += a.y + b.y;
    float hi = s + err;
    return float2(hi, err - (hi - s));
}

static float2 df_neg(float2 a) {
    return float2(-a.x, -a.y);
}

static float2 df_sub(float2 a, float2 b) {
    return df_add(a, df_neg(b));
}

static float2 df_mul(float2 a, float2 b) {
    float p = a.x * b.x;
    float e = fma(a.x, b.x, -p);   // exact low part of the product
    e = fma(a.x, b.y, e);
    e = fma(a.y, b.x, e);
    float hi = p + e;
    return float2(hi, e - (hi - p));
}

static float2 df_scale2(float2 a) {
    return float2(a.x * 2.0f, a.y * 2.0f);   // exact
}

struct DfPerturbParams {
    uint tile_size;
    uint max_iter;
    uint ref_len;
    float dcx0_hi, dcx0_lo;    // dc at the tile origin (df)
    float dcy0_hi, dcy0_lo;
    float stepx_hi, stepx_lo;  // dc change per pixel column (df)
    float stepy_hi, stepy_lo;  // dc change per pixel row (df, sign included)
};

kernel void mandelbrot_perturb_df(
    device uint *iterations [[buffer(0)]],
    device float *final_z2 [[buffer(1)]],
    constant DfPerturbParams &p [[buffer(2)]],
    constant float4 *ref [[buffer(3)]],   // (re_hi, re_lo, im_hi, im_lo)
    uint2 gid [[thread_position_in_grid]]
) {
    if (gid.x >= p.tile_size || gid.y >= p.tile_size) return;

    float2 dcx = df_add(float2(p.dcx0_hi, p.dcx0_lo),
                        df_mul(float2(p.stepx_hi, p.stepx_lo), float2((float)gid.x, 0.0f)));
    float2 dcy = df_add(float2(p.dcy0_hi, p.dcy0_lo),
                        df_mul(float2(p.stepy_hi, p.stepy_lo), float2((float)gid.y, 0.0f)));

    float2 dx = float2(0.0f), dy = float2(0.0f);
    uint m = 0;
    uint n = 0;
    float zmag = 0.0f;

    for (; n < p.max_iter; n++) {
        float4 Z = ref[m];
        float2 zr = df_add(float2(Z.x, Z.y), dx);
        float2 zi = df_add(float2(Z.z, Z.w), dy);
        zmag = zr.x * zr.x + zi.x * zi.x;
        if (zmag >= 4.0f) break;

        float dmag = dx.x * dx.x + dy.x * dy.x;
        if (m + 1 >= p.ref_len || zmag < dmag) {
            // Rebase: Z_ref[0] == 0, so z re-expressed from index 0 is exact
            dx = zr;
            dy = zi;
            Z = float4(0.0f);
            m = 0;
        }

        float2 Zr = float2(Z.x, Z.y);
        float2 Zi = float2(Z.z, Z.w);
        // dz' = 2*Z*dz + dz^2 + dc
        float2 t_re = df_sub(df_mul(Zr, dx), df_mul(Zi, dy));
        float2 t_im = df_add(df_mul(Zr, dy), df_mul(Zi, dx));
        float2 sq_re = df_sub(df_mul(dx, dx), df_mul(dy, dy));
        float2 sq_im = df_scale2(df_mul(dx, dy));
        float2 ndx = df_add(df_add(df_scale2(t_re), sq_re), dcx);
        float2 ndy = df_add(df_add(df_scale2(t_im), sq_im), dcy);
        dx = ndx;
        dy = ndy;
        m++;
    }

    uint idx = gid.y * p.tile_size + gid.x;
    iterations[idx] = n;
    final_z2[idx] = zmag;
}

// =============================================================================
// Supersampled Kernels (2x2 MSAA)
// =============================================================================

// Helper function to compute smooth iteration for a single point
static float compute_smooth_iter(float cx, float cy, uint max_iter) {
    if (is_in_cardioid_or_bulb(cx, cy)) {
        return float(max_iter);
    }

    float zx = 0.0f, zy = 0.0f, zx2 = 0.0f, zy2 = 0.0f;
    uint iteration = 0;

    while (zx2 + zy2 < 4.0f && iteration < max_iter) {
        zy = 2.0f * zx * zy + cy;
        zx = zx2 - zy2 + cx;
        zx2 = zx * zx;
        zy2 = zy * zy;
        iteration++;
    }

    // Compute smooth iteration
    if (iteration >= max_iter) {
        return float(max_iter);
    }

    float z2 = zx2 + zy2;
    if (z2 <= 4.0f) {
        return float(iteration);
    }

    // smooth = iter + 1 - log2(log(z2) / log(4)) = iter + 1 - log2(log(z2)) + 1
    float log_z2 = log(z2);
    float log_log_z = log(log_z2 * 0.5f);
    float log2_log_z = log_log_z / 0.693147f;

    return float(iteration) + 1.0f - log2_log_z;
}

// 2x2 supersampled tile computation with smooth output
kernel void mandelbrot_compute_tile_ss4(
    device float *smooth_iter [[buffer(0)]],
    constant TileParams &params [[buffer(1)]],
    uint2 gid [[thread_position_in_grid]]
) {
    if (gid.x >= params.tile_size || gid.y >= params.tile_size) return;

    // 2x2 jittered sample offsets (rotated grid pattern)
    const float offsets[4][2] = {
        {-0.375f, -0.125f},
        { 0.125f, -0.375f},
        {-0.125f,  0.375f},
        { 0.375f,  0.125f}
    };

    float px = float(params.tile_x + gid.x);
    float py = float(params.tile_y + gid.y);
    float base_cx = params.center_x + (px - float(params.vp_half_w)) * params.scale;
    float base_cy = params.center_y + (py - float(params.vp_half_h)) * params.scale;

    float sum = 0.0f;
    for (int s = 0; s < 4; s++) {
        float cx = base_cx + offsets[s][0] * params.scale;
        float cy = base_cy + offsets[s][1] * params.scale;
        sum += compute_smooth_iter(cx, cy, params.max_iter);
    }

    smooth_iter[gid.y * params.tile_size + gid.x] = sum / 4.0f;
}

// (The double-typed "perturbation V2 smooth" kernel was removed for the same
// reason — see the note above. CPU replacement: perturb_cpu_tile + smooth
// coloring in gpu.c / deep_render.c.)
