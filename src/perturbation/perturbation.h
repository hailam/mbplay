#ifndef MB_PERTURBATION_H
#define MB_PERTURBATION_H

#include "../config.h"
#include "../precision/mp_complex.h"
#include <stdint.h>
#include <stdbool.h>

#define MB_REF_ORBIT_MAX_ITER 4096

// =============================================================================
// Standard (double precision) Reference Orbit
// =============================================================================

typedef struct {
    double ref_cx, ref_cy;      // Reference point C
    double *z_real, *z_imag;    // Z_ref[n] for each iteration
    uint32_t escape_iter;       // When reference escaped
    uint32_t max_iter;
    bool valid;
} ReferenceOrbit;

// =============================================================================
// High-Precision Reference Orbit
// =============================================================================

typedef struct {
    // Reference point C as decimal strings (full precision)
    char ref_cx_str[MB_HP_COORD_STR_LEN];
    char ref_cy_str[MB_HP_COORD_STR_LEN];

    // Reference point C as doubles (for display/low-precision uses)
    double ref_cx, ref_cy;

    // Orbit history stored as doubles for GPU upload
    // (computed in HP, then converted to double for GPU)
    double *z_real, *z_imag;

    uint32_t escape_iter;       // When reference escaped
    uint32_t max_iter;          // Maximum iterations allocated
    uint32_t precision;         // Bits of precision used for computation
    bool valid;

    // Continuation state: final z of a non-escaped orbit (heap-allocated),
    // so a later, deeper iteration budget at the same center can RESUME
    // instead of recomputing from iteration zero. NULL when the orbit
    // escaped, was aborted, or has not been computed.
    MPComplex *cont_z;
} ReferenceOrbitHP;

/**
 * Initialize a reference orbit structure.
 * Allocates z_real/z_imag arrays.
 * @param orbit The orbit to initialize
 * @param max_iter Maximum iterations to store
 * @return 0 on success, -1 on failure
 */
int ref_orbit_init(ReferenceOrbit *orbit, uint32_t max_iter);

/**
 * Compute the reference orbit for a given center point.
 * Performs standard Mandelbrot iteration, storing Z_ref[n] at each step.
 * @param orbit The orbit structure (must be initialized)
 * @param cx Real part of center point
 * @param cy Imaginary part of center point
 */
void ref_orbit_compute(ReferenceOrbit *orbit, double cx, double cy);

/**
 * Free orbit arrays.
 * @param orbit The orbit to cleanup
 */
void ref_orbit_cleanup(ReferenceOrbit *orbit);

// =============================================================================
// High-Precision Reference Orbit API
// =============================================================================

/**
 * Initialize a high-precision reference orbit structure.
 * @param orbit The orbit to initialize
 * @param max_iter Maximum iterations to store
 * @param precision Precision in bits (128, 256, 512, 1024, etc.)
 * @return 0 on success, -1 on failure
 */
int ref_orbit_hp_init(ReferenceOrbitHP *orbit, uint32_t max_iter, uint32_t precision);

/**
 * Compute the reference orbit at a given center point using arbitrary precision.
 * @param orbit The orbit structure (must be initialized)
 * @param cx_str Real part as decimal string
 * @param cy_str Imaginary part as decimal string
 */
void ref_orbit_hp_compute(ReferenceOrbitHP *orbit, const char *cx_str, const char *cy_str);

/**
 * Abortable variant: should_abort(ctx) is polled every few hundred
 * iterations; returning true stops the computation and leaves the orbit
 * invalid. A deep orbit (500k iterations at 16k-bit precision) can take tens
 * of seconds — the caller must be able to cancel it when the view moves on.
 */
void ref_orbit_hp_compute_cancellable(ReferenceOrbitHP *orbit,
                                      const char *cx_str, const char *cy_str,
                                      bool (*should_abort)(void *ctx), void *ctx);

/**
 * Resume a non-escaped orbit at a larger iteration budget: copies src's
 * stored iterations into dst and appends only the new ones from src's saved
 * MPFR state. Requirements: dst initialized with the SAME precision and a
 * LARGER max_iter; src valid, non-escaped, with continuation state; both
 * describe the same center (caller's responsibility — compare the parsed
 * values, not the strings, which gain digits as the zoom deepens).
 * Returns 0 on success, -1 if the preconditions do not hold or the
 * computation was aborted.
 */
int ref_orbit_hp_continue(ReferenceOrbitHP *dst, const ReferenceOrbitHP *src,
                          bool (*should_abort)(void *ctx), void *ctx);

/**
 * Free high-precision orbit arrays and MPFR resources.
 * @param orbit The orbit to cleanup
 */
void ref_orbit_hp_cleanup(ReferenceOrbitHP *orbit);

/**
 * Check if a high-precision orbit is still valid for a given center.
 * @param orbit The orbit to check
 * @param cx_str Center X as decimal string
 * @param cy_str Center Y as decimal string
 * @param tolerance Maximum distance (as double) before needing recompute
 * @return true if orbit is still usable
 */
bool ref_orbit_hp_is_valid_for(const ReferenceOrbitHP *orbit,
                               const char *cx_str, const char *cy_str,
                               double tolerance);

#endif // MB_PERTURBATION_H
