#!/usr/bin/env python3
"""
Rate phase: the modules.rate_integration run-file section and the rate fixtures through real runs.

Validates, on the selected package's committed fixture in a test build (the default pair runs the
vertical driver; MODEL=halos-only SIMULATION=mini-millennium-horizontal the horizontal driver):
  - test_run_file_section_refusals: a modules.rate_integration section with no rate phase, with
    an unknown key, or with a value out of range (each of the five keys), a section that is not
    a mapping, and a substep phase named rate_integration, each end the run before any module's
    init() with a message naming the key
  - test_explicit_scheme: the four rate fixtures in one rate phase complete under the default
    (explicit) scheme; every one of the seven fixture properties is non-zero on some row, and no
    reservoir, tracer or observer value is negative or non-finite; no leak is reported
  - test_implicit_scheme: the same under scheme: implicit, with all five keys set
  - test_dynamic_timestepping: the same under TimestepScheme: dynamic
  - test_created_records_join: with test_fixture creating records in pre_timestep, the run
    completes and every created row's fixture values are finite and non-negative (that a created
    record joins its component at zero content is shown by tests/unit/test_rate_dispatch.c)

The seven fixture properties (src/module_system/test_fixture/test_properties.yaml) are written to
output in every test build, so the checks read them from the run's HDF5 galaxies.
"""

import shutil
import sys
from pathlib import Path

import h5py
import numpy as np

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "tests"))

from framework import (  # noqa: E402
    MIMIC_EXE,
    TestSkipped,
    check_no_memory_leaks,
    core_input_file,
    create_test_param_file,
    run_mimic,
    run_test_suite,
)

RATE_FIXTURES = (
    "test_rate_supply",
    "test_rate_formation",
    "test_rate_feedback",
    "test_rate_stripping",
)

# Fixture laws (each fixture's README): rates in inverse internal time units, so a snapshot
# interval of order 0.1 moves a visible fraction; a threshold this low lets stars form even on
# the two-galaxy horizontal fixture, whose cold content stays near 1e-5.
RATE_PARAMS = {
    "TestRateSupplyInfallRate": 0.5,
    "TestRateSupplyTracerFraction": 0.1,
    "TestRateSupplyCoolingRate": 5.0,
    "TestRateFormationEfficiency": 5.0,
    "TestRateFormationThreshold": 1.0e-7,
    "TestRateFeedbackReheatRate": 1.0,
    "TestRateFeedbackYield": 0.02,
    "TestRateStrippingRate": 3.0,
    "TestRateStrippingEjectionRate": 0.5,
}

RESERVOIR_FIELDS = (
    "RateFixtureHot",
    "RateFixtureCold",
    "RateFixtureStars",
    "RateFixtureHotTracer",
    "RateFixtureColdTracer",
    "RateFixtureStarsTracer",
)
FIXTURE_FIELDS = RESERVOIR_FIELDS + ("RateFixtureFormed",)

# Logged by test_rate_supply's init(); absent when startup refused the run before any init().
SUPPLY_INIT_LINE = "Test rate supply fixture initialized"

_temp_dirs = []


def _rate_phase():
    return {"rates": [(name, "process_rates") for name in RATE_FIXTURES]}


def _run(name, phase_config, model_params=None, **overrides):
    """Write a run file on the committed HDF5 fixture, run it, return (rc, log, output_dir)."""
    if not MIMIC_EXE.exists():
        raise TestSkipped("Mimic not built")
    ref = core_input_file("test_hdf5.yaml")  # skips for a package with no committed fixture
    param_file, output_dir, temp_dir = create_test_param_file(
        f"rate_phase_{name}",
        phase_config=phase_config,
        model_params=model_params,
        ref_param_file=ref,
        output_format="hdf5",
        **overrides,
    )
    _temp_dirs.append(temp_dir)
    returncode, stdout, stderr = run_mimic(param_file)
    return returncode, stdout + "\n" + stderr, output_dir


def _galaxies(output_dir):
    """Every Galaxies row of every per-file HDF5 output, concatenated."""
    files = sorted(Path(output_dir).glob("model_*.hdf5"))
    assert files, f"no per-file HDF5 output in {output_dir}"
    parts = []
    for path in files:
        with h5py.File(path, "r") as handle:
            for key in sorted(handle.keys()):
                if key.startswith("Snap") and "Galaxies" in handle[key]:
                    parts.append(handle[key]["Galaxies"][:])
    assert parts, f"no Galaxies dataset in {output_dir}"
    return np.concatenate(parts)


def _assert_fixture_output(galaxies, label):
    """Every fixture property non-zero somewhere; none negative or non-finite anywhere."""
    for field in FIXTURE_FIELDS:
        values = np.asarray(galaxies[field], dtype=np.float64)
        bad = ~np.isfinite(values)
        assert not bad.any(), f"{label}: {field} has {int(bad.sum())} non-finite value(s)"
        assert (values >= 0.0).all(), f"{label}: {field} has a negative value ({values.min()!r})"
        assert np.count_nonzero(values) > 0, f"{label}: {field} is zero on every row"


def _assert_completed(returncode, log, label):
    assert returncode == 0, f"{label}: the run failed (exit {returncode})\n{log[-4000:]}"
    assert check_no_memory_leaks(log), f"{label}: the run reported a memory leak"


