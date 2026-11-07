/**
 * @file    test_unit_sage26_apply_metal_enrichment.c
 * @brief   Unit tests for sage26_apply_metal_enrichment module
 *
 * Validates the instantaneous-recycling disk-SF metal yield in isolation using
 * minimal mocks. The module runs process_by_galaxy (one galaxy at a time, ngal=1)
 * and ejects metals into the FOF central supplied through ctx->central_galaxy, so
 * physics tests process a satellite (&halos[1], 1) with a distinct central.
 *
 * Test cases:
 *   - test_module_registration: module registers without error
 *   - test_init_valid: init/cleanup lifecycle succeeds with valid parameters
 *   - test_init_rejects_sf_after: init fails when the SF/SN apply step is ordered after it
 *   - test_init_rejects_starburst_after: init fails when starburst is ordered after it
 *   - test_physics_cold_gas_above_threshold: yield split between disk and ejected reservoirs
 *   - test_physics_cold_gas_below_threshold: full yield ejected to the central hot halo
 *   - test_consumes_new_stellar_mass: NewStellarMass is zeroed after application
 *   - test_zero_new_stellar_mass: no metals added when there are no new stars
 *   - test_zero_event_still_adds_yield: a zero yield is still added, turning -0.0 metals into +0.0
 *   - test_null_galaxy: NULL galaxy handled gracefully
 *   - test_null_central: missing FOF central handled gracefully
 *   - test_invalid_ngal: error when ngal != 1
 *   - test_cgm_yield_destination: CGMrecipeOn 1 against 0 on the same input: the escaping
 *     metals of both branches (retained, ColdGas > 1e-8; all metals, ColdGas <= 1e-8) go to the FoF
 *     central's CGM metals (its Regime 0) or hot metals (Regime 1, or CGMrecipeOn 0); the forming
 *     satellite holds the other Regime; the unselected pair and the gas fields unchanged
 *     (R/src/model_starformation_and_feedback.c:1093-1108, R/src/model_misc.h:67-74)
 *   - test_cgm_recipe_on_refused: CGMrecipeOn is required and only 0 or 1 accepted (D33)
 *   - test_ffb_row_bypassed: a row whose FFBRegime is 1 leaves itself and its FoF central
 *     bytewise unchanged (no yield, NewStellarMass not consumed); the same row with FFBRegime 0
 *     gets the yield split and NewStellarMass consumed (c13 Slice 3)
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../../../tests/framework/test_framework.h"
#include "../../../../tests/framework/test_phase_config.h"

#include "core/module_interface.h"
#include "core/module_registry.h"
#include "include/globals.h"
#include "include/types.h"
#include "shared/sage_constants.h"
#include "util/error.h"
#include "util/memory.h"

/* Shared SAGE16 test fixture boilerplate (counters, config reset, parameters) */
#include "modules/_tests/sage_test_fixtures.h"

extern int sage26_apply_metal_enrichment_init(void);
extern int sage26_apply_metal_enrichment_process(struct ModuleContext *ctx, struct Halo *halos,
                                                 int ngal);
extern int sage26_apply_metal_enrichment_cleanup(void);

#define FLOAT_EQ(a, b, epsilon) (fabs((double)(a) - (double)(b)) < (epsilon))

/* SAGE test defaults (mirror sage_test_fixtures.h set_test_model_parameters) */
#define TEST_YIELD 0.03
#define TEST_FRAC_Z_LEAVE_DISK 0.3

