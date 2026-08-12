---
name: mimic-properties
description: Mimic's property system - the YAML metadata that generates all C structs, output schemas, and unit conversions. Load when a task involves core_properties.yaml, halo_properties.yaml, model_properties.yaml, parameter_units.yaml, adding/removing/renaming a galaxy or halo property, property fields (init_source, output_source, init_repeat, range, sentinels, h_convention, provides_core_role, output_convert), units and the reference basis, float vs double precision decisions, struct GalaxyData / RawHalo / HaloOutput, anything under src/include/generated/, "make generate", "make check-generated", generated-code drift, or transport/scratch properties between modules.
---

# Mimic Properties

Properties are declared in YAML and generated into C. The YAML is the single source of structural truth: struct fields, initialization, output records, HDF5 metadata, binary schemas, and unit conversions are all projections of it. You never edit generated files; you edit metadata and regenerate. This skill is the schema, the semantics, and the workflows.

## When to use / when NOT to use

Use for: any change to the four property/parameter metadata files, precision decisions, unit questions, generated-code questions, transport-field design.

Do NOT use for:
- How modules consume properties in C — see the `mimic-modules` skill.
- Simulation package structure and reader binding beyond the schema — see the `mimic-simulations-and-readers` skill.
- What a property means astrophysically — see the `mimic-sam-reference` skill.
- Whether the change is gated and what tests it needs — see `mimic-change-control` and `mimic-validation-and-qa`.

## First actions

1. Identify which file owns the property (table below) — ownership decides the blast radius and the bar.
2. Search source metadata and consumers before changing anything, excluding generated files unless you are specifically checking generator drift:

```bash
rg -n "<PropertyName>" src models simulations plot tests \
  --glob '*.{c,h,py,yaml}' \
  --glob '!src/**/generated/**' \
  --glob '!src/module_system/generated/**'
```
3. Read 2–3 neighboring entries in the target YAML and copy their key order and style.
4. After ANY edit: `make generate && make check-generated && make` (add `MODEL=<m> SIMULATION=<s>` uniformly for non-default pairs).

## 1. The three property files

| File | Owns | Generated into |
|---|---|---|
| `src/core/core_properties.yaml` | Minimum halo-tracking state every model needs, plus the `reference_units` block and the `required_inputs` roles | `struct Halo` core fields |
| `simulations/<SIM>/halo_properties.yaml` | The COMPLETE on-disk catalog record, **in on-disk order** | `struct RawHalo` (the binary record layout) + reader binding |
| `models/<MODEL>/model_properties.yaml` | Galaxy state the model's physics evolves (`galaxy_properties:` list) | `struct GalaxyData` |

One compiled set at a time (`MODEL=` + `SIMULATION=`). A property name must be unique across all three files; incompatible duplicates fail at generation. A name that is both an on-disk field and a core property (e.g. `SnapNum`, `Len`) is bound via `provides_core_role`, not duplicated.

The core `required_inputs` roles that a simulation package must bind with `provides_core_role`: `Descendant`, `FirstProgenitor`, `NextProgenitor`, `FirstHaloInFOFgroup`, `NextHaloInFOFgroup` (tree links), `SnapNum` (index), `Len` (count), `HaloMass` (mass). The generator emits `mimic_tree_get_<Role>()` accessors so core traversal never hard-codes catalog names.

## 2. The per-property schema (as the generator enforces it)

From `scripts/generate_properties.py`. Required on every entry: `name` (valid C identifier), `type`, `units`, `description`, `output` (bool).

| Field | Allowed values / semantics |
|---|---|
| `type` | `int`, `float`, `double`, `long long`, `vec3_float`, `vec3_int` |
| `init_source` | `default` (requires `init_value`), `copy_from_tree`, `copy_from_tree_array`, `calculate` (requires `init_function`), `skip` |
| `output_source` | `copy_direct`, `copy_direct_array`, `recalculate` (requires `output_function` + `output_function_arg`), `conditional` (requires `output_condition` + `output_true_value` + `output_false_value`), `custom`, `galaxy_property` |
| `output_transform` | `log10` only |
| `output_convert` | Raw C expression escape hatch (e.g. `MimicConfig.UnitMass_in_g / MimicConfig.UnitTime_in_s * SEC_PER_YEAR / SOLAR_MASS`) — bypasses registry conversion |
| `init_repeat` | Galaxy properties only; requires `init_source: default`; marks the field a per-snapshot accumulator (section 3) |
| `range: [min, max]` | Inclusive physical validation range; emitted to `tests/generated/property_ranges.json` only for `output: true` properties |
| `sentinels: [v, ...]` | Values exempt from range checks AND from output conversion/transforms (e.g. `-1.0` for "never set") |
| `h_convention` | `carried` (h folded in, e.g. Mpc/h), `free` (physical, e.g. Mpc), `none` (h-independent, e.g. km/s); defaults from the unit registry |
| `source` | Catalog entries only: on-disk dataset/column name when it differs from `name` |
| `provides_core_role` | Catalog entries only: binds the field to a core `required_inputs` role |
| `notes` | Free text, documentary only — the generator ignores it |

