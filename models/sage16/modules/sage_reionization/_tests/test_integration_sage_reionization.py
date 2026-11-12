#!/usr/bin/env python3
"""
SAGE Reionization Module - Integration Test

Validates: the GlobalBaryonFraction parameter and the HaloBaryonFraction the module writes, on
real halos, and that the run reports no memory leak

The module runs alone in pre_timestep, so the baryon reservoirs stay empty and the field to
assert on is HaloBaryonFraction = GlobalBaryonFraction / (1 + 0.26 M_c / Mvir)^3, where M_c is
one filtering mass for every halo at the output redshift.

Test cases:
  - test_property_values_physical: HaloBaryonFraction has the Gnedin form with the filtering mass
    of Kravtsov et al. (2004) at z = 0 and rises with Mvir; the run reports no memory leak
  - test_parameters_configurable: HaloBaryonFraction scales with GlobalBaryonFraction

Author: Mimic Development Team
Date: 2025-12-17 (Refactored)
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

# The module alone in pre_timestep
PIPELINE = {
    "pre_timestep": [("sage_reionization", "process_full_halo")],
    "galaxy_physics": [],
    "satellite_mergers": [],
    "post_timestep": [],
}
BASELINE_PARAMS = {"GlobalBaryonFraction": 0.17}
SCALED_PARAMS = {"GlobalBaryonFraction": 0.20}

# 0.26 times the Kravtsov et al. (2004) Appendix B filtering mass at z = 0 for Omega = 0.25,
# 0.26 * 25 / sqrt(Omega) * 2.21 * f(a = 1)^1.5 in 1e10 Msun/h, hand-evaluated: the value of
# Mvir * ((GlobalBaryonFraction / HaloBaryonFraction)^(1/3) - 1) for every halo
FILTERING_MASS_CONSTANT = 0.848942830


def _require_mimic():
    if not MIMIC_EXE.exists():
        raise TestSkipped("Mimic not built")


def _baseline_run():
    return RUNS.run("reionization_baseline", phase_config=PIPELINE, model_params=BASELINE_PARAMS)


def _load(run):
    assert run.returncode == 0, f"Mimic should execute successfully\nSTDERR: {run.stderr}"
    halos, _metadata = load_binary_halos(run.output_dir / "model_z0.000_0")
    assert len(halos) > 0, "Should have halos in output"
    return halos


def test_property_values_physical():
    """HaloBaryonFraction has the Gnedin form with the z = 0 filtering mass.

    For every halo with Mvir > 0 (centrals and satellites alike), the cube root of
    GlobalBaryonFraction / HaloBaryonFraction minus one, times Mvir, is the same constant: a wrong
    power or a halo not using its own Mvir breaks it. The constant itself is 0.26 times the
    filtering mass at z = 0, and HaloBaryonFraction rises with Mvir towards GlobalBaryonFraction.
    Halos with Mvir = 0 (orphans) receive 0. The run's own leak report is clean.
    """
    _require_mimic()
    run = _baseline_run()
    halos = _load(run)
    assert check_no_memory_leaks(run.stdout, run.stderr), "Should not have memory leaks"
    global_baryon_fraction = BASELINE_PARAMS["GlobalBaryonFraction"]

    fraction = halos["HaloBaryonFraction"]
    massive = halos["Mvir"] > 0
    assert np.all(fraction[~massive] == 0.0), "Halos with Mvir = 0 should have HaloBaryonFraction 0"
    assert np.all(fraction[massive] > 0.0), "HaloBaryonFraction should be > 0 where Mvir > 0"
    assert np.all(
        fraction[massive] < global_baryon_fraction
    ), "HaloBaryonFraction should lie below GlobalBaryonFraction"

    # Halos well below the suppression-free mass, where the constant is well conditioned
    suppressed = massive & (fraction < 0.95 * global_baryon_fraction)
    assert suppressed.sum() > 100, "Need suppressed halos to test the functional form"
    constant = halos["Mvir"][suppressed] * (
        (global_baryon_fraction / fraction[suppressed]) ** (1.0 / 3.0) - 1.0
    )
    assert np.allclose(constant, FILTERING_MASS_CONSTANT, rtol=1e-5), (
        f"Mvir * ((GlobalBaryonFraction / HaloBaryonFraction)^(1/3) - 1) should be "
        f"{FILTERING_MASS_CONSTANT} for every halo (got {constant.min():.9f} to "
        f"{constant.max():.9f})"
    )

    order = np.argsort(halos["Mvir"][massive], kind="stable")
    assert np.all(
        np.diff(fraction[massive][order]) >= 0.0
    ), "HaloBaryonFraction should not decrease with Mvir"


def test_parameters_configurable():
    """HaloBaryonFraction is GlobalBaryonFraction times a factor independent of it.

    Raising GlobalBaryonFraction from 0.17 to 0.20 scales every halo's HaloBaryonFraction by
    0.20 / 0.17, so the largest value exceeds the 0.17 the run would give if the parameter were
    ignored.
    """
    _require_mimic()
    baseline = _load(_baseline_run())
    scaled = _load(
        RUNS.run("reionization_scaled", phase_config=PIPELINE, model_params=SCALED_PARAMS)
    )

    ratio = SCALED_PARAMS["GlobalBaryonFraction"] / BASELINE_PARAMS["GlobalBaryonFraction"]
    assert np.allclose(
        scaled["HaloBaryonFraction"], ratio * baseline["HaloBaryonFraction"], rtol=1e-6, atol=0.0
    ), "HaloBaryonFraction should scale with GlobalBaryonFraction"
    assert (
        scaled["HaloBaryonFraction"].max() > BASELINE_PARAMS["GlobalBaryonFraction"]
    ), "GlobalBaryonFraction 0.20 should raise HaloBaryonFraction above 0.17"


def main():
    """Run this file's tests via the shared framework runner."""
    try:
        return run_test_suite(
            [test_property_values_physical, test_parameters_configurable],
            "sage_reionization Integration Tests",
        )
    finally:
        RUNS.cleanup()


if __name__ == "__main__":
    sys.exit(main())
