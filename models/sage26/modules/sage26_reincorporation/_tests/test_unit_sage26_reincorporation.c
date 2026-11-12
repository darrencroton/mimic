/**
 * @file    test_unit_sage26_reincorporation.c
 * @brief   Unit tests for sage26_reincorporation module
 *
 * Validates: module placement, the reincorporation rate, the central-only constraint, the
 * transfer's conservation and metallicity, edge cases, error handling and the CGMrecipeOn routing
 *
 * Test cases:
 *   LIFECYCLE:
 *   - test_module_initialization: the module initialises, and cleans up without leaks, in a
 *     valid placement
 *
 *   PHYSICS:
 *   - test_physics_reincorporation_rate: the reincorporated mass against an independently written
 *     expression for a table of (Vvir, ReIncorporationFactor set by name, dT, substeps, ejected
 *     gas) rows: above, below and at the critical velocity, the factor scaling it up and down,
 *     a zero factor, the per-substep dt, and the cap at the ejected gas
 *   - test_physics_central_only: only Type 0 centrals reincorporate
 *   - test_physics_full_halo_nonzero_central_index: full-halo dispatch uses the true FOF central
 *   - test_physics_metallicity_preservation: gas and metals move at the ejected metallicity, and
 *     total gas and metals are conserved
 *   - test_physics_zero_ejected_gas: no reincorporation when EjectedGas = 0
 *
 *   ERROR HANDLING:
 *   - test_null_galaxy_safety: a central with a NULL galaxy returns -1
 *
 *   CGMrecipeOn 1 against 0:
 *   - test_cgm_reincorporation_destination: the destination by CGMrecipeOn and the central's Regime
 *
 * The CGMrecipeOn refusals of the module are asserted, with diagnostics, by
 * test_unit_sage26_parameter_refusals.c.
 *
 * @author  Mimic Development Team
 * @date    2025-12-18
 */

#include "../../../../tests/framework/test_framework.h"
#include "core/module_registry.h"
#include "../../../../tests/framework/test_phase_config.h"
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

/* External module interface for direct testing */
extern int sage26_reincorporation_init(void);
extern int sage26_reincorporation_process(struct ModuleContext *ctx, struct Halo *halos, int ngal);
extern int sage26_reincorporation_cleanup(void);

/* Test fixtures for physics tests */

/**
 * @brief Initialize sage26_reincorporation module for physics testing
 *
 * Sets up module with ReIncorporationFactor = 1.0 (standard SAGE value, from
 * set_test_model_parameters), so Vcrit = 354.53 km/s
 */
static void setup_module_for_physics_test(void) {
  reset_config();
  set_test_model_parameters();
  add_test_cgm_recipe_parameter(); /* CGMrecipeOn: 0, required */

  sage26_reincorporation_init();
}

/**
 * @brief Create a test halo with galaxy for physics tests
 *
 * @param type Halo type (0=central, 1=satellite, 2=orphan)
 * @param vvir Virial velocity (km/s)
 * @param rvir Virial radius (Mpc/h)
 * @return Allocated halo (must be freed with free_test_halo)
 */
static struct Halo create_test_halo(int type, float vvir, float rvir) {
  struct Halo halo;
  memset(&halo, 0, sizeof(halo));

  halo.Type = type;
  halo.Vvir = vvir;
  halo.Rvir = rvir;
  halo.SnapNum = 63; /* z=0 */
  halo.dT = 0.1;     /* Default test timestep (overridden when needed) */

  /* Allocate galaxy data */
  halo.galaxy = mymalloc_cat(sizeof(struct GalaxyData), MEM_HALOS);
  memset(halo.galaxy, 0, sizeof(struct GalaxyData));

  return halo;
}

/**
 * @brief Create minimal module context for testing
 */
static struct ModuleContext create_test_context(double dt) {
  struct ModuleContext ctx;
  memset(&ctx, 0, sizeof(ctx));

  ctx.substep_dt = dt;
  ctx.redshift = 0.0;
  ctx.time = 13.6; /* Gyr */
  ctx.snapshot_number = 63;
  ctx.substep_number = 0;
  ctx.num_substeps = 1;
  ctx.params = &MimicConfig;

  return ctx;
}

/* ========================================================================== */
/* LIFECYCLE TESTS                                                           */
/* ========================================================================== */

/**
 * @test    test_module_initialization
 * @brief   The module initialises and cleans up without leaks in a valid placement
 */
