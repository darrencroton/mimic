/**
 * @file    test_unit_sham_assign_stellar_mass.c
 * @brief   Unit tests for the SHAM stellar mass assignment module
 *
 * Validates: peak-proxy tracking, the Moster et al. (2013) double-power-law
 * SMHM relation at its analytic pivot, resolution thresholds, deterministic
 * per-galaxy scatter, the stellar baryon-fraction cap, orphan aging and
 * removal, and NULL/Type-3 safety.
 */

#include "../../../../tests/framework/test_framework.h"
#include "core/module_registry.h"
#include "../../../../tests/framework/test_phase_config.h"
#include "core/module_interface.h"
#include "include/types.h"
#include "include/globals.h"
#include "util/error.h"
#include "util/memory.h"
#include "module_system/physical_constants.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* Shared SHAM test fixture boilerplate (counters, config reset, parameters) */
#include "modules/_tests/sham_test_fixtures.h"

/* Module under test */
extern int sham_assign_stellar_mass_init(void);
extern int sham_assign_stellar_mass_process(struct ModuleContext *ctx, struct Halo *halos,
                                            int ngal);
extern int sham_assign_stellar_mass_cleanup(void);

/* Canonical test cosmology/units (mini-Millennium) */
#define TEST_HUBBLE_H 0.73
#define TEST_UNIT_TIME_IN_S 3.08568e19 /* Mpc/h / (km/s) in seconds */

/* SMHM parameters fixed by the fixture (must match sham_test_fixtures.h) */
#define TEST_LOG_M1 11.590
#define TEST_SHAM_N 0.0351

static void setup_run_config(void) {
  reset_config();
  MimicConfig.Hubble_h = TEST_HUBBLE_H;
  MimicConfig.UnitTime_in_s = TEST_UNIT_TIME_IN_S;
}

static void init_module_with(int use_scatter, double scatter_dex, double max_baryon_fraction,
                             double orphan_max_age_myr) {
  setup_run_config();
  set_sham_test_parameters(use_scatter, scatter_dex, max_baryon_fraction, orphan_max_age_myr);
}

/* Build a halo/galaxy pair; everything not set here stays zeroed. */
static void setup_test_halo(struct Halo *halo, struct GalaxyData *galaxy, int type, double mvir,
                            double vmax, long long unique_id) {
  memset(halo, 0, sizeof(*halo));
  memset(galaxy, 0, sizeof(*galaxy));
  halo->Type = type;
  halo->Mvir = (float)mvir;
  halo->Vmax = (float)vmax;
  halo->UniqueGalaxyID = unique_id;
  halo->galaxy = galaxy;
}

/* Mpeak (code units, 1e10 Msun/h) that lands exactly on the SMHM pivot M1 */
static double pivot_mpeak_code(void) { return TEST_HUBBLE_H * pow(10.0, TEST_LOG_M1 - 10.0); }

/* Analytic stellar mass at the pivot: ratio = 1 so the double power law
 * denominator is exactly 2 and M* = N * M1 (in Msun), converted to code units. */
static double pivot_stellar_mass_code(void) {
  return TEST_SHAM_N * pow(10.0, TEST_LOG_M1) * TEST_HUBBLE_H / 1.0e10;
}

/**
 * @test   test_module_initialization
 * @brief  Module init and cleanup succeed through the full pipeline
 */
int test_module_initialization(void) {
  init_memory_system(0);
  ensure_modules_registered();
  init_module_with(1, 0.20, 0.17, 0.0);

  test_phase_add("galaxy_physics", "sham_assign_stellar_mass", PROCESSING_MODE_FULL_HALO);
  MimicConfig.SubSteps = 1;

  TEST_ASSERT(module_system_init() == 0, "Module system initialization should succeed");

  module_system_cleanup();
  check_memory_leaks();
  return TEST_PASS;
}

/**
 * @test   test_peak_proxy_tracking
 * @brief  Type 0/1 halos ratchet Mpeak/Vpeak up and never down; orphan age resets on resolution
 */
