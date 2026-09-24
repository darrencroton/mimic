#!/usr/bin/env python3
"""Generalisation acceptance harness: measured runs and an independent comparator.

This is the instrument the converter-generalisation plan's acceptance evidence
is produced with (``docs/dev/MIMIC-CONVERTER-GENERALISATION-IMPLEMENTATION-PLAN.md``,
Slices 10 and 11). It answers one question per comparison: does a converted
horizontal-HDF5 version 3 dataset say exactly what the **selected source
format's own vertical interpretation** says, halo by halo, link by link and bit
by bit? Running it against real data is Slice 11's job; this module makes no
acceptance claim of its own, and a PASS it prints is a statement about the two
inputs it was handed, nothing more.

Independence
------------
Nothing here imports the converter. The reference side comes from two places
that share none of its code:

* the C harness ``tests/unit/tools/dump_ctrees_topology.c`` in its
  ``--source-payload`` mode (``mimic-source-dump v1``): Mimic's own, unmodified
  vertical readers, dumping every halo by its reader-assigned identity
  ``(forest number, within-forest rank)``, every link as the target's
  within-forest rank (plus the target's snapshot for the three
  snapshot-qualified links) and the core payload as exact integers and binary32
  bit patterns;
* for selected extra fields, the independent extractors below, which read the
  source files directly -- L-Halo binary records through a layout recomputed
  here from the package's ordered ``halo_properties.yaml``, forests-HDF5
  through raw ``h5py`` reads walked in ``ForestInfo`` order, and ASCII through
  a plain text parse -- and assign ``SourceHaloID`` from the format
  specification's own definition (a prefix sum over the declared source order,
  starting at 1), not from any converter helper.

The converted side is read with plain ``h5py``. Topology is compared in the
reference's key space: every converted link ``(target snapshot, target row)``
is resolved, by a bounded external sort and merge join, to the target row's
``(ForestIndex, HaloRankInForest)``, and that pair must equal the dump's
``(forest, target rank)``. ``MostBoundID`` is compared as data and never used
as a key (L-Halo particle identifiers are signed and repeat).

What the comparator detects, each as its own named check: dropped and extra
rows, duplicated rows on either side, wrong ``SnapNum``, a link resolving to
the wrong halo (which is how a reordered progenitor or FoF chain shows up),
a wrong target snapshot, a changed payload bit, and -- for the unit-forest
formats, whose forest enumeration *is* the inventory order -- a wrong
``SourceHaloID``. Columns it does not compare (converter extras, or extra
columns in a newer dump) are ignored rather than failing the comparison. A
comparison that matches no rows at all is a FAIL, never a vacuous PASS.

Bounded resources
-----------------
No catalog-sized array or dictionary is built. Every join runs over sorted
streams produced by :class:`ExternalSorter`, which sorts fixed-width records
in runs bounded by its share of ``--budget-mb``, spills them to its own
temporary directory and merges them in bounded blocks, over as many merge
passes as its fan-in requires. Every key and index is int64 throughout, so row
indices above 2^31 and keys above 2^53 are exact. The budget bounds the sort
and merge buffers (split evenly between the comparison's concurrent sorters),
not total interpreter RSS; a join window additionally holds every request for
one target row at once, so its high-water mark (reported) follows the largest
FoF group, not the catalog. The per-file ``ForestInfo`` table of a forests-HDF5
source is an O(forest-count) term, refused before it is read if it exceeds the
budget.

Measurement
-----------
Every subcommand appends one entry to the ``--record`` JSON file (created on
first use, rewritten atomically): the command line, start time, wall and CPU
seconds, peak RSS, exit code, the git commit and working-tree state, SHA-256
identities of the harness and of any executable it ran, the identity (path,
size, mtime, inode, optionally SHA-256) of every input it read, storage widths
and B/halo of converted output, and every external sort's record width, run
count, merge passes and spilled bytes. Child peak RSS comes from ``wait4`` for
that child alone; the in-process comparator reports the process's lifetime peak.

Subcommands
-----------
``build-dump``
    Build the C dump harness for one ``MODEL``/``SIMULATION`` pair into its own
    directory (``tests/unit/tools/build_topology_dump.sh``).
``dump``
    Run the dump harness in ``--source-payload`` mode on a run file.
``convert``
    Run ``convert_trees.py`` ingest, transpose, write, validate and report into
    a fresh workdir, stopping at the first failing stage.
``exec``
    Run any other command under the same measurement.
``compare``
    Compare a version 3 dataset with a source dump.
``compare-extras``
    Compare a version 3 dataset's selected extra fields with independent
    source extraction.

Exit codes: 0 PASS (or a measured command succeeded), 1 FAIL (a comparison
found a defect, or a measured command failed), 2 usage error or unusable input
(a malformed or truncated dump, a missing source file, an unsupported layout).

Example (a Slice 11-style invocation; paths illustrative)::

    H=scripts/convert/tests/run_generalisation_acceptance.py
    python $H build-dump --record rec.json --model halos-only \\
        --simulation mini-millennium --build-dir /scratch/dump-mm
    python $H dump --record rec.json --tool /scratch/dump-mm/dump_ctrees_topology \\
        --run-file run.yaml --out mm.dump
    python $H convert --record rec.json --workdir /scratch/mm-v3 \\
        --simulation-info simulations/mini-millennium/simulation_info.yaml \\
        -- --source-format lhalo_binary ...
    python $H compare --record rec.json --dataset /scratch/mm-v3/write/attempt_... \\
        --dump mm.dump --source-format lhalo_binary --report mm-compare.json
"""

import argparse
import contextlib
import datetime
import hashlib
import json
import os
import platform
import resource
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

import h5py
import numpy as np
import yaml

REPO_ROOT = Path(__file__).resolve().parents[3]
CONVERT_TREES = REPO_ROOT / "scripts" / "convert" / "convert_trees.py"
BUILD_TOPOLOGY_DUMP = REPO_ROOT / "tests" / "unit" / "tools" / "build_topology_dump.sh"

RECORD_FORMAT = "mimic-generalisation-acceptance-record v1"
REPORT_FORMAT = "mimic-generalisation-comparison v1"
SOURCE_DUMP_VERSION_LINE = "# mimic-source-dump v1"

EXIT_PASS = 0
EXIT_FAIL = 1
EXIT_ERROR = 2

DEFAULT_BUDGET_MB = 512
#: Failure samples kept per check; counts are always exact.
SAMPLE_LIMIT = 10
#: Upper bound on runs merged at once; a larger run count takes another pass.
MAX_FAN_IN = 64

#: Link slot markers. -1 is the format's only null; INVALID marks a converted
#: link that could not be resolved to a row (it can never equal a reference).
NULL = -1
INVALID = -2

LINKS = (
    "Descendant",
    "FirstProgenitor",
    "NextProgenitor",
    "FirstHaloInFOFgroup",
    "NextHaloInFOFgroup",
)
#: The three snapshot-qualified links and their target-snapshot datasets.
QUALIFIED = {
    "Descendant": "DescendantSnapshot",
    "FirstProgenitor": "FirstProgenitorSnapshot",
    "NextProgenitor": "NextProgenitorSnapshot",
}
#: Core float payload (name, components), compared as binary32 bit patterns.
FLOAT_PAYLOAD = (
    ("M_Crit200", 1),
    ("Pos", 3),
    ("Vel", 3),
    ("Spin", 3),
    ("VelDisp", 1),
    ("Vmax", 1),
)
INT_PAYLOAD = ("Len", "MostBoundID")

#: ``mimic-source-dump v1`` column names, restated from dump_ctrees_topology.c.
DUMP_LINK_COLUMNS = {
    "Descendant": "descendant",
    "FirstProgenitor": "first_progenitor",
    "NextProgenitor": "next_progenitor",
    "FirstHaloInFOFgroup": "first_fof",
    "NextHaloInFOFgroup": "next_fof",
}
DUMP_TARGET_SNAP_COLUMNS = {
    "Descendant": "descendant_snap",
    "FirstProgenitor": "first_progenitor_snap",
    "NextProgenitor": "next_progenitor_snap",
}
DUMP_FLOAT_COLUMNS = {
    "M_Crit200": ("m_crit200",),
    "Pos": ("pos_x", "pos_y", "pos_z"),
    "Vel": ("vel_x", "vel_y", "vel_z"),
    "Spin": ("spin_x", "spin_y", "spin_z"),
    "VelDisp": ("vel_disp",),
    "Vmax": ("vmax",),
}
DUMP_INT_COLUMNS = (
    ("forest_index", "rank", "partition", "unit", "snapnum")
    + tuple(DUMP_LINK_COLUMNS.values())
    + tuple(DUMP_TARGET_SNAP_COLUMNS.values())
    + ("len", "most_bound_id")
)
DUMP_REQUIRED_COLUMNS = DUMP_INT_COLUMNS + tuple(
    c for cols in DUMP_FLOAT_COLUMNS.values() for c in cols
)

SOURCE_FORMATS = ("lhalo_binary", "consistent_trees_hdf5", "consistent_trees_ascii")
#: Formats whose forest (an L-Halo tree, a forests-HDF5 ForestInfo row) is one
#: inventory unit, so ascending (ForestIndex, rank) *is* SourceHaloID order.
UNIT_FOREST_FORMATS = ("lhalo_binary", "consistent_trees_hdf5")

#: Declarable extra-field types (the property generator's vocabulary), restated
#: here rather than imported: storage dtype and component count.
EXTRA_TYPES = {
    "int": ("<i4", 1),
    "long long": ("<i8", 1),
    "float": ("<f4", 1),
    "double": ("<f8", 1),
    "vec3_int": ("<i4", 3),
    "vec3_float": ("<f4", 3),
}


class AcceptanceError(Exception):
    """Unusable input or harness misuse; reported with exit code 2."""


class DatasetDefect(Exception):
    """A converted dataset too malformed to compare; reported as a FAIL."""


# ===========================================================================
# Findings
# ===========================================================================


class Findings:
    """Named checks, each with an exact failure count and bounded samples."""

    def __init__(self):
        self.checks = {}

    def declare(self, name, description):
        self.checks.setdefault(
            name, {"description": description, "compared": 0, "failures": 0, "samples": []}
        )

    def compared(self, name, count):
        self.checks[name]["compared"] += int(count)

    def fail(self, name, count, samples=()):
        check = self.checks[name]
        check["failures"] += int(count)
        room = SAMPLE_LIMIT - len(check["samples"])
        if room > 0:
            check["samples"].extend(str(sample) for sample in list(samples)[:room])

    def not_applicable(self, name, reason):
        self.checks[name]["not_applicable"] = reason

    @property
    def failed(self):
        return sorted(name for name, check in self.checks.items() if check["failures"])

    def as_dict(self):
        return self.checks


# ===========================================================================
# Bounded external sort and merge join over fixed-width records
# ===========================================================================


def _last_key(rows, key):
    return tuple(int(rows[field][-1]) for field in key)


