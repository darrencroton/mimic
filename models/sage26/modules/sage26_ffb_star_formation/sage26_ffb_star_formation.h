/**
 * @file    sage26_ffb_star_formation.h
 * @brief   Test observer of sage26_ffb_star_formation: the transaction's carried values
 *
 * SAGE26's starformation_ffb keeps every value it carries between its steps in locals, and so
 * does the package's transaction. Tests read them through this observer, which the module calls
 * once per call that reaches the transaction, after the call's last write. It is inactive in a
 * run (no observer is set) and never changes what the module computes.
 */

#ifndef SAGE26_FFB_STAR_FORMATION_H
#define SAGE26_FFB_STAR_FORMATION_H

#include "shared/sage_ffb_feedback_physics.h"
#include "shared/sage_metal_yield_physics.h"
#include "shared/sage_sf_feedback_physics.h"

/** @brief How far one call of the transaction went */
enum Sage26FfbPath {
  SAGE26_FFB_PATH_INITIAL_BOUNDARY = 1, /**< MIMIC_OBJECT_TIME_SKIP_INITIAL: nothing written */
  SAGE26_FFB_PATH_NO_CENTRAL = 2,       /**< no FoF central: H2gas, H1gas and the history sample
                                             written, nothing committed */
  SAGE26_FFB_PATH_COMPLETE = 3          /**< the whole transaction */
};

/**
 * @brief The values one call of the transaction formed (NAN, or -1 for a flag, where its path
 *        does not form them)
 */
struct Sage26FfbStarFormationTrace {
  enum Sage26FfbPath path;
  double dt;             /**< the substep interval dT / num_substeps */
  int bin;               /**< the substep's history bin */
  double redshift;       /**< ZZ of the row's stored SnapNum */
  double reff;           /**< StarFormingDiskFactor * DiskScaleRadius */
  double tdyn;           /**< reff / Vvir, or 0 */
  int nonfinite_guard;   /**< ColdGas, Vvir, reff or tdyn NaN or infinite */
  int tdyn_gas_positive; /**< tdyn > 0 && gas > 0, -1 if not evaluated */
  double strdot;         /**< EnhancedSFEfficiency * gas / tdyn */
  int strdot_rejected;   /**< strdot NaN, infinite or negative, -1 if not formed */
  double stars_uncapped; /**< strdot * dt */
  int stars_cap_binds;   /**< strdot * dt > ColdGas, -1 if not formed */
  int stars_rejected;    /**< the capped stars NaN, infinite or negative */
  double stars_formed;   /**< the stars sampled and committed */
  double metallicity;    /**< the cold-gas metallicity before the commit, reused by the
                              feedback commit */
  struct MimicSageHydrogenCapLocals sf_caps;  /**< the star-formation commit's caps */
  struct MimicSageFfbFeedbackLocals feedback; /**< compute_sn_feedback_ffb's locals */
  double reheated_from_budget;                /**< the budget's reheated mass */
  int reheat_clamp_binds;                     /**< reheated > ColdGas at the second clamp */
  double reheated_to_commit;                  /**< the reheated mass passed to the commit */
  double ejected_to_commit;                   /**< the ejected mass passed to the commit */
  struct MimicSageFeedbackLocals commit;      /**< update_from_feedback's locals */
  double yield_stars;                         /**< the renormalised stars of the yield */
  struct MimicSageDiskYieldLocals yield;      /**< the yield's locals */
};

/** @brief Called once per call that reaches the transaction (never at EnhancedStarFormationOn 0
 *         or on a row whose FFBRegime is not 1) */
typedef void (*Sage26FfbObserver)(void *user, const struct Sage26FfbStarFormationTrace *trace);

/** @brief Set (or clear, with NULL) the test observer; inactive in a run */
void sage26_ffb_star_formation_set_test_observer(Sage26FfbObserver observer, void *user);

#endif /* SAGE26_FFB_STAR_FORMATION_H */
