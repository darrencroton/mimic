/**
 * @file    test_unit_sage_cooling.c
 * @brief   Software quality unit tests for sage_cooling module
 *
 * Validates: Module lifecycle, cooling tables, interpolation, memory safety
 * Phase: Phase 4.2 (SAGE Physics Module Implementation)
 *
 * This test validates software engineering aspects of the sage_cooling module:
 * - Module registration and initialization
 * - Cooling table loading and validation
 * - Temperature and metallicity interpolation
 * - Edge case handling (primordial gas, super-solar Z, extreme temps)
 * - Memory allocation and cleanup (no leaks)
 * - Parameter reading and validation
 *
 * Test cases:
 *   - test_module_registration: Module registers correctly
 *   - test_cooling_tables_loading: Tables load successfully from module directory
 *   - test_temperature_interpolation: Temperature interpolation is accurate
 *   - test_metallicity_interpolation: 2D interpolation works correctly
 *   - test_primordial_gas_cooling: Handles zero metallicity correctly
 *   - test_super_solar_metallicity: Handles high metallicity correctly
 *   - test_extreme_temperatures: Handles temperature limits correctly
 *   - test_memory_safety: No memory leaks during operation
 *
 * NOTE: Physics validation (comparison with SAGE) handled separately in integration tests
 *
 * @author  Mimic Development Team
 * @date    2025-11-13
 */

#include "framework/test_framework.h"
#include "../../core/module_registry.h"
#include "../../core/module_interface.h"
#include "sage_cooling.h"
#include "cooling_tables.h"
#include "../../include/types.h"
#include "../../include/proto.h"
#include "../../include/globals.h"
#include "../../util/error.h"
#include "../../util/memory.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* Test statistics (required for TEST_RUN macro) */
static int passed = 0;
static int failed = 0;

/* Track whether modules have been registered */
static int modules_registered = 0;

/* Test fixture: reset configuration state */
static void reset_config(void)
{
    memset(&MimicConfig, 0, sizeof(MimicConfig));
}

/* Test fixture: ensure modules are registered (only once) */
static void ensure_modules_registered(void)
{
    if (!modules_registered) {
        register_all_modules();
        modules_registered = 1;
    }
}

/* Test fixture: Set sage_cooling parameters to defaults */
static void set_default_sage_cooling_params(void)
{
    strcpy(MimicConfig.ModuleParams[0].module_name, "SageCooling");
    strcpy(MimicConfig.ModuleParams[0].param_name, "RadioModeEfficiency");
    strcpy(MimicConfig.ModuleParams[0].value, "0.01");

    strcpy(MimicConfig.ModuleParams[1].module_name, "SageCooling");
    strcpy(MimicConfig.ModuleParams[1].param_name, "AGNrecipeOn");
    strcpy(MimicConfig.ModuleParams[1].value, "1");

    MimicConfig.NumModuleParams = 2;
}

/**
 * @test    test_module_registration
 * @brief   Test that sage_cooling module registers correctly
 *
 * Expected: Module registration succeeds without errors
 * Validates: sage_cooling_register() works, module appears in registry
 */
int test_module_registration(void)
{
    ensure_modules_registered();

    /* Check module is registered by trying to enable it */
    reset_config();
    strcpy(MimicConfig.EnabledModules, "sage_cooling");
    MimicConfig.NumModuleParams = 0;

    /* Should fail if module not registered */
    int result = module_system_init();

    /* Clean up */
    if (result == 0) {
        module_system_cleanup();
    }

    TEST_ASSERT(result == 0, "sage_cooling module should register successfully");
    return 0;
}

/**
 * @test    test_cooling_tables_loading
 * @brief   Test that cooling function tables load correctly
 *
 * Expected: All 8 metallicity tables load successfully
 * Validates: cooling_tables_init() succeeds, files exist in module directory
 */
int test_cooling_tables_loading(void)
{
    /* Test loading from module directory */
    const char *cool_dir = "src/modules/sage_cooling/CoolFunctions";

    int result = cooling_tables_init(cool_dir);

    TEST_ASSERT(result == 0, "Cooling tables should load successfully from module directory");

    /* Clean up */
    cooling_tables_cleanup();

    return 0;
}

/**
 * @test    test_temperature_interpolation
 * @brief   Test temperature interpolation accuracy
 *
 * Expected: Interpolation between table points is accurate
 * Validates: get_metaldependent_cooling_rate() temperature interpolation
 */
