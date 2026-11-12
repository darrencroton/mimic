#ifndef CORE_RATE_INTEGRATOR_H
#define CORE_RATE_INTEGRATOR_H

/**
 * @file    rate_integrator.h
 * @brief   Advances one rate component across one segment, and checks its closure
 *
 * A component is a set of entries (the reservoir contents of one or more rows)
 * coupled by fluxes. The integrator advances it across one segment in
 * normalised time tau, from 0 to 1, with the explicit SSPRK(4,3) scheme under
 * error control, a positivity guard and a per-segment conservation check.
 *
 * Time. Every row r has its own segment length Delta_r. A flux runs on its
 * clock row's time: an internal transfer or a sink on its donor's row, a source
 * on its recipient's row. Its rate in tau units is Delta of that row times its
 * coefficient, times the donor's content for an internal transfer or a sink.
 * One amount per flux and stage is formed once and applied to both of its
 * ends, as (x - sum of outflows) + sum of inflows, each sum in canonical flux
 * order, so a cross-record transfer conserves whatever the two rows' Delta. A
 * row with Delta = 0 donates nothing and its sources move nothing, but it
 * receives what a donor row with positive Delta sends it. A segment whose every
 * Delta is zero takes no step and evaluates no law.
 *
 * Canonical order. Entries are processed in the order the caller lists them,
 * which the caller makes canonical. Fluxes are processed in the order of
 * (clock row, global transfer index), whatever order the caller lists them in,
 * so a permuted flux list gives a bit-identical result.
 *
 * Positivity. With C = 2 and sigma = 0.9, K_i is Delta of entry i's row times
 * the sum of the coefficients out of entry i, and K_max is the largest K_i over
 * entries with non-zero content at the state in question (no such entry: no
 * cap). The step is capped at sigma * C / K_max before the first stage, and an
 * attempt is rejected when step * K_max at any stage exceeds C or when any
 * stage or final entry is below zero (or not a number). No law is evaluated at
 * such a state and nothing is clamped. After a positivity rejection the step is
 * the smaller of half the step and sigma * C over the largest K_max seen.
 *
 * Error control. Weight w_i = absolute_tolerance * S_i + relative_tolerance *
 * max(|x_i| at step start, |x_i| at the trial end), where S_i is the larger of
 * the entry's own row's sum over the entries of its total at step start and at
 * the trial end, floored at 1e-12 times the larger of that total's sums over
 * the whole component at the same two states. err is the largest |e_i| / w_i,
 * a zero error contributing 0 and a zero weight under a non-zero error, or a
 * NaN, contributing +infinity; a step is accepted when err <= 1. The next step
 * is the step times 0.9 * err^(-1/3), clamped to [0.2, 5], with no growth on a
 * step that follows a rejection. The first step uses the Hairer and Wanner rule
 * adapted to this norm and to the positivity cap, with one extra law
 * evaluation; when every entry's start weight is zero it is min(1, cap) with no
 * extra evaluation. The last step is stretched by up to 10% to land on tau = 1,
 * otherwise a remainder shorter than two steps is split into two equal steps;
 * tau is assigned 1.0 at the end, never accumulated. A step below 2^-46 fails.
 *
 * Fixed steps. With settings.fixed_steps = n > 0 the segment is n equal steps
 * with the controller off and no error estimate. The positivity checks stay on;
 * a fixed step that fails one fails the segment with RATE_CAUSE_REJECTION_BUDGET:
 * a fixed step allows no retry, so its first rejection exhausts the retry budget.
 * The record names the limiting entry, its outflow, tau and the step.
 *
 * Memory and state. The integrator allocates nothing: the caller supplies a
 * workspace of rate_integrator_workspace_bytes() bytes, aligned for double. It
 * keeps no state between calls, so the step sequence is a pure function of the
 * component, the start state, the law, the settings and the Delta values. It
 * works on a copy and writes the caller's state back only on success: a failed
 * segment leaves the state and the external amounts untouched.
 *
 * This unit knows nothing of galaxies, modules, the FoF workspace or the run file.
 */

#include <stddef.h>
#include <stdint.h>

/** Defaults of the integration settings (rate_integrator_default_settings()). */
#define RATE_DEFAULT_RELATIVE_TOLERANCE 1e-4
#define RATE_DEFAULT_ABSOLUTE_TOLERANCE 1e-7
#define RATE_DEFAULT_MAX_STEPS_PER_SEGMENT 100000
#define RATE_DEFAULT_MAX_REJECTIONS_PER_STEP 30

/** The end index of a flux that leaves or enters the component. */
#define RATE_END_EXTERNAL (-1)

