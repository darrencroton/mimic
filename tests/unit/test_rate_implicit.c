/**
 * @file    test_rate_implicit.c
 * @brief   Proving problems for the linearly implicit (Patankar-Euler) rate scheme
 *
 * Validates: rate_integrate_segment() under RATE_SCHEME_IMPLICIT on hand-built
 * components: the block solve against a dense reference solve written here, on
 * random star components; non-negativity and closure at step x rate products up
 * to 1e16; scratch and work linear in the number of galaxies; flat step counts
 * on the stiff pair; first order on the linear chain and in the bimolecular
 * drift; the tracer bound and the tracer total's closure in the yield problem;
 * the ill-posed sink the explicit scheme fails on; non-finite step x rate
 * products; a tiny entry drained hard; canonical-order independence; and the
 * components the block solve refuses.
 *
 * Every expected value comes from a closed form or a dense solve written in
 * this file, never from the integrator. Every system's values are literals
 * here; the random components come from the fixed-seed SplitMix64 generator
 * below. Times are in normalised segment time tau, so a row with segment length
 * Delta has evolved for t = Delta at tau = 1. The error of a problem is the
 * largest absolute difference between a computed entry and its closed form at
 * tau = 1. Unless a case says otherwise the absolute tolerance is 1e-3 times the
 * relative tolerance. L, in tol_F and the tracer bound, is the larger of the
 * most inflow plus outflow terms on one entry and the most reservoir and tracer
 * entries on one row (one galaxy's block); this file counts it itself. Each
 * case prints its measured figures beside their bounds.
 */

#include "../framework/test_framework.h"
#include "../framework/child_capture.h"
#include "../../src/core/rate_integrator.h"
#include "../../src/util/error.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Test statistics (required for TEST_RUN macro) */
static int passed = 0;
static int failed = 0;

/* u = 2^-53, the unit roundoff of IEEE binary64. */
#define UNIT_ROUNDOFF 0x1p-53

#define MAX_ENTRIES 24576
#define MAX_FLUXES 24576
#define MAX_ROWS 10240
#define MAX_TOTALS 4
#define MAX_DENSE 128
#define LOG_CAPACITY 4096

/* The star of the scratch bound: satellites, and the scratch it must fit, per galaxy. */
#define STAR_SATELLITES 10000
#define STAR_BYTES_PER_GALAXY 1024

/* ---------------------------------------------------------------------------
 * Hand-built systems
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
  int row; /* the row add_entry() puts new entries on */
};

