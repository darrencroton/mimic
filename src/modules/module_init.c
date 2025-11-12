/* AUTO-GENERATED FILE - DO NOT EDIT MANUALLY */
/* Generated from module metadata by scripts/generate_module_registry.py */
/* Source: src/modules/[MODULE]/module_info.yaml */
/*
 * To regenerate:
 *   make generate-modules
 *
 * To validate:
 *   make validate-modules
 *
 * Generated: 2025-11-12 16:35:36
 * Source MD5: cb8c41a3333437433d443d9a8ffe3479
 */

#include "module_registry.h"

/* Auto-generated module includes (sorted alphabetically) */
#include "sage_infall/sage_infall.h"
#include "simple_cooling/simple_cooling.h"
#include "simple_sfr/simple_sfr.h"

/**
 * @brief Register all available physics modules
 *
 * Modules registered: 3
 *
 * Dependency order:
 * 1. sage_infall: provides [HotGas, MetalsHotGas, EjectedMass, MetalsEjectedMass, ICS, MetalsICS, TotalSatelliteBaryons]
 * 2. simple_cooling: provides [ColdGas]
 * 3. simple_sfr: requires [ColdGas] → provides [StellarMass]
 */
void register_all_modules(void) {
    /* Register in dependency-resolved order */
    sage_infall_register();  /* Provides: HotGas, MetalsHotGas, EjectedMass, MetalsEjectedMass, ICS, MetalsICS, TotalSatelliteBaryons */
    simple_cooling_register();  /* Provides: ColdGas */
    simple_sfr_register();  /* Requires: ColdGas → Provides: StellarMass */
}
