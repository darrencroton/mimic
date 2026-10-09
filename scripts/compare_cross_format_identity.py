#!/usr/bin/env python3
"""
Compare two Mimic HDF5 runs for cross-format galaxy identity.

This is the executable form of the horizontal-driver phase's gate: running the same
simulation data through the vertical driver and through the horizontal
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

Step 3's comparison covers **tree rows only** (``UniqueGalaxyID > 0``) by
default. Records a module created during the run carry strictly negative ids in
a namespace each driver keys differently (its own processing unit and rows per
unit), so they are not expected to match across drivers; they are counted per
run and snapshot and reported separately, never as a difference. They still
take part in step 1: created ids are unique run-wide too. When both runs come
from the same driver -- a serial and a multi-task run of one horizontal
configuration, say -- created ids are expected to match, and
``--compare-created`` includes them in step 3 under the same byte-exact per-id
rule as tree rows.

**Alternate matching modes.** Two datasets of one simulation can enumerate it
differently and still describe the same galaxies; the default rule then cannot
pair their records. Each mode below changes only how records are paired, never
what a pair must agree on:

- ``--match-by-catalogue-id`` pairs tree rows by the catalogue key
  (``MostBoundID``, ``SnapNum``) instead of ``UniqueGalaxyID``, for datasets
  whose forest enumeration differs.
- ``--match-created-by-lineage`` pairs created rows by birth lineage
  (``MostBoundID``, ``SnapNum``, ordinal). A created record is a copy of its
  host row, so the first two are the host's; the ordinal is the host's
  creation count decoded from the id, ``(-id - 1) mod``
  ``MAX_CREATED_RECORDS_PER_HOST``. The rest of a created id encodes the host's
  row in its driver's processing unit, which a reordered dataset moves.

In either mode a key duplicated within one run, a key present in one run only,
and a field difference are each a failure, reported as for tree rows. Paired
records agree byte for byte in every field but two: ``UniqueGalaxyID`` is what
the mode replaces as the key and is not compared, and ``UniqueCentralGalaxyID``,
the one other identity-valued field, is compared through the pairing itself
-- the first run's central must be paired with the second run's central.
``--compare-created`` pairs created rows by their id and cannot be combined
with the lineage mode.

**The cut-difference report.** Cutting a forest into pieces changes the
galaxies of that forest and of no other. ``--multiplier`` with both datasets'
``forests.h5`` sidecars (``--left-sidecar``, ``--right-sidecar``) and the cut
table's JSON record (``--cut-record``) places every record in its original
forest: a tree record's ``ForestIndex`` is decoded from its id
(``UniqueGalaxyID // multiplier - 1``), looked up in its own dataset's sidecar
``ForestID``, and a piece the record lists is mapped to the forest it was cut
from; a created record follows its host, the tree row of its run and snapshot
with its ``MostBoundID``. A record that cannot be placed is an input error.
Records of the forests the record splits are paired by the catalogue key
(``--match-by-catalogue-id`` is required) and **reported, not failed**: per
forest and in total, how many are identical, how many differ and in which
fields, and how many are unmatched. Records of every other forest keep the
strict rule, including an identical ``UniqueGalaxyID`` for tree rows, since a
forest the cut leaves alone keeps its ``ForestIndex``. ``--report-all-forests``
reports every forest without failing instead, for a model whose identity-keyed
choices can move outside the split forests; input errors still fail.

Usage:

    compare_cross_format_identity.py <dir/basename> <dir/basename> [options]

where each argument is an output directory joined with the run's
``output_filename`` -- for example ``output/halos-only-micro-uchuu-ascii/halos``.
Galaxies are aggregated across every numbered partition file
``<basename>_<digits>.hdf5`` -- or, for a multi-task horizontal run,
``<basename>_<digits>_task<digits>.hdf5`` -- in ascending (number, task) order;
the master ``<basename>.hdf5`` is ignored, since it only links to those
partitions. One run's partitions are either all task-suffixed or none of them
are; a mixture is an input error.

Exit status is 0 when the two runs are identical under the rules above and 1 when
they are not (2 for a usage or input error). Unreadable input is an input error,
never a difference: a corrupt or truncated partition reported as a bitwise
difference would send someone hunting a physics bug that does not exist.

Both runs are read one output snapshot at a time rather than held whole. The
comparator exists to compare production runs, and its memory is heaviest exactly
when a large divergence is being diagnosed, which is the worst moment to run out
of it. The cut-difference report adds, for the whole comparison, both sidecars'
``ForestID`` columns (8 B per forest each), the record's piece ids and their
original forests (16 B per piece, plus one column's line and its text, about
26 B per piece, while it is parsed), and its per-forest tallies (about 40 B per reported forest
per snapshot, and 16 B per differing field of each).
"""

import argparse
import contextlib
import json
import re
import sys
from pathlib import Path
from typing import NamedTuple, Optional

import h5py
import numpy

#: Ids/fields listed per failure before the report is truncated. A failing gate
#: needs a usable lead, not the full difference: an entirely different run would
#: otherwise print millions of lines.
DEFAULT_MAX_REPORT = 10

#: The identity field the comparison is keyed on.
ID_FIELD = "UniqueGalaxyID"

#: The other identity-valued output field: the UniqueGalaxyID of the record's
#: FoF central. The alternate modes compare it through the pairing.
CENTRAL_FIELD = "UniqueCentralGalaxyID"

#: The catalogue key of a tree row; a created row's lineage adds its ordinal.
CATALOGUE_FIELDS = ("MostBoundID", "SnapNum")

#: The ordinals each host row owns in the created-record id namespace. Copied
#: from MAX_CREATED_RECORDS_PER_HOST in src/include/constants.h; the encoding
#: it is the radix of is mimic_encode_created_galaxy_id() in
#: src/include/galaxy_id.h, -(1 + ordinal + radix * host_key). Changing it
#: there changes every created id, so it must change here too.
MAX_CREATED_RECORDS_PER_HOST = 1024

#: The two piece columns of a cut table's record the report reads.
RECORD_PIECE_COLUMNS = ("id", "forest_id")

SNAP_GROUP_RE = re.compile(r"^Snap(\d+)$")


