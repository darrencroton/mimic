/**
 * @file    test_unit_sage26_satellite_stripping.c
 * @brief   Unit tests for sage26_satellite_stripping module physics
 *
 * Tests the satellite stripping physics calculation in isolation using minimal mocks.
 *
 * SAGE parity: the module runs process_by_galaxy (one satellite at a time, ngal=1),
 * as SAGE26 strips each Type 1 satellite in its galaxy loop
 * (SAGE26 src/core_build_model.c:493-498), stripping it and depositing into the FOF central via
 * ctx->central_galaxy. Tests therefore call process with the single satellite halo
 * (&halos[1], 1) and the central supplied through ctx->central_galaxy.
 *
 * Validates:
 *   - Stripping calculation logic
 *   - Mass conservation, and SAGE26's asymmetric metal transfer
 *   - Metallicity preservation
 *   - Edge cases (zero gas, boundary conditions, type filtering)
 *   - Substep distribution
 *   - The num_substeps-dependent stripped-fraction formula (inherited SAGE behavior,
 *     see test_stripped_fraction_follows_geometric_formula)
 *   - c10 (platform independent): with CGMrecipeOn 1 the satellite's regime selects the source
 *     and the FoF central's the destination, all four combinations with the gas cap binding and
 *     not, against CGMrecipeOn 0 on the same input (test_cgm_stripping_source_and_destination);
 *     CGMrecipeOn missing or not 0/1 refused, naming it (test_cgm_recipe_on_refused)
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../../../tests/framework/test_framework.h"

#include "include/types.h"
#include "core/module_interface.h"
#include "include/globals.h"
#include "include/constants.h"
#include "util/memory.h"
#include "util/error.h"

extern int sage26_satellite_stripping_init(void);
extern int sage26_satellite_stripping_process(struct ModuleContext *ctx, struct Halo *halos,
                                              int ngal);
extern int sage26_satellite_stripping_cleanup(void);

#define FLOAT_EQ(a, b, epsilon) (fabs((a) - (b)) < (epsilon))

/* Test statistics (required for TEST_RUN macro) */
static int passed = 0;
static int failed = 0;

static void setup_mock_config(void) {
  memset(&MimicConfig, 0, sizeof(MimicConfig));
  MimicConfig.Omega = 0.25;
  MimicConfig.OmegaLambda = 0.75;
  MimicConfig.Hubble_h = 0.73;
}

static struct ModuleContext create_test_context(int num_substeps) {
  struct ModuleContext ctx;
  memset(&ctx, 0, sizeof(ctx));
  ctx.redshift = 0.0;
  ctx.time = 13.6;
  ctx.snapshot_number = 63;
  ctx.substep_number = 0;
  ctx.num_substeps = num_substeps;
  ctx.time_interval = 0.1;
  ctx.substep_dt = ctx.time_interval / num_substeps;
  ctx.params = &MimicConfig;
  return ctx;
}

static struct Halo create_test_halo(int type, double mvir, struct GalaxyData *galaxy) {
  struct Halo halo;
  memset(&halo, 0, sizeof(halo));
  halo.Type = type;
  halo.Mvir = mvir;
  halo.SnapNum = 63;
  halo.galaxy = galaxy;
  return halo;
}

static struct GalaxyData create_test_galaxy(float hot_gas, float metals_hot, float stellar_mass,
                                            float cold_gas) {
  struct GalaxyData gal;
  memset(&gal, 0, sizeof(gal));
  gal.HotGas = hot_gas;
  gal.MetalsHotGas = metals_hot;
  gal.StellarMass = stellar_mass;
  gal.ColdGas = cold_gas;
  gal.HaloBaryonFraction = 0.17;
  return gal;
}

/**
 * @test    test_no_stripping_when_below_threshold
 * @brief   Satellites below baryon fraction threshold should not be stripped
 */
static int test_no_stripping_when_below_threshold(void) {
  init_memory_system(0);
  struct ModuleContext ctx = create_test_context(1);
  struct GalaxyData cen_gal = create_test_galaxy(100.0, 2.0, 50.0, 20.0);
  struct Halo central = create_test_halo(0, 100.0, &cen_gal);
  /* Mvir=10, baryon_frac=0.17 → allowed=1.7, actual=1.0 → excess=-0.7 (no strip) */
  struct GalaxyData sat_gal = create_test_galaxy(1.0, 0.02, 0.0, 0.0);
  struct Halo satellite = create_test_halo(1, 10.0, &sat_gal);
  ctx.central_galaxy = &central;
  struct Halo halos[2] = {central, satellite};
  float initial_sat_hot = sat_gal.HotGas;
  float initial_cen_hot = cen_gal.HotGas;

  int result = sage26_satellite_stripping_process(&ctx, &halos[1], 1);

  TEST_ASSERT(result == 0, "Process should succeed");
  TEST_ASSERT(FLOAT_EQ(halos[1].galaxy->HotGas, initial_sat_hot, 1e-6),
              "Satellite HotGas should be unchanged (below threshold)");
  TEST_ASSERT(FLOAT_EQ(halos[0].galaxy->HotGas, initial_cen_hot, 1e-6),
              "Central HotGas should be unchanged");
  check_memory_leaks();
  return TEST_PASS;
}

