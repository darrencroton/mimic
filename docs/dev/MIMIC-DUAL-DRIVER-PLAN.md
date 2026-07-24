# Mimic Dual-Driver Plan

**Status:** Active. Phases 0–3 are complete v1.0 work (verified against the tagged baseline). Phases 4–5 are the next implementation direction, merged into one sequence with `SHIN-UCHUU-CONVERSION-PLAN.md`. The former Phases 6–7 have moved to their own plans (`MIMIC-EMBEDDED-ENGINE-PLAN.md`, `MIMIC-DISTRIBUTED-SNAPSHOT-PLAN.md`).
**Date:** 2026-07-02 (post-v1.0 review applied; decisions D1–D12 from the joint plan review are baked in; the review record is archived at `archive/dev-plans/dual-driver-plan-review.md`)
**Context:** Read `MIMIC-DEVELOPMENT-PATHWAY.md` first. This plan and the Shin-Uchuu conversion plan form one pathway: format contract → converter (validated on micro-Uchuu) → snapshot reader → snapshot driver + identity gate → Shin-Uchuu production conversion and run.

---

## Purpose

This document defines the architecture and migration path for letting Mimic process merger data through two input orderings: tree-ordered and snapshot-ordered. The existing tree-ordered behaviour remains the first driver. The snapshot-ordered driver is added as a second front end over shared inheritance, physics execution, output, metadata, and validation services.

The purpose is not to rewrite Mimic or introduce new physics. The purpose is to separate driver-specific ordering and buffering from the shared physics-agnostic core so Mimic can support per-history physics and snapshot-synchronous methods without duplicating scientific behaviour.

---

## Motivation

The single structural fact that gates Mimic's method coverage is that the core processes one FoF workspace at a time in depth-first tree order, with per-forest bounded memory. That ordering is ideal for per-history physics, but it structurally cannot express operations that need a whole snapshot's halo population co-resident: global abundance matching (true SHAM), HOD-style statistical population, a synchronous reionization radiation field, environment-dependent physics, and on-the-fly lightcone assembly. These methods are the established reason snapshot-synchronised codes exist (L-Galaxies, UniverseMachine, and EMERGE are all snapshot-ordered).

There is also a hard capacity motivation: **Shin-Uchuu cannot be processed tree-ordered at all.** Its Consistent-Trees output contains a percolation super-forest (forest `26551468179`) holding 33% of all tree roots — billions of halos that no per-forest memory model can load as a unit, and that also rules out the uchuutools forests-HDF5 packaging. Snapshot ordering, whose working set is one snapshot's population, is the only way Mimic runs this simulation. See `SHIN-UCHUU-CONVERSION-PLAN.md`.

Rather than bolt a global stage onto the tree driver, the input ordering, the driver, and the memory model become a single coherent choice: a tree-ordered file feeds the tree driver with per-forest memory; a snapshot-ordered file feeds the snapshot driver with per-snapshot memory, making a snapshot's population co-resident and global operations natural. Producing the snapshot ordering is the job of an external converter, not Mimic.

---

## Architectural Decisions

### Driver Selection

Mimic reads exactly one input ordering per run, declared in the input YAML:

```yaml
input:
  processing_order: tree_ordered      # or: snapshot_ordered
```

Startup validation must fail fast if the declared ordering, reader, and selected driver do not match. There is no internal auto-detection and no internal conversion. `input.tree_type` stays the reader-format selector; `input.processing_order` stays the driver selector.

### Adjacency Invariant (replaces the former phantom-insertion contract)

Snapshot-ordered input has **strictly adjacent links as a validated format invariant**: every non-null descendant link points exactly one snapshot forward, and therefore every progenitor of a snap-N halo lives at snap N−1. Nobody inserts phantom or bridge halos:

