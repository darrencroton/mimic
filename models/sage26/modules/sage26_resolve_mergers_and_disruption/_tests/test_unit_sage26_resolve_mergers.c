/**
 * @file    test_unit_sage26_resolve_mergers.c
 * @brief   End-state tests of sage26_resolve_mergers_and_disruption's pass and merger transaction
 *
 * One scenario builder places a FoF central (row 0, Type 0) and one Type 1 satellite (row 1)
 * from named reservoir fixtures, runs the pass for one substep and checks the end state exactly.
 * The substep is dT 0.5 in one substep from Age[current] 13.75, so every clock drops by 0.5 and
 * every event time is 13.75 + 0.5 - 0.5 * 0.5 = 14.0, all exact in float. A sink stands in for
 * the phase's merger-event consumers and records each event, so the transaction's own writes are
 * the only ones. Expected values are hand-computed literals.
 *
 * Test cases:
 *   - test_pass_selection: the eligibility gate (failing, at the threshold, zero baryons), the
 *     MergTime >= 999 and NaN aborts, the non-finite-clock skip, disruption for a positive clock
 *     and merger for one reaching 0, and an emitted event for a mass ratio of 0
 *   - test_merger_split_and_transfer: the BulgeSizeOn 0 transaction: the major/minor split
 *     just below, at and just above ThresholdMajorMerger (equality is minor), equal masses at
 *     threshold 1, the satellite disk stars' channel by the target's post-addition disk
 *     fraction, the carried morphology flag, the ICS booking, the bulge-history transfer and
 *     the zeroed bulge radii
 *   - test_mode3_bulge_radii: BulgeSizeOn 3: the remnant radius kept by a major merger and by a
 *     minor merger onto a spheroid-dominated target, discarded onto a disk-dominated one
 *
 * The orders the resolver requires of the clock and of its event consumers are refused, with
 * diagnostics, by test_unit_sage_dependency_contracts.c.
 */

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

/* Shared sage26 test fixture boilerplate (counters, config reset, parameter helper) */
#include "modules/_tests/sage_test_fixtures.h"

extern int sage26_resolve_mergers_and_disruption_init(void);
extern int sage26_resolve_mergers_and_disruption_cleanup(void);

#define RESOLVER "sage26_resolve_mergers_and_disruption"
#define CLOCK "sage26_initialise_merger_clock"

#define SUBSTEP_DT 0.5
#define AGE_CURRENT 13.75
#define EVENT_TIME 14.0f
#define NEVER -1.0f
#define HUBBLE_H 0.73

/** One galaxy's reservoirs and bulge size; a field a fixture does not name is zero */
struct Body {
  float stellar, hot, bulge, merger_bulge, ics, ics_sum_mt, bh;
  float disk_radius, bulge_radius;
};

/* A central of 4 stars, b of them in its merger bulge (disk-dominated for b < 2) */
#define CENTRAL(b)                                                                                 \
  {.stellar = 4.0f,                                                                                \
   .hot = 1.0f,                                                                                    \
   .bulge = (b),                                                                                   \
   .merger_bulge = (b),                                                                            \
   .ics = 0.5f,                                                                                    \
   .ics_sum_mt = 2.0f,                                                                             \
   .bh = 0.125f,                                                                                   \
   .disk_radius = 0.01f,                                                                           \
   .bulge_radius = 0.004f}
/* A central with no stars or cold gas */
#define EMPTY_CENTRAL {.hot = 1.0f, .ics = 0.5f, .ics_sum_mt = 2.0f}
/* A pure-disk satellite of m stars holding ics_mass of ICS */
#define DISK_SAT(m, ics_mass)                                                                      \
  {.stellar = (m),                                                                                 \
   .hot = 0.5f,                                                                                    \
   .ics = (ics_mass),                                                                              \
   .ics_sum_mt = 1.0f,                                                                             \
   .bh = 0.0625f,                                                                                  \
   .disk_radius = 0.005f}
