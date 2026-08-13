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

from framework import (
    MIMIC_EXE,
    TestSkipped,
    create_test_param_file,
    resolve_sim_config_path,
    run_mimic,
    run_test_suite,
)

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

    This test runs under every MODEL/SIMULATION pair the core integration tier is
    invoked with (scripts/generate_test_registry.py globs every test_*.py file into
    every pair), so the injected box_size cannot be a literal tied to one package's
    fixture: a tree reader may cross-check the declared box_size against its own
    file's box size (e.g. read_ctrees_hdf5.c against uchuu's committed fixture), so
    a literal borrowed from mini-millennium (62.5 Mpc/h) would collide with a
    different package's real box size and fail for the wrong reason, or -- for a
    package whose reader carries no such cross-check -- pass vacuously without
    actually exercising a genuine box size. The injected value is instead derived
    from the SELECTED package's own declared box_size, relabelled from Mpc/h to the
    numerically-equivalent kpc/h (1 Mpc/h == 1000 kpc/h), so the run always sees its
    own package's real box size under a different, but dimensionally matching,
    label.
    """
    if not MIMIC_EXE.exists():
        raise TestSkipped("Mimic not built")

    param_file, _output_dir, _ = create_test_param_file(
        output_name="box_size_correct_dimension",
        first_file=0,
        last_file=0,
        temp_dir=TEMP_DIR,
    )
    with open(param_file, "r") as handle:
        config = yaml.safe_load(handle)

    sim_config_path = resolve_sim_config_path(config["simulation"]["config"], param_file)
    with open(sim_config_path, "r") as handle:
        sim_config = yaml.safe_load(handle)
    box_size = (sim_config.get("simulation") or {}).get("box_size")
    if not isinstance(box_size, dict) or box_size.get("units") != "Mpc/h":
        raise TestSkipped(
            "selected package's simulation.box_size is not declared in Mpc/h; cannot "
            "derive an equivalent kpc/h value for the positive case"
        )

    config["simulation"]["box_size"] = {
        "value": box_size["value"] * 1000.0,
        "units": "kpc/h",
        "h_convention": box_size.get("h_convention", "carried"),
    }
    with open(param_file, "w") as handle:
        yaml.safe_dump(config, handle, default_flow_style=False, sort_keys=False)

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
