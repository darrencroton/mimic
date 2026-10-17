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
evidence during the horizontal-driver work by hand-perturbing real output, then
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
import hashlib
import io
import json
import re
import shutil
import sys
import tempfile
import tracemalloc
from pathlib import Path

import h5py
import numpy
from numpy.lib.recfunctions import repack_fields


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


def numbered_filename(basename, index):
    """The name of numbered partition `index`: ``<basename>_<index>.hdf5``."""
    return f"{basename}_{index:03d}.hdf5"


def task_filename(basename, key):
    """The name of the partition for `key` = (snapshot, task): ``<basename>_<snap>_task<task>``."""
    snap, task = key
    return f"{basename}_{snap:03d}_task{task:03d}.hdf5"


def write_run(directory, basename, partitions, filename=numbered_filename):
    """Write one run's partition files and master file; return the comparator's spec string.

    `partitions` is a list of dicts, one per partition file, mapping a snapshot
    number to that partition's records -- which is the layout the comparator
    indexes: a vertical run spreads every snapshot across many partitions, a
    horizontal run writes one snapshot per partition. A dict of such dicts names
    each partition by its own key instead of its list index, and `filename(basename,
    key)` turns that key into the file name; ``task_filename`` with keys
    (snapshot, task) is the layout a multi-task horizontal run produces. The
    master ``<basename>.hdf5`` links every partition's Galaxies, as a run's does.
    """
    directory.mkdir(parents=True, exist_ok=True)
    keyed = partitions.items() if isinstance(partitions, dict) else enumerate(partitions)
    with h5py.File(directory / f"{basename}.hdf5", "w") as master:
        for number, (key, snapshots) in enumerate(keyed):
            name = filename(basename, key)
            with h5py.File(directory / name, "w") as handle:
                for snap, records in sorted(snapshots.items()):
                    handle.create_dataset(f"Snap{snap:03d}/Galaxies", data=records)
                    master[f"Snap{snap:03d}/File{number:03d}/Galaxies"] = h5py.ExternalLink(
                        name, f"Snap{snap:03d}/Galaxies"
                    )
    return str(directory / basename)


def compare(left_spec, right_spec, *options):
    """Run the comparator, returning its exit status and captured report."""
    captured = io.StringIO()
    with contextlib.redirect_stdout(captured), contextlib.redirect_stderr(captured):
        status = comparator.main([left_spec, right_spec, *options])
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

    This is the arrangement the gate actually compares -- a vertical run
    spreads each snapshot over several partition files while a horizontal
    run writes one file per snapshot -- so a comparator that compared partitions
    pairwise instead of aggregating by snapshot would report a false difference
    on every real gate run.
    """
    print("Testing aggregation across partitions...")
    with scratch() as root:
        snap0 = make_records([1_000_000_001, 1_000_000_002, 1_000_000_003])
        snap1 = make_records([2_000_000_001, 2_000_000_002])

        # Left models the vertical layout: each partition carries a slice of
        # EVERY snapshot. Right models the horizontal layout: one partition
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


def test_zero_id_is_a_failure():
    """A UniqueGalaxyID of 0 is neither a tree id nor a created id, so it fails the comparison
    with or without --compare-created, even when both runs carry the identical row and every
    other field matches, and the created-row summary counts the negative id only."""
    print("Testing a zero id...")
    with scratch() as root:
        records = make_records([1_000_000_001, 0, -1_025])
        left, right = build_pair(root, [{0: records}], [{0: records}])
        for options in ((), ("--compare-created",)):
            what = f"zero id with options {options}"
            status, report = compare(left, right, *options)
            assert_status(status, DIFFERENT, report, what)
            assert (
                f"{comparator.ID_FIELD} 0 is neither" in report
            ), f"{what}: the zero id was not reported\n{report}"
            assert (
                f"{comparator.ID_FIELD} < 0), " in report and ": left 1, right 1" in report
            ), f"{what}: the created-row summary did not count the one negative id\n{report}"
    print("  ✓ a zero id fails the comparison either way; created ids stay uncompared")


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
# Multi-task partition names and created-row comparison
# --------------------------------------------------------------------------


def test_task_suffixed_partitions_are_aggregated_in_snapshot_task_order():
    """A multi-task run's ``_task<digits>`` partitions are found, ordered, and aggregated.

    The same galaxies split over several tasks per snapshot must compare equal
    to a serial run writing one unsuffixed file per snapshot, and discovery must
    order by (snapshot, task) numerically. ``%03d`` pads tasks below 1000 only,
    so snapshot 16's tasks 999 and 1000 are named ``task999`` and ``task1000``:
    a plain name sort puts ``task1000`` first, and this ordering assertion
    fails under it.
    """
    print("Testing task-suffixed partition names...")
    with scratch() as root:
        snap16 = make_records([1_000_000_001, 1_000_000_002, 1_000_000_003])
        snap20 = make_records([2_000_000_001, 2_000_000_002])

        serial = write_run(root / "serial", "model", [{16: snap16}, {20: snap20}])
        tasks = write_run(
            root / "tasks",
            "model",
            {
                (20, 10): {20: snap20[1:]},
                (16, 1000): {16: snap16[2:]},
                (16, 0): {16: snap16[:2]},
                (20, 2): {20: snap20[:1]},
                # A task holding no galaxies at a snapshot still writes its partition.
                (16, 999): {16: snap16[:0]},
            },
            task_filename,
        )

        names = [path.name for path in comparator.partition_files(tasks)]
        expected = [
            "model_016_task000.hdf5",
            "model_016_task999.hdf5",
            "model_016_task1000.hdf5",
            "model_020_task002.hdf5",
            "model_020_task010.hdf5",
        ]
        # Guard the premise: a name sort must disagree, or this stops testing it.
        assert sorted(expected) != expected, "the fixture no longer defeats a plain name sort"
        assert (
            names == expected
        ), f"task-suffixed partitions are not in (snapshot, task) order: {names}"

        status, report = compare(serial, tasks)
        assert_status(status, IDENTICAL, report, "task-suffixed against serial")
        assert "5 partition file(s)" in report, f"not every task partition was read\n{report}"
    print("  ✓ task-suffixed partitions are discovered in (snapshot, task) order and aggregated")


def test_mixed_suffixed_and_unsuffixed_partitions_are_an_input_error():
    """One run holding both task-suffixed and unsuffixed partitions exits 2.

    A run writes one kind or the other, so a mixture is stale or merged output;
    aggregating both would double-count galaxies and be reported as a
    difference -- or, worse, a duplicate -- that the physics never produced.
    """
    print("Testing mixed partition names...")
    with scratch() as root:
        records = make_records([1_000_000_001, 1_000_000_002])
        left = write_run(root / "left", "model", [{0: records}])
        mixed = write_run(root / "mixed", "model", [{0: records[:1]}])
        write_run(root / "mixed", "model", {(1, 0): {1: records[1:]}}, task_filename)

        status, report = compare(left, mixed)
        assert_status(status, INPUT_ERROR, report, "mixed partition names")
        assert "task-suffixed" in report, f"the mixture was not named\n{report}"
    print("  ✓ mixing task-suffixed and unsuffixed partitions is an input error")


def test_compare_created_accepts_equal_created_rows():
    """Under --compare-created, identical created rows compare equal and are counted as compared."""
    print("Testing --compare-created on equal created rows...")
    with scratch() as root:
        records = make_records([1_000_000_001, -1_025, -2_049])
        left, right = build_pair(root, [{0: records}], [{0: records[::-1].copy()}])

        status, report = compare(left, right, "--compare-created")
        assert_status(status, IDENTICAL, report, "equal created rows under --compare-created")
        assert "PASSED: 3 galaxies" in report, f"created rows were not compared\n{report}"
        assert "included in the comparison" in report, f"created rows not reported\n{report}"
    print("  ✓ equal created rows compare identical under --compare-created")


def test_compare_created_detects_differing_created_rows():
    """Under --compare-created a created row's field difference fails; by default it does not.

    Both directions are asserted: the flag must catch the difference, and its
    absence must leave the existing tree-rows-only verdict unchanged, which is
    what every existing cross-driver gate relies on.
    """
    print("Testing --compare-created on differing created rows...")
    with scratch() as root:
        left_records = make_records([1_000_000_001, -1_025, -2_049])
        right_records = left_records.copy()
        right_records["StellarMass"][2] = numpy.nextafter(
            right_records["StellarMass"][2], numpy.inf
        )
        left, right = build_pair(root, [{0: left_records}], [{0: right_records}])

        status, report = compare(left, right, "--compare-created")
        assert_status(status, DIFFERENT, report, "differing created rows under --compare-created")
        assert "-2049" in report, f"the differing created id was not named\n{report}"

        status, report = compare(left, right)
        assert_status(status, IDENTICAL, report, "differing created rows by default")
        assert "excluded from the comparison" in report, f"created rows not excluded\n{report}"

        # A created id present in one run only is an id-set difference under the flag.
        dropped = write_run(root / "dropped", "model", [{0: left_records[:2]}])
        status, report = compare(left, dropped, "--compare-created")
        assert_status(status, DIFFERENT, report, "dropped created id under --compare-created")
        assert "-2049" in report, f"the missing created id was not named\n{report}"
    print("  ✓ differing created rows fail only under --compare-created")


def test_compare_created_duplicate_created_id_is_detected():
    """A duplicated created id fails the duplicate pass, with or without --compare-created.

    The duplicate pass scans every id of both runs before anything else,
    independently of the flag, which only decides whether created rows are then
    compared. Both invocations are asserted so that stays true.
    """
    print("Testing a duplicated created id under --compare-created...")
    with scratch() as root:
        duplicated = make_records([1_000_000_001, -1_025, -1_025])
        clean = make_records([1_000_000_001, -1_025])
        left, right = build_pair(root, [{0: duplicated}], [{0: clean}])

        for options in (("--compare-created",), ()):
            status, report = compare(left, right, *options)
            what = f"duplicated created id with options {options}"
            assert_status(status, DIFFERENT, report, what)
            assert "-1025 appears 2 times" in report, f"{what}: id not named\n{report}"
            assert "no further comparison is meaningful" in report, f"{what}: continued\n{report}"
    print("  ✓ a duplicated created id fails first, with or without --compare-created")


# --------------------------------------------------------------------------
# Alternate pairing: tree rows by catalogue key, created rows by lineage
# --------------------------------------------------------------------------

#: A record carrying everything the alternate pairings read: both identity
#: fields, the catalogue key, and one payload field to perturb.
PAIRED_DTYPE = numpy.dtype(
    [
        ("UniqueGalaxyID", "<i8"),
        ("UniqueCentralGalaxyID", "<i8"),
        ("MostBoundID", "<i8"),
        ("SnapNum", "<i4"),
        ("StellarMass", "<f8"),
    ]
)

#: The snapshot every paired record lives in.
PAIRED_SNAP = 5


def created_id(host_key, ordinal):
    """A created record's id, as galaxy_id.h encodes it: -(1 + ordinal + 1024 * host_key)."""
    return -(1 + ordinal + comparator.MAX_CREATED_RECORDS_PER_HOST * host_key)


