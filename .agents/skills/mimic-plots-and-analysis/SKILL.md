---
name: mimic-plots-and-analysis
description: Mimic's plotting system and output analysis. Load when a task involves mimic-plot.py or its flags, files under plot/mimic-plot or model package plot directories (figures, profiles), adding or modifying a diagnostic figure, plot profiles and axis limits (inherits, axes, xmin/xmax), skipped plots or missing plots, plot tests (test_plotting.sh), reading galaxy output for analysis (output_schema.py, hdf5_reader), plotting native SAGE output for comparison (--input-format=sage-hdf5), observational data overlays, or "make a plot of X from a Mimic run".
---

# Mimic Plots and Analysis

The plotting engine lives in `plot/mimic-plot/` and is model-agnostic; the figures are model-local Python modules under `models/<model>/plots/figures/`, selected and styled by a stack of profile YAMLs. Figures never touch files — the engine hands every figure the same NumPy recarray regardless of output format. This skill covers running, extending, and debugging that system.

## When to use / when NOT to use

Use for: plot generation, figure authoring, profile/axis configuration, skipped-plot diagnosis, plot tests, programmatic output reading for analysis.

Do NOT use for:
- What a plot means scientifically and which physics moves it — see the `mimic-sam-reference` skill (and its references/diagnostics-interpretation.md).
- Producing the run itself — see the `mimic-run-and-operate` skill.
- Claiming scientific results from plots — plots are downstream evidence; see the `mimic-scientific-method` skill (numbers before claims).
- Property/schema changes that add plottable fields — see the `mimic-properties` skill.

## First actions

1. `source mimic_venv/bin/activate` — every plotting command needs the venv.
2. Read `model.name`, `simulation.name`, `output.output_directory`, `output.output_filename`, and `output.output_format` from the parameter file. Confirm the run output was produced by the same model/simulation pair and that `<output_dir>/metadata/output_schema.json` exists.
3. Confirm the data files exist before plotting: HDF5 runs need `<output_dir>/<base>.hdf5` and/or `<output_dir>/<base>_*.hdf5`; binary runs need `<output_dir>/<base>_z*_*`. If output is absent, load `mimic-run-and-operate` and produce the run first.
4. Baseline invocation, always with `--verbose` when anything is unclear (skips are silent without it):

```bash
python plot/mimic-plot/mimic-plot.py --param-file=models/sage16/input/sage16_mini-millennium.yaml --verbose
```

4. Before editing a figure, read `models/<model>/plots/figures/__init__.py` (the registry) and one existing figure of the same kind (snapshot: `stellar_mass_function.py` is the reference implementation).

## 1. Running the engine

`plot/mimic-plot/mimic-plot.py` flags (verified against `parse_arguments`): `--param-file` (required), `--first-file`/`--last-file` (override the run file), `--snapshot=<n>` (one snapshot), `--all-snapshots`, `--snapshot-plots` / `--evolution-plots` (each alone restricts to that kind; neither = both), `--output-dir` (default `<OutputDir>/plots`), `--format` (default `.png`), `--plots=name1,name2` (default all), `--use-tex`, `--verbose`/`-v`, `--quiet`/`-q` (mutually exclusive), `--input-format={mimic,sage-hdf5}`.

`--input-format=sage-hdf5` reads native sage-model HDF5 output for side-by-side comparison: it maps SAGE names to Mimic's (`EjectedMass→EjectedGas`, `IntraClusterStars→ICS`, etc.), rebuilds Pos/Vel/Spin vectors, sums `SfrDisk+SfrBulge→StarFormationRate`, and zero-fills Mimic-only fields (which then trigger normal skip logic).

## 2. The model-local figure registry

`models/<model>/plots/figures/__init__.py` exports four structures the engine imports (plus shared styling helpers `setup_plot_fonts`, `setup_legend`, label getters, and `check_required_properties`):

