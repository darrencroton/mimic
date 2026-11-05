# `sage26_disk_instability`

Ports SAGE26's `check_disk_instability` up to its black-hole and burst calls (`R/src/model_disk_instability.c:47-101`) for `BulgeSizeOn` 0 (R0) and 3 (R1): the Mo, Mao & White (1998) stability test, the transfer of unstable disk stars and their metals into the bulge and its instability channel, the incremental instability-bulge radius that follows the transfer, and the unstable cold-gas fraction handed to the quasar-mode and starburst modules.

## Processing Contract

- Supported mode: `process_by_galaxy`
- Expected phase: `galaxy_physics`, after disk radii and the star-formation and feedback commit (`sage26_apply_star_formation_supernova`), and before `sage26_quasar_mode`, `sage26_starburst_feedback` and the disk yield (`sage26_apply_metal_enrichment`), the order of SAGE26's `starformation_and_feedback` (`R/src/model_starformation_and_feedback.c:1087-1108`)
- Receives one galaxy at a time

## Properties

- Reads: `Vmax` (the galaxy's own halo), `ColdGas`, `StellarMass`, `MetalsStellarMass`, `BulgeMass`, `MetalsBulgeMass`, `DiskScaleRadius`; with `BulgeSizeOn` 3 also `InstabilityBulgeMass`, `MergerBulgeMass` and the three bulge radii
- Writes: `BulgeMass`, `MetalsBulgeMass`, `InstabilityBulgeMass`, `UnstableDiskGasFraction`; with `BulgeSizeOn` 3 also `BulgeRadius`, `MergerBulgeRadius` and `InstabilityBulgeRadius`

## Parameters

- `StarFormingDiskFactor`: enters `Mcrit` exactly where SAGE26's `TOOMRE_DISK_FACTOR`, the double `3.0`, does (`R/src/model_disk_instability.c:35`, `:57`). The run files hold `3.0`; any other value departs from SAGE26.
- `BulgeSizeOn` (required, 0 or 3): SAGE26's bulge-size mode. 0, SAGE26's R0, computes no sizes (`R/src/model_misc.c:242-247`, `:395`); 3, the run files' value, is SAGE26's Tonini et al. (2016) merger and instability bulges (`:317-424`, `R1.par:109`). Modes 1 and 2 are not ported: any value other than 0 or 3, or a missing key, fails initialisation with an error naming the parameter. Mode 3 also reads the core's `Hubble_h`.

## Physics

The arithmetic lives in `shared/sage_disk_instability_physics.h` (`mimic_sage_check_disk_instability`), which the shared burst transaction (`shared/sage_starburst_physics.h`) also calls for SAGE26's minor-merger instability recheck (`R/src/model_mergers.c:807-809` calls the same `check_disk_instability`) and continues with the gas branch's black-hole growth and burst itself.

- `diskmass = ColdGas + (StellarMass - BulgeMass)`, a float sum widened once (`:54`). Nothing happens unless `diskmass > 0.0` (`:55`).
- `Mcrit = Vmax * Vmax * (3.0 * DiskScaleRadius) / G`, with SAGE26's float `Vmax * Vmax` product, capped at `diskmass` (`:57-60`).
- `unstable_gas = (ColdGas / diskmass) * (diskmass - Mcrit)` and `unstable_stars = (1 - ColdGas / diskmass) * (diskmass - Mcrit)`, doubles (`:63-66`).
- Where `unstable_stars > 0`: the disk metallicity is `get_metallicity` of the float disk differences (`:75`); the double `unstable_stars` is added straight into the float `BulgeMass` and `InstabilityBulgeMass`, and `metallicity * unstable_stars` into the float `MetalsBulgeMass` as one fused multiply-add, as SAGE26's compiled object does (`:77-79`).
- No cap limits the transfer to the disk's stars or metals, and the bulge is not clamped back to `StellarMass` or `MetalsStellarMass`. SAGE26 has neither defence, and its float stores can leave `BulgeMass` or `MetalsBulgeMass` one rounding above the stellar totals; the port keeps that.
- After the three stores, the incremental instability-bulge radius (`:81-83`, `update_instability_bulge_radius`, `R/src/model_misc.c:385-424`, `shared/sage_bulge_size_physics.h`) from the post-store channel mass and the disk radius saved before the transfer (`:68-70`). With `BulgeSizeOn 3` it stores Tonini et al. (2016) eq. 15, or `0.2 * DiskScaleRadius` for a first transfer, and recombines the three radii (`get_bulge_radius`); with `BulgeSizeOn 0` it returns at once (`R/src/model_misc.c:395`): every bulge radius keeps the value it had, while `InstabilityBulgeMass` is still written.
- Where `unstable_gas > 0` and `ColdGas > 0` (`:90`), `UnstableDiskGasFraction = unstable_gas / ColdGas` (`:101`); otherwise it is 0.

## Notes

`UnstableDiskGasFraction` is a transport property: it is this module's hand-off of the gas branch to SAGE26's continuation. Place its consumers after this module in the same phase, in SAGE26's order: `sage26_quasar_mode` for `grow_black_hole` with its quasar wind, then `sage26_starburst_feedback` for `collisional_starburst_recipe` (`:101-108`), both `process_by_galaxy`.

The oracle test (`_tests/test_unit_sage26_disk_instability_oracle.c`) also runs the c07 fixtures: `update_instability_bulge_radius` through the shared helper directly (`uibr_*`), and `check_disk_instability` with `BulgeSizeOn 3` (`di_s3_*`), whose recorded state includes the bulge sizes. It compares the module bit for bit with SAGE26's own routine on every fixture under `_tests/oracle/`: SAGE26's full end state on stable disks and disks with no cold gas, and on gas-unstable disks the state after the stellar transfer and the handed fraction, both from the oracle's transcription of `:54-101`, which the oracle checks against SAGE26 by composition with SAGE26's own continuation. The handed fraction is pinned to that checked transcription; no fixture validates it two-sided, because SAGE26's float stores downstream absorb a one-ulp change. On the gas-unstable disks the test then runs the package's continuation (`sage26_quasar_mode` and `sage26_starburst_feedback`, `process_by_galaxy`) and compares SAGE26's full end state, every recorded field and history element of every galaxy, bit for bit. The c08 `di_r2_*` fixtures run it with `SFprescription 1` (R2): the continuation's black-hole growth caps the hydrogen and its burst caps and refreshes it, and the test also asserts the galaxy's state after `sage26_quasar_mode` against the oracle's state after SAGE26's own `grow_black_hole`. The c09 `di_fire_*` fixtures (routine `check_disk_instability_fire`, R3's flags) run the continuation's burst with the fixture's `FIREmodeOn` (1, or 0 for the `di_fire0_*` twin) and its stored-snapshot redshifts: the test asserts every galaxy's `MassLoading` unchanged (no burst writes it) and the instability burst's FIRE wiring, through a replay of `sage26_starburst_feedback`'s call of the shared transaction on copies, required to end in `process()`'s state, whose FIRE record shows the bursting galaxy's own stored `SnapNum`, float `Vvir` and that snapshot's redshift, never its FoF central's. The test gives the burst module its required `FIREmodeOn` on every fixture (0 before c09).
