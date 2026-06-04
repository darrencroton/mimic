# Mimic User Guide

**Guide to installing, configuring, running, and reading Mimic simulations.**

This guide uses the shipped **mini-Millennium + SAGE** configuration as its worked example. Mimic itself is a framework: other input simulations, module pipelines, and property metadata can produce different scientific behavior and output schemas.

---

## Table of Contents

1. [Installation](#installation)
2. [Running Simulations](#running-simulations)
3. [How Mimic Processes a Run](#how-mimic-processes-a-run)
4. [Configuration](#configuration)
5. [Output](#output)
6. [Plotting](#plotting)
7. [Troubleshooting](#troubleshooting)

---

## Installation

### Quick Start

```bash
git clone [repository-url]
cd mimic
./scripts/first_run.sh
make MODEL=sage
./mimic models/sage/input/sage_millennium.yaml
```

`first_run.sh` prepares the standard local environment: it creates required directories, downloads the mini-Millennium test data, and creates the Python virtual environment used by plotting tools.

### Prerequisites

Required:

- C compiler (`gcc` or `clang`)
- GNU Make
- Python 3.6+

Optional:

- HDF5 development libraries for HDF5 input/output
- MPI libraries for parallel processing across tree files
- `clang-format`, `black`, and `isort` for formatting workflows

### Build Options

```bash
make MODEL=sage       # Standard SAGE build; HDF5 enabled by default
make MODEL=sham       # Build the SHAM model set instead
make MODEL=sage -j$(nproc)
make MODEL=sage USE-HDF5=no
make MODEL=sage USE-MPI=yes
make MODEL=sage info
```

On macOS, replace `$(nproc)` with the number of build jobs you want, for example `make MODEL=sage -j8`.

`MODEL` defaults to `DEFAULT_MODEL` (set to `sage`) near the top of the `Makefile`, so plain `make` builds the default model. Override it per-invocation with `make MODEL=<name>`, or change the `DEFAULT_MODEL` line if your primary model is not sage. If the selected package does not exist (for example after a model is renamed or removed), the build stops with an `Unknown MODEL` error rather than silently mis-building.

Mimic is compiled against one model set at a time. The selected `MODEL` controls which `models/<model>/` package contributes run files, properties, modules, model-local shared helpers, tests, and plotting figures. A run file whose `model.name` or `model.path` does not match the compiled model fails at startup. If you want to mix modules from multiple model families, create a new `models/<model>/` package and copy the run files, modules, helpers, and plots you need into it, then reconcile property names, parameter names, units, dependencies, and tests inside that package.

### Manual Setup

Use this only if `first_run.sh` fails or you are intentionally setting up pieces by hand.

```bash
mkdir -p simulations/millennium/snapshots

cd simulations/millennium/snapshots
wget "https://www.dropbox.com/s/l5ukpo7ar3rgxo4/mini-millennium-treefiles.tar?dl=0" \
     -O mini-millennium-treefiles.tar
tar -xf mini-millennium-treefiles.tar
rm mini-millennium-treefiles.tar
cd ../../..

python3 -m venv mimic_venv
source mimic_venv/bin/activate
pip install -r requirements.txt
deactivate

make MODEL=sage
./mimic models/sage/input/sage_millennium.yaml
```

Relative paths in the parameter file are resolved from the directory where you run `./mimic`. If path confusion occurs, use absolute paths for `output.output_directory`, `input.simulation_dir`, and `input.snapshot_list_file`. Mimic creates `output.output_directory` automatically, but input data paths must already exist.

---

## Running Simulations

```bash
./mimic <parameter_file.yaml>
```

Command-line options:

| Flag | Output | Use case |
| --- | --- | --- |
| default | INFO, WARNING, ERROR | Normal interactive runs |
| `--verbose`, `-v` | Adds timestamp and file:line context | Detailed run logs |
| `--debug`, `-d` | Adds DEBUG messages | Troubleshooting module/configuration issues |
| `--quiet`, `-q` | WARNING and ERROR only | Batch or production runs |
| `--skip` | Skips existing output files | Resume interrupted runs |

Example:

```bash
./mimic --debug models/sage/input/sage_millennium.yaml 2>&1 | tee debug.log
```

Mimic returns exit code 0 on success. Any non-zero exit code should be treated as a failed run.

---

## How Mimic Processes a Run

Mimic reads merger trees and processes each requested snapshot interval through FoF workspaces. A FoF workspace is the current central galaxy plus any satellites in the same FoF system. Physics modules act on that workspace according to their configured phase and processing mode.

For each snapshot interval:

```text
pre_timestep runs once

for each substep:
  phase_1 runs
  phase_2 runs

post_timestep runs once
```

Inside each phase, Mimic groups modules by processing mode:

- `process_full_halo`: the module receives the whole FoF workspace at once. Use this for calculations that need the central and satellites together, such as infall budgets, merger clocks, and event producers.
- `process_per_event`: the module runs only when a subscribed full-halo producer emits an event. The module receives the event target galaxy with `ctx->active_event` set.
- `process_by_galaxy`: Mimic loops through the FoF workspace and calls the module once per galaxy. Use this for local galaxy physics such as cooling, star formation, and feedback.

`sage_satellite_stripping` also runs as `process_by_galaxy`: it mutates the FOF central through `ctx->central_galaxy`, but the by-galaxy placement is required to match SAGE's strip-then-cool timing for each satellite.

Full-halo modules always run before by-galaxy modules within a phase. Events emitted by full-halo producers are dispatched immediately to subscribed per-event consumers, preserving producer-side event ordering. YAML order is preserved within the same processing mode; it does not make a by-galaxy module run before a full-halo module in the same phase.

---

## Configuration

### YAML Structure

The shipped configuration is `models/sage/input/sage_millennium.yaml`. Its top-level sections are:

```yaml
model:
  name: sage
  path: models/sage
  properties: models/sage/model_properties.yaml

simulation:
  name: millennium
  path: simulations/millennium
  config: simulations/millennium/simulation_info.yaml
  halo_properties: simulations/millennium/halo_properties.yaml

plotting:
  profile: models/sage/plots/profiles/millennium_plot_profile.yaml  # optional

output:
  output_filename: model
  output_directory: output/sage-millennium
  output_format: hdf5                 # binary or hdf5
  snapshot_count: 8                   # -1 writes every snapshot
  snapshot_list: [63, 37, 32, 27, 23, 20, 18, 16]

SubSteps: 10

modules:
  pre_timestep: []
  phase_1: []
  phase_2: []
  post_timestep: []
  parameters: {}
```

The `plotting.profile` section is optional. Omit it entirely if you do not intend to use `mimic-plot.py` — the binary will run without it. If you do want to generate plots, the profile must be present in the run YAML so `mimic-plot.py` can locate it; the path must be repo-relative, not absolute. Inside a plot profile, `inherits` entries are resolved from the directory containing that profile, so model-local profiles should inherit neighbouring defaults by local filename, for example `inherits: [default.yaml]`.

The referenced simulation config, `simulations/millennium/simulation_info.yaml`, owns tree input paths, cosmology, box size, particle mass, and units. `simulation.units` in that file defines the base code units used to derive time, density, pressure, energy, `G`, and related runtime quantities. The shipped Millennium/SAGE example uses `Mpc/h`, `1e10 Msun/h`, and `km/s` conventions.

### Physics Modules

The module pipeline is configured under `modules`. The following abbreviated example shows the phase structure only — the full set of SAGE modules and their parameter values live in `models/sage/input/sage_millennium.yaml`, which is the authoritative shipped configuration.

```yaml
SubSteps: 10

modules:
  pre_timestep:
    - sage_reionization:              process_full_halo
    - sage_prepare_infall_budget:     process_full_halo
    # ... see models/sage/input/sage_millennium.yaml for the full pre_timestep block

  phase_1:
    - sage_apply_infall:              process_full_halo
    - sage_calculate_cooling_budget:  process_by_galaxy
    - sage_radio_mode_heating:        process_by_galaxy
    - sage_apply_cooling:             process_by_galaxy
    # ... star formation, supernova, disk instability, quasar, starburst ...

  phase_2:
    - sage_resolve_mergers_and_disruption: process_full_halo
    - sage_quasar_mode:               process_per_event
    - sage_starburst_feedback:        process_per_event

  post_timestep: []

  parameters:
    # Illustrative values only. See models/sage/input/sage_millennium.yaml for the calibrated set.
    GlobalBaryonFraction: 0.17
    SfrEfficiency: 0.05
    # ... cooling, AGN, BH, metals, mergers ...
```

Module parameters have no global defaults in the core. A module loads and validates the parameters it needs during its `init()` function. If a required parameter is missing, startup fails before trees are processed.

### Configuration Recipes

**Physics-free mode** writes halo-tracking output without galaxy physics:

```yaml
modules:
  pre_timestep: []
  phase_1: []
  phase_2: []
  post_timestep: []
  parameters: {}
```

**Disable a module** by removing or commenting its line. Check the surrounding modules before doing this: many SAGE modules pass transport properties to later modules in the same phase.

```yaml
phase_1:
  - sage_calculate_star_formation: process_by_galaxy
  # - sage_calculate_supernova_feedback: process_by_galaxy
  - sage_apply_star_formation_supernova: process_by_galaxy
```

**Write every snapshot** with `snapshot_count: -1`:

```yaml
output:
  snapshot_count: -1
```

**Add a snapshot** by increasing `snapshot_count` and adding the snapshot number:

```yaml
output:
  snapshot_count: 9
  snapshot_list: [63, 37, 32, 27, 23, 20, 18, 16, 12]
```

**Override simulation input defaults** by adding an `input` section to your run file. `simulation_info.yaml` defines the defaults for a simulation (e.g. which tree files to process); any `input` keys present in the run file take precedence. This lets you control a run entirely from one file without touching the shared simulation config:

```yaml
# In your model-local run file (e.g. models/sage/input/sage_millennium.yaml)
# Processes only file 0 regardless of what simulation_info.yaml sets for last_file
input:
  first_file: 0
  last_file: 0
```

**Run with MPI** after building with MPI support:

```bash
make MODEL=sage USE-MPI=yes
mpirun -np 4 ./mimic models/sage/input/sage_millennium.yaml
```

MPI parallelizes over tree files. For balanced work, choose a rank count that divides `last_file - first_file + 1`.

**Resume an interrupted run** with `--skip`:

```bash
./mimic --skip models/sage/input/sage_millennium.yaml
```

---

## Output

### Formats

Select the output format in YAML:

```yaml
output:
  output_format: hdf5   # or binary
```

HDF5 is self-documenting and portable. Binary is compact and fast, and Mimic writes `metadata/output_schema.json` beside every run so binary readers can reconstruct the exact dtype used by that executable.

### Units and Schema

The current output schema is generated at build time from:

- `src/core/core_properties.yaml` for halo-tracking properties
- `simulations/<simulation>/halo_properties.yaml` for catalog halo properties
- `models/<MODEL>/model_properties.yaml` for galaxy/model properties

The generated executable, `struct HaloOutput`, HDF5 field metadata, output schema writer, and validation ranges are therefore model-set specific. Re-run `make MODEL=<name> generate` or `make MODEL=<name>` after changing the selected model package metadata.

Each property metadata entry declares its output unit label, initialization behavior, output conversion, and whether it is written to output. HDF5 output also writes `FieldMetadata` so analysis code can inspect field names, units, and descriptions directly from the file. Binary output relies on `metadata/output_schema.json`; keep the `metadata/` directory with any binary files you move or sync elsewhere.

For the shipped Millennium/SAGE configuration, common output conventions include:

| Quantity | Typical unit label | Examples |
| --- | --- | --- |
| Mass | `1e10 Msun/h` | `Mvir`, `StellarMass`, `ColdGas` |
| Length | `Mpc/h` | `Rvir`, `Pos`, `DiskScaleRadius` |
| Velocity | `km/s` | `Vvir`, `Vmax`, `Vel` |
| Rates | `Msun/yr` or `log10(erg/s)` | `StarFormationRate`, `Cooling`, `Heating` |
| Time | `Myr/h` or `Gyr/h` | `dT`, `TimeOfLastMajorMerger` |

Do not assume these lists are universal for every future model. Treat the property metadata and HDF5 `FieldMetadata` as the source of truth.

### Reading HDF5 Output

```python
import h5py

with h5py.File("output/sage-millennium/model_000.hdf5", "r") as f:
    galaxies = f["Snap063/Galaxies"][:]
    metadata = f["Snap063/FieldMetadata"][:]

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
/Snap063/
  FieldMetadata
  Galaxies
    @Ntrees
    @TotHalosPerSnap
  TreeHalosPerSnap
```

The master HDF5 file, `model.hdf5`, contains run metadata plus external links to the per-file outputs:

```text
/RunProperties/
/Snap063/
  FieldMetadata
  File000/Galaxies -> model_000.hdf5:/Snap063/Galaxies
  File000/TreeHalosPerSnap -> model_000.hdf5:/Snap063/TreeHalosPerSnap
```

`RunProperties/EnabledModules`, `RunProperties/Parameters`, and `RunProperties/EventContracts` are the main reproducibility datasets for checking which physics pipeline was active.

### Reading Binary Output

Binary output has a small integer header followed by `HaloOutput` records. Use the run-local schema in `metadata/output_schema.json`, not the current checkout's model metadata, to construct the dtype.

```python
from pathlib import Path
import sys

import json
import numpy as np

repo = Path("/path/to/mimic")
sys.path.insert(0, str(repo / "output" / "mimic-plot"))

from output_schema import dtype_from_schema, units_from_schema

path = repo / "output" / "results" / "millennium" / "model_z0.000_0"
schema = json.loads((path.parent / "metadata" / "output_schema.json").read_text())
dtype = dtype_from_schema(schema, binary=True)
units = units_from_schema(schema)

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

Activate the virtual environment before running plotting commands:

```bash
source mimic_venv/bin/activate

python plot/mimic-plot/mimic-plot.py --param-file=models/sage/input/sage_millennium.yaml

python plot/mimic-plot/mimic-plot.py --param-file=models/sage/input/sage_millennium.yaml \
    --plots=halo_mass_function,stellar_mass_function

python plot/mimic-plot/mimic-plot.py --param-file=models/sage/input/sage_millennium.yaml \
    --snapshot-plots

python plot/mimic-plot/mimic-plot.py --param-file=models/sage/input/sage_millennium.yaml \
    --evolution-plots

deactivate
```

The plotting README is the detailed plotting manual: [plot/mimic-plot/README.md](../plot/mimic-plot/README.md). It covers command-line options, available plot names, skipped-plot diagnostics, testing, and adding new plot types. The active plot registry lives in `models/<MODEL>/plots/figures/__init__.py`; plotting should be self-contained inside the selected model package.

Plots are written under the configured output directory, normally `output/sage-millennium/plots/` for the shipped example.

---

## Troubleshooting

### Build Issues

**HDF5 not found**: Install HDF5 development libraries or build without HDF5:

```bash
make MODEL=sage USE-HDF5=no
```

On macOS, Homebrew users usually need:

```bash
brew install hdf5
```

**Generated code is stale**: Regenerate after editing property YAML, module metadata, or module files:

```bash
make MODEL=sage generate
make MODEL=sage clean && make MODEL=sage
```

**Unexpected build configuration**: Use `make MODEL=sage info` to inspect detected compiler, HDF5, MPI, model set, and feature flags.

### Runtime Issues

**Non-zero exit code**: Treat the run as failed. Check the last error messages and rerun with debug logging:

```bash
./mimic --debug models/sage/input/sage_millennium.yaml 2>&1 | tee debug.log
```

**Cannot open input files**: Check `input.simulation_dir`, `input.tree_name`, `input.first_file`, `input.last_file`, and `input.snapshot_list_file`. Mimic creates output directories but does not create or download missing input data during a normal run.

**Missing mini-Millennium data**:

```bash
./scripts/first_run.sh
```

**Module not registered**: Run:

```bash
make MODEL=sage validate-modules
make MODEL=sage generate
make MODEL=sage clean && make MODEL=sage
```

This usually indicates a new module or metadata change was not regenerated, or a YAML configuration references the wrong module name.

### Plotting Issues

**Python import errors**: Activate the virtual environment:

```bash
source mimic_venv/bin/activate
```

**No plots generated or many skipped plots**: Some plots require populated galaxy-physics fields. A physics-free run can still produce halo-property plots, but galaxy plots will be skipped. Run with `--verbose` to see skip reasons:

```bash
python plot/mimic-plot/mimic-plot.py --param-file=models/sage/input/sage_millennium.yaml --verbose
```

**Virtual environment missing**:

```bash
python3 -m venv mimic_venv
source mimic_venv/bin/activate
pip install -r requirements.txt
```

---

## Related Documentation

- [README.md](../README.md): project overview and quick start
- [VISION.md](VISION.md): architecture principles
- [DEVELOPER-GUIDE.md](DEVELOPER-GUIDE.md): module development, metadata, and testing
- [plot/mimic-plot/README.md](../plot/mimic-plot/README.md): plotting manual

## Citations

If you use the shipped SAGE model pathway, cite the relevant SAGE papers:

- Croton et al. 2016, ApJS, 222, 22
- Croton et al. 2006, MNRAS, 365, 11
