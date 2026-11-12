#!/usr/bin/env python3
"""
sage_satellite_stripping integration test.

Stripping can only move gas that is already there, so it is run behind a baryon supply:
sage_reionization and sage_prepare_infall_budget in pre_timestep and sage_apply_infall in
the substep phase fill the centrals' hot gas, and a central that later becomes a satellite takes
its hot gas with it. The same pipeline is run without and with sage_satellite_stripping on the
real mini-Millennium trees (file 0) and the two z = 0 outputs are compared. The stripping
formulas, regimes and metal transfer are asserted by the unit tests.

Test cases:
  - test_stripping_moves_gas_to_centrals: the run initialises, reports no memory leak and writes
    finite values; its satellites and orphans hold less hot gas than without stripping, the
    centrals hold more, and the total hot gas is conserved to within the infall budget's
    rebalancing
"""

import sys
from pathlib import Path

import numpy as np

REPO_ROOT = Path(__file__).parent.parent.parent.parent.parent.parent
sys.path.insert(0, str(REPO_ROOT / "tests"))
from framework import (  # noqa: E402
    MIMIC_EXE,
    SharedRuns,
    TestSkipped,
    assert_no_infs,
    assert_no_nans,
    check_no_memory_leaks,
    load_binary_halos,
    run_test_suite,
)

SUPPLY_PRE_TIMESTEP = [
    ("sage_reionization", "process_full_halo"),
    ("sage_prepare_infall_budget", "process_full_halo"),
]
SUPPLY_PHASE = [("sage_apply_infall", "process_full_halo")]
STRIPPING_PHASE = SUPPLY_PHASE + [("sage_satellite_stripping", "process_by_galaxy")]
PARAMETERS = {"GlobalBaryonFraction": 0.20}

SUPPLY_ONLY_RUN = {
    "phase_config": {
        "pre_timestep": SUPPLY_PRE_TIMESTEP,
        "galaxy_physics": SUPPLY_PHASE,
        "post_timestep": [],
    },
    "model_params": PARAMETERS,
    "first_file": 0,
    "last_file": 0,
}
STRIPPING_RUN = {
    "phase_config": {
        "pre_timestep": SUPPLY_PRE_TIMESTEP,
        "galaxy_physics": STRIPPING_PHASE,
        "post_timestep": [],
    },
    "model_params": PARAMETERS,
    "first_file": 0,
    "last_file": 0,
}

#: The most the total hot gas may differ between the two runs, as a fraction: stripping moves
#: gas within a FoF, but the infall budget then rebalances what each central receives
TOTAL_HOT_GAS_TOLERANCE = 0.02

RUNS = SharedRuns()


def _final_galaxies(name, config):
    """Run the named configuration (SKIP without a built Mimic) and load its z = 0 galaxies."""
    if not MIMIC_EXE.exists():
        raise TestSkipped("Mimic not built")
    run = RUNS.run(name, **config)
    assert run.returncode == 0, f"{name} should run\nStderr: {run.stderr}"
    assert check_no_memory_leaks(run.stdout, run.stderr), f"{name} should not leak memory"
    halos, _metadata = load_binary_halos(run.output_dir / "model_z0.000_0")
    assert len(halos) > 0, f"{name} should write galaxies"
    return run, halos


def test_stripping_moves_gas_to_centrals():
    """Stripping takes hot gas from satellites and orphans and gives it to the centrals."""
    _, supply_only = _final_galaxies("supply_only", SUPPLY_ONLY_RUN)
    run, stripped = _final_galaxies("stripping", STRIPPING_RUN)

    assert (
        "SAGE satellite stripping module initialized" in run.stdout
    ), "the module should log its initialisation"
    assert "GlobalBaryonFraction = 0.2000" in run.stdout, "the module should log the run's value"
    assert_no_nans(stripped)
    assert_no_infs(stripped)

    def hot_gas(halos, is_central):
        return float(np.sum(halos["HotGas"][(halos["Type"] == 0) == is_central], dtype=np.float64))

    # Without stripping the satellites keep the gas they carried off as centrals, so the supply
    # is real: this is what stripping has to take
    assert hot_gas(supply_only, False) > 0.0, "the supply should leave satellites with hot gas"

    lost = hot_gas(supply_only, False) - hot_gas(stripped, False)
    gained = hot_gas(stripped, True) - hot_gas(supply_only, True)
    assert lost > 0.0, f"satellites and orphans should lose hot gas, lost {lost}"
    assert gained > 0.0, f"centrals should gain hot gas, gained {gained}"

    total = hot_gas(supply_only, True) + hot_gas(supply_only, False)
    total_stripped = hot_gas(stripped, True) + hot_gas(stripped, False)
    assert abs(total_stripped - total) < TOTAL_HOT_GAS_TOLERANCE * total, (
        f"stripping should move hot gas, not create or destroy it: {total} without, "
        f"{total_stripped} with"
    )


def main():
    """Run the integration cases, removing the runs' temporary directories afterwards."""
    try:
        return run_test_suite(
            [test_stripping_moves_gas_to_centrals],
            "sage_satellite_stripping Integration Tests",
        )
    finally:
        RUNS.cleanup()


if __name__ == "__main__":
    sys.exit(main())
