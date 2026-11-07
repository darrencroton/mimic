/**
 * @file    test_unit_sage26_disk_instability.c
 * @brief   Comprehensive unit tests for sage26_disk_instability module
 *
 * Validates: Module lifecycle, physics calculation, edge cases, parameter validation
 *
 * This test validates both software engineering and physics aspects:
 * - Module registration and initialization
 * - Parameter reading and validation (StarFormingDiskFactor)
 * - Memory allocation and cleanup (no leaks)
 * - Disk stability criterion (Mo, Mao & White 1998)
 * - Stellar mass transfer to bulge with metallicity preservation
 * - Trigger flag setting for downstream modules
 * - Edge cases (zero mass, zero radius) and SAGE26's absence of transfer caps and bulge clamps
 *
 * Test cases:
 *   - test_module_registration: Module registers correctly
 *   - test_module_initialization: Module init/cleanup lifecycle
 *   - test_parameter_reading: StarFormingDiskFactor parameter
 *   - test_invalid_parameter_fails: Invalid parameters rejected
 *   - test_memory_safety: No memory leaks during operation
 *   - test_process_by_galaxy_mode: ngal=1 requirement
 *   - test_stable_disk_no_transfer: Stable disk physics
 *   - test_stable_disk_at_threshold: Boundary condition
 *   - test_zero_diskmass: Zero disk mass handling
 *   - test_unstable_disk_stars_transfer: Unstable disk bulge formation
 *   - test_unstable_disk_metallicity_preserved: Metal conservation
 *   - test_unstable_disk_gas_only: Gas-only disk
 *   - test_unstable_disk_stars_only: Stars-only disk (triggers NOT set)
 *   - test_zero_vmax: Zero virial velocity
 *   - test_zero_disk_radius: Zero disk radius
 *   - test_full_disk_transfer_is_not_capped: Full disk transfer, SAGE26's bits, no cap
 *   - test_bulge_mass_not_clamped_to_stellar_mass: No bulge clamp (SAGE26's bits)
 *   - test_metals_bulge_not_clamped_to_stellar_metals: No bulge-metals clamp (SAGE26's bits)
 *   - test_disk_factor_sensitivity: Parameter affects results
 *   - test_ffb_row_bypassed: a row whose FFBRegime is 1 leaves itself and its FoF central
 *     bytewise unchanged on an unstable disk (no transfer, UnstableDiskGasFraction keeps 0); the
 *     same row with FFBRegime 0 gets SAGE26's check_disk_instability trigger, written inline
 *     (c13 Slice 3)
 *
 * @author  Mimic Development Team
 * @date    2025-12-23
 */

#include "../../tests/framework/test_framework.h"
#include "core/module_registry.h"
#include "../../../../tests/framework/test_phase_config.h"
#include "core/module_interface.h"
#include "include/types.h"
#include "include/proto.h"
#include "include/globals.h"
#include "util/error.h"
#include "util/memory.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* Shared SAGE16 test fixture boilerplate (counters, config reset, module registration) */
#include "modules/_tests/sage_test_fixtures.h"

/* Module functions (extern declarations for direct testing) */
extern int sage26_disk_instability_init(void);
extern int sage26_disk_instability_process(struct ModuleContext *ctx, struct Halo *halos, int ngal);
extern int sage26_disk_instability_cleanup(void);

/* Test fixture: Setup test parameters */
static void setup_test_parameters(double disk_factor) {
  int idx = 0;
  snprintf(MimicConfig.ModelParams[idx].param_name, MAX_STRING_LEN, "StarFormingDiskFactor");
  snprintf(MimicConfig.ModelParams[idx++].value, MAX_STRING_LEN, "%.6f", disk_factor);
  /* The required bulge-size mode, SAGE26's R0 0 (no sizes) */
  snprintf(MimicConfig.ModelParams[idx].param_name, MAX_STRING_LEN, "BulgeSizeOn");
  snprintf(MimicConfig.ModelParams[idx++].value, MAX_STRING_LEN, "%d", 0);
  MimicConfig.NumModelParams = idx;
}

/* Test fixture: Initialize module for physics tests */
static void setup_module_for_physics_test(double disk_factor) {
  reset_config();
  init_memory_system(0);
  ensure_modules_registered();

  /* Set up cosmology configuration */
  MimicConfig.Omega = 0.25;
  MimicConfig.OmegaLambda = 0.75;
  MimicConfig.Hubble_h = 0.73;
  MimicConfig.G = 43007.1; /* G in code units: (km/s)² Mpc / (1e10 Msun/h) */

  /* Configure sage26_disk_instability module in galaxy_physics */
  test_phase_add("galaxy_physics", "sage26_disk_instability", PROCESSING_MODE_BY_GALAXY);
  MimicConfig.SubSteps = 1;

  /* Set test parameters */
  setup_test_parameters(disk_factor);

  /* Initialize module system */
  int result = module_system_init();
  if (result != 0) {
    ERROR_LOG("Module system initialization failed in test fixture");
  }
}

/* Test fixture: Create test halo with specified properties */
static struct Halo create_test_halo(float vmax, float cold_gas, float stellar_mass,
                                    float bulge_mass, float disk_radius) {
  struct Halo halo;
  memset(&halo, 0, sizeof(halo));

  halo.Type = 0; /* Central galaxy */
  halo.Vmax = vmax;
  halo.HaloNr = 42;

  /* Allocate galaxy */
  halo.galaxy = mymalloc_cat(sizeof(struct GalaxyData), MEM_UTILITY);
  memset(halo.galaxy, 0, sizeof(struct GalaxyData));

  halo.galaxy->ColdGas = cold_gas;
  halo.galaxy->StellarMass = stellar_mass;
  halo.galaxy->BulgeMass = bulge_mass;
  halo.galaxy->DiskScaleRadius = disk_radius;
  halo.galaxy->MetalsStellarMass = stellar_mass * 0.02f; /* 2% metallicity */
  halo.galaxy->MetalsBulgeMass = bulge_mass * 0.02f;
  halo.galaxy->UnstableDiskGasFraction = 0.0f;

  return halo;
}

