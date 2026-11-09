/**
 * @file    sage26_calculate_supernova_feedback.c
 * @brief   SAGE26 supernova feedback budgets (compute_sn_feedback, non-Karpov, FIREmodeOn 0 or 1)
 *
 * Computes the supernova reheated and ejected masses from the stars formed this substep,
 * renormalising stars and reheated mass together when they exceed the cold gas, and stores them
 * in NewStellarMass, SupernovaReheatedMass and SupernovaEjectedMass for
 * sage26_apply_star_formation_supernova. Implements SAGE26's compute_sn_feedback with
 * SupernovaRecipeOn 1 and KarpovModeOn 0, for the required parameter FIREmodeOn:
 *
 * - FIREmodeOn 0, the fixed-epsilon Croton et al. (2016) budgets: reheated mass =
 *   FeedbackReheatingEpsilon * stars; ejected mass = (FeedbackEjectionEfficiency *
 *   (EtaSNcode * EnergySNcode) / Vvir^2 - FeedbackReheatingEpsilon) * stars. MassLoading is not
 *   written.
 * - FIREmodeOn 1, the FIRE budgets (shared/sage_fire_feedback_physics.h): the Muratov et al.
 *   (2015) scaling from the galaxy's own Vvir and the redshift of its stored snapshot; the
 *   reheating loading, capped at 2 * EtaSNcode * EnergySNcode / Vvir^2 and recorded in
 *   MassLoading; the Hirschmann et al. (2016) energy-based ejection.
 *
 * Both use the forming galaxy's own Vvir; a non-positive Vvir ejects nothing, and the ejection
 * is floored at zero. A row whose FFBRegime is 1 is left to sage26_ffb_star_formation.
 *
 * Reference: Croton et al. (2006, 2016); Muratov et al. (2015); Hirschmann et al. (2016)
 *
 * Ported from SAGE26 (MIT; Copyright (c) 2019-2026 Semi-Analytic Galaxy Evolution (SAGE)); see
 * models/sage26/LICENSE-SAGE26.
 */

#include "constants.h"
#include "error.h"
#include "globals.h"
#include "module_interface.h"
#include "module_registry.h"
#include "types.h"
#include "module_system/parameter_helpers.h"
#include "module_system/physical_constants.h"
#include "shared/sage_fire_feedback_physics.h"
#include "shared/sage_sf_feedback_physics.h"
#include "shared/time_parity.h"

// ============================================================================
// MODULE PARAMETERS
// ============================================================================

static double FEEDBACK_REHEATING_EPSILON;
static double FEEDBACK_EJECTION_EFFICIENCY;

/* SAGE26's FIREmodeOn (0 fixed-epsilon, 1 FIRE), required and checked at init
 * (shared/sage_fire_feedback_physics.h) */
static int FIRE_MODE_ON;

// ============================================================================
// MODULE-LOCAL CONSTANTS (converted from physical constants)
// ============================================================================

static double energy_sn_code;
static double eta_sn_code;

// ============================================================================
// MODULE LIFECYCLE FUNCTIONS
// ============================================================================

int sage26_calculate_supernova_feedback_init(void) {
  LOAD_PARAM_DOUBLE("FeedbackReheatingEpsilon", FEEDBACK_REHEATING_EPSILON);
  if (mimic_sage_validate_feedback_reheating_epsilon("sage26_calculate_supernova_feedback",
                                                     FEEDBACK_REHEATING_EPSILON) != 0) {
    return -1;
  }
  LOAD_AND_VALIDATE_RANGE_INCLUSIVE("FeedbackEjectionEfficiency", FEEDBACK_EJECTION_EFFICIENCY, 0.0,
                                    100.0, "ejection efficiency");
  /* Required, no default: a missing key fails here */
  LOAD_PARAM_INT("FIREmodeOn", FIRE_MODE_ON);
  if (mimic_sage_validate_fire_mode("sage26_calculate_supernova_feedback", FIRE_MODE_ON) != 0) {
    return -1;
  }

  /* Dependency checks */

  /* ERROR: apply step is required to commit SN transport fields to galaxy reservoirs */
  if (!module_configured_anywhere("sage26_apply_star_formation_supernova")) {
    ERROR_LOG("sage26_calculate_supernova_feedback requires sage26_apply_star_formation_supernova "
              "in the pipeline — without it, SupernovaReheatedMass and "
              "SupernovaEjectedMass are computed each substep but never "
              "committed to galaxy reservoirs (silent output loss)");
    return -1;
  }

  /* ERROR: if both SF and SN are configured, SF must precede SN in the same phase */
  if (module_configured_anywhere("sage26_calculate_star_formation") &&
      !module_precedes_in_substep_phase(
          "sage26_calculate_star_formation", PROCESSING_MODE_BY_GALAXY,
          "sage26_calculate_supernova_feedback", PROCESSING_MODE_BY_GALAXY)) {
    ERROR_LOG("sage26_calculate_supernova_feedback requires sage26_calculate_star_formation to "
              "precede it in the same substep phase — SN reads NewStellarMass written by "
              "SF; wrong order applies stale values from previous substep");
    return -1;
  }

  // SAGE26's EnergySNcode and EtaSNcode
  energy_sn_code = mimic_sage_energy_sn_code(MimicConfig.UnitEnergy_in_cgs, MimicConfig.Hubble_h);
  eta_sn_code = mimic_sage_eta_sn_code(MimicConfig.UnitMass_in_g, MimicConfig.Hubble_h);

  VERBOSE_LOG("SAGE supernova feedback module initialized");
  VERBOSE_LOG("  FeedbackReheatingEpsilon = %.3f", FEEDBACK_REHEATING_EPSILON);
  VERBOSE_LOG("  FeedbackEjectionEfficiency = %.3f", FEEDBACK_EJECTION_EFFICIENCY);
  VERBOSE_LOG("  FIREmodeOn = %d", FIRE_MODE_ON);
  VERBOSE_LOG("  energy_sn_code = %.6e (from ENERGY_SN physical constant)", energy_sn_code);
  VERBOSE_LOG("  eta_sn_code = %.6e (from ETA_SN physical constant)", eta_sn_code);

  return 0;
}

