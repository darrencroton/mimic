# Mimic Dual-Driver Plan

**Status:** Proposed architecture and migration plan. Phases 0–3 are v1.0 work and are DONE; the pre-v1.0 optimisation/review sweep over the restructured core is also complete. Phases 4+ follow v1.0.
**Date:** 2026-06-19 (revised ordering 2026-06-05; Phase 0 completed 2026-06-19; Phase 2 completed 2026-06-06; Phase 3 completed 2026-06-06; pathway/status refreshed 2026-06-19)
**Context:** Read `MIMIC-DEVELOPMENT-PATHWAY.md` first. **Current ordering:** the core-modularisation phases (1–3), the Phase 0 dispatch seam, and the release sweep have landed as v1.0 work. Only the snapshot reader and driver (Phases 4–7) start after v1.0 is tagged and its baseline refreshed. The phase definitions, gates, and architecture in this document are otherwise unchanged.

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
input:
  processing_order: tree_ordered      # or: snapshot_ordered
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

This maps where today's tree-coupled behaviour lives and what happens to each file during migration. Function names are durable; exact `file:line` anchors are intentionally omitted. Phases 1–3 and the v1.0 optimisation/review sweep have already moved these anchors, so re-derive them against the current `main` tree before starting any later phase.

| File | Role today | Disposition |
|---|---|---|
| `src/core/main.c` | Registry-dispatched tree lifecycle: partition loop → `open_partition` → unit loop → `load_unit` / `build_halo_tree` → output finalisation → `free_unit_halos` | Baseline driver dispatcher for future snapshot work |
| `src/core/build_model.c` | Tree traversal, tree-side gather, physics adapter, and tree-owned output range bookkeeping | Traversal/gather stay (tree driver); range bookkeeping stays driver-specific |
| `src/core/inheritance.{c,h}` | Shared, format-neutral inheritance service (added in Phase 2) | Stays; reused unchanged by the snapshot driver |
| `src/core/output_buffer.{c,h}` | Shared, format-neutral output-buffer marshalling (added in Phase 3) | Stays; reused unchanged by the snapshot driver |
| `src/core/module_registry.c` | `execute_module_pipeline()` physics engine (Phase 1) | Stays; the named shared engine entry point |
| `src/core/module_interface.h` | `Module`, `ModuleContext` contracts | Stays (frozen ABI); doc updates only |
| `src/io/tree/interface.{c,h}` | Format-reader abstraction | Widen to admit a snapshot-grouped data model |
| `src/io/tree/binary.c`, `hdf5.c` | Tree-ordered readers | Stay (tree driver) |
| `src/io/output/*`, HDF5 writers, generated schema | Output schema + writers | Stay; feed from a driver-neutral buffer |
| `src/include/globals.h` | Global state (config, units, `Age`/`ZZ`, halo arrays, registry) | Stays as default instance; candidate for handle encapsulation (Phase 6) |
| `src/core/read_parameter_file.c`, `init.c` | Config + init | Extend: add `input.processing_order`, fail fast on mismatch |

The inheritance science was extracted in Phase 2 into `src/core/inheritance.c` (see the Phase 2 Status block). The output-buffer copy/free rules were extracted in Phase 3 into `src/core/output_buffer.c`. `build_model.c` now retains only tree-driver traversal/gather, tree-specific context setup, and translation between tree halo ids and output-buffer segment ranges.

---

## Migration Plan

Each phase before the snapshot driver is behaviour-preserving for the existing tree-ordered run and is gated against the regression baseline.

**Ordering (current).** Phases 0–3 are complete v1.0 work, and the final review-and-optimisation sweep over that structure is complete. Phases 4–7 (snapshot reader and driver) begin after the v1.0 tag, gated against the refreshed tagged-v1.0 baseline. The short version of the rationale: Phases 1–3 are core modularisation shared by every forward direction (snapshot driver and model builder alike), while Phase 0 establishes the processing-order dispatch seam needed before a second driver can be added. The current release sequence lives in `MIMIC-DEVELOPMENT-PATHWAY.md`.

**Reading the gates below.** Phase 0–3 gates are historical and have passed. Where a Phase 4–7 gate says "the v1.0 baseline", read it as the refreshed tagged-v1.0 baseline.

### Phase 0: Driver Dispatch Seam

