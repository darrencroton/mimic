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
- Adding a simulation package or tree reader (the hands-on steps) — see the `mimic-simulations-and-readers` skill.
- Which gates a change must pass before commit — see the `mimic-change-control` skill.
- Diagnosing a concrete failure — see the `mimic-debugging-playbook` skill.
- Historical incidents and why past decisions were reverted — see the `mimic-failure-archaeology` skill.

## First actions

Before editing anything under `src/` or proposing a structural change:

1. Read `docs/VISION.md` (the principles below are its operational form) and skim `docs/DEVELOPER-GUIDE.md` for the subsystem you are touching.
2. Check `docs/dev/MIMIC-DEVELOPMENT-PATHWAY.md` — an active plan may already own the design space you are entering; do not duplicate or contradict it.
3. Trace your change against the data-flow section below: name the exact function where your change lands and which invariants (section 3) it can reach.
4. Run `make info` to confirm the build configuration you will test against, and pick ONE `MODEL`/`SIMULATION` pair for every command in the task (defaults: `sage16` + `mini-millennium`).
5. If your change touches a file under any `generated/` directory, stop — edit the YAML metadata or the generator and run `make generate` instead (see the `mimic-properties` skill).

## 1. The seven principles, operationally

Each principle from `docs/VISION.md`, restated as what it forbids. These are load-bearing: violating one is an architectural regression even if all tests pass.

1. **Physics-agnostic core.** `src/core/`, `src/io/`, `src/util/`, and `src/module_system/` know nothing about galaxies' physics: no physics function names, no `#include` of model code, no model-specific constants. *This forbids you from* adding any physics-aware branch, name, or include to core — physics enters only through the module vtable and generated registration.
2. **Runtime modularity within one compiled MODEL+SIMULATION pair.** Which modules run, in which phases, is decided by the run YAML at runtime; but one binary compiles exactly one model package and one simulation package. *This forbids you from* compiling two models into one binary, mixing modules across model packages at runtime, or adding compile-time physics switches — to mix physics, create a new model package.
3. **Metadata as structural truth.** Property structs, init/output code, HDF5 metadata, unit conversion, and the binary output schema are all generated from three YAML sets (`src/core/core_properties.yaml`, `simulations/<s>/halo_properties.yaml`, `models/<m>/model_properties.yaml`) by `make generate`. *This forbids you from* hand-editing anything under a `generated/` directory or adding a struct field outside the YAML — the YAML is the source; C is output.
4. **One coherent processing model.** There is one tree traversal with three dispatch modes inside it (`PROCESSING_MODE_FULL_HALO`, `PROCESSING_MODE_PER_EVENT`, `PROCESSING_MODE_BY_GALAXY`), not three algorithms. *This forbids you from* adding a module type that needs its own traversal, its own loop over trees, or out-of-band access to halos the pipeline has not handed it.
5. **Bounded memory, explicit ownership.** Every allocation goes through the tracked allocator with a category, has a named owner, and a defined free point; leak checking runs at exit. *This forbids you from* using raw `malloc` in core/module code, allocating without a clear owner/free site, or letting a module retain pointers into per-tree buffers across trees.
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
  → run_processing_driver()                src/core/tree_driver.c — the tree-ordered driver
      per tree:
      → build_halo_tree()                  depth-first walk; MaxTreeDepth guard (default 500)
      → FoF workspace assembly + inheritance
          inherit_descendant_halos()       src/core/inheritance.c — deep-copies progenitor
                                           galaxies via the galaxy pool; applies Type
                                           transitions; resets snapshot accumulators
      → process_halo_evolution()
          → execute_module_pipeline()      pre_timestep once → for each substep, each named
                                           phase in YAML order → post_timestep once.
                                           Within a phase: full-halo modules first (YAML
                                           order); events dispatched IMMEDIATELY to
                                           subscribed per-event consumers; then by-galaxy
                                           modules galaxy-major.
      → marshal_workspace_to_output_buffer()
  → ProcessedHalos                         per-tree output buffer
  → binary / HDF5 writers                  src/io/output/
