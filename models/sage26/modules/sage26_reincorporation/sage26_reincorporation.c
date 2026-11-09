/**
 * @file    sage26_reincorporation.c
 * @brief   SAGE26 reincorporation module - returns ejected gas to hot halo in massive systems
 *
 * Implements SAGE26's reincorporate_gas: a central whose Vvir exceeds
 * Vcrit = 354.53 * ReIncorporationFactor recaptures previously ejected gas at the rate
 * (Vvir/Vcrit - 1) * EjectedGas / (Rvir/Vvir), with the ejected metallicity. The required parameter
 * CGMrecipeOn selects only the destination, through SAGE26's regime-aware add_gas_to_hot_reservoir
 * (shared/sage_cgm_routing.h): the central's CGM pair with CGMrecipeOn 1 and Regime 0, its hot
 * pair otherwise. The amount, guards, cap, metallicity and ejected debits are the same either way.
 *
 * Reference: Croton et al. (2006, 2016)
 *
 * Ported from SAGE26 (MIT; Copyright (c) 2019-2026 Semi-Analytic Galaxy Evolution (SAGE)); see
 * models/sage26/LICENSE-SAGE26.
 */

#include "constants.h"
#include "error.h"
#include "module_interface.h"
#include "module_registry.h"
#include "types.h"
#include "shared/metallicity.h"
#include "shared/sage_cgm_routing.h"
#include "shared/time_parity.h"
#include "module_system/parameter_helpers.h"

// ============================================================================
// MODULE PARAMETERS AND CONSTANTS
// ============================================================================

#define MODULE_NAME "sage26_reincorporation"

static double REINCORPORATION_FACTOR;

/* SAGE26's CGMrecipeOn (0 hot-gas routing, 1 regime-aware CGM routing), required and
 * checked at init */
static int CGM_RECIPE_ON;

/* SN ejecta leave the disk at ~501 km/s; a
 * halo retains them when its escape velocity sqrt(2) * Vvir exceeds that, i.e.
 * when Vvir > 501 / sqrt(2) ~ 354.53 km/s, scaled by ReIncorporationFactor. */
static const double REINC_VCRIT_KMS = 354.53;

// ============================================================================
// MODULE LIFECYCLE FUNCTIONS
// ============================================================================

int sage26_reincorporation_init(void) {
  /* Required, no default: a missing key fails here */
  LOAD_PARAM_INT("CGMrecipeOn", CGM_RECIPE_ON);
  if (mimic_sage_validate_cgm_recipe_on(MODULE_NAME, CGM_RECIPE_ON) != 0) {
    return -1;
  }

  LOAD_AND_VALIDATE_RANGE_INCLUSIVE("ReIncorporationFactor", REINCORPORATION_FACTOR, 0.0, 10.0,
                                    "reincorporation efficiency factor");

  VERBOSE_LOG("SAGE reincorporation module initialized");
  VERBOSE_LOG("  ReIncorporationFactor = %.3f", REINCORPORATION_FACTOR);
  VERBOSE_LOG("  Critical velocity = %.2f km/s", REINC_VCRIT_KMS * REINCORPORATION_FACTOR);
  VERBOSE_LOG("  CGMrecipeOn = %d", CGM_RECIPE_ON);

  return 0;
}

int sage26_reincorporation_process(struct ModuleContext *ctx, struct Halo *halos, int ngal) {
  double dt_obj = 0.0;
  enum MimicObjectTimeStatus dt_status;
  struct Halo *central_halo;
  struct GalaxyData *gal;

  /* SAGE parity: acts on the FOF central only, as SAGE26 calls reincorporate_gas
   * for p == centralgal. */
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

  // Skip if no ejected gas to reincorporate. SAGE26 has no such guard, and it gates the CGM
  // destination too. For EjectedGas +0.0 SAGE26 adds +0.0 to the destination, which turns a
  // -0.0 destination field into +0.0 where this guard leaves -0.0; the two differ only in an
  // exact zero's sign bit. EjectedGas is never negative: the package floors it at infall
  // preparation and clears it to +0.0 when infall exhausts it, as SAGE26 does.
  if (gal->EjectedGas <= 0.0f) {
    return 0;
  }

  // SAGE parity: Vvir and Rvir are read as the floats SAGE26 holds
  const float Vvir = (float)central_halo->Vvir;
  const float Rvir = (float)central_halo->Rvir;

  const double Vcrit = REINC_VCRIT_KMS * REINCORPORATION_FACTOR;

  // SAGE26's guards: positive Vcrit, own Vvir above it, positive own Rvir
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

  /* (Vvir/Vcrit - 1) * M_eject / (Rvir/Vvir) * dt.
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

  // Remove from the ejected reservoir, the same for every regime
  gal->EjectedGas -= reincorporated;
  gal->MetalsEjectedGas -= metallicity * reincorporated;

  /* Add to the central's regime-selected reservoir (SAGE26's add_gas_to_hot_reservoir) */
  mimic_sage_add_gas_to_hot_reservoir(gal, CGM_RECIPE_ON, reincorporated,
                                      metallicity * reincorporated);

  return 0;
}

int sage26_reincorporation_cleanup(void) {
  VERBOSE_LOG("SAGE reincorporation module cleaned up");
  return 0;
}
