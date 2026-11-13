/**
 * @file    rate_integrator.c
 * @brief   One rate segment by SSPRK(4,3) or Patankar-Euler: error control, closure, tracer bound
 *
 * The explicit scheme, in Butcher (increment) form:
 *
 *   c    = (0, 1/2, 1, 1/2)
 *   A    = (1/2), (1/2, 1/2), (1/6, 1/6, 1/6)
 *   b    = (1/6, 1/6, 1/6, 1/2)        third order, the update
 *   bhat = (1/4, 1/4, 1/4, 1/4)        second order, embedded
 *   d    = b - bhat = (-1/12, -1/12, -1/12, 1/4)
 *
 * Each flux's stage rate phi (tau units) is formed once per stage from the
 * coefficients the laws return there, slaved sources after every other flux;
 * each stage combination, and the update and error combinations, are formed
 * once per flux and applied to both of the flux's ends through the per-entry
 * outflow and inflow lists, which hold the fluxes in canonical order. Tracer
 * moves are fluxes too: derived once per segment from the parent links, they
 * follow the caller's declared fluxes in the internal flux arrays and take their
 * master's coefficient.
 *
 * The implicit scheme (Patankar-Euler, coefficients frozen at the step start)
 * solves one block per row and total in the subtraction-free
 * Grassmann-Taksar-Heyman form; its section below gives the algorithm. The two
 * schemes share the flux lists, the laws, the error norm, the commit and the
 * segment-end checks. rate_integrator.h states the contract.
 */

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "error.h"
#include "rate_integrator.h"

/* Positivity guard: the scheme's SSP coefficient C and the margin sigma under it. */
#define SSP_COEFFICIENT 2.0
#define POSITIVITY_MARGIN 0.9
/* Step controller: safety factor and clamp of the step factor; landing stretch. */
#define CONTROLLER_SAFETY 0.9
#define CONTROLLER_MIN_FACTOR 0.2
#define CONTROLLER_MAX_FACTOR 5.0
#define LANDING_STRETCH 1.1
/* Error weight: the scale S is at least this fraction of the component's total. */
#define SCALE_FLOOR 1e-12
/* The smallest step, in tau. */
#define MIN_STEP 0x1p-46
/* First-step rule (Hairer and Wanner): thresholds, target and growth bound. */
#define FIRST_STEP_SMALL 1e-5
#define FIRST_STEP_TINY 1e-15
#define FIRST_STEP_TARGET 0.01
#define FIRST_STEP_GROWTH 100.0
/* Segment closure: 8u with u = 2^-53. */
#define CLOSURE_FACTOR 0x1p-50

/**
 * Sort key of a flux: canonical order is (clock row, global transfer index); a
 * tracer move shares its master's pair and follows it, ordered by its tracer.
 */
struct FluxKey {
  int row;
  int global_index;
  int tracer; /* a tracer move's tracer entry; -1 for a declared flux */
  int index;  /* the internal flux index; breaks ties only for a malformed description */
};

/**
 * Scratch carved from the caller's workspace (n entries, m internal fluxes: the
 * caller's declared fluxes, then room for the tracer moves; R rows, T totals).
 */
struct Scratch {
  double *x;             /* [n] accepted state at the current step start */
  double *trial;         /* [n] trial end state; the first step keeps the start weights here */
  double *stage;         /* [n] stage state, or the first step's probe state */
  double *net0;          /* [n] first step: net rate at the start */
  double *net1;          /* [n] first step: net rate at the probe */
  double *coef;          /* [m] coefficients of the latest law evaluation */
  double *phi[4];        /* [m] each: flux rates of stages 1 to 4, tau units */
  double *sum3;          /* [m] phi[0] + phi[1] + phi[2] */
  double *amount;        /* [m] amounts of the current stage, then of the update */
  double *error_amount;  /* [m] (b - bhat) amounts of the attempt */
  double *group_start;   /* [R * T] row sums of each total at the step start */
  double *group_trial;   /* [R * T] row sums of each total at the trial end */
  double *total_start;   /* [T] component sums at the step start */
  double *total_trial;   /* [T] component sums at the trial end */
  double *source_entry;  /* [n] external inflow into each entry over accepted steps */
  double *sink_entry;    /* [n] external outflow from each entry over accepted steps */
  double *source;        /* [T] per-total sums of source_entry, formed once at segment end */
  double *sink;          /* [T] per-total sums of sink_entry, formed once at segment end */
  double *inflow_entry;  /* [n] inflow into each entry over accepted steps (tracer bound) */
  struct RateFlux *flux; /* [m] declared fluxes as given, then the derived tracer moves */
  struct FluxKey *keys;  /* [m] */
  int *order;            /* [m] internal flux indices in canonical order */
  int *clock_row;        /* [m] the row whose Delta scales each flux */
  int *out_start;        /* [n + 1] CSR offsets into out_list */
  int *out_list;         /* outflows of each entry, canonical order */
  int *in_start;         /* [n + 1] CSR offsets into in_list */
  int *in_list;          /* inflows of each entry, canonical order */
  int *tracer_start;     /* [n + 1] CSR offsets into tracer_list */
  int *tracer_list;      /* [n] tracers of each parent, entry order */

  /* Implicit scheme only (NULL under the explicit scheme); B blocks, B <= n. */
  double *coef_trial;   /* [m] coefficients at the trial end */
  double *step_rate;    /* [m] step x rate at the step start: per unit donor content, or a
                           source's amount */
  double *error_flux;   /* [m] per-flux amounts of the error solve */
  double *excess;       /* [n] each entry's column excess while its block is factorised */
  double *pivot;        /* [n] each entry's pivot */
  double *filtered;     /* [n] the error estimate M^-1 (dt/2) (F(x_{n+1}) - F(x_n)) */
  double *dense;        /* [sum of block sizes squared] each block's off-diagonals, then its
                           factors: l below the diagonal, U's off-diagonal magnitudes above */
  size_t *block_offset; /* [n] each block's offset into dense */
  int *block_of;        /* [n] each entry's block; -1 for an observer */
  int *local_of;        /* [n] each entry's position in its block */
  int *block_start;     /* [n + 1] CSR offsets into block_member */
  int *block_member;    /* [n] each block's entries, entry order */
  int *block_order;     /* [n] blocks in solve order */
  int *block_map;       /* [R * T] the block of each (row, total), or -1 */
  int *row_count;       /* [R] row marks, then in-degrees of the row graph */
  int *row_edge_start;  /* [R + 1] CSR offsets into row_edge */
  int *row_edge;        /* [m] recipient row of each cross-row transfer, by donor row */
  int *row_order;       /* [R] rows in solve order */
  int *total_depends;   /* [T * T] total a waits for total b at [a * T + b] */
  int *total_order;     /* [T] totals in solve order */
  int *total_placed;    /* [T] whether each total is in total_order yet */
};

/** One segment's integration: inputs, scratch and the running step-start data. */
struct Integration {
  const struct RateComponent *c;
  const struct RateLaw *law;
  const struct RateIntegratorSettings *settings;
  struct RateSegmentOutcome *outcome;
  struct Scratch s;
  int num_declared;  /* the caller's fluxes: internal indices 0 .. num_declared - 1 */
  int num_fluxes;    /* declared fluxes plus tracer moves */
  int num_tracers;   /* tracer entries */
  double k_start;    /* K_max at the current step start */
  int k_start_entry; /* the entry attaining it, or -1 */
  int k_start_flux;  /* that entry's outflow with the largest coefficient, or -1 */
  int start_entry;   /* the start-of-step limiter: the K_max entry and flux above, or */
  int start_flux;    /* without one the largest-rate flux and its donor (or recipient) */
  int limit_entry;   /* the latest limiter, for the failure record (rate_integrator.h) */
  int limit_flux;
  int num_blocks;        /* implicit scheme: blocks, one per (row, total) with entries */
  size_t dense_capacity; /* implicit scheme: doubles in s.dense */
};

/** How one attempted step ended. */
enum AttemptResult {
  ATTEMPT_ACCEPTED,
  ATTEMPT_REJECTED_POSITIVITY,
  ATTEMPT_REJECTED_ERROR,
  ATTEMPT_FAILED /* a law failed; the failure record is written */
};

/* ---------------------------------------------------------------------------
 * Workspace layout
 * ------------------------------------------------------------------------- */

/** Reserve @p count elements of @p size bytes at the 16-byte-aligned @p *offset. */
static void *carve(char *base, size_t *offset, size_t count, size_t size) {
  void *block = (base != NULL) ? (void *)(base + *offset) : NULL;
  *offset += (count * size + 15u) & ~(size_t)15u;
  return block;
}

/**
 * @brief   Room for the tracer moves: every declared flux times the most tracers one parent has
 *
 * A parent's tracers lie in distinct totals other than its own (checked in
 * build_tracer_lists()), so it has at most min(tracer entries, T - 1) of them.
 */
static size_t tracer_move_capacity(const struct RateComponent *c) {
  size_t tracers = 0;
  for (int i = 0; i < c->num_entries; i++) {
    if (c->entries[i].kind == RATE_ENTRY_TRACER) {
      tracers++;
    }
  }
  const size_t other_totals = (c->num_totals > 1) ? (size_t)(c->num_totals - 1) : 0;
  const size_t per_flux = (tracers < other_totals) ? tracers : other_totals;
  return (size_t)c->num_fluxes * per_flux;
}

/**
 * @brief   Doubles the implicit blocks need: the sum over blocks of their size squared
 *
 * A block is the reservoir and tracer entries of one row in one total. With each
 * row's entries listed together (checked by implicit_check_rows()), a row is one
 * run of the entry list, and its blocks need the number of ordered pairs of its
 * entries that share a total; the count costs the square of the entries per
 * row, never of the number of rows.
 */
static size_t implicit_dense_capacity(const struct RateComponent *c) {
  size_t capacity = 0;
  int run_start = 0;
  while (run_start < c->num_entries) {
    int run_end = run_start;
    while (run_end < c->num_entries && c->entries[run_end].row == c->entries[run_start].row) {
      run_end++;
    }
    for (int a = run_start; a < run_end; a++) {
      if (c->entries[a].kind == RATE_ENTRY_OBSERVER) {
        continue;
      }
      for (int b = run_start; b < run_end; b++) {
        if (c->entries[b].kind != RATE_ENTRY_OBSERVER &&
            c->entries[b].total == c->entries[a].total) {
          capacity++;
        }
      }
    }
    run_start = run_end;
  }
  return capacity;
}

/** A scheme the integrator does not know is a programming error. */
static void check_scheme(enum RateScheme scheme) {
  if (scheme != RATE_SCHEME_EXPLICIT && scheme != RATE_SCHEME_IMPLICIT) {
    FATAL_ERROR("Rate integrator scheme %d is neither explicit (%d) nor implicit (%d)", (int)scheme,
                (int)RATE_SCHEME_EXPLICIT, (int)RATE_SCHEME_IMPLICIT);
  }
}

/**
 * @brief   Lay the scratch out over @p base (or only measure it when NULL); returns the bytes
 *
 * The explicit layout comes first and is the same under both schemes; the
 * implicit scheme's arrays follow it, and are NULL under the explicit scheme.
 */