int test_module_initialization(void) {
  reset_config();
  init_memory_system(0);
  ensure_modules_registered();

  MimicConfig.Omega = 0.25;
  MimicConfig.OmegaLambda = 0.75;
  MimicConfig.Hubble_h = 0.73;

  test_phase_add("galaxy_physics", "sage26_reincorporation", PROCESSING_MODE_FULL_HALO);
  MimicConfig.SubSteps = 1;
  set_test_model_parameters();
  add_test_cgm_recipe_parameter(); /* CGMrecipeOn: 0, required */

  int result = module_system_init();
  TEST_ASSERT(result == 0, "Module system initialization should succeed");

  module_system_cleanup();
  check_memory_leaks();

  return TEST_PASS;
}

/* ========================================================================== */
/* PHYSICS TESTS                                                             */
/* ========================================================================== */

/**
 * @brief Initialise the module with ReIncorporationFactor `factor` and CGMrecipeOn 0
 *
 * Both parameters are set by name on a cleared configuration, so the value the module reads is
 * the one the caller passed.
 */
static int init_with_factor(const char *factor) {
  reset_config();
  sage26_test_append_model_parameter("ReIncorporationFactor", factor);
  sage26_test_append_model_parameter("CGMrecipeOn", "0");
  return sage26_reincorporation_init();
}

/**
 * @test    test_physics_reincorporation_rate
 * @brief   The reincorporated mass equals (Vvir/Vcrit - 1) * EjectedGas / (Rvir/Vvir) * dt, capped
 *          at EjectedGas, with Vcrit = 354.53 km/s * ReIncorporationFactor, and zero for a halo
 *          not above Vcrit
 *
 * Each row sets ReIncorporationFactor by name, gives a central an ejected reservoir of metallicity
 * 0.03 and an empty hot halo, and runs one substep. The expected mass is written out here from
 * the formula, with the float Vvir and Rvir the module reads and dt = dT / num_substeps. Hot gas
 * starts empty, so what it gains is the reincorporated mass. The reservoirs are float32, so the
 * tolerance is 1e-6 of the reservoir the value is stored in; a wrong rate, critical velocity or
 * dt moves the result by far more. The small dT keeps the rate below the cap except in the
 * capped row. A row not above Vcrit must leave every reservoir bit for bit.
 */
int test_physics_reincorporation_rate(void) {
  const struct {
    const char *name;
    float vvir;         /* km/s */
    float rvir;         /* Mpc/h */
    const char *factor; /* ReIncorporationFactor */
    double dT;          /* the halo's timestep */
    int num_substeps;
    float ejected; /* EjectedGas */
    int acts;      /* Vvir above Vcrit, so gas returns */
  } rows[] = {
      {"above Vcrit", 500.0f, 0.2f, "1.0", 1e-4, 1, 10.0f, 1},
      {"below Vcrit", 300.0f, 0.2f, "1.0", 1e-4, 1, 10.0f, 0},
      {"float Vvir 354.53 is below the double Vcrit", 354.53f, 0.2f, "1.0", 1e-4, 1, 10.0f, 0},
      {"factor 0.5 lowers Vcrit below Vvir 300", 300.0f, 0.2f, "0.5", 1e-4, 1, 10.0f, 1},
      {"factor 2 raises Vcrit above Vvir 500", 500.0f, 0.2f, "2.0", 1e-4, 1, 10.0f, 0},
      {"factor 0 gives no critical velocity", 500.0f, 0.2f, "0.0", 1e-4, 1, 10.0f, 0},
      {"dt is dT over 5 substeps", 500.0f, 0.2f, "1.0", 1e-4, 5, 10.0f, 1},
      {"capped at the ejected gas", 800.0f, 0.3f, "1.0", 1.0, 1, 0.5f, 1},
  };
  const double ejected_metallicity = 0.03;
  int ok = 1;
  init_memory_system(0);
  for (size_t k = 0; k < sizeof(rows) / sizeof(rows[0]); k++) {
    TEST_ASSERT(init_with_factor(rows[k].factor) == 0, "init must accept the row's factor");

    struct Halo halo = create_test_halo(0, rows[k].vvir, rows[k].rvir);
    halo.dT = rows[k].dT;
    struct GalaxyData *g = halo.galaxy;
    g->EjectedGas = rows[k].ejected;
    g->MetalsEjectedGas = (float)(ejected_metallicity * rows[k].ejected);
    struct ModuleContext ctx = create_test_context(0.1);
    ctx.num_substeps = rows[k].num_substeps;
    ctx.central_galaxy = &halo;
    const double ejected_in = g->EjectedGas;
    const double metals_in = g->MetalsEjectedGas;

    const int status = sage26_reincorporation_process(&ctx, &halo, 1);

    double mass = 0.0;
    if (rows[k].acts) {
      const double vcrit = 354.53 * strtod(rows[k].factor, NULL);
      const double dt = rows[k].dT / rows[k].num_substeps;
      mass = (rows[k].vvir / vcrit - 1.0) * ejected_in / (double)(rows[k].rvir / rows[k].vvir) * dt;
      if (mass > ejected_in) {
        mass = ejected_in;
      }
    }
    const double metallicity = metals_in / ejected_in;
    const int moved_ok =
        fabs(g->HotGas - mass) <= 1e-6 * mass &&
        fabs(g->MetalsHotGas - metallicity * mass) <= 1e-6 * metallicity * mass &&
        fabs(g->EjectedGas - (ejected_in - mass)) <= 1e-6 * ejected_in &&
        fabs(g->MetalsEjectedGas - (metals_in - metallicity * mass)) <= 1e-6 * metals_in;
    const int unchanged_ok = g->HotGas == 0.0f && g->MetalsHotGas == 0.0f &&
                             g->EjectedGas == (float)ejected_in &&
                             g->MetalsEjectedGas == (float)metals_in;
    if (status != 0 || !(rows[k].acts ? moved_ok : unchanged_ok)) {
      printf("\n  row '%s': status %d, hot %.9g (expected %.9g), ejected %.9g (expected %.9g)",
             rows[k].name, status, (double)g->HotGas, mass, (double)g->EjectedGas,
             ejected_in - mass);
      ok = 0;
    }
    free_test_halo(&halo);
    sage26_reincorporation_cleanup();
  }
  check_memory_leaks();
  TEST_ASSERT(ok, "the reincorporated mass must match the formula in every row");
  return TEST_PASS;
}

