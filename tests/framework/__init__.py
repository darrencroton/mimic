"""
Test Framework for Mimic

Shared utilities for testing Mimic functionality.
"""

from .comparison import compare_halos_comprehensive
from .data_loader import (
    assert_hdf5_schema_layout,
    find_nonfinite,
    get_halo_dtype,
    load_binary_halos,
    load_hdf5_halos,
    validate_no_infs,
    validate_no_nans,
    validate_range,
)
from .harness import (
    BASELINE_ATOL_DEFAULT,
    BASELINE_RTOL_DEFAULT,
    MIMIC_EXE,
    REPO_ROOT,
    TEST_DATA_DIR,
    baseline_rtol,
    check_no_memory_leaks,
    compiled_model,
    compiled_simulation,
    core_input_file,
    create_test_param_file,
    default_model,
    default_simulation,
    ensure_output_dirs,
    input_tree_file_for_run,
    is_default_baseline_combo,
    parse_test_fixture_executions,
    read_param_file,
    resolve_sim_config_path,
    run_mimic,
    run_mimic_fresh,
    simulation_input_file,
    skip_non_default_baseline,
)
from .markers import (
    TestSkipped,
    result_error,
    result_fail,
    result_pass,
    result_skip,
    result_warn,
)
from .runner import BLUE, GREEN, NC, RED, YELLOW, run_test_suite

__all__ = [
    # Data loading, validation, and comparison
    "load_binary_halos",
    "load_hdf5_halos",
    "assert_hdf5_schema_layout",
    "compare_halos_comprehensive",
    "get_halo_dtype",
    "validate_no_nans",
    "validate_no_infs",
    "validate_range",
    "find_nonfinite",
    # Test harness utilities
    "REPO_ROOT",
    "TEST_DATA_DIR",
    "MIMIC_EXE",
    "compiled_model",
    "compiled_simulation",
    "default_model",
    "default_simulation",
    "is_default_baseline_combo",
    "skip_non_default_baseline",
    "baseline_rtol",
    "BASELINE_RTOL_DEFAULT",
    "BASELINE_ATOL_DEFAULT",
    "core_input_file",
    "simulation_input_file",
    "ensure_output_dirs",
    "run_mimic",
    "run_mimic_fresh",
    "resolve_sim_config_path",
    "input_tree_file_for_run",
    "read_param_file",
    "create_test_param_file",
    "check_no_memory_leaks",
    "parse_test_fixture_executions",
    # Summary markers
    "TestSkipped",
    "result_pass",
    "result_fail",
    "result_skip",
    "result_warn",
    "result_error",
    # Suite runner and shared output colors
    "run_test_suite",
    "BLUE",
    "GREEN",
    "RED",
    "YELLOW",
    "NC",
]