/* A satellite of m stars, all in its merger bulge */
#define BULGE_SAT(m)                                                                               \
  {.stellar = (m),                                                                                 \
   .hot = 0.5f,                                                                                    \
   .bulge = (m),                                                                                   \
   .merger_bulge = (m),                                                                            \
   .ics = 0.25f,                                                                                   \
   .ics_sum_mt = 1.0f,                                                                             \
   .bh = 0.0625f,                                                                                  \
   .bulge_radius = 0.001f}
/* A satellite with no stars or cold gas */
#define EMPTY_SAT {.hot = 0.5f, .ics = 0.25f, .ics_sum_mt = 1.0f}

/** One run of the pass: the module's parameters, the satellite's halo and clock, both bodies */
struct Scenario {
  int bulge_size_on;
  double threshold_major;
  double sat_mvir;
  float merg_time;
  struct Body central, satellite;
};

/* The merger events the sink received, the last one's values */
static struct {
  int count;
  double ratio, dt;
  int disk_dominated;
} events;

static int record_event(struct ModuleContext *ctx, struct Halo *halos, int source_idx,
                        int target_idx, double mass_ratio, double source_dt) {
  (void)ctx;
  (void)source_idx;
  events.count++;
  events.ratio = mass_ratio;
  events.dt = source_dt;
  events.disk_dominated = halos[target_idx].galaxy->MergerDiskDominated;
  return 0;
}

/** Set only the resolver's parameters: ThresholdSatDisruption 1, CGMrecipeOn 0 */
static void set_parameters(double threshold_major, int bulge_size_on) {
  char value[32];
  reset_config();
  MimicConfig.Hubble_h = HUBBLE_H;
  snprintf(value, sizeof(value), "%.17g", threshold_major);
  sage26_test_append_model_parameter("ThresholdMajorMerger", value);
  sage26_test_append_model_parameter("ThresholdSatDisruption", "1.0");
  snprintf(value, sizeof(value), "%d", bulge_size_on);
  sage26_test_append_model_parameter("BulgeSizeOn", value);
  sage26_test_append_model_parameter("CGMrecipeOn", "0");
}

static void cleanup_resolver(void) {
  sage26_resolve_mergers_and_disruption_cleanup();
  test_free_pre_timestep();
  test_free_substep_phases();
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
  gal->MergerBulgeMass = body->merger_bulge;
  gal->ICS = body->ics;
  gal->ICS_sum_mt = body->ics_sum_mt;
  gal->BlackHoleMass = body->bh;
  gal->DiskScaleRadius = body->disk_radius;
  gal->BulgeRadius = body->bulge_radius;
  gal->MergerBulgeRadius = body->bulge_radius;
  gal->TimeOfLastMajorMerger = NEVER;
  gal->TimeOfLastMinorMerger = NEVER;
}

/**
 * @brief Initialise the resolver for the scenario, place it and run the pass once
 *
 * The satellite also holds an ICS history (ICS_disrupt, ICS_accrete 0.125) and history bin 3
 * (disk 0.25, bulge 0.5), which a merger moves or clears. Returns init()'s status; the pass's
 * return is in *rc.
 */
static int run_scenario(const struct Scenario *s, struct Halo halos[2],
                        struct GalaxyData galaxies[2], int *rc) {
  set_parameters(s->threshold_major, s->bulge_size_on);
  test_pre_timestep_add(CLOCK, PROCESSING_MODE_FULL_HALO);
  const int status = sage26_resolve_mergers_and_disruption_init();
  if (status == 0) {
    struct ModuleContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.time = AGE_CURRENT;
    ctx.num_substeps = 1;
    ctx.substep_number = 0;
    ctx.params = &MimicConfig;
    ctx.central_galaxy = &halos[0];
    place(&halos[0], &galaxies[0], 0, 100.0, 0.0f, &s->central);
    place(&halos[1], &galaxies[1], 1, s->sat_mvir, s->merg_time, &s->satellite);
    galaxies[1].ICS_disrupt = 0.125f;
    galaxies[1].ICS_accrete = 0.125f;
    galaxies[1].SfrDiskBin3 = 0.25f;
    galaxies[1].SfrBulgeBin3 = 0.5f;
    memset(&events, 0, sizeof(events));
    events.ratio = NAN;
    *rc = sage26_resolve_mergers_and_disruption_pass(&ctx, halos, 2, record_event, NULL);
  }
  cleanup_resolver();
  return status;
}

