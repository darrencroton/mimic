#!/usr/bin/env python3
"""
Self-test for the cross-format identity comparator

Validates: scripts/compare_cross_format_identity.py detects each way two runs
           can disagree, accepts genuinely identical runs, and separates
           unreadable input from a scientific difference
Tolerance: none -- the comparator is bitwise by contract, and so is this test

The comparator is the single implementation of the frozen comparison algorithm
behind the cross-format identity gate. The gate's verdict is only as trustworthy
as the comparator's ability to fail, and the gate itself cannot demonstrate that:
a passing gate proves the two runs agreed, not that a disagreement would have
been noticed. Most of the failure modes below were demonstrated once as run
evidence during the snapshot-driver work by hand-perturbing real output, then
discarded; this file commits that evidence. The rest were added afterwards, when
mutation testing showed the committed set could not see them -- see the coverage
note on
``test_perturbation_in_the_last_field_of_a_later_snapshot_is_detected``.

It lives in the core tier rather than beside the gate it defends, deliberately.
The gate is package-local, needs two multi-gigabyte machine-local datasets, and
takes hours, so neither it nor anything registered beside it runs in CI. This
file is package-neutral by construction -- it synthesises its own HDF5, imports
the comparator from ``scripts/``, and never runs Mimic or reads a simulation
package -- so registering it in the core tier means every change to the
comparator is checked in seconds by the default pair and by CI. Proximity to the
gate is worth less than actually running.

It deliberately synthesises its own HDF5 rather than running Mimic. The
comparator reads output files and knows nothing about how they were produced, so
a few hand-built records exercise every branch in seconds, and the negative cases
-- a duplicated id, a flipped sign bit, a truncated file -- can be constructed
exactly, which is the whole point and is not something a real run will do on
request.

Two cases carry more weight than the rest and are why the comparator compares
raw bytes rather than values:

  - **Signed zero.** ``-0.0 == 0.0`` is true, so a value comparison cannot see a
    sign flip. Bytes can.
  - **NaN payload.** ``nan != nan`` is true, so a value comparison reports a
    difference that is always there, hiding a genuinely changed payload behind
    permanent noise. Both directions are tested here: identical NaN payloads must
    compare equal, and differing NaN payloads must compare different. A
    comparator using ``==`` fails the first; one using ``numpy.allclose`` with
    ``equal_nan=True`` fails the second.

Run directly (seconds, no datasets and no Mimic build required):

    python3 tests/scientific/test_compare_cross_format_identity.py
"""

import contextlib
import io
import shutil
import sys
import tempfile
from pathlib import Path

import h5py
import numpy


def find_repo_root(start: Path) -> Path:
    """Find the Mimic repository root from this file's location."""
    for candidate in [start, *start.parents]:
        if (candidate / "Makefile").is_file() and (candidate / "tests" / "framework").is_dir():
            return candidate
    raise RuntimeError(f"Could not find Mimic repository root from {start}")


REPO_ROOT = find_repo_root(Path(__file__).resolve())
sys.path.insert(0, str(REPO_ROOT / "tests"))
sys.path.insert(0, str(REPO_ROOT / "scripts"))

import compare_cross_format_identity as comparator  # noqa: E402
from framework import run_test_suite  # noqa: E402  (path set up above)

#: Exit statuses the comparator's contract assigns. Named because the whole point
#: of several cases below is which of the three a situation maps to -- in
#: particular that unreadable input is an input error and never a difference.
IDENTICAL = 0
DIFFERENT = 1
INPUT_ERROR = 2

#: A minimal Galaxies record: the id the comparison is keyed on, one float field
#: to perturb, and one integer field. Padding-free by construction, so a
#: difference this test creates is always in a field and never in a gap.
RECORD_DTYPE = numpy.dtype(
    [("UniqueGalaxyID", "<i8"), ("StellarMass", "<f8"), ("Type", "<i4"), ("Len", "<i4")]
)


