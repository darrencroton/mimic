"""The file plan: every forest's halo total and the cut of the forests into L-Halo files.

**Forest totals.** In a version 3 dataset ``SourceHaloID`` is a halo's 1-based position in
(``ForestIndex``, ``HaloRankInForest``) order over the whole dataset, and every slab's rows ascend
in it (``convert/mimic-convert/HORIZONTAL-HDF5-FORMAT.md``). A forest's first row in any slab
therefore carries ``1 + (halos of every earlier forest) + rank``, and the minimum over slabs of
those first-row ids is ``1 + (halos of every earlier forest)`` itself, whichever snapshot the
forest's rank 0 lies in and whichever snapshots the forest reaches. :func:`forest_totals` keeps
that minimum per forest (8 B per forest) over one bounded pass of every slab's ``ForestIndex``
and ``SourceHaloID``; each forest's total is the difference of consecutive minima, and the last
forest's is the dataset's halo total less its minimum plus one.

**Files.** :func:`cut_files` cuts the forests, in ascending ``ForestIndex``, into contiguous
ranges: a new file starts when adding the next forest would exceed either the halo cap or the
forest cap, and a forest larger than the halo cap occupies a file alone. The reader's header
counts and its running halo sum are C ``int``, so no file may hold more than 2,147,483,647 halos
or forests.

**The plan file.** ``file_plan.json`` records the dataset identity (``forest_id_sha256``,
``n_forests_total``, the halo total and ``column_mapping_sha256``), the caps, the digest of the
record the files are written in (:mod:`vertical.record`), which is what binds the plan to a
package, and per file its number, first and last forest, forest count and halo count. The
package's ``halo_properties.yaml`` path and md5 are recorded for information only: a copy of the
file elsewhere, or a comment edit, derives the same record. :func:`read_file_plan` refuses a plan
whose dataset identity or record digest differs from the open dataset's and the derived record's.
"""

import hashlib
import json
import os
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, List, Optional, Sequence, Union

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from errors import ConverterError  # noqa: E402
from horizontal_dataset import DEFAULT_BLOCK_ROWS, HorizontalDataset  # noqa: E402
from vertical.common import require_positive_int, write_json_atomic  # noqa: E402
from vertical.record import record_digest, record_dtype, record_layout  # noqa: E402

#: The plan's file name inside a conversion's output directory.
FILE_PLAN_NAME = "file_plan.json"

#: The ``format`` and ``version`` a plan file declares.
FILE_PLAN_FORMAT = "mimic-vertical-file-plan"
FILE_PLAN_VERSION = 1

#: Default caps: a new file starts before it would pass 100,000,000 halos or 1,000,000 forests.
DEFAULT_MAX_HALOS_PER_FILE = 100_000_000
DEFAULT_MAX_FORESTS_PER_FILE = 1_000_000

#: The reader's ``int`` header counts and running halo sum: the most halos or forests in a file.
INT32_MAX = int(np.iinfo(np.int32).max)

_UNSEEN = np.iinfo(np.int64).max

#: The integer fields of each file entry of a plan.
_ENTRY_FIELDS = ("number", "first_forest", "last_forest", "n_forests", "n_halos")


@dataclass(frozen=True)
class PlannedFile:
    """One output file: the forests ``first_forest`` to ``last_forest`` inclusive."""

    number: int
    first_forest: int
    last_forest: int
    n_halos: int

    @property
    def n_forests(self) -> int:
        return self.last_forest - self.first_forest + 1

    def as_dict(self) -> Dict[str, int]:
        return {
            "number": self.number,
            "first_forest": self.first_forest,
            "last_forest": self.last_forest,
            "n_forests": self.n_forests,
            "n_halos": self.n_halos,
        }


