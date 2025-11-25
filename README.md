# Mimic: Physics-Agnostic Galaxy Evolution Framework

**Mimic** is a modular framework for galaxy evolution modeling with runtime-configurable physics modules and clean separation between infrastructure and scientific implementations. Built on a robust halo tracking core, it enables flexible experimentation with different physics combinations without recompilation.

## Key Features

- **Physics-Agnostic Core**: Infrastructure independent of specific physics implementations
- **Runtime Modularity**: Configure module selection via parameters, not compile-time flags
- **Robust Memory Management**: Bounded, predictable memory usage for large cosmological simulations
- **Metadata-Driven Type Safety**: Auto-generated code from YAML with compile-time validation
- **Multi-Format I/O**: Unified interfaces for binary and HDF5 input/output
- **Integrated Visualization**: Comprehensive plotting system for halo properties and distributions

See [docs/architecture/vision.md](docs/architecture/vision.md) for the complete architectural vision.

## Getting Started

### Requirements

- C compiler (gcc or compatible) and GNU Make
- Python 3.x with numpy, matplotlib, pyyaml (for plotting)
- Optional: HDF5 libraries, MPI, clang-format, black/isort

### Automated Setup

```bash
# Clone and set up in one go
git clone [repository-url]
cd mimic
./scripts/first_run.sh  # Creates dirs, downloads data, sets up mimic_venv

# Compile and run
make
./mimic input/millennium.yaml

# Generate plots
source mimic_venv/bin/activate
python output/mimic-plot/mimic-plot.py --param-file=input/millennium.yaml
deactivate
```

### Manual Setup

If automated setup fails:

```bash
# 1. Create directories
mkdir -p input/data/millennium output/results/millennium

# 2. Download mini-Millennium simulation
cd input/data/millennium
wget "https://www.dropbox.com/s/l5ukpo7ar3rgxo4/mini-millennium-treefiles.tar?dl=0" -O mini-millennium-treefiles.tar
tar -xf mini-millennium-treefiles.tar && rm mini-millennium-treefiles.tar
cd ../../..

# 3. Set up Python environment
python3 -m venv mimic_venv
source mimic_venv/bin/activate
pip install -r requirements.txt
deactivate

# 4. Update input/millennium.yaml with absolute paths for:
#    - output.directory, input.simulation_dir, input.snapshot_list_file
```

### Build Options

```bash
make                      # Basic compilation
make USE-HDF5=yes        # With HDF5 support
make USE-MPI=yes         # With MPI support
make generate            # Regenerate code from YAML metadata
make check-generated     # Verify generated code is current (CI)
make clean               # Remove all build artifacts
```

## Usage

### Running Simulations

```bash
./mimic <parameter_file>           # Basic execution
./mimic --verbose <parameter_file> # Add context (timestamp, file:line) to messages
./mimic --quiet <parameter_file>   # Warnings/errors only
./mimic --skip <parameter_file>    # Skip existing output
```

### Parameter File Structure

Configuration uses YAML format with four main sections:

```yaml
output:
  directory: /path/to/output/
  format: binary              # or 'hdf5'
  snapshot_list: [63, 37, 32, 27, 23, 20, 18, 16]

input:
  tree_type: lhalo_binary     # or 'genesis_lhalo_hdf5'
  simulation_dir: /path/to/trees/
  first_file: 0
  last_file: 7

simulation:
  cosmology:
    omega_matter: 0.25
    omega_lambda: 0.75
    hubble_h: 0.73
  box_size: 62.5              # Mpc/h
  particle_mass: 0.0860657    # 10^10 Msun/h

units:
  length_in_cm: 3.08568e+24   # Mpc/h
  mass_in_g: 1.989e+43        # 10^10 Msun
```

See `input/millennium.yaml` for complete parameter documentation.

## Architecture

Mimic uses a modular structure with metadata-driven code generation:

- **src/core/**: Main execution and halo tracking infrastructure
- **src/io/**: Multi-format tree readers (binary, HDF5) and output writers
- **src/modules/**: Runtime-configurable physics modules
- **src/util/**: Memory management, error handling, numeric utilities
- **metadata/**: YAML property definitions (auto-generates C structs and Python dtypes)

Key design patterns: three-tier halo architecture (input → processing → output), category-tracked memory allocation, format-agnostic I/O, and physics-agnostic core with runtime module selection.

See [docs/architecture/vision.md](docs/architecture/vision.md) for detailed design principles.

## Visualization

The plotting system (`output/mimic-plot/`) generates 6 halo property analyses including mass functions, occupation statistics, spin/velocity distributions, and spatial distributions:

```bash
source mimic_venv/bin/activate

# Generate all plots (snapshot + evolution)
python output/mimic-plot/mimic-plot.py --param-file=input/millennium.yaml

# Generate specific types
python output/mimic-plot/mimic-plot.py --param-file=input/millennium.yaml --snapshot-plots
python output/mimic-plot/mimic-plot.py --param-file=input/millennium.yaml --plots=halo_mass_function,spin_distribution
```

See [output/mimic-plot/README.md](output/mimic-plot/README.md) for details.

## Development Tools

### Code Formatting
```bash
./scripts/beautify.sh           # Format all C and Python code
./scripts/beautify.sh --c-only  # C code only (requires clang-format)
./scripts/beautify.sh --py-only # Python only (requires black/isort)
```

### Performance Benchmarking
```bash
cd scripts
./benchmark_mimic.sh                    # Default benchmark
./benchmark_mimic.sh custom.yaml        # Custom parameter file
./benchmark_mimic.sh --verbose          # Detailed output
```

Results saved to `benchmarks/` (gitignored) in JSON format.

## Testing

```bash
make tests              # Run all test tiers (validates metadata first)
make test-unit          # C unit tests (fast, <10s)
make test-integration   # Python integration tests (<1min)
make test-scientific    # Scientific validation tests (<5min)
```

Test data uses the mini-Millennium simulation ([Springel et al. 2005](http://arxiv.org/abs/astro-ph/0504097)), automatically downloaded by `scripts/first_run.sh`.

## Documentation

- **[docs/architecture/vision.md](docs/architecture/vision.md)**: Design principles and architectural philosophy
- **[docs/developer/](docs/developer/)**: Development guides, coding standards, testing framework
- **Inline code documentation**: Comprehensive docstrings following professional standards

## Contributing

Contributions welcome! File issues for bugs, suggest features for new physics modules, or submit pull requests following the coding standards in [docs/developer/coding-standards.md](docs/developer/coding-standards.md). Document as you code, test thoroughly, and align with the architectural vision.

## Historical Context

Mimic builds upon the **Semi-Analytic Galaxy Evolution (SAGE)** model by Croton et al. ([2016](http://arxiv.org/abs/1601.04709), [2006](http://arxiv.org/abs/astro-ph/0508046)). The current codebase represents a refactored foundation focusing on physics-agnostic infrastructure with runtime-configurable modules.

**Related SAGE Resources**: [Original SAGE code](https://github.com/darrencroton/sage) | [ASCL](http://ascl.net/1601.006) | [TAO Platform](https://tao.asvo.org.au/) | [![DOI](https://zenodo.org/badge/13542/darrencroton/sage.svg)](https://zenodo.org/badge/latestdoi/13542/darrencroton/sage)

## License

Mimic is available under an open-source license. See the LICENSE file for details.

## Contact

Questions and comments can be sent to Darren Croton: dcroton@swin.edu.au

Visit [darrencroton.github.io](https://darrencroton.github.io) for more information.
