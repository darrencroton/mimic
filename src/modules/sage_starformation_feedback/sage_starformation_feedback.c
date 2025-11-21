/**
 * @file    sage_starformation_feedback.c
 * @brief   SAGE star formation and feedback module implementation
 *
 * This module implements star formation and supernova feedback from the SAGE model.
 * It handles:
 * - Star formation following the Kennicutt-Schmidt law with a critical gas threshold
 * - Supernova feedback (reheating of cold gas to hot phase)
 * - Energy-driven gas ejection from the halo
 * - Metal enrichment from newly formed stars
 * - Stellar mass growth and disk structure evolution
 *
 * Physics:
 *   Star Formation:
 *     - SFR = ε_SF * (M_cold - M_crit) / τ_dyn
 *     - M_crit = 0.19 * V_vir * r_eff (Kauffmann 1996)
 *     - τ_dyn = r_eff / V_vir
 *
 *   Feedback:
 *     - Reheating: m_reheat = ε_reheat * Δm_*
 *     - Ejection: m_eject = (η * ε_eject * E_SN / V_vir² - ε_reheat) * Δm_*
 *
 *   Metal Enrichment:
 *     - Instantaneous recycling approximation
 *     - Yield: new metals from stars
 *     - FracZleaveDisk: mass-dependent (Krumholz & Dekel 2011)
 *
 * Implementation Notes:
 * - Central galaxies undergo star formation and feedback
 * - Satellites also form stars but feedback goes to central halo
 * - Disk instability check is deferred to sage_disk_instability module
 * - All baryonic components tracked for mass conservation
 *
 * Reference:
 *   - Croton et al. (2016) - SAGE model description
 *   - Kennicutt (1998) - Star formation law
 *   - Kauffmann (1996) - Critical gas density threshold
 *   - Krumholz & Dekel (2011) - Metal distribution model
 *   - SAGE: sage-code/model_starformation_and_feedback.c
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
#include "../shared/disk_radius.h"   // Shared utility for disk radius calculations
#include "../shared/metallicity.h"   // Shared utility for metallicity calculations
#include "module_interface.h"
#include "module_registry.h"
#include "numeric.h"
#include "sage_starformation_feedback.h"
#include "types.h"
#include "globals.h"  // For access to InputTreeHalos

// ============================================================================
// MODULE PARAMETERS
// ============================================================================

/**
 * @brief   Star formation prescription selector
 *
 * Selects which star formation recipe to use:
 *   0 = Kennicutt-Schmidt law with critical threshold (only option currently)
 *
 * Configuration: SageStarformationFeedback_SFprescription
 */
static int SF_PRESCRIPTION = 0;

/**
 * @brief   Star formation efficiency
 *
 * Efficiency parameter ε_SF in the star formation rate formula:
 *   SFR = ε_SF * (M_cold - M_crit) / τ_dyn
 *
 * Typical value: 0.01 - 0.05
 *
 * Configuration: SageStarformationFeedback_SfrEfficiency
 */
static double SFR_EFFICIENCY = 0.02;

/**
 * @brief   Enable supernova feedback
 *
 * If enabled (1), implements supernova-driven reheating and ejection.
 * If disabled (0), no feedback occurs (pure star formation).
 *
 * Configuration: SageStarformationFeedback_SupernovaRecipeOn
 */
static int SUPERNOVA_RECIPE_ON = 1;

/**
 * @brief   Feedback reheating efficiency
 *
 * Efficiency parameter ε_reheat for cold gas reheating:
 *   m_reheat = ε_reheat * Δm_*
 *
 * Typical value: 2.0 - 5.0
 *
 * Configuration: SageStarformationFeedback_FeedbackReheatingEpsilon
 */
static double FEEDBACK_REHEATING_EPSILON = 3.0;

/**
 * @brief   Feedback ejection efficiency
 *
 * Efficiency parameter ε_eject for gas ejection from halo:
 *   m_eject = (η * ε_eject * E_SN / V_vir² - ε_reheat) * Δm_*
 *
 * Typical value: 0.1 - 0.5
 *
 * Configuration: SageStarformationFeedback_FeedbackEjectionEfficiency
 */
