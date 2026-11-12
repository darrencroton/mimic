/**
 * @file    test_unit_sage_reionization.c
 * @brief   Unit tests for sage_reionization module
 *
 * Validates: GlobalBaryonFraction use, the exact reionization modifier in every
 * redshift branch, per-halo mass use, and the Mvir = 0 and Type 3 edge cases
 *
 * The modifier is the Gnedin (2000) suppression 1 / (1 + 0.26 M_c / Mvir)^3 with the filtering
 * mass of Kravtsov et al. (2004) Appendix B. Expected values are hand-computed literals from an
 * independent evaluation of those published formulas (HaloBaryonFraction for
 * GlobalBaryonFraction 0.17, Omega 0.25, OmegaLambda 0.75, Hubble 100, G 43007.1), so a wrong
 * coefficient, power or branch changes a row.
 *
 * Test cases:
 *   - test_global_baryon_fraction_scales_output: GlobalBaryonFraction 1.0 and 0.20 are accepted and
 *     scale HaloBaryonFraction
 *   - test_reionization_modifier_table: exact HaloBaryonFraction across mass, the three
 *     f_of_a redshift branches (z <= 7, 7 < z < 8, z >= 8) and the characteristic-mass branch
 *   - test_physics_calculation_type1_satellite: each Type 0, 1 and 2 halo uses its own Mvir, and
 *     the value is recomputed on every call
 *   - test_zero_mass_halos: Mvir = 0 gives exactly 0
 *   - test_type3_halos_skipped: Type 3 halos are left untouched
 *
 */

#include "../../tests/framework/test_framework.h"
#include "core/module_registry.h"
#include "core/module_interface.h"
#include "include/types.h"
#include "include/proto.h"
#include "include/globals.h"
#include "util/error.h"
#include "util/memory.h"

#include <stdio.h>
#include <string.h>

/* Shared SAGE16 test fixture boilerplate (counters, config reset, module registration) */
#include "modules/_tests/sage_test_fixtures.h"

extern int sage_reionization_process(struct ModuleContext *, struct Halo *, int);

/** @brief Fixture cosmology and the GlobalBaryonFraction every literal below assumes */
#define FIXTURE_GLOBAL_BARYON_FRACTION 0.17
#define FIXTURE_CODE_G 43007.1

/** @brief Relative tolerance on HaloBaryonFraction: the float store is good to 6e-8 */
#define MODIFIER_RTOL 1e-6

/** @brief HaloBaryonFraction for GlobalBaryonFraction 1 at z = 0, Mvir = 100: 0.974958096077 */
#define MODIFIER_Z0_MVIR100 0.974958096077

/**
 * @brief Configure the module for GlobalBaryonFraction `global_baryon_frac` without initialising
 *
 * Sets the fixture cosmology and a one-module pre_timestep pipeline, as a run file would.
 */
static void configure_module(double global_baryon_frac) {
  reset_config();
  init_memory_system(0);
  ensure_modules_registered();

  MimicConfig.Omega = 0.25;
  MimicConfig.OmegaLambda = 0.75;
  MimicConfig.Hubble_h = 0.73;
  MimicConfig.Hubble = 100.0; /* H0 in code units (km/s per Mpc/h) */
  MimicConfig.G = FIXTURE_CODE_G;

  MimicConfig.pre_timestep = mymalloc_cat(sizeof(struct PhaseModuleConfig), MEM_UTILITY);
  MimicConfig.pre_timestep[0].module_name = strdup("sage_reionization");
  MimicConfig.pre_timestep[0].processing_mode = PROCESSING_MODE_FULL_HALO;
  MimicConfig.num_pre_timestep = 1;
  MimicConfig.SubSteps = 1;

  set_test_model_parameters();
  for (int i = 0; i < MimicConfig.NumModelParams; i++) {
    if (strcmp(MimicConfig.ModelParams[i].param_name, "GlobalBaryonFraction") == 0) {
      snprintf(MimicConfig.ModelParams[i].value, sizeof(MimicConfig.ModelParams[i].value), "%.6f",
               global_baryon_frac);
    }
  }
}