/**
 * @test    test_module_registration
 * @brief   Test that sage26_disk_instability module registers correctly
 *
 * Expected: Module registration succeeds without errors
 * Validates: sage26_disk_instability_register() works, module appears in registry
 */
int test_module_registration(void) {
  /* ===== SETUP ===== */
  reset_config();

  /* ===== EXECUTE ===== */
  ensure_modules_registered();

  /* ===== VALIDATE ===== */
  /* If we got here without crashing, registration succeeded */

  return TEST_PASS;
}

/**
 * @test    test_module_initialization
 * @brief   Test module initialization and cleanup lifecycle
 *
 * Expected: Module init and cleanup succeed without errors or leaks
 * Validates: Module lifecycle management
 */
int test_module_initialization(void) {
  /* ===== SETUP ===== */
  reset_config();
  init_memory_system(0);
  ensure_modules_registered();

  /* Set up minimal cosmology configuration */
  MimicConfig.Omega = 0.25;
  MimicConfig.OmegaLambda = 0.75;
  MimicConfig.Hubble_h = 0.73;
  MimicConfig.G = 43007.1;

  /* Configure sage26_disk_instability module */
  test_phase_add("galaxy_physics", "sage26_disk_instability", PROCESSING_MODE_BY_GALAXY);
  MimicConfig.SubSteps = 1;

  setup_test_parameters(3.0);

  /* ===== EXECUTE ===== */
  int result = module_system_init();

  /* ===== VALIDATE ===== */
  TEST_ASSERT(result == 0, "Module system initialization should succeed");

  /* ===== CLEANUP ===== */
  module_system_cleanup();
  check_memory_leaks();

  return TEST_PASS;
}

/**
 * @test    test_parameter_reading
 * @brief   Test that module reads StarFormingDiskFactor parameter
 *
 * Expected: Module reads StarFormingDiskFactor parameter successfully
 * Validates: Parameter reading and validation
 */
int test_parameter_reading(void) {
  /* ===== SETUP ===== */
  reset_config();
  init_memory_system(0);
  ensure_modules_registered();

  MimicConfig.Omega = 0.25;
  MimicConfig.OmegaLambda = 0.75;
  MimicConfig.Hubble_h = 0.73;
  MimicConfig.G = 43007.1;

  test_phase_add("galaxy_physics", "sage26_disk_instability", PROCESSING_MODE_BY_GALAXY);
  MimicConfig.SubSteps = 1;

  setup_test_parameters(5.0); /* Custom value */

  /* ===== EXECUTE ===== */
  int result = module_system_init();

  /* ===== VALIDATE ===== */
  TEST_ASSERT(result == 0, "Module should initialize with custom parameter");

  /* ===== CLEANUP ===== */
  module_system_cleanup();
  check_memory_leaks();

  return TEST_PASS;
}

/**
 * @test    test_invalid_parameter_fails
 * @brief   Test that module rejects invalid StarFormingDiskFactor values
 *
 * Expected: Module init fails with invalid parameters
 * Validates: Parameter validation (valid range: [0, 10.0])
 */
int test_invalid_parameter_fails(void) {
  /* Test cases: values that should fail */
  double invalid_values[] = {-1.0, 15.0};
  int num_cases = 2;

  for (int i = 0; i < num_cases; i++) {
    /* ===== SETUP ===== */
    reset_config();
    init_memory_system(0);
    ensure_modules_registered();

    MimicConfig.Omega = 0.25;
    MimicConfig.OmegaLambda = 0.75;
    MimicConfig.Hubble_h = 0.73;
    MimicConfig.G = 43007.1;

    test_phase_add("galaxy_physics", "sage26_disk_instability", PROCESSING_MODE_BY_GALAXY);
    MimicConfig.SubSteps = 1;

    setup_test_parameters(invalid_values[i]);

    /* ===== EXECUTE ===== */
    int result = module_system_init();

    /* ===== VALIDATE ===== */
    char error_msg[256];
    snprintf(error_msg, sizeof(error_msg),
             "Module should fail with invalid StarFormingDiskFactor=%.2f", invalid_values[i]);
    TEST_ASSERT(result != 0, error_msg);

    /* ===== CLEANUP ===== */
    /* Note: module_system_cleanup() not called since init failed */
    check_memory_leaks();
  }

  return TEST_PASS;
}

/**
 * @test    test_memory_safety
 * @brief   Test that module doesn't leak memory during normal operation
 *
 * Expected: No memory leaks after init, process, cleanup cycle
 * Validates: Memory management in module
 */
int test_memory_safety(void) {
  /* ===== SETUP ===== */
  setup_module_for_physics_test(3.0);

  /* Create test halo */
  struct Halo test_halo = create_test_halo(200.0, 5.0, 10.0, 2.0, 0.03);

  /* Create module context */
  struct ModuleContext ctx;
  memset(&ctx, 0, sizeof(ctx));
  ctx.redshift = 0.0;
  ctx.time = 13.6;
  ctx.snapshot_number = 63;
  ctx.substep_dt = 0.1;
  ctx.params = &MimicConfig;
  ctx.central_galaxy = &test_halo;

  /* ===== EXECUTE ===== */
  int result = sage26_disk_instability_process(&ctx, &test_halo, 1);

  /* ===== VALIDATE ===== */
  TEST_ASSERT(result == 0, "Module process should succeed");

  /* ===== CLEANUP ===== */
  free_test_halo(&test_halo);
  module_system_cleanup();
  check_memory_leaks();

  return TEST_PASS;
}

