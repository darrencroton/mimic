#!/usr/bin/env python3
"""
Rate provenance: RunProperties/RateTransfers, RunProperties/RateIntegration and the step statistics.

Validates, on the selected package's committed fixture in a test build, with the four rate
fixtures in one rate phase (their modules and parameters read from the rate battery's run file,
tests/framework/rate_fixture.py):
  - test_records_exist_only_with_a_rate_phase: with a rate phase, RunProperties/RateTransfers is
    in the master file and in every partition file and RunProperties/RateIntegration in the
    master file only; with no rate phase neither is written anywhere
  - test_rate_transfers_rows: RateTransfers holds one row per configured transfer in global
    transfer order (module name, then declaration order), each row's module, transfer, ends,
    recipient record, kind and slaved master as the fixtures' module_info.yaml declare them, and
    every partition file holds the master's rows
  - test_rate_integration_settings: RateIntegration's attributes record the scheme, the four
    numeric keys, check_purity and the method string as the run file set them, under both
    schemes
  - test_step_statistics_arithmetic: each histogram's counts sum to StepTotals.segments, the
    per-segment maxima lie in the occupied top bins and within the totals, and with no discrete
    phase every total's declared-change count is zero
  - test_summary_line: a rate-phase run logs one "Rate integration:" summary line; a run with no
    rate phase logs none
  - test_binary_run_logs_the_summary_only: a binary-output rate-phase run logs the summary line
    and writes no HDF5 (not applicable on a horizontal package, which is HDF5-only)
"""

import shutil
import sys
from pathlib import Path

import h5py
import numpy as np
import yaml

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "tests"))

from framework import (  # noqa: E402
    MIMIC_EXE,
    TestSkipped,
    check_no_memory_leaks,
    core_input_file,
    create_test_param_file,
    not_applicable_if_selected_package_is_horizontal,
    run_mimic,
    run_test_suite,
)
from framework.rate_fixture import load_rate_fixture  # noqa: E402

MODULE_SYSTEM = REPO_ROOT / "src" / "module_system"
# The four fixtures and their law parameters (each fixture's README) come from the run file the
# rate battery uses, simulations/mini-millennium-horizontal/_tests/input/forest_blocks_rate.yaml.
RATE_FIXTURES, RATE_PARAMS = load_rate_fixture()

# A section with every numeric key away from its default, so a recorded default cannot pass.
EXPLICIT_SECTION = {
    "scheme": "explicit",
    "relative_tolerance": 2.0e-4,
    "absolute_tolerance": 3.0e-7,
    "max_steps_per_segment": 50000,
    "max_rejections_per_step": 20,
    "check_purity": False,
}
IMPLICIT_SECTION = {
    "scheme": "implicit",
    "relative_tolerance": 1.0e-3,
    "absolute_tolerance": 1.0e-6,
    "max_steps_per_segment": 40000,
    "max_rejections_per_step": 25,
    "check_purity": True,
}

FIXTURE_TOTALS = ("rate_fixture_mass", "rate_fixture_tracer")
SUMMARY_PREFIX = "Rate integration: "
RATE_TRANSFERS = "RunProperties/RateTransfers"
RATE_INTEGRATION = "RunProperties/RateIntegration"

_temp_dirs = []
_runs = {}


def _rate_phase():
    return {"rates": [(name, "process_rates") for name in RATE_FIXTURES]}


def _run(name, phase_config, output_format="hdf5", **overrides):
    """Write a run file on the committed fixture, run it once, return (rc, log, output_dir)."""
    if name in _runs:
        return _runs[name]
    if not MIMIC_EXE.exists():
        raise TestSkipped("Mimic not built")
    ref = core_input_file("test_hdf5.yaml")  # skips for a package with no committed fixture
    param_file, output_dir, temp_dir = create_test_param_file(
        f"rate_provenance_{name}",
        phase_config=phase_config,
        model_params=RATE_PARAMS if phase_config else None,
        ref_param_file=ref,
        output_format=output_format,
        **overrides,
    )
    _temp_dirs.append(temp_dir)
    returncode, stdout, stderr = run_mimic(param_file)
    _runs[name] = (returncode, stdout + "\n" + stderr, Path(output_dir))
    return _runs[name]


def _completed(name, phase_config, **overrides):
    returncode, log, output_dir = _run(name, phase_config, **overrides)
    assert returncode == 0, f"{name}: the run failed (exit {returncode})\n{log[-4000:]}"
    assert check_no_memory_leaks(log), f"{name}: the run reported a memory leak"
    return log, output_dir


def _explicit_run():
    return _completed("explicit", _rate_phase(), rate_integration=EXPLICIT_SECTION)


def _implicit_run():
    return _completed("implicit", _rate_phase(), rate_integration=IMPLICIT_SECTION)


def _master(output_dir):
    path = output_dir / "model.hdf5"
    assert path.is_file(), f"no master file at {path}"
    return path


