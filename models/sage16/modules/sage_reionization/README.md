# `sage_reionization`

Calculates the baryon suppression factor for each galaxy using the configured global baryon fraction and the current halo state.

## Processing Contract

- Supported mode: `process_full_halo`
- Expected phase: `pre_timestep`
- Receives the full FoF workspace

## Ordering

`init()` enforces ordering: when `sage_prepare_infall_budget` is also configured in `pre_timestep`, reionization must run before it, because reionization writes `HaloBaryonFraction` that the infall budget reads. Later-phase consumers such as `sage_satellite_stripping` (a substep phase) are structurally guaranteed to run after `pre_timestep`, so no explicit check is needed for them.

## Properties

- Reads: `Type`, `Mvir`
- Writes: `HaloBaryonFraction`

## Parameters

- `GlobalBaryonFraction`
