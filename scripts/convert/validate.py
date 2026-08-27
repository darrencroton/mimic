"""Producer validation battery for the ctrees -> snapshot-HDF5 converter
(plan Slice 7).

Implements the full producer battery from docs/dev/SNAPSHOT-HDF5-FORMAT.md
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
"""

import argparse
import json
import os
import sys
from pathlib import Path
from typing import Dict, List, Optional, Tuple

import h5py
import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from ctrees_parser import ConverterError  # noqa: E402
from hdf5_writer import (  # noqa: E402
    CHUNK_1D,
    CHUNK_VEC,
    FORMAT_VERSION,
    HALO_DATASETS,
    HEADER_ATTRS,
    snapshot_h5_name,
)
from scatter import file_md5, load_a_list  # noqa: E402

#: Default UniqueGalaxyID multiplier (TREE_MUL_FAC, src/include/constants.h).
DEFAULT_MULTIPLIER = 10**9

#: Rows the two identity passes read at a time. Four HDF5 chunks (``CHUNK_1D``
#: is 65536 rows) per read, so reads stay chunk-aligned, and small enough that
#: the per-chunk working arrays enumerated in ``IDENTITY_CHUNK_BYTES_PER_ROW``
#: cost tens of MB rather than a fraction of the dataset.
IDENTITY_CHUNK_ROWS = 4 * CHUNK_1D[0]

#: Bytes an identity pass holds per row of a chunk, enumerated rather than
#: estimated. Eleven int64-wide arrays: the ForestIndex and HaloRankInForest
#: blocks read from the file, each row's forest count and bitset base, the
#: in-range row indices, their slots, those slots' byte indices, the stable
#: sort permutation, the sorted slots, the deduplicated slots and their byte
#: indices. Four one-byte arrays: the in-range mask, the slot bit masks, the
#: already-claimed mask and the repeat mask.
IDENTITY_CHUNK_BYTES_PER_ROW = 11 * 8 + 4

#: Slots of the identity bitset examined at once when a failure message needs
#: the first rank a forest holds no halo for. Bounded so the diagnostic scan of
#: a single huge forest cannot itself allocate without limit.
_MISSING_RANK_SCAN_SLOTS = 1 << 20

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


