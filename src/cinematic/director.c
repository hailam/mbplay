#include "director.h"
#include "nucleus.h"
#include "../perturbation/deep_render.h"
#include "../precision/view_hp.h"
#include "../precision/floatexp.h"
#include "../mandelbrot/mandelbrot.h"
#include "../color/color.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <dispatch/dispatch.h>
#include <stdatomic.h>

// Diagnostics for offline dive analysis (analyzer builds with -DMB_STEER_DEBUG)
#ifdef MB_STEER_DEBUG
#include <stdio.h>
#define STEER_LOG(...) fprintf(stderr, __VA_ARGS__)
#else
#define STEER_LOG(...) ((void)0)
#endif

// Steering probe: PROBE_GRID^2 samples over a square covering the frame.
// Fine enough that hairline filaments (the deep structure at depth) don't
// slip between samples too easily — they are invisible at ANY budget once
// they fall between cells.
#define PROBE_GRID 32
// Candidate next-window centers evaluated per axis (see steer_path).
#define STEER_CANDIDATES 17
// Candidate offsets reach +-STEER_REACH_FRACTION * span in BOTH axes. The
// zoom doubles per keyframe, so a feature at offset p is at 2(p - move)
// next frame: the trackable region is |p| < 2*move_max. Reach must exceed
// span/4 * (long/short ratio considerations) so that 2*reach covers the
// whole frame — otherwise structure in the frame's outer third can be held
// at the edge but never re-centered, and one probe-noise wobble loses it.
#define STEER_REACH_FRACTION 0.42
// Moves beyond this knee cost extra: normal tracking stays gentle (smooth
// camera), larger catch-up moves fire only on a decisive deep differential.
#define STEER_KNEE_FRACTION 0.25
// Retarget cap (fraction of the short span) for the no-escapes
// reverse-course backstop, where there is no window to score.
#define STEER_LOST_FRACTION 0.45
// Ceiling for the adaptive iteration budget (matches the app-wide deep
// render cap in mb_max_iter_for_zoom_log10).
#define STEER_ITER_CEIL 524288u

struct MBDirector {
    pthread_mutex_t lock;

    MBDeepRenderer *renderer;
    MBRenderSettings settings;
    int width, height;

    MBCineKeyframe slots[MB_CINE_SLOTS];

    // Dive path state (worker-owned between start/stop)
    MBViewState path;      // center = dive target; zoom set per keyframe
    int next_index;        // next keyframe index to render
    int playback_index;    // floor(playback zoom / step), under lock
    bool first_keyframe;   // entry frame renders unsteered (seamless entry)

    // Last steering move in pixels (worker-owned): reversed as a failsafe
    // when a probe finds no escaping samples at all.
    double last_move_x, last_move_y;
    bool last_move_valid;

    // Adaptive iteration budget (worker-owned): raised when probe counts
    // press against it, decayed when they fall far below. See steer_path.
    uint32_t iter_budget;

    // Visual RENDER budget (worker-owned): ~2x the p95 of escaping probe
    // counts. Probes must resolve the deepest fringe (they are 1024 cells
    // — the steering's eyes), but interior pixels burn the whole render
    // budget per frame: tying rendering to the fringe max makes boundary
    // orbits unaffordable. The >p95 sliver renders as set-black.
    uint32_t render_budget;

    // Capped probe fraction verified as REAL set (a budget doubling failed
    // to collapse it). Raising the budget past real interior is pure waste:
    // it never uncaps. Worker-owned.
    double real_frac;

    // Nucleus target lock (worker-owned): a FIXED minibrot coordinate the
    // camera flies toward geometrically. Field-following chases what the
    // probe sees and can be outrun by structure receding at the zoom rate;
    // a fixed coordinate cannot recede. See steer_nucleus.
    char target_x[MB_HP_COORD_STR_LEN];
    char target_y[MB_HP_COORD_STR_LEN];
    bool target_valid;
    uint32_t target_period;
    int retarget_cooldown;        // keyframes to skip Newton after a failure
    uint32_t retarget_min_period; // skip |z| dips at/below this when locking:
                                  // raised past a parent nucleus so the next
                                  // lock finds the EMBEDDED minibrot beyond it

    // ANCHOR: the last successfully locked nucleus, never cleared on veer.
    // When embedded periods outgrow the Newton budget (locks become
    // unaffordable at depth), the dive descends onto THIS minibrot's
    // boundary instead — an exact coordinate whose boundary is the whole
    // Mandelbrot boundary in miniature: infinitely deep at any zoom.
    char anchor_x[MB_HP_COORD_STR_LEN];
    char anchor_y[MB_HP_COORD_STR_LEN];
    bool anchor_valid;

    // Scratch (worker-owned). One tile buffer per viewport tile: the tiles
    // of a keyframe render CONCURRENTLY (the deep renderer is built for
    // concurrent tile calls), which is where keyframe throughput comes from.
    PixelColor **tile_scratch;   // tiles_total buffers
    int tiles_x, tiles_y;
    uint32_t *probe_scratch;
};

