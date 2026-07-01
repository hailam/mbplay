// Unit tests for the deep-zoom pipeline: CPU perturbation, HP view-state
// operations, the deep tile renderer, and zoom-limit plumbing.
//
// Build & run:  xmake build mandelbrot_tests && xmake run mandelbrot_tests

#include "../src/config.h"
#include "../src/mandelbrot/mandelbrot.h"
#include "../src/perturbation/perturbation.h"
#include "../src/perturbation/perturb_cpu.h"
#include "../src/perturbation/deep_render.h"
#include "../src/precision/mp_real.h"
#include "../src/precision/mp_complex.h"
#include "../src/precision/view_hp.h"
#include "../src/color/palettes.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static int tests_run = 0;
static int tests_failed = 0;

#define CHECK(cond, ...) do { \
    tests_run++; \
    if (!(cond)) { \
        tests_failed++; \
        printf("FAIL %s:%d: ", __func__, __LINE__); \
        printf(__VA_ARGS__); \
        printf("\n"); \
    } \
} while (0)

// =============================================================================
// Direct MPFR iteration (ground truth for deep-zoom tests)
// =============================================================================

// Same iteration-count convention as mb_compute_point / perturb_cpu_pixel.
static unsigned int direct_iterate_mpfr(const char *cx_str, const char *cy_str,
                                        double dcx, double dcy,
                                        unsigned int max_iter, unsigned int prec) {
    MPComplex c, z, z_new;
    MPReal norm;
    mp_complex_init_set_str(&c, cx_str, cy_str, prec);
    mp_complex_init(&z, prec);
    mp_complex_init(&z_new, prec);
    mp_real_init(&norm, prec);

    // c += (dcx, dcy)
    mp_real_add_d(&c.re, &c.re, dcx);
    mp_real_add_d(&c.im, &c.im, dcy);

    mp_complex_set_d(&z, 0.0, 0.0);

    unsigned int iteration = 0;
    while (iteration < max_iter) {
        mp_complex_norm_sqr(&norm, &z);
        if (mp_real_cmp_d(&norm, 4.0) >= 0) break;
        mp_complex_mandelbrot_iter(&z_new, &z, &c);
        mp_complex_set(&z, &z_new);
        iteration++;
    }

    mp_complex_clear(&c);
    mp_complex_clear(&z);
    mp_complex_clear(&z_new);
    mp_real_clear(&norm);
    return iteration;
}

// =============================================================================
// Tests
// =============================================================================

// Perturbation with double deltas must match direct double iteration at
// moderate zoom, where both are exact.
static void test_perturb_matches_direct_double(void) {
    const double center_x = -0.7436438870372;
    const double center_y = 0.1318259043;
    const double zoom = 1e6;
    const unsigned int max_iter = 3000;
    const double scale = (2.0 / 1024.0) / zoom;

    ReferenceOrbit orbit;
    CHECK(ref_orbit_init(&orbit, max_iter) == 0, "orbit init failed");
    ref_orbit_compute(&orbit, center_x, center_y);
    CHECK(orbit.valid && orbit.escape_iter >= 1, "orbit invalid");

    uint32_t ref_len = orbit.escape_iter + 1;
    if (ref_len > orbit.max_iter) ref_len = orbit.max_iter;

    int total = 0, exact = 0, close = 0;
    for (int py = -16; py <= 16; py += 2) {
        for (int px = -16; px <= 16; px += 2) {
            double dcx = px * scale;
            double dcy = py * scale;

            uint32_t it_p = perturb_cpu_pixel(orbit.z_real, orbit.z_imag, ref_len,
                                              dcx, dcy, max_iter, NULL);
            unsigned int it_d = mb_compute_point(center_x + dcx, center_y + dcy,
                                                 max_iter);
            total++;
            if (it_p == it_d) exact++;
            else if ((it_p > it_d ? it_p - it_d : it_d - it_p) <= 2) close++;
        }
    }

    // Both computations run in double and accumulate rounding that diverges
    // chaotically for boundary pixels with high iteration counts, so a hard
    // per-pixel bound is not meaningful here (the MPFR ground-truth test
    // below provides that). Statistically they must agree almost everywhere.
    CHECK(exact * 100 >= total * 95,
          "only %d/%d pixels match direct double exactly", exact, total);
    CHECK((total - exact - close) * 100 <= total * 3,
          "%d/%d pixels differ by more than 2 iterations", total - exact - close, total);

    ref_orbit_cleanup(&orbit);
}