static size_t layout_scratch(const struct RateComponent *c, enum RateScheme scheme, char *base,
                             struct Scratch *s) {
  const size_t n = (size_t)c->num_entries;
  const size_t m = (size_t)c->num_fluxes + tracer_move_capacity(c);
  const size_t totals = (size_t)c->num_totals;
  const size_t rows = (size_t)c->num_rows;
  const size_t groups = rows * totals;
  size_t offset = 0;

  s->x = carve(base, &offset, n, sizeof(double));
  s->trial = carve(base, &offset, n, sizeof(double));
  s->stage = carve(base, &offset, n, sizeof(double));
  s->net0 = carve(base, &offset, n, sizeof(double));
  s->net1 = carve(base, &offset, n, sizeof(double));
  s->coef = carve(base, &offset, m, sizeof(double));
  for (int k = 0; k < 4; k++) {
    s->phi[k] = carve(base, &offset, m, sizeof(double));
  }
  s->sum3 = carve(base, &offset, m, sizeof(double));
  s->amount = carve(base, &offset, m, sizeof(double));
  s->error_amount = carve(base, &offset, m, sizeof(double));
  s->group_start = carve(base, &offset, groups, sizeof(double));
  s->group_trial = carve(base, &offset, groups, sizeof(double));
  s->total_start = carve(base, &offset, totals, sizeof(double));
  s->total_trial = carve(base, &offset, totals, sizeof(double));
  s->source_entry = carve(base, &offset, n, sizeof(double));
  s->sink_entry = carve(base, &offset, n, sizeof(double));
  s->source = carve(base, &offset, totals, sizeof(double));
  s->sink = carve(base, &offset, totals, sizeof(double));
  s->inflow_entry = carve(base, &offset, n, sizeof(double));
  s->flux = carve(base, &offset, m, sizeof(struct RateFlux));
  s->keys = carve(base, &offset, m, sizeof(struct FluxKey));
  s->order = carve(base, &offset, m, sizeof(int));
  s->clock_row = carve(base, &offset, m, sizeof(int));
  s->out_start = carve(base, &offset, n + 1, sizeof(int));
  s->out_list = carve(base, &offset, m, sizeof(int));
  s->in_start = carve(base, &offset, n + 1, sizeof(int));
  s->in_list = carve(base, &offset, m, sizeof(int));
  s->tracer_start = carve(base, &offset, n + 1, sizeof(int));
  s->tracer_list = carve(base, &offset, n, sizeof(int));

  if (scheme != RATE_SCHEME_IMPLICIT) {
    s->coef_trial = s->step_rate = s->error_flux = NULL;
    s->excess = s->pivot = s->filtered = s->dense = NULL;
    s->block_offset = NULL;
    s->block_of = s->local_of = s->block_start = s->block_member = s->block_order = NULL;
    s->block_map = s->row_count = s->row_edge_start = s->row_edge = s->row_order = NULL;
    s->total_depends = s->total_order = s->total_placed = NULL;
    return offset;
  }
  s->coef_trial = carve(base, &offset, m, sizeof(double));
  s->step_rate = carve(base, &offset, m, sizeof(double));
  s->error_flux = carve(base, &offset, m, sizeof(double));
  s->excess = carve(base, &offset, n, sizeof(double));
  s->pivot = carve(base, &offset, n, sizeof(double));
  s->filtered = carve(base, &offset, n, sizeof(double));
  s->dense = carve(base, &offset, implicit_dense_capacity(c), sizeof(double));
  s->block_offset = carve(base, &offset, n, sizeof(size_t));
  s->block_of = carve(base, &offset, n, sizeof(int));
  s->local_of = carve(base, &offset, n, sizeof(int));
  s->block_start = carve(base, &offset, n + 1, sizeof(int));
  s->block_member = carve(base, &offset, n, sizeof(int));
  s->block_order = carve(base, &offset, n, sizeof(int));
  s->block_map = carve(base, &offset, groups, sizeof(int));
  s->row_count = carve(base, &offset, rows, sizeof(int));
  s->row_edge_start = carve(base, &offset, rows + 1, sizeof(int));
  s->row_edge = carve(base, &offset, m, sizeof(int));
  s->row_order = carve(base, &offset, rows, sizeof(int));
  s->total_depends = carve(base, &offset, totals * totals, sizeof(int));
  s->total_order = carve(base, &offset, totals, sizeof(int));
  s->total_placed = carve(base, &offset, totals, sizeof(int));
  return offset;
}

size_t rate_integrator_workspace_bytes(const struct RateComponent *component,
                                       enum RateScheme scheme) {
  check_scheme(scheme);
  struct Scratch unused;
  return layout_scratch(component, scheme, NULL, &unused);
}

/* ---------------------------------------------------------------------------
 * Canonical flux order and the per-entry flux lists
 * ------------------------------------------------------------------------- */

static int compare_flux_keys(const void *a, const void *b) {
  const struct FluxKey *ka = a;
  const struct FluxKey *kb = b;
  if (ka->row != kb->row) {
    return (ka->row < kb->row) ? -1 : 1;
  }
  if (ka->global_index != kb->global_index) {
    return (ka->global_index < kb->global_index) ? -1 : 1;
  }
  if (ka->tracer != kb->tracer) {
    return (ka->tracer < kb->tracer) ? -1 : 1;
  }
  return (ka->index > kb->index) - (ka->index < kb->index);
}

/** True when flux @p f moves content out of entry `from` (a self-transfer moves nothing). */
static int flux_has_donor(const struct RateFlux *f) {
  return (f->kind == RATE_FLUX_INTERNAL || f->kind == RATE_FLUX_SINK) && f->from != f->to;
}

/** True when flux @p f moves content into entry `to`. */
static int flux_has_recipient(const struct RateFlux *f) {
  return f->kind != RATE_FLUX_SINK && f->from != f->to;
}

/** True when flux @p f enters the component from outside: a source or a slaved source. */
static int flux_is_external_source(const struct RateFlux *f) {
  return f->kind == RATE_FLUX_SOURCE || f->kind == RATE_FLUX_SLAVED;
}

/** The entry a failure record names for flux @p f: its donor, or its recipient for a source. */
static int flux_named_entry(const struct RateFlux *f) {
  return flux_is_external_source(f) ? f->to : f->from;
}

/** Turn per-entry counts in start[1..n] into CSR offsets start[0..n]. */
static void counts_to_offsets(int *start, int n) {
  start[0] = 0;
  for (int i = 0; i < n; i++) {
    start[i + 1] += start[i];
  }
}

/** After filling with start[i] as a cursor, shift the offsets back into place. */
static void restore_offsets(int *start, int n) {
  for (int i = n; i > 0; i--) {
    start[i] = start[i - 1];
  }
  start[0] = 0;
}

/**
 * @brief   Check every tracer's parent link and list each parent's tracers in entry order
 *
 * A tracer's parent is a reservoir entry of the same row in another total, and
 * one parent has at most one tracer per total; anything else is a programming
 * error and fatal, since the tracer moves and the workspace size rely on it.
 */
static void build_tracer_lists(struct Integration *in) {
  const struct RateComponent *c = in->c;
  struct Scratch *s = &in->s;
  const int n = c->num_entries;

  memset(s->tracer_start, 0, (size_t)(n + 1) * sizeof(int));
  in->num_tracers = 0;
  for (int z = 0; z < n; z++) {
    if (c->entries[z].kind != RATE_ENTRY_TRACER) {
      continue;
    }
    const int p = c->entries[z].parent;
    if (p < 0 || p >= n || c->entries[p].kind != RATE_ENTRY_RESERVOIR ||
        c->entries[p].row != c->entries[z].row || c->entries[p].total == c->entries[z].total) {
      FATAL_ERROR("Rate tracer entry %d names parent %d, which is not a reservoir entry of its "
                  "row in another total",
                  z, p);
    }
    s->tracer_start[p + 1]++;
    in->num_tracers++;
  }
  counts_to_offsets(s->tracer_start, n);
  for (int z = 0; z < n; z++) {
    if (c->entries[z].kind == RATE_ENTRY_TRACER) {
      s->tracer_list[s->tracer_start[c->entries[z].parent]++] = z;
    }
  }
  restore_offsets(s->tracer_start, n);
  for (int p = 0; p < n; p++) {
    for (int a = s->tracer_start[p]; a < s->tracer_start[p + 1]; a++) {
      for (int b = a + 1; b < s->tracer_start[p + 1]; b++) {
        if (c->entries[s->tracer_list[a]].total == c->entries[s->tracer_list[b]].total) {
          FATAL_ERROR("Rate tracer entries %d and %d share parent %d and total %d",
                      s->tracer_list[a], s->tracer_list[b], p, c->entries[s->tracer_list[a]].total);
        }
      }
    }
  }
}

/**
 * @brief   Check every observer's flux and every slaved source's master; fatal when malformed
 *
 * Slaved sources are one level deep: a master is a declared internal transfer
 * or sink, never a source or another slaved source.
 */
static void check_links(const struct RateComponent *c) {
  for (int i = 0; i < c->num_entries; i++) {
    const struct RateEntry *entry = &c->entries[i];
    if (entry->kind == RATE_ENTRY_OBSERVER && (entry->flux < 0 || entry->flux >= c->num_fluxes)) {
      FATAL_ERROR("Rate observer entry %d names flux %d, which the component does not declare", i,
                  entry->flux);
    }
  }
  for (int f = 0; f < c->num_fluxes; f++) {
    if (c->fluxes[f].kind != RATE_FLUX_SLAVED) {
      continue;
    }
    const int master = c->fluxes[f].master;
    if (master < 0 || master >= c->num_fluxes ||
        (c->fluxes[master].kind != RATE_FLUX_INTERNAL &&
         c->fluxes[master].kind != RATE_FLUX_SINK)) {
      FATAL_ERROR("Rate slaved source %d names master %d, which is not a declared internal "
                  "transfer or sink (slaved sources are one level deep)",
                  f, master);
    }
  }
}

/** The tracer of entry @p parent in total @p total, or -1. */
static int tracer_in_total(const struct Integration *in, int parent, int total) {
  const struct Scratch *s = &in->s;
  for (int k = s->tracer_start[parent]; k < s->tracer_start[parent + 1]; k++) {
    if (in->c->entries[s->tracer_list[k]].total == total) {
      return s->tracer_list[k];
    }
  }
  return -1;
}

/**
 * @brief   Copy the declared fluxes and append the tracer moves (rate_integrator.h, Tracers)
 *
 * For every declared flux with a donor, each tracer of the donor moves into the
 * recipient's tracer of the same total, or leaves the component as a sink when
 * the flux is a sink or the recipient has no such tracer. A move keeps its
 * master's global index and names the master in `master`.
 */
static void derive_tracer_moves(struct Integration *in) {
  const struct RateComponent *c = in->c;
  struct Scratch *s = &in->s;
  const size_t capacity = tracer_move_capacity(c);
  int count = c->num_fluxes;

  memcpy(s->flux, c->fluxes, (size_t)c->num_fluxes * sizeof(struct RateFlux));
  for (int f = 0; f < c->num_fluxes; f++) {
    const struct RateFlux *master = &c->fluxes[f];
    if (!flux_has_donor(master)) {
      continue;
    }
    for (int k = s->tracer_start[master->from]; k < s->tracer_start[master->from + 1]; k++) {
      const int z = s->tracer_list[k];
      const int target = (master->kind == RATE_FLUX_INTERNAL)
                             ? tracer_in_total(in, master->to, c->entries[z].total)
                             : -1;
      if ((size_t)(count - c->num_fluxes) >= capacity) {
        FATAL_ERROR("Rate component needs more tracer moves than its workspace holds (%zu)",
                    capacity);
      }
      struct RateFlux *move = &s->flux[count++];
      move->kind = (target >= 0) ? RATE_FLUX_INTERNAL : RATE_FLUX_SINK;
      move->from = z;
      move->to = (target >= 0) ? target : RATE_END_EXTERNAL;
      move->global_index = master->global_index;
      move->master = f;
    }
  }
  in->num_declared = c->num_fluxes;
  in->num_fluxes = count;
}

