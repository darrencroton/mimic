# Mimic Development Pathway

**Status:** Master context note for the dev plans.
**Date:** 2026-06-02
**Scope:** Explains the intended sequence and how to read the dual-driver and galaxy-model-builder documents.

---

## Purpose

This document is the short framing note for the development plans in `docs/dev/`. It records the intended order of work, the assumptions each plan depends on, and the point at which each document becomes actionable source of truth. It should be read before the dual-driver architecture, the dual-driver change map, or the galaxy model builder proposal.

The larger direction is deliberate: Mimic remains a physics-agnostic galaxy evolution framework, but its scope grows from one tree-ordered execution path into a framework that can run tree-ordered and snapshot-ordered front-ends over a shared core, then eventually support a more automated workflow for building and validating new galaxy formation models. That is a vision extension, not a replacement for the current architectural principles in `docs/VISION.md`.

---

## Intended Sequence

NOTE: `MIMIC-NAMED-SUBSTEP-PHASES.md` has now be archived in `archive/` (2026-06-04).

This sequence was reordered so that named substep phases land
**before** the final optimisation/HDF5/benchmark-freeze, not after v1.0. The
substep refactor changes the output provenance schema and the per-substep
execution structure; doing it first means the optimisation, benchmark baseline,
and frozen output format are produced against the structure v1.0 actually ships,
avoiding a second pass. See the decision rationale folded into this section and
`MIMIC-NAMED-SUBSTEP-PHASES.md`.

1. **Establish the golden SAGE baseline. (DONE — 2026-06-04)** The shared
   `tests/data/output/baseline/` mechanism remains the physics-free core/catalog
   reference. It was extended for SAGE with a model-owned full-physics baseline
   (`models/sage/modules/_tests/test_scientific_sage_physics_baseline.py` against
   `models/sage/modules/_tests/baseline/physics-binary/`) that compares all core
   **and** baryonic properties, plus a local byte-identity gate over the full
   8-file run. This is the safety net for the substep refactor and a down payment
   on the stronger v1.0 baseline.
2. **Generalise substep phase configuration. (DONE — 2026-06-04)** The fixed
   `phase_1`/`phase_2` middle slots are replaced by fixed optional
   `pre_timestep`/`post_timestep` phases plus an arbitrary ordered set of
   user-named substep phases declared under `modules.phases:`. Within each phase,
   full-halo/event work precedes galaxy-local work. The change is byte-identical
   for SAGE (proven by the gate and baseline above). **No backwards
   compatibility was kept:** the legacy `phase_1`/`phase_2`/`enabled` keys were
   removed entirely and the parser now rejects unknown `modules.*` keys. See
   `MIMIC-NAMED-SUBSTEP-PHASES.md`.
3. **Finish Mimic v1.0 preparation.** Complete the remaining core optimisation
   work — memory behaviour, HDF5 writer performance, benchmarking, and a
   clean/lint pass — now against the final named-phase execution structure and
   the final output provenance schema. Then tag v1.0 and refresh/extend the
   golden baseline as the dual-driver acceptance reference. This work is outside
   the dual-driver plan and should not be mixed with it.
4. **Implement the dual-driver architecture and change map.** The dual-driver
   documents become the actionable implementation plan only after v1.0 is tagged
   and the golden baseline exists. The named substep phase contract is now the
   engine's phase-sequence contract. Their migration phases are intentionally
   behaviour-preserving until the snapshot driver is introduced.
5. **Prove the snapshot driver.** The snapshot driver is accepted only when it passes the cross-format identity gate on equivalent converted inputs, with global-only snapshot physics disabled for that comparison.
6. **Review and amend `docs/VISION.md`.** Once the snapshot driver works and passes identity tests, update the vision narrowly: per-driver memory bounds, determinism as an invariant, and a pointer to the dual-driver architecture. (A small, accurate VISION update for named substep phases can also be made now that the behaviour exists.)
7. **Rework the galaxy model builder proposal.** The model builder design is currently aspirational. It should be reviewed and rewritten against the post-v1.0, post-dual-driver codebase before it becomes an exact work plan or source of truth.

---

