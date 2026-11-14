#ifndef CORE_RATE_INTEGRATOR_H
#define CORE_RATE_INTEGRATOR_H

/**
 * @file    rate_integrator.h
 * @brief   Advances one rate component across one segment, and checks its closure
 *
 * A component is a set of entries (the reservoir and tracer contents of one or
 * more rows) coupled by fluxes. The integrator advances it across one
 * segment in normalised time tau, from 0 to 1, under error control, with a
 * per-segment conservation check and a tracer-bound check, by one of two
 * schemes (settings.scheme): the explicit SSPRK(4,3) scheme with a positivity
 * guard, the default, or the linearly implicit Patankar-Euler scheme (see "The
 * implicit scheme" below). Unless a paragraph names a scheme, it holds for both.
 *
 * Time. Every row r has its own segment length Delta_r. A flux runs on its
 * clock row's time: an internal transfer or a sink on its donor's row, a source
 * on its recipient's row, a slaved source on its master's clock row. Its rate
 * in tau units is Delta of that row times its coefficient, times the donor's
 * content for an internal transfer or a sink; a slaved source's rate is its
 * coefficient times its master's rate, which already carries the master's Delta.
 * One amount per flux and stage is formed once and applied to both of its
 * ends, as (x - sum of outflows) + sum of inflows, each sum in canonical flux
 * order, so a cross-record transfer conserves whatever the two rows' Delta. A
 * row with Delta = 0 donates nothing and its sources move nothing, but it
 * receives what a donor row with positive Delta sends it. A segment whose every
 * Delta is zero takes no step, evaluates no law and passes its unchanged state
 * (the tracer bound, below, cannot fail without a step).
 *
 * Canonical order. Entries are processed in the order the caller lists them,
 * which the caller makes canonical; the implicit scheme needs each row's
 * entries listed together. Fluxes are processed in the
 * order of (clock row, global transfer index), whatever order the caller lists
 * them in, so a permuted flux list gives a bit-identical result; a tracer move
 * follows its master transfer, tracer moves of one transfer in entry order.
 * Fluxes that share a clock row and a global index (one transfer declared as a
 * source into the central, evaluated on several rows) keep the caller's
 * relative order, so the caller lists them canonically.
 *
 * Tracers. A tracer entry names its parent, a reservoir entry of the same row in
 * another total. Tracers move without declaration: for every internal transfer
 * or sink out of a parent, each of the parent's tracers moves at the same
 * coefficient times the tracer's own content (no division by the parent, so an
 * empty donor is well defined) into the recipient's tracer of the same total.
 * Where the transfer is a sink, or its recipient has no tracer of that total,
 * the tracer amount leaves its total as a sink of that total. A source or a
 * slaved source carries no tracer: a source into a parent dilutes its tracers.
 *
 * Slaved sources. A RATE_FLUX_SLAVED flux is an external source whose
 * coefficient is dimensionless; its rate at each stage is that coefficient times
 * its master's rate at the stage, evaluated after the transfers. The master is a
 * declared internal transfer or sink, never a source or another slaved source
 * (one level deep), and a master that moves nothing (both ends one entry) gives
 * its slaved sources nothing. Its amounts accumulate as a source of the
 * recipient's total.
 *
 * Flux amounts. A segment reports each declared flux's committed amount: the
 * sum over its accepted steps of the same number that moved (zero for a flux
 * whose two ends are one entry). An observer, a property that accumulates what
 * one flux moved, is not integrator state: it is no entry, belongs to no total,
 * and no law sees it change within a segment. Its caller adds the flux's amount
 * to it once the segment has succeeded.
 *
 * Positivity (explicit scheme). With C = 2 and sigma = 0.9, K_i is Delta of
 * entry i's row times the sum of the coefficients out of entry i, and K_max is
 * the largest K_i over entries with non-zero content at the state in question
 * (no such entry: no cap). The step is capped at sigma * C / K_max before the
 * first stage, and an attempt is rejected when step * K_max at any stage
 * exceeds C or when any stage or final entry is below zero (or not a number).
 * No law is evaluated at such a state and nothing is clamped. After a
 * positivity rejection the step is the smaller of half the step and sigma * C
 * over the largest K_max seen. K_max also feeds the step statistics under the
 * implicit scheme, which has no cap.
 *
 * Error control. Weight w_i = absolute_tolerance * S_i + relative_tolerance *
 * max(|x_i| at step start, |x_i| at the trial end), where S_i is the largest of
 * the entry's own row's sum over the entries of its total at the segment start,
 * at the step start and at the trial end, floored at 1e-12 times the largest of
 * that total's sums over the whole component at the same three states; a tracer
 * uses its parent's total. So a reservoir that drains is followed to an
 * absolute accuracy of absolute_tolerance times its row's segment-start total,
 * not relatively through every decade it falls, and a reservoir holding a small
 * fraction of its row's total is held to that absolute accuracy, not to the
 * relative tolerance. err is the largest |e_i| / w_i, a zero error contributing
 * 0 and a zero weight under a non-zero error, or a NaN, contributing +infinity;
 * a step is accepted when err <= 1. Under the explicit scheme e is the embedded
 * (b - bhat) estimate and the next step is the step times 0.9 * err^(-1/3),
 * clamped to [0.2, 5], with no growth on a step that follows a rejection.
 *
 * Starting step and landing. Every segment starts with a step of min(1,
 * cap(x0)) under the explicit scheme, cap(x0) being the positivity cap sigma *
 * C / K_max at the segment start, and of 1 under the implicit scheme; the error
 * control and the rejection rules size every step from there, and starting
 * costs no law evaluation of its own. So no in-domain start state fails with
 * step_underflow at tau = 0 unless the positivity cap itself is below the
 * minimum step; what the starting step cannot prevent is a law whose error does
 * not fall as the step shrinks, whose rejections at tau = 0 can still drive the
 * step below it. The last step is stretched by up to 10% to land on tau = 1,
 * otherwise a remainder shorter than two steps is split into two equal steps;
 * tau is assigned 1.0 at the end, never accumulated. A step below 2^-46 fails.
 *
 * The implicit scheme (settings.scheme = RATE_SCHEME_IMPLICIT). Patankar-Euler
 * with the coefficients frozen at the step start: (I - dt A(x_n)) x_{n+1} =
 * x_n + dt s(x_n), where A holds only the rates of the declared transfers and
 * sinks and of the tracer moves, never a Jacobian, and s the sources. A
 * transfer's amount is dt times its rate at x_n times its donor's solved
 * content; a slaved source's amount is its coefficient at x_n times its
 * master's amount. Each total's matrix is a column-diagonally-dominant
 * M-matrix, and a slaved amount enters its recipient's total as a known term,
 * so every solved entry is non-negative at any step and each total changes by
 * its external amounts to rounding; the scheme is first order.
 *   The solve is blocked, one block per row and total (a row is one galaxy), and
 * each block is factorised without pivoting and without subtraction, in the
 * Grassmann-Taksar-Heyman form: off-diagonal magnitudes and each column's excess
 * (one plus dt times its rates out of the block) are kept apart and each pivot
 * is formed as their sum, so work and scratch grow linearly with the number of
 * rows. Totals are solved in dependency order, each after every total it
 * depends on: a tracer's total after its parent's, a slaved source's recipient
 * total after its master donor's. Within a total the rows are solved in an
 * order where every cross-row transfer runs from an earlier row to a later one;
 * for a FoF component, the satellites in row order and then the central, which
 * gathers their cross-record terms. A block's right-hand side is its entries'
 * step-start content plus, in canonical order, the inflow amounts already
 * known: the sources, the slaved sources and the transfers out of blocks solved
 * before it.
 *   The error estimate is e = M^-1 M^-1 (dt/2) (F(x_{n+1}) - F(x_n)), F the net
 * rate of each entry, which reuses the factors for both solves and the law
 * evaluation at the trial end (the next step's start when the step is
 * accepted), so no law sees a state the solve did not produce. It is filtered
 * twice because, for a stiff component, the once-filtered estimate saturates at
 * half the jump in F while the true local error falls as 1/(dt k), k the stiff
 * rate; the second solve restores that scaling and leaves the non-stiff limit,
 * where M is near the identity, unchanged. It is weighed in the norm above; the
 * next step is the step times 0.9 * err^(-1/2), clamped to [0.2, 5], with no
 * growth after a rejection, and the step has no positivity cap. The starting
 * step, landing and the minimum step are as above.
 *   A declared transfer, sink or source whose dt times rate is not finite, or a
 * block pivot that is not finite, fails the segment with
 * RATE_CAUSE_BAD_COEFFICIENT. A solved entry that is not finite and
 * non-negative (an overflow) is a positivity rejection, after which the step
 * halves. The scheme needs each row's entries listed together (as the
 * canonical order lists them), cross-row transfers that form no cycle among
 * rows, internal transfers within one total, and totals whose dependencies form
 * no cycle; a component that breaks one is a programming error and fatal. It
 * integrates an ill-posed law, such as a sink whose coefficient diverges as its
 * donor empties, without complaint, which is why it is never the default.
 *
 * Segment-end checks. The external amounts accumulate per entry over accepted
 * steps and are summed per total once at segment end, in entry order, so the
 * rounding of the per-total amounts stays inside the closure tolerance however
 * many external fluxes a total has; the segment then fails with
 * RATE_CAUSE_LEDGER when a total does not close (rate_check_closure()). It then
 * fails with RATE_CAUSE_TRACER_BOUND when a tracer z of parent p ends with
 * z - p > e0 + 8u (L + 2) N_acc (Theta_p + e0), u = 2^-53, where e0 = max(0,
 * z - p) at the segment start and Theta_p, the parent's gross throughput, is
 * p's segment-start content plus the inflow amounts into p over the accepted
 * steps (accumulated per entry, like the external amounts). The bound limits
 * the growth of a tracer's excess over its parent, not its level: a tracer a
 * rounding error above its parent at the segment start (a residue a discrete
 * phase left) passes a segment that does not add to that excess, and a segment
 * that takes no step (N_acc = 0) passes its unchanged state. The scheme
 * preserves the bound when every inflow to a tracer is matched by a parent
 * inflow at least as large; a source or slaved source into the tracer can
 * break that premise in the exact solution, so a tracer with sources of its
 * own that overtakes its parent fails, and the failure's detail says whether
 * the tracer has sources of its own. L is the largest number of inflow plus
 * outflow terms on one entry; under the implicit scheme it is at least the
 * number of reservoir and tracer entries on one row (one galaxy's block), which
 * covers the block solve's rounding. L is a maximum over the whole component,
 * so a central receiving one transfer per satellite has an L of about the
 * satellite count, and the closure tolerance of every total in that component
 * grows, and its sensitivity to an imbalance falls, in proportion.
 *
 * Fixed steps. With settings.fixed_steps = n > 0 the segment is n equal steps
 * with the controller off and no error estimate, under either scheme. The
 * positivity checks stay on; a fixed step that fails one fails the segment
 * with RATE_CAUSE_REJECTION_BUDGET: a fixed step allows no retry, so its first
 * rejection exhausts the retry budget. The record names the limiting entry, its
 * outflow, tau and the step.
 *
 * Memory and state. The integrator allocates nothing: the caller supplies a
 * workspace of rate_integrator_workspace_bytes() bytes for the scheme, aligned
 * for double. It keeps no state between calls, so the step sequence is a pure
 * function of the component, the start state, the law, the settings and the
 * Delta values. It works on a copy and writes the caller's state back only on
 * success: a failed segment leaves the state, the external amounts and the flux
 * amounts untouched. A malformed tracer or slaved-source link (struct
 * RateEntry, struct RateFlux) is a programming error and fatal.
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

/** The integration scheme (RateIntegratorSettings.scheme); the zero value is the default. */
enum RateScheme {
  RATE_SCHEME_EXPLICIT = 0, /* SSPRK(4,3) with the positivity guard: the default */
  RATE_SCHEME_IMPLICIT      /* Patankar-Euler with frozen coefficients: never the default */
};

