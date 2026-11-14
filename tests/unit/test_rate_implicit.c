/**
 * @file    test_rate_implicit.c
 * @brief   Proving problems for the linearly implicit (Patankar-Euler) rate scheme
 *
 * Validates: rate_integrate_segment() under RATE_SCHEME_IMPLICIT on hand-built
 * components: the block solve against a dense reference solve written here, on
 * random star components (one row of each at Delta = 0); non-negativity and
 * closure at step x rate products up to 1e16; scratch and work linear in the
 * number of galaxies; flat step counts on the stiff pair, from its slow
 * manifold and from off it; a draining sink; first order on the linear chain,
 * on a time-dependent law and in the bimolecular drift; the tracer bound and
 * the tracer total's closure in the yield problem; the starting step, the
 * twice-filtered error estimate and the controller, with no growth after a
 * rejection; flux amounts and a self-transfer; the ill-posed sink the explicit
 * scheme fails on; non-finite step x rate products; a tiny entry drained hard;
 * canonical-order independence; and the components the block solve refuses.
 *
 * Every expected value comes from a closed form or a dense solve written in
 * this file, never from the integrator. Every system's values are literals
 * here; the random components come from the fixed-seed SplitMix64 generator of
 * rate_test_support.h. Times are in normalised segment time tau, so a row with
 * segment length Delta has evolved for t = Delta at tau = 1. The error of a
 * problem is the largest absolute difference between a computed entry and its
 * closed form at tau = 1. Unless a case says otherwise the absolute tolerance is
 * 1e-3 times the relative tolerance. L, in tol_F and the tracer bound, is the
 * larger of the most inflow plus outflow terms on one entry and the most
 * reservoir and tracer entries on one row (one galaxy's block); this file
 * counts it itself. Each case prints its measured figures beside their bounds.
 */

#define MAX_ENTRIES 24576
#define MAX_FLUXES 24576
#define MAX_ROWS 10240
#define WORKSPACE_DOUBLES (1 << 18)

#include "../framework/test_framework.h"
#include "../framework/child_capture.h"
#include "../../src/core/rate_integrator.h"
#include "../../src/util/error.h"
#include "rate_test_support.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Test statistics (required for TEST_RUN macro) */
static int passed = 0;
static int failed = 0;

#define MAX_DENSE 128

/* The star of the scratch bound: satellites, and the scratch it must fit, per galaxy. */
#define STAR_SATELLITES 10000
#define STAR_BYTES_PER_GALAXY 1024

/* ---------------------------------------------------------------------------
 * Laws (rate_test_support.h, struct Probe): each overwrites the coefficients it
 * owns after the constant coefficients are set
 * ------------------------------------------------------------------------- */

/** Bimolecular: A to C at 2 B, B to C at 2 A (fluxes 0 and 1). */
static int bimolecular_law(const struct Probe *probe, double tau, const double *x, double *k) {
  (void)probe;
  (void)tau;
  k[0] = 2.0 * x[1];
  k[1] = 2.0 * x[0];
  return 0;
}

/** A sink at c / x with c = 1 (flux 0): a constant outflow that empties x at t = x0. */
static int inverse_law(const struct Probe *probe, double tau, const double *x, double *k) {
  (void)probe;
  (void)tau;
  k[0] = 1.0 / x[0];
  return 0;
}

/** A sink whose coefficient is 1 + 3 tau^2 (flux 0): x' = -Delta (1 + 3 tau^2) x. */
static int time_dependent_law(const struct Probe *probe, double tau, const double *x, double *k) {
  (void)probe;
  (void)x;
  k[0] = 1.0 + 3.0 * tau * tau;
  return 0;
}

/** Adaptive implicit settings at @p relative_tolerance (absolute 1e-3 times it). */
static struct RateIntegratorSettings implicit_settings(double relative_tolerance) {
  struct RateIntegratorSettings settings = adaptive_settings(relative_tolerance);
  settings.scheme = RATE_SCHEME_IMPLICIT;
  return settings;
}

/** Implicit settings of @p steps fixed steps. */
static struct RateIntegratorSettings implicit_fixed_settings(int64_t steps) {
  struct RateIntegratorSettings settings = fixed_settings(steps);
  settings.scheme = RATE_SCHEME_IMPLICIT;
  return settings;
}

/* ---------------------------------------------------------------------------
 * The test's own L, closure tolerance and per-total sums
 * ------------------------------------------------------------------------- */

/**
 * L: the most inflow plus outflow terms on one entry and, under the implicit
 * scheme, at least the most reservoir and tracer entries on one row. Tracer
 * moves are counted as the integrator's contract derives them: for every
 * transfer or sink out of a parent, a move out of each of its tracers, into the
 * recipient's tracer of the same total when the transfer is internal and the
 * recipient has one.
 */
static int expected_l_for(const struct System *system, enum RateScheme scheme) {
  static int terms[MAX_ENTRIES];
  static int tracer_of[MAX_ENTRIES][MAX_TOTALS];
  static int row_size[MAX_ROWS];
  const int n = system->num_entries;
  for (int i = 0; i < n; i++) {
    terms[i] = 0;
    for (int t = 0; t < MAX_TOTALS; t++) {
      tracer_of[i][t] = -1;
    }
  }
  for (int i = 0; i < n; i++) {
    if (system->entries[i].kind == RATE_ENTRY_TRACER) {
      tracer_of[system->entries[i].parent][system->entries[i].total] = i;
    }
  }
  for (int f = 0; f < system->num_fluxes; f++) {
    const struct RateFlux *flux = &system->fluxes[f];
    if (flux->kind == RATE_FLUX_SOURCE || flux->kind == RATE_FLUX_SLAVED) {
      terms[flux->to]++;
      continue;
    }
    if (flux->from == flux->to) {
      continue; /* a self-transfer moves nothing and derives no tracer move */
    }
    terms[flux->from]++;
    if (flux->kind == RATE_FLUX_INTERNAL) {
      terms[flux->to]++;
    }
    for (int t = 0; t < MAX_TOTALS; t++) {
      const int z = tracer_of[flux->from][t];
      if (z < 0) {
        continue;
      }
      terms[z]++;
      if (flux->kind == RATE_FLUX_INTERNAL && tracer_of[flux->to][t] >= 0) {
        terms[tracer_of[flux->to][t]]++;
      }
    }
  }
  int l = 0;
  for (int r = 0; r < system->num_rows; r++) {
    row_size[r] = 0;
  }
  for (int i = 0; i < n; i++) {
    if (terms[i] > l) {
      l = terms[i];
    }
    row_size[system->entries[i].row]++;
  }
  for (int r = 0; r < system->num_rows && scheme == RATE_SCHEME_IMPLICIT; r++) {
    if (row_size[r] > l) {
      l = row_size[r];
    }
  }
  return l;
}

/** L under the implicit scheme (expected_l_for()). */
static int expected_l(const struct System *system) {
  return expected_l_for(system, RATE_SCHEME_IMPLICIT);
}

/**
 * The worst closure of the system's totals against tol_F, as drift / tol_F
 * (a closure holds at <= 1), from @p start and the run's external amounts.
 */
static double worst_closure(const struct System *system, const double *start, const struct Run *run,
                            int l) {
  double worst = 0.0;
  for (int t = 0; t < system->num_totals; t++) {
    double start_sum = 0.0;
    double end_sum = 0.0;
    double magnitude = 0.0;
    int members = 0;
    for (int i = 0; i < system->num_entries; i++) {
      if (system->entries[i].total != t) {
        continue;
      }
      start_sum += start[i];
      end_sum += system->state[i];
      magnitude += fabs(start[i]);
      members++;
    }
    const double drift = fabs((end_sum - start_sum) - (run->source[t] - run->sink[t]));
    const double tolerance = closure_tolerance(l, run->outcome.stats.accepted_steps, members,
                                               magnitude, run->source[t], run->sink[t]);
    const double ratio = (drift == 0.0) ? 0.0 : drift / tolerance;
    if (!(ratio <= worst)) {
      worst = ratio;
    }
  }
  return worst;
}

/* ---------------------------------------------------------------------------
 * Random star components and their dense reference solve
 * ------------------------------------------------------------------------- */

/* The random star's totals, numbered so the solve order (mass first) is not index order. */
#define STAR_YIELD_TOTAL 0
#define STAR_TRACER_TOTAL 1
#define STAR_MASS_TOTAL 2

/** One galaxy of a random star: its mass reservoirs, its yield reservoir, zero flags. */
struct Galaxy {
  int mass[6];
  int num_mass;
  int yield; /* the reservoir of the yield total, or -1 */
};

/**
 * A random star: 1 to 11 satellites and a central at a random row, each galaxy
 * with 2 to 6 entries in three totals: mass reservoirs, tracers of some of them
 * and at most one reservoir of a yield total. With @p zero_rows, every fifth
 * reservoir or tracer entry, and the tracer of a zero parent, has a zero
 * right-hand side (zero content, no source, no slaved source). Fluxes: transfers between a galaxy's
 * mass reservoirs in both directions, sinks, sources, a yield slaved to one of
 * the galaxy's mass transfers or sinks, cross-record transfers from satellites
 * to the central in the mass and yield totals, and an occasional self-transfer.
 * Every transfer and sink has Delta times coefficient 10^U(@p low_exponent,
 * @p high_exponent), so one fixed step of length 1 has that step x rate; the
 * rows' Delta lie in [0.5, 1].
 */
