#!/usr/bin/env python3
"""
sage_quasar_mode - Integration Test

Validates: the module's black-hole growth and quasar wind through real runs of mimic on the
committed test trees, on a pipeline that supplies the gas the module acts on.

A pipeline with the disk instability and the module alone gives an all-zero output (nothing creates
gas or stars), where a finite or sign check cannot fail. Every run here therefore uses GAS_PHASES:
the infall, cooling, star-formation and supernova chain builds the stellar disk and cold gas that
a disk instability needs, then sage_disk_instability writes the trigger and sage_quasar_mode
grows the black hole from it. The merger channel (process_per_event) is exercised through the real
merger producer in sage_resolve_mergers_and_disruption/_tests/
test_integration_sage_merger_event_consumers.py.

The test cases share three runs of GAS_PHASES (AGNrecipe 2),
which differ in one parameter each:
  gas_mid      BlackHoleGrowthRate 0.01, QuasarModeEfficiency 0.001
  gas_growth   BlackHoleGrowthRate 0.02, QuasarModeEfficiency 0.001
  gas_wind     BlackHoleGrowthRate 0.01, QuasarModeEfficiency 0.01

Test cases:
  - test_quasar_physics_correctness: on gas_mid, the fields are finite, non-negative and
    non-trivial; the disk instability drives quasar accretion; black holes stay far below the halo
    mass and quasar accretion never exceeds the black-hole mass; the run's own leak report is
    clean
  - test_parameter_sensitivity: a higher BlackHoleGrowthRate grows more black-hole mass and a
    higher QuasarModeEfficiency ejects more gas and leaves less black-hole mass, each against
    gas_mid

Run (from the repository root, after `make MODEL=sage16 SIMULATION=mini-millennium`):
  MODEL=sage16 SIMULATION=mini-millennium python3 models/sage16/modules/sage_quasar_mode/\\
_tests/test_integration_sage_quasar_mode.py

Author: Mimic Development Team
Date: 2025-12-23
"""

import sys
from pathlib import Path

import numpy as np

# Repository root and paths
REPO_ROOT = Path(__file__).parent.parent.parent.parent.parent.parent
sys.path.insert(0, str(REPO_ROOT / "tests"))
from framework import (
    MIMIC_EXE,
    SharedRuns,
    TestSkipped,
    check_no_memory_leaks,
    load_binary_halos,
    run_test_suite,
)

RUNS = SharedRuns()

# The gas supply and star-formation chain that build the stellar disk and cold gas a disk
# instability needs, then the instability and the module
GAS_PHASES = {
    "pre_timestep": [
        ("sage_set_disk_scale_radius", "process_full_halo"),
        ("sage_reionization", "process_full_halo"),
        ("sage_prepare_infall_budget", "process_full_halo"),
    ],
    "galaxy_physics": [
        ("sage_apply_infall", "process_full_halo"),
        ("sage_calculate_cooling_budget", "process_by_galaxy"),
        ("sage_radio_mode_heating", "process_by_galaxy"),
        ("sage_apply_cooling", "process_by_galaxy"),
        ("sage_calculate_star_formation", "process_by_galaxy"),
        ("sage_calculate_supernova_feedback", "process_by_galaxy"),
        ("sage_apply_star_formation_supernova", "process_by_galaxy"),
        ("sage_disk_instability", "process_by_galaxy"),
        ("sage_quasar_mode", "process_by_galaxy"),
    ],
    "satellite_mergers": [],
    "post_timestep": [],
}


def gas_params(black_hole_growth_rate, quasar_mode_efficiency):
    """Model parameters of GAS_PHASES with the two quasar parameters."""
    return {
        "BlackHoleGrowthRate": black_hole_growth_rate,
        "QuasarModeEfficiency": quasar_mode_efficiency,
        "StarFormingDiskFactor": 3.0,
        "GlobalBaryonFraction": 0.17,
        "AGNrecipe": 2,
        "RadioModeEfficiency": 0.08,
        "SfrEfficiency": 0.02,
        "FeedbackReheatingEpsilon": 3.0,
        "FeedbackEjectionEfficiency": 0.3,
        "RecycleFraction": 0.43,
    }


GAS_MID = gas_params(0.01, 0.001)
GAS_GROWTH = gas_params(0.02, 0.001)
GAS_WIND = gas_params(0.01, 0.01)


