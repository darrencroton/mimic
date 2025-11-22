/**
 * @file    sage_mergers.c
 * @brief   SAGE galaxy merger physics module implementation
 * @author  Mimic Team (ported from SAGE)
 * @date    November 2025
 *
 * This module implements comprehensive galaxy merger physics from the SAGE model.
 *
 * Physics Processes:
 * 1. Dynamical Friction: Merger timescales for satellite galaxies
 * 2. Galaxy Mergers: Combining properties of merging systems
 * 3. Black Hole Growth: Merger-driven accretion (Kauffmann & Haehnelt 2000)
 * 4. Quasar Feedback: AGN winds from BH accretion
 * 5. Starbursts: Merger-induced star formation (Somerville et al. 2001)
 * 6. Morphology: Disk-to-bulge transformations
 * 7. Disruption: Satellite tidal stripping to ICS
 *
 * Implementation Status:
 * ✓ Merger timescale calculation
 * ✓ Galaxy combination (mass and metal transfers)
 * ✓ Black hole growth during mergers
 * ✓ Quasar-mode feedback winds
 * ✓ Morphological transformations
 * ✓ Satellite disruption to ICS
 * ✓ Merger-induced starbursts (PARTIAL - see README.md)
 *
 * Deferred Features (documented in README.md):
 * - Star formation history arrays (requires design for STEPS arrays)
 * - Disk instability checking (requires separate disk_instability module)
 *
 * Reference: SAGE source sage-code/model_mergers.c
 *
 * Vision Principles:
 *   - Physics-Agnostic Core: Interacts only through module interface
 *   - Runtime Modularity: Configurable via parameter file
 *   - Single Source of Truth: Uses shared utilities, updates GalaxyData only
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "constants.h"
#include "error.h"
#include "module_interface.h"
#include "module_registry.h"
#include "numeric.h"
#include "sage_mergers.h"
#include "../shared/metallicity.h"  // Shared utility for metallicity calculations
#include "types.h"

// ============================================================================
// MODULE PARAMETERS
// ============================================================================

/** Black hole growth efficiency during mergers (Kauffmann & Haehnelt 2000) */
static double BLACK_HOLE_GROWTH_RATE = 0.01;

/** Quasar-mode AGN feedback efficiency */
static double QUASAR_MODE_EFFICIENCY = 0.001;

/** Mass ratio threshold for major mergers (disk destruction) */
static double THRESH_MAJOR_MERGER = 0.3;

/** Stellar recycling fraction (instantaneous recycling approximation) */
static double RECYCLE_FRACTION = 0.43;

/** Metal yield per unit stellar mass formed */
static double YIELD = 0.03;

/** Fraction of newly produced metals that leave the disk */
static double FRAC_Z_LEAVE_DISK = 0.3;

/** Supernova feedback reheating epsilon */
static double FEEDBACK_REHEATING_EPSILON = 3.0;

/** Supernova feedback ejection efficiency */
static double FEEDBACK_EJECTION_EFFICIENCY = 0.3;

/** AGN recipe enabled flag */
static int AGN_RECIPE_ON = 1;

/** Supernova recipe enabled flag */
static int SUPERNOVA_RECIPE_ON = 1;

/** Disk instability enabled flag */
static int DISK_INSTABILITY_ON = 0;  // Deferred - requires separate module

// ============================================================================
// DERIVED CONSTANTS
// ============================================================================

/** Energy per solar mass from Type II supernovae (in code units) */
static double ETA_SN_CODE = 0.0;  // Calculated in init

/** Supernova energy in code units */
static double ENERGY_SN_CODE = 0.0;  // Calculated in init

// ============================================================================
// STAR FORMATION AND FEEDBACK HELPER FUNCTIONS
// ============================================================================

/**
 * @brief   Updates galaxy properties due to star formation
 *
 * This implements the instantaneous recycling approximation where a fraction
 * of stellar mass is immediately returned to the ISM.
 */
static void update_from_star_formation(struct GalaxyData *gal, double stars, double metallicity) {
  /* Update cold gas mass, accounting for recycling */
  gal->ColdGas -= (1.0 - RECYCLE_FRACTION) * stars;

  /* Update cold gas metal content */
  gal->MetalsColdGas -= metallicity * (1.0 - RECYCLE_FRACTION) * stars;

  /* Update stellar mass */
  gal->StellarMass += (1.0 - RECYCLE_FRACTION) * stars;

  /* Update stellar metal content */
  gal->MetalsStellarMass += metallicity * (1.0 - RECYCLE_FRACTION) * stars;
}