/** What a flux's coefficient means and which ends it has. */
enum RateFluxKind {
  RATE_FLUX_INTERNAL, /* entry to entry; coefficient is a specific rate (inverse time) */
  RATE_FLUX_SOURCE,   /* external to entry; coefficient is an absolute rate (content per time) */
  RATE_FLUX_SINK,     /* entry to external; coefficient is a specific rate (inverse time) */
  RATE_FLUX_SLAVED    /* external to entry; coefficient is dimensionless, times the master's flux */
};

/** What a state entry holds; the zero value is a reservoir. */
enum RateEntryKind {
  RATE_ENTRY_RESERVOIR = 0, /* a reservoir's content, a member of a conserved total */
  RATE_ENTRY_TRACER         /* a tracer's content, moved with its parent's outflows */
};

/**
 * @brief   One state entry: the content of one reservoir or tracer of one row
 *
 * A tracer's `parent` is the entry index of a reservoir of the same row whose
 * total differs from the tracer's; one parent has at most one tracer per total.
 * A reservoir's `parent` is not read.
 */
struct RateEntry {
  int row;   /* index into RateComponent.row_delta */
  int total; /* conserved total, 0 <= total < RateComponent.num_totals */
  enum RateEntryKind kind;
  int parent; /* a tracer's parent entry */
};

