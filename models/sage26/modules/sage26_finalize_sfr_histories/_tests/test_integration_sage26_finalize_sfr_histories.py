#!/usr/bin/env python3
"""
sage26_finalize_sfr_histories - Integration Test

Validates: module lifecycle in post_timestep and its effect on a real run

Disk star formation samples the per-substep history SfrDisk[bin] += stars / dt on
every call (SAGE26 model_starformation_and_feedback.c:1058-1060), and the finaliser
averages the bins into SfrDisk, SfrBulge, SfrDiskZ and SfrBulgeZ once per FoF, as
SAGE26's writer does (io/save_gals_hdf5.c:934-959). Without bursts in the pipeline,
the mean disk rate over the substeps is the snapshot's disk stellar mass formed over
its interval, which the package's StarFormationRate diagnostic also holds (it adds
stars / dT per substep); the two are formed in different float orders, so they agree
to float rounding, not bit for bit. The averaging itself is asserted bit for bit by
the module's oracle unit test.

Test cases:
  - test_module_loads: module registers and initialises in post_timestep
  - test_only_sfr_outputs_change: with and without the finaliser, every field but the
    four averages is identical, and without it the four stay zero
  - test_disk_average_matches_formed_mass: SfrDisk equals StarFormationRate to float
    rounding at 10 and at 13 substeps (bins shared) on every Type 0 and 1 row, and is at
    least it on an orphan row, which keeps its earlier snapshots' bins; SfrBulge and
    SfrBulgeZ are zero without bursts, and SfrDiskZ is a metallicity in [0, 1]
"""

import shutil
import sys
from pathlib import Path

# Add tests directory to path to import framework
REPO_ROOT = Path(__file__).parent.parent.parent.parent.parent.parent
sys.path.insert(0, str(REPO_ROOT / "tests"))

from framework import (  # noqa: E402
    BLUE,
    GREEN,
    MIMIC_EXE,
    NC,
    RED,
    TestSkipped,
    create_test_param_file,
    load_binary_halos,
    result_error,
    result_fail,
    result_pass,
    result_skip,
    run_mimic,
)

SFR_OUTPUTS = ("SfrDisk", "SfrBulge", "SfrDiskZ", "SfrBulgeZ")

# The merger clock zeroes the histories of the rows SAGE26 resets at joining (a galaxy that
# stays a Type 0 or 1, core_build_model.c:278-282): the histories are inherited, so without it
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

MODEL_PARAMS = {
    "GlobalBaryonFraction": 0.17,
    "AGNrecipe": 2,
    "RadioModeEfficiency": 0.08,
    "SfrEfficiency": 0.05,
    "StarFormingDiskFactor": 3.0,
    "FeedbackReheatingEpsilon": 2.9,
    "FeedbackEjectionEfficiency": 0.3,
    "RecycleFraction": 0.43,
    "Yield": 0.025,
    "FracZleaveDisk": 0.0,
    "BulgeSizeOn": 0,
    # Required by the SF module (and SFprescription by the apply step), at R1's values
    "SFprescription": 0,
    "H2RadialIntegrationOn": 1,
    "H2RadialNBins": 25,
    "H2RadialRMaxFactor": 5.0,
}


def _run(output_name, with_finaliser, substeps=None):
    post = [("sage26_finalize_sfr_histories", "process_full_halo")] if with_finaliser else []
    param_file, output_dir, temp_dir = create_test_param_file(
        output_name=output_name,
        phase_config={
            "pre_timestep": PRE_TIMESTEP,
            "galaxy_physics": GALAXY_PHYSICS,
            "satellite_mergers": [],
            "post_timestep": post,
        },
        model_params=MODEL_PARAMS,
        substeps=substeps,
    )
    returncode, stdout, stderr = run_mimic(param_file)
    return returncode, stdout, stderr, output_dir, temp_dir


def test_module_loads():
    """
    The module registers and initialises when placed in post_timestep

    Expected: the run succeeds and logs the module's initialisation
    """
    print("Testing module load and initialization...")

    returncode, stdout, stderr, _, temp_dir = _run("sage26_finalize_sfr_load", True)

    assert returncode == 0, f"Mimic should run with the finaliser\nStderr: {stderr}"
    assert (
        "SAGE finalize SFR histories module initialized" in stdout
    ), "sage26_finalize_sfr_histories should log its initialisation"

    shutil.rmtree(temp_dir)
    print("  ✓ Module loads and initializes successfully")


