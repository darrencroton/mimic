/**
 * @file    test_rate_mode_contract.c
 * @brief   Unit tests for the declared rate mode (process_rates) and its callback family
 *
 * These tests pin the mode's declaration and placement; the rate phase's
 * dispatch is tested by test_rate_dispatch.c:
 * - Registration of hand-built modules: a module advertising process_rates with a
 *   NULL rates callback is fatal (a callback of another family does not satisfy
 *   it), a module with the callback registers, and a FoF mode that lacks its own
 *   process callback stays fatal even when the rates callback is present
 * - The rate mode in the C mode table: its name, family and parsing
 * - Placement: process_rates is refused in pre_timestep, post_timestep and
 *   post_snapshot, and for a module that does not advertise it, before any
 *   init() with a diagnostic naming the phase and the module; a named substep
 *   phase holding only process_rates entries (the rate phase) is accepted
 * - struct RateContext as a module sees it: the evaluation's own time, a NULL
 *   central for a module that did not declare reads_central, and no solver state
 * - Generated registration of the four rate fixtures: register_all_modules()
 *   succeeds with them, and has registered each one (registering it again is
 *   refused as a duplicate)
 *
 * Registration aborts through FATAL_ERROR and the registry has no unregister, so
 * each case runs in a forked child (child_capture.h).
 */

#include "framework/test_framework.h"
#include "framework/child_capture.h"
#include "../../core/module_registry.h"
#include "framework/test_phase_config.h"
#include "../../core/module_interface.h"
#include "../../include/types.h"
#include "../../include/globals.h"
#include "../../util/error.h"
#include "../../util/memory.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Test statistics (required for TEST_RUN macro) */
static int passed = 0;
static int failed = 0;

static void reset_config(void) { memset(&MimicConfig, 0, sizeof(MimicConfig)); }

/* ==========================================================================
 * Hand-built modules
 * ========================================================================== */

/** Calls of probe_init() in this process; non-zero means an init() ran */
static int init_calls = 0;

static int probe_init(void) {
  init_calls++;
  return 0;
}
static int probe_cleanup(void) { return 0; }

/** FoF callback of the valid control module; never reached by these tests */
static int fof_probe_process(struct ModuleContext *ctx, struct Halo *halos, int ngal) {
  (void)ctx;
  (void)halos;
  (void)ngal;
  return 0;
}

/** Rate callback: writes one coefficient derived from the context, for the layout test */
static int rate_probe_rates(const struct RateContext *ctx, const struct Halo *halo,
                            double *coefficients) {
  (void)halo;
  coefficients[0] = ctx->time + (double)ctx->snapshot_number;
  return ctx->central == NULL ? 0 : 1;
}

static const enum ProcessingMode rates_only_modes[] = {PROCESSING_MODE_RATES};
static const enum ProcessingMode rates_and_full_halo_modes[] = {PROCESSING_MODE_FULL_HALO,
                                                                PROCESSING_MODE_RATES};
static const enum ProcessingMode by_galaxy_modes[] = {PROCESSING_MODE_BY_GALAXY};

/**
 * @brief   Every hand-built registration case, by name
 *
 * Each rejected case differs from an accepted one by exactly the defect its
 * name states.
 */
static const struct Module registration_cases[] = {
    {.name = "rates_only",
     .init = probe_init,
     .cleanup = probe_cleanup,
     .rates = rate_probe_rates,
     .supported_processing_modes = rates_only_modes,
     .num_supported_modes = 1},
    {.name = "rates_and_full_halo",
     .init = probe_init,
     .cleanup = probe_cleanup,
     .process = fof_probe_process,
     .rates = rate_probe_rates,
     .supported_processing_modes = rates_and_full_halo_modes,
     .num_supported_modes = 2},
    {.name = "rates_missing_callback",
     .init = probe_init,
     .cleanup = probe_cleanup,
     .supported_processing_modes = rates_only_modes,
     .num_supported_modes = 1},
    {.name = "rates_with_only_a_fof_callback",
     .init = probe_init,
     .cleanup = probe_cleanup,
     .process = fof_probe_process,
     .supported_processing_modes = rates_only_modes,
     .num_supported_modes = 1},
    {.name = "dual_missing_rates",
     .init = probe_init,
     .cleanup = probe_cleanup,
     .process = fof_probe_process,
     .supported_processing_modes = rates_and_full_halo_modes,
     .num_supported_modes = 2},
    {.name = "dual_missing_fof",
     .init = probe_init,
     .cleanup = probe_cleanup,
     .rates = rate_probe_rates,
     .supported_processing_modes = rates_and_full_halo_modes,
     .num_supported_modes = 2},
};

