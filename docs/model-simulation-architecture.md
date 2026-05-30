# Model, Simulation, and Plotting Architecture

## Purpose

This note captures the proposed end-state structure for separating Mimic's physics-agnostic core from model-specific physics, simulation-specific metadata, and model/simulation-specific plotting.

It is intended as input for an implementation plan. It focuses on the desired architecture, ownership boundaries, current-to-future mapping, and migration shape rather than line-by-line implementation details.

## Guiding Principle

Mimic should be organised around four distinct responsibilities:

1. **Core framework**: execution, tree traversal, memory, I/O, validation, generated metadata machinery, and module dispatch.
2. **Model packages**: scientific physics modules, model properties, model parameters, model-local shared helpers, model-specific figures, and validation expectations.
3. **Simulation packages**: simulation/catalog metadata, snapshot lists, units, particle masses, box sizes, input manifests, and simulation-specific halo catalog fields.
4. **Plotting framework**: reusable plotting infrastructure that discovers and executes model-provided figures using model/simulation plot profiles.

The core should provide mechanisms. Models should provide physics. Simulations should provide catalog context. Plot profiles should bind a model and simulation into a reproducible analysis or validation view.

## Proposed End State

```text
mimic/
├── src/
│   ├── core/
│   │   ├── core_properties.yaml
│   │   ├── main.c
│   │   ├── init.c
│   │   ├── build_model.c
│   │   └── ...
│   │
│   ├── module_system/
│   │   ├── module_interface.h
│   │   ├── module_registry.c
│   │   ├── parameter_helpers.h
│   │   ├── output_helpers.h
│   │   ├── physical_constants.h
│   │   ├── generated/
│   │   └── template/
│   │
│   ├── io/
│   ├── util/
│   └── include/
│
├── models/
│   ├── shared/
│   │   ├── metallicity.h
│   │   ├── cooling_tables/
│   │   └── ...
│   │
│   └── sage/
│       ├── model_properties.yaml
│       ├── modules/
│       │   ├── sage_reionization/
│       │   ├── sage_apply_cooling/
│       │   ├── sage_radio_mode_heating/
│       │   └── ...
│       │
│       ├── shared/
│       │   ├── sage_agn_physics.h
│       │   ├── sage_starburst_physics.h
│       │   └── ...
│       │
│       ├── configs/
│       │   └── sage_defaults.yaml
│       │
│       ├── plots/
│       │   ├── figures/
│       │   │   ├── stellar_mass_function.py
│       │   │   ├── gas_fraction.py
│       │   │   ├── baryon_fraction.py
│       │   │   └── ...
│       │   │
│       │   └── profiles/
│       │       ├── default.yaml
│       │       ├── millennium.yaml
│       │       └── mini_millennium.yaml
│       │
│       ├── validation/
│       │   ├── observational_data/
│       │   ├── reference_outputs/
│       │   └── tolerances.yaml
│       │
│       └── README.md
│
├── simulations/
│   ├── shared/
│   │   ├── units/
│   │   └── snapshot_tools/
│   │
│   └── millennium/
│       ├── simulation.yaml
│       ├── halo_properties.yaml
│       ├── snapshots/
│       │   └── millennium.a_list
│       ├── input_data_manifest.yaml
│       ├── plot_profile.yaml
│       └── README.md
│
├── output/
│   └── mimic-plot/
│       ├── mimic-plot.py
│       ├── plot_loader.py
│       ├── profile_loader.py
│       ├── hdf5_reader.py
│       ├── output_utils.py
│       ├── generated/
│       └── tests/
│
├── input/
│   └── runs/
│       ├── sage_millennium.yaml
│       └── halo_tracking_only_millennium.yaml
│
└── docs/
```

## Directory Roles

### `src/`

`src/` is the Mimic framework. Code here should remain valid if no production physics model is loaded. It owns:

- executable startup and shutdown
- tree loading interfaces
- FoF workspace construction
- module dispatch and processing phases
- memory management
- output writing
- generated-code machinery
- validation machinery
- minimum core properties needed for Mimic to process trees

The current `src/modules/_system/` contents should eventually become `src/module_system/` or equivalent. These are generic framework components, not SAGE physics.

### `models/`

`models/` contains versioned scientific model packages.

