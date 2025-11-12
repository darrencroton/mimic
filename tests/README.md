# Mimic Testing Infrastructure

**Status**: Phase 4 Complete (Module Testing with Auto-Discovery)
**Last Updated**: 2025-11-13

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
- **Count**: 6 core tests
- **Examples**: `test_memory_system.c`, `test_parameter_parsing.c`, `test_tree_loading.c`

**Integration Tests** (`integration/`)
- **Purpose**: Test complete core workflows end-to-end
- **Language**: Python
- **Runtime**: <1 minute total
- **Count**: 3 core tests
- **Examples**: `test_full_pipeline.py`, `test_output_formats.py`, `test_module_pipeline.py`

**Scientific Tests** (`scientific/`)
- **Purpose**: Validate core property correctness (NaN/Inf, ranges)
- **Language**: Python
- **Runtime**: <5 minutes total
- **Count**: 1 comprehensive core test
- **Example**: `test_scientific.py` (validates all output properties)

### 2. Module Tests (Co-Located with Modules)

Tests for **physics modules** in `src/modules/*/test_*.{c,py}`:

- **Auto-discovered** from `module_info.yaml` declarations
- **Co-located** with module source code
- **Three tiers** per module: unit (C), integration (Python), scientific (Python)
- **Count**: Currently 3 modules × 3 test tiers = 9 module tests
- **Examples**:
  - `src/modules/sage_infall/test_unit_sage_infall.c`
  - `src/modules/sage_infall/test_integration_sage_infall.py`
  - `src/modules/sage_infall/test_scientific_sage_infall_validation.py`

**See [docs/developer/testing.md](../docs/developer/testing.md) for complete module testing guide.**

---

## Directory Structure

**Core Tests** (this directory):
```
tests/
├── unit/                      # C unit tests for core infrastructure
│   ├── test_memory_system.c
│   ├── test_parameter_parsing.c
│   ├── test_tree_loading.c
│   ├── test_property_metadata.c
│   ├── test_numeric_utilities.c
│   ├── test_module_configuration.c
│   ├── run_tests.sh           # Test runner (auto-discovers module tests too)
│   └── build/                 # Compiled tests
├── integration/               # Python integration tests for core
│   ├── test_full_pipeline.py
│   ├── test_output_formats.py
│   └── test_module_pipeline.py
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

**Module Tests** (co-located with modules):
```
src/modules/
├── sage_infall/
│   ├── sage_infall.c
│   ├── sage_infall.h
│   ├── module_info.yaml                    # Declares test files
│   ├── test_unit_sage_infall.c             # Module unit test
│   ├── test_integration_sage_infall.py     # Module integration test
│   └── test_scientific_sage_infall_validation.py  # Module scientific test
├── simple_cooling/
│   ├── simple_cooling.c
│   ├── module_info.yaml
│   ├── test_unit_simple_cooling.c
│   ├── test_integration_simple_cooling.py
│   └── test_scientific_simple_cooling.py
└── simple_sfr/
    ├── simple_sfr.c
    ├── module_info.yaml
    ├── test_unit_simple_sfr.c
    ├── test_integration_simple_sfr.py
    └── test_scientific_simple_sfr.py
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

1. **Copy existing module test as example** (e.g., from `src/modules/sage_infall/test_*`)
2. **Adapt for your module's functionality**
3. **Place in module directory**: `src/modules/YOUR_MODULE/test_*.{c,py}`
4. **Declare in `module_info.yaml`**:
   ```yaml
   tests:
     unit: test_unit_YOUR_MODULE.c
     integration: test_integration_YOUR_MODULE.py
     scientific: test_scientific_YOUR_MODULE_validation.py
   ```
5. **Run `make generate-test-registry`** to register tests
6. **Verify**: `make tests` (tests are auto-discovered and run)

**Important**:
- **Core test templates** (in `framework/`) are for core infrastructure only
- **Module tests** should use existing module tests as examples (in `src/modules/*/`)
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

**Last Updated**: 2025-11-10 (Phase 3 Complete - Scientific tests consolidated)
