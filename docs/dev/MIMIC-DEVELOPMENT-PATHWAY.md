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
| `SHIN-UCHUU-CONVERSION-PLAN.md` | **Converter done — Shin-Uchuu production conversion pending** | External ctrees-ASCII → snapshot-HDF5 converter, validated on micro-Uchuu; Shin-Uchuu production conversion remains, after the dual-driver identity gate | Converter built and micro-Uchuu-validated 2026-07-24; format contract frozen 2026-07-18 at `docs/dev/SNAPSHOT-HDF5-FORMAT.md` |
| `MIMIC-SNAPSHOT-GLOBAL-MODULES-PLAN.md` | Requirements brief | Module contracts over a co-resident snapshot population (true global SHAM, HOD, environment, synchronous reionization) — the single-node scientific payoff of the snapshot driver | Blocked on the dual-driver Phase 5 identity gate; prerequisite for the distributed plan below |
| `MIMIC-DISTRIBUTED-SNAPSHOT-PLAN.md` | Requirements brief | MPI/domain decomposition for snapshot-global operations (former dual-driver Phase 7); pairs with the module-contracts brief above | Blocked on the snapshot driver and on at least one snapshot-global module contract existing |
| `MIMIC-EMBEDDED-ENGINE-PLAN.md` | Requirements brief | Physics-only API for external hosts (former dual-driver Phase 6) | Not scheduled; independent of the snapshot pathway |
| `MIMIC-MODEL-BUILDER-PLAN.md` | Requirements brief | Assisted, gate-driven model-package construction from scientific evidence | Re-review against the tagged baseline before any implementation RFC |

Mimic v1.0 is tagged and released from `main` as the first production baseline. The 2026-07-02 joint review of the dual-driver and Shin-Uchuu plans (findings, decisions D1–D12, and rationale) is archived at `archive/dev-plans/dual-driver-plan-review.md`; its decisions are baked into the two active plans, which are self-contained.

Completed plans are archived out of `docs/dev/` to `archive/dev-plans/` (gitignored local history); durable current instructions live in `docs/USER-GUIDE.md`, `docs/DEVELOPER-GUIDE.md`, `docs/dev/SNAPSHOT-HDF5-FORMAT.md`, the package READMEs, the skills, and the code itself. Already archived this way: `chunked-output-plan.md` (chunked HDF5 output) and `MIMIC-CONVERTER-IMPLEMENTATION-PLAN.md` (the ctrees→snapshot converter build, complete 2026-07-24 — the converter now lives under `scripts/convert/`).

Archived predecessor plans, validation records, and closeout handoffs are historical evidence, not active planning inputs.

---

## Current Sequence

The snapshot pathway is the chosen direction (it is both the scientific priority and the only way Mimic can process Shin-Uchuu, whose percolation super-forest defeats forest-ordered loading). One sequence, each step gating the next:

1. **Freeze the snapshot-HDF5 format contract** as a durable `docs/` spec, from the schema drafted in `SHIN-UCHUU-CONVERSION-PLAN.md`. — **Done 2026-07-18:** frozen at [`docs/dev/SNAPSHOT-HDF5-FORMAT.md`](SNAPSHOT-HDF5-FORMAT.md) (`format_version = 1`).
2. **Build the converter and validate it on micro-Uchuu ASCII** (`SHIN-UCHUU-CONVERSION-PLAN.md`) — topology cross-check against the existing tree-ordered reader. — **Done 2026-07-24:** converter built under [`scripts/convert/`](../../scripts/convert/) and validated end to end on the real micro-Uchuu ASCII data (22,580,924 halos, 50 snapshots, 440,651 forests); producer validation battery passes all invariants, and the cross-check against a `halos-only` reference run passes all seven checks with zero unexplained mismatches. The topology-order gate is **fully discharged**: the optional Slice 10 read-only reference-topology dump harness (`tests/unit/tools/dump_ctrees_topology.c`, the plan's one deliberate exception to "no new Mimic code") let `crosscheck.py --reference-topology` compare `FirstProgenitor`/`NextProgenitor`/`NextHaloInFOFgroup` chain order directly against the tree-ordered reader by stable id.
3. **Snapshot reader** (dual-driver Phase 4b) against the micro-Uchuu fixtures. — **← NEXT.** Steps 1–2 are done; the converter's validated micro-Uchuu HDF5 output is the reader-development fixture. Owned by `MIMIC-DUAL-DRIVER-PLAN.md`.
4. **Snapshot driver + cross-format identity gate** (dual-driver Phase 5) on micro-Uchuu.
5. **Shin-Uchuu production conversion** (one-time, 5.6 TB) and the `simulations/shin-uchuu/` package; sage16 end to end with HMF/GSMF sanity checks.

Afterwards, the requirements briefs, in the expected (not frozen) order: `MIMIC-SNAPSHOT-GLOBAL-MODULES-PLAN.md` then `MIMIC-DISTRIBUTED-SNAPSHOT-PLAN.md`, which go together — the module contracts are the single-node physics payoff of the snapshot driver, and distribution has nothing to parallelise until they exist; then `MIMIC-EMBEDDED-ENGINE-PLAN.md`; then `MIMIC-MODEL-BUILDER-PLAN.md`. Re-prioritise on scientific need once the sequence above completes.

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
