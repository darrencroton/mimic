/**
 * @file    test_rate_tracers.c
 * @brief   Tracers, slaved sources and flux amounts in the explicit rate integrator
 *
 * Validates: rate_integrate_segment() on hand-built components with tracer
 * entries and slaved sources: tracer dilution by a pristine source, a yield
 * slaved to a formation transfer against its closed forms, the tracer total's
 * closure, the yield-to-formed ratio, an empty donor, the tracer bound (the
 * growth of a tracer's excess over its parent, with credit for the excess at
 * the segment start), the segment's flux amounts as exact sums of the
 * committed amounts, a tracer leaving its total where the recipient has no
 * tracer, a tracer's error weight on its parent's total, a self-transfer that
 * commits nothing, the tracer bound on a zero-Delta segment and on a discrete
 * residue, a cross-row tracer move with mixed Delta, two tracers of one parent,
 * canonical order of the derived tracer moves, and the refusal of a slaved
 * chain.
 *
 * Every expected value comes from a closed form written in this file, never
 * from the integrator; L, in tol_F, is a literal counted here for each
 * problem. Times are in normalised segment time tau, so a row with segment
 * length Delta has evolved for t = Delta at tau = 1. The error of a problem is
 * the largest absolute difference between a computed entry and its closed form
 * at tau = 1. Unless a case says otherwise the absolute tolerance is 1e-3 times
 * the relative tolerance. Each case prints its measured figures beside their
 * bounds.
 */

#define MAX_ENTRIES 16
#define MAX_FLUXES 16
#define MAX_ROWS 2
#define WORKSPACE_DOUBLES (1 << 14)

#include "../framework/test_framework.h"
#include "../framework/child_capture.h"
#include "../../src/core/rate_integrator.h"
#include "../../src/util/error.h"
#include "rate_test_support.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Test statistics (required for TEST_RUN macro) */
static int passed = 0;
static int failed = 0;

/** Start a one-row system: row 0 with segment length @p delta, @p num_totals totals. */
static void one_row_system(struct System *system, double delta, int num_totals) {
  system_init(system, 0, num_totals);
  add_row(system, delta);
}

/* ---------------------------------------------------------------------------
 * The yield problem and its closed forms
 * ------------------------------------------------------------------------- */

/* Yield problem constants: source s into G, formation G to S at k, yield y into Z; Delta. */
#define YIELD_SOURCE 0.4
#define YIELD_FORMATION 1.3
#define YIELD_FRACTION 0.02
#define YIELD_DELTA 3.0

/** Entry and flux indices of a built yield problem. */
struct Yield {
  int gas;       /* G, total 0 */
  int stars;     /* S, total 0 */
  int metals;    /* Z, tracer of G, total 1 */
  int star_z;    /* Z_S, tracer of S, total 1 */
  int source;    /* flux: s into G, or -1 */
  int formation; /* flux: G to S at k */
  int yield;     /* flux: y times formation into Z (slaved) */
};

/**
 * Yield: G (start g0) fed by an absolute source s (when @p source > 0), formation
 * G to S at k carrying Z to Z_S, and a yield of @p fraction slaved to formation
 * into Z. Fluxes are listed in declaration order, or reversed when @p reversed.
 */
static struct Yield build_yield(struct System *system, double g0, double z0, double source,
                                double fraction, int reversed) {
  struct Yield y = {.source = -1};
  one_row_system(system, YIELD_DELTA, 2);
  y.gas = add_reservoir(system, 0, g0);
  y.stars = add_reservoir(system, 0, 0.0);
  y.metals = add_tracer(system, 1, y.gas, z0);
  y.star_z = add_tracer(system, 1, y.stars, 0.0);
  if (!reversed) {
    if (source > 0.0) {
      y.source = add_flux(system, RATE_FLUX_SOURCE, RATE_END_EXTERNAL, y.gas, 0, -1, source);
    }
    y.formation = add_flux(system, RATE_FLUX_INTERNAL, y.gas, y.stars, 1, -1, YIELD_FORMATION);
    y.yield =
        add_flux(system, RATE_FLUX_SLAVED, RATE_END_EXTERNAL, y.metals, 2, y.formation, fraction);
  } else {
    /* The slaved source comes first and names index 1, the index formation takes next. */
    y.yield = add_flux(system, RATE_FLUX_SLAVED, RATE_END_EXTERNAL, y.metals, 2, 1, fraction);
    y.formation = add_flux(system, RATE_FLUX_INTERNAL, y.gas, y.stars, 1, -1, YIELD_FORMATION);
    if (source > 0.0) {
      y.source = add_flux(system, RATE_FLUX_SOURCE, RATE_END_EXTERNAL, y.gas, 0, -1, source);
    }
  }
  return y;
}

/** The yield problem's four closed forms at t = Delta, for start (g0, 0, z0, 0). */
struct YieldExact {
  double gas;
  double stars;
  double metals;
  double star_z;
};

/**
 * G = s/k + (G0 - s/k) e^(-kt); Z = Z0 e^(-kt) + y (s/k)(1 - e^(-kt)) + y k (G0 - s/k) t e^(-kt).
 * Mass and tracer totals then fix S = G0 + s t - G and Z_S = Z0 + y (S - S0) - Z, the
 * formed amount S - S0 = G0 + s t - G being what the yield is slaved to.
 */
static struct YieldExact yield_exact(double g0, double z0, double s) {
  const double k = YIELD_FORMATION;
  const double y = YIELD_FRACTION;
  const double t = YIELD_DELTA;
  const double decay = exp(-k * t);
  const double equilibrium = s / k;
  struct YieldExact exact;
  exact.gas = equilibrium + (g0 - equilibrium) * decay;
  exact.metals =
      z0 * decay + y * equilibrium * (1.0 - decay) + y * k * (g0 - equilibrium) * t * decay;
  exact.stars = g0 + s * t - exact.gas;
  exact.star_z = z0 + y * exact.stars - exact.metals;
  return exact;
}

