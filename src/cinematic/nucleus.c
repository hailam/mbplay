#include "nucleus.h"
#include "../precision/mp_real.h"

#include <mpfr.h>
#include <math.h>

// Newton iterations: quadratic convergence, so ~24 covers any realistic
// precision once inside the basin; the basin check is the radius test.
#define NUCLEUS_NEWTON_MAX 24

bool mb_nucleus_find(const char *guess_x, const char *guess_y,
                     double zoom_log10,
                     uint32_t min_period, uint32_t max_period,
                     char out_x[MB_HP_COORD_STR_LEN],
                     char out_y[MB_HP_COORD_STR_LEN],
                     uint32_t *out_period) {
    if (max_period < 4 || min_period >= max_period) return false;

    // Extra headroom beyond the current zoom: the lock must stay accurate
    // for the ~60 doublings of flight+descent that follow (an error of
    // 1e-6 frame spans is frame-sized 20 doublings later).
    mpfr_prec_t prec = mp_required_precision_log10(zoom_log10 + 24.0);
    mpfr_rnd_t R = MPFR_RNDN;

    mpfr_t cx, cy, gx, gy;      // current Newton c, original guess
    mpfr_t zx, zy, dx, dy;      // orbit z and dz/dc
    mpfr_t t1, t2, t3, r2;      // temps
    mpfr_t best_r2, radius2, tol2;
    mpfr_inits2(prec, cx, cy, gx, gy, zx, zy, dx, dy,
                t1, t2, t3, r2, best_r2, radius2, tol2, (mpfr_ptr)0);

    mpfr_set_str(gx, guess_x, 10, R);
    mpfr_set_str(gy, guess_y, 10, R);
    mpfr_set(cx, gx, R);
    mpfr_set(cy, gy, R);

    // Frame span at this zoom: 4 * 10^-zoom_log10 (base view spans ~4).
    // Accept a nucleus within 0.8 span of the guess; converge to 1e-18
    // span so the coordinate stays nucleus-accurate through the ~60
    // doublings of approach and descent that follow.
    mpfr_set_d(t1, -zoom_log10, R);
    mpfr_exp10(t1, t1, R);                    // 10^-zoom (any exponent: mpfr)
    mpfr_mul_d(t2, t1, 4.0 * 0.8, R);
    mpfr_sqr(radius2, t2, R);
    mpfr_mul_d(t2, t1, 4.0e-18, R);
    mpfr_sqr(tol2, t2, R);

    // ---- Phase 1: near-period = argmin |z| along the orbit at the guess
    uint32_t period = 0;
    mpfr_set_d(best_r2, 1e300, R);
    mpfr_set_zero(zx, 1);
    mpfr_set_zero(zy, 1);
    for (uint32_t k = 1; k <= max_period; k++) {
        // z = z^2 + c
        mpfr_sqr(t1, zx, R);
        mpfr_sqr(t2, zy, R);
        mpfr_mul(t3, zx, zy, R);
        mpfr_sub(zx, t1, t2, R);
        mpfr_add(zx, zx, cx, R);
        mpfr_mul_2ui(zy, t3, 1, R);
        mpfr_add(zy, zy, cy, R);

        mpfr_sqr(t1, zx, R);
        mpfr_sqr(t2, zy, R);
        mpfr_add(r2, t1, t2, R);
        if (mpfr_cmp_d(r2, 4.0) > 0) break;   // escaped: dips end here
        if (k >= 2 && k > min_period && mpfr_cmp(r2, best_r2) < 0) {
            mpfr_set(best_r2, r2, R);
            period = k;
        }
    }
    if (period < 2) {
        mpfr_clears(cx, cy, gx, gy, zx, zy, dx, dy,
                    t1, t2, t3, r2, best_r2, radius2, tol2, (mpfr_ptr)0);
        return false;
    }

    // ---- Phase 2: Newton on z_p(c) = 0
    bool ok = false;
    for (int it = 0; it < NUCLEUS_NEWTON_MAX; it++) {
        // Orbit with derivative: dz' = 2 z dz + 1, z' = z^2 + c
        mpfr_set_zero(zx, 1);
        mpfr_set_zero(zy, 1);
        mpfr_set_zero(dx, 1);
        mpfr_set_zero(dy, 1);
        bool blew_up = false;
        for (uint32_t k = 0; k < period; k++) {
            // dz = 2*z*dz + 1  (complex)
            mpfr_mul(t1, zx, dx, R);
            mpfr_mul(t2, zy, dy, R);
            mpfr_sub(t1, t1, t2, R);          // Re(z*dz)
            mpfr_mul(t2, zx, dy, R);
            mpfr_mul(t3, zy, dx, R);
            mpfr_add(t2, t2, t3, R);          // Im(z*dz)
            mpfr_mul_2ui(dx, t1, 1, R);
            mpfr_add_ui(dx, dx, 1, R);
            mpfr_mul_2ui(dy, t2, 1, R);
            // z = z^2 + c
            mpfr_sqr(t1, zx, R);
            mpfr_sqr(t2, zy, R);
            mpfr_mul(t3, zx, zy, R);
            mpfr_sub(zx, t1, t2, R);
            mpfr_add(zx, zx, cx, R);
            mpfr_mul_2ui(zy, t3, 1, R);
            mpfr_add(zy, zy, cy, R);

            mpfr_sqr(t1, zx, R);
            mpfr_sqr(t2, zy, R);
            mpfr_add(r2, t1, t2, R);
            if (mpfr_cmp_d(r2, 1e10) > 0) {
                blew_up = true;
                break;
            }
        }
        if (blew_up) break;

        // delta = z / dz;  c -= delta
        mpfr_sqr(t1, dx, R);
        mpfr_sqr(t2, dy, R);
        mpfr_add(t3, t1, t2, R);              // |dz|^2
        if (mpfr_zero_p(t3)) break;
        mpfr_mul(t1, zx, dx, R);
        mpfr_mul(t2, zy, dy, R);
        mpfr_add(t1, t1, t2, R);
        mpfr_div(t1, t1, t3, R);              // Re(delta)
        mpfr_mul(t2, zy, dx, R);
        mpfr_mul(r2, zx, dy, R);
        mpfr_sub(t2, t2, r2, R);
        mpfr_div(t2, t2, t3, R);              // Im(delta)
        mpfr_sub(cx, cx, t1, R);
        mpfr_sub(cy, cy, t2, R);

        // Converged?
        mpfr_sqr(t1, t1, R);
        mpfr_sqr(t2, t2, R);
        mpfr_add(r2, t1, t2, R);
        if (mpfr_cmp(r2, tol2) <= 0) {
            ok = true;
            break;
        }
        // Left the search basin?
        mpfr_sub(t1, cx, gx, R);
        mpfr_sub(t2, cy, gy, R);
        mpfr_sqr(t1, t1, R);
        mpfr_sqr(t2, t2, R);
        mpfr_add(r2, t1, t2, R);
        if (mpfr_cmp(r2, radius2) > 0) break;
    }

    if (ok) {
        // Final radius check (convergence could land at the guess's edge)
        mpfr_sub(t1, cx, gx, R);
        mpfr_sub(t2, cy, gy, R);
        mpfr_sqr(t1, t1, R);
        mpfr_sqr(t2, t2, R);
        mpfr_add(r2, t1, t2, R);
        ok = mpfr_cmp(r2, radius2) <= 0;
    }
    if (ok) {
        // Digit budget: the coordinate must stay meaningful as an anchor
        // ~40 doublings past the lock (boundary-orbit descent)
        mpfr_snprintf(out_x, MB_HP_COORD_STR_LEN, "%.*Rg",
                      (int)(zoom_log10 + 40.0), cx);
        mpfr_snprintf(out_y, MB_HP_COORD_STR_LEN, "%.*Rg",
                      (int)(zoom_log10 + 40.0), cy);
        *out_period = period;
    }

    mpfr_clears(cx, cy, gx, gy, zx, zy, dx, dy,
                t1, t2, t3, r2, best_r2, radius2, tol2, (mpfr_ptr)0);
    return ok;
}
