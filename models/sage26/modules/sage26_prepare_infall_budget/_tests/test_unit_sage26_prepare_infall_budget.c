/**
 * @file    test_unit_sage26_prepare_infall_budget.c
 * @brief   Unit tests for the sage26_prepare_infall_budget module
 *
 * Validates SAGE26's infall_recipe: the budget HaloBaryonFraction * Mvir minus every baryon of the
 * FoF group, the satellites' surrender of ejected gas and ICS (with its assembly history) to the
 * central, the central's consolidation clamps, the fallback of an unset HaloBaryonFraction to
 * GlobalBaryonFraction, and the CGMrecipeOn handling of the CGM reservoirs.
 *
 * Test cases:
 *   - test_module_initialization: a pipeline with the module in pre_timestep initialises
 *   - test_halo_baryon_fraction_sentinel_falls_back_to_global: an unset (-1) HaloBaryonFraction
 *     becomes GlobalBaryonFraction, a set one (reionization's) is kept
 *   - test_physics_calculation_basic: the budget sums all six baryon terms of a central, exactly
 *   - test_orphan_hot_gas_retained: a Type 2 orphan keeps its hot gas (SAGE parity)
 *   - test_multiple_satellites: ejected gas, ICS and their metals pool into the central, the ICS
 *     assembly history is rebooked as accreted, satellites are zeroed, the budget counts the pool
 *   - test_central_consolidation_clamps: the ejected, CGM and ICS clamp chains of the central
 *   - test_null_central_galaxy_is_an_error: a Type 0 row without galaxy data is an error
 *   - test_cgm_central_migration_by_regime: CGMrecipeOn 1 migrates only the central's own gas by
 *     its regime, no satellite CGM pooled; CGMrecipeOn 0 on the same input pools it
 *
 * Process cases initialise the module directly with the parameters they need and build their
 * groups on the stack or with create_test_halo(); the group's central is always row 0.
 *
 * The module's GlobalBaryonFraction and CGMrecipeOn refusals are asserted, with diagnostics, by
 * test_unit_sage26_parameter_refusals.c.
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
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* Shared sage26 test fixture boilerplate (counters, config reset, module registration) */
#include "modules/_tests/sage_test_fixtures.h"

extern int sage26_prepare_infall_budget_init(void);
extern int sage26_prepare_infall_budget_process(struct ModuleContext *, struct Halo *, int);

/**
 * @brief Initialise the module directly with GlobalBaryonFraction `gbf` and CGMrecipeOn `cgm`
 * @return init()'s status, 0 on success
 */
static int init_with(const char *gbf, const char *cgm) {
  reset_config();
  sage26_test_append_model_parameter("GlobalBaryonFraction", gbf);
  sage26_test_append_model_parameter("CGMrecipeOn", cgm);
  return sage26_prepare_infall_budget_init();
}

/** @brief Initialise with GlobalBaryonFraction 0.17 and CGMrecipeOn `cgm` */
static int cgm_init(const char *cgm) { return init_with("0.17", cgm); }

/** @brief Initialise for a physics case: memory system up, GlobalBaryonFraction `gbf`, CGMrecipeOn
 * 0 */
static int setup_module_for_physics_test(const char *gbf) {
  init_memory_system(0);
  return init_with(gbf, "0");
}

