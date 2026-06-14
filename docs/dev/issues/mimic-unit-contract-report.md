# Mimic Unit Contract — Implementation Specification

Version: 2.0
Date: 2026-06-14
Status: Approved design, ready for implementation

---

## 1. Executive Summary

Mimic needs an explicit, enforced unit contract before additional simulations and models are added. The immediate forcing function is the **Uchuu** simulation, which defines mass and other halo properties with a different unit scale and different catalog field names than the bundled Millennium catalogs, and which will be paired with new models that assume their own units. Uchuu also makes performance a first-class concern: the full merger-tree dataset is ~37 TB, and this scale will become normal as Mimic grows.

The adopted design is:

- **One fixed internal reference unit system** for all of Mimic, equal to today's effective code units: mass in `1e10 Msun`, length in `Mpc`, velocity in `km/s`, with little-h carried numerically in values (the Millennium/`Msun/h` convention). Core physics constants (`G`, `Hubble`, `RhoCrit`, etc.) are derived from this fixed basis and never vary by simulation.
- **Conversion at the tree-reader boundary.** Each simulation declares its native catalog units and its catalog→canonical field-name mapping. Generated copy-in code converts each catalog value into reference units *as it is read*, folded into the field copy that already happens. Mimic never transcodes the dataset to disk.
- **All core and model physics runs in reference units, unchanged.** Because the internal basis is fixed, every formula sees the same constants and the same-unit inputs for every simulation. There is no per-model "make physics unit-agnostic" audit. Millennium catalogs are an identity conversion, so existing output stays byte-identical to the upstream `sage-model` baseline by construction.
- **Metadata-driven output.** Every output property declares its output unit label; generated code converts reference→label (identity when equal). The written value is in the labeled unit by construction, closing today's gap where the label and the value are produced by independent, unchecked code paths.
- **Opt-in dimensional parameters.** A model parameter that must be compared against internal physical values declares optional `units`/`h` metadata on its existing `module_info.yaml` declaration; all other parameters stay raw exactly as today.

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
- Property `units:` is required and written verbatim to HDF5 `FieldMetadata` and the binary `output_schema.json`, but it is **just a label**. The value path (`output_convert` expressions in `scripts/generate_properties.py:638-661,699-723`) and the label path (`generate_properties.py` HDF5 writer ~`812-901`, binary schema writer ~`990-1045`) never cross-check each other.
- Model parameters are read raw via `model_get_double/int/string()` (`src/core/module_registry.c`); there is no parameter-unit metadata.

The result: a new simulation or model can compile and run while silently mixing incompatible units, because nothing converts catalog values into a known basis and nothing enforces that an output label matches the value written.

### 2.3 Why units, specifically

The vision states Mimic should make hidden assumptions and silent configuration errors harder to introduce. Units are the highest-risk hidden assumption in this architecture: a unit error preserves compilation and often looks plausible in a plot (a factor of little-h ≈ 0.7 is easy to miss), while corrupting the science. The contract must live in metadata, generated code, and validation rather than in scattered comments and copy-pasted conversions.

---

## 3. Key Concept: Two Unit Systems and Two Meanings of "h"

Implementers must keep four distinct quantities separate. Conflating any pair is the source of the bugs this contract prevents.

### 3.1 Reference units vs. catalog units

- **Reference units (fixed, internal):** the one basis all of Mimic computes in — `mass = 1e10 Msun`, `length = Mpc`, `velocity = km/s`, time derived. This is core-owned and identical for every simulation. All physics constants and all module formulas operate here.
- **Catalog units (per-simulation, native):** the units a given simulation's tree files actually use on disk. Declared by the simulation package. Used **only** by the generator/reader to compute the catalog→reference conversion factors.

For Millennium catalogs, catalog units already equal reference units, so every conversion factor is exactly `1.0` and behavior is byte-identical to today.

### 3.2 The value of little-h vs. the h convention

