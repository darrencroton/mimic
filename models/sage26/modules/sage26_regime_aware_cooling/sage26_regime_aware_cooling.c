/**
 * @file    sage26_regime_aware_cooling.c
 * @brief   SAGE26 regime-aware cooling: cooling_recipe_regime_aware as one by-galaxy transaction
 *
 * Ports SAGE26's cooling_recipe_regime_aware (R = the staged SAGE26 source,
 * C = R/src/model_cooling_heating.c:420-506) whole, which SAGE26 calls instead of
 * cooling_recipe_hot when CGMrecipeOn is 1 (C:125-132, R/src/core_build_model.c:503-509). The
 * module runs by galaxy in galaxy_physics immediately after sage26_apply_cooling. With CGMrecipeOn
 * 0 it writes nothing; with CGMrecipeOn 1 sage26_calculate_cooling_budget writes only its
 * no-cooling transport, so the radio and apply modules write nothing, and this module owns
 * every galaxy's cooling:
 *
 *   mdot_cool = mdot_stream = 0                                        (C:433-434)
 *   Regime 0:    CGMgas > 0 ? cooling_recipe_cgm : reset_cgm_diagnostics  (C:436-444)
 *   otherwise:   HotGas > 0 ? cooling_recipe_hot, then save the float tcool
 *                             when Vvir > 0 && tcool > 0               (C:451-457)
 *                CGMgas > 0 ? cooling_recipe_cgm
 *                           : reset_cgm_diagnostics if the hot diagnostics are invalid (C:460-464)
 *                valid hot diagnostics: tcool = saved, tff = -1        (C:466-469)
 *   apply CGM, then hot, each capped to its post-AGN reservoir      (C:475-496)
 *   CoolingRate = rate(CGM + hot)                                    (C:498-502)
 *
 * Each recipe runs with its AGN call, through the shared composed recipes
 * (shared/sage_cooling_physics.h: mimic_sage_cooling_recipe_hot and mimic_sage_cooling_recipe_cgm),
 * so the CGM AGN reads the black-hole mass and heating radius the hot AGN changed, and Cooling and
 * Heating accumulate inside the recipes, before the application caps. Every wrapper local is local
 * to the call; nothing is carried between modules. The hot recipe's float mdot stores (C:282-283)
 * go to the raw state properties mdot_cool_raw and mdot_stream_raw, which
 * sage26_finalize_cooling_heating converts to the output mdot fields.
 *
 * Rounding points. Each expression is one C expression with SAGE26's operand order and types.
 * SAGE26's object fuses the applications' metal credits and debits (C:481, :483, :493, :495;
 * model_cooling_heating.o 0x168, 0x190, 0x200, 0x228, an fmadd or fmsub of the double metallicity
 * and the double mass with the widened float reservoir); each stays one contractible compound
 * assignment here, which clang contracts alike (the oracle README, "The c11 Group").
 *
 * Reference: Croton et al. (2006, 2016); Dekel & Birnboim (2006); Carr et al. (2023)
 */

#include <math.h>
#include <stddef.h>
#include <string.h>

#include "constants.h"
#include "error.h"
#include "globals.h"
#include "module_interface.h"
#include "module_registry.h"
#include "module_system/parameter_helpers.h"
#include "shared/metallicity.h"
#include "shared/sage_constants.h"
#include "shared/sage_cooling_physics.h"
#include "shared/time_parity.h"
#include "types.h"

#include "sage26_regime_aware_cooling.h"

// ============================================================================
// MODULE PARAMETERS
// ============================================================================

#define MODULE_NAME "sage26_regime_aware_cooling"

/* SAGE26's CGMrecipeOn (0 off, 1 on), required and checked at init */
static int CGM_RECIPE_ON;

/* SAGE26's MShockMsun, the streams' shock mass in physical Msun (no h), required, positive and
 * finite; SAGE26's default is 6.0e11 (R/src/core_read_parameter_file.c:114) */
static double MSHOCK_MSUN;

/* SAGE26's AGNrecipeOn (the package's AGNrecipe) and RadioModeEfficiency, as
 * sage26_radio_mode_heating reads them */
static int AGN_RECIPE_ON;
static double RADIO_MODE_EFFICIENCY;

// ============================================================================
// HELPER FUNCTIONS
// ============================================================================

