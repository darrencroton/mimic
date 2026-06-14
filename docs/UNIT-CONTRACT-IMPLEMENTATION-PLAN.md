# Mimic Unit Contract Implementation Plan

## Summary

Implement the full approved unit contract, with no Uchuu package added yet. Preserve Millennium-family numeric behavior by making current conversions identity under the new reference system.

Use the report's phase boundaries as implementation and verification checkpoints, even though the work ships as one full contract branch.

## Implementation Status

Status: implemented in the working tree on 2026-06-14.

The implementation follows this plan with one practical clarification: the fixed reference labels are written with their carried-h convention visible (`1e10 Msun/h`, `Mpc/h`, `km/s`) so output labels, schema metadata, and stored values stay directly consistent.

## Key Implementation Steps

1. Save this plan to `docs/` first, as the implementation reference for this work.
2. Add core-owned `reference_units` metadata and generate a C reference-units header used by `set_units()`.
3. Update `set_units()` to derive runtime constants only from generated reference units, not `simulation.units`.
4. Replace `simulation.units` with explicit `box_size` and `particle_mass` `{value, units, h_convention}` metadata; convert `PartMass` to reference units before physics use.
5. Add `core_property_map`, explicit catalog fields, `units`, `h_convention`, and reader source metadata to both Millennium simulation packages.
6. Add a small generator-side unit registry for supported labels only; unknown labels or missing h conventions fail generation.
7. Introduce a generated raw-catalog accessor/conversion layer used by payload copy-in, `get_virial_mass()`, `CentralMvir` stamping, and any other direct `InputTreeHalos` physical-value consumer.
8. Keep `RawHalo` static for LHaloTree, but distinguish source semantics: binary source resolves to a `RawHalo` member/role; HDF5 source resolves to a dataset name copied into a raw member.
9. Generate metadata-driven output conversion for linear unit changes, while retaining manual `output_convert` and `output_transform` for custom/nonlinear fields.
10. Define derived time reference semantics explicitly so `dT` and future time fields have valid label/value assertions.
11. Add opt-in model-global `parameter_units.yaml`, converted double accessor, and migrate `ShamMinMpeak`; leave SHAM's internal SHMR mass conversions explicitly out of this parameter-unit migration.
12. Stop copying property YAMLs into run metadata; keep schema, run config, simulation config, snapshot list, version metadata, and Python example. Document that full provenance now relies on `output_schema.json` plus version metadata/git state.

## Plotting, Docs, And Tests

- Add shared plotting/test unit helpers based on `metadata/output_schema.json`.
- Replace hard-coded mass/length conversions in SAGE figures with schema-aware helpers where practical.
- Fix report defects: scientific virial test computes `G` from reference/schema data; `mass_reservoir_scatter.py` uses consistent mass conversion; SFR and stellar-mass-density evolution plots require `hubble_h`, with no silent Millennium fallback.
- Update user/developer/plotting docs for fixed reference units, per-field catalog units, output schema trust, removed `simulation.units`, and schema-only metadata provenance.
- Include a non-identity unit-conversion fixture or generator/unit test so catalog-to-reference conversion is tested without waiting for Uchuu.

## Verification

- Completed package checks:
  - `make MODEL=sage16 SIMULATION=mini-millennium generate check-generated validate-modules`
  - `make MODEL=sage16 SIMULATION=millennium generate check-generated validate-modules`
  - `make MODEL=sham SIMULATION=mini-millennium generate check-generated validate-modules`
  - `make MODEL=sage16 SIMULATION=mini-millennium generate check-generated validate-modules` to restore default generated files.
- Completed final checks:
  - `make`
  - `make check-format`
  - `make check-docs`
  - `mimic_venv/bin/python tests/integration/test_unit_contract_generation.py`
  - `make tests-scientific summary` (passed with the existing zero-value warning marker)
  - Delegated `make tests-unit` with log capture to `archive/test-logs/tests-unit-unit-contract.log` (exit code 0; one expected skip marker)
  - Delegated `make tests-integration` with log capture to `archive/test-logs/tests-integration-unit-contract.log` (exit code 0; no failing markers)

Millennium numeric parity remains protected by identity conversion factors for the bundled Millennium-family metadata. The intentionally changed metadata/provenance behavior is that property YAML snapshots are no longer copied into run metadata; `output_schema.json`, HDF5 field metadata, run/simulation config copies, and version metadata are the output contract.

## Post-implementation review refinements

A code-review and simplification pass (2026-06-15) applied the following vision-aligned refinements on top of the initial implementation. All package pairs still pass `generate`/`check-generated`/`validate-modules`, Millennium output stays byte-identical, and the fast suites pass.

- **Single source of truth for the unit registry.** The generator now emits `src/include/generated/unit_registry.h` from its Python `UNIT_REGISTRY`, and `read_parameter_file.c` uses `mimic_unit_label_cgs()`/`mimic_unit_label_carried()` instead of a hand-maintained cgs table. `reference_units_from_core()` asserts each base dimension's declared `in_cgs` matches the registry, so the two cannot drift.
- **Honest, derived time reference.** The reference time unit is computed (`length/velocity`) and reported in `output_schema.json` as `{label: "code", in_cgs: <true value>}` rather than the misleading `Myr/h`. Metadata-driven linear time conversion is never generated (time output fields are verbatim, e.g. `ShamOrphanAge` in Myr/h, or carry an explicit `output_convert`, e.g. `dT`); `_linear_conversion_expr` rejects any time conversion as a safeguard.
- **Schema-trusting tests.** The scientific virial test reads each reference unit's `in_cgs` straight from the schema; the generator test covers non-identity mass, h-free length, identity, velocity, and the time fail-loud path.
- **Dead-code and duplication removal.** Removed unused `Unit*_in_*` assignments from the plot driver and test harness and the unused `length_to_mpc`/`reference_units_from_schema` plot helpers; collapsed redundant casts in the generated tree accessors; and shared the parameter-units loader between the generator and `validate_modules.py`.

## Assumptions

- No Uchuu files are created until exact catalog fields, units, h convention, and reader format are confirmed.
- LHaloTree binary keeps its current positional struct read in this implementation.
- SHAM galaxy-property unit cleanup is separate from the parameter-unit accessor work.
