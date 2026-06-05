# Mimic Dual-Driver Plan

**Status:** Proposed architecture and migration plan. Phases 1–3 are reclassified as v1.0 work (see below); Phases 4+ follow v1.0. **Phase 1 is DONE** (landed on `main` as commit `56af880`, verified byte-identical); Phase 2 is next and has a dedicated implementation-notes block under its phase heading.
**Date:** 2026-06-05 (revised ordering 2026-06-05)
**Context:** Read `MIMIC-DEVELOPMENT-PATHWAY.md` first. **Revised ordering:** the core-modularisation phases (1–3, and optionally 0) now land *as part of v1.0*, before the final review-and-optimisation sweep, validated byte-identical against the existing SAGE physics baseline (`test_scientific_sage_physics_baseline.py`). Only the snapshot reader and driver (Phases 4–7) start after v1.0 is tagged and its baseline refreshed. The reasoning is recorded under "Why core modularisation moves into v1.0" in the pathway document; a short summary is in "Migration Plan" below. The phase definitions, gates, and architecture in this document are otherwise unchanged.

---

## Purpose

This document defines the architecture and migration path for letting Mimic process merger data through two input orderings: tree-ordered and snapshot-ordered. The existing tree-ordered behaviour remains the first driver. The snapshot-ordered driver is added as a second front end over shared inheritance, physics execution, output, metadata, and validation services.

The purpose is not to rewrite Mimic or introduce new physics. The purpose is to separate driver-specific ordering and buffering from the shared physics-agnostic core so Mimic can support per-history physics and snapshot-synchronous methods without duplicating scientific behaviour.

---

## Motivation

The single structural fact that gates Mimic's method coverage is that the core processes one FoF workspace at a time in depth-first tree order, with per-tree bounded memory. That ordering is ideal for per-history physics, but it structurally cannot express operations that need a whole snapshot's halo population co-resident: global abundance matching (true SHAM), HOD-style statistical population, a synchronous reionization radiation field, environment-dependent physics, and on-the-fly lightcone assembly. These methods are the established reason snapshot-synchronised codes exist (L-Galaxies, UniverseMachine, and EMERGE are all snapshot-ordered).

Rather than bolt a global stage onto the tree driver, the input ordering, the driver, and the memory model become a single coherent choice: a tree-ordered file feeds the tree driver with per-forest memory; a snapshot-ordered file feeds the snapshot driver with per-snapshot memory, making a snapshot's population co-resident and global operations natural. Producing the two orderings is the job of an external converter, not Mimic (see External Conversion Contract).

---

## Architectural Decisions

### Driver Selection

Mimic reads exactly one input ordering per run, declared in the input YAML:

```yaml
TreeFormat: tree_ordered      # or: snapshot_ordered
```

Startup validation must fail fast if the declared ordering, reader, and selected driver do not match. There is no internal auto-detection and no internal conversion.

### External Conversion Contract

A standalone converter owns conversion between tree-ordered and snapshot-ordered files. For snapshot-ordered inputs, that converter also owns any phantom or bridge halo insertion needed to produce adjacent-snapshot continuity. Mimic validates declared ordering and link consistency enough to catch obvious mismatches, then runs on the converted input as given.

### Shared Engine

Both drivers call the same shared services:

- **Inheritance and tracking service:** internal-only service that turns already-processed progenitor galaxies plus descendant halo properties into the inherited FoF workspace. It owns type transitions, orphan handling, infall capture, merger-clock handling, accumulator reset, and local central selection.
- **Physics execution engine:** format-neutral module execution over `(ctx, halos, ngal)` using the current configured module lifecycle. The dual-driver work must not bake in obsolete or numbered phase assumptions.
- **Core services:** configuration, units, cosmology/time tables, memory, logging, module registry, generated property metadata, binary/HDF5 writers, and output provenance.

### Driver Responsibilities

| Responsibility | Tree driver | Snapshot driver |
|---|---|---|
| Input ordering | Forests stored contiguously across snapshots | Halos grouped by snapshot with adjacent-snapshot links |
| Traversal | Depth-first per forest | Increasing-time snapshot loop |
| Progenitor lookup | Tree links within the current forest | Descendant/progenitor index from previous snapshot state |
| Working set | One forest | One snapshot population, per MPI domain when distributed |
| Output buffering | Per forest/tree-compatible buffer | Per snapshot buffer |
| Natural methods | Per-history physics | Global ranking, HOD, environment, radiation-field, and lightcone workflows |

