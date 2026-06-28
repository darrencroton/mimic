# `sham_assign_stellar_mass`

Tracks each galaxy branch's peak virial mass (`ShamMpeak`) and peak maximum circular velocity (`ShamVpeak`), then assigns `StellarMass` from a monotonic double-power-law stellar-to-halo mass relation with optional deterministic log-normal scatter.

## Processing Contract

- Supported mode: `process_full_halo`
- Expected phase: `post_timestep`, once per snapshot after halo inheritance has been advanced
- Receives all galaxies in the FoF workspace; skips NULL-galaxy entries and Type 3 halos

## Ordering

No ordering constraints. This module has no upstream budget producers or downstream consumers within the SHAM pipeline.

## Properties

- Reads: `Type`, `dT`, `Mvir`, `Vmax`
- Writes: `StellarMass`, `BulgeMass`, `MetalsStellarMass`, `MetalsBulgeMass`, `StarFormationRate`, `ShamMpeak`, `ShamVpeak`, `ShamStellarMassNoScatter`, `ShamScatterDex`, `ShamOrphanAge`

## Parameters

- `ShamLogM1`, `ShamN`, `ShamBeta`, `ShamGamma`: double-power-law SHMR parameters.
- `ShamScatterDex`: log-normal scatter in dex.
- `ShamUseScatter`: `0` disables scatter, `1` enables deterministic scatter.
- `ShamMinMpeak`: minimum peak mass in `1e10 Msun/h`.
- `ShamMinVpeak`: minimum peak circular velocity in `km/s`.
- `ShamMaxStellarBaryonFraction`: cap on `StellarMass / ShamMpeak`.
- `ShamOrphanMaxAgeMyr`: maximum Type 2 lifetime in Myr/h; `0` disables removal.

## Notes

Type 0 and Type 1 galaxies update their peak proxies from the current resolved halo. Type 2 galaxies retain their last resolved peak proxies and accumulate `ShamOrphanAge`. If `ShamOrphanMaxAgeMyr > 0`, orphans older than the configured maximum are marked Type 3 before output. `BulgeMass`, `MetalsStellarMass`, `MetalsBulgeMass`, and `StarFormationRate` are reset to zero by this module; they are included in the dependency list because the module unconditionally clears them.

## References

Conroy et al. (2006), Vale & Ostriker (2006), Reddick et al. (2013), Moster et al. (2013), Guo & White (2014).
