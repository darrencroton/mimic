#!/usr/bin/env python3
"""
SAGE26 Reincorporation Module - Integration Test

Validates sage26_reincorporation in a supply pipeline on the committed mini-millennium trees.

Ejected gas comes only from supernova ejection, so reincorporation has nothing to move unless the
pipeline forms stars first: cooling (with radio-mode heating) feeds the disk, the star-formation
chain ejects gas and sage26_apply_metal_enrichment enriches it. The same pipeline is run with and
without the module, so the comparison isolates what it does.

Test cases:
  - test_reincorporation_moves_ejected_gas_to_hot_gas: with the module the ejected gas and its
    metals fall and the hot gas and its metals rise in total, while the total baryon mass is
    unchanged

Author: Mimic Development Team
Date: 2025-12-18
"""

import sys
from pathlib import Path

import numpy as np

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

# The supply pipeline: the infall and, with the module under test, reincorporation, then cooling,
# the star-formation chain (SFprescription 0) and the metal yield
PRE_TIMESTEP = [
    ("sage26_set_disk_scale_radius", "process_full_halo"),
    ("sage26_reionization", "process_full_halo"),
    ("sage26_prepare_infall_budget", "process_full_halo"),
]
INFALL = [("sage26_apply_infall", "process_full_halo")]
REINCORPORATION = [("sage26_reincorporation", "process_full_halo")]
COOLING_AND_STARS = [
    ("sage26_calculate_cooling_budget", "process_by_galaxy"),
    ("sage26_radio_mode_heating", "process_by_galaxy"),
    ("sage26_apply_cooling", "process_by_galaxy"),
    ("sage26_calculate_star_formation", "process_by_galaxy"),
    ("sage26_calculate_supernova_feedback", "process_by_galaxy"),
    ("sage26_apply_star_formation_supernova", "process_by_galaxy"),
    ("sage26_apply_metal_enrichment", "process_by_galaxy"),
]


def pipeline(galaxy_physics):
    """The run file's phases around a galaxy_physics list."""
    return {
        "pre_timestep": PRE_TIMESTEP,
        "galaxy_physics": galaxy_physics,
        "satellite_mergers": [],
        "post_timestep": [],
    }


WITHOUT_REINCORPORATION = pipeline(INFALL + COOLING_AND_STARS)
WITH_REINCORPORATION = pipeline(INFALL + REINCORPORATION + COOLING_AND_STARS)

PARAMS_WITHOUT_REINCORPORATION = {
    "GlobalBaryonFraction": 0.17,
    "CGMrecipeOn": 0,
    "AGNrecipe": 2,
    "RadioModeEfficiency": 0.08,
    "BulgeSizeOn": 0,
    "SFprescription": 0,
    "H2RadialIntegrationOn": 1,
    "H2RadialNBins": 25,
    "H2RadialRMaxFactor": 5.0,
    "SfrEfficiency": 0.02,
    "StarFormingDiskFactor": 3.0,
    "FeedbackReheatingEpsilon": 3.0,
    "FeedbackEjectionEfficiency": 0.3,
    "FIREmodeOn": 0,
    "RecycleFraction": 0.43,
    "Yield": 0.025,
    "FracZleaveDisk": 0.0,
}
PARAMS_WITH_REINCORPORATION = {**PARAMS_WITHOUT_REINCORPORATION, "ReIncorporationFactor": 1.0}

# Baryon reservoirs whose total the run conserves
BARYON_FIELDS = ["ColdGas", "HotGas", "EjectedGas", "StellarMass", "BlackHoleMass", "ICS", "CGMgas"]


def _snapshot(name, phase_config, model_params):
    """Run (once) a configuration and return its z=0 galaxies."""
    if not MIMIC_EXE.exists():
        raise TestSkipped("Mimic not built")
    run = RUNS.run(name, phase_config=phase_config, model_params=model_params)
    assert run.returncode == 0, f"Mimic should execute successfully\nSTDERR: {run.stderr}"
    assert check_no_memory_leaks(run.stdout, run.stderr), "Should not have memory leaks"
    halos, _metadata = load_binary_halos(run.output_dir / "model_z0.000_0")
    return halos


def _total(halos, field):
    return float(np.sum(halos[field], dtype=np.float64))


def test_reincorporation_moves_ejected_gas_to_hot_gas():
    """Reincorporation returns ejected gas and its metals to the hot halo, conserving baryons.

    Both runs form stars and eject gas, so the without-module run holds ejected gas to be
    reincorporated. Only a halo with Vvir above 354.53 km/s reincorporates, so the change is a
    fraction of the ejected total, but it must be there, in the right direction, and it must not
    create or destroy baryons. Cooling and star formation downstream see the extra hot gas, which
    is why the baryon total, not the individual reservoirs, is the conserved quantity.
    """
    without = _snapshot(
        "reincorporation_absent", WITHOUT_REINCORPORATION, PARAMS_WITHOUT_REINCORPORATION
    )
    with_module = _snapshot(
        "reincorporation_present", WITH_REINCORPORATION, PARAMS_WITH_REINCORPORATION
    )
    assert len(with_module) == len(without), "the two runs must write the same galaxies"

    ejected_without = _total(without, "EjectedGas")
    assert ejected_without > 0.0, "the supply pipeline must leave ejected gas to reincorporate"
    assert np.count_nonzero(without["MetalsEjectedGas"]) > 0, "the ejected gas must be enriched"

    ejected_with = _total(with_module, "EjectedGas")
    hot_without, hot_with = _total(without, "HotGas"), _total(with_module, "HotGas")
    print(f"  ejected gas {ejected_without:.4f} -> {ejected_with:.4f}")
    print(f"  hot gas {hot_without:.4f} -> {hot_with:.4f}")
    assert ejected_with < ejected_without, "reincorporation should lower the ejected gas"
    assert hot_with > hot_without, "reincorporation should raise the hot gas"
    assert _total(with_module, "MetalsEjectedGas") < _total(
        without, "MetalsEjectedGas"
    ), "reincorporation should lower the ejected metals"
    assert _total(with_module, "MetalsHotGas") > _total(
        without, "MetalsHotGas"
    ), "reincorporation should raise the hot metals"

    baryons_without = sum(_total(without, field) for field in BARYON_FIELDS)
    baryons_with = sum(_total(with_module, field) for field in BARYON_FIELDS)
    assert np.isclose(
        baryons_with, baryons_without, rtol=1e-6, atol=0.0
    ), f"baryons must be conserved: {baryons_without:.6f} without, {baryons_with:.6f} with"


def main():
    """Run this file's tests via the shared framework runner."""
    try:
        return run_test_suite(
            [test_reincorporation_moves_ejected_gas_to_hot_gas],
            "sage26_reincorporation Integration (test_integration_sage26_reincorporation.py)",
        )
    finally:
        RUNS.cleanup()


if __name__ == "__main__":
    sys.exit(main())
