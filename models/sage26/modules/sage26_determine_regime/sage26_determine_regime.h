/**
 * @file    sage26_determine_regime.h
 * @brief   The CGM regime classifier's per-row record and its test seam
 *
 * sage26_determine_regime ports SAGE26's determine_and_store_regime (R = the staged SAGE26
 * source, R/src/model_regimes.c:63-100). This header declares what its tests need beyond the
 * module's lifecycle entries:
 *
 * - struct Sage26RegimeRowTrace, the classifier's values for one row: SAGE26's locals Mshock,
 *   mass_ratio, x and hot_fraction, the deviate it compared, whether it requested one, and the
 *   Regime it wrote;
 * - struct Sage26RegimeSeam and sage26_determine_regime_set_test_seam(), the test seam. With a
 *   seam installed, process() takes each deviate from the seam's deviate() instead of the
 *   production keyed draw (shared/sage_keyed_draw.h), and reports every classified row to its
 *   observe(). A row that takes the no-draw branch (mass_ratio <= 0.0) never calls deviate(),
 *   so a test sees which rows requested a deviate and in what order. Without a seam (the
 *   production state, and after sage26_determine_regime_set_test_seam(NULL)) process() draws
 *   mimic_sage_cgm_regime_deviate(UniqueGalaxyID, ctx->snapshot_number) and reports nothing.
 *
 * The seam holds no random state: it only redirects where a deviate comes from. It is for
 * tests; a run never installs one.
 */

#ifndef SAGE26_DETERMINE_REGIME_H
#define SAGE26_DETERMINE_REGIME_H

/** SAGE26's sigmoid width in dex, the double local delta_log_M (R/src/model_regimes.c:85) */
#define SAGE26_REGIME_DELTA_LOG_M 0.1

/** The classifier's values for one row (each double as SAGE26 forms it) */
struct Sage26RegimeRowTrace {
  int row;                    /**< Index of the row in the FoF workspace */
  long long unique_galaxy_id; /**< The row's UniqueGalaxyID (the draw's key) */
  int snapshot;               /**< ctx->snapshot_number (the draw's key) */
  double mshock;       /**< MShockMsun * Hubble_h / 1.0e10, in 1e10 Msun/h (R/src/macros.h:47) */
  double mass_ratio;   /**< (double)(float)Mvir / Mshock (R/src/model_regimes.c:74) */
  double x;            /**< log10(mass_ratio) / delta_log_M (:89); NaN on the no-draw branch */
  double hot_fraction; /**< 1.0 / (1.0 + exp(-x)) (:93); NaN on the no-draw branch */
  double deviate;      /**< The deviate compared with hot_fraction (:94-95); NaN without one */
  int drew;            /**< 1 when the row requested a deviate (mass_ratio not <= 0.0) */
  int regime;          /**< The Regime written: 0 CGM, 1 hot (:77-98) */
};

/**
 * @brief The deviate for one row on the draw branch
 *
 * @param user  The seam's user pointer
 * @param row   The row's values so far (mshock, mass_ratio, x and hot_fraction are set; deviate,
 *              drew and regime are not yet)
 * @return The deviate the classifier compares with hot_fraction
 */
typedef double (*Sage26RegimeDeviateFn)(void *user, const struct Sage26RegimeRowTrace *row);

/** @brief Report one classified row (every field set) */
typedef void (*Sage26RegimeObserveFn)(void *user, const struct Sage26RegimeRowTrace *row);

/** The test seam: where deviates come from, and who sees each classified row */
struct Sage26RegimeSeam {
  Sage26RegimeDeviateFn deviate; /**< Required: the deviate of each drawing row */
  Sage26RegimeObserveFn observe; /**< Optional (NULL): told each classified row */
  void *user;                    /**< Passed to both */
};

/**
 * @brief Install a test seam (copied), or restore the production keyed draw with NULL
 *
 * A seam whose deviate() is NULL is treated as NULL.
 */
void sage26_determine_regime_set_test_seam(const struct Sage26RegimeSeam *seam);

#endif /* SAGE26_DETERMINE_REGIME_H */
