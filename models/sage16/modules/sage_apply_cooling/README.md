# `sage_apply_cooling`

Commits the cooling budget calculated earlier in `galaxy_physics` by transferring `CoolingGas` from the hot reservoir to the cold reservoir and accumulating cooling energy diagnostics.

## Processing Contract

- Supported mode: `process_by_galaxy`
- Expected phase: `galaxy_physics`, after `sage_calculate_cooling_budget` and any cooling modifiers such as `sage_radio_mode_heating`
- Receives one galaxy at a time; skips invalid galaxies according to the module implementation

## Ordering

**Enforced at init (fails with ERROR if violated):**

1. `sage_calculate_cooling_budget` must precede this module in the same substep phase — `CoolingGas` will be zero without it and no gas is transferred.

## Properties

- Reads: `Type`, `dT`, `Vvir`, `HotGas`, `MetalsHotGas`, `CoolingGas`
- Writes: `HotGas`, `MetalsHotGas`, `ColdGas`, `MetalsColdGas`, `Cooling`

## Parameters

None.

## Notes

`CoolingGas` is a transport property. If no upstream module sets it, this module has no gas budget to apply.
