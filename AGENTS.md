# AGENTS.md

This file provides guidance to AI coding assistants (Claude Code, Codex CLI, etc.) working in this repository.

---

## ⚠️ CRITICAL: Development Rules

**These rules are MANDATORY and override default behaviors:**

### Code Quality (MUST DO)
- ✅ **MUST** work to highest professional coding standards at all times
- ✅ **MUST** write documentation as you go (documentation-as-you-go always)
- ✅ **MUST** check mimic exit codes after every execution and report failures immediately

### Code Quality (NEVER DO)
- ❌ **NEVER** simplify tests - failing tests indicate real problems that must be fixed
- ❌ **NEVER** delete files - **ALWAYS** archive to `ignore/` subdirectories instead

### Git Workflow (REQUIRE USER APPROVAL)
- 🔒 **MUST** ask before committing to git (never commit without explicit user approval)
- 📝 **Commit messages MUST**:
  - Be meaningful and descriptive
  - List every changed file, grouped logically
  - Include reason for each change

### File Organization
- 📁 When asked to write to obsidian → use `obsidian-inbox/` directory
- 🗄️ When removing files → move to `ignore/` subdirectories (never delete)

---

## Quick Setup

For new repository clones, use the automated setup script:

```bash
# Complete setup from fresh clone
./scripts/first_run.sh

# This creates directories, downloads data, sets up Python environment
# Creates mimic_venv/ virtual environment with plotting dependencies
```

## Build Commands

```bash
# Basic compilation
make

# Note: Property code auto-regenerates during `make` when YAML changes

# Show build configuration (library detection, compiler, enabled features)
make info

# Regenerate all code from metadata (smart - only regenerates what changed)
# Run after editing property YAML files or module metadata
make generate

# Verify generated code is up-to-date (CI check)
make check-generated

# Validate documentation links and USER-GUIDE module phase consistency
make check-docs

# Validate module metadata (checks dependencies, properties, files)
make validate-modules

# HDF5 is enabled by default; disable with
make USE-HDF5=no

# With MPI support for parallel processing
make USE-MPI=yes

# Parallel build (faster compilation - use all cores)
make -j$(nproc)

# Clean build artifacts
make clean

# Remove object files but keep executable
make tidy
```

## Code Formatting

```bash
# Format all C and Python code
./scripts/beautify.sh

# Format only C code (requires clang-format)
./scripts/beautify.sh --c-only

# Format only Python code (requires black and isort)
./scripts/beautify.sh --py-only
```

## Running Mimic

```bash
# Basic execution
./mimic input/millennium.yaml

# With command-line options
./mimic --debug input/millennium.yaml    # Enable debug output with context (most verbose)
./mimic --verbose input/millennium.yaml  # Add context (timestamp, file:line)
./mimic --quiet input/millennium.yaml    # Only warnings/errors (least verbose)
./mimic --skip input/millennium.yaml     # Skip existing output files
```

## Testing

```bash
# Run all tests (validates metadata first, then runs all test tiers)
make tests

# Run specific steps
make validate-modules   # Validate module metadata only
make test-unit          # C unit tests (fast, <10s)
make test-integration   # Python integration tests (medium, <1min)
make test-scientific    # Python scientific validation (slow, <5min)

# Run individual tests
cd tests/unit && ./test_memory_system.test
cd tests/integration && python test_full_pipeline.py
cd tests/scientific && python test_scientific.py

# Test data loader (shared framework for scientific tests)
cd tests && python -c "from framework import load_binary_halos; print(load_binary_halos.__doc__)"

# Test the plotting system (activate virtual environment first)
source mimic_venv/bin/activate

# Run plotting validation unit tests
cd output/mimic-plot/tests
python3 test_validation_helpers.py

# Run plotting integration tests
./test_plotting.sh

# Generate all plots (both snapshot and evolution - default behavior)
# 18 snapshot plots + 4 evolution plots
cd ..
python mimic-plot.py --param-file=../../input/millennium.yaml

# Generate specific plots
python mimic-plot.py --param-file=../../input/millennium.yaml --plots=halo_mass_function,spin_distribution

# Generate only snapshot plots (18 plots: 5 halo + 13 galaxy physics)
python mimic-plot.py --param-file=../../input/millennium.yaml --snapshot-plots

# Generate only evolution plots (4 plots: 1 halo + 3 galaxy physics)
python mimic-plot.py --param-file=../../input/millennium.yaml --evolution-plots

# Cross-directory execution works from anywhere
cd ../..
python output/mimic-plot/mimic-plot.py --param-file=input/millennium.yaml --plots=halo_mass_function

# Deactivate when done
deactivate
```

