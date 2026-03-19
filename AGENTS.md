# AGENTS.md — Mimic

This file provides guidance to AI coding assistants (Claude Code, Codex CLI, etc.) working in the Mimic repository. Global behavioural rules (code quality, git workflow, file organisation) are defined in the global AGENTS.md and apply here without repetition.

---

## Quick Setup

For new repository clones, use the automated setup script:

```bash
# Complete setup from fresh clone
./scripts/first_run.sh

# Creates directories, downloads data, sets up Python environment
# Creates mimic_venv/ virtual environment with plotting dependencies
```

---

## Build Commands

```bash
# Basic compilation
make

# Note: Property code auto-regenerates during `make` when YAML changes

# Show build configuration (library detection, compiler, enabled features)
make info

# Regenerate all code from metadata (smart — only regenerates what changed)
# Run after editing property YAML files or module metadata
make generate

# Verify generated code is up-to-date (CI check)
make check-generated

# Validate documentation links and USER-GUIDE module phase consistency
make check-docs

# Validate module metadata (checks dependencies, properties, files)
make validate-modules

# HDF5 is enabled by default; disable with:
make USE-HDF5=no

# With MPI support for parallel processing
make USE-MPI=yes

# Parallel build (faster — uses all cores)
make -j$(nproc)

# Clean build artifacts
make clean

# Remove object files but keep executable
make tidy
```

---

## Code Formatting

```bash
# Format all C and Python code
./scripts/beautify.sh

# Format only C code (requires clang-format)
./scripts/beautify.sh --c-only

# Format only Python code (requires black and isort)
./scripts/beautify.sh --py-only
```

---

## Running Mimic

```bash
# Basic execution
./mimic input/millennium.yaml

# Verbosity options
./mimic --debug input/millennium.yaml    # Most verbose (debug output + context)
./mimic --verbose input/millennium.yaml  # Add context (timestamp, file:line)
./mimic --quiet input/millennium.yaml    # Warnings/errors only
./mimic --skip input/millennium.yaml     # Skip existing output files
```

---

## Testing

Long-running tests should be captured to a log file; check the exit code explicitly:

```bash
mkdir -p archive/test-logs
make test-unit > archive/test-logs/test-unit.log 2>&1
test_rc=$?
tail -n 60 archive/test-logs/test-unit.log
rg -n -i "failed|error|traceback" archive/test-logs/test-unit.log
echo "exit_code=${test_rc}"
# Treat any non-zero exit code as failure, regardless of log text
```

```bash
# Run all tests (validates metadata first, then all tiers)
make tests

# Run specific tiers
make validate-modules   # Validate module metadata only
make test-unit          # C unit tests (fast, <10s)
make test-integration   # Python integration tests (medium, <1min)
make test-scientific    # Python scientific validation (slow, <5min)

Individual tests: use `tests/unit/run_tests.sh <test_name>` for C unit tests and `python3 path/to/test.py` for integration/scientific scripts.

# Test data loader
cd tests && python -c "from framework import load_binary_halos; print(load_binary_halos.__doc__)"
```

### Plotting Tests

```bash
# Activate virtual environment first
source mimic_venv/bin/activate

# Plotting unit tests
cd output/mimic-plot/tests
python3 test_validation_helpers.py

# Plotting integration tests
./test_plotting.sh

# Generate all plots (18 snapshot + 4 evolution)
cd ..
python mimic-plot.py --param-file=../../input/millennium.yaml

# Generate specific plots
python mimic-plot.py --param-file=../../input/millennium.yaml --plots=halo_mass_function,spin_distribution

# Snapshot-only or evolution-only
python mimic-plot.py --param-file=../../input/millennium.yaml --snapshot-plots
python mimic-plot.py --param-file=../../input/millennium.yaml --evolution-plots

# Works from any directory
cd ../..
python output/mimic-plot/mimic-plot.py --param-file=input/millennium.yaml --plots=halo_mass_function

deactivate
```

---

## Benchmarking

