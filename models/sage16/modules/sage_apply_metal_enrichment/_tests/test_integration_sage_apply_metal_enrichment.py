#!/usr/bin/env python3
"""Integration checks for the sage_apply_metal_enrichment module.

Validates pipeline integration and data flow: the module loads, reads its parameters,
runs without leaks, and exposes its output properties. The module is run standalone
(no star-formation producer), so NewStellarMass stays zero and no metals are applied;
the yield physics is covered by the unit test. Scientific correctness is covered by the
SAGE scientific tier.
"""

import shutil
import sys
from pathlib import Path

import numpy as np

REPO_ROOT = Path(__file__).resolve().parents[5]
sys.path.insert(0, str(REPO_ROOT / "tests"))

from framework import (
    MIMIC_EXE,
    TestSkipped,
    create_test_param_file,
    load_binary_halos,
    run_mimic,
    run_test_suite,
)

# Standalone pipeline: the ordering checks in init() are skipped because neither the
# SF/SN apply step nor starburst feedback is configured alongside this module.
PHASE_CONFIG = {
    "pre_timestep": [],
    "galaxy_physics": [("sage_apply_metal_enrichment", "process_by_galaxy")],
    "post_timestep": [],
}
MODEL_PARAMS = {"Yield": 0.025, "FracZleaveDisk": 0.0}


def _run(output_name, **kwargs):
    """Run the module standalone and return (returncode, stdout, stderr, output_dir, temp_dir)."""
    param_file, output_dir, temp_dir = create_test_param_file(
        output_name=output_name,
        phase_config=PHASE_CONFIG,
        model_params=MODEL_PARAMS,
        **kwargs,
    )
    returncode, stdout, stderr = run_mimic(param_file)
    return returncode, stdout, stderr, output_dir, temp_dir


def test_module_loads():
    if not MIMIC_EXE.exists():
        raise TestSkipped("Mimic not built")
    returncode, stdout, _, _, temp_dir = _run("metal_enrichment_load")
    try:
        assert returncode == 0, "module should load and run"
        assert "SAGE metal enrichment module initialized" in stdout
        assert "SAGE metal enrichment module cleaned up" in stdout
    finally:
        shutil.rmtree(temp_dir)


def test_parameter_configuration():
    if not MIMIC_EXE.exists():
        raise TestSkipped("Mimic not built")
    returncode, stdout, _, _, temp_dir = _run("metal_enrichment_params")
    try:
        assert returncode == 0, "execution with custom parameters should succeed"
        assert "Yield = 0.0250" in stdout, "custom Yield should be read and logged"
        assert "FracZleaveDisk = 0.000" in stdout, "custom FracZleaveDisk should be read and logged"
    finally:
        shutil.rmtree(temp_dir)


def test_no_memory_leaks():
    if not MIMIC_EXE.exists():
        raise TestSkipped("Mimic not built")
    returncode, stdout, stderr, _, temp_dir = _run("metal_enrichment_memory")
    try:
        assert returncode == 0, "execution should succeed"
        assert "Memory leak detected" not in stdout + stderr, "should not leak memory"
    finally:
        shutil.rmtree(temp_dir)


def test_output_properties_exist():
    if not MIMIC_EXE.exists():
        raise TestSkipped("Mimic not built")
    returncode, _, stderr, output_dir, temp_dir = _run(
        "metal_enrichment_properties", output_format="binary", first_file=0, last_file=0
    )
    try:
        assert returncode == 0, f"Mimic should execute successfully\n{stderr}"
        halos, _ = load_binary_halos(str(output_dir / "model_z0.000_0"))
        for prop in ("MetalsColdGas", "MetalsHotGas", "ColdGas", "Type"):
            assert prop in halos.dtype.names, f"property '{prop}' should exist in output"
        assert halos["MetalsColdGas"].dtype == np.float32, "MetalsColdGas should be float32"
    finally:
        shutil.rmtree(temp_dir)


def main():
    return run_test_suite(
        [
            test_module_loads,
            test_parameter_configuration,
            test_no_memory_leaks,
            test_output_properties_exist,
        ],
        "sage_apply_metal_enrichment Integration",
    )


if __name__ == "__main__":
    sys.exit(main())