static double yield_error(const struct System *system, const struct Yield *y,
                          const struct YieldExact *exact) {
  const double *x = system->state;
  return fmax(fmax(fabs(x[y->gas] - exact->gas), fabs(x[y->metals] - exact->metals)),
              fmax(fabs(x[y->stars] - exact->stars), fabs(x[y->star_z] - exact->star_z)));
}

/* ---------------------------------------------------------------------------
 * Tests
 * ------------------------------------------------------------------------- */

/**
 * @test    A pristine source dilutes a tracer without changing its content
 *
 * Parent G0 = 1 fed by a pristine source of 0.5, tracer Z0 = 0.02, Delta = 3,
 * default tolerances. Expected: Z/G at tau = 1 equals Z0/(G0 + 0.5 Delta) to
 * 1e-12 relative, and no tracer amount enters or leaves the tracer's total.
 */
static int test_dilution_concentration(void) {
  struct System system;
  one_row_system(&system, 3.0, 2);
  const int gas = add_reservoir(&system, 0, 1.0);
  const int metals = add_tracer(&system, 1, gas, 0.02);
  add_flux(&system, RATE_FLUX_SOURCE, RATE_END_EXTERNAL, gas, 0, -1, 0.5);
  struct RateIntegratorSettings settings;
  rate_integrator_default_settings(&settings);
  struct Run run;
  TEST_ASSERT(run_segment(&system, NULL, &settings, &run) == 0, "workspace too small");
  TEST_ASSERT(run.cause == RATE_CAUSE_NONE, "dilution segment failed");

  const double expected = 0.02 / (1.0 + 0.5 * 3.0);
  const double concentration = system.state[metals] / system.state[gas];
  const double relative = fabs(concentration - expected) / expected;
  printf("  dilution: Z/G = %.17g, expected %.17g, relative error %.3e (bound 1e-12)\n",
         concentration, expected, relative);
  TEST_ASSERT(relative <= 1e-12, "dilution concentration off by more than 1e-12 relative");
  TEST_ASSERT(run.source[1] == 0.0 && run.sink[1] == 0.0,
              "a pristine source moved tracer content into or out of the tracer total");
  return 0;
}

/**
 * @test    A yield slaved to formation meets the closed forms of G and Z
 *
 * G0 = 1, Z0 = 0.01, source 0.4 into G, formation G to S at k = 1.3, yield 0.02
 * slaved to formation into Z, Delta = 3, relative tolerance 1e-8. Expected: G,
 * Z, S and Z_S each within 10 x the relative tolerance of their closed forms.
 */
static int test_yield_closed_forms(void) {
  struct System system;
  const struct Yield y = build_yield(&system, 1.0, 0.01, YIELD_SOURCE, YIELD_FRACTION, 0);
  const struct RateIntegratorSettings settings = adaptive_settings(1e-8);
  struct Run run;
  TEST_ASSERT(run_segment(&system, NULL, &settings, &run) == 0, "workspace too small");
  TEST_ASSERT(run.cause == RATE_CAUSE_NONE, "yield segment failed");

  const struct YieldExact exact = yield_exact(1.0, 0.01, YIELD_SOURCE);
  const double gas_error = fabs(system.state[y.gas] - exact.gas);
  const double metals_error = fabs(system.state[y.metals] - exact.metals);
  const double error = yield_error(&system, &y, &exact);
  printf("  yield: |G - exact| = %.3e, |Z - exact| = %.3e, all four entries %.3e "
         "(bound 1e-7), %lld accepted steps\n",
         gas_error, metals_error, error, (long long)run.outcome.stats.accepted_steps);
  TEST_ASSERT(error <= 10.0 * 1e-8, "yield problem misses its closed forms");
  return 0;
}

/**
 * @test    The tracer total of the yield problem closes with the yield as its source
 *
 * Expected: the tracer total (Z + Z_S) records the yield as its external source
 * and no sink, and changes by that source within tol_F.
 */
static int test_yield_tracer_total_closes(void) {
  struct System system;
  const struct Yield y = build_yield(&system, 1.0, 0.01, YIELD_SOURCE, YIELD_FRACTION, 0);
  const double start = system.state[y.metals] + system.state[y.star_z];
  const double start_magnitude = fabs(system.state[y.metals]) + fabs(system.state[y.star_z]);
  const struct RateIntegratorSettings settings = adaptive_settings(1e-8);
  struct Run run;
  TEST_ASSERT(run_segment(&system, NULL, &settings, &run) == 0, "workspace too small");
  TEST_ASSERT(run.cause == RATE_CAUSE_NONE, "yield segment failed");

  const double end = system.state[y.metals] + system.state[y.star_z];
  const double drift = fabs((end - start) - (run.source[1] - run.sink[1]));
  /* L = 2: G and Z each carry two terms (a source or yield in, formation or its move out). */
  const double tolerance = closure_tolerance(2, run.outcome.stats.accepted_steps, 2,
                                             start_magnitude, run.source[1], run.sink[1]);
  printf("  tracer total: source %.17g, sink %.3g, drift %.3e (tol_F %.3e)\n", run.source[1],
         run.sink[1], drift, tolerance);
  TEST_ASSERT(run.source[1] > 0.0 && run.sink[1] == 0.0,
              "the tracer total's external amounts are not the yield alone");
  TEST_ASSERT(drift <= tolerance, "the tracer total does not close within tol_F");
  return 0;
}

/**
 * @test    The yield amount is 0.02 x the formed amount
 *
 * The formed amount is formation's flux amount over the segment; the yield
 * amount is the tracer total's source. Expected: |yield - 0.02 formed| <= 8u
 * N_acc (0.02 formed).
 */
