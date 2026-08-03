# All Configuration Keys — Verified Catalog

Exhaustive per-key reference for Mimic's YAML configuration, transcribed from `src/core/read_parameter_file.c` (1437 lines) on 2026-07-04. If this file and the code disagree, the code wins. Line numbers drift; use the grep commands in the SKILL.md Provenance section to relocate.

## How parsing works (load order and precedence)

`read_parameter_file()` processes the run file in this order:

1. `simulation:` section is peeked first (`set_default_simulation_config_path`): `simulation.name` derives `simulations/<name>/` package paths and the default config path `simulations/<name>/simulation_info.yaml`; an explicit `simulation.config` overrides that path (resolved as-is if absolute or readable, otherwise relative to the run file's directory — `resolve_config_path`).
2. The simulation config file is parsed FIRST (`parse_simulation_config_file`): its `input:`, `output:` (restricted whitelist), and `simulation:` sections load as defaults.
3. Run-file sections then parse in order: `output`, `input`, `model`, `simulation`, `plotting`, top-level `SubSteps`/`TimestepScheme`/`MaxDynamicSubsteps`, `modules`. Any key present in both files takes the run-file value.
4. `validate_and_postprocess()` enforces required keys, model/simulation-vs-binary match, reader/processing-order compatibility, then reads the snapshot list and expands `output.snapshot_list`.

**Unknown-key rejection** (`reject_unknown_keys`): each fixed-schema section is checked against its `valid_keys[]` whitelist; violation is `FATAL_ERROR("Unknown key '%s.%s'", section_name, key)` — e.g. `Unknown key 'output.snapshotlist'`. **TRAP: there is NO whitelist at the top level.** An unknown top-level key — including case typos like `substeps:` instead of `SubSteps:`, or a whole misnamed section — is silently ignored. Only keys INSIDE recognized sections are typo-safe.

All numeric scalars use strict parsers (`get_strict_int_value`, `get_strict_int64_value`, `get_strict_double_value`): trailing junk, overflow, or non-finite values are fatal with the field name in the message.

## `model:` section — valid keys: `name`

| Key | Type | Behavior |
|---|---|---|
| `model.name` | string | Required. Derives `models/<name>/` and `models/<name>/model_properties.yaml`. Validation: must equal the compiled-in `MIMIC_COMPILED_MODEL` or startup errors with "Run file selects model.name='X' but this executable was built with MODEL=Y" |

## `simulation:` section — valid keys: `name`, `config`, `cosmology`, `box_size`, `particle_mass`, `unique_galaxy_id_multiplier`

| Key | Type | Behavior |
|---|---|---|
| `simulation.name` | string | Required. Derives `simulations/<name>/` and `halo_properties.yaml` path. Must equal compiled `MIMIC_COMPILED_SIMULATION` |
| `simulation.config` | string path | Default: `simulations/<name>/simulation_info.yaml`. Pre-loaded before run-file sections so run-file values override its defaults; the run-file `parse_simulation_section` does NOT reload it. May point at a smaller test fixture while keeping the compiled property package fixed |
| `simulation.cosmology` | mapping | Sub-whitelist: `omega_matter`, `omega_lambda`, `hubble_h` (all strict doubles → `MimicConfig.Omega`, `OmegaLambda`, `Hubble_h`). `hubble_h` is required non-zero |
| `simulation.box_size` | unit scalar | Reference label `Mpc/h`. Required non-zero |
| `simulation.particle_mass` | unit scalar | Reference label `1e10 Msun/h` |
| `simulation.unique_galaxy_id_multiplier` | int64 > 0 | Optional. Forest multiplier in `UniqueGalaxyID = halonr + multiplier × (forestnr_global + 1)`. **Default: `TREE_MUL_FAC`** (10⁹, `src/include/constants.h`), seeded once before either parser pass, so the parser assigns only when the key is present — a `simulation_info.yaml` value survives a run file that omits it, and a run-file value wins. Zero or negative is fatal at parse time. **A non-default value is currently accepted only for snapshot-ordered configurations:** every helper in `src/include/galaxy_id.h` is hard-coded to `TREE_MUL_FAC`, so a tree-ordered configuration declaring anything else is rejected by `validate_and_postprocess` instead of silently encoding ids from the compile-time constant. Snapshot readers additionally bounds-check it against the dataset headers at `open_run` (`snapshot_identity_bounds_valid`) |

**Unit scalar form** (`get_unit_scalar_value`): a bare number is taken as already in the reference units. A mapping form accepts ONLY `value`, `units`, `h_convention` (unknown keys fatal); `value` and `units` are required; `h_convention` defaults to the unit label's own convention from the generated unit registry and must be one of `carried`, `free`, `none`. Conversion = cgs ratio × h-convention correction (`× Hubble_h` when converting into `carried`, `÷` when out of it); converting between `none` and an h-dependent convention is fatal.

## `input:` section — valid keys: `first_file`, `last_file`, `tree_name`, `tree_type`, `processing_order`, `simulation_dir`, `snapshot_list_file`, `max_tree_depth`, `forest_distribution_scheme`, `exponent_forest_dist_scheme`

| Key | Type | Behavior |
|---|---|---|
| `first_file` / `last_file` | int | Tree-file number range to process (split across MPI tasks when built with `USE-MPI`) |
| `tree_name` | string | Required unconditionally, but its **meaning is reader-specific** — it is not a general filename pattern. `lhalo_binary`: filename base before the file-number suffix, with the reader's extension appended. Both ctrees readers: a literal filename under `simulation_dir`. `lhalo_hdf5`: an explicit filename, optionally with a `%d` file-number placeholder. `snapshot_hdf5`: a *declaration of the format's fixed convention* — accepted only as the exact literal `snapshot_%03d.h5` (`SNAPSHOT_READER_TREE_NAME`, `src/io/snapshot/reader.h`), every other value rejected by `validate_and_postprocess` with a message naming the literal. The reader builds paths from a fixed internal format string; configured text is never used as a `printf` format |
| `tree_type` | string | Required. Resolved at parse time against **two** registries: `tree_reader_lookup()` (`src/io/tree/registry.c`) first, then `snapshot_reader_lookup()` (`src/io/snapshot/registry.c`). The name sets are disjoint, so the order fixes only which registry answers first. Unknown → FATAL naming both registries and reminding that HDF5 types need an HDF5-enabled build. Exactly one of `MimicConfig.reader` / `MimicConfig.snapshot_reader` is left non-NULL; `TreeExtension` is set only for tree readers. Registered names: forest-ordered `lhalo_binary`, `lhalo_hdf5`, `consistent_trees_ascii`, `consistent_trees_hdf5`; snapshot-ordered `snapshot_hdf5` |
| `processing_order` | string | Case-insensitive `tree_ordered` (default) or `snapshot_ordered`; anything else fatal. Validated against the resolved reader's own declared `processing_order`, whichever registry answered it, so a mismatched reader/order pair is a config error. A correctly paired `snapshot_ordered` configuration now passes configuration validation and fails later at `run_processing_driver()` (`src/core/tree_driver.c`) with "The snapshot-ordered driver is not implemented yet" — distinguishable from a config rejection by the absence of "Parameter validation failed". The snapshot files are **not** opened on this path: the reader's `open_run` dataset validation has no caller in `src/` until the Phase 5 driver, so input errors go undetected |
| `simulation_dir` | string | Required. Directory containing the tree files |
| `snapshot_list_file` | string | Required. Path to the `.a_list` scale-factor file; line count defines `MAXSNAPS` |
| `max_tree_depth` | int | Default 500 (seeded in `parse_cli`). Recursion guard for `build_halo_tree` |
| `forest_distribution_scheme` | string | `uniform` (default), `linear`, `quadratic`, `exponent`, `generic_power`; unknown value fatal listing the valid five. Consistent-Trees forest→task load balancing only; other readers ignore it, and the ASCII reader always splits uniformly |
| `exponent_forest_dist_scheme` | double | Default 0.7. Exponent for the power-law schemes |

## `output:` section — valid keys: `output_filename`, `output_directory`, `output_format`, `snapshot_list`, `target_file_size_mb`, `forests_per_file`

| Key | Type | Behavior |
|---|---|---|
| `output_filename` | string | Required. Output base name |
| `output_directory` | string | Required. Destination for output files and the run-local `metadata/` |
| `output_format` | string | `binary` or `hdf5` (case-insensitive). `hdf5` in a non-HDF5 build is fatal with "Recompile with HDF5 enabled (default) or remove USE-HDF5=no". **Quirk (verified):** any OTHER string is silently ignored — no else-branch — leaving the zero-initialized default `output_binary`. Do not rely on this; spell it correctly |
| `snapshot_list` | int sequence | Which snapshots to output. Empty or omitted → ALL snapshots (`validate_output_snapshots` fills 0..MAXSNAPS-1 and logs "All N snapshots selected"). Max `ABSOLUTEMAXSNAPS` (1000) entries; each must be in `[0, LastSnapshotNr]`; duplicates fatal |
| `target_file_size_mb` | int64 > 0 | Stored as bytes in `TargetFileSize`; drives forest chunking into output files |
| `forests_per_file` | int64 ≥ 0 | Alternative chunking control |

## Simulation-owned `output:` defaults (inside simulation_info.yaml) — valid keys: `target_file_size_mb`, `forests_per_file` ONLY

`parse_simulation_output_section` (section name in errors: "simulation output defaults") accepts just the two chunking keys. Output destinations, format, and snapshot selection are deliberately run-file-only concerns — putting `output_directory` in a simulation_info.yaml is fatal.

## `plotting:` section — valid keys: `profile`

`plotting.profile` is a repo-relative path to a plot profile YAML. Validation: a leading `/` errors "plotting.profile must be package-relative, not absolute". Profile stack semantics belong to the `mimic-plots-and-analysis` skill.

## Top-level timestep keys (not in any section)

| Key | Type | Default | Behavior |
|---|---|---|---|
| `SubSteps` | int | 1 | Fixed scheme: substeps per snapshot interval. Dynamic scheme: substeps per dynamical time (resolution) |
| `TimestepScheme` | string | `fixed` | Case-insensitive `fixed` or `dynamic`; unknown value fatal; non-scalar fatal |
| `MaxDynamicSubsteps` | int ≥ 1 | 200 (`DEFAULT_MAX_DYNAMIC_SUBSTEPS`, `src/include/constants.h`) | Ceiling only. `compute_dynamic_substeps` (`src/core/timestep.c`): `n = ceil(dt × SubSteps / t_dyn)` clamped to `[1, MaxDynamicSubsteps]`, `t_dyn = Rvir/Vvir`, evaluated per FoF group; non-finite or non-positive inputs fall back to 1 |

## `modules:` section — accepted keys: `pre_timestep`, `phases`, `post_timestep`, `parameters`

Unknown keys here get a dedicated fatal: `Unknown key 'modules.<key>'; supported keys are pre_timestep, phases, post_timestep, parameters` (this deliberately kills stale pre-multi-phase forms like `phase_1:`/`enabled:`).

- `pre_timestep` / `post_timestep`: module sequences run once per snapshot around the substep loop. An absent key, explicit `null`/`~`, or a fully commented-out list all count as a valid empty phase.
- `phases`: an ordered mapping `phase_name -> module sequence`. libyaml preserves mapping order, so phases execute in declared order. Phase names must be non-empty, unique, shorter than `MAX_STRING_LEN`, and not one of the reserved words `pre_timestep`, `post_timestep`, `parameters`, `phases`. Hard cap `MAX_SUBSTEP_PHASES` = 32 (`src/core/module_registry.h`).
- Each module entry is a single-pair mapping `module_name: mode` where mode is exactly `process_full_halo`, `process_per_event`, or `process_by_galaxy`; anything else is an error listing the three. Whether a module SUPPORTS a mode is validated later by `module_system_init` — see the `mimic-modules` skill.
- `parameters`: flat mapping `param_name: value`, max `MAX_MODEL_PARAMS` = 256 entries, stored as strings and interpreted by typed getters (`model_get_double/int/string`) during module `init()`. There are NO defaults: a parameter a module asks for that is missing here fails that module's init.

## Ownership matrix: simulation_info.yaml vs run file

| Axis | simulation_info.yaml (defaults) | Run file (overrides) |
|---|---|---|
| `input.*` (all ten keys) | yes — typical home for tree_name, tree_type, simulation_dir, snapshot_list_file, file range | yes — any key can be overridden |
| `simulation.cosmology`, `box_size`, `particle_mass`, `unique_galaxy_id_multiplier` | yes — canonical home | yes, but overriding catalog physics is almost always wrong |
| `output.target_file_size_mb`, `forests_per_file` | yes (the ONLY output keys allowed) | yes |
| `output.output_filename/directory/format/snapshot_list` | NO — fatal if present | run-file only |
| `model.*`, `plotting.*`, `SubSteps`, `TimestepScheme`, `MaxDynamicSubsteps`, `modules.*` | not parsed from simulation config | run-file only |

## Required keys (enforced by `validate_and_postprocess`)

`output.output_directory`, `output.output_filename`, `model.name` (must match binary), `simulation.name` (must match binary), a resolvable `simulation.config`, `input.simulation_dir`, `input.tree_name`, `input.tree_type` (recognized), `input.snapshot_list_file`, `simulation.box_size` non-zero, `simulation.cosmology.hubble_h` non-zero. All errors are accumulated and reported together before the single fatal "Parameter validation failed", so fix everything listed in one pass.