// Rebasing correctness: reference escapes very early, pixel does not.
static void test_perturb_rebase_short_reference(void) {
    // Reference outside the set (escapes fast), probe pixel on the real axis
    // inside the set (never escapes).
    const double ref_x = 0.34, ref_y = 0.61;  // escapes after a few dozen iters
    const unsigned int max_iter = 2000;

    ReferenceOrbit orbit;
    CHECK(ref_orbit_init(&orbit, max_iter) == 0, "orbit init failed");
    ref_orbit_compute(&orbit, ref_x, ref_y);
    CHECK(orbit.valid, "orbit invalid");
    CHECK(orbit.escape_iter < 200, "expected an early-escaping reference (got %u)",
          orbit.escape_iter);

    uint32_t ref_len = orbit.escape_iter + 1;
    if (ref_len > orbit.max_iter) ref_len = orbit.max_iter;

    // A few probe offsets, including one that lands inside the set.
    const double probes[][2] = {
        {-0.6, -0.6},   // lands at (-0.26, 0.01): interior
        {-0.1, -0.05},  // exterior, escapes later than the reference
        {0.05, 0.02},   // near the reference
    };
    for (size_t i = 0; i < sizeof(probes) / sizeof(probes[0]); i++) {
        double dcx = probes[i][0], dcy = probes[i][1];
        uint32_t it_p = perturb_cpu_pixel(orbit.z_real, orbit.z_imag, ref_len,
                                          dcx, dcy, max_iter, NULL);
        unsigned int it_d = mb_compute_point(ref_x + dcx, ref_y + dcy, max_iter);
        uint32_t diff = it_p > it_d ? it_p - it_d : it_d - it_p;
        CHECK(diff <= 2, "probe %zu: perturb=%u direct=%u", i, it_p, it_d);
    }

    ref_orbit_cleanup(&orbit);
}

// Deep zoom: perturbation against an HP reference orbit must match direct
// MPFR iteration at zoom 1e30 (far beyond double precision).
static void test_perturb_matches_mpfr_at_1e30(void) {
    const char *cx_str = "-0.743643887037158704752191506114774";
    const char *cy_str = "0.131825904205311970493132056385139";
    const double zoom = 1e30;
    const unsigned int max_iter = 8000;
    const unsigned int prec = (unsigned int)mp_required_precision(zoom);
    const double scale = (2.0 / 1024.0) / zoom;

    ReferenceOrbitHP orbit;
    CHECK(ref_orbit_hp_init(&orbit, max_iter, prec) == 0, "hp orbit init failed");
    ref_orbit_hp_compute(&orbit, cx_str, cy_str);
    CHECK(orbit.valid && orbit.escape_iter >= 1, "hp orbit invalid");

    uint32_t ref_len = orbit.escape_iter + 1;
    if (ref_len > orbit.max_iter) ref_len = orbit.max_iter;

    const double offsets[][2] = {
        {0.0, 0.0}, {13.0, -7.0}, {-128.0, 96.0}, {200.0, 150.0}, {-3.0, -250.0},
    };
    for (size_t i = 0; i < sizeof(offsets) / sizeof(offsets[0]); i++) {
        double dcx = offsets[i][0] * scale;
        double dcy = offsets[i][1] * scale;

        uint32_t it_p = perturb_cpu_pixel(orbit.z_real, orbit.z_imag, ref_len,
                                          dcx, dcy, max_iter, NULL);
        unsigned int it_m = direct_iterate_mpfr(cx_str, cy_str, dcx, dcy,
                                                max_iter, prec);
        uint32_t diff = it_p > it_m ? it_p - it_m : it_m - it_p;
        CHECK(diff <= 1, "offset %zu: perturb=%u mpfr=%u (zoom 1e30)",
              i, it_p, it_m);
    }

    ref_orbit_hp_cleanup(&orbit);
}

