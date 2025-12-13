#!/usr/bin/env python3
"""
Unit tests for validation helper functions in output_utils.py

Tests the new validation helpers added for skipping empty plots:
- validate_filtered_data()
- validate_evolution_snapshot()
- check_field_has_values()
"""

import numpy as np
import sys
import os

# Add parent directory to path for imports
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from output_utils import (
    validate_filtered_data,
    validate_evolution_snapshot,
    check_field_has_values,
)


def test_validate_filtered_data_with_data():
    """Test that validation passes when data exists."""
    print("Testing validate_filtered_data with valid data...")
    indices = np.array([0, 1, 2, 3])
    is_valid, skip_msg = validate_filtered_data(indices, "Test Plot", verbose=False)

    assert is_valid == True, "Should be valid with data"
    assert skip_msg is None, "Skip message should be None when valid"
    print("  ✅ PASSED")


def test_validate_filtered_data_empty():
    """Test that validation fails when no data."""
    print("Testing validate_filtered_data with empty data...")
    indices = np.array([])
    is_valid, skip_msg = validate_filtered_data(indices, "Test Plot", verbose=False)

    assert is_valid == False, "Should be invalid with no data"
    assert skip_msg is not None, "Skip message should be provided"
    assert "Test Plot" in skip_msg, "Skip message should mention plot name"
    print("  ✅ PASSED")


def test_validate_filtered_data_verbose():
    """Test verbose mode prints warnings."""
    print("Testing validate_filtered_data verbose mode...")
    indices = np.array([])
    # Note: verbose=True will print warning to stdout
    is_valid, skip_msg = validate_filtered_data(indices, "Test Plot", verbose=True)

    assert is_valid == False, "Should be invalid"
    assert skip_msg is not None, "Skip message should be provided"
    print("  ✅ PASSED")


def test_validate_evolution_snapshot_with_data():
    """Test evolution validation with data."""
    print("Testing validate_evolution_snapshot with valid data...")
    indices = np.array([0, 1, 2])
    is_valid, skip_msg = validate_evolution_snapshot(
        indices, 1.5, "Test Evolution", verbose=False
    )

    assert is_valid == True, "Should be valid with data"
    assert skip_msg is None, "Skip message should be None when valid"
    print("  ✅ PASSED")


def test_validate_evolution_snapshot_empty():
    """Test evolution validation without data."""
    print("Testing validate_evolution_snapshot with empty data...")
    indices = np.array([])
    is_valid, skip_msg = validate_evolution_snapshot(
        indices, 2.0, "Test Evolution", verbose=False
    )

    assert is_valid == False, "Should be invalid with no data"
    assert skip_msg is not None, "Skip message should be provided"
    assert "z=2.0" in skip_msg, "Skip message should mention redshift"
    assert "Test Evolution" in skip_msg, "Skip message should mention plot name"
    print("  ✅ PASSED")


def test_validate_evolution_snapshot_verbose():
    """Test evolution validation verbose mode."""
    print("Testing validate_evolution_snapshot verbose mode...")
    indices = np.array([])
    is_valid, skip_msg = validate_evolution_snapshot(
        indices, 1.0, "Test Evolution", verbose=True
    )

    assert is_valid == False, "Should be invalid"
    assert skip_msg is not None, "Skip message should be provided"
    print("  ✅ PASSED")


def test_check_field_has_values_all_nonzero():
    """Test field validation with all nonzero values."""
    print("Testing check_field_has_values with all nonzero...")
    data = np.array([1.0, 2.0, 3.0, 4.0])
    has_values, count, msg = check_field_has_values(data, "TestField")

    assert has_values == True, "Should have values"
    assert count == 4, f"Should count 4 values, got {count}"
    assert msg == "", "Message should be empty when valid"
    print("  ✅ PASSED")