/** The row whose Delta scales internal flux @p f (rate_integrator.h, Time). */
static int clock_row_of(const struct Integration *in, int f) {
  const struct RateFlux *flux = &in->s.flux[f];
  if (flux->kind == RATE_FLUX_SOURCE) {
    return in->c->entries[flux->to].row;
  }
  if (flux->kind == RATE_FLUX_SLAVED) {
    return in->c->entries[in->s.flux[flux->master].from].row;
  }
  return in->c->entries[flux->from].row;
}

/**
 * @brief   Derive the tracer moves, sort every flux canonically and build each entry's lists
 * @return  L, the largest number of inflow plus outflow terms on one entry
 */
static int build_flux_lists(struct Integration *in) {
  const struct RateComponent *c = in->c;
  struct Scratch *s = &in->s;
  const int n = c->num_entries;

  build_tracer_lists(in);
  check_links(c);
  derive_tracer_moves(in);
  const int m = in->num_fluxes;

  for (int f = 0; f < m; f++) {
    s->clock_row[f] = clock_row_of(in, f);
    s->keys[f].row = s->clock_row[f];
    s->keys[f].global_index = s->flux[f].global_index;
    s->keys[f].tracer = (f < in->num_declared) ? -1 : s->flux[f].from;
    s->keys[f].index = f;
  }
  if (m > 1) {
    qsort(s->keys, (size_t)m, sizeof(struct FluxKey), compare_flux_keys);
  }
  for (int k = 0; k < m; k++) {
    s->order[k] = s->keys[k].index;
  }

  memset(s->out_start, 0, (size_t)(n + 1) * sizeof(int));
  memset(s->in_start, 0, (size_t)(n + 1) * sizeof(int));
  for (int f = 0; f < m; f++) {
    const struct RateFlux *flux = &s->flux[f];
    if (flux_has_donor(flux)) {
      s->out_start[flux->from + 1]++;
    }
    if (flux_has_recipient(flux)) {
      s->in_start[flux->to + 1]++;
    }
  }
  int max_terms = 0;
  for (int i = 0; i < n; i++) {
    const int terms = s->out_start[i + 1] + s->in_start[i + 1];
    if (terms > max_terms) {
      max_terms = terms;
    }
  }
  counts_to_offsets(s->out_start, n);
  counts_to_offsets(s->in_start, n);
  for (int k = 0; k < m; k++) {
    const int f = s->order[k];
    const struct RateFlux *flux = &s->flux[f];
    if (flux_has_donor(flux)) {
      s->out_list[s->out_start[flux->from]++] = f;
    }
    if (flux_has_recipient(flux)) {
      s->in_list[s->in_start[flux->to]++] = f;
    }
  }
  restore_offsets(s->out_start, n);
  restore_offsets(s->in_start, n);
  return max_terms;
}

/* ---------------------------------------------------------------------------
 * Failure records and attempt reports
 * ------------------------------------------------------------------------- */

/** Record a failure; @p flux is an internal index, named to the caller by its master if a move. */
static enum RateCause record_failure(struct Integration *in, enum RateCause cause, int entry,
                                     int flux, double tau, double step) {
  struct RateFailure *failure = &in->outcome->failure;
  failure->cause = cause;
  failure->entry = entry;
  failure->flux = (flux >= in->num_declared) ? in->s.flux[flux].master : flux;
  if (entry >= 0) {
    failure->row = in->c->entries[entry].row;
  } else if (flux >= 0) {
    failure->row = in->s.clock_row[flux];
  } else {
    failure->row = -1;
  }
  failure->total = -1;
  failure->tau = tau;
  failure->step = step;
  failure->detail = "";
  return cause;
}

/** Report an attempt; an accepted one carries the declared fluxes' committed amounts. */
static void report_attempt(const struct Integration *in, double tau, double step, int accepted,
                           enum RateRejection rejection, double error_norm, double min_weight) {
  if (in->settings->on_attempt == NULL) {
    return;
  }
  const struct RateAttempt attempt = {
      .tau = tau,
      .step = step,
      .accepted = accepted,
      .rejection = rejection,
      .error_norm = error_norm,
      .min_weight = min_weight,
      .state = in->s.x,
      .amounts = accepted ? in->s.amount : NULL,
  };
  in->settings->on_attempt(in->settings->attempt_data, &attempt);
}

/* ---------------------------------------------------------------------------
 * Laws, rates and the positivity measure
 * ------------------------------------------------------------------------- */

/**
 * @brief   Evaluate the laws at @p state and check every coefficient
 *
 * A failure is recorded against the step that starts at @p step_tau with length
 * @p step; the first bad coefficient in canonical order is named. Each tracer
 * move then takes its master's coefficient.
 */
static enum RateCause evaluate_law(struct Integration *in, double eval_tau, const double *state,
                                   double step_tau, double step) {
  struct Scratch *s = &in->s;

  memset(s->coef, 0, (size_t)in->num_declared * sizeof(double));
  in->outcome->stats.law_evaluations++;
  if (in->law->evaluate(in->law->data, eval_tau, state, s->coef) != 0) {
    return record_failure(in, RATE_CAUSE_BAD_COEFFICIENT, -1, -1, step_tau, step);
  }
  for (int k = 0; k < in->num_fluxes; k++) {
    const int f = s->order[k];
    if (f >= in->num_declared) {
      continue;
    }
    const double coefficient = s->coef[f];
    if (!(coefficient >= 0.0) || isinf(coefficient)) {
      return record_failure(in, RATE_CAUSE_BAD_COEFFICIENT, flux_named_entry(&s->flux[f]), f,
                            step_tau, step);
    }
  }
  for (int f = in->num_declared; f < in->num_fluxes; f++) {
    s->coef[f] = s->coef[s->flux[f].master];
  }
  return RATE_CAUSE_NONE;
}

/**
 * @brief   Each flux's rate in tau units at @p state, from the latest coefficients
 *
 * Slaved sources are formed after every other flux: the coefficient times the
 * master's rate, or zero when the master moves nothing.
 */
static void flux_rates(const struct Integration *in, const double *state, double *phi) {
  const struct RateComponent *c = in->c;
  const struct Scratch *s = &in->s;
  for (int f = 0; f < in->num_fluxes; f++) {
    const struct RateFlux *flux = &s->flux[f];
    if (flux->kind == RATE_FLUX_SLAVED) {
      continue;
    }
    const double scaled = c->row_delta[s->clock_row[f]] * s->coef[f];
    phi[f] = (flux->kind == RATE_FLUX_SOURCE) ? scaled : scaled * state[flux->from];
  }
  for (int f = 0; f < in->num_declared; f++) {
    const struct RateFlux *flux = &s->flux[f];
    if (flux->kind == RATE_FLUX_SLAVED) {
      const struct RateFlux *master = &s->flux[flux->master];
      phi[f] = (master->from != master->to) ? s->coef[f] * phi[flux->master] : 0.0;
    }
  }
}

/** The outflow of @p entry with the largest value in @p values (first on ties), or -1. */
static int largest_outflow(const struct Integration *in, int entry, const double *values) {
  const struct Scratch *s = &in->s;
  int best = -1;
  for (int k = s->out_start[entry]; k < s->out_start[entry + 1]; k++) {
    const int f = s->out_list[k];
    if (best < 0 || values[f] > values[best]) {
      best = f;
    }
  }
  return best;
}

/**
 * @brief   K_max at @p state from the latest coefficients
 *
 * The largest Delta * (sum of outflow coefficients) over entries with non-zero
 * content; 0 when no such entry has an outflow. Feeds the step statistics.
 */
static double k_max(struct Integration *in, const double *state, int *argmax) {
  const struct RateComponent *c = in->c;
  const struct Scratch *s = &in->s;
  double largest = 0.0;
  int arg = -1;
  for (int i = 0; i < c->num_entries; i++) {
    if (state[i] == 0.0) {
      continue;
    }
    double coefficient_sum = 0.0;
    for (int k = s->out_start[i]; k < s->out_start[i + 1]; k++) {
      coefficient_sum += s->coef[s->out_list[k]];
    }
    const double k_i = c->row_delta[c->entries[i].row] * coefficient_sum;
    if (k_i > largest) {
      largest = k_i;
      arg = i;
    }
  }
  if (largest > in->outcome->stats.max_k_max) {
    in->outcome->stats.max_k_max = largest;
  }
  if (argmax != NULL) {
    *argmax = arg;
  }
  return largest;
}

/** The step cap sigma * C / K_max; +infinity when K_max is zero. */
static double positivity_cap(double k) {
  return (k > 0.0) ? (POSITIVITY_MARGIN * SSP_COEFFICIENT) / k : INFINITY;
}

/**
 * @brief   Form (x - sum of outflow amounts) + sum of inflow amounts per entry
 * @return  The first reservoir or tracer entry below zero or not a number, or -1
 *
 * An observer has no flux lists, so it keeps its value here; commit_step() adds
 * its amount.
 */
static int form_state(const struct Integration *in, const double *x, const double *amount,
                      double *out) {
  const struct Scratch *s = &in->s;
  int first_bad = -1;
  for (int i = 0; i < in->c->num_entries; i++) {
    double outflow = 0.0;
    for (int k = s->out_start[i]; k < s->out_start[i + 1]; k++) {
      outflow += amount[s->out_list[k]];
    }
    double inflow = 0.0;
    for (int k = s->in_start[i]; k < s->in_start[i + 1]; k++) {
      inflow += amount[s->in_list[k]];
    }
    const double value = (x[i] - outflow) + inflow;
    out[i] = value;
    if (!(value >= 0.0) && first_bad < 0 && in->c->entries[i].kind != RATE_ENTRY_OBSERVER) {
      first_bad = i;
    }
  }
  return first_bad;
}

/** Net change per entry from per-flux @p values: sum of inflows less sum of outflows. */
static void net_change(const struct Integration *in, const double *values, double *net) {
  const struct Scratch *s = &in->s;
  for (int i = 0; i < in->c->num_entries; i++) {
    double outflow = 0.0;
    for (int k = s->out_start[i]; k < s->out_start[i + 1]; k++) {
      outflow += values[s->out_list[k]];
    }
    double inflow = 0.0;
    for (int k = s->in_start[i]; k < s->in_start[i + 1]; k++) {
      inflow += values[s->in_list[k]];
    }
    net[i] = inflow - outflow;
  }
}

/**
 * @brief   Set the start-of-step limiter from the stage-1 coefficients and rates
 *
 * The entry attaining K_max and its outflow with the largest coefficient; when
 * no entry with content has an outflow, the flux with the largest stage-1 rate
 * (first in canonical order on ties) and its donor, or its recipient for a
 * source. It seeds the failure record's limiter while no other limiter is set.
 */
static void set_start_limiter(struct Integration *in) {
  const struct Scratch *s = &in->s;
  in->start_entry = in->k_start_entry;
  in->start_flux = in->k_start_flux;
  if (in->start_entry >= 0) {
    return;
  }
  for (int k = 0; k < in->num_fluxes; k++) {
    const int f = s->order[k];
    if (in->start_flux < 0 || s->phi[0][f] > s->phi[0][in->start_flux]) {
      in->start_flux = f;
    }
  }
  if (in->start_flux >= 0) {
    in->start_entry = flux_named_entry(&s->flux[in->start_flux]);
  }
}

/**
 * @brief   Adopt the coefficients in s.coef as those of the accepted state s.x
 *
 * Forms the stage-1 rates and K_max of the next step, and seeds the failure
 * record's limiter with the start-of-step limiter while no other limiter has
 * been met in the segment.
 */
