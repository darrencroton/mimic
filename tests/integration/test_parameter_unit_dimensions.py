#!/usr/bin/env python3
"""
Integration tests for the run-file scalar-parameter dimension guard.

convert_unit_scalar() (src/core/read_parameter_file.c) now rejects a
source/reference dimension mismatch before computing any conversion factor.
These tests exercise that guard through the real run-file loader rather than
at the generator level, since the two existing callers -- simulation.box_size
and simulation.particle_mass -- are parsed there.
"""

import shutil
import sys
import tempfile
from pathlib import Path

import yaml

sys.path.insert(0, str(Path(__file__).parent.parent))

from framework import MIMIC_EXE, TestSkipped, create_test_param_file, run_mimic, run_test_suite

TEMP_DIR = None


def _make_param_file(name, box_size_override):
    """Return a physics-free run file whose simulation.box_size is overridden."""
    param_file, _output_dir, _ = create_test_param_file(
        output_name=name,
        first_file=0,
        last_file=0,
        temp_dir=TEMP_DIR,
    )
    with open(param_file, "r") as handle:
        config = yaml.safe_load(handle)

    config.setdefault("simulation", {})["box_size"] = box_size_override

    rewritten = Path(TEMP_DIR) / f"{name}.yaml"
    with open(rewritten, "w") as handle:
        yaml.safe_dump(config, handle, default_flow_style=False, sort_keys=False)
    return rewritten


def test_box_size_with_dimensionally_wrong_units_is_rejected():
    """
    Test that a dimensionally-wrong units label for simulation.box_size fails at load.

    Expected: Non-zero exit; the message names the field (simulation.box_size) and
              both dimensions ('mass' from the declared units, 'length' from the
              Mpc/h reference), rather than silently mis-scaling the value.
    Validates: convert_unit_scalar()'s new dimension guard fires before any factor
               arithmetic for the source/reference mismatch case.
    """
    if not MIMIC_EXE.exists():
        raise TestSkipped("Mimic not built")

    # "1e10 Msun/h" is registered with dimension 'mass'; simulation.box_size's
    # reference label is 'Mpc/h', dimension 'length'. Declaring a mass unit for a
    # length field is the dimensionally-wrong case this guard exists to catch.
    param_file = _make_param_file(
        "box_size_wrong_dimension",
        {"value": 100.0, "units": "1e10 Msun/h", "h_convention": "carried"},
    )
    returncode, stdout, stderr = run_mimic(param_file)
    output = stdout + stderr

    assert returncode != 0, f"a dimensionally-wrong box_size units should be rejected:\n{output}"
    assert "simulation.box_size" in output, f"the failure should name the field:\n{output}"
    assert "'mass'" in output, f"the failure should name the declared units' dimension:\n{output}"
    assert "'length'" in output, f"the failure should name the reference dimension:\n{output}"


def test_box_size_with_correct_dimension_still_runs():
    """
    Test that a dimensionally-correct, differently-labelled units for
    simulation.box_size still passes config load and runs.

    Expected: exit 0.
    Validates: the new guard is a dimension check, not a label-identity check --
               'kpc/h' (dimension 'length', same as the 'Mpc/h' reference) is
               accepted and converted normally.
    """
    if not MIMIC_EXE.exists():
        raise TestSkipped("Mimic not built")

    # 62500 kpc/h == 62.5 Mpc/h, the same box size the reference test run file
    # already declares directly -- this exercises a different, but dimensionally
    # matching, units label rather than merely re-declaring the reference label.
    param_file = _make_param_file(
        "box_size_correct_dimension",
        {"value": 62500.0, "units": "kpc/h", "h_convention": "carried"},
    )
    returncode, stdout, stderr = run_mimic(param_file)
    output = stdout + stderr

    assert returncode == 0, f"a dimensionally-correct box_size units should run:\n{output}"


def main():
    global TEMP_DIR
    TEMP_DIR = Path(tempfile.mkdtemp(prefix="mimic_parameter_unit_dimensions_"))
    try:
        tests = [
            test_box_size_with_dimensionally_wrong_units_is_rejected,
            test_box_size_with_correct_dimension_still_runs,
        ]
        return run_test_suite(
            tests, "Parameter Unit Dimensions (test_parameter_unit_dimensions.py)"
        )
    finally:
        shutil.rmtree(TEMP_DIR)


if __name__ == "__main__":
    sys.exit(main())
