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

#endif // MB_PERTURB_CPU_H