/**
 * @test    test_stripping_when_above_threshold
 * @brief   Satellites above baryon fraction threshold should be stripped
 */
static int test_stripping_when_above_threshold(void) {
  init_memory_system(0);
  struct ModuleContext ctx = create_test_context(10);
  struct GalaxyData cen_gal = create_test_galaxy(100.0, 2.0, 50.0, 20.0);
  struct Halo central = create_test_halo(0, 100.0, &cen_gal);
  /* Mvir=10, baryon_frac=0.17 → allowed=1.7; actual=5.0 → excess=3.3; per substep=0.33 */
  struct GalaxyData sat_gal = create_test_galaxy(5.0, 0.1, 0.0, 0.0);
  struct Halo satellite = create_test_halo(1, 10.0, &sat_gal);
  ctx.central_galaxy = &central;
  struct Halo halos[2] = {central, satellite};
  float initial_sat_hot = sat_gal.HotGas;
  float initial_cen_hot = cen_gal.HotGas;

  int result = sage26_satellite_stripping_process(&ctx, &halos[1], 1);

  double baryon_frac = 0.17;
  double total_baryons = 5.0;
  double allowed = baryon_frac * 10.0;
  double excess = total_baryons - allowed;
  double stripped_per_substep = excess / 10.0;

  TEST_ASSERT(result == 0, "Process should succeed");
  TEST_ASSERT(halos[1].galaxy->HotGas < initial_sat_hot, "Satellite HotGas should decrease");
  TEST_ASSERT(FLOAT_EQ(halos[1].galaxy->HotGas, initial_sat_hot - stripped_per_substep, 0.01),
              "Satellite should lose correct amount");
  TEST_ASSERT(halos[0].galaxy->HotGas > initial_cen_hot, "Central HotGas should increase");
  check_memory_leaks();
  return TEST_PASS;
}

/**
 * @test    test_mass_conservation
 * @brief   Stripped mass from satellite equals mass gained by central
 */
static int test_mass_conservation(void) {
  init_memory_system(0);
  struct ModuleContext ctx = create_test_context(1);
  struct GalaxyData cen_gal = create_test_galaxy(100.0, 2.0, 50.0, 20.0);
  struct Halo central = create_test_halo(0, 100.0, &cen_gal);
  struct GalaxyData sat_gal = create_test_galaxy(10.0, 0.2, 0.0, 0.0);
  struct Halo satellite = create_test_halo(1, 10.0, &sat_gal);
  ctx.central_galaxy = &central;
  struct Halo halos[2] = {central, satellite};
  float initial_sat_hot = sat_gal.HotGas;
  float initial_cen_hot = cen_gal.HotGas;
  float initial_total = initial_sat_hot + initial_cen_hot;

  sage26_satellite_stripping_process(&ctx, &halos[1], 1);

  float final_total = halos[1].galaxy->HotGas + halos[0].galaxy->HotGas;
  float sat_lost = initial_sat_hot - halos[1].galaxy->HotGas;
  float cen_gained = halos[0].galaxy->HotGas - initial_cen_hot;

  TEST_ASSERT(FLOAT_EQ(final_total, initial_total, 1e-4), "Total mass should be conserved");
  TEST_ASSERT(FLOAT_EQ(sat_lost, cen_gained, 1e-4),
              "Satellite mass lost should equal central mass gained");
  check_memory_leaks();
  return TEST_PASS;
}

/**
 * @test    test_mass_conservation_max_dynamic_substeps
 * @brief   Repeated stripping at the dynamic substep cap conserves mass
 */
static int test_mass_conservation_max_dynamic_substeps(void) {
  init_memory_system(0);
  struct ModuleContext ctx = create_test_context(DEFAULT_MAX_DYNAMIC_SUBSTEPS);
  struct GalaxyData cen_gal = create_test_galaxy(100.0, 2.0, 50.0, 20.0);
  struct Halo central = create_test_halo(0, 100.0, &cen_gal);
  struct GalaxyData sat_gal = create_test_galaxy(10.0, 0.2, 0.0, 0.0);
  struct Halo satellite = create_test_halo(1, 10.0, &sat_gal);
  ctx.central_galaxy = &central;
  struct Halo halos[2] = {central, satellite};
  const double initial_sat_hot = sat_gal.HotGas;
  const double initial_cen_hot = cen_gal.HotGas;
  const double initial_total = initial_sat_hot + initial_cen_hot;

  for (int i = 0; i < DEFAULT_MAX_DYNAMIC_SUBSTEPS; i++) {
    ctx.substep_number = i;
    int result = sage26_satellite_stripping_process(&ctx, &halos[1], 1);
    TEST_ASSERT(result == 0, "Process should succeed for each dynamic substep");
  }

  const double final_total = (double)halos[1].galaxy->HotGas + (double)halos[0].galaxy->HotGas;
  const double sat_lost = initial_sat_hot - (double)halos[1].galaxy->HotGas;
  const double cen_gained = (double)halos[0].galaxy->HotGas - initial_cen_hot;
  const double mass_tol = fmax(1e-4, fabs(initial_total) * 2e-6);
  const double transfer_tol = fmax(1e-4, fabs(initial_sat_hot) * 2e-5);

  TEST_ASSERT_DOUBLE_EQUAL(final_total, initial_total, mass_tol,
                           "Total mass should be conserved at max dynamic substeps");
  TEST_ASSERT_DOUBLE_EQUAL(sat_lost, cen_gained, transfer_tol,
                           "Satellite mass lost should equal central mass gained");
  check_memory_leaks();
  return TEST_PASS;
}

