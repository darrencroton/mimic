/**
 * @file    test_unit_sage_satellite_stripping.c
 * @brief   Unit tests for sage_satellite_stripping module physics
 *
 * Tests the satellite stripping physics calculation in isolation using minimal mocks.
 *
 * SAGE parity: the module runs process_by_galaxy (one satellite at a time, ngal=1),
 * stripping a Type 1 satellite and depositing into the FOF central via
 * ctx->central_galaxy. Tests therefore call process with the single satellite halo
 * (&halos[1], 1) and the central supplied through ctx->central_galaxy.
 *
 * Test cases:
 *   - test_no_stripping_when_below_threshold: a satellite under its baryon allowance neither
 *     loses nor gives gas or metals
 *   - test_stripped_fraction_follows_geometric_formula: the num_substeps-dependent stripped
 *     fraction (inherited SAGE behaviour) at N = 1, 8, 10 (one substep) and the substep cap,
 *     with the satellite's loss equal to the central's gain
 *   - test_metal_conservation_all_regimes: in every clamp regime, including the Z > 1 capped
 *     ones, the satellite loses and the central gains exactly the gas and metals the strip
 *     demand, the gas clamp and the metallicity cap allow
 *   - test_zero_hot_gas_no_stripping: no gas or metals move, and none turn non-finite, when the
 *     satellite has no hot gas but a positive demand
 *   - test_unstripped_types_skipped: a Type 0, 2 or 3 row is not stripped
 *   - test_null_galaxy_handling: a NULL satellite galaxy, NULL context, NULL central or central
 *     without a galaxy return 0 and move nothing; ngal != 1 is an error
 *   - test_halo_baryon_fraction_fallback: a satellite HaloBaryonFraction that is not positive
 *     (-1 or 0) falls back to GlobalBaryonFraction; a positive one is the satellite's own
 */

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "../../../../tests/framework/test_framework.h"

#include "include/types.h"
#include "core/module_interface.h"
#include "include/globals.h"
#include "include/constants.h"
#include "util/memory.h"
#include "util/error.h"

extern int sage_satellite_stripping_init(void);
extern int sage_satellite_stripping_process(struct ModuleContext *ctx, struct Halo *halos,
                                            int ngal);
extern int sage_satellite_stripping_cleanup(void);

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

  int result = sage_satellite_stripping_process(&ctx, &halos[1], 1);

  TEST_ASSERT(result == 0, "Process should succeed");
  TEST_ASSERT(FLOAT_EQ(halos[1].galaxy->HotGas, initial_sat_hot, 1e-6),
              "Satellite HotGas should be unchanged (below threshold)");
  TEST_ASSERT(FLOAT_EQ(halos[0].galaxy->HotGas, initial_cen_hot, 1e-6),
              "Central HotGas should be unchanged");
  TEST_ASSERT(halos[1].galaxy->MetalsHotGas == 0.02f, "Satellite metals should be unchanged");
  TEST_ASSERT(halos[0].galaxy->MetalsHotGas == 2.0f, "Central metals should be unchanged");
  check_memory_leaks();
  return TEST_PASS;
}

/**
 * @test    test_stripped_fraction_follows_geometric_formula
 * @brief   Pins the inherited (not Mimic-introduced) 1-(1-1/N)^N stripped-fraction formula
 *
 * SAGE divides the recomputed excess by the substep count every substep
 * which gives this formula; it is expected, not a bug.
 * Guards against a future refactor silently changing it.
 *
 * The satellite (Mvir 10, HaloBaryonFraction 0.17, hot gas 5.0) has excess 3.3 over its allowed
 * 1.7. After N substeps of N calls the excess has decayed by (1-1/N)^N; the satellite's loss
 * is the central's gain throughout.
 */