## How To Read The Dev Docs

`MIMIC-DUAL-DRIVER-ARCHITECTURE.md` is the architectural target for adding tree-ordered and snapshot-ordered front-ends over one shared inheritance and physics execution core. It extends the current vision by generalising the processing and memory model, but it should not be treated as implemented behaviour until the change map has landed and passed its gates.

`MIMIC-DUAL-DRIVER-CHANGE-MAP.md` is the phased migration plan for the dual-driver work. Its gates assume a tagged v1.0 baseline. Where it discusses byte identity, read that as a v1.0 migration requirement, not a claim that the current pre-v1 integration baseline already provides every needed comparison.

`MIMIC-NAMED-SUBSTEP-PHASES.md` documents the user-named substep phase pipeline, which **is now implemented** (2026-06-04). It records the design and the as-built decisions, including that the legacy `phase_1`/`phase_2` form was removed outright rather than kept as a compatibility shim. The within-phase dispatch rule — full-halo/event work precedes galaxy-local work — is preserved. The named phase sequence is the engine's phase contract for the later dual-driver work.

`galaxy-model-builder-design.md` is a longer-term design proposal for a gate-driven system that helps build new Mimic model packages from papers. It is useful now because it clarifies the future pressure on the module ABI, generated-code contracts, science gates, deterministic stochastic physics, and validation reports. It is not yet an implementation plan. It must be revised after the dual-driver work is complete and after the science-gate problem has been grounded in working code.

---

## Snapshot Input Contract

The snapshot driver is not responsible for repairing skipped halo links. Snapshot-ordered inputs are produced by an external converter. That converter must emit a temporally complete snapshot sequence for the snapshot driver, inserting phantom or bridge halos where needed, following the standard approach used by tools such as Consistent Trees. Mimic should validate the declared ordering and enough link metadata to fail fast on obvious mismatches, but the driver may assume adjacent-snapshot continuity after conversion.

This keeps the driver coherent: the tree driver owns vertical tree traversal; the converter owns ordering conversion and phantom-halo insertion; the snapshot driver owns snapshot-synchronous processing over an already valid snapshot-ordered input.

---

## Baseline Contract

The repository contains a shared, physics-free baseline mechanism in `tests/data/output/baseline/` (core tracking and catalog fields, model-agnostic). For dual-driver migration, the v1.0 baseline must also catch behaviour drift in SAGE baryonic output. As of 2026-06-04 that baryonic coverage exists for SAGE as a model-owned full-physics regression (`models/sage/modules/_tests/test_scientific_sage_physics_baseline.py` comparing all core and galaxy properties), with a local full-run byte-identity gate alongside it. At v1.0 these should be refreshed against the optimised build and treated as the acceptance reference; the migration plan should not rely on the narrower physics-free comparison alone.

When a phase claims byte-identical output, the default expectation is exact identity against the v1.0 baseline. If a phase genuinely requires a numeric tolerance because of a science-neutral floating-point reordering, the tolerance must be documented with a specific justification and reviewed explicitly.

---

## Standing Constraints

The physics-module ABI is a stability boundary. The dual-driver work changes how halos are gathered, inherited, ordered, and buffered; it must not casually change how ordinary FoF-scoped modules are called.

Snapshot-global operations are a real future capability, not just a small implementation detail. The snapshot driver makes global SHAM, HOD, radiation-field, environment, and lightcone workflows expressible, but a production per-snapshot collective module contract needs its own design and validation. Cross-format identity should be proven first with ordinary FoF-scoped physics.

Generated metadata remains the structural source of truth. Any driver-neutral output work must remove tree-index assumptions from generated output paths rather than papering over them in one driver.

The model builder must inherit these constraints. In particular, stochastic modules must use deterministic per-halo or per-FoF seeds, not traversal-order RNG streams, or they will break cross-format identity.

The model builder should also inherit the named substep phase contract (now implemented). It should map paper processes onto physically named middle phases under `modules.phases:`, and it should treat the within-phase dispatch rule (full-halo/event work precedes galaxy-local work) as part of the Mimic module contract. The legacy `phase_1`/`phase_2` form no longer exists.