/**
 * @test    test_physics_central_only
 * @brief   Test that only Type 0 centrals reincorporate gas
 */
int test_physics_central_only(void) {
  init_memory_system(0);
  setup_module_for_physics_test();

  /* Test satellite (Type 1) */
  struct Halo satellite = create_test_halo(1, 500.0, 0.2);
  satellite.galaxy->EjectedGas = 10.0;
  satellite.galaxy->HotGas = 5.0;

  struct ModuleContext ctx = create_test_context(0.1);

  /* Create a central for the context */
  struct Halo central = create_test_halo(0, 600.0, 0.3);
  ctx.central_galaxy = &central;

  double initial_ejected = satellite.galaxy->EjectedGas;
  double initial_hot = satellite.galaxy->HotGas;

  /* Execute on satellite */
  int result = sage26_reincorporation_process(&ctx, &satellite, 1);
  TEST_ASSERT(result == 0, "Process should succeed");

  /* Validate: Satellite properties should be unchanged */
  TEST_ASSERT_DOUBLE_EQUAL(satellite.galaxy->EjectedGas, initial_ejected, 1e-10,
                           "Satellite EjectedGas should not change");
  TEST_ASSERT_DOUBLE_EQUAL(satellite.galaxy->HotGas, initial_hot, 1e-10,
                           "Satellite HotGas should not change");

  /* Test orphan (Type 2) */
  struct Halo orphan = create_test_halo(2, 500.0, 0.2);
  orphan.galaxy->EjectedGas = 10.0;
  orphan.galaxy->HotGas = 5.0;

  initial_ejected = orphan.galaxy->EjectedGas;
  initial_hot = orphan.galaxy->HotGas;

  result = sage26_reincorporation_process(&ctx, &orphan, 1);
  TEST_ASSERT(result == 0, "Process should succeed");

  /* Validate: Orphan properties should be unchanged */
  TEST_ASSERT_DOUBLE_EQUAL(orphan.galaxy->EjectedGas, initial_ejected, 1e-10,
                           "Orphan EjectedGas should not change");
  TEST_ASSERT_DOUBLE_EQUAL(orphan.galaxy->HotGas, initial_hot, 1e-10,
                           "Orphan HotGas should not change");

  /* Cleanup */
  free_test_halo(&satellite);
  free_test_halo(&orphan);
  free_test_halo(&central);
  sage26_reincorporation_cleanup();
  check_memory_leaks();

  return TEST_PASS;
}