static void build_random_star(struct System *system, uint64_t seed, double low_exponent,
                              double high_exponent, int zero_rows) {
  static struct Galaxy galaxy[12];
  system_init(system, 0, 3);
  const int satellites = 1 + uniform_index(&seed, 11);
  const int central = uniform_index(&seed, satellites + 1);
  static int zero[MAX_DENSE];
  int counter = 0;
  int global = 0;

  for (int r = 0; r <= satellites; r++) {
    add_row(system, uniform_in(&seed, 0.5, 1.0));
    const int size = 2 + uniform_index(&seed, 5);
    const int has_yield = (size >= 3 && next_uniform(&seed) < 0.5);
    const int rest = size - has_yield;
    const int tracers = uniform_index(&seed, rest / 2 + 1);
    struct Galaxy *g = &galaxy[r];
    g->num_mass = rest - tracers;
    g->yield = -1;
    for (int k = 0; k < g->num_mass; k++) {
      const int zero_mass = zero_rows && (counter++ % 5 == 4);
      const int m = add_reservoir(system, STAR_MASS_TOTAL,
                                  zero_mass ? 0.0 : pow(10.0, uniform_in(&seed, -3.0, 0.0)));
      zero[m] = zero_mass;
      g->mass[k] = m;
      if (k < tracers) {
        const int zero_tracer = (zero_rows && (counter++ % 5 == 4)) || zero_mass;
        const int z = add_tracer(system, STAR_TRACER_TOTAL, m,
                                 zero_tracer ? 0.0 : system->state[m] * next_uniform(&seed));
        zero[z] = zero_tracer;
      }
    }
    if (has_yield) {
      const int zero_yield = zero_rows && (counter++ % 5 == 4);
      g->yield = add_reservoir(system, STAR_YIELD_TOTAL,
                               zero_yield ? 0.0 : pow(10.0, uniform_in(&seed, -3.0, 0.0)));
      zero[g->yield] = zero_yield;
    }
  }

  for (int r = 0; r <= satellites; r++) {
    const struct Galaxy *g = &galaxy[r];
    const double delta = system->delta[r];
    const int first_flux = system->num_fluxes;
    for (int a = 0; a < g->num_mass; a++) {
      for (int b = 0; b < g->num_mass; b++) {
        if (a != b && next_uniform(&seed) < 0.5) {
          add_flux(system, RATE_FLUX_INTERNAL, g->mass[a], g->mass[b], global++, -1,
                   pow(10.0, uniform_in(&seed, low_exponent, high_exponent)) / delta);
        }
      }
      if (next_uniform(&seed) < 0.4) {
        add_flux(system, RATE_FLUX_SINK, g->mass[a], RATE_END_EXTERNAL, global++, -1,
                 pow(10.0, uniform_in(&seed, low_exponent, high_exponent)) / delta);
      }
    }
    const int movers = system->num_fluxes - first_flux;
    if (next_uniform(&seed) < 0.1) {
      const int m = g->mass[uniform_index(&seed, g->num_mass)];
      add_flux(system, RATE_FLUX_INTERNAL, m, m, global++, -1, 1.0 / delta);
    }
    for (int a = 0; a <= g->num_mass; a++) {
      const int entry = (a < g->num_mass) ? g->mass[a] : g->yield;
      if (entry >= 0 && !zero[entry] && next_uniform(&seed) < 0.5) {
        add_flux(system, RATE_FLUX_SOURCE, RATE_END_EXTERNAL, entry, global++, -1,
                 pow(10.0, uniform_in(&seed, -3.0, 0.0)) / delta);
      }
    }
    if (g->yield >= 0) {
      if (next_uniform(&seed) < 0.5) {
        add_flux(system, RATE_FLUX_SINK, g->yield, RATE_END_EXTERNAL, global++, -1,
                 pow(10.0, uniform_in(&seed, low_exponent, high_exponent)) / delta);
      }
      if (!zero[g->yield] && movers > 0) {
        add_flux(system, RATE_FLUX_SLAVED, RATE_END_EXTERNAL, g->yield, global++,
                 first_flux + uniform_index(&seed, movers), uniform_in(&seed, 0.05, 0.5));
      }
    }
    if (r == central) {
      continue;
    }
    const struct Galaxy *c = &galaxy[central];
    for (int a = 0; a < g->num_mass; a++) {
      if (next_uniform(&seed) < 0.5) {
        add_flux(system, RATE_FLUX_INTERNAL, g->mass[a], c->mass[uniform_index(&seed, c->num_mass)],
                 global++, -1, pow(10.0, uniform_in(&seed, low_exponent, high_exponent)) / delta);
      }
    }
    if (g->yield >= 0 && c->yield >= 0 && next_uniform(&seed) < 0.5) {
      add_flux(system, RATE_FLUX_INTERNAL, g->yield, c->yield, global++, -1,
               pow(10.0, uniform_in(&seed, low_exponent, high_exponent)) / delta);
    }
  }
}

/** Entries whose right-hand side is zero: zero content and no source or slaved source. */
static int zero_right_hand_sides(const struct System *system) {
  int count = 0;
  for (int i = 0; i < system->num_entries; i++) {
    int fed = 0;
    for (int f = 0; f < system->num_fluxes; f++) {
      const struct RateFlux *flux = &system->fluxes[f];
      fed |= (flux->kind == RATE_FLUX_SOURCE || flux->kind == RATE_FLUX_SLAVED) && flux->to == i;
    }
    count += (system->state[i] == 0.0 && !fed);
  }
  return count;
}

/**
 * One implicit step of length 1 as one dense system over every entry, solved
 * by Gaussian elimination with partial pivoting: (I - A) x = x0 + s, where A
 * holds each transfer's and sink's rate (Delta times coefficient, on the donor's
 * row), the tracer moves derived as the contract states, and each slaved source
 * as its coefficient times its master's rate, coupling the recipient to the
 * master's donor; s holds each source's rate (on the recipient's row).
 */
static void dense_reference(const struct System *system, double *x) {
  static double m[MAX_DENSE][MAX_DENSE];
  static double b[MAX_DENSE];
  static int tracer_of[MAX_DENSE][MAX_TOTALS];
  const int n = system->num_entries;
  for (int i = 0; i < n; i++) {
    for (int j = 0; j < n; j++) {
      m[i][j] = (i == j) ? 1.0 : 0.0;
    }
    b[i] = system->state[i];
    for (int t = 0; t < MAX_TOTALS; t++) {
      tracer_of[i][t] = -1;
    }
  }
  for (int i = 0; i < n; i++) {
    if (system->entries[i].kind == RATE_ENTRY_TRACER) {
      tracer_of[system->entries[i].parent][system->entries[i].total] = i;
    }
  }
  for (int f = 0; f < system->num_fluxes; f++) {
    const struct RateFlux *flux = &system->fluxes[f];
    if (flux->kind == RATE_FLUX_SOURCE) {
      b[flux->to] += system->delta[system->entries[flux->to].row] * system->coefficient[f];
      continue;
    }
    if (flux->kind == RATE_FLUX_SLAVED) {
      const struct RateFlux *master = &system->fluxes[flux->master];
      if (master->from != master->to) {
        const double master_rate =
            system->delta[system->entries[master->from].row] * system->coefficient[flux->master];
        m[flux->to][master->from] -= system->coefficient[f] * master_rate;
      }
      continue;
    }
    if (flux->from == flux->to) {
      continue;
    }
    const double rate = system->delta[system->entries[flux->from].row] * system->coefficient[f];
    m[flux->from][flux->from] += rate;
    if (flux->kind == RATE_FLUX_INTERNAL) {
      m[flux->to][flux->from] -= rate;
    }
    for (int t = 0; t < MAX_TOTALS; t++) {
      const int z = tracer_of[flux->from][t];
      if (z < 0) {
        continue;
      }
      m[z][z] += rate;
      const int target = (flux->kind == RATE_FLUX_INTERNAL) ? tracer_of[flux->to][t] : -1;
      if (target >= 0) {
        m[target][z] -= rate;
      }
    }
  }
  for (int k = 0; k < n; k++) {
    int pivot = k;
    for (int i = k + 1; i < n; i++) {
      if (fabs(m[i][k]) > fabs(m[pivot][k])) {
        pivot = i;
      }
    }
    if (pivot != k) {
      for (int j = 0; j < n; j++) {
        const double swap = m[k][j];
        m[k][j] = m[pivot][j];
        m[pivot][j] = swap;
      }
      const double swap = b[k];
      b[k] = b[pivot];
      b[pivot] = swap;
    }
    for (int i = k + 1; i < n; i++) {
      const double factor = m[i][k] / m[k][k];
      if (factor == 0.0) {
        continue;
      }
      for (int j = k; j < n; j++) {
        m[i][j] -= factor * m[k][j];
      }
      b[i] -= factor * b[k];
    }
  }
  for (int i = n - 1; i >= 0; i--) {
    double sum = b[i];
    for (int j = i + 1; j < n; j++) {
      sum -= m[i][j] * x[j];
    }
    x[i] = sum / m[i][i];
  }
}

/* ---------------------------------------------------------------------------
 * Problem builders and closed forms
 * ------------------------------------------------------------------------- */

/** Linear chain A to B to C at k1 = 1, k2 = 0.3, start (1, 0, 0), Delta = 2. */
static void build_chain(struct System *system) {
  system_init(system, 0, 1);
  add_row(system, 2.0);
  const int a = add_reservoir(system, 0, 1.0);
  const int b = add_reservoir(system, 0, 0.0);
  const int c = add_reservoir(system, 0, 0.0);
  add_flux(system, RATE_FLUX_INTERNAL, a, b, 0, -1, 1.0);
  add_flux(system, RATE_FLUX_INTERNAL, b, c, 1, -1, 0.3);
}

static double chain_error(const double *x) {
  const double k1 = 1.0;
  const double k2 = 0.3;
  const double t = 2.0;
  const double a = exp(-k1 * t);
  const double b = k1 / (k2 - k1) * (exp(-k1 * t) - exp(-k2 * t));
  const double c = 1.0 - a - b;
  return fmax(fabs(x[0] - a), fmax(fabs(x[1] - b), fabs(x[2] - c)));
}

/** Bimolecular A to C at 2 B, B to C at 2 A, start (1, 0.6, 0), Delta = 3 (bimolecular_law). */
static void build_bimolecular(struct System *system) {
  system_init(system, 0, 1);
  add_row(system, 3.0);
  const int a = add_reservoir(system, 0, 1.0);
  const int b = add_reservoir(system, 0, 0.6);
  const int c = add_reservoir(system, 0, 0.0);
  add_flux(system, RATE_FLUX_INTERNAL, a, c, 0, -1, 0.0);
  add_flux(system, RATE_FLUX_INTERNAL, b, c, 1, -1, 0.0);
}