static void adopt_start(struct Integration *in) {
  struct Scratch *s = &in->s;
  flux_rates(in, s->x, s->phi[0]);
  in->k_start = k_max(in, s->x, &in->k_start_entry);
  in->k_start_flux =
      (in->k_start_entry >= 0) ? largest_outflow(in, in->k_start_entry, s->coef) : -1;
  set_start_limiter(in);
  if (in->limit_entry < 0) {
    in->limit_entry = in->start_entry;
    in->limit_flux = in->start_flux;
  }
}

/** Evaluate the laws at the accepted state and adopt them for the next step (adopt_start()). */
static enum RateCause begin_step(struct Integration *in, double tau, double step) {
  struct Scratch *s = &in->s;
  const enum RateCause cause = evaluate_law(in, tau, s->x, tau, step);
  if (cause != RATE_CAUSE_NONE) {
    return cause;
  }
  adopt_start(in);
  return RATE_CAUSE_NONE;
}

/* ---------------------------------------------------------------------------
 * Error estimate
 * ------------------------------------------------------------------------- */

/** Per-row and per-component sums of each total at @p state, in entry order; no observer. */
static void sum_totals(const struct Integration *in, const double *state, double *group,
                       double *total) {
  const struct RateComponent *c = in->c;
  const int totals = c->num_totals;
  memset(group, 0, (size_t)c->num_rows * (size_t)totals * sizeof(double));
  memset(total, 0, (size_t)totals * sizeof(double));
  for (int i = 0; i < c->num_entries; i++) {
    const struct RateEntry *entry = &c->entries[i];
    if (entry->kind == RATE_ENTRY_OBSERVER) {
      continue;
    }
    group[(size_t)entry->row * (size_t)totals + (size_t)entry->total] += state[i];
    total[entry->total] += state[i];
  }
}

/** The total whose sums scale entry @p i's weight: its own, or its parent's for a tracer. */
static int scale_total(const struct RateComponent *c, int i) {
  const struct RateEntry *entry = &c->entries[i];
  return (entry->kind == RATE_ENTRY_TRACER) ? c->entries[entry->parent].total : entry->total;
}

/**
 * @brief   err = max_i |e_i| / w_i of the attempt (rate_integrator.h, Error control)
 *
 * @p error holds e_i per entry; the weights use the step start s.x and the trial
 * end s.trial. Records the entry with the largest ratio, and its flux with the
 * largest error amount in s.error_amount, as the step's limiter.
 */
static double weighted_error_norm(struct Integration *in, const double *error, double *min_weight) {
  const struct RateComponent *c = in->c;
  struct Scratch *s = &in->s;
  const double atol = in->settings->absolute_tolerance;
  const double rtol = in->settings->relative_tolerance;

  sum_totals(in, s->x, s->group_start, s->total_start);
  sum_totals(in, s->trial, s->group_trial, s->total_trial);

  double norm = 0.0;
  double smallest_weight = INFINITY;
  int arg = -1;
  for (int i = 0; i < c->num_entries; i++) {
    if (c->entries[i].kind == RATE_ENTRY_OBSERVER) {
      continue;
    }
    const int t = scale_total(c, i);
    const size_t g = (size_t)c->entries[i].row * (size_t)c->num_totals + (size_t)t;
    double scale = fmax(s->group_start[g], s->group_trial[g]);
    const double scale_floor = SCALE_FLOOR * fmax(s->total_start[t], s->total_trial[t]);
    if (scale < scale_floor) {
      scale = scale_floor;
    }
    const double weight = atol * scale + rtol * fmax(fabs(s->x[i]), fabs(s->trial[i]));
    if (weight < smallest_weight) {
      smallest_weight = weight;
    }
    const double e = error[i];
    double ratio = (e == 0.0) ? 0.0 : fabs(e) / weight;
    if (!(ratio <= INFINITY)) {
      ratio = INFINITY; /* NaN */
    }
    if (ratio > norm) {
      norm = ratio;
      arg = i;
    }
  }
  if (arg >= 0) {
    in->limit_entry = arg;
    in->limit_flux = -1;
    double largest = -1.0;
    for (int k = s->out_start[arg]; k < s->out_start[arg + 1]; k++) {
      const int f = s->out_list[k];
      if (fabs(s->error_amount[f]) > largest) {
        largest = fabs(s->error_amount[f]);
        in->limit_flux = f;
      }
    }
    for (int k = s->in_start[arg]; k < s->in_start[arg + 1]; k++) {
      const int f = s->in_list[k];
      if (fabs(s->error_amount[f]) > largest) {
        largest = fabs(s->error_amount[f]);
        in->limit_flux = f;
      }
    }
  }
  *min_weight = smallest_weight;
  return norm;
}

/** The controller's step factor 0.9 err^(-1/3), clamped to [0.2, 5]. */
static double step_factor(double norm) {
  if (norm == 0.0) {
    return CONTROLLER_MAX_FACTOR;
  }
  const double factor = CONTROLLER_SAFETY / cbrt(norm);
  return fmin(CONTROLLER_MAX_FACTOR, fmax(CONTROLLER_MIN_FACTOR, factor));
}

/* ---------------------------------------------------------------------------
 * One attempted step
 * ------------------------------------------------------------------------- */

/** Name @p entry (below zero) and its outflow with the largest stage amount as the limiter. */
static void limit_by_negative_entry(struct Integration *in, int entry) {
  in->limit_entry = entry;
  in->limit_flux = largest_outflow(in, entry, in->s.amount);
}

/** Name @p entry (attaining K_max) and its outflow with the largest coefficient. */
static void limit_by_k(struct Integration *in, int entry) {
  in->limit_entry = entry;
  in->limit_flux = (entry >= 0) ? largest_outflow(in, entry, in->s.coef) : -1;
}

/**
 * @brief   Form one inner stage from the current amounts and evaluate the laws there
 *
 * Rejects (positivity) before evaluating when the stage state has an entry
 * below zero, and after evaluating when step * K_max at the stage exceeds C.
 */
static enum AttemptResult run_stage(struct Integration *in, double tau, double step, double c_s,
                                    double *phi, double *k_seen) {
  struct Scratch *s = &in->s;
  const int negative = form_state(in, s->x, s->amount, s->stage);
  if (negative >= 0) {
    limit_by_negative_entry(in, negative);
    return ATTEMPT_REJECTED_POSITIVITY;
  }
  if (evaluate_law(in, tau + c_s * step, s->stage, tau, step) != RATE_CAUSE_NONE) {
    return ATTEMPT_FAILED;
  }
  flux_rates(in, s->stage, phi);
  int arg = -1;
  const double k = k_max(in, s->stage, &arg);
  if (k > *k_seen) {
    *k_seen = k;
  }
  if (step * k > SSP_COEFFICIENT) {
    limit_by_k(in, arg);
    return ATTEMPT_REJECTED_POSITIVITY;
  }
  return ATTEMPT_ACCEPTED;
}

/**
 * @brief   Attempt one step of length @p step from the accepted state at @p tau
 *
 * On acceptance s.trial holds the end state and s.amount the update amounts.
 * @p norm and @p min_weight are written when the error estimate ran (adaptive
 * mode, every stage and the end state non-negative), and left negative
 * otherwise; @p k_seen is the largest K_max met in the attempt.
 */
static enum AttemptResult attempt_step(struct Integration *in, double tau, double step,
                                       int adaptive, double *norm, double *min_weight,
                                       double *k_seen) {
  struct Scratch *s = &in->s;
  const int m = in->num_fluxes;
  const double *phi0 = s->phi[0];
  enum AttemptResult result;

  *norm = -1.0;
  *min_weight = -1.0;
  *k_seen = in->k_start;
  if (step * in->k_start > SSP_COEFFICIENT) {
    in->limit_entry = in->k_start_entry;
    in->limit_flux = in->k_start_flux;
    return ATTEMPT_REJECTED_POSITIVITY;
  }

  /* Stage 2 at c = 1/2: A row (1/2). */
  for (int f = 0; f < m; f++) {
    s->amount[f] = step * (0.5 * phi0[f]);
  }
  result = run_stage(in, tau, step, 0.5, s->phi[1], k_seen);
  if (result != ATTEMPT_ACCEPTED) {
    return result;
  }

  /* Stage 3 at c = 1: A row (1/2, 1/2). */
  for (int f = 0; f < m; f++) {
    s->amount[f] = step * (0.5 * (phi0[f] + s->phi[1][f]));
  }
  result = run_stage(in, tau, step, 1.0, s->phi[2], k_seen);
  if (result != ATTEMPT_ACCEPTED) {
    return result;
  }

  /* Stage 4 at c = 1/2: A row (1/6, 1/6, 1/6). */
  for (int f = 0; f < m; f++) {
    s->sum3[f] = (phi0[f] + s->phi[1][f]) + s->phi[2][f];
    s->amount[f] = step * (s->sum3[f] / 6.0);
  }
  result = run_stage(in, tau, step, 0.5, s->phi[3], k_seen);
  if (result != ATTEMPT_ACCEPTED) {
    return result;
  }

  /* Update with b = (1/6, 1/6, 1/6, 1/2); error amounts with d = b - bhat. A flux whose two
   * ends are one entry moves nothing, so its committed amount, the one the attempt report and
   * the observers read, is zero. */
  for (int f = 0; f < m; f++) {
    const double phi3 = s->phi[3][f];
    const int moves = s->flux[f].from != s->flux[f].to;
    s->amount[f] = moves ? step * (s->sum3[f] / 6.0 + 0.5 * phi3) : 0.0;
    s->error_amount[f] = step * (0.25 * phi3 - s->sum3[f] / 12.0);
  }
  const int negative = form_state(in, s->x, s->amount, s->trial);
  if (negative >= 0) {
    limit_by_negative_entry(in, negative);
    return ATTEMPT_REJECTED_POSITIVITY;
  }
  if (!adaptive) {
    return ATTEMPT_ACCEPTED;
  }
  net_change(in, s->error_amount, s->stage);
  *norm = weighted_error_norm(in, s->stage, min_weight);
  return (*norm <= 1.0) ? ATTEMPT_ACCEPTED : ATTEMPT_REJECTED_ERROR;
}

/**
 * @brief   Commit an accepted attempt: the trial becomes the state
 *
 * Each external amount accumulates on its own entry, so an entry's accumulator
 * takes at most its own flux terms per step and its rounding stays inside the
 * closure tolerance's (L + 2) N_acc terms; the per-total sums are formed once at
 * segment end (sum_external_amounts()). Each observer adds its flux's committed
 * amount, the number that moved (zero for a flux whose two ends are one entry,
 * attempt_step()); under the implicit scheme implicit_attempt() has already
 * put that sum in the trial, where its trial-end law evaluation reads it, so it
 * is not added again. With tracers present, each entry's inflow amounts
 * accumulate the same way for the tracer bound's throughput.
 */
static void commit_step(struct Integration *in) {
  const struct RateComponent *c = in->c;
  struct Scratch *s = &in->s;
  double *previous = s->x;
  s->x = s->trial;
  s->trial = previous;
  for (int k = 0; k < in->num_fluxes; k++) {
    const int f = s->order[k];
    const struct RateFlux *flux = &s->flux[f];
    if (flux_is_external_source(flux)) {
      s->source_entry[flux->to] += s->amount[f];
    } else if (flux->kind == RATE_FLUX_SINK) {
      s->sink_entry[flux->from] += s->amount[f];
    }
  }
  for (int i = 0; i < c->num_entries; i++) {
    const struct RateEntry *entry = &c->entries[i];
    if (entry->kind == RATE_ENTRY_OBSERVER) {
      if (in->settings->scheme != RATE_SCHEME_IMPLICIT) {
        s->x[i] += s->amount[entry->flux];
      }
    } else if (in->num_tracers > 0) {
      double inflow = 0.0;
      for (int k = s->in_start[i]; k < s->in_start[i + 1]; k++) {
        inflow += s->amount[s->in_list[k]];
      }
      s->inflow_entry[i] += inflow;
    }
  }
  in->outcome->stats.accepted_steps++;
}

