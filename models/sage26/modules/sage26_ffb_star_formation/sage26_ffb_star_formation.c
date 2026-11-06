/**
 * @file    sage26_ffb_star_formation.c
 * @brief   SAGE26 feedback-free-burst star formation - the whole starformation_ffb transaction
 *          (star formation, history, commit, FFB feedback, commit and yield) for one FFB row
 *
 * Ports SAGE26's starformation_ffb (R/src/model_starformation_and_feedback.c:1262-1368, R = the
 * staged SAGE26 source, S = that file), with its static compute_sn_feedback_ffb (S:901-972,
 * shared/sage_ffb_feedback_physics.h), which SAGE26's starformation_and_feedback calls in place
 * of its whole ordinary disk chain for a row whose FFBRegime is 1 when EnhancedStarFormationOn is
 * at least 1, and returns (S:1004-1008). The module runs by galaxy in a substep phase,
 * immediately before sage26_calculate_star_formation in the run files. With
 * EnhancedStarFormationOn 0 it writes nothing; with 1 it writes nothing on a row whose FFBRegime
 * is not 1, and on an FFB row runs, in SAGE26's order:
 *
 *   reff = StarFormingDiskFactor * DiskScaleRadius; tdyn = reff / Vvir or 0   (S:1273-1274)
 *   H2gas = 0; the H I block                                                  (S:1280-1292)
 *   stars = min(EnhancedSFEfficiency * ColdGas / tdyn * dt, ColdGas), with
 *           SAGE26's NaN, infinity and sign guards                            (S:1297-1320)
 *   the disk history sampled from these stars and the cold gas              (S:1325-1327)
 *   metallicity = Z(cold gas); update_from_star_formation(stars, metallicity) (S:1329-1330)
 *   compute_sn_feedback_ffb against the remaining cold gas, which may reduce
 *           the local stars and the reheated mass                            (S:1344)
 *   the reheated mass clamped to the remaining cold gas                      (S:1346)
 *   update_from_feedback(reheated, ejected, the same metallicity)            (S:1348)
 *   the yield from the renormalised stars                                    (S:1353-1362)
 *
 * and no disk instability (S:1365-1367). Every value carried between the steps is a local of the
 * call; no transport property is written. The arithmetic is the package's shared code: the H I
 * block (shared/sage_hi_physics.h), the history (shared/sage_sfr_history.h), the commits
 * (shared/sage_sf_feedback_physics.h), the FFB budget and the yield
 * (shared/sage_metal_yield_physics.h). The package's StarFormationRate diagnostic (not a SAGE26
 * output) gains the stars formed over the snapshot interval, as
 * sage26_apply_star_formation_supernova does for the ordinary path.
 *
 * Mvir and Vvir are SAGE26 floats; the core holds them as doubles and they are read here as the
 * floats SAGE26 holds. SAGE26's star-forming radius factor SF_DISK_RADIUS_FRAC (3.0, S:60) enters
 * as the package's StarFormingDiskFactor, as in sage26_calculate_star_formation. SAGE26's
 * SaveFullSFH block (S:1332-1337) is off on every rung and has no package property; it is not
 * ported.
 *
 * Reference: Li et al. (2024), as SAGE26 cites it (S:1266); Croton et al. (2016)
 */

#include <math.h>
#include <stddef.h>

#include "constants.h"
#include "error.h"
#include "globals.h"
#include "module_interface.h"
#include "module_registry.h"
#include "types.h"
#include "module_system/parameter_helpers.h"
#include "module_system/physical_constants.h"
#include "shared/metallicity.h"
#include "shared/sage_cgm_routing.h"
#include "shared/sage_ffb_feedback_physics.h"
#include "shared/sage_ffb_regime.h"
#include "shared/sage_fire_feedback_physics.h"
#include "shared/sage_h2_physics.h"
#include "shared/sage_hi_physics.h"
#include "shared/sage_metal_yield_physics.h"
#include "shared/sage_sf_feedback_physics.h"
#include "shared/sage_sfr_history.h"
#include "shared/time_parity.h"

#include "sage26_ffb_star_formation.h"

// ============================================================================
// MODULE PARAMETERS
// ============================================================================

#define MODULE_NAME "sage26_ffb_star_formation"

/* SAGE26's EnhancedStarFormationOn (0 off, 1 FFB star formation for FFBRegime 1 rows), required
 * and checked at init (campaign decision D33) */
