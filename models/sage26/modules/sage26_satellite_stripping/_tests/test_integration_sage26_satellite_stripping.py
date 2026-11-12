#!/usr/bin/env python3
"""
sage26_satellite_stripping integration test.

Stripping can only move gas that is already there, so it is run behind a baryon supply:
sage26_reionization and sage26_prepare_infall_budget in pre_timestep and sage26_apply_infall in
the substep phase fill the centrals' hot gas, and a central that later becomes a satellite takes
its hot gas with it. The same pipeline is run without and with sage26_satellite_stripping on the
real mini-Millennium trees (file 0) and the two z = 0 outputs are compared. The stripping
formulas, regimes and metal transfer are asserted by the unit tests.

Test cases:
  - test_stripping_moves_gas_to_centrals: the run reports no memory leak and writes finite values;
    its satellites and orphans hold less hot gas than without stripping, the centrals hold more,
    and the total hot gas is conserved to within the infall budget's rebalancing
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
    ("sage26_reionization", "process_full_halo"),
    ("sage26_prepare_infall_budget", "process_full_halo"),
]
SUPPLY_PHASE = [("sage26_apply_infall", "process_full_halo")]
STRIPPING_PHASE = SUPPLY_PHASE + [("sage26_satellite_stripping", "process_by_galaxy")]
PARAMETERS = {"GlobalBaryonFraction": 0.20, "CGMrecipeOn": 0}

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
#: gas within a FoF, but the infall budget then rebalances what each central receives. Measured on
#: the test trees: the stripped run's total is 0.55% below the supply-only run's (12 of 3676 FoF
#: groups end with a different total). Gas deleted rather than moved would lower the total by all
#: the gas the satellites lose, 20% of it here, so 1% separates the two with headroom over the
#: measurement.
TOTAL_HOT_GAS_TOLERANCE = 0.01

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
    """Stripping takes hot gas from satellites and orphans and gives it to the centrals.

    What the case tells apart: that satellites and orphans lose hot gas and centrals gain it (the
    direction), and, through the total, that the gas lost arrives rather than vanishes. The
    loss and gain assertions alone cannot separate "moved to the central" from "deleted", since
    deleted gas also lowers the satellites' total; it is the total-hot-gas bound that rules
    deletion out, and it bounds the whole z = 0 population, not each FoF group. What it cannot
    tell: that each satellite's gas reached its own FoF central, the metals that travel with the
    gas, or the amounts the formulas give (the unit tests assert those).
    """
    _, supply_only = _final_galaxies("supply_only", SUPPLY_ONLY_RUN)
    _, stripped = _final_galaxies("stripping", STRIPPING_RUN)

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
            "sage26_satellite_stripping Integration Tests",
        )
    finally:
        RUNS.cleanup()


if __name__ == "__main__":
    sys.exit(main())
