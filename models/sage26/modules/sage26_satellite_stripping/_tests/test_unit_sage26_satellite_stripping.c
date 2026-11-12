/**
 * @file    test_unit_sage26_satellite_stripping.c
 * @brief   Unit tests for sage26_satellite_stripping module physics
 *
 * Tests the satellite stripping physics calculation in isolation using minimal mocks.
 *
 * SAGE parity: the module runs process_by_galaxy (one satellite at a time, ngal=1),
 * as SAGE26 strips each Type 1 satellite in its galaxy loop, stripping it and depositing into
 * the FOF central via ctx->central_galaxy. Tests therefore call process with the single
 * satellite halo (&halos[1], 1) and the central supplied through ctx->central_galaxy.
 *
 * Test cases:
 *   - test_no_stripping_when_below_threshold: a satellite under its baryon allowance neither
 *     loses nor gives gas or metals
 *   - test_stripped_fraction_follows_geometric_formula: the num_substeps-dependent stripped
 *     fraction (inherited SAGE behaviour) at N = 1, 8, 10 (one substep) and the substep cap,
 *     with the satellite's loss equal to the central's gain
 *   - test_metal_transfer_all_regimes: SAGE26's asymmetric metal debit and credit across the
 *     no-clamp, gas-clamp and Z > 1 capped rows
 *   - test_zero_hot_gas_no_stripping: no gas or metals move, and none turn non-finite, when the
 *     satellite has no hot gas but a positive demand
 *   - test_unstripped_types_skipped: a Type 0, 2 or 3 row is not stripped
 *   - test_null_galaxy_handling: a NULL satellite galaxy, NULL context, NULL central or central
 *     without a galaxy return 0 and move nothing; ngal != 1 is an error
 *   - test_cgm_stripping_source_and_destination: CGMrecipeOn 1, the satellite's regime selects
 *     the source and the FoF central's the destination, all four combinations with the gas cap
 *     binding and not, against CGMrecipeOn 0 on the same input; each unselected pair unchanged
 *   - test_cgm_stripping_metals_above_gas: the same four combinations with source metals above
 *     source gas and both caps binding, so the destination credit (capped gas times
 *     metallicity 1) differs from the capped metal debit
 *   - test_halo_baryon_fraction_sentinel: HaloBaryonFraction -1 selects GlobalBaryonFraction;
 *     any other value, zero included, is used as the satellite's own
 *
 * The CGMrecipeOn refusals of the module are asserted, with diagnostics, by
 * test_unit_sage26_parameter_refusals.c.
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

/* Shared sage26 test fixture boilerplate (counters, config reset, log capture) */
#include "modules/_tests/sage_test_fixtures.h"

extern int sage26_satellite_stripping_init(void);
extern int sage26_satellite_stripping_process(struct ModuleContext *ctx, struct Halo *halos,
                                              int ngal);
extern int sage26_satellite_stripping_cleanup(void);

#define FLOAT_EQ(a, b, epsilon) (fabs((a) - (b)) < (epsilon))

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
  TEST_ASSERT(halos[1].galaxy->MetalsHotGas == 0.02f, "Satellite metals should be unchanged");
  TEST_ASSERT(halos[0].galaxy->MetalsHotGas == 2.0f, "Central metals should be unchanged");
  check_memory_leaks();
  return TEST_PASS;
}

