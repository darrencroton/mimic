#!/usr/bin/env python3
"""
Self-test for the SAGE-versus-sage16 comparator (models/sage16/tools/compare_sage_mimic.py).

Validates: identical data reports a full bit-identical match; a one-ULP change is within
           tolerance but not bit-identical; a large change is outside tolerance; a SAGE-only
           galaxy is unmatched and reported; NaN against a number is a mismatch
Tolerance: none; every expected count is exact (comparator defaults rtol 1e-6, atol 1e-10)

A parity figure is only trustworthy if the comparator can report a difference, so this
synthesises tiny catalogues in both native layouts and perturbs one side in a known way.
It needs no SAGE or Mimic run.

Run: python3 models/sage16/modules/_tests/test_scientific_compare_sage_mimic.py
"""

import contextlib
import importlib.util
import io
import json
import sys
import tempfile
from pathlib import Path

import h5py
import numpy as np

REPO_ROOT = Path(__file__).resolve().parents[4]
sys.path.insert(0, str(REPO_ROOT / "tests"))

from framework import run_test_suite  # noqa: E402  (path set up above)

TOOL = REPO_ROOT / "models" / "sage16" / "tools" / "compare_sage_mimic.py"
_spec = importlib.util.spec_from_file_location("compare_sage_mimic", TOOL)
tool = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(tool)

SNAP = 63
N_GAL = 4


def catalogue():
    """Four galaxies with distinct float32 positions, keyed by SAGE field name."""
    return {
        "Posx": np.arange(N_GAL, dtype=np.float32) + 0.5,
        "Posy": np.arange(N_GAL, dtype=np.float32) + 10.5,
        "Posz": np.arange(N_GAL, dtype=np.float32) + 20.5,
        "Len": np.arange(N_GAL, dtype=np.int32) + 100,
        "Type": np.zeros(N_GAL, dtype=np.int32),
        "SimulationHaloIndex": np.arange(N_GAL, dtype=np.int64) + 1000,
        "Mvir": np.full(N_GAL, 5.0, dtype=np.float32),
        "ColdGas": np.arange(N_GAL, dtype=np.float32) + 1.0,
    }


def run_tool(directory, sage, mimic):
    """Write both layouts, run the tool and return the snapshot result."""
    sage_dir, mimic_dir, out = directory / "sage", directory / "mimic", directory / "r.json"
    sage_dir.mkdir()
    mimic_dir.mkdir()
    with h5py.File(sage_dir / "model_0.hdf5", "w") as handle:
        for name, values in sage.items():
            handle.create_dataset(f"Snap_{SNAP}/{name}", data=values)
    fields = [("Pos", "<f4", (3,)), ("Len", "<i4"), ("Type", "<i4"), ("MostBoundID", "<i8")]
    records = np.zeros(len(mimic["Len"]), dtype=fields + [("Mvir", "<f4"), ("ColdGas", "<f4")])
    records["Pos"] = np.column_stack([mimic["Posx"], mimic["Posy"], mimic["Posz"]])
    for name in ("Len", "Type", "Mvir", "ColdGas"):
        records[name] = mimic[name]
    records["MostBoundID"] = mimic["SimulationHaloIndex"]
    with h5py.File(mimic_dir / "model_000.hdf5", "w") as handle:
        handle.create_dataset(f"Snap{SNAP:03d}/Galaxies", data=records)
    argv = ["--sage-dir", str(sage_dir), "--mimic-dir", str(mimic_dir)]
    with contextlib.redirect_stdout(io.StringIO()):
        status = tool.main(argv + ["--snapshots", str(SNAP), "--out", str(out)])
    assert status == 0, f"comparator exited {status}"
    return json.loads(out.read_text())["snapshots"][0]


def compare(perturb=None, sage_extra=False):
    """Compare a catalogue with a copy; `perturb` edits the Mimic copy in place."""
    sage, mimic = catalogue(), catalogue()
    if perturb:
        perturb(mimic)
    if sage_extra:  # a fifth galaxy only on the SAGE side
        for name, values in sage.items():
            sage[name] = np.append(values, values[-1] + values.dtype.type(7))
    with tempfile.TemporaryDirectory() as tmp:
        result = run_tool(Path(tmp), sage, mimic)
    return result, {p["label"]: p for p in result["properties"]}


def test_identical_catalogues_are_fully_bit_identical():
    result, props = compare()
    assert result["matching"]["primary"] == "tuple"
    assert result["matching"]["tuple"]["n_matched"] == N_GAL
    assert result["unmatched"] == {"sage_only": [], "mimic_only": []}
    assert all(p["frac_bit_identical"] == 1.0 for p in props.values())
    assert all(p["frac_within_tol"] == 1.0 for p in props.values())


def test_one_ulp_is_within_tolerance_but_not_bit_identical():
    def nudge(m):
        m["ColdGas"][2] = np.nextafter(m["ColdGas"][2], np.float32(10.0))

    _, props = compare(nudge)
    assert props["ColdGas"]["frac_bit_identical"] == 0.75
    assert props["ColdGas"]["frac_within_tol"] == 1.0
    assert props["ColdGas"]["n_outside_tol"] == 0


def test_large_difference_is_outside_tolerance():
    def scale(m):
        m["ColdGas"][1] *= np.float32(1.5)

    result, props = compare(scale)
    assert props["ColdGas"]["frac_within_tol"] == 0.75
    assert props["ColdGas"]["n_outside_tol"] == 1
    assert result["galaxies"]["worst"][0]["properties"] == ["ColdGas"]


def test_sage_only_galaxy_is_unmatched_and_reported():
    result, _ = compare(sage_extra=True)
    stats = result["matching"]["tuple"]
    assert (stats["n_matched"], stats["n_sage_only"], stats["n_mimic_only"]) == (N_GAL, 1, 0)
    assert [row["halo_id"] for row in result["unmatched"]["sage_only"]] == [1000 + N_GAL - 1 + 7]
    assert result["unmatched"]["mimic_only"] == []


def test_nan_against_a_number_counts_as_a_mismatch():
    def blank(m):
        m["ColdGas"][0] = np.nan

    _, props = compare(blank)
    assert props["ColdGas"]["n_nonfinite_mismatch"] == 1
    assert props["ColdGas"]["frac_bit_identical"] == 0.75
    assert props["ColdGas"]["frac_within_tol"] == 0.75
    assert props["ColdGas"]["max_rdiff"] == "inf"


def main():
    return run_test_suite(
        [
            test_identical_catalogues_are_fully_bit_identical,
            test_one_ulp_is_within_tolerance_but_not_bit_identical,
            test_large_difference_is_outside_tolerance,
            test_sage_only_galaxy_is_unmatched_and_reported,
            test_nan_against_a_number_counts_as_a_mismatch,
        ],
        "SAGE parity comparator (test_scientific_compare_sage_mimic.py)",
    )


if __name__ == "__main__":
    sys.exit(main())