- **The value of little-h (`hubble_h`, e.g. 0.73 for Millennium, 0.6774 for Uchuu/Planck):** a *cosmological parameter*, declared per simulation in `simulation.cosmology`, used inside physics. It is **not** a unit and is **not** touched by the unit contract. Formulas that use `MimicConfig.Hubble_h` simply pick up the correct per-simulation value, which is already correct.
- **The h convention of the catalog (carried vs. free):** whether catalog values embed little-h (e.g. mass in `Msun/h`) or are h-free (e.g. mass in physical `Msun`). This *is* a unit concern. The reference convention is **h-carried** (the Millennium convention). If a simulation's catalog is h-free, the reader applies the appropriate factor of little-h at the boundary to bring values into the h-carried reference convention. This is the only place the contract manipulates h.

This separation is why "make SAGE simulation-agnostic" requires no formula audit: `set_units()` derives constants from the fixed reference basis, so every formula sees identical constants and reference-unit inputs for every simulation; the per-simulation differences (catalog scale, catalog field names, catalog h convention, and the cosmological value of `hubble_h`) are all absorbed before physics runs or are already physical inputs.

---

## 4. The Unit Contract

### 4.1 Reference unit system (core-owned)

The core declares the fixed reference basis as metadata so it is structural truth, not a magic constant. Recommended location: a `reference_units:` block in core-owned metadata (e.g. the header of `src/core/core_properties.yaml` or a dedicated `src/core/reference_units.yaml`).

```yaml
reference_units:
  mass:     { label: "1e10 Msun", in_g: 1.989e43,     h_convention: carried }
  length:   { label: "Mpc",       in_cm: 3.08568e24,  h_convention: carried }
  velocity: { label: "km/s",      in_cm_per_s: 1.0e5, h_convention: none }
  # time is derived from length/velocity, as today
```

`set_units()` must derive `G`, `Hubble`, `RhoCrit`, `UnitTime_in_s`, `UnitDensity_in_cgs`, `UnitPressure_in_cgs`, `UnitEnergy_in_cgs`, and `UnitCoolingRate_in_cgs` from this fixed reference basis — **not** from the selected simulation's catalog units. For Millennium this yields numerically identical constants to today (the reference values equal Millennium's `simulation.units`), preserving the byte-identical baseline.

### 4.2 Core property contract (core-owned)

`src/core/core_properties.yaml` defines Mimic's canonical required halo/tree properties by **semantic role**, independent of any catalog's field names. For each canonical field it declares: name, C type/shape, role (tree-topology link, structural identifier, direct halo property, or derived core property), which core lifecycle stage creates/updates it, and whether it is required for processing, output, or both. It must **not** hard-code catalog field names or catalog-specific unit scales.

Derived core properties (`Mvir`, `Rvir`, `Vvir`, `dT`) are computed by core helpers (`src/core/virial.c`) in reference units; their *output labels* are declared as in §4.4.

### 4.3 Simulation contract (per-simulation, mandatory)

`simulations/<simulation>/halo_properties.yaml` is the single source of truth for how a simulation satisfies the core contract. It is mandatory for every simulation package and contains:

1. A **field-name mapping** from canonical core fields to catalog source fields, with each mapped field's catalog units and h convention.
2. Metadata for simulation-owned **optional** halo/catalog fields the model may use or output (e.g. `Pos`, `Vel`, `Spin`, `Vmax`, `VelDisp`), with catalog source, units, and h convention.

The native catalog cgs scale lives in `simulations/<simulation>/simulation_info.yaml` under `simulation.units` (its existing location), now interpreted strictly as the *catalog's native* scale used to compute conversion factors. Recommended mapping structure:

```yaml
# simulations/<simulation>/halo_properties.yaml
core_property_map:
  # canonical_core_field: { source: <catalog field>, units/h for conversion }
  Descendant:          { source: Descendant }            # topology link, no units
  FirstProgenitor:     { source: FirstProgenitor }
  FirstHaloInFOFgroup: { source: FirstHaloInFOFgroup }
  SnapNum:             { source: SnapNum }
  Len:                 { source: Len, units: particles }
  MostBoundID:         { source: MostBoundID }
  Mvir:                { source: M_Crit200, units: "1e10 Msun", h_convention: carried }

halo_properties:        # optional, simulation-owned, output/model-facing
  - name: Pos
    source: Pos
    type: vec3_float
    units: "Mpc"
    h_convention: carried
    output: true
    init_source: copy_from_tree_array
```

