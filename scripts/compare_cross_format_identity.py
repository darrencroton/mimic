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
pair their records. Each mode below pairs one kind of row by what it describes
rather than by its id:

- ``--match-by-catalogue-id`` pairs tree rows by the catalogue key
  (``MostBoundID``, ``SnapNum``), for datasets whose forest enumeration differs.
  Every output row's ``SnapNum`` is the snapshot it is written in
  (``src/core/output_buffer.c``), so within one output snapshot the key is
  ``MostBoundID`` alone; ``SnapNum`` is still compared byte for byte.
- ``--match-created-by-lineage`` pairs created rows by (``MostBoundID``,
  ``SnapNum``, ordinal). A created record copies its host's ``MostBoundID`` at
  birth and keeps it when it is inherited, so the first is its birth host's
  even after that host's halo has moved on, and ``SnapNum`` is the output
  snapshot as for every row. The ordinal is the host's creation count decoded
  from the id, ``(-id - 1) mod MAX_CREATED_RECORDS_PER_HOST``; the rest of a
  created id encodes the host's row in its driver's processing unit, which a
  reordered dataset moves. Two surviving records born on hosts of one
  ``MostBoundID`` with one ordinal share a key, and fail as a duplicate.

In either mode a key duplicated within one run, a key present in one run only,
and a field difference are each a failure, reported as for tree rows. Paired
records agree byte for byte in every field but the two identity-valued ones:

- ``UniqueCentralGalaxyID`` is compared through the pairing: the first run's
  central must be paired with the second run's central.
- ``UniqueGalaxyID`` is not compared, since relabelling it is what the mode
  accepts. It is persistent instead (a tree galaxy keeps the id of the halo it
  formed in, a created record the id it was created with), so its pairing must
  persist: a
  first-run id paired with second-run id R at one compared output snapshot
  must be paired with R, or be unpaired, at the next one, and the same holds
  for every second-run id. A broken pairing fails, naming both snapshots and a
  sample of the ids. The check sees only consecutive compared snapshots: an id
  relabelled at the same snapshot in both runs, or relabelled across a snapshot
  where it is not paired (absent, unmatched, or in a kind of row whose
  duplicate keys stopped its pairing), is not seen.

Rows paired by id under an alternate mode -- tree rows without
``--match-by-catalogue-id`` -- are compared as under the default rule.
``--compare-created`` pairs created rows by their id and cannot be combined
with the lineage mode.

**The cut-difference report.** Cutting a forest into pieces changes the
galaxies of that forest and of no other. ``--multiplier`` with both datasets'
``forests.h5`` sidecars (``--left-sidecar``, ``--right-sidecar``) and the cut
table's JSON record (``--cut-record``) places every record in its original
forest: a tree record's ``ForestIndex`` is decoded from its id
(``UniqueGalaxyID // multiplier - 1``), looked up in its own dataset's sidecar
``ForestID``, and a piece the record lists is mapped to the forest it was cut
from; a created record follows its host into the forest of the central its
``UniqueCentralGalaxyID`` names in the same run and snapshot. The multiplier
must be the one both runs record (``RunProperties/UniqueGalaxyIDMultiplier``),
and one of the two sidecars must be the dataset the record was computed on (its
``dataset`` entry's ``n_forests_total`` and ``forest_id_sha256``).
A record that cannot be placed is an input error.
Records of the forests the record splits are paired by the catalogue key
(``--match-by-catalogue-id`` is required) and **reported, not failed**: per
forest (one line for every split forest with records) and in total, how many
are identical, how many differ and in which fields, and how many are
unmatched; their pairing is not held to persist. Records of every other forest
keep the strict rule, including an identical ``UniqueGalaxyID`` for tree rows,
since a forest the cut leaves alone keeps its ``ForestIndex``.
``--report-all-forests`` reports every forest without failing instead, for a
model whose identity-keyed choices can move outside the split forests; input
errors still fail.

Usage:

    compare_cross_format_identity.py <dir/basename> <dir/basename> [options]

where each argument is an output directory joined with the run's
``output_filename`` -- for example ``output/halos-only-micro-uchuu-ascii/halos``.
Galaxies are aggregated across every numbered partition file
``<basename>_<digits>.hdf5`` -- or, for a multi-task horizontal run,
``<basename>_<digits>_task<digits>.hdf5`` -- in ascending (number, task) order.
The master ``<basename>.hdf5`` is read for its links only: the partitions found
must be exactly the files it links, so a stale partition left by an earlier run
in the same directory, or a linked partition that is missing, is an input
error. One run's partitions are either all task-suffixed or none of them are;
a mixture is an input error.

Exit status is 0 when the two runs are identical under the rules above, 1 when
they are not, 2 for a usage or input error, and 3 when the comparator itself
fails (an exhausted memory, or a defect in this script): its traceback is
printed and the status says nothing about the runs. Unreadable input is an
input error, never a difference: a corrupt or truncated partition reported as a
bitwise difference would send someone hunting a physics bug that does not
exist.

Both runs are read one output snapshot at a time rather than held whole. The
comparator exists to compare production runs, and its memory is heaviest exactly
when a large divergence is being diagnosed, which is the worst moment to run out
of it. A Shin-Uchuu output snapshot holds up to 490 million records, over
100 GB per run, so no step copies a run's records: rows are selected and paired
by position (``Rows``) and one field of the pairs is gathered at a time. Beside
both runs' snapshot records, per row of one run it holds:

- the pairing: each run's key column (8 B, a view of the records under the
  default rule when every row is compared) with its sort order and sorted copy
  (16 B), then each run's paired positions (8 B per pair); an alternate mode
  also holds each run's tree and created positions (8 B) and a tree mask (1 B
  per pair), and key sets that differ add the search that pairs them (about
  10 B);