/** @brief Child body: register the named case; a rejected case aborts in the registry */
static void register_case_body(const char *which) {
  static struct Module mod;
  for (size_t i = 0; i < sizeof(registration_cases) / sizeof(registration_cases[0]); i++) {
    if (strcmp(registration_cases[i].name, which) == 0) {
      mod = registration_cases[i];
      module_registry_add(&mod);
      return;
    }
  }
  fprintf(stderr, "no registration case named '%s'\n", which);
  exit(3);
}

/* ==========================================================================
 * Tests
 * ========================================================================== */

/**
 * @test    test_rate_mode_table_entry
 * @brief   process_rates is in the C mode table, in the rate family, and parses
 */
int test_rate_mode_table_entry(void) {
  /* ===== SETUP ===== */
  enum ProcessingMode parsed = PROCESSING_MODE_COUNT;
  enum ModuleCallbackFamily family = MODULE_CALLBACK_FAMILY_FOF;

  /* ===== EXECUTE / VALIDATE ===== */
  TEST_ASSERT_STRING_EQUAL(processing_mode_to_string(PROCESSING_MODE_RATES), "process_rates",
                           "the rate mode's configuration name");
  TEST_ASSERT_EQUAL(processing_mode_from_string("process_rates", &parsed), 0,
                    "process_rates parses");
  TEST_ASSERT_EQUAL((int)parsed, (int)PROCESSING_MODE_RATES, "process_rates parses to its mode");
  TEST_ASSERT_EQUAL(processing_mode_family(PROCESSING_MODE_RATES, &family), 0,
                    "the rate mode has a family");
  TEST_ASSERT_EQUAL((int)family, (int)MODULE_CALLBACK_FAMILY_RATE, "it is the rate family");
  TEST_ASSERT_EQUAL(PROCESSING_MODE_RATES, PROCESSING_MODE_COUNT - 1,
                    "the rate mode is appended after every existing mode");
  return TEST_PASS;
}

/**
 * @test    test_rate_registration_requires_callback
 * @brief   A rates module registers with its callback and is fatal without it
 */
int test_rate_registration_requires_callback(void) {
  /* ===== EXECUTE / VALIDATE: accepted ===== */
  const char *accepted[] = {"rates_only", "rates_and_full_halo"};
  for (size_t i = 0; i < sizeof(accepted) / sizeof(accepted[0]); i++) {
    const int ok = expect_success(accepted[i], register_case_body);
    if (ok != 1) {
      fprintf(stderr, "  accepted case: %s\n", accepted[i]);
    }
    TEST_ASSERT_EQUAL(ok, 1, "a module advertising process_rates with its callback registers");
  }

  /* ===== EXECUTE / VALIDATE: rejected ===== */
  struct {
    const char *which;
    const char *needle;
  } rejected[] = {
      {"rates_missing_callback", "advertises rate mode 'process_rates' but has a NULL rates"},
      {"rates_with_only_a_fof_callback", "has a NULL rates callback"},
      {"dual_missing_rates", "has a NULL rates callback"},
      {"dual_missing_fof", "NULL process callback"},
  };
  for (size_t i = 0; i < sizeof(rejected) / sizeof(rejected[0]); i++) {
    const int ok =
        expect_fatal(rejected[i].which, register_case_body, rejected[i].which, rejected[i].needle);
    TEST_ASSERT_EQUAL(ok, 1, "a missing advertised callback is fatal at registration");
  }
  return TEST_PASS;
}

/* Generated registrations of the four rate fixtures (rate modules have external linkage) */
extern struct Module test_rate_supply_module;
extern struct Module test_rate_formation_module;
extern struct Module test_rate_feedback_module;
extern struct Module test_rate_stripping_module;

/** @brief Child body: run the generated registration of every compiled module */
static void register_generated_body(const char *unused) {
  (void)unused;
  register_all_modules();
}

/** @brief Child body: run the generated registration, then register one rate fixture again */
static void register_generated_then_repeat_body(const char *which) {
  register_all_modules();
  struct Module *const fixtures[] = {&test_rate_supply_module, &test_rate_formation_module,
                                     &test_rate_feedback_module, &test_rate_stripping_module};
  for (size_t i = 0; i < sizeof(fixtures) / sizeof(fixtures[0]); i++) {
    if (strcmp(fixtures[i]->name, which) == 0) {
      module_registry_add(fixtures[i]);
    }
  }
}

/**
 * @test    test_generated_rate_fixtures_register
 * @brief   Generated registration includes the four rate fixtures and passes the family checks
 *
 * register_all_modules() aborts if a module advertising process_rates has no
 * rates callback, so its success shows every rate fixture's binding is valid;
 * registering a fixture a second time is then refused as already registered,
 * which shows the generated registration added it.
 */