def _partitions(output_dir):
    files = sorted(p for p in output_dir.glob("model_*.hdf5"))
    assert files, f"no partition file in {output_dir}"
    return files


def _expected_transfer_rows():
    """The fixtures' transfers in global order, as module_info.yaml declares them."""
    rows = []
    for module in sorted(RATE_FIXTURES):
        with open(MODULE_SYSTEM / module / "module_info.yaml", "r", encoding="utf-8") as handle:
            metadata = yaml.safe_load(handle)["module"]
        for transfer in metadata["rates"]["transfers"]:
            if transfer["from"] == "external":
                kind = "slaved_source" if "slaved_to" in transfer else "source"
            elif transfer["to"] == "external":
                kind = "sink"
            else:
                kind = "internal"
            rows.append(
                (
                    module,
                    transfer["name"],
                    transfer["from"],
                    transfer["to"],
                    transfer.get("to_record", "self"),
                    kind,
                    transfer.get("slaved_to", ""),
                )
            )
    return rows


def _transfer_rows(path):
    with h5py.File(path, "r") as handle:
        data = handle[RATE_TRANSFERS][:]
    fields = ("module", "transfer", "from", "to", "to_record", "kind", "master")
    return [tuple(row[field].decode() for field in fields) for row in data]


def _attribute_text(value):
    """A string attribute's text; RunProperties strings are one-element fixed-width arrays."""
    if isinstance(value, np.ndarray):
        assert value.shape == (1,), f"a string attribute of shape {value.shape}"
        value = value[0]
    return value.decode() if isinstance(value, bytes) else str(value)


def test_records_exist_only_with_a_rate_phase():
    """The rate records exist exactly when a rate phase is configured, where the design says."""
    _, output_dir = _explicit_run()
    with h5py.File(_master(output_dir), "r") as handle:
        assert RATE_TRANSFERS in handle, "the master file has no RateTransfers"
        assert RATE_INTEGRATION in handle, "the master file has no RateIntegration"
    for path in _partitions(output_dir):
        with h5py.File(path, "r") as handle:
            assert RATE_TRANSFERS in handle, f"{path.name} has no RateTransfers"
            assert RATE_INTEGRATION not in handle, f"{path.name} has a RateIntegration group"

    _, plain_dir = _completed("no_rate_phase", {})
    for path in [_master(plain_dir)] + _partitions(plain_dir):
        with h5py.File(path, "r") as handle:
            for name in (RATE_TRANSFERS, RATE_INTEGRATION):
                assert name not in handle, f"{path.name}: a run with no rate phase wrote {name}"
    print("  ✓ RateTransfers in every file and RateIntegration in the master, only with a phase")


def test_rate_transfers_rows():
    """One RateTransfers row per configured transfer, in global order, in every file."""
    _, output_dir = _explicit_run()
    expected = _expected_transfer_rows()
    master_rows = _transfer_rows(_master(output_dir))
    assert master_rows == expected, f"master RateTransfers rows {master_rows} != {expected}"
    for path in _partitions(output_dir):
        assert _transfer_rows(path) == expected, f"{path.name}: RateTransfers rows differ"
    print(f"  ✓ {len(expected)} RateTransfers rows in global transfer order in every file")


def test_rate_integration_settings():
    """RateIntegration records the scheme, the four numeric keys, check_purity and the method."""
    methods = {}
    for label, run, section in (
        ("explicit", _explicit_run, EXPLICIT_SECTION),
        ("implicit", _implicit_run, IMPLICIT_SECTION),
    ):
        _, output_dir = run()
        with h5py.File(_master(output_dir), "r") as handle:
            attrs = dict(handle[RATE_INTEGRATION].attrs)
        assert _attribute_text(attrs["scheme"]) == section["scheme"], f"{label}: scheme {attrs}"
        assert float(attrs["relative_tolerance"]) == section["relative_tolerance"], label
        assert float(attrs["absolute_tolerance"]) == section["absolute_tolerance"], label
        assert int(attrs["max_steps_per_segment"]) == section["max_steps_per_segment"], label
        assert int(attrs["max_rejections_per_step"]) == section["max_rejections_per_step"], label
        purity = "true" if section["check_purity"] else "false"
        assert _attribute_text(attrs["check_purity"]) == purity, f"{label}: check_purity {attrs}"
        methods[label] = _attribute_text(attrs["method"])
    assert methods["explicit"].startswith("SSPRK(4,3)"), methods["explicit"]
    assert methods["implicit"].startswith("Linearly implicit Patankar-Euler"), methods["implicit"]
    for label, method in methods.items():
        for constant in ("[0.2, 5]", "scale floor 1e-12", "minimum step 2^-46"):
            assert constant in method, f"{label}: the method string lacks {constant!r}: {method}"
    print("  ✓ RateIntegration attributes record both schemes' settings and methods")


