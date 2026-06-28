# Mimic Development Pathway

**Status:** Active planning index for `docs/dev/`.
**Date:** 2026-06-28
**Scope:** Current plan ownership, release sequence, and source-of-truth boundaries for the v1.0 release and first post-v1.0 work.

---

## Purpose

This document is the entry point for active development plans in `docs/dev/`. It should answer three questions quickly: what is active, what order should it happen in, and which document owns the details. It deliberately avoids completed-work logs; archived records remain available under `archive/dev-plans/` when historical evidence is needed.

The architectural direction is still governed by `docs/VISION.md`: Mimic is a physics-agnostic core with runtime-configurable physics modules, metadata as structural truth, explicit validation, bounded memory, and reproducible output provenance.

---

## Active Plans

| Document | Status | Role | Actionability |
|---|---|---|---|
| `MIMIC-DUAL-DRIVER-PLAN.md` | Post-v1.0 architecture plan | Add snapshot-ordered reader/driver support and later distributed snapshot-global operations over the shared core seams | Re-review after v1.0 is tagged and the baseline is refreshed; do not start snapshot phases before then |
| `MIMIC-MODEL-BUILDER-PLAN.md` | Post-v1.0 requirements brief | Preserve requirements for assisted, gate-driven model-package construction from scientific evidence | Re-review after v1.0 and before any implementation RFC; current text is a conservative brief, not an active build plan |

The pre-v1.0 style sweep is complete and archived outside the tracked docs tree at `archive/dev-plans/mimic-style-sweep-plan.md`. Its final checkpoint, code review, and full `make tests` gate are green; Mimic is ready for the v1.0 tag and release once the completed sweep branch has landed on `main`.

`chunked-output-plan.md` is complete. It remains in `docs/dev/` as implementation history until archived, but durable current instructions now live in `docs/USER-GUIDE.md`, `docs/DEVELOPER-GUIDE.md`, the simulation/debug skills, and the code itself.

Archived predecessor plans, validation records, and closeout handoffs are historical evidence, not active planning inputs. If future work needs a detail from them, cite the archived record and create a new narrow active plan rather than reopening a completed umbrella.

---

## Current Sequence

1. **Land the completed style-sweep branch on `main`.** The pre-release style sweep and review checkpoints are complete, the sweep plan has been archived, and the full test gate is green.

2. **Tag v1.0 and refresh the baseline.** Record the tagged v1.0 output and test baseline as the forward reference for behaviour-preserving post-v1.0 work.

3. **Re-review post-v1.0 plans before executing them.** The dual-driver and model-builder plans should be revised only after v1.0 is tagged, because the final chunked-output and style-sweep state should be the baseline they build on.

4. **Choose the next major direction post-v1.0.** The snapshot driver and model builder share the same v1.0 core foundation, but their relative priority should be decided after the release based on scientific need, risk, and available validation gates.

---

## Source-Of-Truth Boundaries

- `docs/VISION.md` owns architectural principles and should change only when implemented behaviour justifies a narrow vision update.
- Active plan files own implementation scope, acceptance criteria, risk gates, and validation commands.
- `docs/DEVELOPER-GUIDE.md`, `docs/USER-GUIDE.md`, package READMEs, and `.agents/skills/` own durable user/developer instructions after a plan lands.
- `archive/dev-plans/` owns historical records and closeouts. Do not mine it as current instruction unless an active plan explicitly cites it.
- Generated files remain generated; metadata YAML and generator scripts are the editable sources of structural truth.

---

## Standing Constraints

- Do not retain backwards-compatibility code for replaced pre-v1.0 behaviours unless the active plan explicitly says the behaviour remains live.
- Keep `input.tree_type` as the reader-format selector and `input.processing_order` as the processing-driver selector. Do not overload one with the other.
- Keep ctrees ASCII as a live supported tree-ordered reader; Shin-Uchuu depends on it.
- Preserve the ordinary physics-module ABI unless a future approved plan explicitly changes it.
- Keep output identity deterministic across MPI task counts and, for future dual-driver work, across equivalent tree-ordered and snapshot-ordered inputs.
- Treat failing tests, generated-code drift, docs-link failures, and validation failures as real release blockers.
- Before post-v1.0 execution, refresh plan language against the tagged v1.0 baseline rather than assuming historical line anchors or partition models still apply.
