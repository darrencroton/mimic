/**
 * @file    test_rate_dispatch.c
 * @brief   Unit tests for the rate phase through real dispatch (module_registry.c, rate_assembly.c)
 *
 * Validates, on hand-built FoF workspaces, hand-built probe modules and the
 * generated test-build modules:
 * - The startup rules of the rate phase, table-driven: process_rates mixed with
 *   another mode, in pre_timestep, post_timestep or post_snapshot, in a second
 *   phase, or listed twice; a slaved source whose master's module is not in the
 *   phase; a dependency cycle among totals (named); a transfer whose end has the
 *   wrong kind in the reservoir table; settings out of range. Each refusal comes
 *   from module_system_init() before any init(), naming the phase and the module,
 *   transfer or totals
 * - The cycle finder on its own, on graphs with and without a cycle
 * - The run-file defaults (constants.h) equal the integrator's own defaults
 * - The domain: with no central transfer and no central read, a galaxy's end
 *   state is bit-identical whatever the other rows hold; with a central reader
 *   it is not
 * - The central view: a module declaring reads_central gets the central's
 *   scratch copy at the trial state and makes the group one component; a module
 *   that does not gets NULL; test_rate_stripping makes the group one component
 * - The first-snapshot sentinel: an all-sentinel group is unchanged and runs no
 *   law; a non-sentinel row with dT <= 0 is fatal, naming the row
 * - A rate law can neither emit an event nor create a record
 * - A law returning NaN is fatal, naming the module, its transfer and the cause;
 *   for a source into the central, the message names the satellite row whose
 *   law returned it as well as the central's recipient entry
 * - The failure file of a failed segment: written to the output directory before
 *   the run ends, holding the segment-start state, each row's Delta and the
 *   failing evaluation's coefficients as hexadecimal floats; a coefficient whose
 *   law had not run (a law returned non-zero mid-group) is marked not_evaluated,
 *   and a segment that failed before any law ran lists none, even when an earlier
 *   component left coefficients in the scratch
 * - The purity probe (check_purity): with it off no law is evaluated beyond the
 *   integrator's own; with it on every law is evaluated once more, in reversed
 *   canonical order, at a segment's first evaluation; a law whose coefficient
 *   changes between the two evaluations, or that writes its scratch row, is
 *   fatal naming the module and transfer, and runs to completion with the probe off
 * - A law reading an observer sees its segment-start value at every evaluation,
 *   and the written-back observer is its start plus the committed amounts
 * - A large per-galaxy workspace gives each row the end state it gets alone
 * - A row retired by a discrete phase after the rate phase leaves the next segment
 * - A record test_fixture creates in pre_timestep joins the component at zero content
 * - The scratch is one tracked allocation, grown to its high water and released
 *   by module_system_cleanup()
 *
 * Fatal paths run in forked children (child_capture.h). Unit tests always run
 * against test-build generated code, so the fixture reservoir table and the
 * rate fixture modules are always compiled here.
 */

#include "framework/test_framework.h"
#include "framework/child_capture.h"
#include "core/fof_workspace.h"
#include "core/galaxy_pool.h"
#include "core/module_interface.h"
#include "core/module_registry.h"
#include "core/rate_assembly.h"
#include "core/rate_integrator.h"
#include "framework/test_phase_config.h"
#include "include/constants.h"
#include "include/globals.h"
#include "include/proto.h"
#include "include/types.h"
#include "include/generated/reservoir_table.h"
#include "util/error.h"
#include "util/memory.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "framework/core_test_fixtures.h"

/* Test statistics (required for TEST_RUN macro) */
static int passed = 0;
static int failed = 0;

/** The rate phase's name in every case */
#define RATE_PHASE "rates"

/** First UniqueGalaxyID of a hand-built workspace; row i has ID ROW_ID_BASE + i */
#define ROW_ID_BASE 1000

/** Most rows a hand-built workspace has */
#define MAX_ROWS 8

/** Law calls a probe records */
#define MAX_CALLS 200000

/* ==========================================================================
 * Probe state
 * ========================================================================== */

/** Calls of any probe init(); non-zero means an init() ran */
static int init_calls = 0;

static int probe_init(void) {
  init_calls++;
  return 0;
}
static int probe_cleanup(void) { return 0; }

/** Coefficients of the counting probe: a source into Cold, and a sink out of it */
static double feed_rate = 0.0;
static double drain_rate = 1.0;

/** One recorded law call */
struct LawCall {
  int row;                    /* UniqueGalaxyID - ROW_ID_BASE, or -1 for a created row */
  long long id;               /* the row's UniqueGalaxyID */
  double cold;                /* the row's trial RateFixtureCold */
  const struct Halo *halo;    /* the scratch row the law received */
  const struct Halo *central; /* RateContext.central */
  double central_cold;        /* the central view's RateFixtureCold, or NaN */
  int phase_mark;             /* discrete_phase_calls when the law ran */
};

static struct LawCall calls[MAX_CALLS];
static int num_calls = 0;

/** Calls of the full-halo probes; the counting law records it with every call */
static int discrete_phase_calls = 0;

static void reset_probes(void) {
  num_calls = 0;
  discrete_phase_calls = 0;
  feed_rate = 0.0;
  drain_rate = 1.0;
}

static void record_call(const struct RateContext *ctx, const struct Halo *halo) {
  if (num_calls >= MAX_CALLS) {
    return;
  }
  struct LawCall *c = &calls[num_calls++];
  c->id = halo->UniqueGalaxyID;
  c->row = halo->UniqueGalaxyID >= ROW_ID_BASE ? (int)(halo->UniqueGalaxyID - ROW_ID_BASE) : -1;
  c->cold = halo->galaxy->RateFixtureCold;
  c->halo = halo;
  c->central = ctx->central;
  c->central_cold = ctx->central != NULL ? ctx->central->galaxy->RateFixtureCold : NAN;
  c->phase_mark = discrete_phase_calls;
}

/** Counting law: feed into Cold, and a drain whose coefficient grows with the content */
static int count_rates(const struct RateContext *ctx, const struct Halo *halo,
                       double *coefficients) {
  record_call(ctx, halo);
  coefficients[0] = feed_rate;
  coefficients[1] = drain_rate * (1.0 + halo->galaxy->RateFixtureCold);
  return 0;
}

/** Central-reading law: a drain out of Cold whose coefficient grows with the central's Cold */
static int central_reader_rates(const struct RateContext *ctx, const struct Halo *halo,
                                double *coefficients) {
  record_call(ctx, halo);
  if (ctx->central == NULL) {
    return 7;
  }
  coefficients[0] = 1.0 + ctx->central->galaxy->RateFixtureCold;
  return 0;
}

static int nan_rates(const struct RateContext *ctx, const struct Halo *halo, double *coefficients) {
  (void)ctx;
  (void)halo;
  coefficients[0] = NAN;
  return 0;
}

/** Impure law: its coefficient depends on how many times it has been called */
static unsigned long impure_counter = 0;
static int impure_rates(const struct RateContext *ctx, const struct Halo *halo,
                        double *coefficients) {
  (void)ctx;
  (void)halo;
  coefficients[0] = 1.0 + 1.0e-3 * (double)(++impure_counter);
  return 0;
}

/** Scratch-writing law: a constant coefficient, and a write into the scratch galaxy it was given */
static int scratch_writer_rates(const struct RateContext *ctx, const struct Halo *halo,
                                double *coefficients) {
  (void)ctx;
  halo->galaxy->RateFixtureFormed += 1.0;
  coefficients[0] = 0.5;
  return 0;
}

/** Fails (returns 5) on a Type 1 row, else a small constant coefficient */
static int fail_on_satellite_rates(const struct RateContext *ctx, const struct Halo *halo,
                                   double *coefficients) {
  (void)ctx;
  if (halo->Type == 1) {
    return 5;
  }
  coefficients[0] = 0.1;
  return 0;
}

/** NaN on satellite rows only, for a source into the central */
static int nan_central_rates(const struct RateContext *ctx, const struct Halo *halo,
                             double *coefficients) {
  (void)ctx;
  coefficients[0] = (halo->Type == 0) ? 0.0 : NAN;
  return 0;
}

/** RateFixtureFormed as each observer_probe call saw it */
static double observed_formed[MAX_CALLS];
static int num_observed = 0;

/** Formation law that reads its own observer: Cold to Stars at 1 + Cold */
static int observer_rates(const struct RateContext *ctx, const struct Halo *halo,
                          double *coefficients) {
  (void)ctx;
  if (num_observed < MAX_CALLS) {
    observed_formed[num_observed++] = halo->galaxy->RateFixtureFormed;
  }
  coefficients[0] = 1.0 + halo->galaxy->RateFixtureCold;
  return 0;
}

/** The context a full-halo probe saw, kept for the rate laws that misuse it */
static struct ModuleContext *saved_context = NULL;
static int emit_result = 0;
static int create_result = 0;
static bool misuse_tried = false;

static int emit_rates(const struct RateContext *ctx, const struct Halo *halo,
                      double *coefficients) {
  (void)ctx;
  (void)halo;
  coefficients[0] = 0.0;
  if (!misuse_tried) {
    misuse_tried = true;
    emit_result = module_emit_event(saved_context, 1, 0, 0, 0.0, 0.0);
  }
  return 0;
}

static int create_rates(const struct RateContext *ctx, const struct Halo *halo,
                        double *coefficients) {
  (void)ctx;
  (void)halo;
  coefficients[0] = 0.0;
  if (!misuse_tried) {
    struct Halo *row = NULL;
    misuse_tried = true;
    create_result = module_create_record(saved_context, 0, &row);
  }
  return 0;
}

