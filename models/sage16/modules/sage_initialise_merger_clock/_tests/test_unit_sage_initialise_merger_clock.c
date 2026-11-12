/**
 * @file    test_unit_sage_initialise_merger_clock.c
 * @brief   Unit tests for sage_initialise_merger_clock module physics
 *
 * Tests the merger timescale calculation physics in isolation using minimal mocks.
 *
 * Physics: Binney & Tremaine (1987) dynamical friction
 *   t_merge = 2 * 1.17 * R_vir^2 * V_vir / (ln(1 + N_cen/N_sat) * G * M_sat)
 *
 * The expected clocks are hand-evaluated literals of that formula (G = 43.02), compared to
 * a relative 1e-6, the rounding of the float MergTime. The Coulomb logarithm is log1p, not
 * log: a plain log changes the first literal from 0.11342 to 0.11811.
 *
 * Test cases:
 *   - test_type_dispatch: a table of (Type, MergTime held) rows giving the clock afterwards:
 *     a central at or off the sentinel (reset to 999.9), an orphan with an unset clock (forced
 *     to 0), Type > 2 (skipped), and a satellite or orphan with a clock at or below the 999
 *     threshold (kept)
 *   - test_dynamical_friction_formula: exact clocks for satellites of several masses (including
 *     the StellarMass and ColdGas that add to the satellite mass), with infallMvir positive and
 *     zero, and for two satellites in one FoF
 *   - test_coulomb_logarithm: the ratio of two clocks differing only in the central's Len is
 *     the ratio of log1p
 *   - test_small_satellite_immediate_merge: Len < 10 gives -1, Len 10 a finite clock
 *   - test_mergtime_cap: a clock of 999 or more is capped to 998
 *   - test_zero_mass_handling: zero satellite mass gives the immediate-merge -1
 *   - test_null_galaxy_handling: a NULL-galaxy row is skipped without disturbing the rest
 *   - test_no_central_returns_early: no Type 0 leaves every clock at the sentinel
 *   - test_empty_halos_array: empty input succeeds; a NULL context or parameters fail
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../../../tests/framework/test_framework.h"

#include "include/types.h"
#include "core/module_interface.h"
#include "include/globals.h"
#include "util/memory.h"
#include "util/error.h"
#include "shared/sage_constants.h"

extern int sage_initialise_merger_clock_init(void);
extern int sage_initialise_merger_clock_process(struct ModuleContext *ctx, struct Halo *halos,
                                                int ngal);
extern int sage_initialise_merger_clock_cleanup(void);

#define FLOAT_EQ(a, b, epsilon) (fabs((a) - (b)) < (epsilon))

/* Test statistics (required for TEST_RUN macro) */
static int passed = 0;
static int failed = 0;

static void setup_mock_config(void) {
  memset(&MimicConfig, 0, sizeof(MimicConfig));
  MimicConfig.Omega = 0.25;
  MimicConfig.OmegaLambda = 0.75;
  MimicConfig.Hubble_h = 0.73;
  MimicConfig.G = 43.02;
}

static struct ModuleContext create_test_context(void) {
  struct ModuleContext ctx;
  memset(&ctx, 0, sizeof(ctx));
  ctx.redshift = 0.0;
  ctx.time = 13.6;
  ctx.snapshot_number = 63;
  ctx.substep_number = 0;
  ctx.num_substeps = 1;
  ctx.time_interval = 0.1;
  ctx.substep_dt = ctx.time_interval;
  ctx.params = &MimicConfig;
  return ctx;
}

static struct Halo create_test_halo(int type, double mvir, double rvir, double vvir, int len,
                                    double infall_mvir, struct GalaxyData *galaxy) {
  struct Halo halo;
  memset(&halo, 0, sizeof(halo));
  halo.Type = type;
  halo.Mvir = mvir;
  halo.Rvir = rvir;
  halo.Vvir = vvir;
  halo.Len = len;
  halo.infallMvir = infall_mvir;
  halo.SnapNum = 63;
  halo.HaloNr = 1;
  halo.galaxy = galaxy;
  return halo;
}

static struct GalaxyData create_test_galaxy(float mergtime, float stellar_mass, float cold_gas) {
  struct GalaxyData gal;
  memset(&gal, 0, sizeof(gal));
  gal.MergTime = mergtime;
  gal.StellarMass = stellar_mass;
  gal.ColdGas = cold_gas;
  return gal;
}

/** @brief 1 if a stored float clock matches the expectation to within float rounding */
static int clock_matches(const float stored, const double expected) {
  return fabs(stored / expected - 1.0) < 1e-6;
}