The generator validates that the map satisfies every required core field before build/startup. The conversion factor for each mapped/optional field is computed from `(catalog unit in cgs) / (reference unit in cgs)` for its dimension, with an additional factor of little-h if the catalog and reference h conventions differ.

### 4.4 Output labels (model- and simulation-owned)

Every output property declares the unit label it should be written in. The human-facing label is a free-form string (e.g. `1e10 Msun/h`, `Mpc/h`, `Msun`) — including the `/h` suffix when the value is h-carried — and is written verbatim to HDF5 `FieldMetadata` and the binary schema. The conversion math, however, is driven by the structured scale + `h_convention` fields, not by parsing that string, so the label and the value cannot silently diverge. Generated output code converts reference→label using those structured fields. When the label equals the reference unit, the conversion is identity and the value is written verbatim (preserving byte-identical Millennium output). Sentinels are preserved and never scaled unless explicitly declared physical. The existing `output_convert`/`output_transform` mechanism (used today for SFR, `log10(erg/s)` cooling/heating, Gyr times, and `dT`) is retained for genuinely custom or non-linear cases; metadata-driven conversion replaces only the linear unit conversions.

### 4.5 Model properties (model-owned)

`models/<model>/model_properties.yaml` declares model-owned properties in model-facing/output units (the `units:`, `init_value:`, `range:`, `sentinels:`, output label). Where a model property has a physical dimension and a non-trivial `init_value`/`range`, generated code converts those into reference units for storage/processing and converts back for output. Properties whose `init_value` is `0.0`/sentinel (the common case in SAGE) need no input conversion.

### 4.6 Parameters (model-owned, opt-in)

Model parameters stay raw by default and continue to flow through `model_get_double/int/string()`. A parameter that Mimic must compare against internal physical values declares optional `units`/`h` metadata **on its existing declaration in `module_info.yaml`** — no separate `parameter_units.yaml` file. A generated/shared accessor (`model_get_double_internal()` or equivalent) exposes the converted value; the raw accessors keep current behavior. Today the only dimensional parameter in the repo is SHAM's `ShamMinMpeak` (a mass threshold, currently converted inline as `mpeak * 1.0e10 / h` in `sham_assign_stellar_mass.c`); it is the first and currently only consumer.

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
- No `h_power`/`dimension` field sprinkled onto every internal property. The h convention is declared only where conversion happens (catalog mapping and output labels). Under the byte-identical constraint, internal values stay h-carried, so a universal internal `h_power` could only ever multiply by 1.
- No separate `parameter_units.yaml` (folded into `module_info.yaml`, §4.6).
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

- Introduce core-owned reference-unit metadata (§4.1).
- Change `set_units()` (`src/core/init.c:77`) to derive constants from the reference basis. Keep `simulation.units` parsing in `read_parameter_file.c`, but its values now feed only the conversion-factor generator, not `set_units()`.
- Convert `PartMass` (`simulation.particle_mass`) from catalog units into reference units before use in `get_virial_mass()` (`src/core/virial.c`). Identity for Millennium.

### 7.2 Core/simulation contract split and field-name mapping

- `src/core/core_properties.yaml`: reduce to canonical required fields + derived core fields by semantic role; remove catalog-specific unit scales/ranges that belong to simulations.
- `simulations/<simulation>/halo_properties.yaml`: add the mandatory `core_property_map` plus optional simulation-owned fields (§4.3).
- `scripts/generate_properties.py`: load the core contract + simulation map; validate completeness; generate the canonical-field copy-in from `source` mappings; merge optional simulation-owned fields into the halo/output schema.
- `src/include/generated/populate_halo_payload_from_tree.inc`: now generated from the mapping rather than assuming name matches.
- `src/include/types.h` / tree readers (`src/io/tree/binary.c`, `src/io/tree/hdf5.c`): the `RawHalo` layout and `READ_TREE_PROPERTY` calls become driven by the simulation's declared catalog fields. The first implementation may keep the existing `RawHalo` shape for LHaloTree inputs while making the mapping the source of truth.

