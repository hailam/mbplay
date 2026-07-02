#ifndef MB_PERTURB_CPU_H
#define MB_PERTURB_CPU_H

#include <stdint.h>

// =============================================================================
// CPU Perturbation Iteration (double-precision deltas)
// =============================================================================
//
// Metal has no 64-bit float type, so the "V2 double-precision" GPU kernels
// could never compile; this module is the working replacement. It iterates
// the perturbation recurrence
//
//     dz_{n+1} = 2 * Z_ref[m] * dz_n + dz_n^2 + dc
//
// entirely in double precision on the CPU, against a reference orbit stored
// as doubles. Instead of glitch markers + fallback passes it uses rebasing
// (Zhuoran's method): whenever the full value z = Z_ref[m] + dz becomes
// smaller than |dz| (imminent cancellation), or the reference orbit data is
// exhausted, the pixel re-anchors to the start of the orbit with dz := z.
// Since Z_ref[0] = 0 this is exact, so no pixel is ever mis-rendered and no
// second reference is required.
//
// All functions here are pure and thread-safe: callers may render different
// tiles concurrently as long as each output buffer is distinct.

/**
 * Iterate a single pixel by perturbation against a reference orbit.
 *
 * @param ref_re   Reference orbit real parts, ref_re[0] must be 0.
 * @param ref_im   Reference orbit imaginary parts.
 * @param ref_len  Number of usable orbit entries (escape_iter of the orbit).
 *                 Must be >= 1.
 * @param dcx      Pixel delta from the reference point C (real).
 * @param dcy      Pixel delta from the reference point C (imaginary).
 * @param max_iter Iteration budget.
 * @param final_z2 Output: |z|^2 at escape (for smooth coloring); may be NULL.
 * @return Iteration count at escape, or max_iter if the pixel never escaped.
 */
uint32_t perturb_cpu_pixel(const double *ref_re, const double *ref_im,
                           uint32_t ref_len,
                           double dcx, double dcy,
                           uint32_t max_iter, float *final_z2);

/**
 * Iterate a full tile of pre-computed deltas.
 *
 * @param ref_re     Reference orbit real parts (ref_re[0] == 0).
 * @param ref_im     Reference orbit imaginary parts.
 * @param ref_len    Usable orbit entries.
 * @param deltas     Interleaved per-pixel deltas {dcx, dcy}, tile_size^2 pairs.
 * @param tile_size  Tile dimension in pixels.
 * @param max_iter   Iteration budget.
 * @param iterations Output: iteration count per pixel (never a glitch marker).
 * @param final_z2   Output: |z|^2 at escape per pixel; may be NULL.
 */
void perturb_cpu_tile(const double *ref_re, const double *ref_im,
                      uint32_t ref_len,
                      const double *deltas, uint32_t tile_size,
                      uint32_t max_iter,
                      uint32_t *iterations, float *final_z2);

// =============================================================================
// Extended-exponent variant (zoom beyond ~1e290)
// =============================================================================

#include "../precision/floatexp.h"
#include "bla.h"

/**
 * Iterate a single pixel whose delta does not fit a double. Runs a floatexp
 * phase while |dz| is below double range and switches to the plain double
 * loop once it grows (returning to floatexp if a rebase shrinks it again).
 * Semantics otherwise identical to perturb_cpu_pixel.
 */
uint32_t perturb_cpu_pixel_fx(const double *ref_re, const double *ref_im,
                              uint32_t ref_len,
                              FloatExp dcx, FloatExp dcy,
                              uint32_t max_iter, float *final_z2);

// =============================================================================
// BLA-accelerated variants (identical results, skips iteration runs)
// =============================================================================

/**
 * Like perturb_cpu_pixel / perturb_cpu_pixel_fx, but consults a bilinear
 * approximation table (see bla.h) to skip runs of iterations whose combined
 * effect is a precomputed linear map. Pass bla == NULL to disable skipping.
 * The table must have been built for this orbit with a dc_max covering
 * |dcx|, |dcy|.
 */
uint32_t perturb_cpu_pixel_bla(const double *ref_re, const double *ref_im,
                               uint32_t ref_len,
                               double dcx, double dcy,
                               uint32_t max_iter,
                               const MBBlaTable *bla, float *final_z2);

uint32_t perturb_cpu_pixel_fx_bla(const double *ref_re, const double *ref_im,
                                  uint32_t ref_len,
                                  FloatExp dcx, FloatExp dcy,
                                  uint32_t max_iter,
                                  const MBBlaTable *bla, float *final_z2);

#endif // MB_PERTURB_CPU_H