/** Start an empty system with @p num_totals totals; add_row() opens its first row. */
static void system_init(struct System *system, int num_totals) {
  system->num_entries = 0;
  system->num_fluxes = 0;
  system->num_rows = 0;
  system->num_totals = num_totals;
  system->row = -1;
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

static int add_observer(struct System *system, int flux) {
  return add_entry(system, RATE_ENTRY_OBSERVER, 0, flux, 0.0);
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

/* ---------------------------------------------------------------------------
 * Laws, with a probe that sees every state the integrator evaluates them at
 * ------------------------------------------------------------------------- */

/** A state-dependent law: overwrites the coefficients it owns after the constants are set. */
typedef void (*LawFill)(const struct System *system, const double *x, double *k);

struct Probe {
  const struct System *system;
  LawFill fill;        /* NULL: the constant coefficients alone */
  int64_t evaluations; /* law evaluations seen */
  double min_state;    /* smallest reservoir or tracer entry any law was evaluated at */
  int non_finite;      /* a law was evaluated at a reservoir or tracer entry not finite */
};

static int probed_law(void *data, double tau, const double *state, double *coefficients) {
  struct Probe *probe = data;
  const struct System *system = probe->system;
  (void)tau;
  probe->evaluations++;
  for (int i = 0; i < system->num_entries; i++) {
    if (system->entries[i].kind == RATE_ENTRY_OBSERVER) {
      continue;
    }
    probe->min_state = (state[i] < probe->min_state) ? state[i] : probe->min_state;
    probe->non_finite |= !isfinite(state[i]);
  }
  for (int f = 0; f < system->num_fluxes; f++) {
    coefficients[f] = system->coefficient[f];
  }
  if (probe->fill != NULL) {
    probe->fill(system, state, coefficients);
  }
  return 0;
}

/** Bimolecular: A to C at 2 B, B to C at 2 A (fluxes 0 and 1). */
static void bimolecular_law(const struct System *system, const double *x, double *k) {
  (void)system;
  k[0] = 2.0 * x[1];
  k[1] = 2.0 * x[0];
}

/** A sink at c / x with c = 1 (flux 0): a constant outflow that empties x at t = x0. */
static void inverse_law(const struct System *system, const double *x, double *k) {
  (void)system;
  k[0] = 1.0 / x[0];
}

/* ---------------------------------------------------------------------------
 * Attempt log (RateIntegratorSettings.on_attempt)
 * ------------------------------------------------------------------------- */

/* Entries of the committed states the log keeps for the first two accepted steps. */
#define KEPT_ENTRIES 8

struct AttemptLog {
  const struct System *system;
  int64_t count;
  double tau[LOG_CAPACITY];
  double step[LOG_CAPACITY];
  int accepted[LOG_CAPACITY];
  double error_norm[LOG_CAPACITY];
  double min_committed; /* smallest reservoir or tracer entry of any committed state */
  int non_finite;       /* a committed entry that was not finite */
  int watched[2];       /* declared flux indices whose committed amounts are summed, or -1 */
  double sum[2];        /* sum of the watched fluxes' committed amounts, in step order */
  int kept;             /* accepted steps whose committed state is kept below (at most 2) */
  double kept_step[2];
  double kept_state[2][KEPT_ENTRIES];
};

static void log_attempt(void *data, const struct RateAttempt *attempt) {
  struct AttemptLog *log = data;
  if (log->count < LOG_CAPACITY) {
    log->tau[log->count] = attempt->tau;
    log->step[log->count] = attempt->step;
    log->accepted[log->count] = attempt->accepted;
    log->error_norm[log->count] = attempt->error_norm;
  }
  log->count++;
  if (!attempt->accepted) {
    return;
  }
  for (int w = 0; w < 2; w++) {
    if (log->watched[w] >= 0) {
      log->sum[w] += attempt->amounts[log->watched[w]];
    }
  }
  if (log->kept < 2 && log->system->num_entries <= KEPT_ENTRIES) {
    log->kept_step[log->kept] = attempt->step;
    memcpy(log->kept_state[log->kept], attempt->state,
           (size_t)log->system->num_entries * sizeof(double));
    log->kept++;
  }
  for (int i = 0; i < log->system->num_entries; i++) {
    if (log->system->entries[i].kind == RATE_ENTRY_OBSERVER) {
      continue;
    }
    if (!isfinite(attempt->state[i])) {
      log->non_finite = 1;
    }
    if (attempt->state[i] < log->min_committed) {
      log->min_committed = attempt->state[i];
    }
  }
}

static void attach_log(struct RateIntegratorSettings *settings, struct AttemptLog *log,
                       const struct System *system) {
  memset(log, 0, sizeof(*log));
  log->system = system;
  log->min_committed = INFINITY;
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

/* The caller-owned workspace every case but the large star integrates in. */
static double workspace[1 << 18];

static struct RateIntegratorSettings implicit_settings(double relative_tolerance) {
  struct RateIntegratorSettings settings;
  rate_integrator_default_settings(&settings);
  settings.scheme = RATE_SCHEME_IMPLICIT;
  settings.relative_tolerance = relative_tolerance;
  settings.absolute_tolerance = 1e-3 * relative_tolerance;
  return settings;
}

static struct RateIntegratorSettings implicit_fixed_settings(int64_t steps) {
  struct RateIntegratorSettings settings;
  rate_integrator_default_settings(&settings);
  settings.scheme = RATE_SCHEME_IMPLICIT;
  settings.fixed_steps = steps;
  return settings;
}

struct Run {
  enum RateCause cause;
  struct RateSegmentOutcome outcome;
  double source[MAX_TOTALS];
  double sink[MAX_TOTALS];
  int64_t evaluations; /* law evaluations the probe saw */
  double min_state;    /* smallest reservoir or tracer entry a law saw */
  int non_finite;      /* a law saw a reservoir or tracer entry not finite */
};

/** Integrate @p system's state in place in @p buffer; returns 1 when the buffer is too small. */
static int run_segment_in(struct System *system, LawFill fill,
                          const struct RateIntegratorSettings *settings, void *buffer, size_t bytes,
                          struct Run *run) {
  const struct RateComponent component = component_of(system);
  if (rate_integrator_workspace_bytes(&component, settings->scheme) > bytes) {
    return 1;
  }
  struct Probe probe = {.system = system, .fill = fill, .min_state = INFINITY};
  const struct RateLaw law = {.evaluate = probed_law, .data = &probe};
  run->cause = rate_integrate_segment(&component, &law, settings, buffer, bytes, system->state,
                                      run->source, run->sink, &run->outcome);
  run->evaluations = probe.evaluations;
  run->min_state = probe.min_state;
  run->non_finite = probe.non_finite;
  return 0;
}

static int run_segment(struct System *system, LawFill fill,
                       const struct RateIntegratorSettings *settings, struct Run *run) {
  return run_segment_in(system, fill, settings, workspace, sizeof(workspace), run);
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
    if (system->entries[i].kind != RATE_ENTRY_OBSERVER) {
      row_size[system->entries[i].row]++;
    }
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

/** tol_F = 8u [(L + 2) N_acc + n_F + 2] (sum |x(start)| + Src + Snk). */
static double closure_tolerance(int l, int64_t accepted, int members, double start_magnitude,
                                double source, double sink) {
  const double terms = (double)(l + 2) * (double)accepted + (double)members + 2.0;
  return 8.0 * UNIT_ROUNDOFF * terms * (start_magnitude + source + sink);
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
      if (system->entries[i].kind == RATE_ENTRY_OBSERVER || system->entries[i].total != t) {
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

/** SplitMix64: the fixed-seed generator of the random components. */
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

/** A uniform integer in [0, count). */
static int uniform_index(uint64_t *seed, int count) {
  const int index = (int)(next_uniform(seed) * (double)count);
  return (index < count) ? index : count - 1;
}

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
  system_init(system, 3);
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
  system_init(system, 1);
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
  system_init(system, 1);
  add_row(system, 3.0);
  const int a = add_reservoir(system, 0, 1.0);
  const int b = add_reservoir(system, 0, 0.6);
  const int c = add_reservoir(system, 0, 0.0);
  add_flux(system, RATE_FLUX_INTERNAL, a, c, 0, -1, 0.0);
  add_flux(system, RATE_FLUX_INTERNAL, b, c, 1, -1, 0.0);
}

/** Stiff pair: A and B exchanging at k, B drained at 1, Delta = 3, A0 = B0 = 0.5. */
static void build_stiff_pair(struct System *system, double k) {
  system_init(system, 1);
  add_row(system, 3.0);
  const int a = add_reservoir(system, 0, 0.5);
  const int b = add_reservoir(system, 0, 0.5);
  add_flux(system, RATE_FLUX_INTERNAL, a, b, 0, -1, k);
  add_flux(system, RATE_FLUX_INTERNAL, b, a, 1, -1, k);
  add_flux(system, RATE_FLUX_SINK, b, RATE_END_EXTERNAL, 2, -1, 1.0);
}

/**
 * The stiff pair at t = 3: A' = -k A + k B, B' = k A - (k + 1) B. The rates are
 * the roots of lambda^2 + (2k + 1) lambda + k = 0, the fast one
 * -((2k + 1) + sqrt(4k^2 + 1)) / 2 and the slow one k over it; an eigenvector is
 * (1, 1 + lambda / k). With A0 = B0, c_slow lambda_slow = -c_fast lambda_fast.
 */
static void stiff_pair_exact(double k, double *a, double *b) {
  const double t = 3.0;
  const double a0 = 0.5;
  const double fast = -((2.0 * k + 1.0) + sqrt(4.0 * k * k + 1.0)) / 2.0;
  const double slow = k / fast;
  const double c_slow = a0 / (1.0 - slow / fast);
  const double c_fast = -c_slow * slow / fast;
  *a = c_slow * exp(slow * t) + c_fast * exp(fast * t);
  *b = c_slow * (1.0 + slow / k) * exp(slow * t) + c_fast * (1.0 + fast / k) * exp(fast * t);
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
  int formed;    /* observer of formation */
};

/**
 * Yield: G0 = 1 fed by a source of 0.4, formation G to S at 1.3 carrying Z to
 * Z_S, a yield of 0.02 slaved to formation into Z, Z0 = 0.01, Delta = 3, and an
 * observer of formation.
 */
static struct Yield build_yield(struct System *system) {
  struct Yield y;
  system_init(system, 2);
  add_row(system, YIELD_DELTA);
  y.gas = add_reservoir(system, 0, 1.0);
  y.stars = add_reservoir(system, 0, 0.0);
  y.metals = add_tracer(system, 1, y.gas, 0.01);
  y.star_z = add_tracer(system, 1, y.stars, 0.0);
  add_flux(system, RATE_FLUX_SOURCE, RATE_END_EXTERNAL, y.gas, 0, -1, YIELD_SOURCE);
  y.formation = add_flux(system, RATE_FLUX_INTERNAL, y.gas, y.stars, 1, -1, YIELD_FORMATION);
  add_flux(system, RATE_FLUX_SLAVED, RATE_END_EXTERNAL, y.metals, 2, y.formation, YIELD_FRACTION);
  y.formed = add_observer(system, y.formation);
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
  system_init(system, 1);
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
 * of the right-hand sides zero) against dense_reference(). Expected: every
 * entry within 1e-10 relative of the reference (an exact zero where it is zero),
 * and the integrator's L equal to this file's.
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
    TEST_ASSERT(system.num_entries <= MAX_DENSE, "random component too large for the reference");
    dense_reference(&system, expected);
    zero_rhs += zero_right_hand_sides(&system);
    entries += system.num_entries;
    if (system.num_entries > largest) {
      largest = system.num_entries;
    }
    struct Run run;
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
    struct Run run;
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
    struct Run run;
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
    build_stiff_pair(&system, rates[j]);
    struct Run run;
    TEST_ASSERT(run_segment(&system, NULL, &settings, &run) == 0, "workspace too small");
    TEST_ASSERT(run.cause == RATE_CAUSE_NONE, "stiff pair segment failed");
    double a;
    double b;
    stiff_pair_exact(rates[j], &a, &b);
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
    struct Run run;
    TEST_ASSERT(run_segment(&system, bimolecular_law, &settings, &run) == 0, "workspace too small");
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
    struct Run run;
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
 * @test    Yield problem: tracers within the tracer bound, the tracer total within tol_F
 *
 * G0 = 1, Z0 = 0.01, source 0.4 into G, formation G to S at 1.3, yield 0.02
 * slaved to formation into Z, Delta = 3, at relative tolerance 1e-4 and 1e-8.
 * Theta of G is its start content plus the source amount; of S, the formed
 * amount (an observer). Expected: z - p <= 8u (L + 2) N_acc Theta_p for both
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
    struct Run run;
    TEST_ASSERT(run_segment(&system, NULL, &settings, &run) == 0, "workspace too small");
    TEST_ASSERT(run.cause == RATE_CAUSE_NONE, "yield segment failed");

    const int l = expected_l(&system);
    const int64_t accepted = run.outcome.stats.accepted_steps;
    const double *x = system.state;
    const double formed = x[y.formed];
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
 * The first step, observers and self-transfers
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
  system_init(system, 1);
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
 * @test    An empty central fed from outside its row does not shrink the first step
 *
 * build_empty_central(), stripped and source-fed, under both schemes at the
 * default tolerances. The central's own total is zero at the segment start
 * while the component's is not; without leaving an empty row of its total out
 * of the first-step rule, its floored weight (about 1e-19) asks for a first step
 * below 2^-46 and the segment fails with step_underflow. Expected: every case
 * integrates without failure, its first step is at least 2^-46, and its total
 * closes within tol_F (this file's L for the scheme).
 */
static int test_empty_central_first_step(void) {
  static struct System system;
  static struct AttemptLog log;
  const char *feeds[2] = {"stripped into", "source into"};
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
      struct Run run;
      TEST_ASSERT(run_segment(&system, NULL, &settings, &run) == 0, "workspace too small");
      const double closure =
          worst_closure(&system, start, &run, expected_l_for(&system, settings.scheme));
      const double first = (log.count > 0) ? log.step[0] : 0.0;
      const int ok = run.cause == RATE_CAUSE_NONE && first >= 0x1p-46 && closure <= 1.0;
      printf("  %s an empty central, %s: %s, first step %.3e, %lld accepted, closure %.3e of "
             "tol_F%s\n",
             feeds[feed], implicit ? "implicit" : "explicit", rate_cause_name(run.cause), first,
             (long long)run.outcome.stats.accepted_steps, closure, ok ? "" : "  <- FAILED");
      failures += !ok;
    }
  }
  TEST_ASSERT(failures == 0, "a segment feeding an empty central failed, started below 2^-46 "
                             "or did not close within tol_F");
  return 0;
}

/** A sink whose coefficient is the observer's content (entry 1), flux 1. */
static void observer_law(const struct System *system, const double *x, double *k) {
  (void)system;
  k[1] = x[1];
}

/**
 * @test    The implicit scheme's next step starts from the observers it committed
 *
 * R0 = 1 fed by a source of 1, an observer O (O0 = 0) on that source, and a
 * sink from R whose coefficient is O; Delta = 1, relative tolerance 1e-3,
 * absolute 1e-6. With the coefficients frozen at the step start, the second
 * accepted step of length h solves R2 = (R1 + h) / (1 + h O1), O1 the observer
 * committed by the first. Expected: the first step commits O1 = h1 and R1 =
 * 1 + h1, and the second meets its equation to 4u relative, which a law that
 * saw O at its pre-commit value 0 would miss.
 */
static int test_observer_seen_at_committed_value(void) {
  static struct System system;
  static struct AttemptLog log;
  system_init(&system, 1);
  add_row(&system, 1.0);
  const int r = add_reservoir(&system, 0, 1.0);
  const int source = add_flux(&system, RATE_FLUX_SOURCE, RATE_END_EXTERNAL, r, 0, -1, 1.0);
  add_observer(&system, source);
  add_flux(&system, RATE_FLUX_SINK, r, RATE_END_EXTERNAL, 1, -1, 0.0);
  struct RateIntegratorSettings settings = implicit_settings(1e-3);
  settings.absolute_tolerance = 1e-6;
  attach_log(&settings, &log, &system);
  struct Run run;
  TEST_ASSERT(run_segment(&system, observer_law, &settings, &run) == 0, "workspace too small");
  TEST_ASSERT(run.cause == RATE_CAUSE_NONE && log.kept == 2, "observer segment failed");

  const double h1 = log.kept_step[0];
  const double r1 = log.kept_state[0][0];
  const double o1 = log.kept_state[0][1];
  const double h2 = log.kept_step[1];
  const double r2 = log.kept_state[1][0];
  const double expected = (r1 + h2) / (1.0 + h2 * o1);
  const double stale = r1 + h2; /* the second step with O seen at 0 */
  const double gap = fabs(r2 - expected) / expected;
  printf("  step 1: h = %.17g, R1 = %.17g, O1 = %.17g; step 2: h = %.17g, R2 = %.17g against "
         "(R1 + h) / (1 + h O1) = %.17g (relative gap %.1e, bound 4u; with O stale %.17g)\n",
         h1, r1, o1, h2, r2, expected, gap, stale);
  TEST_ASSERT(o1 == h1 && r1 == 1.0 + h1, "the first step did not commit O1 = h1, R1 = 1 + h1");
  TEST_ASSERT(gap <= 4.0 * UNIT_ROUNDOFF, "the second step did not start from the committed O1");
  return 0;
}

/**
 * @test    Under the implicit scheme a self-transfer commits nothing and observers sum exactly
 *
 * G0 = 1 with tracer Z0 = 0.01, S with tracer Z_S; formation G to S at 1.3, a
 * transfer G to G at 0.7 (both ends one entry) and a yield of 0.02 slaved to
 * the self-transfer into Z; Delta = 3, relative tolerance 1e-6. Observers on the
 * self-transfer and on formation start at zero. Expected: the self-transfer
 * observer and the sum of its reported committed amounts are both exactly 0,
 * the formation observer equals the sum of its reported amounts bit for bit,
 * and the tracer total gains no source.
 */
static int test_self_transfer_and_observers_implicit(void) {
  static struct System system;
  static struct AttemptLog log;
  system_init(&system, 2);
  add_row(&system, 3.0);
  const int gas = add_reservoir(&system, 0, 1.0);
  const int stars = add_reservoir(&system, 0, 0.0);
  const int metals = add_tracer(&system, 1, gas, 0.01);
  add_tracer(&system, 1, stars, 0.0);
  const int formation = add_flux(&system, RATE_FLUX_INTERNAL, gas, stars, 0, -1, 1.3);
  const int self = add_flux(&system, RATE_FLUX_INTERNAL, gas, gas, 1, -1, 0.7);
  add_flux(&system, RATE_FLUX_SLAVED, RATE_END_EXTERNAL, metals, 2, self, 0.02);
  const int self_observer = add_observer(&system, self);
  const int formed = add_observer(&system, formation);
  struct RateIntegratorSettings settings = implicit_settings(1e-6);
  attach_log(&settings, &log, &system);
  log.watched[0] = self;
  log.watched[1] = formation;
  struct Run run;
  TEST_ASSERT(run_segment(&system, NULL, &settings, &run) == 0, "workspace too small");
  TEST_ASSERT(run.cause == RATE_CAUSE_NONE, "self-transfer segment failed");

  const double zero = 0.0;
  printf("  self-transfer observer %.17g, sum of its reported amounts %.17g; formed %.17g, "
         "sum %.17g; tracer source %.3g; %lld accepted steps\n",
         system.state[self_observer], log.sum[0], system.state[formed], log.sum[1], run.source[1],
         (long long)run.outcome.stats.accepted_steps);
  TEST_ASSERT(memcmp(&system.state[self_observer], &zero, sizeof(double)) == 0,
              "the self-transfer observer is not exactly zero");
  TEST_ASSERT(memcmp(&log.sum[0], &zero, sizeof(double)) == 0,
              "the self-transfer's reported committed amounts are not exactly zero");
  TEST_ASSERT(memcmp(&system.state[formed], &log.sum[1], sizeof(double)) == 0,
              "the formation observer differs from the sum of its committed amounts");
  TEST_ASSERT(run.source[1] == 0.0, "a yield slaved to a self-transfer moved content");
  return 0;
}

/* ---------------------------------------------------------------------------
 * Ill-posed laws, failures and extreme states
 * ------------------------------------------------------------------------- */

/**
 * @test    The first step, the filtered error estimate and the controller on one sink
 *
 * x0 = 1 drained at k = 1, Delta = 1, default tolerances (relative 1e-4,
 * absolute 1e-7), so K = 1 and S = x0. Closed forms: the first step is h0 =
 * 0.01 d0 / d1 with d0 = x0 / w0 and d1 = K x0 / w0, so h0 = 0.01 / K; the solve
 * gives x1 = x0 / (1 + h0 K); the estimate is e = (h0 / 2) (F(x1) - F(x0)) /
 * (1 + h0 K), F(x) = -K x; err = |e| / (absolute + relative), the weight's
 * scale and its larger content both x0. Expected: the first attempt accepted
 * with that step and error norm to 1e-12 relative, and the second attempt's
 * step the first times 0.9 err^(-1/2), to 1e-12 relative.
 */
static int test_first_step_estimate_and_controller(void) {
  static struct System system;
  static struct AttemptLog log;
  system_init(&system, 1);
  add_row(&system, 1.0);
  const int x = add_reservoir(&system, 0, 1.0);
  add_flux(&system, RATE_FLUX_SINK, x, RATE_END_EXTERNAL, 0, -1, 1.0);
  struct RateIntegratorSettings settings = implicit_settings(1e-4);
  settings.absolute_tolerance = 1e-7;
  attach_log(&settings, &log, &system);
  struct Run run;
  TEST_ASSERT(run_segment(&system, NULL, &settings, &run) == 0, "workspace too small");
  TEST_ASSERT(run.cause == RATE_CAUSE_NONE && log.count >= 2, "single sink segment failed");

  const double k = 1.0;
  const double h0 = 0.01 / k;
  const double x1 = 1.0 / (1.0 + h0 * k);
  const double estimate = (h0 / 2.0) * (-k * x1 + k * 1.0) / (1.0 + h0 * k);
  const double err = estimate / (1e-7 + 1e-4);
  const double second = h0 * fmin(5.0, fmax(0.2, 0.9 / sqrt(err)));
  const double step_gap = fabs(log.step[0] - h0) / h0;
  const double norm_gap = fabs(log.error_norm[0] - err) / err;
  const double next_gap = fabs(log.step[1] - second) / second;
  printf("  first step %.17g (closed form %.17g), err %.17g (closed form %.17g), second step "
         "%.17g (closed form %.17g); relative gaps %.1e, %.1e, %.1e (bound 1e-12)\n",
         log.step[0], h0, log.error_norm[0], err, log.step[1], second, step_gap, norm_gap,
         next_gap);
  TEST_ASSERT(log.accepted[0], "the first attempt was rejected");
  TEST_ASSERT(step_gap <= 1e-12, "the first step is not h0 = 0.01 d0 / d1");
  TEST_ASSERT(norm_gap <= 1e-12, "the error norm is not the filtered estimate's");
  TEST_ASSERT(next_gap <= 1e-12, "the controller is not 0.9 err^(-1/2)");
  return 0;
}

/** A row holding 1e308 fed by a source of 1e308 per unit time, Delta = 1. */
static void build_overflow(struct System *system) {
  system_init(system, 1);
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
  struct Run run;
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
 * x0 = 0.5, c = 1, Delta = 1: the exact x reaches zero at t = 0.5, where the
 * coefficient diverges, so the law is ill-posed beyond it. Expected: the
 * explicit scheme fails with step_underflow; the implicit scheme ends at tau = 1
 * without failure, with a finite non-negative entry. That the implicit scheme
 * integrates an ill-posed law without complaint is why it is never the default.
 */
static int test_ill_posed_sink_integrates_without_complaint(void) {
  static struct System system;
  struct RateIntegratorSettings settings;
  rate_integrator_default_settings(&settings);
  for (int implicit = 0; implicit <= 1; implicit++) {
    system_init(&system, 1);
    add_row(&system, 1.0);
    const int x = add_reservoir(&system, 0, 0.5);
    add_flux(&system, RATE_FLUX_SINK, x, RATE_END_EXTERNAL, 0, -1, 0.0);
    settings.scheme = implicit ? RATE_SCHEME_IMPLICIT : RATE_SCHEME_EXPLICIT;
    struct Run run;
    TEST_ASSERT(run_segment(&system, inverse_law, &settings, &run) == 0, "workspace too small");
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
    system_init(&system, 1);
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
    struct Run run;
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
    system_init(&system, 1);
    add_row(&system, 1.0);
    const int a = add_reservoir(&system, 0, 1e-20);
    const int b = add_reservoir(&system, 0, 1.0);
    add_flux(&system, RATE_FLUX_INTERNAL, a, b, 0, -1, 1e12);
    struct RateIntegratorSettings settings = implicit_settings(1e-4);
    settings.fixed_steps = fixed[j];
    struct AttemptLog log;
    attach_log(&settings, &log, &system);
    struct Run run;
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
    struct Run forward_run;
    TEST_ASSERT(run_segment(&forward, NULL, &settings, &forward_run) == 0, "workspace too small");
    attach_log(&settings, &reversed_log, &reversed);
    struct Run reversed_run;
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
  struct Run run;
  (void)run_segment(system, NULL, &settings, &run);
}

/** Row 0's entries split by row 1's. */
static void run_split_row(const char *arg) {
  (void)arg;
  static struct System system;
  system_init(&system, 1);
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
  system_init(&system, 1);
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
  system_init(&system, 1);
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
  system_init(&system, 2);
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
  TEST_RUN(test_bimolecular_drift_first_order);
  TEST_RUN(test_linear_chain_first_order);
  TEST_RUN(test_yield_tracer_bound_and_closure);
  TEST_RUN(test_empty_central_first_step);
  TEST_RUN(test_observer_seen_at_committed_value);
  TEST_RUN(test_self_transfer_and_observers_implicit);
  TEST_RUN(test_first_step_estimate_and_controller);
  TEST_RUN(test_overflowing_solve_rejected);
  TEST_RUN(test_ill_posed_sink_integrates_without_complaint);
  TEST_RUN(test_non_finite_product_fails);
  TEST_RUN(test_tiny_entry_drained_hard);
  TEST_RUN(test_flux_order_independent);
  TEST_RUN(test_unblockable_components_refused);

  TEST_SUMMARY();
  return TEST_RESULT();
}
