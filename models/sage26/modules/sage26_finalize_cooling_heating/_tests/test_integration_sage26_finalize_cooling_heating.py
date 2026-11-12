#!/usr/bin/env python3
"""
sage26_finalize_cooling_heating - Integration Test

Validates: the module's effect on a real run of mimic on the committed test trees.

The module normalises the Cooling, Heating and SupernovaOutflowRate sums, accumulated undivided over
the substeps by sage26_apply_cooling and sage26_radio_mode_heating (and the feedback modules), once
per FoF by the first galaxy's snapshot interval dT, as SAGE26 does. The run with the finaliser must
therefore differ from the same run without it only in those fields, and each output Cooling, a
log10 of the energy rate, must rise by exactly -log10(dT) with dT in code time.

Pipeline (CGMrecipeOn 0, the cooling chain without star formation):
  pre_timestep: sage26_reionization, sage26_prepare_infall_budget
  galaxy_physics: sage26_apply_infall, sage26_calculate_cooling_budget,
                  sage26_radio_mode_heating, sage26_apply_cooling
  post_timestep: sage26_finalize_cooling_heating (the second run only)

The chain has no star formation, so SupernovaOutflowRate is zero in both runs and the AGN heating
is zero on these trees; the case asserts that both stay unchanged and puts its numeric assertion on
Cooling, which is positive for over half the output rows. The exact SupernovaOutflowRate and
Heating normalisation is asserted by the module's C unit test.

Test cases:
  - test_only_energies_change: with and without the finaliser, every field but Cooling and Heating
    is bit-identical, a zero energy stays zero, and every positive Cooling rises by -log10(dT)
    of its FoF's first galaxy (its central), to 1e-9
"""

import sys
from pathlib import Path

import numpy as np

REPO_ROOT = Path(__file__).resolve().parents[5]
sys.path.insert(0, str(REPO_ROOT / "tests"))

from framework import (  # noqa: E402
    SharedRuns,
    check_no_memory_leaks,
    load_binary_halos,
    run_test_suite,
)

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "_tests"))
from sage26_run_helpers import require_mimic, require_pair  # noqa: E402

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
    "CGMrecipeOn": 0,  # required by the infall modules (the CGM recipe off)
}

WITHOUT_FINALISER = {
    "pre_timestep": PRE_TIMESTEP,
    "galaxy_physics": COOLING_CHAIN,
    "satellite_mergers": [],
    "post_timestep": [],
}
WITH_FINALISER = {
    **WITHOUT_FINALISER,
    "post_timestep": [("sage26_finalize_cooling_heating", "process_full_halo")],
}

# mini-Millennium's code time unit in seconds (UnitLength 3.08568e24 cm over UnitVelocity 1e5 cm/s)
# and the seconds per megayear of physical_constants.h: the output dT is dT_code * UNIT_TIME_IN_S /
# SEC_PER_MEGAYEAR in Myr
UNIT_TIME_IN_S = 3.08568e24 / 1.0e5
SEC_PER_MEGAYEAR = 3.155e13

RUNS = SharedRuns()


def run_snapshot(name, phases):
    """The z = 0 output of the named run, after requiring a clean exit and no leak report."""
    require_pair()
    require_mimic()
    run = RUNS.run(name, phase_config=phases, model_params=MODEL_PARAMS)
    assert run.returncode == 0, f"{name}: mimic exited {run.returncode}\nStderr: {run.stderr}"
    assert check_no_memory_leaks(run.stdout, run.stderr), f"{name}: mimic reported a memory leak"
    halos, _metadata = load_binary_halos(run.output_dir / "model_z0.000_0")
    assert len(halos) > 0, f"{name}: no output rows"
    return halos


def test_only_energies_change():
    """
    The finaliser changes Cooling and Heating only, and raises Cooling by -log10(dT)

    Expected: identical rows in both runs; every field but Cooling and Heating is bit-identical; a
    zero energy stays zero; a positive Cooling (log10 of an energy rate) is larger by exactly
    -log10 of the first galaxy's dT in code time, which is the dT of the row's central: the
    output dT is in Myr, so dT_code = dT_Myr * SEC_PER_MEGAYEAR / UNIT_TIME_IN_S.
    """
    raw = run_snapshot("sage26_finalize_raw", WITHOUT_FINALISER)
    fin = run_snapshot("sage26_finalize_norm", WITH_FINALISER)

    assert len(raw) == len(fin), "both runs should write the same rows"
    assert raw.dtype == fin.dtype, "both runs should write the same fields"

    for field in raw.dtype.names:
        if field in ("Cooling", "Heating"):
            continue
        same = np.array_equal(raw[field], fin[field], equal_nan=True)
        assert same, f"{field} should not depend on the finaliser"

    for field in ("Cooling", "Heating"):
        zero = raw[field] == 0
        assert np.all(fin[field][zero] == 0), f"a zero {field} should stay zero"

    # The FoF's first galaxy is its central: each row's interval is its central's dT
    central_dT = {
        int(galaxy_id): dT
        for galaxy_id, dT, galaxy_type in zip(
            raw["UniqueGalaxyID"], raw["dT"], raw["Type"], strict=True
        )
        if galaxy_type == 0
    }
    positive = raw["Cooling"] > 0
    assert positive.sum() > 0, "some galaxies should cool"
    first_dT_myr = np.array([central_dT[int(c)] for c in raw["UniqueCentralGalaxyID"][positive]])
    first_dT_code = first_dT_myr * SEC_PER_MEGAYEAR / UNIT_TIME_IN_S
    rise = fin["Cooling"][positive] - raw["Cooling"][positive]
    worst = float(np.max(np.abs(rise + np.log10(first_dT_code))))
    print(
        f"  Cooling: {int(positive.sum())} of {len(raw)} galaxies, worst |rise + log10(dT)| {worst:.2e}"
    )
    assert worst < 1e-9, f"Cooling must rise by -log10(dT of the first galaxy); worst error {worst}"


def main():
    try:
        return run_test_suite(
            [test_only_energies_change], "sage26_finalize_cooling_heating Integration Tests"
        )
    finally:
        RUNS.cleanup()


if __name__ == "__main__":
    sys.exit(main())
