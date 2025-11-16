/**
 * @file    sage_infall.c
 * @brief   SAGE infall module implementation
 *
 * This module implements gas infall and stripping processes from the SAGE model.
 * It handles:
 * - Cosmological gas infall onto halos
 * - Reionization suppression of gas accretion onto low-mass halos
 * - Stripping of hot gas from satellite galaxies
 * - Redistribution of baryons between galaxies
 *
 * Physics:
 *   infallingMass = f_reion * BaryonFrac * Mvir - (total baryon content)
 *   HotGas += infallingMass
 *
 * The reionization suppression follows Gnedin (2000) with fitting formulas
 * from Kravtsov et al. (2004). After the universe is reionized, low-mass halos
 * have their gas accretion suppressed due to the increased Jeans mass.
 *
 * Implementation Notes:
 * - Central galaxies accrete gas from the cosmic web
 * - Satellites can only lose gas through stripping
 * - All baryonic components are tracked for mass conservation
 * - Metal content is preserved during gas transfers
 *
 * Reference:
 *   - Croton et al. (2016) - SAGE model description
 *   - Gnedin (2000) - Reionization model
 *   - Kravtsov et al. (2004) - Filtering mass formulas
 *   - SAGE: sage-code/model_infall.c
 *
 * Vision Principles:
 *   - Physics-Agnostic Core: Interacts only through module interface
 *   - Runtime Modularity: Configurable via parameter file
 *   - Single Source of Truth: Updates GalaxyData properties only
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "constants.h"
#include "error.h"
#include "../shared/metallicity.h"  // Shared utility for metallicity calculations
#include "module_interface.h"
#include "module_registry.h"
#include "numeric.h"
#include "sage_infall.h"
#include "types.h"

// ============================================================================
// MODULE PARAMETERS
// ============================================================================

/**
 * @brief   Cosmic baryon fraction
 *
 * The universal mass fraction in baryons (Omega_b / Omega_m).
 * Typical value from Planck: 0.17
 *
 * Configuration: SageInfall_BaryonFrac
 */
static double BARYON_FRAC = 0.17;

/**
 * @brief   Enable reionization suppression
 *
 * If enabled (1), gas accretion onto low-mass halos is suppressed after
 * cosmic reionization using the Gnedin (2000) model.
 *
 * Configuration: SageInfall_ReionizationOn
 */
static int REIONIZATION_ON = 1;

/**
 * @brief   Reionization parameter: redshift when UV background turns on
 *
 * Epoch when the UV background from first stars begins to heat the IGM.
 * Typical value: z0 = 8.0
 *
 * Configuration: SageInfall_Reionization_z0
 */
static double REIONIZATION_Z0 = 8.0;

/**
 * @brief   Reionization parameter: redshift of full reionization
 *
 * Epoch when the universe becomes fully reionized.
 * Typical value: zr = 7.0
 *
 * Configuration: SageInfall_Reionization_zr
 */
static double REIONIZATION_ZR = 7.0;

// Derived reionization parameters (calculated in init)
static double a0 = 0.0; /* Scale factor when UV background turns on */
static double ar = 0.0; /* Scale factor at full reionization */

/**
 * @brief   Number of substeps for satellite stripping
 *
 * Satellite gas stripping is applied gradually over STEPS timesteps
 * to approximate continuous environmental effects.
 *
 * Configuration: SageInfall_StrippingSteps
 */
static int STRIPPING_STEPS = 10;

// ============================================================================
// HELPER FUNCTIONS (Physics Calculations)
// ============================================================================

// Metallicity calculation now provided by shared utility: mimic_get_metallicity()
// See: src/modules/shared/metallicity.h

/**
 * @brief   Calculates the reionization suppression factor for gas accretion
 *
 * Implements the Gnedin (2000) reionization model with fitting formulas
 * from Kravtsov et al. (2004) Appendix B.
 *
 * The suppression depends on the ratio between the halo mass and a
 * characteristic mass (the maximum of the filtering mass and the mass
 * corresponding to a virial temperature of 10^4 K).
 *
 * @param   mvir       Virial mass of halo (1e10 Msun/h)
 * @param   redshift   Current redshift
 * @param   omega      Matter density parameter
 * @param   omega_lambda Dark energy density parameter
 * @param   hubble_h   Hubble parameter (H0 / 100 km/s/Mpc)
 * @return  Modifier factor (between 0 and 1) for gas accretion
 */