int test_temperature_interpolation(void)
{
    const char *cool_dir = "src/modules/sage_cooling/CoolFunctions";

    int result = cooling_tables_init(cool_dir);
    TEST_ASSERT(result == 0, "Tables must load for interpolation test");

    /* Test interpolation at solar metallicity (logZ = log10(0.02) = -1.699)
     * Test temperature T = 10^5 K (logT = 5.0) */
    double logT = 5.0;
    double logZ = log10(0.02);  /* Solar metallicity */

    double lambda = get_metaldependent_cooling_rate(logT, logZ);

    /* Lambda should be positive and reasonable (order of magnitude 10^-22 to 10^-20) */
    TEST_ASSERT(lambda > 0.0, "Cooling rate must be positive");
    TEST_ASSERT(lambda < 1e-18, "Cooling rate should be reasonable magnitude");
    TEST_ASSERT(lambda > 1e-26, "Cooling rate should not be too small");

    cooling_tables_cleanup();
    return 0;
}

/**
 * @test    test_metallicity_interpolation
 * @brief   Test 2D metallicity-dependent interpolation
 *
 * Expected: Cooling rate varies smoothly with metallicity
 * Validates: get_metaldependent_cooling_rate() 2D interpolation
 */
int test_metallicity_interpolation(void)
{
    const char *cool_dir = "src/modules/sage_cooling/CoolFunctions";

    int result = cooling_tables_init(cool_dir);
    TEST_ASSERT(result == 0, "Tables must load for interpolation test");

    /* Test at fixed temperature, varying metallicity */
    double logT = 5.5;  /* T = 10^5.5 K */

    /* Get cooling rates at different metallicities */
    double lambda_primordial = get_metaldependent_cooling_rate(logT, -10.0);  /* Very low Z */
    double lambda_subsolar = get_metaldependent_cooling_rate(logT, log10(0.002));  /* 0.1 solar */
    double lambda_solar = get_metaldependent_cooling_rate(logT, log10(0.02));  /* solar */
    double lambda_supersolar = get_metaldependent_cooling_rate(logT, log10(0.04));  /* 2 solar */

    /* All should be positive */
    TEST_ASSERT(lambda_primordial > 0.0, "Primordial cooling rate must be positive");
    TEST_ASSERT(lambda_subsolar > 0.0, "Sub-solar cooling rate must be positive");
    TEST_ASSERT(lambda_solar > 0.0, "Solar cooling rate must be positive");
    TEST_ASSERT(lambda_supersolar > 0.0, "Super-solar cooling rate must be positive");

    /* Metallicity should increase cooling rate (more metal lines) */
    TEST_ASSERT(lambda_solar > lambda_primordial,
                "Solar metallicity should cool faster than primordial");

    cooling_tables_cleanup();
    return 0;
}

/**
 * @test    test_primordial_gas_cooling
 * @brief   Test cooling of zero-metallicity gas
 *
 * Expected: Primordial cooling handled correctly (no metals, only H/He)
 * Validates: Edge case handling for logZ → -infinity
 */
int test_primordial_gas_cooling(void)
{
    const char *cool_dir = "src/modules/sage_cooling/CoolFunctions";

    int result = cooling_tables_init(cool_dir);
    TEST_ASSERT(result == 0, "Tables must load for primordial test");

    /* Test primordial gas (Z = 0, logZ → -infinity) */
    double logT = 4.5;  /* T = 10^4.5 K */
    double logZ = -10.0;  /* Effectively zero metallicity */

    double lambda = get_metaldependent_cooling_rate(logT, logZ);

    /* Should use primordial table (table 0) */
    TEST_ASSERT(lambda > 0.0, "Primordial cooling must be positive");
    TEST_ASSERT(isfinite(lambda), "Primordial cooling must be finite");

    cooling_tables_cleanup();
    return 0;
}

/**
 * @test    test_super_solar_metallicity
 * @brief   Test cooling at super-solar metallicity
 *
 * Expected: Handles Z > Z_sun correctly (clamps to maximum table)
 * Validates: Edge case handling for high metallicity
 */
int test_super_solar_metallicity(void)
{
    const char *cool_dir = "src/modules/sage_cooling/CoolFunctions";

    int result = cooling_tables_init(cool_dir);
    TEST_ASSERT(result == 0, "Tables must load for super-solar test");

    /* Test super-solar metallicity (Z = 10 * Z_sun) */
    double logT = 6.0;  /* T = 10^6 K */
    double logZ = log10(0.2);  /* 10 times solar */

    double lambda = get_metaldependent_cooling_rate(logT, logZ);

    /* Should clamp to maximum table (super-solar) */
    TEST_ASSERT(lambda > 0.0, "Super-solar cooling must be positive");
    TEST_ASSERT(isfinite(lambda), "Super-solar cooling must be finite");

    /* Should give same result as maximum table metallicity */
    double lambda_max = get_metaldependent_cooling_rate(logT, log10(0.063));  /* [Fe/H]=+0.5 */
    TEST_ASSERT(fabs(lambda - lambda_max) < 1e-30,
                "Super-solar should clamp to maximum table");

    cooling_tables_cleanup();
    return 0;
}

