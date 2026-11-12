/**
 * @file    test_unit_sage_prepare_infall_budget.c
 * @brief   Unit tests for the sage_prepare_infall_budget module
 *
 * Validates SAGE's infall_recipe: the budget HaloBaryonFraction * Mvir minus every baryon of the
 * FoF group, the satellites' surrender of ejected gas and ICS to the central, the central's
 * consolidation clamps, and the fallback of an unset HaloBaryonFraction to GlobalBaryonFraction.
 *
 * Test cases:
 *   - test_module_initialization: a pipeline with the module in pre_timestep initialises
 *   - test_halo_baryon_fraction_sentinel_falls_back_to_global: an unset (-1) HaloBaryonFraction
 *     becomes GlobalBaryonFraction, a set one (reionization's) is kept
 *   - test_physics_calculation_basic: the budget sums all six baryon terms of a central, exactly
 *   - test_orphan_hot_gas_retained: a Type 2 orphan keeps its hot gas (SAGE parity)
 *   - test_multiple_satellites: ejected gas, ICS and their metals pool into the central,
 *     satellites are zeroed, the budget counts the pool
 *   - test_central_consolidation_clamps: the ejected and ICS clamps of the central
 *   - test_null_central_galaxy_is_an_error: a Type 0 row without galaxy data is an error
 *
 * Process cases initialise the module directly with the parameters they need and build their
 * groups with create_test_halo(); the group's central is always row 0.
 */

#include "../../../../tests/framework/test_framework.h"
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

extern int sage_prepare_infall_budget_init(void);
extern int sage_prepare_infall_budget_process(struct ModuleContext *, struct Halo *, int);

/**
 * @brief Initialise the module directly with GlobalBaryonFraction `gbf`
 * @return init()'s status, 0 on success
 */
static int init_with(const char *gbf) {
  reset_config();
  strncpy(MimicConfig.ModelParams[0].param_name, "GlobalBaryonFraction", MAX_STRING_LEN - 1);
  strncpy(MimicConfig.ModelParams[0].value, gbf, MAX_STRING_LEN - 1);
  MimicConfig.NumModelParams = 1;
  return sage_prepare_infall_budget_init();
}

/** @brief Initialise for a physics case: memory system up, GlobalBaryonFraction `gbf` */
static int setup_module_for_physics_test(const char *gbf) {
  init_memory_system(0);
  return init_with(gbf);
}

/** @brief Run the module over `ngal` halos whose central is row 0 */
static int run_process(struct Halo *halos, const int ngal) {
  struct ModuleContext ctx;
  memset(&ctx, 0, sizeof(ctx));
  ctx.params = &MimicConfig;
  ctx.num_substeps = 1;
  ctx.central_galaxy = &halos[0];
  return sage_prepare_infall_budget_process(&ctx, halos, ngal);
}

/* Test fixture: Create test halo with specified properties */
static struct Halo create_test_halo(int type, float mvir) {
  struct Halo halo;
  memset(&halo, 0, sizeof(halo));

  halo.Type = type;
  halo.Mvir = mvir;
  halo.Vvir = 100.0;
  halo.Rvir = 0.1;

  /* Allocate galaxy */
  halo.galaxy = mymalloc_cat(sizeof(struct GalaxyData), MEM_UTILITY);
  memset(halo.galaxy, 0, sizeof(struct GalaxyData));
  halo.galaxy->HaloBaryonFraction = -1.0f; /* Sentinel for uninitialized */

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

