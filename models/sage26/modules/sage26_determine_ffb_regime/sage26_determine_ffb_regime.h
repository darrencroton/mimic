/**
 * @file    sage26_determine_ffb_regime.h
 * @brief   The FFB classifier's per-row record and its test seam
 *
 * sage26_determine_ffb_regime ports SAGE26's determine_and_store_ffb_regime at
 * EnhancedStarFormationOn 1 (R = the staged SAGE26 source, R/src/model_regimes.c:188-240). This
 * header declares what its tests need beyond the module's lifecycle entries:
 *
 * - struct Sage26FfbRowTrace, the classifier's values for one row: the keys of its draw
 *   (MostBoundID and the current snapshot), the current redshift, the widened float Mvir, the
 *   fraction's locals (shared/sage_ffb_regime.h), the fraction, the draw's 64 hashed bits and
 *   deviate, the deviate compared and the FFBRegime written;
 * - struct Sage26FfbSeam and sage26_determine_ffb_regime_set_test_seam(), the test seam. With a
 *   seam installed, process() still forms SAGE26's draw (reported in draw_bits and draw) but
 *   compares the seam's deviate() instead when one is given, and reports every classified row to
 *   observe(). Without a seam (the production state, and after
 *   sage26_determine_ffb_regime_set_test_seam(NULL)) process() compares SAGE26's draw and reports
 *   nothing.
 *
 * The seam holds no random state: it only lets a test place a deviate exactly at or next to the
 * fraction (the tie draw == f_ffb, which no MostBoundID can be chosen to give), and observe each
 * row. It is for tests; a run never installs one.
 */

#ifndef SAGE26_DETERMINE_FFB_REGIME_H
#define SAGE26_DETERMINE_FFB_REGIME_H

#include <stdint.h>

#include "shared/sage_ffb_regime.h"

/** The classifier's values for one row (each double as SAGE26 forms it) */
struct Sage26FfbRowTrace {
  int row;                 /**< Index of the row in the FoF workspace */
  long long most_bound_id; /**< The row's MostBoundID (the draw's key) */
  int snapshot;            /**< ctx->snapshot_number (the draw's key, halo_snapnum) */
  double redshift;         /**< ctx->redshift (SAGE26's Zcurr) */
  double mvir;             /**< (double)(float)Mvir, the float SAGE26 holds (:229) */
  struct MimicSageFfbFractionLocals fraction; /**< The fraction's locals (:321-341) */
  double f_ffb;                               /**< calculate_ffb_fraction(Mvir, Zcurr) (:232) */
  uint64_t draw_bits;                         /**< halo_uniform's 64 hashed bits (:153-157) */
  double draw;                                /**< halo_uniform(MostBoundID, snapnum) (:225) */
  double deviate; /**< The deviate compared with f_ffb: draw, or the seam's */
  int ffb_regime; /**< The FFBRegime written (:236-240) */
};

/**
 * @brief The deviate to compare for one row in place of SAGE26's draw
 *
 * @param user  The seam's user pointer
 * @param row   The row's values so far (every field but deviate and ffb_regime is set)
 * @return The deviate the classifier compares with f_ffb
 */
typedef double (*Sage26FfbDeviateFn)(void *user, const struct Sage26FfbRowTrace *row);

/** @brief Report one classified row (every field set) */
typedef void (*Sage26FfbObserveFn)(void *user, const struct Sage26FfbRowTrace *row);

/** The test seam: an optional deviate substitute, and who sees each classified row */
struct Sage26FfbSeam {
  Sage26FfbDeviateFn deviate; /**< Optional (NULL keeps SAGE26's draw) */
  Sage26FfbObserveFn observe; /**< Optional (NULL): told each classified row */
  void *user;                 /**< Passed to both */
};

/**
 * @brief Install a test seam (copied), or remove it with NULL
 *
 * A seam with neither deviate() nor observe() is treated as NULL.
 */
void sage26_determine_ffb_regime_set_test_seam(const struct Sage26FfbSeam *seam);

#endif /* SAGE26_DETERMINE_FFB_REGIME_H */
