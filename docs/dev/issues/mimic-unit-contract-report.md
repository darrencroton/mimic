# Mimic Unit Contract Report

Date: 2026-06-13

## Executive Summary

Mimic needs an explicit unit contract before more model and simulation packages are added. The desired end state is:

- The selected simulation defines Mimic's internal runtime units through `simulation.units`.
- Tree/catalog values are read and processed in those simulation-defined internal units.
- The Mimic core defines canonical required halo/tree properties, while each selected simulation maps its catalog field names onto those canonical core properties in `simulations/<simulation>/halo_properties.yaml`.
- Core physical calculations, including time, `G`, critical density, `Mvir`, `Rvir`, and `Vvir`, operate in those internal units.
- Hubble scaling (`h`) is treated as an explicit cosmological scale factor, not as a base physical unit, even when unit labels use astrophysical shorthand such as `Msun/h` or `Mpc/h`.
- Model property metadata declares the model-facing/output physical units for model-owned properties, defaults, ranges, and output labels.
- Generated code converts model property values from their declared model/output units into internal units for processing, then converts them back for output.
- Model physics code is audited and migrated so formula-level unit assumptions match the new internal unit contract rather than relying on hard-coded SAGE/Millennium conversions.
- Model parameters remain model-owned and raw by default. If a parameter needs Mimic-managed conversion, the model declares it in a model-local `parameter_units.yaml` file so Mimic can expose converted values or conversion factors.

This design keeps simulation packages faithful to their native units, avoids forcing users into Millennium/SAGE-style units, and aligns with Mimic's vision: a physics-agnostic core, runtime-configurable model packages, metadata as structural truth, reproducible output, and fast failure for invalid metadata. Baseline and scientific regression tests are required throughout the migration because unit conversion errors can preserve compilation while changing the physics.

## Problem

Mimic currently has unit metadata, runtime unit constants, output conversions, and model formulas, but these pieces do not yet form one explicit, enforced contract.

Current behavior:

- `simulation.units` is parsed into `MimicConfig.UnitLength_in_cm`, `UnitMass_in_g`, and `UnitVelocity_in_cm_per_s`.
- `set_units()` derives runtime constants such as `UnitTime_in_s`, `G`, `Hubble`, `RhoCrit`, energy, pressure, and density.
- Core virial helpers use those derived constants to compute physical quantities.
- Tree readers copy raw values into `RawHalo`; generated `copy_from_tree` payload code copies many of those values directly into processing structs.
- `src/core/core_properties.yaml` currently declares both the canonical core fields and simulation-dependent unit/range labels for fields such as `Mvir`, `Rvir`, `Vvir`, `dT`, `CentralMvir`, and infall properties.
- `simulations/<simulation>/halo_properties.yaml` currently declares additional simulation/catalog fields such as `Pos`, `Vel`, `Spin`, `Vmax`, and `VelDisp`, but it does not define a mandatory mapping from raw simulation field names to Mimic's required canonical core fields.
- Property `units` in YAML is required and is written to HDF5 `FieldMetadata` and binary `output_schema.json`.
- `output_convert` can manually convert selected fields at output time.
- Model parameters are parsed as raw `double`, `int`, or `string` values through `model_get_*()`; there is no general parameter-unit metadata or conversion.
- Several model modules, plots, and tests still encode SAGE/Millennium assumptions directly, including `1e10 Msun/h`, `Mpc/h`, `km/s`, fixed code-unit `G`, and explicit factors of `Hubble_h`.
- Current unit labels conflate physical dimensions with the astrophysical `h` scaling convention. In astrophysics `h` is often written like part of a unit label, but in Mimic's contract it must be represented separately as a cosmological scaling exponent.

The gap is that property `units` are currently mostly labels plus manual conversion hints. They do not generally drive input conversion, default conversion, internal storage conversion, range conversion, model/output conversion, formula-level model conversion, or parameter conversion. As a result, a new model or simulation can appear valid while silently mixing incompatible units.

## Motivation

The risk grows as Mimic adds more simulation and model choices. Without a clear unit contract:

- A simulation package can correctly declare native units while a model silently assumes Millennium-style values.
- A simulation package can use different catalog names for required concepts such as virial mass, particle count, snapshot number, halo ID, or tree links, while Mimic currently assumes canonical field names in core/tree structures.
- A model property default or range can be written in human/output units but used as if it were already in simulation internal units.
- Output metadata can claim a physical unit that does not match the value written.
- Tests and plots can hard-code legacy unit assumptions such as `1e10 Msun/h`, `Mpc/h`, or a fixed code-unit `G`.
- Developers adding models may duplicate unit conversions manually, creating drift and inconsistent behavior.
- Formula-level model code can continue to be wrong even after property metadata is converted, because physics equations may contain hidden normalizations, implicit code-unit constants, or explicit `h` factors.

The vision explicitly says Mimic should make hidden assumptions and silent configuration errors harder to introduce. Units are one of the highest-risk hidden assumptions in this architecture, so they need to be part of metadata validation and generated code rather than scattered comments.

## Target Unit Semantics

### Simulation Units

The selected simulation defines Mimic's internal runtime unit system. Its `simulation.units` values are the authoritative mapping from simulation code units to cgs base units:

```yaml
simulation:
  units:
    length_in_cm: ...
    mass_in_g: ...
    velocity_in_cm_per_s: ...
```

Mimic should assume the raw tree/catalog values for that simulation are already in these simulation code units. The tree reader should not convert a simulation into Millennium units or any other canonical package unit. Internal core/halo processing uses the selected simulation's unit basis.

### Core Property Contract And Simulation Mapping

The Mimic core should define canonical required properties by semantic role, not by simulation catalog field name. This contract belongs in core-owned metadata because users should not need to rediscover which fields Mimic requires in order to run.

Core-owned metadata should answer:

- What canonical field does Mimic require?
- Is the field a tree topology field, a structural identifier, a direct halo property, or a derived core property?
- What C type and shape does Mimic use internally?
- Which core function or lifecycle stage creates or updates it?
- Is it required for processing, output, or both?

Core-owned metadata should not hard-code simulation-specific field names, units, `h_power`, physical ranges, or catalog-specific assumptions when those belong to the selected simulation.

Each simulation package must complete the core contract in `simulations/<simulation>/halo_properties.yaml`. This file is the simulation-side single source of truth for all halo/catalog property metadata needed by a run. It should contain:

- A mandatory mapping from simulation catalog fields to Mimic's canonical required core/tree properties.
- Units, dimensions, `h_power`, ranges, sentinels, and conversion rules for mapped core properties where those values are simulation-dependent.
- Metadata for simulation-owned optional halo/catalog properties that the model may use or output, such as position, velocity, spin, maximum circular velocity, or velocity dispersion.

Recommended structure:

```yaml
core_property_map:
  Descendant:
    source: Descendant
    units: dimensionless
    dimension: index
    output: false

  FirstProgenitor:
    source: FirstProgenitor
    units: dimensionless
    dimension: index
    output: false

  FirstHaloInFOFgroup:
    source: FirstHaloInFOFgroup
    units: dimensionless
    dimension: index
    output: false

  SnapNum:
    source: SnapNum
    units: dimensionless
    dimension: index

  Len:
    source: Len
    units: particles
    dimension: count
    range: [20, 1000000000]
    sentinels: [0]

  Mvir:
    source: Mvir
    units: 1e10 Msun/h
    dimension: mass
    h_power: -1
    range: [0.0, 1000000.0]

  MostBoundID:
    source: MostBoundID
    units: dimensionless
    dimension: identifier

derived_core_properties:
  dT:
    units: Myr/h
    dimension: time
    h_power: -1
    range: [0.0, 2000.0]
    sentinels: [-1.0]

  Rvir:
    units: Mpc/h
    dimension: length
    h_power: -1
    range: [0.0, 10.0]
    sentinels: [0.0]

halo_properties:
  - name: Pos
    source: Pos
    type: vec3_float
    units: Mpc/h
    dimension: length
    h_power: -1
    output: true
    init_source: copy_from_tree_array
```

The mapping allows one simulation to call virial mass `Mvir` while another calls it `HaloMass`, `M_Crit200`, or another catalog-specific name. Mimic still exposes the canonical internal property `Mvir` to core code and model modules. The simulation package owns the adapter from catalog naming and units to Mimic's canonical internal contract.

