/**
 * @file    test_rate_integrator.c
 * @brief   Proving problems for the explicit rate integrator and its segment closure
 *
 * Validates: rate_integrate_segment() and rate_check_closure() on hand-built
 * components: measured convergence orders (one of them on a time-dependent
 * law, which pins the stage times) and tolerance ratios, conservation within
 * tol_F, positivity, canonical-order independence, mixed and zero row Delta
 * values, the starting step, near-empty and draining states, the controller,
 * repeatability, and every named failure with the caller's state left
 * untouched.
 *
 * Every expected value comes from a closed form written in this file, never
 * from the integrator. Every system's values are literals here; the star's
 * random satellites come from the fixed-seed SplitMix64 generator of
 * rate_test_support.h. Times are in normalised segment time tau, so a row with
 * segment length Delta has evolved for t = Delta at tau = 1. The error of a
 * problem is the largest absolute difference between a computed entry and its
 * closed form at tau = 1. Unless a case says otherwise the absolute tolerance
 * is 1e-3 times the relative tolerance. Each case prints the measured figures
 * beside its bound.
 */

#include "../framework/test_framework.h"
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

/* ---------------------------------------------------------------------------
 * Laws (rate_test_support.h, struct Probe): each overwrites the coefficients it
 * owns after the constant coefficients are set
 * ------------------------------------------------------------------------- */

/**
 * Each sink drains its constant in System.coefficient as an amount per unit
 * time (coefficient c / x for the donor's content x).
 */
static int constant_outflow_law(const struct Probe *probe, double tau, const double *x, double *k) {
  (void)tau;
  for (int f = 0; f < probe->system->num_fluxes; f++) {
    const struct RateFlux *flux = &probe->system->fluxes[f];
    if (flux->kind == RATE_FLUX_SINK) {
      k[f] = probe->system->coefficient[f] / x[flux->from];
    }
  }
  return 0;
}

/** Closed cycle: A to B at 1 + 0.5 B, B to C at 2 / (1 + C), C to A at 0.7 + 0.3 A^2. */
static int cycle_law(const struct Probe *probe, double tau, const double *x, double *k) {
  (void)probe;
  (void)tau;
  k[0] = 1.0 + 0.5 * x[1];
  k[1] = 2.0 / (1.0 + x[2]);
  k[2] = 0.7 + 0.3 * x[0] * x[0];
  return 0;
}

/** Bimolecular: A to C at 2 B, B to C at 2 A. */
static int bimolecular_law(const struct Probe *probe, double tau, const double *x, double *k) {
  (void)probe;
  (void)tau;
  k[0] = 2.0 * x[1];
  k[1] = 2.0 * x[0];
  return 0;
}

/** Threshold: a source of 1 into G; G to S at 3 max(0, G - 1) / G. */
static int threshold_law(const struct Probe *probe, double tau, const double *x, double *k) {
  (void)probe;
  (void)tau;
  k[0] = 1.0;
  k[1] = (x[0] > 1.0) ? 3.0 * (x[0] - 1.0) / x[0] : 0.0;
  return 0;
}

/** A sink at c / x with c = 1: a constant outflow that empties x in finite time. */
static int inverse_law(const struct Probe *probe, double tau, const double *x, double *k) {
  (void)probe;
  (void)tau;
  k[0] = 1.0 / x[0];
  return 0;
}

/** A source of 1, and a sink at 10 / (x - 0.5) above 0.5 and zero at or below it. */
static int divergent_law(const struct Probe *probe, double tau, const double *x, double *k) {
  (void)probe;
  (void)tau;
  k[0] = 1.0;
  k[1] = (x[0] > 0.5) ? 10.0 / (x[0] - 0.5) : 0.0;
  return 0;
}

/** A sink whose coefficient is 1 + 3 tau^2: x' = -Delta (1 + 3 tau^2) x in tau units. */
static int time_dependent_law(const struct Probe *probe, double tau, const double *x, double *k) {
  (void)probe;
  (void)x;
  k[0] = 1.0 + 3.0 * tau * tau;
  return 0;
}

/** K_max of the coupled sink (Delta = 2): only A has an outflow, at 1 + 50 B. */
static double coupled_sink_k(const double *x) {
  return (x[0] != 0.0) ? 2.0 * (1.0 + 50.0 * x[1]) : 0.0;
}

/** A source of 10 into B, and a sink out of A at 1 + 50 B. */
static int coupled_sink_law(const struct Probe *probe, double tau, const double *x, double *k) {
  (void)probe;
  (void)tau;
  k[0] = 10.0;
  k[1] = 1.0 + 50.0 * x[1];
  return 0;
}

/** Constant coefficients until tau = 0.5, then flux 1 returns the probe's bad value. */
static int faulty_law(const struct Probe *probe, double tau, const double *x, double *k) {
  (void)x;
  if (tau >= 0.5) {
    k[1] = probe->bad_value;
  }
  return 0;
}

/** Constant coefficients until tau = 0.5, then the law reports failure. */
static int failing_law(const struct Probe *probe, double tau, const double *x, double *k) {
  (void)probe;
  (void)x;
  (void)k;
  return (tau >= 0.5) ? 1 : 0;
}

/** A sink at c / x, c = 1, that returns +infinity at an empty donor. */
static int empty_donor_law(const struct Probe *probe, double tau, const double *x, double *k) {
  (void)probe;
  (void)tau;
  k[0] = (x[0] > 0.0) ? 1.0 / x[0] : INFINITY;
  return 0;
}

static double max3(double a, double b, double c) { return fmax(a, fmax(b, c)); }

/**
 * The controller's step factor written from its rule: 0.9 err^(-1/3) clamped to
 * [0.2, 5], or 5 at err = 0.
 */