/**
 * @test    test_process_by_galaxy_mode
 * @brief   Test that module enforces process_by_galaxy mode (ngal=1)
 *
 * Expected: Module returns error if ngal != 1
 * Validates: Processing mode validation
 */
int test_process_by_galaxy_mode(void) {
  /* ===== SETUP ===== */
  setup_module_for_physics_test(3.0);

  /* Create multiple test halos */
  struct Halo halos[3];
  for (int i = 0; i < 3; i++) {
    halos[i] = create_test_halo(200.0, 5.0, 10.0, 2.0, 0.03);
  }

  struct ModuleContext ctx;
  memset(&ctx, 0, sizeof(ctx));
  ctx.params = &MimicConfig;
  ctx.central_galaxy = &halos[0];

  /* ===== EXECUTE ===== */
  int result = sage26_disk_instability_process(&ctx, halos, 3); /* ngal=3, should fail */

  /* ===== VALIDATE ===== */
  TEST_ASSERT(result != 0, "Module should fail with ngal != 1");

  /* ===== CLEANUP ===== */
  for (int i = 0; i < 3; i++) {
    free_test_halo(&halos[i]);
  }
  module_system_cleanup();
  check_memory_leaks();

  return TEST_PASS;
}

/**
 * @test    test_stable_disk_no_transfer
 * @brief   Test that stable disks (diskmass < Mcrit) don't transfer stars
 *
 * Expected: BulgeMass unchanged, trigger flags = 0
 * Validates: Stability criterion
 * Physics: Mcrit = Vmax² × (factor × radius) / G
 *          With Vmax=700, radius=0.5, factor=3.0: Mcrit = 17.09 > 13 (STABLE)
 */
int test_stable_disk_no_transfer(void) {
  /* ===== SETUP ===== */
  const double disk_factor = 3.0;
  setup_module_for_physics_test(disk_factor);

  /* Create stable disk: Mcrit > diskmass
   * diskmass = 5 + (10 - 2) = 13
   * Mcrit = 700² × (3.0 × 0.5) / 43007.1 = 17.09 > 13 → STABLE
   */
  struct Halo test_halo = create_test_halo(700.0, 5.0, 10.0, 2.0, 0.5);
  const float initial_bulge = test_halo.galaxy->BulgeMass;

  struct ModuleContext ctx;
  memset(&ctx, 0, sizeof(ctx));
  ctx.params = &MimicConfig;
  ctx.central_galaxy = &test_halo;

  /* ===== EXECUTE ===== */
  int result = sage26_disk_instability_process(&ctx, &test_halo, 1);

  /* ===== VALIDATE ===== */
  TEST_ASSERT(result == 0, "Process should succeed");
  TEST_ASSERT_DOUBLE_EQUAL(test_halo.galaxy->BulgeMass, initial_bulge, 1e-6,
                           "Stable disk should not transfer stars to bulge");
  TEST_ASSERT(test_halo.galaxy->UnstableDiskGasFraction == 0.0f,
              "Stable disk should have UnstableDiskGasFraction = 0");

  /* ===== CLEANUP ===== */
  free_test_halo(&test_halo);
  module_system_cleanup();
  check_memory_leaks();

  return TEST_PASS;
}

/**
 * @test    test_stable_disk_at_threshold
 * @brief   Test boundary condition where calculated Mcrit >= diskmass (stable)
 *
 * Expected: No star transfer (Mcrit capped to diskmass, unstable_mass = 0)
 * Validates: Mcrit capping logic at stability threshold
 * Physics: Mcrit slightly exceeds diskmass → capped → unstable_mass = 0
 */
int test_stable_disk_at_threshold(void) {
  /* ===== SETUP ===== */
  const double disk_factor = 3.0;
  setup_module_for_physics_test(disk_factor);

  /* Create disk at threshold for robust testing:
   * diskmass = 13.0 (ColdGas=5.0, disk_stars=8.0)
   * Calculate radius for Vmax=500 to give calculated Mcrit = 13.01 (slightly > diskmass):
   * radius = (13.01 × 43007.1) / (500² × 3.0) = 0.746029828
   * Module caps Mcrit to diskmass, so unstable_mass = diskmass - min(Mcrit, diskmass) = 0
   */
  struct Halo test_halo = create_test_halo(500.0, 5.0, 10.0, 2.0, 0.746029828);
  const float initial_bulge = test_halo.galaxy->BulgeMass;

  struct ModuleContext ctx;
  memset(&ctx, 0, sizeof(ctx));
  ctx.params = &MimicConfig;
  ctx.central_galaxy = &test_halo;

  /* ===== EXECUTE ===== */
  sage26_disk_instability_process(&ctx, &test_halo, 1);

  /* ===== VALIDATE ===== */
  TEST_ASSERT_DOUBLE_EQUAL(test_halo.galaxy->BulgeMass, initial_bulge, 1e-4,
                           "Disk at threshold (Mcrit capped) should not transfer stars");
  TEST_ASSERT(test_halo.galaxy->UnstableDiskGasFraction == 0.0f,
              "At threshold: UnstableDiskGasFraction = 0");

  /* ===== CLEANUP ===== */
  free_test_halo(&test_halo);
  module_system_cleanup();
  check_memory_leaks();

  return TEST_PASS;
}