static struct ModuleContext create_test_context(void) {
  struct ModuleContext ctx;
  memset(&ctx, 0, sizeof(ctx));
  ctx.num_substeps = 1;
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

/**
 * @test    test_module_registration
 * @brief   Module registers correctly
 */
int test_module_registration(void) {
  reset_config();
  ensure_modules_registered();
  return TEST_PASS;
}

/**
 * @test    test_init_valid
 * @brief   init/cleanup lifecycle succeeds with valid parameters and no predecessors
 */
int test_init_valid(void) {
  reset_config();
  init_memory_system(0);
  set_test_model_parameters();
  add_test_cgm_recipe_parameter(); /* CGMrecipeOn: 0, required (D33) */

  /* No SF/SN or starburst modules configured, so the ordering checks are skipped. */
  TEST_ASSERT(sage26_apply_metal_enrichment_init() == 0,
              "init should succeed with valid parameters");

  sage26_apply_metal_enrichment_cleanup();
  check_memory_leaks();
  return TEST_PASS;
}

/**
 * @test    test_init_rejects_sf_after
 * @brief   init fails when sage26_apply_star_formation_supernova is ordered after this module
 */
int test_init_rejects_sf_after(void) {
  reset_config();
  init_memory_system(0);
  set_test_model_parameters();
  add_test_cgm_recipe_parameter(); /* CGMrecipeOn: 0, required (D33) */

  /* Wrong order: the SF/SN apply step must precede the yield. */
  test_phase_add("galaxy_physics", "sage26_apply_metal_enrichment", PROCESSING_MODE_BY_GALAXY);
  test_phase_add("galaxy_physics", "sage26_apply_star_formation_supernova",
                 PROCESSING_MODE_BY_GALAXY);

  TEST_ASSERT(sage26_apply_metal_enrichment_init() == -1,
              "init should reject SF/SN apply step ordered after the yield");

  test_free_substep_phases();
  check_memory_leaks();
  return TEST_PASS;
}

/**
 * @test    test_init_rejects_starburst_after
 * @brief   init fails when sage26_starburst_feedback is ordered after this module
 */
int test_init_rejects_starburst_after(void) {
  reset_config();
  init_memory_system(0);
  set_test_model_parameters();
  add_test_cgm_recipe_parameter(); /* CGMrecipeOn: 0, required (D33) */

  /* Wrong order: SAGE adds the disk yield after the disk-instability starburst. */
  test_phase_add("galaxy_physics", "sage26_apply_metal_enrichment", PROCESSING_MODE_BY_GALAXY);
  test_phase_add("galaxy_physics", "sage26_starburst_feedback", PROCESSING_MODE_BY_GALAXY);

  TEST_ASSERT(sage26_apply_metal_enrichment_init() == -1,
              "init should reject starburst ordered after the yield");

  test_free_substep_phases();
  check_memory_leaks();
  return TEST_PASS;
}

/**
 * @test    test_physics_cold_gas_above_threshold
 * @brief   Above the cold-gas threshold the yield splits between disk cold gas and the
 *          central hot halo, scaled by the Krumholz & Dekel ejection factor.
 */
int test_physics_cold_gas_above_threshold(void) {
  reset_config();
  init_memory_system(0);
  set_test_model_parameters();
  add_test_cgm_recipe_parameter(); /* CGMrecipeOn: 0, required (D33) */
  TEST_ASSERT(sage26_apply_metal_enrichment_init() == 0, "init should succeed");

  struct ModuleContext ctx = create_test_context();
  struct GalaxyData cen_gal;
  memset(&cen_gal, 0, sizeof(cen_gal));
  struct Halo central = create_test_halo(0, 30.0, &cen_gal); /* Mvir=30 → exp(-1) ejection scale */

  struct GalaxyData sat_gal;
  memset(&sat_gal, 0, sizeof(sat_gal));
  sat_gal.ColdGas = 5.0; /* above SAGE_COLD_GAS_YIELD_THRESHOLD */
  sat_gal.NewStellarMass = 2.0;
  struct Halo satellite = create_test_halo(1, 10.0, &sat_gal);

  ctx.central_galaxy = &central;
  struct Halo halos[2] = {central, satellite};
  /* halos[] copies the structs, but galaxy pointers still reference cen_gal/sat_gal. */
  ctx.central_galaxy = &halos[0];

  const double stars = 2.0;
  const double frac_z_leave = TEST_FRAC_Z_LEAVE_DISK * exp(-30.0 / SAGE_METAL_EJECTION_MVIR_SCALE);
  const double expected_cold = TEST_YIELD * (1.0 - frac_z_leave) * stars;
  const double expected_hot = TEST_YIELD * frac_z_leave * stars;

  int result = sage26_apply_metal_enrichment_process(&ctx, &halos[1], 1);

  TEST_ASSERT(result == 0, "process should succeed");
  TEST_ASSERT(FLOAT_EQ(halos[1].galaxy->MetalsColdGas, expected_cold, 1e-6),
              "disk metals should receive the retained yield");
  TEST_ASSERT(FLOAT_EQ(halos[0].galaxy->MetalsHotGas, expected_hot, 1e-6),
              "central hot metals should receive the ejected yield");
  check_memory_leaks();
  return TEST_PASS;
}

/**
 * @test    test_physics_cold_gas_below_threshold
 * @brief   At/below the cold-gas threshold the full yield is ejected to the central hot halo.
 */
int test_physics_cold_gas_below_threshold(void) {
  reset_config();
  init_memory_system(0);
  set_test_model_parameters();
  add_test_cgm_recipe_parameter(); /* CGMrecipeOn: 0, required (D33) */
  TEST_ASSERT(sage26_apply_metal_enrichment_init() == 0, "init should succeed");

  struct ModuleContext ctx = create_test_context();
  struct GalaxyData cen_gal;
  memset(&cen_gal, 0, sizeof(cen_gal));
  struct Halo central = create_test_halo(0, 30.0, &cen_gal);

  struct GalaxyData sat_gal;
  memset(&sat_gal, 0, sizeof(sat_gal));
  sat_gal.ColdGas = 0.0; /* at/below SAGE_COLD_GAS_YIELD_THRESHOLD */
  sat_gal.NewStellarMass = 2.0;
  struct Halo satellite = create_test_halo(1, 10.0, &sat_gal);

  struct Halo halos[2] = {central, satellite};
  ctx.central_galaxy = &halos[0];

  const double expected_hot = TEST_YIELD * 2.0;

  int result = sage26_apply_metal_enrichment_process(&ctx, &halos[1], 1);

  TEST_ASSERT(result == 0, "process should succeed");
  TEST_ASSERT(FLOAT_EQ(halos[1].galaxy->MetalsColdGas, 0.0, 1e-9),
              "disk metals should be untouched below the cold-gas threshold");
  TEST_ASSERT(FLOAT_EQ(halos[0].galaxy->MetalsHotGas, expected_hot, 1e-6),
              "central hot metals should receive the full yield");
  check_memory_leaks();
  return TEST_PASS;
}

/**
 * @test    test_consumes_new_stellar_mass
 * @brief   NewStellarMass is the per-substep transport field and must be zeroed after use.
 */
int test_consumes_new_stellar_mass(void) {
  reset_config();
  init_memory_system(0);
  set_test_model_parameters();
  add_test_cgm_recipe_parameter(); /* CGMrecipeOn: 0, required (D33) */
  TEST_ASSERT(sage26_apply_metal_enrichment_init() == 0, "init should succeed");

  struct ModuleContext ctx = create_test_context();
  struct GalaxyData cen_gal;
  memset(&cen_gal, 0, sizeof(cen_gal));
  struct Halo central = create_test_halo(0, 30.0, &cen_gal);

  struct GalaxyData sat_gal;
  memset(&sat_gal, 0, sizeof(sat_gal));
  sat_gal.ColdGas = 5.0;
  sat_gal.NewStellarMass = 2.0;
  struct Halo satellite = create_test_halo(1, 10.0, &sat_gal);

  struct Halo halos[2] = {central, satellite};
  ctx.central_galaxy = &halos[0];

  sage26_apply_metal_enrichment_process(&ctx, &halos[1], 1);

  TEST_ASSERT(FLOAT_EQ(halos[1].galaxy->NewStellarMass, 0.0, 1e-12),
              "NewStellarMass should be consumed (zeroed)");
  check_memory_leaks();
  return TEST_PASS;
}

/**
 * @test    test_zero_new_stellar_mass
 * @brief   No new stars means no metals are produced.
 */
int test_zero_new_stellar_mass(void) {
  reset_config();
  init_memory_system(0);
  set_test_model_parameters();
  add_test_cgm_recipe_parameter(); /* CGMrecipeOn: 0, required (D33) */
  TEST_ASSERT(sage26_apply_metal_enrichment_init() == 0, "init should succeed");

  struct ModuleContext ctx = create_test_context();
  struct GalaxyData cen_gal;
  memset(&cen_gal, 0, sizeof(cen_gal));
  struct Halo central = create_test_halo(0, 30.0, &cen_gal);

  struct GalaxyData sat_gal;
  memset(&sat_gal, 0, sizeof(sat_gal));
  sat_gal.ColdGas = 5.0;
  sat_gal.NewStellarMass = 0.0;
  struct Halo satellite = create_test_halo(1, 10.0, &sat_gal);

  struct Halo halos[2] = {central, satellite};
  ctx.central_galaxy = &halos[0];

  int result = sage26_apply_metal_enrichment_process(&ctx, &halos[1], 1);

  TEST_ASSERT(result == 0, "process should succeed");
  TEST_ASSERT(FLOAT_EQ(halos[1].galaxy->MetalsColdGas, 0.0, 1e-9), "no disk metals when no stars");
  TEST_ASSERT(FLOAT_EQ(halos[0].galaxy->MetalsHotGas, 0.0, 1e-9), "no hot metals when no stars");
  check_memory_leaks();
  return TEST_PASS;
}

/**
 * @test    test_zero_event_still_adds_yield
 * @brief   SAGE26 adds the yield on every call, also with no new stars
 *
 * SAGE26's starformation_and_feedback adds the disk yield unconditionally
 * (R/src/model_starformation_and_feedback.c:1092-1109), so a zero yield is still
 * added: a -0.0 metal reservoir becomes +0.0 (-0.0 + +0.0 = +0.0). A module that
 * returned early on zero stars would leave -0.0. No oracle fixture has both zero
 * stars and a -0.0 reservoir, so this pins the behaviour.
 */
int test_zero_event_still_adds_yield(void) {
  reset_config();
  init_memory_system(0);
  set_test_model_parameters();
  add_test_cgm_recipe_parameter(); /* CGMrecipeOn: 0, required (D33) */
  TEST_ASSERT(sage26_apply_metal_enrichment_init() == 0, "init should succeed");

  struct ModuleContext ctx = create_test_context();
  struct GalaxyData cen_gal;
  memset(&cen_gal, 0, sizeof(cen_gal));
  cen_gal.MetalsHotGas = -0.0f;
  struct Halo central = create_test_halo(0, 30.0, &cen_gal);

  struct GalaxyData sat_gal;
  memset(&sat_gal, 0, sizeof(sat_gal));
  sat_gal.ColdGas = 5.0;
  sat_gal.MetalsColdGas = -0.0f;
  sat_gal.NewStellarMass = 0.0;
  struct Halo satellite = create_test_halo(1, 10.0, &sat_gal);

  struct Halo halos[2] = {central, satellite};
  ctx.central_galaxy = &halos[0];

  TEST_ASSERT(sage26_apply_metal_enrichment_process(&ctx, &halos[1], 1) == 0,
              "process should succeed");
  TEST_ASSERT(halos[1].galaxy->MetalsColdGas == 0.0f && !signbit(halos[1].galaxy->MetalsColdGas),
              "the retained zero yield turns -0.0 cold metals into +0.0");
  TEST_ASSERT(halos[0].galaxy->MetalsHotGas == 0.0f && !signbit(halos[0].galaxy->MetalsHotGas),
              "the escaping zero yield turns -0.0 hot metals into +0.0");
  check_memory_leaks();
  return TEST_PASS;
}

/**
 * @test    test_null_galaxy
 * @brief   A halo with no galaxy is a no-op.
 */
int test_null_galaxy(void) {
  reset_config();
  init_memory_system(0);
  set_test_model_parameters();
  add_test_cgm_recipe_parameter(); /* CGMrecipeOn: 0, required (D33) */
  sage26_apply_metal_enrichment_init();

  struct ModuleContext ctx = create_test_context();
  struct GalaxyData cen_gal;
  memset(&cen_gal, 0, sizeof(cen_gal));
  struct Halo central = create_test_halo(0, 30.0, &cen_gal);
  struct Halo galaxyless = create_test_halo(1, 10.0, NULL);

  ctx.central_galaxy = &central;
  int result = sage26_apply_metal_enrichment_process(&ctx, &galaxyless, 1);

  TEST_ASSERT(result == 0, "NULL galaxy should be a graceful no-op");
  check_memory_leaks();
  return TEST_PASS;
}

/**
 * @test    test_null_central
 * @brief   A missing FOF central (no ejection destination) is a no-op.
 */
int test_null_central(void) {
  reset_config();
  init_memory_system(0);
  set_test_model_parameters();
  add_test_cgm_recipe_parameter(); /* CGMrecipeOn: 0, required (D33) */
  sage26_apply_metal_enrichment_init();

  struct ModuleContext ctx = create_test_context();
  struct GalaxyData sat_gal;
  memset(&sat_gal, 0, sizeof(sat_gal));
  sat_gal.ColdGas = 5.0;
  sat_gal.NewStellarMass = 2.0;
  struct Halo satellite = create_test_halo(1, 10.0, &sat_gal);

  ctx.central_galaxy = NULL;
  int result = sage26_apply_metal_enrichment_process(&ctx, &satellite, 1);

  TEST_ASSERT(result == 0, "missing central should be a graceful no-op");
  TEST_ASSERT(FLOAT_EQ(satellite.galaxy->MetalsColdGas, 0.0, 1e-9),
              "no metals applied without an ejection destination");
  check_memory_leaks();
  return TEST_PASS;
}

/**
 * @test    test_invalid_ngal
 * @brief   process_by_galaxy expects ngal=1; anything else is an error.
 */
int test_invalid_ngal(void) {
  reset_config();
  init_memory_system(0);
  set_test_model_parameters();
  add_test_cgm_recipe_parameter(); /* CGMrecipeOn: 0, required (D33) */
  sage26_apply_metal_enrichment_init();

  struct ModuleContext ctx = create_test_context();
  struct GalaxyData gal;
  memset(&gal, 0, sizeof(gal));
  struct Halo halo = create_test_halo(0, 30.0, &gal);
  ctx.central_galaxy = &halo;

  TEST_ASSERT(sage26_apply_metal_enrichment_process(&ctx, &halo, 2) == -1,
              "ngal != 1 should return an error");
  check_memory_leaks();
  return TEST_PASS;
}

// ============================================================================
// CGM ROUTING TESTS (c10, CGMrecipeOn)
// ============================================================================
//
// SAGE26's disk yield (R = the staged SAGE26 source, R/src/model_starformation_and_feedback.c
// :1093-1108) sends the escaping metals through add_metals_to_hot_reservoir
// (R/src/model_misc.h:67-74) to the FoF CENTRAL's MetalsCGMgas when CGMrecipeOn is 1 and the
// central's Regime is 0, and to its MetalsHotGas otherwise. With Yield 0.5, FracZleaveDisk 0.5 and
// a central Mvir of 0 (exp(-0) = 1, so FracZleaveDiskVal = 0.5) and 1 star formed, every value is
// exact: the retained branch (satellite ColdGas 4) keeps 0.5 * (1 - 0.5) * 1 = 0.25 in the
// satellite's cold metals and sends 0.5 * 0.5 * 1 = 0.25 out; the all-metals branch (ColdGas 0)
// sends 0.5 * 1 = 0.5 out. Both central metal fields start at 0.125.

/** @brief Read a captured log stream into a static buffer (valid until the next call) */
static const char *c10_read_log(FILE *fp) {
  static char buf[8192];
  rewind(fp);
  const size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
  buf[n] = '\0';
  return buf;
}

/** @brief Initialise with Yield 0.5, FracZleaveDisk 0.5 and CGMrecipeOn `cgm` (NULL: missing) */
static int c10_init(const char *cgm, const char **log) {
  reset_config();
  sage26_test_append_model_parameter("Yield", "0.5");
  sage26_test_append_model_parameter("FracZleaveDisk", "0.5");
  if (cgm != NULL) {
    sage26_test_append_model_parameter("CGMrecipeOn", cgm);
  }
  FILE *capture = tmpfile();
  FILE *old_out = capture != NULL ? set_log_output(capture) : NULL;
  const int status = sage26_apply_metal_enrichment_init();
  if (capture != NULL) {
    set_log_output(old_out);
    *log = c10_read_log(capture);
    fclose(capture);
  } else {
    *log = "";
  }
  return status;
}

/**
 * @test    test_cgm_yield_destination
 * @brief   The escaping disk yield reaches the FoF central's metal field its Regime selects
 */
int test_cgm_yield_destination(void) {
  const struct {
    const char *cgm;
    int central_regime;
    int to_cgm;
  } runs[] = {{"1", 0, 1}, {"1", 1, 0}, {"0", 0, 0}, {"0", 1, 0}};
  const struct {
    const char *name;
    float cold_gas;    /* the satellite's ColdGas */
    float cold_metals; /* the satellite's MetalsColdGas after the call (input 0.125) */
    float selected;    /* the central's selected metals after the call (input 0.125) */
  } branches[] = {{"retained", 4.0f, 0.375f, 0.375f}, {"all metals", 0.0f, 0.125f, 0.625f}};
  int ok = 1;
  init_memory_system(0);
  for (size_t r = 0; r < sizeof(runs) / sizeof(runs[0]); r++) {
    for (size_t b = 0; b < sizeof(branches) / sizeof(branches[0]); b++) {
      const char *log = "";
      TEST_ASSERT(c10_init(runs[r].cgm, &log) == 0, "init must accept CGMrecipeOn 0 and 1");
      struct ModuleContext ctx = create_test_context();
      struct GalaxyData cen_gal;
      memset(&cen_gal, 0, sizeof(cen_gal));
      cen_gal.Regime = runs[r].central_regime;
      cen_gal.HotGas = 1.0f;
      cen_gal.MetalsHotGas = 0.125f;
      cen_gal.CGMgas = 1.0f;
      cen_gal.MetalsCGMgas = 0.125f;
      struct GalaxyData sat_gal;
      memset(&sat_gal, 0, sizeof(sat_gal));
      sat_gal.Regime = 1 - runs[r].central_regime;
      sat_gal.ColdGas = branches[b].cold_gas;
      sat_gal.MetalsColdGas = 0.125f;
      sat_gal.HotGas = 2.0f;
      sat_gal.MetalsHotGas = 0.25f;
      sat_gal.CGMgas = 3.0f;
      sat_gal.MetalsCGMgas = 0.5f;
      sat_gal.NewStellarMass = 1.0;
      struct Halo halos[2] = {create_test_halo(0, 0.0, &cen_gal),
                              create_test_halo(1, 10.0, &sat_gal)};
      ctx.central_galaxy = &halos[0];

      const int status = sage26_apply_metal_enrichment_process(&ctx, &halos[1], 1);

      const float sel = runs[r].to_cgm ? cen_gal.MetalsCGMgas : cen_gal.MetalsHotGas;
      const float oth = runs[r].to_cgm ? cen_gal.MetalsHotGas : cen_gal.MetalsCGMgas;
      const int central_ok = sel == branches[b].selected && oth == 0.125f &&
                             cen_gal.HotGas == 1.0f && cen_gal.CGMgas == 1.0f;
      const int satellite_ok = sat_gal.MetalsColdGas == branches[b].cold_metals &&
                               sat_gal.ColdGas == branches[b].cold_gas &&
                               sat_gal.MetalsHotGas == 0.25f && sat_gal.MetalsCGMgas == 0.5f &&
                               sat_gal.NewStellarMass == 0.0;
      if (status != 0 || !central_ok || !satellite_ok) {
        printf("\n  CGMrecipeOn %s, central Regime %d, %s: status %d, central hot metals %.9g, CGM "
               "metals %.9g, satellite cold metals %.9g",
               runs[r].cgm, runs[r].central_regime, branches[b].name, status,
               (double)cen_gal.MetalsHotGas, (double)cen_gal.MetalsCGMgas,
               (double)sat_gal.MetalsColdGas);
        ok = 0;
      }
      sage26_apply_metal_enrichment_cleanup();
    }
  }
  check_memory_leaks();
  TEST_ASSERT(ok, "the escaping yield must reach exactly the FoF central's selected metals");
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
  return TEST_PASS;
}

/**
 * @test    test_ffb_row_bypassed
 * @brief   An FFBRegime 1 row gets no yield; the same row with FFBRegime 0 does (c13 Slice 3)
 *
 * SAGE26's starformation_and_feedback hands an FFBRegime 1 row to starformation_ffb and returns
 * before its ordinary yield (R/src/model_starformation_and_feedback.c:1004-1008). The row is a
 * satellite with a seeded NewStellarMass and its own FoF central; with FFBRegime 1 both galaxy
 * structs are compared byte for byte with their inputs. With FFBRegime 0 the yield is SAGE26's
 * (:1093-1103): the retained Yield * (1 - FracZleaveDiskVal) * stars into the cold metals and the
 * rest to the central's hot metals (CGMrecipeOn 0), and NewStellarMass is consumed.
 */
int test_ffb_row_bypassed(void) {
  reset_config();
  init_memory_system(0);
  set_test_model_parameters();
  add_test_cgm_recipe_parameter(); /* CGMrecipeOn: 0, required (D33) */
  TEST_ASSERT(sage26_apply_metal_enrichment_init() == 0, "init should succeed");

  struct ModuleContext ctx = create_test_context();
  struct GalaxyData cen_gal, sat_gal;
  memset(&cen_gal, 0, sizeof(cen_gal));
  memset(&sat_gal, 0, sizeof(sat_gal));
  cen_gal.MetalsHotGas = 0.0;
  sat_gal.ColdGas = 5.0;
  sat_gal.NewStellarMass = 2.0;
  sat_gal.FFBRegime = 1;
  struct Halo halos[2] = {create_test_halo(0, 30.0, &cen_gal), create_test_halo(1, 10.0, &sat_gal)};
  ctx.central_galaxy = &halos[0];

  struct GalaxyData row_in, central_in;
  memcpy(&row_in, &sat_gal, sizeof(sat_gal));
  memcpy(&central_in, &cen_gal, sizeof(cen_gal));

  TEST_ASSERT(sage26_apply_metal_enrichment_process(&ctx, &halos[1], 1) == 0,
              "process succeeds on an FFB row");
  TEST_ASSERT(memcmp(&sat_gal, &row_in, sizeof(sat_gal)) == 0,
              "FFBRegime 1: every field of the row is unchanged (NewStellarMass not consumed)");
  TEST_ASSERT(memcmp(&cen_gal, &central_in, sizeof(cen_gal)) == 0,
              "FFBRegime 1: every field of its central is unchanged");

  sat_gal.FFBRegime = 0;
  TEST_ASSERT(sage26_apply_metal_enrichment_process(&ctx, &halos[1], 1) == 0,
              "process succeeds on an ordinary row");
  const double stars = 2.0;
  const double frac_z_leave = TEST_FRAC_Z_LEAVE_DISK * exp(-1.0 * 30.0 / 30.0); /* :1094 */
  const double expected_cold = TEST_YIELD * (1.0 - frac_z_leave) * stars;       /* :1097 */
  const double expected_hot = TEST_YIELD * frac_z_leave * stars;                /* :1100-1102 */
  printf("\n  FFBRegime 0: MetalsColdGas %.9g (expected %.9g), central MetalsHotGas %.9g (%.9g)",
         (double)sat_gal.MetalsColdGas, expected_cold, (double)cen_gal.MetalsHotGas, expected_hot);
  TEST_ASSERT(FLOAT_EQ(sat_gal.MetalsColdGas, expected_cold, 1e-7),
              "FFBRegime 0: the disk keeps the retained yield");
  TEST_ASSERT(FLOAT_EQ(cen_gal.MetalsHotGas, expected_hot, 1e-7),
              "FFBRegime 0: the central's hot metals receive the escaping yield");
  TEST_ASSERT(sat_gal.NewStellarMass == 0.0, "FFBRegime 0: NewStellarMass is consumed");
  check_memory_leaks();
  return TEST_PASS;
}

int main(void) {
  printf("%s", BLUE);
  printf("============================================================\n");
  printf("Test Suite: sage26_apply_metal_enrichment Module\n");
  printf("============================================================\n");
  printf("%s\n", NC);

  initialize_error_handling(LOG_LEVEL_DEBUG, NULL);

  TEST_RUN(test_module_registration);
  TEST_RUN(test_init_valid);
  TEST_RUN(test_init_rejects_sf_after);
  TEST_RUN(test_init_rejects_starburst_after);
  TEST_RUN(test_physics_cold_gas_above_threshold);
  TEST_RUN(test_physics_cold_gas_below_threshold);
  TEST_RUN(test_consumes_new_stellar_mass);
  TEST_RUN(test_zero_new_stellar_mass);
  TEST_RUN(test_zero_event_still_adds_yield);
  TEST_RUN(test_null_galaxy);
  TEST_RUN(test_null_central);
  TEST_RUN(test_invalid_ngal);
  TEST_RUN(test_cgm_yield_destination);
  TEST_RUN(test_cgm_recipe_on_refused);
  TEST_RUN(test_ffb_row_bypassed);

  TEST_SUMMARY();
  return TEST_RESULT();
}
