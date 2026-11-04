/**
 * @file    test_unit_sage26_initialise_merger_clock.c
 * @brief   Unit tests for sage26_initialise_merger_clock: placement, parameter, transitions
 *
 * The module makes SAGE26's joining writes (R/src/core_build_model.c:284-338) from
 * each galaxy's PreviousType (recorded by sage26_record_transition_state) and the
 * Type the core assigned. These tests build FoF workspaces by hand; the bit-exact
 * comparison with SAGE26 is the oracle test, and the two-snapshot test through the
 * core's inheritance service is sage26_record_transition_state's unit test.
 *
 * Test cases:
 *   - test_placement: init() accepts one pre_timestep process_full_halo entry and
 *     rejects a substep-phase or post_timestep entry
 *   - test_merger_time_factor: absent means SAGE26's default 2.0; a run value
 *     scales the clock linearly; a non-positive value is rejected
 *   - test_central_clock_reset: a Type 0 gets MergTime 999.9f whatever it held
 *   - test_type0_to_type1: infall state recorded, TimeOfInfall the snapshot, clock
 *     estimated from the fresh FoF-central virial values
 *   - test_type1_to_type1: an estimated clock and the infall state are kept; a
 *     clock above 999 is re-estimated
 *   - test_type0_to_type2: clock 0, infallStellarMass recorded, TimeOfInfall kept
 *   - test_type1_to_type2: the clock and infall state are kept; a clock above 999
 *     becomes 0
 *   - test_type2_untouched: a galaxy that was already Type 2 is not changed
 *   - test_without_recorder: with no previous Type (-1) the clock half still runs
 *   - test_history_reset: the six SFR histories are zeroed for a row that stays a
 *     Type 0 or 1 and kept for a new or old orphan (R/src/core_build_model.c:252,
 *     :278-282, :322-337)
 *   - test_missing_central: a satellite needing a clock without a Type 0 is an error
 *   - test_empty_and_null_rows: empty FoFs and rows without a galaxy
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../../../tests/framework/test_framework.h"
#include "../../../../tests/framework/test_phase_config.h"

#include "core/module_interface.h"
#include "core/module_registry.h"
#include "include/globals.h"
#include "include/types.h"
#include "util/error.h"
#include "util/memory.h"

#include "modules/_tests/sage_test_fixtures.h"
#include "shared/sage_constants.h"
#include "shared/sage_merger_clock.h"
#include "shared/sage_sfr_history.h"

extern int sage26_initialise_merger_clock_init(void);
extern int sage26_initialise_merger_clock_process(struct ModuleContext *ctx, struct Halo *halos,
                                                  int ngal);
extern int sage26_initialise_merger_clock_cleanup(void);

static const char *CLOCK = "sage26_initialise_merger_clock";

#define SNAPSHOT 41
#define REDSHIFT 0.98870811532120606

// ============================================================================
// FIXTURES
// ============================================================================

static void set_cosmology(void) {
  MimicConfig.PartMass = 0.086;
  MimicConfig.Hubble = 100.0;
  MimicConfig.Omega = 0.25;
  MimicConfig.OmegaLambda = 0.75;
  MimicConfig.G = 43.00707785642063;
}

/** @brief Configure the clock in pre_timestep (optionally with a MergerTimeFactor) and init it */
static int init_clock(const char *merger_time_factor) {
  reset_config();
  init_memory_system(0);
  set_cosmology();
  test_pre_timestep_add(CLOCK, PROCESSING_MODE_FULL_HALO);
  if (merger_time_factor != NULL) {
    snprintf(MimicConfig.ModelParams[0].param_name, MAX_STRING_LEN, "MergerTimeFactor");
    snprintf(MimicConfig.ModelParams[0].value, MAX_STRING_LEN, "%s", merger_time_factor);
    MimicConfig.NumModelParams = 1;
  }
  return sage26_initialise_merger_clock_init();
}

static void finish_clock(void) {
  sage26_initialise_merger_clock_cleanup();
  test_free_pre_timestep();
  test_free_substep_phases();
  check_memory_leaks();
}

