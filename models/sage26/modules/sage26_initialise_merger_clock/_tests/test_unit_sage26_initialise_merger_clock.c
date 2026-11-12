/**
 * @file    test_unit_sage26_initialise_merger_clock.c
 * @brief   Unit tests for sage26_initialise_merger_clock: parameter, transitions
 *
 * The module makes SAGE26's joining writes from each galaxy's PreviousType (recorded by
 * sage26_record_transition_state) and the Type the core assigned. These tests build FoF
 * workspaces by hand; the two-snapshot test through the core's inheritance service is
 * sage26_record_transition_state's unit test.
 *
 * Expected clocks come from an independent statement of SAGE26's dynamical-friction formula
 * (expected_clock), not from the shared helper the module calls; test_type0_to_type1 also pins
 * the helper's float store.
 *
 * Test cases:
 *   - test_merger_time_factor: absent means SAGE26's default 2.0; a run value
 *     scales the clock linearly
 *   - test_type0_to_type1: infall state recorded, TimeOfInfall the snapshot, clock
 *     estimated from the fresh FoF-central virial values
 *   - test_transitions: a table of (new Type, previous Type, clock held) rows giving the clock
 *     and the infall state afterwards: a central's reset, a kept or re-estimated satellite
 *     clock (strict > 999), the orphan transitions, an untouched orphan, and the rows without
 *     the recorder (previous Type -1)
 *   - test_history_reset: the six SFR histories are zeroed for a row that stays a
 *     Type 0 or 1 and kept for a new or old orphan
 *   - test_missing_central: a satellite needing a clock without a Type 0 is an error
 *   - test_empty_and_null_rows: empty FoFs succeed, a row without a galaxy is skipped and leaves
 *     its neighbours as they were, a NULL context fails
 *
 * The module's refused MergerTimeFactor values are in test_unit_sage26_parameter_refusals.c; its
 * placement (once, in pre_timestep) is refused, with diagnostics, by
 * test_unit_sage_dependency_contracts.c.
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

/**
 * @brief SAGE26's dynamical-friction clock for the pair, written out independently of the helper
 *
 * The FoF central's virial radius and velocity come from its Mvir at this redshift (not its
 * stored Rvir/Vvir); the satellite mass is its Mvir plus StellarMass and ColdGas.
 */
static double expected_clock(struct Halo rows[2], const double factor) {
  const double radius = virial_radius_for_mass(rows[0].Mvir, REDSHIFT);
  const double velocity = virial_velocity_for(rows[0].Mvir, radius);
  const double coulomb = log1p((double)rows[0].Len / (double)rows[1].Len);
  const double mass = rows[1].Mvir + rows[1].galaxy->StellarMass + rows[1].galaxy->ColdGas;
  return factor * 1.17 * radius * radius * velocity / (coulomb * MimicConfig.G * mass);
}

/** @brief 1 if a stored float clock matches the double expectation to within float rounding */
static int clock_matches(const float stored, const double expected) {
  return fabs(stored / expected - 1.0) < 1e-6;
}

// ============================================================================
// PLACEMENT AND PARAMETER
// ============================================================================

/**
 * @test    test_merger_time_factor
 * @brief   Absent: SAGE26's default 2.0; present: the clock scales with it
 */
static int test_merger_time_factor(void) {
  struct Halo rows[2];
  struct GalaxyData galaxies[2];
  struct ModuleContext ctx = make_context();

  TEST_ASSERT(init_clock(NULL) == 0, "init without MergerTimeFactor succeeds");
  make_pair(rows, galaxies, 1, 0, SAGE_MERGTIME_UNSET);
  TEST_ASSERT(sage26_initialise_merger_clock_process(&ctx, rows, 2) == 0, "process succeeds");
  const float default_clock = galaxies[1].MergTime;
  TEST_ASSERT(clock_matches(default_clock, expected_clock(rows, 2.0)),
              "without the parameter the clock uses SAGE26's default factor 2.0");
  finish_clock();

  TEST_ASSERT(init_clock("3.0") == 0, "init with MergerTimeFactor 3.0 succeeds");
  make_pair(rows, galaxies, 1, 0, SAGE_MERGTIME_UNSET);
  TEST_ASSERT(sage26_initialise_merger_clock_process(&ctx, rows, 2) == 0, "process succeeds");
  TEST_ASSERT(clock_matches(galaxies[1].MergTime, expected_clock(rows, 3.0)),
              "the clock uses the run's MergerTimeFactor");
  TEST_ASSERT(fabs(galaxies[1].MergTime / default_clock - 1.5) < 1e-6,
              "the clock scales linearly with MergerTimeFactor");
  finish_clock();
  return TEST_PASS;
}

