# Mimic Testing Infrastructure

**Status**: Phase 4 Complete (Module Testing with Auto-Discovery)
**Last Updated**: 2025-11-30

This directory contains Mimic's automated testing infrastructure for **core infrastructure only**. Module tests are co-located with module code in `src/modules/*/test_*.{c,py}` and auto-discovered via metadata.

For detailed information on writing tests, debugging failures, and CI integration, see **[`docs/developer/testing.md`](../docs/developer/testing.md)**.

---

## Quick Start

```bash
# Run all tests
make tests

# Run specific test tiers
make test-unit          # C unit tests (<10s)
make test-integration   # Python integration tests (<1min)
make test-scientific    # Python scientific tests (<5min)
```

---

## Test Organization

Mimic has **two separate test systems**:

### 1. Core Tests (This Directory)

Tests for **physics-agnostic infrastructure** (memory, I/O, tree processing, module system):

```
         /\
        /  \       Scientific Tests (core property validation)
       /____\
      /      \     Integration Tests (pipeline, formats)
     /________\
    /          \   Unit Tests (infrastructure components)
   /____________\
```

**Unit Tests** (`unit/`)
- **Purpose**: Test individual infrastructure components in isolation
- **Language**: C
- **Runtime**: <10 seconds total
- **Count**: 8 core tests
- **Examples**: `test_memory_system.c`, `test_property_metadata.c`, `test_model_parameter_metadata.c`, `test_parameter_parsing.c`

**Integration Tests** (`integration/`)
- **Purpose**: Test complete core workflows end-to-end
- **Language**: Python
- **Runtime**: <1 minute total
- **Count**: 10 core tests
- **Coverage**: Pipeline execution, output formats, multi-phase system, time-stepping, loop modes, tree preservation

**Scientific Tests** (`scientific/`)
- **Purpose**: Validate core property correctness (NaN/Inf, ranges)
- **Language**: Python
- **Runtime**: <5 minutes total
- **Count**: 1 comprehensive core test
- **Example**: `test_scientific.py` (validates all output properties)

### 2. Module Tests (Co-Located with Modules)

Tests for **physics modules** in `src/modules/*/tests/test_*.{c,py}`:

- **Auto-discovered** from `module_info.yaml` declarations
- **Co-located** with module source code in `tests/` subdirectories
- **Three tiers** per module: unit (C), integration (Python), scientific (Python)
- **Count**: Currently 9 modules × up to 3 test tiers = 25 module tests
- **Examples**: See test files in `src/modules/*/tests/test_*`

**See [docs/developer/testing.md](../docs/developer/testing.md) for complete module testing guide.**

### 3. Test Fixture Module (Special Purpose)

The **`test_fixture` module** exists solely for **infrastructure testing**:

- **Purpose**: Provides stable test interface for core infrastructure tests
- **Location**: `src/modules/_system/test_fixture/`
- **Usage**: Referenced by core unit/integration tests in this directory
- **NOT FOR PRODUCTION**: Never use in scientific runs
- **Benefit**: Decouples infrastructure tests from production physics modules