/**
 * @test    test_zero_diskmass
 * @brief   Test handling of zero disk mass
 *
 * Expected: Trigger flags = 0, no changes
 * Validates: Edge case handling
 */
int test_zero_diskmass(void) {
  /* ===== SETUP ===== */
  setup_module_for_physics_test(3.0);

  /* Create galaxy with zero disk: ColdGas=0, StellarMass=BulgeMass */
  struct Halo test_halo = create_test_halo(200.0, 0.0, 10.0, 10.0, 0.03);

  struct ModuleContext ctx;
  memset(&ctx, 0, sizeof(ctx));
  ctx.params = &MimicConfig;
  ctx.central_galaxy = &test_halo;

  /* ===== EXECUTE ===== */
  int result = sage26_disk_instability_process(&ctx, &test_halo, 1);

  /* ===== VALIDATE ===== */
  TEST_ASSERT(result == 0, "Zero diskmass should be handled gracefully");
  TEST_ASSERT(test_halo.galaxy->UnstableDiskGasFraction == 0.0f,
              "Zero diskmass: UnstableDiskGasFraction = 0");

  /* ===== CLEANUP ===== */
  free_test_halo(&test_halo);
  module_system_cleanup();
  check_memory_leaks();

  return TEST_PASS;
}

/**
 * @test    test_unstable_disk_stars_transfer
 * @brief   Test that unstable disks transfer stars to bulge
 *
 * Expected: BulgeMass increases by unstable_stars
 * Validates: Core physics calculation
 * Physics: With Vmax=200, radius=0.03, Mcrit = 0.084 << 13 (UNSTABLE)
 */
int test_unstable_disk_stars_transfer(void) {
  /* ===== SETUP ===== */
  const double disk_factor = 3.0;
  setup_module_for_physics_test(disk_factor);

  /* Create unstable disk: diskmass > Mcrit
   * diskmass = 5 + (10 - 2) = 13
   * Mcrit = 200² × (3.0 × 0.03) / 43007.1 = 0.084 << 13 → UNSTABLE
   */
  struct Halo test_halo = create_test_halo(200.0, 5.0, 10.0, 2.0, 0.03);
  const float initial_bulge = test_halo.galaxy->BulgeMass;

  struct ModuleContext ctx;
  memset(&ctx, 0, sizeof(ctx));
  ctx.params = &MimicConfig;
  ctx.central_galaxy = &test_halo;

  /* ===== EXECUTE ===== */
  sage26_disk_instability_process(&ctx, &test_halo, 1);

  /* ===== VALIDATE ===== */
  TEST_ASSERT(test_halo.galaxy->BulgeMass > initial_bulge,
              "Unstable disk should transfer stars to bulge");

  /* Verify physics calculation matches expectations */
  const double diskmass = 13.0;
  const double Mcrit = 0.084; /* Pre-calculated */
  const double star_fraction = 8.0 / 13.0;
  const double expected_unstable_stars = star_fraction * (diskmass - Mcrit);
  const double expected_bulge = initial_bulge + expected_unstable_stars;

  TEST_ASSERT_DOUBLE_EQUAL(test_halo.galaxy->BulgeMass, expected_bulge, 0.01,
                           "BulgeMass should match expected value");

  /* ===== CLEANUP ===== */
  free_test_halo(&test_halo);
  module_system_cleanup();
  check_memory_leaks();

  return TEST_PASS;
}

/**
 * @test    test_unstable_disk_metallicity_preserved
 * @brief   Test that metallicity is preserved when transferring stars to bulge
 *
 * Expected: MetalsBulgeMass increases by Z × unstable_stars
 * Validates: Metal mass conservation
 */
int test_unstable_disk_metallicity_preserved(void) {
  /* ===== SETUP ===== */
  const double disk_factor = 3.0;
  setup_module_for_physics_test(disk_factor);

  /* Create unstable disk with non-zero metallicity */
  struct Halo test_halo = create_test_halo(200.0, 5.0, 10.0, 2.0, 0.03);
  const float initial_metals_bulge = test_halo.galaxy->MetalsBulgeMass;
  const float initial_bulge = test_halo.galaxy->BulgeMass;

  struct ModuleContext ctx;
  memset(&ctx, 0, sizeof(ctx));
  ctx.params = &MimicConfig;
  ctx.central_galaxy = &test_halo;

  /* ===== EXECUTE ===== */
  sage26_disk_instability_process(&ctx, &test_halo, 1);

  /* ===== VALIDATE ===== */
  const float transferred_stars = test_halo.galaxy->BulgeMass - initial_bulge;
  TEST_ASSERT(transferred_stars > 0.0f, "Stars should be transferred");

  /* Check metallicity preservation (2% metallicity) */
  const float expected_metals = initial_metals_bulge + 0.02f * transferred_stars;
  TEST_ASSERT_DOUBLE_EQUAL(test_halo.galaxy->MetalsBulgeMass, expected_metals, 0.001,
                           "Metallicity should be preserved in bulge transfer");

  /* ===== CLEANUP ===== */
  free_test_halo(&test_halo);
  module_system_cleanup();
  check_memory_leaks();

  return TEST_PASS;
}

/**
 * @test    test_unstable_disk_gas_only
 * @brief   Test unstable disk with only gas (no stellar disk)
 *
 * Expected: Only gas unstable, triggers set appropriately
 * Validates: Gas-only disk edge case
 */