/** Sum the per-entry external amounts into each total, in entry order, once per segment. */
static void sum_external_amounts(struct Integration *in) {
  const struct RateComponent *c = in->c;
  struct Scratch *s = &in->s;
  memset(s->source, 0, (size_t)c->num_totals * sizeof(double));
  memset(s->sink, 0, (size_t)c->num_totals * sizeof(double));
  for (int i = 0; i < c->num_entries; i++) {
    if (c->entries[i].kind == RATE_ENTRY_OBSERVER) {
      continue;
    }
    s->source[c->entries[i].total] += s->source_entry[i];
    s->sink[c->entries[i].total] += s->sink_entry[i];
  }
}

/** Count a rejected attempt by its kind. */
static void count_rejection(struct Integration *in, enum AttemptResult result) {
  struct RateStepStats *stats = &in->outcome->stats;
  stats->rejected_steps++;
  if (result == ATTEMPT_REJECTED_POSITIVITY) {
    stats->positivity_rejections++;
  } else {
    stats->error_rejections++;
  }
}

/* ---------------------------------------------------------------------------
 * Step-size selection and the two drivers
 * ------------------------------------------------------------------------- */

/**
 * @brief   The segment-start weights w0 and the first-step ratios d0 and d1
 *
 * w0_i = absolute_tolerance * S + relative_tolerance * |x0_i|, S the entry's row
 * sum of its total floored at 1e-12 of the component's (rate_integrator.h,
 * Error control); d0 = max |x0_i| / w0_i and d1 = max |F0_i| / w0_i, F0 the net
 * rate from the stage-1 rates s.phi[0] (kept in s.net0). An entry is left out,
 * with weight zero here, when its weight is zero, when it is an observer, or
 * (owner decision S7-D2) when its own row's sum of its total (its parent's,
 * for a tracer) is zero: the floored scale would give it a weight near 1e-12
 * of the component's and a first step far below the minimum, so the error
 * control governs it from the first step through the trial-end scale instead.
 *
 * @return  1 when some entry is left in, else 0 (d0 and d1 are then 0)
 */
static int start_weights(struct Integration *in, double *start_weight, double *d0, double *d1) {
  const struct RateComponent *c = in->c;
  struct Scratch *s = &in->s;
  const double atol = in->settings->absolute_tolerance;
  const double rtol = in->settings->relative_tolerance;

  sum_totals(in, s->x, s->group_start, s->total_start);
  net_change(in, s->phi[0], s->net0);
  int weighted = 0;
  *d0 = 0.0;
  *d1 = 0.0;
  for (int i = 0; i < c->num_entries; i++) {
    if (c->entries[i].kind == RATE_ENTRY_OBSERVER) {
      start_weight[i] = 0.0; /* no weight: left out of d0, d1 and d2 */
      continue;
    }
    const int t = scale_total(c, i);
    const size_t g = (size_t)c->entries[i].row * (size_t)c->num_totals + (size_t)t;
    if (!(s->group_start[g] > 0.0)) {
      start_weight[i] = 0.0; /* an empty row of its total: left out (S7-D2) */
      continue;
    }
    const double scale = fmax(s->group_start[g], SCALE_FLOOR * s->total_start[t]);
    start_weight[i] = atol * scale + rtol * fabs(s->x[i]);
    if (start_weight[i] > 0.0) {
      weighted = 1;
      *d0 = fmax(*d0, fabs(s->x[i]) / start_weight[i]);
      *d1 = fmax(*d1, fabs(s->net0[i]) / start_weight[i]);
    }
  }
  return weighted;
}

/**
 * @brief   The first step (Hairer and Wanner, adapted to the norm and the cap)
 *
 * Uses the stage-1 rates already evaluated at the segment start. Entries are
 * left out as start_weights() states (zero weight, or an empty row of their
 * total by S7-D2); when all are, the step is min(1, cap) with no probe. Otherwise one forward-Euler
 * probe, non-negative under the cap sigma / K_max, costs one extra law evaluation.
 */
static enum RateCause first_step(struct Integration *in, double *step) {
  const struct RateComponent *c = in->c;
  struct Scratch *s = &in->s;
  const double cap = positivity_cap(in->k_start);
  double *start_weight = s->trial; /* free until the first attempt */
  double d0;
  double d1;

  if (!start_weights(in, start_weight, &d0, &d1)) {
    *step = fmin(1.0, cap);
    return RATE_CAUSE_NONE;
  }

  double h0 = (d0 < FIRST_STEP_SMALL || d1 < FIRST_STEP_SMALL) ? 1.0 : FIRST_STEP_TARGET * d0 / d1;
  h0 = fmin(h0, 1.0);
  if (in->k_start > 0.0) {
    h0 = fmin(h0, POSITIVITY_MARGIN / in->k_start);
  }
  for (int f = 0; f < in->num_fluxes; f++) {
    s->amount[f] = h0 * s->phi[0][f];
  }
  /* Outflows take at most sigma of each entry, so the probe cannot go negative
   * inside the input domain; this guard keeps any law off a state that did. */
  if (!(h0 > 0.0) || form_state(in, s->x, s->amount, s->stage) >= 0) {
    *step = fmin(h0, cap);
    return RATE_CAUSE_NONE;
  }
  const enum RateCause cause = evaluate_law(in, h0, s->stage, 0.0, h0);
  if (cause != RATE_CAUSE_NONE) {
    return cause;
  }
  flux_rates(in, s->stage, s->phi[1]);
  (void)k_max(in, s->stage, NULL);
  net_change(in, s->phi[1], s->net1);
  double d2 = 0.0;
  for (int i = 0; i < c->num_entries; i++) {
    if (start_weight[i] > 0.0) {
      d2 = fmax(d2, fabs(s->net1[i] - s->net0[i]) / start_weight[i]);
    }
  }
  d2 /= h0;
  const double d_max = fmax(d1, d2);
  const double h1 = (d_max <= FIRST_STEP_TINY) ? 1.0 : cbrt(FIRST_STEP_TARGET / d_max);
  *step = fmin(fmin(FIRST_STEP_GROWTH * h0, h1), fmin(1.0, cap));
  return RATE_CAUSE_NONE;
}

/**
 * @brief   Land on tau = 1, then check the minimum step and the attempt budget
 *
 * The step is stretched by up to 10% to end at tau = 1 when the remainder is
 * within the cap, and a remainder shorter than two steps is otherwise split into
 * two equal steps. A failure names the latest limiter.
 */
static enum RateCause prepare_attempt(struct Integration *in, double tau, double cap, double *step,
                                      int64_t *attempts, int *last) {
  const double remaining = 1.0 - tau;
  *last = 0;
  if (remaining <= LANDING_STRETCH * *step && remaining <= cap) {
    *step = remaining;
    *last = 1;
  } else if (remaining < 2.0 * *step) {
    *step = 0.5 * remaining;
  }
  if (!(*step >= MIN_STEP)) {
    return record_failure(in, RATE_CAUSE_STEP_UNDERFLOW, in->limit_entry, in->limit_flux, tau,
                          *step);
  }
  if (*attempts >= in->settings->max_steps_per_segment) {
    return record_failure(in, RATE_CAUSE_STEP_BUDGET, in->limit_entry, in->limit_flux, tau, *step);
  }
  (*attempts)++;
  return RATE_CAUSE_NONE;
}

/**
 * @brief   Count and report a rejected adaptive attempt
 * @return  RATE_CAUSE_REJECTION_BUDGET, naming the latest limiter, once
 *          max_rejections_per_step consecutive attempts have been rejected
 */
static enum RateCause reject_attempt(struct Integration *in, enum AttemptResult result, double tau,
                                     double step, double norm, double min_weight,
                                     int *consecutive_rejections) {
  count_rejection(in, result);
  report_attempt(in, tau, step, 0,
                 (result == ATTEMPT_REJECTED_POSITIVITY) ? RATE_REJECTION_POSITIVITY
                                                         : RATE_REJECTION_ERROR,
                 norm, min_weight);
  (*consecutive_rejections)++;
  if (*consecutive_rejections >= in->settings->max_rejections_per_step) {
    return record_failure(in, RATE_CAUSE_REJECTION_BUDGET, in->limit_entry, in->limit_flux, tau,
                          step);
  }
  return RATE_CAUSE_NONE;
}

/** The explicit adaptive driver: first step, then accept/reject with the controller. */
static enum RateCause integrate_adaptive(struct Integration *in) {
  double tau = 0.0;
  double step = 0.0;
  enum RateCause cause = begin_step(in, tau, step);
  if (cause != RATE_CAUSE_NONE) {
    return cause;
  }
  cause = first_step(in, &step);
  if (cause != RATE_CAUSE_NONE) {
    return cause;
  }

  int64_t attempts = 0;
  int consecutive_rejections = 0;
  int after_rejection = 0;
  for (;;) {
    const double cap = positivity_cap(in->k_start);
    if (step >= cap) {
      step = cap;
      in->limit_entry = in->k_start_entry;
      in->limit_flux = in->k_start_flux;
    }
    int last;
    cause = prepare_attempt(in, tau, cap, &step, &attempts, &last);
    if (cause != RATE_CAUSE_NONE) {
      return cause;
    }

    double norm;
    double min_weight;
    double k_seen;
    const enum AttemptResult result = attempt_step(in, tau, step, 1, &norm, &min_weight, &k_seen);
    if (result == ATTEMPT_FAILED) {
      return in->outcome->failure.cause;
    }
    if (result == ATTEMPT_ACCEPTED) {
      commit_step(in);
      report_attempt(in, tau, step, 1, RATE_REJECTION_NONE, norm, min_weight);
      if (last) {
        return RATE_CAUSE_NONE;
      }
      tau += step;
      double factor = step_factor(norm);
      if (after_rejection && factor > 1.0) {
        factor = 1.0;
      }
      step *= factor;
      after_rejection = 0;
      consecutive_rejections = 0;
      cause = begin_step(in, tau, step);
      if (cause != RATE_CAUSE_NONE) {
        return cause;
      }
      continue;
    }

    after_rejection = 1;
    cause = reject_attempt(in, result, tau, step, norm, min_weight, &consecutive_rejections);
    if (cause != RATE_CAUSE_NONE) {
      return cause;
    }
    if (result == ATTEMPT_REJECTED_POSITIVITY) {
      step = fmin(0.5 * step, positivity_cap(k_seen));
    } else {
      step *= step_factor(norm);
    }
  }
}

/* ---------------------------------------------------------------------------
 * The linearly implicit scheme
 *
 * Patankar-Euler with the coefficients frozen at the step start, one block per
 * row and total. With g_f = dt * Delta * coefficient of flux f at x_n (tau
 * units), a block of R entries is stored as off[i][j], the sum of g_f over its
 * transfers from entry j to entry i (i != j, so off >= 0), and the column
 * excess e[j] = 1 + the sum of g_f over j's transfers that leave the block
 * (sinks, cross-row transfers, tracer moves out of the total). The diagonal,
 * e[j] + sum_i off[i][j], is never formed. Each block is factorised in the
 * Grassmann-Taksar-Heyman form, without pivoting and without subtraction:
 *
 *   for k = 0 .. R-1:
 *     piv[k] = e[k] + sum_{i > k} off[i][k]
 *     for i > k:   l[i][k] = off[i][k] / piv[k]                  0 <= l <= 1
 *     for j > k with off[k][j] != 0:
 *       for i > k, i != j:   off[i][j] += l[i][k] * off[k][j]    magnitudes only grow
 *       e[j] += off[k][j] * (e[k] / piv[k])                      the Schur complement's excess
 *   forward:    y[i] = b[i] + sum_{j < i} l[i][j] y[j]
 *   backward:   x[i] = (y[i] + sum_{j > i} off[i][j] x[j]) / piv[i]
 *
 * Every quantity is a sum or product of non-negative numbers, so a non-negative
 * right-hand side has a non-negative solution at any step. No array holds more
 * than one block's square, so the scratch, and the work for a fixed number of
 * entries per row, grow linearly with the number of rows.
 * ------------------------------------------------------------------------- */