/** @brief Run the module over `ngal` halos whose central is row 0 */
static int run_process(struct Halo *halos, const int ngal) {
  struct ModuleContext ctx;
  memset(&ctx, 0, sizeof(ctx));
  ctx.params = &MimicConfig;
  ctx.num_substeps = 1;
  ctx.central_galaxy = &halos[0];
  return sage26_prepare_infall_budget_process(&ctx, halos, ngal);
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
  MimicConfig.pre_timestep[0].module_name = strdup("sage26_prepare_infall_budget");
  MimicConfig.pre_timestep[0].processing_mode = PROCESSING_MODE_FULL_HALO;
  MimicConfig.num_pre_timestep = 1;
  MimicConfig.SubSteps = 1;
  set_test_model_parameters();
  add_test_cgm_recipe_parameter(); /* CGMrecipeOn: 0, required */

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
 * @brief   An unset HaloBaryonFraction (the -1 sentinel, no sage26_reionization in the pipeline)
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
 *          central. SAGE26 infall_recipe consolidates satellites' ejected gas,
 *          ICS and, with CGMrecipeOn 0 (as here), CGM, never their hot gas; with the CGM
 *          recipe off it adds the group's CGM, zero here, to the central's hot gas. Orphans
 *          keep their hot gas and cool it themselves until merging.
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
   * until it merges). SAGE26 infall_recipe consolidates satellites' ejected
   * gas, ICS and, with CGMrecipeOn 0 (as here), CGM and adds the group's CGM to the central's
   * hot gas; both CGM reservoirs are zero here. */
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
 * @brief   Satellites surrender ejected gas and ICS, with the ICS assembly history, to the central
 *
 * A central (ejected 1.0 / 0.125 metals, ICS 0.5 / 0.0625, ICS_accrete 0.25, ICS_sum_mt 2.0) and
 * three satellites i = 1..3 (ejected i / 0.125 i, ICS 0.5 i / 0.0625 i, ICS_sum_mt 4 i, ICS_accrete
 * 1.0, ICS_disrupt 0.5) pool into the central: ejected 7.0 / 0.875, ICS 3.5 / 0.4375. Each
 * satellite's ICS also books as accreted (0.25 + 0.5 + 1.0 + 1.5 = 3.25) with its ICS_sum_mt
 * (2 + 4 + 8 + 12 = 26) and its own ICS_disrupt, ICS_accrete and ICS_sum_mt are zeroed (SAGE
 * parity: the central's ICS_disrupt is untouched). A fourth satellite without ICS keeps its history
 * and contributes none. The budget 0.25 * 128 - (7.0 + 3.5) is 21.5. Every value is a binary
 * fraction, so all are exact literals.
 */
int test_multiple_satellites(void) {
  /* ===== SETUP ===== */
  TEST_ASSERT(setup_module_for_physics_test("0.17") == 0, "init must succeed");

  struct Halo halos[5];
  halos[0] = create_test_halo(0, 128.0f); /* Central */
  for (int i = 1; i < 5; i++) {
    halos[i] = create_test_halo(1, 10.0f);
  }

  struct GalaxyData *c = halos[0].galaxy;
  c->HaloBaryonFraction = 0.25;
  c->EjectedGas = 1.0f;
  c->MetalsEjectedGas = 0.125f;
  c->ICS = 0.5f;
  c->MetalsICS = 0.0625f;
  c->ICS_accrete = 0.25f;
  c->ICS_sum_mt = 2.0f;
  c->ICS_disrupt = 0.125f;

  for (int i = 1; i < 4; i++) {
    struct GalaxyData *s = halos[i].galaxy;
    s->EjectedGas = (float)i; /* 1.0, 2.0, 3.0 */
    s->MetalsEjectedGas = 0.125f * (float)i;
    s->ICS = 0.5f * (float)i; /* 0.5, 1.0, 1.5 */
    s->MetalsICS = 0.0625f * (float)i;
    s->ICS_sum_mt = 4.0f * (float)i;
    s->ICS_accrete = 1.0f;
    s->ICS_disrupt = 0.5f;
  }
  /* A satellite with no ICS: its history is not rebooked */
  halos[4].galaxy->ICS_sum_mt = 16.0f;
  halos[4].galaxy->ICS_accrete = 0.5f;

  /* ===== EXECUTE ===== */
  TEST_ASSERT(run_process(halos, 5) == 0, "Process should succeed");

  /* ===== VALIDATE ===== */
  TEST_ASSERT(c->EjectedGas == 7.0f && c->MetalsEjectedGas == 0.875f,
              "the central must hold all ejected gas and its metals");
  TEST_ASSERT(c->ICS == 3.5f && c->MetalsICS == 0.4375f,
              "the central must hold all ICS and its metals");
  TEST_ASSERT(c->ICS_accrete == 3.25f, "the satellites' ICS must be booked as accreted");
  TEST_ASSERT(c->ICS_sum_mt == 26.0f, "the satellites' ICS_sum_mt must move to the central");
  TEST_ASSERT(c->ICS_disrupt == 0.125f, "the central's ICS_disrupt must be untouched");
  TEST_ASSERT(c->InfallingGas == 21.5, "the budget counts the pooled ejected gas and ICS");

  for (int i = 1; i < 4; i++) {
    const struct GalaxyData *s = halos[i].galaxy;
    TEST_ASSERT(s->EjectedGas == 0.0f && s->MetalsEjectedGas == 0.0f,
                "a satellite must be left without ejected gas");
    TEST_ASSERT(s->ICS == 0.0f && s->MetalsICS == 0.0f, "a satellite must be left without ICS");
    TEST_ASSERT(s->ICS_disrupt == 0.0f && s->ICS_accrete == 0.0f && s->ICS_sum_mt == 0.0f,
                "a satellite must be left without ICS history");
  }
  TEST_ASSERT(halos[4].galaxy->ICS_sum_mt == 16.0f && halos[4].galaxy->ICS_accrete == 0.5f,
              "a satellite without ICS keeps its history");

  /* ===== CLEANUP ===== */
  for (int i = 0; i < 5; i++) {
    free_test_halo(&halos[i]);
  }
  check_memory_leaks();

  return TEST_PASS;
}

/**
 * @test    test_central_consolidation_clamps
 * @brief   The central's ejected, CGM and ICS reservoirs pass SAGE26's clamp chain: metals capped
 *          to mass, a negative mass cleared with its metals, negative metals floored
 *
 * Each row loads the same (mass, metals) pair into all three reservoirs of a lone central and
 * expects the clamped pair in each (CGMrecipeOn 1, Regime 0, so the CGM pair meets only its own
 * clamp). A mass of -0.5 with metals 0.25 is first capped to -0.5 and then cleared with it.
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
    TEST_ASSERT(cgm_init("1") == 0, "init must accept CGMrecipeOn 1");
    struct GalaxyData gal;
    struct Halo halo;
    memset(&gal, 0, sizeof(gal));
    memset(&halo, 0, sizeof(halo));
    halo.galaxy = &gal;
    halo.Type = 0;
    halo.Mvir = 64.0;
    gal.Regime = 0;
    gal.HaloBaryonFraction = 0.25;
    gal.EjectedGas = gal.CGMgas = gal.ICS = rows[i].mass;
    gal.MetalsEjectedGas = gal.MetalsCGMgas = gal.MetalsICS = rows[i].metals;

    TEST_ASSERT(run_process(&halo, 1) == 0, "process must succeed");

    snprintf(msg, sizeof(msg), "ejected pair of row %zu (%g, %g)", i, (double)rows[i].mass,
             (double)rows[i].metals);
    TEST_ASSERT(gal.EjectedGas == rows[i].mass_out && gal.MetalsEjectedGas == rows[i].metals_out,
                msg);
    snprintf(msg, sizeof(msg), "CGM pair of row %zu (%g, %g)", i, (double)rows[i].mass,
             (double)rows[i].metals);
    TEST_ASSERT(gal.CGMgas == rows[i].mass_out && gal.MetalsCGMgas == rows[i].metals_out, msg);
    snprintf(msg, sizeof(msg), "ICS pair of row %zu (%g, %g)", i, (double)rows[i].mass,
             (double)rows[i].metals);
    TEST_ASSERT(gal.ICS == rows[i].mass_out && gal.MetalsICS == rows[i].metals_out, msg);
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

// ============================================================================
// CGMrecipeOn 1 AGAINST 0
// ============================================================================
//
// SAGE26's infall_recipe with CGMrecipeOn 1 keeps the satellites' CGM, adds none of it to the
// central and migrates only the central's own gas by its Regime; with CGMrecipeOn 0 it zeroes the
// satellites' CGM and adds the group's CGM totals to the central's hot gas. Every input below is a
// short binary fraction, so each float-plus-float migration, each double sum and the budget are
// exact, and each expectation is written as its literal value.

struct cgm_group {
  struct Halo halos[2];
  struct GalaxyData gal[2];
};

/** @brief A Type 0 central (Mvir 64) and a Type 1 satellite (Mvir 8), both holding both pairs */
static void cgm_make_group(struct cgm_group *g, const int central_regime,
                           const int satellite_regime) {
  memset(g, 0, sizeof(*g));
  for (int i = 0; i < 2; i++) {
    g->halos[i].galaxy = &g->gal[i];
    g->halos[i].SnapNum = 63;
  }
  g->halos[0].Type = 0;
  g->halos[0].Mvir = 64.0;
  g->halos[1].Type = 1;
  g->halos[1].Mvir = 8.0;

  struct GalaxyData *c = &g->gal[0];
  c->Regime = central_regime;
  c->HaloBaryonFraction = 0.25;
  c->StellarMass = 1.0f;
  c->BlackHoleMass = 0.0625f;
  c->ColdGas = 0.5f;
  c->HotGas = 2.0f;
  c->MetalsHotGas = 0.25f;
  c->CGMgas = 1.5f;
  c->MetalsCGMgas = 0.125f;
  c->EjectedGas = 0.5f;
  c->MetalsEjectedGas = 0.0625f;
  c->ICS = 0.25f;
  c->MetalsICS = 0.03125f;

  struct GalaxyData *s = &g->gal[1];
  s->Regime = satellite_regime;
  s->HaloBaryonFraction = 0.25;
  s->StellarMass = 0.5f;
  s->ColdGas = 0.25f;
  s->HotGas = 0.75f;
  s->MetalsHotGas = 0.0625f;
  s->CGMgas = 0.5f;
  s->MetalsCGMgas = 0.0625f;
  s->EjectedGas = 0.25f;
  s->MetalsEjectedGas = 0.03125f;
}

/**
 * @test    test_cgm_central_migration_by_regime
 * @brief   CGMrecipeOn 1 migrates only the central's own gas by its regime, pooling no satellite
 *          CGM; CGMrecipeOn 0 on the same input pools the group's CGM into the central's hot gas
 *
 * Group CGM 1.5 + 0.5 = 2.0 (metals 0.1875); budget 0.25 * 64 - 8.0625 = 7.9375 either way, since
 * SAGE26 counts every galaxy's CGM in the baryon sum.
 */
int test_cgm_central_migration_by_regime(void) {
  for (int regime = 0; regime <= 1; regime++) {
    struct cgm_group g;

    /* CGMrecipeOn 1 */
    TEST_ASSERT(cgm_init("1") == 0, "init must accept CGMrecipeOn 1");
    cgm_make_group(&g, regime, 1 - regime);
    TEST_ASSERT(run_process(g.halos, 2) == 0, "process must succeed");
    const struct GalaxyData *c = &g.gal[0];
    const struct GalaxyData *s = &g.gal[1];
    TEST_ASSERT(c->InfallingGas == 7.9375, "the budget counts every galaxy's CGM");
    if (regime == 0) {
      /* hot folds into CGM */
      TEST_ASSERT(c->CGMgas == 3.5f && c->MetalsCGMgas == 0.375f,
                  "a Regime 0 central's CGM must gain only its own hot pair (no satellite CGM)");
      TEST_ASSERT(c->HotGas == 0.0f && c->MetalsHotGas == 0.0f,
                  "a Regime 0 central's hot pair must be zeroed");
    } else {
      /* CGM folds into hot */
      TEST_ASSERT(
          c->HotGas == 3.5f && c->MetalsHotGas == 0.375f,
          "a Regime 1 central's hot gas must gain only its own CGM pair (no satellite CGM)");
      TEST_ASSERT(c->CGMgas == 0.0f && c->MetalsCGMgas == 0.0f,
                  "a Regime 1 central's CGM pair must be zeroed");
    }
    TEST_ASSERT(s->CGMgas == 0.5f && s->MetalsCGMgas == 0.0625f,
                "a satellite must keep its CGM with CGMrecipeOn 1");
    TEST_ASSERT(s->HotGas == 0.75f && s->MetalsHotGas == 0.0625f,
                "a satellite's hot pair must be unchanged");
    TEST_ASSERT(c->EjectedGas == 0.75f && c->MetalsEjectedGas == 0.09375f && s->EjectedGas == 0.0f,
                "the ejected pooling must be unchanged");

    /* CGMrecipeOn 0, the same input */
    TEST_ASSERT(cgm_init("0") == 0, "init must accept CGMrecipeOn 0");
    cgm_make_group(&g, regime, 1 - regime);
    TEST_ASSERT(run_process(g.halos, 2) == 0, "process must succeed");
    TEST_ASSERT(c->InfallingGas == 7.9375, "the budget must not depend on CGMrecipeOn");
    TEST_ASSERT(c->HotGas == 4.0f && c->MetalsHotGas == 0.4375f,
                "CGMrecipeOn 0 adds the group's CGM totals to the central's hot gas");
    TEST_ASSERT(c->CGMgas == 1.5f && c->MetalsCGMgas == 0.125f,
                "CGMrecipeOn 0 leaves the central's own CGM in place (SAGE26's off-branch quirk)");
    TEST_ASSERT(s->CGMgas == 0.0f && s->MetalsCGMgas == 0.0f,
                "CGMrecipeOn 0 zeroes a satellite's CGM");
    TEST_ASSERT(s->HotGas == 0.75f && s->MetalsHotGas == 0.0625f,
                "a satellite's hot pair must be unchanged");
  }
  return TEST_PASS;
}

/**
 * @brief   Main test runner
 *
 * Executes all sage26_prepare_infall_budget unit tests and reports results.
 */
int main(void) {
  printf("%s", BLUE);
  printf("============================================================\n");
  printf("Test Suite: sage26_prepare_infall_budget Module\n");
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

  /* CGMrecipeOn 1 against 0 */
  printf("\n%s=== CGM Regime Routing Tests ===%s\n", BLUE, NC);
  TEST_RUN(test_cgm_central_migration_by_regime);

  /* Print summary and return result */
  TEST_SUMMARY();
  return TEST_RESULT();
}
