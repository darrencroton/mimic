#!/usr/bin/env python3
"""
Compare two Mimic HDF5 runs for cross-format galaxy identity.

This is the executable form of the snapshot-driver phase's gate: running the same
simulation data through the tree-ordered driver and through the snapshot-ordered
driver must produce, for every output snapshot, the same set of ``UniqueGalaxyID``
values, and for each of those ids every output field identical **as raw bytes**.

The comparison is bitwise and carries no tolerance of any kind. That is
deliberate and is why the field comparison works on the underlying bytes rather
than on values: ``numpy.allclose`` implements a tolerance, and even ``==`` is not
the required predicate, because it reports NaN != NaN (hiding a changed NaN
payload behind a difference that is always there) and -0.0 == 0.0 (hiding a sign
flip). Comparing bytes decides both correctly.

In this order:

1. **No duplicate ``UniqueGalaxyID`` within either run.** Checked first, across
   *every* output snapshot of *both* runs — including snapshots only one run
   has — because every later step assumes an id identifies one galaxy: a
   duplicated id would otherwise let a set comparison pass while the runs
   disagree about how many galaxies exist, and would make "the record for this
   id" ambiguous. Scanning the shared snapshots only would leave a duplicate in
   a one-sided snapshot unnamed, reported as nothing more than a snapshot-set
   mismatch.
2. **The same field schema** in both runs, then **the same set of output
   snapshots**.
3. Per shared output snapshot: **identical id sets**, reporting the symmetric
   difference with counts, then **byte-identical fields** for every shared id,
   reported per field with a bounded sample of the ids that differ.

Usage:

    compare_cross_format_identity.py <dir/basename> <dir/basename> [options]

where each argument is an output directory joined with the run's
``output_filename`` -- for example ``output/halos-only-micro-uchuu-ascii/halos``.
Galaxies are aggregated across every numbered partition file
``<basename>_<digits>.hdf5`` in ascending numeric order; the master
``<basename>.hdf5`` is ignored, since it only links to those partitions.

Exit status is 0 when the two runs are identical under the rules above and 1 when
they are not (2 for a usage or input error).
"""

import argparse
import re
import sys
from pathlib import Path

import h5py
import numpy

#: Ids/fields listed per failure before the report is truncated. A failing gate
#: needs a usable lead, not the full difference: an entirely different run would
#: otherwise print millions of lines.
DEFAULT_MAX_REPORT = 10

#: The identity field the comparison is keyed on.
ID_FIELD = "UniqueGalaxyID"

SNAP_GROUP_RE = re.compile(r"^Snap(\d+)$")


class ComparisonError(Exception):
    """A run could not be read or is structurally unusable for comparison."""


def schema_signature(dtype):
    """Return the field schema a bitwise field comparison depends on.

    Field names, their order, each field's base type, and each field's shape --
    but not the record's byte offsets or total size. Padding between fields is
    not part of any field and is never compared, so two records whose fields all
    agree describe the same galaxies whether or not the compound type they were
    read through happens to be packed.
    """
    signature = []
    for name in dtype.names:
        field = dtype.fields[name][0]
        if field.subdtype is not None:
            base, shape = field.subdtype
        else:
            base, shape = field, ()
        signature.append((name, base.str, tuple(shape)))
    return tuple(signature)


def partition_files(spec):
    """Return the numbered partition files of one run, in ascending numeric order.

    `spec` is a directory path joined with the run's output basename, the same
    string the run YAML's output_directory and output_filename produce.
    """
    path = Path(spec)
    directory, base = path.parent, path.name
    if not base:
        raise ComparisonError(f"{spec}: expected <directory>/<output_filename>")
    if not directory.is_dir():
        raise ComparisonError(f"{directory}: not a directory")

    pattern = re.compile(r"^" + re.escape(base) + r"_(\d+)\.hdf5$")
    chunks = []
    for entry in directory.iterdir():
        match = pattern.match(entry.name)
        if match:
            chunks.append((int(match.group(1)), entry))
    chunks.sort()
    if not chunks:
        raise ComparisonError(f"{directory}: no partition files {base}_<digits>.hdf5 found")
    return [entry for _, entry in chunks]


