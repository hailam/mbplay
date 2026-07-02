#ifndef MB_BLA_H
#define MB_BLA_H

#include "../precision/floatexp.h"
#include <stdint.h>
#include <stddef.h>

// =============================================================================
// Bilinear Approximation (BLA) — iteration skipping for perturbation
// =============================================================================
//
// While |dz| is small, the perturbation step
//
//     dz' = 2*Z_m*dz + dz^2 + dc
//
// is dominated by its linear part, so a run of l steps collapses to
//
//     dz_{m+l} = A*dz_m + B*dc
//
// with complex coefficients A, B precomputed from the reference orbit
// (Zhuoran's bilinear approximation). Each entry carries a validity radius:
// the linearization holds when |dz|^2 < r2, with the dz^2 truncation error
// kept below ~2^-MB_BLA_EPS_BITS of the linear term.
//
// The table is a binary merge tree over the orbit: level k entry i skips the
// 2^(k+1) steps starting at orbit index i*2^(k+1) (levels begin at pair
// size 2 — single-step "BLAs" cost as much as real steps, so they are not
// stored). Coefficients and radii are FloatExp: |A| grows like prod|2Z| and
// overflows double for long skips, and radii shrink below double range at
// deep zoom.
//
// At boundary detail the skip factor is routinely 10-100x; interior pixels
// collapse to O(log n) applications per orbit pass, which is what makes
// 100k+ iteration budgets interactive.

#define MB_BLA_EPS_BITS 40

typedef struct {
    FloatExp a_re, a_im;   // A
    FloatExp b_re, b_im;   // B
    FloatExp r2;           // squared validity radius (compare with |dz|^2)
    uint32_t l;            // steps skipped (0 marks an unusable hole)
} MBBlaEntry;

typedef struct {
    MBBlaEntry *entries;   // all levels, concatenated
    uint32_t *level_off;   // entries offset per level
    uint32_t *level_cnt;   // entry count per level
    int levels;            // number of levels (level k skips 2^(k+1) steps)
    uint32_t orbit_len;
} MBBlaTable;

/**
 * Build a BLA table for a reference orbit.
 *
 * @param t       Output table (zeroed on failure).
 * @param ref_re  Reference orbit real parts (Z_0 == 0).
 * @param ref_im  Reference orbit imaginary parts.
 * @param ref_len Usable orbit entries.
 * @param dc_max  Upper bound on |dc| over every pixel this table will serve;
 *                baked into the merge radii.
 * @return 0 on success, -1 on allocation failure (table unusable but safe
 *         to pass around: lookups return NULL).
 */
int mb_bla_build(MBBlaTable *t, const double *ref_re, const double *ref_im,
                 uint32_t ref_len, FloatExp dc_max);

void mb_bla_free(MBBlaTable *t);

/**
 * Find the longest valid skip starting at orbit index m for a delta with
 * squared magnitude dz_mag2. Returns NULL when no entry applies (caller
 * does a single normal step).
 */
static inline const MBBlaEntry *mb_bla_lookup(const MBBlaTable *t, uint32_t m,
                                              FloatExp dz_mag2) {
    if (!t || !t->entries || m == 0) return NULL;

    // Level k entries start at orbit indices aligned to 2^(k+1); the largest
    // usable k is bounded by the alignment of m.
    int max_k = __builtin_ctz(m) - 1;
    if (max_k >= t->levels) max_k = t->levels - 1;

    for (int k = max_k; k >= 0; k--) {
        uint32_t i = m >> (k + 1);
        if (i >= t->level_cnt[k]) continue;
        const MBBlaEntry *e = &t->entries[t->level_off[k] + i];
        if (e->l == 0) continue;
        if (fx_cmp_abs(dz_mag2, e->r2) < 0) return e;
    }
    return NULL;
}

#endif // MB_BLA_H