### Module ABI Stability

The physics-module ABI is frozen for this work. Do not change `process(struct ModuleContext *ctx, struct Halo *halos, int ngal)`, the `Module` registration contract, or the YAML-to-C property and metadata generation contracts. If engine state later becomes explicit, it must be carried through state handles, context, or the existing global instance without changing ordinary module call signatures.

### Determinism

Cross-format identity depends on deterministic per-FoF physics. Future stochastic modules must seed from stable per-halo or per-FoF keys, never from a global RNG stream consumed in traversal order. A traversal-order RNG would make tree-driver and snapshot-driver output diverge permanently.

### Output Contract

Both drivers emit the same generated output schema and provenance model. The producer of output records becomes pluggable, but binary/HDF5 interpretation remains driven by property metadata and run-local schema metadata.

---

## Current Coupling (File Inventory)

This maps where today's tree-coupled behaviour lives and what happens to each file during migration. Function names are durable; exact `file:line` anchors are intentionally omitted. Under the revised ordering, Phases 1–3 run *before* the v1.0 memory/HDF5 optimisation sweep, so re-derive anchors against the current `main` tree before starting — not against a tagged build. The optimisation sweep then runs over the already-restructured code and will move these anchors again afterward; that is expected, and is precisely why the sweep follows the extraction rather than preceding it.

| File | Role today | Disposition |
|---|---|---|
| `src/core/main.c` | Hardcoded tree lifecycle: file loop → `load_tree_table` → tree loop → `build_halo_tree` → `save_halos` → `free_halos_and_tree` | Generalise → driver dispatcher |
| `src/core/build_model.c` | Tree traversal, inheritance, physics-evolution call, and output marshalling combined | Split: traversal stays (tree driver); inheritance extracted (shared); evolution/output separated |
| `src/core/module_registry.c` | `execute_phase()` physics engine | Stays; becomes the named shared engine entry point |
| `src/core/module_interface.h` | `Module`, `ModuleContext` contracts | Stays (frozen ABI); doc updates only |
| `src/io/tree/interface.{c,h}` | Format-reader abstraction | Widen to admit a snapshot-grouped data model |
| `src/io/tree/binary.c`, `hdf5.c` | Tree-ordered readers | Stay (tree driver) |
| `src/io/output/*`, HDF5 writers, generated schema | Output schema + writers | Stay; feed from a driver-neutral buffer |
| `src/include/globals.h` | Global state (config, units, `Age`/`ZZ`, halo arrays, registry) | Stays as default instance; candidate for handle encapsulation (Phase 6) |
| `src/core/read_parameter_file.c`, `init.c` | Config + init | Extend: add `TreeFormat`, fail fast on mismatch |

The inheritance science to be extracted in Phase 2 currently lives in `build_model.c` (`find_most_massive_progenitor`, `copy_progenitor_halos`, `set_halo_centrals`, `join_progenitor_halos`); see Phase 2 for the tree-index coupling these carry and the gather/inherit split.

---

## Migration Plan

Each phase before the snapshot driver is behaviour-preserving for the existing tree-ordered run and is gated against the regression baseline.

**Ordering (revised).** Phases 1–3 are v1.0 work and land *before* the final review-and-optimisation sweep, each gated byte-identical against the current SAGE physics baseline (`test_scientific_sage_physics_baseline.py`). Phase 0 is optional at v1.0 — it is the only purely snapshot-driver-anticipatory step — and may be done last or deferred without gating the release. The sweep then hardens the restructured core, v1.0 is tagged, and the baseline is refreshed. Phases 4–7 (snapshot reader and driver) begin after the v1.0 tag, gated against the refreshed tagged-v1.0 baseline. The short version of the rationale: Phases 1–3 are core modularisation shared by every forward direction (snapshot driver and model builder alike), the baseline already exists to prove them behaviour-preserving, and the single high-value sweep is worth far more applied to the final architecture than to code about to be demolished. The full rationale is in `MIMIC-DEVELOPMENT-PATHWAY.md`.

**Reading the gates below.** Where a Phase 1–3 gate says "the v1.0 baseline", read it as the *current pre-sweep* SAGE physics baseline (Phases 1–3 run before the sweep). Where a Phase 4–7 gate says it, read it as the *refreshed tagged-v1.0* baseline. Each behaviour-preserving phase lands as its own gated commit; structural extraction is never blended into the optimisation sweep.