  /* Configure module */
  MimicConfig.pre_timestep = mymalloc_cat(sizeof(struct PhaseModuleConfig), MEM_UTILITY);
  MimicConfig.pre_timestep[0].module_name = strdup("sage_prepare_infall_budget");
  MimicConfig.pre_timestep[0].processing_mode = PROCESSING_MODE_FULL_HALO;
  MimicConfig.num_pre_timestep = 1;
  MimicConfig.SubSteps = 1;
  set_test_model_parameters();

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
 * @test    test_halo_baryon_fraction_sentinel_falls_back_to_global
 * @brief   An unset HaloBaryonFraction (the -1 sentinel, no sage_reionization in the pipeline)
 *          becomes GlobalBaryonFraction, while a value already set is kept
 *
 * GlobalBaryonFraction 0.25, a central of Mvir 64 holding 1.0 of stellar mass: the budget is
 * fraction * 64 - 1.0. The zero row pins the sentinel test as exactly -1: a set fraction of 0 is
 * kept, not replaced.
 */
int test_halo_baryon_fraction_sentinel_falls_back_to_global(void) {
  static const struct {
    double preset;
    double fraction;
    double budget;
  } rows[] = {
      {-1.0, 0.25, 15.0},
      {0.5, 0.5, 31.0},
      {0.0, 0.0, -1.0},
  };

  for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
    char msg[96];
    /* ===== SETUP ===== */
    TEST_ASSERT(setup_module_for_physics_test("0.25") == 0, "init must succeed");
    struct Halo central = create_test_halo(0, 64.0f);
    central.galaxy->HaloBaryonFraction = rows[i].preset;
    central.galaxy->StellarMass = 1.0f;

    /* ===== EXECUTE ===== */
    TEST_ASSERT(run_process(&central, 1) == 0, "process must succeed");

    /* ===== VALIDATE ===== */
    snprintf(msg, sizeof(msg), "HaloBaryonFraction after a preset of %g", rows[i].preset);
    TEST_ASSERT(central.galaxy->HaloBaryonFraction == rows[i].fraction, msg);
    snprintf(msg, sizeof(msg), "budget after a preset of %g", rows[i].preset);
    TEST_ASSERT(central.galaxy->InfallingGas == rows[i].budget, msg);

    /* ===== CLEANUP ===== */
    free_test_halo(&central);
    check_memory_leaks();
  }

  return TEST_PASS;
}

/**
 * @test    test_physics_calculation_basic
 * @brief   The budget of a lone central sums all six baryon terms
 *
 * HaloBaryonFraction 0.25 at Mvir 64 allows 16; the central holds stellar 8, cold 4, hot 2.5,
 * ejected 1.25, ICS 0.5 and black hole 0.0625, 16.3125 in all. Every input is a binary fraction,
 * so the budget is the exact literal -0.3125: a dropped or doubled term, or a float narrowing of
 * the sum, changes it.
 */
int test_physics_calculation_basic(void) {
  /* ===== SETUP ===== */
  TEST_ASSERT(setup_module_for_physics_test("0.17") == 0, "init must succeed");

  struct Halo central = create_test_halo(0, 64.0f);
  central.galaxy->HaloBaryonFraction = 0.25;
  central.galaxy->StellarMass = 8.0f;
  central.galaxy->ColdGas = 4.0f;
  central.galaxy->HotGas = 2.5f;
  central.galaxy->EjectedGas = 1.25f;
  central.galaxy->ICS = 0.5f;
  central.galaxy->BlackHoleMass = 0.0625f;

  /* ===== EXECUTE ===== */
  TEST_ASSERT(run_process(&central, 1) == 0, "Process should succeed");

  /* ===== VALIDATE ===== */
  TEST_ASSERT(central.galaxy->InfallingGas == -0.3125, "InfallingGas must be 16 - 16.3125");

  /* ===== CLEANUP ===== */
  free_test_halo(&central);
  check_memory_leaks();

  return TEST_PASS;
}

/**
 * @test    test_orphan_hot_gas_retained
 * @brief   SAGE parity: Type 2 orphan hot gas is NOT consolidated to the
 *          central. SAGE infall_recipe consolidates only satellites' ejected gas and ICS,
 *          never their hot gas. Orphans keep their hot gas and cool it themselves until merging.
 *
 * Expected: Central keeps only its own hot gas; orphan retains its hot gas.
 */
