/**
 * @file    sage26_regime_aware_cooling.h
 * @brief   SAGE26's regime-aware cooling transaction, exported for the module's tests
 *
 * sage26_regime_aware_cooling ports SAGE26's cooling_recipe_regime_aware (R = the staged SAGE26
 * source, C = R/src/model_cooling_heating.c:420-506) whole, as one by-galaxy transaction
 * (campaign decision D38). This header declares what its tests need beyond the module's
 * lifecycle entries:
 *
 * - struct Sage26RegimeAwareCoolingConfig, the run quantities the transaction reads
 *   (CGMrecipeOn, MShockMsun and the AGN configuration);
 * - struct Sage26RegimeAwareCoolingTrace, SAGE26's wrapper locals and the locals of each nested
 *   call, for the oracle and behavioural tests;
 * - sage26_regime_aware_cooling_run(), the transaction on one galaxy with an explicit dt, which
 *   process() calls once per galaxy with CGMrecipeOn 1.
 *
 * Test-facing: the module's process() is the only production caller.
 */

#ifndef SAGE26_REGIME_AWARE_COOLING_H
#define SAGE26_REGIME_AWARE_COOLING_H

#include "shared/sage_cooling_physics.h"
#include "types.h"

/** @brief The run quantities the transaction reads */
struct Sage26RegimeAwareCoolingConfig {
  int cgm_recipe_on;             /**< CGMrecipeOn; the transaction runs only with 1 */
  double mshock_msun;            /**< MShockMsun, the streams' shock mass in physical Msun */
  struct MimicSageAgnConfig agn; /**< AGNrecipe and RadioModeEfficiency */
};

/** @brief How an application cap of C:476-478 or C:488-490 ended */
#define SAGE26_RAC_APPLY_NOT_REACHED -1 /* the contribution was not positive: no apply */
#define SAGE26_RAC_APPLY_UNCAPPED 0
#define SAGE26_RAC_APPLY_CAPPED 1 /* capped to the post-AGN reservoir */

/** @brief SAGE26's wrapper locals of one call and the locals of its nested calls */
struct Sage26RegimeAwareCoolingTrace {
  int cgm_regime;            /**< 1 when Regime == 0 took the CGM branch (C:436) */
  int hot_called;            /**< 1 when cooling_recipe_hot ran (HotGas > 0, C:451-452) */
  int hot_diagnostics_valid; /**< C:453-456: Vvir > 0 && tcool > 0 after the hot call */
  float hot_tcool;           /**< the saved float tcool (C:454); -1 when not saved (C:424) */
  int cgm_called;            /**< 1 when cooling_recipe_cgm ran (CGMgas > 0, C:440-441, :460-461) */
  int reset_called;          /**< 1 when reset_cgm_diagnostics ran (C:442-443, :462-463) */
  int restored;              /**< 1 when the saved tcool was restored with tff -1 (C:466-469) */
  double hot_returned;       /**< cooling_recipe_hot's return; 0 when not called */
  double cgm_returned;       /**< cooling_recipe_cgm's return; 0 when not called */
  int cgm_apply;             /**< SAGE26_RAC_APPLY_* of the CGM application (C:475-484) */
  int hot_apply;             /**< SAGE26_RAC_APPLY_* of the hot application (C:487-496) */
  double cgm_metallicity;    /**< get_metallicity of the post-AGN CGM pair (C:479); NaN if none */
  double hot_metallicity;    /**< get_metallicity of the post-AGN hot pair (C:491); NaN if none */
  double cgm_reservoir;      /**< the CGMgas the CGM cap compared (C:476); NaN if none */
  double hot_reservoir;      /**< the HotGas the hot cap compared (C:488); NaN if none */
  double cgm_applied;        /**< cgm_cooling after its cap (C:476-478) */
  double hot_applied;        /**< hot_cooling after its cap (C:488-490) */
  double total_cooling;      /**< cgm_cooling + hot_cooling (C:498) */
  struct MimicSageHotCoolingLocals hot; /**< valid when hot_called */
  struct MimicSageAgnLocals hot_agn;    /**< valid when hot_called; suppressed -1 without AGN */
  struct MimicSageCgmCoolingLocals cgm; /**< valid when cgm_called */
  struct MimicSageAgnLocals cgm_agn;    /**< valid when cgm_called; suppressed -1 without AGN */
};

/**
 * @brief SAGE26's cooling_recipe_regime_aware on one galaxy (C:420-506)
 *
 * The whole transaction with CGMrecipeOn 1, through the shared helpers
 * (shared/sage_cooling_physics.h): the mdot reset, the regime branch with the hot recipe and its
 * AGN, the saved hot diagnostics, the CGM recipe and its AGN (reading the BH mass and heating
 * radius the hot AGN changed), the diagnostic reset or restoration, the CGM then the hot
 * application, each capped to its post-AGN reservoir with its own metallicity, and the final
 * CoolingRate. The raw float mdot rates go to mdot_cool_raw and mdot_stream_raw (campaign
 * decision D39). The cooling tables must be loaded (sage26_calculate_cooling_budget_init).
 *
 * @param halo    Halo whose galaxy cools (galaxy not NULL)
 * @param params  Run configuration (units, Hubble_h, G, Omega, ZZ, Snaplistlen)
 * @param cfg     The transaction's run quantities
 * @param dt      Substep interval, SAGE26's deltaT / effective_steps (positive)
 * @param trace   Optional trace (may be NULL)
 * @return 0, or -1 when the hot regime's streams refuse the stored SnapNum (campaign decision
 *         D35); the galaxy then holds the hot budget's partial diagnostic writes and the caller
 *         fails the run
 */
int sage26_regime_aware_cooling_run(struct Halo *halo, const struct MimicConfig *params,
                                    const struct Sage26RegimeAwareCoolingConfig *cfg,
                                    const double dt, struct Sage26RegimeAwareCoolingTrace *trace);

#endif /* SAGE26_REGIME_AWARE_COOLING_H */