/** Fatal unless each row's entries are listed together, which the block storage relies on. */
static void implicit_check_rows(struct Integration *in) {
  const struct RateComponent *c = in->c;
  int *seen = in->s.row_count;
  memset(seen, 0, (size_t)c->num_rows * sizeof(int));
  int previous = -1;
  for (int i = 0; i < c->num_entries; i++) {
    const int row = c->entries[i].row;
    if (row == previous) {
      continue;
    }
    if (seen[row]) {
      FATAL_ERROR("Rate entry %d returns to row %d after another row's entries; the implicit "
                  "scheme needs each row's entries listed together",
                  i, row);
    }
    seen[row] = 1;
    previous = row;
  }
}

/**
 * @brief   Order the totals so each is solved after every total it depends on
 *
 * A tracer's total depends on its parent's, and a slaved source's recipient
 * total on its master donor's; the lowest ready index goes first. A cycle, and
 * an internal transfer that joins two totals (movement between totals is a sink
 * and a slaved source), are fatal.
 */
static void implicit_order_totals(struct Integration *in) {
  const struct RateComponent *c = in->c;
  struct Scratch *s = &in->s;
  const int totals = c->num_totals;
  int *depends = s->total_depends;

  memset(depends, 0, (size_t)totals * (size_t)totals * sizeof(int));
  memset(s->total_placed, 0, (size_t)totals * sizeof(int));
  for (int z = 0; z < c->num_entries; z++) {
    if (c->entries[z].kind == RATE_ENTRY_TRACER) {
      depends[c->entries[z].total * totals + c->entries[c->entries[z].parent].total] = 1;
    }
  }
  for (int f = 0; f < c->num_fluxes; f++) {
    const struct RateFlux *flux = &c->fluxes[f];
    if (flux->kind == RATE_FLUX_INTERNAL &&
        c->entries[flux->from].total != c->entries[flux->to].total) {
      FATAL_ERROR("Rate internal transfer %d joins totals %d and %d; an internal transfer stays "
                  "in one total",
                  f, c->entries[flux->from].total, c->entries[flux->to].total);
    }
    if (flux->kind == RATE_FLUX_SLAVED) {
      const int donor = c->fluxes[flux->master].from;
      depends[c->entries[flux->to].total * totals + c->entries[donor].total] = 1;
    }
  }
  for (int k = 0; k < totals; k++) {
    int next = -1;
    for (int t = 0; t < totals && next < 0; t++) {
      int ready = !s->total_placed[t];
      for (int u = 0; u < totals && ready; u++) {
        ready = !(depends[t * totals + u] && !s->total_placed[u]);
      }
      if (ready) {
        next = t;
      }
    }
    if (next < 0) {
      FATAL_ERROR("Rate totals depend on one another in a cycle (%d of %d totals ordered); the "
                  "implicit scheme solves each total after every total it depends on",
                  k, totals);
    }
    s->total_order[k] = next;
    s->total_placed[next] = 1;
  }
}

/** True when internal flux @p f moves content between two rows, given in @p from_row, @p to_row. */
static int cross_row_transfer(const struct Integration *in, int f, int *from_row, int *to_row) {
  const struct RateFlux *flux = &in->s.flux[f];
  if (flux->kind != RATE_FLUX_INTERNAL || flux->from == flux->to) {
    return 0;
  }
  *from_row = in->c->entries[flux->from].row;
  *to_row = in->c->entries[flux->to].row;
  return *from_row != *to_row;
}

/**
 * @brief   Order the rows so every cross-row transfer runs from an earlier row to a later one
 *
 * Kahn's algorithm over the rows, with the cross-row transfers and tracer moves
 * as edges in canonical order and the ready rows taken first in, first out,
 * starting from the rows in index order: a FoF component's satellites in row
 * order, then its central. A cycle among rows is fatal. Any order that respects
 * the edges gives the same result, since each block's right-hand side is summed
 * in canonical order at its own solve.
 */
static void implicit_order_rows(struct Integration *in) {
  const struct RateComponent *c = in->c;
  struct Scratch *s = &in->s;
  const int rows = c->num_rows;
  int *in_degree = s->row_count;
  int from_row;
  int to_row;

  memset(in_degree, 0, (size_t)rows * sizeof(int));
  memset(s->row_edge_start, 0, (size_t)(rows + 1) * sizeof(int));
  for (int k = 0; k < in->num_fluxes; k++) {
    if (cross_row_transfer(in, s->order[k], &from_row, &to_row)) {
      s->row_edge_start[from_row + 1]++;
      in_degree[to_row]++;
    }
  }
  counts_to_offsets(s->row_edge_start, rows);
  for (int k = 0; k < in->num_fluxes; k++) {
    if (cross_row_transfer(in, s->order[k], &from_row, &to_row)) {
      s->row_edge[s->row_edge_start[from_row]++] = to_row;
    }
  }
  restore_offsets(s->row_edge_start, rows);

  int tail = 0;
  for (int r = 0; r < rows; r++) {
    if (in_degree[r] == 0) {
      s->row_order[tail++] = r;
    }
  }
  for (int head = 0; head < tail; head++) {
    const int r = s->row_order[head];
    for (int e = s->row_edge_start[r]; e < s->row_edge_start[r + 1]; e++) {
      if (--in_degree[s->row_edge[e]] == 0) {
        s->row_order[tail++] = s->row_edge[e];
      }
    }
  }
  if (tail < rows) {
    FATAL_ERROR("Rate cross-row transfers form a cycle among rows (%d of %d rows ordered); the "
                "implicit scheme solves each row after every row that sends to it",
                tail, rows);
  }
}

/**
 * @brief   Form the blocks, their dense storage and their solve order
 *
 * Blocks are numbered in order of their first entry; each lists its entries in
 * entry order. The solve order takes the totals in dependency order and, within
 * a total, the rows in row-graph order.
 *
 * @return  The most reservoir and tracer entries on one row (one galaxy's block)
 */
static int implicit_build_blocks(struct Integration *in) {
  const struct RateComponent *c = in->c;
  struct Scratch *s = &in->s;
  const int n = c->num_entries;
  const int totals = c->num_totals;
  const size_t groups = (size_t)c->num_rows * (size_t)totals;

  for (size_t g = 0; g < groups; g++) {
    s->block_map[g] = -1;
  }
  memset(s->block_start, 0, (size_t)(n + 1) * sizeof(int));
  int blocks = 0;
  int largest_row = 0;
  int row_entries = 0;
  for (int i = 0; i < n; i++) {
    const struct RateEntry *entry = &c->entries[i];
    if (i == 0 || entry->row != c->entries[i - 1].row) {
      row_entries = 0;
    }
    if (entry->kind == RATE_ENTRY_OBSERVER) {
      s->block_of[i] = -1;
      continue;
    }
    if (++row_entries > largest_row) {
      largest_row = row_entries;
    }
    const size_t g = (size_t)entry->row * (size_t)totals + (size_t)entry->total;
    if (s->block_map[g] < 0) {
      s->block_map[g] = blocks++;
    }
    s->block_of[i] = s->block_map[g];
    s->block_start[s->block_of[i] + 1]++;
  }
  counts_to_offsets(s->block_start, blocks);
  for (int i = 0; i < n; i++) {
    if (s->block_of[i] >= 0) {
      s->block_member[s->block_start[s->block_of[i]]++] = i;
    }
  }
  restore_offsets(s->block_start, blocks);

  size_t offset = 0;
  for (int b = 0; b < blocks; b++) {
    const int size = s->block_start[b + 1] - s->block_start[b];
    for (int p = s->block_start[b]; p < s->block_start[b + 1]; p++) {
      s->local_of[s->block_member[p]] = p - s->block_start[b];
    }
    s->block_offset[b] = offset;
    offset += (size_t)size * (size_t)size;
  }
  if (offset > in->dense_capacity) {
    FATAL_ERROR("Rate implicit blocks need %zu doubles; the workspace holds %zu", offset,
                in->dense_capacity);
  }

  int position = 0;
  for (int a = 0; a < totals; a++) {
    for (int q = 0; q < c->num_rows; q++) {
      const int b =
          s->block_map[(size_t)s->row_order[q] * (size_t)totals + (size_t)s->total_order[a]];
      if (b >= 0) {
        s->block_order[position++] = b;
      }
    }
  }
  in->num_blocks = blocks;
  return largest_row;
}

/**
 * @brief   Check and order the component for the block solve
 * @return  L of the closure and the tracer bound: the larger of @p max_terms and the most
 *          reservoir and tracer entries on one row
 */
static int implicit_prepare(struct Integration *in, int max_terms) {
  implicit_check_rows(in);
  implicit_order_totals(in);
  implicit_order_rows(in);
  in->dense_capacity = implicit_dense_capacity(in->c);
  const int largest_row = implicit_build_blocks(in);
  return (largest_row > max_terms) ? largest_row : max_terms;
}

/**
 * @brief   Assemble block @p b from s.step_rate and factorise it in place
 *
 * Leaves l below the diagonal of the block's dense array, U's off-diagonal
 * magnitudes above it and each entry's pivot in s.pivot (see the section
 * comment).
 *
 * @return  -1, or the first entry whose pivot is not finite
 */
static int factor_block(struct Integration *in, int b) {
  struct Scratch *s = &in->s;
  const int *member = &s->block_member[s->block_start[b]];
  const int size = s->block_start[b + 1] - s->block_start[b];
  const size_t width = (size_t)size;
  double *a = &s->dense[s->block_offset[b]]; /* a[i * width + j] */

  memset(a, 0, width * width * sizeof(double));
  for (int j = 0; j < size; j++) {
    const int entry = member[j];
    double leave = 0.0;
    for (int k = s->out_start[entry]; k < s->out_start[entry + 1]; k++) {
      const int f = s->out_list[k];
      const struct RateFlux *flux = &s->flux[f];
      if (flux->kind == RATE_FLUX_INTERNAL && s->block_of[flux->to] == b) {
        a[(size_t)s->local_of[flux->to] * width + (size_t)j] += s->step_rate[f];
      } else {
        leave += s->step_rate[f];
      }
    }
    s->excess[entry] = 1.0 + leave;
  }

  for (int k = 0; k < size; k++) {
    const int entry = member[k];
    double *row_k = &a[(size_t)k * width];
    double pivot = s->excess[entry];
    for (int i = k + 1; i < size; i++) {
      pivot += a[(size_t)i * width + (size_t)k];
    }
    if (!isfinite(pivot)) {
      return entry;
    }
    s->pivot[entry] = pivot;
    const double kept = s->excess[entry] / pivot;
    for (int i = k + 1; i < size; i++) {
      a[(size_t)i * width + (size_t)k] /= pivot;
    }
    for (int j = k + 1; j < size; j++) {
      const double u = row_k[j];
      if (u == 0.0) {
        continue;
      }
      for (int i = k + 1; i < size; i++) {
        if (i != j) {
          a[(size_t)i * width + (size_t)j] += a[(size_t)i * width + (size_t)k] * u;
        }
      }
      s->excess[member[j]] += u * kept;
    }
  }
  return -1;
}

