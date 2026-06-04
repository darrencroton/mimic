# Mimic Dual-Driver Change Map & Migration Plan

**Status:** Implementation plan (proposed).
**Companion:** `docs/dev/MIMIC-DUAL-DRIVER-ARCHITECTURE.md` (the vision this realises).
**Context:** Read `docs/dev/MIMIC-DEVELOPMENT-PATHWAY.md` first for sequencing, v1.0 baseline assumptions, named substep phase work, and how this plan relates to the longer-term model-builder proposal.
**Date:** 2026-06-02

---

## How to read this

A file-by-file map of what **stays**, what is **extracted**, what is **added**, and what **generalises**, sequenced as a phased migration. The ordering is deliberate: every phase up to the snapshot driver is **behaviour-preserving for the existing tree-ordered run** and is **gated by the existing test suites** (unit, integration, scientific, and SAGE-parity). The risky science (inheritance) is refactored *before* any new driver exists, so regressions surface against a known-good baseline.

Validation gate used throughout — **"green"** means:

```bash
make check-generated && make validate-modules
make test-unit          # fast
make test-integration   # medium
make test-scientific    # slow — delegate to a subagent, act on summary
```

Treat any non-zero exit code as failure regardless of log text.

### On "byte-identical" and the existing regression baseline

Phases 0–3 below assert outputs are **byte-identical** to a pre-change baseline. Two clarifications the implementer needs:

- **The baseline mechanism already exists.** `tests/data/output/baseline/` (binary + HDF5) is captured Mimic output used as a regression reference by `tests/integration/test_output_formats.py`. This is a *Mimic-vs-Mimic* mechanism — exactly the right foundation for the behaviour-preserving gates here. Extend/refresh it against the tagged v1 so the migration gate covers the relevant core and SAGE baryonic output byte-identically, then diff every phase against that v1 baseline. The current pre-v1 baseline is the seed, not necessarily the final gate. The v1 SAGE baseline will also serve as the trusted external reference that `docs/dev/galaxy-model-builder-design.md` relies on for reference-parity gate validation.
- **Byte-identity is the target, with a defined fallback.** A pure extraction *should* preserve every floating-point operation. If a phase — realistically Phase 2 — produces a sub-ULP diff from a legitimate, science-neutral reordering (struct field init order, an unavoidable expression regrouping), the gate degrades to a **documented numeric tolerance with a recorded justification**, not a silent pass and not a hard block on a scientifically null diff. Default to byte-identity; reach for tolerance only with an explicit, reviewed reason. This keeps Vision Principle 7 about *useful* failure rather than brittle failure.

## Preconditions (baseline before Phase 0)

- **A finalised, released v1 is the golden baseline.** This plan's safety rests on *byte-identity* checks against the v1 tree-driver output, including the relevant SAGE baryonic properties. That baseline should be the finalised v1 — after the routine pre-release work (core testing, code simplification/linting, large-simulation memory optimisation, and the HDF5-writer optimisation). Tag it; every "outputs byte-identical" gate below diffs against that tag. Doing the refactor against an un-finalised, still-being-optimised base means chasing two moving targets and a noisy parity diff.
- **Named substep phases are now the pipeline contract.** `MIMIC-NAMED-SUBSTEP-PHASES.md` records the implemented contract: fixed optional `pre_timestep`/`post_timestep` phases plus ordered user-named middle phases under `modules.phases:`. The dual-driver extraction must preserve that named phase sequence rather than baking numbered middle slots into the new engine API.
- **The module ABI is frozen** (`MIMIC-DUAL-DRIVER-ARCHITECTURE.md` §5.2). No phase here changes `process(ctx, halos, ngal)`, the `Module` struct, or the property/metadata generation — existing models and the planned model-builder (`docs/galaxy-model-builder-design.md`) depend on it.
- **Line references below are current-tree as of 2026-06-02.** The v1 finalisation will touch `main.c` (memory), the output/HDF5 path, and core code; refresh the `file:line` anchors in the file inventory after v1 is tagged.

> **Sequencing reality check.** Phase 0 cannot begin until v1 is tagged, and v1 is gated behind in-progress work (memory optimisation, HDF5-writer optimisation) whose effort is **scoped separately and not estimated in this plan**. The detailed 8-phase map below is ready to *sequence*, not ready to *start*: treat "tag v1" as the real entry gate, not Phase 0.

---

## File inventory (current coupling)