static int test_yield_amount_matches_formed(void) {
  struct System system;
  const struct Yield y = build_yield(&system, 1.0, 0.01, YIELD_SOURCE, YIELD_FRACTION, 0);
  const struct RateIntegratorSettings settings = adaptive_settings(1e-8);
  static struct Run run;
  TEST_ASSERT(run_segment(&system, NULL, &settings, &run) == 0, "workspace too small");
  TEST_ASSERT(run.cause == RATE_CAUSE_NONE, "yield segment failed");

  const double expected = YIELD_FRACTION * run.flux[y.formation];
  const double difference = fabs(run.source[1] - expected);
  const double bound =
      8.0 * UNIT_ROUNDOFF * (double)run.outcome.stats.accepted_steps * fabs(expected);
  printf("  yield %.17g against 0.02 x formed %.17g: difference %.3e (bound %.3e)\n", run.source[1],
         expected, difference, bound);
  TEST_ASSERT(difference <= bound, "the yield amount departs from 0.02 x the formed amount");
  return 0;
}

/**
 * @test    An empty donor stays finite and meets the closed forms
 *
 * The yield problem with G0 = Z0 = 0. Expected: every committed value, step,
 * weight, error norm, external amount and flux amount finite, and every entry
 * within 10 x the relative tolerance of its closed form.
 */
static int test_empty_donor(void) {
  struct System system;
  const struct Yield y = build_yield(&system, 0.0, 0.0, YIELD_SOURCE, YIELD_FRACTION, 0);
  struct RateIntegratorSettings settings = adaptive_settings(1e-8);
  struct AttemptLog log;
  attach_log(&settings, &log, &system);
  struct Run run;
  TEST_ASSERT(run_segment(&system, NULL, &settings, &run) == 0, "workspace too small");
  TEST_ASSERT(run.cause == RATE_CAUSE_NONE, "empty-donor segment failed");

  int finite = !log.non_finite;
  for (int i = 0; i < system.num_entries; i++) {
    finite = finite && isfinite(system.state[i]);
  }
  for (int t = 0; t < system.num_totals; t++) {
    finite = finite && isfinite(run.source[t]) && isfinite(run.sink[t]);
  }
  for (int f = 0; f < system.num_fluxes; f++) {
    finite = finite && isfinite(run.flux[f]);
  }
  const struct YieldExact exact = yield_exact(0.0, 0.0, YIELD_SOURCE);
  const double error = yield_error(&system, &y, &exact);
  printf("  empty donor: error %.3e (bound 1e-7), formed %.17g, %lld attempts, all finite: %s\n",
         error, run.flux[y.formation], (long long)log.count, finite ? "yes" : "no");
  TEST_ASSERT(finite, "an empty donor produced a value that is not finite");
  TEST_ASSERT(error <= 10.0 * 1e-8, "empty-donor problem misses its closed forms");
  return 0;
}

/**
 * @test    The tracer bound limits the growth of a tracer's excess over its parent
 *
 * (a) The yield problem with a yield of 0.5 and no source: Z - G = e^(-kt) (Z0 -
 * G0 + 0.5 k t) > 0 at t = 3, from Z0 below G0. Expected: cause tracer_bound
 * naming Z, its total and the yield, a detail saying the tracer has sources of
 * its own, and the state untouched. (b) An over-full donor: G0 = 1 with Z0 = 2
 * (an excess of 1), formation G to S at 1.3 into S0 = 0 with Z_S0 = 0, Delta =
 * 3, no source: Z - G = e^(-1.3 t) stays below its start excess, but Z_S - S = 1
 * - e^(-1.3 t) grows from none. Expected: tracer_bound naming Z_S alone, with a
 * detail saying it has no sources of its own, and the state untouched. (c) A
 * tracer started above its parent whose excess falls: G0 = 1 with Z0 = 2,
 * drained by a sink at 1.3, Delta = 3, relative tolerance 1e-8. Expected:
 * success, with G = e^(-1.3 t) and Z = 2 e^(-1.3 t) within 10 x the relative
 * tolerance.
 */