def _key_masks(rows, key, bound):
    """(rows < bound, rows == bound), lexicographically over int64 ``key``."""
    less = np.zeros(len(rows), dtype=bool)
    equal = np.ones(len(rows), dtype=bool)
    for field, value in zip(key, bound):
        column = rows[field]
        value = np.int64(value)
        less |= equal & (column < value)
        equal &= column == value
    return less, equal


def _count_through(rows, key, bound, strict):
    """How many leading rows of sorted ``rows`` are < (strict) or <= ``bound``."""
    less, equal = _key_masks(rows, key, bound)
    return int(np.count_nonzero(less if strict else less | equal))


def _sort_rows(rows, key):
    return rows[np.lexsort([rows[field] for field in reversed(key)])]


class _RunReader:
    """Bounded sequential reader of one sorted spill run."""

    def __init__(self, path, dtype, rows, block_rows):
        self._handle = open(path, "rb")
        self._dtype = dtype
        self._remaining = int(rows)
        self._block_rows = int(block_rows)
        self.buf = np.empty(0, dtype=dtype)

    @property
    def done(self):
        return self._remaining == 0

    def refill(self):
        if len(self.buf) or self.done:
            return
        count = min(self._block_rows, self._remaining)
        block = np.fromfile(self._handle, dtype=self._dtype, count=count)
        if len(block) != count:
            raise AcceptanceError(
                "spill run {} is short: read {} of {} rows".format(
                    self._handle.name, len(block), count
                )
            )
        self._remaining -= count
        self.buf = block

    def close(self):
        self._handle.close()