static double FEEDBACK_EJECTION_EFFICIENCY = 0.3;

/**
 * @brief   Energy per supernova in code units
 *
 * Supernova energy E_SN in simulation code units.
 * Typical value: ~1e51 erg converted to code units
 *
 * In SAGE, this is derived from EnergySN parameter with unit conversions.
 * For Mimic, we use the code-unit value directly.
 *
 * Typical code-unit value: ~1.0 (depends on unit system)
 *
 * Configuration: SageStarformationFeedback_EnergySNcode
 */
static double ENERGY_SN_CODE = 1.0;

/**
 * @brief   Supernova efficiency in code units
 *
 * Number of supernovae per unit stellar mass formed (η) in code units.
 * Typical value: ~0.001-0.01 SNe per solar mass
 *
 * In SAGE, this is derived from EtaSN parameter with unit conversions.
 * For Mimic, we use the code-unit value directly.
 *
 * Typical code-unit value: ~0.5 (depends on unit system)
 *
 * Configuration: SageStarformationFeedback_EtaSNcode
 */
static double ETA_SN_CODE = 0.5;

/**
 * @brief   Stellar recycling fraction
 *
 * Fraction of stellar mass immediately returned to ISM through stellar winds
 * and supernovae (instantaneous recycling approximation).
 *
 * Typical value: 0.43 (Chabrier IMF)
 *
 * Configuration: SageStarformationFeedback_RecycleFraction
 */
static double RECYCLE_FRACTION = 0.43;

/**
 * @brief   Metal yield from star formation
 *
 * Yield of new metals produced per unit stellar mass formed.
 * Represents metal production from Type II supernovae.
 *
 * Typical value: 0.02 - 0.04
 *
 * Configuration: SageStarformationFeedback_Yield
 */
static double YIELD = 0.03;

/**
 * @brief   Normalization for metals leaving disk
 *
 * Normalization factor for the mass-dependent fraction of newly produced
 * metals that bypass the disk and go directly to the hot halo gas.
 * Follows Krumholz & Dekel (2011) Eq. 22.
 *
 * Actual fraction: FracZleaveDisk * exp(-Mvir / 30)
 *
 * Typical value: 0.2 - 0.5
 *
 * Configuration: SageStarformationFeedback_FracZleaveDisk
 */
static double FRAC_Z_LEAVE_DISK = 0.3;

/**
 * @brief   Enable disk instability checks
 *
 * If enabled (1), calls disk instability module after feedback.
 * If disabled (0), skips disk instability (deferred to future module).
 *
 * Configuration: SageStarformationFeedback_DiskInstabilityOn
 */
static int DISK_INSTABILITY_ON = 0;

// ============================================================================
// HELPER FUNCTIONS (Physics Calculations)
// ============================================================================

// Metallicity calculation provided by shared utility: mimic_get_metallicity()
// See: src/modules/shared/metallicity.h

// Disk radius calculation provided by shared utility: mimic_get_disk_radius()
// See: src/modules/shared/disk_radius.h

/**
 * @brief   Updates galaxy properties due to star formation
 *
 * Implements the changes to galaxy properties caused by star formation:
 * 1. Reduces cold gas mass (accounting for recycling)
 * 2. Reduces cold gas metal content
 * 3. Increases stellar mass
 * 4. Increases stellar metal content
 *
 * The recycling fraction represents the portion of stellar mass returned
 * to the ISM immediately through stellar winds and supernovae.
 *
 * @param   gal           Pointer to galaxy data structure
 * @param   stars         Mass of stars formed in this time step
 * @param   metallicity   Current metallicity of the cold gas
 */
