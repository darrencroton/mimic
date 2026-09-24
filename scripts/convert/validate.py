"""Producer validation battery for the ctrees -> horizontal-HDF5 converter
(plan Slice 7).

Implements the full producer battery from docs/dev/HORIZONTAL-HDF5-FORMAT.md
(Validation Requirements): count conservation against the INDEPENDENT
per-source-file pre-counts from the Slice 2 pre-scan (never the parser-derived
totals alone — plan review finding 7), all six format invariants, progenitor
round-trip closure, NextProgenitor same-file scope, FoF chain
integrity/cycle-freedom, identity uniqueness/density, header identity bounds,
a_list <-> scale_factor consistency, and Len >= 0 with the zero count logged.

Standalone CLI over a directory of snapshot files::

    python scripts/convert/validate.py <hdf5-dir> --a-list <a_list> \\
        --manifest <workdir>/manifest.json [--multiplier 1000000000]

Exit status is non-zero on any failure. ``--manifest`` is REQUIRED: count
conservation against the independent pre-counts is a mandatory part of the
producer battery, so the CLI never reports PASS without it, and the manifest
is BOUND to the dataset being validated (the supplied a_list must match the
manifest's recorded provenance md5, and every .h5 file must match the
emission checksum the writer recorded) so an unrelated manifest cannot
satisfy the mandatory checks. (The ``run_battery`` API accepts
``manifest_path=None`` for targeted unit tests of the other checks; that path
records count-conservation and manifest-binding as SKIP and is not reachable
from the CLI.)

The battery validates in two stages: structural conformance per file (object
set, dtypes, shapes, chunking, compression, attribute set) first, and the
semantic checks only when every file is structurally conformant — semantic
checks cannot be trusted on files whose layout is already wrong.

Memory is bounded by the window each check needs rather than by the dataset
(converter scale pass, plan Slice 6). Nothing loads the whole dataset: the
per-snapshot checks hold one file's arrays, progenitor closure holds the
adjacent pair, count conservation and the run-scoped header comparison carry
scalars, and ``check_identity`` streams two chunked passes over
``(ForestIndex, HaloRankInForest)`` behind an exact one-bit-per-halo structure.
The in-memory formulation this replaced measured 73.27 GB on a 1.8% subset of
Shin-Uchuu and was unbounded at production.

**Format version 3** (converter generalisation Slice 8) has its own battery,
:func:`run_battery_v3`, in a separate section at the end of this module; the
version 2 functions above are unchanged and never loosened for it. The CLI
dispatches on what the dataset declares (:func:`detect_dataset_version`): the
v3 battery only when every snapshot file declares version 3, a refusal when a
dataset mixes 3 with anything else, and the unchanged v2 battery otherwise --
which is what rejects version 1 and unknown versions, exactly as before.
For v3, ``--manifest`` names the generic conversion's ``manifest.json``.
"""

import argparse
import heapq
import json
import os
import shutil
import sys
import tempfile
from pathlib import Path
from typing import Dict, List, Mapping, Optional, Tuple

import h5py
import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from conversion_manifest import ConversionManifest, sha256_file  # noqa: E402
from ctrees_parser import ConverterError  # noqa: E402
from hdf5_writer import (  # noqa: E402
    CHUNK_1D,
    CHUNK_VEC,
    FORMAT_VERSION,
    HALO_DATASETS,
    HEADER_ATTRS,
    snapshot_h5_name,
)
from rank_sort import KeyedSorter, RankSortError, ResidencyMeter, SpillLedger  # noqa: E402
from scatter import file_md5, load_a_list  # noqa: E402
from source_keys import (  # noqa: E402
    CHAIN_DTYPE,
    CHAIN_KEY,
    SnapshotLayout,
    verify_chains_acyclic,
)

#: Default UniqueGalaxyID multiplier (TREE_MUL_FAC, src/include/constants.h).
DEFAULT_MULTIPLIER = 10**9

#: Rows the two identity passes read at a time. Four HDF5 chunks (``CHUNK_1D``
#: is 65536 rows) per read, so reads stay chunk-aligned, and small enough that
#: the per-chunk working arrays enumerated in ``IDENTITY_CHUNK_BYTES_PER_ROW``
#: cost tens of MB rather than a fraction of the dataset.
IDENTITY_CHUNK_ROWS = 4 * CHUNK_1D[0]

#: Bytes an identity pass may hold per row of a chunk. The vectorized bitset
#: pass holds, per row, the ForestIndex and HaloRankInForest blocks read from
#: the file, the in-range row indices and the ForestIndex and rank values
#: gathered through them, each row's forest count and bitset base, the claimed
#: rows, their slots and those slots' byte indices, the stable sort
#: permutation, the sorted slots, the deduplicated slots with their byte
#: indices and byte-group starts, and the violating rows — sixteen int64-wide
#: arrays — plus the in-range, held, rejected, duplicate and repeat masks and
#: the slot bit masks. numpy also materialises unnamed temporaries for the
#: comparisons and the gathers, and their count is an implementation detail of
#: numpy rather than of this module, so this figure is a CEILING rather than an
#: exact enumeration: ``test_identity_chunk_cost_is_within_the_declared_bound``
#: measures the real vectorized path and fails if it exceeds this. It was 117
#: bytes per row measured at IDENTITY_CHUNK_ROWS when this ceiling was set.
IDENTITY_CHUNK_BYTES_PER_ROW = 128

#: Slots of the identity bitset examined at once when a failure message needs
#: the first rank a forest holds no halo for. Bounded so the diagnostic scan of
#: a single huge forest cannot itself allocate without limit.
_MISSING_RANK_SCAN_SLOTS = 1 << 20

#: One out-of-range (ForestIndex, HaloRankInForest) pair in the external
#: ordering. Both int64, matching the emitted datasets.
_PAIR_DTYPE = np.dtype([("forest", "<i8"), ("rank", "<i8")], align=False)

#: Pairs buffered before a sorted run is spilled, how many sorted runs are
#: merged at once, and the rows read or written per block while merging and
#: scanning. Every one of them bounds a buffer rather than a total. The read
#: block is the one that also bounds a PYTHON-side cost: ``_ordered_groups``
#: materialises one int per group in a block, and a block whose values are all
#: distinct is all groups, so 8192 rows caps that at a few hundred KB.
_ORDERING_RUN_ROWS = 1 << 16
_ORDERING_FANIN = 8
_ORDERING_MERGE_ROWS = 1 << 13
_ORDERING_READ_ROWS = 1 << 13
#: Rows a single merge input reads at a time. Smaller than the scan's block
#: because up to _ORDERING_FANIN of these are live at once AND each one is
#: turned into Python ints for heapq, which costs far more per row than the
#: numpy block it came from.
_ORDERING_MERGE_READ_ROWS = 1 << 10

#: Prefix of the ordering's private spill directory, created under the system
#: temporary directory. ``check_identity`` creates it only when a dataset
#: actually carries an out-of-range ForestIndex, and removes it on the success,
#: failure and exception paths alike, so it never outlives the call that made
#: it and no other stage may depend on it.
ORDERING_DIR_PREFIX = "validate_identity_"

_INT64_MAX = np.iinfo(np.int64).max
_INT64_MIN = np.iinfo(np.int64).min

#: Header attributes that are run-scoped or physical and must be identical in
#: every file of the dataset.
RUN_SCOPED_ATTRS = (
    "n_forests_total",
    "max_halo_rank_in_forest",
    "box_size_mpc_h",
    "particle_mass_msun_h",
    "omega_matter",
    "omega_lambda",
    "hubble_h",
)


class Outcome:
    """One named battery check outcome."""

    def __init__(self, name: str, status: str, detail: str = ""):
        self.name = name
        self.status = status  # PASS | FAIL | SKIP
        self.detail = detail

    def as_dict(self) -> dict:
        return {"name": self.name, "status": self.status, "detail": self.detail}

    def line(self) -> str:
        text = "{}: {}".format(self.name, self.status)
        if self.detail:
            text += " — {}".format(self.detail)
        return text


def _examples(values, limit: int = 5) -> str:
    return ", ".join(str(v) for v in list(values)[:limit])


def _log(message: str) -> None:
    print(message, file=sys.stderr)


# ---------------------------------------------------------------------------
# Stage A: structural conformance
# ---------------------------------------------------------------------------


def _filter_failures(dataset, name: str) -> List[str]:
    """The storage contract is chunked and UNFILTERED: compression of any
    kind, scale-offset, shuffle, and fletcher32 are all prohibited."""
    failures = []
    if dataset.compression is not None:
        failures.append(
            "{} is compressed ({}); the contract forbids compression".format(
                name, dataset.compression
            )
        )
    if dataset.scaleoffset is not None:
        failures.append(
            "{} uses the scale-offset filter; the contract forbids filters".format(name)
        )
    if dataset.shuffle:
        failures.append("{} uses the shuffle filter; the contract forbids filters".format(name))
    if dataset.fletcher32:
        failures.append("{} uses the fletcher32 filter; the contract forbids filters".format(name))
    return failures


def check_file_set(directory: Path, n_snapshots: int) -> List[str]:
    """The directory must contain exactly snapshot_NNN.h5 for every a_list
    snapshot plus forests.h5 — nothing else that claims to be part of the
    dataset."""
    failures = []
    expected = {snapshot_h5_name(snap) for snap in range(n_snapshots)}
    expected.add("forests.h5")
    present = {p.name for p in directory.glob("*.h5")}
    missing = sorted(expected - present)
    extra = sorted(present - expected)
    if missing:
        failures.append("{} missing file(s): {}".format(len(missing), _examples(missing)))
    if extra:
        failures.append("{} unexpected .h5 file(s): {}".format(len(extra), _examples(extra)))
    return failures


def check_snapshot_structure(path: Path) -> List[str]:
    """Exact object set, dataset dtypes/shapes/chunking/compression, and
    header attribute names/dtypes for one snapshot file."""
    failures = []
    with h5py.File(path, "r") as handle:
        root_objects = set(handle.keys())
        if root_objects != {"header", "halos"}:
            failures.append(
                "root object set {} != {{'header', 'halos'}}".format(sorted(root_objects))
            )
            return failures
        if not isinstance(handle["header"], h5py.Group) or not isinstance(
            handle["halos"], h5py.Group
        ):
            failures.append("/header and /halos must both be groups")
            return failures

        attrs = handle["header"].attrs
        attr_names = set(attrs.keys())
        expected_attrs = set(HEADER_ATTRS)
        if attr_names != expected_attrs:
            failures.append(
                "header attribute set mismatch: missing {}, extra {}".format(
                    sorted(expected_attrs - attr_names), sorted(attr_names - expected_attrs)
                )
            )
        for name in sorted(attr_names & expected_attrs):
            expected_dtype = np.dtype(HEADER_ATTRS[name])
            actual = np.asarray(attrs[name])
            if actual.dtype != expected_dtype or actual.shape != ():
                failures.append(
                    "attribute {} has dtype {} shape {}, contract requires scalar {}".format(
                        name, actual.dtype, actual.shape, expected_dtype
                    )
                )

        halos = handle["halos"]
        dataset_names = set(halos.keys())
        expected_datasets = set(HALO_DATASETS)
        if dataset_names != expected_datasets:
            failures.append(
                "/halos dataset set mismatch: missing {}, extra {}".format(
                    sorted(expected_datasets - dataset_names),
                    sorted(dataset_names - expected_datasets),
                )
            )
        for name in sorted(dataset_names & expected_datasets):
            dtype, is_vec = HALO_DATASETS[name]
            dataset = halos[name]
            if not isinstance(dataset, h5py.Dataset):
                failures.append("/halos/{} is not a dataset".format(name))
                continue
            if dataset.dtype != np.dtype(dtype):
                failures.append(
                    "/halos/{} dtype {} != contract {}".format(name, dataset.dtype, np.dtype(dtype))
                )
            expected_chunks = CHUNK_VEC if is_vec else CHUNK_1D
            if is_vec:
                if dataset.ndim != 2 or dataset.shape[1] != 3:
                    failures.append(
                        "/halos/{} shape {} is not [n_halos, 3]".format(name, dataset.shape)
                    )
            elif dataset.ndim != 1:
                failures.append("/halos/{} shape {} is not [n_halos]".format(name, dataset.shape))
            if dataset.chunks != expected_chunks:
                failures.append(
                    "/halos/{} chunks {} != contract {}".format(
                        name, dataset.chunks, expected_chunks
                    )
                )
            failures += _filter_failures(dataset, "/halos/{}".format(name))
    return failures


def check_sidecar_structure(path: Path) -> List[str]:
    """forests.h5 must contain exactly the /ForestID int64 dataset."""
    failures = []
    with h5py.File(path, "r") as handle:
        objects = set(handle.keys())
        if objects != {"ForestID"}:
            failures.append("object set {} != {{'ForestID'}}".format(sorted(objects)))
            return failures
        dataset = handle["ForestID"]
        if not isinstance(dataset, h5py.Dataset):
            failures.append("/ForestID is not a dataset")
            return failures
        if dataset.dtype != np.dtype(np.int64):
            failures.append("/ForestID dtype {} != int64".format(dataset.dtype))
        if dataset.ndim != 1:
            failures.append("/ForestID shape {} is not 1-D".format(dataset.shape))
        if dataset.chunks != CHUNK_1D:
            failures.append("/ForestID chunks {} != contract {}".format(dataset.chunks, CHUNK_1D))
        failures += _filter_failures(dataset, "/ForestID")
    return failures


def check_manifest_binding(directory: Path, a_list_md5: str, manifest_path: Path) -> List[str]:
    """The mandatory manifest must actually describe the dataset being
    validated: the supplied a_list must be the one the conversion was bound to
    (provenance md5), and every .h5 file in the directory must match the
    emission checksum the writer recorded — otherwise count conservation could
    be 'satisfied' by an unrelated manifest, and uniform tampering (e.g. the
    same wrong physical header in every file) would evade the cross-file
    consistency checks."""
    failures = []
    with open(manifest_path) as handle:
        manifest = json.load(handle)
    recorded = manifest.get("provenance", {}).get("a_list", {}).get("md5")
    if recorded != a_list_md5:
        failures.append(
            "supplied a_list content md5 {} != manifest-recorded {} — this manifest does not "
            "describe the supplied a_list".format(a_list_md5, recorded)
        )
    outputs = manifest.get("outputs", {})
    if not outputs:
        failures.append("manifest records no emitted outputs (run write before validating)")
        return failures
    recorded_md5 = {}
    duplicates = set()
    for path, entry in outputs.items():
        name = Path(path).name
        if name in recorded_md5:
            duplicates.add(name)
        recorded_md5[name] = entry.get("md5")
    if duplicates:
        failures.append(
            "{} output basename(s) recorded under more than one manifest path (the workdir "
            "was written to multiple output directories?) — binding is ambiguous, refusing "
            "to validate: {}".format(len(duplicates), _examples(sorted(duplicates)))
        )
        return failures
    present = {p.name for p in directory.glob("*.h5")}
    unrecorded = sorted(present - set(recorded_md5))
    if unrecorded:
        failures.append(
            "{} file(s) not recorded as manifest outputs: {}".format(
                len(unrecorded), _examples(unrecorded)
            )
        )
    absent = sorted(set(recorded_md5) - present)
    if absent:
        failures.append(
            "{} manifest-recorded output(s) missing from the directory: {}".format(
                len(absent), _examples(absent)
            )
        )
    mismatched = [
        name
        for name in sorted(present & set(recorded_md5))
        if file_md5(directory / name) != recorded_md5[name]
    ]
    if mismatched:
        failures.append(
            "{} file(s) whose content differs from the manifest-recorded emission "
            "checksum: {}".format(len(mismatched), _examples(mismatched))
        )
    return failures


# ---------------------------------------------------------------------------
# Bounded dataset access (after structural conformance)
# ---------------------------------------------------------------------------


