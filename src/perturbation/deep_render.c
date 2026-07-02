#include "deep_render.h"
#include "perturbation.h"
#include "perturb_cpu.h"
#include "bla.h"
#include "../gpu/gpu.h"
#include "../precision/mp_real.h"
#include "../precision/floatexp.h"
#include "../color/color.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

// How far (in pixels) the reference point may drift from the view center
// before the orbit is rebuilt. Kept small: the BLA validity radii are baked
// with a |dc| bound that covers viewport + this drift, and a loose bound
// would collapse the radii (no skipping). Rebuilding the orbit after a long
// pan is far cheaper than losing BLA on every tile.
#define MB_DEEP_MAX_REF_DRIFT_PX 4096.0

// Above this zoom (log10), per-pixel deltas leave double range and the
// extended-exponent pixel loop is used.
#define MB_DEEP_FX_LOG10 280.0

// Up to this zoom (log10), per-pixel deltas still fit FLOAT exponent range
// (|dc| >= ~1e-32) and tiles can run on the GPU in float-float arithmetic.
#define MB_DEEP_GPU_LOG10 28.0

static _Atomic bool g_gpu_enabled = true;

void mb_deep_render_set_gpu_enabled(bool enabled) {
    atomic_store(&g_gpu_enabled, enabled);
}

// =============================================================================
// Refcounted orbit snapshot
// =============================================================================

typedef struct {
    ReferenceOrbitHP orbit;      // owns the double arrays + ref strings
    MBBlaTable bla;              // iteration-skipping table for this orbit
    FloatExp bla_dc_max;         // |dc| bound the BLA radii were built with
    uint32_t computed_max_iter;  // iteration budget the orbit was built with
    _Atomic int refs;
} DeepOrbit;

static void deep_orbit_release(DeepOrbit *o) {
    if (!o) return;
    if (atomic_fetch_sub(&o->refs, 1) == 1) {
        mb_bla_free(&o->bla);
        ref_orbit_hp_cleanup(&o->orbit);
        free(o);
    }
}

