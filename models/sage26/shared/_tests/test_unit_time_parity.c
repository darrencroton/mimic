/**
 * @file    test_unit_time_parity.c
 * @brief   Unit tests of the per-object substep timestep helper (shared/time_parity.h)
 *
 * mimic_object_substep_dt gives the substep dt, halo->dT / ctx->num_substeps, with SAGE's
 * strict boundary handling. The modules that call it turn a non-OK status into their own
 * invalid-timestep error or no-op, so the status each input maps to is the contract tested here.
 *
 * Every expected dt is a literal or the quotient written out from the row's own inputs, compared
 * exactly. Inputs that must not trap (a NULL argument) are rows too.
 *
 * Test cases:
 *   - test_substep_dt_table: one table of (dT, SnapNum, num_substeps) rows: OK with 1, 3 and
 *     more substeps (dT > 0 wins even at SnapNum < 0); SKIP_INITIAL for the boundary sentinel
 *     (SnapNum < 0 with dT <= 0); INVALID for dT <= 0 at SnapNum >= 0, a NaN dT, and a
 *     non-positive num_substeps (even for a boundary halo); dt_out is 0.0 for every non-OK row
 *   - test_substep_dt_null_arguments: a NULL halo, context or output returns INVALID without a
 *     trap, and dt_out is cleared whenever it is not NULL
 *   - test_status_strings: the name of each status; an unknown value reads as INVALID
 */

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "../../../../tests/framework/test_framework.h"

#include "include/types.h"
#include "shared/time_parity.h"

/* Test statistics (required for TEST_RUN macro) */
static int passed = 0;
static int failed = 0;

/** A value the helper must overwrite with 0.0 on every call that gets as far as writing dt_out */
#define DT_SENTINEL 99.0

/**
 * @test    test_substep_dt_table
 * @brief   Status and dt for each (dT, SnapNum, num_substeps) combination
 */
int test_substep_dt_table(void) {
  static const struct {
    const char *name;
    double dT;
    int snap_num;
    int num_substeps;
    enum MimicObjectTimeStatus status;
    double dt; /* the expected dt; 0.0 for every non-OK status */
  } rows[] = {
      {"one substep: dt is dT", 0.5, 10, 1, MIMIC_OBJECT_TIME_OK, 0.5},
      {"three substeps: dt is dT / 3 (a binary fraction here)", 0.75, 10, 3, MIMIC_OBJECT_TIME_OK,
       0.25},
      {"four substeps", 1.5, 10, 4, MIMIC_OBJECT_TIME_OK, 0.375},
      {"many substeps (30) with a non-binary dT", 0.1, 10, 30, MIMIC_OBJECT_TIME_OK, 0.1 / 30.0},
      {"dT > 0 at SnapNum < 0 is still OK", 0.5, -1, 2, MIMIC_OBJECT_TIME_OK, 0.25},
      {"SnapNum 0 with dT > 0 is OK", 0.5, 0, 2, MIMIC_OBJECT_TIME_OK, 0.25},
      {"boundary sentinel: SnapNum < 0 and dT == 0", 0.0, -1, 3, MIMIC_OBJECT_TIME_SKIP_INITIAL,
       0.0},
      {"boundary sentinel: SnapNum < 0 and dT < 0", -0.5, -1, 3, MIMIC_OBJECT_TIME_SKIP_INITIAL,
       0.0},
      {"boundary sentinel: any negative SnapNum", 0.0, -7, 1, MIMIC_OBJECT_TIME_SKIP_INITIAL, 0.0},
      {"dT == 0 at SnapNum 0 is invalid", 0.0, 0, 3, MIMIC_OBJECT_TIME_INVALID, 0.0},
      {"dT < 0 at SnapNum >= 0 is invalid", -1.0, 5, 3, MIMIC_OBJECT_TIME_INVALID, 0.0},
      {"a NaN dT at SnapNum < 0 is not the boundary sentinel", NAN, -1, 3,
       MIMIC_OBJECT_TIME_INVALID, 0.0},
      {"a NaN dT at SnapNum >= 0 is invalid", NAN, 5, 3, MIMIC_OBJECT_TIME_INVALID, 0.0},
      {"num_substeps 0 is invalid", 0.5, 10, 0, MIMIC_OBJECT_TIME_INVALID, 0.0},
      {"negative num_substeps is invalid", 0.5, 10, -2, MIMIC_OBJECT_TIME_INVALID, 0.0},
      {"non-positive num_substeps is invalid even at the boundary", 0.0, -1, 0,
       MIMIC_OBJECT_TIME_INVALID, 0.0},
  };

  for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
    struct Halo halo;
    struct ModuleContext ctx;
    memset(&halo, 0, sizeof(halo));
    memset(&ctx, 0, sizeof(ctx));
    halo.dT = rows[i].dT;
    halo.SnapNum = rows[i].snap_num;
    ctx.num_substeps = rows[i].num_substeps;

    double dt = DT_SENTINEL;
    const enum MimicObjectTimeStatus status = mimic_object_substep_dt(&halo, &ctx, &dt);
    TEST_ASSERT(status == rows[i].status, rows[i].name);
    TEST_ASSERT(dt == rows[i].dt, rows[i].name);
  }
  return TEST_PASS;
}

