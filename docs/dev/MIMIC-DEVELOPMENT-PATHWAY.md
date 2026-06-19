# Mimic Development Pathway

**Status:** Active planning index for `docs/dev/`.
**Date:** 2026-06-19
**Scope:** Defines the current release position, active planning documents, source-of-truth boundaries, and near-term sequence for Mimic v1.0 and the first post-v1.0 architecture choices.

---

## Purpose

This document is the entry point for the development plans in `docs/dev/`. It records what should be worked on next, which document owns each plan, and which assumptions must hold before a plan becomes actionable. It deliberately avoids historical implementation detail unless that detail constrains future work.

The direction remains consistent with `docs/VISION.md`: Mimic is a physics-agnostic galaxy evolution framework with runtime-configurable physics modules. The core now has the main v1.0 architecture intended by the dual-driver pre-work: a shared physics-execution engine, a format-neutral inheritance service, a driver-neutral output-buffer path, a reader registry, a reader-provided partition/unit model, metadata-driven catalog units, and enough test and documentation infrastructure to make release decisions mechanically defensible.

The pre-v1.0 optimisation and review sweep that was previously listed here as the next blocker is complete. The remaining v1.0 work is much narrower: complete the real-data Consistent-Trees/Uchuu validation in `CTREES-UCHUU-VALIDATION.md`, then run final generated-code, documentation, format/lint, and test gates before tagging. Phase 0 of the dual-driver plan is still optional; its value is reassessed below in light of the reader-registry work that has already landed.

---

## Active Planning Documents

| Document | Status | Role | Becomes actionable when |
|---|---|---|---|
| `MIMIC-DEVELOPMENT-PATHWAY.md` | Active | Planning index and release sequence | Now |
| `CTREES-UCHUU-VALIDATION.md` | Active v1.0 release checklist | Real-data validation checklist for the Consistent-Trees ASCII and forests-HDF5 readers, especially Uchuu packaging and unit/topology conventions | Now; this is the current v1.0 blocker |
| `MIMIC-DUAL-DRIVER-PLAN.md` | Partially active | Architecture and phased migration for tree-ordered and snapshot-ordered drivers, plus a physics-only embedded engine | Phases 1–3 are DONE. Phase 0 is optional before v1.0 and should be small if taken. Phases 4–7 begin only after v1.0 is tagged and its baseline is refreshed |
| `MIMIC-MODEL-BUILDER-PLAN.md` | Aspirational planning brief | Long-term requirements for assisted, gate-driven model construction | Post-v1.0 and after a working science-gate prototype exists; builds on the v1.0 core seams and does not strictly require the snapshot driver |

Archived predecessor documents are retained under `archive/dev-plans/` for traceability, but the active planning package is the table above.

---

## Current Release Position

The v1.0 architecture and quality sweep are now substantially complete. The commits from `f4c0b86` through `HEAD` include the work that this pathway previously described as open: code formatting and style enforcement, HDF5 writer buffering and compression support, full-Millennium package/test applicability work, galaxy-pool memory scaling, dynamic `ProcessedHalos` growth, source/model/tests/plot/scripts review batches, SAGE parity and baseline refreshes, framework-first documentation cleanup, metadata-driven catalog unit contracts, reader registry and reader-provided partition/unit dispatch, and wired Consistent-Trees ASCII and forests-HDF5 readers.

The important architectural outcome is that Mimic is no longer just a single hard-coded L-Halo file loop. It still has only the tree-ordered processing driver, but that driver now consumes reader-declared partition models:

- `PARTITION_PER_FILE` for L-Halo binary/HDF5 inputs, where one output partition corresponds to one input file and a unit is one tree.
- `PARTITION_PER_TASK` for Consistent-Trees ASCII/HDF5 inputs, where one output partition corresponds to one MPI task and a unit is one forest.

That reader generalisation was not Phase 0 by itself. It removed much of the old file-loop coupling, but it did not add an explicit top-level driver dispatcher, an input-ordering field, or validation that maps ordering to driver. Phase 0 now adds that seam with `input.processing_order`.

---

## Intended Sequence

1. **Keep the completed v1.0 sweep closed.** Treat the optimisation/review sweep as done, not as a continuing umbrella for unrelated cleanup. New fixes before v1.0 should be release blockers, validation fallout, or narrowly scoped polish from final gates.

2. **Complete `CTREES-UCHUU-VALIDATION.md`.** This is the current release blocker. The repository has the readers and unit/helper coverage; the remaining evidence is an end-to-end run against real Consistent-Trees data, checking dataset packaging, units, topology, MPI partitioning, HDF5 forest distribution, and ASCII/HDF5 parity where both forms are available.

3. **Run final release hygiene.** After the real-data reader validation is clean, run the generated-code, metadata, docs, format/lint, and relevant test gates. At minimum this means `make check-generated`, `make validate-modules`, `make check-docs`, `make check-format`, and the standard test tiers in summary/logged form. Any non-zero exit code is a release blocker.

4. **Decide whether to take Phase 0 before the tag.** Phase 0 is now a small dispatcher/validation seam rather than a broad lifecycle extraction. It is still snapshot-driver-anticipatory and should not gate v1.0. If taken before v1.0, it should be one behaviour-preserving commit that extracts the existing tree loop into `run_tree_driver()`, adds `input.processing_order` with default `tree_ordered`, rejects unsupported ordering/reader combinations, and keeps all existing tree outputs byte-identical. If that cannot stay small, defer it.

