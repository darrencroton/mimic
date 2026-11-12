/**
 * @file    test_unit_sage26_disk_instability.c
 * @brief   Comprehensive unit tests for sage26_disk_instability module
 *
 * Validates: module initialisation, the disk stability criterion and the transfer it drives,
 * the guards, and SAGE26's absence of transfer caps and bulge clamps
 *
 * - Disk stability criterion (Mo, Mao & White 1998): Mcrit = Vmax^2 (factor * radius) / G capped
 *   at the disk mass
 * - Stellar mass transfer to the bulge with metallicity preservation, and the unstable gas
 *   fraction handed to the downstream modules
 * - Edge cases (zero or negative disk mass, zero Vmax, zero radius) and SAGE26's absence of
 *   transfer caps and bulge clamps (float bit patterns of SAGE26's own results)
 *
 * Test cases:
 *   - test_module_initialization: module_system_init accepts the module's placement
 *   - test_process_by_galaxy_mode: ngal != 1 and a missing run configuration are refused
 *   - test_stable_disk_no_transfer: a table of disks that are stable or empty, each left
 *     bytewise unchanged
 *   - test_unstable_disk_transfer: a table of unstable disks, each asserting the stars moved,
 *     their metals, the instability-bulge mass and the unstable gas fraction
 *   - test_sage26_transfer_bits: SAGE26's bit patterns for a full-disk transfer: no cap, no clamp
 *   - test_ffb_row_bypassed: a row whose FFBRegime is 1 leaves itself and its FoF central
 *     bytewise unchanged on an unstable disk (no transfer, UnstableDiskGasFraction keeps 0); the
 *     same row with FFBRegime 0 gets SAGE26's check_disk_instability trigger, written inline
 *
 * The StarFormingDiskFactor refusals are asserted, with diagnostics, by
 * test_unit_sage26_parameter_refusals.c.
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
#include <float.h>

/* Shared sage26 test fixture boilerplate (counters, config reset, module registration) */
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
  /* The required bulge-size mode: BulgeSizeOn 0 (no sizes) */
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

  /* A missing run configuration (G, Hubble_h) is refused, and nothing is transferred */
  struct GalaxyData before;
  memcpy(&before, halos[0].galaxy, sizeof(before));
  TEST_ASSERT(sage26_disk_instability_process(NULL, halos, 1) != 0, "a NULL context is refused");
  struct ModuleContext no_params;
  memset(&no_params, 0, sizeof(no_params));
  TEST_ASSERT(sage26_disk_instability_process(&no_params, halos, 1) != 0,
              "a context without params is refused");
  TEST_ASSERT(memcmp(halos[0].galaxy, &before, sizeof(before)) == 0,
              "a refused call leaves the galaxy unchanged");

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

    ROW_ASSERT(sage26_disk_instability_process(&ctx, &test_halo, 1) == 0, r, "process succeeds");
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
 * @brief   The stars an unstable disk moves to the bulge, their metals, the instability-bulge
 *          mass and the unstable gas fraction handed on
 *
 * Expected: with diskmass = ColdGas + (StellarMass - BulgeMass), gas fraction f = ColdGas /
 *           diskmass and Mcrit = Vmax^2 (factor * radius) / G (G = 43007.1) capped at diskmass:
 *           stars moved = (1 - f)(diskmass - Mcrit), UnstableDiskGasFraction = f (diskmass -
 *           Mcrit) / ColdGas when positive, and the bulge metals gain the disk's metallicity
 *           times the stars moved. The literals are those formulas, hand-evaluated.
 * Validates: the transfer, the gas branch, Mcrit 0 (Vmax 0 or radius 0 moves the whole disk), the
 *            dependence on StarFormingDiskFactor, and a disk just on the unstable side of the cap
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

    ROW_ASSERT(sage26_disk_instability_process(&ctx, &test_halo, 1) == 0, r, "process succeeds");
    /* the bulge fields are floats: a tolerance of a few float steps of the result */
    ROW_ASSERT(fabs(gal->BulgeMass - (bulge0 + r->stars)) <=
                   2.0 * FLT_EPSILON * (bulge0 + r->stars),
               r, "BulgeMass gains the stars moved");
    ROW_ASSERT(fabs(gal->InstabilityBulgeMass - r->stars) <= 2.0 * FLT_EPSILON * r->stars, r,
               "InstabilityBulgeMass gains the stars moved");
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

