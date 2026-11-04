/**
 * @file    test_unit_sage26_finalize_sfr_histories_oracle.c
 * @brief   Bit-exact comparison of the SFR history substrate with SAGE26's driver and writer
 *
 * Runs every SAGE26 oracle fixture fixtures.manifest lists for
 * sage26_finalize_sfr_histories (its _tests/oracle/ directory). evolve_galaxies and
 * the HDF5 writer are not in the oracle's link, so the oracle records transcriptions
 * of their lines (oracle README). The fixtures name one of two routines, and the test
 * dispatches on that line:
 *
 * - step_bin_map (bins_es*): for effective_steps N and a snapshot interval deltaT,
 *   the substep interval dt = deltaT / N and the history bin of every substep,
 *   (step * STEPS) / N capped at STEPS - 1 (R/src/core_build_model.c:514-516). The
 *   package forms dt with mimic_object_substep_dt (a row's dT over the context's
 *   num_substeps, the core-owned count standing for N) and the bin with
 *   mimic_sage_sfr_step_bin (shared/sage_sfr_history.h); both are asserted.
 * - sfr_history_average (avg_*): one galaxy's six histories and SubstepsUsed, and the
 *   writer's averaging into SfrDisk, SfrBulge, SfrDiskZ and SfrBulgeZ with its two
 *   divisors and its skipped-bin counts (R/src/io/save_gals_hdf5.c:934-959,
 *   R/src/model_misc.h:49-58). The test asserts all eight through
 *   mimic_sage_average_sfr_histories, the helper the module calls. Where SubstepsUsed
 *   is positive it also runs the module's process() on the galaxy as a FoF of one with
 *   num_substeps = SubstepsUsed, and asserts that process() records SubstepsUsed and
 *   writes the same four outputs. SubstepsUsed 0 (avg_substeps0, SAGE26's divisor
 *   guard) has no process() counterpart: the package always has at least one substep.
 *
 * The writer's units are the run's: UnitMass_in_g and UnitTime_in_s, which the core
 * forms as SAGE26 does (UnitTime_in_s = UnitLength_in_cm / UnitVelocity_in_cm_per_s,
 * R/src/core_init.c:94). The fixtures give them as inputs, and the test supplies them
 * to the module through MimicConfig. Outside the oracle environment (macOS arm64,
 * Apple clang 21) the bit-exact test reports SKIP.
 *
 * Test cases:
 *   - test_oracle_fixture_count: every fixture the manifest lists is present
 *   - test_oracle_bit_identity: every fixture reproduces the oracle bit for bit
 */

#include "../../../../tests/framework/test_framework.h"
#include "core/module_interface.h"
#include "core/module_registry.h"
#include "include/globals.h"
#include "include/types.h"
#include "util/error.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "modules/_tests/sage_test_fixtures.h"
#include "modules/_tests/sage26_oracle_fixture.h"
#include "shared/sage_sfr_history.h"
#include "shared/time_parity.h"

extern int sage26_finalize_sfr_histories_process(struct ModuleContext *ctx, struct Halo *halos,
                                                 int ngal);

static const char *MODULE = "sage26_finalize_sfr_histories";

#define MAX_STEPS 64

/** @brief One step_bin_map fixture: the substep interval and every substep's bin */
static int check_step_bin_map(struct oracle_fixture *in, struct oracle_fixture *out) {
  int problems = 0;
  const int effective_steps = oracle_in_i32(in, "call.effective_steps");
  const double deltaT = oracle_in_f64(in, "call.deltaT");
  if (effective_steps < 1 || effective_steps > MAX_STEPS) {
    fprintf(stderr, "oracle: %s: call.effective_steps out of range\n", in->path);
    return problems + 1;
  }

  /* The core-owned substep interval: the row's dT over the context's substep count */
  struct Halo halo;
  memset(&halo, 0, sizeof(halo));
  halo.dT = deltaT;
  struct ModuleContext ctx;
  memset(&ctx, 0, sizeof(ctx));
  ctx.num_substeps = effective_steps;
  double dt = 0.0;
  if (mimic_object_substep_dt(&halo, &ctx, &dt) != MIMIC_OBJECT_TIME_OK) {
    fprintf(stderr, "oracle: %s: no substep interval\n", in->path);
    problems++;
  }
  problems += oracle_check_f64(out, "intermediate.dt", dt);

  for (int step = 0; step < effective_steps; step++) {
    char field[ORACLE_MAX_NAME];
    snprintf(field, sizeof(field), "intermediate.step[%d].step_bin", step);
    problems += oracle_check_i32(out, field, mimic_sage_sfr_step_bin(step, effective_steps));
  }
  return problems;
}