// ============================================================================
// TYPE DISPATCH
// ============================================================================

/**
 * @test    test_type_dispatch
 * @brief   The clock after one pass, for each Type and held MergTime
 *
 * A FoF central (Type 0, Mvir 100, Rvir 0.5, Vvir 200, Len 1000) with one other row of the given
 * Type; a Type 0 row is the central alone. A central's clock is reset to 999.9 whatever it
 * held. A Type 2 orphan with an unset clock is forced to 0 (immediate merge), and one with a set
 * clock keeps it. A Type 3 row is skipped. A Type 1 satellite whose clock is at or below the 999
 * threshold keeps it (the sentinel state, not infallMvir, decides recalculation).
 */
static int test_type_dispatch(void) {
  static const struct {
    int type;
    float mergtime_in;
    float mergtime_out;
    const char *what;
  } rows[] = {
      {0, 999.9f, 999.9f, "a Type 0 central at the sentinel stays there"},
      {0, 5.0f, 999.9f, "a Type 0 central with a set clock is reset to 999.9"},
      {2, 999.9f, 0.0f, "a Type 2 orphan with an unset clock is forced to 0"},
      {2, 5.0f, 5.0f, "a Type 2 orphan with a set clock keeps it"},
      {3, 999.9f, 999.9f, "a Type 3 row is skipped"},
      {1, 5.5f, 5.5f, "a Type 1 satellite with a calculated clock keeps it"},
      {1, 999.0f, 999.0f, "a Type 1 satellite at exactly 999 keeps its clock"},
  };

  for (size_t k = 0; k < sizeof(rows) / sizeof(rows[0]); k++) {
    init_memory_system(0);
    struct ModuleContext ctx = create_test_context();
    struct GalaxyData cen_gal =
        create_test_galaxy(rows[k].type == 0 ? rows[k].mergtime_in : 999.9f, 50.0, 20.0);
    struct Halo central = create_test_halo(0, 100.0, 0.5, 200.0, 1000, 0.0, &cen_gal);
    struct GalaxyData other_gal = create_test_galaxy(rows[k].mergtime_in, 5.0, 2.0);
    struct Halo other = create_test_halo(rows[k].type, 20.0, 0.2, 100.0, 200, 25.0, &other_gal);
    ctx.central_galaxy = &central;
    struct Halo halos[2] = {central, other};
    const int ngal = rows[k].type == 0 ? 1 : 2;
    const struct GalaxyData *target = rows[k].type == 0 ? &cen_gal : &other_gal;
    char msg[160];

    TEST_ASSERT(sage_initialise_merger_clock_process(&ctx, halos, ngal) == 0,
                "Process should succeed");
    snprintf(msg, sizeof(msg), "%s (MergTime %.9g)", rows[k].what, (double)target->MergTime);
    TEST_ASSERT(target->MergTime == rows[k].mergtime_out, msg);
    check_memory_leaks();
  }
  return TEST_PASS;
}

// ============================================================================
// PHYSICS CALCULATION TESTS
// ============================================================================

/**
 * @test    test_dynamical_friction_formula
 * @brief   Exact Binney & Tremaine clocks, one per satellite, and two satellites in one FoF
 *
 * The central is Mvir 100, Rvir 0.5, Vvir 200, Len 1000, so the clock is
 * 2 * 1.17 * 0.25 * 200 / (log1p(1000 / Len_sat) * 43.02 * M_sat) with M_sat = Mvir + StellarMass
 * + ColdGas. A satellite whose clock is unset is calculated whether or not infallMvir is
 * positive.
 */
