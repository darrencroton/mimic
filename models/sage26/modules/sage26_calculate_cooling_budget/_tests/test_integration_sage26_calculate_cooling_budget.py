#!/usr/bin/env python3
"""
sage26_calculate_cooling_budget - Integration Test

Validates: the hot-halo cooling diagnostics the budget module writes, on a pipeline that feeds it
hot gas

The budget module's cooling mass, cooling radius and return path are carried between modules and
never written to output. What it does write to output are the diagnostics tcool, tff and
RcoolToRvir, so this test reads those. One Mimic run of reionization, prepare_infall_budget,
apply_infall and the budget module (CGMrecipeOn 0), with no apply_cooling, leaves the hot gas the
budget saw in the output, so each galaxy's diagnostics can be tied to its HotGas.

Test cases:
  - test_diagnostics_follow_hot_gas: tcool is positive exactly where a galaxy has hot gas and a
    virial velocity, tff is sqrt(2) tcool, and RcoolToRvir is positive there
  - test_tcool_unit_conversion_on_orphans: for Type 2 orphans (whose Rvir and Vvir never change)
    tcool is Rvir / Vvir times SAGE26's mass-rate conversion factor

NOTE: sage26_calculate_cooling_budget requires CGMrecipeOn, set to 0 (the CGM recipe off).
"""

import sys
from pathlib import Path

import numpy as np

# Add tests directory to path to import framework
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

# Infall -> cooling budget only: nothing downstream removes the hot gas the budget saw
BUDGET_CHAIN = {
    "pre_timestep": [
        ("sage26_reionization", "process_full_halo"),
        ("sage26_prepare_infall_budget", "process_full_halo"),
    ],
    "galaxy_physics": [
        ("sage26_apply_infall", "process_full_halo"),
        ("sage26_calculate_cooling_budget", "process_by_galaxy"),
    ],
    "satellite_mergers": [],
    "post_timestep": [],
}
BUDGET_PARAMS = {"GlobalBaryonFraction": 0.17, "CGMrecipeOn": 0}

# SAGE26's tcool conversion: 1e10 / Hubble_h * SEC_PER_GIGAYEAR / UnitTime_in_s with the
# mini-millennium Hubble_h 0.73, SEC_PER_GIGAYEAR 3.155e16 and UnitTime_in_s 3.08568e19 (Mpc/h over
# km/s); a mass-rate factor applied to a time, as SAGE26 does, so tcool is not a time in Gyr
TCOOL_CONVERSION = 1.0e10 / 0.73 * 3.155e16 / 3.08568e19


def budget_chain_galaxies():
    """Run the budget chain once and return its z = 0 galaxies."""
    if not MIMIC_EXE.exists():
        raise TestSkipped("Mimic not built")
    run = RUNS.run("budget_chain", phase_config=BUDGET_CHAIN, model_params=BUDGET_PARAMS)
    assert run.returncode == 0, f"Mimic should execute successfully\nStderr: {run.stderr}"
    assert check_no_memory_leaks(run.stdout, run.stderr), "Should have no memory leaks"
    halos, _metadata = load_binary_halos(run.output_dir / "model_z0.000_0")
    assert len(halos) > 0, "Should have galaxies in output"
    return halos


def test_diagnostics_follow_hot_gas():
    """The budget writes tcool, tff and RcoolToRvir for every galaxy that has hot gas.

    The cooling recipe's outer guard needs HotGas > 0 and Vvir > 0: a galaxy that fails it has
    tcool reset to 0. tff is sqrt(2) times tcool (the free-fall time of the same Rvir / Vvir), and
    a galaxy that passes forms a cooling radius, so RcoolToRvir is positive.
    """
    halos = budget_chain_galaxies()
    passes_guard = (halos["HotGas"] > 0) & (halos["Vvir"] > 0)
    print(f"  {int(np.sum(passes_guard))} / {len(halos)} galaxies pass the outer guard")
    assert np.any(passes_guard), "Some galaxies should hold hot gas"

    assert np.all(halos["tcool"][passes_guard] > 0), "tcool is positive where the guard passes"
    assert np.all(halos["tcool"][~passes_guard] == 0), "tcool is reset to 0 where it fails"

    ratio = halos["tff"][passes_guard] / halos["tcool"][passes_guard]
    assert np.allclose(ratio, np.sqrt(2.0), rtol=1.0e-6), "tff should be sqrt(2) times tcool"
    assert np.all(halos["RcoolToRvir"][passes_guard] > 0), "A cooling radius is formed"


def test_tcool_unit_conversion_on_orphans():
    """tcool is Rvir / Vvir times SAGE26's conversion factor.

    A Type 2 orphan keeps the Rvir and Vvir its last halo had, so the output values are the ones
    the budget used. A central's or satellite's output Rvir and Vvir need not equal the values
    its last budget call read, so only orphans give an exact check.
    """
    halos = budget_chain_galaxies()
    orphans = halos[(halos["Type"] == 2) & (halos["HotGas"] > 0) & (halos["Vvir"] > 0)]
    print(f"  {len(orphans)} orphans with hot gas")
    assert len(orphans) > 0, "Some orphans should hold hot gas"

    expected = orphans["Rvir"] / orphans["Vvir"] * TCOOL_CONVERSION
    assert np.allclose(orphans["tcool"], expected, rtol=1.0e-5), "tcool = Rvir / Vvir * factor"


def main():
    """Run the sage26_calculate_cooling_budget integration tests."""
    try:
        return run_test_suite(
            [test_diagnostics_follow_hot_gas, test_tcool_unit_conversion_on_orphans],
            "sage26_calculate_cooling_budget Integration Tests",
        )
    finally:
        RUNS.cleanup()


if __name__ == "__main__":
    sys.exit(main())