/** The detail of a bad_coefficient failure raised by the implicit scheme's products. */
#define IMPLICIT_PRODUCT_DETAIL "the step times the flux's rate is not finite (implicit scheme)"
#define IMPLICIT_PIVOT_DETAIL                                                                      \
  "a block pivot is not finite (implicit scheme): the step times the rates out of the entry "      \
  "overflow"

/**
 * @brief   Form step x rate of every flux at the step start, then factorise every block
 *
 * Fails with RATE_CAUSE_BAD_COEFFICIENT when a declared transfer, sink or source
 * has a product that is not finite (the first in canonical order, named by its
 * donor or a source's recipient), or when a block pivot is not finite (named by
 * its entry and that entry's outflow with the largest product).
 */
static enum RateCause implicit_factor(struct Integration *in, double tau, double step) {
  const struct RateComponent *c = in->c;
  struct Scratch *s = &in->s;
  for (int f = 0; f < in->num_fluxes; f++) {
    s->step_rate[f] = (s->flux[f].kind == RATE_FLUX_SLAVED)
                          ? 0.0
                          : step * (c->row_delta[s->clock_row[f]] * s->coef[f]);
  }
  for (int k = 0; k < in->num_fluxes; k++) {
    const int f = s->order[k];
    const struct RateFlux *flux = &s->flux[f];
    if (f < in->num_declared && (flux_has_donor(flux) || flux->kind == RATE_FLUX_SOURCE) &&
        !isfinite(s->step_rate[f])) {
      record_failure(in, RATE_CAUSE_BAD_COEFFICIENT, flux_named_entry(flux), f, tau, step);
      in->outcome->failure.detail = IMPLICIT_PRODUCT_DETAIL;
      return RATE_CAUSE_BAD_COEFFICIENT;
    }
  }
  for (int b = 0; b < in->num_blocks; b++) {
    const int entry = factor_block(in, b);
    if (entry >= 0) {
      record_failure(in, RATE_CAUSE_BAD_COEFFICIENT, entry,
                     largest_outflow(in, entry, s->step_rate), tau, step);
      in->outcome->failure.detail = IMPLICIT_PIVOT_DETAIL;
      return RATE_CAUSE_BAD_COEFFICIENT;
    }
  }
  return RATE_CAUSE_NONE;
}

/**
 * @brief   Solve M out = base + the known inflows, block by block in solve order
 *
 * A block's right-hand side is @p base plus, in canonical order, the amounts
 * into each entry that are already known: the sources (at their step amounts
 * with @p with_sources, the step's solve; zero without, the error solve), the
 * slaved sources and the transfers out of blocks solved before it. @p amount
 * receives each flux's amount: step x rate times the donor's solved value for a
 * transfer or sink, the coefficient times the master's amount for a slaved
 * source, zero for a flux that moves nothing. Observers in @p out are not
 * written.
 */
static void implicit_solve(struct Integration *in, int with_sources, const double *base,
                           double *out, double *amount) {
  struct Scratch *s = &in->s;
  for (int f = 0; f < in->num_fluxes; f++) {
    amount[f] = (with_sources && s->flux[f].kind == RATE_FLUX_SOURCE) ? s->step_rate[f] : 0.0;
  }
  for (int q = 0; q < in->num_blocks; q++) {
    const int b = s->block_order[q];
    const int *member = &s->block_member[s->block_start[b]];
    const int size = s->block_start[b + 1] - s->block_start[b];
    const size_t width = (size_t)size;
    const double *a = &s->dense[s->block_offset[b]];

    for (int i = 0; i < size; i++) {
      const int entry = member[i];
      double known = 0.0;
      for (int k = s->in_start[entry]; k < s->in_start[entry + 1]; k++) {
        const int f = s->in_list[k];
        const struct RateFlux *flux = &s->flux[f];
        if (flux->kind == RATE_FLUX_SLAVED) {
          amount[f] = s->coef[f] * amount[flux->master];
          known += amount[f];
        } else if (flux->kind == RATE_FLUX_SOURCE || s->block_of[flux->from] != b) {
          known += amount[f];
        }
      }
      out[entry] = base[entry] + known;
    }
    for (int i = 1; i < size; i++) {
      double y = out[member[i]];
      for (int j = 0; j < i; j++) {
        y += a[(size_t)i * width + (size_t)j] * out[member[j]];
      }
      out[member[i]] = y;
    }
    for (int i = size - 1; i >= 0; i--) {
      double y = out[member[i]];
      for (int j = i + 1; j < size; j++) {
        y += a[(size_t)i * width + (size_t)j] * out[member[j]];
      }
      out[member[i]] = y / s->pivot[member[i]];
    }
    for (int j = 0; j < size; j++) {
      const int entry = member[j];
      for (int k = s->out_start[entry]; k < s->out_start[entry + 1]; k++) {
        amount[s->out_list[k]] = s->step_rate[s->out_list[k]] * out[entry];
      }
    }
  }
}

/** Exchange the step-start and trial coefficient arrays. */
static void swap_coefficients(struct Scratch *s) {
  double *start = s->coef;
  s->coef = s->coef_trial;
  s->coef_trial = start;
}

/**
 * @brief   Evaluate the laws at the trial end s.trial, keeping the step start's in s.coef
 *
 * The trial's coefficients land in s.coef_trial, its flux rates in s.phi[1]; its
 * K_max feeds the step statistics.
 */
static enum RateCause evaluate_trial(struct Integration *in, double tau, double step) {
  struct Scratch *s = &in->s;
  swap_coefficients(s);
  const enum RateCause cause = evaluate_law(in, tau + step, s->trial, tau, step);
  if (cause == RATE_CAUSE_NONE) {
    flux_rates(in, s->trial, s->phi[1]);
    (void)k_max(in, s->trial, NULL);
  }
  swap_coefficients(s);
  return cause;
}

/**
 * @brief   Attempt one implicit step of length @p step from the accepted state at @p tau
 *
 * On acceptance s.trial holds the end state, observers included at their
 * committed values, and s.amount the committed amounts, so the trial end the
 * laws are evaluated at is exactly the state the next step starts from. With
 * @p adaptive the laws are evaluated at the trial end and the filtered
 * estimate M^-1 (dt/2) (F(x_{n+1}) - F(x_n)) is weighed: @p norm and
 * @p min_weight are written then, and left negative otherwise. A solved entry
 * that is not finite and non-negative (an overflow) rejects the attempt
 * before any law sees it.
 */
static enum AttemptResult implicit_attempt(struct Integration *in, double tau, double step,
                                           int adaptive, double *norm, double *min_weight) {
  const struct RateComponent *c = in->c;
  struct Scratch *s = &in->s;

  *norm = -1.0;
  *min_weight = -1.0;
  if (implicit_factor(in, tau, step) != RATE_CAUSE_NONE) {
    return ATTEMPT_FAILED;
  }
  implicit_solve(in, 1, s->x, s->trial, s->amount);
  int bad = -1;
  for (int i = 0; i < c->num_entries; i++) {
    if (c->entries[i].kind == RATE_ENTRY_OBSERVER) {
      /* The committed value, as the trial-end law evaluation must see it; commit_step() keeps it.
       */
      s->trial[i] = s->x[i] + s->amount[c->entries[i].flux];
    } else if (bad < 0 && (!(s->trial[i] >= 0.0) || isinf(s->trial[i]))) {
      bad = i;
    }
  }
  if (bad >= 0) {
    limit_by_negative_entry(in, bad);
    return ATTEMPT_REJECTED_POSITIVITY;
  }
  if (!adaptive) {
    return ATTEMPT_ACCEPTED;
  }

  if (evaluate_trial(in, tau, step) != RATE_CAUSE_NONE) {
    return ATTEMPT_FAILED;
  }
  const double half_step = 0.5 * step;
  for (int f = 0; f < in->num_fluxes; f++) {
    s->error_amount[f] = half_step * (s->phi[1][f] - s->phi[0][f]);
  }
  net_change(in, s->error_amount, s->stage);
  implicit_solve(in, 0, s->stage, s->filtered, s->error_flux);
  *norm = weighted_error_norm(in, s->filtered, min_weight);
  return (*norm <= 1.0) ? ATTEMPT_ACCEPTED : ATTEMPT_REJECTED_ERROR;
}

/**
 * @brief   The implicit first step: h0 of the explicit rule, with no probe and no cap
 *
 * 1 when start_weights() leaves every entry out, or when d0 or d1 is below
 * 1e-5; otherwise min(1, 0.01 d0 / d1).
 */
static double implicit_first_step(struct Integration *in) {
  double d0;
  double d1;
  if (!start_weights(in, in->s.trial, &d0, &d1) || d0 < FIRST_STEP_SMALL || d1 < FIRST_STEP_SMALL) {
    return 1.0;
  }
  return fmin(1.0, FIRST_STEP_TARGET * d0 / d1);
}

/** The implicit controller's step factor 0.9 err^(-1/2), clamped to [0.2, 5]. */
static double implicit_step_factor(double norm) {
  if (norm == 0.0) {
    return CONTROLLER_MAX_FACTOR;
  }
  const double factor = CONTROLLER_SAFETY / sqrt(norm);
  return fmin(CONTROLLER_MAX_FACTOR, fmax(CONTROLLER_MIN_FACTOR, factor));
}

/**
 * @brief   The implicit adaptive driver
 *
 * The law evaluation at an accepted trial end starts the next step, so each
 * attempt costs one evaluation. A positivity rejection halves the step; an
 * error rejection scales it by the controller.
 */
static enum RateCause integrate_implicit(struct Integration *in) {
  double tau = 0.0;
  enum RateCause cause = begin_step(in, tau, 0.0);
  if (cause != RATE_CAUSE_NONE) {
    return cause;
  }
  double step = implicit_first_step(in);

  int64_t attempts = 0;
  int consecutive_rejections = 0;
  int after_rejection = 0;
  for (;;) {
    int last;
    cause = prepare_attempt(in, tau, INFINITY, &step, &attempts, &last);
    if (cause != RATE_CAUSE_NONE) {
      return cause;
    }

    double norm;
    double min_weight;
    const enum AttemptResult result = implicit_attempt(in, tau, step, 1, &norm, &min_weight);
    if (result == ATTEMPT_FAILED) {
      return in->outcome->failure.cause;
    }
    if (result == ATTEMPT_ACCEPTED) {
      commit_step(in);
      report_attempt(in, tau, step, 1, RATE_REJECTION_NONE, norm, min_weight);
      if (last) {
        return RATE_CAUSE_NONE;
      }
      tau += step;
      double factor = implicit_step_factor(norm);
      if (after_rejection && factor > 1.0) {
        factor = 1.0;
      }
      step *= factor;
      after_rejection = 0;
      consecutive_rejections = 0;
      swap_coefficients(&in->s); /* the trial end's coefficients start the next step */
      adopt_start(in);
      continue;
    }

    after_rejection = 1;
    cause = reject_attempt(in, result, tau, step, norm, min_weight, &consecutive_rejections);
    if (cause != RATE_CAUSE_NONE) {
      return cause;
    }
    step *= (result == ATTEMPT_REJECTED_POSITIVITY) ? 0.5 : implicit_step_factor(norm);
  }
}

/* ---------------------------------------------------------------------------
 * Fixed steps, under either scheme
 * ------------------------------------------------------------------------- */

