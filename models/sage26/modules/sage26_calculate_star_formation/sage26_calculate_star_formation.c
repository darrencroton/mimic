/**
 * @file    sage26_calculate_star_formation.c
 * @brief   SAGE26 disk star formation (SFprescription 0 or 1) and the H I diagnostic
 *
 * Ports the start of SAGE26's starformation_and_feedback()
 * (R/src/model_starformation_and_feedback.c, R = the staged SAGE26 source), selected by
 * SFprescription as SAGE26's switch selects it (:1015-1017):
 *
 * - SFprescription 0 (R0, R1), sfr_croton06 (:160-184): stars form from the cold gas
 *   above the Kauffmann (1996) critical mass over the dynamical time of the
 *   star-forming disk, reff = 3.0 * DiskScaleRadius (SF_DISK_RADIUS_FRAC, :60). A
 *   galaxy whose own Vvir is not positive forms no stars and SAGE26 never forms tdyn
 *   (:170-171). H2gas is not written.
 * - SFprescription 1 (R2 onward), sfr_br06 (:193-252): the Blitz & Rosolowsky (2006)
 *   molecular gas of the disk, by radial integration (shared/sage_h2_physics.h), is
 *   written to H2gas, capped at the double ColdGas * 0.74, and stars form from it,
 *   strdot = SfrEfficiency * H2gas / tdyn, with no critical-mass threshold. A galaxy
 *   whose own Vvir is not positive, or whose disk scale radius in pc is not positive,
 *   gets H2gas 0 and forms no stars (:208-218).
 * - the H I block (:1035-1046), on every ordinary (non-FFB) call: the atomic remainder of the cold
 *   hydrogen, ColdGas * 0.74 - H2gas floored at zero, times the neutral fraction
 *   1 - f_ion of an exponential gas disk truncated at 0.5 Msun/pc^2
 *   (ionized_gas_fraction, :130-152), stored in the float H1gas; the package's one H I
 *   block, shared/sage_hi_physics.h, which sage26_ffb_star_formation also calls.
 * - stars = strdot * dt, floored at zero (:1048-1051), stored in NewStellarMass
 *   for sage26_calculate_supernova_feedback.
 *
 * A row whose FFBRegime is 1 (written by sage26_determine_ffb_regime only with
 * EnhancedStarFormationOn 1) is not processed: SAGE26 hands it to starformation_ffb before any of
 * the above (:1004-1008), so this module returns at once and writes nothing, and
 * sage26_ffb_star_formation, immediately before it in the run files, runs that row's transaction.
 *
 * Mvir, Vvir and the other halo values are SAGE26 floats; the core holds them as
 * doubles and they are read here as the floats SAGE26 holds.
 *
 * Reference: Croton et al. (2006, 2016)
 *
 * Ported from SAGE26 (MIT; Copyright (c) 2019-2026 Semi-Analytic Galaxy Evolution (SAGE)); see
 * models/sage26/LICENSE-SAGE26.
 */

#include <math.h>

#include "constants.h"
#include "error.h"
#include "globals.h"
#include "module_interface.h"
#include "module_registry.h"
#include "types.h"
#include "shared/sage_constants.h"
#include "shared/sage_h2_physics.h"
#include "shared/sage_hi_physics.h"
#include "shared/time_parity.h"
#include "module_system/parameter_helpers.h"

// ============================================================================
// MODULE PARAMETERS
// ============================================================================

static double SFR_EFFICIENCY;
static double STAR_FORMING_DISK_FACTOR;

/* SAGE26's SFprescription (0 Croton, 1 BR06) and the radial H2 integration's parameters,
 * required and checked at init (shared/sage_h2_physics.h) */
static int SF_PRESCRIPTION;
static int H2_RADIAL_INTEGRATION_ON;
static int H2_RADIAL_NBINS;
static double H2_RADIAL_RMAX_FACTOR;

// ============================================================================
// SAGE26 CONSTANTS (R/src/model_starformation_and_feedback.c)
// ============================================================================

/* Kauffmann (1996) eq. 7 critical cold-gas coefficient (:66) */
static const double KAUFFMANN96_SF_THRESHOLD = 0.19;

// ============================================================================
// HELPER FUNCTIONS
// ============================================================================

/**
 * @brief SAGE26's H I block of starformation_and_feedback (:1035-1046)
 *
 * SAGE parity: H I is computed on every ordinary (non-FFB) call (an FFB row returned before it,
 * :1004-1008, and its transaction runs the block itself), from the cold gas before this substep's
 * star formation and the H2gas at that point: under SFprescription 0 the inherited
 * H2gas, which that prescription never writes, under SFprescription 1 the BR06 H2gas
 * sfr_br06 has just written. Under SFprescription 0 H I is not resynchronised after the
 * depletion (the commits' caps, :1131-1135, :1169-1173, run only for prescriptions that
 * track H2), so it can exceed the hydrogen left after star formation and feedback: a
 * deliberate SAGE26 quirk. Under SFprescription 1 the commits and the black-hole accretion cap
 * it (shared/sage_sf_feedback_physics.h, shared/sage_agn_physics.h, :1131-1135, :1169-1173,
 * R/src/model_mergers.c:366-372), and every burst overwrites it with the un-ionised atomic
 * remainder (shared/sage_starburst_physics.h, R/src/model_mergers.c:798-803), which this block
 * replaces with the ionisation-corrected value at the next call.
 */
