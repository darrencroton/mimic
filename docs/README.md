# Mimic Documentation

**Mimic** is a **physics-agnostic galaxy evolution framework** with runtime-configurable physics modules. This modular architecture enables researchers to experiment with different physics combinations without recompilation.

---

## Quick Start

**First time here?** Choose your path:

| I want to... | Go to... |
|--------------|----------|
| **Run simulations** | [For Users](#for-users) below |
| **Develop features or modules** | [For Developers](#for-developers) below |
| **Get AI coding context** | [For AI Coders](#for-ai-coders) below |
| **Understand the architecture** | [Architecture & Design](#architecture--design) |
| **Find specific documentation** | [Documentation by Topic](#documentation-by-topic) |

---

## First Time Setup

### Prerequisites

- C compiler (gcc/clang)
- Python 3.6+ (for testing and code generation)
- Optional: HDF5 library, MPI

### Build and Run

```bash
# Clone and setup (creates directories, downloads data, sets up Python environment)
./scripts/first_run.sh

# Build Mimic
make

# Run a test simulation
./mimic input/millennium.yaml

# Verify success (should output 0)
echo $?
```

**That's it!** You're ready to use Mimic.

---

## For Users

### Running Simulations

**Basic execution:**
```bash
./mimic input/millennium.yaml
```

**Command-line options:**
```bash
./mimic --verbose input/millennium.yaml  # Detailed output
./mimic --quiet input/millennium.yaml    # Minimal output
./mimic --skip input/millennium.yaml     # Skip certain operations
```

### Configuring Physics Modules

Mimic's physics is configured at runtime via YAML files. **No recompilation needed.**

**Example configuration:**
```yaml
modules:
  enabled:
  - infall_model
  - cooling_model
  parameters:
    InflallModel:
      BaryonFrac: 0.17
      ReionizationOn: 1
    CoolingModel:
      CoolFunctionsDir: input/CoolFunctions
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
python mimic-plot.py --param-file=../../input/millennium.yaml

# Deactivate when done
deactivate
```

---

## For Developers

### Core Concepts

Mimic uses a **metadata-driven architecture** where properties and modules are defined in YAML files and code is auto-generated.

**Key Systems:**
1. **Property Metadata** - Defines galaxy/halo properties
   - Halo properties: `src/core/halo_properties.yaml`
   - Galaxy properties: `src/modules/galaxy_properties.yaml`
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
./mimic input/millennium.yaml
```

**2. Adding a New Property:**
```bash
# Edit metadata
vim src/modules/galaxy_properties.yaml

# Add your property definition (see property-metadata-schema.md)
# Regenerate code
make generate

# Build and test
make clean && make
```

**3. Adding a New Module:**
```bash
# Copy module template
cp -r src/modules/_system/template src/modules/my_module

# Implement module (my_module.c, my_module.h)
# Create module_info.yaml from template

# Auto-register module
make generate

# Build and test
make clean && make
```

### Essential Developer Documentation

| Document | Purpose | Lines |
|----------|---------|-------|
| **[Architecture Vision](architecture/vision.md)** | 8 core architectural principles | 227 |
| **[Module Developer Guide](developer/module-developer-guide.md)** | Creating physics modules | 1122 |
| **[Testing Guide](developer/testing.md)** | Three-tier testing framework | 2127 |
| **[Module Metadata Schema](developer/module-metadata-schema.md)** | Module YAML specification | 1237 |
| **[Property Metadata Schema](architecture/property-metadata-schema.md)** | Property YAML specification | 990 |
| **[Execution Flow Reference](developer/execution-flow-reference.md)** | Complete function call trace | 1028 |
| **[Coding Standards](developer/coding-standards.md)** | Code style requirements | 71 |

**All reference docs include quick-start summaries at the top.**

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
2. **Runtime Modularity** - Configure modules via YAML files
3. **Metadata-Driven** - Single source of truth in YAML files
4. **Single Source of Truth** - No manual synchronization
5. **Unified Processing Model** - Consistent module lifecycle

**See [Architecture Vision](architecture/vision.md) for complete principles.**

### Code Organization

```
mimic/
├── src/
│   ├── core/          # Core execution (physics-agnostic)
│   │   └── halo_properties.yaml    # Halo property metadata
│   ├── io/            # Input/output (tree readers, writers)
│   ├── util/          # Utilities (memory, error, numeric)
│   ├── modules/       # Physics modules (modular, hot-swappable)
│   │   └── galaxy_properties.yaml  # Galaxy property metadata
│   └── include/       # Headers + auto-generated code
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

| Task | Documentation |
|------|---------------|
| Implement a new physics module | [Module Developer Guide](developer/module-developer-guide.md) |
| Add a new property | [Property Metadata Schema](architecture/property-metadata-schema.md) quick start |
| Understand code flow | [Execution Flow Reference](developer/execution-flow-reference.md) |
| Add tests | [Testing Guide](developer/testing.md) |
| Fix a bug | Ensure `make tests` passes |

### Build System

```bash
# Standard build
make

# With HDF5 support
make USE-HDF5=yes

# With MPI support
make USE-MPI=yes

# Regenerate code from metadata (smart - only regenerates what changed)
make generate          # All code (properties + modules)
make validate-modules  # Validate module metadata

# Verify generated code is current (CI check)
make check-generated

# Clean
make clean  # Remove all build artifacts
make tidy   # Remove object files, keep executable
```

---

## Documentation by Topic

### Architecture & Design
- **[Vision](architecture/vision.md)** - 8 core architectural principles
- **[Roadmap v4](architecture/roadmap_v4.md)** - Implementation status (Phases 1-3 complete)
- **[Property Metadata Schema](architecture/property-metadata-schema.md)** - Property system specification

### Development Guides
- **[Module Developer Guide](developer/module-developer-guide.md)** - Creating physics modules (1122 lines)
- **[Module Metadata Schema](developer/module-metadata-schema.md)** - Module YAML specification (1237 lines)
- **[Testing Guide](developer/testing.md)** - Comprehensive testing framework (2127 lines)
- **[Execution Flow Reference](developer/execution-flow-reference.md)** - Function call trace (1028 lines)
- **[Coding Standards](developer/coding-standards.md)** - Code style requirements

### User Guides
- **[Module Configuration](user/module-configuration.md)** - Runtime module configuration
- **[Output Formats](user/output-formats.md)** - Binary and HDF5 output specifications

---

## Documentation Structure

```
docs/
├── README.md (this file)        ⭐ START HERE - Entry point for all users
├── architecture/
│   ├── vision.md                - 8 core architectural principles
│   ├── roadmap_v4.md            - Implementation roadmap
│   ├── property-metadata-schema.md - Property system specification
│   └── next-task.md             - Current development tasks
├── developer/
│   ├── module-developer-guide.md   - Creating physics modules
│   ├── module-metadata-schema.md   - Module YAML specification
│   ├── testing.md                  - Comprehensive testing guide
│   ├── execution-flow-reference.md - Function call trace reference
│   └── coding-standards.md         - Code style standards
└── user/
    ├── module-configuration.md     - Configuring physics modules
    └── output-formats.md           - Output file formats
```

---

## Common Questions

**Q: How do I build and run Mimic?**
→ See [First Time Setup](#first-time-setup) above

**Q: How do I add a new physics module?**
→ See [Module Developer Guide](developer/module-developer-guide.md) - complete workflow with examples

**Q: How do I add a new property?**
→ See [Property Metadata Schema](architecture/property-metadata-schema.md) - quick start at top

**Q: How do I configure modules at runtime?**
→ See [Module Configuration](user/module-configuration.md) - all modules and parameters

**Q: Where is the detailed function call trace?**
→ See [Execution Flow Reference](developer/execution-flow-reference.md) - complete trace from entry to exit

**Q: What are Mimic's architectural principles?**
→ See [Architecture Vision](architecture/vision.md) - 8 core principles

**Q: What's the current implementation status?**
→ See [Roadmap](architecture/roadmap_v4.md) - Phases 1-3 complete, Phase 4 in progress

**Q: How do I run tests?**
→ See [Testing Guide](developer/testing.md) quick reference or run `make tests`

---

## Contributing

When adding new features or modules:
1. Follow the **[Coding Standards](developer/coding-standards.md)**
2. Align with the **[Architectural Vision](architecture/vision.md)**
3. Write comprehensive tests (see **[Testing Guide](developer/testing.md)**)
4. Update relevant documentation
5. Document as you go

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
│   │   ├── module_a/      # Example physics module
│   │   ├── module_b/      # Example physics module
│   │   ├── test_fixture/  # Testing infrastructure module
│   │   └── generated/     # Auto-generated module registration
│   ├── modules/           # Physics modules
│   │   └── galaxy_properties.yaml  # Galaxy property metadata
│   ├── core/              # Core execution
│   │   └── halo_properties.yaml    # Halo property metadata
│   └── include/           # Headers
│       └── generated/     # Auto-generated property code
├── build/                 # Build artifacts (gitignored)
│   └── generated/         # Build-time generated files (git_version.h, test lists)
├── tests/                 # Testing framework
│   ├── unit/              # C unit tests
│   ├── integration/       # Python integration tests
│   ├── scientific/        # Physics validation tests
│   ├── framework/         # Test utilities and templates
│   └── generated/         # Auto-generated test metadata
├── docs/                  # Documentation (you are here)
│   ├── generated/         # Auto-generated documentation
│   └── user/              # User guides
├── scripts/               # Development tools
├── input/                 # Input files and data
├── output/                # Simulation output
│   └── mimic-plot/        # Plotting system
│       └── generated/     # Auto-generated Python dtypes
└── mimic_venv/            # Python virtual environment
```