/**
 * @brief   One flux of a component
 *
 * `from` is an entry index for an internal transfer or a sink and
 * RATE_END_EXTERNAL for a source or a slaved source; `to` is an entry index for
 * an internal transfer, a source or a slaved source and RATE_END_EXTERNAL for a
 * sink. An internal transfer's two ends belong to one total; its ends, and a
 * sink's donor, are reservoirs (tracers move without declaration). An internal
 * transfer whose two ends are the same entry moves nothing; its coefficient is
 * still evaluated and checked. `global_index` is the transfer's global index;
 * (clock row, global_index) orders the fluxes ("Canonical order"). `master` is read
 * for a slaved source only: the caller's index of its master, a declared
 * internal transfer or sink.
 */
struct RateFlux {
  enum RateFluxKind kind;
  int from;
  int to;
  int global_index;
  int master; /* a slaved source's master flux */
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
 * only the law knows what failed. It must not keep the state pointer. The
 * coefficient array it fills stays valid, and its slots unchanged, until the
 * next evaluation begins, so the law's data may refer to it until then instead
 * of copying it.
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
 * step-start state after a rejection; `amounts` is each declared flux's
 * committed amount, in the component's flux order, after an accepted step (zero
 * for a flux whose two ends are one entry, which moves nothing), and NULL after
 * a rejection. Both are valid only during the call. `error_norm` and
 * `min_weight` are negative when the attempt ended before its error estimate (a
 * positivity rejection, or any fixed step).
 */
