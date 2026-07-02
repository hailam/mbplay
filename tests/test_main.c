// Unit tests for the deep-zoom pipeline: CPU perturbation, HP view-state
// operations, the deep tile renderer, and zoom-limit plumbing.
//
// Build & run:  xmake build mandelbrot_tests && xmake run mandelbrot_tests

#include "../src/config.h"
#include "../src/mandelbrot/mandelbrot.h"
#include "../src/perturbation/perturbation.h"
#include "../src/perturbation/perturb_cpu.h"
#include "../src/perturbation/bla.h"
#include "../src/perturbation/deep_render.h"
#include "../src/cinematic/director.h"
#include "../src/gpu/gpu.h"
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

// Direct MPFR iteration with an extended-exponent pixel offset (dc does not
// fit a double past zoom ~1e300).
static unsigned int direct_iterate_mpfr_fx(const char *cx_str, const char *cy_str,
                                           FloatExp dcx, FloatExp dcy,
                                           unsigned int max_iter, unsigned int prec) {
    MPComplex c, z, z_new;
    MPReal norm;
    mp_complex_init_set_str(&c, cx_str, cy_str, prec);
    mp_complex_init(&z, prec);
    mp_complex_init(&z_new, prec);
    mp_real_init(&norm, prec);

    mp_real_add_fx(&c.re, &c.re, dcx.m, dcx.e);
    mp_real_add_fx(&c.im, &c.im, dcy.m, dcy.e);

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

// Beyond double range entirely: render at zoom 10^500 and 10^1000 (floatexp
// deltas, floatexp pixel phase) and check sampled pixels against MPFR.
static void check_deep_consistency_at_log10(double zoom_l10, uint64_t gen) {
    MBViewState view;
    mb_view_state_init(&view, 512, 512);
    mb_view_set_zoom_log10(&view, zoom_l10);
    mb_view_hp_set_center(&view,
                          "-0.743643887037158704752191506114774",
                          "0.131825904205311970493132056385139");
    view.high_precision_mode = true;

    MBDeepRenderer *r = mb_deep_renderer_create();
    CHECK(r != NULL, "renderer create failed");

    MBRenderSettings settings = {
        .color_mode = MB_COLOR_MODE_CLASSIC,
        .palette_id = MB_PALETTE_CLASSIC,
        .antialiasing_enabled = false,
        .color_cycle_scale = 64.0f,
    };

    const int ts = 16;
    const int tile_px = 248, tile_py = 248;
    PixelColor *buf = malloc((size_t)ts * ts * sizeof(PixelColor));
    CHECK(buf != NULL, "tile alloc failed");

    unsigned int max_iter = 12000;
    int rc = mb_deep_renderer_render_tile(r, &view, gen, tile_px, tile_py, ts,
                                          max_iter, &settings, buf);
    CHECK(rc == MB_DEEP_OK, "render_tile returned %d at 10^%.0f", rc, zoom_l10);

    FloatExp scale = mb_view_get_scale_fx(&view);
    unsigned int prec = (unsigned int)mp_required_precision_log10(zoom_l10);

    const int samples[][2] = { {0, 0}, {15, 15}, {7, 3} };
    for (size_t i = 0; i < sizeof(samples) / sizeof(samples[0]); i++) {
        int lx = samples[i][0], ly = samples[i][1];
        int px = tile_px + lx, py = tile_py + ly;
        FloatExp dcx = fx_mul_d(scale, px - 256.0);
        FloatExp dcy = fx_mul_d(scale, 256.0 - py);  // imaginary axis up

        unsigned int it_m = direct_iterate_mpfr_fx(view.center_x_str, view.center_y_str,
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
        CHECK(matched,
              "10^%.0f pixel (%d,%d): color (%u,%u,%u) vs MPFR iter=%u",
              zoom_l10, lx, ly, c.r, c.g, c.b, it_m);
    }

    free(buf);
    mb_deep_renderer_destroy(r);
}

static void test_pipeline_consistency_beyond_double_range(void) {
    check_deep_consistency_at_log10(500.0, 1);
    check_deep_consistency_at_log10(1000.0, 1);
}

#include <time.h>

static double now_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

// BLA must not change results (within the +-1 escape-boundary tolerance) and
// must actually skip work.
static void test_bla_equivalence_and_speedup(void) {
    const char *cx_str = "-0.743643887037158704752191506114774";
    const char *cy_str = "0.131825904205311970493132056385139";
    const double zoom = 1e30;
    const unsigned int max_iter = 60000;
    const unsigned int prec = (unsigned int)mp_required_precision(zoom);
    const double scale = (2.0 / 1024.0) / zoom;

    ReferenceOrbitHP orbit;
    CHECK(ref_orbit_hp_init(&orbit, max_iter, prec) == 0, "hp orbit init failed");
    ref_orbit_hp_compute(&orbit, cx_str, cy_str);
    CHECK(orbit.valid, "hp orbit invalid");

    uint32_t ref_len = orbit.escape_iter + 1;
    if (ref_len > orbit.max_iter) ref_len = orbit.max_iter;

    MBBlaTable bla;
    FloatExp dc_max = fx_from_d(scale * 2000.0);
    CHECK(mb_bla_build(&bla, orbit.z_real, orbit.z_imag, ref_len, dc_max) == 0,
          "bla build failed");
    CHECK(bla.entries != NULL && bla.levels > 8, "bla table degenerate");

    const int N = 8;
    uint32_t plain[N * N], fast[N * N];

    double t0 = now_seconds();
    for (int py = 0; py < N; py++) {
        for (int px = 0; px < N; px++) {
            double dcx = (px - N / 2) * 64.0 * scale;
            double dcy = (py - N / 2) * 64.0 * scale;
            plain[py * N + px] = perturb_cpu_pixel(orbit.z_real, orbit.z_imag,
                                                   ref_len, dcx, dcy, max_iter, NULL);
        }
    }
    double t1 = now_seconds();
    for (int py = 0; py < N; py++) {
        for (int px = 0; px < N; px++) {
            double dcx = (px - N / 2) * 64.0 * scale;
            double dcy = (py - N / 2) * 64.0 * scale;
            fast[py * N + px] = perturb_cpu_pixel_bla(orbit.z_real, orbit.z_imag,
                                                      ref_len, dcx, dcy, max_iter,
                                                      &bla, NULL);
        }
    }
    double t2 = now_seconds();

    int exact = 0;
    for (int i = 0; i < N * N; i++) {
        uint32_t d = plain[i] > fast[i] ? plain[i] - fast[i] : fast[i] - plain[i];
        if (d == 0) exact++;
        CHECK(d <= 1, "pixel %d: plain=%u bla=%u", i, plain[i], fast[i]);
    }
    CHECK(exact * 100 >= N * N * 90, "only %d/%d exact matches with BLA", exact, N * N);

    double plain_t = t1 - t0;
    double bla_t = t2 - t1;
    CHECK(bla_t * 2.0 < plain_t,
          "BLA not faster: plain=%.3fs bla=%.3fs", plain_t, bla_t);
    printf("  [bla] plain %.3fs, bla %.3fs (%.0fx speedup)\n",
           plain_t, bla_t, plain_t / bla_t);

    mb_bla_free(&bla);
    ref_orbit_hp_cleanup(&orbit);
}

// Resuming an orbit at a larger budget must produce bit-identical results to
// computing it fresh (same values, same rounding sequence).
static void test_orbit_continuation(void) {
    const char *cx_str = "-0.743643887037158704752191506114774";
    const char *cy_str = "0.131825904205311970493132056385139";
    const unsigned int prec = (unsigned int)mp_required_precision_log10(30.0);

    ReferenceOrbitHP small, resumed, fresh;
    CHECK(ref_orbit_hp_init(&small, 20000, prec) == 0, "init small");
    ref_orbit_hp_compute(&small, cx_str, cy_str);
    CHECK(small.valid && small.escape_iter == small.max_iter,
          "expected a non-escaping reference (escape=%u)", small.escape_iter);
    CHECK(small.cont_z != NULL, "continuation state missing");

    CHECK(ref_orbit_hp_init(&resumed, 30000, prec) == 0, "init resumed");
    CHECK(ref_orbit_hp_continue(&resumed, &small, NULL, NULL) == 0, "continue failed");

    CHECK(ref_orbit_hp_init(&fresh, 30000, prec) == 0, "init fresh");
    ref_orbit_hp_compute(&fresh, cx_str, cy_str);

    CHECK(resumed.escape_iter == fresh.escape_iter, "escape_iter mismatch: %u vs %u",
          resumed.escape_iter, fresh.escape_iter);
    int mismatches = 0;
    for (uint32_t i = 0; i < 30000 && mismatches < 5; i++) {
        if (resumed.z_real[i] != fresh.z_real[i] || resumed.z_imag[i] != fresh.z_imag[i]) {
            mismatches++;
            CHECK(0, "orbit value differs at %u", i);
        }
    }

    // Preconditions enforced: cannot continue into a smaller/equal budget
    ReferenceOrbitHP tiny;
    CHECK(ref_orbit_hp_init(&tiny, 10000, prec) == 0, "init tiny");
    CHECK(ref_orbit_hp_continue(&tiny, &small, NULL, NULL) == -1,
          "continue into smaller budget must fail");

    ref_orbit_hp_cleanup(&small);
    ref_orbit_hp_cleanup(&resumed);
    ref_orbit_hp_cleanup(&fresh);
    ref_orbit_hp_cleanup(&tiny);
}

// The paired (SIMD) iteration must statistically match the scalar path.
// (FMA rounding differs, so chaotic boundary pixels may drift by a little —
// the same tolerance philosophy as perturbation-vs-direct.)
static void test_pair_matches_scalar(void) {
    const double cx0 = -0.7436438870372, cy0 = 0.1318259043;
    const double scale = 1e-9;
    const unsigned int max_iter = 3000;

    int total = 0, exact = 0, far = 0;
    for (int py = 0; py < 16; py++) {
        for (int px = 0; px < 16; px += 2) {
            double ax = cx0 + (px - 8) * scale;
            double bx = cx0 + (px - 7) * scale;
            double y = cy0 + (py - 8) * scale;

            unsigned int p0, p1;
            float z0, z1;
            mb_compute_pair_smooth(ax, y, bx, y, max_iter, &p0, &p1, &z0, &z1);

            float sz;
            unsigned int s0 = mb_compute_point_smooth(ax, y, max_iter, &sz);
            unsigned int s1 = mb_compute_point_smooth(bx, y, max_iter, &sz);

            total += 2;
            if (p0 == s0) exact++;
            else if ((p0 > s0 ? p0 - s0 : s0 - p0) > 2) far++;
            if (p1 == s1) exact++;
            else if ((p1 > s1 ? p1 - s1 : s1 - p1) > 2) far++;

            // Interior verdicts must agree exactly
            CHECK((p0 >= max_iter) == (s0 >= max_iter), "interior verdict differs (lane0)");
            CHECK((p1 >= max_iter) == (s1 >= max_iter), "interior verdict differs (lane1)");
        }
    }
    CHECK(exact * 100 >= total * 90, "pair vs scalar: only %d/%d exact", exact, total);
    CHECK(far * 100 <= total * 3, "pair vs scalar: %d/%d far off", far, total);
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
    CHECK(mb_clamp_zoom_log10(-3.0) == MB_ZOOM_LOG10_MIN, "lower clamp");
    CHECK(mb_clamp_zoom_log10(99999.0) == MB_ZOOM_LOG10_MAX, "upper clamp");
    CHECK(mb_clamp_zoom_log10(123.0) == 123.0, "identity inside range");

    // At the ceiling, the FloatExp scale must stay exact and nonzero
    MBViewState view;
    mb_view_state_init(&view, 300, 300);  // smallest supported viewport
    mb_view_set_zoom_log10(&view, MB_ZOOM_LOG10_MAX);
    CHECK(view.zoom_level == MB_ZOOM_MIRROR_MAX, "mirror saturation");
    CHECK(view.zoom_log10 == MB_ZOOM_LOG10_MAX, "log10 stored");

    FloatExp scale = mb_view_get_scale_fx(&view);
    CHECK(!fx_is_zero(scale), "fx scale zero at ceiling");
    double l10 = fx_log10(scale);
    // scale = (2/300) * 10^-4000 -> log10 ~ -4002.2
    CHECK(fabs(l10 - (-4000.0 + log10(2.0 / 300.0))) < 0.01,
          "fx scale magnitude wrong at ceiling: 10^%.2f", l10);
}

static void test_floatexp_arithmetic(void) {
    // Round trips
    CHECK(fx_to_d(fx_from_d(1.5)) == 1.5, "roundtrip 1.5");
    CHECK(fx_to_d(fx_from_d(-3.25e-200)) == -3.25e-200, "roundtrip small");
    CHECK(fx_to_d(fx_from_d(0.0)) == 0.0, "roundtrip zero");

    // Arithmetic identities within double range
    FloatExp a = fx_from_d(3.0), b = fx_from_d(-7.5);
    CHECK(fx_to_d(fx_add(a, b)) == -4.5, "add");
    CHECK(fx_to_d(fx_sub(a, b)) == 10.5, "sub");
    CHECK(fx_to_d(fx_mul(a, b)) == -22.5, "mul");
    CHECK(fx_to_d(fx_div(b, a)) == -2.5, "div");

    // Beyond double range: 10^-1000 constructed, squared, and recovered
    FloatExp tiny = fx_from_log10(-1000.0);
    CHECK(fabs(fx_log10(tiny) + 1000.0) < 1e-9, "from_log10: 10^%.12f", fx_log10(tiny));
    FloatExp tiny2 = fx_mul(tiny, tiny);
    CHECK(fabs(fx_log10(tiny2) + 2000.0) < 1e-9, "mul beyond range");
    CHECK(fx_to_d(tiny) == 0.0, "to_d underflow flush");

    // Addition ordering: adding something 2^-100 smaller is absorbed
    FloatExp one = fx_from_d(1.0);
    FloatExp eps = fx_norm(1.0, -100);
    CHECK(fx_to_d(fx_add(one, eps)) == 1.0, "absorption");
    // ...but comparable magnitudes are exact
    FloatExp x = fx_norm(1.0, -1030);   // 2^-1030 (subnormal-range value)
    FloatExp y = fx_norm(1.0, -1031);
    FloatExp s = fx_add(x, y);          // 1.5 * 2^-1030
    CHECK(s.e == -1030 + 1 && fabs(s.m - 0.75) < 1e-15, "sub-double-range add");

    // Comparisons
    CHECK(fx_cmp_abs(tiny, tiny2) > 0, "cmp magnitudes");
    CHECK(fx_ge_d(fx_from_d(4.0), 4.0), "ge equal");
    CHECK(!fx_ge_d(tiny, 4.0), "ge small");
}

static void test_iteration_scaling(void) {
    CHECK(mb_max_iter_for_zoom(1.0) == MB_DEFAULT_MAX_ITER, "base iter");
    CHECK(mb_max_iter_for_zoom(1e12) > mb_max_iter_for_zoom(1e6), "monotonic");
    CHECK(mb_max_iter_for_zoom_log10(MB_ZOOM_LOG10_MAX) <= 500000, "cap");
    CHECK(mb_max_iter_for_zoom_log10(20.0) == mb_max_iter_for_zoom(1e20),
          "log10 variant consistent");
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

// GPU float-float perturbation must match the CPU double path (within +-2
// counts on chaotic boundary pixels; df carries ~48-bit mantissas).
static void test_gpu_df_matches_cpu(void) {
    if (gpu_init_tiles(256, 1000) != 0 || gpu_init_perturbation(4096) != 0 ||
        !gpu_df_available()) {
        printf("  [gpu] no Metal device/pipeline — skipping df test\n");
        return;
    }

    MBViewState view;
    mb_view_state_init(&view, 512, 512);
    mb_view_set_zoom_log10(&view, 20.0);   // inside the float-float window
    mb_view_hp_set_center(&view,
                          "-0.743643887037158704752191506114774",
                          "0.131825904205311970493132056385139");
    view.high_precision_mode = true;

    MBDeepRenderer *r = mb_deep_renderer_create();
    CHECK(r != NULL, "renderer create failed");

    const int ts = 64;
    uint32_t max_iter = mb_max_iter_for_zoom_log10(20.0);
    uint32_t *cpu_it = malloc((size_t)ts * ts * sizeof(uint32_t));
    uint32_t *gpu_it = malloc((size_t)ts * ts * sizeof(uint32_t));
    CHECK(cpu_it && gpu_it, "alloc failed");

    mb_deep_render_set_gpu_enabled(false);
    int rc = mb_deep_renderer_probe_strided(r, &view, 1, 224, 224, ts, 1,
                                            max_iter, cpu_it);
    CHECK(rc == MB_DEEP_OK, "cpu probe failed");

    mb_deep_render_set_gpu_enabled(true);
    rc = mb_deep_renderer_probe_strided(r, &view, 2, 224, 224, ts, 1,
                                        max_iter, gpu_it);
    CHECK(rc == MB_DEEP_OK, "gpu probe failed");

    int total = ts * ts, exact = 0, far = 0;
    for (int i = 0; i < total; i++) {
        uint32_t d = cpu_it[i] > gpu_it[i] ? cpu_it[i] - gpu_it[i]
                                           : gpu_it[i] - cpu_it[i];
        if (d == 0) exact++;
        else if (d > 2) far++;
        // Interior verdicts must agree
        CHECK((cpu_it[i] >= max_iter) == (gpu_it[i] >= max_iter),
              "interior verdict differs at %d (cpu=%u gpu=%u)", i, cpu_it[i], gpu_it[i]);
    }
    CHECK(exact * 100 >= total * 85, "gpu vs cpu: only %d/%d exact", exact, total);
    CHECK(far * 100 <= total * 3, "gpu vs cpu: %d/%d far off", far, total);
    printf("  [gpu] df vs cpu: %d/%d exact, %d far\n", exact, total, far);

    free(cpu_it);
    free(gpu_it);
    mb_deep_renderer_destroy(r);
}

// The cinematic director: keyframes render, bracket correctly, pipeline
// bounds itself, and steering keeps producing non-degenerate frames.
static void test_cinematic_director(void) {
    MBRenderSettings settings = {
        .color_mode = MB_COLOR_MODE_SMOOTH,
        .palette_id = MB_PALETTE_FIRE,
        .antialiasing_enabled = false,
        .color_cycle_scale = 64.0f,
    };

    MBDirector *d = mb_director_create(192, 128, &settings);
    CHECK(d != NULL, "director create failed");

    MBViewState seed;
    mb_view_state_init(&seed, 192, 128);
    seed.center_x = -0.7436;
    seed.center_y = 0.1318;
    mb_view_hp_sync_from_doubles(&seed);
    mb_view_set_zoom_log10(&seed, 3.0);
    mb_director_start(d, &seed);

    int k0 = mb_director_render_next(d);
    CHECK(k0 >= 0, "first keyframe failed");
    int k1 = mb_director_render_next(d);
    CHECK(k1 == k0 + 1, "keyframe indices not sequential: %d after %d", k1, k0);
    mb_director_render_next(d);

    // Bracketing lookup mid-way between the first two keyframes
    double z = (k0 + 0.5) * MB_CINE_STEP;
    const MBCineKeyframe *lo = NULL, *hi = NULL;
    mb_director_lock_frames(d, z, &lo, &hi);
    CHECK(lo && lo->index == k0, "lo keyframe missing/wrong");
    CHECK(hi && hi->index == k0 + 1, "hi keyframe missing/wrong");
    int nonblack = 0;
    if (lo) {
        for (int i = 0; i < 192 * 128; i++) {
            if (lo->pixels[i].r || lo->pixels[i].g || lo->pixels[i].b) nonblack++;
        }
    }
    mb_director_unlock_frames(d);
    CHECK(nonblack > 100, "keyframe nearly black (%d colored px)", nonblack);

    CHECK(mb_director_ready_log10(d) >= (k0 + 3) * MB_CINE_STEP - 1e-9,
          "ready watermark wrong: %f", mb_director_ready_log10(d));

    // The pipeline must self-limit to MB_CINE_AHEAD past playback
    int guard = 0;
    while (mb_director_render_next(d) >= 0 && guard < 20) guard++;
    CHECK(guard < 20, "pipeline never reported full");

    mb_director_destroy(d);
}

// Void regression: an autopilot dive must keep the set boundary in frame at
// every keyframe — i.e. every keyframe's view contains BOTH interior and
// escaping points, and the escaping counts must not be a flat featureless
// field. Runs at the real viewer viewport: the historical escapes (steering
// oversteer at 10^3, budget blindness at 10^19) only reproduced at full
// frame size, not at thumbnail sizes.
static void test_cinematic_dive_stays_on_boundary(void) {
    MBRenderSettings settings = {
        .color_mode = MB_COLOR_MODE_CLASSIC,
        .palette_id = MB_PALETTE_CLASSIC,
        .antialiasing_enabled = false,
        .color_cycle_scale = 64.0f,
    };

    const int W = 1280, H = 800;
    MBDirector *d = mb_director_create(W, H, &settings);
    CHECK(d != NULL, "director create failed");

    MBViewState seed;
    mb_view_state_init(&seed, W, H);   // default full-set view
    mb_director_start(d, &seed);

    MBDeepRenderer *checker = mb_deep_renderer_create();
    CHECK(checker != NULL, "checker renderer failed");
    uint32_t probe[24 * 24];

    const int DIVE = 70;   // ~21 decades: crosses the budget-pressure zone
    double frac_sum = 0.0;
    int frac_frames = 0;
    for (int step = 0; step < DIVE; step++) {
        // Advance playback so the pipeline never reports full
        const MBCineKeyframe *lo = NULL, *hi = NULL;
        mb_director_lock_frames(d, step * MB_CINE_STEP, &lo, &hi);
        mb_director_unlock_frames(d);

        int k = mb_director_render_next(d);
        CHECK(k == step, "keyframe %d failed (got %d)", step, k);
        if (k != step) break;

        // Inspect the keyframe at the budget it actually rendered with
        mb_director_lock_frames(d, k * MB_CINE_STEP, &lo, &hi);
        CHECK(lo && lo->index == k, "keyframe %d not retrievable", k);
        MBViewState v = lo->view;
        uint32_t max_iter = lo->max_iter;
        mb_director_unlock_frames(d);

        int span = W < H ? W : H;
        int probe_size = span - (span % 24);
        int rc = mb_deep_renderer_probe_strided(checker, &v, (uint64_t)(k + 1),
                                                (W - probe_size) / 2,
                                                (H - probe_size) / 2,
                                                probe_size, probe_size / 24,
                                                max_iter, probe);
        CHECK(rc == MB_DEEP_OK, "probe failed at keyframe %d", k);

        int interior = 0, escaped = 0;
        uint32_t vmin = UINT32_MAX, vmax = 0;
        for (int i = 0; i < 24 * 24; i++) {
            if (probe[i] >= max_iter) interior++;
            else {
                escaped++;
                if (probe[i] < vmin) vmin = probe[i];
                if (probe[i] > vmax) vmax = probe[i];
            }
        }
        CHECK(interior > 0 && escaped > 0,
              "keyframe %d lost the boundary (interior=%d escaped=%d, zoom 10^%.1f)",
              k, interior, escaped, mb_view_zoom_log10(&v));

        // A frame that has escapes but a flat count field is the "plain
        // orange" failure: visually featureless even though not interior
        if (escaped > 0 && interior == 0) {
            double spread = (double)(vmax - vmin) / (double)(vmax + 1);
            CHECK(spread >= 0.15,
                  "keyframe %d is featureless exterior (counts %u..%u, zoom 10^%.1f)",
                  k, vmin, vmax, mb_view_zoom_log10(&v));
        }

        // Track the interior fraction once steering has had time to settle
        if (k >= 6) {
            frac_sum += (double)interior / (24 * 24);
            frac_frames++;
        }
    }

    // The dive must not merely keep the boundary "somewhere in frame": a
    // frame that is 95% smooth exterior is a bright featureless gradient.
    // The steering regulates toward ~30-55% set, so the settled average has
    // to be comfortably inside a broad band.
    if (frac_frames > 0) {
        double mean_frac = frac_sum / frac_frames;
        CHECK(mean_frac > 0.12 && mean_frac < 0.80,
              "dive settled on non-cinematic framing (mean interior %.0f%%)",
              mean_frac * 100.0);
        printf("  [cine] dive mean set coverage: %.0f%%\n", mean_frac * 100.0);
    }

    mb_deep_renderer_destroy(checker);
    mb_director_destroy(d);
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
    test_floatexp_arithmetic();
    test_pipeline_consistency_beyond_double_range();
    test_bla_equivalence_and_speedup();
    test_orbit_continuation();
    test_pair_matches_scalar();
    test_view_hp_translate();
    test_view_hp_zoom_towards_fixed_point();
    test_zoom_clamps();
    test_iteration_scaling();
    test_pixel_complex_orientation();
    test_gpu_df_matches_cpu();
    test_cinematic_director();
    test_cinematic_dive_stays_on_boundary();
    test_interior_black_with_custom_max_iter();

    printf("%d checks, %d failed\n", tests_run, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