/** Stiff pair: A and B exchanging at k, B drained at 1, Delta = 3, started at (a0, b0). */
static void build_stiff_pair(struct System *system, double k, double a0, double b0) {
  system_init(system, 0, 1);
  add_row(system, 3.0);
  const int a = add_reservoir(system, 0, a0);
  const int b = add_reservoir(system, 0, b0);
  add_flux(system, RATE_FLUX_INTERNAL, a, b, 0, -1, k);
  add_flux(system, RATE_FLUX_INTERNAL, b, a, 1, -1, k);
  add_flux(system, RATE_FLUX_SINK, b, RATE_END_EXTERNAL, 2, -1, 1.0);
}

/**
 * The stiff pair at t = 3 from (a0, b0): A' = -k A + k B, B' = k A - (k + 1) B.
 * The rates are the roots of lambda^2 + (2k + 1) lambda + k = 0, the fast one
 * -((2k + 1) + sqrt(4k^2 + 1)) / 2 and the slow one k over it; an eigenvector is
 * (1, m) with m = 1 + lambda / k, so a0 = c_slow + c_fast and b0 = c_slow m_slow
 * + c_fast m_fast.
 */
static void stiff_pair_exact(double k, double a0, double b0, double *a, double *b) {
  const double t = 3.0;
  const double fast = -((2.0 * k + 1.0) + sqrt(4.0 * k * k + 1.0)) / 2.0;
  const double slow = k / fast;
  const double m_slow = 1.0 + slow / k;
  const double m_fast = 1.0 + fast / k;
  const double c_fast = (b0 - a0 * m_slow) / (m_fast - m_slow);
  const double c_slow = a0 - c_fast;
  *a = c_slow * exp(slow * t) + c_fast * exp(fast * t);
  *b = c_slow * m_slow * exp(slow * t) + c_fast * m_fast * exp(fast * t);
}

/* Yield problem constants: source s into G, formation G to S at k, yield y into Z; Delta. */
#define YIELD_SOURCE 0.4
#define YIELD_FORMATION 1.3
#define YIELD_FRACTION 0.02
#define YIELD_DELTA 3.0

/** Entry and flux indices of a built yield problem. */
struct Yield {
  int gas;       /* G, mass total 0 */
  int stars;     /* S, mass total 0 */
  int metals;    /* Z, tracer of G, total 1 */
  int star_z;    /* Z_S, tracer of S, total 1 */
  int formation; /* flux: G to S at k */
};

/**
 * Yield: G0 = 1 fed by a source of 0.4, formation G to S at 1.3 carrying Z to
 * Z_S, a yield of 0.02 slaved to formation into Z, Z0 = 0.01, Delta = 3.
 */
static struct Yield build_yield(struct System *system) {
  struct Yield y;
  system_init(system, 0, 2);
  add_row(system, YIELD_DELTA);
  y.gas = add_reservoir(system, 0, 1.0);
  y.stars = add_reservoir(system, 0, 0.0);
  y.metals = add_tracer(system, 1, y.gas, 0.01);
  y.star_z = add_tracer(system, 1, y.stars, 0.0);
  add_flux(system, RATE_FLUX_SOURCE, RATE_END_EXTERNAL, y.gas, 0, -1, YIELD_SOURCE);
  y.formation = add_flux(system, RATE_FLUX_INTERNAL, y.gas, y.stars, 1, -1, YIELD_FORMATION);
  add_flux(system, RATE_FLUX_SLAVED, RATE_END_EXTERNAL, y.metals, 2, y.formation, YIELD_FRACTION);
  return y;
}

/**
 * Uniform star: the central (row 0, Delta = 1.25) holds H0 = 0.08 N and
 * Cold0 = 0.01 N for N satellites, H to Cold at 0.9; each satellite (Delta =
 * 1.1) holds G0 = 0.6 and Stars0 = 0, G to the central's H at 1.5
 * (cross-record) and to its own Stars at 0.75: two entries per galaxy. The
 * central's contents scale with N, so the component's dynamics, in each
 * entry's own scale, do not depend on N.
 */
static void build_uniform_star(struct System *system, int satellites) {
  system_init(system, 0, 1);
  add_row(system, 1.25);
  const int h = add_reservoir(system, 0, 0.08 * (double)satellites);
  const int cold = add_reservoir(system, 0, 0.01 * (double)satellites);
  add_flux(system, RATE_FLUX_INTERNAL, h, cold, 0, -1, 0.9);
  for (int s = 1; s <= satellites; s++) {
    add_row(system, 1.1);
    const int gas = add_reservoir(system, 0, 0.6);
    const int stars = add_reservoir(system, 0, 0.0);
    add_flux(system, RATE_FLUX_INTERNAL, gas, h, 1, -1, 1.5);
    add_flux(system, RATE_FLUX_INTERNAL, gas, stars, 2, -1, 0.75);
  }
}

/* ---------------------------------------------------------------------------
 * The block solve
 * ------------------------------------------------------------------------- */

/* Random components per case, and the seed of the first; each component adds its index. */
#define RANDOM_COMPONENTS 400
#define RANDOM_SEED 0x5EED7A11ull

/**
 * @test    The block solve matches a dense reference solve on random star components
 *
 * One fixed implicit step of length 1 on random stars (1 to 11 satellites, 2 to
 * 6 entries per galaxy, step x rate spanning eight decades up to 1e3, a fifth
 * of the right-hand sides zero, and one row of each, the k-th modulo its rows,
 * at Delta = 0, so it donates nothing and its sources move nothing but it
 * still receives) against dense_reference(). Expected: every entry within 1e-10
 * relative of the reference (an exact zero where it is zero), and the
 * integrator's L equal to this file's.
 */
static int test_block_solve_matches_dense_reference(void) {
  static struct System system;
  static double expected[MAX_DENSE];
  const struct RateIntegratorSettings settings = implicit_fixed_settings(1);
  double worst = 0.0;
  int largest = 0;
  int zero_rhs = 0;
  int entries = 0;
  for (int k = 0; k < RANDOM_COMPONENTS; k++) {
    build_random_star(&system, RANDOM_SEED + (uint64_t)k, -5.0, 3.0, 1);
    system.delta[k % system.num_rows] = 0.0;
    TEST_ASSERT(system.num_entries <= MAX_DENSE, "random component too large for the reference");
    dense_reference(&system, expected);
    zero_rhs += zero_right_hand_sides(&system);
    entries += system.num_entries;
    if (system.num_entries > largest) {
      largest = system.num_entries;
    }
    static struct Run run;
    TEST_ASSERT(run_segment(&system, NULL, &settings, &run) == 0, "workspace too small");
    if (run.cause != RATE_CAUSE_NONE) {
      printf("  component %d failed with %s\n", k, rate_cause_name(run.cause));
    }
    TEST_ASSERT(run.cause == RATE_CAUSE_NONE, "a random component's implicit step failed");
    TEST_ASSERT(run.outcome.max_terms_per_entry == expected_l(&system),
                "the integrator's L is not the larger of the terms and the row size");
    for (int i = 0; i < system.num_entries; i++) {
      const double difference = fabs(system.state[i] - expected[i]);
      const double relative = (expected[i] != 0.0)  ? difference / fabs(expected[i])
                              : (difference == 0.0) ? 0.0
                                                    : INFINITY;
      if (!(relative <= worst)) {
        worst = relative;
      }
    }
  }
  printf("  %d components, up to %d entries, %d of %d right-hand sides zero: worst relative "
         "difference %.3e (bound 1e-10)\n",
         RANDOM_COMPONENTS, largest, zero_rhs, entries, worst);
  TEST_ASSERT(worst <= 1e-10, "the block solve departs from the dense reference");
  return 0;
}

/**
 * @test    At step x rate up to 1e16 every solved entry is non-negative and each total closes
 *
 * The same random stars with step x rate spanning eight decades up to 1e16, one
 * fixed implicit step. Expected: every entry finite and non-negative, and every
 * total within tol_F of its start plus sources less sinks, with this file's L.
 */
static int test_block_solve_extreme_rates(void) {
  static struct System system;
  static double start[MAX_DENSE];
  const struct RateIntegratorSettings settings = implicit_fixed_settings(1);
  double smallest = INFINITY;
  double worst_ratio = 0.0;
  int non_finite = 0;
  for (int k = 0; k < RANDOM_COMPONENTS; k++) {
    build_random_star(&system, RANDOM_SEED + (uint64_t)k, 8.0, 16.0, 1);
    memcpy(start, system.state, (size_t)system.num_entries * sizeof(double));
    static struct Run run;
    TEST_ASSERT(run_segment(&system, NULL, &settings, &run) == 0, "workspace too small");
    if (run.cause != RATE_CAUSE_NONE) {
      printf("  component %d failed with %s\n", k, rate_cause_name(run.cause));
    }
    TEST_ASSERT(run.cause == RATE_CAUSE_NONE, "a random component's implicit step failed");
    for (int i = 0; i < system.num_entries; i++) {
      non_finite += !isfinite(system.state[i]);
      if (system.state[i] < smallest) {
        smallest = system.state[i];
      }
    }
    const double ratio = worst_closure(&system, start, &run, expected_l(&system));
    if (!(ratio <= worst_ratio)) {
      worst_ratio = ratio;
    }
  }
  printf("  %d components at step x rate in [1e8, 1e16]: smallest entry %.3e, %d non-finite, "
         "worst closure %.3e of tol_F\n",
         RANDOM_COMPONENTS, smallest, non_finite, worst_ratio);
  TEST_ASSERT(non_finite == 0 && smallest >= 0.0, "a solved entry is negative or not finite");
  TEST_ASSERT(worst_ratio <= 1.0, "a total does not close within tol_F");
  return 0;
}

