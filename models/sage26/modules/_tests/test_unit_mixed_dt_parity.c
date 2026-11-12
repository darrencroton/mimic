/**
 * @file    test_unit_mixed_dt_parity.c
 * @brief   Mixed-dT parity test — verifies per-object timestep in FOF groups
 *
 * When satellites in a FOF group have different SnapNum values (and hence
 * different dT), each object must evolve with its own timestep. This test
 * constructs deterministic fixtures where the objects have different dT values and the module
 * context carries yet another substep_dt, then verifies that each module uses the per-object dT
 * rather than the context's value.
 *
 * SAGE parity rule: deltaT_p = Age[snap_of_p] - Age[current_halo_snap]
 * Per-substep dt for object p: deltaT_p / num_substeps
 *
 * Test cases:
 *   MERGER TIME TESTS (IMMEDIATE HANDLER):
 *   - test_mixed_dt_merger_timestamp_source_time: Merger timestamp uses source-object time
 *
 *   INVALID DT TESTS (IMMEDIATE HANDLER):
 *   - test_invalid_nonboundary_merger_time_error: non-boundary dT<=0 fails fast, naming the halo
 *     and its dT, and changes no galaxy
 *
 *   PER-OBJECT DT (REINCORPORATION, STAR FORMATION, COOLING, RADIO-MODE HEATING):
 *   - test_mixed_dt_uses_object_dt: the module's result scales with the object's dT under one
 *     fixed context dt, and does not change with the context's dt
 *
 *   REINCORPORATION BOUNDARY TESTS:
 *   - test_boundary_sentinel_reincorporation_noop: SnapNum<0,dT<=0 is a deterministic no-op
 *   - test_invalid_nonboundary_reincorporation_error: SnapNum>=0,dT<=0 fails fast, naming the halo
 *     and its dT, and changes no gas
 */

#include "../../../tests/framework/test_framework.h"
#include "../../../tests/framework/test_phase_config.h"
#include "../core/module_interface.h"
#include "../core/module_registry.h"
#include "../include/types.h"
#include "../include/globals.h"
#include "../util/error.h"
#include "../util/memory.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* Shared sage26 test fixture boilerplate (counters, config reset, module registration) */
#include "modules/_tests/sage_test_fixtures.h"

#define COUNT(array) (sizeof(array) / sizeof((array)[0]))

/* External module interfaces */
extern int sage26_reincorporation_init(void);
extern int sage26_reincorporation_process(struct ModuleContext *ctx, struct Halo *halos, int ngal);
extern int sage26_reincorporation_cleanup(void);

extern int sage26_calculate_star_formation_init(void);
extern int sage26_calculate_star_formation_process(struct ModuleContext *ctx, struct Halo *halos,
                                                   int ngal);
extern int sage26_calculate_star_formation_cleanup(void);

extern int sage26_calculate_cooling_budget_init(void);
extern int sage26_calculate_cooling_budget_process(struct ModuleContext *ctx, struct Halo *halos,
                                                   int ngal);
extern int sage26_calculate_cooling_budget_cleanup(void);

extern int sage26_radio_mode_heating_init(void);
extern int sage26_radio_mode_heating_process(struct ModuleContext *ctx, struct Halo *halos,
                                             int ngal);
extern int sage26_radio_mode_heating_cleanup(void);

extern int sage26_resolve_mergers_and_disruption_init(void);
extern int sage26_resolve_mergers_and_disruption_process(struct ModuleContext *ctx,
                                                         struct Halo *halos, int ngal);
extern int sage26_resolve_mergers_and_disruption_cleanup(void);

/* ========================================================================== */
/* HELPERS                                                                    */
/* ========================================================================== */

static struct ModuleContext create_context(double central_dt, int num_substeps) {
  struct ModuleContext ctx;
  memset(&ctx, 0, sizeof(ctx));

  ctx.redshift = 0.0;
  ctx.time = 13.6;
  ctx.snapshot_number = 63;
  ctx.substep_number = 0;
  ctx.num_substeps = num_substeps;
  ctx.time_interval = central_dt;
  ctx.substep_dt = central_dt / num_substeps;
  ctx.params = &MimicConfig;

  return ctx;
}

static struct GalaxyData *alloc_galaxy(void) {
  struct GalaxyData *gal = mymalloc_cat(sizeof(struct GalaxyData), MEM_HALOS);
  memset(gal, 0, sizeof(struct GalaxyData));
  return gal;
}