- Consistent-Trees already writes interpolated phantom halos into its tree data, so all ctrees-derived input (Shin-Uchuu, micro-Uchuu) is adjacent by construction. The tree driver already processes these phantoms today as ordinary halos with no special treatment — the snapshot driver inherits the same property for free.
- The converter asserts adjacency and aborts on violation; for ctrees input a violation means corrupt data, not a policy choice. The snapshot reader re-validates cheaply at load.
- Gap-ful sources (L-Halo trees skip snapshots) are simply never converted. The identity fixture is `micro-uchuu-ascii` (local, already a registered package), where identity can be exact because phantoms are native. If a gap-ful source ever genuinely needs snapshot conversion, phantom synthesis becomes that converter front-end's problem, with documented interpolation science and no identity claim against the tree driver.

Consequence for Mimic: the snapshot driver keeps exactly **two generations** of processed state (previous and current snapshot). No lookback window, no per-halo gap handling, and memory bounds that scale with one snapshot regardless of simulation size.

### Shared Engine

Both drivers call the same shared services:

- **Inheritance and tracking service** (`src/core/inheritance.c`): turns already-processed progenitor galaxies plus descendant halo properties into the inherited FoF workspace. It owns type transitions, orphan handling, infall capture, merger-clock handling, accumulator reset, and local central selection.
- **Physics execution engine** (`execute_module_pipeline` in `src/core/module_registry.c`): format-neutral module execution over `(ctx, halos, ngal)` using the current configured module lifecycle.
- **Output-buffer marshalling** (`marshal_workspace_to_output_buffer` in `src/core/output_buffer.c`): driver-neutral workspace → output-buffer transfer.
- **Core services:** configuration, units, cosmology/time tables, memory, logging, module registry, generated property metadata, HDF5 writers, and output provenance.

### Driver Responsibilities

| Responsibility | Tree driver | Snapshot driver |
|---|---|---|
| Input ordering | Forests stored contiguously across snapshots | Halos grouped by snapshot with adjacent-snapshot links |
| Traversal | Depth-first per forest | Increasing-time snapshot loop |
| Progenitor lookup | Tree links within the current forest | `FirstProgenitor`/`NextProgenitor` links into the previous snapshot slab |
| Working set | One forest | Current input slab + two generations of processed state |
| Output buffering | Per forest/tree-compatible buffer | Per snapshot buffer, written per snapshot |
| Output formats | Binary and HDF5 | HDF5 only (see Output Contract) |
| Galaxy memory | Per-unit pool, bulk reset per forest | Two pools, ping-pong bulk reset per snapshot |
| Natural methods | Per-history physics | Global ranking, HOD, environment, radiation-field, and lightcone workflows |

### Module ABI Stability

The physics-module ABI is frozen for this work. Do not change `process(struct ModuleContext *ctx, struct Halo *halos, int ngal)`, the `Module` registration contract, or the YAML-to-C property and metadata generation contracts.

### Reader Interface

The snapshot reader gets its own small `struct SnapshotReader` vtable and registry, not a widening of `struct TreeReader`. The tree vtable is deeply partition/unit-shaped (14 hooks: partition enumeration, per-file paths, LPT costs, unit loading); the snapshot shape is open-run → enumerate snapshots → load slab → close. Two disjoint hook sets in one struct would defeat the `REQUIRE_READER_HOOK` fail-fast checks. `input.tree_type` still selects the reader (e.g. `snapshot_hdf5`); the existing `processing_order` validation routes each `tree_type` to exactly one driver, so lookup consults the registry matching the configured order.

### Determinism and Galaxy Identity

Cross-format identity depends on deterministic per-FoF physics. Future stochastic modules must seed from stable per-halo or per-FoF keys, never from a global RNG stream consumed in traversal order.