int test_generated_rate_fixtures_register(void) {
  TEST_ASSERT_EQUAL(expect_success(NULL, register_generated_body), 1,
                    "generated registration with the rate fixtures succeeds");

  const char *const fixtures[] = {"test_rate_supply", "test_rate_formation", "test_rate_feedback",
                                  "test_rate_stripping"};
  char needle[128];
  for (size_t i = 0; i < sizeof(fixtures) / sizeof(fixtures[0]); i++) {
    snprintf(needle, sizeof(needle), "Module '%s' is already registered", fixtures[i]);
    TEST_ASSERT_EQUAL(
        expect_fatal(fixtures[i], register_generated_then_repeat_body, fixtures[i], needle), 1,
        "the generated registration has registered the rate fixture");
  }
  return TEST_PASS;
}

/** One illegal placement of process_rates and what startup must say about it */
struct PlacementCase {
  const char *phase;  /**< pre_timestep, a substep phase name, post_timestep or post_snapshot */
  const char *module; /**< Module configured with process_rates */
};

/** The case placement_case_body() runs; set by the parent before each fork */
static const struct PlacementCase *current_case = NULL;

/** Name of the one named substep phase used by the substep-phase case */
#define SUBSTEP_PHASE "rate_substep"

/**
 * @brief   Child body: configure a valid FoF entry plus process_rates in one phase
 *
 * Exits 1 only when module_system_init() rejected the configuration before any
 * init() ran (the valid control module's init() counts calls) and cleanup then
 * ran; any other outcome returns, so the child exits 0 and expect_fatal() prints
 * the reason and the captured diagnostics.
 */
static void placement_case_body(const char *unused) {
  (void)unused;
  static struct Module fof_control = {.name = "fof_control",
                                      .init = probe_init,
                                      .cleanup = probe_cleanup,
                                      .process = fof_probe_process,
                                      .supported_processing_modes = by_galaxy_modes,
                                      .num_supported_modes = 1};
  static struct Module rate_module = {.name = "rate_probe",
                                      .init = probe_init,
                                      .cleanup = probe_cleanup,
                                      .rates = rate_probe_rates,
                                      .supported_processing_modes = rates_only_modes,
                                      .num_supported_modes = 1};
  const struct PlacementCase *c = current_case;

  reset_config();
  init_memory_system(0);
  module_registry_add(&fof_control);
  module_registry_add(&rate_module);
  MimicConfig.SubSteps = 1;

  /* A valid control entry in a FoF phase, so a premature init() would be visible. */
  test_phase_add("control_phase", "fof_control", PROCESSING_MODE_BY_GALAXY);
  if (strcmp(c->phase, POST_SNAPSHOT_PHASE_NAME) == 0) {
    test_post_snapshot_add(c->module, PROCESSING_MODE_RATES);
  } else if (strcmp(c->phase, "pre_timestep") == 0) {
    test_pre_timestep_add(c->module, PROCESSING_MODE_RATES);
  } else if (strcmp(c->phase, "post_timestep") == 0) {
    MimicConfig.post_timestep = mymalloc_cat(sizeof(struct PhaseModuleConfig), MEM_UTILITY);
    MimicConfig.post_timestep[0].module_name = strdup(c->module);
    MimicConfig.post_timestep[0].processing_mode = PROCESSING_MODE_RATES;
    MimicConfig.post_timestep[0].resolved = NULL;
    MimicConfig.num_post_timestep = 1;
  } else {
    test_phase_add(c->phase, c->module, PROCESSING_MODE_RATES);
  }

  if (module_system_init() != -1) {
    fprintf(stderr, "case accepted: process_rates in '%s'\n", c->phase);
    return;
  }
  if (init_calls != 0) {
    fprintf(stderr, "case %s: a module init() ran before the rejection\n", c->phase);
    return;
  }
  module_system_cleanup();
  exit(1);
}

/**
 * @brief   Child body: the placement of current_case initialises, and every init() ran
 *
 * Exits 2 when module_system_init() refuses the configuration or skips an
 * init(); returns (exit 0) otherwise.
 */
static void accepted_placement_body(const char *unused) {
  (void)unused;
  static struct Module rate_module = {.name = "rate_probe",
                                      .init = probe_init,
                                      .cleanup = probe_cleanup,
                                      .rates = rate_probe_rates,
                                      .supported_processing_modes = rates_only_modes,
                                      .num_supported_modes = 1};
  reset_config();
  init_memory_system(0);
  module_registry_add(&rate_module);
  MimicConfig.SubSteps = 1;
  MimicConfig.RateIntegration = (struct MimicRateIntegration){
      .scheme = RATE_INTEGRATION_DEFAULT_SCHEME,
      .relative_tolerance = RATE_INTEGRATION_DEFAULT_RELATIVE_TOLERANCE,
      .absolute_tolerance = RATE_INTEGRATION_DEFAULT_ABSOLUTE_TOLERANCE,
      .max_steps_per_segment = RATE_INTEGRATION_DEFAULT_MAX_STEPS_PER_SEGMENT,
      .max_rejections_per_step = RATE_INTEGRATION_DEFAULT_MAX_REJECTIONS_PER_STEP};
  test_phase_add(current_case->phase, current_case->module, PROCESSING_MODE_RATES);
  if (module_system_init() != 0 || init_calls != 1) {
    fprintf(stderr, "rate phase '%s' refused or init() count %d\n", current_case->phase,
            init_calls);
    exit(2);
  }
  module_system_cleanup();
}