Add `input.processing_order` configuration and a driver dispatcher with only the tree driver wired initially. Extract the current file/tree lifecycle into `run_tree_driver()`. The default remains `tree_ordered`.

**Status: DONE — verified behaviour-preserving.** The implementation added `input.processing_order` as the processing-driver selector, keeps `input.tree_type` as the reader-format selector, dispatches through `run_processing_driver()`, isolates the existing tree lifecycle in `run_tree_driver()`, declares all current readers as `INPUT_PROCESSING_ORDER_TREE`, and fails fast for `snapshot_ordered` until the snapshot driver exists. It passed drift audit, code review after fixing the one redundant-error finding, full `make tests`, and a regular `sage16` + `mini-millennium` run.

**Gate:** standard checks and tests pass; binary/HDF5 output is byte-identical to the v1.0 baseline.

### Phase 1: Separate Physics Execution From Output Marshalling — DONE

Split the current evolution path into a pure FoF phase runner and a driver-owned output marshalling step. The tree driver calls both in the same order as today.

**Gate:** standard checks and tests pass; output remains byte-identical.

**Status: DONE — landed on `main` as commit `56af880`, verified byte-identical** against the SAGE physics baseline (42 properties × 4196 halos) plus the full unit, integration, and scientific tiers. What it produced (the starting state for Phase 2):

- `execute_module_pipeline(ctx, halos, ngal)` in `src/core/module_registry.c` is the shared, format-neutral physics-execution engine. It runs the pre-timestep phase, the substep loop with its user-named phases, and the post-timestep phase, reading phase configuration from `ctx->params` (not a global), so it carries no tree-index, output-array, or traversal-order assumptions. `update_context_for_substep()` moved here from `build_model.c` because substep timing is now an engine concern.
- Phase 1 introduced `marshal_processed_halos(int ngal)` as the post-physics output step. **Superseded by Phase 3:** that copy/free logic now lives in `marshal_workspace_to_output_buffer()` in `src/core/output_buffer.c`, while tree-specific range updates stay in `build_model.c`.
- `process_halo_evolution(halonr, ngal)` is now a thin **tree-driver adapter**: it selects the FOF Type 0 central, propagates its `UniqueGalaxyID` into every member's `UniqueCentralGalaxyID`, populates the `ModuleContext` (via `setup_module_context`), then calls the engine. It no longer marshals output.
- `build_halo_tree()` now keeps physics execution and output buffering as explicit separate steps, in the same order as before.

Phase 1 deliberately did **not** touch the inheritance path (`join_progenitor_halos` and friends); that is entirely Phase 2's scope and is unchanged from pre-Phase-1.

### Phase 2: Extract Format-Neutral Inheritance — DONE

Move inheritance science out of tree-index-coupled code into a shared service. Today this science lives in `build_model.c` (`find_most_massive_progenitor`, `copy_progenitor_halos`, `set_halo_centrals`, `join_progenitor_halos`) and reaches progenitors through `ProcessedHalos[HaloAux[prog].FirstHalo + i]` and descendants through `InputTreeHalos[halonr]`. Split it into two responsibilities:

- **Gather (driver-specific):** for one descendant, produce the list of already-processed progenitor galaxies plus the descendant's halo properties. For the tree driver this is a thin shim over the existing tree links.
- **Inherit (shared, new `src/core/inheritance.c`):** owns Type 0/1/2/3 transitions, orphan creation, infall-property capture, merger-clock handling, snapshot-accumulator reset, deep galaxy copy, and local-central selection, with no reference to `InputTreeHalos`/`HaloAux`/`ProcessedHalos`.

This is the highest-risk extraction and should happen while the tree driver is still the only driver, so the scientific and SAGE-parity suites validate it against a known-good baseline. The dynamic `FoFWorkspace` growth logic moves with the output-marshalling side, not the inheritance science.

**Gate:** standard checks and tests pass, with special weight on scientific tests and SAGE parity; full reference output remains byte-identical unless a reviewed tolerance is explicitly accepted.

**Status: DONE — verified byte-identical** against the SAGE physics baseline (42 properties × 4196 halos) plus the full unit (incl. new `test_inheritance`), integration, and `check-generated`/`validate-modules` tiers, and a clean end-to-end run ("No memory leaks detected"). What it produced (the starting state for Phase 3):

