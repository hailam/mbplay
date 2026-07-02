#include "perturb_cpu.h"
#include "bla.h"

#include <stddef.h>
#include <math.h>

// =============================================================================
// Double-precision pixel loop (with optional BLA skipping)
// =============================================================================

uint32_t perturb_cpu_pixel_bla(const double *ref_re, const double *ref_im,
                               uint32_t ref_len,
                               double dcx, double dcy,
                               uint32_t max_iter,
                               const MBBlaTable *bla, float *final_z2)
{
    double dx = 0.0, dy = 0.0;   // dz relative to the current anchor
    uint32_t m = 0;              // index into the reference orbit
    uint32_t n = 0;
    double zmag = 0.0;
    FloatExpC dcf = fxc_from_d(dcx, dcy);   // for BLA applications

    while (n < max_iter) {
        double rr = ref_re[m];
        double ri = ref_im[m];
        double zr = rr + dx;
        double zi = ri + dy;
        zmag = zr * zr + zi * zi;

        if (zmag >= 4.0) {
            if (final_z2) *final_z2 = (float)zmag;
            return n;
        }

        double dmag = dx * dx + dy * dy;

        // Rebase when the full value is smaller than the delta (cancellation
        // ahead) or when the next orbit entry does not exist. Z_ref[0] = 0,
        // so representing z as a delta from orbit index 0 is exact.
        if (m + 1 >= ref_len || zmag < dmag) {
            dx = zr;
            dy = zi;
            rr = 0.0;
            ri = 0.0;
            m = 0;
            dmag = zmag;
        }

        // Try to skip a run of iterations with a bilinear approximation
        if (bla && m != 0) {
            const MBBlaEntry *e = mb_bla_lookup(bla, m, fx_from_d(dmag));
            if (e && (uint64_t)m + e->l < ref_len) {
                FloatExpC dz = fxc_from_d(dx, dy);
                FloatExpC A = {e->a_re, e->a_im};
                FloatExpC B = {e->b_re, e->b_im};
                dz = fxc_add(fxc_mul(A, dz), fxc_mul(B, dcf));
                dx = fx_to_d(dz.re);
                dy = fx_to_d(dz.im);
                m += e->l;
                n += e->l;
                continue;
            }
        }

        // dz' = 2*Z_ref*dz + dz^2 + dc
        double ndx = 2.0 * (rr * dx - ri * dy) + (dx * dx - dy * dy) + dcx;
        double ndy = 2.0 * (rr * dy + ri * dx) + 2.0 * dx * dy + dcy;
        dx = ndx;
        dy = ndy;
        m++;
        n++;
    }

    if (final_z2) *final_z2 = (float)zmag;
    return max_iter;
}