static void update_from_star_formation(struct GalaxyData *gal, float stars,
                                       float metallicity) {
    /* Update cold gas mass, accounting for recycling */
    gal->ColdGas -= (1.0f - (float)RECYCLE_FRACTION) * stars;

    /* Update cold gas metal content */
    gal->MetalsColdGas -= metallicity * (1.0f - (float)RECYCLE_FRACTION) * stars;

    /* Update stellar mass */
    gal->StellarMass += (1.0f - (float)RECYCLE_FRACTION) * stars;

    /* Update stellar metal content */
    gal->MetalsStellarMass +=
        metallicity * (1.0f - (float)RECYCLE_FRACTION) * stars;
}

/**
 * @brief   Updates galaxy properties due to supernova feedback
 *
 * Implements the changes to galaxy properties caused by supernova feedback:
 * 1. Reheating of cold gas to the hot phase (within the halo)
 * 2. Ejection of hot gas from the halo (to the ejected reservoir)
 * 3. Transfer of metals between the different gas phases
 * 4. Updating the outflow rate for the galaxy
 *
 * The supernova feedback follows a two-stage process:
 * - First, cold gas is reheated and added to the hot gas of the central galaxy
 * - Then, some of this hot gas may be ejected from the halo completely if
 *   the supernova energy is sufficient to overcome the halo potential
 *
 * @param   gal              Pointer to this galaxy's data
 * @param   central_gal      Pointer to central galaxy's data
 * @param   reheated_mass    Mass of cold gas reheated to hot phase
 * @param   ejected_mass     Mass of hot gas ejected from the halo
 * @param   metallicity      Current metallicity of the cold gas
 */