static double do_reionization(float mvir, double redshift, double omega,
                              double omega_lambda, double hubble_h) {
  const double alpha = 6.0;   /* Best fit to Gnedin data */
  const double Tvir = 1e4;    /* Virial temperature (K) */
  const double EPSILON = 1e-10;

  /* Calculate scale factor and ratios */
  double a = 1.0 / (1.0 + redshift);
  double a_on_a0 = a / a0;
  double a_on_ar = a / ar;

  /* Calculate f_of_a from Kravtsov et al. (2004) fitting formula */
  double f_of_a;
  if (a <= a0) {
    /* Before UV background turns on */
    f_of_a = 3.0 * a / ((2.0 + alpha) * (5.0 + 2.0 * alpha)) *
             pow(a_on_a0, alpha);
  } else if (a < ar) {
    /* During partial reionization */
    f_of_a = (3.0 / a) * a0 * a0 *
                 (1.0 / (2.0 + alpha) -
                  2.0 * pow(a_on_a0, -0.5) / (5.0 + 2.0 * alpha)) +
             a * a / 10.0 - (a0 * a0 / 10.0) * (5.0 - 4.0 * pow(a_on_a0, -0.5));
  } else {
    /* After full reionization */
    f_of_a = (3.0 / a) *
                 (a0 * a0 *
                      (1.0 / (2.0 + alpha) -
                       2.0 * pow(a_on_a0, -0.5) / (5.0 + 2.0 * alpha)) +
                  (ar * ar / 10.0) * (5.0 - 4.0 * pow(a_on_ar, -0.5)) -
                  (a0 * a0 / 10.0) * (5.0 - 4.0 * pow(a_on_a0, -0.5)) +
                  a * ar / 3.0 - (ar * ar / 3.0) * (3.0 - 2.0 * pow(a_on_ar, -0.5)));
  }

  /* Calculate filtering mass (in units of 1e10 Msun/h) */
  /* Note: mu=0.59 and mu^-1.5 = 2.21 for fully ionized gas */
  double Mjeans = 25.0 * pow(omega, -0.5) * 2.21;
  double Mfiltering = Mjeans * pow(f_of_a, 1.5);

  /* Calculate characteristic mass from virial temperature */
  double Vchar = sqrt(Tvir / 36.0); /* Characteristic velocity */

  /* Cosmological parameters at current redshift */
  double omegaZ = omega * pow(1.0 + redshift, 3.0) /
                  (omega * pow(1.0 + redshift, 3.0) + omega_lambda + EPSILON);
  double xZ = omegaZ - 1.0;
  double deltacritZ =
      18.0 * M_PI * M_PI + 82.0 * xZ - 39.0 * xZ * xZ; /* Critical overdensity */

  /* Hubble parameter at z (in units of 100 km/s/Mpc) */
  double H0 = 100.0 * hubble_h; /* km/s/Mpc */
  double HubbleZ = H0 * sqrt(omega * pow(1.0 + redshift, 3.0) + omega_lambda);

  /* Convert to code units: G in (Mpc/h) (km/s)^2 / (1e10 Msun/h) */
  double G_code = GRAVITY * (1.0 / 1000.0) * (1.0 / 1000.0) * /* km^2/s^2 */
                  CM_PER_MPC * hubble_h *                      /* Mpc/h */
                  (1.0 / SOLAR_MASS) * (1.0 / 1e10) *          /* 1/(1e10 Msun) */
                  (1.0 / hubble_h);                            /* /h */

  /* Calculate characteristic mass */
  double Mchar = Vchar * Vchar * Vchar /
                 (G_code * HubbleZ * sqrt(0.5 * deltacritZ) + EPSILON);

  /* Use maximum of filtering mass and characteristic mass */
  double mass_to_use = fmax(Mfiltering, Mchar);

  /* Calculate suppression modifier */
  double modifier =
      1.0 / pow(1.0 + 0.26 * mass_to_use / (mvir + EPSILON), 3.0);

  return modifier;
}

