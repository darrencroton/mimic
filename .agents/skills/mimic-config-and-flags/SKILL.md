---
name: mimic-config-and-flags
description: Catalog of every Mimic configuration axis and how to look one up or add one. Load when a task mentions a Make variable (MODEL, SIMULATION, SIM, USE-HDF5, USE-MPI, TEST_BUILD, EXTRA_CFLAGS), a CLI flag (--verbose, --debug, --quiet, --skip, --compress), a run-file YAML key (model.name, simulation.*, input.*, output.*, SubSteps, TimestepScheme, MaxDynamicSubsteps, plotting.profile, modules.*), simulation_info.yaml defaults, parameter_units.yaml, an "Unknown key" error, environment variables (MIMIC_BASELINE_RTOL, MIMIC_TEST_BUILD, NO_COLOR), changing project defaults, or adding a new configuration option/parameter axis.
---

# Mimic Configuration and Flags

The lookup catalog for every knob Mimic exposes: compile-time Make variables, invocation-time CLI flags, run-time YAML keys, simulation-package defaults, model parameter unit declarations, and environment variables — plus the checklist for adding a new configuration axis. A "run file" is the YAML file passed to `./mimic` (e.g. `models/sage16/input/sage16_mini-millennium.yaml`).

## When to use / when NOT to use

Use this skill to: look up what a configuration key means, its type, default, allowed values, and which layer owns it; resolve an `Unknown key 'section.key'` fatal; decide where a new option belongs and wire it in end to end.

Do NOT use it for:

- Build workflows, library detection, environment setup — see the `mimic-build-and-env` skill.
- Running Mimic and interpreting output artifacts — see the `mimic-run-and-operate` skill.
- Module metadata axes (`module_info.yaml` keys, processing modes in depth, events) — see the `mimic-modules` skill; this skill only catalogs the run-file `modules:` section shape.
- The plot profile stack and plotting CLI — see the `mimic-plots-and-analysis` skill.
- Test tolerances and baseline policy behind `MIMIC_BASELINE_RTOL` — see the `mimic-validation-and-qa` skill.
- Deciding whether a config change needs review/validation gates — see the `mimic-change-control` skill.

## First actions

1. Unknown key or unsure of a key's semantics? Check the tables below, then `references/all-config-keys.md` in this skill for the exhaustive per-key catalog.
2. Still unsure, or suspect drift? The single source of truth for run-file keys is `src/core/read_parameter_file.c` — every section has a `static const char *const valid_keys[]` array and a `parse_*` function next to it: `grep -n "valid_keys\[\]" src/core/read_parameter_file.c`.
3. For CLI flags and their runtime defaults: `grep -n "parse_cli" -A 60 src/core/main.c`.
4. For Make variables: `grep -n "DEFAULT_MODEL\|DEFAULT_SIMULATION\|USE-HDF5\|USE-MPI" Makefile` or `make help`.
5. Gather this evidence before editing anything.

## The four configuration layers and their precedence

Configuration flows through four layers, applied in this order (later layers win where they overlap):

1. **Makefile selectors and flags** (compile-time): `MODEL=`, `SIMULATION=`, `USE-HDF5=`, etc. decide what is compiled into the binary. The run file cannot override these — running a `sage16` binary with a `sham` run file fails fast at startup because `model.name` must match the compiled model.
2. **`simulations/<sim>/simulation_info.yaml`** (catalog defaults): per-simulation cosmology, box size, particle mass, input file layout, and output defaults (`target_file_size_mb`, `forests_per_file`). Loaded first when the run file names the simulation.
3. **Run-file YAML** (run-time): overrides the simulation defaults for `input:`/`output:` keys and supplies everything simulation_info cannot know (model parameters, module pipeline, snapshot selection, timestepping).
4. **CLI flags** (invocation-time): logging verbosity, `--skip`, `--compress` — never stored in YAML; there is deliberately no YAML compression key.

