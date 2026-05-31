# SHAM Stellar Mass Assignment

This module is a Mimic-native subhalo abundance matching approximation. It
tracks each galaxy branch's peak virial mass (`ShamMpeak`) and peak circular
velocity (`ShamVpeak`) through the existing Mimic halo inheritance system, then
assigns `StellarMass` from a monotonic double-power-law stellar-to-halo mass
relation.

## Pipeline

Run as `process_full_halo`, normally in `post_timestep`, so each snapshot gets
one stellar-mass assignment after the halo branch has been updated.

## Properties

Reads: `Type`, `dT`, `Mvir`, `Vmax`.

Writes: `StellarMass`, `BulgeMass`, `MetalsStellarMass`, `MetalsBulgeMass`,
`StarFormationRate`, `ShamMpeak`, `ShamVpeak`, `ShamStellarMassNoScatter`,
`ShamScatterDex`, `ShamOrphanAge`.

Type 0 and Type 1 galaxies update their peak proxies from the current resolved
halo. Type 2 galaxies retain their last resolved peak proxies and accumulate
`ShamOrphanAge`. If `ShamOrphanMaxAgeMyr > 0`, older orphans are marked Type 3
before output.

## Parameters

- `ShamLogM1`, `ShamN`, `ShamBeta`, `ShamGamma`: double-power-law SHMR parameters.
- `ShamScatterDex`: log-normal scatter in dex.
- `ShamUseScatter`: `0` disables scatter, `1` enables deterministic scatter.
- `ShamMinMpeak`: minimum peak mass in `1e10 Msun/h`.
- `ShamMinVpeak`: minimum peak circular velocity in `km/s`.
- `ShamMaxStellarBaryonFraction`: cap on `StellarMass / ShamMpeak`.
- `ShamOrphanMaxAgeMyr`: maximum Type 2 lifetime in Myr/h; `0` disables removal.

## References

Conroy et al. (2006), Vale & Ostriker (2006), Reddick et al. (2013), Moster et
al. (2013), Guo & White (2014).