/* Test fixture: Initialize module for physics tests */
static void setup_module_for_physics_test(double global_baryon_frac) {
  configure_module(global_baryon_frac);
  int result = module_system_init();
  if (result != 0) {
    ERROR_LOG("Module system initialization failed in test fixture");
  }
}

/* Test fixture: Create test halo with specified properties */
static struct Halo create_test_halo(int type, float mvir) {
  struct Halo halo;
  memset(&halo, 0, sizeof(halo));

  halo.Type = type;
  halo.Mvir = mvir;
  halo.Vvir = 100.0; /* km/s */
  halo.Rvir = 0.1;   /* Mpc/h */

  /* Allocate galaxy */
  halo.galaxy = mymalloc_cat(sizeof(struct GalaxyData), MEM_UTILITY);
  memset(halo.galaxy, 0, sizeof(struct GalaxyData));
  halo.galaxy->HaloBaryonFraction = -1.0f; /* Sentinel for uninitialized */

  return halo;
}

/**
 * @test    test_global_baryon_fraction_scales_output
 * @brief   The accepted GlobalBaryonFraction is the one used
 *
 * Expected: 1.0 (the inclusive upper bound) and 0.20 initialise, and the halo then receives
 *           GlobalBaryonFraction times the z = 0, Mvir = 100 modifier. The refused values are
 *           covered by test_unit_sage_parameter_refusals.c
 * Validates: The loaded value reaches HaloBaryonFraction
 */
int test_global_baryon_fraction_scales_output(void) {
  static const double values[] = {1.0, 0.20};

  for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
    /* ===== SETUP ===== */
    configure_module(values[i]);
    char message[256];

    /* ===== EXECUTE ===== */
    int result = module_system_init();

    /* ===== VALIDATE ===== */
    snprintf(message, sizeof(message), "GlobalBaryonFraction=%.2f must initialise", values[i]);
    TEST_ASSERT(result == 0, message);

    struct Halo halo = create_test_halo(0, 100.0f);
    struct ModuleContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.params = &MimicConfig;
    TEST_ASSERT(sage_reionization_process(&ctx, &halo, 1) == 0, "Process should succeed");
    const double expected = values[i] * MODIFIER_Z0_MVIR100;
    snprintf(message, sizeof(message), "GlobalBaryonFraction=%.2f must scale HaloBaryonFraction",
             values[i]);
    TEST_ASSERT_DOUBLE_EQUAL((double)halo.galaxy->HaloBaryonFraction, expected,
                             MODIFIER_RTOL * expected, message);

    /* ===== CLEANUP ===== */
    free_test_halo(&halo);
    module_system_cleanup();
    check_memory_leaks();
  }

  return TEST_PASS;
}

/**
 * @test    test_reionization_modifier_table
 * @brief   HaloBaryonFraction equals GlobalBaryonFraction times the exact modifier
 *
 * Expected: each row's literal, to the float store's precision. The rows cover the mass
 *           dependence (Mvir 0.1 to 1e5), the redshift branch of f_of_a (a >= a_r for z = 0 and
 *           2, the middle branch a_0 < a < a_r at z = 7.5, a <= a_0 at z = 8 and 9) and a code G
 *           small enough that the characteristic mass exceeds the filtering mass.
 * Validates: Every coefficient and power of the Gnedin / Kravtsov et al. formulas
 */