static int ENHANCED_STAR_FORMATION_ON = MIMIC_SAGE_FFB_OFF;

/* SAGE26's EnhancedSFEfficiency, epsilon_FFB (S:1308), required, finite and within [0, 1] */
static double ENHANCED_SF_EFFICIENCY;

/* The existing keys the transaction reads, each with its existing validation */
static double STAR_FORMING_DISK_FACTOR;
static int SF_PRESCRIPTION;
static double RECYCLE_FRACTION;
static int CGM_RECIPE_ON;
static int FIRE_MODE_ON;
static double FEEDBACK_REHEATING_EPSILON;
static double FEEDBACK_EJECTION_EFFICIENCY;
static double YIELD;
static double FRAC_Z_LEAVE_DISK;

/* SAGE26's EnergySNcode and EtaSNcode (R/src/core_init.c:102-103) */
static double energy_sn_code;
static double eta_sn_code;

/* The test observer (sage26_ffb_star_formation.h); inactive in a run */
static Sage26FfbObserver test_observer = NULL;
static void *test_observer_user = NULL;

// ============================================================================
// HELPER FUNCTIONS
// ============================================================================

void sage26_ffb_star_formation_set_test_observer(Sage26FfbObserver observer, void *user) {
  test_observer = observer;
  test_observer_user = user;
}

/**
 * @brief Refuse a placement other than once, by galaxy, in a substep phase
 *
 * SAGE26 calls starformation_ffb from starformation_and_feedback, once per galaxy per substep
 * (S:1004-1008). An entry in pre_timestep or post_timestep, a second entry, or an entry with
 * another processing mode is refused, naming the module. The check names no substep phase (phase
 * names are run configuration).
 *
 * @return 0 when placed correctly; -1 otherwise
 */
static int check_placement(void) {
  if (module_count_phase_entries(MODULE_NAME, MimicConfig.pre_timestep,
                                 MimicConfig.num_pre_timestep, PROCESSING_MODE_BY_GALAXY,
                                 NULL) > 0) {
    ERROR_LOG("%s is configured in modules.pre_timestep; it must run once per substep in a "
              "substep phase as process_by_galaxy: SAGE26 forms an FFB galaxy's stars in each "
              "substep (model_starformation_and_feedback.c:1004-1008)",
              MODULE_NAME);
    return -1;
  }
  if (module_count_phase_entries(MODULE_NAME, MimicConfig.post_timestep,
                                 MimicConfig.num_post_timestep, PROCESSING_MODE_BY_GALAXY,
                                 NULL) > 0) {
    ERROR_LOG("%s is configured in modules.post_timestep; it must run once per substep in a "
              "substep phase as process_by_galaxy: SAGE26 forms an FFB galaxy's stars in each "
              "substep (model_starformation_and_feedback.c:1004-1008)",
              MODULE_NAME);
    return -1;
  }
  int entries = 0;
  int by_galaxy_entries = 0;
  for (int p = 0; p < MimicConfig.num_substep_phases; p++) {
    const struct ModulePhaseConfig *phase = &MimicConfig.substep_phases[p];
    int matching = 0;
    entries += module_count_phase_entries(MODULE_NAME, phase->modules, phase->num_modules,
                                          PROCESSING_MODE_BY_GALAXY, &matching);
    by_galaxy_entries += matching;
  }
  if (entries != 1 || by_galaxy_entries != 1) {
    ERROR_LOG("%s must be configured exactly once in a substep phase as process_by_galaxy "
              "(found %d substep entries, %d as process_by_galaxy)",
              MODULE_NAME, entries, by_galaxy_entries);
    return -1;
  }
  return 0;
}

/**
 * @brief SAGE26's star formation of starformation_ffb (S:1297-1320) from the cold gas
 *
 * @param cold_gas  The float ColdGas after the H I block (unchanged by it)
 * @param vvir      The row's own float Vvir
 * @param reff      reff (S:1273)
 * @param tdyn      tdyn (S:1274)
 * @param dt        The substep interval
 * @param t         Receives the branch flags, strdot and the uncapped stars
 * @return The stars formed (double)
 */