/** Rate law of the startup-rule probes; never reached */
static int idle_rates(const struct RateContext *ctx, const struct Halo *halo,
                      double *coefficients) {
  (void)ctx;
  (void)halo;
  (void)coefficients;
  return 0;
}

/** Full-halo probe: keeps the context, for the laws that misuse it */
static int saver_process(struct ModuleContext *ctx, struct Halo *halos, int ngal) {
  (void)halos;
  (void)ngal;
  saved_context = ctx;
  discrete_phase_calls++;
  return 0;
}

/** Row the retire probe retires on its first call */
#define RETIRED_ROW 2
static double retired_cold = 0.0;

/** Full-halo probe: retires RETIRED_ROW (Type 3) on its first call */
static int retire_process(struct ModuleContext *ctx, struct Halo *halos, int ngal) {
  (void)ctx;
  if (discrete_phase_calls == 0 && ngal > RETIRED_ROW) {
    halos[RETIRED_ROW].Type = 3;
    retired_cold = halos[RETIRED_ROW].galaxy->RateFixtureCold;
  }
  discrete_phase_calls++;
  return 0;
}

/** By-galaxy probe for the mixing rule; never reached */
static int fof_idle_process(struct ModuleContext *ctx, struct Halo *halos, int ngal) {
  (void)ctx;
  (void)halos;
  (void)ngal;
  return 0;
}

/* ==========================================================================
 * Probe modules
 * ========================================================================== */

static const enum ProcessingMode rates_modes[] = {PROCESSING_MODE_RATES};
static const enum ProcessingMode full_halo_modes[] = {PROCESSING_MODE_FULL_HALO};
static const enum ProcessingMode by_galaxy_modes[] = {PROCESSING_MODE_BY_GALAXY};

/** A transfer descriptor as the generator emits one */
#define TRANSFER(NAME, SLOT, GLOBAL, KIND, FROM, TO, RECORD, MASTER, SLAVED_TO)                    \
  {.name = (NAME),                                                                                 \
   .coefficient_index = (SLOT),                                                                    \
   .global_index = (GLOBAL),                                                                       \
   .kind = (KIND),                                                                                 \
   .from = (FROM),                                                                                 \
   .to = (TO),                                                                                     \
   .to_record = (RECORD),                                                                          \
   .slaved_master = (MASTER),                                                                      \
   .slaved_to = (SLAVED_TO)}

#define EXTERNAL RATE_TRANSFER_EXTERNAL
#define SELF RATE_TRANSFER_TO_SELF

static const struct RateTransfer count_transfers[] = {
    TRANSFER("feed", 0, 100, RATE_TRANSFER_SOURCE, EXTERNAL, RESERVOIR_INDEX_RateFixtureCold, SELF,
             -1, NULL),
    TRANSFER("drain", 1, 101, RATE_TRANSFER_SINK, RESERVOIR_INDEX_RateFixtureCold, EXTERNAL, SELF,
             -1, NULL),
};
static const struct RateTransfer central_reader_transfers[] = {
    TRANSFER("drain", 0, 110, RATE_TRANSFER_SINK, RESERVOIR_INDEX_RateFixtureCold, EXTERNAL, SELF,
             -1, NULL),
};
static const struct RateTransfer nan_transfers[] = {
    TRANSFER("poison", 0, 120, RATE_TRANSFER_SINK, RESERVOIR_INDEX_RateFixtureCold, EXTERNAL, SELF,
             -1, NULL),
};
static const struct RateTransfer impure_transfers[] = {
    TRANSFER("seep", 0, 190, RATE_TRANSFER_SINK, RESERVOIR_INDEX_RateFixtureCold, EXTERNAL, SELF,
             -1, NULL),
};
static const struct RateTransfer scratch_writer_transfers[] = {
    TRANSFER("scribble", 0, 191, RATE_TRANSFER_SINK, RESERVOIR_INDEX_RateFixtureCold, EXTERNAL,
             SELF, -1, NULL),
};
static const struct RateTransfer fail_on_satellite_transfers[] = {
    TRANSFER("boom", 0, 192, RATE_TRANSFER_SINK, RESERVOIR_INDEX_RateFixtureCold, EXTERNAL, SELF,
             -1, NULL),
};
static const struct RateTransfer misuse_transfers[] = {
    TRANSFER("noop", 0, 130, RATE_TRANSFER_SINK, RESERVOIR_INDEX_RateFixtureCold, EXTERNAL, SELF,
             -1, NULL),
};
/* An internal transfer whose donor is a tracer: the reservoir table refuses the kind. */
static const struct RateTransfer wrong_kind_transfers[] = {
    TRANSFER("leak", 0, 140, RATE_TRANSFER_INTERNAL, RESERVOIR_INDEX_RateFixtureHotTracer,
             RESERVOIR_INDEX_RateFixtureCold, SELF, -1, NULL),
};
static const struct RateTransfer master_transfers[] = {
    TRANSFER("form", 0, 150, RATE_TRANSFER_INTERNAL, RESERVOIR_INDEX_RateFixtureCold,
             RESERVOIR_INDEX_RateFixtureStars, SELF, -1, NULL),
};
static const struct RateTransfer slaved_transfers[] = {
    TRANSFER("enrich", 0, 151, RATE_TRANSFER_SLAVED_SOURCE, EXTERNAL,
             RESERVOIR_INDEX_RateFixtureColdTracer, SELF, 150, "master_probe.form"),
};
/* A slaved source into the mass total, mastered by a transfer out of the mass total. */
static const struct RateTransfer cycle_transfers[] = {
    TRANSFER("form", 0, 160, RATE_TRANSFER_INTERNAL, RESERVOIR_INDEX_RateFixtureCold,
             RESERVOIR_INDEX_RateFixtureStars, SELF, -1, NULL),
    TRANSFER("loop", 1, 161, RATE_TRANSFER_SLAVED_SOURCE, EXTERNAL, RESERVOIR_INDEX_RateFixtureHot,
             SELF, 160, "cycle_probe.form"),
};

#define RATE_MODULE(NAME, LAW, TABLE, READS_CENTRAL)                                               \
  {.name = (NAME),                                                                                 \
   .init = probe_init,                                                                             \
   .cleanup = probe_cleanup,                                                                       \
   .rates = (LAW),                                                                                 \
   .supported_processing_modes = rates_modes,                                                      \
   .num_supported_modes = 1,                                                                       \
   .rate_transfers = (TABLE),                                                                      \
   .num_rate_transfers = (int)(sizeof(TABLE) / sizeof((TABLE)[0])),                                \
   .reads_central = (READS_CENTRAL)}

static struct Module count_probe = RATE_MODULE("count_probe", count_rates, count_transfers, false);
static struct Module central_reader =
    RATE_MODULE("central_reader", central_reader_rates, central_reader_transfers, true);
static struct Module nan_probe = RATE_MODULE("nan_probe", nan_rates, nan_transfers, false);
static struct Module impure_probe =
    RATE_MODULE("impure_probe", impure_rates, impure_transfers, false);
static struct Module scratch_writer_probe =
    RATE_MODULE("scratch_writer_probe", scratch_writer_rates, scratch_writer_transfers, false);
/* Reads the central only to make the group one component, so its rows fail in sequence. */
static struct Module fail_on_satellite_probe = RATE_MODULE(
    "fail_on_satellite_probe", fail_on_satellite_rates, fail_on_satellite_transfers, true);
static struct Module emit_probe = RATE_MODULE("emit_probe", emit_rates, misuse_transfers, false);
static struct Module create_probe =
    RATE_MODULE("create_probe", create_rates, misuse_transfers, false);
static struct Module wrong_kind_probe =
    RATE_MODULE("wrong_kind_probe", idle_rates, wrong_kind_transfers, false);
static struct Module master_probe =
    RATE_MODULE("master_probe", idle_rates, master_transfers, false);
static struct Module slaved_probe =
    RATE_MODULE("slaved_probe", idle_rates, slaved_transfers, false);
static struct Module cycle_probe = RATE_MODULE("cycle_probe", idle_rates, cycle_transfers, false);

/* A source into the central's Cold, evaluated on every row. */
static const struct RateTransfer nan_central_transfers[] = {
    TRANSFER("gift", 0, 180, RATE_TRANSFER_SOURCE, EXTERNAL, RESERVOIR_INDEX_RateFixtureCold,
             RATE_TRANSFER_TO_CENTRAL, -1, NULL),
};
static struct Module nan_central_probe =
    RATE_MODULE("nan_central_probe", nan_central_rates, nan_central_transfers, false);

/* Cold to Stars, observed into RateFixtureFormed. */
static const struct RateTransfer observer_transfers[] = {
    TRANSFER("form", 0, 170, RATE_TRANSFER_INTERNAL, RESERVOIR_INDEX_RateFixtureCold,
             RESERVOIR_INDEX_RateFixtureStars, SELF, -1, NULL),
};
static const struct RateObserver observer_observers[] = {
    {.property = RESERVOIR_INDEX_RateFixtureFormed, .transfer = 0},
};
static struct Module observer_probe = {.name = "observer_probe",
                                       .init = probe_init,
                                       .cleanup = probe_cleanup,
                                       .rates = observer_rates,
                                       .supported_processing_modes = rates_modes,
                                       .num_supported_modes = 1,
                                       .rate_transfers = observer_transfers,
                                       .num_rate_transfers = 1,
                                       .rate_observers = observer_observers,
                                       .num_rate_observers = 1};