def load_dataset(
    directory: Path, n_snapshots: int
) -> Tuple[List[dict], List[Dict[str, np.ndarray]]]:
    """Load every snapshot file's header attributes and /halos arrays.

    **The battery no longer uses this**, and nothing in this module calls it:
    the checks stream through :class:`_Snapshots` instead. It is retained
    because ``crosscheck.py`` still imports it, and the cross-check is bounded
    by its own slice of the converter scale pass (plan Slice 7), which owns
    both that consumer and this function's removal.
    """
    headers = []
    arrays = []
    for snap in range(n_snapshots):
        path = directory / snapshot_h5_name(snap)
        with h5py.File(path, "r") as handle:
            headers.append(
                {name: np.asarray(handle["header"].attrs[name])[()] for name in HEADER_ATTRS}
            )
            arrays.append({name: handle["halos"][name][...] for name in HALO_DATASETS})
    return headers, arrays


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
    """Halo count and bitset base for every ForestIndex group in the dataset.

    In-range values are two ``n_forests_total``-sized int64 arrays — the
    forest-count-sized metadata the battery is allowed to hold. Out-of-range
    values get their own groups after them, because the whole-dataset battery
    grouped them too: it sorted on ForestIndex alone, so a pair of halos with
    ForestIndex -1 and ranks 0 and 1 formed one dense group and failed only the
    density condition, not the pair-uniqueness one. They are held in arrays
    sized by the number of DISTINCT out-of-range values, which is zero on any
    dataset that satisfies the density condition.
    """

    def __init__(self, n_forests_total: int, counts: np.ndarray, extra_counts: Dict[int, int]):
        self.n_forests_total = int(n_forests_total)
        self.counts = counts
        self.bases = np.zeros(counts.size, dtype=np.int64)
        if counts.size > 1:
            np.cumsum(counts[:-1], out=self.bases[1:])
        self.extra_values = np.asarray(sorted(extra_counts), dtype=np.int64)
        self.extra_counts = np.asarray(
            [extra_counts[value] for value in self.extra_values.tolist()], dtype=np.int64
        )
        self.extra_bases = np.zeros(self.extra_values.size, dtype=np.int64)
        if self.extra_values.size > 1:
            np.cumsum(self.extra_counts[:-1], out=self.extra_bases[1:])
        self.extra_bases += int(counts.sum())

    def lookup(self, forest: np.ndarray) -> Tuple[np.ndarray, np.ndarray]:
        """Per-row forest halo count and bitset base for one chunk."""
        limits = np.empty(forest.size, dtype=np.int64)
        bases = np.empty(forest.size, dtype=np.int64)
        in_range = (forest >= 0) & (forest < self.n_forests_total)
        rows = np.nonzero(in_range)[0]
        if rows.size:
            limits[rows] = self.counts[forest[rows]]
            bases[rows] = self.bases[forest[rows]]
        rows = np.nonzero(~in_range)[0]
        if rows.size:
            index = np.searchsorted(self.extra_values, forest[rows])
            limits[rows] = self.extra_counts[index]
            bases[rows] = self.extra_bases[index]
        return limits, bases

    def group_of(self, value: int) -> Tuple[int, int]:
        """``(base, count)`` of one ForestIndex value, in range or not."""
        if 0 <= value < self.n_forests_total:
            return int(self.bases[value]), int(self.counts[value])
        index = int(np.searchsorted(self.extra_values, value))
        return int(self.extra_bases[index]), int(self.extra_counts[index])


def _identity_counts(
    snapshots: _Snapshots, n_forests_total: int
) -> Tuple[np.ndarray, Dict[int, int], int, Optional[int]]:
    """First identity pass: halos per in-range ForestIndex, halos per distinct
    out-of-range ForestIndex value, the dataset's halo total, and the largest
    HaloRankInForest.

    Between them these settle the density condition over
    ``[0, n_forests_total)`` and the maximum-rank condition, and they size both
    the bitset the second pass claims into and the groups within it.
    """
    counts = np.zeros(max(int(n_forests_total), 0), dtype=np.int64)
    extra_counts: Dict[int, int] = {}
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
                values, repeats = np.unique(forest[~in_range], return_counts=True)
                for value, repeat in zip(values.tolist(), repeats.tolist()):
                    extra_counts[value] = extra_counts.get(value, 0) + repeat
    return counts, extra_counts, total, measured_max


