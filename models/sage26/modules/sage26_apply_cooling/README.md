# `sage26_apply_cooling`

Commits the AGN-suppressed cooling mass: runs the tail of SAGE26's hot cooling routine, which records the cooling energy and rate, then moves the mass from the hot to the cold reservoir.

## SAGE26 Source

The last of the three modules SAGE26's `cooling_recipe_hot` is split across (see `sage26_calculate_cooling_budget`). `CoolingGas` holds the mass the routine returns, after `sage26_radio_mode_heating`'s suppression.

1. **The routine's tail** (`src/model_cooling_heating.c:301-313`). `Cooling` gains `0.5 * coolingGas * Vvir^2`, undivided and only for a positive mass; `sage26_finalize_cooling_heating` normalises it once per snapshot (`src/core_build_model.c:590-603`). `CoolingRate` becomes the float `(coolingGas / dt) * 1e10 / Hubble_h * SEC_PER_GIGAYEAR / UnitTime_in_s`, the true Msun/Gyr rate over little h. The tail runs on the normal path and after an outer-guard failure (with zero mass, so `CoolingRate` becomes 0); the `lambda <= 0` and `rho_rcool <= 0` early returns skip it, leaving both fields as they were (`CoolingReturnPath` 2 or 3).
2. **`cool_gas_onto_galaxy`** (`:689-705`, called at `src/core_build_model.c:508-509`; the shared `mimic_sage_cool_gas_onto_galaxy`, `shared/sage_cooling_physics.h`, the `CGMrecipeOn 0` transfer). Below the hot reservoir the mass moves with the hot gas's metallicity; at or above it the whole hot reservoir, metals included, moves.

The tail reads only the mass, `Vvir` and `dt`, none of which the transfer changes, so running it here gives SAGE26's values; this module is the first that always holds the final mass. `Vvir` is read as the float SAGE26 holds.

## Processing Contract

- Supported mode: `process_by_galaxy`
- Expected phase: `galaxy_physics`, after `sage26_calculate_cooling_budget` and `sage26_radio_mode_heating`
- Receives one galaxy at a time; every non-merged galaxy cools, Type 2 orphans included (`src/core_build_model.c:473-516`)
- Without a positive interval (the core's snapshot-zero birth) cooling did not run, and the module changes nothing

## Ordering

**Enforced at init (fails with ERROR if violated):**

1. `sage26_calculate_cooling_budget` must precede this module in the same substep phase; `CoolingGas` and `CoolingReturnPath` come from it.

## Properties

- Reads: `Type`, `HaloNr`, `SnapNum`, `dT`, `Vvir`, `HotGas`, `MetalsHotGas`, `CoolingGas`, `CoolingReturnPath`
- Writes: `HotGas`, `MetalsHotGas`, `ColdGas`, `MetalsColdGas`, `Cooling`, `CoolingRate`

## Parameters

None. `Hubble_h` and `UnitTime_in_s` are core-owned.

## Tests

`_tests/test_unit_sage26_apply_cooling_oracle.c` runs every standalone `cool_gas_onto_galaxy` fixture under `_tests/oracle/` (listed in `../_tests/oracle/fixtures.manifest`) through `process()` and asserts bit-for-bit equality with the SAGE26 function-level oracle (`../_tests/oracle/README.md`). The tail's `Cooling` and `CoolingRate` are asserted by `sage26_calculate_cooling_budget`'s chained oracle test. Outside the oracle environment (macOS arm64, Apple clang 21) the bit-exact tests report SKIP.