class ExternalSorter:
    """Sort fixed-width records by int64 ``key`` fields within ``budget_bytes``.

    Records are buffered in runs of at most ``run_rows`` (half the budget,
    allowing for the lexsort permutation and key temporaries), each run is
    sorted and spilled to this sorter's own temporary directory, and the runs
    are merged ``fan_in`` at a time in blocks of ``block_rows`` per run, with
    extra passes when there are more runs than the fan-in. The merge takes, in
    each step, every buffered row not greater than the smallest last-buffered
    key of any unfinished run, so the output is exactly sorted (ties keep no
    particular order) while each run holds at most one block. A budget too
    small for two records is rounded up to two. Use as a context manager, or
    call :meth:`close`, to remove the spill directory.
    """

    def __init__(self, dtype, key, budget_bytes, spill_dir, label):
        self.dtype = np.dtype(dtype)
        self.key = tuple(key)
        for field in self.key:
            if self.dtype[field] != np.dtype("<i8"):
                raise AcceptanceError("sort key {!r} of {} must be int64".format(field, label))
        per_row = self.dtype.itemsize + 8 * (len(self.key) + 1)
        self.run_rows = max(2, int(budget_bytes) // (2 * per_row))
        self.fan_in = max(2, min(MAX_FAN_IN, self.run_rows // 4))
        self.block_rows = max(1, self.run_rows // (self.fan_in + 1))
        self._dir = Path(tempfile.mkdtemp(prefix="acceptance_sort_", dir=spill_dir))
        self._buffer = np.empty(self.run_rows, dtype=self.dtype)
        self._fill = 0
        self._runs = []
        self._next_run = 0
        self._sealed = False
        self.stats = {
            "label": label,
            "record_bytes": self.dtype.itemsize,
            "key": list(self.key),
            "budget_bytes": int(budget_bytes),
            "run_rows": self.run_rows,
            "fan_in": self.fan_in,
            "block_rows": self.block_rows,
            "records": 0,
            "runs": 0,
            "merge_passes": 0,
            "spilled_bytes": 0,
        }

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()
        return False

    def close(self):
        self._buffer = None
        shutil.rmtree(self._dir, ignore_errors=True)

    def add(self, rows):
        if self._sealed:
            raise AcceptanceError("{}: add() after sorted_blocks()".format(self.stats["label"]))
        if rows.dtype != self.dtype:
            raise AcceptanceError(
                "{}: record dtype {} != {}".format(self.stats["label"], rows.dtype, self.dtype)
            )
        self.stats["records"] += len(rows)
        position = 0
        while position < len(rows):
            take = min(self.run_rows - self._fill, len(rows) - position)
            self._buffer[self._fill : self._fill + take] = rows[position : position + take]
            self._fill += take
            position += take
            if self._fill == self.run_rows:
                self._spill_buffer()

    def _new_run_path(self):
        path = self._dir / "run_{:06d}.bin".format(self._next_run)
        self._next_run += 1
        return path

    def _spill_buffer(self):
        rows = _sort_rows(self._buffer[: self._fill], self.key)
        path = self._new_run_path()
        with open(path, "wb") as handle:
            rows.tofile(handle)
        self._runs.append((path, len(rows)))
        self.stats["runs"] += 1
        self.stats["spilled_bytes"] += rows.nbytes
        self._fill = 0

    def _merge(self, runs):
        readers = [_RunReader(path, self.dtype, rows, self.block_rows) for path, rows in runs]
        try:
            while True:
                for reader in readers:
                    reader.refill()
                live = [reader for reader in readers if len(reader.buf)]
                if not live:
                    return
                unfinished = [reader for reader in live if not reader.done]
                if unfinished:
                    bound = min(_last_key(reader.buf, self.key) for reader in unfinished)
                    parts = []
                    for reader in live:
                        count = _count_through(reader.buf, self.key, bound, strict=False)
                        parts.append(reader.buf[:count])
                        reader.buf = reader.buf[count:]
                else:
                    parts = [reader.buf for reader in live]
                    for reader in live:
                        reader.buf = reader.buf[:0]
                yield _sort_rows(np.concatenate(parts), self.key)
        finally:
            for reader in readers:
                reader.close()

    def sorted_blocks(self):
        """Yield every added record, in ascending key order, in bounded blocks."""
        if self._sealed:
            raise AcceptanceError("{}: sorted_blocks() called twice".format(self.stats["label"]))
        self._sealed = True
        if not self._runs:
            rows = _sort_rows(self._buffer[: self._fill], self.key)
            self._buffer = None
            for start in range(0, len(rows), self.run_rows):
                yield rows[start : start + self.run_rows]
            return
        if self._fill:
            self._spill_buffer()
        self._buffer = None
        runs = list(self._runs)
        while len(runs) > self.fan_in:
            merged = []
            for start in range(0, len(runs), self.fan_in):
                group = runs[start : start + self.fan_in]
                if len(group) == 1:
                    merged.append(group[0])
                    continue
                path = self._new_run_path()
                rows = 0
                with open(path, "wb") as handle:
                    for block in self._merge(group):
                        block.tofile(handle)
                        rows += len(block)
                        self.stats["spilled_bytes"] += block.nbytes
                for run_path, _rows in group:
                    run_path.unlink()
                merged.append((path, rows))
            runs = merged
            self.stats["merge_passes"] += 1
        self.stats["merge_passes"] += 1
        yield from self._merge(runs)


class _Stream:
    """A sorted block iterator with a look-ahead buffer."""

    def __init__(self, blocks, dtype):
        self._blocks = iter(blocks)
        self.buf = np.empty(0, dtype=dtype)
        self.done = False

    def pull(self):
        for block in self._blocks:
            if len(block):
                self.buf = block if not len(self.buf) else np.concatenate([self.buf, block])
                return
        self.done = True


def _match_window(many, many_key, one, one_key):
    """For each ``many`` row, the index of the ``one`` row with an equal key, or -1.

    Both windows are sorted by their keys and ``one`` has unique keys. A
    stable lexsort of the concatenated keys, with ``one`` rows ordered before
    ``many`` rows of the same key, puts each ``one`` row immediately ahead of
    its matches; a running maximum of the ``one`` positions then names each
    ``many`` row's candidate, which is kept only if its key is equal.
    """
    n_one = len(one)
    tag = np.concatenate([np.zeros(n_one, dtype=np.int8), np.ones(len(many), dtype=np.int8)])
    position = np.concatenate(
        [np.arange(n_one, dtype=np.int64), np.arange(len(many), dtype=np.int64)]
    )
    columns = [
        np.concatenate([one[one_field], many[many_field]])
        for one_field, many_field in zip(one_key, many_key)
    ]
    order = np.lexsort([tag] + columns[::-1])
    is_one = tag[order] == 0
    candidate = np.maximum.accumulate(np.where(is_one, position[order], -1))
    many_position = position[order][~is_one]
    candidate = candidate[~is_one]
    valid = candidate >= 0
    safe = np.where(valid, candidate, 0)
    for one_field, many_field in zip(one_key, many_key):
        if n_one:
            valid &= one[one_field][safe] == many[many_field][many_position]
    match = np.full(len(many), -1, dtype=np.int64)
    match[many_position] = np.where(valid, candidate, -1)
    one_matched = np.zeros(n_one, dtype=bool)
    one_matched[match[match >= 0]] = True
    return match, one_matched


def lookup_join(many_blocks, many_key, many_dtype, one_blocks, one_key, one_dtype, stats=None):
    """Merge-join two key-sorted block streams in bounded windows.

    ``one`` has unique keys; ``many`` may repeat a key. Yields
    ``(many_rows, match, one_rows, one_matched)`` per window, where ``match``
    indexes ``one_rows`` (-1 for no match) and ``one_matched`` flags the
    ``one_rows`` some ``many`` row matched. Every row of either stream appears
    in exactly one window, and all rows sharing a key share a window: a window
    ends strictly before the last buffered ``many`` key (more rows of that key
    may follow), or at the last buffered ``one`` key, whichever is smaller.
    ``stats["max_window_rows"]`` records the largest window.
    """
    many = _Stream(many_blocks, many_dtype)
    one = _Stream(one_blocks, one_dtype)
    while True:
        if not many.done and not len(many.buf):
            many.pull()
            continue
        if not one.done and not len(one.buf):
            one.pull()
            continue
        if many.done and one.done and not len(many.buf) and not len(one.buf):
            return
        bound = None
        strict = False
        if not many.done:
            bound, strict = _last_key(many.buf, many_key), True
        if not one.done:
            last = _last_key(one.buf, one_key)
            if bound is None or last < bound:
                bound, strict = last, False
        if bound is None:
            n_many, n_one = len(many.buf), len(one.buf)
        else:
            n_many = _count_through(many.buf, many_key, bound, strict)
            n_one = _count_through(one.buf, one_key, bound, strict)
            if n_many == 0 and n_one == 0:
                # every buffered many row shares the bound key: read further
                many.pull()
                continue
        many_rows, one_rows = many.buf[:n_many], one.buf[:n_one]
        many.buf, one.buf = many.buf[n_many:], one.buf[n_one:]
        if stats is not None:
            stats["max_window_rows"] = max(
                stats.get("max_window_rows", 0), len(many_rows) + len(one_rows)
            )
        match, one_matched = _match_window(many_rows, many_key, one_rows, one_key)
        yield many_rows, match, one_rows, one_matched


def dedupe(blocks, key, findings, check, describe):
    """Drop repeated keys from a sorted stream, recording each as a ``check`` failure."""
    previous = None
    for block in blocks:
        if not len(block):
            continue
        duplicate = np.zeros(len(block), dtype=bool)
        same = np.ones(len(block) - 1, dtype=bool)
        for field in key:
            same &= block[field][1:] == block[field][:-1]
        duplicate[1:] = same
        if previous is not None:
            duplicate[0] = tuple(int(block[field][0]) for field in key) == previous
        previous = _last_key(block, key)
        if duplicate.any():
            findings.fail(
                check, np.count_nonzero(duplicate), [describe(row) for row in block[duplicate]]
            )
            block = block[~duplicate]
        yield block


# ===========================================================================
# Record layouts
# ===========================================================================


def _halo_dtype():
    """The comparison record both sides are reduced to."""
    fields = [
        ("ForestIndex", "<i8"),
        ("HaloRankInForest", "<i8"),
        ("SnapNum", "<i8"),
        ("FileSnapshot", "<i8"),
        ("SourceHaloID", "<i8"),
    ]
    for link in LINKS:
        fields += [(link + "_forest", "<i8"), (link + "_rank", "<i8")]
    for link in QUALIFIED:
        fields.append((link + "_snap", "<i8"))
    fields += [(name, "<i8") for name in INT_PAYLOAD]
    for name, components in FLOAT_PAYLOAD:
        fields.append((name, "<u4") if components == 1 else (name, "<u4", (components,)))
    return np.dtype(fields)


HALO_DTYPE = _halo_dtype()
HALO_KEY = ("ForestIndex", "HaloRankInForest")
#: One converted link awaiting resolution: its target coordinate and owner.
REQUEST_DTYPE = np.dtype(
    [("tsnap", "<i8"), ("trow", "<i8"), ("sforest", "<i8"), ("srank", "<i8"), ("link", "<i8")]
)
#: One converted row's position and identity.
IDENTITY_DTYPE = np.dtype([("snap", "<i8"), ("row", "<i8"), ("forest", "<i8"), ("rank", "<i8")])
#: One converted link resolved into the reference key space.
RESOLVED_DTYPE = np.dtype(
    [("sforest", "<i8"), ("srank", "<i8"), ("link", "<i8"), ("tforest", "<i8"), ("trank", "<i8")]
)


def _describe_key(row):
    return "(ForestIndex={}, HaloRankInForest={})".format(
        int(row["ForestIndex"]), int(row["HaloRankInForest"])
    )


# ===========================================================================
# The reference: mimic-source-dump v1
# ===========================================================================


_HEX = np.full(256, 255, dtype=np.uint8)
_HEX[np.frombuffer(b"0123456789", dtype=np.uint8)] = np.arange(10, dtype=np.uint8)
_HEX[np.frombuffer(b"abcdef", dtype=np.uint8)] = np.arange(10, 16, dtype=np.uint8)
_HEX[np.frombuffer(b"ABCDEF", dtype=np.uint8)] = np.arange(10, 16, dtype=np.uint8)


def parse_hex32(tokens, where):
    """Exact uint32 values of 8-digit hexadecimal tokens, vectorised."""
    tokens = np.asarray(tokens)
    if len(tokens) == 0:
        return np.zeros(0, dtype=np.uint32)
    if not np.all(np.char.str_len(tokens) == 8):
        raise AcceptanceError("{}: binary32 bit patterns must be 8 hex digits".format(where))
    digits = _HEX[np.frombuffer(tokens.astype("S8").tobytes(), dtype=np.uint8).reshape(-1, 8)]
    if np.any(digits == 255):
        raise AcceptanceError("{}: non-hexadecimal binary32 bit pattern".format(where))
    value = np.zeros(len(tokens), dtype=np.uint32)
    for position in range(8):
        value |= digits[:, position].astype(np.uint32) << np.uint32(4 * (7 - position))
    return value


class SourceDump:
    """A ``mimic-source-dump v1`` file, read in bounded blocks.

    The header is exactly four lines -- the version marker, ``# reader <name>
    partition_model <model>``, ``# columns ...`` and one ``#`` notes line --
    and the file ends with ``# end rows N forests F`` and nothing after it.
    Columns are located by name: every required column must be present, and
    any additional column is ignored. A wrong marker, a missing column, a
    ragged or non-numeric row, a comment inside the data, a missing trailer or
    a trailer disagreeing with the rows read raises :class:`AcceptanceError`:
    a broken reference is unusable evidence, not a converter failure.
    """

    def __init__(self, path):
        self.path = Path(path)
        with open(self.path, "r", encoding="ascii") as handle:
            header = [handle.readline().rstrip("\n") for _ in range(4)]
        if header[0] != SOURCE_DUMP_VERSION_LINE:
            raise AcceptanceError(
                "{}: first line {!r} is not {!r}".format(path, header[0], SOURCE_DUMP_VERSION_LINE)
            )
        reader = header[1].split()
        if len(reader) != 5 or reader[:2] != ["#", "reader"] or reader[3] != "partition_model":
            raise AcceptanceError("{}: malformed reader line {!r}".format(path, header[1]))
        self.reader = reader[2]
        self.partition_model = reader[4]
        columns = header[2].split()
        if columns[:2] != ["#", "columns"]:
            raise AcceptanceError("{}: malformed columns line {!r}".format(path, header[2]))
        self.columns = columns[2:]
        if len(set(self.columns)) != len(self.columns):
            raise AcceptanceError("{}: repeated column names".format(path))
        missing = [name for name in DUMP_REQUIRED_COLUMNS if name not in self.columns]
        if missing:
            raise AcceptanceError("{}: missing required column(s) {}".format(path, missing))
        if not header[3].startswith("#"):
            raise AcceptanceError("{}: header has fewer than four lines".format(path))
        self.ignored_columns = [name for name in self.columns if name not in DUMP_REQUIRED_COLUMNS]
        self.rows = None
        self.forests = None

    def blocks(self, block_rows):
        """Yield :data:`HALO_DTYPE` blocks in file order; check the trailer at the end."""
        index = {name: position for position, name in enumerate(self.columns)}
        width = len(self.columns)
        rows_read = 0
        trailer = None
        line_number = 4
        with open(self.path, "r", encoding="ascii") as handle:
            for _ in range(4):
                handle.readline()
            lines = []
            first_line = line_number + 1
            for line in handle:
                line_number += 1
                if trailer is not None:
                    raise AcceptanceError(
                        "{}:{}: content after a '#' line; the only comment allowed after the "
                        "header is the final '# end' trailer".format(self.path, line_number)
                    )
                if line.startswith("#"):
                    trailer = (line_number, line.split())
                    continue
                lines.append(line)
                if len(lines) == block_rows:
                    yield self._parse(lines, index, width, first_line)
                    rows_read += len(lines)
                    lines = []
                    first_line = line_number + 1
            if lines:
                yield self._parse(lines, index, width, first_line)
                rows_read += len(lines)
        if trailer is None:
            raise AcceptanceError("{}: no '# end' trailer (truncated dump)".format(self.path))
        where, tokens = trailer
        if len(tokens) != 6 or tokens[:3] != ["#", "end", "rows"] or tokens[4] != "forests":
            raise AcceptanceError(
                "{}:{}: comment inside the data or malformed trailer {!r}".format(
                    self.path, where, " ".join(tokens)
                )
            )
        declared = int(tokens[3])
        if declared != rows_read:
            raise AcceptanceError(
                "{}: trailer declares {} rows but {} were read".format(
                    self.path, declared, rows_read
                )
            )
        self.rows = rows_read
        self.forests = int(tokens[5])

    def _parse(self, lines, index, width, first_line):
        split = [line.split() for line in lines]
        for offset, tokens in enumerate(split):
            if len(tokens) != width:
                raise AcceptanceError(
                    "{}:{}: {} fields, header declares {}".format(
                        self.path, first_line + offset, len(tokens), width
                    )
                )
        table = np.array(split, dtype=str).reshape(len(lines), width)
        where = "{}:{}-{}".format(self.path, first_line, first_line + len(lines) - 1)

        def integer(name):
            try:
                return table[:, index[name]].astype(np.int64)
            except (ValueError, OverflowError) as error:
                raise AcceptanceError("{}: column {}: {}".format(where, name, error)) from None

        out = np.zeros(len(lines), dtype=HALO_DTYPE)
        forest = integer("forest_index")
        out["ForestIndex"] = forest
        out["HaloRankInForest"] = integer("rank")
        out["SnapNum"] = integer("snapnum")
        out["FileSnapshot"] = out["SnapNum"]
        out["SourceHaloID"] = -1
        for link, column in DUMP_LINK_COLUMNS.items():
            rank = integer(column)
            out[link + "_rank"] = rank
            out[link + "_forest"] = np.where(
                rank >= 0, forest, np.where(rank == NULL, NULL, INVALID)
            )
        for link, column in DUMP_TARGET_SNAP_COLUMNS.items():
            out[link + "_snap"] = integer(column)
        out["Len"] = integer("len")
        out["MostBoundID"] = integer("most_bound_id")
        for name, columns in DUMP_FLOAT_COLUMNS.items():
            bits = [parse_hex32(table[:, index[column]], where) for column in columns]
            out[name] = bits[0] if len(bits) == 1 else np.stack(bits, axis=1)
        return out


def reference_blocks(dump_blocks, source_format, findings):
    """Check a (ForestIndex, rank)-sorted, de-duplicated dump; assign expected SourceHaloID.

    Ranks must be dense from 0 within every forest (``dump_integrity``), and a
    qualified link naming a target rank must name a target snapshot. For the
    unit-forest formats ascending (ForestIndex, rank) is the inventory order,
    so the expected ``SourceHaloID`` of the k-th row is k (from 1); for ASCII
    that order differs from the inventory's and the check does not apply.
    """
    assign = source_format in UNIT_FOREST_FORMATS
    position = 0
    carry_forest, carry_next = None, 0
    for block in dump_blocks:
        n = len(block)
        forest = block["ForestIndex"]
        starts = np.ones(n, dtype=bool)
        starts[1:] = forest[1:] != forest[:-1]
        index = np.arange(n, dtype=np.int64)
        group_start = np.maximum.accumulate(np.where(starts, index, 0))
        expected = index - group_start
        if carry_forest is not None and n and int(forest[0]) == carry_forest:
            # the block's first group continues the previous block's last forest
            expected[group_start == 0] += carry_next
        bad = block["HaloRankInForest"] != expected
        if bad.any():
            findings.fail(
                "dump_integrity",
                np.count_nonzero(bad),
                [
                    "{} breaks within-forest rank density".format(_describe_key(row))
                    for row in block[bad]
                ],
            )
        for link in QUALIFIED:
            orphan = (block[link + "_rank"] >= 0) != (block[link + "_snap"] >= 0)
            if orphan.any():
                findings.fail(
                    "dump_integrity",
                    np.count_nonzero(orphan),
                    [
                        "{} {} rank/snapshot disagree on nullness".format(_describe_key(row), link)
                        for row in block[orphan]
                    ],
                )
        for link in LINKS:
            invalid = block[link + "_forest"] == INVALID
            if invalid.any():
                findings.fail(
                    "dump_integrity",
                    np.count_nonzero(invalid),
                    [
                        "{} {} holds an invalid negative link".format(_describe_key(row), link)
                        for row in block[invalid]
                    ],
                )
        findings.compared("dump_integrity", n)
        if n:
            carry_forest = int(forest[-1])
            carry_next = int(expected[-1]) + 1
        if assign:
            block = block.copy()
            block["SourceHaloID"] = np.arange(position + 1, position + n + 1, dtype=np.int64)
        position += n
        yield block


# ===========================================================================
# The converted side: horizontal-HDF5 version 3
# ===========================================================================


_INT_DATASETS = (
    ("SourceHaloID", "ForestIndex", "HaloRankInForest", "SnapNum")
    + LINKS
    + tuple(QUALIFIED.values())
    + INT_PAYLOAD
)


class V3Dataset:
    """A version 3 dataset directory, read with plain h5py in bounded blocks.

    Only the datasets the comparison needs are read; any other ``/halos``
    dataset (a selected extra, or anything else) is ignored. Structural
    problems that make a comparison meaningless -- a wrong format version, a
    header snapshot disagreeing with its file name, a missing required
    dataset, a ragged ``/halos`` group, mixed source formats -- raise
    :class:`DatasetDefect`. A float payload dataset that is not little-endian
    binary32 (the vertical readers' ``float``) is a failure of that field's
    check and is excluded from bit comparison.
    """

    def __init__(self, directory, findings=None):
        self.directory = Path(directory)
        self.files = []
        source_formats = set()
        paths = sorted(self.directory.glob("snapshot_*.h5"))
        if not paths:
            raise DatasetDefect("{}: no snapshot_*.h5 files".format(self.directory))
        self.float_excluded = set()
        for path in paths:
            try:
                number = int(path.stem.split("_", 1)[1])
            except ValueError:
                raise DatasetDefect("{}: unparsable snapshot file name".format(path)) from None
            with h5py.File(path, "r") as handle:
                if "header" not in handle or "halos" not in handle:
                    raise DatasetDefect("{}: missing /header or /halos".format(path))
                attrs = handle["header"].attrs
                if int(attrs.get("format_version", -1)) != 3:
                    raise DatasetDefect(
                        "{}: format_version {} is not 3".format(path, attrs.get("format_version"))
                    )
                if int(attrs.get("snapshot_number", -1)) != number:
                    raise DatasetDefect(
                        "{}: header snapshot_number {} disagrees with the file name".format(
                            path, attrs.get("snapshot_number")
                        )
                    )
                source = attrs.get("source_format")
                source_formats.add(source.decode() if isinstance(source, bytes) else str(source))
                halos = handle["halos"]
                required = _INT_DATASETS + tuple(name for name, _n in FLOAT_PAYLOAD)
                missing = [name for name in required if name not in halos]
                if missing:
                    raise DatasetDefect("{}: missing /halos dataset(s) {}".format(path, missing))
                rows = halos["SourceHaloID"].shape[0]
                for name in _INT_DATASETS:
                    dataset = halos[name]
                    if dataset.dtype.kind not in "iu" or dataset.shape != (rows,):
                        raise DatasetDefect(
                            "{}: /halos/{} is {} {}, not an integer [{}]".format(
                                path, name, dataset.dtype, dataset.shape, rows
                            )
                        )
                for name, components in FLOAT_PAYLOAD:
                    dataset = halos[name]
                    shape = (rows,) if components == 1 else (rows, components)
                    if dataset.shape != shape:
                        raise DatasetDefect(
                            "{}: /halos/{} shape {} != {}".format(path, name, dataset.shape, shape)
                        )
                    if dataset.dtype != np.dtype("<f4"):
                        self.float_excluded.add(name)
                        if findings is not None:
                            findings.fail(
                                "payload_" + name,
                                rows,
                                [
                                    "{}: /halos/{} is {}, not <f4 binary32".format(
                                        path.name, name, dataset.dtype
                                    )
                                ],
                            )
            self.files.append((number, path, rows))
        if len(source_formats) != 1:
            raise DatasetDefect("mixed source_format values {}".format(sorted(source_formats)))
        self.source_format = source_formats.pop()
        self.total_rows = sum(rows for _number, _path, rows in self.files)

    def _slices(self, block_rows):
        for number, path, rows in self.files:
            with h5py.File(path, "r") as handle:
                halos = handle["halos"]
                for start in range(0, rows, block_rows):
                    yield number, halos, start, min(rows, start + block_rows)

    def identity_blocks(self, block_rows):
        """(snap, row, forest, rank) for every row, ascending by (snap, row)."""
        for number, halos, start, stop in self._slices(block_rows):
            out = np.empty(stop - start, dtype=IDENTITY_DTYPE)
            out["snap"] = number
            out["row"] = np.arange(start, stop, dtype=np.int64)
            out["forest"] = halos["ForestIndex"][start:stop]
            out["rank"] = halos["HaloRankInForest"][start:stop]
            yield out

    def raw_blocks(self, block_rows):
        """Every row as ``(snapshot, row, columns)``: the columns the comparison reads."""
        for number, halos, start, stop in self._slices(block_rows):
            columns = {name: halos[name][start:stop].astype(np.int64) for name in _INT_DATASETS}
            for name, _components in FLOAT_PAYLOAD:
                if name not in self.float_excluded:
                    columns[name] = halos[name][start:stop].view("<u4")
            yield number, np.arange(start, stop, dtype=np.int64), columns

    def extra_blocks(self, names, block_rows):
        """SourceHaloID and the raw arrays of ``names`` (checked by the caller)."""
        for _number, halos, start, stop in self._slices(block_rows):
            ids = halos["SourceHaloID"][start:stop].astype(np.int64)
            yield ids, {name: halos[name][start:stop] for name in names}

    def dataset_descriptions(self, names):
        """(dtype, shape-after-rows) of each named dataset, identical in every file."""
        found = {}
        for _number, path, _rows in self.files:
            with h5py.File(path, "r") as handle:
                for name in names:
                    if name not in handle["halos"]:
                        raise DatasetDefect("{}: no /halos/{}".format(path, name))
                    dataset = handle["halos"][name]
                    description = (dataset.dtype, dataset.shape[1:])
                    if found.setdefault(name, description) != description:
                        raise DatasetDefect(
                            "{}: /halos/{} is {} here but {} elsewhere".format(
                                path, name, description, found[name]
                            )
                        )
        return found


def converter_halo_blocks(raw_blocks, requests, findings):
    """Reduce raw converted rows to :data:`HALO_DTYPE`; queue their links for resolution.

    ``raw_blocks`` yields ``(snapshot, rows, columns)``. Each non-null link is
    added to ``requests`` (an :class:`ExternalSorter` of
    :data:`REQUEST_DTYPE`) with its target coordinate: the target-snapshot
    column's value for the three qualified links, the row's own snapshot for
    the two FoF links. A null link must be -1 with, for the qualified links,
    a -1 target snapshot; any other negative value, or a snapshot column
    disagreeing about nullness, is a ``converter_link_encoding`` failure and
    leaves the slot :data:`INVALID`.
    """
    for snapshot, rows, columns in raw_blocks:
        n = len(rows)
        out = np.zeros(n, dtype=HALO_DTYPE)
        forest = columns["ForestIndex"]
        rank = columns["HaloRankInForest"]
        out["ForestIndex"] = forest
        out["HaloRankInForest"] = rank
        out["SnapNum"] = columns["SnapNum"]
        out["FileSnapshot"] = snapshot
        out["SourceHaloID"] = columns["SourceHaloID"]
        for name in INT_PAYLOAD:
            out[name] = columns[name]
        for name, _components in FLOAT_PAYLOAD:
            if name in columns:
                out[name] = columns[name]
        findings.compared("converter_link_encoding", n)
        for link_id, link in enumerate(LINKS):
            value = columns[link]
            if link in QUALIFIED:
                target_snap = columns[QUALIFIED[link]]
                out[link + "_snap"] = target_snap
            else:
                target_snap = np.full(n, snapshot, dtype=np.int64)
            null = value == NULL
            bad = (value < NULL) | (value >= 0) & (target_snap < 0)
            if link in QUALIFIED:
                bad |= null & (target_snap != NULL)
            if bad.any():
                findings.fail(
                    "converter_link_encoding",
                    np.count_nonzero(bad),
                    [
                        "snapshot {} row {} {}={} target snapshot {}".format(
                            snapshot, int(r), link, int(v), int(t)
                        )
                        for r, v, t in zip(rows[bad], value[bad], target_snap[bad])
                    ],
                )
            out[link + "_forest"] = np.where(null & ~bad, NULL, INVALID)
            out[link + "_rank"] = out[link + "_forest"]
            want = (value >= 0) & ~bad
            request = np.empty(np.count_nonzero(want), dtype=REQUEST_DTYPE)
            request["tsnap"] = target_snap[want]
            request["trow"] = value[want]
            request["sforest"] = forest[want]
            request["srank"] = rank[want]
            request["link"] = link_id
            requests.add(request)
        yield out


def resolve_requests(request_blocks, identity_blocks, resolved, findings, stats):
    """Resolve each queued link to its target's (ForestIndex, rank).

    Both streams ascend by target coordinate / (snapshot, row). A request
    whose coordinate names no converted row is a ``converter_link_targets``
    failure and resolves to :data:`INVALID`.
    """
    for many, match, one, _matched in lookup_join(
        request_blocks,
        ("tsnap", "trow"),
        REQUEST_DTYPE,
        identity_blocks,
        ("snap", "row"),
        IDENTITY_DTYPE,
        stats,
    ):
        findings.compared("converter_link_targets", len(many))
        out = np.empty(len(many), dtype=RESOLVED_DTYPE)
        for field in ("sforest", "srank", "link"):
            out[field] = many[field]
        found = match >= 0
        safe = np.where(found, match, 0)
        out["tforest"] = np.where(found, one["forest"][safe] if len(one) else INVALID, INVALID)
        out["trank"] = np.where(found, one["rank"][safe] if len(one) else INVALID, INVALID)
        if not found.all():
            lost = many[~found]
            findings.fail(
                "converter_link_targets",
                len(lost),
                [
                    "({}, {}) {} targets snapshot {} row {}, which does not exist".format(
                        int(row["sforest"]),
                        int(row["srank"]),
                        LINKS[int(row["link"])],
                        int(row["tsnap"]),
                        int(row["trow"]),
                    )
                    for row in lost
                ],
            )
        resolved.add(out)


def complete_links(resolved_blocks, converted_blocks, stats):
    """Write each resolved link into its owner's slot; yield completed converted blocks."""
    for many, match, one, _matched in lookup_join(
        resolved_blocks,
        ("sforest", "srank"),
        RESOLVED_DTYPE,
        converted_blocks,
        HALO_KEY,
        HALO_DTYPE,
        stats,
    ):
        if np.any(match < 0):
            raise AcceptanceError("internal: a resolved link lost its owning converted row")
        out = one.copy()
        for link_id, link in enumerate(LINKS):
            select = many["link"] == link_id
            out[link + "_forest"][match[select]] = many["tforest"][select]
            out[link + "_rank"][match[select]] = many["trank"][select]
        if len(out):
            yield out


# ===========================================================================
# The comparison
# ===========================================================================


def _declare_checks(findings, source_format):
    findings.declare("dump_integrity", "reference ranks dense per forest, links well formed")
    findings.declare("duplicate_reference_rows", "no (ForestIndex, rank) appears twice in the dump")
    findings.declare(
        "duplicate_converted_rows", "no (ForestIndex, rank) appears twice in the dataset"
    )
    findings.declare(
        "converter_link_encoding", "null links are exactly -1 with a -1 target snapshot"
    )
    findings.declare("converter_link_targets", "every converted link names an existing row")
    findings.declare("row_coverage", "the same halos on both sides: none dropped, none extra")
    findings.declare("snapnum", "SnapNum and file snapshot equal the reference snapshot")
    for link in LINKS:
        findings.declare("link_" + link, "{} resolves to the reference's target".format(link))
    for link in QUALIFIED:
        findings.declare(
            "target_snapshot_" + link,
            "{} equals the reference target snapshot".format(QUALIFIED[link]),
        )
    for name in INT_PAYLOAD:
        findings.declare("payload_" + name, "{} equals the reference exactly".format(name))
    for name, _components in FLOAT_PAYLOAD:
        findings.declare("payload_" + name, "{} binary32 bits equal the reference".format(name))
    findings.declare("source_halo_id", "SourceHaloID equals the inventory prefix sum")
    if source_format not in UNIT_FOREST_FORMATS:
        findings.not_applicable(
            "source_halo_id",
            "{} forest enumeration is not the inventory order".format(source_format),
        )


def _compare_window(reference, converted, source_format, float_excluded, findings):
    n = len(reference)

    def check(name, bad, describe):
        findings.compared(name, n)
        if bad.any():
            findings.fail(
                name,
                np.count_nonzero(bad),
                [describe(r, c) for r, c in zip(reference[bad], converted[bad])],
            )

    check(
        "snapnum",
        (reference["SnapNum"] != converted["SnapNum"])
        | (reference["SnapNum"] != converted["FileSnapshot"]),
        lambda r, c: "{}: reference {} converted SnapNum {} in file {}".format(
            _describe_key(r), int(r["SnapNum"]), int(c["SnapNum"]), int(c["FileSnapshot"])
        ),
    )
    for link in LINKS:
        forest, rank = link + "_forest", link + "_rank"
        check(
            "link_" + link,
            (reference[forest] != converted[forest]) | (reference[rank] != converted[rank]),
            lambda r, c, f=forest, k=rank: "{}: reference ({}, {}) converted ({}, {})".format(
                _describe_key(r), int(r[f]), int(r[k]), int(c[f]), int(c[k])
            ),
        )
    for link in QUALIFIED:
        field = link + "_snap"
        check(
            "target_snapshot_" + link,
            reference[field] != converted[field],
            lambda r, c, f=field: "{}: reference {} converted {}".format(
                _describe_key(r), int(r[f]), int(c[f])
            ),
        )
    for name in INT_PAYLOAD:
        check(
            "payload_" + name,
            reference[name] != converted[name],
            lambda r, c, f=name: "{}: reference {} converted {}".format(
                _describe_key(r), int(r[f]), int(c[f])
            ),
        )
    for name, components in FLOAT_PAYLOAD:
        if name in float_excluded:
            continue
        bad = reference[name] != converted[name]
        if components > 1:
            bad = bad.any(axis=1)
        check(
            "payload_" + name,
            bad,
            lambda r, c, f=name: "{}: reference bits {} converted bits {}".format(
                _describe_key(r),
                np.atleast_1d(r[f]).tolist(),
                np.atleast_1d(c[f]).tolist(),
            ),
        )
    if source_format in UNIT_FOREST_FORMATS:
        check(
            "source_halo_id",
            reference["SourceHaloID"] != converted["SourceHaloID"],
            lambda r, c: "{}: expected SourceHaloID {} converted {}".format(
                _describe_key(r), int(r["SourceHaloID"]), int(c["SourceHaloID"])
            ),
        )


def compare_streams(
    dump_blocks,
    converted_raw_blocks,
    identity_blocks,
    source_format,
    budget_bytes,
    spill_dir=None,
    float_excluded=(),
    findings=None,
):
    """Run the whole comparison over block streams; return its report dict.

    ``converted_raw_blocks`` and ``identity_blocks`` are zero-argument
    callables returning fresh iterators (the converted side is read twice),
    so the pipeline can run over files or over virtual blocks alike.
    """
    findings = findings if findings is not None else Findings()
    _declare_checks(findings, source_format)
    share = max(1, int(budget_bytes) // 4)
    stats = {"joins": {}}
    sorters = []

    def sorter(dtype, key, label):
        made = ExternalSorter(dtype, key, share, spill_dir, label)
        sorters.append(made)
        return made

    try:
        reference = sorter(HALO_DTYPE, HALO_KEY, "reference halos by (ForestIndex, rank)")
        for block in dump_blocks:
            reference.add(block)
        converted = sorter(HALO_DTYPE, HALO_KEY, "converted halos by (ForestIndex, rank)")
        requests = sorter(REQUEST_DTYPE, ("tsnap", "trow"), "link requests by target (snap, row)")
        for block in converter_halo_blocks(converted_raw_blocks(), requests, findings):
            converted.add(block)
        resolved = sorter(RESOLVED_DTYPE, ("sforest", "srank", "link"), "resolved links by owner")
        stats["joins"]["resolve"] = {}
        resolve_requests(
            requests.sorted_blocks(),
            identity_blocks(),
            resolved,
            findings,
            stats["joins"]["resolve"],
        )
        stats["joins"]["complete"] = {}
        completed = complete_links(
            resolved.sorted_blocks(),
            dedupe(
                converted.sorted_blocks(),
                HALO_KEY,
                findings,
                "duplicate_converted_rows",
                _describe_key,
            ),
            stats["joins"]["complete"],
        )
        checked_reference = reference_blocks(
            dedupe(
                reference.sorted_blocks(),
                HALO_KEY,
                findings,
                "duplicate_reference_rows",
                _describe_key,
            ),
            source_format,
            findings,
        )
        stats["joins"]["compare"] = {}
        matched = reference_rows = converted_rows = 0
        for ref, match, conv, conv_matched in lookup_join(
            checked_reference,
            HALO_KEY,
            HALO_DTYPE,
            completed,
            HALO_KEY,
            HALO_DTYPE,
            stats["joins"]["compare"],
        ):
            reference_rows += len(ref)
            converted_rows += len(conv)
            findings.compared("row_coverage", len(ref) + np.count_nonzero(~conv_matched))
            dropped = ref[match < 0]
            if len(dropped):
                findings.fail(
                    "row_coverage",
                    len(dropped),
                    ["{} is absent from the conversion".format(_describe_key(r)) for r in dropped],
                )
            extra = conv[~conv_matched]
            if len(extra):
                findings.fail(
                    "row_coverage",
                    len(extra),
                    [
                        "{} (snapshot {}) is absent from the reference".format(
                            _describe_key(r), int(r["FileSnapshot"])
                        )
                        for r in extra
                    ],
                )
            hit = match >= 0
            matched += int(np.count_nonzero(hit))
            if hit.any():
                _compare_window(ref[hit], conv[match[hit]], source_format, float_excluded, findings)
    finally:
        stats["sorts"] = [made.stats for made in sorters]
        for made in sorters:
            made.close()
    failed = findings.failed
    if matched == 0:
        failed = failed + ["nothing_compared"]
    return {
        "report_format": REPORT_FORMAT,
        "verdict": "FAIL" if failed else "PASS",
        "failed_checks": failed,
        "source_format": source_format,
        "reference_rows": reference_rows,
        "converted_rows": converted_rows,
        "matched_rows": matched,
        "checks": findings.as_dict(),
        "resources": stats,
    }


def compare_dataset(dataset_dir, dump_path, source_format, budget_bytes, spill_dir, block_rows):
    """Compare a version 3 dataset directory against a source dump file."""
    if source_format not in SOURCE_FORMATS:
        raise AcceptanceError("unknown source format {!r}".format(source_format))
    dump = SourceDump(dump_path)
    if dump.reader != source_format:
        raise AcceptanceError(
            "dump was written by the {!r} reader; the comparison is for {!r} (compare against "
            "the selected format's own vertical interpretation only)".format(
                dump.reader, source_format
            )
        )
    findings = Findings()
    _declare_checks(findings, source_format)
    try:
        dataset = V3Dataset(dataset_dir, findings)
    except DatasetDefect as defect:
        findings.declare("dataset_integrity", "the dataset is structurally comparable")
        findings.fail("dataset_integrity", 1, [str(defect)])
        return {
            "report_format": REPORT_FORMAT,
            "verdict": "FAIL",
            "failed_checks": findings.failed,
            "source_format": source_format,
            "checks": findings.as_dict(),
        }
    if dataset.source_format != source_format:
        raise AcceptanceError(
            "dataset source_format {!r} is not {!r}".format(dataset.source_format, source_format)
        )
    report = compare_streams(
        dump.blocks(block_rows),
        lambda: dataset.raw_blocks(block_rows),
        lambda: dataset.identity_blocks(block_rows),
        source_format,
        budget_bytes,
        spill_dir,
        dataset.float_excluded,
        findings,
    )
    report["dump"] = {
        "path": str(dump.path),
        "reader": dump.reader,
        "partition_model": dump.partition_model,
        "rows": dump.rows,
        "forests": dump.forests,
        "ignored_columns": dump.ignored_columns,
    }
    report["dataset"] = {"path": str(dataset.directory), "rows": dataset.total_rows}
    return report


# ===========================================================================
# Extras: independent source extraction
# ===========================================================================


def load_extra_declarations(profile_path):
    """(name, storage dtype, components, sources) for each declared extra, from plain YAML."""
    with open(profile_path, "r", encoding="utf-8") as handle:
        document = yaml.safe_load(handle)
    extras = document.get("extra_fields") if isinstance(document, dict) else None
    if not extras:
        raise AcceptanceError(
            "{}: declares no extra_fields; there is nothing to compare".format(profile_path)
        )
    declared = []
    for entry in extras:
        name, type_name, sources = entry.get("name"), entry.get("type"), entry.get("sources")
        if type_name not in EXTRA_TYPES or not isinstance(sources, list):
            raise AcceptanceError("{}: malformed extra {!r}".format(profile_path, entry))
        dtype, components = EXTRA_TYPES[type_name]
        if len(sources) != components:
            raise AcceptanceError(
                "{}: extra {} has {} source(s) for type {}".format(
                    profile_path, name, len(sources), type_name
                )
            )
        declared.append((name, np.dtype(dtype), components, sources))
    return declared


def _bits_dtype(dtype):
    return np.dtype("<u{}".format(np.dtype(dtype).itemsize))


def extras_record_dtype(declared):
    fields = [("SourceHaloID", "<i8")]
    for name, dtype, components, _sources in declared:
        bits = _bits_dtype(dtype)
        fields.append((name, bits) if components == 1 else (name, bits, (components,)))
    return np.dtype(fields)


def _cast(values, dtype, where, findings, check):
    """``values`` cast to the declared storage dtype, as bits; out-of-range integers fail."""
    values = np.asarray(values)
    if dtype.kind == "i":
        if values.dtype.kind == "f":
            raise AcceptanceError(
                "{}: the harness does not model float-to-integer extras".format(where)
            )
        info = np.iinfo(dtype)
        wide = values.astype(np.int64) if values.dtype.kind in "iu" else values
        outside = (wide < info.min) | (wide > info.max)
        if outside.any():
            findings.fail(
                check,
                np.count_nonzero(outside),
                ["{}: source value outside {}".format(where, dtype)],
            )
    native = np.dtype(dtype).newbyteorder("<")
    return values.astype(native).view(_bits_dtype(dtype))


def _extra_values(columns, name, dtype, components, sources, where, findings):
    """The declared value of one extra from a block of named source columns."""
    parts = []
    for source in sources:
        column = np.asarray(columns[source["field"]])
        if "component" in source:
            if column.ndim != 2:
                raise AcceptanceError("{}: {} is not a vector field".format(where, source["field"]))
            column = column[:, int(source["component"])]
        elif column.ndim != 1:
            raise AcceptanceError(
                "{}: vector field {} needs a component".format(where, source["field"])
            )
        parts.append(_cast(column, dtype, where, findings, "extra_" + name))
    return parts[0] if components == 1 else np.stack(parts, axis=1)


def lhalo_layout(halo_properties_path, byte_order):
    """The packed binary record recomputed from an ordered ``halo_properties.yaml``."""
    if byte_order not in ("little", "big"):
        raise AcceptanceError("byte order must be little or big, not {!r}".format(byte_order))
    prefix = "<" if byte_order == "little" else ">"
    with open(halo_properties_path, "r", encoding="utf-8") as handle:
        properties = yaml.safe_load(handle)["halo_properties"]
    fields = []
    for entry in properties:
        base, components = EXTRA_TYPES[entry["type"]]
        dtype = prefix + base[1:]
        fields.append((entry["name"], dtype) if components == 1 else (entry["name"], dtype, (3,)))
    return np.dtype(fields)


def iter_lhalo_source(source_dir, tree_name, first_file, last_file, layout, byte_order, block_rows):
    """(first SourceHaloID, record block) over the inventory, in file and record order."""
    prefix = "<" if byte_order == "little" else ">"
    source_halo_id = 1
    for number in range(int(first_file), int(last_file) + 1):
        path = Path(source_dir) / "{}.{}".format(tree_name, number)
        if not path.is_file():
            raise AcceptanceError("requested L-Halo file {} is missing".format(path))
        size = path.stat().st_size
        with open(path, "rb") as handle:
            head = np.fromfile(handle, dtype=prefix + "i4", count=2)
            if len(head) != 2 or head[0] < 0 or head[1] < 0:
                raise AcceptanceError("{}: truncated or negative header".format(path))
            counts = np.fromfile(handle, dtype=prefix + "i4", count=int(head[0]))
        if len(counts) != int(head[0]) or np.any(counts < 0) or int(counts.sum()) != int(head[1]):
            raise AcceptanceError("{}: tree counts disagree with the header".format(path))
        offset = 8 + 4 * int(head[0])
        halos = int(head[1])
        if size != offset + halos * layout.itemsize:
            raise AcceptanceError(
                "{}: {} bytes, expected {} for {} records of {} bytes".format(
                    path, size, offset + halos * layout.itemsize, halos, layout.itemsize
                )
            )
        if halos:
            records = np.memmap(path, dtype=layout, mode="r", offset=offset, shape=(halos,))
            for start in range(0, halos, block_rows):
                yield source_halo_id + start, records[start : start + block_rows]
            del records
        source_halo_id += halos


def _coalesce(offsets, counts):
    """Contiguous (start, length) ranges covering the forests in the given order."""
    ranges = []
    for start, length in zip(offsets.tolist(), counts.tolist()):
        if length == 0:
            continue
        if ranges and ranges[-1][0] + ranges[-1][1] == start:
            ranges[-1][1] += length
        else:
            ranges.append([start, length])
    return ranges


def iter_hdf5_source(info_file, first_file, last_file, fields, block_rows, budget_bytes):
    """(first SourceHaloID, {field: values}) over ``File<N>`` groups in ForestInfo order."""
    source_halo_id = 1
    with h5py.File(info_file, "r") as handle:
        for number in range(int(first_file), int(last_file) + 1):
            name = "File{}".format(number)
            try:
                group = handle[name]
                info = group["ForestInfo"]
                forests = group["Forests"]
            except KeyError as error:
                raise AcceptanceError(
                    "{}: {} is missing or unresolvable ({})".format(info_file, name, error)
                ) from None
            if info.shape[0] * info.dtype.itemsize > budget_bytes:
                raise AcceptanceError(
                    "{}/{}: ForestInfo ({} rows) exceeds the budget".format(
                        info_file, name, info.shape[0]
                    )
                )
            table = info[...]
            offsets = table["ForestHalosOffset"].astype(np.int64)
            counts = table["ForestNhalos"].astype(np.int64)
            for field in fields:
                if field not in forests:
                    raise AcceptanceError("{}/{}: no Forests/{}".format(info_file, name, field))
            length = forests[fields[0]].shape[0]
            if np.any(offsets < 0) or np.any(counts < 0) or np.any(offsets + counts > length):
                raise AcceptanceError(
                    "{}/{}: ForestInfo ranges exceed Forests".format(info_file, name)
                )
            for start, total in _coalesce(offsets, counts):
                for begin in range(start, start + total, block_rows):
                    stop = min(start + total, begin + block_rows)
                    yield source_halo_id, {field: forests[field][begin:stop] for field in fields}
                    source_halo_id += stop - begin


def _ascii_header(path):
    with open(path, "r", encoding="ascii") as handle:
        first = handle.readline()
    if not first.startswith("#"):
        raise AcceptanceError("{}: no column header line".format(path))
    return [token.split("(")[0].lower() for token in first[1:].split()]


def _ascii_forest_of_tree(forests_list):
    mapping = {}
    with open(forests_list, "r", encoding="ascii") as handle:
        for line in handle:
            if line.startswith("#") or not line.strip():
                continue
            tree, forest = line.split()
            mapping[int(tree)] = int(forest)
    return mapping


def _ascii_trees(path, width):
    """Yield (tree root id, data row lines) per ``#tree`` block, in file order."""
    tree, rows, seen_tree = None, [], False
    with open(path, "r", encoding="ascii") as handle:
        handle.readline()
        for number, line in enumerate(handle, start=2):
            if line.startswith("#tree"):
                if seen_tree:
                    yield tree, rows
                tree, rows, seen_tree = int(line.split()[1]), [], True
            elif line.startswith("#") or not line.strip():
                continue
            else:
                tokens = line.split()
                if len(tokens) == width and seen_tree:
                    rows.append(tokens)
                elif len(tokens) == 1 and not seen_tree:
                    continue  # the tree-count line before the first marker
                else:
                    raise AcceptanceError("{}:{}: malformed data row".format(path, number))
    if seen_tree:
        yield tree, rows


def iter_ascii_source(tree_files, forests_list, fields):
    """(SourceHaloID array, {field: token arrays}) per tree block; not in id order.

    A unit is one forest's part of one file, numbered by the forest's first
    ``#tree`` marker in that file; a row's ordinal is its position among its
    unit's rows in file order; ``SourceHaloID`` prefix-sums unit sizes in
    ascending (file, unit) order, from 1. A first pass counts each tree's rows.
    Only the indexed-header dialect (``#name(0) name(1) ...`` on line 1) is
    read; anything else fails rather than being guessed at. Float tokens are
    parsed with Python's correctly rounded ``float()`` before the declared
    cast, so a converter parse that is not correctly rounded is reported, not
    mirrored.
    """
    forest_of_tree = _ascii_forest_of_tree(forests_list)
    base = 1
    for path in tree_files:
        header = _ascii_header(path)
        missing = [field for field in fields if field.lower() not in header]
        if missing:
            raise AcceptanceError("{}: no column(s) {}".format(path, missing))
        position = {field: header.index(field.lower()) for field in fields}
        unit_of_forest, unit_sizes, tree_offset = {}, [], []
        for tree, rows in _ascii_trees(path, len(header)):
            if tree not in forest_of_tree:
                raise AcceptanceError("{}: tree {} is not in {}".format(path, tree, forests_list))
            unit = unit_of_forest.setdefault(forest_of_tree[tree], len(unit_of_forest))
            if unit == len(unit_sizes):
                unit_sizes.append(0)
            tree_offset.append((unit, unit_sizes[unit]))
            unit_sizes[unit] += len(rows)
        unit_base = np.concatenate([[0], np.cumsum(unit_sizes, dtype=np.int64)])
        for (_tree, rows), (unit, offset) in zip(_ascii_trees(path, len(header)), tree_offset):
            if not rows:
                continue
            table = np.array(rows, dtype=str)
            ids = base + unit_base[unit] + offset + np.arange(len(rows), dtype=np.int64)
            yield ids, {field: table[:, position[field]] for field in fields}
        base += int(unit_base[-1])


def _ascii_values(tokens, dtype):
    """float64 parse then the declared cast for floats; exact integers otherwise."""
    if np.dtype(dtype).kind == "f":
        return np.array([float(token) for token in tokens], dtype=np.float64)
    return np.array([int(token) for token in tokens], dtype=np.int64)


def source_extra_blocks(source_format, declared, inventory, block_rows, budget_bytes, findings):
    """Independently extracted extras as :func:`extras_record_dtype` blocks."""
    record = extras_record_dtype(declared)
    fields = sorted({source["field"] for _n, _d, _c, sources in declared for source in sources})
    if source_format == "lhalo_binary":
        layout = lhalo_layout(inventory["halo_properties"], inventory["byte_order"])
        missing = [field for field in fields if field not in layout.names]
        if missing:
            raise AcceptanceError("binary layout has no field(s) {}".format(missing))
        blocks = (
            (first + np.arange(len(records), dtype=np.int64), {f: records[f] for f in fields})
            for first, records in iter_lhalo_source(
                inventory["source_dir"],
                inventory["tree_name"],
                inventory["first_file"],
                inventory["last_file"],
                layout,
                inventory["byte_order"],
                block_rows,
            )
        )
    elif source_format == "consistent_trees_hdf5":
        blocks = (
            (first + np.arange(len(columns[fields[0]]), dtype=np.int64), columns)
            for first, columns in iter_hdf5_source(
                inventory["info_file"],
                inventory["first_file"],
                inventory["last_file"],
                fields,
                block_rows,
                budget_bytes,
            )
        )
    elif source_format == "consistent_trees_ascii":
        blocks = iter_ascii_source(inventory["tree_files"], inventory["forests_list"], fields)
    else:
        raise AcceptanceError("unknown source format {!r}".format(source_format))
    for ids, columns in blocks:
        out = np.zeros(len(ids), dtype=record)
        out["SourceHaloID"] = ids
        for name, dtype, components, sources in declared:
            if source_format == "consistent_trees_ascii":
                columns_for = {
                    source["field"]: _ascii_values(columns[source["field"]], dtype)
                    for source in sources
                }
            else:
                columns_for = columns
            out[name] = _extra_values(
                columns_for, name, dtype, components, sources, "extra " + name, findings
            )
        yield out


def compare_extras(
    dataset_dir, source_format, profile, inventory, budget_bytes, spill_dir, block_rows
):
    """Compare every declared extra of a dataset with independent source extraction."""
    declared = load_extra_declarations(profile)
    findings = Findings()
    findings.declare("duplicate_reference_rows", "no SourceHaloID extracted twice")
    findings.declare("duplicate_converted_rows", "no SourceHaloID converted twice")
    findings.declare("row_coverage", "the same SourceHaloIDs on both sides")
    for name, _dtype, _components, _sources in declared:
        findings.declare("extra_" + name, "{} bits equal the source's declared cast".format(name))
    try:
        dataset = V3Dataset(dataset_dir)
    except DatasetDefect as defect:
        findings.declare("dataset_integrity", "the dataset is structurally comparable")
        findings.fail("dataset_integrity", 1, [str(defect)])
        return {"report_format": REPORT_FORMAT, "verdict": "FAIL", "checks": findings.as_dict()}
    if dataset.source_format != source_format:
        raise AcceptanceError(
            "dataset source_format {!r} is not {!r}".format(dataset.source_format, source_format)
        )
    descriptions = dataset.dataset_descriptions([name for name, *_rest in declared])
    usable = []
    for name, dtype, components, _sources in declared:
        want = (dtype, () if components == 1 else (components,))
        if descriptions[name] != want:
            findings.fail(
                "extra_" + name,
                dataset.total_rows,
                ["/halos/{} is {}, declared {}".format(name, descriptions[name], want)],
            )
        else:
            usable.append(name)
    record = extras_record_dtype(declared)
    share = max(1, int(budget_bytes) // 2)
    stats = {}
    with contextlib.ExitStack() as stack:
        key = ("SourceHaloID",)
        ref = stack.enter_context(ExternalSorter(record, key, share, spill_dir, "extracted extras"))
        conv = stack.enter_context(
            ExternalSorter(record, key, share, spill_dir, "converted extras")
        )
        for block in source_extra_blocks(
            source_format, declared, inventory, block_rows, budget_bytes, findings
        ):
            ref.add(block)
        for ids, arrays in dataset.extra_blocks(usable, block_rows):
            out = np.zeros(len(ids), dtype=record)
            out["SourceHaloID"] = ids
            for name in usable:
                out[name] = arrays[name].view(record[name].base)
            conv.add(out)

        def describe(row):
            return "SourceHaloID {}".format(int(row["SourceHaloID"]))

        matched = 0
        for r, match, c, c_matched in lookup_join(
            dedupe(
                ref.sorted_blocks(),
                ("SourceHaloID",),
                findings,
                "duplicate_reference_rows",
                describe,
            ),
            ("SourceHaloID",),
            record,
            dedupe(
                conv.sorted_blocks(),
                ("SourceHaloID",),
                findings,
                "duplicate_converted_rows",
                describe,
            ),
            ("SourceHaloID",),
            record,
            stats,
        ):
            findings.compared("row_coverage", len(r) + np.count_nonzero(~c_matched))
            if np.any(match < 0):
                findings.fail(
                    "row_coverage",
                    np.count_nonzero(match < 0),
                    [describe(row) + " is absent from the conversion" for row in r[match < 0]],
                )
            if np.any(~c_matched):
                findings.fail(
                    "row_coverage",
                    np.count_nonzero(~c_matched),
                    [describe(row) + " is absent from the source" for row in c[~c_matched]],
                )
            hit = match >= 0
            matched += int(np.count_nonzero(hit))
            rr, cc = r[hit], c[match[hit]]
            for name in usable:
                bad = rr[name] != cc[name]
                if bad.ndim > 1:
                    bad = bad.any(axis=1)
                findings.compared("extra_" + name, len(rr))
                if bad.any():
                    findings.fail(
                        "extra_" + name,
                        np.count_nonzero(bad),
                        [
                            "{}: source bits {} converted bits {}".format(
                                describe(a),
                                np.atleast_1d(a[name]).tolist(),
                                np.atleast_1d(b[name]).tolist(),
                            )
                            for a, b in zip(rr[bad], cc[bad])
                        ],
                    )
        stats["sorts"] = [ref.stats, conv.stats]
    failed = findings.failed + ([] if matched else ["nothing_compared"])
    return {
        "report_format": REPORT_FORMAT,
        "verdict": "FAIL" if failed else "PASS",
        "failed_checks": failed,
        "source_format": source_format,
        "extras": [name for name, *_rest in declared],
        "matched_rows": matched,
        "dataset": {"path": str(dataset.directory), "rows": dataset.total_rows},
        "checks": findings.as_dict(),
        "resources": stats,
    }


# ===========================================================================
# Measurement and the run record
# ===========================================================================


def sha256_file(path):
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def file_identity(path, hash_contents=False):
    """Path, size, mtime, inode and (optionally) SHA-256 of one file."""
    path = Path(path)
    status = path.stat()
    identity = {
        "path": str(path.resolve()),
        "bytes": status.st_size,
        "mtime_ns": status.st_mtime_ns,
        "device": status.st_dev,
        "inode": status.st_ino,
    }
    if hash_contents:
        identity["sha256"] = sha256_file(path)
    return identity


def source_identities(paths, hash_contents):
    """Identities of every file named, expanding directories one level."""
    identities = []
    for path in paths or ():
        path = Path(path)
        if path.is_dir():
            identities.extend(
                file_identity(child, hash_contents)
                for child in sorted(path.iterdir())
                if child.is_file()
            )
        else:
            identities.append(file_identity(path, hash_contents))
    return identities


def code_identity():
    """The repository commit, working-tree state and the harness's own digest."""

    def git(*args):
        result = subprocess.run(
            ["git"] + list(args), cwd=str(REPO_ROOT), capture_output=True, text=True
        )
        return result.stdout.strip() if result.returncode == 0 else None

    status = git("status", "--porcelain")
    return {
        "git_commit": git("rev-parse", "HEAD"),
        "git_branch": git("rev-parse", "--abbrev-ref", "HEAD"),
        "git_dirty_paths": None if status is None else len(status.splitlines()),
        "harness_sha256": sha256_file(Path(__file__)),
        "python": sys.version.split()[0],
        "numpy": np.__version__,
        "h5py": h5py.__version__,
        "platform": platform.platform(),
    }


def _maxrss_bytes(value):
    """``ru_maxrss`` is bytes on macOS and KiB on Linux."""
    return int(value) if sys.platform == "darwin" else int(value) * 1024


def tree_storage(path):
    """Total apparent and allocated bytes of every file under ``path``."""
    apparent = allocated = files = 0
    for root, _dirs, names in os.walk(str(path)):
        for name in names:
            status = os.lstat(os.path.join(root, name))
            apparent += status.st_size
            allocated += getattr(status, "st_blocks", 0) * 512
            files += 1
    return {
        "path": str(path),
        "files": files,
        "apparent_bytes": apparent,
        "allocated_bytes": allocated,
    }


def dataset_storage(dataset_dir):
    """Rows, logical record width and B/halo (logical and on disk) of a v3 dataset."""
    rows = 0
    widths = {}
    for path in sorted(Path(dataset_dir).glob("snapshot_*.h5")):
        with h5py.File(path, "r") as handle:
            halos = handle["halos"]
            rows += halos["SourceHaloID"].shape[0]
            for name, dataset in halos.items():
                width = dataset.dtype.itemsize * int(np.prod(dataset.shape[1:], dtype=np.int64))
                widths.setdefault(name, width)
    storage = tree_storage(dataset_dir)
    logical = sum(widths.values())
    storage.update(
        {
            "halos": rows,
            "halo_dataset_bytes_per_row": widths,
            "logical_bytes_per_halo": logical,
            "apparent_bytes_per_halo": storage["apparent_bytes"] / rows if rows else None,
            "allocated_bytes_per_halo": storage["allocated_bytes"] / rows if rows else None,
        }
    )
    return storage


class RunRecord:
    """The ``--record`` JSON file: one entry per measured run, saved atomically."""

    def __init__(self, path):
        self.path = Path(path)
        if self.path.exists():
            with open(self.path, "r", encoding="utf-8") as handle:
                self.document = json.load(handle)
            if self.document.get("record_format") != RECORD_FORMAT:
                raise AcceptanceError("{} is not a {} file".format(path, RECORD_FORMAT))
        else:
            self.document = {
                "record_format": RECORD_FORMAT,
                "created": _now(),
                "host": platform.node(),
                "runs": [],
            }

    def append(self, entry):
        self.document["runs"].append(entry)
        self.path.parent.mkdir(parents=True, exist_ok=True)
        temporary = self.path.with_name(self.path.name + ".tmp")
        with open(temporary, "w", encoding="utf-8") as handle:
            json.dump(self.document, handle, indent=2, sort_keys=True, default=str)
            handle.write("\n")
        os.replace(temporary, self.path)


def _now():
    return datetime.datetime.now(datetime.timezone.utc).isoformat()


def measured_run(argv, label, log_dir, cwd=None, env=None):
    """Run one child to completion; measure its wall/CPU time and its own peak RSS."""
    log_dir = Path(log_dir)
    log_dir.mkdir(parents=True, exist_ok=True)
    stem = "{}_{}".format(
        time.strftime("%Y%m%dT%H%M%S"), "".join(c if c.isalnum() else "_" for c in label)
    )
    stdout_path, stderr_path = log_dir / (stem + ".out"), log_dir / (stem + ".err")
    argv = [str(arg) for arg in argv]
    executable = shutil.which(argv[0]) or argv[0]
    entry = {
        "label": label,
        "kind": "command",
        "command": argv,
        "cwd": str(cwd or Path.cwd()),
        "started": _now(),
        "stdout": str(stdout_path),
        "stderr": str(stderr_path),
    }
    if Path(executable).is_file():
        entry["executable"] = file_identity(executable, hash_contents=True)
    start = time.monotonic()
    with open(stdout_path, "wb") as out, open(stderr_path, "wb") as err:
        try:
            process = subprocess.Popen(argv, stdout=out, stderr=err, cwd=cwd, env=env)
        except OSError as error:
            entry.update({"exit_code": None, "error": str(error), "wall_seconds": 0.0})
            return entry
        _pid, status, usage = os.wait4(process.pid, 0)
    process.returncode = os.waitstatus_to_exitcode(status)
    entry.update(
        {
            "exit_code": process.returncode,
            "wall_seconds": time.monotonic() - start,
            "user_seconds": usage.ru_utime,
            "system_seconds": usage.ru_stime,
            "peak_rss_bytes": _maxrss_bytes(usage.ru_maxrss),
        }
    )
    return entry


def in_process_measurement(label, function):
    """Run ``function()`` here; measure wall/CPU time and the process's peak RSS."""
    before = resource.getrusage(resource.RUSAGE_SELF)
    start = time.monotonic()
    entry = {"label": label, "kind": "in_process", "command": sys.argv, "started": _now()}
    try:
        result = function()
        entry["result"] = result
    finally:
        after = resource.getrusage(resource.RUSAGE_SELF)
        entry.update(
            {
                "wall_seconds": time.monotonic() - start,
                "user_seconds": after.ru_utime - before.ru_utime,
                "system_seconds": after.ru_stime - before.ru_stime,
                "peak_rss_bytes": _maxrss_bytes(after.ru_maxrss),
                "peak_rss_scope": "process lifetime",
            }
        )
    return entry


# ===========================================================================
# Command line
# ===========================================================================


CONVERT_STAGES = ("ingest", "transpose", "write", "validate", "report")


def _log_dir(args):
    return Path(args.log_dir) if args.log_dir else Path(args.record).resolve().parent / "logs"


def cmd_build_dump(args):
    env = dict(os.environ, MODEL=args.model, SIMULATION=args.simulation)
    env["TOPOLOGY_DUMP_BUILD_DIR"] = str(Path(args.build_dir).resolve())
    entry = measured_run(
        ["bash", BUILD_TOPOLOGY_DUMP], "build-dump", _log_dir(args), REPO_ROOT, env
    )
    entry["model"], entry["simulation"] = args.model, args.simulation
    tool = Path(args.build_dir) / "dump_ctrees_topology"
    if entry["exit_code"] == 0 and tool.is_file():
        entry["tool"] = file_identity(tool, hash_contents=True)
    return entry, EXIT_PASS if entry["exit_code"] == 0 else EXIT_FAIL


def cmd_dump(args):
    argv = [args.tool, "--source-payload", args.run_file, args.out]
    entry = measured_run(argv, "dump", _log_dir(args), REPO_ROOT)
    entry["run_file"] = file_identity(args.run_file, hash_contents=True)
    if entry["exit_code"] == 0:
        entry["dump"] = file_identity(args.out, hash_contents=True)
    return entry, EXIT_PASS if entry["exit_code"] == 0 else EXIT_FAIL


def cmd_exec(args):
    if not args.argv:
        raise AcceptanceError("exec needs a command after --")
    entry = measured_run(args.argv, args.label, _log_dir(args))
    return entry, EXIT_PASS if entry["exit_code"] == 0 else EXIT_FAIL


def cmd_convert(args):
    workdir = Path(args.workdir)
    if workdir.exists():
        raise AcceptanceError(
            "{} exists; acceptance conversions use a fresh workdir".format(workdir)
        )
    python = sys.executable
    stages = []
    exit_code = EXIT_PASS
    for stage in CONVERT_STAGES:
        argv = [python, CONVERT_TREES, stage, "--workdir", workdir]
        if stage == "ingest":
            argv += list(args.argv)
        elif stage == "write":
            argv += ["--simulation-info", args.simulation_info]
        entry = measured_run(argv, "convert-" + stage, _log_dir(args))
        entry["workdir_storage"] = tree_storage(workdir) if workdir.exists() else None
        stages.append(entry)
        if entry["exit_code"] != 0:
            exit_code = EXIT_FAIL
            break
    result = {"label": "convert", "kind": "pipeline", "workdir": str(workdir), "stages": stages}
    attempts = sorted((workdir / "write").glob("attempt_*")) if exit_code == EXIT_PASS else []
    if attempts:
        result["dataset"] = str(attempts[-1])
        result["dataset_storage"] = dataset_storage(attempts[-1])
    return result, exit_code


def _write_report(path, report):
    if path:
        Path(path).parent.mkdir(parents=True, exist_ok=True)
        with open(path, "w", encoding="utf-8") as handle:
            json.dump(report, handle, indent=2, sort_keys=True, default=str)
            handle.write("\n")


def _budget(args):
    if args.budget_mb <= 0:
        raise AcceptanceError("--budget-mb must be positive")
    return int(args.budget_mb * 1024 * 1024)


def _block_rows(args, budget):
    return args.block_rows or max(1024, budget // 4096)


def cmd_compare(args):
    budget = _budget(args)
    entry = in_process_measurement(
        "compare",
        lambda: compare_dataset(
            args.dataset,
            args.dump,
            args.source_format,
            budget,
            args.spill_dir,
            _block_rows(args, budget),
        ),
    )
    report = entry["result"]
    _write_report(args.report, report)
    entry["inputs"] = source_identities([args.dump, args.dataset], args.hash_sources)
    entry["result"] = {key: report.get(key) for key in ("verdict", "failed_checks", "matched_rows")}
    entry["result"]["report"] = args.report
    entry["resources"] = report.get("resources")
    return entry, EXIT_PASS if report["verdict"] == "PASS" else EXIT_FAIL


def cmd_compare_extras(args):
    budget = _budget(args)
    inventory = {
        "source_dir": args.source_dir,
        "tree_name": args.tree_name,
        "first_file": args.first_file,
        "last_file": args.last_file,
        "halo_properties": args.halo_properties,
        "byte_order": args.byte_order,
        "info_file": args.info_file,
        "forests_list": args.forests_list,
        "tree_files": args.tree_file or [],
    }
    required = {
        "lhalo_binary": ("source_dir", "tree_name", "first_file", "last_file", "halo_properties"),
        "consistent_trees_hdf5": ("info_file", "first_file", "last_file"),
        "consistent_trees_ascii": ("forests_list", "tree_files"),
    }[args.source_format]
    missing = [key for key in required if inventory[key] in (None, [])]
    if missing:
        raise AcceptanceError("{} extraction needs {}".format(args.source_format, missing))
    entry = in_process_measurement(
        "compare-extras",
        lambda: compare_extras(
            args.dataset,
            args.source_format,
            args.column_map,
            inventory,
            budget,
            args.spill_dir,
            _block_rows(args, budget),
        ),
    )
    report = entry["result"]
    _write_report(args.report, report)
    sources = [args.dataset, args.column_map]
    if args.source_format == "lhalo_binary":
        sources += [
            Path(args.source_dir) / "{}.{}".format(args.tree_name, n)
            for n in range(args.first_file, args.last_file + 1)
        ]
    elif args.source_format == "consistent_trees_hdf5":
        sources.append(args.info_file)
    else:
        sources += [args.forests_list] + list(args.tree_file)
    entry["inputs"] = source_identities(sources, args.hash_sources)
    entry["result"] = {key: report.get(key) for key in ("verdict", "failed_checks", "matched_rows")}
    entry["result"]["report"] = args.report
    entry["resources"] = report.get("resources")
    return entry, EXIT_PASS if report["verdict"] == "PASS" else EXIT_FAIL


def build_parser():
    parser = argparse.ArgumentParser(
        prog="run_generalisation_acceptance",
        description=__doc__.split("\n\n")[0],
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    common = argparse.ArgumentParser(add_help=False)
    common.add_argument("--record", required=True, help="acceptance record JSON (appended)")
    common.add_argument("--log-dir", help="child stdout/stderr logs (default: <record dir>/logs)")
    common.add_argument(
        "--source", action="append", default=[], help="input file/dir identity to record"
    )
    common.add_argument(
        "--hash-sources", action="store_true", help="also SHA-256 every recorded input"
    )
    sub = parser.add_subparsers(dest="command", required=True)

    build = sub.add_parser("build-dump", parents=[common], help="build the C dump harness")
    build.add_argument("--model", required=True)
    build.add_argument("--simulation", required=True)
    build.add_argument("--build-dir", required=True, help="its own directory per SIMULATION")
    build.set_defaults(handler=cmd_build_dump)

    dump = sub.add_parser("dump", parents=[common], help="run the harness in source-payload mode")
    dump.add_argument("--tool", required=True)
    dump.add_argument("--run-file", required=True)
    dump.add_argument("--out", required=True)
    dump.set_defaults(handler=cmd_dump)

    convert = sub.add_parser("convert", parents=[common], help="run every convert_trees stage")
    convert.add_argument("--workdir", required=True)
    convert.add_argument("--simulation-info", required=True, help="passed to the write stage")
    convert.add_argument("argv", nargs=argparse.REMAINDER, help="-- then the ingest arguments")
    convert.set_defaults(handler=cmd_convert)

    run = sub.add_parser("exec", parents=[common], help="run any command, measured")
    run.add_argument("--label", required=True)
    run.add_argument("argv", nargs=argparse.REMAINDER, help="-- then the command")
    run.set_defaults(handler=cmd_exec)

    limits = argparse.ArgumentParser(add_help=False)
    limits.add_argument("--dataset", required=True, help="the version 3 dataset directory")
    limits.add_argument("--source-format", required=True, choices=SOURCE_FORMATS)
    limits.add_argument("--budget-mb", type=float, default=DEFAULT_BUDGET_MB)
    limits.add_argument("--block-rows", type=int, help="rows per read block")
    limits.add_argument("--spill-dir", help="external-sort spill directory (default: TMPDIR)")
    limits.add_argument("--report", help="full JSON report path")

    compare = sub.add_parser("compare", parents=[common, limits], help="dataset vs source dump")
    compare.add_argument("--dump", required=True)
    compare.set_defaults(handler=cmd_compare)

    extras = sub.add_parser(
        "compare-extras", parents=[common, limits], help="extras vs independent source extraction"
    )
    extras.add_argument("--column-map", required=True, help="the profile that declared the extras")
    extras.add_argument("--source-dir")
    extras.add_argument("--tree-name")
    extras.add_argument("--first-file", type=int)
    extras.add_argument("--last-file", type=int)
    extras.add_argument("--halo-properties")
    extras.add_argument("--byte-order", default="little", choices=("little", "big"))
    extras.add_argument("--info-file")
    extras.add_argument("--forests-list")
    extras.add_argument("--tree-file", action="append")
    extras.set_defaults(handler=cmd_compare_extras)
    return parser


def main(argv=None):
    parser = build_parser()
    args = parser.parse_args(argv)
    if getattr(args, "argv", None) and args.argv[0] == "--":
        args.argv = args.argv[1:]
    try:
        record = RunRecord(args.record)
        entry, exit_code = args.handler(args)
    except AcceptanceError as error:
        print("run_generalisation_acceptance: error: {}".format(error), file=sys.stderr)
        return EXIT_ERROR
    entry["exit_status"] = exit_code
    entry["code"] = code_identity()
    if args.source:
        entry["recorded_sources"] = source_identities(args.source, args.hash_sources)
    record.append(entry)
    verdict = (
        entry.get("result", {}).get("verdict") if isinstance(entry.get("result"), dict) else None
    )
    print(
        "{}: {} (exit {}); recorded in {}".format(
            entry["label"],
            verdict or ("ok" if exit_code == EXIT_PASS else "failed"),
            exit_code,
            args.record,
        )
    )
    return exit_code


if __name__ == "__main__":
    sys.exit(main())
