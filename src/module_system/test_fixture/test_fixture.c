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
 * It is dual mode: it advertises the three FoF modes (test_fixture_process)
 * and process_snapshot (test_fixture_process_snapshot), so tests can exercise
 * a module that registers both typed callbacks.
 *
 * With the optional TestFixtureCreateRecords parameter set above zero it also
 * creates records through module_create_record(), so tests can drive the
 * record-creation contract through a real run. Creation is legal only from
 * process_full_halo, so a configuration that sets the parameter must place the
 * fixture there; any other placement fails at the first creation call.
 *
 * Vision Principle #1: Physics-Agnostic Core Infrastructure
 * - Infrastructure tests MUST use this fixture, not production modules
 * - This prevents production module changes from breaking infrastructure tests
 * - Maintains clean separation between core and physics
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <string.h>

#include "error.h"
#include "globals.h"
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
 * @brief   Records to create per Type 0 host on every process() call
 *
 * Read from the optional TestFixtureCreateRecords parameter; absent means 0,
 * so every configuration written before the parameter existed is unchanged.
 */
static int CREATE_RECORDS = 0;

/**
 * @brief   Execution counter for tracking module calls
 *
 * Incremented each time process() is called. Used for test validation
 * of phase execution frequency.
 */
static int execution_count = 0;

/**
 * @brief   Snapshot-callback counter
 *
 * Incremented each time process_snapshot() is called, separately from the
 * FoF execution_count so tests can tell the two callback families apart.
 */
static int snapshot_execution_count = 0;

/**
 * @brief   Initialize test fixture module
 *
 * Called once during program startup. Reads module parameters from
 * model_get_*() API system and logs module configuration.
 *
 * @return  0 on success, -1 on error
 */
int test_fixture_init(void) {
  if (model_get_double("TestFixtureDummyParameter", &DUMMY_PARAMETER) != 0) {
    ERROR_LOG("Failed to read TestFixtureDummyParameter from model_parameters");
    return -1;
  }

  if (model_get_int("TestFixtureEnableLogging", &ENABLE_LOGGING) != 0) {
    ERROR_LOG("Failed to read TestFixtureEnableLogging from model_parameters");
    return -1;
  }

  /* Optional: check presence first, since model_get_int() treats a missing
   * parameter as an error. */
  CREATE_RECORDS = 0;
  for (int i = 0; i < MimicConfig.NumModelParams; i++) {
    if (strcmp(MimicConfig.ModelParams[i].param_name, "TestFixtureCreateRecords") == 0) {
      if (model_get_int("TestFixtureCreateRecords", &CREATE_RECORDS) != 0) {
        ERROR_LOG("Failed to read TestFixtureCreateRecords from model_parameters");
        return -1;
      }
      if (CREATE_RECORDS < 0) {
        ERROR_LOG("TestFixtureCreateRecords = %d must be >= 0", CREATE_RECORDS);
        return -1;
      }
      break;
    }
  }

  INFO_LOG("Test fixture module initialized");
  INFO_LOG("  ⚠️  WARNING: Testing infrastructure only - NOT FOR PRODUCTION");
  INFO_LOG("  DummyParameter = %.3f", DUMMY_PARAMETER);
  INFO_LOG("  EnableLogging = %d", ENABLE_LOGGING);
  INFO_LOG("  CreateRecords = %d", CREATE_RECORDS);

  return 0;
}

/**
 * @brief   Process halos in a FOF group
 *
 * Performs minimal processing:
 * - Sets TestDummyProperty = DUMMY_PARAMETER on all galaxies
 * - Logs processing if EnableLogging=1
 * - When CREATE_RECORDS > 0, creates that many records on every Type 0 row
 *   through module_create_record(), sets TestDummyProperty = DUMMY_PARAMETER on
 *   each, and logs one TEST_FIXTURE_CREATE line per host (regardless of
 *   EnableLogging). Only rows present when the call began are hosts.
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
 * @param   halos   Array of halos in the FOF group (the FoF workspace)
 * @param   ngal    Number of halos in the array
 * @return  0 on success, -1 on error (including a refused creation)
 */