- per field: both runs' gathered values and the boolean array of their byte
  comparison (each the field's itemsize per pair: 12 B for ``Pos``), plus a
  mismatch mask (1 B per pair); ``UniqueCentralGalaxyID`` instead maps each
  central through a sort of the first run's ids (about 60 B per pair, released
  before the next field, keeping only the reported sample);
- under the cut report, each pair's original forest (8 B) and the reported,
  strict and any-difference masks (3 B per pair), and each run's placement of
  its rows (8 B, and a sort of the tree ids only when created rows are placed);
- under an alternate mode, from one snapshot to the next, the previous
  snapshot's id pairs (16 B per pair) that the persistence check reads.

Measured with ``tracemalloc`` beside both runs' records, at a snapshot after the
first, a comparison peaked at about 57 B per row of one run under the default
rule, 99 B under the catalogue pairing and 114 B with the cut report as well
(the self-test pins these figures). The cut-difference
report adds, for the whole comparison, both sidecars' ``ForestID`` columns (8 B
per forest each), the record's piece ids and their original forests (16 B per
piece, plus one column's line and its text, about 26 B per piece, while it is
parsed), and its per-forest tallies (about 40 B per reported forest per
snapshot, and 16 B per differing field of each).
"""

import argparse
import contextlib
import hashlib
import json
import re
import sys
import traceback
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

#: The lineage key packs (MostBoundID, ordinal) into one int64 as
#: MostBoundID * radix + ordinal, which holds for |MostBoundID| up to this.
LINEAGE_MOSTBOUNDID_LIMIT = numpy.iinfo(numpy.int64).max // MAX_CREATED_RECORDS_PER_HOST - 1

#: The two piece columns of a cut table's record the report reads.
RECORD_PIECE_COLUMNS = ("id", "forest_id")

#: The record's dataset identity entries the sidecars are checked against.
RECORD_IDENTITY_KEYS = ("n_forests_total", "forest_id_sha256")

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


#: The default rule: tree rows by id, created rows not compared.
BY_ID = Matching("id", None)


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
    holding both is stale or mixed output and an input error. The files found
    must be exactly those the master ``<base>.hdf5`` links.
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
    files = [entry for _, _, entry in chunks]
    linked = master_links(directory / f"{base}.hdf5")
    stale = sorted(entry.name for entry in files if entry.name not in linked)
    missing = sorted(linked - {entry.name for entry in files})
    if stale or missing:
        raise ComparisonError(
            f"{directory / base}.hdf5 does not link exactly the partition files present: "
            f"{len(stale)} present but not linked (stale output?) {stale[:5]}, "
            f"{len(missing)} linked but missing {missing[:5]}"
        )
    return files


def master_links(master):
    """The file names a run's master file links its Snap###/<partition>/Galaxies to."""
    if not master.is_file():
        raise ComparisonError(f"{master}: no master file, which lists the run's partitions")
    names = set()
    with open_partition(master) as handle:
        for snap in handle:
            if SNAP_GROUP_RE.match(snap) is None:
                continue
            if not isinstance(handle[snap], h5py.Group):
                raise ComparisonError(f"{master}: /{snap} is not a group of partition links")
            for partition in handle[snap].values():
                if not isinstance(partition, h5py.Group):
                    continue
                link = partition.get("Galaxies", getlink=True)
                if isinstance(link, h5py.ExternalLink):
                    names.add(link.filename)
    return names