/**
 * @test    test_stripped_fraction_follows_geometric_formula
 * @brief   Pins the inherited (not Mimic-introduced) 1-(1-1/N)^N stripped-fraction formula
 *
 * SAGE26 divides the recomputed excess by the substep count every substep
 * (SAGE26 src/model_infall.c:238-244), which gives this formula; it is expected, not a bug.
 * Guards against a future refactor silently changing it.
 */
static int test_stripped_fraction_follows_geometric_formula(void) {
  init_memory_system(0);

  /* N=1: strips the entire excess in a single step. */
  {
    struct ModuleContext ctx = create_test_context(1);
    struct GalaxyData cen_gal = create_test_galaxy(100.0, 2.0, 50.0, 20.0);
    struct Halo central = create_test_halo(0, 100.0, &cen_gal);
    /* Mvir=10, baryon_frac=0.17 -> allowed=1.7; HotGas=5.0 -> excess=3.3 */
    struct GalaxyData sat_gal = create_test_galaxy(5.0, 0.0, 0.0, 0.0);
    struct Halo satellite = create_test_halo(1, 10.0, &sat_gal);
    ctx.central_galaxy = &central;
    struct Halo halos[2] = {central, satellite};

    int result = sage26_satellite_stripping_process(&ctx, &halos[1], 1);
    TEST_ASSERT(result == 0, "Process should succeed at N=1");
    TEST_ASSERT_DOUBLE_EQUAL((double)halos[1].galaxy->HotGas, 1.7, 1e-4,
                             "N=1 should strip the full excess down to the baryon-fraction target");
  }

  /* N=8: excess decays by a factor (1-1/8) each substep, so after 8 substeps
   * HotGas = allowed + excess0*(1-1/8)^8 = 1.7 + 3.3*0.343609... = 2.833909... */
  {
    struct ModuleContext ctx = create_test_context(8);
    struct GalaxyData cen_gal = create_test_galaxy(100.0, 2.0, 50.0, 20.0);
    struct Halo central = create_test_halo(0, 100.0, &cen_gal);
    struct GalaxyData sat_gal = create_test_galaxy(5.0, 0.0, 0.0, 0.0);
    struct Halo satellite = create_test_halo(1, 10.0, &sat_gal);
    ctx.central_galaxy = &central;
    struct Halo halos[2] = {central, satellite};

    for (int i = 0; i < 8; i++) {
      ctx.substep_number = i;
      int result = sage26_satellite_stripping_process(&ctx, &halos[1], 1);
      TEST_ASSERT(result == 0, "Process should succeed for each of 8 substeps");
    }

    TEST_ASSERT_DOUBLE_EQUAL((double)halos[1].galaxy->HotGas, 2.833909422159195, 5e-4,
                             "N=8 should match the geometric-decay prediction, not full stripping");
  }

  check_memory_leaks();
  return TEST_PASS;
}

/**
 * @test    test_metal_conservation
 * @brief   Stripped metals from satellite equals metals gained by central
 */
static int test_metal_conservation(void) {
  init_memory_system(0);
  struct ModuleContext ctx = create_test_context(1);
  struct GalaxyData cen_gal = create_test_galaxy(100.0, 2.0, 50.0, 20.0);
  struct Halo central = create_test_halo(0, 100.0, &cen_gal);
  struct GalaxyData sat_gal = create_test_galaxy(10.0, 0.2, 0.0, 0.0);
  struct Halo satellite = create_test_halo(1, 10.0, &sat_gal);
  ctx.central_galaxy = &central;
  struct Halo halos[2] = {central, satellite};
  float initial_sat_metals = sat_gal.MetalsHotGas;
  float initial_cen_metals = cen_gal.MetalsHotGas;
  float initial_total_metals = initial_sat_metals + initial_cen_metals;

  sage26_satellite_stripping_process(&ctx, &halos[1], 1);

  float final_total_metals = halos[1].galaxy->MetalsHotGas + halos[0].galaxy->MetalsHotGas;
  float sat_metals_lost = initial_sat_metals - halos[1].galaxy->MetalsHotGas;
  float cen_metals_gained = halos[0].galaxy->MetalsHotGas - initial_cen_metals;

  TEST_ASSERT(FLOAT_EQ(final_total_metals, initial_total_metals, 1e-5),
              "Total metals should be conserved");
  TEST_ASSERT(FLOAT_EQ(sat_metals_lost, cen_metals_gained, 1e-5),
              "Satellite metals lost should equal central metals gained");
  check_memory_leaks();
  return TEST_PASS;
}

