/**
 * @file    test_unit_sage_resolve_mergers.c
 * @brief   End-state tests of sage_resolve_mergers_and_disruption's merger and disruption pass
 *
 * One scenario builder places a FoF central (row 0, Type 0) and one Type 1 satellite (row 1)
 * from named reservoir fixtures, runs process() for one substep and checks the end state exactly.
 * The substep is dT 0.5 in one substep from Age[current] 13.75, so every clock drops by 0.5 and
 * every merger time is 13.75 + 0.5 - 0.5 * 0.5 = 14.0, all exact in float. The module's action
 * hook records what the pass did to the satellite and the merger's mass ratio. Expected values
 * are hand-computed literals.
 *
 * Test cases:
 *   - test_pass_end_state: the eligibility gate (failing, at the threshold, zero baryons), the
 *     MergTime >= 999 abort, a NaN clock left in place, disruption for a positive clock and merger
 *     for one reaching 0, the ratio of 1 for two empty galaxies, the strict 0.1 floor of the
 *     minor-merger time, and the major/minor split just below, at and just above
 *     ThresholdMajorMerger (equality is minor)
 */

#include "../../../../tests/framework/test_framework.h"
#include "../../../../tests/framework/test_phase_config.h"
#include "core/module_interface.h"
#include "include/globals.h"
#include "include/types.h"
#include "util/error.h"
#include "util/memory.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* Shared SAGE16 test fixture boilerplate (test counters) */
#include "modules/_tests/sage_test_fixtures.h"

extern int sage_resolve_mergers_and_disruption_init(void);
extern int sage_resolve_mergers_and_disruption_process(struct ModuleContext *ctx,
                                                       struct Halo *halos, int ngal);
extern int sage_resolve_mergers_and_disruption_cleanup(void);
extern void sage_resolve_mergers_and_disruption_set_action_hook(
    void (*hook)(const char *action, int source_index, int target_index, double mass_ratio));

#define CLOCK "sage_initialise_merger_clock"

#define SUBSTEP_DT 0.5
#define AGE_CURRENT 13.75
#define EVENT_TIME 14.0f
#define NEVER -1.0f

/** One galaxy's reservoirs; a field a fixture does not name is zero */
struct Body {
  float stellar, hot, bulge, ics;
};

/* A central of m stars, b of them in its bulge */
#define CENTRAL(m, b) {.stellar = (m), .hot = 1.0f, .bulge = (b), .ics = 0.5f}
/* A central with no stars or cold gas */
#define EMPTY_CENTRAL {.hot = 1.0f, .ics = 0.5f}
/* A satellite of m stars */
#define SAT(m) {.stellar = (m), .hot = 0.5f, .ics = 0.25f}
/* A satellite with no stars or cold gas */
#define EMPTY_SAT {.hot = 0.5f, .ics = 0.25f}

/* What the pass did to the satellite: 'm' merged, 'd' disrupted, 0 nothing; the merger's ratio */
static struct {
  char action;
  double ratio;
} recorded;

static void record_action(const char *action, int source_index, int target_index,
                          double mass_ratio) {
  (void)source_index;
  (void)target_index;
  recorded.action = action[0];
  recorded.ratio = mass_ratio;
}

static void add_parameter(const char *name, const char *value) {
  const int i = MimicConfig.NumModelParams++;
  snprintf(MimicConfig.ModelParams[i].param_name, MAX_STRING_LEN, "%s", name);
  snprintf(MimicConfig.ModelParams[i].value, MAX_STRING_LEN, "%s", value);
}

/** Reset the configuration to the resolver's parameters (ThresholdSatDisruption 1) */
static void set_parameters(double threshold_major) {
  char value[32];
  memset(&MimicConfig, 0, sizeof(MimicConfig));
  snprintf(value, sizeof(value), "%.17g", threshold_major);
  add_parameter("ThresholdMajorMerger", value);
  add_parameter("ThresholdSatDisruption", "1.0");
}

static void place(struct Halo *halo, struct GalaxyData *gal, int type, double mvir, float merg_time,
                  const struct Body *body) {
  memset(halo, 0, sizeof(*halo));
  memset(gal, 0, sizeof(*gal));
  halo->Type = type;
  halo->CentralHalo = 0;
  halo->Mvir = mvir;
  halo->SnapNum = 63;
  halo->dT = SUBSTEP_DT;
  halo->galaxy = gal;
  gal->MergTime = merg_time;
  gal->StellarMass = body->stellar;
  gal->HotGas = body->hot;
  gal->BulgeMass = body->bulge;
  gal->ICS = body->ics;
  gal->TimeOfLastMajorMerger = NEVER;
  gal->TimeOfLastMinorMerger = NEVER;
}

