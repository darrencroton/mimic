#!/usr/bin/env python3
"""
SAGE Calculate Star Formation Module - Integration Test

Validates: Module pipeline integration, physics correctness, parameter sensitivity

This test validates the sage26_calculate_star_formation module integration on a pipeline that
supplies cold gas: the disk scale radius (which sets the star-forming disk, reff = 3 r_s),
reionization, infall and cooling (with radio-mode heating) feed the disk, then the star-formation
chain. Every case requires star formation to have happened (a positive witness), so none can pass
on an empty disk. SFprescription 0 (the Croton law) and no supernova calculator, so no feedback.

Test cases:
  - test_module_pipeline_integration: Full pipeline execution; finite, non-negative output with
    star-forming rows
  - test_star_formation_physics: the stars formed over the snapshot, net of recycling, are in the
    galaxy; rows without cold gas form no stars
  - test_parameter_sensitivity: a higher efficiency turns more of the same cooled gas into stars
  - test_memory_and_performance: Memory leaks and performance baseline

Author: Mimic Development Team
Date: 2025-12-18
"""

import shutil
import sys
from pathlib import Path

import numpy as np

# Repository root and paths
REPO_ROOT = Path(__file__).parent.parent.parent.parent.parent.parent
sys.path.insert(0, str(REPO_ROOT / "tests"))
from framework import (
    BLUE,
    GREEN,
    MIMIC_EXE,
    NC,
    RED,
    TestSkipped,
    check_no_memory_leaks,
    create_test_param_file,
    load_binary_halos,
    result_error,
    result_fail,
    result_pass,
    result_skip,
    run_mimic,
)

# The gas supply before the module: without the disk scale radius no disk forms stars, and
# without infall and cooling the disk holds no cold gas
PHASES = {
    "pre_timestep": [
        ("sage26_set_disk_scale_radius", "process_full_halo"),
        ("sage26_reionization", "process_full_halo"),
        ("sage26_prepare_infall_budget", "process_full_halo"),
    ],
    "galaxy_physics": [
        ("sage26_apply_infall", "process_full_halo"),
        ("sage26_calculate_cooling_budget", "process_by_galaxy"),
        ("sage26_radio_mode_heating", "process_by_galaxy"),
        ("sage26_apply_cooling", "process_by_galaxy"),
        ("sage26_calculate_star_formation", "process_by_galaxy"),
        ("sage26_apply_star_formation_supernova", "process_by_galaxy"),
    ],
    "satellite_mergers": [],
    "post_timestep": [],
}

RECYCLE_FRACTION = 0.43


def model_params(sfr_efficiency):
    """Every key the pipeline reads, the SF chain's with SFprescription 0, and SfrEfficiency."""
    return {
        "GlobalBaryonFraction": 0.17,
        "AGNrecipe": 2,
        "RadioModeEfficiency": 0.08,
        "BulgeSizeOn": 0,
        "SFprescription": 0,
        "CGMrecipeOn": 0,  # required (the CGM recipe off)
        "H2RadialIntegrationOn": 1,
        "H2RadialNBins": 25,
        "H2RadialRMaxFactor": 5.0,
        "SfrEfficiency": sfr_efficiency,
        "StarFormingDiskFactor": 3.0,
        "RecycleFraction": RECYCLE_FRACTION,
        "Yield": 0.025,
        "FracZleaveDisk": 0.0,
    }


def run_z0(output_name, sfr_efficiency):
    """Run the pipeline; return (halos at z = 0, stdout, stderr, temp_dir)."""
    param_file, output_dir, temp_dir = create_test_param_file(
        output_name=output_name, phase_config=PHASES, model_params=model_params(sfr_efficiency)
    )
    returncode, stdout, stderr = run_mimic(param_file)
    if returncode != 0:
        shutil.rmtree(temp_dir)
    assert returncode == 0, f"Mimic should execute successfully\nSTDERR: {stderr}"
    output_file = output_dir / "model_z0.000_0"
    assert output_file.exists(), "Output file should exist"
    halos, _metadata = load_binary_halos(output_file)
    return halos, stdout, stderr, temp_dir


def test_module_pipeline_integration():
    """Test that module integrates correctly into full pipeline

    Validates:
    - Module loads and initializes
    - Pipeline executes without errors
    - StarFormationRate exists, is finite and non-negative, and is positive on some rows
    """
    print(f"\n{BLUE}TEST: Module pipeline integration{NC}")

    halos, _stdout, _stderr, temp_dir = run_z0("sf_integration", 0.02)
    try:
        assert len(halos) > 0, "Should have output halos"
        assert "StarFormationRate" in halos.dtype.names, "Output should have StarFormationRate"
        sfr = halos["StarFormationRate"]
        assert np.all(np.isfinite(sfr)), "StarFormationRate should be finite"
        assert np.all(sfr >= 0.0), "StarFormationRate should be non-negative"
        forming = int(np.sum(sfr > 0))
        assert forming > 0, "some rows should form stars (the pipeline supplies cold gas)"
        print(f"  {forming} of {len(halos)} rows form stars")
    finally:
        shutil.rmtree(temp_dir)
    print(f"{GREEN}✓ Module pipeline integration successful{NC}")