/* ---------------------------------------------------------------------------
 * No cap and no clamp
 *
 * SAGE26 adds the double unstable_stars and its metallicity product straight into
 * the float bulge fields. With Vmax = 0 (Mcrit = 0) and ColdGas = 0 the whole float
 * disk difference is transferred, and the float stores can leave BulgeMass or
 * MetalsBulgeMass one rounding above StellarMass or MetalsStellarMass. SAGE26 keeps
 * that; it has no cap on the transfer and no clamp of the bulge.
 *
 * The expected bit patterns below were produced by executing SAGE26's own
 * check_disk_instability on exactly these inputs, with G = 43007.1, AGNrecipeOn 2 and
 * BulgeSizeOn 0. ColdGas = 0 keeps SAGE26's routine from reaching its black-hole and
 * burst calls, so its end state is the transfer alone. SAGE26 leaves the bulge radii
 * (set to 0.25, 0.125 and 0.0625) unchanged.
 * --------------------------------------------------------------------------- */

static uint32_t f32_bits(const float value) {
  uint32_t bits;
  memcpy(&bits, &value, sizeof(bits));
  return bits;
}

/** @brief Inputs (float bit patterns) and SAGE26's resulting bulge fields */
struct sage26_transfer_case {
  const char *name;
  uint32_t stellar_mass;
  uint32_t bulge_mass;
  uint32_t metals_stellar_mass;
  uint32_t metals_bulge_mass;
  uint32_t expected_bulge_mass;
  uint32_t expected_metals_bulge_mass;
  uint32_t expected_instability_bulge_mass; /* from InstabilityBulgeMass = 0 */
  int bulge_above_stellar;                  /* the case leaves BulgeMass above StellarMass */
  int metals_above_stellar_metals;          /* the case leaves MetalsBulgeMass above its disk's */
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

  ROW_ASSERT(result == 0, c, "Full disk transfer should complete");
  ROW_ASSERT(f32_bits(gal->BulgeMass) == c->expected_bulge_mass, c,
             "BulgeMass must equal SAGE26's uncapped, unclamped result bit for bit");
  ROW_ASSERT(f32_bits(gal->MetalsBulgeMass) == c->expected_metals_bulge_mass, c,
             "MetalsBulgeMass must equal SAGE26's uncapped, unclamped result bit for bit");
  ROW_ASSERT(f32_bits(gal->InstabilityBulgeMass) == c->expected_instability_bulge_mass, c,
             "InstabilityBulgeMass must receive SAGE26's unstable_stars bit for bit");
  ROW_ASSERT(gal->UnstableDiskGasFraction == 0.0, c, "No cold gas: no unstable gas is handed on");
  ROW_ASSERT(gal->BulgeRadius == 0.25f && gal->MergerBulgeRadius == 0.125f &&
                 gal->InstabilityBulgeRadius == 0.0625f,
             c, "BulgeSizeOn 0: the bulge radii keep their values");

  free_test_halo(&test_halo);
  module_system_cleanup();
  check_memory_leaks();
  return TEST_PASS;
}

/**
 * @test    test_sage26_transfer_bits
 * @brief   A full stellar-disk transfer is not capped to the disk's stars or metals, and the bulge
 *          is not clamped to the stellar totals
 *
 * Expected: SAGE26's bits. The first two rows leave BulgeMass one float above StellarMass; the
 *           third leaves MetalsBulgeMass one float above MetalsStellarMass.
 * Validates: no transfer cap and no bulge clamp (SAGE parity)
 */
