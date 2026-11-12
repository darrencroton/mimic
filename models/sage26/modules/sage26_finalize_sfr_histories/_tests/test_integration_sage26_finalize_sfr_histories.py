#!/usr/bin/env python3
"""
sage26_finalize_sfr_histories - Integration Test

Validates: the module's effect on a real run, in post_timestep of the physics pipeline.

Disk star formation samples the per-substep history SfrDisk[bin] += stars / dt on
every call, and the finaliser averages the bins into SfrDisk, SfrBulge, SfrDiskZ and
SfrBulgeZ once per FoF, as SAGE26's writer does. Without bursts in the pipeline,
the mean disk rate over the substeps is the snapshot's disk stellar mass formed over
its interval, which the package's StarFormationRate diagnostic also holds (it adds
stars / dT per substep); the two are formed in different float orders, so they agree
to float rounding, not bit for bit.

Each distinct configuration is run once and shared between the cases: the pipeline with the
finaliser at 10 substeps, the same without it, and the pipeline with the finaliser at 13.

Test cases:
  - test_only_sfr_outputs_change: with and without the finaliser, every field but the
    four averages is identical, and without it the four stay zero
  - test_disk_average_matches_formed_mass: SfrDisk equals StarFormationRate to float
    rounding at 10 and at 13 substeps (bins shared) on every Type 0 and 1 row, and is at
    least it on an orphan row, which keeps its earlier snapshots' bins; SfrBulge and
    SfrBulgeZ are zero without bursts, and SfrDiskZ is a metallicity in [0, 1]
"""

import sys
from pathlib import Path

import numpy as np

# Add tests directory to path to import framework
REPO_ROOT = Path(__file__).parent.parent.parent.parent.parent.parent
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

SFR_OUTPUTS = ("SfrDisk", "SfrBulge", "SfrDiskZ", "SfrBulgeZ")

# The merger clock zeroes the histories of the rows SAGE26 resets at joining (a galaxy that
# stays a Type 0 or 1): the histories are inherited, so without it
# they would accumulate over the snapshots. The disk scale radius sets the star-forming disk
# (reff = 3 r_s): without it no stars form
PRE_TIMESTEP = [
    ("sage26_initialise_merger_clock", "process_full_halo"),
    ("sage26_set_disk_scale_radius", "process_full_halo"),
    ("sage26_reionization", "process_full_halo"),
    ("sage26_prepare_infall_budget", "process_full_halo"),
]

# Cooling feeds the cold disk; the SF chain samples the disk history. No burst module.
GALAXY_PHYSICS = [
    ("sage26_apply_infall", "process_full_halo"),
    ("sage26_calculate_cooling_budget", "process_by_galaxy"),
    ("sage26_radio_mode_heating", "process_by_galaxy"),
    ("sage26_apply_cooling", "process_by_galaxy"),
    ("sage26_calculate_star_formation", "process_by_galaxy"),
    ("sage26_calculate_supernova_feedback", "process_by_galaxy"),
    ("sage26_apply_star_formation_supernova", "process_by_galaxy"),
    ("sage26_apply_metal_enrichment", "process_by_galaxy"),
]

# The pipeline with the finaliser, and the same without it
WITH_FINALISER = {
    "pre_timestep": PRE_TIMESTEP,
    "galaxy_physics": GALAXY_PHYSICS,
    "satellite_mergers": [],
    "post_timestep": [("sage26_finalize_sfr_histories", "process_full_halo")],
}
WITHOUT_FINALISER = {**WITH_FINALISER, "post_timestep": []}

MODEL_PARAMS = {
    "GlobalBaryonFraction": 0.17,
    "AGNrecipe": 2,
    "RadioModeEfficiency": 0.08,
    "SfrEfficiency": 0.05,
    "StarFormingDiskFactor": 3.0,
    "FeedbackReheatingEpsilon": 2.9,
    "FeedbackEjectionEfficiency": 0.3,
    "FIREmodeOn": 0,  # required by the SN module, the fixed-epsilon feedback
    "RecycleFraction": 0.43,
    "Yield": 0.025,
    "FracZleaveDisk": 0.0,
    "BulgeSizeOn": 0,
    # Required by the SF module (SFprescription also by the apply step)
    "SFprescription": 0,
    "H2RadialIntegrationOn": 1,
    "H2RadialNBins": 25,
    "H2RadialRMaxFactor": 5.0,
    "CGMrecipeOn": 0,  # required by the infall modules (the CGM recipe off)
}