| File | Role today | Disposition |
|------|-----------|-------------|
| `src/core/main.c` | Hardcoded tree lifecycle: file loop → `load_tree_table` → tree loop → `build_halo_tree` → `save_halos` → `free_halos_and_tree` (`:432–471`) | **Generalise** → driver dispatcher |
| `src/core/build_model.c` | Tree traversal **and** inheritance **and** physics-evolution call **and** output marshalling | **Split**: traversal stays (tree driver); inheritance extracted (shared); evolution/output separated |
| `src/core/module_registry.c` | `execute_phase()` (`:769`) physics engine | **Stays** (becomes the named shared engine entry point) |
| `src/core/module_interface.h` | `Module`, `ModuleContext` contracts | **Stays**; minor doc updates |
| `src/io/tree/interface.{c,h}` | Format-reader abstraction | **Widen** to admit a snapshot-grouped data model |
| `src/io/tree/binary.c`, `hdf5.c` | Tree-ordered readers | **Stay** (tree driver) |
| `src/io/output/*`, HDF5 writers, generated schema | Output schema + writers | **Stay**; feed from a driver-neutral buffer |
| `src/include/globals.h` | Global state (config, units, `Age`/`ZZ`, halo arrays) | **Stays** as default instance; candidate for handle encapsulation (Phase 6) |
| `src/core/read_parameter_file.c`, `init.c` | Config + init | **Extend**: add `TreeFormat`, fail-fast |

---

## Phase 0 — Driver dispatch seam + `TreeFormat` (behaviour-preserving)

**Goal:** introduce the selection point and the dispatcher with only the tree driver behind it. No behaviour change.

- `read_parameter_file.c` / `init.c`: add `TreeFormat` parameter (`tree_ordered` | `snapshot_ordered`); default `tree_ordered`. Validate against the compiled-in readers/drivers; **fail fast** with a clear message otherwise (Vision Principle 7).
- `main.c`: extract the per-file/per-tree lifecycle body (`:428–498`) into `run_tree_driver()` (new `src/core/driver_tree.c`). `main()` becomes: read config → init → register/init modules → **dispatch on `TreeFormat`** → shutdown/metadata. With only `tree_ordered` wired, output is bit-identical.

**Gate:** green; binary/HDF5 outputs byte-identical to pre-change baseline.

---

## Phase 1 — Separate physics evolution from output marshalling

**Goal:** make the physics-execution boundary explicit by removing output bookkeeping from the evolution path.

- `build_model.c`: `process_halo_evolution()` (`:552`) currently runs the configured phase lifecycle **and** calls `update_halo_properties()` (`:445`), which writes into the global `ProcessedHalos`/`HaloAux` (tree-shaped output marshalling). Split them:
  - `run_fof_phases(ctx, halos, ngal)` — central selection, context setup, `pre_timestep`, the configured named substep phase sequence, and `post_timestep`. Pure engine; no output side effects.
  - `marshal_processed_fof(...)` — the `update_halo_properties` body, owned by the driver's output buffering.
- Tree driver calls both in sequence exactly as today.

**Gate:** green; outputs byte-identical. This isolates the engine call so the snapshot driver can reuse `run_fof_phases` without inheriting tree output layout.

---

## Phase 2 — Extract the format-neutral inheritance service (the critical step)

**Goal:** lift inheritance off tree indices into a shared service, preserving exact tree-driver semantics. **This is the highest-risk, parity-gated step and is done while the tree driver is still the only driver**, so the SAGE-parity and scientific suites validate it against a known-good baseline.

- Source today (all `build_model.c`): `find_most_massive_progenitor` (`:128`), `copy_progenitor_halos` (`:183`), `set_halo_centrals` (`:359`), `join_progenitor_halos` (`:420`). These read progenitor galaxies via `ProcessedHalos[HaloAux[prog].FirstHalo + i]` and descendant properties via `InputTreeHalos[halonr]`.
- Refactor into two responsibilities:
  - **Gather (driver-specific):** produce, for one descendant, the list of already-processed **progenitor galaxies** and the descendant's **halo properties** (`Mvir`, `Pos`, `Vel`, `Spin`, `Len`, `Vmax`, `VelDisp`, `MostBoundID`, virial quantities, `SnapNum`, FOF membership). For the tree driver this is a thin shim over the existing tree links.
  - **Inherit (shared, new `src/core/inheritance.c`):** `inherit_descendant(progenitor_galaxies[], n_prog, descendant_props, out_slice)` — owns Type 0/1/2/3 transitions, orphan creation, infall capture (`previousMvir/Vvir/Vmax`), merger-clock handling, snapshot-accumulator reset (`reset_galaxy_properties.inc`, `:238`), deep galaxy copy, and the local-central rule (`set_halo_centrals`). No reference to `InputTreeHalos`/`HaloAux`/`ProcessedHalos`.
