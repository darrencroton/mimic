/**
 * @file    test_unit_sage26_finalize_sfr_histories.c
 * @brief   Unit tests for sage26_finalize_sfr_histories
 *
 * Validates: module lifecycle and placement, SubstepsUsed recorded on every galaxy row
 * of the FoF (merged rows included, as SAGE26 does), and the writer's averaging of the
 * histories into the four output scalars on hand-checkable values.
 *
 * Test cases:
 *   - test_module_initialization: module init/cleanup lifecycle through the module system
 *   - test_placement_enforced: init() rejects a substep-phase, pre_timestep, missing or
 *     duplicated post_timestep entry (it must average once, after the substeps)
 *   - test_requires_merger_clock: init() rejects a configuration without
 *     sage26_initialise_merger_clock in pre_timestep as process_full_halo (the histories'
 *     only reset) and accepts one with it
 *   - test_substeps_used_on_every_row: every galaxy row, merged rows included, records
 *     the halo's substep count; rows without a galaxy are skipped
 *   - test_rate_average: SfrDisk and SfrBulge are the mean per-substep rate in Msun/yr
 *   - test_metallicity_skips_nonpositive_gas: SfrDiskZ and SfrBulgeZ add metals / gas
 *     only for bins whose gas is positive, over min(SubstepsUsed, STEPS)
 *   - test_divisors: SAGE26's divisors for 0, below, at and above STEPS substeps
 *   - test_other_fields_untouched: the histories and the other outputs are not changed
 *   - test_invalid_context: a missing context or substep count is an error
 *   - test_empty_input: NULL halos or ngal 0 is a no-op
 */

#include "../../../../tests/framework/test_framework.h"
#include "../../../../tests/framework/test_phase_config.h"
#include "core/module_interface.h"
#include "core/module_registry.h"
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

extern int sage26_finalize_sfr_histories_init(void);
extern int sage26_finalize_sfr_histories_process(struct ModuleContext *ctx, struct Halo *halos,
                                                 int ngal);
extern int sage26_finalize_sfr_histories_cleanup(void);

#define NROWS 4

static struct GalaxyData galaxies[NROWS];
static struct Halo halos[NROWS];
static struct ModuleContext ctx;

/* SAGE26's default units: 1e10 Msun, 1 Mpc / (1 km/s) */
#define UNIT_MASS_IN_G 1.989e43
#define UNIT_TIME_IN_S (3.08568e24 / 1.0e5)

/**
 * @brief Append one entry to MimicConfig.post_timestep
 *
 * Allocated as tests/framework/test_phase_config.h allocates the other phases
 * (mymalloc_cat array, strdup name), so module_system_cleanup() frees it.
 */
static void test_post_timestep_add(const char *module_name, enum ProcessingMode mode) {
  if (MimicConfig.post_timestep == NULL) {
    MimicConfig.post_timestep =
        mymalloc_cat(TEST_PHASE_MODULE_CAP * sizeof(struct PhaseModuleConfig), MEM_UTILITY);
    MimicConfig.num_post_timestep = 0;
  }
  const int i = MimicConfig.num_post_timestep++;
  MimicConfig.post_timestep[i].module_name = strdup(module_name);
  MimicConfig.post_timestep[i].processing_mode = mode;
  MimicConfig.post_timestep[i].resolved = NULL;
}

/** @brief Fresh configuration with the memory system and modules registered, no phases */
static void bare_config(void) {
  reset_config();
  init_memory_system(0);
  ensure_modules_registered();
  MimicConfig.SubSteps = 1;
  set_test_model_parameters();
}

/**
 * @brief Fresh configuration with sage26_initialise_merger_clock in pre_timestep
 *
 * The clock module owns the histories' reset at joining, which init() requires.
 */
static void fresh_config(void) {
  bare_config();
  test_pre_timestep_add("sage26_initialise_merger_clock", PROCESSING_MODE_FULL_HALO);
}

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
 * @test    test_module_initialization
 * @brief   The module initialises and cleans up through the module system in post_timestep
 */
int test_module_initialization(void) {
  fresh_config();
  test_post_timestep_add("sage26_finalize_sfr_histories", PROCESSING_MODE_FULL_HALO);

  const int result = module_system_init();
  TEST_ASSERT(result == 0, "module system initialization should succeed");

  module_system_cleanup();
  check_memory_leaks();
  return TEST_PASS;
}

/**
 * @test    test_placement_enforced
 * @brief   The module must run exactly once in post_timestep as process_full_halo
 */
