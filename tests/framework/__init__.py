"""
Test Framework for Mimic

Shared utilities for testing Mimic functionality.
"""

from .data_loader import (
    get_halo_dtype,
    load_binary_halos,
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
    ensure_output_dirs,
    read_param_file,
    resolve_sim_config_path,
    run_mimic,
    run_mimic_fresh,
    simulation_input_file,
)

__all__ = [
    # Data loading and validation
    "load_binary_halos",
    "get_halo_dtype",
    "validate_no_nans",
    "validate_no_infs",
    "validate_range",
    # Test harness utilities
    "REPO_ROOT",
    "TEST_DATA_DIR",
    "MIMIC_EXE",
    "compiled_model",
    "compiled_simulation",
    "baseline_rtol",
    "BASELINE_RTOL_DEFAULT",
    "BASELINE_ATOL_DEFAULT",
    "core_input_file",
    "simulation_input_file",
    "ensure_output_dirs",
    "run_mimic",
    "run_mimic_fresh",
    "resolve_sim_config_path",
    "read_param_file",
    "create_test_param_file",
    "check_no_memory_leaks",
]
