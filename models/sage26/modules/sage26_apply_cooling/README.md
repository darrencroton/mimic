# `sage26_apply_cooling`

Commits the cooling mass of the classic hot-halo path (`CGMrecipeOn 0`): it records the substep's cooling energy and rate, then moves the mass from the hot to the cold reservoir. It is the last of the three modules that together implement SAGE26's `cooling_recipe_hot`: `sage26_calculate_cooling_budget` forms the mass, `sage26_radio_mode_heating` suppresses it with AGN heating, and this module applies what is left in `CoolingGas`.

## Physics

`Cooling` gains `0.5 * CoolingGas * Vvir^2` for a positive mass, undivided; `sage26_finalize_cooling_heating` turns the sum into a rate once per snapshot. `CoolingRate` becomes `CoolingGas / dt` converted with SAGE26's factor, which gives the true Msun/Gyr rate divided by h (the property's metadata states it). After the budget's outer guard (no hot gas or no `Vvir`) the mass is zero and `CoolingRate` becomes 0; on the budget's `lambda <= 0` and `rho_rcool <= 0` early returns (`CoolingReturnPath` 2 or 3) both fields keep their values. The transfer is SAGE26's `cool_gas_onto_galaxy` (`mimic_sage_cool_gas_onto_galaxy`, `shared/sage_cooling_physics.h`): a mass below the hot reservoir moves with the hot gas's metallicity, and a mass at or above it moves the whole hot reservoir, metals included.

## Processing Contract

- Supported mode: `process_by_galaxy`
- Expected phase: `galaxy_physics`, after `sage26_calculate_cooling_budget` and `sage26_radio_mode_heating`
- Receives one galaxy at a time; every galaxy that has not merged cools, Type 2 orphans included
- Without a positive interval (the core's snapshot-zero birth) cooling did not run, and the module changes nothing

## Ordering

**Enforced at init (fails with ERROR if violated):**

1. `sage26_calculate_cooling_budget` must precede this module as `process_by_galaxy` in the same substep phase — `CoolingGas` and `CoolingReturnPath` come from it, so its absence is refused too.

**Not refused:** without `sage26_radio_mode_heating` the module commits the budget's unsuppressed mass (that module's own `init()` refuses it placed after this one). With `CGMrecipeOn 1` the budget carries return path -1 and zero mass, so this module writes nothing; `sage26_regime_aware_cooling`, whose own `init()` requires it immediately after this module, then owns the cooling.

## Properties

- Reads: `HaloNr`, `SnapNum`, `dT`, `Vvir`, `HotGas`, `MetalsHotGas`, `CoolingGas`, `CoolingReturnPath`
- Writes: `HotGas`, `MetalsHotGas`, `ColdGas`, `MetalsColdGas`, `Cooling`, `CoolingRate`

## Parameters

None. `Hubble_h` and `UnitTime_in_s` are core-owned.

## Tests

- `_tests/test_unit_sage26_apply_cooling.c`: registration and initialisation behind the budget, the partial and complete transfers and the metallicity they preserve, the `Cooling` and `CoolingRate` updates on each return path, and the no-interval, zero-mass and orphan cases.
- `_tests/test_integration_sage26_apply_cooling.py`: a `CGMrecipeOn 0` run with infall and the cooling budget, checking that it leaks no memory and that the reservoirs, their metallicities and `Cooling` are finite and physical.