def paired_records(tree, created, tree_ids, host_keys, snap=PAIRED_SNAP):
    """One run's records for output snapshot `snap`.

    `tree` is a list of (MostBoundID, central MostBoundID, mass); `created` a list
    of (host MostBoundID, ordinal, mass), each inheriting its host's central as
    a created record does. `tree_ids` maps a MostBoundID to this run's tree id
    and `host_keys` a host MostBoundID to this run's created-id host key, which
    is how two enumerations of one simulation label the same galaxies apart.
    """
    centrals = {mbid: central for mbid, central, _ in tree}
    rows = [(tree_ids[mbid], tree_ids[central], mbid, mass) for mbid, central, mass in tree]
    rows += [
        (created_id(host_keys[host], ordinal), tree_ids[centrals[host]], host, mass)
        for host, ordinal, mass in created
    ]
    records = numpy.zeros(len(rows), dtype=PAIRED_DTYPE)
    for index, (galaxy_id, central, mbid, mass) in enumerate(rows):
        records[index] = (galaxy_id, central, mbid, snap, mass)
    return records


#: The science both runs share: three tree galaxies in two FoF groups and three
#: created satellites, two of them on one host.
PAIRED_TREE = [(11, 11, 1.5), (12, 11, 2.5), (21, 21, 3.5)]
PAIRED_CREATED = [(11, 0, 0.25), (11, 1, 0.5), (21, 0, 0.75)]
LEFT_TREE_IDS = {11: 1_000_001, 12: 1_000_002, 21: 2_000_001}
RIGHT_TREE_IDS = {11: 5_000_001, 12: 5_000_002, 21: 4_000_001}
LEFT_HOST_KEYS = {11: 7, 21: 9}
RIGHT_HOST_KEYS = {11: 3, 21: 40}

BOTH_PAIRINGS = ("--match-by-catalogue-id", "--match-created-by-lineage")


def paired_run(root, name, tree=PAIRED_TREE, created=PAIRED_CREATED, tree_ids=None, keys=None):
    """Write one run of the paired science, labelled the right run's way unless told otherwise."""
    records = paired_records(tree, created, tree_ids or RIGHT_TREE_IDS, keys or RIGHT_HOST_KEYS)
    return write_run(root / name, "model", [{PAIRED_SNAP: records}])


