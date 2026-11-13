/**
 * @file    rate_cost.c
 * @brief   Manual cost driver: times hand-built stars through the rate integrator alone
 *
 * Not part of any test tier and not built by the Makefile: `python3
 * tests/manual/run_rate_battery.py --cost` compiles it against an explicit source list and runs
 * it. It measures what the integrator costs by itself, with no galaxies, modules, dispatch or I/O:
 *
 *   - a star of N satellites around one central, two reservoirs per galaxy and the cross-record
 *     transfers of the unit tests' uniform star (satellite gas to the central's hot gas and to
 *     its own stars, the central's hot gas to its cold gas), whose central content scales with N
 *     so the dynamics, in each entry's own scale, do not depend on N;
 *   - constant coefficients, so a law evaluation is a copy and what is timed is the integrator;
 *   - the production defaults of the integration settings, adaptive, under each scheme.
 *
 * For each scheme and each N in {10, 100, 1,000, 10,000} one segment is run and discarded, then
 * REPEATS timed repeats. A repeat is as many back-to-back segments as it takes the integrator's
 * own time to reach MIN_REPEAT_SECONDS (counted by the discarded segment's duration), each
 * timed individually with CLOCK_MONOTONIC around the integrator call alone (the state is
 * restored outside the timed region), so a small star's repeat is not a clock-resolution
 * measurement. The figures per size are the accepted, rejected and law-evaluation counts of one
 * segment (identical in every segment, which the driver checks), the workspace bytes per
 * galaxy, and the time per galaxy per law evaluation: the median over the repeats (the
 * statistic the bounds judge) and the minimum and maximum.
 *
 * Bounds, fixed before measuring and judged on the median: for each scheme, the time per galaxy
 * per law evaluation at 10,000 satellites is at most 1.5 times that at 100; the workspace bytes
 * per galaxy at 10,000 are within 10% of those at 1,000. Every figure is printed whether or not
 * a bound holds; the exit status is 1 when a bound misses or a segment fails, else 0.
 *
 * Usage: rate_cost            (no arguments; one line per size and one verdict per bound)
 */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "rate_integrator.h"

/* The two globals and the exit hook that util/error.c and the integrator reach for, which the
 * executable's main.c and allvars.c define; the driver stands in for them so it links against
 * the integrator and the error module alone. */
int ThisTask, NTask;
void myexit(int signum) { exit(signum); }

#define NUM_SIZES 4
#define NUM_SCHEMES 2
#define REPEATS 7               /* timed repeats per size and scheme, after one discarded segment */
#define MIN_REPEAT_SECONDS 0.05 /* integrator time one repeat accumulates at least */
#define MAX_SEGMENTS_PER_REPEAT 100000
#define TIME_BOUND 1.5       /* time per galaxy per evaluation, 10,000 against 100 */
#define WORKSPACE_BOUND 0.10 /* workspace bytes per galaxy, 10,000 within this of 1,000 */

static const int SATELLITES[NUM_SIZES] = {10, 100, 1000, 10000};
static const enum RateScheme SCHEMES[NUM_SCHEMES] = {RATE_SCHEME_EXPLICIT, RATE_SCHEME_IMPLICIT};
static const char *const SCHEME_NAMES[NUM_SCHEMES] = {"explicit", "implicit"};

/* Segment lengths of the central and of every satellite, and the constant coefficients. */
#define CENTRAL_DELTA 1.25
#define SATELLITE_DELTA 1.1
#define CENTRAL_COOLING 0.9
#define SATELLITE_TO_CENTRAL 1.5
#define SATELLITE_FORMATION 0.75

/** A star under construction: its component, its start state and its constant coefficients. */
struct Star {
  struct RateEntry *entries;
  struct RateFlux *fluxes;
  double *delta;
  double *start;
  double *coefficient;
  int num_entries;
  int num_fluxes;
  int num_rows;
  int galaxies;
};

/** The law: every coefficient is its flux's constant, so an evaluation is one copy. */
static int constant_law(void *data, double tau, const double *state, double *coefficients) {
  const struct Star *star = data;
  (void)tau;
  (void)state;
  memcpy(coefficients, star->coefficient, (size_t)star->num_fluxes * sizeof(double));
  return 0;
}

static void *allocate(size_t count, size_t size) {
  void *pointer = calloc(count, size);
  if (pointer == NULL) {
    fprintf(stderr, "rate_cost: out of memory\n");
    exit(2);
  }
  return pointer;
}

/** Build the uniform star of @p satellites satellites: 2 entries and 2 fluxes per satellite. */
static struct Star build_star(int satellites) {
  struct Star star;
  star.galaxies = satellites + 1;
  star.entries = allocate((size_t)(2 * star.galaxies), sizeof(*star.entries));
  star.fluxes = allocate((size_t)(2 * satellites + 1), sizeof(*star.fluxes));
  star.delta = allocate((size_t)star.galaxies, sizeof(*star.delta));
  star.start = allocate((size_t)(2 * star.galaxies), sizeof(*star.start));
  star.coefficient = allocate((size_t)(2 * satellites + 1), sizeof(*star.coefficient));
  star.num_entries = 0;
  star.num_fluxes = 0;
  star.num_rows = 0;