static struct Module context_saver = {.name = "context_saver",
                                      .init = probe_init,
                                      .cleanup = probe_cleanup,
                                      .process = saver_process,
                                      .supported_processing_modes = full_halo_modes,
                                      .num_supported_modes = 1};
static struct Module retire_probe = {.name = "retire_probe",
                                     .init = probe_init,
                                     .cleanup = probe_cleanup,
                                     .process = retire_process,
                                     .supported_processing_modes = full_halo_modes,
                                     .num_supported_modes = 1};
static struct Module fof_control = {.name = "fof_control",
                                    .init = probe_init,
                                    .cleanup = probe_cleanup,
                                    .process = fof_idle_process,
                                    .supported_processing_modes = by_galaxy_modes,
                                    .num_supported_modes = 1};

/** @brief Register the generated modules and every probe, once per process */
static void ensure_probes_registered(void) {
  static bool registered = false;
  ensure_modules_registered();
  if (!registered) {
    struct Module *probes[] = {&count_probe,          &central_reader,         &nan_probe,
                               &emit_probe,           &create_probe,           &wrong_kind_probe,
                               &master_probe,         &slaved_probe,           &cycle_probe,
                               &context_saver,        &retire_probe,           &fof_control,
                               &nan_central_probe,    &observer_probe,         &impure_probe,
                               &scratch_writer_probe, &fail_on_satellite_probe};
    for (size_t i = 0; i < sizeof(probes) / sizeof(probes[0]); i++) {
      module_registry_add(probes[i]);
    }
    registered = true;
  }
}

/* ==========================================================================
 * Configuration, workspace and context
 * ========================================================================== */

/** @brief An empty configuration with the default rate settings and one substep */
static void reset_rate_config(void) {
  reset_config();
  MimicConfig.SubSteps = 1;
  MimicConfig.RateIntegration = (struct MimicRateIntegration){
      .scheme = RATE_INTEGRATION_DEFAULT_SCHEME,
      .relative_tolerance = RATE_INTEGRATION_DEFAULT_RELATIVE_TOLERANCE,
      .absolute_tolerance = RATE_INTEGRATION_DEFAULT_ABSOLUTE_TOLERANCE,
      .max_steps_per_segment = RATE_INTEGRATION_DEFAULT_MAX_STEPS_PER_SEGMENT,
      .max_rejections_per_step = RATE_INTEGRATION_DEFAULT_MAX_REJECTIONS_PER_STEP,
      .check_purity = RATE_INTEGRATION_DEFAULT_CHECK_PURITY};
}

/** @brief Append one model parameter to the configuration */
static void add_param(const char *name, const char *value) {
  const int i = MimicConfig.NumModelParams++;
  snprintf(MimicConfig.ModelParams[i].param_name, MAX_STRING_LEN, "%s", name);
  snprintf(MimicConfig.ModelParams[i].value, MAX_STRING_LEN, "%s", value);
}

static struct FoFWorkspace workspace;
static struct GalaxyPool *pool = NULL;
static struct ModuleContext context;

/**
 * @brief   Build a FoF workspace: row 0 the Type 0 central, then the given satellites
 *
 * Row i has UniqueGalaxyID ROW_ID_BASE + i, HaloNr 10 + i, SnapNum 4 and
 * dT 0.5 + 0.25 i, a galaxy with every property at its default, and
 * RateFixtureCold = cold[i]. Every row's CentralHalo is 0, so row 0 may host
 * created records. The context is snapshot 5 at time 1, one substep.
 */
static void build_workspace(const int *types, const double *cold, int n) {
  memset(&workspace, 0, sizeof(workspace));
  pool = galaxy_pool_create(64);
  workspace.halos = mymalloc_cat((size_t)n * sizeof(struct Halo), MEM_HALOS);
  memset(workspace.halos, 0, (size_t)n * sizeof(struct Halo));
  workspace.count = n;
  workspace.capacity = n;
  workspace.pool = pool;
  workspace.identity = (struct RecordIdentitySpace){
      .unit = 0, .rows_per_unit = 1000, .fits = true, .units = 1, .driver = "vertical"};
  workspace.base_count = n;
  for (int i = 0; i < n; i++) {
    struct Halo *h = &workspace.halos[i];
    h->Type = types[i];
    h->HaloNr = 10 + i;
    h->UniqueGalaxyID = ROW_ID_BASE + i;
    h->CentralHalo = 0;
    h->SnapNum = 4;
    h->dT = 0.5 + 0.25 * i;
    h->Mvir = 10.0 + i;
    h->galaxy = galaxy_pool_alloc(pool);
    init_galaxy_defaults(h->galaxy);
    h->galaxy->RateFixtureCold = cold[i];
  }

  memset(&context, 0, sizeof(context));
  context.snapshot_number = 5;
  context.time = 1.0;
  context.num_substeps = 1;
  context.central_index = 0;
  context.central_galaxy = &workspace.halos[0];
  context.params = &MimicConfig;
}

static void free_workspace(void) {
  fof_workspace_destroy(&workspace);
  if (pool != NULL) {
    galaxy_pool_destroy(pool);
    pool = NULL;
  }
}

/** @brief Run the configured pipeline once over the workspace */
static void run_pipeline(void) { execute_module_pipeline(&context, &workspace); }

/** @brief Release the module system, the workspace and the creation scratch */
static void release_case(void) {
  module_system_cleanup();
  module_release_record_creation_scratch();
  free_workspace();
}

/* ==========================================================================
 * Startup rules
 * ========================================================================== */

/** One refused configuration and the two fragments its diagnostic must carry */
struct RuleCase {
  const char *name;
  void (*configure)(void);
  const char *needle_a;
  const char *needle_b;
};

static void config_mixed(void) {
  test_phase_add(RATE_PHASE, "count_probe", PROCESSING_MODE_RATES);
  test_phase_add(RATE_PHASE, "fof_control", PROCESSING_MODE_BY_GALAXY);
}
static void config_pre_timestep(void) {
  test_pre_timestep_add("count_probe", PROCESSING_MODE_RATES);
}
static void config_post_timestep(void) {
  MimicConfig.post_timestep = mymalloc_cat(sizeof(struct PhaseModuleConfig), MEM_UTILITY);
  MimicConfig.post_timestep[0].module_name = strdup("count_probe");
  MimicConfig.post_timestep[0].processing_mode = PROCESSING_MODE_RATES;
  MimicConfig.post_timestep[0].resolved = NULL;
  MimicConfig.num_post_timestep = 1;
}
static void config_post_snapshot(void) {
  test_post_snapshot_add("count_probe", PROCESSING_MODE_RATES);
}
static void config_second_phase(void) {
  test_phase_add(RATE_PHASE, "count_probe", PROCESSING_MODE_RATES);
  test_phase_add("more_rates", "nan_probe", PROCESSING_MODE_RATES);
}
static void config_listed_twice(void) {
  test_phase_add(RATE_PHASE, "count_probe", PROCESSING_MODE_RATES);
  test_phase_add(RATE_PHASE, "count_probe", PROCESSING_MODE_RATES);
}
static void config_master_missing(void) {
  test_phase_add(RATE_PHASE, "slaved_probe", PROCESSING_MODE_RATES);
}
static void config_cycle(void) { test_phase_add(RATE_PHASE, "cycle_probe", PROCESSING_MODE_RATES); }
static void config_wrong_kind(void) {
  test_phase_add(RATE_PHASE, "wrong_kind_probe", PROCESSING_MODE_RATES);
}
static void config_bad_tolerance(void) {
  test_phase_add(RATE_PHASE, "count_probe", PROCESSING_MODE_RATES);
  MimicConfig.RateIntegration.relative_tolerance = 0.0;
}
static void config_bad_purity(void) {
  test_phase_add(RATE_PHASE, "count_probe", PROCESSING_MODE_RATES);
  MimicConfig.RateIntegration.check_purity = 2;
}
static void config_unsupported(void) {
  test_phase_add(RATE_PHASE, "fof_control", PROCESSING_MODE_RATES);
}

static const struct RuleCase *current_rule = NULL;

/**
 * @brief   Child body: configure a valid FoF control phase and the case, then initialise
 *
 * Exits 1 only when module_system_init() refused the configuration before any
 * init() ran; any other outcome returns, so the child exits 0 and
 * expect_fatal() prints the captured diagnostics.
 */
static void rule_case_body(const char *unused) {
  (void)unused;
  ensure_probes_registered();
  reset_rate_config();
  init_calls = 0;
  /* A valid entry ahead of the case, so a premature init() would be visible. */
  test_phase_add("control_phase", "fof_control", PROCESSING_MODE_BY_GALAXY);
  current_rule->configure();

  if (module_system_init() != -1) {
    fprintf(stderr, "case accepted: %s\n", current_rule->name);
    return;
  }
  if (init_calls != 0) {
    fprintf(stderr, "case %s: a module init() ran before the refusal\n", current_rule->name);
    return;
  }
  module_system_cleanup();
  exit(1);
}

/**
 * @test    test_rate_phase_startup_rules
 * @brief   Every startup refusal of the rate phase fires before any init(), naming its subject
 */