static int test_stripped_fraction_follows_geometric_formula(void) {
  init_memory_system(0);

  static const struct {
    int substeps;
    int calls;
    double sat_hot;
    const char *what;
  } rows[] = {
      /* N=1 strips the entire excess down to the target: 1.7 */
      {1, 1, 1.7, "N=1 strips the full excess"},
      /* N=8: HotGas = 1.7 + 3.3 * (1-1/8)^8 = 2.833909422159195 */
      {8, 8, 2.833909422159195, "N=8 follows the geometric decay"},
      /* one call of N=10 strips a tenth of the excess: 5.0 - 0.33 */
      {10, 1, 4.67, "one substep of N=10 strips a tenth of the excess"},
      /* the dynamic-substep cap: 1.7 + 3.3 * (1-1/200)^200 = 2.9109608116963512 */
      {DEFAULT_MAX_DYNAMIC_SUBSTEPS, DEFAULT_MAX_DYNAMIC_SUBSTEPS, 2.9109608116963512,
       "the substep cap follows the geometric decay"},
  };

  for (size_t k = 0; k < sizeof(rows) / sizeof(rows[0]); k++) {
    struct ModuleContext ctx = create_test_context(rows[k].substeps);
    struct GalaxyData cen_gal = create_test_galaxy(100.0, 2.0, 50.0, 20.0);
    struct Halo central = create_test_halo(0, 100.0, &cen_gal);
    struct GalaxyData sat_gal = create_test_galaxy(5.0, 0.0, 0.0, 0.0);
    struct Halo satellite = create_test_halo(1, 10.0, &sat_gal);
    ctx.central_galaxy = &central;
    char msg[160];

    for (int i = 0; i < rows[k].calls; i++) {
      ctx.substep_number = i;
      TEST_ASSERT(sage_satellite_stripping_process(&ctx, &satellite, 1) == 0,
                  "Process should succeed");
    }

    snprintf(msg, sizeof(msg), "%s (HotGas %.7g)", rows[k].what, (double)sat_gal.HotGas);
    TEST_ASSERT(FLOAT_EQ((double)sat_gal.HotGas, rows[k].sat_hot, 1e-4), msg);
    snprintf(msg, sizeof(msg), "N=%d: the satellite's loss is the central's gain",
             rows[k].substeps);
    TEST_ASSERT(FLOAT_EQ(5.0 - (double)sat_gal.HotGas, (double)cen_gal.HotGas - 100.0, 1e-4), msg);
  }

  check_memory_leaks();
  return TEST_PASS;
}

/**
 * @test    test_metal_conservation_all_regimes
 * @brief   The satellite loses, and the central gains, the same literal gas and metals in
 *          every clamp regime, including constructed unphysical M > G states where the
 *          metallicity cap (Z = 1) is active.
 *
 * The central is credited with exactly the strippedMetals debited from the satellite, so metals
 * are conserved in every regime. Stock SAGE credits the clamped gas times the metallicity,
 * which destroys metals in the two capped-Z rows below; Mimic's sage16 package deliberately
 * conserves them, so the two sides must both equal min(s * Z, M).
 */
