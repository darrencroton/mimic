/**
 * @file    test_unit_sage26_set_halo_concentration.c
 * @brief   Behavioural tests of sage26_set_halo_concentration
 *
 * The tests build FoF workspaces by hand and run the module's init() and process(). Every
 * expected concentration is derived independently of the module and of
 * shared/sage_halo_concentration.h: SAGE26's inversion is written here
 * (sage26_inline_concentration) and fed the float pair SAGE26's mode-3 dispatch selects, or the
 * expectation is a literal.
 *
 * Software quality:
 * - test_module_lifecycle: registration, module_system_init in pre_timestep with ConcentrationOn
 *   0 (which needs no recorder), cleanup, no leaks
 * - test_concentration_on_missing_refused: a missing ConcentrationOn fails init, naming it
 * - test_concentration_on_values: 1, 2 and 7 are refused naming ConcentrationOn; 0 and 3 accepted
 * - test_placement_refused: a substep-phase entry, a post_timestep entry, two pre_timestep
 *   entries, a process_by_galaxy entry and no entry are each refused, naming the module
 * - test_recorder_required_at_3: ConcentrationOn 3 without sage26_record_transition_state in
 *   post_timestep is refused, naming the recorder; at 0 the check does not apply
 *
 * Behaviour (ConcentrationOn 3 unless stated):
 * - test_current_and_infall_pairs: a new Type 0 with PreviousType 0 takes the current pair and
 *   with PreviousType 1 the infall pair; each row's pairs give different results
 * - test_kept_rows: a new Type 0 with PreviousType -1 or 2, and every new Type 1 or 2 whatever its
 *   PreviousType, keep a seeded non-zero Concentration
 * - test_float_store: the stored float is SAGE26's (float) of the double, a visible rounding, with
 *   each core double velocity narrowed to SAGE26's float first
 * - test_ratio_cases: a central whose ratio is below 1 is overwritten with 0, one at ratio 1 gets
 *   exactly 1.0, one beyond f(200) exactly 200
 * - test_off_writes_nothing: with ConcentrationOn 0 no row changes
 * - test_empty_and_null_rows: an empty FoF and rows without a galaxy
 *
 * Joining compositions (ConcentrationOn 3): the core's inheritance service
 * (inherit_descendant_halos, src/core/inheritance.c) joins one progenitor into its descendant,
 * then the module runs on the joined rows, as the run files order them in pre_timestep. Each
 * progenitor's PreviousType is its Type, as sage26_record_transition_state wrote it at the end of
 * its snapshot. The expectations are SAGE26's inversion written here, fed the float pair SAGE26's
 * joining has at the call, never the module's or the shared helper's result:
 * - test_join_type1_to_0_infall_pair: a Type 1 whose subhalo becomes its FoF's first halo takes
 *   its infall pair; its current pair's result differs
 * - test_join_type0_to_0_updated_current_pair: a central whose halo grew takes the updated pair
 *   (the halo's Vmax and the new Vvir); its pre-join pair's result differs
 * - test_join_type0_to_1_keeps: a central that becomes a satellite keeps its value
 */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../../../tests/framework/test_framework.h"
#include "../../../../tests/framework/test_phase_config.h"

#include "core/galaxy_pool.h"
#include "core/inheritance.h"
#include "core/module_interface.h"
#include "core/module_registry.h"
#include "include/globals.h"
#include "include/types.h"
#include "util/error.h"
#include "util/memory.h"

#include "modules/_tests/sage_test_fixtures.h"

extern int sage26_set_halo_concentration_init(void);
extern int sage26_set_halo_concentration_process(struct ModuleContext *ctx, struct Halo *halos,
                                                 int ngal);
extern int sage26_set_halo_concentration_cleanup(void);

#define MODULE_NAME "sage26_set_halo_concentration"
#define RECORDER_NAME "sage26_record_transition_state"
#define SEED_CONCENTRATION 7.5f

// ============================================================================
// FIXTURES
// ============================================================================

static char module_name[] = MODULE_NAME;
static char recorder_name[] = RECORDER_NAME;
static char clock_name[] = "sage26_initialise_merger_clock";
static char phase_name[] = "galaxy_physics";
static struct PhaseModuleConfig pre_timestep_entries[3];
static struct PhaseModuleConfig post_timestep_entries[2];
static struct PhaseModuleConfig substep_entries[1];
static struct ModulePhaseConfig substep_phase[1];