MBDirector *mb_director_create(int width, int height,
                               const MBRenderSettings *settings) {
    if (width <= 0 || height <= 0) return NULL;

    MBDirector *d = calloc(1, sizeof(MBDirector));
    if (!d) return NULL;

    d->width = width;
    d->height = height;
    d->settings = *settings;
    d->renderer = mb_deep_renderer_create();
    d->tiles_x = (width + MB_INTERACTIVE_TILE_SIZE - 1) / MB_INTERACTIVE_TILE_SIZE;
    d->tiles_y = (height + MB_INTERACTIVE_TILE_SIZE - 1) / MB_INTERACTIVE_TILE_SIZE;
    int tiles_total = d->tiles_x * d->tiles_y;
    d->tile_scratch = calloc((size_t)tiles_total, sizeof(PixelColor *));
    d->probe_scratch = malloc((size_t)PROBE_GRID * PROBE_GRID * sizeof(uint32_t));

    bool ok = d->renderer && d->tile_scratch && d->probe_scratch &&
              pthread_mutex_init(&d->lock, NULL) == 0;
    if (ok) {
        for (int i = 0; i < tiles_total; i++) {
            d->tile_scratch[i] = malloc((size_t)MB_INTERACTIVE_TILE_SIZE *
                                        MB_INTERACTIVE_TILE_SIZE * sizeof(PixelColor));
            if (!d->tile_scratch[i]) ok = false;
        }
    }
    if (!ok) {
        if (d->renderer) mb_deep_renderer_destroy(d->renderer);
        if (d->tile_scratch) {
            for (int i = 0; i < tiles_total; i++) free(d->tile_scratch[i]);
            free(d->tile_scratch);
        }
        free(d->probe_scratch);
        free(d);
        return NULL;
    }

    for (int i = 0; i < MB_CINE_SLOTS; i++) {
        d->slots[i].pixels = malloc((size_t)width * height * sizeof(PixelColor));
        if (!d->slots[i].pixels) {
            mb_director_destroy(d);
            return NULL;
        }
        d->slots[i].ready = false;
        d->slots[i].index = -1;
    }
    return d;
}

void mb_director_destroy(MBDirector *d) {
    if (!d) return;
    for (int i = 0; i < MB_CINE_SLOTS; i++) {
        free(d->slots[i].pixels);
    }
    if (d->renderer) mb_deep_renderer_destroy(d->renderer);
    if (d->tile_scratch) {
        for (int i = 0; i < d->tiles_x * d->tiles_y; i++) free(d->tile_scratch[i]);
        free(d->tile_scratch);
    }
    free(d->probe_scratch);
    pthread_mutex_destroy(&d->lock);
    free(d);
}

void mb_director_start(MBDirector *d, const MBViewState *seed) {
    pthread_mutex_lock(&d->lock);

    d->path = *seed;
    d->path.viewport_width = d->width;
    d->path.viewport_height = d->height;

    double z = mb_view_zoom_log10(seed);
    d->next_index = (int)floor(z / MB_CINE_STEP);
    if (d->next_index < 0) d->next_index = 0;
    d->playback_index = d->next_index;
    d->first_keyframe = true;
    d->last_move_valid = false;
    d->iter_budget = 0;
    d->render_budget = 0;
    d->real_frac = 0.0;
    d->target_valid = false;
    d->target_period = 0;
    d->retarget_cooldown = 0;
    d->retarget_min_period = 0;
    d->anchor_valid = false;

    for (int i = 0; i < MB_CINE_SLOTS; i++) {
        d->slots[i].ready = false;
        d->slots[i].index = -1;
    }

    pthread_mutex_unlock(&d->lock);
}

// Zoom depth where nucleus targeting engages. Shallower, window steering
// is reliable and nucleus locks degenerate (cardioid/bulb centers are
// featureless interior).
#define STEER_NUCLEUS_MIN_LOG10 3.0

// BOUNDARY ORBIT — the deep-dive endgame. Descend onto the anchor
// minibrot's boundary: hold its interior at a visual setpoint by moving
// radially along the EXACT anchor direction (toward when too little set
// shows, away when too much), plus a slow tangential drift that makes the
// descent a spiral. Anchored to an exact coordinate, valid at any depth,
// needs no further Newton locks. Returns false only without an anchor.
static bool steer_boundary_orbit(MBDirector *d, const MBViewState *pv,
                                 double capped_frac, int span, double reach,
                                 double *out_mx, double *out_my) {
    if (!d->anchor_valid) return false;

    MBViewState av = *pv;
    if (mb_view_hp_set_center(&av, d->anchor_x, d->anchor_y) != 0) {
        d->anchor_valid = false;
        return false;
    }
    FloatExp scale = mb_view_get_scale_fx(pv);
    FloatExp dRe, dIm;
    mb_view_hp_center_delta_fx(pv, &av, &dRe, &dIm);
    double ax = fx_to_d(fx_div(dRe, scale));
    double ay = fx_to_d(fx_div(dIm, scale));
    double dist = hypot(ax, ay);
    if (!(dist > 1e-9) || !isfinite(dist)) {
        // Sitting exactly on the nucleus: step off in any direction
        ax = 1.0;
        ay = 0.0;
        dist = 1.0;
    }
    double ux = ax / dist, uy = ay / dist;

    // PROPORTIONAL radial control on the visible set fraction. The
    // boundary-distance error doubles every keyframe like everything
    // else, so a dead-band controller silently lets it explode while
    // "in band" — correction must be continuous, every frame, with gain
    // above the 0.5 that stabilizes the doubling map. The capped
    // fraction is frame-relative, so the same law holds at any depth.
    double err = 0.28 - capped_frac;   // +: too little set, go toward
    double radial = 1.5 * err * span;
    double lim = capped_frac <= 0.001 ? 0.85 * span : reach;  // emergency
    if (radial > lim) radial = lim;
    if (radial < -lim) radial = -lim;
    if (radial > 0.5 * dist) radial = 0.5 * dist;   // don't overshoot
    // Tangential drift: the spiral
    double tang = 0.05 * span;

    double mx = radial * ux + tang * -uy;
    double my = radial * uy + tang * ux;
    double mag = hypot(mx, my);
    if (mag > lim) {
        mx *= lim / mag;
        my *= lim / mag;
    }
    *out_mx = mx;
    *out_my = my;
    STEER_LOG("[steer] ORBIT dist=%.0fpx capped=%.2f radial=%.0f\n",
              dist, capped_frac, radial);
    return true;
}

