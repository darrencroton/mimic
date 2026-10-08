"""The census aggregate directory: identity binding and atomic writes.

An aggregate directory holds one dataset's census results, one subdirectory per
subcommand (``occupancy/``, ``trees/``, ``partition/``), beside
``identity.json``, the :meth:`horizontal_dataset.HorizontalDataset.identity`
record of the dataset the results were computed from. The first subcommand run
against a directory writes it; every later one compares the dataset it was
given against it and refuses a different dataset, so results from two datasets
are never mixed. The directory and its results are deleted by hand when the
census is done; the only file anything here removes is a subcommand's own
``summary.json``, as below.

Every file is written to a temporary name and renamed into place, and a
subcommand writes its ``summary.json`` last, so a summary's presence means the
subcommand completed. A subcommand that writes arrays first removes its own
``summary.json`` (:func:`begin`), so a rerun that fails part-way through a
completed directory cannot leave the old summary vouching for half-rewritten
arrays.
"""

import json
import os
import sys
from pathlib import Path
from typing import Mapping, Optional, Sequence

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from errors import ConverterError  # noqa: E402

IDENTITY_NAME = "identity.json"
SUMMARY_NAME = "summary.json"

#: The fields of :meth:`horizontal_dataset.HorizontalDataset.identity`.
IDENTITY_KEYS = (
    "format_version",
    "source_format",
    "n_forests_total",
    "n_halos",
    "forest_id_sha256",
)


def slab_stem(snap: int) -> str:
    """The per-slab file stem, ``slab_NNN``."""
    return "slab_{:03d}".format(int(snap))


def write_json(path, payload: Mapping) -> Path:
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_name(path.name + ".tmp")
    with open(tmp, "w") as handle:
        json.dump(payload, handle, indent=2, sort_keys=True)
        handle.write("\n")
    os.replace(str(tmp), str(path))
    return path


def read_json(path):
    """Parse a JSON file.

    Raises:
        ConverterError: when the file is not valid UTF-8 JSON, naming it.
        OSError: when it cannot be opened.
    """
    with open(path, "rb") as handle:
        raw = handle.read()
    try:
        return json.loads(raw.decode("utf-8"))
    except (UnicodeDecodeError, ValueError) as exc:
        raise ConverterError("{}: not readable as JSON ({})".format(path, exc)) from exc


def read_record(path, keys: Sequence[str] = ()) -> dict:
    """A JSON object holding at least ``keys``.

    Raises:
        ConverterError: for unreadable JSON, a value that is not an object, or
            a missing key, naming the file.
    """
    record = read_json(path)
    if not isinstance(record, dict):
        raise ConverterError("{}: not a JSON object".format(path))
    missing = [key for key in keys if key not in record]
    if missing:
        raise ConverterError("{}: missing {}".format(path, missing))
    return record


def save_array(path, array: np.ndarray) -> int:
    """``np.save`` ``array`` atomically to ``path``; returns the file's size."""
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_name(path.name + ".tmp")
    with open(tmp, "wb") as handle:
        np.save(handle, array, allow_pickle=False)
    os.replace(str(tmp), str(path))
    return path.stat().st_size


def load_array(path, mmap: bool = False) -> np.ndarray:
    """Load an aggregate array; ``mmap`` maps it read-only instead of reading it."""
    return np.load(path, mmap_mode="r" if mmap else None, allow_pickle=False)


def bind(aggregate_dir, identity: Mapping) -> Path:
    """Bind ``aggregate_dir`` to the dataset ``identity``: write it to a new
    directory, or require an existing directory's record to be equal.

    Raises:
        ConverterError: when the directory was produced from another dataset.
    """
    aggregate_dir = Path(aggregate_dir)
    aggregate_dir.mkdir(parents=True, exist_ok=True)
    path = aggregate_dir / IDENTITY_NAME
    if not path.exists():
        write_json(path, identity)
        return aggregate_dir
    recorded = read_record(path)
    current = json.loads(json.dumps(identity))
    if recorded != current:
        differing = sorted(
            key
            for key in set(recorded) | set(current)
            if recorded.get(key, None) != current.get(key, None)
        )
        raise ConverterError(
            "{}: produced from a different dataset (identity differs in {}); use a new "
            "aggregate directory for this dataset".format(aggregate_dir, differing)
        )
    return aggregate_dir


def begin(directory) -> Path:
    """Open a subcommand's output ``directory`` for writing: create it, and
    remove its completion marker (``summary.json``) if a previous run left one.
    Nothing else in the directory is touched."""
    directory = Path(directory)
    directory.mkdir(parents=True, exist_ok=True)
    marker = directory / SUMMARY_NAME
    if marker.exists():
        os.remove(marker)
    return directory


def bound_identity(aggregate_dir) -> dict:
    """The identity an existing aggregate directory is bound to.

    Raises:
        ConverterError: when the directory has no identity record.
    """
    path = Path(aggregate_dir) / IDENTITY_NAME
    if not path.is_file():
        raise ConverterError(
            "{}: not a census aggregate directory (no {})".format(aggregate_dir, IDENTITY_NAME)
        )
    return read_record(path, IDENTITY_KEYS)


def require_summary(directory, what: str, keys: Sequence[str] = ()) -> dict:
    """The ``summary.json`` of a completed subcommand, holding at least ``keys``.

    Raises:
        ConverterError: when the subcommand has not completed in this directory,
            or its summary is unreadable or lacks a key.
    """
    path = Path(directory) / SUMMARY_NAME
    if not path.is_file():
        raise ConverterError("{}: no {}; run {} first".format(directory, SUMMARY_NAME, what))
    return read_record(path, keys)


def directory_bytes(directory, pattern: Optional[str] = None) -> int:
    """Total size of the regular files under ``directory`` (matching ``pattern``)."""
    directory = Path(directory)
    paths = directory.rglob(pattern) if pattern else directory.rglob("*")
    return sum(path.stat().st_size for path in paths if path.is_file())