/** @brief Read all content from a FILE* into a static buffer (valid until the next call) */
static const char *read_captured_log(FILE *fp) {
  static char buf[8192];
  rewind(fp);
  const size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
  buf[n] = '\0';
  return buf;
}

/**
 * @brief Configure the shipped placement: the clock and the module in pre_timestep, the recorder
 *        in post_timestep (each process_full_halo), and the parameter's run-file text
 *
 * @param concentration_on  ConcentrationOn's run-file text, or NULL for a missing key
 */
static void configure(const char *concentration_on) {
  reset_config();
  pre_timestep_entries[0] = (struct PhaseModuleConfig){clock_name, PROCESSING_MODE_FULL_HALO, NULL};
  pre_timestep_entries[1] =
      (struct PhaseModuleConfig){module_name, PROCESSING_MODE_FULL_HALO, NULL};
  MimicConfig.pre_timestep = pre_timestep_entries;
  MimicConfig.num_pre_timestep = 2;
  post_timestep_entries[0] =
      (struct PhaseModuleConfig){recorder_name, PROCESSING_MODE_FULL_HALO, NULL};
  MimicConfig.post_timestep = post_timestep_entries;
  MimicConfig.num_post_timestep = 1;
  if (concentration_on != NULL) {
    sage26_test_append_model_parameter("ConcentrationOn", concentration_on);
  }
}

/** @brief Run init() on the current configuration, capturing the log */
static int init_captured(const char **log) {
  FILE *capture = tmpfile();
  FILE *old_out = capture != NULL ? set_log_output(capture) : NULL;
  const int status = sage26_set_halo_concentration_init();
  if (capture != NULL) {
    set_log_output(old_out);
    *log = read_captured_log(capture);
    fclose(capture);
  } else {
    *log = "";
  }
  return status;
}

/** @brief Configure the shipped placement with the given ConcentrationOn and initialise */
static int init_ok(const char *concentration_on) {
  const char *log = "";
  configure(concentration_on);
  return init_captured(&log);
}

/**
 * @brief A joined row: the core's new Type and double velocities, the galaxy's previous Type and
 *        a seeded Concentration
 */
static void make_row(struct Halo *halo, struct GalaxyData *gal, const int type,
                     const int previous_type, const double vmax, const double vvir,
                     const double infall_vmax, const double infall_vvir) {
  memset(halo, 0, sizeof(*halo));
  init_galaxy_defaults(gal);
  halo->galaxy = gal;
  halo->Type = type;
  halo->Vmax = vmax;
  halo->Vvir = vvir;
  halo->infallVmax = infall_vmax;
  halo->infallVvir = infall_vvir;
  gal->PreviousType = previous_type;
  gal->Concentration = SEED_CONCENTRATION;
}

static struct ModuleContext make_context(void) {
  struct ModuleContext ctx;
  memset(&ctx, 0, sizeof(ctx));
  ctx.params = &MimicConfig;
  ctx.snapshot_number = 40;
  ctx.redshift = 1.0;
  ctx.num_substeps = 1;
  return ctx;
}

static uint32_t bits32(const float v) {
  uint32_t b;
  memcpy(&b, &v, sizeof(b));
  return b;
}

#if defined(__GNUC__) || defined(__clang__)
#define CONCENTRATION_TEST_NOINLINE __attribute__((noinline))
#else
#define CONCENTRATION_TEST_NOINLINE
#endif

/**
 * @brief SAGE26's concentration_from_vmax_vvir, written here, on the float pair the dispatch
 *        selected (each widened)
 *
 * Never inlined, inputs through volatile locals: the expectation must use the runtime libm.
 */
static CONCENTRATION_TEST_NOINLINE double sage26_inline_concentration(const float vmax_f,
                                                                      const float vvir_f) {
  volatile double vmax_runtime = vmax_f;
  volatile double vvir_runtime = vvir_f;
  const double Vmax = vmax_runtime;
  const double Vvir = vvir_runtime;
  if (Vvir <= 0.0 || Vmax <= 0.0)
    return 0.0;
  const double ratio_sq = (Vmax / Vvir) * (Vmax / Vvir);
  const double A = 0.21621;
  double c_lo = 1.0, c_hi = 200.0;
  if (ratio_sq < 1.0)
    return 0.0;
  for (int iter = 0; iter < 60; iter++) {
    const double c_mid = 0.5 * (c_lo + c_hi);
    const double mu = log(1.0 + c_mid) - c_mid / (1.0 + c_mid);
    const double f_mid = A * c_mid / mu;
    if (f_mid < ratio_sq) {
      c_lo = c_mid;
    } else {
      c_hi = c_mid;
    }
  }
  return 0.5 * (c_lo + c_hi);
}

