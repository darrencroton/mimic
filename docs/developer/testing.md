# Mimic Testing Guide

**Version**: 1.1 (Phase 3 Complete)
**Status**: Production
**Last Updated**: 2025-11-09

This comprehensive guide explains how to use Mimic's testing infrastructure, write new tests, and debug test failures.

---

## Table of Contents

1. [Quick Start](#quick-start)
2. [Testing Philosophy](#testing-philosophy)
3. [Test Output Standards](#test-output-standards)
4. [Test Organization](#test-organization)
5. [Running Tests](#running-tests)
6. [Writing Unit Tests (C)](#writing-unit-tests-c)
7. [Writing Integration Tests (Python)](#writing-integration-tests-python)
8. [Writing Scientific Tests (Python)](#writing-scientific-tests-python)
9. [Test Templates](#test-templates)
10. [Debugging Test Failures](#debugging-test-failures)
11. [CI Integration](#ci-integration)
12. [Best Practices](#best-practices)
13. [FAQ](#faq)

---

## Quick Start

### Running All Tests

```bash
# Build Mimic first
make

# Run all tests
make test

# Run specific test tiers
make test-unit          # C unit tests (<10s)
make test-integration   # Python integration tests (<1min)
make test-scientific    # Python scientific tests (<5min)
```

### Running Individual Tests

```bash
# Unit test
cd tests/unit
./test_memory_system.test

# Integration test
cd tests/integration
python test_full_pipeline.py

# Scientific test
cd tests/scientific
python test_physics_sanity.py
```

### Adding a New Test

1. Choose test type (unit/integration/scientific)
2. Copy appropriate template from `tests/framework/`
3. Implement test functions
4. Add to test runner (for unit tests: `run_tests.sh`)
5. Verify: `make test`

---

## Testing Philosophy

Mimic follows the **testing pyramid** approach:

```
         /\
        /  \       Scientific Tests (slow, physics validation)
       /____\
      /      \     Integration Tests (medium, end-to-end)
     /________\
    /          \   Unit Tests (fast, component-focused)
   /____________\
```

### Test Tiers

1. **Unit Tests** (C)
   - **Purpose**: Test individual components in isolation
   - **Runtime**: <10 seconds total
   - **Coverage**: Memory, properties, parameters, I/O, utilities
   - **When**: Every code change

2. **Integration Tests** (Python)
   - **Purpose**: Test full pipeline execution
   - **Runtime**: <1 minute total
   - **Coverage**: End-to-end workflows, output formats, regressions
   - **When**: Before commits

3. **Scientific Tests** (Python)
   - **Purpose**: Validate physics correctness
   - **Runtime**: <5 minutes total
   - **Coverage**: Conservation laws, property ranges, physics sanity
   - **When**: Before releases or physics changes

### Why This Approach?

- **Fast feedback**: Unit tests run in seconds
- **Comprehensive coverage**: Integration tests catch system-level bugs
- **Scientific accuracy**: Scientific tests validate physics
- **Maintainable**: Clear separation of concerns

---

## Test Output Standards

All test output must provide clear, meaningful feedback that enables rapid diagnosis of issues.

### Requirements

Test output must be:
1. **Descriptive**: Clearly state what is being tested
2. **Informative**: Provide context about the test's purpose
3. **Formatted**: Use consistent visual separation between tests and test groups
4. **Actionable**: When tests fail, provide enough information to debug

### Visual Formatting

- **Separator lines**: 60 characters (`============================================================`)
- **Test groups**: Separated by double newlines with clear headers
- **Individual tests**: Single newline before each test
- **Pass/Fail indicators**: Use ✓/✗ symbols for clarity

### Example Output

**Good test output**:
```
============================================================
RUNNING UNIT TESTS
============================================================

Running test: test_module_configuration
------------------------------------------------------------

Module Configuration System Tests

Running: test_module_registry_init                          ✓ PASS
Running: test_module_parameter_parsing                      ✓ PASS
Running: test_enabled_modules_parsing                       ✓ PASS

============================================================
Test Summary
============================================================
Passed: 3
Failed: 0
Total:  3
============================================================
✓ All tests passed!
============================================================
```

**Good failure output**:
```
Running: test_basic_allocation                              ✗ FAIL

✗ FAIL: Small allocation should succeed
  Location: test_memory_system.c:45
  Condition: small != NULL
```

### Implementation

- **C tests**: Use macros from `tests/framework/test_framework.h` (TEST_RUN, TEST_SUMMARY)
- **Python tests**: Print descriptive messages at test start and completion
- **Test runners**: Provide clear headers and summaries for test groups

---

## Test Organization

### Directory Structure

```
tests/
├── unit/                      # C unit tests
│   ├── test_memory_system.c
│   ├── test_property_metadata.c
│   ├── test_parameter_parsing.c
│   ├── test_tree_loading.c
│   ├── test_numeric_utilities.c
│   ├── test_module_configuration.c
│   ├── run_tests.sh           # Test runner
│   └── build/                 # Compiled tests
├── integration/               # Python integration tests
│   ├── test_full_pipeline.py
│   ├── test_output_formats.py
│   └── test_module_pipeline.py
├── scientific/                # Python scientific tests
│   ├── test_physics_sanity.py
│   └── test_property_ranges.py
├── data/                      # Test data
│   ├── input/                 # Input test data
│   │   ├── trees_063.0        # Single tree file (17M)
│   │   └── millennium.a_list  # Snapshot ages (577B)
│   ├── test_binary.par        # Binary format test parameter file
│   ├── test_hdf5.par          # HDF5 format test parameter file
│   └── output/                # Test outputs
│       ├── baseline/          # Known-good baseline outputs (committed to git)
│       │   ├── binary/        # Binary format baseline
│       │   └── hdf5/          # HDF5 format baseline
│       ├── binary/            # Binary format test outputs (generated, not in git)
│       └── hdf5/              # HDF5 format test outputs (generated, not in git)
└── framework/                 # Test framework and templates
    ├── test_framework.h
    ├── data_loader.py         # Binary output file loader (Python)
    ├── c_unit_test_template.c
    ├── python_integration_test_template.py
    └── python_scientific_test_template.py
```

### Test Data

**Input Data** (`tests/data/input/`):
- `trees_063.0` (17M) - Single tree file from mini-Millennium
- `millennium.a_list` (577B) - Snapshot ages/redshifts

**Why this data**:
- Real simulation data (proven to work)
- Small enough for fast tests (~10s execution)
- Representative of production workloads
- Committed to repository (self-contained)

**Parameter Files** (`tests/data/`):
- `test_binary.par` - Binary format test configuration (outputs to `tests/data/output/binary/`)
- `test_hdf5.par` - HDF5 format test configuration (outputs to `tests/data/output/hdf5/`)

**Output Data**:
- `tests/data/output/baseline/binary/` - Known-good binary baseline (committed to git, used for regression testing)
- `tests/data/output/baseline/hdf5/` - Known-good HDF5 baseline (committed to git, used for regression testing)
- `tests/data/output/binary/` - Generated test outputs in binary format (gitignored)
- `tests/data/output/hdf5/` - Generated test outputs in HDF5 format (gitignored)

**Baseline Testing Approach**:
Tests compare generated output against baseline using direct data loading and comparison (halo counts, mass ranges, etc.), not checksums. This provides more meaningful validation of scientific correctness.

---

## Running Tests

### Via Makefile (Recommended)

```bash
# All tests
make test

# Specific tiers
make test-unit          # Unit tests only
make test-integration   # Integration tests only
make test-scientific    # Scientific tests only

# Clean test artifacts
make test-clean
```

### Unit Tests (Direct)

```bash
cd tests/unit

# Run all unit tests
./run_tests.sh

# Run specific test
./run_tests.sh test_memory_system

# Compile and run manually
gcc -I../../src/include -o test_memory_system.test test_memory_system.c ../../src/util/memory.c ../../src/util/error.c -lm
./test_memory_system.test
```

### Integration Tests (Direct)

```bash
cd tests/integration

# Run standalone
python test_full_pipeline.py

# Run with pytest
pytest test_full_pipeline.py -v

# Run all with pytest
pytest -v
```

### Scientific Tests (Direct)

```bash
cd tests/scientific

# Run standalone
python test_physics_sanity.py

# Run with pytest
pytest -v
```

### Expected Output

**Successful run**:
```
============================================================
RUNNING UNIT TESTS
============================================================

Running test: test_memory_system
------------------------------------------------------------

========================================
Test Suite: Memory System
========================================

Running: test_memory_init                                   ✓ PASS
Running: test_basic_allocation                              ✓ PASS
Running: test_categorized_allocation                        ✓ PASS
Running: test_reallocation                                  ✓ PASS
Running: test_leak_detection                                ✓ PASS
Running: test_multiple_alloc_free_cycles                    ✓ PASS


============================================================
Test Summary
============================================================
Passed: 6
Failed: 0
Total:  6
============================================================
✓ All tests passed!
============================================================
------------------------------------------------------------
✓ test_memory_system PASSED
============================================================
```

**Failed test**:
```
Running: test_basic_allocation                              ✗ FAIL

✗ FAIL: Small allocation should succeed
  Location: test_memory_system.c:45
  Condition: small != NULL
```

---

## Writing Unit Tests (C)

### Step-by-Step Guide

1. **Copy template**:
   ```bash
   cp tests/framework/c_unit_test_template.c tests/unit/test_yourname.c
   ```

2. **Update header**:
   ```c
   /**
    * @file    test_yourname.c
    * @brief   Test description
    *
    * Validates: What this test validates
    * Phase: Phase 2 (or later phase if adding for new feature)
    */
   ```

3. **Implement test functions**:
   ```c
   int test_specific_behavior(void) {
       /* ===== SETUP ===== */
       init_memory_system(0);

       /* ===== EXECUTE ===== */
       int result = function_under_test();

       /* ===== VALIDATE ===== */
       TEST_ASSERT(result == expected, "Description of failure");

       /* ===== CLEANUP ===== */
       check_memory_leaks();
       return TEST_PASS;
   }
   ```

4. **Add to main()**:
   ```c
   int main(void) {
       TEST_RUN(test_specific_behavior);
       TEST_SUMMARY();
       return TEST_RESULT();
   }
   ```

5. **Add to run_tests.sh**:
   ```bash
   TESTS="... test_yourname"
   ```

6. **Verify**:
   ```bash
   cd tests/unit
   ./run_tests.sh test_yourname
   ```

### Assertion Macros

```c
TEST_ASSERT(cond, msg)                    // General assertion
TEST_ASSERT_EQUAL(a, b, msg)              // Integer equality
TEST_ASSERT_DOUBLE_EQUAL(a, b, tol, msg)  // Float equality with tolerance
TEST_ASSERT_STRING_EQUAL(a, b, msg)       // String equality
```

### Example: Testing a Utility Function

```c
int test_safe_division(void) {
    init_memory_system(0);

    // Test normal division
    double result = safe_divide(10.0, 2.0);
    TEST_ASSERT_DOUBLE_EQUAL(result, 5.0, 0.001, "10/2 should be 5");

    // Test division by zero (should return 0 or handle gracefully)
    result = safe_divide(10.0, 0.0);
    TEST_ASSERT(isfinite(result), "Division by zero should not produce NaN/Inf");

    check_memory_leaks();
    return TEST_PASS;
}
```

### Unit Test Best Practices

✅ **DO**:
- Test one behavior per test function
- Always check for memory leaks
- Use descriptive test names
- Keep tests fast (<1 second each)
- Test edge cases and error conditions

❌ **DON'T**:
- Test multiple components together (use integration tests)
- Skip cleanup (memory leaks!)
- Use hardcoded paths (use TEST_DATA_DIR)
- Depend on test execution order

---

## Writing Integration Tests (Python)

### Step-by-Step Guide

1. **Copy template**:
   ```bash
   cp tests/framework/python_integration_test_template.py tests/integration/test_yourname.py
   chmod +x tests/integration/test_yourname.py
   ```

2. **Update header docstring**

3. **Implement test functions**:
   ```python
   def test_specific_workflow():
       """Test description"""
       # Execute
       returncode, stdout, stderr = run_mimic("tests/data/test.par")

       # Validate
       assert returncode == 0, f"Mimic failed: {stderr}"
       assert check_no_memory_leaks(output_dir), "Memory leaks detected"
   ```

4. **Add to main()**:
   ```python
   tests = [
       test_specific_workflow,
       # ... other tests
   ]
   ```

5. **Verify**:
   ```bash
   python tests/integration/test_yourname.py
   ```

### Helper Functions

```python
run_mimic(param_file)              # Execute Mimic, return (code, stdout, stderr)
check_no_memory_leaks(output_dir)  # Check logs for leaks
load_binary_halos(output_file)     # Load output data (implement as needed)
```

### Example: Testing New Output Format

```python
def test_new_format():
    """Test that new output format works"""
    # Modify parameter file for new format
    # ...

    # Run Mimic
    returncode, stdout, stderr = run_mimic(temp_par)
    assert returncode == 0, "Execution failed"

    # Validate output
    output_file = Path("tests/data/output/baseline/model_063.newformat")
    assert output_file.exists(), "Output file not created"
    assert output_file.stat().st_size > 0, "Output file empty"
```

### Integration Test Best Practices

✅ **DO**:
- Test end-to-end workflows
- Verify output files created
- Check for memory leaks in logs
- Use realistic data (trees_063.0)
- Clean up temporary files

❌ **DON'T**:
- Test individual functions (use unit tests)
- Hardcode expected values (use ranges)
- Depend on specific halo counts (may change)
- Skip error checking

---

## Writing Scientific Tests (Python)

### Step-by-Step Guide

1. **Copy template**:
   ```bash
   cp tests/framework/python_scientific_test_template.py tests/scientific/test_yourname.py
   chmod +x tests/scientific/test_yourname.py
   ```

2. **Define physical expectations**:
   ```python
   EXPECTED_RANGES = {
       'property_min': value,
       'property_max': value,
       'tolerance': value,
   }
   ```

3. **Implement validation**:
   ```python
   def test_conservation_law():
       """Test specific conservation law"""
       halos = load_halos("output.dat")

       # Calculate conserved quantity
       total = np.sum(halos['Mass'])
       expected = known_value

       # Validate within tolerance
       relative_error = abs(total - expected) / expected
       assert relative_error < 0.01, f"Not conserved: {relative_error*100}% error"
   ```

4. **Document physics**:
   - Reference papers if comparing to literature
   - Explain tolerance choices
   - Note assumptions

### Example: Testing Conservation Law

```python
def test_baryon_conservation():
    """
    Test that baryonic mass is conserved

    Expected: Total baryons = ColdGas + StellarMass + HotGas
    Tolerance: 1% (numerical precision + physics approximations)
    Reference: Croton et al. 2006, Eq. 15
    """
    halos = load_halos("output.dat")

    total_baryons = (halos['ColdGas'] +
                     halos['StellarMass'] +
                     halos['HotGas'])
    expected_baryons = halos['Mvir'] * BARYON_FRACTION

    relative_error = np.abs(total_baryons - expected_baryons) / expected_baryons

    assert np.all(relative_error < 0.01), \
        f"Baryon conservation violated: max error {np.max(relative_error)*100:.2f}%"
```

### Scientific Test Best Practices

✅ **DO**:
- Use physically motivated tolerances
- Document expected ranges and why
- Reference published results when comparing
- Test conservation laws
- Explain physics assumptions

❌ **DON'T**:
- Use overly tight tolerances (numerical precision limits)
- Test implementation details
- Expect exact matches to literature (different codes)
- Skip documenting tolerance choices

---

## Test Templates

### Available Templates

1. **tests/framework/c_unit_test_template.c**
   - For C unit tests
   - Includes memory leak checking
   - Setup/execute/validate/cleanup structure

2. **tests/framework/python_integration_test_template.py**
   - For integration tests
   - Pytest-compatible
   - Helper functions for running Mimic

3. **tests/framework/python_scientific_test_template.py**
   - For scientific validation
   - Tolerance-based comparisons
   - Conservation law structure

### Template Guidelines

**Every test must include**:
- File header documenting purpose and phase
- Setup → Execute → Validate → Cleanup structure
- Memory leak check (C tests)
- Clear assertion messages
- Documentation of expected behavior

**Naming conventions**:
- Files: `test_<component>_<aspect>.c` or `.py`
- Functions: `test_<specific_behavior>()`
- Descriptive: `test_memory_leak_detection()` not `test1()`

---

## Debugging Test Failures

### Unit Test Failures

**Problem**: Test compilation fails

**Solution**:
```bash
# Check compilation log
cat tests/unit/build/test_name.compile.log

# Common issues:
# - Missing include: Add -I flag to run_tests.sh
# - Missing source: Add source file to ALL_SRCS
# - Linking error: Add library to LDFLAGS
```

**Problem**: Test fails at runtime

**Solution**:
```bash
# Run test directly to see full output
cd tests/unit
./test_name.test

# Use GDB for segfaults
gdb ./test_name.test
(gdb) run
(gdb) backtrace
```

**Problem**: Memory leak reported

**Solution**:
```c
// Add diagnostic prints
printf("Before operation\n");
print_allocated();
// ... operation ...
printf("After operation\n");
print_allocated();

// Check what's still allocated
print_allocated_by_category();
```

### Integration Test Failures

**Problem**: Mimic execution fails

**Solution**:
```bash
# Run Mimic manually to see errors
./mimic tests/data/test.par

# Check logs
cat tests/data/output/baseline/metadata/*.log
```

**Problem**: Output file not created

**Solution**:
- Check OutputDir in test_binary.par or test_hdf5.par
- Verify directory exists and is writable
- Check for errors in Mimic output

**Problem**: Baseline comparison fails

**Solution**:
- Check if baseline files exist in `tests/data/output/baseline/binary/` or `tests/data/output/baseline/hdf5/`
- If intentional change to physics or output: regenerate baseline by running Mimic once, then commit updated baseline files
- If unintended change: investigate why halo counts or properties differ from baseline

### Scientific Test Failures

**Problem**: Conservation law violated

**Solution**:
- Check tolerance is appropriate
- Verify physics implementation
- Check for numerical instability
- Compare to baseline (known-good run)

**Problem**: Property out of range

**Solution**:
- Check if range is too restrictive
- Verify property calculation
- Look for NaN/Inf propagation
- Check input data quality

---

## CI Integration

### GitHub Actions Workflow

**Location**: `.github/workflows/ci.yml`

**Triggers**:
- Push to main or develop
- Pull requests to main or develop

**Jobs**:
1. **test**: Build and run all tests
2. **test-hdf5**: Build with HDF5 and test
3. **code-quality**: Check code quality

**Steps**:
1. Checkout code
2. Install dependencies
3. Build Mimic
4. Check property metadata (`make check-generated`)
5. Run unit tests
6. Run integration tests
7. Check for memory leaks
8. Upload test results (if failure)

### CI Requirements

For PR to be mergeable:
- ✅ All tests pass
- ✅ No memory leaks
- ✅ Property metadata up-to-date
- ✅ Code quality checks pass

### Running CI Locally

```bash
# Simulate CI environment
make clean
make
make check-generated
make test-unit
make test-integration

# Check for memory leaks
grep -i "memory leak" tests/data/output/baseline/metadata/*.log
```

---

## Best Practices

### General

✅ **DO**:
- Write tests for new features before implementing (TDD)
- Run tests frequently during development
- Keep tests focused (one behavior per test)
- Use descriptive names and error messages
- Document expected behavior
- Check for memory leaks in all C tests

❌ **DON'T**:
- Write tests that depend on execution order
- Use hardcoded absolute paths
- Skip error checking
- Test implementation details (test behavior)
- Commit failing tests

### Test Data

✅ **DO**:
- Use realistic test data (trees_063.0)
- Keep test data small (<20M)
- Commit test data to repository
- Document test data provenance

❌ **DON'T**:
- Use production data (too large)
- Assume specific halo counts
- Hardcode file paths

### Performance

✅ **DO**:
- Keep unit tests fast (<10s total)
- Use small test data
- Run expensive tests nightly only

❌ **DON'T**:
- Add slow tests to unit tier
- Use full simulation data

---

## FAQ

### Q: How do I add a test for my new feature?

**A**:
1. Determine test tier (unit/integration/scientific)
2. Copy appropriate template
3. Implement test functions
4. Add to test runner
5. Verify with `make test`

### Q: My test fails in CI but passes locally. Why?

**A**:
- Different environment (Ubuntu vs macOS)
- Missing dependencies in CI
- Floating-point precision differences
- File path issues (use relative paths)

**Solution**: Run tests in CI-like environment (Docker)

### Q: How do I update the regression baseline?

**A**:
```bash
# Run Mimic to generate new baseline data
./mimic tests/data/test_binary.par

# Copy new output to baseline directory
cp tests/data/output/binary/model_z0.000_0 tests/data/output/baseline/binary/

# For HDF5 (if compiled with HDF5 support)
./mimic tests/data/test_hdf5.par
cp tests/data/output/hdf5/model_000.hdf5 tests/data/output/baseline/hdf5/

# Commit updated baseline files
git add tests/data/output/baseline/
git commit -m "Update regression baseline (reason: describe why baseline changed)"
```

**Note**: Baselines should only change when physics calculations change intentionally. Document why!

### Q: Can I skip tests for a quick commit?

**A**: NO. Tests are mandatory. They catch bugs before they reach production.

If tests are too slow, fix the slow tests, don't skip them.

### Q: How do I test HDF5 output?

**A**:
```bash
# Build with HDF5
make clean
make USE-HDF5=yes

# Run integration tests
make test-integration
```

### Q: What's the coverage goal?

**A**: >60% of critical paths:
- Memory management: ✓
- I/O system: ✓
- Tree processing: ✓
- Parameter parsing: ✓
- Property metadata: ✓

### Q: How often should I run tests?

**A**:
- **Every code change**: Unit tests (10s)
- **Before commit**: Integration tests (1min)
- **Before PR**: All tests (2min)
- **Before release**: Full validation + scientific tests

---

## Summary

Mimic's testing framework provides:
- **Fast feedback**: Unit tests run in seconds
- **Comprehensive coverage**: 11 tests covering core functionality and modules
- **Scientific validation**: Physics correctness verified
- **Baseline comparison**: Direct data validation instead of checksum-based regression testing
- **CI integration**: Automated testing on every commit
- **Developer-friendly**: Templates and clear documentation

**Current test coverage** (Phase 3):
- 6 unit tests (memory, properties, parameters, trees, numerics, modules)
- 3 integration tests (pipeline, output formats with baseline comparison, module workflows)
- 2 scientific tests (physics sanity, property ranges)

**Testing is not optional**. It catches bugs early, prevents regressions, and ensures scientific accuracy.

**Next steps**:
- Familiarize yourself with test structure
- Run `make test` to see tests in action
- Add tests for your features using templates
- Keep tests passing at all times

---

**For questions or issues**, see:
- Test README: `tests/README.md`
- Architecture docs: `docs/architecture/`
- CI logs: GitHub Actions tab

**Last Updated**: 2025-11-09 (Phase 3 Complete)