static double ffb_stars(const float cold_gas, const float vvir, const double reff,
                        const double tdyn, const double dt, struct Sage26FfbStarFormationTrace *t) {
  /* SAGE26 forms stars from the cold gas, not H2 (S:1297) */
  const double gas_for_sf = cold_gas;
  double stars;

  /* SAGE parity: stars are 0 if the cold gas, the float Vvir, reff or tdyn is NaN or infinite
   * (S:1303-1306); then only a positive tdyn and gas form stars (S:1307, S:1318-1320) */
  t->nonfinite_guard = isnan(cold_gas) || isinf(cold_gas) || isnan(vvir) || isinf(vvir) ||
                       isnan(reff) || isinf(reff) || isnan(tdyn) || isinf(tdyn);
  if (t->nonfinite_guard) {
    stars = 0.0;
  } else if (tdyn > 0.0 && gas_for_sf > 0.0) {
    t->tdyn_gas_positive = 1;
    /* Li et al. (2024) eq. 4 as SAGE26 writes it: epsilon_FFB * M_gas / t_dyn (S:1308-1309) */
    const double strdot = ENHANCED_SF_EFFICIENCY * gas_for_sf / tdyn;
    t->strdot = strdot;

    t->strdot_rejected = isnan(strdot) || isinf(strdot) || strdot < 0.0;
    if (t->strdot_rejected) {
      stars = 0.0;
    } else {
      stars = strdot * dt;
      t->stars_uncapped = stars;
      /* SAGE parity: the stars are capped at the float cold gas, then rejected if NaN, infinite or
       * negative (S:1314-1316) */
      t->stars_cap_binds = stars > cold_gas;
      if (stars > cold_gas) {
        stars = cold_gas;
      }
      t->stars_rejected = isnan(stars) || isinf(stars) || stars < 0.0;
      if (t->stars_rejected) {
        stars = 0.0;
      }
    }
  } else {
    t->tdyn_gas_positive = 0;
    stars = 0.0;
  }
  return stars;
}

/** @brief A trace with every value marked as not formed */
static void trace_init(struct Sage26FfbStarFormationTrace *t) {
  t->path = SAGE26_FFB_PATH_INITIAL_BOUNDARY;
  t->dt = NAN;
  t->bin = -1;
  t->redshift = NAN;
  t->reff = NAN;
  t->tdyn = NAN;
  t->nonfinite_guard = -1;
  t->tdyn_gas_positive = -1;
  t->strdot = NAN;
  t->strdot_rejected = -1;
  t->stars_uncapped = NAN;
  t->stars_cap_binds = -1;
  t->stars_rejected = -1;
  t->stars_formed = NAN;
  t->metallicity = NAN;
  t->sf_caps = (struct MimicSageHydrogenCapLocals){0, NAN, -1, -1};
  mimic_sage_fire_locals_init(&t->feedback.fire);
  t->reheated_from_budget = NAN;
  t->reheat_clamp_binds = -1;
  t->reheated_to_commit = NAN;
  t->ejected_to_commit = NAN;
  t->yield_stars = NAN;
  t->yield = (struct MimicSageDiskYieldLocals){-1, NAN, NAN, -1};
}

static void observe(const struct Sage26FfbStarFormationTrace *t) {
  if (test_observer != NULL) {
    test_observer(test_observer_user, t);
  }
}

// ============================================================================
// MODULE LIFECYCLE FUNCTIONS
// ============================================================================

