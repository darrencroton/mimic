# `sage26_calculate_star_formation`

Calculates the stellar mass formed during the current substep with SAGE26's disk star formation, the Blitz & Rosolowsky (2006) molecular gas with radial integration (`SFprescription 1`, the run files' value, SAGE26's R2) or the Croton et al. (2006) critical-mass law (`SFprescription 0`, R0 and R1), and stores it in a transport field for the feedback and apply steps. It also writes SAGE26's H I diagnostic, `H1gas`, and, with `SFprescription 1`, `H2gas`.

## SAGE26 Source

The module ports the start of SAGE26's `starformation_and_feedback()` (`src/model_starformation_and_feedback.c` in the staged SAGE26 source), the law selected by `SFprescription` as SAGE26's switch selects it (`:1015-1017`):

- `SFprescription 0`, `sfr_croton06` (`:160-184`): `reff = 3.0 * DiskScaleRadius`, `tdyn = reff / Vvir`, `cold_crit = 0.19 * Vvir * reff`, and `strdot = SfrEfficiency * (ColdGas - cold_crit) / tdyn` when `ColdGas > cold_crit`. A galaxy whose own `Vvir` is not positive forms no stars, and SAGE26 never forms `tdyn` for it (`:170-171`). `StarFormingDiskFactor` enters exactly where SAGE26's double literal `SF_DISK_RADIUS_FRAC` (`3.0`, `:60`) does, and the run files hold `3.0`. `H2gas` is not written.
- `SFprescription 1`, `sfr_br06` (`:193-252`) with radial integration: the double `reff` and `tdyn` as above; a non-positive own `Vvir` writes `H2gas` 0 and forms no stars (`:208-210`); the float `h` and the float `rs_pc = DiskScaleRadius * 1e6 / h` (`:214-215`), non-positive writing `H2gas` 0 and forming no stars (`:216-218`); otherwise SAGE26's radial integration (`src/model_h2_chemistry.c:134-290`, `shared/sage_h2_physics.h`) writes `H2gas`, except on its early return for no cold gas, when the earlier `H2gas` stays; `H2gas` is capped at the double `ColdGas * 0.74` (`:239-242`, which clears an `H2gas` left from before when the cold gas is gone), and `strdot = SfrEfficiency * H2gas / tdyn` only when `H2gas > 0` and `tdyn > 0` (`:244-248`). There is no critical-mass threshold, and `H2DepletionTime_Gyr` is not written (`sfr_br06` passes no SFR-output pointer to the integration, `:222`).
- The H I block (`:1035-1046`), which runs on every ordinary (non-FFB) call: `atomicH = ColdGas * 0.74 - H2gas` (one fused multiply-subtract in SAGE26's build, an explicit `fma` here), floored at zero, times `1 - f_ion`, the neutral fraction of an exponential gas disk truncated at 0.5 Msun/pc^2 (`ionized_gas_fraction`, `:135-152`), stored in the float `H1gas`. Under `SFprescription 0` its `H2gas` is the inherited value, which that prescription never writes, and H I is not resynchronised after the depletion that follows (the commits' caps, `:1131-1135`, `:1169-1173`, run only for prescriptions that track H2), so `H1gas` can exceed the hydrogen left in `ColdGas`: a deliberate SAGE26 quirk. Under `SFprescription 1` its `H2gas` is the BR06 value just written, and `sage26_apply_star_formation_supernova`'s commits cap `H2gas` and `H1gas` afterwards.
- `stars = strdot * dt`, floored at zero (`:1048-1051`), stored in `NewStellarMass`.

`Vvir` is read as the float SAGE26 holds; the core's double `Vvir`, the substep `dt` and the snapshot-zero interval are core-owned.

## Processing Contract

- Supported mode: `process_by_galaxy`
- Expected phase: `galaxy_physics`, before `sage26_calculate_supernova_feedback` and `sage26_apply_star_formation_supernova`
- Receives one galaxy at a time
- A row whose `FFBRegime` is 1 (written by `sage26_determine_ffb_regime` only with `EnhancedStarFormationOn` 1) is not processed: SAGE26 hands it to `starformation_ffb` and returns before its ordinary chain (`R/src/model_starformation_and_feedback.c:1004-1008`), so the module returns at once and writes nothing, before its time guard. `sage26_ffb_star_formation` runs that row's transaction. The module reads no new parameter: `FFBRegime` is 1 only where the classifier wrote it.
- The time guard (`mimic_object_substep_dt`, `shared/time_parity.h`): in the core's first-snapshot boundary state (stored `SnapNum` negative and `dT` not positive), where SAGE26 always has a positive interval, the module returns at once and writes nothing; any other non-positive interval is an error.

## Ordering

**Enforced at init (fails with ERROR if violated):**

1. `sage26_apply_star_formation_supernova` must be present somewhere in the pipeline (`module_configured_anywhere`: any phase, any processing mode) — without it, `NewStellarMass` is computed each substep but never committed to galaxy reservoirs (silent output loss).

**Not refused by this module's `init()`:**

- `sage26_apply_star_formation_supernova` in another phase or processing mode, or before this module: this check tests presence only. That module's own `init()` refuses it unless this module precedes it in the same substep phase, both as `process_by_galaxy`.
- `sage26_calculate_supernova_feedback` absent, or placed anywhere: this module does not check it. That module's own `init()` requires this module to precede it in the same substep phase when this module is configured.
- `sage26_ffb_star_formation` absent: this module does not check it. With `EnhancedStarFormationOn` 1, `sage26_determine_ffb_regime`'s `init()` refuses a configuration without it in a substep phase, and its own `init()` requires it immediately before this module.

