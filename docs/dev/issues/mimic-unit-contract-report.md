# Mimic Unit Contract — Implementation Specification

Version: 2.1
Date: 2026-06-14
Status: Implemented in working tree; Uchuu package still pending

---

## Implementation Update

This report's design has been implemented in the current working tree on 2026-06-14, with no Uchuu package added yet.

The implementation follows the approved contract and uses visible carried-h labels for the fixed reference basis (`1e10 Msun/h`, `Mpc/h`, `km/s`). That keeps the human-facing label, `output_schema.json`, HDF5 `FieldMetadata`, and stored numeric values directly aligned. Where examples below use shorthand labels such as `1e10 Msun` or `Mpc` with `h_convention: carried`, read them as the same carried-h reference convention now written explicitly as `/h` in implemented metadata.

Completed implementation points:

- `src/core/core_properties.yaml` now declares `reference_units`, required core inputs, and dimensions for core-produced physical fields.
- Simulation halo metadata now declares `core_property_map`, explicit catalog properties, source/raw/HDF5 names, units, and h conventions for both Millennium packages.
- The generator now emits reference-unit constants, raw tree accessors/conversions, HDF5 read metadata, output conversion code, schema reference-unit metadata, and parameter-unit conversion helpers.
- `set_units()` derives runtime constants from generated core reference units rather than `simulation.units`.
- Runtime simulation config parsing now accepts explicit `{value, units, h_convention}` metadata for `box_size` and `particle_mass`; legacy scalar values remain accepted for compatibility, but the legacy `simulation.units` block is rejected.
- Raw catalog values flow through generated accessors in payload copy-in and virial-mass fallback logic.
- SHAM's `ShamMinMpeak` uses opt-in model-global `models/sham/parameter_units.yaml` plus `model_get_double_internal()`.
- Plotting and scientific tests now consume reference-unit metadata from `output_schema.json` instead of hard-coded Millennium assumptions.
- Run metadata no longer copies property YAML snapshots; provenance is through `output_schema.json`, HDF5 field metadata, copied run/simulation configs, snapshot list, Python example, and version metadata.

Verification completed:

- `make MODEL=sage16 SIMULATION=mini-millennium generate check-generated validate-modules`
- `make MODEL=sage16 SIMULATION=millennium generate check-generated validate-modules`
- `make MODEL=sham SIMULATION=mini-millennium generate check-generated validate-modules`
- Restored default generated files with `make MODEL=sage16 SIMULATION=mini-millennium generate check-generated validate-modules`
- `make`
- `make check-format`
- `make check-docs`
- `mimic_venv/bin/python tests/integration/test_unit_contract_generation.py`
- `make tests-scientific summary` (passed with the existing zero-value warning marker)
- Delegated `make tests-unit` with log capture to `archive/test-logs/tests-unit-unit-contract.log` (exit code 0; one expected skip marker)
- Delegated `make tests-integration` with log capture to `archive/test-logs/tests-integration-unit-contract.log` (exit code 0; no failing markers)

Remaining work outside this implementation: confirm Uchuu's actual catalog fields, units, h convention, value ranges, and tree-reader format before adding an Uchuu simulation package.

## 1. Executive Summary

Mimic needs an explicit, enforced unit contract before additional simulations and models are added. The immediate forcing function is the **Uchuu** simulation, which defines mass and other halo properties with a different unit scale and different catalog field names than the bundled Millennium catalogs, and which will be paired with new models that assume their own units. Uchuu also makes performance a first-class concern: the full merger-tree dataset is ~37 TB, and this scale will become normal as Mimic grows.

The adopted design is:

- **One fixed internal reference unit system** for all of Mimic, equal to today's effective code units: mass in `1e10 Msun/h`, length in `Mpc/h`, velocity in `km/s`, with little-h carried numerically in mass and length values (the Millennium convention). Core physics constants (`G`, `Hubble`, `RhoCrit`, etc.) are derived from this fixed basis and never vary by simulation. It is declared once, in core.
- **Conversion at the tree-reader boundary.** Each simulation declares its catalog field names, per-field unit labels, and h convention. Generated copy-in code converts each catalog value into reference units *as it is read*, folded into the field copy that already happens. Mimic never transcodes the dataset to disk.
- **All core and model physics runs in reference units, unchanged.** Because the internal basis is fixed, every formula sees the same constants and the same-unit inputs for every simulation. There is no per-model "make physics unit-agnostic" audit. Millennium catalogs are an identity conversion, so existing output stays byte-identical to the upstream `sage-model` baseline by construction.
- **Metadata-driven output.** Every output property declares (or inherits) its output unit label; generated code converts reference→label (identity when equal). The written value is in the labeled unit by construction, closing today's gap where the label and the value are produced by independent, unchecked code paths.
- **Opt-in dimensional parameters.** Parameters are global to a run, not module-owned. A parameter that must be compared against internal physical values declares units in a model-global, opt-in `models/<model>/parameter_units.yaml`; all other parameters stay raw exactly as today.

This delivers the user-facing goal — native catalogs in, scientist-readable model YAML, output labels that match values — while keeping the engineering small, keeping the hot path free of measurable cost, and preserving the existing baseline. It directly serves the vision: a physics-agnostic core, metadata as structural truth, reproducible output, and fast failure on invalid metadata.