5. **Tag v1.0 and refresh the release baseline.** After validation and final gates, tag v1.0 and record the tagged baseline as the forward reference for behaviour-preserving work. From that point, the tagged-v1.0 baseline protects later Phase 0/4–7 work if Phase 0 was deferred, and all snapshot-driver work if Phase 0 was included.

6. **Choose the next major direction post-v1.0.** The snapshot driver and the model builder are now a real choice. The snapshot driver builds on the shared execution/inheritance/output seams plus the reader/partition generalisation; the model builder builds on the same stable module interfaces, generated metadata, deterministic physics contracts, and validation gates. Scientific priority should decide the order.

7. **Review `docs/VISION.md` only after new behaviour exists.** The current v1.0 work remains consistent with the existing vision. Do not pre-emptively edit the vision for dual-driver or model-builder behaviour. Once the snapshot driver passes its identity gate, review the vision narrowly for per-driver memory bounds, determinism as an invariant, and a pointer to the implemented dual-driver architecture.

---

## Phase 0 Assessment

Phase 0 in `MIMIC-DUAL-DRIVER-PLAN.md` originally meant adding an input-ordering selector, a driver dispatcher, and a tree-driver wrapper while only the tree driver existed. The current codebase already did part of the adjacent work for another reason: `src/io/tree/reader.h` defines a reader registry and partition model, `src/io/tree/registry.c` resolves `input.tree_type`, and `src/core/main.c` dispatches between reader-owned partition strategies.

The remaining Phase 0 work is therefore:

- Add an explicit input ordering concept, implemented as `input.processing_order`, defaulting to `tree_ordered`.
- Extract the current processing loop in `main.c` into `run_tree_driver()` without changing its behaviour.
- Add a small dispatcher that selects `run_tree_driver()` for `tree_ordered` and fails fast for `snapshot_ordered` until that driver exists.
- Validate that every registered current reader is tree-ordered; do not infer snapshot capability from `tree_type`.
- Preserve existing output naming, skip semantics, MPI partitioning, HDF5 master-file behaviour, metadata writing, and final cleanup order.

The value is mostly architectural clarity and future error quality. It would make the release say, explicitly, "Mimic v1.0 has one supported driver, tree-ordered, selected through a dispatcher that is ready for a second driver." It would also prevent a future snapshot-reader branch from mixing ordering selection into `tree_type` or further overloading the reader partition model.

The cost is release risk, not implementation size. Even a clean extraction touches `main.c`, run configuration parsing, and user-facing YAML validation. That is exactly the code path every release run exercises, and v1.0 is now close enough that a nonessential seam should earn its place.

**Recommendation:** do not take Phase 0 before the Consistent-Trees/Uchuu validation. After that validation is clean, Phase 0 is reasonable only if it stays surgical and behaviour-preserving. If there is any pressure to tag, defer Phase 0 and make it the first post-v1.0 commit, because the reader registry and partition model already removed the most urgent coupling.

---

## Baseline Contract

The repository has a shared regression-baseline mechanism, including the SAGE full-physics regression (`test_scientific_sage_physics_baseline.py`) and local output-format baselines. Before v1.0, use the current release-candidate baseline to catch accidental drift from final validation fixes and optional Phase 0 work. After v1.0, refresh or record the tagged-v1.0 baseline as the forward reference.

For any behaviour-preserving release-candidate change, "same output" means exact identity unless a documented and reviewed numeric tolerance is explicitly accepted. Silent tolerance is a failed gate. For Phase 0 specifically, no output drift is expected: it is only a dispatcher/extraction seam over the existing tree driver.

The baseline must continue to protect SAGE baryonic output, not just physics-free core/catalog fields, because inheritance, merger handling, and module execution are the behaviours most likely to reveal driver-seam mistakes.

---

## Standing Constraints

- **Release-blocker discipline:** before v1.0, do not reopen the completed sweep as a general cleanup bucket. Fix validation failures and final-gate issues; defer opportunistic refactors.
- **The physics-execution, inheritance, output-buffer, and reader/partition seams are shared foundation, not dual-driver-only.** Keep them format-neutral and free of tree-index, traversal-order, and driver-specific assumptions unless the file is explicitly tree-driver code.
- **Phase 0 remains optional for v1.0.** If taken, it must be behaviour-preserving, small, and baseline-gated. It must not add snapshot input support, snapshot-global hooks, or reader model churn.
- **The physics-module ABI is a stability boundary.** The dual-driver work may change how halos are gathered, inherited, ordered, buffered, and written, but it must not casually change how ordinary FoF-scoped modules are called.
- **Generated metadata remains the structural source of truth.** Driver-neutral output and reader-boundary unit conversion should keep using generated property metadata rather than hand-maintained duplicate structures.
- **Snapshot input conversion is external.** Mimic will not repair skipped halo links or insert phantom/bridge halos internally. The future snapshot driver may assume the converter has produced a temporally complete adjacent-snapshot representation after startup validation passes.
- **Snapshot-global operations are follow-on work.** The first snapshot-driver acceptance target is cross-format identity for ordinary FoF-scoped physics, with snapshot-global physics disabled.
- **Determinism is required for cross-format identity.** Future stochastic modules must seed from stable per-halo or per-FoF keys, not from traversal-order RNG streams.
- **The model builder inherits these constraints.** It should not push Mimic toward unstable module interfaces, ad hoc metadata, traversal-order stochasticity, or validation claims that cannot be mechanically defended.