Each model package owns the things that are scientifically coupled and should be reviewed together:

- runtime physics modules
- model property metadata
- model-specific shared helpers
- model parameter defaults
- model-local documentation
- model-specific figures
- model-specific plotting profiles
- validation data, reference outputs, and tolerances

`models/shared/` is for genuinely reusable physics utilities that are not owned by one model. For example, a metallicity helper or cooling table implementation could live here if it is intended to be reused across SAGE and another model.

`models/sage/shared/` remains appropriate for helpers that encode SAGE-specific assumptions or APIs.

### `simulations/`

`simulations/` contains simulation/catalog-specific material that is not core code and not owned by a physics model.

A simulation package owns:

- snapshot lists
- simulation units
- cosmology defaults
- box size and particle mass defaults
- input data manifests
- tree/catalog field metadata
- simulation-specific halo property metadata
- simulation-specific plotting preferences

For example, Millennium-specific settings should live under `simulations/millennium/`, not in core and not inside `models/sage/`.

### `output/mimic-plot/`

`mimic-plot` should become plotting infrastructure rather than a container for SAGE/Millennium science plots.

It should own:

- output readers
- profile loading
- figure discovery
- common plotting utilities
- command-line execution
- generated dtype helpers
- plotting framework tests

It should not own model-specific figure implementations. Those should move to the relevant model package, such as `models/sage/plots/figures/`.

## Property Ownership

### Core Properties

The current `src/core/halo_properties.yaml` name is too specific. It can be confused with simulation-specific halo catalog metadata. The proposed name is:

```text
src/core/core_properties.yaml
```

This file should contain only the minimum properties required by Mimic's core processing model.

Likely minimum core set:

```text
SnapNum
Type
CentralHalo
HaloNr
UniqueGalaxyID
UniqueCentralGalaxyID
dT
Len
Mvir
Rvir
Vvir
deltaMvir
```

These fields support identity, dispatch, inheritance, timestep calculation, workspace construction, and basic halo evolution. They are not SAGE properties.

### Simulation Halo Properties

Simulation/catalog properties should move out of the core property file once the property generator supports multiple property roots.

Example location:

```text
simulations/millennium/halo_properties.yaml
```

Examples:

```text
MostBoundID
Pos
Vel
Spin
Vmax
VelDisp
raw catalog mass variants
catalog-specific IDs
catalog-specific metadata fields
```

These properties are important, but they are not necessarily required by the core framework for every simulation or tree format.

### Model Properties

Model properties remain model-owned and should move with the model package.

Example location:

```text
models/sage/model_properties.yaml
```

Examples:

```text
ColdGas
HotGas
EjectedGas
StellarMass
BulgeMass
BlackHoleMass
CoolingGas
InfallingGas
MergTime
TimeOfLastMajorMerger
SupernovaReheatedMass
```

Transport properties used for communication between SAGE modules should live in the SAGE model package because they are part of that model's internal contract.

### Transitional Properties

Some properties are awkward during migration because the current core maintains them directly while they also carry model-like meaning.

Examples:

```text
infallMvir
infallVvir
infallVmax
```

Today, these are effectively core because `build_model.c` updates them. In the end state, they should be resolved deliberately as either:

- core satellite/orphan tracking state, if Mimic requires them independent of model choice, or
- model-requested halo extension properties, if they are only needed by a model such as SAGE.

The implementation plan should call these out explicitly rather than moving them mechanically.

## Run Configuration

A run file should compose a core, a model package, and a simulation package.

Example:

```yaml
model:
  name: sage
  path: models/sage
  properties: models/sage/model_properties.yaml

simulation:
  name: millennium
  path: simulations/millennium
  config: simulations/millennium/simulation.yaml
  halo_properties: simulations/millennium/halo_properties.yaml

plotting:
  profile: models/sage/plots/profiles/millennium.yaml

output:
  output_filename: model
  output_directory: ./output/results/millennium/
  output_format: hdf5

modules:
  pre_timestep:
    - sage_reionization: process_full_halo
    - sage_prepare_infall_budget: process_full_halo

  phase_1:
    - sage_apply_infall: process_full_halo
    - sage_apply_cooling: process_by_galaxy

  phase_2:
    - sage_resolve_mergers_and_disruption: process_full_halo
    - sage_quasar_mode: process_per_event

  post_timestep:

  parameters:
    GlobalBaryonFraction: 0.17
    SfrEfficiency: 0.05
```

