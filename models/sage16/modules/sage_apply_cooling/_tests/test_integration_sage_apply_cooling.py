#!/usr/bin/env python3
"""
sage_apply_cooling - Integration Test

Validates: cooling forms cold gas in a pipeline

One Mimic run of the classic cooling chain (reionization, prepare_infall_budget, apply_infall,
calculate_cooling_budget, apply_cooling) feeds both cases. Infall puts gas in the
hot halo, the budget module decides how much cools, and sage_apply_cooling moves it to the cold
reservoir. The pipeline has no star formation or enrichment, so every metal reservoir is zero and
the metal transfer is covered by the unit test, not here.

Test cases:
  - test_cooling_forms_cold_gas: the run is clean, and centrals gain cold gas while hot gas
    remains in others
  - test_cooling_energy_implies_cold_gas: a galaxy with a Cooling energy has cold gas, and a
    galaxy without cold gas has none

NOTE: sage_apply_cooling requires sage_calculate_cooling_budget before it in galaxy_physics.
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

# Infall -> cooling budget -> cooling transfer
COOLING_CHAIN = {
    "pre_timestep": [
        ("sage_reionization", "process_full_halo"),
        ("sage_prepare_infall_budget", "process_full_halo"),
    ],
    "galaxy_physics": [
        ("sage_apply_infall", "process_full_halo"),
        ("sage_calculate_cooling_budget", "process_by_galaxy"),
        ("sage_apply_cooling", "process_by_galaxy"),
    ],
    "satellite_mergers": [],
    "post_timestep": [],
}
COOLING_PARAMS = {"GlobalBaryonFraction": 0.17}


def cooling_chain_galaxies():
    """Run the cooling chain once and return its z = 0 galaxies."""
    if not MIMIC_EXE.exists():
        raise TestSkipped("Mimic not built")
    run = RUNS.run("cooling_chain", phase_config=COOLING_CHAIN, model_params=COOLING_PARAMS)
    assert run.returncode == 0, f"Mimic should execute successfully\nStderr: {run.stderr}"
    assert check_no_memory_leaks(run.stdout, run.stderr), "Should have no memory leaks"
    halos, _metadata = load_binary_halos(run.output_dir / "model_z0.000_0")
    assert len(halos) > 0, "Should have galaxies in output"
    return halos


def test_cooling_forms_cold_gas():
    """Cooling moves infalling hot gas into cold gas in centrals.

    The chain has no other source of cold gas, so cold gas in a central is cooled hot gas. Some
    centrals must have cooled (cold gas) and some must still hold hot gas, or the run says
    nothing about the transfer.
    """
    halos = cooling_chain_galaxies()
    centrals = halos[halos["Type"] == 0]

    for name in ("HotGas", "ColdGas"):
        assert np.all(np.isfinite(centrals[name])), f"{name} should be finite"
        assert np.all(centrals[name] >= 0), f"{name} should be non-negative"

    with_cold = int(np.sum(centrals["ColdGas"] > 0))
    with_hot = int(np.sum(centrals["HotGas"] > 0))
    print(f"  {with_cold} / {len(centrals)} centrals with cold gas, {with_hot} with hot gas")
    assert with_cold > 0.5 * len(centrals), "Most centrals should have cooled some gas"
    assert with_hot > 0, "Some centrals should still hold hot gas"


def test_cooling_energy_implies_cold_gas():
    """Cooling is written only where gas moved to the cold reservoir.

    Cooling accumulates 0.5 m Vvir^2 / dT (log10 of it in the output; 0 means none) only for a
    positive cooled mass, and no module in the chain removes cold gas. So a galaxy with a Cooling
    energy has cold gas, and a galaxy without cold gas has none.
    """
    halos = cooling_chain_galaxies()
    cooled = halos["Cooling"] != 0

    print(f"  {int(np.sum(cooled))} / {len(halos)} galaxies with Cooling energy")
    assert np.any(cooled), "Some galaxies should record cooling energy"
    assert np.all(halos["ColdGas"][cooled] > 0), "A galaxy with Cooling energy has cold gas"
    no_cold = halos["ColdGas"] <= 0
    assert not np.any(halos["Cooling"][no_cold] != 0), "No cold gas, no Cooling energy"


def main():
    """Run the sage_apply_cooling integration tests."""
    try:
        return run_test_suite(
            [test_cooling_forms_cold_gas, test_cooling_energy_implies_cold_gas],
            "sage_apply_cooling Integration Tests",
        )
    finally:
        RUNS.cleanup()


if __name__ == "__main__":
    sys.exit(main())