int test_unstable_disk_gas_only(void) {
  /* ===== SETUP ===== */
  const double disk_factor = 3.0;
  setup_module_for_physics_test(disk_factor);

  /* Create gas-only disk: StellarMass = BulgeMass (no stellar disk) */
  struct Halo test_halo = create_test_halo(200.0, 10.0, 2.0, 2.0, 0.03);
  const float initial_bulge = test_halo.galaxy->BulgeMass;

  struct ModuleContext ctx;
  memset(&ctx, 0, sizeof(ctx));
  ctx.params = &MimicConfig;
  ctx.central_galaxy = &test_halo;

  /* ===== EXECUTE ===== */
  sage26_disk_instability_process(&ctx, &test_halo, 1);

  /* ===== VALIDATE ===== */
  /* Gas-only disk: no stars to transfer */
  TEST_ASSERT_DOUBLE_EQUAL(test_halo.galaxy->BulgeMass, initial_bulge, 1e-6,
                           "Gas-only disk should not transfer stars");
  TEST_ASSERT(test_halo.galaxy->UnstableDiskGasFraction > 0.0f,
              "Gas-only unstable disk should set gas trigger flag");

  /* ===== CLEANUP ===== */
  free_test_halo(&test_halo);
  module_system_cleanup();
  check_memory_leaks();

  return TEST_PASS;
}

/**
 * @test    test_unstable_disk_stars_only
 * @brief   Test unstable disk with only stars (no gas)
 *
 * Expected: Stars transferred, trigger flags = 0 (only set if unstable_gas > 0)
 * Validates: Stars-only disk behavior
 * Note: Module only sets triggers if unstable_gas > 0 and ColdGas > 0
 *       (R/src/model_disk_instability.c:90)
 */
int test_unstable_disk_stars_only(void) {
  /* ===== SETUP ===== */
  const double disk_factor = 3.0;
  setup_module_for_physics_test(disk_factor);

  /* Create stars-only disk: ColdGas = 0 */
  struct Halo test_halo = create_test_halo(200.0, 0.0, 10.0, 2.0, 0.03);
  const float initial_bulge = test_halo.galaxy->BulgeMass;

  struct ModuleContext ctx;
  memset(&ctx, 0, sizeof(ctx));
  ctx.params = &MimicConfig;
  ctx.central_galaxy = &test_halo;

  /* ===== EXECUTE ===== */
  sage26_disk_instability_process(&ctx, &test_halo, 1);

  /* ===== VALIDATE ===== */
  TEST_ASSERT(test_halo.galaxy->BulgeMass > initial_bulge,
              "Stars-only unstable disk should transfer stars");

  /* Trigger flag is ONLY set if unstable_gas > 0 and ColdGas > 0
   * (R/src/model_disk_instability.c:90) */
  TEST_ASSERT(test_halo.galaxy->UnstableDiskGasFraction == 0.0f,
              "Stars-only disk should have gas trigger flag = 0 (no gas)");

  /* ===== CLEANUP ===== */
  free_test_halo(&test_halo);
  module_system_cleanup();
  check_memory_leaks();

  return TEST_PASS;
}

/**
 * @test    test_zero_vmax
 * @brief   Test handling of zero virial velocity
 *
 * Expected: Mcrit = 0, all disk mass becomes unstable
 * Validates: Division by zero protection
 */
int test_zero_vmax(void) {
  /* ===== SETUP ===== */
  setup_module_for_physics_test(3.0);

  /* Create galaxy with Vmax = 0 → Mcrit = 0 → all unstable */
  struct Halo test_halo = create_test_halo(0.0, 5.0, 10.0, 2.0, 0.03);
  const float initial_bulge = test_halo.galaxy->BulgeMass;

  struct ModuleContext ctx;
  memset(&ctx, 0, sizeof(ctx));
  ctx.params = &MimicConfig;
  ctx.central_galaxy = &test_halo;

  /* ===== EXECUTE ===== */
  int result = sage26_disk_instability_process(&ctx, &test_halo, 1);

  /* ===== VALIDATE ===== */
  TEST_ASSERT(result == 0, "Zero Vmax should be handled gracefully");

  /* Vmax=0 → Mcrit=0 → entire stellar disk unstable */
  const float disk_stars = 10.0f - 2.0f; /* StellarMass - BulgeMass */
  TEST_ASSERT_DOUBLE_EQUAL(test_halo.galaxy->BulgeMass, initial_bulge + disk_stars, 0.01,
                           "Zero Vmax: all stellar disk should transfer to bulge");

  /* ===== CLEANUP ===== */
  free_test_halo(&test_halo);
  module_system_cleanup();
  check_memory_leaks();

  return TEST_PASS;
}

/**
 * @test    test_zero_disk_radius
 * @brief   Test handling of zero disk scale radius
 *
 * Expected: Mcrit = 0, all disk mass becomes unstable
 * Validates: Edge case handling
 */
int test_zero_disk_radius(void) {
  /* ===== SETUP ===== */
  setup_module_for_physics_test(3.0);

  /* Create galaxy with DiskScaleRadius = 0 → Mcrit = 0 */
  struct Halo test_halo = create_test_halo(200.0, 5.0, 10.0, 2.0, 0.0);
  const float initial_bulge = test_halo.galaxy->BulgeMass;

  struct ModuleContext ctx;
  memset(&ctx, 0, sizeof(ctx));
  ctx.params = &MimicConfig;
  ctx.central_galaxy = &test_halo;

  /* ===== EXECUTE ===== */
  int result = sage26_disk_instability_process(&ctx, &test_halo, 1);

  /* ===== VALIDATE ===== */
  TEST_ASSERT(result == 0, "Zero DiskScaleRadius should be handled gracefully");

  const float disk_stars = 10.0f - 2.0f;
  TEST_ASSERT_DOUBLE_EQUAL(test_halo.galaxy->BulgeMass, initial_bulge + disk_stars, 0.01,
                           "Zero radius: all stellar disk should transfer to bulge");

  /* ===== CLEANUP ===== */
  free_test_halo(&test_halo);
  module_system_cleanup();
  check_memory_leaks();

  return TEST_PASS;
}