static void calculate_h1gas(struct GalaxyData *gal, const double hubble_h) {
  /* The package's one H I block (shared/sage_hi_physics.h): SAGE26's fused
   * ColdGas * HYDROGEN_MASS_FRAC - H2gas (:1036, model_starformation_and_feedback.o offset
   * 0x728) floored at zero, times 1 - ionized_gas_fraction (:130-152) */
  mimic_sage_calculate_h1gas(gal, hubble_h);
}

/**
 * @brief SAGE26's sfr_br06 (:193-252) with radial integration, the SFprescription 1 law
 *
 * reff and tdyn are doubles (:195), h and rs_pc floats (:214-215), as SAGE26 has them. The
 * radial integration (R/src/model_h2_chemistry.c:134-290) writes H2gas, except when the
 * cold gas is not positive, when H2gas keeps its value for the cap (:141-143); the cap
 * compares the float H2gas with the double ColdGas * 0.74 and stores that product
 * narrowed (:239-242). There is no critical-mass threshold, and the depletion time
 * H2DepletionTime_Gyr is not written (sfr_br06 passes no SFR-output pointer, :222).
 *
 * @return The instantaneous star-formation rate strdot (double)
 */
static double sfr_br06(struct GalaxyData *gal, const float vvir, const double hubble_h) {
  /* The star-forming region, StarFormingDiskFactor * r_s, where SAGE26's double
   * SF_DISK_RADIUS_FRAC (3.0, :60) enters (:206) */
  const double reff = STAR_FORMING_DISK_FACTOR * gal->DiskScaleRadius;
  double strdot = 0.0;

  /* SAGE parity: a non-positive own Vvir sets H2gas to 0 and forms no stars, before tdyn is
   * formed (:208-210) */
  if (vvir <= 0.0) {
    gal->H2gas = 0.0f;
    return strdot;
  }
  const double tdyn = reff / vvir;

  /* SAGE parity: the float h and the float rs_pc of CODE_LENGTH_TO_PC (:214-215,
   * R/src/macros.h:46); a non-positive one sets H2gas to 0 and forms no stars (:216-218) */
  const float h = (float)hubble_h;
  const float rs_pc = (float)(gal->DiskScaleRadius * 1.0e6 / h);
  if (rs_pc <= 0.0) {
    gal->H2gas = 0.0f;
    return strdot;
  }

  /* Radial integration, as sfr_br06 calls it with H2RadialIntegrationOn 1 (:221-223) */
  (void)mimic_sage_h2_radial_integration(gal, hubble_h, H2_RADIAL_NBINS, H2_RADIAL_RMAX_FACTOR,
                                         NULL);

  /* SAGE parity: H2gas is capped at the double ColdGas * HYDROGEN_MASS_FRAC, which also clears
   * an H2gas the radial integration left from before when the cold gas is gone (:239-242) */
  if (gal->H2gas > gal->ColdGas * SAGE_HYDROGEN_MASS_FRAC) {
    gal->H2gas = gal->ColdGas * SAGE_HYDROGEN_MASS_FRAC;
  }

  if (gal->H2gas > 0.0 && tdyn > 0.0) {
    strdot = SFR_EFFICIENCY * gal->H2gas / tdyn;
  }
  return strdot;
}

// ============================================================================
// MODULE LIFECYCLE FUNCTIONS
// ============================================================================

int sage26_calculate_star_formation_init(void) {
  LOAD_AND_VALIDATE_RANGE_INCLUSIVE("SfrEfficiency", SFR_EFFICIENCY, 0.0, 1.0,
                                    "star formation efficiency");
  LOAD_AND_VALIDATE_RANGE_INCLUSIVE("StarFormingDiskFactor", STAR_FORMING_DISK_FACTOR, 0.0, 10.0,
                                    "star forming disk factor");
  LOAD_PARAM_INT("SFprescription", SF_PRESCRIPTION);
  if (mimic_sage_validate_sf_prescription("sage26_calculate_star_formation", SF_PRESCRIPTION) !=
      0) {
    return -1;
  }
  LOAD_PARAM_INT("H2RadialIntegrationOn", H2_RADIAL_INTEGRATION_ON);
  LOAD_PARAM_INT("H2RadialNBins", H2_RADIAL_NBINS);
  LOAD_PARAM_DOUBLE("H2RadialRMaxFactor", H2_RADIAL_RMAX_FACTOR);
  if (mimic_sage_validate_h2_radial_parameters("sage26_calculate_star_formation",
                                               H2_RADIAL_INTEGRATION_ON, H2_RADIAL_NBINS,
                                               H2_RADIAL_RMAX_FACTOR) != 0) {
    return -1;
  }

  /* Dependency check: apply step is required to commit NewStellarMass to galaxy
   * reservoirs.  Without it, every substep's SF results are silently discarded. */
  if (!module_configured_anywhere("sage26_apply_star_formation_supernova")) {
    ERROR_LOG("sage26_calculate_star_formation requires sage26_apply_star_formation_supernova "
              "in the pipeline — without it, NewStellarMass is computed each "
              "substep but never committed to galaxy reservoirs (silent output loss)");
    return -1;
  }

  VERBOSE_LOG("SAGE star formation module initialized");
  VERBOSE_LOG("  SfrEfficiency = %.4f", SFR_EFFICIENCY);
  VERBOSE_LOG("  StarFormingDiskFactor = %.2f", STAR_FORMING_DISK_FACTOR);
  VERBOSE_LOG("  SFprescription = %d", SF_PRESCRIPTION);
  VERBOSE_LOG("  H2RadialIntegrationOn = %d, H2RadialNBins = %d, H2RadialRMaxFactor = %.2f",
              H2_RADIAL_INTEGRATION_ON, H2_RADIAL_NBINS, H2_RADIAL_RMAX_FACTOR);

  return 0;
}

