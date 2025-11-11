# Mimic Testing Guide

**Version**: 1.2 (Metadata-Driven Validation)
**Status**: Production
**Last Updated**: 2025-11-11

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
make tests

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
5. Verify: `make tests`

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
   - **Purpose**: Validate physics correctness and property ranges
   - **Runtime**: <5 minutes total
   - **Coverage**: Numerical validity (NaN/Inf), zero value warnings, physical ranges
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
│   └── test_scientific.py
├── data/                      # Test data
│   ├── input/                 # Input test data
│   │   ├── trees_063.0        # Single tree file (17M)
│   │   └── millennium.a_list  # Snapshot ages (577B)
│   ├── test_binary.par        # Binary format test parameter file
│   ├── test_hdf5.par          # HDF5 format test parameter file
│   └── output/                # Test outputs
│       ├── baseline/          # Known-good baseline outputs (committed to git)
│       │   ├── binary/        # Binary format baseline (not used for testing, kept for reference)
│       │   └── hdf5/          # HDF5 format baseline (used for core property validation)
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
- `tests/data/output/baseline/hdf5/` - Known-good HDF5 baseline (committed to git, used for core property validation)
- `tests/data/output/baseline/binary/` - Binary baseline files (committed to git, not used for testing, kept for reference)
- `tests/data/output/binary/` - Generated test outputs in binary format (gitignored)
- `tests/data/output/hdf5/` - Generated test outputs in HDF5 format (gitignored)

**Baseline Testing Strategy (Three-Tier Validation)**:

Mimic uses a three-tier approach to validate correctness while supporting schema evolution:

1. **HDF5 Baseline Comparison** - Validates core determinism
   - Tests core physics (halo tracking, virial properties) without modules
   - HDF5's self-describing format handles schema evolution (new properties added)
   - Baseline remains valid as modules/properties evolve
   - Compares halo counts and core halo properties

2. **Binary vs HDF5 Equivalence** - Validates format consistency
   - Tests that binary and HDF5 formats write identical data
   - Compares halo counts, field structure, and property values
   - Ensures both formats stay synchronized via property metadata system
   - Works regardless of which modules are enabled

3. **Module Integration Tests** - Validates module functionality
   - Tests with modules enabled (no baseline comparison)
   - Validates expected properties exist and have reasonable values
   - Ensures modules execute successfully and produce valid output

**Why No Binary Baseline?**
Binary format requires exact struct size matching - it cannot read files when the output schema changes (e.g., new properties added). Since runtime module configuration means output structure varies, a binary baseline would need regeneration for every property change, making it unmaintainable. Instead, we validate binary correctness through equivalence with HDF5, which is validated against the baseline.

---

## Running Tests

### Via Makefile (Recommended)

```bash
# All tests
make tests

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
python test_scientific.py

# Run with pytest
pytest test_scientific.py -v
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

**NEW in Phase 3**: Scientific tests are now **fully metadata-driven**. Validation rules come from YAML property definitions, not hardcoded test logic.

The scientific test validates physical correctness through three levels:

1. **Numerical Validity** (FAIL): NaN or Inf values in ALL floating-point properties
2. **Zero Values** (WARNING): Zero values in ALL numeric properties (respects sentinels)
3. **Physical Ranges** (FAIL): Properties outside ranges defined in YAML metadata

### Metadata-Driven Validation

**Key Principle**: The test automatically validates ALL output properties by reading validation rules from `tests/generated/property_ranges.json`, which is auto-generated from property YAML files.

**To add validation for a property**:
1. Edit `metadata/properties/halo_properties.yaml` or `galaxy_properties.yaml`
2. Add `range: [min, max]` for the property
3. Optionally add `sentinels: [...]` for special values to exclude
4. Run `make generate` to regenerate the validation manifest
5. Tests automatically pick up the new validation rules

**Example YAML**:
```yaml
- name: Mvir
  type: float
  units: "1e10 Msun/h"
  description: "Virial mass"
  output: true
  range: [1.0e-5, 1.0e4]  # 10^5 to 10^14 Msun/h

- name: infallMvir
  type: float
  units: "1e10 Msun/h"
  description: "Virial mass at infall (satellites only)"
  output: true
  range: [1.0e-5, 1.0e4]
  sentinels: [0.0, -1.0]  # 0.0 for centrals, -1.0 for unset
```

### Test Structure

The test is organized into three automated sections:

```python
def test_numerical_validity():
    """Check for NaN/Inf in ALL floating-point properties"""
    # Dynamically checks ALL properties with dtype.kind == 'f'
    # No hardcoded property lists
    # Returns: (passed: bool, failures: int)

def test_zero_values():
    """Check for zeros in ALL numeric properties"""
    # Dynamically checks ALL properties with dtype.kind in ('f', 'i')
    # Respects sentinels: skips properties with 0/0.0 in sentinels list
    # Returns: (passed: bool, warning_count: int)

def test_physical_ranges():
    """Check ranges for properties defined in YAML metadata"""
    # Loads tests/generated/property_ranges.json
    # Validates each property with a 'range' field
    # Excludes sentinel values from range checks
    # Returns: (passed: bool, failures: int)
```

### Key Features

- **Fully automatic**: Adapts to property changes without test code modifications
- **Metadata-driven**: Validation rules defined once in YAML, used everywhere
- **Sentinel-aware**: Respects special values (e.g., 0.0, -1.0) in all checks
- **Comprehensive**: Validates ALL output properties, not just a hardcoded subset
- **Clear output**: Shows property names, ranges, and failure counts
- **Examples shown**: Up to 5 examples for each failing property

### Adding Validation for New Properties

**Scenario**: You add a new property `HotGas` to `galaxy_properties.yaml`

**Step 1**: Define the property with validation fields
```yaml
- name: HotGas
  type: float
  units: "1e10 Msun/h"
  description: "Hot gas mass in halo atmosphere"
  output: true
  created_by: cooling_module
  init_source: default
  init_value: 0.0
  output_source: galaxy_property
  range: [0.0, 1.0e5]     # Physically reasonable range
  sentinels: [0.0]        # Zero is legitimate (no hot gas yet)
