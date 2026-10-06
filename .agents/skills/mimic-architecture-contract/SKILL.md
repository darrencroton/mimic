---
name: mimic-architecture-contract
description: Load-bearing design decisions of the Mimic framework and WHY they exist; the invariants you must not break; the end-to-end data flow with real function names; memory ownership; the reader/driver seam; known weak points. Load this skill BEFORE touching anything under src/, before any structural or architectural decision (new driver, new dispatch mode, new package type, changing the pipeline, changing ID schemes, changing ownership of a buffer), or when asked "how does Mimic work end to end", "why is it designed this way", "can I change X in core", or "where does this data come from". This is the pre-flight contract, not a how-to.
---

# Mimic Architecture Contract

Mimic is a physics-agnostic semi-analytic galaxy-evolution framework: a C core that walks dark-matter merger trees and a set of interchangeable model packages (`models/<model>/`) that supply the physics. This skill states the design decisions that everything else leans on, the invariants that must survive any change, and the places the architecture is known to be weak. Read it before you edit `src/` or propose a structural change; it will stop you from breaking a contract you did not know existed.

## When to use / when NOT to use

Use this skill for: understanding the end-to-end data flow, checking whether a planned change violates an invariant, memory-ownership questions, reader/driver boundary questions, and evaluating structural proposals.

Do NOT use it for:
- Writing or modifying a physics module — see the `mimic-modules` skill.
- Property YAML schemas, precision policy, generated-code workflow — see the `mimic-properties` skill.
- Adding a simulation package or vertical reader (the hands-on steps) — see the `mimic-simulations-and-readers` skill.
- Which gates a change must pass before commit — see the `mimic-change-control` skill.
- Diagnosing a concrete failure — see the `mimic-debugging-playbook` skill.
- Historical incidents and why past decisions were reverted — see the `mimic-failure-archaeology` skill.

## First actions

Before editing anything under `src/` or proposing a structural change:

1. Read `docs/VISION.md` (the principles below are its operational form) and skim `docs/DEVELOPER-GUIDE.md` for the subsystem you are touching.
2. Structural work is planned under the owner's development pathway in `docs/dev/`; plans there are ephemeral and never a source of facts. `ls docs/dev/` to see whether one already covers the design space you are entering.
3. Trace your change against the data-flow section below: name the exact function where your change lands and which invariants (section 3) it can reach.
4. Run `make info` to confirm the build configuration you will test against, and pick ONE `MODEL`/`SIMULATION` pair for every command in the task (defaults: `sage16` + `mini-millennium`).
5. If your change touches a file under any `generated/` directory, stop — edit the YAML metadata or the generator and run `make generate` instead (see the `mimic-properties` skill).

## 1. The seven principles, operationally

Each principle from `docs/VISION.md`, restated as what it forbids. These are load-bearing: violating one is an architectural regression even if all tests pass.

1. **Physics-agnostic core.** `src/core/`, `src/io/`, `src/util/`, and `src/module_system/` know nothing about galaxies' physics: no physics function names, no `#include` of model code, no model-specific constants. *This forbids you from* adding any physics-aware branch, name, or include to core — physics enters only through the module vtable and generated registration.
2. **Runtime modularity within one compiled MODEL+SIMULATION pair.** Which modules run, in which phases, is decided by the run YAML at runtime; but one binary compiles exactly one model package and one simulation package. *This forbids you from* compiling two models into one binary, mixing modules across model packages at runtime, or adding compile-time physics switches — to mix physics, create a new model package.
3. **Metadata as structural truth.** Property structs, init/output code, HDF5 metadata, unit conversion, and the binary output schema are all generated from three YAML sets (`src/core/core_properties.yaml`, `simulations/<s>/halo_properties.yaml`, `models/<m>/model_properties.yaml`) by `make generate`. *This forbids you from* hand-editing anything under a `generated/` directory or adding a struct field outside the YAML — the YAML is the source; C is output.
4. **One coherent processing model.** There is one tree traversal with three FoF dispatch modes inside it (`PROCESSING_MODE_FULL_HALO`, `PROCESSING_MODE_PER_EVENT`, `PROCESSING_MODE_BY_GALAXY`), not three algorithms, plus one additive snapshot scope (`PROCESSING_MODE_SNAPSHOT`, horizontal driver only, run once per snapshot over the borrowed whole population after the FoF sweep). The snapshot scope adds no traversal of its own; it may update galaxy properties but never topology, and it cannot emit or consume FoF events. The one way a module adds galaxy records is core's creation operation (`module_create_record()`, full-halo callbacks only): the rows join the workspace when the creating callback returns and marshal inside their host's segment, with core owning their defaults, identity, linkage and memory and the module their physics and lifetime. *This forbids you from* adding a module type that needs its own traversal, its own loop over trees, or out-of-band access to halos the pipeline has not handed it.
5. **Bounded memory, explicit ownership.** Every allocation goes through the tracked allocator with a category, has a named owner, and a defined free point; leak checking runs at exit. *This forbids you from* using raw `malloc` in core/module code, allocating without a clear owner/free site, or letting a module retain pointers into per-tree buffers across trees. Created records are accounted like any row (their `GalaxyData` in the galaxy pool, the rows in the output buffer) and the creation scratch is run-persistent `MEM_HALOS`.
6. **Format-agnostic I/O and provenance-carrying output.** Readers hide the on-disk tree format behind one vtable; every output run carries its own schema and provenance (`metadata/` directory, HDF5 `RunProperties`). *This forbids you from* leaking format-specific logic past the reader boundary, or emitting output that cannot be interpreted without the current source checkout.
7. **Validation and fast failure.** Configuration errors (unknown module, unsupported mode, broken event contract, unknown YAML key) fail at startup with a clear message, never mid-run or silently. *This forbids you from* adding a config path that degrades silently, guesses a default for a missing physics parameter, or defers a detectable error past `module_system_init`.