// NUCLEUS TARGETING — the deep-dive backbone. Lock the superstable center
// of the minibrot that owns the deepest visible neighborhood (period
// detection + Newton, see nucleus.c) and fly toward that FIXED coordinate.
// A fixed target cannot be outrun; en route the frame shows the filament
// cascade leading to it, on arrival the minibrot grows until it fills the
// resolved frame, then the next embedded structure is locked — the classic
// infinite minibrot descent. Returns true with a move when a target is
// held or freshly locked; false hands the frame to window steering.
static bool steer_nucleus(MBDirector *d, const MBViewState *pv,
                          const uint32_t *it, uint32_t max_iter,
                          double capped_frac,
                          int px0, int py0, int stride,
                          int span, double reach,
                          double *out_mx, double *out_my) {
    FloatExp scale = mb_view_get_scale_fx(pv);

    if (d->target_valid) {
        MBViewState tv = *pv;
        if (mb_view_hp_set_center(&tv, d->target_x, d->target_y) != 0) {
            d->target_valid = false;
        } else {
            FloatExp dRe, dIm;
            mb_view_hp_center_delta_fx(pv, &tv, &dRe, &dIm);
            double px = fx_to_d(fx_div(dRe, scale));
            double py_up = fx_to_d(fx_div(dIm, scale));
            double dist = hypot(px, py_up);
            if (dist > 4.0 * span) {
                d->target_valid = false;   // impossibly far: stale lock
            } else if ((dist < span / 4.0 && capped_frac > 0.05) ||
                       capped_frac > 0.25) {
                // VEER EARLY: the locked minibrot's interior has appeared.
                // Sitting atop a nucleus explodes iteration counts (the
                // budget chases them to the ceiling, the frame drowns in
                // set) — hand off to the next embedded target NOW, and
                // skip the parent's |z| dips so Newton cannot relock it.
                // The second clause fires MID-FLIGHT: on a long approach a
                // real minibrot balloons before the camera ever centers
                // it, and waiting for arrival lets it swallow the frame.
                d->target_valid = false;
                if (d->target_period > d->retarget_min_period) {
                    d->retarget_min_period = d->target_period;
                }
                STEER_LOG("[steer] VEER capped=%.2f dist=%.0f min_period=%u\n",
                          capped_frac, dist, d->retarget_min_period);
            } else {
                // Far targets get emergency catch-up speed: a feature at
                // offset p recedes to 2(p - move), so beyond 2*reach the
                // normal cap diverges. One briefly rougher composite
                // beats discarding a valid nucleus.
                double lim = dist > 1.5 * reach ? 0.85 * span : reach;
                double f = dist > lim ? lim / dist : 1.0;
                *out_mx = px * f;
                *out_my = py_up * f;
                STEER_LOG("[steer] FLY dist=%.0fpx capped=%.2f\n",
                          dist, capped_frac);
                return true;
            }
        }
    }

    if (d->retarget_cooldown > 0) {
        d->retarget_cooldown--;
        return false;
    }

    // Guess: the deepest ESCAPING cell — embedded structure whose nucleus
    // lies deeper than wherever we are. Cells hugging the capped interior
    // are dominated by the parent minibrot's atom (Newton just relocks
    // it), so prefer deep cells with no capped neighbor; fall back to the
    // global deepest if the frame has no such cell.
    uint32_t best = 0, best_any = 0;
    int bi = -1, bi_any = -1;
    for (int i = 0; i < PROBE_GRID * PROBE_GRID; i++) {
        uint32_t v = it[i];
        if (v >= max_iter) continue;
        if (v > best_any) {
            best_any = v;
            bi_any = i;
        }
        if (v > best) {
            int gx = i % PROBE_GRID, gy = i / PROBE_GRID;
            bool near_capped = false;
            for (int oy = -1; oy <= 1 && !near_capped; oy++) {
                for (int ox = -1; ox <= 1; ox++) {
                    int nxg = gx + ox, nyg = gy + oy;
                    if (nxg < 0 || nxg >= PROBE_GRID ||
                        nyg < 0 || nyg >= PROBE_GRID) continue;
                    if (it[nyg * PROBE_GRID + nxg] >= max_iter) {
                        near_capped = true;
                        break;
                    }
                }
            }
            if (!near_capped) {
                best = v;
                bi = i;
            }
        }
    }
    if (bi < 0) bi = bi_any;
    if (bi < 0) return false;
    uint32_t guess_count = it[bi];
    double sx = px0 + (bi % PROBE_GRID + 0.5) * stride;
    double sy = py0 + (bi / PROBE_GRID + 0.5) * stride;

    MBViewState gv = *pv;
    mb_view_hp_translate_fx(&gv,
                            fx_mul_d(scale, sx - pv->viewport_width / 2.0),
                            fx_mul_d(scale, pv->viewport_height / 2.0 - sy));

    char nx[MB_HP_COORD_STR_LEN], ny[MB_HP_COORD_STR_LEN];
    uint32_t period = 0;
    uint32_t maxp = max_iter * 2 < 250000u ? max_iter * 2 : 250000u;
    if (guess_count >= maxp) {
        // The guess's dip lies beyond the period-scan budget: the Newton
        // orbit cannot even reach it. Don't burn an MPFR orbit to fail.
        d->retarget_cooldown = 2;
        STEER_LOG("[steer] NEWTON-SKIP guess_count=%u maxp=%u\n",
                  guess_count, maxp);
        return false;
    }
    if (!mb_nucleus_find(gv.center_x_str, gv.center_y_str,
                         mb_view_zoom_log10(pv),
                         d->retarget_min_period, maxp, nx, ny, &period) ||
        period < 8) {
        d->retarget_cooldown = 4;
        STEER_LOG("[steer] NEWTON-FAIL guess_cell=%u floor=%u maxp=%u\n",
                  best_any, d->retarget_min_period, maxp);
        return false;
    }

    MBViewState tv = *pv;
    if (mb_view_hp_set_center(&tv, nx, ny) != 0) {
        d->retarget_cooldown = 4;
        return false;
    }
    FloatExp dRe, dIm;
    mb_view_hp_center_delta_fx(pv, &tv, &dRe, &dIm);
    double px = fx_to_d(fx_div(dRe, scale));
    double py_up = fx_to_d(fx_div(dIm, scale));
    double dist = hypot(px, py_up);
    // Reject relocking where we already are — and jump the floor past
    // this nucleus's HARMONICS too (its dips repeat at every multiple of
    // the period; climbing them one rung at a time never escapes).
    if (dist < span / 64.0) {
        uint32_t floor2 = period * 2 + 1;
        if (floor2 > d->retarget_min_period) d->retarget_min_period = floor2;
        d->retarget_cooldown = 4;
        STEER_LOG("[steer] RELOCK-SAME rejected, min_period=%u\n",
                  d->retarget_min_period);
        return false;
    }
    // Beyond the convergent flight envelope even with catch-up moves
    // (0.85*span, see the FLY branch): fixed point at 1.7*span, keep
    // margin. In practice this only rejects locks outside the frame.
    if (dist > 1.3 * span) {
        d->retarget_cooldown = 4;
        STEER_LOG("[steer] LOCK-TOO-FAR period=%u dist=%.0fpx\n", period, dist);
        return false;
    }

    memcpy(d->target_x, nx, MB_HP_COORD_STR_LEN);
    memcpy(d->target_y, ny, MB_HP_COORD_STR_LEN);
    memcpy(d->anchor_x, nx, MB_HP_COORD_STR_LEN);
    memcpy(d->anchor_y, ny, MB_HP_COORD_STR_LEN);
    d->anchor_valid = true;
    d->target_valid = true;
    d->target_period = period;
    STEER_LOG("[steer] LOCK period=%u dist=%.0fpx\n", period, dist);

    double f = dist > reach ? reach / dist : 1.0;
    *out_mx = px * f;
    *out_my = py_up * f;
    return true;
}

