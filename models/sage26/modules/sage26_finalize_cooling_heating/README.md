# `sage26_finalize_cooling_heating`

Turns the cooling and AGN heating energies and the supernova-reheated mass accumulated over a snapshot's substeps into rates, once per FoF group after the last substep. It normalises all three of SAGE26's end-of-halo sums: `Cooling`, `Heating` and `OutflowRate` (the package's `SupernovaOutflowRate`).

## SAGE26 Source

`sage26_apply_cooling` and `sage26_radio_mode_heating` add `0.5 * m * Vvir^2` to `Cooling` and `Heating` on every substep, undivided, and the shared feedback commit (`shared/sage_sf_feedback_physics.h`, SAGE26's `update_from_feedback`) adds the reheated mass to `SupernovaOutflowRate` for every disk star-formation and burst event, undivided, as SAGE26 does (`src/model_starformation_and_feedback.c:1249`). After its last substep SAGE26 normalises the three sums once per halo (`src/core_build_model.c:590-604`):

```c
const double deltaT = run_params->Age[galaxies[0].SnapNum] - halo_age;
const double inv_deltaT = 1.0/deltaT;
/* for every galaxy with mergeType 0 */
galaxies[p].Cooling *= inv_deltaT;
galaxies[p].Heating *= inv_deltaT;
galaxies[p].OutflowRate *= inv_deltaT;
```

This module reproduces that block:

- `deltaT` is the `dT` of the FoF's first galaxy, in the order Mimic holds the rows; the core computes each row's `dT` with SAGE26's `deltaT` expression. The first galaxy sets the interval even when it has merged itself, as SAGE26's galaxy zero does.
- `inv_deltaT = 1.0 / deltaT` multiplies `Cooling`, `Heating` and `SupernovaOutflowRate`, in that order, of every galaxy that has not merged or been disrupted this snapshot. `SupernovaOutflowRate` is a float, so its product is rounded to float, as SAGE26's `*=` on its float `OutflowRate` is. SAGE26's `mergeType > 0` is the package's `Type == 3`, which `sage26_resolve_mergers_and_disruption` sets.
- Workspace rows without a galaxy (which the core can keep) are skipped.
- Where the core gives the first galaxy no positive interval (its snapshot-zero birth, `dT = -1`), no physics ran, and `Cooling`, `Heating` and `SupernovaOutflowRate` of every non-merged galaxy in the FoF are set to 0. SAGE26 always has a positive interval there (`src/core_init.c:60-67`); that difference is core-owned.

## Processing Contract

- Supported mode: `process_full_halo`
- Required phase: `post_timestep` only, after all substep and merger work. In a substep phase it would multiply by `1/dT` once per substep, so `init()` enforces the placement: exactly one `post_timestep` entry as `process_full_halo`, and none in `pre_timestep` or any substep phase
- Receives the full FoF workspace

## Properties

- Reads: `Type`, `dT`, `Cooling`, `Heating`, `SupernovaOutflowRate`
- Writes: `Cooling`, `Heating`, `SupernovaOutflowRate`

## Parameters

None.

## Tests

`_tests/test_unit_sage26_finalize_cooling_heating_oracle.c` runs every normalisation fixture under `_tests/oracle/` (listed in `../_tests/oracle/fixtures.manifest`) through `process()` and asserts bit-for-bit equality of every galaxy's `Cooling` and `Heating` with the SAGE26 function-level oracle (`../_tests/oracle/README.md`), whose normalisation fixtures transcribe SAGE26's lines, since `evolve_galaxies` is static and not in the oracle's link. Outside the oracle environment (macOS arm64, Apple clang 21) it reports SKIP. `_tests/test_unit_sage26_finalize_cooling_heating.c` covers the module's boundary handling and the placement check, and `_tests/test_integration_sage26_finalize_cooling_heating.py` checks on a real run that only `Cooling` and `Heating` change.
