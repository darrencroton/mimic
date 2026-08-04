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
| `MIMIC-SNAPSHOT-READER-PLAN.md` | **Executed — retained until Phase 5 lands** | The five-slice implementation plan for sequence item 3 (dual-driver Phase 4b): the `SnapshotReader` vtable, its registry, the `snapshot_hdf5` reader, the fixture package, and startup wiring. Derives its scope from `MIMIC-DUAL-DRIVER-PLAN.md` and corrected that plan's Phase 4b/5 boundaries in its Slice 5 | All five slices implemented under `project-manager` Mode B (Revision 5), 2026-08-03 to 2026-08-04. Retained rather than archived because its Deferred section is the input list for Phase 5; archive it once Phase 5 closes |
| `SHIN-UCHUU-CONVERSION-PLAN.md` | **Converter done — Shin-Uchuu production conversion pending** | External ctrees-ASCII → snapshot-HDF5 converter, validated on micro-Uchuu; Shin-Uchuu production conversion remains, after the dual-driver identity gate | Converter built and micro-Uchuu-validated 2026-07-24, and re-gated on a regenerated dataset 2026-08-03; format contract frozen 2026-07-18 at `docs/dev/SNAPSHOT-HDF5-FORMAT.md` |
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
2. **Build the converter and validate it on micro-Uchuu ASCII** (`SHIN-UCHUU-CONVERSION-PLAN.md`) — topology cross-check against the existing tree-ordered reader. — **Done 2026-07-24:** converter built under [`scripts/convert/`](../../scripts/convert/) and validated end to end on the real micro-Uchuu ASCII data (22,580,924 halos, 50 snapshots, 440,651 forests); producer validation battery passes all invariants, and the cross-check against a `halos-only` reference run passes all seven checks with zero unexplained mismatches. The topology-order gate is **fully discharged**: the optional Slice 10 read-only reference-topology dump harness (`tests/unit/tools/dump_ctrees_topology.c`, the plan's one deliberate exception to "no new Mimic code") let `crosscheck.py --reference-topology` compare, per halo and by stable id, `FirstProgenitor`/`NextProgenitor`/`NextHaloInFOFgroup` chain order, `ForestIndex`/`HaloRankInForest`, and the signed `MostBoundID` directly against the tree-ordered reader — over an asserted-complete dump of all 22,580,924 halos, so the comparison is exhaustive rather than a sample.
   **Dataset regenerated and re-gated 2026-08-03.** The converted micro-Uchuu dataset was rebuilt from the ASCII source and now lives at `/Volumes/Internal/data/uchuu/micro-uchuu/micro-uchuu-snapshot/` (50 `snapshot_NNN.h5` plus `forests.h5`, 2.3 GB), reachable through the machine-local gitignored symlink `simulations/micro-uchuu-snapshot/snapshots`. The whole gate was re-run rather than assumed, because the Python stack is not the one the original run used (observed now: pandas 3.0.5, numpy 2.4.6; the 2026-07-24 stack was never recorded, so the delta is unknown rather than measured): the three totals the original gate recorded are reproduced exactly (22,580,924 halos, 50 snapshots, 440,651 forests), the producer battery passes all 15 checks, and the cross-check against a fresh `halos-only` reference run passes every check including `topology-chains` over an asserted-complete dump of all 22,580,924 halos. `max_halo_rank_in_forest = 350074` is recorded here for the first time. This satisfies step 3's development fixture and step 4's Phase 5 prerequisite.

3. **Snapshot reader** (dual-driver Phase 4b) against the micro-Uchuu fixtures. — **Done 2026-08-04**, implemented from the frozen [`MIMIC-SNAPSHOT-READER-PLAN.md`](MIMIC-SNAPSHOT-READER-PLAN.md) (five slices, Mode B) whose scope derives from `MIMIC-DUAL-DRIVER-PLAN.md` Phase 4b. What shipped: the `simulations/micro-uchuu-snapshot/` fixture package with committed re-chunked fixtures; `struct SnapshotReader` with its own registry and dispatchers under `src/io/snapshot/`; the `snapshot_hdf5` reader validating structure, headers, exact `scale_factor` agreement with the a_list, invariant 5 in full via bounded hyperslab scans, and the identity-multiplier bounds at `open_run`, plus slab loading with link-range validation under a defined lifecycle; and startup wiring resolving `input.tree_type` against both registries, with the new `simulation.unique_galaxy_id_multiplier` key.
   **Gate met (Phase 4b's stated gate: reader unit tests pass against the Phase 4a fixtures, tree path untouched and green).** Reader unit tests pass under `MODEL=halos-only SIMULATION=micro-uchuu-snapshot`, covering every corrupt-input abort with no leak diagnostic, and the opt-in real-data test opened all 50 snapshots of the regenerated dataset rather than skipping. The tree-ordered path was proven unchanged by an explicit bitwise comparison of a default-pair binary run's galaxy output across the wiring slice, and the default-pair unit, integration, and scientific tiers are green. A snapshot-ordered configuration now passes *configuration* validation and stops at exactly one place: `run_processing_driver()`. Note the boundary precisely, because it is Phase 5's first job: the reader's dataset validation (`open_run`) and link checks (`load_slab`) are implemented and unit-tested but have **no caller in `src/`**, so a snapshot-ordered run aborts before opening any snapshot file. `docs/dev/SNAPSHOT-HDF5-FORMAT.md` was consumed unchanged (`format_version` still 1).
4. **Snapshot driver + cross-format identity gate** (dual-driver Phase 5) on micro-Uchuu. — **← NEXT.** The reader and the regenerated 50-snapshot dataset are both in place; the reader's dataset validation is implemented and unit-tested but still has no run-path caller, so wiring it in is part of this step rather than something already done. `MIMIC-DUAL-DRIVER-PLAN.md` Phase 5 owns the scope and carries, in its "Recorded Phase 5 inputs" block, the full set of five findings the reader work surfaced — read that block rather than a summary here, since a partial list invites planning against the wrong scope.
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