// ============================================================================
// SOFTWARE QUALITY TESTS
// ============================================================================

/**
 * @test    test_module_lifecycle
 * @brief   The module registers, initialises through module_system_init in pre_timestep with
 *          ConcentrationOn 0, cleans up, no leaks
 */
int test_module_lifecycle(void) {
  reset_config();
  init_memory_system(0);
  ensure_modules_registered();
  test_pre_timestep_add(MODULE_NAME, PROCESSING_MODE_FULL_HALO);
  set_test_model_parameters();
  sage26_test_append_model_parameter("ConcentrationOn", "0");

  const int result = module_system_init();
  TEST_ASSERT(result == 0, "module system initialisation must succeed");

  module_system_cleanup();
  check_memory_leaks();
  return TEST_PASS;
}

/**
 * @test    test_concentration_on_missing_refused
 * @brief   Init fails without ConcentrationOn, naming it: required, with no default
 */
int test_concentration_on_missing_refused(void) {
  const char *log = "";
  configure(NULL);
  const int status = init_captured(&log);
  printf("\n  without ConcentrationOn: init %d", status);
  TEST_ASSERT(status != 0, "init must fail when ConcentrationOn is missing");
  TEST_ASSERT(strstr(log, "ConcentrationOn") != NULL, "the error must name ConcentrationOn");
  return TEST_PASS;
}

/**
 * @test    test_concentration_on_values
 * @brief   ConcentrationOn 1, 2 (not supported) and 7 are refused naming it; 0 and 3 initialise
 */
int test_concentration_on_values(void) {
  const char *refused[] = {"1", "2", "7"};
  for (size_t k = 0; k < sizeof(refused) / sizeof(refused[0]); k++) {
    const char *log = "";
    configure(refused[k]);
    const int status = init_captured(&log);
    printf("\n  ConcentrationOn %s: init %d", refused[k], status);
    TEST_ASSERT(status != 0, "init must refuse an unsupported ConcentrationOn");
    TEST_ASSERT(strstr(log, "ConcentrationOn") != NULL, "the error must name ConcentrationOn");
  }
  TEST_ASSERT(init_ok("0") == 0, "init must accept ConcentrationOn 0");
  TEST_ASSERT(init_ok("3") == 0, "init must accept ConcentrationOn 3");
  return TEST_PASS;
}

/**
 * @test    test_placement_refused
 * @brief   Only exactly one pre_timestep process_full_halo entry, and no other entry, initialises;
 *          each misplacement is refused naming the module (at ConcentrationOn 0 and 3)
 */
int test_placement_refused(void) {
  const char *values[2] = {"0", "3"};
  for (int v = 0; v < 2; v++) {
    const char *log = "";

    /* A substep-phase entry as well */
    configure(values[v]);
    substep_entries[0] = (struct PhaseModuleConfig){module_name, PROCESSING_MODE_FULL_HALO, NULL};
    substep_phase[0] = (struct ModulePhaseConfig){phase_name, substep_entries, 1};
    MimicConfig.substep_phases = substep_phase;
    MimicConfig.num_substep_phases = 1;
    int status = init_captured(&log);
    printf("\n  ConcentrationOn %s, substep-phase entry: init %d", values[v], status);
    TEST_ASSERT(status != 0, "init must refuse a substep-phase entry");
    TEST_ASSERT(strstr(log, MODULE_NAME) != NULL, "the error must name the module");
    MimicConfig.substep_phases = NULL;
    MimicConfig.num_substep_phases = 0;

    /* A post_timestep entry as well */
    configure(values[v]);
    post_timestep_entries[1] =
        (struct PhaseModuleConfig){module_name, PROCESSING_MODE_FULL_HALO, NULL};
    MimicConfig.num_post_timestep = 2;
    status = init_captured(&log);
    printf("\n  ConcentrationOn %s, post_timestep entry: init %d", values[v], status);
    TEST_ASSERT(status != 0, "init must refuse a post_timestep entry");
    TEST_ASSERT(strstr(log, MODULE_NAME) != NULL, "the error must name the module");

    /* Twice in pre_timestep */
    configure(values[v]);
    pre_timestep_entries[2] =
        (struct PhaseModuleConfig){module_name, PROCESSING_MODE_FULL_HALO, NULL};
    MimicConfig.num_pre_timestep = 3;
    status = init_captured(&log);
    printf("\n  ConcentrationOn %s, two pre_timestep entries: init %d", values[v], status);
    TEST_ASSERT(status != 0, "init must refuse two pre_timestep entries");
    TEST_ASSERT(strstr(log, MODULE_NAME) != NULL, "the error must name the module");

    /* process_by_galaxy in pre_timestep */
    configure(values[v]);
    pre_timestep_entries[1].processing_mode = PROCESSING_MODE_BY_GALAXY;
    status = init_captured(&log);
    printf("\n  ConcentrationOn %s, process_by_galaxy: init %d", values[v], status);
    TEST_ASSERT(status != 0, "init must refuse a process_by_galaxy entry");
    TEST_ASSERT(strstr(log, MODULE_NAME) != NULL, "the error must name the module");

    /* No pre_timestep entry */
    configure(values[v]);
    MimicConfig.num_pre_timestep = 1;
    status = init_captured(&log);
    printf("\n  ConcentrationOn %s, no pre_timestep entry: init %d", values[v], status);
    TEST_ASSERT(status != 0, "init must refuse a missing pre_timestep entry");
    TEST_ASSERT(strstr(log, MODULE_NAME) != NULL, "the error must name the module");
  }
  return TEST_PASS;
}

