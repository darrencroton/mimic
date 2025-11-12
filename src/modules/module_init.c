/**
 * @file    module_init.c
 * @brief   Module registration initialization
 *
 * This file contains the module registration code, isolating physics-specific
 * knowledge from the core infrastructure. The core (main.c) calls
 * register_all_modules() without knowing which specific modules exist.
 *
 * Vision Principle 1 (Physics-Agnostic Core): By isolating module registration
 * here, the core infrastructure has zero knowledge of specific physics
 * implementations.
 *
 * Phase 5 Implementation Note: This file will be auto-generated from module
 * metadata (module_info.yaml files) to enable build-time module selection.
 * For Phase 3, it's manually maintained but architecturally separated from
 * core.
 */

#include "module_registry.h"

/* Module headers - isolated from core */
#include "sage_infall/sage_infall.h"
#include "simple_cooling/simple_cooling.h"
#include "simple_sfr/simple_sfr.h"

/**
 * @brief   Register all available physics modules
 *
 * This function registers all physics modules that are compiled into the
 * current build. The core calls this function without knowing which specific
 * modules exist.
 *
 * Module registration order matters for property dependencies:
 * - sage_infall provides HotGas, MetalsHotGas, EjectedMass, ICS
 * - simple_cooling provides ColdGas
 * - simple_sfr consumes ColdGas
 *
 * Therefore modules must be registered in dependency order.
 *
 * Phase 4.2: SAGE physics modules added
 * Phase 5: Auto-generated from module metadata and build configuration
 */
void register_all_modules(void) {
  /* Register modules in dependency order */
  sage_infall_register();   /* Provides: HotGas, MetalsHotGas, etc. */
  simple_cooling_register(); /* Provides: ColdGas */
  simple_sfr_register();     /* Consumes: ColdGas */
}