This ensures core infrastructure tests remain physics-agnostic (Vision Principle #1), allowing production modules to be archived without breaking core tests.

**See [src/modules/_system/test_fixture/README.md](../src/modules/_system/test_fixture/README.md) for details.**

---

## Directory Structure

**Core Tests** (this directory):
```
tests/
├── unit/                      # C unit tests for core infrastructure
│   ├── test_memory_system.c
│   ├── test_property_metadata.c
│   ├── test_model_parameter_metadata.c
│   ├── test_parameter_parsing.c
│   ├── test_tree_loading.c
│   ├── test_numeric_utilities.c
│   ├── test_module_configuration.c
│   ├── test_virial_properties.c
│   ├── run_tests.sh           # Test runner (auto-discovers module tests too)
│   └── build/                 # Compiled tests
├── integration/               # Python integration tests for core
│   ├── test_full_pipeline.py
│   ├── test_output_formats.py
│   ├── test_module_pipeline.py
│   ├── test_tree_preservation.py
│   ├── test_unique_galaxy_id.py
│   ├── test_satellite_spatial_distribution.py
│   ├── test_phase_execution.py
│   ├── test_substeps.py
│   ├── test_processing_modes.py
│   └── test_galaxy_major_loop.py
├── scientific/                # Python scientific tests for core
│   └── test_scientific.py     # Validates all output properties
├── data/                      # Test data and outputs
│   ├── input/                 # Input test data (trees_063.0, millennium.a_list)
│   └── output/                # Test outputs
│       ├── baseline/          # Static reference (committed to git)
│       ├── binary/            # Test output, binary format (gitignored)
│       └── hdf5/              # Test output, HDF5 format (gitignored)
└── framework/                 # Test framework and templates (CORE ONLY)
    ├── test_framework.h       # C testing macros
    ├── data_loader.py         # Binary file loader
    ├── c_unit_test_template.c              # Template for core unit tests
    ├── python_integration_test_template.py # Template for core integration tests
    └── python_scientific_test_template.py  # Template for core scientific tests
```

**Module Tests** (co-located with modules in tests/ subdirectories):
```
src/modules/
├── module_a/
│   ├── tests/                               # Module tests subdirectory
│   │   ├── test_unit_module_a.c             # Module unit test
│   │   ├── test_integration_module_a.py     # Module integration test
│   │   └── test_scientific_module_a_validation.py  # Module scientific test
│   ├── module_a.c
│   ├── module_a.h
│   └── module_info.yaml                     # Declares test files
├── module_b/
│   ├── tests/                               # Module tests subdirectory
│   │   ├── test_unit_module_b.c
│   │   ├── test_integration_module_b.py
│   │   └── test_scientific_module_b.py
│   ├── module_b.c
│   └── module_info.yaml
├── _system/test_fixture/                    # Infrastructure testing only
│   ├── tests/                               # Test fixture tests
│   │   ├── test_unit_test_fixture.c
│   │   └── test_integration_test_fixture.py
│   ├── fixture.c
│   ├── module_info.yaml
│   └── README.md                            # Explains special purpose
└── [other modules...]
```

**Test Registry** (auto-generated):
```
build/generated_test_lists/
├── unit_tests.txt              # Auto-discovered module unit tests
├── integration_tests.txt       # Auto-discovered module integration tests
├── scientific_tests.txt        # Auto-discovered module scientific tests
└── test_registry_hash.txt      # Registry validation hash
```

---

## Test Data

**Input files** (`data/input/`):
- `trees_063.0` (17M) - Single merger tree file
- `millennium.a_list` (577B) - Snapshot ages/redshifts

**Output directories** (`data/output/`):
- `baseline/` - **Static reference data** (committed to git, NEVER regenerated by tests)
  - Used as comparison baseline for regression testing
  - Established once and preserved across all test runs
  - Contains known-good output from validated Mimic run
- `binary/` - Test output from binary format runs (gitignored, regenerated by tests)
  - Tests write here and compare against baseline/
- `hdf5/` - Test output from HDF5 format runs (gitignored, regenerated by tests)
  - Tests write here and compare against baseline/

---

## Adding New Tests

### For Core Infrastructure Tests

1. Choose test type (unit/integration/scientific)
2. Copy appropriate template from `framework/`
3. Implement test following the template structure
4. Add to test runner (C tests: add to `CORE_TESTS` in `unit/run_tests.sh`)
5. Place in appropriate directory (`tests/unit/`, `tests/integration/`, `tests/scientific/`)
6. Verify: `make tests`

### For Module Tests

1. **Copy existing module test as example** (from `src/modules/*/tests/test_*`)
2. **Adapt for your module's functionality**
3. **Place in module tests directory**: `src/modules/YOUR_MODULE/tests/test_*.{c,py}`
4. **Declare in `module_info.yaml`**:
   ```yaml
   tests:
     unit: tests/test_unit_YOUR_MODULE.c
     integration: tests/test_integration_YOUR_MODULE.py
     scientific: tests/test_scientific_YOUR_MODULE_validation.py
   ```
5. **Run `make generate-test-registry`** to register tests
6. **Verify**: `make tests` (tests are auto-discovered and run)

**Important**:
- **Core test templates** (in `framework/`) are for core infrastructure only
- **Module tests** should use existing module tests as examples (in `src/modules/*/tests/`)
- Module tests must be in `tests/` subdirectory within each module
- Module tests must follow naming convention: `test_unit_*.c`, `test_integration_*.py`, `test_scientific_*_validation.py`

See **[`docs/developer/testing.md`](../docs/developer/testing.md)** for comprehensive module testing guide with complete examples.

---

## Test Framework

### C Unit Tests
- Framework: `framework/test_framework.h`
- Macros: `TEST_ASSERT`, `TEST_RUN`, `TEST_SUMMARY`
- Features: Memory leak detection, setup/execute/verify/cleanup structure

### Python Tests
- Compatible with standalone execution or pytest
- Helper utilities in `framework/data_loader.py`

---

## Continuous Integration

All tests run automatically on every commit via GitHub Actions.

**Requirements for merge**:
- ✅ All tests pass
- ✅ No memory leaks
- ✅ Property metadata up-to-date
- ✅ Code quality checks pass

---

## Documentation

**Comprehensive guide**: [`docs/developer/testing.md`](../docs/developer/testing.md)

**Topics covered**:
- Detailed testing philosophy and standards
- Test output formatting requirements
- How to write unit/integration/scientific tests
- Template usage and examples
- Debugging test failures
- CI integration details
- Best practices and common patterns

---

**Last Updated**: 2025-12-10