static int test_dynamical_friction_formula(void) {
  static const struct {
    double mvir;
    int len;
    float stellar;
    float cold;
    double infall_mvir;
    double expected;
    const char *what;
  } rows[] = {
      {10.0, 100, 0.0f, 0.0f, 15.0, 0.1134188512241674, "M_sat 10, Len 100"},
      {20.0, 200, 5.0f, 2.0f, 25.0, 0.056217558849885434, "M_sat 27 with baryons, infallMvir 25"},
      {20.0, 200, 5.0f, 2.0f, 0.0, 0.056217558849885434, "M_sat 27 with baryons, infallMvir 0"},
  };

  for (size_t k = 0; k < sizeof(rows) / sizeof(rows[0]); k++) {
    init_memory_system(0);
    struct ModuleContext ctx = create_test_context();
    struct GalaxyData cen_gal = create_test_galaxy(999.9f, 0.0, 0.0);
    struct Halo central = create_test_halo(0, 100.0, 0.5, 200.0, 1000, 0.0, &cen_gal);
    struct GalaxyData sat_gal = create_test_galaxy(999.9f, rows[k].stellar, rows[k].cold);
    struct Halo satellite =
        create_test_halo(1, rows[k].mvir, 0.1, 100.0, rows[k].len, rows[k].infall_mvir, &sat_gal);
    ctx.central_galaxy = &central;
    struct Halo halos[2] = {central, satellite};
    char msg[160];

    TEST_ASSERT(sage_initialise_merger_clock_process(&ctx, halos, 2) == 0,
                "Process should succeed");
    snprintf(msg, sizeof(msg), "%s: MergTime %.9g should be %.9g", rows[k].what,
             (double)sat_gal.MergTime, rows[k].expected);
    TEST_ASSERT(clock_matches(sat_gal.MergTime, rows[k].expected), msg);
    check_memory_leaks();
  }

  /* Two satellites of one FoF are each calculated against the central, independently */
  init_memory_system(0);
  struct ModuleContext ctx = create_test_context();
  struct GalaxyData cen_gal = create_test_galaxy(999.9f, 50.0, 20.0);
  struct Halo central = create_test_halo(0, 100.0, 0.5, 200.0, 1000, 0.0, &cen_gal);
  struct GalaxyData sat_gal1 = create_test_galaxy(999.9f, 0.0, 0.0);
  struct Halo satellite1 = create_test_halo(1, 20.0, 0.2, 100.0, 200, 25.0, &sat_gal1);
  struct GalaxyData sat_gal2 = create_test_galaxy(999.9f, 0.0, 0.0);
  struct Halo satellite2 = create_test_halo(1, 5.0, 0.1, 50.0, 50, 8.0, &sat_gal2);
  ctx.central_galaxy = &central;
  struct Halo halos[3] = {central, satellite1, satellite2};

  TEST_ASSERT(sage_initialise_merger_clock_process(&ctx, halos, 3) == 0, "Process should succeed");
  TEST_ASSERT(clock_matches(sat_gal1.MergTime, 0.07589370444734533),
              "Satellite 1 (M_sat 20, Len 200) should have its own clock");
  TEST_ASSERT(clock_matches(sat_gal2.MergTime, 0.17865956501211983),
              "Satellite 2 (M_sat 5, Len 50) should have its own clock");
  check_memory_leaks();
  return TEST_PASS;
}

/**
 * @test    test_coulomb_logarithm
 * @brief   The Coulomb logarithm is log1p(N_cen/N_sat): the clock ratio for two central Lens
 *
 * Two satellites of Len 100 differ only in the central's Len (10000 and 1000). The clock goes
 * as 1 / log1p(N_cen/N_sat), so the ratio of the second to the first is
 * log1p(100) / log1p(10) = 1.924654745849414; a plain log(N_cen / N_sat), without the +1,
 * would give 2.
 */
static int test_coulomb_logarithm(void) {
  init_memory_system(0);
  struct ModuleContext ctx = create_test_context();
  struct GalaxyData cen_gal = create_test_galaxy(999.9f, 0.0, 0.0);
  struct Halo central = create_test_halo(0, 100.0, 0.5, 200.0, 10000, 0.0, &cen_gal);
  struct GalaxyData sat_gal = create_test_galaxy(999.9f, 0.0, 0.0);
  struct Halo satellite = create_test_halo(1, 10.0, 0.1, 100.0, 100, 15.0, &sat_gal);
  ctx.central_galaxy = &central;
  struct Halo halos[2] = {central, satellite};

  sage_initialise_merger_clock_process(&ctx, halos, 2);
  float mergtime1 = halos[1].galaxy->MergTime;

  halos[0].Len = 1000;
  halos[1].galaxy->MergTime = 999.9f;
  sage_initialise_merger_clock_process(&ctx, halos, 2);
  float mergtime2 = halos[1].galaxy->MergTime;

  TEST_ASSERT(clock_matches(mergtime2 / mergtime1, 1.924654745849414),
              "the clock should go as 1 / log1p(N_cen / N_sat)");
  check_memory_leaks();
  return TEST_PASS;
}

/**
 * @test    test_small_satellite_immediate_merge
 * @brief   SAGE parity: satellites with Len < MIN_NUM_PART_SAT_HALO (10) get
 *          mergtime = -1.0 (immediate merge), not a finite dynamical-friction
 *          clock. Satellites with Len >= 10 get a finite positive time.
 *          (SAGE model_mergers.c estimate_merging_time: the Len >= 10 condition.)
 */