def make_records(ids, masses=None):
    """Build one snapshot's records from a list of ids."""
    records = numpy.zeros(len(ids), dtype=RECORD_DTYPE)
    records["UniqueGalaxyID"] = ids
    records["StellarMass"] = (
        numpy.arange(len(ids), dtype=numpy.float64) + 1.5 if masses is None else masses
    )
    records["Type"] = numpy.arange(len(ids)) % 3
    records["Len"] = 100 + numpy.arange(len(ids))
    return records


def write_run(directory, basename, partitions):
    """Write one run's partition files and return the comparator's spec string.

    `partitions` is a list of dicts, one per numbered partition file, mapping a
    snapshot number to that partition's records -- which is the layout the
    comparator indexes: a tree-ordered run spreads every snapshot across many
    partitions, a snapshot-ordered run writes one snapshot per partition.
    """
    directory.mkdir(parents=True, exist_ok=True)
    for index, snapshots in enumerate(partitions):
        with h5py.File(directory / f"{basename}_{index:03d}.hdf5", "w") as handle:
            for snap, records in sorted(snapshots.items()):
                handle.create_dataset(f"Snap{snap:03d}/Galaxies", data=records)
    return str(directory / basename)


def compare(left_spec, right_spec):
    """Run the comparator, returning its exit status and captured report."""
    captured = io.StringIO()
    with contextlib.redirect_stdout(captured), contextlib.redirect_stderr(captured):
        status = comparator.main([left_spec, right_spec])
    return status, captured.getvalue()


@contextlib.contextmanager
def scratch():
    """A temporary directory for one case's synthetic runs."""
    path = Path(tempfile.mkdtemp(prefix="mimic-comparator-selftest-"))
    try:
        yield path
    finally:
        shutil.rmtree(path, ignore_errors=True)


def build_pair(root, left_partitions, right_partitions):
    """Write a left and a right run under `root` and return both specs."""
    return (
        write_run(root / "left", "model", left_partitions),
        write_run(root / "right", "model", right_partitions),
    )


def assert_status(actual, expected, report, what):
    """Assert an exit status, quoting the comparator's own report when it differs."""
    assert actual == expected, f"{what}: expected exit {expected}, got {actual}\n{report}"


# --------------------------------------------------------------------------
# The passing cases: identity, and aggregation across the real partition layouts
# --------------------------------------------------------------------------


def test_identical_runs_compare_equal():
    """Two runs with the same ids and the same field bytes compare identical."""
    print("Testing identical runs...")
    with scratch() as root:
        records = {
            0: make_records([1_000_000_001, 1_000_000_002]),
            1: make_records([2_000_000_001]),
        }
        left, right = build_pair(root, [records], [records])
        status, report = compare(left, right)
        assert_status(status, IDENTICAL, report, "identical runs")
        assert "PASSED" in report, f"identical runs did not report PASSED\n{report}"
    print("  ✓ identical runs compare equal")


def test_records_are_aggregated_across_partitions():
    """The same galaxies compare equal however they are split across partitions.

    This is the arrangement the gate actually compares -- a tree-ordered run
    spreads each snapshot over several partition files while a snapshot-ordered
    run writes one file per snapshot -- so a comparator that compared partitions
    pairwise instead of aggregating by snapshot would report a false difference
    on every real gate run.
    """
    print("Testing aggregation across partitions...")
    with scratch() as root:
        snap0 = make_records([1_000_000_001, 1_000_000_002, 1_000_000_003])
        snap1 = make_records([2_000_000_001, 2_000_000_002])

        # Left models the tree-ordered layout: each partition carries a slice of
        # EVERY snapshot. Right models the snapshot-ordered layout: one partition
        # per snapshot, whole.
        left = write_run(
            root / "left",
            "model",
            [{0: snap0[:2], 1: snap1[:1]}, {0: snap0[2:], 1: snap1[1:]}],
        )
        # Reversed within each snapshot, so the two runs agree as sets but not as
        # sequences. A comparator that dropped its per-id sort alignment and
        # compared row-by-row would fail here.
        right = write_run(root / "right", "model", [{0: snap0[::-1]}, {1: snap1[::-1]}])

        status, report = compare(left, right)
        assert_status(status, IDENTICAL, report, "split across partitions")
    print("  ✓ galaxies are aggregated by snapshot and aligned by id, not compared row by row")


