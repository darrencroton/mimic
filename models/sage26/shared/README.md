# SAGE26 Shared Utilities

This directory contains helper APIs that are local to the SAGE26 model package. They are available when Mimic is built with `MODEL=sage26` and should not be treated as framework-wide conventions for other model sets.

If another model needs similar behavior, copy or reimplement the helper in that model package and reconcile the property names, units, parameters, and tests there.

Utility tests are registered in `shared/module_info.yaml`.

## Available Helpers

- **`metallicity.h`** — `mimic_get_metallicity(gas, metals)`: safe metal mass fraction with zero/negative guards and a 1.0 cap (SAGE parity).
- **`central_link.h`** — FoF central index resolution: `mimic_find_fof_central_index` finds the Type 0 central and `mimic_resolve_immediate_target_index` gives SAGE26's merger and disruption target, redirecting once past a target already removed.
- **`time_parity.h`** — Substep timestep: `mimic_object_substep_dt` with strict initial-boundary handling (SAGE parity), and `mimic_object_time_status_str` for its status.
- **`sage_constants.h`** — Shared literal constants: the `MergTime` sentinel protocol (`SAGE_MERGTIME_UNSET`, `SAGE_MERGTIME_CEILING`, `SAGE_MERGTIME_IMMEDIATE`), the cold-gas yield threshold, the metal-ejection halo-mass scale, `SAGE_HYDROGEN_MASS_FRAC`, the virial-temperature coefficient, `SAGE_SEC_PER_GIGAYEAR` and the hot-cooling return-path codes `SAGE_COOLING_PATH_*`.
- **`sage_exp10.h`** — `mimic_sage_exp10(x)`: 10^x as SAGE26 computes `pow(10.0, x)` (`__exp10` on Apple platforms, `pow` elsewhere), for the cold streams and the feedback-free-burst threshold.
- **`sage_keyed_draw.h`** — The stateless keyed uniform draw that replaces SAGE26's libc `rand()` in the CGM regime classifier: `mimic_sage_keyed_bits`, `mimic_sage_keyed_uniform_from_bits`, `mimic_sage_keyed_uniform` and `mimic_sage_cgm_regime_deviate`, a pure function of `UniqueGalaxyID`, snapshot, purpose and ordinal.
- **`sage_cgm_routing.h`** — SAGE26's regime-aware hot-phase deposits: `mimic_sage_add_gas_to_hot_reservoir` and `mimic_sage_add_metals_to_hot_reservoir` (CGM pair with `CGMrecipeOn 1` and `Regime` 0, hot pair otherwise), `mimic_sage_cgm_reservoir_selected`, and the `CGMrecipeOn` and `MShockMsun` validators.
- **`sage_agn_recipe.h`** — `mimic_sage_validate_agn_recipe`: accepts `AGNrecipe` 0 (no radio mode and no black-hole growth) and 2 (Bondi-Hoyle radio mode), refusing every other value.
- **`sage_cooling_physics.h`** — SAGE26's cooling and radio-mode AGN heating for both `CGMrecipeOn` paths: the hot and CGM cooling budgets and recipes, cold-stream cooling, the clustering-mass interpolation, the Bondi accretion and AGN heating helpers, and `mimic_sage_cool_gas_onto_galaxy`.
- **`sage_hi_physics.h`** — SAGE26's H I block: `mimic_sage_calculate_h1gas` with its `mimic_sage_ionized_gas_fraction` and `mimic_sage_atomic_disk_radius`.
- **`sage_h2_physics.h`** — SAGE26's Blitz & Rosolowsky (2006) molecular gas: the BR06 scale height and molecular fraction, `mimic_sage_h2_radial_integration` over the exponential disk, `mimic_sage_sf_prescription_tracks_h2`, and the `SFprescription`, H2 radial and `StarburstColdGasOn` validators.
- **`sage_sf_feedback_physics.h`** — SAGE26's star-formation and supernova-feedback commits, `mimic_sage_update_from_star_formation` and `mimic_sage_update_from_feedback` with their hydrogen caps, plus the supernova code constants and the `FeedbackReheatingEpsilon` validator.
- **`sage_fire_feedback_physics.h`** — SAGE26's FIRE supernova feedback for `FIREmodeOn 1`: the Muratov et al. (2015) scaling, `capped_eta_reheat`, `sn_energy_coupling`, the energy-based ejection, the stored-snapshot redshift lookup and the `FIREmodeOn` validator.
- **`sage_metal_yield_physics.h`** — `mimic_sage_apply_disk_yield`: SAGE26's disk star-formation metal yield, retained metals to the cold gas and escaping metals to the FoF central's selected reservoir.
- **`sage_sfr_history.h`** — SAGE26's six per-substep star-formation histories held as `<history>Bin<k>` properties: element access, substep binning, disk and burst sampling, the reset at joining and the writer's averaging into `SfrDisk`, `SfrBulge`, `SfrDiskZ` and `SfrBulgeZ`.
- **`sage_agn_physics.h`** — SAGE26's `grow_black_hole` and `quasar_mode_wind`: `mimic_apply_black_hole_growth` and `mimic_apply_quasar_mode_wind`, shared by sage26_quasar_mode and the burst's post-merger instability recheck.
- **`sage_bulge_size_physics.h`** — SAGE26's bulge sizes for `BulgeSizeOn` 0 and 3: `mimic_sage_get_bulge_radius`, `mimic_sage_update_instability_bulge_radius` (Tonini et al. 2016) and the `BulgeSizeOn` validator.
- **`sage_disk_instability_physics.h`** — SAGE26's `check_disk_instability` stellar transfer: `mimic_sage_check_disk_instability_with_sizes`, `mimic_sage_check_disk_instability` and `mimic_sage_apply_disk_instability` return the unstable gas fraction.
- **`sage_starburst_physics.h`** — `mimic_apply_collisional_starburst`: SAGE26's `collisional_starburst_recipe` for merger and disk-instability bursts, with feedback, history sampling, hydrogen refresh, the instability recheck and the metal yield.
- **`sage_merger_physics.h`** — The pieces of SAGE26's merger outside the black hole and the burst: the mass ratio, morphology and burst channel, `add_galaxies_together`, `make_bulge_from_burst`, the remnant radius (Covington et al. 2011), the bulge-radius reset and `disrupt_satellite_to_ICS`.
- **`sage_merger_clock.h`** — `mimic_sage_estimate_merging_time`: SAGE26's dynamical-friction merger clock `estimate_merging_time`, with its guards and the 998 cap.
- **`sage_halo_concentration.h`** — SAGE26's NFW halo concentration for `ConcentrationOn` 0 and 3: `mimic_sage_concentration_from_vmax_vvir`, `mimic_sage_concentration_mode3_pair`, `mimic_sage_get_halo_concentration` and the `ConcentrationOn` validator.
- **`sage_ffb_regime.h`** — SAGE26's feedback-free-burst classification: the Li et al. (2024) threshold mass and fraction, SAGE26's `halo_uniform` draw of `MostBoundID` and snapshot, and the `EnhancedStarFormationOn` validator.
- **`sage_ffb_feedback_physics.h`** — `mimic_sage_ffb_sn_feedback`: SAGE26's `compute_sn_feedback_ffb`, the feedback-free-burst supernova budget at `FIREmodeOn 1`.
