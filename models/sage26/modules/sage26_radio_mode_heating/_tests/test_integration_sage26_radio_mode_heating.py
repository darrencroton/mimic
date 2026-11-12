#!/usr/bin/env python3
"""
sage26_radio_mode_heating - Integration Test

Validates the radio-mode AGN module through real Mimic runs on the committed test trees (CGMrecipeOn 0,
the only setting in which this module acts).

One pipeline, the gas pipeline: infall, cooling, the star-formation chain and the disk
instability's quasar-mode growth (which seeds the black holes the radio mode heats with) around
the radio mode. Without the infall the radio mode is inert (no hot gas), so no run omits it. It is
run at RadioModeEfficiency 0.08 with the Bondi recipe, with the radio mode off (AGNrecipe 0) and at
the higher efficiency 0.20, each configured once and shared between the cases that need it.

Test cases:
  - test_agn_physics_correctness: on the Bondi run the run reports no memory leak, the outputs are
    finite and non-negative, black holes exist in massive halos, centrals hold hot gas below their
    halo mass, and radio-mode heating acts on some galaxies
  - test_parameter_sensitivity: AGNrecipe 0 heats no row and seeds no black hole; the two
    efficiencies heat some rows and give different Heating and BlackHoleMass

Run (from the repository root, after `make MODEL=sage26 SIMULATION=mini-millennium`):
  MODEL=sage26 SIMULATION=mini-millennium python3 \\
    models/sage26/modules/sage26_radio_mode_heating/_tests/test_integration_sage26_radio_mode_heating.py
"""

import sys
from pathlib import Path

import numpy as np

REPO_ROOT = Path(__file__).resolve().parents[5]
sys.path.insert(0, str(REPO_ROOT / "tests"))
from framework import (  # noqa: E402
    MIMIC_EXE,
    SharedRuns,
    TestSkipped,
    check_no_memory_leaks,
    load_binary_halos,
    run_test_suite,
)

RUNS = SharedRuns()

# The gas supply, the star-formation chain and the disk instability's quasar-mode growth, which
# seeds the black holes the radio mode heats with
GAS_PHASES = {
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
        ("sage26_calculate_supernova_feedback", "process_by_galaxy"),
        ("sage26_apply_star_formation_supernova", "process_by_galaxy"),
        ("sage26_disk_instability", "process_by_galaxy"),
        ("sage26_quasar_mode", "process_by_galaxy"),
    ],
    "satellite_mergers": [],
    "post_timestep": [],
}


def gas_params(radio_mode_efficiency, agn_recipe):
    """The gas pipeline's parameters (SFprescription 0) with the two radio-mode parameters."""
    return {
        "RadioModeEfficiency": radio_mode_efficiency,
        "AGNrecipe": agn_recipe,
        "GlobalBaryonFraction": 0.17,
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
        "BlackHoleGrowthRate": 0.01,
        "QuasarModeEfficiency": 0.001,
        "CGMrecipeOn": 0,
    }


GAS_BONDI = ("gas_bondi", GAS_PHASES, gas_params(0.08, 2))
GAS_OFF = ("gas_off", GAS_PHASES, gas_params(0.08, 0))
GAS_HIGH_EFFICIENCY = ("gas_high_efficiency", GAS_PHASES, gas_params(0.20, 2))


def run_configuration(name, phases, params):
    """Run (once) and return the named configuration, asserting a clean exit."""
    if not MIMIC_EXE.exists():
        raise TestSkipped("Mimic not built")
    run = RUNS.run(name, phase_config=phases, model_params=params)
    assert run.returncode == 0, f"Mimic should execute successfully\nSTDERR: {run.stderr}"
    return run


def load_z0_halos(configuration):
    """The z = 0 output rows of a configuration."""
    run = run_configuration(*configuration)
    halos, _metadata = load_binary_halos(run.output_dir / "model_z0.000_0")
    assert len(halos) > 0, "Should have output halos"
    return halos