static struct ModuleContext make_context(void) {
  struct ModuleContext ctx;
  memset(&ctx, 0, sizeof(ctx));
  ctx.params = &MimicConfig;
  ctx.snapshot_number = SNAPSHOT;
  ctx.redshift = REDSHIFT;
  ctx.num_substeps = 1;
  return ctx;
}

/** @brief A joined row: the core's Type and halo values, the galaxy's previous Type */
static void make_row(struct Halo *halo, struct GalaxyData *galaxy, const long long halo_nr,
                     const int type, const int previous_type, const int len, const double mvir,
                     const float mergtime) {
  memset(halo, 0, sizeof(*halo));
  init_galaxy_defaults(galaxy);
  halo->galaxy = galaxy;
  halo->HaloNr = halo_nr;
  halo->Type = type;
  halo->Len = len;
  halo->Mvir = mvir;
  /* Stored values the clock must not use (the core freezes them while a halo shrinks) */
  halo->Rvir = 123.0;
  halo->Vvir = 4567.0;
  galaxy->PreviousType = previous_type;
  galaxy->MergTime = mergtime;
  galaxy->StellarMass = 0.8f;
  galaxy->ColdGas = 0.3f;
  galaxy->infallStellarMassState = 0.6f;
  galaxy->TimeOfInfallState = 33.0f;
}

/** @brief A FoF central (Type 0) at row 0 and one satellite row at row 1 */
static void make_pair(struct Halo rows[2], struct GalaxyData galaxies[2], const int sat_type,
                      const int sat_previous_type, const float sat_mergtime) {
  make_row(&rows[0], &galaxies[0], 100, 0, 0, 1200, 103.2, SAGE_MERGTIME_UNSET);
  make_row(&rows[1], &galaxies[1], 101, sat_type, sat_previous_type, 30, 30 * 0.086, sat_mergtime);
}

/** @brief The clock SAGE26's estimate_merging_time gives the pair */
static double expected_clock(struct Halo rows[2], const double factor) {
  return mimic_sage_estimate_merging_time(&rows[1], &rows[0], REDSHIFT, factor, MimicConfig.G,
                                          NULL);
}

// ============================================================================
// PLACEMENT AND PARAMETER
// ============================================================================

/**
 * @test    test_placement
 * @brief   One pre_timestep process_full_halo entry initialises; other phases are rejected
 */
static int test_placement(void) {
  TEST_ASSERT(init_clock(NULL) == 0, "a pre_timestep entry initialises");
  finish_clock();

  reset_config();
  init_memory_system(0);
  test_phase_add("galaxy_physics", CLOCK, PROCESSING_MODE_FULL_HALO);
  TEST_ASSERT(sage26_initialise_merger_clock_init() != 0, "a substep-phase entry is rejected");
  test_free_substep_phases();

  test_pre_timestep_add(CLOCK, PROCESSING_MODE_FULL_HALO);
  test_pre_timestep_add(CLOCK, PROCESSING_MODE_FULL_HALO);
  TEST_ASSERT(sage26_initialise_merger_clock_init() != 0, "two pre_timestep entries are rejected");
  test_free_pre_timestep();

  TEST_ASSERT(sage26_initialise_merger_clock_init() != 0, "no pre_timestep entry is rejected");
  check_memory_leaks();
  return TEST_PASS;
}

/**
 * @test    test_merger_time_factor
 * @brief   Absent: SAGE26's default 2.0; present: the clock scales with it; <= 0 rejected
 */