static void update_from_feedback(struct GalaxyData *gal,
                                 struct GalaxyData *central_gal,
                                 float reheated_mass, float ejected_mass,
                                 float metallicity) {
    float metallicity_hot;

    if (SUPERNOVA_RECIPE_ON != 1) {
        return;  /* No feedback if disabled */
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
    metallicity_hot =
        mimic_get_metallicity(central_gal->HotGas, central_gal->MetalsHotGas);

    /* Remove ejected gas from hot phase */
    central_gal->HotGas -= ejected_mass;
    central_gal->MetalsHotGas -= metallicity_hot * ejected_mass;

    /* Add ejected gas to ejected reservoir */
    central_gal->EjectedMass += ejected_mass;
    central_gal->MetalsEjectedMass += metallicity_hot * ejected_mass;

    /* Update outflow rate for the galaxy */
    gal->OutflowRate += reheated_mass;
}

// ============================================================================
// MODULE LIFECYCLE FUNCTIONS
// ============================================================================

/**
 * @brief   Initialize the SAGE star formation and feedback module
 *
 * Called once during program startup. Reads module parameters from
 * configuration and logs module setup.
 *
 * @return  0 on success, -1 on failure
 */
static int sage_starformation_feedback_init(void) {
    /* Read module parameters from configuration */
    module_get_int("SageStarformationFeedback", "SFprescription",
                  &SF_PRESCRIPTION, 0);
    module_get_double("SageStarformationFeedback", "SfrEfficiency",
                     &SFR_EFFICIENCY, 0.02);
    module_get_int("SageStarformationFeedback", "SupernovaRecipeOn",
                  &SUPERNOVA_RECIPE_ON, 1);
    module_get_double("SageStarformationFeedback", "FeedbackReheatingEpsilon",
                     &FEEDBACK_REHEATING_EPSILON, 3.0);
    module_get_double("SageStarformationFeedback", "FeedbackEjectionEfficiency",
                     &FEEDBACK_EJECTION_EFFICIENCY, 0.3);
    module_get_double("SageStarformationFeedback", "EnergySNcode",
                     &ENERGY_SN_CODE, 1.0);
    module_get_double("SageStarformationFeedback", "EtaSNcode", &ETA_SN_CODE,
                     0.5);
    module_get_double("SageStarformationFeedback", "RecycleFraction",
                     &RECYCLE_FRACTION, 0.43);
    module_get_double("SageStarformationFeedback", "Yield", &YIELD, 0.03);
    module_get_double("SageStarformationFeedback", "FracZleaveDisk",
                     &FRAC_Z_LEAVE_DISK, 0.3);
    module_get_int("SageStarformationFeedback", "DiskInstabilityOn",
                  &DISK_INSTABILITY_ON, 0);

    /* Validate parameters */
    if (SF_PRESCRIPTION != 0) {
        ERROR_LOG("SageStarformationFeedback_SFprescription = %d is invalid. "
                 "Only prescription 0 (Kennicutt-Schmidt with threshold) is "
                 "currently implemented.",
                 SF_PRESCRIPTION);
        return -1;
    }

    if (SFR_EFFICIENCY < 0.0 || SFR_EFFICIENCY > 1.0) {
        ERROR_LOG("SageStarformationFeedback_SfrEfficiency = %.3f is outside "
                 "valid range [0.0, 1.0]",
                 SFR_EFFICIENCY);
        return -1;
    }

    if (FEEDBACK_REHEATING_EPSILON < 0.0) {
        ERROR_LOG("SageStarformationFeedback_FeedbackReheatingEpsilon = %.3f "
                 "must be non-negative",
                 FEEDBACK_REHEATING_EPSILON);
        return -1;
    }

    if (FEEDBACK_EJECTION_EFFICIENCY < 0.0) {
        ERROR_LOG("SageStarformationFeedback_FeedbackEjectionEfficiency = %.3f "
                 "must be non-negative",
                 FEEDBACK_EJECTION_EFFICIENCY);
        return -1;
    }

    if (RECYCLE_FRACTION < 0.0 || RECYCLE_FRACTION > 1.0) {
        ERROR_LOG("SageStarformationFeedback_RecycleFraction = %.3f is outside "
                 "valid range [0.0, 1.0]",
                 RECYCLE_FRACTION);
        return -1;
    }

    if (YIELD < 0.0 || YIELD > 1.0) {
        ERROR_LOG("SageStarformationFeedback_Yield = %.3f is outside "
                 "valid range [0.0, 1.0]",
                 YIELD);
        return -1;
    }

    if (ENERGY_SN_CODE < 0.0) {
        ERROR_LOG("SageStarformationFeedback_EnergySNcode = %.3e must be non-negative",
                 ENERGY_SN_CODE);
        return -1;
    }

    if (ETA_SN_CODE < 0.0) {
        ERROR_LOG("SageStarformationFeedback_EtaSNcode = %.3f must be non-negative",
                 ETA_SN_CODE);
        return -1;
    }

    if (FRAC_Z_LEAVE_DISK < 0.0 || FRAC_Z_LEAVE_DISK > 1.0) {
        ERROR_LOG("SageStarformationFeedback_FracZleaveDisk = %.3f is outside "
                 "valid range [0.0, 1.0]",
                 FRAC_Z_LEAVE_DISK);
        return -1;
    }

    /* Log module configuration */
    INFO_LOG("SAGE Star Formation and Feedback module initialized");
    INFO_LOG("  SF prescription: %d (Kennicutt-Schmidt with threshold)",
             SF_PRESCRIPTION);
    INFO_LOG("  SFR efficiency: %.4f", SFR_EFFICIENCY);
    INFO_LOG("  Supernova feedback: %s",
             SUPERNOVA_RECIPE_ON ? "enabled" : "disabled");
    if (SUPERNOVA_RECIPE_ON) {
        INFO_LOG("    Reheating epsilon: %.3f", FEEDBACK_REHEATING_EPSILON);
        INFO_LOG("    Ejection efficiency: %.3f", FEEDBACK_EJECTION_EFFICIENCY);
        INFO_LOG("    EnergySNcode: %.3f", ENERGY_SN_CODE);
        INFO_LOG("    EtaSNcode: %.3f", ETA_SN_CODE);
    }
    INFO_LOG("  Recycle fraction: %.3f", RECYCLE_FRACTION);
    INFO_LOG("  Metal yield: %.4f", YIELD);
    INFO_LOG("  FracZleaveDisk: %.3f", FRAC_Z_LEAVE_DISK);
    INFO_LOG("  Disk instability: %s",
             DISK_INSTABILITY_ON ? "enabled" : "disabled (deferred to future module)");

    return 0;
}

/**
 * @brief   Process star formation and feedback for halos in a FOF group
 *
 * Called for each FOF group after infall and cooling modules. Implements:
 * 1. Star formation in galaxies with cold gas
 * 2. Supernova feedback (reheating and ejection)
 * 3. Metal enrichment from newly formed stars
 * 4. Disk structure updates
 *
 * @param   ctx    Module execution context (redshift, time, params)
 * @param   halos  Array of halos in the FOF group
 * @param   ngal   Number of halos in the array
 * @return  0 on success, -1 on failure
 */
static int sage_starformation_feedback_process(struct ModuleContext *ctx,
                                                struct Halo *halos, int ngal) {
    (void)ctx;  /* Context available for future use (e.g., redshift, time) */

    /* Validate inputs */
    if (halos == NULL || ngal <= 0) {
        return 0;  /* Nothing to process */
    }

    /* Find central galaxy (Type == 0) once before processing - only one per FOF group */
    int central_idx = -1;
    for (int j = 0; j < ngal; j++) {
        if (halos[j].Type == 0) {
            central_idx = j;
            break;
        }
    }

    /* Skip entire FOF group if no central found (shouldn't happen in well-formed groups) */
    if (central_idx < 0) {
        DEBUG_LOG("No central galaxy found in FOF group, skipping all %d halos", ngal);
        return 0;
    }

    /* Process each halo */
    for (int i = 0; i < ngal; i++) {
        /* Validate galaxy data */
        if (halos[i].galaxy == NULL) {
            ERROR_LOG("Halo %d has NULL galaxy data", i);
            return -1;
        }

        /* Get pointers to galaxy data */
        struct GalaxyData *gal = halos[i].galaxy;
        struct GalaxyData *central_gal = halos[central_idx].galaxy;

        /* Update disk scale radius (needed for star formation calculation) */
        gal->DiskScaleRadius = mimic_get_disk_radius(
            InputTreeHalos[halos[i].HaloNr].Spin[0],
            InputTreeHalos[halos[i].HaloNr].Spin[1],
            InputTreeHalos[halos[i].HaloNr].Spin[2],
            halos[i].Vvir, halos[i].Rvir);

        /* Get timestep from halo (calculated by core) */
        float dt = halos[i].dT;

        /* Validate timestep */
        if (dt <= 0.0f) {
            DEBUG_LOG("Halo %d: Invalid dT=%.3e, skipping star formation", i,
                     dt);
            continue;
        }

        /* Initialize star formation rate */
        float strdot = 0.0f;

        /* Apply star formation recipe */
        if (SF_PRESCRIPTION == 0) {
            /* Kennicutt-Schmidt law with critical threshold */

            /* Calculate effective star-forming radius (3 scale lengths) */
            float reff = 3.0f * gal->DiskScaleRadius;

            /* Calculate dynamical time of the disk */
            float tdyn = safe_div(reff, halos[i].Vvir, 0.0f);

            /* Calculate critical cold gas mass (Kauffmann 1996 eq7 × πR²) */
            float cold_crit = 0.19f * halos[i].Vvir * reff;

            /* Star formation occurs only if gas mass exceeds critical threshold */
            if (gal->ColdGas > cold_crit) {
                strdot = (float)SFR_EFFICIENCY *
                         safe_div(gal->ColdGas - cold_crit, tdyn, 0.0f);
            } else {
                strdot = 0.0f;
            }
        }

        /* Calculate mass of stars formed in this time step */
        float stars = strdot * dt;
        if (stars < 0.0f) {
            stars = 0.0f;
        }

        /* Calculate gas reheated by supernova feedback */
        float reheated_mass = 0.0f;
        if (SUPERNOVA_RECIPE_ON == 1) {
            reheated_mass = (float)FEEDBACK_REHEATING_EPSILON * stars;
        }

        /* Ensure reheated mass is non-negative */
        if (reheated_mass < 0.0f) {
            reheated_mass = 0.0f;
        }

        /* Ensure total gas used doesn't exceed available cold gas */
        if (stars + reheated_mass > gal->ColdGas) {
            float fac = safe_div(gal->ColdGas, (stars + reheated_mass), 1.0f);
            stars *= fac;
            reheated_mass *= fac;
        }

        /* Calculate gas ejection due to powerful feedback */
        float ejected_mass = 0.0f;
        if (SUPERNOVA_RECIPE_ON == 1) {
            /* Energy-driven outflow model */
            float vvir_sq = halos[central_idx].Vvir * halos[central_idx].Vvir;
            ejected_mass =
                ((float)FEEDBACK_EJECTION_EFFICIENCY *
                     safe_div((float)(ETA_SN_CODE * ENERGY_SN_CODE), vvir_sq,
                              0.0f) -
                 (float)FEEDBACK_REHEATING_EPSILON) *
                stars;

            /* Ensure ejected mass is non-negative */
            if (ejected_mass < 0.0f) {
                ejected_mass = 0.0f;
            }
        }

        /* Update galaxy properties from star formation */
        float metallicity = mimic_get_metallicity(gal->ColdGas, gal->MetalsColdGas);
        update_from_star_formation(gal, stars, metallicity);

        /* Recompute metallicity after star formation */
        metallicity = mimic_get_metallicity(gal->ColdGas, gal->MetalsColdGas);

        /* Update galaxy properties from supernova feedback */
        update_from_feedback(gal, central_gal, reheated_mass, ejected_mass,
                           metallicity);

        /* Metal production from newly formed stars (instantaneous recycling) */
        if (gal->ColdGas > 1.0e-8f) {
            /* Calculate mass-dependent fraction of metals leaving disk */
            /* Following Krumholz & Dekel 2011 Eq. 22 */
            float frac_z_leave_disk_val =
                (float)FRAC_Z_LEAVE_DISK *
                expf(-1.0f * halos[central_idx].Mvir / 30.0f);

            /* Distribute newly produced metals */
            gal->MetalsColdGas +=
                (float)YIELD * (1.0f - frac_z_leave_disk_val) * stars;
            central_gal->MetalsHotGas +=
                (float)YIELD * frac_z_leave_disk_val * stars;
        } else {
            /* If no cold gas, all metals go to hot gas */
            central_gal->MetalsHotGas += (float)YIELD * stars;
        }

        /* Disk instability check (deferred to future module) */
        if (DISK_INSTABILITY_ON) {
            /* TODO: Call sage_disk_instability module when implemented */
            /* For now, this is a no-op */
        }

        /* Debug logging */
        DEBUG_LOG(
            "Halo %d (Type=%d): SF=%.3e, Reheat=%.3e, Eject=%.3e, "
            "ColdGas=%.3e, StellarMass=%.3e",
            i, halos[i].Type, stars / dt, reheated_mass, ejected_mass,
            gal->ColdGas, gal->StellarMass);
    }

    return 0;
}

/**
 * @brief   Cleanup the SAGE star formation and feedback module
 *
 * Called once during program shutdown. Frees any allocated resources.
 *
 * @return  0 on success
 */
static int sage_starformation_feedback_cleanup(void) {
    INFO_LOG("SAGE Star Formation and Feedback module cleaned up");
    return 0;
}

// ============================================================================
// MODULE REGISTRATION
// ============================================================================

/**
 * @brief   Module interface structure
 */
static struct Module sage_starformation_feedback_module = {
    .name = "sage_starformation_feedback",
    .init = sage_starformation_feedback_init,
    .process_halos = sage_starformation_feedback_process,
    .cleanup = sage_starformation_feedback_cleanup};

/**
 * @brief   Register the SAGE star formation and feedback module
 *
 * Called during module system initialization to add this module to the
 * execution pipeline.
 */
void sage_starformation_feedback_register(void) {
    module_registry_add(&sage_starformation_feedback_module);
}
