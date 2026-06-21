/**
 * @file    sage_calculate_cooling_budget.c
 * @brief   SAGE cooling budget calculation - computes CoolingGas budget for substep
 *
 * Calculates gas cooling from hot halos based on cooling radius and regime.
 * Two cooling regimes: cold accretion (Rcool > Rvir) or hot halo cooling (Rcool < Rvir).
 * Uses metallicity-dependent cooling functions from Sutherland & Dopita (1993).
 *
 * Reference: White & Frenk (1991), Croton et al. (2006, 2016)
 */

#include <math.h>

#include "constants.h"
#include "error.h"
#include "shared/metallicity.h"
#include "shared/sage_constants.h"
#include "shared/time_parity.h"
#include "module_system/physical_constants.h"
#include "module_interface.h"
#include "module_registry.h"
#include "types.h"
#include "cooling_tables.h"

// ============================================================================
// HELPER FUNCTIONS
// ============================================================================

/**
 * @brief Calculate gas cooling based on halo properties and cooling functions
 *
 * Two regimes: (1) cold accretion when rcool > Rvir, (2) hot halo cooling when rcool < Rvir
 */
static double cooling_recipe(struct Halo *halo, struct ModuleContext *ctx, const double dt,
                             double *rcool_out, double *lambda_out) {
  double coolingGas;

  if (halo->galaxy->HotGas > 0.0 && halo->Vvir > 0.0) {
    const double tcool = halo->Rvir / halo->Vvir;
    const double temp = SAGE_TVIR_K_PER_SQKMS * halo->Vvir * halo->Vvir; // T_vir in Kelvin

    double logZ = -10.0;
    if (halo->galaxy->MetalsHotGas > 0.0) {
      logZ = log10(halo->galaxy->MetalsHotGas / halo->galaxy->HotGas);
    }

    double lambda = get_metaldependent_cooling_rate(log10(temp), logZ);
    double x = PROTONMASS * BOLTZMANN * temp / lambda;                   // sec * g/cm^3
    x /= (ctx->params->UnitDensity_in_cgs * ctx->params->UnitTime_in_s); // convert to code units

    const double rho_rcool = x / tcool * 0.885; // 0.885 = 3/2 * mu, mu=0.59 for fully ionized gas

    // Isothermal density profile for hot gas
    const double rho0 = halo->galaxy->HotGas / (4.0 * M_PI * halo->Rvir);
    const double rcool = sqrt(rho0 / rho_rcool);

    coolingGas = 0.0;
    if (rcool > halo->Rvir) {
      // Cold accretion regime
      coolingGas = halo->galaxy->HotGas / (halo->Rvir / halo->Vvir) * dt;
    } else {
      // Hot halo cooling regime
      coolingGas = (halo->galaxy->HotGas / halo->Rvir) * (rcool / (2.0 * tcool)) * dt;
    }

    if (coolingGas > halo->galaxy->HotGas) {
      coolingGas = halo->galaxy->HotGas;
    } else {
      if (coolingGas < 0.0)
        coolingGas = 0.0;
    }

    *rcool_out = rcool;
    *lambda_out = lambda;
  } else {
    coolingGas = 0.0;
    *rcool_out = 0.0;
    *lambda_out = 0.0;
  }

  return coolingGas;
}

// ============================================================================
// MODULE LIFECYCLE FUNCTIONS
// ============================================================================

int sage_calculate_cooling_budget_init(void) {
  // Initialize cooling function tables from the active model package.
  if (cooling_tables_init(MIMIC_COMPILED_MODEL_PATH
                          "/modules/sage_calculate_cooling_budget/CoolFunctions") != 0) {
    ERROR_LOG("Failed to initialize cooling function tables");
    return -1;
  }

  VERBOSE_LOG("SAGE calculate cooling budget module initialized");
  return 0;
}

int sage_calculate_cooling_budget_process(struct ModuleContext *ctx, struct Halo *halos, int ngal) {
  double dt_obj = 0.0;
  enum MimicObjectTimeStatus dt_status;

  if (ngal != 1) {
    ERROR_LOG("process_by_galaxy expects ngal=1, got %d", ngal);
    return -1;
  }

  struct Halo *halo = &halos[0];

  // SAGE parity: cooling runs for every non-merged galaxy, including Type 2
  // orphans, which retain and cool their own hot gas until they merge.
  // Safe for orphans because Type 2 creation zeros Mvir/Len but preserves the
  // prior Rvir/Vvir (build_model.c), and cooling_recipe() needs only HotGas>0
  // && Vvir>0 (not Mvir), so an orphan with Mvir=0 still cools correctly.
  if (halo->galaxy == NULL) {
    return 0;
  }

  dt_status = mimic_object_substep_dt(halo, ctx, &dt_obj);
  if (dt_status == MIMIC_OBJECT_TIME_SKIP_INITIAL) {
    return 0;
  }
  if (dt_status != MIMIC_OBJECT_TIME_OK) {
    ERROR_LOG("Invalid cooling dt for halo %d (SnapNum=%d, dT=%.3e, num_substeps=%d, status=%s)",
              halo->HaloNr, halo->SnapNum, halo->dT, (ctx != NULL) ? ctx->num_substeps : -1,
              mimic_object_time_status_str(dt_status));
    return -1;
  }

  // Calculate cooling using per-object substep timestep (SAGE parity)
  double rcool, lambda;
  double coolingGas = cooling_recipe(halo, ctx, dt_obj, &rcool, &lambda);

  // Store in properties for subsequent modules (double properties: no rounding
  // between budget, AGN-heating, and apply steps — SAGE keeps these as double locals)
  halo->galaxy->CoolingGas = coolingGas;
  halo->galaxy->Rcool = rcool;
  halo->galaxy->CoolingLambda = lambda;

  return 0;
}

int sage_calculate_cooling_budget_cleanup(void) {
  cooling_tables_cleanup();
  VERBOSE_LOG("SAGE calculate cooling budget module cleaned up");
  return 0;
}
