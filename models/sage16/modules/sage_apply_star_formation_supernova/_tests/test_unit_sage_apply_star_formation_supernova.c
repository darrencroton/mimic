/**
 * @file    test_unit_sage_apply_star_formation_supernova.c
 * @brief   Unit tests for sage_apply_star_formation_supernova module
 *
 * Validates: the commit of one substep's star formation and supernova feedback to the forming
 * galaxy and its FoF central
 *
 * Test cases:
 *   - test_module_initialization: the module initialises and cleans up in its valid placement
 *   - test_star_formation_capped_at_cold_gas_without_calculator: stars capped at ColdGas and
 *     written back to NewStellarMass when no SN calculator is configured
 *   - test_commit_table: one table of substeps with exact literal expectations (RecycleFraction
 *     0.25, so a swapped R and 1 - R differ): star formation with recycling and the metal
 *     transfer at the cold-gas metallicity, the reheated gas and metals into the central's hot
 *     gas at the post-star-formation metallicity, the ejection capped after the reheating and its
 *     metals at the central's metallicity after the reheating, SupernovaOutflowRate accumulating
 *     the reheated mass over dT, a substep with no stars (and the cold-gas cap at no cold gas)
 *     leaving everything but the consumed transport fields alone, and the StarFormationRate and
 *     SupernovaOutflowRate guards on a zero interval. Every row also asserts that the forming
 *     galaxy's own hot and ejected pairs and the central's other reservoirs are untouched, that
 *     NewStellarMass is left for the enrichment module, that the consumed transport fields are
 *     zeroed, and that baryons and metals are conserved
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
#include <string.h>
#include <math.h>

/* Shared SAGE16 test fixture boilerplate (counters, config reset, module registration) */
#include "modules/_tests/sage_test_fixtures.h"

/* Module functions (extern declarations for direct testing) */
extern int sage_apply_star_formation_supernova_init(void);
extern int sage_apply_star_formation_supernova_process(struct ModuleContext *ctx,
                                                       struct Halo *halos, int ngal);
extern int sage_apply_star_formation_supernova_cleanup(void);

// ============================================================================
// TEST FIXTURES AND HELPERS
// ============================================================================

/**
 * @brief   Setup module parameters for testing
 *
 * @param   recycle     RecycleFraction [0,1], the module's only parameter
 */
static void setup_test_parameters(double recycle) {
  /* Set model parameters in MimicConfig */
  snprintf(MimicConfig.ModelParams[0].param_name, MAX_STRING_LEN, "RecycleFraction");
  snprintf(MimicConfig.ModelParams[0].value, MAX_STRING_LEN, "%.6f", recycle);
  MimicConfig.NumModelParams = 1;
}

/**
 * @brief   Setup module context for testing
 *
 * @param   ctx             ModuleContext to initialize
 * @param   central         Pointer to central galaxy halo
 * @param   dt              Time step
 */
static void setup_module_context(struct ModuleContext *ctx, struct Halo *central, double dt) {
  memset(ctx, 0, sizeof(struct ModuleContext));
  ctx->central_galaxy = central;
  ctx->substep_dt = dt;
  ctx->params = &MimicConfig;
  ctx->redshift = 0.0;
  ctx->time = 13.8; /* Gyr */
  ctx->snapshot_number = 63;
  ctx->substep_number = 0;
  ctx->num_substeps = 1;
}