/**
 * @brief   Calculate the amount of gas infalling onto a central galaxy
 *
 * Calculates the amount of gas that should be accreted onto a halo based on:
 * 1. Cosmic baryon fraction × halo mass
 * 2. Current baryon content (all components)
 * 3. Reionization suppression (if enabled)
 *
 * Also handles:
 * - Consolidation of ejected gas from satellites to central
 * - Consolidation of ICS (intracluster stars) to central
 * - Mass conservation accounting
 *
 * @param   halos    Array of halos in FOF group
 * @param   ngal     Number of halos
 * @param   central_idx Index of central galaxy
 * @param   redshift Current redshift
 * @param   omega    Matter density parameter
 * @param   omega_lambda Dark energy density parameter
 * @param   hubble_h Hubble parameter
 * @return  Mass of infalling gas (can be negative for mass loss)
 */
static double infall_recipe(struct Halo *halos, int ngal, int central_idx,
                             double redshift, double omega, double omega_lambda,
                             double hubble_h) {
  double tot_stellarMass, tot_coldMass, tot_hotMass, tot_ejected;
  double tot_hotMetals, tot_ejectedMetals;
  double tot_ICS, tot_ICSMetals;
  double tot_satBaryons;
  double infallingMass, reionization_modifier;

  /* Initialize counters for all baryonic components */
  tot_stellarMass = tot_coldMass = tot_hotMass = tot_hotMetals = tot_ejected =
      tot_ejectedMetals = tot_ICS = tot_ICSMetals = tot_satBaryons = 0.0;

  /* Loop over all galaxies in FOF group to sum baryonic components */
  for (int i = 0; i < ngal; i++) {
    if (halos[i].galaxy == NULL)
      continue;

    /* Sum all baryonic mass components */
    tot_stellarMass += halos[i].galaxy->StellarMass;
    tot_coldMass += halos[i].galaxy->ColdGas;
    tot_hotMass += halos[i].galaxy->HotGas;
    tot_hotMetals += halos[i].galaxy->MetalsHotGas;
    tot_ejected += halos[i].galaxy->EjectedMass;
    tot_ejectedMetals += halos[i].galaxy->MetalsEjectedMass;
    tot_ICS += halos[i].galaxy->ICS;
    tot_ICSMetals += halos[i].galaxy->MetalsICS;

    /* Track baryons in satellite galaxies separately */
    if (i != central_idx) {
      tot_satBaryons +=
          halos[i].galaxy->StellarMass + halos[i].galaxy->ColdGas +
          halos[i].galaxy->HotGas;
    }

    /* Move satellite ejected gas to central galaxy's ejected reservoir */
    if (i != central_idx) {
      halos[i].galaxy->EjectedMass = 0.0f;
      halos[i].galaxy->MetalsEjectedMass = 0.0f;
    }

    /* Move satellite intracluster stars to central galaxy */
    if (i != central_idx) {
      halos[i].galaxy->ICS = 0.0f;
      halos[i].galaxy->MetalsICS = 0.0f;
    }
  }

  /* Calculate reionization suppression factor if enabled */
  if (REIONIZATION_ON) {
    reionization_modifier =
        do_reionization(halos[central_idx].Mvir, redshift, omega, omega_lambda,
                        hubble_h);
  } else {
    reionization_modifier = 1.0;
  }

  /* Calculate infalling gas mass */
  infallingMass =
      reionization_modifier * BARYON_FRAC * halos[central_idx].Mvir -
      (tot_stellarMass + tot_coldMass + tot_hotMass + tot_ejected + tot_ICS);

  /* Assign all ejected mass to the central galaxy */
  halos[central_idx].galaxy->EjectedMass = (float)tot_ejected;
  halos[central_idx].galaxy->MetalsEjectedMass = (float)tot_ejectedMetals;

  /* Enforce physical constraints on ejected mass and metals */
  if (halos[central_idx].galaxy->MetalsEjectedMass >
      halos[central_idx].galaxy->EjectedMass) {
    halos[central_idx].galaxy->MetalsEjectedMass =
        halos[central_idx].galaxy->EjectedMass;
  }
  if (halos[central_idx].galaxy->EjectedMass < 0.0f) {
    halos[central_idx].galaxy->EjectedMass = 0.0f;
    halos[central_idx].galaxy->MetalsEjectedMass = 0.0f;
  }
  if (halos[central_idx].galaxy->MetalsEjectedMass < 0.0f) {
    halos[central_idx].galaxy->MetalsEjectedMass = 0.0f;
  }

  /* Assign all intracluster stars to the central galaxy */
  halos[central_idx].galaxy->ICS = (float)tot_ICS;
  halos[central_idx].galaxy->MetalsICS = (float)tot_ICSMetals;

  /* Enforce physical constraints on ICS mass and metals */
  if (halos[central_idx].galaxy->MetalsICS >
      halos[central_idx].galaxy->ICS) {
    halos[central_idx].galaxy->MetalsICS = halos[central_idx].galaxy->ICS;
  }
  if (halos[central_idx].galaxy->ICS < 0.0f) {
    halos[central_idx].galaxy->ICS = 0.0f;
    halos[central_idx].galaxy->MetalsICS = 0.0f;
  }
  if (halos[central_idx].galaxy->MetalsICS < 0.0f) {
    halos[central_idx].galaxy->MetalsICS = 0.0f;
  }

  /* Update tracking variable */
  halos[central_idx].galaxy->TotalSatelliteBaryons = (float)tot_satBaryons;

  return infallingMass;
}