/**
 * @test    test_metal_transfer_all_regimes
 * @brief   SAGE26's asymmetric stripping metal transfer in every clamp regime,
 *          including constructed unphysical M > G states where the metallicity
 *          cap (Z = 1) is active.
 *
 * SAGE26 (model_infall.c:256-270) debits the satellite min(s * Z, M), the
 * unclamped-gas metals capped at its own metals, but credits the central the
 * clamped gas times the original Z. Metals are therefore conserved in the two
 * normal-Z rows and destroyed in the two capped-Z rows, where the gas clamp
 * engages with Z = 1. The package mirrors this source quirk exactly.
 */
static int test_metal_transfer_all_regimes(void) {
  /* Each row drives a distinct branch of the stripping transfer. With
   * num_substeps = 1 and only HotGas + StellarMass carrying baryons, the
   * unclamped strip demand is s = HotGas + StellarMass - bf*Mvir. G = HotGas,
   * M = MetalsHotGas. The last two rows construct M >= G so Z is capped at 1. */
  struct {
    const char *label;
    float hot;
    float metals;
    float stellar;
    double bf;
    double mvir;
    double sat_lost;   /* min(s * Z, M) */
    double cen_gained; /* min(s, G) * Z */
  } cases[] = {
      {"no-clamp", 10.0f, 0.2f, 0.0f, 0.5, 10.0, 0.1, 0.1},
      {"gas-clamp normal-Z", 10.0f, 0.2f, 20.0f, 0.5, 20.0, 0.2, 0.2},
      {"cap both-clamps", 2.0f, 3.0f, 10.0f, 0.2, 10.0, 3.0, 2.0},
      {"cap gas-clamp-only", 2.0f, 10.0f, 5.0f, 0.2, 10.0, 5.0, 2.0},
  };

  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    init_memory_system(0);
    struct ModuleContext ctx = create_test_context(1);
    struct GalaxyData cen_gal = create_test_galaxy(100.0f, 2.0f, 50.0f, 20.0f);
    struct Halo central = create_test_halo(0, 100.0, &cen_gal);
    struct GalaxyData sat_gal =
        create_test_galaxy(cases[i].hot, cases[i].metals, cases[i].stellar, 0.0f);
    sat_gal.HaloBaryonFraction = cases[i].bf;
    struct Halo satellite = create_test_halo(1, cases[i].mvir, &sat_gal);
    ctx.central_galaxy = &central;
    struct Halo halos[2] = {central, satellite};
    float sat_metals_before = sat_gal.MetalsHotGas;
    float cen_metals_before = cen_gal.MetalsHotGas;

    int result = sage26_satellite_stripping_process(&ctx, &halos[1], 1);
    TEST_ASSERT(result == 0, "Process should succeed");

    float sat_lost = sat_metals_before - halos[1].galaxy->MetalsHotGas;
    float cen_gained = halos[0].galaxy->MetalsHotGas - cen_metals_before;

    if (!FLOAT_EQ(sat_lost, cases[i].sat_lost, 1e-4) ||
        !FLOAT_EQ(cen_gained, cases[i].cen_gained, 1e-4)) {
      printf("FAIL: regime '%s' lost=%.6f (expected %.6f), gained=%.6f (expected %.6f)\n",
             cases[i].label, sat_lost, cases[i].sat_lost, cen_gained, cases[i].cen_gained);
      return TEST_FAIL;
    }
  }

  check_memory_leaks();
  return TEST_PASS;
}

/**
 * @test    test_metallicity_preservation
 * @brief   Metallicity should be preserved during stripping
 */
static int test_metallicity_preservation(void) {
  init_memory_system(0);
  struct ModuleContext ctx = create_test_context(1);
  struct GalaxyData cen_gal = create_test_galaxy(100.0, 1.0, 50.0, 20.0);
  struct Halo central = create_test_halo(0, 100.0, &cen_gal);
  struct GalaxyData sat_gal = create_test_galaxy(10.0, 0.3, 0.0, 0.0);
  struct Halo satellite = create_test_halo(1, 10.0, &sat_gal);
  float initial_sat_Z = sat_gal.MetalsHotGas / sat_gal.HotGas;
  ctx.central_galaxy = &central;
  struct Halo halos[2] = {central, satellite};

  sage26_satellite_stripping_process(&ctx, &halos[1], 1);

  if (halos[1].galaxy->HotGas > 0.0) {
    float final_sat_Z = halos[1].galaxy->MetalsHotGas / halos[1].galaxy->HotGas;
    TEST_ASSERT(FLOAT_EQ(final_sat_Z, initial_sat_Z, 1e-4),
                "Satellite metallicity should be preserved");
  }

  check_memory_leaks();
  return TEST_PASS;
}

/**
 * @test    test_zero_hot_gas_no_stripping
 * @brief   Satellites with zero hot gas lose nothing
 *
 * SAGE26 strips every live Type 1 satellite, with no HotGas guard
 * (core_build_model.c:493-498), but the gas clamp at zero hot gas and the zero
 * metallicity it gives move no gas or metals (model_infall.c:256-270).
 */