/** Equal floats, or both NaN */
static int same(float actual, float expected) {
  return (isnan(actual) && isnan(expected)) || actual == expected;
}

/**
 * @test    test_pass_selection
 * @brief   Which satellites the pass merges, disrupts, leaves or aborts on
 *
 * The gate passes zero baryons, or Mvir / (StellarMass + ColdGas) at most ThresholdSatDisruption
 * (1 here); the clock is decremented before it. A clock still positive disrupts the satellite to
 * the target's ICS (stars and ICS 0.5 + 0.25 + 1 = 1.75; ICS_sum_mt 2 + 1 * 14 + 1 = 17); one
 * at or below 0 merges it (StellarMass 4 + 1 = 5, ICS 0.75, ICS_sum_mt 2 + 1 = 3). A clock not
 * below 999, or NaN, aborts the pass before any change.
 */
int test_pass_selection(void) {
  const struct {
    const char *name;
    double sat_mvir;
    float merg_time;
    struct Body central, satellite;
    int rc, sat_type, events;
    double ratio;
    float merg_time_after, stellar, ics, ics_sum_mt;
  } rows[] = {
      {"halo above ThresholdSatDisruption times its baryons: stays", 1.5, 0.25f, CENTRAL(1.0f),
       DISK_SAT(1.0f, 0.25f), 0, 1, 0, NAN, -0.25f, 4.0f, 0.5f, 2.0f},
      {"halo at the threshold, clock expired: merges", 1.0, 0.25f, CENTRAL(1.0f),
       DISK_SAT(1.0f, 0.25f), 0, 3, 1, 0.25, -0.25f, 5.0f, 0.75f, 3.0f},
      {"clock reaching exactly 0: merges", 0.0, 0.5f, CENTRAL(1.0f), DISK_SAT(1.0f, 0.25f), 0, 3, 1,
       0.25, 0.0f, 5.0f, 0.75f, 3.0f},
      {"clock still positive: disrupted to the ICS", 0.0, 1.0f, CENTRAL(1.0f),
       DISK_SAT(1.0f, 0.25f), 0, 3, 0, NAN, 0.5f, 4.0f, 1.75f, 17.0f},
      {"no baryons: passes the gate whatever its halo, ratio 0", 50.0, 0.25f, CENTRAL(1.0f),
       EMPTY_SAT, 0, 3, 1, 0.0, -0.25f, 4.0f, 0.75f, 3.0f},
      {"both baryon sums zero: ratio 0, the event still emitted", 0.0, 0.25f, EMPTY_CENTRAL,
       EMPTY_SAT, 0, 3, 1, 0.0, -0.25f, 0.0f, 0.75f, 3.0f},
      {"MergTime 999: the pass aborts", 0.0, 999.0f, CENTRAL(1.0f), DISK_SAT(1.0f, 0.25f), -1, 1, 0,
       NAN, 999.0f, 4.0f, 0.5f, 2.0f},
      {"NaN MergTime: the pass aborts", 0.0, NAN, CENTRAL(1.0f), DISK_SAT(1.0f, 0.25f), -1, 1, 0,
       NAN, NAN, 4.0f, 0.5f, 2.0f},
      {"clock -inf: neither merged nor disrupted", 0.0, -INFINITY, CENTRAL(1.0f),
       DISK_SAT(1.0f, 0.25f), 0, 1, 0, NAN, -INFINITY, 4.0f, 0.5f, 2.0f},
  };
  int ok = 1;
  init_memory_system(0);
  for (size_t r = 0; r < sizeof(rows) / sizeof(rows[0]); r++) {
    const struct Scenario s = {.threshold_major = 0.3,
                               .sat_mvir = rows[r].sat_mvir,
                               .merg_time = rows[r].merg_time,
                               .central = rows[r].central,
                               .satellite = rows[r].satellite};
    struct Halo halos[2];
    struct GalaxyData gal[2];
    int rc = 0;
    TEST_ASSERT(run_scenario(&s, halos, gal, &rc) == 0, "init must accept the fixture");
    const int row_ok = rc == rows[r].rc && halos[1].Type == rows[r].sat_type &&
                       same(gal[1].MergTime, rows[r].merg_time_after) &&
                       events.count == rows[r].events &&
                       (rows[r].events == 0 || events.ratio == rows[r].ratio) &&
                       gal[0].StellarMass == rows[r].stellar && gal[0].ICS == rows[r].ics &&
                       gal[0].ICS_sum_mt == rows[r].ics_sum_mt;
    if (!row_ok) {
      fprintf(stderr,
              "  row '%s': rc %d, satellite Type %d, MergTime %.9g, events %d (ratio %.9g); "
              "target StellarMass %.9g, ICS %.9g, ICS_sum_mt %.9g\n",
              rows[r].name, rc, halos[1].Type, (double)gal[1].MergTime, events.count, events.ratio,
              (double)gal[0].StellarMass, (double)gal[0].ICS, (double)gal[0].ICS_sum_mt);
      ok = 0;
    }
  }
  check_memory_leaks();
  TEST_ASSERT(ok, "each satellite must be merged, disrupted, left or aborted on as tabulated");
  return TEST_PASS;
}