/** Equal floats, or both NaN */
static int same(float actual, float expected) {
  return (isnan(actual) && isnan(expected)) || actual == expected;
}

/**
 * @test    test_pass_end_state
 * @brief   What the pass leaves on the satellite and its target
 *
 * The gate passes zero baryons, or Mvir / (StellarMass + ColdGas) at most ThresholdSatDisruption
 * (1 here); the clock is decremented before it. A clock still positive disrupts the satellite
 * (the target's hot gas takes its cold and hot gas, 1 + 0.5; its ICS the satellite's ICS and
 * stars, 0.5 + 0.25 + 1); one at or below 0 merges it (stars and bulge + 1, hot gas 1.5, ICS
 * 0.75). TimeOfLastMinorMerger is set for a ratio strictly above 0.1, and a ratio strictly above
 * ThresholdMajorMerger also makes the whole stellar mass a bulge and sets TimeOfLastMajorMerger.
 * The ratio rows sit at one float either side of 1 / 4, where the float sums round to the same
 * target state. Two empty galaxies merge with ratio 1. A clock not below 999 aborts the pass.
 */
int test_pass_end_state(void) {
  const struct {
    const char *name;
    double threshold_major, sat_mvir;
    float merg_time;
    struct Body central, satellite;
    int rc, sat_type;
    float merg_time_after;
    char action;
    double ratio;
    float stellar, bulge, hot, ics, major, minor;
  } rows[] = {
      {"halo above ThresholdSatDisruption times its baryons: stays", 0.3, 1.5, 0.25f,
       CENTRAL(4.0f, 1.0f), SAT(1.0f), 0, 1, -0.25f, 0, NAN, 4.0f, 1.0f, 1.0f, 0.5f, NEVER, NEVER},
      {"halo at the threshold, clock expired: merges", 0.3, 1.0, 0.25f, CENTRAL(4.0f, 1.0f),
       SAT(1.0f), 0, 3, -0.25f, 'm', 0.25, 5.0f, 2.0f, 1.5f, 0.75f, NEVER, EVENT_TIME},
      {"clock reaching exactly 0: merges", 0.3, 0.0, 0.5f, CENTRAL(4.0f, 1.0f), SAT(1.0f), 0, 3,
       0.0f, 'm', 0.25, 5.0f, 2.0f, 1.5f, 0.75f, NEVER, EVENT_TIME},
      {"clock still positive: disrupted", 0.3, 0.0, 1.0f, CENTRAL(4.0f, 1.0f), SAT(1.0f), 0, 3,
       0.5f, 'd', 0.0, 4.0f, 1.0f, 1.5f, 1.75f, NEVER, NEVER},
      {"MergTime 999: the pass aborts", 0.3, 0.0, 999.0f, CENTRAL(4.0f, 1.0f), SAT(1.0f), -1, 1,
       999.0f, 0, NAN, 4.0f, 1.0f, 1.0f, 0.5f, NEVER, NEVER},
      {"NaN MergTime: left in place, no abort", 0.3, 0.0, NAN, CENTRAL(4.0f, 1.0f), SAT(1.0f), 0, 1,
       NAN, 0, NAN, 4.0f, 1.0f, 1.0f, 0.5f, NEVER, NEVER},
      {"no baryons: merges at ratio 0 with no merger time", 0.3, 50.0, 0.25f, CENTRAL(4.0f, 1.0f),
       EMPTY_SAT, 0, 3, -0.25f, 'm', 0.0, 4.0f, 1.0f, 1.5f, 0.75f, NEVER, NEVER},
      {"both baryon sums zero: ratio 1, a major merger", 0.3, 0.0, 0.25f, EMPTY_CENTRAL, EMPTY_SAT,
       0, 3, -0.25f, 'm', 1.0, 0.0f, 0.0f, 1.5f, 0.75f, EVENT_TIME, EVENT_TIME},
      {"ratio exactly 0.1: no minor-merger time", 0.3, 0.0, 0.25f, CENTRAL(5.0f, 1.0f), SAT(0.5f),
       0, 3, -0.25f, 'm', 0.1, 5.5f, 1.5f, 1.5f, 0.75f, NEVER, NEVER},
      {"ratio just below the threshold: minor", 0.25, 0.0, 0.25f, CENTRAL(4.0f, 1.0f),
       SAT(0x1.fffffep-1f), 0, 3, -0.25f, 'm', 0x1.fffffep-3, 5.0f, 2.0f, 1.5f, 0.75f, NEVER,
       EVENT_TIME},
      {"ratio equal to the threshold: minor", 0.25, 0.0, 0.25f, CENTRAL(4.0f, 1.0f), SAT(1.0f), 0,
       3, -0.25f, 'm', 0.25, 5.0f, 2.0f, 1.5f, 0.75f, NEVER, EVENT_TIME},
      {"ratio just above the threshold: major", 0.25, 0.0, 0.25f, CENTRAL(4.0f, 1.0f),
       SAT(0x1.000002p+0f), 0, 3, -0.25f, 'm', 0x1.000002p-2, 5.0f, 5.0f, 1.5f, 0.75f, EVENT_TIME,
       EVENT_TIME},
  };
  int ok = 1;
  init_memory_system(0);
  for (size_t r = 0; r < sizeof(rows) / sizeof(rows[0]); r++) {
    set_parameters(rows[r].threshold_major);
    test_pre_timestep_add(CLOCK, PROCESSING_MODE_FULL_HALO);
    TEST_ASSERT(sage_resolve_mergers_and_disruption_init() == 0, "init must accept the fixture");
    sage_resolve_mergers_and_disruption_set_action_hook(record_action);

    struct ModuleContext ctx;
    struct Halo halos[2];
    struct GalaxyData gal[2];
    memset(&ctx, 0, sizeof(ctx));
    ctx.time = AGE_CURRENT;
    ctx.num_substeps = 1;
    ctx.params = &MimicConfig;
    ctx.central_galaxy = &halos[0];
    place(&halos[0], &gal[0], 0, 100.0, 0.0f, &rows[r].central);
    place(&halos[1], &gal[1], 1, rows[r].sat_mvir, rows[r].merg_time, &rows[r].satellite);
    recorded.action = 0;
    recorded.ratio = NAN;
    const int rc = sage_resolve_mergers_and_disruption_process(&ctx, halos, 2);
    sage_resolve_mergers_and_disruption_cleanup();
    test_free_pre_timestep();

    const struct GalaxyData *t = &gal[0];
    const int row_ok =
        rc == rows[r].rc && halos[1].Type == rows[r].sat_type &&
        same(gal[1].MergTime, rows[r].merg_time_after) && recorded.action == rows[r].action &&
        (rows[r].action == 0 || recorded.ratio == rows[r].ratio) &&
        t->StellarMass == rows[r].stellar && t->BulgeMass == rows[r].bulge &&
        t->HotGas == rows[r].hot && t->ICS == rows[r].ics &&
        t->TimeOfLastMajorMerger == rows[r].major && t->TimeOfLastMinorMerger == rows[r].minor;
    if (!row_ok) {
      fprintf(
          stderr,
          "  row '%s': rc %d, satellite Type %d, MergTime %.9g, action '%c' ratio %a; "
          "target StellarMass %.9g BulgeMass %.9g HotGas %.9g ICS %.9g, major %.9g minor %.9g\n",
          rows[r].name, rc, halos[1].Type, (double)gal[1].MergTime,
          recorded.action ? recorded.action : '-', recorded.ratio, (double)t->StellarMass,
          (double)t->BulgeMass, (double)t->HotGas, (double)t->ICS, (double)t->TimeOfLastMajorMerger,
          (double)t->TimeOfLastMinorMerger);
      ok = 0;
    }
  }
  check_memory_leaks();
  TEST_ASSERT(ok, "each satellite and target must end in the tabulated state");
  return TEST_PASS;
}

int main(void) {
  initialize_error_handling(LOG_LEVEL_WARNING, NULL);

  printf("%s", BLUE);
  printf("============================================================\n");
  printf("Unit Test Suite: SAGE Merger and Disruption Resolver End State\n");
  printf("============================================================\n");
  printf("%s", NC);

  TEST_RUN(test_pass_end_state);

  TEST_SUMMARY();
  return TEST_RESULT();
}