static int test_metal_conservation_all_regimes(void) {
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
    double gas_moved;    /* min(s, G) */
    double metals_moved; /* min(s * Z, M) */
  } cases[] = {
      {"no-clamp", 10.0f, 0.2f, 0.0f, 0.5, 10.0, 5.0, 0.1},
      {"gas-clamp normal-Z", 10.0f, 0.2f, 20.0f, 0.5, 20.0, 10.0, 0.2},
      {"cap both-clamps", 2.0f, 3.0f, 10.0f, 0.2, 10.0, 2.0, 3.0},
      {"cap gas-clamp-only", 2.0f, 10.0f, 5.0f, 0.2, 10.0, 2.0, 5.0},
  };

  init_memory_system(0);
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    struct ModuleContext ctx = create_test_context(1);
    struct GalaxyData cen_gal = create_test_galaxy(100.0f, 2.0f, 50.0f, 20.0f);
    struct Halo central = create_test_halo(0, 100.0, &cen_gal);
    struct GalaxyData sat_gal =
        create_test_galaxy(cases[i].hot, cases[i].metals, cases[i].stellar, 0.0f);
    sat_gal.HaloBaryonFraction = cases[i].bf;
    struct Halo satellite = create_test_halo(1, cases[i].mvir, &sat_gal);
    ctx.central_galaxy = &central;
    struct Halo halos[2] = {central, satellite};
    float sat_gas_before = sat_gal.HotGas;
    float sat_metals_before = sat_gal.MetalsHotGas;
    float cen_gas_before = cen_gal.HotGas;
    float cen_metals_before = cen_gal.MetalsHotGas;
    char msg[200];

    int result = sage_satellite_stripping_process(&ctx, &halos[1], 1);
    TEST_ASSERT(result == 0, "Process should succeed");

    const double gas_lost = sat_gas_before - halos[1].galaxy->HotGas;
    const double gas_gained = halos[0].galaxy->HotGas - cen_gas_before;
    const double metals_lost = sat_metals_before - halos[1].galaxy->MetalsHotGas;
    const double metals_gained = halos[0].galaxy->MetalsHotGas - cen_metals_before;

    snprintf(msg, sizeof(msg),
             "regime '%s': gas lost/gained %.6f/%.6f (expected %.6f), metals lost/gained "
             "%.6f/%.6f (expected %.6f)",
             cases[i].label, gas_lost, gas_gained, cases[i].gas_moved, metals_lost, metals_gained,
             cases[i].metals_moved);
    TEST_ASSERT(FLOAT_EQ(gas_lost, cases[i].gas_moved, 1e-4) &&
                    FLOAT_EQ(gas_gained, cases[i].gas_moved, 1e-4) &&
                    FLOAT_EQ(metals_lost, cases[i].metals_moved, 1e-4) &&
                    FLOAT_EQ(metals_gained, cases[i].metals_moved, 1e-4),
                msg);
  }

  check_memory_leaks();
  return TEST_PASS;
}

/**
 * @test    test_zero_hot_gas_no_stripping
 * @brief   Satellites with zero hot gas lose nothing, and nothing turns non-finite
 *
 * The demand here is positive (stellar mass and cold gas exceed the allowance), so a
 * division by the satellite's zero hot gas would show as a non-finite metal value on either
 * galaxy.
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

  sage_satellite_stripping_process(&ctx, &halos[1], 1);

  TEST_ASSERT(FLOAT_EQ(halos[1].galaxy->HotGas, 0.0, 1e-6), "Satellite HotGas should remain zero");
  TEST_ASSERT(FLOAT_EQ(halos[0].galaxy->HotGas, initial_cen_hot, 1e-6),
              "Central should not gain gas (satellite has none to strip)");
  TEST_ASSERT(halos[1].galaxy->MetalsHotGas == 0.0f, "Satellite metals should remain zero");
  TEST_ASSERT(halos[0].galaxy->MetalsHotGas == 2.0f, "Central metals should be unchanged");
  TEST_ASSERT(isfinite(halos[0].galaxy->HotGas) && isfinite(halos[0].galaxy->MetalsHotGas) &&
                  isfinite(halos[1].galaxy->HotGas) && isfinite(halos[1].galaxy->MetalsHotGas),
              "No gas or metal value should turn non-finite");
  check_memory_leaks();
  return TEST_PASS;
}

/**
 * @test    test_unstripped_types_skipped
 * @brief   Only a Type 1 satellite is stripped: a Type 0, 2 (orphan) or 3 (ejected) row is not
 */
