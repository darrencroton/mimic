# Mimic Snapshot-Global Module Contracts Plan

**Status:** Requirements brief. Its prerequisite is met — the snapshot driver passed its cross-format identity gate on 2026-08-12 (`MIMIC-DUAL-DRIVER-PLAN.md` Phase 5) — so this brief is unblocked. Not yet scheduled.
**Date:** 2026-07-02

---

## Goal

Define how physics modules operate on a whole co-resident snapshot population instead of one FoF workspace. This is the scientific payoff of the snapshot driver: the driver makes the population co-resident, but today's module contracts (`PROCESSING_MODE_FULL_HALO` / `BY_GALAXY` / `PER_EVENT`) only ever see one FoF group. A snapshot-global contract — a new processing mode or a per-snapshot phase hook that receives the full population — is what unlocks the methods that motivated the dual-driver work: true global abundance matching (SHAM by rank over the whole box), HOD-style statistical population, environment-dependent physics, a synchronous reionization radiation field, and on-the-fly lightcone assembly.

## Scope and Independent Value

Single-node only. Shin-Uchuu's peak snapshot is **expected** to fit on the current hardware — **recomputed 2026-08-13 from measured struct sizes at ≈317 GB against 512 GB installed** for the `sage16` production configuration, clear of the 85% fallback trigger even with the driver retaining two complete raw slabs, though the galaxy-pool high-water is still unmeasured and the rehearsal is the binding check (`POST-PHASE-5-WORK.md` §2.2) — so every method above should be scientifically usable without MPI — this plan delivers value on its own. The first concrete candidate is a true global SHAM module (the existing `sham_assign_stellar_mass` is per-galaxy from `ShamVpeak` and never needed the co-resident population).

## Relationship to Other Plans

- **Requires:** the snapshot driver with its identity gate green (`MIMIC-DUAL-DRIVER-PLAN.md`). The dual-driver plan deliberately excludes global module contracts from its gate so driver acceptance is not conflated with new physics contracts.
- **Prerequisite for:** `MIMIC-DISTRIBUTED-SNAPSHOT-PLAN.md`, which parallelises snapshot-global operations across domains — there is nothing to distribute until at least one such contract exists single-node.
- **Unrelated to:** the embedded engine (external hosts driving FoF-scoped modules) and the model builder (assisted model-package construction); both operate within the existing FoF-scoped module contracts.

## Constraints Carried Forward

- The ordinary FoF-scoped physics-module ABI stays frozen; a snapshot-global contract is additive (new mode/phase in module metadata), never a change to `process(ctx, halos, ngal)`.
- Determinism: global operations must be reproducible for a given input — stable ordering for rank ties, stable per-halo seeding, no traversal-order RNG.
- Cross-format identity for FoF-scoped physics must remain green with snapshot-global modules disabled; runs using snapshot-global modules are snapshot-driver-only by definition and make no tree-driver identity claim.

## Gate (when activated)

A first snapshot-global module (global SHAM) runs on the micro-Uchuu snapshot fixture with documented, reproducible output, and all existing FoF-scoped gates stay green.