static int test_merger_time_factor(void) {
  struct Halo rows[2];
  struct GalaxyData galaxies[2];
  struct ModuleContext ctx = make_context();

  TEST_ASSERT(init_clock(NULL) == 0, "init without MergerTimeFactor succeeds");
  make_pair(rows, galaxies, 1, 0, SAGE_MERGTIME_UNSET);
  TEST_ASSERT(sage26_initialise_merger_clock_process(&ctx, rows, 2) == 0, "process succeeds");
  const float default_clock = galaxies[1].MergTime;
  TEST_ASSERT(default_clock == (float)expected_clock(rows, 2.0),
              "without the parameter the clock uses SAGE26's default factor 2.0");
  finish_clock();

  TEST_ASSERT(init_clock("3.0") == 0, "init with MergerTimeFactor 3.0 succeeds");
  make_pair(rows, galaxies, 1, 0, SAGE_MERGTIME_UNSET);
  TEST_ASSERT(sage26_initialise_merger_clock_process(&ctx, rows, 2) == 0, "process succeeds");
  TEST_ASSERT(galaxies[1].MergTime == (float)expected_clock(rows, 3.0),
              "the clock uses the run's MergerTimeFactor");
  TEST_ASSERT(fabs(galaxies[1].MergTime / default_clock - 1.5) < 1e-6,
              "the clock scales linearly with MergerTimeFactor");
  finish_clock();

  TEST_ASSERT(init_clock("0.0") != 0, "a MergerTimeFactor of 0 is rejected");
  finish_clock();
  return TEST_PASS;
}

// ============================================================================
// TRANSITIONS
// ============================================================================

/**
 * @test    test_central_clock_reset
 * @brief   A Type 0 central gets MergTime 999.9f (R/src/core_build_model.c:288)
 */
static int test_central_clock_reset(void) {
  TEST_ASSERT(init_clock(NULL) == 0, "init succeeds");
  struct ModuleContext ctx = make_context();
  struct Halo rows[1];
  struct GalaxyData galaxies[1];
  const int previous[] = {0, 1, -1};
  for (int k = 0; k < 3; k++) {
    make_row(&rows[0], &galaxies[0], 100, 0, previous[k], 1200, 103.2, 2.5f);
    TEST_ASSERT(sage26_initialise_merger_clock_process(&ctx, rows, 1) == 0, "process succeeds");
    TEST_ASSERT(galaxies[0].MergTime == SAGE_MERGTIME_UNSET, "a central's clock is 999.9f");
    TEST_ASSERT(galaxies[0].infallStellarMassState == 0.6f &&
                    galaxies[0].TimeOfInfallState == 33.0f,
                "a central keeps its infall state");
  }
  finish_clock();
  return TEST_PASS;
}

/**
 * @test    test_type0_to_type1
 * @brief   Infall recorded with the snapshot number; clock from the fresh FoF virial values
 */
static int test_type0_to_type1(void) {
  TEST_ASSERT(init_clock(NULL) == 0, "init succeeds");
  struct ModuleContext ctx = make_context();
  struct Halo rows[2];
  struct GalaxyData galaxies[2];
  make_pair(rows, galaxies, 1, 0, SAGE_MERGTIME_UNSET);

  TEST_ASSERT(sage26_initialise_merger_clock_process(&ctx, rows, 2) == 0, "process succeeds");
  TEST_ASSERT(galaxies[1].infallStellarMassState == 0.8f, "infallStellarMass = StellarMass");
  TEST_ASSERT(galaxies[1].TimeOfInfallState == (float)SNAPSHOT, "TimeOfInfall = the snapshot");

  /* The FoF central's virial radius and velocity from its Mvir at this redshift
   * (SAGE26's get_virial_radius/velocity), not its stored Rvir/Vvir */
  const double radius = virial_radius_for_mass(103.2, REDSHIFT);
  const double velocity = virial_velocity_for(103.2, radius);
  const double coulomb = log1p(1200 / (double)30);
  const double mass = 30 * 0.086 + 0.8f + 0.3f;
  const double expected =
      2.0 * 1.17 * radius * radius * velocity / (coulomb * MimicConfig.G * mass);
  TEST_ASSERT(fabs(galaxies[1].MergTime / expected - 1.0) < 1e-6,
              "the clock is SAGE26's dynamical-friction time from fresh virial values");
  TEST_ASSERT(galaxies[1].MergTime == (float)expected_clock(rows, 2.0),
              "the module stores the shared estimate as a float");
  finish_clock();
  return TEST_PASS;
}

/**
 * @test    test_type1_to_type1
 * @brief   An estimated clock is kept; a clock above 999 is re-estimated; infall kept
 */