/**
 * @test    test_substep_dt_null_arguments
 * @brief   A NULL halo, context or output is INVALID; dt_out is cleared when it can be
 */
int test_substep_dt_null_arguments(void) {
  struct Halo halo;
  struct ModuleContext ctx;
  memset(&halo, 0, sizeof(halo));
  memset(&ctx, 0, sizeof(ctx));
  halo.dT = 0.5;
  halo.SnapNum = 10;
  ctx.num_substeps = 2;

  double dt = DT_SENTINEL;
  TEST_ASSERT(mimic_object_substep_dt(NULL, &ctx, &dt) == MIMIC_OBJECT_TIME_INVALID && dt == 0.0,
              "a NULL halo is invalid and clears dt_out");
  dt = DT_SENTINEL;
  TEST_ASSERT(mimic_object_substep_dt(&halo, NULL, &dt) == MIMIC_OBJECT_TIME_INVALID && dt == 0.0,
              "a NULL context is invalid and clears dt_out");
  TEST_ASSERT(mimic_object_substep_dt(&halo, &ctx, NULL) == MIMIC_OBJECT_TIME_INVALID,
              "a NULL dt_out is invalid");
  return TEST_PASS;
}

/**
 * @test    test_status_strings
 * @brief   Each status names itself; any other value reads as INVALID
 */
int test_status_strings(void) {
  TEST_ASSERT(strcmp(mimic_object_time_status_str(MIMIC_OBJECT_TIME_OK), "OK") == 0,
              "OK names itself");
  TEST_ASSERT(
      strcmp(mimic_object_time_status_str(MIMIC_OBJECT_TIME_SKIP_INITIAL), "SKIP_INITIAL") == 0,
      "SKIP_INITIAL names itself");
  TEST_ASSERT(strcmp(mimic_object_time_status_str(MIMIC_OBJECT_TIME_INVALID), "INVALID") == 0,
              "INVALID names itself");
  TEST_ASSERT(strcmp(mimic_object_time_status_str((enum MimicObjectTimeStatus)7), "INVALID") == 0,
              "an unknown status reads as INVALID");
  return TEST_PASS;
}

int main(void) {
  printf("============================================================\n");
  printf("UNIT TEST: Per-object substep dt (time_parity.h)\n");
  printf("============================================================\n");

  TEST_RUN(test_substep_dt_table);
  TEST_RUN(test_substep_dt_null_arguments);
  TEST_RUN(test_status_strings);

  TEST_SUMMARY();

  return TEST_RESULT();
}