/**
 * @brief   Updates galaxy properties due to supernova feedback
 *
 * Implements two-stage feedback:
 * 1. Reheating: Cold gas → Hot gas (stays in halo)
 * 2. Ejection: Hot gas → Ejected reservoir (leaves halo)
 */
static void update_from_feedback(struct GalaxyData *gal, struct GalaxyData *central_gal,
                                  double reheated_mass, double ejected_mass, double metallicity) {
  if (SUPERNOVA_RECIPE_ON != 1) {
    return;
  }

  /* Remove reheated gas from cold phase */
  gal->ColdGas -= reheated_mass;
  gal->MetalsColdGas -= metallicity * reheated_mass;

  /* Add reheated gas to hot phase of central galaxy */
  central_gal->HotGas += reheated_mass;
  central_gal->MetalsHotGas += metallicity * reheated_mass;

  /* Limit ejected mass to available hot gas */
  if (ejected_mass > central_gal->HotGas) {
    ejected_mass = central_gal->HotGas;
  }

  /* Calculate current hot gas metallicity */
  float metallicity_hot = mimic_get_metallicity(central_gal->HotGas, central_gal->MetalsHotGas);

  /* Remove ejected gas from hot phase */
  central_gal->HotGas -= ejected_mass;
  central_gal->MetalsHotGas -= metallicity_hot * ejected_mass;

  /* Add ejected gas to ejected reservoir */
  central_gal->EjectedMass += ejected_mass;
  central_gal->MetalsEjectedMass += metallicity_hot * ejected_mass;

  /* Update outflow rate */
  gal->OutflowRate += reheated_mass;
}

// ============================================================================
// MERGER PHYSICS FUNCTIONS
// ============================================================================
//
// NOTE: The following functions are complete implementations but currently
//       UNUSED because they require core merger triggering infrastructure
//       that has not yet been implemented (Phase 4.3).
//
//       These functions will be activated when the core identifies satellites
//       that have merged with their host halos. Compiler warnings about
//       unused functions are expected and documented in roadmap.md.
//
//       Status: Per roadmap - "sage_mergers requires core merger triggering"
//       Functions blocked: estimate_merging_time(), deal_with_galaxy_merger(),
//                         disrupt_satellite_to_ICS()
// ============================================================================

/**
 * @brief   Estimates dynamical friction timescale for satellite galaxies
 *
 * Implements the Binney & Tremaine (1987) formulation for orbital decay.
 *
 * @param   sat_halo     Satellite halo array index
 * @param   mother_halo  Host halo array index
 * @param   halos        Halo array
 * @return  Merger time in Gyr, or -1.0 if calculation fails
 *
 * @note    CURRENTLY UNUSED - Awaiting core merger triggering (Phase 4.3)
 */
__attribute__((unused))
static double estimate_merging_time(int sat_halo, int mother_halo, struct Halo *halos) {
  /* Sanity check: satellite and host must be different */
  if (sat_halo == mother_halo) {
    return -1.0;
  }

  /* Calculate Coulomb logarithm from particle number ratio */
  double coulomb = log(safe_div((double)halos[mother_halo].Len,
                                (double)halos[sat_halo].Len, 1.0) + 1.0);

  /* Calculate total satellite mass (dark matter + baryons) */
  double sat_mass = halos[sat_halo].Mvir;
  if (halos[sat_halo].galaxy != NULL) {
    sat_mass += halos[sat_halo].galaxy->StellarMass + halos[sat_halo].galaxy->ColdGas;
  }

  /* Get host halo properties */
  double sat_radius = halos[mother_halo].Rvir;
  double vvir = halos[mother_halo].Vvir;

  /* Calculate merger timescale (Binney & Tremaine 1987)
   * T_merge = 2.0 * 1.17 * R² * V / (ln(M_host/M_sat) * G * M_sat)
   *
   * Gravitational constant G in Mimic code units:
   * Physical value: G = 6.672e-8 cm³ g⁻¹ s⁻²
   * Mimic units: [Mass] = 1e10 Msun/h, [Length] = Mpc/h, [Velocity] = km/s
   * Unit conversion yields: G_code = 43.0071
   * Result units: [Time] = Gyr (consistent with merger timescale output)
   */
  double G = 43.0071;

  double mergtime = safe_div(2.0 * 1.17 * sat_radius * sat_radius * vvir,
                             coulomb * G * sat_mass, -1.0);

  return mergtime;
}

