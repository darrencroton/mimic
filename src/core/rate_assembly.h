#ifndef CORE_RATE_ASSEMBLY_H
#define CORE_RATE_ASSEMBLY_H

/**
 * @file    rate_assembly.h
 * @brief   The rate phase: its startup plan, and one segment's components built from a FoF
 * workspace
 *
 * A rate phase is a phase under modules.phases whose entries are all
 * process_rates. At startup, rate_assembly_plan_build() checks the configured
 * rate modules against the generated reservoir table and against each other,
 * and fixes the integration domain; once per substep, rate_assembly_run()
 * gathers the workspace into components, integrates each across the substep
 * with rate_integrate_segment() (rate_integrator.h) and writes the result back.
 *
 * Startup checks (each refusal logs "Configuration error in phase '<phase>':"
 * and names the module and transfer, or the totals):
 * - every transfer's ends are what its kind allows, read against the
 *   reservoir table: an internal transfer runs between two reservoirs of one
 *   total; a sink leaves a reservoir; a source or slaved source enters a
 *   reservoir or a tracer; only a transfer into a record may go to the central;
 * - every observer names a property of kind observer and a transfer of its module,
 *   and no module observes one transfer into one property twice (the
 *   property would receive the amount twice);
 * - transfer global indices are unique among the configured transfers;
 * - every slaved source's master is a transfer of a module configured in the
 *   phase, and is an internal transfer or a sink;
 * - the totals form no dependency cycle: a slaved source's recipient total
 *   depends on its master's donor total, and a tracer's total on its parent's;
 * - the settings (struct MimicRateIntegration) lie in their ranges (check_purity
 *   is 0 or 1; max_rejections_per_step is at least RATE_MIN_REJECTIONS_PER_STEP).
 *
 * Domain. The whole FoF workspace is one component when a configured transfer
 * has to_record: central or a configured module declares reads_central;
 * otherwise every row is its own component. The domain is fixed at startup
 * from declared metadata only.
 *
 * A segment. The active rows are those with a galaxy and Type != 3, in
 * workspace order. A component's entries are, row by row (row index among its
 * active rows, then reservoir table index): one entry per reservoir and tracer
 * of the table; an observer is not integrator state and has no entry. Its
 * fluxes are, row by row, one per configured transfer in global transfer
 * order, so the entry and flux order is decision-free and never depends on
 * run-file order, registration order, IDs or pointers. Row r's segment length is
 * Delta_r = dT_r / num_substeps; a row carrying the first-snapshot sentinel
 * (SnapNum < 0 and dT <= 0) has Delta_r = 0, and any other row whose dT is not
 * positive and finite is fatal, naming the row. The gather reads each row's
 * reservoir and tracer fields as the start state; an entry that is negative or
 * not finite is fatal before any law or step sees it, naming the phase, the
 * snapshot and substep, the workspace row and its UniqueGalaxyID, the property
 * and the value, and saying that the value was left before the segment, by a
 * discrete phase or by initialisation. It is refused, never clamped.
 *
 * A law evaluation. Each active row has a scratch copy of its halo whose galaxy
 * pointer addresses a scratch copy of its galaxy, taken at segment start; at
 * every evaluation the reservoir and tracer fields of every row's scratch
 * galaxy are overwritten with the trial state (observer fields keep their
 * segment-start values, since an observer has no entry), and then each
 * configured module's law is called for each row, rows in order and modules by
 * name, with its coefficient slots zeroed. A module that declares reads_central
 * receives the central's scratch copy (at the same trial state) as
 * RateContext.central; every other module receives NULL. RateContext.time is
 * the row's own lookback time at the evaluation point, ctx->time + dT_r -
 * (substep + tau) Delta_r, with dT_r taken as 0 for a sentinel row.
 *
 * Purity probe. With settings->check_purity set, a segment's first law
 * evaluation also snapshots the scratch rows and evaluates every law a second
 * time, rows and modules in reversed canonical order, with the same trial
 * state. Every coefficient must match the first pass bit for bit (compared as
 * 64-bit patterns), a law may not return non-zero the second time, and the
 * scratch halo and galaxy a law was given (and the central's, for a module that
 * reads it) must be byte-identical to the snapshot after every call; a
 * mismatch is fatal, naming the module and transfer, and writes the failure
 * file. With it off no law is evaluated beyond the integrator's own.
 *
 * Write-back. On success the reservoir and tracer fields of each row's galaxy
 * take the end state, and each configured observer's property gains the amount
 * its transfer moved on that row over the segment (the integrator's flux
 * amount; several observers of one property add in global transfer order);
 * nothing else of the scratch copies is written back. On failure the run ends
 * through FATAL_ERROR and the workspace is left as it was. The message always
 * names the cause and tau, and each of these only where the cause has it: the
 * module and transfer (a law that returned non-zero is named by its module and
 * return code), the galaxy (the workspace row whose law it is and its
 * UniqueGalaxyID, with the entry's row as well when that differs) and the
 * entry. A ledger failure names the total and, since it names no row, the
 * component's row count and its first row's UniqueGalaxyID in place of a
 * galaxy; a tracer_bound failure names the tracer, its total and the tracer's
 * first source or slaved source when it has one. A field the failure does not
 * name is left out of the message.
 *
 * Failure file. Before FATAL_ERROR, for a failed segment and a failed purity
 * probe alike, <output_directory>/rate_failure_task<N>.txt is written, N the
 * MPI task number (0 in a serial run); with no output directory configured, or
 * when the file cannot be written, a warning is logged and the failure still
 * ends the run. It is plain text: the build's version and git commit
 * (mimic_version, git_commit), so a file left by an earlier build can be told
 * apart; the phase, snapshot and substep, the law's base time (the module
 * context's time, from which each row's RateContext.time is formed) and the
 * redshift, so a time-dependent law can be replayed; the run's rate settings;
 * the cause (a rate_cause_name(), or impure_law), module, transfer, tau and
 * step; one line per row with its workspace row, UniqueGalaxyID, Type, dT and
 * segment length Delta; one line per entry with the segment-start state (the
 * integrator leaves it untouched on a failure); and one line per row and
 * transfer with the coefficient of the failing law evaluation (the latest
 * one), marked not_evaluated where its law had not yet returned when the
 * evaluation ended (a law returned non-zero, or the probe failed mid-pass).
 * Every floating-point value is a C99 hexadecimal float (%a), exact; a value
 * that is not finite also carries its bit pattern. The file is written only on
 * a failure; a later successful run into the same output directory does not
 * remove it.
 *
 * Step statistics. rate_assembly_run() adds every segment it integrates (one
 * component across one substep) to a struct RateStatistics: the segment count;
 * the accepted steps, rejected steps and law evaluations the integrator
 * reports (the purity probe's repeat evaluations are not among them); a
 * histogram of the segment's K_max, the largest it met at any state where the
 * laws were evaluated, in whole-octave bins taken from the binary exponent,
 * with dedicated bins for zero, underflow and overflow; a histogram of the
 * segment's accepted steps by bit length; and the exact maxima of each per
 * segment. The registry adds the discrete-phase ledger's declared changes per
 * total (module_registry.h). Every member is an unsigned 64-bit integer, a
 * maximum of K_max kept as the bit pattern of the non-negative double (whose
 * order the patterns share), so tasks merge by integer sum and maximum and the
 * result is the same for any task count, chunk count or driver; no
 * floating-point sum is kept.
 *
 * Memory. Everything a segment needs (the row, entry and flux tables, the flux
 * amounts, the scratch rows, the purity probe's snapshots when it is on, and
 * the integrator's workspace, which holds the latest coefficients a failure
 * file reads) lives in one caller-owned tracked MEM_UTILITY allocation (struct
 * RateAssemblyScratch), grown to the high water of the largest component and
 * released by rate_assembly_scratch_release(). The plan is one tracked
 * MEM_UTILITY allocation of its own, released by rate_assembly_plan_free().
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "module_interface.h"
#include "module_registry.h"
#include "generated/reservoir_table.h"

struct FoFWorkspace; /* fof_workspace.h */