/**
 * @brief   Strip hot gas from satellite galaxies
 *
 * Implements environmental stripping of hot gas from satellite galaxies
 * as they move through the hot halo of the central galaxy. The stripping
 * occurs gradually over STRIPPING_STEPS timesteps.
 *
 * @param   halos      Array of halos in FOF group
 * @param   central_idx Index of central galaxy
 * @param   sat_idx    Index of satellite galaxy being stripped
 * @param   redshift   Current redshift
 * @param   omega      Matter density parameter
 * @param   omega_lambda Dark energy density parameter
 * @param   hubble_h   Hubble parameter
 */
static void strip_from_satellite(struct Halo *halos, int central_idx,
                                  int sat_idx, double redshift, double omega,
                                  double omega_lambda, double hubble_h) {
  double reionization_modifier;
  double strippedGas, strippedGasMetals;
  float metallicity;

  /* Apply reionization modifier if enabled */
  if (REIONIZATION_ON) {
    reionization_modifier = do_reionization(
        halos[sat_idx].Mvir, redshift, omega, omega_lambda, hubble_h);
  } else {
    reionization_modifier = 1.0;
  }

  /* Calculate amount of gas to strip (gradual over STRIPPING_STEPS) */
  strippedGas = -1.0 *
                (reionization_modifier * BARYON_FRAC * halos[sat_idx].Mvir -
                 (halos[sat_idx].galaxy->StellarMass +
                  halos[sat_idx].galaxy->ColdGas +
                  halos[sat_idx].galaxy->HotGas +
                  halos[sat_idx].galaxy->EjectedMass +
                  halos[sat_idx].galaxy->ICS)) /
                STRIPPING_STEPS;

  /* Only proceed if there is positive stripping */
  if (strippedGas > 0.0) {
    /* Calculate metals in stripped gas */
    metallicity = mimic_get_metallicity(halos[sat_idx].galaxy->HotGas,
                                   halos[sat_idx].galaxy->MetalsHotGas);
    strippedGasMetals = strippedGas * metallicity;

    /* Limit stripping to available hot gas and metals */
    if (strippedGas > halos[sat_idx].galaxy->HotGas) {
      strippedGas = halos[sat_idx].galaxy->HotGas;
    }
    if (strippedGasMetals > halos[sat_idx].galaxy->MetalsHotGas) {
      strippedGasMetals = halos[sat_idx].galaxy->MetalsHotGas;
    }

    /* Remove gas and metals from satellite */
    halos[sat_idx].galaxy->HotGas -= (float)strippedGas;
    halos[sat_idx].galaxy->MetalsHotGas -= (float)strippedGasMetals;

    /* Add stripped gas and metals to central galaxy */
    halos[central_idx].galaxy->HotGas += (float)strippedGas;
    halos[central_idx].galaxy->MetalsHotGas += (float)(strippedGas * metallicity);
  }
}

