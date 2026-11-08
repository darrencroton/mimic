#!/usr/bin/env python3
"""
sage26_finalize_cooling_heating - Integration Test

Validates: module lifecycle in post_timestep and its effect on a real run

The module normalises the Cooling and Heating energies, accumulated undivided over
the substeps by sage26_apply_cooling and sage26_radio_mode_heating, once per FoF by
the first galaxy's snapshot interval (SAGE26 core_build_model.c:590-603). The run
with the finaliser must therefore differ from the same run without it only in
Cooling and Heating: a zero energy stays zero, and every positive energy rises
(it is multiplied by 1 / deltaT, with deltaT well below one code time unit). The
factor itself is asserted bit for bit by the module's oracle unit test.

Test cases:
  - test_module_loads: module registers and initialises in post_timestep
  - test_only_energies_change: with and without the finaliser, every other field
    is identical and Cooling and Heating are rescaled as expected
"""

import shutil
import sys
from pathlib import Path

# Add tests directory to path to import framework
REPO_ROOT = Path(__file__).parent.parent.parent.parent.parent.parent
sys.path.insert(0, str(REPO_ROOT / "tests"))

from framework import (  # noqa: E402
    BLUE,
    GREEN,
    MIMIC_EXE,
    NC,
    RED,
    TestSkipped,
    create_test_param_file,
    load_binary_halos,
    result_error,
    result_fail,
    result_pass,
    result_skip,
    run_mimic,
)

COOLING_CHAIN = [
    ("sage26_apply_infall", "process_full_halo"),
    ("sage26_calculate_cooling_budget", "process_by_galaxy"),
    ("sage26_radio_mode_heating", "process_by_galaxy"),
    ("sage26_apply_cooling", "process_by_galaxy"),
]

PRE_TIMESTEP = [
    ("sage26_reionization", "process_full_halo"),
    ("sage26_prepare_infall_budget", "process_full_halo"),
]

MODEL_PARAMS = {
    "GlobalBaryonFraction": 0.17,
    "AGNrecipe": 2,
    "RadioModeEfficiency": 0.08,
    "CGMrecipeOn": 0,  # required by the infall modules, R3's value
}


def _phase_config(with_finaliser):
    post = [("sage26_finalize_cooling_heating", "process_full_halo")] if with_finaliser else []
    return {
        "pre_timestep": PRE_TIMESTEP,
        "galaxy_physics": COOLING_CHAIN,
        "satellite_mergers": [],
        "post_timestep": post,
    }


def _run(output_name, with_finaliser):
    param_file, output_dir, temp_dir = create_test_param_file(
        output_name=output_name,
        phase_config=_phase_config(with_finaliser),
        model_params=MODEL_PARAMS,
    )
    returncode, stdout, stderr = run_mimic(param_file)
    return returncode, stdout, stderr, output_dir, temp_dir


def test_module_loads():
    """
    The module registers and initialises when placed in post_timestep

    Expected: the run succeeds and logs the module's initialisation
    """
    print("Testing module load and initialization...")

    returncode, stdout, stderr, _, temp_dir = _run("sage26_finalize_load", True)

    assert returncode == 0, f"Mimic should run with the finaliser\nStderr: {stderr}"
    assert (
        "SAGE finalize cooling and heating module initialized" in stdout
    ), "sage26_finalize_cooling_heating should log its initialisation"

    shutil.rmtree(temp_dir)
    print("  ✓ Module loads and initializes successfully")


def test_only_energies_change():
    """
    The finaliser changes Cooling and Heating only, and raises every positive energy

    Expected: identical rows in both runs; every field but Cooling and Heating is
    bit-identical; a zero energy stays zero; a positive energy is larger (log10
    output) after the 1 / deltaT normalisation, deltaT being below one code unit.
    """
    import numpy as np

    print("Testing that only the energies change...")

    rc_raw, _, err_raw, out_raw, tmp_raw = _run("sage26_finalize_raw", False)
    rc_fin, _, err_fin, out_fin, tmp_fin = _run("sage26_finalize_norm", True)
    assert rc_raw == 0, f"run without the finaliser should succeed\nStderr: {err_raw}"
    assert rc_fin == 0, f"run with the finaliser should succeed\nStderr: {err_fin}"

    raw, _ = load_binary_halos(out_raw / "model_z0.000_0")
    fin, _ = load_binary_halos(out_fin / "model_z0.000_0")

    assert len(raw) == len(fin) and len(raw) > 0, "both runs should write the same rows"
    assert raw.dtype == fin.dtype, "both runs should write the same fields"

    for field in raw.dtype.names:
        if field in ("Cooling", "Heating"):
            continue
        same = np.array_equal(raw[field], fin[field], equal_nan=True)
        assert same, f"{field} should not depend on the finaliser"

    for field in ("Cooling", "Heating"):
        zero = raw[field] == 0
        assert np.all(fin[field][zero] == 0), f"a zero {field} should stay zero"
        positive = ~zero
        assert np.all(fin[field][positive] > raw[field][positive]), (
            f"every positive {field} should rise by the 1/deltaT normalisation "
            f"(log10 output, deltaT < 1 code unit)"
        )
        print(f"  {field}: {int(np.sum(positive))} of {len(raw)} galaxies normalised")

    assert np.sum(raw["Cooling"] > 0) > 0, "some galaxies should cool"

    shutil.rmtree(tmp_raw)
    shutil.rmtree(tmp_fin)
    print("  ✓ Only Cooling and Heating change")


def main():
    """
    Main test runner

    Executes all test cases and reports results.
    """
    print(f"{BLUE}{'=' * 60}{NC}")
    print(f"{BLUE}Test Suite: sage26_finalize_cooling_heating Integration Tests{NC}")
    print(f"{BLUE}{'=' * 60}{NC}")

    tests = [
        test_module_loads,
        test_only_energies_change,
    ]

    passed = 0
    failed = 0
    skipped = 0

    if not MIMIC_EXE.exists():
        for test in tests:
            result_skip(test.__name__, "Mimic not built")
        return 0

    for test in tests:
        print()
        try:
            test()
            result_pass(test.__name__)
            passed += 1
        except TestSkipped as e:
            result_skip(test.__name__, str(e))
            skipped += 1
        except AssertionError as e:
            result_fail(test.__name__, str(e).splitlines()[0])
            failed += 1
        except Exception as e:
            result_error(test.__name__, str(e).splitlines()[0])
            failed += 1

    print()
    print(f"{BLUE}{'=' * 60}{NC}")
    print(f"{BLUE}Test Summary{NC}")
    print(f"{BLUE}{'=' * 60}{NC}")
    print(f"Passed:  {passed}")
    if skipped:
        print(f"Skipped: {skipped}")
    print(f"Failed:  {failed}")
    print(f"Total:   {passed + failed + skipped}")
    print(f"{BLUE}{'=' * 60}{NC}")
    print()

    if failed == 0:
        print(f"{GREEN}✓ All tests passed!{NC}")
        return 0
    else:
        print(f"{RED}✗ {failed} test(s) failed{NC}")
        return 1


if __name__ == "__main__":
    sys.exit(main())