```

Two structures carry the state and are easy to confuse:

| Structure | Role | Lifetime |
|---|---|---|
| `FoFWorkspace` | Per-FoF-group processing scratch: the halos and galaxies modules actually operate on | One FoF group at one snapshot |
| `ProcessedHalos` | Per-tree output buffer AND the source of already-processed progenitor state for inheritance | One tree |

`ProcessedHalos` is dual-purpose by design: the driver backs the output buffer with it, and `inherit_descendant_halos` reads progenitor galaxies back out of it. Any change to one role must preserve the other.

Galaxy types through inheritance: 0 = central, 1 = satellite, 2 = orphan (subhalo lost), 3 = consumed by merger (skipped, never output). On the 0→1 transition, `infallMvir`, `infallVvir`, and `infallVmax` are recorded. `make_orphan()` zeros `Mvir` and `Len` but preserves `Rvir` and `Vvir` — orphan dynamics still need them. Do not "clean up" that asymmetry.

## 3. Invariants

Breaking any row below is a defect even if the build and quick tests stay green. The framework enforces some at runtime; the rest are contracts you must keep by hand.

| Invariant | Enforcement | Why it exists |
|---|---|---|
| Exactly one Type 0/1 central per subhalo slice of a FoF group | `FATAL` in `set_local_centrals` (src/core/inheritance.c) | Every physics module assumes a unique central to attach hot gas / infall to |
| Modules never call each other | Convention + physics-agnostic core; modules communicate ONLY via properties, events, and model-local `shared/` helpers | Direct calls create hidden ordering dependencies the phase system cannot see |
| `ModuleContext` is read-only to modules | Convention | It is shared across all modules in a phase; mutation would create cross-module aliasing |
| Property names unique across the three YAML sets (core, simulation, model) | Generator fails loudly | One flat namespace feeds one struct; collisions would silently shadow |
| `input.tree_type` = on-disk format; `input.processing_order` = driver. Never overload one with the other's meaning | Convention + registry design | The reader/driver seam (section 5) depends on these staying orthogonal |
| `UniqueGalaxyID = halonr + TREE_MUL_FAC × (forestnr_global + 1)` with `TREE_MUL_FAC = 1e9`, independent of MPI rank layout and file partitioning | Code in the driver/ID path | IDs must be reproducible across serial/MPI runs and file splits; see weak point W2 |
| Binary output is readable only via the run-local `metadata/output_schema.json` written alongside it | Design of the schema writer | Struct layout changes between checkouts; the run carries its own truth |
| Events flow only from full-halo producers to per-event consumers registered in the SAME phase | Checked at `module_system_init` | Immediate dispatch inside the phase loop; a cross-phase event would run against half-updated state |
| Galaxy pool is bulk-reset once per tree | `galaxy_pool` design | Per-galaxy frees would be slow and leak-prone; nothing may hold pool pointers across trees |
| Snapshot accumulators (`init_repeat: true` properties) reset once per SNAPSHOT, at inheritance time | `reset_galaxy_snapshot_accumulators()` called from src/core/inheritance.c | Accumulators (e.g. SFR sums) integrate across all substeps within a snapshot. NOTE: `docs/DEVELOPER-GUIDE.md` says "each substep" — the code is the truth; the doc wording is imprecise. See the `mimic-properties` skill |

## 4. Memory ownership map

All allocation goes through `mymalloc_cat` / `myrealloc_cat` / `myfree` (src/util/) with a category; `check_memory_leaks()` runs at exit. Owners:

| Data | Category | Owner / free point |
|---|---|---|
| Input tree halos (raw reader output) | `MEM_TREES` | Driver, freed per tree/partition |
| `HaloAux`, `FoFWorkspace`, `ProcessedHalos` | `MEM_HALOS` | Driver, freed per tree |
| `GalaxyData` | pool-managed | Galaxy pool; bulk-reset per tree, never individually freed |
| Module-private allocations | module's choice of category | The module itself — allocate in `init()`/`process()`, free in `cleanup()`; nothing outlives `cleanup()` |

Two ownership facts are load-bearing and non-obvious:

- **`ProcessedHalos` must be growable.** Orphan galaxies (Type 2) emit one output record per snapshot they survive, so output size scales with simulation depth (number of snapshots), not with input tree size. Any "preallocate from input halo count" refactor is wrong by construction.
- **`OutputBuffer.halos` must be tracked heap** (`mymalloc_cat`/`myrealloc_cat`). It grows via realloc when orphans accumulate; backing it with a stack or fixed array is fatal on growth, not merely slow.

## 5. The reader/driver seam

Readers and drivers are deliberately independent axes:

- **Reader** (`input.tree_type`): how trees are stored on disk. Forest-ordered formats sit behind the `struct TreeReader` vtable in `src/io/tree/reader.h` (12 function-pointer hooks), registered in `src/io/tree/registry.c` (`lhalo_binary`, `lhalo_hdf5`, `consistent_trees_ascii`, `consistent_trees_hdf5`). Each declares a partition model: `PARTITION_PER_FILE` (work unit = input file) or `PARTITION_ENUMERATED` (reader enumerates forests as work units).
- **Snapshot readers are a second family, behind a second registry.** `struct SnapshotReader` (`src/io/snapshot/reader.h`) is a separate small vtable — `open_run`, `close_run`, `snapshot_halo_count`, `load_slab`, `release_slab` — registered in `src/io/snapshot/registry.c` (`snapshot_hdf5`). It is deliberately NOT a widening of `struct TreeReader`: the tree hooks are partition/unit-shaped, and two disjoint hook sets in one struct would defeat the `REQUIRE_READER_HOOK` fail-fast. Snapshot readers have no partitions and no units; the working set is one snapshot's slab, with `int64_t` counts throughout.
- **One key, two registries.** `input.tree_type` still resolves at a single site (`parse_input_section`, `src/core/read_parameter_file.c`), which tries `tree_reader_lookup()` and then `snapshot_reader_lookup()`. The name sets are disjoint, so the order fixes only which registry answers first. Exactly one of `MimicConfig.reader` / `MimicConfig.snapshot_reader` is non-`NULL` afterwards, and `TreeExtension` is set only for tree readers — so `MimicConfig.reader` is legitimately `NULL` for a snapshot configuration. Do not add a third resolution site, and do not resolve on `processing_order`.
- **Driver** (`input.processing_order`): the order halos are processed. `tree_ordered` is the shipped driver (`run_processing_driver` in src/core/tree_driver.c); `snapshot_ordered` resolves and validates but has no driver (weak point W1). Startup validation compares the resolved reader's declared `processing_order` against the configured one, whichever registry answered.

Only three points outside a reader observe partitioning, and any new reader or driver must satisfy exactly these and nothing more: (1) the unique-ID forest offsets (global forest numbering feeding `UniqueGalaxyID`), (2) the per-file work-unit count scan used for file distribution, (3) the HDF5 master file's external-link layout. Everything else must go through the vtable.

Durable v1.0-verified fact future drivers rely on: `execute_module_pipeline`, `inherit_descendant_halos`, and `marshal_workspace_to_output_buffer` carry NO traversal-order assumptions — they operate on a FoF workspace plus progenitor state, however it was assembled. A snapshot-ordered driver can reuse them as-is; do not add traversal assumptions to these three functions.

For adding a reader or simulation package, see the `mimic-simulations-and-readers` skill.

## 6. Known weak points

Stated plainly so you neither trip over them nor "fix" them casually. None of these is an invitation to a drive-by fix — structural changes go through section 7.

- **W1 — `snapshot_ordered` has a reader, but no driver and therefore no production reader call.** `snapshot_hdf5` is registered behind `struct SnapshotReader`, and a `snapshot_hdf5` + `snapshot_ordered` configuration passes **configuration** validation (two-registry resolution, reader/order compatibility, the exact `tree_name` literal, the multiplier rules) and then FATALs at exactly one place — `run_processing_driver()` in `src/core/tree_driver.c`, "not implemented yet". **Do not reason from "the input has been validated".** The reader's dataset validation (`open_run`: structure, headers, exact `scale_factor` agreement with the a_list, measured identity bounds) and its link-range validation (`load_slab`) are implemented and unit-tested, but `snapshot_reader_open_run()` has **no caller in `src/`** — only the fixture unit tests under `simulations/micro-uchuu-snapshot/_tests/unit/`. The run path is `read_parameter_file()` → `init()` → `run_processing_driver()` (`src/core/main.c:372-373`, `:413`), which aborts before any snapshot file is opened, so a corrupt dataset and a valid one fail identically. This is the deliberate Phase 4b/5 split — the Phase 5 driver is `open_run`'s intended caller — and the fast-fail itself is deliberate (principle 7), not a bug. Its single location is a contract: do not add a second not-implemented point. Also true until the driver lands: the HDF5 output writers (`src/io/output/master_hdf5.c`, `metadata_hdf5.c`) still dereference `MimicConfig.reader` unguarded, reachable only past that FATAL, and must be made reader-kind-neutral as one change with the driver.
- **W2 — `UniqueGalaxyID` overflows at super-forest scale.** The fixed `TREE_MUL_FAC = 1e9` multiplier fails when a forest holds ≥1e9 halos (Shin-Uchuu-class inputs). Half-landed: `simulation.unique_galaxy_id_multiplier` is parsed and stored (`MimicConfig.UniqueGalaxyIDMultiplier`, default `TREE_MUL_FAC`), and a snapshot reader bounds-checks it against the dataset headers at `open_run`. But every helper in `src/include/galaxy_id.h` is still hard-coded to `TREE_MUL_FAC`, so a tree-ordered configuration declaring a non-default value is rejected at startup rather than silently misencoding. Replacing the encoder is planned — see `docs/dev/MIMIC-DEVELOPMENT-PATHWAY.md`. Do not invent a different ID scheme ad hoc.
- **W3 — `lhalo_hdf5` reader is registered but unused.** No shipped simulation package selects it; it has less real-world exercise than the other three readers. Treat it as less battle-tested.
- **W4 — Stale legacy generated files.** A few files under generated directories are written by NO current generator (`init_halo_properties.inc`, `init_galaxy_properties.inc`, `reset_galaxy_properties.inc`, `tests/generated/module_sources.mk`), and the Makefile's `GENERATED_HEADERS` still names two of them. Removing them is a real cleanup but must be done deliberately (Makefile + generator + check-generated together), not in passing. See the `mimic-properties` skill.
- **W5 — Doc wording on accumulator reset.** `docs/DEVELOPER-GUIDE.md` says `init_repeat` fields reset "each substep"; the code resets once per snapshot at inheritance. Code is truth (section 3, last row).
- **W6 — `sham` abundance matching is a local proxy.** The sham model ranks within locally available data, but true SHAM needs a global (whole-volume) ranking capability Mimic does not yet have. Its outputs are a proxy, not a reference SHAM.
- **W7 — Plot profile `xlim`/`ylim` keys are silently ignored.** Shipped profiles set `axes.<plot>.xlim/ylim` lists but the reader consumes only `xmin/xmax/ymin/ymax` scalars. Details and workaround: see the `mimic-plots-and-analysis` skill.

## 7. Where structural proposals go

Any change that alters this contract — a new driver, a new dispatch mode, a new package type, a new ID scheme, buffer-ownership changes — starts as a planning document in `docs/dev/`, with `docs/dev/MIMIC-DEVELOPMENT-PATHWAY.md` as the index of what is active versus a dormant requirements brief. Do not treat the contents of any live plan as settled instruction (plans are ephemeral and move to `archive/` when done); do check the pathway index so you neither collide with an active plan nor re-derive a rejected one. Gates and review for the eventual implementation: the `mimic-change-control` skill.

## Provenance and maintenance

Written 2026-07-04 against Mimic v1.0 (tagged 2026-06-29); the reader/driver seam and weak points W1–W2 updated 2026-08-04 when the snapshot reader landed. The principles and invariants are durable; the function names and weak points can drift. Re-verify before relying on specifics:

```bash
# Data-flow function names still exist where stated
grep -rn "run_processing_driver\|process_halo_evolution\|execute_module_pipeline" src/core/ src/module_system/ --include="*.c" -l
grep -n "inherit_descendant_halos\|make_orphan\|set_local_centrals\|reset_galaxy_snapshot_accumulators" src/core/inheritance.c
grep -rn "marshal_workspace_to_output_buffer" src/core/ -l

# ID scheme and multiplier
grep -rn "TREE_MUL_FAC" src/

# Reader vtable and partition models
grep -n "PARTITION_PER_FILE\|PARTITION_ENUMERATED" src/io/tree/reader.h src/io/tree/registry.c

# snapshot_ordered still fails fast at the driver, and only there (weak point W1)
grep -rn "snapshot_ordered\|not implemented yet" src/core/
# The second reader family and the two-registry resolution
sed -n '/^struct SnapshotReader {/,/^};/p' src/io/snapshot/reader.h
grep -n "tree_reader_lookup\|snapshot_reader_lookup" src/core/read_parameter_file.c

# Stale generated files still present (weak point W4)
grep -n "GENERATED_HEADERS" Makefile

# Active structural plans
ls docs/dev/ && head -40 docs/dev/MIMIC-DEVELOPMENT-PATHWAY.md
```

If a re-verification command comes back empty or different, trust the repo, fix this skill, and note the drift.