static int test_unstripped_types_skipped(void) {
  static const struct {
    int type;
    const char *what;
  } rows[] = {
      {0, "a Type 0 row should not be stripped"},
      {2, "a Type 2 orphan should not be stripped"},
      {3, "a Type 3 ejected galaxy should not be stripped"},
  };

  init_memory_system(0);
  for (size_t k = 0; k < sizeof(rows) / sizeof(rows[0]); k++) {
    struct ModuleContext ctx = create_test_context(1);
    struct GalaxyData cen_gal = create_test_galaxy(100.0, 2.0, 50.0, 20.0);
    struct Halo central = create_test_halo(0, 100.0, &cen_gal);
    /* Mvir=10 allows 1.7: a Type 1 row with 10.0 hot gas would be stripped */
    struct GalaxyData other_gal = create_test_galaxy(10.0, 0.2, 0.0, 0.0);
    struct Halo other = create_test_halo(rows[k].type, 10.0, &other_gal);
    ctx.central_galaxy = &central;

    TEST_ASSERT(sage_satellite_stripping_process(&ctx, &other, 1) == 0, "Process should succeed");

    TEST_ASSERT(other_gal.HotGas == 10.0f && other_gal.MetalsHotGas == 0.2f, rows[k].what);
    TEST_ASSERT(cen_gal.HotGas == 100.0f && cen_gal.MetalsHotGas == 2.0f,
                "The central should not gain gas or metals");
  }

  check_memory_leaks();
  return TEST_PASS;
}

/**
 * @test    test_null_galaxy_handling
 * @brief   Missing state returns 0 and moves nothing; ngal != 1 is an error
 *
 * A Type 1 satellite holding 5.0 hot gas (allowance 1.7) would be stripped if the module did
 * not return early on: a NULL satellite galaxy, a NULL context, a NULL central pointer or a
 * central without a galaxy. process_by_galaxy requires ngal == 1, anything else is an error.
 */
static int test_null_galaxy_handling(void) {
  init_memory_system(0);
  struct ModuleContext ctx = create_test_context(1);
  struct GalaxyData cen_gal = create_test_galaxy(100.0, 2.0, 50.0, 20.0);
  struct Halo central = create_test_halo(0, 100.0, &cen_gal);
  struct Halo central_without_galaxy = create_test_halo(0, 100.0, NULL);
  struct GalaxyData sat_gal = create_test_galaxy(5.0, 0.1, 0.0, 0.0);
  struct Halo satellite = create_test_halo(1, 10.0, &sat_gal);
  struct Halo satellite_without_galaxy = create_test_halo(1, 10.0, NULL);

  ctx.central_galaxy = &central;
  TEST_ASSERT(sage_satellite_stripping_process(&ctx, &satellite_without_galaxy, 1) == 0,
              "Should handle NULL galaxy gracefully");
  TEST_ASSERT(cen_gal.HotGas == 100.0f, "Central should not change when satellite has NULL galaxy");

  TEST_ASSERT(sage_satellite_stripping_process(NULL, &satellite, 1) == 0,
              "Should handle a NULL context");
  ctx.central_galaxy = NULL;
  TEST_ASSERT(sage_satellite_stripping_process(&ctx, &satellite, 1) == 0,
              "Should handle a NULL central");
  ctx.central_galaxy = &central_without_galaxy;
  TEST_ASSERT(sage_satellite_stripping_process(&ctx, &satellite, 1) == 0,
              "Should handle a central without a galaxy");
  TEST_ASSERT(sat_gal.HotGas == 5.0f && sat_gal.MetalsHotGas == 0.1f,
              "Without a central to give to, the satellite should keep its gas");
  TEST_ASSERT(cen_gal.HotGas == 100.0f && cen_gal.MetalsHotGas == 2.0f,
              "The central should be unchanged");

  ctx.central_galaxy = &central;
  struct Halo pair[2] = {satellite, satellite};
  FILE *capture = tmpfile();
  FILE *old_out = capture != NULL ? set_log_output(capture) : NULL;
  const int two = sage_satellite_stripping_process(&ctx, pair, 2);
  const int none = sage_satellite_stripping_process(&ctx, pair, 0);
  if (capture != NULL) {
    set_log_output(old_out);
    fclose(capture);
  }
  TEST_ASSERT(two != 0 && none != 0, "ngal != 1 should be an error");
  TEST_ASSERT(sat_gal.HotGas == 5.0f && cen_gal.HotGas == 100.0f,
              "An ngal != 1 call should move nothing");

  check_memory_leaks();
  return TEST_PASS;
}