static int test_type1_to_type1(void) {
  TEST_ASSERT(init_clock(NULL) == 0, "init succeeds");
  struct ModuleContext ctx = make_context();
  struct Halo rows[2];
  struct GalaxyData galaxies[2];

  make_pair(rows, galaxies, 1, 1, 0.75f);
  TEST_ASSERT(sage26_initialise_merger_clock_process(&ctx, rows, 2) == 0, "process succeeds");
  TEST_ASSERT(galaxies[1].MergTime == 0.75f, "an estimated clock is kept");
  TEST_ASSERT(galaxies[1].infallStellarMassState == 0.6f && galaxies[1].TimeOfInfallState == 33.0f,
              "the infall state is kept");

  make_pair(rows, galaxies, 1, 1, 999.0f);
  TEST_ASSERT(sage26_initialise_merger_clock_process(&ctx, rows, 2) == 0, "process succeeds");
  TEST_ASSERT(galaxies[1].MergTime == 999.0f, "a clock of exactly 999 is kept (strict >)");

  make_pair(rows, galaxies, 1, 1, SAGE_MERGTIME_UNSET);
  TEST_ASSERT(sage26_initialise_merger_clock_process(&ctx, rows, 2) == 0, "process succeeds");
  TEST_ASSERT(galaxies[1].MergTime == (float)expected_clock(rows, 2.0),
              "a clock above 999 is re-estimated");
  TEST_ASSERT(galaxies[1].infallStellarMassState == 0.6f && galaxies[1].TimeOfInfallState == 33.0f,
              "re-estimating does not record infall");
  finish_clock();
  return TEST_PASS;
}

/**
 * @test    test_type0_to_type2
 * @brief   A central losing its subhalo: clock 0, infallStellarMass recorded, TimeOfInfall kept
 */
static int test_type0_to_type2(void) {
  TEST_ASSERT(init_clock(NULL) == 0, "init succeeds");
  struct ModuleContext ctx = make_context();
  struct Halo rows[2];
  struct GalaxyData galaxies[2];
  make_pair(rows, galaxies, 2, 0, SAGE_MERGTIME_UNSET);
  rows[1].Mvir = 0.0;

  TEST_ASSERT(sage26_initialise_merger_clock_process(&ctx, rows, 2) == 0, "process succeeds");
  TEST_ASSERT(galaxies[1].MergTime == 0.0f, "the orphan's clock starts at 0");
  TEST_ASSERT(galaxies[1].infallStellarMassState == 0.8f, "infallStellarMass = StellarMass");
  TEST_ASSERT(galaxies[1].TimeOfInfallState == 33.0f, "TimeOfInfall keeps its earlier value");
  finish_clock();
  return TEST_PASS;
}

/**
 * @test    test_type1_to_type2
 * @brief   A satellite losing its subhalo keeps its clock; an unset clock becomes 0
 */
static int test_type1_to_type2(void) {
  TEST_ASSERT(init_clock(NULL) == 0, "init succeeds");
  struct ModuleContext ctx = make_context();
  struct Halo rows[2];
  struct GalaxyData galaxies[2];

  make_pair(rows, galaxies, 2, 1, 1.25f);
  TEST_ASSERT(sage26_initialise_merger_clock_process(&ctx, rows, 2) == 0, "process succeeds");
  TEST_ASSERT(galaxies[1].MergTime == 1.25f, "the clock is kept");
  TEST_ASSERT(galaxies[1].infallStellarMassState == 0.6f, "the infall state is kept");

  make_pair(rows, galaxies, 2, 1, SAGE_MERGTIME_UNSET);
  TEST_ASSERT(sage26_initialise_merger_clock_process(&ctx, rows, 2) == 0, "process succeeds");
  TEST_ASSERT(galaxies[1].MergTime == 0.0f, "an unset clock becomes 0");
  TEST_ASSERT(galaxies[1].infallStellarMassState == 0.8f, "and infallStellarMass is recorded");
  finish_clock();
  return TEST_PASS;
}

/**
 * @test    test_type2_untouched
 * @brief   A galaxy that was already Type 2 is copied unchanged (R/src/core_build_model.c:240)
 */