  star.delta[star.num_rows] = CENTRAL_DELTA;
  const int hot = star.num_entries++;
  star.entries[hot] = (struct RateEntry){.row = 0, .total = 0, .parent = -1, .flux = -1};
  star.start[hot] = 0.08 * (double)satellites;
  const int cold = star.num_entries++;
  star.entries[cold] = (struct RateEntry){.row = 0, .total = 0, .parent = -1, .flux = -1};
  star.start[cold] = 0.01 * (double)satellites;
  star.num_rows++;
  star.fluxes[star.num_fluxes] = (struct RateFlux){
      .kind = RATE_FLUX_INTERNAL, .from = hot, .to = cold, .global_index = 0, .master = -1};
  star.coefficient[star.num_fluxes++] = CENTRAL_COOLING;

  for (int s = 1; s <= satellites; s++) {
    const int row = star.num_rows++;
    star.delta[row] = SATELLITE_DELTA;
    const int gas = star.num_entries++;
    star.entries[gas] = (struct RateEntry){.row = row, .total = 0, .parent = -1, .flux = -1};
    star.start[gas] = 0.6;
    const int stars = star.num_entries++;
    star.entries[stars] = (struct RateEntry){.row = row, .total = 0, .parent = -1, .flux = -1};
    star.start[stars] = 0.0;
    star.fluxes[star.num_fluxes] = (struct RateFlux){
        .kind = RATE_FLUX_INTERNAL, .from = gas, .to = hot, .global_index = 1, .master = -1};
    star.coefficient[star.num_fluxes++] = SATELLITE_TO_CENTRAL;
    star.fluxes[star.num_fluxes] = (struct RateFlux){
        .kind = RATE_FLUX_INTERNAL, .from = gas, .to = stars, .global_index = 2, .master = -1};
    star.coefficient[star.num_fluxes++] = SATELLITE_FORMATION;
  }
  return star;
}

static void free_star(struct Star *star) {
  free(star->entries);
  free(star->fluxes);
  free(star->delta);
  free(star->start);
  free(star->coefficient);
}

static double now_seconds(void) {
  struct timespec time;
  clock_gettime(CLOCK_MONOTONIC, &time);
  return (double)time.tv_sec + 1e-9 * (double)time.tv_nsec;
}

static int compare_doubles(const void *a, const void *b) {
  const double x = *(const double *)a;
  const double y = *(const double *)b;
  return (x > y) - (x < y);
}

/** The result of measuring one size under one scheme. */
struct Measurement {
  int satellites;
  size_t workspace_bytes;
  double bytes_per_galaxy;
  int64_t accepted;
  int64_t rejected;
  int64_t evaluations;
  int segments_per_repeat;
  double median, minimum, maximum; /* seconds per galaxy per law evaluation over the repeats */
  int ok;
};

/**
 * Run one segment from the star's start state, timing the integrator call alone.
 * @return  The call's duration in seconds, or -1 when the segment failed.
 */
static double run_segment(const struct Star *star, const struct RateIntegratorSettings *settings,
                          void *workspace, size_t workspace_bytes, double *state,
                          struct RateSegmentOutcome *outcome) {
  const struct RateComponent component = {.entries = star->entries,
                                          .num_entries = star->num_entries,
                                          .fluxes = star->fluxes,
                                          .num_fluxes = star->num_fluxes,
                                          .row_delta = star->delta,
                                          .num_rows = star->num_rows,
                                          .num_totals = 1};
  const struct RateLaw law = {.evaluate = constant_law, .data = (void *)star};
  double source = 0.0;
  double sink = 0.0;
  memcpy(state, star->start, (size_t)star->num_entries * sizeof(double));
  const double begin = now_seconds();
  const enum RateCause cause = rate_integrate_segment(
      &component, &law, settings, workspace, workspace_bytes, state, &source, &sink, outcome);
  const double end = now_seconds();
  return (cause == RATE_CAUSE_NONE) ? end - begin : -1.0;
}

static struct Measurement measure(int satellites, enum RateScheme scheme) {
  struct Measurement result;
  memset(&result, 0, sizeof(result));
  result.satellites = satellites;
  struct Star star = build_star(satellites);
  const struct RateComponent component = {.entries = star.entries,
                                          .num_entries = star.num_entries,
                                          .fluxes = star.fluxes,
                                          .num_fluxes = star.num_fluxes,
                                          .row_delta = star.delta,
                                          .num_rows = star.num_rows,
                                          .num_totals = 1};
  struct RateIntegratorSettings settings;
  rate_integrator_default_settings(&settings);
  settings.scheme = scheme;

  result.workspace_bytes = rate_integrator_workspace_bytes(&component, scheme);
  result.bytes_per_galaxy = (double)result.workspace_bytes / (double)star.galaxies;
  double *workspace = allocate(1, result.workspace_bytes);
  double *state = allocate((size_t)star.num_entries, sizeof(double));