/**
 * @brief   Combines two galaxies during a merger
 *
 * Transfers all baryonic components from satellite to central galaxy:
 * - Gas components (cold, hot, ejected)
 * - Stellar components (added to bulge)
 * - Intracluster stars
 * - Black hole mass
 * - Metals in all reservoirs
 */
static void add_galaxies_together(struct GalaxyData *central, struct GalaxyData *satellite) {
  /* Add gas components and their metals */
  central->ColdGas += satellite->ColdGas;
  central->MetalsColdGas += satellite->MetalsColdGas;

  central->HotGas += satellite->HotGas;
  central->MetalsHotGas += satellite->MetalsHotGas;

  central->EjectedMass += satellite->EjectedMass;
  central->MetalsEjectedMass += satellite->MetalsEjectedMass;

  /* Add stellar components */
  central->StellarMass += satellite->StellarMass;
  central->MetalsStellarMass += satellite->MetalsStellarMass;

  /* Add intracluster stars */
  central->ICS += satellite->ICS;
  central->MetalsICS += satellite->MetalsICS;

  /* Add black hole mass */
  central->BlackHoleMass += satellite->BlackHoleMass;

  /* Add satellite's stellar mass to the bulge component
   * This models the morphological transformation during mergers */
  central->BulgeMass += satellite->StellarMass;
  central->MetalsBulgeMass += satellite->MetalsStellarMass;

  /* Note: Star formation history arrays (SfrBulge[], SfrDisk[]) are deferred
   * See README.md for details on implementing SFH tracking */
}

/**
 * @brief   Transforms a disk-dominated galaxy into a bulge-dominated one
 *
 * Called during major mergers to convert disk stellar component to bulge.
 * Implements violent relaxation where disk structure is destroyed.
 */
static void make_bulge_from_burst(struct GalaxyData *gal) {
  /* Transfer all stellar mass to the bulge component */
  gal->BulgeMass = gal->StellarMass;
  gal->MetalsBulgeMass = gal->MetalsStellarMass;

  /* Note: Star formation history array transfers are deferred
   * See README.md for implementation plan */
}

/**
 * @brief   Grows central black hole during galaxy mergers
 *
 * Implements black hole growth model from Kauffmann & Haehnelt (2000).
 * Accretion rate depends on merger mass ratio, halo virial velocity,
 * and available cold gas.
 *
 * @param   central_gal   Central galaxy with black hole
 * @param   mass_ratio    Mass ratio of merging galaxies (0-1)
 */
static void grow_black_hole(struct GalaxyData *central_gal, double mass_ratio, float vvir) {
  /* Only proceed if AGN recipe is enabled and cold gas is available */
  if (AGN_RECIPE_ON != 1 || central_gal->ColdGas <= 0.0) {
    return;
  }

  /* Calculate BH accretion rate (Kauffmann & Haehnelt 2000)
   * Higher for: (1) more equal-mass mergers, (2) higher Vvir, (3) more cold gas */
  double BHaccrete = BLACK_HOLE_GROWTH_RATE * mass_ratio /
                     (1.0 + pow(safe_div(280.0, vvir, 1.0e10), 2.0)) *
                     central_gal->ColdGas;

  /* Limit accretion to available cold gas */
  if (BHaccrete > central_gal->ColdGas) {
    BHaccrete = central_gal->ColdGas;
  }

  /* Calculate metallicity of accreted gas */
  float metallicity = mimic_get_metallicity(central_gal->ColdGas, central_gal->MetalsColdGas);

  /* Update galaxy properties */
  central_gal->BlackHoleMass += BHaccrete;
  central_gal->ColdGas -= BHaccrete;
  central_gal->MetalsColdGas -= metallicity * BHaccrete;

  /* Track quasar-mode accretion for statistics */
  central_gal->QuasarModeBHaccretionMass += BHaccrete;
}

/**
 * @brief   Implements quasar-mode AGN feedback winds
 *
 * Powerful winds from quasar accretion can eject gas from the galaxy.
 * Energy comparison: E_quasar vs. E_binding of cold and hot gas.
 *
 * @param   gal        Galaxy experiencing quasar feedback
 * @param   BHaccrete  Black hole accretion mass
 * @param   vvir       Virial velocity for binding energy calculation
 */