/**
 * @test    test_merger_split_and_transfer
 * @brief   The BulgeSizeOn 0 merger transaction's end state on the target
 *
 * A ratio strictly above ThresholdMajorMerger makes the whole stellar mass a merger bulge and
 * stamps TimeOfLastMajorMerger; anything else stamps TimeOfLastMinorMerger. The ratio rows sit at
 * one float either side of 1 / 4 (the satellite's 1 - 2^-24 or 1 + 2^-23 against 4), where the
 * float sums round to the same target state. The satellite's disk stars join the instability
 * bulge when the target's post-addition disk fraction exceeds one half, else the merger bulge,
 * whatever the carried pre-addition flag (MergerDiskDominated, disk mass above half the stars). A
 * positive satellite ICS is booked as accreted (0.25) with its accumulator (ICS_sum_mt 2 + 1).
 * Every merger moves the satellite's history bin 3 (0.25 + 0.5) into the target's bulge history,
 * clears the satellite's ICS history and zeroes the target's three bulge radii.
 */
int test_merger_split_and_transfer(void) {
  const struct {
    const char *name;
    double threshold_major;
    struct Body central, satellite;
    double ratio;
    float stellar, bulge, merger_bulge, inst_bulge, ics_accrete, ics_sum_mt, major, minor;
    int disk_dominated;
  } rows[] = {
      {"ratio just below the threshold: minor", 0.25, CENTRAL(1.0f), BULGE_SAT(0x1.fffffep-1f),
       0x1.fffffep-3, 5.0f, 2.0f, 2.0f, 0.0f, 0.25f, 3.0f, NEVER, EVENT_TIME, 1},
      {"ratio equal to the threshold: minor", 0.25, CENTRAL(1.0f), BULGE_SAT(1.0f), 0.25, 5.0f,
       2.0f, 2.0f, 0.0f, 0.25f, 3.0f, NEVER, EVENT_TIME, 1},
      {"ratio just above the threshold: major", 0.25, CENTRAL(1.0f), BULGE_SAT(0x1.000002p+0f),
       0x1.000002p-2, 5.0f, 5.0f, 5.0f, 0.0f, 0.25f, 3.0f, EVENT_TIME, NEVER, 1},
      {"equal masses at threshold 1: minor", 1.0, CENTRAL(1.0f), BULGE_SAT(4.0f), 1.0, 8.0f, 5.0f,
       5.0f, 0.0f, 0.25f, 3.0f, NEVER, EVENT_TIME, 1},
      {"post-addition disk fraction 0.6: satellite disk to the instability bulge", 0.3,
       CENTRAL(1.0f), DISK_SAT(1.0f, 0.25f), 0.25, 5.0f, 2.0f, 1.0f, 1.0f, 0.25f, 3.0f, NEVER,
       EVENT_TIME, 1},
      {"disk-dominated target, post-addition fraction exactly 0.5: merger bulge", 0.3,
       CENTRAL(1.5f), DISK_SAT(1.0f, 0.25f), 0.25, 5.0f, 2.5f, 2.5f, 0.0f, 0.25f, 3.0f, NEVER,
       EVENT_TIME, 1},
      {"spheroid-dominated target: satellite disk to the merger bulge", 0.3, CENTRAL(3.0f),
       DISK_SAT(1.0f, 0.25f), 0.25, 5.0f, 4.0f, 4.0f, 0.0f, 0.25f, 3.0f, NEVER, EVENT_TIME, 0},
      {"satellite without ICS: no accretion booked", 0.3, CENTRAL(1.0f), DISK_SAT(1.0f, 0.0f), 0.25,
       5.0f, 2.0f, 1.0f, 1.0f, 0.0f, 2.0f, NEVER, EVENT_TIME, 1},
  };
  int ok = 1;
  init_memory_system(0);
  for (size_t r = 0; r < sizeof(rows) / sizeof(rows[0]); r++) {
    const struct Scenario s = {.threshold_major = rows[r].threshold_major,
                               .merg_time = 0.25f,
                               .central = rows[r].central,
                               .satellite = rows[r].satellite};
    struct Halo halos[2];
    struct GalaxyData gal[2];
    int rc = -1;
    TEST_ASSERT(run_scenario(&s, halos, gal, &rc) == 0, "init must accept the fixture");
    const struct GalaxyData *t = &gal[0];
    const int split_ok = t->StellarMass == rows[r].stellar && t->BulgeMass == rows[r].bulge &&
                         t->MergerBulgeMass == rows[r].merger_bulge &&
                         t->InstabilityBulgeMass == rows[r].inst_bulge &&
                         t->TimeOfLastMajorMerger == rows[r].major &&
                         t->TimeOfLastMinorMerger == rows[r].minor;
    const int event_ok = events.count == 1 && events.ratio == rows[r].ratio &&
                         events.dt == SUBSTEP_DT && events.disk_dominated == rows[r].disk_dominated;
    const int transfer_ok = t->ICS_accrete == rows[r].ics_accrete &&
                            t->ICS_sum_mt == rows[r].ics_sum_mt && t->BlackHoleMass == 0.1875f &&
                            t->SfrBulgeBin3 == 0.75f && t->BulgeRadius == 0.0f &&
                            t->MergerBulgeRadius == 0.0f && t->InstabilityBulgeRadius == 0.0f;
    const int satellite_ok =
        rc == 0 && halos[1].Type == 3 && gal[1].StellarMass == rows[r].satellite.stellar &&
        gal[1].ICS_disrupt == 0.0f && gal[1].ICS_accrete == 0.0f && gal[1].ICS_sum_mt == 0.0f;
    if (!(split_ok && event_ok && transfer_ok && satellite_ok)) {
      fprintf(stderr,
              "  row '%s': rc %d; target StellarMass %.9g BulgeMass %.9g merger %.9g instability "
              "%.9g, major %.9g minor %.9g; event count %d ratio %a flag %d; ICS_accrete %.9g "
              "ICS_sum_mt %.9g; satellite Type %d\n",
              rows[r].name, rc, (double)t->StellarMass, (double)t->BulgeMass,
              (double)t->MergerBulgeMass, (double)t->InstabilityBulgeMass,
              (double)t->TimeOfLastMajorMerger, (double)t->TimeOfLastMinorMerger, events.count,
              events.ratio, events.disk_dominated, (double)t->ICS_accrete, (double)t->ICS_sum_mt,
              halos[1].Type);
      ok = 0;
    }
  }
  check_memory_leaks();
  TEST_ASSERT(ok, "each merger must leave the tabulated target state");
  return TEST_PASS;
}