int test_peak_proxy_tracking(void) {
  init_memory_system(0);
  init_module_with(0, 0.0, 0.17, 0.0);
  TEST_ASSERT(sham_assign_stellar_mass_init() == 0, "Module init should succeed");

  struct Halo halo;
  struct GalaxyData galaxy;
  setup_test_halo(&halo, &galaxy, 0, 50.0, 200.0, 1001);
  galaxy.ShamOrphanAge = 123.0f;

  struct ModuleContext ctx;
  memset(&ctx, 0, sizeof(ctx));

  TEST_ASSERT(sham_assign_stellar_mass_process(&ctx, &halo, 1) == 0, "process should succeed");
  TEST_ASSERT_DOUBLE_EQUAL(galaxy.ShamMpeak, 50.0, 1e-4, "Mpeak should track Mvir");
  TEST_ASSERT_DOUBLE_EQUAL(galaxy.ShamVpeak, 200.0, 1e-4, "Vpeak should track Vmax");
  TEST_ASSERT_DOUBLE_EQUAL(galaxy.ShamOrphanAge, 0.0, 1e-6,
                           "Resolved halo should reset orphan age");

  /* The halo shrinks: peaks must not decrease (the ratchet is the SHAM proxy) */
  halo.Mvir = 10.0f;
  halo.Vmax = 120.0f;
  TEST_ASSERT(sham_assign_stellar_mass_process(&ctx, &halo, 1) == 0, "process should succeed");
  TEST_ASSERT_DOUBLE_EQUAL(galaxy.ShamMpeak, 50.0, 1e-4, "Mpeak must not decrease");
  TEST_ASSERT_DOUBLE_EQUAL(galaxy.ShamVpeak, 200.0, 1e-4, "Vpeak must not decrease");

  TEST_ASSERT(sham_assign_stellar_mass_cleanup() == 0, "Module cleanup should succeed");
  check_memory_leaks();
  return TEST_PASS;
}

/**
 * @test   test_smhm_pivot_value
 * @brief  Without scatter, the pivot halo mass gives the analytic M* = N * M1
 */
int test_smhm_pivot_value(void) {
  init_memory_system(0);
  init_module_with(0, 0.0, 1.0, 0.0); /* scatter off; cap above the expected value */
  TEST_ASSERT(sham_assign_stellar_mass_init() == 0, "Module init should succeed");

  struct Halo halo;
  struct GalaxyData galaxy;
  setup_test_halo(&halo, &galaxy, 0, pivot_mpeak_code(), 200.0, 1002);

  struct ModuleContext ctx;
  memset(&ctx, 0, sizeof(ctx));

  TEST_ASSERT(sham_assign_stellar_mass_process(&ctx, &halo, 1) == 0, "process should succeed");

  const double expected = pivot_stellar_mass_code();
  TEST_ASSERT_DOUBLE_EQUAL(galaxy.StellarMass, expected, expected * 1e-5,
                           "Pivot-mass halo should get M* = N * M1 (double power law at ratio 1)");
  TEST_ASSERT_DOUBLE_EQUAL(galaxy.ShamStellarMassNoScatter, expected, expected * 1e-5,
                           "No-scatter diagnostic should equal the assigned mass");
  TEST_ASSERT_DOUBLE_EQUAL(galaxy.ShamScatterDex, 0.0, 1e-12,
                           "Scatter diagnostic should be zero with scatter disabled");

  TEST_ASSERT(sham_assign_stellar_mass_cleanup() == 0, "Module cleanup should succeed");
  check_memory_leaks();
  return TEST_PASS;
}

/**
 * @test   test_below_thresholds_assigns_zero
 * @brief  Halos below the Mpeak/Vpeak resolution thresholds receive zero stellar mass
 */
int test_below_thresholds_assigns_zero(void) {
  init_memory_system(0);
  init_module_with(0, 0.0, 0.17, 0.0); /* thresholds: Mpeak 0.10, Vpeak 80 km/s */
  TEST_ASSERT(sham_assign_stellar_mass_init() == 0, "Module init should succeed");

  struct ModuleContext ctx;
  memset(&ctx, 0, sizeof(ctx));

  struct Halo halo;
  struct GalaxyData galaxy;

  /* Below the Mpeak floor */
  setup_test_halo(&halo, &galaxy, 0, 0.05, 200.0, 1003);
  galaxy.StellarMass = 5.0f; /* stale value must be cleared, not kept */
  TEST_ASSERT(sham_assign_stellar_mass_process(&ctx, &halo, 1) == 0, "process should succeed");
  TEST_ASSERT_DOUBLE_EQUAL(galaxy.StellarMass, 0.0, 1e-12,
                           "Below-Mpeak-threshold halo should get zero stellar mass");

  /* Below the Vpeak floor */
  setup_test_halo(&halo, &galaxy, 0, 10.0, 50.0, 1004);
  TEST_ASSERT(sham_assign_stellar_mass_process(&ctx, &halo, 1) == 0, "process should succeed");
  TEST_ASSERT_DOUBLE_EQUAL(galaxy.StellarMass, 0.0, 1e-12,
                           "Below-Vpeak-threshold halo should get zero stellar mass");

  TEST_ASSERT(sham_assign_stellar_mass_cleanup() == 0, "Module cleanup should succeed");
  check_memory_leaks();
  return TEST_PASS;
}

