/**
 * @file    test_unit_sage_merger_ordering_parity.c
 * @brief   Immediate-order parity fixtures for SAGE merger/disruption ordering
 *
 * These tests exercise the dedicated immediate-order parity handler against the
 * live SAGE ordering contract documented in the module README.
 */

#include "../../../../tests/framework/parity_trace.h"
#include "../../../../tests/framework/test_framework.h"
#include "../../../core/module_interface.h"
#include "../../../include/globals.h"
#include "../../../include/types.h"
#include "../../../util/error.h"
#include "../../../util/memory.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int passed = 0;
static int failed = 0;

extern int sage_resolve_mergers_and_disruption_init(void);
extern int sage_resolve_mergers_and_disruption_process(struct ModuleContext *ctx,
                                                 struct Halo *halos, int ngal);
extern int sage_resolve_mergers_and_disruption_cleanup(void);
extern void sage_resolve_mergers_and_disruption_set_action_hook(
    void (*hook)(const char *action, int source_index, int target_index,
                 double mass_ratio));

static const double TEST_METALLICITY = 0.02;
static const double TEST_THRESHOLD_MAJOR = 0.3;
static const double TEST_THRESHOLD_DISRUPTION = 1.0;

static struct ParityTrace *active_trace = NULL;

static void init_unit_constants(void) {
  UnitLength_in_cm = 3.08568e24;
  UnitVelocity_in_cm_per_s = 1.0e5;
  UnitMass_in_g = 1.989e43;

  UnitTime_in_s = UnitLength_in_cm / UnitVelocity_in_cm_per_s;
  UnitEnergy_in_cgs = UnitMass_in_g * UnitLength_in_cm * UnitLength_in_cm /
                      (UnitTime_in_s * UnitTime_in_s);
}

static void reset_config(void) {
  memset(&MimicConfig, 0, sizeof(MimicConfig));
  init_unit_constants();
  MimicConfig.Hubble_h = 0.73;
  MimicConfig.G = 43.02;
  MimicConfig.UnitVelocity_in_cm_per_s = 1.0e5;
}

static void setup_model_parameters(double threshold_sat_disruption,
                                   double threshold_major_merger) {
  int idx = 0;

  snprintf(MimicConfig.ModelParams[idx].param_name, MAX_STRING_LEN,
           "ThresholdSatDisruption");
  snprintf(MimicConfig.ModelParams[idx++].value, MAX_STRING_LEN, "%.6f",
           threshold_sat_disruption);
  snprintf(MimicConfig.ModelParams[idx].param_name, MAX_STRING_LEN,
           "ThresholdMajorMerger");
  snprintf(MimicConfig.ModelParams[idx++].value, MAX_STRING_LEN, "%.6f",
           threshold_major_merger);

  MimicConfig.NumModelParams = idx;
}

static void setup_context(struct ModuleContext *ctx) {
  memset(ctx, 0, sizeof(*ctx));
  ctx->redshift = 0.0;
  ctx->time = 13.8;
  ctx->substep_time = 13.75;
  ctx->snapshot_number = 63;
  ctx->substep_number = 0;
  ctx->num_substeps = 1;
  ctx->time_interval = 0.1;
  ctx->substep_dt = 0.1;
  ctx->params = &MimicConfig;
}