def test_created_id_ordinal_decoding_matches_the_c_encoding():
    """The copied radix is the C constant, and the ordinal decodes as the C decoder does.

    The radix is copied from src/include/constants.h; reading the header here
    makes a change on the C side fail this test instead of silently mispairing
    every created record.
    """
    print("Testing the created-id ordinal decoding...")
    header = (REPO_ROOT / "src" / "include" / "constants.h").read_text()
    defined = [
        int(line.split()[2])
        for line in header.splitlines()
        if line.startswith("#define MAX_CREATED_RECORDS_PER_HOST ")
    ]
    assert defined == [comparator.MAX_CREATED_RECORDS_PER_HOST], f"constants.h defines {defined}"

    host_keys = numpy.array([0, 1, 9, 2**52], dtype=numpy.int64)
    for ordinal in (0, 1, comparator.MAX_CREATED_RECORDS_PER_HOST - 1):
        ids = numpy.array([created_id(int(key), ordinal) for key in host_keys], dtype=numpy.int64)
        decoded = comparator.created_ordinals(ids)
        assert (decoded == ordinal).all(), f"ordinal {ordinal} decoded as {decoded}"
    # The most negative id decodes without overflow: -(id + 1) is INT64_MAX.
    lowest = numpy.array([numpy.iinfo(numpy.int64).min], dtype=numpy.int64)
    expected = numpy.iinfo(numpy.int64).max % comparator.MAX_CREATED_RECORDS_PER_HOST
    assert comparator.created_ordinals(lowest)[0] == expected
    print("  ✓ the radix matches constants.h and ordinals decode as -(id + 1) mod radix")


def test_alternate_pairings_accept_equal_science_with_different_labels():
    """The same galaxies under different ids compare identical once paired by what they are.

    Each run labels the galaxies its own way: created ids encode a different
    host row, and in the second half the tree ids enumerate the forests
    differently too, with every central relabelled to match. The default rule
    is shown to fail on the relabelled runs first, so a pass is the pairing's
    doing and not an accident of equal ids.
    """
    print("Testing equal science under different labels...")
    with scratch() as root:
        left = paired_run(root, "left", tree_ids=LEFT_TREE_IDS, keys=LEFT_HOST_KEYS)

        # Tree ids equal, created ids relabelled: the lineage pairing alone.
        lineage = paired_run(root, "lineage", tree_ids=LEFT_TREE_IDS)
        status, report = compare(left, lineage, "--compare-created")
        assert_status(status, DIFFERENT, report, "relabelled created rows paired by id")
        status, report = compare(left, lineage, "--match-created-by-lineage")
        assert_status(status, IDENTICAL, report, "relabelled created rows by lineage")
        assert "PASSED: 6 galaxies" in report, f"not every record was compared\n{report}"
        assert "identical under the lineage comparator" in report, report
        assert "3 created row(s) compared" in report, f"created count not reported\n{report}"

        # Every id relabelled: both pairings together.
        relabelled = paired_run(root, "relabelled")
        status, report = compare(left, relabelled)
        assert_status(status, DIFFERENT, report, "relabelled tree rows paired by id")
        status, report = compare(left, relabelled, *BOTH_PAIRINGS)
        assert_status(status, IDENTICAL, report, "relabelled runs under both pairings")
        assert "catalogue-key and lineage comparator" in report, report
        assert "PASSED: 6 galaxies" in report, f"not every record was compared\n{report}"
    print("  ✓ relabelled runs compare identical by catalogue key and lineage, and not by id")


def test_lineage_mismatch_and_created_field_difference_fail():
    """A created record whose lineage has no partner fails, as does a created field difference."""
    print("Testing a lineage mismatch...")
    with scratch() as root:
        left = paired_run(root, "left", tree_ids=LEFT_TREE_IDS, keys=LEFT_HOST_KEYS)

        # Host 11's second satellite carries ordinal 2 instead of 1.
        shifted = [(11, 0, 0.25), (11, 2, 0.5), (21, 0, 0.75)]
        right = paired_run(root, "shifted", created=shifted)
        status, report = compare(left, right, *BOTH_PAIRINGS)
        assert_status(status, DIFFERENT, report, "lineage mismatch")
        assert "created-row key sets differ" in report, f"not reported as unpaired\n{report}"
        assert "ordinal=1" in report and "ordinal=2" in report, f"keys not named\n{report}"

        heavier = [(11, 0, 0.25), (11, 1, 0.5), (21, 0, numpy.nextafter(0.75, 1.0))]
        right = paired_run(root, "heavier", created=heavier)
        status, report = compare(left, right, *BOTH_PAIRINGS)
        assert_status(status, DIFFERENT, report, "created field difference")
        assert "StellarMass: 1 record(s) differ" in report, f"field not named\n{report}"
    print("  ✓ an unpaired lineage and a created field difference both fail")


def test_duplicate_catalogue_key_fails():
    """Two tree rows sharing (MostBoundID, SnapNum) in one run fail the catalogue pairing."""
    print("Testing a duplicate catalogue key...")
    with scratch() as root:
        left = paired_run(root, "left", tree_ids=LEFT_TREE_IDS, keys=LEFT_HOST_KEYS)
        records = paired_records(PAIRED_TREE, PAIRED_CREATED, RIGHT_TREE_IDS, RIGHT_HOST_KEYS)
        twin = records[1:2].copy()
        twin["UniqueGalaxyID"] = 5_000_009  # a distinct id, so only the key repeats
        right = write_run(
            root / "right", "model", [{PAIRED_SNAP: numpy.concatenate([records, twin])}]
        )

        status, report = compare(left, right, *BOTH_PAIRINGS)
        assert_status(status, DIFFERENT, report, "duplicate catalogue key")
        assert "duplicated tree-row key" in report, f"duplicate not reported\n{report}"
        assert "(MostBoundID=12, SnapNum=5) appears 2 times" in report, report
    print("  ✓ a duplicated catalogue key fails")


def test_broken_central_mapping_fails():
    """A central that does not map through the pairing fails, naming both centrals.

    The second run's galaxy 12 names the record paired with the first run's
    galaxy 21 as its central. Its own value is a valid id of that run, so only
    a comparison through the pairing can see it is the wrong one.
    """
    print("Testing a broken central mapping...")
    with scratch() as root:
        records = paired_records(PAIRED_TREE, PAIRED_CREATED, RIGHT_TREE_IDS, RIGHT_HOST_KEYS)
        records["UniqueCentralGalaxyID"][records["MostBoundID"] == 12] = RIGHT_TREE_IDS[21]
        right = write_run(root / "right", "model", [{PAIRED_SNAP: records[:3]}])
        left_tree = write_run(
            root / "left-tree",
            "model",
            [{PAIRED_SNAP: paired_records(PAIRED_TREE, [], LEFT_TREE_IDS, LEFT_HOST_KEYS)}],
        )

        status, report = compare(left_tree, right, "--match-by-catalogue-id")
        assert_status(status, DIFFERENT, report, "broken central mapping")
        assert (
            "central 1000001 maps to 5000001, right central 4000001" in report
        ), f"both centrals not named\n{report}"
    print("  ✓ a central that does not map through the pairing fails and names both")