class ComparisonError(Exception):
    """A run could not be read or is structurally unusable for comparison."""


class Matching(NamedTuple):
    """How records are paired: ``tree`` is "id" or "catalogue"; ``created`` is
    None (not compared), "id" or "lineage"."""

    tree: str
    created: Optional[str]

    @property
    def alternate(self):
        """True when any record is paired other than by its UniqueGalaxyID."""
        return self.tree == "catalogue" or self.created == "lineage"

    def describe(self):
        tree = (
            f"tree rows by {ID_FIELD}"
            if self.tree == "id"
            else f"tree rows by ({', '.join(CATALOGUE_FIELDS)})"
        )
        created = {
            None: "created rows not compared",
            "id": f"created rows by {ID_FIELD}",
            "lineage": f"created rows by lineage ({', '.join(CATALOGUE_FIELDS)}, ordinal)",
        }[self.created]
        return f"{tree}, {created}"


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
    """Return the numbered partition files of one run, in ascending (number, task) order.

    `spec` is a directory path joined with the run's output basename, the same
    string the run YAML's output_directory and output_filename produce. A
    multi-task horizontal run writes ``<base>_<snap>_task<task>.hdf5`` and every
    other run ``<base>_<number>.hdf5``; one run never writes both, so a directory
    holding both is stale or mixed output and an input error.
    """
    path = Path(spec)
    directory, base = path.parent, path.name
    if not base:
        raise ComparisonError(f"{spec}: expected <directory>/<output_filename>")
    if not directory.is_dir():
        raise ComparisonError(f"{directory}: not a directory")

    pattern = re.compile(r"^" + re.escape(base) + r"_(\d+)(?:_task(\d+))?\.hdf5$")
    chunks = []
    for entry in directory.iterdir():
        match = pattern.match(entry.name)
        if match:
            task = -1 if match.group(2) is None else int(match.group(2))
            chunks.append((int(match.group(1)), task, entry))
    chunks.sort()
    if not chunks:
        raise ComparisonError(
            f"{directory}: no partition files {base}_<digits>.hdf5 or "
            f"{base}_<digits>_task<digits>.hdf5 found"
        )
    suffixed = sum(1 for _, task, _ in chunks if task >= 0)
    if 0 < suffixed < len(chunks):
        raise ComparisonError(
            f"{directory}: mixes {suffixed} task-suffixed partition file(s) "
            f"{base}_<digits>_task<digits>.hdf5 with {len(chunks) - suffixed} unsuffixed "
            f"{base}_<digits>.hdf5; one run writes only one kind"
        )
    return [entry for _, _, entry in chunks]


@contextlib.contextmanager
def open_partition(path, snap=None):
    """Open one partition file, turning any I/O failure into a ComparisonError.

    h5py raises OSError for a file that is missing, truncated, not HDF5, or
    corrupt mid-dataset, and dataset reads inside the block raise it too. Left
    uncaught that would surface as a traceback and the exit status reserved for
    "the two runs differ". Unreadable input is an input error, so it is named
    with its path (and snapshot, where the caller knows one) and reported as one.
    """
    where = str(path) if snap is None else f"{path} (Snap{snap:03d})"
    try:
        with h5py.File(path, "r") as handle:
            yield handle
    except OSError as error:
        raise ComparisonError(f"{where}: cannot be read ({error})") from error


class RunIndex:
    """Where one run's galaxies live, without holding any of them.

    Records only the partition files, the field schema, and how many rows each
    partition contributes to each output snapshot. Records are read per snapshot,
    on demand, so neither run is ever held whole.
    """

    def __init__(self, files, signature, dtype, layout):
        self.files = files
        self.signature = signature
        self.dtype = dtype
        #: snapshot -> [(path, rows)], partitions in ascending (number, task) order
        self.layout = layout

    @property
    def snapshots(self):
        return set(self.layout)


def scan_run(spec):
    """Index one run's partitions: schema and per-snapshot row counts, no payload.

    Dataset dtypes and shapes are HDF5 metadata, so the whole scan reads no
    galaxy records at all. Every partition must carry the same Galaxies schema,
    since a widened dtype in a later partition would silently promote the
    concatenated array and defeat a bitwise comparison.
    """
    files = partition_files(spec)
    layout = {}
    signature = None
    signature_source = None
    dtype = None

    for path in files:
        with open_partition(path) as handle:
            for name in handle:
                match = SNAP_GROUP_RE.match(name)
                if match is None:
                    continue
                group = handle[name]
                if "Galaxies" not in group:
                    continue
                snap = int(match.group(1))
                dataset = group["Galaxies"]
                if dataset.dtype.names is None:
                    raise ComparisonError(f"{path}: {name}/Galaxies is not a record dataset")
                current = schema_signature(dataset.dtype)
                if signature is None:
                    signature, signature_source, dtype = current, path, dataset.dtype
                elif current != signature:
                    raise ComparisonError(
                        f"{path}: {name}/Galaxies field schema {current} differs from "
                        f"{signature_source}'s {signature}"
                    )
                layout.setdefault(snap, []).append((path, dataset.shape[0]))

    if signature is None:
        raise ComparisonError(f"{spec}: partitions carry no Snap###/Galaxies datasets")
    if ID_FIELD not in [name for name, _, _ in signature]:
        raise ComparisonError(f"{spec}: Galaxies records carry no {ID_FIELD} field")

    return RunIndex(files, signature, dtype, layout)


