# Mimic Development Pathway

**Status:** Active planning index for `docs/dev/`.
**Date:** 2026-06-05
**Scope:** Defines the forward sequence, active planning documents, source-of-truth boundaries, and standing constraints for the next major Mimic work.

---

## Purpose

This document is the entry point for the development plans in `docs/dev/`. It records what should be worked on next, which document owns each plan, and which assumptions must hold before a plan becomes actionable. It deliberately avoids historical implementation detail unless that detail constrains future work.

The direction remains consistent with `docs/VISION.md`: Mimic is a physics-agnostic galaxy evolution framework with runtime-configurable physics modules. The core modularisation into a clean physics-execution engine, a shared inheritance service, and a driver-neutral output path is now complete as dual-driver plan Phases 1–3. That work is **no longer treated as post-v1.0 dual-driver work**: it is core architecture that belongs *in* v1.0 and underpins every candidate forward direction. Two such directions are on the table — a snapshot-ordered second driver (the rest of the dual-driver plan) and the model builder — and they are now largely independent: both build on the same Phase 1–3 core seams, and nothing in the model builder requires the snapshot driver. The model-builder proposal should still not drive near-term architecture except where it reinforces already-important contracts such as stable module interfaces, generated metadata, deterministic stochastic physics, and strong validation gates.

This document was revised on 2026-06-05 to pull the core-modularisation phases into v1.0. The earlier ordering (tag v1.0 on the current architecture first, then do all dual-driver work) is superseded; the full reasoning is recorded under "Why core modularisation moves into v1.0" below so it does not have to be re-argued.

---

## Active Planning Documents

| Document | Status | Role | Becomes actionable when |
|---|---|---|---|
| `MIMIC-DEVELOPMENT-PATHWAY.md` | Active | Planning index and sequence | Now |
| `MIMIC-DUAL-DRIVER-PLAN.md` | Partially active | Architecture and phased migration for tree-ordered and snapshot-ordered drivers, plus a physics-only embedded engine | **Phases 1–3 DONE** (behaviour-preserving; Phase 1 extracted physics execution, Phase 2 extracted `src/core/inheritance.c`, Phase 3 extracted `src/core/output_buffer.c`). The pre-v1.0 optimisation/review sweep is next. Phases 4–7 (snapshot reader and driver): after v1.0 is tagged and its baseline refreshed |
| `MIMIC-MODEL-BUILDER-PLAN.md` | Aspirational planning brief | Long-term requirements for assisted, gate-driven model construction | Post-v1.0 and after a working science-gate prototype exists; builds on the Phase 1–3 core seams delivered in v1.0 and does **not** strictly require the snapshot driver |

Archived predecessor documents are retained under `archive/dev-plans/` for traceability, but the active planning package is the table above.

---

## Intended Sequence

This sequence supersedes the earlier "tag v1.0 on the current architecture, then do all dual-driver work afterward" ordering. The reasoning is in the next section ("Why core modularisation moves into v1.0").

1. **Complete the core modularisation (dual-driver Phases 1–3) as part of v1.0 — DONE.** These phases separated physics execution from output marshalling (Phase 1), extracted the format-neutral inheritance service (Phase 2, now `src/core/inheritance.c`), and made output buffering driver-neutral (Phase 3, now `src/core/output_buffer.c`). They are behaviour-preserving core refactors, not snapshot-driver features. Phase 0 (the driver-dispatch seam) remains the one genuinely snapshot-driver-anticipatory step in this group; defer it unless it becomes necessary, and do not let it gate v1.0.

2. **Run the final v1.0 review and optimisation sweep over the restructured core.** This is the current release blocker: a full code review plus the remaining memory, large-simulation, and HDF5-writer optimisation, benchmark freeze, bug hunt, simplification, documentation cleanup, and lint/format pass. Running it *after* Phases 1–3 is deliberate — the sweep then hardens and optimises the *final* architecture instead of polishing a fused `build_model.c` that the extraction would immediately demolish, and the riskiest extraction (Phase 2) is reviewed under maximum scrutiny while the baseline still guards it.

   **Sequence, do not blend.** Each modularisation phase in step 1 lands as its own behaviour-preserving, baseline-gated commit *before* the sweep begins. The sweep is the only place where deliberate behaviour changes (real bug fixes, output-affecting optimisation) are allowed, and they are reviewed. Keeping the two activities sequential preserves clean drift attribution: a refactor phase says "I changed nothing observable", the sweep says "here are the intended changes". Blending extraction and optimisation into one undifferentiated pass loses that and makes any output drift impossible to attribute to a cause.