int test_rate_phase_startup_rules(void) {
  static const struct RuleCase cases[] = {
      {"mixed modes", config_mixed, "Configuration error in phase '" RATE_PHASE "'",
       "mixes process_rates (module 'count_probe') with process_by_galaxy (module 'fof_control')"},
      {"pre_timestep", config_pre_timestep, "Configuration error in phase 'pre_timestep'",
       "Module 'count_probe' is configured with processing mode 'process_rates'"},
      {"post_timestep", config_post_timestep, "Configuration error in phase 'post_timestep'",
       "Module 'count_probe' is configured with processing mode 'process_rates'"},
      {"post_snapshot", config_post_snapshot, "Configuration error in phase 'post_snapshot'",
       "Module 'count_probe' is configured with processing mode 'process_rates'"},
      {"second rate phase", config_second_phase, "Configuration error in phase 'more_rates'",
       "A second rate phase: phase '" RATE_PHASE "' already runs process_rates"},
      {"listed twice", config_listed_twice, "Configuration error in phase '" RATE_PHASE "'",
       "Module 'count_probe' is listed more than once"},
      {"master not configured", config_master_missing,
       "Configuration error in phase '" RATE_PHASE "'",
       "Module 'slaved_probe' transfer 'enrich' is a slaved source of 'master_probe.form' (global "
       "index 150), whose module is not configured in this rate phase"},
      {"total cycle", config_cycle, "Configuration error in phase '" RATE_PHASE "'",
       "form a dependency cycle: 'rate_fixture_mass' -> 'rate_fixture_mass'"},
      {"wrong end kind", config_wrong_kind, "Configuration error in phase '" RATE_PHASE "'",
       "Module 'wrong_kind_probe' transfer 'leak' (internal) runs from 'RateFixtureHotTracer' (a "
       "tracer) to 'RateFixtureCold' (a reservoir)"},
      {"tolerance out of range", config_bad_tolerance,
       "Configuration error in phase '" RATE_PHASE "'", "tolerances (relative 0, absolute"},
      {"check_purity out of range", config_bad_purity,
       "Configuration error in phase '" RATE_PHASE "'",
       "modules.rate_integration.check_purity has value 2"},
      {"mode not supported", config_unsupported, "Configuration error in phase '" RATE_PHASE "'",
       "Module 'fof_control' does not support processing mode 'process_rates'"},
  };

  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    current_rule = &cases[i];
    const int ok =
        expect_fatal(cases[i].name, rule_case_body, cases[i].needle_a, cases[i].needle_b);
    if (ok != 1) {
      fprintf(stderr, "  rule case: %s\n", cases[i].name);
    }
    TEST_ASSERT_EQUAL(ok, 1, "the rate phase rule refuses before any init(), naming its subject");
  }
  return TEST_PASS;
}

/** @brief Child body: the master's module configured with its slaved source initialises */
static void master_present_body(const char *unused) {
  (void)unused;
  ensure_probes_registered();
  reset_rate_config();
  test_phase_add(RATE_PHASE, "slaved_probe", PROCESSING_MODE_RATES);
  test_phase_add(RATE_PHASE, "master_probe", PROCESSING_MODE_RATES);
  if (module_system_init() != 0) {
    exit(2);
  }
  module_system_cleanup();
}

/**
 * @test    test_slaved_source_accepted_with_its_master
 * @brief   The refused slaved source initialises once its master's module is in the phase
 */
int test_slaved_source_accepted_with_its_master(void) {
  TEST_ASSERT_EQUAL(expect_success(NULL, master_present_body), 1,
                    "a slaved source and its master's module in one rate phase initialise");
  return TEST_PASS;
}

/**
 * @test    test_total_cycle_finder
 * @brief   The cycle finder names a cycle in dependency order, and finds none in a DAG
 */
int test_total_cycle_finder(void) {
  /* ===== A three-total cycle: 0 -> 1 -> 2 -> 0, plus 3 -> 0 off the cycle ===== */
  int depends[16] = {0};
  int cycle[5] = {0};
  depends[0 * 4 + 1] = 1;
  depends[1 * 4 + 2] = 1;
  depends[2 * 4 + 0] = 1;
  depends[3 * 4 + 0] = 1;
  TEST_ASSERT_EQUAL(rate_assembly_find_total_cycle(4, depends, cycle), 4,
                    "the cycle's three totals, its first repeated");
  TEST_ASSERT(cycle[0] == 0 && cycle[1] == 1 && cycle[2] == 2 && cycle[3] == 0,
              "the cycle is named in dependency order");

  /* ===== Acyclic: 0 -> 1 -> 2, 0 -> 2 ===== */
  memset(depends, 0, sizeof(depends));
  depends[0 * 4 + 1] = 1;
  depends[1 * 4 + 2] = 1;
  depends[0 * 4 + 2] = 1;
  TEST_ASSERT_EQUAL(rate_assembly_find_total_cycle(4, depends, cycle), 0, "no cycle in a DAG");

  /* ===== A self-dependency ===== */
  memset(depends, 0, sizeof(depends));
  depends[3 * 4 + 3] = 1;
  TEST_ASSERT_EQUAL(rate_assembly_find_total_cycle(4, depends, cycle), 2,
                    "a total depending on itself is a cycle");
  TEST_ASSERT(cycle[0] == 3 && cycle[1] == 3, "the self-dependent total, repeated");
  return TEST_PASS;
}

/**
 * @test    test_defaults_match_integrator
 * @brief   The run-file defaults (constants.h) equal the integrator's own defaults
 */
int test_defaults_match_integrator(void) {
  struct RateIntegratorSettings settings;
  rate_integrator_default_settings(&settings);
  TEST_ASSERT_EQUAL((int)settings.scheme, RATE_INTEGRATION_DEFAULT_SCHEME, "default scheme");
  TEST_ASSERT(settings.relative_tolerance == RATE_INTEGRATION_DEFAULT_RELATIVE_TOLERANCE,
              "default relative tolerance");
  TEST_ASSERT(settings.absolute_tolerance == RATE_INTEGRATION_DEFAULT_ABSOLUTE_TOLERANCE,
              "default absolute tolerance");
  TEST_ASSERT(settings.max_steps_per_segment == RATE_INTEGRATION_DEFAULT_MAX_STEPS_PER_SEGMENT,
              "default step budget");
  TEST_ASSERT_EQUAL(settings.max_rejections_per_step,
                    RATE_INTEGRATION_DEFAULT_MAX_REJECTIONS_PER_STEP, "default rejection budget");
  TEST_ASSERT_EQUAL(RATE_INTEGRATION_DEFAULT_CHECK_PURITY, 0, "the purity probe is off by default");
  return TEST_PASS;
}

/* ==========================================================================
 * Domain and central view
 * ========================================================================== */

/**
 * @brief   Integrate a three-row group once and return row 1's end Cold
 *
 * @param   module  The rate module configured alone in the rate phase
 * @param   cold    Each row's start Cold (row 1 always 0.5)
 */
static double row_one_end_cold(const char *module, const double cold[3]) {
  static const int types[3] = {0, 1, 2};
  reset_rate_config();
  reset_probes();
  feed_rate = 0.3;
  test_phase_add(RATE_PHASE, module, PROCESSING_MODE_RATES);
  if (module_system_init() != 0) {
    return NAN;
  }
  build_workspace(types, cold, 3);
  workspace.halos[0].galaxy->RateFixtureHot = cold[0] * 3.0; /* other rows' other content */
  workspace.halos[2].galaxy->RateFixtureStars = cold[2] + 1.0;
  run_pipeline();
  const double end = workspace.halos[1].galaxy->RateFixtureCold;
  release_case();
  return end;
}

/**
 * @test    test_per_galaxy_domain_isolates_rows
 * @brief   With no central transfer or read, a row's end state ignores every other row
 *
 * The same row 1 is integrated in two groups whose other rows, the central
 * included, differ; its end state is bit-identical. With a module that reads
 * the central the group is one component and the two end states differ, which
 * shows the comparison would see a coupling.
 */
int test_per_galaxy_domain_isolates_rows(void) {
  const double group_a[3] = {1.0, 0.5, 2.0};
  const double group_b[3] = {7.0, 0.5, 0.01};

  const double a = row_one_end_cold("count_probe", group_a);
  const double b = row_one_end_cold("count_probe", group_b);
  TEST_ASSERT(isfinite(a) && a != 0.5, "row 1 was integrated");
  TEST_ASSERT(memcmp(&a, &b, sizeof(double)) == 0,
              "row 1's end state is bit-identical whatever the other rows hold");

  const double coupled_a = row_one_end_cold("central_reader", group_a);
  const double coupled_b = row_one_end_cold("central_reader", group_b);
  TEST_ASSERT(isfinite(coupled_a) && isfinite(coupled_b), "the coupled group integrates");
  TEST_ASSERT(coupled_a != coupled_b, "a central reader couples row 1 to the central");
  return TEST_PASS;
}

/** @brief Whether the recorded calls cycle through rows 0..n-1 in order, one group per evaluation
 */
static bool calls_interleave_rows(int module_calls_per_row, int n) {
  const int block = module_calls_per_row * n;
  if (num_calls == 0 || num_calls % block != 0) {
    return false;
  }
  for (int i = 0; i < num_calls; i++) {
    if (calls[i].row != (i % block) / module_calls_per_row) {
      return false;
    }
  }
  return true;
}

/**
 * @test    test_central_view
 * @brief   A reads_central law sees the central's scratch at the trial state; others see NULL
 *
 * central_reader (declares reads_central) and count_probe (does not) run
 * together on a three-row group. Every count_probe call gets a NULL central.
 * Every central_reader call gets the same scratch copy of the central, which
 * is the row the central's own laws receive (not the committed workspace
 * row), holding the trial Cold the central's own laws see in the same
 * evaluation, and that value moves during the segment. The calls cycle
 * through all rows at every evaluation: one component.
 */