def test_alternate_pairings_require_each_id_to_keep_its_partner():
    """A galaxy relabelled partway through its history in one run fails; a consistent
    relabelling passes.

    UniqueGalaxyID is persistent, so the pairing of two runs' ids must be the same at
    every compared snapshot. Three relabellings at the second snapshot each fail: a
    second-run tree id (its satellite's central follows it, so every snapshot agrees
    with itself), a first-run tree id, seen from the second run's side, and a
    second-run created id under the lineage pairing.
    """
    print("Testing that a paired id keeps its partner across snapshots...")
    with scratch() as root:

        def run(name, ids, keys, later_ids=None, later_keys=None):
            snapshots = {
                PAIRED_SNAP: paired_records(PAIRED_TREE, PAIRED_CREATED, ids, keys),
                PAIRED_SNAP
                + 1: paired_records(
                    PAIRED_TREE,
                    PAIRED_CREATED,
                    later_ids or ids,
                    later_keys or keys,
                    PAIRED_SNAP + 1,
                ),
            }
            return write_run(root / name, "model", [snapshots])

        left = run("left", LEFT_TREE_IDS, LEFT_HOST_KEYS)
        status, report = compare(
            left, run("consistent", RIGHT_TREE_IDS, RIGHT_HOST_KEYS), *BOTH_PAIRINGS
        )
        assert_status(status, IDENTICAL, report, "a relabelling kept on every snapshot")

        relabelled_right = {**RIGHT_TREE_IDS, 11: 7_000_009}
        relabelled_left = {**LEFT_TREE_IDS, 21: 2_000_777}
        for what, first, second, line in (
            (
                "a second-run tree id",
                left,
                run("tree", RIGHT_TREE_IDS, RIGHT_HOST_KEYS, relabelled_right),
                "left UniqueGalaxyID 1000001: paired with 7000009 here, with 5000001 at Snap005",
            ),
            (
                "a first-run tree id",
                run("first", LEFT_TREE_IDS, LEFT_HOST_KEYS, relabelled_left),
                run("plain", RIGHT_TREE_IDS, RIGHT_HOST_KEYS),
                "right UniqueGalaxyID 4000001: paired with 2000777 here, with 2000001 at Snap005",
            ),
            (
                "a second-run created id",
                left,
                run("created", RIGHT_TREE_IDS, RIGHT_HOST_KEYS, None, {**RIGHT_HOST_KEYS, 21: 41}),
                f"paired with {created_id(41, 0)} here, with {created_id(40, 0)} at Snap005",
            ),
        ):
            status, report = compare(first, second, *BOTH_PAIRINGS)
            assert_status(status, DIFFERENT, report, what)
            assert "changed partner since Snap005" in report and line in report, f"{what}\n{report}"
    print("  ✓ an id that changes partner fails; a relabelling kept on every snapshot passes")


def test_alternate_pairings_fail_a_zero_id_and_an_unpaired_tree_row():
    """A zero id and a tree row in one run only each fail under the alternate pairings.

    Neither row belongs to a paired kind, so each is a failure only because it is
    checked for in its own right.
    """
    print("Testing a zero id and an unpaired tree row under the alternate pairings...")
    with scratch() as root:
        left = paired_run(root, "left", tree_ids=LEFT_TREE_IDS, keys=LEFT_HOST_KEYS)
        records = paired_records(PAIRED_TREE, PAIRED_CREATED, LEFT_TREE_IDS, LEFT_HOST_KEYS)
        zero = records[:1].copy()
        zero["UniqueGalaxyID"], zero["MostBoundID"] = 0, 99
        right = paired_records(PAIRED_TREE, PAIRED_CREATED, RIGHT_TREE_IDS, RIGHT_HOST_KEYS)
        zeroed = (
            write_run(
                root / "zero-left", "model", [{PAIRED_SNAP: numpy.concatenate([records, zero])}]
            ),
            write_run(
                root / "zero-right", "model", [{PAIRED_SNAP: numpy.concatenate([right, zero])}]
            ),
        )
        status, report = compare(*zeroed, *BOTH_PAIRINGS)
        assert_status(status, DIFFERENT, report, "a zero id under the alternate pairings")
        assert "UniqueGalaxyID 0 is neither" in report, f"the zero id was not reported\n{report}"

        # Galaxy 12 is a satellite with no created record, so nothing else fails with it.
        dropped = paired_run(root, "dropped", tree=[row for row in PAIRED_TREE if row[0] != 12])
        status, report = compare(left, dropped, "--match-by-catalogue-id")
        assert_status(status, DIFFERENT, report, "a tree row in one run only")
        assert (
            "tree-row key sets differ" in report and "(MostBoundID=12, SnapNum=5)" in report
        ), report
    print("  ✓ a zero id and an unpaired tree row each fail")


def test_lineage_pairing_fails_a_wrong_central_and_a_duplicated_key():
    """A created record naming the wrong central fails, and so do two created records
    sharing a lineage key.

    The twin carries an id of its own, so the run-wide duplicate-id pass cannot see
    it: only the key check can.
    """
    print("Testing a created record's central and a duplicated lineage key...")
    with scratch() as root:
        left = paired_run(root, "left", tree_ids=LEFT_TREE_IDS, keys=LEFT_HOST_KEYS)
        records = paired_records(PAIRED_TREE, PAIRED_CREATED, RIGHT_TREE_IDS, RIGHT_HOST_KEYS)
        moved = records.copy()
        moved["UniqueCentralGalaxyID"][moved["UniqueGalaxyID"] == created_id(40, 0)] = (
            RIGHT_TREE_IDS[11]
        )
        status, report = compare(
            left, write_run(root / "moved", "model", [{PAIRED_SNAP: moved}]), *BOTH_PAIRINGS
        )
        assert_status(status, DIFFERENT, report, "a created record's wrong central")
        assert "central 2000001 maps to 4000001, right central 5000001" in report, report

        twin = records[records["UniqueGalaxyID"] == created_id(3, 0)].copy()
        twin["UniqueGalaxyID"] = created_id(99, 0)
        twinned = write_run(
            root / "twin", "model", [{PAIRED_SNAP: numpy.concatenate([records, twin])}]
        )
        status, report = compare(left, twinned, *BOTH_PAIRINGS)
        assert_status(status, DIFFERENT, report, "a duplicated lineage key")
        assert "duplicated created-row key" in report, f"duplicate not reported\n{report}"
        assert "(MostBoundID=11, SnapNum=5, ordinal=0) appears 2 times" in report, report
    print("  ✓ a created record's wrong central and a duplicated lineage key both fail")


