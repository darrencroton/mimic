/**
 * @file    rate_test_support.h
 * @brief   Shared harness of the rate integrator unit tests: systems, laws, attempt log, runs
 *
 * Included by test_rate_integrator.c, test_rate_tracers.c and
 * test_rate_implicit.c. A test file may define MAX_ENTRIES, MAX_FLUXES,
 * MAX_ROWS and WORKSPACE_DOUBLES before including it to size its systems and
 * its workspace. Everything here builds or observes a hand-built component; no
 * expected value comes from it: each test writes its closed forms itself.
 *
 * A system is a component under construction with its start state and a
 * constant coefficient per flux. A probe is its law: it sets every flux's
 * constant coefficient and then lets an optional fill function overwrite the
 * coefficients it owns, and it records what states the integrator evaluated
 * the law at. The attempt log records every attempt the integrator reports.
 */

#ifndef TESTS_UNIT_RATE_TEST_SUPPORT_H
#define TESTS_UNIT_RATE_TEST_SUPPORT_H

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "../../src/core/rate_integrator.h"

/* u = 2^-53, the unit roundoff of IEEE binary64. */
#define UNIT_ROUNDOFF 0x1p-53

#ifndef MAX_ENTRIES
#define MAX_ENTRIES 1024
#endif
#ifndef MAX_FLUXES
#define MAX_FLUXES 2048
#endif
#ifndef MAX_ROWS
#define MAX_ROWS 1024
#endif
#ifndef WORKSPACE_DOUBLES
#define WORKSPACE_DOUBLES (1 << 17)
#endif
#define MAX_TOTALS 4
#define LOG_CAPACITY 8192
/* Entries of the committed states the log keeps for the first two accepted steps. */
#define KEPT_ENTRIES 8

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
  int row; /* the row add_reservoir() and add_tracer() put new entries on */
};

/**
 * Start an empty system of @p num_totals totals with @p num_rows rows, each of
 * segment length 0 until the caller sets system->delta; new entries go on the
 * last of them (none: add_row() opens the first).
 */
static inline void system_init(struct System *system, int num_rows, int num_totals) {
  memset(system, 0, sizeof(*system));
  system->num_rows = num_rows;
  system->num_totals = num_totals;
  system->row = num_rows - 1;
}

/** Add a row with segment length @p delta; later entries go on it. */
static inline int add_row(struct System *system, double delta) {
  const int row = system->num_rows++;
  system->delta[row] = delta;
  system->row = row;
  return row;
}

static inline int add_state_entry(struct System *system, int row, enum RateEntryKind kind,
                                  int total, int parent, double content) {
  const int index = system->num_entries++;
  system->entries[index].row = row;
  system->entries[index].total = total;
  system->entries[index].kind = kind;
  system->entries[index].parent = parent;
  system->state[index] = content;
  return index;
}

/** A reservoir of @p total on row @p row. */
static inline int add_entry(struct System *system, int row, int total, double content) {
  return add_state_entry(system, row, RATE_ENTRY_RESERVOIR, total, -1, content);
}

/** A reservoir of @p total on the current row. */
static inline int add_reservoir(struct System *system, int total, double content) {
  return add_entry(system, system->row, total, content);
}

/** A tracer of @p total, of the reservoir @p parent, on the current row. */
static inline int add_tracer(struct System *system, int total, int parent, double content) {
  return add_state_entry(system, system->row, RATE_ENTRY_TRACER, total, parent, content);
}

