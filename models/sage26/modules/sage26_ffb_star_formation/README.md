# `sage26_ffb_star_formation`

Forms the stars of a feedback-free-burst (FFB) galaxy row, `FFBRegime` 1, in one by-galaxy transaction per substep: star formation from the cold gas at `EnhancedSFEfficiency` over the disk's dynamical time, the history sample and the star-formation commit, then the FFB supernova feedback renormalised against the gas left, its commit to the FoF central's pair, and the metal yield, with no disk instability.

## SAGE26 Source

A port of SAGE26's `starformation_ffb` (`src/model_starformation_and_feedback.c:1262-1368`) with its static `compute_sn_feedback_ffb` (`:901-972`). SAGE26's `starformation_and_feedback` calls it in place of its whole ordinary disk chain for a row whose `FFBRegime` is 1 when `EnhancedStarFormationOn` is at least 1, and returns (`:1004-1008`). In SAGE26's order:

1. `reff = SF_DISK_RADIUS_FRAC * DiskScaleRadius` (the float radius widened; the package's `StarFormingDiskFactor`, 3.0 in the run files) and `tdyn = (reff > 0.0 && Vvir > 0.0) ? reff / Vvir : 0.0` with the row's own float `Vvir` (`:1273-1274`);
2. `H2gas = 0`, then the H I block (`:1280-1292`): `H1gas = (ColdGas * 0.74 - H2gas, floored at 0) * (1 - f_ion)`, through `shared/sage_hi_physics.h`;
3. the stars from the cold gas (`:1297-1320`): 0 if `ColdGas`, `Vvir`, `reff` or `tdyn` is NaN or infinite; else, for a positive `tdyn` and gas, `strdot = EnhancedSFEfficiency * ColdGas / tdyn`, rejected if NaN, infinite or negative, `stars = strdot * dt`, capped at `ColdGas`, then rejected if NaN, infinite or negative; else 0;
4. the disk history sampled from these stars and the cold gas before any commit (`:1325-1327`), in the substep's bin, through `shared/sage_sfr_history.h`;
5. the cold-gas metallicity, then `update_from_star_formation` with these stars (`:1329-1330`), through `shared/sage_sf_feedback_physics.h`, its prescription-1 hydrogen caps included;
6. the package's `StarFormationRate` diagnostic (not a SAGE26 output) gains these stars over the snapshot interval, as `sage26_apply_star_formation_supernova` does for the ordinary path;
7. the FFB feedback budget (`:1344`, `shared/sage_ffb_feedback_physics.h`) against the cold gas the commit left, which may reduce a local copy of the stars and the reheated mass;
8. the reheated mass clamped again to the remaining cold gas (`:1346`);
9. `update_from_feedback` (`:1348`) with the metallicity of step 5, not recomputed, to the FoF central's pair by the central's `Regime` (`shared/sage_sf_feedback_physics.h`, the CGM routing included);
10. the yield from the renormalised stars (`:1353-1362`, `shared/sage_metal_yield_physics.h`);
11. no disk instability (`:1365-1367`) and no transport property: every value carried between the steps is a local of the call.

**SAGE26's quirks, kept.** "Feedback-free" still runs supernova feedback. The star formation and its history are committed before the feedback renormalises the stars, so `SfrDisk` and the locked `StellarMass` use the stars formed while the feedback and the yield may use fewer (the source comments "(or H2 for modes 6/7)" and "runs standard SN feedback", `:1257-1258`, are stale and not ported). The feedback reuses the metallicity formed before the star-formation commit. An invalid FIRE scaling (the row's own `Vvir <= 0`, or a negative redshift of its stored snapshot) leaves `MassLoading` as it was and forms no reheated mass and no ejection, where the ordinary path writes `MassLoading` 0. The BR06 law is bypassed and `H2gas` zeroed, but the prescription-1 hydrogen caps of the commits still apply, and `H2DepletionTime_Gyr` is not written. Only this disk call skips the instability; merger continuations are unchanged.

**Precision and rounding.** `reff`, `tdyn`, `strdot`, the stars, the metallicity and every feedback local are doubles; `Vvir`, `DiskScaleRadius` and the reservoirs are SAGE26's floats. Where SAGE26's object fuses a multiply and an add (the commits' four updates, the feedback commit's metal debit and credit, the yield's retained metals) the shared code writes an explicit `fma()`; where it computes a form other than its source text (the high-velocity `pow(x, -1.0)` as the division `1.0 / x`; the FFB atomic hydrogen as a plain multiply, which the shared H I block's fused form equals when `H2gas` is +0) the package writes or proves the object's form. The table, read from SAGE26's `model_starformation_and_feedback.o`, is in the oracle README (`../_tests/oracle/README.md`, "The c13 Group, Part 2").

## Processing Contract

- Supported mode: `process_by_galaxy`
- Expected phase: a substep phase, once, immediately before `sage26_calculate_star_formation` in the run files

