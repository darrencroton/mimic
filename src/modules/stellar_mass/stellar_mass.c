/**
 * @file stellar_mass.c
 * @brief Stellar mass module implementation
 *
 * This module implements a simple stellar mass calculation: StellarMass = 0.1 * Mvir
 *
 * This is a minimal proof-of-concept demonstrating:
 * - Module interface implementation (init/process/cleanup)
 * - Galaxy property updates via physics-agnostic GalaxyData structure
 * - Integration with the module registry system
 *
 * Vision Principle 1 (Physics-Agnostic Core): This module interacts with core
 * only through well-defined interfaces. Core has zero knowledge of stellar mass
 * physics.
 */

#include <stdio.h>
#include <stdlib.h>
#include "stellar_mass.h"
#include "module_registry.h"
#include "module_interface.h"
#include "types.h"
#include "error.h"

/**
 * @brief Stellar mass efficiency parameter
 *
 * Fraction of halo virial mass that converts to stellar mass.
 * In this simple model: StellarMass = STELLAR_EFFICIENCY * Mvir
 */
static const float STELLAR_EFFICIENCY = 0.1;

/**
 * @brief Initialize stellar mass module
 *
 * Called once during program startup. Logs module configuration.
 *
 * @return 0 on success
 */
static int stellar_mass_init(void)
{
    INFO_LOG("Stellar mass module initialized");
    INFO_LOG("  Physics: StellarMass = %.2f * Mvir", STELLAR_EFFICIENCY);
    return 0;
}

/**
 * @brief Process halos in a FOF group
 *
 * Computes stellar mass for all halos in the group using:
 *   StellarMass = STELLAR_EFFICIENCY * Mvir
 *
 * @param halos Array of halos in the FOF group (FoFWorkspace)
 * @param ngal Number of halos in the array
 * @return 0 on success
 */
static int stellar_mass_process(struct Halo *halos, int ngal)
{
    if (halos == NULL || ngal <= 0) {
        return 0; // Nothing to process
    }

    // Process each halo in the FOF group
    for (int i = 0; i < ngal; i++) {
        // Check that galaxy data is allocated
        if (halos[i].galaxy == NULL) {
            ERROR_LOG("Halo %d has NULL galaxy data", i);
            return -1;
        }

        // Compute stellar mass: 10% of halo virial mass
        halos[i].galaxy->StellarMass = STELLAR_EFFICIENCY * halos[i].Mvir;

        DEBUG_LOG("Halo %d: Mvir=%.3e -> StellarMass=%.3e",
                  i, halos[i].Mvir, halos[i].galaxy->StellarMass);
    }

    return 0;
}

/**
 * @brief Cleanup stellar mass module
 *
 * Called once during program shutdown. Logs cleanup.
 *
 * @return 0 on success
 */
static int stellar_mass_cleanup(void)
{
    INFO_LOG("Stellar mass module cleaned up");
    return 0;
}

/**
 * @brief Module structure for stellar mass module
 *
 * Defines this module's interface to the module system.
 */
static struct Module stellar_mass_module = {
    .name = "stellar_mass",
    .init = stellar_mass_init,
    .process_halos = stellar_mass_process,
    .cleanup = stellar_mass_cleanup
};

/**
 * @brief Register the stellar mass module
 *
 * Registers this module with the module registry. Should be called once
 * during program initialization before module_system_init().
 */
void stellar_mass_register(void)
{
    register_module(&stellar_mass_module);
}
