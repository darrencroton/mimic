/**
 * @file    test_rate_tracers.c
 * @brief   Tracers, slaved sources and observers in the explicit rate integrator
 *
 * Validates: rate_integrate_segment() on hand-built components with tracer
 * entries, slaved sources and observer entries: tracer dilution by a pristine
 * source, a yield slaved to a formation transfer against its closed forms, the
 * tracer total's closure, the yield-to-formed ratio, an empty donor, the
 * tracer-bound failure, observers as exact sums of committed amounts and their
 * exclusion from the error norm, a tracer leaving its total where the recipient
 * has no tracer, a tracer's error weight on its parent's total, a self-transfer
 * that commits nothing, the tracer bound on a zero-Delta segment, a cross-row
 * tracer move with mixed Delta, two tracers of one parent, canonical order of
 * the derived tracer moves, and the refusal of a slaved chain.
 *
 * Every expected value comes from a closed form written in this file, never
 * from the integrator. Times are in normalised segment time tau, so a row with
 * segment length Delta has evolved for t = Delta at tau = 1. The error of a
 * problem is the largest absolute difference between a computed entry and its
 * closed form at tau = 1. Unless a case says otherwise the absolute tolerance
 * is 1e-3 times the relative tolerance. Each case prints its measured figures
 * beside their bounds.
 */

#include "../framework/test_framework.h"
#include "../framework/child_capture.h"
#include "../../src/core/rate_integrator.h"
#include "../../src/util/error.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Test statistics (required for TEST_RUN macro) */
static int passed = 0;
static int failed = 0;

/* u = 2^-53, the unit roundoff of IEEE binary64. */
#define UNIT_ROUNDOFF 0x1p-53

#define MAX_ENTRIES 16
#define MAX_FLUXES 16
#define MAX_TOTALS 4
#define MAX_ROWS 2
#define LOG_CAPACITY 8192

/* ---------------------------------------------------------------------------
 * Hand-built systems with constant coefficients
 * ------------------------------------------------------------------------- */

/** A component under construction, its state, and the constant coefficient of each flux. */
struct System {
  struct RateEntry entries[MAX_ENTRIES];
  struct RateFlux fluxes[MAX_FLUXES];
  double delta[MAX_ROWS];
  double state[MAX_ENTRIES];
  double coefficient[MAX_FLUXES];
  int num_entries;
  int num_fluxes;
  int num_rows;
  int num_totals;
  int row;             /* the row add_entry() puts new entries on */
  int64_t evaluations; /* law evaluations seen by constant_law() */
};

/** Start a one-row system: row 0 with segment length @p delta. */
static void system_init(struct System *system, double delta, int num_totals) {
  memset(system, 0, sizeof(*system));
  system->delta[0] = delta;
  system->num_rows = 1;
  system->num_totals = num_totals;
}

/** Add a row with segment length @p delta; later entries go on it. */
static int add_row(struct System *system, double delta) {
  const int row = system->num_rows++;
  system->delta[row] = delta;
  system->row = row;
  return row;
}

static int add_entry(struct System *system, enum RateEntryKind kind, int total, int link,
                     double content) {
  const int index = system->num_entries++;
  system->entries[index].row = system->row;
  system->entries[index].total = total;
  system->entries[index].kind = kind;
  system->entries[index].parent = (kind == RATE_ENTRY_TRACER) ? link : -1;
  system->entries[index].flux = (kind == RATE_ENTRY_OBSERVER) ? link : -1;
  system->state[index] = content;
  return index;
}

static int add_reservoir(struct System *system, int total, double content) {
  return add_entry(system, RATE_ENTRY_RESERVOIR, total, -1, content);
}

static int add_tracer(struct System *system, int total, int parent, double content) {
  return add_entry(system, RATE_ENTRY_TRACER, total, parent, content);
}

static int add_observer(struct System *system, int flux, double content) {
  /* An observer's total is not read; total 0 makes any use of it change the run. */
  return add_entry(system, RATE_ENTRY_OBSERVER, 0, flux, content);
}

static int add_flux(struct System *system, enum RateFluxKind kind, int from, int to,
                    int global_index, int master, double coefficient) {
  const int index = system->num_fluxes++;
  system->fluxes[index].kind = kind;
  system->fluxes[index].from = from;
  system->fluxes[index].to = to;
  system->fluxes[index].global_index = global_index;
  system->fluxes[index].master = master;
  system->coefficient[index] = coefficient;
  return index;
}