- **`src/core/inheritance.c` is the shared, format-neutral inheritance service.** `inherit_descendant_halos(workspace, start, capacity, descendant, progenitors, nprogenitors)` owns the deep galaxy copy + snapshot-accumulator reset, Type 0/1/2/3 transitions, infall capture, Type-3 skip/free, orphan creation, new-object creation, and subhalo-local central selection. It has **zero** references to `InputTreeHalos`/`HaloAux`/`ProcessedHalos`/`FoFWorkspace`/`MimicConfig`/`Age`/`ZZ` (enforce this invariant in review). Its only includes are `inheritance.h`, `galaxy_pool.h`, `error.h` (galaxy storage moved from `mymalloc` to the per-tree pool in the v1.0 memory sweep — see the galaxy-pool learning below).
- **The gather/inherit contract is three driver-neutral structs** (`src/core/inheritance.h`): `InheritanceDescendant` (identity, time, virial quantities precomputed as doubles, `is_fof_central`, `halo_payload`), `InheritanceProgenitorGalaxy` (source pointer into processed state + `source_time` + `is_main_branch`), and the generated `struct HaloInitPayload` (the descendant's tree-sourced init fields). Inherit consumes plain pointers/structs and never indexes driver arrays.
- **Gather stays in the tree driver** (`build_model.c`): `count_progenitor_galaxies` → `ensure_fof_workspace_capacity` (FoFWorkspace growth is driver-owned, as required) → `gather_progenitor_galaxies` resolves `ProcessedHalos[HaloAux[prog].FirstHalo + i]` and hands inherit plain pointers. `make_unique_galaxy_id` is the single owner of the `file*FILE_MUL + tree*TREE_MUL + halonr` identity encoding.
- **New-object init is now metadata-driven and format-neutral.** `init_halo_from_payload`, `init_galaxy_defaults`, and `reset_galaxy_snapshot_accumulators` are generated **inline functions** in `property_defs.h` (no tree coupling). The tree-side payload populator (`populate_halo_payload_from_tree.inc`) is generated from the same property metadata and is the only place tree-index coupling touches halo init; it lives inside `make_halo_init_payload()`. This replaced the former `init_halo()` in `virial.c` and retired the legacy `init_halo_properties.inc` / `init_galaxy_properties.inc` / `reset_galaxy_properties.inc` — one source of truth per concern.

Cleanups and optimisations landed with the extraction (all byte-identical, none deferred to the v1.0 sweep because they arise directly from this work and change no science):

- **No per-subhalo allocation.** The gather progenitor list uses a run-persistent, monotonically-grown scratch buffer (`ProgenitorScratch`, freed by `free_tree_driver_scratch()` before the final leak check in `main.c`). This restores the pre-Phase-2 property that the depth-first hot path allocates nothing per subhalo. Safe because the allocator is explicitly non-LIFO (`memory.c`).
- **Removed dead/duplicated code:** the unused `init_halo()` and its duplicate identity encoding; the three legacy init/reset `.inc` files (consolidated onto the inline functions); `test_property_reset.c` migrated to test the inline functions directly.
- **Capacity contract documented** on `inherit_descendant_halos` (caller pre-sizes `capacity`; the function asserts and never grows the workspace).

Learnings for the next phases:

- **Phase 3 target was isolated and has now landed.** The former tree-index coupling in `marshal_processed_halos()` was split into driver-supplied output segments plus the shared `marshal_workspace_to_output_buffer()` contract; inheritance and the physics engine needed no further work.
- **The snapshot driver (Phase 4–5) writes its own gather + payload populator** from snapshot structures and reuses `init_halo_from_payload` (neutral) and `inherit_descendant_halos` unchanged. A generated `populate_halo_payload_from_snapshot.inc` analogous to the tree one is the natural pattern.
- **Cross-format identity depends on more than RNG.** `UniqueGalaxyID` is encoded from `(file, tree, halonr)` indices. For Phase 5 cross-format identity the snapshot driver must reproduce the *same* per-galaxy identity, which is tree/file-index derived — treat the identity scheme (not just stochastic seeding) as a first-class cross-format-identity contract.
- **Galaxy storage is pooled per unit, not per halo (v1.0 memory sweep).** A later optimisation replaced the one-`mymalloc`-per-galaxy pattern with a chunked per-unit pool (`src/core/galaxy_pool.{c,h}`): `inherit_descendant_halos` allocates each galaxy via `galaxy_pool_alloc()` (a stable pointer into a contiguous chunk that never moves), the Type-3 and marshal paths only *clear* the `galaxy` pointer, and the tree driver bulk-resets the pool per unit (`free_unit_halos`) and destroys it once at shutdown. This removed the allocator's 50 000 concurrent-block cap as a hard limit on halos-per-forest — the cap was previously one tracked block per halo-galaxy, which a single massive Millennium forest exceeded (full Millennium files 0–15 now complete; peak ~22 tracked blocks). **This is the one place the pool's discipline must change for the snapshot driver:** there, galaxies persist for the whole run and individually merge/disrupt, so a whole-run pool needs *per-galaxy* reclamation — an additive `galaxy_pool_free`/free-list on the same module — and must **not** be bulk-reset like the per-unit pool. The pool primitive, the `galaxy` pointer interface, and the `NULL`-means-no-galaxy sentinel are otherwise reusable unchanged, so building the snapshot driver does not require undoing the pool. A focused regression (`tests/unit/test_galaxy_pool.c`) allocates past the block cap and asserts growth, pointer stability, reset reuse, and a clean leak check.

### Phase 3: Driver-Neutral Output Buffering

Define an output buffer contract that can be filled by either driver. Remove tree-index assumptions from output preparation and generated output helpers before claiming the path is driver-neutral.

**Gate:** standard checks and tests pass; tree-driver output remains byte-identical.

**Status: DONE — verified behaviour-preserving** against the SAGE physics baseline plus the standard checks. What it produced:

- **`src/core/output_buffer.c` is the shared, format-neutral output-buffer service.** `marshal_workspace_to_output_buffer(workspace, buffer, segments, nsegments)` copies non-Type-3 workspace entries into a caller-supplied output buffer, clears Type-3 galaxy pointers (the per-tree galaxy pool owns and reclaims the memory — see the Phase 2 galaxy-pool learning), stamps final output snapshot numbers, and records segment output ranges. It carries every physical halo field (including `CentralMvir`) to output by plain struct copy, so it holds no knowledge of any specific property, and it does not reference tree input arrays, `HaloAux`, global output arrays, configuration, or cosmology/time globals.
- **The tree driver supplies output segments.** `build_model.c` records one `OutputBufferSegment` per FoF subhalo after gather/inherit and before physics, with source halo id, workspace range, and final snapshot. After physics, it calls the shared marshaller and copies segment ranges back into `HaloAux[source_id].{FirstHalo,NHalos}` for tree-driver progenitor lookup.
- **Generated output helpers no longer index tree input.** `CentralMvir` is now an internal `struct Halo` field with direct-copy output semantics; the tree driver stamps it onto every workspace member **before physics** from the input-catalog FoF central mass, and physics never mutates it. The generator no longer supports tree-indexed output sources (`copy_from_tree`/`copy_from_tree_array`) because output conversion must be driver-neutral.
- **Phase 1–3 surface cleanup landed.** The obsolete `marshal_processed_halos()` API, unused output-order allocation, and live `allvars.h` compatibility include were removed; `allvars.h` was archived under `archive/include/`.

Hand-off from Phase 3: the core seams are now physics execution (`execute_module_pipeline`), inheritance (`inherit_descendant_halos`), and output buffering (`marshal_workspace_to_output_buffer`). Broad cleanup and optimisation subsequently happened in the v1.0 sweep; do not reopen that sweep for unrelated post-validation refactors.

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

Phases 1–3 ship value on their own — cleaner separation with no behaviour change — and de-risk everything after, which is exactly why they are now v1.0 work rather than post-v1.0 work (see the revised ordering above and the pathway rationale). Phase 0 has also landed as v1.0 work, establishing the processing-order dispatch seam while leaving snapshot-reader and snapshot-driver behaviour for later phases.

Phase 2 was the linchpin and the project's highest-risk refactor; it is now green and proven behaviour-preserving. The snapshot driver (Phase 5) still must not start until v1.0 is tagged and the tagged baseline is fixed.

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

- `input.processing_order` selects a validated driver and fails fast on mismatches.
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