/**
 * @brief Refuse a CGMrecipeOn other than 0 or 1, naming the parameter
 * @return 0 when supported; -1 otherwise
 */
static int validate_cgm_recipe_on(const int value) {
  if (value != 0 && value != 1) {
    ERROR_LOG("%s: CGMrecipeOn = %d is not supported: the package ports only 0 (classic hot "
              "cooling, this module writes nothing) and 1 (SAGE26's regime-aware cooling)",
              MODULE_NAME, value);
    return -1;
  }
  return 0;
}

/**
 * @brief Refuse a non-positive or non-finite MShockMsun, naming the parameter
 * @return 0 when valid; -1 otherwise
 */
static int validate_mshock_msun(const double value) {
  if (!isfinite(value) || !(value > 0.0)) {
    ERROR_LOG("%s: MShockMsun = %g is not valid: the shock mass must be a positive, finite mass "
              "in Msun",
              MODULE_NAME, value);
    return -1;
  }
  return 0;
}

/**
 * @brief With CGMrecipeOn 1, require exactly one entry, immediately after sage26_apply_cooling
 *
 * SAGE26 cools each galaxy once per substep, in cooling_recipe_regime_aware, at the point of the
 * substep where the classic trio runs (R/src/core_build_model.c:503-509): the module must have
 * exactly one entry, in a substep phase (galaxy_physics in the run files), as process_by_galaxy,
 * directly after sage26_apply_cooling's by-galaxy entry, and none in pre_timestep or
 * post_timestep. The entries are counted with the registry's phase queries; the adjacency is read
 * from the phase's entry list.
 *
 * @return 0 when placed correctly; -1 otherwise
 */
static int validate_placement(void) {
  const int pre_entries =
      module_count_phase_entries(MODULE_NAME, MimicConfig.pre_timestep,
                                 MimicConfig.num_pre_timestep, PROCESSING_MODE_BY_GALAXY, NULL);
  const int post_entries =
      module_count_phase_entries(MODULE_NAME, MimicConfig.post_timestep,
                                 MimicConfig.num_post_timestep, PROCESSING_MODE_BY_GALAXY, NULL);
  int substep_entries = 0;
  int by_galaxy_entries = 0;
  int after_apply = 0;
  for (int p = 0; p < MimicConfig.num_substep_phases; p++) {
    const struct ModulePhaseConfig *phase = &MimicConfig.substep_phases[p];
    int matching = 0;
    substep_entries += module_count_phase_entries(MODULE_NAME, phase->modules, phase->num_modules,
                                                  PROCESSING_MODE_BY_GALAXY, &matching);
    by_galaxy_entries += matching;
    for (int i = 1; i < phase->num_modules; i++) {
      const struct PhaseModuleConfig *entry = &phase->modules[i];
      const struct PhaseModuleConfig *previous = &phase->modules[i - 1];
      if (entry->module_name != NULL && previous->module_name != NULL &&
          strcmp(entry->module_name, MODULE_NAME) == 0 &&
          entry->processing_mode == PROCESSING_MODE_BY_GALAXY &&
          strcmp(previous->module_name, "sage26_apply_cooling") == 0 &&
          previous->processing_mode == PROCESSING_MODE_BY_GALAXY) {
        after_apply = 1;
      }
    }
  }
  if (pre_entries != 0 || post_entries != 0 || substep_entries != 1 || by_galaxy_entries != 1) {
    ERROR_LOG("%s must be configured exactly once, as process_by_galaxy in a substep phase "
              "(galaxy_physics in the run files), with CGMrecipeOn 1 (found %d substep-phase "
              "entries, %d as process_by_galaxy, %d in pre_timestep, %d in post_timestep): SAGE26 "
              "cools each galaxy once per substep",
              MODULE_NAME, substep_entries, by_galaxy_entries, pre_entries, post_entries);
    return -1;
  }
  if (!after_apply) {
    ERROR_LOG("%s must run immediately after sage26_apply_cooling (process_by_galaxy) in the same "
              "substep phase, with CGMrecipeOn 1: it takes the classic cooling trio's place "
              "(R/src/core_build_model.c:503-509)",
              MODULE_NAME);
    return -1;
  }
  return 0;
}

/**
 * @brief SAGE26's static reset_cgm_diagnostics (C:413-418)
 *
 * Writes exactly tcool 0, tff -1 and RcoolToRvir -1; no mass or energy.
 */