def test_lineage_key_beyond_its_packing_is_an_input_error():
    """A created record whose MostBoundID the int64 lineage key cannot pack exits 2."""
    print("Testing a MostBoundID beyond the lineage key's range...")
    # both extremes: the negative one is where an absolute-value check would wrap
    for mbid in (2**62, -(2**63)):
        with scratch() as root:
            tree, created = [(mbid, mbid, 1.0)], [(mbid, 0, 0.5)]
            runs = [
                paired_run(
                    root,
                    name,
                    tree=tree,
                    created=created,
                    tree_ids={mbid: 1_000_001},
                    keys={mbid: 7},
                )
                for name in ("left", "right")
            ]
            status, report = compare(*runs, "--match-created-by-lineage")
            assert_status(
                status, INPUT_ERROR, report, f"a MostBoundID {mbid} beyond the lineage key"
            )
            assert f"MostBoundID {mbid} is beyond" in report, report
    print("  ✓ a MostBoundID the lineage key cannot pack is an input error, at both extremes")


def test_alternate_mode_usage_errors():
    """Contradictory options are usage errors (exit 2), before any input is read."""
    print("Testing alternate-mode usage errors...")
    for options in (
        ("--compare-created", "--match-created-by-lineage"),
        ("--match-by-catalogue-id", "--multiplier", "1000"),
        (
            "--multiplier",
            "1000",
            "--left-sidecar",
            "l",
            "--right-sidecar",
            "r",
            "--cut-record",
            "c",
        ),
        ("--report-all-forests",),
    ):
        with contextlib.redirect_stderr(io.StringIO()):
            try:
                comparator.main(["left", "right", *options])
            except SystemExit as stop:
                assert stop.code == INPUT_ERROR, f"{options}: exit {stop.code}"
                continue
        raise AssertionError(f"{options} was accepted")
    print("  ✓ contradictory or incomplete option sets are refused")


# --------------------------------------------------------------------------
# The cut-difference report
# --------------------------------------------------------------------------

#: The UniqueGalaxyID forest multiplier of the cut fixture.
CUT_MULTIPLIER = 1000

#: The uncut dataset's forests by ForestIndex, and the cut dataset's: forest 20
#: is cut into the piece that keeps its id and a fresh piece 100, which
#: enumerates after every original forest (ForestIndex 3). The fresh-id floor
#: is the catalogue's largest forest id, as the census writes it, so forest 30
#: is both unsplit and at the floor.
UNCUT_FORESTS = [10, 20, 30]
CUT_FORESTS = [10, 20, 30, 100]
CUT_FRESH_FLOOR = max(UNCUT_FORESTS)

#: Tree galaxies as (MostBoundID, central MostBoundID, forest index in the
#: uncut and in the cut dataset, halo number, mass). Galaxy 203 lands in the
#: fresh piece, so its id differs between the datasets.
CUT_TREE = [
    (101, 101, 0, 0, 0, 1.0),
    (201, 201, 1, 1, 0, 2.0),
    (202, 201, 1, 1, 1, 3.0),
    (203, 203, 1, 3, 2, 4.0),
    (301, 301, 2, 2, 0, 5.0),
    (302, 301, 2, 2, 1, 6.0),
]

#: Created satellites as (host MostBoundID, ordinal, mass), one in the fresh
#: piece and one in an unchanged forest. A fourth entry, when present, is the
#: MostBoundID the record carries instead of its host's: a record inherited
#: into a later snapshot keeps the one of the host it was born on.
CUT_CREATED = [(203, 0, 0.5), (301, 0, 0.75)]


def cut_records(cut, snap, tree, created):
    """One snapshot of the cut fixture, on the cut dataset's enumeration when `cut` is set."""
    ids = {
        mbid: halo + CUT_MULTIPLIER * ((after if cut else before) + 1)
        for mbid, _, before, after, halo, _ in tree
    }
    centrals = {mbid: central for mbid, central, *_ in tree}
    rows = [(ids[mbid], ids[central], mbid, mass) for mbid, central, *_, mass in tree]
    for index, (host, ordinal, mass, *birth) in enumerate(created):
        key = 100 * snap + index + (50 if cut else 0)
        rows.append((created_id(key, ordinal), ids[centrals[host]], (birth or [host])[0], mass))
    records = numpy.zeros(len(rows), dtype=PAIRED_DTYPE)
    for index, (galaxy_id, central, mbid, mass) in enumerate(rows):
        records[index] = (galaxy_id, central, mbid, snap, mass)
    return records


def cut_run(
    root, name, cut, tree=CUT_TREE, created=CUT_CREATED, later=(), multiplier=CUT_MULTIPLIER
):
    """One run of the cut fixture: PAIRED_SNAP, then `later` (tree, created) snapshots.

    Every partition records `multiplier` as RunProperties/UniqueGalaxyIDMultiplier,
    as a run does.
    """
    snapshots = [(tree, created), *later]
    spec = write_run(
        root / name,
        "model",
        [
            {PAIRED_SNAP + offset: cut_records(cut, PAIRED_SNAP + offset, *contents)}
            for offset, contents in enumerate(snapshots)
        ],
    )
    for index in range(len(snapshots)):
        with h5py.File(root / name / numbered_filename("model", index), "a") as handle:
            properties = handle.create_group("RunProperties")
            properties.attrs["UniqueGalaxyIDMultiplier"] = numpy.int64(multiplier)
    return spec


def dataset_identity(forests):
    """The identity entries a cut record binds to the dataset of sidecar `forests`."""
    column = numpy.array(forests, dtype="<i8")
    return {"forest_id_sha256": hashlib.sha256(column).hexdigest(), "n_forests_total": len(forests)}


def cut_inputs(root, record_forests=UNCUT_FORESTS, pieces=("[20,100]", "[20,20]")):
    """Both sidecars and the cut record; returns the comparator options naming them.

    The record is computed on the dataset of `record_forests` and lists the
    piece columns `pieces` (ids, original forests), as JSON list text.
    """
    for name, forests in (("uncut", UNCUT_FORESTS), ("cut", CUT_FORESTS)):
        with h5py.File(root / f"{name}-forests.h5", "w") as handle:
            handle.create_dataset("ForestID", data=numpy.array(forests, dtype=numpy.int64))
    # The record's layout: one header entry per line, one line per piece column.
    (root / "record.json").write_text(
        "{\n"
        f'  "dataset": {json.dumps(dataset_identity(record_forests), sort_keys=True)},\n'
        f'  "fresh_id_floor": {CUT_FRESH_FLOOR},\n'
        '  "rule": {"text": "the z = 0 FoF groups of every forest"},\n'
        '  "pieces": {\n'
        f'    "id": {pieces[0]},\n'
        f'    "forest_id": {pieces[1]},\n'
        '    "trees": [2,1]\n'
        "  }\n"
        "}\n"
    )
    return (
        *BOTH_PAIRINGS,
        "--multiplier",
        str(CUT_MULTIPLIER),
        "--left-sidecar",
        str(root / "uncut-forests.h5"),
        "--right-sidecar",
        str(root / "cut-forests.h5"),
        "--cut-record",
        str(root / "record.json"),
    )