  struct RateSegmentOutcome outcome;
  const double first =
      run_segment(&star, &settings, workspace, result.workspace_bytes, state, &outcome);
  if (first < 0.0) {
    fprintf(stderr, "rate_cost: the %d-satellite %s segment failed: %s\n", satellites,
            (scheme == RATE_SCHEME_EXPLICIT) ? "explicit" : "implicit",
            rate_cause_name(outcome.failure.cause));
    free(workspace);
    free(state);
    free_star(&star);
    return result;
  }
  result.accepted = outcome.stats.accepted_steps;
  result.rejected = outcome.stats.rejected_steps;
  result.evaluations = outcome.stats.law_evaluations;
  int segments = (int)ceil(MIN_REPEAT_SECONDS / (first > 1e-9 ? first : 1e-9));
  segments = (segments < 1)
                 ? 1
                 : (segments > MAX_SEGMENTS_PER_REPEAT ? MAX_SEGMENTS_PER_REPEAT : segments);
  result.segments_per_repeat = segments;

  double per_unit[REPEATS];
  result.ok = 1;
  for (int r = 0; r < REPEATS && result.ok; r++) {
    double total = 0.0;
    for (int k = 0; k < segments; k++) {
      const double seconds =
          run_segment(&star, &settings, workspace, result.workspace_bytes, state, &outcome);
      if (seconds < 0.0 || outcome.stats.accepted_steps != result.accepted ||
          outcome.stats.rejected_steps != result.rejected ||
          outcome.stats.law_evaluations != result.evaluations) {
        fprintf(stderr, "rate_cost: a %d-satellite segment failed or differed from the first\n",
                satellites);
        result.ok = 0;
        break;
      }
      total += seconds;
    }
    per_unit[r] = total / (double)segments / (double)star.galaxies / (double)result.evaluations;
  }
  if (result.ok) {
    qsort(per_unit, REPEATS, sizeof(double), compare_doubles);
    result.minimum = per_unit[0];
    result.median = per_unit[REPEATS / 2];
    result.maximum = per_unit[REPEATS - 1];
  }
  free(workspace);
  free(state);
  free_star(&star);
  return result;
}

int main(void) {
  struct Measurement all[NUM_SCHEMES][NUM_SIZES];
  int failed = 0;
  printf("rate_cost: uniform stars, production default settings, %d timed repeats after one "
         "discarded segment\n",
         REPEATS);
  printf("%-9s %6s %8s %10s %9s %8s %8s %6s %14s %14s %14s\n", "scheme", "sats", "galaxies",
         "ws_B/gal", "accepted", "rejected", "evals", "segs", "median_ns", "min_ns", "max_ns");
  for (int s = 0; s < NUM_SCHEMES; s++) {
    for (int n = 0; n < NUM_SIZES; n++) {
      all[s][n] = measure(SATELLITES[n], SCHEMES[s]);
      const struct Measurement *m = &all[s][n];
      if (!m->ok) {
        failed = 1;
        printf("%-9s %6d FAILED\n", SCHEME_NAMES[s], m->satellites);
        continue;
      }
      printf("%-9s %6d %8d %10.2f %9lld %8lld %8lld %6d %14.4f %14.4f %14.4f\n", SCHEME_NAMES[s],
             m->satellites, m->satellites + 1, m->bytes_per_galaxy, (long long)m->accepted,
             (long long)m->rejected, (long long)m->evaluations, m->segments_per_repeat,
             m->median * 1e9, m->minimum * 1e9, m->maximum * 1e9);
      fflush(stdout);
    }
  }
  printf("(median_ns, min_ns, max_ns: nanoseconds per galaxy per law evaluation over the repeats; "
         "segs: segments per repeat)\n");
  if (failed) {
    printf("RATE_COST: FAIL a segment failed\n");
    return 1;
  }
  for (int s = 0; s < NUM_SCHEMES; s++) {
    const struct Measurement *small = &all[s][1]; /* 100 satellites */
    const struct Measurement *mid = &all[s][2];   /* 1,000 satellites */
    const struct Measurement *large = &all[s][3]; /* 10,000 satellites */
    const double time_ratio = large->median / small->median;
    const double best_ratio = large->minimum / small->minimum;
    const double bytes_ratio = large->bytes_per_galaxy / mid->bytes_per_galaxy;
    const int time_ok = time_ratio <= TIME_BOUND;
    const int bytes_ok = fabs(bytes_ratio - 1.0) <= WORKSPACE_BOUND;
    printf("RATE_COST: %s time per galaxy per evaluation, 10,000 against 100: median ratio %.3f "
           "(best-case ratio %.3f), bound %.1f: %s\n",
           SCHEME_NAMES[s], time_ratio, best_ratio, TIME_BOUND, time_ok ? "PASS" : "FAIL");
    printf("RATE_COST: %s workspace bytes per galaxy, 10,000 against 1,000: ratio %.4f, bound "
           "within %.0f%%: %s\n",
           SCHEME_NAMES[s], bytes_ratio, WORKSPACE_BOUND * 100.0, bytes_ok ? "PASS" : "FAIL");
    failed |= !time_ok || !bytes_ok;
  }
  printf("RATE_COST: %s\n", failed ? "FAIL a bound missed" : "PASS every bound held");
  return failed ? 1 : 0;
}