static int test_type2_untouched(void) {
  TEST_ASSERT(init_clock(NULL) == 0, "init succeeds");
  struct ModuleContext ctx = make_context();
  struct Halo rows[2];
  struct GalaxyData galaxies[2];
  make_pair(rows, galaxies, 2, 2, SAGE_MERGTIME_UNSET);

  TEST_ASSERT(sage26_initialise_merger_clock_process(&ctx, rows, 2) == 0, "process succeeds");
  TEST_ASSERT(galaxies[1].MergTime == SAGE_MERGTIME_UNSET, "the clock is not changed");
  TEST_ASSERT(galaxies[1].infallStellarMassState == 0.6f, "the infall state is not changed");
  finish_clock();
  return TEST_PASS;
}

/**
 * @test    test_without_recorder
 * @brief   With no previous Type (-1), the clock half of each condition still runs
 */
static int test_without_recorder(void) {
  TEST_ASSERT(init_clock(NULL) == 0, "init succeeds without the recorder (with a warning)");
  struct ModuleContext ctx = make_context();
  struct Halo rows[2];
  struct GalaxyData galaxies[2];

  make_pair(rows, galaxies, 1, -1, SAGE_MERGTIME_UNSET);
  TEST_ASSERT(sage26_initialise_merger_clock_process(&ctx, rows, 2) == 0, "process succeeds");
  TEST_ASSERT(galaxies[1].MergTime == (float)expected_clock(rows, 2.0),
              "an unset satellite clock is estimated");
  TEST_ASSERT(galaxies[1].TimeOfInfallState == 33.0f, "but no infall is recorded");

  make_pair(rows, galaxies, 2, -1, SAGE_MERGTIME_UNSET);
  TEST_ASSERT(sage26_initialise_merger_clock_process(&ctx, rows, 2) == 0, "process succeeds");
  TEST_ASSERT(galaxies[1].MergTime == 0.0f, "an orphan with an unset clock starts at 0");
  finish_clock();
  return TEST_PASS;
}

/** @brief Give every history element of a galaxy a distinct non-zero value */
static void fill_histories(struct GalaxyData *galaxy) {
  for (int h = 0; h < MIMIC_SFR_HISTORY_COUNT; h++) {
    for (int bin = 0; bin < MIMIC_SAGE_SFR_STEPS; bin++) {
      *mimic_sage_sfr_history(galaxy, (enum MimicSageSfrHistory)h, bin) =
          1.0f + (float)(h * MIMIC_SAGE_SFR_STEPS + bin);
    }
  }
}

/** @brief 1 if every history element of a galaxy is zero, 0 if every one keeps its fill, else -1 */
static int histories_state(struct GalaxyData *galaxy) {
  int zero = 0, kept = 0;
  for (int h = 0; h < MIMIC_SFR_HISTORY_COUNT; h++) {
    for (int bin = 0; bin < MIMIC_SAGE_SFR_STEPS; bin++) {
      const float value = *mimic_sage_sfr_history(galaxy, (enum MimicSageSfrHistory)h, bin);
      zero += (value == 0.0f);
      kept += (value == 1.0f + (float)(h * MIMIC_SAGE_SFR_STEPS + bin));
    }
  }
  const int elements = MIMIC_SFR_HISTORY_COUNT * MIMIC_SAGE_SFR_STEPS;
  return zero == elements ? 1 : (kept == elements ? 0 : -1);
}

/**
 * @test    test_history_reset
 * @brief   The histories are zeroed exactly for the rows SAGE26 resets
 *
 * SAGE26 zeroes the six histories only on the first-occupied update of a galaxy that
 * was a Type 0 or 1 (R/src/core_build_model.c:252, :278-282), the rows that stay a Type
 * 0 or 1; a galaxy that becomes an orphan keeps them (:322-337), and a Type 2 is copied
 * unchanged (:240). The reset reads the new Type alone, so a run without the recorder
 * (PreviousType -1) resets the same rows.
 */