/* The large star's workspace: the stated bound for its galaxies, in doubles. */
static double
    star_workspace[(size_t)STAR_BYTES_PER_GALAXY * (STAR_SATELLITES + 1) / sizeof(double)];

/** Integrate @p system once in the large star's workspace; CPU seconds, or -1 on failure. */
static double timed_segment(struct System *system, struct Run *run) {
  const struct RateIntegratorSettings settings = implicit_settings(1e-4);
  const clock_t begin = clock();
  if (run_segment_in(system, NULL, &settings, star_workspace, sizeof(star_workspace), run) != 0 ||
      run->cause != RATE_CAUSE_NONE) {
    return -1.0;
  }
  return (double)(clock() - begin) / (double)CLOCKS_PER_SEC;
}

/**
 * @test    Scratch and work grow linearly with the number of galaxies
 *
 * Uniform stars of 100, 1,000 and 10,000 satellites, two entries per galaxy,
 * whose dynamics do not depend on the number of satellites. Expected: the
 * workspace for each is at most STAR_BYTES_PER_GALAXY (1,024) bytes per galaxy;
 * its growth per galaxy is the same from 100 to 1,000 satellites as from 1,000
 * to 10,000 (within one byte); each star integrates inside a buffer of exactly
 * the bound for 10,000; and the accepted steps and law evaluations per segment
 * are the same for every size (within one), so with a step's work linear in the
 * entries and fluxes the segment's work is linear in the galaxies. CPU times are
 * printed, not asserted.
 */
static int test_scratch_and_work_linear_in_galaxies(void) {
  static struct System system;
  const int satellites[3] = {100, 1000, STAR_SATELLITES};
  size_t bytes[3];
  int64_t accepted[3];
  int64_t evaluations[3];
  double seconds[3];
  for (int j = 0; j < 3; j++) {
    build_uniform_star(&system, satellites[j]);
    const struct RateComponent component = component_of(&system);
    bytes[j] = rate_integrator_workspace_bytes(&component, RATE_SCHEME_IMPLICIT);
    const size_t bound = (size_t)STAR_BYTES_PER_GALAXY * (size_t)(satellites[j] + 1);
    static struct Run run;
    seconds[j] = timed_segment(&system, &run);
    accepted[j] = run.outcome.stats.accepted_steps;
    evaluations[j] = run.evaluations;
    printf("  %5d satellites: %zu workspace bytes, %.1f per galaxy (bound %d); %lld accepted "
           "steps, %lld law evaluations, %.3e s CPU (not asserted)\n",
           satellites[j], bytes[j], (double)bytes[j] / (double)(satellites[j] + 1),
           STAR_BYTES_PER_GALAXY, (long long)accepted[j], (long long)evaluations[j], seconds[j]);
    TEST_ASSERT(bytes[j] <= bound, "the workspace exceeds the stated bound per galaxy");
    TEST_ASSERT(seconds[j] >= 0.0, "a star failed inside the stated bound");
  }
  const double small_slope = (double)(bytes[1] - bytes[0]) / 900.0;
  const double large_slope = (double)(bytes[2] - bytes[1]) / 9000.0;
  printf("  growth per galaxy: %.3f bytes (100 to 1,000), %.3f bytes (1,000 to 10,000); %zu "
         "bytes hold the 10,000-satellite star\n",
         small_slope, large_slope, sizeof(star_workspace));
  TEST_ASSERT(fabs(large_slope - small_slope) <= 1.0, "the workspace does not grow linearly");
  for (int j = 1; j < 3; j++) {
    TEST_ASSERT(llabs(accepted[j] - accepted[0]) <= 1 &&
                    llabs(evaluations[j] - evaluations[0]) <= 1,
                "the steps or law evaluations per segment depend on the number of galaxies");
  }
  return 0;
}

/* ---------------------------------------------------------------------------
 * Accuracy and order
 * ------------------------------------------------------------------------- */

/**
 * @test    The stiff pair takes the same number of steps at every exchange rate
 *
 * kf = kb = 1e3, 1e6, 1e9 and 1e12, drain 1, Delta = 3, A0 = B0 = 0.5, relative
 * tolerance 1e-3. Expected: accepted step counts equal within one, and each
 * entry within 0.05 relative of stiff_pair_exact().
 */
static int test_stiff_pair_flat_step_count(void) {
  static struct System system;
  const double rates[4] = {1e3, 1e6, 1e9, 1e12};
  const struct RateIntegratorSettings settings = implicit_settings(1e-3);
  int64_t fewest = INT64_MAX;
  int64_t most = 0;
  for (int j = 0; j < 4; j++) {
    build_stiff_pair(&system, rates[j], 0.5, 0.5);
    static struct Run run;
    TEST_ASSERT(run_segment(&system, NULL, &settings, &run) == 0, "workspace too small");
    TEST_ASSERT(run.cause == RATE_CAUSE_NONE, "stiff pair segment failed");
    double a;
    double b;
    stiff_pair_exact(rates[j], 0.5, 0.5, &a, &b);
    const double relative = fmax(fabs(system.state[0] - a) / a, fabs(system.state[1] - b) / b);
    const int64_t steps = run.outcome.stats.accepted_steps;
    printf("  k = %.0e: %lld accepted, %lld rejected, relative error %.3e (bound 0.05)\n", rates[j],
           (long long)steps, (long long)run.outcome.stats.rejected_steps, relative);
    TEST_ASSERT(relative <= 0.05, "stiff pair misses its closed form by more than 0.05");
    fewest = (steps < fewest) ? steps : fewest;
    most = (steps > most) ? steps : most;
  }
  printf("  accepted steps from %lld to %lld (equal within one)\n", (long long)fewest,
         (long long)most);
  TEST_ASSERT(most - fewest <= 1, "stiff pair step counts differ by more than one");
  return 0;
}

/**
 * @test    Off its slow manifold, the stiff pair still takes a bounded number of steps
 *
 * The stiff pair started at (1, 0), kf = kb = 1e3, 1e6, 1e9 and 1e12, relative
 * tolerance 1e-3: the fast transient, of time scale 1 / (2k), lies inside the
 * first step. Expected: every segment completes; the accepted step counts lie
 * in [25, 150] (measured 112 at 1e3, where the transient is resolved, and 35 at
 * each faster rate), the largest at most 4 times the smallest; each entry
 * within 0.05 relative of stiff_pair_exact(). (Filtered once, the estimate
 * saturated on the first step's jump, and the segment underflowed at tau = 0
 * from 1e9.)
 */
static int test_stiff_pair_off_manifold(void) {
  static struct System system;
  const double rates[4] = {1e3, 1e6, 1e9, 1e12};
  const struct RateIntegratorSettings settings = implicit_settings(1e-3);
  int64_t fewest = INT64_MAX;
  int64_t most = 0;
  for (int j = 0; j < 4; j++) {
    build_stiff_pair(&system, rates[j], 1.0, 0.0);
    static struct Run run;
    TEST_ASSERT(run_segment(&system, NULL, &settings, &run) == 0, "workspace too small");
    double a;
    double b;
    stiff_pair_exact(rates[j], 1.0, 0.0, &a, &b);
    const double relative = fmax(fabs(system.state[0] - a) / a, fabs(system.state[1] - b) / b);
    const int64_t steps = run.outcome.stats.accepted_steps;
    printf("  k = %.0e from (1, 0): %s, %lld accepted, %lld rejected, relative error %.3e (bound "
           "0.05)\n",
           rates[j], rate_cause_name(run.cause), (long long)steps,
           (long long)run.outcome.stats.rejected_steps, relative);
    TEST_ASSERT(run.cause == RATE_CAUSE_NONE, "the stiff pair off its manifold failed");
    TEST_ASSERT(steps >= 25 && steps <= 150, "an off-manifold step count left [25, 150]");
    TEST_ASSERT(relative <= 0.05, "the stiff pair misses its closed form by more than 0.05");
    fewest = (steps < fewest) ? steps : fewest;
    most = (steps > most) ? steps : most;
  }
  printf("  accepted steps from %lld to %lld (the largest at most 4 times the smallest)\n",
         (long long)fewest, (long long)most);
  TEST_ASSERT(most <= 4 * fewest, "the off-manifold step counts spread by more than 4 times");
  return 0;
}

/**
 * @test    A draining sink completes without exhausting the step budget
 *
 * One entry x0 = 1 drained by a sink at rate x Delta = 1e3 and 1e6, Delta = 1,
 * default tolerances: the content falls through hundreds of decades. Its
 * weight's scale keeps the segment-start total, so the error control asks for
 * absolute accuracy once the content is small. Expected: each completes (no
 * step_budget) in at most 2,000 accepted steps (measured 647 and 1; about
 * 57,700 when the scale followed the content down), with a non-negative end
 * and its total within tol_F (L = 1).
 */
static int test_draining_sink_completes(void) {
  static struct System system;
  const double rates[2] = {1e3, 1e6};
  for (int j = 0; j < 2; j++) {
    system_init(&system, 0, 1);
    add_row(&system, 1.0);
    const int x = add_reservoir(&system, 0, 1.0);
    add_flux(&system, RATE_FLUX_SINK, x, RATE_END_EXTERNAL, 0, -1, rates[j]);
    struct RateIntegratorSettings settings;
    rate_integrator_default_settings(&settings);
    settings.scheme = RATE_SCHEME_IMPLICIT;
    static struct Run run;
    TEST_ASSERT(run_segment(&system, NULL, &settings, &run) == 0, "workspace too small");
    const double drift = fabs((system.state[x] - 1.0) + run.sink[0]);
    const double tolerance =
        closure_tolerance(1, run.outcome.stats.accepted_steps, 1, 1.0, 0.0, run.sink[0]);
    printf("  sink rate x Delta = %.0e: %s, %lld accepted, %lld rejected, x = %.3e, closure %.3e "
           "(tol_F %.3e)\n",
           rates[j], rate_cause_name(run.cause), (long long)run.outcome.stats.accepted_steps,
           (long long)run.outcome.stats.rejected_steps, system.state[x], drift, tolerance);
    TEST_ASSERT(run.cause == RATE_CAUSE_NONE, "the draining sink did not complete");
    TEST_ASSERT(run.outcome.stats.accepted_steps <= 2000,
                "the draining sink took more than 2,000 accepted steps");
    TEST_ASSERT(system.state[x] >= 0.0, "the drained entry ended negative");
    TEST_ASSERT(drift <= tolerance, "the drained total does not close within tol_F");
  }
  return 0;
}