Identity is also an encoding contract. `UniqueGalaxyID = halonr + multiplier × (forestnr_global + 1)`, and the compile-time `TREE_MUL_FAC = 10⁹` **cannot represent the Shin-Uchuu super-forest**, whose within-forest halo ranks plausibly reach 5–9 billion. Decision D9: the multiplier becomes per-simulation metadata (`simulation_info.yaml`, default 10⁹, recorded in output provenance; the existing `galaxy_id.h` bounds validation enforces it at startup). Shin-Uchuu sets 10¹⁰, confirmed against the measured super-forest halo count from the conversion report. The snapshot input carries a dense run-scoped `ForestIndex` and `HaloRankInForest` per halo — the index enumeration replicates the ASCII reader's forest ordering and ranks are in the reference tree-driver order — so both drivers compute identical IDs from identical components with no runtime id mapping.

### Output Contract

Both drivers emit the same generated output schema and provenance model. Snapshot-ordered runs are **HDF5-only** and fail fast on `output_format: binary`: the binary writer's per-tree count header assumes per-tree contiguity, which snapshot-major emission over id-sorted slabs cannot honour (and a Shin-Uchuu header would carry 166M entries per file). The tree driver keeps both output formats unchanged. Snapshot runs use a single output partition and are not `--skip`-resumable in v1.

### Cross-Format Identity Gate

Byte-identical output files across drivers are impossible: the tree driver emits records in depth-first completion order, the snapshot driver snapshot-major. The gate is defined as: **for every output snapshot, the two runs produce the same set of `UniqueGalaxyID`s, and for each ID every output field is identical** (bitwise target; any relaxation must be an explicitly reviewed tolerance). Fixture: `micro-uchuu-ascii` read tree-ordered versus the same ASCII converted to snapshot-HDF5 and read snapshot-ordered. The test lives in the scientific tier.

Identity preconditions carried explicitly by the snapshot format (converter-owned, never reconstructed heuristically):

1. **Progenitor chain order** — `FirstProgenitor`/`NextProgenitor` links replicate the reference chain order, because `find_most_massive_progenitor` tie-breaks by chain position (SAGE parity) and gather order fixes workspace layout and merger processing order.
2. **FoF chain order** — `FirstHaloInFOFgroup`/`NextHaloInFOFgroup` replicate the reference sort, fixing subhalo slice order and central selection.
3. **Identity components** — dense `ForestIndex` + reference-ordered `HaloRankInForest` reproduce `UniqueGalaxyID` exactly.

These are the format-carried preconditions; the driver-side behaviours the snapshot driver must replicate (CentralMvir stamping, new-object sentinels, timestep derivation, central-ID propagation, marshal-time snapshot stamping) are enumerated in the Phase 5 driver parity checklist.

---

## Current Coupling (File Inventory)

| File | Role today | Disposition |
|---|---|---|
| `src/core/main.c` | Init → modules → `run_processing_driver()` → cleanup | Unchanged; dispatch seam already in place |
| `src/core/tree_driver.c` | Tree-ordered partition driver + driver dispatch | Dispatch gains the snapshot branch; tree lifecycle untouched |
| `src/core/build_model.c` | Tree traversal, tree-side gather, physics adapter, tree-owned output ranges | Stays tree-driver-specific |
| `src/core/inheritance.{c,h}` | Shared, format-neutral inheritance service (Phase 2) | Reused unchanged by the snapshot driver |
| `src/core/output_buffer.{c,h}` | Shared, format-neutral output marshalling (Phase 3) | Reused unchanged by the snapshot driver |
| `src/core/module_registry.c` | `execute_module_pipeline()` physics engine (Phase 1) | Reused unchanged |
| `src/core/galaxy_pool.{c,h}` | Per-unit chunked galaxy pool, bulk reset — currently a file-static singleton | Small refactor to an instanced handle API (`GalaxyPool*`); tree driver holds one instance, snapshot driver ping-pongs two. Discipline (stable pointers, bulk reset) unchanged |
| `src/io/tree/reader.h`, `registry.c` | Tree reader vtable + registry | Unchanged; a parallel `SnapshotReader` vtable + registry is added beside them |
| `src/io/output/binary.c` | Per-tree-header binary writer | Tree-driver-only; snapshot runs reject binary output |
| `src/io/output/master_hdf5.c`, `hdf5.c` | Master file enumerates partitions via `MimicConfig.reader` hooks; attrs write `Ntrees`/`TreeHalosPerSnap` from tree globals | Phase 5 adds a driver-neutral output partition/provenance seam; snapshot runs write per-snapshot counts and no per-tree table |
| `src/core/read_parameter_file.c`, `init.c` | Config + init; `processing_order` validation | Extend: `snapshot_hdf5` reader wiring, HDF5-only output check, per-simulation identity multiplier |
| `src/include/constants.h`, `galaxy_id.h` | `TREE_MUL_FAC` and ID encoding/validation | Multiplier moves to per-simulation metadata (D9); encoder/validators unchanged in form |
| *(new)* `src/core/snapshot_driver.c` | Snapshot loop, gather, context setup, per-snapshot output | Phase 5 |
| *(new)* `src/io/snapshot/…` | `snapshot_hdf5` reader | Phase 4 |

