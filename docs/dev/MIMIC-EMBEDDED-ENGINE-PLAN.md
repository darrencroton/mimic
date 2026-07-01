# Mimic Embedded Engine Plan (Physics-Only API)

**Status:** Requirements brief. Split out of `MIMIC-DUAL-DRIVER-PLAN.md` (its former Phase 6) in the 2026-07-02 joint review because it shares only the v1.0 Phase 1–2 seams and nothing snapshot-specific. Not scheduled; promote to an active plan when scientific need arises.
**Date:** 2026-07-02

---

## Goal

Expose a documented physics-only API that lets an external host initialise Mimic core services and run configured modules over host-supplied halos. The host owns halo finding, progenitor tracking, ordering, and I/O. The inheritance service remains internal to Mimic drivers.

The seam already exists: the module unit-test harnesses drive modules with a hand-built `ModuleContext` + `Halo[]` and no merger tree, which is a working proof of concept for external invocation.

## Scoping Notes (carried over verbatim in substance)

Thread the engine entry points through an explicit engine-state argument with a default global instance, so internal drivers and single-instance hosts are unaffected. True reentrancy is **not** purely a `ModuleContext` change: the module ABI also includes `init(void)` and `cleanup(void)`, which take no arguments and read globals (`MimicConfig`, units, `Age`/`ZZ`, the registry) directly. Threading state through `ModuleContext` covers only the `process()` path. Reaching instance config from `init`/`cleanup` requires either an init-time "current engine instance" mechanism, or an explicit decision that init-time configuration stays process-global while only per-timestep state is instanced. Either is acceptable, but the gap must not be under-scoped as a `ModuleContext` change alone. Document remaining globals before offering multi-instance or threaded guarantees.

## Gate (when activated)

An external example host runs the engine and matches an equivalent in-tree result on a shared fixture. Standard checks and tests stay green; the ordinary physics-module ABI is preserved.
