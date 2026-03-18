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
#include "types.h"

/**
 * @brief   Dummy parameter for testing parameter API
 *
 * Read from model_get_*() API via TestFixtureDummyParameter.
 * Has no physical meaning - exists only to test parameter system.
 */
static double DUMMY_PARAMETER;

/**
 * @brief   Enable verbose logging for test validation
 *
 * Read from model_get_*() API via TestFixtureEnableLogging.
 * 0 = minimal logging, 1 = verbose logging for test validation
 */
static int ENABLE_LOGGING;

/**
 * @brief   Execution counter for tracking module calls
 *
 * Incremented each time process() is called. Used for test validation
 * of phase execution frequency.
 */
static int execution_count = 0;

/**
 * @brief   Initialize test fixture module
 *
 * Called once during program startup. Reads module parameters from
 * model_get_*() API system and logs module configuration.
 *
 * @return  0 on success, -1 on error
 */
int test_fixture_init(void) {
  // Read parameters from model_get_*() API system
  if (model_get_double("TestFixtureDummyParameter", &DUMMY_PARAMETER) != 0) {
    ERROR_LOG("Failed to read TestFixtureDummyParameter from model_parameters");
    return -1;
  }

  if (model_get_int("TestFixtureEnableLogging", &ENABLE_LOGGING) != 0) {
    ERROR_LOG("Failed to read TestFixtureEnableLogging from model_parameters");
    return -1;
  }

  INFO_LOG("Test fixture module initialized");
  INFO_LOG("  ⚠️  WARNING: Testing infrastructure only - NOT FOR PRODUCTION");
  INFO_LOG("  DummyParameter = %.3f", DUMMY_PARAMETER);
  INFO_LOG("  EnableLogging = %d", ENABLE_LOGGING);

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
 * When ENABLE_LOGGING=1, logs detailed execution information for test validation:
 * - Execution count (for frequency verification)
 * - Substep information (for time-stepping tests)
 * - ngal parameter (for loop mode tests)
 * - Galaxy processing (for ordering tests)
 *
 * @param   ctx     Module execution context (provides redshift, time, params)
 * @param   halos   Array of halos in the FOF group (FoFWorkspace)
 * @param   ngal    Number of halos in the array
 * @return  0 on success, -1 on error
 */
int test_fixture_process(struct ModuleContext *ctx, struct Halo *halos,
                                 int ngal) {
  if (halos == NULL || ngal <= 0) {
    return 0; // Nothing to process
  }

  // Increment execution counter
  execution_count++;

  if (ENABLE_LOGGING) {
    // Log detailed execution information for test validation
    INFO_LOG("TEST_FIXTURE_EXEC: count=%d ngal=%d substep=%d/%d substep_dt=%.6e z=%.4f",
             execution_count, ngal, ctx->substep_number, ctx->num_substeps,
             ctx->substep_dt, ctx->redshift);
    if (ctx->active_event != NULL) {
      INFO_LOG("TEST_FIXTURE_EVENT: producer_module_id=%d event_id=%d "
               "source=%d target=%d value0=%.6e value1=%.6e",
               ctx->active_event->producer_module_id, ctx->active_event->event_id,
               ctx->active_event->source_index, ctx->active_event->target_index,
               ctx->active_event->value0, ctx->active_event->value1);
    }
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
int test_fixture_cleanup(void) {
  if (ENABLE_LOGGING) {
    INFO_LOG("TEST_FIXTURE_CLEANUP: total_executions=%d", execution_count);
  }
  // Reset execution counter for next run
  execution_count = 0;
  // No resources to free
  return 0;
}