/** The startup plan of the rate phase (opaque; rate_assembly_plan_build()). */
struct RateAssemblyPlan;

/** Lowest and highest octave of the K_max histogram: bin [2^k, 2^(k+1)) for k in this range */
#define RATE_STATS_K_MAX_MIN_EXPONENT (-64)
#define RATE_STATS_K_MAX_MAX_EXPONENT 63

/** K_max histogram bins: zero, underflow (below 2^-64), one per octave, overflow (2^64 and up) */
#define RATE_STATS_K_MAX_ZERO_BIN 0
#define RATE_STATS_K_MAX_UNDERFLOW_BIN 1
#define RATE_STATS_K_MAX_FIRST_OCTAVE_BIN 2
#define RATE_STATS_K_MAX_OCTAVES (RATE_STATS_K_MAX_MAX_EXPONENT - RATE_STATS_K_MAX_MIN_EXPONENT + 1)
#define RATE_STATS_K_MAX_OVERFLOW_BIN (RATE_STATS_K_MAX_FIRST_OCTAVE_BIN + RATE_STATS_K_MAX_OCTAVES)
#define RATE_STATS_K_MAX_BINS (RATE_STATS_K_MAX_OVERFLOW_BIN + 1)

/** Accepted-steps histogram bins: bin b counts segments whose step count has bit length b */
#define RATE_STATS_STEP_BINS 65