// ============================================================================
// TRANSITIONS
// ============================================================================

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

  /* SAGE26's dynamical-friction time from the FoF central's fresh virial values, not its
   * stored Rvir/Vvir (expected_clock) */
  TEST_ASSERT(clock_matches(galaxies[1].MergTime, expected_clock(rows, 2.0)),
              "the clock is SAGE26's dynamical-friction time from fresh virial values");
  TEST_ASSERT(galaxies[1].MergTime == (float)mimic_sage_estimate_merging_time(
                                          &rows[1], &rows[0], REDSHIFT, 2.0, MimicConfig.G, NULL),
              "the module stores the shared estimate as a float");
  finish_clock();
  return TEST_PASS;
}

/**
 * @test    test_transitions
 * @brief   The clock and infall state after each joining transition, one row per transition
 *
 * Each row joins a satellite (or, for a new Type 0, the FoF central alone) with the given new
 * Type, previous Type and held clock, then checks the clock and the infall state that follow:
 *   - a central's clock is 999.9f whatever it held, its infall state untouched
 *   - Type 1 from Type 1 keeps a clock up to and including 999 (strict >) and re-estimates a
 *     larger one without recording infall
 *   - an orphan's clock starts at 0 when it was a Type 0 or held an unset clock, with
 *     infallStellarMass recorded and TimeOfInfall kept; otherwise it is kept, and an orphan that
 *     was already one is copied unchanged
 *   - with no previous Type (-1, no recorder) the clock half of each condition still runs
 * An estimated clock (NAN in the table) is compared with the independent formula.
 */
