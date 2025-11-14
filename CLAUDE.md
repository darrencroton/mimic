# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.


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

# Regenerate property code from metadata (after editing YAML files)
make generate

# Verify generated code is up-to-date (CI check)
make check-generated

# With HDF5 support for HDF5 tree format
make USE-HDF5=yes

# With MPI support for parallel processing
make USE-MPI=yes

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
./mimic input/millennium.par

# With command-line options
./mimic --verbose input/millennium.par
./mimic --quiet input/millennium.par
./mimic --skip input/millennium.par
```

## Testing

```bash
# Run all tests (unit + integration + scientific)
make tests

# Run specific test tiers
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
cd output/mimic-plot
./test_plotting.sh

# Generate all halo plots (both snapshot and evolution - default behavior)
python mimic-plot.py --param-file=../../input/millennium.par

# Generate specific plots
python mimic-plot.py --param-file=../../input/millennium.par --plots=halo_mass_function,spin_distribution

# Generate only snapshot plots (5 halo plots)
python mimic-plot.py --param-file=../../input/millennium.par --snapshot-plots

# Generate only evolution plots (1 halo plot)
python mimic-plot.py --param-file=../../input/millennium.par --evolution-plots

# Cross-directory execution works from anywhere
cd ../..
python output/mimic-plot/mimic-plot.py --param-file=input/millennium.par --plots=halo_mass_function

# Deactivate when done
deactivate
```

## Benchmarking

```bash
# Run performance benchmark (from scripts/ directory)
# Default uses input/millennium.par
cd scripts
./benchmark_mimic.sh

# Benchmark with custom parameter file
./benchmark_mimic.sh --param-file ../tests/data/test_binary.par
./benchmark_mimic.sh ../tests/data/test_binary.par

# Benchmark with verbose output
./benchmark_mimic.sh --verbose

# Benchmark with specific build flags
MAKE_FLAGS="USE-HDF5=yes" ./benchmark_mimic.sh

# Benchmark with MPI
MPI_RUN_COMMAND="mpirun -np 4" MAKE_FLAGS="USE-MPI=yes" ./benchmark_mimic.sh

# Results saved to benchmarks/ directory (gitignored)
# Compare benchmark results between runs
diff ../benchmarks/baseline_YYYYMMDD_HHMMSS.json ../benchmarks/baseline_YYYYMMDD_HHMMSS.json
```

## Code Architecture

### Directory Structure
```
src/
├── core/          Core execution (main, init, build_model, parameter reading)
├── io/
│   ├── tree/      Tree readers (binary, HDF5 formats)
│   └── output/    Output writers (binary, HDF5)
├── util/          Utilities (memory, error, numeric, version, I/O)
├── modules/       Physics modules (sage_infall, sage_cooling, etc.)
│   └── generated/ Auto-generated module registration code
└── include/       Headers (types, globals, constants)
    └── generated/ Auto-generated property code

build/generated/     Build-time generated files (git_version.h, test lists)
metadata/            Property definitions (YAML → auto-generated C code)
tests/               Unit, integration, and scientific tests
  └── generated/     Auto-generated test metadata
docs/generated/      Auto-generated documentation
output/mimic-plot/   Plotting system (6 halo plots, modular figures)
  └── generated/     Auto-generated Python dtypes
```

### Key Concepts

**Three-Tier Halo Architecture:**
- `InputTreeHalos`: Raw merger tree input (immutable)
- `FoFWorkspace`: Temporary processing workspace (dynamic)
- `ProcessedHalos`: Final processed halos (written to output)

**Metadata-Driven Property System:**
- Properties defined in `metadata/*.yaml`
- Auto-generated into C structs via `make generate`
- Includes: struct Halo, struct GalaxyData, struct HaloOutput
- Python dtypes auto-generated for reading output

**Module System (Phase 3 complete):**
- Runtime-configurable via `EnabledModules` parameter
- Physics-agnostic core (zero knowledge of specific modules)
- Module lifecycle: init → process → cleanup
- Module parameters: `ModuleName_ParameterName` format

**Memory Management:**
- Custom allocator with leak detection
- Categorized tracking (MEM_HALOS, MEM_TREES, MEM_IO, MEM_UTILITY)
- Use `print_allocated()` or `print_allocated_by_category()` to check leaks

**Core Execution Flow:**
1. `load_tree_table()` → Load tree metadata
2. `build_halo_tree()` → Recursively construct halo tracking structures
3. `save_halos()` → Write to binary or HDF5 output
4. `free_halos_and_tree()` → Cleanup memory

See also:
- **docs/architecture/vision.md**: Architectural principles and future vision
- **docs/architecture/roadmap_v4.md**: Development roadmap (Phases 1-3 complete)
- **docs/developer/getting-started.md**: Developer setup and workflow
- **docs/developer/testing.md**: Comprehensive testing guide and standards
- **docs/user/module-configuration.md**: Guide to configuring physics modules

## Development Guidelines
- All work to highest professional coding standards
- Documentation-as-you-go always
- When running mimic always check the exit code for success or failure
- Never simplify tests - failing tests indicate real problems
- Ask before committing to git
- Commit messages must be meaningful and list every changed file, grouped logically, with a brief reason for each
- When asked to write to obsidian, use `obsidian-inbox/`
- Never delete files! Archive to `ignore/` instead