/**
 * @brief   Setup test halo and galaxy with specified properties
 *
 * @param   halo            Halo to initialize
 * @param   galaxy          Galaxy to initialize
 * @param   type            Halo type (0=central, 1=satellite, 2=orphan)
 * @param   mvir            Virial mass (1e10 Msun/h)
 * @param   dt              Time step (Gyr) for rate calculations
 * @param   cold_gas        Cold gas mass (1e10 Msun/h)
 * @param   hot_gas         Hot gas mass (1e10 Msun/h)
 * @param   ejected_gas     Ejected gas mass (1e10 Msun/h)
 * @param   stellar_mass    Stellar mass (1e10 Msun/h)
 * @param   metals_cold     Metals in cold gas (1e10 Msun/h)
 * @param   metals_hot      Metals in hot gas (1e10 Msun/h)
 * @param   metals_ejected  Metals in ejected gas (1e10 Msun/h)
 * @param   metals_stellar  Metals in stellar mass (1e10 Msun/h)
 * @param   new_stars       NewStellarMass (calculated by previous module)
 * @param   reheated_mass   SupernovaReheatedMass (calculated by previous module)
 * @param   ejected_mass    SupernovaEjectedMass (calculated by previous module)
 */
static void setup_test_halo(struct Halo *halo, struct GalaxyData *galaxy, int type, double mvir,
                            double dt, double cold_gas, double hot_gas, double ejected_gas,
                            double stellar_mass, double metals_cold, double metals_hot,
                            double metals_ejected, double metals_stellar, double new_stars,
                            double reheated_mass, double ejected_mass) {
  memset(halo, 0, sizeof(struct Halo));
  memset(galaxy, 0, sizeof(struct GalaxyData));

  halo->Type = type;
  halo->Mvir = mvir;
  halo->Vvir = 150.0; /* km/s */
  halo->SnapNum = 63;
  halo->dT = dt;
  halo->galaxy = galaxy;

  galaxy->ColdGas = cold_gas;
  galaxy->HotGas = hot_gas;
  galaxy->EjectedGas = ejected_gas;
  galaxy->StellarMass = stellar_mass;
  galaxy->MetalsColdGas = metals_cold;
  galaxy->MetalsHotGas = metals_hot;
  galaxy->MetalsEjectedGas = metals_ejected;
  galaxy->MetalsStellarMass = metals_stellar;
  galaxy->NewStellarMass = new_stars;
  galaxy->SupernovaReheatedMass = reheated_mass;
  galaxy->SupernovaEjectedMass = ejected_mass;
  galaxy->StarFormationRate = 0.0;
  galaxy->SupernovaOutflowRate = 0.0;
}

// ============================================================================
// SOFTWARE QUALITY TESTS
// ============================================================================

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

  /* Configure sage_apply_star_formation_supernova module in galaxy_physics */
  test_phase_add("galaxy_physics", "sage_apply_star_formation_supernova",
                 PROCESSING_MODE_BY_GALAXY);
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
 * @test    test_star_formation_capped_at_cold_gas_without_calculator
 * @brief   Test that stars are capped at ColdGas when no SN calculator is configured
 *
 * Physics: SAGE's renormalisation with reheated_mass = 0 (stars *= ColdGas / stars,
 *          sage-model/src/model_starformation_and_feedback.c:50-54). With no pipeline
 *          configured, sage_calculate_supernova_feedback is absent, so nothing else caps SF.
 *
 * Expected: NewStellarMass written back as ColdGas; ColdGas ends at RecycleFraction * ColdGas
 * Validates: Cap value, write-back for sage_apply_metal_enrichment, no negative ColdGas
 */