static int test_zero_hot_gas_no_stripping(void) {
  init_memory_system(0);
  struct ModuleContext ctx = create_test_context(1);
  struct GalaxyData cen_gal = create_test_galaxy(100.0, 2.0, 50.0, 20.0);
  struct Halo central = create_test_halo(0, 100.0, &cen_gal);
  struct GalaxyData sat_gal = create_test_galaxy(0.0, 0.0, 5.0, 2.0);
  struct Halo satellite = create_test_halo(1, 10.0, &sat_gal);
  ctx.central_galaxy = &central;
  struct Halo halos[2] = {central, satellite};
  float initial_cen_hot = cen_gal.HotGas;

  sage26_satellite_stripping_process(&ctx, &halos[1], 1);

  TEST_ASSERT(FLOAT_EQ(halos[1].galaxy->HotGas, 0.0, 1e-6), "Satellite HotGas should remain zero");
  TEST_ASSERT(FLOAT_EQ(halos[0].galaxy->HotGas, initial_cen_hot, 1e-6),
              "Central should not gain gas (satellite has none to strip)");
  check_memory_leaks();
  return TEST_PASS;
}

/**
 * @test    test_clamping_to_available_gas
 * @brief   Cannot strip more gas than satellite has available
 */
static int test_clamping_to_available_gas(void) {
  init_memory_system(0);
  struct ModuleContext ctx = create_test_context(1);
  struct GalaxyData cen_gal = create_test_galaxy(100.0, 2.0, 50.0, 20.0);
  struct Halo central = create_test_halo(0, 100.0, &cen_gal);
  /* Mvir=100 → allowed=17; stellar=50 + hot=2 = 52 → excess=35; strip clamped to 2.0 */
  struct GalaxyData sat_gal = create_test_galaxy(2.0, 0.04, 50.0, 0.0);
  struct Halo satellite = create_test_halo(1, 100.0, &sat_gal);
  ctx.central_galaxy = &central;
  struct Halo halos[2] = {central, satellite};
  float initial_sat_hot = sat_gal.HotGas;
  float initial_cen_hot = cen_gal.HotGas;

  sage26_satellite_stripping_process(&ctx, &halos[1], 1);

  TEST_ASSERT(FLOAT_EQ(halos[1].galaxy->HotGas, 0.0, 1e-6),
              "Satellite should be stripped to zero (clamped)");
  TEST_ASSERT(FLOAT_EQ(halos[0].galaxy->HotGas, initial_cen_hot + initial_sat_hot, 1e-4),
              "Central should gain all available satellite hot gas");
  check_memory_leaks();
  return TEST_PASS;
}

/**
 * @test    test_type_2_orphans_skipped
 * @brief   Type 2 orphans should not be processed
 */
static int test_type_2_orphans_skipped(void) {
  init_memory_system(0);
  struct ModuleContext ctx = create_test_context(1);
  struct GalaxyData cen_gal = create_test_galaxy(100.0, 2.0, 50.0, 20.0);
  struct Halo central = create_test_halo(0, 100.0, &cen_gal);
  struct GalaxyData orphan_gal = create_test_galaxy(10.0, 0.2, 0.0, 0.0);
  struct Halo orphan = create_test_halo(2, 10.0, &orphan_gal);
  ctx.central_galaxy = &central;
  struct Halo halos[2] = {central, orphan};
  float initial_orphan_hot = orphan_gal.HotGas;
  float initial_cen_hot = cen_gal.HotGas;

  sage26_satellite_stripping_process(&ctx, &halos[1], 1);

  TEST_ASSERT(FLOAT_EQ(halos[1].galaxy->HotGas, initial_orphan_hot, 1e-6),
              "Type 2 orphan should not be stripped");
  TEST_ASSERT(FLOAT_EQ(halos[0].galaxy->HotGas, initial_cen_hot, 1e-6),
              "Central should not gain gas from orphan");
  check_memory_leaks();
  return TEST_PASS;
}

/**
 * @test    test_type_3_ejected_skipped
 * @brief   Type 3 ejected galaxies should not be processed
 */
static int test_type_3_ejected_skipped(void) {
  init_memory_system(0);
  struct ModuleContext ctx = create_test_context(1);
  struct GalaxyData cen_gal = create_test_galaxy(100.0, 2.0, 50.0, 20.0);
  struct Halo central = create_test_halo(0, 100.0, &cen_gal);
  struct GalaxyData ejected_gal = create_test_galaxy(10.0, 0.2, 0.0, 0.0);
  struct Halo ejected = create_test_halo(3, 10.0, &ejected_gal);
  ctx.central_galaxy = &central;
  struct Halo halos[2] = {central, ejected};
  float initial_ejected_hot = ejected_gal.HotGas;
  float initial_cen_hot = cen_gal.HotGas;

  sage26_satellite_stripping_process(&ctx, &halos[1], 1);

  TEST_ASSERT(FLOAT_EQ(halos[1].galaxy->HotGas, initial_ejected_hot, 1e-6),
              "Type 3 ejected galaxy should not be stripped");
  TEST_ASSERT(FLOAT_EQ(halos[0].galaxy->HotGas, initial_cen_hot, 1e-6),
              "Central should not gain gas from ejected galaxy");
  check_memory_leaks();
  return TEST_PASS;
}

