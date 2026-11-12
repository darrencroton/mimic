/**
 * @file    test_unit_sage_disk_instability.c
 * @brief   Comprehensive unit tests for sage_disk_instability module
 *
 * Validates: module initialisation, the disk stability criterion and the transfer it drives, the
 * guards, and the clamps of the bulge to the stellar totals
 *
 * - Disk stability criterion (Efstathiou; Mo, Mao & White 1998): Mcrit = Vmax^2 (factor * radius)
 *   / G capped at the disk mass
 * - Stellar mass transfer to the bulge with metallicity preservation, and the unstable gas
 *   fraction handed to the downstream modules
 * - Edge cases (zero or negative disk mass, zero Vmax, zero radius) and the clamps of the bulge
 *   and its metals to the stellar totals
 *
 * Test cases:
 *   - test_module_initialization: module_system_init accepts the module's placement
 *   - test_process_by_galaxy_mode: ngal != 1 is refused
 *   - test_stable_disk_no_transfer: a table of disks that are stable or empty, each left
 *     bytewise unchanged
 *   - test_unstable_disk_transfer: a table of unstable disks, each asserting the stars moved,
 *     their metals and the unstable gas fraction
 *   - test_full_disk_transfer_clamps_to_available_mass: a full-disk transfer leaves the bulge and
 *     its metals at the stellar totals
 *   - test_transfer_clamps_to_stellar_totals: float bit patterns whose unclamped sum rounds above
 *     the stellar total, for the stars and for the metals, are clamped to it
 *
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

#include <stdio.h>
#include <string.h>
#include <math.h>
#include <float.h>
#include <stdint.h>

/* Shared SAGE16 test fixture boilerplate (counters, config reset, module registration) */
#include "modules/_tests/sage_test_fixtures.h"

/* Module functions (extern declarations for direct testing) */
extern int sage_disk_instability_init(void);
extern int sage_disk_instability_process(struct ModuleContext *ctx, struct Halo *halos, int ngal);
extern int sage_disk_instability_cleanup(void);

