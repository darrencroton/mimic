# Getting Started with Mimic

Mimic is a **physics-agnostic galaxy evolution framework** with runtime-configurable physics modules. This modular architecture enables researchers to experiment with different physics combinations without recompilation.

---

## Quick Navigation

**Choose your path:**

- **[First Time Setup](#first-time-setup)** - Clone and build Mimic
- **[For Users](#for-users)** - Run simulations and configure physics
- **[For Developers](#for-developers)** - Add features and physics modules
- **[For AI Coders](#for-ai-coders)** - Essential context and documentation map
- **[Testing](#testing)** - Verify your changes

---

## First Time Setup

### Prerequisites

- C compiler (gcc/clang)
- Python 3.6+ (for testing and code generation)
- Optional: HDF5 library, MPI

### Quick Setup

```bash
# Clone and setup (creates directories, downloads data, sets up Python environment)
./scripts/first_run.sh

# Build Mimic
make

# Run a test simulation
./mimic input/millennium.par

# Verify success (should output 0)
echo $?
```

---

## For Users

### Running Simulations

**Basic execution:**
```bash
./mimic input/millennium.par
```

**Command-line options:**
```bash
./mimic --verbose input/millennium.par  # Detailed output
./mimic --quiet input/millennium.par    # Minimal output
./mimic --skip input/millennium.par     # Skip certain operations
```

### Configuring Physics Modules

Mimic's physics is configured at runtime via the `.par` file. No recompilation needed.

**Example configuration:**
```
# Select modules to run (order matters for dependencies)
EnabledModules  sage_infall,sage_cooling

# Configure module parameters
SageInfall_BaryonFrac  0.17
SageInfall_ReionizationOn  1
SageCooling_CoolFunctionsDir  input/CoolFunctions
```

**Documentation:**
- **[Module Configuration Guide](user/module-configuration.md)** - All available modules and parameters
- **[Output Formats](user/output-formats.md)** - Binary and HDF5 output specifications

### Plotting Results

```bash
# Activate plotting environment
source mimic_venv/bin/activate

# Generate all plots
cd output/mimic-plot
python mimic-plot.py --param-file=../../input/millennium.par

# Generate specific plots
python mimic-plot.py --param-file=../../input/millennium.par --plots=halo_mass_function

# Deactivate when done
deactivate
```

---

## For Developers

### Core Concepts

Mimic uses a **metadata-driven architecture** where properties and modules are defined in YAML files and code is auto-generated.

**Key Systems:**
1. **Property Metadata** (`metadata/properties/*.yaml`) - Defines galaxy/halo properties
2. **Module Metadata** (`src/modules/*/module_info.yaml`) - Defines physics modules
3. **Auto-Generation** - Code generated from metadata via `make generate`

### Development Workflow

**1. Making Code Changes:**
```bash
# Edit source files
vim src/core/model_building.c

# Format code
./scripts/beautify.sh

# Build and test
make clean && make
./mimic input/millennium.par
```

**2. Adding a New Property:**
```bash
# Edit metadata
vim metadata/properties/galaxy_properties.yaml

# Add your property definition:
# - name: MyNewProperty
#   type: float
#   output: true
#   units: "Msun"
#   description: "My new property"

# Regenerate code
make generate

# Build and test
make clean && make
```

**3. Adding a New Module:**
```bash
# Copy module template
cp -r src/modules/_template src/modules/my_module

# Implement module (my_module.c, my_module.h)
# Create module_info.yaml from template

# Auto-register module
make generate-modules

# Build and test
make clean && make
```

### Essential Developer Documentation

| Document | Purpose |
|----------|---------|
| **[Architecture Vision](architecture/vision.md)** | 8 core architectural principles |
| **[Coding Standards](developer/coding-standards.md)** | Code style and documentation requirements |
| **[Testing Guide](developer/testing.md)** | Writing and running tests |
| **[Module Developer Guide](developer/module-developer-guide.md)** | Creating physics modules |
| **[Property Metadata Schema](architecture/property-metadata-schema.md)** | Property definition reference |
| **[Module Metadata Schema](developer/module-metadata-schema.md)** | Module definition reference |
| **[Execution Flow Reference](developer/execution-flow-reference.md)** | Complete function call trace |

### Testing Your Changes

```bash
# Run all tests
make tests

# Run specific test tiers
make test-unit          # C unit tests (fast, <10s)
make test-integration   # Python integration tests (<1min)
make test-scientific    # Python scientific validation (<5min)
```

**See [Testing Guide](developer/testing.md) for comprehensive testing documentation.**

---

## For AI Coders

### Essential Context

**What is Mimic?**
- Physics-agnostic galaxy evolution framework
- Runtime-configurable physics modules (no recompilation needed)
- Metadata-driven architecture (YAML → auto-generated C code)
- Comprehensive testing (unit + integration + scientific validation)

**Core Principles:**
1. **Physics-Agnostic Core** - Core has zero knowledge of specific physics
2. **Runtime Modularity** - Configure modules via `.par` files
3. **Metadata-Driven** - Single source of truth in YAML files
4. **Single Source of Truth** - No manual synchronization
5. **Unified Processing Model** - Consistent module lifecycle

**See [Architecture Vision](architecture/vision.md) for complete principles.**

### Code Organization

```
mimic/
├── src/
│   ├── core/          # Core execution (physics-agnostic)
│   ├── io/            # Input/output (tree readers, writers)
│   ├── util/          # Utilities (memory, error, numeric)
│   ├── modules/       # Physics modules (modular, hot-swappable)
│   └── include/       # Headers + auto-generated code
├── metadata/
│   └── properties/    # Property definitions (YAML)
├── tests/             # Three-tier testing (unit/integration/scientific)
├── docs/              # Documentation (architecture/developer/user)
├── scripts/           # Code generation and development tools
└── output/            # Simulation output and plotting
```

### Key Documentation Map

**Architecture & Design:**
- `docs/architecture/vision.md` - 8 core principles
- `docs/architecture/roadmap_v4.md` - Implementation status and roadmap

**Development:**
- `docs/developer/module-developer-guide.md` - Creating physics modules
- `docs/developer/testing.md` - Testing framework and standards
- `docs/developer/coding-standards.md` - Code style requirements

**Reference:**
- `docs/developer/module-metadata-schema.md` - Module YAML specification
- `docs/architecture/property-metadata-schema.md` - Property YAML specification
- `docs/developer/execution-flow-reference.md` - Function call trace

**User Guides:**
- `docs/user/module-configuration.md` - Configuring physics modules
- `docs/user/output-formats.md` - Output file specifications

### Common AI Coding Tasks

**Task: Implement a new physics module**
→ See `docs/developer/module-developer-guide.md` (complete workflow)

**Task: Add a new property**
→ Edit `metadata/properties/*.yaml`, run `make generate`

**Task: Fix a bug**
→ Ensure tests pass: `make tests`

**Task: Understand code flow**
→ See `docs/developer/execution-flow-reference.md`

**Task: Add tests**
→ See `docs/developer/testing.md`

### Build System

```bash
# Standard build
make

# With HDF5 support
make USE-HDF5=yes

# With MPI support
make USE-MPI=yes

# Regenerate code from metadata
make generate          # Both properties and modules
make generate-modules  # Modules only
make validate-modules  # Validate module metadata

# Verify generated code is current (CI check)
make check-generated

# Clean
make clean  # Remove all build artifacts
make tidy   # Remove object files, keep executable
```

### Important Files to Check

**Before making changes:**
- `CLAUDE.md` - Project-specific instructions for Claude Code
- `docs/architecture/vision.md` - Architectural principles

**When adding modules:**
- `src/modules/_template/` - Module template
- `docs/developer/module-metadata-schema.md` - Module metadata spec

**When adding properties:**
- `metadata/properties/galaxy_properties.yaml` - Galaxy properties
- `metadata/properties/halo_properties.yaml` - Halo properties
- `docs/architecture/property-metadata-schema.md` - Property schema

**When writing tests:**
- `docs/developer/testing.md` - Testing guide
- `tests/framework/` - Test templates

---

## Testing

### Quick Test

```bash
# Build and run
make clean && make
./mimic input/millennium.par

# Check exit code (should be 0)
echo $?
```

### Comprehensive Testing

```bash
# Run all test tiers
make tests

# Individual tiers
make test-unit          # Fast component tests (<10s)
make test-integration   # End-to-end pipeline tests (<1min)
make test-scientific    # Physics validation (<5min)
```

**See [Testing Guide](developer/testing.md) for detailed testing documentation.**

---

## Getting Help

**Documentation:**
- Architecture questions → [vision.md](architecture/vision.md)
- Implementation status → [roadmap_v4.md](architecture/roadmap_v4.md)
- Code flow questions → [execution-flow-reference.md](developer/execution-flow-reference.md)
- Module configuration → [module-configuration.md](user/module-configuration.md)
- Testing → [testing.md](developer/testing.md)

**Development:**
- All code to highest professional standards
- Document as you go
- Never delete files - archive to `ignore/` instead
- Ask before committing
- Commit messages must be meaningful and detailed

---

## Directory Structure Reference

```
mimic/
├── CLAUDE.md              # Claude Code instructions
├── README.md              # Project overview
├── Makefile               # Build system
├── src/                   # Source code
│   ├── core/              # Core execution (main, init, model building)
│   ├── io/                # Input/output (tree readers, writers)
│   ├── util/              # Utilities (memory, error, numeric)
│   ├── modules/           # Physics modules
│   │   ├── _template/     # Module template
│   │   ├── sage_infall/   # SAGE infall module
│   │   └── test_fixture/  # Testing infrastructure module
│   └── include/           # Headers
│       └── generated/     # Auto-generated code
├── metadata/
│   └── properties/        # Property metadata (YAML)
├── tests/                 # Testing framework
│   ├── unit/              # C unit tests
│   ├── integration/       # Python integration tests
│   ├── scientific/        # Physics validation tests
│   └── framework/         # Test utilities and templates
├── docs/                  # Documentation
│   ├── architecture/      # Design and principles
│   ├── developer/         # Developer guides
│   ├── user/              # User guides
│   └── physics/           # Physics module documentation
├── scripts/               # Development tools
├── input/                 # Input files and data
├── output/                # Simulation output
│   └── mimic-plot/        # Plotting system
├── ignore/                # Archived and temporary files
└── mimic_venv/            # Python virtual environment (created by first_run.sh)
```

---

## What's Next?

**For Users:**
1. Run your first simulation: `./mimic input/millennium.par`
2. Explore module configuration: [module-configuration.md](user/module-configuration.md)
3. Generate plots: `cd output/mimic-plot && python mimic-plot.py`

**For Developers:**
1. Read the [Architecture Vision](architecture/vision.md)
2. Review the [Roadmap](architecture/roadmap_v4.md) for current status
3. Follow the [Module Developer Guide](developer/module-developer-guide.md) to add features

**For AI Coders:**
1. Review `CLAUDE.md` for project-specific instructions
2. Understand [Architecture Vision](architecture/vision.md) principles
3. Check [Roadmap](architecture/roadmap_v4.md) for current implementation status
4. Use [Testing Guide](developer/testing.md) for all code changes
