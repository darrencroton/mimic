# Mimic: Physics-Agnostic Galaxy Evolution Framework

**Mimic** is a physics-agnostic core framework for galaxy evolution modeling, designed around runtime-configurable physics modules and a clean separation between infrastructure and scientific implementations. The framework emphasizes scientific flexibility, maintainability, and extensibility.

## Vision

Mimic embodies a **physics-agnostic architecture** where the core infrastructure has zero knowledge of specific physics implementations. Physics modules are pure add-ons that extend core functionality through well-defined interfaces, enabling:

- **Scientific Flexibility**: Experiment with different physics combinations without recompilation
- **Independent Development**: Core infrastructure and physics modules evolve independently
- **Long-term Maintainability**: Clean separation of concerns reduces complexity
- **Extensibility**: Add new physics modules following clear, documented patterns

See [docs/architecture/vision.md](docs/architecture/vision.md) for the complete architectural vision and design principles.

## Historical Context

Mimic builds upon the **Semi-Analytic Galaxy Evolution (SAGE)** model developed by Croton et al. The current codebase represents a refactored foundation focusing on halo tracking infrastructure, with physics modules planned for future implementation following the modular architecture outlined in the vision document.

### Related SAGE Resources

- **Original SAGE**: [github.com/darrencroton/sage](https://github.com/darrencroton/sage) - Full semi-analytic galaxy formation model
- **Publications**: [Croton et al. (2016)](http://arxiv.org/abs/1601.04709), [Croton et al. (2006)](http://arxiv.org/abs/astro-ph/0508046)
- **ASCL Listing**: [ascl.net/1601.006](http://ascl.net/1601.006)
- **TAO Platform**: [tao.asvo.org.au](https://tao.asvo.org.au/) - Hosts pre-computed SAGE models
- **DOI**: [![DOI](https://zenodo.org/badge/13542/darrencroton/sage.svg)](https://zenodo.org/badge/latestdoi/13542/darrencroton/sage)

## Features

- **Physics-Agnostic Core**: Infrastructure independent of specific physics implementations
- **Runtime Modularity**: Module selection via configuration, not compile-time flags
- **Flexible Input**: Multiple N-body simulation formats (binary, HDF5)
- **Robust Memory Management**: Bounded, predictable memory usage optimized for large simulations
- **Format-Agnostic I/O**: Unified interfaces supporting multiple input/output formats
- **Type Safety**: Metadata-driven code generation with compile-time validation
- **Computational Efficiency**: Processes large cosmological simulations efficiently
- **Comprehensive Testing**: Multi-level test framework for scientific validation
- **Extensive Documentation**: Professional coding standards with detailed documentation
- **Integrated Visualization**: Dedicated plotting system for halo properties and distributions

## Quick Start

```bash
# Clone the repository
git clone [repository-url]
cd mimic

# Run automated setup (creates directories, downloads data, sets up Python environment)
./scripts/first_run.sh

# Compile Mimic
make

# Run with the mini-Millennium simulation
./mimic input/millennium.par

# Generate plots (using virtual environment)
source mimic_venv/bin/activate
cd output/mimic-plot
python mimic-plot.py --param-file=../../input/millennium.par
deactivate
```

The `scripts/first_run.sh` script automatically:
- Creates necessary directories
- Downloads the mini-Millennium simulation trees
- Sets up Python virtual environment (`mimic_venv`) with plotting dependencies
- Configures paths in parameter files

## Installation

### Requirements

- C compiler (gcc or compatible)
- GNU Make
- Python 3.x (for plotting)
- (Optional) HDF5 libraries for HDF5 format support
- (Optional) MPI for parallel processing
- (Optional) clang-format for code formatting
- (Optional) black and isort for Python formatting

### Building Mimic

```bash
# Basic compilation
make

# With HDF5 support (recommended)
make USE-HDF5=yes

# With MPI support for parallel processing
make USE-MPI=yes

# Clean build artifacts
make clean

# Remove build directory only (keep executable)
make tidy
```

Build artifacts are organized in the `build/` directory.

### Manual Setup

If the automated script doesn't work for your system:

#### 1. Create Directory Structure
```bash
mkdir -p input/data/millennium
mkdir -p output/results/millennium
```

#### 2. Download Simulation Data
```bash
cd input/data/millennium

# Using wget
wget "https://www.dropbox.com/s/l5ukpo7ar3rgxo4/mini-millennium-treefiles.tar?dl=0" -O mini-millennium-treefiles.tar

# Or using curl
curl -L -o mini-millennium-treefiles.tar "https://www.dropbox.com/s/l5ukpo7ar3rgxo4/mini-millennium-treefiles.tar?dl=0"

# Extract
tar -xf mini-millennium-treefiles.tar
rm mini-millennium-treefiles.tar
cd ../../..
```

#### 3. Set Up Python Environment
```bash
# Recommended: Use virtual environment
python3 -m venv mimic_venv
source mimic_venv/bin/activate
pip install -r requirements.txt
```

#### 4. Configure Parameter File
Update `input/millennium.par` with absolute paths for:
- `OutputDir`
- `SimulationDir`
- `FileWithSnapList`

## Basic Usage

### Running Simulations

```bash
# Basic execution
./mimic <parameter_file>

# With command-line options
./mimic --verbose <parameter_file>  # Show debug messages
./mimic --quiet <parameter_file>    # Show only warnings/errors
./mimic --skip <parameter_file>     # Skip existing output files
./mimic --help                      # Display help
```

### Parameter File Structure

```
%------------------------------------------
%----- Mimic output file information -----
%------------------------------------------

OutputFileBaseName     model
OutputDir              /path/to/output/directory/

FirstFile              0
LastFile               7

%------------------------------------------
%----- Snapshot output list ---------------
%------------------------------------------

NumOutputs             8

% List output snapshots after arrow, highest to lowest
-> 63 37 32 27 23 20 18 16

%------------------------------------------
%----- Simulation information -------------
%------------------------------------------

TreeName               trees_063
TreeType               lhalo_binary  % or 'genesis_lhalo_hdf5'
OutputFormat           binary        % or 'hdf5'
SimulationDir          /path/to/simulation/data/
FileWithSnapList       /path/to/snapshot/list
LastSnapshotNr         63
BoxSize                62.5  % Mpc/h

Omega                  0.25
OmegaLambda            0.75
Hubble_h               0.73
PartMass               0.0860657
```

See `input/millennium.par` for a complete example.

## Code Architecture

Mimic follows a hierarchical structure under `src/`:

### Directory Structure
- **src/core/**: Core execution (main, initialization, halo tracking)
- **src/io/**: Input/output operations
  - **tree/**: Tree readers (interface, binary, HDF5 formats)
  - **output/**: Output writers (binary, HDF5, utilities)
- **src/util/**: Utility functions (memory, error, numeric, version)
- **src/modules/**: Physics modules (halo properties with virial calculations)
- **src/include/**: Public headers (types, globals, constants, config)

### Key Design Patterns

1. **Three-Tier Halo Architecture**: Separation between input (InputTreeHalos), processing (FoFWorkspace), and storage (ProcessedHalos)
2. **Memory Categories**: Tracked allocation by category (halos, trees, parameters, I/O)
3. **Error Propagation**: Consistent error handling with context preservation
4. **Format Abstraction**: I/O operations support multiple tree and output formats
5. **State Management**: Single source of truth via globals for runtime state

### Data Structures

- **RawHalo**: Immutable merger tree input data
- **Halo**: Mutable halo tracking structure (24 fields)
- **HaloOutput**: Output format structure (24 fields)
- **HaloAuxData**: Auxiliary processing metadata
- **MimicConfig**: Configuration parameters

## Code Formatting

```bash
# Format all code (C and Python)
./scripts/beautify.sh

# Format only C code
./scripts/beautify.sh --c-only

# Format only Python code
./scripts/beautify.sh --py-only
```

## Visualization System

The **mimic-plot** system (`output/mimic-plot/`) provides comprehensive halo property analysis:

- **6 halo plot types**: Mass functions, occupation statistics, spin/velocity distributions, spatial distributions
- **Snapshot & evolution plots**: Default generates both automatically
- **Cross-directory execution**: Robust path resolution from any location
- **Modular design**: Clean interfaces, consistent styling
- **Single entry point**: Minimal configuration required

Basic usage:
```bash
# Activate virtual environment
source mimic_venv/bin/activate

# Generate all plots (both snapshot and evolution)
python output/mimic-plot/mimic-plot.py --param-file=input/millennium.par

# Generate only snapshot plots (5 plots)
python output/mimic-plot/mimic-plot.py --param-file=input/millennium.par --snapshot-plots

# Generate only evolution plots (1 plot)
python output/mimic-plot/mimic-plot.py --param-file=input/millennium.par --evolution-plots

# Generate specific plots
python output/mimic-plot/mimic-plot.py --param-file=input/millennium.par --plots=halo_mass_function,spin_distribution

# Deactivate when done
deactivate
```

See [output/mimic-plot/README.md](output/mimic-plot/README.md) for detailed usage.

## Sample Data

The mini-Millennium Simulation ([Springel et al. 2005](http://arxiv.org/abs/astro-ph/0504097)) is used for testing. Tree files are automatically downloaded by `scripts/first_run.sh`, or manually from [here](https://data-portal.hpc.swin.edu.au/dataset/mini-millennium-simulation).

## Documentation

- **Architecture Vision**: [docs/architecture/vision.md](docs/architecture/vision.md) - Design principles and philosophy
- **Developer Guide**: [docs/developer/](docs/developer/) - Coding standards and getting started
- **Code Documentation**: Comprehensive inline documentation following professional standards

## Contributing

Contributions are welcome! Please consider:

1. **Bug Reports**: File issues for bugs or unexpected behavior
2. **Feature Requests**: Suggest enhancements or new physics modules
3. **Pull Requests**: Submit improvements following coding standards
4. **Documentation**: Help improve documentation and examples

## Development Guidelines

- Follow professional coding standards (see [docs/developer/coding-standards.md](docs/developer/coding-standards.md))
- Document as you code
- Test thoroughly before submitting
- Align with architectural vision
- Never delete files - archive to `archive/` directory

## License

Mimic is available under an open-source license. See the LICENSE file for details.

## Contact

Questions and comments can be sent to Darren Croton: dcroton@swin.edu.au

Visit [darrencroton.github.io](https://darrencroton.github.io) for more information.