# --------------------------------------------------------------------------
# Divergence between the runs: values, ids, and duplicates
# --------------------------------------------------------------------------


def test_perturbed_float_field_is_detected():
    """A one-ulp change to a float field in one record is a difference."""
    print("Testing a perturbed float value...")
    with scratch() as root:
        left_records = make_records([1_000_000_001, 1_000_000_002, 1_000_000_003])
        right_records = left_records.copy()
        right_records["StellarMass"][1] = numpy.nextafter(
            right_records["StellarMass"][1], numpy.inf
        )

        left, right = build_pair(root, [{0: left_records}], [{0: right_records}])
        status, report = compare(left, right)
        assert_status(status, DIFFERENT, report, "one perturbed value")
        assert "StellarMass" in report, f"the differing field was not named\n{report}"
    print("  ✓ a one-ulp float change is detected and the field is named")


def test_perturbation_in_the_last_field_of_a_later_snapshot_is_detected():
    """A difference in the final field, in a non-first snapshot, is still found.

    Placed there deliberately, because "some difference is detected" is a weaker
    claim than "every field of every shared snapshot is compared". Two real
    comparator defects survive a fixture whose only divergence is an early field
    of the first snapshot: dropping the last field from the per-field loop, and
    comparing only the first shared snapshot. Both were confirmed to pass the rest
    of this file before this case existed. Snap000 is identical here so the
    divergence cannot be found early, and the perturbed field is the last one in
    the record.
    """
    print("Testing the last field of a later snapshot...")
    with scratch() as root:
        first = make_records([1_000_000_001, 1_000_000_002])
        later = make_records([2_000_000_001, 2_000_000_002])
        perturbed = later.copy()
        # RECORD_DTYPE's final field, so a loop that stops one short misses it.
        assert RECORD_DTYPE.names[-1] == "Len"
        perturbed["Len"][1] += 1

        left, right = build_pair(root, [{0: first, 1: later}], [{0: first, 1: perturbed}])
        status, report = compare(left, right)
        assert_status(status, DIFFERENT, report, "last field of a later snapshot")
        assert "Len" in report, f"the differing field was not named\n{report}"
        assert "Snap001" in report, f"the differing snapshot was not named\n{report}"
    print("  ✓ a change to the last field of a later snapshot is detected")


def test_dropped_id_is_detected():
    """A galaxy present in one run and missing from the other is a difference."""
    print("Testing a dropped id...")
    with scratch() as root:
        left_records = make_records([1_000_000_001, 1_000_000_002, 1_000_000_003])
        left, right = build_pair(root, [{0: left_records}], [{0: left_records[:-1]}])
        status, report = compare(left, right)
        assert_status(status, DIFFERENT, report, "dropped id")
        assert "1000000003" in report, f"the missing id was not named\n{report}"
    print("  ✓ a dropped id is detected and named")


def test_duplicated_id_is_detected_before_anything_else():
    """A duplicated id fails, and stops the comparison rather than being described later.

    The comparator checks duplicates first precisely because every later step
    assumes an id names one galaxy. Asserting the report says so pins the
    ordering, not merely the verdict.
    """
    print("Testing a duplicated id...")
    with scratch() as root:
        duplicated = make_records([1_000_000_001, 1_000_000_002, 1_000_000_002])
        clean = make_records([1_000_000_001, 1_000_000_002])
        left, right = build_pair(root, [{0: duplicated}], [{0: clean}])
        status, report = compare(left, right)
        assert_status(status, DIFFERENT, report, "duplicated id")
        assert "duplicated" in report, f"duplication was not reported as such\n{report}"
        assert (
            "no further comparison is meaningful" in report
        ), f"the comparison continued past a duplicated id instead of stopping\n{report}"
    print("  ✓ a duplicated id fails first and stops the comparison")