/**
 * @test    test_null_galaxy_handling
 * @brief   NULL galaxy pointers should be handled safely
 */
static int test_null_galaxy_handling(void) {
  init_memory_system(0);
  struct ModuleContext ctx = create_test_context(1);
  struct GalaxyData cen_gal = create_test_galaxy(100.0, 2.0, 50.0, 20.0);
  struct Halo central = create_test_halo(0, 100.0, &cen_gal);
  struct Halo satellite = create_test_halo(1, 10.0, NULL);
  ctx.central_galaxy = &central;
  struct Halo halos[2] = {central, satellite};
  float initial_cen_hot = cen_gal.HotGas;

  int result = sage26_satellite_stripping_process(&ctx, &halos[1], 1);

  TEST_ASSERT(result == 0, "Should handle NULL galaxy gracefully");
  TEST_ASSERT(FLOAT_EQ(halos[0].galaxy->HotGas, initial_cen_hot, 1e-6),
              "Central should not change when satellite has NULL galaxy");
  check_memory_leaks();
  return TEST_PASS;
}

/**
 * @test    test_multiple_satellites
 * @brief   Multiple satellites should all be processed correctly
 */
static int test_multiple_satellites(void) {
  init_memory_system(0);
  struct ModuleContext ctx = create_test_context(1);
  struct GalaxyData cen_gal = create_test_galaxy(100.0, 2.0, 50.0, 20.0);
  struct Halo central = create_test_halo(0, 100.0, &cen_gal);
  struct GalaxyData sat1_gal = create_test_galaxy(10.0, 0.2, 0.0, 0.0);
  struct Halo sat1 = create_test_halo(1, 10.0, &sat1_gal);
  struct GalaxyData sat2_gal = create_test_galaxy(8.0, 0.16, 0.0, 0.0);
  struct Halo sat2 = create_test_halo(1, 10.0, &sat2_gal);
  ctx.central_galaxy = &central;
  struct Halo halos[3] = {central, sat1, sat2};
  float initial_cen_hot = cen_gal.HotGas;
  float initial_sat1_hot = sat1_gal.HotGas;
  float initial_sat2_hot = sat2_gal.HotGas;

  /* by-galaxy contract — the galaxy-major loop calls the module once per satellite */
  sage26_satellite_stripping_process(&ctx, &halos[1], 1);
  sage26_satellite_stripping_process(&ctx, &halos[2], 1);

  TEST_ASSERT(halos[0].galaxy->HotGas > initial_cen_hot,
              "Central should gain gas from both satellites");
  TEST_ASSERT(halos[1].galaxy->HotGas < initial_sat1_hot, "Satellite 1 should lose gas");
  TEST_ASSERT(halos[2].galaxy->HotGas < initial_sat2_hot, "Satellite 2 should lose gas");
  check_memory_leaks();
  return TEST_PASS;
}

// ============================================================================
// MAIN TEST RUNNER
// ============================================================================

// ============================================================================
// CGMrecipeOn 1 AGAINST 0 (c10 Slice 2, platform independent)
// ============================================================================
//
// SAGE26's strip_from_satellite (R = the staged SAGE26 source, R/src/model_infall.c:225-273) with
// CGMrecipeOn 1 takes from the satellite's CGM pair if the satellite is in Regime 0 and gives to
// the central's CGM pair if the central is in Regime 0, each independently (:250-254, :265-271);
// with CGMrecipeOn 0 both are the hot pair. The satellite (Mvir 8, HaloBaryonFraction 0.125, so
// 1.0 allowed) holds 3.5 of baryons: hot 1.0 / 0.125 (Z 1/8) and CGM 0.5 / 0.125 (Z 1/4). Its
// excess 2.5 strips 2.5 / 8 = 0.3125 over 8 substeps (below either source: no cap) or 2.5 in one
// (the gas cap binds at the source gas, the metal cap at the source metals, :259-260); the
// destination metals gain the capped gas times the source metallicity (:267, :270). Every value
// is a short binary fraction, so each expectation is an exact literal.

/** @brief Read a captured log stream into a static buffer (valid until the next call) */
static const char *c10_read_log(FILE *fp) {
  static char buf[8192];
  rewind(fp);
  const size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
  buf[n] = '\0';
  return buf;
}

static void c10_append_param(const char *name, const char *value) {
  const int idx = MimicConfig.NumModelParams;
  snprintf(MimicConfig.ModelParams[idx].param_name, sizeof(MimicConfig.ModelParams[idx].param_name),
           "%s", name);
  snprintf(MimicConfig.ModelParams[idx].value, sizeof(MimicConfig.ModelParams[idx].value), "%s",
           value);
  MimicConfig.NumModelParams = idx + 1;
}