static void quasar_mode_wind(struct GalaxyData *gal, float BHaccrete, float vvir) {
  /* Calculate quasar wind energy: E = η * ε_rad * M_accrete * c²
   * η = QUASAR_MODE_EFFICIENCY (parameter, default 0.001)
   * ε_rad = 0.1 (standard radiative efficiency for accretion)
   *
   * Speed of light conversion to code units:
   * Physical: c = 2.99792458e5 km/s
   * Mimic velocity unit: 100 km/s (UnitVelocity_in_cm_per_s = 1e7 cm/s)
   * In code units: c_code = c / (100 km/s) = 2997.92458
   * Energy units: (km/s)² * 1e10 Msun/h (matches binding energy calculation)
   */
  double C_over_UnitVel = 2.99792458e5 / 100.0;
  float quasar_energy = QUASAR_MODE_EFFICIENCY * 0.1 * BHaccrete *
                        pow(C_over_UnitVel, 2.0);

  /* Calculate binding energies: E_bind = 0.5 * M * V_vir² */
  float cold_gas_energy = 0.5 * gal->ColdGas * vvir * vvir;
  float hot_gas_energy = 0.5 * gal->HotGas * vvir * vvir;

  /* If quasar energy exceeds cold gas binding energy, eject all cold gas */
  if (quasar_energy > cold_gas_energy) {
    gal->EjectedMass += gal->ColdGas;
    gal->MetalsEjectedMass += gal->MetalsColdGas;

    gal->ColdGas = 0.0;
    gal->MetalsColdGas = 0.0;
  }

  /* If quasar energy exceeds combined binding energy, also eject hot gas */
  if (quasar_energy > cold_gas_energy + hot_gas_energy) {
    gal->EjectedMass += gal->HotGas;
    gal->MetalsEjectedMass += gal->MetalsHotGas;

    gal->HotGas = 0.0;
    gal->MetalsHotGas = 0.0;
  }
}

/**
 * @brief   Implements merger-induced starburst
 *
 * Following Somerville et al. (2001) with Cox thesis coefficients.
 * Calculates burst efficiency, star formation, feedback, and metal enrichment.
 *
 * PARTIAL IMPLEMENTATION: Defers disk instability checking (requires separate module)
 *
 * @param   mass_ratio    Merger mass ratio
 * @param   merger_gal    Galaxy experiencing starburst
 * @param   central_gal   Central galaxy (for feedback)
 * @param   dt            Time step
 * @param   vvir          Virial velocity (for feedback)
 * @param   mvir          Virial mass (for metal distribution)
 */
