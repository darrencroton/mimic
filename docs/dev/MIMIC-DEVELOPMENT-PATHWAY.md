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

1. **Finish Mimic v1.0 preparation.** Complete the remaining core optimisation work, especially memory behaviour and HDF5 writer performance. This work is outside the dual-driver plan and should not be mixed with it.
2. **Tag v1.0 and establish the golden SAGE baseline.** The existing baseline mechanism under `tests/data/output/baseline/` is the starting point. At v1.0, refresh or extend the baseline so it supports the dual-driver migration gates, including byte-identical regression coverage for the relevant baryonic properties as well as core halo properties.
3. **Implement the dual-driver architecture and change map.** The dual-driver documents become the actionable implementation plan only after v1.0 is tagged and the golden baseline exists. Their migration phases are intentionally behaviour-preserving until the snapshot driver is introduced.
4. **Prove the snapshot driver.** The snapshot driver is accepted only when it passes the cross-format identity gate on equivalent converted inputs, with global-only snapshot physics disabled for that comparison.
5. **Review and amend `docs/VISION.md`.** Once the snapshot driver works and passes identity tests, update the vision narrowly: per-driver memory bounds, determinism as an invariant, and a pointer to the dual-driver architecture.
6. **Rework the galaxy model builder proposal.** The model builder design is currently aspirational. It should be reviewed and rewritten against the post-v1.0, post-dual-driver codebase before it becomes an exact work plan or source of truth.

---

## How To Read The Dev Docs

`MIMIC-DUAL-DRIVER-ARCHITECTURE.md` is the architectural target for adding tree-ordered and snapshot-ordered front-ends over one shared inheritance and physics execution core. It extends the current vision by generalising the processing and memory model, but it should not be treated as implemented behaviour until the change map has landed and passed its gates.

`MIMIC-DUAL-DRIVER-CHANGE-MAP.md` is the phased migration plan for the dual-driver work. Its gates assume a tagged v1.0 baseline. Where it discusses byte identity, read that as a v1.0 migration requirement, not a claim that the current pre-v1 integration baseline already provides every needed comparison.

`galaxy-model-builder-design.md` is a longer-term design proposal for a gate-driven system that helps build new Mimic model packages from papers. It is useful now because it clarifies the future pressure on the module ABI, generated-code contracts, science gates, deterministic stochastic physics, and validation reports. It is not yet an implementation plan. It must be revised after the dual-driver work is complete and after the science-gate problem has been grounded in working code.

---

## Snapshot Input Contract

The snapshot driver is not responsible for repairing skipped halo links. Snapshot-ordered inputs are produced by an external converter. That converter must emit a temporally complete snapshot sequence for the snapshot driver, inserting phantom or bridge halos where needed, following the standard approach used by tools such as Consistent Trees. Mimic should validate the declared ordering and enough link metadata to fail fast on obvious mismatches, but the driver may assume adjacent-snapshot continuity after conversion.

This keeps the driver coherent: the tree driver owns vertical tree traversal; the converter owns ordering conversion and phantom-halo insertion; the snapshot driver owns snapshot-synchronous processing over an already valid snapshot-ordered input.

---

## Baseline Contract

The current repository already contains a useful baseline mechanism in `tests/data/output/baseline/`. For dual-driver migration, the v1.0 baseline must be strong enough to catch behaviour drift in both core tracking and SAGE baryonic output. The existing mechanism can serve this role if refreshed and extended at v1.0, but the migration plan should not rely on the narrower pre-v1 comparison as the final acceptance gate.

When a phase claims byte-identical output, the default expectation is exact identity against the v1.0 baseline. If a phase genuinely requires a numeric tolerance because of a science-neutral floating-point reordering, the tolerance must be documented with a specific justification and reviewed explicitly.

---

## Standing Constraints

The physics-module ABI is a stability boundary. The dual-driver work changes how halos are gathered, inherited, ordered, and buffered; it must not casually change how ordinary FoF-scoped modules are called.

Snapshot-global operations are a real future capability, not just a small implementation detail. The snapshot driver makes global SHAM, HOD, radiation-field, environment, and lightcone workflows expressible, but a production per-snapshot collective module contract needs its own design and validation. Cross-format identity should be proven first with ordinary FoF-scoped physics.

Generated metadata remains the structural source of truth. Any driver-neutral output work must remove tree-index assumptions from generated output paths rather than papering over them in one driver.

The model builder must inherit these constraints. In particular, stochastic modules must use deterministic per-halo or per-FoF seeds, not traversal-order RNG streams, or they will break cross-format identity.