int test_orphan_hot_gas_retained(void) {
  /* ===== SETUP ===== */
  TEST_ASSERT(setup_module_for_physics_test("0.17") == 0, "init must succeed");

  /* Create central and Type 2 orphan */
  struct Halo halos[2];
  halos[0] = create_test_halo(0, 100.0); /* Central */
  halos[1] = create_test_halo(2, 0.0);   /* Type 2 orphan */

  halos[0].galaxy->HaloBaryonFraction = 0.17f;
  halos[0].galaxy->HotGas = 5.0f;
  halos[0].galaxy->MetalsHotGas = 0.1f;

  halos[1].galaxy->HaloBaryonFraction = 0.17f;
  halos[1].galaxy->HotGas = 3.0f;
  halos[1].galaxy->MetalsHotGas = 0.06f;

  /* ===== EXECUTE ===== */
  TEST_ASSERT(run_process(halos, 2) == 0, "Process should succeed");

  /* ===== VALIDATE ===== */
  /* SAGE parity: hot gas is NOT consolidated. The central keeps only its own
   * hot gas, and the Type 2 orphan retains its hot gas (it cools it itself
   * until it merges). */
  TEST_ASSERT_DOUBLE_EQUAL(halos[0].galaxy->HotGas, 5.0, 0.01,
                           "Central keeps only its own hot gas");
  TEST_ASSERT_DOUBLE_EQUAL(halos[0].galaxy->MetalsHotGas, 0.1, 0.01,
                           "Central keeps only its own hot gas metals");

  TEST_ASSERT_DOUBLE_EQUAL(halos[1].galaxy->HotGas, 3.0, 0.01, "Type 2 orphan retains its hot gas");
  TEST_ASSERT_DOUBLE_EQUAL(halos[1].galaxy->MetalsHotGas, 0.06, 0.01,
                           "Type 2 orphan retains its hot gas metals");

  /* ===== CLEANUP ===== */
  free_test_halo(&halos[0]);
  free_test_halo(&halos[1]);
  check_memory_leaks();

  return TEST_PASS;
}

/**
 * @test    test_multiple_satellites
 * @brief   Satellites surrender their ejected gas and ICS, with the metals, to the central
 *
 * A central (ejected 1.0 / 0.125 metals, ICS 0.5 / 0.0625) and three satellites i = 1..3 (ejected
 * i / 0.125 i, ICS 0.5 i / 0.0625 i) pool into the central: ejected 7.0 / 0.875, ICS 3.5 / 0.4375,
 * and each satellite is left with none. The budget 0.25 * 128 - (7.0 + 3.5) is 21.5. Every value is
 * a binary fraction, so all are exact literals.
 */
int test_multiple_satellites(void) {
  /* ===== SETUP ===== */
  TEST_ASSERT(setup_module_for_physics_test("0.17") == 0, "init must succeed");

  struct Halo halos[4];
  halos[0] = create_test_halo(0, 128.0f); /* Central */
  for (int i = 1; i < 4; i++) {
    halos[i] = create_test_halo(1, 10.0f);
  }

  struct GalaxyData *c = halos[0].galaxy;
  c->HaloBaryonFraction = 0.25;
  c->EjectedGas = 1.0f;
  c->MetalsEjectedGas = 0.125f;
  c->ICS = 0.5f;
  c->MetalsICS = 0.0625f;

  for (int i = 1; i < 4; i++) {
    struct GalaxyData *s = halos[i].galaxy;
    s->EjectedGas = (float)i; /* 1.0, 2.0, 3.0 */
    s->MetalsEjectedGas = 0.125f * (float)i;
    s->ICS = 0.5f * (float)i; /* 0.5, 1.0, 1.5 */
    s->MetalsICS = 0.0625f * (float)i;
  }

  /* ===== EXECUTE ===== */
  TEST_ASSERT(run_process(halos, 4) == 0, "Process should succeed");

  /* ===== VALIDATE ===== */
  TEST_ASSERT(c->EjectedGas == 7.0f && c->MetalsEjectedGas == 0.875f,
              "the central must hold all ejected gas and its metals");
  TEST_ASSERT(c->ICS == 3.5f && c->MetalsICS == 0.4375f,
              "the central must hold all ICS and its metals");
  TEST_ASSERT(c->InfallingGas == 21.5, "the budget counts the pooled ejected gas and ICS");

  for (int i = 1; i < 4; i++) {
    const struct GalaxyData *s = halos[i].galaxy;
    TEST_ASSERT(s->EjectedGas == 0.0f && s->MetalsEjectedGas == 0.0f,
                "a satellite must be left without ejected gas");
    TEST_ASSERT(s->ICS == 0.0f && s->MetalsICS == 0.0f, "a satellite must be left without ICS");
  }

  /* ===== CLEANUP ===== */
  for (int i = 0; i < 4; i++) {
    free_test_halo(&halos[i]);
  }
  check_memory_leaks();

  return TEST_PASS;
}