def read_snapshot(index, snap, field=None):
    """Read one output snapshot's records, aggregated across its partitions.

    The destination is allocated once from the row counts the scan already
    recorded, and whole records are read straight into their slice of it with
    read_direct, so no partition array outlives the read that produced it and no
    concatenated second copy of the snapshot ever exists. That matters most
    where it is largest: a horizontal run holds one snapshot's whole
    record set in a single partition file, and would otherwise hold that
    snapshot twice at the moment it is read. (Such a run writes one
    partition file per requested output snapshot rather than one for the whole
    run; the memory argument is unchanged, because it was always about one
    snapshot's records landing in one file.)

    With `field`, only that column is read -- which is how the duplicate pass
    looks at every snapshot of both runs for the price of one int64 per galaxy
    instead of a whole record. h5py has no read_direct for a single field of a
    compound dataset, so that path takes the copy; it is 8 bytes per galaxy
    against a 160-byte record, and it is released before the next snapshot.
    """
    parts = index.layout.get(snap, ())
    total = sum(rows for _, rows in parts)
    dtype = index.dtype if field is None else index.dtype[field]
    out = numpy.empty(total, dtype=dtype)

    offset = 0
    for path, rows in parts:
        if rows == 0:
            continue
        with open_partition(path, snap) as handle:
            dataset = handle[f"Snap{snap:03d}/Galaxies"]
            try:
                if field is None:
                    dataset.read_direct(out, numpy.s_[0:rows], numpy.s_[offset : offset + rows])
                else:
                    out[offset : offset + rows] = dataset[field]
            except (TypeError, ValueError) as error:
                # read_direct needs the destination to match the stored compound
                # type exactly, and the destination was built from the first
                # partition's. Partitions of one run that disagree on their
                # record layout are unusable input, not a difference.
                raise ComparisonError(
                    f"{path} (Snap{snap:03d}): Galaxies records cannot be read into this "
                    f"run's record layout ({error})"
                ) from error
        offset += rows

    return out


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


def report_run_duplicates(label, index, max_report):
    """Scan every output snapshot of one run for duplicated ids.

    Returns the number of snapshots that carry duplicates. Run before anything
    else and over every snapshot the run has, not only the shared ones, so a
    duplicate in a snapshot the other run lacks is still named. Only the id
    column is read, and only one snapshot's worth at a time.
    """
    duplicated = 0
    for snap in sorted(index.snapshots):
        ids = read_snapshot(index, snap, field=ID_FIELD)
        if report_duplicates(label, snap, ids, max_report):
            duplicated += 1
        del ids
    return duplicated


def compared_rows(records, compare_created=False):
    """Split one snapshot's records into compared rows and the tree, created and zero-id counts.

    Tree rows carry positive ids and records created during the run strictly
    negative ones; no encoder produces 0, so a zero id is a defect the caller
    reports rather than a row to skip. The compared rows are the tree rows, plus
    the created rows when `compare_created` is set. The counts are of ids
    greater than, less than and equal to zero respectively.
    """
    ids = records[ID_FIELD]
    compared = ids != 0 if compare_created else ids > 0
    return (
        records[compared],
        int(numpy.count_nonzero(ids > 0)),
        int(numpy.count_nonzero(ids < 0)),
        int(numpy.count_nonzero(ids == 0)),
    )


def report_created(snap, created, labels):
    """Report one snapshot's created-row counts, which are never compared."""
    if any(created):
        counts = ", ".join(f"{label} {count}" for label, count in zip(labels, created))
        print(
            f"  note Snap{snap:03d}: created rows ({ID_FIELD} < 0) are driver-specific and "
            f"not compared -- {counts}"
        )


class SnapshotResult(NamedTuple):
    """The outcome of comparing one output snapshot.

    `failures` is the number of differences found. `tree_rows` is the number of
    tree rows (``UniqueGalaxyID > 0``) compared -- the first run's count by
    default, the paired count in an alternate mode -- and `created_rows` each
    run's count of created rows (``UniqueGalaxyID < 0``) as ``(first, second)``;
    zero ids are in neither, so the caller's summary never counts a defective row
    as created. `created_compared` is how many created rows were compared.
    """

    failures: int
    tree_rows: int
    created_rows: tuple
    created_compared: int = 0


def compare_snapshot(snap, left, right, labels, max_report, compare_created=False):
    """Compare one output snapshot's shared rows. Returns a SnapshotResult.

    Records created during a run (negative ids) are split off first and
    reported by count, unless `compare_created` is set, in which case they are
    compared exactly like tree rows; everything below sees the compared rows
    only.

    Precondition: neither run carries duplicated ids — report_run_duplicates()
    has already run over both and the comparison stopped if it found any. That
    is what lets the set difference below assume unique inputs.
    """
    left_label, right_label = labels
    failures = 0

    left, left_tree, left_created, left_zero = compared_rows(left, compare_created)
    right, _, right_created, right_zero = compared_rows(right, compare_created)
    created = (left_created, right_created)

    def result(failures):
        return SnapshotResult(failures, left_tree, created, left_created if compare_created else 0)

    if not compare_created:
        report_created(snap, created, labels)
    if left_zero or right_zero:
        print(
            f"  FAIL Snap{snap:03d}: {ID_FIELD} 0 is neither a tree id nor a created id -- "
            f"{left_label} {left_zero}, {right_label} {right_zero} row(s)"
        )
        failures += 1

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
        return result(failures + 1)

    if left_ids.size == 0:
        print(f"  ok   Snap{snap:03d}: both runs are empty")
        return result(failures)

    # Byte-identical fields for every shared id, aligned by id.
    left_order = numpy.argsort(left_ids, kind="stable")
    right_order = numpy.argsort(right_ids, kind="stable")

    #: [(field, total mismatches, bounded sample of aligned row indices)]. Only
    #: the count and the sample are kept: on a large divergence the full
    #: mismatch index array is the same order of magnitude as the data itself,
    #: and it is retained for every differing field at once, which is the worst
    #: possible moment to hold it.
    differing_fields = []
    for name in left.dtype.names:
        left_field = field_bytes(left[name][left_order])
        right_field = field_bytes(right[name][right_order])
        mismatched = numpy.nonzero((left_field != right_field).any(axis=1))[0]
        del left_field, right_field
        if mismatched.size == 0:
            continue
        differing_fields.append((name, int(mismatched.size), mismatched[:max_report].copy()))
        del mismatched

    if not differing_fields:
        print(
            f"  ok   Snap{snap:03d}: {left_ids.size} galaxies, "
            f"all {len(left.dtype.names)} fields byte-identical"
        )
        return result(failures)

    print(
        f"  FAIL Snap{snap:03d}: {len(differing_fields)} of {len(left.dtype.names)} field(s) "
        f"differ over {left_ids.size} shared id(s)"
    )
    for name, total, sample in differing_fields:
        print(f"    {name}: {total} record(s) differ")
        for row in sample:
            # Index scalar-wise through the sort permutation. Sorting the whole
            # field again per reported record would rebuild a full copy of the
            # column for each of at most max_report samples.
            galaxy_id = int(left_ids[left_order[row]])
            left_value = left[name][left_order[row]]
            right_value = right[name][right_order[row]]
            print(
                f"      {ID_FIELD} {galaxy_id}: "
                f"{left_label}={left_value!r} {right_label}={right_value!r}"
            )
        if total > max_report:
            print(f"      ... and {total - max_report} more record(s)")
    return result(failures + len(differing_fields))