def load_run(spec):
    """Aggregate one run's galaxies per output snapshot across all its partitions.

    Returns (snapshot -> structured array, list of partition paths). Partitions
    are concatenated in ascending numeric-suffix order; every partition must
    carry the same Galaxies dtype, since a widened dtype in a later partition
    would silently promote the concatenated array and defeat a bitwise
    comparison.
    """
    files = partition_files(spec)
    per_snap = {}
    signature = None
    signature_source = None

    for path in files:
        with h5py.File(path, "r") as handle:
            for name in handle:
                match = SNAP_GROUP_RE.match(name)
                if match is None:
                    continue
                group = handle[name]
                if "Galaxies" not in group:
                    continue
                snap = int(match.group(1))
                data = group["Galaxies"][...]
                if data.dtype.names is None:
                    raise ComparisonError(f"{path}: {name}/Galaxies is not a record dataset")
                current = schema_signature(data.dtype)
                if signature is None:
                    signature, signature_source = current, path
                elif current != signature:
                    raise ComparisonError(
                        f"{path}: {name}/Galaxies field schema {current} differs from "
                        f"{signature_source}'s {signature}"
                    )
                per_snap.setdefault(snap, []).append(data)

    if signature is None:
        raise ComparisonError(f"{spec}: partitions carry no Snap###/Galaxies datasets")
    if ID_FIELD not in [name for name, _, _ in signature]:
        raise ComparisonError(f"{spec}: Galaxies records carry no {ID_FIELD} field")

    return (
        {snap: numpy.concatenate(parts) for snap, parts in per_snap.items()},
        files,
        signature,
    )


def field_bytes(values):
    """Return one row of raw bytes per record for a single field's values.

    Structured-array padding is never included, because this is applied to one
    field at a time: padding bytes between fields are not part of any field and
    are not written deterministically by either run.
    """
    contiguous = numpy.ascontiguousarray(values)
    return contiguous.view(numpy.uint8).reshape(len(contiguous), -1)


def report_duplicates(label, snap, ids, max_report):
    """Report duplicated ids within one run. Returns the number of duplicated values."""
    unique, counts = numpy.unique(ids, return_counts=True)
    repeated = unique[counts > 1]
    if repeated.size == 0:
        return 0

    total_extra = int(counts[counts > 1].sum() - repeated.size)
    print(
        f"  FAIL Snap{snap:03d}: {label} contains {repeated.size} duplicated {ID_FIELD} "
        f"value(s) ({total_extra} extra record(s) beyond one per id)"
    )
    for value in repeated[:max_report]:
        occurrences = int(counts[unique == value][0])
        print(f"    {ID_FIELD} {int(value)} appears {occurrences} times")
    if repeated.size > max_report:
        print(f"    ... and {repeated.size - max_report} more duplicated id(s)")
    return int(repeated.size)


def report_run_duplicates(label, run, max_report):
    """Scan every output snapshot of one run for duplicated ids.

    Returns the number of snapshots that carry duplicates. Run before anything
    else and over every snapshot the run has, not only the shared ones, so a
    duplicate in a snapshot the other run lacks is still named.
    """
    return sum(
        1 for snap in sorted(run) if report_duplicates(label, snap, run[snap][ID_FIELD], max_report)
    )