int test_central_view(void) {
  static const int types[3] = {0, 1, 2};
  static const double cold[3] = {2.0, 0.5, 1.0};
  reset_rate_config();
  reset_probes();
  test_phase_add(RATE_PHASE, "count_probe", PROCESSING_MODE_RATES);
  test_phase_add(RATE_PHASE, "central_reader", PROCESSING_MODE_RATES);
  TEST_ASSERT_EQUAL(module_system_init(), 0, "the phase initialises");
  build_workspace(types, cold, 3);
  run_pipeline();

  /* Laws run per row by module name: central_reader, then count_probe. */
  TEST_ASSERT(calls_interleave_rows(2, 3), "every evaluation covers all three rows: one component");
  bool central_moved = false;
  bool all_ok = true;
  for (int i = 0; i + 5 < num_calls; i += 6) {
    const struct LawCall *central_own = &calls[i]; /* central_reader on row 0 */
    for (int k = 0; k < 6; k++) {
      const struct LawCall *c = &calls[i + k];
      if (k % 2 == 1) {
        all_ok &= (c->central == NULL); /* count_probe */
        continue;
      }
      all_ok &= (c->central == central_own->halo);
      all_ok &= (c->central_cold == central_own->cold);
    }
    central_moved |= (central_own->cold != cold[0]);
  }
  TEST_ASSERT(all_ok, "the declaring law sees the central's own scratch row at the same trial "
                      "state, and the other law sees NULL");
  TEST_ASSERT(calls[0].central != &workspace.halos[0], "the central view is not the workspace row");
  TEST_ASSERT(central_moved, "the central view holds trial states, not only the segment start");
  release_case();
  return TEST_PASS;
}

/**
 * @test    test_stripping_couples_the_group
 * @brief   With test_rate_stripping configured, the whole group is one component
 *
 * count_probe alone integrates each row on its own, so its calls run row by
 * row; with test_rate_stripping (to_record: central) beside it, every
 * evaluation covers all rows.
 */
int test_stripping_couples_the_group(void) {
  static const int types[3] = {0, 1, 2};
  static const double cold[3] = {2.0, 0.5, 1.0};

  reset_rate_config();
  reset_probes();
  test_phase_add(RATE_PHASE, "count_probe", PROCESSING_MODE_RATES);
  TEST_ASSERT_EQUAL(module_system_init(), 0, "count_probe alone initialises");
  build_workspace(types, cold, 3);
  run_pipeline();
  TEST_ASSERT(!calls_interleave_rows(1, 3), "alone, each row is its own component");
  release_case();

  reset_rate_config();
  reset_probes();
  add_param("TestRateStrippingRate", "2.0");
  add_param("TestRateStrippingEjectionRate", "0.5");
  test_phase_add(RATE_PHASE, "count_probe", PROCESSING_MODE_RATES);
  test_phase_add(RATE_PHASE, "test_rate_stripping", PROCESSING_MODE_RATES);
  TEST_ASSERT_EQUAL(module_system_init(), 0, "with test_rate_stripping the phase initialises");
  build_workspace(types, cold, 3);
  for (int i = 0; i < 3; i++) {
    workspace.halos[i].galaxy->RateFixtureHot = 1.0 + i;
  }
  run_pipeline();
  TEST_ASSERT(calls_interleave_rows(1, 3), "with test_rate_stripping the group is one component");
  TEST_ASSERT(workspace.halos[0].galaxy->RateFixtureHot > 1.0,
              "the central received the satellites' stripped Hot");
  release_case();
  return TEST_PASS;
}

/* ==========================================================================
 * The first-snapshot sentinel
 * ========================================================================== */

/**
 * @test    test_sentinel_group_is_unchanged
 * @brief   A group whose rows all carry the first-snapshot sentinel is unchanged and runs no law
 *
 * Both domains: count_probe alone (each row its own component) and
 * central_reader beside it (the group one component).
 */
int test_sentinel_group_is_unchanged(void) {
  static const int types[3] = {0, 1, 2};
  static const double cold[3] = {2.0, 0.5, 1.0};
  const char *const second[] = {NULL, "central_reader"};

  for (int variant = 0; variant < 2; variant++) {
    reset_rate_config();
    reset_probes();
    feed_rate = 1.0;
    test_phase_add(RATE_PHASE, "count_probe", PROCESSING_MODE_RATES);
    if (second[variant] != NULL) {
      test_phase_add(RATE_PHASE, second[variant], PROCESSING_MODE_RATES);
    }
    TEST_ASSERT_EQUAL(module_system_init(), 0, "the phase initialises");
    build_workspace(types, cold, 3);
    struct GalaxyData before[3];
    for (int i = 0; i < 3; i++) {
      workspace.halos[i].SnapNum = -1;
      workspace.halos[i].dT = (i == 1) ? 0.0 : -1.0;
      before[i] = *workspace.halos[i].galaxy;
    }
    run_pipeline();
    TEST_ASSERT_EQUAL(num_calls, 0, "no law was evaluated");
    for (int i = 0; i < 3; i++) {
      TEST_ASSERT(memcmp(&before[i], workspace.halos[i].galaxy, sizeof(struct GalaxyData)) == 0,
                  "every galaxy is bitwise unchanged");
    }
    release_case();
  }
  return TEST_PASS;
}

/** @brief Child body: a non-sentinel row 1 with dT = 0 reaches the rate phase */
static void zero_dt_body(const char *unused) {
  (void)unused;
  static const int types[3] = {0, 1, 2};
  static const double cold[3] = {2.0, 0.5, 1.0};
  ensure_probes_registered();
  reset_rate_config();
  test_phase_add(RATE_PHASE, "count_probe", PROCESSING_MODE_RATES);
  if (module_system_init() != 0) {
    return;
  }
  build_workspace(types, cold, 3);
  workspace.halos[1].dT = 0.0; /* SnapNum stays 4: not the sentinel */
  run_pipeline();
}

/**
 * @test    test_invalid_dt_is_fatal
 * @brief   A non-sentinel row with dT <= 0 is fatal, naming the row
 */
int test_invalid_dt_is_fatal(void) {
  TEST_ASSERT_EQUAL(expect_fatal(NULL, zero_dt_body,
                                 "workspace row 1 (UniqueGalaxyID 1001, Type 1, SnapNum 4)",
                                 "has dT = 0 at snapshot 5"),
                    1, "a non-sentinel row with dT = 0 is fatal, naming the row");
  return TEST_PASS;
}

/* ==========================================================================
 * Laws that misbehave
 * ========================================================================== */

/** @brief Child body: run pre_timestep context_saver, then the named misusing law */
static void misuse_body(const char *module) {
  static const int types[2] = {0, 1};
  static const double cold[2] = {1.0, 1.0};
  ensure_probes_registered();
  reset_rate_config();
  misuse_tried = false;
  emit_result = 0;
  create_result = 0;
  test_pre_timestep_add("context_saver", PROCESSING_MODE_FULL_HALO);
  test_phase_add(RATE_PHASE, module, PROCESSING_MODE_RATES);
  if (module_system_init() != 0) {
    return;
  }
  build_workspace(types, cold, 2);
  run_pipeline();
  const int result = strcmp(module, "emit_probe") == 0 ? emit_result : create_result;
  if (misuse_tried && saved_context != NULL && result == -1) {
    exit(1); /* refused; expect_fatal() checks the logged reason */
  }
  fprintf(stderr, "misuse by %s returned %d\n", module, result);
}

/**
 * @test    test_rate_law_cannot_emit_or_create
 * @brief   A rate law calling module_emit_event() or module_create_record() is refused
 *
 * Both use the context a full-halo callback saw earlier in the same FoF step.
 */
int test_rate_law_cannot_emit_or_create(void) {
  TEST_ASSERT_EQUAL(expect_fatal("emit_probe", misuse_body,
                                 "module_emit_event refused for module 'emit_probe'",
                                 "called from a process_rates callback"),
                    1, "emission from a rate law is refused, naming the module");
  TEST_ASSERT_EQUAL(expect_fatal("create_probe", misuse_body,
                                 "module_create_record refused for module 'create_probe'",
                                 "from a process_rates callback"),
                    1, "creation from a rate law is refused, naming the module");
  return TEST_PASS;
}

/** @brief Child body: a law returning NaN in a one-row group */
static void nan_body(const char *unused) {
  (void)unused;
  static const int types[1] = {0};
  static const double cold[1] = {1.0};
  ensure_probes_registered();
  reset_rate_config();
  test_phase_add(RATE_PHASE, "nan_probe", PROCESSING_MODE_RATES);
  if (module_system_init() != 0) {
    return;
  }
  build_workspace(types, cold, 1);
  run_pipeline();
}

/**
 * @test    test_nan_law_is_fatal
 * @brief   A law returning NaN fails the segment, naming the module, the transfer and the cause
 */
int test_nan_law_is_fatal(void) {
  char captured[CHILD_CAPTURE_BYTES];
  const int ok = expect_fatal_capture(NULL, nan_body, "module 'nan_probe', transfer 'poison'",
                                      "cause bad_coefficient", captured, sizeof(captured));
  TEST_ASSERT_EQUAL(ok, 1, "a NaN coefficient is fatal, naming module, transfer and cause");
  TEST_ASSERT(strstr(captured, "workspace row 0 (UniqueGalaxyID 1000)") != NULL,
              "the message names the row");
  TEST_ASSERT(strstr(captured, "tau 0") != NULL, "the message names tau");
  return TEST_PASS;
}

/* ==========================================================================
 * The failure file and the purity probe
 * ========================================================================== */

/** The failure-file test's output directory, made before the child runs */
static char failure_dir[MAX_STRING_LEN];