/** What a flux's coefficient means and which ends it has. */
enum RateFluxKind {
  RATE_FLUX_INTERNAL, /* entry to entry; coefficient is a specific rate (inverse time) */
  RATE_FLUX_SOURCE,   /* external to entry; coefficient is an absolute rate (content per time) */
  RATE_FLUX_SINK      /* entry to external; coefficient is a specific rate (inverse time) */
};

/** One state entry: the content of one reservoir of one row. */
struct RateEntry {
  int row;   /* index into RateComponent.row_delta */
  int total; /* conserved total, 0 <= total < RateComponent.num_totals */
};

/**
 * @brief   One flux of a component
 *
 * `from` is an entry index for an internal transfer or a sink and
 * RATE_END_EXTERNAL for a source; `to` is an entry index for an internal
 * transfer or a source and RATE_END_EXTERNAL for a sink. An internal transfer's
 * two ends belong to one total. An internal transfer whose two ends are the same
 * entry moves nothing; its coefficient is still evaluated and checked.
 * `global_index` is the transfer's global index; (clock row, global_index) must
 * be unique within a component.
 */
struct RateFlux {
  enum RateFluxKind kind;
  int from;
  int to;
  int global_index;
};

/** The static description of one component; the integrator reads it and never writes it. */
struct RateComponent {
  const struct RateEntry *entries; /* [num_entries], in canonical entry order */
  int num_entries;
  const struct RateFlux *fluxes; /* [num_fluxes], in any order */
  int num_fluxes;
  const double *row_delta; /* [num_rows]: each row's segment length, >= 0 */
  int num_rows;
  int num_totals;
};

/**
 * @brief   The laws: fills one coefficient per flux for a given state
 *
 * `evaluate` receives the segment time tau of the evaluation, the state (one
 * value per entry, in the component's entry order, never negative) and the
 * coefficient array (one slot per flux, in the component's flux order, zeroed
 * before the call). It returns 0 on success; a non-zero return fails the
 * segment with RATE_CAUSE_BAD_COEFFICIENT and no entry or flux named, since
 * only the law knows what failed. It must not keep the state pointer.
 */
struct RateLaw {
  int (*evaluate)(void *data, double tau, const double *state, double *coefficients);
  void *data;
};

/** Why an attempt was rejected. */
enum RateRejection { RATE_REJECTION_NONE, RATE_REJECTION_POSITIVITY, RATE_REJECTION_ERROR };

/**
 * @brief   One attempted step, as reported to RateIntegratorSettings.on_attempt
 *
 * `state` is the committed state after an accepted step, or the unchanged
 * step-start state after a rejection; it is valid only during the call.
 * `error_norm` and `min_weight` are negative when the attempt ended before its
 * error estimate (a positivity rejection, or any fixed step).
 */
struct RateAttempt {
  double tau;  /* segment time at the step start */
  double step; /* step length in tau */
  int accepted;
  enum RateRejection rejection;
  double error_norm; /* err of the attempt */
  double min_weight; /* the smallest weight w_i of the attempt */
  const double *state;
};

/** Tolerances, budgets and the optional attempt observer of one segment. */
struct RateIntegratorSettings {
  double relative_tolerance;     /* in (0, 1) */
  double absolute_tolerance;     /* in (0, 1) */
  int64_t max_steps_per_segment; /* attempts, accepted and rejected */
  int max_rejections_per_step;   /* consecutive rejections of one step */
  int64_t fixed_steps;           /* 0: adaptive; n > 0: n equal steps, controller off */
  /* Called after every attempt when non-NULL (diagnostics and tests). */
  void (*on_attempt)(void *data, const struct RateAttempt *attempt);
  void *attempt_data;
};

/** Why a segment failed; RATE_CAUSE_NONE is success. */
enum RateCause {
  RATE_CAUSE_NONE = 0,
  RATE_CAUSE_BAD_COEFFICIENT,  /* a coefficient not finite or negative, or the law failed */
  RATE_CAUSE_STEP_BUDGET,      /* max_steps_per_segment attempts without reaching tau = 1 */
  RATE_CAUSE_REJECTION_BUDGET, /* max_rejections_per_step consecutive rejections of one step,
                                  or any rejection of a fixed step */
  RATE_CAUSE_STEP_UNDERFLOW,   /* the step fell below the minimum step, 2^-46 */
  RATE_CAUSE_LEDGER            /* a conserved total failed the segment closure */
};

