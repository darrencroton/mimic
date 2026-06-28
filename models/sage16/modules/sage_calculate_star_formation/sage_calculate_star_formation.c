/**
 * @file    sage_calculate_star_formation.c
 * @brief   SAGE star formation - computes stellar mass formed this substep (swappable SF rate
 * prescription)
 *
 * Calculates star formation via Kennicutt-Schmidt efficiency-based model with
 * critical threshold. Stores result in NewStellarMass property for subsequent
 * processing by feedback and update modules.
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
// MODULE LIFECYCLE FUNCTIONS
// ============================================================================

int sage_calculate_star_formation_init(void) {
  LOAD_AND_VALIDATE_RANGE_INCLUSIVE("SfrEfficiency", SFR_EFFICIENCY, 0.0, 1.0,
                                    "star formation efficiency");
  LOAD_AND_VALIDATE_RANGE_INCLUSIVE("StarFormingDiskFactor", STAR_FORMING_DISK_FACTOR, 0.0, 10.0,
                                    "star forming disk factor");

  /* Dependency check: apply step is required to commit NewStellarMass to galaxy
   * reservoirs.  Without it, every substep's SF results are silently discarded. */
  if (!module_configured_anywhere("sage_apply_star_formation_supernova")) {
    ERROR_LOG("sage_calculate_star_formation requires sage_apply_star_formation_supernova "
              "in the pipeline — without it, NewStellarMass is computed each "
              "substep but never committed to galaxy reservoirs (silent output loss)");
    return -1;
  }

  VERBOSE_LOG("SAGE star formation module initialized");
  VERBOSE_LOG("  SfrEfficiency = %.4f", SFR_EFFICIENCY);
  VERBOSE_LOG("  StarFormingDiskFactor = %.2f", STAR_FORMING_DISK_FACTOR);

  return 0;
}

int sage_calculate_star_formation_process(struct ModuleContext *ctx, struct Halo *halos, int ngal) {
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
        "Invalid star formation dt for halo %d (SnapNum=%d, dT=%.3e, num_substeps=%d, status=%s)",
        halo->HaloNr, halo->SnapNum, halo->dT, (ctx != NULL) ? ctx->num_substeps : -1,
        mimic_object_time_status_str(dt_status));
    return -1;
  }

  // Star formation recipe: Kennicutt-Schmidt with critical threshold
  // We take the typical star forming region as STAR_FORMING_DISK_FACTOR*r_s (typically 3.0*r_s)
  const double reff = STAR_FORMING_DISK_FACTOR * gal->DiskScaleRadius;
  const double tdyn = reff / halo->Vvir;

  // From Kauffmann (1996) eq7 x piR^2, (Vvir in km/s, reff in Mpc/h) in units of 10^10Msun/h
  const double cold_crit = 0.19 * halo->Vvir * reff;

  double strdot = 0.0;
  if (gal->ColdGas > cold_crit && tdyn > 0.0) {
    strdot = SFR_EFFICIENCY * (gal->ColdGas - cold_crit) / tdyn;
  }

  double stars = strdot * dt;
  if (stars < 0.0) {
    stars = 0.0;
  }

  gal->NewStellarMass = stars;

  DEBUG_LOG("Type=%d: Calculated SF=%.3e, SFR=%.3e, ColdGas=%.3e, DiskScaleRadius=%.3e", halo->Type,
            stars, stars / dt, gal->ColdGas, gal->DiskScaleRadius);

  return 0;
}

int sage_calculate_star_formation_cleanup(void) {
  VERBOSE_LOG("SAGE star formation module cleaned up");
  return 0;
}