static int test_small_satellite_immediate_merge(void) {
  init_memory_system(0);
  struct ModuleContext ctx = create_test_context();
  struct GalaxyData cen_gal = create_test_galaxy(999.9f, 0.0, 0.0);
  struct Halo central = create_test_halo(0, 100.0, 0.5, 200.0, 1000, 0.0, &cen_gal);
  struct GalaxyData sat_gal1 = create_test_galaxy(999.9f, 0.0, 0.0);
  struct Halo satellite1 = create_test_halo(1, 10.0, 0.1, 100.0, 5, 15.0, &sat_gal1);
  struct GalaxyData sat_gal2 = create_test_galaxy(999.9f, 0.0, 0.0);
  struct Halo satellite2 = create_test_halo(1, 10.0, 0.1, 100.0, 10, 15.0, &sat_gal2);
  ctx.central_galaxy = &central;

  struct Halo halos1[2] = {central, satellite1};
  sage_initialise_merger_clock_process(&ctx, halos1, 2);
  float mergtime_below = halos1[1].galaxy->MergTime;

  struct Halo halos2[2] = {central, satellite2};
  sage_initialise_merger_clock_process(&ctx, halos2, 2);
  float mergtime_at = halos2[1].galaxy->MergTime;

  TEST_ASSERT(FLOAT_EQ(mergtime_below, -1.0f, 0.01),
              "Len<10 satellite should get mergtime=-1 (immediate merge)");
  TEST_ASSERT(mergtime_at > 0.0f, "Len>=10 satellite should get a finite positive merger time");
  check_memory_leaks();
  return TEST_PASS;
}

// ============================================================================
// EDGE CASE TESTS
// ============================================================================

/**
 * @test    test_mergtime_cap
 * @brief   MergTime >= 999.0 is capped to exactly 998.0
 */
static int test_mergtime_cap(void) {
  init_memory_system(0);
  struct ModuleContext ctx = create_test_context();
  struct GalaxyData cen_gal = create_test_galaxy(999.9f, 0.0, 0.0);
  struct Halo central = create_test_halo(0, 100.0, 100.0, 200.0, 1000, 0.0, &cen_gal);
  struct GalaxyData sat_gal = create_test_galaxy(999.9f, 0.0, 0.0);
  struct Halo satellite = create_test_halo(1, 0.001, 0.001, 10.0, 10, 0.002, &sat_gal);
  ctx.central_galaxy = &central;
  struct Halo halos[2] = {central, satellite};

  int result = sage_initialise_merger_clock_process(&ctx, halos, 2);

  TEST_ASSERT(result == 0, "Process should succeed");
  TEST_ASSERT(halos[1].galaxy->MergTime == (float)SAGE_MERGTIME_CEILING,
              "MergTime should be capped at exactly 998.0");
  check_memory_leaks();
  return TEST_PASS;
}

/**
 * @test    test_zero_mass_handling
 * @brief   Zero satellite mass gives the immediate-merge clock -1.0
 */
static int test_zero_mass_handling(void) {
  init_memory_system(0);
  struct ModuleContext ctx = create_test_context();
  struct GalaxyData cen_gal = create_test_galaxy(999.9f, 0.0, 0.0);
  struct Halo central = create_test_halo(0, 100.0, 0.5, 200.0, 1000, 0.0, &cen_gal);
  struct GalaxyData sat_gal = create_test_galaxy(999.9f, 0.0, 0.0);
  struct Halo satellite = create_test_halo(1, 0.0, 0.1, 100.0, 100, 0.001, &sat_gal);
  ctx.central_galaxy = &central;
  struct Halo halos[2] = {central, satellite};

  int result = sage_initialise_merger_clock_process(&ctx, halos, 2);

  TEST_ASSERT(result == 0, "Process should succeed");
  TEST_ASSERT(halos[1].galaxy->MergTime == (float)SAGE_MERGTIME_IMMEDIATE,
              "Zero mass should give the immediate-merge MergTime = -1.0");
  check_memory_leaks();
  return TEST_PASS;
}

/**
 * @test    test_null_galaxy_handling
 * @brief   A row without a galaxy is skipped; the central before it and the satellite after it
 *          are still processed
 */