@contextlib.contextmanager
def open_partition(path, snap=None):
    """Open one partition file, turning any I/O failure into a ComparisonError.

    h5py raises OSError for a file that is missing, truncated, not HDF5, or
    corrupt mid-dataset, and dataset reads inside the block raise it too. Left
    uncaught that would surface as a traceback and the exit status reserved for
    a failure of the comparator itself. Unreadable input is an input error, so it
    is named with its path (and snapshot, where the caller knows one).
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
    snapshot twice at the moment it is read.

    With `field`, only that column is read -- which is how the duplicate pass
    looks at every snapshot of both runs for the price of one column per galaxy
    instead of a whole record. h5py has no read_direct for a single field of a
    compound dataset, so that path takes the copy, the column's width per galaxy,
    released before the next snapshot.
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


def report_repeats(label, snap, what, unit, ordered, name_of, max_report):
    """Report the keys repeated in one run's sorted keys `ordered`. Returns how many repeat.

    `name_of(i)` names the key at sorted position i. Only a run that has a
    repeat pays for more than one comparison of neighbours.
    """
    same = ordered[1:] == ordered[:-1]
    if not same.any():
        return 0
    starts = numpy.flatnonzero(numpy.concatenate(([True], ~same)))
    counts = numpy.diff(numpy.append(starts, ordered.size))
    starts, counts = starts[counts > 1], counts[counts > 1]
    print(
        f"  FAIL Snap{snap:03d}: {label} contains {starts.size} duplicated {what} "
        f"({int(counts.sum() - starts.size)} extra record(s) beyond one per {unit})"
    )
    for start, count in zip(starts[:max_report], counts[:max_report]):
        print(f"    {name_of(start)} appears {int(count)} times")
    if starts.size > max_report:
        print(f"    ... and {starts.size - max_report} more duplicated {unit}(s)")
    return int(starts.size)


def report_run_duplicates(label, index, max_report):
    """Scan every output snapshot of one run for duplicated ids.

    Returns the number of snapshots that carry duplicates. Run before anything
    else and over every snapshot the run has, not only the shared ones, so a
    duplicate in a snapshot the other run lacks is still named. Only the id
    column is read, and only one snapshot's worth at a time.
    """
    duplicated = 0
    for snap in sorted(index.snapshots):
        ids = numpy.sort(read_snapshot(index, snap, field=ID_FIELD))
        name_of = lambda at, ids=ids: f"{ID_FIELD} {int(ids[at])}"  # noqa: E731
        if report_repeats(label, snap, f"{ID_FIELD} value(s)", "id", ids, name_of, max_report):
            duplicated += 1
        del ids
    return duplicated


class Rows:
    """Some rows of one snapshot's records, held as their positions in them.

    ``rows[name]`` gathers one field of the selected rows, so a selection costs
    8 B per row instead of a copy of every record it holds; positions of None
    select every row, and ``rows[name]`` is then a view of the records.
    ``rows.at(name, i)`` reads one field of the i-th row alone, and
    ``rows.where(index)`` turns indices into the selection into record positions.
    """

    __slots__ = ("records", "positions")

    def __init__(self, records, positions=None):
        self.records = records
        self.positions = positions

    def __len__(self):
        return len(self.records) if self.positions is None else len(self.positions)

    def __getitem__(self, name):
        column = self.records[name]
        return column if self.positions is None else column[self.positions]

    def at(self, name, row):
        return self.records[name][row if self.positions is None else self.positions[row]]

    def where(self, index):
        return index if self.positions is None else self.positions[index]

    @property
    def dtype(self):
        return self.records.dtype


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


# --------------------------------------------------------------------------
# Pairing
# --------------------------------------------------------------------------


def created_ordinals(ids):
    """Each created id's host creation ordinal, ``(-id - 1) mod`` the radix.

    ``-(id + 1)`` is formed rather than ``-id - 1``, as the C decoder does, so
    no step overflows even for the most negative int64.
    """
    return numpy.negative(ids + 1) % MAX_CREATED_RECORDS_PER_HOST


def pairing_keys(rows, mode, where):
    """Each row's int64 pairing key under `mode` ("id", "catalogue" or "lineage").

    Within one output snapshot ``SnapNum`` is the snapshot itself, so a
    catalogue key is ``MostBoundID`` and a lineage key ``MostBoundID * radix +
    ordinal``, the lineage pair packed into one int64 so one sort pairs it.
    """
    if mode == "id":
        return rows[ID_FIELD]
    keys = numpy.asarray(rows["MostBoundID"], dtype=numpy.int64)
    if mode == "catalogue":
        return keys
    # signed bounds: abs(INT64_MIN) stays negative and would slip through
    outside = (keys > LINEAGE_MOSTBOUNDID_LIMIT) | (keys < -LINEAGE_MOSTBOUNDID_LIMIT)
    if outside.any():
        raise ComparisonError(
            f"{where}: created record MostBoundID {int(keys[outside][0])} is beyond the "
            f"+/-{LINEAGE_MOSTBOUNDID_LIMIT} the lineage key packs"
        )
    return keys * MAX_CREATED_RECORDS_PER_HOST + created_ordinals(rows[ID_FIELD])


def describe_key(rows, mode, row):
    """One row's pairing key as ``(name=value, ...)``, read from its record."""
    names = [ID_FIELD] if mode == "id" else list(CATALOGUE_FIELDS)
    values = [f"{name}={int(rows.at(name, row))}" for name in names]
    if mode == "lineage":
        values.append(f"ordinal={int(created_ordinals(rows.at(ID_FIELD, row)))}")
    return "(" + ", ".join(values) + ")"