def heavier(tree, mbid):
    """`tree` with galaxy `mbid`'s mass moved up by one ulp."""
    return [
        (*row[:-1], numpy.nextafter(row[-1], numpy.inf) if row[0] == mbid else row[-1])
        for row in tree
    ]


def test_cut_report_reports_a_split_forest_difference_and_fails_an_unchanged_one():
    """A difference inside a split forest is reported and passes; outside one it fails.

    The same one-ulp change is made to a galaxy of the split forest 20 and to a
    galaxy of forest 30, which the cut leaves alone. Forest 30 is the
    catalogue's largest forest id, the record's fresh-id floor: an unsplit
    forest at the floor is no fresh piece, so identical runs compare clean. A
    galaxy whose catalogue key pairs but whose tree id changed outside a split
    forest fails too: an unchanged forest keeps its ForestIndex and so its ids.
    Every split forest with records gets its line, identical ones included,
    and under --report-all-forests every forest does.
    """
    print("Testing the cut-difference report's split and unchanged forests...")
    with scratch() as root:
        options = cut_inputs(root)
        uncut = cut_run(root, "uncut", cut=False)

        status, report = compare(uncut, cut_run(root, "same", cut=True), *options)
        assert_status(status, IDENTICAL, report, "cut with no difference")
        assert (
            "forest 20: 4 identical, 0 differing, 0 only in left, 0 only in right" in report
        ), f"the identical split forest has no line of its own\n{report}"

        inside = cut_run(root, "inside", cut=True, tree=heavier(CUT_TREE, 202))
        status, report = compare(uncut, inside, *options)
        assert_status(status, IDENTICAL, report, "difference inside a split forest")
        assert (
            "forest 20: 3 identical, 1 differing (StellarMass 1), 0 only in left, 0 only in right"
            in report
        ), f"the split forest's difference was not reported\n{report}"
        assert "outside the split forests" in report, f"the PASSED scope is missing\n{report}"

        outside = cut_run(root, "outside", cut=True, tree=heavier(CUT_TREE, 302))
        status, report = compare(uncut, outside, *options)
        assert_status(status, DIFFERENT, report, "difference in an unchanged forest")
        assert "StellarMass: 1 record(s) differ" in report, f"field not named\n{report}"

        status, report = compare(uncut, outside, *options, "--report-all-forests")
        assert_status(status, IDENTICAL, report, "difference reported under --report-all-forests")
        assert "forest 30: 2 identical, 1 differing (StellarMass 1)" in report, report
        assert "forest 10: 1 identical, 0 differing" in report, f"forest 10 not listed\n{report}"
        assert "total over other forests" in report, report

        moved = [(*row[:4], 7, row[5]) if row[0] == 302 else row for row in CUT_TREE]
        status, report = compare(uncut, cut_run(root, "moved", cut=True, tree=moved), *options)
        assert_status(status, DIFFERENT, report, "tree id changed in an unchanged forest")
        assert "UniqueGalaxyID: 1 record(s) differ" in report, f"id change not named\n{report}"
    print("  ✓ split-forest differences are reported; unchanged-forest differences fail")


def test_cut_report_classifies_a_fresh_piece_record_to_its_original_forest():
    """Records only in the fresh piece are counted against the forest it was cut from.

    One tree galaxy and one created satellite (whose host is in the fresh
    piece) exist only in the cut run. Both are placed through the cut
    dataset's sidecar and the record in forest 20, so they are reported there.
    """
    print("Testing the classification of a fresh piece's unmatched records...")
    with scratch() as root:
        options = cut_inputs(root)
        uncut = cut_run(root, "uncut", cut=False)
        extra_tree = [*CUT_TREE, (204, 203, 1, 3, 3, 4.5)]
        extra_created = [*CUT_CREATED, (203, 1, 0.625)]
        cut = cut_run(root, "cut", cut=True, tree=extra_tree, created=extra_created)

        status, report = compare(uncut, cut, *options)
        assert_status(status, IDENTICAL, report, "unmatched records in a fresh piece")
        assert (
            "forest 20: 4 identical, 0 differing, 0 only in left, 2 only in right" in report
        ), f"the fresh piece's records were not placed in forest 20\n{report}"
    print("  ✓ a fresh piece's unmatched records are reported under their original forest")


def test_cut_report_places_an_inherited_created_record_through_its_central():
    """A created record inherited into a later snapshot is placed in its current host's forest.

    In the second snapshot the satellite born on host 203 still carries 203's
    MostBoundID, which no halo of that snapshot has; its central is the fresh
    piece's galaxy 213. It is placed through that central in forest 20, so the
    two identical runs compare clean rather than failing as unplaceable.
    """
    print("Testing an inherited created record's placement...")
    with scratch() as root:
        options = cut_inputs(root)
        later_tree = [(213, 213, 1, 3, 10, 4.0), (311, 311, 2, 2, 10, 5.0)]
        later = [(later_tree, [(213, 0, 0.5, 203), (311, 0, 0.75, 301)])]
        uncut = cut_run(root, "uncut", cut=False, later=later)
        cut = cut_run(root, "cut", cut=True, later=later)

        status, report = compare(uncut, cut, *options)
        assert_status(status, IDENTICAL, report, "inherited created record")
        assert (
            "forest 20: 6 identical, 0 differing, 0 only in left, 0 only in right" in report
        ), f"the inherited record was not placed in forest 20\n{report}"
    print("  ✓ an inherited created record is placed through its current central")


def test_cut_report_checks_the_multiplier_against_both_runs():
    """A --multiplier neither run records, or two runs recording different ones, exit 2."""
    print("Testing the recorded multiplier...")
    with scratch() as root:
        options = list(cut_inputs(root))
        uncut = cut_run(root, "uncut", cut=False)
        cut = cut_run(root, "cut", cut=True)

        wrong = options.copy()
        wrong[wrong.index("--multiplier") + 1] = "999"
        status, report = compare(uncut, cut, *wrong)
        assert_status(status, INPUT_ERROR, report, "a multiplier the runs do not record")
        assert "--multiplier 999" in report and "1000" in report, f"values not named\n{report}"

        other = cut_run(root, "other", cut=True, multiplier=2 * CUT_MULTIPLIER)
        status, report = compare(uncut, other, *options)
        assert_status(status, INPUT_ERROR, report, "runs recording different multipliers")
        assert "2000" in report, f"the second run's multiplier was not named\n{report}"

        # The runs share a label, so a check keyed by label would see one of them.
        first = cut_run(root, "first", cut=False, multiplier=2 * CUT_MULTIPLIER)
        labels = ("--left-label", "run", "--right-label", "run")
        status, report = compare(first, cut, *options, *labels)
        assert_status(status, INPUT_ERROR, report, "equal labels, different multipliers")
        assert "2000" in report, f"the first run's multiplier was not named\n{report}"
    print("  ✓ the multiplier must be the one both runs record")