int test_fixture_process(struct ModuleContext *ctx, struct Halo *halos, int ngal) {
  if (halos == NULL || ngal <= 0) {
    return 0; // Nothing to process
  }

  execution_count++;

  if (ENABLE_LOGGING) {
    // Log detailed execution information for test validation
    INFO_LOG("TEST_FIXTURE_EXEC: count=%d ngal=%d substep=%d/%d substep_dt=%.6e z=%.4f",
             execution_count, ngal, ctx->substep_number, ctx->num_substeps, ctx->substep_dt,
             ctx->redshift);
    if (ctx->active_event != NULL) {
      INFO_LOG("TEST_FIXTURE_EVENT: producer_module_id=%d event_id=%d "
               "source=%d target=%d value0=%.6e value1=%.6e",
               ctx->active_event->producer_module_id, ctx->active_event->event_id,
               ctx->active_event->source_index, ctx->active_event->target_index,
               ctx->active_event->value0, ctx->active_event->value1);
    }
  }

  for (int i = 0; i < ngal; i++) {
    if (halos[i].Type != 0) {
      continue;
    }

    if (halos[i].galaxy == NULL) {
      ERROR_LOG("Halo %d (Type=0) has NULL galaxy data", i);
      return -1;
    }

    /* Set dummy property — validates that the property system is wired correctly */
    halos[i].galaxy->TestDummyProperty = (float)DUMMY_PARAMETER;

    if (ENABLE_LOGGING) {
      DEBUG_LOG("  Halo %d: Set TestDummyProperty = %.3f", i, DUMMY_PARAMETER);
    }

    for (int n = 0; n < CREATE_RECORDS; n++) {
      struct Halo *created = NULL;
      if (module_create_record(ctx, i, &created) < 0) {
        ERROR_LOG("test_fixture: creating record %d on host %d (UniqueGalaxyID %lld) failed", n, i,
                  halos[i].UniqueGalaxyID);
        return -1;
      }
      created->galaxy->TestDummyProperty = (float)DUMMY_PARAMETER;
    }
    if (CREATE_RECORDS > 0) {
      INFO_LOG("TEST_FIXTURE_CREATE: host=%lld created=%d", halos[i].UniqueGalaxyID,
               CREATE_RECORDS);
    }
  }

  return 0;
}

/**
 * @brief   Return code of a snapshot call refused because DUMMY_PARAMETER is
 *          outside TestDummyProperty's declared [0, 1] range
 *
 * Distinct from the -1 of a contract violation, so callback-failure tests can
 * check that the core reports the module's own return code.
 */
#define TEST_FIXTURE_SNAPSHOT_RANGE_ERROR 2

/**
 * @brief   Process one borrowed snapshot population (snapshot callback family)
 *
 * Checks the borrowed-view contract (see struct Module.process_snapshot), then
 * sets TestDummyProperty = DUMMY_PARAMETER on every entry, of any Type, through
 * halos[i].galaxy, the only permitted write. When ENABLE_LOGGING=1 it logs one
 * TEST_FIXTURE_SNAPSHOT_EXEC marker per call, including the smallest and
 * largest TestDummyProperty it found before writing, so tests can see what an
 * earlier callback of the same phase wrote.
 *
 * A non-empty population with DUMMY_PARAMETER outside TestDummyProperty's
 * declared [0, 1] range is refused with TEST_FIXTURE_SNAPSHOT_RANGE_ERROR
 * before anything is written; that is the fixture's way to make a real run's
 * snapshot callback fail. An empty population has nothing to write and
 * succeeds.
 *
 * @param   ctx     Snapshot context
 * @param   halos   Borrowed snapshot population; may be NULL when count is 0
 * @param   count   Number of entries in halos
 * @return  0 on success, -1 if the caller violated the borrowed-view contract,
 *          TEST_FIXTURE_SNAPSHOT_RANGE_ERROR if the value to write is out of range
 */
int test_fixture_process_snapshot(const struct SnapshotContext *ctx, const struct Halo *halos,
                                  int64_t count) {
  if (ctx == NULL || count < 0) {
    ERROR_LOG("test_fixture: invalid snapshot call (ctx=%p, count=%lld)", (const void *)ctx,
              (long long)count);
    return -1;
  }
  if (count > 0 && halos == NULL) {
    ERROR_LOG("test_fixture: NULL population with count=%lld at snapshot %d", (long long)count,
              ctx->snapshot_number);
    return -1;
  }

  if (count > 0 && !(DUMMY_PARAMETER >= 0.0 && DUMMY_PARAMETER <= 1.0)) {
    ERROR_LOG("test_fixture: TestFixtureDummyParameter = %g is outside TestDummyProperty's [0, 1] "
              "range; refusing to write it at snapshot %d",
              DUMMY_PARAMETER, ctx->snapshot_number);
    return TEST_FIXTURE_SNAPSHOT_RANGE_ERROR;
  }

  snapshot_execution_count++;

  float seen_min = 0.0f;
  float seen_max = 0.0f;
  for (int64_t i = 0; i < count; i++) {
    if (halos[i].galaxy == NULL) {
      ERROR_LOG("test_fixture: snapshot entry %lld has NULL galaxy at snapshot %d", (long long)i,
                ctx->snapshot_number);
      return -1;
    }
    const float seen = halos[i].galaxy->TestDummyProperty;
    if (i == 0 || seen < seen_min) {
      seen_min = seen;
    }
    if (i == 0 || seen > seen_max) {
      seen_max = seen;
    }
    halos[i].galaxy->TestDummyProperty = (float)DUMMY_PARAMETER;
  }

  if (ENABLE_LOGGING) {
    INFO_LOG("TEST_FIXTURE_SNAPSHOT_EXEC: count=%d snapshot=%d n=%lld z=%.4f seen_min=%.4f "
             "seen_max=%.4f",
             snapshot_execution_count, ctx->snapshot_number, (long long)count, ctx->redshift,
             (double)seen_min, (double)seen_max);
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
    INFO_LOG("TEST_FIXTURE_SNAPSHOT_CLEANUP: total_snapshot_executions=%d",
             snapshot_execution_count);
  }
  execution_count = 0; /* reset for re-init safety */
  snapshot_execution_count = 0;
  return 0;
}
