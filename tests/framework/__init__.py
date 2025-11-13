"""
Test Framework for Mimic

Shared utilities for testing Mimic functionality.
"""

from .data_loader import (
    load_binary_halos,
    get_halo_dtype,
    validate_no_nans,
    validate_no_infs,
    validate_range,
)

from .harness import (
    REPO_ROOT,
    TEST_DATA_DIR,
    MIMIC_EXE,
    ensure_output_dirs,
    run_mimic,
    read_param_file,
    create_test_param_file,
    check_no_memory_leaks,
)

__all__ = [
    # Data loading and validation
    'load_binary_halos',
    'get_halo_dtype',
    'validate_no_nans',
    'validate_no_infs',
    'validate_range',
    # Test harness utilities
    'REPO_ROOT',
    'TEST_DATA_DIR',
    'MIMIC_EXE',
    'ensure_output_dirs',
    'run_mimic',
    'read_param_file',
    'create_test_param_file',
    'check_no_memory_leaks',
]
