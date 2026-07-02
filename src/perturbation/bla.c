#include "bla.h"

#include <stdlib.h>
#include <string.h>

// Internal build representation: radius kept un-squared until the end.
typedef struct {
    FloatExpC a, b;
    FloatExp r;     // validity radius on |dz|
    uint32_t l;
} BuildEntry;

// Conservative |v| bounds without square roots:
//   upper: |re| + |im|      >= |v|
//   lower: max(|re|, |im|)  >= |v| / sqrt(2)
static FloatExp mag_upper(FloatExpC v) {
    return fx_add(fx_abs(v.re), fx_abs(v.im));
}

static FloatExp mag_lower(FloatExpC v) {
    FloatExp a = fx_abs(v.re), b = fx_abs(v.im);
    return fx_cmp_abs(a, b) >= 0 ? a : b;
}

// Single-step BLA at orbit index m: dz' ~ 2*Z_m*dz + dc, valid while the
// dropped dz^2 term stays ~2^-MB_BLA_EPS_BITS below the linear term:
// |dz| < eps * |Z_m|.
static BuildEntry single_step(double z_re, double z_im) {
    BuildEntry e;
    e.a.re = fx_from_d(2.0 * z_re);
    e.a.im = fx_from_d(2.0 * z_im);
    e.b.re = fx_from_d(1.0);
    e.b.im = fx_zero();
    FloatExp zmag = mag_lower(fxc_from_d(z_re, z_im));
    e.r = fx_scale2(zmag, -MB_BLA_EPS_BITS);
    e.l = 1;
    return e;
}

// Merge: apply x (first), then y.
//   A = Ay*Ax, B = Ay*Bx + By
//   valid when |dz| < rx AND |dz after x| < ry, with
//   |dz after x| <= |Ax||dz| + |Bx|*dc_max.
static BuildEntry merge(BuildEntry x, BuildEntry y, FloatExp dc_max) {
    BuildEntry e;
    e.a = fxc_mul(y.a, x.a);
    e.b = fxc_add(fxc_mul(y.a, x.b), y.b);
    e.l = x.l + y.l;

    FloatExp ax = mag_upper(x.a);
    FloatExp bx_dc = fx_mul(mag_upper(x.b), dc_max);
    FloatExp budget = fx_sub(y.r, bx_dc);

    FloatExp r;
    if (budget.m <= 0.0) {
        r = fx_zero();
    } else if (fx_is_zero(ax)) {
        // x maps every dz to Bx*dc: y's constraint is dz-independent and
        // already satisfied (budget > 0), so only x's own radius limits.
        r = x.r;
    } else {
        r = fx_div(budget, ax);
        if (fx_cmp_abs(x.r, r) < 0) r = x.r;
    }
    e.r = r;
    return e;
}

int mb_bla_build(MBBlaTable *t, const double *ref_re, const double *ref_im,
                 uint32_t ref_len, FloatExp dc_max) {
    memset(t, 0, sizeof(*t));
    if (ref_len < 4) return 0;  // nothing worth skipping

    // Level counts: level 0 pairs two single steps; each next level pairs
    // the previous one.
    uint32_t cnt0 = ref_len / 2;
    int levels = 0;
    uint32_t total = 0;
    for (uint32_t c = cnt0; c >= 1 && levels < 24; c /= 2) {
        total += c;
        levels++;
        if (c == 1) break;
    }

    t->entries = malloc((size_t)total * sizeof(MBBlaEntry));
    t->level_off = malloc((size_t)levels * sizeof(uint32_t));
    t->level_cnt = malloc((size_t)levels * sizeof(uint32_t));
    BuildEntry *prev = malloc((size_t)cnt0 * sizeof(BuildEntry));
    BuildEntry *curw = malloc((size_t)(cnt0 / 2 + 1) * sizeof(BuildEntry));
    if (!t->entries || !t->level_off || !t->level_cnt || !prev || !curw) {
        free(prev);
        free(curw);
        mb_bla_free(t);
        return -1;
    }

    t->levels = levels;
    t->orbit_len = ref_len;

    uint32_t off = 0;
    uint32_t cnt = cnt0;

    // Level 0: merge pairs of single steps (2i, 2i+1)
    for (uint32_t i = 0; i < cnt0; i++) {
        BuildEntry a = single_step(ref_re[2 * i], ref_im[2 * i]);
        BuildEntry b = single_step(ref_re[2 * i + 1], ref_im[2 * i + 1]);
        prev[i] = merge(a, b, dc_max);
    }

    for (int k = 0; k < levels; k++) {
        t->level_off[k] = off;
        t->level_cnt[k] = cnt;
        for (uint32_t i = 0; i < cnt; i++) {
            MBBlaEntry *e = &t->entries[off + i];
            e->a_re = prev[i].a.re;
            e->a_im = prev[i].a.im;
            e->b_re = prev[i].b.re;
            e->b_im = prev[i].b.im;
            e->r2 = fx_mul(prev[i].r, prev[i].r);
            e->l = prev[i].r.m > 0.0 ? prev[i].l : 0;
        }
        off += cnt;

        uint32_t next = cnt / 2;
        if (next == 0 || k + 1 >= levels) break;
        for (uint32_t i = 0; i < next; i++) {
            curw[i] = merge(prev[2 * i], prev[2 * i + 1], dc_max);
        }
        BuildEntry *tmp = prev;
        prev = curw;
        curw = tmp;
        cnt = next;
    }

    free(prev);
    free(curw);
    return 0;
}

void mb_bla_free(MBBlaTable *t) {
    if (!t) return;
    free(t->entries);
    free(t->level_off);
    free(t->level_cnt);
    memset(t, 0, sizeof(*t));
}