/**
 * @test    test_physics_full_halo_nonzero_central_index
 * @brief   Test that full-halo dispatch uses the context central, not halos[0]
 */
int test_physics_full_halo_nonzero_central_index(void) {
  init_memory_system(0);
  setup_module_for_physics_test();

  struct Halo halos[2];
  halos[0] = create_test_halo(1, 500.0, 0.2);
  halos[1] = create_test_halo(0, 600.0, 0.3);

  halos[0].galaxy->EjectedGas = 4.0;
  halos[0].galaxy->HotGas = 2.0;
  halos[1].galaxy->EjectedGas = 10.0;
  halos[1].galaxy->HotGas = 5.0;

  struct ModuleContext ctx = create_test_context(0.1);
  ctx.central_index = 1;
  ctx.central_galaxy = &halos[1];

  const double central_initial_ejected = halos[1].galaxy->EjectedGas;
  const double central_initial_hot = halos[1].galaxy->HotGas;
  const double satellite_initial_ejected = halos[0].galaxy->EjectedGas;
  const double satellite_initial_hot = halos[0].galaxy->HotGas;

  int result = sage26_reincorporation_process(&ctx, halos, 2);
  TEST_ASSERT(result == 0, "Full-halo reincorporation should succeed");

  TEST_ASSERT(halos[1].galaxy->EjectedGas < central_initial_ejected,
              "Central EjectedGas should decrease");
  TEST_ASSERT(halos[1].galaxy->HotGas > central_initial_hot, "Central HotGas should increase");
  TEST_ASSERT_DOUBLE_EQUAL(halos[0].galaxy->EjectedGas, satellite_initial_ejected, 1e-10,
                           "Satellite EjectedGas should be unchanged");
  TEST_ASSERT_DOUBLE_EQUAL(halos[0].galaxy->HotGas, satellite_initial_hot, 1e-10,
                           "Satellite HotGas should be unchanged");

  free_test_halo(&halos[0]);
  free_test_halo(&halos[1]);
  sage26_reincorporation_cleanup();
  check_memory_leaks();

  return TEST_PASS;
}

/**
 * @test    test_physics_metallicity_preservation
 * @brief   Test that gas moves from the ejected to the hot reservoir at the ejected metallicity,
 *          conserving total gas and total metals
 */
int test_physics_metallicity_preservation(void) {
  init_memory_system(0);
  setup_module_for_physics_test();

  /* Create halo with specific metallicity in ejected gas. The timestep is small enough that the
   * rate stays below the cap, so only part of the ejected gas moves. */
  struct Halo halo = create_test_halo(0, 500.0, 0.2);
  halo.dT = 1e-4;
  halo.galaxy->EjectedGas = 10.0;      /* 1e11 Msun/h */
  halo.galaxy->MetalsEjectedGas = 0.3; /* 3e9 Msun/h → Z = 0.03 */
  halo.galaxy->HotGas = 5.0;
  halo.galaxy->MetalsHotGas = 0.05; /* Z_hot initial = 0.01 */

  struct ModuleContext ctx = create_test_context(0.1);
  ctx.central_galaxy = &halo;

  /* Calculate expected metallicity */
  double Z_ejected = halo.galaxy->MetalsEjectedGas / halo.galaxy->EjectedGas;

  /* Store initial values */
  double initial_ejected = halo.galaxy->EjectedGas;
  double initial_metals_ejected = halo.galaxy->MetalsEjectedGas;
  double initial_hot = halo.galaxy->HotGas;
  double initial_metals_hot = halo.galaxy->MetalsHotGas;

  /* Execute */
  int result = sage26_reincorporation_process(&ctx, &halo, 1);
  TEST_ASSERT(result == 0, "Process should succeed");

  /* Calculate transferred amounts */
  double mass_transferred = initial_ejected - halo.galaxy->EjectedGas;
  double metals_transferred = initial_metals_ejected - halo.galaxy->MetalsEjectedGas;
  double expected_metals_transferred = Z_ejected * mass_transferred;

  /* Validate: gas moved from the ejected to the hot reservoir */
  TEST_ASSERT(mass_transferred > 0.0, "Some mass should be reincorporated");
  TEST_ASSERT(halo.galaxy->HotGas > initial_hot, "HotGas should increase");

  /* Validate: Metallicity preserved in transfer */
  TEST_ASSERT_DOUBLE_EQUAL(metals_transferred, expected_metals_transferred, 1e-6,
                           "Metals transferred should match ejected metallicity");

  /* Validate: Total mass conservation */
  double total_mass_before = initial_ejected + initial_hot;
  double total_mass_after = halo.galaxy->EjectedGas + halo.galaxy->HotGas;
  TEST_ASSERT_DOUBLE_EQUAL(total_mass_before, total_mass_after, 1e-6,
                           "Total mass should be conserved");

  /* Validate: Total metals conservation */
  double total_metals_before = initial_metals_ejected + initial_metals_hot;
  double total_metals_after = halo.galaxy->MetalsEjectedGas + halo.galaxy->MetalsHotGas;
  TEST_ASSERT_DOUBLE_EQUAL(total_metals_before, total_metals_after, 1e-6,
                           "Total metals should be conserved");

  /* Cleanup */
  free_test_halo(&halo);
  sage26_reincorporation_cleanup();
  check_memory_leaks();

  return TEST_PASS;
}

