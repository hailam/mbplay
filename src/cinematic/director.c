#include "director.h"
#include "../perturbation/deep_render.h"
#include "../precision/view_hp.h"
#include "../precision/floatexp.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

// Steering probe: PROBE_GRID^2 samples over the central square of the next
// keyframe's view.
#define PROBE_GRID 24
// Max retarget per keyframe, as a fraction of the viewport span. The zoom
// doubles per keyframe, so a feature at pixel offset r drifts to 2(r - move)
// by the next keyframe: tracking converges only when move > r/2, i.e. this
// cap can hold features within half the central span. Kept at 1/4 so
// consecutive keyframes still overlap enough for clean compositing.
#define STEER_MAX_FRACTION 0.25
// When the boundary has drifted far off-center ("lost"), allow one larger
// catch-up move — a briefly rougher composite beats diving into a void.
#define STEER_LOST_FRACTION 0.45

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

    // Scratch (worker-owned)
    PixelColor *tile_scratch;
    uint32_t *probe_scratch;
    uint32_t *sort_scratch;
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
    d->tile_scratch = malloc((size_t)MB_INTERACTIVE_TILE_SIZE * MB_INTERACTIVE_TILE_SIZE * sizeof(PixelColor));
    d->probe_scratch = malloc((size_t)PROBE_GRID * PROBE_GRID * sizeof(uint32_t));
    d->sort_scratch = malloc((size_t)PROBE_GRID * PROBE_GRID * sizeof(uint32_t));

    bool ok = d->renderer && d->tile_scratch && d->probe_scratch && d->sort_scratch &&
              pthread_mutex_init(&d->lock, NULL) == 0;
    if (!ok) {
        if (d->renderer) mb_deep_renderer_destroy(d->renderer);
        free(d->tile_scratch);
        free(d->probe_scratch);
        free(d->sort_scratch);
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
    free(d->tile_scratch);
    free(d->probe_scratch);
    free(d->sort_scratch);
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

    for (int i = 0; i < MB_CINE_SLOTS; i++) {
        d->slots[i].ready = false;
        d->slots[i].index = -1;
    }

    pthread_mutex_unlock(&d->lock);
}