int test_sage26_transfer_bits(void) {
  static const struct sage26_transfer_case rows[] = {
      {.name = "full_disk_transfer_is_not_capped",
       .stellar_mass = 0x423aed91u,        /* 46.7320 */
       .bulge_mass = 0x412b39deu,          /* 10.7016 */
       .metals_stellar_mass = 0x3ee721fau, /* 0.451431 */
       .metals_bulge_mass = 0x3ec7b2dcu,   /* 0.390036 */
       .expected_bulge_mass = 0x423aed92u, /* one float above StellarMass */
       .expected_metals_bulge_mass = 0x3ee721fau,
       .expected_instability_bulge_mass = 0x42101f1au,
       .bulge_above_stellar = 1},
      {.name = "bulge_mass_not_clamped_to_stellar_mass",
       .stellar_mass = 0x4222b53fu,        /* 40.6770 */
       .bulge_mass = 0x4102d136u,          /* 8.17607 */
       .metals_stellar_mass = 0x3dc644fau, /* 0.0968112 */
       .metals_bulge_mass = 0x3cb27148u,   /* 0.0217903 */
       .expected_bulge_mass = 0x4222b540u, /* one float above StellarMass */
       .expected_metals_bulge_mass = 0x3dc644fau,
       .expected_instability_bulge_mass = 0x420200f2u,
       .bulge_above_stellar = 1},
      {.name = "metals_bulge_not_clamped_to_stellar_metals",
       .stellar_mass = 0x42079893u,               /* 33.8990 */
       .bulge_mass = 0x419b649du,                 /* 19.4241 */
       .metals_stellar_mass = 0x3eb9b657u,        /* 0.362719 */
       .metals_bulge_mass = 0x3db5ff7eu,          /* 0.0888662 */
       .expected_bulge_mass = 0x42079893u,        /* equal to StellarMass */
       .expected_metals_bulge_mass = 0x3eb9b658u, /* one float above MetalsStellarMass */
       .expected_instability_bulge_mass = 0x41679912u,
       .metals_above_stellar_metals = 1},
  };
  for (size_t k = 0; k < sizeof(rows) / sizeof(rows[0]); k++) {
    const struct sage26_transfer_case *c = &rows[k];
    if (c->bulge_above_stellar) {
      ROW_ASSERT(f32_from_bits(c->expected_bulge_mass) > f32_from_bits(c->stellar_mass), c,
                 "This SAGE26 case leaves BulgeMass above StellarMass");
    }
    if (c->metals_above_stellar_metals) {
      ROW_ASSERT(f32_from_bits(c->expected_metals_bulge_mass) >
                     f32_from_bits(c->metals_stellar_mass),
                 c, "This SAGE26 case leaves MetalsBulgeMass above MetalsStellarMass");
    }
    const int status = run_sage26_transfer_case(c);
    if (status != TEST_PASS) {
      return status;
    }
  }
  return TEST_PASS;
}

// ============================================================================
// FEEDBACK-FREE-BURST BYPASS
// ============================================================================

/**
 * @test    test_ffb_row_bypassed
 * @brief   An FFBRegime 1 row is not checked; the same row with FFBRegime 0 is
 *
 * SAGE26's starformation_and_feedback hands an FFBRegime 1 row to starformation_ffb, which
 * returns with no disk-instability check. The row is a satellite on an unstable disk (diskmass 13
 * far above Mcrit) with its own FoF central; with FFBRegime 1 both galaxy structs are compared
 * byte for byte with their inputs, so no stars move and UnstableDiskGasFraction keeps its reset
 * value 0. With FFBRegime 0 the trigger is SAGE26's check_disk_instability, written here with its
 * operand order and float sums.
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
  const double diskmass = row_in.ColdGas + (row_in.StellarMass - row_in.BulgeMass);
  const double Mcrit = vmax * vmax * (3.0 * row_in.DiskScaleRadius) / MimicConfig.G;
  const double gas_fraction = row_in.ColdGas / diskmass;
  const double unstable_gas = gas_fraction * (diskmass - Mcrit);
  const double expected = unstable_gas / row_in.ColdGas;
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

  TEST_RUN(test_module_initialization);
  TEST_RUN(test_process_by_galaxy_mode);
  TEST_RUN(test_stable_disk_no_transfer);
  TEST_RUN(test_unstable_disk_transfer);
  TEST_RUN(test_sage26_transfer_bits);
  TEST_RUN(test_ffb_row_bypassed);

  /* Print summary and return result */
  TEST_SUMMARY();
  return TEST_RESULT();
}