/**
 * @test    test_mode3_bulge_radii
 * @brief   BulgeSizeOn 3: where the remnant radius goes, by the major/minor split and morphology
 *
 * The remnant radius is M_tot^2 / (E_init + E_orb + E_rad) with E_init = M1^2 / R1 + M2^2 / R2,
 * E_orb = M1 M2 / (R1 + R2) and E_rad 0 here (no cold gas), each R the progenitor's mass-weighted
 * half-mass radius (1.68 DiskScaleRadius for the disk, BulgeRadius for the bulge). In the major
 * row the target after the addition holds 6 stars, 3 in the bulge at 0.004, so
 * R1 = (3 x 1.68 x 0.01 + 3 x 0.004) / 6 = 0.0104, and the satellite R2 = 0.001:
 * 64 / (36 / 0.0104 + 4 / 0.001 + 12 / 0.0114) = 0.00751688. In the spheroid row
 * R1 = (1 x 1.68 x 0.01 + 4 x 0.004) / 5 = 0.00656, R2 = 1.68 x 0.005 = 0.0084:
 * 36 / (25 / 0.00656 + 1 / 0.0084 + 5 / 0.01496) = 0.00844229. In the disk row the satellite's
 * disk star joins the instability bulge at 0.2 x 0.01 = 0.002 and the merger bulge keeps 0.004, so
 * BulgeRadius is (0.004 + 0.002) / 2. The radii are float stores of double arithmetic (with fma),
 * so they are compared to 1e-6 relative.
 */