static void free_galaxy(struct GalaxyData **gal) {
  if (*gal != NULL) {
    myfree(*gal);
    *gal = NULL;
  }
}

/** @brief Set an existing run-file parameter to `value` (the fixture's value is replaced) */
static void set_param(const char *name, const char *value) {
  for (int i = 0; i < MimicConfig.NumModelParams; i++) {
    if (strcmp(MimicConfig.ModelParams[i].param_name, name) == 0) {
      snprintf(MimicConfig.ModelParams[i].value, MAX_STRING_LEN, "%s", value);
    }
  }
}

/**
 * @brief Initialise the merger resolver on the package's test parameters (ThresholdSatDisruption
 *        as given) with the merger clock configured in pre_timestep, as its init requires
 *
 * @return the init status
 */
static int init_resolver(const char *threshold_sat_disruption) {
  reset_config();
  set_test_model_parameters();
  set_param("ThresholdSatDisruption", threshold_sat_disruption);
  add_test_cgm_recipe_parameter(); /* CGMrecipeOn: 0, required */
  test_pre_timestep_add("sage26_initialise_merger_clock", PROCESSING_MODE_FULL_HALO);
  return sage26_resolve_mergers_and_disruption_init();
}

/** @brief Run a module's process() with its log captured into `log` */
static int process_logged(int (*process)(struct ModuleContext *, struct Halo *, int),
                          struct ModuleContext *ctx, struct Halo *halos, int ngal, char *log,
                          size_t log_size) {
  FILE *capture = tmpfile();
  FILE *old_out = capture != NULL ? set_log_output(capture) : NULL;
  const int result = process(ctx, halos, ngal);
  log[0] = '\0';
  if (capture != NULL) {
    set_log_output(old_out);
    rewind(capture);
    const size_t n = fread(log, 1, log_size - 1, capture);
    log[n] = '\0';
    fclose(capture);
  }
  return result;
}

/* ========================================================================== */
/* MERGER TIME: MIXED-DT TIMESTAMP (IMMEDIATE HANDLER)                       */
/* ========================================================================== */

/**
 * @test    test_mixed_dt_merger_timestamp_source_time
 * @brief   TimeOfLastMinorMerger uses source-object time, not ctx->substep_time
 *
 * Satellite dT=0.4, num_substeps=2, substep_number=1.
 * source_dt = 0.4/2 = 0.2.
 * Satellite MergTime = 0.1 -> after decrement: 0.1 - 0.2 = -0.1 <= 0 -> merge.
 * Expected TimeOfLastMinorMerger = ctx.time + sat.dT - (substep+0.5)*dt_obj
 *                                 = 13.0 + 0.4 - (1.5 * 0.2) = 13.1
 * ctx.substep_time = 12.7 (deliberately different to detect wrong source).
 */
int test_mixed_dt_merger_timestamp_source_time(void) {
  init_memory_system(0);
  TEST_ASSERT(init_resolver("2.0") == 0, "the resolver must initialise");

  struct GalaxyData *central_gal = alloc_galaxy();
  central_gal->StellarMass = 10.0;
  central_gal->ColdGas = 5.0;
  central_gal->TimeOfLastMinorMerger = 0.0;
  struct Halo central = {0};
  central.Type = 0;
  central.SnapNum = 63;
  central.dT = 0.2;
  central.galaxy = central_gal;

  struct GalaxyData *sat_gal = alloc_galaxy();
  sat_gal->StellarMass = 2.0; /* mass_ratio = (2+1)/(10+5) = 0.2 -> minor merger */
  sat_gal->ColdGas = 1.0;
  sat_gal->MergTime = 0.1f; /* 0.1 - source_dt(0.2) = -0.1 -> merge */
  struct Halo sat = {0};
  sat.Type = 1;
  sat.HaloNr = 7;
  sat.SnapNum = 60;
  sat.dT = 0.4;
  sat.Mvir = 5.0; /* virial_to_baryons = 5/(2+1) = 1.67 <= 2.0 -> eligible */
  sat.galaxy = sat_gal;

  struct ModuleContext ctx = create_context(0.2, 2);
  ctx.time = 13.0;
  ctx.snapshot_number = 63;
  ctx.substep_number = 1;
  ctx.substep_time = 12.7; /* Deliberately different from source-object time */
  ctx.central_galaxy = &central;

  struct Halo halos[2] = {central, sat};
  int result = sage26_resolve_mergers_and_disruption_process(&ctx, halos, 2);
  TEST_ASSERT(result == 0, "Immediate merger processing should succeed");

  const double dt_obj = sat.dT / ctx.num_substeps;
  const double expected_time = (ctx.time + sat.dT) - ((double)ctx.substep_number + 0.5) * dt_obj;
  TEST_ASSERT_DOUBLE_EQUAL(halos[0].galaxy->TimeOfLastMinorMerger, expected_time, 1e-6,
                           "Minor merger timestamp should use source-object time");
  TEST_ASSERT(fabs(halos[0].galaxy->TimeOfLastMinorMerger - ctx.substep_time) > 1e-3,
              "Minor merger timestamp should not use global ctx->substep_time");

  free_galaxy(&central_gal);
  free_galaxy(&sat_gal);
  sage26_resolve_mergers_and_disruption_cleanup();
  test_free_pre_timestep();
  check_memory_leaks();

  return TEST_PASS;
}