The safety net at the YAML layers is unknown-key rejection: each fixed-schema section is checked against a `valid_keys[]` whitelist and any stray key is fatal with the exact message `Unknown key '<section>.<key>'` (from `reject_unknown_keys` in `src/core/read_parameter_file.c`) — e.g. `Unknown key 'output.snapshotlist'`. **TRAP: the top level of the run file has NO whitelist.** A misspelled top-level key (`substeps:` instead of `SubSteps:`, or a whole misnamed section) is silently ignored; only keys inside recognized sections are typo-safe. Required-key validation at startup (`validate_and_postprocess`) is the backstop — it accumulates ALL missing/invalid required keys and reports them together before one fatal "Parameter validation failed".

## Make variables (compile-time)

Verified against the `Makefile` (2026-07-04). Full build workflow and library detection belong to the `mimic-build-and-env` skill.

| Variable | Default | Meaning |
|---|---|---|
| `MODEL` | `DEFAULT_MODEL` = `sage16` | Model package to compile (`models/<MODEL>/`). Unknown package → `$(error Unknown MODEL ...)` |
| `SIMULATION` | `DEFAULT_SIMULATION` = `mini-millennium` | Simulation/catalog property package (`simulations/<SIMULATION>/`). Unknown → `$(error Unknown SIMULATION ...)` |
| `SIM` | — | Shorthand for `SIMULATION`; an explicit `SIMULATION=` wins |
| `USE-HDF5` | `yes` | HDF5 readers/writers; `USE-HDF5=no` opts out |
| `USE-MPI` | off | Compile with `mpicc -DMPI` for parallel file processing |
| `CC` | auto-detected | Override the compiler (e.g. a specific MPI wrapper) |
| `EXTRA_CFLAGS` | empty | Extra compiler flags appended to the standard set |
| `TEST_BUILD` | off | `TEST_BUILD=yes` compiles the `test_fixture` infrastructure module (defines `MIMIC_TEST_BUILD=1`) |
| `summary` | — | Goal *modifier*, not a variable: `make tests summary` sets `TEST_SUMMARY=1` to filter test output to failures/skips/warnings |

Guards and conventions:

- **Lowercase typo guards**: `make model=...`, `simulation=...`, or `sim=...` all error immediately with "Make variables are case-sensitive. Did you mean: make MODEL=... ?". Make variable names are case-sensitive, so without the guard these would be silently ignored.
- **MODEL_FREE_TARGETS** — targets that skip the package-existence checks: `clean tidy help check-docs check-format test-clean summary`. Everything else validates that both package directories exist before doing anything.
- **Selector consistency**: use the SAME `MODEL=<name> SIMULATION=<name>` pair across `make generate`, `make validate-modules`, `make tests*`, and `make`. Mixing selectors produces silently inconsistent generated code.

**Changing the project defaults**: edit `DEFAULT_MODEL` / `DEFAULT_SIMULATION` in the `Makefile`. `scripts/lib/defaults.sh` derives the shell-side defaults from the Makefile (it greps those lines), so no second edit is needed there — but update the run file you use by default so its `model.name`/`simulation.name` match, or startup validation fails.

## CLI flags (invocation-time)

`./mimic [flags] <run.yaml>` — verified against `parse_cli` in `src/core/main.c`. There is **no `--version` flag** (version info goes into output metadata instead).

| Flag | Effect |
|---|---|
| `-h`, `--help` | Print usage and exit |
| `-v`, `--verbose` | Add context (timestamp, file:line) and enable `VERBOSE_LOG` |
| `-d`, `--debug` | Most verbose: debug output plus context |
| `-q`, `--quiet` | Warnings and errors only |
| `--skip` | Skip a work partition if ALL of its output files already exist; a PARTIAL set of existing files is a fatal error (sets `OverwriteOutputFiles = 0`) |
| `--compress` | gzip HDF5 galaxy datasets (sets `HDF5CompressionLevel = 1`; default 0 = off) |

`parse_cli` also seeds runtime defaults that YAML keys later override:

| Runtime default | Value | Overridden by |
|---|---|---|
| `OverwriteOutputFiles` | 1 (overwrite) | `--skip` |
| `HDF5CompressionLevel` | 0 (off) | `--compress` |
| `MaxTreeDepth` | 500 | `input.max_tree_depth` |
| `ForestDistributionScheme` | 0 (`uniform_in_forests`) | `input.forest_distribution_scheme` |
| `Exponent_Forest_Dist_Scheme` | 0.7 | `input.exponent_forest_dist_scheme` |

## Run-file YAML keys (run-time)

Summary table; full per-key semantics, parse behavior, and the simulation_info-vs-run-file ownership matrix are in `references/all-config-keys.md`. "Sim-legal" = the key may also appear in `simulations/<sim>/simulation_info.yaml` as a default that the run file overrides.

| Key | Type | Default | Notes |
|---|---|---|---|
| `model.name` | string | required | Must match the compiled model (`MIMIC_COMPILED_MODEL`) or startup fails |
| `simulation.name` | string | required | Selects `simulations/<name>/`; loads its `simulation_info.yaml` |
| `simulation.config` | string | from sim pkg | Path to the simulation_info file (rarely overridden) |
| `simulation.cosmology.omega_matter` | double | sim pkg | Ωm |
| `simulation.cosmology.omega_lambda` | double | sim pkg | ΩΛ |
| `simulation.cosmology.hubble_h` | double | sim pkg | little-h |
| `simulation.box_size` | unit scalar | sim pkg | `{value, units, h_convention}` map or bare number |
| `simulation.particle_mass` | unit scalar | sim pkg | same form as box_size |
| `simulation.unique_galaxy_id_multiplier` | int64 > 0 | `TREE_MUL_FAC` (10⁹) | Forest multiplier for `UniqueGalaxyID`. Sim-legal and the canonical home; a package value survives a run file that omits the key. **A non-default value is currently accepted only for snapshot-ordered configurations** — the tree-ordered encoder in `src/include/galaxy_id.h` is still hard-coded to `TREE_MUL_FAC`, so a tree-ordered run declaring anything else is rejected at startup |
| `input.first_file` | int | sim pkg | First tree-file number to process |
| `input.last_file` | int | sim pkg | Last tree-file number |
| `input.tree_name` | string | sim pkg | Reader-specific meaning, not a general pattern: filename base for `lhalo_binary`, a literal filename for the ctrees readers, an explicit name or `%d` pattern for `lhalo_hdf5`, and for `snapshot_hdf5` exactly the literal `snapshot_%03d.h5` (anything else rejected at startup) |
| `input.tree_type` | string | sim pkg | On-disk reader format, resolved against two registries — forest-ordered (`src/io/tree/registry.c`): `lhalo_binary`, `lhalo_hdf5`, `consistent_trees_ascii`, `consistent_trees_hdf5`; snapshot-ordered (`src/io/snapshot/registry.c`): `snapshot_hdf5`. Names are disjoint across the two |
| `input.processing_order` | string | `tree_ordered` | `tree_ordered` or `snapshot_ordered`, validated against the resolved reader's declared order. A correctly paired `snapshot_ordered` run clears configuration, then FATALs "not implemented yet" at the driver — before its dataset is opened, so input errors are not detected. Never overload `tree_type` with ordering meaning |
| `input.simulation_dir` | string | sim pkg | Directory holding the tree files |
| `input.snapshot_list_file` | string | sim pkg | Path to the `.a_list` scale-factor file |
| `input.max_tree_depth` | int | 500 | Recursion guard for `build_halo_tree` |
| `input.forest_distribution_scheme` | string | `uniform` | Forest→file balancing (ctrees readers): `uniform`, `linear`, `quadratic`, `exponent`, `generic_power` |
| `input.exponent_forest_dist_scheme` | double | 0.7 | Exponent for the power-law schemes |
| `output.output_filename` | string | required | Output file base name |
| `output.output_directory` | string | required | Where outputs + `metadata/` go |
| `output.output_format` | string | required | `binary` or `hdf5` |
| `output.snapshot_list` | int list | all snapshots | Which snapshots to write; empty/omitted = all |
| `output.target_file_size_mb` | number | sim pkg | Sim-legal; drives forest chunking into output files |
| `output.forests_per_file` | int | sim pkg | Sim-legal; alternative chunking control |
| `SubSteps` | int (top-level) | 1 | Fixed scheme: substeps per snapshot interval; dynamic scheme: substeps per dynamical time (resolution knob) |
| `TimestepScheme` | string (top-level) | `fixed` | `fixed` or `dynamic` |
| `MaxDynamicSubsteps` | int (top-level) | 200 | ≥1. Safety ceiling, NOT a resolution target: dynamic scheme computes `n = ceil(dt × SubSteps / t_dyn)` clamped to `[1, MaxDynamicSubsteps]`, with `t_dyn = Rvir/Vvir`, per FoF group (`src/core/timestep.c`) |
| `plotting.profile` | string | none | Repo-relative path ONLY to a plot profile YAML; absolute paths rejected |
| `modules.pre_timestep` | list | optional | Modules run once per snapshot before substepping |
| `modules.phases` | ordered map | required for physics | User-named substep phases, executed in YAML order, max 32; each maps to module lists with modes `process_full_halo` / `process_per_event` / `process_by_galaxy` |
| `modules.post_timestep` | list | optional | Modules run once per snapshot after substepping |
| `modules.parameters` | map | per model | Flat `name: value` map read by `model_get_double/int/string`; no defaults — a missing parameter fails module init |