/** @brief Initialise with GlobalBaryonFraction 0.17 and CGMrecipeOn `cgm` (NULL: missing) */
static int c10_init(const char *cgm, const char **log) {
  setup_mock_config();
  c10_append_param("GlobalBaryonFraction", "0.17");
  if (cgm != NULL) {
    c10_append_param("CGMrecipeOn", cgm);
  }
  FILE *capture = tmpfile();
  FILE *old_out = capture != NULL ? set_log_output(capture) : NULL;
  const int status = sage26_satellite_stripping_init();
  if (capture != NULL) {
    set_log_output(old_out);
    *log = c10_read_log(capture);
    fclose(capture);
  } else {
    *log = "";
  }
  return status;
}

/** @brief A galaxy's hot and CGM pairs */
struct c10_pairs {
  float hot, metals_hot, cgm, metals_cgm;
};

static int c10_same_bits(const float a, const float b) { return memcmp(&a, &b, sizeof(a)) == 0; }

static int c10_pairs_equal(const struct c10_pairs *a, const struct c10_pairs *b) {
  return c10_same_bits(a->hot, b->hot) && c10_same_bits(a->metals_hot, b->metals_hot) &&
         c10_same_bits(a->cgm, b->cgm) && c10_same_bits(a->metals_cgm, b->metals_cgm);
}

/** @brief Strip the satellite once at CGMrecipeOn `cgm` with the given regimes and substeps */
static int c10_strip(const char *cgm, const int sat_regime, const int cen_regime,
                     const int substeps, struct c10_pairs *sat_out, struct c10_pairs *cen_out) {
  const char *log = "";
  if (c10_init(cgm, &log) != 0) {
    return -1;
  }
  struct GalaxyData cen_gal, sat_gal;
  memset(&cen_gal, 0, sizeof(cen_gal));
  memset(&sat_gal, 0, sizeof(sat_gal));
  cen_gal.Regime = cen_regime;
  cen_gal.HotGas = 4.0f;
  cen_gal.MetalsHotGas = 0.5f;
  cen_gal.CGMgas = 2.0f;
  cen_gal.MetalsCGMgas = 0.25f;
  sat_gal.Regime = sat_regime;
  sat_gal.HaloBaryonFraction = 0.125;
  sat_gal.StellarMass = 1.0f;
  sat_gal.ColdGas = 0.5f;
  sat_gal.HotGas = 1.0f;
  sat_gal.MetalsHotGas = 0.125f;
  sat_gal.CGMgas = 0.5f;
  sat_gal.MetalsCGMgas = 0.125f;
  sat_gal.EjectedGas = 0.25f;
  sat_gal.ICS = 0.25f;
  struct Halo central = create_test_halo(0, 64.0, &cen_gal);
  struct Halo satellite = create_test_halo(1, 8.0, &sat_gal);
  struct ModuleContext ctx = create_test_context(substeps);
  ctx.central_galaxy = &central;
  const int status = sage26_satellite_stripping_process(&ctx, &satellite, 1);
  *sat_out = (struct c10_pairs){sat_gal.HotGas, sat_gal.MetalsHotGas, sat_gal.CGMgas,
                                sat_gal.MetalsCGMgas};
  *cen_out = (struct c10_pairs){cen_gal.HotGas, cen_gal.MetalsHotGas, cen_gal.CGMgas,
                                cen_gal.MetalsCGMgas};
  return status;
}

/**
 * @brief The expected pairs after one strip from `take_cgm` (else hot) to `give_cgm` (else hot)
 *
 * Literal values: uncapped (8 substeps) 0.3125 of gas with 0.078125 (CGM source, Z 1/4) or
 * 0.0390625 (hot source, Z 1/8) of metals; capped (1 substep) the source gas, 0.5 or 1.0, with the
 * source metals 0.125 debited and the capped gas times Z, 0.125 either way, credited.
 */
static void c10_expected(const int take_cgm, const int give_cgm, const int capped,
                         struct c10_pairs *sat, struct c10_pairs *cen) {
  *sat = (struct c10_pairs){1.0f, 0.125f, 0.5f, 0.125f};
  *cen = (struct c10_pairs){4.0f, 0.5f, 2.0f, 0.25f};
  float gas, credit_metals;
  if (take_cgm) {
    gas = capped ? 0.5f : 0.3125f;
    credit_metals = capped ? 0.125f : 0.078125f;
    sat->cgm = capped ? 0.0f : 0.1875f;
    sat->metals_cgm = capped ? 0.0f : 0.046875f;
  } else {
    gas = capped ? 1.0f : 0.3125f;
    credit_metals = capped ? 0.125f : 0.0390625f;
    sat->hot = capped ? 0.0f : 0.6875f;
    sat->metals_hot = capped ? 0.0f : 0.0859375f;
  }
  if (give_cgm) {
    cen->cgm = 2.0f + gas;
    cen->metals_cgm = 0.25f + credit_metals;
  } else {
    cen->hot = 4.0f + gas;
    cen->metals_hot = 0.5f + credit_metals;
  }
}