/* Test fixture: Setup test parameters */
static void setup_test_parameters(double disk_factor) {
  int idx = 0;
  snprintf(MimicConfig.ModelParams[idx].param_name, MAX_STRING_LEN, "StarFormingDiskFactor");
  snprintf(MimicConfig.ModelParams[idx++].value, MAX_STRING_LEN, "%.6f", disk_factor);
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

  /* Configure sage_disk_instability module in galaxy_physics */
  test_phase_add("galaxy_physics", "sage_disk_instability", PROCESSING_MODE_BY_GALAXY);
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

  /* Configure sage_disk_instability module */
  test_phase_add("galaxy_physics", "sage_disk_instability", PROCESSING_MODE_BY_GALAXY);
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
  int result = sage_disk_instability_process(&ctx, halos, 3); /* ngal=3, should fail */

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

/** @brief Fails the case naming the table row and the quantity that missed */
#define ROW_ASSERT(cond, row, what)                                                                \
  do {                                                                                             \
    char row_msg[160];                                                                             \
    snprintf(row_msg, sizeof(row_msg), "row '%s': %s", (row)->name, (what));                       \
    TEST_ASSERT(cond, row_msg);                                                                    \
  } while (0)

/** @brief A central with one disk and, for the unstable table, what the module must make of it */
struct disk_row {
  const char *name;
  double disk_factor; /**< StarFormingDiskFactor */
  float vmax;
  float cold_gas;
  float stellar_mass;
  float bulge_mass;
  float disk_radius;
  double stars;        /**< expected stars moved to the bulge */
  double gas_fraction; /**< expected UnstableDiskGasFraction */
};

/**
 * @test    test_stable_disk_no_transfer
 * @brief   A disk whose critical mass reaches its mass, or that has no mass, is left untouched
 *
 * Expected: every field of the galaxy is unchanged (no stars move, no metals move,
 *           UnstableDiskGasFraction stays 0)
 * Validates: the stability criterion Mcrit = Vmax^2 (factor * radius) / G capped at the disk mass
 *            (Mo, Mao & White 1998), and the disk-mass guard
 * Physics: diskmass = ColdGas + (StellarMass - BulgeMass); at G = 43007.1 the first two rows have
 *          Mcrit 17.09 and 13.01 against a disk of 13, the others a disk of 0 and of -1
 */
int test_stable_disk_no_transfer(void) {
  static const struct disk_row rows[] = {
      {"mcrit_above_disk", 3.0, 700.0f, 5.0f, 10.0f, 2.0f, 0.5f, 0.0, 0.0},
      {"mcrit_just_above_disk", 3.0, 500.0f, 5.0f, 10.0f, 2.0f, 0.746029828f, 0.0, 0.0},
      {"zero_disk_mass", 3.0, 200.0f, 0.0f, 10.0f, 10.0f, 0.03f, 0.0, 0.0},
      {"negative_disk_mass", 3.0, 200.0f, 1.0f, 8.0f, 10.0f, 0.03f, 0.0, 0.0},
  };
  for (size_t k = 0; k < sizeof(rows) / sizeof(rows[0]); k++) {
    const struct disk_row *r = &rows[k];
    setup_module_for_physics_test(r->disk_factor);
    struct Halo test_halo =
        create_test_halo(r->vmax, r->cold_gas, r->stellar_mass, r->bulge_mass, r->disk_radius);
    struct GalaxyData before;
    memcpy(&before, test_halo.galaxy, sizeof(before));

    struct ModuleContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.params = &MimicConfig;
    ctx.central_galaxy = &test_halo;

    ROW_ASSERT(sage_disk_instability_process(&ctx, &test_halo, 1) == 0, r, "process succeeds");
    ROW_ASSERT(memcmp(test_halo.galaxy, &before, sizeof(before)) == 0, r,
               "a stable or empty disk leaves every field unchanged");

    free_test_halo(&test_halo);
    module_system_cleanup();
  }
  check_memory_leaks();
  return TEST_PASS;
}

/**
 * @test    test_unstable_disk_transfer
 * @brief   The stars an unstable disk moves to the bulge, their metals and the unstable gas
 *          fraction handed on
 *
 * Expected: with diskmass = ColdGas + (StellarMass - BulgeMass), gas fraction f = ColdGas /
 *           diskmass and Mcrit = Vmax^2 (factor * radius) / G (G = 43007.1) capped at diskmass:
 *           stars moved = (1 - f)(diskmass - Mcrit), UnstableDiskGasFraction = f (diskmass -
 *           Mcrit) / ColdGas when positive, and the bulge metals gain the disk's metallicity
 *           times the stars moved. The literals are those formulas, hand-evaluated.
 * Validates: the transfer, the gas branch, Mcrit 0 (Vmax 0 or radius 0 moves the whole disk), the
 *            dependence on StarFormingDiskFactor, and a disk just on the unstable side of the cap
 *            (the transfer is also capped at the disk's stars, which these rows do not reach)
 */
int test_unstable_disk_transfer(void) {
  static const struct disk_row rows[] = {
      /* diskmass 13, Mcrit 0.0837071 */
      {"stars_and_gas", 3.0, 200.0f, 5.0f, 10.0f, 2.0f, 0.03f, 7.948487934, 0.9935609918},
      /* a gas-only disk (StellarMass = BulgeMass): no stars move, the gas fraction is
       * 1 - Mcrit / ColdGas */
      {"gas_only", 3.0, 200.0f, 10.0f, 2.0f, 2.0f, 0.03f, 0.0, 0.9916292893},
      /* a stars-only disk: the stars move and there is no gas to hand on */
      {"stars_only", 3.0, 200.0f, 0.0f, 10.0f, 2.0f, 0.03f, 7.916292893, 0.0},
      /* Mcrit 0: the whole stellar disk (8) moves and all the gas is unstable */
      {"zero_vmax", 3.0, 0.0f, 5.0f, 10.0f, 2.0f, 0.03f, 8.0, 1.0},
      {"zero_disk_radius", 3.0, 200.0f, 5.0f, 10.0f, 2.0f, 0.0f, 8.0, 1.0},
      /* the factor scales Mcrit: 0.0279024 at 1.0 and 0.279024 at 10.0 (3.0 is the first row) */
      {"factor_1", 1.0, 200.0f, 5.0f, 10.0f, 2.0f, 0.03f, 7.982829311, 0.9978536639},
      {"factor_10", 10.0, 200.0f, 5.0f, 10.0f, 2.0f, 0.03f, 7.828293114, 0.9785366392},
      /* Mcrit 12.9903 just below the disk's 13: only (8 / 13)(0.0097033) moves */
      {"mcrit_just_below_disk", 3.0, 500.0f, 5.0f, 10.0f, 2.0f, 0.7449f, 0.005971234004,
       0.0007464042506},
  };
  for (size_t k = 0; k < sizeof(rows) / sizeof(rows[0]); k++) {
    const struct disk_row *r = &rows[k];
    setup_module_for_physics_test(r->disk_factor);
    struct Halo test_halo =
        create_test_halo(r->vmax, r->cold_gas, r->stellar_mass, r->bulge_mass, r->disk_radius);
    struct GalaxyData *gal = test_halo.galaxy;
    const double bulge0 = gal->BulgeMass, metals_bulge0 = gal->MetalsBulgeMass;
    /* the stellar disk's metallicity, from the fixture's float metals (no disk stars, no stars to
     * move) */
    const double disk_stars = (double)gal->StellarMass - (double)gal->BulgeMass;
    const double metallicity =
        disk_stars > 0.0
            ? ((double)gal->MetalsStellarMass - (double)gal->MetalsBulgeMass) / disk_stars
            : 0.0;

    struct ModuleContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.params = &MimicConfig;
    ctx.central_galaxy = &test_halo;

    ROW_ASSERT(sage_disk_instability_process(&ctx, &test_halo, 1) == 0, r, "process succeeds");
    /* the bulge fields are floats: a tolerance of a few float steps of the result */
    ROW_ASSERT(fabs(gal->BulgeMass - (bulge0 + r->stars)) <=
                   2.0 * FLT_EPSILON * (bulge0 + r->stars),
               r, "BulgeMass gains the stars moved");
    const double metals_expected = metals_bulge0 + metallicity * r->stars;
    ROW_ASSERT(fabs(gal->MetalsBulgeMass - metals_expected) <= 4.0 * FLT_EPSILON * metals_expected,
               r, "MetalsBulgeMass gains the disk metallicity times the stars moved");
    ROW_ASSERT(fabs(gal->UnstableDiskGasFraction - r->gas_fraction) <=
                   2.0 * FLT_EPSILON * r->gas_fraction,
               r, "UnstableDiskGasFraction");

    free_test_halo(&test_halo);
    module_system_cleanup();
  }
  check_memory_leaks();
  return TEST_PASS;
}

/**
 * @test    test_full_disk_transfer_clamps_to_available_mass
 * @brief   Test full stellar disk transfer at the bulge/stellar boundary
 *
 * Expected: Bulge and bulge metals snap exactly to the available total reservoirs
 * Validates: Transfer capping prevents tolerance-level component overshoot
 */
int test_full_disk_transfer_clamps_to_available_mass(void) {
  /* ===== SETUP ===== */
  setup_module_for_physics_test(3.0);

  struct Halo test_halo = create_test_halo(0.0, 5.0e-4f, 1.3544e-4f, 5.0e-5f, 0.03f);
  test_halo.galaxy->MetalsStellarMass = 2.8763e-8f;
  test_halo.galaxy->MetalsBulgeMass = 1.0e-8f;

  struct ModuleContext ctx;
  memset(&ctx, 0, sizeof(ctx));
  ctx.params = &MimicConfig;
  ctx.central_galaxy = &test_halo;

  /* ===== EXECUTE ===== */
  int result = sage_disk_instability_process(&ctx, &test_halo, 1);

  /* ===== VALIDATE ===== */
  TEST_ASSERT(result == 0, "Full disk transfer should complete");
  TEST_ASSERT(test_halo.galaxy->BulgeMass <= test_halo.galaxy->StellarMass,
              "BulgeMass should not exceed StellarMass after full disk transfer");
  TEST_ASSERT(test_halo.galaxy->MetalsBulgeMass <= test_halo.galaxy->MetalsStellarMass,
              "MetalsBulgeMass should not exceed MetalsStellarMass after full disk transfer");
  TEST_ASSERT_DOUBLE_EQUAL(test_halo.galaxy->BulgeMass, test_halo.galaxy->StellarMass, 1e-10,
                           "Full disk transfer should leave all stars in bulge");
  TEST_ASSERT_DOUBLE_EQUAL(test_halo.galaxy->MetalsBulgeMass, test_halo.galaxy->MetalsStellarMass,
                           1e-12, "Full disk transfer should leave all stellar metals in bulge");

  /* ===== CLEANUP ===== */
  free_test_halo(&test_halo);
  module_system_cleanup();
  check_memory_leaks();

  return TEST_PASS;
}

/* ---------------------------------------------------------------------------
 * The clamps of the bulge to the stellar totals
 *
 * The transfer adds the double unstable stars into the float BulgeMass, and the metals into the
 * float MetalsBulgeMass, so a full-disk transfer can leave either one float rounding above its
 * total; SAGE16 then clamps the bulge to the total (the overshoot is within the warning
 * tolerance, so nothing is logged). With Vmax = 0 (Mcrit = 0) and ColdGas = 0 the whole float disk
 * difference is transferred. The inputs below are float bit patterns for which the unclamped
 * float sum is one float above the total, for the stars in the first two rows and for the
 * metals in the third.
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

/** @brief A full-disk transfer's inputs (float bit patterns) and the total that is clamped */
struct clamp_row {
  const char *name;
  uint32_t stellar_mass;
  uint32_t bulge_mass;
  uint32_t metals_stellar_mass;
  uint32_t metals_bulge_mass;
  int clamps_stars;  /**< the unclamped BulgeMass would exceed StellarMass */
  int clamps_metals; /**< the unclamped MetalsBulgeMass would exceed MetalsStellarMass */
};

/**
 * @test    test_transfer_clamps_to_stellar_totals
 * @brief   A full-disk transfer whose float sum rounds above the stellar total is clamped to it
 *
 * Expected: BulgeMass is bit for bit StellarMass, and MetalsBulgeMass bit for bit
 *           MetalsStellarMass, where the unclamped float sum exceeds the total (the first two
 *           rows for the stars, the third for the metals)
 * Validates: the clamp of the bulge and of the bulge metals to the stellar totals
 */
int test_transfer_clamps_to_stellar_totals(void) {
  static const struct clamp_row rows[] = {
      {"bulge_rounds_above_stellar_mass", 0x423aed91u /* 46.7320 */, 0x412b39deu /* 10.7016 */,
       0x3ee721fau /* 0.451431 */, 0x3ec7b2dcu /* 0.390036 */, 1, 0},
      {"bulge_rounds_above_stellar_mass_2", 0x4222b53fu /* 40.6770 */, 0x4102d136u /* 8.17607 */,
       0x3dc644fau /* 0.0968112 */, 0x3cb27148u /* 0.0217903 */, 1, 0},
      {"metals_round_above_stellar_metals", 0x42079893u /* 33.8990 */, 0x419b649du /* 19.4241 */,
       0x3eb9b657u /* 0.362719 */, 0x3db5ff7eu /* 0.0888662 */, 0, 1},
  };
  for (size_t k = 0; k < sizeof(rows) / sizeof(rows[0]); k++) {
    const struct clamp_row *r = &rows[k];
    setup_module_for_physics_test(3.0);
    const float stellar = f32_from_bits(r->stellar_mass), bulge = f32_from_bits(r->bulge_mass);
    struct Halo test_halo = create_test_halo(0.0f, 0.0f, stellar, bulge, 0.01f);
    struct GalaxyData *gal = test_halo.galaxy;
    gal->MetalsStellarMass = f32_from_bits(r->metals_stellar_mass);
    gal->MetalsBulgeMass = f32_from_bits(r->metals_bulge_mass);

    /* the row's premise: adding the disk (a float difference) into the float bulge overshoots */
    const float unclamped_bulge = (float)((double)bulge + (double)(stellar - bulge));
    ROW_ASSERT((unclamped_bulge > stellar) == (r->clamps_stars != 0), r,
               "the unclamped float bulge overshoots the stellar mass exactly where the row says");
    /* and the same for the metals: the disk metals (a float difference) at the disk's metallicity
     */
    const float metals_stellar = gal->MetalsStellarMass, metals_bulge = gal->MetalsBulgeMass;
    const double disk_metals = metals_stellar - metals_bulge;
    const double moved_metals =
        (disk_metals / (double)(stellar - bulge)) * (double)(stellar - bulge);
    const float unclamped_metals =
        (float)((double)metals_bulge + (moved_metals > disk_metals ? disk_metals : moved_metals));
    ROW_ASSERT((unclamped_metals > metals_stellar) == (r->clamps_metals != 0), r,
               "the unclamped float bulge metals overshoot exactly where the row says");

    struct ModuleContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.params = &MimicConfig;
    ctx.central_galaxy = &test_halo;

    ROW_ASSERT(sage_disk_instability_process(&ctx, &test_halo, 1) == 0, r, "process succeeds");
    ROW_ASSERT(f32_bits(gal->BulgeMass) == r->stellar_mass, r,
               "the whole disk moves and the bulge equals StellarMass bit for bit");
    ROW_ASSERT(f32_bits(gal->MetalsBulgeMass) == r->metals_stellar_mass, r,
               "the bulge metals equal MetalsStellarMass bit for bit");
    ROW_ASSERT(gal->UnstableDiskGasFraction == 0.0, r, "No cold gas: no unstable gas");

    free_test_halo(&test_halo);
    module_system_cleanup();
  }
  check_memory_leaks();
  return TEST_PASS;
}

/**
 * @brief   Main test runner
 *
 * Executes all sage_disk_instability comprehensive tests and reports results.
 */
int main(void) {
  printf("%s", BLUE);
  printf("============================================================\n");
  printf("Test Suite: sage_disk_instability Module (Comprehensive)\n");
  printf("============================================================\n");
  printf("%s", NC);

  /* Initialize error handling for tests */
  initialize_error_handling(LOG_LEVEL_WARNING, NULL);

  TEST_RUN(test_module_initialization);
  TEST_RUN(test_process_by_galaxy_mode);
  TEST_RUN(test_stable_disk_no_transfer);
  TEST_RUN(test_unstable_disk_transfer);
  TEST_RUN(test_full_disk_transfer_clamps_to_available_mass);
  TEST_RUN(test_transfer_clamps_to_stellar_totals);

  /* Print summary and return result */
  TEST_SUMMARY();
  return TEST_RESULT();
}