def pair_sorted(left, right):
    """Pair two runs' keys, each given as (order, sorted keys) and unique within its run.

    Returns (left index, right index, left only, right only): indices into each
    run's keys, all in ascending key order. Equal key sets, the common case, are
    decided by one comparison of the sorted keys.
    """
    (left_order, left_keys), (right_order, right_keys) = left, right
    if numpy.array_equal(left_keys, right_keys):
        none = left_order[:0]
        return left_order, right_order, none, none
    at = numpy.searchsorted(right_keys, left_keys)
    at[at == right_keys.size] = 0
    found = right_keys[at] == left_keys if right_keys.size else numpy.zeros(left_keys.size, bool)
    hit = numpy.zeros(right_keys.size, dtype=bool)
    hit[at[found]] = True
    return left_order[found], right_order[at[found]], left_order[~found], right_order[~hit]


class Pairs(NamedTuple):
    """One snapshot's paired records: positions in each run's records, a tree mask,
    each pair's original forest (None without the cut report), and the number
    of tree and created pairs."""

    left: numpy.ndarray
    right: numpy.ndarray
    is_tree: numpy.ndarray
    forests: Optional[numpy.ndarray]
    tree: int
    created: int


def row_groups(records, matching):
    """The rows of one run's snapshot that are paired, as [(kind, mode, Rows)].

    The default rule pairs every compared row by id in one group, so its id-set
    report covers them all; an alternate mode pairs tree and created rows apart.
    """
    ids = records[ID_FIELD]
    if not matching.alternate:
        compared = ids != 0 if matching.created else ids > 0
        return [
            ("id", "id", Rows(records, None if compared.all() else numpy.flatnonzero(compared)))
        ]
    groups = [("tree", matching.tree, Rows(records, numpy.flatnonzero(ids > 0)))]
    if matching.created is not None:
        groups.append(("created", matching.created, Rows(records, numpy.flatnonzero(ids < 0))))
    return groups


class Unpaired(NamedTuple):
    """One run's rows of one kind and the indices into them that found no partner."""

    rows: Rows
    only: numpy.ndarray


def report_unpaired(snap, kind, mode, labels, left, right, max_report):
    """Report keys present in one run only, given each run's Unpaired rows."""
    sides = (left, right)
    totals = ", ".join(
        f"{only.size} only in {label} ({len(rows)} total)"
        for label, (rows, only) in zip(labels, sides)
    )
    what = f"{ID_FIELD} sets" if kind == "id" else f"{kind}-row key sets"
    print(f"  FAIL Snap{snap:03d}: {what} differ -- {totals}")
    for label, (rows, only) in zip(labels, sides):
        if only.size == 0:
            continue
        more = only.size - max_report
        if kind == "id":
            sample = ", ".join(str(int(rows.at(ID_FIELD, row))) for row in only[:max_report])
            print(f"    only in {label}: {sample}" + (f", ... (+{more})" if more > 0 else ""))
            continue
        for row in only[:max_report]:
            key = describe_key(rows, mode, row)
            print(f"    only in {label}: {key} ({ID_FIELD} {int(rows.at(ID_FIELD, row))})")
        if more > 0:
            print(f"    only in {label}: ... (+{more})")


#: The cut report's tally columns of each run's unpaired records.
COLUMNS_ONLY = ("only_left", "only_right")


def pair_snapshot(snap, left, right, labels, max_report, matching, cut):
    """Pair one output snapshot's records under `matching`. Returns (failures, Pairs).

    Each group of rows (``row_groups``) is keyed (``pairing_keys``), sorted
    once per run, checked for a repeated key unless keyed by the ids already
    proven unique, and paired. The default rule stops at an id-set difference,
    as the field comparison of its remaining ids would add nothing; an alternate
    mode compares whatever paired. With `cut`, every row is first placed in its
    original forest and unpaired rows of a reported forest are tallied, not failed.
    """
    groups = [row_groups(records, matching) for records in (left, right)]
    paired = []  # (kind, left positions, right positions, forests or None)
    failures = 0
    for (kind, mode, left_rows), (_, _, right_rows) in zip(*groups):
        sides = (left_rows, right_rows)
        forests = (None, None)
        if cut is not None:
            forests = tuple(
                cut.place(side, rows, kind, f"{labels[side]} Snap{snap:03d}")
                for side, rows in enumerate(sides)
            )
        keyed = []
        for label, rows in zip(labels, sides):
            keys = pairing_keys(rows, mode, f"{label} Snap{snap:03d}")
            order = numpy.argsort(keys)
            keyed.append((order, keys[order]))
            del keys
        repeats = 0
        for label, rows, (order, ordered) in zip(labels, sides, keyed):
            if mode != "id":  # ids are already proven unique

                def name_of(at, r=rows, o=order, m=mode):
                    return describe_key(r, m, o[at])

                what = f"{kind}-row key(s)"
                repeats += report_repeats(label, snap, what, "key", ordered, name_of, max_report)
        if repeats:
            failures += 1
            continue
        left_index, right_index, only_left, only_right = pair_sorted(*keyed)
        del keyed

        if cut is not None:
            strict = []
            for column, only, side in zip(COLUMNS_ONLY, (only_left, only_right), forests):
                reported = cut.reported(side[only])
                cut.add(column, side[only][reported])
                strict.append(only[~reported])
            only_left, only_right = strict
        if only_left.size or only_right.size:
            left_only, right_only = Unpaired(left_rows, only_left), Unpaired(right_rows, only_right)
            report_unpaired(snap, kind, mode, labels, left_only, right_only, max_report)
            failures += 1
            if kind == "id":
                return failures, None

        placed = None
        if cut is not None:
            placed = forests[0][left_index]
            moved = placed != forests[1][right_index]
            if moved.any():
                raise ComparisonError(
                    f"Snap{snap:03d}: paired {kind} records {ID_FIELD} "
                    f"{int(left_rows[ID_FIELD][left_index][moved][0])} and "
                    f"{int(right_rows[ID_FIELD][right_index][moved][0])} are placed in original "
                    f"forests {int(placed[moved][0])} and "
                    f"{int(forests[1][right_index][moved][0])}; the sidecars or the cut "
                    f"record do not describe these runs"
                )
        paired.append((kind, left_rows.where(left_index), right_rows.where(right_index), placed))

    counts = {kind: len(positions) for kind, positions, _, _ in paired}
    if sum(counts.values()) == 0:
        return failures, None
    return failures, Pairs(
        numpy.concatenate([positions for _, positions, _, _ in paired]),
        numpy.concatenate([positions for _, _, positions, _ in paired]),
        numpy.concatenate([numpy.full(len(p), kind != "created") for kind, p, _, _ in paired]),
        None if cut is None else numpy.concatenate([f for _, _, _, f in paired]),
        counts.get("tree", 0) + counts.get("id", 0),
        counts.get("created", 0),
    )


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