def test_cut_report_fails_a_record_unmatched_outside_the_split_forests():
    """A tree galaxy of the unchanged forest 30 present in the cut run only fails."""
    print("Testing an unmatched record outside the split forests...")
    with scratch() as root:
        options = cut_inputs(root)
        extra = cut_run(root, "extra", cut=True, tree=[*CUT_TREE, (303, 301, 2, 2, 2, 7.0)])
        status, report = compare(cut_run(root, "uncut", cut=False), extra, *options)
        assert_status(status, DIFFERENT, report, "an unmatched record in an unchanged forest")
        assert "tree-row key sets differ" in report and "MostBoundID=303" in report, report
    print("  ✓ an unmatched record outside the split forests fails")


def test_cut_report_refuses_inputs_that_do_not_describe_the_runs():
    """Sidecars and records that do not describe the runs exit 2, never 0 or 1.

    A record computed on another dataset; a second sidecar that swaps two
    forests, so galaxy 101's pair lands in forests 10 and 30; a record that does
    not list the cut run's fresh piece 100; and runs without the central field
    that places created records.
    """
    print("Testing cut inputs that do not describe the runs...")
    with scratch() as root:
        uncut, cut = cut_run(root, "uncut", cut=False), cut_run(root, "cut", cut=True)
        status, report = compare(uncut, cut, *cut_inputs(root, record_forests=[10, 20, 31]))
        assert_status(status, INPUT_ERROR, report, "a record of another dataset")
        assert "neither sidecar is" in report, f"the mismatch was not named\n{report}"

        options = cut_inputs(root)
        with h5py.File(root / "cut-forests.h5", "w") as handle:
            handle.create_dataset(
                "ForestID", data=numpy.array([30, 20, 10, 100], dtype=numpy.int64)
            )
        status, report = compare(uncut, cut, *options)
        assert_status(status, INPUT_ERROR, report, "a pair placed in two forests")
        assert "placed in original forests 10 and 30" in report, f"not named\n{report}"

        status, report = compare(uncut, cut, *cut_inputs(root, pieces=("[20]", "[20]")))
        assert_status(status, INPUT_ERROR, report, "a fresh piece the record does not list")
        assert "ForestID 100 is a fresh piece id" in report, f"not named\n{report}"

        for name in ("uncut", "cut"):
            for path in (root / name).glob("model_*.hdf5"):
                with h5py.File(path, "a") as handle:
                    for snap in [key for key in handle if key.startswith("Snap")]:
                        rows = handle[f"{snap}/Galaxies"][()]
                        del handle[f"{snap}/Galaxies"]
                        kept = [n for n in rows.dtype.names if n != "UniqueCentralGalaxyID"]
                        handle[f"{snap}/Galaxies"] = repack_fields(rows[kept])
        tree_pairing = [o for o in cut_inputs(root) if o != "--match-created-by-lineage"]
        status, report = compare(uncut, cut, *tree_pairing)
        assert_status(status, INPUT_ERROR, report, "records without UniqueCentralGalaxyID")
        assert "carry no ['UniqueCentralGalaxyID']" in report, f"field not named\n{report}"
    print("  ✓ a foreign record, swapped sidecar, unlisted piece or missing central is refused")


def test_cut_report_unplaceable_record_is_an_input_error():
    """A tree id decoding outside its dataset's sidecar exits 2, never 0 or 1."""
    print("Testing an unplaceable record...")
    with scratch() as root:
        options = cut_inputs(root)
        stray = [*CUT_TREE, (401, 401, 5, 5, 0, 9.0)]
        status, report = compare(
            cut_run(root, "uncut", cut=False, tree=stray),
            cut_run(root, "cut", cut=True, tree=stray),
            *options,
        )
        assert_status(status, INPUT_ERROR, report, "record outside the sidecar")
        assert "outside the sidecar's 3 forests" in report, f"not named\n{report}"
    print("  ✓ a record that cannot be placed in a forest is an input error")


# --------------------------------------------------------------------------
# Memory: no comparison copies a run's records
# --------------------------------------------------------------------------


