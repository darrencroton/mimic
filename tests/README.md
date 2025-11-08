# Mimic Testing Infrastructure

**Status**: Phase 2 Implementation (Testing Framework)
**Version**: 1.0
**Documentation**: See `docs/developer/testing.md` for comprehensive guide

---

## Overview

This directory contains the automated testing infrastructure for Mimic. Tests are organized into three tiers following the testing pyramid:

```
         /\
        /  \       Scientific Tests (slow, comprehensive physics validation)
       /____\
      /      \     Integration Tests (medium, end-to-end pipeline)
     /________\
    /          \   Unit Tests (fast, focused component testing)
   /____________\
```

**Test Execution Time Targets**:
- Unit tests: <10 seconds total
- Integration tests: <1 minute total
- Scientific tests: <5 minutes total
- **Total CI runtime**: <2 minutes (unit + integration)

---

## Quick Start

```bash
# Run all tests
make test

# Run specific test tier
make test-unit          # C unit tests
make test-integration   # Python integration tests
make test-scientific    # Python scientific tests

# Run individual test
cd tests/unit && ./test_memory_system.test
cd tests/integration && python test_full_pipeline.py
cd tests/scientific && python test_physics_sanity.py

# Clean test outputs
make test-clean
```

---

## Directory Structure

```
tests/
├── README.md                  # This file
├── unit/                      # C unit tests (fast, focused)
│   ├── test_memory_system.c
│   ├── test_property_metadata.c
│   ├── test_parameter_parsing.c
│   ├── test_tree_loading.c
│   ├── test_numeric_utilities.c
│   └── run_tests.sh           # Unit test runner
├── integration/               # Python integration tests (end-to-end)
│   ├── test_full_pipeline.py
│   ├── test_output_formats.py
│   └── test_bit_identical.py
├── scientific/                # Python scientific tests (physics validation)
│   ├── test_physics_sanity.py
│   └── test_property_ranges.py
├── data/                      # Test data and baselines
│   ├── millennium/            # Mini-Millennium test data
│   │   ├── trees_063.0        # Single tree file (17M)
│   │   └── millennium.a_list  # Snapshot ages (577B)
│   ├── test.par               # Test parameter file
│   └── expected/              # Known-good test outputs
│       └── test/              # Output from test.par runs
└── framework/                 # Test framework and shared utilities
    ├── test_framework.h       # C testing framework
    ├── data_loader.py         # Binary output file loader (Python)
    ├── c_unit_test_template.c
    ├── python_integration_test_template.py
    └── python_scientific_test_template.py
```

---

## Test Data

### Mini-Millennium Test Data

**Location**: `tests/data/millennium/`

**Files**:
- `trees_063.0` (17M) - Single merger tree file from mini-Millennium simulation
- `millennium.a_list` (577B) - Snapshot ages and redshifts

**Why this data**:
- Real simulation data (proven to work with Mimic)
- Small enough for fast testing (~10 seconds per run)
- Committed to git repository (self-contained testing)
- Representative of production workloads

**Parameter file**: `tests/data/test.par`
- Processes only trees_063.0 (FirstFile=0, LastFile=0)
- Outputs only snapshot 63
- Fast execution for CI/CD

---

## Test Tiers

### 1. Unit Tests (C)

**Purpose**: Test individual components in isolation
**Runtime**: <10 seconds total
**Language**: C
**Framework**: Custom minimal framework (tests/framework/test_framework.h)

**Tests**:
- `test_memory_system.c` - Memory allocation, leak detection, categorization
- `test_property_metadata.c` - Generated structures, property counts, metadata
- `test_parameter_parsing.c` - Config file parsing, validation, bounds checking
- `test_tree_loading.c` - Tree file reading, structure validation
- `test_numeric_utilities.c` - Safe math, integration routines

**Running**:
```bash
cd tests/unit
./run_tests.sh              # Run all unit tests
./test_memory_system.test   # Run specific test
```

**Adding new test**:
1. Copy `framework/c_unit_test_template.c` to `unit/test_yourname.c`
2. Implement test functions
3. Add to `run_tests.sh`
4. Verify: `./test_yourname.test`

### 2. Integration Tests (Python)

**Purpose**: Test full Mimic pipeline end-to-end
**Runtime**: <1 minute total
**Language**: Python
**Framework**: Pytest-compatible (can run standalone or with pytest)

**Tests**:
- `test_full_pipeline.py` - Complete execution, output validation, leak checking
- `test_output_formats.py` - Binary and HDF5 output format verification
- `test_bit_identical.py` - Regression detection via checksums

**Running**:
```bash
cd tests/integration
python test_full_pipeline.py       # Run standalone
pytest test_full_pipeline.py -v    # Run with pytest
pytest -v                           # Run all integration tests
```

**Adding new test**:
1. Copy `framework/python_integration_test_template.py`
2. Implement test functions
3. Test: `python test_yourname.py`

### 3. Scientific Tests (Python)

**Purpose**: Validate physics correctness and halo property ranges
**Runtime**: <5 minutes total (may run nightly for comprehensive tests)
**Language**: Python
**Framework**: Pytest-compatible, uses `framework/data_loader.py` for loading binary output

**Tests**:
- `test_physics_sanity.py` - Validates ALL halos: no NaN/Inf, positive masses/radii/velocities
- `test_property_ranges.py` - Validates ALL halos: mass, radius, velocity, position ranges

**Running**:
```bash
cd tests/scientific
python test_physics_sanity.py      # Run standalone
pytest -v                           # Run all scientific tests
```

