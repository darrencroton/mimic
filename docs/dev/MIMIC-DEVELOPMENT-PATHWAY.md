# Mimic Development Pathway

**Status:** Active planning index for `docs/dev/`.
**Date:** 2026-06-22
**Scope:** Defines the current release position, active planning documents, source-of-truth boundaries, and near-term sequence for Mimic v1.0 and the first post-v1.0 architecture choices.

---

## Purpose

This document is the entry point for the development plans in `docs/dev/`. It records what should be worked on next, which document owns each plan, and which assumptions must hold before a plan becomes actionable. It deliberately avoids historical implementation detail unless that detail constrains future work.

The direction remains consistent with `docs/VISION.md`: Mimic is a physics-agnostic galaxy evolution framework with runtime-configurable physics modules. The core now has the main v1.0 architecture intended by the dual-driver pre-work: a shared physics-execution engine, a format-neutral inheritance service, a driver-neutral output-buffer path, a reader registry, a reader-provided partition/unit model, metadata-driven catalog units, and enough test and documentation infrastructure to make release decisions mechanically defensible.

The pre-v1.0 optimisation and review sweep that was previously listed here as the next blocker is complete. Phase 0 of the dual-driver plan is also complete: Mimic now has an explicit `input.processing_order` selector, a top-level processing-driver dispatcher, the existing tree lifecycle extracted into `run_tree_driver()`, and fail-fast validation for the future `snapshot_ordered` driver. The galaxy-ID encoding redesign is also complete: `UniqueGalaxyID` is now based on run-scoped global forest identity rather than MPI task or file partition identity, and the implementation has passed its full gate. The Uchuu import work is signed off: the micro-Uchuu, mini-Uchuu, and full Uchuu simulation packages have all been imported, run, and validated successfully. The remaining v1.0 work is a focused style sweep before release, followed by the final release gates and tag.

---

## Active Planning Documents

| Document | Status | Role | Becomes actionable when |
|---|---|---|---|
| `MIMIC-DEVELOPMENT-PATHWAY.md` | Active | Planning index and release sequence | Now |
| `MIMIC-DUAL-DRIVER-PLAN.md` | Partially active | Architecture and phased migration for tree-ordered and snapshot-ordered drivers, plus a physics-only embedded engine | Phases 0–3 are DONE for v1.0. Phases 4–7 begin only after v1.0 is tagged and its baseline is refreshed |
| `MIMIC-MODEL-BUILDER-PLAN.md` | Aspirational planning brief | Long-term requirements for assisted, gate-driven model construction | Post-v1.0 and after a working science-gate prototype exists; builds on the v1.0 core seams and does not strictly require the snapshot driver |

Archived predecessor and closeout documents are retained under `archive/dev-plans/` for traceability, but the active planning package is the table above. The completed galaxy-ID implementation plan, galaxy-ID redesign record, Uchuu validation record, and closeout handoff are historical evidence, not active planning inputs.

---

## Current Release Position

The v1.0 architecture and quality sweep are now substantially complete. The commits from `f4c0b86` through `HEAD` include the work that this pathway previously described as open: code formatting and style enforcement, HDF5 writer buffering and compression support, full-Millennium package/test applicability work, galaxy-pool memory scaling, dynamic `ProcessedHalos` growth, source/model/tests/plot/scripts review batches, SAGE parity and baseline refreshes, framework-first documentation cleanup, metadata-driven catalog unit contracts, reader registry and reader-provided partition/unit dispatch, wired Consistent-Trees ASCII and forests-HDF5 readers, and run-scoped global-forest `UniqueGalaxyID` encoding.

The important architectural outcome is that Mimic is no longer just a single hard-coded L-Halo file loop. It still has only one implemented processing driver, but that driver is explicitly selected as `tree_ordered` through `input.processing_order` and now consumes reader-declared partition models:

- `PARTITION_PER_FILE` for L-Halo binary/HDF5 inputs, where one output partition corresponds to one input file and a unit is one tree.
- `PARTITION_PER_TASK` for Consistent-Trees ASCII/HDF5 inputs, where one output partition corresponds to one MPI task and a unit is one forest.

That reader generalisation was not Phase 0 by itself. It removed much of the old file-loop coupling, and Phase 0 has now added the missing input-ordering seam with `input.processing_order`, top-level dispatch through `run_processing_driver()`, and explicit validation that current readers feed the `tree_ordered` driver.

The current release-candidate state is:

- `UniqueGalaxyID` is encoded from `halonr + TREE_MUL_FAC * forestnr_global`, where `forestnr_global` is run-scoped and independent of MPI rank, task count, or input partition number.
- `micro-uchuu`, `mini-uchuu`, and `uchuu` are imported, runnable, and validated. The retained package choices are tier-specific: micro-Uchuu keeps L-Halo binary as the preferred production path plus forests-HDF5 for cross-validation; mini-Uchuu uses L-Halo binary; full Uchuu uses forests-HDF5.
- The historical Consistent-Trees ASCII micro-Uchuu validation remains useful as archived evidence of reader behaviour and the final-snapshot `fix_flybys` topology policy, but it is no longer an active v1.0 work item.
- The next task before v1.0 is a focused style sweep, not another architecture or optimisation pass.

---

## Intended Sequence

1. **Run the pre-release style sweep.** Re-read the current diff and release-critical surfaces against `docs/STYLE-GUIDE.md`, fix local style problems in touched or release-facing files, run `./scripts/beautify.sh`, and keep the sweep narrow. This is the next task before v1.0.

2. **Keep the completed v1.0 sweep closed.** Treat the optimisation/review sweep, galaxy-ID work, and Uchuu import/validation work as done, not as continuing umbrellas for unrelated cleanup. New fixes before v1.0 should be release blockers, style-sweep fallout, or narrowly scoped polish from final gates.

3. **Run final release gates.** After the style sweep, run the generated-code, metadata, docs, format/lint, and relevant test gates one final time in the release environment. At minimum this means `make check-generated`, `make validate-modules`, `make check-docs`, `make check-format`, and the standard test tiers in summary/logged form. Any non-zero exit code is a release blocker.

4. **Keep Phase 0 closed.** Phase 0 landed as a behaviour-preserving dispatcher/validation seam: `input.processing_order` defaults to `tree_ordered`, the existing tree loop is isolated in `run_tree_driver()`, current readers declare tree-driver compatibility, and `snapshot_ordered` fails fast until the later snapshot-driver phases exist. Do not expand Phase 0 into snapshot reader or snapshot driver work before v1.0.

5. **Tag v1.0 and refresh the release baseline.** After validation and final gates, tag v1.0 and record the tagged baseline as the forward reference for behaviour-preserving work. From that point, the tagged-v1.0 baseline protects all later snapshot-reader and snapshot-driver work.

6. **Choose the next major direction post-v1.0.** The snapshot driver and the model builder are now a real choice. The snapshot driver builds on the shared execution/inheritance/output seams plus the reader/partition generalisation; the model builder builds on the same stable module interfaces, generated metadata, deterministic physics contracts, and validation gates. Scientific priority should decide the order.

7. **Review `docs/VISION.md` only after new behaviour exists.** The current v1.0 work remains consistent with the existing vision. Do not pre-emptively edit the vision for dual-driver or model-builder behaviour. Once the snapshot driver passes its identity gate, review the vision narrowly for per-driver memory bounds, determinism as an invariant, and a pointer to the implemented dual-driver architecture.

---

## Phase 0 Status

Phase 0 in `MIMIC-DUAL-DRIVER-PLAN.md` meant adding an input-ordering selector, a driver dispatcher, and a tree-driver wrapper while only the tree driver existed. That work is complete and stays deliberately limited to the Phase 0 seam:

- `input.processing_order` is the explicit processing-order selector and defaults to `tree_ordered`, so existing run files remain valid.
- `input.tree_type` remains the reader-format selector and is not overloaded with processing-order meaning.
- `run_tree_driver()` owns the existing tree-ordered lifecycle, and `run_processing_driver()` dispatches to it.
- `snapshot_ordered` is recognized as a future processing order but fails fast because the snapshot driver is not implemented yet.
- Current readers declare `INPUT_PROCESSING_ORDER_TREE` compatibility, preserving the existing L-Halo and Consistent-Trees partition semantics.

The implementation passed drift audit, code review after fixing the one redundant-error finding, full `make tests`, and a regular `sage16` + `mini-millennium` run. Treat Phase 0 as closed for v1.0; the next dual-driver work is Phases 4–7 after the v1.0 tag and tagged-baseline refresh.

## Completed Closeouts

The following work is signed off and archived under `archive/dev-plans/`:

- Galaxy-ID encoding implementation plan and redesign record: the active code uses run-scoped global forest identity, and the old partition-term ID scheme is retired.
- Uchuu validation record: micro-Uchuu, mini-Uchuu, and full Uchuu have all been imported, run, and validated. The archived record remains useful for dataset provenance, format notes, and historical Consistent-Trees ASCII comparison details.
- Galaxy-ID handoff: the implementation handoff records the completed slice sequence, validation commands, and review outcomes.

Do not reopen these closeouts as active v1.0 planning documents. If future work needs a detail from them, cite the archived record and create a new narrowly scoped active plan.

---

## Baseline Contract

The repository has a shared regression-baseline mechanism, including the SAGE full-physics regression (`test_scientific_sage_physics_baseline.py`) and local output-format baselines. Before v1.0, use the current release-candidate baseline to catch accidental drift from final validation fixes and optional Phase 0 work. After v1.0, refresh or record the tagged-v1.0 baseline as the forward reference.

For any behaviour-preserving release-candidate change, "same output" means exact identity unless a documented and reviewed numeric tolerance is explicitly accepted. Silent tolerance is a failed gate. For Phase 0 specifically, no output drift is expected: it is only a dispatcher/extraction seam over the existing tree driver.

The baseline must continue to protect SAGE baryonic output, not just physics-free core/catalog fields, because inheritance, merger handling, and module execution are the behaviours most likely to reveal driver-seam mistakes.

---

## Standing Constraints

- **Release-blocker discipline:** before v1.0, do not reopen the completed sweep as a general cleanup bucket. Fix validation failures and final-gate issues; defer opportunistic refactors.
- **The physics-execution, inheritance, output-buffer, and reader/partition seams are shared foundation, not dual-driver-only.** Keep them format-neutral and free of tree-index, traversal-order, and driver-specific assumptions unless the file is explicitly tree-driver code.
- **Phase 0 is closed for v1.0.** Do not expand it with snapshot input support, snapshot-global hooks, or reader model churn. Those belong to later dual-driver phases.
- **The physics-module ABI is a stability boundary.** The dual-driver work may change how halos are gathered, inherited, ordered, buffered, and written, but it must not casually change how ordinary FoF-scoped modules are called.
- **Generated metadata remains the structural source of truth.** Driver-neutral output and reader-boundary unit conversion should keep using generated property metadata rather than hand-maintained duplicate structures.
- **Snapshot input conversion is external.** Mimic will not repair skipped halo links or insert phantom/bridge halos internally. The future snapshot driver may assume the converter has produced a temporally complete adjacent-snapshot representation after startup validation passes.
- **Snapshot-global operations are follow-on work.** The first snapshot-driver acceptance target is cross-format identity for ordinary FoF-scoped physics, with snapshot-global physics disabled.
- **Determinism is required for cross-format identity.** Future stochastic modules must seed from stable per-halo or per-FoF keys, not from traversal-order RNG streams.
- **The model builder inherits these constraints.** It should not push Mimic toward unstable module interfaces, ad hoc metadata, traversal-order stochasticity, or validation claims that cannot be mechanically defended.
