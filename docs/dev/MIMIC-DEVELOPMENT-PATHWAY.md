# Mimic Development Pathway

**Status:** Active planning index for `docs/dev/`.
**Date:** 2026-06-05
**Scope:** Defines the forward sequence, active planning documents, source-of-truth boundaries, and standing constraints for the next major Mimic work.

---

## Purpose

This document is the entry point for the development plans in `docs/dev/`. It records what should be worked on next, which document owns each plan, and which assumptions must hold before a plan becomes actionable. It deliberately avoids historical implementation detail unless that detail constrains future work.

The direction remains consistent with `docs/VISION.md`: Mimic is a physics-agnostic galaxy evolution framework with runtime-configurable physics modules. The next architectural extension is to support both tree-ordered and snapshot-ordered front ends over shared core services. The later model-builder proposal is downstream of that work and should not drive near-term architecture except where it reinforces already-important contracts such as stable module interfaces, generated metadata, deterministic stochastic physics, and strong validation gates.

---

## Active Planning Documents

| Document | Status | Role | Becomes actionable when |
|---|---|---|---|
| `MIMIC-DEVELOPMENT-PATHWAY.md` | Active | Planning index and sequence | Now |
| `MIMIC-DUAL-DRIVER-PLAN.md` | Proposed implementation plan | Architecture and phased migration for tree-ordered and snapshot-ordered drivers, plus a physics-only embedded engine | Mimic v1.0 is tagged and the v1.0 baseline is refreshed |
| `MIMIC-MODEL-BUILDER-PLAN.md` | Aspirational planning brief | Long-term requirements for assisted, gate-driven model construction | Post-v1.0, post-dual-driver, and after a working science-gate prototype exists |

Archived predecessor documents are retained under `archive/dev-plans/` for traceability, but the active planning package is the table above.

---

## Intended Sequence

1. **Finish Mimic v1.0 preparation.** Complete the remaining core optimisation, memory behaviour, HDF5 writer performance, benchmark freeze, documentation cleanup, and lint/format pass. This work should be done against the current module pipeline and output provenance schema, not mixed with the dual-driver migration.

2. **Refresh the v1.0 baseline.** Treat the released v1.0 output as the migration reference. The baseline must cover the existing model-agnostic core/catalog output and SAGE baryonic output strongly enough to catch behaviour drift during the dual-driver extraction.

3. **Implement the dual-driver plan.** Follow `MIMIC-DUAL-DRIVER-PLAN.md` after v1.0 is tagged. The early migration phases are intentionally behaviour-preserving for the existing tree-ordered run. Do not introduce the snapshot driver until the shared inheritance service, physics execution boundary, and driver-neutral output path are proven against the v1.0 baseline.

4. **Prove the snapshot driver.** The snapshot driver is accepted only when equivalent tree-ordered and snapshot-ordered inputs produce identical galaxies with snapshot-global physics disabled for the comparison. If exact identity becomes genuinely impossible because of a documented, science-neutral floating-point reordering, the tolerance and rationale must be explicit and reviewed.

5. **Review `docs/VISION.md` only after the behaviour exists.** Do not pre-emptively edit the vision for dual-driver behaviour. Once the snapshot driver passes its identity gate, review the vision narrowly for per-driver memory bounds, determinism as an invariant, and a pointer to the dual-driver architecture.

6. **Rework the model-builder plan.** The model-builder idea becomes an implementation plan only after the dual-driver architecture exists and a science-gate prototype has been grounded in working code. Until then it is a requirements brief for future planning, not a mandate for current implementation.

---

## Baseline Contract

The repository already has a shared regression-baseline mechanism under `tests/data/output/baseline/`. For the dual-driver migration, the v1.0 baseline must also protect SAGE baryonic output, not just physics-free core/catalog fields. The current SAGE full-physics regression and local byte-identity gate are useful foundations, but the migration reference should be refreshed against the final v1.0 build.

When a dual-driver migration phase claims byte-identical output, that means exact identity against the v1.0 reference unless a documented and reviewed numeric tolerance is explicitly accepted. A silent tolerance is a failed gate.

---

## Standing Constraints

- **The physics-module ABI is a stability boundary.** The dual-driver work may change how halos are gathered, inherited, ordered, buffered, and written, but it must not casually change how ordinary FoF-scoped modules are called.
- **Generated metadata remains the structural source of truth.** Driver-neutral output work must remove tree-index assumptions from generated output paths instead of papering over them in one driver.
- **Snapshot input conversion is external.** Mimic will not repair skipped halo links or insert phantom/bridge halos internally. The snapshot driver may assume the converter has produced a temporally complete adjacent-snapshot representation after startup validation passes.
- **Snapshot-global operations are follow-on work.** The snapshot driver makes global SHAM, HOD, environment, radiation-field, and lightcone workflows expressible, but the first acceptance target is cross-format identity for ordinary FoF-scoped physics.
- **Determinism is required for cross-format identity.** Future stochastic modules must seed from stable per-halo or per-FoF keys, not from traversal-order RNG streams.
- **The model builder inherits these constraints.** It should not push Mimic toward unstable module interfaces, ad hoc metadata, traversal-order stochasticity, or validation claims that cannot be mechanically defended.