def forest_totals(dataset: HorizontalDataset, block_rows: int = DEFAULT_BLOCK_ROWS) -> np.ndarray:
    """Every forest's halo total, indexed by ``ForestIndex`` (int64).

    One pass over every slab's ``ForestIndex`` and ``SourceHaloID`` in blocks of ``block_rows``
    rows, holding 8 B per forest plus one block of each column; deriving the totals adds the
    returned array's 8 B per forest.

    Raises:
        ConverterError: naming the first offending value, for a slab whose ``ForestIndex`` is not
            non-decreasing or leaves ``[0, n_forests_total)``, a forest present in no slab,
            minima that do not strictly increase with ``ForestIndex``, a first minimum other
            than 1, or totals that do not sum to the dataset's halo total with every forest
            holding at least one halo.
    """
    n_forests = dataset.n_forests_total
    total = dataset.total_halos
    minima = np.full(n_forests, _UNSEEN, dtype=np.int64)
    for snap in dataset.snapshots:
        previous = -1
        for start, columns in dataset.iter_columns(
            snap, ("ForestIndex", "SourceHaloID"), block_rows
        ):
            forest = columns["ForestIndex"].astype(np.int64, copy=False)
            ids = columns["SourceHaloID"].astype(np.int64, copy=False)
            before = np.concatenate(([previous], forest[:-1]))
            drops = np.flatnonzero(forest < before)
            if drops.size:
                row = int(drops[0])
                raise ConverterError(
                    "{}: ForestIndex is not non-decreasing: row {} holds {} after {}".format(
                        dataset.snapshot_path(snap), start + row, forest[row], before[row]
                    )
                )
            outside = np.flatnonzero((forest < 0) | (forest >= n_forests))
            if outside.size:
                row = int(outside[0])
                raise ConverterError(
                    "{}: row {} has ForestIndex {}, outside [0, {})".format(
                        dataset.snapshot_path(snap), start + row, forest[row], n_forests
                    )
                )
            first = forest != before
            starts = forest[first]
            minima[starts] = np.minimum(minima[starts], ids[first])
            previous = int(forest[-1])

    absent = np.flatnonzero(minima == _UNSEEN)
    if absent.size:
        raise ConverterError(
            "{}: forest {} (ForestIndex) is present in no slab ({} forest(s) absent)".format(
                dataset.directory, int(absent[0]), absent.size
            )
        )
    if n_forests == 0:
        if total:
            raise ConverterError("{}: {} halos but no forests".format(dataset.directory, total))
        return minima
    totals = np.empty(n_forests, dtype=np.int64)
    totals[:-1] = minima[1:]
    totals[:-1] -= minima[:-1]
    stalls = np.flatnonzero(totals[:-1] <= 0)
    if stalls.size:
        forest = int(stalls[0]) + 1
        raise ConverterError(
            "{}: forest {}'s first SourceHaloID {} does not exceed forest {}'s {}".format(
                dataset.directory, forest, minima[forest], forest - 1, minima[forest - 1]
            )
        )
    if minima[0] != 1:
        raise ConverterError(
            "{}: forest 0's first SourceHaloID is {}, not 1 (the totals need SourceHaloID to be "
            "the dense 1-based (ForestIndex, HaloRankInForest) position)".format(
                dataset.directory, minima[0]
            )
        )
    totals[-1] = total - minima[-1] + 1
    if totals[-1] < 1 or int(totals.sum()) != total:
        raise ConverterError(
            "{}: forest totals do not sum to the dataset's {} halos: forests 0 to {} hold {}, "
            "leaving {} for forest {} (the totals need SourceHaloID to be the dense 1-based "
            "(ForestIndex, HaloRankInForest) position, which gapped ids are not)".format(
                dataset.directory,
                total,
                n_forests - 2,
                int(minima[-1]) - 1,
                int(totals[-1]),
                n_forests - 1,
            )
        )
    return totals


def cut_files(
    totals: Sequence[int], max_halos_per_file: int, max_forests_per_file: int
) -> List[PlannedFile]:
    """Cut forests, in ascending ``ForestIndex``, into contiguous files.

    A new file starts when adding the next forest would exceed ``max_halos_per_file`` halos or
    ``max_forests_per_file`` forests; a forest above the halo cap occupies a file alone. The
    files tile ``[0, len(totals))`` and are numbered from 0.

    Raises:
        ConverterError: for a cap that is not an integer from 1 to 2,147,483,647, a forest total
            below 1, or a forest above 2,147,483,647 halos, naming the value.
    """
    halo_cap = require_positive_int("max_halos_per_file", max_halos_per_file, INT32_MAX)
    forest_cap = require_positive_int("max_forests_per_file", max_forests_per_file, INT32_MAX)
    totals = np.asarray(totals)
    if totals.ndim != 1 or (totals.size and totals.dtype.kind not in "iu"):
        raise ConverterError("forest totals must be a one-dimensional integer array")
    totals = totals.astype(np.int64, copy=False)
    empty = np.flatnonzero(totals < 1)
    if empty.size:
        raise ConverterError(
            "forest {} has {} halos; every forest holds at least one".format(
                int(empty[0]), totals[empty[0]]
            )
        )
    huge = np.flatnonzero(totals > INT32_MAX)
    if huge.size:
        raise ConverterError(
            "forest {} has {} halos, more than a file can hold ({})".format(
                int(huge[0]), totals[huge[0]], INT32_MAX
            )
        )
    before = np.concatenate(([0], np.cumsum(totals)))
    files = []
    first = 0
    while first < totals.size:
        stop = int(np.searchsorted(before, before[first] + halo_cap, side="right")) - 1
        stop = max(min(stop, first + forest_cap, totals.size), first + 1)
        files.append(PlannedFile(len(files), first, stop - 1, int(before[stop] - before[first])))
        first = stop
    return files