// Keep the dive framed on the SET BOUNDARY by an explicit window search,
// not by reactive nudging. Each keyframe the zoom doubles, so the next
// probe sees the half-size window around whatever center we choose now.
// Candidate centers span ±STEER_REACH_FRACTION*span; each candidate's
// half-size window is scored directly from the current probe grid (the
// probe square covers the frame, so extended windows lose at most a few
// edge cells). Pick the best-scoring window:
//
//   - a window is scored by its COUNT CONTRAST: the log2 spread between
//     the 10th and 90th percentile of its cells' iteration counts, with
//     budget-capped cells scored as extra-deep. The boundary is exactly
//     where counts span decades within a small region; flat exterior and
//     solid (pseudo-)interior both have zero contrast. Percentiles make
//     the score robust: capped DUST (noise cells at the budget contour,
//     <10% of a window) is excluded by construction, so the steering
//     tracks the persistent count gradient instead of chasing transient
//     specks — chasing dust is a random walk that loses the dive. A
//     window >90% capped has p90 == p10 == cap and scores zero, so the
//     framing self-regulates to a mixed, visually rich composition.
//
// Tracking guarantee: a feature at screen offset p moves to 2(p - move) by
// the next keyframe, so the convergent region is |p| < 2*move_max. With
// reach 0.42*span in both axes, 2*move_max exceeds the frame half-extents:
// anything visible in the frame can be re-centered, and since the seed
// frame contains the whole set, the dive never ends up featureless.
//
// "Interior" here means "did not escape within the budget", which includes
// deep-boundary points — equally good steering targets.
//
// Probes at the keyframe's own zoom with generation 2k, right before the
// tile pass at 2k+1: monotonic generations, and the probe's reference orbit
// is reused by the tiles (same view, drift within the reuse allowance).
static void steer_path(MBDirector *d, int keyframe_index) {
    MBViewState probe_view = d->path;
    mb_view_set_zoom_log10(&probe_view, keyframe_index * MB_CINE_STEP);

    // Probe a square covering the WHOLE frame (the long dimension): boundary
    // structure near the frame's edges must be visible to the steering, and
    // sampling slightly beyond the short dimension costs little while giving
    // the tracker a peek past the frame.
    int span = d->width < d->height ? d->width : d->height;
    int cover = d->width > d->height ? d->width : d->height;
    int probe_size = cover + (PROBE_GRID - cover % PROBE_GRID) % PROBE_GRID;
    int stride = probe_size / PROBE_GRID;
    if (stride < 1) return;

    // ADAPTIVE iteration budget. The formula budget grows linearly with
    // depth, but true counts near minibrot cascades grow much faster. The
    // steering must KEEP ITS TARGET RESOLVED: while a soft (budget-capped
    // pseudo-interior) blob covers the frame, the true deep core inside it
    // is invisible and drifts out of frame unseen — every historical dive
    // escape traces back to a budget step revealing that drift too late.
    // So keep the soft mass small: whenever more than ~10% of the probe
    // caps out with counts pressing the budget, double the budget and
    // re-probe (same view + generation, so orbit continuation makes the
    // deeper pass incremental). REAL interior never uncaps at any budget;
    // if a doubling fails to collapse the capped mass by ~30%, that mass
    // is real set — remember it (real_frac) and stop testing until the
    // capped fraction grows past it again.
    uint32_t floor_iter = mb_max_iter_for_zoom_log10(mb_view_zoom_log10(&probe_view));
    uint32_t max_iter = floor_iter > d->iter_budget ? floor_iter : d->iter_budget;
    max_iter = (max_iter + 2047u) & ~2047u;

    int px0 = (d->width - probe_size) / 2;
    int py0 = (d->height - probe_size) / 2;

    const int total = PROBE_GRID * PROBE_GRID;
    double capped_frac = 0.0;
    for (;;) {
        if (mb_deep_renderer_probe_strided(d->renderer, &probe_view,
                                           (uint64_t)keyframe_index * 2,
                                           px0, py0, probe_size, stride,
                                           max_iter, d->probe_scratch) != MB_DEEP_OK) {
            return;
        }
        int esc = 0;
        uint32_t vm = 0;
        for (int i = 0; i < total; i++) {
            uint32_t v = d->probe_scratch[i];
            if (v < max_iter) {
                esc++;
                if (v > vm) vm = v;
            }
        }
        double prev_frac = capped_frac;
        capped_frac = (double)(total - esc) / total;
        if (prev_frac > 0.0 && capped_frac > 0.7 * prev_frac) {
            // The doubling barely collapsed the capped mass: it is REAL set
            // (or beyond resolution). Remember that, and REVERT the budget:
            // raising past real interior buys nothing and every rendered
            // interior pixel would burn the inflated budget. The deeper
            // probe data is kept — escaped cells got extra resolution free.
            d->real_frac = capped_frac;
            max_iter /= 2;
            break;
        }
        bool pressing = esc == 0 || vm >= max_iter - max_iter / 4;
        // Trigger on ANY soft mass beyond known-real (a few cells): the
        // deepest spot is the steering target and must stay resolved even
        // when it is a single probe cell — a soft cell is indistinguishable
        // from noise and gets chased like dust.
        if (capped_frac <= d->real_frac + 2.5 / total || !pressing ||
            max_iter >= STEER_ITER_CEIL) {
            break;
        }
        max_iter *= 2;
    }

    // Re-derive stats at the FINAL threshold (the last probe may have run
    // at a deeper, reverted budget — its data stays valid above max_iter).
    int escaped = 0;
    uint32_t vmax = 0;
    for (int i = 0; i < total; i++) {
        uint32_t v = d->probe_scratch[i];
        if (v < max_iter) {
            escaped++;
            if (v > vmax) vmax = v;
        }
    }
    capped_frac = (double)(total - escaped) / total;
    if (capped_frac < d->real_frac) d->real_frac = capped_frac;

    // Decay when over-provisioned: capped cells are then REAL interior, and
    // every rendered interior pixel burns the whole budget.
    if (escaped > 0 && vmax < max_iter / 4) {
        uint32_t want = vmax * 2 > floor_iter ? vmax * 2 : floor_iter;
        want = (want + 2047u) & ~2047u;
        if (want < max_iter) max_iter = want;
    }
    d->iter_budget = max_iter;

    // Visual render budget: 2x the p95 escaping count (see struct field)
    if (escaped > 0) {
        uint32_t esc_sorted[PROBE_GRID * PROBE_GRID];
        int m = 0;
        for (int i = 0; i < total; i++) {
            if (d->probe_scratch[i] < max_iter) esc_sorted[m++] = d->probe_scratch[i];
        }
        for (int a = 1; a < m; a++) {   // insertion sort, m <= 1024
            uint32_t key = esc_sorted[a];
            int b = a - 1;
            while (b >= 0 && esc_sorted[b] > key) {
                esc_sorted[b + 1] = esc_sorted[b];
                b--;
            }
            esc_sorted[b + 1] = key;
        }
        uint64_t rb = (uint64_t)esc_sorted[m - 1 - m / 20] * 2u;
        if (rb < floor_iter) rb = floor_iter;
        if (rb > max_iter) rb = max_iter;
        d->render_budget = ((uint32_t)rb + 2047u) & ~2047u;
    } else {
        d->render_budget = max_iter;
    }

    const uint32_t *it = d->probe_scratch;

    double mx, my;

    if (escaped == 0) {
        // Swallowed by the (pseudo-)interior: counts carry no gradient, so
        // back out the way we came in until escapes reappear.
        if (!d->last_move_valid) return;
        double mag = hypot(d->last_move_x, d->last_move_y);
        if (mag < 1.0) return;
        double cap = STEER_LOST_FRACTION * span;
        mx = -d->last_move_x / mag * cap;
        my = -d->last_move_y / mag * cap;
    } else {
        // Deep enough, prefer a nucleus lock: geometric flight to a fixed
        // coordinate. When locks are unaffordable (embedded periods beyond
        // the Newton budget), descend onto the last locked minibrot's
        // boundary instead. Window steering handles shallow zoom and the
        // pre-first-lock phase.
        if (mb_view_zoom_log10(&probe_view) >= STEER_NUCLEUS_MIN_LOG10 &&
            (steer_nucleus(d, &probe_view, it, max_iter, capped_frac,
                           px0, py0, stride, span,
                           STEER_REACH_FRACTION * span, &mx, &my) ||
             steer_boundary_orbit(d, &probe_view, capped_frac, span,
                                  STEER_REACH_FRACTION * span, &mx, &my))) {
            goto move_chosen;
        }
        // Score every feasible next-window placement. Screen coords (y
        // down) throughout; flipped once at the end for the translate.
        // Per-cell log2 counts computed once; capped cells score as
        // extra-deep so real set presence reads as maximum depth.
        // Cell WEIGHTS are counts relative to the frame maximum: only
        // cells within a couple of doublings of the deepest matter, but
        // they ALL contribute — sharp enough to track a narrow nucleus
        // ridge (a region mean dilutes it), robust enough not to chase a
        // single noisy cell around a filament tangle (an argmax does).
        double cell_log[PROBE_GRID * PROBE_GRID];
        double cell_w[PROBE_GRID * PROBE_GRID];
        double cap_log = log2((double)max_iter * 4.0);
        double lmax = 0.0;
        for (int i = 0; i < PROBE_GRID * PROBE_GRID; i++) {
            uint32_t v = it[i];
            cell_log[i] = v >= max_iter ? cap_log : log2((double)v + 1.0);
            if (cell_log[i] > lmax) lmax = cell_log[i];
        }
        // When the set dominates the frame, capped cells must not act as
        // the depth attractor — a centroid over a dominant blob sits at
        // its CORE, and the dive sinks into featureless black interior.
        // Down-weight them so the deepest ESCAPING structure (the blob's
        // boundary ring) pulls the window instead.
        double cap_w = capped_frac > 0.35 ? 0.2 : 1.0;
        for (int i = 0; i < PROBE_GRID * PROBE_GRID; i++) {
            cell_w[i] = it[i] >= max_iter ? cap_w
                                          : exp2(cell_log[i] - lmax);
        }

        double reach = STEER_REACH_FRACTION * span;
        double knee = STEER_KNEE_FRACTION * span;
        double max_dist = hypot(reach, reach);
        double last_mag = hypot(d->last_move_x, d->last_move_y);
        double best_score = -1e30, best_ox = 0.0, best_oy = 0.0;

        for (int cy = 0; cy < STEER_CANDIDATES; cy++) {
            for (int cx = 0; cx < STEER_CANDIDATES; cx++) {
                double ox = (cx / (STEER_CANDIDATES - 1.0) * 2.0 - 1.0) * reach;
                double oy = (cy / (STEER_CANDIDATES - 1.0) * 2.0 - 1.0) * reach;
                double wcx = d->width / 2.0 + ox;
                double wcy = d->height / 2.0 + oy;

                double vals[PROBE_GRID * PROBE_GRID];
                int n = 0, n_int = 0;
                double sw = 0.0, swl = 0.0, swx = 0.0, swy = 0.0;
                for (int gy = 0; gy < PROBE_GRID; gy++) {
                    double sy = py0 + (gy + 0.5) * stride;
                    if (fabs(sy - wcy) > d->height / 4.0) continue;
                    for (int gx = 0; gx < PROBE_GRID; gx++) {
                        double sx = px0 + (gx + 0.5) * stride;
                        if (fabs(sx - wcx) > d->width / 4.0) continue;
                        int i = gy * PROBE_GRID + gx;
                        vals[n++] = cell_log[i];
                        if (it[i] >= max_iter) n_int++;
                        sw += cell_w[i];
                        swl += cell_w[i] * cell_log[i];
                        swx += cell_w[i] * (sx - wcx);
                        swy += cell_w[i] * (sy - wcy);
                    }
                }
                if (n == 0 || sw <= 0.0) continue;

                // Insertion sort (n is small)
                for (int a = 1; a < n; a++) {
                    double key = vals[a];
                    int b = a - 1;
                    while (b >= 0 && vals[b] > key) {
                        vals[b + 1] = vals[b];
                        b--;
                    }
                    vals[b + 1] = key;
                }
                // Contrast (p90 - p10, in doublings of iteration count)
                // keeps a shallow reference in frame for visual shape.
                double contrast = vals[n - 1 - n / 10] - vals[n / 10];
                // DEEP PULL: count-weighted mean log depth, discounted by
                // how far the weighted centroid sits from the window
                // center. The only feature that persists under indefinite
                // zoom is a boundary point; its neighborhood is the
                // near-maximal count mass, and the budget policy above
                // keeps that mass resolved.
                double half_w = d->width / 4.0, half_h = d->height / 4.0;
                double center_off = hypot((swx / sw) / half_w,
                                          (swy / sw) / half_h);
                double deep = swl / sw - 0.5 * center_off;

                double m = hypot(ox, oy);
                double pen = 0.30 * (m / max_dist);
                if (m > knee) pen += 7.0 * (m - knee) / span;

                // Direction hysteresis: competing deep regions on opposite
                // sides make the argmax flip-flop, wasting the chase budget
                // on dither. Reward continuing the established course.
                double hyst = 0.0;
                if (d->last_move_valid && last_mag > 1.0 && m > 1.0) {
                    double cosang = (ox * d->last_move_x - oy * d->last_move_y)
                                    / (m * last_mag);
                    double s = m / knee;
                    hyst = 0.25 * cosang * (s < 1.0 ? s : 1.0);
                }

                double fint = (double)n_int / n;
                double score = deep + 0.35 * contrast + hyst
                             - 0.6 * fabs(fint - 0.30)
                             - pen;
                // Hard guard: interior-heavy windows are near-black on
                // screen. The gentle composition bias above cannot stop
                // the depth attractor from picking them when the deepest
                // escaping cells sit inside an interior channel.
                if (fint > 0.55) score -= 4.0 * (fint - 0.55);

                if (score > best_score) {
                    best_score = score;
                    best_ox = ox;
                    best_oy = oy;
                }
            }
        }

        mx = best_ox;
        my = -best_oy;   // screen y-down -> translate y-up

        // PURSUIT: a structure receding at the zoom rate outruns a greedy
        // centroid chase — the probe only sees the visible TAIL of the
        // deep field (its core can be a hairline between probe samples),
        // so the window move systematically under-leads. When this move
        // agrees with the previous course, lead the target by carrying
        // the previous move forward; cap at reach. Disengages on course
        // change or once the target is re-centered (tiny window move).
        if (d->last_move_valid) {
            double lm = hypot(d->last_move_x, d->last_move_y);
            double bm = hypot(mx, my);
            if (lm > 1.0 && bm > 1.0) {
                double cosang = (mx * d->last_move_x + my * d->last_move_y)
                                / (lm * bm);
                if (cosang > 0.5) {
                    mx += d->last_move_x * cosang;
                    my += d->last_move_y * cosang;
                    // Escaping a dominant blob needs catch-up speed: its
                    // boundary ring recedes at the zoom rate.
                    double cap = capped_frac > 0.35 ? 0.6 * span : reach;
                    double mm = hypot(mx, my);
                    if (mm > cap) {
                        mx *= cap / mm;
                        my *= cap / mm;
                    }
                }
            }
        }

#ifdef MB_STEER_DEBUG
        {
            int g_int = PROBE_GRID * PROBE_GRID - escaped;
            // re-score the chosen window for the log
            double wcx = d->width / 2.0 + best_ox, wcy = d->height / 2.0 + best_oy;
            int n = 0, n_int = 0;
            for (int gy = 0; gy < PROBE_GRID; gy++) {
                double sy = py0 + (gy + 0.5) * stride;
                if (fabs(sy - wcy) > d->height / 4.0) continue;
                for (int gx = 0; gx < PROBE_GRID; gx++) {
                    double sx = px0 + (gx + 0.5) * stride;
                    if (fabs(sx - wcx) > d->width / 4.0) continue;
                    n++;
                    if (it[gy * PROBE_GRID + gx] >= max_iter) n_int++;
                }
            }
            STEER_LOG("[steer] k=%d budget=%u grid_int=%d/%d move=(%+.0f,%+.0f) "
                      "win_f=%.2f score=%.2f vmax=%u\n",
                      keyframe_index, max_iter, g_int, PROBE_GRID * PROBE_GRID,
                      best_ox, best_oy, n ? (double)n_int / n : -1.0,
                      best_score, vmax);
        }
#endif
    }

move_chosen:
    d->last_move_x = mx;
    d->last_move_y = my;
    d->last_move_valid = true;

    FloatExp scale = mb_view_get_scale_fx(&probe_view);
    mb_view_hp_translate_fx(&d->path,
                            fx_mul_d(scale, mx),
                            fx_mul_d(scale, my));
}