### Phase 0: Driver Dispatch Seam

Add `TreeFormat` configuration and a driver dispatcher with only the tree driver wired initially. Extract the current file/tree lifecycle into `run_tree_driver()`. The default remains `tree_ordered`.

**Ordering note:** Phase 0 is the only purely snapshot-driver-anticipatory step in the 0–3 group — with a single driver it adds an indirection that nothing in v1.0 exercises. It is therefore optional for v1.0: do it last (after Phases 1–3) and trivially, or defer it until the snapshot-driver work begins, and do not let it gate the v1.0 tag. Phases 1–3, by contrast, are core modularisation that belongs in v1.0 regardless of whether a second driver is ever built.

**Gate:** standard checks and tests pass; binary/HDF5 output is byte-identical to the v1.0 baseline.

### Phase 1: Separate Physics Execution From Output Marshalling — DONE

Split the current evolution path into a pure FoF phase runner and a driver-owned output marshalling step. The tree driver calls both in the same order as today.

**Gate:** standard checks and tests pass; output remains byte-identical.

**Status: DONE — landed on `main` as commit `56af880`, verified byte-identical** against the SAGE physics baseline (42 properties × 4196 halos) plus the full unit, integration, and scientific tiers. What it produced (the starting state for Phase 2):

- `execute_module_pipeline(ctx, halos, ngal)` in `src/core/module_registry.c` is the shared, format-neutral physics-execution engine. It runs the pre-timestep phase, the substep loop with its user-named phases, and the post-timestep phase, reading phase configuration from `ctx->params` (not a global), so it carries no tree-index, output-array, or traversal-order assumptions. `update_context_for_substep()` moved here from `build_model.c` because substep timing is now an engine concern.
- `marshal_processed_halos(int ngal)` in `src/core/build_model.c` is the post-physics output step (renamed from `update_halo_properties`): it copies non-merged `FoFWorkspace` entries into `ProcessedHalos`, updates `HaloAux` tracking pointers, and frees galaxy data for Type 3 (merged) halos.
- `process_halo_evolution(halonr, ngal)` is now a thin **tree-driver adapter**: it selects the FOF Type 0 central, propagates its `UniqueGalaxyID` into every member's `UniqueCentralGalaxyID`, populates the `ModuleContext` (via `setup_module_context`), then calls the engine. It no longer marshals output.
- `build_halo_tree()` now calls `process_halo_evolution()` and `marshal_processed_halos()` as two explicit steps, in the same order as before.

Phase 1 deliberately did **not** touch the inheritance path (`join_progenitor_halos` and friends); that is entirely Phase 2's scope and is unchanged from pre-Phase-1.

### Phase 2: Extract Format-Neutral Inheritance

Move inheritance science out of tree-index-coupled code into a shared service. Today this science lives in `build_model.c` (`find_most_massive_progenitor`, `copy_progenitor_halos`, `set_halo_centrals`, `join_progenitor_halos`) and reaches progenitors through `ProcessedHalos[HaloAux[prog].FirstHalo + i]` and descendants through `InputTreeHalos[halonr]`. Split it into two responsibilities:

- **Gather (driver-specific):** for one descendant, produce the list of already-processed progenitor galaxies plus the descendant's halo properties. For the tree driver this is a thin shim over the existing tree links.
- **Inherit (shared, new `src/core/inheritance.c`):** owns Type 0/1/2/3 transitions, orphan creation, infall-property capture, merger-clock handling, snapshot-accumulator reset, deep galaxy copy, and local-central selection, with no reference to `InputTreeHalos`/`HaloAux`/`ProcessedHalos`.

This is the highest-risk extraction and should happen while the tree driver is still the only driver, so the scientific and SAGE-parity suites validate it against a known-good baseline. The dynamic `FoFWorkspace` growth logic moves with the output-marshalling side, not the inheritance science.

**Gate:** standard checks and tests pass, with special weight on scientific tests and SAGE parity; full reference output remains byte-identical unless a reviewed tolerance is explicitly accepted.

#### Phase 2 implementation notes (read this before starting — written post-Phase-1)

This block captures the concrete state of the code as of commit `56af880` so the work can begin in a fresh chat without rediscovery. Anchors below were re-derived against `main` after Phase 1 and are current. Phase 1 did not move any inheritance code, so the high-level anchors elsewhere in this document still hold.