/** @brief Append one model parameter to the mock run configuration */
static void append_param(const char *name, const char *value) {
  const int idx = MimicConfig.NumModelParams;
  snprintf(MimicConfig.ModelParams[idx].param_name, sizeof(MimicConfig.ModelParams[idx].param_name),
           "%s", name);
  snprintf(MimicConfig.ModelParams[idx].value, sizeof(MimicConfig.ModelParams[idx].value), "%s",
           value);
  MimicConfig.NumModelParams = idx + 1;
}

/**
 * @test    test_halo_baryon_fraction_fallback
 * @brief   A HaloBaryonFraction that is not positive falls back to GlobalBaryonFraction
 *
 * sage_reionization sets the satellite's own fraction; an unset one (-1) or a zero uses
 * GlobalBaryonFraction, here 0.3. The satellite (Mvir 10, hot gas 5.0, one substep) is stripped
 * to its allowance: 3.0 under the global 0.3 and 1.7 under its own positive 0.17.
 */
static int test_halo_baryon_fraction_fallback(void) {
  static const struct {
    double halo_baryon_fraction;
    double sat_hot;
    const char *what;
  } rows[] = {
      {-1.0, 3.0, "an unset (-1) fraction should fall back to GlobalBaryonFraction"},
      {0.0, 3.0, "a zero fraction should fall back to GlobalBaryonFraction"},
      {0.17, 1.7, "a positive fraction should be the satellite's own"},
  };

  setup_mock_config();
  append_param("GlobalBaryonFraction", "0.3");
  TEST_ASSERT(sage_satellite_stripping_init() == 0, "init should succeed");

  init_memory_system(0);
  for (size_t k = 0; k < sizeof(rows) / sizeof(rows[0]); k++) {
    struct ModuleContext ctx = create_test_context(1);
    struct GalaxyData cen_gal = create_test_galaxy(100.0, 2.0, 50.0, 20.0);
    struct Halo central = create_test_halo(0, 100.0, &cen_gal);
    struct GalaxyData sat_gal = create_test_galaxy(5.0, 0.1, 0.0, 0.0);
    sat_gal.HaloBaryonFraction = rows[k].halo_baryon_fraction;
    struct Halo satellite = create_test_halo(1, 10.0, &sat_gal);
    ctx.central_galaxy = &central;

    TEST_ASSERT(sage_satellite_stripping_process(&ctx, &satellite, 1) == 0,
                "Process should succeed");

    TEST_ASSERT(FLOAT_EQ((double)sat_gal.HotGas, rows[k].sat_hot, 1e-5), rows[k].what);
    TEST_ASSERT(FLOAT_EQ((double)cen_gal.HotGas, 100.0 + (5.0 - rows[k].sat_hot), 1e-5),
                "The central should gain what the satellite lost");
  }

  check_memory_leaks();
  return TEST_PASS;
}

int main(void) {
  initialize_error_handling(LOG_LEVEL_WARNING, NULL);
  setup_mock_config();

  TEST_RUN(test_no_stripping_when_below_threshold);
  TEST_RUN(test_stripped_fraction_follows_geometric_formula);
  TEST_RUN(test_metal_conservation_all_regimes);
  TEST_RUN(test_zero_hot_gas_no_stripping);
  TEST_RUN(test_unstripped_types_skipped);
  TEST_RUN(test_null_galaxy_handling);
  TEST_RUN(test_halo_baryon_fraction_fallback);

  TEST_SUMMARY();
  return TEST_RESULT();
}