/**
 * @test    Bimolecular: the total closes and the A - B drift is first order
 *
 * A to C at 2 B, B to C at 2 A, start (1, 0.6, 0), Delta = 3, at 16, 64, 256 and
 * 1024 fixed implicit steps. A - B is constant in the exact solution. Expected:
 * the total within tol_F at every n, and log4(drift_n / drift_4n) >= 0.9 on
 * each quadrupling from n = 64 (64 -> 256 and 256 -> 1024). The 16 -> 64
 * quadrupling is pre-asymptotic (h kappa A = 0.375 at n = 16), so its rate is
 * printed but not asserted, as the threshold law's order is asserted only from
 * n = 40.
 */
static int test_bimolecular_drift_first_order(void) {
  static struct System system;
  const int64_t steps[4] = {16, 64, 256, 1024};
  double drift[4];
  for (int j = 0; j < 4; j++) {
    build_bimolecular(&system);
    const double start[3] = {1.0, 0.6, 0.0};
    const struct RateIntegratorSettings settings = implicit_fixed_settings(steps[j]);
    struct Probe probe = law_probe(bimolecular_law);
    static struct Run run;
    TEST_ASSERT(run_segment(&system, &probe, &settings, &run) == 0, "workspace too small");
    TEST_ASSERT(run.cause == RATE_CAUSE_NONE, "bimolecular segment failed");
    drift[j] = fabs((system.state[0] - system.state[1]) - (start[0] - start[1]));
    const double ratio = worst_closure(&system, start, &run, expected_l(&system));
    printf("  n = %4lld: A - B drift %.4e, closure %.3e of tol_F\n", (long long)steps[j], drift[j],
           ratio);
    TEST_ASSERT(ratio <= 1.0, "bimolecular total does not close within tol_F");
  }
  for (int j = 0; j < 3; j++) {
    const double order = log(drift[j] / drift[j + 1]) / log(4.0);
    const int asserted = (steps[j] >= 64);
    printf("  quadrupling %lld -> %lld: log4 of the drift ratio %.4f (%s)\n", (long long)steps[j],
           (long long)steps[j + 1], order, asserted ? ">= 0.9" : "pre-asymptotic, not asserted");
    if (asserted) {
      TEST_ASSERT(order >= 0.9, "the A - B drift falls more slowly than first order");
    }
  }
  return 0;
}

/**
 * @test    Linear chain at fixed n = 16, 32, 64, 128 converges at first order
 *
 * Expected: |p - 1| <= 0.1 on each halving, p = log2(e_n / e_2n), against the
 * closed form of the chain A to B to C at 1 and 0.3, Delta = 2.
 */
static int test_linear_chain_first_order(void) {
  static struct System system;
  const int64_t steps[4] = {16, 32, 64, 128};
  double error[4];
  for (int j = 0; j < 4; j++) {
    build_chain(&system);
    const struct RateIntegratorSettings settings = implicit_fixed_settings(steps[j]);
    static struct Run run;
    TEST_ASSERT(run_segment(&system, NULL, &settings, &run) == 0, "workspace too small");
    TEST_ASSERT(run.cause == RATE_CAUSE_NONE, "chain segment failed");
    error[j] = chain_error(system.state);
    printf("  chain n=%-4lld error=%.4e\n", (long long)steps[j], error[j]);
  }
  for (int j = 0; j < 3; j++) {
    const double p = log2(error[j] / error[j + 1]);
    printf("  chain halving %lld->%lld p=%.4f (|p-1|<=0.1)\n", (long long)steps[j],
           (long long)steps[j + 1], p);
    TEST_ASSERT(fabs(p - 1.0) <= 0.1, "chain order departs from 1 by more than 0.1");
  }
  return 0;
}

/**
 * @test    A time-dependent law converges at first order at fixed steps
 *
 * x' = -Delta (1 + 3 tau^2) x in tau units, Delta = 2, x0 = 1, so x(1) =
 * e^(-2 Delta); the coefficients are frozen at each step's start time. Every
 * other proving problem is autonomous, so this case alone pins the law's time.
 * Expected: |p - 1| <= 0.1 on each halving from n = 16 to 256.
 */
static int test_time_dependent_first_order(void) {
  static struct System system;
  const int64_t steps[5] = {16, 32, 64, 128, 256};
  double error[5];
  for (int j = 0; j < 5; j++) {
    system_init(&system, 0, 1);
    add_row(&system, 2.0);
    const int x = add_reservoir(&system, 0, 1.0);
    add_flux(&system, RATE_FLUX_SINK, x, RATE_END_EXTERNAL, 0, -1, 0.0);
    const struct RateIntegratorSettings settings = implicit_fixed_settings(steps[j]);
    struct Probe probe = law_probe(time_dependent_law);
    static struct Run run;
    TEST_ASSERT(run_segment(&system, &probe, &settings, &run) == 0, "workspace too small");
    TEST_ASSERT(run.cause == RATE_CAUSE_NONE, "the time-dependent decay failed");
    error[j] = fabs(system.state[x] - exp(-2.0 * 2.0));
    printf("  time-dependent n=%-4lld error=%.4e\n", (long long)steps[j], error[j]);
  }
  for (int j = 0; j < 4; j++) {
    const double p = log2(error[j] / error[j + 1]);
    printf("  time-dependent halving %lld->%lld p=%.4f (|p-1|<=0.1)\n", (long long)steps[j],
           (long long)steps[j + 1], p);
    TEST_ASSERT(fabs(p - 1.0) <= 0.1, "the time-dependent order departs from 1 by more than 0.1");
  }
  return 0;
}

/**
 * @test    Yield problem: tracers within the tracer bound, the tracer total within tol_F
 *
 * G0 = 1, Z0 = 0.01, source 0.4 into G, formation G to S at 1.3, yield 0.02
 * slaved to formation into Z, Delta = 3, at relative tolerance 1e-4 and 1e-8.
 * Theta of G is its start content plus the source amount; of S, the formed
 * amount (formation's flux amount). Expected: z - p <= 8u (L + 2) N_acc Theta_p for both
 * tracers, both totals within tol_F with the yield as the tracer total's only
 * external amount, and the yield amount 0.02 x the formed amount within 8u N_acc
 * relative.
 */
static int test_yield_tracer_bound_and_closure(void) {
  static struct System system;
  const double tolerances[2] = {1e-4, 1e-8};
  for (int j = 0; j < 2; j++) {
    const struct Yield y = build_yield(&system);
    double start[5];
    memcpy(start, system.state, sizeof(start));
    const struct RateIntegratorSettings settings = implicit_settings(tolerances[j]);
    static struct Run run;
    TEST_ASSERT(run_segment(&system, NULL, &settings, &run) == 0, "workspace too small");
    TEST_ASSERT(run.cause == RATE_CAUSE_NONE, "yield segment failed");

    const int l = expected_l(&system);
    const int64_t accepted = run.outcome.stats.accepted_steps;
    const double *x = system.state;
    const double formed = run.flux[y.formation];
    const double gas_bound =
        8.0 * UNIT_ROUNDOFF * (double)(l + 2) * (double)accepted * (start[y.gas] + run.source[0]);
    const double stars_bound =
        8.0 * UNIT_ROUNDOFF * (double)(l + 2) * (double)accepted * (start[y.stars] + formed);
    const double closure = worst_closure(&system, start, &run, l);
    const double yield_gap = fabs(run.source[1] - YIELD_FRACTION * formed);
    const double yield_bound = 8.0 * UNIT_ROUNDOFF * (double)accepted * YIELD_FRACTION * formed;
    printf("  rtol %.0e: %lld steps, L = %d; Z - G = %.3e (bound %.3e), Z_S - S = %.3e (bound "
           "%.3e); closure %.3e of tol_F; yield - 0.02 formed = %.3e (bound %.3e)\n",
           tolerances[j], (long long)accepted, l, x[y.metals] - x[y.gas], gas_bound,
           x[y.star_z] - x[y.stars], stars_bound, closure, yield_gap, yield_bound);
    TEST_ASSERT(x[y.metals] - x[y.gas] <= gas_bound, "Z ends above G beyond the tracer bound");
    TEST_ASSERT(x[y.star_z] - x[y.stars] <= stars_bound,
                "Z_S ends above S beyond the tracer bound");
    TEST_ASSERT(run.source[1] > 0.0 && run.sink[1] == 0.0,
                "the tracer total's external amounts are not the yield alone");
    TEST_ASSERT(closure <= 1.0, "a total of the yield problem does not close within tol_F");
    TEST_ASSERT(yield_gap <= yield_bound, "the yield departs from 0.02 x the formed amount");
  }
  return 0;
}

/* ---------------------------------------------------------------------------
 * The starting step, the error estimate, flux amounts and self-transfers
 * ------------------------------------------------------------------------- */

/** An empty central: what feeds it. */
enum EmptyCentral { STRIPPED_INTO_EMPTY_CENTRAL, SOURCE_INTO_EMPTY_CENTRAL };

/**
 * The central (row 0, Delta = 1.25) holds H = Cold = 0, H to Cold at 0.9; one
 * satellite (row 1, Delta = 1) holds G0 = 0.8 and Stars0 = 0, G to Stars at
 * 0.6. STRIPPED: G is also stripped into the central's H at 1.7 (cross-record).
 * SOURCE: a source of 0.5 feeds the central's H instead.
 */