/** @brief Child body: nan_body with the output directory set, so the failure file is written */
static void failure_file_body(const char *unused) {
  (void)unused;
  static const int types[1] = {0};
  static const double cold[1] = {1.0};
  ensure_probes_registered();
  reset_rate_config();
  snprintf(MimicConfig.OutputDir, sizeof(MimicConfig.OutputDir), "%s", failure_dir);
  test_phase_add(RATE_PHASE, "nan_probe", PROCESSING_MODE_RATES);
  if (module_system_init() != 0) {
    return;
  }
  build_workspace(types, cold, 1);
  run_pipeline();
}

/** @brief The whole text of the file at @p path, or NULL; the caller frees it */
static char *read_text_file(const char *path) {
  FILE *file = fopen(path, "r");
  if (file == NULL) {
    return NULL;
  }
  char *text = calloc(1, 65536);
  const size_t got = text != NULL ? fread(text, 1, 65535, file) : 0;
  (void)got;
  fclose(file);
  return text;
}

/**
 * @test    test_failure_file_holds_the_segment
 * @brief   A failed segment writes rate_failure_task0.txt before the run ends
 *
 * A one-row component with Cold = 1 and dT = 0.5 whose law returns NaN: the
 * file names the cause, module and transfer, and holds the row's dT and Delta,
 * the segment-start state and the failing evaluation's coefficient as
 * hexadecimal floats (1 is 0x1p+0, 0.5 is 0x1p-1; a NaN carries its bits).
 */
int test_failure_file_holds_the_segment(void) {
  const char *tmp = getenv("TMPDIR");
  snprintf(failure_dir, sizeof(failure_dir), "%s/mimic_rate_failure_XXXXXX",
           tmp != NULL && tmp[0] != '\0' ? tmp : "/tmp");
  TEST_ASSERT(mkdtemp(failure_dir) != NULL, "a temporary output directory is made");

  const int ok = expect_fatal(NULL, failure_file_body, "module 'nan_probe', transfer 'poison'",
                              "failure file: ");
  TEST_ASSERT_EQUAL(ok, 1, "the failing run ends fatally and names its failure file");

  char path[MAX_STRING_LEN + 64];
  snprintf(path, sizeof(path), "%s/rate_failure_task0.txt", failure_dir);
  char *text = read_text_file(path);
  TEST_ASSERT(text != NULL, "rate_failure_task0.txt exists in the output directory");
  static const char *const wanted[] = {
      "cause: bad_coefficient\n",
      "module: nan_probe\n",
      "transfer: poison\n",
      "task: 0\n",
      "check_purity: false\n",
      "rows: 1\n",
      "row 0 workspace_row 0 unique_galaxy_id 1000 type 0 dT 0x1p-1 delta 0x1p-1\n",
      "state 0 1 RateFixtureCold 0x1p+0\n",
      "state 0 0 RateFixtureHot 0x0p+0\n",
      "coefficient 0 nan_probe.poison nan bits=0x7ff8000000000000\n"};
  int missing = 0;
  for (size_t i = 0; text != NULL && i < sizeof(wanted) / sizeof(wanted[0]); i++) {
    if (strstr(text, wanted[i]) == NULL) {
      fprintf(stderr, "  failure file lacks: %s", wanted[i]);
      missing++;
    }
  }
  TEST_ASSERT_EQUAL(missing, 0, "the failure file holds the cause, row clock, state and law");
  free(text);
  unlink(path);
  rmdir(failure_dir);
  return TEST_PASS;
}

/** @brief Make the temporary output directory the failure-file cases write into */
static int make_failure_dir(void) {
  const char *tmp = getenv("TMPDIR");
  snprintf(failure_dir, sizeof(failure_dir), "%s/mimic_rate_failure_XXXXXX",
           tmp != NULL && tmp[0] != '\0' ? tmp : "/tmp");
  return mkdtemp(failure_dir) != NULL;
}

/**
 * @brief   Child body: a failing component that follows or sits beside others
 *
 * @param   arg  "zero_delta": row 0 integrates normally and leaves coefficients in the scratch,
 *               then row 1, a first-snapshot sentinel row (Delta 0, no law runs) holding a
 *               tracer above its parent, fails the tracer bound before any law ran.
 *               "mid_row": a three-row group (one component) whose second module's law returns
 *               non-zero on row 1 after the rest of row 1's laws and every law of row 0 ran.
 */
static void failure_case_body(const char *arg) {
  static const int types[3] = {0, 1, 2};
  static const double cold_sentinel[3] = {0.0, 0.5, 0.0};
  static const double cold_empty[3] = {0.0, 0.0, 0.0};
  const bool zero_delta = strcmp(arg, "zero_delta") == 0;
  ensure_probes_registered();
  reset_rate_config();
  reset_probes();
  feed_rate = 0.5;
  snprintf(MimicConfig.OutputDir, sizeof(MimicConfig.OutputDir), "%s", failure_dir);
  test_phase_add(RATE_PHASE, "count_probe", PROCESSING_MODE_RATES);
  if (!zero_delta) {
    test_phase_add(RATE_PHASE, "fail_on_satellite_probe", PROCESSING_MODE_RATES);
  }
  if (module_system_init() != 0) {
    return;
  }
  if (zero_delta) {
    build_workspace(types, cold_sentinel, 2);
    workspace.halos[1].SnapNum = -1;
    workspace.halos[1].dT = 0.0;
    workspace.halos[1].galaxy->RateFixtureColdTracer = 1.0;
  } else {
    build_workspace(types, cold_empty, 3);
  }
  run_pipeline();
}

/** @brief Whether @p text has a line starting with @p prefix */
static bool has_line_starting(const char *text, const char *prefix) {
  const size_t length = strlen(prefix);
  for (const char *line = text; line != NULL && *line != '\0';) {
    if (strncmp(line, prefix, length) == 0) {
      return true;
    }
    line = strchr(line, '\n');
    line = line != NULL ? line + 1 : NULL;
  }
  return false;
}

/** @brief Read the failure file the case wrote, then remove it and the directory */
static char *take_failure_file(void) {
  char path[MAX_STRING_LEN + 64];
  snprintf(path, sizeof(path), "%s/rate_failure_task0.txt", failure_dir);
  char *text = read_text_file(path);
  unlink(path);
  rmdir(failure_dir);
  return text;
}

/**
 * @test    test_failure_file_never_invents_coefficients
 * @brief   The file lists only coefficients a law evaluation produced
 *
 * Zero-Delta case: component 1 fails its tracer bound before any law ran, after
 * component 0 filled the scratch with coefficients; the file says no law was
 * evaluated and lists no coefficient line. Mid-group case: the second module's
 * law returns non-zero on row 1, so row 0's and row 1's first-module
 * coefficients are exact hex floats and everything after the failing call is
 * marked not_evaluated.
 */
int test_failure_file_never_invents_coefficients(void) {
  TEST_ASSERT(make_failure_dir(), "a temporary output directory is made");
  int ok = expect_fatal("zero_delta", failure_case_body, "cause tracer_bound", "failure file: ");
  TEST_ASSERT_EQUAL(ok, 1, "the zero-Delta tracer-bound failure ends the run");
  char *text = take_failure_file();
  TEST_ASSERT(text != NULL, "the zero-Delta failure wrote its file");
  TEST_ASSERT(strstr(text, "cause: tracer_bound\n") != NULL, "the file names the cause");
  TEST_ASSERT(strstr(text, "law_evaluations_begun: 0\n") != NULL, "no evaluation had begun");
  TEST_ASSERT(strstr(text, "failing_evaluation_tau: none\n") != NULL, "no evaluation tau");
  TEST_ASSERT(strstr(text, "coefficients: no law was evaluated\n") != NULL,
              "the file says no law was evaluated");
  TEST_ASSERT(!has_line_starting(text, "coefficient "), "no coefficient line is listed");
  TEST_ASSERT(strstr(text, "state 0 1 RateFixtureCold 0x1p-1\n") != NULL,
              "the segment-start state is still listed");
  free(text);

  TEST_ASSERT(make_failure_dir(), "a second temporary output directory is made");
  ok = expect_fatal("mid_row", failure_case_body, "module 'fail_on_satellite_probe'",
                    "failure file: ");
  TEST_ASSERT_EQUAL(ok, 1, "the mid-group law failure ends the run");
  text = take_failure_file();
  TEST_ASSERT(text != NULL, "the mid-group failure wrote its file");
  static const char *const wanted[] = {
      "law_evaluations_begun: 1\n",
      "coefficient 0 count_probe.feed 0x1p-1\n",
      "coefficient 0 count_probe.drain 0x1p+0\n",
      "coefficient 0 fail_on_satellite_probe.boom 0x1.999999999999ap-4\n",
      "coefficient 1 count_probe.feed 0x1p-1\n",
      "coefficient 1 count_probe.drain 0x1p+0\n",
      "coefficient 1 fail_on_satellite_probe.boom not_evaluated\n",
      "coefficient 2 count_probe.feed not_evaluated\n",
      "coefficient 2 count_probe.drain not_evaluated\n",
      "coefficient 2 fail_on_satellite_probe.boom not_evaluated\n"};
  int missing = 0;
  for (size_t i = 0; i < sizeof(wanted) / sizeof(wanted[0]); i++) {
    if (strstr(text, wanted[i]) == NULL) {
      fprintf(stderr, "  failure file lacks: %s", wanted[i]);
      missing++;
    }
  }
  TEST_ASSERT_EQUAL(missing, 0, "evaluated coefficients are exact and the rest not_evaluated");
  free(text);
  return TEST_PASS;
}