3. **Tag v1.0 and fix the tagged baseline as the forward reference.** After the sweep, refresh the regression baseline against the tagged build so it reflects any reviewed behaviour changes the sweep introduced. The baseline must cover the existing model-agnostic core/catalog output and SAGE baryonic output strongly enough to catch behaviour drift during later extraction work. From this point the tagged-v1.0 baseline — not the pre-sweep baseline — is the reference that protects all later work, including the snapshot driver.

4. **Choose the next major direction; it is now a real choice.** Because Phases 1–3 are already in v1.0, the snapshot driver (dual-driver Phases 4–7) and the model builder are no longer strictly ordered. Nothing in the model builder requires the snapshot driver; both build on the same Phase 1–3 execution/inheritance/output seams delivered in v1.0. The default remains the snapshot driver next, but starting the model builder first is a legitimate option, to be decided on scientific priority at the time rather than forced by this document.

5. **Implement and prove the snapshot driver (dual-driver Phases 4–7) when chosen.** Add the snapshot-grouped reader and the snapshot driver over the shared services already in v1.0, gated against the refreshed tagged-v1.0 baseline. The snapshot driver is accepted only when equivalent tree-ordered and snapshot-ordered inputs produce identical galaxies with snapshot-global physics disabled for the comparison. If exact identity becomes genuinely impossible because of a documented, science-neutral floating-point reordering, the tolerance and rationale must be explicit and reviewed.

6. **Review `docs/VISION.md` only after new behaviour exists.** Do not pre-emptively edit the vision for dual-driver or model-builder behaviour. The Phase 1–3 modularisation in v1.0 is behaviour-preserving and needs no vision change. Once the snapshot driver passes its identity gate, review the vision narrowly for per-driver memory bounds, determinism as an invariant, and a pointer to the dual-driver architecture.

7. **Rework the model-builder plan into an implementation plan** only after a science-gate prototype has been grounded in working code. Until then it is a requirements brief, not a mandate. Its hard dependency is the Phase 1–3 core seam (delivered in v1.0), not the snapshot driver.

---

## Why Core Modularisation Moves Into v1.0

This section records the full reasoning for pulling dual-driver Phases 1–3 into v1.0, so the decision does not have to be re-argued. The earlier plan tagged v1.0 on the current architecture and did all dual-driver work afterward. Three facts changed that:

1. **The migration baseline already exists.** The original ordering's main safety argument was that the highest-risk refactor — Phase 2, extracting the inheritance service — needed a known-good v1.0 baseline to validate against, and that baseline did not yet exist. It now does, as the SAGE full-physics regression baseline (`test_scientific_sage_physics_baseline.py`). Phase 2 can therefore be validated for byte-identical behaviour today, just as well as it could after a tag. The baseline also persists across the tag, so it protects these phases whether they run before or after v1.0 — meaning there is no *correctness* reason to defer them.

2. **Phases 1–3 are core modularisation, not snapshot-driver features.** They separate physics execution from output marshalling (Phase 1), extract the inheritance science out of tree-index-coupled code into a shared service (Phase 2), and remove tree-index assumptions from the output path (Phase 3). The result is a cleaner, more legible core regardless of whether a second driver is ever added. Only Phase 0 (the dispatcher seam) and Phases 4+ (the snapshot reader and driver) are genuinely dual-driver-specific.

3. **The Phase 1–2 seam is the shared foundation of every forward direction.** The dual-driver plan's own embedded-engine phase (Phase 6) depends only on the Phase 1–2 seam, and a model builder that assembles and tests models programmatically leans on exactly that physics-execution engine and inheritance service. Because the model builder may even precede the snapshot driver, building Phases 1–2 is not a bet on snapshot processing — it is infrastructure both futures need. That makes including them in v1.0 a low-regret move.