static double controller_factor(double err) {
  if (err == 0.0) {
    return 5.0;
  }
  return fmin(5.0, fmax(0.2, 0.9 / cbrt(err)));
}

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
  add_flux(system, RATE_FLUX_INTERNAL, a, b, 0, -1, 1.0);
  add_flux(system, RATE_FLUX_INTERNAL, b, c, 1, -1, 0.3);
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
  add_flux(system, RATE_FLUX_INTERNAL, a, c, 0, -1, 0.0);
  add_flux(system, RATE_FLUX_INTERNAL, b, c, 1, -1, 0.0);
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
  add_flux(system, RATE_FLUX_SOURCE, RATE_END_EXTERNAL, g, 0, -1, 0.0);
  add_flux(system, RATE_FLUX_INTERNAL, g, s, 1, -1, 0.0);
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
  add_flux(system, RATE_FLUX_INTERNAL, a, b, 0, -1, k);
  add_flux(system, RATE_FLUX_INTERNAL, b, a, 1, -1, k);
  add_flux(system, RATE_FLUX_SINK, b, RATE_END_EXTERNAL, 2, -1, 1.0);
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
  add_flux(system, RATE_FLUX_INTERNAL, h, cold, 0, -1, 0.9);
  for (int s = 1; s <= satellites; s++) {
    star->satellite_delta[s] = uniform_in(&seed, 1.0, 1.5);
    star->k_to_central[s] = uniform_in(&seed, 1.0, 2.0);
    star->k_to_stars[s] = uniform_in(&seed, 0.5, 1.0);
    star->g0[s] = uniform_in(&seed, 0.2, 1.0);
    system->delta[s] = star->satellite_delta[s];
    const int gas = add_entry(system, s, 0, star->g0[s]);
    const int stars = add_entry(system, s, 0, 0.0);
    add_flux(system, RATE_FLUX_INTERNAL, gas, h, 1, -1, star->k_to_central[s]);
    add_flux(system, RATE_FLUX_INTERNAL, gas, stars, 2, -1, star->k_to_stars[s]);
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
    struct Probe probe = law_probe(NULL);
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
    struct Probe probe = law_probe(NULL);
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
    struct Probe probe = law_probe(bimolecular_law);
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
    struct Probe probe = law_probe(threshold_law);
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
    struct Probe probe = law_probe(threshold_law);
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
    struct Probe probe = law_probe(cycle_law);
    const struct RateIntegratorSettings settings = fixed_settings(steps[j]);
    struct Run run;
    system_init(&system, 1, 1);
    system.delta[0] = 50.0;
    const int a = add_entry(&system, 0, 0, 0.5);
    const int b = add_entry(&system, 0, 0, 0.3);
    const int c = add_entry(&system, 0, 0, 0.2);
    add_flux(&system, RATE_FLUX_INTERNAL, a, b, 0, -1, 0.0);
    add_flux(&system, RATE_FLUX_INTERNAL, b, c, 1, -1, 0.0);
    add_flux(&system, RATE_FLUX_INTERNAL, c, a, 2, -1, 0.0);
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
    struct Probe probe = law_probe(NULL);
    const struct RateIntegratorSettings settings = fixed_settings(steps[j]);
    struct Run run;
    system_init(&system, 1, 1);
    system.delta[0] = delta;
    const int r = add_entry(&system, 0, 0, r0);
    add_flux(&system, RATE_FLUX_SOURCE, RATE_END_EXTERNAL, r, 0, -1, s);
    add_flux(&system, RATE_FLUX_SINK, r, RATE_END_EXTERNAL, 1, -1, k);
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
  struct Probe probe = law_probe(NULL);
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
  add_flux(&system, RATE_FLUX_INTERNAL, h, c, 0, -1, 0.5);
  add_flux(&system, RATE_FLUX_INTERNAL, g1, h, 1, -1, 1.3);
  add_flux(&system, RATE_FLUX_INTERNAL, g2, h, 1, -1, 0.9);
  add_flux(&system, RATE_FLUX_SINK, c, RATE_END_EXTERNAL, 2, -1, 0.2);
  add_flux(&system, RATE_FLUX_SOURCE, RATE_END_EXTERNAL, g1, 3, -1, 0.3);
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
 * end removes it. Then an imbalance planted at 0.5 and at 2 times total 0's
 * tol_F (L = 2, N_acc = 10, three members, start magnitude 1), written here:
 * the first closes, the second fails naming total 0.
 */
static int test_closure_check_detects_imbalance(void) {
  const struct RateEntry entries[] = {{0, 0, RATE_ENTRY_RESERVOIR, -1},
                                      {0, 0, RATE_ENTRY_RESERVOIR, -1},
                                      {1, 0, RATE_ENTRY_RESERVOIR, -1},
                                      {1, 1, RATE_ENTRY_RESERVOIR, -1}};
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

  const double tol_f = closure_tolerance(2, 10, 3, (0.3 + 0.5) + 0.2, 0.0, 0.0);
  double planted[] = {0.3, 0.5, 0.2 + 0.5 * tol_f, 0.9};
  input.end_state = planted;
  input.source_amounts = NULL;
  input.sink_amounts = NULL;
  printf("\n  closure tol_F=%.6e; planted at 0.5 tol_F and 2 tol_F\n", tol_f);
  TEST_ASSERT(rate_check_closure(&input, &failure) == RATE_CAUSE_NONE,
              "an imbalance of half tol_F must close");
  planted[2] = 0.2 + 2.0 * tol_f;
  TEST_ASSERT(rate_check_closure(&input, &failure) == RATE_CAUSE_LEDGER && failure.total == 0,
              "an imbalance of twice tol_F must fail the closure, naming total 0");
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
    add_flux(system, RATE_FLUX_INTERNAL, s, 1 + transfer, transfer, -1, coefficient[transfer]);
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
  struct Probe probe = law_probe(NULL);
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
    struct Probe probe = law_probe(NULL);
    struct RateIntegratorSettings settings = adaptive_settings(1e-4);
    struct AttemptLog log;
    struct Run run;
    system_init(&system, 1, 1);
    system.delta[0] = 1.0;
    const int s = add_entry(&system, 0, 0, 1.0);
    const int x = add_entry(&system, 0, 0, 0.0);
    const int y = add_entry(&system, 0, 0, 0.0);
    const enum RateFluxKind kind = external ? RATE_FLUX_SINK : RATE_FLUX_INTERNAL;
    add_flux(&system, kind, s, external ? RATE_END_EXTERNAL : x, 0, -1, 300.0);
    add_flux(&system, kind, s, external ? RATE_END_EXTERNAL : y, 1, -1, 700.0);
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
    struct Probe probe = law_probe(coupled_sink_law);
    struct RateIntegratorSettings settings = adaptive_settings(tolerances[j]);
    struct AttemptLog log;
    struct Run run;
    system_init(&system, 1, 1);
    system.delta[0] = 2.0;
    const int a = add_entry(&system, 0, 0, 1.0);
    const int b = add_entry(&system, 0, 0, 0.0);
    add_flux(&system, RATE_FLUX_SOURCE, RATE_END_EXTERNAL, b, 0, -1, 0.0);
    add_flux(&system, RATE_FLUX_SINK, a, RATE_END_EXTERNAL, 1, -1, 0.0);
    attach_log(&settings, &log, &system);
    probe.k_of_state = coupled_sink_k;
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
    struct Probe probe = law_probe(NULL);
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
    struct Probe probe = law_probe(NULL);
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
  add_flux(system, RATE_FLUX_INTERNAL, a, b, 0, -1, 0.8);
  add_flux(system, RATE_FLUX_INTERNAL, s, a, 1, -1, 1.7);
}

/** Segment 2: the satellite row is gone; row 0 holds A and B, A to B at 0.3. */
static void build_second_interval(struct System *system, double a_start, double b_start) {
  system_init(system, 1, 1);
  system->delta[0] = 1.0;
  const int a = add_entry(system, 0, 0, a_start);
  const int b = add_entry(system, 0, 0, b_start);
  add_flux(system, RATE_FLUX_INTERNAL, a, b, 0, -1, 0.3);
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
  struct Probe probe = law_probe(NULL);
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
  struct Probe probe = law_probe(NULL);
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
    struct Probe probe = law_probe(NULL);
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
  struct Probe probe = law_probe(NULL);
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
  struct Probe probe = law_probe(NULL);
  const struct RateIntegratorSettings settings = adaptive_settings(1e-8);
  struct Run run;
  system_init(&system, 2, 1);
  system.delta[0] = 0.0;
  system.delta[1] = 1.5;
  const int p = add_entry(&system, 0, 0, 0.7);
  const int q = add_entry(&system, 0, 0, 0.2);
  const int r = add_entry(&system, 0, 0, 0.0);
  const int d = add_entry(&system, 1, 0, 1.0);
  add_flux(&system, RATE_FLUX_INTERNAL, p, q, 0, -1, 2.0);
  add_flux(&system, RATE_FLUX_SOURCE, RATE_END_EXTERNAL, p, 1, -1, 0.5);
  add_flux(&system, RATE_FLUX_INTERNAL, d, r, 2, -1, 1.1);
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
  struct Probe probe = law_probe(NULL);
  const struct RateIntegratorSettings settings = adaptive_settings(1e-4);
  struct Run run;
  system_init(&system, 1, 1);
  system.delta[0] = 1.0;
  const int a = add_entry(&system, 0, 0, 0.0);
  const int b = add_entry(&system, 0, 0, 0.0);
  const int c = add_entry(&system, 0, 0, 0.0);
  add_flux(&system, RATE_FLUX_INTERNAL, a, b, 0, -1, 1.0);
  add_flux(&system, RATE_FLUX_INTERNAL, b, c, 1, -1, 2.0);
  add_flux(&system, RATE_FLUX_INTERNAL, c, a, 2, -1, 0.5);
  add_flux(&system, RATE_FLUX_SINK, c, RATE_END_EXTERNAL, 3, -1, 0.7);
  TEST_ASSERT(run_segment(&system, &probe, &settings, &run) == 0, "workspace too small");
  TEST_ASSERT(run.cause == RATE_CAUSE_NONE, "all-empty component must integrate");
  TEST_ASSERT_EQUAL(run.outcome.stats.accepted_steps, 1, "exactly one accepted step");
  TEST_ASSERT_EQUAL(run.outcome.stats.rejected_steps, 0, "no rejected step");
  TEST_ASSERT_EQUAL(run.evaluations, 4, "four law evaluations: the start and three stages");
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
  struct Probe probe = law_probe(NULL);
  struct RateIntegratorSettings settings = adaptive_settings(1e-8);
  struct AttemptLog log;
  struct Run run;
  system_init(&system, 1, 1);
  system.delta[0] = 3.0;
  const int r = add_entry(&system, 0, 0, 0.0);
  add_flux(&system, RATE_FLUX_SOURCE, RATE_END_EXTERNAL, r, 0, -1, s);
  add_flux(&system, RATE_FLUX_SINK, r, RATE_END_EXTERNAL, 1, -1, k);
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
  TEST_ASSERT(!log.non_finite, "every step, weight, error norm and committed entry must be finite");
  return TEST_PASS;
}

/* ---------------------------------------------------------------------------
 * Failures
 * ------------------------------------------------------------------------- */

/* Rules for a failure record's tau and step (struct FailureCase). */
enum TimeRule {
  TIME_EXACT,     /* tau and step equal the case's values */
  TIME_WITHIN,    /* tau in [tau, tau_high] and step in (0, step] */
  TIME_SPANS_HALF /* the failing step spans tau = 0.5, where the law turns bad: tau <= 0.5 <= */
                  /* tau + step */
};

/** One failing segment: how to build it and what its record must hold. */
struct FailureCase {
  const char *name;
  void (*build)(struct System *system, struct Probe *probe,
                struct RateIntegratorSettings *settings);
  enum RateCause cause;
  int entry; /* expected failure.entry, -1 for none */
  int flux;  /* expected failure.flux, -1 for none */
  int row;   /* expected failure.row, -1 for none */
  enum TimeRule time_rule;
  double tau;          /* TIME_EXACT: tau; TIME_WITHIN: the lowest tau */
  double tau_high;     /* TIME_WITHIN: the highest tau */
  double step;         /* TIME_EXACT: the step; TIME_WITHIN: the largest step */
  int64_t accepted;    /* expected accepted steps, or -1 when not pinned */
  int64_t rejected;    /* expected rejected steps, or -1 when not pinned */
  int64_t evaluations; /* expected law evaluations, or -1 when not pinned */
};

/**
 * A sink at c / x, c = 1, x0 = 0.6, Delta = 1: the exact x empties at tau = 0.6,
 * where the coefficient diverges. x0 is no dyadic fraction, so no stage lands
 * on x = 0 exactly.
 */
static void build_inverse_sink(struct System *system, struct Probe *probe,
                               struct RateIntegratorSettings *settings) {
  system_init(system, 1, 1);
  system->delta[0] = 1.0;
  const int x = add_entry(system, 0, 0, 0.6);
  add_flux(system, RATE_FLUX_SINK, x, RATE_END_EXTERNAL, 0, -1, 0.0);
  *probe = law_probe(inverse_law);
  *settings = adaptive_settings(1e-4);
}

/** An empty donor whose sink law returns +infinity there (c / x at x = 0), Delta = 1. */
static void build_empty_donor(struct System *system, struct Probe *probe,
                              struct RateIntegratorSettings *settings) {
  system_init(system, 1, 1);
  system->delta[0] = 1.0;
  const int x = add_entry(system, 0, 0, 0.0);
  add_flux(system, RATE_FLUX_SINK, x, RATE_END_EXTERNAL, 0, -1, 0.0);
  *probe = law_probe(empty_donor_law);
  *settings = adaptive_settings(1e-4);
}

/** A (1) to B at 1, B to a sink at 0.5, Delta = 1; the sink's law turns bad at tau = 0.5. */
static void build_faulty(struct System *system, struct Probe *probe,
                         struct RateIntegratorSettings *settings, double bad_value) {
  system_init(system, 1, 1);
  system->delta[0] = 1.0;
  const int a = add_entry(system, 0, 0, 1.0);
  const int b = add_entry(system, 0, 0, 0.0);
  add_flux(system, RATE_FLUX_INTERNAL, a, b, 0, -1, 1.0);
  add_flux(system, RATE_FLUX_SINK, b, RATE_END_EXTERNAL, 1, -1, 0.5);
  *probe = law_probe(faulty_law);
  probe->bad_value = bad_value;
  *settings = adaptive_settings(1e-4);
}

static void build_nan_coefficient(struct System *system, struct Probe *probe,
                                  struct RateIntegratorSettings *settings) {
  build_faulty(system, probe, settings, NAN);
}

static void build_negative_coefficient(struct System *system, struct Probe *probe,
                                       struct RateIntegratorSettings *settings) {
  build_faulty(system, probe, settings, -1.0);
}

static void build_infinite_coefficient(struct System *system, struct Probe *probe,
                                       struct RateIntegratorSettings *settings) {
  build_faulty(system, probe, settings, INFINITY);
}

static void build_failing_law(struct System *system, struct Probe *probe,
                              struct RateIntegratorSettings *settings) {
  build_faulty(system, probe, settings, 0.0);
  probe->fill = failing_law;
}

static void build_budgeted_stiff_pair(struct System *system, struct Probe *probe,
                                      struct RateIntegratorSettings *settings) {
  build_stiff_pair(system, 1e6);
  *probe = law_probe(NULL);
  *settings = adaptive_settings(1e-6);
  settings->max_steps_per_segment = 1000;
}

static void build_divergent_sink(struct System *system, struct Probe *probe,
                                 struct RateIntegratorSettings *settings) {
  system_init(system, 1, 1);
  system->delta[0] = 1.0;
  const int x = add_entry(system, 0, 0, 0.5);
  add_flux(system, RATE_FLUX_SOURCE, RATE_END_EXTERNAL, x, 0, -1, 0.0);
  add_flux(system, RATE_FLUX_SINK, x, RATE_END_EXTERNAL, 1, -1, 0.0);
  *probe = law_probe(divergent_law);
  *settings = adaptive_settings(1e-4);
  settings->max_rejections_per_step = 5;
}

/** The linear chain over Delta = 20 in one fixed step: step * K_max = 20 > C. */
static void build_fixed_step_too_long(struct System *system, struct Probe *probe,
                                      struct RateIntegratorSettings *settings) {
  build_chain(system, 20.0);
  *probe = law_probe(NULL);
  *settings = fixed_settings(1);
}

/**
 * One entry at 1 with a constant sink at 1e15, Delta = 1: the starting step is the
 * positivity cap 0.9 * 2 / 1e15 = 1.8e-15 itself, below 2^-46.
 */
static void build_underflow_at_cap(struct System *system, struct Probe *probe,
                                   struct RateIntegratorSettings *settings) {
  system_init(system, 1, 1);
  system->delta[0] = 1.0;
  const int x = add_entry(system, 0, 0, 1.0);
  add_flux(system, RATE_FLUX_SINK, x, RATE_END_EXTERNAL, 0, -1, 1e15);
  *probe = law_probe(NULL);
  *settings = adaptive_settings(1e-4);
}

/** The linear chain over Delta = 2 asked for 2000 fixed steps against a budget of 1000. */
static void build_fixed_step_budget(struct System *system, struct Probe *probe,
                                    struct RateIntegratorSettings *settings) {
  build_chain(system, 2.0);
  *probe = law_probe(NULL);
  *settings = fixed_settings(2000);
  settings->max_steps_per_segment = 1000;
}

/**
 * A reservoir at 1 fed by a constant source of 1, Delta = 1, with a budget of no
 * attempts: no outflow (no K_max limiter, no cap clamp), so the first attempt
 * is refused before any limiter is met.
 */
static void build_unclamped_budget(struct System *system, struct Probe *probe,
                                   struct RateIntegratorSettings *settings) {
  system_init(system, 1, 1);
  system->delta[0] = 1.0;
  const int r = add_entry(system, 0, 0, 1.0);
  add_flux(system, RATE_FLUX_SOURCE, RATE_END_EXTERNAL, r, 0, -1, 1.0);
  *probe = law_probe(NULL);
  *settings = adaptive_settings(1e-4);
  settings->max_steps_per_segment = 0;
}

/* The stiff pair's cap at its start: 0.9 * 2 / K_max, K_max = Delta (kb + 1) = 3 (1e6 + 1). */
#define STIFF_BUDGET_CAP ((0.9 * 2.0) / (3.0 * (1e6 + 1.0)))

static const struct FailureCase FAILURE_CASES[] = {
    {"sink at c/x", build_inverse_sink, RATE_CAUSE_STEP_UNDERFLOW, 0, 0, 0, TIME_WITHIN, 0.0, 0.6,
     0x1p-46, -1, -1, -1},
    {"empty donor, infinite law", build_empty_donor, RATE_CAUSE_BAD_COEFFICIENT, 0, 0, 0,
     TIME_EXACT, 0.0, 0.0, 0.0, 0, 0, 1},
    {"NaN coefficient", build_nan_coefficient, RATE_CAUSE_BAD_COEFFICIENT, 1, 1, 0, TIME_SPANS_HALF,
     0.0, 0.0, 0.0, -1, -1, -1},
    {"negative coefficient", build_negative_coefficient, RATE_CAUSE_BAD_COEFFICIENT, 1, 1, 0,
     TIME_SPANS_HALF, 0.0, 0.0, 0.0, -1, -1, -1},
    {"infinite coefficient", build_infinite_coefficient, RATE_CAUSE_BAD_COEFFICIENT, 1, 1, 0,
     TIME_SPANS_HALF, 0.0, 0.0, 0.0, -1, -1, -1},
    {"stiff pair 1e6, 1000 steps", build_budgeted_stiff_pair, RATE_CAUSE_STEP_BUDGET, 1, 1, 0,
     TIME_WITHIN, 0x1p-46, 1.0, STIFF_BUDGET_CAP, -1, -1, -1},
    {"divergent sink, 5 rejections", build_divergent_sink, RATE_CAUSE_REJECTION_BUDGET, 0, 1, 0,
     TIME_WITHIN, 0.0, 0.0, 1.0, 0, 5, -1},
    {"fixed step past the bound", build_fixed_step_too_long, RATE_CAUSE_REJECTION_BUDGET, 0, 0, 0,
     TIME_EXACT, 0.0, 0.0, 1.0, 0, 1, 1},
    {"law reports failure", build_failing_law, RATE_CAUSE_BAD_COEFFICIENT, -1, -1, -1,
     TIME_SPANS_HALF, 0.0, 0.0, 0.0, -1, -1, -1},
    {"underflow at the positivity cap", build_underflow_at_cap, RATE_CAUSE_STEP_UNDERFLOW, 0, 0, 0,
     TIME_EXACT, 0.0, 0.0, (0.9 * 2.0) / 1e15, 0, 0, 1},
    {"fixed steps past the budget", build_fixed_step_budget, RATE_CAUSE_STEP_BUDGET, 0, 0, 0,
     TIME_EXACT, 0.5, 0.0, 1.0 / 2000.0, 1000, 0, 4001},
    {"budget of no attempts", build_unclamped_budget, RATE_CAUSE_STEP_BUDGET, 0, 0, 0, TIME_EXACT,
     0.0, 0.0, 1.0, 0, 0, 1},
};
#define NUM_FAILURE_CASES ((int)(sizeof(FAILURE_CASES) / sizeof(FAILURE_CASES[0])))

/** Whether @p failure's tau and step meet @p fc's time rule. */
static int failure_time_holds(const struct FailureCase *fc, const struct RateFailure *failure) {
  switch (fc->time_rule) {
  case TIME_EXACT:
    return failure->tau == fc->tau && failure->step == fc->step;
  case TIME_WITHIN:
    return failure->tau >= fc->tau && failure->tau <= fc->tau_high && failure->step > 0.0 &&
           failure->step <= fc->step;
  case TIME_SPANS_HALF:
    return failure->tau <= 0.5 && failure->tau + failure->step >= 0.5;
  }
  return 0;
}

/** Whether a pinned count @p expected (-1: not pinned) equals @p actual. */
static int count_holds(int64_t expected, int64_t actual) {
  return expected < 0 || expected == actual;
}

/**
 * @test    Every named failure returns its cause, names its place and leaves the state unchanged
 *
 * The causes: step_underflow (a sink at c / x, x0 = 0.6, emptied in finite
 * time; a positivity cap below 2^-46 at the start), bad_coefficient (a law
 * that returns +infinity at an empty donor at its first evaluation; NaN,
 * negative and infinite coefficients from tau = 0.5 on; a law that reports
 * failure), step_budget (the stiff pair at 1e6 against 1000 attempts, about
 * 1.7e6 needed; 2000 fixed steps against 1000; no attempt allowed) and
 * rejection_budget (a sink whose coefficient diverges above 0.5 against 5
 * retries; one fixed step past the positivity bound). Expected for each: the
 * returned cause and the record's cause; the state, the external amounts and
 * the flux amounts bitwise unchanged; the record's entry, flux and row; its tau
 * and step by the case's rule (a law that turns bad at tau = 0.5 fails in the
 * step spanning it); the accepted and rejected steps and law evaluations the
 * case pins; and the outcome's law_evaluations equal to the evaluations the
 * law itself counted. The cases that meet no limiter before failing (the
 * underflow at the cap, the fixed-step budget, the budget of no attempts) name
 * the start-of-step limiter: the K_max entry and its outflow, or the
 * largest-rate flux and its recipient.
 */
static int test_failure_table(void) {
  for (int c = 0; c < NUM_FAILURE_CASES; c++) {
    const struct FailureCase *fc = &FAILURE_CASES[c];
    struct System system;
    struct Probe probe;
    struct RateIntegratorSettings settings;
    static struct Run run;
    fc->build(&system, &probe, &settings);
    double before[MAX_ENTRIES];
    memcpy(before, system.state, sizeof(before));
    for (int t = 0; t < MAX_TOTALS; t++) {
      run.source[t] = -1.0;
      run.sink[t] = -1.0;
    }
    for (int f = 0; f < MAX_FLUXES; f++) {
      run.flux[f] = -1.0;
    }
    TEST_ASSERT(run_segment(&system, &probe, &settings, &run) == 0, "workspace too small");
    const struct RateFailure *failure = &run.outcome.failure;
    const struct RateStepStats *stats = &run.outcome.stats;
    int unchanged = memcmp(before, system.state, sizeof(before)) == 0;
    for (int t = 0; t < system.num_totals; t++) {
      unchanged &= (run.source[t] == -1.0 && run.sink[t] == -1.0);
    }
    for (int f = 0; f < system.num_fluxes; f++) {
      unchanged &= (run.flux[f] == -1.0);
    }
    printf("\n  %-31s cause=%-16s entry=%d flux=%d row=%d tau=%.6f step=%.3e accepted=%lld "
           "rejected=%lld evaluations=%lld",
           fc->name, rate_cause_name(run.cause), failure->entry, failure->flux, failure->row,
           failure->tau, failure->step, (long long)stats->accepted_steps,
           (long long)stats->rejected_steps, (long long)stats->law_evaluations);
    TEST_ASSERT(run.cause == fc->cause, fc->name);
    TEST_ASSERT(failure->cause == fc->cause, "the failure record must carry the returned cause");
    TEST_ASSERT(unchanged, "a failure must leave the state and every amount bitwise unchanged");
    TEST_ASSERT_EQUAL(failure->entry, fc->entry, "the record must name the entry");
    TEST_ASSERT_EQUAL(failure->flux, fc->flux, "the record must name the flux");
    TEST_ASSERT_EQUAL(failure->row, fc->row, "the record must name the row");
    TEST_ASSERT(failure_time_holds(fc, failure), "the record's tau and step break the case's rule");
    TEST_ASSERT(count_holds(fc->accepted, stats->accepted_steps), "accepted steps");
    TEST_ASSERT(count_holds(fc->rejected, stats->rejected_steps), "rejected steps");
    TEST_ASSERT(count_holds(fc->evaluations, stats->law_evaluations), "law evaluations");
    TEST_ASSERT(stats->law_evaluations == run.evaluations,
                "the outcome must count every law evaluation the law saw");
    if (fc->cause == RATE_CAUSE_STEP_BUDGET && fc->time_rule == TIME_WITHIN) {
      TEST_ASSERT(stats->accepted_steps + stats->rejected_steps == 1000,
                  "an adaptive step budget is spent on exactly its attempts");
    }
  }
  printf("\n");
  return TEST_PASS;
}

/**
 * @test    Many external fluxes into one total close within tol_F
 *
 * 1000 rows of one entry each, a transfer from each entry to the next at
 * 1e-12, every Delta 1, 10000 fixed steps, default settings: (a) every entry
 * empty and fed by a source of 1; (b) every entry at 1.25 drained by a sink of
 * a constant 1 per unit time (coefficient 1 / x). Both add the same amount,
 * 1e-4, on every step, the case where the rounding of one running sum per
 * total accumulates fastest. Each entry carries at most three flux terms (L =
 * 3) and n_F = 1000, while the total's external amount gathers 1e7 flux
 * amounts. Expected: the segment succeeds (no ledger failure), the total
 * closes within tol_F, and the external amount is 1000 within that tolerance.
 */
static int test_many_external_fluxes_close_within_tolerance(void) {
  for (int sinks = 0; sinks <= 1; sinks++) {
    static struct System system;
    struct Probe probe = law_probe(sinks ? constant_outflow_law : NULL);
    const struct RateIntegratorSettings settings = fixed_settings(10000);
    struct Run run;
    system_init(&system, 1000, 1);
    for (int r = 0; r < 1000; r++) {
      system.delta[r] = 1.0;
      add_entry(&system, r, 0, sinks ? 1.25 : 0.0);
    }
    for (int r = 0; r < 1000; r++) {
      if (sinks) {
        add_flux(&system, RATE_FLUX_SINK, r, RATE_END_EXTERNAL, 0, -1, 1.0);
      } else {
        add_flux(&system, RATE_FLUX_SOURCE, RATE_END_EXTERNAL, r, 0, -1, 1.0);
      }
      if (r + 1 < 1000) {
        add_flux(&system, RATE_FLUX_INTERNAL, r, r + 1, 1, -1, 1e-12);
      }
    }
    double start_total = 0.0;
    for (int i = 0; i < system.num_entries; i++) {
      start_total += system.state[i];
    }
    TEST_ASSERT(run_segment(&system, &probe, &settings, &run) == 0, "workspace too small");
    TEST_ASSERT(run.cause == RATE_CAUSE_NONE, "many external fluxes must not fail the ledger");
    TEST_ASSERT(run.outcome.max_terms_per_entry == 3, "L is 3 (an external and two transfers)");
    double end_total = 0.0;
    for (int i = 0; i < system.num_entries; i++) {
      end_total += system.state[i];
    }
    const double drift = fabs((end_total - start_total) - (run.source[0] - run.sink[0]));
    const double tolerance =
        closure_tolerance(3, 10000, 1000, start_total, run.source[0], run.sink[0]);
    printf("\n  many %s Src=%.12f Snk=%.12f closure=%.3e tol_F=%.3e", sinks ? "sinks  " : "sources",
           run.source[0], run.sink[0], drift, tolerance);
    TEST_ASSERT(drift <= tolerance, "many external fluxes must close within tol_F");
    const double external = sinks ? run.sink[0] : run.source[0];
    TEST_ASSERT(fabs(external - 1000.0) <= tolerance,
                "the external amount must be 1000 within tol_F");
  }
  printf("\n");
  return TEST_PASS;
}

/* ---------------------------------------------------------------------------
 * The starting step, the controller and landing
 * ------------------------------------------------------------------------- */

/** One entry x0 = 1 drained by a constant sink at @p k, Delta = 1 (K = k). */
static void build_decay(struct System *system, double k) {
  system_init(system, 1, 1);
  system->delta[0] = 1.0;
  const int x = add_entry(system, 0, 0, 1.0);
  add_flux(system, RATE_FLUX_SINK, x, RATE_END_EXTERNAL, 0, -1, k);
}

/**
 * @test    A segment starts at min(1, cap); the controller sizes the next step; the last stretches
 *
 * build_decay(), so the cap is 0.9 * 2 / k. (a) k = 1, default tolerances: the
 * cap 1.8 exceeds the segment, so the first attempt's step is 1. (b) k = 4,
 * relative tolerance 0.08: the first attempt's step is the cap 0.45 exactly
 * (the remainder 1 is more than twice it, so landing leaves it); its reported
 * error norm lies in (1, 2), so it is rejected, and the second attempt's step
 * is the first times 0.9 err^(-1/3), from that norm, bit for bit, the factor
 * within (0.2, 5) so the clamp does not decide it; the last attempt follows two
 * accepted steps and its remainder lies within (h, 1.1 h] of the step h the
 * controller proposes from the attempt before it, so its step is that
 * remainder exactly (without the stretch it would be split into two steps).
 * Neither case evaluates the law beyond its stages: 1 evaluation at the start,
 * 3 per attempt and 1 per accepted step that does not end the segment.
 */
static int test_start_controller_and_landing(void) {
  static struct AttemptLog log;
  struct System system;
  struct Run run;

  build_decay(&system, 1.0);
  struct RateIntegratorSettings settings = adaptive_settings(1e-4);
  attach_log(&settings, &log, &system);
  TEST_ASSERT(run_segment(&system, NULL, &settings, &run) == 0, "workspace too small");
  TEST_ASSERT(run.cause == RATE_CAUSE_NONE, "the decay at k = 1 must integrate");
  printf("\n  k = 1: first step %.17g, %lld attempts, %lld evaluations", log.step[0],
         (long long)log.count, (long long)run.outcome.stats.law_evaluations);
  TEST_ASSERT(log.count > 0 && log.tau[0] == 0.0 && log.step[0] == 1.0,
              "a cap above the segment must start the segment at step 1");
  TEST_ASSERT(run.outcome.stats.law_evaluations ==
                  1 + 3 * log.count + (run.outcome.stats.accepted_steps - 1),
              "the start must cost no law evaluation beyond the stages");

  build_decay(&system, 4.0);
  settings = adaptive_settings(0.08);
  attach_log(&settings, &log, &system);
  TEST_ASSERT(run_segment(&system, NULL, &settings, &run) == 0, "workspace too small");
  TEST_ASSERT(run.cause == RATE_CAUSE_NONE, "the decay at k = 4 must integrate");
  const int64_t n = log.count;
  const double cap = (0.9 * 2.0) / 4.0;
  const double factor = controller_factor(log.error_norm[0]);
  const double second = log.step[0] * factor;
  const double proposal = (n >= 3) ? log.step[n - 2] * controller_factor(log.error_norm[n - 2]) : 0;
  const double remainder = (n >= 1) ? 1.0 - log.tau[n - 1] : 0.0;
  printf(
      "\n  k = 4: first step %.17g (cap %.17g), err %.6f, second step %.17g (first x %.6f = "
      "%.17g); %lld attempts; landing remainder / proposal %.6f (in (1, 1.1]), %lld evaluations\n",
      log.step[0], cap, log.error_norm[0], log.step[1], factor, second, (long long)n,
      remainder / proposal, (long long)run.outcome.stats.law_evaluations);
  TEST_ASSERT(n >= 4 && n <= LOG_CAPACITY, "the k = 4 case takes at least four attempts");
  TEST_ASSERT(log.tau[0] == 0.0 && log.step[0] == cap, "the first step must be the cap exactly");
  TEST_ASSERT(!log.accepted[0] && log.error_norm[0] > 1.0 && log.error_norm[0] < 2.0,
              "the first attempt must be rejected with an error norm in (1, 2)");
  TEST_ASSERT(factor > 0.2 && factor < 5.0, "the case must leave the controller unclamped");
  TEST_ASSERT(log.tau[1] == 0.0 && log.step[1] == second,
              "the second step must be the first times 0.9 err^(-1/3) of the reported err");
  TEST_ASSERT(log.accepted[n - 3] && log.accepted[n - 2] && log.accepted[n - 1],
              "the landing must follow two accepted steps");
  TEST_ASSERT(remainder > proposal && remainder <= 1.1 * proposal,
              "the remainder must lie within (h, 1.1 h] of the proposal");
  TEST_ASSERT(log.step[n - 1] == remainder, "the last step must be the remainder 1 - tau exactly");
  TEST_ASSERT(run.outcome.stats.positivity_rejections == 0 &&
                  run.outcome.stats.law_evaluations ==
                      1 + 3 * log.count + (run.outcome.stats.accepted_steps - 1),
              "the start must cost no law evaluation beyond the stages");
  return TEST_PASS;
}

/* ---------------------------------------------------------------------------
 * Near-empty and draining states
 * ------------------------------------------------------------------------- */

/* The near-empty starts: exactly empty, then from the smallest normal scale up. */
static const double NEAR_EMPTY_STARTS[] = {0.0, 1e-300, 1e-20, 1e-14, 1e-8};
#define NUM_NEAR_EMPTY_STARTS ((int)(sizeof(NEAR_EMPTY_STARTS) / sizeof(NEAR_EMPTY_STARTS[0])))

/** (a) One reservoir at @p x0 with a source of 0.4 and a sink at 1.3, Delta = 3. */
static void build_fed_reservoir(struct System *system, double x0) {
  system_init(system, 1, 1);
  system->delta[0] = 3.0;
  const int r = add_entry(system, 0, 0, x0);
  add_flux(system, RATE_FLUX_SOURCE, RATE_END_EXTERNAL, r, 0, -1, 0.4);
  add_flux(system, RATE_FLUX_SINK, r, RATE_END_EXTERNAL, 1, -1, 1.3);
}

/** (a)'s closed form: R = s/k + (x0 - s/k) e^(-k Delta). */
static double fed_reservoir_exact(double x0) {
  const double equilibrium = 0.4 / 1.3;
  return equilibrium + (x0 - equilibrium) * exp(-1.3 * 3.0);
}

/**
 * (b) A central (row 0, Delta 1.25) holding H = @p x0 and Cold = 0, H to Cold at
 * 0.9, fed by a satellite (row 1, Delta 1) holding G = 0.8 and Stars = 0, G to
 * the central's H at 1.7 (cross-record) and to its own Stars at 0.6.
 */
static void build_fed_central(struct System *system, double x0) {
  system_init(system, 2, 1);
  system->delta[0] = 1.25;
  system->delta[1] = 1.0;
  const int h = add_entry(system, 0, 0, x0);
  const int cold = add_entry(system, 0, 0, 0.0);
  const int gas = add_entry(system, 1, 0, 0.8);
  const int stars = add_entry(system, 1, 0, 0.0);
  add_flux(system, RATE_FLUX_INTERNAL, h, cold, 0, -1, 0.9);
  add_flux(system, RATE_FLUX_INTERNAL, gas, stars, 1, -1, 0.6);
  add_flux(system, RATE_FLUX_INTERNAL, gas, h, 2, -1, 1.7);
}

/** (b)'s closed form: H = x0 e^(-b) + Delta_s k_s G0 (e^(-a) - e^(-b)) / (b - a). */
static double fed_central_exact(double x0) {
  const double a = (1.7 + 0.6) * 1.0;
  const double b = 0.9 * 1.25;
  return x0 * exp(-b) + 1.0 * 1.7 * 0.8 * (exp(-a) - exp(-b)) / (b - a);
}

/** Drift of total 0 against tol_F (L = 2, a literal: every entry here has at most two terms). */
static double closure_ratio(const struct System *system, const double *start,
                            const struct Run *run) {
  double start_sum = 0.0;
  double end_sum = 0.0;
  double magnitude = 0.0;
  for (int i = 0; i < system->num_entries; i++) {
    start_sum += start[i];
    end_sum += system->state[i];
    magnitude += fabs(start[i]);
  }
  const double drift = fabs((end_sum - start_sum) - (run->source[0] - run->sink[0]));
  const double tolerance =
      closure_tolerance(2, run->outcome.stats.accepted_steps, system->num_entries, magnitude,
                        run->source[0], run->sink[0]);
  return (drift == 0.0) ? 0.0 : drift / tolerance;
}

/**
 * @test    A nearly empty reservoir fed from outside integrates from tau = 0, under both schemes
 *
 * For starts 0, 1e-300, 1e-20, 1e-14 and 1e-8: (a) one reservoir with a source
 * of 0.4 and a sink at 1.3, Delta = 3; (b) a satellite feeding a central whose
 * receiving reservoir holds the start (build_fed_central()). Explicit scheme at
 * relative tolerance 1e-8: the closed form within 10 times the relative
 * tolerance. Implicit scheme at the default tolerances: the closed form within
 * 1e-2 relative (the first-order scheme measured 1.5e-3). Expected for every
 * row: the segment completes (no step_underflow), meets its closed form, and
 * its total closes within tol_F.
 */
static int test_near_empty_start(void) {
  int failures = 0;
  for (int implicit = 0; implicit <= 1; implicit++) {
    for (int problem = 0; problem < 2; problem++) {
      for (int j = 0; j < NUM_NEAR_EMPTY_STARTS; j++) {
        const double x0 = NEAR_EMPTY_STARTS[j];
        struct System system;
        struct RateIntegratorSettings settings;
        rate_integrator_default_settings(&settings);
        if (implicit) {
          settings.scheme = RATE_SCHEME_IMPLICIT;
        } else {
          settings = adaptive_settings(1e-8);
        }
        if (problem == 0) {
          build_fed_reservoir(&system, x0);
        } else {
          build_fed_central(&system, x0);
        }
        double start[MAX_ENTRIES];
        memcpy(start, system.state, sizeof(start));
        static struct Run run;
        TEST_ASSERT(run_segment(&system, NULL, &settings, &run) == 0, "workspace too small");
        const double exact = (problem == 0) ? fed_reservoir_exact(x0) : fed_central_exact(x0);
        const double relative = fabs(system.state[0] - exact) / exact;
        const double bound = implicit ? 1e-2 : 10.0 * 1e-8 / exact;
        const double closure = closure_ratio(&system, start, &run);
        const int ok = run.cause == RATE_CAUSE_NONE && relative <= bound && closure <= 1.0;
        printf("\n  %s (%c) x0=%-7.0e %-14s accepted=%-4lld rejected=%-3lld relative error=%.3e "
               "(<=%.1e) closure=%.3f of tol_F%s",
               implicit ? "implicit" : "explicit", problem == 0 ? 'a' : 'b', x0,
               rate_cause_name(run.cause), (long long)run.outcome.stats.accepted_steps,
               (long long)run.outcome.stats.rejected_steps, relative, bound, closure,
               ok ? "" : "  <- FAILED");
        failures += !ok;
      }
    }
  }
  printf("\n");
  TEST_ASSERT(failures == 0, "a near-empty start failed, missed its closed form or did not close");
  return TEST_PASS;
}

/**
 * @test    A draining sink is followed to an absolute accuracy, not through every decade
 *
 * One entry x0 = 1 drained at 700, Delta = 1, default tolerances: the content
 * falls through about 300 decades. Its weight's scale keeps the segment-start
 * total, so the error control asks for absolute accuracy once the content is
 * small, and the positivity cap governs the steps. Expected: the segment
 * completes in at most 1.25 N_pos accepted steps, N_pos = ceil(700 / (0.9 x
 * 2)) = 389 (the content's relative error followed through every decade took
 * 5,748); the end within 10 x absolute_tolerance of e^-700; the total closes
 * within tol_F (L = 1).
 */
static int test_draining_sink(void) {
  struct System system;
  struct Run run;
  build_decay(&system, 700.0);
  struct RateIntegratorSettings settings;
  rate_integrator_default_settings(&settings);
  TEST_ASSERT(run_segment(&system, NULL, &settings, &run) == 0, "workspace too small");
  const double n_pos = ceil(700.0 / (0.9 * 2.0));
  const double accepted = (double)run.outcome.stats.accepted_steps;
  const double error = fabs(system.state[0] - exp(-700.0));
  const double drift = fabs((system.state[0] - 1.0) + run.sink[0]);
  const double tolerance =
      closure_tolerance(1, run.outcome.stats.accepted_steps, 1, 1.0, 0.0, run.sink[0]);
  printf("\n  sink k Delta = 700: %s, accepted=%.0f (<= 1.25 N_pos = %.2f), rejected=%lld, "
         "|x - e^-700|=%.3e (<= 1e-6), closure %.3e (<= %.3e)\n",
         rate_cause_name(run.cause), accepted, 1.25 * n_pos,
         (long long)run.outcome.stats.rejected_steps, error, drift, tolerance);
  TEST_ASSERT(run.cause == RATE_CAUSE_NONE, "the draining sink must integrate");
  TEST_ASSERT(accepted <= 1.25 * n_pos, "the draining sink must take at most 1.25 N_pos steps");
  TEST_ASSERT(system.state[0] >= 0.0 && error <= 10.0 * 1e-7,
              "the drained entry must end within 10 absolute tolerances of e^-700");
  TEST_ASSERT(drift <= tolerance, "the drained total must close within tol_F");
  return TEST_PASS;
}

/* ---------------------------------------------------------------------------
 * Time dependence
 * ------------------------------------------------------------------------- */

/**
 * @test    A time-dependent law converges at third order at fixed steps
 *
 * x' = -Delta (1 + 3 tau^2) x in tau units (a sink whose coefficient is 1 +
 * 3 tau^2), Delta = 2, x0 = 1, so x(1) = e^(-2 Delta). Every other proving
 * problem is autonomous, so this case alone pins the stage times: a stage
 * evaluated at the wrong tau drops the order. Expected: |p - 3| <= 0.2 on each
 * halving from n = 16 to 256.
 */
static int test_time_dependent_fixed_steps_third_order(void) {
  const int64_t steps[] = {16, 32, 64, 128, 256};
  double error[5];
  for (int j = 0; j < 5; j++) {
    struct System system;
    struct Probe probe = law_probe(time_dependent_law);
    const struct RateIntegratorSettings settings = fixed_settings(steps[j]);
    struct Run run;
    build_decay(&system, 0.0);
    system.delta[0] = 2.0;
    TEST_ASSERT(run_segment(&system, &probe, &settings, &run) == 0, "workspace too small");
    TEST_ASSERT(run.cause == RATE_CAUSE_NONE, "the time-dependent decay must integrate");
    error[j] = fabs(system.state[0] - exp(-2.0 * 2.0));
    printf("\n  time-dependent n=%-4lld error=%.4e", (long long)steps[j], error[j]);
  }
  for (int j = 0; j < 4; j++) {
    const double p = log2(error[j] / error[j + 1]);
    printf("\n  time-dependent halving %lld->%lld p=%.4f (|p-3|<=0.2)", (long long)steps[j],
           (long long)steps[j + 1], p);
    TEST_ASSERT(fabs(p - 3.0) <= 0.2, "the time-dependent order must be 3 within 0.2");
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
  TEST_RUN(test_source_driven_sink_stays_non_negative);
  TEST_RUN(test_mixed_delta_cross_record_conserves);
  TEST_RUN(test_zero_delta_row);
  TEST_RUN(test_all_empty_component_single_step);
  TEST_RUN(test_empty_reservoir_fed_by_source);
  TEST_RUN(test_closure_check_detects_imbalance);
  TEST_RUN(test_star_repeatable);
  TEST_RUN(test_failure_table);
  TEST_RUN(test_many_external_fluxes_close_within_tolerance);
  TEST_RUN(test_start_controller_and_landing);
  TEST_RUN(test_near_empty_start);
  TEST_RUN(test_draining_sink);
  TEST_RUN(test_time_dependent_fixed_steps_third_order);

  TEST_SUMMARY();
  return TEST_RESULT();
}