def check_identity(
    snapshots: _Snapshots, n_forests_total: int, max_rank_header: int
) -> Tuple[List[str], int]:
    """Invariant 4: (ForestIndex, HaloRankInForest) unique across the dataset,
    ForestIndex dense over [0, n_forests_total), per-forest ranks dense, and
    the measured maximum rank equal to the header value.

    Three independent conditions, each still able to fail on its own and each
    reported in the order the whole-dataset formulation reported it:

    (a) ForestIndex dense over ``[0, n_forests_total)`` — settled by the
        counting pass, which sees every distinct value the dataset carries;
    (b) per-forest ranks dense and unique over ``0 .. count-1`` — settled
        exactly by :class:`_IdentityBits`, one bit per halo;
    (c) the measured maximum rank equals ``max_halo_rank_in_forest`` — a
        running maximum over the same counting pass.

    Returns the failures and the identity structure's peak bytes, which the
    caller reports rather than estimates.

    **The (b) violation count and its five examples are this formulation's
    own.** The whole-dataset battery counted positions in a global lexsort of
    every pair, and reproducing that count exactly would need a multiplicity
    per slot instead of a bit (>= 1 byte per halo, 22.9 GB at production) — the
    memory wall this battery exists to remove. What is DETECTED is identical: a
    rank outside its forest's ``[0, count)``, or a slot claimed twice, IS a
    per-forest density or pair-uniqueness violation, and nothing else is. The
    message shape is identical, the examples are still the five lowest
    ``(ForestIndex, rank)`` pairs that violated, and ``expected`` in each is the
    first rank that forest holds no halo for.
    """
    failures: List[str] = []
    counts, extra_counts, total, measured_max = _identity_counts(snapshots, n_forests_total)
    if total == 0:
        if n_forests_total != 0:
            failures.append(
                "dataset has no halos but n_forests_total is {}".format(n_forests_total)
            )
        return failures, 0

    distinct = int(np.count_nonzero(counts)) + len(extra_counts)
    if extra_counts or distinct != n_forests_total:
        lowest = sorted(np.flatnonzero(counts)[:5].tolist() + sorted(extra_counts)[:5])[:5]
        failures.append(
            "ForestIndex values are not dense over [0, {}); {} distinct value(s) observed, "
            "examples: {}".format(n_forests_total, distinct, _examples(lowest))
        )

    forests = _ForestTable(n_forests_total, counts, extra_counts)
    bits = _IdentityBits(total)
    try:
        peak_bytes = bits.peak_bytes
        n_bad = 0
        examples: List[Tuple[int, int]] = []
        for snap in range(snapshots.n_snapshots):
            for forest, rank in snapshots.chunks(
                snap, ("ForestIndex", "HaloRankInForest"), IDENTITY_CHUNK_ROWS
            ):
                limits, bases = forests.lookup(forest)
                held = (rank >= 0) & (rank < limits)
                rows = np.nonzero(held)[0]
                rejected = ~held
                rejected[rows[bits.claim(bases[rows] + rank[rows])]] = True
                bad_rows = np.nonzero(rejected)[0]
                if bad_rows.size:
                    n_bad += int(bad_rows.size)
                    # keep only the five lowest (ForestIndex, rank) pairs seen
                    # so far, so the example state is five tuples, not a list
                    # that grows with the number of violations
                    pick = bad_rows[np.lexsort((rank[bad_rows], forest[bad_rows]))[:5]]
                    examples = sorted(
                        examples + list(zip(forest[pick].tolist(), rank[pick].tolist()))
                    )[:5]
        if n_bad:
            detail = [
                "(ForestIndex={}, rank={}, expected {})".format(
                    value, held_rank, bits.first_unheld(*forests.group_of(value))
                )
                for value, held_rank in examples
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
    return failures, peak_bytes


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
        identity_failures, identity_bytes = check_identity(snapshots, n_forests_total, max_rank)
        record("identity", identity_failures)
        _log(
            "validate: identity checked exactly against a {} byte bitset "
            "(1 bit per halo)".format(identity_bytes)
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


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(
        prog="validate",
        description="Producer validation battery for snapshot-HDF5 datasets "
        "(docs/dev/SNAPSHOT-HDF5-FORMAT.md)",
    )
    parser.add_argument("directory", help="directory of snapshot_NNN.h5 files + forests.h5")
    parser.add_argument("--a-list", required=True, help="canonical a_list (one scale per line)")
    parser.add_argument(
        "--manifest",
        required=True,
        help="conversion manifest.json (count conservation against the independent "
        "per-source-file pre-counts is mandatory for the producer battery)",
    )
    parser.add_argument(
        "--multiplier",
        type=int,
        default=DEFAULT_MULTIPLIER,
        help="UniqueGalaxyID multiplier for the header bound checks "
        "(default {})".format(DEFAULT_MULTIPLIER),
    )
    args = parser.parse_args(argv)
    try:
        outcomes = run_battery(
            args.directory, args.a_list, manifest_path=args.manifest, multiplier=args.multiplier
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