uint32_t perturb_cpu_pixel(const double *ref_re, const double *ref_im,
                           uint32_t ref_len,
                           double dcx, double dcy,
                           uint32_t max_iter, float *final_z2)
{
    return perturb_cpu_pixel_bla(ref_re, ref_im, ref_len, dcx, dcy,
                                 max_iter, NULL, final_z2);
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

// =============================================================================
// Extended-exponent variant
// =============================================================================

// |dz| components above 2^MB_FX_EXIT_EXP are safe to iterate in double:
// dz^2 stays in normal range and anything the double math cannot see
// (|dc| below the rounding floor of the recurrence terms) is genuinely
// negligible at that magnitude.
#define MB_FX_EXIT_EXP (-480)

static inline int64_t fx_exp_or_min(FloatExp a) {
    return a.m == 0.0 ? INT64_MIN : a.e;
}

uint32_t perturb_cpu_pixel_fx_bla(const double *ref_re, const double *ref_im,
                                  uint32_t ref_len,
                                  FloatExp dcx, FloatExp dcy,
                                  uint32_t max_iter,
                                  const MBBlaTable *bla, float *final_z2)
{
    // If the delta already fits comfortably in double range, use the fast path.
    if (fx_exp_or_min(dcx) > -460 || fx_exp_or_min(dcy) > -460) {
        return perturb_cpu_pixel_bla(ref_re, ref_im, ref_len,
                                     fx_to_d(dcx), fx_to_d(dcy),
                                     max_iter, bla, final_z2);
    }

    double dc_d_x = fx_to_d(dcx);   // 0.0 when below double range (negligible
    double dc_d_y = fx_to_d(dcy);   // relative to a double-range dz)

    FloatExpC dc = {dcx, dcy};
    FloatExpC dz = fxc_zero();
    uint32_t m = 0;
    uint32_t n = 0;
    double last_zmag = 0.0;

    while (n < max_iter) {
        // ---------------- floatexp phase ----------------
        while (n < max_iter) {
            FloatExpC Z = fxc_from_d(ref_re[m], ref_im[m]);
            FloatExpC z = fxc_add(Z, dz);
            FloatExp zmag = fxc_norm_sqr(z);

            if (fx_ge_d(zmag, 4.0)) {
                if (final_z2) *final_z2 = (float)fx_to_d(zmag);
                return n;
            }

            FloatExp dmag = fxc_norm_sqr(dz);
            if (m + 1 >= ref_len || fx_cmp_abs(zmag, dmag) < 0) {
                dz = z;
                Z = fxc_zero();
                m = 0;
                dmag = zmag;
            }

            // BLA skip (floatexp application)
            if (bla && m != 0) {
                const MBBlaEntry *e = mb_bla_lookup(bla, m, dmag);
                if (e && (uint64_t)m + e->l < ref_len) {
                    FloatExpC A = {e->a_re, e->a_im};
                    FloatExpC B = {e->b_re, e->b_im};
                    dz = fxc_add(fxc_mul(A, dz), fxc_mul(B, dc));
                    m += e->l;
                    n += e->l;
                } else {
                    goto fx_single_step;
                }
            } else {
            fx_single_step:;
                // dz' = 2*Z*dz + dz^2 + dc
                FloatExpC two_z_dz = fxc_mul(Z, dz);
                two_z_dz.re = fx_scale2(two_z_dz.re, 1);
                two_z_dz.im = fx_scale2(two_z_dz.im, 1);
                FloatExpC dz2 = fxc_mul(dz, dz);
                dz = fxc_add(fxc_add(two_z_dz, dz2), dc);
                m++;
                n++;
            }

            // Exit to the double phase once dz is comfortably representable
            if (fx_exp_or_min(dz.re) > MB_FX_EXIT_EXP ||
                fx_exp_or_min(dz.im) > MB_FX_EXIT_EXP) {
                break;
            }
        }

        if (n >= max_iter) break;

        // ---------------- double phase ----------------
        double dx = fx_to_d(dz.re);
        double dy = fx_to_d(dz.im);
        FloatExpC dcf = dc;

        bool back_to_fx = false;
        while (n < max_iter) {
            double rr = ref_re[m];
            double ri = ref_im[m];
            double zr = rr + dx;
            double zi = ri + dy;
            double zmag = zr * zr + zi * zi;
            last_zmag = zmag;

            if (zmag >= 4.0) {
                if (final_z2) *final_z2 = (float)zmag;
                return n;
            }

            double dmag = dx * dx + dy * dy;

            if (m + 1 >= ref_len || zmag < dmag) {
                dx = zr;
                dy = zi;
                rr = 0.0;
                ri = 0.0;
                m = 0;
                dmag = zmag;
                // If the rebase left dz below the double comfort zone, the dc
                // term matters again and must be added in floatexp.
                double mag = fabs(dx) > fabs(dy) ? fabs(dx) : fabs(dy);
                if (mag < 0x1p-460) {
                    dz.re = fx_from_d(dx);
                    dz.im = fx_from_d(dy);
                    back_to_fx = true;
                    break;
                }
            }

            if (bla && m != 0) {
                const MBBlaEntry *e = mb_bla_lookup(bla, m, fx_from_d(dmag));
                if (e && (uint64_t)m + e->l < ref_len) {
                    FloatExpC dzf = fxc_from_d(dx, dy);
                    FloatExpC A = {e->a_re, e->a_im};
                    FloatExpC B = {e->b_re, e->b_im};
                    dzf = fxc_add(fxc_mul(A, dzf), fxc_mul(B, dcf));
                    m += e->l;
                    n += e->l;
                    // A skip can shrink dz back below double comfort
                    if (fx_exp_or_min(dzf.re) <= MB_FX_EXIT_EXP &&
                        fx_exp_or_min(dzf.im) <= MB_FX_EXIT_EXP) {
                        dz = dzf;
                        back_to_fx = true;
                        break;
                    }
                    dx = fx_to_d(dzf.re);
                    dy = fx_to_d(dzf.im);
                    continue;
                }
            }

            double ndx = 2.0 * (rr * dx - ri * dy) + (dx * dx - dy * dy) + dc_d_x;
            double ndy = 2.0 * (rr * dy + ri * dx) + 2.0 * dx * dy + dc_d_y;
            dx = ndx;
            dy = ndy;
            m++;
            n++;

            // dz can also decay *gradually* (sustained |2Z| < 1) without a
            // rebase; once it drops below the comfort zone the flushed dc
            // term matters again and iteration must continue in floatexp,
            // or dz would eventually flush to zero and freeze the pixel
            // onto the reference orbit.
            if (fabs(dx) < 0x1p-460 && fabs(dy) < 0x1p-460) {
                dz.re = fx_from_d(dx);
                dz.im = fx_from_d(dy);
                back_to_fx = true;
                break;
            }
        }

        if (!back_to_fx) break;
    }

    if (final_z2) *final_z2 = (float)last_zmag;
    return max_iter;
}

uint32_t perturb_cpu_pixel_fx(const double *ref_re, const double *ref_im,
                              uint32_t ref_len,
                              FloatExp dcx, FloatExp dcy,
                              uint32_t max_iter, float *final_z2)
{
    return perturb_cpu_pixel_fx_bla(ref_re, ref_im, ref_len, dcx, dcy,
                                    max_iter, NULL, final_z2);
}
