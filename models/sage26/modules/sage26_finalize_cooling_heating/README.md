# `sage26_finalize_cooling_heating`

Turns the cooling and AGN-heating energies and the supernova-reheated mass accumulated over a snapshot's substeps into rates, once per FoF group after the last substep, and writes the output cooling and cold-stream rates `mdot_cool` and `mdot_stream`.

## Processing Contract

- Supported mode: `process_full_halo`
- Required phase: `post_timestep` only, after all substep and merger work; in a substep phase it would divide by the interval once per substep
- Receives the full FoF workspace; rows without a galaxy are skipped

## Ordering

**Enforced at init (fails with ERROR if violated):**

1. No entry of this module, in any mode, in `pre_timestep` or a substep phase, and exactly one entry in `post_timestep`, as `process_full_halo`.

**Not refused:** its position among the other `post_timestep` modules; the absence of the modules that accumulate the sums or write the raw rates (it normalises whatever the substeps left); `CGMrecipeOn`, which it does not read.

## Properties

- Reads: `Type`, `dT`, `Cooling`, `Heating`, `SupernovaOutflowRate`, `mdot_cool_raw`, `mdot_stream_raw`
- Writes: `Cooling`, `Heating`, `SupernovaOutflowRate`, `mdot_cool`, `mdot_stream`

## Parameters

None. The unit factors are the run's (`UnitMass_in_g`, `UnitTime_in_s`).

## Notes

During the substeps, `sage26_apply_cooling` and `sage26_radio_mode_heating` (or, with `CGMrecipeOn 1`, `sage26_regime_aware_cooling`) add `0.5 * m * Vvir^2` to `Cooling` and `Heating`, and the feedback commits add each event's reheated mass to `SupernovaOutflowRate`, all undivided. This module multiplies the three by `1 / dT` of the FoF's first galaxy, the snapshot interval, for every galaxy that has not merged or been disrupted this snapshot (`Type` other than 3), as SAGE26 does once per halo. Where the first galaxy has no positive interval (the core's snapshot-zero birth) no physics ran, and the three are set to 0.

`mdot_cool_raw` and `mdot_stream_raw` are package-private code-unit rates written only by `sage26_regime_aware_cooling`. For every galaxy this module finalises, on both paths, it converts them to the outputs `mdot_cool` and `mdot_stream` in Msun/yr with SAGE26's writer arithmetic (`raw * UnitMass_in_g / UnitTime_in_s * SEC_PER_YEAR / SOLAR_MASS`, left to right, narrowed to float). The conversion is done here rather than by an `output_convert`, whose grouping of the factors can round differently. With `CGMrecipeOn 0` nothing writes the raw rates and both outputs are 0.

## Tests

- `_tests/test_unit_sage26_finalize_cooling_heating.c`: the normalisation and the no-interval path, the placement check, and the writer conversion against SAGE26's expression written inline on both paths, including a never-assigned rate.
- `_tests/test_integration_sage26_finalize_cooling_heating.py`: checks that the module initialises in `post_timestep` and that, in a `CGMrecipeOn 0` cooling run, adding it changes only `Cooling` and `Heating`, leaving zero energies zero and raising every positive one.
