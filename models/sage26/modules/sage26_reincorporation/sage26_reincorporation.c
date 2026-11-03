/**
 * @file    sage26_reincorporation.c
 * @brief   SAGE26 reincorporation module - returns ejected gas to hot halo in massive systems
 *
 * Ports SAGE26's reincorporate_gas (model_reincorporation.c:44-66) on the R0 path
 * (CGMrecipeOn 0): a central whose Vvir exceeds Vcrit = 354.53 * ReIncorporationFactor
 * recaptures previously ejected gas into its hot halo at the rate
 * (Vvir/Vcrit - 1) * EjectedGas / (Rvir/Vvir), with the ejected metallicity.
 *
 * Every floating-point expression is transcribed from SAGE26 with its operand
 * order and types, so that compiler contraction treats both alike.
 *
 * Reference: Croton et al. (2006, 2016)
 */

#include "constants.h"
#include "error.h"
#include "module_interface.h"
#include "module_registry.h"
#include "types.h"
#include "shared/metallicity.h"
#include "shared/time_parity.h"
#include "module_system/parameter_helpers.h"

// ============================================================================
// MODULE PARAMETERS AND CONSTANTS
// ============================================================================

static double REINCORPORATION_FACTOR;

/* SAGE26 model_reincorporation.c:28-34: SN ejecta leave the disk at ~501 km/s; a
 * halo retains them when its escape velocity sqrt(2) * Vvir exceeds that, i.e.
 * when Vvir > 501 / sqrt(2) ~ 354.53 km/s, scaled by ReIncorporationFactor. */
static const double REINC_VCRIT_KMS = 354.53;

// ============================================================================
// HELPER FUNCTIONS
// ============================================================================

/**
 * @brief SAGE26 add_gas_to_hot_reservoir (model_misc.h:76-85), CGMrecipeOn 0 branch
 *
 * SAGE parity: the metal mass arrives as an argument already rounded to double,
 * so the caller's product and this sum are two operations, as in SAGE26
 * (model_reincorporation.c:64), not one fused multiply-add.
 */
static inline void add_gas_to_hot_reservoir(struct GalaxyData *central, const double gas,
                                            const double metals) {
  central->HotGas += gas;
  central->MetalsHotGas += metals;
}

// ============================================================================
// MODULE LIFECYCLE FUNCTIONS
// ============================================================================

int sage26_reincorporation_init(void) {
  LOAD_AND_VALIDATE_RANGE_INCLUSIVE("ReIncorporationFactor", REINCORPORATION_FACTOR, 0.0, 10.0,
                                    "reincorporation efficiency factor");

  VERBOSE_LOG("SAGE reincorporation module initialized");
  VERBOSE_LOG("  ReIncorporationFactor = %.3f", REINCORPORATION_FACTOR);
  VERBOSE_LOG("  Critical velocity = %.2f km/s", REINC_VCRIT_KMS * REINCORPORATION_FACTOR);

  return 0;
}

int sage26_reincorporation_process(struct ModuleContext *ctx, struct Halo *halos, int ngal) {
  double dt_obj = 0.0;
  enum MimicObjectTimeStatus dt_status;
  struct Halo *central_halo;
  struct GalaxyData *gal;

  /* SAGE parity: acts on the FOF central only, as SAGE26 calls reincorporate_gas
   * for p == centralgal (core_build_model.c:486-492, model_reincorporation.c:44). */
  if (ctx == NULL || halos == NULL || ngal <= 0 || ctx->central_galaxy == NULL) {
    return 0;
  }

  // Direct single-halo calls on satellites/orphans should remain a no-op.
  if (ngal == 1 && halos[0].Type != 0) {
    return 0;
  }

  central_halo = ctx->central_galaxy;
  if (central_halo->Type != 0) {
    return 0;
  }

  if (central_halo->galaxy == NULL) {
    ERROR_LOG("Central halo has NULL galaxy data");
    return -1;
  }

  gal = central_halo->galaxy;

  // Skip if no ejected gas to reincorporate. SAGE26 has no such guard, but on
  // R0's domain (no negative reservoirs) it is a no-op there too: the
  // reincorporated mass scales with EjectedMass, so zero stays zero.
  if (gal->EjectedGas <= 0.0f) {
    return 0;
  }

  // Vvir and Rvir are read as the floats SAGE26 holds (core_allvars.h:131-132)
  const float Vvir = (float)central_halo->Vvir;
  const float Rvir = (float)central_halo->Rvir;

  const double Vcrit = REINC_VCRIT_KMS * REINCORPORATION_FACTOR;

  // SAGE26's guards: positive Vcrit, own Vvir above it, positive own Rvir
  // (model_reincorporation.c:48)
  if (!(Vcrit > 0.0 && Vvir > Vcrit && Rvir > 0.0)) {
    return 0;
  }

  dt_status = mimic_object_substep_dt(central_halo, ctx, &dt_obj);
  if (dt_status == MIMIC_OBJECT_TIME_SKIP_INITIAL) {
    return 0;
  }
  if (dt_status != MIMIC_OBJECT_TIME_OK) {
    ERROR_LOG("Invalid reincorporation dt for halo %lld (SnapNum=%d, dT=%.3e, num_substeps=%d, "
              "status=%s)",
              central_halo->HaloNr, central_halo->SnapNum, central_halo->dT,
              (ctx != NULL) ? ctx->num_substeps : -1, mimic_object_time_status_str(dt_status));
    return -1;
  }

  /* (Vvir/Vcrit - 1) * M_eject / (Rvir/Vvir) * dt (model_reincorporation.c:50-52).
   * SAGE parity: Rvir / Vvir is a float division, as SAGE26's float fields make
   * it, before it enters the double expression. */
  double reincorporated = (Vvir / Vcrit - 1.0) * gal->EjectedGas / (Rvir / Vvir) * dt_obj;

  if (reincorporated < 0.0) {
    ERROR_LOG("Negative reincorporation: %.3e (Vvir=%.1f, Rvir=%.3f, EjectedGas=%.3e, dt=%.3e)",
              reincorporated, (double)Vvir, (double)Rvir, gal->EjectedGas, dt_obj);
    return -1;
  }

  if (reincorporated > gal->EjectedGas) {
    reincorporated = gal->EjectedGas;
  }

  const double metallicity = mimic_get_metallicity(gal->EjectedGas, gal->MetalsEjectedGas);

  // Remove from the ejected reservoir, add to the hot one (model_reincorporation.c:59-64)
  gal->EjectedGas -= reincorporated;
  gal->MetalsEjectedGas -= metallicity * reincorporated;

  add_gas_to_hot_reservoir(gal, reincorporated, metallicity * reincorporated);

  return 0;
}

int sage26_reincorporation_cleanup(void) {
  VERBOSE_LOG("SAGE reincorporation module cleaned up");
  return 0;
}
