"""Bounded, read-only access to a horizontal-HDF5 dataset of version 2 or 3.

A dataset directory holds one ``snapshot_NNN.h5`` per snapshot, contiguous from
``000``, and the ``forests.h5`` sidecar
(``convert/mimic-convert/HORIZONTAL-HDF5-FORMAT.md``, File Set and Naming, and
V3 File Set and Naming). :class:`HorizontalDataset` opens one, reads every
snapshot's ``/header`` attributes, checks the run-scoped ones agree across files,
and then reads ``/halos`` columns only in **bounded blocks**: one named column of
one slab, at most ``block_rows`` rows at a time (default 2^22). Nothing here
ever materialises a whole column of a slab, which is what lets the forest census
(``forest_census.py``) walk a 519 million-row slab in a few hundred megabytes.

The :meth:`HorizontalDataset.identity` record -- format version,
``source_format`` (version 3 only; ``None`` for version 2), ``n_forests_total``,
per-snapshot ``n_halos`` and the SHA-256 of the sidecar's ``ForestID`` column --
is what census aggregate directories are bound to, so that a later subcommand
refuses a directory produced from a different dataset.

This module validates only what it relies on (file set, supported version,
agreement of the run-scoped attributes, column lengths, the sidecar length); the
full conformance battery is ``validate.py`` / ``validate_v3.py``.
"""

import hashlib
import os
import re
import sys
from pathlib import Path
from typing import Dict, Iterator, List, Optional, Sequence, Tuple

import h5py
import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from errors import ConverterError  # noqa: E402
from hdf5_writer_v3 import SIDECAR_NAME, V3_STRING_ATTRS, snapshot_h5_name  # noqa: E402

#: Rows per block read: 2^22 rows is 32 MiB of an int64 column.
DEFAULT_BLOCK_ROWS = 1 << 22

#: The ``format_version`` values this reader opens.
SUPPORTED_FORMAT_VERSIONS = (2, 3)

#: Header attributes that are properties of the whole dataset and must be
#: identical in every snapshot file. ``source_format`` exists in version 3 only.
RUN_SCOPED_ATTRS = (
    "format_version",
    "links_adjacent",
    "n_forests_total",
    "max_halo_rank_in_forest",
    "source_format",
)

#: Header attributes read after opening, required in every file: name -> type.
REQUIRED_ATTRS = {
    "n_halos": int,
    "n_forests_total": int,
    "max_halo_rank_in_forest": int,
    "links_adjacent": int,
    "scale_factor": float,
}

_SNAPSHOT_FILE = re.compile(r"^snapshot_(\d{3,})\.h5$")


def _require_block_rows(block_rows) -> int:
    if isinstance(block_rows, bool) or not isinstance(block_rows, (int, np.integer)):
        raise ConverterError("block_rows must be a positive integer, got {!r}".format(block_rows))
    if int(block_rows) < 1:
        raise ConverterError("block_rows must be a positive integer, got {}".format(block_rows))
    return int(block_rows)


def _attr_value(value):
    """An HDF5 attribute as a plain Python value: numpy scalars unwrapped,
    fixed-length ASCII decoded with its NUL padding removed."""
    if isinstance(value, (bytes, np.bytes_)):
        return bytes(value).rstrip(b"\0").decode("ascii")
    if isinstance(value, np.generic):
        return value.item()
    return value