static int test_tracer_bound(void) {
  struct System system;
  const struct Yield y = build_yield(&system, 1.0, 0.01, 0.0, 0.5, 0);
  double before[MAX_ENTRIES];
  memcpy(before, system.state, sizeof(before));
  const struct RateIntegratorSettings settings = adaptive_settings(1e-8);
  static struct Run run;
  TEST_ASSERT(run_segment(&system, NULL, &settings, &run) == 0, "workspace too small");
  const struct RateFailure *failure = &run.outcome.failure;
  printf("  yield 0.5: cause %s, entry %d, flux %d, total %d, detail \"%s\"\n",
         rate_cause_name(run.cause), failure->entry, failure->flux, failure->total,
         failure->detail);
  TEST_ASSERT(run.cause == RATE_CAUSE_TRACER_BOUND, "a yield of 0.5 did not fail tracer_bound");
  TEST_ASSERT(strcmp(rate_cause_name(run.cause), "tracer_bound") == 0, "wrong cause name");
  TEST_ASSERT(failure->entry == y.metals && failure->flux == y.yield && failure->total == 1,
              "the failure does not name the tracer, its total and its yield");
  TEST_ASSERT(strstr(failure->detail, "has sources of its own") != NULL,
              "the detail does not say the tracer has sources of its own");
  TEST_ASSERT(memcmp(before, system.state, sizeof(before)) == 0,
              "a tracer_bound failure changed the caller's state");

  one_row_system(&system, YIELD_DELTA, 2);
  const int gas = add_reservoir(&system, 0, 1.0);
  const int stars = add_reservoir(&system, 0, 0.0);
  add_tracer(&system, 1, gas, 2.0);
  const int star_metals = add_tracer(&system, 1, stars, 0.0);
  add_flux(&system, RATE_FLUX_INTERNAL, gas, stars, 0, -1, YIELD_FORMATION);
  memcpy(before, system.state, sizeof(before));
  TEST_ASSERT(run_segment(&system, NULL, &settings, &run) == 0, "workspace too small");
  printf("  over-full donor: cause %s, entry %d, flux %d, detail \"%s\"\n",
         rate_cause_name(run.cause), failure->entry, failure->flux, failure->detail);
  TEST_ASSERT(run.cause == RATE_CAUSE_TRACER_BOUND, "a tracer overtaking its parent passed");
  TEST_ASSERT(failure->entry == star_metals && failure->flux == -1 && failure->total == 1,
              "the failure does not name the overtaking tracer alone");
  TEST_ASSERT(strstr(failure->detail, "has no sources of its own") != NULL &&
                  strstr(failure->detail, "has sources of its own") == NULL,
              "the detail does not say the tracer has no sources of its own");
  TEST_ASSERT(memcmp(before, system.state, sizeof(before)) == 0,
              "a tracer_bound failure changed the caller's state");

  one_row_system(&system, YIELD_DELTA, 2);
  const int drained = add_reservoir(&system, 0, 1.0);
  const int drained_metals = add_tracer(&system, 1, drained, 2.0);
  add_flux(&system, RATE_FLUX_SINK, drained, RATE_END_EXTERNAL, 0, -1, YIELD_FORMATION);
  TEST_ASSERT(run_segment(&system, NULL, &settings, &run) == 0, "workspace too small");
  const double decay = exp(-YIELD_FORMATION * YIELD_DELTA);
  const double error =
      fmax(fabs(system.state[drained] - decay), fabs(system.state[drained_metals] - 2.0 * decay));
  printf("  tracer above a drained parent: cause %s, Z - G = %.3e (start excess 1), error %.3e "
         "(bound 1e-7)\n",
         rate_cause_name(run.cause), system.state[drained_metals] - system.state[drained], error);
  TEST_ASSERT(run.cause == RATE_CAUSE_NONE, "a tracer whose excess fell failed the tracer bound");
  TEST_ASSERT(error <= 10.0 * 1e-8, "the drained parent and tracer miss their closed forms");
  return 0;
}

/**
 * @test    A segment's flux amounts are the sums of its committed amounts, bit for bit
 *
 * The yield problem at relative tolerance 1e-8. Expected: formation's and the
 * yield's flux amounts each equal the sum, in step order, of the committed
 * amounts the attempt reports give for that flux; the source's flux amount
 * equals the mass total's external source and the yield's the tracer total's,
 * each the only external inflow of its total, bit for bit.
 */
static int test_flux_amounts_sum_committed_amounts(void) {
  struct System system;
  const struct Yield y = build_yield(&system, 1.0, 0.01, YIELD_SOURCE, YIELD_FRACTION, 0);
  struct RateIntegratorSettings settings = adaptive_settings(1e-8);
  struct AttemptLog log;
  attach_log(&settings, &log, &system);
  log.watched[0] = y.formation;
  log.watched[1] = y.yield;
  static struct Run run;
  TEST_ASSERT(run_segment(&system, NULL, &settings, &run) == 0, "workspace too small");
  TEST_ASSERT(run.cause == RATE_CAUSE_NONE, "yield segment failed");
  TEST_ASSERT(!log.null_amounts, "an accepted attempt reported no committed amounts");

  printf("  formed %.17g, sum of committed amounts %.17g; yielded %.17g, sum %.17g; source "
         "%.17g, mass total's source %.17g\n",
         run.flux[y.formation], log.sum[0], run.flux[y.yield], log.sum[1], run.flux[y.source],
         run.source[0]);
  TEST_ASSERT(memcmp(&run.flux[y.formation], &log.sum[0], sizeof(double)) == 0,
              "formation's flux amount differs from the sum of its committed amounts");
  TEST_ASSERT(memcmp(&run.flux[y.yield], &log.sum[1], sizeof(double)) == 0,
              "the yield's flux amount differs from the sum of its committed amounts");
  TEST_ASSERT(memcmp(&run.flux[y.source], &run.source[0], sizeof(double)) == 0,
              "the source's flux amount differs from the mass total's external source");
  TEST_ASSERT(memcmp(&run.flux[y.yield], &run.source[1], sizeof(double)) == 0,
              "the yield's flux amount differs from the tracer total's external source");
  return 0;
}

/**
 * @test    A tracer leaves its total where the recipient has no tracer of that total
 *
 * G0 = 1 with tracer Z0 = 0.05; formation G to S at 1.3, where S has no tracer,
 * and a sink out of G at 0.4; Delta = 3, relative tolerance 1e-8. Expected: Z =
 * Z0 e^(-1.7 t) and the tracer total's sink Z0 (1 - e^(-1.7 t)), each within 10
 * x the relative tolerance, no tracer source, and the tracer total within tol_F.
 */
static int test_tracer_without_recipient_tracer_is_a_sink(void) {
  struct System system;
  one_row_system(&system, 3.0, 2);
  const int gas = add_reservoir(&system, 0, 1.0);
  const int stars = add_reservoir(&system, 0, 0.0);
  const int metals = add_tracer(&system, 1, gas, 0.05);
  add_flux(&system, RATE_FLUX_INTERNAL, gas, stars, 0, -1, 1.3);
  add_flux(&system, RATE_FLUX_SINK, gas, RATE_END_EXTERNAL, 1, -1, 0.4);
  const struct RateIntegratorSettings settings = adaptive_settings(1e-8);
  struct Run run;
  TEST_ASSERT(run_segment(&system, NULL, &settings, &run) == 0, "workspace too small");
  TEST_ASSERT(run.cause == RATE_CAUSE_NONE, "segment failed");

  const double decay = exp(-1.7 * 3.0);
  const double metals_error = fabs(system.state[metals] - 0.05 * decay);
  const double sink_error = fabs(run.sink[1] - 0.05 * (1.0 - decay));
  const double drift = fabs((system.state[metals] - 0.05) - (run.source[1] - run.sink[1]));
  /* L = 2: G carries formation and the sink out, Z their two tracer moves. */
  const double tolerance =
      closure_tolerance(2, run.outcome.stats.accepted_steps, 1, 0.05, run.source[1], run.sink[1]);
  printf("  |Z - exact| = %.3e, |sink - exact| = %.3e (bound 1e-7); drift %.3e (tol_F %.3e)\n",
         metals_error, sink_error, drift, tolerance);
  TEST_ASSERT(metals_error <= 10.0 * 1e-8, "the tracer misses its closed form");
  TEST_ASSERT(sink_error <= 10.0 * 1e-8, "the tracer total's sink misses its closed form");
  TEST_ASSERT(run.source[1] == 0.0, "the tracer total gained a source");
  TEST_ASSERT(drift <= tolerance, "the tracer total does not close within tol_F");
  return 0;
}