/**
 * @brief   Run the whole-group probe case and return the number of law calls
 *
 * Three rows, every reservoir empty, central_reader and count_probe in the
 * phase (the central read makes the group one component, so one probe covers
 * the three rows). An empty component with no source takes one four-stage step.
 * The call order of the first two evaluations is copied to @p rows and
 * @p from_reader, 12 entries each: the row and whether the module is
 * central_reader.
 */
static int whole_group_calls(int check_purity, int rows[12], int from_reader[12]) {
  static const int types[3] = {0, 1, 2};
  static const double cold[3] = {0.0, 0.0, 0.0};
  reset_rate_config();
  reset_probes();
  MimicConfig.RateIntegration.check_purity = check_purity;
  test_phase_add(RATE_PHASE, "count_probe", PROCESSING_MODE_RATES);
  test_phase_add(RATE_PHASE, "central_reader", PROCESSING_MODE_RATES);
  if (module_system_init() != 0) {
    return -1;
  }
  build_workspace(types, cold, 3);
  run_pipeline();
  const int total = num_calls;
  for (int i = 0; i < 12 && i < total; i++) {
    rows[i] = calls[i].row;
    from_reader[i] = calls[i].central != NULL;
  }
  release_case();
  return total;
}

/**
 * @test    test_purity_probe_cost_and_order
 * @brief   Off, no law is evaluated beyond the integrator's; on, one reversed pass at the first
 *
 * The same one-step group runs with check_purity false and true. The calls of
 * the false run are the integrator's own (four stages of three rows and two
 * modules); the true run makes exactly one more pass of six calls, and the
 * first evaluation runs forward (rows 0 to 2, central_reader before
 * count_probe) then in reversed canonical order.
 */
int test_purity_probe_cost_and_order(void) {
  int off_rows[12] = {0};
  int off_reader[12] = {0};
  int on_rows[12] = {0};
  int on_reader[12] = {0};
  const int off = whole_group_calls(0, off_rows, off_reader);
  const int on = whole_group_calls(1, on_rows, on_reader);

  TEST_ASSERT_EQUAL(off, 4 * 3 * 2, "with the probe off, the four stages of one step and no more");
  TEST_ASSERT_EQUAL(on - off, 3 * 2, "with the probe on, one more pass of every row and module");

  static const int forward_rows[6] = {0, 0, 1, 1, 2, 2};
  static const int forward_reader[6] = {1, 0, 1, 0, 1, 0};
  static const int reversed_rows[6] = {2, 2, 1, 1, 0, 0};
  static const int reversed_reader[6] = {0, 1, 0, 1, 0, 1};
  int off_forward = 0;
  int on_forward = 0;
  int on_reversed = 0;
  for (int i = 0; i < 6; i++) {
    off_forward += off_rows[i] == forward_rows[i] && off_reader[i] == forward_reader[i];
    on_forward += on_rows[i] == forward_rows[i] && on_reader[i] == forward_reader[i];
    on_reversed += on_rows[6 + i] == reversed_rows[i] && on_reader[6 + i] == reversed_reader[i];
  }
  TEST_ASSERT_EQUAL(on_forward, 6, "the first evaluation runs rows and modules in order");
  TEST_ASSERT_EQUAL(on_reversed, 6, "the repeat runs rows and modules in reversed order");

  int off_second_forward = 0;
  for (int i = 0; i < 6; i++) {
    off_second_forward +=
        off_rows[6 + i] == forward_rows[i] && off_reader[6 + i] == forward_reader[i];
  }
  TEST_ASSERT_EQUAL(off_forward + off_second_forward, 12,
                    "with the probe off, the second evaluation is the next forward pass");
  return TEST_PASS;
}

/**
 * @brief   Child body: one row under the impure or the scratch-writing law
 *
 * @param   arg  "counter" or "scratch", then ":on" or ":off" for check_purity
 */
static void purity_body(const char *arg) {
  static const int types[1] = {0};
  static const double cold[1] = {1.0};
  ensure_probes_registered();
  reset_rate_config();
  MimicConfig.RateIntegration.check_purity = strstr(arg, ":on") != NULL;
  test_phase_add(RATE_PHASE,
                 strncmp(arg, "counter", 7) == 0 ? "impure_probe" : "scratch_writer_probe",
                 PROCESSING_MODE_RATES);
  if (module_system_init() != 0) {
    exit(2);
  }
  build_workspace(types, cold, 1);
  run_pipeline();
}

/**
 * @test    test_purity_probe_catches_impure_laws
 * @brief   An impure law or a scratch write is fatal with the probe on, and runs with it off
 */
int test_purity_probe_catches_impure_laws(void) {
  char captured[CHILD_CAPTURE_BYTES];
  int ok = expect_fatal_capture("counter:on", purity_body, "module 'impure_probe', transfer 'seep'",
                                "Rate law is impure", captured, sizeof(captured));
  TEST_ASSERT_EQUAL(ok, 1, "a call-counting law is fatal under the probe, naming module, transfer");
  TEST_ASSERT(strstr(captured, "on the first evaluation and") != NULL,
              "the message says the coefficient changed between the two evaluations");

  ok = expect_fatal_capture("scratch:on", purity_body,
                            "module 'scratch_writer_probe', transfer 'scribble'",
                            "changed the scratch row", captured, sizeof(captured));
  TEST_ASSERT_EQUAL(ok, 1, "a law writing its scratch row is fatal under the probe");

  TEST_ASSERT_EQUAL(expect_success("counter:off", purity_body), 1,
                    "the call-counting law runs to completion with the probe off");
  TEST_ASSERT_EQUAL(expect_success("scratch:off", purity_body), 1,
                    "the scratch-writing law runs to completion with the probe off");
  return TEST_PASS;
}

/* ==========================================================================
 * Rows that change between segments
 * ========================================================================== */

/**
 * @test    test_retired_row_leaves_the_next_segment
 * @brief   A row a later discrete phase retires is not in the next substep's segment
 *
 * Two substeps of [rates (count_probe), retire (retire_probe)]: the probe
 * retires row 2 in substep 0, after its first segment. Substep 1's segment
 * evaluates rows 0 and 1 only and leaves row 2's galaxy as it was retired.
 */
int test_retired_row_leaves_the_next_segment(void) {
  static const int types[3] = {0, 1, 2};
  static const double cold[3] = {2.0, 0.5, 1.0};
  reset_rate_config();
  reset_probes();
  feed_rate = 0.4;
  MimicConfig.SubSteps = 2;
  test_phase_add(RATE_PHASE, "count_probe", PROCESSING_MODE_RATES);
  test_phase_add("retire", "retire_probe", PROCESSING_MODE_FULL_HALO);
  TEST_ASSERT_EQUAL(module_system_init(), 0, "the phases initialise");
  build_workspace(types, cold, 3);
  context.num_substeps = 2;
  run_pipeline();

  int before[3] = {0, 0, 0};
  int after[3] = {0, 0, 0};
  for (int i = 0; i < num_calls; i++) {
    if (calls[i].row >= 0 && calls[i].row < 3) {
      (calls[i].phase_mark == 0 ? before : after)[calls[i].row]++;
    }
  }
  TEST_ASSERT_EQUAL(discrete_phase_calls, 2, "the discrete phase ran once per substep");
  TEST_ASSERT(before[0] > 0 && before[1] > 0 && before[2] > 0, "substep 0 evaluates every row");
  TEST_ASSERT(after[0] > 0 && after[1] > 0, "substep 1 evaluates the remaining rows");
  TEST_ASSERT_EQUAL(after[2], 0, "substep 1 evaluates no law for the retired row");
  TEST_ASSERT(memcmp(&retired_cold, &workspace.halos[2].galaxy->RateFixtureCold, sizeof(double)) ==
                  0,
              "the retired row keeps the content it was retired with");
  release_case();
  return TEST_PASS;
}

/**
 * @test    test_created_record_joins_at_zero
 * @brief   A record test_fixture creates in pre_timestep joins the rate component at zero content
 *
 * test_fixture (process_full_halo in pre_timestep, TestFixtureCreateRecords 1)
 * creates one record on the Type 0 row; count_probe feeds every row. The
 * created row is evaluated, its first evaluation sees zero Cold, and it ends
 * with the content the feed brought it.
 */
int test_created_record_joins_at_zero(void) {
  static const int types[2] = {0, 1};
  static const double cold[2] = {2.0, 0.5};
  reset_rate_config();
  reset_probes();
  feed_rate = 0.4;
  add_param("TestFixtureDummyParameter", "0.25");
  add_param("TestFixtureEnableLogging", "0");
  add_param("TestFixtureCreateRecords", "1");
  test_pre_timestep_add("test_fixture", PROCESSING_MODE_FULL_HALO);
  test_phase_add(RATE_PHASE, "count_probe", PROCESSING_MODE_RATES);
  TEST_ASSERT_EQUAL(module_system_init(), 0, "the phases initialise");
  build_workspace(types, cold, 2);
  run_pipeline();

  TEST_ASSERT_EQUAL((int)workspace.count, 3, "test_fixture created one record");
  const struct Halo *created = &workspace.halos[2];
  TEST_ASSERT(created->UniqueGalaxyID < 0, "the new row is a created record");
  int created_calls = 0;
  double first_cold = -1.0;
  for (int i = 0; i < num_calls; i++) {
    if (calls[i].id == created->UniqueGalaxyID) {
      if (created_calls++ == 0) {
        first_cold = calls[i].cold;
      }
    }
  }
  TEST_ASSERT(created_calls > 0, "the created row is evaluated in the segment");
  TEST_ASSERT(first_cold == 0.0, "it joins with zero content");
  TEST_ASSERT(created->galaxy->RateFixtureCold > 0.0, "and ends with what the feed brought it");
  release_case();
  return TEST_PASS;
}