### 7.3 Small unit registry and conversion-factor generation

- Add a small explicit registry in the generator covering only the labels Mimic uses: mass (`Msun`, `1e10 Msun`), length (`cm`, `kpc`, `Mpc`), velocity (`cm/s`, `km/s`), time (`s`, `yr`, `Myr`, `Gyr`), energy/luminosity (`erg`, `erg/s`), the structural labels (`index`, `identifier`, `count`, `particles`), and the special labels (`dimensionless`, `dex`, `Internal`, `log10(...)`). Unknown labels fail validation.
- For each dimensioned field, generate the catalog→reference factor (for input) and reference→label factor (for output), applying a factor of little-h only when h conventions differ.

### 7.4 Metadata-driven output

- `scripts/generate_properties.py`: generate reference→label conversion for output properties from metadata; emit identity when label equals reference; retain manual `output_convert`/`output_transform` for custom/non-linear cases; preserve sentinels.
- Make the HDF5 `FieldMetadata` and binary schema writers emit the label that matches the generated conversion (so value == label by construction).
- Add a **generation-time assertion**: any property whose output label differs from its reference unit must have a conversion (generated or manual); otherwise `make generate` fails.

### 7.5 Opt-in parameter units

- `models/*/modules/*/module_info.yaml`: allow optional `units`/`h` on parameter declarations.
- `scripts/discovery.py`, `scripts/validate_modules.py`, `scripts/lint_parameter_usage.py`: discover and validate parameter-unit metadata; cross-check against declared/used parameters.
- Add one converted accessor (e.g. `model_get_double_internal()`); keep raw accessors unchanged. Migrate only `ShamMinMpeak` initially.

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

- Implement §7.1 and §7.3. `set_units()` derives from the reference basis; the reader converts catalog→reference at copy-in; `PartMass` converted. Millennium = identity factors = byte-identical. This is what lets Uchuu's different unit scale and h convention work.

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
- a required core property has no simulation mapping, or a mapped `source` names a catalog field the selected tree reader cannot provide;
- a physical property/field uses an unknown unit label or lacks a needed h convention;
- an output label differs from the reference unit but has no conversion (generation-time assertion);
- a dimensional parameter requests conversion but lacks supported units, or `module_info.yaml` parameter-unit metadata references an unknown/unused parameter;
- a range/sentinel cannot be converted, or an output conversion is ambiguous.

These checks live in `make generate`, `make validate-modules`, and startup validation as appropriate.

---

## 10. Present Defects to Fix (verified)

These exist today and are addressed in Phase 1, independent of the engine:

- **Hard-coded code-unit constant in the scientific test.** `tests/scientific/test_scientific.py:434` sets `G_CODE = 43.00710968931344` (and `C_LIGHT` at `:437`), valid only for Millennium units; the virial-relation check at `:449,467` would assert a wrong constant under other units. Compute `G` from the active reference/simulation units.
- **Inconsistent mass conversion in a plot.** `models/sage16/plots/figures/mass_reservoir_scatter.py:85-92` applies bare `* 1.0e10` (no `/ hubble_h`) while loading `hubble_h` at `:64` and ignoring it; every other figure divides by `hubble_h`. The masses in this plot are in a different unit than the rest, unflagged.
- **Silent cosmology fallback.** `sfr_density_evolution.py` and `stellar_mass_density_evolution.py` default `hubble_h` to `0.73`/`0.7` if absent from metadata, silently assuming Millennium.
- **Unused schema reader / duplicated conversions.** `plot/mimic-plot/output_schema.py:94` defines `units_from_schema()` but no figure or test calls it; 18 figures under `models/sage16/plots/figures/` hard-code `* 1.0e10` mass conversions. Centralize via a shared helper that reads the schema label.

