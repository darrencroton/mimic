/**
 * @file    sage26_calculate_star_formation.c
 * @brief   SAGE26 disk star formation (SFprescription 0) and the H I diagnostic
 *
 * Ports the start of SAGE26's starformation_and_feedback() on R0
 * (R/src/model_starformation_and_feedback.c, R = the staged SAGE26 source):
 *
 * - sfr_croton06 (:160-184): stars form from the cold gas above the Kauffmann
 *   (1996) critical mass over the dynamical time of the star-forming disk,
 *   reff = 3.0 * DiskScaleRadius (SF_DISK_RADIUS_FRAC, :60). A galaxy whose own
 *   Vvir is not positive forms no stars and SAGE26 never forms tdyn (:170-171).
 * - the H I block (:1035-1046), unconditional on R0: the atomic remainder of the
 *   cold hydrogen, ColdGas * 0.74 - H2gas floored at zero, times the neutral
 *   fraction 1 - f_ion of an exponential gas disk truncated at 0.5 Msun/pc^2
 *   (ionized_gas_fraction, :130-152), stored in the float H1gas.
 * - stars = strdot * dt, floored at zero (:1048-1051), stored in NewStellarMass
 *   for sage26_calculate_supernova_feedback.
 *
 * Mvir, Vvir and the other halo values are SAGE26 floats; the core holds them as
 * doubles and they are read here as the floats SAGE26 holds.
 *
 * Reference: Croton et al. (2006, 2016)
 */

#include <math.h>

#include "constants.h"
#include "error.h"
#include "globals.h"
#include "module_interface.h"
#include "module_registry.h"
#include "types.h"
#include "shared/time_parity.h"
#include "module_system/parameter_helpers.h"

// ============================================================================
// MODULE PARAMETERS
// ============================================================================

static double SFR_EFFICIENCY;
static double STAR_FORMING_DISK_FACTOR;

// ============================================================================
// SAGE26 CONSTANTS (R/src/model_starformation_and_feedback.c)
// ============================================================================

/* Kauffmann (1996) eq. 7 critical cold-gas coefficient (:66) */
static const double KAUFFMANN96_SF_THRESHOLD = 0.19;

/* Primordial hydrogen mass fraction, HYDROGEN_MASS_FRAC
 * (R/src/model_starformation_and_feedback.h:14) */
static const double HYDROGEN_MASS_FRAC = 0.74;

/* Critical neutral surface density of the H I ionisation cut, Msun/pc^2 (:97) */
static const double SIGMA_HI_CRIT = 0.5;

/* Atomic-disk to stellar-disk scale-length ratio chi (:106) */
static const double GAS_DISK_RADIUS_FACTOR = 1.0;

// ============================================================================
// HELPER FUNCTIONS
// ============================================================================

/** @brief SAGE26's atomic_disk_radius: chi * r_s (:130-133) */
static double atomic_disk_radius(const double rs_code) { return GAS_DISK_RADIUS_FACTOR * rs_code; }

/**
 * @brief SAGE26's ionized_gas_fraction (:135-152): ionised mass fraction of the cold disk
 *
 * For an exponential gas disk of central surface density Sigma0 = M / (2 pi r_s^2)
 * the gas beyond the radius where Sigma falls to sigma_hi_crit is ionised:
 * f_ion = (1 + ln(Sigma0 / sigma_hi_crit)) * sigma_hi_crit / Sigma0, clamped to
 * [0, 1]. Non-positive inputs give 0; a disk entirely below the cut gives 1. The
 * code-to-physical conversions are SAGE26's CODE_MASS_TO_MSUN and CODE_LENGTH_TO_PC
 * macros (R/src/macros.h:45-46), each left to right.
 */
static double ionized_gas_fraction(const double coldgas_code, const double rs_code, const double h,
                                   const double sigma_hi_crit) {
  if (coldgas_code <= 0.0 || rs_code <= 0.0 || sigma_hi_crit <= 0.0 || h <= 0.0) {
    return 0.0;
  }
  const double mgas = coldgas_code * 1.0e10 / h;             /* Msun */
  const double rs_pc = rs_code * 1.0e6 / h;                  /* pc */
  const double sigma0 = mgas / (2.0 * M_PI * rs_pc * rs_pc); /* Msun/pc^2 */
  if (sigma0 <= sigma_hi_crit) {
    return 1.0;
  }
  const double x = log(sigma0 / sigma_hi_crit);
  double f_ion = (1.0 + x) * sigma_hi_crit / sigma0;
  if (f_ion < 0.0) {
    f_ion = 0.0;
  }
  if (f_ion > 1.0) {
    f_ion = 1.0;
  }
  return f_ion;
}

/**
 * @brief SAGE26's H I block of starformation_and_feedback (:1035-1046)
 *
 * SAGE parity: R0 (SFprescription 0) computes H I on every call although its star
 * formation ignores it, from the cold gas before this substep's star formation and
 * the inherited H2gas, which R0 never writes. The prescription-0 path does not
 * resynchronise H I after the depletion (:1128-1135, :1169-1172 apply only to
 * prescriptions that track H2), so H I can exceed the hydrogen left after star
 * formation and feedback: a deliberate SAGE26 quirk.
 */
static void calculate_h1gas(struct GalaxyData *gal, const double hubble_h) {
  /* SAGE parity: SAGE26's build fuses ColdGas * HYDROGEN_MASS_FRAC - H2gas into one
   * multiply-subtract (:1036) */
  double atomicH = fma((double)gal->ColdGas, HYDROGEN_MASS_FRAC, -(double)gal->H2gas);
  if (atomicH < 0.0) {
    atomicH = 0.0;
  }
  const double f_ion = ionized_gas_fraction(gal->ColdGas, atomic_disk_radius(gal->DiskScaleRadius),
                                            hubble_h, SIGMA_HI_CRIT);
  atomicH *= (1.0 - f_ion);
  gal->H1gas = atomicH;
}

// ============================================================================
// MODULE LIFECYCLE FUNCTIONS
// ============================================================================

int sage26_calculate_star_formation_init(void) {
  LOAD_AND_VALIDATE_RANGE_INCLUSIVE("SfrEfficiency", SFR_EFFICIENCY, 0.0, 1.0,
                                    "star formation efficiency");
  LOAD_AND_VALIDATE_RANGE_INCLUSIVE("StarFormingDiskFactor", STAR_FORMING_DISK_FACTOR, 0.0, 10.0,
                                    "star forming disk factor");

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

  // sfr_croton06 (:160-184). The star-forming region is StarFormingDiskFactor * r_s,
  // entering exactly where SAGE26's double SF_DISK_RADIUS_FRAC (3.0, :60) does; the
  // run files hold 3.0.
  const float vvir = (float)halo->Vvir;
  const double reff = STAR_FORMING_DISK_FACTOR * gal->DiskScaleRadius;
  double strdot = 0.0;
  /* SAGE parity: a non-positive own Vvir forms no stars, before tdyn is formed (:170-171) */
  if (vvir > 0.0) {
    const double tdyn = reff / vvir;
    // From Kauffmann (1996) eq7 x piR^2, (Vvir in km/s, reff in Mpc/h) in units of 10^10Msun/h
    const double cold_crit = KAUFFMANN96_SF_THRESHOLD * vvir * reff;
    if (gal->ColdGas > cold_crit && tdyn > 0.0) {
      strdot = SFR_EFFICIENCY * (gal->ColdGas - cold_crit) / tdyn;
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
