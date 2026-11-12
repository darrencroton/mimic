/**
 * @file    test_rate_integrator.c
 * @brief   Proving problems for the explicit rate integrator and its segment closure
 *
 * Validates: rate_integrate_segment() and rate_check_closure() on hand-built
 * components: measured convergence orders and tolerance ratios, conservation
 * within tol_F, positivity, canonical-order independence, mixed and zero row
 * Delta values, the first-step and empty-state rules, repeatability, and every
 * named failure with the caller's state left untouched.
 *
 * Every expected value comes from a closed form written in this file, never
 * from the integrator. Every system's values are literals here; the star's
 * random satellites come from the fixed-seed SplitMix64 generator below. Times
 * are in normalised segment time tau, so a row with segment length Delta has
 * evolved for t = Delta at tau = 1. The error of a problem is the largest
 * absolute difference between a computed entry and its closed form at tau = 1.
 * Unless a case says otherwise the absolute tolerance is 1e-3 times the
 * relative tolerance. Each case prints the measured figures beside its bound.
 */

#include "../framework/test_framework.h"
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

#define MAX_ENTRIES 96
#define MAX_FLUXES 96
#define MAX_ROWS 40
#define MAX_TOTALS 4
#define LOG_CAPACITY 8192

/* ---------------------------------------------------------------------------
 * Hand-built systems
 * ------------------------------------------------------------------------- */

/** A component under construction, its state, and the constant coefficient of each flux. */
struct System {
  struct RateEntry entries[MAX_ENTRIES];
  struct RateFlux fluxes[MAX_FLUXES];
  double delta[MAX_ROWS];
  double state[MAX_ENTRIES];
  double coefficient[MAX_FLUXES]; /* used by constant_law() */
  int num_entries;
  int num_fluxes;
  int num_rows;
  int num_totals;
};

static void system_init(struct System *system, int num_rows, int num_totals) {
  memset(system, 0, sizeof(*system));
  system->num_rows = num_rows;
  system->num_totals = num_totals;
}

static int add_entry(struct System *system, int row, int total, double content) {
  const int index = system->num_entries++;
  system->entries[index].row = row;
  system->entries[index].total = total;
  system->state[index] = content;
  return index;
}