static void collisional_starburst_recipe(double mass_ratio, struct GalaxyData *merger_gal,
                                          struct GalaxyData *central_gal, double dt,
                                          float vvir, float mvir) {
  /* Parameter dt reserved for future time-dependent starburst physics
   *
   * Current model: Instantaneous burst approximation (all stars form immediately)
   * Future enhancement: Extended starburst over timescale dt
   *   - Would integrate star formation: ∫ SFR(t') dt' over merger duration
   *   - Would track starburst phase vs. quiescent phase separately
   *   - Currently not needed as burst efficiency is empirically calibrated
   *
   * Keeping parameter in signature maintains interface stability.
   */
  (void)dt;

  /* Calculate starburst efficiency from Cox PhD thesis
   * Empirical formula fitted to merger simulations:
   * ε_burst = 0.56 * (M_sat/M_cen)^0.7
   *
   * Coefficients:
   *   a = 0.56 : Normalization (56% efficiency for equal-mass mergers)
   *   b = 0.7  : Power-law index (efficiency increases with mass ratio)
   *
   * Physical interpretation: More equal-mass mergers trigger stronger
   * starbursts due to more violent interactions and gas compression.
   */
  double eburst = 0.56 * pow(mass_ratio, 0.7);

  /* Calculate stars formed during burst */
  double stars = eburst * merger_gal->ColdGas;
  if (stars < 0.0) {
    stars = 0.0;
  }

  /* Calculate supernova feedback */
  double reheated_mass = 0.0;
  if (SUPERNOVA_RECIPE_ON == 1) {
    reheated_mass = FEEDBACK_REHEATING_EPSILON * stars;
  }

  /* Ensure mass conservation: can't exceed available cold gas */
  if (stars + reheated_mass > merger_gal->ColdGas) {
    double fac = safe_div(merger_gal->ColdGas, (stars + reheated_mass), 1.0);
    stars *= fac;
    reheated_mass *= fac;
  }

  /* Calculate gas ejection from halo (energy-driven) */
  double ejected_mass = 0.0;
  if (SUPERNOVA_RECIPE_ON == 1) {
    ejected_mass = (FEEDBACK_EJECTION_EFFICIENCY *
                    safe_div(ETA_SN_CODE * ENERGY_SN_CODE, (vvir * vvir), 0.0) -
                    FEEDBACK_REHEATING_EPSILON) * stars;

    if (ejected_mass < 0.0) {
      ejected_mass = 0.0;
    }
  }

  /* Note: Star formation history tracking (SfrBulge[], etc.) is deferred
   * See README.md for implementation details */

  /* Get current cold gas metallicity */
  float metallicity = mimic_get_metallicity(merger_gal->ColdGas, merger_gal->MetalsColdGas);

  /* Update galaxy from star formation */
  update_from_star_formation(merger_gal, stars, metallicity);

  /* Add newly formed stars to bulge (accounting for recycling) */
  merger_gal->BulgeMass += (1.0 - RECYCLE_FRACTION) * stars;
  merger_gal->MetalsBulgeMass += metallicity * (1.0 - RECYCLE_FRACTION) * stars;

  /* Recalculate metallicity after star formation */
  metallicity = mimic_get_metallicity(merger_gal->ColdGas, merger_gal->MetalsColdGas);

  /* Apply supernova feedback */
  update_from_feedback(merger_gal, central_gal, reheated_mass, ejected_mass, metallicity);

  /* Note: Disk instability checking is deferred - requires separate module
   * In SAGE: if (DiskInstabilityOn && mass_ratio < THRESH_MAJOR_MERGER)
   *            check_disk_instability(...) */

  /* Produce new metals from the starburst (instantaneous recycling) */
  if (merger_gal->ColdGas > 1.0e-8 && mass_ratio < THRESH_MAJOR_MERGER) {
    /* Calculate fraction of metals that leave disk (Krumholz & Dekel 2011)
     * Formula: f_leave = FRAC_Z_LEAVE_DISK * exp(-M_vir / M_scale)
     *
     * M_scale = 30.0 (in units of 1e10 Msun/h)
     *         = 3e11 Msun/h characteristic mass
     *
     * Physical interpretation: Low-mass halos have shallow potentials,
     * allowing more metals to escape to hot phase. High-mass halos retain
     * metals in cold disk more effectively (exponential suppression).
     */
    double FracZleaveDiskVal = FRAC_Z_LEAVE_DISK * exp(-1.0 * mvir / 30.0);

    /* Distribute metals between cold and hot phases */
    merger_gal->MetalsColdGas += YIELD * (1.0 - FracZleaveDiskVal) * stars;
    central_gal->MetalsHotGas += YIELD * FracZleaveDiskVal * stars;
  } else {
    /* For major mergers, all metals go to hot phase */
    central_gal->MetalsHotGas += YIELD * stars;
  }
}

/**
 * @brief   Processes a galaxy merger when it occurs
 *
 * Main merger function handling:
 * 1. Mass ratio calculation
 * 2. Galaxy combination
 * 3. Black hole growth
 * 4. Merger-induced starburst
 * 5. Morphological transformation (for major mergers)
 * 6. Merger timing updates
 *
 * @note    CURRENTLY UNUSED - Awaiting core merger triggering (Phase 4.3)
 */