struct RateAttempt {
  double tau;  /* segment time at the step start */
  double step; /* step length in tau */
  int accepted;
  enum RateRejection rejection;
  double error_norm; /* err of the attempt */
  double min_weight; /* the smallest weight w_i of the attempt */
  const double *state;
  const double *amounts;
};

/** Scheme, tolerances, budgets and the optional attempt callback of one segment. */
struct RateIntegratorSettings {
  enum RateScheme scheme;        /* RATE_SCHEME_EXPLICIT (default) or RATE_SCHEME_IMPLICIT */
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
  RATE_CAUSE_LEDGER,           /* a conserved total failed the segment closure */
  RATE_CAUSE_TRACER_BOUND      /* a tracer ended above its parent beyond the rounding bound */
};

/**
 * @brief   What failed, where and when
 *
 * `entry` and `flux` are the caller's indices (RateComponent order), or -1; a
 * limiter that is a tracer move is named by its master transfer.
 *
 * - bad_coefficient: the first offending flux in canonical order and its donor
 *   (its recipient for a source or a slaved source). A law that returns
 *   non-zero names neither. Under the implicit scheme the offending flux may be
 *   one whose step times rate is not finite, named the same way; a block pivot
 *   that is not finite names its entry and that entry's outflow with the
 *   largest step times rate. `detail` says which of these two it was.
 * - step_budget, step_underflow and rejection_budget in adaptive mode: the
 *   latest limiter met in the segment, which is one of
 *   - (explicit scheme) a step clamped to the positivity cap (the cap binds
 *     when the step reaches it), or rejected because step * K_max exceeded C at
 *     a stage: the entry attaining K_max at that state and its outflow with the
 *     largest coefficient there;
 *   - a rejection because an entry went below zero (under the implicit scheme,
 *     a solved entry that is not finite and non-negative): the first such entry
 *     in entry order and its outflow with the largest amount there;
 *   - an attempt whose error estimate ran with a non-zero error: the entry with
 *     the largest weighted error and its flux, outflow or inflow, with the
 *     largest error amount (outflows first, canonical order, on ties);
 *   - while none of these has been met: the start-of-step limiter, the entry
 *     attaining K_max at the step start and its outflow with the largest
 *     coefficient, or, when K_max is zero (no entry with content has an outflow
 *     with a non-zero coefficient), the flux with the largest start-of-step rate
 *     and its donor (recipient for a source).
 * - Fixed steps: step_budget names the failing step's start-of-step limiter; a
 *   positivity rejection (rejection_budget) names its own limiter as above.
 * - ledger: `total` names the total and both are -1.
 * - tracer_bound: the first failing tracer in entry order, its total, and its
 *   first source or slaved source in canonical order (-1 when it has none);
 *   `detail` says whether the tracer has sources of its own.
 */