static struct RateComponent component_of(const struct System *system) {
  const struct RateComponent component = {
      .entries = system->entries,
      .num_entries = system->num_entries,
      .fluxes = system->fluxes,
      .num_fluxes = system->num_fluxes,
      .row_delta = system->delta,
      .num_rows = system->num_rows,
      .num_totals = system->num_totals,
  };
  return component;
}

/** Each flux's coefficient is its constant in System.coefficient. */
static int constant_law(void *data, double tau, const double *state, double *coefficients) {
  struct System *system = data;
  system->evaluations++;
  (void)tau;
  (void)state;
  for (int f = 0; f < system->num_fluxes; f++) {
    coefficients[f] = system->coefficient[f];
  }
  return 0;
}

/* ---------------------------------------------------------------------------
 * Attempt log (RateIntegratorSettings.on_attempt)
 * ------------------------------------------------------------------------- */

/** The attempt sequence, finiteness of every step and weight, and observed flux sums. */
struct AttemptLog {
  int num_entries;
  int64_t count;
  double tau[LOG_CAPACITY];
  double step[LOG_CAPACITY];
  int accepted[LOG_CAPACITY];
  int non_finite;   /* a step, weight, error norm or committed value that was not finite */
  int watched[2];   /* declared flux indices whose committed amounts are summed, or -1 */
  double sum[2];    /* sum of the watched fluxes' committed amounts, in step order */
  int null_amounts; /* an accepted attempt reported no amounts */
};

static void log_attempt(void *data, const struct RateAttempt *attempt) {
  struct AttemptLog *log = data;
  if (log->count < LOG_CAPACITY) {
    log->tau[log->count] = attempt->tau;
    log->step[log->count] = attempt->step;
    log->accepted[log->count] = attempt->accepted;
  }
  log->count++;
  if (!isfinite(attempt->step) || !(attempt->step > 0.0)) {
    log->non_finite = 1;
  }
  if (attempt->error_norm >= 0.0 &&
      (!isfinite(attempt->error_norm) || !isfinite(attempt->min_weight))) {
    log->non_finite = 1;
  }
  if (!attempt->accepted) {
    return;
  }
  for (int i = 0; i < log->num_entries; i++) {
    if (!isfinite(attempt->state[i])) {
      log->non_finite = 1;
    }
  }
  if (attempt->amounts == NULL) {
    log->null_amounts = 1;
    return;
  }
  for (int w = 0; w < 2; w++) {
    if (log->watched[w] >= 0) {
      log->sum[w] += attempt->amounts[log->watched[w]];
    }
  }
}

static void attach_log(struct RateIntegratorSettings *settings, struct AttemptLog *log,
                       const struct System *system) {
  memset(log, 0, sizeof(*log));
  log->num_entries = system->num_entries;
  log->watched[0] = -1;
  log->watched[1] = -1;
  settings->on_attempt = log_attempt;
  settings->attempt_data = log;
}

/** True when two logs hold the same attempt sequence, bit for bit. */
static int same_sequence(const struct AttemptLog *a, const struct AttemptLog *b) {
  if (a->count != b->count || a->count > LOG_CAPACITY) {
    return 0;
  }
  const size_t n = (size_t)a->count;
  return memcmp(a->tau, b->tau, n * sizeof(double)) == 0 &&
         memcmp(a->step, b->step, n * sizeof(double)) == 0 &&
         memcmp(a->accepted, b->accepted, n * sizeof(int)) == 0;
}

/* ---------------------------------------------------------------------------
 * Running a segment
 * ------------------------------------------------------------------------- */

/* The caller-owned workspace every case integrates in. */
static double workspace[1 << 14];

static struct RateIntegratorSettings adaptive_settings(double relative_tolerance) {
  struct RateIntegratorSettings settings;
  rate_integrator_default_settings(&settings);
  settings.relative_tolerance = relative_tolerance;
  settings.absolute_tolerance = 1e-3 * relative_tolerance;
  return settings;
}