class PairHistory:
    """The previous compared snapshot's id pairs, for the persistence check.

    Holds the pairs (first-run id, second-run id) sorted by the first, 16 B per
    pair, and only those of the most recent snapshot.
    """

    def __init__(self):
        self.snap = None
        self.ids = (numpy.zeros(0, numpy.int64), numpy.zeros(0, numpy.int64))

    def check(self, snap, ids, labels, max_report):
        """Report the ids at `snap` whose partner differs from the previous snapshot's,
        then keep `snap`'s pairs (`ids`, each run's ids per pair). Returns 0 or 1."""
        total, samples = 0, []
        for this in (0, 1):
            before, partner = self.ids[this], self.ids[1 - this]
            if before.size == 0 or ids[this].size == 0:
                continue
            if this:
                order = numpy.argsort(before)
                before, partner = before[order], partner[order]
            at = numpy.minimum(numpy.searchsorted(before, ids[this]), before.size - 1)
            rows = numpy.flatnonzero((before[at] == ids[this]) & (partner[at] != ids[1 - this]))
            total += rows.size
            samples += [(this, row, int(partner[at[row]])) for row in rows[:max_report]]
        if total:
            print(
                f"  FAIL Snap{snap:03d}: {total} paired {ID_FIELD}(s) changed partner since "
                f"Snap{self.snap:03d}; a paired id keeps its partner from one compared snapshot "
                f"to the next"
            )
            for this, row, earlier in samples[:max_report]:
                print(
                    f"    {labels[this]} {ID_FIELD} {int(ids[this][row])}: paired with "
                    f"{int(ids[1 - this][row])} here, with {earlier} at Snap{self.snap:03d}"
                )
            if total > max_report:
                print(f"    ... and {total - max_report} more")
        order = numpy.argsort(ids[0])
        self.snap, self.ids = snap, (ids[0][order], ids[1][order])
        return int(total > 0)


# --------------------------------------------------------------------------
# The cut-difference report
# --------------------------------------------------------------------------


class CutRecord(NamedTuple):
    """What the report needs from a cut table's record: every piece of every
    split forest (ascending piece id) with the forest it was cut from, the
    fresh-id floor (the catalogue's largest forest id, above which every fresh
    piece id lies) and the identity of the dataset it was computed on."""

    piece_ids: numpy.ndarray
    forest_ids: numpy.ndarray
    fresh_id_floor: int
    dataset: dict

    @property
    def split(self):
        """The split original forests, ascending."""
        return numpy.unique(self.forest_ids)