struct RateFailure {
  enum RateCause cause;
  int entry;
  int flux;
  int row;            /* the row of `entry`, or the flux's clock row; -1 when neither is named */
  int total;          /* the failing total of a ledger or tracer_bound failure, else -1 */
  double tau;         /* segment time at the start of the failing step */
  double step;        /* the step being attempted, or 0 */
  const char *detail; /* a static sentence qualifying the cause, or "" */
};

/** Step statistics of one segment, complete on failure too. */
struct RateStepStats {
  int64_t accepted_steps;
  int64_t rejected_steps; /* positivity_rejections + error_rejections */
  int64_t positivity_rejections;
  int64_t error_rejections;
  int64_t law_evaluations; /* every stage, and the implicit scheme's evaluation at every trial
                              end */
  double max_k_max;        /* the largest K_max at any state where the laws were evaluated */
};

/** Everything a segment reports besides its state and external amounts. */
struct RateSegmentOutcome {
  struct RateFailure failure; /* cause RATE_CAUSE_NONE on success */
  struct RateStepStats stats;
  /* L: the largest number of inflow plus outflow terms on one entry; under the implicit
   * scheme, at least the most reservoir and tracer entries on one row (its block). */
  int max_terms_per_entry;
};

/**
 * @brief   Inputs of the segment closure check of one component
 *
 * For every total F, |sum_F end - sum_F start - (Src_F - Snk_F)| must not
 * exceed tol_F = 8u [(L + 2) N_acc + n_F + 2] (sum_F |start| + Src_F + Snk_F),
 * u = 2^-53, every sum taken in entry order. L is a component-wide maximum
 * (rate_integrator.h, Segment-end checks).
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

/** @brief   Fill @p settings with the defaults: explicit scheme, adaptive, no attempt callback. */
void rate_integrator_default_settings(struct RateIntegratorSettings *settings);

/** @brief   The cause's name as the failure messages print it, e.g. "step_underflow". */
const char *rate_cause_name(enum RateCause cause);

/**
 * @brief   One sentence naming @p scheme's method and its compile-time step-control constants
 *
 * Formatted from the macros the integrator uses, so the recorded description
 * cannot drift from the constants. The string is static, overwritten only with
 * the same text. An unknown scheme is a programming error and fatal.
 */
const char *rate_integrator_method_description(enum RateScheme scheme);

/**
 * @brief   Workspace bytes rate_integrate_segment() needs for @p component under @p scheme
 *
 * Linear in the number of rows for a fixed number of entries per row. Under the
 * implicit scheme the size assumes each row's entries are listed together,
 * which rate_integrate_segment() checks.
 */
size_t rate_integrator_workspace_bytes(const struct RateComponent *component,
                                       enum RateScheme scheme);

/**
 * @brief   Advance one component across one segment, tau from 0 to 1
 *
 * @param   component       The entries, fluxes, row Delta values and totals
 * @param   law             The laws (coefficients for a state)
 * @param   settings        Scheme, tolerances, budgets, fixed-step count and callback
 * @param   workspace       Caller-owned scratch, aligned for double
 * @param   workspace_bytes At least rate_integrator_workspace_bytes(component, scheme)
 * @param   state           [num_entries]: start state in, end state out on success
 * @param   source_amounts  [num_totals] or NULL: per-total external inflow, out on success
 * @param   sink_amounts    [num_totals] or NULL: per-total external outflow, out on success
 * @param   flux_amounts    [num_fluxes] or NULL: each declared flux's committed amount over the
 *                          segment, in the caller's flux order ("Flux amounts"), out on success
 * @param   outcome         Failure record, step statistics and L (always written)
 * @return  RATE_CAUSE_NONE on success, else the failure cause (also in outcome->failure)
 *
 * Inputs outside the domain (a negative reservoir or tracer entry, a negative or
 * non-finite Delta, tolerances outside (0, 1), a malformed flux) are unspecified
 * behaviour. A workspace smaller than required, an unknown scheme, a malformed
 * tracer or slaved-source link, and under the implicit scheme a component it
 * cannot block (see "The implicit scheme" above), are programming errors and
 * fatal.
 */
enum RateCause rate_integrate_segment(const struct RateComponent *component,
                                      const struct RateLaw *law,
                                      const struct RateIntegratorSettings *settings,
                                      void *workspace, size_t workspace_bytes, double *state,
                                      double *source_amounts, double *sink_amounts,
                                      double *flux_amounts, struct RateSegmentOutcome *outcome);

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
