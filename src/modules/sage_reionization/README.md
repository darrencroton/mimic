# `sage_reionization`

Calculates the baryon suppression factor for each galaxy using the configured global baryon fraction and the current halo state.

## Processing Contract

- Supported mode: `process_full_halo`
- Expected phase: `pre_timestep`
- Receives the full FoF workspace

## Properties

- Reads: `Type`, `Mvir`
- Writes: `HaloBaryonFraction`

## Parameters

- `GlobalBaryonFraction`

## Notes

Downstream infall and stripping modules use `HaloBaryonFraction`, so this module should run before `sage_prepare_infall_budget` and `sage_satellite_stripping`.