/** G0 = 1 to S0 = 1 at k = 3 over Delta = 3, with trace tracers of both when @p traced. */
static void build_traced_formation(struct System *system, int traced) {
  one_row_system(system, 3.0, 2);
  const int gas = add_reservoir(system, 0, 1.0);
  const int stars = add_reservoir(system, 0, 1.0);
  add_flux(system, RATE_FLUX_INTERNAL, gas, stars, 0, -1, 3.0);
  if (traced) {
    add_tracer(system, 1, gas, 1e-9);
    add_tracer(system, 1, stars, 1e-9);
  }
}

/**
 * @test    A tracer is weighed on its parent's total, not its own
 *
 * G0 = S0 = 1, G to S at k = 3, Delta = 3, relative tolerance 1e-6, once bare
 * and once with tracers of 1e-9 in both. A tracer's error is about 1e-9 of its
 * parent's, and its weight's scale is its parent's total (G + S = 2), so it
 * never sets the norm. Expected: bit-identical attempt sequences and end states
 * of G and S. Weighed on its own total (about 2e-9) it would set the norm once
 * G has decayed below the absolute term, and change the sequence.
 */
static int test_tracer_weighed_on_parent_total(void) {
  struct System bare;
  struct System traced;
  build_traced_formation(&bare, 0);
  build_traced_formation(&traced, 1);
  static struct AttemptLog bare_log;
  static struct AttemptLog traced_log;
  struct RateIntegratorSettings bare_settings = adaptive_settings(1e-6);
  struct RateIntegratorSettings traced_settings = adaptive_settings(1e-6);
  attach_log(&bare_settings, &bare_log, &bare);
  attach_log(&traced_settings, &traced_log, &traced);
  struct Run bare_run;
  struct Run traced_run;
  TEST_ASSERT(run_segment(&bare, NULL, &bare_settings, &bare_run) == 0, "workspace too small");
  TEST_ASSERT(run_segment(&traced, NULL, &traced_settings, &traced_run) == 0,
              "workspace too small");
  TEST_ASSERT(bare_run.cause == RATE_CAUSE_NONE && traced_run.cause == RATE_CAUSE_NONE,
              "formation segment failed");

  printf("  without tracers: %lld attempts; with tracers: %lld attempts\n",
         (long long)bare_log.count, (long long)traced_log.count);
  TEST_ASSERT(same_sequence(&bare_log, &traced_log), "trace tracers changed the step sequence");
  TEST_ASSERT(memcmp(bare.state, traced.state, 2 * sizeof(double)) == 0,
              "trace tracers changed the end state of their parents");
  return 0;
}

/**
 * @test    A self-transfer commits nothing: its flux amount, its reported amounts and its yield
 *
 * G0 = 1 with tracer Z0 = 0.01, S with tracer Z_S; formation G to S at 1.3, a
 * transfer G to G at 0.7 (both ends one entry), and a yield of 0.02 slaved to
 * the self-transfer into Z; Delta = 3, relative tolerance 1e-8. Expected: the
 * self-transfer's flux amount and the sum of its reported committed amounts are
 * both exactly 0, formation's flux amount equals the sum of its reported
 * amounts bit for bit, the tracer total gains no source, and Z = Z0 e^(-1.3 t)
 * within 10 x the relative tolerance.
 */
static int test_self_transfer_commits_nothing(void) {
  struct System system;
  one_row_system(&system, 3.0, 2);
  const int gas = add_reservoir(&system, 0, 1.0);
  const int stars = add_reservoir(&system, 0, 0.0);
  const int metals = add_tracer(&system, 1, gas, 0.01);
  add_tracer(&system, 1, stars, 0.0);
  const int formation = add_flux(&system, RATE_FLUX_INTERNAL, gas, stars, 0, -1, 1.3);
  const int self = add_flux(&system, RATE_FLUX_INTERNAL, gas, gas, 1, -1, 0.7);
  add_flux(&system, RATE_FLUX_SLAVED, RATE_END_EXTERNAL, metals, 2, self, 0.02);
  struct RateIntegratorSettings settings = adaptive_settings(1e-8);
  struct AttemptLog log;
  attach_log(&settings, &log, &system);
  log.watched[0] = self;
  log.watched[1] = formation;
  static struct Run run;
  TEST_ASSERT(run_segment(&system, NULL, &settings, &run) == 0, "workspace too small");
  TEST_ASSERT(run.cause == RATE_CAUSE_NONE, "self-transfer segment failed");

  const double zero = 0.0;
  const double metals_error = fabs(system.state[metals] - 0.01 * exp(-1.3 * 3.0));
  printf("  self-transfer flux amount %.17g, sum of its reported amounts %.17g; formed %.17g, "
         "sum %.17g; tracer source %.3g; |Z - exact| = %.3e (bound 1e-7)\n",
         run.flux[self], log.sum[0], run.flux[formation], log.sum[1], run.source[1], metals_error);
  TEST_ASSERT(memcmp(&run.flux[self], &zero, sizeof(double)) == 0,
              "the self-transfer's flux amount is not exactly zero");
  TEST_ASSERT(memcmp(&log.sum[0], &zero, sizeof(double)) == 0,
              "the self-transfer's reported committed amounts are not exactly zero");
  TEST_ASSERT(memcmp(&run.flux[formation], &log.sum[1], sizeof(double)) == 0,
              "formation's flux amount differs from the sum of its committed amounts");
  TEST_ASSERT(run.source[1] == 0.0, "a yield slaved to a self-transfer moved content");
  TEST_ASSERT(metals_error <= 10.0 * 1e-8, "the tracer misses its closed form");
  return 0;
}