- Tree driver wires gather→inherit to reproduce current control flow precisely.
- The dynamic `FoFWorkspace` growth logic (`:192–216`) moves with the marshalling side, not the inheritance science.

**Gate:** green, with **special weight on `test-scientific` and SAGE parity** — this must be a pure behaviour-preserving extraction. Recommended: capture full output before and after and assert byte-identity on a reference tree set.

---

## Phase 3 — Driver-neutral output buffering

**Goal:** let any driver feed the existing writers without assuming the per-tree `ProcessedHalos` layout.

- `build_model.c:445` / output path: define a small buffer contract (append processed galaxies → flush to writer) that the **tree driver** fills per-tree (as today) and the **snapshot driver** will fill per-snapshot.
- Remove tree-index assumptions from generated output preparation. Today some generated output logic and helper functions reach back through `InputTreeHalos`/`HaloNr` for values such as central virial mass and conditional virial quantities. Phase 3 must replace those with driver-neutral output context or precomputed `Halo`/`HaloOutput` fields before claiming the buffer is driver-neutral.
- The generated output **schema**, provenance, and binary/HDF5 writers (`src/io/output/*`, `main.c:189` schema writer, HDF5 attrs `:476–489`) should remain semantically unchanged for the tree driver. The producer of records becomes pluggable only after output preparation no longer depends on tree-only globals.

**Gate:** green; outputs byte-identical for the tree driver.

---

## Phase 4 — Snapshot-ordered reader

**Goal:** read the snapshot-ordered file format into an in-memory snapshot-grouped model with adjacent-snapshot descendant/progenitor links.

- Widen `src/io/tree/interface.{c,h}` so a reader can expose **snapshot slabs** (halos grouped by `SnapNum`) plus a **descendant index**, alongside the existing per-forest tree model.
- Add `src/io/tree/snapshot.c` implementing the reader for the snapshot-ordered format (binary and/or HDF5 as needed).
- The snapshot reader assumes conversion has already inserted phantom/bridge halos for skipped links, producing a temporally complete adjacent-snapshot representation. Mimic should validate this contract enough to fail fast on mismatched metadata or broken links; it should not attempt to repair skipped vertical-tree links internally.
- No driver yet — unit-test the reader in isolation (counts, link integrity, adjacent-snapshot continuity, round-trip against a converter-produced fixture).

**Gate:** green; new reader unit tests pass; existing tree path untouched.

---

## Phase 5 — Snapshot driver (single-node, globally correct)

**Goal:** the second front-end, end-to-end, reusing the shared inheritance service, physics engine, and output buffer.

- Add `src/core/driver_snapshot.c`: loop snapshots in increasing time; at snapshot *N*, for each halo gather its progenitor galaxies (produced at *N−1*, kept resident, keyed by halo ID), call the shared `inherit_descendant`, then `run_fof_phases`, then append to the per-snapshot output buffer; advance.
- Persistent galaxy state across snapshots (descendant-keyed) lives here — the snapshot analogue of the tree driver's `ProcessedHalos` carry-over.
- Wire `TreeFormat: snapshot_ordered` → `run_snapshot_driver()` in the dispatcher.
- **Collective-operation seam (the payoff, not the acceptance gate):** because a snapshot's population is co-resident, global ranking for SHAM, occupation statistics, a shared radiation field, environment fields, and lightcone assembly become expressible. Do not hide this as a small FoF-module tweak. Record the seam and any minimal no-op plumbing needed now, but treat a production per-snapshot collective module contract as follow-on design work after the cross-format identity driver lands.

**Gate:** green **plus cross-format identity** — same trees converted to both orderings must yield identical galaxies (global-only physics disabled for the comparison), per `MIMIC-DUAL-DRIVER-ARCHITECTURE.md` §7. This gate is exact **only** under the determinism invariant (§7.1 there): no global RNG stream, per-FoF independence. If a stochastic module is ever added, it must seed per-halo deterministically or this gate becomes statistical — confirm the invariant holds before relying on byte-level identity here.

---

## Phase 6 — Embeddable physics-only API + engine-state design

**Goal:** package the engine for external callers and put the seam in place for future reentrancy, without forcing de-globalisation now.

- Expose a stable, documented physics-only surface around `run_fof_phases` /`execute_phase`: an init/teardown contract (config, units, time tables, module registry) plus the `(ctx, halos, ngal)` call. Host owns halos, tracking, and ordering (per the Q3 decision: physics-only; inheritance stays internal).
- Thread engine entry points through an **explicit engine-state argument with a default global instance**. Internal drivers and single-instance hosts use the default unchanged. Document which globals (`MimicConfig`, units, `Age`/`ZZ`, registry — `globals.h`) must migrate into the handle for a truly reentrant host, but do not migrate them until a reentrant host exists.
- Ship a minimal external example (e.g. a small C driver, and optionally a thin Python binding) that runs configured modules on host-supplied halos — mirroring what the module unit tests already do.

