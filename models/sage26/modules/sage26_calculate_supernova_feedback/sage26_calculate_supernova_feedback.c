/**
 * @file    sage26_calculate_supernova_feedback.c
 * @brief   SAGE26 supernova feedback budgets (non-FIRE, non-Karpov compute_sn_feedback)
 *
 * Ports compute_sn_feedback() on R0 (FIREmodeOn 0, KarpovModeOn 0,
 * SupernovaRecipeOn 1; R/src/model_starformation_and_feedback.c:732-889, R = the
 * staged SAGE26 source):
 *
 * - reheated mass = FeedbackReheatingEpsilon * stars (:767);
 * - when stars + reheated mass exceed the cold gas, both are scaled together by
 *   ColdGas / (stars + reheated mass) (:775-779), before any recycling;
 * - ejected mass = (FeedbackEjectionEfficiency * (EtaSNcode * EnergySNcode) /
 *   (Vvir * Vvir) - FeedbackReheatingEpsilon) * stars, with the forming galaxy's
 *   own Vvir (:783, :795-798), zero for a non-positive own Vvir (:876-878) and floored at
 *   zero (:880-882).
 *
 * Stores the renormalised stars in NewStellarMass and the budgets in
 * SupernovaReheatedMass and SupernovaEjectedMass for
 * sage26_apply_star_formation_supernova, which commits them.
 *
 * Reference: Croton et al. (2006, 2016)
 */

#include "constants.h"
#include "error.h"
#include "globals.h"
#include "module_interface.h"
#include "module_registry.h"
#include "types.h"
#include "module_system/parameter_helpers.h"
#include "module_system/physical_constants.h"
#include "shared/sage_sf_feedback_physics.h"

// ============================================================================
// MODULE PARAMETERS
// ============================================================================

static double FEEDBACK_REHEATING_EPSILON;
static double FEEDBACK_EJECTION_EFFICIENCY;

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

  (void)ctx;
  struct GalaxyData *gal = halo->galaxy;

  double stars = gal->NewStellarMass;
  double reheated_mass = FEEDBACK_REHEATING_EPSILON * stars;

  // Can't use more cold gas than is available, so balance SF and feedback (:775-779)
  if ((stars + reheated_mass) > gal->ColdGas && (stars + reheated_mass) > 0.0) {
    const double fac = gal->ColdGas / (stars + reheated_mass);
    stars *= fac;
    reheated_mass *= fac;
  }

  /* SAGE parity: SAGE26 ejects against the forming galaxy's own float Vvir, squared
   * in float, also for satellites and orphans (:783, :795-798); a non-positive own Vvir
   * ejects nothing (:876-878) */
  const float vvir = (float)halo->Vvir;
  double ejected_mass = 0.0;
  if (vvir > 0.0) {
    const float vvir_sq = vvir * vvir;
    ejected_mass = (FEEDBACK_EJECTION_EFFICIENCY * (eta_sn_code * energy_sn_code) / vvir_sq -
                    FEEDBACK_REHEATING_EPSILON) *
                   stars;
  }

  if (ejected_mass < 0.0) {
    ejected_mass = 0.0;
  }

  gal->NewStellarMass = stars;
  gal->SupernovaReheatedMass = reheated_mass;
  gal->SupernovaEjectedMass = ejected_mass;

  DEBUG_LOG("Type=%d: Stars=%.3e, Reheat=%.3e, Eject=%.3e (Vvir=%.2f)", halo->Type, stars,
            reheated_mass, ejected_mass, (double)vvir);

  return 0;
}

int sage26_calculate_supernova_feedback_cleanup(void) {
  VERBOSE_LOG("SAGE supernova feedback module cleaned up");
  return 0;
}