def _run(with_finaliser, substeps):
    """The shared run of the pipeline, with or without the finaliser, at the substep count."""
    if not MIMIC_EXE.exists():
        raise TestSkipped("Mimic not built")
    name = f"sage26_finalize_sfr_{'with' if with_finaliser else 'without'}_{substeps}"
    run = RUNS.run(
        name,
        phase_config=WITH_FINALISER if with_finaliser else WITHOUT_FINALISER,
        model_params=MODEL_PARAMS,
        substeps=substeps,
    )
    assert run.returncode == 0, f"{name} should succeed\nStderr: {run.stderr}"
    assert check_no_memory_leaks(run.stdout, run.stderr), f"{name} reported a memory leak"
    galaxies, _ = load_binary_halos(run.output_dir / "model_z0.000_0")
    assert len(galaxies) > 0, f"{name} should write galaxies"
    return galaxies


def test_only_sfr_outputs_change():
    """
    The finaliser writes the four averages and nothing else

    Expected: identical rows in both runs; every field but the four averages is
    bit-identical; without the finaliser the four are zero; with it SfrDisk is
    positive somewhere.
    """
    raw = _run(False, 10)
    fin = _run(True, 10)

    assert len(raw) == len(fin), "both runs should write the same rows"
    assert raw.dtype == fin.dtype, "both runs should write the same fields"

    for field in raw.dtype.names:
        if field in SFR_OUTPUTS:
            continue
        same = np.array_equal(raw[field], fin[field], equal_nan=True)
        assert same, f"{field} should not depend on the finaliser"

    for field in SFR_OUTPUTS:
        assert np.all(raw[field] == 0), f"without the finaliser {field} should stay zero"
    forming = int(np.sum(fin["SfrDisk"] > 0))
    assert forming > 0, "some galaxies should form disk stars"
    print(f"  only the four averages change ({forming} forming)")


def test_disk_average_matches_formed_mass():
    """
    SfrDisk is the snapshot's mean disk rate at any substep count

    Expected: at 10 and at 13 substeps (where several substeps share a bin and the
    rate divisor is 13), SfrDisk equals StarFormationRate to float rounding on every
    row that stays a Type 0 or 1, whose histories joining zeroes, as in
    SAGE26. This pipeline has no merger pass, so orphans
    persist; an orphan keeps its histories from the snapshots before (SAGE26 resets them
    only on the main branch), so its SfrDisk is at least the snapshot's own
    StarFormationRate (StarFormationRate is reset on every row). With no burst module
    SfrBulge and SfrBulgeZ are zero; SfrDiskZ lies in [0, 1] (a disk forming from
    metal-free gas has SfrDiskZ 0).
    """
    for substeps in (10, 13):
        gal = _run(True, substeps)

        sfr = gal["StarFormationRate"].astype(np.float64)
        disk = gal["SfrDisk"].astype(np.float64)
        reset = gal["Type"] <= 1
        orphan = gal["Type"] == 2
        assert np.all(np.isfinite(disk)) and np.all(disk >= 0), "SfrDisk is finite, >= 0"
        assert np.sum(reset) > 0, "the run should write Type 0 and 1 rows"
        assert np.allclose(
            disk[reset], sfr[reset], rtol=1e-4, atol=1e-10
        ), f"SfrDisk should equal StarFormationRate to float rounding ({substeps} substeps)"
        assert np.all(
            disk[orphan] >= sfr[orphan] * (1.0 - 1e-4) - 1e-10
        ), f"an orphan's kept histories hold at least the snapshot's rate ({substeps} substeps)"
        assert np.all(gal["SfrBulge"] == 0), "no burst module: SfrBulge is zero"
        assert np.all(gal["SfrBulgeZ"] == 0), "no burst module: SfrBulgeZ is zero"
        z = gal["SfrDiskZ"]
        assert np.all((z >= 0) & (z <= 1)), "SfrDiskZ is a metallicity in [0, 1]"
        forming = disk > 0
        assert np.sum(forming) > 0, f"some galaxies should form disk stars ({substeps} substeps)"
        worst = np.max(np.abs(disk[reset] - sfr[reset]) / np.maximum(sfr[reset], 1e-30))
        kept = int(np.sum(orphan & (disk > sfr * (1.0 + 1e-4) + 1e-10)))
        print(
            f"  {substeps} substeps: {int(np.sum(forming))} of {len(gal)} forming, "
            f"max relative difference {worst:.2e} on {int(np.sum(reset))} Type 0/1 rows; "
            f"{kept} of {int(np.sum(orphan))} orphan rows carry earlier snapshots' rates"
        )


def main():
    """Run this file's tests via the shared framework runner."""
    try:
        return run_test_suite(
            [test_only_sfr_outputs_change, test_disk_average_matches_formed_mass],
            "sage26_finalize_sfr_histories Integration Tests",
        )
    finally:
        RUNS.cleanup()


if __name__ == "__main__":
    sys.exit(main())