/**
 * @brief   Add infalling gas to the hot gas component
 *
 * Handles addition/removal of gas to/from the hot gas reservoir.
 * For negative infall (mass loss):
 * 1. First remove from ejected gas reservoir
 * 2. Then remove from hot gas reservoir
 *
 * @param   galaxy       Pointer to galaxy data
 * @param   infallingGas Amount of gas to add (can be negative)
 */
static void add_infall_to_hot(struct GalaxyData *galaxy, double infallingGas) {
  float metallicity;

  /* Handle mass loss case (negative infall) */
  if (infallingGas < 0.0 && galaxy->EjectedMass > 0.0f) {
    /* First remove from ejected gas reservoir */
    metallicity = mimic_get_metallicity(galaxy->EjectedMass, galaxy->MetalsEjectedMass);

    /* Update ejected metals */
    galaxy->MetalsEjectedMass += (float)(infallingGas * metallicity);
    if (galaxy->MetalsEjectedMass < 0.0f) {
      galaxy->MetalsEjectedMass = 0.0f;
    }

    /* Update ejected gas mass */
    galaxy->EjectedMass += (float)infallingGas;

    /* If ejected reservoir is depleted, continue removing from hot gas */
    if (galaxy->EjectedMass < 0.0f) {
      infallingGas = galaxy->EjectedMass;
      galaxy->EjectedMass = 0.0f;
      galaxy->MetalsEjectedMass = 0.0f;
    } else {
      infallingGas = 0.0;
    }
  }

  /* If we still have mass loss, remove from hot gas metals */
  if (infallingGas < 0.0 && galaxy->MetalsHotGas > 0.0f) {
    metallicity = mimic_get_metallicity(galaxy->HotGas, galaxy->MetalsHotGas);

    galaxy->MetalsHotGas += (float)(infallingGas * metallicity);
    if (galaxy->MetalsHotGas < 0.0f) {
      galaxy->MetalsHotGas = 0.0f;
    }
  }

  /* Finally update the hot gas component */
  galaxy->HotGas += (float)infallingGas;

  /* Ensure non-negative values */
  if (galaxy->HotGas < 0.0f) {
    galaxy->HotGas = 0.0f;
    galaxy->MetalsHotGas = 0.0f;
  }
}

// ============================================================================
// MODULE LIFECYCLE FUNCTIONS
// ============================================================================

/**
 * @brief   Initialize sage_infall module
 *
 * Reads configuration parameters and calculates derived quantities.
 *
 * @return  0 on success, non-zero on failure
 */
static int sage_infall_init(void) {
  /* Read module parameters from configuration */
  module_get_double("SageInfall", "BaryonFrac", &BARYON_FRAC, 0.17);
  module_get_int("SageInfall", "ReionizationOn", &REIONIZATION_ON, 1);
  module_get_double("SageInfall", "Reionization_z0", &REIONIZATION_Z0, 8.0);
  module_get_double("SageInfall", "Reionization_zr", &REIONIZATION_ZR, 7.0);
  module_get_int("SageInfall", "StrippingSteps", &STRIPPING_STEPS, 10);

  /* Validate parameters */
  if (BARYON_FRAC < 0.0 || BARYON_FRAC > 1.0) {
    ERROR_LOG("SageInfall_BaryonFrac = %.3f is outside valid range [0.0, 1.0]",
              BARYON_FRAC);
    return -1;
  }

  if (REIONIZATION_Z0 < 0.0 || REIONIZATION_ZR < 0.0) {
    ERROR_LOG("Reionization redshifts must be positive");
    return -1;
  }

  if (STRIPPING_STEPS < 1) {
    ERROR_LOG("SageInfall_StrippingSteps must be >= 1");
    return -1;
  }

  /* Calculate derived reionization parameters */
  a0 = 1.0 / (1.0 + REIONIZATION_Z0);
  ar = 1.0 / (1.0 + REIONIZATION_ZR);

  /* Log module configuration */
  INFO_LOG("SAGE infall module initialized");
  INFO_LOG("  Physics: infallingMass = f_reion * BaryonFrac * Mvir - baryons");
  INFO_LOG("  BaryonFrac = %.4f (from config)", BARYON_FRAC);
  INFO_LOG("  ReionizationOn = %d (from config)", REIONIZATION_ON);
  if (REIONIZATION_ON) {
    INFO_LOG("  Reionization_z0 = %.2f (a0 = %.4f)", REIONIZATION_Z0, a0);
    INFO_LOG("  Reionization_zr = %.2f (ar = %.4f)", REIONIZATION_ZR, ar);
  }
  INFO_LOG("  StrippingSteps = %d (from config)", STRIPPING_STEPS);

  return 0;
}