/* ========================================================================== */
/* INVALID DT: FAIL-FAST (IMMEDIATE HANDLER)                                 */
/* ========================================================================== */

/**
 * @test    test_invalid_nonboundary_merger_time_error
 * @brief   Immediate handler fails fast for non-boundary dT<=0, names the halo and its dT, and
 *          leaves both galaxies as they were
 */
int test_invalid_nonboundary_merger_time_error(void) {
  init_memory_system(0);
  TEST_ASSERT(init_resolver("1.0") == 0, "the resolver must initialise");

  struct GalaxyData *cen_gal = alloc_galaxy();
  cen_gal->StellarMass = 10.0;
  cen_gal->ColdGas = 5.0;
  struct Halo central = {0};
  central.Type = 0;
  central.SnapNum = 63;
  central.dT = 0.2;
  central.galaxy = cen_gal;

  struct GalaxyData *sat_gal = alloc_galaxy();
  sat_gal->MergTime = 1.0f;
  sat_gal->StellarMass = 1.0;
  sat_gal->ColdGas = 1.0;
  struct Halo sat = {0};
  sat.Type = 1;
  sat.HaloNr = 3;
  sat.SnapNum = 63; /* Non-boundary */
  sat.dT = -1.0;    /* Invalid */
  sat.galaxy = sat_gal;

  struct ModuleContext ctx = create_context(0.2, 1);
  ctx.central_galaxy = &central;
  struct Halo halos[2] = {central, sat};
  const struct GalaxyData central_before = *cen_gal;
  const struct GalaxyData sat_before = *sat_gal;

  char log[2048];
  const int result = process_logged(sage26_resolve_mergers_and_disruption_process, &ctx, halos, 2,
                                    log, sizeof(log));
  TEST_ASSERT(result != 0, "Immediate handler should fail for non-boundary dT<=0");
  TEST_ASSERT(strstr(log, "Invalid merger-pass dt for halo 3 (SnapNum=63, dT=-1.000e+00") != NULL,
              "The error must name the halo, its SnapNum and its dT");
  TEST_ASSERT(memcmp(cen_gal, &central_before, sizeof(central_before)) == 0,
              "The central's galaxy must be unchanged");
  TEST_ASSERT(memcmp(sat_gal, &sat_before, sizeof(sat_before)) == 0,
              "The satellite's galaxy, its merger clock included, must be unchanged");

  free_galaxy(&cen_gal);
  free_galaxy(&sat_gal);
  sage26_resolve_mergers_and_disruption_cleanup();
  test_free_pre_timestep();
  check_memory_leaks();

  return TEST_PASS;
}

/* ========================================================================== */
/* PER-OBJECT DT: REINCORPORATION, STAR FORMATION, COOLING, RADIO-MODE HEATING */
/* ========================================================================== */

/**
 * @brief The mass the reincorporation module moves from the ejected reservoir for a central whose
 *        halo dT is `object_dt`, under a module context whose substep dt is `context_dt`
 *
 * @return 0 on success
 */
static int reincorporated_mass(const double object_dt, const double context_dt, double *response) {
  reset_config();
  set_test_model_parameters();
  add_test_cgm_recipe_parameter(); /* CGMrecipeOn: 0, required */
  if (sage26_reincorporation_init() != 0) {
    return -1;
  }
  struct GalaxyData *gal = alloc_galaxy();
  gal->EjectedGas = 10.0;
  gal->MetalsEjectedGas = 0.2;
  gal->HotGas = 5.0;
  gal->MetalsHotGas = 0.1;
  struct Halo halo = {0};
  halo.Type = 0;
  halo.Vvir = 500.0;
  halo.Rvir = 0.2;
  halo.SnapNum = 63;
  halo.dT = object_dt;
  halo.galaxy = gal;
  struct ModuleContext ctx = create_context(context_dt, 1);
  ctx.central_galaxy = &halo;

  const int status = sage26_reincorporation_process(&ctx, &halo, 1);
  *response = 10.0 - gal->EjectedGas;

  free_galaxy(&gal);
  sage26_reincorporation_cleanup();
  return status;
}