def gas_run(name, model_params):
    """Return the named GAS_PHASES run (made once) and its z = 0 halos."""
    if not MIMIC_EXE.exists():
        raise TestSkipped("Mimic not built")
    run = RUNS.run(name, phase_config=GAS_PHASES, model_params=model_params)
    assert run.returncode == 0, f"Mimic should execute successfully\nSTDERR: {run.stderr}"
    halos, _metadata = load_binary_halos(run.output_dir / "model_z0.000_0")
    return run, halos


def test_quasar_physics_correctness():
    """On the supplied-gas pipeline the module grows black holes from the disk instability.

    Asserts, on gas_mid:
    - BlackHoleMass, QuasarModeBHaccretionMass, ColdGas and EjectedGas are finite and
      non-negative, and each is non-zero somewhere, so the checks run on real values
    - Some rows accrete in quasar mode (the instability's trigger reached the module), and every
      accreting row holds a black hole
    - Black holes in massive halos stay below 1e-3 of the halo mass, the upper end of the
      observed M_BH / M_halo relation; a growth rate or Vvir suppression that is off by orders of
      magnitude exceeds it (the largest value here is about 5e-5)
    - Quasar accretion never exceeds the black-hole mass it is part of; the bound holds exactly
      (the module adds the accretion to the black-hole mass), so it carries no tolerance
    - The run exits 0 and its own leak report is clean
    """
    run, halos = gas_run("gas_mid", GAS_MID)
    assert check_no_memory_leaks(run.stdout, run.stderr), "Should not have memory leaks"

    for name in ("BlackHoleMass", "QuasarModeBHaccretionMass", "ColdGas", "EjectedGas"):
        values = halos[name]
        assert np.all(np.isfinite(values)), f"{name} should be finite"
        assert np.all(values >= 0.0), f"{name} should be non-negative"
        assert np.any(values > 0.0), f"{name} should be non-zero somewhere"

    bh_mass = halos["BlackHoleMass"]
    quasar_accretion = halos["QuasarModeBHaccretionMass"]

    accreting = quasar_accretion > 0.0
    assert np.any(accreting), "some unstable disks should accrete in quasar mode"
    assert np.all(bh_mass[accreting] > 0.0), "a row that accreted in quasar mode holds a black hole"

    massive = halos["Mvir"] > 10.0
    assert np.any(bh_mass[massive] > 0.0), "massive halos should host black holes"
    max_fraction = np.max(bh_mass[massive] / halos["Mvir"][massive])
    assert max_fraction < 1e-3, f"M_BH / M_halo should stay below 1e-3 (largest {max_fraction:.2e})"

    assert np.all(
        quasar_accretion <= bh_mass
    ), "Quasar accretion should not exceed the black-hole mass"


def test_parameter_sensitivity():
    """BlackHoleGrowthRate and QuasarModeEfficiency each move the output in the physical direction.

    Each comparison is against gas_mid, on totals that are positive (a rate of 0.01 and an
    efficiency of 0.001). The z = 0 QuasarModeBHaccretionMass is the last snapshot's accretion
    only and is not monotonic in the rate, so the total BlackHoleMass is the growth measure.
    - Twice the growth rate grows more black-hole mass
    - Ten times the efficiency drives more cold gas out (more EjectedGas) and so grows less
      black-hole mass
    """
    _run, mid = gas_run("gas_mid", GAS_MID)
    _run, fast = gas_run("gas_growth", GAS_GROWTH)
    _run, windy = gas_run("gas_wind", GAS_WIND)

    mid_bh = np.sum(mid["BlackHoleMass"])
    assert mid_bh > 0, "black holes should grow at the reference rate"
    fast_bh = np.sum(fast["BlackHoleMass"])
    assert (
        fast_bh > mid_bh
    ), f"a higher growth rate should grow more black-hole mass ({fast_bh:.4g} vs {mid_bh:.4g})"

    mid_ejected = np.sum(mid["EjectedGas"])
    assert mid_ejected > 0, "gas should be ejected at the reference efficiency"
    windy_ejected = np.sum(windy["EjectedGas"])
    assert (
        windy_ejected > mid_ejected
    ), f"a higher efficiency should eject more gas ({windy_ejected:.4g} vs {mid_ejected:.4g})"
    windy_bh = np.sum(windy["BlackHoleMass"])
    assert (
        windy_bh < mid_bh
    ), f"a higher efficiency should leave less black-hole mass ({windy_bh:.4g} vs {mid_bh:.4g})"


def main():
    tests = [test_quasar_physics_correctness, test_parameter_sensitivity]
    try:
        return run_test_suite(tests, "sage_quasar_mode Integration Tests")
    finally:
        RUNS.cleanup()


if __name__ == "__main__":
    sys.exit(main())