int sage26_ffb_star_formation_init(void) {
  /* Required, no default (campaign decision D33): a missing key fails here */
  LOAD_PARAM_INT("EnhancedStarFormationOn", ENHANCED_STAR_FORMATION_ON);
  if (mimic_sage_validate_enhanced_star_formation_on(MODULE_NAME, ENHANCED_STAR_FORMATION_ON) !=
      0) {
    return -1;
  }
  LOAD_PARAM_DOUBLE("EnhancedSFEfficiency", ENHANCED_SF_EFFICIENCY);
  /* Finite and within [0, 1] (R6.par:67, "1.0 is theoretical maximum"); a NaN, which the range
   * comparisons alone would let through, is refused too */
  if (!isfinite(ENHANCED_SF_EFFICIENCY) || ENHANCED_SF_EFFICIENCY < 0.0 ||
      ENHANCED_SF_EFFICIENCY > 1.0) {
    ERROR_LOG("%s: EnhancedSFEfficiency = %g is not supported: it must be finite and within "
              "[0, 1] (the FFB star-formation efficiency, R6.par:67)",
              MODULE_NAME, ENHANCED_SF_EFFICIENCY);
    return -1;
  }
  LOAD_AND_VALIDATE_RANGE_INCLUSIVE("StarFormingDiskFactor", STAR_FORMING_DISK_FACTOR, 0.0, 10.0,
                                    "star forming disk factor");
  LOAD_PARAM_INT("SFprescription", SF_PRESCRIPTION);
  if (mimic_sage_validate_sf_prescription(MODULE_NAME, SF_PRESCRIPTION) != 0) {
    return -1;
  }
  LOAD_AND_VALIDATE_RANGE_INCLUSIVE("RecycleFraction", RECYCLE_FRACTION, 0.0, 1.0,
                                    "recycle fraction");
  LOAD_PARAM_INT("CGMrecipeOn", CGM_RECIPE_ON);
  if (mimic_sage_validate_cgm_recipe_on(MODULE_NAME, CGM_RECIPE_ON) != 0) {
    return -1;
  }
  LOAD_PARAM_INT("FIREmodeOn", FIRE_MODE_ON);
  if (mimic_sage_validate_fire_mode(MODULE_NAME, FIRE_MODE_ON) != 0) {
    return -1;
  }
  /* FFB's fixed-epsilon branch (FIREmodeOn 0, S:935-937, S:960-965) is on no rung and not ported
   * (campaign decision D6) */
  if (ENHANCED_STAR_FORMATION_ON != MIMIC_SAGE_FFB_OFF && FIRE_MODE_ON != MIMIC_SAGE_FIRE_MODE_ON) {
    ERROR_LOG("%s: FIREmodeOn = %d is not supported with EnhancedStarFormationOn = %d: the FFB "
              "feedback ports only FIREmodeOn 1 (model_starformation_and_feedback.c:914-934); "
              "its fixed-epsilon branch is on no rung",
              MODULE_NAME, FIRE_MODE_ON, ENHANCED_STAR_FORMATION_ON);
    return -1;
  }
  LOAD_AND_VALIDATE_RANGE_INCLUSIVE("FeedbackReheatingEpsilon", FEEDBACK_REHEATING_EPSILON, 0.0,
                                    100.0, "reheating efficiency");
  LOAD_AND_VALIDATE_RANGE_INCLUSIVE("FeedbackEjectionEfficiency", FEEDBACK_EJECTION_EFFICIENCY, 0.0,
                                    100.0, "ejection efficiency");
  LOAD_AND_VALIDATE_RANGE_INCLUSIVE("Yield", YIELD, 0.0, 1.0, "metal yield");
  LOAD_AND_VALIDATE_RANGE_INCLUSIVE("FracZleaveDisk", FRAC_Z_LEAVE_DISK, 0.0, 1.0,
                                    "metal outflow fraction");
  if (check_placement() != 0) {
    return -1;
  }

  // SAGE26's EnergySNcode and EtaSNcode (R/src/core_init.c:102-103)
  energy_sn_code = mimic_sage_energy_sn_code(MimicConfig.UnitEnergy_in_cgs, MimicConfig.Hubble_h);
  eta_sn_code = mimic_sage_eta_sn_code(MimicConfig.UnitMass_in_g, MimicConfig.Hubble_h);

  VERBOSE_LOG("SAGE FFB star formation module initialized");
  VERBOSE_LOG("  EnhancedStarFormationOn = %d%s", ENHANCED_STAR_FORMATION_ON,
              ENHANCED_STAR_FORMATION_ON == MIMIC_SAGE_FFB_OFF
                  ? " (no FFB star formation: nothing is written)"
                  : " (FFB star formation for FFBRegime 1 rows)");
  VERBOSE_LOG("  EnhancedSFEfficiency = %.4f", ENHANCED_SF_EFFICIENCY);
  return 0;
}