/**
 * @test   test_scatter_is_deterministic_per_galaxy
 * @brief  Scatter is deterministic per galaxy ID: same ID gives same M*, different ID differs
 */
int test_scatter_is_deterministic_per_galaxy(void) {
  init_memory_system(0);
  init_module_with(1, 0.20, 1.0, 0.0);
  TEST_ASSERT(sham_assign_stellar_mass_init() == 0, "Module init should succeed");

  struct ModuleContext ctx;
  memset(&ctx, 0, sizeof(ctx));

  struct Halo halo;
  struct GalaxyData galaxy;
  const double mpeak = pivot_mpeak_code();

  setup_test_halo(&halo, &galaxy, 0, mpeak, 200.0, 42);
  TEST_ASSERT(sham_assign_stellar_mass_process(&ctx, &halo, 1) == 0, "process should succeed");
  const float first = galaxy.StellarMass;
  const float first_scatter = galaxy.ShamScatterDex;
  TEST_ASSERT(fabsf(first_scatter) > 0.0f, "Scatter should be applied when enabled");

  /* Same galaxy ID, fresh galaxy state: assignment must reproduce exactly */
  setup_test_halo(&halo, &galaxy, 0, mpeak, 200.0, 42);
  TEST_ASSERT(sham_assign_stellar_mass_process(&ctx, &halo, 1) == 0, "process should succeed");
  TEST_ASSERT(galaxy.StellarMass == first && galaxy.ShamScatterDex == first_scatter,
              "Same UniqueGalaxyID must reproduce the identical scattered mass");

  /* Different galaxy ID: the deterministic draw differs */
  setup_test_halo(&halo, &galaxy, 0, mpeak, 200.0, 43);
  TEST_ASSERT(sham_assign_stellar_mass_process(&ctx, &halo, 1) == 0, "process should succeed");
  TEST_ASSERT(galaxy.ShamScatterDex != first_scatter,
              "A different UniqueGalaxyID should draw a different scatter");

  TEST_ASSERT(sham_assign_stellar_mass_cleanup() == 0, "Module cleanup should succeed");
  check_memory_leaks();
  return TEST_PASS;
}

/**
 * @test   test_baryon_fraction_cap
 * @brief  The stellar baryon-fraction cap bounds M* to cap * Mpeak
 */
int test_baryon_fraction_cap(void) {
  init_memory_system(0);
  const double cap_fraction = 0.001; /* far below the pivot's uncapped stellar fraction */
  init_module_with(0, 0.0, cap_fraction, 0.0);
  TEST_ASSERT(sham_assign_stellar_mass_init() == 0, "Module init should succeed");

  struct Halo halo;
  struct GalaxyData galaxy;
  const double mpeak = pivot_mpeak_code();
  setup_test_halo(&halo, &galaxy, 0, mpeak, 200.0, 1005);

  struct ModuleContext ctx;
  memset(&ctx, 0, sizeof(ctx));

  TEST_ASSERT(sham_assign_stellar_mass_process(&ctx, &halo, 1) == 0, "process should succeed");
  TEST_ASSERT(pivot_stellar_mass_code() > cap_fraction * mpeak,
              "Test setup: uncapped mass must exceed the cap for this check to bite");
  TEST_ASSERT_DOUBLE_EQUAL(galaxy.StellarMass, cap_fraction * mpeak, cap_fraction * mpeak * 1e-5,
                           "Assigned mass should be capped at the stellar baryon fraction");

  TEST_ASSERT(sham_assign_stellar_mass_cleanup() == 0, "Module cleanup should succeed");
  check_memory_leaks();
  return TEST_PASS;
}