/** @brief The NewStellarMass the star-formation module forms; see reincorporated_mass() */
static int new_stellar_mass(const double object_dt, const double context_dt, double *response) {
  reset_config();
  set_test_model_parameters();
  /* The module's required SFprescription and H2 integration parameters, and the apply step its
   * init requires */
  add_test_sf_prescription_parameters(SAGE26_TEST_KEYS_SF_MODULE);
  test_phase_add("galaxy_physics", "sage26_apply_star_formation_supernova",
                 PROCESSING_MODE_BY_GALAXY);
  const int init_status = sage26_calculate_star_formation_init();
  test_free_substep_phases();
  if (init_status != 0) {
    return -1;
  }
  struct GalaxyData *gal = alloc_galaxy();
  gal->ColdGas = 5.0;
  gal->DiskScaleRadius = 0.01;
  struct Halo halo = {0};
  halo.Type = 0;
  halo.Vvir = 200.0;
  halo.SnapNum = 63;
  halo.dT = object_dt;
  halo.galaxy = gal;
  struct ModuleContext ctx = create_context(context_dt, 1);
  ctx.central_galaxy = &halo;

  const int status = sage26_calculate_star_formation_process(&ctx, &halo, 1);
  *response = gal->NewStellarMass;

  free_galaxy(&gal);
  sage26_calculate_star_formation_cleanup();
  return status;
}

/** @brief The CoolingGas the cooling-budget module forms; see reincorporated_mass() */
static int cooling_gas(const double object_dt, const double context_dt, double *response) {
  reset_config();
  /* Unit conversions needed by cooling_recipe() */
  MimicConfig.UnitDensity_in_cgs = 6.769911178294543e-22;
  MimicConfig.UnitTime_in_s = 3.08568e16;
  add_test_cgm_recipe_parameter(); /* CGMrecipeOn: 0, required */
  if (sage26_calculate_cooling_budget_init() != 0) {
    return -1;
  }
  struct GalaxyData *gal = alloc_galaxy();
  gal->HotGas = 10.0;
  gal->MetalsHotGas = 0.02; /* 0.2% metallicity */
  struct Halo halo = {0};
  halo.Type = 0;
  halo.Vvir = 200.0;
  halo.Rvir = 0.2;
  halo.SnapNum = 63;
  halo.dT = object_dt;
  halo.galaxy = gal;
  struct ModuleContext ctx = create_context(context_dt, 1);

  const int status = sage26_calculate_cooling_budget_process(&ctx, &halo, 1);
  *response = gal->CoolingGas;

  free_galaxy(&gal);
  sage26_calculate_cooling_budget_cleanup();
  return status;
}

/**
 * @brief The heating radius the radio mode forms, from the black-hole mass it accretes in the
 *        substep (Rheat starts at 0 and the accreted mass is proportional to the dt used); see
 *        reincorporated_mass()
 */
static int radio_heating_radius(const double object_dt, const double context_dt, double *response) {
  reset_config();
  /* Unit conversions and G needed by the Bondi rate */
  MimicConfig.UnitMass_in_g = 1.989e43;
  MimicConfig.UnitTime_in_s = 3.08568e16;
  MimicConfig.UnitEnergy_in_cgs = 1.989e53;
  MimicConfig.Hubble_h = 0.73;
  MimicConfig.G = 43.0071;
  /* AGNrecipe 2 (Bondi), the package accepts only 0 and 2 */
  snprintf(MimicConfig.ModelParams[0].param_name, MAX_STRING_LEN, "RadioModeEfficiency");
  snprintf(MimicConfig.ModelParams[0].value, MAX_STRING_LEN, "0.08");
  snprintf(MimicConfig.ModelParams[1].param_name, MAX_STRING_LEN, "AGNrecipe");
  snprintf(MimicConfig.ModelParams[1].value, MAX_STRING_LEN, "2");
  MimicConfig.NumModelParams = 2;
  if (sage26_radio_mode_heating_init() != 0) {
    return -1;
  }
  struct GalaxyData *gal = alloc_galaxy();
  gal->HotGas = 10.0;
  gal->MetalsHotGas = 0.2;
  gal->BlackHoleMass = 0.01;
  gal->CoolingGas = 1.0;
  gal->Rcool = 0.05;
  gal->Rheat = 0.0;
  /* A carried x small enough that neither the Eddington nor the heating cap binds, so the
   * accreted mass is proportional to the dt the module uses */
  gal->CoolingX = 1.0e-5;
  struct Halo halo = {0};
  halo.Type = 0;
  halo.Mvir = 100.0;
  halo.Vvir = 200.0;
  halo.Rvir = 0.2;
  halo.SnapNum = 63;
  halo.dT = object_dt;
  halo.galaxy = gal;
  struct ModuleContext ctx = create_context(context_dt, 1);

  const int status = sage26_radio_mode_heating_process(&ctx, &halo, 1);
  *response = gal->Rheat;

  free_galaxy(&gal);
  sage26_radio_mode_heating_cleanup();
  return status;
}

