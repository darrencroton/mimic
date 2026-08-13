# Mimic User Guide

**How to install Mimic, generate galaxy catalogues, configure the physics, and analyse the results.**

Mimic turns dark-matter halo merger trees into mock galaxy catalogues. You give it three things: a **simulation package** (the merger trees plus their cosmology and units), a **model package** (the galaxy physics, as runtime-configurable modules), and a **run file** (a YAML file pairing the two and setting parameters). It gives you back a catalogue of galaxies — masses, gas reservoirs, star formation rates, positions — at the redshifts you ask for, plus enough metadata to reproduce and interpret the run later.

This guide works through that journey using the current default configuration — the `sage16` model package run on the mini-Millennium simulation — purely as a worked example. Everything generalises: models and simulations are both interchangeable packages, and every pairing follows the same workflow, run-file structure, and output conventions. Wherever you see `sage16` or `mini-millennium` in a path below, substitute the packages you are actually using.

If you're still deciding whether Mimic fits your science, start with the [README](../README.md). If you want to modify or extend the code, see the [Developer Guide](DEVELOPER-GUIDE.md).

---

## Table of Contents

1. [Getting Set Up](#getting-set-up)
2. [Your First Galaxy Catalogue](#your-first-galaxy-catalogue)
3. [How Mimic Processes a Run](#how-mimic-processes-a-run)
4. [Configuring Your Runs](#configuring-your-runs)
5. [Choosing Model and Simulation Packages](#choosing-model-and-simulation-packages)
6. [Working With Your Catalogue](#working-with-your-catalogue)
7. [Plotting](#plotting)
8. [Troubleshooting](#troubleshooting)
9. [Documentation Directory](#documentation-directory)
10. [Citations](#citations)

---

## Getting Set Up

### Prerequisites

Required:

- C compiler (`gcc` or `clang`)
- GNU Make
- Python 3.9+

Optional:

- HDF5 development libraries for HDF5 input/output (recommended; on macOS, `brew install hdf5`)
- MPI libraries for parallel processing across tree files
- `clang-format`, `black`, and `isort` if you plan to contribute code

### Quick Setup

```bash
git clone https://github.com/darrencroton/mimic.git
cd mimic
./scripts/first_run.sh
make
```

`first_run.sh` prepares the standard local environment: it creates required directories, downloads the mini-Millennium test data (~270 MB of merger trees), and creates the `mimic_venv/` Python virtual environment used by the plotting tools.

### Build Options

```bash
make                                          # default: sage16 model + mini-Millennium simulation
make MODEL=sham SIMULATION=mini-millennium    # build the SHAM example model instead
make MODEL=halos-only SIMULATION=mini-millennium  # halo catalogue only, no galaxy physics
make -j$(nproc)                               # parallel build (on macOS use e.g. make -j8)
make USE-HDF5=no                              # build without HDF5 (binary output only)
make USE-MPI=yes                              # build with MPI support
make info                                     # show detected compiler, libraries, and selected packages
```

A Mimic executable is compiled against **one model package and one simulation package at a time**. `MODEL` selects which `models/<model>/` package contributes galaxy properties, physics modules, and plots; `SIMULATION` selects which `simulations/<simulation>/` package contributes catalogue halo properties. A model package can intentionally provide no galaxy physics: `halos-only` is the shipped package for dark-matter halo catalogue exploration. The defaults are `DEFAULT_MODEL` (`sage16`) and `DEFAULT_SIMULATION` (`mini-millennium`) near the top of the `Makefile`, so plain `make` builds the shipped full-physics example. Override them per invocation, or edit the defaults if you mainly work with a different pair. If a selected package does not exist, the build stops with an `Unknown MODEL` or `Unknown SIMULATION` error rather than silently mis-building.

This pairing is enforced at runtime too: a run file whose `model.name` or `simulation.name` does not match the executable fails at startup, so you can't accidentally analyse output produced by the wrong physics. Mimic derives package paths from those names: `models/<model>` and `simulations/<simulation>`. If you want to mix modules from different model families, create a new `models/<model>/` package and reconcile property names, parameters, units, dependencies, tests, and plots there — see the [Developer Guide](DEVELOPER-GUIDE.md#module-communication).

### Manual Setup

Use this only if `first_run.sh` fails or you are intentionally setting up pieces by hand:

```bash
mkdir -p simulations/mini-millennium/snapshots

cd simulations/mini-millennium/snapshots
wget "https://www.dropbox.com/s/l5ukpo7ar3rgxo4/mini-millennium-treefiles.tar?dl=0" \
     -O mini-millennium-treefiles.tar
tar -xf mini-millennium-treefiles.tar
rm mini-millennium-treefiles.tar
cd ../../..

python3 -m venv mimic_venv
source mimic_venv/bin/activate
pip install -r requirements.txt
deactivate

make
```

Relative paths in the run file are resolved from the directory where you run `./mimic`. If path confusion occurs, use absolute paths for `output.output_directory`, `input.simulation_dir`, and `input.snapshot_list_file`. Mimic creates the output directory automatically, but input data paths must already exist.

---

## Your First Galaxy Catalogue

With the build done, generate a catalogue:

```bash
./mimic models/sage16/input/sage16_mini-millennium.yaml
```

This reads the mini-Millennium merger trees, evolves galaxies through the default model's full physics pipeline — for `sage16`, that's reionization, gas infall, cooling, star formation, supernova and AGN feedback, and mergers — and writes a catalogue for eight snapshots between z ≈ 8 and z = 0. On a laptop it takes well under a minute. Mimic returns exit code 0 on success; treat any non-zero exit code as a failed run.

You now have, under `output/sage16-mini-millennium/`:

- `model_000.hdf5` … — per-tree-file galaxy catalogues
- `model.hdf5` — a master file linking them together, so you can analyse the run as one dataset
- `metadata/` — the run's output schema and provenance
- `example_Mvir_Len_plot.py` — a ready-to-run Python script, pre-configured for this run's format, filename, snapshots, and cosmology

The example script is the fastest way to take a first look:

```bash
source mimic_venv/bin/activate
python output/sage16-mini-millennium/example_Mvir_Len_plot.py
```

It prints the full list of output fields and produces a halo mass scatter plot — confirmation that the catalogue is real and readable. Then generate the standard diagnostic figures:

```bash
python plot/mimic-plot/mimic-plot.py --param-file=models/sage16/input/sage16_mini-millennium.yaml
deactivate
```

Look in `output/sage16-mini-millennium/plots/` for the stellar mass function, baryonic Tully-Fisher relation, gas fractions, star formation history, and more.

### Command-Line Options

```bash
./mimic <run_file.yaml>
```

| Flag | Output | Use case |
| --- | --- | --- |
| default | INFO, WARNING, ERROR | Normal interactive runs |
| `--verbose`, `-v` | Adds timestamp and file:line context | Detailed run logs |
| `--debug`, `-d` | Adds DEBUG messages | Troubleshooting module/configuration issues |
| `--quiet`, `-q` | WARNING and ERROR only | Batch or production runs |
| `--skip` | Skips existing output files | Resume interrupted runs |
| `--compress` | gzip-compresses HDF5 galaxy output (off by default) | Disk-constrained or archival runs |

`--compress` trades a little CPU for roughly half the HDF5 file size and changes only the on-disk byte layout, not the stored values. It has no effect on binary output. Leave it off unless disk space is a constraint.

During tree processing Mimic prints a banner line summarising the configured input file range, then shows a single unified live progress bar (percentage, elapsed time, and ETA) spanning all input files when standard output is an interactive terminal, marking completion with "- COMPLETED" in place. When output is redirected to a file or pipe, or when running under MPI with more than one rank, it falls back to bar-format log lines at every 5% boundary plus a "Completed input file" checkpoint per file instead of an in-place bar, keeping captured logs and multi-rank output clean. `--quiet` suppresses the progress display entirely.

Example debugging invocation:

```bash
./mimic --debug models/sage16/input/sage16_mini-millennium.yaml 2>&1 | tee debug.log
```

---

## How Mimic Processes a Run

Understanding the processing model helps you configure pipelines correctly and interpret what the physics modules see.

An N-body simulation's halo catalogue is organised into **merger trees**: each tree records how a z = 0 halo was assembled from smaller progenitors over cosmic time. Mimic walks these trees snapshot by snapshot. At each snapshot interval it groups galaxies into **FoF workspaces** — the current central galaxy plus any satellites in the same friends-of-friends halo system — and hands those workspaces to the physics modules. Because galaxy physics (cooling, star formation) evolves on shorter timescales than the gap between simulation snapshots, each snapshot interval can be divided into substeps (`SubSteps` in the run file; the shipped fixed-timestep SAGE configuration uses 10).

For each snapshot interval:

```text
pre_timestep runs once

for each substep:
  each modules.phases entry runs in declared order

post_timestep runs once
```

Inside each phase, Mimic groups modules by **processing mode** — the contract describing what slice of the workspace a module receives:

- `process_full_halo`: the module receives the whole FoF workspace at once. Use this for calculations that need the central and satellites together, such as infall budgets, merger clocks, and event producers.
- `process_per_event`: the module runs only when a subscribed full-halo producer emits an event (a merger, say). The module receives the event target galaxy with `ctx->active_event` set.
- `process_by_galaxy`: Mimic loops through the FoF workspace and calls the module once per galaxy. Use this for local galaxy physics such as cooling, star formation, and feedback.

Full-halo modules always run before by-galaxy modules within a phase. Events emitted by full-halo producers are dispatched immediately to subscribed per-event consumers, preserving producer-side event ordering. YAML order is preserved within the same processing mode; it does not make a by-galaxy module run before a full-halo module in the same phase.

(One shipped exception worth knowing about: `sage_satellite_stripping` runs as `process_by_galaxy` even though it mutates the FoF central through `ctx->central_galaxy`, because the by-galaxy placement is required to match SAGE's strip-then-cool timing for each satellite.)

---

## Configuring Your Runs

### Run File Structure

The shipped configuration is `models/sage16/input/sage16_mini-millennium.yaml`. Its top-level sections are:

```yaml
model:
  name: sage16

simulation:
  name: mini-millennium

output:
  output_filename: model
  output_directory: output/sage16-mini-millennium
  output_format: hdf5                 # binary or hdf5
  snapshot_list: [63, 37, 32, 27, 23, 20, 18, 16]

SubSteps: 10
TimestepScheme: fixed

modules:
  pre_timestep: []
  phases: {}
  post_timestep: []
  parameters: {}
```

`TimestepScheme` controls how `SubSteps` is interpreted. The default `fixed` scheme runs exactly `SubSteps` substeps per snapshot interval (`SubSteps: 0` is treated as one step). The opt-in `dynamic` scheme treats `SubSteps` as the requested resolution per halo dynamical time, computes the central halo's `t_dyn = Rvir / Vvir`, and runs `ceil(deltaT * SubSteps / t_dyn)` substeps clamped to at least 1 and at most `MaxDynamicSubsteps` (optional top-level key, default 200) — a safety ceiling against pathological snapshot spacing, not a resolution target, so raise it only if it is clipping the redshift range you care about. Dynamic mode therefore uses fewer substeps when a snapshot interval is shorter than the dynamical time and more when it spans many dynamical times. HDF5 master outputs record both `SubSteps` and `TimestepScheme` under `RunProperties`.

`model.name` and `simulation.name` are package names, not display labels. Mimic derives `models/<model.name>/model_properties.yaml`, `simulations/<simulation.name>/simulation_info.yaml`, and `simulations/<simulation.name>/halo_properties.yaml` from them. These package paths are not user-overridable because the executable is generated and compiled for exactly one model/simulation pair.

`simulation.config` is optional and defaults to `simulations/<simulation.name>/simulation_info.yaml`. Add it only when a run should use an alternate simulation metadata file, such as a small test fixture with the same compiled simulation package:

```yaml
simulation:
  name: mini-millennium
  config: tests/data/test_simulation.yaml
```

The `plotting` section is optional. `mimic-plot.py` automatically layers the global plotting defaults, `models/<model.name>/plots/profiles/default.yaml`, `simulations/<simulation.name>/plot_profile.yaml`, and `models/<model.name>/plots/profiles/<simulation.name>_plot_profile.yaml` when those files exist. Use `plotting.profile` only for an additional run-specific override; the path must be repo-relative, not absolute. Inside a plot profile, `inherits` entries are resolved from the directory containing that profile, so model-local profiles should inherit neighbouring defaults by local filename, for example `inherits: [default.yaml]`.

The referenced simulation config, `simulations/mini-millennium/simulation_info.yaml`, owns tree input paths, cosmology, box size, and particle mass. Mimic's internal reference units are fixed by core metadata (`Mpc/h`, `1e10 Msun/h`, `km/s`); simulation packages declare catalog field units in `halo_properties.yaml`, and the generated reader boundary converts catalog values into that reference basis.

### The Physics Pipeline

The module pipeline is configured under `modules`. The following abbreviated example shows the phase structure only — the full set of SAGE modules and their parameter values live in `models/sage16/input/sage16_mini-millennium.yaml`, which is the authoritative shipped configuration.

```yaml
SubSteps: 10

modules:
  pre_timestep:
    - sage_reionization:              process_full_halo
    - sage_prepare_infall_budget:     process_full_halo
    # ... see models/sage16/input/sage16_mini-millennium.yaml for the full pre_timestep block

  phases:
    galaxy_physics:
      - sage_apply_infall:              process_full_halo
      - sage_calculate_cooling_budget:  process_by_galaxy
      - sage_radio_mode_heating:        process_by_galaxy
      - sage_apply_cooling:             process_by_galaxy
      # ... star formation, supernova, disk instability, quasar, starburst ...

    satellite_mergers:
      - sage_resolve_mergers_and_disruption: process_full_halo
      - sage_quasar_mode:               process_per_event
      - sage_starburst_feedback:        process_per_event

  post_timestep: []

  parameters:
    # Illustrative values only. See models/sage16/input/sage16_mini-millennium.yaml for the calibrated set.
    GlobalBaryonFraction: 0.17
    SfrEfficiency: 0.05
    # ... cooling, AGN, BH, metals, mergers ...
```

Module parameters have no global defaults in the core. A module loads and validates the parameters it needs during its `init()` function. If a required parameter is missing, startup fails before trees are processed — a few seconds, not after a long run.

### Configuration Recipes

**Physics-free mode** writes halo-tracking output without galaxy physics — useful for testing your input trees or for halo-only science. For normal use, prefer the shipped `halos-only` package:

```bash
make MODEL=halos-only SIMULATION=mini-millennium
./mimic models/halos-only/input/halos-only_mini-millennium.yaml
```

Inside any model package, the same runtime behaviour is an empty module pipeline:

```yaml
modules:
  phases: {}
  parameters: {}
```

**Disable a module** by removing or commenting its line. Check the surrounding modules before doing this: many SAGE modules pass transport properties to later modules in the same phase (e.g. `sage_calculate_supernova_feedback` computes masses that `sage_apply_star_formation_supernova` commits).

```yaml
modules:
  phases:
    galaxy_physics:
      - sage_calculate_star_formation: process_by_galaxy
      # - sage_calculate_supernova_feedback: process_by_galaxy
      - sage_apply_star_formation_supernova: process_by_galaxy
```

**Write every snapshot** by omitting `snapshot_list` or leaving it empty:

```yaml
output:
  snapshot_list: []
```

**Add a snapshot** by adding the snapshot number to `snapshot_list`:

```yaml
output:
  snapshot_list: [63, 37, 32, 27, 23, 20, 18, 16, 12]
```

**Override simulation input defaults** by adding an `input` section to your run file. `simulation_info.yaml` defines the defaults for a simulation (e.g. which tree files to process); any `input` keys present in the run file take precedence. This lets you control a run entirely from one file without touching the shared simulation config:

```yaml
# In your model-local run file (e.g. models/sage16/input/sage16_mini-millennium.yaml)
# Processes only file 0 regardless of what simulation_info.yaml sets for last_file
input:
  first_file: 0
  last_file: 0
```

`simulation_info.yaml` may also define catalogue-scale output chunking defaults with `output.target_file_size_mb` and `output.forests_per_file`. A run file can override those two keys in its own `output:` section. Output destination, format, and snapshot list remain run-file settings.

**Run with MPI** after building with MPI support — Mimic parallelizes over tree files:

```bash
make USE-MPI=yes
mpirun -np 4 ./mimic models/sage16/input/sage16_mini-millennium.yaml
```

For balanced work, choose a rank count that divides `last_file - first_file + 1`.

**Resume an interrupted run** with `--skip`, which leaves existing output files in place:

```bash
./mimic --skip models/sage16/input/sage16_mini-millennium.yaml
```

---

## Choosing Model and Simulation Packages

Both halves of a Mimic run are interchangeable packages. **Model packages** live under `models/`, and each one is self-documenting: its README describes the scientific scope, module pipeline, parameters, and references, and its `input/` directory holds ready-to-run configurations. **Simulation packages** live under `simulations/` and wrap a merger-tree catalogue with its cosmology, units, and snapshot list. The workflow in this guide applies to every *runnable* combination equally — including packages you build yourself.

`micro-uchuu-snapshot` is the shipped snapshot-ordered package — the same micro-Uchuu catalogue as `micro-uchuu-ascii`, converted to snapshot-ordered HDF5 and read by the snapshot driver. It is runnable like any other package, with its own shipped run files (`models/halos-only/input/halos-only_micro-uchuu-snapshot.yaml`, `models/sage16/input/sage16_micro-uchuu-snapshot.yaml`); see [Running Snapshot-Ordered Input](#running-snapshot-ordered-input) for what is different about it. A package is runnable when a run file pairs it with a model under `models/<model>/input/`.

To run any runnable pairing, build for it and use the matching run file:

```bash
make MODEL=<model> SIMULATION=<simulation>
./mimic models/<model>/input/<run_file>.yaml
```

For example, [sage16](../models/sage16/README.md) is the default complete galaxy-formation model, [sham](../models/sham/README.md) is a compact one-module example, and [halos-only](../models/halos-only/README.md) is the no-galaxy-physics package for exploring the dark-matter halo catalogue. Check `models/` for the current list, and each package's README before drawing scientific conclusions from it.

Swapping the simulation under a fixed model is a workflow in its own right, not just a configuration detail: develop and calibrate on a small box, then rerun the identical physics on a larger volume for production statistics, or across catalogues with different resolutions or cosmologies to test how robust your conclusions are to the input simulation. The shipped [mini-Millennium package](../simulations/mini-millennium/README.md) is the small working example, and a [full Millennium package](../simulations/millennium/README.md) is provided for users with access to the complete tree data — check `simulations/` for the current list. Adding your own simulation is a developer task — see [Adding a New Simulation](DEVELOPER-GUIDE.md#adding-a-new-simulation).

---

## Working With Your Catalogue

### Input Tree Formats

Mimic separates the on-disk reader format from the processing driver. The input merger-tree format is set by `input.tree_type`. It normally lives in the simulation package's `simulation_info.yaml` (it is a property of the catalogue), but like any `input` key it can be overridden per run. The value names a format, not a simulation — the same reader serves any catalogue written in that format:

| `tree_type` | Format | Build |
| --- | --- | --- |
| `lhalo_binary` | LHaloTree binary (Springel et al.) | any |
| `lhalo_hdf5` | LHaloTree HDF5 (per-tree `tree_NNN/<field>` groups) | HDF5 build |
| `consistent_trees_ascii` | Consistent-Trees / Rockstar ASCII (`forests.list` + `locations.dat` + `tree_i_j_k.dat`) | any |
| `consistent_trees_hdf5` | Consistent-Trees forests-HDF5 (uchuutools) | HDF5 build |
| `snapshot_hdf5` | Snapshot-ordered HDF5, one `snapshot_NNN.h5` file per snapshot | HDF5 build |

The HDF5-based readers are only available when Mimic is built with HDF5 (the default; see [Build Options](#build-options)). Selecting one in a `USE-HDF5=no` build stops with a clear configuration error.

The first four formats are forest-ordered and feed the tree-ordered driver. `snapshot_hdf5` is the one snapshot-ordered format, read by a separate reader family; its on-disk contract is `docs/dev/SNAPSHOT-HDF5-FORMAT.md`, and the `micro-uchuu-snapshot` package is the shipped example. Its driver opens and validates the whole dataset before processing any halo data (see `input.processing_order` below and [Running Snapshot-Ordered Input](#running-snapshot-ordered-input)).

`input.tree_name` is reader-specific — each reader decides what the value means, so it is not a general filename pattern. `lhalo_binary` is the prefix before the numbered file suffix (`tree_name.<file_number>`). `consistent_trees_ascii` and `consistent_trees_hdf5` are literal filenames under `input.simulation_dir`, including any extension. `lhalo_hdf5` uses explicit HDF5 filenames: for one file, set `tree_name` to that filename; for multiple files, include a `%d` file-number placeholder, for example `trees_063.%d.hdf5`. `snapshot_hdf5` fixes its filename convention in the format itself and therefore accepts exactly the literal `snapshot_%03d.h5` — any other value, including `snapshot_%d.h5`, is rejected at startup with a message naming the accepted literal.

The `consistent_trees_hdf5` reader keeps memory bounded while reducing HDF5 call overhead: it caches chunk-range `ForestInfo`, keeps per-file field handles open for the partition lifetime, and reads normal forests through a fixed 128 MiB per-rank slab window. This is an internal reader detail, not a run-YAML option.

The processing driver is selected separately with `input.processing_order`. It defaults to `tree_ordered`, so existing run files and simulation packages do not need to set it. The other accepted value is `snapshot_ordered`. Startup validation checks the two keys against each other: every reader declares the one driver it feeds, and a mismatched pair — say `snapshot_hdf5` with `tree_ordered`, or `consistent_trees_ascii` with `snapshot_ordered` — is a configuration error naming both. A correctly paired `snapshot_ordered` run gets through configuration — reader resolution, the reader/order check, the `tree_name` literal, the identity-multiplier rules, and the three snapshot-specific rejections below — and then the snapshot driver opens and fully validates its dataset before processing anything, exactly as the tree driver opens and reads its tree files.

```yaml
input:
  tree_type: lhalo_binary          # reader format
  processing_order: tree_ordered   # processing driver; optional default
```

### Running Snapshot-Ordered Input

A `snapshot_ordered` run works like any other run — build for the package, point `./mimic` at its run file — with three deliberate restrictions enforced at startup, before any snapshot file is opened:

- **HDF5-only output.** `output.output_format: binary` is rejected with a message stating snapshot-ordered runs are HDF5-only; the binary writer's per-tree header has no meaning for snapshot-major output.
- **No `--skip`.** Resume is not supported for snapshot-ordered runs in this phase; `--skip` is rejected at configuration rather than silently ignored.
- **Serial only.** A snapshot-ordered configuration requires `NTask == 1` and is rejected with a message naming `docs/dev/MIMIC-DISTRIBUTED-SNAPSHOT-PLAN.md`. Multi-rank snapshot execution belongs to that plan, not to the current driver.

```bash
make MODEL=halos-only SIMULATION=micro-uchuu-snapshot
./mimic models/halos-only/input/halos-only_micro-uchuu-snapshot.yaml
```

**Reading snapshot-run output.** A snapshot-ordered run's HDF5 output differs from a tree-ordered run's in a few specific, deliberate ways:

- Each `Snap%03d/Galaxies` group carries no `Ntrees` attribute and no `TreeHalosPerSnap` dataset — they are omitted entirely (absent, not zero or empty), because a snapshot run has no per-tree structure to report.
- `TotHalosPerSnap` is still the per-snapshot galaxy-count attribute, but stored as `int64` rather than `int` — the same widened type a tree-ordered run's output now carries too.
- A snapshot-ordered run writes one HDF5 partition file per requested output snapshot, named by that snapshot's number (`model_<snapnum>.hdf5` — filenames stay self-describing even for an unsorted `output.snapshot_list`), plus the master `<basename>.hdf5`. Each partition file holds exactly one `Snap%03d` group: its own. The master creates one `Snap%03d` group per requested output snapshot, each holding a single `File%03d` subgroup whose external link resolves into that snapshot's own partition file, and no per-tree table.
- `RunProperties/Version/hdf5_format_version` reads `1.2`.

Everything else in [Reading HDF5 Output](#reading-hdf5-output) below applies unchanged.

**`simulation.unique_galaxy_id_multiplier`** sets the forest multiplier in the galaxy identity encoding `UniqueGalaxyID = halonr + multiplier × (forestnr_global + 1)`. It is optional, must be positive, and defaults to the compile-time `TREE_MUL_FAC` (10⁹, in `src/include/constants.h`). It belongs with the catalogue in `simulations/<name>/simulation_info.yaml` and may be overridden in a run file; a value declared in the package survives a run file that omits the key. Raising it is how a catalogue whose largest forest holds ≥10⁹ halos stays encodable. **Both processing orders honour the configured value.** Every identity helper takes the multiplier explicitly, and the Consistent-Trees readers guard their forest sizes against the same value, so raising it really does raise the forest size a run accepts. Every HDF5 output file records the value it used as the `RunProperties/UniqueGalaxyIDMultiplier` attribute — in per-file outputs and in the master file — so ids can always be decomposed back into `(halonr, forestnr_global)`. For snapshot-ordered configurations the reader also bounds-checks the value against the dataset's own recorded forest count and maximum halo rank, as part of `open_run`'s dataset validation on every snapshot-ordered run. What a run enforces at startup for the value itself, on either processing order, is only that it is positive.

```yaml
simulation:
  name: micro-uchuu-snapshot
  unique_galaxy_id_multiplier: 1000000000   # optional; default TREE_MUL_FAC (10^9)
```

The two Consistent-Trees readers enumerate output chunks from forest ranges, then assign those chunks across MPI tasks. Output file ids are chunk ids, independent of `NTask`, so serial and MPI runs produce the same chunk layout. Two optional `input` keys tune HDF5 chunk-cost estimation; both are ignored by the L-Halo readers, and `forest_distribution_scheme` is honoured only by `consistent_trees_hdf5` (the ASCII reader cannot know per-forest halo counts before loading, so its chunk costs are uniform):

```yaml
input:
  forest_distribution_scheme: uniform   # uniform | linear | quadratic | exponent | generic_power
  exponent_forest_dist_scheme: 0.7      # exponent for the exponent/generic_power schemes
```

`uniform` (the default) gives every task an equal number of forests. The other schemes weight by per-forest halo count to balance work — `linear` by `nhalos`, `quadratic` by `nhalos²`, `exponent` by `nhalos` raised to the integer part of `exponent_forest_dist_scheme` (repeated multiplication, so a fractional value is truncated), and `generic_power` by `pow(nhalos, exponent_forest_dist_scheme)` (fractional exponents allowed) — so tasks receive a comparable total halo load rather than a comparable forest count.

### Output Formats

Select the output format in the run file:

```yaml
output:
  output_format: hdf5   # or binary
```

HDF5 is self-documenting and portable — field names, units, and run provenance travel inside the file. Binary is compact and fast; Mimic writes `metadata/output_schema.json` beside every run so binary readers can reconstruct the exact record layout used by that executable. The bundled `example_Mvir_Len_plot.py` in each output directory is a ready-to-run Python example configured for the exact output that was just written. If you move or sync binary outputs elsewhere, keep the `metadata/` directory with them.

Consistent-Trees chunked output is configured with two optional output keys. `target_file_size_mb` is a soft size target in MiB (1024² bytes) for HDF5 chunk planning and defaults to 4096 (4 GiB). The planner estimates chunk size from input halo counts, so processed output can be larger when model physics creates orphans or when HDF5 compression and metadata change on-disk size. `forests_per_file` defaults to 0, which means derive chunks from `target_file_size_mb`; when set above 0, it gives an exact deterministic forest count per output chunk. These keys may live in `simulation_info.yaml` when the catalogue size requires a shared default across models, and the run file can override them. ASCII Consistent-Trees catalogues cannot estimate chunk sizes from `target_file_size_mb`, so `consistent_trees_ascii` requires `forests_per_file > 0` before processing starts. Existing chunks are resumable with `--skip`; a partial chunk is rejected so reruns do not mix complete and incomplete partition files.

```yaml
output:
  target_file_size_mb: 4096  # soft target in MiB (4 GiB)
  forests_per_file: 0        # 0 = derive from target_file_size_mb
```

### Units and Schema

The output schema is generated at build time from three property metadata files:

- `src/core/core_properties.yaml` for halo-tracking properties
- `simulations/<simulation>/halo_properties.yaml` for catalogue halo properties
- `models/<model>/model_properties.yaml` for galaxy/model properties

The generated executable, output record layout, HDF5 field metadata, and validation ranges are therefore model-and-simulation specific. Each property declares its output unit label, initialization behaviour, output conversion, and whether it is written at all. HDF5 output includes a `FieldMetadata` table (under `RunProperties`, once per file) so analysis code can inspect field names, units, and descriptions directly from the file.

For the shipped mini-Millennium/sage16 configuration, common output conventions are:

| Quantity | Typical unit label | Examples |
| --- | --- | --- |
| Mass | `1e10 Msun/h` | `Mvir`, `StellarMass`, `ColdGas` |
| Length | `Mpc/h` | `Rvir`, `Pos`, `DiskScaleRadius` |
| Velocity | `km/s` | `Vvir`, `Vmax`, `Vel` |
| Rates | `Msun/yr` or `log10(erg/s)` | `StarFormationRate`, `Cooling`, `Heating` |
| Time | `Myr/h` or `Gyr/h` | `dT`, `TimeOfLastMajorMerger` |

Do not assume these lists are universal for every model. Treat the property metadata and HDF5 `FieldMetadata` as the source of truth.

### Reading HDF5 Output

```python
import h5py

with h5py.File("output/sage16-mini-millennium/model_000.hdf5", "r") as f:
    galaxies = f["Snap063/Galaxies"][:]
    metadata = f["RunProperties/FieldMetadata"][:]

    units = {
        row["field_name"].decode(): row["units"].decode()
        for row in metadata
    }

    mvir = galaxies["Mvir"]
    stellar_mass = galaxies["StellarMass"]

    print(f"Mvir unit: {units['Mvir']}")
    print(f"Loaded {len(galaxies)} objects")
```

Per-file HDF5 output contains:

```text
/RunProperties/
  Version/
  EnabledModules
  EventContracts          # present only when event contracts exist
  Parameters
  Redshifts
  FieldMetadata           # field names, units, descriptions (once per file)
/Snap063/
  Galaxies
    @Ntrees
    @TotHalosPerSnap
  TreeHalosPerSnap
```

The master HDF5 file, `model.hdf5`, contains run metadata plus external links to the per-file outputs:

```text
/RunProperties/
  FieldMetadata
/Snap063/
  File000/Galaxies -> model_000.hdf5:/Snap063/Galaxies
  File000/TreeHalosPerSnap -> model_000.hdf5:/Snap063/TreeHalosPerSnap
```

A snapshot-ordered run's master links one partition file per requested output snapshot instead — each named for the snapshot it holds, not a chunk number — and no `TreeHalosPerSnap` link exists because snapshot-run output never carries per-tree structure. This is real output from an unsorted `output.snapshot_list: [5, 1, 0]`; every link still resolves to the partition file named for its own snapshot number:

```text
/RunProperties/
  FieldMetadata
/Snap000/
  File000/Galaxies -> model_000.hdf5:/Snap000/Galaxies
/Snap001/
  File001/Galaxies -> model_001.hdf5:/Snap001/Galaxies
/Snap005/
  File005/Galaxies -> model_005.hdf5:/Snap005/Galaxies
```

`RunProperties/EnabledModules`, `RunProperties/Parameters`, and `RunProperties/EventContracts` are the main reproducibility datasets: months later, you can recover exactly which physics pipeline and parameter values produced a file without finding the original run YAML.

### Reading Binary Output

Binary output has a small integer header followed by fixed-layout galaxy records. Use the run-local schema in `metadata/output_schema.json` — not the current checkout's model metadata, which may have changed since the run — to construct the dtype. The `output_schema` helper module ships with the plotting tool:

```python
from pathlib import Path
import sys

import json
import numpy as np

repo = Path("/path/to/mimic")
sys.path.insert(0, str(repo / "plot" / "mimic-plot"))

from output_schema import descriptions_from_schema, dtype_from_schema, units_from_schema

path = repo / "output" / "sage16-mini-millennium" / "model_z0.000_0"
schema = json.loads((path.parent / "metadata" / "output_schema.json").read_text())
dtype = dtype_from_schema(schema, binary=True)
units = units_from_schema(schema)
descriptions = descriptions_from_schema(schema)  # {field: human-readable description}

with path.open("rb") as f:
    ntrees = np.fromfile(f, dtype=np.int32, count=1)[0]
    ngalaxies = np.fromfile(f, dtype=np.int32, count=1)[0]
    halos_per_tree = np.fromfile(f, dtype=np.int32, count=ntrees)
    galaxies = np.fromfile(f, dtype=dtype, count=ngalaxies)

print(f"Read {len(galaxies)} objects")
print(f"Mvir unit: {units['Mvir']}")
```

---

## Plotting

The plotting tool generates the standard diagnostic figures for a run directly from its run file. Activate the virtual environment first:

```bash
source mimic_venv/bin/activate

# All plots for the run
python plot/mimic-plot/mimic-plot.py --param-file=models/sage16/input/sage16_mini-millennium.yaml

# Specific plots
python plot/mimic-plot/mimic-plot.py --param-file=models/sage16/input/sage16_mini-millennium.yaml \
    --plots=halo_mass_function,stellar_mass_function

# Single-snapshot plots only, or evolution-across-redshift plots only
python plot/mimic-plot/mimic-plot.py --param-file=models/sage16/input/sage16_mini-millennium.yaml \
    --snapshot-plots
python plot/mimic-plot/mimic-plot.py --param-file=models/sage16/input/sage16_mini-millennium.yaml \
    --evolution-plots

deactivate
```

Plots are written under the configured output directory, normally `output/sage16-mini-millennium/plots/` for the shipped example.

The plot registry is model-specific — it lives in `models/<model>/plots/figures/` — so build Mimic with the same `MODEL` as the run file before plotting. The `halos-only` registry intentionally advertises only halo/catalogue diagnostics. The detailed plotting manual is [plot/mimic-plot/README.md](../plot/mimic-plot/README.md): command-line options, available plot names, skipped-plot diagnostics, plotting native SAGE output for comparison, and adding new plot types.

---

## Troubleshooting

### Build Issues

**HDF5 not found**: Install HDF5 development libraries or build without HDF5:

```bash
brew install hdf5      # macOS/Homebrew
make USE-HDF5=no       # or skip HDF5 entirely (binary output only)
```

**Generated code is stale**: Regenerate after editing property YAML, module metadata, or module files:

```bash
make generate
make clean && make
```

**Unexpected build configuration**: Use `make info` to inspect the detected compiler, HDF5, MPI, model set, simulation package, and feature flags.

### Runtime Issues

**Non-zero exit code**: Treat the run as failed. Check the last error messages and rerun with debug logging:

```bash
./mimic --debug models/sage16/input/sage16_mini-millennium.yaml 2>&1 | tee debug.log
```

**Run file rejected at startup (model/simulation mismatch)**: The executable was built for a different `MODEL`/`SIMULATION` pair than the run file selects. Rebuild with matching selectors, e.g. `make MODEL=sham SIMULATION=mini-millennium` for the SHAM run file.

**Cannot open input files**: Check `input.simulation_dir`, `input.tree_name`, `input.first_file`, `input.last_file`, and `input.snapshot_list_file`. Mimic creates output directories but does not create or download missing input data during a normal run.

**Missing mini-Millennium data**:

```bash
./scripts/first_run.sh
```

**Module not registered**: Run:

```bash
make validate-modules
make generate
make clean && make
```

This usually indicates a new module or metadata change was not regenerated, or a YAML configuration references the wrong module name.

### Plotting Issues

**Python import errors**: Activate the virtual environment:

```bash
source mimic_venv/bin/activate
```

**No plots generated or many skipped plots**: Some plots require populated galaxy-physics fields. A physics-free run can still produce halo-property plots, and the `halos-only` package avoids advertising galaxy-property plots in the first place. Run with `--verbose` to see skip reasons:

```bash
python plot/mimic-plot/mimic-plot.py --param-file=models/sage16/input/sage16_mini-millennium.yaml --verbose
```

**Virtual environment missing**:

```bash
python3 -m venv mimic_venv
source mimic_venv/bin/activate
pip install -r requirements.txt
```

---

## Documentation Directory

- [README.md](../README.md): project overview and shortest path to a first result
- [VISION.md](VISION.md): architectural principles and design boundaries
- [DEVELOPER-GUIDE.md](DEVELOPER-GUIDE.md): extending models, modules, simulations, properties, tests, and generated metadata
- [STYLE-GUIDE.md](STYLE-GUIDE.md): naming, comments, documentation, metadata, tests, and review conventions
- [plot/mimic-plot/README.md](../plot/mimic-plot/README.md): detailed plotting manual
- [tests/README.md](../tests/README.md): test-suite quick reference
- `models/<model>/README.md`: model-package science scope, module pipeline, parameters, plots, and references
- `simulations/<simulation>/README.md`: simulation-package data, units, snapshot lists, and maintenance notes

## Citations

Cite the references for the model package you used in your research — each package's README lists them. For the default `sage16` package, those are the SAGE papers:

- Croton et al. 2016, ApJS, 222, 22
- Croton et al. 2006, MNRAS, 365, 11
