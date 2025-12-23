# Mimic: Physics-Agnostic Galaxy Evolution Framework

```
    __  ___  ____  __  ___  ____  ______
   /  |/  / /  _/ /  |/  / /  _/ / ____/
  / /|_/ /  / /  / /|_/ /  / /  / /
 / /  / / _/ /  / /  / / _/ /  / /___
/_/  /_/ /___/ /_/  /_/ /___/  \____/
```

**Mimic** is a physics-agnostic galaxy evolution framework with runtime-configurable physics modules. Built on robust metadata-driven architecture, it enables flexible experimentation with different physics combinations without recompilation—making it ideal for exploring galaxy formation models and testing alternative physics implementations.

## What Makes Mimic Different

- **Runtime Physics Configuration**: Select and configure physics modules via YAML files—no recompilation needed
- **Physics-Agnostic Core**: Infrastructure completely independent of specific physics implementations
- **Metadata-Driven Architecture**: Properties and modules defined once in YAML, auto-generated into type-safe C code
- **Robust Testing Framework**: Three-tier testing (unit, integration, scientific) ensures reliability
- **Multi-Format I/O**: Binary and HDF5 output with self-documenting metadata
- **Production-Ready**: Complete SAGE physics implementation with comprehensive validation

**Scientific Heritage**: Mimic builds upon the SAGE (Semi-Analytic Galaxy Evolution) model ([Croton et al. 2016](http://arxiv.org/abs/1601.04709), [2006](http://arxiv.org/abs/astro-ph/0508046)) while providing a modern, extensible foundation for galaxy evolution modeling.

## Quick Start

### Prerequisites

- C compiler (gcc or compatible) and GNU Make
- Python 3.6+ with numpy, matplotlib, pyyaml
- Optional: HDF5 libraries, MPI, clang-format, black/isort

### Build and Run

```bash
# Clone and setup
git clone [repository-url]
cd mimic
./scripts/first_run.sh        # Creates directories, downloads test data, sets up Python environment

# Build and run test simulation
make
./mimic input/millennium.yaml

# Verify success (should output: 0)
echo $?
```

**That's it!** You've just run Mimic on the mini-Millennium simulation.

### Generate Plots

```bash
# Activate Python environment
source mimic_venv/bin/activate

# Generate all plots
python output/mimic-plot/mimic-plot.py --param-file=input/millennium.yaml

# Deactivate when done
deactivate
```

## Build Options

```bash
make                   # Standard build (HDF5 enabled by default)
make USE-HDF5=no       # Build without HDF5 support (binary output only)
make USE-MPI=yes       # With MPI parallelization
make -j$(nproc)        # Parallel compilation (faster)
make clean             # Clean all build artifacts
make tests             # Run comprehensive test suite
```

## Where to Go Next

### For Users (Running Simulations)

→ **[docs/USER-GUIDE.md](docs/USER-GUIDE.md)** - Complete guide to installation, configuration, running simulations, and analyzing output

**Key Topics**:
- Detailed installation instructions
- Configuring physics modules
- Multi-phase pipeline configuration
- Output formats (binary and HDF5)
- Plotting and visualization
- Troubleshooting

### For Developers (Extending Mimic)

→ **[docs/DEVELOPER-GUIDE.md](docs/DEVELOPER-GUIDE.md)** - Complete guide to architecture, module development, and testing

**Key Topics**:
- Architecture overview and core principles
- Creating new physics modules
- Property system and metadata schemas
- Testing framework and standards
- Development workflow
- Output format specifications
- Configuration file reference
- API reference
- Code standards

### For Understanding Design Philosophy

→ **[docs/VISION.md](docs/VISION.md)** - Architectural principles and design philosophy

**Key Topics**:
- 8 core architectural principles
- Implementation philosophy
- Design decisions and rationale
- Future direction

## Documentation

**Complete documentation** is available in the [docs/](docs/) directory:

- **USER-GUIDE.md** - Using Mimic (installation, configuration, running simulations)
- **DEVELOPER-GUIDE.md** - Extending Mimic (architecture, modules, testing, schemas, formats, API)
- **VISION.md** - Design philosophy and architectural principles

## Testing

Mimic includes a comprehensive three-tier testing framework:

```bash
make tests              # Run all tests (validates metadata, runs all tiers)
make test-unit          # C unit tests
make test-integration   # Python integration tests
make test-scientific    # Physics validation tests
```

Test data uses the mini-Millennium simulation ([Springel et al. 2005](http://arxiv.org/abs/astro-ph/0504097)), automatically downloaded during first-time setup.

## Contributing

Contributions are welcome! When contributing:

1. Follow the coding standards in [docs/DEVELOPER-GUIDE.md](docs/DEVELOPER-GUIDE.md)
2. Align with architectural principles in [docs/VISION.md](docs/VISION.md)
3. Write comprehensive tests for new features
4. Update relevant documentation

## Citation

If you use Mimic (SAGE) in your research, please cite the SAGE paper:

- Croton et al. 2016: [Semi-Analytic Galaxy Evolution (SAGE): Model Calibration and Basic Results](http://arxiv.org/abs/1601.04709)

**Related SAGE Resources**:
- [Original SAGE code](https://github.com/darrencroton/sage)
- [ASCL Entry](http://ascl.net/1601.006)
- [TAO Platform](https://tao.asvo.org.au/)
- [![DOI](https://zenodo.org/badge/13542/darrencroton/sage.svg)](https://zenodo.org/badge/latestdoi/13542/darrencroton/sage)

## License

Mimic is available under an open-source license. See LICENSE file for details.

## Contact

**Darren Croton**
Email: dcroton@swin.edu.au
Web: [darrencroton.github.io](https://darrencroton.github.io)

---

**Architecture Note**: Mimic is designed around 8 core principles including physics-agnostic infrastructure, runtime modularity, and metadata-driven development. See [docs/VISION.md](docs/VISION.md) for the complete architectural vision.