/**
 * @test    test_cgm_stripping_source_and_destination
 * @brief   All four source/destination combinations, the gas cap binding and not, at
 *          CGMrecipeOn 1, and the same inputs at CGMrecipeOn 0 (hot to hot); each unselected pair
 *          of both galaxies unchanged
 */
int test_cgm_stripping_source_and_destination(void) {
  int ok = 1;
  for (int take_cgm = 0; take_cgm <= 1; take_cgm++) {
    for (int give_cgm = 0; give_cgm <= 1; give_cgm++) {
      for (int capped = 0; capped <= 1; capped++) {
        const int substeps = capped ? 1 : 8;
        const int sat_regime = take_cgm ? 0 : 1;
        const int cen_regime = give_cgm ? 0 : 1;
        struct c10_pairs sat, cen, want_sat, want_cen;

        if (c10_strip("1", sat_regime, cen_regime, substeps, &sat, &cen) != 0) {
          printf("\n  CGMrecipeOn 1: init or process failed");
          ok = 0;
          continue;
        }
        c10_expected(take_cgm, give_cgm, capped, &want_sat, &want_cen);
        if (!c10_pairs_equal(&sat, &want_sat) || !c10_pairs_equal(&cen, &want_cen)) {
          printf("\n  CGMrecipeOn 1, %s to %s, %s: satellite %.9g/%.9g/%.9g/%.9g central "
                 "%.9g/%.9g/%.9g/%.9g",
                 take_cgm ? "CGM" : "hot", give_cgm ? "CGM" : "hot", capped ? "capped" : "uncapped",
                 (double)sat.hot, (double)sat.metals_hot, (double)sat.cgm, (double)sat.metals_cgm,
                 (double)cen.hot, (double)cen.metals_hot, (double)cen.cgm, (double)cen.metals_cgm);
          ok = 0;
        }

        /* CGMrecipeOn 0, the same input: hot to hot whatever the regimes */
        if (c10_strip("0", sat_regime, cen_regime, substeps, &sat, &cen) != 0) {
          printf("\n  CGMrecipeOn 0: init or process failed");
          ok = 0;
          continue;
        }
        c10_expected(0, 0, capped, &want_sat, &want_cen);
        if (!c10_pairs_equal(&sat, &want_sat) || !c10_pairs_equal(&cen, &want_cen)) {
          printf("\n  CGMrecipeOn 0 (regimes %d, %d), %s: not hot to hot", sat_regime, cen_regime,
                 capped ? "capped" : "uncapped");
          ok = 0;
        }
      }
    }
  }
  TEST_ASSERT(ok, "each combination must strip from and to exactly the selected pairs");
  return TEST_PASS;
}

/**
 * @test    test_cgm_recipe_on_refused
 * @brief   CGMrecipeOn is required (missing key fails init) and only 0 or 1 is accepted, a refusal
 *          naming the parameter (campaign decision D33)
 */
int test_cgm_recipe_on_refused(void) {
  const char *log = "";
  TEST_ASSERT(c10_init(NULL, &log) != 0, "init must fail when CGMrecipeOn is missing");
  TEST_ASSERT(strstr(log, "CGMrecipeOn") != NULL, "the missing-key error must name CGMrecipeOn");
  TEST_ASSERT(c10_init("2", &log) != 0, "init must refuse CGMrecipeOn 2");
  TEST_ASSERT(strstr(log, "CGMrecipeOn") != NULL, "the refusal must name CGMrecipeOn");
  TEST_ASSERT(c10_init("-1", &log) != 0, "init must refuse CGMrecipeOn -1");
  TEST_ASSERT(strstr(log, "CGMrecipeOn") != NULL, "the refusal must name CGMrecipeOn");
  TEST_ASSERT(c10_init("1", &log) == 0, "init must accept CGMrecipeOn 1");
  TEST_ASSERT(c10_init("0", &log) == 0, "init must accept CGMrecipeOn 0");
  setup_mock_config();
  return TEST_PASS;
}

int main(void) {
  initialize_error_handling(LOG_LEVEL_WARNING, NULL);
  setup_mock_config();

  TEST_RUN(test_no_stripping_when_below_threshold);
  TEST_RUN(test_stripping_when_above_threshold);
  TEST_RUN(test_mass_conservation);
  TEST_RUN(test_mass_conservation_max_dynamic_substeps);
  TEST_RUN(test_stripped_fraction_follows_geometric_formula);
  TEST_RUN(test_metal_conservation);
  TEST_RUN(test_metal_transfer_all_regimes);
  TEST_RUN(test_metallicity_preservation);
  TEST_RUN(test_zero_hot_gas_no_stripping);
  TEST_RUN(test_clamping_to_available_gas);
  TEST_RUN(test_type_2_orphans_skipped);
  TEST_RUN(test_type_3_ejected_skipped);
  TEST_RUN(test_null_galaxy_handling);
  TEST_RUN(test_multiple_satellites);

  /* c10: CGMrecipeOn 1 against 0 (platform independent) */
  TEST_RUN(test_cgm_stripping_source_and_destination);
  TEST_RUN(test_cgm_recipe_on_refused);

  TEST_SUMMARY();
  return TEST_RESULT();
}
