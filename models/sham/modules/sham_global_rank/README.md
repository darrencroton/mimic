# `sham_global_rank`

Whole-snapshot rank abundance matching: at every snapshot it ranks every galaxy of the snapshot, across all FoF groups, by peak maximum circular velocity and assigns `StellarMass` by inverting an explicit analytic cumulative stellar mass function. The target is an **uncalibrated framework demonstration**. It is not fitted to any observed stellar mass function and makes no claim of realistic stellar masses or observational parity.

## Processing Contract

- Supported mode: `process_snapshot` only, configured exactly once under `modules.post_snapshot`. That phase runs only under the horizontal driver, once per snapshot after every FoF group is processed and before the snapshot is published for inheritance or written.
- Receives the borrowed current-generation population (Types 0/1/2 of every FoF group). Every entry must have a non-NULL galaxy and a positive `UniqueGalaxyID` unique within the snapshot. A violation fails the snapshot (and the run); entries are never dropped and IDs are never invented.
- The population is never reordered: ranking sorts module-owned scratch (one 24-byte record per entry, tracked under `MEM_UTILITY`), which is released before the callback returns. Scratch scales with the current population, not with the number of snapshots. An empty snapshot allocates nothing.
- All validation runs before any write, so a failed snapshot leaves its galaxies untouched.

## Ordering

**Enforced at init (fails with ERROR if violated):**

1. The module must appear exactly once in `modules.post_snapshot` as `process_snapshot`.
2. `sham_assign_stellar_mass` must not be configured in any phase: the two modules are independent stellar-mass prescriptions and would silently overwrite each other.

## Prescription

- **Peaks.** Type 0/1 entries update `ShamVpeak = max(ShamVpeak, Vmax)` and `ShamMpeak = max(ShamMpeak, Mvir)`; Type 2 orphans keep the peaks they inherited. Every consumed peak and proxy must be finite and nonnegative, and the updated mass peak must fit the float `ShamMpeak` (at most `FLT_MAX`), even for an entry that is not ranked.
- **Candidates.** Every Type 0/1/2 entry with `ShamVpeak > 0`. There is no central-only cut, mass cut, scatter, orphan lifetime cut or baryon cap.
- **Rank.** Descending `ShamVpeak`; exact ties go to the lower `UniqueGalaxyID`. Zero-based rank `r`.
- **Target.** `n(>M) = n0 (M / M0)^(-alpha)` evaluated at the rank density `n = (r + 0.5) / BoxSize^3`, so `ln M_r = ln M0 - [ln(r + 0.5) - 3 ln BoxSize - ln n0] / alpha`. It is evaluated in double logarithms only: the rank density, the volume and their ratio to `n0` are never formed, because they overflow or underflow for admitted parameters whose mass is representable. There is no table, extrapolation, or normalisation to the candidate count.
- **Range.** A rank with `ln M_r > ln(100000)` (decided on the computed double, before exponentiation) fails the snapshot; so does a float mass that is not finite, rounds to zero, or exceeds `100000`. Nothing is clipped; a mass that rounds to a positive float subnormal is stored as that subnormal. The diagnostic names the rank, `UniqueGalaxyID`, `ln M_r` and the four parameters.
- **Writes.** Every entry has `StellarMass`, `ShamStellarMassNoScatter`, `ShamScatterDex`, `BulgeMass`, `MetalsStellarMass`, `MetalsBulgeMass` and `StarFormationRate` reset to zero; ranked entries then receive the float `M_r` in `StellarMass` and `ShamStellarMassNoScatter`. `ShamOrphanAge`, `Type` and every halo field are left unchanged: the module neither ages nor removes orphans.

The upper-bound decision follows the double computation, including its platform roundoff, and does not promise an exact-real classification arbitrarily close to the boundary. The exact endpoint (`M0 = 100000`, `n0 * BoxSize^3 = 0.5`, `alpha = 1`, rank 0) is accepted and stores exactly `100000.0f`.

## Properties

- Reads: `Type`, `UniqueGalaxyID`, `Vmax`, `Mvir`, `ShamVpeak`, `ShamMpeak`
- Writes: `ShamVpeak`, `ShamMpeak`, `StellarMass`, `ShamStellarMassNoScatter`, `ShamScatterDex`, `BulgeMass`, `MetalsStellarMass`, `MetalsBulgeMass`, `StarFormationRate`

## Parameters

- `ShamGlobalMassScale`: `M0`, in `(0, 100000]` internal units of `1e10 Msun/h` (declared in `parameter_units.yaml` and checked after conversion).
- `ShamGlobalNumberDensity`: `n0`, in `[1e-12, 1e3]` `(Mpc/h)^-3` (fixed units, not converted).
- `ShamGlobalSlope`: `alpha`, dimensionless, in `[0.1, 10]`.

Every value must be finite (`nan` and `inf` are rejected explicitly). `BoxSize` comes from the simulation package and must be finite and positive with a finite positive cube; the cube is formed only for that check.

## Example and Tests

`models/sham/input/sham_global_micro-uchuu-ascii-horizontal.yaml` runs the module on the committed micro-Uchuu horizontal fixture (`BoxSize` 100 Mpc/h) with `M0 = 8`, `n0 = 1e-6`, `alpha = 1`, so rank `r` receives `8 / (r + 0.5)` (rank 0: exactly 16). The values were chosen only to keep the example inside the module's range. The fixture is three synthetic forests over six snapshots, not a complete cosmological volume, so its masses say nothing about whole-box science. Build `MODEL=sham SIMULATION=micro-uchuu-ascii-horizontal` and run the file from the repository root.

`make tests-snapshot-global-sham` builds that pair as a test build and runs `_tests/test_unit_sham_global_rank.c` (oracle, invariance, invalid inputs, numerical edge battery) and `_tests/test_integration_sham_global_rank.py` (end-to-end ranks per `UniqueGalaxyID`, inheritance, determinism and temporary fixture derivatives), failing on any skip. `_tests/sham_global_rank_reference.py` is the independent 60-digit Decimal reference behind the edge battery. Under the default vertical `sham` pair the Python cases skip with a stated reason, and the C unit test runs in full.

## References

Conroy et al. (2006) and Vale & Ostriker (2006) for the abundance-matching construction; Reddick et al. (2013) for peak circular velocity as the ranking proxy.