static void setup_test_halo(struct Halo *halo, struct GalaxyData *galaxy,
                            long long halo_nr, int type, int central_halo,
                            double mvir, double delta_mvir, double vvir,
                            double merg_time, double stellar_mass,
                            double cold_gas, double hot_gas,
                            double ejected_gas, double ics, double bulge_mass) {
  memset(halo, 0, sizeof(*halo));
  memset(galaxy, 0, sizeof(*galaxy));

  halo->Type = type;
  halo->CentralHalo = central_halo;
  halo->Mvir = mvir;
  halo->deltaMvir = delta_mvir;
  halo->Rvir = 0.2;
  halo->Vvir = vvir;
  halo->SnapNum = 63;
  halo->HaloNr = halo_nr;
  halo->dT = 0.1;
  halo->galaxy = galaxy;

  galaxy->MergTime = merg_time;
  galaxy->StellarMass = stellar_mass;
  galaxy->MetalsStellarMass = stellar_mass * TEST_METALLICITY;
  galaxy->ColdGas = cold_gas;
  galaxy->MetalsColdGas = cold_gas * TEST_METALLICITY;
  galaxy->HotGas = hot_gas;
  galaxy->MetalsHotGas = hot_gas * TEST_METALLICITY;
  galaxy->EjectedGas = ejected_gas;
  galaxy->MetalsEjectedGas = ejected_gas * TEST_METALLICITY;
  galaxy->ICS = ics;
  galaxy->MetalsICS = ics * TEST_METALLICITY;
  galaxy->BulgeMass = bulge_mass;
  galaxy->MetalsBulgeMass = bulge_mass * TEST_METALLICITY;
  galaxy->TimeOfLastMajorMerger = -1.0;
  galaxy->TimeOfLastMinorMerger = -1.0;
}

static double calculate_mass_ratio(double lhs_mass, double rhs_mass) {
  const double smaller = (lhs_mass < rhs_mass) ? lhs_mass : rhs_mass;
  const double larger = (lhs_mass < rhs_mass) ? rhs_mass : lhs_mass;
  return (larger > 0.0) ? smaller / larger : 1.0;
}

static const char *classify_merger(double ratio) {
  return (ratio > TEST_THRESHOLD_MAJOR) ? "major" : "minor";
}

static void record_immediate_action(const char *action, int source_index,
                                    int target_index, double mass_ratio) {
  if (active_trace == NULL || action == NULL) {
    return;
  }

  if (strcmp(action, "merge") == 0) {
    parity_trace_append(active_trace, "%02d|merge|src=%d|dst=%d|%s",
                        active_trace->count + 1, source_index, target_index,
                        classify_merger(mass_ratio));
    return;
  }

  if (strcmp(action, "disrupt") == 0) {
    parity_trace_append(active_trace, "%02d|disrupt|src=%d|dst=%d",
                        active_trace->count + 1, source_index, target_index);
  }
}

static int init_parity_modules(void) {
  if (sage_resolve_mergers_and_disruption_init() != 0) {
    return -1;
  }

  sage_resolve_mergers_and_disruption_set_action_hook(record_immediate_action);
  return 0;
}

static void cleanup_parity_modules(void) {
  sage_resolve_mergers_and_disruption_set_action_hook(NULL);
  sage_resolve_mergers_and_disruption_cleanup();
  active_trace = NULL;
}

static int trace_matches_contract(const char *label,
                                  const struct ParityTrace *expected,
                                  const struct ParityTrace *actual) {
  char expected_buf[PARITY_TRACE_MAX_LINES * PARITY_TRACE_LINE_LEN];
  char actual_buf[PARITY_TRACE_MAX_LINES * PARITY_TRACE_LINE_LEN];

  if (parity_trace_equal(expected, actual)) {
    return 0;
  }

  parity_trace_render(expected, expected_buf, sizeof(expected_buf));
  parity_trace_render(actual, actual_buf, sizeof(actual_buf));

  fprintf(stderr, "FAIL: %s\n", label);
  fprintf(stderr, "  Expected trace:\n%s\n", expected_buf);
  fprintf(stderr, "  Actual trace:\n%s\n", actual_buf);
  return 1;
}

static int state_matches_contract(const char *label, double expected_stellar,
                                  double expected_bulge,
                                  const struct GalaxyData *actual_target,
                                  const struct GalaxyData *actual_fof) {
  if (fabs(actual_target->StellarMass - expected_stellar) <= 1e-6 &&
      fabs(actual_target->BulgeMass - expected_bulge) <= 1e-6) {
    return 0;
  }

  fprintf(stderr, "FAIL: %s\n", label);
  fprintf(stderr,
          "  Expected redirect target StellarMass=%.3f BulgeMass=%.3f\n",
          expected_stellar, expected_bulge);
  fprintf(stderr,
          "  Actual redirect target StellarMass=%.3f BulgeMass=%.3f\n",
          actual_target->StellarMass, actual_target->BulgeMass);
  fprintf(stderr,
          "  Actual FOF target      StellarMass=%.3f BulgeMass=%.3f\n",
          actual_fof->StellarMass, actual_fof->BulgeMass);
  return 1;
}

