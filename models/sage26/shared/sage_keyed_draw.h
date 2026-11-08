/**
 * @file    sage_keyed_draw.h
 * @brief   The sage26 package's keyed uniform draw, in place of SAGE26's libc rand()
 *
 * SAGE26 draws its CGM regime from the process-global libc stream,
 * `(double)rand() / (double)RAND_MAX` (R = the staged SAGE26 source,
 * R/src/model_regimes.c:94), so a galaxy's draw depends on how many draws every earlier galaxy
 * of the run consumed, which docs/VISION.md Principle 4 forbids. The package replaces that one draw
 * (the one deliberate departure from SAGE26's arithmetic in the classifier) with a stateless keyed
 * draw:
 * the uniform deviate is a pure function of four inputs,
 *
 *   - the galaxy row's UniqueGalaxyID (64-bit, converted by its two's-complement bits),
 *   - the current snapshot number (the halo's snapshot at the call, ctx->snapshot_number; never
 *     a substep or traversal count),
 *   - a fixed purpose salt (MIMIC_SAGE_DRAW_PURPOSE_CGM for the CGM regime classifier), and
 *   - an ordinal (0 for the classifier),
 *
 * so it does not depend on traversal order, array position, the driver, the MPI rank or how
 * many draws came before. There is no seed parameter and no process-global state.
 * UniqueGalaxyID is built identically by both drivers (src/core/build_model.c
 * make_unique_galaxy_id, src/core/horizontal_driver.c horizontal_make_unique_galaxy_id), so the
 * draw is driver-invariant by construction.
 *
 * The mapping is frozen (a change moves every CGM regime draw), in unsigned 64-bit wrapping
 * arithmetic:
 *
 *   mix(x):  the splitmix64 finaliser
 *            x += 0x9e3779b97f4a7c15; x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9;
 *            x = (x ^ (x >> 27)) * 0x94d049bb133111eb; return x ^ (x >> 31);
 *   k    = mix((uint64_t)galaxy_id ^ GALAXY)
 *   k    = mix(k ^ ((uint64_t)(int64_t)snapshot + SNAPSHOT))
 *   k    = mix(k ^ PURPOSE)
 *   bits = mix(k ^ mix((uint64_t)ordinal + INDEX))
 *   u    = ((double)(bits >> 12) + 0.5) * 0x1.0p-52, strictly inside (0, 1)
 *
 * with the salts GALAXY, SNAPSHOT, PURPOSE_CGM and INDEX below. The deviate takes the top 52
 * bits, centred in their cells: (bits >> 12) is below 2^52, so it and the + 0.5 are exact in
 * double, and the extremes are 2^-53 and 1 - 2^-53. Eight reference vectors are asserted bit for
 * bit by shared/_tests/test_unit_sage_keyed_draw.c; the code and a vector are
 * never changed to fit each other.
 *
 * The mixer is the same finaliser as the HOD package's hod_random_mix, written here again: the
 * sage26 package depends on no other model package.
 *
 * Header-only and private to models/sage26; it is not framework API.
 */

#ifndef MIMIC_SHARED_SAGE_KEYED_DRAW_H
#define MIMIC_SHARED_SAGE_KEYED_DRAW_H

#include <stdint.h>

/** Weyl increment of splitmix64 (2^64 / golden ratio) */
#define MIMIC_SAGE_DRAW_GOLDEN UINT64_C(0x9e3779b97f4a7c15)

/** Domain-separation salts of the galaxy and snapshot key components (frozen) */
#define MIMIC_SAGE_DRAW_SALT_GALAXY UINT64_C(0x8c5e2b7d4a1f3069)
#define MIMIC_SAGE_DRAW_SALT_SNAPSHOT UINT64_C(0x3b9f6d2e8a7c1405)

/** Salt of the ordinal before it is mixed into the key (frozen) */
#define MIMIC_SAGE_DRAW_SALT_INDEX UINT64_C(0x6f1c3a9e5d27b483)

/** Purpose salt of the CGM regime classifier's draw (frozen) */
#define MIMIC_SAGE_DRAW_PURPOSE_CGM UINT64_C(0xd2a4f17e6c3b8095)

/** The classifier's ordinal: one draw per galaxy row and snapshot */
#define MIMIC_SAGE_DRAW_ORDINAL_CGM UINT64_C(0)