## Properties

- Reads: `FFBRegime` (1 returns at once), `Type`, `HaloNr`, `SnapNum`, `dT`, `Vvir`, `ColdGas`, `DiskScaleRadius`, `H2gas`, and with `SFprescription 1` `StellarMass` and `BulgeMass` (the disk stars of the radial integration)
- Writes: `NewStellarMass`, `H1gas`, and with `SFprescription 1` `H2gas`

## Parameters

- `SfrEfficiency` (required, within [0, 1]; the run files hold 0.05, SAGE26's `input/millennium.par:66`)
- `StarFormingDiskFactor` (required, within [0, 10]; the run files hold 3.0, SAGE26's `SF_DISK_RADIUS_FRAC`, `src/model_starformation_and_feedback.c:60`)
- `SFprescription` (required, 0 or 1): the star-formation law, 1 (BR06 with radial integration, the run files' value and SAGE26's default, `input/millennium.par:48`; configurations R2–R6 set it) or 0 (Croton, R0 and R1)
- `H2RadialIntegrationOn` (required, 1), `H2RadialNBins` (required, 25), `H2RadialRMaxFactor` (required, 5.0): the radial integration as SAGE26's defaults configure it (`src/core_read_parameter_file.c:99-101`); the single-slab branch and its `H2DiskAreaOption` are not ported

Each of the last four is loaded with the required-parameter path: a missing key, or any value but the supported ones (also when `SFprescription` is 0), fails initialisation with an error naming the parameter.

## Notes

`NewStellarMass` is a transport field. The persistent stellar reservoir is updated later by `sage26_apply_star_formation_supernova`.

## Tests

`_tests/test_unit_sage26_calculate_star_formation_oracle.c` is the chained oracle test of the whole disk star-formation chain: it runs this module, `sage26_calculate_supernova_feedback`, `sage26_apply_star_formation_supernova`, `sage26_disk_instability`, `sage26_quasar_mode` and `sage26_starburst_feedback` (the instability's black-hole growth and burst, run only on the `SFprescription 1` fixtures whose disk is gas-unstable after feedback, `sf_br06_r2_*_unstable`, whose state after the black-hole cap the test also asserts) and `sage26_apply_metal_enrichment` through their `process()` entries on every `starformation_and_feedback` fixture under `_tests/oracle/`, single calls and whole-snapshot sequences, and asserts bit-for-bit equality with the SAGE26 function-level oracle (`../_tests/oracle/README.md`), including every element of the six per-substep histories (after each single call and after the last substep of each sequence) and each substep's history bin. After the last substep of a sequence it runs `sage26_finalize_sfr_histories` and asserts each galaxy's `SubstepsUsed` and SAGE26's writer averages (`SfrDisk`, `SfrBulge`, `SfrDiskZ`, `SfrBulgeZ`) with their divisors. Its file comment lists the SAGE26 locals it does not assert, and why. On the `SFprescription 1` fixtures it also asserts the size block, `H2DepletionTime_Gyr` and the values and flags of the oracle README's "The c08 Group" (the BR06 branch through a replay of the shared H2 helper, the handed-on `H2gas` and `H1gas`, and the commits' cap bounds); and it runs the module's fixtures of the two public H2 routines (`br06_*`, `radial_*`) through the shared helpers, asserting the return, `H2gas`, every local and every annulus bit for bit. On the `FIREmodeOn 1` fixtures of the oracle README's "The c09 Group" (`fire_*`, `sf_fire_*`) it also asserts `MassLoading` and replays the feedback module's FIRE budget through the shared helpers. On the `CGMrecipeOn 1` fixtures (`cgm_sf_*`, routine `starformation_and_feedback_fire` with configuration R4's flags, the oracle README's "The c10 Group, Part 3": real R4 FoF groups at snapshots 32 and 63 and constructed probes of the ejection cap, the retained yield and the gas-unstable continuation) it sets each galaxy's `Regime` and CGM pair from the fixture and asserts every routing site of the disk path (the disk commit and the disk yield by the FoF central's `Regime`; on gas-unstable fixtures the instability's wind by the forming galaxy's and its burst's commit and yield by the central's) against both the oracle's selection and the routing galaxy's input `Regime`, with the unselected pairs unchanged. Outside the oracle environment (macOS arm64, Apple clang 21) it reports SKIP. `_tests/test_unit_sage26_calculate_star_formation.c` covers the Croton law above, at and below the critical mass, a zero disk radius and a zero `Vvir`, the efficiency's effect, the BR06 branch (a non-positive `Vvir` or disk radius, a stale `H2gas` cleared with no cold gas, `H2gas` within the hydrogen budget with the H2 law and `H2DepletionTime_Gyr` unchanged, no critical-mass threshold), the lifecycle and memory safety, and that an `FFBRegime` 1 row leaves itself and its FoF central unchanged. `_tests/test_integration_sage26_calculate_star_formation.py` runs Mimic with this module and `sage26_apply_star_formation_supernova` in `galaxy_physics` at `SFprescription 0` and checks that the run succeeds and writes a finite, non-negative `StarFormationRate`; that star-forming galaxies' `StarFormationRate` is below half their `ColdGas`, galaxies with `ColdGas` below 1e-6 form no stars and the total is below a tenth of the total cold gas; that `SfrEfficiency` 0.05 forms at least as much as 0.01; and that the run leaks no memory and finishes within 30 seconds.
