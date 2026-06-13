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

Example debugging invocation:

```bash
./mimic --debug models/sage16/input/sage16_mini-millennium.yaml 2>&1 | tee debug.log
```

---

## How Mimic Processes a Run

Understanding the processing model helps you configure pipelines correctly and interpret what the physics modules see.

An N-body simulation's halo catalogue is organised into **merger trees**: each tree records how a z = 0 halo was assembled from smaller progenitors over cosmic time. Mimic walks these trees snapshot by snapshot. At each snapshot interval it groups galaxies into **FoF workspaces** — the current central galaxy plus any satellites in the same friends-of-friends halo system — and hands those workspaces to the physics modules. Because galaxy physics (cooling, star formation) evolves on shorter timescales than the gap between simulation snapshots, each snapshot interval is divided into substeps (`SubSteps` in the run file; the shipped configuration uses 10).

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

modules:
  pre_timestep: []
  phases: {}
  post_timestep: []
  parameters: {}
```

`model.name` and `simulation.name` are package names, not display labels. Mimic derives `models/<model.name>/model_properties.yaml`, `simulations/<simulation.name>/simulation_info.yaml`, and `simulations/<simulation.name>/halo_properties.yaml` from them. These package paths are not user-overridable because the executable is generated and compiled for exactly one model/simulation pair.

`simulation.config` is optional and defaults to `simulations/<simulation.name>/simulation_info.yaml`. Add it only when a run should use an alternate simulation metadata file, such as a small test fixture with the same compiled simulation package:

```yaml
simulation:
  name: mini-millennium
  config: tests/data/test_simulation.yaml
```

The `plotting` section is optional. `mimic-plot.py` automatically layers the global plotting defaults, `models/<model.name>/plots/profiles/default.yaml`, `simulations/<simulation.name>/plot_profile.yaml`, and `models/<model.name>/plots/profiles/<simulation.name>_plot_profile.yaml` when those files exist. Use `plotting.profile` only for an additional run-specific override; the path must be repo-relative, not absolute. Inside a plot profile, `inherits` entries are resolved from the directory containing that profile, so model-local profiles should inherit neighbouring defaults by local filename, for example `inherits: [default.yaml]`.

The referenced simulation config, `simulations/mini-millennium/simulation_info.yaml`, owns tree input paths, cosmology, box size, particle mass, and units. `simulation.units` in that file defines the base code units used to derive time, density, pressure, energy, `G`, and related runtime quantities. The shipped mini-Millennium/SAGE example uses `Mpc/h`, `1e10 Msun/h`, and `km/s` conventions.

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

Both halves of a Mimic run are interchangeable packages. **Model packages** live under `models/`, and each one is self-documenting: its README describes the scientific scope, module pipeline, parameters, and references, and its `input/` directory holds ready-to-run configurations. **Simulation packages** live under `simulations/` and wrap a merger-tree catalogue with its cosmology, units, and snapshot list. The workflow in this guide applies to every combination equally — including packages you build yourself.

To run any pairing, build for it and use the matching run file:

```bash
make MODEL=<model> SIMULATION=<simulation>
./mimic models/<model>/input/<run_file>.yaml
```

For example, [sage16](../models/sage16/README.md) is the default complete galaxy-formation model, [sham](../models/sham/README.md) is a compact one-module example, and [halos-only](../models/halos-only/README.md) is the no-galaxy-physics package for exploring the dark-matter halo catalogue. Check `models/` for the current list, and each package's README before drawing scientific conclusions from it.

Swapping the simulation under a fixed model is a workflow in its own right, not just a configuration detail: develop and calibrate on a small box, then rerun the identical physics on a larger volume for production statistics, or across catalogues with different resolutions or cosmologies to test how robust your conclusions are to the input simulation. The shipped [mini-Millennium package](../simulations/mini-millennium/README.md) is the small working example, and a [full Millennium package](../simulations/millennium/README.md) is provided for users with access to the complete tree data — check `simulations/` for the current list. Adding your own simulation is a developer task — see [Adding a New Simulation](DEVELOPER-GUIDE.md#adding-a-new-simulation).

---

## Working With Your Catalogue

### Formats

Select the output format in the run file:

```yaml
output:
  output_format: hdf5   # or binary
```

HDF5 is self-documenting and portable — field names, units, and run provenance travel inside the file. Binary is compact and fast; Mimic writes `metadata/output_schema.json` beside every run so binary readers can reconstruct the exact record layout used by that executable. The bundled `example_Mvir_Len_plot.py` in each output directory is a ready-to-run Python example configured for the exact output that was just written. If you move or sync binary outputs elsewhere, keep the `metadata/` directory with them.

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
- [plot/mimic-plot/README.md](../plot/mimic-plot/README.md): detailed plotting manual
- [tests/README.md](../tests/README.md): test-suite quick reference
- `models/<model>/README.md`: model-package science scope, module pipeline, parameters, plots, and references
- `simulations/<simulation>/README.md`: simulation-package data, units, snapshot lists, and maintenance notes

## Citations

Cite the references for the model package you used in your research — each package's README lists them. For the default `sage16` package, those are the SAGE papers:

- Croton et al. 2016, ApJS, 222, 22
- Croton et al. 2006, MNRAS, 365, 11