**The inheritance path today.** All functions are in `src/core/build_model.c` unless noted:

- `build_halo_tree()` (the per-FOF orchestration): for each FOF group it loops subhalos calling `join_progenitor_halos(fofhalo, ngal, tree, filenr)`, then calls `process_halo_evolution()` then `marshal_processed_halos()`. This is the seam where, after Phase 2, the body should read as four explicit steps: **gather (driver) → inherit (shared) → process_halo_evolution (Phase 1 engine adapter) → marshal_processed_halos (Phase 1 output)**.
- `join_progenitor_halos(halonr, ngalstart, tree, filenr)` — orchestrator: `find_most_massive_progenitor` → `copy_progenitor_halos` → `set_halo_centrals`. Natural host for the gather+inherit split (or replace it with an explicit gather call + inherit call).
- `find_most_massive_progenitor(halonr)` — **pure gather (tree-index only)**: reads `InputTreeHalos[].FirstProgenitor/Len/NextProgenitor` and `HaloAux[].NHalos`, returns `first_occupied` (the main-branch progenitor). Stays driver-side.
- `copy_progenitor_halos(halonr, ngalstart, first_occupied, tree, filenr)` — **the mixed function; the heart of the extraction.** It interleaves four concerns that must be separated: (1) **gather** — iterate progenitors via `InputTreeHalos` links and read already-processed progenitor galaxies from `ProcessedHalos[HaloAux[prog].FirstHalo + i]`; (2) **driver buffer management** — the dynamic `FoFWorkspace` realloc/growth block; (3) **inherit (shared science)** — deep galaxy copy plus accumulator reset (`#include reset_galaxy_properties.inc`), `dT` computation, Type 3 merged-skip and galaxy free, Type 0/1/2/3 transitions, infall-property capture (`previousMvir/Vvir/Vmax`), descendant-property application for the main branch, and orphan creation; (4) **new-object creation** — when a descendant has no progenitor galaxies, `init_halo(ngal, halonr, tree, filenr)` (in `src/core/virial.c`).
- `set_halo_centrals(ngalstart, ngal)` — **already format-neutral**: touches only `FoFWorkspace[].Type` and `.CentralHalo` (subhalo-local central selection, SAGE parity). It can move into the new `src/core/inheritance.c` essentially verbatim. It is inherit-side.

**Tree-index coupling that `inheritance.c` must NOT inherit.** `inheritance.c` must contain zero references to `InputTreeHalos`, `HaloAux`, or `ProcessedHalos`. Inside `copy_progenitor_halos` today the hard couplings are: descendant catalog fields read straight from `InputTreeHalos[halonr]` (`MostBoundID`, `Pos[3]`, `Vel[3]`, `Spin[3]`, `Len`, `Vmax`, `VelDisp`, `SnapNum`, and the `FirstHaloInFOFgroup` central test); descendant virial quantities via `get_virial_mass/radius/velocity(halonr)` (in `virial.c`, which themselves read `InputTreeHalos[halonr]` and `MimicConfig`), called mid-transition with the "use maximum-ever `Rvir`/`Vvir`" rule; progenitor galaxies via `ProcessedHalos[HaloAux[prog].FirstHalo + i]`; and `init_halo` plus the `UniqueGalaxyID` encoding (`file*1e15 + tree*1e9 + halonr`), which is tree/file identity.

**Recommended gather/inherit contract.** Define a driver-neutral descendant payload that gather fills and inherit consumes. Gather produces, for one descendant subhalo: (a) an ordered list of progenitor galaxy records — gather resolves `ProcessedHalos[HaloAux[...]]` and hands inherit plain pointers/structs so inherit never indexes those arrays; (b) a marker for which progenitor is the main branch (`first_occupied`), expressed as a flag/position, not a tree index; and (c) a "descendant halo properties" struct carrying the raw catalog fields above **plus the precomputed virial quantities** (`Mvir`, `Rvir`, `Vvir`) so inherit applies them without calling `get_virial_*(halonr)`. Inherit then produces the `FoFWorkspace` slice: deep galaxy copy + accumulator reset, Type transitions, infall capture, Type 3 skip/free, orphan creation, and local-central selection. New-object creation (no progenitors) becomes inherit-side too, but identity (`UniqueGalaxyID`) and descendant properties are gather-supplied. The generated `init_halo_properties.inc`, `init_galaxy_properties.inc`, and `reset_galaxy_properties.inc` includes are metadata-driven and carry no tree coupling themselves, so they can live wherever the creation/reset logic lands.