static int add_flux(struct System *system, enum RateFluxKind kind, int from, int to,
                    int global_index, double coefficient) {
  const int index = system->num_fluxes++;
  system->fluxes[index].kind = kind;
  system->fluxes[index].from = from;
  system->fluxes[index].to = to;
  system->fluxes[index].global_index = global_index;
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

/* ---------------------------------------------------------------------------
 * Laws, with a probe that sees every state the integrator evaluates them at
 * ------------------------------------------------------------------------- */

struct LawProbe;
typedef int (*LawFill)(const struct LawProbe *probe, double tau, const double *x, double *k);

/**
 * K_max bookkeeping, for a law whose K_max at a state the test can write down
 * (k_of_state): the K at the latest stage-1 evaluation (the first evaluation
 * after an accepted step, or of the segment) and the largest K at the other
 * evaluations since the last attempt report. The adaptive first step's probe,
 * the segment's second evaluation when the start has a non-zero weight, is no
 * stage of any attempt and is skipped through skip_evaluation.
 */
struct LawProbe {
  const struct System *system;
  LawFill fill;
  double bad_value;                      /* the faulty laws' coefficient from tau = 0.5 on */
  int64_t evaluations;                   /* law evaluations seen */
  double min_state;                      /* smallest entry any law was evaluated at */
  double (*k_of_state)(const double *x); /* NULL: no K bookkeeping */
  int64_t skip_evaluation;               /* 1-based evaluation to leave out, or 0 */
  int expect_stage1;
  double k_stage1;
  double k_attempt;
};

static int probed_law(void *data, double tau, const double *state, double *coefficients) {
  struct LawProbe *probe = data;
  probe->evaluations++;
  for (int i = 0; i < probe->system->num_entries; i++) {
    if (state[i] < probe->min_state) {
      probe->min_state = state[i];
    }
  }
  if (probe->k_of_state != NULL && probe->evaluations != probe->skip_evaluation) {
    const double k = probe->k_of_state(state);
    if (probe->expect_stage1) {
      probe->k_stage1 = k;
      probe->expect_stage1 = 0;
    } else if (k > probe->k_attempt) {
      probe->k_attempt = k;
    }
  }
  return probe->fill(probe, tau, state, coefficients);
}

/** Each flux's coefficient is its constant in System.coefficient. */
static int constant_law(const struct LawProbe *probe, double tau, const double *x, double *k) {
  (void)tau;
  (void)x;
  for (int f = 0; f < probe->system->num_fluxes; f++) {
    k[f] = probe->system->coefficient[f];
  }
  return 0;
}

/** Closed cycle: A to B at 1 + 0.5 B, B to C at 2 / (1 + C), C to A at 0.7 + 0.3 A^2. */
static int cycle_law(const struct LawProbe *probe, double tau, const double *x, double *k) {
  (void)probe;
  (void)tau;
  k[0] = 1.0 + 0.5 * x[1];
  k[1] = 2.0 / (1.0 + x[2]);
  k[2] = 0.7 + 0.3 * x[0] * x[0];
  return 0;
}

/** Bimolecular: A to C at 2 B, B to C at 2 A. */
static int bimolecular_law(const struct LawProbe *probe, double tau, const double *x, double *k) {
  (void)probe;
  (void)tau;
  k[0] = 2.0 * x[1];
  k[1] = 2.0 * x[0];
  return 0;
}

/** Threshold: a source of 1 into G; G to S at 3 max(0, G - 1) / G. */
static int threshold_law(const struct LawProbe *probe, double tau, const double *x, double *k) {
  (void)probe;
  (void)tau;
  k[0] = 1.0;
  k[1] = (x[0] > 1.0) ? 3.0 * (x[0] - 1.0) / x[0] : 0.0;
  return 0;
}

/** A sink at c / x with c = 1: a constant outflow that empties x in finite time. */
static int inverse_law(const struct LawProbe *probe, double tau, const double *x, double *k) {
  (void)probe;
  (void)tau;
  k[0] = 1.0 / x[0];
  return 0;
}

/** A source of 1, and a sink at 10 / (x - 0.5) above 0.5 and zero at or below it. */
static int divergent_law(const struct LawProbe *probe, double tau, const double *x, double *k) {
  (void)probe;
  (void)tau;
  k[0] = 1.0;
  k[1] = (x[0] > 0.5) ? 10.0 / (x[0] - 0.5) : 0.0;
  return 0;
}

/** K_max of the coupled sink (Delta = 2): only A has an outflow, at 1 + 50 B. */
static double coupled_sink_k(const double *x) {
  return (x[0] != 0.0) ? 2.0 * (1.0 + 50.0 * x[1]) : 0.0;
}

/** A source of 10 into B, and a sink out of A at 1 + 50 B. */
static int coupled_sink_law(const struct LawProbe *probe, double tau, const double *x, double *k) {
  (void)probe;
  (void)tau;
  k[0] = 10.0;
  k[1] = 1.0 + 50.0 * x[1];
  return 0;
}

/** Constant coefficients until tau = 0.5, then flux 1 returns the probe's bad value. */
static int faulty_law(const struct LawProbe *probe, double tau, const double *x, double *k) {
  constant_law(probe, tau, x, k);
  if (tau >= 0.5) {
    k[1] = probe->bad_value;
  }
  return 0;
}

/** Constant coefficients until tau = 0.5, then the law reports failure. */
static int failing_law(const struct LawProbe *probe, double tau, const double *x, double *k) {
  constant_law(probe, tau, x, k);
  return (tau >= 0.5) ? 1 : 0;
}

static struct LawProbe law_probe(LawFill fill) {
  const struct LawProbe probe = {
      .system = NULL, .fill = fill, .bad_value = 0.0, .k_of_state = NULL, .expect_stage1 = 1};
  return probe;
}

/* ---------------------------------------------------------------------------
 * Attempt log (RateIntegratorSettings.on_attempt)
 * ------------------------------------------------------------------------- */

struct AttemptLog {
  int num_entries;
  int64_t count;
  double tau[LOG_CAPACITY];
  double step[LOG_CAPACITY];
  int accepted[LOG_CAPACITY];
  double min_committed;     /* smallest entry of any committed state */
  int bad_step;             /* a step that was not finite and positive */
  int64_t weighed;          /* attempts whose error estimate ran */
  int bad_weight;           /* an estimate with a non-finite or non-positive smallest weight */
  int non_finite_error;     /* an estimate with a non-finite error norm */
  int previous_rejected;    /* the previous attempt was rejected */
  double post_rejection;    /* an accepted step that followed a rejection, else 0 */
  int grew_after_rejection; /* the attempt after it was longer than the landing stretch allows */
  struct LawProbe *probe;   /* K bookkeeping source, or NULL */
  double worst_k_product;   /* largest step * K_max over the stages of accepted steps */
};

/** Track step * K_max over every stage of accepted attempts (LawProbe.k_of_state). */
static void check_k_product(struct AttemptLog *log, const struct RateAttempt *attempt) {
  struct LawProbe *probe = log->probe;
  if (probe == NULL || probe->k_of_state == NULL) {
    return;
  }
  if (attempt->accepted) {
    const double product = attempt->step * fmax(probe->k_stage1, probe->k_attempt);
    if (product > log->worst_k_product) {
      log->worst_k_product = product;
    }
    probe->expect_stage1 = 1;
  }
  probe->k_attempt = 0.0;
}

static void log_attempt(void *data, const struct RateAttempt *attempt) {
  struct AttemptLog *log = data;
  if (log->count < LOG_CAPACITY) {
    log->tau[log->count] = attempt->tau;
    log->step[log->count] = attempt->step;
    log->accepted[log->count] = attempt->accepted;
  }
  log->count++;
  check_k_product(log, attempt);
  /* No growth on the step after a rejection: only the 10% landing stretch may lengthen it. */
  if (log->post_rejection > 0.0 && attempt->step > 1.1 * log->post_rejection) {
    log->grew_after_rejection = 1;
  }
  log->post_rejection = (attempt->accepted && log->previous_rejected) ? attempt->step : 0.0;
  log->previous_rejected = !attempt->accepted;
  if (!isfinite(attempt->step) || !(attempt->step > 0.0)) {
    log->bad_step = 1;
  }
  if (attempt->accepted) {
    for (int i = 0; i < log->num_entries; i++) {
      if (attempt->state[i] < log->min_committed) {
        log->min_committed = attempt->state[i];
      }
    }
  }
  if (attempt->error_norm >= 0.0) {
    log->weighed++;
    if (!isfinite(attempt->min_weight) || !(attempt->min_weight > 0.0)) {
      log->bad_weight = 1;
    }
    if (!isfinite(attempt->error_norm)) {
      log->non_finite_error = 1;
    }
  }
}

static void attach_log(struct RateIntegratorSettings *settings, struct AttemptLog *log,
                       const struct System *system) {
  memset(log, 0, sizeof(*log));
  log->num_entries = system->num_entries;
  log->min_committed = INFINITY;
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
static double workspace[1 << 15];

static struct RateIntegratorSettings adaptive_settings(double relative_tolerance) {
  struct RateIntegratorSettings settings;
  rate_integrator_default_settings(&settings);
  settings.relative_tolerance = relative_tolerance;
  settings.absolute_tolerance = 1e-3 * relative_tolerance;
  return settings;
}

static struct RateIntegratorSettings fixed_settings(int64_t steps) {
  struct RateIntegratorSettings settings;
  rate_integrator_default_settings(&settings);
  settings.fixed_steps = steps;
  return settings;
}

struct Run {
  enum RateCause cause;
  struct RateSegmentOutcome outcome;
  double source[MAX_TOTALS];
  double sink[MAX_TOTALS];
};

/** Integrate @p system's state in place; returns 0, or 1 when the workspace is too small. */
static int run_segment(struct System *system, struct LawProbe *probe,
                       const struct RateIntegratorSettings *settings, struct Run *run) {
  const struct RateComponent component = component_of(system);
  if (rate_integrator_workspace_bytes(&component) > sizeof(workspace)) {
    return 1;
  }
  const struct RateLaw law = {.evaluate = probed_law, .data = probe};
  probe->system = system;
  probe->evaluations = 0;
  probe->min_state = INFINITY;
  probe->expect_stage1 = 1;
  probe->k_stage1 = 0.0;
  probe->k_attempt = 0.0;
  run->cause = rate_integrate_segment(&component, &law, settings, workspace, sizeof(workspace),
                                      system->state, run->source, run->sink, &run->outcome);
  return 0;
}

/** tol_F = 8u [(L + 2) N_acc + n_F + 2] (sum |x(start)| + Src + Snk). */
static double closure_tolerance(int max_terms, int64_t accepted, int members,
                                double start_magnitude, double source, double sink) {
  const double terms = (double)(max_terms + 2) * (double)accepted + (double)members + 2.0;
  return 8.0 * UNIT_ROUNDOFF * terms * (start_magnitude + source + sink);
}

static double max3(double a, double b, double c) { return fmax(a, fmax(b, c)); }

/* ---------------------------------------------------------------------------
 * Problem builders and closed forms
 * ------------------------------------------------------------------------- */

/** Linear chain A to B to C at k1 = 1, k2 = 0.3, start (1, 0, 0). */
static void build_chain(struct System *system, double delta) {
  system_init(system, 1, 1);
  system->delta[0] = delta;
  const int a = add_entry(system, 0, 0, 1.0);
  const int b = add_entry(system, 0, 0, 0.0);
  const int c = add_entry(system, 0, 0, 0.0);
  add_flux(system, RATE_FLUX_INTERNAL, a, b, 0, 1.0);
  add_flux(system, RATE_FLUX_INTERNAL, b, c, 1, 0.3);
}

static double chain_error(const double *x, double t) {
  const double k1 = 1.0;
  const double k2 = 0.3;
  const double a = exp(-k1 * t);
  const double b = k1 / (k2 - k1) * (exp(-k1 * t) - exp(-k2 * t));
  const double c = 1.0 - a - b;
  return max3(fabs(x[0] - a), fabs(x[1] - b), fabs(x[2] - c));
}

/** Bimolecular A to C at 2 B, B to C at 2 A, start (1, 0.6, 0), Delta = 3. */
static void build_bimolecular(struct System *system) {
  system_init(system, 1, 1);
  system->delta[0] = 3.0;
  const int a = add_entry(system, 0, 0, 1.0);
  const int b = add_entry(system, 0, 0, 0.6);
  const int c = add_entry(system, 0, 0, 0.0);
  add_flux(system, RATE_FLUX_INTERNAL, a, c, 0, 0.0);
  add_flux(system, RATE_FLUX_INTERNAL, b, c, 1, 0.0);
}

static double bimolecular_error(const double *x) {
  const double kappa = 2.0;
  const double a0 = 1.0;
  const double b0 = 0.6;
  const double t = 3.0;
  const double d = a0 - b0;
  const double b = d * b0 / (a0 * exp(kappa * d * t) - b0);
  const double a = b + d;
  const double c = (a0 + b0) - a - b;
  return max3(fabs(x[0] - a), fabs(x[1] - b), fabs(x[2] - c));
}

/** Threshold: G (start g0) fed by a source of 1, G to S above G = 1; Delta = 2. */
static void build_threshold(struct System *system, double g0) {
  system_init(system, 1, 1);
  system->delta[0] = 2.0;
  const int g = add_entry(system, 0, 0, g0);
  const int s = add_entry(system, 0, 0, 0.0);
  add_flux(system, RATE_FLUX_SOURCE, RATE_END_EXTERNAL, g, 0, 0.0);
  add_flux(system, RATE_FLUX_INTERNAL, g, s, 1, 0.0);
}

/**
 * Threshold closed form at tau = 1: G rises at 1 per unit time to the crossing
 * t* = 1 - g0, then G - 1 = (1 - e^(-3 t'))/3 and S = t' - (1 - e^(-3 t'))/3,
 * with t' = 2 - t*.
 */
static double threshold_error(const double *x, double g0) {
  const double t_cross = 1.0 - g0;
  const double t_after = 2.0 - t_cross;
  const double relaxed = (1.0 - exp(-3.0 * t_after)) / 3.0;
  const double g = 1.0 + relaxed;
  const double s = t_after - relaxed;
  return fmax(fabs(x[0] - g), fabs(x[1] - s));
}

/** Stiff pair: A and B exchanging at k, B drained at 1, Delta = 3, A0 = B0 = 0.5. */
static void build_stiff_pair(struct System *system, double k) {
  system_init(system, 1, 1);
  system->delta[0] = 3.0;
  const int a = add_entry(system, 0, 0, 0.5);
  const int b = add_entry(system, 0, 0, 0.5);
  add_flux(system, RATE_FLUX_INTERNAL, a, b, 0, k);
  add_flux(system, RATE_FLUX_INTERNAL, b, a, 1, k);
  add_flux(system, RATE_FLUX_SINK, b, RATE_END_EXTERNAL, 2, 1.0);
}

/** SplitMix64: the fixed-seed generator of the star's satellites. */
static double next_uniform(uint64_t *seed) {
  uint64_t z = (*seed += 0x9E3779B97F4A7C15ull);
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
  z ^= z >> 31;
  return (double)(z >> 11) * 0x1p-53;
}

static double uniform_in(uint64_t *seed, double low, double high) {
  return low + (high - low) * next_uniform(seed);
}

/** The star's parameters, kept for its closed form. */
struct Star {
  int satellites;
  double central_delta;
  double h0;
  double satellite_delta[MAX_ROWS];
  double k_to_central[MAX_ROWS];
  double k_to_stars[MAX_ROWS];
  double g0[MAX_ROWS];
};

/**
 * Star: the central (row 0) holds H and Cold, H to Cold at 0.9; satellite s
 * (row s) holds G_s and Stars_s, G_s to the central's H at k_s (cross-record)
 * and to its own Stars_s at k'_s, with Delta_s in [1, 1.5].
 */
static void build_star(struct System *system, struct Star *star, int satellites, uint64_t seed) {
  system_init(system, satellites + 1, 1);
  star->satellites = satellites;
  star->central_delta = 1.25;
  star->h0 = 0.8;
  system->delta[0] = star->central_delta;
  const int h = add_entry(system, 0, 0, star->h0);
  const int cold = add_entry(system, 0, 0, 0.1);
  add_flux(system, RATE_FLUX_INTERNAL, h, cold, 0, 0.9);
  for (int s = 1; s <= satellites; s++) {
    star->satellite_delta[s] = uniform_in(&seed, 1.0, 1.5);
    star->k_to_central[s] = uniform_in(&seed, 1.0, 2.0);
    star->k_to_stars[s] = uniform_in(&seed, 0.5, 1.0);
    star->g0[s] = uniform_in(&seed, 0.2, 1.0);
    system->delta[s] = star->satellite_delta[s];
    const int gas = add_entry(system, s, 0, star->g0[s]);
    const int stars = add_entry(system, s, 0, 0.0);
    add_flux(system, RATE_FLUX_INTERNAL, gas, h, 1, star->k_to_central[s]);
    add_flux(system, RATE_FLUX_INTERNAL, gas, stars, 2, star->k_to_stars[s]);
  }
}

/** H(1) = H0 e^(-b) + sum_s Delta_s k_s G_s0 (e^(-a_s) - e^(-b)) / (b - a_s). */
static double star_h_closed_form(const struct Star *star) {
  const double b = star->central_delta * 0.9;
  double h = star->h0 * exp(-b);
  for (int s = 1; s <= star->satellites; s++) {
    const double a = (star->k_to_central[s] + star->k_to_stars[s]) * star->satellite_delta[s];
    h += star->satellite_delta[s] * star->k_to_central[s] * star->g0[s] * (exp(-a) - exp(-b)) /
         (b - a);
  }
  return h;
}

/* ---------------------------------------------------------------------------
 * Order and convergence
 * ------------------------------------------------------------------------- */

/**
 * @test    Linear chain at fixed n = 16, 32, 64, 128 converges at third order
 *
 * Expected: |p - 3| <= 0.2 on each halving, p = log2(e_n / e_2n), against the
 * chain's closed form at t = Delta = 2.
 */
static int test_linear_chain_fixed_steps_third_order(void) {
  const int64_t steps[] = {16, 32, 64, 128};
  double error[4];
  for (int j = 0; j < 4; j++) {
    struct System system;
    struct LawProbe probe = law_probe(constant_law);
    const struct RateIntegratorSettings settings = fixed_settings(steps[j]);
    struct Run run;
    build_chain(&system, 2.0);
    TEST_ASSERT(run_segment(&system, &probe, &settings, &run) == 0, "workspace too small");
    TEST_ASSERT(run.cause == RATE_CAUSE_NONE, "fixed-step chain must integrate");
    error[j] = chain_error(system.state, 2.0);
    printf("\n  chain n=%-4lld error=%.4e", (long long)steps[j], error[j]);
  }
  for (int j = 0; j < 3; j++) {
    const double p = log2(error[j] / error[j + 1]);
    printf("\n  chain halving %lld->%lld p=%.4f (|p-3|<=0.2)", (long long)steps[j],
           (long long)steps[j + 1], p);
    TEST_ASSERT(fabs(p - 3.0) <= 0.2, "linear chain fixed-step order must be 3 within 0.2");
  }
  printf("\n");
  return TEST_PASS;
}

/**
 * @test    Linear chain adaptive, Delta = 20, relative tolerance 1e-3 to 1e-10
 *
 * Expected: the end error strictly decreases and each per-decade ratio lies in [5, 20].
 */
static int test_linear_chain_adaptive_tolerance_ratio(void) {
  double error[8];
  double relative_tolerance = 1e-3;
  for (int j = 0; j < 8; j++) {
    struct System system;
    struct LawProbe probe = law_probe(constant_law);
    const struct RateIntegratorSettings settings = adaptive_settings(relative_tolerance);
    struct Run run;
    build_chain(&system, 20.0);
    TEST_ASSERT(run_segment(&system, &probe, &settings, &run) == 0, "workspace too small");
    TEST_ASSERT(run.cause == RATE_CAUSE_NONE, "adaptive chain must integrate");
    error[j] = chain_error(system.state, 20.0);
    printf("\n  chain rtol=%.0e error=%.4e accepted=%lld rejected=%lld", relative_tolerance,
           error[j], (long long)run.outcome.stats.accepted_steps,
           (long long)run.outcome.stats.rejected_steps);
    relative_tolerance /= 10.0;
  }
  for (int j = 0; j < 7; j++) {
    const double ratio = error[j] / error[j + 1];
    printf("\n  chain decade %d ratio=%.3f (in [5, 20])", j, ratio);
    TEST_ASSERT(error[j + 1] < error[j], "chain end error must strictly decrease");
    TEST_ASSERT(ratio >= 5.0 && ratio <= 20.0, "chain per-decade error ratio must be in [5, 20]");
  }
  printf("\n");
  return TEST_PASS;
}

/**
 * @test    Bimolecular at fixed n = 16, 32, 64, 128: third order and two invariants
 *
 * Expected: |p - 3| <= 0.2 on each halving against the closed form; at every n,
 * A - B and the total each change by at most tol_F (L = 2, n_F = 3, no external
 * amounts).
 */
static int test_bimolecular_fixed_steps_order_and_invariants(void) {
  const int64_t steps[] = {16, 32, 64, 128};
  double error[4];
  for (int j = 0; j < 4; j++) {
    struct System system;
    struct LawProbe probe = law_probe(bimolecular_law);
    const struct RateIntegratorSettings settings = fixed_settings(steps[j]);
    struct Run run;
    build_bimolecular(&system);
    TEST_ASSERT(run_segment(&system, &probe, &settings, &run) == 0, "workspace too small");
    TEST_ASSERT(run.cause == RATE_CAUSE_NONE, "fixed-step bimolecular must integrate");
    error[j] = bimolecular_error(system.state);
    const double tolerance = closure_tolerance(2, steps[j], 3, 1.6, 0.0, 0.0);
    const double difference_drift = fabs((system.state[0] - system.state[1]) - (1.0 - 0.6));
    const double total_drift =
        fabs(((system.state[0] + system.state[1]) + system.state[2]) - (1.0 + 0.6));
    printf("\n  bimolecular n=%-4lld error=%.4e |d(A-B)|=%.3e |d total|=%.3e tol_F=%.3e",
           (long long)steps[j], error[j], difference_drift, total_drift, tolerance);
    TEST_ASSERT(difference_drift <= tolerance, "A - B must change by at most tol_F");
    TEST_ASSERT(total_drift <= tolerance, "the total must change by at most tol_F");
  }
  for (int j = 0; j < 3; j++) {
    const double p = log2(error[j] / error[j + 1]);
    printf("\n  bimolecular halving %lld->%lld p=%.4f (|p-3|<=0.2)", (long long)steps[j],
           (long long)steps[j + 1], p);
    TEST_ASSERT(fabs(p - 3.0) <= 0.2, "bimolecular fixed-step order must be 3 within 0.2");
  }
  printf("\n");
  return TEST_PASS;
}

/**
 * @test    Threshold law at fixed steps with the crossing at a step's midpoint
 *
 * G0 = 1/n puts the crossing (G = 1 at tau* = (1 - G0)/2 = 0.5 - 0.5/n) at the
 * midpoint of step n/2 - 1, the last midpoint before tau = 1/2 (G0 >= 0 needs
 * tau* <= 1/2). The relaxation after the crossing damps the kink's O(h^2) error
 * by about e^(-6 (1 - tau*)), so the latest crossing is the one whose measured
 * order shows the kink rather than the smooth O(h^3) error. Expected: |p - 2|
 * <= 0.2 for every halving from n = 40 to 1280.
 */
static int test_threshold_fixed_steps_second_order(void) {
  const int64_t steps[] = {40, 80, 160, 320, 640, 1280};
  double error[6];
  for (int j = 0; j < 6; j++) {
    const double g0 = 1.0 / (double)steps[j];
    struct System system;
    struct LawProbe probe = law_probe(threshold_law);
    const struct RateIntegratorSettings settings = fixed_settings(steps[j]);
    struct Run run;
    build_threshold(&system, g0);
    TEST_ASSERT(run_segment(&system, &probe, &settings, &run) == 0, "workspace too small");
    TEST_ASSERT(run.cause == RATE_CAUSE_NONE, "fixed-step threshold must integrate");
    error[j] = threshold_error(system.state, g0);
    printf("\n  threshold n=%-5lld G0=%.7f error=%.4e", (long long)steps[j], g0, error[j]);
  }
  for (int j = 0; j < 5; j++) {
    const double p = log2(error[j] / error[j + 1]);
    printf("\n  threshold halving %lld->%lld p=%.4f (|p-2|<=0.2)", (long long)steps[j],
           (long long)steps[j + 1], p);
    TEST_ASSERT(fabs(p - 2.0) <= 0.2, "threshold fixed-step order must be 2 within 0.2");
  }
  printf("\n");
  return TEST_PASS;
}

/**
 * @test    Threshold law adaptive at relative tolerance 1e-4, 1e-6, 1e-8, 1e-10 (G0 = 0.5)
 *
 * Expected: the error falls by a factor in [30, 300] per two decades, with at
 * most 30 rejections per run; and the step after an accepted step that
 * followed a rejection never grows beyond the 10% landing stretch.
 */
static int test_threshold_adaptive_tolerance_ratio(void) {
  const double tolerances[] = {1e-4, 1e-6, 1e-8, 1e-10};
  double error[4];
  for (int j = 0; j < 4; j++) {
    struct System system;
    struct LawProbe probe = law_probe(threshold_law);
    struct RateIntegratorSettings settings = adaptive_settings(tolerances[j]);
    static struct AttemptLog log;
    struct Run run;
    build_threshold(&system, 0.5);
    attach_log(&settings, &log, &system);
    TEST_ASSERT(run_segment(&system, &probe, &settings, &run) == 0, "workspace too small");
    TEST_ASSERT(run.cause == RATE_CAUSE_NONE, "adaptive threshold must integrate");
    error[j] = threshold_error(system.state, 0.5);
    printf("\n  threshold rtol=%.0e error=%.4e accepted=%lld rejected=%lld (<= 30)", tolerances[j],
           error[j], (long long)run.outcome.stats.accepted_steps,
           (long long)run.outcome.stats.rejected_steps);
    TEST_ASSERT(run.outcome.stats.rejected_steps <= 30, "at most 30 rejections per run");
    TEST_ASSERT(!log.grew_after_rejection, "no step may grow after a rejection");
  }
  for (int j = 0; j < 3; j++) {
    const double ratio = error[j] / error[j + 1];
    printf("\n  threshold two-decade ratio %d=%.2f (in [30, 300])", j, ratio);
    TEST_ASSERT(ratio >= 30.0 && ratio <= 300.0,
                "threshold error must fall by a factor in [30, 300] per two decades");
  }
  printf("\n");
  return TEST_PASS;
}

/* ---------------------------------------------------------------------------
 * Conservation and the external amounts
 * ------------------------------------------------------------------------- */

/**
 * @test    Closed cycle with nonlinear coefficients keeps its total within tol_F
 *
 * Start (0.5, 0.3, 0.2), Delta = 50, after 1e3, 1e4 and 1e5 fixed steps; each
 * entry has one inflow and one outflow, so L = 2, and n_F = 3.
 */
static int test_closed_cycle_total_within_tolerance(void) {
  const int64_t steps[] = {1000, 10000, 100000};
  for (int j = 0; j < 3; j++) {
    struct System system;
    struct LawProbe probe = law_probe(cycle_law);
    const struct RateIntegratorSettings settings = fixed_settings(steps[j]);
    struct Run run;
    system_init(&system, 1, 1);
    system.delta[0] = 50.0;
    const int a = add_entry(&system, 0, 0, 0.5);
    const int b = add_entry(&system, 0, 0, 0.3);
    const int c = add_entry(&system, 0, 0, 0.2);
    add_flux(&system, RATE_FLUX_INTERNAL, a, b, 0, 0.0);
    add_flux(&system, RATE_FLUX_INTERNAL, b, c, 1, 0.0);
    add_flux(&system, RATE_FLUX_INTERNAL, c, a, 2, 0.0);
    TEST_ASSERT(run_segment(&system, &probe, &settings, &run) == 0, "workspace too small");
    TEST_ASSERT(run.cause == RATE_CAUSE_NONE, "fixed-step cycle must integrate");
    const double start_total = (0.5 + 0.3) + 0.2;
    const double drift =
        fabs(((system.state[a] + system.state[b]) + system.state[c]) - start_total);
    const double tolerance = closure_tolerance(2, steps[j], 3, start_total, 0.0, 0.0);
    printf("\n  cycle N=%-6lld |d total|=%.3e tol_F=%.3e ratio=%.2e", (long long)steps[j], drift,
           tolerance, drift / tolerance);
    TEST_ASSERT(drift <= tolerance, "cycle total must stay within tol_F");
  }
  printf("\n");
  return TEST_PASS;
}

/**
 * @test    Source into R and sink out of it at fixed N = 1e3, 1e4, 1e5
 *
 * s = 0.31416, k = 1.73205, R0 = 0.73681, Delta = 30, R_exact = s/k + (R0 -
 * s/k) e^(-k Delta). Expected: |Src - s Delta| <= 8u N s Delta; |Snk - (s Delta
 * - (R_exact - R0))| <= |R - R_exact| + 8u N (Src + Snk); closure within tol_F
 * (L = 2, n_F = 1).
 */
static int test_source_sink_external_amounts(void) {
  const double s = 0.31416;
  const double k = 1.73205;
  const double r0 = 0.73681;
  const double delta = 30.0;
  const double r_exact = s / k + (r0 - s / k) * exp(-k * delta);
  const int64_t steps[] = {1000, 10000, 100000};
  for (int j = 0; j < 3; j++) {
    struct System system;
    struct LawProbe probe = law_probe(constant_law);
    const struct RateIntegratorSettings settings = fixed_settings(steps[j]);
    struct Run run;
    system_init(&system, 1, 1);
    system.delta[0] = delta;
    const int r = add_entry(&system, 0, 0, r0);
    add_flux(&system, RATE_FLUX_SOURCE, RATE_END_EXTERNAL, r, 0, s);
    add_flux(&system, RATE_FLUX_SINK, r, RATE_END_EXTERNAL, 1, k);
    TEST_ASSERT(run_segment(&system, &probe, &settings, &run) == 0, "workspace too small");
    TEST_ASSERT(run.cause == RATE_CAUSE_NONE, "fixed-step source and sink must integrate");
    const double n = (double)steps[j];
    const double src = run.source[0];
    const double snk = run.sink[0];
    const double r_error = fabs(system.state[r] - r_exact);
    const double source_gap = fabs(src - s * delta);
    const double source_bound = 8.0 * UNIT_ROUNDOFF * n * s * delta;
    const double sink_gap = fabs(snk - (s * delta - (r_exact - r0)));
    const double sink_bound = r_error + 8.0 * UNIT_ROUNDOFF * n * (src + snk);
    const double closure = fabs((system.state[r] - r0) - (src - snk));
    const double tolerance = closure_tolerance(2, steps[j], 1, r0, src, snk);
    printf("\n  source-sink N=%-6lld |Src-s*D|=%.3e (<=%.3e) |Snk-cf|=%.3e (<=%.3e) "
           "closure=%.3e (<=%.3e)",
           (long long)steps[j], source_gap, source_bound, sink_gap, sink_bound, closure, tolerance);
    TEST_ASSERT(source_gap <= source_bound, "Src must be s * Delta within 8u N s Delta");
    TEST_ASSERT(sink_gap <= sink_bound, "Snk must meet its closed form within the bound");
    TEST_ASSERT(closure <= tolerance, "R must close within tol_F");
  }
  printf("\n");
  return TEST_PASS;
}

/**
 * @test    A cross-record transfer between rows of different Delta conserves
 *
 * Central row (Delta 0.7) holds H and C; satellites (Delta 2.3 and 1.6) send
 * their gas to the central's H; H to C, a sink out of C and a source into one
 * satellite's gas. Expected: the total closes within tol_F (L = 3 at H, n_F = 4).
 */
static int test_mixed_delta_cross_record_conserves(void) {
  struct System system;
  struct LawProbe probe = law_probe(constant_law);
  const struct RateIntegratorSettings settings = adaptive_settings(1e-6);
  struct Run run;
  system_init(&system, 3, 1);
  system.delta[0] = 0.7;
  system.delta[1] = 2.3;
  system.delta[2] = 1.6;
  const int h = add_entry(&system, 0, 0, 0.4);
  const int c = add_entry(&system, 0, 0, 0.2);
  const int g1 = add_entry(&system, 1, 0, 1.0);
  const int g2 = add_entry(&system, 2, 0, 0.6);
  add_flux(&system, RATE_FLUX_INTERNAL, h, c, 0, 0.5);
  add_flux(&system, RATE_FLUX_INTERNAL, g1, h, 1, 1.3);
  add_flux(&system, RATE_FLUX_INTERNAL, g2, h, 1, 0.9);
  add_flux(&system, RATE_FLUX_SINK, c, RATE_END_EXTERNAL, 2, 0.2);
  add_flux(&system, RATE_FLUX_SOURCE, RATE_END_EXTERNAL, g1, 3, 0.3);
  const double start_total = ((0.4 + 0.2) + 1.0) + 0.6;
  TEST_ASSERT(run_segment(&system, &probe, &settings, &run) == 0, "workspace too small");
  TEST_ASSERT(run.cause == RATE_CAUSE_NONE, "mixed-Delta component must integrate");
  TEST_ASSERT(run.outcome.max_terms_per_entry == 3, "L is 3 (two inflows and an outflow at H)");
  const double end_total =
      ((system.state[h] + system.state[c]) + system.state[g1]) + system.state[g2];
  const double drift = fabs((end_total - start_total) - (run.source[0] - run.sink[0]));
  const double tolerance = closure_tolerance(3, run.outcome.stats.accepted_steps, 4, start_total,
                                             run.source[0], run.sink[0]);
  printf("\n  mixed Delta accepted=%lld closure=%.3e tol_F=%.3e\n",
         (long long)run.outcome.stats.accepted_steps, drift, tolerance);
  TEST_ASSERT(run.source[0] > 0.0 && run.sink[0] > 0.0, "both external amounts must be non-zero");
  TEST_ASSERT(drift <= tolerance, "mixed-Delta cross-record transfer must close within tol_F");
  return TEST_PASS;
}

/**
 * @test    The closure check, called on its own, detects a 1e-6 relative imbalance
 *
 * Total 0 holds three entries summing to 1 at the start; the end adds 1e-6 to
 * one entry. Expected: cause ledger naming total 0 with no external amount, and
 * success when the difference is declared as a source, or as a sink when the
 * end removes it.
 */
static int test_closure_check_detects_imbalance(void) {
  const struct RateEntry entries[] = {{0, 0}, {0, 0}, {1, 0}, {1, 1}};
  const double start[] = {0.3, 0.5, 0.2, 0.9};
  const double raised[] = {0.3, 0.5, 0.2 + 1e-6, 0.9};
  const double lowered[] = {0.3, 0.5, 0.2 - 1e-6, 0.9};
  const double declared[] = {1e-6, 0.0};
  const double none[] = {0.0, 0.0};
  struct RateClosureInput input = {
      .entries = entries,
      .num_entries = 4,
      .num_totals = 2,
      .start_state = start,
      .end_state = raised,
      .source_amounts = NULL,
      .sink_amounts = NULL,
      .max_terms_per_entry = 2,
      .accepted_steps = 10,
  };
  struct RateFailure failure;

  TEST_ASSERT(rate_check_closure(&input, &failure) == RATE_CAUSE_LEDGER,
              "an undeclared 1e-6 gain must fail the closure");
  TEST_ASSERT(failure.cause == RATE_CAUSE_LEDGER && failure.total == 0,
              "the ledger failure must name total 0");
  TEST_ASSERT_STRING_EQUAL(rate_cause_name(failure.cause), "ledger", "cause name is ledger");

  input.source_amounts = declared;
  input.sink_amounts = none;
  TEST_ASSERT(rate_check_closure(&input, &failure) == RATE_CAUSE_NONE,
              "a gain declared as a source must close");
  TEST_ASSERT(failure.cause == RATE_CAUSE_NONE, "a closing segment records no failure");

  input.end_state = lowered;
  input.source_amounts = NULL;
  input.sink_amounts = NULL;
  TEST_ASSERT(rate_check_closure(&input, &failure) == RATE_CAUSE_LEDGER,
              "an undeclared 1e-6 loss must fail the closure");
  input.source_amounts = none;
  input.sink_amounts = declared;
  TEST_ASSERT(rate_check_closure(&input, &failure) == RATE_CAUSE_NONE,
              "a loss declared as a sink must close");
  return TEST_PASS;
}

/* ---------------------------------------------------------------------------
 * Positivity and canonical order
 * ------------------------------------------------------------------------- */

/** S (start 1) with three transfers to X, Y and Z, flux i holding transfer order[i]. */
static void build_three_sinks(struct System *system, const int *order) {
  const double coefficient[] = {0.7, 1.9, 0.31};
  system_init(system, 1, 1);
  system->delta[0] = 3.0;
  const int s = add_entry(system, 0, 0, 1.0);
  add_entry(system, 0, 0, 0.0);
  add_entry(system, 0, 0, 0.0);
  add_entry(system, 0, 0, 0.0);
  for (int i = 0; i < 3; i++) {
    const int transfer = order[i];
    add_flux(system, RATE_FLUX_INTERNAL, s, 1 + transfer, transfer, coefficient[transfer]);
  }
}

/**
 * @test    Three transfers out of one source split it in the ratio of their coefficients
 *
 * S to X, Y, Z at 0.7, 1.9, 0.31, S0 = 1, Delta = 3, default tolerances.
 * Expected: X/0.7, Y/1.9 and Z/0.31 agree to 1e-13 relative.
 */
static int test_three_sinks_split_in_coefficient_ratio(void) {
  const int order[] = {0, 1, 2};
  struct System system;
  struct LawProbe probe = law_probe(constant_law);
  const struct RateIntegratorSettings settings = adaptive_settings(1e-4);
  struct Run run;
  build_three_sinks(&system, order);
  TEST_ASSERT(run_segment(&system, &probe, &settings, &run) == 0, "workspace too small");
  TEST_ASSERT(run.cause == RATE_CAUSE_NONE, "three sinks must integrate");
  const double x = system.state[1] / 0.7;
  const double y = system.state[2] / 1.9;
  const double z = system.state[3] / 0.31;
  const double spread = fmax(fabs(y - x), fabs(z - x)) / x;
  printf("\n  three sinks X=%.6f Y=%.6f Z=%.6f relative spread=%.3e (<=1e-13)\n", system.state[1],
         system.state[2], system.state[3], spread);
  TEST_ASSERT(x > 0.1, "the recipients must receive a substantial amount");
  TEST_ASSERT(spread <= 1e-13, "received amounts must be in the coefficients' ratio to 1e-13");
  return TEST_PASS;
}

/**
 * @test    Two fast transfers out of S (300 and 700, Delta = 1) never go negative
 *
 * Both as internal transfers to X and Y and as external sinks, default
 * tolerances. Expected: no law sees a negative entry and no committed state has one.
 */
static int test_two_fast_sinks_stay_non_negative(void) {
  for (int external = 0; external <= 1; external++) {
    struct System system;
    struct LawProbe probe = law_probe(constant_law);
    struct RateIntegratorSettings settings = adaptive_settings(1e-4);
    struct AttemptLog log;
    struct Run run;
    system_init(&system, 1, 1);
    system.delta[0] = 1.0;
    const int s = add_entry(&system, 0, 0, 1.0);
    const int x = add_entry(&system, 0, 0, 0.0);
    const int y = add_entry(&system, 0, 0, 0.0);
    const enum RateFluxKind kind = external ? RATE_FLUX_SINK : RATE_FLUX_INTERNAL;
    add_flux(&system, kind, s, external ? RATE_END_EXTERNAL : x, 0, 300.0);
    add_flux(&system, kind, s, external ? RATE_END_EXTERNAL : y, 1, 700.0);
    attach_log(&settings, &log, &system);
    TEST_ASSERT(run_segment(&system, &probe, &settings, &run) == 0, "workspace too small");
    TEST_ASSERT(run.cause == RATE_CAUSE_NONE, "two fast sinks must integrate");
    printf("\n  two sinks (%s) accepted=%lld positivity rejections=%lld min law state=%.3e "
           "min committed=%.3e",
           external ? "external" : "internal", (long long)run.outcome.stats.accepted_steps,
           (long long)run.outcome.stats.positivity_rejections, probe.min_state, log.min_committed);
    TEST_ASSERT(probe.min_state >= 0.0, "no law may be evaluated at a negative entry");
    TEST_ASSERT(log.min_committed >= 0.0, "no committed entry may be negative");
  }
  printf("\n");
  return TEST_PASS;
}

/**
 * @test    A sink at 1 + 50 B out of A, with a source of 10 into B, stays non-negative
 *
 * A0 = 1, B0 = 0, Delta = 2, at relative tolerance 1e-4 (the default) and 0.5,
 * where the error control no longer limits the step and the coefficient's
 * growth within the step is held by the positivity guard alone. Expected: no
 * law sees a negative entry, no committed state has one, and every stage of
 * every accepted step has step * K_max <= C = 2.
 */
static int test_source_driven_sink_stays_non_negative(void) {
  const double tolerances[] = {1e-4, 0.5};
  for (int j = 0; j < 2; j++) {
    struct System system;
    struct LawProbe probe = law_probe(coupled_sink_law);
    struct RateIntegratorSettings settings = adaptive_settings(tolerances[j]);
    struct AttemptLog log;
    struct Run run;
    system_init(&system, 1, 1);
    system.delta[0] = 2.0;
    const int a = add_entry(&system, 0, 0, 1.0);
    const int b = add_entry(&system, 0, 0, 0.0);
    add_flux(&system, RATE_FLUX_SOURCE, RATE_END_EXTERNAL, b, 0, 0.0);
    add_flux(&system, RATE_FLUX_SINK, a, RATE_END_EXTERNAL, 1, 0.0);
    attach_log(&settings, &log, &system);
    probe.k_of_state = coupled_sink_k;
    probe.skip_evaluation = 2; /* the first-step probe: A0 = 1 has a non-zero weight */
    log.probe = &probe;
    TEST_ASSERT(run_segment(&system, &probe, &settings, &run) == 0, "workspace too small");
    TEST_ASSERT(run.cause == RATE_CAUSE_NONE, "source-driven sink must integrate");
    printf("\n  coupled sink rtol=%.0e accepted=%lld positivity rejections=%lld min law "
           "state=%.3e min committed=%.3e max step*K=%.4f (<=2)",
           tolerances[j], (long long)run.outcome.stats.accepted_steps,
           (long long)run.outcome.stats.positivity_rejections, probe.min_state, log.min_committed,
           log.worst_k_product);
    TEST_ASSERT(system.state[a] >= 0.0, "A must end non-negative");
    TEST_ASSERT(probe.min_state >= 0.0, "no law may be evaluated at a negative entry");
    TEST_ASSERT(log.min_committed >= 0.0, "no committed entry may be negative");
    TEST_ASSERT(log.worst_k_product <= 2.0,
                "every stage of an accepted step must have step * K_max <= C");
  }
  printf("\n");
  return TEST_PASS;
}

/**
 * @test    The three sinks listed in all six orders give bit-identical end states
 *
 * Each transfer keeps its global index and coefficient; only the listing order
 * changes. Expected: identical end-state bytes and step counts.
 */
static int test_three_sinks_order_independent(void) {
  const int orders[6][3] = {{0, 1, 2}, {0, 2, 1}, {1, 0, 2}, {1, 2, 0}, {2, 0, 1}, {2, 1, 0}};
  double reference[4];
  int64_t reference_steps = 0;
  for (int p = 0; p < 6; p++) {
    struct System system;
    struct LawProbe probe = law_probe(constant_law);
    const struct RateIntegratorSettings settings = adaptive_settings(1e-4);
    struct Run run;
    build_three_sinks(&system, orders[p]);
    TEST_ASSERT(run_segment(&system, &probe, &settings, &run) == 0, "workspace too small");
    TEST_ASSERT(run.cause == RATE_CAUSE_NONE, "three sinks must integrate in every order");
    if (p == 0) {
      memcpy(reference, system.state, sizeof(reference));
      reference_steps = run.outcome.stats.accepted_steps;
      continue;
    }
    TEST_ASSERT(memcmp(reference, system.state, sizeof(reference)) == 0,
                "every listing order must give bit-identical end states");
    TEST_ASSERT(run.outcome.stats.accepted_steps == reference_steps,
                "every listing order must take the same steps");
  }
  return TEST_PASS;
}

/**
 * @test    Stiff exchange with a slow drain is held at the positivity bound
 *
 * kf = kb = 1e3 and 1e4, drain 1, Delta = 3, A0 = B0 = 0.5, relative tolerance
 * 1e-6. Expected: N_pos <= accepted <= 1.1 N_pos + 10, N_pos =
 * ceil(Delta (kb + 1) / (0.9 * 2)); and, since the step is capped at sigma C /
 * K_max before its first stage, no positivity rejection.
 */
static int test_stiff_pair_steps_at_positivity_bound(void) {
  const double rates[] = {1e3, 1e4};
  for (int j = 0; j < 2; j++) {
    struct System system;
    struct LawProbe probe = law_probe(constant_law);
    const struct RateIntegratorSettings settings = adaptive_settings(1e-6);
    struct Run run;
    build_stiff_pair(&system, rates[j]);
    TEST_ASSERT(run_segment(&system, &probe, &settings, &run) == 0, "workspace too small");
    TEST_ASSERT(run.cause == RATE_CAUSE_NONE, "stiff pair must integrate");
    const double n_pos = ceil(3.0 * (rates[j] + 1.0) / (0.9 * 2.0));
    const double accepted = (double)run.outcome.stats.accepted_steps;
    printf("\n  stiff pair k=%.0e accepted=%lld rejected=%lld N_pos=%.0f bound=[%.0f, %.1f]",
           rates[j], (long long)run.outcome.stats.accepted_steps,
           (long long)run.outcome.stats.rejected_steps, n_pos, n_pos, 1.1 * n_pos + 10.0);
    TEST_ASSERT(accepted >= n_pos, "accepted steps must be at least N_pos");
    TEST_ASSERT(accepted <= 1.1 * n_pos + 10.0, "accepted steps must be at most 1.1 N_pos + 10");
    TEST_ASSERT(run.outcome.stats.positivity_rejections == 0,
                "the a-priori cap must leave no positivity rejection on the stiff pair");
  }
  printf("\n");
  return TEST_PASS;
}

/* ---------------------------------------------------------------------------
 * Segments, rows and repeatability
 * ------------------------------------------------------------------------- */

/** Segment 1: central row 0 holds A (1) and B (0); satellite row 1 holds S (0.5). */
static void build_first_interval(struct System *system) {
  system_init(system, 2, 1);
  system->delta[0] = 1.0;
  system->delta[1] = 1.0;
  const int a = add_entry(system, 0, 0, 1.0);
  const int b = add_entry(system, 0, 0, 0.0);
  const int s = add_entry(system, 1, 0, 0.5);
  add_flux(system, RATE_FLUX_INTERNAL, a, b, 0, 0.8);
  add_flux(system, RATE_FLUX_INTERNAL, s, a, 1, 1.7);
}

/** Segment 2: the satellite row is gone; row 0 holds A and B, A to B at 0.3. */
static void build_second_interval(struct System *system, double a_start, double b_start) {
  system_init(system, 1, 1);
  system->delta[0] = 1.0;
  const int a = add_entry(system, 0, 0, a_start);
  const int b = add_entry(system, 0, 0, b_start);
  add_flux(system, RATE_FLUX_INTERNAL, a, b, 0, 0.3);
}

/**
 * @test    Two intervals with a removed row meet the piecewise closed form
 *
 * Relative tolerance 1e-10. Expected within 1e-9: S1 = S0 e^(-1.7), A1 =
 * e^(-0.8) + 1.7 S0 (e^(-1.7) - e^(-0.8))/(0.8 - 1.7), B1 = 1.5 - A1 - S1;
 * after S merges into A, A2 = (A1 + S1) e^(-0.3) and B2 = 1.5 - A2.
 */
static int test_two_intervals_piecewise_closed_form(void) {
  const double s0 = 0.5;
  const double s1 = s0 * exp(-1.7);
  const double a1 = exp(-0.8) + 1.7 * s0 * (exp(-1.7) - exp(-0.8)) / (0.8 - 1.7);
  const double b1 = 1.5 - a1 - s1;
  const double a2 = (a1 + s1) * exp(-0.3);
  const double b2 = 1.5 - a2;
  const double bound = 10.0 * 1e-10;
  struct System system;
  struct LawProbe probe = law_probe(constant_law);
  const struct RateIntegratorSettings settings = adaptive_settings(1e-10);
  struct Run run;

  build_first_interval(&system);
  TEST_ASSERT(run_segment(&system, &probe, &settings, &run) == 0, "workspace too small");
  TEST_ASSERT(run.cause == RATE_CAUSE_NONE, "first interval must integrate");
  const double error1 =
      max3(fabs(system.state[0] - a1), fabs(system.state[1] - b1), fabs(system.state[2] - s1));
  const double a_merged = system.state[0] + system.state[2];
  const double b_carried = system.state[1];

  build_second_interval(&system, a_merged, b_carried);
  TEST_ASSERT(run_segment(&system, &probe, &settings, &run) == 0, "workspace too small");
  TEST_ASSERT(run.cause == RATE_CAUSE_NONE, "second interval must integrate");
  const double error2 = fmax(fabs(system.state[0] - a2), fabs(system.state[1] - b2));
  printf("\n  two intervals error1=%.3e error2=%.3e bound=%.1e\n", error1, error2, bound);
  TEST_ASSERT(error1 <= bound, "first interval must meet its closed form within 10 rtol");
  TEST_ASSERT(error2 <= bound, "second interval must meet its closed form within 10 rtol");
  return TEST_PASS;
}

/**
 * @test    The second interval depends on its start state only
 *
 * Expected: integrating the second interval after the first, and again from a
 * freshly built system holding the same start values in a scribbled workspace,
 * gives the same attempt sequence, step count and end-state bytes.
 */
static int test_second_interval_depends_on_start_only(void) {
  struct System system;
  struct LawProbe probe = law_probe(constant_law);
  struct RateIntegratorSettings settings = adaptive_settings(1e-10);
  static struct AttemptLog chained_log;
  static struct AttemptLog fresh_log;
  struct Run run;

  build_first_interval(&system);
  TEST_ASSERT(run_segment(&system, &probe, &settings, &run) == 0, "workspace too small");
  TEST_ASSERT(run.cause == RATE_CAUSE_NONE, "first interval must integrate");
  const double a_merged = system.state[0] + system.state[2];
  const double b_carried = system.state[1];
  build_second_interval(&system, a_merged, b_carried);
  attach_log(&settings, &chained_log, &system);
  TEST_ASSERT(run_segment(&system, &probe, &settings, &run) == 0, "workspace too small");
  TEST_ASSERT(run.cause == RATE_CAUSE_NONE, "chained second interval must integrate");
  const int64_t chained_steps = run.outcome.stats.accepted_steps;
  double chained_end[2];
  memcpy(chained_end, system.state, sizeof(chained_end));

  struct System fresh;
  memset(workspace, 0xA5, sizeof(workspace));
  build_second_interval(&fresh, a_merged, b_carried);
  attach_log(&settings, &fresh_log, &fresh);
  TEST_ASSERT(run_segment(&fresh, &probe, &settings, &run) == 0, "workspace too small");
  TEST_ASSERT(run.cause == RATE_CAUSE_NONE, "fresh second interval must integrate");
  printf("\n  second interval chained steps=%lld fresh steps=%lld\n", (long long)chained_steps,
         (long long)run.outcome.stats.accepted_steps);
  TEST_ASSERT(run.outcome.stats.accepted_steps == chained_steps, "step counts must be equal");
  TEST_ASSERT(memcmp(chained_end, fresh.state, sizeof(chained_end)) == 0,
              "end states must be bit-identical");
  TEST_ASSERT(same_sequence(&chained_log, &fresh_log), "attempt sequences must be bit-identical");
  return TEST_PASS;
}

/**
 * @test    Star of 3 and 30 satellites with mixed Delta meets its closed form
 *
 * Relative tolerance 1e-8. Expected: H(1) within 10 times the relative
 * tolerance of the closed form (star_h_closed_form()).
 */
static int test_star_closed_form(void) {
  const int satellites[] = {3, 30};
  const uint64_t seeds[] = {20261112u, 20261113u};
  for (int j = 0; j < 2; j++) {
    struct System system;
    static struct Star star;
    struct LawProbe probe = law_probe(constant_law);
    const struct RateIntegratorSettings settings = adaptive_settings(1e-8);
    struct Run run;
    build_star(&system, &star, satellites[j], seeds[j]);
    TEST_ASSERT(run_segment(&system, &probe, &settings, &run) == 0, "workspace too small");
    TEST_ASSERT(run.cause == RATE_CAUSE_NONE, "star must integrate");
    const double expected = star_h_closed_form(&star);
    const double error = fabs(system.state[0] - expected);
    printf("\n  star N=%-2d H=%.12f closed form=%.12f error=%.3e (<=1e-7) accepted=%lld",
           satellites[j], system.state[0], expected, error,
           (long long)run.outcome.stats.accepted_steps);
    TEST_ASSERT(error <= 10.0 * 1e-8, "star H(1) must meet its closed form within 10 rtol");
  }
  printf("\n");
  return TEST_PASS;
}

/**
 * @test    Repeated star integrations are bit-identical
 *
 * The 30-satellite star integrated twice, then again after a different
 * component (the 3-satellite star) and a scribbled workspace. Expected: the
 * same end-state bytes and attempt sequence every time.
 */
static int test_star_repeatable(void) {
  static struct AttemptLog first_log;
  static struct AttemptLog repeat_log;
  static struct Star star;
  struct System system;
  struct LawProbe probe = law_probe(constant_law);
  struct RateIntegratorSettings settings = adaptive_settings(1e-8);
  struct Run run;
  double first_end[MAX_ENTRIES];

  build_star(&system, &star, 30, 20261113u);
  attach_log(&settings, &first_log, &system);
  TEST_ASSERT(run_segment(&system, &probe, &settings, &run) == 0, "workspace too small");
  TEST_ASSERT(run.cause == RATE_CAUSE_NONE, "star must integrate");
  memcpy(first_end, system.state, sizeof(first_end));
  const int num_entries = system.num_entries;

  for (int repeat = 0; repeat < 2; repeat++) {
    if (repeat == 1) {
      struct System other;
      static struct Star other_star;
      struct RateIntegratorSettings other_settings = adaptive_settings(1e-6);
      build_star(&other, &other_star, 3, 20261112u);
      TEST_ASSERT(run_segment(&other, &probe, &other_settings, &run) == 0, "workspace too small");
      TEST_ASSERT(run.cause == RATE_CAUSE_NONE, "the other component must integrate");
      memset(workspace, 0x5A, sizeof(workspace));
    }
    build_star(&system, &star, 30, 20261113u);
    attach_log(&settings, &repeat_log, &system);
    TEST_ASSERT(run_segment(&system, &probe, &settings, &run) == 0, "workspace too small");
    TEST_ASSERT(run.cause == RATE_CAUSE_NONE, "repeated star must integrate");
    TEST_ASSERT(memcmp(first_end, system.state, (size_t)num_entries * sizeof(double)) == 0,
                "repeated star end state must be bit-identical");
    TEST_ASSERT(same_sequence(&first_log, &repeat_log),
                "repeated star attempt sequence must be bit-identical");
  }
  printf("\n  star repeat attempts=%lld (identical twice, before and after another component)\n",
         (long long)first_log.count);
  return TEST_PASS;
}

/**
 * @test    A row with Delta = 0 donates nothing, takes no source, but receives
 *
 * Row 0 (Delta 0) holds P (0.7, with a source of 0.5 into it and a transfer to Q
 * at 2), Q (0.2) and R (0); row 1 (Delta 1.5) holds D (1) sending to R at 1.1.
 * Relative tolerance 1e-8. Expected: P and Q bit-unchanged; no source amount;
 * R = D0 (1 - e^(-1.1 * 1.5)) within 10 rtol and equal to D's loss within
 * tol_F; the total closes within tol_F. A segment whose every Delta is zero
 * takes no step, evaluates no law and leaves the state bit-unchanged.
 */
static int test_zero_delta_row(void) {
  struct System system;
  struct LawProbe probe = law_probe(constant_law);
  const struct RateIntegratorSettings settings = adaptive_settings(1e-8);
  struct Run run;
  system_init(&system, 2, 1);
  system.delta[0] = 0.0;
  system.delta[1] = 1.5;
  const int p = add_entry(&system, 0, 0, 0.7);
  const int q = add_entry(&system, 0, 0, 0.2);
  const int r = add_entry(&system, 0, 0, 0.0);
  const int d = add_entry(&system, 1, 0, 1.0);
  add_flux(&system, RATE_FLUX_INTERNAL, p, q, 0, 2.0);
  add_flux(&system, RATE_FLUX_SOURCE, RATE_END_EXTERNAL, p, 1, 0.5);
  add_flux(&system, RATE_FLUX_INTERNAL, d, r, 2, 1.1);
  const double p_start = system.state[p];
  const double q_start = system.state[q];
  TEST_ASSERT(run_segment(&system, &probe, &settings, &run) == 0, "workspace too small");
  TEST_ASSERT(run.cause == RATE_CAUSE_NONE, "zero-Delta row component must integrate");
  const double r_expected = 1.0 * (1.0 - exp(-1.1 * 1.5));
  const double donor_loss = 1.0 - system.state[d];
  const double start_total = ((0.7 + 0.2) + 0.0) + 1.0;
  const double end_total =
      ((system.state[p] + system.state[q]) + system.state[r]) + system.state[d];
  const double tolerance = closure_tolerance(2, run.outcome.stats.accepted_steps, 4, start_total,
                                             run.source[0], run.sink[0]);
  printf("\n  zero Delta R=%.12f closed form=%.12f |R - D loss|=%.3e closure=%.3e tol_F=%.3e",
         system.state[r], r_expected, fabs(system.state[r] - donor_loss),
         fabs(end_total - start_total), tolerance);
  TEST_ASSERT(memcmp(&system.state[p], &p_start, sizeof(double)) == 0,
              "a Delta = 0 row's own law and source must move nothing out of or into P");
  TEST_ASSERT(memcmp(&system.state[q], &q_start, sizeof(double)) == 0,
              "a Delta = 0 row's own law must move nothing into Q");
  TEST_ASSERT(run.source[0] == 0.0, "a source into a Delta = 0 row must move nothing");
  TEST_ASSERT(fabs(system.state[r] - r_expected) <= 10.0 * 1e-8,
              "R must receive the donor's closed-form amount within 10 rtol");
  TEST_ASSERT(fabs(system.state[r] - donor_loss) <= tolerance,
              "R must gain exactly the donor's amount (within tol_F)");
  TEST_ASSERT(fabs(end_total - start_total) <= tolerance, "the total must close within tol_F");

  /* Every Delta zero: no step, no law. */
  double before[MAX_ENTRIES];
  memcpy(before, system.state, sizeof(before));
  system.delta[1] = 0.0;
  TEST_ASSERT(run_segment(&system, &probe, &settings, &run) == 0, "workspace too small");
  TEST_ASSERT(run.cause == RATE_CAUSE_NONE, "an all-zero-Delta segment succeeds");
  TEST_ASSERT_EQUAL(probe.evaluations, 0, "an all-zero-Delta segment evaluates no law");
  TEST_ASSERT_EQUAL(run.outcome.stats.accepted_steps + run.outcome.stats.rejected_steps, 0,
                    "an all-zero-Delta segment takes no step");
  TEST_ASSERT(memcmp(before, system.state, sizeof(before)) == 0,
              "an all-zero-Delta segment leaves the state bit-unchanged");
  printf("\n  all-zero Delta: evaluations=%lld steps=0\n", (long long)probe.evaluations);
  return TEST_PASS;
}

/**
 * @test    An all-empty component with no source takes one step and stays zero
 *
 * Three empty entries in a cycle plus a sink, all coefficients non-zero.
 * Expected: exactly one accepted step, no rejection, four law evaluations (no
 * first-step probe), every entry exactly zero.
 */
static int test_all_empty_component_single_step(void) {
  struct System system;
  struct LawProbe probe = law_probe(constant_law);
  const struct RateIntegratorSettings settings = adaptive_settings(1e-4);
  struct Run run;
  system_init(&system, 1, 1);
  system.delta[0] = 1.0;
  const int a = add_entry(&system, 0, 0, 0.0);
  const int b = add_entry(&system, 0, 0, 0.0);
  const int c = add_entry(&system, 0, 0, 0.0);
  add_flux(&system, RATE_FLUX_INTERNAL, a, b, 0, 1.0);
  add_flux(&system, RATE_FLUX_INTERNAL, b, c, 1, 2.0);
  add_flux(&system, RATE_FLUX_INTERNAL, c, a, 2, 0.5);
  add_flux(&system, RATE_FLUX_SINK, c, RATE_END_EXTERNAL, 3, 0.7);
  TEST_ASSERT(run_segment(&system, &probe, &settings, &run) == 0, "workspace too small");
  TEST_ASSERT(run.cause == RATE_CAUSE_NONE, "all-empty component must integrate");
  TEST_ASSERT_EQUAL(run.outcome.stats.accepted_steps, 1, "exactly one accepted step");
  TEST_ASSERT_EQUAL(run.outcome.stats.rejected_steps, 0, "no rejected step");
  TEST_ASSERT_EQUAL(probe.evaluations, 4, "four law evaluations: no first-step probe");
  for (int i = 0; i < system.num_entries; i++) {
    TEST_ASSERT(system.state[i] == 0.0, "every entry must stay exactly zero");
  }
  return TEST_PASS;
}

/**
 * @test    An initially empty reservoir fed by a source meets its closed form
 *
 * s = 0.4, sink k = 1.3, Delta = 3, relative tolerance 1e-8. Expected: R =
 * (s/k)(1 - e^(-k Delta)) within 10 rtol; every step finite and positive, and
 * every error estimate with a finite norm and a finite positive smallest weight.
 */
static int test_empty_reservoir_fed_by_source(void) {
  const double s = 0.4;
  const double k = 1.3;
  struct System system;
  struct LawProbe probe = law_probe(constant_law);
  struct RateIntegratorSettings settings = adaptive_settings(1e-8);
  struct AttemptLog log;
  struct Run run;
  system_init(&system, 1, 1);
  system.delta[0] = 3.0;
  const int r = add_entry(&system, 0, 0, 0.0);
  add_flux(&system, RATE_FLUX_SOURCE, RATE_END_EXTERNAL, r, 0, s);
  add_flux(&system, RATE_FLUX_SINK, r, RATE_END_EXTERNAL, 1, k);
  attach_log(&settings, &log, &system);
  TEST_ASSERT(run_segment(&system, &probe, &settings, &run) == 0, "workspace too small");
  TEST_ASSERT(run.cause == RATE_CAUSE_NONE, "empty reservoir with a source must integrate");
  const double expected = (s / k) * (1.0 - exp(-k * 3.0));
  const double error = fabs(system.state[r] - expected);
  printf("\n  empty reservoir R=%.12f closed form=%.12f error=%.3e (<=1e-7) attempts=%lld "
         "estimates=%lld\n",
         system.state[r], expected, error, (long long)log.count, (long long)log.weighed);
  TEST_ASSERT(error <= 10.0 * 1e-8, "R must meet its closed form within 10 rtol");
  TEST_ASSERT(!log.bad_step, "every step must be finite and positive");
  TEST_ASSERT(log.weighed > 0, "the error estimate must have run");
  TEST_ASSERT(!log.bad_weight, "every smallest weight must be finite and positive");
  TEST_ASSERT(!log.non_finite_error, "every error norm must be finite");
  return TEST_PASS;
}

/* ---------------------------------------------------------------------------
 * Failures
 * ------------------------------------------------------------------------- */

/** One failing segment: how to build it and what its record must name. */
struct FailureCase {
  const char *name;
  void (*build)(struct System *system, struct LawProbe *probe,
                struct RateIntegratorSettings *settings);
  enum RateCause cause;
  int entry; /* expected failure.entry, -1 for none */
  int flux;  /* expected failure.flux, -1 for none */
};

static void build_inverse_sink(struct System *system, struct LawProbe *probe,
                               struct RateIntegratorSettings *settings) {
  system_init(system, 1, 1);
  system->delta[0] = 1.0;
  const int x = add_entry(system, 0, 0, 0.5);
  add_flux(system, RATE_FLUX_SINK, x, RATE_END_EXTERNAL, 0, 0.0);
  *probe = law_probe(inverse_law);
  *settings = adaptive_settings(1e-4);
}

/** A (1) to B at 1, B to a sink at 0.5, Delta = 1; the sink's law turns bad at tau = 0.5. */
static void build_faulty(struct System *system, struct LawProbe *probe,
                         struct RateIntegratorSettings *settings, double bad_value) {
  system_init(system, 1, 1);
  system->delta[0] = 1.0;
  const int a = add_entry(system, 0, 0, 1.0);
  const int b = add_entry(system, 0, 0, 0.0);
  add_flux(system, RATE_FLUX_INTERNAL, a, b, 0, 1.0);
  add_flux(system, RATE_FLUX_SINK, b, RATE_END_EXTERNAL, 1, 0.5);
  *probe = law_probe(faulty_law);
  probe->bad_value = bad_value;
  *settings = adaptive_settings(1e-4);
}

static void build_nan_coefficient(struct System *system, struct LawProbe *probe,
                                  struct RateIntegratorSettings *settings) {
  build_faulty(system, probe, settings, NAN);
}

static void build_negative_coefficient(struct System *system, struct LawProbe *probe,
                                       struct RateIntegratorSettings *settings) {
  build_faulty(system, probe, settings, -1.0);
}

static void build_infinite_coefficient(struct System *system, struct LawProbe *probe,
                                       struct RateIntegratorSettings *settings) {
  build_faulty(system, probe, settings, INFINITY);
}

static void build_failing_law(struct System *system, struct LawProbe *probe,
                              struct RateIntegratorSettings *settings) {
  build_faulty(system, probe, settings, 0.0);
  probe->fill = failing_law;
}

static void build_budgeted_stiff_pair(struct System *system, struct LawProbe *probe,
                                      struct RateIntegratorSettings *settings) {
  build_stiff_pair(system, 1e6);
  *probe = law_probe(constant_law);
  *settings = adaptive_settings(1e-6);
  settings->max_steps_per_segment = 1000;
}

static void build_divergent_sink(struct System *system, struct LawProbe *probe,
                                 struct RateIntegratorSettings *settings) {
  system_init(system, 1, 1);
  system->delta[0] = 1.0;
  const int x = add_entry(system, 0, 0, 0.5);
  add_flux(system, RATE_FLUX_SOURCE, RATE_END_EXTERNAL, x, 0, 0.0);
  add_flux(system, RATE_FLUX_SINK, x, RATE_END_EXTERNAL, 1, 0.0);
  *probe = law_probe(divergent_law);
  *settings = adaptive_settings(1e-4);
  settings->max_rejections_per_step = 5;
}

/** The linear chain over Delta = 20 in one fixed step: step * K_max = 20 > C. */
static void build_fixed_step_too_long(struct System *system, struct LawProbe *probe,
                                      struct RateIntegratorSettings *settings) {
  build_chain(system, 20.0);
  *probe = law_probe(constant_law);
  *settings = fixed_settings(1);
}

static const struct FailureCase FAILURE_CASES[] = {
    {"sink at c/x", build_inverse_sink, RATE_CAUSE_STEP_UNDERFLOW, 0, 0},
    {"NaN coefficient", build_nan_coefficient, RATE_CAUSE_BAD_COEFFICIENT, 1, 1},
    {"negative coefficient", build_negative_coefficient, RATE_CAUSE_BAD_COEFFICIENT, 1, 1},
    {"infinite coefficient", build_infinite_coefficient, RATE_CAUSE_BAD_COEFFICIENT, 1, 1},
    {"stiff pair 1e6, 1000 steps", build_budgeted_stiff_pair, RATE_CAUSE_STEP_BUDGET, 1, 1},
    {"divergent sink, 5 rejections", build_divergent_sink, RATE_CAUSE_REJECTION_BUDGET, 0, 1},
    {"fixed step past the bound", build_fixed_step_too_long, RATE_CAUSE_REJECTION_BUDGET, 0, 0},
    {"law reports failure", build_failing_law, RATE_CAUSE_BAD_COEFFICIENT, -1, -1},
};
#define NUM_FAILURE_CASES ((int)(sizeof(FAILURE_CASES) / sizeof(FAILURE_CASES[0])))

/** Run one failure case; @p unchanged reports whether the state and amounts kept their bytes. */
static int run_failure_case(const struct FailureCase *fc, struct Run *run, int *unchanged) {
  struct System system;
  struct LawProbe probe;
  struct RateIntegratorSettings settings;
  fc->build(&system, &probe, &settings);
  double before[MAX_ENTRIES];
  memcpy(before, system.state, sizeof(before));
  for (int t = 0; t < MAX_TOTALS; t++) {
    run->source[t] = -1.0;
    run->sink[t] = -1.0;
  }
  if (run_segment(&system, &probe, &settings, run) != 0) {
    return 1;
  }
  *unchanged = memcmp(before, system.state, sizeof(before)) == 0;
  for (int t = 0; t < system.num_totals; t++) {
    if (run->source[t] != -1.0 || run->sink[t] != -1.0) {
      *unchanged = 0;
    }
  }
  printf("\n  %-30s cause=%-16s entry=%d flux=%d row=%d tau=%.6f step=%.3e accepted=%lld", fc->name,
         rate_cause_name(run->cause), run->outcome.failure.entry, run->outcome.failure.flux,
         run->outcome.failure.row, run->outcome.failure.tau, run->outcome.failure.step,
         (long long)run->outcome.stats.accepted_steps);
  return 0;
}

/** The failure case named @p name, or NULL. */
static const struct FailureCase *failure_case(const char *name) {
  for (int c = 0; c < NUM_FAILURE_CASES; c++) {
    if (strcmp(FAILURE_CASES[c].name, name) == 0) {
      return &FAILURE_CASES[c];
    }
  }
  return NULL;
}

/**
 * @test    The named failures: step_underflow, bad_coefficient (NaN, negative,
 *          infinite) and step_budget
 *
 * A sink at c/x (x0 = 0.5, c = 1, Delta = 1) empties x in finite time, so its
 * steps shrink below 2^-46; the faulty laws turn bad at tau = 0.5; the stiff
 * pair at kf = kb = 1e6 needs about 1.7e6 steps against a budget of 1000.
 * Expected: each returns its cause, and its record carries the same cause.
 */
static int test_failure_causes(void) {
  const char *const names[] = {"sink at c/x", "NaN coefficient", "negative coefficient",
                               "infinite coefficient", "stiff pair 1e6, 1000 steps"};
  for (int n = 0; n < 5; n++) {
    const struct FailureCase *fc = failure_case(names[n]);
    struct Run run;
    int unchanged = 0;
    TEST_ASSERT(fc != NULL, "every named failure case must exist");
    TEST_ASSERT(run_failure_case(fc, &run, &unchanged) == 0, "workspace too small");
    TEST_ASSERT(run.cause == fc->cause, fc->name);
    TEST_ASSERT(run.outcome.failure.cause == fc->cause,
                "the failure record must carry the returned cause");
  }
  printf("\n");
  return TEST_PASS;
}

/**
 * @test    A sink whose coefficient diverges above 0.5 exhausts the rejection budget
 *
 * A reservoir at 0.5 fed by a source of 1, sink 10/(x - 0.5) above 0.5 and zero
 * at or below it, Delta = 1, max_rejections_per_step 5: the stage coefficient
 * grows as the step shrinks, so every retry is rejected. Expected: cause
 * rejection_budget, carried by the record.
 */
static int test_rejection_budget(void) {
  const struct FailureCase *fc = failure_case("divergent sink, 5 rejections");
  struct Run run;
  int unchanged = 0;
  TEST_ASSERT(fc != NULL, "the divergent-sink failure case must exist");
  TEST_ASSERT(run_failure_case(fc, &run, &unchanged) == 0, "workspace too small");
  TEST_ASSERT(run.cause == RATE_CAUSE_REJECTION_BUDGET, "the divergent sink must fail with "
                                                        "rejection_budget");
  TEST_ASSERT(run.outcome.failure.cause == RATE_CAUSE_REJECTION_BUDGET,
              "the failure record must carry rejection_budget");
  printf("\n");
  return TEST_PASS;
}

/**
 * @test    Every failure leaves the caller's state unchanged and names its place
 *
 * Expected for every failure case: the state and the external-amount arrays
 * keep their bytes, and the record names the cause, the entry and the flux
 * (none for a law that reports failure as a whole).
 */
static int test_failures_leave_state_unchanged(void) {
  for (int c = 0; c < NUM_FAILURE_CASES; c++) {
    const struct FailureCase *fc = &FAILURE_CASES[c];
    struct Run run;
    int unchanged = 0;
    TEST_ASSERT(run_failure_case(fc, &run, &unchanged) == 0, "workspace too small");
    TEST_ASSERT(run.cause == fc->cause, fc->name);
    TEST_ASSERT(unchanged, "a failure must leave the state and amounts bitwise unchanged");
    TEST_ASSERT_EQUAL(run.outcome.failure.entry, fc->entry, "the record must name the entry");
    TEST_ASSERT_EQUAL(run.outcome.failure.flux, fc->flux, "the record must name the flux");
    TEST_ASSERT(strcmp(rate_cause_name(run.outcome.failure.cause), "none") != 0,
                "the record must name a cause");
  }
  printf("\n");
  return TEST_PASS;
}

int main(void) {
  printf("%s", BLUE);
  printf("============================================================\n");
  printf("Test Suite: Explicit Rate Integrator\n");
  printf("============================================================\n");
  printf("%s\n", NC);

  initialize_error_handling(LOG_LEVEL_WARNING, NULL);

  TEST_RUN(test_linear_chain_fixed_steps_third_order);
  TEST_RUN(test_linear_chain_adaptive_tolerance_ratio);
  TEST_RUN(test_closed_cycle_total_within_tolerance);
  TEST_RUN(test_three_sinks_split_in_coefficient_ratio);
  TEST_RUN(test_two_fast_sinks_stay_non_negative);
  TEST_RUN(test_three_sinks_order_independent);
  TEST_RUN(test_source_sink_external_amounts);
  TEST_RUN(test_stiff_pair_steps_at_positivity_bound);
  TEST_RUN(test_bimolecular_fixed_steps_order_and_invariants);
  TEST_RUN(test_threshold_fixed_steps_second_order);
  TEST_RUN(test_threshold_adaptive_tolerance_ratio);
  TEST_RUN(test_two_intervals_piecewise_closed_form);
  TEST_RUN(test_second_interval_depends_on_start_only);
  TEST_RUN(test_star_closed_form);
  TEST_RUN(test_failure_causes);
  TEST_RUN(test_rejection_budget);
  TEST_RUN(test_source_driven_sink_stays_non_negative);
  TEST_RUN(test_mixed_delta_cross_record_conserves);
  TEST_RUN(test_zero_delta_row);
  TEST_RUN(test_all_empty_component_single_step);
  TEST_RUN(test_empty_reservoir_fed_by_source);
  TEST_RUN(test_closure_check_detects_imbalance);
  TEST_RUN(test_star_repeatable);
  TEST_RUN(test_failures_leave_state_unchanged);

  TEST_SUMMARY();
  return TEST_RESULT();
}