/**
 * @test    test_mixed_dt_uses_object_dt
 * @brief   Each module's result is proportional to its object's dT and independent of the
 *          context's substep dt
 *
 * Two identical objects with dT = small and dT = large run under the same context dt (0.9, equal
 * to neither): the ratio of their results is small/large only if the module takes its timestep
 * from the object. A third run of the large object under a different context dt (0.2) gives the
 * same result as under 0.9, so the context's value is not used. The dT values keep each result
 * below the cap that would make it independent of dt (the ejected gas, the hot gas, the Eddington
 * and heating limits).
 */
int test_mixed_dt_uses_object_dt(void) {
  static const struct {
    const char *name;
    int (*response)(double object_dt, double context_dt, double *response);
    double small_dt;
    double large_dt;
    double tolerance; /* on the ratio: the float rounding of the fields the response is read from */
  } rows[] = {
      {"reincorporation", reincorporated_mass, 1.0e-4, 3.0e-4, 1e-5},
      {"star formation", new_stellar_mass, 0.1, 0.3, 1e-6},
      {"cooling", cooling_gas, 1.0e-4, 3.0e-4, 1e-5},
      {"radio-mode heating", radio_heating_radius, 0.1, 0.3, 1e-6},
  };
  const double fixed_context_dt = 0.9;
  const double other_context_dt = 0.2;

  for (size_t r = 0; r < COUNT(rows); r++) {
    init_memory_system(0);
    double small = 0.0;
    double large = 0.0;
    double large_other_context = 0.0;
    int status = rows[r].response(rows[r].small_dt, fixed_context_dt, &small);
    status |= rows[r].response(rows[r].large_dt, fixed_context_dt, &large);
    status |= rows[r].response(rows[r].large_dt, other_context_dt, &large_other_context);
    printf("\n  %s: small %.9g, large %.9g, large under another context dt %.9g", rows[r].name,
           small, large, large_other_context);

    char message[256];
    snprintf(message, sizeof(message), "%s: the module must initialise and run", rows[r].name);
    TEST_ASSERT(status == 0, message);
    snprintf(message, sizeof(message), "%s: every response must be finite", rows[r].name);
    TEST_ASSERT(isfinite(small) && isfinite(large) && isfinite(large_other_context), message);
    snprintf(message, sizeof(message), "%s: both objects must respond (a zero is a cap or a no-op)",
             rows[r].name);
    TEST_ASSERT(small > 0.0 && large > 0.0, message);
    snprintf(message, sizeof(message),
             "%s: the result must scale with the object's dT under one context dt", rows[r].name);
    TEST_ASSERT_DOUBLE_EQUAL(small / large, rows[r].small_dt / rows[r].large_dt, rows[r].tolerance,
                             message);
    snprintf(message, sizeof(message), "%s: the result must not depend on the context's dt",
             rows[r].name);
    TEST_ASSERT(large == large_other_context, message);
    check_memory_leaks();
  }
  return TEST_PASS;
}

/* ========================================================================== */
/* REINCORPORATION: BOUNDARY AND INVALID DT                                  */
/* ========================================================================== */

/**
 * @test    test_boundary_sentinel_reincorporation_noop
 * @brief   Boundary sentinel (SnapNum<0,dT<=0) is a deterministic no-op
 */