def dataset_identity(dataset: HorizontalDataset) -> Dict[str, object]:
    """The identity a plan is bound to: ``forest_id_sha256``, ``n_forests_total``,
    ``total_halos`` and ``column_mapping_sha256``.

    Raises:
        ConverterError: if the slabs' ``column_mapping_sha256`` differ.
    """
    mappings = {dataset.header(snap).get("column_mapping_sha256") for snap in dataset.snapshots}
    if len(mappings) != 1:
        raise ConverterError(
            "{}: column_mapping_sha256 differs between snapshot files: {}".format(
                dataset.directory, sorted(map(str, mappings))
            )
        )
    return {
        "forest_id_sha256": dataset.forest_id_sha256(),
        "n_forests_total": dataset.n_forests_total,
        "total_halos": dataset.total_halos,
        "column_mapping_sha256": mappings.pop(),
    }


def record_binding(halo_properties: Union[str, Path, Sequence[dict]]) -> Dict[str, object]:
    """The record digest, with the YAML's path and md5 when it is a file (information only)."""
    binding = {"digest": record_digest(record_layout(record_dtype(halo_properties)))}
    if isinstance(halo_properties, (str, Path)):
        binding["halo_properties"] = str(halo_properties)
        binding["halo_properties_md5"] = hashlib.md5(Path(halo_properties).read_bytes()).hexdigest()
    return binding


def make_file_plan(
    dataset: HorizontalDataset,
    halo_properties: Union[str, Path, Sequence[dict]],
    max_halos_per_file: int = DEFAULT_MAX_HALOS_PER_FILE,
    max_forests_per_file: int = DEFAULT_MAX_FORESTS_PER_FILE,
    block_rows: int = DEFAULT_BLOCK_ROWS,
) -> Dict[str, object]:
    """The file plan of a dataset written in a package's record, as ``file_plan.json`` holds it.

    The caps are checked before the dataset is read.
    """
    require_positive_int("max_halos_per_file", max_halos_per_file, INT32_MAX)
    require_positive_int("max_forests_per_file", max_forests_per_file, INT32_MAX)
    record = record_binding(halo_properties)
    files = cut_files(forest_totals(dataset, block_rows), max_halos_per_file, max_forests_per_file)
    return {
        "format": FILE_PLAN_FORMAT,
        "version": FILE_PLAN_VERSION,
        "dataset": dataset_identity(dataset),
        "record": record,
        "caps": {
            "max_halos_per_file": int(max_halos_per_file),
            "max_forests_per_file": int(max_forests_per_file),
        },
        "files": [planned.as_dict() for planned in files],
    }


def write_file_plan(directory, plan: Dict[str, object]) -> Path:
    """Write ``plan`` to ``<directory>/file_plan.json`` atomically and durably; returns the path."""
    path = Path(directory) / FILE_PLAN_NAME
    path.parent.mkdir(parents=True, exist_ok=True)
    write_json_atomic(path, plan)
    return path