def test_one_sided_duplicate_in_an_unshared_snapshot_is_detected():
    """A duplicate in a snapshot only one run has is still named as a duplicate.

    The duplicate scan covers every snapshot of both runs rather than only the
    shared ones. Scanning the intersection would report this as nothing worse
    than a snapshot-set mismatch, losing the more serious fault.
    """
    print("Testing a one-sided duplicate in an unshared snapshot...")
    with scratch() as root:
        shared = make_records([1_000_000_001, 1_000_000_002])
        extra = make_records([3_000_000_001, 3_000_000_001])
        # Deliberately on the RIGHT run, because the other duplicate case puts
        # its defect on the left: a scan that checked only one side would
        # otherwise pass both.
        left, right = build_pair(root, [{0: shared}], [{0: shared, 5: extra}])
        status, report = compare(left, right)
        assert_status(status, DIFFERENT, report, "one-sided duplicate")
        assert "duplicated" in report, f"the one-sided duplicate was not reported\n{report}"
        assert "Snap005" in report, f"the unshared snapshot holding it was not named\n{report}"
    print("  ✓ a duplicate in an unshared snapshot is found and named")


# --------------------------------------------------------------------------
# Why the comparison is on bytes: signed zero and NaN payloads
# --------------------------------------------------------------------------


def test_signed_zero_is_a_difference():
    """-0.0 against 0.0 is a difference, though the two compare equal as values."""
    print("Testing signed zero...")
    with scratch() as root:
        left_records = make_records([1_000_000_001, 1_000_000_002])
        left_records["StellarMass"][:] = [0.0, 1.0]
        right_records = left_records.copy()
        right_records["StellarMass"][0] = -0.0

        # Guard the premise: if these ever stop comparing equal as values, this
        # case has stopped testing what it claims to.
        assert left_records["StellarMass"][0] == right_records["StellarMass"][0]

        left, right = build_pair(root, [{0: left_records}], [{0: right_records}])
        status, report = compare(left, right)
        assert_status(status, DIFFERENT, report, "signed zero")
    print("  ✓ a sign flip on zero is detected despite comparing equal as a value")


def test_identical_nan_payloads_are_not_a_difference():
    """Matching NaNs compare equal, though NaN != NaN as a value.

    The direction a naive ``==`` gets wrong: it would report every NaN as a
    difference on every run, making the gate fail permanently on data that agrees.
    """
    print("Testing identical NaN payloads...")
    with scratch() as root:
        records = make_records([1_000_000_001, 1_000_000_002])
        records["StellarMass"][0] = numpy.nan
        assert records["StellarMass"][0] != records["StellarMass"][0]

        left, right = build_pair(root, [{0: records}], [{0: records.copy()}])
        status, report = compare(left, right)
        assert_status(status, IDENTICAL, report, "identical NaN payloads")
    print("  ✓ identical NaN payloads compare equal")


def test_differing_nan_payloads_are_a_difference():
    """Two different NaN bit patterns are a difference, though both are NaN.

    The direction ``equal_nan=True`` gets wrong: it would treat any two NaNs as
    agreeing and hide a changed payload.
    """
    print("Testing differing NaN payloads...")
    with scratch() as root:
        left_records = make_records([1_000_000_001, 1_000_000_002])
        right_records = left_records.copy()
        # Two distinct quiet-NaN payloads, written through the raw bit pattern
        # because arithmetic will not preserve a chosen payload.
        left_records["StellarMass"][0] = numpy.frombuffer(
            numpy.uint64(0x7FF8000000000001).tobytes(), dtype=numpy.float64
        )[0]
        right_records["StellarMass"][0] = numpy.frombuffer(
            numpy.uint64(0x7FF8000000000002).tobytes(), dtype=numpy.float64
        )[0]
        assert numpy.isnan(left_records["StellarMass"][0])
        assert numpy.isnan(right_records["StellarMass"][0])

        left, right = build_pair(root, [{0: left_records}], [{0: right_records}])
        status, report = compare(left, right)
        assert_status(status, DIFFERENT, report, "differing NaN payloads")
    print("  ✓ differing NaN payloads are detected")


# --------------------------------------------------------------------------
# Structural disagreements
# --------------------------------------------------------------------------