static void build_empty_central(struct System *system, enum EmptyCentral feed) {
  system_init(system, 0, 1);
  add_row(system, 1.25);
  const int h = add_reservoir(system, 0, 0.0);
  const int cold = add_reservoir(system, 0, 0.0);
  add_flux(system, RATE_FLUX_INTERNAL, h, cold, 0, -1, 0.9);
  add_row(system, 1.0);
  const int gas = add_reservoir(system, 0, 0.8);
  const int stars = add_reservoir(system, 0, 0.0);
  add_flux(system, RATE_FLUX_INTERNAL, gas, stars, 1, -1, 0.6);
  if (feed == STRIPPED_INTO_EMPTY_CENTRAL) {
    add_flux(system, RATE_FLUX_INTERNAL, gas, h, 2, -1, 1.7);
  } else {
    add_flux(system, RATE_FLUX_SOURCE, RATE_END_EXTERNAL, h, 2, -1, 0.5);
  }
}

/**
 * The first attempt's step from the starting step min(1, @p cap) after landing:
 * the remainder 1 is taken whole when it is within 1.1 of the step and within
 * the cap, halved when it is shorter than two steps, and the step kept otherwise.
 */
static double landed_first_step(double cap) {
  const double step = fmin(1.0, cap);
  if (1.0 <= 1.1 * step && 1.0 <= cap) {
    return 1.0;
  }
  return (1.0 < 2.0 * step) ? 0.5 : step;
}

/**
 * @test    An empty central fed from outside its row starts at the starting step
 *
 * build_empty_central(), stripped and source-fed, under both schemes at the
 * default tolerances. The central's own total is zero at the segment start
 * while the component's is not. Expected: every case integrates without
 * failure; its first attempt's step is the starting step after landing
 * (landed_first_step()): under the explicit scheme of the cap 0.9 x 2 / K_max,
 * K_max = Delta_s (0.6 + 1.7) = 2.3 stripped and 0.6 source-fed (only G has
 * content), under the implicit scheme of no cap; and its total closes within
 * tol_F (this file's L for the scheme).
 */
static int test_empty_central_start(void) {
  static struct System system;
  static struct AttemptLog log;
  const char *feeds[2] = {"stripped into", "source into"};
  const double k_max[2] = {1.0 * (0.6 + 1.7), 1.0 * 0.6};
  int failures = 0;
  for (int feed = 0; feed < 2; feed++) {
    for (int implicit = 0; implicit <= 1; implicit++) {
      build_empty_central(&system, (enum EmptyCentral)feed);
      double start[4];
      memcpy(start, system.state, sizeof(start));
      struct RateIntegratorSettings settings;
      rate_integrator_default_settings(&settings);
      settings.scheme = implicit ? RATE_SCHEME_IMPLICIT : RATE_SCHEME_EXPLICIT;
      attach_log(&settings, &log, &system);
      static struct Run run;
      TEST_ASSERT(run_segment(&system, NULL, &settings, &run) == 0, "workspace too small");
      const double closure =
          worst_closure(&system, start, &run, expected_l_for(&system, settings.scheme));
      const double first = (log.count > 0) ? log.step[0] : 0.0;
      const double expected = landed_first_step(implicit ? INFINITY : (0.9 * 2.0) / k_max[feed]);
      const int ok = run.cause == RATE_CAUSE_NONE && first == expected && closure <= 1.0;
      printf("  %s an empty central, %s: %s, first step %.17g (expected %.17g), %lld accepted, "
             "closure %.3e of tol_F%s\n",
             feeds[feed], implicit ? "implicit" : "explicit", rate_cause_name(run.cause), first,
             expected, (long long)run.outcome.stats.accepted_steps, closure,
             ok ? "" : "  <- FAILED");
      failures += !ok;
    }
  }
  TEST_ASSERT(failures == 0, "a segment feeding an empty central failed, did not start at the "
                             "starting step or did not close within tol_F");
  return 0;
}

/**
 * @test    Under the implicit scheme a self-transfer commits nothing and flux amounts sum exactly
 *
 * G0 = 1 with tracer Z0 = 0.01, S with tracer Z_S; formation G to S at 1.3, a
 * transfer G to G at 0.7 (both ends one entry) and a yield of 0.02 slaved to
 * the self-transfer into Z; Delta = 3, relative tolerance 1e-6. Expected: the
 * self-transfer's flux amount and the sum of its reported committed amounts are
 * both exactly 0, formation's flux amount equals the sum of its reported
 * amounts bit for bit, and the tracer total gains no source.
 */
static int test_self_transfer_and_flux_amounts_implicit(void) {
  static struct System system;
  static struct AttemptLog log;
  system_init(&system, 0, 2);
  add_row(&system, 3.0);
  const int gas = add_reservoir(&system, 0, 1.0);
  const int stars = add_reservoir(&system, 0, 0.0);
  const int metals = add_tracer(&system, 1, gas, 0.01);
  add_tracer(&system, 1, stars, 0.0);
  const int formation = add_flux(&system, RATE_FLUX_INTERNAL, gas, stars, 0, -1, 1.3);
  const int self = add_flux(&system, RATE_FLUX_INTERNAL, gas, gas, 1, -1, 0.7);
  add_flux(&system, RATE_FLUX_SLAVED, RATE_END_EXTERNAL, metals, 2, self, 0.02);
  struct RateIntegratorSettings settings = implicit_settings(1e-6);
  attach_log(&settings, &log, &system);
  log.watched[0] = self;
  log.watched[1] = formation;
  static struct Run run;
  TEST_ASSERT(run_segment(&system, NULL, &settings, &run) == 0, "workspace too small");
  TEST_ASSERT(run.cause == RATE_CAUSE_NONE, "self-transfer segment failed");

  const double zero = 0.0;
  printf("  self-transfer flux amount %.17g, sum of its reported amounts %.17g; formed %.17g, "
         "sum %.17g; tracer source %.3g; %lld accepted steps\n",
         run.flux[self], log.sum[0], run.flux[formation], log.sum[1], run.source[1],
         (long long)run.outcome.stats.accepted_steps);
  TEST_ASSERT(memcmp(&run.flux[self], &zero, sizeof(double)) == 0,
              "the self-transfer's flux amount is not exactly zero");
  TEST_ASSERT(memcmp(&log.sum[0], &zero, sizeof(double)) == 0,
              "the self-transfer's reported committed amounts are not exactly zero");
  TEST_ASSERT(memcmp(&run.flux[formation], &log.sum[1], sizeof(double)) == 0,
              "formation's flux amount differs from the sum of its committed amounts");
  TEST_ASSERT(run.source[1] == 0.0, "a yield slaved to a self-transfer moved content");
  return 0;
}

/* ---------------------------------------------------------------------------
 * Ill-posed laws, failures and extreme states
 * ------------------------------------------------------------------------- */

/**
 * @test    The starting step, the twice-filtered error estimate and the controller on one sink
 *
 * x0 = 1 drained at k = 1, Delta = 1, relative tolerance 1e-2 (absolute 1e-5),
 * so K = 1. Closed forms: the first step is 1; the solve gives x1 = x0 / (1 +
 * K); the estimate is e = (1 / 2) (F(x1) - F(x0)) / (1 + K)^2, F(x) = -K x,
 * filtered by both solves; its weight is absolute x 1 + relative x x0 (the
 * scale is the segment-start total 1, the larger content x0), err = e / weight
 * = 6.24, so the step is rejected and the second is 1 x 0.9 err^(-1/2) = 0.36
 * (within the clamp [0.2, 5], and short enough that landing leaves it).
 * Expected: the first attempt rejected with that step and error norm to 1e-12
 * relative (filtered once, the norm would be twice as large), and the second
 * step to 1e-12 relative.
 */
static int test_start_estimate_and_controller(void) {
  static struct System system;
  static struct AttemptLog log;
  system_init(&system, 0, 1);
  add_row(&system, 1.0);
  const int x = add_reservoir(&system, 0, 1.0);
  add_flux(&system, RATE_FLUX_SINK, x, RATE_END_EXTERNAL, 0, -1, 1.0);
  struct RateIntegratorSettings settings = implicit_settings(1e-2);
  attach_log(&settings, &log, &system);
  static struct Run run;
  TEST_ASSERT(run_segment(&system, NULL, &settings, &run) == 0, "workspace too small");
  TEST_ASSERT(run.cause == RATE_CAUSE_NONE && log.count >= 3, "single sink segment failed");

  const double k = 1.0;
  const double x1 = 1.0 / (1.0 + k);
  const double estimate = 0.5 * (-k * x1 + k * 1.0) / ((1.0 + k) * (1.0 + k));
  const double err = estimate / (1e-5 * 1.0 + 1e-2 * 1.0);
  const double second = 1.0 * fmin(5.0, fmax(0.2, 0.9 / sqrt(err)));
  const double norm_gap = fabs(log.error_norm[0] - err) / err;
  const double next_gap = fabs(log.step[1] - second) / second;
  printf("  first step %.17g (closed form 1), err %.17g (closed form %.17g), second step %.17g "
         "(closed form %.17g); relative gaps %.1e, %.1e (bound 1e-12)\n",
         log.step[0], log.error_norm[0], err, log.step[1], second, norm_gap, next_gap);
  TEST_ASSERT(log.step[0] == 1.0 && log.tau[0] == 0.0, "the first step is not 1");
  TEST_ASSERT(!log.accepted[0], "the first attempt was not rejected");
  TEST_ASSERT(norm_gap <= 1e-12, "the error norm is not the twice-filtered estimate's");
  TEST_ASSERT(next_gap <= 1e-12, "the controller is not 0.9 err^(-1/2)");
  return 0;
}

/**
 * @test    Under the implicit scheme the step does not grow right after a rejection
 *
 * The stiff pair from (1, 0) at kf = kb = 1e6, relative tolerance 1e-4: its
 * first attempts are rejected on the transient, and the first step accepted
 * after them has an error norm below 0.9^2, where the controller alone would
 * lengthen the step (0.9 err^(-1/2) > 1; measured err 0.794). Expected: that
 * accepted step exists, and the next attempt is no longer than it.
 */
