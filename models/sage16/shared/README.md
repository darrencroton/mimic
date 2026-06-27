# SAGE Shared Utilities

This directory contains helper APIs that are local to the SAGE model package. They are available when Mimic is built with `MODEL=sage16` and should not be treated as framework-wide conventions for other model sets.

If another model needs similar behavior, copy or reimplement the helper in that model package and reconcile the property names, units, parameters, and tests there.

Utility tests are registered in `shared/module_info.yaml`.

## Available Helpers

- **`metallicity.h`** — `mimic_get_metallicity(gas, metals)`: safe metal mass fraction with zero/negative guards and a 1.0 cap (SAGE parity).
- **`central_link.h`** — FoF central index resolution: `mimic_find_fof_central_index`, `mimic_resolve_type2_target_index`, `mimic_resolve_immediate_target_index` for SAGE merge-target traversal.
- **`time_parity.h`** — Substep timestep and midpoint time: `mimic_object_substep_dt` and `mimic_object_substep_time` with strict initial-boundary handling (SAGE parity).
- **`sage_constants.h`** — Shared literal constants: `MergTime` sentinel protocol values (`SAGE_MERGTIME_UNSET`, `SAGE_MERGTIME_CEILING`, `SAGE_MERGTIME_IMMEDIATE`), cold-gas yield threshold, metal-ejection halo-mass scale, and virial-temperature coefficient.
- **`sage_agn_physics.h`** — BH growth and quasar-mode wind kernels: `mimic_apply_black_hole_growth` and `mimic_apply_quasar_mode_wind` (shared by sage_starburst_feedback and sage_quasar_mode).
- **`sage_starburst_physics.h`** — Collisional starburst kernel: `mimic_apply_collisional_starburst` with SN feedback and metal enrichment (used by sage_starburst_feedback for both disk-instability and merger channels).
- **`sage_disk_instability_physics.h`** — Disk-instability structural response: `mimic_sage_apply_disk_instability` applies the Efstathiou stability criterion and returns the unstable gas fraction.
