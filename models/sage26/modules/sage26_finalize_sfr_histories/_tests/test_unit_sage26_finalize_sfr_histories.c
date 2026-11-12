/**
 * @file    test_unit_sage26_finalize_sfr_histories.c
 * @brief   Unit tests for sage26_finalize_sfr_histories
 *
 * Validates: SubstepsUsed recorded on every galaxy row of the FoF (merged rows included, as
 * SAGE26 does), and the writer's averaging of the histories into the four output scalars on
 * hand-checkable values.
 *
 * Test cases:
 *   - test_substeps_used_on_every_row: every galaxy row, merged rows included, records
 *     the halo's substep count; rows without a galaxy are skipped
 *   - test_rate_average: SfrDisk and SfrBulge are the mean per-substep rate in Msun/yr
 *   - test_metallicity_skips_nonpositive_gas: SfrDiskZ and SfrBulgeZ add metals / gas
 *     only for bins whose gas is positive, over min(SubstepsUsed, STEPS)
 *   - test_other_fields_untouched: the histories and the other outputs are not changed
 *   - test_invalid_context: a missing context, parameters or substep count is an error;
 *     NULL halos or ngal 0 is a no-op
 *
 * The module's placement (post_timestep, once, as process_full_halo) and its requirement of
 * sage26_initialise_merger_clock in pre_timestep as process_full_halo are refused, with
 * diagnostics, by test_unit_sage_dependency_contracts.c. The divisor a zero substep count falls
 * back to is asserted in shared/_tests/test_unit_sage_sfr_history.c.
 */

#include "../../../../tests/framework/test_framework.h"
#include "core/module_interface.h"
#include "include/globals.h"
#include "include/proto.h"
#include "include/types.h"
#include "util/error.h"
#include "util/memory.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "modules/_tests/sage_test_fixtures.h"
#include "shared/sage_sfr_history.h"

extern int sage26_finalize_sfr_histories_process(struct ModuleContext *ctx, struct Halo *halos,
                                                 int ngal);

#define NROWS 4

static struct GalaxyData galaxies[NROWS];
static struct Halo halos[NROWS];
static struct ModuleContext ctx;

/* SAGE26's default units: 1e10 Msun, 1 Mpc / (1 km/s) */
#define UNIT_MASS_IN_G 1.989e43
#define UNIT_TIME_IN_S (3.08568e24 / 1.0e5)

/** @brief Set every element of one history of a galaxy to value */
static void fill_history(struct GalaxyData *gal, const enum MimicSageSfrHistory h,
                         const float value) {
  for (int k = 0; k < MIMIC_SAGE_SFR_STEPS; k++) {
    *mimic_sage_sfr_history(gal, h, k) = value;
  }
}

/** @brief NROWS rows (Type 0 then Type 1) with zero histories, ten substeps */
static void setup_rows(void) {
  memset(galaxies, 0, sizeof(galaxies));
  memset(halos, 0, sizeof(halos));
  memset(&ctx, 0, sizeof(ctx));
  reset_config();
  MimicConfig.UnitMass_in_g = UNIT_MASS_IN_G;
  MimicConfig.UnitTime_in_s = UNIT_TIME_IN_S;
  ctx.params = &MimicConfig;
  ctx.num_substeps = 10;
  for (int i = 0; i < NROWS; i++) {
    halos[i].galaxy = &galaxies[i];
    halos[i].Type = (i == 0) ? 0 : 1;
    galaxies[i].SubstepsUsed = -7; /* the creation value is overwritten */
  }
}

/** @brief The Msun/yr conversion of one code-unit rate, left to right as SAGE26's writer */
static double to_msun_per_yr(const double rate) {
  return rate * UNIT_MASS_IN_G / UNIT_TIME_IN_S * MIMIC_SAGE_SEC_PER_YEAR / MIMIC_SAGE_SOLAR_MASS;
}

/**
 * @test    test_substeps_used_on_every_row
 * @brief   SubstepsUsed = the halo's substep count on every galaxy row, merged ones included
 */
int test_substeps_used_on_every_row(void) {
  setup_rows();
  ctx.num_substeps = 13;
  halos[2].Type = 3;       /* merged this snapshot: SAGE26 sets it too */
  halos[3].galaxy = NULL;  /* the core can keep a row whose galaxy it discarded */
  galaxies[3].SfrDisk = 5; /* ... which must not be touched */

  TEST_ASSERT(sage26_finalize_sfr_histories_process(&ctx, halos, NROWS) == 0,
              "process should succeed");
  for (int i = 0; i < 3; i++) {
    TEST_ASSERT(galaxies[i].SubstepsUsed == 13, "every galaxy row records the substep count");
  }
  TEST_ASSERT(galaxies[3].SubstepsUsed == -7 && galaxies[3].SfrDisk == 5.0f,
              "a galaxy detached from its row is not touched");
  return TEST_PASS;
}

/**
 * @test    test_rate_average
 * @brief   SfrDisk and SfrBulge are the bins' Msun/yr rates summed and divided by N
 */
int test_rate_average(void) {
  setup_rows();
  ctx.num_substeps = 5; /* bins 0, 2, 4, 6, 8 sampled */
  struct GalaxyData *g = &galaxies[0];
  for (int k = 0; k < MIMIC_SAGE_SFR_STEPS; k += 2) {
    *mimic_sage_sfr_history(g, MIMIC_SFR_HISTORY_DISK, k) = 4.0f;
    *mimic_sage_sfr_history(g, MIMIC_SFR_HISTORY_BULGE, k) = (k == 4) ? 10.0f : 0.0f;
  }

  TEST_ASSERT(sage26_finalize_sfr_histories_process(&ctx, halos, 1) == 0, "process should succeed");
  /* Five bins of rate 4 over five substeps: mean 4; one burst bin of 10: mean 2 */
  const double disk = to_msun_per_yr(4.0);
  const double bulge = to_msun_per_yr(2.0);
  TEST_ASSERT(fabs(g->SfrDisk - disk) <= 1e-6 * disk, "SfrDisk is the mean rate in Msun/yr");
  TEST_ASSERT(fabs(g->SfrBulge - bulge) <= 1e-6 * bulge, "SfrBulge is the mean rate in Msun/yr");
  TEST_ASSERT(g->SfrDiskZ == 0.0f && g->SfrBulgeZ == 0.0f,
              "no positive gas sample gives zero metallicities");
  return TEST_PASS;
}