// The deep tile renderer end to end: every sampled pixel must agree with
// direct MPFR iteration (interior <=> pure black), and staleness is honored.
static void test_deep_renderer_end_to_end(void) {
    MBViewState view;
    mb_view_state_init(&view, 512, 512);
    view.zoom_level = 1e18;  // far beyond double pixel precision
    mb_view_hp_set_center(&view,
                          "-0.743643887037158704752191506114774",
                          "0.131825904205311970493132056385139");
    view.high_precision_mode = true;

    MBDeepRenderer *r = mb_deep_renderer_create();
    CHECK(r != NULL, "renderer create failed");

    // Classic integer coloring: deterministic map from iteration count to
    // color, so tile pixels can be checked exactly against MPFR ground truth.
    // (Cosine palettes can produce pure black for escaped pixels, so "black
    // means interior" would not hold there.)
    MBRenderSettings settings = {
        .color_mode = MB_COLOR_MODE_CLASSIC,
        .palette_id = MB_PALETTE_CLASSIC,
        .antialiasing_enabled = false,
        .color_cycle_scale = 64.0f,
    };

    const int ts = 128;
    const int tile_px = 192, tile_py = 192;
    PixelColor *buf = malloc((size_t)ts * ts * sizeof(PixelColor));
    CHECK(buf != NULL, "tile alloc failed");

    // Render with an escalating iteration budget until the tile shows both
    // escaped (colored) and interior (black) pixels; the boundary region
    // guarantees both exist at a sufficient budget.
    unsigned int max_iter = mb_max_iter_for_zoom(view.zoom_level);
    uint64_t gen = 5;
    int black_idx = -1, colored_idx = -1;
    int rc = MB_DEEP_ERROR;
    for (int attempt = 0; attempt < 3; attempt++) {
        rc = mb_deep_renderer_render_tile(r, &view, gen, tile_px, tile_py, ts,
                                          max_iter, &settings, buf);
        CHECK(rc == MB_DEEP_OK, "render_tile returned %d", rc);

        black_idx = colored_idx = -1;
        for (int i = 0; i < ts * ts; i++) {
            int is_black = (buf[i].r == 0 && buf[i].g == 0 && buf[i].b == 0);
            if (is_black && black_idx < 0) black_idx = i;
            if (!is_black && colored_idx < 0) colored_idx = i;
            if (black_idx >= 0 && colored_idx >= 0) break;
        }
        if (black_idx >= 0 && colored_idx >= 0) break;
        max_iter *= 4;
        gen++;  // new budget = new render
    }
    CHECK(colored_idx >= 0, "no escaped pixel even at %u iterations", max_iter);
    CHECK(black_idx >= 0, "no interior pixel in tile");

    // Ground-truth check: sampled pixels must color exactly as the MPFR
    // iteration count dictates (allowing +-1 iteration for double rounding
    // at the escape boundary).
    double scale = mb_view_get_scale(&view);
    unsigned int prec = (unsigned int)mp_required_precision(view.zoom_level);
    const int checks[2] = {colored_idx, black_idx};
    for (int i = 0; i < 2; i++) {
        if (checks[i] < 0) continue;
        int lx = checks[i] % ts, ly = checks[i] / ts;
        int px = tile_px + lx, py = tile_py + ly;
        double dcx = (px - 256.0) * scale;
        double dcy = (256.0 - py) * scale;  // imaginary axis up

        unsigned int it_m = direct_iterate_mpfr(view.center_x_str, view.center_y_str,
                                                dcx, dcy, max_iter, prec);
        PixelColor c = buf[checks[i]];

        int matched = 0;
        for (int d = -1; d <= 1 && !matched; d++) {
            long candidate = (long)it_m + d;
            if (candidate < 0) continue;
            unsigned int it_c = (unsigned long)candidate > max_iter
                                    ? max_iter : (unsigned int)candidate;
            PixelColor expect;
            color_from_iteration_classic(&expect, it_c, max_iter);
            if (expect.r == c.r && expect.g == c.g && expect.b == c.b) matched = 1;
        }
        CHECK(matched,
              "pixel (%d,%d): color (%u,%u,%u) does not match MPFR iter=%u/%u",
              lx, ly, c.r, c.g, c.b, it_m, max_iter);
    }

    // Stale generation is rejected
    rc = mb_deep_renderer_render_tile(r, &view, gen - 1, 0, 0, ts,
                                      max_iter, &settings, buf);
    CHECK(rc == MB_DEEP_STALE, "stale generation not rejected (rc=%d)", rc);

    free(buf);
    mb_deep_renderer_destroy(r);
}