Tree topology fields should be included in this mapping even when they are not output fields. Mimic cannot run without fields such as descendant/progenitor links and FOF-group links, and different catalog formats may name or encode them differently. Validation should fail before build or startup if the selected simulation does not satisfy the complete core mapping contract.

### Hubble Scaling

`h` is a cosmological scale factor, not a base physical unit. Mimic should still support astrophysical labels such as `1e10 Msun/h`, `Mpc/h`, `Myr/h`, and `Gyr/h`, but the metadata contract must represent the `h` dependence separately from the physical dimension and cgs scale.

Recommended semantics:

```yaml
- name: ColdGas
  units: 1e10 Msun/h
  dimension: mass
  h_power: -1
```

The user-facing `units` string remains the label written to output metadata. The structured fields define the conversion. For example:

- `1e10 Msun/h` is a mass scale of `1e10 Msun` with `h_power: -1`.
- `Mpc/h` is a length scale of `Mpc` with `h_power: -1`.
- `Msun/yr` has `h_power: 0` unless a model explicitly declares otherwise.
- `dimensionless`, `particles`, `dex`, and `Internal` are special labels and must not silently receive `h` scaling.

Generated conversion factors should use `MimicConfig.Hubble_h` only through this structured `h_power` metadata. Hard-coded `* Hubble_h`, `/ Hubble_h`, `* 1e10 / h`, or equivalent formula fragments should be removed from generic conversion paths and retained only inside explicitly model-specific formulas that have been audited and documented.

### Core Properties

Core halo properties are processed in internal units. Core-owned metadata defines the canonical property names and behavior, while the selected simulation's `halo_properties.yaml` supplies simulation-dependent mapping and physical metadata.

For core properties that are output, merged metadata must distinguish:

- internal unit dimension and scale, derived from `simulation.units`
- output unit label, declared by the selected simulation for core/simulation-owned halo properties or by the selected model for model-owned properties
- generated output conversion, unless the output unit equals the internal unit

For example, `dT` can be stored internally in simulation code time and output in `Myr/h` or another declared time unit with explicit `h_power` metadata. `Rvir` can be computed internally in simulation length units and output in the declared length unit.

### Simulation Halo Properties

Simulation-owned halo/catalog properties in `simulations/<simulation>/halo_properties.yaml` describe raw tree fields, the mandatory mapping onto Mimic's canonical core fields, and optional simulation-owned output/model-facing fields. Their declared units must match the raw values as read from the tree files unless a reader has an explicit, documented format-level conversion.

This file is mandatory for every simulation package because every simulation must define how Mimic obtains the minimum halo/tree information it needs to run. It should not force values into a model's preferred unit system before core processing. Instead, it maps the catalog into Mimic's internal simulation-defined unit basis and records how values should be exposed at output.

### Model Properties

Model-owned properties in `models/<model>/model_properties.yaml` should be specified in model-facing/output physical units. This applies to:

- `units`
- `init_value`
- `range`
- `sentinels`, where applicable
- output labels
- any metadata used by tests or validation

Generated code should convert model-owned property values into internal simulation units for storage and processing when those properties have physical dimensions. Generated output code should convert them back to the declared model/output units.

This means the model property YAML remains readable to scientists, while internal processing remains coherent with the selected simulation.

### Model Parameters

Model parameters live inside the model package and should remain model-owned by default. A user who sets `SfrEfficiency`, `AGNrecipe`, `GlobalBaryonFraction`, or a dimensionless threshold already understands those values in the context of the model. Most SAGE parameters are currently dimensionless or unit-free and should continue to use the existing raw `model_get_*()` path.

Dimensional parameters need an opt-in metadata path. Each model package should own an optional `models/<model>/parameter_units.yaml` file. If the model has no dimensional parameters that require Mimic-managed conversion, the file may be empty or contain an empty `parameters: []` list.

Recommended schema:

```yaml
parameters:
  - name: ShamMinMpeak
    type: double
    units: 1e10 Msun/h
    dimension: mass
    h_power: -1
    convert: true
```

Rules:

- Parameters omitted from `parameter_units.yaml` are treated exactly as they are today.
- Dimensionless parameters do not need entries.
- A parameter should be listed only when Mimic must provide an internal-unit value or conversion factor.
- Generated helpers should expose converted values or conversion factors, while `model_get_double()`, `model_get_int()`, and `model_get_string()` keep their current raw behavior.
- Validation should fail if `parameter_units.yaml` references an unknown parameter, unsupported unit label, incompatible dimension, or ambiguous `h` scaling.

This keeps KISS and DRY: simple parameters stay simple, and only dimensional parameters enter the unit-conversion system.

## Required Design Changes

### 1. Split The Core Contract From Simulation Mapping

Core metadata should declare the canonical required Mimic fields and derived fields without embedding simulation-specific field names or physical unit labels. Simulation metadata should complete that contract in `simulations/<simulation>/halo_properties.yaml`.

Required generator behavior:

- Load the core canonical property contract from core-owned metadata.
- Load the selected simulation's `halo_properties.yaml`.
- Validate that `core_property_map` provides every required raw core/tree field.
- Validate that `derived_core_properties` provides required simulation-dependent units/ranges for core-derived physical fields.
- Generate canonical internal Mimic fields from the core contract.
- Generate tree-reader or payload mapping code from simulation `source` fields to canonical Mimic fields.
- Merge optional simulation-owned `halo_properties` into the generated halo/output schema after the core contract is satisfied.

This preserves the core as the owner of what Mimic needs while keeping catalog names, units, and ranges in the user-managed simulation package.

### 2. Split Unit Concepts In Metadata

Property metadata needs to distinguish model/output units from internal units. The existing `units` field can remain the output/model-facing unit label, but the generator must treat it as a physical declaration rather than a passive label.

Recommended metadata semantics:

```yaml
- name: ColdGas
  type: float
  units: 1e10 Msun/h
  dimension: mass
  h_power: -1
  description: Cold gas mass available for star formation
  init_source: default
  init_value: 0.0
  range: [0.0, 100000.0]
  output: true
```

The exact schema can vary, but Mimic needs enough structured information to know that `ColdGas` is a mass, resolve the scale represented by `1e10 Msun`, apply the declared `h_power`, and compute conversion factors to and from the selected simulation's internal mass unit. The `units` string is a label; `dimension` and `h_power` are the contract.

### 3. Add A Small Unit Registry

Do not build a broad symbolic unit algebra system. Start with a small explicit registry for units Mimic actually uses:

- mass: `Msun`, `1e10 Msun`, plus explicit `h_power` for labels such as `Msun/h` and `1e10 Msun/h`
- length: `cm`, `kpc`, `Mpc`, plus explicit `h_power` for labels such as `kpc/h` and `Mpc/h`
- velocity: `cm/s`, `km/s`
- time: `s`, `yr`, `Myr`, `Gyr`, plus explicit `h_power` for labels such as `Myr/h` or `Gyr/h` if required
- energy and luminosity: `erg`, `erg/s`
- density/rate forms that are already used by model properties and outputs
- non-physical structural dimensions: `index`, `identifier`, `count`
- dimensionless and special labels such as `particles`, `dex`, and `Internal`

Unknown units should fail validation with a clear message. This is more aligned with KISS than a general parser, and it is safer than silently treating unknown strings as labels.

The registry should not infer `h_power` from string parsing alone. It may check that a label and `h_power` are consistent, but the structured `h_power` field should be the value used by conversion generation.

### 4. Generate Conversion Factors

From `simulation.units` and property metadata, generated code should provide conversion helpers:

- property/output units to internal units
- internal units to property/output units
- opt-in parameter units to internal units
- internal units to output units for generated writers

For scalar physical properties, generated initialization should convert `init_value` into internal units. Generated output should convert internal values back to declared output units. Generated validation manifests should either store both user-facing and internal ranges, or store enough metadata for tests to compare values in the correct unit system. Sentinels must be preserved and not scaled unless explicitly declared as physical values.

### 5. Convert Model-Owned Properties At The Boundary

Model-owned properties should be stored internally in simulation units once initialized. Modules should operate on those internal values so they can safely combine model state with halo/core properties.

Examples:

- A model YAML `ColdGas` default of `1.0` in `1e10 Msun/h` should be converted to the selected simulation's internal mass unit before storage.
- A module comparing `ColdGas` with `Mvir` should compare internal mass units.
- Output should convert `ColdGas` back to `1e10 Msun/h` or the model-declared output unit.