---

## 11. Affected Code Areas

- `src/core/read_parameter_file.c` — parses `simulation.units` (now catalog-native scale, feeds conversion generation only).
- `src/core/init.c` (`set_units`) — derive constants from the fixed reference basis.
- `src/core/virial.c` — virial helpers in reference units; convert `PartMass`.
- `src/core/core_properties.yaml` — canonical core contract; add `reference_units` (or a dedicated core units file).
- `src/include/types.h`, `src/io/tree/binary.c`, `src/io/tree/hdf5.c` — catalog field access driven by the simulation mapping.
- `scripts/generate_properties.py` — contract merge, mapping-driven copy-in, unit registry, conversion-factor generation, metadata-driven output, label/value assertion.
- `src/include/generated/property_defs.h`, `src/include/generated/populate_halo_payload_from_tree.inc` — generated from the contract + mapping.
- `src/io/output/util.c`, `src/io/output/hdf5.c`, `src/io/output/metadata_hdf5.c` — generated output values/metadata.
- `plot/mimic-plot/output_schema.py` and `models/*/plots/**` — schema-aware reading; remove hard-coded conversions.
- `src/core/module_registry.c` — converted parameter accessor.
- `scripts/discovery.py`, `scripts/validate_modules.py`, `scripts/lint_parameter_usage.py` — parameter-unit discovery/validation.
- `simulations/*/{simulation_info.yaml,halo_properties.yaml}` — catalog scale + mandatory `core_property_map` + optional fields.
- `models/*/model_properties.yaml`, `models/*/modules/*/module_info.yaml` — model output labels, optional parameter units.
- `tests/scientific/test_scientific.py` and baseline/unit/integration tests — compute unit-dependent constants from active units; protect Millennium parity.

---

## 12. Alignment with the Vision

- **Physics-agnostic core:** unit conversion lives at the I/O boundary and in generated code; the core owns the reference basis and validation without depending on a model.
- **Runtime modularity:** model/simulation combinations stay selectable; unit compatibility becomes explicit and validated.
- **Metadata as structural truth:** the reference basis, core contract, simulation mapping, output labels, and opt-in parameter units drive catalog mapping, conversion, output labels, defaults, ranges, and validation.
- **One coherent processing model:** all internal processing uses the single reference unit system.
- **Format-agnostic, reproducible I/O:** output values match their labels by construction; readers can trust schema metadata; Millennium stays byte-identical.
- **Validation and fast failure:** missing mappings, unknown fields/labels, ambiguous conversions, and stale parameter metadata fail before a long run.

---

## 13. Open Questions to Confirm with Uchuu's Real Numbers

- **Uchuu's catalog conventions:** confirm Uchuu's mass scale, length unit, and h convention (carried vs. free), and its catalog field names, to fill in `simulation_info.yaml` and `core_property_map`.
- **Reference-basis precision:** confirm Uchuu's value ranges (very high-mass halos, fine mass resolution) stay within `float` after conversion into reference units. If any field's dynamic range is uncomfortable, widen that specific field to `double` — do not abandon the reference basis. Precision is a per-field type decision, not a reason to make internal units track the simulation.
- **Tree format/reader:** confirm whether Uchuu trees use an existing reader (LHaloTree binary/HDF5) or need a new format reader; the field mapping must name fields the selected reader can provide.

---

## 14. Decision

Adopt a single fixed internal reference unit system equal to today's effective code units, convert each simulation's catalog into reference units at the tree-reader boundary (never by transcoding the dataset), keep all core and model physics in reference units unchanged, drive output conversion and labels from metadata so the written value always matches its label, and make dimensional-parameter conversion opt-in on the existing module metadata. Treat the value of little-h as a per-simulation cosmological parameter and the catalog's h convention as a boundary-conversion concern. Preserve Millennium byte-identical output as the regression guardrail throughout. Keep the supported unit set small and fail loudly when metadata is insufficient.