/**
 * @test    test_recorder_required_at_3
 * @brief   ConcentrationOn 3 without sage26_record_transition_state in post_timestep is refused,
 *          naming the recorder; ConcentrationOn 0 without it initialises
 */
int test_recorder_required_at_3(void) {
  const char *log = "";
  configure("3");
  MimicConfig.post_timestep = NULL;
  MimicConfig.num_post_timestep = 0;
  int status = init_captured(&log);
  printf("\n  ConcentrationOn 3 without the recorder: init %d", status);
  TEST_ASSERT(status != 0, "init must refuse ConcentrationOn 3 without the recorder");
  TEST_ASSERT(strstr(log, RECORDER_NAME) != NULL, "the error must name the recorder");

  configure("3");
  post_timestep_entries[0].processing_mode = PROCESSING_MODE_BY_GALAXY;
  status = init_captured(&log);
  printf("\n  ConcentrationOn 3, recorder not process_full_halo: init %d", status);
  TEST_ASSERT(status != 0, "init must refuse a recorder that is not process_full_halo");
  TEST_ASSERT(strstr(log, RECORDER_NAME) != NULL, "the error must name the recorder");

  configure("0");
  MimicConfig.post_timestep = NULL;
  MimicConfig.num_post_timestep = 0;
  status = init_captured(&log);
  printf("\n  ConcentrationOn 0 without the recorder: init %d", status);
  TEST_ASSERT(status == 0, "at ConcentrationOn 0 the recorder check must not apply");
  return TEST_PASS;
}

// ============================================================================
// BEHAVIOURAL TESTS
// ============================================================================

/**
 * @test    test_current_and_infall_pairs
 * @brief   New Type 0 rows: PreviousType 0 takes the current pair, PreviousType 1 the infall pair
 *          (SAGE26 selects the pair at the Type before joining). The other pair of each row
 *          would give a different result (ratio below 1, so 0)
 */
int test_current_and_infall_pairs(void) {
  TEST_ASSERT(init_ok("3") == 0, "init must succeed");
  struct Halo halos[2];
  struct GalaxyData gals[2];
  /* previous central: current 300/200 (ratio 1.5), infall 100/200 (ratio 0.25 -> 0) */
  make_row(&halos[0], &gals[0], 0, 0, 300.0, 200.0, 100.0, 200.0);
  /* returning satellite: current 100/200 (-> 0), infall 330/200 (ratio 1.65) */
  make_row(&halos[1], &gals[1], 0, 1, 100.0, 200.0, 330.0, 200.0);
  struct ModuleContext ctx = make_context();
  TEST_ASSERT(sage26_set_halo_concentration_process(&ctx, halos, 2) == 0, "process must succeed");

  const float expected0 = (float)sage26_inline_concentration(300.0f, 200.0f);
  const float expected1 = (float)sage26_inline_concentration(330.0f, 200.0f);
  printf("\n  PreviousType 0: %.9g (expected %.9g); PreviousType 1: %.9g (expected %.9g)",
         (double)gals[0].Concentration, (double)expected0, (double)gals[1].Concentration,
         (double)expected1);
  TEST_ASSERT(expected0 > 1.0f && expected1 > 1.0f && expected0 != expected1,
              "the chosen pairs must give distinct non-trivial concentrations");
  TEST_ASSERT(bits32(gals[0].Concentration) == bits32(expected0),
              "a previous central must take its current Vmax/Vvir");
  TEST_ASSERT(bits32(gals[1].Concentration) == bits32(expected1),
              "a returning satellite must take its infall Vmax/Vvir");
  sage26_set_halo_concentration_cleanup();
  return TEST_PASS;
}