// The ceiling: at zoom 1e250 (1024-bit references, deltas ~1e-253) the
// rendered tile must still agree with direct MPFR iteration pixel by pixel.
static void test_pipeline_consistency_at_zoom_ceiling(void) {
    MBViewState view;
    mb_view_state_init(&view, 512, 512);
    view.zoom_level = 1e250;
    mb_view_hp_set_center(&view,
                          "-0.743643887037158704752191506114774",
                          "0.131825904205311970493132056385139");
    view.high_precision_mode = true;

    double scale = mb_view_get_scale(&view);
    CHECK(isnormal(scale), "scale degenerate at 1e250");

    MBDeepRenderer *r = mb_deep_renderer_create();
    CHECK(r != NULL, "renderer create failed");

    MBRenderSettings settings = {
        .color_mode = MB_COLOR_MODE_CLASSIC,
        .palette_id = MB_PALETTE_CLASSIC,
        .antialiasing_enabled = false,
        .color_cycle_scale = 64.0f,
    };

    const int ts = 32;
    const int tile_px = 240, tile_py = 240;
    PixelColor *buf = malloc((size_t)ts * ts * sizeof(PixelColor));
    CHECK(buf != NULL, "tile alloc failed");

    // Modest budget keeps the test fast; consistency matters, not aesthetics.
    unsigned int max_iter = 12000;
    int rc = mb_deep_renderer_render_tile(r, &view, 1, tile_px, tile_py, ts,
                                          max_iter, &settings, buf);
    CHECK(rc == MB_DEEP_OK, "render_tile returned %d at 1e250", rc);

    unsigned int prec = (unsigned int)mp_required_precision(view.zoom_level);
    CHECK(prec >= 896, "precision tier too small at 1e250: %u bits", prec);

    const int samples[][2] = { {0, 0}, {31, 31}, {16, 7} };
    for (size_t i = 0; i < sizeof(samples) / sizeof(samples[0]); i++) {
        int lx = samples[i][0], ly = samples[i][1];
        int px = tile_px + lx, py = tile_py + ly;
        double dcx = (px - 256.0) * scale;
        double dcy = (256.0 - py) * scale;

        unsigned int it_m = direct_iterate_mpfr(view.center_x_str, view.center_y_str,
                                                dcx, dcy, max_iter, prec);
        PixelColor c = buf[(size_t)ly * ts + lx];

        int matched = 0;
        for (int d = -1; d <= 1 && !matched; d++) {
            long candidate = (long)it_m + d;
            if (candidate < 0) continue;
            unsigned int it_c = (unsigned long)candidate > max_iter
                                    ? max_iter : (unsigned int)candidate;
            PixelColor expect;
            color_from_iteration_classic(&expect, it_c, max_iter);
            if (expect.r == c.r && expect.g == c.g && expect.b == c.b) matched = 1;
        }
        CHECK(matched, "1e250 pixel (%d,%d): color (%u,%u,%u) vs MPFR iter=%u",
              lx, ly, c.r, c.g, c.b, it_m);
    }

    free(buf);
    mb_deep_renderer_destroy(r);
}

// HP center translation must be exact beyond double precision.
static void test_view_hp_translate(void) {
    MBViewState view;
    mb_view_state_init(&view, 1024, 768);
    view.zoom_level = 1e40;
    mb_view_hp_set_center(&view, "-0.75", "0.1");

    double scale = mb_view_get_scale(&view);
    mb_view_hp_translate(&view, 3.0 * scale, -2.0 * scale);

    // The double mirror cannot see a 1e-40-scale move...
    CHECK(view.center_x == -0.75, "double mirror drifted: %.17g", view.center_x);

    // ...but the strings must have: check with MPFR
    mpfr_prec_t prec = mp_required_precision(view.zoom_level) + 64;
    MPReal a, b, d;
    mp_real_init_set_str(&a, view.center_x_str, prec);
    mp_real_init_set_str(&b, "-0.75", prec);
    mp_real_init(&d, prec);
    mp_real_sub(&d, &a, &b);
    double moved = mp_real_get_d(&d);
    CHECK(fabs(moved - 3.0 * scale) <= fabs(3.0 * scale) * 1e-9,
          "re move wrong: got %.6e want %.6e", moved, 3.0 * scale);

    mp_real_set_str(&a, view.center_y_str);
    mp_real_set_str(&b, "0.1");
    mp_real_sub(&d, &a, &b);
    moved = mp_real_get_d(&d);
    CHECK(fabs(moved - (-2.0 * scale)) <= fabs(2.0 * scale) * 1e-9,
          "im move wrong: got %.6e want %.6e", moved, -2.0 * scale);

    mp_real_clear(&a);
    mp_real_clear(&b);
    mp_real_clear(&d);
}