/**
 * @brief   splitmix64 finaliser: a bijective, well-mixed 64-bit hash
 *
 * @param   x  Value to mix
 * @return  Mixed value
 */
static inline uint64_t mimic_sage_draw_mix(uint64_t x) {
  x += MIMIC_SAGE_DRAW_GOLDEN;
  x = (x ^ (x >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
  x = (x ^ (x >> 27)) * UINT64_C(0x94d049bb133111eb);
  return x ^ (x >> 31);
}

/**
 * @brief   The 64 keyed bits of one draw: (galaxy, snapshot, purpose, ordinal)
 *
 * Each key component is salted and folded through the mixer in turn, so changing any one of
 * them gives an unrelated value; the salted ordinal is mixed on its own before it is folded in.
 * A negative galaxy ID (a record created by a module) is folded by its two's-complement bits,
 * and the snapshot by those of its 64-bit sign extension.
 *
 * @param   galaxy_id  The row's UniqueGalaxyID
 * @param   snapshot   The current snapshot number (ctx->snapshot_number)
 * @param   purpose    The draw's purpose salt (MIMIC_SAGE_DRAW_PURPOSE_CGM for the classifier)
 * @param   ordinal    The draw's ordinal within its key (0 for the classifier)
 * @return  The keyed bits
 */
static inline uint64_t mimic_sage_keyed_bits(const int64_t galaxy_id, const int snapshot,
                                             const uint64_t purpose, const uint64_t ordinal) {
  uint64_t key = mimic_sage_draw_mix((uint64_t)galaxy_id ^ MIMIC_SAGE_DRAW_SALT_GALAXY);
  key = mimic_sage_draw_mix(key ^ ((uint64_t)(int64_t)snapshot + MIMIC_SAGE_DRAW_SALT_SNAPSHOT));
  key = mimic_sage_draw_mix(key ^ purpose);
  return mimic_sage_draw_mix(key ^ mimic_sage_draw_mix(ordinal + MIMIC_SAGE_DRAW_SALT_INDEX));
}

/**
 * @brief   The uniform deviate of 64 keyed bits, strictly inside (0, 1)
 *
 * The top 52 bits centred in their cells: ((double)(bits >> 12) + 0.5) * 2^-52, every step
 * exact in double; the smallest value is 2^-53 and the largest 1 - 2^-53.
 *
 * @param   bits  Keyed bits (mimic_sage_keyed_bits)
 * @return  The deviate
 */
static inline double mimic_sage_keyed_uniform_from_bits(const uint64_t bits) {
  return ((double)(bits >> 12) + 0.5) * 0x1.0p-52;
}

/**
 * @brief   The keyed uniform deviate of (galaxy, snapshot, purpose, ordinal), inside (0, 1)
 *
 * @param   galaxy_id  The row's UniqueGalaxyID
 * @param   snapshot   The current snapshot number
 * @param   purpose    The draw's purpose salt
 * @param   ordinal    The draw's ordinal within its key
 * @return  The deviate
 */
static inline double mimic_sage_keyed_uniform(const int64_t galaxy_id, const int snapshot,
                                              const uint64_t purpose, const uint64_t ordinal) {
  return mimic_sage_keyed_uniform_from_bits(
      mimic_sage_keyed_bits(galaxy_id, snapshot, purpose, ordinal));
}

/**
 * @brief   The CGM regime classifier's deviate: the CGM purpose and ordinal 0
 *
 * The package's replacement for SAGE26's `(double)rand() / (double)RAND_MAX`
 * (R/src/model_regimes.c:94), the one deliberate departure in the classifier.
 *
 * @param   galaxy_id  The row's UniqueGalaxyID
 * @param   snapshot   The current snapshot number (ctx->snapshot_number)
 * @return  The deviate, strictly inside (0, 1)
 */
static inline double mimic_sage_cgm_regime_deviate(const int64_t galaxy_id, const int snapshot) {
  return mimic_sage_keyed_uniform(galaxy_id, snapshot, MIMIC_SAGE_DRAW_PURPOSE_CGM,
                                  MIMIC_SAGE_DRAW_ORDINAL_CGM);
}

#endif /* MIMIC_SHARED_SAGE_KEYED_DRAW_H */
