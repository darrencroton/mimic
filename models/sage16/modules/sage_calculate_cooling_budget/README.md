# `sage_calculate_cooling_budget`

Calculates the gas cooling budget from the hot halo to the cold disk using metallicity-dependent cooling functions. The result is stored in transport fields for downstream cooling modifiers and the apply step.

## Processing Contract

- Supported mode: `process_by_galaxy`
- Expected phase: `galaxy_physics`, before `sage_radio_mode_heating` and `sage_apply_cooling`
- Receives one galaxy at a time

## Properties

- Reads: `HotGas`, `MetalsHotGas`, `dT`, `Vvir`, `Rvir`
- Writes: `CoolingGas`, `CoolingLambda`, `Rcool`

## Parameters

None.

## Notes

Two cooling regimes based on the cooling radius (Rcool):

**Cold accretion (Rcool > Rvir):** rapid cooling throughout the halo — `CoolingGas = HotGas * (Vvir / Rvir) * dt`. Dominant in low-mass halos and at high redshift.

**Hot halo cooling (Rcool < Rvir):** cooling within the cooling radius only — `CoolingGas = (HotGas / Rvir) * (Rcool / (2 * tcool)) * dt`. Dominant in massive halos with hot atmospheres.

Cooling tables are the Sutherland & Dopita (1993) tables covering 10^4–10^8.5 K at 8 metallicity bins from primordial to super-solar. They are loaded from `CoolFunctions/` during `init()`. Virial temperature is derived from virial velocity as `T_vir = 35.9 * Vvir^2` K (Vvir in km/s).

`CoolingGas`, `CoolingLambda`, and `Rcool` are transport properties. If this module does not run, downstream modules (`sage_radio_mode_heating`, `sage_apply_cooling`) see zero cooling budget.