---

## 2. Context and Motivation

### 2.1 The concrete trigger: Uchuu

Mimic today bundles one simulation with data (mini-Millennium); `millennium` exists as metadata with identical units. Uchuu is the first simulation that genuinely differs:

- Its catalog uses a **different unit scale** (e.g. masses and lengths not expressed as `1e10 Msun/h` and `Mpc/h`).
- Its catalog uses **different field names** (e.g. virial mass is not the column Mimic currently hard-codes as `Mvir`).
- It will be paired with **new models** that were written assuming their own unit conventions.
- It is **large**: ~37 TB of merger trees, and this scale is expected to be common going forward.

### 2.2 The gap in the current system

Mimic already has unit metadata, runtime unit constants, output conversions, and model formulas, but they do not form one enforced contract:

- `simulation.units` is parsed into `MimicConfig.UnitLength_in_cm`, `UnitMass_in_g`, `UnitVelocity_in_cm_per_s` (`src/core/read_parameter_file.c`).
- `set_units()` (`src/core/init.c:77`) derives `UnitTime_in_s`, `G`, `Hubble`, `RhoCrit`, `UnitDensity_in_cgs`, `UnitPressure_in_cgs`, `UnitEnergy_in_cgs`, `UnitCoolingRate_in_cgs` **from those parsed simulation units**. Today this is harmless only because the one simulation's units equal the values these formulas were tuned for.
- Tree readers copy raw catalog values into a fixed-layout `struct RawHalo` (`src/include/types.h:8`); the generated copy-in (`src/include/generated/populate_halo_payload_from_tree.inc`) assumes catalog field names match Mimic's canonical names (`payload.Mvir = get_virial_mass(halonr)`, `payload.Pos[j] = InputTreeHalos[halonr].Pos[j]`, …).
- Core-owned property metadata is split awkwardly: `src/core/core_properties.yaml` declares canonical fields **and** simulation-specific unit/range labels (e.g. `Mvir` as `1e10 Msun/h`), while the catalog field that *feeds* `Mvir` (`M_Crit200` in mini-Millennium) is implicit in the tree reader.
- Property `units:` is required and written verbatim to HDF5 `FieldMetadata` and the binary `output_schema.json`, but it is **just a label**. The value path (`output_convert` expressions in `scripts/generate_properties.py:638-661,699-723`) and the label path (`generate_properties.py` HDF5 writer ~`812-901`, binary schema writer ~`990-1045`) never cross-check each other.
- Model parameters are declared with values globally in the run YAML under `modules.parameters:` and read raw via `model_get_double/int/string()` (`src/core/module_registry.c:1140`). `module_info.yaml`'s `parameters:` list is only a per-module *usage* declaration consumed by `scripts/lint_parameter_usage.py`; the same parameter legitimately appears in several modules' lists. There is no parameter-unit metadata anywhere.

The result: a new simulation or model can compile and run while silently mixing incompatible units, because nothing converts catalog values into a known basis and nothing enforces that an output label matches the value written.

### 2.3 Why units, specifically

The vision states Mimic should make hidden assumptions and silent configuration errors harder to introduce. Units are the highest-risk hidden assumption in this architecture: a unit error preserves compilation and often looks plausible in a plot (a factor of little-h ≈ 0.7 is easy to miss), while corrupting the science. The contract must live in metadata, generated code, and validation rather than in scattered comments and copy-pasted conversions.

---

## 3. Key Concept: Two Unit Systems and Two Meanings of "h"

Implementers must keep four distinct quantities separate. Conflating any pair is the source of the bugs this contract prevents.

### 3.1 Reference units vs. catalog units

- **Reference units (fixed, internal):** the one basis all of Mimic computes in — `mass = 1e10 Msun`, `length = Mpc`, `velocity = km/s`, time derived. This is core-owned, declared once, and identical for every simulation. All physics constants and all module formulas operate here.
- **Catalog units (per-simulation, native):** the units a given simulation's tree files use on disk. Declared **per field** as unit labels in the simulation package (not as a global cgs block). A small unit registry resolves each label to cgs, so the catalog→reference conversion factor for a field is `registry_cgs(field_label) / registry_cgs(reference_label)`, with a factor of little-h applied only when the field's h convention differs from the reference convention.

For Millennium catalogs, every field label already equals the reference unit, so every conversion factor is exactly `1.0` and behavior is byte-identical to today.

Because the catalog scale is fully determined by per-field labels + the registry + the core reference units, the legacy global `simulation.units` cgs block (`length_in_cm`/`mass_in_g`/`velocity_in_cm_per_s`) is **redundant and is removed** (see §4.3). Per-field labels are also strictly more expressive: a catalog may mix units within one dimension (e.g. a mass in `Msun` and a rate in `Msun/yr`), which a single global scale cannot represent.

### 3.2 The value of little-h vs. the h convention