def read_cut_record(path):
    """Read a cut table's record: its dataset identity, fresh-id floor and two piece columns.

    The record is written one header entry per line, then one line per piece
    column, so the entries needed are parsed line by line rather than loading
    the whole document: a whole-simulation record holds six columns of tens of
    millions of pieces. Raises ComparisonError for anything else.
    """
    header = {}
    columns = {}
    try:
        with open(path) as handle:
            for line in handle:
                # Entries are identified by their start; a column line is
                # sliced once, never stripped whole.
                head = line[:80].lstrip()
                for name in ("fresh_id_floor", "dataset"):
                    if head.startswith(f'"{name}":'):
                        header[name] = json.loads(line.split(":", 1)[1].strip().rstrip(","))
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

    floor, dataset = header.get("fresh_id_floor"), header.get("dataset")
    missing = [name for name in RECORD_PIECE_COLUMNS if name not in columns]
    if not isinstance(dataset, dict) or any(key not in dataset for key in RECORD_IDENTITY_KEYS):
        missing.append(f"dataset {list(RECORD_IDENTITY_KEYS)}")
    if not isinstance(floor, int) or missing:
        raise ComparisonError(
            f"{path}: not a cut table record (fresh_id_floor {floor!r}, missing {missing})"
        )
    piece_ids, forest_ids = columns["id"], columns["forest_id"]
    if piece_ids.size != forest_ids.size:
        raise ComparisonError(
            f"{path}: {piece_ids.size} piece ids but {forest_ids.size} original forest ids"
        )
    if piece_ids.size and numpy.any(piece_ids[1:] <= piece_ids[:-1]):
        raise ComparisonError(f"{path}: piece ids are not strictly ascending")
    # A piece either keeps its forest's id or carries a fresh one.
    invalid = (piece_ids != forest_ids) & (piece_ids <= floor)
    if invalid.any():
        raise ComparisonError(
            f"{path}: piece {int(piece_ids[invalid][0])} of forest "
            f"{int(forest_ids[invalid][0])} neither keeps the forest's id nor is fresh "
            f"(above {floor})"
        )
    return CutRecord(piece_ids, forest_ids, floor, dataset)


def recorded_multiplier(index):
    """The UniqueGalaxyIDMultiplier every partition of one run records in RunProperties."""
    values = set()
    for path in index.files:
        with open_partition(path) as handle:
            properties = handle.get("RunProperties")
            if properties is None or "UniqueGalaxyIDMultiplier" not in properties.attrs:
                raise ComparisonError(f"{path}: no RunProperties/UniqueGalaxyIDMultiplier")
            values.add(int(numpy.asarray(properties.attrs["UniqueGalaxyIDMultiplier"]).ravel()[0]))
    if len(values) != 1:
        raise ComparisonError(
            f"{index.files[0].parent}: partitions record UniqueGalaxyIDMultiplier {sorted(values)}"
        )
    return values.pop()


def read_sidecar_forest_ids(path):
    """A dataset sidecar's ForestID column; row i is the forest of ForestIndex i."""
    try:
        with h5py.File(path, "r") as handle:
            if "ForestID" not in handle:
                raise ComparisonError(f"{path}: no ForestID dataset")
            return numpy.asarray(handle["ForestID"][()], dtype=numpy.int64)
    except OSError as error:
        raise ComparisonError(f"{path}: cannot be read ({error})") from error


def check_record_dataset(record, record_path, sidecars, sidecar_paths):
    """One sidecar must be the dataset the record was computed on, by its identity.

    The census binds a record to its dataset by the ForestID column's count and
    SHA-256 (as little-endian int64, in ForestIndex order), so a record of
    another dataset, or of another version of this one, is refused by name.
    """
    wanted = tuple(record.dataset[key] for key in RECORD_IDENTITY_KEYS)
    found = [
        (int(ids.size), hashlib.sha256(numpy.ascontiguousarray(ids, dtype="<i8")).hexdigest())
        for ids in sidecars
    ]
    if wanted not in found:
        described = "; ".join(f"{path}: {identity}" for path, identity in zip(sidecar_paths, found))
        raise ComparisonError(
            f"{record_path}: the cut record was computed on the dataset with "
            f"{dict(zip(RECORD_IDENTITY_KEYS, wanted))}, which neither sidecar is ({described})"
        )


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
        unlisted_fresh = ~listed & (forest_ids > record.fresh_id_floor)
        if unlisted_fresh.any():
            raise ComparisonError(
                f"{where}: ForestID {int(forest_ids[unlisted_fresh][0])} is a fresh piece id "
                f"(above {record.fresh_id_floor}) that the cut record does not list"
            )
        return mapped

    def place(self, side, rows, kind, where):
        """Original forests of one run's tree or created rows in one snapshot.

        A created record follows its host into the host's forest. Its
        MostBoundID is its birth host's and goes stale once the record is
        inherited, but its UniqueCentralGalaxyID names the central of the FoF
        group it is in now, a tree row of the same run and snapshot.
        """
        ids = rows.records[ID_FIELD]
        wanted = rows[ID_FIELD] if kind == "tree" else rows[CENTRAL_FIELD]
        if kind == "created" and len(wanted):
            tree = numpy.flatnonzero(ids > 0)
            order = numpy.argsort(ids[tree], kind="stable")
            ordered = ids[tree][order]
            at = numpy.minimum(numpy.searchsorted(ordered, wanted), max(ordered.size - 1, 0))
            found = ordered[at] == wanted if ordered.size else numpy.zeros(len(wanted), bool)
            if not found.all():
                missing = ~found
                raise ComparisonError(
                    f"{where}: created record {ID_FIELD} {int(rows[ID_FIELD][missing][0])} "
                    f"cannot be placed: its {CENTRAL_FIELD} {int(wanted[missing][0])} is no tree "
                    f"row of this snapshot"
                )
        sidecar = self.sidecars[side]
        forest_index = wanted // self.multiplier - 1
        outside = (forest_index < 0) | (forest_index >= sidecar.size)
        if outside.any():
            raise ComparisonError(
                f"{where}: {ID_FIELD} {int(wanted[outside][0])} decodes to ForestIndex "
                f"{int(forest_index[outside][0])} under multiplier {self.multiplier}, outside "
                f"the sidecar's {sidecar.size} forests"
            )
        return self.original(sidecar[forest_index], where)

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
        """The per-forest and total report: one line for every reported forest with records."""
        forests, columns, fields = self.table()
        names = {"only_left": f"only in {labels[0]}", "only_right": f"only in {labels[1]}"}
        scope = "every forest" if self.report_all else f"the {self.split.size} split forest(s)"
        print(f"\nCut-difference report ({scope}; reported, not failed):")
        changed = (columns["differing"] + columns["only_left"] + columns["only_right"]) > 0
        for position in range(forests.size):
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