This makes the scientific composition explicit: run SAGE on Millennium with the SAGE+Millennium plotting profile.

## How the End State Works

In the end state, a typical run follows this path:

1. The user selects a run YAML file, such as `input/runs/sage_millennium.yaml`.
2. The run YAML identifies the model package, simulation package, output settings, module pipeline, model parameters, and plotting profile.
3. The build/generation system combines property metadata from:
   - `src/core/core_properties.yaml`
   - `models/sage/model_properties.yaml`
   - `simulations/millennium/halo_properties.yaml`, if present
4. The module generator discovers available modules from the configured model root, such as `models/sage/modules/`, plus any compatibility roots during migration.
5. `make validate-modules` validates module metadata against the merged property set, declared parameters, event contracts, files, processing modes, and docs.
6. At runtime, Mimic loads the simulation metadata, reads the requested tree files, builds FoF workspaces using the core processing model, and dispatches the configured model modules.
7. Output files record enough provenance to recover the active model, simulation, modules, parameters, properties, redshift mapping, units, and version information.
8. `mimic-plot` reads the output, loads the configured plotting profile, discovers figure modules from the model package, applies profile and run overrides, and writes the requested figures.

This preserves the current generated-code discipline while making the ownership of each input explicit.

### Metadata Composition

The generated property schema should be assembled from multiple roots but still produce one coherent C/Python output schema for a given build or run.

Conceptually:

```text
core properties
  + model properties
  + simulation/catalog properties
  = generated structs, output schema, HDF5 metadata, Python dtype helpers
```

The implementation should continue to fail fast if two property packages define the same field incompatibly, if a module depends on an unknown property, or if a plot expects a field that is not present in the active output schema.

### Module Discovery

Module discovery should be driven by configured roots rather than hard-coded `src/modules/` assumptions.

Example discovery roots during migration:

```text
models/sage/modules/
src/modules/                 # compatibility root only
src/module_system/test_*/     # framework test fixtures, if still needed
```

Long term, production physics should live in `models/<model>/modules/`. Framework fixtures should live under framework-owned test locations, not beside production models.

## Plotting Architecture

The available figures are often tied to a model plus simulation combination. For example, SAGE-on-Millennium validation figures are not necessarily meaningful for a different model, a different simulation volume, or a halo-only run.

The proposed rule is:

- `mimic-plot` provides the plotting engine.
- `models/<model>/plots/figures/` provides model-specific figure code.
- `models/<model>/plots/profiles/` provides model and model+simulation plotting profiles.
- `simulations/<simulation>/plot_profile.yaml` provides simulation-specific plotting defaults.
- run YAML can override profile choices for a specific run.

### Profile Precedence

When plotting, configuration should be layered from general to specific:

```text
output/mimic-plot/profiles/default.yaml
models/<model>/plots/profiles/default.yaml
simulations/<simulation>/plot_profile.yaml
models/<model>/plots/profiles/<simulation>.yaml
run YAML plotting overrides
```

The most specific layer wins.

### Axis Ranges

Hard-coded Millennium ranges should become profile settings or auto-derived ranges.

Recommended behavior:

- use simulation metadata for spatial plots, such as box size
- use particle mass and resolution metadata for lower halo-mass limits
- use output volume for number-density plots
- use available redshift metadata for evolution plots
- use robust data quantiles for scatter and histogram axes
- allow validation profiles to pin axes for comparison with reference figures

This gives two useful modes:

1. **Exploration mode**: derive sensible ranges from the output data and simulation metadata.
2. **Validation mode**: use fixed profile ranges so figures are directly comparable across runs.

## Current State to End State Mapping

