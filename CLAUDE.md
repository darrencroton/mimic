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

# Show build configuration (library detection, compiler, enabled features)
make info

# Regenerate all code from metadata (smart - only regenerates what changed)
# Run after editing property YAML files or module metadata
make generate

# Verify generated code is up-to-date (CI check)
make check-generated

# Validate module metadata (checks dependencies, properties, files)
make validate-modules

# With HDF5 support for HDF5 tree format
make USE-HDF5=yes

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
cd output/mimic-plot
./test_plotting.sh

# Generate all halo plots (both snapshot and evolution - default behavior)
python mimic-plot.py --param-file=../../input/millennium.yaml

# Generate specific plots
python mimic-plot.py --param-file=../../input/millennium.yaml --plots=halo_mass_function,spin_distribution

# Generate only snapshot plots (5 halo plots)
python mimic-plot.py --param-file=../../input/millennium.yaml --snapshot-plots

# Generate only evolution plots (1 halo plot)
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
output/mimic-plot/   Plotting system (6 halo plots, modular figures)
  └── generated/     Auto-generated Python dtypes
```

### Key Concepts

**Three-Tier Halo Architecture:**
- `InputTreeHalos`: Raw merger tree input (immutable)
- `FoFWorkspace`: Temporary processing workspace (dynamic)
- `ProcessedHalos`: Final processed halos (written to output)

**Metadata-Driven Property System:**
- Halo properties: `src/core/halo_properties.yaml`
- Model properties: `src/modules/model_properties.yaml`
- Auto-generated into C structs via `make generate`
- Includes: struct Halo, struct GalaxyData, struct HaloOutput
- Python dtypes auto-generated for reading output

**Model Parameter System:**
- Parameters loaded from input YAML file (required, no defaults)
- Type-safe access via `model_get_double()`, `model_get_int()`, `model_get_string()`
- Physics-based validation in each module's init function
- Complete module independence: modules read and validate their own parameters

**Module System:**
- Runtime-configurable via multi-phase pipeline in YAML
- Four execution phases: pre_timestep, phase_1, phase_2, post_timestep
- Two processing modes: PROCESSING_MODE_FULL_HALO (module processes full array), PROCESSING_MODE_BY_GALAXY (galaxy-major loop)
- Physics-agnostic core (zero knowledge of specific modules)
- Module lifecycle: init → process → cleanup

**Multi-Phase Pipeline:**
- **pre_timestep**: Setup calculations (runs once before substeps)
  - Example: reionization, infall budget calculation
- **phase_1**: Main physics (runs each substep for each galaxy)
  - Example: cooling, star formation, feedback, stripping
- **phase_2**: Secondary physics (runs each substep for each galaxy)
  - Example: mergers, disruption
- **post_timestep**: Finalization (runs once after all substeps)
  - Example: converting accumulators to rates
- **SubSteps**: Time sub-stepping parameter for numerical stability (1 = no substeps)

**Memory Management:**
- Custom allocator with leak detection
- Categorized tracking (MEM_HALOS, MEM_TREES, MEM_IO, MEM_UTILITY)
- Use `print_allocated()` or `print_allocated_by_category()` to check leaks

**Core Execution Flow:**
1. `load_tree_table()` → Load tree metadata
2. `build_halo_tree()` → Recursively construct halo tracking structures
3. `process_halo_evolution()` → Execute multi-phase pipeline:
   - Execute pre_timestep phase (once)
   - Loop over SubSteps:
     - Execute phase_1 (galaxy-major or array)
     - Execute phase_2 (galaxy-major or array)
   - Execute post_timestep phase (once)
4. `save_halos()` → Write to binary or HDF5 output
5. `free_halos_and_tree()` → Cleanup memory

**HDF5 Output Structure:**
Mimic's HDF5 output is self-contained and fully reproducible, containing both data and complete metadata.

Master file (`model.hdf5`):
```
RunProperties/
  ├── @BoxSize, @Hubble_h, @Omega, etc.    (simulation config)
  ├── Version/                             (identity & provenance)
  │   ├── @git_commit                      (e.g., e1288e79...)
  │   ├── @git_branch                      (e.g., main)
  │   ├── @git_date, @build_date           (build timestamps)
  │   └── @hdf5_format_version             (schema version: 1.0)
  ├── EnabledModules [dataset]             (active physics modules list)
  ├── Parameters [dataset]                 (all runtime parameters)
  │   └── (param_name, value) pairs        (e.g., AGNrecipeOn: 1)
  └── Redshifts [dataset]                  (z for each snapshot: 127.0→0.0)

Snap063/
  ├── FieldMetadata [dataset]              (field names, units, descriptions)
  └── File000/
      ├── Galaxies [external link]         (→ model_000.hdf5)
      └── TreeHalosPerSnap [external link] (→ model_000.hdf5)
```

Per-file output (`model_000.hdf5`):
```
RunProperties/                             (same as master - self-contained)
  ├── Version/                             (identity & provenance)
  ├── EnabledModules [dataset]             (configuration)
  ├── Parameters [dataset]                 (configuration)
  └── Redshifts [dataset]                  (auxiliary data)

Snap063/
  ├── FieldMetadata [dataset]              (47 fields with metadata)
  ├── Galaxies [compound dataset]          (9265 halos, all properties)
  │   └── @Ntrees, @TotHalosPerSnap
  └── TreeHalosPerSnap [dataset]           (halos per tree array)
```

Benefits:
- **Self-contained**: Each file has complete metadata for standalone analysis
- **Reproducible**: Version info and all parameters stored for exact reproduction
- **Self-documenting**: FieldMetadata describes every field (name, units, description)
- **No external dependencies**: Redshifts included (no need for .a_list file)

See also:
- **docs/architecture/vision.md**: Architectural principles and future vision
- **docs/architecture/roadmap.md**: Development roadmap (Phases 1-3 complete)
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