This avoids forcing every module to decide ad hoc whether a field is in model units or simulation units.

### 6. Handle Parameters Deliberately

Parameter conversion should be opt-in through `models/<model>/parameter_units.yaml`, not through scattered module code. A model with no dimensional parameters can ship an empty file:

```yaml
parameters: []
```

A model with dimensional parameters declares only those parameters that need Mimic-managed conversion:

```yaml
parameters:
  - name: ShamMinMpeak
    type: double
    units: 1e10 Msun/h
    dimension: mass
    h_power: -1
    convert: true
```

Then generated or shared helpers can expose:

- `model_get_double()` for raw model-owned values, preserving current behavior
- `model_get_double_internal()` or a generated parameter accessor for converted values

This keeps current simple parameters simple, while giving unit-sensitive parameters a DRY conversion path. Validation should cross-check `parameter_units.yaml` against module dependency metadata and input `modules.parameters` so stale or misspelled parameter-unit declarations fail early.

### 7. Audit And Migrate Model Physics

Every model package must be audited before it is considered compatible with arbitrary simulation units. This includes module formulas, model-local shared helpers, unit tests, integration tests, scientific tests, plots, and documentation.

The migration rule is:

- Generic unit conversion belongs in generated code or shared unit helpers.
- Model-specific physical formulas may keep explicit conversions only when the formula is inherently defined in those physical units and the conversion is documented next to the formula.
- Hard-coded constants such as `1e10`, `100.0`, `Hubble_h`, fixed code-unit `G`, and assumptions like `Mpc/h` or `1e10 Msun/h` must be replaced with generated conversion helpers, internal-unit values, or explicit compatibility validation.
- Baseline parity tests must be run before and after each migrated model workstream to prove that the current SAGE/Millennium behavior remains unchanged when using the current SAGE/Millennium units.
- A model that has not been audited should declare compatible simulation unit systems and fail fast when paired with unsupported units.

This is required because property-boundary conversion alone does not make model physics unit-safe.

### 8. Make Output Conversion Metadata-Driven

Most output conversion should be generated from unit metadata. Manual `output_convert` should become the exception for custom, logarithmic, or derived quantities that cannot be represented by a simple unit conversion.

Generated output schema and HDF5 `FieldMetadata` should reflect the values actually written, not the internal storage units.

### 9. Validate Early

Validation should fail before long runs when:

- a simulation package lacks a `halo_properties.yaml` file
- a required core property has no simulation mapping
- a simulation mapping names a raw catalog field that the selected tree reader cannot provide
- a physical property lacks a dimension or supported unit
- a physical property label implies `h` scaling but `h_power` is absent or inconsistent
- a dimensional parameter requests conversion but lacks supported units
- `parameter_units.yaml` references a parameter that is not declared or used by the model
- a range is specified for a physical property but cannot be converted
- an output conversion would be ambiguous
- a module uses legacy assumptions but is paired with a simulation unit system it does not support

This belongs in generation checks, module validation, and startup validation where appropriate.

## Current Code Areas Affected

The implementation team should inspect at least:

- `src/core/read_parameter_file.c`: parses `simulation.units`
- `src/core/init.c`: derives internal runtime constants
- `src/core/virial.c`: uses internal constants for halo physical calculations
- `src/core/core_properties.yaml`: should become the canonical core property contract rather than the place where simulation-specific units/ranges are declared
- `src/include/types.h` and tree-reader code: currently assume raw catalog fields are available under fixed `RawHalo` member names
- `scripts/generate_properties.py`: should merge the core contract, simulation core mapping, simulation-owned halo properties, model properties, initialization, output conversion, HDF5 metadata, binary schema, and validation manifests
- `src/include/generated/property_defs.h`: generated internal structs and initialization helpers
- `src/include/generated/populate_halo_payload_from_tree.inc`: should be generated from simulation `source` mappings rather than assuming matching canonical/raw field names
- `src/io/output/util.c`: generated output preparation
- `src/io/output/hdf5.c` and `src/io/output/metadata_hdf5.c`: output metadata and values
- `plot/mimic-plot/output_schema.py` and model-local plot modules: schema-aware output reading and hard-coded unit assumptions
- `src/core/module_registry.c`: model parameter accessors
- `scripts/discovery.py`, `scripts/validate_modules.py`, and `scripts/lint_parameter_usage.py`: model-local `parameter_units.yaml` discovery, validation, and consistency checks
- `models/*/model_properties.yaml`: model-owned property units, defaults, ranges, and output labels
- `models/*/parameter_units.yaml`: opt-in dimensional parameter metadata
- `models/*/modules/*/module_info.yaml`: parameter metadata and property dependency metadata
- `simulations/*/halo_properties.yaml`: mandatory simulation-side core property map plus optional simulation-owned halo/catalog properties
- `models/*/modules/**/*.c` and `models/*/shared/*`: model formulas and helper code that may contain hard-coded SAGE/Millennium unit assumptions
- `tests/scientific/test_scientific.py`: currently has unit-specific hard-coded checks
- baseline, unit, integration, and model-local scientific tests: regression protection for physics-preserving migration

## Implementation Pathway

### Phase 1: Define And Enforce Metadata Semantics

Update user/developer documentation and generator validation to state:

- simulation units define internal runtime units
- the core owns canonical required halo/tree property semantics
- the selected simulation maps catalog fields onto those canonical core properties in `simulations/<simulation>/halo_properties.yaml`
- `halo_properties.yaml` is mandatory for every simulation package and is the simulation-side single source of truth for mapped core fields plus optional simulation-owned halo/catalog fields
- model property YAML units are model-facing/output units
- `h` scaling is represented by structured metadata such as `h_power`, not inferred as an ordinary unit
- generated code converts model-owned properties into internal units for processing
- output is converted back to declared property units
- parameter conversion is opt-in through `models/<model>/parameter_units.yaml`
- unaudited models may declare compatible simulation unit systems and reject unsupported combinations

Add validation that rejects missing core mappings, unknown catalog source fields, unsupported physical units, missing dimensions for physical properties, missing or inconsistent `h_power`, and invalid special labels. Initially this can be narrow and only cover units currently present in the repository.

### Phase 2: Add Core Property Mapping Infrastructure

Teach the generator and tree-reader layer to use simulation `source` mappings instead of assuming that raw catalog fields and Mimic canonical fields have the same name. The first implementation can keep the existing `RawHalo` shape for current LHaloTree inputs, but generated mapping metadata should become the source of truth for how raw fields populate canonical Mimic fields.

Required behavior:

- `src/core/core_properties.yaml` defines canonical required fields and derived core fields.
- `simulations/<simulation>/halo_properties.yaml` maps raw catalog fields to canonical required fields.
- Generated initialization code populates canonical Mimic properties from the mapped raw source fields.
- Build or startup validation fails if a selected tree format cannot provide a mapped source field.

This phase should preserve current mini-Millennium and Millennium behavior exactly while making alternate field names possible.

### Phase 3: Add Unit Resolution Infrastructure

Add a small unit registry in the generator/tooling layer. It should resolve a unit label, dimension, and `h_power` into a scale factor relative to cgs or directly relative to `simulation.units`.

Keep this in Python generation code first if possible. Generate C constants or inline functions rather than parsing unit strings at runtime.

The registry should support only the labels Mimic uses. It should not attempt broad symbolic unit algebra.

### Phase 4: Convert Generated Property Initialization, Ranges, And Output

Teach `scripts/generate_properties.py` to:

- merge simulation physical metadata into canonical core properties before code generation
- convert physical `init_value` values into internal units in generated initialization
- convert physical ranges into internal ranges for validation, or emit both forms for tests
- generate output conversion from unit metadata, including `h_power`
- preserve sentinels correctly
- keep manual `output_convert` for custom cases

This is the main DRY win: property conversions live in generated code rather than hand-coded modules.

### Phase 5: Add Opt-In Parameter Unit Metadata

Add model-local `parameter_units.yaml` discovery and validation. Keep existing `model_get_*()` behavior for raw parameters. Add optional metadata only for dimensional values that need internal conversion.

Add one converted accessor path, then migrate only parameters that are actually compared with internal physical values. A model with no dimensional parameters should use `parameters: []` and require no code changes.