int mb_director_render_next(MBDirector *d) {
    pthread_mutex_lock(&d->lock);

    int k = d->next_index;
    if (k >= d->playback_index + MB_CINE_AHEAD) {
        pthread_mutex_unlock(&d->lock);
        return -1;   // pipeline full
    }

    MBCineKeyframe *slot = &d->slots[k % MB_CINE_SLOTS];
    slot->ready = false;
    slot->index = k;

    pthread_mutex_unlock(&d->lock);

    // Keep the path's OWN zoom current before any HP mutation: digit
    // preservation in view_hp derives its precision from the view's zoom,
    // and a path stuck at the seed zoom silently truncates deep-zoom
    // moves to zero — the camera freezes past ~1e17 while the world zooms
    // on, which reads as "the dive sailed into a plain area".
    mb_view_set_zoom_log10(&d->path, k * MB_CINE_STEP);

    // Steering happens outside the lock (probes cost real time). The path is
    // worker-owned, so no other thread touches it. Only the entry keyframe
    // renders unsteered, so the movie starts exactly at the user's view.
    if (!d->first_keyframe) {
        steer_path(d, k);
    }

    MBViewState view = d->path;
    mb_view_set_zoom_log10(&view, k * MB_CINE_STEP);
    view.high_precision_mode = mb_view_needs_high_precision(&view);

    // Visual budget from the steering probe: without it, regions whose
    // true counts outrun the formula render as featureless budget-black;
    // capped above at the p95-derived render budget so real-interior
    // pixels don't burn the (much deeper) probe budget every frame.
    uint32_t max_iter = mb_max_iter_for_zoom_log10(mb_view_zoom_log10(&view));
    if (d->render_budget && max_iter < d->render_budget) {
        max_iter = d->render_budget;
    }
    max_iter = (max_iter + 2047u) & ~2047u;

    // Render the frame's tiles CONCURRENTLY (the deep renderer shares one
    // refcounted reference orbit across threads; the first tile builds or
    // reuses it, the rest proceed in parallel). Keyframe throughput scales
    // with cores, which is what sets the maximum sustainable zoom speed.
    int ts = MB_INTERACTIVE_TILE_SIZE;
    size_t tiles_total = (size_t)d->tiles_x * d->tiles_y;
    _Atomic int failed = 0;
    _Atomic int *failedp = &failed;   // blocks capture by const copy

    // Below the deep threshold, doubles address every pixel exactly: render
    // tiles directly with the SIMD pair path — no reference orbit, no BLA
    // lookup overhead. Perturbation takes over where doubles cannot.
    bool shallow = view.zoom_level < MB_DEEP_ZOOM_THRESHOLD;

    MBDirector *dd = d;
    MBViewState *viewp = &view;
    MBCineKeyframe *slotp = slot;
    uint32_t mi = max_iter;
    int kk = k;
    dispatch_apply(tiles_total,
                   dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0),
                   ^(size_t i) {
        if (atomic_load(failedp)) return;
        int tx = (int)(i % dd->tiles_x);
        int ty = (int)(i / dd->tiles_x);
        PixelColor *scratch = dd->tile_scratch[i];

        if (shallow) {
            double scale = mb_view_get_scale(viewp);
            double half_w = dd->width / 2.0;
            double half_h = dd->height / 2.0;
            for (int ly = 0; ly < ts; ly++) {
                int py = ty * ts + ly;
                // Screen py grows downward, imaginary axis grows upward
                double cy = viewp->center_y + (half_h - py) * scale;
                for (int lx = 0; lx < ts; lx += 2) {
                    int px = tx * ts + lx;
                    double cx0 = viewp->center_x + (px - half_w) * scale;
                    double cx1 = viewp->center_x + (px + 1 - half_w) * scale;
                    unsigned int i0, i1;
                    float z0, z1;
                    mb_compute_pair_smooth(cx0, cy, cx1, cy, mi, &i0, &i1, &z0, &z1);
                    color_from_iteration_ex(&scratch[(size_t)ly * ts + lx],
                                            i0, z0, mi, &dd->settings);
                    color_from_iteration_ex(&scratch[(size_t)ly * ts + lx + 1],
                                            i1, z1, mi, &dd->settings);
                }
            }
        } else {
            int rc = mb_deep_renderer_render_tile(dd->renderer, viewp,
                                                  (uint64_t)kk * 2 + 1,
                                                  tx * ts, ty * ts, ts,
                                                  mi, &dd->settings, scratch);
            if (rc != MB_DEEP_OK) {
                atomic_store(failedp, 1);
                return;
            }
        }
        int copy_w = dd->width - tx * ts;
        if (copy_w > ts) copy_w = ts;
        int copy_h = dd->height - ty * ts;
        if (copy_h > ts) copy_h = ts;
        for (int row = 0; row < copy_h; row++) {
            memcpy(&slotp->pixels[(size_t)(ty * ts + row) * dd->width + tx * ts],
                   &scratch[(size_t)row * ts],
                   (size_t)copy_w * sizeof(PixelColor));
        }
    });

    if (atomic_load(&failed)) {
        return -1;
    }

    pthread_mutex_lock(&d->lock);
    slot->view = view;
    slot->max_iter = max_iter;
    slot->ready = true;
    d->next_index = k + 1;
    d->first_keyframe = false;
    pthread_mutex_unlock(&d->lock);

    return k;
}