/** G0 = 1 to S at 1.3 with tracers Z (start @p z0) and Z_S, on one row of Delta = 0. */
static void build_zero_delta(struct System *system, double z0) {
  one_row_system(system, 0.0, 2);
  const int gas = add_reservoir(system, 0, 1.0);
  const int stars = add_reservoir(system, 0, 0.0);
  add_tracer(system, 1, gas, z0);
  add_tracer(system, 1, stars, 0.0);
  add_flux(system, RATE_FLUX_INTERNAL, gas, stars, 0, -1, 1.3);
}

/**
 * @test    A segment with every Delta zero passes its unchanged state, whatever the tracers' excess
 *
 * The segment takes no step and evaluates no law, so N_acc = 0 and each
 * tracer's bound is its own start excess. Expected: Z0 = 0.5, 1 (equal to its
 * parent) and 1.5 (above it) all succeed with the state bitwise unchanged and
 * every external amount and the flux amount, each seeded with a sentinel before
 * the run, exactly zero; no case evaluates a law or takes a step.
 */
static int test_zero_delta_tracer_bound(void) {
  const double starts[] = {0.5, 1.0, 1.5};
  struct System system;
  static struct Run run;
  const struct RateIntegratorSettings settings = adaptive_settings(1e-8);
  for (int c = 0; c < 3; c++) {
    build_zero_delta(&system, starts[c]);
    double before[MAX_ENTRIES];
    memcpy(before, system.state, sizeof(before));
    for (int t = 0; t < MAX_TOTALS; t++) {
      run.source[t] = 42.0;
      run.sink[t] = 42.0;
    }
    run.flux[0] = 42.0;
    TEST_ASSERT(run_segment(&system, NULL, &settings, &run) == 0, "workspace too small");
    printf("  Delta = 0, Z0 = %.2f: cause %s, %lld law evaluations, %lld steps\n", starts[c],
           rate_cause_name(run.cause), (long long)run.evaluations,
           (long long)run.outcome.stats.accepted_steps);
    TEST_ASSERT(run.cause == RATE_CAUSE_NONE, "a zero-Delta segment failed its unchanged state");
    TEST_ASSERT(memcmp(before, system.state, sizeof(before)) == 0,
                "a zero-Delta segment changed the state");
    TEST_ASSERT(run.source[0] == 0.0 && run.sink[0] == 0.0 && run.source[1] == 0.0 &&
                    run.sink[1] == 0.0,
                "a zero-Delta segment reported a non-zero external amount");
    TEST_ASSERT(run.flux[0] == 0.0, "a zero-Delta segment reported a non-zero flux amount");
    TEST_ASSERT(run.evaluations == 0 && run.outcome.stats.law_evaluations == 0 &&
                    run.outcome.stats.accepted_steps == 0,
                "a zero-Delta segment evaluated a law or took a step");
  }
  return 0;
}

/**
 * @test    A discrete phase's tracer residue on an empty parent passes a segment
 *
 * Row 0, Delta = 3: G = 0 exactly with its tracer Z = 1.48e-19, the residue
 * x -= (x / g) g leaves, and no flux into or out of G; H = 1 drained at 1.3, so
 * the segment takes steps. Expected: success, G and Z bitwise unchanged, at
 * least one accepted step, and H within 10 x the relative tolerance of
 * e^(-1.3 t). A bound on the tracer's level rather than on the growth of its
 * excess would fail this segment, since Theta_G = 0.
 */
static int test_discrete_residue_passes_tracer_bound(void) {
  struct System system;
  one_row_system(&system, 3.0, 2);
  const int gas = add_reservoir(&system, 0, 0.0);
  const int metals = add_tracer(&system, 1, gas, 1.48e-19);
  const int hot = add_reservoir(&system, 0, 1.0);
  add_flux(&system, RATE_FLUX_SINK, hot, RATE_END_EXTERNAL, 0, -1, 1.3);
  const struct RateIntegratorSettings settings = adaptive_settings(1e-8);
  static struct Run run;
  TEST_ASSERT(run_segment(&system, NULL, &settings, &run) == 0, "workspace too small");
  const double error = fabs(system.state[hot] - exp(-1.3 * 3.0));
  printf("  residue Z = %.3g on G = 0: cause %s, %lld accepted steps, |H - exact| = %.3e (bound "
         "1e-7)\n",
         system.state[metals], rate_cause_name(run.cause),
         (long long)run.outcome.stats.accepted_steps, error);
  TEST_ASSERT(run.cause == RATE_CAUSE_NONE, "a residue on an empty parent failed the tracer bound");
  TEST_ASSERT(system.state[gas] == 0.0 && system.state[metals] == 1.48e-19,
              "a parent and tracer with no flux changed");
  TEST_ASSERT(run.outcome.stats.accepted_steps > 0, "the segment took no step");
  TEST_ASSERT(error <= 10.0 * 1e-8, "the drained reservoir misses its closed form");
  return 0;
}

/* Cross-row problem: satellite gas G to the central's H at k_s on the satellite's clock, H
 * drained at k_c on the central's; each carries a tracer. */