# --------------------------------------------------------------------------
# Alternate matching modes
# --------------------------------------------------------------------------


def created_ordinals(ids):
    """Each created id's host creation ordinal, ``(-id - 1) mod`` the radix.

    ``-(id + 1)`` is formed rather than ``-id - 1``, as the C decoder does, so
    no step overflows even for the most negative int64.
    """
    return numpy.negative(ids + 1) % MAX_CREATED_RECORDS_PER_HOST


def pairing_keys(records, kind, matching):
    """Each record's pairing key under `matching`, as a structured array.

    `kind` is "tree" or "created". A record paired by id is keyed by its
    ``UniqueGalaxyID``; a tree row paired by catalogue by (``MostBoundID``,
    ``SnapNum``); a created row paired by lineage by those plus its ordinal.
    """
    mode = matching.tree if kind == "tree" else matching.created
    if mode == "id":
        keys = numpy.empty(len(records), dtype=[(ID_FIELD, "<i8")])
        keys[ID_FIELD] = records[ID_FIELD]
        return keys
    names = [*CATALOGUE_FIELDS, *(["ordinal"] if kind == "created" else [])]
    keys = numpy.empty(len(records), dtype=[(name, "<i8") for name in names])
    for name in CATALOGUE_FIELDS:
        keys[name] = records[name]
    if kind == "created":
        keys["ordinal"] = created_ordinals(records[ID_FIELD])
    return keys


def format_key(key):
    """One structured key as ``(name=value, ...)``."""
    return "(" + ", ".join(f"{name}={int(key[name])}" for name in key.dtype.names) + ")"


def report_duplicate_keys(label, snap, kind, keys, max_report):
    """Report pairing keys duplicated within one run. Returns the number of duplicated keys."""
    unique, counts = numpy.unique(keys, return_counts=True)
    repeated = unique[counts > 1]
    if repeated.size == 0:
        return 0
    total_extra = int(counts[counts > 1].sum() - repeated.size)
    print(
        f"  FAIL Snap{snap:03d}: {label} contains {repeated.size} duplicated {kind}-row "
        f"key(s) ({total_extra} extra record(s) beyond one per key)"
    )
    for key, count in zip(repeated[:max_report], counts[counts > 1][:max_report]):
        print(f"    {format_key(key)} appears {int(count)} times")
    if repeated.size > max_report:
        print(f"    ... and {repeated.size - max_report} more duplicated key(s)")
    return int(repeated.size)


def pair_keys(left_keys, right_keys):
    """Pair two runs' keys, each unique within its run.

    Returns (left_index, right_index, only_left, only_right): the paired
    positions in each run, in ascending key order, and the positions of each
    run's unpaired keys. The keys are labelled through one sort of both runs'
    keys together, so composite keys pair exactly as their fields do.
    """
    labels = numpy.unique(numpy.concatenate([left_keys, right_keys]), return_inverse=True)[1]
    labels = labels.reshape(-1)
    left_labels, right_labels = labels[: len(left_keys)], labels[len(left_keys) :]
    _, left_index, right_index = numpy.intersect1d(
        left_labels, right_labels, assume_unique=True, return_indices=True
    )
    unpaired = []
    for size, index in ((len(left_keys), left_index), (len(right_keys), right_index)):
        mask = numpy.ones(size, dtype=bool)
        mask[index] = False
        unpaired.append(numpy.flatnonzero(mask))
    return left_index, right_index, unpaired[0], unpaired[1]


def central_mapping(left, right):
    """Whether each pair's first-run central maps through the pairing to its second-run central.

    The pairing maps every paired first-run id to its second-run id. Returns
    (mismatched, found, mapped) per pair: the failure mask, whether the first
    run's central is a paired record at all, and the second-run id it maps to.
    """
    left_ids = left[ID_FIELD]
    order = numpy.argsort(left_ids, kind="stable")
    sorted_ids = left_ids[order]
    centrals = left[CENTRAL_FIELD]
    position = numpy.minimum(numpy.searchsorted(sorted_ids, centrals), max(len(left) - 1, 0))
    found = sorted_ids[position] == centrals
    mapped = numpy.where(found, right[ID_FIELD][order][position], 0)
    return ~found | (mapped != right[CENTRAL_FIELD]), found, mapped


def report_unpaired(snap, kind, labels, sides, max_report):
    """Report keys present in one run only. `sides` holds each run's (keys, ids, total)."""
    (left_keys, left_ids, left_total), (right_keys, right_ids, right_total) = sides
    print(
        f"  FAIL Snap{snap:03d}: {kind}-row key sets differ -- "
        f"{left_keys.size} only in {labels[0]} ({left_total} total), "
        f"{right_keys.size} only in {labels[1]} ({right_total} total)"
    )
    for label, keys, ids in ((labels[0], left_keys, left_ids), (labels[1], right_keys, right_ids)):
        for key, galaxy_id in zip(keys[:max_report], ids[:max_report]):
            print(f"    only in {label}: {format_key(key)} ({ID_FIELD} {int(galaxy_id)})")
        if keys.size > max_report:
            print(f"    only in {label}: ... (+{keys.size - max_report})")


# --------------------------------------------------------------------------
# The cut-difference report
# --------------------------------------------------------------------------


class CutRecord(NamedTuple):
    """What the report needs from a cut table's record: every piece of every
    split forest (ascending piece id) with the forest it was cut from, and the
    floor at which fresh piece ids start."""

    piece_ids: numpy.ndarray
    forest_ids: numpy.ndarray
    fresh_id_floor: int

    @property
    def split(self):
        """The split original forests, ascending."""
        return numpy.unique(self.forest_ids)