int test_boundary_sentinel_reincorporation_noop(void) {
  init_memory_system(0);
  reset_config();
  set_test_model_parameters();
  add_test_cgm_recipe_parameter(); /* CGMrecipeOn: 0, required */
  sage26_reincorporation_init();

  struct GalaxyData *gal = alloc_galaxy();
  gal->EjectedGas = 8.0;
  gal->MetalsEjectedGas = 0.16;
  gal->HotGas = 2.0;
  gal->MetalsHotGas = 0.04;

  struct Halo halo = {0};
  halo.Type = 0;
  halo.Vvir = 500.0;
  halo.Rvir = 0.2;
  halo.SnapNum = -1; /* Boundary sentinel state */
  halo.dT = -1.0;
  halo.galaxy = gal;

  struct ModuleContext ctx = create_context(0.2, 1);
  ctx.central_galaxy = &halo;

  const float initial_ejected = gal->EjectedGas;
  const float initial_hot = gal->HotGas;
  const int result = sage26_reincorporation_process(&ctx, &halo, 1);

  TEST_ASSERT(result == 0, "Boundary sentinel should be a no-op success");
  TEST_ASSERT_DOUBLE_EQUAL(gal->EjectedGas, initial_ejected, 1e-10,
                           "Boundary sentinel should not change EjectedGas");
  TEST_ASSERT_DOUBLE_EQUAL(gal->HotGas, initial_hot, 1e-10,
                           "Boundary sentinel should not change HotGas");

  free_galaxy(&gal);
  sage26_reincorporation_cleanup();
  check_memory_leaks();

  return TEST_PASS;
}

/**
 * @test    test_invalid_nonboundary_reincorporation_error
 * @brief   Non-boundary dT<=0 in reincorporation fails fast, names the halo and its dT, and
 *          leaves the ejected and hot gas as they were
 */
int test_invalid_nonboundary_reincorporation_error(void) {
  init_memory_system(0);
  reset_config();
  set_test_model_parameters();
  add_test_cgm_recipe_parameter(); /* CGMrecipeOn: 0, required */
  TEST_ASSERT(sage26_reincorporation_init() == 0, "reincorporation must initialise");

  struct GalaxyData *gal = alloc_galaxy();
  gal->EjectedGas = 8.0;
  gal->MetalsEjectedGas = 0.16;
  gal->HotGas = 2.0;
  gal->MetalsHotGas = 0.04;

  struct Halo halo = {0};
  halo.Type = 0;
  halo.Vvir = 500.0;
  halo.Rvir = 0.2;
  halo.SnapNum = 63; /* Non-boundary */
  halo.dT = -1.0;    /* Invalid */
  halo.galaxy = gal;

  struct ModuleContext ctx = create_context(0.2, 1);
  ctx.central_galaxy = &halo;

  char log[2048];
  const int result =
      process_logged(sage26_reincorporation_process, &ctx, &halo, 1, log, sizeof(log));
  TEST_ASSERT(result != 0, "Non-boundary dT<=0 should fail fast");
  TEST_ASSERT(strstr(log, "Invalid reincorporation dt for halo 0 (SnapNum=63, dT=-1.000e+00") !=
                  NULL,
              "The error must name the halo, its SnapNum and its dT");
  TEST_ASSERT(gal->EjectedGas == 8.0f && gal->MetalsEjectedGas == 0.16f && gal->HotGas == 2.0f &&
                  gal->MetalsHotGas == 0.04f,
              "The ejected and hot reservoirs must be unchanged");

  free_galaxy(&gal);
  sage26_reincorporation_cleanup();
  check_memory_leaks();

  return TEST_PASS;
}

/* ========================================================================== */
/* MAIN TEST RUNNER                                                          */
/* ========================================================================== */

int main(void) {
  initialize_error_handling(LOG_LEVEL_WARNING, NULL);

  printf("%s", BLUE);
  printf("============================================================\n");
  printf("Test Suite: Mixed-dT SAGE Parity\n");
  printf("============================================================\n");
  printf("%s\n", NC);

  printf("\n%sMERGER TIME TESTS:%s\n", BLUE, NC);
  TEST_RUN(test_mixed_dt_merger_timestamp_source_time);
  TEST_RUN(test_invalid_nonboundary_merger_time_error);

  printf("\n%sPER-OBJECT DT TESTS:%s\n", BLUE, NC);
  TEST_RUN(test_mixed_dt_uses_object_dt);

  printf("\n%sREINCORPORATION BOUNDARY TESTS:%s\n", BLUE, NC);
  TEST_RUN(test_boundary_sentinel_reincorporation_noop);
  TEST_RUN(test_invalid_nonboundary_reincorporation_error);

  TEST_SUMMARY();
  return TEST_RESULT();
}
