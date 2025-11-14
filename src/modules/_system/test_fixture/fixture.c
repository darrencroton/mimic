/**
 * @file    test_fixture.c
 * @brief   Test fixture module implementation
 *
 * ⚠️  WARNING: This module is for TESTING INFRASTRUCTURE ONLY ⚠️
 *
 * DO NOT USE IN PRODUCTION RUNS
 *
 * This minimal module exists solely to test core module system functionality
 * (configuration, registration, pipeline execution) without coupling
 * infrastructure tests to production physics modules.
 *
 * Vision Principle #1: Physics-Agnostic Core Infrastructure
 * - Infrastructure tests MUST use this fixture, not production modules
 * - This prevents production module changes from breaking infrastructure tests
 * - Maintains clean separation between core and physics
 *
 * @author  Mimic Development Team
 * @date    2025-11-13
 */

#include <stdio.h>
#include <stdlib.h>

#include "error.h"
#include "module_interface.h"
#include "module_registry.h"
#include "fixture.h"
#include "types.h"

/**
 * @brief   Dummy parameter for testing parameter API
 *
 * Read from configuration file via TestFixture_DummyParameter parameter.
 * Has no physical meaning - exists only to test parameter system.
 */
static double DUMMY_PARAMETER = 1.0; /* Default value */

/**
 * @brief   Enable verbose logging for test validation
 *
 * Read from configuration file via TestFixture_EnableLogging parameter.
 * 0 = minimal logging (default), 1 = verbose logging for test validation
 */
static int ENABLE_LOGGING = 0; /* Default: off */

/**
 * @brief   Initialize test fixture module
 *
 * Called once during program startup. Reads module parameters from
 * configuration and logs module configuration.
 *
 * @return  0 on success
 */
static int test_fixture_init(void) {
  // Read module parameters from configuration
  module_get_double("TestFixture", "DummyParameter", &DUMMY_PARAMETER, 1.0);
  module_get_int("TestFixture", "EnableLogging", &ENABLE_LOGGING, 0);

  INFO_LOG("Test fixture module initialized");
  INFO_LOG("  ⚠️  WARNING: Testing infrastructure only - NOT FOR PRODUCTION");
  INFO_LOG("  DummyParameter = %.3f (from config)", DUMMY_PARAMETER);
  INFO_LOG("  EnableLogging = %d (from config)", ENABLE_LOGGING);

  return 0;
}

/**
 * @brief   Process halos in a FOF group
 *
 * Performs minimal processing:
 * - Sets TestDummyProperty = DUMMY_PARAMETER on all galaxies
 * - Logs processing if EnableLogging=1
 *
 * This validates the module system can execute modules and access properties.
 *
 * @param   ctx     Module execution context (provides redshift, time, params)
 * @param   halos   Array of halos in the FOF group (FoFWorkspace)
 * @param   ngal    Number of halos in the array
 * @return  0 on success, -1 on error
 */
static int test_fixture_process(struct ModuleContext *ctx, struct Halo *halos,
                                 int ngal) {
  if (halos == NULL || ngal <= 0) {
    return 0; // Nothing to process
  }

  if (ENABLE_LOGGING) {
    DEBUG_LOG("Test fixture processing %d halos at z=%.2f", ngal, ctx->redshift);
  }

  // Process each halo in the FOF group
  for (int i = 0; i < ngal; i++) {
    // Only process central galaxies
    if (halos[i].Type != 0) {
      continue;
    }

    // Validate galaxy data is allocated
    if (halos[i].galaxy == NULL) {
      ERROR_LOG("Halo %d (Type=0) has NULL galaxy data", i);
      return -1;
    }

    // Set dummy property (validates property system works)
    halos[i].galaxy->TestDummyProperty = (float)DUMMY_PARAMETER;

    if (ENABLE_LOGGING) {
      DEBUG_LOG("  Halo %d: Set TestDummyProperty = %.3f", i, DUMMY_PARAMETER);
    }
  }

  return 0;
}

/**
 * @brief   Cleanup test fixture module
 *
 * Called once during program shutdown. No resources to clean up for this
 * minimal module.
 *
 * @return 0 on success
 */
static int test_fixture_cleanup(void) {
  if (ENABLE_LOGGING) {
    DEBUG_LOG("Test fixture module cleanup");
  }
  // No resources to free
  return 0;
}

/**
 * @brief   Register test fixture module
 *
 * Creates and registers this module with the module registry.
 * Should be called once during program initialization.
 */
void test_fixture_register(void) {
  static struct Module test_fixture_module = {
      .name = "test_fixture",
      .init = test_fixture_init,
      .process_halos = test_fixture_process,
      .cleanup = test_fixture_cleanup};

  module_registry_add(&test_fixture_module);
}