/**
 * @test    test_rate_mode_placement
 * @brief   process_rates outside a named substep phase, or for a module that does not
 *          advertise it, is refused at startup, before any init(), naming the phase
 *          and the module; a named substep phase of process_rates entries is accepted
 */
int test_rate_mode_placement(void) {
  static const struct PlacementCase cases[] = {
      {"pre_timestep", "rate_probe"},
      {"post_timestep", "rate_probe"},
      {POST_SNAPSHOT_PHASE_NAME, "rate_probe"},
      /* A module that does not advertise the mode is refused for the support. */
      {SUBSTEP_PHASE, "fof_control"},
  };

  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    /* ===== SETUP ===== */
    char phase_needle[128];
    char module_needle[192];
    snprintf(phase_needle, sizeof(phase_needle), "Configuration error in phase '%s'",
             cases[i].phase);
    if (strcmp(cases[i].module, "fof_control") == 0) {
      snprintf(module_needle, sizeof(module_needle),
               "Module '%s' does not support processing mode 'process_rates'", cases[i].module);
    } else {
      snprintf(module_needle, sizeof(module_needle),
               "Module '%s' is configured with processing mode 'process_rates'", cases[i].module);
    }
    current_case = &cases[i];

    /* ===== EXECUTE / VALIDATE ===== */
    const int ok = expect_fatal(cases[i].phase, placement_case_body, phase_needle, module_needle);
    if (ok != 1) {
      fprintf(stderr, "  rejected case: process_rates in '%s' for '%s'\n", cases[i].phase,
              cases[i].module);
    }
    TEST_ASSERT_EQUAL(ok, 1, "process_rates is refused before any init(), naming phase and module");
  }

  /* ===== The rate phase itself: a named substep phase of process_rates entries ===== */
  static const struct PlacementCase accepted = {SUBSTEP_PHASE, "rate_probe"};
  current_case = &accepted;
  TEST_ASSERT_EQUAL(expect_success(NULL, accepted_placement_body), 1,
                    "a named substep phase holding only process_rates is accepted");
  return TEST_PASS;
}

/**
 * @test    test_rate_context_as_a_module_sees_it
 * @brief   A rates callback can read every struct RateContext field it is handed
 *
 * The context here is hand-built and the callback is the test's own, so this
 * exercises no core path: whether the central is NULL or non-NULL is the
 * caller's choice here; the core's choice (NULL unless the module declares
 * reads_central) is tested by test_rate_dispatch.c. The Python mirror test
 * pins the field list, which carries no solver step, substep index or substep count.
 */
int test_rate_context_as_a_module_sees_it(void) {
  /* ===== SETUP ===== */
  struct Halo halo;
  struct Halo central;
  memset(&halo, 0, sizeof(halo));
  memset(&central, 0, sizeof(central));
  double coefficients[1] = {0.0};
  struct RateContext ctx = {
      .snapshot_number = 7, .redshift = 0.5, .time = 2.25, .central = NULL, .params = &MimicConfig};
  const struct Module mod = registration_cases[0];

  /* ===== EXECUTE / VALIDATE ===== */
  TEST_ASSERT(mod.rates == rate_probe_rates, "the Module member binds the callback");
  TEST_ASSERT_EQUAL(mod.rates(&ctx, &halo, coefficients), 0,
                    "the callback sees the NULL central it is handed");
  TEST_ASSERT(coefficients[0] == 9.25, "the callback reads the row's own time and the snapshot");

  ctx.central = &central;
  TEST_ASSERT_EQUAL(mod.rates(&ctx, &halo, coefficients), 1,
                    "the callback sees the non-NULL central it is handed");
  return TEST_PASS;
}

int main(void) {
  printf("%s", BLUE);
  printf("============================================================\n");
  printf("Test Suite: rate mode contract\n");
  printf("============================================================\n");
  printf("%s", NC);

  TEST_RUN(test_rate_mode_table_entry);
  TEST_RUN(test_rate_registration_requires_callback);
  TEST_RUN(test_rate_mode_placement);
  TEST_RUN(test_rate_context_as_a_module_sees_it);
  TEST_RUN(test_generated_rate_fixtures_register);

  TEST_SUMMARY();
  return TEST_RESULT();
}