static void reset_cgm_diagnostics(struct GalaxyData *galaxy) {
  galaxy->tcool = 0.0f;
  galaxy->tff = -1.0f;
  galaxy->RcoolToRvir = -1.0f;
}

/** @brief A trace with every field at its "not reached" value */
static void trace_init(struct Sage26RegimeAwareCoolingTrace *t) {
  memset(t, 0, sizeof(*t));
  t->hot_tcool = -1.0f;
  t->cgm_apply = SAGE26_RAC_APPLY_NOT_REACHED;
  t->hot_apply = SAGE26_RAC_APPLY_NOT_REACHED;
  t->cgm_metallicity = NAN;
  t->hot_metallicity = NAN;
  t->cgm_reservoir = NAN;
  t->hot_reservoir = NAN;
  t->hot_agn.suppressed = -1;
  t->cgm_agn.suppressed = -1;
}

// ============================================================================
// THE TRANSACTION
// ============================================================================

int sage26_regime_aware_cooling_run(struct Halo *halo, const struct MimicConfig *params,
                                    const struct Sage26RegimeAwareCoolingConfig *cfg,
                                    const double dt, struct Sage26RegimeAwareCoolingTrace *trace) {
  struct GalaxyData *galaxy = halo->galaxy;
  struct Sage26RegimeAwareCoolingTrace local;
  struct Sage26RegimeAwareCoolingTrace *t = (trace != NULL) ? trace : &local;
  trace_init(t);

  /* C:422-425 */
  double cgm_cooling = 0.0;
  double hot_cooling = 0.0;
  float hot_tcool = -1.0f;
  int hot_diagnostics_valid = 0;

  // SAGE parity: the float mdot rates are reset on every regime-aware call and are assigned only
  // on the hot recipe's cold-stream path below (R/src/model_cooling_heating.c:433-434, :282-283);
  // here they are the raw state properties
  galaxy->mdot_cool_raw = 0.0f;
  galaxy->mdot_stream_raw = 0.0f;

  if (galaxy->Regime == 0) {
    /* C:436-444: the CGM regime cools its CGM pair only */
    t->cgm_regime = 1;
    if (galaxy->CGMgas > 0.0) {
      cgm_cooling = mimic_sage_cooling_recipe_cgm(halo, params, &cfg->agn, cfg->cgm_recipe_on, dt,
                                                  &t->cgm, &t->cgm_agn);
      t->cgm_called = 1;
    } else {
      reset_cgm_diagnostics(galaxy);
      t->reset_called = 1;
    }
  } else {
    /* C:450-457: the hot recipe and its AGN, then the hot diagnostics it left */
    if (galaxy->HotGas > 0.0) {
      if (mimic_sage_cooling_recipe_hot(halo, params, &cfg->agn, cfg->cgm_recipe_on,
                                        cfg->mshock_msun, dt, &t->hot, &t->hot_agn,
                                        &hot_cooling) != 0) {
        return -1; /* the stored SnapNum has no redshift */
      }
      t->hot_called = 1;
      if (t->hot.branch == MIMIC_SAGE_HOT_BRANCH_STREAMS) {
        /* C:282-283: the streams' float stores, before the cap and the AGN */
        galaxy->mdot_cool_raw = t->hot.stream.mdot_cool;
        galaxy->mdot_stream_raw = t->hot.stream.mdot_stream;
      }
      // SAGE parity: "valid" needs only Vvir > 0 and the float tcool > 0 after the hot call, not
      // a positive cooling mass or a completed lambda or rho_rcool step
      // (R/src/model_cooling_heating.c:453-456)
      if ((float)halo->Vvir > 0.0 && galaxy->tcool > 0.0) {
        hot_tcool = galaxy->tcool;
        hot_diagnostics_valid = 1;
      }
    }

    // SAGE parity: residual CGM gas cools in a hot-regime galaxy too, after the hot AGN changed
    // the black-hole mass and heating radius the CGM AGN reads (R/src/model_cooling_heating.c:
    // 459-461); without CGM gas the diagnostics are reset only when the hot ones are invalid
    // (:462-464)
    if (galaxy->CGMgas > 0.0) {
      cgm_cooling = mimic_sage_cooling_recipe_cgm(halo, params, &cfg->agn, cfg->cgm_recipe_on, dt,
                                                  &t->cgm, &t->cgm_agn);
      t->cgm_called = 1;
    } else if (!hot_diagnostics_valid) {
      reset_cgm_diagnostics(galaxy);
      t->reset_called = 1;
    }

    // SAGE parity: valid hot diagnostics restore the saved float tcool and set tff to -1, even
    // after CGM cooling wrote its own (R/src/model_cooling_heating.c:466-469)
    if (hot_diagnostics_valid) {
      galaxy->tcool = hot_tcool;
      galaxy->tff = -1.0f;
      t->restored = 1;
    }
  }
  t->hot_diagnostics_valid = hot_diagnostics_valid;
  t->hot_tcool = hot_tcool;
  t->hot_returned = hot_cooling;
  t->cgm_returned = cgm_cooling;

  // SAGE parity: the CGM mass is applied before the hot mass, each capped to its reservoir as the
  // AGN left it and moved with that reservoir's metallicity, without the classic transfer's
  // "move all metals on equality" branch (R/src/model_cooling_heating.c:475-496, :689-705)
  if (cgm_cooling > 0.0) {
    t->cgm_apply = SAGE26_RAC_APPLY_UNCAPPED;
    t->cgm_reservoir = galaxy->CGMgas;
    if (cgm_cooling > galaxy->CGMgas) { /* C:476-478 */
      cgm_cooling = galaxy->CGMgas;
      t->cgm_apply = SAGE26_RAC_APPLY_CAPPED;
    }
    const double metallicity = mimic_get_metallicity(galaxy->CGMgas, galaxy->MetalsCGMgas);
    galaxy->ColdGas += cgm_cooling;                     /* C:480 */
    galaxy->MetalsColdGas += metallicity * cgm_cooling; /* C:481, fused in SAGE26's object */
    galaxy->CGMgas -= cgm_cooling;                      /* C:482 */
    galaxy->MetalsCGMgas -= metallicity * cgm_cooling;  /* C:483, fused in SAGE26's object */
    t->cgm_metallicity = metallicity;
  }

  if (hot_cooling > 0.0) {
    t->hot_apply = SAGE26_RAC_APPLY_UNCAPPED;
    t->hot_reservoir = galaxy->HotGas;
    if (hot_cooling > galaxy->HotGas) { /* C:488-490 */
      hot_cooling = galaxy->HotGas;
      t->hot_apply = SAGE26_RAC_APPLY_CAPPED;
    }
    const double metallicity = mimic_get_metallicity(galaxy->HotGas, galaxy->MetalsHotGas);
    galaxy->ColdGas += hot_cooling;                     /* C:492 */
    galaxy->MetalsColdGas += metallicity * hot_cooling; /* C:493, fused in SAGE26's object */
    galaxy->HotGas -= hot_cooling;                      /* C:494 */
    galaxy->MetalsHotGas -= metallicity * hot_cooling;  /* C:495, fused in SAGE26's object */
    t->hot_metallicity = metallicity;
  }
  t->cgm_applied = cgm_cooling;
  t->hot_applied = hot_cooling;

  // SAGE parity: CoolingRate is always overwritten with the total applied, after both caps, even
  // where a nested recipe returned early (R/src/model_cooling_heating.c:498-502)
  const double total_cooling = cgm_cooling + hot_cooling;
  galaxy->CoolingRate = mimic_sage_cooling_rate(total_cooling, dt, params);
  t->total_cooling = total_cooling;

  // SAGE parity: SAGE26 ends the wrapper with XASSERT(total_cooling >= 0.0)
  // (R/src/model_cooling_heating.c:503-504), which is not ported: from a valid state it cannot
  // fire. Each recipe returns a non-negative mass: the CGM recipe's floor (:366-369), its
  // suppression factor with r_heat < rcool (:638-643) and its re-cap to the non-negative CGMgas
  // (:675-677); the hot recipe's cap and floor (:288-292), checked by its own XASSERT (:307-308).
  // A skipped application adds that unchanged non-negative term to the total (:498), and an
  // applied one caps a positive mass to its non-negative reservoir, so the total is non-negative
  return 0;
}