/* ---------------------------------------------------------------------------
 * No cap and no clamp (SAGE26 R/src/model_disk_instability.c:77-79)
 *
 * SAGE26 adds the double unstable_stars and its metallicity product straight into
 * the float bulge fields. With Vmax = 0 (Mcrit = 0) and ColdGas = 0 the whole float
 * disk difference is transferred, and the float stores can leave BulgeMass or
 * MetalsBulgeMass one rounding above StellarMass or MetalsStellarMass. SAGE26 keeps
 * that; it has no cap on the transfer and no clamp of the bulge.
 *
 * The inputs and expected bit patterns below come from executing SAGE26's own
 * compiled check_disk_instability (SAGE26 commit 11b1ae85, Apple clang 21, the
 * oracle's compile flags) on exactly these inputs, with G = 43007.1, AGNrecipeOn 2
 * and BulgeSizeOn 0. ColdGas = 0 keeps SAGE26's routine from reaching its
 * black-hole and burst calls, so its end state is the transfer alone. SAGE26 left
 * the bulge radii (set to 0.25, 0.125 and 0.0625) unchanged on every input.
 * --------------------------------------------------------------------------- */

static float f32_from_bits(const uint32_t bits) {
  float value;
  memcpy(&value, &bits, sizeof(value));
  return value;
}

static uint32_t f32_bits(const float value) {
  uint32_t bits;
  memcpy(&bits, &value, sizeof(bits));
  return bits;
}

/** @brief Inputs (float bit patterns) and SAGE26's resulting bulge fields */
struct sage26_transfer_case {
  uint32_t stellar_mass;
  uint32_t bulge_mass;
  uint32_t metals_stellar_mass;
  uint32_t metals_bulge_mass;
  uint32_t expected_bulge_mass;
  uint32_t expected_metals_bulge_mass;
  uint32_t expected_instability_bulge_mass; /* from InstabilityBulgeMass = 0 */
};

/**
 * @brief Run the module on a ColdGas = 0, Vmax = 0 galaxy and compare with SAGE26's bits
 * @return TEST_PASS or TEST_FAIL
 */
static int run_sage26_transfer_case(const struct sage26_transfer_case *c) {
  setup_module_for_physics_test(3.0);

  struct Halo test_halo =
      create_test_halo(0.0f, 0.0f, f32_from_bits(c->stellar_mass), f32_from_bits(c->bulge_mass),
                       f32_from_bits(0x3c23d70au) /* 0.01 */);
  struct GalaxyData *gal = test_halo.galaxy;
  gal->MetalsStellarMass = f32_from_bits(c->metals_stellar_mass);
  gal->MetalsBulgeMass = f32_from_bits(c->metals_bulge_mass);
  gal->InstabilityBulgeMass = 0.0f;
  gal->BulgeRadius = 0.25f;
  gal->MergerBulgeRadius = 0.125f;
  gal->InstabilityBulgeRadius = 0.0625f;

  struct ModuleContext ctx;
  memset(&ctx, 0, sizeof(ctx));
  ctx.params = &MimicConfig;
  ctx.central_galaxy = &test_halo;

  int result = sage26_disk_instability_process(&ctx, &test_halo, 1);

  TEST_ASSERT(result == 0, "Full disk transfer should complete");
  TEST_ASSERT(f32_bits(gal->BulgeMass) == c->expected_bulge_mass,
              "BulgeMass must equal SAGE26's uncapped, unclamped result bit for bit");
  TEST_ASSERT(f32_bits(gal->MetalsBulgeMass) == c->expected_metals_bulge_mass,
              "MetalsBulgeMass must equal SAGE26's uncapped, unclamped result bit for bit");
  TEST_ASSERT(f32_bits(gal->InstabilityBulgeMass) == c->expected_instability_bulge_mass,
              "InstabilityBulgeMass must receive SAGE26's unstable_stars bit for bit");
  TEST_ASSERT(gal->UnstableDiskGasFraction == 0.0, "No cold gas: no unstable gas is handed on");
  TEST_ASSERT(gal->BulgeRadius == 0.25f && gal->MergerBulgeRadius == 0.125f &&
                  gal->InstabilityBulgeRadius == 0.0625f,
              "BulgeSizeOn 0: the bulge radii keep their values (R/src/model_misc.c:395)");

  free_test_halo(&test_halo);
  module_system_cleanup();
  check_memory_leaks();
  return TEST_PASS;
}

/**
 * @test    test_full_disk_transfer_is_not_capped
 * @brief   A full stellar-disk transfer is not capped to the disk's stars or metals
 *
 * Expected: SAGE26's bits; BulgeMass ends one float above StellarMass, as in SAGE26
 * Validates: no transfer cap and no bulge clamp (R/src/model_disk_instability.c:77-79)
 */
int test_full_disk_transfer_is_not_capped(void) {
  const struct sage26_transfer_case c = {
      .stellar_mass = 0x423aed91u,        /* 46.7320 */
      .bulge_mass = 0x412b39deu,          /* 10.7016 */
      .metals_stellar_mass = 0x3ee721fau, /* 0.451431 */
      .metals_bulge_mass = 0x3ec7b2dcu,   /* 0.390036 */
      .expected_bulge_mass = 0x423aed92u, /* one float above StellarMass */
      .expected_metals_bulge_mass = 0x3ee721fau,
      .expected_instability_bulge_mass = 0x42101f1au,
  };
  const int status = run_sage26_transfer_case(&c);
  TEST_ASSERT(f32_from_bits(c.expected_bulge_mass) > f32_from_bits(c.stellar_mass),
              "This SAGE26 case leaves BulgeMass above StellarMass");
  return status;
}

