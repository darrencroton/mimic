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
 * - every observer names a property of kind observer and a transfer of its module;
 * - transfer global indices are unique among the configured transfers;
 * - every slaved source's master is a transfer of a module configured in the
 *   phase, and is an internal transfer or a sink;
 * - the totals form no dependency cycle: a slaved source's recipient total
 *   depends on its master's donor total, and a tracer's total on its parent's;
 * - the settings (struct MimicRateIntegration) lie in their ranges (check_purity
 *   is 0 or 1).
 *
 * Domain. The whole FoF workspace is one component when a configured transfer
 * has to_record: central or a configured module declares reads_central;
 * otherwise every row is its own component. The domain is fixed at startup
 * from declared metadata only.
 *
 * A segment. The active rows are those with a galaxy and Type != 3, in
 * workspace order. A component's entries are, row by row (row index among its
 * active rows, then reservoir table index): one entry per reservoir and tracer
 * of the table, and one entry per configured observer of an observer property
 * (several observers of one property in global transfer order). Its fluxes are,
 * row by row, one per configured transfer in global transfer order, so the
 * entry and flux order is decision-free and never depends on run-file order,
 * registration order, IDs or pointers. Row r's segment length is
 * Delta_r = dT_r / num_substeps; a row carrying the first-snapshot sentinel
 * (SnapNum < 0 and dT <= 0) has Delta_r = 0, and any other row whose dT is not
 * positive and finite is fatal, naming the row.
 *
 * A law evaluation. Each active row has a scratch copy of its halo whose galaxy
 * pointer addresses a scratch copy of its galaxy, taken at segment start; at
 * every evaluation the reservoir and tracer fields of every row's scratch
 * galaxy are overwritten with the trial state (observer fields keep their
 * segment-start values, since observers are out of error control), and then each
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
 * Write-back. On success the reservoir, tracer and observer fields of each
 * row's galaxy take the end state (an observer property the sum of its
 * entries, the first of which started at the property's value); nothing else
 * of the scratch copies is written back. On failure the run ends through
 * FATAL_ERROR, naming the cause, the module and transfer, the row whose law it
 * is (and the entry's row when that differs), the entry and tau, and the
 * workspace is left as it was.
 *
 * Failure file. Before FATAL_ERROR, for a failed segment and a failed purity
 * probe alike, <output_directory>/rate_failure_task<N>.txt is written, N the
 * MPI task number (0 in a serial run); with no output directory configured, or
 * when the file cannot be written, a warning is logged and the failure still
 * ends the run. It is plain text: the run's rate settings; the cause (a
 * rate_cause_name(), or impure_law), module, transfer, tau and step; one line
 * per row with its workspace row, UniqueGalaxyID, Type, dT and segment length
 * Delta; one line per entry with the segment-start state (the integrator
 * leaves it untouched on a failure); and one line per row and transfer with
 * the coefficient of the failing law evaluation (the latest one). Every
 * floating-point value is a C99 hexadecimal float (%a), exact; a value that is
 * not finite also carries its bit pattern.
 *
 * Memory. Everything a segment needs (the row, entry and flux tables, the
 * scratch rows, the latest coefficients, the purity probe's snapshots when it
 * is on, and the integrator's workspace) lives in one caller-owned
 * tracked MEM_UTILITY allocation (struct RateAssemblyScratch), grown to the
 * high water of the largest component and released by
 * rate_assembly_scratch_release(). The plan is one tracked MEM_UTILITY
 * allocation of its own, released by rate_assembly_plan_free().
 */

#include <stdbool.h>
#include <stddef.h>

#include "module_interface.h"
#include "module_registry.h"

struct FoFWorkspace; /* fof_workspace.h */

/** The startup plan of the rate phase (opaque; rate_assembly_plan_build()). */
struct RateAssemblyPlan;

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
 *
 * Fatal on a failed segment and on a non-sentinel row whose dT is not
 * positive and finite.
 */
void rate_assembly_run(const struct RateAssemblyPlan *plan, const struct ModuleContext *ctx,
                       struct FoFWorkspace *ws, struct RateAssemblyScratch *scratch,
                       RateLawInvoker invoke);

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
