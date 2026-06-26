# Mimic Developer Guide

**Practical guide to extending Mimic with physics modules, properties, tests, and generated metadata.**

This guide is for contributors and researchers modifying Mimic internals: writing a new physics module, adding properties, wiring up a new simulation, or working on the framework itself. It assumes you have already run Mimic successfully — if not, start with the [User Guide](USER-GUIDE.md). For the architectural principles and design rationale behind the structures described here, see [VISION.md](VISION.md). The shipped model packages are worked examples of everything in this guide: [models/sage16/](../models/sage16/README.md) is a mature production package, and [models/sham/](../models/sham/README.md) is the minimal pattern.

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
9. [Adding a Tree Reader](#adding-a-tree-reader)
10. [Testing](#testing)
11. [Development Workflow](#development-workflow)
12. [Debugging](#debugging)
13. [Reference](#reference)
14. [Documentation Directory](#documentation-directory)

Common tasks:

- Adding a module: [Creating Physics Modules](#creating-physics-modules)
- Choosing a processing mode: [Processing Modes and Phases](#processing-modes-and-phases)
- Adding a property: [Property System](#property-system)
- Working with units (different from Millennium): [Units and the Reference Basis](#units-and-the-reference-basis)
- Loading parameters: [Parameters](#parameters)
- Adding a simulation: [Adding a New Simulation](#adding-a-new-simulation)
- Adding a tree input format: [Adding a Tree Reader](#adding-a-tree-reader)
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

    gal->ColdGas += (float)(my_efficiency * ctx->substep_dt);
    return 0;
}

int my_module_cleanup(void)
{
    return 0;
}
```

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

Mimic separates core infrastructure from physics modules. Most scientific customisation should happen under `models/<model>/` and `simulations/<simulation>/`: new physics modules, galaxy properties, model-local helpers, run files, plot definitions, catalog halo properties, and simulation metadata all belong there. Core code under `src/` is shared infrastructure; changing it is appropriate for framework work such as new tree readers, output writers, dispatch behavior, generated-code support, or memory/I/O changes, but it can affect every model package at once.

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
| `src/io/` | Tree readers and binary/HDF5 output writers |
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
| `FoFWorkspace` / `struct Halo` | Temporary processing workspace modified by modules |
| `ProcessedHalos` / `struct Halo` | Tree-driver output buffer and processed progenitor state |
| `OutputBufferSegment` | Driver-supplied range/snapshot metadata for shared output marshalling |
| `struct GalaxyData` | Generated galaxy/model property storage attached to `struct Halo` |
| `struct HaloOutput` | Generated output record written to binary/HDF5 |

Galaxy inheritance copies previous processed galaxy state into the current workspace, resets snapshot-scoped properties marked `init_repeat: true`, and updates halo properties from driver-supplied descendant data. After physics execution, the shared output-buffer marshaller copies surviving workspace entries into the driver-owned output buffer and frees Type 3 entries.

### Per-Tree Memory Lifecycle

Four arrays are allocated per unit and freed together by `free_unit_halos()` after output is written:

| Global | Category | Lifetime note |
| --- | --- | --- |
| `InputTreeHalos` | `MEM_TREES` | Fixed size: `InputTreeNHalos[treenr]` entries; immutable after load |
| `HaloAux` | `MEM_HALOS` | Fixed size: parallel to `InputTreeHalos`; tracks per-halo processing flags |
| `FoFWorkspace` | `MEM_HALOS` | Grows dynamically via `myrealloc_cat` as deep or wide FoF groups are encountered |
| `ProcessedHalos` | `MEM_HALOS` | Grows dynamically via `myrealloc_cat`; see below |

`GalaxyData` is not in this table because it is pool-managed: `galaxy_pool_alloc()` hands out slots from a per-tree chunk pool, and the whole pool is reset in one call at the end of the tree rather than per-halo.

**Why `ProcessedHalos` must grow**

`ProcessedHalos` accumulates every marshalled output halo across all snapshot intervals for the entire tree. Each time a FoF group is processed, `marshal_workspace_to_output_buffer` appends the surviving workspace entries. The initial allocation is `MAXHALOFAC (5) × InputTreeNHalos`, but this is only an estimate. Orphan halos (Type 2) persist across snapshots and produce one new output record per snapshot they survive; in deep simulations with many snapshots (e.g., full Millennium at 64 snapshots), a single catalog subhalo that disappears early can generate dozens of output records. The actual count therefore scales with simulation depth and cannot be bounded by a fixed multiple of the tree input size.

`marshal_workspace_to_output_buffer` grows the buffer using the same factor / minimum / cap policy as `FoFWorkspace` (`HALO_ARRAY_GROWTH_FACTOR`, `MIN_HALO_ARRAY_GROWTH`, `MAX_HALO_ARRAY_SIZE`). After each marshal call, `build_halo_tree` syncs the global `ProcessedHalos` pointer and `MaxProcessedHalos` back from the `OutputBuffer` struct. `myfree(ProcessedHalos)` in `free_unit_halos()` correctly frees the final (possibly grown) allocation because the custom allocator tracks the pointer through every `myrealloc_cat` call.

**OutputBuffer contract**

`marshal_workspace_to_output_buffer` takes a `struct OutputBuffer *`. The `halos` field must be a tracked heap allocation (`mymalloc_cat` or `myrealloc_cat`); passing a stack array will produce a fatal error on overflow because the allocator cannot find a stack address in its tracking table. After the call, callers must read back `buffer->halos` and `buffer->capacity` if they mirror those values in globals.

### Module Lifecycle

Every runtime module implements three functions named after the module:

```c
int module_name_init(void);
int module_name_process(struct ModuleContext *ctx, struct Halo *halos, int ngal);
int module_name_cleanup(void);
```

Lifecycle behavior:

- `init()` runs once at startup. Load and validate module parameters here.
- `process()` runs during configured phases. Return non-zero after logging an `ERROR_LOG()` message if the module cannot continue.
- `cleanup()` runs during shutdown. Free module-owned memory here.

Return conventions:

- `init()` non-zero: startup aborts.
- `process()` non-zero: Mimic exits with failure.
- `cleanup()` non-zero: error is logged and cleanup continues for other modules.

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
- supported modes: `process_full_halo`, `process_per_event`, and `process_by_galaxy`
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

---

## Processing Modes and Phases

### Processing Modes

| YAML mode | C enum | Module receives | Use for |
| --- | --- | --- | --- |
| `process_full_halo` | `PROCESSING_MODE_FULL_HALO` | Entire FoF workspace, `ngal >= 1` | Calculations needing central plus satellites; event producers |
| `process_per_event` | `PROCESSING_MODE_PER_EVENT` | One event target, `ngal = 1`, `ctx->active_event != NULL` | Physics triggered by emitted events |
| `process_by_galaxy` | `PROCESSING_MODE_BY_GALAXY` | One galaxy, `ngal = 1` | Local per-galaxy physics and time integration |

Choose the narrowest mode that gives the module the context it needs. A module that only modifies one galaxy at a time should usually use `process_by_galaxy`. A module that redistributes reservoirs across a FoF group or emits merger events should use `process_full_halo`.

### Phase Order

For each snapshot interval:

```text
pre_timestep
for each substep:
  each modules.phases entry in declared order
post_timestep
```

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

### Units and the Reference Basis

Mimic runs entirely in one fixed internal reference basis, declared once in `src/core/core_properties.yaml` under `reference_units`:

| Dimension | Reference unit | `h_convention` |
| --- | --- | --- |
| mass | `1e10 Msun/h` | `carried` |
| length | `Mpc/h` | `carried` |
| velocity | `km/s` | `none` |
| time | derived (`length/velocity`) | `carried` |

Every quantity entering from a catalog or a parameter declares its own units and is converted into this basis at the boundary, so internal code and output never have to ask which simulation produced a value. Two metadata fields drive conversion:

- `units` — a label from the unit registry in `scripts/generate_properties.py` (e.g. `Mpc`, `Mpc/h`, `Msun`, `1e10 Msun/h`, `km/s`). The label is the single source of dimensional truth: the registry maps each label to its dimension (`mass`, `length`, `velocity`, `time`, `dimensionless`, `count`, …) and default `h_convention`.
- `h_convention` — whether the value carries the Hubble parameter: `carried` (h folded in, e.g. `Mpc/h`), `free` (h divided out / physical, e.g. `Mpc`), or `none` (h-independent, e.g. `km/s`). Defaults to the registry value for the label.

The generator emits a linear conversion — a cgs scale factor, plus a factor of `MimicConfig.Hubble_h` where source and target are both h-dependent but differ between `carried` and `free` — into `src/include/generated/unit_registry.h`. Catalog fields are converted at the tree-reader boundary; output labels come from the reference basis, so a written value always matches its label. Values with `h_convention: none` are h-independent and cannot be converted to or from h-dependent conventions. Millennium catalogs are already in the reference basis, so their conversion is the identity and output stays byte-identical.

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
  processing_order: tree_ordered  # optional; processing driver selector
  simulation_dir: ./simulations/my_sim/snapshots/
  snapshot_list_file: simulations/my_sim/my_sim.a_list

output:
  target_file_size: 4294967296  # optional soft HDF5 chunk target in bytes
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

Only catalogue-scale output planning defaults belong in `simulation_info.yaml`: `output.target_file_size` and `output.forests_per_file`. They are defaults because large Consistent-Trees catalogues impose the chunking requirement regardless of which model runs on them. Run files may override those two keys, while output paths, output format, and snapshot selection remain run-file settings. `consistent_trees_ascii` cannot derive chunk sizes from `target_file_size`, so ASCII simulation packages that use chunked output should set a positive `forests_per_file` default.

**Supported tree formats:**

| `tree_type` value | Format | Build |
| --- | --- | --- |
| `lhalo_binary` | Standard LHaloTree binary format (Springel et al.) | any |
| `lhalo_hdf5` | LHaloTree HDF5 layout (per-tree `tree_NNN/<field>` groups) | HDF5 |
| `consistent_trees_ascii` | Consistent-Trees / Rockstar ASCII output (`forests.list` + `locations.dat` + `tree_i_j_k.dat`) | any |
| `consistent_trees_hdf5` | Consistent-Trees forests-HDF5 packaging (uchuutools) | HDF5 |

`tree_type` selects a format, not a simulation: the same reader serves any simulation whose catalogue is written in that format. The HDF5-based readers are only present in an HDF5-enabled build; selecting one in a `USE-HDF5=no` build is a fatal configuration error. `processing_order` selects the processing driver independently of the reader format. It defaults to `tree_ordered`; `snapshot_ordered` is reserved for the future snapshot driver and fails fast in v1.0. To add a format of your own, see [Adding a Tree Reader](#adding-a-tree-reader) — it is a self-contained reader file plus one registry row, with no changes to the core read path.

`tree_name` is interpreted by the selected reader. `lhalo_binary` uses it as the prefix before the file number (`tree_name.<file_number>`). `consistent_trees_ascii` and `consistent_trees_hdf5` use it as a literal filename under `simulation_dir`, including any extension. `lhalo_hdf5` also uses explicit HDF5 filenames: for one file, set `tree_name` to that filename; for multiple files, include a `%d` file-number placeholder such as `trees_063.%d.hdf5`.

The `consistent_trees_hdf5` reader is the reference high-throughput HDF5 input path. It caches chunk-range `ForestInfo`, opens each per-file `Forests/<field>` dataset once for the partition lifetime, validates field extents and datatypes at cache-open time, and serves normal forests from a fixed `CTREES_READ_WINDOW_BYTES` slab window (`128 MiB` per rank). Forests larger than the window use the same cached-handle direct read primitive, so the persistent window stays bounded. Do not add run-YAML knobs or whole-file slab buffering for this path without a new plan and validation gate.

### Snapshot Scale Factor List

The `.a_list` file contains one scale factor per line, ordered from earliest to latest snapshot (increasing `a`, decreasing redshift). Mimic derives the last valid snapshot index from this file, so a file with 64 entries defines snapshots `0..63`:

```
0.0078125
0.012346
0.019608
...
1.0
```

These values drive all redshift and timestep calculations. Mimic counts snapshots by position in this file, so the ordering is critical. The last line corresponds to the highest snapshot index, normally `a = 1.0` (z = 0).

### halo_properties.yaml

This file is the single self-contained description of the simulation's on-disk halo catalog. Its `halo_properties` list contains **every** field of the on-disk catalog record, **in on-disk order**. This one list is the source of truth for the generated `struct RawHalo` (the binary record layout) and the HDF5 reader.

An entry that satisfies a core required-input role declares `provides_core_role`, using a role from `src/core/core_properties.yaml` under `required_inputs` (for example `HaloMass`, the tree links, `SnapNum`, or `Len`). The generator emits `mimic_tree_get_<Role>()` accessors from those bindings, and core tree traversal uses those accessors rather than hard-coded catalog member names. An entry that Mimic copies into a halo property and/or writes to output also carries `output`, `init_source`, `output_source`, `description`, and `range`; entries with none of those (tree links, the virial-mass input, unused accounting fields) are registered for a complete record but produce no halo property.

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

Any `input:` key in the run file takes precedence over the same key in `simulation_info.yaml`. The same precedence applies to `output.target_file_size` and `output.forests_per_file`; other `output:` keys are intentionally run-owned and are not valid in simulation metadata.

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

---

## Adding a Tree Reader

A tree reader teaches Mimic to read a new on-disk merger-tree *format*. It is independent of [Adding a New Simulation](#adding-a-new-simulation): a simulation package describes one catalogue (its cosmology, units, snapshot list, and `RawHalo` fields), while a reader describes how any catalogue stored in a given format is parsed into Mimic's halo structures. One reader serves every simulation written in its format.

Readers are registry-driven. Adding one is a self-contained implementation file plus a single row in `src/io/tree/registry.c`; the core read path in `src/io/tree/interface.c` and the driver loop in `src/core/main.c` never change.

### The reader interface

Each format defines exactly one `struct TreeReader` (declared in `src/io/tree/reader.h`). The core dispatches through its function pointers rather than switching on a format enum:

```c
struct TreeReader {
  const char *name;           /* tree_type string in the run YAML */
  const char *file_extension; /* optional fixed suffix for legacy readers */

  enum TreePartitionModel partition_model; /* per-file, enumerated, or legacy per-task */
  enum InputProcessingOrder processing_order; /* currently INPUT_PROCESSING_ORDER_TREE */

  /* PARTITION_PER_FILE and PARTITION_ENUMERATED readers: */
  int (*num_partitions)(void);
  int (*partition_output_id)(int partition);
  void (*format_partition_path)(char *buf, size_t size, int output_id);
  int64_t (*count_partition_trees)(int output_id);

  void (*open_partition)(int output_id); /* open + read the unit table */
  void (*load_unit)(int unit);           /* read one unit into InputTreeHalos */
  void (*close_partition)(void);         /* release per-partition scaffolding */
};
```

The vtable is deliberately minimal — fields exist only because a wired reader uses them. Do not add speculative callbacks; fold setup/teardown into `open_partition`/`close_partition`.

### Partition and unit model

The core iterates the input as **partitions** of **units**. A partition is the unit of output: the driver opens one set of output files per partition, names them by the partition's `output_id`, and finalises them when the partition is done. A unit is one independently processed merger structure within a partition. Current readers use these partition models:

| Model | Partition | Unit | Output id | Example |
| --- | --- | --- | --- | --- |
| `PARTITION_PER_FILE` | one input file | one tree | file number | both L-Halo readers |
| `PARTITION_ENUMERATED` | reader-defined chunk | one tree/forest | chunk or reader id | both Consistent-Trees readers |
| `PARTITION_PER_TASK` | one MPI task | one forest | `ThisTask` | legacy compatibility path; do not use for new readers |

- **`PARTITION_PER_FILE`**: the driver strides partitions across MPI tasks. Supply the enumeration via the shared helpers `tree_partition_per_file_count()` / `tree_partition_per_file_output_id()` (in `interface.c`) as your `num_partitions` / `partition_output_id`. Implement `count_partition_trees(output_id)` as an allocation-free header read so the driver can build the run-scoped global forest-offset table before processing. `open_partition(output_id)` opens file `output_id` and reads its tree table.
- **`PARTITION_ENUMERATED`**: the reader publishes a deterministic list of output partitions and costs. The driver assigns those partitions to MPI tasks, but output ids remain the reader's partition ids and therefore do not depend on `NTask`. The Consistent-Trees readers use this for chunked output.
- **`PARTITION_PER_TASK`**: legacy compatibility path where each task owns exactly one output partition and the output id is `ThisTask`. It remains only until the chunked-output cleanup removes the old ctrees per-task model; new readers should not use it.

### Where the partition model is observed outside the reader

Three pieces of the core key on `partition_model`; a new reader inherits them by setting the field correctly and supplying the matching callbacks:

- **Unique galaxy ids** — `make_unique_galaxy_id()` in `src/core/build_model.c` encodes `halonr + TREE_MUL_FAC * forestnr_global`, where `forestnr_global = GlobalForestOffset + unit`. `PARTITION_PER_FILE` readers get `GlobalForestOffset` from the driver's prefix-sum scan over present files; `PARTITION_ENUMERATED` readers publish chunk offsets; legacy `PARTITION_PER_TASK` readers set it from their global forest-distribution start. Partition ids and MPI task ranks are not part of the identity.
- **Per-file offset scan** — `run_tree_driver()` calls `count_partition_trees(output_id)` for every present `PARTITION_PER_FILE` input file before processing so missing files keep the existing skip semantics and present files receive contiguous run-scoped offsets.
- **HDF5 master file** — `write_master_file()` asks enumerable readers for their partition ids and still handles the legacy `PARTITION_PER_TASK` path until that code is removed.

### Steps

1. Create `src/io/tree/read_<format>.c` (and a `.h` only if it exposes a shared seam). Implement the callbacks and define one `const struct TreeReader <Format>Reader = { ... }`. Bridge the format's halo records into the generated `struct RawHalo` by field name; let the generated reference-unit accessors apply unit conversion at the boundary (declare native units in the simulation package, not in reader code).
2. Append one row to `reader_table[]` in `src/io/tree/registry.c`. If the reader requires HDF5, guard both the `extern` declaration and the table entry with `#ifdef HDF5` so non-HDF5 builds simply do not register it (`tree_reader_lookup` then returns `NULL` and the run fails fast with a clear message).
3. If the reader pulls in `src/io/tree/<format>/*.c` support code that compiles in every build, keep it warning-clean under `-Wall -Wextra -Wshadow -Wformat-security -Wundef` and exercise it from `tests/unit/` so nothing is dead. The unit harness enables HDF5 reader sources when HDF5 development libraries are available, while HDF5 integration paths are still validated end-to-end against real or fixture datasets.
4. A new *format* needs no run-YAML changes beyond `tree_type` when it feeds the existing `tree_ordered` driver. Set `.processing_order = INPUT_PROCESSING_ORDER_TREE` in the reader initializer. A future snapshot reader must use the snapshot-ordering contract and driver added by that later phase rather than overloading `tree_type`.

The Consistent-Trees readers (`src/io/tree/read_ctrees_ascii.c`, `read_ctrees_hdf5.c`) are the worked reference for `PARTITION_ENUMERATED` chunked readers, including forest load-balancing and the `RawHalo` bridge.

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

```
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
| `scripts/generate_properties.py` | `src/core/core_properties.yaml`, `simulations/<SIMULATION>/halo_properties.yaml`, `models/<MODEL>/model_properties.yaml` | `src/include/generated/property_defs.h`, `populate_halo_payload_from_tree.inc`, `property_test_helpers.h`, `copy_to_output.inc`, `hdf5_field_*.inc`, `output_schema_writer.inc`, and `tests/generated/property_ranges.json` |
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

---

## Reference

### Module Metadata Schema

Used by [Creating Physics Modules](#creating-physics-modules) and [Events](#events).

Required fields for directory runtime modules:

| Field | Type | Description |
| --- | --- | --- |
| `name` | string | Module name, usually matching directory and C function prefix |
| `supported_processing_modes` | array | Allowed YAML processing modes |

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

The validator implementation in `scripts/validate_modules.py` is the enforcement source for this schema.

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
| `num_substeps` | Configured number of substeps |
| `time_interval` | Full snapshot interval |
| `substep_dt` | Substep duration for integration |
| `central_index` | Index of Type 0 central in the FoF workspace |
| `central_galaxy` | Pointer to Type 0 central |
| `active_event` | Event payload for `process_per_event`; otherwise `NULL` |
| `params` | Read-only pointer to `MimicConfig` |

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
- [plot/mimic-plot/README.md](../plot/mimic-plot/README.md): detailed plotting manual
- [tests/README.md](../tests/README.md): test-suite quick reference
- `models/<model>/README.md`: model-package science scope, module pipeline, parameters, plots, and references
- `simulations/<simulation>/README.md`: simulation-package data, units, snapshot lists, and maintenance notes