| Current path | Proposed path | Notes |
| --- | --- | --- |
| `src/core/halo_properties.yaml` | `src/core/core_properties.yaml` plus simulation/model property roots | Rename and reduce to core-required properties over time. |
| `src/modules/model_properties.yaml` | `models/sage/model_properties.yaml` | SAGE galaxy/model state belongs with the SAGE model package. |
| `src/modules/sage_*` | `models/sage/modules/sage_*` | Production SAGE physics modules move out of core source. |
| `src/modules/_shared/` | `models/shared/` and `models/sage/shared/` | Split genuinely reusable physics from SAGE-specific helpers. |
| `src/modules/_system/` | `src/module_system/` | Framework module infrastructure, not physics. |
| `src/modules/_system/template/` | `src/module_system/template/` | Generic module template. |
| `src/modules/_system/generated/` | `src/module_system/generated/` | Generated module registry code. |
| `input/millennium.yaml` | `input/runs/sage_millennium.yaml` plus `simulations/millennium/simulation.yaml` | Run file composes model and simulation packages. |
| `input/data/millennium/millennium.a_list` | `simulations/millennium/snapshots/millennium.a_list` | Snapshot list is simulation-specific metadata. |
| `output/mimic-plot/figures/*.py` | `models/sage/plots/figures/*.py` | Existing figures are largely SAGE/SAGE+Millennium science figures. |
| hard-coded plot ranges | `models/sage/plots/profiles/*.yaml` and `simulations/*/plot_profile.yaml` | Fixed validation ranges become profile data. |
| `output/mimic-plot/hdf5_reader.py` | unchanged | Generic plotting infrastructure remains in `mimic-plot`. |
| `output/mimic-plot/output_utils.py` | unchanged, with cleanup as needed | Generic plotting utilities remain in `mimic-plot`. |

## Implementation Shape

The migration should be incremental. The first priority is to make the build and generation systems accept configurable roots while keeping current paths working.

Recommended phases:

1. **Introduce roots without moving science code**
   - Add configurable model root and simulation root.
   - Teach generators to read core, model, and simulation property files.
   - Teach module discovery to accept one or more module roots.
   - Keep `src/modules/` working as a compatibility root.

2. **Rename core property metadata**
   - Rename `src/core/halo_properties.yaml` to `src/core/core_properties.yaml`.
   - Update generator inputs, Makefile dependencies, docs, and tests.
   - Do not reduce the property set in the same step unless the generator can already merge property packages safely.

3. **Move SAGE model metadata and modules**
   - Move `src/modules/model_properties.yaml` to `models/sage/model_properties.yaml`.
   - Move SAGE production modules to `models/sage/modules/`.
   - Move SAGE-specific helpers to `models/sage/shared/`.
   - Move genuinely reusable helpers to `models/shared/`.

4. **Extract simulation metadata**
   - Create `simulations/millennium/`.
   - Move snapshot lists, simulation defaults, and simulation metadata there.
   - Split simulation halo properties out of the core property file once property-package merging is ready.

5. **Refactor plotting**
   - Add figure discovery to `mimic-plot`.
   - Add profile loading and precedence.
   - Move SAGE figure Python files to `models/sage/plots/figures/`.
   - Move fixed Millennium plotting ranges into profiles.
   - Keep generic readers and utilities in `output/mimic-plot/`.

6. **Tighten validation**
   - Update `make validate-modules` to understand model roots.
   - Update `make check-generated` to validate merged property metadata.
   - Add tests proving an empty module pipeline still works.
   - Add tests proving model-specific figures are discovered through profiles.

## Open Design Questions

The implementation plan should resolve these explicitly:

1. Should a run choose exactly one model package, or should multiple model packages be composable in one run?
2. Should simulation halo properties be selected by simulation package, tree type, or both?
3. Should model packages be compiled into the executable at build time, or should they eventually become dynamically loadable?
4. Which currently core-maintained satellite/orphan fields are genuinely core, and which should become model-requested halo extensions?
5. Should plotting profiles live only under model/simulation packages, or should users be able to provide arbitrary external profile paths?

## Summary

The proposed architecture makes Mimic's existing vision visible in the directory layout:

- `src/` is the physics-agnostic framework.
- `models/` contains scientific model packages.
- `simulations/` contains catalog and simulation context.
- `mimic-plot` is the plotting engine, not the owner of SAGE/Millennium figures.
- run files compose a model, a simulation, a module pipeline, parameters, output settings, and plotting profile choices.

This separation should make it easier to add new physics models, run the same model against different simulations, validate model/simulation combinations, and avoid accidentally treating SAGE or Millennium assumptions as core Mimic behavior.