`init()` refuses any placement but exactly one substep-phase entry as `process_by_galaxy`: an entry in `pre_timestep` or `post_timestep`, a second entry, or another processing mode, each naming the module. The check names no substep phase (phase names are run configuration). The guards that name other modules (the adjacency to `sage26_calculate_star_formation`, the classifier's presence) and the ordinary chain's bypass of FFB rows are a later port.

**Rows and states.** With `EnhancedStarFormationOn` 0 the module writes nothing. With 1 it writes nothing on a row whose `FFBRegime` is not 1 (SAGE26's dispatch, `:1004`). In the initial-boundary state (stored `SnapNum` -1 and `dT` 0, `MIMIC_OBJECT_TIME_SKIP_INITIAL`), where SAGE26 is undefined, it returns at once and writes nothing (campaign decision D37); any other invalid interval is an error. The FFB feedback reads the redshift of the row's stored snapshot, `ZZ[SnapNum]` (`:915`), through `mimic_sage_fire_stored_redshift`, which refuses a `SnapNum` outside the run's redshift table with an error naming it (campaign decision D35); the module looks it up, with the substep's history bin, before it writes anything. With no FoF central (a Mimic-only state: SAGE26 always has one) it follows `sage26_apply_star_formation_supernova`'s convention: it writes `H2gas`, `H1gas` and the history sample and commits nothing.

## Properties

- Reads: `dT`, `SnapNum`, the row's own `Vvir` (narrowed to SAGE26's float), the FoF central's `Mvir` (narrowed, for the yield), `FFBRegime`, `DiskScaleRadius`, the central's `Regime`
- Writes (FFB rows at `EnhancedStarFormationOn` 1 only): `H2gas`, `H1gas`, `ColdGas`, `MetalsColdGas`, `StellarMass`, `MetalsStellarMass`, the disk histories (`SfrDiskBin<k>`, `SfrDiskColdGasBin<k>`, `SfrDiskColdGasMetalsBin<k>`), `StarFormationRate`, `SupernovaOutflowRate`, `MassLoading` (valid FIRE scaling only), and the FoF central's selected pair (`HotGas`, `MetalsHotGas` or `CGMgas`, `MetalsCGMgas`) and `EjectedGas`, `MetalsEjectedGas`

## Parameters

- `EnhancedStarFormationOn`: required, with no default (campaign decision D33); 0 (nothing written; SAGE26 R0-R5, `R5.par:58`) or 1 (FFB star formation for `FFBRegime` 1 rows; R6, `R6.par:58`, SAGE26's default). Any other value is refused, naming it. The run files hold 0.
- `EnhancedSFEfficiency`: required, with no default; finite and within [0, 1] (`R6.par:67`, "1.0 is theoretical maximum"), else refused naming it (a NaN included). The run files hold 0.2 (`R6.par:67`; SAGE26's default, `src/core_read_parameter_file.c:104`).
- `FIREmodeOn`: required; at `EnhancedStarFormationOn` 1 only 1 is accepted, since FFB's fixed-epsilon branch (`:935-937`, `:960-965`) is on no rung (campaign decision D6); 0 is refused there, naming it.
- The existing keys the transaction reads, each with its existing validation: `StarFormingDiskFactor`, `SFprescription` (0 or 1), `RecycleFraction`, `CGMrecipeOn` (0 or 1), `FeedbackReheatingEpsilon`, `FeedbackEjectionEfficiency`, `Yield`, `FracZleaveDisk`. The run's `Hubble_h` and units come from the run configuration (SAGE26's `EtaSNcode` and `EnergySNcode`, `src/core_init.c:102-103`).

## Test Observer

`sage26_ffb_star_formation.h` declares `sage26_ffb_star_formation_set_test_observer()`. With an observer installed, `process()` reports each call that reaches the transaction after its last write: the path, `dt` and bin, the redshift, `reff`, `tdyn`, the star-formation branch flags, `strdot`, the stars formed, the metallicity, the commits' locals, the FFB budget's locals, the reheated mass before and after the clamp of `:1346` (the value passed to the feedback commit), the ejected mass and the yield's locals. It never changes what the module computes; a run never installs one.

## Tests

- `_tests/test_unit_sage26_ffb_star_formation.c`: platform-independent behavioural tests. Each runs `process()` and compares the row and its central byte for byte with SAGE26's `starformation_ffb` written out in the test (cited line by line, never calling the package's code), and asserts the named behaviour with literals or the observer: the committed stellar mass and the history use the stars formed and the yield the renormalised stars; the feedback uses the metallicity before the commit; an invalid FIRE scaling leaves a seeded `MassLoading` with no reheating or ejection; `H2gas` zeroed, `H1gas` the H I block's, `H2DepletionTime_Gyr` not written; the NaN/infinity guards and `tdyn` 0; a satellite's feedback and yield to its central's pair by the central's `Regime`; no instability or transport field written on a seeded unstable disk; the clamp of `:1346` (an observer assertion on the reheated mass passed to the commit); `SFprescription` 0; no central; nothing written at `EnhancedStarFormationOn` 0, on an `FFBRegime` 0 row or in the initial-boundary state; D35's refusal; every parameter refusal and misplacement. Tests that target a misordering also show, through the written-out transaction run with that misordering, that their input discriminates it.
- `_tests/test_unit_sage26_ffb_star_formation_oracle.c` runs every fixture under `_tests/oracle/` (listed in `../_tests/oracle/fixtures.manifest`, written by `oracle_c13`) through `process()` and the shared helpers, bit for bit against the SAGE26 function-level oracle (`../_tests/oracle/README.md`, "The c13 Group, Part 2"); outside the oracle environment (macOS arm64, Apple clang 21) it reports SKIP.
- The shared helpers are tested by `shared/_tests/test_unit_sage_ffb_shared_helpers.c`.

## References

- Li et al. (2024), feedback-free starbursts, as SAGE26 cites it (`src/model_starformation_and_feedback.c:1266`)
- Muratov et al. (2015), the FIRE mass loading; Hirschmann et al. (2016), the energy-based ejection, as the package's FIRE helpers cite them