```bash
# Run performance benchmark (default uses input/millennium.yaml)
cd scripts
./benchmark_mimic.sh

# With options
./benchmark_mimic.sh --param-file ../tests/data/test_binary.yaml
./benchmark_mimic.sh --verbose
MAKE_FLAGS="USE-HDF5=no" ./benchmark_mimic.sh
MPI_RUN_COMMAND="mpirun -np 4" MAKE_FLAGS="USE-MPI=yes" ./benchmark_mimic.sh

# Results saved to benchmarks/ (gitignored)
diff ../benchmarks/baseline_YYYYMMDD_HHMMSS.json ../benchmarks/baseline_YYYYMMDD_HHMMSS.json
```

---

## Code Architecture

For comprehensive details see `docs/DEVELOPER-GUIDE.md` (architecture, module development, API reference) and `docs/USER-GUIDE.md` (configuration, pipeline setup, output formats).

### Directory Structure

```
src/
├── core/          Core execution (main, init, build_model, parameter reading)
│   └── halo_properties.yaml    Halo property metadata (auto-generates C code)
├── io/
│   ├── tree/      Tree readers (binary, HDF5 formats)
│   └── output/    Output writers (binary, HDF5)
├── util/          Utilities (memory, error, numeric, version, I/O)
├── modules/       Physics modules
│   ├── model_properties.yaml   Model property metadata (auto-generates C code)
│   ├── _archive/               Archived modules (historical reference)
│   ├── _system/                Framework infrastructure (do not modify)
│   │   ├── physical_constants.h  Universal physical constants (G, c, Z_sun, etc.)
│   │   ├── output_helpers.h      Output formatting utilities
│   │   ├── generated/            Auto-generated module registration
│   │   ├── template/             Template for creating new modules
│   │   └── test_fixture/         Infrastructure testing module
│   ├── _shared/                User physics utilities (can modify/add)
│   │   └── *.h                   Reusable physics calculations and swappable models
│   └── <module_name>/          Individual physics modules
└── include/       Headers (types, globals, constants)
    └── generated/ Auto-generated property code and model parameter validation

build/generated/     Build-time generated files (git_version.h, test lists)
tests/               Unit, integration, and scientific tests
  └── generated/     Auto-generated test metadata
output/mimic-plot/   Plotting system (22 plots: 18 snapshot, 4 evolution)
  ├── tests/         Plotting system tests (unit and integration)
  └── generated/     Auto-generated Python dtypes
```

### Key Concepts

**Halo Data Structures:** `InputTreeHalos` (immutable tree input) → `FoFWorkspace` (processing workspace) → `ProcessedHalos` (written to output). Structs: `struct Halo`, `struct GalaxyData`, `struct HaloOutput`.

**Property System:** Properties are defined in YAML (`src/core/halo_properties.yaml`, `src/modules/model_properties.yaml`) and generated via `make generate` into C structs, init/output logic, and Python dtypes.

**Module System:** Runtime-configurable physics modules execute through a 4-phase pipeline (`pre_timestep` → `phase_1` → `phase_2` → `post_timestep`) with two processing modes: `PROCESSING_MODE_FULL_HALO` and `PROCESSING_MODE_BY_GALAXY`. Module lifecycle: `init()` → `process()` → `cleanup()`.

**Parameters:** Model parameters come from the input YAML and are accessed via typed getters (`model_get_double()`, `model_get_int()`, `model_get_string()`), with module-local validation in each module `init()`.

**Core Execution Flow:**
1. `load_tree_table()` — Load tree metadata
2. `build_halo_tree()` — Construct halo tracking structures
3. `process_halo_evolution()` — Execute multi-phase pipeline
4. `save_halos()` — Write to binary or HDF5 output
5. `free_halos_and_tree()` — Cleanup memory

**Memory:** Custom allocator with leak detection and categorised tracking (`MEM_GALAXIES`, `MEM_HALOS`, `MEM_TREES`, `MEM_IO`, `MEM_UTILITY`).

**Output:** Binary/HDF5 structure documented in `docs/USER-GUIDE.md#output`.

---

## Documentation Reference

- **docs/VISION.md** — Architectural principles and design philosophy
- **docs/USER-GUIDE.md** — Complete user guide (installation, configuration, running simulations)
- **docs/DEVELOPER-GUIDE.md** — Complete developer guide (architecture, modules, testing)
