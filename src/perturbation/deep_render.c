#include "deep_render.h"
#include "perturbation.h"
#include "perturb_cpu.h"
#include "../precision/mp_real.h"
#include "../color/color.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

// How far (in complex units, as a multiple of the pixel scale) the reference
// point may drift from the view center before the orbit is rebuilt. Deltas
// are doubles; at 1e12 pixels of drift their relative rounding (~1e-16)
// still leaves the per-pixel error under 1e-4 of a pixel.
#define MB_DEEP_MAX_REF_DRIFT_PX 1e12

// =============================================================================
// Refcounted orbit snapshot
// =============================================================================

typedef struct {
    ReferenceOrbitHP orbit;      // owns the double arrays + ref strings
    uint32_t computed_max_iter;  // iteration budget the orbit was built with
    _Atomic int refs;
} DeepOrbit;

static void deep_orbit_release(DeepOrbit *o) {
    if (!o) return;
    if (atomic_fetch_sub(&o->refs, 1) == 1) {
        ref_orbit_hp_cleanup(&o->orbit);
        free(o);
    }
}

struct MBDeepRenderer {
    pthread_mutex_t lock;
    DeepOrbit *cur;          // current orbit snapshot (may be NULL)
    uint64_t latest_gen;     // newest generation seen (guarded by lock)
    // Offset (view center - reference point) for latest_gen, in complex units.
    double gen_ref_dx, gen_ref_dy;
    // Newest generation announced by the UI; read lock-free by tile loops so
    // stale tiles can abort mid-render.
    _Atomic uint64_t announced_gen;
};

MBDeepRenderer *mb_deep_renderer_create(void) {
    MBDeepRenderer *r = calloc(1, sizeof(MBDeepRenderer));
    if (!r) return NULL;
    if (pthread_mutex_init(&r->lock, NULL) != 0) {
        free(r);
        return NULL;
    }
    return r;
}

void mb_deep_renderer_destroy(MBDeepRenderer *r) {
    if (!r) return;
    deep_orbit_release(r->cur);
    pthread_mutex_destroy(&r->lock);
    free(r);
}

void mb_deep_renderer_note_generation(MBDeepRenderer *r, uint64_t generation) {
    if (!r) return;
    atomic_store(&r->announced_gen, generation);
}

// Compute (view center - orbit reference) in HP; returns the offsets as
// doubles. Small results are exact; large results only need enough relative
// precision to keep sub-pixel accuracy (checked against the drift limit).
static void compute_ref_offset(const MBViewState *view, const DeepOrbit *orbit,
                               double *out_dx, double *out_dy) {
    mpfr_prec_t prec = mp_required_precision(view->zoom_level) + 64;

    MPReal a, b, d;
    mp_real_init_set_str(&a, view->center_x_str, prec);
    mp_real_init_set_str(&b, orbit->orbit.ref_cx_str, prec);
    mp_real_init(&d, prec);
    mp_real_sub(&d, &a, &b);
    *out_dx = mp_real_get_d(&d);

    mp_real_set_str(&a, view->center_y_str);
    mp_real_set_str(&b, orbit->orbit.ref_cy_str);
    mp_real_sub(&d, &a, &b);
    *out_dy = mp_real_get_d(&d);

    mp_real_clear(&a);
    mp_real_clear(&b);
    mp_real_clear(&d);
}

// Build a new orbit at the view center. Returns NULL on failure.
static DeepOrbit *build_orbit(const MBViewState *view, uint32_t max_iter) {
    DeepOrbit *o = calloc(1, sizeof(DeepOrbit));
    if (!o) return NULL;

    mpfr_prec_t prec = mp_required_precision(view->zoom_level);
    if (ref_orbit_hp_init(&o->orbit, max_iter, (uint32_t)prec) != 0) {
        free(o);
        return NULL;
    }

    ref_orbit_hp_compute(&o->orbit, view->center_x_str, view->center_y_str);
    if (!o->orbit.valid || o->orbit.escape_iter < 1) {
        ref_orbit_hp_cleanup(&o->orbit);
        free(o);
        return NULL;
    }

    o->computed_max_iter = max_iter;
    atomic_store(&o->refs, 1);
    return o;
}