def _check_files(path: Path, plan: Dict[str, object]) -> None:
    """The plan's files are numbered from 0, tile ``[0, n_forests_total)`` and hold the
    dataset's halos, with each entry's fields plain integers and its counts consistent with
    each other, the reader's int limit and the plan's recorded caps (a lone forest may exceed
    the halo cap, as :func:`cut_files` leaves it alone in its file).

    Raises:
        ConverterError: naming the first entry that is malformed, inconsistent or over a limit,
            or the totals the entries do not reach.
    """
    dataset = plan["dataset"]
    halo_cap = require_positive_int(
        "{}: caps max_halos_per_file".format(path), plan["caps"]["max_halos_per_file"], INT32_MAX
    )
    forest_cap = require_positive_int(
        "{}: caps max_forests_per_file".format(path),
        plan["caps"]["max_forests_per_file"],
        INT32_MAX,
    )
    expected = 0
    halos = 0
    for number, entry in enumerate(plan["files"]):
        if not isinstance(entry, dict) or any(
            isinstance(entry.get(name), bool) or not isinstance(entry.get(name), int)
            for name in _ENTRY_FIELDS
        ):
            raise ConverterError(
                "{}: file entry {} is malformed: every field of {} must be a plain integer: "
                "{}".format(path, number, list(_ENTRY_FIELDS), entry)
            )
        first, last = entry["first_forest"], entry["last_forest"]
        if (
            entry["number"] != number
            or first != expected
            or last < first
            or entry["n_forests"] != last - first + 1
            or entry["n_halos"] < entry["n_forests"]
        ):
            raise ConverterError(
                "{}: file entry {} is inconsistent: {}".format(path, number, entry)
            )
        if (
            entry["n_forests"] > forest_cap
            or entry["n_halos"] > INT32_MAX
            or (entry["n_halos"] > halo_cap and entry["n_forests"] > 1)
        ):
            raise ConverterError(
                "{}: file entry {} exceeds a limit (at most {} forests per file, {} halos per "
                "file, or a lone forest's, and the reader's {}): {}".format(
                    path, number, forest_cap, halo_cap, INT32_MAX, entry
                )
            )
        expected = last + 1
        halos += entry["n_halos"]
    if expected != dataset["n_forests_total"] or halos != dataset["total_halos"]:
        raise ConverterError(
            "{}: the files cover {} forests and {} halos, not the dataset's {} and {}".format(
                path, expected, halos, dataset["n_forests_total"], dataset["total_halos"]
            )
        )


def read_file_plan(
    directory,
    dataset: HorizontalDataset,
    halo_properties: Union[str, Path, Sequence[dict]],
    identity: Optional[Dict[str, object]] = None,
) -> Dict[str, object]:
    """Read ``<directory>/file_plan.json`` and check it binds this dataset and record.

    Args:
        directory: the output directory holding the plan.
        dataset: the open dataset.
        halo_properties: the package's ``halo_properties.yaml`` path, or its parsed list.
        identity: the dataset's :func:`dataset_identity`, when the caller has it already (it
            hashes the whole forest sidecar); derived here otherwise.

    Raises:
        ConverterError: for a missing or unreadable plan, one of another format or version,
            file entries that are not plain integers, are inconsistent or exceed the reader's
            int limit or the plan's recorded caps, or a dataset identity or record digest that
            differs from the open dataset's or the record derived from ``halo_properties``.
    """
    path = Path(directory) / FILE_PLAN_NAME
    try:
        plan = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as error:
        raise ConverterError("{}: cannot read the file plan: {}".format(path, error)) from error
    if (
        not isinstance(plan, dict)
        or plan.get("format") != FILE_PLAN_FORMAT
        or plan.get("version") != FILE_PLAN_VERSION
    ):
        raise ConverterError(
            "{}: not a {} version {} file".format(path, FILE_PLAN_FORMAT, FILE_PLAN_VERSION)
        )
    try:
        _check_files(path, plan)
        recorded_digest = plan["record"]["digest"]
        recorded_identity = plan["dataset"]
    except (KeyError, TypeError) as error:
        raise ConverterError("{}: malformed file plan ({!r})".format(path, error)) from error
    if identity is None:
        identity = dataset_identity(dataset)
    for name, value in identity.items():
        if recorded_identity.get(name) != value:
            raise ConverterError(
                "{}: planned for a dataset whose {} is {!r}; {} has {!r}".format(
                    path, name, recorded_identity.get(name), dataset.directory, value
                )
            )
    digest = record_digest(record_layout(record_dtype(halo_properties)))
    if recorded_digest != digest:
        raise ConverterError(
            "{}: planned for the record with digest {}; the record derived now has {}".format(
                path, recorded_digest, digest
            )
        )
    return plan