struct MBDeepRenderer {
    pthread_mutex_t lock;
    DeepOrbit *cur;          // current orbit snapshot (may be NULL)
    uint64_t latest_gen;     // newest generation seen (guarded by lock)
    // Offset (view center - reference point) for latest_gen, in complex units.
    FloatExp gen_ref_dx, gen_ref_dy;
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

// Compute (view center - orbit reference) in HP; returned as FloatExp so it
// stays exact at any zoom depth.
static void compute_ref_offset(const MBViewState *view, const DeepOrbit *orbit,
                               FloatExp *out_dx, FloatExp *out_dy) {
    mpfr_prec_t prec = mp_required_precision_log10(mb_view_zoom_log10(view)) + 64;

    MPReal a, b, d;
    mp_real_init_set_str(&a, view->center_x_str, prec);
    mp_real_init_set_str(&b, orbit->orbit.ref_cx_str, prec);
    mp_real_init(&d, prec);
    mp_real_sub(&d, &a, &b);
    mp_real_get_fx(&d, &out_dx->m, &out_dx->e);

    mp_real_set_str(&a, view->center_y_str);
    mp_real_set_str(&b, orbit->orbit.ref_cy_str);
    mp_real_sub(&d, &a, &b);
    mp_real_get_fx(&d, &out_dy->m, &out_dy->e);

    mp_real_clear(&a);
    mp_real_clear(&b);
    mp_real_clear(&d);
}

// Abort polling for orbit builds: a deep orbit takes seconds; the moment the
// UI announces a newer generation the build must stop (it holds the renderer
// mutex, blocking every tile worker).
typedef struct {
    MBDeepRenderer *r;
    uint64_t gen;
} OrbitAbortCtx;

static bool orbit_should_abort(void *p) {
    OrbitAbortCtx *a = (OrbitAbortCtx *)p;
    return atomic_load(&a->r->announced_gen) > a->gen;
}

// Build a new orbit at the view center. Returns NULL on failure or abort.
// Called with r->lock held; `old` (r->cur) may serve as a continuation
// source when the center is identical and only the budget grew.
static DeepOrbit *build_orbit(MBDeepRenderer *r, uint64_t generation,
                              const MBViewState *view, uint32_t max_iter) {
    DeepOrbit *o = calloc(1, sizeof(DeepOrbit));
    if (!o) return NULL;

    mpfr_prec_t prec = mp_required_precision_log10(mb_view_zoom_log10(view));
    if (ref_orbit_hp_init(&o->orbit, max_iter, (uint32_t)prec) != 0) {
        free(o);
        return NULL;
    }

    OrbitAbortCtx abort_ctx = {r, generation};

    // Zooming in on the same center only grows the iteration budget: resume
    // the previous orbit from its saved final state instead of recomputing
    // hundreds of thousands of MPFR iterations from zero. "Same center" is
    // decided by value (the strings gain digits as the zoom deepens).
    bool continued = false;
    DeepOrbit *old = r->cur;
    if (old && old->orbit.valid && old->orbit.cont_z &&
        old->orbit.precision == (uint32_t)prec &&
        old->orbit.max_iter < max_iter) {
        FloatExp dx, dy;
        compute_ref_offset(view, old, &dx, &dy);
        if (fx_is_zero(dx) && fx_is_zero(dy)) {
            continued = ref_orbit_hp_continue(&o->orbit, &old->orbit,
                                              orbit_should_abort, &abort_ctx) == 0;
        }
    }

    if (!continued) {
        ref_orbit_hp_compute_cancellable(&o->orbit,
                                         view->center_x_str, view->center_y_str,
                                         orbit_should_abort, &abort_ctx);
    }
    if (!o->orbit.valid || o->orbit.escape_iter < 1) {
        ref_orbit_hp_cleanup(&o->orbit);
        free(o);
        return NULL;
    }

    // BLA table for iteration skipping. dc_max covers every pixel this orbit
    // can serve: the viewport diagonal plus the allowed reference drift.
    uint32_t ref_len = o->orbit.escape_iter + 1;
    if (ref_len > o->orbit.max_iter) ref_len = o->orbit.max_iter;
    double reach_px = hypot(view->viewport_width, view->viewport_height)
                      + MB_DEEP_MAX_REF_DRIFT_PX;
    FloatExp dc_max = fx_mul_d(mb_view_get_scale_fx(view), reach_px);
    if (mb_bla_build(&o->bla, o->orbit.z_real, o->orbit.z_imag,
                     ref_len, dc_max) != 0) {
        // Table allocation failed: render without skipping (correct, slower)
        mb_bla_free(&o->bla);
    }
    o->bla_dc_max = dc_max;

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
    return mb_deep_renderer_render_tile_strided(r, view, generation,
                                                tile_px, tile_py, tile_size,
                                                1, max_iter, settings, out);
}

// Shared core; exactly one of out_colors / out_iters is non-NULL.
static int render_tile_impl(MBDeepRenderer *r,
                            const MBViewState *view, uint64_t generation,
                            int tile_px, int tile_py, int tile_size,
                            int stride,
                            uint32_t max_iter,
                            const MBRenderSettings *settings,
                            PixelColor *out_colors, uint32_t *out_iters) {
    if (!r || !view || (!out_colors && !out_iters) || tile_size <= 0) return MB_DEEP_ERROR;
    if (stride <= 0 || tile_size % stride != 0) return MB_DEEP_ERROR;
    if (view->viewport_width <= 0 || view->viewport_height <= 0) return MB_DEEP_ERROR;

    double zoom_l10 = mb_view_zoom_log10(view);
    FloatExp scale = mb_view_get_scale_fx(view);
    if (fx_is_zero(scale)) return MB_DEEP_ERROR;

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
        FloatExp dx = fx_zero(), dy = fx_zero();

        if (r->cur && r->cur->computed_max_iter >= max_iter) {
            // The BLA radii were built assuming |dc| <= bla_dc_max. Zooming
            // OUT grows per-pixel |dc| beyond that bound and would let
            // merged entries skip outside their validity budget, so reuse is
            // only sound when the current reach still fits.
            double reach_px = hypot(view->viewport_width, view->viewport_height)
                              + MB_DEEP_MAX_REF_DRIFT_PX;
            FloatExp needed_dc_max = fx_mul_d(scale, reach_px);
            if (fx_cmp_abs(needed_dc_max, r->cur->bla_dc_max) <= 0) {
                compute_ref_offset(view, r->cur, &dx, &dy);
                FloatExp drift_limit = fx_mul_d(scale, MB_DEEP_MAX_REF_DRIFT_PX);
                if (fx_cmp_abs(dx, drift_limit) < 0 && fx_cmp_abs(dy, drift_limit) < 0) {
                    rebuild = false;
                }
            }
        }

        if (rebuild) {
            DeepOrbit *fresh = build_orbit(r, generation, view, max_iter);
            if (!fresh) {
                pthread_mutex_unlock(&r->lock);
                // Distinguish "the view moved on mid-build" from real failure
                return atomic_load(&r->announced_gen) > generation
                           ? MB_DEEP_STALE : MB_DEEP_ERROR;
            }
            deep_orbit_release(r->cur);
            r->cur = fresh;
            dx = fx_zero();
            dy = fx_zero();
        }

        r->latest_gen = generation;
        r->gen_ref_dx = dx;
        r->gen_ref_dy = dy;
    }