**Gate:** green; external example runs and matches an equivalent in-tree result on a shared fixture.

---

## Phase 7 (later) — MPI-distributed global operations in the snapshot driver

**Goal:** scale the snapshot driver's global step beyond a single node.

- Add domain decomposition for the snapshot driver and cross-domain communication for global operations (ranking, occupation statistics, radiation field).
- The single-node snapshot driver from Phase 5 remains the correctness reference; distributed results must match it on a box that fits one node.

**Gate:** green; distributed vs. single-node global results agree within tolerance on a reference box.

---

## Dependency order

```
Phase 0 ─► Phase 1 ─► Phase 2 ─► Phase 3 ─► Phase 4 ─► Phase 5 ─► Phase 6
(seam)     (engine    (shared    (neutral   (snapshot  (snapshot  (external
            split)     inherit)   output)    reader)    driver)    API)
                                                                     │
                                                          Phase 7 ◄──┘
                                                          (MPI global)
```

Phases 0–3 ship value on their own (cleaner separation, no behaviour change) and de-risk everything after. Phase 2 is the linchpin; do not start Phase 5 until it is green and proven behaviour-preserving. Phase 6 depends only on Phases 1–2 (the engine seam) and can proceed in parallel with 4–5 if desired.

---

## Risks & mitigations

- **Inheritance semantics drift (Phase 2).** Highest risk. Mitigation: pure extraction with no logic change; byte-identity assertion on reference trees; full SAGE-parity + scientific suites as the gate before any new driver exists.
- **Snapshot-driver memory footprint (Phase 5).** O(halos-per-snapshot) is larger than O(forest). Mitigation: documented, bounded, single-node first; box sizing guidance; streaming/decomposition deferred to Phase 7.
- **Invalid snapshot-ordered inputs (Phases 4-5).** The driver assumes the external converter has inserted phantom/bridge halos for skipped links. Mitigation: state this as an input contract; add reader validation and converter-produced fixtures that prove adjacent-snapshot continuity.
- **Persistent cross-snapshot state bugs (Phase 5).** Descendant-keyed carry-over is new bookkeeping. Mitigation: cross-format identity test is a direct check on inheritance carry-over correctness.
- **Output preparation remains tree-indexed (Phase 3).** Generated output currently has paths that can reach `InputTreeHalos` through `HaloNr`. Mitigation: make driver-neutral output context/precomputed output fields a Phase 3 deliverable, then prove tree-driver byte identity against the v1 baseline.
- **Global reads from modules (Phase 6).** Modules still read globals directly during `init()`, including phase-aware dependency helpers such as `module_in_substep_phase(...)`, `modules_in_same_substep_phase(...)`, and `module_precedes_in_substep_phase(...)`. The reentrancy caveat remains: `init(void)`/`cleanup(void)` take no `ModuleContext`, so threading engine state through `ModuleContext` does **not** cover init-time configuration access (see `MIMIC-DUAL-DRIVER-ARCHITECTURE.md` §5.1 caveat). Mitigation: treat init-time config access as a first-class part of the reentrancy work — either an init-time "current engine" mechanism or a documented decision that init-time config stays process-global; harmless while the default global instance is used.
- **Reader/interface widening churn (Phase 4).** Mitigation: keep the tree model path untouched; add the snapshot model alongside rather than reshaping the existing one.

---

## Definition of done

- `TreeFormat` selects the driver; wrong/mismatched formats fail fast at startup.
- Tree driver output is byte-identical to the pre-migration baseline.
- Snapshot driver runs end-to-end and matches the tree driver on cross-converted trees (non-global physics).
- The snapshot-collective capability is explicitly designed or scoped as follow-on work; it is not required to pass the cross-format identity acceptance gate.
- An external host can run configured physics modules on supplied halos via a documented physics-only API, using the default engine instance.
- All four test tiers green; cross-format identity test added to the suite.

---

## When to review `VISION.md`

`VISION.md` is the guiding source of truth and is **not** edited by this plan. Review and amend it **after Phase 5 is green** (snapshot driver passing cross-format identity): that is when Principle 4 (one processing model) and Principle 5 (bounded memory) have demonstrably generalised to two drivers, and the determinism invariant deserves to be named in the vision. Keep the amendment small and descriptive of behaviour that already exists; see `MIMIC-DUAL-DRIVER-ARCHITECTURE.md` §6.1.