def read_cut_record(path):
    """Read a cut table's record: its fresh-id floor and two piece columns.

    The record is written one header entry per line, then one line per piece
    column, so the two columns needed are parsed line by line rather than
    loading the whole document: a whole-simulation record holds six columns of
    tens of millions of pieces. Raises ComparisonError for anything else.
    """
    floor = None
    columns = {}
    try:
        with open(path) as handle:
            for line in handle:
                # Entries are identified by their start; a column line is
                # sliced once, never stripped whole.
                head = line[:80].lstrip()
                if head.startswith('"fresh_id_floor":'):
                    floor = json.loads(line.split(":", 1)[1].strip().rstrip(","))
                    continue
                for name in RECORD_PIECE_COLUMNS:
                    prefix = f'"{name}": ['
                    if head.startswith(prefix):
                        text = line[line.index(prefix) + len(prefix) : line.rindex("]")]
                        values = numpy.fromstring(text, dtype=numpy.int64, sep=",")
                        if values.size != (text.count(",") + 1 if text else 0):
                            raise ComparisonError(
                                f"{path}: pieces.{name} is not a list of integers"
                            )
                        columns[name] = values
    except (OSError, UnicodeDecodeError, ValueError) as error:
        raise ComparisonError(f"{path}: not a readable cut table record ({error})") from error

    missing = [name for name in RECORD_PIECE_COLUMNS if name not in columns]
    if not isinstance(floor, int) or missing:
        raise ComparisonError(
            f"{path}: not a cut table record (fresh_id_floor {floor!r}, missing piece "
            f"columns {missing})"
        )
    piece_ids, forest_ids = columns["id"], columns["forest_id"]
    if piece_ids.size != forest_ids.size:
        raise ComparisonError(
            f"{path}: {piece_ids.size} piece ids but {forest_ids.size} original forest ids"
        )
    if piece_ids.size and numpy.any(piece_ids[1:] <= piece_ids[:-1]):
        raise ComparisonError(f"{path}: piece ids are not strictly ascending")
    # A piece either keeps its forest's id or carries a fresh one.
    invalid = (piece_ids != forest_ids) & (piece_ids < floor)
    if invalid.any():
        raise ComparisonError(
            f"{path}: piece {int(piece_ids[invalid][0])} of forest "
            f"{int(forest_ids[invalid][0])} neither keeps the forest's id nor is fresh "
            f"(at or above {floor})"
        )
    return CutRecord(piece_ids, forest_ids, floor)


def read_sidecar_forest_ids(path):
    """A dataset sidecar's ForestID column; row i is the forest of ForestIndex i."""
    try:
        with h5py.File(path, "r") as handle:
            if "ForestID" not in handle:
                raise ComparisonError(f"{path}: no ForestID dataset")
            return numpy.asarray(handle["ForestID"][()], dtype=numpy.int64)
    except OSError as error:
        raise ComparisonError(f"{path}: cannot be read ({error})") from error


class CutReport:
    """Places records in their original forests and tallies the reported ones.

    `sidecars` holds each run's sidecar ForestID column, in (first, second)
    order. Tallies are kept per snapshot as (forests, counts) pairs and summed
    once at the end.
    """

    COLUMNS = ("identical", "differing", "only_left", "only_right")

    def __init__(self, multiplier, sidecars, record, report_all):
        self.multiplier = multiplier
        self.sidecars = sidecars
        self.record = record
        self.split = record.split
        self.report_all = report_all
        self.tallies = {column: [] for column in self.COLUMNS}
        self.field_tallies = {}

    def original(self, forest_ids, where):
        """Map dataset forest ids to the forests they were cut from (unlisted ids are their own)."""
        record = self.record
        listed = numpy.zeros(forest_ids.shape, dtype=bool)
        mapped = forest_ids
        if record.piece_ids.size:
            position = numpy.minimum(
                numpy.searchsorted(record.piece_ids, forest_ids), record.piece_ids.size - 1
            )
            listed = record.piece_ids[position] == forest_ids
            mapped = numpy.where(listed, record.forest_ids[position], forest_ids)
        unlisted_fresh = ~listed & (forest_ids >= record.fresh_id_floor)
        if unlisted_fresh.any():
            raise ComparisonError(
                f"{where}: ForestID {int(forest_ids[unlisted_fresh][0])} is a fresh piece id "
                f"(at or above {record.fresh_id_floor}) that the cut record does not list"
            )
        return mapped

    def place(self, side, tree, created, where):
        """Original forests of one run's tree rows and created rows in one snapshot."""
        sidecar = self.sidecars[side]
        ids = tree[ID_FIELD]
        forest_index = ids // self.multiplier - 1
        outside = (forest_index < 0) | (forest_index >= sidecar.size)
        if outside.any():
            raise ComparisonError(
                f"{where}: {ID_FIELD} {int(ids[outside][0])} decodes to ForestIndex "
                f"{int(forest_index[outside][0])} under multiplier {self.multiplier}, outside "
                f"the sidecar's {sidecar.size} forests"
            )
        tree_forests = self.original(sidecar[forest_index], where)

        # A created record follows its host: the tree row with its MostBoundID.
        hosts = tree["MostBoundID"]
        order = numpy.argsort(hosts, kind="stable")
        wanted = created["MostBoundID"]
        position = numpy.minimum(numpy.searchsorted(hosts[order], wanted), max(len(hosts) - 1, 0))
        found = hosts[order][position] == wanted if len(hosts) else numpy.zeros(len(wanted), bool)
        if not found.all():
            missing = ~found
            raise ComparisonError(
                f"{where}: created record {ID_FIELD} {int(created[ID_FIELD][missing][0])} has no "
                f"host: no tree row carries its MostBoundID {int(wanted[missing][0])}"
            )
        created_forests = tree_forests[order][position] if len(hosts) else wanted[:0]
        return tree_forests, created_forests

    def reported(self, forests):
        """Which records are reported rather than held to the strict rule."""
        if self.report_all:
            return numpy.ones(forests.shape, dtype=bool)
        return numpy.isin(forests, self.split)

    def add(self, column, forests):
        if forests.size:
            self.tallies[column].append(numpy.unique(forests, return_counts=True))

    def add_field(self, field, forests):
        if forests.size:
            self.field_tallies.setdefault(field, []).append(
                numpy.unique(forests, return_counts=True)
            )

    def table(self):
        """(forests, {column: counts}, {field: counts}) summed over every snapshot."""
        parts = [part for column in self.COLUMNS for part in self.tallies[column]]
        forests = numpy.unique(
            numpy.concatenate([f for f, _ in parts]) if parts else numpy.zeros(0, numpy.int64)
        )

        def summed(pieces):
            total = numpy.zeros(forests.size, dtype=numpy.int64)
            for keys, counts in pieces:
                numpy.add.at(total, numpy.searchsorted(forests, keys), counts)
            return total

        columns = {column: summed(self.tallies[column]) for column in self.COLUMNS}
        fields = {field: summed(pieces) for field, pieces in self.field_tallies.items()}
        return forests, columns, fields

    def print_report(self, labels):
        """The per-forest and total report, every forest with a difference listed."""
        forests, columns, fields = self.table()
        names = {"only_left": f"only in {labels[0]}", "only_right": f"only in {labels[1]}"}
        scope = "every forest" if self.report_all else f"the {self.split.size} split forest(s)"
        print(f"\nCut-difference report ({scope}; reported, not failed):")
        changed = (columns["differing"] + columns["only_left"] + columns["only_right"]) > 0
        for position in numpy.flatnonzero(changed):
            differing = ", ".join(
                f"{field} {int(counts[position])}"
                for field, counts in fields.items()
                if counts[position]
            )
            print(
                f"  forest {int(forests[position])}: "
                f"{int(columns['identical'][position])} identical, "
                f"{int(columns['differing'][position])} differing"
                + (f" ({differing})" if differing else "")
                + "".join(f", {int(columns[c][position])} {names[c]}" for c in names)
            )
        groups = [("split forests", numpy.isin(forests, self.split))]
        if self.report_all:
            groups.append(("other forests", ~groups[0][1]))
        for group, mask in groups:
            totals = ", ".join(
                f"{int(columns[c][mask].sum())} {names.get(c, c)}" for c in self.COLUMNS
            )
            per_field = ", ".join(
                f"{field} {int(counts[mask].sum())}"
                for field, counts in fields.items()
                if counts[mask].sum()
            )
            print(
                f"  total over {group}: {int(mask.sum())} with records, "
                f"{int((mask & changed).sum())} with a difference; {totals}"
                + (f"; differing per field: {per_field}" if per_field else "")
            )