/**
 * @test    test_kept_rows
 * @brief   Rows the joining branch does not reach keep a seeded non-zero Concentration: a new
 *          Type 0 created this snapshot (PreviousType -1) or formerly a Type 2, and every new
 *          Type 1 or 2 (satellites keep their last central value). Every row holds valid pairs,
 *          so any write would be visible
 */
int test_kept_rows(void) {
  TEST_ASSERT(init_ok("3") == 0, "init must succeed");
  const int new_type[8] = {0, 0, 1, 1, 1, 2, 2, 2};
  const int prev_type[8] = {-1, 2, 0, 1, -1, 0, 1, 2};
  struct Halo halos[8];
  struct GalaxyData gals[8];
  for (int i = 0; i < 8; i++) {
    make_row(&halos[i], &gals[i], new_type[i], prev_type[i], 300.0, 200.0, 330.0, 200.0);
  }
  struct ModuleContext ctx = make_context();
  TEST_ASSERT(sage26_set_halo_concentration_process(&ctx, halos, 8) == 0, "process must succeed");
  for (int i = 0; i < 8; i++) {
    printf("\n  new Type %d, PreviousType %d: Concentration %.9g", new_type[i], prev_type[i],
           (double)gals[i].Concentration);
    TEST_ASSERT(bits32(gals[i].Concentration) == bits32(SEED_CONCENTRATION),
                "a row the joining branch does not reach must keep its Concentration");
  }
  sage26_set_halo_concentration_cleanup();
  return TEST_PASS;
}

/**
 * @test    test_float_store
 * @brief   The stored float is (float) of SAGE26's double, a visible rounding;
 *          each core double is narrowed to SAGE26's float before the inversion (Vvir 200.00001
 *          and the infall pair's doubles round to floats that change the result)
 */
int test_float_store(void) {
  TEST_ASSERT(init_ok("3") == 0, "init must succeed");
  struct Halo halos[2];
  struct GalaxyData gals[2];
  const double vvir_core = 200.00001;          /* not a float */
  const double infall_vmax_core = 330.0000001; /* not a float */
  make_row(&halos[0], &gals[0], 0, 0, 300.0, vvir_core, 1.0, 1.0);
  make_row(&halos[1], &gals[1], 0, 1, 1.0, 1.0, infall_vmax_core, vvir_core);
  struct ModuleContext ctx = make_context();
  TEST_ASSERT(sage26_set_halo_concentration_process(&ctx, halos, 2) == 0, "process must succeed");

  const double d0 = sage26_inline_concentration(300.0f, (float)vvir_core);
  const double d1 = sage26_inline_concentration((float)infall_vmax_core, (float)vvir_core);
  printf("\n  current: double %.17g, stored %.9g; infall: double %.17g, stored %.9g", d0,
         (double)gals[0].Concentration, d1, (double)gals[1].Concentration);
  TEST_ASSERT((float)vvir_core != vvir_core && (float)infall_vmax_core != infall_vmax_core,
              "the core doubles must not be floats");
  TEST_ASSERT(bits32(gals[0].Concentration) == bits32((float)d0) &&
                  bits32(gals[1].Concentration) == bits32((float)d1),
              "the store must be (float) of SAGE26's double of the narrowed pair");
  TEST_ASSERT((double)gals[0].Concentration != d0 && (double)gals[1].Concentration != d1,
              "the rounding to float must be visible for these inputs");
  sage26_set_halo_concentration_cleanup();
  return TEST_PASS;
}

/**
 * @test    test_ratio_cases
 * @brief   A previous central whose ratio is below 1 is overwritten with 0 (a non-zero earlier
 *          value is not kept); one at ratio exactly 1 gets exactly 1.0; one beyond f(200) exactly
 *          200 (literal expectations)
 */