## Benchmarking

```bash
# Run performance benchmark (from scripts/ directory)
# Default uses input/millennium.yaml
cd scripts
./benchmark_mimic.sh

# Benchmark with custom parameter file
./benchmark_mimic.sh --param-file ../tests/data/test_binary.yaml
./benchmark_mimic.sh ../tests/data/test_binary.yaml

# Benchmark with verbose output
./benchmark_mimic.sh --verbose

# Benchmark with specific build flags
MAKE_FLAGS="USE-HDF5=no" ./benchmark_mimic.sh   # Opt out of HDF5 if needed

# Benchmark with MPI
MPI_RUN_COMMAND="mpirun -np 4" MAKE_FLAGS="USE-MPI=yes" ./benchmark_mimic.sh

# Results saved to benchmarks/ directory (gitignored)
# Compare benchmark results between runs
diff ../benchmarks/baseline_YYYYMMDD_HHMMSS.json ../benchmarks/baseline_YYYYMMDD_HHMMSS.json
```

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
│   ├── _system/                Framework infrastructure (don't modify)
│   │   ├── physical_constants.h  Universal physical constants (G, c, Z_sun, etc.)
│   │   ├── output_helpers.h      Output formatting utilities
│   │   ├── generated/            Auto-generated module registration
│   │   ├── template/             Template for creating new modules
│   │   └── test_fixture/         Infrastructure testing module
│   ├── _shared/                User physics utilities (can modify/add)
│   │   └── *.h                   Reusable physics calculations and swappable models
│   ├── module_a/               Example physics module
│   ├── module_b/               Example physics module
│   └── module_c/               Example physics module
└── include/       Headers (types, globals, constants)
    └── generated/ Auto-generated property code and model parameter validation

build/generated/     Build-time generated files (git_version.h, test lists)
tests/               Unit, integration, and scientific tests
  └── generated/     Auto-generated test metadata
docs/generated/      Auto-generated documentation
output/mimic-plot/   Plotting system (22 plots: 18 snapshot, 4 evolution)
  ├── tests/         Plotting system tests (unit and integration)
  └── generated/     Auto-generated Python dtypes
```

### Key Concepts

**Halo Data Structures:** `InputTreeHalos` (immutable tree input) → `FoFWorkspace` (processing workspace) → `ProcessedHalos` (written to output). Structs: `struct Halo`, `struct GalaxyData`, `struct HaloOutput`.

**Property System:** Properties are defined in YAML (`src/core/halo_properties.yaml`, `src/modules/model_properties.yaml`) and generated via `make generate` into C structs, init/output logic, and Python dtypes.

**Module System:** Runtime-configurable physics modules execute through a 4-phase pipeline (`pre_timestep` → `phase_1` → `phase_2` → `post_timestep`) with two processing modes: `PROCESSING_MODE_FULL_HALO` and `PROCESSING_MODE_BY_GALAXY`. Module lifecycle is `init()` → `process()` → `cleanup()`.

**Parameters:** Model parameters come from the input YAML and are accessed via typed getters (`model_get_double()`, `model_get_int()`, `model_get_string()`), with module-local validation in each module `init()`.

**Core Execution Flow:**
1. `load_tree_table()` → Load tree metadata
2. `build_halo_tree()` → Construct halo tracking structures
3. `process_halo_evolution()` → Execute multi-phase pipeline
4. `save_halos()` → Write to binary or HDF5 output
5. `free_halos_and_tree()` → Cleanup memory

**Memory:** Custom allocator with leak detection and categorized tracking (`MEM_HALOS`, `MEM_TREES`, `MEM_IO`, `MEM_UTILITY`, `MEM_PHYSICS`).

**Output:** Detailed binary/HDF5 structure is documented in `docs/USER-GUIDE.md#output`.

---

## Documentation Reference

For comprehensive documentation, see:
- **docs/VISION.md**: Architectural principles and design philosophy
- **docs/USER-GUIDE.md**: Complete user guide (installation, configuration, running simulations)
- **docs/DEVELOPER-GUIDE.md**: Complete developer guide (architecture, modules, testing)