int test_reionization_modifier_table(void) {
  static const struct {
    const char *label;
    double redshift;
    float mvir;    /* 1e10 Msun/h */
    double code_g; /* G in code units; 0 keeps the fixture value */
    double expected;
  } rows[] = {
      {"z=0 Mvir=100", 0.0, 100.0f, 0.0, 0.165742876333},
      {"z=0 Mvir=1e5", 0.0, 1e5f, 0.0, 0.169995670465},
      {"z=0 Mvir=1", 0.0, 1.0f, 0.0, 0.0268954287933},
      {"z=2 Mvir=0.1", 2.0, 0.1f, 0.0, 0.00172958909714},
      {"z=2 Mvir=1", 2.0, 1.0f, 0.0, 0.0673595341801},
      {"z=2 Mvir=10", 2.0, 10.0f, 0.0, 0.152820435688},
      {"z=2 Mvir=100", 2.0, 100.0f, 0.0, 0.168169615948},
      {"z=7.5 Mvir=1 (a_0 < a < a_r)", 7.5, 1.0f, 0.0, 0.167167050417},
      {"z=8 Mvir=1 (a = a_0)", 8.0, 1.0f, 0.0, 0.168234390935},
      {"z=9 Mvir=1 (a < a_0)", 9.0, 1.0f, 0.0, 0.169413239258},
      {"z=2 Mvir=100 G=1e-3 (characteristic mass)", 2.0, 100.0f, 1e-3, 0.000880109185195},
  };

  /* ===== SETUP ===== */
  setup_module_for_physics_test(FIXTURE_GLOBAL_BARYON_FRACTION);

  for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
    struct Halo halo = create_test_halo(0, rows[i].mvir);
    struct ModuleContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.redshift = rows[i].redshift;
    ctx.params = &MimicConfig;
    MimicConfig.G = rows[i].code_g > 0.0 ? rows[i].code_g : FIXTURE_CODE_G;

    /* ===== EXECUTE ===== */
    int result = sage_reionization_process(&ctx, &halo, 1);

    /* ===== VALIDATE ===== */
    char message[256];
    snprintf(message, sizeof(message), "Process should succeed (%s)", rows[i].label);
    TEST_ASSERT(result == 0, message);
    snprintf(message, sizeof(message), "HaloBaryonFraction must match the exact value (%s)",
             rows[i].label);
    TEST_ASSERT_DOUBLE_EQUAL((double)halo.galaxy->HaloBaryonFraction, rows[i].expected,
                             MODIFIER_RTOL * rows[i].expected, message);

    free_test_halo(&halo);
  }

  /* ===== CLEANUP ===== */
  module_system_cleanup();
  check_memory_leaks();

  return TEST_PASS;
}

/**
 * @test    test_physics_calculation_type1_satellite
 * @brief   Centrals, satellites and orphans each use their own Mvir, recomputed on every call
 *
 * Expected: One process call over a Type 0 (Mvir 100), a Type 1 (Mvir 0.5) and a Type 2
 *           (Mvir 10) halo at z = 2 gives each the modifier of its own mass, with the central
 *           named as ctx->central_galaxy; a second call over stale values restores them
 * Validates: SAGE parity: strip_from_satellite() calls do_reionization(gal, ...) on the
 *            satellite itself, reading the satellite's own Mvir, so a satellite receives its own
 *            suppression modifier, not the central's
 */
int test_physics_calculation_type1_satellite(void) {
  /* ===== SETUP ===== */
  setup_module_for_physics_test(FIXTURE_GLOBAL_BARYON_FRACTION);

  struct Halo halos[3] = {create_test_halo(0, 100.0f), create_test_halo(1, 0.5f),
                          create_test_halo(2, 10.0f)};
  const double expected[3] = {0.168169615948, 0.0332351752924, 0.152820435688};

  struct ModuleContext ctx;
  memset(&ctx, 0, sizeof(ctx));
  ctx.redshift = 2.0;
  ctx.params = &MimicConfig;
  ctx.central_galaxy = &halos[0];

  /* ===== EXECUTE AND VALIDATE ===== */
  for (int pass = 0; pass < 2; pass++) {
    TEST_ASSERT(sage_reionization_process(&ctx, halos, 3) == 0, "Process should succeed");
    for (int i = 0; i < 3; i++) {
      TEST_ASSERT_DOUBLE_EQUAL((double)halos[i].galaxy->HaloBaryonFraction, expected[i],
                               MODIFIER_RTOL * expected[i],
                               "Each halo should receive the modifier of its own Mvir");
      halos[i].galaxy->HaloBaryonFraction = 0.10f; /* a stale value the next call must replace */
    }
  }

  /* ===== CLEANUP ===== */
  for (int i = 0; i < 3; i++) {
    free_test_halo(&halos[i]);
  }
  module_system_cleanup();
  check_memory_leaks();

  return TEST_PASS;
}