int sage26_calculate_supernova_feedback_process(struct ModuleContext *ctx, struct Halo *halos,
                                                int ngal) {
  if (ngal != 1) {
    ERROR_LOG("process_by_galaxy expects ngal=1, got %d", ngal);
    return -1;
  }

  struct Halo *halo = &halos[0];

  if (halo->galaxy == NULL) {
    return 0;
  }

  struct GalaxyData *gal = halo->galaxy;

  /* SAGE parity: SAGE26 hands an FFBRegime 1 row to starformation_ffb before
   * compute_sn_feedback, so this module writes nothing on that row (no MassLoading, no transport
   * field); sage26_ffb_star_formation runs its own FFB feedback. FFBRegime is 1 only with
   * EnhancedStarFormationOn 1. */
  if (gal->FFBRegime == 1) {
    return 0;
  }

  /* SAGE parity: the budgets read the forming galaxy's own Vvir, a float in SAGE26, also for
   * satellites and orphans; the reheated gas still goes to the FoF central's hot gas, in the
   * apply step */
  const float vvir = (float)halo->Vvir;
  double stars = gal->NewStellarMass;

  /* The FIRE scaling from the redshift of the galaxy's stored snapshot, read before the
   * velocity test as SAGE26 orders it; FIREmodeOn 0 leaves it 0 and unused */
  double fire_scaling = 0.0;
  if (FIRE_MODE_ON == MIMIC_SAGE_FIRE_MODE_ON) {
    /* The initial-boundary state writes nothing, MassLoading included */
    double substep_dt = 0.0;
    if (ctx != NULL &&
        mimic_object_substep_dt(halo, ctx, &substep_dt) == MIMIC_OBJECT_TIME_SKIP_INITIAL) {
      // SAGE parity: SAGE26 is undefined in this state (it would index the snapshot -1), so
      // the package follows its SKIP_INITIAL convention, as the SF and burst modules do.
      return 0;
    }
    double z_fire = 0.0;
    if (ctx == NULL || mimic_sage_fire_stored_redshift(ctx->params, halo->SnapNum, &z_fire) != 0) {
      ERROR_LOG("sage26_calculate_supernova_feedback: no FIRE scaling for galaxy HaloNr %lld",
                halo->HaloNr);
      return -1;
    }
    fire_scaling = mimic_sage_fire_scaling(vvir, z_fire, NULL);
  }

  double reheated_mass;
  if (FIRE_MODE_ON == MIMIC_SAGE_FIRE_MODE_ON) {
    /* SAGE parity: the capped loading is recorded as the float MassLoading before the
     * renormalisation below, also when no stars form */
    const double eta_reheat = mimic_sage_capped_eta_reheat(
        FEEDBACK_REHEATING_EPSILON * fire_scaling, vvir, eta_sn_code, energy_sn_code, NULL);
    gal->MassLoading = (float)eta_reheat;
    reheated_mass = eta_reheat * stars;
  } else {
    reheated_mass = FEEDBACK_REHEATING_EPSILON * stars;
  }

  // Can't use more cold gas than is available, so balance SF and feedback
  if ((stars + reheated_mass) > gal->ColdGas && (stars + reheated_mass) > 0.0) {
    const double fac = gal->ColdGas / (stars + reheated_mass);
    stars *= fac;
    reheated_mass *= fac;
  }

  /* A non-positive own Vvir ejects nothing */
  double ejected_mass = 0.0;
  if (vvir > 0.0) {
    if (FIRE_MODE_ON == MIMIC_SAGE_FIRE_MODE_ON) {
      /* FIRE: the velocity is the float Vvir widened to double, squared in double */
      const double coupling =
          mimic_sage_sn_energy_coupling(fire_scaling, FEEDBACK_EJECTION_EFFICIENCY, NULL);
      ejected_mass = mimic_sage_fire_energy_ejection(coupling, stars, reheated_mass, vvir,
                                                     eta_sn_code, energy_sn_code, NULL);
    } else {
      /* SAGE parity: with FIREmodeOn 0 only, SAGE26 squares the float Vvir in float */
      const float vvir_sq = vvir * vvir;
      ejected_mass = (FEEDBACK_EJECTION_EFFICIENCY * (eta_sn_code * energy_sn_code) / vvir_sq -
                      FEEDBACK_REHEATING_EPSILON) *
                     stars;
    }
  }

  if (ejected_mass < 0.0) {
    ejected_mass = 0.0;
  }

  gal->NewStellarMass = stars;
  gal->SupernovaReheatedMass = reheated_mass;
  gal->SupernovaEjectedMass = ejected_mass;

  DEBUG_LOG("Type=%d: Stars=%.3e, Reheat=%.3e, Eject=%.3e (Vvir=%.2f, FIRE scaling=%.3e)",
            halo->Type, stars, reheated_mass, ejected_mass, (double)vvir, fire_scaling);

  return 0;
}

int sage26_calculate_supernova_feedback_cleanup(void) {
  VERBOSE_LOG("SAGE supernova feedback module cleaned up");
  return 0;
}