    DeepOrbit *orbit = r->cur;
    atomic_fetch_add(&orbit->refs, 1);
    FloatExp ref_dx = r->gen_ref_dx;
    FloatExp ref_dy = r->gen_ref_dy;

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
    bool use_fx = zoom_l10 > MB_DEEP_FX_LOG10;
    int out_dim = tile_size / stride;
    double sample_off = (stride - 1) / 2.0;  // sample block centers

    // GPU float-float fast path: full-resolution tiles inside the float
    // exponent window. Falls through to the CPU loops on any refusal.
    if (stride == 1 && zoom_l10 <= MB_DEEP_GPU_LOG10 &&
        atomic_load(&g_gpu_enabled) && gpu_df_available()) {
        size_t pixels = (size_t)tile_size * tile_size;
        uint32_t *iters = malloc(pixels * sizeof(uint32_t));
        float *z2s = malloc(pixels * sizeof(float));
        if (iters && z2s) {
            double dc0x = fx_to_d(fx_add(ref_dx, fx_mul_d(scale, tile_px - half_w)));
            double dc0y = fx_to_d(fx_add(ref_dy, fx_mul_d(scale, half_h - tile_py)));
            double step = fx_to_d(scale);

            if (gpu_perturb_df_tile(ref_re, ref_im, ref_len,
                                    dc0x, dc0y, step, -step,
                                    (uint32_t)tile_size, max_iter,
                                    iters, z2s) == 0) {
                for (size_t i = 0; i < pixels; i++) {
                    if (out_colors) {
                        color_from_iteration_ex(&out_colors[i], iters[i], z2s[i],
                                                max_iter, settings);
                    } else {
                        out_iters[i] = iters[i];
                    }
                }
                free(iters);
                free(z2s);
                deep_orbit_release(orbit);
                return MB_DEEP_OK;
            }
        }
        free(iters);
        free(z2s);
    }

    for (int ly = 0; ly < out_dim; ly++) {
        // Abort mid-tile if the UI announced a newer generation: at extreme
        // zoom a tile can take seconds, and the user has already moved on.
        if (atomic_load(&r->announced_gen) > generation) {
            deep_orbit_release(orbit);
            return MB_DEEP_STALE;
        }

        double py = tile_py + ly * stride + sample_off;
        // Screen py grows downward, imaginary axis grows upward.
        FloatExp dcy = fx_add(ref_dy, fx_mul_d(scale, half_h - py));
        double dcy_d = fx_to_d(dcy);

        for (int lx = 0; lx < out_dim; lx++) {
            double px = tile_px + lx * stride + sample_off;
            FloatExp dcx = fx_add(ref_dx, fx_mul_d(scale, px - half_w));

            float z2 = 0.0f;
            uint32_t iter;
            if (use_fx) {
                iter = perturb_cpu_pixel_fx_bla(ref_re, ref_im, ref_len,
                                                dcx, dcy, max_iter,
                                                &orbit->bla, &z2);
            } else {
                iter = perturb_cpu_pixel_bla(ref_re, ref_im, ref_len,
                                             fx_to_d(dcx), dcy_d, max_iter,
                                             &orbit->bla, &z2);
            }
            size_t idx = (size_t)ly * (size_t)out_dim + (size_t)lx;
            if (out_colors) {
                color_from_iteration_ex(&out_colors[idx], iter, z2, max_iter, settings);
            } else {
                out_iters[idx] = iter;
            }
        }
    }

    deep_orbit_release(orbit);
    return MB_DEEP_OK;
}

int mb_deep_renderer_render_tile_strided(MBDeepRenderer *r,
                                         const MBViewState *view, uint64_t generation,
                                         int tile_px, int tile_py, int tile_size,
                                         int stride,
                                         uint32_t max_iter,
                                         const MBRenderSettings *settings,
                                         PixelColor *out) {
    return render_tile_impl(r, view, generation, tile_px, tile_py, tile_size,
                            stride, max_iter, settings, out, NULL);
}

int mb_deep_renderer_probe_strided(MBDeepRenderer *r,
                                   const MBViewState *view, uint64_t generation,
                                   int tile_px, int tile_py, int tile_size,
                                   int stride,
                                   uint32_t max_iter,
                                   uint32_t *iters_out) {
    return render_tile_impl(r, view, generation, tile_px, tile_py, tile_size,
                            stride, max_iter, NULL, NULL, iters_out);
}