int test_ratio_cases(void) {
  TEST_ASSERT(init_ok("3") == 0, "init must succeed");
  struct Halo halos[3];
  struct GalaxyData gals[3];
  make_row(&halos[0], &gals[0], 0, 0, 180.0, 200.0, 330.0, 200.0);
  make_row(&halos[1], &gals[1], 0, 0, 200.0, 200.0, 330.0, 200.0);
  make_row(&halos[2], &gals[2], 0, 1, 1.0, 1.0, 2000.0, 200.0);
  struct ModuleContext ctx = make_context();
  TEST_ASSERT(sage26_set_halo_concentration_process(&ctx, halos, 3) == 0, "process must succeed");
  printf("\n  ratio 0.81: %a; ratio 1: %a; ratio 100: %a", (double)gals[0].Concentration,
         (double)gals[1].Concentration, (double)gals[2].Concentration);
  TEST_ASSERT(bits32(gals[0].Concentration) == bits32(0.0f),
              "a ratio below 1 must overwrite the earlier value with +0");
  TEST_ASSERT(gals[1].Concentration == 1.0f, "ratio exactly 1 must give exactly 1.0");
  TEST_ASSERT(gals[2].Concentration == 200.0f, "a ratio beyond f(200) must saturate at 200");
  sage26_set_halo_concentration_cleanup();
  return TEST_PASS;
}

/**
 * @test    test_off_writes_nothing
 * @brief   With ConcentrationOn 0 no row is written, whatever its Type, PreviousType and pairs
 *          (SAGE26 makes no call)
 */
int test_off_writes_nothing(void) {
  TEST_ASSERT(init_ok("0") == 0, "init must succeed");
  struct Halo halos[4];
  struct GalaxyData gals[4];
  make_row(&halos[0], &gals[0], 0, 0, 300.0, 200.0, 330.0, 200.0);
  make_row(&halos[1], &gals[1], 0, 1, 300.0, 200.0, 330.0, 200.0);
  make_row(&halos[2], &gals[2], 0, 0, 180.0, 200.0, 330.0, 200.0);
  make_row(&halos[3], &gals[3], 1, 0, 300.0, 200.0, 330.0, 200.0);
  struct Halo halos_before[4];
  struct GalaxyData gals_before[4];
  memcpy(halos_before, halos, sizeof(halos));
  memcpy(gals_before, gals, sizeof(gals));
  struct ModuleContext ctx = make_context();
  TEST_ASSERT(sage26_set_halo_concentration_process(&ctx, halos, 4) == 0, "process must succeed");
  for (int i = 0; i < 4; i++) {
    printf("\n  row %d: Concentration %.9g", i, (double)gals[i].Concentration);
  }
  TEST_ASSERT(memcmp(gals, gals_before, sizeof(gals)) == 0,
              "ConcentrationOn 0 must write no galaxy");
  TEST_ASSERT(memcmp(halos, halos_before, sizeof(halos)) == 0,
              "ConcentrationOn 0 must leave the rows unchanged");
  sage26_set_halo_concentration_cleanup();
  return TEST_PASS;
}

/**
 * @test    test_empty_and_null_rows
 * @brief   An empty FoF and a NULL workspace succeed; a row without a galaxy is skipped
 */
int test_empty_and_null_rows(void) {
  TEST_ASSERT(init_ok("3") == 0, "init must succeed");
  struct ModuleContext ctx = make_context();
  TEST_ASSERT(sage26_set_halo_concentration_process(&ctx, NULL, 0) == 0, "NULL must succeed");
  struct Halo halos[2];
  struct GalaxyData gals[2];
  make_row(&halos[0], &gals[0], 0, 0, 300.0, 200.0, 330.0, 200.0);
  make_row(&halos[1], &gals[1], 0, 0, 300.0, 200.0, 330.0, 200.0);
  halos[0].galaxy = NULL;
  TEST_ASSERT(sage26_set_halo_concentration_process(&ctx, halos, 0) == 0, "ngal 0 must succeed");
  TEST_ASSERT(gals[1].Concentration == SEED_CONCENTRATION, "ngal 0 must write nothing");
  TEST_ASSERT(sage26_set_halo_concentration_process(&ctx, halos, 2) == 0, "process must succeed");
  TEST_ASSERT(gals[0].Concentration == SEED_CONCENTRATION, "a galaxy-less row must be skipped");
  TEST_ASSERT(gals[1].Concentration != SEED_CONCENTRATION, "the galaxy row must be assigned");
  sage26_set_halo_concentration_cleanup();
  return TEST_PASS;
}

// ============================================================================
// JOINING COMPOSITION TESTS (THE CORE'S INHERITANCE, THEN THE MODULE)
// ============================================================================

/**
 * @brief A progenitor row as the previous snapshot left it: its Type, PreviousType equal to it
 *        (the recorder's write), its double halo values and a seeded Concentration
 */