def test_agn_physics_correctness():
    """AGN physics is physically reasonable on the gas pipeline.

    Validates:
    - the run reports no memory leak
    - the black hole, hot gas and Heating outputs are finite and non-negative
    - black holes exist in massive halos and are a small fraction of the halo mass
    - centrals hold hot gas, bounded by the halo mass
    - radio-mode heating acts on some galaxies

    The radio-mode rate is proportional to the black hole mass and acts only on cooling hot gas, so
    the pipeline supplies both: infall and cooling give the hot and cold gas, the star-formation
    chain the stellar disk, and the disk instability's quasar-mode growth seeds the black holes.

    The hot gas bound applies to centrals: a central's hot gas is part of its FoF group's baryons,
    which the infall budget sets to HaloBaryonFraction * Mvir (sage26_prepare_infall_budget), far
    below Mvir. A satellite keeps the hot gas it held as a central while its subhalo's Mvir is
    stripped (only sage26_satellite_stripping, not in this pipeline, removes it), so its hot gas
    can exceed its current Mvir (up to 2.3 times on the test trees) and is not bounded by it.
    """
    run = run_configuration(*GAS_BONDI)
    assert check_no_memory_leaks(run.stdout, run.stderr), "Should not have memory leaks"
    halos = load_z0_halos(GAS_BONDI)

    bh_mass = halos["BlackHoleMass"]
    hot_gas = halos["HotGas"]
    heating = halos["Heating"]
    mvir = halos["Mvir"]

    for name, values in (("BlackHoleMass", bh_mass), ("HotGas", hot_gas), ("Heating", heating)):
        assert np.all(np.isfinite(values)), f"{name} should be finite"
        assert np.all(values >= 0.0), f"{name} should be non-negative"

    # Typical M_BH / M_halo is 1e-3 to 1e-5
    bh_in_massive_halos = (mvir > 10.0) & (bh_mass > 0)
    assert np.sum(bh_in_massive_halos) > 0, "massive halos should host black holes"
    bh_fraction = bh_mass[bh_in_massive_halos] / mvir[bh_in_massive_halos]
    assert np.all(bh_fraction < 0.1), "BH mass should be < 10% of halo mass (unreasonably high)"
    print(f"  BH mass fraction in massive halos: {np.mean(bh_fraction):.4e}")

    centrals = (mvir > 1.0) & (halos["Type"] == 0)
    assert np.sum(centrals & (hot_gas > 0)) > 0, "some centrals should hold hot gas"
    assert np.all(hot_gas[centrals] / mvir[centrals] <= 1.0), "Hot gas cannot exceed the halo mass"

    assert np.sum(heating > 0) > 0, "radio-mode heating should act on some galaxies"


def test_parameter_sensitivity():
    """The radio-mode parameters act on the gas pipeline.

    Validates:
    - AGNrecipe 0 turns the radio mode off: no row is heated and, as in SAGE26, the quasar-mode
      growth seeds no black hole
    - RadioModeEfficiency is read and acts: 0.08 and 0.20 both heat some rows and give different
      Heating and BlackHoleMass. No direction is asserted: the radio mode's feedback on the cooling
      that feeds it and the quasar-mode growth makes the z = 0 totals non-monotonic
    """
    off = load_z0_halos(GAS_OFF)
    middle = load_z0_halos(GAS_BONDI)
    high = load_z0_halos(GAS_HIGH_EFFICIENCY)

    for name, halos in (("off", off), ("0.08", middle), ("0.20", high)):
        print(
            f"  {name}: BlackHoleMass {np.sum(halos['BlackHoleMass']):.3e}, "
            f"Heating {np.sum(halos['Heating']):.3e} on {int(np.sum(halos['Heating'] > 0))} rows"
        )

    assert np.all(off["Heating"] == 0.0), "AGNrecipe 0 should heat no row"
    assert np.all(off["BlackHoleMass"] == 0.0), "AGNrecipe 0 should seed no black hole"
    assert np.sum(middle["Heating"]) > 0, "the efficiency 0.08 should heat some rows"
    assert np.sum(high["Heating"]) > 0, "the efficiency 0.20 should heat some rows"
    for field in ("Heating", "BlackHoleMass"):
        assert not np.array_equal(
            middle[field], high[field]
        ), f"RadioModeEfficiency should change {field}"


def main():
    """Run this file's tests via the shared framework runner."""
    try:
        return run_test_suite(
            [test_agn_physics_correctness, test_parameter_sensitivity],
            "sage26_radio_mode_heating Integration Tests",
        )
    finally:
        RUNS.cleanup()


if __name__ == "__main__":
    sys.exit(main())
