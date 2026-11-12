# `sage26_set_halo_concentration`

Assigns each new FoF central its NFW halo concentration at progenitor joining, as SAGE26 does with `ConcentrationOn 3`: the concentration is inverted from `Vmax/Vvir`, using the galaxy's current velocities if it was a central and its infall-frozen velocities if it was a satellite. With `ConcentrationOn 0` it writes nothing. The helpers are in `shared/sage_halo_concentration.h`.

## Physics

For an NFW halo (Navarro, Frenk & White 1997), `(Vmax / Vvir)^2 = 0.21621 * c / mu(c)` with `mu(c) = ln(1 + c) - c / (1 + c)`. The module solves this for `c` by exactly 60 bisection steps on [1, 200], returning the final bracket's midpoint. The concentration is 0 when `Vvir` or `Vmax` is not positive or when `(Vmax / Vvir)^2 < 1`; a squared ratio beyond the value at `c = 200` (about 10.04) saturates at 200.

SAGE26's bisection is kept as written (`SAGE parity:` in the source), including one consequence users may notice: `c / mu(c)` is not monotonic below about `c = 2.16`, so every squared ratio from 1 up to about 1.005 returns exactly 1.0, and no concentration strictly between 1 and 2.5546875 is produced.

For each row whose new `Type` is 0, with `ConcentrationOn 3`:

| `PreviousType` | `Concentration` |
|---|---|
| 0 | the inversion of the current `Vmax` and `Vvir` |
| 1 | the inversion of `infallVmax` and `infallVvir` |
| -1 (created this snapshot) | kept at its creation value 0 |
| 2 or 3 | kept |

Every row whose new `Type` is not 0 keeps its value, so a satellite carries its last central concentration. The velocities are narrowed to float before the inversion, and the result is stored as a float.

## Processing Contract

- Supported mode: `process_full_halo`, in `pre_timestep`, exactly once. `init()` refuses any other placement, naming the module.
- Receives the full FoF workspace
- The shipped run files place it after `sage26_initialise_merger_clock` and before `sage26_set_disk_scale_radius`, SAGE26's order; the three modules touch disjoint state, and the position within `pre_timestep` is not checked.

## Ordering

- With `ConcentrationOn 3`, `init()` refuses a run without `sage26_record_transition_state` in `post_timestep` as `process_full_halo`: it records `PreviousType`, without which no central would ever be assigned. With `ConcentrationOn 0` this check does not apply.
- No other module requires this one; without it every row keeps `Concentration` 0.

## Properties

- Reads: `Type`, `PreviousType`, `Vmax`, `Vvir`, `infallVmax`, `infallVvir`
- Writes: `Concentration` (only with `ConcentrationOn 3`)

## Parameters

- `ConcentrationOn` (required, 0 or 3): 3 (the shipped value and SAGE26's default) assigns the `Vmax/Vvir` inversion; 0 assigns none. SAGE26's modes 1 (the Ishiyama et al. 2021 table) and 2 (`Vmax/Vvir` for every Type) are not supported; any other value or a missing key is refused at initialisation, naming the parameter.

## Tests

- `_tests/test_unit_sage26_set_halo_concentration.c`: the velocity-pair selection by `PreviousType`, the kept cases, the ratio edge cases, `ConcentrationOn 0`, and joining compositions through the core's inheritance.
- `_tests/test_integration_sage26_set_halo_concentration.py`: the no-op with `ConcentrationOn 0` and the writes with 3.
- The inversion helpers are tested by `../../shared/_tests/test_unit_sage_halo_concentration.c`.
- The placement and recorder checks are covered at package level by `../_tests/test_unit_sage_dependency_contracts.c`, and the parameter refusals also by `../_tests/test_unit_sage26_parameter_refusals.c`.
