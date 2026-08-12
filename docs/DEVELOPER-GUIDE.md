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
- Working on snapshot-ordered input: [Snapshot-ordered readers](#snapshot-ordered-readers)
- Working on the snapshot driver or the cross-format identity gate: [The Snapshot Driver](#the-snapshot-driver)
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

`GalaxyData` is not in this table because it is pool-managed: `galaxy_pool_alloc()` hands out slots from a chunk pool, and the whole pool is reset in one call rather than per-halo. The pool API takes an explicit `struct GalaxyPool *` handle (`src/core/galaxy_pool.h`) rather than reaching into file-static state, so each driver owns its own instance(s) with the same chunked-allocation, stable-pointer, and bulk-reset discipline: the tree driver holds one pool, reset per tree; the snapshot driver holds two, ping-ponging a bulk reset once per snapshot (see [The Snapshot Driver](#the-snapshot-driver)).

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
| `snapshot_hdf5` | Snapshot-ordered HDF5 (`snapshot_NNN.h5` per snapshot); feeds `snapshot_ordered` | HDF5 |

`tree_type` selects a format, not a simulation: the same reader serves any simulation whose catalogue is written in that format. The HDF5-based readers are only present in an HDF5-enabled build; selecting one in a `USE-HDF5=no` build is a fatal configuration error. `processing_order` selects the processing driver independently of the reader format, and startup validation rejects any combination whose reader and driver disagree. It defaults to `tree_ordered`; the four forest-ordered readers above feed that driver, while `snapshot_hdf5` feeds `snapshot_ordered`, whose driver (`run_snapshot_driver()`) opens and validates the dataset, processes every snapshot, and writes HDF5 output. To add a forest-ordered format of your own, see [Adding a Tree Reader](#adding-a-tree-reader) — it is a self-contained reader file plus one registry row, with no changes to the core read path; for the snapshot family, see [Snapshot-ordered readers](#snapshot-ordered-readers) and [The Snapshot Driver](#the-snapshot-driver).

`tree_name` is interpreted by the selected reader. `lhalo_binary` uses it as the prefix before the file number (`tree_name.<file_number>`). `consistent_trees_ascii` and `consistent_trees_hdf5` use it as a literal filename under `simulation_dir`, including any extension. `lhalo_hdf5` also uses explicit HDF5 filenames: for one file, set `tree_name` to that filename; for multiple files, include a `%d` file-number placeholder such as `trees_063.%d.hdf5`. `snapshot_hdf5` uses it as a declaration of the format's fixed filename convention and accepts exactly the literal `snapshot_%03d.h5`, rejecting every other value at startup.

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

  enum TreePartitionModel partition_model; /* per-file or reader-enumerated */
  enum InputProcessingOrder processing_order; /* currently INPUT_PROCESSING_ORDER_TREE */

  /* PARTITION_PER_FILE and PARTITION_ENUMERATED readers: */
  int (*num_partitions)(void);
  int (*partition_output_id)(int partition);
  int (*partition_exists)(int partition);
  void (*format_partition_path)(char *buf, size_t size, int output_id);
  int64_t (*count_partition_units)(int partition);
  int64_t (*global_forest_offset)(int partition);
  double (*partition_cost)(int partition);

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

- **`PARTITION_PER_FILE`**: the driver strides partitions across MPI tasks. Supply the enumeration via the shared helpers `tree_partition_per_file_count()` / `tree_partition_per_file_output_id()` (in `interface.c`) as your `num_partitions` / `partition_output_id`. Implement `count_partition_units(partition)` as an allocation-free header read so the driver can build the run-scoped global forest-offset table before processing. `open_partition(output_id)` opens file `output_id` and reads its tree table.
- **`PARTITION_ENUMERATED`**: the reader publishes a deterministic list of output partitions and costs. The driver assigns those partitions to MPI tasks, but output ids remain the reader's partition ids and therefore do not depend on `NTask`. The Consistent-Trees readers use this for chunked output: each partition is a forest-range chunk, `GlobalForestOffset` is the chunk's first global forest, and chunk ids drive output names and per-chunk resume.

### Where the partition model is observed outside the reader

Three pieces of the core key on `partition_model`; a new reader inherits them by setting the field correctly and supplying the matching callbacks:

- **Unique galaxy ids** — `make_unique_galaxy_id()` in `src/core/build_model.c` computes `forestnr_global = GlobalForestOffset + unit`, range-checks both components, and encodes through `mimic_encode_unique_galaxy_id()` (`src/include/galaxy_id.h:48`), which is `halonr + multiplier * (forestnr_global + 1LL)` for the configured `MimicConfig.UniqueGalaxyIDMultiplier`. **The `+ 1` is load-bearing** — it reserves the first multiplier block, so dropping it shifts every id by a whole block and breaks cross-driver identity. Encode through that helper rather than reimplementing the arithmetic. `PARTITION_PER_FILE` readers get `GlobalForestOffset` from the driver's prefix-sum scan over present files; `PARTITION_ENUMERATED` readers publish chunk offsets. Partition ids and MPI task ranks are not part of the identity.
- **Per-file offset scan** — `run_tree_driver()` calls `count_partition_units(partition)` for every present `PARTITION_PER_FILE` input file before processing so missing files keep the existing skip semantics and present files receive contiguous run-scoped offsets.
- **HDF5 master file** — `write_master_file()` asks readers for their partition ids, creates links to existing output files, and records per-snapshot totals from each partition file.

### Steps

1. Create `src/io/tree/read_<format>.c` (and a `.h` only if it exposes a shared seam). Implement the callbacks and define one `const struct TreeReader <Format>Reader = { ... }`. Bridge the format's halo records into the generated `struct RawHalo` by field name; let the generated reference-unit accessors apply unit conversion at the boundary (declare native units in the simulation package, not in reader code).
2. Append one row to `reader_table[]` in `src/io/tree/registry.c`. If the reader requires HDF5, guard both the `extern` declaration and the table entry with `#ifdef HDF5` so non-HDF5 builds simply do not register it (`tree_reader_lookup` then returns `NULL` and the run fails fast with a clear message).
3. If the reader pulls in `src/io/tree/<format>/*.c` support code that compiles in every build, keep it warning-clean under `-Wall -Wextra -Wshadow -Wformat-security -Wundef` and exercise it from `tests/unit/` so nothing is dead. The unit harness enables HDF5 reader sources when HDF5 development libraries are available, while HDF5 integration paths are still validated end-to-end against real or fixture datasets.
4. A new *format* needs no run-YAML changes beyond `tree_type` when it feeds the existing `tree_ordered` driver. Set `.processing_order = INPUT_PROCESSING_ORDER_TREE` in the reader initializer. A snapshot-ordered format belongs to the other reader family instead of being squeezed in here — see [Snapshot-ordered readers](#snapshot-ordered-readers).

The Consistent-Trees readers (`src/io/tree/read_ctrees_ascii.c`, `read_ctrees_hdf5.c`) are the worked reference for `PARTITION_ENUMERATED` chunked readers, including forest load-balancing and the `RawHalo` bridge.

### Snapshot-ordered readers

Snapshot-ordered input is a second reader family, not a variant of the tree readers. A tree reader hands the core one forest at a time; a snapshot reader hands it one snapshot's whole halo population — a *slab* — so global, snapshot-synchronous operations become expressible. The on-disk contract these readers consume is frozen in [dev/SNAPSHOT-HDF5-FORMAT.md](dev/SNAPSHOT-HDF5-FORMAT.md) (`format_version = 1`). One snapshot reader ships: `snapshot_hdf5` (`src/io/snapshot/read_snapshot_hdf5.c`), exercised by the `micro-uchuu-snapshot` simulation package.

The snapshot **driver** (`run_snapshot_driver()`, `src/core/snapshot_driver.c`) now exists, so every level of reader checking is on the run path:

- **Configuration validation — runs on every run.** Two-registry `tree_type` resolution, the reader/order compatibility check, the exact `tree_name` literal, the identity-multiplier rules, and the snapshot-ordered rejections (HDF5-only output, no `--skip`, `NTask == 1`; see [The Snapshot Driver](#the-snapshot-driver)), all in `src/core/read_parameter_file.c`.
- **Dataset validation (`open_run`) — the driver's first call.** `snapshot_reader_open_run()` is called from `run_snapshot_driver()` before any snapshot is loaded, so a missing, unreadable, or corrupt dataset aborts here with the file, object, and value that failed.
- **Link-range validation (`load_slab`)** — runs once per snapshot as the driver loads each slab.

The run path is `read_parameter_file()` → `init()` → `run_processing_driver()` (`src/core/main.c:372-373`, `:413`) → `run_snapshot_driver()` for a snapshot-ordered configuration (`src/core/tree_driver.c`, the `INPUT_PROCESSING_ORDER_SNAPSHOT` dispatch case). A snapshot-ordered run therefore opens and fully validates its dataset before processing anything, exactly like the tree-ordered path opens and reads its tree files.

#### The snapshot reader interface

`struct SnapshotReader` (`src/io/snapshot/reader.h`) is a separate, small vtable rather than a widening of `struct TreeReader`, whose twelve hooks are partition/unit-shaped and carry no meaning for snapshot input. `enum InputProcessingOrder` and `input_processing_order_name()` are shared with the tree side, because the processing order is a property of the run rather than of one reader family:

```c
struct SnapshotReader {
  const char *name;                        /* tree_type string in the input YAML */
  enum InputProcessingOrder processing_order;

  void (*open_run)(struct SnapshotRunInfo *info);  /* open + fully validate the dataset */
  void (*close_run)(void);                         /* release every run-scoped resource */
  int64_t (*snapshot_halo_count)(int64_t snapnum); /* count without loading */
  void (*load_slab)(int64_t snapnum, struct SnapshotSlab *slab);
  void (*release_slab)(struct SnapshotSlab *slab);
};
```

`struct SnapshotRunInfo` carries the run-scoped metadata `open_run` publishes: snapshot count, `format_version`, `n_forests_total`, and `max_halo_rank_in_forest`. Slab indices, counts, and offsets are `int64_t` throughout — production slabs reach hundreds of millions of halos, so the tree driver's `int` idiom does not carry over.

Readers register in `src/io/snapshot/registry.c`, a static table mirroring `src/io/tree/registry.c` with case-insensitive lookup through `snapshot_reader_lookup()`. Both the `extern` and the table row are `#ifdef HDF5`, so a non-HDF5 build registers nothing and the lookup returns `NULL` for every name. `snapshot_reader_count()` and `snapshot_reader_at()` enumerate the table, which is how tests assert that the tree and snapshot name sets stay disjoint. Dispatch goes through the thin wrappers in `src/io/snapshot/interface.c`, each of which checks at its point of use that the hook it needs is implemented and aborts naming that hook otherwise.

Two filename rules follow from the format being fixed rather than user-chosen. `input.tree_name` must be exactly the literal `snapshot_%03d.h5` (`SNAPSHOT_READER_TREE_NAME`), and the reader builds every path with a fixed internal format string and truncation checking — configured text is never passed to a `printf`-family format argument.

#### How `tree_type` resolves to a reader

`input.tree_type` is still the single reader selector, and there is still exactly one resolution site (`parse_input_section` in `src/core/read_parameter_file.c`). It consults two registries: `tree_reader_lookup()` first, then `snapshot_reader_lookup()`. The two name sets are disjoint, so the order fixes only which registry answers first, never which reader a name resolves to. After a successful resolution exactly one of `MimicConfig.reader` and `MimicConfig.snapshot_reader` is non-`NULL`, and `MimicConfig.TreeExtension` is set from `reader->file_extension` for a tree reader and left empty for a snapshot reader. An unrecognised `tree_type` fails with one message naming both registries.

Startup validation then reads the resolved reader's declared `processing_order` and rejects any mismatch with `input.processing_order`, whichever registry answered. Consequently `MimicConfig.reader` is legitimately `NULL` for a snapshot configuration, and every consumer must either guard for that or run only on the tree-ordered path. `validate_and_postprocess()` consumes it on the configuration path itself (`src/core/read_parameter_file.c:1401-1412`) and is correctly guarded — it tests both pointers for `NULL` and then branches on `is_tree_reader`. The tree-only consumers `src/io/tree/interface.c` (`:51`, `:88`, `:118`, `:132`) and `src/core/tree_driver.c:493` dereference `MimicConfig.reader` unguarded, which is safe because they run only inside `run_tree_driver()`. **No file under `src/io/output/` reads `MimicConfig.reader` at all any more**: both HDF5 output writers go through the driver-neutral `struct OutputPartitionSource get_output_partition_source(void)` (`src/io/output/util.h`, constructed in `src/core/tree_driver.c`), which the tree driver populates from `MimicConfig.reader`'s hooks and the snapshot driver from a trivial single-partition implementation — see [The Snapshot Driver](#the-snapshot-driver).

#### What `open_run` validates

`open_run` runs on every snapshot-ordered run, called from `run_snapshot_driver()` before any snapshot is processed. The reader validates structure before reading any data, so a non-conforming file is rejected rather than read into a buffer sized from different assumptions. In order, for every snapshot in the configured snapshot list:

1. **Structure** — exactly the `/header` and `/halos` groups; exactly the contract header attribute set, each scalar and of the contract dtype; exactly the contract `/halos` dataset set, each of the contract dtype, rank 1 for scalars and shape `[n_halos, 3]` for `Pos`, `Vel`, and `Spin`.
2. **Header values** — a supported `format_version`; `links_adjacent == 1`; `snapshot_number` equal to the filename index; `n_halos` in `[0, INT32_MAX]` and equal to the length of every `/halos` dataset in that file; `n_forests_total` and `max_halo_rank_in_forest` identical across all files.
3. **Agreement with the snapshot list and configuration** — each file's `scale_factor` equals its a_list entry **exactly**, with no tolerance, matching the producer's own comparison. The five physical header attributes — `box_size_mpc_h`, `particle_mass_msun_h`, `omega_matter`, `omega_lambda`, `hubble_h` — are compared against `MimicConfig`'s configured values for **every** snapshot file, not only the first, and the run aborts on mismatch naming the file, the attribute, and both values. The comparison is a rounding tolerance, not a scientific one: reject non-finite values, require exact equality when both are zero, otherwise accept iff `fabs(header - configured) <= 16 * DBL_EPSILON * fmax(fabs(header), fabs(configured))`. Particle mass is compared against `MimicConfig.PartMass * 1e10`, multiplying the configured value up to native units rather than dividing the header down, because a naive comparison in the wrong direction fails by exactly the 10¹⁰ unit factor.
4. **Identity bounds against measured data** — every `SnapNum` equals the file's `snapshot_number`; `max_halo_rank_in_forest` equals the measured maximum of `HaloRankInForest`; every `ForestIndex` lies in `[0, n_forests_total)`. These run as fixed-size hyperslab scans that accumulate a running maximum or range, never allocating a buffer proportional to `n_halos`. A dataset with no halos in any snapshot carries the sentinel `(n_forests_total, max_halo_rank_in_forest) == (0, -1)` and skips the measured-maximum equalities.
5. **Encodability** — `snapshot_identity_bounds_valid()` checks the published bounds against the configured identity multiplier before the run info is published.

Every failure aborts with the file path, the offending object, attribute, or field, and the value; nothing is repaired. Chain *construction* is the producer's obligation, discharged by the converter and its topology gate — the reader reads the links and validates their index ranges, and never reconstructs or reorders them.

#### Slab lifecycle

```text
open_run  ->  [ load_slab / release_slab ]*  ->  close_run
```

A slab handle has a defined empty state (`SNAPSHOT_SLAB_INIT`, tested with `snapshot_slab_is_empty()`); `snapnum` is the marker, not `nhalos`, because a snapshot containing zero halos is a legal load result. `load_slab` requires an empty destination and aborts otherwise, allocates `struct RawHalo[nhalos]` through `mymalloc_cat(..., MEM_TREES)`, and fills every field by including the generated `src/include/generated/read_tree_hdf5_properties.inc` under snapshot-flavoured macros — the same mechanism `src/io/tree/hdf5.c` uses, with no generator change. `release_slab` frees the array and returns the handle to its empty state; releasing an already-empty slab is a no-op. `close_run` aborts if any slab is still loaded.

At load time — once per snapshot as `run_snapshot_driver()` loads each slab — the reader validates link ranges: `FirstProgenitor` is `-1` or an index into snapshot `N-1`; `NextProgenitor` and `NextHaloInFOFgroup` are `-1` or indices into snapshot `N`; `FirstHaloInFOFgroup` is always a valid index into snapshot `N` and never `-1`; `Descendant` is `-1` or an index into snapshot `N+1`, and `-1` for every halo in the final snapshot. Diagnostics are bounded counted summaries — one line per snapshot and field, carrying the count and the first offending index and value — never one line per halo.

#### The identity multiplier

`UniqueGalaxyID` encodes `halonr + multiplier × (forestnr_global + 1)`, and the compile-time `TREE_MUL_FAC = 10⁹` cannot represent a super-forest whose within-forest halo ranks reach into the billions. `simulation.unique_galaxy_id_multiplier` makes the multiplier per-simulation metadata: it is legal in `simulation_info.yaml` and in the run file, defaults to `TREE_MUL_FAC`, must be positive, and is stored in `MimicConfig.UniqueGalaxyIDMultiplier`. Because `parse_simulation_section` runs once per file, the default is seeded once before either pass and the parser assigns only when the key is present — so a package value survives a run file that omits it, and an explicit run-file value wins.

One bound is enforced, at two points. The snapshot reader checks the configured multiplier against the dataset's own bounds at `open_run`, which now runs on every snapshot-ordered run (`snapshot_identity_bounds_valid()`: the multiplier must exceed every halo rank, and `multiplier × (n_forests_total + 1)` must fit in `int64_t` — the `+ 1` reserves the encoder's forest offset). Every helper in `src/include/galaxy_id.h` also takes the multiplier as an explicit `int64_t` parameter — the same bound expression, `mimic_unique_galaxy_id_max_forests()` — so **both** processing orders encode with the configured value and a tree-ordered configuration may set a non-default multiplier; what a run enforces at startup for the value itself is only that it is positive. The three Consistent-Trees forest-size guards (`read_ctrees_ascii.c`, `read_ctrees_hdf5.c`) check against the same configured value, so raising the multiplier genuinely raises the forest size those readers accept. HDF5 output records the value as an `int64` `RunProperties/UniqueGalaxyIDMultiplier` attribute, written to both per-file outputs and the master file, so any output file can be decoded back into its identity components.

#### Adding a snapshot reader

1. Implement one `const struct SnapshotReader` in `src/io/snapshot/read_<format>.c`. If it needs HDF5, the filename **must** end in `hdf5.c` — the Makefile drops that pattern from `USE-HDF5=no` builds.
2. Append one row to `snapshot_reader_table[]` in `src/io/snapshot/registry.c`, guarding both the `extern` and the row with `#ifdef HDF5` when the reader needs it. Never reuse a name registered in `src/io/tree/registry.c`; the two sets must stay disjoint. `registry.c` and `interface.c` must **not** be named `*hdf5.c`, because the configuration path calls `snapshot_reader_lookup()` in every build.
3. Validate the whole dataset at `open_run`, structure before values and values before bulk reads, and abort rather than repair.
4. Keep slab counts and indices `int64_t`, honour the empty-state lifecycle above, and allocate through `mymalloc_cat(..., MEM_TREES)`.
5. Ship a fixture simulation package with committed, small fixtures and C unit tests under `simulations/<name>/_tests/unit/`; `micro-uchuu-snapshot` is the worked reference.

### The Snapshot Driver

`run_snapshot_driver()` (`src/core/snapshot_driver.c`) is the second live driver behind `run_processing_driver()`'s dispatch (`src/core/tree_driver.c`), reached when `input.processing_order: snapshot_ordered` resolves against a snapshot reader. Where the tree driver walks one forest's full history depth-first with exactly one input generation live, the snapshot driver sweeps snapshots in increasing time order and holds exactly two: snapshot N is processed against the retained snapshot N−1's raw slab and processed state, which are released as soon as every FoF group at N has deep-copied what it inherits. Two raw slabs are unconditionally kept live — never one — because `FirstProgenitor` resolves into the previous slab while `NextProgenitor`/`Len` are also read from it.

Snapshot-ordered configurations are gated at config time (`validate_and_postprocess()`, `src/core/read_parameter_file.c`): `output_format: binary` is rejected (HDF5-only), `--skip` is rejected (no resume), and `NTask > 1` is rejected (serial only in this phase — see `docs/dev/MIMIC-DISTRIBUTED-SNAPSHOT-PLAN.md` for multi-rank execution).

**Explicit input view.** The generated `mimic_tree_get_*` accessors and the virial helpers (`get_virial_mass`/`get_virial_velocity`/`get_virial_radius`, `src/core/virial.c`) take a `struct HaloInputView { const struct RawHalo *halos; int64_t count; }` (`src/include/types.h`) as their first argument instead of reading a global array. The tree driver constructs its view from `InputTreeHalos` and the loaded unit's halo count; the snapshot driver constructs its view from whichever slab (N or N−1) a call site needs. This is what lets exactly the same physics-coupled code serve both drivers with no duplicated arithmetic: there is one shared generated payload populator (`populate_halo_payload.inc`), and `prepare_halo_for_output()` (`src/io/output/util.c`) takes the view too, so no file under `src/io/output/` reads a raw input global.

**Two instanced galaxy pools.** `struct GalaxyPool` (`src/core/galaxy_pool.h`) is created with `galaxy_pool_create()` and threaded explicitly through `inherit_descendant_halos()`; the snapshot driver holds two instances, one per live generation, ping-ponging a bulk reset once per snapshot, where the tree driver resets its single instance once per tree.

**Driver parity.** The physics engine, inheritance service, and output-buffer marshaller are shared unchanged; what is replicated in `snapshot_driver.c` is the tree-index-shaped bookkeeping the tree driver performs outside those seams — progenitor lookup, gather, FoF assembly, and module-context setup — as line-for-line equivalents of `find_most_massive_progenitor()`, `gather_progenitor_galaxies()`, `join_progenitor_halos()`, `process_halo_evolution()`, and `setup_module_context()` in `src/core/build_model.c`. At a summary level, the snapshot driver replicates: stamping `CentralMvir` from the FoF-central catalog mass onto every workspace member before physics; the new-object `SnapNum = current − 1` and `dT` sentinel; deriving `ctx->time_interval` and dynamic substep counts from the workspace's pre-marshal progenitor `SnapNum`; propagating `UniqueCentralGalaxyID` from the FoF Type 0 central to all members before physics; and stamping output `SnapNum` at marshal time. These parity behaviours are exactly what the cross-format identity gate below checks.

**Output-partition seam.** `write_master_file()` and the metadata writers no longer read `MimicConfig.reader` directly; they call `struct OutputPartitionSource get_output_partition_source(void)` (`src/io/output/util.h`), which the tree driver populates from its reader's partition hooks and the snapshot driver from a trivial single-partition implementation (one partition, output id 0). Snapshot-run provenance records the resolved reader-format name (`snapshot_hdf5`) through this seam rather than dereferencing a `NULL` tree reader.

**Snapshot output schema.** A snapshot-ordered run's per-snapshot HDF5 groups omit `Ntrees` and the `TreeHalosPerSnap` dataset entirely — absent, not zero or empty — because there is no per-tree structure to report; a consumer that needs one must fail loudly on the missing attribute rather than read a plausible lie. `TotHalosPerSnap` keeps its name across both drivers and is widened to `int64` (from `int`) in both the per-file writer (`src/io/output/hdf5.c`) and the master's read/republish path (`src/io/output/master_hdf5.c`), since that widening is shared writer code the tree path shares too. The master links a snapshot run's output as exactly one partition: one `Snap%03d` group per requested output snapshot, a single external link, no per-tree table. `hdf5_format_version` is `1.2` (from `1.1`), the increment the metadata writer's own rule requires for the `TotHalosPerSnap` schema change.

**The configured identity multiplier on both paths.** See [The identity multiplier](#the-identity-multiplier) above — the same `MimicConfig.UniqueGalaxyIDMultiplier` value, parsed once, is honoured by every `galaxy_id.h` helper regardless of driver, so a tree-ordered and a snapshot-ordered run over the same catalogue with the same multiplier encode identical `UniqueGalaxyID`s for identical `(halonr, forestnr_global)` pairs.

#### The cross-format identity gate

The gate proves the two drivers agree: for every output snapshot, the same set of `UniqueGalaxyID`s and per-ID bitwise-identical fields, aggregated across every output partition, with no tolerance of any kind. It compares `micro-uchuu-ascii` read tree-ordered against `micro-uchuu-snapshot` read snapshot-ordered, under **both** models (`halos-only`, then `sage16`) and **both** timestep schemes (fixed, then dynamic).

- **Comparator** — `scripts/compare_cross_format_identity.py`: one implementation of the frozen algorithm (duplicate-ID assertion, then ID-set equality, then per-field raw-byte comparison).
- **Harness/test** — `simulations/micro-uchuu-snapshot/_tests/scientific/test_cross_format_identity.py`: package-local to `micro-uchuu-snapshot`, so it is registered by the scientific-tier test registry only when that package is selected. It builds each of the four `{halos-only, sage16} × {ascii, snapshot}` pairs in its own git worktree (never touching the ambient tier build), runs both timestep schemes, and also re-proves the tree-ordered path byte-identical against the pre-Phase-5 baseline.
- **How to run it** — `make MODEL=halos-only SIMULATION=micro-uchuu-snapshot tests-scientific`, on a machine holding both the `micro-uchuu-ascii` and `micro-uchuu-snapshot` datasets. This is a **manual, dataset-present operation**: it is not part of the default-pair suite or CI, takes on the order of hours (four builds, nine full runs), and fails loudly rather than skipping when a dataset is absent.

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
| `num_substeps` | Active substep count for this timestep; fixed from `SubSteps` or scheme-derived |
| `time_interval` | Full snapshot interval |
| `substep_dt` | Shared substep duration; model packages may provide per-object helpers for integration |
| `central_index` | Index of Type 0 central in the FoF workspace |
| `central_galaxy` | Pointer to Type 0 central |
| `active_event` | Event payload for `process_per_event`; otherwise `NULL` |
| `params` | Read-only pointer to `MimicConfig` |

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
- [SNAPSHOT-HDF5-FORMAT.md](dev/SNAPSHOT-HDF5-FORMAT.md): frozen on-disk contract for snapshot-ordered HDF5 merger-tree input
- [plot/mimic-plot/README.md](../plot/mimic-plot/README.md): detailed plotting manual
- [tests/README.md](../tests/README.md): test-suite quick reference
- `models/<model>/README.md`: model-package science scope, module pipeline, parameters, plots, and references
- `simulations/<simulation>/README.md`: simulation-package data, units, snapshot lists, and maintenance notes