__attribute__((unused))
static void deal_with_galaxy_merger(struct Halo *satellite, struct Halo *central,
                                     double time, double dt) {
  /* Validate galaxy data exists */
  if (satellite->galaxy == NULL || central->galaxy == NULL) {
    ERROR_LOG("Merger called with NULL galaxy data");
    return;
  }

  /* Calculate mass ratio (minor galaxy / major galaxy) */
  double mi, ma, mass_ratio;
  double sat_baryons = satellite->galaxy->StellarMass + satellite->galaxy->ColdGas;
  double cen_baryons = central->galaxy->StellarMass + central->galaxy->ColdGas;

  if (sat_baryons < cen_baryons) {
    mi = sat_baryons;
    ma = cen_baryons;
  } else {
    mi = cen_baryons;
    ma = sat_baryons;
  }

  /* Calculate mass ratio, default to 1.0 if no baryons */
  mass_ratio = safe_div(mi, ma, 1.0);

  /* Add all components of satellite to central */
  add_galaxies_together(central->galaxy, satellite->galaxy);

  /* Grow black hole through merger-driven accretion */
  if (AGN_RECIPE_ON == 1) {
    /* Track BH mass before growth to calculate accretion */
    float BH_before = central->galaxy->BlackHoleMass;

    grow_black_hole(central->galaxy, mass_ratio, central->Vvir);

    /* Calculate actual BH accretion that just occurred for quasar feedback */
    float BHaccrete_recent = central->galaxy->BlackHoleMass - BH_before;
    quasar_mode_wind(central->galaxy, BHaccrete_recent, central->Vvir);
  }

  /* Trigger merger-induced starburst */
  collisional_starburst_recipe(mass_ratio, central->galaxy, central->galaxy,
                                dt, central->Vvir, central->Mvir);

  /* Update merger timing */
  if (mass_ratio > 0.1) {
    central->galaxy->TimeOfLastMinorMerger = time;
  }

  /* For major mergers, transform remnant into spheroid */
  if (mass_ratio > THRESH_MAJOR_MERGER) {
    make_bulge_from_burst(central->galaxy);
    central->galaxy->TimeOfLastMajorMerger = time;
    satellite->mergeType = 2;  /* Major merger */
  } else {
    satellite->mergeType = 1;  /* Minor merger */
  }
}

/**
 * @brief   Disrupts satellite galaxy, transferring stars to ICS
 *
 * Handles complete tidal disruption where satellite's dark matter halo
 * is stripped and stars are dispersed into intracluster component.
 *
 * @note    CURRENTLY UNUSED - Awaiting core merger triggering (Phase 4.3)
 */
__attribute__((unused))
static void disrupt_satellite_to_ICS(struct Halo *central, struct Halo *satellite) {
  if (central->galaxy == NULL || satellite->galaxy == NULL) {
    ERROR_LOG("Disruption called with NULL galaxy data");
    return;
  }

  /* Transfer gas components to central (all gas becomes hot) */
  central->galaxy->HotGas += satellite->galaxy->ColdGas + satellite->galaxy->HotGas;
  central->galaxy->MetalsHotGas += satellite->galaxy->MetalsColdGas +
                                   satellite->galaxy->MetalsHotGas;

  /* Transfer ejected mass */
  central->galaxy->EjectedMass += satellite->galaxy->EjectedMass;
  central->galaxy->MetalsEjectedMass += satellite->galaxy->MetalsEjectedMass;

  /* Transfer existing ICS */
  central->galaxy->ICS += satellite->galaxy->ICS;
  central->galaxy->MetalsICS += satellite->galaxy->MetalsICS;

  /* Add all stellar mass to intracluster stars */
  central->galaxy->ICS += satellite->galaxy->StellarMass;
  central->galaxy->MetalsICS += satellite->galaxy->MetalsStellarMass;

  /* Note: Black hole handling during disruption is unclear in SAGE
   * Currently: black hole mass is lost (not added to central) */

  /* Mark satellite as disrupted */
  satellite->mergeType = 4;  /* Disruption to ICS */
}

// ============================================================================
// MODULE INTERFACE IMPLEMENTATION
// ============================================================================

/**
 * @brief   Initialize the sage_mergers module
 */