def _top_bin(counts):
    occupied = np.flatnonzero(counts)
    return int(occupied[-1]) if occupied.size else -1


def _k_max_bin(k_max, min_exponent, max_exponent):
    """The KMaxHistogram bin of a value, by the layout its description states."""
    if k_max == 0.0:
        return 0
    if not np.isfinite(k_max):
        return 2 + (max_exponent - min_exponent + 1)
    octave = int(np.frexp(k_max)[1]) - 1
    if octave < min_exponent:
        return 1
    if octave > max_exponent:
        return 2 + (max_exponent - min_exponent + 1)
    return 2 + octave - min_exponent


def test_step_statistics_arithmetic():
    """The step statistics satisfy their own arithmetic."""
    for run in (_explicit_run, _implicit_run):
        _, output_dir = run()
        with h5py.File(_master(output_dir), "r") as handle:
            group = handle[RATE_INTEGRATION]
            k_max_counts = group["KMaxHistogram"][:]
            min_exponent = int(group["KMaxHistogram"].attrs["octave_min_exponent"])
            max_exponent = int(group["KMaxHistogram"].attrs["octave_max_exponent"])
            step_counts = group["AcceptedStepsHistogram"][:]
            totals = group["StepTotals"][0]
            maxima = group["StepMaxima"][0]
            changes = group["DeclaredDiscreteChanges"][:]
            scheme = _attribute_text(group.attrs["scheme"])

        segments = int(totals["segments"])
        assert segments > 0, f"{scheme}: no segment was recorded"
        assert k_max_counts.dtype == np.uint64 and step_counts.dtype == np.uint64
        assert len(k_max_counts) == max_exponent - min_exponent + 4, "K_max bin count"
        assert len(step_counts) == 65, "accepted-steps bin count"
        assert int(k_max_counts.sum()) == segments, f"{scheme}: K_max histogram != segments"
        assert int(step_counts.sum()) == segments, f"{scheme}: steps histogram != segments"

        most_steps = int(maxima["accepted_steps_per_segment"])
        assert 0 < most_steps <= int(totals["accepted_steps"]), f"{scheme}: step maximum"
        assert _top_bin(step_counts) == most_steps.bit_length(), f"{scheme}: top step bin"
        assert int(maxima["rejected_steps_per_segment"]) <= int(totals["rejected_steps"])
        most_evaluations = int(maxima["law_evaluations_per_segment"])
        assert 0 < most_evaluations <= int(totals["law_evaluations"]), f"{scheme}: evaluations"
        assert int(totals["law_evaluations"]) >= int(totals["accepted_steps"]), scheme
        k_max = float(maxima["k_max"])
        assert k_max >= 0.0, f"{scheme}: negative K_max maximum"
        assert _top_bin(k_max_counts) == _k_max_bin(k_max, min_exponent, max_exponent), scheme

        recorded = {row["total"].decode(): int(row["count"]) for row in changes}
        assert recorded == {total: 0 for total in FIXTURE_TOTALS}, f"{scheme}: {recorded}"
    print("  ✓ Histograms sum to the segments; maxima sit in the top bins; no declared change")


def test_summary_line():
    """One summary line with a rate phase; none without one."""
    log, _ = _explicit_run()
    lines = [line for line in log.splitlines() if SUMMARY_PREFIX in line]
    assert len(lines) == 1, f"expected one summary line, found {lines}"
    for total in FIXTURE_TOTALS:
        assert f"{total} 0" in lines[0], f"the summary line lacks {total}: {lines[0]}"

    log, _ = _completed("no_rate_phase", {})
    assert SUMMARY_PREFIX not in log, "a run with no rate phase logged a rate summary"
    print("  ✓ One summary line per rate-phase run, none without a rate phase")


def test_binary_run_logs_the_summary_only():
    """A binary-output rate-phase run logs the summary line and writes no HDF5 record."""
    not_applicable_if_selected_package_is_horizontal("binary galaxy output")
    log, output_dir = _completed("binary", _rate_phase(), output_format="binary")
    lines = [line for line in log.splitlines() if SUMMARY_PREFIX in line]
    assert len(lines) == 1, f"binary run: expected one summary line, found {lines}"
    assert not list(output_dir.glob("*.hdf5")), "a binary-output run wrote HDF5"
    print("  ✓ A binary-output run records its statistics in the summary line only")


def _cleanup():
    for temp_dir in _temp_dirs:
        shutil.rmtree(temp_dir, ignore_errors=True)


def main():
    try:
        return run_test_suite(
            [
                test_records_exist_only_with_a_rate_phase,
                test_rate_transfers_rows,
                test_rate_integration_settings,
                test_step_statistics_arithmetic,
                test_summary_line,
                test_binary_run_logs_the_summary_only,
            ],
            "Rate Provenance (test_rate_provenance.py)",
        )
    finally:
        _cleanup()


if __name__ == "__main__":
    sys.exit(main())