#define CROSS_K_SATELLITE 1.7
#define CROSS_K_CENTRAL 0.9
#define CROSS_DELTA_SATELLITE 1.4
#define CROSS_DELTA_CENTRAL 1.25

/**
 * @test    A tracer follows a cross-row transfer on its donor's clock, with mixed Delta
 *
 * Central (row 0, Delta 1.25): H0 = 0.8 with tracer Z_H0 = 0.004, H drained at
 * 0.9. Satellite (row 1, Delta 1.4): G0 = 0.6 with tracer Z_G0 = 0.012, G to the
 * central's H at 1.7 (a to_record: central transfer). Relative tolerance 1e-8.
 * With a = 1.7 x 1.4 and b = 0.9 x 1.25: G = G0 e^(-a), Z_G = Z_G0 e^(-a), H = H0
 * e^(-b) + a G0 (e^(-a) - e^(-b)) / (b - a), and Z_H the same with the tracer's
 * starts. Expected: every entry within 10 x the relative tolerance; the tracer
 * total's sink, the tracer leaving with H's drain, within 10 x the relative
 * tolerance of (Z_G0 + Z_H0) - Z_G - Z_H with no source; the tracer total within
 * tol_F.
 */
static int test_cross_row_tracer_move(void) {
  struct System system;
  one_row_system(&system, CROSS_DELTA_CENTRAL, 2);
  const double h0 = 0.8;
  const double zh0 = 0.004;
  const double g0 = 0.6;
  const double zg0 = 0.012;
  const int hot = add_reservoir(&system, 0, h0);
  const int hot_z = add_tracer(&system, 1, hot, zh0);
  add_row(&system, CROSS_DELTA_SATELLITE);
  const int gas = add_reservoir(&system, 0, g0);
  const int gas_z = add_tracer(&system, 1, gas, zg0);
  add_flux(&system, RATE_FLUX_INTERNAL, gas, hot, 0, -1, CROSS_K_SATELLITE);
  add_flux(&system, RATE_FLUX_SINK, hot, RATE_END_EXTERNAL, 1, -1, CROSS_K_CENTRAL);
  const struct RateIntegratorSettings settings = adaptive_settings(1e-8);
  struct Run run;
  TEST_ASSERT(run_segment(&system, NULL, &settings, &run) == 0, "workspace too small");
  TEST_ASSERT(run.cause == RATE_CAUSE_NONE, "cross-row segment failed");

  const double a = CROSS_K_SATELLITE * CROSS_DELTA_SATELLITE;
  const double b = CROSS_K_CENTRAL * CROSS_DELTA_CENTRAL;
  const double transit = (exp(-a) - exp(-b)) / (b - a);
  const double exact_g = g0 * exp(-a);
  const double exact_zg = zg0 * exp(-a);
  const double exact_h = h0 * exp(-b) + a * g0 * transit;
  const double exact_zh = zh0 * exp(-b) + a * zg0 * transit;
  const double *x = system.state;
  const double error = fmax(fmax(fabs(x[gas] - exact_g), fabs(x[gas_z] - exact_zg)),
                            fmax(fabs(x[hot] - exact_h), fabs(x[hot_z] - exact_zh)));
  const double sink_error = fabs(run.sink[1] - ((zg0 + zh0) - exact_zg - exact_zh));
  const double drift = fabs(((x[gas_z] + x[hot_z]) - (zh0 + zg0)) - (run.source[1] - run.sink[1]));
  /* L = 2: H and Z_H each carry an inflow from the satellite and their drain out. */
  const double tolerance = closure_tolerance(2, run.outcome.stats.accepted_steps, 2, zh0 + zg0,
                                             run.source[1], run.sink[1]);
  printf("  cross-row: error %.3e, |tracer sink - exact| = %.3e (bound 1e-7); drift %.3e "
         "(tol_F %.3e)\n",
         error, sink_error, drift, tolerance);
  TEST_ASSERT(error <= 10.0 * 1e-8, "the cross-row problem misses its closed forms");
  TEST_ASSERT(sink_error <= 10.0 * 1e-8, "the tracer total's sink misses its closed form");
  TEST_ASSERT(run.source[1] == 0.0, "the tracer total gained a source");
  TEST_ASSERT(drift <= tolerance, "the tracer total does not close within tol_F");
  return 0;
}

/**
 * @test    Two tracers of one parent move to the recipient's tracers of their own totals
 *
 * G0 = 1 with metals Z0 = 0.02 (total 1) and dust D0 = 0.005 (total 2); S0 = 0.5
 * with its dust (0.003) listed before its metals (0.01); G to S at 1.3, Delta =
 * 3, relative tolerance 1e-8. Expected: each tracer of G decays as e^(-1.3 t),
 * each tracer of S gains what the same-total tracer of G lost, every entry
 * within 10 x the relative tolerance, and both tracer totals within tol_F with
 * no external amount.
 */