**CRITICAL GOTCHA — "FoFWorkspace growth moves with the output-marshalling side" does NOT mean `marshal_processed_halos()`.** Phase 1 created a function literally named the output-marshalling step (`marshal_processed_halos`), which runs *after* physics. The Phase 2 sentence about growth predates that name. `FoFWorkspace` growth happens at **gather time (before physics)** because gather is what fills the workspace — it must stay on the driver/gather buffer-management side. Do **not** push it into `marshal_processed_halos` (wrong side of the physics call) and do **not** put it in `inheritance.c` (which must be free of allocation and array indexing). Read "output-marshalling side" as "the driver-owned buffer-management side", i.e. gather.

**Memory-ownership invariant to preserve** (documented at `free_halos_and_tree()` in `src/io/tree/interface.c`): galaxy data is deep-copied when read from `ProcessedHalos` into `FoFWorkspace` (today inside `copy_progenitor_halos`); physics mutates it in place; `marshal_processed_halos` struct-copies `FoFWorkspace → ProcessedHalos` (the galaxy pointer transfers; Type 3 entries are freed at marshal time); `free_halos_and_tree` frees galaxies from `ProcessedHalos` only. When the deep copy moves into inherit, gather hands inherit the source galaxy pointer (resolved from `ProcessedHalos`) and inherit allocates the new copy. Do not double-free, and do not leave `FoFWorkspace` and `ProcessedHalos` sharing a galaxy pointer across a tree boundary. Update the `interface.c` comment when `copy_progenitor_halos` is split.

**Two distinct "centrals" — keep them separate.** (1) *Subhalo-local central*: `set_halo_centrals` sets `FoFWorkspace[i].CentralHalo` to the single Type 0/1 within each subhalo slice (SAGE parity allows a Type 2 to point to a Type 1). This is inherit-side. (2) *FOF Type-0 central*: `process_halo_evolution` scans the whole workspace for the one Type 0, uses it for the module context, and propagates its `UniqueGalaxyID` to `UniqueCentralGalaxyID`. This stays on the tree-driver adapter (physics context), not in `inheritance.c`.

**Baseline guard and gate.** The pre-sweep SAGE physics baseline (`models/sage/modules/_tests/test_scientific_sage_physics_baseline.py`) exercises the full inheritance path — Type 0/1/2/3 transitions, orphan creation, infall capture — across 4196 halos and 42 properties, and currently passes byte-identical. Phase 2 must keep it byte-identical (this is the linchpin and highest-risk extraction; the gate weights scientific tests and SAGE parity). Run the standard gate (`make check-generated && make validate-modules`, then `tests-unit`, `tests-integration`, `tests-scientific`); any non-zero exit code is a failure regardless of log text.

**No Phase 1 cleanup is owed.** A post-Phase-1 review confirmed there is nothing to refine, complete, or remove before Phase 2: the inheritance functions sit exactly where the anchors say, `set_halo_centrals` is already format-neutral, and there is no dead code or orphaned symbol from the Phase 1 split. The only Phase-1-introduced hazard is the naming clash documented in the CRITICAL GOTCHA above, which is captured here rather than fixed in code.

### Phase 3: Driver-Neutral Output Buffering

Define an output buffer contract that can be filled by either driver. Remove tree-index assumptions from output preparation and generated output helpers before claiming the path is driver-neutral.

**Gate:** standard checks and tests pass; tree-driver output remains byte-identical.

### Phase 4: Snapshot-Ordered Reader

Add a snapshot-grouped reader model alongside the existing tree model. It should expose snapshot slabs and descendant/progenitor link metadata, validate adjacent-snapshot continuity, and be tested independently before the snapshot driver exists.

**Gate:** reader unit tests pass; existing tree path remains untouched and green.

### Phase 5: Snapshot Driver

Add the snapshot driver. For each snapshot, gather progenitor galaxies from the previous processed snapshot state, call shared inheritance, run shared physics phases, append output records, and advance. Snapshot-global operation hooks may be identified, but production global module contracts are follow-on work.

**Gate:** all standard checks and tests pass, and cross-format identity passes on equivalent converted inputs with snapshot-global physics disabled.

### Phase 6: Physics-Only Embedded Engine

