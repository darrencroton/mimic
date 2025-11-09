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
make test

# Run specific test tiers
make test-unit          # C unit tests (fast, <10s)
make test-integration   # Python integration tests (medium, <1min)
make test-scientific    # Python scientific tests (slow, <5min)

# Run individual tests
cd tests/unit && ./test_memory_system.test
cd tests/integration && python test_full_pipeline.py
cd tests/scientific && python test_physics_sanity.py

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

## Code Architecture

The codebase follows a hierarchical structure under `src/`:

### Directory Structure
- **src/core/**: Core execution (main, initialization, model building, parameter reading)
- **src/io/**: Input/output operations
  - **tree/**: Tree readers (interface, binary, HDF5 formats)
  - **output/**: Output writers (binary, HDF5, utilities)
- **src/util/**: Utility functions (memory, error, numeric, version, etc.)
- **src/modules/**: Physics modules (currently empty, will contain galaxy physics modules)
- **src/include/**: Public headers (types, globals, constants, config, proto)

### Core Execution Flow
- **src/core/main.c**: Program entry point, handles initialization, file processing loop, and cleanup
- **src/core/init.c**: System initialization, memory setup, parameter validation
- **src/core/read_parameter_file.c**: Parameter file parsing and configuration setup
- **src/core/build_model.c**: Halo tracking and property updates through merger trees
- **src/core/halo_properties/virial.c**: Halo initialization and virial property calculations

### I/O System
- **src/io/tree/interface.c**: Master tree loading interface
- **src/io/tree/binary.c**: Binary format tree reader (LHalo format)
- **src/io/tree/hdf5.c**: HDF5 format tree reader (Genesis format)
- **src/io/output/binary.c**: Binary output format writer (halo properties only)
- **src/io/output/hdf5.c**: HDF5 output format writer (halo properties only, 24 fields)
- **src/io/output/util.c**: Shared output utilities for both binary and HDF5 formats
  - I/O wrappers (`myfread`, `myfwrite`, `myfseek`) handle endianness and errors and call the C standard library (fread/fwrite/fseek). There is no custom buffering layer.

### Module System
- **src/core/module_interface.h**: Module API definition (lifecycle: init → process → cleanup)
- **src/core/module_registry.{c,h}**: Module registration and execution pipeline management
- **src/modules/module_init.c**: Module registration (manually maintained in Phase 3, will be auto-generated in Phase 5)
- **src/modules/simple_cooling/**: Placeholder cooling module (provides ColdGas property)
- **src/modules/simple_sfr/**: Placeholder star formation module (converts ColdGas → StellarMass)
- **Module Configuration**: Enabled via `EnabledModules` parameter in `.par` files, module-specific parameters use `ModuleName_ParameterName` format
- **Physics-Agnostic Core**: Core has zero knowledge of specific physics modules, all interaction through well-defined interfaces

### Utilities
- **src/util/memory.c**: Custom memory management with leak detection and categorization
- **src/util/error.c**: Comprehensive error handling and logging system
- **src/util/numeric.c**: Numerical stability functions and safe math operations
- **src/util/parameters.c**: Parameter processing and validation
- **src/util/integration.c**: Numerical integration routines
- **src/util/io.c**: File I/O utilities (file copying, etc.)
- **src/util/version.c**: Version tracking and metadata generation

### Data Structures
- **src/include/types.h**: Core data structures - three-tier halo tracking architecture
  - `struct RawHalo`: Immutable merger tree input data (from simulation files)
  - `struct Halo`: Mutable halo tracking structure (auto-generated from metadata)
  - `struct GalaxyData`: Baryonic physics properties (auto-generated from metadata)
  - `struct HaloOutput`: Output format structure (auto-generated from metadata)
  - `struct HaloAuxData`: Auxiliary processing metadata
  - `struct MimicConfig`: Configuration parameters
  - **Property Metadata System**: Halo/galaxy properties defined in `metadata/properties/*.yaml` and auto-generated into C code via `make generate`
  - Runtime simulation state is tracked via individual global variables declared in `globals.h` (e.g., `Ntrees`, `FileNum`, `TreeID`, `NumProcessedHalos`).
- **src/include/globals.h**: Global variable declarations for halo arrays
  - `InputTreeHalos`: Raw merger tree input (RawHalo*)
  - `FoFWorkspace`: Temporary FoF processing workspace (Halo*)
  - `ProcessedHalos`: Permanent storage for current tree (Halo*)
- **src/include/constants.h**: Numerical constants
- **src/include/config.h**: Compile-time configuration options

### Key Design Patterns
1. **Three-Tier Halo Architecture**: Clear separation between input (InputTreeHalos), processing (FoFWorkspace), and storage (ProcessedHalos)
2. **Physics-Agnostic Core**: Core infrastructure has zero knowledge of specific physics modules; all interaction through module interfaces
3. **Runtime Modularity**: Physics modules can be enabled/disabled via parameter files without recompilation
4. **Metadata-Driven Architecture**: Property definitions and (future) module registration auto-generated from YAML metadata
5. **Memory Categories**: Memory allocation is tracked by category (halos, trees, parameters, etc.)
6. **Error Propagation**: Consistent error handling with context preservation throughout the call stack
7. **Format Abstraction**: I/O operations abstracted to support multiple tree and output formats
8. **State Management**: Single source of truth via globals for runtime state

### Plotting System (output/mimic-plot/)
- **mimic-plot.py**: Central plotting script with comprehensive command-line interface
- **figures/**: Modular plot implementations (6 halo plot types)
  - 5 snapshot plots: halo_mass_function, halo_occupation, spin_distribution, velocity_distribution, spatial_distribution
  - 1 evolution plot: hmf_evolution (halo mass function evolution)
  - **figures/archive/**: 15 galaxy-physics plots archived for potential future use
- **snapshot_redshift_mapper.py**: Handles snapshot-redshift conversions with robust path resolution
- **Features**:
  - Default behavior generates both snapshot and evolution plots
  - Cross-directory execution with automatic path resolution
  - Robust parameter file parsing (handles comments, arrow notation)
  - Consistent flag naming (`--evolution-plots`, `--snapshot-plots`)
- Each plot module follows consistent interface: `plot(halos, volume, metadata, params, output_dir, output_format)`

### Parameter File Structure
Parameter files use a key-value format with sections for:
- File information (FirstFile, LastFile, OutputDir)
- Simulation parameters (BoxSize, Hubble_h, Omega, PartMass)
- Module configuration (EnabledModules, module-specific parameters)
  - Example: `EnabledModules simple_cooling,simple_sfr`
  - Module parameters: `SimpleCooling_BaryonFraction 0.15`

### Tree Processing Flow
1. **load_tree_table()**: Load tree metadata and structure
2. **build_halo_tree()**: Recursively construct halo tracking structures from merger trees
3. **save_halos()**: Write halo properties to output files
4. **free_halos_and_tree()**: Clean up memory

### Memory Management
Uses custom allocator with categorized tracking:
- MEM_HALOS: Halo tracking data structures (Halo structs)
- MEM_TREES: Merger tree data (RawHalo structs)
- MEM_IO: I/O working data and temporary arrays
- MEM_UTILITY: Utility arrays and buffers
- MEM_GALAXIES: Legacy category (deprecated, use MEM_HALOS)
Call `print_allocated()` or `print_allocated_by_category()` to check for memory leaks.

**Key Arrays:**
- `InputTreeHalos[]`: Input halos from merger trees (allocated per tree, freed after processing)
- `FoFWorkspace[]`: Temporary workspace (grows dynamically during FoF processing)
- `ProcessedHalos[]`: Accumulates all processed halos for current tree (written to output)

### Documentation Standards
Follow the documentation template in `docs/developer/coding-standards.md`:
- Function headers with @brief, @param, @return
- File headers explaining purpose and key functions
- Inline comments for complex calculations
- Units explicitly stated for physical quantities

See also:
- **docs/architecture/vision.md**: Architectural principles and future vision
- **docs/developer/getting-started.md**: Developer setup and workflow
- **docs/developer/testing.md**: Comprehensive testing guide and standards
- **docs/user/module-configuration.md**: Guide to configuring physics modules
- **docs/architecture/roadmap_v3.md**: Development roadmap and phase implementation details

## Development Guidelines
- All work to highest professional coding standards
- Documentation-as-you-go always
- When running mimic always check the exit code for success or failure
- Never simplify tests - failing tests indicate real problems
- Ask before committing to git
- Commit messages must be meaningful and list every changed file, grouped logically, with a brief reason for each
- When asked to write to obsidian, use `obsidian-inbox/`
- Never delete files! Archive to `archive/` instead