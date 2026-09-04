# Mimic Snapshot-Global Module Contracts Plan

**Status:** Requirements brief. Its prerequisite is met — the snapshot driver passed its cross-format identity gate on 2026-08-12 (`MIMIC-DUAL-DRIVER-PLAN.md` Phase 5) — so this brief is unblocked. Not yet scheduled.
**Date:** 2026-07-02

---

## Goal

Define how physics modules operate on a whole co-resident snapshot population instead of one FoF workspace. This is the scientific payoff of the snapshot driver: the driver makes the population co-resident, but today's module contracts (`PROCESSING_MODE_FULL_HALO` / `BY_GALAXY` / `PER_EVENT`) only ever see one FoF group. A snapshot-global contract — a new processing mode or a per-snapshot phase hook that receives the full population — is what unlocks the methods that motivated the dual-driver work: true global abundance matching (SHAM by rank over the whole box), HOD-style statistical population, environment-dependent physics, a synchronous reionization radiation field, and on-the-fly lightcone assembly.

## Scope and Independent Value

Single-node only. **This "expected to fit" claim is now measured and refuted.** The 2026-08-13 ≈317 GB projection (recomputed here from measured struct sizes, clear of the 85% fallback trigger) predated the production conversion; the real production `conversion_report.json` puts Shin-Uchuu's largest slab at 519,342,987 halos (snapshot 34), 46.5% above the rehearsal-scale projection this plan's figure was built on, re-projecting to **≈639–697 GB** — well past 512 GB installed on the Mac Studio (`SHIN-UCHUU-CONVERSION-PLAN.md` → "P3b, P4 done and measured 2026-09-04", 2026-09-04). Shin-Uchuu itself is therefore **not** single-node-feasible on current hardware as this section claimed; the production `sage16` run is relocating to an NT large-memory node for that reason, and the durable single-node fix is tracked separately as a concept note, [`MIMIC-CHUNKED-SLAB-STREAMING-PLAN.md`](MIMIC-CHUNKED-SLAB-STREAMING-PLAN.md). **This plan's own value is not refuted by that** — a global SHAM module built and gated on the micro-Uchuu fixture (small enough to fit trivially) delivers everything this section's "Independent Value" argument claims regardless of Shin-Uchuu's footprint; only the claim that Shin-Uchuu specifically is single-node-feasible today should be treated as retracted. The first concrete candidate is a true global SHAM module (the existing `sham_assign_stellar_mass` is per-galaxy from `ShamVpeak` and never needed the co-resident population).

## Relationship to Other Plans

- **Requires:** the snapshot driver with its identity gate green (`MIMIC-DUAL-DRIVER-PLAN.md`). The dual-driver plan deliberately excludes global module contracts from its gate so driver acceptance is not conflated with new physics contracts.
- **Prerequisite for:** `MIMIC-DISTRIBUTED-SNAPSHOT-PLAN.md`, which parallelises snapshot-global operations across domains — there is nothing to distribute until at least one such contract exists single-node.
- **Unrelated to:** the embedded engine (external hosts driving FoF-scoped modules) and the model builder (assisted model-package construction); both operate within the existing FoF-scoped module contracts.

## Constraints Carried Forward

- The ordinary FoF-scoped physics-module ABI stays frozen; a snapshot-global contract is additive (new mode/phase in module metadata), never a change to `process(ctx, halos, ngal)`.
- Determinism: global operations must be reproducible for a given input — stable ordering for rank ties, stable per-halo seeding, no traversal-order RNG.
- Cross-format identity for FoF-scoped physics must remain green with snapshot-global modules disabled; runs using snapshot-global modules are snapshot-driver-only by definition and make no tree-driver identity claim.

**One of the methods this brief exists to unlock may not be a one-way population operation. Recorded 2026-08-20.** [`MIMIC-COUPLED-RATE-FORMULATION-PLAN.md`](MIMIC-COUPLED-RATE-FORMULATION-PLAN.md) treats snapshot-global work as orthogonal to its coupled system because global operations *"are population operations, not transfers"* — true of rank-order SHAM, HOD population and environment measures, which read the population and write per-galaxy results one way. A **synchronous reionization radiation field couples in both directions**, since the field suppresses the sources that produce it. That does not by itself require an implicit solve: the field could be lagged between steps, evolved causally, or made consistent within a snapshot, and those are different contracts. Which of them the mode supports — or that it excludes the case — is a decision for this brief's implementation plan, cheaper made before the machinery exists than retrofitted after. The shipped `sage16` reionization is not this case; it is the one-way algebraic prescription (`SAGE16-PRESCRIPTION-CLASSIFICATION.md` item 1).

## Gate (when activated)

A first snapshot-global module (global SHAM) runs on the micro-Uchuu snapshot fixture with documented, reproducible output, and all existing FoF-scoped gates stay green.