/** @brief Child body: a source into the central whose law returns NaN on satellite rows */
static void nan_central_body(const char *unused) {
  (void)unused;
  static const int types[3] = {0, 1, 2};
  static const double cold[3] = {1.0, 1.0, 1.0};
  ensure_probes_registered();
  reset_rate_config();
  test_phase_add(RATE_PHASE, "nan_central_probe", PROCESSING_MODE_RATES);
  if (module_system_init() != 0) {
    return;
  }
  build_workspace(types, cold, 3);
  run_pipeline();
}

/**
 * @test    test_failure_names_the_evaluating_row
 * @brief   A bad coefficient of a source into the central names the row whose law returned it
 *
 * The integrator names the source's recipient entry, the central's Cold, so
 * the message names that entry's row as well as the evaluating satellite row.
 */
int test_failure_names_the_evaluating_row(void) {
  char captured[CHILD_CAPTURE_BYTES];
  const int ok = expect_fatal_capture(
      NULL, nan_central_body, "module 'nan_central_probe', transfer 'gift'",
      "workspace row 1 (UniqueGalaxyID 1001), tau 0", captured, sizeof(captured));
  TEST_ASSERT_EQUAL(ok, 1, "the fatal line names satellite row 1, whose law returned NaN");
  TEST_ASSERT(strstr(captured, "its flux names entry 'RateFixtureCold' on row 0 (workspace row 0, "
                               "UniqueGalaxyID 1000)") != NULL,
              "the message also names the recipient entry on the central's row");
  return TEST_PASS;
}

/**
 * @test    test_law_sees_observer_at_segment_start
 * @brief   A law reading an observer sees its segment-start value; write-back adds the amounts
 *
 * Stars and the observer RateFixtureFormed start at the same value and receive
 * the same committed amounts, so they end bit-identical; every evaluation of
 * the law sees the observer at its start value.
 */
int test_law_sees_observer_at_segment_start(void) {
  static const int types[1] = {0};
  static const double cold[1] = {2.0};
  reset_rate_config();
  num_observed = 0;
  test_phase_add(RATE_PHASE, "observer_probe", PROCESSING_MODE_RATES);
  TEST_ASSERT_EQUAL(module_system_init(), 0, "the phase initialises");
  build_workspace(types, cold, 1);
  struct GalaxyData *g = workspace.halos[0].galaxy;
  g->RateFixtureStars = 0.25;
  g->RateFixtureFormed = 0.25;
  run_pipeline();

  TEST_ASSERT(num_observed > 4, "the segment took several evaluations");
  bool constant = true;
  for (int i = 0; i < num_observed; i++) {
    constant &= (observed_formed[i] == 0.25);
  }
  TEST_ASSERT(constant, "every evaluation saw the observer at its segment-start value");
  TEST_ASSERT(g->RateFixtureFormed > 0.25, "the observer accumulated the formed amount");
  TEST_ASSERT(memcmp(&g->RateFixtureFormed, &g->RateFixtureStars, sizeof(double)) == 0,
              "the written-back observer is its start plus the committed amounts");
  release_case();
  return TEST_PASS;
}

/** Rows of the per-galaxy scaling case */
#define LARGE_ROWS 3000

/** @brief Row i of the scaling case: its Type, start Cold and dT */
static void large_row(int i, int *type, double *cold, double *dt) {
  *type = (i == 0) ? 0 : 2;
  *cold = 0.05 + 0.01 * (double)(i % 97);
  *dt = 0.2 + 0.003 * (double)(i % 53);
}

/**
 * @test    test_per_galaxy_rows_match_rows_alone
 * @brief   A large per-galaxy workspace gives each row the end state it gets integrated alone
 *
 * Each row of a LARGE_ROWS workspace is its own component; the gather takes
 * the row directly (no scan of the workspace per row). Every row's end Cold is
 * bit-identical to the same row integrated in a one-row workspace.
 */
int test_per_galaxy_rows_match_rows_alone(void) {
  static int types[LARGE_ROWS];
  static double cold[LARGE_ROWS];
  static double dts[LARGE_ROWS];
  static double together[LARGE_ROWS];
  for (int i = 0; i < LARGE_ROWS; i++) {
    large_row(i, &types[i], &cold[i], &dts[i]);
  }

  reset_rate_config();
  reset_probes();
  feed_rate = 0.3;
  test_phase_add(RATE_PHASE, "count_probe", PROCESSING_MODE_RATES);
  TEST_ASSERT_EQUAL(module_system_init(), 0, "the phase initialises");
  build_workspace(types, cold, LARGE_ROWS);
  for (int i = 0; i < LARGE_ROWS; i++) {
    workspace.halos[i].dT = dts[i];
  }
  run_pipeline();
  for (int i = 0; i < LARGE_ROWS; i++) {
    together[i] = workspace.halos[i].galaxy->RateFixtureCold;
  }
  free_workspace();

  int mismatches = 0;
  for (int i = 0; i < LARGE_ROWS; i++) {
    num_calls = 0;
    build_workspace(&types[i], &cold[i], 1);
    workspace.halos[0].dT = dts[i];
    run_pipeline();
    const double alone = workspace.halos[0].galaxy->RateFixtureCold;
    mismatches += memcmp(&alone, &together[i], sizeof(double)) != 0;
    free_workspace();
  }
  TEST_ASSERT_EQUAL(mismatches, 0, "every row ends bit-identical to the row integrated alone");
  TEST_ASSERT(together[1] != cold[1], "the rows were integrated");
  module_system_cleanup();
  return TEST_PASS;
}

/* ==========================================================================
 * Memory
 * ========================================================================== */

/** @brief Run one segment of central_reader over a group of @p n rows (one component) */
static void run_group(int n) {
  static const int types[MAX_ROWS] = {0, 1, 2, 2, 2, 2, 2, 2};
  static const double cold[MAX_ROWS] = {2.0, 0.5, 1.0, 0.25, 3.0, 0.75, 1.5, 0.1};
  build_workspace(types, cold, n);
  run_pipeline();
  free_workspace();
}

/**
 * @test    test_scratch_is_one_high_water_allocation
 * @brief   The rate scratch is one tracked allocation, grown to its high water, freed at cleanup
 */
int test_scratch_is_one_high_water_allocation(void) {
  reset_rate_config();
  reset_probes();
  const size_t utility_start = memory_category_bytes(MEM_UTILITY);
  test_phase_add(RATE_PHASE, "central_reader", PROCESSING_MODE_RATES);
  TEST_ASSERT_EQUAL(module_system_init(), 0, "the phase initialises");
  const size_t utility_init = memory_category_bytes(MEM_UTILITY);
  TEST_ASSERT(module_rate_scratch_bytes() == 0, "no scratch before the first segment");

  run_group(2);
  const size_t small = module_rate_scratch_bytes();
  TEST_ASSERT(small > 0, "the first segment allocates the scratch");
  TEST_ASSERT(memory_category_bytes(MEM_UTILITY) - utility_init == small,
              "the scratch is the one MEM_UTILITY allocation the segment added");

  run_group(MAX_ROWS);
  const size_t large = module_rate_scratch_bytes();
  TEST_ASSERT(large > small, "a larger component grows the scratch");
  TEST_ASSERT(memory_category_bytes(MEM_UTILITY) - utility_init == large,
              "growing kept it one allocation");

  run_group(2);
  TEST_ASSERT(module_rate_scratch_bytes() == large, "a smaller component keeps the high water");

  module_system_cleanup();
  TEST_ASSERT(module_rate_scratch_bytes() == 0, "cleanup releases the scratch");
  TEST_ASSERT(memory_category_bytes(MEM_UTILITY) == utility_start,
              "cleanup releases the plan, the scratch and the phase configuration");
  return TEST_PASS;
}

int main(void) {
  printf("%s", BLUE);
  printf("============================================================\n");
  printf("Test Suite: rate phase dispatch\n");
  printf("============================================================\n");
  printf("%s", NC);

  init_memory_system(0);
  initialize_error_handling(LOG_LEVEL_WARNING, NULL);
  ensure_probes_registered();

  TEST_RUN(test_rate_phase_startup_rules);
  TEST_RUN(test_slaved_source_accepted_with_its_master);
  TEST_RUN(test_total_cycle_finder);
  TEST_RUN(test_defaults_match_integrator);
  TEST_RUN(test_per_galaxy_domain_isolates_rows);
  TEST_RUN(test_central_view);
  TEST_RUN(test_stripping_couples_the_group);
  TEST_RUN(test_sentinel_group_is_unchanged);
  TEST_RUN(test_invalid_dt_is_fatal);
  TEST_RUN(test_rate_law_cannot_emit_or_create);
  TEST_RUN(test_nan_law_is_fatal);
  TEST_RUN(test_failure_file_holds_the_segment);
  TEST_RUN(test_failure_file_never_invents_coefficients);
  TEST_RUN(test_purity_probe_cost_and_order);
  TEST_RUN(test_purity_probe_catches_impure_laws);
  TEST_RUN(test_retired_row_leaves_the_next_segment);
  TEST_RUN(test_created_record_joins_at_zero);
  TEST_RUN(test_failure_names_the_evaluating_row);
  TEST_RUN(test_law_sees_observer_at_segment_start);
  TEST_RUN(test_per_galaxy_rows_match_rows_alone);
  TEST_RUN(test_scratch_is_one_high_water_allocation);

  TEST_SUMMARY();
  return TEST_RESULT();
}