// Zoom-towards-point must keep the point under the cursor fixed.
static void test_view_hp_zoom_towards_fixed_point(void) {
    MBViewState view;
    mb_view_state_init(&view, 1000, 800);
    view.zoom_level = 1e20;
    mb_view_hp_set_center(&view, "-0.7436438870371587", "0.1318259042053119");

    const double offX = 137.0, offYUp = -73.0;

    // Point under the cursor before: center + off*scale
    double scale_before = mb_view_get_scale(&view);
    mpfr_prec_t prec = mp_required_precision(view.zoom_level) + 64;
    MPReal before_re, before_im;
    mp_real_init_set_str(&before_re, view.center_x_str, prec);
    mp_real_init_set_str(&before_im, view.center_y_str, prec);
    mp_real_add_d(&before_re, &before_re, offX * scale_before);
    mp_real_add_d(&before_im, &before_im, offYUp * scale_before);

    mb_view_hp_zoom_towards(&view, offX, offYUp, 3.7);

    double scale_after = mb_view_get_scale(&view);
    MPReal after_re, after_im, d;
    mp_real_init_set_str(&after_re, view.center_x_str, prec);
    mp_real_init_set_str(&after_im, view.center_y_str, prec);
    mp_real_add_d(&after_re, &after_re, offX * scale_after);
    mp_real_add_d(&after_im, &after_im, offYUp * scale_after);
    mp_real_init(&d, prec);

    mp_real_sub(&d, &after_re, &before_re);
    double drift_re = mp_real_get_d(&d);
    mp_real_sub(&d, &after_im, &before_im);
    double drift_im = mp_real_get_d(&d);

    CHECK(fabs(drift_re) < scale_after * 1e-3 && fabs(drift_im) < scale_after * 1e-3,
          "cursor point drifted by (%.3e, %.3e), pixel=%.3e",
          drift_re, drift_im, scale_after);

    mp_real_clear(&before_re);
    mp_real_clear(&before_im);
    mp_real_clear(&after_re);
    mp_real_clear(&after_im);
    mp_real_clear(&d);
}

static void test_zoom_clamps(void) {
    CHECK(mb_clamp_zoom(0.25) == MB_ZOOM_MIN, "lower clamp");
    CHECK(mb_clamp_zoom(1e299 * 10.0) == MB_ZOOM_MAX, "upper clamp");
    CHECK(mb_clamp_zoom(12345.0) == 12345.0, "identity inside range");

    // The full budget must stay in normal double range
    MBViewState view;
    mb_view_state_init(&view, 300, 300);  // smallest supported viewport
    view.zoom_level = MB_ZOOM_MAX;
    double scale = mb_view_get_scale(&view);
    CHECK(scale > 0.0 && isnormal(scale), "scale degenerate at MB_ZOOM_MAX: %g", scale);
    // Largest delta across a big viewport must be normal too
    CHECK(isnormal(scale * 4000.0), "pixel delta degenerate at MB_ZOOM_MAX");
}

static void test_iteration_scaling(void) {
    CHECK(mb_max_iter_for_zoom(1.0) == MB_DEFAULT_MAX_ITER, "base iter");
    CHECK(mb_max_iter_for_zoom(1e12) > mb_max_iter_for_zoom(1e6), "monotonic");
    CHECK(mb_max_iter_for_zoom(MB_ZOOM_MAX) <= 150000, "cap");
    CHECK(mb_max_iter_for_tile_zoom(40) > mb_max_iter_for_tile_zoom(10),
          "tile zoom monotonic");
}

static void test_pixel_complex_orientation(void) {
    MBViewState view;
    mb_view_state_init(&view, 800, 600);
    view.zoom_level = 4.0;

    // A pixel above the center (smaller py) must map to larger imaginary part
    double cx, cy;
    mb_pixel_to_complex(&view, 400, 100, &cx, &cy);
    CHECK(cy > view.center_y, "top of screen should be +imag (got %g)", cy);

    // Roundtrip identity
    int px, py;
    mb_complex_to_pixel(&view, cx, cy, &px, &py);
    CHECK(abs(px - 400) <= 1 && abs(py - 100) <= 1,
          "roundtrip failed: (400,100) -> (%d,%d)", px, py);
}

static void test_interior_black_with_custom_max_iter(void) {
    PixelColor p = {1, 2, 3};
    color_from_iteration_classic(&p, 500, 500);
    CHECK(p.r == 0 && p.g == 0 && p.b == 0,
          "interior at custom max_iter must be black (got %u,%u,%u)", p.r, p.g, p.b);
}

int main(void) {
    test_perturb_matches_direct_double();
    test_perturb_rebase_short_reference();
    test_perturb_matches_mpfr_at_1e30();
    test_deep_renderer_end_to_end();
    test_pipeline_consistency_at_zoom_ceiling();
    test_view_hp_translate();
    test_view_hp_zoom_towards_fixed_point();
    test_zoom_clamps();
    test_iteration_scaling();
    test_pixel_complex_orientation();
    test_interior_black_with_custom_max_iter();

    printf("%d checks, %d failed\n", tests_run, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