def compare_snapshot(snap, left, right, labels, max_report):
    """Compare one output snapshot's shared records. Returns the failures found.

    Precondition: neither run carries duplicated ids — report_run_duplicates()
    has already run over both and the comparison stopped if it found any. That
    is what lets the set difference below assume unique inputs.
    """
    left_label, right_label = labels
    failures = 0

    left_ids = left[ID_FIELD]
    right_ids = right[ID_FIELD]

    # Identical id sets.
    only_left = numpy.setdiff1d(left_ids, right_ids, assume_unique=True)
    only_right = numpy.setdiff1d(right_ids, left_ids, assume_unique=True)
    if only_left.size or only_right.size:
        print(
            f"  FAIL Snap{snap:03d}: {ID_FIELD} sets differ -- "
            f"{only_left.size} only in {left_label} ({left_ids.size} total), "
            f"{only_right.size} only in {right_label} ({right_ids.size} total)"
        )
        for label, values in ((left_label, only_left), (right_label, only_right)):
            if values.size:
                sample = ", ".join(str(int(v)) for v in values[:max_report])
                suffix = "" if values.size <= max_report else f", ... (+{values.size - max_report})"
                print(f"    only in {label}: {sample}{suffix}")
        return failures + 1

    if left_ids.size == 0:
        print(f"  ok   Snap{snap:03d}: both runs are empty")
        return 0

    # Byte-identical fields for every shared id, aligned by id.
    left_order = numpy.argsort(left_ids, kind="stable")
    right_order = numpy.argsort(right_ids, kind="stable")
    aligned_ids = left_ids[left_order]

    differing_fields = []
    for name in left.dtype.names:
        left_field = field_bytes(left[name][left_order])
        right_field = field_bytes(right[name][right_order])
        mismatched = numpy.nonzero((left_field != right_field).any(axis=1))[0]
        if mismatched.size == 0:
            continue
        differing_fields.append((name, mismatched))

    if not differing_fields:
        print(f"  ok   Snap{snap:03d}: {left_ids.size} galaxies, all fields byte-identical")
        return 0

    print(
        f"  FAIL Snap{snap:03d}: {len(differing_fields)} of {len(left.dtype.names)} field(s) "
        f"differ over {left_ids.size} shared id(s)"
    )
    for name, mismatched in differing_fields:
        print(f"    {name}: {mismatched.size} record(s) differ")
        for row in mismatched[:max_report]:
            galaxy_id = int(aligned_ids[row])
            left_value = left[name][left_order][row]
            right_value = right[name][right_order][row]
            print(
                f"      {ID_FIELD} {galaxy_id}: "
                f"{left_label}={left_value!r} {right_label}={right_value!r}"
            )
        if mismatched.size > max_report:
            print(f"      ... and {mismatched.size - max_report} more record(s)")
    return failures + len(differing_fields)


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Bitwise cross-format galaxy identity comparison of two Mimic HDF5 runs.",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    parser.add_argument("left", help="<output directory>/<output_filename> of the first run")
    parser.add_argument("right", help="<output directory>/<output_filename> of the second run")
    parser.add_argument("--left-label", default="left", help="name for the first run in the report")
    parser.add_argument(
        "--right-label", default="right", help="name for the second run in the report"
    )
    parser.add_argument(
        "--max-report",
        type=int,
        default=DEFAULT_MAX_REPORT,
        help="ids or records listed per failure before the report is truncated",
    )
    args = parser.parse_args(argv)

    if args.max_report < 1:
        parser.error("--max-report must be at least 1")

    try:
        left, left_files, left_signature = load_run(args.left)
        right, right_files, right_signature = load_run(args.right)
    except ComparisonError as error:
        print(f"ERROR: {error}", file=sys.stderr)
        return 2

    labels = (args.left_label, args.right_label)
    print(f"{args.left_label}:  {args.left} ({len(left_files)} partition file(s))")
    print(f"{args.right_label}: {args.right} ({len(right_files)} partition file(s))")

    # Duplicate ids first, over every output snapshot of BOTH runs -- including
    # snapshots only one run has. Everything below assumes an id names exactly
    # one galaxy, so a duplicate is reported and the comparison stops here
    # rather than being described later as a set or field difference.
    duplicate_snapshots = report_run_duplicates(
        args.left_label, left, args.max_report
    ) + report_run_duplicates(args.right_label, right, args.max_report)
    if duplicate_snapshots:
        print(
            f"\nFAILED: duplicated {ID_FIELD} values in {duplicate_snapshots} output "
            f"snapshot(s); no further comparison is meaningful"
        )
        return 1

    failures = 0

    if left_signature != right_signature:
        print(
            "  FAIL: the two runs have different Galaxies field schemas; "
            "a bitwise field comparison is not meaningful"
        )
        print(f"    {args.left_label} fields:  {left_signature}")
        print(f"    {args.right_label} fields: {right_signature}")
        return 1

    left_snaps, right_snaps = set(left), set(right)
    if left_snaps != right_snaps:
        print(
            f"  FAIL: output snapshot sets differ -- "
            f"only in {args.left_label}: {sorted(left_snaps - right_snaps)}, "
            f"only in {args.right_label}: {sorted(right_snaps - left_snaps)}"
        )
        failures += 1

    for snap in sorted(left_snaps & right_snaps):
        failures += compare_snapshot(snap, left[snap], right[snap], labels, args.max_report)

    shared = len(left_snaps & right_snaps)
    if failures:
        print(f"\nFAILED: {failures} difference(s) across {shared} shared output snapshot(s)")
        return 1

    total = sum(left[snap].size for snap in sorted(left_snaps))
    print(
        f"\nPASSED: {total} galaxies over {shared} output snapshot(s) are bitwise identical "
        f"in every field, with identical {ID_FIELD} sets and no duplicates"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
