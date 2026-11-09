/**
 * @file sage_cgm_routing.h
 * @brief SAGE26's regime-aware hot-phase deposits (CGMrecipeOn 1) and the required CGMrecipeOn
 *        parameter's check
 *
 * Implements SAGE26's add_metals_to_hot_reservoir and add_gas_to_hot_reservoir: a deposit into a
 * galaxy's hot-phase reservoir goes to its CGM pair (CGMgas, MetalsCGMgas) when the CGM recipe is
 * on (CGMrecipeOn 1) and the galaxy is in the CGM regime (Regime 0), and to its hot pair (HotGas,
 * MetalsHotGas) otherwise. With CGMrecipeOn 0 every deposit goes to the hot pair, whatever Regime
 * holds. Also validates the required CGMrecipeOn and MShockMsun parameters.
 *
 * Each helper adds an already completed double to the selected float field, narrowing once on the
 * store, as SAGE26's helpers do. The caller chooses which galaxy's regime routes the deposit (the
 * FoF central for infall and reincorporation, the passed recipient for disruption).
 *
 * @note This file is part of the model-local shared utilities system.
 *       Modules include it as #include "shared/sage_cgm_routing.h"
 *
 * Ported from SAGE26 (MIT; Copyright (c) 2019-2026 Semi-Analytic Galaxy Evolution (SAGE)); see
 * models/sage26/LICENSE-SAGE26.
 */

#ifndef MIMIC_SHARED_SAGE_CGM_ROUTING_H
#define MIMIC_SHARED_SAGE_CGM_ROUTING_H

#include <math.h>

#include "error.h"
#include "types.h"

/** SAGE26's CGMrecipeOn values: 0 routes every deposit to the hot pair, 1 routes by regime */
#define MIMIC_SAGE_CGM_RECIPE_OFF 0
#define MIMIC_SAGE_CGM_RECIPE_ON 1

/** SAGE26's CGM regime value (Regime 0; Regime 1 is the hot-halo regime) */
#define MIMIC_SAGE_REGIME_CGM 0

/**
 * @brief Refuse a CGMrecipeOn the package does not support
 *
 * CGMrecipeOn is a required package parameter, 0 or 1; each module that selects a reservoir by
 * regime loads it with the required-parameter path and passes it here.
 *
 * @param module_name  The calling module, for the error message
 * @param value        The loaded CGMrecipeOn
 * @return 0 for 0 or 1; -1 (with an error naming the parameter) otherwise
 */
static inline int mimic_sage_validate_cgm_recipe_on(const char *module_name, const int value) {
  if (value != MIMIC_SAGE_CGM_RECIPE_OFF && value != MIMIC_SAGE_CGM_RECIPE_ON) {
    ERROR_LOG("%s: CGMrecipeOn = %d is not supported: the package accepts only 0 (hot-gas "
              "routing) and 1 (SAGE26's regime-aware CGM routing)",
              module_name, value);
    return -1;
  }
  return 0;
}

/**
 * @brief Refuse a non-positive or non-finite MShockMsun
 *
 * MShockMsun, the Dekel & Birnboim (2006) shock mass in physical Msun, is a required package
 * parameter of the CGM regime classifier and the regime-aware cooling; each loads it with the
 * required-parameter path and passes it here.
 *
 * @param module_name  The calling module, for the error message
 * @param value        The loaded MShockMsun
 * @return 0 for a positive, finite mass; -1 (with an error naming the parameter) otherwise
 */
static inline int mimic_sage_validate_mshock_msun(const char *module_name, const double value) {
  if (!isfinite(value) || !(value > 0.0)) {
    ERROR_LOG("%s: MShockMsun = %g is not valid: the shock mass must be a positive, finite mass "
              "in Msun",
              module_name, value);
    return -1;
  }
  return 0;
}

/**
 * @brief Whether a deposit routed by this regime goes to the CGM pair, as SAGE26's helpers select
 *
 * @param cgm_recipe_on  The module's CGMrecipeOn
 * @param regime         The routing galaxy's Regime
 * @return 1 for the CGM pair (CGMrecipeOn 1 and Regime 0); 0 for the hot pair
 */
static inline int mimic_sage_cgm_reservoir_selected(const int cgm_recipe_on, const int regime) {
  return cgm_recipe_on == MIMIC_SAGE_CGM_RECIPE_ON && regime == MIMIC_SAGE_REGIME_CGM;
}

/**
 * @brief SAGE26's add_metals_to_hot_reservoir
 *
 * @param galaxy         The galaxy whose regime routes the deposit and which receives it
 * @param cgm_recipe_on  The module's CGMrecipeOn
 * @param metals         The completed double metal mass, added to the selected float field
 */
static inline void mimic_sage_add_metals_to_hot_reservoir(struct GalaxyData *galaxy,
                                                          const int cgm_recipe_on,
                                                          const double metals) {
  if (mimic_sage_cgm_reservoir_selected(cgm_recipe_on, galaxy->Regime)) {
    galaxy->MetalsCGMgas += metals;
  } else {
    galaxy->MetalsHotGas += metals;
  }
}

/**
 * @brief SAGE26's add_gas_to_hot_reservoir
 *
 * The gas is added before the metals, each a double sum narrowed once into its float field.
 *
 * @param galaxy         The galaxy whose regime routes the deposit and which receives it
 * @param cgm_recipe_on  The module's CGMrecipeOn
 * @param gas            The completed double gas mass
 * @param metals         The completed double metal mass
 */
static inline void mimic_sage_add_gas_to_hot_reservoir(struct GalaxyData *galaxy,
                                                       const int cgm_recipe_on, const double gas,
                                                       const double metals) {
  if (mimic_sage_cgm_reservoir_selected(cgm_recipe_on, galaxy->Regime)) {
    galaxy->CGMgas += gas;
    galaxy->MetalsCGMgas += metals;
  } else {
    galaxy->HotGas += gas;
    galaxy->MetalsHotGas += metals;
  }
}

#endif /* MIMIC_SHARED_SAGE_CGM_ROUTING_H */