void mb_director_lock_frames(MBDirector *d, double zoom_log10,
                             const MBCineKeyframe **lo, const MBCineKeyframe **hi) {
    pthread_mutex_lock(&d->lock);

    int k = (int)floor(zoom_log10 / MB_CINE_STEP);
    if (k < 0) k = 0;
    d->playback_index = k;

    const MBCineKeyframe *a = &d->slots[k % MB_CINE_SLOTS];
    const MBCineKeyframe *b = &d->slots[(k + 1) % MB_CINE_SLOTS];
    *lo = (a->ready && a->index == k) ? a : NULL;
    *hi = (b->ready && b->index == k + 1) ? b : NULL;
}

const MBCineKeyframe *mb_director_frame_at(MBDirector *d, int index) {
    if (index < 0) return NULL;
    const MBCineKeyframe *s = &d->slots[index % MB_CINE_SLOTS];
    return (s->ready && s->index == index) ? s : NULL;
}

void mb_director_unlock_frames(MBDirector *d) {
    pthread_mutex_unlock(&d->lock);
}

double mb_director_ready_log10(MBDirector *d) {
    pthread_mutex_lock(&d->lock);
    // Highest contiguous ready index starting at playback
    int k = d->playback_index;
    while (true) {
        const MBCineKeyframe *s = &d->slots[k % MB_CINE_SLOTS];
        if (!(s->ready && s->index == k)) break;
        k++;
    }
    pthread_mutex_unlock(&d->lock);
    return (double)k * MB_CINE_STEP;
}