/** A flux with constant coefficient @p coefficient; @p master is a slaved source's, else -1. */
static inline int add_flux(struct System *system, enum RateFluxKind kind, int from, int to,
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

static inline struct RateComponent component_of(const struct System *system) {
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

struct Probe;

/** A state-dependent law: overwrites the coefficients it owns after the constants are set. */
typedef int (*LawFill)(const struct Probe *probe, double tau, const double *x, double *k);

/**
 * The law of a run and what it saw. K bookkeeping, for a law whose K_max at a
 * state the test can write down (k_of_state): the K at the latest stage-1
 * evaluation (the first evaluation after an accepted step, or of the segment)
 * and the largest K at the other evaluations since the last attempt report.
 */
struct Probe {
  const struct System *system;
  LawFill fill;                          /* NULL: the constant coefficients alone */
  double bad_value;                      /* a faulty law's coefficient */
  int64_t evaluations;                   /* law evaluations seen */
  double min_state;                      /* smallest entry any law was evaluated at */
  int non_finite;                        /* a law was evaluated at an entry that was not finite */
  double (*k_of_state)(const double *x); /* NULL: no K bookkeeping */
  int expect_stage1;
  double k_stage1;
  double k_attempt;
};

static inline struct Probe law_probe(LawFill fill) {
  const struct Probe probe = {.system = NULL,
                              .fill = fill,
                              .bad_value = 0.0,
                              .min_state = INFINITY,
                              .k_of_state = NULL,
                              .expect_stage1 = 1};
  return probe;
}

static inline int probed_law(void *data, double tau, const double *state, double *coefficients) {
  struct Probe *probe = data;
  const struct System *system = probe->system;
  probe->evaluations++;
  for (int i = 0; i < system->num_entries; i++) {
    if (state[i] < probe->min_state) {
      probe->min_state = state[i];
    }
    probe->non_finite |= !isfinite(state[i]);
  }
  if (probe->k_of_state != NULL) {
    const double k = probe->k_of_state(state);
    if (probe->expect_stage1) {
      probe->k_stage1 = k;
      probe->expect_stage1 = 0;
    } else if (k > probe->k_attempt) {
      probe->k_attempt = k;
    }
  }
  for (int f = 0; f < system->num_fluxes; f++) {
    coefficients[f] = system->coefficient[f];
  }
  return (probe->fill != NULL) ? probe->fill(probe, tau, state, coefficients) : 0;
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
  double error_norm[LOG_CAPACITY];
  double min_committed;     /* smallest entry of any committed state */
  int non_finite;           /* a step, weight, error norm or committed entry not finite */
  int bad_step;             /* a step that was not finite and positive */
  int64_t weighed;          /* attempts whose error estimate ran */
  int bad_weight;           /* an estimate with a non-finite or non-positive smallest weight */
  int previous_rejected;    /* the previous attempt was rejected */
  double post_rejection;    /* an accepted step that followed a rejection, else 0 */
  int grew_after_rejection; /* the attempt after it was longer than the landing stretch allows */
  struct Probe *probe;      /* K bookkeeping source, or NULL */
  double worst_k_product;   /* largest step * K_max over the stages of accepted steps */
  int watched[2];           /* declared flux indices whose committed amounts are summed, or -1 */
  double sum[2];            /* sum of the watched fluxes' committed amounts, in step order */
  int null_amounts;         /* an accepted attempt reported no amounts */
  int kept;                 /* accepted steps whose committed state is kept below (at most 2) */
  double kept_step[2];
  double kept_state[2][KEPT_ENTRIES];
};

/** Track step * K_max over every stage of accepted attempts (Probe.k_of_state). */
static inline void check_k_product(struct AttemptLog *log, const struct RateAttempt *attempt) {
  struct Probe *probe = log->probe;
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

/** Record one committed state: its smallest entry, its finiteness, the first two kept. */
static inline void log_committed(struct AttemptLog *log, const struct RateAttempt *attempt) {
  for (int i = 0; i < log->num_entries; i++) {
    if (attempt->state[i] < log->min_committed) {
      log->min_committed = attempt->state[i];
    }
    log->non_finite |= !isfinite(attempt->state[i]);
  }
  if (log->kept < 2 && log->num_entries <= KEPT_ENTRIES) {
    log->kept_step[log->kept] = attempt->step;
    memcpy(log->kept_state[log->kept], attempt->state, (size_t)log->num_entries * sizeof(double));
    log->kept++;
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

static inline void log_attempt(void *data, const struct RateAttempt *attempt) {
  struct AttemptLog *log = data;
  if (log->count < LOG_CAPACITY) {
    log->tau[log->count] = attempt->tau;
    log->step[log->count] = attempt->step;
    log->accepted[log->count] = attempt->accepted;
    log->error_norm[log->count] = attempt->error_norm;
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
    log->non_finite = 1;
  }
  if (attempt->error_norm >= 0.0) {
    log->weighed++;
    if (!isfinite(attempt->min_weight) || !(attempt->min_weight > 0.0)) {
      log->bad_weight = 1;
    }
    if (!isfinite(attempt->error_norm) || !isfinite(attempt->min_weight)) {
      log->non_finite = 1;
    }
  }
  if (attempt->accepted) {
    log_committed(log, attempt);
  }
}

static inline void attach_log(struct RateIntegratorSettings *settings, struct AttemptLog *log,
                              const struct System *system) {
  memset(log, 0, sizeof(*log));
  log->num_entries = system->num_entries;
  log->min_committed = INFINITY;
  log->watched[0] = -1;
  log->watched[1] = -1;
  settings->on_attempt = log_attempt;
  settings->attempt_data = log;
}

/** True when two logs hold the same attempt sequence, bit for bit. */
static inline int same_sequence(const struct AttemptLog *a, const struct AttemptLog *b) {
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

/* The caller-owned workspace every case integrates in, unless it brings its own. */
static double workspace[WORKSPACE_DOUBLES];

static inline struct RateIntegratorSettings adaptive_settings(double relative_tolerance) {
  struct RateIntegratorSettings settings;
  rate_integrator_default_settings(&settings);
  settings.relative_tolerance = relative_tolerance;
  settings.absolute_tolerance = 1e-3 * relative_tolerance;
  return settings;
}

static inline struct RateIntegratorSettings fixed_settings(int64_t steps) {
  struct RateIntegratorSettings settings;
  rate_integrator_default_settings(&settings);
  settings.fixed_steps = steps;
  return settings;
}

/** One segment's outcome, its amounts and what its law saw. */
struct Run {
  enum RateCause cause;
  struct RateSegmentOutcome outcome;
  double source[MAX_TOTALS];
  double sink[MAX_TOTALS];
  double flux[MAX_FLUXES]; /* each declared flux's committed amount (written on success) */
  int64_t evaluations;     /* law evaluations the probe saw */
  double min_state;        /* smallest entry a law saw */
  int non_finite;          /* a law saw an entry that was not finite */
};

/**
 * @brief   Integrate @p system's state in place in @p buffer
 *
 * @p probe is the law (NULL: the constant coefficients alone); its records are
 * reset first and copied into @p run afterwards. @p run's amount arrays are
 * passed as they are, so a caller may seed them to see what a failure leaves.
 *
 * @return  0, or 1 when the buffer is too small
 */
static inline int run_segment_in(struct System *system, struct Probe *probe,
                                 const struct RateIntegratorSettings *settings, void *buffer,
                                 size_t bytes, struct Run *run) {
  const struct RateComponent component = component_of(system);
  if (rate_integrator_workspace_bytes(&component, settings->scheme) > bytes) {
    return 1;
  }
  struct Probe constant = law_probe(NULL);
  struct Probe *used = (probe != NULL) ? probe : &constant;
  used->system = system;
  used->evaluations = 0;
  used->min_state = INFINITY;
  used->non_finite = 0;
  used->expect_stage1 = 1;
  used->k_stage1 = 0.0;
  used->k_attempt = 0.0;
  const struct RateLaw law = {.evaluate = probed_law, .data = used};
  run->cause = rate_integrate_segment(&component, &law, settings, buffer, bytes, system->state,
                                      run->source, run->sink, run->flux, &run->outcome);
  run->evaluations = used->evaluations;
  run->min_state = used->min_state;
  run->non_finite = used->non_finite;
  return 0;
}

/** run_segment_in() in the shared workspace. */
static inline int run_segment(struct System *system, struct Probe *probe,
                              const struct RateIntegratorSettings *settings, struct Run *run) {
  return run_segment_in(system, probe, settings, workspace, sizeof(workspace), run);
}

/** tol_F = 8u [(L + 2) N_acc + n_F + 2] (sum |x(start)| + Src + Snk), L given by the test. */
static inline double closure_tolerance(int l, int64_t accepted, int members, double start_magnitude,
                                       double source, double sink) {
  const double terms = (double)(l + 2) * (double)accepted + (double)members + 2.0;
  return 8.0 * UNIT_ROUNDOFF * terms * (start_magnitude + source + sink);
}

/* ---------------------------------------------------------------------------
 * SplitMix64: the fixed-seed generator of the random components
 * ------------------------------------------------------------------------- */

static inline double next_uniform(uint64_t *seed) {
  uint64_t z = (*seed += 0x9E3779B97F4A7C15ull);
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
  z ^= z >> 31;
  return (double)(z >> 11) * 0x1p-53;
}

static inline double uniform_in(uint64_t *seed, double low, double high) {
  return low + (high - low) * next_uniform(seed);
}

/** A uniform integer in [0, count). */
static inline int uniform_index(uint64_t *seed, int count) {
  const int index = (int)(next_uniform(seed) * (double)count);
  return (index < count) ? index : count - 1;
}

#endif /* TESTS_UNIT_RATE_TEST_SUPPORT_H */
