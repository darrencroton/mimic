# `sage_reionization`

Calculates the baryon suppression factor for each galaxy using the configured global baryon fraction and the current halo state.

## Processing Contract

- Supported mode: `process_full_halo`
- Expected phase: `pre_timestep`
- Receives the full FoF workspace

## Ordering

No ordering enforcement is applied in `init()`. This module should run first in `pre_timestep` — before `sage_prepare_infall_budget` and `sage_satellite_stripping` — so that `HaloBaryonFraction` is set before infall and stripping calculations.

## Properties

- Reads: `Type`, `Mvir`
- Writes: `HaloBaryonFraction`

## Parameters

- `GlobalBaryonFraction`