def test_only_sfr_outputs_change():
    """
    The finaliser writes the four averages and nothing else

    Expected: identical rows in both runs; every field but the four averages is
    bit-identical; without the finaliser the four are zero; with it SfrDisk is
    positive somewhere.
    """
    import numpy as np

    print("Testing that only the four SFR averages change...")

    rc_raw, _, err_raw, out_raw, tmp_raw = _run("sage26_finalize_sfr_raw", False)
    rc_fin, _, err_fin, out_fin, tmp_fin = _run("sage26_finalize_sfr_avg", True)
    assert rc_raw == 0, f"run without the finaliser should succeed\nStderr: {err_raw}"
    assert rc_fin == 0, f"run with the finaliser should succeed\nStderr: {err_fin}"

    raw, _ = load_binary_halos(out_raw / "model_z0.000_0")
    fin, _ = load_binary_halos(out_fin / "model_z0.000_0")

    assert len(raw) == len(fin) and len(raw) > 0, "both runs should write the same rows"
    assert raw.dtype == fin.dtype, "both runs should write the same fields"

    for field in raw.dtype.names:
        if field in SFR_OUTPUTS:
            continue
        same = np.array_equal(raw[field], fin[field], equal_nan=True)
        assert same, f"{field} should not depend on the finaliser"

    for field in SFR_OUTPUTS:
        assert np.all(raw[field] == 0), f"without the finaliser {field} should stay zero"
    assert np.sum(fin["SfrDisk"] > 0) > 0, "some galaxies should form disk stars"

    shutil.rmtree(tmp_raw)
    shutil.rmtree(tmp_fin)
    print(f"  ✓ Only the four averages change ({int(np.sum(fin['SfrDisk'] > 0))} forming)")


def test_disk_average_matches_formed_mass():
    """
    SfrDisk is the snapshot's mean disk rate at any substep count

    Expected: at 10 and at 13 substeps (where several substeps share a bin and the
    rate divisor is 13), SfrDisk equals StarFormationRate to float rounding on every
    row that stays a Type 0 or 1, whose histories joining zeroes (SAGE26
    core_build_model.c:252, :278-282). This pipeline has no merger pass, so orphans
    persist; an orphan keeps its histories from the snapshots before (SAGE26 resets them
    only on the main branch, :322-337), so its SfrDisk is at least the snapshot's own
    StarFormationRate (StarFormationRate is reset on every row). With no burst module
    SfrBulge and SfrBulgeZ are zero; SfrDiskZ lies in [0, 1] (a disk forming from
    metal-free gas has SfrDiskZ 0).
    """
    import numpy as np

    print("Testing the disk average against the formed mass...")

    for substeps in (10, 13):
        rc, _, err, out, tmp = _run(f"sage26_finalize_sfr_steps{substeps}", True, substeps)
        assert rc == 0, f"run with {substeps} substeps should succeed\nStderr: {err}"
        gal, _ = load_binary_halos(out / "model_z0.000_0")
        assert len(gal) > 0, "the run should write galaxies"

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
        shutil.rmtree(tmp)

    print("  ✓ SfrDisk is the mean disk rate at 10 and 13 substeps")


def main():
    """
    Main test runner

    Executes all test cases and reports results.
    """
    print(f"{BLUE}{'=' * 60}{NC}")
    print(f"{BLUE}Test Suite: sage26_finalize_sfr_histories Integration Tests{NC}")
    print(f"{BLUE}{'=' * 60}{NC}")

    tests = [
        test_module_loads,
        test_only_sfr_outputs_change,
        test_disk_average_matches_formed_mass,
    ]

    passed = 0
    failed = 0
    skipped = 0

    if not MIMIC_EXE.exists():
        for test in tests:
            result_skip(test.__name__, "Mimic not built")
        return 0

    for test in tests:
        print()
        try:
            test()
            result_pass(test.__name__)
            passed += 1
        except TestSkipped as e:
            result_skip(test.__name__, str(e))
            skipped += 1
        except AssertionError as e:
            result_fail(test.__name__, str(e).splitlines()[0])
            failed += 1
        except Exception as e:
            result_error(test.__name__, str(e).splitlines()[0])
            failed += 1

    print()
    print(f"{BLUE}{'=' * 60}{NC}")
    print(f"{BLUE}Test Summary{NC}")
    print(f"{BLUE}{'=' * 60}{NC}")
    print(f"Passed:  {passed}")
    if skipped:
        print(f"Skipped: {skipped}")
    print(f"Failed:  {failed}")
    print(f"Total:   {passed + failed + skipped}")
    print(f"{BLUE}{'=' * 60}{NC}")
    print()

    if failed == 0:
        print(f"{GREEN}✓ All tests passed!{NC}")
        return 0
    else:
        print(f"{RED}✗ {failed} test(s) failed{NC}")
        return 1


if __name__ == "__main__":
    sys.exit(main())