def wide_records(rows, forests, multiplier):
    """One snapshot of `rows` tree galaxies in `forests` equal forests, about 300 B a record.

    A production output snapshot holds hundreds of millions of 264 B records,
    so the record, not the 8 B keys, is what a copy would cost. A production
    multiplier keeps the ids as sparse as a real run's, so the set operations
    take the same path they take on production output.
    """
    payload = [(f"Field{index:02d}", "<f8") for index in range(32)]
    records = numpy.zeros(rows, dtype=PAIRED_DTYPE.descr + payload + [("Pos", "<f4", (3,))])
    rank, forest = numpy.arange(rows) % (rows // forests), numpy.arange(rows) // (rows // forests)
    records["UniqueGalaxyID"] = 1 + rank + multiplier * (forest + 1)
    records["UniqueCentralGalaxyID"] = records["UniqueGalaxyID"]
    records["MostBoundID"] = numpy.arange(rows)[::-1] + 1
    records["SnapNum"] = PAIRED_SNAP
    records["StellarMass"] = numpy.arange(rows) * 0.5
    for name, _ in payload:
        records[name] = numpy.arange(rows) + 0.25
    return records


def peak_traced_bytes(call):
    """The peak bytes traced while ``call(snap)`` compares a second snapshot, stdout discarded.

    The first snapshot is compared first, untimed, so an alternate mode holds its
    id pairs as it does at every later snapshot.
    """
    tracemalloc.start()
    try:
        with contextlib.redirect_stdout(io.StringIO()):
            call(PAIRED_SNAP)
            tracemalloc.reset_peak()
            outcome = call(PAIRED_SNAP + 1)
        return tracemalloc.get_traced_memory()[1], outcome
    finally:
        tracemalloc.stop()


#: The comparator docstring's measured peaks per row, in the order the test checks them.
DOCUMENTED_PEAKS_RE = re.compile(
    r"about (\d+) B per row of one run under the default rule, (\d+) B under the catalogue "
    r"pairing and (\d+) B with the cut report"
)


def test_no_comparison_copies_a_runs_records():
    """The default rule, the catalogue pairing and the cut report each stay below one run's
    records, and near the per-row peak the comparator's docstring states.

    A copy of either run's records (a boolean selection that keeps every row,
    the paired records, or their concatenation) traces at least that run's
    record bytes; positions, keys, masks, the pairing's sorts and one gathered
    field at a time stay below it. Each peak must also lie within 20% of the
    docstring's figure, so neither can drift from the other unnoticed, and each
    comparison must find the identical runs identical, so the bound is not met
    by skipping the work.
    """
    print("Testing that no comparison copies a run's records...")
    documented = DOCUMENTED_PEAKS_RE.search(" ".join(comparator.__doc__.split()))
    assert documented, "the comparator's docstring no longer states its measured peaks"
    rows, forests, multiplier = 200_000, 50, 20_000_000_000
    left, right = wide_records(rows, forests, multiplier), wide_records(rows, forests, multiplier)
    labels = ("left", "right")
    sidecar = numpy.arange(forests, dtype=numpy.int64) + 10
    record = comparator.CutRecord(
        numpy.array([10], dtype=numpy.int64), numpy.array([10], dtype=numpy.int64), forests + 9, {}
    )
    catalogue = comparator.Matching("catalogue", None)
    cases = {
        "the default rule": (comparator.BY_ID, None),
        "the catalogue pairing": (catalogue, None),
        "the cut report": (
            catalogue,
            comparator.CutReport(multiplier, (sidecar, sidecar), record, False),
        ),
    }
    for (what, (matching, cut)), figure in zip(cases.items(), documented.groups()):
        history = comparator.PairHistory() if matching.alternate else None
        peak, outcome = peak_traced_bytes(
            lambda snap, m=matching, c=cut, h=history: comparator.compare_snapshot(
                snap, left, right, labels, 10, m, c, h
            )
        )
        assert outcome.failures == 0 and outcome.tree_rows == rows, f"{what}: {outcome}"
        assert peak < left.nbytes, (
            f"{what} traced {peak} B at its peak, at least one run's {left.nbytes} B of "
            f"records: a copy of the records was made"
        )
        per_row = peak / rows
        assert abs(per_row - int(figure)) <= 0.2 * int(figure), (
            f"{what} peaked at {per_row:.0f} B per row; the comparator's docstring states "
            f"{figure} B"
        )
    print(f"  ✓ every comparison stays below one run's {left.nbytes} B and near its stated peak")


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


def test_partitions_must_be_exactly_the_master_files_links():
    """A partition the master does not link, or a linked one that is missing, exits 2.

    A stale partition left by an earlier run in the same directory would
    otherwise be aggregated as if this run had written it.
    """
    print("Testing the partition set against the master file...")
    with scratch() as root:
        records = make_records([1_000_000_001, 1_000_000_002])
        left, right = build_pair(root, [{0: records}], [{0: records[:1]}, {0: records[1:]}])
        stale = root / "left" / numbered_filename("model", 7)
        with h5py.File(stale, "w") as handle:
            handle.create_dataset("Snap000/Galaxies", data=records[:0])
        status, report = compare(left, right)
        assert_status(status, INPUT_ERROR, report, "a stale partition")
        assert "present but not linked" in report and stale.name in report, report

        stale.unlink()
        (root / "right" / numbered_filename("model", 1)).unlink()
        status, report = compare(left, right)
        assert_status(status, INPUT_ERROR, report, "a missing partition")
        assert "1 linked but missing ['model_001.hdf5']" in report, report
    print("  ✓ the partitions must be exactly the files the master links")


def test_a_master_snapshot_entry_that_is_not_a_group_is_an_input_error():
    """A master whose Snap### entry is a dataset, not a group of partition links, exits 2."""
    print("Testing a malformed master file...")
    with scratch() as root:
        records = make_records([1_000_000_001])
        left, right = build_pair(root, [{0: records}], [{0: records}])
        with h5py.File(root / "left" / "model.hdf5", "r+") as handle:
            del handle["Snap000"]
            handle.create_dataset("Snap000", data=records[:0])
        status, report = compare(left, right)
        assert_status(status, INPUT_ERROR, report, "a master entry that is not a group")
        assert "/Snap000 is not a group of partition links" in report, report
    print("  ✓ a master entry that is not a group is an input error")


def test_an_unexpected_failure_exits_3_with_its_traceback():
    """A failure of the comparator itself is neither a difference (1) nor an input error (2)."""
    print("Testing an unexpected failure...")
    with scratch() as root:
        records = make_records([1_000_000_001])
        left, right = build_pair(root, [{0: records}], [{0: records}])

        def exhausted(*_args, **_kwargs):
            raise MemoryError("no memory left")

        original, comparator.compare_snapshot = comparator.compare_snapshot, exhausted
        try:
            status, report = compare(left, right)
        finally:
            comparator.compare_snapshot = original
        assert_status(status, 3, report, "an unexpected failure")
        assert "Traceback" in report and "MemoryError: no memory left" in report, report
    print("  ✓ an unexpected failure exits 3 and prints its traceback")


def main():
    """Run this file's tests via the shared framework runner."""
    return run_test_suite(
        [
            test_identical_runs_compare_equal,
            test_records_are_aggregated_across_partitions,
            test_perturbed_float_field_is_detected,
            test_perturbation_in_the_last_field_of_a_later_snapshot_is_detected,
            test_dropped_id_is_detected,
            test_zero_id_is_a_failure,
            test_duplicated_id_is_detected_before_anything_else,
            test_one_sided_duplicate_in_an_unshared_snapshot_is_detected,
            test_signed_zero_is_a_difference,
            test_identical_nan_payloads_are_not_a_difference,
            test_differing_nan_payloads_are_a_difference,
            test_snapshot_set_mismatch_is_detected,
            test_schema_mismatch_is_detected,
            test_task_suffixed_partitions_are_aggregated_in_snapshot_task_order,
            test_mixed_suffixed_and_unsuffixed_partitions_are_an_input_error,
            test_compare_created_accepts_equal_created_rows,
            test_compare_created_detects_differing_created_rows,
            test_compare_created_duplicate_created_id_is_detected,
            test_created_id_ordinal_decoding_matches_the_c_encoding,
            test_alternate_pairings_accept_equal_science_with_different_labels,
            test_lineage_mismatch_and_created_field_difference_fail,
            test_duplicate_catalogue_key_fails,
            test_broken_central_mapping_fails,
            test_alternate_pairings_require_each_id_to_keep_its_partner,
            test_alternate_pairings_fail_a_zero_id_and_an_unpaired_tree_row,
            test_lineage_pairing_fails_a_wrong_central_and_a_duplicated_key,
            test_lineage_key_beyond_its_packing_is_an_input_error,
            test_alternate_mode_usage_errors,
            test_cut_report_reports_a_split_forest_difference_and_fails_an_unchanged_one,
            test_cut_report_classifies_a_fresh_piece_record_to_its_original_forest,
            test_cut_report_places_an_inherited_created_record_through_its_central,
            test_cut_report_checks_the_multiplier_against_both_runs,
            test_cut_report_fails_a_record_unmatched_outside_the_split_forests,
            test_cut_report_refuses_inputs_that_do_not_describe_the_runs,
            test_cut_report_unplaceable_record_is_an_input_error,
            test_no_comparison_copies_a_runs_records,
            test_truncated_partition_is_an_input_error,
            test_missing_run_directory_is_an_input_error,
            test_directory_without_partitions_is_an_input_error,
            test_partitions_must_be_exactly_the_master_files_links,
            test_a_master_snapshot_entry_that_is_not_a_group_is_an_input_error,
            test_an_unexpected_failure_exits_3_with_its_traceback,
        ],
        "Cross-format identity comparator (test_compare_cross_format_identity.py)",
    )


if __name__ == "__main__":
    sys.exit(main())
