/**
 * @file    sage26_calculate_supernova_feedback.c
 * @brief   SAGE26 supernova feedback budgets (compute_sn_feedback, non-Karpov, FIREmodeOn 0 or 1)
 *
 * Ports compute_sn_feedback() with SupernovaRecipeOn 1 and KarpovModeOn 0
 * (R/src/model_starformation_and_feedback.c:732-889, R = the staged SAGE26 source), for the
 * required parameter FIREmodeOn (campaign decision D33):
 *
 * FIREmodeOn 0 (R0-R2), the fixed-epsilon Croton et al. (2016) budgets:
 * - reheated mass = FeedbackReheatingEpsilon * stars (:767);
 * - when stars + reheated mass exceed the cold gas, both are scaled together by
 *   ColdGas / (stars + reheated mass) (:775-779), before any recycling;
 * - ejected mass = (FeedbackEjectionEfficiency * (EtaSNcode * EnergySNcode) /
 *   (Vvir * Vvir) - FeedbackReheatingEpsilon) * stars, with the forming galaxy's
 *   own Vvir (:783, :795-798), zero for a non-positive own Vvir (:876-878) and floored at
 *   zero (:880-882). MassLoading is not written.
 *
 * FIREmodeOn 1 (R3), the FIRE budgets (shared/sage_fire_feedback_physics.h):
 * - the Muratov et al. (2015) scaling from the forming galaxy's own float Vvir and the redshift
 *   of its stored snapshot, ZZ[SnapNum] (:743-753); an out-of-range SnapNum is refused (D35);
 * - the reheating loading FeedbackReheatingEpsilon * scaling capped at
 *   2 * EtaSNcode * EnergySNcode / Vvir^2 (capped_eta_reheat, R/src/model_misc.h:141-149),
 *   written to MassLoading as a float before the renormalisation, also when no stars form
 *   (:762-764), and the reheated mass eta * stars (:765);
 * - the same renormalisation (:775-779);
 * - the Hirschmann et al. (2016) energy-based ejection from the renormalised stars and reheated
 *   mass (:786-792, sn_energy_coupling, R/src/model_misc.h:118-124), zero for a non-positive own
 *   Vvir (:876-878), then the same floor at zero (:880-882).
 *
 * Stores the renormalised stars in NewStellarMass and the budgets in
 * SupernovaReheatedMass and SupernovaEjectedMass for
 * sage26_apply_star_formation_supernova, which commits them: the reheated gas leaves the forming
 * galaxy's cold gas for its FoF central's hot gas, and the ejection is capped against that
 * central's hot gas (R/src/model_starformation_and_feedback.c:1146-1251).
 *
 * Reference: Croton et al. (2006, 2016); Muratov et al. (2015); Hirschmann et al. (2016)
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
  LOAD_AND_VALIDATE_RANGE_INCLUSIVE("FeedbackReheatingEpsilon", FEEDBACK_REHEATING_EPSILON, 0.0,
                                    100.0, "reheating efficiency");
  LOAD_AND_VALIDATE_RANGE_INCLUSIVE("FeedbackEjectionEfficiency", FEEDBACK_EJECTION_EFFICIENCY, 0.0,
                                    100.0, "ejection efficiency");
  /* Required, no default (campaign decision D33): a missing key fails here */
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

  // SAGE26's EnergySNcode and EtaSNcode (R/src/core_init.c:102-103)
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

  /* SAGE parity: the budgets read the forming galaxy's own Vvir, a float in SAGE26, also for
   * satellites and orphans (R/src/model_starformation_and_feedback.c:746, :783); the reheated gas
   * still goes to the FoF central's hot gas, in the apply step */
  const float vvir = (float)halo->Vvir;
  double stars = gal->NewStellarMass;

  /* The FIRE scaling (:743-753) from the redshift of the galaxy's stored snapshot (:745), read
   * before the velocity test as SAGE26 orders it; FIREmodeOn 0 leaves it 0 and unused */
  double fire_scaling = 0.0;
  if (FIRE_MODE_ON == MIMIC_SAGE_FIRE_MODE_ON) {
    double z_fire = 0.0;
    if (ctx == NULL || mimic_sage_fire_stored_redshift(ctx->params, halo->SnapNum, &z_fire) != 0) {
      ERROR_LOG("sage26_calculate_supernova_feedback: no FIRE scaling for galaxy HaloNr %d",
                halo->HaloNr);
      return -1;
    }
    fire_scaling = mimic_sage_fire_scaling(vvir, z_fire, NULL);
  }

  double reheated_mass;
  if (FIRE_MODE_ON == MIMIC_SAGE_FIRE_MODE_ON) {
    /* SAGE parity: the capped loading is recorded as the float MassLoading before the
     * renormalisation below, also when no stars form (:762-764) */
    const double eta_reheat = mimic_sage_capped_eta_reheat(
        FEEDBACK_REHEATING_EPSILON * fire_scaling, vvir, eta_sn_code, energy_sn_code, NULL);
    gal->MassLoading = (float)eta_reheat;
    reheated_mass = eta_reheat * stars;
  } else {
    reheated_mass = FEEDBACK_REHEATING_EPSILON * stars;
  }

  // Can't use more cold gas than is available, so balance SF and feedback (:775-779)
  if ((stars + reheated_mass) > gal->ColdGas && (stars + reheated_mass) > 0.0) {
    const double fac = gal->ColdGas / (stars + reheated_mass);
    stars *= fac;
    reheated_mass *= fac;
  }

  /* A non-positive own Vvir ejects nothing (:876-878) */
  double ejected_mass = 0.0;
  if (vvir > 0.0) {
    if (FIRE_MODE_ON == MIMIC_SAGE_FIRE_MODE_ON) {
      /* FIRE: the velocity is the float Vvir widened to double, squared in double (:787-792) */
      const double coupling =
          mimic_sage_sn_energy_coupling(fire_scaling, FEEDBACK_EJECTION_EFFICIENCY, NULL);
      ejected_mass = mimic_sage_fire_energy_ejection(coupling, stars, reheated_mass, vvir,
                                                     eta_sn_code, energy_sn_code, NULL);
    } else {
      /* SAGE parity: with FIREmodeOn 0 only, SAGE26 squares the float Vvir in float
       * (:795-798) */
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