/**
 * @test    test_physics_zero_ejected_gas
 * @brief   Test that module handles zero ejected gas gracefully
 */
int test_physics_zero_ejected_gas(void) {
  init_memory_system(0);
  setup_module_for_physics_test();

  /* Create halo with no ejected gas */
  struct Halo halo = create_test_halo(0, 500.0, 0.2);
  halo.galaxy->EjectedGas = 0.0;
  halo.galaxy->MetalsEjectedGas = 0.0;
  halo.galaxy->HotGas = 5.0;
  halo.galaxy->MetalsHotGas = 0.1;

  struct ModuleContext ctx = create_test_context(0.1);
  ctx.central_galaxy = &halo;

  double initial_hot = halo.galaxy->HotGas;

  /* Execute */
  int result = sage26_reincorporation_process(&ctx, &halo, 1);
  TEST_ASSERT(result == 0, "Process should succeed with zero ejected gas");

  /* Validate: Nothing should change */
  TEST_ASSERT_DOUBLE_EQUAL(halo.galaxy->EjectedGas, 0.0, 1e-10, "EjectedGas should remain zero");
  TEST_ASSERT_DOUBLE_EQUAL(halo.galaxy->HotGas, initial_hot, 1e-10, "HotGas should not change");

  /* Cleanup */
  free_test_halo(&halo);
  sage26_reincorporation_cleanup();
  check_memory_leaks();

  return TEST_PASS;
}

/* ========================================================================== */
/* ERROR HANDLING TESTS                                                      */
/* ========================================================================== */

/**
 * @test    test_null_galaxy_safety
 * @brief   Test that module handles NULL galaxy pointer safely
 */
int test_null_galaxy_safety(void) {
  init_memory_system(0);
  setup_module_for_physics_test();

  struct Halo halo = create_test_halo(0, 500.0, 0.2);

  /* Free the galaxy to create NULL pointer */
  myfree(halo.galaxy);
  halo.galaxy = NULL;

  struct ModuleContext ctx = create_test_context(0.1);
  ctx.central_galaxy = &halo; /* Still need valid central */

  /* Execute - should handle NULL gracefully */
  int result = sage26_reincorporation_process(&ctx, &halo, 1);

  /* Module returns -1 for NULL galaxy (error condition) */
  TEST_ASSERT(result == -1, "Should return error for NULL galaxy");

  /* Cleanup (no galaxy to free) */
  sage26_reincorporation_cleanup();
  check_memory_leaks();

  return TEST_PASS;
}

// ============================================================================
// CGMrecipeOn 1 AGAINST 0
// ============================================================================
//
// SAGE26's reincorporate_gas changes only its destination with CGMrecipeOn:
// add_gas_to_hot_reservoir gives the central's CGM pair for CGMrecipeOn 1 and Regime 0, its hot
// pair otherwise. The central (Vvir 800, Rvir 0.3, dT 1.0 over one substep, ReIncorporationFactor
// 1.0) would reincorporate far more than its ejected 0.5, so the cap binds: all 0.5 returns, with
// its metals 0.0625 (metallicity 1/8), leaving 0 / 0 ejected. Every reservoir value, before and
// after, is a short binary fraction, so each expectation is an exact literal; Vvir, Rvir and dT
// (0.3 is not one) only make the cap bind and never reach a reservoir.

/** @brief Initialise with ReIncorporationFactor 1.0 and CGMrecipeOn `cgm` */
static int cgm_init(const char *cgm) {
  reset_config();
  sage26_test_append_model_parameter("ReIncorporationFactor", "1.0");
  sage26_test_append_model_parameter("CGMrecipeOn", cgm);
  return sage26_reincorporation_init();
}