/** @brief One sfr_history_average fixture: the helper, and process() where it applies */
static int check_history_average(struct oracle_fixture *in, struct oracle_fixture *out) {
  int problems = 0;
  reset_config();
  MimicConfig.UnitMass_in_g = oracle_in_f64(in, "params.UnitMass_in_g");
  MimicConfig.UnitTime_in_s = oracle_in_f64(in, "params.UnitTime_in_s");

  static struct GalaxyData input;
  oracle_poison(&input, sizeof(input));
  input.SubstepsUsed = oracle_in_i32(in, "galaxy[0].SubstepsUsed");
  problems += oracle_c05_in_histories(in, 0, &input);

  /* The helper the module calls, on the fixture's SubstepsUsed */
  static struct GalaxyData averaged;
  memcpy(&averaged, &input, sizeof(averaged));
  struct MimicSageSfrAverageLocals locals;
  mimic_sage_average_sfr_histories(&averaged, MimicConfig.UnitMass_in_g, MimicConfig.UnitTime_in_s,
                                   &locals);
  problems += oracle_c05_check_writer(out, "intermediate.writer.", &averaged, &locals);

  /* The module's process(): a FoF of one whose substep count is SubstepsUsed */
  if (input.SubstepsUsed > 0) {
    static struct GalaxyData processed;
    memcpy(&processed, &input, sizeof(processed));
    processed.SubstepsUsed = -1; /* process() must record the count itself */
    struct Halo halo;
    memset(&halo, 0, sizeof(halo));
    halo.galaxy = &processed;
    struct ModuleContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.params = &MimicConfig;
    ctx.num_substeps = input.SubstepsUsed;
    if (sage26_finalize_sfr_histories_process(&ctx, &halo, 1) != 0) {
      fprintf(stderr, "oracle: %s: process() failed\n", in->path);
      problems++;
    }
    if (processed.SubstepsUsed != input.SubstepsUsed) {
      fprintf(stderr, "oracle: %s: process() recorded SubstepsUsed %d, not %d\n", in->path,
              processed.SubstepsUsed, input.SubstepsUsed);
      problems++;
    }
    const float got[4] = {processed.SfrDisk, processed.SfrBulge, processed.SfrDiskZ,
                          processed.SfrBulgeZ};
    const float want[4] = {averaged.SfrDisk, averaged.SfrBulge, averaged.SfrDiskZ,
                           averaged.SfrBulgeZ};
    for (int k = 0; k < 4; k++) {
      if (oracle_f32_to_bits(got[k]) != oracle_f32_to_bits(want[k])) {
        fprintf(stderr, "oracle: %s: process() output %d differs from the helper's\n", in->path, k);
        problems++;
      }
    }
    /* The histories are read, not changed */
    for (size_t h = 0; h < (size_t)MIMIC_SFR_HISTORY_COUNT; h++) {
      for (int k = 0; k < MIMIC_SAGE_SFR_STEPS; k++) {
        if (oracle_f32_to_bits(*oracle_c05_history_element(&processed, h, k)) !=
            oracle_f32_to_bits(*oracle_c05_history_element(&input, h, k))) {
          fprintf(stderr, "oracle: %s: process() changed a history element\n", in->path);
          problems++;
        }
      }
    }
  }
  return problems;
}

/** @brief Compare one fixture of either routine; returns the number of mismatches and errors */
static int check_fixture(const char *name) {
  static struct oracle_fixture in, out;
  char path[ORACLE_MAX_PATH];
  oracle_fixture_path(path, sizeof(path), MODULE, name, ".in");
  if (oracle_load(path, &in) != 0) {
    return 1;
  }
  const int average = strcmp(in.routine, "sfr_history_average") == 0;
  if (oracle_load_pair(MODULE, name, average ? "sfr_history_average" : "step_bin_map", &in, &out) !=
      0) {
    return 1;
  }
  int problems = average ? check_history_average(&in, &out) : check_step_bin_map(&in, &out);
  problems += oracle_report_unconsumed(&in);
  problems += oracle_report_unconsumed(&out);
  return problems;
}

/**
 * @test    test_oracle_fixture_count
 * @brief   Every fixture fixtures.manifest lists for the module is present (platform independent)
 */
int test_oracle_fixture_count(void) {
  TEST_ASSERT(oracle_fixture_count_ok(MODULE),
              "fixtures.manifest must list its declared count and every listed fixture must exist");
  return TEST_PASS;
}

/**
 * @test    test_oracle_bit_identity
 * @brief   Every fixture reproduces the SAGE26 oracle bit for bit (SKIP outside its environment)
 */
int test_oracle_bit_identity(void) {
  const char *mismatch = oracle_environment_mismatch();
  if (mismatch != NULL) {
    return TEST_SKIP_WITH(mismatch);
  }
  TEST_ASSERT(oracle_all_bit_identical(MODULE, check_fixture),
              "every fixture must reproduce the oracle bit for bit");
  return TEST_PASS;
}

int main(void) {
  printf("%s", BLUE);
  printf("============================================================\n");
  printf("Test Suite: sage26_finalize_sfr_histories SAGE26 oracle (bit-exact)\n");
  printf("============================================================\n");
  printf("%s", NC);

  initialize_error_handling(LOG_LEVEL_WARNING, NULL);

  TEST_RUN(test_oracle_fixture_count);
  TEST_RUN(test_oracle_bit_identity);

  TEST_SUMMARY();
  return TEST_RESULT();
}