class _Snapshots:
    """Bounded access to the emitted snapshot files.

    The whole-dataset battery this replaced read every file's complete /halos
    arrays into one list and handed that list to every check. Each accessor
    here opens one file, reads only what the caller names, and closes it again,
    so what stays resident is whatever window the calling check chose to hold —
    one snapshot for the per-snapshot checks, the adjacent pair for progenitor
    closure, one chunk for the identity passes, and nothing at all for the
    checks that need only row counts.
    """

    def __init__(self, directory, n_snapshots: int):
        self.directory = Path(directory)
        self.n_snapshots = int(n_snapshots)
        self._rows: Dict[int, int] = {}

    def path(self, snap: int) -> Path:
        return self.directory / snapshot_h5_name(snap)

    def header(self, snap: int) -> dict:
        """One file's header attributes, as numpy scalars."""
        with h5py.File(self.path(snap), "r") as handle:
            return {name: np.asarray(handle["header"].attrs[name])[()] for name in HEADER_ATTRS}

    def field_rows(self, snap: int) -> Dict[str, int]:
        """Row count of every /halos dataset of one file, in HALO_DATASETS
        order, read from the shapes without touching the data."""
        with h5py.File(self.path(snap), "r") as handle:
            return {name: int(handle["halos"][name].shape[0]) for name in HALO_DATASETS}

    def rows(self, snap: int) -> int:
        """Halos in one file — /halos/MostBoundID's row count, which is the
        same number the whole-dataset battery took from that array's ``size``.
        Cached, so the count costs one int per snapshot."""
        if snap not in self._rows:
            with h5py.File(self.path(snap), "r") as handle:
                self._rows[snap] = int(handle["halos"]["MostBoundID"].shape[0])
        return self._rows[snap]

    def load(self, snap: int, fields) -> Dict[str, np.ndarray]:
        """Named /halos datasets of one file, whole."""
        with h5py.File(self.path(snap), "r") as handle:
            return {name: handle["halos"][name][...] for name in fields}

    def chunks(self, snap: int, fields, chunk_rows: int):
        """Named /halos datasets of one file, yielded together in row-aligned
        blocks of at most ``chunk_rows`` rows."""
        with h5py.File(self.path(snap), "r") as handle:
            datasets = [handle["halos"][name] for name in fields]
            total = int(datasets[0].shape[0])
            for start in range(0, total, chunk_rows):
                stop = min(start + chunk_rows, total)
                yield tuple(dataset[start:stop] for dataset in datasets)


# ---------------------------------------------------------------------------
# Stage B: semantic checks
# ---------------------------------------------------------------------------


def check_headers(snapshots: _Snapshots, a_list: np.ndarray) -> Tuple[List[str], List[str]]:
    """Invariant 5 (header consistency) plus format_version, links_adjacent,
    the int32 topology bound (invariant 2), and a_list <-> scale_factor.

    The run-scoped comparison asks only whether the files agree, so a carried
    set of the distinct values per attribute answers it exactly, without a list
    of every file's header.
    """
    failures = []
    run_scoped_failures = []
    observed = {attr: set() for attr in RUN_SCOPED_ATTRS}
    for snap in range(snapshots.n_snapshots):
        name = snapshot_h5_name(snap)
        header = snapshots.header(snap)
        for attr in RUN_SCOPED_ATTRS:
            observed[attr].add(repr(header[attr].item()))
        if int(header["format_version"]) != FORMAT_VERSION:
            failures.append(
                "{}: format_version {} != {}".format(name, header["format_version"], FORMAT_VERSION)
            )
        if int(header["links_adjacent"]) != 1:
            failures.append("{}: links_adjacent {} != 1".format(name, header["links_adjacent"]))
        if int(header["snapshot_number"]) != snap:
            failures.append(
                "{}: snapshot_number {} != filename index {}".format(
                    name, header["snapshot_number"], snap
                )
            )
        if float(header["scale_factor"]) != float(a_list[snap]):
            failures.append(
                "{}: scale_factor {!r} != a_list[{}] = {!r}".format(
                    name, float(header["scale_factor"]), snap, float(a_list[snap])
                )
            )
        n_halos = int(header["n_halos"])
        if n_halos > np.iinfo(np.int32).max:
            failures.append("{}: n_halos {} exceeds the int32 topology bound".format(name, n_halos))
        for field, rows in snapshots.field_rows(snap).items():
            if rows != n_halos:
                failures.append(
                    "{}: dataset {} has {} rows, header n_halos is {}".format(
                        name, field, rows, n_halos
                    )
                )
        snapnum = snapshots.load(snap, ("SnapNum",))["SnapNum"]
        bad = snapnum != snap
        if bad.any():
            failures.append(
                "{}: {} SnapNum value(s) != snapshot_number {}; examples: {}".format(
                    name, int(bad.sum()), snap, _examples(snapnum[bad].tolist())
                )
            )
    for attr in RUN_SCOPED_ATTRS:
        values = observed[attr]
        if len(values) > 1:
            run_scoped_failures.append(
                "{} differs across files: {}".format(attr, _examples(sorted(values)))
            )
    return failures, run_scoped_failures


def check_slab_order(snapshots: _Snapshots) -> List[str]:
    """Invariant 3: ascending unique |MostBoundID| within every file.

    INT64_MIN is rejected explicitly: signed absolute value overflows on it
    (``abs(INT64_MIN) == INT64_MIN``), so it cannot be the negation of any
    positive source-catalog id and would silently corrupt every
    magnitude-based comparison — including in single-halo slabs where no
    adjacent-order comparison would otherwise fire."""
    failures = []
    for snap in range(snapshots.n_snapshots):
        mostbound = snapshots.load(snap, ("MostBoundID",))["MostBoundID"]
        sentinel = mostbound == _INT64_MIN
        if sentinel.any():
            failures.append(
                "{}: {} MostBoundID value(s) equal INT64_MIN, whose magnitude overflows "
                "signed int64; example rows: {}".format(
                    snapshot_h5_name(snap),
                    int(sentinel.sum()),
                    _examples(np.nonzero(sentinel)[0].tolist()),
                )
            )
        mb_abs = np.abs(mostbound)
        if mb_abs.size > 1:
            bad = np.nonzero(mb_abs[1:] <= mb_abs[:-1])[0]
            if bad.size:
                examples = [
                    "(row={}, |MostBoundID|={}, next {})".format(
                        int(r), int(mb_abs[r]), int(mb_abs[r + 1])
                    )
                    for r in bad[:5]
                ]
                failures.append(
                    "{}: not strictly ascending in |MostBoundID| at {} position(s); "
                    "examples: {}".format(snapshot_h5_name(snap), bad.size, ", ".join(examples))
                )
    return failures


def _range_failures(
    values: np.ndarray, n_target: int, field: str, snap: int, allow_null: bool = True
) -> List[str]:
    low = -1 if allow_null else 0
    bad = (values < low) | (values >= n_target)
    if bad.any():
        return [
            "{}: {} {} value(s) outside [{}, {}); examples: {}".format(
                snapshot_h5_name(snap),
                int(bad.sum()),
                field,
                low,
                n_target,
                _examples(values[bad].tolist()),
            )
        ]
    return []


def check_link_ranges(snapshots: _Snapshots) -> List[str]:
    """Invariant 6's range component, resolved per the Link Scope table.
    Neighbours beyond the dataset have zero halos, so the final snapshot's
    Descendant and the first snapshot's FirstProgenitor may only be -1.

    The widest data dependency here is a neighbour's ROW COUNT, which comes
    from its dataset shape, so only the snapshot under test is resident.
    """
    failures = []
    last = snapshots.n_snapshots - 1
    for snap in range(snapshots.n_snapshots):
        data = snapshots.load(
            snap,
            (
                "Descendant",
                "FirstProgenitor",
                "NextProgenitor",
                "NextHaloInFOFgroup",
                "FirstHaloInFOFgroup",
            ),
        )
        n = snapshots.rows(snap)
        n_next = snapshots.rows(snap + 1) if snap < last else 0
        n_prev = snapshots.rows(snap - 1) if snap > 0 else 0
        failures += _range_failures(data["Descendant"], n_next, "Descendant", snap)
        failures += _range_failures(data["FirstProgenitor"], n_prev, "FirstProgenitor", snap)
        for field in ("NextProgenitor", "NextHaloInFOFgroup"):
            failures += _range_failures(data[field], n, field, snap)
        failures += _range_failures(
            data["FirstHaloInFOFgroup"], n, "FirstHaloInFOFgroup", snap, allow_null=False
        )
    return failures


def _frontier_duplicates(node: np.ndarray) -> np.ndarray:
    unique, counts = np.unique(node, return_counts=True)
    return unique[counts > 1]


def check_fof_chains(snapshots: _Snapshots) -> List[str]:
    """Invariant 6's chain component: FoF chains are cycle-free, terminate at
    -1, every FirstHaloInFOFgroup target is a self-referencing central, and
    the chains starting at the centrals cover every halo exactly once with a
    consistent FirstHaloInFOFgroup along each chain."""
    failures = []
    for snap in range(snapshots.n_snapshots):
        name = snapshot_h5_name(snap)
        data = snapshots.load(snap, ("FirstHaloInFOFgroup", "NextHaloInFOFgroup"))
        first = data["FirstHaloInFOFgroup"].astype(np.int64)
        nxt = data["NextHaloInFOFgroup"].astype(np.int64)
        n = first.size
        if n == 0:
            continue
        idx = np.arange(n, dtype=np.int64)
        bad = first[first] != first
        if bad.any():
            failures.append(
                "{}: {} FirstHaloInFOFgroup target(s) are not self-referencing centrals; "
                "example rows: {}".format(name, int(bad.sum()), _examples(idx[bad].tolist()))
            )
            continue
        visited = np.zeros(n, dtype=bool)
        node = idx[first == idx]
        central = node.copy()
        ok = True
        while node.size:
            duplicates = _frontier_duplicates(node)
            if duplicates.size:
                failures.append(
                    "{}: FoF chain(s) converge on the same halo; example rows: {}".format(
                        name, _examples(duplicates.tolist())
                    )
                )
                ok = False
                break
            revisit = visited[node]
            if revisit.any():
                failures.append(
                    "{}: FoF chain cycle or duplicate membership at row(s) {}".format(
                        name, _examples(node[revisit].tolist())
                    )
                )
                ok = False
                break
            visited[node] = True
            mismatch = first[node] != central
            if mismatch.any():
                failures.append(
                    "{}: {} chain member(s) whose FirstHaloInFOFgroup is not the chain's "
                    "central; example rows: {}".format(
                        name, int(mismatch.sum()), _examples(node[mismatch].tolist())
                    )
                )
                ok = False
                break
            step = nxt[node]
            keep = step != -1
            node = step[keep]
            central = central[keep]
        if ok and not visited.all():
            missing = idx[~visited]
            failures.append(
                "{}: {} halo(s) not reachable from any FoF central (orphaned or cyclic "
                "chain); example rows: {}".format(name, missing.size, _examples(missing.tolist()))
            )
    return failures


def check_progenitor_closure(snapshots: _Snapshots) -> List[str]:
    """Producer round-trip closure: for every snapshot pair (N, N+1), the
    progenitor chains recorded at N+1 (FirstProgenitor into N, then
    NextProgenitor within N) cover exactly the N-halos whose Descendant is
    non-null, each exactly once, with every chain member's Descendant naming
    the chain's owner. NextProgenitor's same-file scope is the range check;
    here every non-null NextProgenitor must also share the descendant.

    This is the battery's widest per-record dependency: snapshot ``snap``'s
    Descendant and NextProgenitor against snapshot ``snap + 1``'s
    FirstProgenitor — the two-snapshot window everything else fits inside.
    """
    failures = []
    for snap in range(snapshots.n_snapshots):
        name = snapshot_h5_name(snap)
        data = snapshots.load(snap, ("Descendant", "NextProgenitor"))
        desc = data["Descendant"].astype(np.int64)
        nxt = data["NextProgenitor"].astype(np.int64)
        n = desc.size

        stray = (nxt != -1) & (desc == -1)
        if stray.any():
            failures.append(
                "{}: {} halo(s) carry NextProgenitor but no Descendant; example rows: "
                "{}".format(name, int(stray.sum()), _examples(np.nonzero(stray)[0].tolist()))
            )
        both = (nxt != -1) & (desc != -1)
        if both.any():
            sibling_desc = desc[nxt[both]]
            mismatch = sibling_desc != desc[both]
            if mismatch.any():
                rows = np.nonzero(both)[0][mismatch]
                failures.append(
                    "{}: {} NextProgenitor sibling(s) with a different Descendant; "
                    "example rows: {}".format(name, int(mismatch.sum()), _examples(rows.tolist()))
                )

        if snap + 1 >= snapshots.n_snapshots:
            continue
        first = snapshots.load(snap + 1, ("FirstProgenitor",))["FirstProgenitor"].astype(np.int64)
        visited = np.zeros(n, dtype=bool)
        owners = np.nonzero(first != -1)[0]
        node = first[owners]
        dest = owners
        ok = True
        while node.size:
            duplicates = _frontier_duplicates(node)
            if duplicates.size:
                failures.append(
                    "{}: progenitor chain(s) converge on the same halo; example rows: "
                    "{}".format(name, _examples(duplicates.tolist()))
                )
                ok = False
                break
            revisit = visited[node]
            if revisit.any():
                failures.append(
                    "{}: progenitor chain cycle or duplicate membership at row(s) {}".format(
                        name, _examples(node[revisit].tolist())
                    )
                )
                ok = False
                break
            visited[node] = True
            mismatch = desc[node] != dest
            if mismatch.any():
                failures.append(
                    "{}: {} progenitor chain member(s) whose Descendant is not the chain "
                    "owner; example rows: {}".format(
                        name, int(mismatch.sum()), _examples(node[mismatch].tolist())
                    )
                )
                ok = False
                break
            step = nxt[node]
            keep = step != -1
            node = step[keep]
            dest = dest[keep]
        if ok:
            has_desc = desc != -1
            unclaimed = has_desc & ~visited
            if unclaimed.any():
                failures.append(
                    "{}: {} halo(s) with a Descendant appear in no progenitor chain; "
                    "example rows: {}".format(
                        name,
                        int(unclaimed.sum()),
                        _examples(np.nonzero(unclaimed)[0].tolist()),
                    )
                )
    return failures


