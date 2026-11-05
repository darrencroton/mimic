#!/usr/bin/env python3
"""
Immediate merger event consumer integration tests.

Validates the production runtime chain:
sage26_resolve_mergers_and_disruption -> module_emit_event() -> process_per_event consumers,
the written merger timestamps (SAGE26's lookback times times UnitTime_in_Megayears, the
never-merged -1 included), and the bound SAGE26's arithmetic puts on the merger
transaction's bulge writes.
"""

import shutil
import sys
from pathlib import Path

import numpy as np
import yaml
from scipy.integrate import quad

# Add tests directory to path to import framework
# This test is one level deeper than models/sage26/modules/_tests/ so needs 5 parent hops
REPO_ROOT = Path(__file__).parent.parent.parent.parent.parent.parent
sys.path.insert(0, str(REPO_ROOT / "tests"))

from framework import (
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

MERGER_MODEL_PARAMS = {
    "BulgeSizeOn": 0,
    "GlobalBaryonFraction": 0.17,
    "SfrEfficiency": 0.05,
    "StarFormingDiskFactor": 3.0,
    "FeedbackReheatingEpsilon": 3.0,
    "FeedbackEjectionEfficiency": 0.3,
    "ReIncorporationFactor": 0.15,
    "AGNrecipe": 2,
    "RadioModeEfficiency": 0.08,
    "BlackHoleGrowthRate": 0.015,
    "QuasarModeEfficiency": 0.005,
    "RecycleFraction": 0.43,
    "Yield": 0.025,
    "FracZleaveDisk": 0.0,
    "ThresholdMajorMerger": 0.3,
    "ThresholdSatDisruption": 1.0,
    # Required by the SF module, the apply step and the burst module, at R1's values
    "SFprescription": 0,
    "H2RadialIntegrationOn": 1,
    "H2RadialNBins": 25,
    "H2RadialRMaxFactor": 5.0,
    "StarburstColdGasOn": 1,
}

PRE_TIMESTEP_PHASE = [
    ("sage26_reionization", "process_full_halo"),
    ("sage26_prepare_infall_budget", "process_full_halo"),
    ("sage26_set_disk_scale_radius", "process_full_halo"),
    ("sage26_initialise_merger_clock", "process_full_halo"),
]

GALAXY_PHYSICS_MODULES = [
    ("sage26_apply_infall", "process_full_halo"),
    ("sage26_reincorporation", "process_full_halo"),
    ("sage26_satellite_stripping", "process_by_galaxy"),
    ("sage26_calculate_cooling_budget", "process_by_galaxy"),
    ("sage26_radio_mode_heating", "process_by_galaxy"),
    ("sage26_apply_cooling", "process_by_galaxy"),
    ("sage26_calculate_star_formation", "process_by_galaxy"),
    ("sage26_calculate_supernova_feedback", "process_by_galaxy"),
    ("sage26_apply_star_formation_supernova", "process_by_galaxy"),
    ("sage26_disk_instability", "process_by_galaxy"),
    ("sage26_quasar_mode", "process_by_galaxy"),
    ("sage26_starburst_feedback", "process_by_galaxy"),
]


def build_phase_config(
    merger_mode="process_full_halo", enable_phase2_quasar=True, enable_phase2_starburst=True
):
    """Build the smallest deterministic pipeline that exercises merger events."""
    satellite_mergers = [("sage26_resolve_mergers_and_disruption", merger_mode)]
    if enable_phase2_quasar:
        satellite_mergers.append(("sage26_quasar_mode", "process_per_event"))
    if enable_phase2_starburst:
        satellite_mergers.append(("sage26_starburst_feedback", "process_per_event"))

    return {
        "pre_timestep": PRE_TIMESTEP_PHASE,
        "galaxy_physics": GALAXY_PHYSICS_MODULES,
        "satellite_mergers": satellite_mergers,
        "post_timestep": [],
    }


def run_merger_pipeline(
    output_name,
    merger_mode="process_full_halo",
    enable_phase2_quasar=True,
    enable_phase2_starburst=True,
):
    """Execute the immediate-merger pipeline on the standard test dataset."""
    return create_test_param_file(
        output_name=output_name,
        phase_config=build_phase_config(
            merger_mode=merger_mode,
            enable_phase2_quasar=enable_phase2_quasar,
            enable_phase2_starburst=enable_phase2_starburst,
        ),
        model_params=MERGER_MODEL_PARAMS,
    )


def load_output_halos(output_dir):
    """Load the single-snapshot binary output for a test run."""
    output_file = output_dir / "model_z0.000_0"
    halos, _ = load_binary_halos(output_file)
    return halos


def merger_remnant_mask(halos):
    """Select galaxies that recorded a merger in the current run."""
    return (halos["TimeOfLastMinorMerger"] > 0.0) | (halos["TimeOfLastMajorMerger"] > 0.0)


def common_merger_field_values(halos_a, halos_b, field_name):
    """Return aligned field values for merger remnants present in both outputs."""
    subset_a = halos_a[merger_remnant_mask(halos_a)]
    subset_b = halos_b[merger_remnant_mask(halos_b)]

    order_a = np.argsort(subset_a["UniqueGalaxyID"])
    order_b = np.argsort(subset_b["UniqueGalaxyID"])

    ids_a = subset_a["UniqueGalaxyID"][order_a]
    ids_b = subset_b["UniqueGalaxyID"][order_b]

    common_ids, idx_a, idx_b = np.intersect1d(ids_a, ids_b, return_indices=True)

    values_a = subset_a[field_name][order_a][idx_a]
    values_b = subset_b[field_name][order_b][idx_b]
    return common_ids, values_a, values_b


# The merger-bound pipeline: the supply, cooling and disk star formation of the base
# pipeline, without the disk instability and without any burst (no process_by_galaxy
# quasar or burst, no merger-event burst consumer), so the merger transaction's own
# writes are the only bulge writes. The quasar consumer stays (it writes no stars).
BOUND_GALAXY_PHYSICS_MODULES = [
    module
    for module in GALAXY_PHYSICS_MODULES
    if module[0]
    not in ("sage26_disk_instability", "sage26_quasar_mode", "sage26_starburst_feedback")
]

# SAGE26's never-merged timestamp, -1 (src/model_misc.c:179-180)
NEVER_MERGED = -1.0


def unit_time_in_megayears():
    """Mimic's UnitTime_in_Megayears, as src/core/init.c:84-85 forms it

    UnitTime_in_s = UnitLength_in_cm / UnitVelocity_in_cm_per_s, divided by
    SEC_PER_MEGAYEAR (src/module_system/physical_constants.h), in double precision, the
    same operations as SAGE26's set_units (src/core_init.c:94-95). Mimic's reference units
    are fixed (src/core/core_properties.yaml reference_units).
    """
    unit_length_in_cm = 3.08568e24
    unit_velocity_in_cm_per_s = 1.0e5
    sec_per_megayear = 3.155e13
    return (unit_length_in_cm / unit_velocity_in_cm_per_s) / sec_per_megayear


def substep_event_times(param_file):
    """Every substep midpoint lookback time of the run, in code units

    Mimic's event time is Age[SnapNum] - (step + 0.5) * dT / SubSteps, with Age the
    lookback time to each snapshot (src/core/init.c:202-235; the resolver's pass) and
    dT the interval back to the progenitor's snapshot. Returns the midpoints of every
    interval of up to five snapshots, enough for any progenitor gap of these trees.
    """
    with open(param_file) as handle:
        params = yaml.safe_load(handle)
    substeps = int(params.get("SubSteps", 1))
    with open(params["simulation"]["config"]) as handle:
        simulation = yaml.safe_load(handle)
    cosmology = simulation["simulation"]["cosmology"]
    omega = float(cosmology["omega_matter"])
    omega_lambda = float(cosmology["omega_lambda"])
    a_list = Path(simulation["input"]["snapshot_list_file"])
    if not a_list.is_absolute():
        a_list = REPO_ROOT / a_list
    scale_factors = [float(line) for line in a_list.read_text().split() if line.strip()]
    hubble = 3.2407789e-18 * 3.08568e24 / 1.0e5

    def lookback(a):
        integrand = lambda x: 1.0 / np.sqrt(
            omega / x + (1 - omega - omega_lambda) + omega_lambda * x * x
        )
        return quad(integrand, a, 1.0, epsabs=1e-13, epsrel=1e-12)[0] / hubble

    ages = [lookback(a) for a in scale_factors]
    times = []
    for snap in range(1, len(ages)):
        for gap in range(1, 6):
            if snap - gap < 0:
                break
            interval = ages[snap - gap] - ages[snap]
            for step in range(substeps):
                times.append(ages[snap - gap] - (step + 0.5) * interval / substeps)
    return np.array(sorted(times)), substeps


def run_bound_pipeline(output_name):
    """Run the merger-bound pipeline; returns (param_file, output_dir, temp_dir)"""
    phase_config = build_phase_config(enable_phase2_starburst=False)
    phase_config["galaxy_physics"] = BOUND_GALAXY_PHYSICS_MODULES
    return create_test_param_file(
        output_name=output_name, phase_config=phase_config, model_params=MERGER_MODEL_PARAMS
    )


def merger_bulge_bound_holds(stellar, bulge):
    """SAGE26's arithmetic bound on the merger transaction's bulge writes: bulge <= stellar

    Applies to BulgeMass <= StellarMass and MetalsBulgeMass <= MetalsStellarMass of every
    row of the merger-bound pipeline, exactly, with no rounding allowance.

    Why it holds. In that pipeline a galaxy's StellarMass, BulgeMass and their metals
    start at 0 (creation) and change only through:

    - disk star formation, StellarMass += (1 - RecycleFraction) * stars and its metals
      += Z * (1 - RecycleFraction) * stars, each one fused multiply-add rounded to double
      and then to float (src/model_starformation_and_feedback.c:1123-1126), with stars >= 0
      and Z >= 0 (get_metallicity): a non-negative addend, and rounding is monotone, so
      the float result is never below the old float value, and the bulge is untouched;
    - add_galaxies_together, StellarMass += p.StellarMass and BulgeMass += p.StellarMass
      (src/model_mergers.c:461, :500), and the same for the metals (:462, :501): the same
      float addend on both sides, and float addition is monotone (x <= y implies
      fl(x + s) <= fl(y + s) for any s), so bulge <= stellar survives;
    - make_bulge_from_burst, BulgeMass = StellarMass and MetalsBulgeMass =
      MetalsStellarMass (:556, :559): exact float copies, equality.

    Nothing else writes these four fields here (no burst, no disk instability, and a
    satellite's own bulge excess never reaches its target, which adds the satellite's
    StellarMass to its bulge). By induction bulge <= stellar holds after every write,
    and the copy shows the bound is attained, so it cannot be tightened. With the
    merger-event burst or the instability in the pipeline the excess can grow with the
    number of burst writes (test_integration_sage26_starburst_feedback.py,
    bulge_upper_bound), and the number of merger bursts a galaxy receives has no limit
    the run's configuration fixes, so this test isolates the merger writes.
    """
    stellar = np.asarray(stellar, dtype=np.float32)
    bulge = np.asarray(bulge, dtype=np.float32)
    return bulge <= stellar


def test_invalid_processing_mode_rejected():
    """Immediate merger handler must reject non-full-halo configuration at startup."""
    param_file, output_dir, temp_dir = run_merger_pipeline(
        output_name="merger_event_invalid_mode",
        merger_mode="process_by_galaxy",
    )

    try:
        returncode, stdout, stderr = run_mimic(param_file)
        combined_output = stdout + stderr

        assert returncode != 0, "Invalid processing mode should fail startup"
        assert "does not support processing mode 'process_by_galaxy'" in combined_output
        assert "Supported modes: process_full_halo" in combined_output
    finally:
        shutil.rmtree(temp_dir)


def test_quasar_consumer_receives_merger_events():
    """Disabling the satellite_mergers quasar consumer must remove merger-driven BH growth."""
    base_param_file, base_output_dir, base_temp_dir = run_merger_pipeline(
        output_name="merger_event_quasar_base"
    )
    no_quasar_param_file, no_quasar_output_dir, no_quasar_temp_dir = run_merger_pipeline(
        output_name="merger_event_quasar_disabled",
        enable_phase2_quasar=False,
    )

    try:
        returncode, stdout, stderr = run_mimic(base_param_file)
        assert returncode == 0, f"Baseline merger pipeline should succeed\nSTDERR: {stderr}"

        returncode, stdout, stderr = run_mimic(no_quasar_param_file)
        assert returncode == 0, (
            "Comparison pipeline without satellite_mergers quasar consumer should succeed\n"
            f"STDERR: {stderr}"
        )

        base_halos = load_output_halos(base_output_dir)
        no_quasar_halos = load_output_halos(no_quasar_output_dir)

        common_ids, base_bh, no_quasar_bh = common_merger_field_values(
            base_halos, no_quasar_halos, "BlackHoleMass"
        )
        assert len(common_ids) >= 100, "Test dataset should produce merger remnants"

        _, base_accretion, no_quasar_accretion = common_merger_field_values(
            base_halos, no_quasar_halos, "QuasarModeBHaccretionMass"
        )

        bh_delta = float(np.sum(base_bh - no_quasar_bh))
        accretion_delta = float(np.sum(base_accretion - no_quasar_accretion))

        # ~0.05 M_sun/h total BH mass added across merger remnants in the test snapshot
        assert (
            bh_delta > 0.05
        ), "satellite_mergers quasar consumer should increase remnant BH mass via merger events"
        # ~0.002 M_sun/h total accretion recorded across merger remnants
        assert (
            accretion_delta > 0.002
        ), "satellite_mergers quasar consumer should record merger-driven BH accretion"
    finally:
        shutil.rmtree(base_temp_dir)
        shutil.rmtree(no_quasar_temp_dir)


def test_starburst_consumer_receives_merger_events():
    """Disabling the satellite_mergers starburst consumer must remove merger-driven bulge growth."""
    base_param_file, base_output_dir, base_temp_dir = run_merger_pipeline(
        output_name="merger_event_starburst_base"
    )
    no_starburst_param_file, no_starburst_output_dir, no_starburst_temp_dir = run_merger_pipeline(
        output_name="merger_event_starburst_disabled",
        enable_phase2_starburst=False,
    )

    try:
        returncode, stdout, stderr = run_mimic(base_param_file)
        assert returncode == 0, f"Baseline merger pipeline should succeed\nSTDERR: {stderr}"

        returncode, stdout, stderr = run_mimic(no_starburst_param_file)
        assert returncode == 0, (
            "Comparison pipeline without satellite_mergers starburst consumer should succeed\n"
            f"STDERR: {stderr}"
        )

        base_halos = load_output_halos(base_output_dir)
        no_starburst_halos = load_output_halos(no_starburst_output_dir)

        common_ids, base_bulge, no_starburst_bulge = common_merger_field_values(
            base_halos, no_starburst_halos, "BulgeMass"
        )
        assert len(common_ids) >= 100, "Test dataset should produce merger remnants"

        _, base_stellar, no_starburst_stellar = common_merger_field_values(
            base_halos, no_starburst_halos, "StellarMass"
        )
        _, base_cold_gas, no_starburst_cold_gas = common_merger_field_values(
            base_halos, no_starburst_halos, "ColdGas"
        )

        bulge_delta = float(np.sum(base_bulge - no_starburst_bulge))
        stellar_delta = float(np.sum(base_stellar - no_starburst_stellar))
        cold_gas_delta = float(np.sum(base_cold_gas - no_starburst_cold_gas))

        # ~9 M_sun/h total bulge mass added across merger remnants in the test snapshot
        assert (
            bulge_delta > 5.0
        ), "satellite_mergers starburst consumer should increase merger-remnant bulge mass"
        # ~6.5 M_sun/h net stellar mass increase from burst star formation
        assert (
            stellar_delta > 5.0
        ), "satellite_mergers starburst consumer should form additional stars in merger remnants"
        # The burst net-consumes a few M_sun/h of cold gas. SAGE parity note: the
        # merger quasar wind (grow_black_hole -> quasar_mode_wind, which fires
        # BEFORE the starburst in SAGE deal_with_galaxy_merger,
        # src/model_mergers.c:262-287) ejects some cold
        # gas first, so the net cold-gas reduction attributable to the burst is a
        # few M_sun/h (~-4.7 here), not the larger value seen before the quasar
        # wind energy was unit-corrected.
        assert (
            cold_gas_delta < -3.0
        ), "satellite_mergers starburst consumer should net-consume cold gas in merger remnants"
    finally:
        shutil.rmtree(base_temp_dir)
        shutil.rmtree(no_starburst_temp_dir)


def test_merger_bulge_bound():
    """The merger transaction's bulge writes keep BulgeMass <= StellarMass exactly

    Runs the merger-bound pipeline (see merger_bulge_bound_holds), which reaches the
    writes of src/model_mergers.c:461-462, :500-501 (every merger) and :556, :559 (major
    mergers), and requires both bounds on every row, with major and minor mergers present
    and the bound attained.
    """
    param_file, output_dir, temp_dir = run_bound_pipeline("merger_bulge_bound")
    try:
        returncode, stdout, stderr = run_mimic(param_file)
        assert returncode == 0, f"Merger-bound pipeline should succeed\nSTDERR: {stderr}"
        halos = load_output_halos(output_dir)

        never = np.float32(np.float32(NEVER_MERGED) * unit_time_in_megayears())
        major = halos["TimeOfLastMajorMerger"] != never
        minor = halos["TimeOfLastMinorMerger"] != never
        assert np.count_nonzero(major) >= 10, "the pipeline should reach major mergers"
        assert np.count_nonzero(minor) >= 10, "the pipeline should reach minor mergers"

        stellar_ok = merger_bulge_bound_holds(halos["StellarMass"], halos["BulgeMass"])
        metals_ok = merger_bulge_bound_holds(halos["MetalsStellarMass"], halos["MetalsBulgeMass"])
        attained = (halos["BulgeMass"] == halos["StellarMass"]) & (halos["StellarMass"] > 0)
        print(
            f"  rows {len(halos)}, with a major merger {np.count_nonzero(major)}, with a minor "
            f"merger {np.count_nonzero(minor)}; BulgeMass <= StellarMass violated "
            f"{np.count_nonzero(~stellar_ok)}, MetalsBulgeMass <= MetalsStellarMass violated "
            f"{np.count_nonzero(~metals_ok)}; bound attained (BulgeMass == StellarMass > 0) "
            f"{np.count_nonzero(attained)}"
        )
        assert stellar_ok.all(), "BulgeMass must not exceed StellarMass"
        assert metals_ok.all(), "MetalsBulgeMass must not exceed MetalsStellarMass"
        assert np.count_nonzero(attained) >= 1, "major mergers should attain the bound"
    finally:
        shutil.rmtree(temp_dir)


def test_written_merger_timestamps():
    """The timestamps are written as SAGE26 writes them: value * UnitTime_in_Megayears

    A galaxy that never had a merger of a kind is written as float(-1 * factor); a merged
    one as float(t * factor) for its float lookback time t, a substep midpoint event time
    (src/model_mergers.c:310, :316, src/io/save_gals_hdf5.c:992-993).
    """
    param_file, output_dir, temp_dir = run_bound_pipeline("merger_timestamps")
    try:
        returncode, stdout, stderr = run_mimic(param_file)
        assert returncode == 0, f"Merger pipeline should succeed\nSTDERR: {stderr}"
        halos = load_output_halos(output_dir)
        factor = unit_time_in_megayears()
        never = np.float32(np.float64(np.float32(NEVER_MERGED)) * factor)
        event_times, substeps = substep_event_times(param_file)
        print(
            f"  UnitTime_in_Megayears = {factor!r} ({float(factor).hex()}); never merged "
            f"written as {never!r} ({hex(never.view(np.uint32))})"
        )

        for field in ("TimeOfLastMajorMerger", "TimeOfLastMinorMerger"):
            written = np.asarray(halos[field], dtype=np.float32)
            never_rows = written == never
            merged = written[~never_rows]
            assert np.count_nonzero(never_rows) >= 1, f"{field}: some galaxy never merged"
            assert len(merged) >= 10, f"{field}: the pipeline should reach mergers"
            assert np.all(written[never_rows].view(np.uint32) == never.view(np.uint32))

            # Each merged value is float(t * factor) for a float lookback time t: the
            # float nearest written / factor, or a neighbour, reproduces it exactly
            lookback = np.float32(merged.astype(np.float64) / factor)
            candidates = [
                lookback,
                np.nextafter(lookback, np.float32(np.inf)),
                np.nextafter(lookback, np.float32(-np.inf)),
            ]
            reproduced = np.zeros(len(merged), dtype=bool)
            t = np.full(len(merged), np.nan)
            for c in candidates:
                hit = np.float32(c.astype(np.float64) * factor) == merged
                t = np.where(hit & ~reproduced, c.astype(np.float64), t)
                reproduced |= hit
            assert reproduced.all(), f"{field}: a written value is not float(t * factor)"

            # And t is a substep midpoint lookback time of the run
            nearest = np.searchsorted(event_times, t)
            lo = event_times[np.clip(nearest - 1, 0, len(event_times) - 1)]
            hi = event_times[np.clip(nearest, 0, len(event_times) - 1)]
            distance = np.minimum(np.abs(t - lo), np.abs(t - hi)) / t
            print(
                f"  {field}: never merged {np.count_nonzero(never_rows)}, merged "
                f"{len(merged)} with lookback t in [{t.min():.6g}, {t.max():.6g}] code units, "
                f"written [{merged.min():.6g}, {merged.max():.6g}] Myr, largest relative "
                f"distance to a substep midpoint ({substeps} substeps) {distance.max():.2e}"
            )
            assert np.all(t > 0), f"{field}: a merged lookback time must be positive"
            assert distance.max() < 1e-5, f"{field}: t is not a substep midpoint event time"
    finally:
        shutil.rmtree(temp_dir)


def main():
    """Run the immediate merger event integration suite."""
    print(f"{BLUE}{'=' * 60}{NC}")
    print(f"{BLUE}Test Suite: {Path(__file__).name}{NC}")
    print(f"{BLUE}{'=' * 60}{NC}")
    print()

    tests = [
        test_invalid_processing_mode_rejected,
        test_quasar_consumer_receives_merger_events,
        test_starburst_consumer_receives_merger_events,
        test_merger_bulge_bound,
        test_written_merger_timestamps,
    ]

    if not MIMIC_EXE.exists():
        for test in tests:
            result_skip(test.__name__, "Mimic not built")
        return 0

    passed = 0
    failed = 0
    skipped = 0

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
