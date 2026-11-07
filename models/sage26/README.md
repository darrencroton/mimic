# sage26 Model Package

**sage26** is a model package for Mimic that ports SAGE26 (`sage-home/SAGE26` at commit `11b1ae851524baadb76cfd17e0b38f8a89c07224`; Bradley et al. 2026, in preparation), which its own README describes as a major update to Croton et al. (2016), the SAGE model. Mimic's physics-agnostic core handles tree traversal, memory, configuration, validation and output, while this package owns the baryonic physics, as runtime modules that reproduce SAGE26's routines and their arithmetic.

The package is independent: it shares `sage16`'s lineage, but nothing in it depends on anything under `models/sage16`. It is not the build default (that is `sage16`), so every `make` command below names `MODEL=sage26`, except the cross-format gate's (see [Build, Run, and Test](#build-run-and-test)). For the general model-package concepts, see the [Developer Guide](../../docs/DEVELOPER-GUIDE.md); for running and configuring a model, see the [User Guide](../../docs/USER-GUIDE.md).

## Scientific Scope

The package follows the baryonic reservoirs that [`model_properties.yaml`](model_properties.yaml) declares on dark-matter merger trees: hot gas, the circumgalactic medium (`CGMgas`), cold gas (with its molecular and atomic hydrogen, `H2gas` and `H1gas`), ejected gas, stellar mass, the bulge (with its merger and instability channels, `MergerBulgeMass` and `InstabilityBulgeMass`), the intracluster stars (`ICS`) and the black hole, each gas and stellar reservoir with its metals.

The run files configure SAGE26's default configuration (R6 in [SAGE26 Configurations R0–R6](#sage26-configurations-r0r6)). By subsystem:

- **The baryon cycle inherited from Croton et al. (2006, 2016):** reionization, gas infall, reincorporation of ejected gas, satellite stripping, radiative cooling with radio-mode AGN heating (`AGNrecipe 2`, Bondi-Hoyle accretion), star formation, supernova feedback, disk instabilities, black-hole growth and quasar-mode winds, starbursts, mergers and satellite disruption into the intracluster stars.
- **Bulge sizes, `BulgeSizeOn 3`:** the Tonini et al. (2016) merger and instability bulges, each with its own mass and radius.
- **Star formation, `SFprescription 1`:** the Blitz & Rosolowsky (2006) molecular fraction, integrated radially over the exponential disk (`H2RadialIntegrationOn 1`, 25 bins out to 5 disk scale radii), with stars formed from the molecular gas (`H2gas`).
- **Supernova feedback, `FIREmodeOn 1`:** the Muratov et al. (2015) FIRE mass loading, scaled with redshift and the galaxy's own virial velocity and capped, with the Hirschmann et al. (2016) energy-based ejection, on the disk path and in every starburst.
- **The two-regime CGM, `CGMrecipeOn 1`:** once per snapshot every galaxy is classified as CGM regime (`Regime` 0) or hot-halo regime (`Regime` 1) by a sigmoid 0.1 dex wide about the Dekel & Birnboim (2006) shock mass (`MShockMsun`), against a uniform deviate (see [Departures from SAGE26](#departures-from-sage26)). Infall, reincorporated gas and supernova-reheated gas go to the receiving central's CGM reservoir when it is in the CGM regime and to its hot gas otherwise, and cooling follows SAGE26's regime-aware recipe: bulk cooling of the CGM, and hot-halo cooling with cold streams. SAGE26's README cites Dekel & Birnboim (2006) and Voit (2015) for the model and Carr et al. (2023) for the recipe it follows.
- **Halo concentration, `ConcentrationOn 3`:** each new FoF central's NFW (Navarro, Frenk & White 1997) concentration, inverted from `Vmax/Vvir` at progenitor joining, from the infall-frozen velocities of a galaxy that was a satellite. `Concentration` is an output only: no other module reads it.
- **Feedback-free bursts, `EnhancedStarFormationOn 1`:** once per snapshot every galaxy is classified as a feedback-free-burst row (`FFBRegime` 1) with the probability of the Li et al. (2024) sigmoid about their threshold halo mass. An FFB row forms its stars from the cold gas at efficiency `EnhancedSFEfficiency` over the disk's dynamical time, with FIRE feedback and no disk instability, in place of the ordinary star-formation chain.

The package does not port SAGE26's other options. `AGNrecipe` 1 and 3 are accepted, but the empirical and cold-cloud accretion rates they select are not SAGE26's forms; they are the same expressions as `sage16`'s (see [Departures from SAGE26](#departures-from-sage26)). Each value below is refused at initialisation with an error naming the parameter, by the modules that read it:

| Parameter | Refused values | SAGE26 option not ported |
|---|---|---|
| `SFprescription` | anything but 0 and 1 | Prescriptions 2–7 |
| `H2RadialIntegrationOn`, `H2RadialNBins`, `H2RadialRMaxFactor` | anything but 1, 25 and 5.0 respectively | The single-slab molecular fraction and other integration grids |
| `StarburstColdGasOn` | anything but 1 | Bursts fuelled by the molecular gas |
| `BulgeSizeOn` | anything but 0 and 3 | Modes 1 and 2 |
| `ConcentrationOn` | anything but 0 and 3 | Mode 1 (the lookup table) and mode 2 (`Vmax/Vvir` for every galaxy) |
| `EnhancedStarFormationOn` | anything but 0 and 1 | Mode 2, the acceleration criterion |
| `FIREmodeOn` | 0 when `EnhancedStarFormationOn` is 1 (by `sage26_ffb_star_formation`) | The fixed-efficiency feedback of feedback-free bursts |

SAGE26's adaptive substep count has no package parameter: see [Runtime Pipeline](#runtime-pipeline).

## Departures from SAGE26

The package reproduces SAGE26's routines and arithmetic except where the code differs in the ways below.

- **Random draws.** SAGE26 draws the CGM regime classifier's deviate from libc's process-global `rand()` stream (SAGE26 `src/model_regimes.c:94`), so a galaxy's draw depends on every earlier draw of the run. The package replaces it with a keyed deviate that is a pure function of the galaxy's `UniqueGalaxyID`, the snapshot, a purpose salt and an ordinal ([`shared/sage_keyed_draw.h`](shared/sage_keyed_draw.h)). Both drivers build `UniqueGalaxyID` identically, so the vertical and horizontal drivers classify a galaxy alike. The feedback-free-burst classifier uses SAGE26's own draw, the SplitMix64 hash of the halo's `MostBoundID` and the snapshot (SAGE26 `src/model_regimes.c:151-159`), which [`shared/sage_ffb_regime.h`](shared/sage_ffb_regime.h) transcribes. The package consumes no global random stream, so SAGE26's other `rand()` calls, whose values it discards (two at each galaxy's creation, SAGE26 `src/model_misc.c:91-92`, and one per feedback-free-burst classification, `src/model_regimes.c:224`), have no counterpart.
- **`AGNrecipe` 1 and 3 are not SAGE26's.** Of the radio-mode accretion rates, only SAGE26's Bondi-Hoyle branch (`AGNrecipe 2`) is ported. `AGNrecipe 1` (empirical) and `AGNrecipe 3` (cold cloud) are accepted, but their rates are not SAGE26's forms (`sage26_radio_mode_heating` README, *SAGE26 Source*; `mimic_sage_agn_rate_empirical` and `mimic_sage_agn_rate_cold_cloud` in `shared/sage_cooling_physics.h`). They are the same expressions as `sage16`'s `calculate_agn_rate_empirical` and `calculate_agn_rate_cold_cloud` (`models/sage16/modules/sage_radio_mode_heating/sage_radio_mode_heating.c`).
- **Mimic's core is kept where SAGE26's equivalent differs.** The package does not change the core; the differences the package's documents record are:
  - the substep count: the run files use the core's fixed 10 substeps, while SAGE26 chooses its step count per halo between a floor and a cap (`sage26_finalize_sfr_histories` README, *Differences From SAGE26*);
  - the storage precision of eight core properties, `dT`, `Mvir`, `CentralMvir`, `Rvir`, `Vvir`, `infallMvir`, `infallVvir` and `infallVmax`: the core types them double (`src/core/core_properties.yaml`) and writes them as float64, where SAGE26 writes them as float32. SAGE26 declares seven of them float in its galaxy struct (SAGE26 `src/core_allvars.h:123`, `:129`, `:131-132`, `:198-200`) and computes `CentralMvir` as it writes it (SAGE26 `src/io/save_gals_hdf5.c:900`). Of the core mappings in `instrument/field_map.yaml`, these eight alone record float32 against float64;
  - the previous `Type`: the core does not publish it, so `sage26_record_transition_state` records it for the next snapshot's joining, where SAGE26's joining reads the old `Type` before writing the new one (that module's README, *Differences From SAGE26*);
  - a Type 1 with a merger clock above 999 that loses its subhalo: SAGE26 records its `infallMvir`, `infallVvir` and `infallVmax`, the core only for a previous Type 0 (`sage26_initialise_merger_clock` README);
  - orphan `Len`: the core zeros a new orphan's `Len`, which SAGE26 keeps (`sage26_initialise_merger_clock` README; the instrument README, rule 4);
  - the rows written: which rows Mimic writes, orphans included, is the core's (`sage26_finalize_sfr_histories` README);
  - the particle mass: mini-Millennium's is 0.0860657 in `simulations/mini-millennium/simulation_info.yaml` and 0.086 in SAGE26's output header (the instrument README, *Scope*).
- **Two package behaviours differ from SAGE26's.** The modules' READMEs record these as package differences, not core ones:
  - a new orphan's `Cooling`, `Heating`, `QuasarModeBHaccretionMass` and outflow rate (`SupernovaOutflowRate`): SAGE26 resets them only on the main branch (SAGE26 `src/core_build_model.c:273-276`), so a new orphan keeps them, while the package's `init_repeat` resets them on every inherited row. None of the four is read by the merger-clock pass or moved by a merger or disruption, and an orphan is removed within its snapshot, so on R0 no written row carries the difference (`sage26_initialise_merger_clock` README, *Differences From SAGE26*);
  - the output rule of `infallStellarMass` and `TimeOfInfall`: SAGE26 applies it when it writes a galaxy, while `sage26_record_transition_state` applies it once per snapshot to every galaxy row, so a row the module has not processed keeps the creation value -1 (that module's README, *Differences From SAGE26*).
- **Undefined reads are refused.** Where SAGE26 reads its redshift table at a galaxy's stored `SnapNum` without a bounds check, a stored `SnapNum` outside the table is refused with an error naming it, and the module's `process()` fails (`sage26_calculate_supernova_feedback`, `sage26_starburst_feedback`, `sage26_ffb_star_formation` and `sage26_regime_aware_cooling`, each through `mimic_sage_fire_stored_redshift` in `shared/sage_fire_feedback_physics.h`). In the core's first-snapshot boundary state (stored `SnapNum` -1, no positive interval), which SAGE26 does not have (it always has a positive interval there, as the `sage26_finalize_cooling_heating` README states) and where its reads would be undefined, each module takes the path its README gives. Among them, `sage26_calculate_cooling_budget` runs no cooling and writes the return path -1, `sage26_regime_aware_cooling` and `sage26_ffb_star_formation` write nothing, `sage26_apply_star_formation_supernova` samples no history, and `sage26_finalize_cooling_heating` sets `Cooling`, `Heating` and `SupernovaOutflowRate` to 0 on every non-merged galaxy of a FoF whose first galaxy is in that state.
- **Parameters have no defaults.** Every package parameter is required, with no default, except `MergerTimeFactor`, which is optional and defaults to 2.0, SAGE26's default (`sage26_initialise_merger_clock.c`, whose README states it). See [Parameters](#parameters).
- **The output contract.** [`instrument/field_map.yaml`](instrument/field_map.yaml) pairs each SAGE26 per-snapshot dataset it maps with a package, core or simulation property, and records for each pairing the two names, the two storage types and a factor of 1. For the core and simulation pairings only, it also records a unit check (`unit_check`) of SAGE26's written unit against Mimic's output unit, and each finds them equal. It records no unit check for the package pairings; their unit is the one each property's metadata in `model_properties.yaml` states. The map records the reason for each dataset it leaves uncompared. Where SAGE26's own label for a value is wrong, the property's metadata states the value's true unit and meaning (for example `SfrDiskZ` and `SfrBulgeZ`, dimensionless metallicities that SAGE26 labels Msun/yr, and `mdot_cool` and `mdot_stream`, written in Msun/yr where SAGE26's label says 1.0e10 Msun/yr). This README claims no numerical agreement with SAGE26's output. Because of the random-draw departure above, a galaxy's CGM regime, and everything downstream of it, is not SAGE26's realisation by construction. See [Output Contract](#output-contract).
- **Deliberate reproductions are marked.** Where the code reproduces SAGE26's arithmetic or a quirk of it deliberately, it carries a comment with the marker `SAGE parity:`, in any of three forms: a C line comment (`// SAGE parity:`), a C block comment (`/* SAGE parity:`, or `* SAGE parity:` on a continuation line) or a YAML comment (`# SAGE parity:`) in `model_properties.yaml`; in some comments the marker follows other text. `git grep -n 'SAGE parity:' -- models/sage26 ':!*.md'`, which leaves out Markdown prose that quotes the marker, lists 313 lines: 74 in `model_properties.yaml` (pathspec `models/sage26/model_properties.yaml`), 20 in the C test sources under `_tests/` directories (pathspecs `'models/sage26/**/_tests/**' ':!*.md'`), 4 of which are test messages that quote the marker in a string literal rather than comments, and 219 in the other C sources and headers (106 in `.c` files and 113 in `.h`).

## Agreement with SAGE26

This section gives the methods by which the package is compared with SAGE26; this README quotes no galaxy-level agreement figure.

**The function-level oracle** ([`modules/_tests/oracle/README.md`](modules/_tests/oracle/README.md)). SAGE26's own physics sources, compiled unchanged, are linked into ten small oracle mains (`oracle_c03.c` to `oracle_c13.c`; there is no `oracle_c09.c`) that call one SAGE26 routine on an explicit input and record what it computed. The fixtures they write live with the module they test, under `modules/<module>/_tests/oracle/`: `modules/_tests/oracle/fixtures.manifest` lists 1044, owned by 24 modules. Each module's oracle unit test runs the same inputs through its `process()` entry or shared helpers and compares the outputs bit for bit. The fixtures were generated on macOS 27.0.1 (build 26A434) on arm64 with Apple clang 21.0.0 (clang-2100.4.1.1), target `arm64-apple-darwin27.0.0`, the environment of the SAGE26 reference build (the oracle README, *Oracle Environment*). The oracle tests compare only where `oracle_environment_mismatch()` in `modules/_tests/sage26_oracle_fixture.h` returns NULL, which it does only inside `#if defined(__APPLE__) && defined(__aarch64__) && defined(__apple_build_version__)` and the nested `#if __clang_major__ == 21`; elsewhere it returns a reason, and the tests report SKIP.

**The galaxy-by-galaxy instrument** ([`instrument/README.md`](instrument/README.md)). `instrument/compare_sage26.py` compares a `sage26` HDF5 run with a SAGE26 reference output galaxy by galaxy at the run's eight snapshots, through `instrument/field_map.yaml`, and gives a `pass`, `fail` or `error` verdict; its matching rules, tolerances and thresholds are those of the instrument README, *Comparison rules*. `instrument/controls/` holds the known-good and known-bad controls that check it.

**Core alignment** ([`instrument/core-alignment/README.md`](instrument/core-alignment/README.md)). This is scaffolding for a core-aligned measurement. `apply.sh` applies the patches that `instrument/core-alignment/series` names, in order, on a throwaway branch that is never merged; each patch would give Mimic's core one of SAGE26's behaviours. At this commit `series` is empty and no patch exists, so the set changes nothing. Nothing in it is a proposal to change the core.

## SAGE26 Configurations R0–R6

The labels R0 to R6 name SAGE26 configurations in this package's notes, READMEs, oracle fixture names and run-file comments. Each is SAGE26's `input/millennium.par` with only its paths changed and these flags set:

| Configuration | Flags | Adds |
|---|---|---|
| R0 | `SFprescription 0`, `CGMrecipeOn 0`, `FIREmodeOn 0`, `EnhancedStarFormationOn 0`, `ConcentrationOn 0`, `BulgeSizeOn 0` | SAGE26 with the six switches off |
| R1 | R0 with `BulgeSizeOn 3` | Tonini et al. (2016) bulge sizes |
| R2 | R1 with `SFprescription 1` | Blitz & Rosolowsky (2006) molecular star formation |
| R3 | R2 with `FIREmodeOn 1` | FIRE supernova feedback |
| R4 | R3 with `CGMrecipeOn 1` | The two-regime CGM |
| R5 | R4 with `ConcentrationOn 3` | Halo concentration |
| R6 | R5 with `EnhancedStarFormationOn 1` | Feedback-free bursts |

R6 is SAGE26's default configuration: `input/millennium.par` sets the first five flags as R6 does and leaves `BulgeSizeOn` at its default, 3 (SAGE26 `src/core_read_parameter_file.c:107`). Both run files are on R6.

## Package Contents

- `model_properties.yaml`: the package's galaxy property metadata: reservoirs, transport properties, SAGE26's diagnostics and outputs. Edit it when the galaxy state changes, then run `make MODEL=sage26 SIMULATION=mini-millennium generate`.
- `input/`: the run files, `sage26_mini-millennium.yaml` (vertical driver, L-Halo trees) and `sage26_mini-millennium-horizontal.yaml` (horizontal driver, the horizontal HDF5 conversion of the same trees).
- `modules/`: the 26 runtime modules. Each has its own directory with C source, `module_info.yaml`, README and `_tests/`, which holds its unit tests and, for 24 modules, its oracle fixtures and oracle test.
- `modules/_tests/`: the package's shared tests (processing-mode and dependency contracts, parameter refusals, mixed-interval parity, the oracle reader's self-test) and the shared test headers: `sage_test_fixtures.h`, whose `set_test_model_parameters()` is the standard parameter fixture, and the oracle readers `sage26_oracle_*.h`.
- `modules/_tests/oracle/`: the function-level oracle: `build.sh`, `regenerate.sh`, the oracle mains and `fixtures.manifest`.
- `shared/`: model-local helper headers (SAGE26's physics kernels, the keyed draw, constants), described in [`shared/README.md`](shared/README.md), with their unit tests in `shared/_tests/`. They are not framework APIs.
- `plots/figures/`: the package's diagnostic figures for `mimic-plot.py`.
- `plots/profiles/`: the plot profiles (`default.yaml`).
- `instrument/`: the galaxy-by-galaxy comparator, its field map and controls, and the core-alignment scaffolding (see [Agreement with SAGE26](#agreement-with-sage26)).

## Runtime Pipeline

The vertical run file, [`input/sage26_mini-millennium.yaml`](input/sage26_mini-millennium.yaml), lists the exact modules and their order; the horizontal run file has the same pipeline. It builds the pipeline as:

- `pre_timestep`, once per FoF group at the start of each snapshot, in SAGE26's order: progenitor joining (the merger clocks, the SFR-history reset and the infall records, then the central's halo concentration and disk radius), reionization, the CGM regime classifier, the FoF infall budget, and the feedback-free-burst classifier.
- `galaxy_physics`, a substep phase run once per substep: infall, reincorporation and satellite stripping; cooling and AGN heating (with `CGMrecipeOn 1` the classic budget writes only its no-cooling transport and `sage26_regime_aware_cooling` cools); the feedback-free-burst transaction, then the ordinary star formation, supernova feedback and their commit; disk instability with its quasar-mode and starburst consumers; and the disk star formation's metal yield.
- `satellite_mergers`, a substep phase run after `galaxy_physics`: the merger and disruption pass, with the quasar-mode and starburst modules consuming its merger events.
- `post_timestep`, once per FoF group after the last substep: the normalisation of the accumulated `Cooling` and `Heating`, the averaging of the SFR histories into `SfrDisk`, `SfrBulge`, `SfrDiskZ` and `SfrBulgeZ`, and the record of each galaxy's final `Type` with the outputs `infallStellarMass` and `TimeOfInfall`.

Ten transport properties, which are not written, carry a value from the module that computes it to the modules that use it, separating calculation from commit. In `model_properties.yaml` order, they are: `InfallingGas`, `CoolingGas`, `NewStellarMass`, `SupernovaReheatedMass`, `SupernovaEjectedMass`, `Rcool`, `CoolingX`, `CoolingReturnPath`, `UnstableDiskGasFraction` and `MergerDiskDominated`. A row whose `FFBRegime` is 1 skips the ordinary star-formation chain (`sage26_calculate_star_formation`, `sage26_calculate_supernova_feedback`, `sage26_apply_star_formation_supernova`, `sage26_disk_instability` and `sage26_apply_metal_enrichment` return at once on it), as in SAGE26's dispatch.

Modules refuse a missing or misplaced partner at initialisation, naming it (each module's README gives its checks):

- `sage26_apply_infall` needs `sage26_prepare_infall_budget` in `pre_timestep`; `sage26_apply_cooling` needs `sage26_calculate_cooling_budget` before it in the same substep phase; `sage26_calculate_star_formation` and `sage26_calculate_supernova_feedback` each need `sage26_apply_star_formation_supernova` in the pipeline, which needs each of them that is configured before it in the same substep phase, star formation before feedback; `sage26_apply_metal_enrichment` needs `sage26_apply_star_formation_supernova` and the by-galaxy `sage26_starburst_feedback`, where configured, before it.
- With `CGMrecipeOn 1`, `sage26_calculate_cooling_budget` needs `sage26_regime_aware_cooling` in its substep phase and `sage26_determine_regime` in `pre_timestep`, and `sage26_regime_aware_cooling` must sit once, by galaxy in a substep phase, immediately after `sage26_apply_cooling`.
- `sage26_ffb_star_formation` must appear once, by galaxy in a substep phase. With `EnhancedStarFormationOn 1`, `sage26_determine_ffb_regime` and `sage26_ffb_star_formation` need each other, and the transaction must sit immediately before `sage26_calculate_star_formation`.
- With `ConcentrationOn 3`, `sage26_set_halo_concentration` needs `sage26_record_transition_state` in `post_timestep`.
- `sage26_initialise_merger_clock`, `sage26_set_halo_concentration`, `sage26_determine_regime` and `sage26_determine_ffb_regime` must each appear once, in `pre_timestep`. Where they are configured there, `sage26_reionization` and `sage26_determine_regime` must precede `sage26_prepare_infall_budget`, and `sage26_determine_ffb_regime` must follow both `sage26_determine_regime` and `sage26_prepare_infall_budget`.
- `sage26_finalize_cooling_heating`, `sage26_finalize_sfr_histories` and `sage26_record_transition_state` must each appear once, in `post_timestep`, the recorder last; `sage26_finalize_sfr_histories` and `sage26_resolve_mergers_and_disruption` need `sage26_initialise_merger_clock` in `pre_timestep`, and a resolver configured in `pre_timestep` must follow it.
- The merger-event consumers `sage26_quasar_mode` and `sage26_starburst_feedback` need `sage26_resolve_mergers_and_disruption` in their substep phase, the quasar consumer first.

`SubSteps: 10` with no `TimestepScheme` is Mimic's fixed scheme, exactly 10 substeps per snapshot interval, where SAGE26 adapts its step count per halo.

## Parameters

Every key below is required by each module that reads it, with no default: a missing key fails initialisation. The one exception is `MergerTimeFactor`, which is optional and defaults to 2.0. Both run files hold the same parameters with the same values. No parameter goes through the core's unit conversion (the package has no `parameter_units.yaml`); the modules that read `MShockMsun` convert it to 1e10 Msun/h themselves. The accepted values are those every reading module enforces; a range check does not refuse a NaN, except for `MShockMsun`, `EnhancedSFEfficiency` and `H2RadialRMaxFactor`.

| Parameter | Units | Value | Accepted values | Meaning |
|---|---|---|---|---|
| `GlobalBaryonFraction` | dimensionless | 0.17 | (0, 1] | Cosmic baryon fraction, which reionization suppresses in low-mass halos |
| `CGMrecipeOn` | flag | 1 | 0 or 1 | 1: the two-regime CGM (classifier, regime-aware routing and cooling); 0: `Regime` stays -1 and cooling takes the hot path |
| `MShockMsun` | Msun (physical, no h) | 6.0e11 | positive and finite | Shock mass of the regime classifier and the cold streams |
| `EnhancedStarFormationOn` | flag | 1 | 0 or 1 | 1: the feedback-free-burst classifier and star formation; 0: `FFBRegime` stays 0 |
| `EnhancedSFEfficiency` | dimensionless | 0.2 | finite, in [0, 1] | Star-formation efficiency of a feedback-free burst |
| `ConcentrationOn` | flag | 3 | 0 or 3 | 3: the `Vmax/Vvir` concentration with the infall freeze; 0: `Concentration` stays 0 |
| `SfrEfficiency` | dimensionless | 0.05 | [0, 1] | Star-formation efficiency |
| `StarFormingDiskFactor` | dimensionless | 3.0 | [0, 10] | Disk scale radii over which stars form; also the factor in the disk-instability critical mass |
| `SFprescription` | flag | 1 | 0 or 1 | 1: Blitz & Rosolowsky (2006) molecular star formation; 0: Croton et al. (2006) |
| `H2RadialIntegrationOn` | flag | 1 | 1 | Radial integration of the molecular fraction |
| `H2RadialNBins` | count | 25 | 25 | Radial bins of the integration |
| `H2RadialRMaxFactor` | dimensionless | 5.0 | 5.0 | Outer radius of the integration, in disk scale radii |
| `StarburstColdGasOn` | flag | 1 | 1 | Bursts consume the cold gas |
| `FeedbackReheatingEpsilon` | dimensionless | 2.9 | (0, 10] | Supernova reheating efficiency |
| `FeedbackEjectionEfficiency` | dimensionless | 0.3 | [0, 10] | Supernova ejection efficiency |
| `FIREmodeOn` | flag | 1 | 0 or 1; 0 is refused when `EnhancedStarFormationOn` is 1 and `sage26_ffb_star_formation` is configured | 1: FIRE feedback on the disk path and in every burst; 0: fixed-efficiency feedback |
| `ReIncorporationFactor` | dimensionless | 0.19 | [0, 10] | Scales the critical velocity of reincorporation, 354.53 km/s |
| `AGNrecipe` | flag | 2 | 0 to 3; 1 and 3 are not SAGE26's rates | Radio-mode accretion: 0 off, 2 SAGE26's Bondi-Hoyle; 1 empirical and 3 cold cloud, not SAGE26's forms but the same expressions as `sage16`'s |
| `RadioModeEfficiency` | dimensionless | 0.08 | [0, 1] | Radio-mode AGN heating efficiency |
| `BlackHoleGrowthRate` | dimensionless | 0.015 | (0, 1] | Black-hole growth efficiency in mergers and instabilities |
| `QuasarModeEfficiency` | dimensionless | 0.005 | [0, 1] | Quasar-mode wind efficiency |
| `RecycleFraction` | dimensionless | 0.43 | [0, 1] | Instantaneous recycling fraction |
| `Yield` | dimensionless | 0.025 | [0, 1] | Metal yield per unit stellar mass formed |
| `FracZleaveDisk` | dimensionless | 0.0 | [0, 1] | Low-mass limit of the fraction of the metal yield of disk, feedback-free-burst and starburst star formation sent directly to the FoF central's hot gas (CGM in the CGM regime). The applied fraction is `FracZleaveDisk * exp(-Mvir / 30.0)`, with the central's `Mvir` in 1e10 Msun/h, where the forming galaxy's cold gas exceeds 1e-8 (and, for a burst, its mass ratio is below `ThresholdMajorMerger`); otherwise the whole yield goes there |
| `ThresholdMajorMerger` | dimensionless | 0.3 | [0, 1] | Mass-ratio threshold of a major merger |
| `ThresholdSatDisruption` | dimensionless | 1.0 | (0, 1000] | `Mvir`-to-baryonic mass-ratio threshold of a satellite's merger or disruption |
| `MergerTimeFactor` | dimensionless | 2.0 | optional, default 2.0; (0, 1000] | Scaling of the dynamical-friction merger clock |
| `BulgeSizeOn` | flag | 3 | 0 or 3 | 3: Tonini et al. (2016) merger and instability bulges; 0: no bulge sizes |

## Output Contract

A run writes 72 properties per galaxy: the 52 `output: true` properties of [`model_properties.yaml`](model_properties.yaml) and 20 core and simulation halo properties (the run's `metadata/output_schema.json` lists them with their units).

SAGE26 writes 81 datasets per snapshot. [`instrument/field_map.yaml`](instrument/field_map.yaml) maps 73 of them, 50 to package properties, 11 to core properties and 12 to simulation properties (9 to the components of `Pos`, `Vel` and `Spin`, and 3 to `MostBoundID` (from SAGE26's `SimulationHaloIndex`), `Vmax` and `VelDisp`), each by name, storage type and conversion, and every conversion is a factor of 1. Only the 23 core and simulation mappings also carry a unit check, which finds SAGE26's written unit and Mimic's output unit equal. It leaves 8 uncompared, each with its reason: SAGE26's identity encodings (`GalaxyIndex`, `CentralGalaxyIndex`), which `UniqueGalaxyID` and `UniqueCentralGalaxyID` replace; its forest-local halo and tree numbering (`SAGEHaloIndex`, `SAGETreeIndex`); the merger fields SAGE26 back-patches into a galaxy's earlier output record (`mergeType`, `mergeIntoID`, `mergeIntoSnapNum`), which have no counterpart where each record is written once; and `g_max`, which SAGE26's default configuration writes as zero. Where SAGE26's label for a value's unit is wrong, the property's metadata states its true unit and meaning. The map is a correspondence of names, storage types and conversions, with units checked for the core and simulation mappings only: no numerical agreement is claimed, and a galaxy's CGM regime is not SAGE26's realisation (see the random draws under [Departures from SAGE26](#departures-from-sage26)).

The written properties with no SAGE26 dataset are the package's `HaloBaryonFraction` and `StarFormationRate` and the core's `UniqueGalaxyID`, `UniqueCentralGalaxyID` and `deltaMvir`.

Both run files write HDF5 (`output_format: hdf5`): a master file `model.hdf5` and partition files, with `Snap<NNN>/Galaxies` tables and a `RunProperties` group recording the run's parameters, enabled modules, event contracts, field metadata, redshifts and version. The vertical run file writes one partition per input tree file, `model_000.hdf5` to `model_007.hdf5` (`first_file` 0 to `last_file` 7 in `simulations/mini-millennium/simulation_info.yaml`), each holding all eight output snapshots' `Snap<NNN>` groups. The horizontal run file writes one partition per selected output snapshot (its `snapshot_list`), eight files named by snapshot number, `model_016.hdf5`, `model_018.hdf5`, `model_020.hdf5`, `model_023.hdf5`, `model_027.hdf5`, `model_032.hdf5`, `model_037.hdf5` and `model_063.hdf5`, each holding only its own snapshot's group. The vertical run file may select `binary` instead, whose record layout `metadata/output_schema.json` gives; horizontal runs are HDF5-only.

## Plots

The figure registry is [`plots/figures/__init__.py`](plots/figures/__init__.py) and the plot profiles are in `plots/profiles/`. After a run, plot it with its run file:

```bash
source mimic_venv/bin/activate
python plot/mimic-plot/mimic-plot.py --param-file=models/sage26/input/sage26_mini-millennium.yaml
python plot/mimic-plot/mimic-plot.py --param-file=models/sage26/input/sage26_mini-millennium-horizontal.yaml
```

## Build, Run, and Test

The default model is `sage16`, so a plain `make` does not build this package. Build and run each run file with its own simulation package, from the repository root:

```bash
make MODEL=sage26 SIMULATION=mini-millennium
./mimic models/sage26/input/sage26_mini-millennium.yaml

make MODEL=sage26 SIMULATION=mini-millennium-horizontal
./mimic models/sage26/input/sage26_mini-millennium-horizontal.yaml
```

The output goes to `output/sage26-mini-millennium/` and `output/sage26-mini-millennium-horizontal/`. Useful checks, with the same `MODEL` and `SIMULATION` as the build:

```bash
make MODEL=sage26 SIMULATION=mini-millennium validate-modules
make MODEL=sage26 SIMULATION=mini-millennium check-generated
make MODEL=sage26 SIMULATION=mini-millennium tests-unit
make MODEL=sage26 SIMULATION=mini-millennium tests-integration
make MODEL=sage26 SIMULATION=mini-millennium tests-scientific
```

Model tests register in the default tiers only for the simulations in `FULL_MODEL_TEST_SIMULATIONS` (`scripts/discovery.py`), which does not include `mini-millennium-horizontal`, so the horizontal package's tiers run none of this package's tests. The package's horizontal guard is the cross-format identity gate, [`test_cross_format_identity.py`](../../simulations/mini-millennium-horizontal/_tests/scientific/test_cross_format_identity.py), whose `models` tuple includes `sage26`: it needs the package's dataset, builds its models itself and is run as `make MODEL=halos-only SIMULATION=mini-millennium-horizontal tests-scientific`.

## Caveats

- Run files exist for mini-Millennium only, vertical and horizontal; no other simulation is supported.
- The oracle tests compare bit for bit only where `oracle_environment_mismatch()` in `modules/_tests/sage26_oracle_fixture.h` returns NULL, which it does only inside `#if defined(__APPLE__) && defined(__aarch64__) && defined(__apple_build_version__)` and the nested `#if __clang_major__ == 21`, and they report SKIP elsewhere. The predicate does not test the operating-system version or the compiler's minor version.
- The fixtures' provenance is separate from that predicate: they were generated on macOS 27.0.1 on arm64 with Apple clang 21.0.0 (clang-2100.4.1.1) (the oracle README, *Oracle Environment*).
- Where a departure in [Departures from SAGE26](#departures-from-sage26) reaches a written value, it moves `sage26` output away from SAGE26's galaxy by galaxy; not every departure does (the orphan resets reach no written row on R0).
- The CGM classifier's deviate is keyed on `UniqueGalaxyID`, so a galaxy's regime follows that identifier's construction, not SAGE26's draw order. Both drivers build it with the shared encoder `mimic_encode_unique_galaxy_id` (`src/include/galaxy_id.h`), called by `make_unique_galaxy_id` in `src/core/build_model.c` and by `horizontal_make_unique_galaxy_id` in `src/core/horizontal_driver.c`.
- No CI job (`.github/workflows/ci.yml`) selects `MODEL=sage26`: the only `MODEL=` any of its `make` invocations names is `halos-only`, with `SIMULATION=mini-millennium-horizontal`.
- Some configurations are not refused at initialisation:
  - `EnhancedStarFormationOn 1` with neither `sage26_determine_ffb_regime` nor `sage26_ffb_star_formation` configured runs as R5: those are the only modules that read the flag, and each refuses the other's absence only when it is configured itself, so `FFBRegime` stays 0.
  - `ConcentrationOn 3` without `sage26_set_halo_concentration` leaves `Concentration` at 0: that module alone reads the flag.
  - `CGMrecipeOn 1` is refused without `sage26_regime_aware_cooling` and `sage26_determine_regime` by `sage26_calculate_cooling_budget`, which the other two cooling modules require. In a pipeline without `sage26_calculate_cooling_budget` no module requires the classifier, and without it `Regime` stays -1 and every regime-routed deposit goes to the hot gas.
  - `sage26_apply_star_formation_supernova` without either prescription module, star formation without `sage26_apply_metal_enrichment`, `sage26_starburst_feedback` by galaxy without `sage26_disk_instability` before it, `sage26_starburst_feedback` and `sage26_disk_instability` without `sage26_quasar_mode` as a merger-event consumer, and `sage26_initialise_merger_clock` without `sage26_record_transition_state` each run with a warning naming what is lost.

## Extending sage26

To add or change a module:

1. Work under `models/sage26/modules/<module_name>/`.
2. Declare supported processing modes, property dependencies, parameter dependencies, event contracts, tests and docs in `module_info.yaml`.
3. Add or update properties in `model_properties.yaml` only when the model's galaxy state changes.
4. Keep reusable model-local helper logic in `shared/`; do not promote it to core unless it is genuinely physics-agnostic.
5. Load each new SAGE26 parameter as required, with no default, and refuse every value the package does not port, naming the parameter.
6. Regenerate and validate with `make MODEL=sage26 SIMULATION=mini-millennium generate` and `make MODEL=sage26 SIMULATION=mini-millennium validate-modules`.

A change to the SAGE26 arithmetic a module ports (a new routine, branch or configuration) needs its oracle fixtures added and regenerated with `regenerate.sh`, never edited by hand ([`modules/_tests/oracle/README.md`](modules/_tests/oracle/README.md), *Regenerating*).

When creating a new model family from `sage26`, copy the needed modules into a new `models/<model>/` package and reconcile property names, units, parameters, tests, plots and run YAML there instead of mixing model packages at runtime. See [Creating Physics Modules](../../docs/DEVELOPER-GUIDE.md#creating-physics-modules) for the full workflow.

## References

- Blitz & Rosolowsky (2006), ApJ, 650, 933 — the pressure-based molecular fraction.
- Bradley, M., Croton, D. J., et al. (2026), SAGE26 Paper I: Modelling the Baryon Cycle from Cosmic Dawn to the Present Day, ApJS, in preparation — SAGE26.
- Carr et al. (2023), ApJ, 949, 21 — the bulk CGM cooling SAGE26's CGM recipe follows.
- Croton et al. (2006), MNRAS, 365, 11 — the original SAGE cooling and AGN feedback.
- Croton et al. (2016), ApJS, 222, 22 — SAGE: model calibration and basic results.
- Dekel & Birnboim (2006), MNRAS, 368, 2 — the shock mass.
- Hirschmann et al. (2016), MNRAS, 461, 1760 — the energy-based ejection.
- Li et al. (2024), as SAGE26 cites it (SAGE26 `src/model_regimes.c:46`, `:167`) — the feedback-free-burst threshold halo mass (their eq. 2) and fraction (their eq. 3); SAGE26's sources give no journal reference.
- Muratov et al. (2015), MNRAS, 454, 2691 — the FIRE mass loading.
- Navarro, Frenk & White (1997), ApJ, 490, 493 — the NFW halo profile.
- Tonini et al. (2016), MNRAS, 459, 4109 — merger and instability bulges.
- Voit (2015), ApJL, 808, L30 — self-regulating precipitation, cited by SAGE26's README for the two-regime CGM.