- `SNAPSHOT_PLOTS` / `EVOLUTION_PLOTS` — ordered name lists (sage16 ships 18 + 4; sham 8 + 3; halos-only 4 + 1, halo-diagnostics only by design).
- `PLOT_REQUIREMENTS` — dict name → required galaxy fields (`[]` = always available). This is the gate that makes physics-free runs skip galaxy plots cleanly.
- `PLOT_FUNCS` — dict name → the figure module's `plot` callable.

## 3. The profile stack (what plots run, with which axes)

Profiles merge in this order, later wins (verified in `configure_plot_profile`):

1. `plot/mimic-plot/profiles/default.yaml` (global base; empty lists)
2. `models/<M>/plots/profiles/default.yaml` (model's full plot list)
3. `simulations/<S>/plot_profile.yaml` (simulation-level defaults, if present)
4. `models/<M>/plots/profiles/<SimulationName>_plot_profile.yaml` (model+simulation match)
5. Run-file `plotting.profile` (repo-relative path; optional extra override)
6. Inline plotting overrides in a plotting-only parameter file: any extra keys under `plotting:` besides `profile` are copied by `mimic-plot.py` into its internal `PlottingOverrides` dict and merged as the most specific layer. The C executable only accepts `plotting.profile` and will reject extra `plotting:` keys, so do not add inline overrides to a run file you also pass to `./mimic`; validate plotting-only files with `mimic-plot.py --verbose`.

`inherits:` entries inside a profile resolve relative to the declaring file's directory (so package profiles inherit `default.yaml` by local name); inheritance cycles raise an error; diamonds are allowed. The active profile's `plots.snapshot`/`plots.evolution` lists intersect with the registry — a registered figure absent from the profile doesn't run unless named via `--plots`.

**KNOWN TRAP — axis keys**: `get_profile_axes()` (`plot/mimic-plot/output_utils.py`) reads scalar keys `xmin`/`xmax`/`ymin`/`ymax` under `axes.<plot>` (with `ymin`/`ymax` interpreted as log10 values when the figure passes `log_y=True`). Several shipped profiles instead carry list-form `xlim: [a, b]` / `ylim: [a, b]` keys, which the code NEVER reads — those overrides are silently ignored and figures fall back to their hard-coded defaults. When an axis override "doesn't work", check the key spelling first. Use `xmin`/`xmax`/`ymin`/`ymax` in new profile entries; treat the shipped `xlim`/`ylim` entries as a known repo inconsistency (report it, don't copy it).

## 4. The figure contract

One Python module per figure in `models/<model>/plots/figures/`. Signatures:

```python
# snapshot figure
def plot(galaxies, volume, metadata, params, output_dir="plots", output_format=".png", verbose=False):
    ...
    return plot_path, None          # exactly one of (path, skip_message) is non-None

# evolution figure: snapshots = {snap_num: (galaxies, volume, metadata)}
def plot(snapshots, params, output_dir="plots", output_format=".png", verbose=False):
```

`galaxies` is a NumPy recarray (same shape from binary, HDF5, and sage-native readers — figures are format-agnostic); `volume` is (Mpc/h)³ already scaled by the file fraction processed; `metadata` carries `hubble_h`, `box_size`, `redshift`, `schema_units`. Masses arrive in code units (`1e10 Msun/h`); figures convert (`* 1.0e10 / hubble_h`) at plot time.

Validation-first pattern (the house style — see `stellar_mass_function.py`): `check_required_fields` → `check_field_has_values` → filter → `validate_filtered_data` (or `validate_evolution_snapshot` per snapshot) → only then `setup_figure()` → draw → `save_and_close_figure()`. Helpers live in `plot/mimic-plot/output_utils.py` (including `calculate_mass_function` for the standard φ = N/V/Δlog M) and the model `figures` package (fonts, legends, labels). Observational overlays are inline NumPy arrays inside the figure modules (Baldry 2008 SMF, etc.) with the run's `hubble_h` and `WhichIMF` corrections applied at plot time — there are no separate data files.

## 5. Skip diagnostics

Two distinct skip paths (both summarized at the end of a run; per-plot reasons only with `--verbose`):

1. **Missing-property skip** (pre-call): a figure whose `PLOT_REQUIREMENTS` fields are absent from the data is skipped with a printed field list and the hint to enable physics modules. Expected for physics-free/halos-only runs.
2. **Validation skip** (in-call): the figure returned `(None, reason)` — e.g. "No data found after filtering", "All values in 'X' are <= 0". Expected when a run is tiny or a field is legitimately all-zero.

A third failure mode: an exception inside a figure is caught and reported as `Error generating <name>: ...` without killing the run. And one trap: stale `__pycache__/*.pyc` under `figures/` can shadow reality after a figure is removed — clean pycache when the registry and disk disagree.

## 6. Adding a figure

1. Create `models/<model>/plots/figures/<new_plot>.py` with the correct signature and the validation-first pattern; use `get_profile_axes` for limits; return `(path, skip_msg)`.
2. Register in `figures/__init__.py`: `from . import <new_plot>`; append to `SNAPSHOT_PLOTS` or `EVOLUTION_PLOTS`; add `PLOT_REQUIREMENTS["<new_plot>"]`; add `PLOT_FUNCS["<new_plot>"] = <new_plot>.plot`.
3. Add the name to the model's profile lists (`plots/profiles/default.yaml` and any simulation-specific profile) — registry membership alone doesn't run it.
4. Test: `python plot/mimic-plot/mimic-plot.py --param-file=<run.yaml> --plots=<new_plot> --verbose`, then the plot suite (section 7). Model plots belong in the model package — never in `plot/mimic-plot/`.

## 7. Plot tests

`plot/mimic-plot/tests/test_plotting.sh` (needs venv + real run output; uses the Makefile defaults via `scripts/lib/defaults.sh`; `$PARAM_FILE` overridable) drives five CLI invocations plus the Python unit tests: `test_validation_helpers.py`, `test_profile_inheritance.py`, `test_sage_native_hdf5.py`, `test_snapshot_redshift_mapper.py`, `test_chunked_consumers.py` — each runnable directly with `python3 plot/mimic-plot/tests/test_<name>.py`.

## 8. Programmatic analysis (outside the plot engine)

For ad-hoc analysis, reuse the engine's readers rather than hand-rolling: `plot/mimic-plot/output_schema.py` (`load_schema`, `dtype_from_schema(binary=True)`, `units_from_schema`, `descriptions_from_schema`, `mass_to_msun`) for binary; `plot/mimic-plot/hdf5_reader.py` or plain h5py for HDF5; the auto-generated `example_Mvir_Len_plot.py` in every output directory as a dependency-light starting point. Reading recipes: `mimic-run-and-operate`; measurement/validation helpers: `mimic-diagnostics-and-tooling`.

## Provenance and maintenance

Verified against the live repo 2026-07-04. Re-verify drift-prone specifics:

```bash
grep -n '"--' plot/mimic-plot/mimic-plot.py | head -20                    # CLI flags
grep -n "SNAPSHOT_PLOTS\s*=\|EVOLUTION_PLOTS\s*=" models/sage16/plots/figures/__init__.py
sed -n '305,352p' plot/mimic-plot/mimic-plot.py                            # profile stack order
sed -n '308,341p' plot/mimic-plot/output_utils.py                          # axis keys (xmin/xmax/ymin/ymax)
grep -rn "xlim:\|ylim:" models/*/plots/profiles/*.yaml simulations/*/plot_profile.yaml | head -5   # trap still present?
ls plot/mimic-plot/tests/
```

The registry export names and figure contract are engine architecture (drift only with `mimic-plot.py`); the profile-stack order and the axis-key trap should be re-checked if `plot/mimic-plot/output_utils.py` or the shipped profiles change — if the `xlim`/`ylim` grep comes back empty, the inconsistency has been fixed and section 3's trap note should be updated.