/**
 * @brief   What failed, where and when
 *
 * `entry` and `flux` are the caller's indices (RateComponent order), or -1. For
 * a bad coefficient they are the offending flux and its donor (its recipient
 * for a source). For a budget or underflow failure they are the entry that
 * limited the last step and its outflow with the largest coefficient (or, when
 * the error estimate limited it, the flux with the largest error amount at that
 * entry). For a ledger failure `total` names the total and both are -1.
 */
struct RateFailure {
  enum RateCause cause;
  int entry;
  int flux;
  int row;     /* the row of `entry`, or the flux's clock row; -1 when neither is named */
  int total;   /* the failing total of a ledger failure, else -1 */
  double tau;  /* segment time at the start of the failing step */
  double step; /* the step being attempted, or 0 */
};

/** Step statistics of one segment, complete on failure too. */
struct RateStepStats {
  int64_t accepted_steps;
  int64_t rejected_steps; /* positivity_rejections + error_rejections */
  int64_t positivity_rejections;
  int64_t error_rejections;
  int64_t law_evaluations; /* including the first step's probe */
  double max_k_max;        /* the largest K_max at any state where the laws were evaluated */
};

/** Everything a segment reports besides its state and external amounts. */
struct RateSegmentOutcome {
  struct RateFailure failure; /* cause RATE_CAUSE_NONE on success */
  struct RateStepStats stats;
  int max_terms_per_entry; /* L: the largest number of inflow plus outflow terms on one entry */
};

/**
 * @brief   Inputs of the segment closure check of one component
 *
 * For every total F, |sum_F end - sum_F start - (Src_F - Snk_F)| must not
 * exceed tol_F = 8u [(L + 2) N_acc + n_F + 2] (sum_F |start| + Src_F + Snk_F),
 * u = 2^-53, every sum taken in entry order.
 */
struct RateClosureInput {
  const struct RateEntry *entries; /* [num_entries]: only `total` is read */
  int num_entries;
  int num_totals;
  const double *start_state;    /* [num_entries] */
  const double *end_state;      /* [num_entries] */
  const double *source_amounts; /* [num_totals], or NULL for none */
  const double *sink_amounts;   /* [num_totals], or NULL for none */
  int max_terms_per_entry;      /* L */
  int64_t accepted_steps;       /* N_acc */
};

/** @brief   Fill @p settings with the defaults: adaptive, no observer. */
void rate_integrator_default_settings(struct RateIntegratorSettings *settings);

/** @brief   The cause's name as the failure messages print it, e.g. "step_underflow". */
const char *rate_cause_name(enum RateCause cause);

/** @brief   Workspace bytes rate_integrate_segment() needs for @p component. */
size_t rate_integrator_workspace_bytes(const struct RateComponent *component);

/**
 * @brief   Advance one component across one segment, tau from 0 to 1
 *
 * @param   component       The entries, fluxes, row Delta values and totals
 * @param   law             The laws (coefficients for a state)
 * @param   settings        Tolerances, budgets, fixed-step count and observer
 * @param   workspace       Caller-owned scratch, aligned for double
 * @param   workspace_bytes At least rate_integrator_workspace_bytes(component)
 * @param   state           [num_entries]: start state in, end state out on success
 * @param   source_amounts  [num_totals] or NULL: per-total external inflow, out on success
 * @param   sink_amounts    [num_totals] or NULL: per-total external outflow, out on success
 * @param   outcome         Failure record, step statistics and L (always written)
 * @return  RATE_CAUSE_NONE on success, else the failure cause (also in outcome->failure)
 *
 * Inputs outside the domain (a negative entry, a negative or non-finite Delta,
 * tolerances outside (0, 1), a malformed flux) are unspecified behaviour. A
 * workspace smaller than required is a programming error and fatal.
 */
enum RateCause rate_integrate_segment(const struct RateComponent *component,
                                      const struct RateLaw *law,
                                      const struct RateIntegratorSettings *settings,
                                      void *workspace, size_t workspace_bytes, double *state,
                                      double *source_amounts, double *sink_amounts,
                                      struct RateSegmentOutcome *outcome);

/**
 * @brief   Check the segment closure of every total (struct RateClosureInput)
 *
 * @param   input    Start and end states, external amounts, L and N_acc
 * @param   failure  Written in full: cause RATE_CAUSE_LEDGER naming the first
 *                   failing total in index order, or RATE_CAUSE_NONE
 * @return  RATE_CAUSE_NONE when every total closes, else RATE_CAUSE_LEDGER
 */
enum RateCause rate_check_closure(const struct RateClosureInput *input,
                                  struct RateFailure *failure);

#endif /* CORE_RATE_INTEGRATOR_H */