New driver and reader internals use `int64_t` for slab indices, counts, and offset arithmetic: peak slabs are 315M halos, and `int` state like `Ntrees`/`NumProcessedHalos` is a tree-driver idiom that does not carry over.

---

## Migration Plan

### Phases 0–3 — DONE (v1.0, verified)

Full historical detail lives in the v1.0-era revision of this plan (git history) and in the archived review. What matters going forward:

- **Phase 0** landed `input.processing_order`, `run_processing_driver()` dispatch, `run_tree_driver()` isolation, per-reader `processing_order` declarations, and fail-fast on `snapshot_ordered`.
- **Phase 1** landed `execute_module_pipeline(ctx, halos, ngal)` as the shared physics engine, reading phase configuration from `ctx->params`, with substep timing an engine concern. `process_halo_evolution()` is a thin tree-driver adapter.
- **Phase 2** landed `inherit_descendant_halos(workspace, start, capacity, descendant, progenitors, nprogenitors)` with zero references to tree arrays, config, or time globals (enforce in review). The gather/inherit contract is `InheritanceDescendant` (precomputed identity/time/virial doubles + generated `HaloInitPayload`), and `InheritanceProgenitorGalaxy` (source pointer + `source_time` + `is_main_branch`). New-object init is metadata-generated and format-neutral (`init_halo_from_payload`, `init_galaxy_defaults`, `reset_galaxy_snapshot_accumulators`); the tree-side payload populator (`populate_halo_payload_from_tree.inc`) is the only tree-coupled init code. The snapshot driver writes its own gather and a generated `populate_halo_payload_from_snapshot.inc` and reuses the rest unchanged.
- **Phase 3** landed `marshal_workspace_to_output_buffer(workspace, buffer, segments, nsegments)`: property-agnostic struct copy, Type-3 galaxy-pointer clearing, snapshot stamping, segment range recording. The driver owns segments and any back-references (the tree driver copies ranges into `HaloAux`).
- **Galaxy pool learning (corrected):** galaxies are pool-allocated with stable pointers and bulk reset per unit. The earlier idea that the snapshot driver needs per-galaxy reclamation (a free-list) is wrong: inheritance **deep-copies** every surviving galaxy into the current workspace, so once snapshot k is marshalled, generation k−1 is entirely dead. The snapshot driver uses **two pools, ping-pong bulk reset** — the existing discipline, twice. The current implementation is a file-static singleton (`galaxy_pool.c`), so Phase 5 includes a small refactor to an instanced handle API; the allocation discipline, stable-pointer guarantee, and `NULL`-means-no-galaxy sentinel carry over unchanged.

### Phase 4a: Format Contract + Converter + Fixtures