- **The value of little-h (`hubble_h`, e.g. 0.73 for Millennium, 0.6774 for Uchuu/Planck):** a *cosmological parameter*, declared per simulation in `simulation.cosmology`, used inside physics. It is **not** a unit and is **not** touched by the unit contract. Formulas that use `MimicConfig.Hubble_h` simply pick up the correct per-simulation value, which is already correct.
- **The h convention of the catalog (carried vs. free):** whether catalog values embed little-h (e.g. mass in `Msun/h`) or are h-free (e.g. mass in physical `Msun`). This *is* a unit concern. The reference convention is **h-carried** (the Millennium convention). If a simulation's catalog is h-free, the reader applies the appropriate factor of little-h at the boundary to bring values into the h-carried reference convention. This is the only place the contract manipulates h.

This separation is why "make SAGE simulation-agnostic" requires no formula audit: `set_units()` derives constants from the fixed reference basis, so every formula sees identical constants and reference-unit inputs for every simulation; the per-simulation differences (catalog scale, catalog field names, catalog h convention, and the cosmological value of `hubble_h`) are all absorbed before physics runs or are already physical inputs.

---

## 4. The Unit Contract

### 4.1 Reference unit system (core-owned)

The core declares the fixed reference basis as metadata so it is structural truth, not a magic constant. Recommended location: a `reference_units:` block in `src/core/core_properties.yaml` (see the example in Appendix A.1).

```yaml
reference_units:
  mass:     { label: "1e10 Msun", in_g: 1.989e43,     h_convention: carried }
  length:   { label: "Mpc",       in_cm: 3.08568e24,  h_convention: carried }
  velocity: { label: "km/s",      in_cm_per_s: 1.0e5, h_convention: none }
  # time is derived from length/velocity, as today
```