/**
 * @test    test_zero_mass_halos
 * @brief   Test that zero-mass halos are handled correctly
 *
 * Expected: HaloBaryonFraction = 0 for Mvir = 0
 * Validates: Edge case handling
 */
int test_zero_mass_halos(void) {
  /* ===== SETUP ===== */
  setup_module_for_physics_test(FIXTURE_GLOBAL_BARYON_FRACTION);

  /* Create halo with zero mass */
  struct Halo test_halo = create_test_halo(0, 0.0);

  struct ModuleContext ctx;
  memset(&ctx, 0, sizeof(ctx));
  ctx.redshift = 0.0;
  ctx.params = &MimicConfig;
  ctx.central_galaxy = &test_halo;

  /* ===== EXECUTE ===== */
  int result = sage_reionization_process(&ctx, &test_halo, 1);

  /* ===== VALIDATE ===== */
  TEST_ASSERT(result == 0, "Process should succeed with zero mass");
  TEST_ASSERT(test_halo.galaxy->HaloBaryonFraction == 0.0f,
              "Zero-mass halo should have HaloBaryonFraction = 0");

  /* ===== CLEANUP ===== */
  free_test_halo(&test_halo);
  module_system_cleanup();
  check_memory_leaks();

  return TEST_PASS;
}

/**
 * @test    test_type3_halos_skipped
 * @brief   Test that Type 3 halos are skipped
 *
 * Expected: Type 3 halos are not processed
 * Validates: Type 3 handling
 */
int test_type3_halos_skipped(void) {
  /* ===== SETUP ===== */
  setup_module_for_physics_test(FIXTURE_GLOBAL_BARYON_FRACTION);

  /* Create Type 3 halo */
  struct Halo test_halo = create_test_halo(3, 50.0);

  struct ModuleContext ctx;
  memset(&ctx, 0, sizeof(ctx));
  ctx.redshift = 0.0;
  ctx.params = &MimicConfig;
  ctx.central_galaxy = &test_halo;

  /* ===== EXECUTE ===== */
  int result = sage_reionization_process(&ctx, &test_halo, 1);

  /* ===== VALIDATE ===== */
  TEST_ASSERT(result == 0, "Process should succeed");
  /* Type 3 halos should be skipped, HaloBaryonFraction remains at sentinel value */
  TEST_ASSERT(test_halo.galaxy->HaloBaryonFraction == -1.0f,
              "Type 3 halo should be skipped (HaloBaryonFraction unchanged)");

  /* ===== CLEANUP ===== */
  free_test_halo(&test_halo);
  module_system_cleanup();
  check_memory_leaks();

  return TEST_PASS;
}

/**
 * @brief   Main test runner
 *
 * Executes all sage_reionization unit tests and reports results.
 */
int main(void) {
  printf("%s", BLUE);
  printf("============================================================\n");
  printf("Test Suite: sage_reionization Module\n");
  printf("============================================================\n");
  printf("%s", NC);

  /* Initialize error handling for tests */
  initialize_error_handling(LOG_LEVEL_WARNING, NULL);

  TEST_RUN(test_global_baryon_fraction_scales_output);
  TEST_RUN(test_reionization_modifier_table);
  TEST_RUN(test_physics_calculation_type1_satellite);
  TEST_RUN(test_zero_mass_halos);
  TEST_RUN(test_type3_halos_skipped);

  /* Print summary and return result */
  TEST_SUMMARY();
  return TEST_RESULT();
}