int test_star_formation_capped_at_cold_gas_without_calculator(void) {
  /* ===== SETUP ===== */
  const double recycle_fraction = 0.43;
  reset_config();
  setup_test_parameters(recycle_fraction);

  int init_result = sage_apply_star_formation_supernova_init();
  TEST_ASSERT(init_result == 0, "Module initialization should succeed");

  struct Halo test_halo, central_halo;
  struct GalaxyData test_galaxy, central_galaxy;

  setup_test_halo(&central_halo, &central_galaxy, 0, 100.0, 0.1, 10.0, 50.0, 0.0, 20.0, 0.0, 0.0,
                  0.0, 0.0, 0.0, 0.0, 0.0);

  /* 0.5 ColdGas but 2.0 new stars requested: far more than the cold gas */
  const double initial_cold = 0.5;
  const double initial_stellar = 20.0;
  setup_test_halo(&test_halo, &test_galaxy, 0, 100.0, 0.1, initial_cold, 50.0, 0.0, initial_stellar,
                  0.0, 0.0, 0.0, 0.0, 2.0, 0.0, 0.0);

  struct ModuleContext ctx;
  setup_module_context(&ctx, &central_halo, 0.1);

  /* ===== EXECUTE ===== */
  int result = sage_apply_star_formation_supernova_process(&ctx, &test_halo, 1);

  /* ===== VALIDATE ===== */
  TEST_ASSERT(result == 0, "Module process should succeed");
  TEST_ASSERT_DOUBLE_EQUAL(test_galaxy.NewStellarMass, initial_cold, 1e-6,
                           "NewStellarMass should be written back capped at ColdGas");
  TEST_ASSERT_DOUBLE_EQUAL(test_galaxy.ColdGas, recycle_fraction * initial_cold, 1e-6,
                           "ColdGas should end at RecycleFraction * initial ColdGas");
  TEST_ASSERT(test_galaxy.ColdGas >= 0.0, "ColdGas must not go negative");
  TEST_ASSERT_DOUBLE_EQUAL(test_galaxy.StellarMass,
                           initial_stellar + (1.0 - recycle_fraction) * initial_cold, 1e-6,
                           "StellarMass should grow by the capped star formation");

  return TEST_PASS;
}

// ============================================================================
// THE COMMIT TABLE
// ============================================================================
//
// One forming galaxy and one separate FoF central, one substep, no supernova calculator
// configured (so the cold-gas cap is live). Every value is a float: the inputs and expectations
// that are binary fractions are exact, the rest are the nine-digit literals of the floats they
// round to. RecycleFraction is 0.25, so the locked fraction 1 - R = 0.75 is not the recycled one.

/** @brief Whether two floats are equal; reports the row and field if not */
static int expect_float(const char *row, const char *field, const float actual,
                        const float expected) {
  if (actual == expected) {
    return 1;
  }
  printf("\n  row %s: %s = %.9g, expected %.9g", row, field, (double)actual, (double)expected);
  return 0;
}

/**
 * @test    test_commit_table
 * @brief   The apply step's commit of one substep, row by row, on exact literals
 *
 * Physics per row: stars lock (1 - R) * stars of cold gas, at the cold gas's metallicity, into
 * the galaxy's stars; the reheated mass moves from the galaxy's cold gas to the central's hot gas
 * at the post-star-formation metallicity; the ejected mass (capped at the central's hot gas after
 * the reheating) moves from the central's hot gas to its ejected gas at the central's
 * metallicity after the reheating; SupernovaOutflowRate gains reheated / dT and StarFormationRate
 * gains stars / dT, both only when dT is positive. A substep with no stars (after the cold-gas
 * cap, which is zero for no cold gas) returns early: nothing moves, so a reheated or ejected mass
 * it carries is discarded, and only the two consumed transport fields are zeroed. NewStellarMass
 * is left for the enrichment module.
 *
 * The "metals after star formation" row is the one a stale pre-star-formation metallicity would get
 * wrong: the cold gas 2.0 holds 0.1f metals, the stars take 0.5 * 0.75 of it, and the metallicity
 * the reheating uses is the ratio of the floats left, which differs from the pre-star-formation
 * ratio by one float rounding that shows in the credited metals.
 */