/**
 * @test    test_cgm_reincorporation_destination
 * @brief   The capped reincorporation goes to the CGM pair at CGMrecipeOn 1 with Regime 0 and to
 *          the hot pair at CGMrecipeOn 1 with Regime 1 and at CGMrecipeOn 0; the other pair is
 *          unchanged and the ejected debits are the same in each
 */
int test_cgm_reincorporation_destination(void) {
  const struct {
    const char *cgm;
    int regime;
    int to_cgm;
  } runs[] = {{"1", 0, 1}, {"1", 1, 0}, {"0", 0, 0}};
  int ok = 1;
  init_memory_system(0);
  for (size_t k = 0; k < sizeof(runs) / sizeof(runs[0]); k++) {
    TEST_ASSERT(cgm_init(runs[k].cgm) == 0, "init must accept CGMrecipeOn 0 and 1");
    struct Halo halo = create_test_halo(0, 800.0, 0.3);
    halo.dT = 1.0;
    struct GalaxyData *g = halo.galaxy;
    g->Regime = runs[k].regime;
    g->EjectedGas = 0.5f;
    g->MetalsEjectedGas = 0.0625f;
    g->HotGas = 3.0f;
    g->MetalsHotGas = 0.375f;
    g->CGMgas = 2.0f;
    g->MetalsCGMgas = 0.25f;
    struct ModuleContext ctx = create_test_context(1.0);
    ctx.central_galaxy = &halo;
    const int status = sage26_reincorporation_process(&ctx, &halo, 1);

    const int dest_ok = runs[k].to_cgm ? (g->CGMgas == 2.5f && g->MetalsCGMgas == 0.3125f &&
                                          g->HotGas == 3.0f && g->MetalsHotGas == 0.375f)
                                       : (g->HotGas == 3.5f && g->MetalsHotGas == 0.4375f &&
                                          g->CGMgas == 2.0f && g->MetalsCGMgas == 0.25f);
    const int ejected_ok = g->EjectedGas == 0.0f && g->MetalsEjectedGas == 0.0f;
    if (status != 0 || !dest_ok || !ejected_ok) {
      printf("\n  CGMrecipeOn %s, Regime %d: status %d, hot %.9g/%.9g, CGM %.9g/%.9g, ejected "
             "%.9g/%.9g",
             runs[k].cgm, runs[k].regime, status, (double)g->HotGas, (double)g->MetalsHotGas,
             (double)g->CGMgas, (double)g->MetalsCGMgas, (double)g->EjectedGas,
             (double)g->MetalsEjectedGas);
      ok = 0;
    }
    free_test_halo(&halo);
    sage26_reincorporation_cleanup();
  }
  check_memory_leaks();
  TEST_ASSERT(ok, "the reincorporated gas and metals must reach exactly the selected pair");
  return TEST_PASS;
}

/**
 * @brief   Main test runner
 *
 * Executes all sage26_reincorporation tests and reports results.
 */
int main(void) {
  printf("%s", BLUE);
  printf("============================================================\n");
  printf("Test Suite: sage26_reincorporation Module\n");
  printf("============================================================\n");
  printf("%s\n", NC);

  /* Initialize error handling for tests */
  initialize_error_handling(LOG_LEVEL_DEBUG, NULL);

  /* Run lifecycle tests */
  printf("\n%sLIFECYCLE TESTS:%s\n", BLUE, NC);
  TEST_RUN(test_module_initialization);

  /* Run physics tests */
  printf("\n%sPHYSICS TESTS:%s\n", BLUE, NC);
  TEST_RUN(test_physics_reincorporation_rate);
  TEST_RUN(test_physics_central_only);
  TEST_RUN(test_physics_full_halo_nonzero_central_index);
  TEST_RUN(test_physics_metallicity_preservation);
  TEST_RUN(test_physics_zero_ejected_gas);

  /* Run error handling tests */
  printf("\n%sERROR HANDLING TESTS:%s\n", BLUE, NC);
  TEST_RUN(test_null_galaxy_safety);

  /* CGMrecipeOn 1 against 0 */
  printf("\n%sCGM REGIME ROUTING TESTS:%s\n", BLUE, NC);
  TEST_RUN(test_cgm_reincorporation_destination);

  /* Print summary and return result */
  TEST_SUMMARY();
  return TEST_RESULT();
}
