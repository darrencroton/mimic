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
 * Generated: 2025-11-13 18:08:29
 * Source MD5: 083b2048ab7b6afa4fa96a532c63fb8f
 */

#include "module_registry.h"

/* Auto-generated module includes (sorted alphabetically) */
#include "sage_cooling/sage_cooling.h"
#include "sage_cooling/cooling_tables.h"
#include "sage_infall/sage_infall.h"
#include "simple_sfr/simple_sfr.h"
#include "test_fixture/fixture.h"

/**
 * @brief Register all available physics modules
 *
 * Modules registered: 4
 *
 * Dependency order:
 * 1. sage_infall: provides [HotGas, MetalsHotGas, EjectedMass, MetalsEjectedMass, ICS, MetalsICS, TotalSatelliteBaryons]
 * 2. test_fixture: provides [TestDummyProperty]
 * 3. sage_cooling: requires [HotGas, MetalsHotGas] → provides [ColdGas, MetalsColdGas, BlackHoleMass, Cooling, Heating, r_heat]
 * 4. simple_sfr: requires [ColdGas] → provides [StellarMass]
 */
void register_all_modules(void) {
    /* Register in dependency-resolved order */
    sage_infall_register();  /* Provides: HotGas, MetalsHotGas, EjectedMass, MetalsEjectedMass, ICS, MetalsICS, TotalSatelliteBaryons */
    test_fixture_register();  /* Provides: TestDummyProperty */
    sage_cooling_register();  /* Requires: HotGas, MetalsHotGas → Provides: ColdGas, MetalsColdGas, BlackHoleMass, Cooling, Heating, r_heat */
    simple_sfr_register();  /* Requires: ColdGas → Provides: StellarMass */
}
