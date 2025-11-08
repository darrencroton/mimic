#!/usr/bin/env python3
"""
[TEST NAME] - Integration Test

Validates: [SPECIFIC REQUIREMENT OR INTEGRATION BEHAVIOR]
Phase: [ROADMAP PHASE - e.g., Phase 2, Phase 3, etc.]

This test validates [detailed explanation of what is being tested and why].
It executes the full Mimic pipeline and verifies [what is checked].

Test cases:
  - test_[specific_behavior_1]: [Description]
  - test_[specific_behavior_2]: [Description]
  - test_[specific_behavior_3]: [Description]

Author: [YOUR NAME]
Date: [DATE]
"""

import subprocess
import sys
from pathlib import Path
import numpy as np

# Add output/mimic-plot to path for data loading utilities
sys.path.insert(0, str(Path(__file__).parent.parent.parent / "output" / "mimic-plot"))

# You may need to import plotting utilities for data loading
# from mimic_plot import load_binary_data, load_hdf5_data


def run_mimic(param_file, cwd=None):
    """
    Execute Mimic with specified parameter file

    Args:
        param_file (str): Path to parameter file
        cwd (str): Working directory for execution (default: repo root)

    Returns:
        tuple: (returncode, stdout, stderr)

    Example:
        returncode, stdout, stderr = run_mimic("tests/data/test.par")
        assert returncode == 0, f"Mimic failed: {stderr}"
    """
    if cwd is None:
        cwd = Path(__file__).parent.parent.parent  # Repo root

    mimic_exe = cwd / "mimic"
    if not mimic_exe.exists():
        raise FileNotFoundError(f"Mimic executable not found at {mimic_exe}")

    result = subprocess.run(
        [str(mimic_exe), param_file],
        cwd=str(cwd),
        capture_output=True,
        text=True
    )

    return result.returncode, result.stdout, result.stderr


def load_binary_output(output_file):
    """
    Load halo data from binary output file

    Args:
        output_file (str): Path to binary output file

    Returns:
        np.ndarray: Structured array with halo properties

    TODO: Implement using existing plotting infrastructure
    See output/mimic-plot/mimic_plot.py for reference
    """
    # Implementation depends on binary format
    # Use code from mimic-plot.py
    raise NotImplementedError("Load binary data using mimic-plot utilities")


def load_hdf5_output(output_file):
    """
    Load halo data from HDF5 output file

    Args:
        output_file (str): Path to HDF5 output file

    Returns:
        np.ndarray: Structured array with halo properties

    TODO: Implement using existing plotting infrastructure
    See output/mimic-plot/hdf5_reader.py for reference
    """
    # Implementation depends on HDF5 structure
    # Use code from hdf5_reader.py
    raise NotImplementedError("Load HDF5 data using existing utilities")


def check_no_memory_leaks(log_dir):
    """
    Check that Mimic run had no memory leaks

    Args:
        log_dir (str): Directory containing log files

    Returns:
        bool: True if no leaks, False if leaks detected

    Example:
        assert check_no_memory_leaks("tests/data/expected/test/metadata/")
    """
    log_dir = Path(log_dir)
    if not log_dir.exists():
        print(f"Warning: Log directory not found: {log_dir}")
        return True

    for log_file in log_dir.glob("*.log"):
        with open(log_file) as f:
            content = f.read().lower()
            if "memory leak" in content:
                print(f"Memory leak detected in {log_file}")
                return False

    return True


def test_basic_integration():
    """
    [ONE-LINE DESCRIPTION OF WHAT THIS TEST DOES]

    Expected: [WHAT SHOULD HAPPEN]
    Validates: [WHAT THIS PREVENTS/ENSURES]

    Detailed explanation of what this test verifies...
    """
    print("Testing [basic integration]...")

    # ===== SETUP =====
    # Prepare test environment
    param_file = "tests/data/test.par"
    output_dir = Path("tests/data/expected/test/")

    # Clean previous test output if needed
    # if output_dir.exists():
    #     shutil.rmtree(output_dir)

    # ===== EXECUTE =====
    # Run Mimic pipeline
    returncode, stdout, stderr = run_mimic(param_file)

    # ===== VALIDATE =====
    # Check execution success
    assert returncode == 0, f"Mimic execution failed with code {returncode}\nSTDERR: {stderr}"

    # Check no memory leaks
    assert check_no_memory_leaks(output_dir / "metadata"), "Memory leak detected"

    # Check output files exist
    # expected_output = output_dir / "model_063.dat"
    # assert expected_output.exists(), f"Output file not created: {expected_output}"

    # Load and validate output
    # halos = load_binary_output(expected_output)
    # assert len(halos) > 0, "No halos in output"
    # assert np.all(halos['Mvir'] > 0), "Invalid Mvir values"

    print("✓ Basic integration test passed")