struct Run {
  enum RateCause cause;
  struct RateSegmentOutcome outcome;
  double source[MAX_TOTALS];
  double sink[MAX_TOTALS];
};

/** Integrate @p system's state in place; returns 0, or 1 when the workspace is too small. */
static int run_segment(struct System *system, const struct RateIntegratorSettings *settings,
                       struct Run *run) {
  const struct RateComponent component = component_of(system);
  if (rate_integrator_workspace_bytes(&component, settings->scheme) > sizeof(workspace)) {
    return 1;
  }
  const struct RateLaw law = {.evaluate = constant_law, .data = system};
  system->evaluations = 0;
  run->cause = rate_integrate_segment(&component, &law, settings, workspace, sizeof(workspace),
                                      system->state, run->source, run->sink, &run->outcome);
  return 0;
}

/** tol_F = 8u [(L + 2) N_acc + n_F + 2] (sum |x(start)| + Src + Snk). */
static double closure_tolerance(const struct Run *run, int members, double start_magnitude,
                                double source, double sink) {
  const double terms =
      (double)(run->outcome.max_terms_per_entry + 2) * (double)run->outcome.stats.accepted_steps +
      (double)members + 2.0;
  return 8.0 * UNIT_ROUNDOFF * terms * (start_magnitude + source + sink);
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
  system_init(system, YIELD_DELTA, 2);
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
  system_init(&system, 3.0, 2);
  const int gas = add_reservoir(&system, 0, 1.0);
  const int metals = add_tracer(&system, 1, gas, 0.02);
  add_flux(&system, RATE_FLUX_SOURCE, RATE_END_EXTERNAL, gas, 0, -1, 0.5);
  struct RateIntegratorSettings settings;
  rate_integrator_default_settings(&settings);
  struct Run run;
  TEST_ASSERT(run_segment(&system, &settings, &run) == 0, "workspace too small");
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
  TEST_ASSERT(run_segment(&system, &settings, &run) == 0, "workspace too small");
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
  TEST_ASSERT(run_segment(&system, &settings, &run) == 0, "workspace too small");
  TEST_ASSERT(run.cause == RATE_CAUSE_NONE, "yield segment failed");

  const double end = system.state[y.metals] + system.state[y.star_z];
  const double drift = fabs((end - start) - (run.source[1] - run.sink[1]));
  const double tolerance = closure_tolerance(&run, 2, start_magnitude, run.source[1], run.sink[1]);
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
 * The formed amount is an observer on formation; the yield amount is the tracer
 * total's source. Expected: |yield - 0.02 formed| <= 8u N_acc (0.02 formed).
 */
static int test_yield_amount_matches_formed(void) {
  struct System system;
  const struct Yield y = build_yield(&system, 1.0, 0.01, YIELD_SOURCE, YIELD_FRACTION, 0);
  const int formed = add_observer(&system, y.formation, 0.0);
  const struct RateIntegratorSettings settings = adaptive_settings(1e-8);
  struct Run run;
  TEST_ASSERT(run_segment(&system, &settings, &run) == 0, "workspace too small");
  TEST_ASSERT(run.cause == RATE_CAUSE_NONE, "yield segment failed");

  const double expected = YIELD_FRACTION * system.state[formed];
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
 * weight, error norm, external amount and observer finite, and every entry
 * within 10 x the relative tolerance of its closed form.
 */
static int test_empty_donor(void) {
  struct System system;
  const struct Yield y = build_yield(&system, 0.0, 0.0, YIELD_SOURCE, YIELD_FRACTION, 0);
  const int formed = add_observer(&system, y.formation, 0.0);
  struct RateIntegratorSettings settings = adaptive_settings(1e-8);
  struct AttemptLog log;
  attach_log(&settings, &log, &system);
  struct Run run;
  TEST_ASSERT(run_segment(&system, &settings, &run) == 0, "workspace too small");
  TEST_ASSERT(run.cause == RATE_CAUSE_NONE, "empty-donor segment failed");

  int finite = !log.non_finite;
  for (int i = 0; i < system.num_entries; i++) {
    finite = finite && isfinite(system.state[i]);
  }
  for (int t = 0; t < system.num_totals; t++) {
    finite = finite && isfinite(run.source[t]) && isfinite(run.sink[t]);
  }
  const struct YieldExact exact = yield_exact(0.0, 0.0, YIELD_SOURCE);
  const double error = yield_error(&system, &y, &exact);
  printf("  empty donor: error %.3e (bound 1e-7), formed %.17g, %lld attempts, all finite: %s\n",
         error, system.state[formed], (long long)log.count, finite ? "yes" : "no");
  TEST_ASSERT(finite, "an empty donor produced a value that is not finite");
  TEST_ASSERT(error <= 10.0 * 1e-8, "empty-donor problem misses its closed forms");
  return 0;
}

/**
 * @test    A tracer pushed above its parent fails with tracer_bound
 *
 * The yield problem with a yield of 0.5 and no source: Z - G = e^(-kt) (Z0 - G0
 * + 0.5 k t) > 0 at t = 3. Expected: cause tracer_bound naming Z, its total and
 * the yield, a detail saying the tracer has sources of its own, and the state
 * untouched. A tracer started above its parent with no source of its own fails
 * the same way with a detail saying it has none.
 */
static int test_tracer_bound(void) {
  struct System system;
  const struct Yield y = build_yield(&system, 1.0, 0.01, 0.0, 0.5, 0);
  double before[MAX_ENTRIES];
  memcpy(before, system.state, sizeof(before));
  const struct RateIntegratorSettings settings = adaptive_settings(1e-8);
  struct Run run;
  TEST_ASSERT(run_segment(&system, &settings, &run) == 0, "workspace too small");
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

  system_init(&system, YIELD_DELTA, 2);
  const int gas = add_reservoir(&system, 0, 1.0);
  const int stars = add_reservoir(&system, 0, 0.0);
  const int metals = add_tracer(&system, 1, gas, 2.0);
  add_tracer(&system, 1, stars, 0.0);
  add_flux(&system, RATE_FLUX_INTERNAL, gas, stars, 0, -1, YIELD_FORMATION);
  TEST_ASSERT(run_segment(&system, &settings, &run) == 0, "workspace too small");
  printf("  tracer above parent: cause %s, entry %d, flux %d, detail \"%s\"\n",
         rate_cause_name(run.cause), failure->entry, failure->flux, failure->detail);
  TEST_ASSERT(run.cause == RATE_CAUSE_TRACER_BOUND, "a tracer above its parent passed");
  TEST_ASSERT(failure->entry == metals && failure->flux == -1,
              "the failure does not name the tracer alone");
  TEST_ASSERT(strstr(failure->detail, "has no sources of its own") != NULL &&
                  strstr(failure->detail, "has sources of its own") == NULL,
              "the detail does not say the tracer has no sources of its own");
  return 0;
}

/**
 * @test    An observer is the sum of its flux's committed amounts, bit for bit
 *
 * The yield problem with observers, started at zero, on formation and on the
 * yield. Expected: each observer equals the sum, in step order, of the committed
 * amounts the attempt reports give for its flux.
 */
static int test_observer_sums_committed_amounts(void) {
  struct System system;
  const struct Yield y = build_yield(&system, 1.0, 0.01, YIELD_SOURCE, YIELD_FRACTION, 0);
  const int formed = add_observer(&system, y.formation, 0.0);
  const int yielded = add_observer(&system, y.yield, 0.0);
  struct RateIntegratorSettings settings = adaptive_settings(1e-8);
  struct AttemptLog log;
  attach_log(&settings, &log, &system);
  log.watched[0] = y.formation;
  log.watched[1] = y.yield;
  struct Run run;
  TEST_ASSERT(run_segment(&system, &settings, &run) == 0, "workspace too small");
  TEST_ASSERT(run.cause == RATE_CAUSE_NONE, "yield segment failed");
  TEST_ASSERT(!log.null_amounts, "an accepted attempt reported no committed amounts");

  printf("  formed %.17g, sum of committed amounts %.17g; yielded %.17g, sum %.17g\n",
         system.state[formed], log.sum[0], system.state[yielded], log.sum[1]);
  TEST_ASSERT(memcmp(&system.state[formed], &log.sum[0], sizeof(double)) == 0,
              "the formation observer differs from the sum of its committed amounts");
  TEST_ASSERT(memcmp(&system.state[yielded], &log.sum[1], sizeof(double)) == 0,
              "the yield observer differs from the sum of its committed amounts");
  return 0;
}

/**
 * @test    An observer is left out of the error norm
 *
 * The yield problem at relative tolerance 1e-8, once without an observer and
 * once with an observer on formation started at 5 (total 0, which the
 * integrator must not read). Expected: bit-identical attempt sequences and end
 * states of every shared entry.
 */
static int test_observer_excluded_from_norm(void) {
  struct System bare;
  struct System observed;
  build_yield(&bare, 1.0, 0.01, YIELD_SOURCE, YIELD_FRACTION, 0);
  const struct Yield y = build_yield(&observed, 1.0, 0.01, YIELD_SOURCE, YIELD_FRACTION, 0);
  add_observer(&observed, y.formation, 5.0);
  static struct AttemptLog bare_log;
  static struct AttemptLog observed_log;
  struct RateIntegratorSettings bare_settings = adaptive_settings(1e-8);
  struct RateIntegratorSettings observed_settings = adaptive_settings(1e-8);
  attach_log(&bare_settings, &bare_log, &bare);
  attach_log(&observed_settings, &observed_log, &observed);
  struct Run bare_run;
  struct Run observed_run;
  TEST_ASSERT(run_segment(&bare, &bare_settings, &bare_run) == 0, "workspace too small");
  TEST_ASSERT(run_segment(&observed, &observed_settings, &observed_run) == 0,
              "workspace too small");
  TEST_ASSERT(bare_run.cause == RATE_CAUSE_NONE && observed_run.cause == RATE_CAUSE_NONE,
              "yield segment failed");

  printf("  without observer: %lld attempts; with observer: %lld attempts\n",
         (long long)bare_log.count, (long long)observed_log.count);
  TEST_ASSERT(same_sequence(&bare_log, &observed_log), "an observer changed the step sequence");
  TEST_ASSERT(memcmp(bare.state, observed.state, (size_t)bare.num_entries * sizeof(double)) == 0,
              "an observer changed the end state of the other entries");
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
  system_init(&system, 3.0, 2);
  const int gas = add_reservoir(&system, 0, 1.0);
  const int stars = add_reservoir(&system, 0, 0.0);
  const int metals = add_tracer(&system, 1, gas, 0.05);
  add_flux(&system, RATE_FLUX_INTERNAL, gas, stars, 0, -1, 1.3);
  add_flux(&system, RATE_FLUX_SINK, gas, RATE_END_EXTERNAL, 1, -1, 0.4);
  const struct RateIntegratorSettings settings = adaptive_settings(1e-8);
  struct Run run;
  TEST_ASSERT(run_segment(&system, &settings, &run) == 0, "workspace too small");
  TEST_ASSERT(run.cause == RATE_CAUSE_NONE, "segment failed");

  const double decay = exp(-1.7 * 3.0);
  const double metals_error = fabs(system.state[metals] - 0.05 * decay);
  const double sink_error = fabs(run.sink[1] - 0.05 * (1.0 - decay));
  const double drift = fabs((system.state[metals] - 0.05) - (run.source[1] - run.sink[1]));
  const double tolerance = closure_tolerance(&run, 1, 0.05, run.source[1], run.sink[1]);
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
  system_init(system, 3.0, 2);
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
  TEST_ASSERT(run_segment(&bare, &bare_settings, &bare_run) == 0, "workspace too small");
  TEST_ASSERT(run_segment(&traced, &traced_settings, &traced_run) == 0, "workspace too small");
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
 * @test    A self-transfer commits nothing: its observer, its reported amounts and its yield
 *
 * G0 = 1 with tracer Z0 = 0.01, S with tracer Z_S; formation G to S at 1.3, a
 * transfer G to G at 0.7 (both ends one entry), and a yield of 0.02 slaved to
 * the self-transfer into Z; Delta = 3, relative tolerance 1e-8. Observers on
 * the self-transfer and on formation start at zero. Expected: the self-transfer
 * observer and the sum of its reported committed amounts are both exactly 0,
 * the formation observer equals the sum of its reported amounts bit for bit,
 * the tracer total gains no source, and Z = Z0 e^(-1.3 t) within 10 x the
 * relative tolerance.
 */
static int test_self_transfer_commits_nothing(void) {
  struct System system;
  system_init(&system, 3.0, 2);
  const int gas = add_reservoir(&system, 0, 1.0);
  const int stars = add_reservoir(&system, 0, 0.0);
  const int metals = add_tracer(&system, 1, gas, 0.01);
  add_tracer(&system, 1, stars, 0.0);
  const int formation = add_flux(&system, RATE_FLUX_INTERNAL, gas, stars, 0, -1, 1.3);
  const int self = add_flux(&system, RATE_FLUX_INTERNAL, gas, gas, 1, -1, 0.7);
  add_flux(&system, RATE_FLUX_SLAVED, RATE_END_EXTERNAL, metals, 2, self, 0.02);
  const int self_observer = add_observer(&system, self, 0.0);
  const int formed = add_observer(&system, formation, 0.0);
  struct RateIntegratorSettings settings = adaptive_settings(1e-8);
  struct AttemptLog log;
  attach_log(&settings, &log, &system);
  log.watched[0] = self;
  log.watched[1] = formation;
  struct Run run;
  TEST_ASSERT(run_segment(&system, &settings, &run) == 0, "workspace too small");
  TEST_ASSERT(run.cause == RATE_CAUSE_NONE, "self-transfer segment failed");

  const double zero = 0.0;
  const double metals_error = fabs(system.state[metals] - 0.01 * exp(-1.3 * 3.0));
  printf("  self-transfer observer %.17g, sum of its reported amounts %.17g; formed %.17g, "
         "sum %.17g; tracer source %.3g; |Z - exact| = %.3e (bound 1e-7)\n",
         system.state[self_observer], log.sum[0], system.state[formed], log.sum[1], run.source[1],
         metals_error);
  TEST_ASSERT(memcmp(&system.state[self_observer], &zero, sizeof(double)) == 0,
              "the self-transfer observer is not exactly zero");
  TEST_ASSERT(memcmp(&log.sum[0], &zero, sizeof(double)) == 0,
              "the self-transfer's reported committed amounts are not exactly zero");
  TEST_ASSERT(memcmp(&system.state[formed], &log.sum[1], sizeof(double)) == 0,
              "the formation observer differs from the sum of its committed amounts");
  TEST_ASSERT(run.source[1] == 0.0, "a yield slaved to a self-transfer moved content");
  TEST_ASSERT(metals_error <= 10.0 * 1e-8, "the tracer misses its closed form");
  return 0;
}

/** G0 = 1 to S at 1.3 with tracers Z (start @p z0) and Z_S, on one row of Delta = 0. */
static void build_zero_delta(struct System *system, double z0) {
  system_init(system, 0.0, 2);
  const int gas = add_reservoir(system, 0, 1.0);
  const int stars = add_reservoir(system, 0, 0.0);
  add_tracer(system, 1, gas, z0);
  add_tracer(system, 1, stars, 0.0);
  add_flux(system, RATE_FLUX_INTERNAL, gas, stars, 0, -1, 1.3);
}

/**
 * @test    A segment with every Delta zero still holds each tracer to its parent
 *
 * The segment takes no step and evaluates no law, so N_acc = 0 and the bound is
 * zero. Expected: Z0 = 0.5 and Z0 = 1 (equal to its parent) succeed with the
 * state bitwise unchanged and zero external amounts; Z0 = 1.5 fails with
 * tracer_bound naming Z with no flux, leaving the state and the caller's
 * external-amount arrays bitwise untouched. No case evaluates a law.
 */
static int test_zero_delta_tracer_bound(void) {
  const double within[] = {0.5, 1.0};
  struct System system;
  struct Run run;
  const struct RateIntegratorSettings settings = adaptive_settings(1e-8);
  for (int c = 0; c < 2; c++) {
    build_zero_delta(&system, within[c]);
    double before[MAX_ENTRIES];
    memcpy(before, system.state, sizeof(before));
    TEST_ASSERT(run_segment(&system, &settings, &run) == 0, "workspace too small");
    printf("  Delta = 0, Z0 = %.2f: cause %s, %lld law evaluations, %lld steps\n", within[c],
           rate_cause_name(run.cause), (long long)system.evaluations,
           (long long)run.outcome.stats.accepted_steps);
    TEST_ASSERT(run.cause == RATE_CAUSE_NONE, "a tracer within its bound failed at Delta = 0");
    TEST_ASSERT(memcmp(before, system.state, sizeof(before)) == 0,
                "a zero-Delta segment changed the state");
    TEST_ASSERT(run.source[1] == 0.0 && run.sink[1] == 0.0,
                "a zero-Delta segment reported an external amount");
    TEST_ASSERT(system.evaluations == 0 && run.outcome.stats.law_evaluations == 0 &&
                    run.outcome.stats.accepted_steps == 0,
                "a zero-Delta segment evaluated a law or took a step");
  }

  build_zero_delta(&system, 1.5);
  double before[MAX_ENTRIES];
  memcpy(before, system.state, sizeof(before));
  for (int t = 0; t < MAX_TOTALS; t++) {
    run.source[t] = 42.0;
    run.sink[t] = 42.0;
  }
  TEST_ASSERT(run_segment(&system, &settings, &run) == 0, "workspace too small");
  const struct RateFailure *failure = &run.outcome.failure;
  printf("  Delta = 0, Z0 = 1.50: cause %s, entry %d, flux %d, %lld law evaluations\n",
         rate_cause_name(run.cause), failure->entry, failure->flux, (long long)system.evaluations);
  TEST_ASSERT(run.cause == RATE_CAUSE_TRACER_BOUND,
              "a tracer above its parent passed a zero-Delta segment");
  TEST_ASSERT(failure->entry == 2 && failure->flux == -1 && failure->total == 1,
              "the failure does not name the tracer and its total alone");
  TEST_ASSERT(memcmp(before, system.state, sizeof(before)) == 0,
              "a zero-Delta tracer_bound failure changed the state");
  TEST_ASSERT(run.source[0] == 42.0 && run.source[1] == 42.0 && run.sink[0] == 42.0 &&
                  run.sink[1] == 42.0,
              "a zero-Delta tracer_bound failure wrote the external amounts");
  TEST_ASSERT(system.evaluations == 0 && run.outcome.stats.law_evaluations == 0,
              "a zero-Delta segment evaluated a law");
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
  system_init(&system, CROSS_DELTA_CENTRAL, 2);
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
  TEST_ASSERT(run_segment(&system, &settings, &run) == 0, "workspace too small");
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
  const double tolerance = closure_tolerance(&run, 2, zh0 + zg0, run.source[1], run.sink[1]);
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
  system_init(&system, 3.0, 3);
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
  TEST_ASSERT(run_segment(&system, &settings, &run) == 0, "workspace too small");
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
  const double metals_tolerance = closure_tolerance(&run, 2, z0 + zs0, 0.0, 0.0);
  const double dust_tolerance = closure_tolerance(&run, 2, d0 + ds0, 0.0, 0.0);
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
  TEST_ASSERT(run_segment(&forward, &settings, &forward_run) == 0, "workspace too small");
  TEST_ASSERT(run_segment(&reversed, &settings, &reversed_run) == 0, "workspace too small");
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
  (void)run_segment(&system, &settings, &run);
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
  printf("Test Suite: Rate Tracers, Slaved Sources and Observers\n");
  printf("============================================================\n");
  printf("%s\n", NC);

  initialize_error_handling(LOG_LEVEL_WARNING, NULL);

  TEST_RUN(test_dilution_concentration);
  TEST_RUN(test_yield_closed_forms);
  TEST_RUN(test_yield_tracer_total_closes);
  TEST_RUN(test_yield_amount_matches_formed);
  TEST_RUN(test_empty_donor);
  TEST_RUN(test_tracer_bound);
  TEST_RUN(test_observer_sums_committed_amounts);
  TEST_RUN(test_observer_excluded_from_norm);
  TEST_RUN(test_tracer_without_recipient_tracer_is_a_sink);
  TEST_RUN(test_tracer_weighed_on_parent_total);
  TEST_RUN(test_self_transfer_commits_nothing);
  TEST_RUN(test_zero_delta_tracer_bound);
  TEST_RUN(test_cross_row_tracer_move);
  TEST_RUN(test_two_tracers_on_one_parent);
  TEST_RUN(test_tracer_moves_order_independent);
  TEST_RUN(test_slaved_chain_refused);

  TEST_SUMMARY();
  return TEST_RESULT();
}
