/**
 * @file    sage_add_cooling.c
 * @brief   SAGE add cooling module - transfers cooled gas to cold reservoir
 *
 * Transfers gas from hot to cold reservoir based on CoolingGas property
 * (calculated by sage_calculate_cooling and modified by sage_radio_mode_heating).
 * Transfers gas with metallicity tracking and energy accounting.
 *
 * Physics: Transfer CoolingGas → ColdGas with metallicity preservation
 *          Tracks cooling energy: E_cool = 0.5 × m × V_vir²
 *
 * Reference: Croton et al. (2006, 2016), based on SAGE model_cooling_heating.c
 */

#include "constants.h"
#include "error.h"
#include "_shared/metallicity.h"
#include "_system/physical_constants.h"
#include "module_interface.h"
#include "module_registry.h"
#include "numeric.h"
#include "types.h"

// ============================================================================
// HELPER FUNCTIONS
// ============================================================================

/**
 * @brief   Transfers cooled gas from hot to cold reservoir
 *
 * @param   halo         Pointer to the halo structure
 * @param   coolingGas   Mass of gas to transfer this substep
 * @param   vvir         Virial velocity (for energy tracking)
 *
 * This function moves gas from the hot halo to the cold disk, along with
 * its associated metals. It ensures mass conservation and tracks cooling energy.
 */
static void cool_gas_onto_galaxy(struct Halo *halo, double coolingGas, float vvir)
{
    float metallicity;

    /* Only proceed if there is gas to cool */
    if (coolingGas <= EPSILON_SMALL)
        return;

    /* Check if we're trying to cool more gas than is available in the hot halo */
    if (coolingGas < halo->galaxy->HotGas) {
        /* Normal case: cooling doesn't deplete hot gas completely */
        metallicity = mimic_get_metallicity(halo->galaxy->HotGas, halo->galaxy->MetalsHotGas);
    } else {
        /* Edge case: cooling all remaining hot gas */
        coolingGas = halo->galaxy->HotGas;
        metallicity = mimic_get_metallicity(halo->galaxy->HotGas, halo->galaxy->MetalsHotGas);
    }

    /* Transfer gas from hot to cold reservoir */
    halo->galaxy->HotGas -= coolingGas;
    halo->galaxy->ColdGas += coolingGas;

    /* Transfer metals from hot to cold reservoir */
    halo->galaxy->MetalsHotGas -= metallicity * coolingGas;
    halo->galaxy->MetalsColdGas += metallicity * coolingGas;

    /* Track cooling energy for energy budget calculations
     * E_cool = 0.5 * m * V_vir^2 = change in potential energy */
    halo->galaxy->Cooling += KINETIC_ENERGY_FACTOR * coolingGas * vvir * vvir;
}

// ============================================================================
// MODULE LIFECYCLE FUNCTIONS
// ============================================================================

/**
 * @brief   Initialize the sage_add_cooling module
 *
 * No parameters to load - all configuration from predecessor modules.
 *
 * @return  0 on success
 */
int sage_add_cooling_init(void)
{
    INFO_LOG("SAGE add cooling module initialized");
    VERBOSE_LOG("  Physics: Transfer CoolingGas → ColdGas with metallicity");
    VERBOSE_LOG("  Execution: Runs each substep in phase_1 (process_by_galaxy)");
    VERBOSE_LOG("  Note: CoolingGas already calculated for this substep (no division needed)");
    return 0;
}

/**
 * @brief   Process individual galaxy for cooling transfer
 *
 * Transfers cooling gas from hot to cold reservoir. Processes galaxies with
 * hot gas (types 0 and 1 only).
 *
 * @param   ctx    Module context (substep info, redshift, etc.)
 * @param   halos  Pointer to single halo (ngal=1 for process_by_galaxy)
 * @param   ngal   Number of halos (always 1 for process_by_galaxy)
 * @return  0 on success
 */
int sage_add_cooling_process(struct ModuleContext *ctx, struct Halo *halos, int ngal)
{
    /* Validate process_by_galaxy contract */
    if (ngal != 1) {
        ERROR_LOG("process_by_galaxy expects ngal=1, got %d", ngal);
        return -1;
    }

    struct Halo *halo = &halos[0];

    /* Skip orphan galaxies (type 2) */
    if (halo->Type == 2) {
        return 0;
    }

    /* Validate galaxy data */
    if (halo->galaxy == NULL) {
        return 0;
    }

    /* CRITICAL: NO division by num_substeps - CoolingGas already calculated for this substep
     * sage_calculate_cooling calculates cooling for THIS substep using substep_dt */
    double coolingGas = (double)halo->galaxy->CoolingGas;

    if (coolingGas > EPSILON_SMALL) {
        cool_gas_onto_galaxy(halo, coolingGas, halo->Vvir);

        DEBUG_LOG("Cooling transfer: Type=%d, CoolingGas=%.3e, ColdGas=%.3e (after), HotGas=%.3e (after), z=%.3f",
                  halo->Type, coolingGas, halo->galaxy->ColdGas, halo->galaxy->HotGas, ctx->redshift);
    }

    return 0;
}

/**
 * @brief   Cleanup the sage_add_cooling module
 *
 * No resources to free.
 *
 * @return  0 on success
 */
int sage_add_cooling_cleanup(void)
{
    VERBOSE_LOG("SAGE add cooling module cleaned up");
    return 0;
}
