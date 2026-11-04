# SAGE Shared Utilities

This directory contains helper APIs that are local to the SAGE model package. They are available when Mimic is built with `MODEL=sage26` and should not be treated as framework-wide conventions for other model sets.

If another model needs similar behavior, copy or reimplement the helper in that model package and reconcile the property names, units, parameters, and tests there.

Utility tests are registered in `shared/module_info.yaml`.

## Available Helpers

- **`metallicity.h`** — `mimic_get_metallicity(gas, metals)`: safe metal mass fraction with zero/negative guards and a 1.0 cap (SAGE parity).
- **`central_link.h`** — FoF central index resolution: `mimic_find_fof_central_index`, `mimic_resolve_type2_target_index`, `mimic_resolve_immediate_target_index` for SAGE merge-target traversal.
- **`time_parity.h`** — Substep timestep and midpoint time: `mimic_object_substep_dt` and `mimic_object_substep_time` with strict initial-boundary handling (SAGE parity).
- **`sage_constants.h`** — Shared literal constants: `MergTime` sentinel protocol values (`SAGE_MERGTIME_UNSET`, `SAGE_MERGTIME_CEILING`, `SAGE_MERGTIME_IMMEDIATE`), cold-gas yield threshold, metal-ejection halo-mass scale, virial-temperature coefficient, SAGE26's seconds per gigayear (`SAGE_SEC_PER_GIGAYEAR`, the literal `3.155e16` of `src/macros.h:37`, used by the cooling diagnostics), and the hot-cooling return-path codes (`SAGE_COOLING_PATH_*`) that `sage26_calculate_cooling_budget` carries to `sage26_apply_cooling`.
- **`sage_agn_physics.h`** — BH growth and quasar-mode wind kernels: `mimic_apply_black_hole_growth` and `mimic_apply_quasar_mode_wind` (shared by sage26_starburst_feedback and sage26_quasar_mode).
- **`sage_sf_feedback_physics.h`** — SAGE26's star-formation and supernova-feedback commits, `mimic_sage_update_from_star_formation` and `mimic_sage_update_from_feedback` (`update_from_star_formation` and `update_from_feedback`, `src/model_starformation_and_feedback.c:1119-1251`), and the supernova code constants `mimic_sage_energy_sn_code` and `mimic_sage_eta_sn_code` (`src/core_init.c:102-103`). The one implementation of every star-formation and feedback commit: sage26_apply_star_formation_supernova uses it for disk star formation and `mimic_apply_collisional_starburst` for both burst channels.
- **`sage_starburst_physics.h`** — Collisional starburst kernel: `mimic_apply_collisional_starburst` with SN feedback and metal enrichment (used by sage26_starburst_feedback for both disk-instability and merger channels). It commits its star formation and feedback through `sage_sf_feedback_physics.h`.
- **`sage_disk_instability_physics.h`** — SAGE26's disk-instability stellar transfer, `check_disk_instability` up to its black-hole and burst calls (`src/model_disk_instability.c:47-101`): `mimic_sage_check_disk_instability` applies the Mo, Mao & White (1998) criterion, adds the unstable disk stars and their metals to `BulgeMass`, `MetalsBulgeMass` and `InstabilityBulgeMass` with no cap or clamp, returns the unstable cold-gas fraction, and optionally reports SAGE26's locals; `mimic_sage_apply_disk_instability` is the same without the locals. Used by sage26_disk_instability and by sage26_starburst_feedback's minor-merger instability recheck.