/**
 * @test    test_bulge_mass_not_clamped_to_stellar_mass
 * @brief   BulgeMass above StellarMass after a transfer is kept, not clamped
 *
 * Expected: SAGE26's bits; BulgeMass ends one float above StellarMass
 * Validates: the sage16-era clamp of BulgeMass to StellarMass is gone
 *            (absent from R/src/model_disk_instability.c:77-84)
 */
int test_bulge_mass_not_clamped_to_stellar_mass(void) {
  const struct sage26_transfer_case c = {
      .stellar_mass = 0x4222b53fu,        /* 40.6770 */
      .bulge_mass = 0x4102d136u,          /* 8.17607 */
      .metals_stellar_mass = 0x3dc644fau, /* 0.0968112 */
      .metals_bulge_mass = 0x3cb27148u,   /* 0.0217903 */
      .expected_bulge_mass = 0x4222b540u, /* one float above StellarMass */
      .expected_metals_bulge_mass = 0x3dc644fau,
      .expected_instability_bulge_mass = 0x420200f2u,
  };
  TEST_ASSERT(f32_from_bits(c.expected_bulge_mass) > f32_from_bits(c.stellar_mass),
              "This SAGE26 case leaves BulgeMass above StellarMass");
  return run_sage26_transfer_case(&c);
}

/**
 * @test    test_metals_bulge_not_clamped_to_stellar_metals
 * @brief   MetalsBulgeMass above MetalsStellarMass after a transfer is kept, not clamped
 *
 * Expected: SAGE26's bits; MetalsBulgeMass ends one float above MetalsStellarMass
 * Validates: no cap of the transferred metals to the disk metals and no clamp of the
 *            bulge metals (R/src/model_disk_instability.c:75, :79)
 */
int test_metals_bulge_not_clamped_to_stellar_metals(void) {
  const struct sage26_transfer_case c = {
      .stellar_mass = 0x42079893u,               /* 33.8990 */
      .bulge_mass = 0x419b649du,                 /* 19.4241 */
      .metals_stellar_mass = 0x3eb9b657u,        /* 0.362719 */
      .metals_bulge_mass = 0x3db5ff7eu,          /* 0.0888662 */
      .expected_bulge_mass = 0x42079893u,        /* equal to StellarMass */
      .expected_metals_bulge_mass = 0x3eb9b658u, /* one float above MetalsStellarMass */
      .expected_instability_bulge_mass = 0x41679912u,
  };
  TEST_ASSERT(f32_from_bits(c.expected_metals_bulge_mass) > f32_from_bits(c.metals_stellar_mass),
              "This SAGE26 case leaves MetalsBulgeMass above MetalsStellarMass");
  return run_sage26_transfer_case(&c);
}

/**
 * @test    test_disk_factor_sensitivity
 * @brief   Test that changing StarFormingDiskFactor affects stability
 *
 * Expected: Higher disk_factor → higher Mcrit → more stability
 * Validates: Parameter sensitivity
 */
int test_disk_factor_sensitivity(void) {
  /* ===== TEST LOW DISK FACTOR ===== */
  setup_module_for_physics_test(1.0); /* Low factor → low Mcrit → unstable */

  struct Halo test_halo_low = create_test_halo(200.0, 5.0, 10.0, 2.0, 0.03);
  const float initial_bulge_low = test_halo_low.galaxy->BulgeMass;

  struct ModuleContext ctx;
  memset(&ctx, 0, sizeof(ctx));
  ctx.params = &MimicConfig;
  ctx.central_galaxy = &test_halo_low;

  sage26_disk_instability_process(&ctx, &test_halo_low, 1);
  const float bulge_low = test_halo_low.galaxy->BulgeMass;
  const float transferred_low = bulge_low - initial_bulge_low;

  free_test_halo(&test_halo_low);
  module_system_cleanup();

  /* ===== TEST HIGH DISK FACTOR ===== */
  setup_module_for_physics_test(10.0); /* High factor → high Mcrit → stable */

  struct Halo test_halo_high = create_test_halo(200.0, 5.0, 10.0, 2.0, 0.03);
  const float initial_bulge_high = test_halo_high.galaxy->BulgeMass;

  ctx.params = &MimicConfig;
  ctx.central_galaxy = &test_halo_high;

  sage26_disk_instability_process(&ctx, &test_halo_high, 1);
  const float bulge_high = test_halo_high.galaxy->BulgeMass;
  const float transferred_high = bulge_high - initial_bulge_high;

  /* ===== VALIDATE ===== */
  TEST_ASSERT(transferred_low > transferred_high,
              "Lower disk_factor should produce more instability (more star transfer)");

  /* ===== CLEANUP ===== */
  free_test_halo(&test_halo_high);
  module_system_cleanup();
  check_memory_leaks();

  return TEST_PASS;
}

// ============================================================================
// FEEDBACK-FREE-BURST BYPASS (c13 Slice 3)
// ============================================================================

/**
 * @test    test_ffb_row_bypassed
 * @brief   An FFBRegime 1 row is not checked; the same row with FFBRegime 0 is
 *
 * SAGE26's starformation_and_feedback hands an FFBRegime 1 row to starformation_ffb, which
 * returns with no disk-instability check (R/src/model_starformation_and_feedback.c:1004-1008,
 * :1365-1367). The row is a satellite on an unstable disk (diskmass 13 far above Mcrit) with its
 * own FoF central; with FFBRegime 1 both galaxy structs are compared byte for byte with their
 * inputs, so no stars move and UnstableDiskGasFraction keeps its reset value 0. With FFBRegime 0
 * the trigger is SAGE26's check_disk_instability (R/src/model_disk_instability.c:54-101), written
 * here with its operand order and float sums.
 */
