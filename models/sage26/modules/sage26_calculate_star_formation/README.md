# `sage26_calculate_star_formation`

Calculates the stellar mass formed during the current substep with SAGE26's disk star formation (`SFprescription 0`, Croton et al. 2006) and stores it in a transport field for the feedback and apply steps. It also writes SAGE26's H I diagnostic, `H1gas`.

## SAGE26 Source

The module ports the start of SAGE26's `starformation_and_feedback()` on R0 (`src/model_starformation_and_feedback.c` in the staged SAGE26 source):

- `sfr_croton06` (`:160-184`): `reff = 3.0 * DiskScaleRadius`, `tdyn = reff / Vvir`, `cold_crit = 0.19 * Vvir * reff`, and `strdot = SfrEfficiency * (ColdGas - cold_crit) / tdyn` when `ColdGas > cold_crit`. A galaxy whose own `Vvir` is not positive forms no stars, and SAGE26 never forms `tdyn` for it (`:170-171`). `StarFormingDiskFactor` enters exactly where SAGE26's double literal `SF_DISK_RADIUS_FRAC` (`3.0`, `:60`) does, and the run files hold `3.0`.
- The H I block (`:1035-1046`), which R0 runs on every call although its star formation ignores it: `atomicH = ColdGas * 0.74 - H2gas` (one fused multiply-subtract in SAGE26's build, an explicit `fma` here), floored at zero, times `1 - f_ion`, the neutral fraction of an exponential gas disk truncated at 0.5 Msun/pc^2 (`ionized_gas_fraction`, `:135-152`), stored in the float `H1gas`. `H2gas` is read, never written, on R0. The prescription-0 path does not resynchronise H I after the depletion that follows (`:1128-1135`, `:1169-1172` run only for prescriptions that track H2), so `H1gas` can exceed the hydrogen left in `ColdGas`: a deliberate SAGE26 quirk.
- `stars = strdot * dt`, floored at zero (`:1048-1051`), stored in `NewStellarMass`.

`Vvir` is read as the float SAGE26 holds; the core's double `Vvir`, the substep `dt` and the snapshot-zero interval are core-owned.

## Processing Contract

- Supported mode: `process_by_galaxy`
- Expected phase: `galaxy_physics`, before `sage26_calculate_supernova_feedback` and `sage26_apply_star_formation_supernova`
- Receives one galaxy at a time

## Ordering

**Enforced at init (fails with ERROR if violated):**

1. `sage26_apply_star_formation_supernova` must be present somewhere in the pipeline — without it, `NewStellarMass` is computed each substep but never committed to galaxy reservoirs (silent output loss).

## Properties

- Reads: `Type`, `HaloNr`, `SnapNum`, `dT`, `Vvir`, `ColdGas`, `DiskScaleRadius`, `H2gas`
- Writes: `NewStellarMass`, `H1gas`

## Parameters

- `SfrEfficiency`
- `StarFormingDiskFactor`

## Notes

`NewStellarMass` is a transport field. The persistent stellar reservoir is updated later by `sage26_apply_star_formation_supernova`.

## Tests

`_tests/test_unit_sage26_calculate_star_formation_oracle.c` is the chained oracle test of the whole disk star-formation chain: it runs this module, `sage26_calculate_supernova_feedback`, `sage26_apply_star_formation_supernova`, `sage26_disk_instability` (where SAGE26's instability transfers nothing) and `sage26_apply_metal_enrichment` through their `process()` entries on every `starformation_and_feedback` fixture under `_tests/oracle/`, single calls and whole-snapshot sequences, and asserts bit-for-bit equality with the SAGE26 function-level oracle (`../_tests/oracle/README.md`), including every element of the six per-substep histories (after each single call and after the last substep of each sequence) and each substep's history bin. After the last substep of a sequence it runs `sage26_finalize_sfr_histories` and asserts each galaxy's `SubstepsUsed` and SAGE26's writer averages (`SfrDisk`, `SfrBulge`, `SfrDiskZ`, `SfrBulgeZ`) with their divisors. Its file comment lists the SAGE26 locals it does not assert, and why. Outside the oracle environment (macOS arm64, Apple clang 21) it reports SKIP. `_tests/test_unit_sage26_calculate_star_formation.c` covers the module's edge cases and lifecycle.