def test_run_file_section_refusals():
    """Each refusal of modules.rate_integration ends the run before any init(), naming the key."""
    fixtures = _rate_phase()
    cases = [
        (
            "no_rate_phase",
            {},
            {"scheme": "explicit"},
            "modules.rate_integration is set, but no phase under modules.phases runs "
            "process_rates",
        ),
        (
            "unknown_key",
            fixtures,
            {"method": "explicit"},
            "Unknown key 'modules.rate_integration.method'",
        ),
        (
            "bad_scheme",
            fixtures,
            {"scheme": "rk4"},
            "Unknown modules.rate_integration.scheme 'rk4'",
        ),
        (
            "relative_zero",
            fixtures,
            {"relative_tolerance": 0},
            "modules.rate_integration.relative_tolerance must lie in (0, 1), got 0",
        ),
        (
            "relative_one",
            fixtures,
            {"relative_tolerance": 1},
            "modules.rate_integration.relative_tolerance must lie in (0, 1), got 1",
        ),
        (
            "absolute_negative",
            fixtures,
            {"absolute_tolerance": -0.001},
            "modules.rate_integration.absolute_tolerance must lie in (0, 1), got -0.001",
        ),
        (
            "absolute_not_a_number",
            fixtures,
            {"absolute_tolerance": "tiny"},
            "modules.rate_integration.absolute_tolerance must be a valid finite number",
        ),
        (
            "steps_zero",
            fixtures,
            {"max_steps_per_segment": 0},
            "modules.rate_integration.max_steps_per_segment must be >= 1, got 0",
        ),
        (
            "rejections_zero",
            fixtures,
            {"max_rejections_per_step": 0},
            "modules.rate_integration.max_rejections_per_step must be >= 1, got 0",
        ),
        (
            "rejections_fraction",
            fixtures,
            {"max_rejections_per_step": 2.5},
            "modules.rate_integration.max_rejections_per_step must be a valid integer",
        ),
        ("not_a_mapping", fixtures, ["explicit"], "modules.rate_integration must be a mapping"),
        (
            "reserved_phase_name",
            {"rate_integration": [(name, "process_rates") for name in RATE_FIXTURES]},
            None,
            "Substep phase name 'rate_integration' is reserved",
        ),
    ]
    for name, phase_config, section, fragment in cases:
        returncode, log, _ = _run(
            f"refuse_{name}", phase_config, model_params=RATE_PARAMS, rate_integration=section
        )
        assert returncode != 0, f"{name}: the run was accepted"
        assert fragment in log, f"{name}: the refusal does not say {fragment!r}\n{log[-3000:]}"
        assert SUPPLY_INIT_LINE not in log, f"{name}: a module init() ran before the refusal"


def test_explicit_scheme():
    """The four rate fixtures run under the default (explicit) scheme with sound output."""
    returncode, log, output_dir = _run("explicit", _rate_phase(), model_params=RATE_PARAMS)
    _assert_completed(returncode, log, "explicit")
    _assert_fixture_output(_galaxies(output_dir), "explicit")


def test_implicit_scheme():
    """The four rate fixtures run under scheme: implicit, every key set, with sound output."""
    section = {
        "scheme": "implicit",
        "relative_tolerance": 1.0e-3,
        "absolute_tolerance": 1.0e-6,
        "max_steps_per_segment": 50000,
        "max_rejections_per_step": 20,
    }
    returncode, log, output_dir = _run(
        "implicit", _rate_phase(), model_params=RATE_PARAMS, rate_integration=section
    )
    _assert_completed(returncode, log, "implicit")
    _assert_fixture_output(_galaxies(output_dir), "implicit")


def test_dynamic_timestepping():
    """The four rate fixtures run under TimestepScheme: dynamic with sound output."""
    returncode, log, output_dir = _run(
        "dynamic",
        _rate_phase(),
        model_params=RATE_PARAMS,
        timestep_scheme="dynamic",
        substeps=3,
    )
    _assert_completed(returncode, log, "dynamic")
    _assert_fixture_output(_galaxies(output_dir), "dynamic")


def test_created_records_join():
    """Records test_fixture creates in pre_timestep run through the rate phase with sound values."""
    phase_config = {"pre_timestep": [("test_fixture", "process_full_halo")], **_rate_phase()}
    params = dict(RATE_PARAMS)
    params.update(
        {
            "TestFixtureDummyParameter": 0.25,
            "TestFixtureEnableLogging": 0,
            "TestFixtureCreateRecords": 1,
        }
    )
    returncode, log, output_dir = _run("created", phase_config, model_params=params)
    _assert_completed(returncode, log, "created")
    galaxies = _galaxies(output_dir)
    _assert_fixture_output(galaxies, "created")
    created = galaxies[galaxies["UniqueGalaxyID"] < 0]
    assert len(created) > 0, "test_fixture created no record"
    for field in FIXTURE_FIELDS:
        values = np.asarray(created[field], dtype=np.float64)
        assert (
            np.isfinite(values).all() and (values >= 0.0).all()
        ), f"a created record has a negative or non-finite {field}"


def _cleanup():
    for temp_dir in _temp_dirs:
        shutil.rmtree(temp_dir, ignore_errors=True)


def main():
    try:
        return run_test_suite(
            [
                test_run_file_section_refusals,
                test_explicit_scheme,
                test_implicit_scheme,
                test_dynamic_timestepping,
                test_created_records_join,
            ],
            "Rate Phase (test_rate_phase.py)",
        )
    finally:
        _cleanup()


if __name__ == "__main__":
    sys.exit(main())