int test_mode3_bulge_radii(void) {
  const struct {
    const char *name;
    struct Body central, satellite;
    float major, minor;
    double merger_radius, inst_radius, bulge_radius;
  } rows[] = {
      {"major merger: the remnant radius is the merger bulge's", CENTRAL(1.0f), BULGE_SAT(2.0f),
       EVENT_TIME, NEVER, 7.5168808e-3, 0.0, 7.5168808e-3},
      {"minor onto a disk-dominated target: the remnant radius is discarded", CENTRAL(1.0f),
       DISK_SAT(1.0f, 0.25f), NEVER, EVENT_TIME, 4.0e-3, 2.0e-3, 3.0e-3},
      {"minor onto a spheroid-dominated target: the remnant radius is kept", CENTRAL(3.0f),
       DISK_SAT(1.0f, 0.25f), NEVER, EVENT_TIME, 8.4422861e-3, 0.0, 8.4422861e-3},
  };
  int ok = 1;
  init_memory_system(0);
  for (size_t r = 0; r < sizeof(rows) / sizeof(rows[0]); r++) {
    const struct Scenario s = {.bulge_size_on = 3,
                               .threshold_major = 0.3,
                               .merg_time = 0.25f,
                               .central = rows[r].central,
                               .satellite = rows[r].satellite};
    struct Halo halos[2];
    struct GalaxyData gal[2];
    int rc = -1;
    TEST_ASSERT(run_scenario(&s, halos, gal, &rc) == 0, "init must accept BulgeSizeOn 3");
    const struct GalaxyData *t = &gal[0];
    const int row_ok =
        rc == 0 && events.count == 1 && t->TimeOfLastMajorMerger == rows[r].major &&
        t->TimeOfLastMinorMerger == rows[r].minor &&
        fabs(t->MergerBulgeRadius - rows[r].merger_radius) <= 1e-6 * rows[r].merger_radius &&
        fabs(t->InstabilityBulgeRadius - rows[r].inst_radius) <= 1e-6 * rows[r].inst_radius &&
        fabs(t->BulgeRadius - rows[r].bulge_radius) <= 1e-6 * rows[r].bulge_radius;
    if (!row_ok) {
      fprintf(stderr,
              "  row '%s': rc %d, major %.9g minor %.9g; radii merger %.9g instability %.9g "
              "bulge %.9g\n",
              rows[r].name, rc, (double)t->TimeOfLastMajorMerger, (double)t->TimeOfLastMinorMerger,
              (double)t->MergerBulgeRadius, (double)t->InstabilityBulgeRadius,
              (double)t->BulgeRadius);
      ok = 0;
    }
  }
  check_memory_leaks();
  TEST_ASSERT(ok, "each BulgeSizeOn 3 merger must leave the tabulated bulge radii");
  return TEST_PASS;
}

int main(void) {
  initialize_error_handling(LOG_LEVEL_WARNING, NULL);

  printf("%s", BLUE);
  printf("============================================================\n");
  printf("Unit Test Suite: SAGE26 Merger and Disruption Resolver End State\n");
  printf("============================================================\n");
  printf("%s", NC);

  TEST_RUN(test_pass_selection);
  TEST_RUN(test_merger_split_and_transfer);
  TEST_RUN(test_mode3_bulge_radii);

  TEST_SUMMARY();
  return TEST_RESULT();
}