## 2. Data flow, with real names

The single path every galaxy takes. Function names verified against the source; if any drifts, re-verify with the provenance commands at the end.

```text
run YAML
  → read_parameter_file()                  src/core/read_parameter_file.c — validates sections,
                                           rejects unknown keys (fast failure)
  → register_all_modules()                 src/module_system/generated/module_init.c (GENERATED)
  → module_system_init()                   builds the pipeline; FATAL on unknown module,
                                           ERROR on unsupported mode, ERROR on per-event module
                                           with no subscription, ERROR unless event producer is
                                           full-halo in the SAME phase as its consumers
  → run_processing_driver()                src/core/vertical_driver.c — the vertical driver
      per tree:
      → build_halo_tree()                  depth-first walk; MaxTreeDepth guard (default 500)
      → FoF workspace assembly + inheritance
          inherit_descendant_halos()       src/core/inheritance.c — deep-copies progenitor
                                           galaxies via the galaxy pool; applies Type
                                           transitions; resets snapshot accumulators
      → process_halo_evolution()          src/core/halo_evolution.c — shared driver adapter;
                                           the horizontal driver calls the same function;
                                           takes the driver's struct FoFWorkspace
          → execute_module_pipeline()      pre_timestep once → for each substep, each named
                                           phase in YAML order → post_timestep once.
                                           Within a phase: full-halo modules first (YAML
                                           order); events dispatched IMMEDIATELY to
                                           subscribed per-event consumers; then by-galaxy
                                           modules galaxy-major. Records a full-halo
                                           module creates (module_create_record()) are
                                           appended to the workspace when its callback
                                           returns, before the next module (its events
                                           were delivered as emitted, against committed
                                           rows only).
      → marshal_workspace_to_output_buffer()  each subhalo slice, then the records created
                                           on hosts in that slice
      (horizontal driver only, once per snapshot after every FoF group:
       execute_post_snapshot() runs modules.post_snapshot over the
       generation's processed buffer, before publication/release/output)
  → ProcessedHalos                         per-tree output buffer
  → binary / HDF5 writers                  src/io/output/
```

Two structures carry the state and are easy to confuse:

| Structure | Role | Lifetime |
|---|---|---|
| `struct FoFWorkspace` (src/core/fof_workspace.h) | Per-FoF-group processing scratch: the halos and galaxies modules actually operate on, with the borrowed galaxy pool, the published created-record identity space, and the record-creation bookkeeping (`base_count`, the owned `created_host` map). One descriptor per driver, its rows grown only by `fof_workspace_reserve()` (during the pipeline only by the record-creation commit, between callbacks); the four workspace functions (`process_halo_evolution`, `execute_module_pipeline`, `execute_phase`, `marshal_workspace_to_output_buffer`) take the descriptor, and `execute_phase` re-reads `ws->halos`/`ws->count` at every full-halo and by-galaxy callback | Rows: one FoF group at one snapshot, allocated per unit by `load_unit()`/`free_unit_halos()` (vertical) or once per run (horizontal). Descriptor: a run-lifetime static behind `vertical_fof_workspace()` (vertical, see `src/include/globals.h`) or part of the driver's per-run state (horizontal) |
| `ProcessedHalos` | Per-tree output buffer AND the source of already-processed progenitor state for inheritance | One tree |