static int test_history_reset(void) {
  TEST_ASSERT(init_clock(NULL) == 0, "init succeeds");
  struct ModuleContext ctx = make_context();
  const struct {
    int type;
    int previous_type;
    int zeroed;
    const char *what;
  } cases[] = {
      {1, 0, 1, "a Type 0 that becomes a Type 1 is reset"},
      {1, 1, 1, "a Type 1 that stays a Type 1 is reset"},
      {2, 0, 0, "a Type 0 that becomes an orphan keeps its histories"},
      {2, 1, 0, "a Type 1 that becomes an orphan keeps its histories"},
      {2, 2, 0, "an orphan that stays an orphan keeps its histories"},
      {1, -1, 1, "without the recorder a Type 1 is still reset"},
      {2, -1, 0, "without the recorder an orphan still keeps its histories"},
  };
  for (size_t k = 0; k < sizeof(cases) / sizeof(cases[0]); k++) {
    struct Halo rows[2];
    struct GalaxyData galaxies[2];
    make_pair(rows, galaxies, cases[k].type, cases[k].previous_type, 0.5f);
    fill_histories(&galaxies[0]);
    fill_histories(&galaxies[1]);
    TEST_ASSERT(sage26_initialise_merger_clock_process(&ctx, rows, 2) == 0, "process succeeds");
    TEST_ASSERT(histories_state(&galaxies[0]) == 1, "the FoF central (Type 0 from 0) is reset");
    TEST_ASSERT(histories_state(&galaxies[1]) == (cases[k].zeroed ? 1 : 0), cases[k].what);
  }
  finish_clock();
  return TEST_PASS;
}

/**
 * @test    test_missing_central
 * @brief   A satellite that needs a clock in a FoF without a Type 0 is an error
 */
static int test_missing_central(void) {
  TEST_ASSERT(init_clock(NULL) == 0, "init succeeds");
  struct ModuleContext ctx = make_context();
  struct Halo rows[1];
  struct GalaxyData galaxies[1];
  make_row(&rows[0], &galaxies[0], 101, 1, 0, 30, 2.58, SAGE_MERGTIME_UNSET);
  TEST_ASSERT(sage26_initialise_merger_clock_process(&ctx, rows, 1) != 0,
              "a new satellite without a FoF central fails");
  finish_clock();
  return TEST_PASS;
}

/**
 * @test    test_empty_and_null_rows
 * @brief   Empty FoFs succeed; rows without a galaxy are skipped; a NULL context fails
 */
static int test_empty_and_null_rows(void) {
  TEST_ASSERT(init_clock(NULL) == 0, "init succeeds");
  struct ModuleContext ctx = make_context();
  TEST_ASSERT(sage26_initialise_merger_clock_process(&ctx, NULL, 0) == 0, "NULL halos");
  struct Halo rows[2];
  struct GalaxyData galaxies[2];
  make_pair(rows, galaxies, 1, 0, SAGE_MERGTIME_UNSET);
  TEST_ASSERT(sage26_initialise_merger_clock_process(&ctx, rows, 0) == 0, "ngal 0");
  rows[1].galaxy = NULL;
  TEST_ASSERT(sage26_initialise_merger_clock_process(&ctx, rows, 2) == 0, "a NULL galaxy row");
  TEST_ASSERT(sage26_initialise_merger_clock_process(NULL, rows, 2) != 0, "a NULL context");
  finish_clock();
  return TEST_PASS;
}

// ============================================================================
// MAIN TEST RUNNER
// ============================================================================

int main(void) {
  printf("%s", BLUE);
  printf("============================================================\n");
  printf("Test Suite: sage26_initialise_merger_clock\n");
  printf("============================================================\n");
  printf("%s", NC);

  initialize_error_handling(LOG_LEVEL_ERROR, NULL);

  TEST_RUN(test_placement);
  TEST_RUN(test_merger_time_factor);
  TEST_RUN(test_central_clock_reset);
  TEST_RUN(test_type0_to_type1);
  TEST_RUN(test_type1_to_type1);
  TEST_RUN(test_type0_to_type2);
  TEST_RUN(test_type1_to_type2);
  TEST_RUN(test_type2_untouched);
  TEST_RUN(test_without_recorder);
  TEST_RUN(test_history_reset);
  TEST_RUN(test_missing_central);
  TEST_RUN(test_empty_and_null_rows);

  TEST_SUMMARY();
  return TEST_RESULT();
}
