# Mimic Developer Guide

**Practical guide to extending Mimic with physics modules, properties, tests, and generated metadata.**

This guide is for contributors and researchers modifying Mimic internals: writing a new physics module, adding properties, wiring up a new simulation, or working on the framework itself. It assumes you have already run Mimic successfully — if not, start with the [User Guide](USER-GUIDE.md). For the architectural principles and design rationale behind the structures described here, see [VISION.md](VISION.md). The shipped model packages are worked examples of everything in this guide: [models/sage16/](../models/sage16/README.md) is a mature production package, and [models/sham/](../models/sham/README.md) is a compact horizontal-only example with a whole-snapshot module.

---

## Table of Contents

1. [Quick Start](#quick-start)
2. [Architecture Overview](#architecture-overview)
3. [Creating Physics Modules](#creating-physics-modules)
4. [Processing Modes and Phases](#processing-modes-and-phases)
5. [Events](#events)
6. [Parameters](#parameters)
7. [Property System](#property-system)
8. [Adding a New Simulation](#adding-a-new-simulation)
9. [Adding a Vertical Reader](#adding-a-vertical-reader)
10. [Testing](#testing)
11. [Development Workflow](#development-workflow)
12. [Debugging](#debugging)
13. [Reference](#reference)
14. [Documentation Directory](#documentation-directory)

Common tasks:

- Adding a module: [Creating Physics Modules](#creating-physics-modules)
- Adding a model package: [Adding a New Model Package](#adding-a-new-model-package)
- Creating galaxy records from a module: [Record Creation Contract](#record-creation-contract)
- Choosing a processing mode: [Processing Modes and Phases](#processing-modes-and-phases)
- Adding a property: [Property System](#property-system)
- Working with units (different from Millennium): [Units and the Reference Basis](#units-and-the-reference-basis)
- Loading parameters: [Parameters](#parameters)
- Adding a simulation: [Adding a New Simulation](#adding-a-new-simulation)
- Adding a tree input format: [Adding a Vertical Reader](#adding-a-vertical-reader)
- Working on horizontal input: [Horizontal readers](#horizontal-readers)
- Working on the horizontal driver or the cross-format identity gate: [The Horizontal Driver](#the-horizontal-driver)
- Bringing a simulation to the horizontal driver: [Converting Trees to Horizontal Input](#converting-trees-to-horizontal-input)
- Wiring event-triggered modules: [Events](#events)
- Running tests: [Testing](#testing)
- Day-to-day development (including regenerating code): [Development Workflow](#development-workflow)

---

## Quick Start

This minimal example creates a directory module. Directory modules are the recommended production pattern because they declare supported processing modes, dependencies, tests, event contracts, and documentation in `module_info.yaml`. For simple prototypes, model packages also support standalone source modules; see [Standalone Modules](#standalone-modules).

```bash
mkdir -p models/<model>/modules/my_module/_tests
```

Create `models/<model>/modules/my_module/my_module.c`:

```c
#include "module_system/parameter_helpers.h"

static double my_efficiency;

int my_module_init(void)
{
    LOAD_AND_VALIDATE_RANGE_INCLUSIVE("MyEfficiency", my_efficiency,
                                      0.0, 1.0,
                                      "fractional efficiency");
    return 0;
}

int my_module_process(struct ModuleContext *ctx, struct Halo *halos, int ngal)
{
    if (ngal != 1) {
        ERROR_LOG("my_module expects process_by_galaxy mode (ngal=1), got %d", ngal);
        return -1;
    }

    struct GalaxyData *gal = halos[0].galaxy;
    if (gal == NULL) {
        return 0;
    }

    double dt = ctx->substep_dt;
    gal->ColdGas += (float)(my_efficiency * dt);
    return 0;
}

int my_module_cleanup(void)
{
    return 0;
}
```

`ctx->substep_dt` is the shared FoF substep duration. If a model package needs per-object timing semantics, follow its model-local helper pattern, such as SAGE's `mimic_object_substep_dt()` wrapper.

Create `models/<model>/modules/my_module/module_info.yaml`:

```yaml
module:
  name: my_module
  description: "Minimal example module"
  supported_processing_modes: [process_by_galaxy]

  dependencies:
    properties:
      - ColdGas
    parameters:
      - MyEfficiency

  tests:
    unit: []
    integration: []
    scientific: []

  docs:
    physics: README.md
```

Add the module to an input YAML file:

```yaml
modules:
  phases:
    galaxy_physics:
      - my_module: process_by_galaxy
  parameters:
    MyEfficiency: 0.5
```

Then regenerate, build, and run:

```bash
make validate-modules
make generate
make
./mimic models/sage16/input/sage16_mini-millennium.yaml
```

---

## Architecture Overview

Mimic separates core infrastructure from physics modules. Most scientific customisation should happen under `models/<model>/` and `simulations/<simulation>/`: new physics modules, galaxy properties, model-local helpers, run files, plot definitions, catalog halo properties, and simulation metadata all belong there. Core code under `src/` is shared infrastructure; changing it is appropriate for framework work such as new vertical readers, output writers, dispatch behavior, generated-code support, or memory/I/O changes, but it can affect every model package at once.

```text
Mimic application
  configuration and validation
  module registry and generated metadata
  physics-agnostic core
    tree loading
    FoF workspace construction
    shared inheritance
    phase dispatch
    output buffering
    output writing
  runtime physics modules
```

Key directories:

| Path | Purpose |
| --- | --- |
| `src/core/` | Main execution, configuration parsing, unit setup, tree processing, module dispatch |
| `src/io/` | Vertical readers and binary/HDF5 output writers |
| `models/<model>/modules/` | Runtime physics modules and module-owned tests for one model set |
| `src/module_system/` | Framework helpers, templates, generated module code, constants |
| `models/<model>/shared/` | Model-local helper APIs used by modules in that model set |
| `models/<model>/modules/_tests/` | Cross-module tests that do not belong to one module |
| `simulations/<simulation>/` | Simulation metadata, snapshot lists, catalog halo properties, tree data, and simulation-owned tests |
| `src/include/generated/` | Generated property structs and output helpers |
| `tests/` | Core unit, integration, scientific, framework, and generated test support |
| `plot/mimic-plot/` | Plotting, schema readers, and model-local plot discovery |

### FoF Workspaces

Mimic processes each snapshot interval by building FoF workspaces. A FoF workspace is an array of `struct Halo` entries for one FoF system: the Type 0 central and any Type 1/Type 2 satellites. The same workspace is passed to all modules in a phase.

Both drivers hold their workspace in one `struct FoFWorkspace` descriptor (`src/core/fof_workspace.h`): the rows (`halos`, `count`, `capacity`), the galaxy pool those rows' galaxies come from (borrowed, never owned), the created-record identity space the driver published, and the record-creation bookkeeping (`base_count`, the row count when the module pipeline started, and the owned `created_host` map giving each created row's host). The vertical driver owns one descriptor whose rows are sized per unit by `load_unit()` and released by `free_unit_halos()`, whose pool is `VerticalGalaxyPool` and whose identity `process_partition()` copies after each load; the horizontal driver owns one for the run, and before each FoF group sets its pool to the current generation's and copies its identity from the run's published space, of which only `unit` is per snapshot (`rows_per_unit`, `fits` and `units` are run-wide). Both grow the rows through the one `fof_workspace_reserve()` (factor `HALO_ARRAY_GROWTH_FACTOR`, at least `MIN_HALO_ARRAY_GROWTH` rows, capped at `MAX_HALO_ARRAY_SIZE`, `myrealloc_cat` in `MEM_HALOS`, new rows zeroed) and hand the descriptor itself to `process_halo_evolution()`, `execute_module_pipeline()`, `execute_phase()` and `marshal_workspace_to_output_buffer()`. `execute_phase()` re-reads `ws->halos` and `ws->count` at the start of every full-halo and by-galaxy callback, and the module `process()` ABI still receives a plain `(halos, ngal)` pair. Rows grow during the pipeline only when records a full-halo callback created are committed as it returns; that commit refreshes `ctx->central_galaxy` and the phase's event-dispatch view, the two caches of the rows' address and count the phase holds. See [Record Creation Contract](#record-creation-contract).

Galaxy types:

| Type | Meaning |
| --- | --- |
| 0 | FoF central galaxy with a resolved halo |
| 1 | Satellite galaxy with a resolved subhalo |
| 2 | Orphan satellite whose subhalo is no longer resolved |
| 3 | Internal consumed/invalid entry; skipped by by-galaxy dispatch and not output as a normal galaxy |

Core data structures:

| Structure | Role |
| --- | --- |
| `InputTreeHalos` / `struct RawHalo` | Immutable input merger tree data (`struct RawHalo` is generated from the simulation's `halo_properties.yaml`) |
| `struct FoFWorkspace` / `struct Halo` | Temporary processing workspace modified by modules, carried by its descriptor |
| `ProcessedHalos` / `struct Halo` | Vertical-driver output buffer and processed progenitor state |
| `OutputBufferSegment` | Driver-supplied range/snapshot metadata for shared output marshalling |
| `struct GalaxyData` | Generated galaxy/model property storage attached to `struct Halo` |
| `struct HaloOutput` | Generated output record written to binary/HDF5 |

Galaxy inheritance copies previous processed galaxy state into the current workspace, resets snapshot-scoped properties marked `init_repeat: true`, and updates halo properties from driver-supplied descendant data. After physics execution, the shared output-buffer marshaller copies surviving workspace entries into the driver-owned output buffer and frees Type 3 entries.

### Per-Tree Memory Lifecycle

Four arrays are allocated per unit and freed together by `free_unit_halos()` after output is written:

| Array | Category | Lifetime note |
| --- | --- | --- |
| `InputTreeHalos` | `MEM_TREES` | Fixed size: `InputTreeNHalos[treenr]` entries; immutable after load |
| `HaloAux` | `MEM_HALOS` | Fixed size: parallel to `InputTreeHalos`; tracks per-halo processing flags |
| FoF workspace rows (`struct FoFWorkspace.halos`) | `MEM_HALOS` | Not a global: the vertical driver's descriptor, reached through `vertical_fof_workspace()`. Seeded at the larger of `INITIAL_FOF_HALOS` and a tenth of `MaxProcessedHalos`, clamped to `MAX_HALO_ARRAY_SIZE`; grows through `fof_workspace_reserve()` as deep or wide FoF groups are encountered or records are created; released by `fof_workspace_destroy()`, which also frees the descriptor's `created_host` map (allocated only once a record is created) |
| `ProcessedHalos` | `MEM_HALOS` | Grows dynamically via `myrealloc_cat`; see below |

`GalaxyData` is not in this table because it is pool-managed: `galaxy_pool_alloc()` hands out slots from a chunk pool, and the whole pool is reset in one call rather than per-halo. The pool API takes an explicit `struct GalaxyPool *` handle (`src/core/galaxy_pool.h`) rather than reaching into file-static state, so each driver owns its own instance(s) with the same chunked-allocation, stable-pointer, and bulk-reset discipline: the vertical driver holds one pool, reset per tree; the horizontal driver holds one per retained snapshot generation, recycling released pools through a spare stack (see [The Horizontal Driver](#the-horizontal-driver)).

**Why `ProcessedHalos` must grow**

`ProcessedHalos` accumulates every marshalled output halo across all snapshot intervals for the entire tree. Each time a FoF group is processed, `marshal_workspace_to_output_buffer` appends the surviving workspace entries. The initial allocation is `MAXHALOFAC (5) × InputTreeNHalos`, but this is only an estimate. Orphan halos (Type 2) persist across snapshots and produce one new output record per snapshot they survive; in deep simulations with many snapshots (e.g., full Millennium at 64 snapshots), a single catalog subhalo that disappears early can generate dozens of output records. The actual count therefore scales with simulation depth and cannot be bounded by a fixed multiple of the tree input size.

`marshal_workspace_to_output_buffer` grows the buffer using the same factor / minimum / cap policy as `fof_workspace_reserve()` (`HALO_ARRAY_GROWTH_FACTOR`, `MIN_HALO_ARRAY_GROWTH`, `MAX_HALO_ARRAY_SIZE`). After each marshal call, `build_halo_tree` syncs the global `ProcessedHalos` pointer and `MaxProcessedHalos` back from the `OutputBuffer` struct. `myfree(ProcessedHalos)` in `free_unit_halos()` correctly frees the final (possibly grown) allocation because the custom allocator tracks the pointer through every `myrealloc_cat` call.

**OutputBuffer contract**

`marshal_workspace_to_output_buffer` takes a `struct OutputBuffer *`. The `halos` field must be a tracked heap allocation (`mymalloc_cat` or `myrealloc_cat`); passing a stack array will produce a fatal error on overflow because the allocator cannot find a stack address in its tracking table. After the call, callers must read back `buffer->halos` and `buffer->capacity` if they mirror those values in globals.

**Observing what these structures actually cost**

Because neither the output buffer's realised capacity nor the galaxy pool's occupancy can be derived from the input size — the paragraphs above explain why — both are measured rather than predicted. `src/util/run_profile.h` accumulates the run-level maximum of each measured term — plus peak RSS, which the operating system reports as a single high-water reading — and `print_run_memory_profile()` reports them once at run end, from rank 0, at every verbosity including `--quiet`:

| Term | Source | Notes |
| --- | --- | --- |
| `C` | output buffer capacity, noted wherever a buffer is seeded or grown | Overshoots `P`; the vertical driver seeds it at `MAXHALOFAC` (5) × the input-tree halo count before any growth |
| `P` | output buffer count, noted after each marshal | The population the record ceilings (`MAX_HALO_ARRAY_SIZE`) apply to, scoped to the live buffer: one snapshot under the horizontal driver, one tree under the vertical driver. Includes records modules created |
| `G` | `galaxy_pool_stats()`, harvested before each pool is destroyed | Peak concurrent galaxies, including created records' galaxies. Survives `galaxy_pool_reset()`, which zeroes the live count but not the high-water. Can exceed `P` because Type 3 galaxies are allocated and never emitted |
| Peak RSS | `getrusage(RUSAGE_SELF).ru_maxrss` | Bytes on macOS, kilobytes on Linux. The authoritative figure: the tracked allocator cannot see a `myrealloc_cat` whose block has to move, since it subtracts the old allocation before calling libc `realloc`, so old and new coexist invisibly to it |

Adding a term means adding a note call at the site that owns the quantity, not deriving it in the reporter. Sizes are reported in GB = 1e9 B, unlike the allocator's own MB = 1024² reports.

### Module Lifecycle

Every runtime module implements `init` and `cleanup`, plus one typed process callback for each callback family its `supported_processing_modes` advertise. All are named after the module:

```c
int module_name_init(void);
int module_name_cleanup(void);

/* FoF family: process_full_halo, process_per_event, process_by_galaxy */
int module_name_process(struct ModuleContext *ctx, struct Halo *halos, int ngal);

/* Snapshot family: process_snapshot */
int module_name_process_snapshot(const struct SnapshotContext *ctx, const struct Halo *halos,
                                 int64_t count);
```

A module that advertises only FoF modes implements `process` and no snapshot callback; a snapshot-only module implements `process_snapshot` and no `process`; a dual-mode module implements both. Generated registration binds each advertised callback and sets the other to `NULL`, and `module_registry_add()` aborts startup if an advertised family's callback is missing, the mode list is empty, or a mode is unknown. No module needs a stub for a family it does not support.

Lifecycle behavior:

- `init()` runs once at startup for each configured module, however many phases it appears in. Load and validate module parameters here.
- `process()` runs during configured FoF phases. Return non-zero after logging an `ERROR_LOG()` message if the module cannot continue. A `process_full_halo` callback is the only place a module may create galaxy records (`module_create_record()`); see the [Record Creation Contract](#record-creation-contract). `init()`, `cleanup()` and every other callback that tries it is refused.
- `process_snapshot()` runs once per snapshot from the `modules.post_snapshot` phase (horizontal driver only) and follows the [snapshot callback contract](#snapshot-callback-contract); it can never create records, because the snapshot scope is topology-immutable.
- `cleanup()` runs once during shutdown for each configured module. Free module-owned memory here.

Return conventions:

- `init()` non-zero: startup aborts.
- `process()` non-zero: Mimic exits with failure.
- `process_snapshot()` non-zero: Mimic exits with failure, naming the module, the `post_snapshot` phase, the snapshot and the return code.
- `cleanup()` non-zero: error is logged and cleanup continues for other modules.

### Snapshot Callback Contract

`process_snapshot` receives one whole snapshot population rather than one FoF workspace. Its `struct SnapshotContext` (in `src/core/module_interface.h`) carries only snapshot-level state:

| Field | Meaning |
| --- | --- |
| `snapshot_number` | Snapshot index |
| `redshift` | Snapshot redshift |
| `time` | Lookback time from z=0 at the snapshot, in internal units (the same units as `ModuleContext.time`) |
| `params` | Read-only pointer to `MimicConfig` |

There is deliberately no FoF central, active event, substep or common galaxy timestep: a snapshot population spans every FoF group and has none of these.

The population is borrowed:

- `halos` is the caller's current-generation storage, valid only for the duration of the call. Neither it nor any pointer reached through it may be retained after the callback returns.
- `count` may be zero, and `halos` may then be `NULL` or non-`NULL`. For a positive count, `halos` is non-`NULL` and every `halos[i].galaxy` is non-`NULL`.
- Index with `int64_t` over `[0, count)`; never narrow the count to `int`.
- The only permitted writes are galaxy properties through `halos[i].galaxy`. A callback must not reorder or resize the population, change any halo field or pointer, or interpret `CentralHalo` as an offset into this array: `CentralHalo` is a FoF-workspace-local index.

The `const` on the view is shallow. It makes a write to a halo field or to `halos[i].galaxy` itself a compile error, while `halos[i].galaxy` remains a mutable `struct GalaxyData *`. The project warning set does not include `-Wcast-qual`, so three violations compile silently and are forbidden by contract rather than by the compiler: casting away `const` to write a halo, retaining `halos` (or a pointer derived from it) in static or heap storage, and indexing `halos` by `CentralHalo`. Every review of a snapshot callback checks for these three patterns explicitly. `tests/integration/test_snapshot_module_schema.py` pins all of these compile-time behaviors.

Snapshot callbacks have no event contract: `events.emits` and `events.consumes` stay tied to the FoF modes `process_full_halo` and `process_per_event`, so a snapshot-only module cannot declare events, while a dual-mode module keeps valid FoF event declarations. At run time `execute_post_snapshot()` (`src/core/module_registry.c`) keeps a snapshot-dispatch-active flag that `module_emit_event()` checks before its no-active-phase shortcut, so an emission attempted from a snapshot callback returns `-1` rather than being accepted as if it came from a direct unit test; outside snapshot dispatch that shortcut is unchanged.

**When it runs.** The horizontal driver calls `execute_post_snapshot()` once per input snapshot, from `horizontal_run_post_snapshot()` in `run_horizontal_driver()`, after the FoF coverage check and before the generation is published for later progenitor lookup, before any earlier generation is released and before the snapshot is written. The context is rebuilt for each snapshot from its index, `MimicConfig.ZZ`, `Age` and `&MimicConfig`. The population is the generation's own output buffer, `cur->processed.halos[0:count]`: every surviving Type 0, 1 and 2 galaxy of the snapshot, across all its FoF groups, in marshalling order. The marshaller has already dropped Type 3 entries, and neither the raw slab nor any other retained generation is passed. Because the buffer's halos carry the same galaxy pointers inheritance later deep-copies from, a callback's writes reach the next `post_snapshot` entry, every descendant (adjacent or across a gap) and the snapshot's own output without any copy. Each entry is called in YAML order, including for an empty snapshot (count zero, whatever the buffer pointer holds) and for snapshots not selected for output; the dispatcher has no count shortcut. A non-zero return is fatal, and the driver's existing exit-time failure cleanup releases every retained generation and removes the master and any in-flight partition.

Memory a snapshot module allocates for its callback is outside the retention-pool accounting and `input.retention_memory_ceiling_mb`; it is the module's own, tracked through the usual allocator categories, and released by the module like any module-private allocation (before the callback returns, or in `cleanup()`). Nothing here bounds total process memory.

**Under MPI.** A distributed horizontal run (more than one task; see [Distributed operation](#distributed-operation)) gives each task only its own forests' rows of every snapshot, so `halos` and `count` describe that task's part of the population, not the whole box. Every task still calls every `post_snapshot` entry for every snapshot, including with `count == 0`. A module that needs a whole-population quantity obtains it through the **snapshot collectives** (`src/core/snapshot_collectives.h`, whose Doxygen is the normative statement), whose results are functions of the complete population (exactly so for ranks, integer sums, extents and agreement; a floating-point sum may differ in its last bits with the reduction order):

| Function | Returns | Purpose |
| --- | --- | --- |
| `module_snapshot_rank(ctx, keys, count, ranks)` | status | Exact global rank of each `struct SnapshotRankKey { double value; int64_t id; }`: descending `value`, ties by ascending `id`, so rank 0 is the largest value |
| `module_snapshot_sum_i64(ctx, values, n)` | status | Sum `n` `int64_t` values over all tasks, in place |
| `module_snapshot_sum_f64(ctx, values, n)` | status | Sum `n` doubles over all tasks, in place; identical on every task of one run, but may differ from a serial sum in the last bits (reduction order) |
| `module_snapshot_min_max_f64(ctx, minima, maxima, n)` | status | Element-wise minimum and maximum over all tasks, in place |
| `module_snapshot_any(ctx, flag)` | 1, 0 or -1 | Logical or of every task's flag, for agreeing on a failure before anyone writes |
| `module_snapshot_is_root_task()` | 1 or 0 | 1 on task 0 and in every serial run; use it to log a whole-population line once. Cannot fail, makes no collective call |

*Return kinds.* The first four are **status** functions (0 on success, -1 on error, the same on every task); `module_snapshot_any` is a **predicate** that returns -1 only when refused; `module_snapshot_is_root_task` has no error path. *Callback-kind gate.* Every function except `module_snapshot_is_root_task` is refused (`ERROR_LOG`, -1) while an `init`, full-halo, per-event, by-galaxy or `cleanup` callback is running, because those are not synchronised across tasks; they are allowed during snapshot dispatch and when no callback is running, so a unit test may call a `process_snapshot` entry point directly. *Ordering rule.* Every task reaches every collective, in the same order, with the same `n`: branch only on snapshot-level facts (`ctx`) or on a previous collective's result, never on a local count, and let a failure set a flag that `module_snapshot_any` agrees before any task writes, rather than returning early. NaN-free input is the caller's precondition for the two floating-point reductions (`module_snapshot_rank` rejects NaN itself); ids passed to `module_snapshot_rank` must be unique within the caller's keys (checked) and across tasks (`UniqueGalaxyID` gives this, because forests are disjoint). In a serial run or a non-MPI build every function is the identity (the rank is the position in the local sort), so a ported module's serial output is unchanged.

*Declaring it.* A module whose `process_snapshot` reaches every whole-population quantity through these functions declares `snapshot_distribution: collective` in `module_info.yaml`; the default, `serial_only`, means it assumes the complete population is resident. The key is accepted only for a module that advertises `process_snapshot`, and startup refuses (`ERROR_LOG`, naming the module and `NTask`) any `serial_only` module configured under `modules.post_snapshot` when `NTask > 1`. `sham_rank_match` and `hod_populate` are ported and declare `collective`; the framework fixtures stay `serial_only`.

### Record Creation Contract

A `process_full_halo` module can add galaxy records to the FoF group it is processing through one core function. Its Doxygen in `src/core/module_interface.h` is the normative statement of the contract; this section explains it and must agree with it:

```c
int module_create_record(struct ModuleContext *ctx, int host_index, struct Halo **row);
```

The created record is an ordinary workspace row from the moment its callback returns: every later module, phase, substep and by-galaxy pass of the same snapshot sees it, it is written with its host's subhalo, and the next snapshot inherits it like any orphan. The snapshot scope stays topology-immutable: only FoF callbacks create.

**API.** Legal only while a `process_full_halo` callback is running, the same gate as event emission. `host_index` must name a committed workspace row of Type 0 or 1 with a non-`NULL` galaxy that was present when the pipeline started: created rows cannot host. "Per host" throughout this contract means per Type 0/1 row as inheritance assembled it: `set_local_centrals()` (`src/core/inheritance.c`) writes each subhalo slice's Type 0/1 row index into every row of the slice as `CentralHalo`, so a host must have `CentralHalo == host_index`, and a row a module promoted to Type 1 is refused (per-host ordinals are kept per row, so two Type 0/1 rows of one `HaloNr` would otherwise draw the same created IDs). On success the function returns the new row's future logical index (at least the committed row count, the `ngal` the callback received) and sets `*row` to a staged row the caller may fill until its callback returns; the pointer stays valid across further creations in the same callback. It returns `-1` with an `ERROR_LOG` naming the module and the reason, sets `*row` to `NULL`, stages nothing and allocates no galaxy when it is called outside a running full-halo callback (from `init()`, `cleanup()`, a by-galaxy, per-event or snapshot callback, or no callback at all), when `host_index` is outside the committed rows or names a created row, when the host is not Type 0 or 1, has no galaxy or is not its slice's assembled Type 0/1 row (`CentralHalo` names another row), when the host already has `MAX_CREATED_RECORDS_PER_HOST` (1024) records in this FoF step, or when the run's identity space does not fit `int64`. A module propagates the `-1` as a non-zero `process()` return, which ends the run. Two core-invariant breaches abort with `FATAL_ERROR` instead of returning, because no module can cause or handle them: a workspace the core did not set up for creation (`base_count`, the created-host map or the pool), and a host whose `HaloNr` or the unit lies outside the identity space the driver published, or a space published as fitting that does not fit `int64` (the predicate is re-derived on every creation, so a mis-built descriptor never reaches the encoder's `assert()`). Where the vertical driver falls back to the forest multiplier as `rows_per_unit`, the bound on `HaloNr` is the reader's own forest-size guard (`consistent_trees_ascii` refuses a forest of the multiplier's size or more); otherwise the breach is a driver defect, distinct from the recoverable refusal of a run whose space does not fit `int64`.

**Initialisation.** The staged row is a full struct copy of the host passed through `make_orphan()` (`src/core/inheritance.c`): Type 2, `Mvir` and `Len` zero, `deltaMvir = -host Mvir`, `Rvir` and `Vvir` kept, and the infall fields set to the host's current `Mvir`, `Vvir` and `Vmax` for a Type 0 host or kept as the host's recorded infall values for a Type 1 host. Every other halo-side field starts as the host's: `HaloNr`, `CentralHalo`, `CentralMvir`, `UniqueCentralGalaxyID`, `SnapNum`, `dT`, `Pos` and `Vel`, and equally the catalogue fields such as `MostBoundID`, `Spin`, `Vmax` and `VelDisp`. Every created row therefore shares its host's `MostBoundID`, so a consumer must not match created rows to halos, or to each other, by `MostBoundID`. `UniqueGalaxyID` is the created ID below, and `galaxy` is a fresh slot from the workspace's galaxy pool initialised by `init_galaxy_defaults()`. The module then sets whatever physics it owns.

**Commit.** When the callback returns, before the next module runs and before the phase's post-callback safety dispatch of any undelivered event, `execute_phase()` appends every staged row in creation order to the end of the workspace through `fof_workspace_reserve()`, records each row's host in `ws->created_host`, grows `ws->count`, and refreshes `ctx->central_galaxy` (same index, possibly a new address) and the event-dispatch view. Events the callback emitted were already delivered as it emitted them, against the committed rows only. The next full-halo module receives the complete array, the same phase's by-galaxy pass visits the created rows, and later phases and substeps see them. No cached pointer is used across a growth, so a module must not keep a `halos` pointer from one callback to the next.

**Events.** An event cannot name a row created in the same callback: `module_emit_event()` validates both indices against the committed row count and rejects such an index with an `ERROR_LOG`. A later callback, in the same phase or a later one, may target created rows.

**Marshal.** `struct FoFWorkspace` carries `base_count`, set by `process_halo_evolution()` before the pipeline, and `created_host[row - base_count]`, the host of each row at or beyond it. `marshal_workspace_to_output_buffer()` emits, per segment in segment order, the slice's surviving rows followed by the surviving rows created on hosts in that slice, in creation order, and counts both in the segment's `output_count`. Each source halo's output range therefore stays contiguous, so `FirstHalo`/`NHalos` and next-snapshot gathering are unchanged, and created rows are inherited as Type 2 rows of their host's descendant with their `UniqueGalaxyID` unchanged; their lifetime from then on is the model's. A created row retired to Type 3 before marshalling is dropped like any other, and a created row whose host lies in no segment is fatal. A descriptor whose `created_host` is `NULL` holds no created rows.

**Identity.** `UniqueGalaxyID = -(1 + ordinal + MAX_CREATED_RECORDS_PER_HOST * (row + rows_per_unit * unit))` (`mimic_encode_created_galaxy_id()`, `src/include/galaxy_id.h`), with `row` the host's global row in its unit (`HaloNr + row_offset`, where `row_offset` is the appended `struct RecordIdentitySpace` member naming the unit's global row of the driver's local row 0: the first row, in the current snapshot, of the range being swept under a partitioned (distributed or chunked) horizontal run, and 0 for the vertical driver, an unpartitioned horizontal run (one task, one chunk) and every hand-built space), `ordinal` the host's created-record count within the FoF step in creation order, and `(unit, rows_per_unit)` the driver's published space (vertical: global forest number and the run-wide largest forest; horizontal: snapshot number and the largest slab; see [Adding a Vertical Reader](#adding-a-vertical-reader)); `mimic_decode_created_galaxy_id()` beside the encoder inverts it. Created IDs are negative, unique run-wide and deterministic for a fixed dataset and run file; tree rows keep their positive IDs. They are independent of MPI layout (including the rank count of a distributed horizontal run and the chunk count of a chunked one, because `row_offset` makes the encoded row the global one while `HaloNr` stays the local row that indexes the slab view), output chunking and `--skip`, with one qualification: under the vertical driver `rows_per_unit` is the largest forest over the run's `first_file..last_file` range, so a run over a subset of the files can give the same host a different created ID when the largest forest lies outside the shared range (positive IDs already shift with the forest offset in subset runs). They are not identical across drivers, and the cross-format identity comparator (`scripts/compare_cross_format_identity.py`) compares tree rows only by default, reporting created rows by count; its `--compare-created` flag compares created rows by the same byte-exact per-ID rule, for runs of the same driver (a serial horizontal run against a distributed one). Each driver evaluates at startup whether `1024 × units × rows_per_unit` fits `int64` and logs the numbers and the verdict at INFO, both through one shared helper (`record_identity_space_evaluate()`, `src/core/halo_evolution.c`) so the two lines share a format. A run that never creates a record is never affected; the first creation in a run whose space does not fit is refused with a message carrying `units`, `rows_per_unit`, the radix and the driver name. Two measured ceilings are known: the vertical driver on Shin-Uchuu ASCII and on full Uchuu, where `1024 × units × rows_per_unit` exceeds `int64` (their runs are unaffected until a module creates a record; a horizontal package of the same dataset would be budgeted on its own snapshot count and largest slab, which no committed package records for either). Pair identity, the source halo's ID plus an ordinal column, is the recorded escape beyond the scalar budget; it is not implemented.

**Memory.** Created galaxies come from the workspace's pool and raise the profile's `G`; created rows end in the output buffer and raise `C`/`P`. The staging blocks, staged-host map and per-host ordinal array are run-persistent, grow-to-high-water `MEM_HALOS` scratch in `module_registry.c`, allocated only once a record is created and released at driver teardown by `module_release_record_creation_scratch()` (the vertical driver through `free_vertical_driver_scratch()`, the horizontal driver in its teardown). The horizontal generation footprint check still covers only the seeded allocation; in-sweep growth from created rows is reported, as other in-sweep growth is.

**Footguns.**

- A module that creates in a substep phase creates once per substep unless it guards on `ctx->substep_number`; its ordinals continue across the substeps, so the IDs stay distinct but the records multiply.
- The staged row pointer is callback-scoped; write through it before returning and never keep it.
- Retiring a subhalo slice's only Type 0/1 row while leaving created dependants makes the next snapshot's inheritance fatal, as it does for tree orphans; the marshal itself still emits those records after the retired host's slice.
- Promoting a row to Type 1 does not make it a host: its `CentralHalo` still names its slice's assembled Type 0/1 row, so creation on it is refused.
- A module cannot tell its dispatch mode from `ModuleContext`. A module that creates must be configured as `process_full_halo`; any other configuration fails at its first creation call.

The framework fixture `test_fixture` creates records when its optional `TestFixtureCreateRecords` parameter is set; `tests/unit/test_record_creation.c` and `tests/integration/test_record_creation.py` prove this contract under both drivers.

### Module Communication

Mimic is compiled against one model set and one simulation/catalog property package at a time with `make MODEL=<name> SIMULATION=<name>`. Discovery, property generation, module registration, model-local shared helpers, selected-simulation tests, selected-model tests, and plotting all come from those selected packages. If a researcher wants to mix modules from different model families, they should create a new model package and copy the desired modules/helpers/plots into it, then reconcile property names, parameter names, units, dependencies, and tests there.

Modules should not call each other directly. They communicate through:

- generated properties in `struct Halo` and `struct GalaxyData`
- explicit event contracts for `process_per_event` consumers
- model-local utility functions in `models/<model>/shared/` when multiple modules in the selected model set need the same calculation

Module metadata dependencies are validation aids. They document properties and parameters a module uses, but they do not automatically sort modules into a scientifically valid order. The YAML phase configuration remains the source of execution ordering.

---

## Creating Physics Modules

### Directory Modules

Directory modules are the production pattern:

```text
models/<model>/modules/my_module/
  my_module.c
  module_info.yaml
  README.md
  helper.c              # optional
  helper.h              # optional
  _tests/
    test_unit_my_module.c
```

Use a directory module when the module has any real mode constraint, tests, helper files, events, or module-local documentation.

Key rules:

- The directory name and `module.name` should match.
- `{module_name}.c` is implicit; do not list it in `additional_files`.
- List only helper `.c` files in `additional_files`; headers may be listed for documentation but only `.c` files are compiled from that field.
- Declare every supported processing mode explicitly.
- Add module-specific tests under `models/<model>/modules/<module>/_tests/`.
- Put model-level cross-module tests under `models/<model>/modules/_tests/`.

### Standalone Modules

A single `.c` file placed directly under a model package module root is also a valid runtime module:

```text
models/<model>/modules/my_prototype.c
```

Standalone modules are package-local.

The generator derives minimal metadata from the file name:

- module name: `my_prototype`
- source file: `my_prototype.c`
- supported modes: exactly the three FoF modes `process_full_halo`, `process_per_event`, and `process_by_galaxy` (a standalone module never advertises `process_snapshot`)
- no declared dependencies, parameters, tests, docs, or events

The C file must still implement the normal lifecycle symbols:

```c
int my_prototype_init(void);
int my_prototype_process(struct ModuleContext *ctx, struct Halo *halos, int ngal);
int my_prototype_cleanup(void);
```

Use standalone modules for small experiments and model-builder prototypes. Convert to a directory module when the module needs explicit mode constraints, dependency validation, parameters, tests, event contracts, additional source files, or module-local documentation.

### Module Metadata

Minimal `module_info.yaml`:

```yaml
module:
  name: my_module
  supported_processing_modes: [process_by_galaxy]
```

Recommended production metadata:

```yaml
module:
  name: my_module
  description: "One-sentence scientific or infrastructure contract"
  supported_processing_modes: [process_by_galaxy]

  additional_files:
    - helper.c

  dependencies:
    properties:
      - ColdGas
      - StellarMass
    parameters:
      - MyEfficiency

  tests:
    unit: _tests/test_unit_my_module.c
    integration: _tests/test_integration_my_module.py
    scientific: []

  docs:
    physics: README.md
```

Use `docs.physics` for production modules. If documentation is intentionally centralised elsewhere, make that explicit in the module metadata or validator policy rather than leaving unexplained warnings.

For the metadata field reference, see [Module Metadata Schema](#module-metadata-schema).

### Module README

Module READMEs should be short, local contracts rather than full papers. Include:

- what the module does
- supported processing mode(s)
- where it belongs in the pipeline
- properties read/written, including transport fields
- parameters used
- events emitted or consumed
- implementation notes that affect configuration
- key references

The concise README in `models/sage16/modules/sage_resolve_mergers_and_disruption/README.md` is a good model.

### Adding a New Model Package

A model package is discovered by directory under `models/`. The one mandatory file is `models/<model>/model_properties.yaml`; everything else is optional, and a package with an empty properties file and no modules is valid (`halos-only` is close to that). The conventions the `hod` and `sham` packages followed are the ones to copy:

- **Layout.** `model_properties.yaml`, `README.md`, `input/<model>_<simulation>.yaml` run files, `modules/<module>/` directory modules (each with `module_info.yaml`, a README and `_tests/`), `shared/` for model-private headers (the HOD's counter-based RNG lives in `shared/hod_random.h`), and `plots/` for the model's registry. Name a run file `<model>_<simulation>.yaml`; `scripts/fuzz_pipeline.py` loads that canonical name first and falls back to a lone `*_<simulation>.yaml` in `input/` only when the canonical file is absent (several candidates are an error).
- **Properties.** Declare only the properties the model owns. There is no `bool` type: a flag such as `HODGhost` or `ShamGhost` is `type: int` with `init_value: 0` and `range: [0, 1]`. Do not add a `parameter_units.yaml` unless a parameter uses an `*_INTERNAL` loader.
- **Ship what you document.** One run file for the committed fixture of the horizontal or vertical simulation the model targets, one for a real dataset, and a README that states the science scope, the parameters with units, the output contract, the caveats and what the first real-data run measured. A model that demonstrates a published prescription says so and makes no observational parity claim it has not tested.
- **Dual-mode modules.** A module that needs both a per-FoF callback and a whole-snapshot callback advertises `[process_full_halo, process_snapshot]` and checks its own phase placement in `init()`, because the pipeline validator checks only each phase's own rules. `hod_populate` requires one `post_timestep` entry as `process_full_halo` and, when a `post_snapshot` phase is configured, one entry there as `process_snapshot`; `sham_rank_match` requires one `pre_timestep` and one `post_snapshot` entry. Validate every numeric parameter with `isfinite` before its range check, because the strict parser accepts `nan` and `inf`.
- **Determinism.** Stochastic modules seed from stable per-halo keys, never a global stream (see [VISION](VISION.md) Principle 4). A module that creates records keys its draws on the host's `UniqueGalaxyID`.
- **Tests.** Module tests registered through `module_info.yaml` join the default tiers only for the vertical simulations in `FULL_MODEL_TEST_SIMULATIONS` (`scripts/discovery.py`). A package that targets a horizontal simulation adds a group to `tests/manual/run_snapshot_global_battery.py`, which runs its tests by path under the right `MODEL`/`SIMULATION`; export both selectors when you run a test standalone, because `tests/unit/run_tests.sh` falls back to the defaults otherwise. The battery treats any `FAIL`, `ERROR` or `SKIP` as a failure.
- **Plots.** The registry is model-local (`plots/figures/__init__.py`); register only figures whose required properties the model declares, and filter ghost or scaffold rows (`HODGhost == 0`, `ShamGhost == 0`) in the figure, not in the output.
- **One pair per build.** Use the same `MODEL=` and `SIMULATION=` for `generate`, `validate-modules`, tests and `make`; `make check-generated`, `make validate-modules` and `make lint-parameters` must pass for each simulation you support, and the default pair's generated code must be restored before the default-tier run.

---

## Processing Modes and Phases

### Processing Modes

| YAML mode | C enum | Module receives | Use for |
| --- | --- | --- | --- |
| `process_full_halo` | `PROCESSING_MODE_FULL_HALO` | Entire FoF workspace, `ngal >= 1` | Calculations needing central plus satellites; event producers |
| `process_per_event` | `PROCESSING_MODE_PER_EVENT` | One event target, `ngal = 1`, `ctx->active_event != NULL` | Physics triggered by emitted events |
| `process_by_galaxy` | `PROCESSING_MODE_BY_GALAXY` | One galaxy, `ngal = 1` | Local per-galaxy physics and time integration |
| `process_snapshot` | `PROCESSING_MODE_SNAPSHOT` | Borrowed whole-snapshot population through `process_snapshot` | Snapshot-wide calculations; see [Snapshot Callback Contract](#snapshot-callback-contract) |

The first three modes form the FoF callback family (`process`); `process_snapshot` is the snapshot family (`process_snapshot`). Each mode belongs to exactly one family, defined once in the C table in `src/core/processing_modes.c` (a `_Static_assert` makes an enumerator without an entry a compile error) and mirrored by `scripts/module_modes.py` for the generator and validator. A module may declare `process_snapshot` alone or alongside FoF modes. The FoF phases below accept only FoF modes, and `modules.post_snapshot` accepts only `process_snapshot`. The run-file parser maps mode names through the same table (`processing_mode_from_string()`) and rejects, in every phase, an entry mapping that names more than one module, and `module_system_init()` checks each entry's mode against its phase's family through the table, and against the module's supported modes and callbacks, before any `init()` runs.

Choose the narrowest mode that gives the module the context it needs. A module that only modifies one galaxy at a time should usually use `process_by_galaxy`. A module that redistributes reservoirs across a FoF group or emits merger events should use `process_full_halo`. `process_full_halo` is also the only mode that may create galaxy records: a created record joins the FoF workspace when the creating callback returns, so the modules after it, the by-galaxy passes of the same phase and every later phase and substep receive it as an ordinary row (see the [Record Creation Contract](#record-creation-contract)). A module cannot tell its dispatch mode from `ModuleContext`, so a module that creates must be configured as `process_full_halo`; configured as `process_by_galaxy` or `process_per_event` it fails at its first creation call. By-galaxy, per-event and snapshot callbacks cannot create.

### Phase Order

For each snapshot interval:

```text
pre_timestep
for each substep:
  each modules.phases entry in declared order
post_timestep
```

Under the horizontal driver, once every FoF group of a snapshot has run that sequence, each `modules.post_snapshot` entry runs once over the whole snapshot population, in YAML order. `for_each_phase()` visits the phases in this order, and visits `post_snapshot` only when it has entries, so pipeline collection, validation, event-contract enumeration and the HDF5 `EnabledModules` rows of a run without snapshot modules are exactly what they were before the phase existed.

Inside each phase:

1. All `process_full_halo` modules run in YAML order.
2. Events emitted by full-halo modules are dispatched immediately to matching `process_per_event` consumers in YAML order.
3. All `process_by_galaxy` modules run in galaxy-major order: for each galaxy, each by-galaxy module runs in YAML order.

That means mode grouping takes precedence over raw YAML line position. If a by-galaxy module appears before a full-halo module in the same phase, it still runs after full-halo/event work.

Phase selection guide:

| Phase | Runs | Typical use |
| --- | --- | --- |
| `pre_timestep` | Once before substeps | Setup, reionization, infall budgets, merger clock setup |
| `modules.phases.<name>` | Each substep, in YAML order | Named physical stages such as `galaxy_physics` or `satellite_mergers` |
| `post_timestep` | Once after substeps | Finalization and accumulator conversion |
| `post_snapshot` | Once per snapshot, after every FoF group (horizontal driver only) | `process_snapshot` modules needing the whole snapshot population |

### Accessing the Central Galaxy

`ctx->central_galaxy` points to the Type 0 central for the current FoF workspace and is available during module execution.

```c
struct Halo *central = ctx->central_galaxy;
double central_vvir = central->Vvir;
double central_hot_gas = central->galaxy->HotGas;
```

Use this when a satellite calculation depends on the central potential or when a module moves material to the central reservoir. Do not assume every entry in the `halos` array is valid for processing; check `halos[i].galaxy != NULL` and any relevant `Type` constraints.

For the full context field reference, see [ModuleContext Fields](#modulecontext-fields).

---

## Events

Events connect a `process_full_halo` producer to one or more `process_per_event` consumers in the same phase. Use them when a full-halo module detects a discrete event, such as a merger, and downstream modules need to respond immediately to the event target. Event contracts belong in each directory module's `module_info.yaml`, alongside supported modes and dependencies; see [Module Metadata Schema](#module-metadata-schema) for the full metadata field list.

Producer `module_info.yaml`:

```yaml
module:
  name: my_merge_producer
  supported_processing_modes: [process_full_halo]
  events:
    emits:
      - name: merger
        description: "value0=mass_ratio, value1=source_dt"
```

Consumer `module_info.yaml`:

```yaml
module:
  name: my_consumer
  supported_processing_modes: [process_per_event]
  events:
    consumes:
      - producer: my_merge_producer
        event: merger
```

Producer code:

```c
#include "module_system/generated/event_contracts.h"

if (module_emit_event(ctx, MY_MERGE_PRODUCER_EVENT_MERGER,
                      satellite_idx, central_idx,
                      mass_ratio, source_dt) != 0) {
    ERROR_LOG("Failed to emit merger event");
    return -1;
}
```

Consumer code:

```c
int my_consumer_process(struct ModuleContext *ctx, struct Halo *halos, int ngal)
{
    if (ctx->active_event == NULL || ngal != 1) {
        return -1;
    }

    double mass_ratio = ctx->active_event->value0;
    apply_event_physics(&halos[0], mass_ratio);
    return 0;
}
```

Configuration:

```yaml
modules:
  phases:
    satellite_mergers:
      - my_merge_producer: process_full_halo
      - my_consumer: process_per_event
```

Rules:

- Only `process_full_halo` modules can emit events.
- Consumers must declare `events.consumes` in their module metadata.
- The producer must be configured in the same phase as the consumer.
- Events are dispatched immediately when emitted.
- Consumer YAML order controls the order of consumers subscribed to the same event.
- HDF5 output records resolved event contracts under `RunProperties/EventContracts`.

---

## Parameters

Module parameters live under `modules.parameters` in the input YAML:

```yaml
modules:
  parameters:
    MyEfficiency: 0.5
    MyMode: 1
    MyTablePath: ./tables/
```

A module should:

1. List required parameters in `module_info.yaml`.
2. Load parameters in `init()`.
3. Validate physical ranges locally.
4. Store validated values in module-private static variables.

Parameters belong to the model package that declares them: `hod_populate`'s ten `HOD*` parameters and `sham_rank_match`'s nine `Sham*` parameters are listed in their `module_info.yaml` and set in the run file, and nothing in the core knows them. A parameter that needs a unit conversion uses the `*_INTERNAL` loaders and a model-local `parameter_units.yaml`; the `hod` and `sham` packages use no such loader and ship none. A module that creates records takes its draw parameters, such as the HOD's seed, from here too, so a run file fully determines the created rows.

```c
#include "module_system/parameter_helpers.h"

static double my_efficiency;
static int my_mode;
static char my_table_path[MAX_STRING_LEN];

int my_module_init(void)
{
    LOAD_AND_VALIDATE_RANGE_INCLUSIVE("MyEfficiency", my_efficiency,
                                      0.0, 1.0, "efficiency");
    LOAD_AND_VALIDATE_OPTION("MyMode", my_mode, 3, "mode selector");
    LOAD_PARAM_STRING("MyTablePath", my_table_path, MAX_STRING_LEN);
    return 0;
}
```

There are no core defaults for module parameters. Missing required parameters fail during module initialization.

Parameter helper definitions live in `src/module_system/parameter_helpers.h`. The reference table is in [Parameter Loading Macros](#parameter-loading-macros).

---

## Property System

Properties are generated from YAML metadata and then accessed as normal C struct fields. Core and simulation halo properties together define the merger-tree fields that Mimic uses to build workspaces; model properties define the galaxy state that physics modules evolve.

| Property type | Metadata file | Typical owner |
| --- | --- | --- |
| Core halo properties | `src/core/core_properties.yaml` | Minimum halo-tracking state Mimic requires to run |
| Simulation halo properties | `simulations/<SIMULATION>/halo_properties.yaml` | Catalog-specific merger-tree fields such as positions, velocities, spins, and IDs |
| Galaxy/model properties | `models/<MODEL>/model_properties.yaml` | Selected model-set physics modules |

Workflow for adding a galaxy property:

1. Add a metadata entry to `models/<MODEL>/model_properties.yaml`.
2. Run `make generate` for the default package pair, or add `MODEL=<name> SIMULATION=<name>` for a non-default pair.
3. Rebuild.
4. Use the generated field in modules.
5. Add or update tests that validate initialization, reset behavior, output behavior, and physics use.

Example galaxy property:

```yaml
- name: MyNewProperty
  type: float
  units: "1e10 Msun/h"
  description: "Short physical meaning"
  output: true
  init_source: default
  init_value: 0.0
  output_source: galaxy_property
  range: [0.0, 100000.0]
  sentinels: [0.0]
```

Use it in a module:

```c
float current = gal->MyNewProperty;
gal->MyNewProperty = current + delta;
```

### Transport Properties

Inter-module scratch fields are simply `output: false` with `init_repeat: true`: not written to output, and reset each substep so a stale value never leaks into the next halo. There is no dedicated `role` key — that combination *is* the transport contract. Record the producer and consumer modules in the optional free-text `notes` field (purely documentary; the generator ignores it).

```yaml
- name: CoolingGas
  type: float
  units: "1e10 Msun/h"
  description: "Gas mass cooling from hot to cold this substep"
  notes: "Transport scratch buffer: written by sage_calculate_cooling_budget; consumed by sage_apply_cooling."
  output: false
  init_source: default
  init_value: 0.0
  init_repeat: true
  range: [0.0, 100000.0]
  sentinels: [0.0]
```

### Output Properties

Set `output: true` to write a property. Generated output code copies or recalculates values according to `output_source`.

For simple galaxy properties:

```yaml
output_source: galaxy_property
```

For simple halo properties:

```yaml
output_source: copy_direct
```

For conditional or recalculated output, use an output helper in `src/module_system/output_helpers.h`:

```yaml
output_source: recalculate
output_function: output_infall_property_or_zero
output_function_arg: "g, g->infallMvir"
```

Property metadata is the source of truth for output fields and unit labels. Do not maintain manual exhaustive property tables in prose documentation unless they are generated or deliberately illustrative. For the metadata field reference, see [Property Metadata Schema](#property-metadata-schema).

### Property Precision

`type:` in property metadata is a precision decision, not a display detail — choose it deliberately.

- Core and simulation properties are shared by every model, so default to `double`. A prior `float` choice for core virial fields masked a real bug: comparing a fresh calculation against a rounded stored value (tracking a halo's historical-maximum `Rvir`/`Vvir`) could pick the wrong branch near the rounding boundary.
- Catalog fields (`simulations/<SIMULATION>/halo_properties.yaml`) should match the source data's *real* precision, not a reader's C variable type or a neighboring package's declaration. A `double` HDF5 dataset can still only hold values a Rockstar/Consistent-Trees ASCII catalog originally wrote to ~7 significant figures — check actual catalog values (not just the reader code) before widening.
- Model-local accumulator properties (gas/mass/metal reservoirs) should also default to `double`. sage16's are `float` only for byte-for-byte parity with sage-model's `struct GALAXY` — see the comment at the top of `models/sage16/model_properties.yaml`. A new model with no parity constraint should not inherit that choice.

### Units and the Reference Basis

Mimic runs entirely in one fixed internal reference basis, declared once in `src/core/core_properties.yaml` under `reference_units`:

| Dimension | Reference unit | `h_convention` |
| --- | --- | --- |
| mass | `1e10 Msun/h` | `carried` |
| length | `Mpc/h` | `carried` |
| velocity | `km/s` | `none` |
| time | derived (`length/velocity`) | `carried` |

`reference_units` also registers derived (composite) reference dimensions built from this basis — currently `specific_angular_momentum` (`Mpc/h km/s`, i.e. length·velocity), used by properties whose natural unit is a product of basis dimensions. These are not additional basis elements: mass/length/velocity/time remains the complete, unchanged four-dimensional basis; a composite dimension is registered only so its label participates in the same registry-driven conversion and dimension-checking machinery as the four basis dimensions.

Every quantity entering from a catalog or a parameter declares its own units and is converted into this basis at the boundary, so internal code and output never have to ask which simulation produced a value. Two metadata fields drive conversion:

- `units` — a label from the unit registry in `scripts/generate_properties.py` (e.g. `Mpc`, `Mpc/h`, `Msun`, `1e10 Msun/h`, `km/s`). The label is the single source of dimensional truth: the registry maps each label to its dimension (`mass`, `length`, `velocity`, `time`, `specific_angular_momentum`, `dimensionless`, `count`, …) and default `h_convention`. The label must match the field's dimension: for run-file scalar parameters (`simulation.box_size`, `simulation.particle_mass`), `convert_unit_scalar()` rejects a source/reference dimension mismatch before computing any conversion factor, so a dimensionally-wrong `units` value now fails at load instead of silently mis-scaling.
- `h_convention` — whether the value carries the Hubble parameter: `carried` (h folded in, e.g. `Mpc/h`), `free` (h divided out / physical, e.g. `Mpc`), or `none` (h-independent, e.g. `km/s`). Defaults to the registry value for the label.

The generator emits a linear conversion — a cgs scale factor, plus a factor of `MimicConfig.Hubble_h` where source and target are both h-dependent but differ between `carried` and `free` — into `src/include/generated/unit_registry.h`. Catalog fields are converted at the vertical-reader boundary; output labels come from the reference basis, so a written value always matches its label. Values with `h_convention: none` are h-independent and cannot be converted to or from h-dependent conventions. Millennium catalogs are already in the reference basis, so their conversion is the identity and output stays byte-identical.

**Adding a catalog whose units differ from Millennium.** Declare the on-disk units on the field's entry in the simulation's single `halo_properties` list (see [halo_properties.yaml](#halo_propertiesyaml)); the generator converts them. For example, a catalog storing the virial-mass column in `Msun` (h-free) under a different on-disk name, and positions in `Mpc` (h-free):

```yaml
halo_properties:
  - name: M_Crit200
    source: Mass_200crit   # on-disk dataset/column name (defaults to `name`)
    type: float
    units: Msun
    h_convention: free
    provides_core_role: HaloMass
  - name: Pos
    type: vec3_float
    units: Mpc
    h_convention: free
    output: true
    init_source: copy_from_tree_array
    output_source: copy_direct_array
```

The reader converts `Msun → 1e10 Msun/h` (× 1e-10 × `Hubble_h`) and `Mpc → Mpc/h` (× `Hubble_h`) on copy-in. No core or module code changes — only the catalog metadata.

**Parameters with units.** A model parameter that is dimensional, rather than already in reference units, is declared in `models/<MODEL>/parameter_units.yaml` and loaded with the `*_INTERNAL` parameter macros, which convert it into the reference basis on load. A parameter not listed there is taken to be already in reference units.

```yaml
# models/<MODEL>/parameter_units.yaml
parameters:
  - name: MyMassThreshold
    type: double
    units: 1e10 Msun/h
    h_convention: carried
```

```c
LOAD_AND_VALIDATE_RANGE_INCLUSIVE_INTERNAL("MyMassThreshold", my_threshold, 0.0, 1.0e8,
                                           "mass threshold in internal units");
```

If you need a unit label the registry does not yet know, add it to `UNIT_REGISTRY` in `scripts/generate_properties.py` with its cgs magnitude and `h_convention`; generation fails loudly on unknown labels rather than guessing.

### HDF5 Output Writer

`src/io/output/hdf5.c` writes each output snapshot as a single compound `Galaxies` table (one row per `struct HaloOutput`), matching the binary record layout so both formats share `prepare_halo_for_output()`.

- **Buffered writes.** Prepared records accumulate in a fixed-size per-snapshot buffer (`HDF5_WRITE_BUFFER_RECORDS`) that flushes when full and once more at end of file (`flush_hdf5_buffers`). This decouples write granularity from tree boundaries and from file size, so memory stays bounded at large scale and the number of `H5TBappend_records` calls drops from O(trees × snapshots) to O(records / buffer).
- **FieldMetadata** (field names, units, descriptions) is identical for every snapshot, so it is written once per file under `RunProperties/FieldMetadata`, not duplicated per snapshot group. Its creation is generated by `scripts/generate_properties.py`; edit the generator, never the generated include.
- **Compression** is off by default and enabled per run with `--compress`, which sets `MimicConfig.HDF5CompressionLevel` and turns on gzip for the `Galaxies` table. HDF5's table API applies a fixed deflate level, so the flag is on/off only. Compression changes on-disk bytes, not stored values.

---

## Adding a New Simulation

A simulation package lives under `simulations/<name>/` and provides the merger tree catalog, cosmology, units, snapshot list, and any catalog-specific halo properties for a particular N-body simulation run. The shipped `simulations/mini-millennium/` package is the reference example.

### Directory Structure

```text
simulations/my_sim/
  simulation_info.yaml      required — catalog paths, cosmology, units, box size, chunking defaults
  my_sim.a_list             required — one scale factor per line per snapshot
  halo_properties.yaml      required — catalog halo fields beyond the core set
  snapshots/                required — tree data directory or symlink to local data
  plot_profile.yaml         optional — simulation-specific plotting defaults
  README.md                 optional — human description of this simulation package
```

### simulation_info.yaml

This file is the authoritative source for catalog paths, cosmology, and units. Its values become defaults for any run that references this simulation package; `input:` keys in the run YAML override them per-run.

```yaml
input:
  first_file: 0             # index of the first tree file to process
  last_file: 7              # index of the last tree file (inclusive)
  tree_name: trees_063      # base filename prefix (without file-number suffix)
  tree_type: lhalo_binary   # format; see Supported Tree Formats below
  processing_order: vertical  # optional; processing driver selector
  simulation_dir: ./simulations/my_sim/snapshots/
  snapshot_list_file: simulations/my_sim/my_sim.a_list

output:
  target_file_size_mb: 4096     # optional soft HDF5 chunk target in MiB (4 GiB default)
  forests_per_file: 0           # optional exact forest-count chunk size

simulation:
  cosmology:
    omega_matter: 0.25
    omega_lambda: 0.75
    hubble_h: 0.73
  box_size:
    value: 62.5
    units: Mpc/h
    h_convention: carried
  particle_mass:
    value: 0.0860657
    units: 1e10 Msun/h
    h_convention: carried
```

Core reference units are fixed in `src/core/core_properties.yaml`; `init.c` derives runtime constants from generated reference-unit definitions, not from the simulation package. Simulation scalar values and catalog fields declare their own units and `h_convention`, and generated code (`src/include/generated/unit_registry.h`) converts them into the fixed reference basis at the reader boundary. A scalar may still be written as a bare number (e.g. `box_size: 62.5`), which is taken to be already in reference units. For how units, dimensions, and `h_convention` are declared and converted, see [Units and the Reference Basis](#units-and-the-reference-basis).

Only catalogue-scale output planning defaults belong in `simulation_info.yaml`: `output.target_file_size_mb` and `output.forests_per_file`. They are defaults because large Consistent-Trees catalogues impose the chunking requirement regardless of which model runs on them. Run files may override those two keys, while output paths, output format, and snapshot selection remain run-file settings. `consistent_trees_ascii` cannot derive chunk sizes from `target_file_size_mb`, so ASCII simulation packages that use chunked output should set a positive `forests_per_file` default.

**Supported tree formats:**

| `tree_type` value | Format | Build |
| --- | --- | --- |
| `lhalo_binary` | Standard LHaloTree binary format (Springel et al.) | any |
| `lhalo_hdf5` | LHaloTree HDF5 layout (per-tree `tree_NNN/<field>` groups) | HDF5 |
| `consistent_trees_ascii` | Consistent-Trees / Rockstar ASCII output (`forests.list` + `locations.dat` + `tree_i_j_k.dat`) | any |
| `consistent_trees_hdf5` | Consistent-Trees forests-HDF5 packaging (uchuutools) | HDF5 |
| `horizontal_hdf5` | Horizontal HDF5 (`snapshot_NNN.h5` per snapshot); feeds `horizontal` | HDF5 |

`tree_type` selects a format, not a simulation: the same reader serves any simulation whose catalogue is written in that format. The HDF5-based readers are only present in an HDF5-enabled build; selecting one in a `USE-HDF5=no` build is a fatal configuration error. `processing_order` selects the processing driver independently of the reader format, and startup validation rejects any combination whose reader and driver disagree. It defaults to `vertical`; the four forest-ordered readers above feed that driver, while `horizontal_hdf5` feeds `horizontal`, whose driver (`run_horizontal_driver()`) opens and validates the dataset, processes every snapshot, and writes HDF5 output. To add a forest-ordered format of your own, see [Adding a Vertical Reader](#adding-a-vertical-reader) — it is a self-contained reader file plus one registry row, with no changes to the core read path; for the horizontal family, see [Horizontal readers](#horizontal-readers) and [The Horizontal Driver](#the-horizontal-driver).

`tree_name` is interpreted by the selected reader. `lhalo_binary` uses it as the prefix before the file number (`tree_name.<file_number>`). `consistent_trees_ascii` and `consistent_trees_hdf5` use it as a literal filename under `simulation_dir`, including any extension. `lhalo_hdf5` also uses explicit HDF5 filenames: for one file, set `tree_name` to that filename; for multiple files, include a `%d` file-number placeholder such as `trees_063.%d.hdf5`. `horizontal_hdf5` uses it as a declaration of the format's fixed filename convention and accepts exactly the literal `snapshot_%03d.h5`, rejecting every other value at startup.

The `consistent_trees_hdf5` reader is the reference high-throughput HDF5 input path. It caches chunk-range `ForestInfo`, opens each per-file `Forests/<field>` dataset once for the partition lifetime, validates field extents and datatypes at cache-open time, and serves normal forests from a fixed `CTREES_READ_WINDOW_BYTES` slab window (`128 MiB` per rank). Forests larger than the window use the same cached-handle direct read primitive, so the persistent window stays bounded. Do not add run-YAML knobs or whole-file slab buffering for this path without a new plan and validation gate.

### Snapshot Scale Factor List

The `.a_list` file contains one scale factor per line, ordered from earliest to latest snapshot (increasing `a`, decreasing redshift). Mimic derives the last valid snapshot index from this file, so a file with 64 entries defines snapshots `0..63`:

```text
0.0078125
0.012346
0.019608
...
1.0
```

These values drive all redshift and timestep calculations. Mimic counts snapshots by position in this file, so the ordering is critical. The last line corresponds to the highest snapshot index, normally `a = 1.0` (z = 0).

### halo_properties.yaml

This file is the single self-contained description of the simulation's on-disk halo catalog. Its `halo_properties` list contains **every** field of the on-disk catalog record, **in on-disk order**. This one list is the source of truth for the generated `struct RawHalo` (the binary record layout) and the HDF5 reader.

An entry that satisfies a core required-input role declares `provides_core_role`, using a role from `src/core/core_properties.yaml` under `required_inputs` (for example `HaloMass`, the tree links, `SnapNum`, or `Len`). The generator emits `mimic_tree_get_<Role>()` accessors from those bindings (and, for the five link roles only, `mimic_tree_set_<Role>(view, halonr, value)` setters, used by the distributed horizontal driver to rebase links), and core tree traversal uses those accessors rather than hard-coded catalog member names. An entry that Mimic copies into a halo property and/or writes to output also carries `output`, `init_source`, `output_source`, `description`, and `range`; entries with none of those (tree links, the virial-mass input, unused accounting fields) are registered for a complete record but produce no halo property.

Per-entry keys: `name` (the generated `RawHalo` member and Mimic-internal name), `source` (the on-disk dataset/column name, defaulting to `name` — declare it only when they differ), `type`, and, for dimensioned fields, `units` and `h_convention`. A field with no `output`, `provides_core_role`, or `init_source` is read only to preserve the complete on-disk record layout and produces no halo property; record that (or any other developer note) in the optional free-text `notes` field. `notes` is documentary only — the generator does not parse or enforce it. When a raw catalog field feeds an effective Mimic property through core policy rather than being copied directly, note that policy too. For example, Millennium `M_Crit200` provides the `HaloMass` role and is interpreted as output `Mvir` for FoF centrals when non-negative; otherwise core falls back to `Len * particle_mass` (policy lives in C core). Tree-link, index, and count roles must bind to scalar integer catalog fields; mass roles must bind to scalar numeric fields. There is no separate `catalog_properties` list and no hand-written `raw_member`: the struct is generated from this list, so list order and types are the binary layout.

The generator includes exactly one simulation package at a time, selected with `SIMULATION=<name>`. Adding a new simulation package is not enough by itself; regenerate and rebuild with that selector so the executable, `struct RawHalo`, `struct Halo`, output schema, validation ranges, and module dependency checks all use the intended catalog.

- A field name must be unique within the selected `src/core/core_properties.yaml` + `simulations/<SIMULATION>/halo_properties.yaml` + `models/<MODEL>/model_properties.yaml` set. A name that is both an on-disk field and a core-owned halo property (e.g. `SnapNum`, `Len`) is bound by `provides_core_role`, not duplicated as a separate halo property. Incompatible duplicate names fail at generation time.
- After adding or editing the default simulation package, run `make generate` followed by `make`. For another simulation, run `make SIMULATION=<name> generate` followed by `make SIMULATION=<name>`; add `MODEL=<name>` too when pairing it with a non-default model.

```yaml
halo_properties:
  # catalog-only entry (registered for the on-disk record; not a halo property)
  - name: M_Crit200
    source: Mvir            # on-disk dataset name differs from the Mimic name
    type: float
    units: 1e10 Msun/h
    h_convention: carried
    description: Catalog spherical-overdensity halo mass, M200c
    provides_core_role: HaloMass
    notes: Interpreted as output Mvir for FoF centrals when non-negative; otherwise core uses Len times particle_mass. Policy lives in C core, not enforced by this field.

  # catalog-only field that Mimic reads but does not use
  - name: M_TopHat
    type: float
    units: 1e10 Msun/h
    h_convention: carried
    notes: Unused catalog field, read only to preserve the on-disk record layout.

  # output halo property copied from the tree
  - name: Pos
    type: vec3_float
    units: Mpc/h
    h_convention: carried
    description: 3D position vector (comoving)
    output: true
    init_source: copy_from_tree_array
    output_source: copy_direct_array
    range: [0.0, 10000.0]
```

See [Property Metadata Schema](#property-metadata-schema) in the Reference section for the full field list.

### Wiring Up the Run YAML

Reference the simulation package from a model-local run file under `models/<model>/input/`:

```yaml
model:
  name: my_model

simulation:
  name: my_sim
```

Mimic derives `models/<model.name>`, `models/<model.name>/model_properties.yaml`, `simulations/<simulation.name>`, `simulations/<simulation.name>/simulation_info.yaml`, and `simulations/<simulation.name>/halo_properties.yaml`. The package paths and property metadata files are not run-file knobs because they must match the generated executable. Use `simulation.config` only when the run needs an alternate simulation metadata file with the same compiled simulation package, for example a smaller fixture:

```yaml
simulation:
  name: my_sim
  config: simulations/my_sim/_tests/input/test_simulation.yaml
```

To override simulation defaults for a specific run without changing the shared config:

```yaml
input:
  first_file: 0
  last_file: 0  # process only the first file
```

Any `input:` key in the run file takes precedence over the same key in `simulation_info.yaml`. The same precedence applies to `output.target_file_size_mb` and `output.forests_per_file`; other `output:` keys are intentionally run-owned and are not valid in simulation metadata.

### Optional: plot_profile.yaml

Provide a simulation-level `plot_profile.yaml` when plots need simulation-specific axis limits, units, or display defaults. `mimic-plot.py` discovers it automatically from `simulations/<simulation.name>/plot_profile.yaml`. It also discovers model-level defaults from `models/<model.name>/plots/profiles/default.yaml` and model/simulation-specific defaults from `models/<model.name>/plots/profiles/<simulation.name>_plot_profile.yaml`.

Use `plotting.profile` only for an additional run-specific override:

```yaml
plotting:
  profile: models/my_model/plots/profiles/custom_validation.yaml
```

The binary itself ignores the plotting section except for recording the configured path in run metadata. Profile `inherits` entries are resolved relative to the profile file that declares them, so package-local profiles should inherit neighbouring defaults with local paths such as `default.yaml`. See `simulations/mini-millennium/plot_profile.yaml` for the format.

### Workflow Summary

```bash
# 1. Create the simulation package directory
mkdir -p simulations/my_sim/snapshots

# 2. Create simulation_info.yaml, my_sim.a_list, and halo_properties.yaml

# 3. Place or symlink tree data under simulations/my_sim/snapshots/

# 4. Create the run file
cp models/sage16/input/sage16_mini-millennium.yaml models/sage16/input/my_sim.yaml
# Edit to point at simulations/my_sim/simulation_info.yaml and halo_properties.yaml

# 5. Regenerate property code for the selected model + simulation package
make MODEL=sage16 SIMULATION=my_sim generate

# 6. Build and run
make MODEL=sage16 SIMULATION=my_sim
./mimic models/sage16/input/my_sim.yaml
```

A horizontal simulation package has the same shape, plus a converted dataset behind `snapshots/` and a converter profile in the vertical package it was converted from; see [Converting Trees to Horizontal Input](#converting-trees-to-horizontal-input).

---

## Adding a Vertical Reader

A vertical reader teaches Mimic to read a new on-disk merger-tree *format*. It is independent of [Adding a New Simulation](#adding-a-new-simulation): a simulation package describes one catalogue (its cosmology, units, snapshot list, and `RawHalo` fields), while a reader describes how any catalogue stored in a given format is parsed into Mimic's halo structures. One reader serves every simulation written in its format.

Readers are registry-driven. Adding one is a self-contained implementation file plus a single row in `src/io/vertical/registry.c`; the core read path in `src/io/vertical/interface.c` and the driver loop in `src/core/main.c` never change.

### The reader interface

Each format defines exactly one `struct VerticalReader` (declared in `src/io/vertical/reader.h`). The core dispatches through its function pointers rather than switching on a format enum:

```c
struct VerticalReader {
  const char *name;           /* tree_type string in the run YAML */
  const char *file_extension; /* optional fixed suffix for legacy readers */

  enum TreePartitionModel partition_model; /* per-file or reader-enumerated */
  enum InputProcessingOrder processing_order; /* currently INPUT_PROCESSING_ORDER_VERTICAL */

  /* PARTITION_PER_FILE and PARTITION_ENUMERATED readers: */
  int (*num_partitions)(void);
  int (*partition_output_id)(int partition);
  int (*partition_exists)(int partition);
  void (*format_partition_path)(char *buf, size_t size, int output_id);
  int64_t (*count_partition_units)(int partition);
  int64_t (*max_partition_unit_halos)(int partition); /* largest unit, or -1 if unknown */
  int64_t (*global_forest_offset)(int partition);
  double (*partition_cost)(int partition);

  void (*open_partition)(int output_id); /* open + read the unit table */
  void (*load_unit)(int unit);           /* read one unit into InputTreeHalos */
  void (*close_partition)(void);         /* release per-partition scaffolding */
};
```

The vtable is deliberately minimal — fields exist only because a wired reader uses them. Do not add speculative callbacks; fold setup/teardown into `open_partition`/`close_partition`.

`max_partition_unit_halos(partition)` is required of every reader (`REQUIRE_READER_HOOK` fails a run whose reader leaves it `NULL`). It answers the largest halo count of any unit in a present partition, `0` for a partition with no units, or `-1` when the reader cannot know it without reading halo rows. Like `count_partition_units` it stages nothing and holds no open handle. The driver calls it in the same startup scan as `count_partition_units`, on every rank, and folds the answers into a run-wide largest unit, where one `-1` makes the whole run's value unknown. That value sizes the created-record identity space (`struct RecordIdentitySpace` in `src/include/types.h`, encoded by `mimic_encode_created_galaxy_id()` in `src/include/galaxy_id.h`): `rows_per_unit` is the run-wide largest unit, or `MimicConfig.UniqueGalaxyIDMultiplier` when it is unknown, and `units` is the run's total forest count. The driver logs `units`, `rows_per_unit`, the radix `MAX_CREATED_RECORDS_PER_HOST` (1024) and whether `1024 * units * rows_per_unit` fits int64 once at INFO, then publishes `(unit = GlobalForestOffset + unit index, rows_per_unit, fits, units)` before loading each unit (`vertical_driver_record_identity_space()`). A space that does not fit never stops a run. The shipped answers: `lhalo_binary` reads the `InputTreeNHalos` table in the file header, `lhalo_hdf5` the `/Header` `InputTreeNHalos` attribute, `consistent_trees_hdf5` the `ForestNhalos` column of each file's `ForestInfo` index over the chunk's forest range, and `consistent_trees_ascii` answers `-1` because its catalogues carry no per-forest halo count.

### Partition and unit model

The core iterates the input as **partitions** of **units**. A partition is the unit of output: the driver opens one set of output files per partition, names them by the partition's `output_id`, and finalises them when the partition is done. A unit is one independently processed merger structure within a partition. This section describes the vertical-reader partition models below, where a partition is one **input chunk** — a whole file or a reader-defined chunk of forests. The horizontal driver partitions its output the same way conceptually but on the other side of the seam: a partition there is one **requested output snapshot**, with the snapshot number as its `output_id`, and there are no readers, units, or a `partition_model` field on that side (see [The Horizontal Driver](#the-horizontal-driver) → "Output-partition seam"). Current readers use these partition models:

| Model | Partition | Unit | Output id | Example |
| --- | --- | --- | --- | --- |
| `PARTITION_PER_FILE` | one input file | one tree | file number | both L-Halo readers |
| `PARTITION_ENUMERATED` | reader-defined chunk | one tree/forest | chunk or reader id | both Consistent-Trees readers |

- **`PARTITION_PER_FILE`**: the driver strides partitions across MPI tasks. Supply the enumeration via the shared helpers `tree_partition_per_file_count()` / `tree_partition_per_file_output_id()` (in `interface.c`) as your `num_partitions` / `partition_output_id`. Implement `count_partition_units(partition)` as an allocation-free header read so the driver can build the run-scoped global forest-offset table before processing. `open_partition(output_id)` opens file `output_id` and reads its tree table.
- **`PARTITION_ENUMERATED`**: the reader publishes a deterministic list of output partitions and costs. The driver assigns those partitions to MPI tasks, but output ids remain the reader's partition ids and therefore do not depend on `NTask`. The Consistent-Trees readers use this for chunked output: each partition is a forest-range chunk, `GlobalForestOffset` is the chunk's first global forest, and chunk ids drive output names and per-chunk resume. An enumerated reader's `global_forest_offset(partition)` plus each partition's unit count must tile `[0, total units)` contiguously, with no gap and no overlap, because the created-record identity and the uniqueness of positive IDs depend on it; the driver does not check it.

### Where the partition model is observed outside the reader

Three pieces of the core key on `partition_model`; a new reader inherits them by setting the field correctly and supplying the matching callbacks:

- **Unique galaxy ids** — `make_unique_galaxy_id()` in `src/core/build_model.c` computes `forestnr_global = GlobalForestOffset + unit`, range-checks both components, and encodes through `mimic_encode_unique_galaxy_id()` (`src/include/galaxy_id.h:48`), which is `halonr + multiplier * (forestnr_global + 1LL)` for the configured `MimicConfig.UniqueGalaxyIDMultiplier`. **The `+ 1` is load-bearing** — it reserves the first multiplier block, so dropping it shifts every id by a whole block and breaks cross-driver identity. Encode through that helper rather than reimplementing the arithmetic. `PARTITION_PER_FILE` readers get `GlobalForestOffset` from the driver's prefix-sum scan over present files; `PARTITION_ENUMERATED` readers publish chunk offsets. Partition ids and MPI task ranks are not part of the identity.
- **Per-file offset scan** — `run_vertical_driver()` calls `count_partition_units(partition)` for every present `PARTITION_PER_FILE` input file before processing so missing files keep the existing skip semantics and present files receive contiguous run-scoped offsets.
- **HDF5 master file** — `write_master_file()` asks readers for their partition ids, creates links to existing output files, and records per-snapshot totals from each partition file.

### Steps

1. Create `src/io/vertical/read_<format>.c` (and a `.h` only if it exposes a shared seam). Implement the callbacks and define one `const struct VerticalReader <Format>Reader = { ... }`. Bridge the format's halo records into the generated `struct RawHalo` by field name; let the generated reference-unit accessors apply unit conversion at the boundary (declare native units in the simulation package, not in reader code).
2. Append one row to `reader_table[]` in `src/io/vertical/registry.c`. If the reader requires HDF5, guard both the `extern` declaration and the table entry with `#ifdef HDF5` so non-HDF5 builds simply do not register it (`vertical_reader_lookup` then returns `NULL` and the run fails fast with a clear message).
3. If the reader pulls in `src/io/vertical/<format>/*.c` support code that compiles in every build, keep it warning-clean under `-Wall -Wextra -Wshadow -Wformat-security -Wundef` and exercise it from `tests/unit/` so nothing is dead. The unit harness enables HDF5 reader sources when HDF5 development libraries are available, while HDF5 integration paths are still validated end-to-end against real or fixture datasets.
4. A new *format* needs no run-YAML changes beyond `tree_type` when it feeds the existing `vertical` driver. Set `.processing_order = INPUT_PROCESSING_ORDER_VERTICAL` in the reader initializer. A horizontal format belongs to the other reader family instead of being squeezed in here — see [Horizontal readers](#horizontal-readers).

The Consistent-Trees readers (`src/io/vertical/read_ctrees_ascii.c`, `read_ctrees_hdf5.c`) are the worked reference for `PARTITION_ENUMERATED` chunked readers, including forest load-balancing and the `RawHalo` bridge.

### Horizontal readers

Horizontal input is a second reader family, not a variant of the vertical readers. A vertical reader hands the core one forest at a time; a horizontal reader hands it one snapshot's whole halo population — a *slab* — so global, snapshot-synchronous operations become expressible. The on-disk contract these readers consume is [`convert/mimic-convert/HORIZONTAL-HDF5-FORMAT.md`](../convert/mimic-convert/HORIZONTAL-HDF5-FORMAT.md), which specifies two versions: the frozen `format_version = 2` and the normative [`format_version = 3`](../convert/mimic-convert/HORIZONTAL-HDF5-FORMAT.md#version-3). Version 1 is rejected outright, with no legacy-read path. One horizontal reader ships: `horizontal_hdf5` (`src/io/horizontal/read_horizontal_hdf5.c`), which reads both versions and dispatches on each file's `format_version`; a dataset never mixes versions. Its version 2 validation path is exactly what it was before version 3 was added. `micro-uchuu-ascii-horizontal` exercises version 2, and `mini-millennium-horizontal` is the reference version 3 package.

**Version 3.** `convert/mimic-convert/convert_trees.py` writes a lossless `format_version = 3` from L-Halo binary, Consistent-Trees forests-HDF5 and Consistent-Trees ASCII sources (manual: [`convert/mimic-convert/README.md`](../convert/mimic-convert/README.md)). Relative to version 2 it carries int64 links, three target-snapshot columns (`DescendantSnapshot`, `FirstProgenitorSnapshot`, `NextProgenitorSnapshot`) so a link may skip snapshots, a `SourceHaloID` row key, and a `/schema` group declaring each payload field's type, units and `h_convention` in the source's native units. The runtime consumes it as follows:

- **Index width.** The property generator accepts `type: long long` for the five `tree_link` roles (index and count roles such as `SnapNum` and `Len` stay `int`). Every generated `mimic_tree_get_*` accessor takes an `int64_t` halo number, and the link accessors return `int64_t` whatever the storage, so vertical packages keep `int` link storage while all index arithmetic is 64-bit. `struct Halo.HaloNr` is `long long`; `struct HaloAuxData`, the output-buffer segments, the inheritance descendant index and every driver walk, capacity and slab-loop variable are `int64_t`. The driver refuses a slab above `INT_MAX` only when it is a requested output snapshot, because the output path caps a snapshot's record count at `INT_MAX` (`output_increment_halo_counters_checked`, `src/io/output/util.c`), so a requested output snapshot above `INT_MAX` halos is refused before loading, and warns above `MAX_HALO_ARRAY_SIZE`, where the output marshaller cannot grow a buffer; otherwise it admits the slab. The version 2 rule `n_halos <= INT32_MAX` stays, because it is a version 2 format rule. The remaining `int` boundary is the module pipeline's galaxy count and central index, reached through a checked narrowing bounded by `MAX_HALO_ARRAY_SIZE`.
- **Reader-owned arrays.** The three target-snapshot columns and `SourceHaloID` are format metadata, not catalog properties: `load_slab` reads them into `struct SnapshotSlab`'s own `descendant_snapshot`, `first_progenitor_snapshot`, `next_progenitor_snapshot` and `source_halo_id` arrays (all `NULL` for a version 2 slab), exactly as `ForestIndex`/`HaloRankInForest` already were. A package never declares them.
- **Schema agreement.** `make generate` emits `src/include/generated/catalog_field_metadata.inc`, the compiled `units` and `h_convention` of every catalog field, and `open_run` compares each field the package declares against the file's `/schema`, aborting on any disagreement (see [What `open_run` validates](#what-open_run-validates)). A `/schema` field the package does not declare is validated for internal consistency and not materialised.
- **One simulation package per (simulation, source format).** Because payload units are the source's own — L-Halo mass is float32 `1e10 Msun/h`, Consistent-Trees mass float32 `Msun/h` — every field a consuming package's `halo_properties.yaml` declares must match that source format's `/schema` (the rule is one-way: undeclared `/schema` fields are validated and not materialised). The shipped packages happen to declare every payload field their converter profile selects. An L-Halo and a forests-HDF5 conversion of one simulation therefore need two packages (`micro-uchuu-horizontal` and `micro-uchuu-hdf5-horizontal`). Each version 3 package declares its five links `long long`, declares none of the reader-owned arrays, mirrors its vertical package's declaration order and ranges, carries its vertical package's cosmology and a byte copy of its a_list, and ships `_tests/integration/test_schema_conformance.py`, which checks the declarations against the converter's `/schema` derivation and the compiled metadata. Its `snapshots/` is a gitignored link to a dataset converted outside the repository.
- **Gaps.** The driver resolves every progenitor link through its target-snapshot column and retains a generation for as long as a later snapshot can name it; see [The Horizontal Driver](#the-horizontal-driver).

**What is supported is what has been gated.** A version 3 route is supported only where a parity gate has shown horizontal output bitwise identical, per `UniqueGalaxyID`, to the vertical reader of the same source format over the same files. The routes, their evidence and their limits are listed in [V3 Runtime Support](../convert/mimic-convert/HORIZONTAL-HDF5-FORMAT.md#v3-runtime-support): mini-Millennium (complete, gapped) under `halos-only` and `sage16`; micro-Uchuu from L-Halo and from forests-HDF5 (complete) under `halos-only`; Millennium (complete, gapped) and mini-Uchuu (complete) under `halos-only`, over all their files. Each gate is the package-local `_tests/scientific/test_cross_format_identity.py`, run with `make MODEL=halos-only SIMULATION=<package> tests-scientific` on a machine holding both datasets. Do not describe any other route as runnable. **Full Uchuu is not claimed**: int64 indices make its slabs addressable, not small enough to hold. Chunked sweeps (see [Chunked sweeps](#chunked-sweeps)) now bound a run's memory by its chunk rather than its widest slab, but full Uchuu stays unclaimed because it is storage-bound before it is memory-bound, and its largest forest bounds any chunk.

The horizontal **driver** (`run_horizontal_driver()`, `src/core/horizontal_driver.c`) now exists, so every level of reader checking is on the run path:

- **Configuration validation — runs on every run.** Two-registry `tree_type` resolution, the reader/order compatibility check, the exact `tree_name` literal, the identity-multiplier rules, and the horizontal rejections (HDF5-only output and no `--skip`; a multi-task run is accepted here and refused later, at startup of the driver, if the dataset is not a forest-blocked version 3 dataset; see [The Horizontal Driver](#the-horizontal-driver)), all in `src/core/read_parameter_file.c`.
- **Dataset validation (`open_run`) — the driver's first call.** `horizontal_reader_open_run()` is called from `run_horizontal_driver()` before any snapshot is loaded, so a missing, unreadable, or corrupt dataset aborts here with the file, object, and value that failed.
- **Link-range validation (`load_slab`)** — runs once per snapshot as the driver loads each slab.

The run path is `read_parameter_file()` → `init()` → `run_processing_driver()` → `run_horizontal_driver()` for a horizontal configuration (`src/core/vertical_driver.c`, the `INPUT_PROCESSING_ORDER_HORIZONTAL` dispatch case). A horizontal run therefore opens its dataset and runs every open-time check before processing anything, and checks links and ranges as each slab loads; producer-only obligations (chain topology, `SourceHaloID` order, `Len ≥ 0`) are not re-checked ([V3 Validation Requirements](../convert/mimic-convert/HORIZONTAL-HDF5-FORMAT.md#v3-validation-requirements)).

#### The horizontal reader interface

`struct HorizontalReader` (`src/io/horizontal/reader.h`) is a separate, small vtable rather than a widening of `struct VerticalReader`, whose thirteen hooks are partition/unit-shaped and carry no meaning for horizontal input. `enum InputProcessingOrder` and `input_processing_order_name()` are shared with the vertical side, because the processing order is a property of the run rather than of one reader family:

```c
struct HorizontalReader {
  const char *name;                        /* tree_type string in the input YAML */
  enum InputProcessingOrder processing_order;

  void (*open_run)(const struct HorizontalOpenOptions *options,
                   struct HorizontalRunInfo *info);  /* open + run the open-time checks */
  void (*close_run)(void);                         /* release every run-scoped resource */
  int64_t (*snapshot_halo_count)(int64_t snapnum); /* count without loading */
  void (*load_slab)(int64_t snapnum, int64_t row_lo, int64_t row_hi,
                    struct SnapshotSlab *slab);    /* rows [row_lo, row_hi) of one snapshot */
  void (*release_slab)(struct SnapshotSlab *slab);
  void (*scan_forest_index)(int64_t snapnum, horizontal_forest_index_visitor visit, void *user);
};
```

Three hooks changed for distributed operation, and the reader stays MPI-free and partition-agnostic throughout:

- **`open_run` takes options.** `struct HorizontalOpenOptions { int validate_columns; }` (a NULL pointer aborts). With `validate_columns == 0` the whole-column data scans of `SnapNum`, `HaloRankInForest` and `ForestIndex` are skipped and the header's `max_halo_rank_in_forest` and `n_forests_total` are published as the measured values; every structural, header, schema and identity-bound check still runs. A serial run passes 1; a distributed run passes 1 on task 0 and 0 elsewhere, so only one task pays for the scans. `struct HorizontalRunInfo` also gains `source_format`, the version 3 header's string (empty for version 2).
- **`load_slab` takes a row range.** `0 <= row_lo <= row_hi <= snapshot_halo_count(snapnum)` (abort otherwise); `slab->nhalos == row_hi - row_lo` and the new `slab->row_offset == row_lo`, so element `i` of every slab array is snapshot row `row_offset + i`. Every `/halos` dataset, the aux columns included, is read for exactly that range through hyperslab selections. The links stay the dataset's **global** row indices, validated against the global per-snapshot counts; the driver rebases them (see [Distributed operation](#distributed-operation)), and no reader check compares a link to the row's own index. The unchunked serial driver passes the whole range, so an unchunked serial run's reader behaviour is unchanged.
- **`scan_forest_index` is new.** It streams one snapshot's `ForestIndex` column to `visit(first_row, values, count, user)` in ascending row order, in blocks of at most `HORIZONTAL_HDF5_SCAN_BLOCK` rows, without allocating per halo; a snapshot with no halos makes no call. `values` points into the reader's buffer, so the visitor copies what it keeps and never calls back into the reader. The values are not range-checked: rely on a `validate_columns = 1` open. The distributed driver uses it to compute the partition and verify forest blocking.

`struct HorizontalRunInfo` carries the run-scoped metadata `open_run` publishes: snapshot count, `format_version`, `source_format`, `links_adjacent` (always 1 for version 2; 0 or 1, measured over the whole dataset, for version 3), `slab_row_bytes` (the bytes `load_slab` allocates per halo across every reader-owned slab array, which the driver's retention accounting consumes), `n_forests_total`, and `max_halo_rank_in_forest`. Slab indices, counts, and offsets are `int64_t` throughout — production slabs reach hundreds of millions of halos, so the vertical driver's `int` idiom does not carry over.

Readers register in `src/io/horizontal/registry.c`, a static table mirroring `src/io/vertical/registry.c` with case-insensitive lookup through `horizontal_reader_lookup()`. Both the `extern` and the table row are `#ifdef HDF5`, so a non-HDF5 build registers nothing and the lookup returns `NULL` for every name. `horizontal_reader_count()` and `horizontal_reader_at()` enumerate the table, which is how tests assert that the vertical and horizontal name sets stay disjoint. Dispatch goes through the thin wrappers in `src/io/horizontal/interface.c`, each of which checks at its point of use that the hook it needs is implemented and aborts naming that hook otherwise.

Two filename rules follow from the format being fixed rather than user-chosen. `input.tree_name` must be exactly the literal `snapshot_%03d.h5` (`HORIZONTAL_READER_TREE_NAME`), and the reader builds every path with a fixed internal format string and truncation checking — configured text is never passed to a `printf`-family format argument.

#### How `tree_type` resolves to a reader

`input.tree_type` is still the single reader selector, and there is still exactly one resolution site (`parse_input_section` in `src/core/read_parameter_file.c`). It consults two registries: `vertical_reader_lookup()` first, then `horizontal_reader_lookup()`. The two name sets are disjoint, so the order fixes only which registry answers first, never which reader a name resolves to. After a successful resolution exactly one of `MimicConfig.vertical_reader` and `MimicConfig.horizontal_reader` is non-`NULL`, and `MimicConfig.TreeExtension` is set from `reader->file_extension` for a vertical reader and left empty for a horizontal reader. An unrecognised `tree_type` fails with one message naming both registries.

Startup validation then reads the resolved reader's declared `processing_order` and rejects any mismatch with `input.processing_order`, whichever registry answered. Consequently `MimicConfig.vertical_reader` is legitimately `NULL` for a horizontal configuration, and every consumer must either guard for that or run only on the vertical path. `validate_and_postprocess()` consumes it on the configuration path itself (`src/core/read_parameter_file.c:1432-1445`) and is correctly guarded — it tests both pointers for `NULL` and then branches on `is_vertical_reader`. The vertical-only consumers `src/io/vertical/interface.c` (`:53`, `:90`, `:120`, `:134`) and `src/core/vertical_driver.c:514` dereference `MimicConfig.vertical_reader` unguarded, which is safe because they run only inside `run_vertical_driver()`. **No file under `src/io/output/` reads `MimicConfig.vertical_reader` at all any more**: both HDF5 output writers go through the driver-neutral `struct OutputPartitionSource get_output_partition_source(void)` (`src/io/output/util.h`, constructed in `src/core/vertical_driver.c`), which the vertical driver populates from `MimicConfig.vertical_reader`'s hooks and the horizontal driver from its own partition source, one partition per requested output snapshot — see [The Horizontal Driver](#the-horizontal-driver).

#### What `open_run` validates

`open_run` runs on every horizontal run, called from `run_horizontal_driver()` before any snapshot is processed. The reader validates structure before reading any data, so a non-conforming file is rejected rather than read into a buffer sized from different assumptions. The list below is the version 2 path; version 3's additions follow it. In order, for every snapshot in the configured snapshot list:

1. **Structure** — exactly the `/header` and `/halos` groups; exactly the contract header attribute set, each scalar and of the contract dtype; exactly the contract `/halos` dataset set, each of the contract dtype, rank 1 for scalars and shape `[n_halos, 3]` for `Pos`, `Vel`, and `Spin`.
2. **Header values** — a supported `format_version`; `links_adjacent == 1`; `snapshot_number` equal to the filename index; `n_halos` in `[0, INT32_MAX]` and equal to the length of every `/halos` dataset in that file; `n_forests_total` and `max_halo_rank_in_forest` identical across all files.
3. **Agreement with the snapshot list and configuration** — each file's `scale_factor` equals its a_list entry **exactly**, with no tolerance, matching the producer's own comparison. The five physical header attributes — `box_size_mpc_h`, `particle_mass_msun_h`, `omega_matter`, `omega_lambda`, `hubble_h` — are compared against `MimicConfig`'s configured values for **every** snapshot file, not only the first, and the run aborts on mismatch naming the file, the attribute, and both values. The comparison is a rounding tolerance, not a scientific one: reject non-finite values, require exact equality when both are zero, otherwise accept iff `fabs(header - configured) <= 16 * DBL_EPSILON * fmax(fabs(header), fabs(configured))`. Particle mass is compared against `MimicConfig.PartMass * 1e10`, multiplying the configured value up to native units rather than dividing the header down, because a naive comparison in the wrong direction fails by exactly the 10¹⁰ unit factor.
4. **Identity bounds against measured data** — every `SnapNum` equals the file's `snapshot_number`; `max_halo_rank_in_forest` equals the measured maximum of `HaloRankInForest`; every `ForestIndex` lies in `[0, n_forests_total)`, and the measured maximum `ForestIndex` equals `n_forests_total - 1`. These run as fixed-size hyperslab scans that accumulate a running maximum or range, never allocating a buffer proportional to `n_halos`. A dataset with no halos in any snapshot carries the sentinel `(n_forests_total, max_halo_rank_in_forest) == (0, -1)` and skips the measured-maximum equalities.
5. **Encodability** — `horizontal_identity_bounds_valid()` checks the published bounds against the configured identity multiplier before the run info is published.

**Version 3 additions.** A version 3 file's structure is exactly `/header`, `/halos` and `/schema`, and no object under the root, `/halos` or `/schema` may be a soft or external link (checked before the object is opened). The header adds `source_format` and `column_mapping_sha256`; `links_adjacent` may be 0 or 1; `n_halos` is int64 with no int32 ceiling. `links_adjacent`, `source_format`, `column_mapping_sha256` and the whole `/schema` group must be identical in every file. `/schema` must be internally consistent (each subgroup carries exactly the four string attributes, with a valid `type` and `h_convention`, and its dataset's dtype and shape match its `type`), and every field the package declares must appear in `/schema` and `/halos` with the `type`, `units` and `h_convention` of the package's compiled declaration. Topology and identity fields are checked against the format's fixed table instead, because `/schema` never declares them. Steps 3–5 apply unchanged.

Every failure aborts with the file path, the offending object, attribute, or field, and the value; nothing is repaired. Chain *construction* is the producer's obligation, discharged by the converter and its topology gate — the reader reads the links and validates their index ranges, and never reconstructs or reorders them.

#### Slab lifecycle

```text
open_run  ->  [ load_slab / release_slab ]*  ->  close_run
```

A slab handle has a defined empty state (`SNAPSHOT_SLAB_INIT`, tested with `snapshot_slab_is_empty()`); `snapnum` is the marker, not `nhalos`, because a snapshot containing zero halos is a legal load result. `load_slab` requires an empty destination and aborts otherwise, allocates `struct RawHalo[nhalos]` through `mymalloc_cat(..., MEM_TREES)`, and fills every field by including the generated `src/include/generated/read_tree_hdf5_properties.inc` under horizontal-flavoured macros — the same mechanism `src/io/vertical/hdf5.c` uses, with no generator change. `release_slab` frees the array and returns the handle to its empty state; releasing an already-empty slab is a no-op. `close_run` aborts if any slab is still loaded.

At load time — once per snapshot as `run_horizontal_driver()` loads each slab — the reader validates link ranges: `FirstProgenitor` is `-1` or an index into snapshot `N-1`; `NextProgenitor` and `NextHaloInFOFgroup` are `-1` or indices into snapshot `N`; `FirstHaloInFOFgroup` is always a valid index into snapshot `N` and never `-1`; `Descendant` is `-1` or an index into snapshot `N+1`, and `-1` for every halo in the final snapshot. For version 3, the ranges come from the target-snapshot columns instead of from `N±1`: every non-null link must be a valid row of the file its column names, every column must be `-1` exactly when its link is `-1`, `DescendantSnapshot` must be later than the owner's snapshot and `FirstProgenitorSnapshot` earlier, and `links_adjacent == 1` additionally requires every descendant at `N+1`. Diagnostics are bounded counted summaries — one line per snapshot and field, carrying the count and the first offending index and value — never one line per halo. Slabs are filled through fixed-size hyperslab blocks, so the only allocations proportional to `n_halos` are the slab arrays themselves.

#### The identity multiplier

`UniqueGalaxyID` encodes `halonr + multiplier × (forestnr_global + 1)`, and the compile-time `TREE_MUL_FAC = 10⁹` cannot represent a super-forest whose within-forest halo ranks reach into the billions. `simulation.unique_galaxy_id_multiplier` makes the multiplier per-simulation metadata: it is legal in `simulation_info.yaml` and in the run file, defaults to `TREE_MUL_FAC`, must be positive, and is stored in `MimicConfig.UniqueGalaxyIDMultiplier`. Because `parse_simulation_section` runs once per file, the default is seeded once before either pass and the parser assigns only when the key is present — so a package value survives a run file that omits it, and an explicit run-file value wins.

One bound is enforced, at two points. The horizontal reader checks the configured multiplier against the dataset's own bounds at `open_run`, which now runs on every horizontal run (`horizontal_identity_bounds_valid()`: the multiplier must exceed every halo rank, and `multiplier × (n_forests_total + 1)` must fit in `int64_t` — the `+ 1` reserves the encoder's forest offset). Every helper in `src/include/galaxy_id.h` also takes the multiplier as an explicit `int64_t` parameter — the same bound expression, `mimic_unique_galaxy_id_max_forests()` — so **both** processing orders encode with the configured value and a vertical configuration may set a non-default multiplier; what a run enforces at startup for the value itself is only that it is positive. The three Consistent-Trees forest-size guards (`read_ctrees_ascii.c`, `read_ctrees_hdf5.c`) check against the same configured value, so raising the multiplier genuinely raises the forest size those readers accept. HDF5 output records the value as an `int64` `RunProperties/UniqueGalaxyIDMultiplier` attribute, written to both per-file outputs and the master file, so any output file can be decoded back into its identity components.

#### Adding a horizontal reader

1. Implement one `const struct HorizontalReader` in `src/io/horizontal/read_<format>.c`. If it needs HDF5, the filename **must** end in `hdf5.c` — the Makefile drops that pattern from `USE-HDF5=no` builds.
2. Append one row to `horizontal_reader_table[]` in `src/io/horizontal/registry.c`, guarding both the `extern` and the row with `#ifdef HDF5` when the reader needs it. Never reuse a name registered in `src/io/vertical/registry.c`; the two sets must stay disjoint. `registry.c` and `interface.c` must **not** be named `*hdf5.c`, because the configuration path calls `horizontal_reader_lookup()` in every build.
3. Run the dataset's open-time checks at `open_run`, structure before values and values before bulk reads, and abort rather than repair; state which invariants remain producer obligations.
4. Keep slab counts and indices `int64_t`, honour the empty-state lifecycle above, and allocate through `mymalloc_cat(..., MEM_TREES)`.
5. Ship a fixture simulation package with committed, small fixtures and C unit tests under `simulations/<name>/_tests/unit/`; `micro-uchuu-ascii-horizontal` is the worked reference.

### The Horizontal Driver

`run_horizontal_driver()` (`src/core/horizontal_driver.c`) is the second live driver behind `run_processing_driver()`'s dispatch (`src/core/vertical_driver.c`), reached when `input.processing_order: horizontal` resolves against a horizontal reader. Where the vertical driver walks one forest's full history depth-first with exactly one input generation live, the horizontal driver sweeps snapshots in increasing time order and holds a **pool of retained generations keyed by snapshot number**. A generation is one snapshot's raw slab with its reader-owned arrays, its aux ranges, its processed output buffer and its galaxy pool; the driver is its single owner, and the reader holds no retention state.

- **Horizon.** When a slab loads, `horizontal_generation_horizon()` computes its horizon: the largest `DescendantSnapshot` over its halos with a descendant, or its own snapshot if none has one. The generation stays live until the snapshot at its horizon has been processed, and is released then (`horizontal_release_expired_generations()`, after every FoF group at that snapshot has deep-copied what it inherits), in ascending order. Released galaxy pools return to a spare stack sized up front, so releasing never allocates.
- **Adjacent input keeps two.** For a `links_adjacent == 1` dataset, including every version 2 dataset, every generation's horizon is the next snapshot (or its own, when none of its halos has a descendant), so the pool never holds more than two generations — the old N/N−1 rotation. Gapped input keeps a generation across the snapshots its descendants skip: mini-Millennium and Millennium hold at most three, because their longest descendant span is 2.
- **Progenitor resolution.** `FirstProgenitor` resolves through `FirstProgenitorSnapshot` and each `NextProgenitor` through its own `NextProgenitorSnapshot`, into the retained generation of that snapshot; a chain may span several. `horizontal_find_most_massive_progenitor()` returns a `struct HorizontalProgenitorRef` (snapshot and row), because a row index alone is ambiguous across generations, and `struct HorizontalGatherContext` indexes the pool by snapshot. The chain-walk cycle guard is bounded by the total retained population. No synthetic halo, phantom generation or interpolated state is ever created, and an empty snapshot is processed as empty.
- **Inheritance across a gap.** An inherited galaxy keeps its progenitor's `SnapNum` until marshalling, so `setup_module_context()` derives the time interval and dynamic substep count over the real gap, `Age[k] → Age[m]`.
- **Sizing and the ceiling.** `horizontal_require_generation_fits()` computes each generation's resident bytes from its halo count before the reader loads its slab, taking the slab term from the per-row width the reader publishes as `HorizontalRunInfo.slab_row_bytes` (measured against the allocator by the fixture tests, so it excludes the allocator's rounding of each block up to 8 bytes) and the aux, output-buffer and pool terms from struct widths, logs them under `--verbose`, and aborts before allocation if they overflow `int64_t` or if `input.retention_memory_ceiling_mb` (stored in `MimicConfig.RetentionMemoryCeiling`, in bytes, 0 for none) is set and the pool plus the new generation would exceed it. A total exactly at the ceiling is accepted. The refusal names the snapshot, the bytes and the ceiling, and names `input.forest_chunks` (or more MPI tasks) as the lever, down to the rows of the largest forest, which no setting splits. Before that sizing it makes two width checks (`horizontal_require_slab_emittable()`): it refuses a snapshot above `INT_MAX` rows at a requested output snapshot, where failure is certain (a limit of the output path's global record count that neither chunks nor tasks lift), and warns once for any sweep whose rows exceed `MAX_HALO_ARRAY_SIZE`, where failure is likely (that one judges the rows a sweep holds, so chunks and tasks do lift it). The ceiling bounds admission only: in-sweep growth of an output buffer or galaxy pool is allocated mid-sweep, so it is measured and warned about once rather than refused, and the driver's run-wide workspace and process RSS are not covered. The configuration parser rejects the key for vertical runs and for zero, negative or non-integer values. The key is not recorded in `RunProperties`. Under chunking every figure in this bullet is the current chunk's, except that the `INT_MAX` record-count refusal judges the snapshot's global count (see [Chunked sweeps](#chunked-sweeps)).
- **Reporting.** The run memory profile's term `R` reports the most generations retained at once and the most bytes resident across them (`run_profile_note_retention()`, `src/util/run_profile.c`). Under `--verbose` the driver logs each load (`Loaded snapshot N (...); K slab(s) live`), each horizon, each release and the peak; `tests/integration/test_processing_order.py` asserts that lifecycle wording.
- **Failure path.** A driver-local `atexit` handler, `horizontal_failure_cleanup()`, releases every retained generation if the run aborts, so `close_run` never finds a slab loaded.
- **Snapshot-wide modules.** After a snapshot's FoF coverage check, `horizontal_run_post_snapshot()` runs the configured `modules.post_snapshot` entries over that generation's processed buffer, before it is published, before any release and before output. See [Snapshot Callback Contract](#snapshot-callback-contract).

Horizontal configurations are gated at config time (`validate_and_postprocess()`, `src/core/read_parameter_file.c`): `output_format: binary` is rejected (HDF5-only) and `--skip` is rejected (no resume). `NTask > 1` is accepted at configuration: the driver distributes the run over its tasks by forest, and refuses at startup, once it has opened the dataset, any dataset that is not a forest-blocked version 3 dataset. `input.forest_chunks > 1` is accepted at configuration for a horizontal reader without a `modules.post_snapshot` phase, and the driver applies the same startup refusals to it as to `NTask > 1`. The same function rejects a non-empty `modules.post_snapshot` under a vertical reader, since only the horizontal driver holds a complete snapshot, and rejects `input.forest_chunks > 1` under a vertical reader, which sweeps no slab.

**Explicit input view.** The generated `mimic_tree_get_*` accessors and the virial helpers (`get_virial_mass`/`get_virial_velocity`/`get_virial_radius`, `src/core/virial.c`) take a `struct HaloInputView { const struct RawHalo *halos; int64_t count; }` (`src/include/types.h`) as their first argument instead of reading a global array. The vertical driver constructs its view from `InputTreeHalos` and the loaded unit's halo count; the horizontal driver constructs its view from whichever retained slab a call site needs — the current snapshot's, or the generation a progenitor link names. The five link roles also have generated setters, `mimic_tree_set_<role>(view, halonr, value)`, which store `value` cast to the field's declared C type with no range guard; the const view is cast away once inside the setter because the horizontal driver owns the slab it rebases, and the caller range-checks first. This is what lets exactly the same physics-coupled code serve both drivers with no duplicated arithmetic: there is one shared generated payload populator (`populate_halo_payload.inc`), and `prepare_halo_for_output()` (`src/io/output/util.c`) takes the view too, so no file under `src/io/output/` reads a raw input global.

**Instanced galaxy pools.** `struct GalaxyPool` (`src/core/galaxy_pool.h`) is created with `galaxy_pool_create()` and threaded explicitly through `inherit_descendant_halos()`; the horizontal driver holds one instance per retained generation and returns a released generation's pool, bulk-reset, to its spare stack, where the vertical driver resets its single instance once per tree.

**Driver parity.** The physics engine, inheritance service, and output-buffer marshaller are shared unchanged, and so are the driver adapters in `src/core/halo_evolution.c` (`process_halo_evolution()`, `setup_module_context()`, `count_fof_subhalos()`, `make_halo_init_payload()`), which both drivers call with their own FoF workspace. What is replicated in `horizontal_driver.c` is only the code that crosses generations — progenitor lookup, count, and gather resolve each link into the retained generation its target-snapshot column names and have no tree equivalent — as line-for-line equivalents of `find_most_massive_progenitor()` and `gather_progenitor_galaxies()` in `src/core/build_model.c`, plus the FoF assembly built on them. At a summary level, the horizontal driver replicates: stamping `CentralMvir` from the FoF-central catalog mass onto every workspace member before physics; the new-object `SnapNum = current − 1` and `dT` sentinel; deriving `ctx->time_interval` and dynamic substep counts from the workspace's pre-marshal progenitor `SnapNum`; propagating `UniqueCentralGalaxyID` from the FoF Type 0 central to all members before physics; and stamping output `SnapNum` at marshal time. These parity behaviours are exactly what the cross-format identity gate below checks.

**Output-partition seam.** `write_master_file()` and the metadata writers no longer read `MimicConfig.vertical_reader` directly; they call `struct OutputPartitionSource get_output_partition_source(void)` (`src/io/output/util.h`), which the vertical driver populates from its reader's partition hooks and the horizontal driver from its own partition source: `MimicConfig.NOUT` partitions, one per requested output snapshot, with `partition_output_id(p)` returning `MimicConfig.ListOutputSnaps[p]` — the snapshot number, not a dense index, so filenames stay self-describing even for an unsorted `output.snapshot_list` — and `partition_snapshots(p)` returning that partition's single selection index. Under a distributed run the source enumerates `NOUT × NTask` partitions and `partition_task(p)` names each one's task; see [Distributed operation](#distributed-operation). Horizontal-run provenance records the resolved reader-format name (`horizontal_hdf5`) through this seam rather than dereferencing a `NULL` vertical reader.

**Per-partition cleanup.** The horizontal driver's incomplete-output cleanup registry has the vertical driver's shape: a partition's cleanup registration is released once its file closes successfully, exactly as the vertical driver releases its own on the success path (`src/core/vertical_driver.c:311`), and a failure removes only the in-flight partition and the (never-written) master (the `forest_chunks: 1` case, where closing a partition finalises it). Cleanup is per partition, not all-or-nothing: a finalised partition file is never partial, so it is final output and survives a later failure. Under `input.forest_chunks > 1` a partition stays in flight from the chunk that creates it to the chunk that finalises it, so the registry holds one slot per requested output snapshot and a failure removes every partition of the failing task that is not yet final (see [Chunked sweeps](#chunked-sweeps)).

**Snapshot output schema.** A horizontal run's per-snapshot HDF5 groups omit `Ntrees` and the `TreeHalosPerSnap` dataset entirely — absent, not zero or empty — because there is no per-tree structure to report; a consumer that needs one must fail loudly on the missing attribute rather than read a plausible lie. `TotHalosPerSnap` keeps its name across both drivers and is widened to `int64` (from `int`) in both the per-file writer (`src/io/output/hdf5.c`) and the master's read/republish path (`src/io/output/master_hdf5.c`), since that widening is shared writer code the vertical path shares too. A horizontal run writes one HDF5 partition file per requested output snapshot, named by that snapshot's number zero-padded to at least three digits (`model_<snapnum>.hdf5`, `%03d`-formatted — e.g. snapshot 5 is `model_005.hdf5`), plus the master; each partition file holds exactly one `Snap%03d` group, its own, and the master's per-snapshot group holds exactly one `File%03d` link resolving into that snapshot's own file. `hdf5_format_version` is `1.2` (from `1.1`), the increment the metadata writer's own rule required for the `TotHalosPerSnap` schema change — and only for that change. **Per-snapshot partitioning does not change the per-file schema: the fields, groups, datasets, attributes, and dtypes inside any one partition file are those a single-partition horizontal file would carry.** Partitioning is a property of the run's *file topology* — each horizontal-run partition carries one `Snap%03d` group rather than all of them, and there are `NOUT` such files rather than one. That distinction is why partitioning does not bump `hdf5_format_version`: the schema-version rule (`src/io/output/metadata_hdf5.c:135`) is tied to the per-file schema, not to how many files a run produces.

**The configured identity multiplier on both paths.** See [The identity multiplier](#the-identity-multiplier) above — the same `MimicConfig.UniqueGalaxyIDMultiplier` value, parsed once, is honoured by every `galaxy_id.h` helper regardless of driver, so a vertical and a horizontal run over the same catalogue with the same multiplier encode identical `UniqueGalaxyID`s for identical `(halonr, forestnr_global)` pairs.

#### Distributed operation

An MPI build run with more than one task (`mpirun -np N ./mimic <run file>`, `N > 1`) distributes the horizontal driver by **forest**. Unchunked serial runs (one task, one chunk), including an MPI build at `-np 1`, take none of the steps below, load whole slabs with `row_offset = 0` and produce exactly the output a non-MPI build produces; a serial run at `forest_chunks > 1` builds the partition and runs the checks (see [Chunked sweeps](#chunked-sweeps)). Each task may in turn sweep its forests in several chunks, which compose with the task partition below (see [Chunked sweeps](#chunked-sweeps)). The shape of the decision, in the order a run meets it:

- **Decomposition unit and partition rule.** Task `r` owns the forests whose `ForestIndex` lies in `[forest_cuts[r], forest_cuts[r+1])`, and in every snapshot slab those forests' rows are one contiguous range `[row_cuts[s][r], row_cuts[s][r+1])`. Nothing a task processes needs a row another task holds: progenitor, descendant and FoF links never leave a forest (a producer obligation the converter enforces), inheritance is pull-based through those links, and created records marshal inside their host's segment. There are no ghost regions and no communication inside the FoF sweep. The partition is the minimum-makespan contiguous partition of the widest slab's per-forest halo counts (`src/core/horizontal_partition.c`): a binary search on the per-task capacity with left-to-right greedy packing, computed on task 0 and broadcast. Trailing tasks may be idle when fewer ranges than tasks are needed, and a heavy forest may share its range with lighter neighbours that fit under the optimum. It minimises the widest slab's largest per-task weight exactly and the other slabs' only approximately, and it is a pure function of (dataset, `NTask`). Task 0 logs it at INFO (each task's forest range and widest-slab weight).
- **The forest-blocked precondition and its check.** A dataset is *forest-blocked* when, in every slab, `ForestIndex` is non-decreasing along the row order. This is not a format guarantee (see the non-normative note in the [format specification](../convert/mimic-convert/HORIZONTAL-HDF5-FORMAT.md#v3-ordering-contracts)): version 3 datasets from `lhalo_binary` and `consistent_trees_hdf5` sources have it by construction, version 2 datasets (sorted by `MostBoundID`) and version 3 datasets from `consistent_trees_ascii` do not. A distributed or chunked run therefore refuses a `format_version == 2` dataset at startup with a message saying distribution needs a forest-blocked version 3 dataset, and task 0 streams every slab's `ForestIndex` through `scan_forest_index`, aborting on the first descent with a message naming the snapshot, the two rows and values and the dataset's `source_format`. Unchunked serial runs never run either check. All tasks open the dataset (`validate_columns = 1` on task 0 only), so an `open_run` abort is identical everywhere; a task 0 abort before the partition broadcast ends the job through `MPI_Abort` (below).
- **Per-snapshot acquisition and the rebase.** Each task loads its own row range of each snapshot (`load_slab`, above) and then rewrites the five link fields to **local** row indices through the generated setters: `FirstHaloInFOFgroup` and `NextHaloInFOFgroup` minus the slab's `row_offset`, and `FirstProgenitor`, `NextProgenitor` and `Descendant` minus `row_cuts[t][r]`, where `t` is the link's target snapshot from the version 3 columns. A rebased value outside the task's range of the target snapshot aborts naming the link, the snapshot, the global row and the target snapshot ("the forest is cut by the partition"): that is a dataset defect or a partition bug, never something to repair. Everything downstream (FoF discovery, `count_fof_subhalos`, progenitor resolution, inheritance, the virial helpers, marshalling, output) then runs unchanged on a slab that is locally self-consistent. `HaloNr` stays the local row, because the output conversion indexes the slab view with it.
- **Identity.** Tree-row `UniqueGalaxyID` is unchanged (it comes from the slab's `ForestIndex` and `HaloRankInForest` columns). Created-record identity keeps `unit = snapnum` and `rows_per_unit` = the largest **global** slab, and encodes the host's global row, `HaloNr + row_offset` (see [Record Creation Contract](#record-creation-contract)), so created ids are identical across task and chunk counts. Computing `rows_per_unit` from local counts would change them.
- **Per-task accounting.** `input.retention_memory_ceiling_mb` and the retention accounting are **per task**: a generation's footprint is computed from the task's local row counts, plus the partition tables (the `struct HorizontalForestPartition` itself, its `NTask + 1` forest cuts and its `snapshot_count × (NTask + 1)` row cuts at one chunk (`NTask·nchunk + 1` and `snapshot_count × (NTask·nchunk + 1)` when chunked, see [Chunked sweeps](#chunked-sweeps)), 8 bytes each, on every task; the `n_forests_total × 8` byte weights are transiently on task 0 and reported rather than counted), with the same exclusions as today. `rows_per_unit` and the `INT_MAX` emittability check use the global counts. A forest is never split, so the largest forest's share of the widest slab is a floor on what any task count achieves.
- **Output layout.** Every task writes one HDF5 partition per requested output snapshot, named `<base>_<snap:03d>_task<task:03d>.hdf5`, including an empty partition for a task with no galaxies there; task 0 writes the master after the existing barrier, with external links `Snap<snap:03d>/File<snap:03d>_task<task:03d>` for every `(snapshot, task)` and `TotHalosPerSnap` republished per link. The horizontal partition source enumerates `NOUT × NTask` partitions (`partition_task(p)` returns the task, -1 for no task component, which the vertical source and every serial run return), a task writes only its own, and the master path is armed on task 0 only so a failing task never unlinks it. Under `NTask <= 1` names and master groups are today's. `RunProperties/NCores` records the task count; the plot reader iterates every `File*` group, so it needs no change.
- **Thread model.** Tasks are single-threaded (`MPI_Init` as before). Every MPI call the horizontal driver makes sits between FoF sweeps (the partition broadcast at startup, the snapshot collectives inside `execute_post_snapshot()`, and the pre-master barrier), none inside a callback, so thread-per-forest parallelism inside a task needs only `MPI_THREAD_FUNNELED`.
- **MPI lifecycle fixes (all drivers).** Three hygiene fixes landed with this work and also apply to a vertical `mpirun`: `myexit()` calls `MPI_Abort` when `NTask > 1` and the exit code is non-zero (after removing that task's in-progress outputs), so a failing task ends the job instead of leaving the others waiting at the barrier; `write_run_metadata()` runs on task 0 only, instead of every task truncating the same files; and every log line (`log_message()` and `log_io_error()`, through their shared emitter) carries a `task <n>:` prefix when `NTask > 1`. A vertical run also marks each partition in flight with a `<basename>_<NNN>.inflight` file in the output directory from just before its output files are claimed until they are closed (`claim_and_process_partition()`, `src/core/vertical_driver.c`), so the partitions an aborted multi-task run or a killed process left mid-write keep their marker; `--skip` redoes any marked partition, skips an unmarked one whose files all exist, and is fatal on an unmarked one with only some of its files.
- **Snapshot modules.** A `post_snapshot` module sees only its task's rows; see [Snapshot Callback Contract](#snapshot-callback-contract) for the collectives and the `snapshot_distribution` declaration.
- **Proof.** The acceptance predicate is per-`UniqueGalaxyID` byte equality of every field, tree and created rows, every output snapshot, between a serial non-MPI build and an MPI build at 1, 2, 3, 4 and 8 tasks, with no tolerance (the one documented exception is the last-bit difference `module_snapshot_sum_f64` may show in a log line). `make tests-distributed` proves it on the committed `forest_blocks` fixture for `halos-only`, `sage16`, `sham` and `hod`, and runs the collectives' MPI control test; non-MPI unit builds compile only the serial paths, so the MPI paths are proven by that gate. Task diagnostics inside the sage16 modules print task-local rows; the reader's link diagnostics print snapshot rows (`row_lo + i`).

#### Chunked sweeps

`input.forest_chunks: G` (`MimicConfig.ForestChunks`, default 1) bounds a task's memory by a size the run file chooses rather than by the widest slab the dataset produces. It works on one task or under MPI, with the same precondition as distribution: a forest-blocked version 3 dataset. A forest is never split, so the largest forest's share of the widest slab is a floor on what any `(NTask, G)` achieves.

- **The chunk is the range unit.** Each task's forest range is divided into `G` contiguous sub-ranges, its chunks, and the task runs the whole snapshot loop once per chunk, in order, loading only that chunk's rows of every slab. A chunk is the same object a task's range is (a contiguous forest range whose rows are one contiguous range of every slab), so links never leave it, inheritance stays pull-based inside it, created records marshal inside their host's segment, and the rebase of [Distributed operation](#distributed-operation) applies unchanged to the chunk's range.
- **Two-level partition.** The task cuts are exactly those of a distributed run, so `G` never changes which forests a task owns or which rows its partition files hold. Each task's range is then cut into `G` chunks by the same minimum-makespan algorithm over that range's weights (`horizontal_partition_cut_chunks()`). `struct HorizontalForestPartition` carries `nchunk`; its `forest_cuts` hold `ntask·nchunk + 1` entries and its `row_cuts` hold `snapshot_count × (ntask·nchunk + 1)`, row-major by snapshot, so range `t·nchunk + c` is task `t`'s chunk `c` and entry `t·nchunk` is task `t`'s first cut (`horizontal_partition_range_count()` is `ntask·nchunk`). `G = 1` reproduces the task tables entry for entry, and on one task the `G` chunks are the ranges `G` tasks would own, so a serial run at `forest_chunks: G` loads exactly the rows `mpirun -np G` loads, one range at a time. Trailing chunks may be idle. A chunked run on one task computes the partition on that task and broadcasts nothing; under MPI the larger tables are broadcast from task 0 as before.
- **The per-chunk loop.** `horizontal_sweep_chunk()` runs the snapshot loop for `range = task·nchunk + c`: row ranges, `identity.row_offset`, the rebase, the coverage check, sizing and the ceiling all use the chunk's rows. Every generation is released by the end of a chunk's sweep (a still-retained generation is fatal, naming the chunk); the spare pools, the FoF workspace and the scratch carry over to the next chunk. Modules are initialised and cleaned up once per process, and no shipped FoF module keeps cross-snapshot state, so returning to snapshot 0 for the next chunk changes nothing a module sees.
- **Per-chunk accounting.** The retention accounting, `horizontal_require_generation_fits()`, the ceiling and the in-sweep warning are the chunk's: a generation's footprint is computed from the chunk's local counts, plus the resident capacity of every galaxy pool, active and spare (a released pool keeps its capacity, so spare capacity carries an earlier chunk's high-water mark into later chunks), plus the partition tables, with the same exclusions as before. The run's `R` term and `max_retained_*` are maxima over chunks. `rows_per_unit` stays the global largest slab and `row_offset` is the chunk's first row (`row_cuts[s][range]`), so created-record ids are the same at every `G` and every task count.
- **Per-visit output and the registry.** The output layout is unchanged at every `G`: the same partitions, names, master links and `TotHalosPerSnap`. A partition is created, with its empty `Galaxies` table and per-file `RunProperties`, when chunk 0 reaches its snapshot; each later chunk reopens it read-write (`reopen_hdf5_output_file()`) and appends through the batched `H5TBappend_records` path; the last chunk stamps `TotHalosPerSnap` and finalises it. A chunk with no rows at a snapshot still takes its turn, so an empty partition is still created and finalised. At most one writable output file is open at a time. Chunks are ascending row ranges and the sweep visits FoF groups in row order, so a partition's rows are in the same order at every `G`. The cleanup registry is an allocation-free table with one in-flight slot per requested output snapshot: a partition is armed at creation and released at finalisation, so under `G > 1` a failure removes every partition of the failing task that is not yet final, while partitions finalised earlier survive.
- **The snapshot scope is refused.** A chunked sweep never holds a whole snapshot's population at once, within a task or collectively, so `modules.post_snapshot` with `forest_chunks > 1` is rejected at configuration time, naming the phase, the first module and the chunk count. Principle 4 defines the snapshot scope's determinism over the complete population and the collectives synchronise tasks at one moment, which sequential chunks never share. `sham_rank_match` therefore needs `forest_chunks: 1`, and so does `hod_populate` whenever its whole-box audit (its `post_snapshot` phase) is configured; `sage16`, `halos-only` and `hod_populate`'s full-halo step alone are chunkable.
- **Startup refusals and logging.** A chunked run applies the `format_version 2` and not-forest-blocked refusals exactly as a distributed one, with messages naming both the task count and `input.forest_chunks`; a serial `G = 1` run never runs the checks. At `G > 1` task 0 logs the partition (`Chunked horizontal partition` on one task, `Distributed horizontal partition` under MPI, each naming the chunk count), each chunk's forest range and widest-slab weight, and each chunk's sweep opens with one `INFO` line naming it; the `Loaded snapshot` row note names the chunk and the progress bar spans `snapshot_count × G` steps. At `G = 1` every line is unchanged. The width messages name `input.forest_chunks` as the lever.
- **The floor.** A forest is never split, so the largest forest's share of the widest slab bounds what any `(NTask, G)` saves: 1.60% on micro-Uchuu, which leaves room to chunk nearly linearly (`sage16`'s measured peak RSS fell nearly as `1/G`), and 61.86% on Shin-Uchuu's percolation super-forest, which neither distribution nor chunking can bring below that forest's own rows. Splitting a forest would need an out-of-core retained store or a descendant-locality row order with a cross-boundary reach (a format and converter change), and is out of scope. Version 2 and ASCII-sourced version 3 datasets are not chunkable.
- **Proof.** `make tests-distributed` also runs chunked serial legs at `forest_chunks` 2, 3 and 8 and MPI legs at `-np 2 × 2` and `-np 3 × 3` for `halos-only`, `sage16` and `hod` (through a run file without the snapshot audit), requiring per-`UniqueGalaxyID` identity with the `G = 1` run (and, on the serial legs only, the same partition row order; the MPI legs get the task-layout checks), and checks that `sham` and the shipped `hod` run file are refused. The fixture integration test `test_chunked_sweep.py` covers the serial legs and the multi-visit failure window.

#### The cross-format identity gate

The gate proves the two drivers agree: for every output snapshot, the same set of `UniqueGalaxyID`s and per-ID bitwise-identical fields, aggregated across every output partition, with no tolerance of any kind. It compares `micro-uchuu-ascii` read vertical against `micro-uchuu-ascii-horizontal` read horizontal, under **both** models (`halos-only`, then `sage16`) and **both** timestep schemes (fixed, then dynamic).

- **Comparator** — `scripts/compare_cross_format_identity.py`: one implementation of the frozen algorithm (duplicate-ID assertion, then ID-set equality, then per-field raw-byte comparison). Its own adversarial self-test is `tests/scientific/test_compare_cross_format_identity.py`, which runs on every default-pair suite: it synthesises tiny HDF5 runs and checks that the comparator reports each kind of disagreement, and separates unreadable input (exit 2) from a real difference (exit 1). Run it after any comparator change.
- **Harness/test** — every gate is a thin package-local file, `simulations/<package>/_tests/scientific/test_cross_format_identity.py`, holding one `GatePackage` (the vertical and horizontal packages, file range, format version, `source_format`, `column_mapping_sha256`, `links_adjacent`, halo, forest and gap census, models, schemes) over the shared harness `tests/framework/parity_gate.py` (`ParityGate`). Being package-local, a gate is registered by the scientific-tier test registry only when its package is selected. The harness builds each `{model} × {vertical, horizontal}` pair in its own git worktree at the one HEAD it captures up front (never touching the ambient tier build), pins the dataset's provenance, checks the run files against HEAD, and for every leg asserts that both runs recorded `RunProperties/TimestepScheme` equal to the leg's scheme before running the horizontal worktree's copy of the comparator. The pure run-file helpers (`dynamic_variant`, `range_override_variant`, `assert_horizontal_run_file_diff`) are covered in the core tier by `tests/integration/test_parity_gate_helpers.py`. The `micro-uchuu-ascii-horizontal` (version 2) gate subclasses `ParityGate`: it runs `sage16` only after both `halos-only` legs pass, requires the vertical run to write several partitions, and appends Stage 8, which re-proves the vertical path byte-identical against the `BASELINE_COMMIT` reference; it gains from the harness the provenance stage (format version, `links_adjacent`, forest and halo totals; version 2 carries no gap census or source-file inventory), the per-use comparator re-check, the `TimestepScheme` guard and the leg-verdict stage.
- **How to run it** — `make MODEL=halos-only SIMULATION=micro-uchuu-ascii-horizontal tests-scientific`, on a machine holding both the `micro-uchuu-ascii` and `micro-uchuu-ascii-horizontal` datasets. This is a **manual, dataset-present operation**: it is not part of the default-pair suite or CI, takes on the order of hours (four builds, nine full runs), and fails loudly rather than skipping when a dataset is absent.

Each version 3 package carries its own gate over the same harness, comparing it against the vertical package of its own source format: `simulations/mini-millennium-horizontal/_tests/scientific/test_cross_format_identity.py` runs all four `{halos-only, sage16} × {fixed, dynamic}` legs, and the four `halos-only`-gated version 3 packages — `micro-uchuu-horizontal`, `micro-uchuu-hdf5-horizontal`, `millennium-horizontal` and `mini-uchuu-horizontal` — run the two `halos-only` legs, pinning the dataset's provenance (format version, `source_format`, `column_mapping_sha256`, `links_adjacent`, halo, forest and gap totals, longest span, the `forests.h5` source-file inventory, and the file range recorded by both runs). They use the same comparator unchanged and fail, never skip, when data is missing or is not the pinned conversion. Each package's `_tests/integration/test_schema_conformance.py` is likewise one `SchemaPackage` over `tests/framework/schema_conformance.py`. Run each with `make MODEL=halos-only SIMULATION=<package> tests-scientific`.

### Converting Trees to Horizontal Input

The converter, `convert/mimic-convert/`, is an offline Python tool that sits beside `plot/mimic-plot/` rather than inside the executable: it turns a simulation's forest-ordered trees (L-Halo binary, Consistent-Trees forests-HDF5 or Consistent-Trees ASCII) into the snapshot-major horizontal HDF5 dataset the `horizontal_hdf5` reader consumes. `convert_trees.py` writes `format_version = 3`, lossless, keeping skipped-snapshot links, int64 indices and each source's native units and precision; the legacy `convert_ctrees.py` writes `format_version = 2` from Consistent-Trees ASCII. Neither is linked into `mimic`; the runtime depends only on the format specification, not on the converter's code.

A developer needs the converter when bringing a simulation to the horizontal driver. The workflow has four parts:

1. **Profile.** Convert the trees under a package profile, `simulations/<package>/converter_columns.yaml`, kept in the vertical package that already reads the source. It selects the columns and fixes the mapping whose `column_mapping_sha256` the dataset records.
2. **Package.** Create a horizontal simulation package whose `halo_properties.yaml` matches the dataset's `/schema` group — type, units and `h_convention` of every declared field — with `input.tree_type: horizontal_hdf5`, `input.processing_order: horizontal` and a `snapshots/` link to the converted dataset. There is one package per simulation and source format, because the dataset keeps its source's native units: a package built from L-Halo binary and one built from forests-HDF5 are different packages even for the same simulation.
3. **Schema conformance.** Add `_tests/integration/test_schema_conformance.py`, one `SchemaPackage` over `tests/framework/schema_conformance.py`, which checks the package's declarations against the dataset's `/schema`.
4. **Parity gate.** Add `_tests/scientific/test_cross_format_identity.py`, one `GatePackage` over `tests/framework/parity_gate.py`, comparing the horizontal package against the vertical package of the same source format over the same files (see [The cross-format identity gate](#the-cross-format-identity-gate)), and run it with `make MODEL=halos-only SIMULATION=<package> tests-scientific` on a machine holding both datasets.

Conversion evidence is not runtime evidence. The converter's validation battery (`convert_trees.py validate`) and its cross-checks establish that a dataset faithfully represents its source; only a passing parity gate establishes that the horizontal driver reproduces the vertical result on that route, and a route is listed as supported in the specification's [V3 Runtime Support](../convert/mimic-convert/HORIZONTAL-HDF5-FORMAT.md#v3-runtime-support) table only on that basis. Record the conversion command, converter commit and profile checksum in the package README so the gate's pinned provenance can be reproduced.

Commands, routes, field selection and restart behaviour are in the converter manual, [`convert/mimic-convert/README.md`](../convert/mimic-convert/README.md); the on-disk contract is [`convert/mimic-convert/HORIZONTAL-HDF5-FORMAT.md`](../convert/mimic-convert/HORIZONTAL-HDF5-FORMAT.md). `simulations/mini-millennium-horizontal/` is the worked example: its README records the conversion, and its `_tests/` holds the schema-conformance test, the four-leg parity gate against `mini-millennium`, and small committed fixture datasets.

---

## Testing

Mimic uses three test tiers. Every tier runs the core tests, selected-simulation tests under `simulations/<SIMULATION>/_tests/`, and, for full-validation simulations, tests declared by the selected model package. Empty generated lists are valid; if a simulation or model has no tests in a tier, that tier still runs the core tests and exits successfully. Unit and integration tiers can each take about three minutes; scientific validation is usually shorter, around tens of seconds for the shipped configuration. The quick-reference version of this section is [tests/README.md](../tests/README.md).

| Tier | Command | Scope |
| --- | --- | --- |
| Unit | `make tests-unit` | C unit tests for core functions, selected-simulation fixtures, selected-model modules, and infrastructure |
| Integration | `make tests-integration` | End-to-end Python tests for core workflows, selected-simulation fixtures, and selected-model modules |
| Scientific | `make tests-scientific` | Core scientific contracts plus selected-simulation and selected-model scientific regressions |

Run everything:

```bash
make tests
```

To see only warnings, failures, skipped tests, and final suite outcomes, add the `summary` goal modifier (e.g. `make tests summary`).

Summary mode works by filtering for structured result markers. Every test emits one of:

```text
MIMIC_RESULT: PASS <test_name>
MIMIC_RESULT: FAIL <test_name> [-- <reason>]
MIMIC_RESULT: SKIP <test_name> [-- <reason>]
MIMIC_RESULT: WARN <test_name> [-- <reason>]
MIMIC_RESULT: ERROR <test_name> [-- <reason>]
```

Summary mode filters structured markers directly: pass markers are suppressed, while fail, skip, warning, and error markers are shown. The filter is deterministic by design, with no natural-language heuristics or exclusion lists. New tests must emit these markers:

- **C unit tests** — use `TEST_MARKER_*` macros from `tests/framework/test_framework.h`. `TEST_RUN` and `TEST_ASSERT*` emit them automatically; no per-test changes needed. To skip a test that cannot run in this configuration, `return TEST_SKIP_WITH("reason")` — `TEST_RUN` emits the SKIP marker and counts it separately from passes.
- **Python tests** — call `result_pass / result_fail / result_skip / result_warn / result_error` from `tests/framework` in the `main()` loop. Raise `TestSkipped` to skip; the standard loop pattern catches it and calls `result_skip` automatically.

For long-running test sessions, capture logs and check the exit code:

```bash
mkdir -p archive/test-logs
make tests > archive/test-logs/tests.log 2>&1
test_rc=$?
tail -n 80 archive/test-logs/tests.log
rg -n "^MIMIC_RESULT: (FAIL|SKIP|WARN|ERROR)" archive/test-logs/tests.log
rg -n -i "traceback|fatal|segmentation fault" archive/test-logs/tests.log
echo "exit_code=${test_rc}"
```

A non-zero exit code is a failure even if the log text looks harmless.

### Test Templates

Starting points for new tests live in `tests/framework/`: `c_unit_test_template.c`, `python_integration_test_template.py`, and `python_scientific_test_template.py`. Each template's header documents where to copy it, how to register the test, and what its tier should (and should not) validate — they are written for any model package, not just the bundled ones.

### Unit Tests

Module unit tests live in `models/<model>/modules/<module>/_tests/` and are registered in `module_info.yaml`:

```yaml
tests:
  unit: _tests/test_unit_my_module.c
```

Run a specific unit test from the repository root:

```bash
tests/unit/run_tests.sh test_unit_my_module
```

The runner compiles tests on demand and uses generated module/test registries.

New sage16 unit tests should include the shared fixture header instead of re-declaring the common boilerplate (test counters, `reset_config()`, `ensure_modules_registered()`, `free_test_halo()`):

```c
#include "modules/_tests/sage_test_fixtures.h"
```

Model-level tests that span multiple modules live in `models/<model>/modules/_tests/` and are registered by `models/<model>/modules/_tests/module_info.yaml`.

### Integration and Scientific Tests

Run Python tests by path:

```bash
python3 tests/integration/test_full_pipeline.py
python3 models/<model>/modules/my_module/_tests/test_integration_my_module.py
python3 tests/scientific/test_scientific.py
```

Use the Python virtual environment when tests need plotting or scientific Python dependencies:

```bash
source mimic_venv/bin/activate
```

### Tests on Horizontal Packages

The generic tiers run on every simulation package, and the run files they use are generated for the selected package's declared processing order: `scripts/generate_test_inputs.py` reads `input.processing_order` from the package's `simulation_info.yaml` and its `_tests/input/test_simulation.yaml` (`package_processing_order()` in `scripts/discovery.py`, which stops if the two disagree), never from the package's name. A vertical package gets the binary and HDF5 run files it always had; a horizontal package gets HDF5-only run files with no input file range, because a horizontal run cannot write binary output and its reader takes its file set from the snapshot list. The generated `manifest.json` records the processing order, the run files written and, when the tier cannot run, a `skip_reason`.

A horizontal package runs the generic tiers only on committed fixture data, which its `_tests/input/test_simulation.yaml` points at. A horizontal package without that file has no runnable test dataset, so every test that needs a generated run file skips with the manifest's `skip_reason` and nothing opens the production data. Each fixture is small, synthetic and rebuilt by the `regenerate.sh` beside it under the package's `_tests/data/`; it exercises the package's compiled schema, reader and driver, not its halo population.

When writing an integration test:

- Take a run from `default_run_file()` in `tests/framework/harness.py` (or let `create_test_param_file()` default to it) rather than naming `test_binary.yaml` or `test_hdf5.yaml`. It returns the binary run file on a vertical package, as before, and the HDF5 one on a horizontal package.
- Guard behaviour that only a vertical package has (binary output, `--skip`, an input file range, forest partitioning, MPI ranks or a vertical reader) with `skip_if_selected_package_is_horizontal("<what the test needs>")`. The argument completes the sentence "this test needs ...", so the skip reason names what is missing. Do not hard-code an output format or a package name to decide.

The selected model's own tests run only on the vertical packages listed in `FULL_MODEL_TEST_SIMULATIONS` in `scripts/discovery.py`, never on a horizontal package, because the `sage16` module tests read the binary `model_z0.000_0` output that a horizontal run cannot write; a horizontal package's physics is covered by its parity gate against the vertical package of the same source. Which packages ship fixtures, and the full target list, are in [tests/README.md](../tests/README.md).

---

## Development Workflow

Daily loop:

```bash
make validate-modules
make generate
make
./mimic --debug models/sage16/input/sage16_mini-millennium.yaml
make check-docs
make tests
```

Format code before requesting review or committing:

```bash
./scripts/beautify.sh
```

Use focused tests while developing, then broader tests before handing work over.

### Documentation Ownership

Keep documentation close to the decision it supports:

| File or location | Owns |
| --- | --- |
| `README.md` | Project overview and shortest viable first run |
| `docs/VISION.md` | Stable architecture principles and boundaries |
| `docs/USER-GUIDE.md` | User workflows, configuration, output, plotting, and troubleshooting |
| `docs/DEVELOPER-GUIDE.md` | Extension workflows, APIs, metadata, tests, and development practices |
| `docs/STYLE-GUIDE.md` | Naming, comments, documentation, metadata, tests, and review conventions |
| `CHANGELOG.md` | What each release changed and what it claims |
| `convert/mimic-convert/README.md` | Converter usage: converting merger trees to horizontal input |
| `convert/mimic-convert/HORIZONTAL-HDF5-FORMAT.md` | The horizontal input contract (`format_version` 2 and 3) |
| `models/<model>/modules/<module>/README.md` | Module-local physics contract, dependencies, parameters, events, and tests |
| Metadata and generated output | Exhaustive field lists, unit labels, module registries, and event IDs |

Prefer prose in guides when it explains decisions and tradeoffs. Prefer links to metadata or code when a list is mechanical, generated, or likely to drift.

### Code Generation

Run `make generate` after editing the default package pair, or add `MODEL=<name> SIMULATION=<name>` when working on another pair:

- `src/core/core_properties.yaml`
- `simulations/<SIMULATION>/halo_properties.yaml`
- `models/<MODEL>/model_properties.yaml`
- any `module_info.yaml`
- module layout that affects discovery

Use the same model and simulation selectors for generation, validation, tests, and build. For the default packages, plain `make generate` and `make` are enough; for a non-default pair, add the same `MODEL=<name> SIMULATION=<name>` values to each command.

To change the project default (e.g. when promoting a new model or simulation package), update `DEFAULT_MODEL` and/or `DEFAULT_SIMULATION` in the Makefile. `scripts/lib/defaults.sh` reads these values at runtime, so `scripts/benchmark_mimic.sh`, `scripts/regenerate_baseline.sh`, and `plot/mimic-plot/tests/test_plotting.sh` all pick up the new defaults automatically. Also update the `model.name` and `simulation.name` fields in the affected model input YAML files to match.

The generator and validator scripts share a few single-source helpers rather than re-implementing them per file: `scripts/discovery.py` resolves the selected model/simulation package paths, and `scripts/console.py` (Python) and `scripts/lib/colors.sh` (shell) provide the common `ERROR:`/`WARNING:` coloured console output. Colour is emitted only when stdout is a TTY and `NO_COLOR` is unset, so piped or CI output stays free of escape codes.

Generated files include:

| Generator | Inputs | Outputs |
| --- | --- | --- |
| `scripts/generate_properties.py` | `src/core/core_properties.yaml`, `simulations/<SIMULATION>/halo_properties.yaml`, `models/<MODEL>/model_properties.yaml` | `src/include/generated/property_defs.h`, `populate_halo_payload.inc`, `property_test_helpers.h`, `copy_to_output.inc`, `hdf5_field_*.inc`, `output_schema_writer.inc`, and `tests/generated/property_ranges.json` |
| `scripts/generate_module_registry.py` | selected model `shared/module_info.yaml`, module `module_info.yaml` files, and standalone module files | `src/module_system/generated/module_init.c`, `src/module_system/generated/event_contracts.h`, `tests/generated/module_sources.txt`, and `build/generated/module_registry_hash.txt` |
| `scripts/generate_test_registry.py` | core tests plus selected simulation and model test metadata | `build/generated/unit_tests.txt`, `integration_tests.txt`, `scientific_tests.txt`, and `test_registry_hash.txt` |
| `scripts/generate_test_inputs.py` | selected model and simulation package metadata | shared test run files under `build/generated/test_inputs/<MODEL>/<SIMULATION>/` |

Use:

```bash
make check-generated
```

to verify ignored generated files are current after generation.

### Benchmarking

Use `scripts/benchmark_mimic.sh` when you need a repeatable runtime and memory baseline before or after performance-sensitive changes. The default invocation benchmarks `models/sage16/input/sage16_mini-millennium.yaml` and writes a timestamped JSON result under `benchmarks/`:

```bash
./scripts/benchmark_mimic.sh
```

For a faster generated test input or a specific run file:

```bash
make MODEL=sage16 SIMULATION=mini-millennium generate-test-inputs
./scripts/benchmark_mimic.sh --param-file build/generated/test_inputs/sage16/mini-millennium/core/test_binary.yaml
./scripts/benchmark_mimic.sh models/sage16/input/sage16_mini-millennium.yaml
```

Run `./scripts/benchmark_mimic.sh --help` for MPI, HDF5, and custom build-flag options.

---

## Debugging

### Broken Module Startup

Start with metadata validation:

```bash
make validate-modules
```

Common failures:

| Message | Meaning | Fix |
| --- | --- | --- |
| Missing `supported_processing_modes` | Directory module metadata is incomplete | Add supported modes |
| Module name mismatch | Directory and `module.name` disagree | Rename one side |
| Unknown property dependency | Metadata references a property not in property YAML | Fix spelling or add the property |
| Invalid processing mode | YAML uses an unsupported mode string | Use `process_full_halo`, `process_by_galaxy`, or `process_per_event` |

Then regenerate and rebuild:

```bash
make generate
make clean && make
```

### Runtime Failures

Run with debug logs:

```bash
./mimic --debug models/sage16/input/sage16_mini-millennium.yaml 2>&1 | tee debug.log
```

If `process()` fails without a useful reason, add `ERROR_LOG()` immediately before the failing `return -1` in the module. The core can identify the module and substep, but only the module knows the physics reason.

### Memory Issues

Use the tracked allocator for module-owned allocations:

```c
#include "util/memory.h"

double *table = mymalloc_cat(n * sizeof(double), MEM_UTILITY);
myfree(table);
```

For deeper checks:

```bash
./mimic --debug models/sage16/input/sage16_mini-millennium.yaml
valgrind --leak-check=full ./mimic models/sage16/input/sage16_mini-millennium.yaml
```

**Sanitizer builds.** AddressSanitizer and UndefinedBehaviorSanitizer catch out-of-bounds access, use-after-free and undefined arithmetic that the tracked allocator cannot see. `EXTRA_CFLAGS` and `EXTRA_LDFLAGS` add the flags to the compile and link steps without editing the `Makefile`:

```bash
make clean
make EXTRA_CFLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer" \
    EXTRA_LDFLAGS="-fsanitize=address,undefined"
```

Add `MODEL=<name> SIMULATION=<name>` when you are not working on the default pair, then run a run file under that build as usual. To sanitize a test tier, pass the same `EXTRA_CFLAGS`/`EXTRA_LDFLAGS` on every `make` command that builds or runs a tier (`make tests-unit`, `make tests-integration`, `make tests-scientific`, `make tests-horizontal-v3`), because each rebuilds through `make`; `tests/unit/run_tests.sh` honours both hooks so the C unit tests are instrumented too, and a plain `make` afterwards rebuilds without sanitizers. Each sanitizer report prints a stack trace, and an AddressSanitizer error stops the run. On macOS Clang, where LeakSanitizer is unsupported, set `ASAN_OPTIONS=detect_leaks=0`; Mimic's own allocator leak report still runs at exit.

---

## Reference

### Module Metadata Schema

Used by [Creating Physics Modules](#creating-physics-modules) and [Events](#events).

Required fields for directory runtime modules:

| Field | Type | Description |
| --- | --- | --- |
| `name` | string | Module name, usually matching directory and C function prefix |
| `supported_processing_modes` | array | Allowed processing modes: a non-empty list without duplicates, drawn from `scripts/module_modes.py` (`process_full_halo`, `process_per_event`, `process_by_galaxy`, `process_snapshot`). The generator and validator reject unknown, duplicate and empty lists identically |

Common optional fields:

| Field | Description |
| --- | --- |
| `description` | One-sentence module contract |
| `additional_files` | Helper source files; `{module_name}.c` is implicit |
| `dependencies.properties` | Properties used by the module, validated against metadata |
| `dependencies.parameters` | Parameter names expected in `modules.parameters` |
| `events.emits` | Events emitted by a full-halo producer |
| `events.consumes` | Producer/event subscriptions for per-event consumers |
| `tests.unit` | C unit test path |
| `tests.integration` | Python integration test path |
| `tests.scientific` | Python scientific test path |
| `docs.physics` | Module-local physics/contract documentation |
| `compilation_requires` | Required optional features such as HDF5 or MPI |
| `snapshot_distribution` | `serial_only` (default) or `collective`; only for a module that advertises `process_snapshot`. `collective` declares that the module reaches every whole-population quantity through the snapshot collectives and may run under `NTask > 1` (see [Snapshot Callback Contract](#snapshot-callback-contract)) |

The validator implementation in `scripts/validate_modules.py` is the enforcement source for this schema. Processing-mode lists are checked through the descriptors in `scripts/module_modes.py`, which the registry generator shares; that file is a module-generation input, so the generator's freshness hash and `make check-generated` both cover its path and bytes and the `Makefile` module-generation stamp depends on it.

### Property Metadata Schema

Used by [Property System](#property-system) and [Adding a New Simulation](#adding-a-new-simulation).

Required fields:

| Field | Description |
| --- | --- |
| `name` | Generated C field name |
| `type` | C/Python type such as `float`, `double`, `int`, `long`, vector types |
| `units` | Output unit label |
| `description` | Human-readable meaning |
| `output` | Whether the property is written to output |

Common initialization sources:

| Value | Meaning |
| --- | --- |
| `default` | Initialize from `init_value` |
| `copy_from_tree` | Copy scalar input tree field |
| `copy_from_tree_array` | Copy vector input tree field |
| `calculate` | Call `init_function` |
| `skip` | Custom initialization outside generated code |

Common optional fields:

| Field | Meaning |
| --- | --- |
| `source` | (Simulation catalog fields) on-disk dataset/column name; defaults to `name`. Declare only when the on-disk name differs |
| `h_convention` | Hubble-parameter convention: `carried`, `free`, or `none`; defaults to the registry value for `units` |
| `init_source` | Initialization method; defaults differ by property category and generator context |
| `output_source` | Output method; defaults to direct halo copy or galaxy-property copy when omitted |
| `init_value` | Default value or tree field, depending on `init_source` |
| `init_repeat` | Reset after inheritance each snapshot |
| `output_convert` | Unit conversion expression |
| `output_transform` | Output transform such as `log10` |
| `output_function` | Helper function for recalculated output |
| `output_function_arg` | Arguments passed to helper function |
| `range` | Validation range used by tests (output properties only) |
| `sentinels` | Values exempt from range checks and from output unit conversion / transforms (output properties only) |
| `notes` | Free-text developer notes (provenance, core-policy descriptions); not parsed or enforced by the generator |

### ModuleContext Fields

`struct ModuleContext` is defined in `src/core/module_interface.h`. Treat all fields as read-only.

Used by [Processing Modes and Phases](#processing-modes-and-phases), especially [Accessing the Central Galaxy](#accessing-the-central-galaxy), and by [Events](#events) through `active_event`.

Common fields:

| Field | Meaning |
| --- | --- |
| `redshift` | Current snapshot redshift |
| `time` | Current cosmic time in internal units |
| `snapshot_number` | Current snapshot index |
| `substep_number` | Zero-based substep index |
| `num_substeps` | Active substep count for this timestep; fixed from `SubSteps` or scheme-derived |
| `time_interval` | Full snapshot interval |
| `substep_dt` | Shared substep duration; model packages may provide per-object helpers for integration |
| `central_index` | Index of Type 0 central in the FoF workspace |
| `central_galaxy` | Pointer to Type 0 central |
| `active_event` | Event payload for `process_per_event`; otherwise `NULL` |
| `params` | Read-only pointer to `MimicConfig` |

`struct ModuleContext` carries data, not operations; the context is passed to the two operations a module may invoke, event emission (`module_emit_event(ctx, ...)`, see [Events](#events)) and record creation (`module_create_record(ctx, host_index, &row)`), and creation is legal only inside a running `process_full_halo` callback (see the [Record Creation Contract](#record-creation-contract)). `central_galaxy` is refreshed when created records are committed, so a module must not cache it, or any row pointer, across a callback boundary.

`num_substeps` is `SubSteps` under `TimestepScheme: fixed`, or computed per FoF group from the halo dynamical time under `TimestepScheme: dynamic` and capped by `MaxDynamicSubsteps` (`src/core/timestep.c`; default `DEFAULT_MAX_DYNAMIC_SUBSTEPS` in `src/include/constants.h`) — see `docs/USER-GUIDE.md` for the run-configuration view.

### Parameter Loading Macros

Definitions live in `src/module_system/parameter_helpers.h`.

Used by [Parameters](#parameters).

| Macro | Use |
| --- | --- |
| `LOAD_PARAM_DOUBLE(name, var)` | Load a double |
| `LOAD_PARAM_INT(name, var)` | Load an int |
| `LOAD_PARAM_STRING(name, var, len)` | Load a string |
| `VALIDATE_RANGE_EXCLUSIVE(param, val, min, max, msg)` | Validate `(min, max]` |
| `VALIDATE_RANGE_INCLUSIVE(param, val, min, max, msg)` | Validate `[min, max]` |
| `VALIDATE_OPTION(param, val, max, msg)` | Validate integer selector `[0, max]` |
| `LOAD_AND_VALIDATE_RANGE_EXCLUSIVE(...)` | Load double and validate |
| `LOAD_AND_VALIDATE_RANGE_INCLUSIVE(...)` | Load double and validate |
| `LOAD_AND_VALIDATE_OPTION(...)` | Load int and validate |
| `LOAD_PARAM_DOUBLE_INTERNAL(name, var)` | Load a double declared in `parameter_units.yaml`, converting it into the reference basis |
| `LOAD_AND_VALIDATE_RANGE_INCLUSIVE_INTERNAL(...)` | Load such a double and validate, in reference units |

The `*_INTERNAL` variants convert a parameter from its declared units (in `models/<MODEL>/parameter_units.yaml`) into the fixed internal reference basis on load; see [Units and the Reference Basis](#units-and-the-reference-basis).

### Memory Categories

Definitions live in `src/util/memory.h`.

Used by [Memory Issues](#memory-issues) and by modules that allocate tables or other persistent state.

| Category | Use |
| --- | --- |
| `MEM_GALAXIES` | Galaxy data |
| `MEM_HALOS` | Halo/workspace data |
| `MEM_TREES` | Tree input data |
| `MEM_IO` | I/O buffers |
| `MEM_UTILITY` | Utility and module-owned allocations |

### Logging Macros

Definitions live in `src/util/error.h`.

Used throughout module `init()`, `process()`, and cleanup paths; see [Broken Module Startup](#broken-module-startup) and [Runtime Failures](#runtime-failures).

| Macro | Visible when | Use for |
| --- | --- | --- |
| `DEBUG_LOG` | `--debug` | Detailed diagnostics |
| `VERBOSE_LOG` | `--verbose` or `--debug` | Configuration/lifecycle detail |
| `INFO_LOG` | default | Normal progress |
| `WARNING_LOG` | always | Non-fatal issues |
| `ERROR_LOG` | always | Errors before returning failure |
| `FATAL_ERROR` | always | Fatal errors that exit |

### Physical Constants

Do not duplicate the physical constants table in documentation. The source of truth is `src/module_system/physical_constants.h`. Runtime-derived unit quantities are computed in `src/core/init.c` from generated fixed reference-unit metadata, while simulation catalog values are converted at the reader boundary.

Used by [Adding a New Simulation](#adding-a-new-simulation) when defining catalog units and by model modules that need shared constants.

---

## Documentation Directory

- [README.md](../README.md): project overview and shortest path to a first result
- [VISION.md](VISION.md): architectural principles and design boundaries
- [USER-GUIDE.md](USER-GUIDE.md): installation, run configuration, output analysis, plotting, and troubleshooting
- [STYLE-GUIDE.md](STYLE-GUIDE.md): naming, comments, documentation, metadata, tests, and review conventions
- [convert/mimic-convert/HORIZONTAL-HDF5-FORMAT.md](../convert/mimic-convert/HORIZONTAL-HDF5-FORMAT.md): on-disk contract for horizontal HDF5 merger-tree input
- [plot/mimic-plot/README.md](../plot/mimic-plot/README.md): detailed plotting manual
- [convert/mimic-convert/README.md](../convert/mimic-convert/README.md): merger-tree converter manual
- [tests/README.md](../tests/README.md): test-suite quick reference
- `models/<model>/README.md`: model-package science scope, module pipeline, parameters, plots, and references
- `simulations/<simulation>/README.md`: simulation-package data, units, snapshot lists, and maintenance notes
