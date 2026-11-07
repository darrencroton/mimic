# `sage26_apply_cooling`

Commits the AGN-suppressed cooling mass: runs the tail of SAGE26's hot cooling routine, which records the cooling energy and rate, then moves the mass from the hot to the cold reservoir.

## SAGE26 Source

The last of the three modules SAGE26's `cooling_recipe_hot` is split across (see `sage26_calculate_cooling_budget`). `CoolingGas` holds the mass the routine returns, after `sage26_radio_mode_heating`'s suppression.

1. **The routine's tail** (`src/model_cooling_heating.c:301-313`). `Cooling` gains `0.5 * coolingGas * Vvir^2`, undivided and only for a positive mass; `sage26_finalize_cooling_heating` normalises it once per snapshot (`src/core_build_model.c:590-603`). `CoolingRate` becomes the float `(coolingGas / dt) * 1e10 / Hubble_h * SEC_PER_GIGAYEAR / UnitTime_in_s`, the true Msun/Gyr rate over little h. The tail runs on the normal path and after an outer-guard failure (with zero mass, so `CoolingRate` becomes 0); the `lambda <= 0` and `rho_rcool <= 0` early returns skip it, leaving both fields as they were (`CoolingReturnPath` 2 or 3).
2. **`cool_gas_onto_galaxy`** (`:689-705`, called at `src/core_build_model.c:508-509`; the shared `mimic_sage_cool_gas_onto_galaxy`, `shared/sage_cooling_physics.h`, the `CGMrecipeOn 0` transfer). Below the hot reservoir the mass moves with the hot gas's metallicity; at or above it the whole hot reservoir, metals included, moves.

The tail reads only the mass, `Vvir` and `dt`, none of which the transfer changes, so running it here gives SAGE26's values; this module is the first that always holds the final mass. `Vvir` is read as the float SAGE26 holds.

This is the `CGMrecipeOn 0` (R0-R3) path. With `CGMrecipeOn 1` SAGE26 never calls `cool_gas_onto_galaxy`: its regime-aware wrapper applies the CGM and hot masses, without the equality branch, and writes `CoolingRate` itself (`:475-502`). `sage26_regime_aware_cooling`, the next module in `galaxy_physics`, ports that routine as one by-galaxy transaction, and `sage26_calculate_cooling_budget` then carries return path -1 and zero mass, so this module writes nothing.

## Processing Contract

- Supported mode: `process_by_galaxy`
- Expected phase: `galaxy_physics`, after `sage26_calculate_cooling_budget` and `sage26_radio_mode_heating`
- Receives one galaxy at a time; every non-merged galaxy cools, Type 2 orphans included (`src/core_build_model.c:473-516`)
- Without a positive interval (the core's snapshot-zero birth) cooling did not run, and the module changes nothing

## Ordering

**Enforced at init (fails with ERROR if violated):**

1. `sage26_calculate_cooling_budget` must precede this module in the same substep phase; `CoolingGas` and `CoolingReturnPath` come from it. `init()` refuses unless some substep phase holds both as `process_by_galaxy` with the budget's first entry before this module's first entry (`module_precedes_in_substep_phase`), so it also refuses the budget's absence.

**Not refused:** `sage26_radio_mode_heating` absent or after this module (this module then commits the budget's unsuppressed mass); a further entry of this module in another phase; and `CGMrecipeOn 1`, which this module does not read (the budget then carries return path -1 and zero mass, so this module writes nothing). Its `init()` does not check `sage26_regime_aware_cooling`, whose own `init()`, with `CGMrecipeOn 1`, requires it to sit immediately after this one.

## Properties

- Reads: `HaloNr`, `SnapNum`, `dT`, `Vvir`, `HotGas`, `MetalsHotGas`, `CoolingGas`, `CoolingReturnPath`
- Writes: `HotGas`, `MetalsHotGas`, `ColdGas`, `MetalsColdGas`, `Cooling`, `CoolingRate`

## Parameters

None. `Hubble_h` and `UnitTime_in_s` are core-owned.

## Tests

`_tests/test_unit_sage26_apply_cooling_oracle.c` runs every standalone `cool_gas_onto_galaxy` fixture under `_tests/oracle/` (listed in `../_tests/oracle/fixtures.manifest`) through `process()` and asserts bit-for-bit equality with the SAGE26 function-level oracle (`../_tests/oracle/README.md`). The tail's `Cooling` and `CoolingRate` are asserted by `sage26_calculate_cooling_budget`'s chained oracle test. Outside the oracle environment (macOS arm64, Apple clang 21) the bit-exact tests report SKIP.

`_tests/test_unit_sage26_apply_cooling.c` tests, on every platform: registration, initialisation through `module_system_init()` with the budget first in `galaxy_physics`, memory safety and property access; the partial and complete transfers and the metallicity they preserve; `Cooling` accumulated undivided and SAGE26's `CoolingRate` conversion; the early returns leaving `Cooling` and `CoolingRate`, and the outer guard writing `CoolingRate` 0; `HotGas` and `CoolingRate` unchanged without a positive interval, and `HotGas` and `ColdGas` unchanged with zero `CoolingGas`; a Type 2 orphan cooling its own hot gas; a row without a galaxy; and the error for `ngal` other than 1.

`_tests/test_integration_sage26_apply_cooling.py` runs Mimic with `sage26_reionization` and `sage26_prepare_infall_budget` in `pre_timestep` and `sage26_apply_infall`, `sage26_calculate_cooling_budget` and this module in `galaxy_physics` (`CGMrecipeOn 0`). It checks that the runs succeed, log the modules' initialisation and this module's cleanup, and report no memory leak; that the output holds `ColdGas`, `HotGas`, their metals and `Cooling`; and that for Type 0 rows those reservoirs and `Cooling` are finite and non-negative, some hold cold gas, and the hot and cold metallicities are at most 1.