static int test_no_growth_after_rejection_implicit(void) {
  static struct System system;
  static struct AttemptLog log;
  build_stiff_pair(&system, 1e6, 1.0, 0.0);
  struct RateIntegratorSettings settings = implicit_settings(1e-4);
  attach_log(&settings, &log, &system);
  static struct Run run;
  TEST_ASSERT(run_segment(&system, NULL, &settings, &run) == 0, "workspace too small");
  TEST_ASSERT(run.cause == RATE_CAUSE_NONE, "the stiff pair failed");
  int64_t held = -1; /* the first accepted step after a rejection with err below 0.9^2 */
  for (int64_t i = 1; i + 1 < log.count && i + 1 < LOG_CAPACITY && held < 0; i++) {
    if (log.accepted[i] && !log.accepted[i - 1] && log.error_norm[i] < 0.9 * 0.9) {
      held = i;
    }
  }
  TEST_ASSERT(held >= 0, "the case must accept a step after a rejection with room to grow");
  printf("  attempt %lld accepted after a rejection at step %.17g with err %.4f (controller "
         "factor %.4f); next step %.17g\n",
         (long long)held, log.step[held], log.error_norm[held], 0.9 / sqrt(log.error_norm[held]),
         log.step[held + 1]);
  TEST_ASSERT(log.step[held + 1] <= log.step[held], "the step grew right after a rejection");
  TEST_ASSERT(!log.grew_after_rejection, "a step grew right after a rejection");
  return 0;
}

/** A row holding 1e308 fed by a source of 1e308 per unit time, Delta = 1. */
static void build_overflow(struct System *system) {
  system_init(system, 0, 1);
  add_row(system, 1.0);
  const int h = add_reservoir(system, 0, 1e308);
  add_flux(system, RATE_FLUX_SOURCE, RATE_END_EXTERNAL, h, 0, -1, 1e308);
}

/**
 * @test    A solve that overflows is rejected before any law sees it
 *
 * H = 1e308 fed by a source of 1e308 per unit time, Delta = 1: a step of length
 * 1 solves H = 2e308, beyond the largest double, while every product is finite.
 * Expected: at one fixed step, rejection_budget naming H, with the state
 * unchanged; adaptive, at least one positivity rejection, no law evaluated at a
 * non-finite entry and no non-finite entry committed, whatever the outcome.
 */
static int test_overflowing_solve_rejected(void) {
  static struct System system;
  static struct AttemptLog log;
  build_overflow(&system);
  struct RateIntegratorSettings settings = implicit_fixed_settings(1);
  static struct Run run;
  TEST_ASSERT(run_segment(&system, NULL, &settings, &run) == 0, "workspace too small");
  printf("  one fixed step: %s, entry %d, H = %.17g\n", rate_cause_name(run.cause),
         run.outcome.failure.entry, system.state[0]);
  TEST_ASSERT(run.cause == RATE_CAUSE_REJECTION_BUDGET && run.outcome.failure.entry == 0,
              "an overflowing fixed step was not rejected naming H");
  TEST_ASSERT(system.state[0] == 1e308, "a failed segment changed the state");

  build_overflow(&system);
  settings = implicit_settings(1e-4);
  attach_log(&settings, &log, &system);
  TEST_ASSERT(run_segment(&system, NULL, &settings, &run) == 0, "workspace too small");
  printf("  adaptive: %s after %lld accepted steps and %lld positivity rejections; a law saw a "
         "non-finite entry: %s; a non-finite entry committed: %s\n",
         rate_cause_name(run.cause), (long long)run.outcome.stats.accepted_steps,
         (long long)run.outcome.stats.positivity_rejections, run.non_finite ? "yes" : "no",
         log.non_finite ? "yes" : "no");
  TEST_ASSERT(run.outcome.stats.positivity_rejections >= 1, "no overflow was rejected");
  TEST_ASSERT(!run.non_finite, "a law was evaluated at a non-finite entry");
  TEST_ASSERT(!log.non_finite, "a non-finite entry was committed");
  return 0;
}

/**
 * @test    The sink at c / x fails under the explicit scheme and integrates under the implicit
 *
 * x0 = 0.6, c = 1, Delta = 1: the exact x reaches zero at t = 0.6, where the
 * coefficient diverges, so the law is ill-posed beyond it (x0 is no dyadic
 * fraction, so no explicit stage lands on zero exactly). Expected: the
 * explicit scheme fails with step_underflow; the implicit scheme ends at tau = 1
 * without failure, with a finite non-negative entry. That the implicit scheme
 * integrates an ill-posed law without complaint is why it is never the default.
 */
static int test_ill_posed_sink_integrates_without_complaint(void) {
  static struct System system;
  struct RateIntegratorSettings settings;
  rate_integrator_default_settings(&settings);
  for (int implicit = 0; implicit <= 1; implicit++) {
    system_init(&system, 0, 1);
    add_row(&system, 1.0);
    const int x = add_reservoir(&system, 0, 0.6);
    add_flux(&system, RATE_FLUX_SINK, x, RATE_END_EXTERNAL, 0, -1, 0.0);
    settings.scheme = implicit ? RATE_SCHEME_IMPLICIT : RATE_SCHEME_EXPLICIT;
    struct Probe probe = law_probe(inverse_law);
    static struct Run run;
    TEST_ASSERT(run_segment(&system, &probe, &settings, &run) == 0, "workspace too small");
    printf("  %s scheme: %s after %lld accepted steps, x = %.3e\n",
           implicit ? "implicit" : "explicit", rate_cause_name(run.cause),
           (long long)run.outcome.stats.accepted_steps, system.state[x]);
    if (implicit) {
      TEST_ASSERT(run.cause == RATE_CAUSE_NONE, "the implicit scheme failed on the c / x sink");
      TEST_ASSERT(isfinite(system.state[x]) && system.state[x] >= 0.0,
                  "the implicit scheme ended the c / x sink at a negative or non-finite value");
    } else {
      TEST_ASSERT(run.cause == RATE_CAUSE_STEP_UNDERFLOW,
                  "the explicit scheme did not fail on the c / x sink with step_underflow");
    }
  }
  printf("  the implicit scheme integrates this ill-posed law without complaint: this is why it "
         "is never the default\n");
  return 0;
}

/** One non-finite-product case: the system it builds and the failure it expects. */
struct ProductCase {
  const char *name;
  enum RateFluxKind kind; /* the flux whose product overflows */
  int fluxes;             /* how many such fluxes leave A */
  double coefficient;
  double delta;
  const char *detail_word; /* words of the expected detail */
};

/**
 * @test    A non-finite step x rate product fails with bad_coefficient
 *
 * Rows: a sink from A at 1e308 over Delta = 10 (its rate overflows), a source
 * into A at 1e308 over Delta = 10, and two sinks from A at 1e308 over Delta = 1
 * (each product finite, the block pivot not). One fixed implicit step. Expected:
 * bad_coefficient naming A and the first flux, a detail saying which product
 * failed, and the state bitwise unchanged.
 */
static int test_non_finite_product_fails(void) {
  static struct System system;
  const struct ProductCase cases[] = {
      {"sink rate overflows", RATE_FLUX_SINK, 1, 1e308, 10.0, "flux's rate"},
      {"source rate overflows", RATE_FLUX_SOURCE, 1, 1e308, 10.0, "flux's rate"},
      {"two sinks overflow the pivot", RATE_FLUX_SINK, 2, 1e308, 1.0, "pivot"},
  };
  const struct RateIntegratorSettings settings = implicit_fixed_settings(1);
  for (size_t j = 0; j < sizeof(cases) / sizeof(cases[0]); j++) {
    const struct ProductCase *row = &cases[j];
    system_init(&system, 0, 1);
    add_row(&system, row->delta);
    const int a = add_reservoir(&system, 0, 1.0);
    for (int f = 0; f < row->fluxes; f++) {
      if (row->kind == RATE_FLUX_SOURCE) {
        add_flux(&system, RATE_FLUX_SOURCE, RATE_END_EXTERNAL, a, f, -1, row->coefficient);
      } else {
        add_flux(&system, RATE_FLUX_SINK, a, RATE_END_EXTERNAL, f, -1, row->coefficient);
      }
    }
    const double before = system.state[a];
    static struct Run run;
    TEST_ASSERT(run_segment(&system, NULL, &settings, &run) == 0, "workspace too small");
    const struct RateFailure *failure = &run.outcome.failure;
    printf("  %s: %s, entry %d, flux %d, detail \"%s\"\n", row->name, rate_cause_name(run.cause),
           failure->entry, failure->flux, failure->detail);
    TEST_ASSERT(run.cause == RATE_CAUSE_BAD_COEFFICIENT, row->name);
    TEST_ASSERT(failure->entry == a && failure->flux == 0, row->name);
    TEST_ASSERT(strstr(failure->detail, row->detail_word) != NULL &&
                    strstr(failure->detail, "implicit") != NULL,
                row->name);
    TEST_ASSERT(memcmp(&before, &system.state[a], sizeof(double)) == 0, row->name);
  }
  return 0;
}

/**
 * @test    A tiny entry drained at 1e12 stays non-negative everywhere
 *
 * State (1e-20, 1), A to B at 1e12, Delta = 1, adaptive at the default
 * tolerances and at 1 and 16 fixed steps. Expected: success, no law evaluated at
 * a negative entry, and no committed entry below zero.
 */
