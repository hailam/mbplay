#include "perturb_cpu.h"

#include <stddef.h>

uint32_t perturb_cpu_pixel(const double *ref_re, const double *ref_im,
                           uint32_t ref_len,
                           double dcx, double dcy,
                           uint32_t max_iter, float *final_z2)
{
    double dx = 0.0, dy = 0.0;   // dz relative to the current anchor
    uint32_t m = 0;              // index into the reference orbit
    double zmag = 0.0;

    for (uint32_t n = 0; n < max_iter; n++) {
        double rr = ref_re[m];
        double ri = ref_im[m];
        double zr = rr + dx;
        double zi = ri + dy;
        zmag = zr * zr + zi * zi;

        if (zmag >= 4.0) {
            if (final_z2) *final_z2 = (float)zmag;
            return n;
        }

        // Rebase when the full value is smaller than the delta (cancellation
        // ahead) or when the next orbit entry does not exist. Z_ref[0] = 0,
        // so representing z as a delta from orbit index 0 is exact.
        if (m + 1 >= ref_len || zmag < dx * dx + dy * dy) {
            dx = zr;
            dy = zi;
            rr = 0.0;
            ri = 0.0;
            m = 0;
        }

        // dz' = 2*Z_ref*dz + dz^2 + dc
        double ndx = 2.0 * (rr * dx - ri * dy) + (dx * dx - dy * dy) + dcx;
        double ndy = 2.0 * (rr * dy + ri * dx) + 2.0 * dx * dy + dcy;
        dx = ndx;
        dy = ndy;
        m++;
    }

    if (final_z2) *final_z2 = (float)zmag;
    return max_iter;
}

void perturb_cpu_tile(const double *ref_re, const double *ref_im,
                      uint32_t ref_len,
                      const double *deltas, uint32_t tile_size,
                      uint32_t max_iter,
                      uint32_t *iterations, float *final_z2)
{
    size_t pixels = (size_t)tile_size * tile_size;
    for (size_t i = 0; i < pixels; i++) {
        float z2 = 0.0f;
        iterations[i] = perturb_cpu_pixel(ref_re, ref_im, ref_len,
                                          deltas[i * 2], deltas[i * 2 + 1],
                                          max_iter, &z2);
        if (final_z2) final_z2[i] = z2;
    }
}