static int test_two_tracers_on_one_parent(void) {
  struct System system;
  one_row_system(&system, 3.0, 3);
  const double z0 = 0.02;
  const double d0 = 0.005;
  const double zs0 = 0.01;
  const double ds0 = 0.003;
  const int gas = add_reservoir(&system, 0, 1.0);
  const int metals = add_tracer(&system, 1, gas, z0);
  const int dust = add_tracer(&system, 2, gas, d0);
  const int stars = add_reservoir(&system, 0, 0.5);
  const int star_dust = add_tracer(&system, 2, stars, ds0);
  const int star_metals = add_tracer(&system, 1, stars, zs0);
  add_flux(&system, RATE_FLUX_INTERNAL, gas, stars, 0, -1, 1.3);
  const struct RateIntegratorSettings settings = adaptive_settings(1e-8);
  struct Run run;
  TEST_ASSERT(run_segment(&system, NULL, &settings, &run) == 0, "workspace too small");
  TEST_ASSERT(run.cause == RATE_CAUSE_NONE, "two-tracer segment failed");

  const double decay = exp(-1.3 * 3.0);
  const double *x = system.state;
  double error = fabs(x[gas] - decay);
  error = fmax(error, fabs(x[stars] - (0.5 + (1.0 - decay))));
  error = fmax(error, fabs(x[metals] - z0 * decay));
  error = fmax(error, fabs(x[dust] - d0 * decay));
  error = fmax(error, fabs(x[star_metals] - (zs0 + z0 * (1.0 - decay))));
  error = fmax(error, fabs(x[star_dust] - (ds0 + d0 * (1.0 - decay))));
  const double metals_drift = fabs((x[metals] + x[star_metals]) - (z0 + zs0));
  const double dust_drift = fabs((x[dust] + x[star_dust]) - (d0 + ds0));
  /* L = 1: every entry carries one term, formation or one of its tracer moves. */
  const int64_t accepted = run.outcome.stats.accepted_steps;
  const double metals_tolerance = closure_tolerance(1, accepted, 2, z0 + zs0, 0.0, 0.0);
  const double dust_tolerance = closure_tolerance(1, accepted, 2, d0 + ds0, 0.0, 0.0);
  printf("  two tracers: error %.3e (bound 1e-7); metals drift %.3e (tol_F %.3e), dust drift "
         "%.3e (tol_F %.3e)\n",
         error, metals_drift, metals_tolerance, dust_drift, dust_tolerance);
  TEST_ASSERT(error <= 10.0 * 1e-8, "the two-tracer problem misses its closed forms");
  TEST_ASSERT(run.source[1] == 0.0 && run.sink[1] == 0.0 && run.source[2] == 0.0 &&
                  run.sink[2] == 0.0,
              "a tracer total gained an external amount");
  TEST_ASSERT(metals_drift <= metals_tolerance && dust_drift <= dust_tolerance,
              "a tracer total does not close within tol_F");
  return 0;
}

/**
 * @test    Derived tracer moves follow canonical order, not the caller's flux order
 *
 * The yield problem with its fluxes listed in reverse. Expected: the end state
 * and the external amounts bit-identical to the declaration-order run.
 */
static int test_tracer_moves_order_independent(void) {
  struct System forward;
  struct System reversed;
  build_yield(&forward, 1.0, 0.01, YIELD_SOURCE, YIELD_FRACTION, 0);
  build_yield(&reversed, 1.0, 0.01, YIELD_SOURCE, YIELD_FRACTION, 1);
  const struct RateIntegratorSettings settings = adaptive_settings(1e-8);
  struct Run forward_run;
  struct Run reversed_run;
  TEST_ASSERT(run_segment(&forward, NULL, &settings, &forward_run) == 0, "workspace too small");
  TEST_ASSERT(run_segment(&reversed, NULL, &settings, &reversed_run) == 0, "workspace too small");
  TEST_ASSERT(forward_run.cause == RATE_CAUSE_NONE && reversed_run.cause == RATE_CAUSE_NONE,
              "yield segment failed");
  TEST_ASSERT(memcmp(forward.state, reversed.state, sizeof(forward.state)) == 0,
              "reversing the flux list changed the end state");
  const size_t written = (size_t)forward.num_totals * sizeof(double);
  TEST_ASSERT(memcmp(forward_run.source, reversed_run.source, written) == 0 &&
                  memcmp(forward_run.sink, reversed_run.sink, written) == 0,
              "reversing the flux list changed the external amounts");
  return 0;
}

/** Integrate a component whose slaved source names another slaved source as its master. */
static void run_slaved_chain(const char *arg) {
  (void)arg;
  static struct System system;
  const struct Yield y = build_yield(&system, 1.0, 0.01, YIELD_SOURCE, YIELD_FRACTION, 0);
  add_flux(&system, RATE_FLUX_SLAVED, RATE_END_EXTERNAL, y.star_z, 3, y.yield, 0.1);
  const struct RateIntegratorSettings settings = adaptive_settings(1e-6);
  struct Run run;
  (void)run_segment(&system, NULL, &settings, &run);
}

/**
 * @test    A slaved chain is refused
 *
 * Slaved sources are one level deep. Expected: a slaved source whose master is
 * another slaved source is fatal, naming the rule.
 */
static int test_slaved_chain_refused(void) {
  TEST_ASSERT(expect_fatal(NULL, run_slaved_chain, "slaved source", "one level deep") == 1,
              "a slaved chain was not refused");
  return 0;
}

int main(void) {
  printf("%s", BLUE);
  printf("============================================================\n");
  printf("Test Suite: Rate Tracers, Slaved Sources and Flux Amounts\n");
  printf("============================================================\n");
  printf("%s\n", NC);

  initialize_error_handling(LOG_LEVEL_WARNING, NULL);

  TEST_RUN(test_dilution_concentration);
  TEST_RUN(test_yield_closed_forms);
  TEST_RUN(test_yield_tracer_total_closes);
  TEST_RUN(test_yield_amount_matches_formed);
  TEST_RUN(test_empty_donor);
  TEST_RUN(test_tracer_bound);
  TEST_RUN(test_flux_amounts_sum_committed_amounts);
  TEST_RUN(test_tracer_without_recipient_tracer_is_a_sink);
  TEST_RUN(test_tracer_weighed_on_parent_total);
  TEST_RUN(test_self_transfer_commits_nothing);
  TEST_RUN(test_zero_delta_tracer_bound);
  TEST_RUN(test_discrete_residue_passes_tracer_bound);
  TEST_RUN(test_cross_row_tracer_move);
  TEST_RUN(test_two_tracers_on_one_parent);
  TEST_RUN(test_tracer_moves_order_independent);
  TEST_RUN(test_slaved_chain_refused);

  TEST_SUMMARY();
  return TEST_RESULT();
}