Freeze the snapshot-HDF5 format contract (schema, invariants, identity fields, validation rules) as a durable spec in `docs/` — the contract outlives both plans. **Done 2026-07-18:** [`docs/dev/SNAPSHOT-HDF5-FORMAT.md`](SNAPSHOT-HDF5-FORMAT.md) (`format_version = 1`). Then implement the converter and validate it on micro-Uchuu, all before any new Mimic code. This phase is specified in `SHIN-UCHUU-CONVERSION-PLAN.md`; its converter-side gate is the validation battery plus the topology cross-check against the existing `read_ctrees_ascii.c` reader by stable halo identity.

**Gate:** converter validation green on micro-Uchuu ASCII; snapshot-HDF5 fixtures exist for reader development.

### Phase 4b: Snapshot Reader

Add the `SnapshotReader` interface, registry, and the `snapshot_hdf5` implementation. A fixture simulation package (micro-Uchuu snapshot variant) declares the on-disk record in `halo_properties.yaml`; the generator produces `RawHalo`, accessors, and the payload populator from it, exactly as tree packages do. The reader exposes: run metadata (snapshot count, per-snapshot halo counts), slab loading for snapshot N, and link datasets (`FirstProgenitor`, `NextProgenitor`, FoF chains, identity columns). It validates the format version, adjacency invariant declaration, and link ranges at open/load.

**Gate:** reader unit tests pass against the Phase 4a fixtures; the tree path is untouched and green.

### Phase 5: Snapshot Driver + Identity Gate

Add `run_snapshot_driver()`:

1. **Snapshot loop** in increasing time order. For snapshot N: load slab N; for each FoF group (walk `FirstHaloInFOFgroup`/`NextHaloInFOFgroup` chains in slab order), gather progenitor galaxies per subhalo via `FirstProgenitor`/`NextProgenitor` into slab N−1's processed state (a per-slab `HaloAux` equivalent maps prev-slab halo → processed range), build `InheritanceDescendant` from the slab payload populator, call shared inheritance, run the shared physics engine with a snapshot-side context setup (slab-based virial/time quantities mirroring `setup_module_context`), marshal through the shared output buffer, and append to per-snapshot HDF5 output.
2. **State rotation:** processed state and galaxy pool for N−1 are bulk-dropped once N completes; N becomes the new previous generation. Requires the instanced galaxy-pool handle API (see file inventory).
3. **Identity:** `UniqueGalaxyID` computed directly from (`HaloRankInForest`, `ForestIndex`) with the per-simulation multiplier. Multiplier plumbing is part of this phase: `simulation_info.yaml` → `MimicConfig`, `galaxy_id.h` helpers take the configured value (replacing compile-time `TREE_MUL_FAC` use), recorded in output provenance. **`HaloNr` contract:** `struct Halo.HaloNr` stays an `int` local index — for the snapshot driver, the halo's slab index — because virial accessors and output helpers index the input array through it. `HaloRankInForest` (int64, up to ~10¹⁰) feeds *only* the `UniqueGalaxyID` encoding and must never be stored in `HaloNr`.
4. **Driver parity checklist** — behaviours the tree driver performs outside the shared seams that the snapshot driver must replicate exactly for the identity gate (from `build_model.c`, `inheritance.c`, `output_buffer.c`): stamp `CentralMvir` from the FoF-central catalog mass onto every workspace member **before physics**; new-object init sets `SnapNum = current − 1` and the `dT` sentinel (`Age[snap−1] − Age[snap]`, −1.0 at snap 0); `ctx->time_interval` and dynamic substep counts derive from the workspace's **pre-marshal** progenitor `SnapNum`; `UniqueCentralGalaxyID` is propagated from the FoF Type 0 central to all members before physics; output `SnapNum` is stamped at marshal time, not before.
5. **Driver-neutral output seam:** `write_master_file()` currently enumerates partitions through `MimicConfig.reader` hooks and the HDF5 attrs write `Ntrees`/`TreeHalosPerSnap` from tree globals. Generalise this narrow seam so snapshot runs (single partition) write per-snapshot counts and no per-tree table, without touching tree-run output.
6. **Memory:** measure `sizeof(struct Halo)` and `sizeof(struct GalaxyData)` during design and produce a real peak estimate for the 315M-halo z=0 slab (rough expectation ~300–450 GB for sage16-scale property sets against 512 GB — inside budget but tight; choose mitigations from data, e.g. freeing the input slab before marshalling, only if needed).