**Unit scalar form** (`box_size`, `particle_mass`): either a bare number — interpreted as already in Mimic's reference units (mass 1e10 Msun/h, length Mpc/h) — or a map `{value: <num>, units: <label>, h_convention: <carried|free|none>}` converted at load. Only `value`, `units`, `h_convention` are accepted in the map (unknown keys fatal).

Unknown keys in `model:`, `simulation:`, `simulation.cosmology:`, `input:`, `output:`, `plotting:`, and simulation_info's output-defaults section are all fatal. The `modules:` section rejects anything other than `pre_timestep`, `post_timestep`, `parameters`, `phases` with its own message listing the supported keys.

## parameter_units.yaml (model packages)

Optional per-model file `models/<model>/parameter_units.yaml` declaring physical units for `modules.parameters` entries so users can write natural units in run files:

- Schema: a `parameters:` list of `{name, type, units, h_convention}`; `type` is `double` only.
- Modules read converted values via the `*_INTERNAL` macro variants in `src/module_system/parameter_helpers.h` (they apply `mimic_parameter_unit_factor` at load).
- Any parameter NOT listed is assumed to already be in reference units — no conversion.
- Currently `sham` has this file; `sage16` does not (all sage16 parameters are reference-unit or dimensionless). Verify: `ls models/*/parameter_units.yaml`.

Generated conversion code lands in `src/include/generated/parameter_unit_conversions.h` via `make generate` — never hand-edit it. See the `mimic-properties` skill for the unit registry and h-convention machinery.

## Environment variables

| Variable | Consumer | Effect |
|---|---|---|
| `MIMIC_BASELINE_RTOL` | `tests/framework/harness.py` | Overrides the baseline comparison relative tolerance (default 1e-6); CI sets 1e-3. Policy: see the `mimic-validation-and-qa` skill |
| `MIMIC_TEST_BUILD` | `scripts/discovery.py`, exported by the Makefile | The Makefile exports `MIMIC_TEST_BUILD=1` when `TEST_BUILD=yes`; `tests/unit/run_tests.sh` sets it directly. Selects the test-instrumented build (compiles the `test_fixture` module) |
| `NO_COLOR` | `scripts/console.py`, `scripts/lib/colors.sh` | Any value disables ANSI color in script output (honors the no-color.org convention) |

## Adjacent configuration axes (owned by sibling skills)