/**
 * @test   test_orphan_aging_and_removal
 * @brief  Orphans accumulate age from dT and are removed (Type 3) once past the configured lifetime
 */
int test_orphan_aging_and_removal(void) {
  init_memory_system(0);
  const double max_age_myr = 500.0;
  init_module_with(0, 0.0, 0.17, max_age_myr);
  TEST_ASSERT(sham_assign_stellar_mass_init() == 0, "Module init should succeed");

  /* dT chosen so one step ages ~300 Myr: below the cap once, past it twice */
  const double step_myr = 300.0;
  const float dt_code = (float)(step_myr * SEC_PER_MEGAYEAR / TEST_UNIT_TIME_IN_S);

  struct Halo halo;
  struct GalaxyData galaxy;
  setup_test_halo(&halo, &galaxy, 2, 0.0, 0.0, 1006);
  halo.dT = dt_code;
  galaxy.ShamMpeak = 10.0f; /* peaks frozen at infall, above thresholds */
  galaxy.ShamVpeak = 200.0f;

  struct ModuleContext ctx;
  memset(&ctx, 0, sizeof(ctx));

  TEST_ASSERT(sham_assign_stellar_mass_process(&ctx, &halo, 1) == 0, "process should succeed");
  TEST_ASSERT(halo.Type == 2, "Orphan within its lifetime should survive");
  TEST_ASSERT(galaxy.ShamOrphanAge > 0.0f, "Orphan age should accumulate from dT");
  TEST_ASSERT(galaxy.StellarMass > 0.0f, "Surviving orphan keeps its SHAM assignment");

  TEST_ASSERT(sham_assign_stellar_mass_process(&ctx, &halo, 1) == 0, "process should succeed");
  TEST_ASSERT(halo.Type == 3, "Orphan past its lifetime should be removed (Type 3)");

  TEST_ASSERT(sham_assign_stellar_mass_cleanup() == 0, "Module cleanup should succeed");
  check_memory_leaks();
  return TEST_PASS;
}

/**
 * @test   test_null_galaxy_and_type3_skipped
 * @brief  NULL-galaxy and Type 3 entries are skipped without touching galaxy state
 */
int test_null_galaxy_and_type3_skipped(void) {
  init_memory_system(0);
  init_module_with(0, 0.0, 0.17, 0.0);
  TEST_ASSERT(sham_assign_stellar_mass_init() == 0, "Module init should succeed");

  struct Halo halos[2];
  struct GalaxyData galaxy;

  setup_test_halo(&halos[0], &galaxy, 3, 50.0, 200.0, 1007);
  galaxy.StellarMass = 5.0f;
  memset(&halos[1], 0, sizeof(halos[1]));
  halos[1].Type = 0;
  halos[1].galaxy = NULL; /* must not crash */

  struct ModuleContext ctx;
  memset(&ctx, 0, sizeof(ctx));

  TEST_ASSERT(sham_assign_stellar_mass_process(&ctx, halos, 2) == 0,
              "process should skip NULL-galaxy and Type-3 entries safely");
  TEST_ASSERT_DOUBLE_EQUAL(galaxy.StellarMass, 5.0, 1e-6, "Type 3 entry must be left untouched");

  TEST_ASSERT(sham_assign_stellar_mass_cleanup() == 0, "Module cleanup should succeed");
  check_memory_leaks();
  return TEST_PASS;
}

int main(void) {
  printf("%s", BLUE);
  printf("============================================================\n");
  printf("Test Suite: sham_assign_stellar_mass\n");
  printf("============================================================\n");
  printf("%s\n", NC);

  initialize_error_handling(LOG_LEVEL_WARNING, NULL);

  TEST_RUN(test_module_initialization);
  TEST_RUN(test_peak_proxy_tracking);
  TEST_RUN(test_smhm_pivot_value);
  TEST_RUN(test_below_thresholds_assigns_zero);
  TEST_RUN(test_scatter_is_deterministic_per_galaxy);
  TEST_RUN(test_baryon_fraction_cap);
  TEST_RUN(test_orphan_aging_and_removal);
  TEST_RUN(test_null_galaxy_and_type3_skipped);

  TEST_SUMMARY();
  return TEST_RESULT();
}