def compare_snapshot_matched(snap, left, right, labels, max_report, matching, cut=None):
    """Compare one output snapshot under an alternate mode. Returns a SnapshotResult.

    Tree rows, then created rows unless they are not compared, are paired by
    their mode's key (``pairing_keys``). A key duplicated within one run fails
    its kind of row; an unpaired key fails; every pair is then compared field
    by field: bytes for every field, except that ``UniqueGalaxyID`` is the key
    and ``UniqueCentralGalaxyID`` is compared through the pairing
    (``central_mapping``).

    With `cut`, every record is first placed in its original forest. Records
    of a reported forest are tallied instead of failing, and records of every
    other forest are held to the strict rule, which for tree rows includes an
    identical ``UniqueGalaxyID``. A pair placed in two different forests means
    the sidecars or record do not describe these runs, an input error.
    """
    left_label, right_label = labels
    failures = 0
    left_all, right_all = left[ID_FIELD], right[ID_FIELD]
    created = (int(numpy.count_nonzero(left_all < 0)), int(numpy.count_nonzero(right_all < 0)))
    zeros = (int(numpy.count_nonzero(left_all == 0)), int(numpy.count_nonzero(right_all == 0)))
    if any(zeros):
        print(
            f"  FAIL Snap{snap:03d}: {ID_FIELD} 0 is neither a tree id nor a created id -- "
            f"{left_label} {zeros[0]}, {right_label} {zeros[1]} row(s)"
        )
        failures += 1
    if matching.created is None:
        report_created(snap, created, labels)

    runs = []
    for side, (records, ids) in enumerate(((left, left_all), (right, right_all))):
        tree, made = records[ids > 0], records[ids < 0]
        placed = (None, None)
        if cut is not None:
            placed = cut.place(side, tree, made, f"{labels[side]} Snap{snap:03d}")
        runs.append({"tree": (tree, placed[0]), "created": (made, placed[1])})

    kinds = ["tree", *(["created"] if matching.created is not None else [])]
    paired = []  # (kind, left rows, right rows, forests or None)
    for kind in kinds:
        (left_rows, left_forests), (right_rows, right_forests) = runs[0][kind], runs[1][kind]
        left_keys = pairing_keys(left_rows, kind, matching)
        right_keys = pairing_keys(right_rows, kind, matching)
        if report_duplicate_keys(left_label, snap, kind, left_keys, max_report) + (
            report_duplicate_keys(right_label, snap, kind, right_keys, max_report)
        ):
            failures += 1
            continue
        left_index, right_index, only_left, only_right = pair_keys(left_keys, right_keys)

        if cut is not None:
            strict = []
            for column, only, forests in (
                ("only_left", only_left, left_forests),
                ("only_right", only_right, right_forests),
            ):
                reported = cut.reported(forests[only])
                cut.add(column, forests[only][reported])
                strict.append(only[~reported])
            only_left, only_right = strict
        if only_left.size or only_right.size:
            report_unpaired(
                snap,
                kind,
                labels,
                (
                    (left_keys[only_left], left_rows[ID_FIELD][only_left], len(left_rows)),
                    (right_keys[only_right], right_rows[ID_FIELD][only_right], len(right_rows)),
                ),
                max_report,
            )
            failures += 1

        forests = None
        if cut is not None:
            forests = left_forests[left_index]
            moved = forests != right_forests[right_index]
            if moved.any():
                raise ComparisonError(
                    f"Snap{snap:03d}: paired {kind} records {ID_FIELD} "
                    f"{int(left_rows[ID_FIELD][left_index][moved][0])} and "
                    f"{int(right_rows[ID_FIELD][right_index][moved][0])} are placed in original "
                    f"forests {int(forests[moved][0])} and "
                    f"{int(right_forests[right_index][moved][0])}; the sidecars or the cut "
                    f"record do not describe these runs"
                )
        paired.append((kind, left_rows[left_index], right_rows[right_index], forests))

    counts = {kind: len(rows) for kind, rows, _, _ in paired}
    tree_pairs, created_pairs = counts.get("tree", 0), counts.get("created", 0)

    def result(failures):
        return SnapshotResult(failures, tree_pairs, created, created_pairs)

    if not paired or tree_pairs + created_pairs == 0:
        if not failures:
            print(f"  ok   Snap{snap:03d}: both runs are empty")
        return result(failures)

    pair_left = numpy.concatenate([rows for _, rows, _, _ in paired])
    pair_right = numpy.concatenate([rows for _, _, rows, _ in paired])
    is_tree = numpy.concatenate(
        [numpy.full(len(rows), kind == "tree") for kind, rows, _, _ in paired]
    )
    reported = numpy.zeros(len(pair_left), dtype=bool)
    forests = None
    if cut is not None:
        forests = numpy.concatenate([f for _, _, _, f in paired])
        reported = cut.reported(forests)
    strict = ~reported

    any_differs = numpy.zeros(len(pair_left), dtype=bool)
    differing_fields = []  # (field, total strict mismatches, bounded sample of pair rows)
    central = None
    for name in pair_left.dtype.names:
        if name == ID_FIELD:
            if cut is None:
                continue
            # A forest the cut leaves alone keeps its ForestIndex, so its tree
            # rows keep their ids; created ids move with their hosts' rows.
            mismatched = is_tree & (pair_left[ID_FIELD] != pair_right[ID_FIELD])
        elif name == CENTRAL_FIELD:
            mismatched, found, mapped = central_mapping(pair_left, pair_right)
            central = (found, mapped)
        else:
            left_bytes = field_bytes(pair_left[name])
            right_bytes = field_bytes(pair_right[name])
            mismatched = (left_bytes != right_bytes).any(axis=1)
            del left_bytes, right_bytes
        if cut is not None and name != ID_FIELD:
            cut.add_field(name, forests[mismatched & reported])
            any_differs |= mismatched & reported
        rows = numpy.flatnonzero(mismatched & strict)
        if rows.size:
            differing_fields.append((name, int(rows.size), rows[:max_report]))

    if cut is not None:
        cut.add("identical", forests[reported & ~any_differs])
        cut.add("differing", forests[reported & any_differs])

    nfields = len(pair_left.dtype.names)
    held = int(strict.sum())
    paired_note = f"{tree_pairs} tree and {created_pairs} created record(s) paired"
    if cut is not None:
        paired_note += (
            f", {int(reported.sum())} in reported forests ({int(any_differs.sum())} differing)"
        )
    if not differing_fields:
        print(
            f"  ok   Snap{snap:03d}: {paired_note}; the {held} held to the strict rule are "
            f"identical in all {nfields} fields under the pairing"
        )
        return result(failures)

    print(
        f"  FAIL Snap{snap:03d}: {paired_note}; {len(differing_fields)} of {nfields} field(s) "
        f"differ over the {held} held to the strict rule"
    )
    for name, total, sample in differing_fields:
        print(f"    {name}: {total} record(s) differ")
        for row in sample:
            pair = (
                f"{ID_FIELD} {int(pair_left[ID_FIELD][row])} <-> {int(pair_right[ID_FIELD][row])}"
            )
            if name == CENTRAL_FIELD:
                found, mapped = central
                left_central = int(pair_left[CENTRAL_FIELD][row])
                maps = f"maps to {int(mapped[row])}" if found[row] else "is paired with no record"
                print(
                    f"      {pair}: {left_label} central {left_central} {maps}, "
                    f"{right_label} central {int(pair_right[CENTRAL_FIELD][row])}"
                )
            else:
                print(
                    f"      {pair}: {left_label}={pair_left[name][row]!r} "
                    f"{right_label}={pair_right[name][row]!r}"
                )
        if total > max_report:
            print(f"      ... and {total - max_report} more record(s)")
    return result(failures + len(differing_fields))