// ============================================================================
// MODULE LIFECYCLE FUNCTIONS
// ============================================================================

int sage26_regime_aware_cooling_init(void) {
  /* Required, no default: a missing key fails here */
  LOAD_PARAM_INT("CGMrecipeOn", CGM_RECIPE_ON);
  if (validate_cgm_recipe_on(CGM_RECIPE_ON) != 0) {
    return -1;
  }
  LOAD_PARAM_DOUBLE("MShockMsun", MSHOCK_MSUN);
  if (validate_mshock_msun(MSHOCK_MSUN) != 0) {
    return -1;
  }
  LOAD_AND_VALIDATE_RANGE_INCLUSIVE("RadioModeEfficiency", RADIO_MODE_EFFICIENCY, 0.0, 1.0,
                                    "AGN radio mode heating efficiency");
  LOAD_AND_VALIDATE_OPTION("AGNrecipe", AGN_RECIPE_ON, 3,
                           "0=off, 1=empirical, 2=Bondi, 3=cold cloud");
  if (CGM_RECIPE_ON == 1 && validate_placement() != 0) {
    return -1;
  }

  VERBOSE_LOG("SAGE regime-aware cooling module initialized");
  VERBOSE_LOG("  CGMrecipeOn = %d%s", CGM_RECIPE_ON,
              CGM_RECIPE_ON == 0 ? " (writes nothing: the classic cooling modules cool)" : "");
  VERBOSE_LOG("  MShockMsun = %.6e Msun", MSHOCK_MSUN);
  VERBOSE_LOG("  AGNrecipe = %d, RadioModeEfficiency = %.4f", AGN_RECIPE_ON, RADIO_MODE_EFFICIENCY);
  return 0;
}