**There is no `transport:` key.** A transport/scratch field between modules is exactly: `output: false` + `init_repeat: true` + producer/consumer recorded in `notes`. That combination *is* the contract.

## 3. Init and reset semantics — the code truth

Two generated functions in `src/include/generated/property_defs.h`, with distinct jobs:

- `init_galaxy_defaults(galaxy)` — full defaults for a **new** galaxy (called from `init_new_halo` in `src/core/inheritance.c`). Every galaxy property gets its `init_value` (e.g. `MergTime = 999.9`, `HaloBaryonFraction = -1.0`).
- `reset_galaxy_snapshot_accumulators(galaxy)` — zeroes only the `init_repeat: true` fields. Called at `src/core/inheritance.c:27`, inside progenitor deep-copy — i.e. **once per snapshot interval at inheritance time, NOT once per substep**. (The Developer Guide's transport wording says "reset each substep"; the code is the truth. Currently 13 fields: InfallingGas, CoolingGas, NewStellarMass, StarFormationRate, QuasarModeBHaccretionMass, SupernovaReheatedMass, SupernovaEjectedMass, Cooling, Heating, Rcool, CoolingLambda, SupernovaOutflowRate, UnstableDiskGasFraction.)

Practical consequences: accumulators like `StarFormationRate` accumulate across all substeps within a snapshot interval and reset at the next snapshot's inheritance; a transport value persists across substeps unless its consumer zeroes it — which is why apply-modules explicitly zero what they commit (e.g. `sage_apply_cooling` zeroes `CoolingGas`; `sage_apply_metal_enrichment` consumes and zeroes `NewStellarMass`). When you add an `init_repeat` field, decide who zeroes it intra-snapshot, or document that carryover across substeps is intended.

## 4. Units and the reference basis

Mimic runs in one fixed internal basis declared under `reference_units` in `src/core/core_properties.yaml`: mass `1e10 Msun/h`, length `Mpc/h`, velocity `km/s`, time derived (length/velocity), all `carried` except velocity (`none`). Every incoming quantity declares `units` (+ optional `h_convention`) and is converted at the boundary by generated code (`src/include/generated/unit_registry.h` for catalog fields; `parameter_unit_conversions.h` + `*_INTERNAL` macros for parameters declared in `models/<MODEL>/parameter_units.yaml` — name/type[`double` only]/units/h_convention; sham has one, sage16 none; unlisted parameters are assumed already in reference units).

Registered unit labels (the ONLY valid `units:` values; generation fails loudly on unknown labels): `dimensionless`, `index`, `identifier`, `count`, `particles`, `Internal`, `dex`, `Msun`, `Msun/h`, `1e10 Msun`, `1e10 Msun/h`, `cm`, `kpc`, `kpc/h`, `Mpc`, `Mpc/h`, `cm/s`, `km/s`, `s`, `yr`, `Myr`, `Myr/h`, `Gyr`, `Gyr/h`, `erg`, `erg/s`, `log10(erg/s)`, `Msun/yr`, `erg cm^3/s`. To add a label, extend `UNIT_REGISTRY` in `scripts/generate_properties.py` with its dimension, cgs magnitude, and default h_convention. Conversion rule: linear cgs scale plus a `Hubble_h` factor when source and target are both h-dependent but differ between `carried` and `free`; `none` cannot convert to/from h-dependent conventions. Time-unit conversions are deliberately rejected (reference time is derived).

## 5. Precision policy (doctrine, with the history)

`type:` is a scientific decision. The settled policy (full stories in `mimic-failure-archaeology`):

1. **Core and simulation halo properties default to `double`.** They serve every model. The prior `float` choice masked a real bug: a float-rounded comparison in inheritance (`descendant->virial_mass > halo->Mvir`) could freeze an orphan's preserved Rvir/Vvir on the wrong branch (fixed by the 2026-07-01 widening, commit `bf0993fa`).
2. **Catalog fields match the SOURCE data's real precision.** Rockstar/Consistent-Trees ASCII carries ~7 significant figures; storing it in `double` adds nothing. This was investigated and settled as don't-widen (commit `4a97d3d0`) — check actual catalog values, not a reader's C variable type, before widening.
3. **Model accumulators default to `double` in new models.** sage16's reservoirs stay `float` ONLY for byte-parity with original SAGE — the YAML's own comments say which and why. Never inherit that choice into a new model package.
4. **Widening a stored field is incomplete until every local copy is chased.** After the core widening, `sage_reincorporation.c` still copied values into local `float`s, silently re-narrowing them; only the FULL test suite caught it (commit `6cbeafe4`). After any precision change: `grep` the modules for local copies of the field, then run the full suite.

## 6. What generation produces

`make generate` runs `scripts/generate_properties.py`, which writes into `src/include/generated/`: `property_defs.h` (structs + init/reset inline functions), `raw_halo_defs.h`, `tree_property_accessors.h`, `reference_units.h`, `unit_registry.h`, `parameter_unit_conversions.h`, `populate_halo_payload_from_tree.inc`, `read_tree_hdf5_properties.inc`, `copy_to_output.inc`, `output_schema_writer.inc` (the binary schema), `hdf5_field_count.inc`, `hdf5_field_definitions.inc`, `hdf5_field_metadata.inc`, `property_test_helpers.h`; plus `tests/generated/property_ranges.json`. Every file embeds a `Source MD5:` header; `make check-generated` recomputes and compares (that is the drift check — never diff generated files by eye).

Known quirk: `reset_galaxy_properties.inc` and `tests/generated/module_sources.mk` exist on disk but are **stale legacy artifacts no current generator writes** (init/reset logic moved into `property_defs.h` inline functions). Don't chase them as drift and don't hand-edit them. The Makefile's `GENERATED_HEADERS` **no longer names any of them**: `init_halo_properties.inc` and `init_galaxy_properties.inc` were verified orphaned on both ends (no generator writes them, no C source includes them), removed from `GENERATED_HEADERS`, and moved to `archive/orphaned-generated/` on 2026-08-13.

Generation is hash-gated: the Makefile re-runs the generator every build (stamp + FORCE), and the generator itself no-ops when the MD5 of generator+YAMLs matches `build/generated/property_hash.txt`.

## 7. Workflows

**Add a galaxy property** (model-owned; commonest case):

```bash
# 1. Add the entry to models/sage16/model_properties.yaml next to similar fields,
#    copying neighbor key order. Decide: output? init_value? init_repeat? range/sentinels?
make generate && make check-generated && make
# 2. Use the new field in module code (gal-><Name>); declare it in that module's
#    dependencies.properties; make validate-modules
# 3. Tests: initialization/reset/output behavior + physics use (mimic-validation-and-qa)
# Delegate this full-suite run unless you are the test subagent; unit/integration are long.
mkdir -p archive/test-logs && make tests summary > archive/test-logs/tests.log 2>&1
rc=$?; echo "exit_code=$rc"
```

**Add a catalog halo property** (simulation-owned): the `halo_properties:` list is the on-disk record — **position matters** (it defines `struct RawHalo`'s binary layout for L-Halo formats), and unused on-disk fields must still be listed to preserve layout. Declare `source:` if the on-disk name differs, `units` + `h_convention` for dimensioned fields, `provides_core_role` if it feeds a core role. Then `make MODEL=<model> SIMULATION=<sim> generate && make MODEL=<model> SIMULATION=<sim> check-generated && make MODEL=<model> SIMULATION=<sim>` (same selector everywhere). Reader-side detail: `mimic-simulations-and-readers`.

**Add or change a core property**: affects every model and every simulation — the highest bar. Default to `double`. Route through `mimic-change-control`; expect baseline impact.

**Choosing `range:` and `sentinels:`**: `range` is consumed by the metadata-driven scientific test (`tests/scientific/test_scientific.py` via `tests/generated/property_ranges.json`) — set it to the physically credible span, not the float span, and list legitimate out-of-band markers (never-set values, valid zeros) as `sentinels` so they are skipped by range checks and output transforms.

## 8. Downstream impact map

A property change can touch, in order of surprise: the struct layout (rebuild everything); the binary output schema (old outputs stay readable ONLY through their own run-local `metadata/output_schema.json` — never reinterpret old data with new metadata); HDF5 `FieldMetadata`; plots (model figure registries gate on field presence via `PLOT_REQUIREMENTS` — see `mimic-plots-and-analysis`); module dependency validation (`make validate-modules`); and baselines (any change to output fields or values forces a justified baseline regeneration — see `mimic-validation-and-qa`). Walk this list before declaring a property change done.

## Provenance and maintenance

Verified against the live repo 2026-07-04. Re-verify drift-prone specifics:

```bash
sed -n '100,121p' scripts/generate_properties.py                    # init/output/transform/h lists
python3 - <<'EOF'
import re; s=open('scripts/generate_properties.py').read()
print(re.findall(r'\n    "([^"]+)":', re.search(r'UNIT_REGISTRY.*?=\s*\{(.*?)\n\}', s, re.S).group(1)))
EOF
grep -n "reset_galaxy_snapshot_accumulators" src/core/inheritance.c  # reset call site (per snapshot)
sed -n '/void reset_galaxy_snapshot_accumulators/,/^}/p' src/include/generated/property_defs.h
sed -n '/^required_inputs/,/^halo_properties/p' src/core/core_properties.yaml   # core roles
ls src/include/generated/                                            # generated set incl. stale legacy reset_ file
grep -n "init_halo_properties\|init_galaxy_properties" Makefile      # expect no hits since 2026-08-13
```

The schema tables and unit-label list drift only with `scripts/generate_properties.py`; the reset-at-inheritance semantics is core architecture (re-check the call site if `src/core/inheritance.c` changes); the precision policy is doctrine anchored in verified history.