def passed_line(total, shared, nfields, matching, created_compared, cut):
    """The summary of a passing comparison: what was compared, how, and the created rows."""
    how = f"{matching.describe()}; {created_compared} created row(s) compared"
    if not matching.alternate:
        return (
            f"PASSED: {total} galaxies over {shared} output snapshot(s) are bitwise identical "
            f"in all {nfields} field(s), with identical {ID_FIELD} sets and no duplicates "
            f"({how})"
        )
    comparator = " and ".join(
        [
            *(["catalogue-key"] if matching.tree == "catalogue" else []),
            *(["lineage"] if matching.created == "lineage" else []),
        ]
    )
    rule = (
        f"all {nfields} field(s) bitwise except {ID_FIELD}, the pairing key, and "
        f"{CENTRAL_FIELD}, compared through the pairing"
    )
    if cut is None:
        return (
            f"PASSED: {total} galaxies over {shared} output snapshot(s) are identical under the "
            f"{comparator} comparator: {rule}, with identical key sets and no duplicates ({how})"
        )
    if cut.report_all:
        return (
            f"PASSED: {total} galaxies over {shared} output snapshot(s) paired under the "
            f"{comparator} comparator; every forest's differences and unmatched records are "
            f"reported above, none failed ({how})"
        )
    return (
        f"PASSED: {total} galaxies over {shared} output snapshot(s) paired under the "
        f"{comparator} comparator; every one outside the split forests is identical ({rule}, "
        f"and the same {ID_FIELD} for tree rows), with no unmatched key and no duplicates; the "
        f"split forests' differences and unmatched records are reported above, not failed ({how})"
    )


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
        "--compare-created",
        action="store_true",
        help=f"also compare created rows ({ID_FIELD} < 0) byte for byte, for two runs of the "
        "same driver",
    )
    parser.add_argument(
        "--match-created-by-lineage",
        action="store_true",
        help="pair created rows by (MostBoundID, SnapNum, creation ordinal) and compare them",
    )
    parser.add_argument(
        "--match-by-catalogue-id",
        action="store_true",
        help="pair tree rows by (MostBoundID, SnapNum), for datasets enumerated differently",
    )
    cut = parser.add_argument_group(
        "cut-difference report",
        "all four together place every record in its original forest and report the split "
        "forests' records instead of failing them (requires --match-by-catalogue-id)",
    )
    cut.add_argument("--multiplier", type=int, help="the runs' UniqueGalaxyID forest multiplier")
    cut.add_argument("--left-sidecar", help="forests.h5 of the first run's dataset")
    cut.add_argument("--right-sidecar", help="forests.h5 of the second run's dataset")
    cut.add_argument("--cut-record", help="the cut table's JSON record")
    cut.add_argument(
        "--report-all-forests",
        action="store_true",
        help="report every forest's differences instead of failing those outside the split "
        "forests (input errors still fail)",
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
    if args.compare_created and args.match_created_by_lineage:
        parser.error(
            "--compare-created pairs created rows by id; it excludes --match-created-by-lineage"
        )
    cut_options = (args.multiplier, args.left_sidecar, args.right_sidecar, args.cut_record)
    if any(option is not None for option in cut_options):
        if any(option is None for option in cut_options):
            parser.error(
                "the cut-difference report needs all of --multiplier, --left-sidecar, "
                "--right-sidecar and --cut-record"
            )
        if not args.match_by_catalogue_id:
            parser.error("the cut-difference report pairs tree rows by --match-by-catalogue-id")
        if args.multiplier < 1:
            parser.error(f"--multiplier must be positive, not {args.multiplier}")
    elif args.report_all_forests:
        parser.error("--report-all-forests modifies the cut-difference report, which is not given")

    try:
        return compare_runs(args)
    except ComparisonError as error:
        # Unreadable or structurally unusable input, never a scientific
        # difference: exit 2 keeps "the runs differ" (1) meaning exactly that.
        print(f"ERROR: {error}", file=sys.stderr)
        return 2


def compare_runs(args):
    """Run the comparison. Raises ComparisonError for unreadable input."""
    matching = Matching(
        "catalogue" if args.match_by_catalogue_id else "id",
        "lineage" if args.match_created_by_lineage else ("id" if args.compare_created else None),
    )
    left = scan_run(args.left)
    right = scan_run(args.right)
    labels = (args.left_label, args.right_label)

    print(f"{args.left_label}:  {args.left} ({len(left.files)} partition file(s))")
    print(f"{args.right_label}: {args.right} ({len(right.files)} partition file(s))")
    if matching.alternate:
        print(f"pairing: {matching.describe()}")

    # Every input the comparison needs is checked before the first snapshot is read.
    cut = None
    if args.cut_record is not None:
        cut = CutReport(
            args.multiplier,
            (
                read_sidecar_forest_ids(args.left_sidecar),
                read_sidecar_forest_ids(args.right_sidecar),
            ),
            read_cut_record(args.cut_record),
            args.report_all_forests,
        )
        print(
            f"cut record: {args.cut_record} ({cut.record.piece_ids.size} pieces of "
            f"{cut.split.size} split forest(s)); multiplier {args.multiplier}"
        )
    if matching.alternate:
        for index in (left, right):
            names = [name for name, _, _ in index.signature]
            missing = [name for name in CATALOGUE_FIELDS if name not in names]
            if missing:
                raise ComparisonError(
                    f"{index.files[0]}: Galaxies records carry no {missing}; the pairing keys on "
                    f"{list(CATALOGUE_FIELDS)}"
                )

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

    if left.signature != right.signature:
        print(
            "  FAIL: the two runs have different Galaxies field schemas; "
            "a bitwise field comparison is not meaningful"
        )
        print(f"    {args.left_label} fields:  {left.signature}")
        print(f"    {args.right_label} fields: {right.signature}")
        return 1

    left_snaps, right_snaps = left.snapshots, right.snapshots
    if left_snaps != right_snaps:
        print(
            f"  FAIL: output snapshot sets differ -- "
            f"only in {args.left_label}: {sorted(left_snaps - right_snaps)}, "
            f"only in {args.right_label}: {sorted(right_snaps - left_snaps)}"
        )
        failures += 1

    # One snapshot at a time: both runs' records for this snapshot are read,
    # compared, and released before the next snapshot is read.
    total = 0
    created = [0, 0]
    created_compared = 0
    for snap in sorted(left_snaps & right_snaps):
        left_records = read_snapshot(left, snap)
        right_records = read_snapshot(right, snap)
        if matching.alternate:
            outcome = compare_snapshot_matched(
                snap, left_records, right_records, labels, args.max_report, matching, cut
            )
        else:
            outcome = compare_snapshot(
                snap, left_records, right_records, labels, args.max_report, args.compare_created
            )
        failures += outcome.failures
        total += outcome.tree_rows + outcome.created_compared
        created_compared += outcome.created_compared
        created[0] += outcome.created_rows[0]
        created[1] += outcome.created_rows[1]
        del left_records, right_records

    if any(created):
        disposition = "excluded from" if matching.created is None else "included in"
        print(
            f"\nCreated rows ({ID_FIELD} < 0), {disposition} the comparison: "
            f"{args.left_label} {created[0]}, {args.right_label} {created[1]}"
        )
    if cut is not None:
        cut.print_report(labels)

    shared = len(left_snaps & right_snaps)
    if failures:
        print(f"\nFAILED: {failures} difference(s) across {shared} shared output snapshot(s)")
        return 1

    # The field count is part of the summary so a passing report states what was
    # compared rather than only that nothing differed: a reader (or a harness)
    # can check it against the run's own output schema without rerunning
    # anything. `total` counts the rows actually compared: tree rows, plus the
    # created rows compared under --compare-created or the lineage pairing.
    print("\n" + passed_line(total, shared, len(left.dtype.names), matching, created_compared, cut))
    return 0


if __name__ == "__main__":
    sys.exit(main())