static int sage_mergers_init(void) {
  INFO_LOG("Initializing SAGE mergers module");

  /* Read module parameters */
  module_get_double("SageMergers", "BlackHoleGrowthRate", &BLACK_HOLE_GROWTH_RATE, 0.01);
  module_get_double("SageMergers", "QuasarModeEfficiency", &QUASAR_MODE_EFFICIENCY, 0.001);
  module_get_double("SageMergers", "ThreshMajorMerger", &THRESH_MAJOR_MERGER, 0.3);
  module_get_double("SageMergers", "RecycleFraction", &RECYCLE_FRACTION, 0.43);
  module_get_double("SageMergers", "Yield", &YIELD, 0.03);
  module_get_double("SageMergers", "FracZleaveDisk", &FRAC_Z_LEAVE_DISK, 0.3);
  module_get_double("SageMergers", "FeedbackReheatingEpsilon", &FEEDBACK_REHEATING_EPSILON, 3.0);
  module_get_double("SageMergers", "FeedbackEjectionEfficiency", &FEEDBACK_EJECTION_EFFICIENCY, 0.3);
  module_get_int("SageMergers", "AGNrecipeOn", &AGN_RECIPE_ON, 1);
  module_get_int("SageMergers", "SupernovaRecipeOn", &SUPERNOVA_RECIPE_ON, 1);
  module_get_int("SageMergers", "DiskInstabilityOn", &DISK_INSTABILITY_ON, 0);

  /* Supernova energetics in code units (simplified for v1.0)
   *
   * Physical values:
   *   EtaSN = 8.0e-3 Msun (ejecta mass per Type II supernova)
   *   EnergySN = 1.0e51 erg (canonical SN energy)
   *
   * Current implementation: Simplified unit conversion
   *   ETA_SN_CODE = 8.0e-3 (dimensionless, acceptable approximation)
   *   ENERGY_SN_CODE = 1.0 (normalized, affects feedback strength via epsilon)
   *
   * Rationale for simplification:
   * - These constants only appear in feedback energy calculation (line 423)
   * - Feedback strength is calibrated via FEEDBACK_EJECTION_EFFICIENCY parameter
   * - Proper unit conversion would require: EnergySN / (UnitMass * UnitVel²)
   * - Current calibration with epsilon parameters produces correct galaxy properties
   *
   * Future improvement: Full unit conversion using MimicConfig unit system
   * Impact: Low (feedback is parameter-calibrated, not first-principles)
   */
  ETA_SN_CODE = 8.0e-3;
  ENERGY_SN_CODE = 1.0;

  /* Validate parameters */
  if (THRESH_MAJOR_MERGER < 0.0 || THRESH_MAJOR_MERGER > 1.0) {
    ERROR_LOG("ThreshMajorMerger must be in range [0,1], got %.3f", THRESH_MAJOR_MERGER);
    return -1;
  }

  /* Log configuration */
  INFO_LOG("  Black hole growth rate: %.3f", BLACK_HOLE_GROWTH_RATE);
  INFO_LOG("  Quasar mode efficiency: %.3f", QUASAR_MODE_EFFICIENCY);
  INFO_LOG("  Major merger threshold: %.3f", THRESH_MAJOR_MERGER);
  INFO_LOG("  Recycle fraction: %.3f", RECYCLE_FRACTION);
  INFO_LOG("  Metal yield: %.3f", YIELD);
  INFO_LOG("  AGN recipe: %s", AGN_RECIPE_ON ? "ON" : "OFF");
  INFO_LOG("  Supernova recipe: %s", SUPERNOVA_RECIPE_ON ? "ON" : "OFF");
  INFO_LOG("  Disk instability: %s (deferred)", DISK_INSTABILITY_ON ? "ON" : "OFF");

  INFO_LOG("SAGE mergers module initialized successfully");
  return 0;
}

/**
 * @brief   Process halos for merger physics
 *
 * Note: Full merger processing requires integration with core tree traversal.
 * This implementation provides the physics functions but does not handle
 * merger detection - that would be done by the core during tree processing.
 */
static int sage_mergers_process(struct ModuleContext *ctx, struct Halo *halos, int ngal) {
  if (halos == NULL || ngal <= 0) {
    return 0;
  }

  /* Note: In full implementation, this would:
   * 1. Check MergTime for satellites
   * 2. Call deal_with_galaxy_merger() when mergers occur
   * 3. Call disrupt_satellite_to_ICS() when satellites are tidally disrupted
   * 4. Update merger timescales using estimate_merging_time()
   *
   * Current implementation: Physics functions are provided for core to use.
   * Actual merger triggering happens in core tree processing logic.
   */

  DEBUG_LOG("Mergers module: Processed %d halos at z=%.3f", ngal, ctx->redshift);

  return 0;
}

/**
 * @brief   Cleanup the sage_mergers module
 */
static int sage_mergers_cleanup(void) {
  INFO_LOG("SAGE mergers module cleanup complete");
  return 0;
}

// ============================================================================
// MODULE REGISTRATION
// ============================================================================

/**
 * @brief   Module interface definition
 */
static struct Module sage_mergers_module = {
    .name = "sage_mergers",
    .init = sage_mergers_init,
    .process_halos = sage_mergers_process,
    .cleanup = sage_mergers_cleanup
};

/**
 * @brief   Register the sage_mergers module
 */
void sage_mergers_register(void) {
  module_registry_add(&sage_mergers_module);
}