int sage26_ffb_star_formation_process(struct ModuleContext *ctx, struct Halo *halos, int ngal) {
  if (ngal != 1) {
    ERROR_LOG("process_by_galaxy expects ngal=1, got %d", ngal);
    return -1;
  }

  struct Halo *halo = &halos[0];
  if (halo->galaxy == NULL) {
    return 0;
  }

  /* SAGE parity: SAGE26 dispatches to starformation_ffb only with EnhancedStarFormationOn >= 1
   * and FFBRegime 1 (S:1004); on any other row this module writes nothing */
  if (ENHANCED_STAR_FORMATION_ON == MIMIC_SAGE_FFB_OFF) {
    return 0;
  }
  struct GalaxyData *gal = halo->galaxy;
  if (gal->FFBRegime != 1) {
    return 0;
  }

  struct Sage26FfbStarFormationTrace t;
  trace_init(&t);

  double dt = 0.0;
  const enum MimicObjectTimeStatus dt_status = mimic_object_substep_dt(halo, ctx, &dt);
  if (dt_status == MIMIC_OBJECT_TIME_SKIP_INITIAL) {
    // SAGE parity: SAGE26 is undefined in this state (Age[-1] at R/src/core_build_model.c:479);
    // the package writes nothing, as its SF, SN and burst modules do (campaign decision D37)
    observe(&t);
    return 0;
  }
  if (dt_status != MIMIC_OBJECT_TIME_OK) {
    ERROR_LOG("Invalid FFB star formation dt for halo %lld (SnapNum=%d, dT=%.3e, num_substeps=%d, "
              "status=%s)",
              halo->HaloNr, halo->SnapNum, halo->dT, (ctx != NULL) ? ctx->num_substeps : -1,
              mimic_object_time_status_str(dt_status));
    return -1;
  }
  t.dt = dt;

  /* Checked before anything is written: the substep's history bin, and the redshift of the
   * row's stored snapshot that the FFB feedback reads (S:915), refused outside the run's redshift
   * table (campaign decision D35) */
  t.bin = mimic_sage_sfr_step_bin(ctx->substep_number, ctx->num_substeps);
  if (t.bin < 0) {
    ERROR_LOG("Invalid FFB disk SF history bin for halo %lld (substep %d of %d)", halo->HaloNr,
              ctx->substep_number, ctx->num_substeps);
    return -1;
  }
  if (mimic_sage_fire_stored_redshift(ctx->params, halo->SnapNum, &t.redshift) != 0) {
    ERROR_LOG("%s: no FFB FIRE scaling for galaxy HaloNr %lld", MODULE_NAME, halo->HaloNr);
    return -1;
  }

  // ========================================================================
  // 2. The dynamical time of the star-forming disk (S:1273-1274)
  // ========================================================================

  /* SAGE parity: the float Vvir and DiskScaleRadius are SAGE26's; reff and tdyn are doubles
   * (S:1270-1274) */
  const float vvir = (float)halo->Vvir;
  const double reff = STAR_FORMING_DISK_FACTOR * gal->DiskScaleRadius;
  const double tdyn = (reff > 0.0 && vvir > 0.0) ? reff / vvir : 0.0;
  t.reff = reff;
  t.tdyn = tdyn;

  // ========================================================================
  // 3. H2gas zeroed, then the H I block (S:1280-1292)
  // ========================================================================

  /* SAGE parity: FFB bypasses the BR06 law and leaves H2gas 0 (S:1280); the H I block then forms
   * H1gas from the whole cold hydrogen with the ionisation cut (S:1283-1292). The prescription-1
   * hydrogen caps of the commits below still apply (S:1131-1135, S:1169-1173). The depletion time
   * H2DepletionTime_Gyr is not written. */
  gal->H2gas = 0.0f;
  mimic_sage_calculate_h1gas(gal, ctx->params->Hubble_h);

  // ========================================================================
  // 4. Star formation from the cold gas (S:1297-1320)
  // ========================================================================

  const double stars = ffb_stars(gal->ColdGas, vvir, reff, tdyn, dt, &t);
  t.stars_formed = stars;

  // ========================================================================
  // 5. The disk history, from the stars formed and the cold gas before any commit (S:1325-1327)
  // ========================================================================

  if (mimic_sage_sample_disk_sfr(gal, t.bin, stars, dt) != 0) {
    ERROR_LOG("Invalid FFB disk SF history bin for halo %lld (substep %d of %d)", halo->HaloNr,
              ctx->substep_number, ctx->num_substeps);
    return -1;
  }

  /* With no FoF central (a Mimic-only state: SAGE26 always has one), the transaction has written
   * H2gas, H1gas and the history sample and commits nothing, the convention of
   * sage26_apply_star_formation_supernova (which samples, then returns before its commits) */
  if (ctx->central_galaxy == NULL || ctx->central_galaxy->galaxy == NULL) {
    DEBUG_LOG("No FOF central available for the FFB feedback and yield destination");
    t.path = SAGE26_FFB_PATH_NO_CENTRAL;
    observe(&t);
    return 0;
  }
  struct GalaxyData *central_gal = ctx->central_galaxy->galaxy;

  // ========================================================================
  // 6. The star-formation commit with the pre-commit metallicity (S:1329-1330)
  // ========================================================================

  /* SAGE parity: this metallicity is computed once, before the commit, and reused by the feedback
   * commit below (S:1329, S:1348); the ordinary path recomputes it after the commit (S:1076) */
  const double metallicity = mimic_get_metallicity(gal->ColdGas, gal->MetalsColdGas);
  t.metallicity = metallicity;
  mimic_sage_update_from_star_formation(gal, stars, metallicity, RECYCLE_FRACTION, SF_PRESCRIPTION,
                                        &t.sf_caps);

  // ========================================================================
  // 7. The package's StarFormationRate diagnostic (not a SAGE26 output)
  // ========================================================================

  // Accumulate the snapshot-averaged StarFormationRate from the stars formed, as
  // sage26_apply_star_formation_supernova does for the ordinary path
  if (halo->dT > 0.0) {
    gal->StarFormationRate += stars / halo->dT;
  }

  // ========================================================================
  // 8. The FFB feedback budget against the remaining cold gas (S:1340-1344)
  // ========================================================================

  /* SAGE parity: the budget runs after the star-formation commit and renormalises a local copy of
   * the stars against the cold gas that commit left (S:943-947); the committed stellar mass and
   * the history keep the stars formed, and only the yield uses the renormalised stars */
  double yield_stars = stars;
  double reheated_mass = 0.0;
  double ejected_mass = 0.0;
  mimic_sage_ffb_sn_feedback(vvir, t.redshift, gal->ColdGas, FEEDBACK_REHEATING_EPSILON,
                             FEEDBACK_EJECTION_EFFICIENCY, eta_sn_code, energy_sn_code,
                             &yield_stars, &reheated_mass, &ejected_mass, &gal->MassLoading,
                             &t.feedback);
  t.reheated_from_budget = reheated_mass;

  // ========================================================================
  // 9. The reheated mass clamped again to the remaining cold gas (S:1346)
  // ========================================================================

  /* SAGE parity: clamped here (S:1346) and again inside update_from_feedback (S:1153-1156) */
  t.reheat_clamp_binds = reheated_mass > gal->ColdGas;
  if (reheated_mass > gal->ColdGas) {
    reheated_mass = gal->ColdGas;
  }
  t.reheated_to_commit = reheated_mass;
  t.ejected_to_commit = ejected_mass;

  // ========================================================================
  // 10. The feedback commit to the FoF central's pair, with the step-6 metallicity (S:1348)
  // ========================================================================

  /* SAGE parity: the FoF central (SAGE26's centralgal) receives the feedback, and ITS Regime
   * selects the pair with CGMrecipeOn 1 (S:1175-1176) */
  mimic_sage_update_from_feedback(gal, central_gal, reheated_mass, ejected_mass, metallicity,
                                  SF_PRESCRIPTION, CGM_RECIPE_ON, &t.commit);

  // ========================================================================
  // 11. The yield from the renormalised stars (S:1353-1362)
  // ========================================================================

  /* SAGE parity: the Krumholz & Dekel (2011) scale reads the FoF central's float Mvir
   * (S:1354-1355) */
  t.yield_stars = yield_stars;
  mimic_sage_apply_disk_yield(gal, central_gal, (float)ctx->central_galaxy->Mvir, yield_stars,
                              YIELD, FRAC_Z_LEAVE_DISK, CGM_RECIPE_ON, &t.yield);

  // ========================================================================
  // 12. No disk instability (S:1365-1367) and no transport field
  // ========================================================================

  t.path = SAGE26_FFB_PATH_COMPLETE;
  observe(&t);

  DEBUG_LOG("Type=%d: FFB stars formed=%.3e, renormalised=%.3e, reheated=%.3e, ejected=%.3e",
            halo->Type, stars, yield_stars, reheated_mass, ejected_mass);
  return 0;
}

int sage26_ffb_star_formation_cleanup(void) {
  VERBOSE_LOG("SAGE FFB star formation module cleaned up");
  return 0;
}
