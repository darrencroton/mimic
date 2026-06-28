#!/usr/bin/env python3
"""Unit tests for output_utils validation helpers.

Covers validate_filtered_data(), validate_evolution_snapshot(), and check_field_has_values().
"""

import os
import sys

import numpy as np

parent_dir = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
repo_root = os.path.dirname(os.path.dirname(parent_dir))
sys.path.insert(0, parent_dir)
sys.path.insert(0, os.path.join(repo_root, "tests"))

from framework import run_test_suite
from output_utils import (
    check_field_has_values,
    validate_evolution_snapshot,
    validate_filtered_data,
)


def test_validate_filtered_data_with_data():
    """Test that validation passes when data exists."""
    indices = np.array([0, 1, 2, 3])
    is_valid, skip_msg = validate_filtered_data(indices, "Test Plot", verbose=False)

    assert is_valid, "Should be valid with data"
    assert skip_msg is None, "Skip message should be None when valid"


def test_validate_filtered_data_empty():
    """Test that validation fails when no data."""
    indices = np.array([])
    is_valid, skip_msg = validate_filtered_data(indices, "Test Plot", verbose=False)

    assert not is_valid, "Should be invalid with no data"
    assert skip_msg is not None, "Skip message should be provided"
    assert "Test Plot" in skip_msg, "Skip message should mention plot name"


def test_validate_filtered_data_verbose():
    """Test verbose mode prints warnings."""
    indices = np.array([])
    is_valid, skip_msg = validate_filtered_data(indices, "Test Plot", verbose=True)

    assert not is_valid, "Should be invalid"
    assert skip_msg is not None, "Skip message should be provided"


def test_validate_evolution_snapshot_with_data():
    """Test evolution validation with data."""
    indices = np.array([0, 1, 2])
    is_valid, skip_msg = validate_evolution_snapshot(indices, 1.5, "Test Evolution", verbose=False)

    assert is_valid, "Should be valid with data"
    assert skip_msg is None, "Skip message should be None when valid"


def test_validate_evolution_snapshot_empty():
    """Test evolution validation without data."""
    indices = np.array([])
    is_valid, skip_msg = validate_evolution_snapshot(indices, 2.0, "Test Evolution", verbose=False)

    assert not is_valid, "Should be invalid with no data"
    assert skip_msg is not None, "Skip message should be provided"
    assert "z=2.0" in skip_msg, "Skip message should mention redshift"
    assert "Test Evolution" in skip_msg, "Skip message should mention plot name"


def test_validate_evolution_snapshot_verbose():
    """Test evolution validation verbose mode."""
    indices = np.array([])
    is_valid, skip_msg = validate_evolution_snapshot(indices, 1.0, "Test Evolution", verbose=True)

    assert not is_valid, "Should be invalid"
    assert skip_msg is not None, "Skip message should be provided"


def test_check_field_has_values_all_nonzero():
    """Test field validation with all nonzero values."""
    data = np.array([1.0, 2.0, 3.0, 4.0])
    has_values, count, msg = check_field_has_values(data, "TestField")

    assert has_values, "Should have values"
    assert count == 4, f"Should count 4 values, got {count}"
    assert msg == "", "Message should be empty when valid"


def test_check_field_has_values_all_zero():
    """Test field validation with all zero values."""
    data = np.array([0.0, 0.0, 0.0])
    has_values, count, msg = check_field_has_values(data, "TestField")

    assert not has_values, "Should not have values"
    assert count == 0, f"Should count 0 values, got {count}"
    assert "TestField" in msg, "Message should mention field name"
    assert "0.0" in msg or "0" in msg, "Message should mention threshold"


def test_check_field_has_values_mixed():
    """Test field validation with mixed values."""
    data = np.array([0.0, 1.0, 0.0, 2.0])
    has_values, count, msg = check_field_has_values(data, "TestField")

    assert has_values, "Should have values"
    assert count == 2, f"Should count 2 nonzero values, got {count}"
    assert msg == "", "Message should be empty when valid"


def test_check_field_has_values_custom_threshold():
    """Test field validation with custom threshold."""
    data = np.array([0.0, 0.5, 1.0, 1.5])
    has_values, count, msg = check_field_has_values(data, "TestField", threshold=1.0)

    assert has_values, "Should have values above threshold"
    assert count == 1, f"Should count 1 value > 1.0, got {count}"


def test_check_field_has_values_below_threshold():
    """Test field validation when all values below threshold."""
    data = np.array([0.1, 0.2, 0.3])
    has_values, count, msg = check_field_has_values(data, "TestField", threshold=1.0)

    assert not has_values, "Should not have values above threshold"
    assert count == 0, f"Should count 0 values, got {count}"
    assert "TestField" in msg, "Message should mention field name"
    assert "1.0" in msg, "Message should mention threshold"


def test_check_field_has_values_large_array():
    """Test field validation with large array."""
    data = np.zeros(100000)
    data[50000:50010] = 1.0  # 10 nonzero values

    has_values, count, msg = check_field_has_values(data, "LargeField")

    assert has_values, "Should detect nonzero values"
    assert count == 10, f"Should count 10 values, got {count}"


def test_check_field_has_values_negative_values():
    """Test field validation with negative values."""
    data = np.array([-1.0, -2.0, 0.0, 1.0])
    has_values, count, msg = check_field_has_values(data, "TestField", threshold=0.0)

    assert has_values, "Should count positive values"
    assert count == 1, f"Should count 1 value > 0, got {count}"


def main():
    """Run this file's tests via the shared framework runner."""
    return run_test_suite(
        [
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
        ],
        "Plot Validation Helpers (test_validation_helpers.py)",
    )


if __name__ == "__main__":
    sys.exit(main())