/**
 * @test    test_central_consolidation_clamps
 * @brief   The central's ejected and ICS reservoirs are clamped: a negative mass is cleared with
 *          its metals, otherwise negative metals are floored and metals above the mass capped
 *
 * Each row loads the same (mass, metals) pair into both reservoirs of a lone central and expects
 * the clamped pair in each.
 */
int test_central_consolidation_clamps(void) {
  static const struct {
    float mass, metals;
    float mass_out, metals_out;
  } rows[] = {
      {2.0f, 0.25f, 2.0f, 0.25f}, /* within bounds: unchanged */
      {1.0f, 2.0f, 1.0f, 1.0f},   /* metals above mass: capped */
      {-0.5f, 0.25f, 0.0f, 0.0f}, /* negative mass: cleared with its metals */
      {1.0f, -0.25f, 1.0f, 0.0f}, /* negative metals: floored */
  };

  for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
    char msg[96];
    TEST_ASSERT(setup_module_for_physics_test("0.17") == 0, "init must succeed");
    struct Halo central = create_test_halo(0, 64.0f);
    struct GalaxyData *gal = central.galaxy;
    gal->HaloBaryonFraction = 0.25;
    gal->EjectedGas = gal->ICS = rows[i].mass;
    gal->MetalsEjectedGas = gal->MetalsICS = rows[i].metals;

    TEST_ASSERT(run_process(&central, 1) == 0, "process must succeed");

    snprintf(msg, sizeof(msg), "ejected pair of row %zu (%g, %g)", i, (double)rows[i].mass,
             (double)rows[i].metals);
    TEST_ASSERT(gal->EjectedGas == rows[i].mass_out && gal->MetalsEjectedGas == rows[i].metals_out,
                msg);
    snprintf(msg, sizeof(msg), "ICS pair of row %zu (%g, %g)", i, (double)rows[i].mass,
             (double)rows[i].metals);
    TEST_ASSERT(gal->ICS == rows[i].mass_out && gal->MetalsICS == rows[i].metals_out, msg);

    free_test_halo(&central);
    check_memory_leaks();
  }

  return TEST_PASS;
}

/**
 * @test    test_null_central_galaxy_is_an_error
 * @brief   A Type 0 row with no galaxy data fails with -1 instead of being skipped
 */
int test_null_central_galaxy_is_an_error(void) {
  TEST_ASSERT(setup_module_for_physics_test("0.17") == 0, "init must succeed");

  struct Halo central;
  memset(&central, 0, sizeof(central));
  central.Type = 0;
  central.Mvir = 100.0;
  central.galaxy = NULL;

  TEST_ASSERT(run_process(&central, 1) == -1, "a central without galaxy data must be an error");

  check_memory_leaks();
  return TEST_PASS;
}

/**
 * @brief   Main test runner
 *
 * Executes all sage_prepare_infall_budget unit tests and reports results.
 */
int main(void) {
  printf("%s", BLUE);
  printf("============================================================\n");
  printf("Test Suite: sage_prepare_infall_budget Module\n");
  printf("============================================================\n");
  printf("%s", NC);

  /* Initialize error handling for tests */
  initialize_error_handling(LOG_LEVEL_WARNING, NULL);

  /* Lifecycle and parameters */
  printf("\n%s=== Initialisation Tests ===%s\n", BLUE, NC);
  TEST_RUN(test_module_initialization);
  TEST_RUN(test_halo_baryon_fraction_sentinel_falls_back_to_global);

  /* Run physics tests */
  printf("\n%s=== Physics Calculation Tests ===%s\n", BLUE, NC);
  TEST_RUN(test_physics_calculation_basic);
  TEST_RUN(test_orphan_hot_gas_retained);
  TEST_RUN(test_multiple_satellites);
  TEST_RUN(test_central_consolidation_clamps);
  TEST_RUN(test_null_central_galaxy_is_an_error);

  /* Print summary and return result */
  TEST_SUMMARY();
  return TEST_RESULT();
}
