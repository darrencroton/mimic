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
make MODEL=sage

# Note: Property code auto-regenerates during `make` when YAML changes
# MODEL selects the model set. It defaults to DEFAULT_MODEL (sage) in the
# Makefile, so plain `make` builds the default; override per-invocation with
# MODEL=<name>, or change DEFAULT_MODEL if your primary model is not sage. If the
# selected package is missing (renamed/removed), make fails loudly with
# "Unknown MODEL" rather than mis-building. Mimic builds one model set at a time:
# MODEL=<name> selects models/<name>/ for properties, modules, model-local shared
# helpers, tests, and plotting.

# Show build configuration (library detection, compiler, enabled features)
make MODEL=sage info

# Regenerate all code from metadata (smart — only regenerates what changed)
# Run after editing property YAML files or module metadata
make MODEL=sage generate

# Verify generated code is up-to-date (CI check)
make MODEL=sage check-generated

# Validate documentation links and anchors
make check-docs

# Validate module metadata (checks dependencies, properties, files)
make MODEL=sage validate-modules

# HDF5 is enabled by default; disable with:
make MODEL=sage USE-HDF5=no

# With MPI support for parallel processing
make MODEL=sage USE-MPI=yes

# Parallel build (faster — uses all cores)
make MODEL=sage -j$(nproc)

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
./mimic models/sage/input/sage_millennium.yaml
./mimic models/sham/input/sham_millennium.yaml    # SHAM model

# Verbosity options
./mimic --debug models/sage/input/sage_millennium.yaml    # Most verbose (debug output + context)
./mimic --verbose models/sage/input/sage_millennium.yaml  # Add context (timestamp, file:line)
./mimic --quiet models/sage/input/sage_millennium.yaml    # Warnings/errors only
./mimic --skip models/sage/input/sage_millennium.yaml     # Skip existing output files
```

---

## Testing

Long-running tests should be captured to a log file; check the exit code explicitly:

```bash
mkdir -p archive/test-logs
make MODEL=sage test-unit > archive/test-logs/test-unit.log 2>&1
test_rc=$?
tail -n 60 archive/test-logs/test-unit.log
rg -n -i "failed|error|traceback" archive/test-logs/test-unit.log
echo "exit_code=${test_rc}"
# Treat any non-zero exit code as failure, regardless of log text
```

### Testing Strategy

Full test suites (`make MODEL=sage tests`, `make MODEL=sage test-scientific`) can take 5+ minutes
and produce large output. **Delegate these to a subagent**: have it capture and summarise the
results, then act on the report in the main context rather than filling it with raw test output.

```bash
# Run all tests (validates metadata first, then all tiers)
make MODEL=sage tests

# Run specific tiers
make MODEL=sage validate-modules   # Validate module metadata only
make MODEL=sage test-unit          # C unit tests (fast, <10s)
make MODEL=sage test-integration   # Python integration tests (medium, <1min)
make MODEL=sage test-scientific    # Python scientific validation (slow, <5min)
```

Individual tests: use `tests/unit/run_tests.sh <test_name>` for C unit tests and `python3 path/to/test.py` for integration/scientific scripts.

```bash
# Test data loader
cd tests && python -c "from framework import load_binary_halos; print(load_binary_halos.__doc__)"
```

### Plotting Tests

```bash
# Activate virtual environment first
source mimic_venv/bin/activate

# Plotting unit tests
cd plot/mimic-plot/tests
python3 test_validation_helpers.py

# Plotting integration tests
./test_plotting.sh

# Generate all plots (18 snapshot + 4 evolution)
cd ..
python mimic-plot.py --param-file=../../models/sage/input/sage_millennium.yaml

# Generate specific plots
python mimic-plot.py --param-file=../../models/sage/input/sage_millennium.yaml --plots=halo_mass_function,spin_distribution

# Snapshot-only or evolution-only
python mimic-plot.py --param-file=../../models/sage/input/sage_millennium.yaml --snapshot-plots
python mimic-plot.py --param-file=../../models/sage/input/sage_millennium.yaml --evolution-plots

# Works from any directory
cd ../..
python plot/mimic-plot/mimic-plot.py --param-file=models/sage/input/sage_millennium.yaml --plots=halo_mass_function

deactivate
```

---

## Benchmarking

```bash
# Run performance benchmark (default uses models/sage/input/sage_millennium.yaml)
cd scripts
./benchmark_mimic.sh

# With options
./benchmark_mimic.sh --param-file ../models/sage/input/test_binary.yaml
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
│   └── core_properties.yaml    Core halo property metadata (auto-generates C code)
├── io/
│   ├── tree/      Tree readers (binary, HDF5 formats)
│   └── output/    Output writers (binary, HDF5)
├── util/          Utilities (memory, error, numeric, version, I/O)
├── module_system/ Framework infrastructure (do not modify)
│   ├── physical_constants.h  Universal physical constants (G, c, Z_sun, etc.)
│   ├── output_helpers.h      Output formatting utilities
│   ├── generated/            Auto-generated module registration
│   ├── template/             Template for creating new modules
│   └── test_fixture/         Infrastructure testing module
└── include/       Headers (types, globals, constants)
    └── generated/ Auto-generated property code and model parameter validation

models/
├── sage/
│   ├── input/    SAGE run parameter YAML files
│   ├── model_properties.yaml
│   ├── modules/  SAGE physics modules and module-local tests
│   ├── shared/   SAGE-local helper APIs
│   └── plots/    SAGE plotting figures and profiles
└── sham/
    ├── input/    SHAM run parameter YAML files
    ├── model_properties.yaml
    ├── modules/  SHAM physics modules and module-local tests
    └── plots/    SHAM plotting figures and profiles

simulations/
└── millennium/    Millennium metadata, halo properties, and snapshot lists

build/generated/     Build-time generated files (git_version.h, test lists)
tests/               Unit, integration, and scientific tests
  └── generated/     Auto-generated test metadata
plot/mimic-plot/     Plotting system (22 plots: 18 snapshot, 4 evolution)
  ├── tests/         Plotting system tests (unit and integration)
  └── output_schema.py  Run-local schema reader for binary outputs
```

### Key Concepts

**Halo Data Structures:** `InputTreeHalos` (immutable tree input) → `FoFWorkspace` (processing workspace) → `ProcessedHalos` (written to output). Structs: `struct Halo`, `struct GalaxyData`, `struct HaloOutput`.

**Property System:** Properties are defined in YAML (`src/core/core_properties.yaml`, `simulations/<simulation>/halo_properties.yaml`, `models/<MODEL>/model_properties.yaml`) and generated via `make MODEL=<name> generate` into C structs, init/output logic, HDF5 metadata writers, and run-local binary output schemas.

**Model Set Boundary:** Mimic compiles one model set at a time via `MODEL=<name>`. A model package must be self-contained for running and plotting: run parameter YAML files, properties, modules, model-local `shared/` helpers, tests, and plot figures should live under `models/<model>/`. To mix modules from different model families, create a new model package and reconcile property names, parameter names, units, dependencies, tests, and plots there.

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