Snapshot-global operation hooks may be identified, but production global module contracts are follow-on work after the gate, owned by `MIMIC-SNAPSHOT-GLOBAL-MODULES-PLAN.md`.

**Gate:** all standard checks and tests pass, and the cross-format identity test (defined above) is green on micro-Uchuu with snapshot-global physics disabled — run under **both** timestep schemes (fixed `SubSteps` and `TimestepScheme: dynamic`) so `time_interval`/substep derivation mismatches cannot hide.

### Then: Shin-Uchuu Production

The 5.6 TB conversion runs exactly once, after the identity gate proves the format and driver. Production conversion, the `simulations/shin-uchuu/` package, and the end-to-end run are specified in `SHIN-UCHUU-CONVERSION-PLAN.md`.

### Sequencing Notes

The converter comes first because it is fully testable against micro-Uchuu ASCII with zero new Mimic code, and every later phase consumes its output. The expensive, slow step (Shin-Uchuu conversion and transfer) is deliberately last, behind the identity gate. The former Phase 6 (embedded engine) depends only on the Phase 1–2 seams and proceeds independently under its own plan; the former Phase 7 (distributed snapshot-global) follows the snapshot driver under its own plan.

---

## Standard Gate

```bash
make check-generated && make validate-modules
make tests-unit
make tests-integration
make tests-scientific
```

Long-running test output should be captured under `archive/test-logs/`, exit codes must be checked explicitly, and any non-zero exit code is a failure regardless of log text.

---

## Definition of Done

- `input.processing_order` selects a validated driver and fails fast on mismatches (done in v1.0).
- The tree driver remains byte-identical to the tagged v1.0 baseline throughout.
- The snapshot-HDF5 format contract is frozen as a durable spec, and the converter passes its validation battery and micro-Uchuu topology cross-check.
- The snapshot driver runs end to end, and the cross-format identity test (same `UniqueGalaxyID` set per snapshot, every field identical per ID) passes on micro-Uchuu and is part of the scientific tier.
- The shared inheritance service, physics engine, and output buffer are consumed by both drivers without modification.
- Shin-Uchuu converts, builds a clean `simulations/shin-uchuu/` package, and runs sage16 end to end with sane HMF/GSMF at z = 0, 1, 2.
- Snapshot-global physics contracts remain explicitly scoped as follow-on work (`MIMIC-SNAPSHOT-GLOBAL-MODULES-PLAN.md`).

---

## Risks And Mitigations

| Risk | Mitigation |
|---|---|
| Inheritance semantics drift | Already extracted and byte-identity-proven under the tree driver; identity gate re-checks end to end |
| Identity encoding overflow (super-forest) | Per-simulation multiplier (D9) set from measured counts in the conversion report; startup validation enforces bounds |
| Chain/order reconstruction diverges | Orders are converter-owned explicit links, validated round-trip; micro-Uchuu cross-check catches divergence before any Mimic code exists |
| Snapshot memory footprint at z=0 | Two-generation bound; measure struct sizes in Phase 5 design; mitigation options identified (P9 in the archived review) |
| Invalid snapshot inputs | Adjacency and link-range validation at conversion and again at read; abort, never repair |
| Persistent cross-snapshot state bugs | Cross-format identity is the direct acceptance check |
| Stochastic physics breaking determinism | Stable per-halo/per-FoF seeding rule stands as a standing constraint |

---

## Vision Review Timing

Do not update `docs/VISION.md` for this plan until the Phase 5 identity gate passes. At that point, review the vision narrowly for per-driver memory bounds, determinism as an invariant, and a pointer to the implemented dual-driver architecture.