def test_check_field_has_values_all_zero():
    """Test field validation with all zero values."""
    print("Testing check_field_has_values with all zeros...")
    data = np.array([0.0, 0.0, 0.0])
    has_values, count, msg = check_field_has_values(data, "TestField")

    assert has_values == False, "Should not have values"
    assert count == 0, f"Should count 0 values, got {count}"
    assert "TestField" in msg, "Message should mention field name"
    assert "0.0" in msg or "0" in msg, "Message should mention threshold"
    print("  ✅ PASSED")


def test_check_field_has_values_mixed():
    """Test field validation with mixed values."""
    print("Testing check_field_has_values with mixed values...")
    data = np.array([0.0, 1.0, 0.0, 2.0])
    has_values, count, msg = check_field_has_values(data, "TestField")

    assert has_values == True, "Should have values"
    assert count == 2, f"Should count 2 nonzero values, got {count}"
    assert msg == "", "Message should be empty when valid"
    print("  ✅ PASSED")


def test_check_field_has_values_custom_threshold():
    """Test field validation with custom threshold."""
    print("Testing check_field_has_values with custom threshold...")
    data = np.array([0.0, 0.5, 1.0, 1.5])
    has_values, count, msg = check_field_has_values(data, "TestField", threshold=1.0)

    assert has_values == True, "Should have values above threshold"
    assert count == 1, f"Should count 1 value > 1.0, got {count}"
    print("  ✅ PASSED")


def test_check_field_has_values_below_threshold():
    """Test field validation when all values below threshold."""
    print("Testing check_field_has_values all below threshold...")
    data = np.array([0.1, 0.2, 0.3])
    has_values, count, msg = check_field_has_values(data, "TestField", threshold=1.0)

    assert has_values == False, "Should not have values above threshold"
    assert count == 0, f"Should count 0 values, got {count}"
    assert "TestField" in msg, "Message should mention field name"
    assert "1.0" in msg, "Message should mention threshold"
    print("  ✅ PASSED")


def test_check_field_has_values_large_array():
    """Test field validation with large array."""
    print("Testing check_field_has_values with large array...")
    # Create large array with some nonzero values
    data = np.zeros(100000)
    data[50000:50010] = 1.0  # 10 nonzero values

    has_values, count, msg = check_field_has_values(data, "LargeField")

    assert has_values == True, "Should detect nonzero values"
    assert count == 10, f"Should count 10 values, got {count}"
    print("  ✅ PASSED")


def test_check_field_has_values_negative_values():
    """Test field validation with negative values."""
    print("Testing check_field_has_values with negative values...")
    data = np.array([-1.0, -2.0, 0.0, 1.0])
    has_values, count, msg = check_field_has_values(data, "TestField", threshold=0.0)

    assert has_values == True, "Should count positive values"
    assert count == 1, f"Should count 1 value > 0, got {count}"
    print("  ✅ PASSED")


def run_all_tests():
    """Run all validation helper tests."""
    print("=" * 60)
    print("Running validation helper unit tests")
    print("=" * 60)
    print()

    tests = [
        test_validate_filtered_data_with_data,
        test_validate_filtered_data_empty,
        test_validate_filtered_data_verbose,
        test_validate_evolution_snapshot_with_data,
        test_validate_evolution_snapshot_empty,
        test_validate_evolution_snapshot_verbose,
        test_check_field_has_values_all_nonzero,
        test_check_field_has_values_all_zero,
        test_check_field_has_values_mixed,
        test_check_field_has_values_custom_threshold,
        test_check_field_has_values_below_threshold,
        test_check_field_has_values_large_array,
        test_check_field_has_values_negative_values,
    ]

    passed = 0
    failed = 0

    for test in tests:
        try:
            test()
            passed += 1
        except AssertionError as e:
            print(f"  ❌ FAILED: {e}")
            failed += 1
        except Exception as e:
            print(f"  ❌ ERROR: {e}")
            failed += 1

    print()
    print("=" * 60)
    print(f"Results: {passed} passed, {failed} failed out of {len(tests)} tests")
    print("=" * 60)

    return failed == 0


if __name__ == "__main__":
    success = run_all_tests()
    sys.exit(0 if success else 1)