static int test_null_galaxy_handling(void) {
  init_memory_system(0);
  struct ModuleContext ctx = create_test_context();
  struct GalaxyData cen_gal = create_test_galaxy(5.0f, 50.0, 20.0);
  struct Halo central = create_test_halo(0, 100.0, 0.5, 200.0, 1000, 0.0, &cen_gal);
  struct Halo satellite_without_galaxy = create_test_halo(1, 20.0, 0.2, 100.0, 200, 25.0, NULL);
  struct GalaxyData sat_gal = create_test_galaxy(999.9f, 0.0, 0.0);
  struct Halo satellite = create_test_halo(1, 20.0, 0.2, 100.0, 200, 25.0, &sat_gal);
  ctx.central_galaxy = &central;
  struct Halo halos[3] = {central, satellite_without_galaxy, satellite};

  int result = sage_initialise_merger_clock_process(&ctx, halos, 3);

  TEST_ASSERT(result == 0, "Should handle NULL galaxy gracefully");
  TEST_ASSERT(cen_gal.MergTime == 999.9f, "The central before the NULL row is still processed");
  TEST_ASSERT(clock_matches(sat_gal.MergTime, 0.07589370444734533),
              "The satellite after the NULL row is still processed");
  check_memory_leaks();
  return TEST_PASS;
}

/**
 * @test    test_no_central_returns_early
 * @brief   No Type 0 central returns 0 (early exit)
 */
static int test_no_central_returns_early(void) {
  init_memory_system(0);
  struct ModuleContext ctx = create_test_context();
  struct GalaxyData sat_gal1 = create_test_galaxy(999.9f, 5.0, 2.0);
  struct Halo satellite1 = create_test_halo(1, 20.0, 0.2, 100.0, 200, 25.0, &sat_gal1);
  struct GalaxyData sat_gal2 = create_test_galaxy(999.9f, 3.0, 1.0);
  struct Halo satellite2 = create_test_halo(2, 10.0, 0.1, 80.0, 100, 15.0, &sat_gal2);
  ctx.central_galaxy = &satellite1;
  struct Halo halos[2] = {satellite1, satellite2};

  int result = sage_initialise_merger_clock_process(&ctx, halos, 2);

  TEST_ASSERT(result == 0, "Should return 0 when no central found");
  TEST_ASSERT(FLOAT_EQ(halos[0].galaxy->MergTime, 999.9f, 0.01),
              "Satellite 1 should remain at sentinel (no central)");
  TEST_ASSERT(FLOAT_EQ(halos[1].galaxy->MergTime, 999.9f, 0.01),
              "Satellite 2 should remain at sentinel (no central)");
  check_memory_leaks();
  return TEST_PASS;
}

/**
 * @test    test_empty_halos_array
 * @brief   Empty input succeeds; a NULL context or NULL parameters fail
 */
static int test_empty_halos_array(void) {
  init_memory_system(0);
  struct ModuleContext ctx = create_test_context();

  int result1 = sage_initialise_merger_clock_process(&ctx, NULL, 0);
  TEST_ASSERT(result1 == 0, "Should handle NULL halos gracefully");

  struct Halo dummy;
  memset(&dummy, 0, sizeof(dummy));
  int result2 = sage_initialise_merger_clock_process(&ctx, &dummy, 0);
  TEST_ASSERT(result2 == 0, "Should handle ngal=0 gracefully");

  /* The refusals log an error; capture it to keep the output clean */
  FILE *capture = tmpfile();
  FILE *old_out = capture != NULL ? set_log_output(capture) : NULL;
  struct Halo halos[1] = {dummy};
  const int null_ctx = sage_initialise_merger_clock_process(NULL, halos, 1);
  ctx.params = NULL;
  const int null_params = sage_initialise_merger_clock_process(&ctx, halos, 1);
  if (capture != NULL) {
    set_log_output(old_out);
    fclose(capture);
  }
  TEST_ASSERT(null_ctx != 0, "A NULL context should fail");
  TEST_ASSERT(null_params != 0, "A context without parameters should fail");

  check_memory_leaks();
  return TEST_PASS;
}

// ============================================================================
// MAIN TEST RUNNER
// ============================================================================

int main(void) {
  initialize_error_handling(LOG_LEVEL_WARNING, NULL);
  setup_mock_config();

  TEST_RUN(test_type_dispatch);
  TEST_RUN(test_dynamical_friction_formula);
  TEST_RUN(test_coulomb_logarithm);
  TEST_RUN(test_small_satellite_immediate_merge);
  TEST_RUN(test_mergtime_cap);
  TEST_RUN(test_zero_mass_handling);
  TEST_RUN(test_null_galaxy_handling);
  TEST_RUN(test_no_central_returns_early);
  TEST_RUN(test_empty_halos_array);

  TEST_SUMMARY();
  return TEST_RESULT();
}