def test_snapshot_set_mismatch_is_detected():
    """Runs covering different output snapshots are a difference."""
    print("Testing a snapshot-set mismatch...")
    with scratch() as root:
        records = make_records([1_000_000_001])
        left, right = build_pair(root, [{0: records, 1: records}], [{0: records}])
        status, report = compare(left, right)
        assert_status(status, DIFFERENT, report, "snapshot-set mismatch")
        assert "snapshot sets differ" in report, f"the mismatch was not named\n{report}"
    print("  ✓ differing output snapshot sets are detected")


def test_schema_mismatch_is_detected():
    """Runs whose Galaxies records carry different fields are a difference."""
    print("Testing a schema mismatch...")
    with scratch() as root:
        left_records = make_records([1_000_000_001])
        widened = numpy.zeros(
            1, dtype=numpy.dtype([("UniqueGalaxyID", "<i8"), ("StellarMass", "<f8")])
        )
        widened["UniqueGalaxyID"] = left_records["UniqueGalaxyID"]
        widened["StellarMass"] = left_records["StellarMass"]

        left, right = build_pair(root, [{0: left_records}], [{0: widened}])
        status, report = compare(left, right)
        assert_status(status, DIFFERENT, report, "schema mismatch")
        assert "field schemas" in report, f"the schema difference was not named\n{report}"
    print("  ✓ differing field schemas are detected")


# --------------------------------------------------------------------------
# Unreadable input is an input error, never a difference
# --------------------------------------------------------------------------


def test_truncated_partition_is_an_input_error():
    """A corrupt partition exits 2, not 1.

    The distinction is the point: a truncated file reported as a bitwise
    difference would send someone hunting a physics bug that does not exist.
    """
    print("Testing a truncated partition...")
    with scratch() as root:
        records = make_records([1_000_000_001, 1_000_000_002])
        left, right = build_pair(root, [{0: records}], [{0: records}])

        target = root / "right" / "model_000.hdf5"
        payload = target.read_bytes()
        target.write_bytes(payload[: len(payload) // 2])

        status, report = compare(left, right)
        assert_status(status, INPUT_ERROR, report, "truncated partition")
    print("  ✓ a truncated partition is an input error, not a difference")


def test_missing_run_directory_is_an_input_error():
    """A run directory that does not exist exits 2."""
    print("Testing a missing run directory...")
    with scratch() as root:
        records = make_records([1_000_000_001])
        left = write_run(root / "left", "model", [{0: records}])
        status, report = compare(left, str(root / "absent" / "model"))
        assert_status(status, INPUT_ERROR, report, "missing directory")
    print("  ✓ a missing run directory is an input error")


def test_directory_without_partitions_is_an_input_error():
    """A directory holding no numbered partition files exits 2.

    An empty comparison must never be reported as success -- the same reasoning
    that makes a missing dataset fail the gate rather than skip it.
    """
    print("Testing a directory with no partition files...")
    with scratch() as root:
        records = make_records([1_000_000_001])
        left = write_run(root / "left", "model", [{0: records}])
        empty = root / "empty"
        empty.mkdir()
        status, report = compare(left, str(empty / "model"))
        assert_status(status, INPUT_ERROR, report, "no partition files")
    print("  ✓ a directory with no partition files is an input error")


def main():
    """Run this file's tests via the shared framework runner."""
    return run_test_suite(
        [
            test_identical_runs_compare_equal,
            test_records_are_aggregated_across_partitions,
            test_perturbed_float_field_is_detected,
            test_perturbation_in_the_last_field_of_a_later_snapshot_is_detected,
            test_dropped_id_is_detected,
            test_duplicated_id_is_detected_before_anything_else,
            test_one_sided_duplicate_in_an_unshared_snapshot_is_detected,
            test_signed_zero_is_a_difference,
            test_identical_nan_payloads_are_not_a_difference,
            test_differing_nan_payloads_are_a_difference,
            test_snapshot_set_mismatch_is_detected,
            test_schema_mismatch_is_detected,
            test_truncated_partition_is_an_input_error,
            test_missing_run_directory_is_an_input_error,
            test_directory_without_partitions_is_an_input_error,
        ],
        "Cross-format identity comparator (test_compare_cross_format_identity.py)",
    )


if __name__ == "__main__":
    sys.exit(main())
