/**
 * @file sage_cgm_routing.h
 * @brief SAGE26's regime-aware hot-phase deposits (CGMrecipeOn 1) and the required CGMrecipeOn
 *        parameter's check
 *
 * Ports SAGE26's add_metals_to_hot_reservoir and add_gas_to_hot_reservoir (R = the staged SAGE26
 * source, R/src/model_misc.h:60-85): a deposit into a galaxy's hot-phase reservoir goes to its
 * CGM pair (CGMgas, MetalsCGMgas) when the CGM recipe is on (CGMrecipeOn 1) and the galaxy is in
 * the CGM regime (Regime 0), and to its hot pair (HotGas, MetalsHotGas) otherwise. With
 * CGMrecipeOn 0 every deposit is the hot pair's, whatever Regime holds, which is SAGE26's R0-R3
 * behaviour.
 *
 * - mimic_sage_cgm_reservoir_selected(): SAGE26's selection, CGMrecipeOn == 1 && Regime == 0
 *   (R/src/model_misc.h:69, :78).
 * - mimic_sage_add_metals_to_hot_reservoir(): add_metals_to_hot_reservoir (:67-74).
 * - mimic_sage_add_gas_to_hot_reservoir(): add_gas_to_hot_reservoir (:76-85).
 * - mimic_sage_validate_cgm_recipe_on(): the required CGMrecipeOn, 0 or 1, refused otherwise with
 *   an error naming the parameter.
 * - mimic_sage_validate_mshock_msun(): the required MShockMsun, positive and finite, refused
 *   otherwise with an error naming the parameter.
 *
 * Rounding points. Each helper takes an already completed double and adds it to the selected
 * float field, a double sum narrowed once on the store, exactly as SAGE26's helpers do: a caller's
 * product such as metallicity * reincorporated is evaluated as the double argument before the
 * call, so it is never contracted into the addition (SAGE26's reincorporate_gas multiplies at
 * R/src/model_reincorporation.c:64 and adds with a separate fadd at R/src/model_misc.h:82 in its
 * object; the oracle README's "The c10 Group"). Clang contracts only within one C expression, and
 * an inlined call does not merge its argument's expression with the callee's.
 *
 * The selection reads the galaxy passed: the caller chooses which galaxy's regime routes the
 * deposit (the FoF central for infall and reincorporation, the passed recipient for disruption),
 * as SAGE26's callers do.
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

/** SAGE26's CGMrecipeOn values the package ports (R/src/core_allvars.h:485; SAGE26 accepts 0 and
 * 1, R/src/core_read_parameter_file.c:526) */
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
    ERROR_LOG("%s: CGMrecipeOn = %d is not supported: the package ports only 0 (hot-gas "
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
 * @brief Whether a deposit routed by this regime goes to the CGM pair (R/src/model_misc.h:69, :78)
 *
 * @param cgm_recipe_on  The module's CGMrecipeOn
 * @param regime         The routing galaxy's Regime
 * @return 1 for the CGM pair (CGMrecipeOn 1 and Regime 0); 0 for the hot pair
 */
static inline int mimic_sage_cgm_reservoir_selected(const int cgm_recipe_on, const int regime) {
  return cgm_recipe_on == MIMIC_SAGE_CGM_RECIPE_ON && regime == MIMIC_SAGE_REGIME_CGM;
}

/**
 * @brief SAGE26's add_metals_to_hot_reservoir (R/src/model_misc.h:67-74)
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
 * @brief SAGE26's add_gas_to_hot_reservoir (R/src/model_misc.h:76-85)
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
