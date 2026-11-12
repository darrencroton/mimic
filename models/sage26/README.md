# sage26 Model Package

**sage26** is a model package for Mimic that implements SAGE26 (Bradley et al. 2026, in preparation), the successor to the SAGE model of Croton et al. (2016). Mimic's physics-agnostic core handles tree traversal, memory, configuration, validation and output, while this package owns the baryonic physics, as runtime modules that follow SAGE26's routines and their arithmetic.

The package is self-contained: its properties, modules, shared helpers, tests, run files and plotting figures all live under `models/sage26/`. It is not the build default, so every `make` command names `MODEL=sage26` with the simulation package of the run file. For the general model-package concepts, see the [Developer Guide](../../docs/DEVELOPER-GUIDE.md); for running and configuring a model, see the [User Guide](../../docs/USER-GUIDE.md).

## Scientific Scope

The package follows the baryonic reservoirs that [`model_properties.yaml`](model_properties.yaml) declares on dark-matter merger trees: hot gas, the circumgalactic medium (`CGMgas`), cold gas (with its molecular and atomic hydrogen, `H2gas` and `H1gas`), ejected gas, stellar mass, the bulge (with its merger and instability channels, `MergerBulgeMass` and `InstabilityBulgeMass`), the intracluster stars (`ICS`) and the black hole, each gas and stellar reservoir with its metals.

The run files configure SAGE26's default physics. By subsystem:

- **The baryon cycle inherited from Croton et al. (2006, 2016):** reionization, gas infall, reincorporation of ejected gas, satellite stripping, radiative cooling with radio-mode AGN heating (`AGNrecipe 2`, Bondi-Hoyle accretion), star formation, supernova feedback, disk instabilities, black-hole growth and quasar-mode winds, starbursts, mergers and satellite disruption into the intracluster stars.
- **Bulge sizes, `BulgeSizeOn 3`:** the Tonini et al. (2016) merger and instability bulges, each with its own mass and radius.
- **Star formation, `SFprescription 1`:** the Blitz & Rosolowsky (2006) molecular fraction, integrated radially over the exponential disk (`H2RadialIntegrationOn 1`, 25 bins out to 5 disk scale radii), with stars formed from the molecular gas (`H2gas`).
- **Supernova feedback, `FIREmodeOn 1`:** the Muratov et al. (2015) FIRE mass loading, scaled with redshift and the galaxy's own virial velocity and capped, with the Hirschmann et al. (2016) energy-based ejection, on the disk path and in every starburst.
- **The two-regime CGM, `CGMrecipeOn 1`:** once per snapshot every galaxy is classified as CGM regime (`Regime` 0) or hot-halo regime (`Regime` 1) by a sigmoid 0.1 dex wide about the Dekel & Birnboim (2006) shock mass (`MShockMsun`), against a uniform deviate keyed on the galaxy (see [Notes on Behaviour](#notes-on-behaviour)). Infall, reincorporated gas and supernova-reheated gas go to the receiving central's CGM reservoir when it is in the CGM regime and to its hot gas otherwise, and cooling follows SAGE26's regime-aware recipe: bulk cooling of the CGM, and hot-halo cooling with cold streams. SAGE26's README cites Dekel & Birnboim (2006) and Voit (2015) for the model and Carr et al. (2023) for the recipe it follows.
- **Halo concentration, `ConcentrationOn 3`:** each new FoF central's NFW (Navarro, Frenk & White 1997) concentration, inverted from `Vmax/Vvir` at progenitor joining, from the infall-frozen velocities of a galaxy that was a satellite. `Concentration` is an output only: no other module reads it.
- **Feedback-free bursts, `EnhancedStarFormationOn 1`:** once per snapshot every galaxy is classified as a feedback-free-burst row (`FFBRegime` 1) with the probability of the Li et al. (2024) sigmoid about their threshold halo mass. An FFB row forms its stars from the cold gas at efficiency `EnhancedSFEfficiency` over the disk's dynamical time, with FIRE feedback and no disk instability, in place of the ordinary star-formation chain.

The package does not implement SAGE26's other options. Each value below is refused at initialisation with an error naming the parameter, by the modules that read it:

| Parameter | Refused values | SAGE26 option not supported |
|---|---|---|
| `AGNrecipe` | anything but 0 and 2 | Radio-mode accretion 1 (empirical) and 3 (cold cloud) |
| `SFprescription` | anything but 0 and 1 | Prescriptions 2–7 |
| `H2RadialIntegrationOn`, `H2RadialNBins`, `H2RadialRMaxFactor` | anything but 1, 25 and 5.0 respectively | The single-slab molecular fraction and other integration grids |
| `StarburstColdGasOn` | anything but 1 | Bursts fuelled by the molecular gas |
| `BulgeSizeOn` | anything but 0 and 3 | Modes 1 and 2 |
| `ConcentrationOn` | anything but 0 and 3 | Mode 1 (the lookup table) and mode 2 (`Vmax/Vvir` for every galaxy) |
| `EnhancedStarFormationOn` | anything but 0 and 1 | Mode 2, the acceleration criterion |
| `FIREmodeOn` | 0 when `EnhancedStarFormationOn` is 1 (by `sage26_ffb_star_formation`) | The fixed-efficiency feedback of feedback-free bursts |

SAGE26's adaptive substep count has no package parameter: see [Notes on Behaviour](#notes-on-behaviour).

## Agreement with SAGE26

Measured on mini-Millennium against SAGE26 in its default configuration on the same trees, under the vertical driver with [`input/sage26_mini-millennium.yaml`](input/sage26_mini-millennium.yaml) (at Mimic commit `d0f51d62`):

- With Mimic's core and the package's random draws aligned to SAGE26's in a throwaway build (Mimic's core given SAGE26's per-halo substep count, float halo state, inheritance resets, written halo values and particle-mass constant, and SAGE26's random draws replayed), the package reproduces SAGE26 bit for bit in every compared property, 67 float and 6 integer, for every galaxy (36,534 at z = 0) at all 56 snapshots from 8 to 63.
- On Mimic's own core, as shipped, the two agree at the population level: at z = 0 the stellar-mass, hot-gas, CGM and intracluster-star totals differ by 0.45%, 0.37%, 1.75% and 1.51%, and 23 of 102 mass-function bins (stellar, cold-gas and black-hole; 0.25 dex, holding at least 20 galaxies) by more than 1%. The difference comes mainly through the realisation of the CGM-regime draw, which is keyed per galaxy here (see [Notes on Behaviour](#notes-on-behaviour)) so that no galaxy's regime is SAGE26's realisation: the four totals (judged beyond 0.2%) and the 102 bins (beyond 1%) make 106 readings, of which 27 lie beyond on Mimic's own core, against 25 with the core aligned and only the random draws left unaligned, and 8 to 31 when SAGE26 itself is run with a changed random stream.
- The vertical and horizontal drivers agree with each other bitwise, per `UniqueGalaxyID` in every written field, under the `mini-millennium-horizontal` cross-format identity gate (`simulations/mini-millennium-horizontal/_tests/scientific/test_cross_format_identity.py`). The horizontal driver is compared with the vertical driver, not with SAGE26.

Other simulations, compilers and platforms are not measured against SAGE26.

## Notes on Behaviour

- **Random draws.** SAGE26 draws the CGM regime classifier's deviate from libc's process-global `rand()`, so a galaxy's draw depends on every earlier draw of the run. The package draws a deviate that is a pure function of the galaxy's `UniqueGalaxyID`, the snapshot, a purpose salt and an ordinal ([`shared/sage_keyed_draw.h`](shared/sage_keyed_draw.h)), so both drivers, any MPI layout and any traversal order classify a galaxy alike. The feedback-free-burst classifier uses SAGE26's own hash of the halo's `MostBoundID` and the snapshot. The package consumes no global random stream.
- **Substeps.** The run files use Mimic's fixed scheme, `SubSteps: 10`; SAGE26 adapts its substep count per halo between 10 and 30.
- **Precision.** Eight core properties that SAGE26 holds and writes as float (`dT`, `Mvir`, `CentralMvir`, `Rvir`, `Vvir`, `infallMvir`, `infallVvir` and `infallVmax`) are double in Mimic's core and written as float64. The package's own reservoirs are float, as SAGE26's.
- **Undefined reads are refused.** Where SAGE26 reads its redshift table at a galaxy's stored `SnapNum` without a bounds check, the package refuses a stored `SnapNum` outside the table with an error naming it.
- **Parameters have no defaults.** Every parameter is required except `MergerTimeFactor` (default 2.0). Values SAGE26 accepts but the package does not implement are refused at initialisation (the table under [Scientific Scope](#scientific-scope)).
- **Partial pipelines.** Modules refuse a missing or misplaced partner where a wrong answer would follow silently (see [Runtime Pipeline](#runtime-pipeline)). Three omissions are accepted without a message, because only the absent module reads the switch: `EnhancedStarFormationOn 1` with neither `sage26_determine_ffb_regime` nor `sage26_ffb_star_formation` configured leaves `FFBRegime` at 0; `ConcentrationOn 3` without `sage26_set_halo_concentration` leaves `Concentration` at 0; and a pipeline without `sage26_calculate_cooling_budget` does not require the regime classifier, so without it `Regime` stays -1 and every regime-routed deposit goes to the hot gas. Other omissions run with a warning naming what is lost: star formation without `sage26_apply_metal_enrichment`, `sage26_starburst_feedback` by galaxy without `sage26_disk_instability` before it, the burst and instability modules without `sage26_quasar_mode` as a merger-event consumer, and `sage26_initialise_merger_clock` without `sage26_record_transition_state` (each module's README lists its checks).

## Package Contents

- `model_properties.yaml`: the package's galaxy property metadata: reservoirs, transport properties, SAGE26's diagnostics and outputs. Edit it when the galaxy state changes, then run `make MODEL=sage26 SIMULATION=<name> generate`.
- `input/`: a run file for every supported simulation package, under both drivers: mini-Millennium, full Millennium, micro-Uchuu (L-Halo binary, HDF5 forests and ASCII trees), mini-Uchuu, full Uchuu and Shin-Uchuu. Every file configures the same pipeline and parameters.
- `modules/`: the 26 runtime modules. Each has its own directory with C source, `module_info.yaml`, README and `_tests/` with its unit and integration tests.
- `modules/_tests/`: the package's shared tests (processing-mode and dependency contracts, the central parameter-refusal and placement tables, mixed-interval timesteps), the shared test fixture header, and the full-physics baseline input with its committed reference output.
- `shared/`: model-local helper headers (SAGE26's physics kernels, the keyed draw, constants), described in [`shared/README.md`](shared/README.md), with their unit tests in `shared/_tests/`. They are not framework APIs.
- `plots/figures/`: the package's diagnostic figures for `mimic-plot.py`, and `plots/profiles/` the plot profiles (`default.yaml`).
- `LICENSE-SAGE26`: SAGE26's MIT licence (see [Licence and Attribution](#licence-and-attribution)).

## Runtime Pipeline

Every run file under `input/` configures the same pipeline; [`input/sage26_mini-millennium.yaml`](input/sage26_mini-millennium.yaml) lists the modules and their order. It builds the pipeline as:

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
- When its partners are configured, `sage26_radio_mode_heating` must sit after `sage26_calculate_cooling_budget` and before `sage26_apply_cooling` in the same substep phase, and `sage26_disk_instability` before the by-galaxy `sage26_quasar_mode` entry in the same substep phase, so neither reads the previous substep's values.
- `sage26_initialise_merger_clock`, `sage26_set_halo_concentration`, `sage26_determine_regime` and `sage26_determine_ffb_regime` must each appear once, in `pre_timestep`. Where they are configured there, `sage26_reionization` and `sage26_determine_regime` must precede `sage26_prepare_infall_budget`, and `sage26_determine_ffb_regime` must follow both `sage26_determine_regime` and `sage26_prepare_infall_budget`.
- `sage26_finalize_cooling_heating`, `sage26_finalize_sfr_histories` and `sage26_record_transition_state` must each appear once, in `post_timestep`, the recorder last; `sage26_finalize_sfr_histories` and `sage26_resolve_mergers_and_disruption` need `sage26_initialise_merger_clock` in `pre_timestep`, and a resolver configured in `pre_timestep` must follow it.
- The merger-event consumers `sage26_quasar_mode` and `sage26_starburst_feedback` need `sage26_resolve_mergers_and_disruption` in their substep phase, the quasar consumer first.

`SubSteps: 10` with no `TimestepScheme` is Mimic's fixed scheme, exactly 10 substeps per snapshot interval (see [Notes on Behaviour](#notes-on-behaviour)).

## Parameters

Every key below is required by each module that reads it, with no default: a missing key fails initialisation. The one exception is `MergerTimeFactor`, which is optional and defaults to 2.0. Every run file holds the same parameters with the same values, SAGE26's defaults. No parameter goes through the core's unit conversion (the package has no `parameter_units.yaml`); the modules that read `MShockMsun` convert it to 1e10 Msun/h themselves. The accepted values are those every reading module enforces; a range check does not refuse a NaN, except for `MShockMsun`, `EnhancedSFEfficiency` and `H2RadialRMaxFactor`.

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
| `FeedbackReheatingEpsilon` | dimensionless | 2.9 | finite and ≥ 0 | Supernova reheating efficiency |
| `FeedbackEjectionEfficiency` | dimensionless | 0.3 | [0, 10] | Supernova ejection efficiency |
| `FIREmodeOn` | flag | 1 | 0 or 1; 0 is refused when `EnhancedStarFormationOn` is 1 and `sage26_ffb_star_formation` is configured | 1: FIRE feedback on the disk path and in every burst; 0: fixed-efficiency feedback |
| `ReIncorporationFactor` | dimensionless | 0.19 | [0, 10] | Scales the critical velocity of reincorporation, 354.53 km/s |
| `AGNrecipe` | flag | 2 | 0 or 2 | 0: no radio-mode heating and no black-hole growth in mergers, instabilities or the post-minor-merger recheck, as SAGE26's `AGNrecipeOn 0`; 2: SAGE26's Bondi-Hoyle radio-mode accretion. Read by `sage26_radio_mode_heating`, `sage26_regime_aware_cooling`, `sage26_quasar_mode` and `sage26_starburst_feedback` (the last when the recheck can grow the black hole) |
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

Property names and storage types follow SAGE26's own output datasets wherever a counterpart exists, so analysis written for SAGE26 output needs little adaptation. SAGE26's identity encodings (`GalaxyIndex`, `CentralGalaxyIndex`) are replaced by Mimic's `UniqueGalaxyID` and `UniqueCentralGalaxyID`; its forest-local numbering (`SAGEHaloIndex`, `SAGETreeIndex`), the merger fields it back-patches into a galaxy's earlier record (`mergeType`, `mergeIntoID`, `mergeIntoSnapNum`) and `g_max` have no counterpart. Where SAGE26's own label for a value's unit is wrong (`SfrDiskZ` and `SfrBulgeZ` are dimensionless metallicities; `mdot_cool` and `mdot_stream` are in Msun/yr), the property's metadata states the true unit and meaning.

The written properties with no SAGE26 dataset are the package's `HaloBaryonFraction` and `StarFormationRate` and the core's `UniqueGalaxyID`, `UniqueCentralGalaxyID` and `deltaMvir`.

The run files write HDF5 (`output_format: hdf5`): a master file `model.hdf5` and partition files, with `Snap<NNN>/Galaxies` tables and a `RunProperties` group recording the run's parameters, enabled modules, event contracts, field metadata, redshifts and version. A vertical run writes one partition per input tree file (on mini-Millennium `model_000.hdf5` to `model_007.hdf5`), each holding every output snapshot's `Snap<NNN>` group; a horizontal run writes one partition per selected output snapshot, named by snapshot number, each holding only its own group. A vertical run file may select `binary` instead, whose record layout `metadata/output_schema.json` gives; horizontal runs are HDF5-only.

## Plots

The figure registry is [`plots/figures/__init__.py`](plots/figures/__init__.py) and the plot profiles are in `plots/profiles/`. After a run, plot it with its run file:

```bash
source mimic_venv/bin/activate
python plot/mimic-plot/mimic-plot.py --param-file=models/sage26/input/sage26_mini-millennium.yaml
```

The registry holds 48 figures: 30 snapshot figures and 18 evolution figures. Twenty-two are Mimic's standard galaxy and halo diagnostics; of these, `baryon_fraction` and `mass_reservoir_scatter` add the `CGMgas` reservoir and `gas_mass_function` adds model H2 and H I mass functions from `H2gas` and `H1gas`, as SAGE26's counterpart figures draw them. The other 26 follow figures of SAGE26's own plotting scripts: twelve z = 0 figures of `allresults-local.py`, thirteen evolution figures of `allresults-history.py`, and `ffb_fraction_cut_down`, a cut-down version of plot 23c of `paper_plots.py` drawn without SAGE26's comparison run.

The profile `plots/profiles/default.yaml` lists the 48 figures in registry order and gives the figures that follow SAGE26 no `axes` entry, so each sets SAGE26's own axis window, or none where SAGE26 sets none.

The evolution figures read the run's output snapshots (the run file's `snapshot_list`) in increasing redshift. A figure that bins by redshift uses SAGE26's fifteen redshift bins, so a bin that holds no written snapshot draws nothing, and on Millennium's snapshot list z = 0 falls in none of them, as in SAGE26; a figure drawn at the snapshot nearest a target redshift takes the nearest written snapshot and labels its actual redshift.

The figures in registry order, the 30 snapshot figures first. In the *Follows* column, a SAGE26 entry names the script in SAGE26's `plotting/` directory and the figure there; a Mimic diagnostic marked "covers" is the package's figure for that SAGE26 figure.

| Figure | What it shows | Follows |
|---|---|---|
| `halo_mass_function` | Halo mass function: abundance against halo mass. | Mimic diagnostic |
| `halo_occupation` | Number of galaxies per halo against halo mass. | Mimic diagnostic |
| `spin_distribution` | Distribution of halo spin parameters. | Mimic diagnostic |
| `velocity_distribution` | Distribution of velocities. | Mimic diagnostic |
| `spatial_distribution` | Galaxy positions in the box, in projection. | Mimic diagnostic; covers `allresults-local.py` `SpatialDistribution` |
| `stellar_mass_function` | Stellar mass function with observations. | Mimic diagnostic; covers `allresults-local.py` `StellarMassFunction` |
| `cold_gas_function` | Cold gas mass function. | Mimic diagnostic |
| `baryon_fraction` | Mean baryon fraction of haloes by reservoir (stars, cold, hot, CGM, ejected, ICS, black hole) against halo mass. | Mimic diagnostic, with SAGE26's CGM curve; covers `allresults-local.py` `BaryonFraction` |
| `baryonic_mass_function` | Mass function of stars plus cold gas. | Mimic diagnostic; covers `allresults-local.py` `BaryonicMassFunction` |
| `gas_mass_function` | Cold gas mass function with the H2 and H I model curves and observations. | Mimic diagnostic, with SAGE26's H2 and H I curves; covers `allresults-local.py` `GasMassFunction` |
| `baryonic_tully_fisher` | Baryonic mass against maximum circular velocity. | Mimic diagnostic; covers `allresults-local.py` `BaryonicTullyFisher` |
| `specific_sfr` | Specific star formation rate against stellar mass. | Mimic diagnostic; covers `allresults-local.py` `SpecificStarFormationRate` |
| `black_hole_bulge_relation` | Black hole mass against bulge mass. | Mimic diagnostic; covers `allresults-local.py` `BlackHoleBulgeRelationship` |
| `gas_fraction` | Cold gas fraction against stellar mass. | Mimic diagnostic; covers `allresults-local.py` `GasFraction` |
| `metallicity` | Cold gas metallicity against stellar mass. | Mimic diagnostic; covers `allresults-local.py` `Metallicity` |
| `bulge_mass_fraction` | Bulge and disk mass fractions against stellar mass. | Mimic diagnostic; covers `allresults-local.py` `BulgeMassFraction` |
| `quiescent_fraction` | Fraction of quiescent galaxies against stellar mass. | Mimic diagnostic; covers `allresults-local.py` `QuiescentFraction` |
| `mass_reservoir_scatter` | Mass in each reservoir (stars, cold, hot, ejected, ICS, CGM) against halo mass, per central. | Mimic diagnostic, with SAGE26's CGM series; covers `allresults-local.py` `MassReservoirScatter` |
| `bulge_to_total` | Bulge-to-total ratio against stellar mass, split into merger- and instability-driven bulges. | `allresults-local.py` `BulgeToTotal` |
| `spatial_distribution_3d_box` | Galaxy positions in the 3-D box, coloured by local density. | `allresults-local.py` `SpatialDistribution3D_Box` |
| `star_formation_rate` | Star formation rate against stellar mass. | `allresults-local.py` `StarFormationRate` |
| `mass_loading_vs_vvir` | Supernova mass-loading factor against virial velocity. | `allresults-local.py` `mass_loading_vs_vvir` |
| `stellar_vs_halo_mass_by_regime` | Stellar mass against halo mass, coloured by CGM or hot regime. | `allresults-local.py` `stellar_vs_halo_mass_by_regime` |
| `specific_star_formation_rate` | Density of galaxies in specific SFR against stellar mass, with the quiescent cut. | `allresults-local.py` `specific_star_formation_rate` |
| `concentration_vs_mvir` | Halo concentration against halo mass for centrals and satellites, coloured by stellar mass. | `allresults-local.py` `concentration_vs_mvir` |
| `sfr_vs_halo_accretion_rate` | Star formation rate against gas accretion rate (cooling plus cold streams). | `allresults-local.py` `sfr_vs_halo_accretion_rate` |
| `tcool_vs_mvir` | Cooling rate against halo mass by regime. | `allresults-local.py` `tcool_vs_mvir` |
| `cgm_fraction_vs_mvir` | SAGE26's CGM-fraction quantity against halo mass. | `allresults-local.py` `cgm_fraction_vs_mvir` |
| `ejection_efficiency_vs_vvir` | Ejection efficiency against virial velocity. | `allresults-local.py` `ejection_efficiency_vs_vvir` |
| `disk_radius_vs_stellar_mass` | Effective disk radius against virial radius for galaxies of stellar mass 10^9.3 to 10^11.3 Msun, with Somerville et al. (2018). | `allresults-local.py` `disk_radius_vs_stellar_mass` |
| `hmf_evolution` | Halo mass function at up to four of the written snapshots, those nearest z = 0, 1, 2 and 3. | Mimic diagnostic; covers `allresults-history.py` `HaloMassFunctionEvolution` |
| `smf_evolution` | Stellar mass function at several redshifts. | Mimic diagnostic; covers `allresults-history.py` `StellarMassFunction_z` |
| `sfr_density_evolution` | Cosmic star formation rate density against redshift. | Mimic diagnostic; covers `allresults-history.py` `History-SFR-density` |
| `stellar_mass_density_evolution` | Cosmic stellar mass density against redshift. | Mimic diagnostic; covers `allresults-history.py` `History-stellar-mass-density` |
| `rcool_to_rvir_above_mshock` | Distribution of cooling radius over virial radius for haloes above 6×10^11 Msun, near z = 0, 2 and 6. | `allresults-history.py` `RcoolToRvir_above_Mshock` |
| `sfr_evolution` | Mean star formation rate against stellar mass in redshift bins. | `allresults-history.py` `SFR_evolution` |
| `stellar_mass_halo_mass_relation` | Mean stellar mass against halo mass in redshift bins. | `allresults-history.py` `StellarMassHaloMassRelation` |
| `black_hole_mass_function` | Black hole mass function near z = 0.1, 1, 2, 4, 6 and 8, with observations. | `allresults-history.py` `BlackHoleMassFunction` |
| `quenched_fraction_evolution` | Quenched fraction of centrals and satellites against stellar mass in redshift bins. | `allresults-history.py` `QuenchedFraction.StellarMassFunctionEvolution` |
| `concentration_evolution` | Median halo concentration against redshift in four halo-mass bins. | `allresults-history.py` `ConcentrationEvolution` |
| `metallicity_evolution` | Mean stellar metallicity against stellar mass in redshift bins. | `allresults-history.py` `MetallicityEvolution` |
| `h2_fraction_evolution` | Mean H2 fraction of cold gas against stellar mass in redshift bins. | `allresults-history.py` `H2FractionEvolution` |
| `star_formation_efficiency` | Median integrated star formation efficiency of centrals against cosmic time. | `allresults-history.py` `StarFormationEfficiency` |
| `central_satellite_counts` | Average numbers of centrals and satellites per redshift bin. | `allresults-history.py` `CentralSatelliteCounts` |
| `central_satellite_fraction` | Central and satellite fractions against redshift. | `allresults-history.py` `CentralSatelliteFraction` |
| `central_satellite_fraction_massive_quiescent` | Central and satellite fractions among the most massive quiescent galaxies against redshift. | `allresults-history.py` `CentralSatelliteFraction_MassiveQuiescent` |
| `quenched_central_satellite_counts` | Average numbers of quenched centrals and satellites per redshift bin. | `allresults-history.py` `QuenchedCentralSatelliteCounts` |
| `ffb_fraction_cut_down` | Fraction of resolved galaxies in the feedback-free-burst regime against redshift at the run's eight snapshots: a cut-down version of SAGE26's paper figure 23c, without its MBK25 comparison run. | `paper_plots.py` plot 23c (`plot_23c_ffb_fraction_bk25`), cut down |

SAGE26's `StellarMassFunctionEvolution` and `SFRFunctionEvolution` have no counterpart: each draws a redshift bin only where the standard error over the bin's snapshots is positive, which needs more than one written snapshot per bin. The other figures of `paper_plots.py` need output the run does not write (a snapshot near z = 10, every snapshot of the simulation) or comparison runs the package does not make.

## Build, Run, and Test

Build with the simulation package of the run file, from the repository root; for example, on mini-Millennium under each driver:

```bash
make MODEL=sage26 SIMULATION=mini-millennium
./mimic models/sage26/input/sage26_mini-millennium.yaml

make MODEL=sage26 SIMULATION=mini-millennium-horizontal
./mimic models/sage26/input/sage26_mini-millennium-horizontal.yaml
```

Any other run file under `input/` runs the same way with its own `SIMULATION=` selector; the output goes to `output/sage26-<simulation>/`. Useful checks, with the same `MODEL` and `SIMULATION` as the build:

```bash
make MODEL=sage26 SIMULATION=mini-millennium validate-modules
make MODEL=sage26 SIMULATION=mini-millennium check-generated
make MODEL=sage26 SIMULATION=mini-millennium tests-unit
make MODEL=sage26 SIMULATION=mini-millennium tests-integration
make MODEL=sage26 SIMULATION=mini-millennium tests-scientific
```

The scientific tier compares a run of the complete pipeline on the committed test trees with the package's baseline under `modules/_tests/baseline/`, and checks that the fresh output holds only finite values inside the ranges its property metadata declares; it belongs to the `mini-millennium` pair and reports not-applicable elsewhere. Model tests register in the default tiers only for the simulations in `FULL_MODEL_TEST_SIMULATIONS` (`scripts/discovery.py`); the horizontal packages' guard is the cross-format identity gate of `simulations/mini-millennium-horizontal/`, which runs `sage26` as one of its models.

## Extending sage26

To add or change a module:

1. Work under `models/sage26/modules/<module_name>/`.
2. Declare supported processing modes, property dependencies, parameter dependencies, event contracts, tests and docs in `module_info.yaml`.
3. Add or update properties in `model_properties.yaml` only when the model's galaxy state changes.
4. Keep reusable model-local helper logic in `shared/`; do not promote it to core unless it is genuinely physics-agnostic.
5. Load each new SAGE26 parameter as required, with no default, and refuse every value the package does not support, naming the parameter.
6. Regenerate and validate with `make MODEL=sage26 SIMULATION=mini-millennium generate` and `make MODEL=sage26 SIMULATION=mini-millennium validate-modules`.

When creating a new model family from `sage26`, copy the needed modules into a new `models/<model>/` package and reconcile property names, units, parameters, tests, plots and run YAML there instead of mixing model packages at runtime. See [Creating Physics Modules](../../docs/DEVELOPER-GUIDE.md#creating-physics-modules) for the full workflow.

## Licence and Attribution

The package implements SAGE26, which is released under the MIT licence, Copyright (c) 2019-2026 Semi-Analytic Galaxy Evolution (SAGE). SAGE26's licence is reproduced verbatim in [`LICENSE-SAGE26`](LICENSE-SAGE26), and every source file and header derived from SAGE26 carries a one-line notice pointing to it; Mimic's own MIT licence is the repository's [`LICENSE.txt`](../../LICENSE.txt). Redistributions of Mimic that include this package must keep both notices. SAGE26's software citation (its `CITATION.cff`) is: Bradley, M. and Croton, D. J., *SAGE26: Semi-Analytic Galaxy Evolution model*, version 26.0, Swinburne University of Technology, `sage-home/SAGE26`; its preferred paper citation is the Bradley et al. (2026) entry under [References](#references).

## References

- Blitz & Rosolowsky (2006), ApJ, 650, 933 — the pressure-based molecular fraction.
- Bradley, M., Croton, D. J., et al. (2026), SAGE26 Paper I: Modelling the Baryon Cycle from Cosmic Dawn to the Present Day, ApJS, in preparation — SAGE26.
- Carr et al. (2023), ApJ, 949, 21 — the bulk CGM cooling SAGE26's CGM recipe follows.
- Croton et al. (2006), MNRAS, 365, 11 — the original SAGE cooling and AGN feedback.
- Croton et al. (2016), ApJS, 222, 22 — SAGE: model calibration and basic results.
- Dekel & Birnboim (2006), MNRAS, 368, 2 — the shock mass.
- Hirschmann et al. (2016), MNRAS, 461, 1760 — the energy-based ejection.
- Li et al. (2024), as SAGE26 cites it — the feedback-free-burst threshold halo mass (their eq. 2) and fraction (their eq. 3).
- Muratov et al. (2015), MNRAS, 454, 2691 — the FIRE mass loading.
- Navarro, Frenk & White (1997), ApJ, 490, 493 — the NFW halo profile.
- Tonini et al. (2016), MNRAS, 459, 4109 — merger and instability bulges.
- Voit (2015), ApJL, 808, L30 — self-regulating precipitation, cited by SAGE26's README for the two-regime CGM.
