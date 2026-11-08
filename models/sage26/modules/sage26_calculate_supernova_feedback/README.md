# `sage26_calculate_supernova_feedback`

Calculates supernova reheating and ejection budgets from newly formed stellar mass, as SAGE26's non-Karpov `compute_sn_feedback()` does (`src/model_starformation_and_feedback.c:732-889` in the staged SAGE26 source, `SupernovaRecipeOn 1`), for either value of the required parameter `FIREmodeOn`.

With `FIREmodeOn 0` (SAGE26's R0-R2, the fixed-epsilon Croton et al. 2016 feedback):

- `reheated = FeedbackReheatingEpsilon * stars` (`:767`);
- when `stars + reheated` exceeds `ColdGas`, both are scaled together by `ColdGas / (stars + reheated)` (`:775-779`), against the full sum, before any recycling;
- `ejected = (FeedbackEjectionEfficiency * (EtaSNcode * EnergySNcode) / (Vvir * Vvir) - FeedbackReheatingEpsilon) * stars` with the forming galaxy's **own** `Vvir`, also for satellites and orphans, squared in float as SAGE26's float member is (`:783`, `:795-798`); zero for a non-positive own `Vvir` (`:876-878`), and floored at zero (`:880-882`). `MassLoading` is not written.

With `FIREmodeOn 1` (SAGE26's R3–R6), through the helpers of `shared/sage_fire_feedback_physics.h`:

- the Muratov et al. (2015) scaling `(1 + z)^1.25 * (max(Vvir, 1) / 60)^beta`, `beta` -3.2 below 60 km/s and -1.0 at and above, from the forming galaxy's **own** float `Vvir` widened to double and the redshift of its **stored** snapshot, `ZZ[SnapNum]` (`:743-753`), never the current halo snapshot's or a substep's; zero for a non-positive `Vvir` or a negative redshift. A stored `SnapNum` outside the run's redshift table is refused with an error naming it and the table size (SAGE26 reads `ZZ[SnapNum]` without a bounds check; `mimic_sage_fire_stored_redshift` in `shared/sage_fire_feedback_physics.h`), except in the core's first-snapshot boundary state, where the module returns before the lookup (see the Processing Contract);
- the reheating loading `FeedbackReheatingEpsilon * scaling`, capped at `2 * EtaSNcode * EnergySNcode / Vvir^2` in double (`capped_eta_reheat`, `src/model_misc.h:141-149`; unchanged for a non-positive `Vvir`), stored as the float `MassLoading` **before** the renormalisation on every ordinary (non-FFB) call, also when no stars form (`:762-764`), and `reheated = eta * stars` (`:765`);
- the same renormalisation (`:775-779`);
- the Hirschmann et al. (2016) energy-based ejection from the renormalised stars and reheated mass: the coupling `FeedbackEjectionEfficiency * scaling` capped at 2 (`sn_energy_coupling`, `src/model_misc.h:118-124`), `E_FB = coupling * 0.5 * stars * (EtaSNcode * EnergySNcode)`, `E_lift = 0.5 * reheated * Vvir * Vvir` and `ejected = (E_FB - E_lift) / (0.5 * Vvir * Vvir)` when `E_FB > E_lift`, else 0 (`:786-792`), in double; zero for a non-positive own `Vvir` (`:876-878`), then the same floor at zero (`:880-882`), which the FIRE ejection never reaches below zero.

The 1 km/s floor applies to the scaling only, never to the loading cap or the energies. The two caps are distinct: the loading is capped by the whole supernova budget, the ejection's coupling by the scaled efficiency, so the reheating can consume all the coupled energy and leave nothing to eject. SAGE26's object computes the high-velocity term `pow(x, -1.0)` as the division `1.0 / x`, which the package writes as such (the shared header and the oracle README's "The c09 Group", "Fused Multiply-Adds and the Simplified pow", give the evidence).

The scaling and the ejection use the forming galaxy's own `Vvir` and stored `SnapNum`; the reheated gas still goes to its FoF central's hot gas and the ejection is capped against that central's hot gas, in `sage26_apply_star_formation_supernova`. `EtaSNcode` and `EnergySNcode` are SAGE26's code constants (`src/core_init.c:102-103`), derived by the shared helpers in `shared/sage_sf_feedback_physics.h`. `FeedbackReheatingEpsilon` is 2.9 and `FeedbackEjectionEfficiency` 0.3 in every configuration R0–R6, as SAGE26's `input/millennium.par:69-70` sets them (also its defaults, `src/core_read_parameter_file.c:120-121`).

## Processing Contract

- Supported mode: `process_by_galaxy`
- Expected phase: `galaxy_physics`, after `sage26_calculate_star_formation` and before `sage26_apply_star_formation_supernova`
- Receives one galaxy at a time
- A row whose `FFBRegime` is 1 (written by `sage26_determine_ffb_regime` only with `EnhancedStarFormationOn` 1) is not processed: SAGE26 hands it to `starformation_ffb` and returns before its ordinary feedback budget (`R/src/model_starformation_and_feedback.c:1004-1008`), so the module returns at once and writes nothing, before its time guard (no `MassLoading`, no transport field). `sage26_ffb_star_formation` runs that row's transaction. The module reads no new parameter: `FFBRegime` is 1 only where the classifier wrote it.
- With `FIREmodeOn 1`, in the core's first-snapshot boundary state (stored `SnapNum` negative and `dT` not positive, `MIMIC_OBJECT_TIME_SKIP_INITIAL` in `shared/time_parity.h`), where SAGE26's read of `ZZ[SnapNum]` would be undefined, the module returns at once and writes nothing, `MassLoading` included, as `sage26_calculate_star_formation` does. With `FIREmodeOn 0` it has no time guard and forms the budgets from the `NewStellarMass` it reads.

## Ordering

**Enforced at init (fails with ERROR if violated):**

1. `sage26_apply_star_formation_supernova` must be present somewhere in the pipeline (`module_configured_anywhere`: any phase, any processing mode) — without it, `SupernovaReheatedMass` and `SupernovaEjectedMass` are computed each substep but never committed to galaxy reservoirs (silent output loss).
2. Only when `sage26_calculate_star_formation` is configured (in any phase): it must precede this module in the same substep phase, both as `process_by_galaxy` (`module_precedes_in_substep_phase`) — SN feedback reads `NewStellarMass` written by SF; wrong order applies stale values from the previous substep.

**Not refused by this module's `init()`:**

- `sage26_calculate_star_formation` absent entirely: check 2 is skipped, and the module reads whatever `NewStellarMass` holds.
- `sage26_apply_star_formation_supernova` in another phase or processing mode, or before this module: check 1 tests presence only. That module's own `init()` refuses it unless this module precedes it in the same substep phase, both as `process_by_galaxy`.
- `sage26_ffb_star_formation` absent: this module does not check it (with `EnhancedStarFormationOn` 1, `sage26_determine_ffb_regime`'s `init()` refuses that configuration).

## Properties

- Reads: `FFBRegime` (1 returns at once), `Vvir` (the forming galaxy's own), `ColdGas`, `NewStellarMass`, and with `FIREmodeOn 1` `SnapNum` (the stored snapshot) and `dT` (the initial-boundary test); `HaloNr` and `Type` in log messages only
- Writes: `NewStellarMass`, `SupernovaReheatedMass`, `SupernovaEjectedMass`, and with `FIREmodeOn 1` `MassLoading`

## Parameters

- `FeedbackReheatingEpsilon` (required, finite and >= 0, the range every reader shares (`mimic_sage_validate_feedback_reheating_epsilon`, `shared/sage_sf_feedback_physics.h`); SAGE26 does not range-check it; the run files hold 2.9)
- `FeedbackEjectionEfficiency` (required, within [0, 100]; the run files hold 0.3)
- `FIREmodeOn` (required, 0 or 1, no default): 1, the run files' value and SAGE26's default (`input/millennium.par:55`; configurations R3–R6 set it), selects the FIRE budgets, 0 (R0-R2) the fixed-epsilon ones. A missing key fails initialisation and any other value is refused, naming the parameter. `sage26_starburst_feedback` reads the same parameter for every burst's budget (`src/model_mergers.c:690-752`).

## Notes

This module only calculates transport budgets. Reservoir changes are committed by `sage26_apply_star_formation_supernova`. `NewStellarMass` can be lowered here when the combined SF + reheating budget would exceed the available `ColdGas`; the apply step consumes the capped value.

## Tests

- `_tests/test_unit_sage26_calculate_supernova_feedback_oracle.c` runs every fixture under `_tests/oracle/` (listed in `../_tests/oracle/fixtures.manifest`; all `FIREmodeOn 1`, routine `starformation_and_feedback_fire` with configuration R3's flags, the oracle README's "The c09 Group") through the disk chain's `process()` entries in order (`sage26_calculate_star_formation`, this module, `sage26_apply_star_formation_supernova`, `sage26_disk_instability`, `sage26_apply_metal_enrichment`) and asserts bit for bit against the SAGE26 function-level oracle (`../_tests/oracle/README.md`): at this module's boundary the stars it receives, the budgets it hands on before and after the renormalisation and the floor, its branch flags, `MassLoading`, and the FIRE flags and values from a replay of its budget through the shared helpers, which must end in the state `process()` produced; and every galaxy's end state. Outside the oracle environment (macOS arm64, Apple clang 21) it reports SKIP. The `FIREmodeOn 0` twins are among `sage26_calculate_star_formation`'s fixtures, whose chained oracle test asserts the whole chain.
- `_tests/test_unit_sage26_calculate_supernova_feedback.c`: registration, initialisation, parameter reading and memory safety; the fixed-epsilon reheating, the renormalisation (triggered and at its boundary), the ejection and its floor at zero; zero stars, zero `Vvir`, a missing galaxy and very little cold gas; and `FIREmodeOn`: the missing key and the values 2, 3 and -1 refused (naming it) with 0 and 1 accepted, 1 against 0 on the same galaxy, the scaling's guards, floor and slopes, the high-velocity division, the reheating cap and `MassLoading` (also with no stars), the energy-based ejection, the initial-boundary skip, the refusal of a stored `SnapNum` outside the redshift table (also a negative one with a positive `dT`), each FIRE expectation derived without the shared helpers (literal inputs with exact results, or SAGE26's own expressions written in the test); and that an `FFBRegime` 1 row leaves itself and its FoF central unchanged.
- `_tests/test_integration_sage26_calculate_supernova_feedback.py` runs Mimic with `sage26_calculate_star_formation`, this module and `sage26_apply_star_formation_supernova` in `galaxy_physics` at `SFprescription 0` and `FIREmodeOn 0` and checks that the run succeeds with default and custom efficiencies and writes `StarFormationRate` and `SupernovaOutflowRate`; that it leaks no memory; that `SupernovaOutflowRate` is non-negative, positive where `StarFormationRate` is positive and zero where it is zero, and at most 100 times `StarFormationRate`; that it stays non-negative and below 10 with both efficiencies 0, and non-negative and finite with very high ones; and that higher efficiencies give a higher mean outflow where both runs have some.