class HorizontalDataset:
    """One horizontal-HDF5 dataset directory, opened read-only.

    Opening reads every snapshot header and the sidecar's shape; no halo row is
    read until a block iterator is consumed. Each iterator opens its file for
    the duration of the iteration only.

    Raises:
        ConverterError: on a missing or non-contiguous snapshot file set, a
            missing sidecar, an unsupported or mixed ``format_version``, a
            run-scoped attribute that differs between files, a header
            ``snapshot_number`` that disagrees with its file name, or a sidecar
            ``ForestID`` whose length is not ``n_forests_total``.
    """

    def __init__(self, directory):
        self.directory = Path(directory)
        if not self.directory.is_dir():
            raise ConverterError("{}: not a dataset directory".format(self.directory))
        numbers = []
        for entry in os.listdir(self.directory):
            match = _SNAPSHOT_FILE.match(entry)
            if match and entry == snapshot_h5_name(int(match.group(1))):
                numbers.append(int(match.group(1)))
        numbers.sort()
        if not numbers:
            raise ConverterError("{}: no snapshot_NNN.h5 files".format(self.directory))
        if numbers != list(range(len(numbers))):
            missing = sorted(set(range(numbers[-1] + 1)) - set(numbers))
            raise ConverterError(
                "{}: snapshot files are not contiguous from 000; missing {}".format(
                    self.directory, [snapshot_h5_name(n) for n in missing[:5]]
                )
            )
        self.sidecar_path = self.directory / SIDECAR_NAME
        if not self.sidecar_path.is_file():
            raise ConverterError("{}: no {}".format(self.directory, SIDECAR_NAME))

        self._headers: List[Dict[str, object]] = []
        for snap in numbers:
            path = self.snapshot_path(snap)
            with h5py.File(path, "r") as handle:
                if "header" not in handle:
                    raise ConverterError("{}: no /header group".format(path))
                header = {key: _attr_value(v) for key, v in handle["header"].attrs.items()}
            if header.get("snapshot_number") != snap:
                raise ConverterError(
                    "{}: header snapshot_number {!r} disagrees with the file name".format(
                        path, header.get("snapshot_number")
                    )
                )
            self._headers.append(header)

        first = self._headers[0]
        version = first.get("format_version")
        if version not in SUPPORTED_FORMAT_VERSIONS:
            raise ConverterError(
                "{}: format_version {!r} is not one of {}".format(
                    self.snapshot_path(0), version, SUPPORTED_FORMAT_VERSIONS
                )
            )
        for name in RUN_SCOPED_ATTRS:
            values = {repr(header.get(name)) for header in self._headers}
            if len(values) != 1:
                raise ConverterError(
                    "{}: header attribute {} differs between snapshot files: {}".format(
                        self.directory, name, sorted(values)
                    )
                )
        for name, kind in REQUIRED_ATTRS.items():
            for snap, header in enumerate(self._headers):
                value = header.get(name)
                if isinstance(value, bool) or not isinstance(value, kind):
                    raise ConverterError(
                        "{}: header attribute {} missing or not {} (got {!r})".format(
                            self.snapshot_path(snap), name, kind.__name__, value
                        )
                    )
        if version >= 3 and not all(name in first for name in V3_STRING_ATTRS):
            raise ConverterError(
                "{}: a version 3 header without {}".format(self.directory, sorted(V3_STRING_ATTRS))
            )

        with h5py.File(self.sidecar_path, "r") as handle:
            if "ForestID" not in handle:
                raise ConverterError("{}: no /ForestID dataset".format(self.sidecar_path))
            n_ids = int(handle["ForestID"].shape[0])
        if n_ids != self.n_forests_total:
            raise ConverterError(
                "{}: /ForestID holds {} entries but n_forests_total is {}".format(
                    self.sidecar_path, n_ids, self.n_forests_total
                )
            )

    # ---- header ----------------------------------------------------------

    @property
    def format_version(self) -> int:
        return int(self._headers[0]["format_version"])

    @property
    def source_format(self) -> Optional[str]:
        """The producing adapter (version 3), or ``None`` for version 2."""
        return self._headers[0].get("source_format")

    @property
    def n_forests_total(self) -> int:
        return int(self._headers[0]["n_forests_total"])

    @property
    def max_halo_rank_in_forest(self) -> int:
        return int(self._headers[0]["max_halo_rank_in_forest"])

    @property
    def links_adjacent(self) -> int:
        return int(self._headers[0]["links_adjacent"])

    @property
    def snapshots(self) -> Tuple[int, ...]:
        """Snapshot numbers, ascending from 0 (the a_list order)."""
        return tuple(range(len(self._headers)))

    @property
    def scale_factors(self) -> Tuple[float, ...]:
        return tuple(float(header["scale_factor"]) for header in self._headers)

    @property
    def n_halos(self) -> Tuple[int, ...]:
        """Per-snapshot halo counts from the headers."""
        return tuple(int(header["n_halos"]) for header in self._headers)

    @property
    def total_halos(self) -> int:
        return sum(self.n_halos)

    def header(self, snap: int) -> Dict[str, object]:
        """A copy of one snapshot's ``/header`` attributes as Python values."""
        return dict(self._headers[self._check_snapshot(snap)])

    def snapshot_path(self, snap: int) -> Path:
        return self.directory / snapshot_h5_name(snap)

    def _check_snapshot(self, snap: int) -> int:
        if not 0 <= int(snap) < len(self._headers):
            raise ConverterError(
                "{}: snapshot {} is outside [0, {})".format(
                    self.directory, snap, len(self._headers)
                )
            )
        return int(snap)

    # ---- bounded reads -----------------------------------------------------

    def _halo_datasets(self, handle, snap: int, names: Sequence[str]) -> List[Tuple[str, object]]:
        """The named ``/halos`` datasets of an open snapshot file, each checked
        to hold the header's ``n_halos`` rows."""
        path = self.snapshot_path(snap)
        if "halos" not in handle:
            raise ConverterError("{}: no /halos group".format(path))
        halos = handle["halos"]
        datasets = []
        for name in names:
            if name not in halos:
                raise ConverterError("{}: no /halos/{} dataset".format(path, name))
            dataset = halos[name]
            if int(dataset.shape[0]) != self.n_halos[snap]:
                raise ConverterError(
                    "{}: /halos/{} holds {} rows but n_halos is {}".format(
                        path, name, dataset.shape[0], self.n_halos[snap]
                    )
                )
            datasets.append((name, dataset))
        return datasets

    def iter_columns(
        self, snap: int, names: Sequence[str], block_rows: int = DEFAULT_BLOCK_ROWS
    ) -> Iterator[Tuple[int, Dict[str, np.ndarray]]]:
        """Yield ``(first_row, {name: block})`` over one slab, every named
        ``/halos`` column read for the same row range of at most ``block_rows``
        rows. An empty slab yields nothing.

        Raises:
            ConverterError: on an unknown column or one whose length is not the
                header's ``n_halos``.
        """
        snap = self._check_snapshot(snap)
        block_rows = _require_block_rows(block_rows)
        n_rows = self.n_halos[snap]
        with h5py.File(self.snapshot_path(snap), "r") as handle:
            datasets = self._halo_datasets(handle, snap, names)
            for start in range(0, n_rows, block_rows):
                stop = min(start + block_rows, n_rows)
                yield start, {name: dataset[start:stop] for name, dataset in datasets}

    def read_rows(
        self, snap: int, names: Sequence[str], start: int, stop: int
    ) -> Dict[str, np.ndarray]:
        """The rows ``[start, stop)`` of every named ``/halos`` column of one
        slab: a bounded read for a caller that already holds a block's range.

        Raises:
            ConverterError: on a range outside the slab, or as :meth:`iter_columns`.
        """
        snap = self._check_snapshot(snap)
        if not 0 <= int(start) <= int(stop) <= self.n_halos[snap]:
            raise ConverterError(
                "{}: rows [{}, {}) are outside [0, {})".format(
                    self.snapshot_path(snap), start, stop, self.n_halos[snap]
                )
            )
        with h5py.File(self.snapshot_path(snap), "r") as handle:
            return {
                name: dataset[int(start) : int(stop)]
                for name, dataset in self._halo_datasets(handle, snap, names)
            }

    def iter_column(
        self, snap: int, name: str, block_rows: int = DEFAULT_BLOCK_ROWS
    ) -> Iterator[Tuple[int, np.ndarray]]:
        """Yield ``(first_row, block)`` over one named ``/halos`` column of one
        slab, at most ``block_rows`` rows per block."""
        for start, columns in self.iter_columns(snap, (name,), block_rows):
            yield start, columns[name]

    def iter_sidecar(
        self, name: str = "ForestID", block_rows: int = DEFAULT_BLOCK_ROWS
    ) -> Iterator[Tuple[int, np.ndarray]]:
        """Yield ``(first_forest, block)`` over one ``forests.h5`` dataset."""
        block_rows = _require_block_rows(block_rows)
        with h5py.File(self.sidecar_path, "r") as handle:
            if name not in handle:
                raise ConverterError("{}: no /{} dataset".format(self.sidecar_path, name))
            dataset = handle[name]
            for start in range(0, int(dataset.shape[0]), block_rows):
                yield start, dataset[start : start + block_rows]

    def forest_ids(self) -> np.ndarray:
        """The sidecar ``ForestID`` column, int64, indexed by ``ForestIndex``
        (one entry per forest: 8 B x ``n_forests_total``)."""
        ids = np.empty(self.n_forests_total, dtype=np.int64)
        for start, block in self.iter_sidecar("ForestID"):
            ids[start : start + block.size] = block
        return ids

    # ---- identity ----------------------------------------------------------

    def forest_id_sha256(self) -> str:
        """SHA-256 of the sidecar ``ForestID`` column as little-endian int64,
        in ``ForestIndex`` order, read in bounded blocks."""
        digest = hashlib.sha256()
        for _start, block in self.iter_sidecar("ForestID"):
            digest.update(np.ascontiguousarray(block, dtype="<i8").tobytes())
        return digest.hexdigest()

    def identity(self) -> Dict[str, object]:
        """The dataset identity record census aggregates are bound to: equal
        records mean the same forest enumeration and the same slab sizes."""
        return {
            "format_version": self.format_version,
            "source_format": self.source_format,
            "n_forests_total": self.n_forests_total,
            "n_halos": list(self.n_halos),
            "forest_id_sha256": self.forest_id_sha256(),
        }