class _IdentityBits:
    """The exact one-bit-per-halo claim structure behind ``check_identity``.

    ``base[forest] + rank`` is the position a halo would occupy in the global
    lexsort of (ForestIndex, HaloRankInForest) the whole-dataset battery built,
    so ranks are dense and pairs unique within every forest if and only if
    every halo claims an in-range slot and no slot is claimed twice. One bit
    per halo is 2.86 GB at the production 22.9e9 halos, and it is EXACT where
    an aggregate is not: forest counts ``[3, 2]`` admit ``[0,0,2 | 1,1]`` with
    the same sum, maximum and modular sum of squares as the dense
    ``[0,1,2 | 0,1]``.

    ``check_identity`` releases it in a ``finally``, so it is released whether
    that check passes, fails, or raises.
    """

    def __init__(self, n_slots: int):
        self.n_slots = int(n_slots)
        self.bits = np.zeros((self.n_slots + 7) // 8, dtype=np.uint8)
        #: bytes this structure holds while it is open — the figure the storage
        #: and memory envelope wants reported rather than estimated
        self.peak_bytes = int(self.bits.nbytes)

    def claim(self, slots: np.ndarray) -> np.ndarray:
        """Claim one chunk's slots; return the mask of those already claimed —
        before this chunk, or earlier within it."""
        bits = self.bits
        byte = slots >> 3
        mask = (1 << (slots & 7)).astype(np.uint8)
        taken = (bits[byte] & mask) != 0
        # ...or twice inside this chunk: sorting the chunk's slots makes equal
        # slots adjacent, and the first of each run keeps the slot
        order = np.argsort(slots, kind="stable")
        sorted_slots = slots[order]
        repeat = np.zeros(sorted_slots.size, dtype=bool)
        if sorted_slots.size > 1:
            repeat[1:] = sorted_slots[1:] == sorted_slots[:-1]
        taken[order] = taken[order] | repeat
        if sorted_slots.size:
            unique_slots = sorted_slots[~repeat]
            unique_bytes = unique_slots >> 3
            unique_masks = (1 << (unique_slots & 7)).astype(np.uint8)
            # distinct slots can share a byte, so the bits of one byte are OR-ed
            # together before the store: a buffered ``|=`` over repeated byte
            # indices would drop all but one of them
            starts = np.nonzero(np.r_[True, unique_bytes[1:] != unique_bytes[:-1]])[0]
            bits[unique_bytes[starts]] |= np.bitwise_or.reduceat(unique_masks, starts)
        return taken

    def first_unheld(self, base: int, count: int) -> int:
        """The first rank in one forest that no halo holds, read back out of
        the bitset for a failure message. One exists whenever that forest
        rejected a pair: its ``count`` halos then claim at most ``count - 1``
        distinct slots. Scanned in bounded blocks so a single huge forest's
        diagnostic cannot itself allocate without limit."""
        for start in range(0, count, _MISSING_RANK_SCAN_SLOTS):
            stop = min(start + _MISSING_RANK_SCAN_SLOTS, count)
            slots = np.arange(base + start, base + stop, dtype=np.int64)
            held = (self.bits[slots >> 3] >> (slots & 7).astype(np.uint8)) & np.uint8(1)
            free = np.nonzero(held == 0)[0]
            if free.size:
                return int(start + int(free[0]))
        return -1

    def close(self) -> None:
        self.bits = None


class _ForestTable:
    """Halo count and bitset base for every ForestIndex in [0, n_forests_total).

    Two ``n_forests_total``-sized int64 arrays — the forest-count-sized
    metadata the battery is allowed to hold. ForestIndex values OUTSIDE that
    range are not here at all: their groups are settled by
    :class:`_PairOrdering`, because there can be one distinct such value per
    halo and a per-value table would grow with the dataset.
    """

    def __init__(self, n_forests_total: int, counts: np.ndarray):
        self.n_forests_total = int(n_forests_total)
        self.counts = counts
        self.bases = np.zeros(counts.size, dtype=np.int64)
        if counts.size > 1:
            np.cumsum(counts[:-1], out=self.bases[1:])


class _PairOrdering:
    """Bounded EXACT ordering of the (ForestIndex, HaloRankInForest) pairs
    whose ForestIndex falls outside ``[0, n_forests_total)``.

    Those pairs cannot be summarised in memory. The whole-dataset battery
    sorted on ForestIndex alone, so halos sharing an out-of-range value formed
    their own group and were judged dense within it — reproducing that needs
    the halo count of every distinct out-of-range value, and a structurally
    conformant dataset may carry one distinct value PER HALO (the structural
    checks validate dtype, shape and chunks, not range). A dict keyed by value
    therefore grows with the dataset, which is the thing this slice exists to
    prevent, and counting distinct values in a stream is not bounded without an
    ordering. This is the plan's second sanctioned mechanism: "an external
    ordering of (ForestIndex, rank)".

    **Nothing here is allowed to scale with the number of runs.** Sorted runs
    are spilled to a private directory and merged AS THEY ACCUMULATE: a level
    holding ``_ORDERING_FANIN`` runs is merged into one run of the next level,
    so the list of live runs is bounded by the fan-in times the number of
    levels (about 50 entries even for the 349,426 runs a wholly out-of-range
    production dataset would produce) rather than by the run count. Each run's
    size is recorded when it is written and subtracted when it is unlinked, so
    the disk accounting costs O(1) per run — an earlier formulation rescanned
    the directory after every spill, which is O(R^2) file stats and would have
    spent about 49 hours on that dataset before the merge began.

    **On a dataset that satisfies the density condition nothing is spilled at
    all**: no directory is created, ``finish`` returns ``None`` and
    ``peak_bytes`` stays 0. The merge is a plain Python ``heapq.merge`` because
    it only ever runs on an already-broken dataset; correctness and
    boundedness matter there, throughput does not.

    The owner must :meth:`close` it on every path; ``check_identity`` does so
    in a ``finally``. ``close`` reports a removal it could not confirm rather
    than swallowing it, and :meth:`raise_if_unremoved` turns that into a real
    error once no other exception is in flight.
    """

    def __init__(self):
        self._directory: Optional[Path] = None
        #: runs not yet merged, by level; every level holds fewer than
        #: _ORDERING_FANIN of them, so this list is bounded by the fan-in times
        #: the level count, never by the number of runs written
        self._levels: List[List[Tuple[Path, int]]] = []
        self._buffer: List[np.ndarray] = []
        self._buffered = 0
        self._serial = 0
        self._held = 0
        self._ordered: Optional[Tuple[Path, int]] = None
        #: pairs handed to :meth:`add`, i.e. out-of-range halos in the dataset
        self.rows = 0
        #: largest number of bytes this ordering has held on disk at once — the
        #: figure the storage envelope wants reported rather than estimated
        self.peak_bytes = 0
        #: set by :meth:`close` when it could not confirm the directory is gone
        self.unremoved: Optional[Path] = None

    # -- writing -----------------------------------------------------------

    def add(self, forest: np.ndarray, rank: np.ndarray) -> None:
        """Record one chunk's out-of-range pairs.

        Taken in ``_ORDERING_RUN_ROWS`` slices rather than whole, so the run
        buffer is bounded by that constant however large the caller's chunk is
        — otherwise a chunk of out-of-range halos would set the buffer size
        instead.
        """
        if forest.size == 0:
            return
        self.rows += int(forest.size)
        for start in range(0, int(forest.size), _ORDERING_RUN_ROWS):
            stop = min(start + _ORDERING_RUN_ROWS, int(forest.size))
            pairs = np.empty(stop - start, dtype=_PAIR_DTYPE)
            pairs["forest"] = forest[start:stop]
            pairs["rank"] = rank[start:stop]
            self._buffer.append(pairs)
            self._buffered += int(pairs.size)
            if self._buffered >= _ORDERING_RUN_ROWS:
                self._spill()

    def _spill(self) -> None:
        if not self._buffer:
            return
        block = self._buffer[0] if len(self._buffer) == 1 else np.concatenate(self._buffer)
        self._buffer = []
        self._buffered = 0
        block.sort(order=("forest", "rank"))
        self._promote(self._write(block))

    def _write(self, block: np.ndarray) -> Tuple[Path, int]:
        """Write one sorted run and charge its bytes to the running total."""
        path = self._new_run()
        block.tofile(str(path))
        return self._charge(path, int(block.size))

    def _charge(self, path: Path, rows: int) -> Tuple[Path, int]:
        size = rows * _PAIR_DTYPE.itemsize
        self._held += size
        self.peak_bytes = max(self.peak_bytes, self._held)
        return path, size

    def _promote(self, run: Tuple[Path, int]) -> None:
        """Add one run at level 0, cascading a full level into a single run of
        the level above. This is what keeps the live-run list bounded."""
        level = 0
        while True:
            while len(self._levels) <= level:
                self._levels.append([])
            self._levels[level].append(run)
            if len(self._levels[level]) < _ORDERING_FANIN:
                return
            group = self._levels[level]
            self._levels[level] = []
            run = self._merge(group)
            level += 1

    def _new_run(self) -> Path:
        if self._directory is None:
            self._directory = Path(tempfile.mkdtemp(prefix=ORDERING_DIR_PREFIX))
        self._serial += 1
        return self._directory / "run_{:08d}.pairs".format(self._serial)

    # -- merging -----------------------------------------------------------

    def finish(self) -> Optional[Path]:
        """Merge the runs down to one fully ordered file, or ``None`` when no
        out-of-range pair was ever recorded. Idempotent."""
        if self._ordered is not None:
            return self._ordered[0]
        self._spill()
        runs = [run for level in self._levels for run in level]
        self._levels = []
        while len(runs) > 1:
            merged = []
            for start in range(0, len(runs), _ORDERING_FANIN):
                group = runs[start : start + _ORDERING_FANIN]
                merged.append(group[0] if len(group) == 1 else self._merge(group))
            runs = merged
        self._ordered = runs[0] if runs else None
        return self._ordered[0] if self._ordered else None

    def _merge(self, runs: List[Tuple[Path, int]]) -> Tuple[Path, int]:
        target = self._new_run()
        readers = [self._iter_pairs(path) for path, _ in runs]
        written = 0
        try:
            with open(str(target), "wb") as handle:
                block: List[tuple] = []
                for row in heapq.merge(*readers):
                    block.append(row)
                    if len(block) >= _ORDERING_MERGE_ROWS:
                        np.asarray(block, dtype=_PAIR_DTYPE).tofile(handle)
                        written += len(block)
                        block = []
                if block:
                    np.asarray(block, dtype=_PAIR_DTYPE).tofile(handle)
                    written += len(block)
        finally:
            for reader in readers:
                reader.close()
        # charged while the inputs are still on disk: that IS the peak
        merged = self._charge(target, written)
        for path, size in runs:
            path.unlink()
            self._held -= size
        return merged

    @staticmethod
    def _iter_pairs(path: Path):
        """Ascending (ForestIndex, rank) tuples from one sorted run."""
        for block in _PairOrdering.blocks(path, _ORDERING_MERGE_READ_ROWS):
            for row in zip(block["forest"].tolist(), block["rank"].tolist()):
                yield row

    # -- reading -----------------------------------------------------------

    @staticmethod
    def blocks(path: Path, rows: Optional[int] = None):
        """Sequential blocks of one pair file; never more than one resident.

        ``rows`` is resolved from the module constant on every call rather than
        bound as a default: a default argument would freeze the value at import
        and silently ignore anything that changed it, including the tests that
        shrink these buffers to exercise the multi-round merge.
        """
        rows = _ORDERING_READ_ROWS if rows is None else int(rows)
        with open(str(path), "rb") as handle:
            while True:
                block = np.fromfile(handle, dtype=_PAIR_DTYPE, count=rows)
                if block.size == 0:
                    return
                yield block

    # -- lifetime ----------------------------------------------------------

    def close(self) -> None:
        """Remove the spill directory, reporting a removal it could not
        confirm instead of swallowing it.

        Ownership is released only on CONFIRMED absence: the directory
        reference is dropped when, and only when, the directory is gone. A
        removal that fails leaves ``unremoved`` set, logs a warning naming the
        path and the bytes at stake, and leaves the reference in place so a
        later call retries.

        **This never raises.** It is called from a ``finally`` and must not
        replace the exception that put it there; :meth:`raise_if_unremoved` is
        how a cleanup failure becomes an error on the paths where nothing else
        is in flight. Safe to call when nothing was ever spilled, and safe to
        call twice.
        """
        self._buffer = []
        self._buffered = 0
        self._levels = []
        self._ordered = None
        directory = self._directory
        if directory is None:
            return
        error: Optional[BaseException] = None
        for _attempt in (1, 2):
            try:
                shutil.rmtree(str(directory))
            except OSError as exc:
                error = exc
            try:
                present = directory.exists()
            except OSError as exc:  # pragma: no cover - stat of a broken mount
                error = exc
                present = True
            if not present:
                self._directory = None
                self._held = 0
                self.unremoved = None
                return
        self.unremoved = directory
        _log(
            "validate: WARNING — could not remove the identity ordering directory {} ({}); "
            "about {} byte(s) may remain. Remove it by hand.".format(directory, error, self._held)
        )

    def raise_if_unremoved(self) -> None:
        """Turn a cleanup failure into a real error, on the paths where no
        other exception is in flight. Call it AFTER the ``finally`` that closed
        the ordering, never inside it."""
        if self.unremoved is not None:
            raise ConverterError(
                "the identity ordering directory {} could not be removed; remove it by hand "
                "before re-running".format(self.unremoved)
            )


class _GroupDensity:
    """Per-forest density and uniqueness for ONE out-of-range ForestIndex
    group, fed the group's ranks in ascending order.

    It reproduces exactly what :class:`_IdentityBits` decides for an in-range
    forest — a rank outside ``[0, count)``, or an already-claimed rank, is
    rejected and nothing else is — using the ordering instead of a bitset. In
    ascending order the first occurrence of a value claims it, so a repeat is
    simply a rank equal to its predecessor; the accepted rows are therefore the
    distinct in-range ranks, and ``count - accepted`` rejected rows is the same
    number the bitset would report.
    """

    def __init__(self, value: int, count: int):
        self.value = int(value)
        self.count = int(count)
        self.n_bad = 0
        self.examples: List[Tuple[int, int]] = []
        self._probe = 0
        self._gap: Optional[int] = None
        self._previous: Optional[int] = None

    def feed(self, ranks: np.ndarray) -> None:
        if ranks.size == 0:
            return
        repeat = np.zeros(ranks.size, dtype=bool)
        repeat[1:] = ranks[1:] == ranks[:-1]
        if self._previous is not None:
            repeat[0] = bool(ranks[0] == self._previous)
        self._previous = int(ranks[-1])
        in_range = (ranks >= 0) & (ranks < self.count)
        rejected = ~in_range | repeat
        bad = np.nonzero(rejected)[0]
        if bad.size:
            self.n_bad += int(bad.size)
            room = 5 - len(self.examples)
            if room > 0:
                # ranks arrive ascending, so the earliest rejected rows ARE the
                # lowest (ForestIndex, rank) pairs this group can offer
                for value in ranks[bad[:room]].tolist():
                    self.examples.append((self.value, int(value)))
        if self._gap is None:
            claimed = ranks[in_range & ~repeat]
            if claimed.size:
                expected = self._probe + np.arange(claimed.size, dtype=np.int64)
                mismatch = np.nonzero(claimed != expected)[0]
                if mismatch.size:
                    self._gap = self._probe + int(mismatch[0])
                else:
                    self._probe += int(claimed.size)

    @property
    def first_unheld(self) -> int:
        """The first rank in ``[0, count)`` no halo of this group holds — the
        same diagnostic ``_IdentityBits.first_unheld`` reads back out of the
        bitset. -1 when the group is complete, which cannot happen for a group
        that rejected a row."""
        gap = self._probe if self._gap is None else self._gap
        return gap if gap < self.count else -1


class _OrderedRanks:
    """Sequential reader that hands out the next ``n`` ranks of the ordered
    pair file, holding one block at a time."""

    def __init__(self, path: Path):
        self._blocks = _PairOrdering.blocks(path)
        self._block: Optional[np.ndarray] = None
        self._offset = 0

    def take(self, count: int):
        remaining = int(count)
        while remaining > 0:
            if self._block is None or self._offset >= self._block.size:
                self._block = next(self._blocks)
                self._offset = 0
            take = min(remaining, int(self._block.size) - self._offset)
            yield self._block["rank"][self._offset : self._offset + take]
            self._offset += take
            remaining -= take

    def close(self) -> None:
        self._blocks.close()


def _ordered_groups(path: Path):
    """Yield ``(value, count)`` for every distinct ForestIndex in the ordering,
    in ascending value order, from a single sequential pass."""
    current: Optional[int] = None
    count = 0
    for block in _PairOrdering.blocks(path):
        forest = block["forest"]
        starts = np.nonzero(np.r_[True, forest[1:] != forest[:-1]])[0]
        lengths = np.diff(np.r_[starts, forest.size])
        for value, length in zip(forest[starts].tolist(), lengths.tolist()):
            if current is None or value == current:
                current = value
                count += length
            else:
                yield current, count
                current, count = value, length
    if current is not None:
        yield current, count


def _scan_ordering(path: Path) -> Tuple[int, List[int], int, List[Tuple[int, int, int]]]:
    """Settle everything the out-of-range ForestIndex values contribute:
    how many distinct values there are, the five lowest of them, how many of
    their halos violate per-forest density/uniqueness, and the five lowest
    violating pairs with the rank their group does not hold.

    Two sequential passes over the same ordered file — the first to learn each
    group's size, the second to judge it — because a group's rank bounds are
    its own halo count and that is not known until the group ends. Neither pass
    holds more than one block.
    """
    distinct = 0
    lowest: List[int] = []
    n_bad = 0
    examples: List[Tuple[int, int, int]] = []
    rows = _OrderedRanks(path)
    groups = _ordered_groups(path)
    try:
        for value, count in groups:
            distinct += 1
            if len(lowest) < 5:
                lowest.append(int(value))
            group = _GroupDensity(value, count)
            for ranks in rows.take(count):
                group.feed(ranks)
            if group.n_bad:
                n_bad += group.n_bad
                if len(examples) < 5:
                    unheld = group.first_unheld
                    for forest_value, rank_value in group.examples[: 5 - len(examples)]:
                        examples.append((forest_value, rank_value, unheld))
    finally:
        # both generators hold an open file handle across their yields, so an
        # exception in the group loop must close them rather than leave that to
        # collection
        groups.close()
        rows.close()
    return distinct, lowest, n_bad, examples


def _identity_counts(
    snapshots: _Snapshots, n_forests_total: int, ordering: _PairOrdering
) -> Tuple[np.ndarray, int, Optional[int]]:
    """First identity pass: halos per in-range ForestIndex, the dataset's halo
    total, and the largest HaloRankInForest. Out-of-range pairs go to
    ``ordering`` rather than to any per-value table in memory.

    Between them these settle the density condition over
    ``[0, n_forests_total)`` and the maximum-rank condition, and they size both
    the bitset the second pass claims into and the groups within it.
    """
    counts = np.zeros(max(int(n_forests_total), 0), dtype=np.int64)
    total = 0
    measured_max = None
    for snap in range(snapshots.n_snapshots):
        for forest, rank in snapshots.chunks(
            snap, ("ForestIndex", "HaloRankInForest"), IDENTITY_CHUNK_ROWS
        ):
            total += int(forest.size)
            if rank.size:
                block_max = int(rank.max())
                measured_max = block_max if measured_max is None else max(measured_max, block_max)
            in_range = (forest >= 0) & (forest < n_forests_total)
            in_range_values = forest[in_range]
            if in_range_values.size:
                counts += np.bincount(in_range_values, minlength=counts.size)
            if not bool(in_range.all()):
                stray = ~in_range
                ordering.add(forest[stray], rank[stray])
    return counts, total, measured_max


def check_identity(
    snapshots: _Snapshots, n_forests_total: int, max_rank_header: int
) -> Tuple[List[str], int, int]:
    """Invariant 4: (ForestIndex, HaloRankInForest) unique across the dataset,
    ForestIndex dense over [0, n_forests_total), per-forest ranks dense, and
    the measured maximum rank equal to the header value.

    Three independent conditions, each still able to fail on its own and each
    reported in the order the whole-dataset formulation reported it:

    (a) ForestIndex dense over ``[0, n_forests_total)`` — settled by the
        counting pass over the in-range values plus the distinct-value count
        the ordering yields for everything outside that range;
    (b) per-forest ranks dense and unique over ``0 .. count-1`` — settled
        exactly by :class:`_IdentityBits` for in-range forests, one bit per
        halo, and by :class:`_GroupDensity` over :class:`_PairOrdering` for the
        out-of-range values that cannot have an in-memory table;
    (c) the measured maximum rank equals ``max_halo_rank_in_forest`` — a
        running maximum over the same counting pass.

    Returns the failures, the bitset's peak bytes and the ordering's peak
    on-disk bytes, which the caller reports rather than estimates. The bitset
    is released on the success, failure and exception paths; the ordering's
    spill directory is removed on all three too, and a removal that cannot be
    CONFIRMED is reported — logged from ``close`` so an exception already in
    flight survives, and raised afterwards on the paths where none is.

    **The (b) violation count and its five examples are this formulation's
    own.** The whole-dataset battery counted positions in a global lexsort of
    every pair, and reproducing that count exactly would need a multiplicity
    per slot instead of a bit (>= 1 byte per halo, 22.9 GB at production) — the
    memory wall this battery exists to remove. What is DETECTED is identical: a
    rank outside its forest's ``[0, count)``, or a rank already claimed within
    that forest, IS a per-forest density or pair-uniqueness violation, and
    nothing else is. The message shape is identical, the examples are still the
    five lowest ``(ForestIndex, rank)`` pairs that violated, and ``expected`` in
    each is the first rank that forest holds no halo for.
    """
    ordering = _PairOrdering()
    try:
        failures, bitset_bytes = _identity_conditions(
            snapshots, n_forests_total, max_rank_header, ordering
        )
    finally:
        # close() never raises: it is called from a finally and must not
        # replace an exception already in flight, so it records instead
        ordering.close()
    # ...and here, where nothing else is in flight, a cleanup failure IS the
    # error, rather than a warning nobody acts on
    ordering.raise_if_unremoved()
    return failures, bitset_bytes, ordering.peak_bytes


def _identity_conditions(
    snapshots: _Snapshots,
    n_forests_total: int,
    max_rank_header: int,
    ordering: "_PairOrdering",
) -> Tuple[List[str], int]:
    """The three identity conditions themselves; :func:`check_identity` owns
    the ordering's lifetime around this."""
    failures: List[str] = []
    bitset_bytes = 0
    counts, total, measured_max = _identity_counts(snapshots, n_forests_total, ordering)
    if total == 0:
        if n_forests_total != 0:
            failures.append(
                "dataset has no halos but n_forests_total is {}".format(n_forests_total)
            )
        return failures, bitset_bytes

    ordered_path = ordering.finish()
    if ordered_path is None:
        stray_distinct, stray_lowest, stray_bad, stray_examples = 0, [], 0, []
    else:
        stray_distinct, stray_lowest, stray_bad, stray_examples = _scan_ordering(ordered_path)

    distinct = int(np.count_nonzero(counts)) + stray_distinct
    if stray_distinct or distinct != n_forests_total:
        lowest = sorted(np.flatnonzero(counts)[:5].tolist() + stray_lowest)[:5]
        failures.append(
            "ForestIndex values are not dense over [0, {}); {} distinct value(s) observed, "
            "examples: {}".format(n_forests_total, distinct, _examples(lowest))
        )

    forests = _ForestTable(n_forests_total, counts)
    bits = _IdentityBits(int(counts.sum()))
    try:
        bitset_bytes = bits.peak_bytes
        n_bad = stray_bad
        # (ForestIndex, rank, expected) triples; expected is None for the
        # in-range candidates until the bitset pass has finished and can be
        # read back for the rank their forest does not hold
        examples: List[Tuple[int, int, Optional[int]]] = list(stray_examples)
        for snap in range(snapshots.n_snapshots):
            for forest, rank in snapshots.chunks(
                snap, ("ForestIndex", "HaloRankInForest"), IDENTITY_CHUNK_ROWS
            ):
                inside = np.nonzero((forest >= 0) & (forest < n_forests_total))[0]
                if inside.size == 0:
                    continue
                group = forest[inside]
                ranks = rank[inside]
                held = (ranks >= 0) & (ranks < forests.counts[group])
                claimed = np.nonzero(held)[0]
                rejected = ~held
                rejected[claimed[bits.claim(forests.bases[group[claimed]] + ranks[claimed])]] = True
                bad_rows = np.nonzero(rejected)[0]
                if bad_rows.size:
                    n_bad += int(bad_rows.size)
                    # keep only the five lowest (ForestIndex, rank) pairs
                    # seen so far, so the example state is five triples,
                    # not a list that grows with the violation count
                    pick = bad_rows[np.lexsort((ranks[bad_rows], group[bad_rows]))[:5]]
                    examples = sorted(
                        examples
                        + [
                            (value, rank_value, None)
                            for value, rank_value in zip(group[pick].tolist(), ranks[pick].tolist())
                        ],
                        key=lambda item: (item[0], item[1]),
                    )[:5]
        if n_bad:
            detail = [
                "(ForestIndex={}, rank={}, expected {})".format(
                    value,
                    rank_value,
                    (
                        unheld
                        if unheld is not None
                        else bits.first_unheld(
                            int(forests.bases[value]), int(forests.counts[value])
                        )
                    ),
                )
                for value, rank_value, unheld in examples
            ]
            failures.append(
                "{} (ForestIndex, HaloRankInForest) pair(s) violate per-forest density/"
                "uniqueness; examples: {}".format(n_bad, ", ".join(detail))
            )
    finally:
        bits.close()

    if measured_max != max_rank_header:
        failures.append(
            "measured max HaloRankInForest {} != header max_halo_rank_in_forest {}".format(
                measured_max, max_rank_header
            )
        )
    return failures, bitset_bytes


def check_header_bounds(n_forests_total: int, max_rank: int, multiplier: int) -> List[str]:
    """Galaxy-identity bound checks from the spec: the multiplier must exceed
    the dataset's max rank, and multiplier x (n_forests_total + 1) must fit in
    int64 (the startup checks the reader performs against these headers)."""
    failures = []
    if multiplier <= max_rank:
        failures.append(
            "identity multiplier {} does not exceed max_halo_rank_in_forest {}".format(
                multiplier, max_rank
            )
        )
    if multiplier * (n_forests_total + 1) > _INT64_MAX:
        failures.append(
            "multiplier {} x (n_forests_total {} + 1) overflows int64".format(
                multiplier, n_forests_total
            )
        )
    return failures


def check_len(snapshots: _Snapshots) -> Tuple[List[str], int]:
    """Len >= 0 everywhere; Len == 0 is legal and its count is logged."""
    failures = []
    zero_total = 0
    for snap in range(snapshots.n_snapshots):
        length = snapshots.load(snap, ("Len",))["Len"]
        bad = length < 0
        if bad.any():
            failures.append(
                "{}: {} negative Len value(s); examples: {}".format(
                    snapshot_h5_name(snap), int(bad.sum()), _examples(length[bad].tolist())
                )
            )
        zero_total += int((length == 0).sum())
    return failures, zero_total


def check_sidecar_content(directory: Path, n_forests_total: int) -> List[str]:
    failures = []
    with h5py.File(directory / "forests.h5", "r") as handle:
        table = handle["ForestID"][...]
    if table.size != n_forests_total:
        failures.append(
            "forests.h5 /ForestID has {} entries, n_forests_total is {}".format(
                table.size, n_forests_total
            )
        )
    return failures


def check_count_conservation(snapshots: _Snapshots, manifest_path: Path) -> List[str]:
    """Total halo count across all emitted files must equal the sum of the
    INDEPENDENT per-source-file pre-counts recorded by the Slice 2 pre-scan
    (plan review finding 7: never validate against parser-derived totals
    alone).

    Global in scope but not in memory: a running accumulator over the emitted
    row counts reproduces the whole-dataset sum exactly, and the row counts
    come from the dataset shapes."""
    failures = []
    with open(manifest_path) as handle:
        manifest = json.load(handle)
    sources = manifest.get("source_files", {})
    if not sources:
        return ["manifest {} records no source files".format(manifest_path)]
    pre_total = sum(entry["pre_count"] for entry in sources.values())
    emitted_total = sum(snapshots.rows(snap) for snap in range(snapshots.n_snapshots))
    if emitted_total != pre_total:
        failures.append(
            "emitted halo total {} != independent source pre-count total {}".format(
                emitted_total, pre_total
            )
        )
    return failures


# ---------------------------------------------------------------------------
# Battery driver
# ---------------------------------------------------------------------------


def run_battery(
    directory,
    a_list_path,
    manifest_path=None,
    multiplier: int = DEFAULT_MULTIPLIER,
) -> List[Outcome]:
    """Run the full producer battery; returns one Outcome per named check."""
    directory = Path(directory)
    if not directory.is_dir():
        raise ConverterError("{}: not a directory".format(directory))
    a_list, a_list_md5 = load_a_list(a_list_path)
    n_snapshots = len(a_list)
    outcomes: List[Outcome] = []

    def record(name: str, failures: List[str], detail_pass: str = "") -> bool:
        if failures:
            outcomes.append(Outcome(name, "FAIL", "; ".join(failures)))
            return False
        outcomes.append(Outcome(name, "PASS", detail_pass))
        return True

    structural_ok = record("file-set", check_file_set(directory, n_snapshots))
    if structural_ok:
        failures = []
        for snap in range(n_snapshots):
            path = directory / snapshot_h5_name(snap)
            try:
                failures += ["{}: {}".format(path.name, f) for f in check_snapshot_structure(path)]
            except OSError as exc:
                failures.append("{}: unreadable as HDF5 ({})".format(path.name, exc))
        structural_ok = record("object-set", failures) and structural_ok
        try:
            sidecar_failures = check_sidecar_structure(directory / "forests.h5")
        except OSError as exc:
            sidecar_failures = ["forests.h5: unreadable as HDF5 ({})".format(exc)]
        structural_ok = record("sidecar-object-set", sidecar_failures) and structural_ok

    if manifest_path is not None:
        record(
            "manifest-binding", check_manifest_binding(directory, a_list_md5, Path(manifest_path))
        )
    else:
        outcomes.append(
            Outcome(
                "manifest-binding",
                "SKIP",
                "no manifest given (API mode; unreachable from the CLI)",
            )
        )

    semantic_names = (
        "header-values",
        "run-scoped-headers",
        "slab-order",
        "link-ranges",
        "fof-chains",
        "progenitor-closure",
        "identity",
        "header-bounds",
        "len-nonnegative",
        "sidecar-content",
        "count-conservation",
    )
    if not structural_ok:
        for name in semantic_names:
            outcomes.append(
                Outcome(name, "SKIP", "structural conformance failed; semantics not trusted")
            )
        return outcomes

    snapshots = _Snapshots(directory, n_snapshots)
    header_failures, run_scoped_failures = check_headers(snapshots, a_list)
    record("header-values", header_failures)
    run_scoped_ok = record("run-scoped-headers", run_scoped_failures)
    record("slab-order", check_slab_order(snapshots))
    ranges_ok = record("link-ranges", check_link_ranges(snapshots))
    if ranges_ok:
        record("fof-chains", check_fof_chains(snapshots))
        record("progenitor-closure", check_progenitor_closure(snapshots))
    else:
        outcomes.append(Outcome("fof-chains", "SKIP", "link ranges invalid; chains not walked"))
        outcomes.append(
            Outcome("progenitor-closure", "SKIP", "link ranges invalid; chains not walked")
        )
    if run_scoped_ok:
        # the run-scoped check just proved every file agrees on these two, so
        # one file's header carries them for the whole run
        header = snapshots.header(0)
        n_forests_total = int(header["n_forests_total"])
        max_rank = int(header["max_halo_rank_in_forest"])
        identity_failures, identity_bytes, ordering_bytes = check_identity(
            snapshots, n_forests_total, max_rank
        )
        record("identity", identity_failures)
        _log(
            "validate: identity checked exactly against a {} byte bitset (1 bit per halo) "
            "and {} byte(s) of transient on-disk ordering for out-of-range "
            "ForestIndex values".format(identity_bytes, ordering_bytes)
        )
        record("header-bounds", check_header_bounds(n_forests_total, max_rank, multiplier))
        record("sidecar-content", check_sidecar_content(directory, n_forests_total))
    else:
        for name in ("identity", "header-bounds", "sidecar-content"):
            outcomes.append(Outcome(name, "SKIP", "run-scoped headers inconsistent"))
    len_failures, zero_total = check_len(snapshots)
    record("len-nonnegative", len_failures, detail_pass="{} Len==0 halo(s)".format(zero_total))
    if manifest_path is not None:
        record("count-conservation", check_count_conservation(snapshots, Path(manifest_path)))
    else:
        outcomes.append(
            Outcome(
                "count-conservation",
                "SKIP",
                "no --manifest given; independent pre-counts unavailable",
            )
        )
    return outcomes


def battery_failed(outcomes: List[Outcome]) -> bool:
    return any(outcome.status == "FAIL" for outcome in outcomes)


# ===========================================================================
# Format version 3 (converter generalisation Slice 8)
# ===========================================================================
#
# docs/dev/HORIZONTAL-HDF5-FORMAT-V3-DRAFT.md, approved at Gate G1. Everything
# above this line is the version 2 battery and is unchanged: v2's adjacency,
# MostBoundID ordering and fixed-unit rules are replaced ONLY for files that
# declare format_version 3, and only here.
#
# **Independence.** The fixed format tables below are restated literally from
# the specification rather than imported from the writer or the schema module,
# so a writer-side table drift is caught instead of being mirrored. The
# per-dataset payload types come from each file's own ``/schema`` and are then
# bound to the conversion manifest's embedded schema.
#
# **Boundedness.** No check holds a whole snapshot. One scan reads every file
# in row blocks, checks every per-row rule there, and feeds four bounded
# external sorts (``rank_sort.KeyedSorter``) whose merges settle everything
# that crosses rows, snapshots or files:
#
# - a *topology join* keyed by each link's target global position: every halo
#   contributes its own record and one request per chain link, so each target
#   is judged against every link into it -- progenitor round-trip closure,
#   descendant-relative NextProgenitor membership, FoF central identity,
#   forest membership, and coverage (every halo with a descendant is reached
#   exactly once, every non-central exactly once by its FoF chain);
# - a *pointer-jumping* proof that no NextProgenitor or NextHaloInFOFgroup
#   chain cycles (``source_keys.verify_chains_acyclic``, a generic primitive
#   over ``(chain, node, successor)`` records);
# - an *identity* ordering of ``(ForestIndex, HaloRankInForest, SourceHaloID)``;
# - a *source-key* ordering of ``SourceHaloID``.
#
# ``budget_bytes`` bounds the scan block and every sorter buffer, all reported
# to one shared meter whose high-water mark is returned as a measurement, as
# is the spill high-water mark. What the meter does not see: O(snapshots)
# header arrays, per-block numpy temporaries of the scan (bounded by the block
# row count), and interpreter churn.

#: The format version this section validates.
V3_FORMAT_VERSION = 3

#: The fixed /halos table (topology, then identity), restated from the spec.
_V3_FIXED_DATASETS = (
    ("Descendant", "<i8"),
    ("FirstProgenitor", "<i8"),
    ("NextProgenitor", "<i8"),
    ("FirstHaloInFOFgroup", "<i8"),
    ("NextHaloInFOFgroup", "<i8"),
    ("DescendantSnapshot", "<i4"),
    ("FirstProgenitorSnapshot", "<i4"),
    ("NextProgenitorSnapshot", "<i4"),
    ("SourceHaloID", "<i8"),
    ("ForestIndex", "<i8"),
    ("HaloRankInForest", "<i8"),
)

#: The declarable ``/schema`` types and the storage each fixes: dtype, vec3.
_V3_TYPES = {
    "int": ("<i4", False),
    "long long": ("<i8", False),
    "float": ("<f4", False),
    "double": ("<f8", False),
    "vec3_int": ("<i4", True),
    "vec3_float": ("<f4", True),
}

#: Payload fields every v3 file declares, and the three whose type the format
#: table pins rather than the producer.
_V3_CORE_PAYLOAD = ("Len", "SnapNum", "M_Crit200", "Pos", "Vel", "Spin", "VelDisp", "Vmax")
_V3_PINNED_TYPES = {"SnapNum": "int", "Len": "int", "MostBoundID": "long long"}

_V3_STRING_ATTRS = {"source_format": 32, "column_mapping_sha256": 64}
_V3_SCHEMA_KEYS = ("type", "units", "h_convention", "description")
_V3_H_CONVENTIONS = ("carried", "free", "none")
_V3_SOURCE_FORMATS = ("consistent_trees_ascii", "consistent_trees_hdf5", "lhalo_binary")
_V3_SIDECAR = ("ForestID", "SourceFileOrdinal", "SourceUnitOrdinal")

#: Formats whose forest IS one inventory unit (an L-Halo tree, a forests-HDF5
#: ForestInfo row), so a forest's halos are one contiguous SourceHaloID range
#: in within-forest row order (C1).
_V3_UNIT_FORESTS = ("lhalo_binary", "consistent_trees_hdf5")

#: Each snapshot-qualified link and its target-snapshot column.
_V3_QUALIFIED = (
    ("Descendant", "DescendantSnapshot"),
    ("FirstProgenitor", "FirstProgenitorSnapshot"),
    ("NextProgenitor", "NextProgenitorSnapshot"),
)

#: Every named v3 check, in report order.
V3_CHECKS = (
    "file-set",
    "object-set",
    "sidecar-object-set",
    "schema-binding",
    "manifest-binding",
    "header-values",
    "run-scoped-headers",
    "row-values",
    "len-nonnegative",
    "field-finiteness",
    "link-targets",
    "links-adjacent",
    "topology-closure",
    "chain-cycles",
    "source-key-coverage",
    "identity",
    "header-bounds",
    "sidecar-content",
    "count-conservation",
)

#: Default working-buffer budget of one v3 battery.
DEFAULT_V3_BUDGET_BYTES = 256 << 20

#: Smallest budget accepted: every sorter share must hold its minimum buffers.
V3_MIN_BUDGET_BYTES = 1 << 20

#: Most rows one scan block reads, whatever the budget.
V3_MAX_SCAN_ROWS = 4 * CHUNK_1D[0]

#: Prefix of the battery's private spill directory.
V3_SPILL_PREFIX = "validate_v3_"

#: The topology join's record. ``tag`` 0 is a halo's own record: ``key`` and
#: ``other`` its global position, ``a`` its descendant's global position (or
#: -1), ``b`` its FoF central's. Tags 1-4 are links INTO ``key`` from the
#: halo at ``other``: 1 FirstProgenitor, 2 NextProgenitor, 3
#: FirstHaloInFOFgroup, 4 NextHaloInFOFgroup; ``a`` is the value the target's
#: own ``a`` (tags 1, 2) or ``b`` (tag 4) must equal. ``forest`` is always the
#: record owner's ForestIndex. ``(key, tag, other)`` is a total order.
_V3_JOIN_DTYPE = np.dtype(
    [
        ("key", "<i8"),
        ("tag", "<i8"),
        ("other", "<i8"),
        ("a", "<i8"),
        ("b", "<i8"),
        ("forest", "<i8"),
    ]
)
_V3_JOIN_KEY = ("key", "tag", "other")
_V3_IDENTITY_DTYPE = np.dtype([("forest", "<i8"), ("rank", "<i8"), ("sid", "<i8")])
_V3_IDENTITY_KEY = ("forest", "rank", "sid")
_V3_SID_DTYPE = np.dtype([("sid", "<i8")])
_V3_SID_KEY = ("sid",)

#: Scratch a merge consumer holds per record of the block it was handed: for
#: the join, the group index, gathered group fields, masks and counters; for
#: the two identity orderings, the shifted predecessor columns and masks.
_V3_JOIN_CONSUMER_BYTES = 12 * 8 + 8
_V3_ORDER_CONSUMER_BYTES = 6 * 8 + 4

#: Shares of the budget held concurrently while the scan feeds the sorters:
#: join, chain, identity and source-key generation buffers, then the scan
#: block itself. They sum to less than one.
_V3_SHARES = {"join": 6, "chain": 4, "identity": 2, "sid": 1, "scan": 3}
_V3_SHARE_DENOMINATOR = 16

_INT32_MAX = int(np.iinfo(np.int32).max)


class V3BatteryResult:
    """A v3 battery's named outcomes and what it measured on the way."""

    def __init__(self, outcomes: List[Outcome], measurements: Dict[str, object]):
        self.outcomes = outcomes
        self.measurements = measurements

    @property
    def failed(self) -> bool:
        return battery_failed(self.outcomes)


class _Failures:
    """Counted failures by kind, keeping five examples each."""

    def __init__(self):
        self._kinds: Dict[str, List] = {}

    def add(self, kind: str, count: int, examples) -> None:
        if count <= 0:
            return
        entry = self._kinds.setdefault(kind, [0, []])
        entry[0] += int(count)
        room = 5 - len(entry[1])
        if room > 0:
            entry[1].extend(list(examples)[:room])

    def __bool__(self) -> bool:
        return bool(self._kinds)

    def messages(self) -> List[str]:
        return [
            "{} {}; examples: {}".format(count, kind, _examples(examples))
            for kind, (count, examples) in self._kinds.items()
        ]


def _v3_rows(snap: int, rows: np.ndarray) -> List[str]:
    return ["{} row {}".format(snapshot_h5_name(snap), int(row)) for row in rows[:5]]


def _share(budget_bytes: int, name: str) -> int:
    return budget_bytes * _V3_SHARES[name] // _V3_SHARE_DENOMINATOR


# ---------------------------------------------------------------------------
# Version detection and dispatch
# ---------------------------------------------------------------------------


def dataset_format_versions(directory, n_snapshots: int) -> Dict[str, Optional[int]]:
    """``format_version`` declared by each expected snapshot file, or ``None``
    for a file that is absent, unreadable or carries no readable version."""
    directory = Path(directory)
    versions: Dict[str, Optional[int]] = {}
    for snap in range(n_snapshots):
        name = snapshot_h5_name(snap)
        try:
            with h5py.File(directory / name, "r") as handle:
                versions[name] = int(np.asarray(handle["header"].attrs["format_version"]))
        except (OSError, KeyError, TypeError, ValueError):
            versions[name] = None
    return versions


def detect_dataset_version(directory, n_snapshots: int) -> int:
    """The single format version a dataset declares: 3 only when EVERY
    snapshot file declares 3. A dataset that declares 3 in some files and
    anything else in others mixes versions, which no version permits, and is
    refused outright. Anything else is version 2's to judge: the unchanged v2
    battery fails version 1, unknown versions and unreadable files exactly as
    it always has."""
    versions = dataset_format_versions(directory, n_snapshots)
    declared = set(versions.values())
    if declared == {V3_FORMAT_VERSION}:
        return V3_FORMAT_VERSION
    if V3_FORMAT_VERSION in declared:
        others = sorted(
            "{}={}".format(name, version)
            for name, version in versions.items()
            if version != V3_FORMAT_VERSION
        )
        raise ConverterError(
            "{}: mixes format versions ({} of {} snapshot files declare {}; others: {}) -- a "
            "dataset never mixes versions".format(
                directory,
                sum(1 for v in versions.values() if v == V3_FORMAT_VERSION),
                n_snapshots,
                V3_FORMAT_VERSION,
                _examples(others),
            )
        )
    return FORMAT_VERSION


def run_producer_battery(
    directory,
    a_list_path,
    manifest_path=None,
    multiplier: int = DEFAULT_MULTIPLIER,
    *,
    budget_bytes: int = DEFAULT_V3_BUDGET_BYTES,
) -> List[Outcome]:
    """Dispatch to the battery of the version the dataset declares: the v2
    battery, called exactly as before, for everything that is not uniformly
    version 3 (``budget_bytes`` is the v3 battery's)."""
    a_list, _ = load_a_list(a_list_path)
    if detect_dataset_version(directory, len(a_list)) == V3_FORMAT_VERSION:
        return run_battery_v3(
            directory,
            a_list_path,
            manifest_path=manifest_path,
            multiplier=multiplier,
            budget_bytes=budget_bytes,
        ).outcomes
    return run_battery(directory, a_list_path, manifest_path=manifest_path, multiplier=multiplier)


# ---------------------------------------------------------------------------
# Structure
# ---------------------------------------------------------------------------


def _v3_check_file_set(directory: Path, n_snapshots: int) -> List[str]:
    """Exactly the a_list's snapshot files plus forests.h5, as regular files;
    no other entry of any kind."""
    expected = {snapshot_h5_name(snap) for snap in range(n_snapshots)} | {"forests.h5"}
    present = {}
    for entry in directory.iterdir():
        present[entry.name] = entry
    failures = []
    missing = sorted(expected - set(present))
    extra = sorted(set(present) - expected)
    if missing:
        failures.append("{} missing file(s): {}".format(len(missing), _examples(missing)))
    if extra:
        failures.append("{} unexpected entr(ies): {}".format(len(extra), _examples(extra)))
    odd = sorted(
        name
        for name in expected & set(present)
        if present[name].is_symlink() or not present[name].is_file()
    )
    if odd:
        failures.append("{} entr(ies) not regular files: {}".format(len(odd), _examples(odd)))
    return failures


def _link_kind_failures(group, where: str) -> List[str]:
    failures = []
    for name in group:
        link = group.get(name, getlink=True)
        if not isinstance(link, h5py.HardLink):
            failures.append(
                "{}/{} is a {}; every object must be physically present".format(
                    where, name, type(link).__name__
                )
            )
    return failures


def _v3_dataset_failures(dataset, name: str, dtype: str, is_vec: bool) -> List[str]:
    failures = []
    kind = dataset.id.get_type()
    if dataset.dtype.str != np.dtype(dtype).str or kind.get_order() != h5py.h5t.ORDER_LE:
        failures.append(
            "{} dtype {} is not explicit little-endian {}".format(name, dataset.dtype, dtype)
        )
    if is_vec:
        if dataset.ndim != 2 or dataset.shape[1] != 3:
            failures.append("{} shape {} is not [n_halos, 3]".format(name, dataset.shape))
        expected_chunks = CHUNK_VEC
    else:
        if dataset.ndim != 1:
            failures.append("{} shape {} is not [n_halos]".format(name, dataset.shape))
        expected_chunks = CHUNK_1D
    if dataset.chunks != expected_chunks:
        failures.append("{} chunks {} != {}".format(name, dataset.chunks, expected_chunks))
    failures += _filter_failures(dataset, name)
    if dataset.external or dataset.is_virtual:
        failures.append("{} is stored outside its file".format(name))
    if dataset.attrs.keys():
        failures.append("{} carries attributes {}".format(name, sorted(dataset.attrs.keys())))
    return failures


def _vlen_utf8_scalar(attrs, key: str) -> bool:
    attr = attrs.get_id(key)
    kind = attr.get_type()
    return (
        isinstance(kind, h5py.h5t.TypeStringID)
        and kind.is_variable_str()
        and kind.get_cset() == h5py.h5t.CSET_UTF8
        and attr.shape == ()
    )


def _v3_schema_structure(schema_group) -> Tuple[List[str], Dict[str, Tuple[str, ...]]]:
    """The file's own /schema declarations, and what is wrong with them as
    declarations (the dataset agreement is checked by the caller)."""
    failures = _link_kind_failures(schema_group, "/schema")
    if schema_group.attrs.keys():
        failures.append("/schema carries attributes {}".format(sorted(schema_group.attrs.keys())))
    declared: Dict[str, Tuple[str, ...]] = {}
    fixed = {name for name, _dtype in _V3_FIXED_DATASETS}
    for name in sorted(schema_group.keys()):
        group = schema_group.get(name)
        where = "/schema/{}".format(name)
        if not isinstance(group, h5py.Group):
            failures.append("{} is not a group".format(where))
            continue
        if len(group):
            failures.append("{} has children {}".format(where, sorted(group.keys())))
        keys = set(group.attrs.keys())
        if keys != set(_V3_SCHEMA_KEYS):
            failures.append(
                "{} attributes {} != exactly {}".format(where, sorted(keys), list(_V3_SCHEMA_KEYS))
            )
            continue
        bad = [key for key in _V3_SCHEMA_KEYS if not _vlen_utf8_scalar(group.attrs, key)]
        if bad:
            failures.append("{} attribute(s) {} are not scalar UTF-8 strings".format(where, bad))
            continue
        values = tuple(str(group.attrs[key]) for key in _V3_SCHEMA_KEYS)
        type_name, units, h_convention, description = values
        if name in fixed:
            failures.append("{} redeclares a fixed-table dataset".format(where))
        if type_name not in _V3_TYPES:
            failures.append("{} type {!r} is not declarable".format(where, type_name))
        if h_convention not in _V3_H_CONVENTIONS:
            failures.append(
                "{} h_convention {!r} is not one of {}".format(
                    where, h_convention, _V3_H_CONVENTIONS
                )
            )
        if not units or not description:
            failures.append("{} has an empty units or description".format(where))
        pinned = _V3_PINNED_TYPES.get(name)
        if pinned is not None and type_name != pinned:
            failures.append(
                "{} type {!r}; the format table pins {!r}".format(where, type_name, pinned)
            )
        declared[name] = values
    missing = sorted(set(_V3_CORE_PAYLOAD + tuple(_V3_PINNED_TYPES)) - set(declared))
    if missing:
        failures.append("/schema lacks core payload declaration(s) {}".format(missing))
    return failures, declared


def _v3_snapshot_structure(path: Path) -> Tuple[List[str], Optional[Dict[str, Tuple[str, ...]]]]:
    """Exact object set, header attribute set and types, /schema, and every
    /halos dataset against the fixed table or its own declaration."""
    failures: List[str] = []
    with h5py.File(path, "r") as handle:
        root = set(handle.keys())
        if root != {"header", "halos", "schema"}:
            return [
                "root object set {} != {{'halos', 'header', 'schema'}}".format(sorted(root))
            ], None
        failures += _link_kind_failures(handle, "")
        if handle.attrs.keys():
            failures.append("root carries attributes {}".format(sorted(handle.attrs.keys())))
        groups = [
            name
            for name in ("header", "halos", "schema")
            if not isinstance(handle.get(name), h5py.Group)
        ]
        if groups:
            return failures + ["/{} must be groups".format(", /".join(groups))], None

        header = handle["header"]
        if len(header):
            failures.append("/header has children {}".format(sorted(header.keys())))
        attrs = header.attrs
        expected_attrs = set(HEADER_ATTRS) | set(_V3_STRING_ATTRS)
        names = set(attrs.keys())
        if names != expected_attrs:
            failures.append(
                "header attribute set mismatch: missing {}, extra {}".format(
                    sorted(expected_attrs - names), sorted(names - expected_attrs)
                )
            )
        for name in sorted(names & set(HEADER_ATTRS)):
            actual = np.asarray(attrs[name])
            if actual.dtype != np.dtype(HEADER_ATTRS[name]) or actual.shape != ():
                failures.append(
                    "attribute {} has dtype {} shape {}, the format requires scalar {}".format(
                        name, actual.dtype, actual.shape, np.dtype(HEADER_ATTRS[name])
                    )
                )
        for name in sorted(names & set(_V3_STRING_ATTRS)):
            attr = attrs.get_id(name)
            kind = attr.get_type()
            if (
                not isinstance(kind, h5py.h5t.TypeStringID)
                or kind.is_variable_str()
                or kind.get_size() != _V3_STRING_ATTRS[name]
                or kind.get_cset() != h5py.h5t.CSET_ASCII
                or attr.shape != ()
            ):
                failures.append(
                    "attribute {} is not a scalar {}-byte fixed ASCII string".format(
                        name, _V3_STRING_ATTRS[name]
                    )
                )

        schema_failures, declared = _v3_schema_structure(handle["schema"])
        failures += schema_failures

        halos = handle["halos"]
        failures += _link_kind_failures(halos, "/halos")
        if halos.attrs.keys():
            failures.append("/halos carries attributes {}".format(sorted(halos.attrs.keys())))
        table = {name: (dtype, False) for name, dtype in _V3_FIXED_DATASETS}
        for name, values in declared.items():
            if values[0] in _V3_TYPES and name not in table:
                table[name] = _V3_TYPES[values[0]]
        present = set(halos.keys())
        if present != set(table):
            failures.append(
                "/halos dataset set mismatch against the fixed table plus /schema: missing {}, "
                "extra {}".format(sorted(set(table) - present), sorted(present - set(table)))
            )
        lengths = set()
        for name in sorted(present & set(table)):
            dataset = halos.get(name)
            if not isinstance(dataset, h5py.Dataset):
                failures.append("/halos/{} is not a dataset".format(name))
                continue
            dtype, is_vec = table[name]
            failures += _v3_dataset_failures(dataset, "/halos/{}".format(name), dtype, is_vec)
            if dataset.ndim:
                lengths.add(int(dataset.shape[0]))
        if len(lengths) > 1:
            failures.append("/halos datasets differ in length: {}".format(sorted(lengths)))
    return failures, declared


def _v3_sidecar_structure(path: Path) -> List[str]:
    """forests.h5: exactly three int64 little-endian 1-D root datasets of one
    length, chunked and unfiltered, with no attributes anywhere."""
    failures: List[str] = []
    with h5py.File(path, "r") as handle:
        names = set(handle.keys())
        if names != set(_V3_SIDECAR):
            return ["object set {} != {}".format(sorted(names), sorted(_V3_SIDECAR))]
        failures += _link_kind_failures(handle, "")
        if handle.attrs.keys():
            failures.append("root carries attributes {}".format(sorted(handle.attrs.keys())))
        lengths = set()
        for name in _V3_SIDECAR:
            dataset = handle.get(name)
            if not isinstance(dataset, h5py.Dataset):
                failures.append("/{} is not a dataset".format(name))
                continue
            failures += _v3_dataset_failures(dataset, "/{}".format(name), "<i8", False)
            lengths.add(int(dataset.shape[0]) if dataset.ndim else -1)
        if len(lengths) > 1:
            failures.append("sidecar datasets differ in length: {}".format(sorted(lengths)))
    return failures


# ---------------------------------------------------------------------------
# Manifest binding
# ---------------------------------------------------------------------------


def _v3_load_manifest(manifest_path: Path):
    """The generic conversion manifest the dataset claims, loaded with its
    own integrity checks (configuration digest, embedded schema identity)."""
    return ConversionManifest.load(Path(manifest_path).parent)


def _v3_manifest_binding(directory: Path, a_list_path, a_list: np.ndarray, manifest) -> List[str]:
    """The manifest must describe THIS dataset: a completed version-3 write
    whose registered outputs are exactly these files by name and SHA-256,
    produced from the supplied a_list."""
    failures = []
    write = manifest.stage("write")
    if write.get("status") != "complete":
        return ["the manifest's write stage is {!r}, not complete".format(write.get("status"))]
    writer = (write.get("result") or {}).get("writer") or {}
    if writer.get("format_version") != V3_FORMAT_VERSION:
        failures.append(
            "the manifest's write stage was completed by writer {} rather than a version {} "
            "writer".format(writer, V3_FORMAT_VERSION)
        )
    recorded = {}
    for relpath in write.get("artifacts", []):
        entry = manifest.artifact(relpath) or {}
        name = Path(relpath).name
        if name in recorded:
            failures.append("output basename {} is registered twice".format(name))
        recorded[name] = entry
    present = {entry.name for entry in directory.iterdir()}
    unrecorded = sorted(present - set(recorded))
    absent = sorted(set(recorded) - present)
    if unrecorded:
        failures.append(
            "{} file(s) not registered as write outputs: {}".format(
                len(unrecorded), _examples(unrecorded)
            )
        )
    if absent:
        failures.append(
            "{} registered write output(s) missing: {}".format(len(absent), _examples(absent))
        )
    mismatched = []
    for name in sorted(present & set(recorded)):
        entry = recorded[name]
        if entry.get("status") != "present" or sha256_file(directory / name) != entry.get("sha256"):
            mismatched.append(name)
    if mismatched:
        failures.append(
            "{} file(s) whose content differs from the registered write output: {}".format(
                len(mismatched), _examples(mismatched)
            )
        )
    pinned = [record for record in manifest.dependencies if "a_list" in record.get("roles", ())]
    supplied = sha256_file(a_list_path)
    if not pinned or any(record.get("sha256") != supplied for record in pinned):
        failures.append(
            "supplied a_list content sha256 {} is not the a_list this conversion pinned".format(
                supplied
            )
        )
    recorded_scales = [
        float(value) for value in manifest.configuration["snapshots"]["scale_factors"]
    ]
    if recorded_scales != [float(value) for value in a_list]:
        failures.append("supplied a_list scale factors differ from the conversion's")
    return failures


# ---------------------------------------------------------------------------
# Headers
# ---------------------------------------------------------------------------


def _v3_read_header(path: Path) -> Dict[str, object]:
    with h5py.File(path, "r") as handle:
        attrs = handle["header"].attrs
        values: Dict[str, object] = {
            name: np.asarray(attrs[name])[()].item() for name in HEADER_ATTRS
        }
        for name in _V3_STRING_ATTRS:
            raw = attrs[name]
            values[name] = raw.decode("ascii") if isinstance(raw, bytes) else str(raw)
        values["_rows"] = int(handle["halos"]["SourceHaloID"].shape[0])
        values["_field_rows"] = {
            name: int(handle["halos"][name].shape[0]) for name in handle["halos"]
        }
        values["_bytes"] = int(path.stat().st_size)
    return values


_V3_RUN_SCOPED = RUN_SCOPED_ATTRS + ("links_adjacent", "source_format", "column_mapping_sha256")


def _v3_check_headers(
    headers: List[Dict[str, object]], a_list: np.ndarray, manifest
) -> Tuple[List[str], List[str]]:
    failures = []
    run_scoped = []
    observed = {name: set() for name in _V3_RUN_SCOPED}
    for snap, header in enumerate(headers):
        name = snapshot_h5_name(snap)
        for attr in _V3_RUN_SCOPED:
            observed[attr].add(repr(header[attr]))
        if header["format_version"] != V3_FORMAT_VERSION:
            failures.append(
                "{}: format_version {} != {}".format(
                    name, header["format_version"], V3_FORMAT_VERSION
                )
            )
        if header["links_adjacent"] not in (0, 1):
            failures.append(
                "{}: links_adjacent {} is not 0 or 1".format(name, header["links_adjacent"])
            )
        if header["snapshot_number"] != snap:
            failures.append(
                "{}: snapshot_number {} != filename index {}".format(
                    name, header["snapshot_number"], snap
                )
            )
        if float(header["scale_factor"]) != float(a_list[snap]):
            failures.append(
                "{}: scale_factor {!r} != a_list[{}] = {!r}".format(
                    name, header["scale_factor"], snap, float(a_list[snap])
                )
            )
        for field, rows in header["_field_rows"].items():
            if rows != header["n_halos"]:
                failures.append(
                    "{}: dataset {} has {} rows, header n_halos is {}".format(
                        name, field, rows, header["n_halos"]
                    )
                )
        if header["source_format"] not in _V3_SOURCE_FORMATS:
            failures.append(
                "{}: source_format {!r} is not a known adapter".format(
                    name, header["source_format"]
                )
            )
        digest = header["column_mapping_sha256"]
        if len(digest) != 64 or any(c not in "0123456789abcdef" for c in digest):
            failures.append(
                "{}: column_mapping_sha256 {!r} is not lowercase hex".format(name, digest)
            )
        if manifest is not None:
            if header["source_format"] != manifest.schema.source_format:
                failures.append(
                    "{}: source_format {!r} != the conversion's {!r}".format(
                        name, header["source_format"], manifest.schema.source_format
                    )
                )
            if digest != manifest.schema.digest:
                failures.append(
                    "{}: column_mapping_sha256 {} != the conversion's schema {}".format(
                        name, digest, manifest.schema.digest
                    )
                )
    for attr in _V3_RUN_SCOPED:
        if len(observed[attr]) > 1:
            run_scoped.append(
                "{} differs across files: {}".format(attr, _examples(sorted(observed[attr])))
            )
    if manifest is not None and headers:
        writer = ((manifest.stage("write").get("result") or {}).get("writer") or {}).get("header")
        if writer is not None:
            for attr, value in writer.items():
                if any(header.get(attr) != value for header in headers):
                    run_scoped.append(
                        "{} differs from the value the conversion's writer recorded ({!r})".format(
                            attr, value
                        )
                    )
    return failures, run_scoped


def _v3_schema_binding(declarations: List[Dict[str, Tuple[str, ...]]], manifest) -> List[str]:
    """Every file declares the same /schema, and it is the conversion's."""
    failures = []
    reference = declarations[0] if declarations else {}
    differing = [
        snapshot_h5_name(snap)
        for snap, declared in enumerate(declarations)
        if declared != reference
    ]
    if differing:
        failures.append(
            "/schema differs from {}'s in {} file(s): {}".format(
                snapshot_h5_name(0), len(differing), _examples(differing)
            )
        )
    if manifest is not None:
        expected = {
            field.name: tuple(getattr(field, key) for key in _V3_SCHEMA_KEYS)
            for field in manifest.schema.output_field_declarations()
        }
        for snap, declared in enumerate(declarations):
            if declared != expected:
                failures.append(
                    "{}: /schema differs from the conversion's embedded schema (missing {}, "
                    "extra {}, changed {})".format(
                        snapshot_h5_name(snap),
                        sorted(set(expected) - set(declared)),
                        sorted(set(declared) - set(expected)),
                        sorted(
                            name
                            for name in set(expected) & set(declared)
                            if expected[name] != declared[name]
                        ),
                    )
                )
    return failures


# ---------------------------------------------------------------------------
# The bounded scan
# ---------------------------------------------------------------------------


class _V3Scan:
    """One pass over every snapshot file in row blocks: every per-row rule,
    the measurements, and the four sorters' input."""

    def __init__(
        self,
        directory: Path,
        counts: List[int],
        declarations: Dict[str, Tuple[str, ...]],
        n_forests_total: Optional[int],
        budget_bytes: int,
        spill_root: Path,
    ):
        self.directory = directory
        self.counts = np.asarray(counts, dtype=np.int64)
        self.n_snapshots = len(counts)
        self.offsets = np.zeros(self.n_snapshots + 1, dtype=np.int64)
        np.cumsum(self.counts, out=self.offsets[1:])
        self.n_forests_total = n_forests_total
        self.floats = [
            name
            for name, values in sorted(declarations.items())
            if _V3_TYPES.get(values[0], ("", False))[0].startswith("<f")
        ]
        self.budget_bytes = budget_bytes
        self.meter = ResidencyMeter()
        self.ledger = SpillLedger()
        self.spill_root = spill_root
        self.row_values = _Failures()
        self.lengths = _Failures()
        self.finiteness = _Failures()
        self.links = _Failures()
        self.identity_ranges = _Failures()
        self.len_zero = 0
        self.links_ok = True
        self.stats = {
            "non_null_links": {name: 0 for name, _dtype in _V3_FIXED_DATASETS[:5]},
            "gapped_descendants": 0,
            "max_descendant_span": 0,
            "gapped_first_progenitors": 0,
            "max_first_progenitor_span": 0,
            "next_progenitors_off_owner_snapshot": 0,
            "max_link_row": -1,
            "min_source_halo_id": None,
            "max_source_halo_id": None,
            "max_forest_index": -1,
            "max_halo_rank_in_forest": -1,
        }

        def sorter(dtype, key, share, tag):
            return KeyedSorter(
                dtype,
                key,
                budget_bytes=_share(budget_bytes, share),
                spill_dir=spill_root,
                residency=self.meter,
                ledger=self.ledger,
                tag=tag,
            )

        self._sorter = sorter
        self.join = sorter(_V3_JOIN_DTYPE, _V3_JOIN_KEY, "join", "v3join")
        self.chain = sorter(CHAIN_DTYPE, CHAIN_KEY, "chain", "v3chain")
        self.identity = sorter(_V3_IDENTITY_DTYPE, _V3_IDENTITY_KEY, "identity", "v3identity")
        self.sids = sorter(_V3_SID_DTYPE, _V3_SID_KEY, "sid", "v3sid")
        self.n_chain_edges = 0
        row_bytes = 13 * 8 + 12 * len(self.floats)
        per_row = (
            4 * row_bytes + 5 * _V3_JOIN_DTYPE.itemsize + 4 * CHAIN_DTYPE.itemsize + 32 + 24 * 8
        )
        self.scan_rows = int(max(1, min(V3_MAX_SCAN_ROWS, _share(budget_bytes, "scan") // per_row)))
        self.scan_bytes = self.scan_rows * per_row

    def close(self) -> None:
        for sorter in (self.join, self.chain, self.identity, self.sids):
            sorter.close()

    # ---- the pass --------------------------------------------------------

    def run(self) -> None:
        self.meter.acquire(self.scan_bytes)
        try:
            for snap in range(self.n_snapshots):
                self._scan_snapshot(snap)
        finally:
            self.meter.release(self.scan_bytes)
        for sorter in (self.join, self.chain, self.identity, self.sids):
            sorter.seal()

    def _scan_snapshot(self, snap: int) -> None:
        names = [name for name, _dtype in _V3_FIXED_DATASETS] + ["SnapNum", "Len"] + self.floats
        previous_sid = None
        with h5py.File(self.directory / snapshot_h5_name(snap), "r") as handle:
            halos = handle["halos"]
            n = int(self.counts[snap])
            for start in range(0, n, self.scan_rows):
                stop = min(start + self.scan_rows, n)
                block = {name: halos[name][start:stop] for name in names}
                previous_sid = self._check_block(snap, start, block, previous_sid)

    def _check_block(self, snap: int, start: int, block: Dict[str, np.ndarray], previous_sid):
        size = int(block["SourceHaloID"].size)
        rows = np.arange(start, start + size, dtype=np.int64)
        n = int(self.counts[snap])
        stats = self.stats

        # -- row values: SnapNum, SourceHaloID order, Len, finiteness ------
        bad = block["SnapNum"] != snap
        self.row_values.add(
            "SnapNum value(s) != the file's snapshot", int(bad.sum()), _v3_rows(snap, rows[bad])
        )
        sid = block["SourceHaloID"].astype(np.int64)
        bad = sid <= 0
        self.row_values.add(
            "non-positive SourceHaloID value(s)", int(bad.sum()), _v3_rows(snap, rows[bad])
        )
        before = np.empty(size, dtype=np.int64)
        before[0] = sid[0] - 1 if previous_sid is None else previous_sid
        before[1:] = sid[:-1]
        bad = sid <= before
        self.row_values.add(
            "row(s) not strictly ascending in SourceHaloID",
            int(bad.sum()),
            _v3_rows(snap, rows[bad]),
        )
        stats["min_source_halo_id"] = _min_none(stats["min_source_halo_id"], int(sid.min()))
        stats["max_source_halo_id"] = _max_none(stats["max_source_halo_id"], int(sid.max()))
        length = block["Len"]
        bad = length < 0
        self.lengths.add("negative Len value(s)", int(bad.sum()), _v3_rows(snap, rows[bad]))
        self.len_zero += int(np.count_nonzero(length == 0))
        for name in self.floats:
            values = block[name]
            bad = ~np.isfinite(values)
            if bad.ndim == 2:
                bad = bad.any(axis=1)
            self.finiteness.add(
                "non-finite {} value(s)".format(name), int(bad.sum()), _v3_rows(snap, rows[bad])
            )

        # -- identity ranges -----------------------------------------------
        forest = block["ForestIndex"].astype(np.int64)
        rank = block["HaloRankInForest"].astype(np.int64)
        stats["max_forest_index"] = max(stats["max_forest_index"], int(forest.max()))
        stats["max_halo_rank_in_forest"] = max(stats["max_halo_rank_in_forest"], int(rank.max()))
        records = np.empty(size, dtype=_V3_IDENTITY_DTYPE)
        records["forest"] = forest
        records["rank"] = rank
        records["sid"] = sid
        self.identity.add(records)
        records = np.empty(size, dtype=_V3_SID_DTYPE)
        records["sid"] = sid
        self.sids.add(records)

        # -- snapshot-qualified links --------------------------------------
        before_links = bool(self.links)
        targets = {}
        for link, column in _V3_QUALIFIED:
            index = block[link].astype(np.int64)
            target = block[column].astype(np.int64)
            null = index == -1
            mismatch = null != (target == -1)
            self.links.add(
                "{} / {} null-ness disagreements".format(link, column),
                int(mismatch.sum()),
                _v3_rows(snap, rows[mismatch]),
            )
            linked = ~null & ~mismatch
            bad = index < -1
            self.links.add(
                "{} value(s) below -1".format(link), int(bad.sum()), _v3_rows(snap, rows[bad])
            )
            bad = linked & ((target < 0) | (target >= self.n_snapshots))
            self.links.add(
                "{} value(s) naming a snapshot outside the dataset".format(column),
                int(bad.sum()),
                _v3_rows(snap, rows[bad]),
            )
            linked &= ~bad & (index >= 0)
            safe_target = np.where(linked, target, 0)
            bad = linked & (index >= self.counts[safe_target])
            self.links.add(
                "{} row(s) outside their target snapshot's n_halos".format(link),
                int(bad.sum()),
                _v3_rows(snap, rows[bad]),
            )
            linked &= ~bad
            stats["non_null_links"][link] += int(np.count_nonzero(~null))
            if linked.any():
                stats["max_link_row"] = max(stats["max_link_row"], int(index[linked].max()))
            targets[link] = (index, target, linked, null)

        index, target, linked, null = targets["Descendant"]
        bad = linked & (target <= snap)
        self.links.add(
            "Descendant(s) not strictly later", int(bad.sum()), _v3_rows(snap, rows[bad])
        )
        span = target[linked] - snap
        stats["gapped_descendants"] += int(np.count_nonzero(span != 1))
        if span.size:
            stats["max_descendant_span"] = max(stats["max_descendant_span"], int(span.max()))
        _, fp_target, fp_linked, _ = targets["FirstProgenitor"]
        bad = fp_linked & (fp_target >= snap)
        self.links.add(
            "FirstProgenitor(s) not strictly earlier", int(bad.sum()), _v3_rows(snap, rows[bad])
        )
        fp_span = snap - fp_target[fp_linked]
        stats["gapped_first_progenitors"] += int(np.count_nonzero(fp_span != 1))
        if fp_span.size:
            stats["max_first_progenitor_span"] = max(
                stats["max_first_progenitor_span"], int(fp_span.max())
            )
        _, np_target, np_linked, np_null = targets["NextProgenitor"]
        bad = ~np_null & null
        self.links.add(
            "NextProgenitor(s) on a halo with no Descendant",
            int(bad.sum()),
            _v3_rows(snap, rows[bad]),
        )
        bad = np_linked & linked & (np_target >= target)
        self.links.add(
            "NextProgenitor(s) not earlier than the shared DescendantSnapshot",
            int(bad.sum()),
            _v3_rows(snap, rows[bad]),
        )
        stats["next_progenitors_off_owner_snapshot"] += int(
            np.count_nonzero(np_linked & (np_target != snap))
        )

        first_fof = block["FirstHaloInFOFgroup"].astype(np.int64)
        next_fof = block["NextHaloInFOFgroup"].astype(np.int64)
        bad = (first_fof < 0) | (first_fof >= n)
        self.links.add(
            "FirstHaloInFOFgroup value(s) outside [0, n_halos)",
            int(bad.sum()),
            _v3_rows(snap, rows[bad]),
        )
        bad = (next_fof < -1) | (next_fof >= n)
        self.links.add(
            "NextHaloInFOFgroup value(s) outside [-1, n_halos)",
            int(bad.sum()),
            _v3_rows(snap, rows[bad]),
        )
        for values in (first_fof, next_fof):
            if values.size:
                stats["max_link_row"] = max(stats["max_link_row"], int(values.max()))
        if self.n_forests_total is not None:
            bad = (forest < 0) | (forest >= self.n_forests_total)
            self.identity_ranges.add(
                "ForestIndex value(s) outside [0, n_forests_total)",
                int(bad.sum()),
                _v3_rows(snap, rows[bad]),
            )
        bad = rank < 0
        self.identity_ranges.add(
            "negative HaloRankInForest value(s)", int(bad.sum()), _v3_rows(snap, rows[bad])
        )

        if bool(self.links) and not before_links:
            self.links_ok = False
        if self.links_ok:
            self._emit_topology(snap, rows, targets, first_fof, next_fof, forest)
        return int(sid[-1])

    def _emit_topology(self, snap, rows, targets, first_fof, next_fof, forest) -> None:
        """Join requests and pointer-jumping edges for one block, in global
        positions. Only reached while every link seen so far is in range."""
        offsets = self.offsets
        gp = offsets[snap] + rows
        d_index, d_target, d_linked, _ = targets["Descendant"]
        desc_gp = np.where(d_linked, offsets[np.where(d_linked, d_target, 0)] + d_index, -1)
        central_gp = offsets[snap] + first_fof
        parts = []

        def part(mask, key, tag, a):
            count = int(np.count_nonzero(mask)) if mask is not None else int(gp.size)
            records = np.empty(count, dtype=_V3_JOIN_DTYPE)
            pick = slice(None) if mask is None else mask
            records["key"] = key if mask is None else key[mask]
            records["tag"] = tag
            records["other"] = gp[pick]
            records["a"] = a[pick]
            records["b"] = central_gp[pick]
            records["forest"] = forest[pick]
            parts.append(records)

        part(None, gp, 0, desc_gp)
        fp_index, fp_target, fp_linked, _ = targets["FirstProgenitor"]
        fp_gp = offsets[np.where(fp_linked, fp_target, 0)] + fp_index
        part(fp_linked, fp_gp, 1, gp)
        np_index, np_target, np_linked, _ = targets["NextProgenitor"]
        np_gp = offsets[np.where(np_linked, np_target, 0)] + np_index
        part(np_linked, np_gp, 2, desc_gp)
        part(np.ones(gp.size, dtype=bool), central_gp, 3, central_gp)
        nf_linked = next_fof != -1
        nf_gp = offsets[snap] + next_fof
        part(nf_linked, nf_gp, 4, central_gp)
        for records in parts:
            self.join.add(records)

        for chain, mask, successor in ((0, np_linked, np_gp), (1, nf_linked, nf_gp)):
            count = int(np.count_nonzero(mask))
            if not count:
                continue
            edges = np.empty(2 * count, dtype=CHAIN_DTYPE)
            nodes, asks = edges[:count], edges[count:]
            nodes["chain"] = chain
            nodes["key"] = gp[mask]
            nodes["tag"] = 0
            nodes["other"] = successor[mask]
            asks["chain"] = chain
            asks["key"] = successor[mask]
            asks["tag"] = 1
            asks["other"] = gp[mask]
            self.chain.add(edges)
            self.n_chain_edges += count


def _min_none(current, value):
    return value if current is None else min(current, value)


def _max_none(current, value):
    return value if current is None else max(current, value)


# ---------------------------------------------------------------------------
# Merge consumers
# ---------------------------------------------------------------------------


def _describe_gp(offsets: np.ndarray, gp: int) -> str:
    position = int(np.searchsorted(offsets, gp, side="right")) - 1
    return "{} row {}".format(snapshot_h5_name(position), int(gp - offsets[position]))


def _link_examples(offsets: np.ndarray, owners, targets, mask) -> List[str]:
    """Up to five ``owner -> target`` links, located by file and row."""
    return [
        "{} -> {}".format(_describe_gp(offsets, owner), _describe_gp(offsets, target))
        for owner, target in zip(owners[mask][:5], targets[mask][:5])
    ]


def _v3_topology_closure(scan: _V3Scan) -> List[str]:
    """Consume the sorted topology join: every link into a halo is judged
    against that halo's own record, and every halo's in-links are counted.

    A group is a halo's own record followed by the links into it. The last
    group of a block may continue into the next, so its record and running
    counts are carried; a group is judged once the next own-record appears.
    """
    failures = _Failures()
    offsets = scan.offsets
    carry = None  # (key, a, b, forest, progenitor_in, fof_in)

    def judge(key, a, b, prog_in, fof_in):
        expected_prog = (a != -1).astype(np.int64)
        expected_fof = (b != key).astype(np.int64)
        bad = prog_in != expected_prog
        failures.add(
            "halo(s) not reached exactly once by their descendant's progenitor chain "
            "(reached k times, expected once iff the halo has a Descendant)",
            int(bad.sum()),
            [
                "{} (k={})".format(_describe_gp(offsets, g), int(k))
                for g, k in zip(key[bad][:5], prog_in[bad][:5])
            ],
        )
        bad = fof_in != expected_fof
        failures.add(
            "halo(s) not reached exactly once by their FoF chain (reached k times, expected "
            "once iff the halo is not its group's central)",
            int(bad.sum()),
            [
                "{} (k={})".format(_describe_gp(offsets, g), int(k))
                for g, k in zip(key[bad][:5], fof_in[bad][:5])
            ],
        )

    blocks = scan.join.sorted_blocks(
        budget_bytes=scan.budget_bytes, consumer_bytes_per_record=_V3_JOIN_CONSUMER_BYTES
    )
    try:
        for block in blocks:
            size = int(block.size)
            tag = block["tag"]
            key = block["key"]
            is_self = tag == 0
            last = np.where(is_self, np.arange(size), -1)
            np.maximum.accumulate(last, out=last)
            requests = np.flatnonzero(~is_self)
            source = last[requests]
            inside = source >= 0
            safe = np.maximum(source, 0)
            if carry is None:
                carry_values = (-2, -1, -1, -1)
            else:
                carry_values = carry[:4]
            g_key = np.where(inside, key[safe], carry_values[0])
            g_a = np.where(inside, block["a"][safe], carry_values[1])
            g_b = np.where(inside, block["b"][safe], carry_values[2])
            g_forest = np.where(inside, block["forest"][safe], carry_values[3])
            r_key = key[requests]
            r_tag = tag[requests]
            r_a = block["a"][requests]
            r_other = block["other"][requests]

            def where(mask, owners=r_other, targets=r_key):
                return _link_examples(offsets, owners, targets, mask)

            orphan = g_key != r_key
            failures.add("link(s) naming a halo that has no row", int(orphan.sum()), where(orphan))
            matched = ~orphan
            prog = matched & ((r_tag == 1) | (r_tag == 2))
            bad = prog & (r_tag == 1) & (g_a != r_a)
            failures.add(
                "FirstProgenitor(s) whose target's Descendant is not the owner",
                int(bad.sum()),
                where(bad),
            )
            bad = prog & (r_tag == 2) & (g_a != r_a)
            failures.add(
                "NextProgenitor(s) whose target names a different descendant than the owner",
                int(bad.sum()),
                where(bad),
            )
            bad = matched & (r_tag == 3) & (g_b != g_key)
            failures.add(
                "FirstHaloInFOFgroup target(s) that are not self-referencing centrals",
                int(bad.sum()),
                where(bad),
            )
            fof = matched & (r_tag == 4)
            bad = fof & (g_b != r_a)
            failures.add(
                "NextHaloInFOFgroup target(s) in a different FoF group than the owner",
                int(bad.sum()),
                where(bad),
            )
            bad = matched & (g_forest != block["forest"][requests])
            failures.add("link(s) crossing forests", int(bad.sum()), where(bad))

            # in-link counts per own-record of this block, plus the carry's
            prog_in = np.bincount(source[prog & inside], minlength=size)
            fof_in = np.bincount(source[fof & inside], minlength=size)
            selves = np.flatnonzero(is_self)
            if carry is not None:
                carry = carry[:4] + (
                    carry[4] + int(np.count_nonzero(prog & ~inside)),
                    carry[5] + int(np.count_nonzero(fof & ~inside)),
                )
            if selves.size:
                if carry is not None:
                    judge(
                        np.asarray([carry[0]]),
                        np.asarray([carry[1]]),
                        np.asarray([carry[2]]),
                        np.asarray([carry[4]]),
                        np.asarray([carry[5]]),
                    )
                done = selves[:-1]
                judge(key[done], block["a"][done], block["b"][done], prog_in[done], fof_in[done])
                tail = int(selves[-1])
                carry = (
                    int(key[tail]),
                    int(block["a"][tail]),
                    int(block["b"][tail]),
                    int(block["forest"][tail]),
                    int(prog_in[tail]),
                    int(fof_in[tail]),
                )
    finally:
        blocks.close()
    if carry is not None:
        judge(
            np.asarray([carry[0]]),
            np.asarray([carry[1]]),
            np.asarray([carry[2]]),
            np.asarray([carry[4]]),
            np.asarray([carry[5]]),
        )
    return failures.messages()


def _v3_chain_cycles(scan: _V3Scan) -> Tuple[List[str], int]:
    """Prove every NextProgenitor and NextHaloInFOFgroup chain terminates."""
    half = scan.budget_bytes // 2
    layout = SnapshotLayout(list(range(scan.n_snapshots)), scan.counts.tolist())

    def new_round():
        return KeyedSorter(
            CHAIN_DTYPE,
            CHAIN_KEY,
            budget_bytes=half,
            spill_dir=scan.spill_root,
            residency=scan.meter,
            ledger=scan.ledger,
            tag="v3chain",
        )

    try:
        rounds = verify_chains_acyclic(
            scan.chain, scan.n_chain_edges, layout, merge_budget_bytes=half, new_round=new_round
        )
    except ConverterError as exc:
        return [str(exc)], -1
    return [], rounds


def _v3_identity(
    scan: _V3Scan, source_format: str, n_forests_total: int, max_rank: int, unsampled: bool
) -> List[str]:
    """Consume the (ForestIndex, HaloRankInForest, SourceHaloID) ordering.

    Every forest's ranks are exactly 0 .. count-1 (dense and unique). For
    Consistent-Trees ASCII every forest holds halos, so ForestIndex is dense
    over [0, n_forests_total). For the unit-forest formats a forest may be a
    zero-halo tree, but its halos are one contiguous SourceHaloID range in rank
    order, so in (ForestIndex, rank) order the ids of an unsampled conversion
    are exactly 1, 2, 3, ...
    """
    failures = _Failures()
    carry_forest, carry_rank, position = -1, -1, 0
    measured_max = -1
    unit_forests = source_format in _V3_UNIT_FORESTS
    blocks = scan.identity.sorted_blocks(
        budget_bytes=scan.budget_bytes, consumer_bytes_per_record=_V3_ORDER_CONSUMER_BYTES
    )
    try:
        for block in blocks:
            size = int(block.size)
            forest, rank, sid = block["forest"], block["rank"], block["sid"]
            prev_forest = np.empty(size, dtype=np.int64)
            prev_rank = np.empty(size, dtype=np.int64)
            prev_forest[0], prev_rank[0] = carry_forest, carry_rank
            prev_forest[1:], prev_rank[1:] = forest[:-1], rank[:-1]
            same = forest == prev_forest
            bad = np.where(same, rank != prev_rank + 1, rank != 0)
            failures.add(
                "(ForestIndex, HaloRankInForest) pair(s) breaking per-forest rank density or "
                "uniqueness",
                int(bad.sum()),
                [
                    "(ForestIndex={}, rank={})".format(f, r)
                    for f, r in zip(forest[bad][:5], rank[bad][:5])
                ],
            )
            if source_format == "consistent_trees_ascii":
                bad = ~same & (forest != prev_forest + 1)
                failures.add(
                    "ForestIndex value(s) skipping a forest (ASCII forests all hold halos)",
                    int(bad.sum()),
                    [
                        "ForestIndex={} after {}".format(f, p)
                        for f, p in zip(forest[bad][:5], prev_forest[bad][:5])
                    ],
                )
            if unit_forests and unsampled:
                expected = position + 1 + np.arange(size, dtype=np.int64)
                bad = sid != expected
                failures.add(
                    "halo(s) whose SourceHaloID is not its (ForestIndex, rank) position in the "
                    "source order",
                    int(bad.sum()),
                    [
                        "(ForestIndex={}, rank={}) has SourceHaloID {}, expected {}".format(
                            f, r, s, e
                        )
                        for f, r, s, e in zip(
                            forest[bad][:5], rank[bad][:5], sid[bad][:5], expected[bad][:5]
                        )
                    ],
                )
            measured_max = max(measured_max, int(rank.max()))
            carry_forest, carry_rank = int(forest[-1]), int(rank[-1])
            position += size
    finally:
        blocks.close()
    messages = failures.messages()
    if source_format == "consistent_trees_ascii" and carry_forest != n_forests_total - 1:
        messages.append(
            "ForestIndex values end at {} but n_forests_total is {}".format(
                carry_forest, n_forests_total
            )
        )
    if measured_max != max_rank:
        messages.append(
            "measured max HaloRankInForest {} != header max_halo_rank_in_forest {}".format(
                measured_max, max_rank
            )
        )
    return messages


def _v3_source_keys(scan: _V3Scan, inventory: Optional[Mapping]) -> List[str]:
    """Consume the SourceHaloID ordering: globally unique, and -- against the
    conversion's inventory -- within [1, total_halos] and exactly as many as
    the inventory selects, which for an unsampled inventory is exactly
    {1 .. total_halos}."""
    failures = _Failures()
    previous = 0
    position = 0
    total = None if inventory is None else int(inventory["total_halos"])
    unsampled = inventory is not None and int(inventory["selected_halos"]) == total
    blocks = scan.sids.sorted_blocks(
        budget_bytes=scan.budget_bytes, consumer_bytes_per_record=_V3_ORDER_CONSUMER_BYTES
    )
    try:
        for block in blocks:
            sid = block["sid"]
            before = np.empty(sid.size, dtype=np.int64)
            before[0] = previous
            before[1:] = sid[:-1]
            bad = sid == before
            failures.add("duplicated SourceHaloID value(s)", int(bad.sum()), sid[bad].tolist())
            if total is not None:
                bad = (sid < 1) | (sid > total)
                failures.add(
                    "SourceHaloID value(s) outside the inventory's [1, {}]".format(total),
                    int(bad.sum()),
                    sid[bad].tolist(),
                )
            if unsampled:
                expected = position + 1 + np.arange(sid.size, dtype=np.int64)
                bad = sid != expected
                failures.add(
                    "gap(s) in SourceHaloID coverage of the unsampled inventory",
                    int(bad.sum()),
                    [
                        "{} where {} was expected".format(s, e)
                        for s, e in zip(sid[bad][:5], expected[bad][:5])
                    ],
                )
            previous = int(sid[-1])
            position += int(sid.size)
    finally:
        blocks.close()
    messages = failures.messages()
    if inventory is not None and position != int(inventory["selected_halos"]):
        messages.append(
            "{} SourceHaloID value(s) emitted; the inventory selects {}".format(
                position, inventory["selected_halos"]
            )
        )
    return messages


# ---------------------------------------------------------------------------
# Sidecar content and conservation
# ---------------------------------------------------------------------------


def _v3_sidecar_content(
    path: Path, source_format: str, n_forests_total: int, inventory, block_rows: int
) -> List[str]:
    failures = _Failures()
    per_file: Dict[int, int] = {}
    with h5py.File(path, "r") as handle:
        n = int(handle["ForestID"].shape[0])
        if n != n_forests_total:
            return ["forests.h5 holds {} forests, n_forests_total is {}".format(n, n_forests_total)]
        prev = (-1, -1, None)  # (file, unit, forest_id)
        for start in range(0, n, block_rows):
            stop = min(start + block_rows, n)
            fid = handle["ForestID"][start:stop]
            files = handle["SourceFileOrdinal"][start:stop]
            units = handle["SourceUnitOrdinal"][start:stop]
            rows = np.arange(start, stop)
            if source_format == "consistent_trees_ascii":
                spans = (files == -1) & (units == -1)
                bad = ~spans & ((files < 0) | (units < 0))
                failures.add(
                    "forest(s) with one -1 ordinal (spanning forests carry -1 in both)",
                    int(bad.sum()),
                    rows[bad].tolist(),
                )
                before = np.empty(fid.size, dtype=np.int64)
                before[0] = fid[0] - 1 if prev[2] is None else prev[2]
                before[1:] = fid[:-1]
                bad = fid <= before
                failures.add(
                    "ForestID value(s) not strictly ascending (the ASCII forest enumeration)",
                    int(bad.sum()),
                    rows[bad].tolist(),
                )
                prev = (-1, -1, int(fid[-1]))
                continue
            if source_format == "lhalo_binary":
                bad = fid != rows
                failures.add(
                    "ForestID value(s) that are not the dense run forest number",
                    int(bad.sum()),
                    rows[bad].tolist(),
                )
            prev_file = np.empty(fid.size, dtype=np.int64)
            prev_unit = np.empty(fid.size, dtype=np.int64)
            prev_file[0], prev_unit[0] = prev[0], prev[1]
            prev_file[1:], prev_unit[1:] = files[:-1], units[:-1]
            same = files == prev_file
            bad = (files < 0) | np.where(
                same, units != prev_unit + 1, (files <= prev_file) | (units != 0)
            )
            failures.add(
                "forest(s) out of dense (file, unit) inventory order",
                int(bad.sum()),
                rows[bad].tolist(),
            )
            for ordinal, count in zip(*np.unique(files, return_counts=True)):
                per_file[int(ordinal)] = per_file.get(int(ordinal), 0) + int(count)
            prev = (int(files[-1]), int(units[-1]), None)
    messages = failures.messages()
    if inventory is not None and source_format in _V3_UNIT_FORESTS:
        recorded = {
            int(entry["source_file_ordinal"]): int(entry["n_units"]) for entry in inventory["files"]
        }
        if per_file != {k: v for k, v in recorded.items() if v}:
            messages.append(
                "per-file forest counts {} differ from the inventory's units {}".format(
                    dict(sorted(per_file.items())), dict(sorted(recorded.items()))
                )
            )
        if n_forests_total != int(inventory["n_units"]):
            messages.append(
                "n_forests_total {} != the inventory's {} units".format(
                    n_forests_total, inventory["n_units"]
                )
            )
    return messages


def _v3_count_conservation(counts: List[int], manifest) -> List[str]:
    failures = []
    total = int(sum(counts))
    inventory = manifest.inventory or {}
    if total != int(inventory.get("selected_halos", -1)):
        failures.append(
            "emitted halo total {} != the source inventory's selected {}".format(
                total, inventory.get("selected_halos")
            )
        )
    ingest = manifest.stage("ingest").get("result") or {}
    recorded = ingest.get("snapshot_counts")
    if recorded is None or [int(v) for v in recorded] != [int(v) for v in counts]:
        differing = [
            snap
            for snap, (got, want) in enumerate(zip(counts, recorded or []))
            if int(got) != int(want)
        ]
        failures.append(
            "per-snapshot halo counts differ from the ingest stage's source counts (snapshots {})".format(
                _examples(differing) if differing else "all"
            )
        )
    transpose_total = (manifest.stage("transpose").get("result") or {}).get("total_halos")
    if transpose_total is not None and int(transpose_total) != total:
        failures.append(
            "emitted halo total {} != the transpose's {}".format(total, transpose_total)
        )
    return failures


# ---------------------------------------------------------------------------
# Battery driver
# ---------------------------------------------------------------------------


def _require_budget(budget_bytes) -> int:
    if isinstance(budget_bytes, bool) or not isinstance(budget_bytes, (int, np.integer)):
        raise ConverterError("budget_bytes must be an integer, got {!r}".format(budget_bytes))
    budget_bytes = int(budget_bytes)
    if budget_bytes < V3_MIN_BUDGET_BYTES:
        raise ConverterError(
            "budget_bytes {} is below the v3 battery's minimum of {}".format(
                budget_bytes, V3_MIN_BUDGET_BYTES
            )
        )
    return budget_bytes


def run_battery_v3(
    directory,
    a_list_path,
    manifest_path=None,
    multiplier: int = DEFAULT_MULTIPLIER,
    *,
    budget_bytes: int = DEFAULT_V3_BUDGET_BYTES,
    spill_dir=None,
) -> V3BatteryResult:
    """Run the version 3 producer battery over one dataset directory.

    ``manifest_path`` names the generic conversion's ``manifest.json``; it
    binds the dataset to its conversion (schema, inventory, stage counts,
    registered outputs). Without it (API mode, unreachable from the CLI) the
    binding and conservation checks are SKIP and coverage is judged for
    uniqueness only. ``budget_bytes`` bounds the scan and sort buffers;
    ``spill_dir`` hosts the private sort directory (default: the system
    temporary directory), which is removed on every path.
    """
    budget_bytes = _require_budget(budget_bytes)
    directory = Path(directory)
    if not directory.is_dir():
        raise ConverterError("{}: not a directory".format(directory))
    a_list, _ = load_a_list(a_list_path)
    n_snapshots = len(a_list)
    outcomes: List[Outcome] = []
    measurements: Dict[str, object] = {"budget_bytes": budget_bytes}

    def record(name: str, failures: List[str], detail: str = "") -> bool:
        if failures:
            outcomes.append(Outcome(name, "FAIL", "; ".join(failures)))
            return False
        outcomes.append(Outcome(name, "PASS", detail))
        return True

    def skip(names, reason: str) -> None:
        for name in names:
            outcomes.append(Outcome(name, "SKIP", reason))

    structural_ok = record("file-set", _v3_check_file_set(directory, n_snapshots))
    declarations: List[Dict[str, Tuple[str, ...]]] = []
    if structural_ok:
        failures = []
        for snap in range(n_snapshots):
            path = directory / snapshot_h5_name(snap)
            try:
                file_failures, declared = _v3_snapshot_structure(path)
            except OSError as exc:
                file_failures, declared = ["unreadable as HDF5 ({})".format(exc)], None
            failures += ["{}: {}".format(path.name, f) for f in file_failures]
            declarations.append(declared or {})
        structural_ok = record("object-set", failures)
        try:
            sidecar_failures = _v3_sidecar_structure(directory / "forests.h5")
        except OSError as exc:
            sidecar_failures = ["forests.h5: unreadable as HDF5 ({})".format(exc)]
        structural_ok = record("sidecar-object-set", sidecar_failures) and structural_ok
    else:
        skip(("object-set", "sidecar-object-set"), "file set invalid")

    manifest = None
    if manifest_path is not None:
        try:
            manifest = _v3_load_manifest(Path(manifest_path))
        except (ConverterError, OSError, ValueError, KeyError) as exc:
            record("manifest-binding", ["cannot load the conversion manifest: {}".format(exc)])
        else:
            record(
                "manifest-binding", _v3_manifest_binding(directory, a_list_path, a_list, manifest)
            )
    else:
        skip(("manifest-binding",), "no manifest given (API mode; unreachable from the CLI)")

    semantic = V3_CHECKS[V3_CHECKS.index("header-values") :]
    if not structural_ok:
        skip(("schema-binding",) + semantic, "structural conformance failed; semantics not trusted")
        return V3BatteryResult(_ordered(outcomes), measurements)
    record("schema-binding", _v3_schema_binding(declarations, manifest))
    if any(declared != declarations[0] for declared in declarations):
        skip(semantic, "files declare different /schema; their columns are not comparable")
        return V3BatteryResult(_ordered(outcomes), measurements)

    headers = [_v3_read_header(directory / snapshot_h5_name(snap)) for snap in range(n_snapshots)]
    counts = [int(header["_rows"]) for header in headers]
    measurements["format_versions"] = sorted({int(h["format_version"]) for h in headers})
    measurements["snapshot_counts"] = counts
    measurements["snapshot_bytes"] = [int(h["_bytes"]) for h in headers]
    measurements["sidecar_bytes"] = int((directory / "forests.h5").stat().st_size)
    header_failures, run_scoped_failures = _v3_check_headers(headers, a_list, manifest)
    record("header-values", header_failures)
    run_scoped_ok = record("run-scoped-headers", run_scoped_failures)
    first = headers[0]
    n_forests_total = int(first["n_forests_total"]) if run_scoped_ok else None
    max_rank = int(first["max_halo_rank_in_forest"])
    source_format = first["source_format"]
    measurements.update(
        n_forests_total=int(first["n_forests_total"]),
        header_max_halo_rank_in_forest=max_rank,
        header_links_adjacent=int(first["links_adjacent"]),
        source_format=source_format,
        column_mapping_sha256=first["column_mapping_sha256"],
    )

    spill_root = Path(tempfile.mkdtemp(prefix=V3_SPILL_PREFIX, dir=spill_dir))
    scan = None
    try:
        scan = _V3Scan(
            directory, counts, declarations[0], n_forests_total, budget_bytes, spill_root
        )
        scan.run()
        record("row-values", scan.row_values.messages())
        record(
            "len-nonnegative",
            scan.lengths.messages(),
            detail="{} Len==0 halo(s)".format(scan.len_zero),
        )
        record(
            "field-finiteness",
            scan.finiteness.messages(),
            detail="{} float field(s)".format(len(scan.floats)),
        )
        links_ok = record("link-targets", scan.links.messages())
        measured_adjacent = int(scan.stats["gapped_descendants"] == 0)
        adjacency = [
            "{}: links_adjacent {} but the dataset measures {} ({} gapped descendant link(s))".format(
                snapshot_h5_name(snap),
                header["links_adjacent"],
                measured_adjacent,
                scan.stats["gapped_descendants"],
            )
            for snap, header in enumerate(headers)
            if header["links_adjacent"] != measured_adjacent
        ]
        if links_ok:
            record("links-adjacent", adjacency)
            record("topology-closure", _v3_topology_closure(scan))
            cycle_failures, rounds = _v3_chain_cycles(scan)
            record(
                "chain-cycles", cycle_failures, detail="{} pointer-jumping round(s)".format(rounds)
            )
        else:
            skip(
                ("links-adjacent", "topology-closure", "chain-cycles"),
                "link targets invalid; topology not walked",
            )
            scan.join.close()
            scan.chain.close()
        inventory = manifest.inventory if manifest is not None else None
        record("source-key-coverage", _v3_source_keys(scan, inventory))
        if run_scoped_ok:
            unsampled = inventory is not None and int(inventory["selected_halos"]) == int(
                inventory["total_halos"]
            )
            record(
                "identity",
                scan.identity_ranges.messages()
                + _v3_identity(scan, source_format, n_forests_total, max_rank, unsampled),
            )
            record("header-bounds", check_header_bounds(n_forests_total, max_rank, multiplier))
            record(
                "sidecar-content",
                _v3_sidecar_content(
                    directory / "forests.h5",
                    source_format,
                    n_forests_total,
                    inventory,
                    scan.scan_rows,
                ),
            )
        else:
            skip(
                ("identity", "header-bounds", "sidecar-content"), "run-scoped headers inconsistent"
            )
        if manifest is not None:
            record("count-conservation", _v3_count_conservation(counts, manifest))
        else:
            skip(("count-conservation",), "no manifest given; source counts unavailable")
        measurements.update(scan.stats)
        measurements.update(
            len_zero=scan.len_zero,
            links_adjacent_measured=measured_adjacent,
            chain_edges=scan.n_chain_edges,
            scan_rows=scan.scan_rows,
        )
    except RankSortError as exc:
        raise ConverterError("v3 battery sort failed: {}".format(exc)) from exc
    finally:
        if scan is not None:
            scan.close()
            measurements["peak_resident_bytes"] = int(scan.meter.bytes_peak)
            measurements["peak_spill_bytes"] = int(scan.ledger.peak_bytes)
        shutil.rmtree(str(spill_root), ignore_errors=True)
    if spill_root.exists():
        raise ConverterError(
            "{}: the battery's spill directory could not be removed".format(spill_root)
        )
    return V3BatteryResult(_ordered(outcomes), measurements)


def _ordered(outcomes: List[Outcome]) -> List[Outcome]:
    """Outcomes in :data:`V3_CHECKS` order, whatever order they were settled
    in."""
    rank = {name: position for position, name in enumerate(V3_CHECKS)}
    return sorted(outcomes, key=lambda outcome: rank[outcome.name])


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(
        prog="validate",
        description="Producer validation battery for horizontal-HDF5 datasets: format "
        "version 2 (docs/dev/HORIZONTAL-HDF5-FORMAT.md) or, when every snapshot file "
        "declares it, version 3 (docs/dev/HORIZONTAL-HDF5-FORMAT-V3-DRAFT.md)",
    )
    parser.add_argument("directory", help="directory of snapshot_NNN.h5 files + forests.h5")
    parser.add_argument("--a-list", required=True, help="canonical a_list (one scale per line)")
    parser.add_argument(
        "--manifest",
        required=True,
        help="conversion manifest.json: the legacy workdir's for version 2 (count "
        "conservation against the independent per-source-file pre-counts), the generic "
        "workdir's for version 3 (schema, inventory and registered-output binding); "
        "mandatory for the producer battery",
    )
    parser.add_argument(
        "--multiplier",
        type=int,
        default=DEFAULT_MULTIPLIER,
        help="UniqueGalaxyID multiplier for the header bound checks "
        "(default {})".format(DEFAULT_MULTIPLIER),
    )
    parser.add_argument(
        "--memory-budget-mb",
        type=int,
        default=DEFAULT_V3_BUDGET_BYTES >> 20,
        help="working-buffer budget of the version 3 battery's bounded sorts, in MiB "
        "(default {}; ignored for version 2)".format(DEFAULT_V3_BUDGET_BYTES >> 20),
    )
    args = parser.parse_args(argv)
    try:
        outcomes = run_producer_battery(
            args.directory,
            args.a_list,
            manifest_path=args.manifest,
            multiplier=args.multiplier,
            budget_bytes=args.memory_budget_mb << 20,
        )
    except ConverterError as exc:
        print("ERROR: {}".format(exc), file=sys.stderr)
        return 1
    for outcome in outcomes:
        print(outcome.line())
    if battery_failed(outcomes):
        print("validation: FAIL", file=sys.stderr)
        return 1
    print("validation: PASS", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
