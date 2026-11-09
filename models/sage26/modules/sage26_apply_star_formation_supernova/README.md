# `sage26_apply_star_formation_supernova`

Commits the disk star formation and supernova feedback computed by `sage26_calculate_star_formation` and `sage26_calculate_supernova_feedback` to the galaxy reservoirs, and samples the disk star-formation history. This is an infrastructure commit step, not a swappable star-formation or feedback prescription.

## Physics

Before any reservoir changes, the module samples the disk history in the substep's bin, `(substep * 10) / num_substeps` capped at 9: `SfrDiskBin<k>` gains `NewStellarMass / dt`, and `SfrDiskColdGasBin<k>` and `SfrDiskColdGasMetalsBin<k>` record the cold gas and its metals; `sage26_finalize_sfr_histories` averages them after the last substep. It then applies SAGE26's `update_from_star_formation` and `update_from_feedback` (`shared/sage_sf_feedback_physics.h`, which both starburst channels share):

- `(1 - RecycleFraction) * NewStellarMass` moves from the cold gas to the stellar mass at the cold gas's metallicity; with `SFprescription 1`, `H2gas` and `H1gas` are then each capped at 0.74 of the cold gas left.
- The reheated mass, clamped to the cold gas left, moves from the galaxy's cold gas to the FoF central's hot reservoir, or with `CGMrecipeOn 1` and a central in `Regime` 0 to its CGM reservoir; the central's regime selects the reservoir, whatever the forming galaxy's own.
- The ejected mass, capped at that reservoir's gas after reheating, moves from it to the central's `EjectedGas` at the reservoir's metallicity, with the metals capped separately; the reservoir not selected is untouched.
- `SupernovaOutflowRate` accumulates the reheated mass, which `sage26_finalize_cooling_heating` turns into a rate; `StarFormationRate` accumulates `NewStellarMass / dT`, a snapshot-averaged diagnostic.

Both commits run on every substep a galaxy takes the ordinary chain, also when no stars formed.

## Processing Contract

- Supported mode: `process_by_galaxy`
- Expected phase: `galaxy_physics`, after `sage26_calculate_star_formation` and `sage26_calculate_supernova_feedback`
- Receives one galaxy at a time and uses the FoF central for the feedback transfers
- A feedback-free-burst row (`FFBRegime` 1) is skipped and nothing on it is written: `sage26_ffb_star_formation` runs that row's transaction
- Without a positive interval (the core's snapshot-zero birth) nothing is sampled or committed; any other non-positive interval is an error. With no FoF central galaxy the module samples the history but commits nothing

## Ordering

**Enforced at init (fails with ERROR if violated):**

1. When `sage26_calculate_star_formation` is configured, it must precede this module as `process_by_galaxy` in the same substep phase — otherwise a stale `NewStellarMass` from the previous substep is committed.
2. When `sage26_calculate_supernova_feedback` is configured, the same holds for it — the same stale-value risk for `SupernovaReheatedMass` and `SupernovaEjectedMass`.

**Advisory (emits WARNING if violated):**

- Neither calculator is configured — every transport field is zero; likely a configuration mistake.
- `sage26_apply_metal_enrichment` is not configured while star formation is — the disk metal yield is never applied.

## Properties

- Reads: `FFBRegime`, `dT`, `SnapNum`, `ColdGas`, `MetalsColdGas`, `NewStellarMass`, `SupernovaReheatedMass`, `SupernovaEjectedMass`, the FoF central's `Regime` and its selected `HotGas`/`MetalsHotGas` or `CGMgas`/`MetalsCGMgas`, and with `SFprescription 1` `H2gas` and `H1gas`; `HaloNr` and `Type` in log messages only
- Writes: `ColdGas`, `MetalsColdGas`, `StellarMass`, `MetalsStellarMass`, the central's selected pair, `EjectedGas`, `MetalsEjectedGas`, `StarFormationRate`, `SupernovaOutflowRate`, `SupernovaReheatedMass` and `SupernovaEjectedMass` (zeroed after the commit), `H2gas` and `H1gas` (`SFprescription 1`), the history bins `SfrDiskBin<k>`, `SfrDiskColdGasBin<k>`, `SfrDiskColdGasMetalsBin<k>`, and `NewStellarMass` only when the cold-gas cap below binds

## Parameters

- `RecycleFraction`: the fraction of the formed stellar mass returned at once to the cold gas; required, within [0, 1]; the run files hold 0.43.
- `SFprescription`: required, 0 or 1; 1 (Blitz & Rosolowsky 2006, the run files' value) applies the hydrogen caps, 0 does not. Any other value, or a missing key, fails initialisation naming the parameter.
- `CGMrecipeOn`: required, 0 or 1; 1 (the run files' value) routes the feedback by the central's `Regime`, 0 always to its hot reservoir. Any other value, or a missing key, fails initialisation naming the parameter.

## Notes

`sage26_calculate_supernova_feedback` scales star formation and reheating together so that their sum does not exceed the cold gas, and this module applies its result unchanged. Without that calculator nothing else limits star formation, so this module caps `NewStellarMass` at `ColdGas` before sampling and committing, and writes the capped value back for `sage26_apply_metal_enrichment`. `Yield` and `FracZleaveDisk` belong to `sage26_apply_metal_enrichment`, which applies the disk yield after the disk-instability chain.

## Tests

- `_tests/test_unit_sage26_apply_star_formation_supernova.c`: the transfers and their edge cases (the zero event included), the disk history sampling over a multi-substep snapshot, the `CGMrecipeOn` feedback destinations with the ejection and metal caps, the parameter refusals, the skipped FFB row and the lifecycle.
- `_tests/test_integration_sage26_apply_star_formation_supernova.py`: runs the star-formation chain and checks finite, non-negative reservoirs, plausible metallicities and star-formation rates, the response to `RecycleFraction` and `Yield`, and no memory leak; a second run without the supernova calculator checks that the cold-gas cap leaves no reservoir negative.
