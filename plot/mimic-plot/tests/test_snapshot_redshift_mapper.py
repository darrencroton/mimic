#!/usr/bin/env python3
"""Unit tests for snapshot scale-factor validation."""

import os
import sys
import tempfile

parent_dir = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
repo_root = os.path.dirname(os.path.dirname(parent_dir))
sys.path.insert(0, parent_dir)
sys.path.insert(0, os.path.join(repo_root, "tests"))

from framework import run_test_suite
from snapshot_redshift_mapper import read_expansion_factors


def write_a_list(contents):
    handle = tempfile.NamedTemporaryFile("w", delete=False)
    try:
        handle.write(contents)
        return handle.name
    finally:
        handle.close()


def assert_rejects(contents, expected_message):
    path = write_a_list(contents)
    try:
        try:
            read_expansion_factors(path)
        except ValueError as exc:
            assert expected_message in str(exc), str(exc)
        else:
            raise AssertionError("Expected ValueError")
    finally:
        os.unlink(path)


def test_read_expansion_factors_accepts_valid_file():
    path = write_a_list("# comment\n0.1\n0.2  # inline comment\n0.3\n")
    try:
        assert read_expansion_factors(path) == [0.1, 0.2, 0.3]
    finally:
        os.unlink(path)


def test_read_expansion_factors_rejects_nan():
    assert_rejects("0.1\nnan\n0.3\n", "must be finite")


def test_read_expansion_factors_rejects_inf():
    assert_rejects("0.1\ninf\n", "must be finite")


def test_read_expansion_factors_rejects_empty_file():
    assert_rejects("# comment only\n\n", "is empty")


def test_read_expansion_factors_rejects_non_positive():
    assert_rejects("0.1\n0.0\n", "must be positive")


def test_read_expansion_factors_rejects_non_increasing():
    assert_rejects("0.1\n0.1\n", "strictly increasing")


def main():
    """Run this file's tests via the shared framework runner."""
    return run_test_suite(
        [
            test_read_expansion_factors_accepts_valid_file,
            test_read_expansion_factors_rejects_nan,
            test_read_expansion_factors_rejects_inf,
            test_read_expansion_factors_rejects_empty_file,
            test_read_expansion_factors_rejects_non_positive,
            test_read_expansion_factors_rejects_non_increasing,
        ],
        "Snapshot Scale-Factor Validation (test_snapshot_redshift_mapper.py)",
    )


if __name__ == "__main__":
    sys.exit(main())