`ProcessedHalos` is dual-purpose by design: the driver backs the output buffer with it, and `inherit_descendant_halos` reads progenitor galaxies back out of it. Any change to one role must preserve the other.

Galaxy types through inheritance: 0 = central, 1 = satellite, 2 = orphan (subhalo lost), 3 = consumed by merger (skipped, never output). On the 0→1 transition, `infallMvir`, `infallVvir`, and `infallVmax` are recorded. `make_orphan()` zeros `Mvir` and `Len` but preserves `Rvir` and `Vvir` — orphan dynamics still need them. Do not "clean up" that asymmetry; `module_create_record()` builds its staged rows through the same function (exported from `inheritance.h`), so created rows are Type 2 orphans of their host.

## 3. Invariants

Breaking any row below is a defect even if the build and quick tests stay green. The framework enforces some at runtime; the rest are contracts you must keep by hand.

| Invariant | Enforcement | Why it exists |
|---|---|---|
| Exactly one Type 0/1 central per subhalo slice of a FoF group | `FATAL` in `set_local_centrals` (src/core/inheritance.c) | Every physics module assumes a unique central to attach hot gas / infall to |
| Modules never call each other | Convention + physics-agnostic core; modules communicate ONLY via properties, events, and model-local `shared/` helpers | Direct calls create hidden ordering dependencies the phase system cannot see |
| `ModuleContext` is read-only to modules | Convention | It is shared across all modules in a phase; mutation would create cross-module aliasing |
| Property names unique across the three YAML sets (core, simulation, model) | Generator fails loudly | One flat namespace feeds one struct; collisions would silently shadow |
| `input.tree_type` = on-disk format; `input.processing_order` = driver. Never overload one with the other's meaning | Convention + registry design | The reader/driver seam (section 5) depends on these staying orthogonal |
| Tree rows: `UniqueGalaxyID = halonr + multiplier × (forestnr_global + 1)` (positive), `multiplier = MimicConfig.UniqueGalaxyIDMultiplier` (default `TREE_MUL_FAC = 1e9`), independent of MPI rank layout and file partitioning | `src/include/galaxy_id.h`, taken by both drivers | IDs must be reproducible across serial/MPI runs, file splits, and processing order; see [The identity multiplier](../../../docs/DEVELOPER-GUIDE.md#the-identity-multiplier) |
| Created records: `UniqueGalaxyID = -(1 + ordinal + 1024 × ((host HaloNr + row_offset) + rows_per_unit × unit))` (strictly negative, so it never meets the tree namespace), with `(unit, rows_per_unit)` the driver's published `struct RecordIdentitySpace` and `ordinal` the host's creation count in the FoF step; `row_offset` (the appended `RecordIdentitySpace` member) is the global row of the driver's local row 0 — the first global row of the range being swept (the task's range, or its current chunk) under a partitioned horizontal run, 0 for the vertical driver and an unpartitioned horizontal run (one task, one chunk) — so created ids are the same at every task count and chunk count while `HaloNr` stays the local row that indexes the slab view. Only a running `process_full_halo` callback creates; hosts are committed Type 0/1 rows present at pipeline start; the rows join the workspace when the callback returns and marshal after their host's slice. A run whose space does not fit int64 is never affected unless it creates | `module_create_record()` (src/core/module_registry.c) refuses every other caller, host, a 1025th record per host and a non-fitting space before staging anything; `mimic_encode_created_galaxy_id()` asserts its preconditions; the marshaller FATALs on a created row with no host segment | Created IDs must be unique run-wide and deterministic without a catalogue row, and created rows must ride inheritance like any orphan; see [Record Creation Contract](../../../docs/DEVELOPER-GUIDE.md#record-creation-contract) |
| Binary output is readable only via the run-local `metadata/output_schema.json` written alongside it | Design of the schema writer | Struct layout changes between checkouts; the run carries its own truth |
| Events flow only from full-halo producers to per-event consumers registered in the SAME phase | Checked at `module_system_init` | Immediate dispatch inside the phase loop; a cross-phase event would run against half-updated state |
| `modules.post_snapshot` (`process_snapshot` only) runs only under the horizontal driver, once per snapshot after the FoF coverage check and before publication, release and output, over exactly `cur->processed` (no Type 3, no raw slab, no older generation); it cannot emit or consume FoF events | Parser (entry shape; vertical rejection in `validate_and_postprocess`), `module_system_init` (mode family per phase, duplicates), `execute_post_snapshot()`'s dispatch-active flag checked by `module_emit_event()` | Writes must reach the next entry, descendants and the snapshot's own output through the shared galaxy pointers with no copy; snapshot topology is immutable |
| A distributed horizontal run (`NTask > 1`) decomposes by forest: each task holds only its forests' contiguous row range of every slab, links are rebased to local rows (abort if one leaves the range), and every whole-population quantity a `process_snapshot` module needs comes from the core collectives, reached by every task in the same order | `horizontal_partition_run()` and the rebase pass in `horizontal_driver.c`; `validate_post_snapshot_entries()` refuses a `snapshot_distribution: serial_only` module under `NTask > 1`; the callback-kind gate in `snapshot_collectives.c` | A forest is never split and no row crosses tasks, so the FoF sweep needs no communication; a collective skipped or reordered on one task hangs or corrupts the others |
| A chunked horizontal run (`input.forest_chunks: G > 1`, on any number of tasks) is the same decomposition one level down: each task's forest range is cut into `G` contiguous chunks and the snapshot loop runs once per chunk, so the chunk (range `task·G + c`) is the unit the rebase, identity offset, sizing and ceiling use. `modules.post_snapshot` with `G > 1` is refused at configuration, because no sweep holds a whole snapshot | `horizontal_sweep_chunk()` and `horizontal_partition_cut()`; the rejection in `validate_and_postprocess()` | `rows_per_unit` stays the global largest slab and `row_offset` is the chunk's first row, so created ids do not move with `G`; a forest is never split, so the largest forest's share of the widest slab is a floor no `(NTask, G)` passes |
| Galaxy pool is bulk-reset once per processing unit — once per tree for the vertical driver; for the horizontal driver, one pool per retained snapshot generation, bulk-reset when that generation is released at its retention horizon and recycled through a spare stack | Instanced `struct GalaxyPool *` API (`src/core/galaxy_pool.h`) | Per-galaxy frees would be slow and leak-prone; nothing may hold pool pointers across units |
| Snapshot accumulators (`init_repeat: true` properties) reset once per SNAPSHOT, at inheritance time | `reset_galaxy_snapshot_accumulators()` called from src/core/inheritance.c | Accumulators (e.g. SFR sums) integrate across all substeps within a snapshot. NOTE: `docs/DEVELOPER-GUIDE.md` says "each substep" — the code is the truth; the doc wording is imprecise. See the `mimic-properties` skill |

