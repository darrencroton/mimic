/**
 * @file    test_unit_sage26_merger_ordering_parity.c
 * @brief   Immediate-order parity fixtures for SAGE merger/disruption ordering
 *
 * These tests exercise the dedicated immediate-order parity handler against the live SAGE ordering
 * contract documented in the module README, and the merger transaction's refusal of BulgeSizeOn 3
 * without the core parameters. The CGMrecipeOn 1 tests check, with exact literal expectations, that
 * a disruption deposits the satellite's gas in the PASSED recipient's selected pair, chosen by that
 * recipient's Regime with CGMrecipeOn 1 and its hot pair otherwise, for an orphan whose
 * recipient is not the FoF central (test_cgm_disruption_to_passed_recipient), and the
 * CGMrecipeOn refusal (test_cgm_recipe_on_refused).
 */

#include "../../../../tests/framework/parity_trace.h"
#include "../../../../tests/framework/test_framework.h"
#include "../../../../tests/framework/test_phase_config.h"
#include "core/module_interface.h"
#include "include/globals.h"
#include "include/types.h"
#include "util/error.h"
#include "util/memory.h"

#include "modules/sage26_resolve_mergers_and_disruption/sage_merger_ops.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int passed = 0;
static int failed = 0;

extern int sage26_resolve_mergers_and_disruption_init(void);
extern int sage26_resolve_mergers_and_disruption_process(struct ModuleContext *ctx,
                                                         struct Halo *halos, int ngal);
extern int sage26_resolve_mergers_and_disruption_cleanup(void);
extern void sage26_resolve_mergers_and_disruption_set_action_hook(
    void (*hook)(const char *action, int source_index, int target_index, double mass_ratio));

static const double TEST_METALLICITY = 0.02;
static const double TEST_THRESHOLD_MAJOR = 0.3;
static const double TEST_THRESHOLD_DISRUPTION = 1.0;

static struct ParityTrace *active_trace = NULL;

static void init_unit_constants(void) {
  MimicConfig.UnitLength_in_cm = 3.08568e24;
  MimicConfig.UnitVelocity_in_cm_per_s = 1.0e5;
  MimicConfig.UnitMass_in_g = 1.989e43;

  MimicConfig.UnitTime_in_s = MimicConfig.UnitLength_in_cm / MimicConfig.UnitVelocity_in_cm_per_s;
  MimicConfig.UnitEnergy_in_cgs = MimicConfig.UnitMass_in_g * MimicConfig.UnitLength_in_cm *
                                  MimicConfig.UnitLength_in_cm /
                                  (MimicConfig.UnitTime_in_s * MimicConfig.UnitTime_in_s);
}

static void reset_config(void) {
  memset(&MimicConfig, 0, sizeof(MimicConfig));
  init_unit_constants();
  MimicConfig.Hubble_h = 0.73;
  MimicConfig.G = 43.02;
  MimicConfig.UnitVelocity_in_cm_per_s = 1.0e5;
}

static void setup_model_parameters_with_sizes(double threshold_sat_disruption,
                                              double threshold_major_merger, int bulge_size_on) {
  int idx = 0;

  snprintf(MimicConfig.ModelParams[idx].param_name, MAX_STRING_LEN, "ThresholdSatDisruption");
  snprintf(MimicConfig.ModelParams[idx++].value, MAX_STRING_LEN, "%.6f", threshold_sat_disruption);
  snprintf(MimicConfig.ModelParams[idx].param_name, MAX_STRING_LEN, "ThresholdMajorMerger");
  snprintf(MimicConfig.ModelParams[idx++].value, MAX_STRING_LEN, "%.6f", threshold_major_merger);
  /* The required bulge-size mode */
  snprintf(MimicConfig.ModelParams[idx].param_name, MAX_STRING_LEN, "BulgeSizeOn");
  snprintf(MimicConfig.ModelParams[idx++].value, MAX_STRING_LEN, "%d", bulge_size_on);
  /* The required CGMrecipeOn, at R3's value 0 */
  snprintf(MimicConfig.ModelParams[idx].param_name, MAX_STRING_LEN, "CGMrecipeOn");
  snprintf(MimicConfig.ModelParams[idx++].value, MAX_STRING_LEN, "%d", 0);

  MimicConfig.NumModelParams = idx;
}