// Keep the dive center pinned to the SET BOUNDARY — the only thing that has
// structure at every depth. Targeting rules, in order of preference:
//
//   1. An INTERFACE sample: an escaping sample with an interior neighbor.
//      The boundary passes between them, so aiming there keeps both classes
//      in frame forever. Among interface samples, prefer the highest
//      iteration count (tighter spirals), biased toward the frame center.
//   2. No interior anywhere (pure exterior — a "void"): iteration counts are
//      a potential function that rises toward the set, so chase the maximum
//      count with an enlarged catch-up cap until the boundary re-enters.
//   3. No escapes anywhere (deep interior): counts carry no gradient; back
//      out toward the last known boundary direction is impossible without
//      history, so hold course — rule 1 prevents ever getting here.
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

    uint32_t max_iter = mb_max_iter_for_zoom_log10(mb_view_zoom_log10(&probe_view));
    max_iter = (max_iter + 2047u) & ~2047u;

    int px0 = (d->width - probe_size) / 2;
    int py0 = (d->height - probe_size) / 2;
    if (mb_deep_renderer_probe_strided(d->renderer, &probe_view,
                                       (uint64_t)keyframe_index * 2,
                                       px0, py0, probe_size, stride,
                                       max_iter, d->probe_scratch) != MB_DEEP_OK) {
        return;
    }

    const uint32_t *it = d->probe_scratch;
    double half_grid = (PROBE_GRID - 1) / 2.0;

    // Classify: "interior" here means "did not escape within the budget" —
    // which includes deep-boundary points, so it grows as the zoom outruns
    // the budget. The regulator below holds its frame fraction steady.
    int interior = 0, escaped = 0;
    for (int i = 0; i < PROBE_GRID * PROBE_GRID; i++) {
        if (it[i] >= max_iter) interior++;
        else d->sort_scratch[escaped++] = it[i];
    }

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
        double f = (double)interior / (PROBE_GRID * PROBE_GRID);
        bool far_off = f < 0.12 || f > 0.70;

        // The attractor is the iteration-count contour JUST OUTSIDE the set
        // (a high quantile of the escaped counts). It is strictly exterior,
        // so aiming at it never sinks into the (pseudo-)interior, yet it
        // hugs the boundary, so approaching it from a void pulls the set
        // back into frame. No quantile switching: the same contour attracts
        // correctly from both sides.
        for (int i = 1; i < escaped; i++) {   // insertion sort (<=576 items)
            uint32_t v = d->sort_scratch[i];
            int j = i - 1;
            while (j >= 0 && d->sort_scratch[j] > v) {
                d->sort_scratch[j + 1] = d->sort_scratch[j];
                j--;
            }
            d->sort_scratch[j + 1] = v;
        }
        uint32_t contour = d->sort_scratch[(int)(0.9 * (escaped - 1))];

        double best_score = -1.0;
        int best_x = 0, best_y = 0;
        for (int gy = 0; gy < PROBE_GRID; gy++) {
            for (int gx = 0; gx < PROBE_GRID; gx++) {
                uint32_t v = it[gy * PROBE_GRID + gx];
                if (v >= max_iter) continue;

                double score;
                double dist = v > contour ? (double)(v - contour)
                                          : (double)(contour - v);
                if (f < 0.12) {
                    // Void: the count field is nearly flat and rises toward
                    // the set — pure gradient chase (center bias would pin
                    // the dive to a featureless middle).
                    score = (double)v;
                } else if (f > 0.70) {
                    // Drowning in (pseudo-)interior: escaped samples sit
                    // toward the frame edge; ride the contour out to them.
                    // No center bias (it points the wrong way), and NOT the
                    // max count (that walks deeper into the blob).
                    score = 1.0 / (1.0 + dist / (contour * 0.05 + 1.0));
                } else {
                    double ddx = (gx - half_grid) / half_grid;
                    double ddy = (gy - half_grid) / half_grid;
                    double bias = 1.0 / (1.0 + 0.5 * (ddx * ddx + ddy * ddy));
                    score = bias / (1.0 + dist / (contour * 0.05 + 1.0));
                }
                if (score > best_score) {
                    best_score = score;
                    best_x = gx;
                    best_y = gy;
                }
            }
        }

        double off_x = px0 + (best_x + 0.5) * stride - d->width / 2.0;
        double off_y_up = d->height / 2.0 - (py0 + (best_y + 0.5) * stride);

        // Move 3/4 of the way (geometric convergence against the 2x zoom
        // per keyframe), with a larger cap when far off equilibrium.
        double cap = (far_off ? STEER_LOST_FRACTION : STEER_MAX_FRACTION) * span;
        mx = off_x * 0.75;
        my = off_y_up * 0.75;
        double mag = hypot(mx, my);
        if (mag > cap) {
            mx *= cap / mag;
            my *= cap / mag;
        }
    }

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

    // Steering happens outside the lock (probes cost real time). The path is
    // worker-owned, so no other thread touches it. Only the entry keyframe
    // renders unsteered, so the movie starts exactly at the user's view.
    if (!d->first_keyframe) {
        steer_path(d, k);
    }

    MBViewState view = d->path;
    mb_view_set_zoom_log10(&view, k * MB_CINE_STEP);
    view.high_precision_mode = mb_view_needs_high_precision(&view);

    uint32_t max_iter = mb_max_iter_for_zoom_log10(mb_view_zoom_log10(&view));
    max_iter = (max_iter + 2047u) & ~2047u;

    // Render the full frame as tiles into the slot buffer
    int ts = MB_INTERACTIVE_TILE_SIZE;
    for (int ty = 0; ty * ts < d->height; ty++) {
        for (int tx = 0; tx * ts < d->width; tx++) {
            int rc = mb_deep_renderer_render_tile(d->renderer, &view,
                                                  (uint64_t)k * 2 + 1,
                                                  tx * ts, ty * ts, ts,
                                                  max_iter, &d->settings,
                                                  d->tile_scratch);
            if (rc != MB_DEEP_OK) {
                return -1;
            }
            int copy_w = d->width - tx * ts;
            if (copy_w > ts) copy_w = ts;
            int copy_h = d->height - ty * ts;
            if (copy_h > ts) copy_h = ts;
            for (int row = 0; row < copy_h; row++) {
                memcpy(&slot->pixels[(size_t)(ty * ts + row) * d->width + tx * ts],
                       &d->tile_scratch[(size_t)row * ts],
                       (size_t)copy_w * sizeof(PixelColor));
            }
        }
    }

    pthread_mutex_lock(&d->lock);
    slot->view = view;
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