/**
 * @test    test_metallicity_skips_nonpositive_gas
 * @brief   metals / gas is added only where gas > 0, and divided by min(N, STEPS)
 */
int test_metallicity_skips_nonpositive_gas(void) {
  setup_rows();
  ctx.num_substeps = 13; /* metallicity divisor 10 */
  struct GalaxyData *g = &galaxies[0];
  fill_history(g, MIMIC_SFR_HISTORY_DISK_COLD_GAS, 2.0f);
  fill_history(g, MIMIC_SFR_HISTORY_DISK_COLD_GAS_METALS, 0.04f); /* Z = 0.02 */
  *mimic_sage_sfr_history(g, MIMIC_SFR_HISTORY_DISK_COLD_GAS, 3) = 0.0f;
  *mimic_sage_sfr_history(g, MIMIC_SFR_HISTORY_DISK_COLD_GAS, 7) = -0.0f;
  *mimic_sage_sfr_history(g, MIMIC_SFR_HISTORY_BULGE_COLD_GAS, 1) = 1.0f;
  *mimic_sage_sfr_history(g, MIMIC_SFR_HISTORY_BULGE_COLD_GAS_METALS, 1) = 0.05f;
  *mimic_sage_sfr_history(g, MIMIC_SFR_HISTORY_BULGE_COLD_GAS, 2) = -1.0f;
  *mimic_sage_sfr_history(g, MIMIC_SFR_HISTORY_BULGE_COLD_GAS_METALS, 2) = 9.0f;

  TEST_ASSERT(sage26_finalize_sfr_histories_process(&ctx, halos, 1) == 0, "process should succeed");
  /* Eight positive disk bins of Z 0.02 over 10: 0.016; one bulge bin of 0.05 over 10 */
  TEST_ASSERT(fabsf(g->SfrDiskZ - 0.016f) <= 1e-6f, "SfrDiskZ skips the zero and -0 bins");
  TEST_ASSERT(fabsf(g->SfrBulgeZ - 0.005f) <= 1e-7f, "SfrBulgeZ skips the negative bin");
  return TEST_PASS;
}

/**
 * @test    test_other_fields_untouched
 * @brief   The histories and the other outputs are read or left, never changed
 */
int test_other_fields_untouched(void) {
  setup_rows();
  struct GalaxyData *g = &galaxies[0];
  for (int h = 0; h < MIMIC_SFR_HISTORY_COUNT; h++) {
    fill_history(g, (enum MimicSageSfrHistory)h, 0.5f + (float)h);
  }
  g->StarFormationRate = 3.0f;
  g->SupernovaOutflowRate = 4.0f;
  g->Cooling = 5.0;
  g->ColdGas = 6.0f;
  struct GalaxyData before;
  memcpy(&before, g, sizeof(before));

  TEST_ASSERT(sage26_finalize_sfr_histories_process(&ctx, halos, 1) == 0, "process should succeed");
  before.SubstepsUsed = g->SubstepsUsed;
  before.SfrDisk = g->SfrDisk;
  before.SfrBulge = g->SfrBulge;
  before.SfrDiskZ = g->SfrDiskZ;
  before.SfrBulgeZ = g->SfrBulgeZ;
  TEST_ASSERT(memcmp(&before, g, sizeof(before)) == 0,
              "only SubstepsUsed and the four averages may change");
  return TEST_PASS;
}

/**
 * @test    test_invalid_context
 * @brief   A missing context, parameters or substep count is an error; empty input is a no-op
 */
int test_invalid_context(void) {
  setup_rows();
  TEST_ASSERT(sage26_finalize_sfr_histories_process(&ctx, NULL, 0) == 0, "NULL halos is a no-op");
  TEST_ASSERT(sage26_finalize_sfr_histories_process(&ctx, halos, 0) == 0, "ngal 0 is a no-op");
  TEST_ASSERT(galaxies[0].SubstepsUsed == -7, "empty input changes nothing");

  TEST_ASSERT(sage26_finalize_sfr_histories_process(NULL, halos, 1) != 0, "NULL context fails");
  ctx.num_substeps = 0;
  TEST_ASSERT(sage26_finalize_sfr_histories_process(&ctx, halos, 1) != 0,
              "a zero substep count fails");
  ctx.num_substeps = 10;
  ctx.params = NULL;
  TEST_ASSERT(sage26_finalize_sfr_histories_process(&ctx, halos, 1) != 0,
              "missing parameters fail");
  return TEST_PASS;
}

int main(void) {
  printf("%s", BLUE);
  printf("============================================================\n");
  printf("Test Suite: sage26_finalize_sfr_histories Module\n");
  printf("============================================================\n");
  printf("%s\n", NC);

  initialize_error_handling(LOG_LEVEL_WARNING, NULL);

  TEST_RUN(test_substeps_used_on_every_row);
  TEST_RUN(test_rate_average);
  TEST_RUN(test_metallicity_skips_nonpositive_gas);
  TEST_RUN(test_other_fields_untouched);
  TEST_RUN(test_invalid_context);

  TEST_SUMMARY();
  return TEST_RESULT();
}