int mb_deep_renderer_render_tile(MBDeepRenderer *r,
                                 const MBViewState *view, uint64_t generation,
                                 int tile_px, int tile_py, int tile_size,
                                 uint32_t max_iter,
                                 const MBRenderSettings *settings,
                                 PixelColor *out) {
    if (!r || !view || !out || tile_size <= 0) return MB_DEEP_ERROR;

    double scale = mb_view_get_scale(view);
    if (!(scale > 0.0) || !isfinite(scale)) return MB_DEEP_ERROR;

    // --- Acquire an orbit snapshot consistent with this generation ---
    pthread_mutex_lock(&r->lock);

    if (generation < r->latest_gen ||
        generation < atomic_load(&r->announced_gen)) {
        // Superseded before we even started: don't build an orbit (an
        // expensive MPFR computation under this mutex) for a view that is
        // already gone.
        pthread_mutex_unlock(&r->lock);
        return MB_DEEP_STALE;
    }

    if (generation > r->latest_gen || !r->cur) {
        // New view state: decide whether the existing orbit is reusable.
        bool rebuild = true;
        double dx = 0.0, dy = 0.0;

        if (r->cur && r->cur->computed_max_iter >= max_iter) {
            compute_ref_offset(view, r->cur, &dx, &dy);
            double drift_limit = MB_DEEP_MAX_REF_DRIFT_PX * scale;
            if (fabs(dx) < drift_limit && fabs(dy) < drift_limit) {
                rebuild = false;
            }
        }

        if (rebuild) {
            DeepOrbit *fresh = build_orbit(view, max_iter);
            if (!fresh) {
                pthread_mutex_unlock(&r->lock);
                return MB_DEEP_ERROR;
            }
            deep_orbit_release(r->cur);
            r->cur = fresh;
            dx = 0.0;
            dy = 0.0;
        }

        r->latest_gen = generation;
        r->gen_ref_dx = dx;
        r->gen_ref_dy = dy;
    }

    DeepOrbit *orbit = r->cur;
    atomic_fetch_add(&orbit->refs, 1);
    double ref_dx = r->gen_ref_dx;
    double ref_dy = r->gen_ref_dy;

    pthread_mutex_unlock(&r->lock);

    // --- Iterate the tile ---
    const double *ref_re = orbit->orbit.z_real;
    const double *ref_im = orbit->orbit.z_imag;
    // Stored entries run to escape_iter inclusive when the reference escaped
    // early, and to max_iter-1 when it never escaped.
    uint32_t ref_len = orbit->orbit.escape_iter + 1;
    if (ref_len > orbit->orbit.max_iter) ref_len = orbit->orbit.max_iter;

    double half_w = view->viewport_width / 2.0;
    double half_h = view->viewport_height / 2.0;

    for (int ly = 0; ly < tile_size; ly++) {
        // Abort mid-tile if the UI announced a newer generation: at extreme
        // zoom a tile can take seconds, and the user has already moved on.
        if (atomic_load(&r->announced_gen) > generation) {
            deep_orbit_release(orbit);
            return MB_DEEP_STALE;
        }

        int py = tile_py + ly;
        // Screen py grows downward, imaginary axis grows upward.
        double dcy = ref_dy + (half_h - py) * scale;

        for (int lx = 0; lx < tile_size; lx++) {
            int px = tile_px + lx;
            double dcx = ref_dx + (px - half_w) * scale;

            float z2 = 0.0f;
            uint32_t iter = perturb_cpu_pixel(ref_re, ref_im, ref_len,
                                              dcx, dcy, max_iter, &z2);
            color_from_iteration_ex(&out[(size_t)ly * (size_t)tile_size + (size_t)lx],
                                    iter, z2, max_iter, settings);
        }
    }

    deep_orbit_release(orbit);
    return MB_DEEP_OK;
}