Given the goal of releasing the best, most complete core as v1.0 — and given that v1.0 is already gated on a quality sweep — the decisive argument is that the single, high-value review-and-optimisation sweep is far more valuable applied to the *final* architecture than to a structure about to be demolished. Doing the sweep first and then immediately extracting the inheritance service would waste the sweep's scrutiny on the seams that change most, and would force a second review pass later to re-bless the extracted structure.

**The cost is real and is accepted deliberately.** This places the project's highest-risk refactor (Phase 2) on the release critical path. The baseline protects *correctness* — it will catch any behaviour drift — but it does **not** protect *schedule*: Phase 2 can still surface subtle inheritance coupling that takes time to unpick, and that time now sits between us and the v1.0 tag. This trade is chosen because the stated priority is the quality and completeness of v1.0, not the earliest possible tag. If release timing later becomes the priority, the defensible fallback is the original ordering — tag v1.0 on the current architecture after the sweep, and do Phases 1–3 afterward against the tagged baseline with their own gates. Choosing that fallback is a schedule decision, not a correctness one.

---

## Baseline Contract

The repository already has a shared regression-baseline mechanism, including the SAGE full-physics regression (`test_scientific_sage_physics_baseline.py`) and a local byte-identity gate. This is the known-good reference that makes pulling the modularisation work into v1.0 safe.

There are two baselines in time, and the distinction matters:

- **Pre-sweep baseline (now):** the current SAGE physics baseline guards the Phase 1–3 modularisation. Each of those phases must be byte-identical against it. Because the phases are behaviour-preserving, this baseline should **not** move while they land — any drift is a bug in the extraction, not an intended change.
- **Tagged-v1.0 baseline (after the sweep):** the optimisation-and-review sweep may legitimately change output (real bug fixes, output-affecting optimisation), each change reviewed. After the sweep and tag, refresh the baseline against the tagged build. From then on, the tagged-v1.0 baseline is the reference that protects the snapshot-driver work (Phases 4–7).

The baseline must protect SAGE baryonic output, not just physics-free core/catalog fields, so it can catch behaviour drift during the inheritance extraction. When any behaviour-preserving phase claims byte-identical output, that means exact identity against the applicable baseline unless a documented and reviewed numeric tolerance is explicitly accepted. A silent tolerance is a failed gate.

---

## Standing Constraints

- **Behaviour-preserving refactors and the optimisation sweep stay sequential, never blended.** The Phase 1–3 modularisation landed as behaviour-preserving, individually gated work before the v1.0 sweep begins. Mixing structural extraction with deliberate optimisation in one pass destroys drift attribution and is not allowed. This is the operational rule that lets core modularisation live inside v1.0 without putting the release baseline at risk.
- **The physics-execution, inheritance, and output-buffer seams (Phases 1–3) are shared foundation, not dual-driver-only.** Both the snapshot driver and the model builder depend on them. Keep them format-neutral and free of tree-index, traversal-order, and driver-specific assumptions even though only the tree driver exists at v1.0. Treat a leak of tree-specific assumptions into these seams as a defect, not a convenience.
- **The physics-module ABI is a stability boundary.** The dual-driver work may change how halos are gathered, inherited, ordered, buffered, and written, but it must not casually change how ordinary FoF-scoped modules are called.
- **Generated metadata remains the structural source of truth.** Driver-neutral output work must remove tree-index assumptions from generated output paths instead of papering over them in one driver.
- **Snapshot input conversion is external.** Mimic will not repair skipped halo links or insert phantom/bridge halos internally. The snapshot driver may assume the converter has produced a temporally complete adjacent-snapshot representation after startup validation passes.
- **Snapshot-global operations are follow-on work.** The snapshot driver makes global SHAM, HOD, environment, radiation-field, and lightcone workflows expressible, but the first acceptance target is cross-format identity for ordinary FoF-scoped physics.
- **Determinism is required for cross-format identity.** Future stochastic modules must seed from stable per-halo or per-FoF keys, not from traversal-order RNG streams.
- **The model builder inherits these constraints.** It should not push Mimic toward unstable module interfaces, ad hoc metadata, traversal-order stochasticity, or validation claims that cannot be mechanically defended.