int sage26_calculate_star_formation_process(struct ModuleContext *ctx, struct Halo *halos,
                                            int ngal) {
  double dt = 0.0;
  enum MimicObjectTimeStatus dt_status;

  if (ngal != 1) {
    ERROR_LOG("process_by_galaxy expects ngal=1, got %d", ngal);
    return -1;
  }

  struct Halo *halo = &halos[0];

  if (halo->galaxy == NULL) {
    return 0;
  }

  struct GalaxyData *gal = halo->galaxy;

  /* SAGE parity: SAGE26's starformation_and_feedback hands an FFBRegime 1 row to
   * starformation_ffb and returns before its ordinary chain
   * (R/src/model_starformation_and_feedback.c:1004-1008), so this module returns at once and
   * writes nothing, before its time guard: no star formation, no H I block, no NewStellarMass.
   * sage26_ffb_star_formation runs that row's whole transaction. FFBRegime is 1 only where
   * sage26_determine_ffb_regime wrote it, which it does only with EnhancedStarFormationOn 1, so
   * this is SAGE26's test EnhancedStarFormationOn >= 1 && FFBRegime == 1 (:1004). */
  if (gal->FFBRegime == 1) {
    return 0;
  }

  dt_status = mimic_object_substep_dt(halo, ctx, &dt);
  if (dt_status == MIMIC_OBJECT_TIME_SKIP_INITIAL) {
    return 0;
  }
  if (dt_status != MIMIC_OBJECT_TIME_OK) {
    ERROR_LOG(
        "Invalid star formation dt for halo %lld (SnapNum=%d, dT=%.3e, num_substeps=%d, status=%s)",
        halo->HaloNr, halo->SnapNum, halo->dT, (ctx != NULL) ? ctx->num_substeps : -1,
        mimic_object_time_status_str(dt_status));
    return -1;
  }

  const float vvir = (float)halo->Vvir;
  double strdot = 0.0;
  if (SF_PRESCRIPTION == MIMIC_SAGE_SF_PRESCRIPTION_BR06) {
    strdot = sfr_br06(gal, vvir, ctx->params->Hubble_h);
  } else {
    // sfr_croton06 (:160-184). The star-forming region is StarFormingDiskFactor * r_s,
    // entering exactly where SAGE26's double SF_DISK_RADIUS_FRAC (3.0, :60) does; the
    // run files hold 3.0.
    const double reff = STAR_FORMING_DISK_FACTOR * gal->DiskScaleRadius;
    /* SAGE parity: a non-positive own Vvir forms no stars, before tdyn is formed (:170-171);
     * the prescription-0 law leaves H2gas as it was */
    if (vvir > 0.0) {
      const double tdyn = reff / vvir;
      // From Kauffmann (1996) eq7 x piR^2, (Vvir in km/s, reff in Mpc/h) in units of 10^10Msun/h
      const double cold_crit = KAUFFMANN96_SF_THRESHOLD * vvir * reff;
      if (gal->ColdGas > cold_crit && tdyn > 0.0) {
        strdot = SFR_EFFICIENCY * (gal->ColdGas - cold_crit) / tdyn;
      }
    }
  }

  // H I from the cold gas before star formation (:1035-1046)
  calculate_h1gas(gal, ctx->params->Hubble_h);

  double stars = strdot * dt;
  if (stars < 0.0) {
    stars = 0.0;
  }

  gal->NewStellarMass = stars;

  DEBUG_LOG("Type=%d: Calculated SF=%.3e, SFR=%.3e, ColdGas=%.3e, DiskScaleRadius=%.3e", halo->Type,
            stars, stars / dt, gal->ColdGas, gal->DiskScaleRadius);

  return 0;
}

int sage26_calculate_star_formation_cleanup(void) {
  VERBOSE_LOG("SAGE star formation module cleaned up");
  return 0;
}