## 4. Memory ownership map

All allocation goes through `mymalloc_cat` / `myrealloc_cat` / `myfree` (src/util/) with a category; `check_memory_leaks()` runs at exit. Owners:

| Data | Category | Owner / free point |
|---|---|---|
| Input tree halos (raw reader output) | `MEM_TREES` | Vertical driver: freed per tree/partition. Horizontal: a reader-owned slab per snapshot (plus reader-owned `ForestIndex`/`HaloRankInForest` and, for version 3, target-snapshot and `SourceHaloID` arrays), held in the driver's retention pool and released through `release_slab` at the generation's horizon |
| Horizontal retained generation (raw slab, aux, processed output buffer, galaxy pool) | several | The horizontal driver alone, in a pool keyed by snapshot number; released once the snapshot at its retention horizon (largest `DescendantSnapshot` its halos name) is processed, and on the failure path by an `atexit` handler. The reader holds no retention state |
| `HaloAux`, FoF workspace rows (`struct FoFWorkspace.halos`) and its `created_host` map, `ProcessedHalos` | `MEM_HALOS` | Driver, freed per tree (the horizontal driver's workspace descriptor lives for the run and is released by `fof_workspace_destroy()` at teardown) |
| Record-creation scratch (staging blocks, staged-host map, per-host ordinals) | `MEM_HALOS` | `module_registry.c`; run-persistent, grow-to-high-water, allocated only once a record is created; released at driver teardown by `module_release_record_creation_scratch()` (vertical through `free_vertical_driver_scratch()`, horizontal in `horizontal_teardown()`) |
| `GalaxyData` | pool-managed | Galaxy pool; bulk-reset per tree, never individually freed |
| Module-private allocations | module's choice of category | The module itself — allocate in `init()`/`process()`, free in `cleanup()`; nothing outlives `cleanup()` |
| Per-task slab rows and partition tables (distributed horizontal run) | `MEM_TREES` slab; `MEM_HALOS` tables | Each task owns only its row range of every retained generation, so the retention accounting and `retention_memory_ceiling_mb` are per task; `forest_cuts` and `row_cuts` are created by `horizontal_partition_create()` on every task (task 0 fills them, `MPI_Bcast` shares them) and the per-forest weights are task 0's alone, freed before any generation is sized |
| Per-chunk retention and output partitions (chunked horizontal run) | retained generations in `MEM_TREES`/`MEM_HALOS`; the output registry is static | Each chunk's sweep owns only its rows of the retained generations and releases all of them before the next chunk; the spare galaxy pools, the FoF workspace and scratch carry over (a released pool keeps its capacity, so spare capacity counts in later chunks). `forest_cuts`/`row_cuts` hold `ntask·nchunk + 1` and `snapshot_count × (ntask·nchunk + 1)` entries. An output partition lives from the chunk that creates it to the chunk that finalises it (`first_visit`/`last_visit` of `horizontal_write_output()`, reopened read-write between), and the cleanup registry is an allocation-free table of one slot per requested output snapshot, so a failure removes every partition not yet final; the master's path is armed on task 0 only |
| Snapshot-callback scratch | module's choice of category | The module, like any module-private allocation (freed before `process_snapshot()` returns or in `cleanup()`); outside the horizontal retention-pool accounting and `input.retention_memory_ceiling_mb`, and bounded by nothing in the core |

`GalaxyData` is pool-managed through an instanced handle API (`struct GalaxyPool *`, `galaxy_pool_create()`/`galaxy_pool_alloc()`/`galaxy_pool_reset()`/`galaxy_pool_destroy()` in `src/core/galaxy_pool.h`), not a file-static singleton: `inherit_descendant_halos()` takes the pool handle explicitly. The vertical driver holds one instance, bulk-reset per tree; the horizontal driver holds one per retained snapshot generation, so every generation a later snapshot can still name as a progenitor keeps valid galaxies while the current one is built from them. A released generation's pool is bulk-reset and returned to a spare stack sized up front, so release never allocates. Adjacent input (`links_adjacent == 1`, every version 2 dataset) never holds more than two generations; gapped version 3 input can hold more, up to its longest descendant span plus one (mini-Millennium at most three, because its longest descendant span is 2). Each generation's resident bytes are computed before allocation (slab term from the reader-published `slab_row_bytes`, the rest from struct widths) and checked against the optional `input.retention_memory_ceiling_mb`; see `docs/DEVELOPER-GUIDE.md` → "The Horizontal Driver".

Two ownership facts are load-bearing and non-obvious:

- **`ProcessedHalos` must be growable.** Orphan galaxies (Type 2) emit one output record per snapshot they survive, so output size scales with simulation depth (number of snapshots), not with input tree size. Any "preallocate from input halo count" refactor is wrong by construction.
- **`OutputBuffer.halos` must be tracked heap** (`mymalloc_cat`/`myrealloc_cat`). It grows via realloc when orphans accumulate; backing it with a stack or fixed array is fatal on growth, not merely slow.

## 5. The reader/driver seam

Readers and drivers are deliberately independent axes:

- **Reader** (`input.tree_type`): how trees are stored on disk. Forest-ordered formats sit behind the `struct VerticalReader` vtable in `src/io/vertical/reader.h` (13 function-pointer hooks), registered in `src/io/vertical/registry.c` (`lhalo_binary`, `lhalo_hdf5`, `consistent_trees_ascii`, `consistent_trees_hdf5`). Each declares a partition model: `PARTITION_PER_FILE` (work unit = input file) or `PARTITION_ENUMERATED` (reader enumerates forests as work units).
- **Horizontal readers are a second family, behind a second registry.** `struct HorizontalReader` (`src/io/horizontal/reader.h`) is a separate small vtable — `open_run(options, info)`, `close_run`, `snapshot_halo_count`, `load_slab(snapnum, row_lo, row_hi, slab)`, `release_slab`, `scan_forest_index(snapnum, visitor, user)` — registered in `src/io/horizontal/registry.c` (`horizontal_hdf5`, which reads horizontal-HDF5 `format_version` 2 and 3, dispatching per file; version 2's validation path is unchanged). It is deliberately NOT a widening of `struct VerticalReader`: the tree hooks are partition/unit-shaped, and two disjoint hook sets in one struct would defeat the `REQUIRE_READER_HOOK` fail-fast. The distribution hooks (`open_run` options and `source_format`, ranged `load_slab` with `row_offset`, `scan_forest_index`) keep the reader MPI-free and partition-agnostic; their contract is in `docs/DEVELOPER-GUIDE.md` → "The horizontal reader interface". Horizontal readers have no partitions and no units; the working set is the slabs the driver retains, with `int64_t` counts and indices throughout. Links and halo numbers are int64 across the input/driver seam (generated link accessors return `int64_t`; `struct Halo.HaloNr` is `long long`); vertical packages keep `int` link storage. For version 3 a progenitor link resolves through its target-snapshot column into the retained generation it names — never assume N−1 — and every field a version 3 package declares must match the file's `/schema` (a one-way rule: undeclared `/schema` fields are validated and ignored); because declared units are the source format's own, there is one simulation package per (simulation, source format).
- **One key, two registries.** `input.tree_type` still resolves at a single site (`parse_input_section`, `src/core/read_parameter_file.c`), which tries `vertical_reader_lookup()` and then `horizontal_reader_lookup()`. The name sets are disjoint, so the order fixes only which registry answers first. Exactly one of `MimicConfig.vertical_reader` / `MimicConfig.horizontal_reader` is non-`NULL` afterwards, and `TreeExtension` is set only for vertical readers — so `MimicConfig.vertical_reader` is legitimately `NULL` for a horizontal configuration. Do not add a third resolution site, and do not resolve on `processing_order`.
- **Driver** (`input.processing_order`): the order halos are processed. Both accepted values now have a live driver: `vertical` (`run_processing_driver` in src/core/vertical_driver.c, dispatching to `run_vertical_driver()`) and `horizontal` (dispatching to `run_horizontal_driver()` in `src/core/horizontal_driver.c` — weak point W1, below, is closed). Startup validation compares the resolved reader's declared `processing_order` against the configured one, whichever registry answered.

Only three points outside a reader observe partitioning, and any new **forest-ordered** reader on the vertical driver must satisfy exactly these and nothing more: (1) the unique-ID forest offsets (global forest numbering feeding `UniqueGalaxyID`), (2) the per-file work-unit count scan used for file distribution (the same scan folds each partition's largest unit into the created-record identity space through the `max_partition_unit_halos` hook), (3) the HDF5 master file's external-link layout. Everything else must go through the vtable.

Horizontal input is a different shape, so do not read that list as a checklist for the horizontal driver. Horizontal readers have no partitions and no units, so **(2) does not apply at all** — there is no per-file work-unit scan to carry over, and inventing pseudo-partitions to satisfy it would be a mistake. Of the three, only (1) is genuinely shared: identity must still come out of the same global forest numbering, which the horizontal format carries per halo as `ForestIndex` and `HaloRankInForest`. (3) has been **generalised**: the master-file and provenance writers no longer enumerate partitions through `MimicConfig.vertical_reader` hooks directly. They call `struct OutputPartitionSource get_output_partition_source(void)` (`src/io/output/util.h`), which the vertical driver populates from its reader's partition hooks and the horizontal driver from its own partition source: `MimicConfig.NOUT` partitions, one per requested output snapshot (`NOUT × NTask` under a distributed run, with `partition_task(p)` naming each one's task), output id = the snapshot number, each writing per-snapshot counts and no per-tree table. No file under `src/io/output/` reads `MimicConfig.vertical_reader` any more.

**The input view.** Below the reader boundary, the generated `mimic_tree_get_*` accessors, the virial helpers (`src/core/virial.c`), and `prepare_halo_for_output()` (`src/io/output/util.c`) all take an explicit `struct HaloInputView { const struct RawHalo *halos; int64_t count; }` (`src/include/types.h`) instead of reading the global `InputTreeHalos`. Alongside the getters the generator emits `mimic_tree_set_<role>()` for the five link roles only (used by the distributed rebase; no range guard, so range-check before calling). The vertical driver builds its view from `InputTreeHalos` and the loaded unit's halo count; the horizontal driver builds its view from whichever retained raw slab a call site needs (the current snapshot's, or the generation a progenitor link names). This is the seam that lets one set of physics-coupled code serve both drivers with no duplicated arithmetic — do not add a driver-specific accessor family or a second generated payload populator.

**Collectives.** `src/core/snapshot_collectives.h` is the only way a snapshot module obtains a whole-population quantity under `NTask > 1`; every task reaches every collective in the same order, and they are the identity in a serial run. The list, return kinds, callback-kind gate and ordering rule are in `docs/DEVELOPER-GUIDE.md` → "Snapshot Callback Contract". Only the horizontal driver distributes; the vertical driver's MPI model (partitions strided across tasks) is unchanged.

Durable v1.0-verified fact both drivers rely on: `execute_module_pipeline`, `inherit_descendant_halos`, and `marshal_workspace_to_output_buffer` carry NO traversal-order assumptions — they operate on a FoF workspace plus progenitor state, however it was assembled. The horizontal driver reuses them as-is; do not add traversal assumptions to these three functions.

For adding a reader or simulation package, see the `mimic-simulations-and-readers` skill.

## 6. Known weak points

Stated plainly so you neither trip over them nor "fix" them casually. None of these is an invitation to a drive-by fix — structural changes go through section 7.

- **W1 — CLOSED 2026-08-12.** `horizontal` now has a live driver: `run_horizontal_driver()` (`src/core/horizontal_driver.c`), dispatched from `run_processing_driver()`'s `INPUT_PROCESSING_ORDER_HORIZONTAL` case. It calls `horizontal_reader_open_run()` before processing anything, so a horizontal run now proves its dataset readable — structure, headers, `scale_factor` agreement, physical-header agreement with configuration, and measured identity bounds — before any halo is processed, and `load_slab`'s link-range validation runs once per snapshot as the driver loads each slab. The HDF5 output writers no longer dereference `MimicConfig.vertical_reader` at all (see the input-view/output-partition-seam paragraph above); both were made reader-kind-neutral as part of this closure. See `docs/DEVELOPER-GUIDE.md` → "The Horizontal Driver" for the loop, the retained-generation pool (which replaced the original two-generation rotation on 2026-09-29, for version 3 gaps), and the parity checklist the cross-format identity gate verifies.
- **W2 — CLOSED 2026-08-12.** `UniqueGalaxyID` no longer overflows at super-forest scale by construction: every helper in `src/include/galaxy_id.h` takes the run's forest multiplier as an explicit `int64_t` parameter (`MimicConfig.UniqueGalaxyIDMultiplier`, default `TREE_MUL_FAC = 1e9`) instead of the hard-coded compile-time constant. Both processing orders honour a non-default value — the former vertical rejection is lifted — and the three Consistent-Trees forest-size guards check against the same configured value. HDF5 output records the value as the `int64` `RunProperties/UniqueGalaxyIDMultiplier` attribute in both per-file and master outputs.
- **W3 — `lhalo_hdf5` reader is registered but unused.** No shipped simulation package selects it; it has less real-world exercise than the other three readers. Treat it as less battle-tested.
- **W4 — Stale legacy generated files (closed for the `.inc` files 2026-08-14).** A few files under generated directories are written by NO current generator. The two the Makefile's `GENERATED_HEADERS` named — `init_halo_properties.inc` and `init_galaxy_properties.inc` — were verified orphaned on both ends, removed from `GENERATED_HEADERS`, and archived on 2026-08-13, with the full validation ladder re-run. `reset_galaxy_properties.inc` was verified the same way (no generator emits it, no source includes it, the Makefile never named it) and archived to `archive/orphaned-generated/` on 2026-08-14, with `check-generated` re-run. `tests/generated/module_sources.mk` remains on disk, referenced by nothing in the Makefile; removing it is a real cleanup but must be done deliberately (Makefile + generator + check-generated together), not in passing. See the `mimic-properties` skill.
- **W5 — Doc wording on accumulator reset.** `docs/DEVELOPER-GUIDE.md` says `init_repeat` fields reset "each substep"; the code resets once per snapshot at inheritance. Code is truth (section 3, last row).
- **W6 — `sham` abundance matching is a calibrated-target rank match, not an observational model.** `sham_rank_match` ranks a whole snapshot through `modules.post_snapshot` (horizontal driver; collective across tasks under MPI); its `pre_timestep` step only tracks peak proxies, retires carried Type 2 rows and resets the non-member flag, because no FoF workspace can see the whole snapshot. Its target is the Baldry et al. (2012) GAMA stellar mass function converted to the simulation's `h`, applied only to output snapshots inside `ShamTargetRedshiftMax`, with zero scatter, resolved-only candidates and no orphan mode. A first run on the real micro-Uchuu box recovers the target on the ranked population (`models/sham/README.md`); clustering, satellite fractions and the stellar-to-halo relation are not validated against observations, its fixture example is an incomplete synthetic volume, and no memory measurement exists beyond that box.
- **W7 — Plot profile `xlim`/`ylim` keys are silently ignored.** Shipped profiles set `axes.<plot>.xlim/ylim` lists but the reader consumes only `xmin/xmax/ymin/ymax` scalars. Details and workaround: see the `mimic-plots-and-analysis` skill.
- **W8 — Created-record identity is a scalar budget with two named ceilings.** `-(1 + ordinal + 1024 × host_key)` must fit int64: Millennium fits under both drivers (vertical about 7.5e15, horizontal about 1.2e12 against 9.2e18), but the vertical driver on Shin-Uchuu ASCII and on full Uchuu does not, so a run there logs the verdict at startup, is otherwise unaffected, and fails at its first `module_create_record()`. The escape is pair identity (source-halo ID plus an ordinal column); do not widen `MAX_CREATED_RECORDS_PER_HOST` or change the host key without it. Related limits that are by design, not defects: only full-halo callbacks create, an event cannot name a row created in the same callback (no event deferral), a module cannot tell its dispatch mode from `ModuleContext`, and a created row is a copy of its host's halo-side fields with default galaxy properties, so only `Pos`, `Vel` and the module's own properties carry information about it.
- **W9 — `hod` demonstrates the framework; it is not a calibrated mock.** `hod_populate` keeps every catalogue halo as scaffold (`HODGhost == 1`) beside the sample (`HODGhost == 0`), so a production-scale output carries the whole halo catalogue per output snapshot and consumers must filter; there is no output-time filter. Its published Zheng, Coil & Zehavi (2007) parameters assume a 200x mean-density halo mass and a different cosmology than the catalogue's `Mvir` and `h`.

## 7. Where structural proposals go

Any change that alters this contract — a new driver, a new dispatch mode, a new package type, a new ID scheme, buffer-ownership changes — is planned under the owner's development pathway in `docs/dev/`; plans there are ephemeral and never a source of facts (they are archived once done). Gates and review for the eventual implementation: the `mimic-change-control` skill.

## Provenance and maintenance

Written 2026-07-04 against Mimic v1.0 (tagged 2026-06-29); the reader/driver seam and weak points W1–W2 updated 2026-08-04 when the horizontal reader landed, and updated again 2026-08-12 when the horizontal driver landed and both W1 and W2 closed, and 2026-09-29 when the general horizontal runtime landed (version 3 reading, int64 indices, retained generations), and 2026-10-04 when record creation (`module_create_record()`), the `hod` package and the `sham_rank_match` replacement landed (W6, W8, W9). The principles and invariants are durable; the function names and weak points can drift. Re-verify before relying on specifics:

```bash
# Data-flow function names still exist where stated
grep -rn "run_processing_driver\|process_halo_evolution\|execute_module_pipeline" src/core/ src/module_system/ --include="*.c" -l
grep -n "inherit_descendant_halos\|make_orphan\|set_local_centrals\|reset_galaxy_snapshot_accumulators" src/core/inheritance.c
grep -rn "marshal_workspace_to_output_buffer" src/core/ -l

# ID scheme and multiplier; created-record identity and the creation gate
grep -rn "TREE_MUL_FAC" src/
grep -n "mimic_encode_created_galaxy_id\|mimic_created_record_space_fits" src/include/galaxy_id.h
grep -n "module_create_record\|RUNNING_CALLBACK_FULL_HALO" src/core/module_registry.c

# Reader vtable and partition models
grep -n "PARTITION_PER_FILE\|PARTITION_ENUMERATED" src/io/vertical/{reader.h,registry.c}

# Both drivers exist and dispatch from one seam (weak point W1 closed)
grep -n "run_horizontal_driver\|INPUT_PROCESSING_ORDER_HORIZONTAL" src/core/vertical_driver.c
# The second reader family and the two-registry resolution
sed -n '/^struct HorizontalReader {/,/^};/p' src/io/horizontal/reader.h
grep -n "vertical_reader_lookup\|horizontal_reader_lookup" src/core/read_parameter_file.c

# Stale generated files still present (weak point W4)
grep -n "GENERATED_HEADERS" Makefile

# Active structural plans
ls docs/dev/
```

If a re-verification command comes back empty or different, trust the repo, fix this skill, and note the drift.