static void setup_shared_target_fixture(struct ModuleContext *ctx,
                                        struct Halo halos[3],
                                        struct GalaxyData galaxies[3]) {
  setup_context(ctx);

  setup_test_halo(&halos[0], &galaxies[0], 1000, 0, -1, 50.0, 0.0, 220.0,
                  999.9, 10.0, 5.0, 4.0, 1.0, 0.5, 2.0);
  setup_test_halo(&halos[1], &galaxies[1], 1001, 1, 0, 1.0, 0.0, 80.0, 0.4,
                  2.0, 1.0, 1.0, 0.2, 0.1, 0.2);
  setup_test_halo(&halos[2], &galaxies[2], 1002, 1, 0, 1.0, 0.0, 90.0, -0.1,
                  1.5, 0.5, 0.2, 0.1, 0.0, 0.1);

  ctx->central_galaxy = &halos[0];
}

static void setup_consumed_target_fixture(struct ModuleContext *ctx,
                                          struct Halo halos[4],
                                          struct GalaxyData galaxies[4]) {
  setup_context(ctx);

  setup_test_halo(&halos[0], &galaxies[0], 2000, 0, -1, 80.0, 0.0, 240.0,
                  999.9, 18.0, 2.0, 6.0, 1.0, 0.2, 3.0);

  /*
   * Type 1 satellites ignore CentralHalo in the split modules. This fixture
   * uses halos[1].CentralHalo as the SAGE-style one-hop redirect destination
   * that should be followed after halos[1] is consumed.
   */
  setup_test_halo(&halos[1], &galaxies[1], 2001, 1, 2, 1.0, 0.0, 120.0, -0.1,
                  2.0, 1.0, 0.5, 0.1, 0.0, 0.3);
  setup_test_halo(&halos[2], &galaxies[2], 2002, 1, 0, 20.0, 0.0, 100.0, 5.0,
                  1.2, 0.3, 0.3, 0.1, 0.0, 0.2);
  setup_test_halo(&halos[3], &galaxies[3], 2003, 2, 1, 0.5, 0.0, 70.0, -0.1,
                  1.2, 0.6, 0.2, 0.1, 0.0, 0.1);

  ctx->central_galaxy = &halos[0];
}

int test_shared_target_immediate_trace(void) {
  struct ModuleContext ctx;
  struct Halo halos[3];
  struct GalaxyData galaxies[3];
  struct ParityTrace expected_trace;
  struct ParityTrace actual_trace;
  const double merge_ratio = calculate_mass_ratio(2.0, 15.0);
  int parity_failure = 0;

  printf("  Testing: immediate-order shared merge/disrupt target trace...\n");

  init_memory_system(0);
  reset_config();
  setup_model_parameters(TEST_THRESHOLD_DISRUPTION, TEST_THRESHOLD_MAJOR);
  TEST_ASSERT(init_parity_modules() == 0,
              "Parity module should initialize for shared-target fixture");

  setup_shared_target_fixture(&ctx, halos, galaxies);

  parity_trace_reset(&expected_trace);
  parity_trace_reset(&actual_trace);
  active_trace = &actual_trace;

  parity_trace_append(&expected_trace, "01|disrupt|src=1|dst=0");
  parity_trace_append(&expected_trace, "02|merge|src=2|dst=0|%s",
                      classify_merger(merge_ratio));

  TEST_ASSERT(sage_resolve_mergers_and_disruption_process(&ctx, halos, 3) == 0,
              "Immediate parity module should succeed");
  TEST_ASSERT(halos[1].Type == 3,
              "Immediate path should consume the disrupted satellite");
  TEST_ASSERT(halos[2].Type == 3,
              "Immediate path should consume the merger satellite");

  parity_failure = trace_matches_contract(
      "Shared-target fixture should match SAGE immediate ordering",
      &expected_trace, &actual_trace);

  cleanup_parity_modules();
  check_memory_leaks();

  return parity_failure ? TEST_FAIL : TEST_PASS;
}