/** @brief The ordering fixtures' parameters, with SAGE26's R0 bulge-size mode 0 (no sizes) */
static void setup_model_parameters(double threshold_sat_disruption, double threshold_major_merger) {
  setup_model_parameters_with_sizes(threshold_sat_disruption, threshold_major_merger, 0);
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

static void setup_test_halo(struct Halo *halo, struct GalaxyData *galaxy, long long halo_nr,
                            int type, int central_halo, double mvir, double delta_mvir, double vvir,
                            double merg_time, double stellar_mass, double cold_gas, double hot_gas,
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

static void record_immediate_action(const char *action, int source_index, int target_index,
                                    double mass_ratio) {
  if (active_trace == NULL || action == NULL) {
    return;
  }

  if (strcmp(action, "merge") == 0) {
    parity_trace_append(active_trace, "%02d|merge|src=%d|dst=%d|%s", active_trace->count + 1,
                        source_index, target_index, classify_merger(mass_ratio));
    return;
  }

  if (strcmp(action, "disrupt") == 0) {
    parity_trace_append(active_trace, "%02d|disrupt|src=%d|dst=%d", active_trace->count + 1,
                        source_index, target_index);
  }
}

static int init_parity_modules(void) {
  test_pre_timestep_add("sage26_initialise_merger_clock", PROCESSING_MODE_FULL_HALO);
  if (sage26_resolve_mergers_and_disruption_init() != 0) {
    return -1;
  }

  sage26_resolve_mergers_and_disruption_set_action_hook(record_immediate_action);
  return 0;
}

static void cleanup_parity_modules(void) {
  sage26_resolve_mergers_and_disruption_set_action_hook(NULL);
  sage26_resolve_mergers_and_disruption_cleanup();
  test_free_pre_timestep();
  active_trace = NULL;
}

static int trace_matches_contract(const char *label, const struct ParityTrace *expected,
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

static int state_matches_contract(const char *label, double expected_stellar, double expected_bulge,
                                  const struct GalaxyData *actual_target,
                                  const struct GalaxyData *actual_fof) {
  if (fabs(actual_target->StellarMass - expected_stellar) <= 1e-6 &&
      fabs(actual_target->BulgeMass - expected_bulge) <= 1e-6) {
    return 0;
  }

  fprintf(stderr, "FAIL: %s\n", label);
  fprintf(stderr, "  Expected redirect target StellarMass=%.3f BulgeMass=%.3f\n", expected_stellar,
          expected_bulge);
  fprintf(stderr, "  Actual redirect target StellarMass=%.3f BulgeMass=%.3f\n",
          actual_target->StellarMass, actual_target->BulgeMass);
  fprintf(stderr, "  Actual FOF target      StellarMass=%.3f BulgeMass=%.3f\n",
          actual_fof->StellarMass, actual_fof->BulgeMass);
  return 1;
}

static void setup_shared_target_fixture(struct ModuleContext *ctx, struct Halo halos[3],
                                        struct GalaxyData galaxies[3]) {
  setup_context(ctx);

  setup_test_halo(&halos[0], &galaxies[0], 1000, 0, -1, 50.0, 0.0, 220.0, 999.9, 10.0, 5.0, 4.0,
                  1.0, 0.5, 2.0);
  setup_test_halo(&halos[1], &galaxies[1], 1001, 1, 0, 1.0, 0.0, 80.0, 0.4, 2.0, 1.0, 1.0, 0.2, 0.1,
                  0.2);
  setup_test_halo(&halos[2], &galaxies[2], 1002, 1, 0, 1.0, 0.0, 90.0, -0.1, 1.5, 0.5, 0.2, 0.1,
                  0.0, 0.1);

  ctx->central_galaxy = &halos[0];
}

static void setup_consumed_target_fixture(struct ModuleContext *ctx, struct Halo halos[4],
                                          struct GalaxyData galaxies[4]) {
  setup_context(ctx);

  setup_test_halo(&halos[0], &galaxies[0], 2000, 0, -1, 80.0, 0.0, 240.0, 999.9, 18.0, 2.0, 6.0,
                  1.0, 0.2, 3.0);

  /*
   * Type 1 satellites ignore CentralHalo in the split modules. This fixture
   * uses halos[1].CentralHalo as the SAGE-style one-hop redirect destination
   * that should be followed after halos[1] is consumed.
   */
  setup_test_halo(&halos[1], &galaxies[1], 2001, 1, 2, 1.0, 0.0, 120.0, -0.1, 2.0, 1.0, 0.5, 0.1,
                  0.0, 0.3);
  setup_test_halo(&halos[2], &galaxies[2], 2002, 1, 0, 20.0, 0.0, 100.0, 5.0, 1.2, 0.3, 0.3, 0.1,
                  0.0, 0.2);
  setup_test_halo(&halos[3], &galaxies[3], 2003, 2, 1, 0.5, 0.0, 70.0, -0.1, 1.2, 0.6, 0.2, 0.1,
                  0.0, 0.1);

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
  parity_trace_append(&expected_trace, "02|merge|src=2|dst=0|%s", classify_merger(merge_ratio));

  TEST_ASSERT(sage26_resolve_mergers_and_disruption_process(&ctx, halos, 3) == 0,
              "Immediate parity module should succeed");
  TEST_ASSERT(halos[1].Type == 3, "Immediate path should consume the disrupted satellite");
  TEST_ASSERT(halos[2].Type == 3, "Immediate path should consume the merger satellite");

  parity_failure = trace_matches_contract(
      "Shared-target fixture should match SAGE immediate ordering", &expected_trace, &actual_trace);

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
  TEST_ASSERT(init_parity_modules() == 0, "Parity module should initialize for redirect fixture");

  setup_consumed_target_fixture(&ctx, halos, galaxies);

  parity_trace_reset(&expected_trace);
  parity_trace_reset(&actual_trace);
  active_trace = &actual_trace;

  parity_trace_append(&expected_trace, "01|merge|src=1|dst=0|%s", classify_merger(first_ratio));
  parity_trace_append(&expected_trace, "02|merge|src=3|dst=2|%s",
                      classify_merger(expected_second_ratio));

  TEST_ASSERT(sage26_resolve_mergers_and_disruption_process(&ctx, halos, 4) == 0,
              "Immediate parity module should succeed");
  TEST_ASSERT(halos[1].Type == 3, "First merger should consume the intermediate target");
  TEST_ASSERT(halos[3].Type == 3, "Second merger should consume the Type 2 orphan");

  parity_failure =
      trace_matches_contract("Consumed-target redirect fixture should match SAGE one-hop redirect",
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
  TEST_ASSERT(init_parity_modules() == 0, "Parity module should initialize for state fixture");

  setup_consumed_target_fixture(&ctx, halos, galaxies);

  TEST_ASSERT(sage26_resolve_mergers_and_disruption_process(&ctx, halos, 4) == 0,
              "Immediate parity module should succeed");
  TEST_ASSERT(halos[3].Type == 3, "Type 2 orphan should be consumed in the immediate merge");

  parity_failure = state_matches_contract(
      "SAGE redirect should deliver a major merger remnant to halo 2", expected_target2_stellar,
      expected_target2_bulge, halos[2].galaxy, halos[0].galaxy);

  cleanup_parity_modules();
  check_memory_leaks();

  return parity_failure ? TEST_FAIL : TEST_PASS;
}

/* Merger events the refusal test's sink received */
static int refusal_emits = 0;

static int count_emit(struct ModuleContext *ctx, struct Halo *halos, int source_idx, int target_idx,
                      double mass_ratio, double source_dt) {
  (void)ctx;
  (void)halos;
  (void)source_idx;
  (void)target_idx;
  (void)mass_ratio;
  (void)source_dt;
  refusal_emits++;
  return 0;
}

/**
 * @test    test_mode3_merge_refuses_missing_core_parameters
 * @brief   With BulgeSizeOn 3 the merger transaction needs the core's Hubble_h
 *          (R/src/model_misc.c:249, :398): a context without parameters is refused with -1
 *          before any galaxy or halo changes and before the merger event is emitted. The same
 *          call with the parameters runs, so the refusal is the mode-3 guard's.
 */
int test_mode3_merge_refuses_missing_core_parameters(void) {
  struct ModuleContext ctx;
  struct Halo halos[3], halos_before[3];
  struct GalaxyData galaxies[3], galaxies_before[3];

  reset_config();
  setup_model_parameters_with_sizes(TEST_THRESHOLD_DISRUPTION, TEST_THRESHOLD_MAJOR, 3);
  TEST_ASSERT(init_parity_modules() == 0, "the resolver must initialise with BulgeSizeOn 3");

  setup_shared_target_fixture(&ctx, halos, galaxies);
  memcpy(halos_before, halos, sizeof(halos));
  memcpy(galaxies_before, galaxies, sizeof(galaxies));

  ctx.params = NULL;
  refusal_emits = 0;
  struct MimicSageMergerLocals locals;
  const int status = sage26_resolve_mergers_and_disruption_merge(&ctx, halos, 3, 1, 0, 13.75, 0.1,
                                                                 count_emit, &locals, NULL);
  const int galaxies_unchanged = memcmp(galaxies, galaxies_before, sizeof(galaxies)) == 0;
  const int halos_unchanged = memcmp(halos, halos_before, sizeof(halos)) == 0;
  const int emitted_when_refused = refusal_emits;

  ctx.params = &MimicConfig;
  const int status_with_params = sage26_resolve_mergers_and_disruption_merge(
      &ctx, halos, 3, 1, 0, 13.75, 0.1, count_emit, &locals, NULL);

  cleanup_parity_modules();
  check_memory_leaks();

  TEST_ASSERT(status == -1, "BulgeSizeOn 3 without ctx->params must be refused with -1");
  TEST_ASSERT(galaxies_unchanged, "the refused merger must leave the target and satellite as "
                                  "they were");
  TEST_ASSERT(halos_unchanged, "the refused merger must leave every halo as it was");
  TEST_ASSERT(emitted_when_refused == 0, "the refused merger must not emit its event");
  TEST_ASSERT(status_with_params == 0, "the same merger with the core parameters must run");
  return TEST_PASS;
}

// ============================================================================
// CGM ROUTING TESTS (CGMrecipeOn)
// ============================================================================
//
// SAGE26's disrupt_satellite_to_ICS (R = the staged SAGE26 source, R/src/model_mergers.c:845-902)
// sums the satellite's ColdGas + HotGas + CGMgas, and their metals, in float, widens each sum and
// deposits it through add_gas_to_hot_reservoir (R/src/model_misc.h:76-85) on the PASSED recipient:
// its CGM pair with CGMrecipeOn 1 and the recipient's Regime 0, its hot pair otherwise. The pass
// passes merger_centralgal, which for a Type 2 orphan is its subhalo's central
// (R/src/core_build_model.c:539-543, :575), here a Type 1 that is not the FoF central; the FoF
// central holds the other Regime. The orphan (Mvir 0, so the gate passes, :534-537, with a clock
// of 5 that stays positive after the 0.1 decrement, :529-530, :574-575) holds cold 0.5 / 0.0625,
// hot 0.25 / 0.03125 and CGM 0.125 / 0.015625, summing exactly to 0.875 / 0.109375; both of the
// recipient's pairs start at 1 / 0.125, so its selected pair ends at 1.875 / 0.234375.

/** @brief Read a captured log stream into a static buffer (valid until the next call) */
static const char *c10_read_log(FILE *fp) {
  static char buf[8192];
  rewind(fp);
  const size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
  buf[n] = '\0';
  return buf;
}

/** @brief Set the parameter list's CGMrecipeOn to `value`, or remove it for NULL */
static void c10_set_cgm_recipe_on(const char *value) {
  int n = 0;
  for (int i = 0; i < MimicConfig.NumModelParams; i++) {
    if (strcmp(MimicConfig.ModelParams[i].param_name, "CGMrecipeOn") != 0) {
      MimicConfig.ModelParams[n++] = MimicConfig.ModelParams[i];
    }
  }
  if (value != NULL) {
    snprintf(MimicConfig.ModelParams[n].param_name, MAX_STRING_LEN, "%s", "CGMrecipeOn");
    snprintf(MimicConfig.ModelParams[n].value, MAX_STRING_LEN, "%s", value);
    n++;
  }
  MimicConfig.NumModelParams = n;
}

/** @brief Initialise the resolver (clock in pre_timestep) with CGMrecipeOn `cgm`, NULL missing */
static int c10_init(const char *cgm, const char **log) {
  reset_config();
  setup_model_parameters(TEST_THRESHOLD_DISRUPTION, TEST_THRESHOLD_MAJOR);
  c10_set_cgm_recipe_on(cgm);
  FILE *capture = tmpfile();
  FILE *old_out = capture != NULL ? set_log_output(capture) : NULL;
  const int status = init_parity_modules();
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
 * @test    test_cgm_disruption_to_passed_recipient
 * @brief   An orphan's disruption deposits its gas in its subhalo central's selected pair, chosen
 *          by that recipient's Regime, not the FoF central's
 */
int test_cgm_disruption_to_passed_recipient(void) {
  const struct {
    const char *cgm;
    int recipient_regime;
    int to_cgm;
  } runs[] = {{"1", 0, 1}, {"1", 1, 0}, {"0", 0, 0}, {"0", 1, 0}};
  int ok = 1;
  init_memory_system(0);
  for (size_t r = 0; r < sizeof(runs) / sizeof(runs[0]); r++) {
    const char *log = "";
    TEST_ASSERT(c10_init(runs[r].cgm, &log) == 0, "init must accept CGMrecipeOn 0 and 1");
    struct ModuleContext ctx;
    struct Halo halos[3];
    struct GalaxyData galaxies[3];
    setup_context(&ctx);
    /* FoF central, a Type 1 subhalo central that stays (Mvir 50 against baryons 1.5, :537) and
     * the Type 2 orphan of that subhalo */
    setup_test_halo(&halos[0], &galaxies[0], 3000, 0, -1, 80.0, 0.0, 240.0, 999.9, 18.0, 2.0, 1.0,
                    1.0, 0.2, 3.0);
    setup_test_halo(&halos[1], &galaxies[1], 3001, 1, 0, 50.0, 0.0, 120.0, 5.0, 1.0, 0.5, 1.0, 0.1,
                    0.0, 0.3);
    setup_test_halo(&halos[2], &galaxies[2], 3002, 2, 1, 0.0, 0.0, 70.0, 5.0, 0.5, 0.5, 0.25, 0.0,
                    0.0, 0.1);
    ctx.central_galaxy = &halos[0];
    for (int k = 0; k < 2; k++) {
      galaxies[k].HotGas = 1.0f;
      galaxies[k].MetalsHotGas = 0.125f;
      galaxies[k].CGMgas = 1.0f;
      galaxies[k].MetalsCGMgas = 0.125f;
    }
    galaxies[1].Regime = runs[r].recipient_regime;
    galaxies[0].Regime = 1 - runs[r].recipient_regime;
    galaxies[2].Regime = 1 - runs[r].recipient_regime;
    galaxies[2].MetalsColdGas = 0.0625f;
    galaxies[2].MetalsHotGas = 0.03125f;
    galaxies[2].CGMgas = 0.125f;
    galaxies[2].MetalsCGMgas = 0.015625f;

    const int status = sage26_resolve_mergers_and_disruption_process(&ctx, halos, 3);

    const struct GalaxyData *rec = &galaxies[1];
    const float sel_gas = runs[r].to_cgm ? rec->CGMgas : rec->HotGas;
    const float sel_metals = runs[r].to_cgm ? rec->MetalsCGMgas : rec->MetalsHotGas;
    const float oth_gas = runs[r].to_cgm ? rec->HotGas : rec->CGMgas;
    const float oth_metals = runs[r].to_cgm ? rec->MetalsHotGas : rec->MetalsCGMgas;
    const int recipient_ok =
        sel_gas == 1.875f && sel_metals == 0.234375f && oth_gas == 1.0f && oth_metals == 0.125f;
    const int fof_ok = galaxies[0].HotGas == 1.0f && galaxies[0].MetalsHotGas == 0.125f &&
                       galaxies[0].CGMgas == 1.0f && galaxies[0].MetalsCGMgas == 0.125f;
    const int orphan_ok = halos[2].Type == 3 && halos[1].Type == 1 && galaxies[2].ColdGas == 0.0f &&
                          galaxies[2].HotGas == 0.0f && galaxies[2].CGMgas == 0.0f;
    if (status != 0 || !recipient_ok || !fof_ok || !orphan_ok) {
      printf("\n  CGMrecipeOn %s, recipient Regime %d: status %d, recipient hot %.9g/%.9g, CGM "
             "%.9g/%.9g, FoF central hot %.9g CGM %.9g, orphan Type %d",
             runs[r].cgm, runs[r].recipient_regime, status, (double)rec->HotGas,
             (double)rec->MetalsHotGas, (double)rec->CGMgas, (double)rec->MetalsCGMgas,
             (double)galaxies[0].HotGas, (double)galaxies[0].CGMgas, halos[2].Type);
      ok = 0;
    }
    cleanup_parity_modules();
  }
  check_memory_leaks();
  TEST_ASSERT(ok, "the disrupted gas must reach exactly the passed recipient's selected pair");
  return TEST_PASS;
}

/**
 * @test    test_cgm_recipe_on_refused
 * @brief   CGMrecipeOn is required (missing key fails init) and only 0 or 1 is accepted,
 *          a refusal naming the parameter
 */
int test_cgm_recipe_on_refused(void) {
  const char *log = "";
  TEST_ASSERT(c10_init(NULL, &log) != 0, "init must fail when CGMrecipeOn is missing");
  TEST_ASSERT(strstr(log, "CGMrecipeOn") != NULL, "the missing-key error must name CGMrecipeOn");
  cleanup_parity_modules();
  TEST_ASSERT(c10_init("2", &log) != 0, "init must refuse CGMrecipeOn 2");
  TEST_ASSERT(strstr(log, "CGMrecipeOn") != NULL, "the refusal must name CGMrecipeOn");
  cleanup_parity_modules();
  TEST_ASSERT(c10_init("-1", &log) != 0, "init must refuse CGMrecipeOn -1");
  TEST_ASSERT(strstr(log, "CGMrecipeOn") != NULL, "the refusal must name CGMrecipeOn");
  cleanup_parity_modules();
  TEST_ASSERT(c10_init("1", &log) == 0, "init must accept CGMrecipeOn 1");
  cleanup_parity_modules();
  TEST_ASSERT(c10_init("0", &log) == 0, "init must accept CGMrecipeOn 0");
  cleanup_parity_modules();
  return TEST_PASS;
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
  TEST_RUN(test_mode3_merge_refuses_missing_core_parameters);
  TEST_RUN(test_cgm_disruption_to_passed_recipient);
  TEST_RUN(test_cgm_recipe_on_refused);

  TEST_SUMMARY();
  return TEST_RESULT();
}