Expose a documented physics-only API that lets an external host initialise Mimic core services and run configured modules over host-supplied halos. The host owns halo finding, progenitor tracking, ordering, and I/O. The inheritance service remains internal to Mimic drivers. The seam already exists: the module unit-test harnesses already drive modules with a hand-built `ModuleContext` + `Halo[]` and no merger tree, which is a working proof of concept for external invocation.

Thread the engine entry points through an explicit engine-state argument with a default global instance, so internal drivers and single-instance hosts are unaffected. Note that true reentrancy is **not** purely a `ModuleContext` change: the module ABI also includes `init(void)` and `cleanup(void)`, which take no arguments and read globals (`MimicConfig`, units, `Age`/`ZZ`, the registry) directly. Threading state through `ModuleContext` covers only the `process()` path. Reaching instance config from `init`/`cleanup` would require an init-time "current engine instance" mechanism, or an explicit decision that init-time configuration stays process-global while only per-timestep state is instanced. Either is acceptable, but the gap must not be under-scoped as a `ModuleContext` change alone.

**Gate:** external example runs and matches an equivalent in-tree result on a shared fixture.

### Phase 7: Distributed Snapshot-Global Operations

Add MPI/domain decomposition and cross-domain communication for snapshot-global operations after the single-node snapshot driver is correct.

**Gate:** distributed results match the single-node reference within a documented tolerance on a reference box.

### Sequencing Notes

Phases 1–3 ship value on their own — cleaner separation with no behaviour change — and de-risk everything after, which is exactly why they are now v1.0 work rather than post-v1.0 work (see the revised ordering above and the pathway rationale). Phase 0 is optional at v1.0 and snapshot-driver-anticipatory; sequence it last or defer it.

Phase 2 is the linchpin and the project's highest-risk refactor; it lands pre-v1.0 under the existing baseline and the v1.0 review sweep, and the snapshot driver (Phase 5) must not start until Phase 2 is green and proven behaviour-preserving.

Phase 6 (the physics-only embedded engine) depends only on the Phase 1–2 engine seam delivered in v1.0, not on the snapshot driver — and that same seam is what a model builder would build on. So after v1.0 the snapshot driver (Phases 4–5, then 7) and the model builder are independent options over a shared foundation: nothing in the model builder requires the snapshot driver, and the next direction should be chosen on scientific priority rather than forced ordering. Phase 7 follows Phase 5.

---

## Standard Gate

For this plan, a standard green gate means:

```bash
make check-generated && make validate-modules
make tests-unit
make tests-integration
make tests-scientific
```

Long-running test output should be captured under `archive/test-logs/`, exit codes must be checked explicitly, and any non-zero exit code is a failure regardless of log text.

---

## Definition of Done

- `TreeFormat` selects a validated driver and fails fast on mismatches.
- The tree driver remains byte-identical to the v1.0 migration baseline through all behaviour-preserving phases.
- The snapshot driver runs end to end and matches the tree driver on cross-converted inputs for ordinary FoF-scoped physics.
- The shared inheritance service, physics execution engine, and output buffer are driver-neutral.
- Snapshot-global capability is explicitly scoped for follow-on work and is not required for the first cross-format identity gate.
- A physics-only embedded engine API exists with a minimal tested example.
- All standard gates pass and a cross-format identity test is added to the suite.

---

## Risks And Mitigations

| Risk | Mitigation |
|---|---|
| Inheritance semantics drift | Extract while tree driver is still the only driver; require byte identity and full scientific/SAGE parity gates |
| Snapshot memory footprint | Document the larger per-snapshot bound; implement single-node correctness first; defer distributed scaling |
| Invalid snapshot inputs | Treat conversion and phantom/bridge insertion as external; validate enough metadata to fail fast |
| Persistent cross-snapshot state bugs | Use cross-format identity as the direct acceptance check |
| Output path remains tree-indexed | Make driver-neutral output context/precomputed fields a Phase 3 deliverable |
| Reentrancy under-scoped | Keep the default global instance initially; treat `init(void)`/`cleanup(void)` global access as first-class reentrancy work, not a `ModuleContext` change; document remaining globals before offering multi-instance/threaded guarantees |
| Snapshot-global modules conflated with driver acceptance | Land cross-format identity first; design collective module contracts later |

---

## Vision Review Timing

Do not update `docs/VISION.md` for this plan until Phase 5 passes cross-format identity. At that point, review the vision narrowly for per-driver memory bounds, determinism as an invariant, and a pointer to the implemented dual-driver architecture.