/** Slots of the per-total counts: one per conserved total, at least one so the array is valid */
#define RATE_STATS_TOTAL_SLOTS (MIMIC_RESERVOIR_TOTAL_COUNT > 0 ? MIMIC_RESERVOIR_TOTAL_COUNT : 1)

/** The summed step statistics (merged across MPI tasks by integer sum). */
struct RateStatisticsCounts {
  uint64_t segments;        /* (component, substep) pairs integrated */
  uint64_t accepted_steps;  /* over every segment */
  uint64_t rejected_steps;  /* over every segment */
  uint64_t law_evaluations; /* integrator evaluations, each one call per row and module */
  uint64_t k_max_histogram[RATE_STATS_K_MAX_BINS];
  uint64_t accepted_steps_histogram[RATE_STATS_STEP_BINS];
  /* Units of a discrete phase whose change to total t its declarations covered */
  uint64_t declared_changes[RATE_STATS_TOTAL_SLOTS];
};

/** The per-segment maxima (merged across MPI tasks by integer maximum). */
struct RateStatisticsMaxima {
  uint64_t accepted_steps;  /* the most accepted steps in one segment */
  uint64_t rejected_steps;  /* the most rejected steps in one segment */
  uint64_t law_evaluations; /* the most law evaluations in one segment */
  uint64_t k_max_bits;      /* bit pattern of the largest K_max (a non-negative double) */
};

/** A run's (or one task's) rate statistics; zero-initialise before the first segment. */
struct RateStatistics {
  struct RateStatisticsCounts counts;
  struct RateStatisticsMaxima maxima;
};

/** Number of uint64_t words of each half, for an MPI reduction over the struct as an array */
#define RATE_STATS_COUNT_WORDS (sizeof(struct RateStatisticsCounts) / sizeof(uint64_t))
#define RATE_STATS_MAXIMA_WORDS (sizeof(struct RateStatisticsMaxima) / sizeof(uint64_t))
_Static_assert(sizeof(struct RateStatisticsCounts) == RATE_STATS_COUNT_WORDS * sizeof(uint64_t),
               "struct RateStatisticsCounts must be an array of uint64_t");
_Static_assert(sizeof(struct RateStatisticsMaxima) == RATE_STATS_MAXIMA_WORDS * sizeof(uint64_t),
               "struct RateStatisticsMaxima must be an array of uint64_t");

/** One configured transfer as the output records it (rate_assembly_plan_transfer_record()). */
struct RateTransferRow {
  const char *module;    /* the rate module's name */
  const char *transfer;  /* the transfer's name */
  const char *from;      /* a property name, or "external" */
  const char *to;        /* a property name, or "external" */
  const char *to_record; /* "self" or "central" */
  const char *kind;      /* "internal", "sink", "source" or "slaved_source" */
  const char *master;    /* a slaved source's master as <module>.<transfer>, else "" */
};

/**
 * @brief   Calls one module's law; the registry supplies it to mark the running callback
 *
 * @return  The law's return value (non-zero fails the segment)
 */
typedef int (*RateLawInvoker)(const struct Module *module, const struct RateContext *ctx,
                              const struct Halo *halo, double *coefficients);

/** The grow-to-high-water scratch of rate_assembly_run(); zero-initialise before first use. */
struct RateAssemblyScratch {
  void *bytes;     /* one tracked MEM_UTILITY allocation, or NULL */
  size_t capacity; /* bytes allocated; only ever grows */
};