static int test_tiny_entry_drained_hard(void) {
  static struct System system;
  const int64_t fixed[3] = {0, 1, 16};
  for (int j = 0; j < 3; j++) {
    system_init(&system, 0, 1);
    add_row(&system, 1.0);
    const int a = add_reservoir(&system, 0, 1e-20);
    const int b = add_reservoir(&system, 0, 1.0);
    add_flux(&system, RATE_FLUX_INTERNAL, a, b, 0, -1, 1e12);
    struct RateIntegratorSettings settings = implicit_settings(1e-4);
    settings.fixed_steps = fixed[j];
    struct AttemptLog log;
    attach_log(&settings, &log, &system);
    static struct Run run;
    TEST_ASSERT(run_segment(&system, NULL, &settings, &run) == 0, "workspace too small");
    printf("  %s: %s, %lld accepted; smallest entry a law saw %.3e, smallest committed %.3e, "
           "A = %.3e\n",
           fixed[j] ? "fixed" : "adaptive", rate_cause_name(run.cause),
           (long long)run.outcome.stats.accepted_steps, run.min_state, log.min_committed,
           system.state[a]);
    TEST_ASSERT(run.cause == RATE_CAUSE_NONE, "the drained tiny entry failed");
    TEST_ASSERT(run.min_state >= 0.0, "a law was evaluated at a negative entry");
    TEST_ASSERT(log.min_committed >= 0.0 && !log.non_finite,
                "a committed entry was negative or not finite");
  }
  return 0;
}

/* ---------------------------------------------------------------------------
 * Canonical order and the components the block solve refuses
 * ------------------------------------------------------------------------- */

/** Reverse the flux list in place, keeping each slaved source's master. */
static void reverse_fluxes(struct System *system) {
  const int m = system->num_fluxes;
  for (int f = 0; f < m / 2; f++) {
    const struct RateFlux flux = system->fluxes[f];
    const double coefficient = system->coefficient[f];
    system->fluxes[f] = system->fluxes[m - 1 - f];
    system->coefficient[f] = system->coefficient[m - 1 - f];
    system->fluxes[m - 1 - f] = flux;
    system->coefficient[m - 1 - f] = coefficient;
  }
  for (int f = 0; f < m; f++) {
    if (system->fluxes[f].kind == RATE_FLUX_SLAVED) {
      system->fluxes[f].master = m - 1 - system->fluxes[f].master;
    }
  }
}

/**
 * @test    A reversed flux list gives a bit-identical implicit segment
 *
 * Random stars, integrated adaptively at relative tolerance 1e-4 with the flux
 * list as built and reversed. Expected: bit-identical end states, external
 * amounts and attempt sequences.
 */
static int test_flux_order_independent(void) {
  static struct System forward;
  static struct System reversed;
  static struct AttemptLog forward_log;
  static struct AttemptLog reversed_log;
  int64_t most = 0;
  for (int k = 0; k < 20; k++) {
    build_random_star(&forward, RANDOM_SEED + (uint64_t)k, -2.0, 2.0, 1);
    build_random_star(&reversed, RANDOM_SEED + (uint64_t)k, -2.0, 2.0, 1);
    reverse_fluxes(&reversed);
    struct RateIntegratorSettings settings = implicit_settings(1e-4);
    attach_log(&settings, &forward_log, &forward);
    static struct Run forward_run;
    TEST_ASSERT(run_segment(&forward, NULL, &settings, &forward_run) == 0, "workspace too small");
    attach_log(&settings, &reversed_log, &reversed);
    static struct Run reversed_run;
    TEST_ASSERT(run_segment(&reversed, NULL, &settings, &reversed_run) == 0, "workspace too small");
    if (forward_run.cause != RATE_CAUSE_NONE || reversed_run.cause != RATE_CAUSE_NONE) {
      const struct RateFailure *failure = &forward_run.outcome.failure;
      printf("  star %d: %s / %s, entry %d, flux %d, total %d, tau %.17g, step %.3e, %lld "
             "accepted, %lld rejected\n",
             k, rate_cause_name(forward_run.cause), rate_cause_name(reversed_run.cause),
             failure->entry, failure->flux, failure->total, failure->tau, failure->step,
             (long long)forward_run.outcome.stats.accepted_steps,
             (long long)forward_run.outcome.stats.rejected_steps);
    }
    TEST_ASSERT(forward_run.cause == RATE_CAUSE_NONE && reversed_run.cause == RATE_CAUSE_NONE,
                "a random star's adaptive implicit segment failed");
    const size_t state_bytes = (size_t)forward.num_entries * sizeof(double);
    const size_t total_bytes = (size_t)forward.num_totals * sizeof(double);
    TEST_ASSERT(memcmp(forward.state, reversed.state, state_bytes) == 0,
                "reversing the flux list changed the end state");
    TEST_ASSERT(memcmp(forward_run.source, reversed_run.source, total_bytes) == 0 &&
                    memcmp(forward_run.sink, reversed_run.sink, total_bytes) == 0,
                "reversing the flux list changed the external amounts");
    TEST_ASSERT(forward_log.count <= LOG_CAPACITY, "the attempt log overflowed");
    TEST_ASSERT(same_sequence(&forward_log, &reversed_log),
                "reversing the flux list changed the attempt sequence");
    most = (forward_log.count > most) ? forward_log.count : most;
  }
  printf("  20 random stars, up to %lld attempts: forward and reversed flux lists bit-identical\n",
         (long long)most);
  return 0;
}

/** Integrate @p system under the implicit scheme in a child (expect_fatal()). */
static void run_refused(struct System *system) {
  const struct RateIntegratorSettings settings = implicit_settings(1e-4);
  static struct Run run;
  (void)run_segment(system, NULL, &settings, &run);
}

/** Row 0's entries split by row 1's. */
static void run_split_row(const char *arg) {
  (void)arg;
  static struct System system;
  system_init(&system, 0, 1);
  add_row(&system, 1.0);
  add_row(&system, 1.0);
  system.row = 0;
  const int a = add_reservoir(&system, 0, 1.0);
  system.row = 1;
  add_reservoir(&system, 0, 1.0);
  system.row = 0;
  const int c = add_reservoir(&system, 0, 0.0);
  add_flux(&system, RATE_FLUX_INTERNAL, a, c, 0, -1, 1.0);
  run_refused(&system);
}

/** Rows 0 and 1 sending to each other. */
static void run_row_cycle(const char *arg) {
  (void)arg;
  static struct System system;
  system_init(&system, 0, 1);
  add_row(&system, 1.0);
  const int a = add_reservoir(&system, 0, 1.0);
  add_row(&system, 1.0);
  const int b = add_reservoir(&system, 0, 1.0);
  add_flux(&system, RATE_FLUX_INTERNAL, a, b, 0, -1, 1.0);
  add_flux(&system, RATE_FLUX_INTERNAL, b, a, 1, -1, 1.0);
  run_refused(&system);
}

/** A slaved source into the total of its own master's donor. */
static void run_total_cycle(const char *arg) {
  (void)arg;
  static struct System system;
  system_init(&system, 0, 1);
  add_row(&system, 1.0);
  const int a = add_reservoir(&system, 0, 1.0);
  const int b = add_reservoir(&system, 0, 0.0);
  const int master = add_flux(&system, RATE_FLUX_INTERNAL, a, b, 0, -1, 1.0);
  add_flux(&system, RATE_FLUX_SLAVED, RATE_END_EXTERNAL, b, 1, master, 0.1);
  run_refused(&system);
}

/** An internal transfer between two totals. */
static void run_joined_totals(const char *arg) {
  (void)arg;
  static struct System system;
  system_init(&system, 0, 2);
  add_row(&system, 1.0);
  const int a = add_reservoir(&system, 0, 1.0);
  const int b = add_reservoir(&system, 1, 0.0);
  add_flux(&system, RATE_FLUX_INTERNAL, a, b, 0, -1, 1.0);
  run_refused(&system);
}

/**
 * @test    Components the block solve cannot order are refused
 *
 * Rows: a row whose entries are split by another row's, two rows sending to
 * each other, a slaved source into its master donor's own total, and an
 * internal transfer between two totals. Expected: each is fatal under the
 * implicit scheme, naming its rule.
 */
static int test_unblockable_components_refused(void) {
  TEST_ASSERT(expect_fatal(NULL, run_split_row, "row", "listed together") == 1,
              "a split row was not refused");
  TEST_ASSERT(expect_fatal(NULL, run_row_cycle, "cycle among rows", "implicit") == 1,
              "a cycle among rows was not refused");
  TEST_ASSERT(expect_fatal(NULL, run_total_cycle, "totals", "cycle") == 1,
              "a cycle among totals was not refused");
  TEST_ASSERT(expect_fatal(NULL, run_joined_totals, "joins totals", "one total") == 1,
              "an internal transfer between totals was not refused");
  return 0;
}

int main(void) {
  printf("%s", BLUE);
  printf("============================================================\n");
  printf("Test Suite: Rate Integrator, Linearly Implicit Scheme\n");
  printf("============================================================\n");
  printf("%s\n", NC);

  initialize_error_handling(LOG_LEVEL_WARNING, NULL);

  TEST_RUN(test_block_solve_matches_dense_reference);
  TEST_RUN(test_block_solve_extreme_rates);
  TEST_RUN(test_scratch_and_work_linear_in_galaxies);
  TEST_RUN(test_stiff_pair_flat_step_count);
  TEST_RUN(test_stiff_pair_off_manifold);
  TEST_RUN(test_draining_sink_completes);
  TEST_RUN(test_bimolecular_drift_first_order);
  TEST_RUN(test_linear_chain_first_order);
  TEST_RUN(test_time_dependent_first_order);
  TEST_RUN(test_yield_tracer_bound_and_closure);
  TEST_RUN(test_empty_central_start);
  TEST_RUN(test_self_transfer_and_flux_amounts_implicit);
  TEST_RUN(test_start_estimate_and_controller);
  TEST_RUN(test_no_growth_after_rejection_implicit);
  TEST_RUN(test_overflowing_solve_rejected);
  TEST_RUN(test_ill_posed_sink_integrates_without_complaint);
  TEST_RUN(test_non_finite_product_fails);
  TEST_RUN(test_tiny_entry_drained_hard);
  TEST_RUN(test_flux_order_independent);
  TEST_RUN(test_unblockable_components_refused);

  TEST_SUMMARY();
  return TEST_RESULT();
}
