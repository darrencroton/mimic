# `sage_apply_cooling`

Commits the cooling budget calculated earlier in `phase_1` by transferring `CoolingGas` from the hot reservoir to the cold reservoir and accumulating cooling energy diagnostics.

## Processing Contract

- Supported mode: `process_by_galaxy`
- Expected phase: `phase_1`, after `sage_calculate_cooling_budget` and any cooling modifiers such as `sage_radio_mode_heating`
- Receives one galaxy at a time; skips invalid galaxies according to the module implementation

## Properties

- Reads: `Type`, `dT`, `Vvir`, `HotGas`, `MetalsHotGas`, `CoolingGas`
- Writes: `HotGas`, `MetalsHotGas`, `ColdGas`, `MetalsColdGas`, `Cooling`

## Parameters

None.

## Notes

`CoolingGas` is a transport property. If no upstream module sets it, this module has no gas budget to apply.