def test_star_formation_physics():
    """Test that star formation physics is physically reasonable

    Validates:
    - The stars formed over the snapshot, net of recycling, are held by the galaxy: with no
      merger, disruption or feedback module nothing removes stars, so (1 - RecycleFraction) times
      the stars formed is at most StellarMass. StarFormationRate (Msun/yr) times dT (Myr) is the
      stars formed in 10^6 Msun, so a factor 10^-4 gives the code's 10^10 Msun (the writer
      conversions of both fields, model_properties.yaml and src/core/core_properties.yaml, at
      UnitMass_in_g = 10^10 solar masses)
    - Rows with no cold gas form no stars, and such rows exist
    """
    print(f"\n{BLUE}TEST: Star formation physics correctness{NC}")

    halos, _stdout, _stderr, temp_dir = run_z0("sf_physics", 0.02)
    try:
        sfr = halos["StarFormationRate"].astype(np.float64)
        cold_gas = halos["ColdGas"]
        forming = sfr > 0
        assert np.sum(forming) > 0, "some rows should form stars (the pipeline supplies cold gas)"

        formed = sfr * halos["dT"].astype(np.float64) * 1e-4
        retained = (1.0 - RECYCLE_FRACTION) * formed[forming]
        stellar_mass = halos["StellarMass"].astype(np.float64)[forming]
        # 1e-5 relative: float stores; a galaxy whose stars all formed this snapshot is at equality
        excess = retained > stellar_mass * (1.0 + 1e-5)
        assert not np.any(excess), (
            f"{int(np.sum(excess))} rows formed more stars this snapshot than they hold "
            f"(largest ratio {np.max(retained / stellar_mass):.6f})"
        )
        print(f"  retained/StellarMass at most {np.max(retained / stellar_mass):.7f}")

        no_gas = cold_gas < 1e-6
        assert np.sum(no_gas) > 0, "some rows should hold no cold gas"
        assert np.all(sfr[no_gas] == 0.0), "Galaxies with no cold gas should have zero SFR"
        print(f"  Verified {np.sum(no_gas)} galaxies with no gas have zero SFR")
    finally:
        shutil.rmtree(temp_dir)
    print(f"{GREEN}✓ Star formation physics is physically reasonable{NC}")


def test_parameter_sensitivity():
    """Test that changing SfrEfficiency parameter changes results

    Validates:
    - Both efficiencies form stars
    - With no feedback, the cold gas and stars together come from the same cooling in both runs,
      so the higher efficiency leaves less cold gas and more stellar mass. The total
      StarFormationRate at z = 0 is not monotonic in the efficiency (a faster law depletes the
      disk it draws on) and is not asserted.
    """
    print(f"\n{BLUE}TEST: Parameter sensitivity{NC}")

    low, _stdout, _stderr, temp_dir_low = run_z0("sf_low_eff", 0.01)
    try:
        high, _stdout, _stderr, temp_dir_high = run_z0("sf_high_eff", 0.05)
        shutil.rmtree(temp_dir_high)
    finally:
        shutil.rmtree(temp_dir_low)

    for name, halos in (("low", low), ("high", high)):
        assert np.sum(halos["StarFormationRate"] > 0) > 0, f"{name} efficiency should form stars"
    totals = {
        name: (float(np.sum(h["StellarMass"])), float(np.sum(h["ColdGas"])))
        for name, h in (("low", low), ("high", high))
    }
    print(f"  (StellarMass, ColdGas) totals: low {totals['low']}, high {totals['high']}")
    assert totals["high"][0] > totals["low"][0], "higher efficiency should form more stars"
    assert totals["high"][1] < totals["low"][1], "higher efficiency should leave less cold gas"

    print(f"{GREEN}✓ Parameter sensitivity validated{NC}")


def test_memory_and_performance():
    """Test memory safety and establish performance baseline

    Validates:
    - No memory leaks during execution
    - Execution completes in reasonable time
    """
    print(f"\n{BLUE}TEST: Memory safety and performance{NC}")

    import time

    start_time = time.time()
    halos, stdout, stderr, temp_dir = run_z0("sf_memory", 0.02)
    elapsed_time = time.time() - start_time
    shutil.rmtree(temp_dir)

    # Check for memory leaks
    assert check_no_memory_leaks(stdout, stderr), "Should not have memory leaks"

    # Performance baseline (should complete in < 30 seconds for test data)
    print(f"  Execution time: {elapsed_time:.2f}s")
    assert elapsed_time < 30.0, f"Execution too slow: {elapsed_time:.2f}s (expected < 30s)"

    print(f"{GREEN}✓ Memory safety and performance validated{NC}")


def main():
    """Main test runner"""
    print(f"{BLUE}{'=' * 60}{NC}")
    print(f"{BLUE}Test Suite: {Path(__file__).name}{NC}")
    print(f"{BLUE}{'=' * 60}{NC}")
    print()

    tests = [
        test_module_pipeline_integration,
        test_star_formation_physics,
        test_parameter_sensitivity,
        test_memory_and_performance,
    ]

    if not MIMIC_EXE.exists():
        for test in tests:
            result_skip(test.__name__, "Mimic not built")
        return 0

    passed = 0
    failed = 0
    skipped = 0

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
