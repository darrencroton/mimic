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
 * Generated: 2025-11-13 13:26:45
 * Source MD5: 905ddaf6dbe943f1c2095d4ccccfb102
 */

#include "module_registry.h"

/* Auto-generated module includes (sorted alphabetically) */
#include "sage_infall/sage_infall.h"
#include "simple_sfr/simple_sfr.h"
#include "test_fixture/fixture.h"

/**
 * @brief Register all available physics modules
 *
 * Modules registered: 3
 *
 * Dependency order:
 * 1. sage_infall: provides [HotGas, MetalsHotGas, EjectedMass, MetalsEjectedMass, ICS, MetalsICS, TotalSatelliteBaryons]
 * 2. simple_sfr: requires [ColdGas] → provides [StellarMass]
 * 3. test_fixture: provides [TestDummyProperty]
 */
void register_all_modules(void) {
    /* Register in dependency-resolved order */
    sage_infall_register();  /* Provides: HotGas, MetalsHotGas, EjectedMass, MetalsEjectedMass, ICS, MetalsICS, TotalSatelliteBaryons */
    simple_sfr_register();  /* Requires: ColdGas → Provides: StellarMass */
    test_fixture_register();  /* Provides: TestDummyProperty */
}