/**
 * @brief   Process halos in a FOF group
 *
 * Implements infall and stripping for all halos in the FOF group.
 * Process order:
 * 1. Find central galaxy (Type == 0)
 * 2. Calculate infall for central
 * 3. Add infalling gas to central's hot reservoir
 * 4. Strip gas from all satellites
 *
 * @param   ctx     Module execution context
 * @param   halos   Array of halos in FOF group
 * @param   ngal    Number of halos
 * @return  0 on success, non-zero on failure
 */
static int sage_infall_process(struct ModuleContext *ctx, struct Halo *halos,
                                int ngal) {
  /* Validate inputs */
  if (halos == NULL || ngal <= 0) {
    return 0; /* Nothing to process */
  }

  /* Extract cosmological parameters from context */
  double z = ctx->redshift;
  double omega = ctx->params->Omega;
  double omega_lambda = ctx->params->OmegaLambda;
  double hubble_h = ctx->params->Hubble_h;

  /* Find central galaxy (Type == 0) */
  int central_idx = -1;
  for (int i = 0; i < ngal; i++) {
    if (halos[i].Type == 0) {
      central_idx = i;
      break;
    }
  }

  if (central_idx == -1) {
    DEBUG_LOG("No central galaxy found in FOF group (ngal=%d)", ngal);
    return 0; /* Not an error - can happen in some tree structures */
  }

  /* Validate central galaxy has data */
  if (halos[central_idx].galaxy == NULL) {
    ERROR_LOG("Central galaxy (index %d) has NULL galaxy data", central_idx);
    return -1;
  }

  /* Calculate infall for central galaxy */
  double infallingMass =
      infall_recipe(halos, ngal, central_idx, z, omega, omega_lambda, hubble_h);

  /* Add infalling gas to central's hot reservoir */
  add_infall_to_hot(halos[central_idx].galaxy, infallingMass);

  /* Strip gas from satellites */
  for (int i = 0; i < ngal; i++) {
    if (i == central_idx)
      continue; /* Skip central */
    if (halos[i].Type != 1)
      continue; /* Only process satellites (Type 1) */
    if (halos[i].galaxy == NULL)
      continue; /* Skip if no galaxy data */

    strip_from_satellite(halos, central_idx, i, z, omega, omega_lambda,
                         hubble_h);
  }

  /* Debug logging */
  DEBUG_LOG("Infall: central Mvir=%.3e, infall=%.3e, HotGas=%.3e, z=%.3f",
            halos[central_idx].Mvir, infallingMass,
            halos[central_idx].galaxy->HotGas, z);

  return 0;
}

/**
 * @brief   Cleanup sage_infall module
 *
 * No allocated resources to free for this module.
 *
 * @return  0 on success
 */
static int sage_infall_cleanup(void) {
  INFO_LOG("SAGE infall module cleaned up");
  return 0;
}

// ============================================================================
// MODULE REGISTRATION
// ============================================================================

/**
 * @brief   Module structure for sage_infall module
 */
static struct Module sage_infall_module = {
    .name = "sage_infall",
    .init = sage_infall_init,
    .process_halos = sage_infall_process,
    .cleanup = sage_infall_cleanup};

/**
 * @brief   Register the sage_infall module
 */
void sage_infall_register(void) { module_registry_add(&sage_infall_module); }