**Adding new test**:
1. Copy `framework/python_scientific_test_template.py`
2. Define conservation law or property range
3. Set appropriate tolerance
4. Document physical expectation

---

## Test Framework

### C Unit Test Framework

**File**: `framework/test_framework.h`

**Features**:
- Minimal, dependency-free
- Assertion macros (TEST_ASSERT, TEST_ASSERT_EQUAL, etc.)
- Test execution macros (TEST_RUN, TEST_SUMMARY, TEST_RESULT)
- Integrates with Mimic's memory and error handling

**Example**:
```c
#include "../framework/test_framework.h"

static int passed = 0, failed = 0;

int test_example(void) {
    TEST_ASSERT(1 + 1 == 2, "Math works");
    return TEST_PASS;
}

int main(void) {
    TEST_RUN(test_example);
    TEST_SUMMARY();
    return TEST_RESULT();
}
```

### Python Test Framework

**Compatibility**: Works standalone or with pytest

**Example**:
```python
def test_example():
    assert 1 + 1 == 2, "Math works"
    print("✓ Test passed")

if __name__ == "__main__":
    test_example()
```

---

## Continuous Integration (CI)

**File**: `.github/workflows/ci.yml`

**Triggers**: Push, Pull Request

**Steps**:
1. Build Mimic
2. Check property metadata (`make check-generated`)
3. Run unit tests (`make test-unit`)
4. Run integration tests (`make test-integration`)
5. Check for memory leaks

**Target runtime**: <2 minutes

**Status**: Tests must pass for merges to main branch

---

## Test Development Workflow

### 1. Identify what to test
- New feature added? → Write integration test
- New physics module? → Write scientific test
- New utility function? → Write unit test

### 2. Choose appropriate tier
- Fast, focused component? → Unit test (C)
- End-to-end pipeline? → Integration test (Python)
- Physics validation? → Scientific test (Python)

### 3. Use templates
- Copy appropriate template from `framework/`
- Follow setup → execute → validate → cleanup structure
- Document what is tested and why

### 4. Verify test works
- Run standalone first
- Run via make target
- Introduce deliberate bug to verify test catches it

### 5. Add to CI
- Unit and integration tests run on every commit
- Scientific tests may run nightly (if slow)

---

## Best Practices

### All Tests

✅ **DO**:
- Use descriptive test names (`test_memory_leak_detection`, not `test1`)
- Document what is tested and why
- Follow template structure
- Keep tests focused (one behavior per test)
- Make failure messages informative

❌ **DON'T**:
- Write tests that depend on other tests
- Use hardcoded paths (use Path objects)
- Skip cleanup (especially memory)
- Test implementation details (test behavior)

### Unit Tests (C)

✅ **DO**:
- Always check for memory leaks
- Test edge cases and error handling
- Keep tests fast (<1 second each)
- Use TEST_ASSERT macros for clear failures

❌ **DON'T**:
- Test multiple components together (use integration tests)
- Depend on external files (mock if needed)

### Integration Tests (Python)

✅ **DO**:
- Test end-to-end pipeline
- Verify output files created
- Check for memory leaks in logs
- Validate output format and structure

❌ **DON'T**:
- Test individual functions (use unit tests)
- Make tests depend on specific values (use ranges)

### Scientific Tests (Python)

✅ **DO**:
- Use physically motivated tolerances
- Document expected ranges and why
- Reference published results when comparing
- Test conservation laws

❌ **DON'T**:
- Use overly tight tolerances (numerical precision)
- Test implementation details
- Expect exact matches to literature (different codes)

---

## Test Coverage

**Current Status** (Phase 2):
- 5 unit tests
- 3 integration tests
- 2 scientific tests
- **Total: 10 tests**

**Coverage Target**: >60% of critical paths
- Memory management: ✓ Covered
- I/O system: ✓ Covered
- Tree processing: ✓ Covered
- Parameter parsing: ✓ Covered
- Property metadata: ✓ Covered

**Future Coverage** (Phases 3-6):
- Module system (Phase 3)
- Module interface (Phase 4)
- Realistic physics modules (Phase 6)

---

## Troubleshooting

### Tests fail after clean build
→ Run `make generate` to regenerate property code

### Memory leak detected
→ Check test log files in `tests/data/expected/test/metadata/`
→ Use Valgrind for detailed analysis

### Integration tests can't find Mimic executable
→ Build Mimic first: `make`
→ Check paths in test scripts

### Scientific tests fail with "unreasonable values"
→ Check if test data is correct (`trees_063.0` present?)
→ Verify parameter file points to correct paths
→ Check if baseline expectations need updating

### CI fails but local tests pass
→ Check CI log for specific error
→ Verify all test data committed to git
→ Check GitHub Actions has correct dependencies

---

## Documentation

**Comprehensive guide**: `docs/developer/testing.md`

**Topics covered**:
- Detailed testing philosophy
- Template usage examples
- How to add tests for new features
- CI integration details
- Debugging test failures
- Test data management

---

## Roadmap

**Phase 2 (Current)**: Testing Framework
- ✓ Test infrastructure
- ✓ 10 essential tests
- ✓ CI integration
- ✓ Templates for future tests

**Phase 3**: Module System Tests
- Module registration tests
- Module execution tests
- Module configuration tests

**Phase 6**: Realistic Module Tests
- Cooling physics tests
- Conservation law tests
- Module interaction tests

---

## Contact

**Questions**: See `docs/developer/testing.md`

**Issues**: Report test failures with:
- Test name that failed
- Error message
- Steps to reproduce
- Environment (OS, compiler version)

---

**Last Updated**: 2025-11-08 (Phase 2 Implementation)