int sage26_regime_aware_cooling_process(struct ModuleContext *ctx, struct Halo *halos, int ngal) {
  // SAGE parity: with CGMrecipeOn 0 SAGE26 calls cooling_recipe_hot, which the classic modules
  // run (R/src/model_cooling_heating.c:128-131); this module writes nothing
  if (CGM_RECIPE_ON == 0) {
    return 0;
  }

  if (ngal != 1) {
    ERROR_LOG("process_by_galaxy expects ngal=1, got %d", ngal);
    return -1;
  }

  struct Halo *halo = &halos[0];

  // SAGE parity: cooling runs for every non-merged galaxy, Type 2 orphans included: SAGE26 calls
  // cooling_recipe for every galaxy with mergeType 0 before the merger pass
  // (R/src/core_build_model.c:473-516)
  if (halo->galaxy == NULL) {
    return 0;
  }

  double dt_obj = 0.0;
  const enum MimicObjectTimeStatus dt_status = mimic_object_substep_dt(halo, ctx, &dt_obj);
  if (dt_status == MIMIC_OBJECT_TIME_SKIP_INITIAL) {
    // No positive interval (the core's snapshot-zero birth; SAGE26 is undefined there, reading
    // Age[-1], R/src/core_build_model.c:479): as the classic trio with CGMrecipeOn 0, which
    // writes only its transport there, the transaction reads no table and writes nothing
    return 0;
  }
  if (dt_status != MIMIC_OBJECT_TIME_OK) {
    ERROR_LOG("Invalid regime-aware cooling dt for halo %lld (SnapNum=%d, dT=%.3e, "
              "num_substeps=%d, status=%s)",
              halo->HaloNr, halo->SnapNum, halo->dT, (ctx != NULL) ? ctx->num_substeps : -1,
              mimic_object_time_status_str(dt_status));
    return -1;
  }

  struct Sage26RegimeAwareCoolingConfig cfg;
  cfg.cgm_recipe_on = CGM_RECIPE_ON;
  cfg.mshock_msun = MSHOCK_MSUN;
  cfg.agn.recipe = AGN_RECIPE_ON;
  cfg.agn.efficiency = RADIO_MODE_EFFICIENCY;
  if (sage26_regime_aware_cooling_run(halo, ctx->params, &cfg, dt_obj, NULL) != 0) {
    ERROR_LOG("%s: halo %lld (SnapNum=%d) refused by the hot regime's cold streams (campaign "
              "decision D35)",
              MODULE_NAME, halo->HaloNr, halo->SnapNum);
    return -1;
  }
  return 0;
}

int sage26_regime_aware_cooling_cleanup(void) {
  VERBOSE_LOG("SAGE regime-aware cooling module cleaned up");
  return 0;
}