static void make_progenitor(struct Halo *halo, struct GalaxyData *gal, const int type,
                            const double mvir, const double vmax, const double vvir,
                            const double infall_vmax, const double infall_vvir) {
  make_row(halo, gal, type, type, vmax, vvir, infall_vmax, infall_vvir);
  halo->Mvir = mvir;
  halo->Rvir = 0.1;
  halo->Len = 100;
  halo->SnapNum = 39;
}

/**
 * @brief The descendant halo as the core's driver describes it: its virial values and
 *        catalogue Vmax, and whether it is its FoF's first halo
 */
static struct InheritanceDescendant make_descendant(const double virial_mass,
                                                    const double virial_velocity, const float vmax,
                                                    const int is_fof_central) {
  struct InheritanceDescendant d;
  memset(&d, 0, sizeof(d));
  d.halo_nr = 7;
  d.current_snap = 40;
  d.virial_mass = virial_mass;
  d.virial_radius = 0.125;
  d.virial_velocity = virial_velocity;
  d.is_fof_central = is_fof_central;
  d.halo_payload.Len = 120;
  d.halo_payload.Vmax = vmax;
  return d;
}

/**
 * @brief Join one main-branch progenitor with the core's inheritance, then run the module
 * @return The number of joined rows, or -1 when process() fails
 */
static int join_then_process(struct GalaxyPool *pool, struct Halo *workspace,
                             const struct InheritanceDescendant *d, const struct Halo *source) {
  struct InheritanceProgenitorGalaxy progenitor = {source, 0.0, 1};
  const int end = (int)inherit_descendant_halos(pool, workspace, 0, 2, d, &progenitor, 1);
  struct ModuleContext ctx = make_context();
  if (sage26_set_halo_concentration_process(&ctx, workspace, end) != 0) {
    return -1;
  }
  return end;
}

/**
 * @test    test_join_type1_to_0_infall_pair
 * @brief   A Type 1 whose subhalo becomes its FoF's first halo (Type 1 -> 0): SAGE26's call sits
 *          before Type = 0, so the dispatch sees Type 1 and inverts the infall pair. The halo did
 *          not grow, so the current pair at the call is the halo's Vmax 76 and the kept Vvir 70,
 *          whose result differs
 */
int test_join_type1_to_0_infall_pair(void) {
  TEST_ASSERT(init_ok("3") == 0, "init must succeed");
  struct GalaxyPool *pool = galaxy_pool_create(4);
  struct Halo source;
  struct GalaxyData source_gal;
  make_progenitor(&source, &source_gal, 1, 5.0, 75.0, 70.0, 95.0, 75.0);
  const struct InheritanceDescendant d = make_descendant(4.0, 64.0, 76.0f, 1);
  struct Halo workspace[2];
  const int end = join_then_process(pool, workspace, &d, &source);
  TEST_ASSERT(end == 1, "the join must keep one row, and process must succeed");

  const float expected = (float)sage26_inline_concentration(95.0f, 75.0f);
  const float current_pair = (float)sage26_inline_concentration(76.0f, 70.0f);
  printf("\n  Type 1 -> 0: Type %d, Concentration %.9g (infall pair %.9g, current pair %.9g)",
         workspace[0].Type, (double)workspace[0].galaxy->Concentration, (double)expected,
         (double)current_pair);
  TEST_ASSERT(workspace[0].Type == 0 && workspace[0].galaxy->PreviousType == 1,
              "the joined row must be the new FoF central with PreviousType 1");
  TEST_ASSERT(expected > 1.0f && current_pair > 1.0f && expected != current_pair,
              "the two pairs must give distinct non-trivial concentrations");
  TEST_ASSERT(bits32(workspace[0].galaxy->Concentration) == bits32(expected),
              "a returning satellite must take its infall pair");
  galaxy_pool_destroy(pool);
  sage26_set_halo_concentration_cleanup();
  return TEST_PASS;
}

/**
 * @test    test_join_type0_to_0_updated_current_pair
 * @brief   A central whose halo grew (Type 0 -> 0): SAGE26's call reads the updated pair, the
 *          halo's Vmax and the new Vvir, not the pre-join pair, whose result differs
 */