# --------------------------------------------------------------------------
# One snapshot
# --------------------------------------------------------------------------


def compare_snapshot(snap, left, right, labels, max_report, matching=BY_ID, cut=None, history=None):
    """Compare one output snapshot's records under `matching`. Returns a SnapshotResult.

    The records are paired (``pair_snapshot``) and every pair compared field by
    field as raw bytes. Under an alternate mode ``UniqueGalaxyID`` is not
    compared but its pairing is checked against the previous snapshot's
    (`history`, a PairHistory), and ``UniqueCentralGalaxyID`` is compared
    through the pairing (``central_mapping``). With `cut`, records of a
    reported forest are tallied instead of failing, and records of every other
    forest are held to the strict rule, which for tree rows includes an
    identical ``UniqueGalaxyID``.

    Precondition: neither run carries duplicated ids -- report_run_duplicates()
    has already run over both and the comparison stopped if it found any.
    """
    alternate = matching.alternate
    ids = (left[ID_FIELD], right[ID_FIELD])
    created = tuple(int(numpy.count_nonzero(column < 0)) for column in ids)
    zeros = tuple(int(numpy.count_nonzero(column == 0)) for column in ids)
    failures = 0
    if matching.created is None:
        report_created(snap, created, labels)
    if any(zeros):
        print(
            f"  FAIL Snap{snap:03d}: {ID_FIELD} 0 is neither a tree id nor a created id -- "
            f"{labels[0]} {zeros[0]}, {labels[1]} {zeros[1]} row(s)"
        )
        failures += 1

    paired_failures, pairs = pair_snapshot(snap, left, right, labels, max_report, matching, cut)
    failures += paired_failures
    if alternate:
        tree_rows, created_compared = (0, 0) if pairs is None else (pairs.tree, pairs.created)
    else:
        tree_rows = int(numpy.count_nonzero(ids[0] > 0))
        created_compared = created[0] if matching.created else 0

    def result(failures):
        return SnapshotResult(failures, tree_rows, created, created_compared)

    if pairs is None:
        if not failures:
            print(f"  ok   Snap{snap:03d}: both runs are empty")
        return result(failures)

    # Each field is gathered from the snapshot records when it is compared.
    pair_left, pair_right = Rows(left, pairs.left), Rows(right, pairs.right)
    reported = (
        numpy.zeros(len(pair_left), dtype=bool) if cut is None else cut.reported(pairs.forests)
    )
    strict = ~reported
    if history is not None:
        # Pairs made by id are trivially persistent; the others are checked.
        by_key = numpy.where(pairs.is_tree, matching.tree != "id", matching.created == "lineage")
        held = numpy.flatnonzero(strict & by_key)
        held_ids = (left[ID_FIELD][pairs.left[held]], right[ID_FIELD][pairs.right[held]])
        del by_key, held
        failures += history.check(snap, held_ids, labels, max_report)
        del held_ids

    any_differs = numpy.zeros(len(pair_left), dtype=bool) if cut is not None else None
    differing_fields = []  # (field, total strict mismatches, bounded sample of pair rows)
    centrals = {}  # sampled pair row -> (found, mapped) for the central report
    for name in pair_left.dtype.names:
        if alternate and name == ID_FIELD:
            if cut is None:
                continue
            # A forest the cut leaves alone keeps its ForestIndex, so its tree
            # rows keep their ids; created ids move with their hosts' rows.
            mismatched = pairs.is_tree & (pair_left[ID_FIELD] != pair_right[ID_FIELD])
        elif alternate and name == CENTRAL_FIELD:
            mismatched, found, mapped = central_mapping(pair_left, pair_right)
            for row in numpy.flatnonzero(mismatched & strict)[:max_report]:
                centrals[int(row)] = (bool(found[row]), int(mapped[row]))
            del found, mapped
        else:
            left_bytes = field_bytes(pair_left[name])
            right_bytes = field_bytes(pair_right[name])
            mismatched = (left_bytes != right_bytes).any(axis=1)
            del left_bytes, right_bytes
        if cut is not None and name != ID_FIELD:
            cut.add_field(name, pairs.forests[mismatched & reported])
            any_differs |= mismatched & reported
        rows = numpy.flatnonzero(mismatched & strict)
        if rows.size:
            differing_fields.append((name, int(rows.size), rows[:max_report]))

    if cut is not None:
        cut.add("identical", pairs.forests[reported & ~any_differs])
        cut.add("differing", pairs.forests[reported & any_differs])

    nfields = len(pair_left.dtype.names)
    held = int(strict.sum())
    differ = f"{len(differing_fields)} of {nfields} field(s) differ over"
    if alternate:
        scope = f"{pairs.tree} tree and {pairs.created} created record(s) paired"
        if cut is not None:
            scope += (
                f", {int(reported.sum())} in reported forests ({int(any_differs.sum())} differing)"
            )
        ok = (
            f"{scope}; the {held} held to the strict rule are identical in all {nfields} fields "
            f"under the pairing"
        )
        fail = f"{scope}; {differ} the {held} held to the strict rule"
    else:
        ok = f"{held} galaxies, all {nfields} fields byte-identical"
        fail = f"{differ} {held} shared id(s)"
    if not differing_fields:
        print(f"  ok   Snap{snap:03d}: {ok}")
        return result(failures)

    print(f"  FAIL Snap{snap:03d}: {fail}")
    for name, total, sample in differing_fields:
        print(f"    {name}: {total} record(s) differ")
        for row in sample:
            pair = f"{ID_FIELD} {int(pair_left.at(ID_FIELD, row))}"
            if alternate:
                pair += f" <-> {int(pair_right.at(ID_FIELD, row))}"
            if alternate and name == CENTRAL_FIELD:
                found, mapped = centrals[int(row)]
                maps = f"maps to {mapped}" if found else "is paired with no record"
                print(
                    f"      {pair}: {labels[0]} central {int(pair_left.at(CENTRAL_FIELD, row))} "
                    f"{maps}, {labels[1]} central {int(pair_right.at(CENTRAL_FIELD, row))}"
                )
            else:
                print(
                    f"      {pair}: {labels[0]}={pair_left.at(name, row)!r} "
                    f"{labels[1]}={pair_right.at(name, row)!r}"
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
        help=f"pair created rows by (MostBoundID, SnapNum, creation ordinal) and compare them; "
        f"their {ID_FIELD} is not compared but must keep its partner from one compared snapshot "
        f"to the next, and {CENTRAL_FIELD} is compared through the pairing",
    )
    parser.add_argument(
        "--match-by-catalogue-id",
        action="store_true",
        help=f"pair tree rows by (MostBoundID, SnapNum), for datasets enumerated differently; "
        f"their {ID_FIELD} is not compared but must keep its partner from one compared snapshot "
        f"to the next, and {CENTRAL_FIELD} is compared through the pairing",
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
    except Exception:  # any other failure is the comparator's own, never the runs'
        traceback.print_exc()
        print(
            "ERROR: the comparator failed (exit 3); this says nothing about the two runs",
            file=sys.stderr,
        )
        return 3


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
        # Kept by side, never by label: two runs may carry the same label.
        for label, index in zip(labels, (left, right)):
            recorded = recorded_multiplier(index)
            if recorded != args.multiplier:
                raise ComparisonError(
                    f"--multiplier {args.multiplier} is not the UniqueGalaxyIDMultiplier "
                    f"{recorded} that {label} ({index.files[0].parent}) records"
                )
        sidecar_paths = (args.left_sidecar, args.right_sidecar)
        sidecars = tuple(read_sidecar_forest_ids(path) for path in sidecar_paths)
        record = read_cut_record(args.cut_record)
        check_record_dataset(record, args.cut_record, sidecars, sidecar_paths)
        cut = CutReport(args.multiplier, sidecars, record, args.report_all_forests)
        print(
            f"cut record: {args.cut_record} ({cut.record.piece_ids.size} pieces of "
            f"{cut.split.size} split forest(s)); multiplier {args.multiplier}"
        )
    if matching.alternate:
        needed = [*CATALOGUE_FIELDS, *([CENTRAL_FIELD] if cut is not None else [])]
        for index in (left, right):
            names = [name for name, _, _ in index.signature]
            missing = [name for name in needed if name not in names]
            if missing:
                raise ComparisonError(
                    f"{index.files[0]}: Galaxies records carry no {missing}; the pairing and "
                    f"the cut report read {needed}"
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
    history = PairHistory() if matching.alternate else None
    for snap in sorted(left_snaps & right_snaps):
        left_records = read_snapshot(left, snap)
        right_records = read_snapshot(right, snap)
        outcome = compare_snapshot(
            snap, left_records, right_records, labels, args.max_report, matching, cut, history
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