int test_consumed_target_redirect_trace(void) {
  struct ModuleContext ctx;
  struct Halo halos[4];
  struct GalaxyData galaxies[4];
  struct ParityTrace expected_trace;
  struct ParityTrace actual_trace;
  const double first_ratio = calculate_mass_ratio(3.0, 20.0);
  const double expected_second_ratio = calculate_mass_ratio(1.8, 1.5);
  int parity_failure = 0;

  printf("  Testing: consumed Type 2 target redirect trace...\n");

  init_memory_system(0);
  reset_config();
  setup_model_parameters(TEST_THRESHOLD_DISRUPTION, TEST_THRESHOLD_MAJOR);
  TEST_ASSERT(init_parity_modules() == 0,
              "Parity module should initialize for redirect fixture");

  setup_consumed_target_fixture(&ctx, halos, galaxies);

  parity_trace_reset(&expected_trace);
  parity_trace_reset(&actual_trace);
  active_trace = &actual_trace;

  parity_trace_append(&expected_trace, "01|merge|src=1|dst=0|%s",
                      classify_merger(first_ratio));
  parity_trace_append(&expected_trace, "02|merge|src=3|dst=2|%s",
                      classify_merger(expected_second_ratio));

  TEST_ASSERT(sage_resolve_mergers_and_disruption_process(&ctx, halos, 4) == 0,
              "Immediate parity module should succeed");
  TEST_ASSERT(halos[1].Type == 3,
              "First merger should consume the intermediate target");
  TEST_ASSERT(halos[3].Type == 3,
              "Second merger should consume the Type 2 orphan");

  parity_failure = trace_matches_contract(
      "Consumed-target redirect fixture should match SAGE one-hop redirect",
      &expected_trace, &actual_trace);

  cleanup_parity_modules();
  check_memory_leaks();

  return parity_failure ? TEST_FAIL : TEST_PASS;
}

int test_consumed_target_redirect_changes_major_minor_state(void) {
  struct ModuleContext ctx;
  struct Halo halos[4];
  struct GalaxyData galaxies[4];
  const double expected_target2_stellar = 2.4;
  const double expected_target2_bulge = 2.4;
  int parity_failure = 0;

  printf("  Testing: consumed target redirect changes major/minor outcome...\n");

  init_memory_system(0);
  reset_config();
  setup_model_parameters(TEST_THRESHOLD_DISRUPTION, TEST_THRESHOLD_MAJOR);
  TEST_ASSERT(init_parity_modules() == 0,
              "Parity module should initialize for state fixture");

  setup_consumed_target_fixture(&ctx, halos, galaxies);

  TEST_ASSERT(sage_resolve_mergers_and_disruption_process(&ctx, halos, 4) == 0,
              "Immediate parity module should succeed");
  TEST_ASSERT(halos[3].Type == 3,
              "Type 2 orphan should be consumed in the immediate merge");

  parity_failure = state_matches_contract(
      "SAGE redirect should deliver a major merger remnant to halo 2",
      expected_target2_stellar, expected_target2_bulge, halos[2].galaxy,
      halos[0].galaxy);

  cleanup_parity_modules();
  check_memory_leaks();

  return parity_failure ? TEST_FAIL : TEST_PASS;
}

int main(void) {
  initialize_error_handling(LOG_LEVEL_WARNING, NULL);

  printf("%s", BLUE);
  printf("============================================================\n");
  printf("Unit Test Suite: SAGE Merger Ordering Parity\n");
  printf("============================================================\n");
  printf("%s", NC);

  TEST_RUN(test_shared_target_immediate_trace);
  TEST_RUN(test_consumed_target_redirect_trace);
  TEST_RUN(test_consumed_target_redirect_changes_major_minor_state);

  TEST_SUMMARY();
  return TEST_RESULT();
}