/**
 * @test    test_stripped_fraction_follows_geometric_formula
 * @brief   Pins the inherited (not Mimic-introduced) 1-(1-1/N)^N stripped-fraction formula
 *
 * SAGE26 divides the recomputed excess by the substep count every substep
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
      TEST_ASSERT(sage26_satellite_stripping_process(&ctx, &satellite, 1) == 0,
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
 * @test    test_metal_transfer_all_regimes
 * @brief   SAGE26's asymmetric stripping metal transfer in every clamp regime,
 *          including constructed unphysical M > G states where the metallicity
 *          cap (Z = 1) is active.
 *
 * SAGE26 debits the satellite min(s * Z, M), the
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
    float sat_metals_before = sat_gal.MetalsHotGas;
    float cen_metals_before = cen_gal.MetalsHotGas;
    char msg[160];

    int result = sage26_satellite_stripping_process(&ctx, &halos[1], 1);
    TEST_ASSERT(result == 0, "Process should succeed");

    float sat_lost = sat_metals_before - halos[1].galaxy->MetalsHotGas;
    float cen_gained = halos[0].galaxy->MetalsHotGas - cen_metals_before;

    snprintf(msg, sizeof(msg),
             "regime '%s': lost=%.6f (expected %.6f), gained=%.6f (expected %.6f)", cases[i].label,
             sat_lost, cases[i].sat_lost, cen_gained, cases[i].cen_gained);
    TEST_ASSERT(FLOAT_EQ(sat_lost, cases[i].sat_lost, 1e-4) &&
                    FLOAT_EQ(cen_gained, cases[i].cen_gained, 1e-4),
                msg);
  }

  check_memory_leaks();
  return TEST_PASS;
}

/**
 * @test    test_zero_hot_gas_no_stripping
 * @brief   Satellites with zero hot gas lose nothing, and nothing turns non-finite
 *
 * SAGE26 strips every live Type 1 satellite, with no HotGas guard, but the gas clamp at zero
 * hot gas and the zero metallicity it gives move no gas or metals. The demand here is positive
 * (stellar mass and cold gas exceed the allowance), so a division by the satellite's zero
 * hot gas would show as a non-finite metal value on either galaxy.
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

    TEST_ASSERT(sage26_satellite_stripping_process(&ctx, &other, 1) == 0, "Process should succeed");

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
  TEST_ASSERT(sage26_satellite_stripping_process(&ctx, &satellite_without_galaxy, 1) == 0,
              "Should handle NULL galaxy gracefully");
  TEST_ASSERT(cen_gal.HotGas == 100.0f, "Central should not change when satellite has NULL galaxy");

  TEST_ASSERT(sage26_satellite_stripping_process(NULL, &satellite, 1) == 0,
              "Should handle a NULL context");
  ctx.central_galaxy = NULL;
  TEST_ASSERT(sage26_satellite_stripping_process(&ctx, &satellite, 1) == 0,
              "Should handle a NULL central");
  ctx.central_galaxy = &central_without_galaxy;
  TEST_ASSERT(sage26_satellite_stripping_process(&ctx, &satellite, 1) == 0,
              "Should handle a central without a galaxy");
  TEST_ASSERT(sat_gal.HotGas == 5.0f && sat_gal.MetalsHotGas == 0.1f,
              "Without a central to give to, the satellite should keep its gas");
  TEST_ASSERT(cen_gal.HotGas == 100.0f && cen_gal.MetalsHotGas == 2.0f,
              "The central should be unchanged");

  ctx.central_galaxy = &central;
  struct Halo pair[2] = {satellite, satellite};
  FILE *capture = tmpfile();
  FILE *old_out = capture != NULL ? set_log_output(capture) : NULL;
  const int two = sage26_satellite_stripping_process(&ctx, pair, 2);
  const int none = sage26_satellite_stripping_process(&ctx, pair, 0);
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

// ============================================================================
// CGMrecipeOn 1 AGAINST 0
// ============================================================================
//
// SAGE26's strip_from_satellite with CGMrecipeOn 1 takes from the satellite's CGM pair if the
// satellite is in Regime 0 and gives to the central's CGM pair if the central is in Regime 0,
// each independently; with CGMrecipeOn 0 both are the hot pair. The satellite (Mvir 8,
// HaloBaryonFraction 0.125, so 1.0 allowed) holds 3.5 of baryons: hot 1.0 / 0.125 (Z 1/8) and
// CGM 0.5 / 0.125 (Z 1/4). Its excess 2.5 strips 2.5 / 8 = 0.3125 over 8 substeps (below either
// source: no cap) or 2.5 in one (the gas cap binds at the source gas, the metal cap at the
// source metals); the destination metals gain the capped gas times the source metallicity.
// Every value is a short binary fraction, so each expectation is an exact literal.

static void cgm_append_param(const char *name, const char *value) {
  const int idx = MimicConfig.NumModelParams;
  snprintf(MimicConfig.ModelParams[idx].param_name, sizeof(MimicConfig.ModelParams[idx].param_name),
           "%s", name);
  snprintf(MimicConfig.ModelParams[idx].value, sizeof(MimicConfig.ModelParams[idx].value), "%s",
           value);
  MimicConfig.NumModelParams = idx + 1;
}

/** @brief Initialise with GlobalBaryonFraction 0.17 and CGMrecipeOn `cgm` */
static int cgm_init(const char *cgm) {
  setup_mock_config();
  cgm_append_param("GlobalBaryonFraction", "0.17");
  cgm_append_param("CGMrecipeOn", cgm);
  return sage26_satellite_stripping_init();
}