`set_units()` must derive `G`, `Hubble`, `RhoCrit`, `UnitTime_in_s`, `UnitDensity_in_cgs`, `UnitPressure_in_cgs`, `UnitEnergy_in_cgs`, and `UnitCoolingRate_in_cgs` from this fixed reference basis — **not** from any per-simulation catalog units. `read_parameter_file.c` no longer parses a `simulation.units` cgs block. For Millennium this yields numerically identical constants to today (the reference values equal Millennium's former `simulation.units`), preserving the byte-identical baseline.

### 4.2 Core property contract (core-owned)

`src/core/core_properties.yaml` is core machinery. **Users never edit it to add a simulation or a model.** It owns three things, none of which carry simulation-specific unit labels:

1. **`reference_units`** — the fixed internal basis (§4.1).
2. **Required inputs** — the canonical fields core needs the selected simulation to provide, declared by *role* only (tree-topology link, index, count, mass-input). No catalog names, no units. The simulation satisfies these via `core_property_map` (§4.3). This includes the merger-tree topology links (`Descendant`, `FirstProgenitor`, `NextProgenitor`, `FirstHaloInFOFgroup`, `NextHaloInFOFgroup`), which today are hand-coded in `src/include/types.h`; bringing them into metadata is part of this work.
3. **Core-produced fields** — fields Mimic computes/stamps that are never read from disk:
   - *Internal working state*, never output: `CentralHalo`, `HaloNr`.
   - *Output identifiers*, simulation- and model-independent, dimensionless: `Type`, `UniqueGalaxyID`, `UniqueCentralGalaxyID`.
   - *Derived/tracked physical fields*, output, with sim-dependent **dimension but no hard-coded unit label**: `Mvir`, `Rvir`, `Vvir`, `dT`, `deltaMvir`, `CentralMvir`, `infallMvir`, `infallVvir`, `infallVmax`. Core declares only their `dimension` (mass/length/velocity/time) and the core function that computes them (e.g. `get_virial_mass` in `src/core/virial.c`). Their output label defaults to the reference unit for that dimension; a simulation may override it (§4.4).

Because core declares dimension rather than a fixed `1e10 Msun/h`-style label for derived fields, core is finally free of simulation-specific unit assumptions — resolving a long-standing defect.

### 4.3 Simulation contract (per-simulation, mandatory)

`simulations/<simulation>/halo_properties.yaml` is the single source of truth for everything about a simulation's on-disk halo catalog. It is mandatory for every simulation package and contains:

1. **`core_property_map`** — the compulsory mapping that satisfies core's required inputs by naming the catalog column for each canonical role (e.g. `VirialMassInput: M_Crit200`). Each mapped name points at a real field defined in the `halo_properties` list below.
2. **`halo_properties`** — every catalog field as represented on disk: its `source` (catalog column), `type`, `units` label, `h_convention`, whether it is `output`, and any output transform. The reader generates each field's catalog→reference conversion from `(units, h_convention)` + the registry. This list includes both the fields that satisfy core inputs (e.g. `SnapNum`, `Len`, the virial-mass-input column) and the simulation's own optional output fields (`Pos`, `Vel`, `Spin`, `Vmax`, `VelDisp`, `MostBoundID`).

The former global `simulation.units` cgs block is removed from `simulation_info.yaml`. Scalar quantities there that previously relied on the implicit global scale — `particle_mass` and `box_size` — gain explicit `units`/`h_convention` (see Appendix A.4). A worked `halo_properties.yaml` is in Appendix A.2.

Note the disambiguation the contract introduces: the catalog's spherical-overdensity mass column (`M_Crit200`) maps to the canonical role `VirialMassInput` and feeds `get_virial_mass`, which produces the core-owned derived output field `Mvir`. The old code conflated both under the name `Mvir`.

### 4.4 Output: ownership and labels

Output metadata is the union of three sources, and a fresh team should expect all three to appear in an output file:

- **Core-produced fields** (§4.2.3) — the universal identifiers and the derived/tracked physical fields.
- **Simulation catalog fields** (§4.3) — everything read from disk that is marked `output`.
- **Model galaxy properties** (§4.5).

Every output property declares (or, for derived core fields, inherits) the unit label it is written in. The human-facing label is a free-form string (e.g. `1e10 Msun/h`, `Mpc/h`, `Msun`) — including the `/h` suffix when the value is h-carried — and is written verbatim to HDF5 `FieldMetadata` and the binary schema. The conversion math, however, is driven by the structured scale + `h_convention` fields, not by parsing that string, so the label and the value cannot silently diverge. Generated output code converts reference→label using those structured fields. When the label equals the reference unit, the conversion is identity and the value is written verbatim (preserving byte-identical Millennium output). For derived core fields the output label defaults to the reference unit for the field's dimension; a simulation may override it (e.g. to output `Mvir` in `Msun` rather than `1e10 Msun/h`). Sentinels are preserved and never scaled unless explicitly declared physical. The existing `output_convert`/`output_transform` mechanism (used today for SFR, `log10(erg/s)` cooling/heating, Gyr times, and `dT`) is retained for genuinely custom or non-linear cases; metadata-driven conversion replaces only the linear unit conversions.

### 4.5 Model properties (model-owned)

`models/<model>/model_properties.yaml` declares model-owned galaxy properties in model-facing/output units (the `units:`, `init_value:`, `range:`, `sentinels:`, output label). Where a model property has a physical dimension and a non-trivial `init_value`/`range`, generated code converts those into reference units for storage/processing and converts back for output. Properties whose `init_value` is `0.0`/sentinel (the common case in SAGE) need no input conversion.

### 4.6 Parameters (model-owned, opt-in, model-global)

Parameters are **global to a run, not module-owned**: they are declared with values in the run YAML under `modules.parameters:`, read by name via `model_get_double/int/string()` from a global pool, and a single parameter may be used by several modules. Their unit metadata therefore belongs at model-global scope — not in `module_info.yaml` (which would duplicate the unit across every module that uses the parameter) and not in the run YAML (which is per-run and would repeat and risk drift).

A model that has dimensional parameters needing conversion ships an opt-in `models/<model>/parameter_units.yaml`, listing only those parameters. Absent or empty (`parameters: []`) means every parameter stays raw exactly as today. A generated/shared accessor (`model_get_double_internal()` or equivalent) exposes the converted value; the raw accessors keep current behavior. Today the only dimensional parameter wired through this path is SHAM's `ShamMinMpeak`, a mass threshold compared against galaxy peak mass. It is declared in `1e10 Msun/h`, which equals the reference mass unit, so its conversion factor is currently the identity — the migration exercises the opt-in machinery without changing behavior, and the path is ready for a future parameter declared in a non-reference unit (e.g. physical `Msun`). (The separate `mpeak * 1.0e10 / h` expression in `sham_assign_stellar_mass.c` converts the per-galaxy peak *mass* to physical solar masses for the stellar-mass-halo-mass relation; it is not a parameter conversion.) See Appendix A.3.

---

## 5. Design Rationale and Alternatives Considered

The chosen approach (fixed reference basis + boundary conversion) was selected over three alternatives. This section records *why* so the decision is not re-litigated.

### 5.1 Why not native per-simulation internal units

The intuitive alternative is "internal units = each simulation's native units; don't convert at the reader." It is rejected because:

- **Internal units are invisible to users.** A scientist only sees the catalog (untouched on disk) and the output (labeled). Native internal units provide no user-facing benefit.
- **It manufactures a formula audit.** If `set_units()` derived constants from native units, then every model formula tuned for the `1e10 Msun/Mpc/km·s⁻¹` basis (e.g. the `Hubble_h`/`1e10` factors in `sage_calculate_supernova_feedback.c`, `sage_radio_mode_heating.c`, `sage_reionization.c`, `sage_starburst_feedback.c`, `sage_agn_physics.h`) would have to be re-derived and re-verified per simulation — the riskiest, most expensive work, and the one most likely to perturb the byte-identical baseline.
- **It scales as N×M.** With N models and M simulations, every model must be valid under every simulation's native units. A fixed reference pivot makes this **N + M**: each simulation declares one catalog→reference mapping; each model declares its output labels; physics lives in the one basis. This is decisive as Mimic grows.

### 5.2 Why not offline transcoding of the catalog

Converting 37 TB into a second 37 TB file in reference units would roughly double storage and triple I/O. The contract instead converts **in stream, at the existing copy-in**, so conversion is a multiply on data already moving through cache (see §6). Mimic must never write a converted copy of the dataset to disk.

### 5.3 Why not fail-fast-only (validate and refuse)

A "detect incompatible units and abort" approach is cheap and safe, but with Uchuu imminent it is not a sufficient end state — it would force users to pre-convert catalogs offline (the 37 TB hazard of §5.2). Fast-failure validation is retained as a *safety layer* (§9), not as the whole solution.

### 5.4 What is deliberately not built

- No general symbolic unit algebra — only a small registry of the labels Mimic actually uses (§7.3).
- No `h_power`/`dimension` field sprinkled onto every internal property. The h convention is declared only where conversion happens (catalog field labels and output labels). Under the byte-identical constraint, internal values stay h-carried, so a universal internal `h_power` could only ever multiply by 1.
- No parameter-unit metadata in `module_info.yaml`. Parameters are global, not module-owned (§4.6); their units live in a model-global `parameter_units.yaml`.
- No model-physics "unit-agnostic" audit — unnecessary because internal == reference always.

---

## 6. Performance and Scale (37 TB)

The unit contract is performance-neutral on the hot path. The reasoning, for the implementing team:

- **The dominant cost is I/O, which is identical under every design.** Reading 37 TB (plus decompression) and running SAGE physics over multiple substeps per halo vastly exceeds any unit arithmetic.
- **Conversion is free because the copy already exists.** The tree readers already copy disk→`RawHalo` and the generated copy-in already copies `RawHalo`→processing struct field by field. Conversion turns `x = raw.x` into `x = raw.x * CONV`, a load-multiply-store on data already in registers/cache, hidden under memory latency.
- **Order of magnitude:** ~37 TB at ~100 bytes/halo ≈ 4×10¹¹ halos; ~10 multiplies/halo ≈ 4×10¹² FLOPs for the entire dataset ≈ a few hundred core-seconds, versus tens of thousands of seconds just to read 37 TB. Conversion is well under 1% of I/O time and a far smaller fraction of total runtime.
- **Convert once, at copy-in.** Conversion happens once per halo when the processing struct is built; physics reuses the converted value across all substeps. This is strictly cheaper than any design that leaves residual per-formula conversions running per substep.
- **Conversion factors are compile-time/init-time constants.** Generate C constants or inline factors; never parse unit strings at runtime.
- **Pay only for fields you read.** Convert required core fields and fields models consume at input; pure pass-through output-only fields may instead be converted at output (negligible volume — galaxies ≪ halos, written once).

The real optimization levers for Uchuu-scale data are streaming/forest-chunked reads, MPI domain decomposition, and decompression throughput — not unit arithmetic. The unit contract must not be shaped by conversion-cost concerns; it is a rounding error in the I/O budget.

---

## 7. Required Design Changes

### 7.1 Decouple reference units from catalog units

- Introduce core-owned `reference_units` metadata (§4.1, Appendix A.1).
- Change `set_units()` (`src/core/init.c:77`) to derive constants from the reference basis.
- Remove the global `simulation.units` cgs block from `simulation_info.yaml`; remove its parsing from `read_parameter_file.c`. The catalog scale now comes from per-field labels + the registry.
- Give `particle_mass` and `box_size` explicit `units`/`h_convention` in `simulation_info.yaml`; convert `particle_mass` into reference units before use in `get_virial_mass()` (`src/core/virial.c`). Identity for Millennium.

### 7.2 Core/simulation contract split and field-name mapping

- `src/core/core_properties.yaml`: restructure into `reference_units` + required-input contract + core-produced fields (internal, identifiers, derived-by-dimension). Remove all simulation-specific unit/range labels (§4.2, Appendix A.1).
- `simulations/<simulation>/halo_properties.yaml`: add the mandatory `core_property_map` plus the full catalog `halo_properties` list with per-field labels (§4.3, Appendix A.2).
- Bring the merger-tree topology links into the metadata system (they are currently hand-coded in `src/include/types.h`).
- `scripts/generate_properties.py`: load the core contract + simulation map; validate completeness; generate the canonical-field copy-in from `source` mappings; merge catalog fields and core-produced fields into the halo/output schema.
- `src/include/generated/populate_halo_payload_from_tree.inc`: now generated from the mapping rather than assuming name matches.
- `src/include/types.h` / tree readers (`src/io/tree/binary.c`, `src/io/tree/hdf5.c`): the `RawHalo` layout and `READ_TREE_PROPERTY` calls become driven by the simulation's declared catalog fields. The first implementation may keep the existing `RawHalo` shape for LHaloTree inputs while making the mapping the source of truth.

### 7.3 Small unit registry and conversion-factor generation

- Add a small explicit registry in the generator covering only the labels Mimic uses: mass (`Msun`, `1e10 Msun`), length (`cm`, `kpc`, `Mpc`), velocity (`cm/s`, `km/s`), time (`s`, `yr`, `Myr`, `Gyr`), energy/luminosity (`erg`, `erg/s`), the structural labels (`index`, `identifier`, `count`, `particles`), and the special labels (`dimensionless`, `dex`, `Internal`, `log10(...)`). The registry maps each label to cgs. Unknown labels fail validation.
- For each dimensioned field, generate the catalog→reference factor (for input) and reference→label factor (for output), applying a factor of little-h only when h conventions differ.

### 7.4 Metadata-driven output

- `scripts/generate_properties.py`: generate reference→label conversion for output properties from metadata; emit identity when label equals reference; default derived-core-field labels to the reference unit with optional simulation override; retain manual `output_convert`/`output_transform` for custom/non-linear cases; preserve sentinels.
- Make the HDF5 `FieldMetadata` and binary schema writers emit the label that matches the generated conversion (so value == label by construction).
- Add a **generation-time assertion**: any property whose output label differs from its reference unit must have a conversion (generated or manual); otherwise `make generate` fails.

### 7.5 Opt-in parameter units

- Add `models/<model>/parameter_units.yaml` (opt-in; absent/empty ⇒ all raw). See Appendix A.3.
- `scripts/discovery.py`, `scripts/validate_modules.py`, `scripts/lint_parameter_usage.py`: discover and validate `parameter_units.yaml`; cross-check its entries against parameters actually declared in the run YAML and used by modules.
- Add one converted accessor (e.g. `model_get_double_internal()`) in `src/core/module_registry.c`; keep raw accessors unchanged. Migrate only `ShamMinMpeak` initially.

---

## 8. Implementation Plan

Phases are ordered cheapest-and-highest-value first. Phase 1 is independent and can ship before the engine. Run baseline/scientific regression before and after each phase; Millennium output must remain byte-identical unless a physics change is separately approved.

### Phase 1 — Output trust and present-defect cleanup (independent, do first)

- Add the generation-time label/value consistency assertion (§7.4).
- Fix the verified present defects (§10).
- Route plotting through schema metadata: make plot figures and the scientific test read units from the run-local schema (`plot/mimic-plot/output_schema.py:units_from_schema`, currently unused) instead of hard-coding conversions.

This makes output trustworthy and de-duplicates conversions with no change to the numeric core. It is a prerequisite for, and independent of, the rest.

### Phase 2 — Core/simulation contract and field-name mapping

- Implement §7.2. Preserve Millennium behavior exactly (identity mapping). This is what lets Uchuu's catalog field names (e.g. `M_Crit200`) populate canonical fields.

### Phase 3 — Reference basis and boundary conversion

- Implement §7.1 and §7.3. `set_units()` derives from the reference basis; the global `simulation.units` block is removed; the reader converts catalog→reference at copy-in; `particle_mass` converted. Millennium = identity factors = byte-identical. This is what lets Uchuu's different unit scale and h convention work.

### Phase 4 — Metadata-driven output and model-property input conversion

- Complete §7.4 generation. Convert physical model `init_value`/`range` into reference units where applicable (§4.5).

### Phase 5 — Opt-in parameter units

- Implement §7.5. Migrate `ShamMinMpeak` to the converted accessor; leave all dimensionless parameters raw.

### Not a phase — model-physics audit

Explicitly out of scope. Because internal == reference units for every simulation, SAGE/SHAM formulas and their existing constants remain correct and are not modified. Cosmetic de-hardcoding of plots/tests is handled in Phase 1/4, not as a physics migration.

---

## 9. Validation and Fast Failure

Validation must fail before a long run when:

- a simulation package lacks `halo_properties.yaml` or `simulation_info.yaml`;
- a required core input has no `core_property_map` entry, or a mapped `source` names a catalog field the selected tree reader cannot provide;
- a catalog field or output property uses an unknown unit label or lacks a needed h convention;
- an output label differs from the reference unit but has no conversion (generation-time assertion);
- `parameter_units.yaml` references a parameter that is not declared in the run YAML or used by any module, or requests conversion with an unsupported unit;
- a range/sentinel cannot be converted, or an output conversion is ambiguous.

These checks live in `make generate`, `make validate-modules`, and startup validation as appropriate.

---

## 10. Present Defects to Fix (verified)

These exist today and are addressed in Phase 1, independent of the engine:

- **Hard-coded code-unit constant in the scientific test.** `tests/scientific/test_scientific.py:434` sets `G_CODE = 43.00710968931344` (and `C_LIGHT` at `:437`), valid only for Millennium units; the virial-relation check at `:449,467` would assert a wrong constant under other units. Compute `G` from the active reference units.
- **Inconsistent mass conversion in a plot.** `models/sage16/plots/figures/mass_reservoir_scatter.py:85-92` applies bare `* 1.0e10` (no `/ hubble_h`) while loading `hubble_h` at `:64` and ignoring it; every other figure divides by `hubble_h`. The masses in this plot are in a different unit than the rest, unflagged.
- **Silent cosmology fallback.** `sfr_density_evolution.py` and `stellar_mass_density_evolution.py` default `hubble_h` to `0.73`/`0.7` if absent from metadata, silently assuming Millennium.
- **Unused schema reader / duplicated conversions.** `plot/mimic-plot/output_schema.py:94` defines `units_from_schema()` but no figure or test calls it; 18 figures under `models/sage16/plots/figures/` hard-code `* 1.0e10` mass conversions. Centralize via a shared helper that reads the schema label.

---

## 11. Affected Code Areas

- `src/core/read_parameter_file.c` — stop parsing the `simulation.units` cgs block; read `particle_mass`/`box_size` with their unit labels.
- `src/core/init.c` (`set_units`) — derive constants from the fixed reference basis.
- `src/core/virial.c` — virial helpers in reference units; convert `particle_mass`; produce derived core fields.
- `src/core/core_properties.yaml` — restructured into `reference_units` + required-input contract + core-produced fields (Appendix A.1).
- `src/include/types.h`, `src/io/tree/binary.c`, `src/io/tree/hdf5.c` — catalog field access driven by the simulation mapping; topology links brought into metadata.
- `scripts/generate_properties.py` — contract merge, mapping-driven copy-in, unit registry, conversion-factor generation, metadata-driven output, label/value assertion.
- `src/include/generated/property_defs.h`, `src/include/generated/populate_halo_payload_from_tree.inc` — generated from the contract + mapping.
- `src/io/output/util.c`, `src/io/output/hdf5.c`, `src/io/output/metadata_hdf5.c` — generated output values/metadata.
- `plot/mimic-plot/output_schema.py` and `models/*/plots/**` — schema-aware reading; remove hard-coded conversions.
- `src/core/module_registry.c` — converted parameter accessor.
- `scripts/discovery.py`, `scripts/validate_modules.py`, `scripts/lint_parameter_usage.py` — `parameter_units.yaml` discovery/validation.
- `simulations/*/simulation_info.yaml` — remove `units` cgs block; add labels to `particle_mass`/`box_size`.
- `simulations/*/halo_properties.yaml` — `core_property_map` + full catalog field list with per-field labels (Appendix A.2).
- `models/*/model_properties.yaml` — model output labels; `models/*/parameter_units.yaml` — opt-in dimensional parameter units (Appendix A.3).
- `tests/scientific/test_scientific.py` and baseline/unit/integration tests — compute unit-dependent constants from reference units; protect Millennium parity.

---

## 12. Alignment with the Vision

- **Physics-agnostic core:** unit conversion lives at the I/O boundary and in generated code; the core owns the reference basis and validation without depending on a model, and no longer hard-codes simulation-specific unit labels.
- **Runtime modularity:** model/simulation combinations stay selectable; unit compatibility becomes explicit and validated.
- **Metadata as structural truth:** the reference basis, core contract, simulation mapping, output labels, and opt-in parameter units drive catalog mapping, conversion, output labels, defaults, ranges, and validation.
- **One coherent processing model:** all internal processing uses the single reference unit system.
- **Format-agnostic, reproducible I/O:** output values match their labels by construction; readers can trust schema metadata; Millennium stays byte-identical.
- **Validation and fast failure:** missing mappings, unknown fields/labels, ambiguous conversions, and stale parameter metadata fail before a long run.

---

## 13. Open Questions and Items Flagged for Review

To confirm with Uchuu's real numbers:

- **Uchuu's catalog conventions:** confirm Uchuu's mass scale, length unit, and h convention (carried vs. free), and its catalog field names, to fill in `core_property_map` and the catalog field labels.
- **Reference-basis precision:** confirm Uchuu's value ranges (very high-mass halos, fine mass resolution) stay within `float` after conversion into reference units. If any field's dynamic range is uncomfortable, widen that specific field to `double` — do not abandon the reference basis. Precision is a per-field type decision, not a reason to make internal units track the simulation.
- **Tree format/reader:** confirm whether Uchuu trees use an existing reader (LHaloTree binary/HDF5) or need a new format reader; the field mapping must name fields the selected reader can provide.

Flagged for review during planning:

- **Can the copied property YAMLs be dropped from the run metadata directory?** Each run currently snapshots `core_properties.yaml`, `model_properties.yaml`, and the simulation `halo_properties.yaml` into `<output>/metadata/` for provenance (`src/core/main.c:write_run_metadata`), alongside the generated `output_schema.json`. Under this design the output's field-level self-description (name, units, description) lives entirely in `output_schema.json` (and the HDF5 `FieldMetadata`/`RunProperties`), and these YAML files become generation-time inputs whose output-relevant content the schema already captures. Their role is therefore changing. During planning, review whether copying them is still needed for provenance or is now redundant — and if redundant, omit them from the output. (A single combined output-metadata file was considered and set aside in favor of this review.)

---

## 14. Decision

Adopt a single fixed internal reference unit system equal to today's effective code units, declared once in core, and convert each simulation's catalog into reference units at the tree-reader boundary (never by transcoding the dataset). Keep all core and model physics in reference units unchanged. Split metadata so core owns the required-input contract plus the fields it produces (by dimension, not simulation unit), and the simulation package owns its full on-disk catalog description plus the compulsory mapping; remove the redundant global `simulation.units` cgs block. Drive output conversion and labels from metadata so the written value always matches its label. Make dimensional-parameter conversion opt-in via a model-global `parameter_units.yaml`. Treat the value of little-h as a per-simulation cosmological parameter and the catalog's h convention as a boundary-conversion concern. Preserve Millennium byte-identical output as the regression guardrail throughout. Keep the supported unit set small and fail loudly when metadata is insufficient.

---

## Appendix A: Example Metadata Files

Illustrative and abbreviated (entries trimmed with `...`); they show structure and new content, not the complete field set.

### A.1 `src/core/core_properties.yaml` (core machinery — never edited per simulation)

```yaml
# Fixed internal reference unit system. set_units() derives G, Hubble, RhoCrit,
# etc. from this. Declared once; identical for every simulation.
reference_units:
  mass:     { label: "1e10 Msun", in_g: 1.989e43,     h_convention: carried }
  length:   { label: "Mpc",       in_cm: 3.08568e24,  h_convention: carried }
  velocity: { label: "km/s",      in_cm_per_s: 1.0e5, h_convention: none }

# Canonical fields core REQUIRES the simulation to provide (by role, no units,
# no catalog names). Satisfied by core_property_map in halo_properties.yaml.
required_inputs:
  - { name: Descendant,          role: tree_link }
  - { name: FirstProgenitor,     role: tree_link }
  - { name: NextProgenitor,      role: tree_link }
  - { name: FirstHaloInFOFgroup, role: tree_link }
  - { name: NextHaloInFOFgroup,  role: tree_link }
  - { name: SnapNum,             role: index }
  - { name: Len,                 role: count }
  - { name: VirialMassInput,     role: mass }   # catalog spherical-overdensity mass; feeds get_virial_mass

# Core-produced fields (computed/stamped by Mimic, never read from disk).
core_produced:
  internal:            # working state, never output, never in metadata
    - { name: CentralHalo, type: int }
    - { name: HaloNr,      type: int }

  identifiers:         # output, simulation- AND model-independent, dimensionless
    - { name: Type,                  type: int,       description: "0=central,1=satellite,2=orphan" }
    - { name: UniqueGalaxyID,        type: long long }
    - { name: UniqueCentralGalaxyID, type: long long }

  derived:             # output; dimension only — NO sim-specific unit label.
                       # output label defaults to reference unit; sim may override.
    - { name: Mvir,        type: float,  dimension: mass,     computed_by: get_virial_mass }
    - { name: Rvir,        type: float,  dimension: length,   computed_by: get_virial_radius }
    - { name: Vvir,        type: float,  dimension: velocity, computed_by: get_virial_velocity }
    - { name: dT,          type: double, dimension: time }
    - { name: deltaMvir,   type: float,  dimension: mass }
    - { name: CentralMvir, type: float,  dimension: mass }
    - { name: infallMvir,  type: float,  dimension: mass }
    - { name: infallVvir,  type: float,  dimension: velocity }
    # ... infallVmax ...
```

### A.2 `simulations/mini-millennium/halo_properties.yaml` (per simulation — user-owned)

```yaml
# Compulsory mapping: satisfy each core required-input role with a catalog column
# (each value names a field defined in halo_properties below).
core_property_map:
  Descendant:          Descendant
  FirstProgenitor:     FirstProgenitor
  NextProgenitor:      NextProgenitor
  FirstHaloInFOFgroup: FirstHaloInFOFgroup
  NextHaloInFOFgroup:  NextHaloInFOFgroup
  SnapNum:             SnapNum
  Len:                 Len
  VirialMassInput:     M_Crit200      # Uchuu would name a different column here

# Every catalog field as represented on disk. `units` is the on-disk label and
# `h_convention` whether little-h is carried; the reader generates catalog->reference
# conversion from these. `output` (and any transform) controls how it is written.
halo_properties:
  - { name: SnapNum,     source: SnapNum,     type: int,        units: dimensionless, output: true }
  - { name: Len,         source: Len,         type: int,        units: particles,     output: true }
  - { name: M_Crit200,   source: M_Crit200,   type: float,      units: "1e10 Msun", h_convention: carried, output: false }
  - { name: MostBoundID, source: MostBoundID, type: long long,  units: dimensionless, output: true }
  - name: Pos
    source: Pos
    type: vec3_float
    units: "Mpc"               # written label resolves to "Mpc/h" (h_convention: carried)
    h_convention: carried
    output: true
  - { name: Vmax,    source: Vmax,    type: float, units: "km/s", output: true }
  - { name: VelDisp, source: VelDisp, type: float, units: "km/s", output: true }
  # ... Vel (vec3, km/s), Spin (vec3, dimensionless) ...
```

### A.3 `models/sham/parameter_units.yaml` (model — opt-in; absent/empty ⇒ all raw)

```yaml
# List ONLY dimensional parameters that Mimic must convert to reference units.
# All others stay raw via model_get_double(), exactly as today.
parameters:
  - name: ShamMinMpeak
    type: double
    units: "1e10 Msun"
    h_convention: carried
    convert: true

# A model with no dimensional parameters ships:
#   parameters: []
```

### A.4 `simulations/mini-millennium/simulation_info.yaml` (units block removed)

```yaml
simulation:
  cosmology:
    omega_matter: 0.25
    omega_lambda: 0.75
    hubble_h: 0.73                                     # value of little-h (cosmology, not a unit)
  box_size:      { value: 62.5,      units: "Mpc",      h_convention: carried }
  particle_mass: { value: 0.0860657, units: "1e10 Msun", h_convention: carried }
  # REMOVED: the former units: { length_in_cm, mass_in_g, velocity_in_cm_per_s }
  # block. Reference units live in core; per-field labels + the unit registry
  # define every catalog scale.
```