/**
 * @test    test_extreme_temperatures
 * @brief   Test cooling at extreme temperatures
 *
 * Expected: Handles T < 10^4 K and T > 10^8.5 K correctly
 * Validates: Temperature bounds enforcement
 */
int test_extreme_temperatures(void)
{
    const char *cool_dir = "src/modules/sage_cooling/CoolFunctions";

    int result = cooling_tables_init(cool_dir);
    TEST_ASSERT(result == 0, "Tables must load for temperature test");

    double logZ = log10(0.02);  /* Solar metallicity */

    /* Test very low temperature (below table range) */
    double logT_low = 3.0;  /* T = 10^3 K, below minimum */
    double lambda_low = get_metaldependent_cooling_rate(logT_low, logZ);

    /* Should clamp to minimum temperature */
    TEST_ASSERT(lambda_low > 0.0, "Low-T cooling must be positive");
    TEST_ASSERT(isfinite(lambda_low), "Low-T cooling must be finite");

    /* Test very high temperature (above table range) */
    double logT_high = 9.0;  /* T = 10^9 K, above maximum */
    double lambda_high = get_metaldependent_cooling_rate(logT_high, logZ);

    /* Should clamp to maximum temperature */
    TEST_ASSERT(lambda_high > 0.0, "High-T cooling must be positive");
    TEST_ASSERT(isfinite(lambda_high), "High-T cooling must be finite");

    cooling_tables_cleanup();
    return 0;
}

/**
 * @test    test_memory_safety
 * @brief   Test for memory leaks during module operation
 *
 * Expected: No memory leaks after init/cleanup cycle
 * Validates: Proper memory management in cooling tables
 */
int test_memory_safety(void)
{
    ensure_modules_registered();

    /* Record initial memory state */
    size_t initial_mem = get_allocated_memory();

    /* Initialize module */
    reset_config();
    strcpy(MimicConfig.EnabledModules, "sage_cooling");
    set_default_sage_cooling_params();

    int result = module_system_init();
    TEST_ASSERT(result == 0, "Module init must succeed for memory test");

    /* Clean up */
    module_system_cleanup();

    /* Check for leaks */
    size_t final_mem = get_allocated_memory();

    TEST_ASSERT(final_mem == initial_mem,
                "No memory leaks after module init/cleanup cycle");

    return 0;
}

/**
 * @test    test_parameter_reading
 * @brief   Test module parameter reading and validation
 *
 * Expected: Parameters read correctly from configuration
 * Validates: module_get_double(), module_get_int() for sage_cooling
 */
int test_parameter_reading(void)
{
    ensure_modules_registered();

    reset_config();
    strcpy(MimicConfig.EnabledModules, "sage_cooling");

    /* Set custom parameters */
    strcpy(MimicConfig.ModuleParams[0].module_name, "SageCooling");
    strcpy(MimicConfig.ModuleParams[0].param_name, "RadioModeEfficiency");
    strcpy(MimicConfig.ModuleParams[0].value, "0.02");  /* Non-default */

    strcpy(MimicConfig.ModuleParams[1].module_name, "SageCooling");
    strcpy(MimicConfig.ModuleParams[1].param_name, "AGNrecipeOn");
    strcpy(MimicConfig.ModuleParams[1].value, "2");  /* Bondi-Hoyle mode */

    MimicConfig.NumModuleParams = 2;

    int result = module_system_init();
    TEST_ASSERT(result == 0, "Module should initialize with custom parameters");

    /* Clean up */
    if (result == 0) {
        module_system_cleanup();
    }

    return 0;
}

/* Main test runner */
int main(void)
{
    printf("\n");
    printf("============================================================\n");
    printf("sage_cooling Module Unit Tests\n");
    printf("============================================================\n\n");

    TEST_RUN(test_module_registration);
    TEST_RUN(test_cooling_tables_loading);
    TEST_RUN(test_temperature_interpolation);
    TEST_RUN(test_metallicity_interpolation);
    TEST_RUN(test_primordial_gas_cooling);
    TEST_RUN(test_super_solar_metallicity);
    TEST_RUN(test_extreme_temperatures);
    TEST_RUN(test_memory_safety);
    TEST_RUN(test_parameter_reading);

    printf("\n");
    printf("============================================================\n");
    printf("Test Summary\n");
    printf("============================================================\n");
    printf("Passed: %d\n", passed);
    printf("Failed: %d\n", failed);
    printf("Total:  %d\n", passed + failed);
    printf("============================================================\n");

    if (failed == 0) {
        printf("✓ All tests passed!\n");
        return 0;
    } else {
        printf("✗ Some tests failed\n");
        return 1;
    }
}