/** The fixed-step driver: n equal steps, controller off, positivity checks on. */
static enum RateCause integrate_fixed(struct Integration *in) {
  const int implicit = (in->settings->scheme == RATE_SCHEME_IMPLICIT);
  const int64_t steps = in->settings->fixed_steps;
  const double step = 1.0 / (double)steps;
  enum RateCause cause = begin_step(in, 0.0, step);
  if (cause != RATE_CAUSE_NONE) {
    return cause;
  }
  for (int64_t k = 0; k < steps; k++) {
    const double tau = (double)k / (double)steps;
    if (k >= in->settings->max_steps_per_segment) {
      /* Nothing else limits a fixed step: name the failing step's start-of-step limiter. */
      return record_failure(in, RATE_CAUSE_STEP_BUDGET, in->start_entry, in->start_flux, tau, step);
    }
    double norm;
    double min_weight;
    double k_seen;
    const enum AttemptResult result =
        implicit ? implicit_attempt(in, tau, step, 0, &norm, &min_weight)
                 : attempt_step(in, tau, step, 0, &norm, &min_weight, &k_seen);
    if (result == ATTEMPT_FAILED) {
      return in->outcome->failure.cause;
    }
    if (result != ATTEMPT_ACCEPTED) {
      count_rejection(in, result);
      report_attempt(in, tau, step, 0, RATE_REJECTION_POSITIVITY, norm, min_weight);
      return record_failure(in, RATE_CAUSE_REJECTION_BUDGET, in->limit_entry, in->limit_flux, tau,
                            step);
    }
    commit_step(in);
    report_attempt(in, tau, step, 1, RATE_REJECTION_NONE, norm, min_weight);
    if (k + 1 < steps) {
      cause = begin_step(in, (double)(k + 1) / (double)steps, step);
      if (cause != RATE_CAUSE_NONE) {
        return cause;
      }
    }
  }
  return RATE_CAUSE_NONE;
}

/* ---------------------------------------------------------------------------
 * Public interface
 * ------------------------------------------------------------------------- */

void rate_integrator_default_settings(struct RateIntegratorSettings *settings) {
  memset(settings, 0, sizeof(*settings));
  settings->scheme = RATE_SCHEME_EXPLICIT;
  settings->relative_tolerance = RATE_DEFAULT_RELATIVE_TOLERANCE;
  settings->absolute_tolerance = RATE_DEFAULT_ABSOLUTE_TOLERANCE;
  settings->max_steps_per_segment = RATE_DEFAULT_MAX_STEPS_PER_SEGMENT;
  settings->max_rejections_per_step = RATE_DEFAULT_MAX_REJECTIONS_PER_STEP;
  settings->fixed_steps = 0;
  settings->on_attempt = NULL;
  settings->attempt_data = NULL;
}

const char *rate_cause_name(enum RateCause cause) {
  switch (cause) {
  case RATE_CAUSE_NONE:
    return "none";
  case RATE_CAUSE_BAD_COEFFICIENT:
    return "bad_coefficient";
  case RATE_CAUSE_STEP_BUDGET:
    return "step_budget";
  case RATE_CAUSE_REJECTION_BUDGET:
    return "rejection_budget";
  case RATE_CAUSE_STEP_UNDERFLOW:
    return "step_underflow";
  case RATE_CAUSE_LEDGER:
    return "ledger";
  case RATE_CAUSE_TRACER_BOUND:
    return "tracer_bound";
  }
  return "unknown";
}

enum RateCause rate_check_closure(const struct RateClosureInput *input,
                                  struct RateFailure *failure) {
  failure->cause = RATE_CAUSE_NONE;
  failure->entry = -1;
  failure->flux = -1;
  failure->row = -1;
  failure->total = -1;
  failure->tau = 1.0;
  failure->step = 0.0;
  failure->detail = "";
  for (int t = 0; t < input->num_totals; t++) {
    double start_sum = 0.0;
    double end_sum = 0.0;
    double start_magnitude = 0.0;
    int members = 0;
    for (int i = 0; i < input->num_entries; i++) {
      if (input->entries[i].kind == RATE_ENTRY_OBSERVER || input->entries[i].total != t) {
        continue;
      }
      start_sum += input->start_state[i];
      end_sum += input->end_state[i];
      start_magnitude += fabs(input->start_state[i]);
      members++;
    }
    const double source = (input->source_amounts != NULL) ? input->source_amounts[t] : 0.0;
    const double sink = (input->sink_amounts != NULL) ? input->sink_amounts[t] : 0.0;
    const double drift = fabs((end_sum - start_sum) - (source - sink));
    const double terms = (double)(input->max_terms_per_entry + 2) * (double)input->accepted_steps +
                         (double)members + 2.0;
    const double tolerance = CLOSURE_FACTOR * terms * (start_magnitude + source + sink);
    if (!(drift <= tolerance)) {
      failure->cause = RATE_CAUSE_LEDGER;
      failure->total = t;
      return RATE_CAUSE_LEDGER;
    }
  }
  return RATE_CAUSE_NONE;
}

/** The detail of a tracer_bound failure, by whether the tracer has sources of its own. */
#define TRACER_BOUND_OWN_SOURCES                                                                   \
  "the tracer has sources of its own (a source or slaved source into it), which can break the "    \
  "bound in the exact solution"
#define TRACER_BOUND_NO_SOURCES                                                                    \
  "the tracer has no sources of its own: its only inflows are tracer moves"

/**
 * @brief   The tracer bound at segment end (rate_integrator.h, Segment-end checks)
 *
 * For each tracer z of parent p, in entry order: z - p <= 8u (L + 2) N_acc
 * Theta_p, with Theta_p p's start content plus its accumulated inflow amounts.
 * The first failing tracer is named with its first source or slaved source in
 * canonical order, if it has one.
 */
static enum RateCause check_tracer_bound(struct Integration *in, const double *start) {
  const struct RateComponent *c = in->c;
  const struct Scratch *s = &in->s;
  const double terms =
      (double)(in->outcome->max_terms_per_entry + 2) * (double)in->outcome->stats.accepted_steps;
  for (int z = 0; z < c->num_entries; z++) {
    if (c->entries[z].kind != RATE_ENTRY_TRACER) {
      continue;
    }
    const int p = c->entries[z].parent;
    const double throughput = start[p] + s->inflow_entry[p];
    const double bound = CLOSURE_FACTOR * terms * throughput;
    if (s->x[z] - s->x[p] <= bound) {
      continue;
    }
    int own_source = -1;
    for (int k = s->in_start[z]; k < s->in_start[z + 1] && own_source < 0; k++) {
      if (flux_is_external_source(&s->flux[s->in_list[k]])) {
        own_source = s->in_list[k];
      }
    }
    record_failure(in, RATE_CAUSE_TRACER_BOUND, z, own_source, 1.0, 0.0);
    in->outcome->failure.total = c->entries[z].total;
    in->outcome->failure.detail =
        (own_source >= 0) ? TRACER_BOUND_OWN_SOURCES : TRACER_BOUND_NO_SOURCES;
    return RATE_CAUSE_TRACER_BOUND;
  }
  return RATE_CAUSE_NONE;
}

/** True when some row has a positive Delta. */
static int any_row_advances(const struct RateComponent *c) {
  for (int r = 0; r < c->num_rows; r++) {
    if (c->row_delta[r] > 0.0) {
      return 1;
    }
  }
  return 0;
}

/** Copy the per-total external amounts to the caller's arrays, when given. */
static void write_amounts(const struct RateComponent *c, const double *source, const double *sink,
                          double *source_amounts, double *sink_amounts) {
  for (int t = 0; t < c->num_totals; t++) {
    if (source_amounts != NULL) {
      source_amounts[t] = (source != NULL) ? source[t] : 0.0;
    }
    if (sink_amounts != NULL) {
      sink_amounts[t] = (sink != NULL) ? sink[t] : 0.0;
    }
  }
}

enum RateCause rate_integrate_segment(const struct RateComponent *component,
                                      const struct RateLaw *law,
                                      const struct RateIntegratorSettings *settings,
                                      void *workspace, size_t workspace_bytes, double *state,
                                      double *source_amounts, double *sink_amounts,
                                      struct RateSegmentOutcome *outcome) {
  memset(outcome, 0, sizeof(*outcome));
  outcome->failure.cause = RATE_CAUSE_NONE;
  outcome->failure.entry = -1;
  outcome->failure.flux = -1;
  outcome->failure.row = -1;
  outcome->failure.total = -1;
  outcome->failure.detail = "";

  const size_t required = rate_integrator_workspace_bytes(component, settings->scheme);
  if (workspace_bytes < required || (required > 0 && workspace == NULL)) {
    FATAL_ERROR("Rate integrator workspace holds %zu bytes; the component needs %zu",
                workspace_bytes, required);
  }

  struct Integration in = {
      .c = component,
      .law = law,
      .settings = settings,
      .outcome = outcome,
      .k_start = 0.0,
      .k_start_entry = -1,
      .start_entry = -1,
      .start_flux = -1,
      .k_start_flux = -1,
      .limit_entry = -1,
      .limit_flux = -1,
  };
  layout_scratch(component, settings->scheme, workspace, &in.s);
  outcome->max_terms_per_entry = build_flux_lists(&in);
  if (settings->scheme == RATE_SCHEME_IMPLICIT) {
    outcome->max_terms_per_entry = implicit_prepare(&in, outcome->max_terms_per_entry);
  }

  if (component->num_entries == 0) {
    write_amounts(component, NULL, NULL, source_amounts, sink_amounts);
    return RATE_CAUSE_NONE;
  }
  memcpy(in.s.x, state, (size_t)component->num_entries * sizeof(double));
  memset(in.s.source_entry, 0, (size_t)component->num_entries * sizeof(double));
  memset(in.s.sink_entry, 0, (size_t)component->num_entries * sizeof(double));
  memset(in.s.inflow_entry, 0, (size_t)component->num_entries * sizeof(double));

  if (!any_row_advances(component)) {
    /* No step and no law: the state stands, and the tracer bound (N_acc = 0, so the bound is
     * zero) still holds every tracer to its parent. */
    if (check_tracer_bound(&in, state) != RATE_CAUSE_NONE) {
      return RATE_CAUSE_TRACER_BOUND;
    }
    write_amounts(component, NULL, NULL, source_amounts, sink_amounts);
    return RATE_CAUSE_NONE;
  }

  enum RateCause cause;
  if (settings->fixed_steps > 0) {
    cause = integrate_fixed(&in);
  } else if (settings->scheme == RATE_SCHEME_IMPLICIT) {
    cause = integrate_implicit(&in);
  } else {
    cause = integrate_adaptive(&in);
  }
  if (cause != RATE_CAUSE_NONE) {
    return cause;
  }
  sum_external_amounts(&in);

  const struct RateClosureInput closure = {
      .entries = component->entries,
      .num_entries = component->num_entries,
      .num_totals = component->num_totals,
      .start_state = state,
      .end_state = in.s.x,
      .source_amounts = in.s.source,
      .sink_amounts = in.s.sink,
      .max_terms_per_entry = outcome->max_terms_per_entry,
      .accepted_steps = outcome->stats.accepted_steps,
  };
  if (rate_check_closure(&closure, &outcome->failure) != RATE_CAUSE_NONE) {
    return RATE_CAUSE_LEDGER;
  }
  if (check_tracer_bound(&in, state) != RATE_CAUSE_NONE) {
    return RATE_CAUSE_TRACER_BOUND;
  }

  memcpy(state, in.s.x, (size_t)component->num_entries * sizeof(double));
  write_amounts(component, in.s.source, in.s.sink, source_amounts, sink_amounts);
  return RATE_CAUSE_NONE;
}