- Module metadata (`module_info.yaml` keys, `supported_processing_modes`, events, dependencies) → see the `mimic-modules` skill.
- Plot profile stack and inline plotting overrides consumed by `mimic-plot.py` → see the `mimic-plots-and-analysis` skill.
- Property YAML schemas (`core_properties.yaml`, `halo_properties.yaml`, `model_properties.yaml`) → see the `mimic-properties` skill.

## Adding a configuration axis (checklist)

Grounded in how the dynamic-timestep configuration surface (`TimestepScheme`, then `MaxDynamicSubsteps`) was added — see `git show --stat 469b7adc` for a real touchpoint set: `src/include/types.h` (field + enum), `src/include/constants.h` (cap constant), `src/include/proto.h` (helper exposure), `src/core/read_parameter_file.c` (parse + validate + log), and `tests/unit/test_parameter_parsing.c` (default, explicit values, invalid value, error-message, non-scalar coverage). For a new run-file key:

1. **Parser**: add the key to the section's `valid_keys[]` array AND parse it in the matching `parse_*` function in `src/core/read_parameter_file.c` (a whitelist entry without parsing silently ignores the value; parsing without the whitelist entry makes the key fatal).
2. **Storage**: add a field to `MimicConfig` in `src/include/types.h`.
3. **Default**: define a named constant in `src/include/constants.h` (e.g. `DEFAULT_MAX_DYNAMIC_SUBSTEPS`) and apply it before parsing so an omitted key is well-defined.
4. **Validation**: fast-fail on invalid values at parse time with a `FATAL_ERROR` naming the key and the accepted range/values — never defer to a mysterious downstream crash.
5. **Consume**: read the field where the behavior lives (e.g. `src/core/timestep.c`).
6. **Document**: add the key to `docs/USER-GUIDE.md` and to this skill's tables + `references/all-config-keys.md`.
7. **Provenance decision**: should the value be recorded in output metadata? HDF5 runs write parameters into `RunProperties` (`src/io/output/metadata_hdf5.c`); decide explicitly whether your key belongs there for reproducibility.
8. **Test**: add/extend a test covering default, valid override, and invalid value (tier selection: see the `mimic-validation-and-qa` skill).

For a new Make variable, mirror the existing patterns: default + lowercase typo guard if user-facing, `make help`/`make info` text, and `mimic-build-and-env` skill update. For a new model parameter (not a framework axis), declare it in `module_info.yaml` and load it in module `init()` — see the `mimic-modules` skill.

## Provenance and maintenance

All facts verified against the live repo on 2026-07-04. Re-verify before trusting any table that may have drifted:

```bash
# Run-file sections and their whitelists (THE source of truth)
grep -n "valid_keys\[\]" src/core/read_parameter_file.c
# CLI flags and runtime defaults
grep -n "parse_cli" -A 60 src/core/main.c
# Make defaults, shorthand, typo guards, model-free targets
grep -n "DEFAULT_MODEL\|DEFAULT_SIMULATION\|MODEL_FREE_TARGETS\|ifdef model\|ifdef SIM" Makefile
# Timestep keys and dynamic-substep formula
grep -n "SubSteps\|TimestepScheme\|MaxDynamicSubsteps" src/core/read_parameter_file.c src/core/timestep.c src/include/constants.h
# Which models declare parameter units
ls models/*/parameter_units.yaml
# Environment variables
grep -rn "MIMIC_BASELINE_RTOL" tests/framework/harness.py; grep -n "NO_COLOR" scripts/console.py scripts/lib/colors.sh
# Registered reader names (the valid input.tree_type values), both registries
grep -n "\.name = " src/io/tree/*.c src/io/snapshot/*.c
# The identity multiplier: whitelist entry, parse, default seeding, tree-ordered rejection
grep -n "unique_galaxy_id_multiplier\|UniqueGalaxyIDMultiplier" src/core/read_parameter_file.c src/include/types.h
```

If a `valid_keys[]` array and the tables here disagree, the code wins — update this skill and `references/all-config-keys.md`.