```

**Step 2**: Regenerate validation manifest
```bash
make generate
```

**Step 3**: Run tests
```bash
make test-scientific
```

The test automatically validates `HotGas` for NaN/Inf (always), zeros (respects sentinel), and range violations (checks [0.0, 1.0e5]).

### Example: Testing Conservation Law

For custom physics validation beyond basic range checking:

```python
def test_baryon_conservation():
    """
    Test that baryonic mass is conserved

    Expected: Total baryons = ColdGas + StellarMass + HotGas
    Tolerance: 1% (numerical precision + physics approximations)
    Reference: Croton et al. 2006, Eq. 15
    """
    # Load validation manifest (for reference, optional)
    with open(VALIDATION_MANIFEST_PATH) as f:
        manifest = json.load(f)

    halos = load_binary_halos("output.dat")

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
- **Add validation to YAML**, not test code (single source of truth)
- Use physically motivated ranges based on simulation and physics
- Add `sentinels` for legitimate special values (0.0, -1.0, etc.)
- Regenerate (`make generate`) after editing YAML
- Document units clearly in YAML (especially 10^10 Msun/h for masses)
- Be generous with ranges (account for edge cases and numerical precision)

✅ **NEW Metadata-Driven Approach**:
- Validation rules in YAML, not Python
- Tests automatically adapt to property changes
- No hardcoded property lists in tests
- Sentinel values declared once, respected everywhere

❌ **DON'T**:
- Hardcode validation logic in test files (use YAML instead)
- Fail tests for zero values with `sentinels: [0.0]` (these are intentional)
- Use overly tight ranges (allow for edge cases)
- Forget to regenerate after YAML changes
- Skip `range` fields for physical quantities (masses, velocities, etc.)

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

**Problem**: HDF5 baseline comparison fails

**Solution**:
- Check if baseline file exists: `tests/data/output/baseline/hdf5/model_000.hdf5`
- Verify test parameter file has NO modules enabled (physics-free mode for baseline)
- If intentional change to **core physics** (not modules): regenerate HDF5 baseline and document why
- If unintended change: investigate why halo counts or core properties differ from baseline
- **Note**: Binary format has no baseline test - binary correctness validated via equivalence with HDF5

**Problem**: Binary vs HDF5 equivalence test fails

**Solution**:
- This indicates binary and HDF5 formats are writing different data (serious bug!)
- Check property metadata system: both formats should use same generated output struct
- Verify both formats ran with same input data and same modules enabled
- Check for format-specific bugs in `src/io/output/binary.c` or `src/io/output/hdf5.c`

### Scientific Test Failures

**Problem**: NaN or Inf values found (FAIL)

**Solution**:
- Check for division by zero in calculations
- Verify input data integrity
- Check for numerical overflow/underflow
- Review recent physics changes

**Problem**: Zero values reported (WARNING)

**Solution**:
- These are informational, not failures
- Review if counts are unexpectedly high
- Check if halos should be filtered earlier
- Verify initialization of properties

**Problem**: Property out of physical range (FAIL)

**Solution**:
- Check if range is appropriate for simulation
- Verify property calculations
- Update EXPECTED_RANGES if simulation changed
- Review mass units (internal: 10^10 Msun/h)

---

## CI Integration

### Generated-code drift protection

- CI runs `make check-generated` to ensure generated files match the current YAML metadata. If they drift, CI fails with clear instructions.
- Locally, you can enable the provided pre-commit hook to catch this before pushing:

```bash
git config core.hooksPath .githooks
chmod +x .githooks/pre-commit
```

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
5. Verify with `make tests`

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
# IMPORTANT: Only HDF5 baseline needs regeneration (binary has no baseline)
# Ensure test parameter files have NO modules enabled (physics-free mode)

# Build with HDF5 support
make clean && make USE-HDF5=yes

# Run Mimic to generate new baseline data (must have NO modules enabled)
./mimic tests/data/test_hdf5.par

# Copy new output to baseline directory
cp tests/data/output/hdf5/model_000.hdf5 tests/data/output/baseline/hdf5/

# Commit updated baseline file
git add tests/data/output/baseline/hdf5/
git commit -m "Update HDF5 regression baseline (reason: describe why baseline changed)"
```

**Important Notes**:
- **Binary format has NO baseline** - binary correctness validated via equivalence with HDF5
- **Baseline must be physics-free** - test parameter files must have no `EnabledModules` parameter (or empty)
- **Baselines validate core only** - halo tracking and virial properties without module physics
- **Schema evolution safe** - HDF5 baseline remains valid even when properties are added to output struct
- **Document changes** - Baselines should only change when core physics calculations change intentionally

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
- 1 scientific test (comprehensive validation: numerical validity, zero warnings, physical ranges)

**Testing is not optional**. It catches bugs early, prevents regressions, and ensures scientific accuracy.

**Next steps**:
- Familiarize yourself with test structure
- Run `make tests` to see tests in action
- Add tests for your features using templates
- Keep tests passing at all times

---

**For questions or issues**, see:
- Test README: `tests/README.md`
- Architecture docs: `docs/architecture/`
- CI logs: GitHub Actions tab

**Last Updated**: 2025-11-10 (Phase 3 Complete - Scientific tests consolidated)