int test_join_type0_to_0_updated_current_pair(void) {
  TEST_ASSERT(init_ok("3") == 0, "init must succeed");
  struct GalaxyPool *pool = galaxy_pool_create(4);
  struct Halo source;
  struct GalaxyData source_gal;
  make_progenitor(&source, &source_gal, 0, 50.0, 170.0, 150.0, -1.0, -1.0);
  const struct InheritanceDescendant d = make_descendant(60.0, 160.5, 210.25f, 1);
  struct Halo workspace[2];
  const int end = join_then_process(pool, workspace, &d, &source);
  TEST_ASSERT(end == 1, "the join must keep one row, and process must succeed");

  const float expected = (float)sage26_inline_concentration(210.25f, 160.5f);
  const float pre_join_pair = (float)sage26_inline_concentration(170.0f, 150.0f);
  printf("\n  Type 0 -> 0, halo grew: Concentration %.9g (updated pair %.9g, pre-join pair %.9g)",
         (double)workspace[0].galaxy->Concentration, (double)expected, (double)pre_join_pair);
  TEST_ASSERT(workspace[0].Type == 0 && workspace[0].galaxy->PreviousType == 0,
              "the joined row must be the FoF central with PreviousType 0");
  TEST_ASSERT(expected > 1.0f && pre_join_pair > 1.0f && expected != pre_join_pair,
              "the updated and pre-join pairs must give distinct non-trivial concentrations");
  TEST_ASSERT(bits32(workspace[0].galaxy->Concentration) == bits32(expected),
              "a central must take the updated current pair");
  galaxy_pool_destroy(pool);
  sage26_set_halo_concentration_cleanup();
  return TEST_PASS;
}

/**
 * @test    test_join_type0_to_1_keeps
 * @brief   A central whose halo becomes a subhalo (Type 0 -> 1) makes no call and keeps its
 *          value; it holds valid current and infall pairs, so any write would be visible
 */
int test_join_type0_to_1_keeps(void) {
  TEST_ASSERT(init_ok("3") == 0, "init must succeed");
  struct GalaxyPool *pool = galaxy_pool_create(4);
  struct Halo source;
  struct GalaxyData source_gal;
  make_progenitor(&source, &source_gal, 0, 50.0, 170.0, 150.0, -1.0, -1.0);
  const struct InheritanceDescendant d = make_descendant(40.0, 140.0, 160.0f, 0);
  struct Halo workspace[2];
  const int end = join_then_process(pool, workspace, &d, &source);
  TEST_ASSERT(end == 1, "the join must keep one row, and process must succeed");

  printf("\n  Type 0 -> 1: Type %d, Concentration %.9g, infall pair %.9g/%.9g", workspace[0].Type,
         (double)workspace[0].galaxy->Concentration, workspace[0].infallVmax,
         workspace[0].infallVvir);
  TEST_ASSERT(workspace[0].Type == 1 && workspace[0].infallVmax == 170.0 &&
                  workspace[0].infallVvir == 150.0,
              "the joined row must be a satellite with its infall pair recorded");
  TEST_ASSERT(bits32(workspace[0].galaxy->Concentration) == bits32(SEED_CONCENTRATION),
              "a central that becomes a satellite must keep its Concentration");
  galaxy_pool_destroy(pool);
  sage26_set_halo_concentration_cleanup();
  return TEST_PASS;
}

// ============================================================================
// MAIN TEST RUNNER
// ============================================================================

int main(void) {
  printf("%s", BLUE);
  printf("============================================================\n");
  printf("Test Suite: sage26_set_halo_concentration Module\n");
  printf("============================================================\n");
  printf("%s", NC);

  initialize_error_handling(LOG_LEVEL_WARNING, NULL);

  printf("\n%s--- Software Quality Tests ---%s\n", BLUE, NC);
  TEST_RUN(test_module_lifecycle);
  TEST_RUN(test_concentration_on_missing_refused);
  TEST_RUN(test_concentration_on_values);
  TEST_RUN(test_placement_refused);
  TEST_RUN(test_recorder_required_at_3);

  printf("\n%s--- Assignment Tests ---%s\n", BLUE, NC);
  TEST_RUN(test_current_and_infall_pairs);
  TEST_RUN(test_kept_rows);
  TEST_RUN(test_float_store);
  TEST_RUN(test_ratio_cases);
  TEST_RUN(test_off_writes_nothing);
  TEST_RUN(test_empty_and_null_rows);

  printf("\n%s--- Joining Composition Tests ---%s\n", BLUE, NC);
  TEST_RUN(test_join_type1_to_0_infall_pair);
  TEST_RUN(test_join_type0_to_0_updated_current_pair);
  TEST_RUN(test_join_type0_to_1_keeps);

  TEST_SUMMARY();
  return TEST_RESULT();
}