int test_placement_enforced(void) {
  /* In a substep phase (as well as post_timestep) */
  fresh_config();
  test_post_timestep_add("sage26_finalize_sfr_histories", PROCESSING_MODE_FULL_HALO);
  test_phase_add("satellite_mergers", "sage26_finalize_sfr_histories", PROCESSING_MODE_FULL_HALO);
  TEST_ASSERT(module_system_init() != 0, "a substep-phase entry is rejected");
  module_system_cleanup();
  check_memory_leaks();

  /* In pre_timestep (as well as post_timestep) */
  fresh_config();
  test_post_timestep_add("sage26_finalize_sfr_histories", PROCESSING_MODE_FULL_HALO);
  test_pre_timestep_add("sage26_finalize_sfr_histories", PROCESSING_MODE_FULL_HALO);
  TEST_ASSERT(module_system_init() != 0, "a pre_timestep entry is rejected");
  module_system_cleanup();
  check_memory_leaks();

  /* Twice in post_timestep */
  fresh_config();
  test_post_timestep_add("sage26_finalize_sfr_histories", PROCESSING_MODE_FULL_HALO);
  test_post_timestep_add("sage26_finalize_sfr_histories", PROCESSING_MODE_FULL_HALO);
  TEST_ASSERT(module_system_init() != 0, "a second post_timestep entry is rejected");
  module_system_cleanup();
  check_memory_leaks();

  /* Beside sage26_finalize_cooling_heating, in either order */
  fresh_config();
  test_post_timestep_add("sage26_finalize_sfr_histories", PROCESSING_MODE_FULL_HALO);
  test_post_timestep_add("sage26_finalize_cooling_heating", PROCESSING_MODE_FULL_HALO);
  TEST_ASSERT(module_system_init() == 0, "the two finalisers are independent of their order");
  module_system_cleanup();
  check_memory_leaks();

  /* Not configured at all: a direct init() finds no post_timestep entry */
  reset_config();
  TEST_ASSERT(sage26_finalize_sfr_histories_init() != 0,
              "init fails without a post_timestep entry");
  return TEST_PASS;
}

/**
 * @test    test_requires_merger_clock
 * @brief   init() requires sage26_initialise_merger_clock in pre_timestep as process_full_halo
 *
 * The histories are inherited (no init_repeat); the clock module's reset at joining is
 * their only reset. Without it they accumulate across
 * snapshots, so the run must be rejected at init.
 */
int test_requires_merger_clock(void) {
  /* No clock module anywhere */
  bare_config();
  test_post_timestep_add("sage26_finalize_sfr_histories", PROCESSING_MODE_FULL_HALO);
  TEST_ASSERT(module_system_init() != 0, "a run without the clock module is rejected");
  module_system_cleanup();
  check_memory_leaks();

  /* The clock module in pre_timestep, but not as process_full_halo */
  bare_config();
  test_pre_timestep_add("sage26_initialise_merger_clock", PROCESSING_MODE_BY_GALAXY);
  test_post_timestep_add("sage26_finalize_sfr_histories", PROCESSING_MODE_FULL_HALO);
  TEST_ASSERT(sage26_finalize_sfr_histories_init() != 0,
              "the clock module must be a process_full_halo pre_timestep entry");
  module_system_cleanup();
  check_memory_leaks();

  /* The clock module in pre_timestep as process_full_halo */
  fresh_config();
  test_post_timestep_add("sage26_finalize_sfr_histories", PROCESSING_MODE_FULL_HALO);
  TEST_ASSERT(sage26_finalize_sfr_histories_init() == 0, "with the clock module init succeeds");
  TEST_ASSERT(module_system_init() == 0, "and the module system initialises");
  module_system_cleanup();
  check_memory_leaks();
  return TEST_PASS;
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
 * @test    test_divisors
 * @brief   SAGE26's sfr_rate_divisor and sfr_metallicity_divisor
 */
int test_divisors(void) {
  TEST_ASSERT(mimic_sage_sfr_rate_divisor(0) == 10 && mimic_sage_sfr_metallicity_divisor(0) == 10,
              "no substeps falls back to STEPS");
  TEST_ASSERT(mimic_sage_sfr_rate_divisor(7) == 7 && mimic_sage_sfr_metallicity_divisor(7) == 7,
              "fewer substeps than bins divide by the count");
  TEST_ASSERT(mimic_sage_sfr_rate_divisor(10) == 10 && mimic_sage_sfr_metallicity_divisor(10) == 10,
              "STEPS substeps divide by STEPS");
  TEST_ASSERT(mimic_sage_sfr_rate_divisor(30) == 30 && mimic_sage_sfr_metallicity_divisor(30) == 10,
              "more substeps than bins: rates by the count, metallicities by STEPS");
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
 * @brief   A missing context, parameters or substep count is an error
 */
int test_invalid_context(void) {
  setup_rows();
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

/**
 * @test    test_empty_input
 * @brief   NULL halos or ngal 0 is a no-op that succeeds
 */
int test_empty_input(void) {
  setup_rows();
  TEST_ASSERT(sage26_finalize_sfr_histories_process(&ctx, NULL, 0) == 0, "NULL halos is a no-op");
  TEST_ASSERT(sage26_finalize_sfr_histories_process(&ctx, halos, 0) == 0, "ngal 0 is a no-op");
  TEST_ASSERT(galaxies[0].SubstepsUsed == -7, "nothing is changed");
  return TEST_PASS;
}

int main(void) {
  printf("%s", BLUE);
  printf("============================================================\n");
  printf("Test Suite: sage26_finalize_sfr_histories Module\n");
  printf("============================================================\n");
  printf("%s\n", NC);

  initialize_error_handling(LOG_LEVEL_WARNING, NULL);

  TEST_RUN(test_module_initialization);
  TEST_RUN(test_placement_enforced);
  TEST_RUN(test_requires_merger_clock);
  TEST_RUN(test_substeps_used_on_every_row);
  TEST_RUN(test_rate_average);
  TEST_RUN(test_metallicity_skips_nonpositive_gas);
  TEST_RUN(test_divisors);
  TEST_RUN(test_other_fields_untouched);
  TEST_RUN(test_invalid_context);
  TEST_RUN(test_empty_input);

  TEST_SUMMARY();
  return TEST_RESULT();
}