### Phase 6: Audit And Migrate Model Physics

Audit SAGE, SHAM, and any other model packages for hard-coded unit assumptions. Update code to rely on internal values or generated conversion helpers where the formula should be unit-system agnostic. Keep explicit physical conversions only for genuinely model-specific formulas, and document those conversions next to the formula.

Run the relevant baseline, unit, integration, and scientific tests before and after each model migration. The current SAGE/Millennium configuration should remain numerically stable under the existing units unless a deliberate physics change is separately approved.

### Phase 7: Audit Tests And Plots

Audit plots that hard-code conversions such as `Mvir * 1e10 / h` or assume `(Mpc/h)^3`. Prefer schema/profile-based helpers so plots reflect the output units actually written.

Update scientific tests to compute expected constants from the active simulation units rather than hard-coding Millennium-specific values.

Tests that validate model parity should continue to protect the legacy SAGE/Millennium behavior while new unit-system tests exercise alternative simulation units.

## KISS And DRY Guidance

Keep the unit system deliberately small. Mimic does not need a general scientific unit language to solve this problem. It needs a supported set of dimensions, labels, scale factors, special labels, and explicit `h_power` values that appear in Mimic metadata.

Keep the simulation-side contract in one file. `simulations/<simulation>/halo_properties.yaml` should be the single user-managed source for mapped core halo/tree fields and optional simulation-owned halo/catalog fields. Do not create a separate simulation mapping file unless the combined file becomes unmanageably large.

Keep the core contract minimal. Core metadata should define canonical names, types, roles, required status, and core lifecycle behavior. It should not duplicate units, ranges, or catalog source names owned by the simulation package.

Centralize conversions in the generator and shared helpers. Do not scatter `* 1e10 / h`, `* UnitMass_in_g`, or custom time conversions through modules and plots unless the formula is genuinely model-specific and documented.

Keep parameter conversion opt-in. The absence of an entry in `parameter_units.yaml` means the parameter remains raw and model-owned exactly as today.

Use baseline tests as migration guardrails. Unit conversion work should preserve existing SAGE/Millennium behavior under the existing units while enabling correctly validated non-Millennium unit systems.

Prefer fail-fast validation over permissive fallback. Unknown units, ambiguous dimensions, and unsupported conversions should be metadata errors.

Do not force users to adopt Millennium units. The simulation sets internal units; model YAML remains readable in model/output units; generated conversions bridge the two.

## Alignment With The Vision

Physics-agnostic core infrastructure: the core still owns execution, memory, I/O, metadata, validation, and runtime units without depending on a specific physics model.

Runtime modularity: model/simulation combinations remain selectable, but unit compatibility becomes explicit and validated.

Metadata as structural truth: core property contracts, simulation halo mappings, model property metadata, and opt-in parameter-unit metadata become the source for catalog mapping, unit conversion, output labels, defaults, ranges, and validation rather than passive documentation.

One coherent processing model: all internal processing uses one active internal unit system, defined by the selected simulation.

Format-agnostic I/O and reproducible output: output files carry units that match the values written, and binary/HDF5 readers can trust schema metadata.

Validation, type safety, and fast failure: missing core mappings, unknown catalog source fields, unsupported units, ambiguous `h` scaling, incompatible model/simulation combinations, and stale parameter-unit metadata fail early instead of producing silent scientific errors.

## Decision

Adopt simulation-defined internal units and metadata-driven model/output conversion. The core should define canonical required halo/tree properties, while `simulations/<simulation>/halo_properties.yaml` maps simulation catalog fields onto those canonical properties and declares the simulation-owned units, `h_power`, ranges, and optional halo/catalog fields. Treat `h` as an explicit scaling exponent rather than a physical unit. Keep raw model parameters as the default, and add model-local `parameter_units.yaml` only for dimensional parameters that need Mimic-managed conversion. Audit each model package so formula-level physics is correctly migrated away from hidden SAGE/Millennium unit assumptions, using baseline tests to preserve current behavior under current units. The implementation should not force a universal canonical unit system, and it should not require users to manually convert model property YAML into simulation units. Mimic should do that conversion from explicit metadata, keep the supported unit system small, and fail loudly when metadata is insufficient.