def test_edge_case_integration():
    """
    [DESCRIPTION OF EDGE CASE BEING TESTED]

    Expected: [WHAT SHOULD HAPPEN]
    Validates: [PROPER HANDLING OF EDGE CASE]
    """
    print("Testing [edge case]...")

    # ===== SETUP =====
    # Set up edge case scenario

    # ===== EXECUTE =====
    # Run with edge case configuration

    # ===== VALIDATE =====
    # Verify edge case handled correctly

    print("✓ Edge case test passed")


def test_error_handling_integration():
    """
    [DESCRIPTION OF ERROR CONDITION BEING TESTED]

    Expected: [WHAT SHOULD HAPPEN ON ERROR]
    Validates: [PROPER ERROR HANDLING]
    """
    print("Testing [error handling]...")

    # ===== SETUP =====
    # Create error condition

    # ===== EXECUTE =====
    # Run with invalid configuration

    # ===== VALIDATE =====
    # Verify error detected and handled gracefully
    # returncode, stdout, stderr = run_mimic("invalid.par")
    # assert returncode != 0, "Should fail with invalid config"
    # assert "error" in stderr.lower(), "Should report error"

    print("✓ Error handling test passed")


def main():
    """
    Main test runner

    Executes all test cases and reports results.
    Can be run directly or via pytest.
    """
    print("=" * 60)
    print("Integration Test Suite: [TEST SUITE NAME]")
    print("=" * 60)
    print()

    tests = [
        test_basic_integration,
        test_edge_case_integration,
        test_error_handling_integration,
    ]

    passed = 0
    failed = 0

    for test in tests:
        try:
            test()
            passed += 1
        except AssertionError as e:
            print(f"✗ FAIL: {test.__name__}")
            print(f"  {e}")
            failed += 1
        except Exception as e:
            print(f"✗ ERROR: {test.__name__}")
            print(f"  {e}")
            failed += 1

    print()
    print("=" * 60)
    print("Test Summary")
    print("=" * 60)
    print(f"Passed: {passed}")
    print(f"Failed: {failed}")
    print(f"Total:  {passed + failed}")
    print("=" * 60)

    if failed == 0:
        print("✓ All tests passed!")
        return 0
    else:
        print(f"✗ {failed} test(s) failed")
        return 1


if __name__ == "__main__":
    sys.exit(main())


"""
TEMPLATE USAGE INSTRUCTIONS:
============================

1. Copy this template to tests/integration/test_yourname.py

2. Update the file header:
   - Change test name
   - Fill in description of what is validated
   - Note the roadmap phase
   - Add your name and date

3. Implement test functions:
   - Follow setup → execute → validate structure
   - Test full pipeline execution
   - Validate output files created and correct
   - Check for memory leaks
   - Use descriptive test function names

4. Implement data loading:
   - Use existing utilities from output/mimic-plot/
   - load_binary_output() or load_hdf5_output()
   - Adapt to actual data formats

5. Add test to pytest discovery:
   - Tests automatically discovered by pytest
   - Or run directly: python test_yourname.py

6. Verify test works:
   - Run: python test_yourname.py
   - Or: pytest test_yourname.py -v
   - Or: make test-integration

PYTEST COMPATIBILITY:
====================
This template works both standalone and with pytest:
- Standalone: python test_yourname.py
- Pytest: pytest test_yourname.py -v
- Pytest discovers test_* functions automatically

GUIDELINES:
===========
- Test end-to-end pipeline execution
- Validate output files exist and have correct format
- Check for memory leaks in all tests
- Use clear, descriptive error messages
- Test both success and failure paths
- Keep test data small and fast (<10 seconds)
- Document expected behavior clearly
"""