/**
 * @brief   Check the rate phase's modules and build its plan
 *
 * @param   phase_name   The rate phase's name, kept for diagnostics (must outlive the plan)
 * @param   entries      The phase's entries, every one process_rates, resolved and distinct
 * @param   num_entries  Number of entries (> 0)
 * @param   settings     The run's rate integration settings
 * @param   plan_out     Receives the plan on success, NULL on failure
 * @return  0 on success; -1 after an ERROR_LOG naming the phase and the refusal
 */
int rate_assembly_plan_build(const char *phase_name, const struct PhaseModuleConfig *entries,
                             int num_entries, const struct MimicRateIntegration *settings,
                             struct RateAssemblyPlan **plan_out);

/** @brief Release a plan (NULL is a no-op). */
void rate_assembly_plan_free(struct RateAssemblyPlan *plan);

/**
 * @brief   Integrate every component of a FoF workspace across the current substep
 *
 * @param   plan      The rate phase's plan
 * @param   ctx       The pipeline's module context (snapshot, time, substep, central)
 * @param   ws        The FoF workspace; reservoir, tracer and observer fields are updated
 * @param   scratch   Grow-to-high-water scratch, owned by the caller
 * @param   invoke    Calls one module's law
 * @param   stats     Receives every integrated segment's step statistics
 *
 * Fatal on a failed segment and on a non-sentinel row whose dT is not
 * positive and finite.
 */
void rate_assembly_run(const struct RateAssemblyPlan *plan, const struct ModuleContext *ctx,
                       struct FoFWorkspace *ws, struct RateAssemblyScratch *scratch,
                       RateLawInvoker invoke, struct RateStatistics *stats);

/** @brief The number of configured transfers, the rows of RunProperties/RateTransfers. */
int rate_assembly_plan_num_transfers(const struct RateAssemblyPlan *plan);

/**
 * @brief   Describe configured transfer @p index, in global transfer order
 *
 * The strings live as long as the plan and the generated tables.
 */
void rate_assembly_plan_transfer_record(const struct RateAssemblyPlan *plan, int index,
                                        struct RateTransferRow *record);

/** @brief The scheme's name as the run file spells it: "explicit" or "implicit". */
const char *rate_assembly_scheme_name(int scheme);

/** @brief K_max histogram bin of a segment's K_max (zero, underflow, an octave or overflow). */
int rate_statistics_k_max_bin(double k_max);

/** @brief Accepted-steps histogram bin of a step count: its bit length, 0 to 64. */
int rate_statistics_step_bin(uint64_t accepted_steps);

/** @brief The largest K_max the statistics hold, decoded from its bit pattern. */
double rate_statistics_max_k_max(const struct RateStatistics *stats);

/** @brief Log the one-line INFO_LOG summary of a run's merged statistics. */
void rate_statistics_log_summary(const struct RateStatistics *stats);

/**
 * @brief   Append one item to a comma-separated list for a diagnostic
 *
 * Each item is written whole or not at all: an item that does not fit, with
 * room kept for ", ...", ends the list with ", ..." (or "..." when it is the
 * first) instead.
 *
 * @param   buffer  The list, NUL-terminated (empty before the first item)
 * @param   size    Bytes of @p buffer, at least 8
 * @param   used    The list's length; advanced past the item
 * @return  true when the item was written; false when the list was ended instead
 */
bool rate_append_list_item(char *buffer, size_t size, size_t *used, const char *item);

/** @brief Release the scratch and zero its descriptor (safe when empty). */
void rate_assembly_scratch_release(struct RateAssemblyScratch *scratch);

/**
 * @brief   Find a dependency cycle among totals
 *
 * @param   num_totals  Number of totals (nodes)
 * @param   depends     [num_totals * num_totals]: depends[a * num_totals + b] != 0 when
 *                      total a depends on total b (a self-dependency is a cycle)
 * @param   cycle       [num_totals + 1]: receives the cycle as total indices, its first
 *                      total repeated at the end, each depending on the next
 * @return  The number of totals written to @p cycle (first repeated, so >= 2), or 0
 *          when there is no cycle
 */
int rate_assembly_find_total_cycle(int num_totals, const int *depends, int *cycle);

#endif /* CORE_RATE_ASSEMBLY_H */