/** @brief A galaxy's hot and CGM pairs */
struct cgm_pairs {
  float hot, metals_hot, cgm, metals_cgm;
};

static int cgm_same_bits(const float a, const float b) { return memcmp(&a, &b, sizeof(a)) == 0; }

static int cgm_pairs_equal(const struct cgm_pairs *a, const struct cgm_pairs *b) {
  return cgm_same_bits(a->hot, b->hot) && cgm_same_bits(a->metals_hot, b->metals_hot) &&
         cgm_same_bits(a->cgm, b->cgm) && cgm_same_bits(a->metals_cgm, b->metals_cgm);
}

/** @brief Strip the satellite once at CGMrecipeOn `cgm` with the given regimes and substeps */
static int cgm_strip_metals(const char *cgm, const int sat_regime, const int cen_regime,
                            const int substeps, const float sat_metals_hot,
                            const float sat_metals_cgm, struct cgm_pairs *sat_out,
                            struct cgm_pairs *cen_out) {
  if (cgm_init(cgm) != 0) {
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
  sat_gal.MetalsHotGas = sat_metals_hot;
  sat_gal.CGMgas = 0.5f;
  sat_gal.MetalsCGMgas = sat_metals_cgm;
  sat_gal.EjectedGas = 0.25f;
  sat_gal.ICS = 0.25f;
  struct Halo central = create_test_halo(0, 64.0, &cen_gal);
  struct Halo satellite = create_test_halo(1, 8.0, &sat_gal);
  struct ModuleContext ctx = create_test_context(substeps);
  ctx.central_galaxy = &central;
  const int status = sage26_satellite_stripping_process(&ctx, &satellite, 1);
  *sat_out = (struct cgm_pairs){sat_gal.HotGas, sat_gal.MetalsHotGas, sat_gal.CGMgas,
                                sat_gal.MetalsCGMgas};
  *cen_out = (struct cgm_pairs){cen_gal.HotGas, cen_gal.MetalsHotGas, cen_gal.CGMgas,
                                cen_gal.MetalsCGMgas};
  return status;
}

/** @brief cgm_strip_metals() with the default source metals, 0.125 in each pair */
static int cgm_strip(const char *cgm, const int sat_regime, const int cen_regime,
                     const int substeps, struct cgm_pairs *sat_out, struct cgm_pairs *cen_out) {
  return cgm_strip_metals(cgm, sat_regime, cen_regime, substeps, 0.125f, 0.125f, sat_out, cen_out);
}

/**
 * @brief The expected pairs after one strip from `take_cgm` (else hot) to `give_cgm` (else hot)
 *
 * Literal values: uncapped (8 substeps) 0.3125 of gas with 0.078125 (CGM source, Z 1/4) or
 * 0.0390625 (hot source, Z 1/8) of metals; capped (1 substep) the source gas, 0.5 or 1.0, with the
 * source metals 0.125 debited and the capped gas times Z, 0.125 either way, credited.
 */
static void cgm_expected(const int take_cgm, const int give_cgm, const int capped,
                         struct cgm_pairs *sat, struct cgm_pairs *cen) {
  *sat = (struct cgm_pairs){1.0f, 0.125f, 0.5f, 0.125f};
  *cen = (struct cgm_pairs){4.0f, 0.5f, 2.0f, 0.25f};
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
        struct cgm_pairs sat, cen, want_sat, want_cen;

        if (cgm_strip("1", sat_regime, cen_regime, substeps, &sat, &cen) != 0) {
          printf("\n  CGMrecipeOn 1: init or process failed");
          ok = 0;
          continue;
        }
        cgm_expected(take_cgm, give_cgm, capped, &want_sat, &want_cen);
        if (!cgm_pairs_equal(&sat, &want_sat) || !cgm_pairs_equal(&cen, &want_cen)) {
          printf("\n  CGMrecipeOn 1, %s to %s, %s: satellite %.9g/%.9g/%.9g/%.9g central "
                 "%.9g/%.9g/%.9g/%.9g",
                 take_cgm ? "CGM" : "hot", give_cgm ? "CGM" : "hot", capped ? "capped" : "uncapped",
                 (double)sat.hot, (double)sat.metals_hot, (double)sat.cgm, (double)sat.metals_cgm,
                 (double)cen.hot, (double)cen.metals_hot, (double)cen.cgm, (double)cen.metals_cgm);
          ok = 0;
        }

        /* CGMrecipeOn 0, the same input: hot to hot whatever the regimes */
        if (cgm_strip("0", sat_regime, cen_regime, substeps, &sat, &cen) != 0) {
          printf("\n  CGMrecipeOn 0: init or process failed");
          ok = 0;
          continue;
        }
        cgm_expected(0, 0, capped, &want_sat, &want_cen);
        if (!cgm_pairs_equal(&sat, &want_sat) || !cgm_pairs_equal(&cen, &want_cen)) {
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
 * @test    test_cgm_stripping_metals_above_gas
 * @brief   Source metals above source gas (metallicity capped at 1) with the gas cap binding: the
 *          source is debited its capped metals, the destination credited the capped gas times the
 *          original metallicity, all four combinations at
 *          CGMrecipeOn 1 and the same inputs at CGMrecipeOn 0 (hot to hot)
 *
 * The satellite holds hot 1.0 / 1.5 and CGM 0.5 / 0.75 (metals not part of the baryon sum, which
 * stays 3.5): one substep strips 2.5, capped at the source gas (1.0 or 0.5); the metals of the
 * uncapped gas, 2.5 * 1, are capped at the source metals (1.5 or 0.75), which the source loses in
 * full, while the destination gains only the capped gas times 1 (1.0 or 0.5). A credit of the
 * capped metal debit would give 1.5 or 0.75 instead.
 */
int test_cgm_stripping_metals_above_gas(void) {
  int ok = 1;
  for (int take_cgm = 0; take_cgm <= 1; take_cgm++) {
    for (int give_cgm = 0; give_cgm <= 1; give_cgm++) {
      const int sat_regime = take_cgm ? 0 : 1;
      const int cen_regime = give_cgm ? 0 : 1;
      for (int cgm_on = 1; cgm_on >= 0; cgm_on--) {
        const int src_cgm = cgm_on ? take_cgm : 0;
        const int dst_cgm = cgm_on ? give_cgm : 0;
        struct cgm_pairs sat, cen;
        if (cgm_strip_metals(cgm_on ? "1" : "0", sat_regime, cen_regime, 1, 1.5f, 0.75f, &sat,
                             &cen) != 0) {
          printf("\n  CGMrecipeOn %d: init or process failed", cgm_on);
          ok = 0;
          continue;
        }
        /* Literal expectations: the source pair emptied, the other source pair untouched */
        struct cgm_pairs want_sat = {1.0f, 1.5f, 0.5f, 0.75f};
        struct cgm_pairs want_cen = {4.0f, 0.5f, 2.0f, 0.25f};
        if (src_cgm) {
          want_sat.cgm = 0.0f;
          want_sat.metals_cgm = 0.0f;
        } else {
          want_sat.hot = 0.0f;
          want_sat.metals_hot = 0.0f;
        }
        /* Credit: the capped gas (0.5 from CGM, 1.0 from hot) times metallicity 1 */
        if (dst_cgm) {
          want_cen.cgm = src_cgm ? 2.5f : 3.0f;
          want_cen.metals_cgm = src_cgm ? 0.75f : 1.25f;
        } else {
          want_cen.hot = src_cgm ? 4.5f : 5.0f;
          want_cen.metals_hot = src_cgm ? 1.0f : 1.5f;
        }
        if (!cgm_pairs_equal(&sat, &want_sat) || !cgm_pairs_equal(&cen, &want_cen)) {
          printf("\n  CGMrecipeOn %d, %s to %s: satellite %.9g/%.9g/%.9g/%.9g central "
                 "%.9g/%.9g/%.9g/%.9g",
                 cgm_on, src_cgm ? "CGM" : "hot", dst_cgm ? "CGM" : "hot", (double)sat.hot,
                 (double)sat.metals_hot, (double)sat.cgm, (double)sat.metals_cgm, (double)cen.hot,
                 (double)cen.metals_hot, (double)cen.cgm, (double)cen.metals_cgm);
          ok = 0;
        }
      }
    }
  }
  TEST_ASSERT(ok, "the destination must gain the capped gas times the original metallicity, not "
                  "the capped metal debit");
  return TEST_PASS;
}

/**
 * @test    test_halo_baryon_fraction_sentinel
 * @brief   HaloBaryonFraction -1 selects GlobalBaryonFraction; any other value is the satellite's
 *          own, even zero
 *
 * Only the -1 sentinel (sage26_reionization not configured) selects the global fraction, here
 * 0.3. The satellite (Mvir 10, hot gas 5.0, one substep) is stripped to its allowance:
 * 3.0 under the global 0.3, 1.7 under its own 0.17, and to nothing under its own zero
 * (the demand 5.0 is clamped to its hot gas), so a zero must not be read as unset.
 */
static int test_halo_baryon_fraction_sentinel(void) {
  static const struct {
    double halo_baryon_fraction;
    double sat_hot;
    const char *what;
  } rows[] = {
      {-1.0, 3.0, "the -1 sentinel should select GlobalBaryonFraction"},
      {0.17, 1.7, "a set fraction should be the satellite's own"},
      {0.0, 0.0, "a zero fraction should be used, not read as unset"},
  };

  setup_mock_config();
  cgm_append_param("GlobalBaryonFraction", "0.3");
  cgm_append_param("CGMrecipeOn", "0");
  TEST_ASSERT(sage26_satellite_stripping_init() == 0, "init should succeed");

  init_memory_system(0);
  for (size_t k = 0; k < sizeof(rows) / sizeof(rows[0]); k++) {
    struct ModuleContext ctx = create_test_context(1);
    struct GalaxyData cen_gal = create_test_galaxy(100.0, 2.0, 50.0, 20.0);
    struct Halo central = create_test_halo(0, 100.0, &cen_gal);
    struct GalaxyData sat_gal = create_test_galaxy(5.0, 0.1, 0.0, 0.0);
    sat_gal.HaloBaryonFraction = rows[k].halo_baryon_fraction;
    struct Halo satellite = create_test_halo(1, 10.0, &sat_gal);
    ctx.central_galaxy = &central;

    TEST_ASSERT(sage26_satellite_stripping_process(&ctx, &satellite, 1) == 0,
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
  TEST_RUN(test_metal_transfer_all_regimes);
  TEST_RUN(test_zero_hot_gas_no_stripping);
  TEST_RUN(test_unstripped_types_skipped);
  TEST_RUN(test_null_galaxy_handling);

  /* CGMrecipeOn 1 against 0 */
  TEST_RUN(test_cgm_stripping_source_and_destination);
  TEST_RUN(test_cgm_stripping_metals_above_gas);

  /* Initialises the module with its own GlobalBaryonFraction, so it runs last */
  TEST_RUN(test_halo_baryon_fraction_sentinel);

  TEST_SUMMARY();
  return TEST_RESULT();
}