static int test_transitions(void) {
  TEST_ASSERT(init_clock(NULL) == 0, "init succeeds");
  struct ModuleContext ctx = make_context();
  const float kept_infall_mass = 0.6f, kept_time = 33.0f, recorded_infall_mass = 0.8f;
  /* An unchecked expectation: the infall stellar mass of an orphan without the recorder */
  const float unchecked = NAN;
  const struct {
    int type;
    int previous_type;
    float clock_in;
    float clock_out; /* NAN: the estimated clock */
    float infall_stellar_mass;
    float time_of_infall;
    const char *what;
  } rows_table[] = {
      {0, 0, 2.5f, SAGE_MERGTIME_UNSET, kept_infall_mass, kept_time, "a central that was a Type 0"},
      {0, 1, 2.5f, SAGE_MERGTIME_UNSET, kept_infall_mass, kept_time, "a central that was a Type 1"},
      {0, -1, 2.5f, SAGE_MERGTIME_UNSET, kept_infall_mass, kept_time, "a central, no recorder"},
      {1, 1, 0.75f, 0.75f, kept_infall_mass, kept_time,
       "Type 1 to Type 1 keeps an estimated clock"},
      {1, 1, 999.0f, 999.0f, kept_infall_mass, kept_time, "a clock of exactly 999 is kept"},
      {1, 1, SAGE_MERGTIME_UNSET, NAN, kept_infall_mass, kept_time,
       "a clock above 999 is re-estimated without recording infall"},
      {2, 0, SAGE_MERGTIME_UNSET, 0.0f, recorded_infall_mass, kept_time,
       "a Type 0 that becomes an orphan"},
      {2, 1, 1.25f, 1.25f, kept_infall_mass, kept_time, "a Type 1 that becomes an orphan"},
      {2, 1, SAGE_MERGTIME_UNSET, 0.0f, recorded_infall_mass, kept_time,
       "a Type 1 that becomes an orphan with an unset clock"},
      {2, 2, SAGE_MERGTIME_UNSET, SAGE_MERGTIME_UNSET, kept_infall_mass, kept_time,
       "an orphan that stays an orphan is copied unchanged"},
      {1, -1, SAGE_MERGTIME_UNSET, NAN, kept_infall_mass, kept_time,
       "an unset satellite clock is estimated without the recorder, and no infall is recorded"},
      {2, -1, SAGE_MERGTIME_UNSET, 0.0f, unchecked, kept_time,
       "an orphan with an unset clock starts at 0 without the recorder"},
  };

  for (size_t k = 0; k < sizeof(rows_table) / sizeof(rows_table[0]); k++) {
    struct Halo rows[2];
    struct GalaxyData galaxies[2];
    int ngal = 2, target = 1;
    char msg[160];
    if (rows_table[k].type == 0) {
      make_row(&rows[0], &galaxies[0], 100, 0, rows_table[k].previous_type, 1200, 103.2,
               rows_table[k].clock_in);
      ngal = 1;
      target = 0;
    } else {
      make_pair(rows, galaxies, rows_table[k].type, rows_table[k].previous_type,
                rows_table[k].clock_in);
      if (rows_table[k].type == 2) {
        rows[1].Mvir = 0.0; /* an orphan has lost its subhalo */
      }
    }

    snprintf(msg, sizeof(msg), "%s: process succeeds", rows_table[k].what);
    TEST_ASSERT(sage26_initialise_merger_clock_process(&ctx, rows, ngal) == 0, msg);

    const float clock = galaxies[target].MergTime;
    snprintf(msg, sizeof(msg), "%s: the clock (%.9g) is wrong", rows_table[k].what, (double)clock);
    if (isnan(rows_table[k].clock_out)) {
      TEST_ASSERT(clock_matches(clock, expected_clock(rows, 2.0)), msg);
    } else {
      TEST_ASSERT(clock == rows_table[k].clock_out, msg);
    }
    if (!isnan(rows_table[k].infall_stellar_mass)) {
      snprintf(msg, sizeof(msg), "%s: infallStellarMass", rows_table[k].what);
      TEST_ASSERT(galaxies[target].infallStellarMassState == rows_table[k].infall_stellar_mass,
                  msg);
    }
    snprintf(msg, sizeof(msg), "%s: TimeOfInfall", rows_table[k].what);
    TEST_ASSERT(galaxies[target].TimeOfInfallState == rows_table[k].time_of_infall, msg);
  }
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
 * was a Type 0 or 1, the rows that stay a Type 0 or 1; a galaxy that becomes an orphan keeps
 * them, and a Type 2 is copied unchanged. The reset reads the new Type alone, so a run without
 * the recorder (PreviousType -1) resets the same rows.
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
 * @brief   Empty FoFs succeed; a row without a galaxy is skipped and leaves the others as they
 *          were; a NULL context fails
 */
static int test_empty_and_null_rows(void) {
  TEST_ASSERT(init_clock(NULL) == 0, "init succeeds");
  struct ModuleContext ctx = make_context();
  TEST_ASSERT(sage26_initialise_merger_clock_process(&ctx, NULL, 0) == 0, "NULL halos");
  struct Halo rows[2];
  struct GalaxyData galaxies[2];
  make_pair(rows, galaxies, 1, 0, SAGE_MERGTIME_UNSET);
  TEST_ASSERT(sage26_initialise_merger_clock_process(&ctx, rows, 0) == 0, "ngal 0");

  galaxies[0].MergTime = 2.5f;
  rows[1].galaxy = NULL;
  TEST_ASSERT(sage26_initialise_merger_clock_process(&ctx, rows, 2) == 0, "a NULL galaxy row");
  TEST_ASSERT(galaxies[0].MergTime == SAGE_MERGTIME_UNSET, "the central is still processed");
  TEST_ASSERT(galaxies[1].MergTime == SAGE_MERGTIME_UNSET &&
                  galaxies[1].infallStellarMassState == 0.6f &&
                  galaxies[1].TimeOfInfallState == 33.0f,
              "the skipped row's galaxy is not touched");
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

  TEST_RUN(test_merger_time_factor);
  TEST_RUN(test_type0_to_type1);
  TEST_RUN(test_transitions);
  TEST_RUN(test_history_reset);
  TEST_RUN(test_missing_central);
  TEST_RUN(test_empty_and_null_rows);

  TEST_SUMMARY();
  return TEST_RESULT();
}
