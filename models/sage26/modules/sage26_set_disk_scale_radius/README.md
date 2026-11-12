# `sage26_set_disk_scale_radius`

Computes the disk scale radius of each FoF's Type 0 central from its halo spin and virial properties, following SAGE26's `get_disk_radius`, then synchronises the central's bulge radii with SAGE26's `get_bulge_radius`.

## Physics

The disk radius follows Mo, Mao & White (1998): `DiskScaleRadius = (lambda / 1.414) * Rvir`, with the Bullock spin parameter `lambda = |J| / (1.414 * Vvir * Rvir)` from the halo's `Spin` vector. When `Vvir` or `Rvir` is not positive the radius falls back to `0.1 * Rvir`. The spin-component arithmetic is float, as in SAGE26.

Immediately after, the bulge radii are synchronised (`shared/sage_bulge_size_physics.h`). With `BulgeSizeOn 0` the three bulge radii are zeroed. With `BulgeSizeOn 3` (Tonini et al. 2016) `BulgeRadius` becomes the channel-mass-weighted average of `MergerBulgeRadius` and `InstabilityBulgeRadius`; a channel with mass but no radius takes a fallback, Shen et al. (2003) eq. 33 for the merger bulge and `0.2 * DiskScaleRadius` (the new disk radius) for the instability bulge.

## Processing Contract

- Supported mode: `process_full_halo`, expected in `pre_timestep`
- Receives the full FoF workspace
- Updates only the Type 0 central; satellites keep the radii inherited from their last time as a central (`SAGE parity:`, SAGE26 recomputes only the FoF central's radius). There is no end-of-snapshot radius pass.

## Ordering

`init()` refuses only a missing or unsupported `BulgeSizeOn`; it makes no placement or partner check, and no other module requires it. Advisory:

- Run in `pre_timestep`, before the substep phases, where star formation and the disk instability read `DiskScaleRadius`.
- The shipped run files place it after `sage26_initialise_merger_clock` and before `sage26_reionization` and `sage26_prepare_infall_budget`, SAGE26's order (radius and merger clock during progenitor joining, then the infall budget).

## Properties

- Reads: `Type`, `Spin`, `Vvir`, `Rvir`; with `BulgeSizeOn 3` also `MergerBulgeMass`, `InstabilityBulgeMass`, `MergerBulgeRadius`, `InstabilityBulgeRadius` and the `DiskScaleRadius` just written
- Writes: `DiskScaleRadius`, `BulgeRadius`, `MergerBulgeRadius`, `InstabilityBulgeRadius`

## Parameters

- `BulgeSizeOn` (required, 0 or 3): 3 (the shipped value and SAGE26's default) computes the Tonini et al. (2016) merger and instability bulge sizes, also reading the core's `Hubble_h`; 0 computes none, though the disk radius is still set and the bulge radii are zeroed. SAGE26's modes 1 and 2 are not supported; any other value or a missing key is refused at initialisation, naming the parameter.

## Tests

- `_tests/test_unit_sage26_set_disk_scale_radius.c`: the central's disk radius across spins and the fallback, satellites and empty rows left unchanged, and lifecycle.
- `_tests/test_integration_sage26_set_disk_scale_radius.py`: the module alone in a full run, checking `DiskScaleRadius` of real centrals against the closed form of the Mo, Mao & White formula.
- The `BulgeSizeOn` refusals (a missing key and every unsupported value) are covered at package level by `../_tests/test_unit_sage26_parameter_refusals.c`; `../_tests/test_unit_sage26_bulge_size_parameter.c` covers the module's refusal of mode 3 without the run configuration and the mode-0 helpers.