int test_ffb_row_bypassed(void) {
  setup_module_for_physics_test(3.0);
  struct Halo central = create_test_halo(150.0, 2.0, 4.0, 1.0, 0.02);
  struct Halo row = create_test_halo(200.0, 5.0, 10.0, 2.0, 0.03);
  row.Type = 1;
  row.galaxy->FFBRegime = 1;
  struct ModuleContext ctx;
  memset(&ctx, 0, sizeof(ctx));
  ctx.params = &MimicConfig;
  ctx.central_galaxy = &central;

  struct GalaxyData row_in, central_in;
  memcpy(&row_in, row.galaxy, sizeof(row_in));
  memcpy(&central_in, central.galaxy, sizeof(central_in));

  TEST_ASSERT(sage26_disk_instability_process(&ctx, &row, 1) == 0,
              "process succeeds on an FFB row");
  TEST_ASSERT(memcmp(row.galaxy, &row_in, sizeof(row_in)) == 0,
              "FFBRegime 1: every field of the row is unchanged (no transfer, no trigger)");
  TEST_ASSERT(memcmp(central.galaxy, &central_in, sizeof(central_in)) == 0,
              "FFBRegime 1: every field of its central is unchanged");

  row.galaxy->FFBRegime = 0;
  TEST_ASSERT(sage26_disk_instability_process(&ctx, &row, 1) == 0,
              "process succeeds on an ordinary row");
  const float vmax = 200.0f;
  const double diskmass = row_in.ColdGas + (row_in.StellarMass - row_in.BulgeMass);  /* :54 */
  const double Mcrit = vmax * vmax * (3.0 * row_in.DiskScaleRadius) / MimicConfig.G; /* :57 */
  const double gas_fraction = row_in.ColdGas / diskmass;                             /* :63 */
  const double unstable_gas = gas_fraction * (diskmass - Mcrit);                     /* :64 */
  const double expected = unstable_gas / row_in.ColdGas;                             /* :101 */
  printf("\n  FFBRegime 0: UnstableDiskGasFraction %.17g, expected %.17g (Mcrit %.6g)",
         row.galaxy->UnstableDiskGasFraction, expected, Mcrit);
  TEST_ASSERT(Mcrit < diskmass, "the disk is unstable");
  TEST_ASSERT(fabs(row.galaxy->UnstableDiskGasFraction - expected) <= 1e-12 * expected,
              "FFBRegime 0: the trigger is SAGE26's unstable gas fraction");
  TEST_ASSERT(row.galaxy->BulgeMass > row_in.BulgeMass, "FFBRegime 0: stars move to the bulge");
  TEST_ASSERT(memcmp(central.galaxy, &central_in, sizeof(central_in)) == 0,
              "the check never writes the central");

  free_test_halo(&row);
  free_test_halo(&central);
  module_system_cleanup();
  check_memory_leaks();
  return TEST_PASS;
}

/**
 * @brief   Main test runner
 *
 * Executes all sage26_disk_instability comprehensive tests and reports results.
 */
int main(void) {
  printf("%s", BLUE);
  printf("============================================================\n");
  printf("Test Suite: sage26_disk_instability Module (Comprehensive)\n");
  printf("============================================================\n");
  printf("%s", NC);

  /* Initialize error handling for tests */
  initialize_error_handling(LOG_LEVEL_WARNING, NULL);

  /* Run software quality tests */
  printf("\n%s=== Software Quality Tests ===%s\n", BLUE, NC);
  TEST_RUN(test_module_registration);
  TEST_RUN(test_module_initialization);
  TEST_RUN(test_parameter_reading);
  TEST_RUN(test_invalid_parameter_fails);
  TEST_RUN(test_memory_safety);
  TEST_RUN(test_process_by_galaxy_mode);

  /* Run physics tests - stable disks */
  printf("\n%s=== Physics Tests: Stable Disks ===%s\n", BLUE, NC);
  TEST_RUN(test_stable_disk_no_transfer);
  TEST_RUN(test_stable_disk_at_threshold);
  TEST_RUN(test_zero_diskmass);

  /* Run physics tests - unstable disks */
  printf("\n%s=== Physics Tests: Unstable Disks ===%s\n", BLUE, NC);
  TEST_RUN(test_unstable_disk_stars_transfer);
  TEST_RUN(test_unstable_disk_metallicity_preserved);
  TEST_RUN(test_unstable_disk_gas_only);
  TEST_RUN(test_unstable_disk_stars_only);

  /* Run edge case tests */
  printf("\n%s=== Edge Case Tests ===%s\n", BLUE, NC);
  TEST_RUN(test_zero_vmax);
  TEST_RUN(test_zero_disk_radius);
  TEST_RUN(test_full_disk_transfer_is_not_capped);
  TEST_RUN(test_bulge_mass_not_clamped_to_stellar_mass);
  TEST_RUN(test_metals_bulge_not_clamped_to_stellar_metals);

  /* Run parameter sensitivity tests */
  printf("\n%s=== Parameter Sensitivity Tests ===%s\n", BLUE, NC);
  TEST_RUN(test_disk_factor_sensitivity);

  /* The feedback-free-burst bypass (c13 Slice 3) */
  TEST_RUN(test_ffb_row_bypassed);

  /* Print summary and return result */
  TEST_SUMMARY();
  return TEST_RESULT();
}
