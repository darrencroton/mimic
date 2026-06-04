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

from .tree_loader import (
    load_binary_tree,
    get_tree_dtype,
    get_halos_by_snapshot,
)

from .harness import (
    REPO_ROOT,
    TEST_DATA_DIR,
    MIMIC_EXE,
    compiled_model,
    model_input_file,
    ensure_output_dirs,
    run_mimic,
    run_mimic_fresh,
    resolve_sim_config_path,
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
    # Tree loading
    'load_binary_tree',
    'get_tree_dtype',
    'get_halos_by_snapshot',
    # Test harness utilities
    'REPO_ROOT',
    'TEST_DATA_DIR',
    'MIMIC_EXE',
    'compiled_model',
    'model_input_file',
    'ensure_output_dirs',
    'run_mimic',
    'run_mimic_fresh',
    'resolve_sim_config_path',
    'read_param_file',
    'create_test_param_file',
    'check_no_memory_leaks',
]
