# Mimic Developer Guide

**Practical guide to extending Mimic with physics modules, properties, tests, and generated metadata.**

This guide is for contributors and researchers modifying Mimic internals: writing a new physics module, adding properties, wiring up a new simulation, or working on the framework itself. It assumes you have already run Mimic successfully — if not, start with the [User Guide](USER-GUIDE.md). For the architectural principles and design rationale behind the structures described here, see [VISION.md](VISION.md). The shipped model packages are worked examples of everything in this guide: [models/sage16/](../models/sage16/README.md) is a mature production package, and [models/sham/](../models/sham/README.md) is the minimal pattern.

---

## Table of Contents

1. [Quick Start](#quick-start)
2. [Architecture Overview](#architecture-overview)
3. [Creating Physics Modules](#creating-physics-modules)
4. [Processing Modes and Phases](#processing-modes-and-phases)
5. [Parameters](#parameters)
6. [Property System](#property-system)
7. [Adding a New Simulation](#adding-a-new-simulation)
8. [Events](#events)
9. [Testing](#testing)
10. [Development Workflow](#development-workflow)
11. [Debugging](#debugging)
12. [Reference](#reference)

Common tasks:

- Adding a module: [Creating Physics Modules](#creating-physics-modules)
- Choosing a processing mode: [Processing Modes and Phases](#processing-modes-and-phases)
- Adding a property: [Property System](#property-system)
- Loading parameters: [Parameters](#parameters)
- Adding a simulation: [Adding a New Simulation](#adding-a-new-simulation)
- Using events: [Events](#events)
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

Mimic separates core infrastructure from physics modules.

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
| `InputTreeHalos` / `struct RawHalo` | Immutable input merger tree data |
| `FoFWorkspace` / `struct Halo` | Temporary processing workspace modified by modules |
| `ProcessedHalos` / `struct Halo` | Tree-driver output buffer and processed progenitor state |
| `OutputBufferSegment` | Driver-supplied range/snapshot metadata for shared output marshalling |
| `struct GalaxyData` | Generated galaxy/model property storage attached to `struct Halo` |
| `struct HaloOutput` | Generated output record written to binary/HDF5 |

Galaxy inheritance copies previous processed galaxy state into the current workspace, resets snapshot-scoped properties marked `init_repeat: true`, and updates halo properties from driver-supplied descendant data. After physics execution, the shared output-buffer marshaller copies surviving workspace entries into the driver-owned output buffer and frees Type 3 entries.

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

Standalone modules are package-local. The old `src/modules/` root is not searched.

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
| `pre_timestep` | Once before substeps | Setup, reionization, infall budgets, disk radii, merger clock setup |
| `modules.phases.<name>` | Each substep, in YAML order | Named physical stages such as `galaxy_physics` or `satellite_mergers` |
| `post_timestep` | Once after substeps | Finalization and accumulator conversion |

Only `pre_timestep`, `phases`, `post_timestep`, and `parameters` are valid under `modules`. Legacy top-level `phase_1`, `phase_2`, and `enabled` keys are rejected at startup.

### Accessing the Central Galaxy

`ctx->central_galaxy` points to the Type 0 central for the current FoF workspace and is available during module execution.

```c
struct Halo *central = ctx->central_galaxy;
double central_vvir = central->Vvir;
double central_hot_gas = central->galaxy->HotGas;
```

Use this when a satellite calculation depends on the central potential or when a module moves material to the central reservoir. Do not assume every entry in the `halos` array is valid for processing; check `halos[i].galaxy != NULL` and any relevant `Type` constraints.

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

Properties are generated from YAML metadata and then accessed as normal C struct fields.

| Property type | Metadata file | Typical owner |
| --- | --- | --- |
| Halo properties | `src/core/core_properties.yaml` | Core tree tracking and output |
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

Use `role: transport` for inter-module scratch fields that are not intended as persistent output. Document producer and consumer modules in the metadata comment.

```yaml
- name: CoolingGas
  type: float
  units: "1e10 Msun/h"
  description: "Gas mass cooling from hot to cold this substep"
  role: transport
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

Property metadata is the source of truth for output fields and unit labels. Do not maintain manual exhaustive property tables in prose documentation unless they are generated or deliberately illustrative.

### HDF5 Output Writer

`src/io/output/hdf5.c` writes each output snapshot as a single compound `Galaxies` table (one row per `struct HaloOutput`), matching the binary record layout so both formats share `prepare_halo_for_output()`.

- **Buffered writes.** Prepared records accumulate in a fixed-size per-snapshot buffer (`HDF5_WRITE_BUFFER_RECORDS`) that flushes when full and once more at end of file (`flush_hdf5_buffers`). This decouples write granularity from tree boundaries and from file size, so memory stays bounded at large scale and the number of `H5TBappend_records` calls drops from O(trees × snapshots) to O(records / buffer). It is what makes HDF5 output as cheap as binary; do not reintroduce per-tree appends.
- **FieldMetadata** (field names, units, descriptions) is identical for every snapshot, so it is written once per file under `RunProperties/FieldMetadata`, not duplicated per snapshot group. Its creation is generated by `scripts/generate_properties.py`; edit the generator, never the generated include.
- **Compression** is off by default and enabled per run with `--compress`, which sets `MimicConfig.HDF5CompressionLevel` and turns on gzip for the `Galaxies` table. HDF5's table API applies a fixed deflate level, so the flag is on/off only. Compression changes on-disk bytes, not stored values.

---

## Adding a New Simulation

A simulation package lives under `simulations/<name>/` and provides the merger tree catalog, cosmology, units, snapshot list, and any catalog-specific halo properties for a particular N-body simulation run. The shipped `simulations/mini-millennium/` package is the reference example.

### Directory Structure

```text
simulations/my_sim/
  simulation_info.yaml      required — catalog paths, cosmology, units, box size
  my_sim.a_list             required — one scale factor per line per snapshot
  halo_properties.yaml      required — catalog halo fields beyond the core set
  snapshots/                required — tree data directory or symlink to local data
  plot_profile.yaml         optional — simulation-specific plotting defaults
  input_data_manifest.yaml  optional — lists required data files for setup scripts
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
  simulation_dir: ./simulations/my_sim/snapshots/
  snapshot_list_file: simulations/my_sim/my_sim.a_list

simulation:
  cosmology:
    omega_matter: 0.25
    omega_lambda: 0.75
    hubble_h: 0.73
  box_size: 62.5            # comoving side length in code length units (Mpc/h)
  particle_mass: 0.0860657  # dark matter particle mass in code mass units (1e10 Msun/h)
  units:
    length_in_cm: 3.08568e+24    # 1 code length unit expressed in cm (1 Mpc/h here)
    mass_in_g:    1.989e+43      # 1 code mass unit expressed in g (1e10 Msun/h here)
    velocity_in_cm_per_s: 100000.0  # 1 code velocity unit in cm/s (1 km/s here)
```

The `simulation.units` block is not labeling — `init.c` derives all runtime unit conversions (time, density, pressure, energy, G) from these three values. Getting them wrong produces physically incorrect output with no error at runtime. The mini-Millennium example uses the standard `Mpc/h`, `1e10 Msun/h`, `km/s` convention.

**Supported tree formats:**

| `tree_type` value | Format |
| --- | --- |
| `lhalo_binary` | Standard LHaloTree binary format (Springel et al.) |
| `genesis_lhalo_hdf5` | Genesis L-Galaxies HDF5 format |

To add support for a different catalog format, implement `load_tree_table_*()` and `load_tree_*()` in `src/io/tree/`, register the new format in the `Valid_TreeTypes` enum in `src/include/types.h`, and add dispatch cases to `src/io/tree/interface.c`.

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

This file declares halo properties that come from the catalog and are specific to this simulation: positions, velocities, spin parameters, particle IDs, and similar catalog fields. It does not duplicate properties already in `src/core/core_properties.yaml`.

The generator includes exactly one simulation package at a time, selected with `SIMULATION=<name>`. Adding a new simulation package is not enough by itself; regenerate and rebuild with that selector so the executable, `struct Halo`, output schema, validation ranges, and module dependency checks all use the intended catalog properties.

- Property names must be unique within the selected `src/core/core_properties.yaml` + `simulations/<SIMULATION>/halo_properties.yaml` + `models/<MODEL>/model_properties.yaml` set. Incompatible duplicate names fail at generation time.
- After adding or editing the default simulation package, run `make generate` followed by `make`. For another simulation, run `make SIMULATION=<name> generate` followed by `make SIMULATION=<name>`; add `MODEL=<name>` too when pairing it with a non-default model.

Property metadata schema is the same as for core halo properties:

```yaml
halo_properties:
  - name: MostBoundID
    type: long long
    units: dimensionless
    description: ID of most bound particle
    output: true
    init_source: copy_from_tree
    output_source: copy_direct

  - name: Pos
    type: vec3_float
    units: Mpc/h
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
simulation:
  name: my_sim
  path: simulations/my_sim
  config: simulations/my_sim/simulation_info.yaml
  halo_properties: simulations/my_sim/halo_properties.yaml
```

To override simulation defaults for a specific run without changing the shared config:

```yaml
input:
  first_file: 0
  last_file: 0  # process only the first file
```

Any `input:` key in the run file takes precedence over the same key in `simulation_info.yaml`.

### Optional: plot_profile.yaml

If users will generate plots with `mimic-plot.py`, provide a `plot_profile.yaml` with simulation-specific axis limits and display parameters. Reference it from the run file:

```yaml
plotting:
  profile: simulations/my_sim/my_sim_plot_profile.yaml
```

The run file `plotting.profile` must be present for `mimic-plot.py` to locate it; the binary itself ignores the plotting section. Profile `inherits` entries are resolved relative to the profile file that declares them, so package-local profiles should inherit neighbouring defaults with local paths such as `default.yaml`. See `simulations/mini-millennium/plot_profile.yaml` for the format.

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

## Events

Events connect a full-halo producer to one or more `process_per_event` consumers in the same phase. Use events when a producer detects discrete events, such as mergers, and downstream modules need to respond immediately to the event target.

Producer metadata:

```yaml
events:
  emits:
    - name: merger
      description: "value0=mass_ratio, value1=source_dt"
```

Consumer metadata:

```yaml
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
- Consumers must declare `events.consumes`.
- The producer must be configured in the same phase as the consumer.
- Events are dispatched immediately when emitted.
- Consumer YAML order controls the order of consumers subscribed to the same event.
- HDF5 output records resolved event contracts under `RunProperties/EventContracts`.

---

## Testing

Mimic uses three test tiers. Every tier runs the core tests, selected-simulation tests under `simulations/<SIMULATION>/_tests/`, and tests declared by the selected model package. Empty generated lists are valid; if a simulation or model has no tests in a tier, that tier still runs the core tests and exits successfully.

| Tier | Command | Scope |
| --- | --- | --- |
| Unit | `make tests-unit` | C unit tests for core functions, selected-simulation fixtures, selected-model modules, and infrastructure |
| Integration | `make tests-integration` | End-to-end Python tests for core workflows, selected-simulation fixtures, and selected-model modules |
| Scientific | `make tests-scientific` | Core scientific contracts plus selected-simulation and selected-model scientific regressions |

Run everything:

```bash
make tests
```

For long-running test sessions, capture logs and check the exit code:

```bash
mkdir -p archive/test-logs
make tests > archive/test-logs/tests.log 2>&1
test_rc=$?
tail -n 80 archive/test-logs/tests.log
rg -n -i "failed|error|traceback" archive/test-logs/tests.log
echo "exit_code=${test_rc}"
```

A non-zero exit code is a failure even if the log text looks harmless.

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

To change the project default (e.g. when promoting a new model or simulation package), update `DEFAULT_MODEL` and/or `DEFAULT_SIMULATION` in the Makefile. `scripts/lib/defaults.sh` reads these values at runtime, so `scripts/benchmark_mimic.sh`, `scripts/regenerate_baseline.sh`, and `plot/mimic-plot/tests/test_plotting.sh` all pick up the new defaults automatically. Also update the `model.name`, `model.path`, `model.model_properties`, and `plotting.profile` fields in the model's input YAML files to match.

Generated files include:

| Generator | Inputs | Outputs |
| --- | --- | --- |
| `scripts/generate_properties.py` | halo and galaxy property metadata | C property structs/includes, HDF5 field metadata, output schema writer, validation ranges |
| `scripts/generate_module_registry.py` | module metadata | runtime module registration, event contracts, module source fragments |

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

Required fields:

| Field | Description |
| --- | --- |
| `name` | Generated C field name |
| `type` | C/Python type such as `float`, `double`, `int`, `long`, vector types |
| `units` | Output unit label |
| `description` | Human-readable meaning |
| `output` | Whether the property is written to output |
| `init_source` | Initialization method |
| `output_source` | Output method when `output: true` |

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
| `init_value` | Default value or tree field, depending on `init_source` |
| `init_repeat` | Reset after inheritance each snapshot |
| `output_convert` | Unit conversion expression |
| `output_transform` | Output transform such as `log10` |
| `output_function` | Helper function for recalculated output |
| `output_function_arg` | Arguments passed to helper function |
| `range` | Validation range used by tests |
| `sentinels` | Values exempt from range checks |
| `role` | Semantic role such as `transport` |

### ModuleContext Fields

`struct ModuleContext` is defined in `src/core/module_interface.h`. Treat all fields as read-only.

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

### Memory Categories

Definitions live in `src/util/memory.h`.

| Category | Use |
| --- | --- |
| `MEM_GALAXIES` | Galaxy data |
| `MEM_HALOS` | Halo/workspace data |
| `MEM_TREES` | Tree input data |
| `MEM_IO` | I/O buffers |
| `MEM_UTILITY` | Utility and module-owned allocations |

### Logging Macros

Definitions live in `src/util/error.h`.

| Macro | Visible when | Use for |
| --- | --- | --- |
| `DEBUG_LOG` | `--debug` | Detailed diagnostics |
| `VERBOSE_LOG` | `--verbose` or `--debug` | Configuration/lifecycle detail |
| `INFO_LOG` | default | Normal progress |
| `WARNING_LOG` | always | Non-fatal issues |
| `ERROR_LOG` | always | Errors before returning failure |
| `FATAL_ERROR` | always | Fatal errors that exit |

### Physical Constants

Do not duplicate the physical constants table in documentation. The source of truth is `src/module_system/physical_constants.h`. Runtime-derived unit quantities are computed in `src/core/init.c` from `simulation.units` in the input YAML.