int test_commit_table(void) {
  static const struct {
    const char *name;
    double recycle, dt;
    /* the forming galaxy */
    float cold, hot, ejected, stellar;
    float metals_cold, metals_hot, metals_ejected, metals_stellar;
    float outflow; /* SupernovaOutflowRate before the call */
    /* NewStellarMass, SupernovaReheatedMass and SupernovaEjectedMass */
    double stars, reheated, ejected_mass;
    /* the FoF central */
    float c_hot, c_ejected, c_metals_hot, c_metals_ejected;
    /* the forming galaxy after the call (hot, ejected and their metals unchanged) */
    float x_cold, x_stellar, x_metals_cold, x_metals_stellar;
    float x_sfr, x_outflow, x_new_stars;
    /* the central after the call */
    float x_c_hot, x_c_ejected, x_c_metals_hot, x_c_metals_ejected;
  } rows[] = {
      /* stars 1.0 lock 0.75 at Z 0.125 (1.25 / 10); SFR 1.0 / 0.5 */
      {.name = "star formation",
       .recycle = 0.25,
       .dt = 0.5,
       .cold = 10.0f,
       .hot = 50.0f,
       .stellar = 20.0f,
       .metals_cold = 1.25f,
       .metals_stellar = 0.25f,
       .stars = 1.0,
       .x_cold = 9.25f,
       .x_stellar = 20.75f,
       .x_metals_cold = 1.15625f,
       .x_metals_stellar = 0.34375f,
       .x_sfr = 2.0f,
       .x_new_stars = 1.0f},
      /* stars as above; reheat 1.0 at the post-star-formation Z 0.125 into the central's hot gas
       * (7 -> 8, metals 1.75 + 0.125 = 1.875, Z 0.234375, not the pre-reheating 0.25), then eject
       * 0.5 at that Z: 0.1171875 of metals; the outflow rate is 1.0 / 0.5 */
      {.name = "reheat then eject",
       .recycle = 0.25,
       .dt = 0.5,
       .cold = 5.0f,
       .metals_cold = 0.625f,
       .stars = 1.0,
       .reheated = 1.0,
       .ejected_mass = 0.5,
       .c_hot = 7.0f,
       .c_ejected = 1.0f,
       .c_metals_hot = 1.75f,
       .c_metals_ejected = 0.125f,
       .x_cold = 3.25f,
       .x_stellar = 0.75f,
       .x_metals_cold = 0.40625f,
       .x_metals_stellar = 0.09375f,
       .x_sfr = 2.0f,
       .x_outflow = 2.0f,
       .x_new_stars = 1.0f,
       .x_c_hot = 7.5f,
       .x_c_ejected = 1.5f,
       .x_c_metals_hot = 1.7578125f,
       .x_c_metals_ejected = 0.2421875f},
      /* the metals credited at the metallicity of the floats left after the stars (see above),
       * and a seeded outflow rate that accumulates: 0.3f + 0.5 / 0.5 */
      {.name = "metals after star formation",
       .recycle = 0.25,
       .dt = 0.5,
       .cold = 2.0f,
       .metals_cold = 0.1f,
       .outflow = 0.3f,
       .stars = 0.5,
       .reheated = 0.5,
       .x_cold = 1.125f,
       .x_stellar = 0.375f,
       .x_metals_cold = 0.0562500022f,
       .x_metals_stellar = 0.0187500007f,
       .x_sfr = 1.0f,
       .x_outflow = 1.29999995f,
       .x_new_stars = 0.5f,
       .x_c_hot = 0.5f,
       .x_c_metals_hot = 0.0250000022f},
      /* the central's hot gas 0.5 plus the reheated 0.5 is 1.0, which caps the 5.0 of ejection
       * (capped before the reheating it would be 0.5); all its metals go with it */
      {.name = "ejection capped after reheating",
       .recycle = 0.25,
       .dt = 0.5,
       .cold = 4.0f,
       .metals_cold = 0.5f,
       .stars = 0.5,
       .reheated = 0.5,
       .ejected_mass = 5.0,
       .c_hot = 0.5f,
       .c_metals_hot = 0.0625f,
       .x_cold = 3.125f,
       .x_stellar = 0.375f,
       .x_metals_cold = 0.390625f,
       .x_metals_stellar = 0.046875f,
       .x_sfr = 1.0f,
       .x_outflow = 1.0f,
       .x_new_stars = 0.5f,
       .x_c_ejected = 1.0f,
       .x_c_metals_ejected = 0.125f},
      /* no stars: the reheated and ejected masses are discarded, nothing moves */
      {.name = "no stars",
       .recycle = 0.25,
       .dt = 0.5,
       .cold = 10.0f,
       .metals_cold = 1.25f,
       .reheated = 0.5,
       .ejected_mass = 0.3,
       .c_hot = 3.0f,
       .c_metals_hot = 0.25f,
       .x_cold = 10.0f,
       .x_metals_cold = 1.25f,
       .x_c_hot = 3.0f,
       .x_c_metals_hot = 0.25f},
      /* no cold gas: the cap is zero (not a negative or the stars), so this is a no-stars substep
       */
      {.name = "stars capped at no cold gas",
       .recycle = 0.25,
       .dt = 0.5,
       .stars = 1.0,
       .reheated = 0.5,
       .ejected_mass = 0.3,
       .c_hot = 3.0f,
       .c_metals_hot = 0.25f,
       .x_c_hot = 3.0f,
       .x_c_metals_hot = 0.25f},
      /* a zero interval: the stars and the reheated gas still move, but the rates (stars / 0 and
       * reheated / 0) are not accumulated */
      {.name = "zero interval",
       .recycle = 0.25,
       .dt = 0.0,
       .cold = 10.0f,
       .metals_cold = 1.25f,
       .stars = 1.0,
       .reheated = 0.5,
       .x_cold = 8.75f,
       .x_stellar = 0.75f,
       .x_metals_cold = 1.09375f,
       .x_metals_stellar = 0.09375f,
       .x_new_stars = 1.0f,
       .x_c_hot = 0.5f,
       .x_c_metals_hot = 0.0625f},
  };

  int ok = 1;
  init_memory_system(0);
  for (size_t r = 0; r < sizeof(rows) / sizeof(rows[0]); r++) {
    const char *name = rows[r].name;
    reset_config();
    setup_test_parameters(rows[r].recycle);
    TEST_ASSERT(sage_apply_star_formation_supernova_init() == 0, "init should succeed");

    struct Halo central_halo, halo;
    struct GalaxyData central, gal;
    setup_test_halo(&central_halo, &central, 0, 100.0, rows[r].dt, 0.0, rows[r].c_hot,
                    rows[r].c_ejected, 0.0, 0.0, rows[r].c_metals_hot, rows[r].c_metals_ejected,
                    0.0, 0.0, 0.0, 0.0);
    setup_test_halo(&halo, &gal, 0, 100.0, rows[r].dt, rows[r].cold, rows[r].hot, rows[r].ejected,
                    rows[r].stellar, rows[r].metals_cold, rows[r].metals_hot,
                    rows[r].metals_ejected, rows[r].metals_stellar, rows[r].stars, rows[r].reheated,
                    rows[r].ejected_mass);
    gal.SupernovaOutflowRate = rows[r].outflow;
    struct ModuleContext ctx;
    setup_module_context(&ctx, &central_halo, rows[r].dt);

    const double baryons_before = (double)gal.ColdGas + gal.HotGas + gal.EjectedGas +
                                  gal.StellarMass + central.ColdGas + central.HotGas +
                                  central.EjectedGas + central.StellarMass;
    const double metals_before = (double)gal.MetalsColdGas + gal.MetalsHotGas +
                                 gal.MetalsEjectedGas + gal.MetalsStellarMass +
                                 central.MetalsColdGas + central.MetalsHotGas +
                                 central.MetalsEjectedGas + central.MetalsStellarMass;
    const float hot = gal.HotGas, ejected = gal.EjectedGas, metals_hot = gal.MetalsHotGas,
                metals_ejected = gal.MetalsEjectedGas;

    const int status = sage_apply_star_formation_supernova_process(&ctx, &halo, 1);
    if (status != 0) {
      printf("\n  row %s: process returned %d", name, status);
      ok = 0;
    }

    /* the forming galaxy */
    ok &= expect_float(name, "ColdGas", gal.ColdGas, rows[r].x_cold);
    ok &= expect_float(name, "StellarMass", gal.StellarMass, rows[r].x_stellar);
    ok &= expect_float(name, "MetalsColdGas", gal.MetalsColdGas, rows[r].x_metals_cold);
    ok &= expect_float(name, "MetalsStellarMass", gal.MetalsStellarMass, rows[r].x_metals_stellar);
    ok &= expect_float(name, "StarFormationRate", gal.StarFormationRate, rows[r].x_sfr);
    ok &= expect_float(name, "SupernovaOutflowRate", gal.SupernovaOutflowRate, rows[r].x_outflow);
    ok &= expect_float(name, "HotGas (own pair untouched)", gal.HotGas, hot);
    ok &= expect_float(name, "EjectedGas (own pair untouched)", gal.EjectedGas, ejected);
    ok &= expect_float(name, "MetalsHotGas (own pair untouched)", gal.MetalsHotGas, metals_hot);
    ok &= expect_float(name, "MetalsEjectedGas (own pair untouched)", gal.MetalsEjectedGas,
                       metals_ejected);
    /* the transport fields: stars kept for the enrichment module, the feedback consumed */
    ok &= expect_float(name, "NewStellarMass (kept)", gal.NewStellarMass, rows[r].x_new_stars);
    ok &= expect_float(name, "SupernovaReheatedMass (zeroed)", gal.SupernovaReheatedMass, 0.0f);
    ok &= expect_float(name, "SupernovaEjectedMass (zeroed)", gal.SupernovaEjectedMass, 0.0f);

    /* the central, whose cold gas, stars and own transport fields are not the destination */
    ok &= expect_float(name, "central HotGas", central.HotGas, rows[r].x_c_hot);
    ok &= expect_float(name, "central EjectedGas", central.EjectedGas, rows[r].x_c_ejected);
    ok &= expect_float(name, "central MetalsHotGas", central.MetalsHotGas, rows[r].x_c_metals_hot);
    ok &= expect_float(name, "central MetalsEjectedGas", central.MetalsEjectedGas,
                       rows[r].x_c_metals_ejected);
    ok &= expect_float(name, "central ColdGas", central.ColdGas, 0.0f);
    ok &= expect_float(name, "central StellarMass", central.StellarMass, 0.0f);

    /* the baryons and the metals only move between reservoirs */
    const double baryons_after = (double)gal.ColdGas + gal.HotGas + gal.EjectedGas +
                                 gal.StellarMass + central.ColdGas + central.HotGas +
                                 central.EjectedGas + central.StellarMass;
    const double metals_after = (double)gal.MetalsColdGas + gal.MetalsHotGas +
                                gal.MetalsEjectedGas + gal.MetalsStellarMass +
                                central.MetalsColdGas + central.MetalsHotGas +
                                central.MetalsEjectedGas + central.MetalsStellarMass;
    if (fabs(baryons_after - baryons_before) > 1e-5 || fabs(metals_after - metals_before) > 1e-5) {
      printf("\n  row %s: baryons %.9g -> %.9g, metals %.9g -> %.9g", name, baryons_before,
             baryons_after, metals_before, metals_after);
      ok = 0;
    }
    sage_apply_star_formation_supernova_cleanup();
  }
  check_memory_leaks();
  TEST_ASSERT(ok, "every substep must commit exactly its row's literals");
  return TEST_PASS;
}

/**
 * @brief   Main test runner
 *
 * Executes all sage_apply_star_formation_supernova unit tests and reports results.
 */
int main(void) {
  printf("%s", BLUE);
  printf("============================================================\n");
  printf("Test Suite: sage_apply_star_formation_supernova Module\n");
  printf("============================================================\n");
  printf("%s\n", NC);

  /* Initialize error handling for tests */
  initialize_error_handling(LOG_LEVEL_DEBUG, NULL);

  /* Module lifecycle */
  TEST_RUN(test_module_initialization);

  /* The cold-gas cap without the supernova calculator */
  TEST_RUN(test_star_formation_capped_at_cold_gas_without_calculator);

  /* The commits */
  TEST_RUN(test_commit_table);

  /* Print summary and return result */
  TEST_SUMMARY();
  return TEST_RESULT();
}
