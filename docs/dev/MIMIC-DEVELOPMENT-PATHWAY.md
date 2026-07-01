# Mimic Development Pathway

**Status:** Active planning index for `docs/dev/`.
**Date:** 2026-07-02
**Scope:** Current plan ownership, post-v1.0 sequence, and source-of-truth boundaries for work after the v1.0 production release.

---

## Purpose

This document is the entry point for active development plans in `docs/dev/`. It should answer three questions quickly: what is active, what order should it happen in, and which document owns the details. It deliberately avoids completed-work logs; archived records remain available under `archive/dev-plans/` when historical evidence is needed.

The architectural direction is still governed by `docs/VISION.md`: Mimic is a physics-agnostic core with runtime-configurable physics modules, metadata as structural truth, explicit validation, bounded memory, and reproducible output provenance.

---

## Active Plans

| Document | Status | Role | Actionability |
|---|---|---|---|
| `MIMIC-DUAL-DRIVER-PLAN.md` | **Active — chosen post-v1.0 direction** | Snapshot-ordered reader and driver over the shared v1.0 core seams; owns the cross-format identity gate and the merged sequence | Ready to execute; reviewed against the tagged v1.0 baseline 2026-07-02 with all decisions resolved |
| `SHIN-UCHUU-CONVERSION-PLAN.md` | **Active — first work item of the sequence** | External ctrees-ASCII → snapshot-HDF5 converter, validated on micro-Uchuu before any new Mimic code; Shin-Uchuu production conversion after the identity gate | Ready to execute; blocked only on freezing the format contract (the plan's schema is the draft) |
| `MIMIC-EMBEDDED-ENGINE-PLAN.md` | Requirements brief | Physics-only API for external hosts (former dual-driver Phase 6) | Not scheduled; independent of the snapshot pathway |
| `MIMIC-DISTRIBUTED-SNAPSHOT-PLAN.md` | Requirements brief | MPI snapshot-global operations (former dual-driver Phase 7) | Blocked on the single-node snapshot driver and a first snapshot-global physics contract |
| `MIMIC-MODEL-BUILDER-PLAN.md` | Requirements brief | Assisted, gate-driven model-package construction from scientific evidence | Re-review against the tagged baseline before any implementation RFC |

Mimic v1.0 is tagged and released from `main` as the first production baseline. The 2026-07-02 joint review of the dual-driver and Shin-Uchuu plans (findings, decisions D1–D12, and rationale) is archived at `archive/dev-plans/dual-driver-plan-review.md`; its decisions are baked into the two active plans, which are self-contained.

`chunked-output-plan.md` is complete. It remains in `docs/dev/` as implementation history until archived; durable current instructions live in `docs/USER-GUIDE.md`, `docs/DEVELOPER-GUIDE.md`, the simulation/debug skills, and the code itself.

Archived predecessor plans, validation records, and closeout handoffs are historical evidence, not active planning inputs.

---

## Current Sequence

The snapshot pathway is the chosen direction (it is both the scientific priority and the only way Mimic can process Shin-Uchuu, whose percolation super-forest defeats forest-ordered loading). One sequence, each step gating the next:

1. **Freeze the snapshot-HDF5 format contract** as a durable `docs/` spec, from the schema drafted in `SHIN-UCHUU-CONVERSION-PLAN.md`.
2. **Build the converter and validate it on micro-Uchuu ASCII** (`SHIN-UCHUU-CONVERSION-PLAN.md`) — topology cross-check against the existing tree-ordered reader; no new Mimic code.
3. **Snapshot reader** (dual-driver Phase 4b) against the micro-Uchuu fixtures.
4. **Snapshot driver + cross-format identity gate** (dual-driver Phase 5) on micro-Uchuu.
5. **Shin-Uchuu production conversion** (one-time, 5.6 TB) and the `simulations/shin-uchuu/` package; sage16 end to end with HMF/GSMF sanity checks.

Afterwards, choose among: snapshot-global module contracts, the distributed snapshot plan, the embedded engine, or the model builder — on scientific priority.

---

## Source-Of-Truth Boundaries

- `docs/VISION.md` owns architectural principles and should change only when implemented behaviour justifies a narrow vision update (for this pathway: only after the identity gate passes).
- Active plan files own implementation scope, acceptance criteria, risk gates, and validation commands.
- `docs/DEVELOPER-GUIDE.md`, `docs/USER-GUIDE.md`, package READMEs, and `.agents/skills/` own durable user/developer instructions after a plan lands.
- `archive/dev-plans/` owns historical records and closeouts. Do not mine it as current instruction unless an active plan explicitly cites it.
- Generated files remain generated; metadata YAML and generator scripts are the editable sources of structural truth.

---

## Standing Constraints

- Do not retain backwards-compatibility code for replaced pre-v1.0 behaviours unless the active plan explicitly says the behaviour remains live.
- Keep `input.tree_type` as the reader-format selector and `input.processing_order` as the processing-driver selector. Do not overload one with the other.
- Keep ctrees ASCII as a live supported tree-ordered reader; the micro-Uchuu identity fixture and the converter's reference semantics depend on it.
- Preserve the ordinary physics-module ABI unless a future approved plan explicitly changes it.
- Keep output identity deterministic across MPI task counts and, for the dual-driver work, across equivalent tree-ordered and snapshot-ordered inputs (per-`UniqueGalaxyID` equality, not byte equality).
- Snapshot-ordered input has strictly adjacent links as a format invariant; Mimic validates and aborts, never repairs. Phantom halos are Consistent-Trees' job and are already present in ctrees data.
- Treat failing tests, generated-code drift, docs-link failures, and validation failures as real release blockers.
