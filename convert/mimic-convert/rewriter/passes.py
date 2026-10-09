"""Passes 1 and 2: per-slab ranking and labelling, writing and verification.

**Pass 1** (:func:`rank_slab`) computes, for one slab in descending snapshot
order, the reference row order and identities from the input's own columns:

- the key is (``ForestIndex`` ascending, upid ascending, pid ascending,
  ``MostBoundID`` ascending), where upid is the FoF central's ``MostBoundID``
  (the ``FirstHaloInFOFgroup`` row's) and pid is -1 for a central, else upid
  -- the post-fix-up reference key restricted to one snapshot, whose
  descending-snapshot term is the pass order itself. Within a slab every
  ``MostBoundID`` is distinct, so the key is total; a repeated key is refused;
- ``HaloRankInForest`` continues each forest's running count over the slabs
  already processed (higher snapshots), ``SourceHaloID`` is the forest's base
  plus the rank, and ``ForestIndex`` is carried. The computed rank must equal
  the input's ``HaloRankInForest`` for every row, which proves the derivation
  on the input rather than assuming it;
- every halo is labelled with its tree: a terminal halo (``Descendant`` -1)
  with the ordinal of its own ``MostBoundID`` among the roots, every other
  with its descendant's label in the next slab. The per-tree counts
  accumulate the inventory's ``n_rows``.

**Pass 2** (:func:`write_slab`, :func:`verify_slab`) produces one output slab
from the input slab and the remap entries of slabs N+1, N and N-1: every
column gathered into the new order, every link remapped through its target
slab's inverse permutation (``Descendant`` into N+1, ``FirstProgenitor`` into
N-1, ``NextProgenitor`` and the FoF links into N) with its companion snapshot
column -1 iff the link is -1, the identities from the remap store, and
``SnapNum`` the slab's number. The file is written under a temporary name,
:func:`verify_slab` re-reads that temporary file, and the caller renames it to
its final name only on success. The verification checks:

- the object set, header attributes, dataset layouts and ``/schema`` the
  format fixes;
- every column against the input's under the permutation;
- every link's target row carries the ``MostBoundID`` the input's target row
  carried (through the target slab's permutation), FoF centrals stay
  self-referencing, and the companion columns name the target slab;
- ``SnapNum``, ``ForestIndex`` non-decreasing, ranks dense within each
  forest's run, and ``SourceHaloID`` the forest base plus the rank.

Memory per slab is stated in the package docstring; every array here is
int64-indexed and released as soon as the next step no longer needs it.
"""

import os
import sys
from dataclasses import dataclass
from typing import Dict, Mapping, Optional, Sequence, Tuple

import h5py
import numpy as np

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from column_schema import PayloadField  # noqa: E402
from errors import ConverterError  # noqa: E402
from hdf5_writer import CHUNK_1D, HEADER_ATTRS  # noqa: E402
from hdf5_writer_v3 import (  # noqa: E402
    V3_SCHEMA_ATTRS,
    V3_SIDECAR_DATASETS,
    V3_STRING_ATTRS,
    V3_WRITE_BLOCK_ROWS,
    write_v3_sidecar,
    write_v3_snapshot_columns,
)
from horizontal_dataset import HorizontalDataset  # noqa: E402

#: Each link, its companion snapshot column (None for the FoF links) and the
#: offset of its target slab from the owner's.
LINKS: Tuple[Tuple[str, Optional[str], int], ...] = (
    ("Descendant", "DescendantSnapshot", 1),
    ("FirstProgenitor", "FirstProgenitorSnapshot", -1),
    ("NextProgenitor", "NextProgenitorSnapshot", 0),
    ("FirstHaloInFOFgroup", None, 0),
    ("NextHaloInFOFgroup", None, 0),
)
_LINK_NAMES = tuple(name for name, _companion, _offset in LINKS)
_LINK_INFO = {name: (companion, offset) for name, companion, offset in LINKS}
_COMPANIONS = {companion: name for name, companion, _offset in LINKS if companion is not None}
_IDENTITY = ("SourceHaloID", "ForestIndex", "HaloRankInForest")

#: Columns pass 1 reads from the input slab.
PASS1_COLUMNS = (
    "ForestIndex",
    "FirstHaloInFOFgroup",
    "MostBoundID",
    "HaloRankInForest",
    "Descendant",
)


def _examples(values, limit: int = 5) -> list:
    return list(values)[:limit]


def _rows(path, rows) -> str:
    return "{} rows {}".format(path, _examples(np.asarray(rows).tolist()))


# ---------------------------------------------------------------------------
# Pass 1
# ---------------------------------------------------------------------------


@dataclass
class SlabRemap:
    """One slab's remap entry: its new row order and identities (32 B per
    row). ``perm[new_row]`` is the input row now at ``new_row``."""

    snap: int
    perm: np.ndarray  # int64 [n]
    forest: np.ndarray  # int64 [n], new order
    rank: np.ndarray  # int64 [n], new order
    sid: np.ndarray  # int64 [n], new order

    @property
    def n(self) -> int:
        return int(self.perm.size)

    @property
    def nbytes(self) -> int:
        return int(self.perm.nbytes + self.forest.nbytes + self.rank.nbytes + self.sid.nbytes)

    def inverse(self) -> np.ndarray:
        """``inverse[old_row]`` = the new row of input row ``old_row``."""
        inverse = np.empty(self.n, dtype=np.int64)
        inverse[self.perm] = np.arange(self.n, dtype=np.int64)
        return inverse


def _int64(dataset: HorizontalDataset, snap: int, name: str) -> np.ndarray:
    return np.ascontiguousarray(dataset.read_column(snap, name), dtype=np.int64)


def rank_slab(
    dataset: HorizontalDataset,
    snap: int,
    sid_base: np.ndarray,
    running: np.ndarray,
    roots: np.ndarray,
    labels_next: Optional[np.ndarray],
    tree_counts: np.ndarray,
    identity_from_input: bool = False,
) -> Tuple[SlabRemap, np.ndarray]:
    """Pass 1 for one slab (module docstring). ``running`` and ``tree_counts``
    are advanced in place; ``labels_next`` is the next slab's labels in its
    input row order (``None`` for the last slab). With ``identity_from_input``
    (the no-op) the derived order must be the input's own: the permutation
    the identity and the derived ``ForestIndex``, ``HaloRankInForest`` and
    ``SourceHaloID`` the stored columns, refused otherwise naming the first
    row. Returns the remap entry and this slab's labels in input row order."""
    path = dataset.snapshot_path(snap)
    n = dataset.n_halos[snap]
    if n == 0:
        empty = np.zeros(0, dtype=np.int64)
        remap = SlabRemap(snap=snap, perm=empty, forest=empty, rank=empty, sid=empty)
        return remap, np.zeros(0, dtype=np.int32)
    forest = _int64(dataset, snap, "ForestIndex")
    fhif = _int64(dataset, snap, "FirstHaloInFOFgroup")
    bad = (fhif < 0) | (fhif >= n)
    if bad.any():
        raise ConverterError(
            "{}: {} FirstHaloInFOFgroup value(s) outside [0, {}); {}".format(
                path, int(bad.sum()), n, _rows(path, np.flatnonzero(bad))
            )
        )
    not_central = fhif[fhif] != fhif
    if not_central.any():
        raise ConverterError(
            "{}: {} halo(s) whose FoF central is not self-referencing; {}".format(
                path, int(not_central.sum()), _rows(path, np.flatnonzero(not_central))
            )
        )
    del not_central
    mbid = _int64(dataset, snap, "MostBoundID")
    rows = np.arange(n, dtype=np.int64)
    upid = mbid[fhif]
    pid = np.where(fhif == rows, -1, upid)
    del fhif, rows
    perm = np.lexsort((mbid, pid, upid, forest))
    if identity_from_input:
        moved = perm != np.arange(n, dtype=np.int64)
        if moved.any():
            first = int(np.flatnonzero(moved)[0])
            raise ConverterError(
                "{}: the stored row order is not the (ForestIndex, HaloRankInForest) order the "
                "input's own columns derive: {} row(s) move, the first at row {} (input row {} "
                "belongs there); refusing to reproduce it".format(
                    path, int(moved.sum()), first, int(perm[first])
                )
            )
        del moved
    sorted_forest = forest[perm]
    del forest
    if n > 1:
        same = sorted_forest[1:] == sorted_forest[:-1]
        for column in (upid, pid, mbid):
            ordered = column[perm]
            same &= ordered[1:] == ordered[:-1]
            del ordered
        if same.any():
            where = perm[1:][same]
            raise ConverterError(
                "{}: {} row(s) repeat another row's (ForestIndex, upid, pid, MostBoundID) key; "
                "the reference order needs distinct ids within a slab; {}".format(
                    path, int(same.sum()), _rows(path, where)
                )
            )
        del same
    del upid, pid
    starts = np.flatnonzero(np.r_[True, sorted_forest[1:] != sorted_forest[:-1]])
    lengths = np.diff(np.append(starts, n))
    run_forests = sorted_forest[starts]
    local = np.arange(n, dtype=np.int64) - np.repeat(starts, lengths)
    rank = np.repeat(running[run_forests], lengths) + local
    del local
    running[run_forests] += lengths
    sid = sid_base[sorted_forest] + rank
    rank_in = _int64(dataset, snap, "HaloRankInForest")[perm]
    bad = rank_in != rank
    if bad.any():
        where = np.flatnonzero(bad)[:5]
        raise ConverterError(
            "{}: {} row(s) whose HaloRankInForest differs from the reference rank derived from "
            "the input columns; examples (input row, held, derived): {}".format(
                path,
                int(bad.sum()),
                [(int(perm[w]), int(rank_in[w]), int(rank[w])) for w in where],
            )
        )
    del rank_in, bad
    if identity_from_input:
        sid_in = _int64(dataset, snap, "SourceHaloID")
        bad = sid_in != sid
        if bad.any():
            first = int(np.flatnonzero(bad)[0])
            raise ConverterError(
                "{}: {} row(s) whose stored SourceHaloID differs from the forest base plus the "
                "derived rank, the first at row {} (stored {}, derived {}); refusing to "
                "reproduce it".format(
                    path, int(bad.sum()), first, int(sid_in[first]), int(sid[first])
                )
            )
        del sid_in, bad

    desc = _int64(dataset, snap, "Descendant")
    terminal = desc == -1
    labels = np.empty(n, dtype=np.int32)
    if terminal.any():
        ids = mbid[terminal]
        position = np.searchsorted(roots, ids)
        position = np.minimum(position, max(roots.size - 1, 0))
        unknown = roots.size == 0 or roots[position] != ids
        if np.any(unknown):
            raise ConverterError(
                "{}: {} terminal halo(s) whose MostBoundID is not a root; {}".format(
                    path, int(np.sum(unknown)), _rows(path, np.flatnonzero(terminal)[unknown])
                )
            )
        del ids, unknown
        labels[terminal] = position
        del position
    del mbid
    linked = ~terminal
    if linked.any():
        if labels_next is None:
            raise ConverterError(
                "{}: {} halo(s) of the last slab carry a Descendant; {}".format(
                    path, int(linked.sum()), _rows(path, np.flatnonzero(linked))
                )
            )
        targets = desc[linked]
        bad = (targets < 0) | (targets >= labels_next.size)
        if bad.any():
            raise ConverterError(
                "{}: {} Descendant value(s) outside [-1, {}); {}".format(
                    path, int(bad.sum()), labels_next.size, _rows(path, np.flatnonzero(linked)[bad])
                )
            )
        labels[linked] = labels_next[targets]
        del targets, bad
    del desc, terminal, linked
    present, counts = np.unique(labels, return_counts=True)
    tree_counts[present] += counts
    return SlabRemap(snap=snap, perm=perm, forest=sorted_forest, rank=rank, sid=sid), labels


# ---------------------------------------------------------------------------
# Pass 2
# ---------------------------------------------------------------------------


@dataclass
class Window:
    """The remap entries pass 2 resolves one slab's links through."""

    own: SlabRemap
    above: Optional[SlabRemap]  # slab N+1, the Descendant target; None for the last
    below: Optional[SlabRemap]  # slab N-1, the FirstProgenitor target; None for the first

    def target(self, offset: int) -> Optional[SlabRemap]:
        return {1: self.above, 0: self.own, -1: self.below}[offset]


class SlabColumns:
    """The output columns of one slab, computed on demand from the input slab
    and the window: one column resident at a time, the target slab's inverse
    permutation while a link is remapped, and the null masks of the links.
    With ``identity_from_input`` the identity columns are the input's own
    (which pass 1 has proved equal to the derived ones)."""

    def __init__(
        self,
        dataset: HorizontalDataset,
        snap: int,
        window: Window,
        table: Mapping[str, Tuple[np.dtype, bool]],
        identity_from_input: bool = False,
    ):
        self.dataset = dataset
        self.snap = snap
        self.window = window
        self.table = table
        self.identity_from_input = identity_from_input
        self.path = dataset.snapshot_path(snap)
        self.n = window.own.n
        self.null: Dict[str, np.ndarray] = {}
        self.n_links: Dict[str, int] = {}

    def old_link(self, name: str) -> np.ndarray:
        """The input's link column, gathered into the new row order (so each
        value is still an input row of the target slab)."""
        return _int64(self.dataset, self.snap, name)[self.window.own.perm]

    def link(self, name: str) -> np.ndarray:
        companion, offset = _LINK_INFO[name]
        old = self.old_link(name)
        target = self.window.target(offset)
        null = old == -1
        if name == "FirstHaloInFOFgroup" and null.any():
            raise ConverterError(
                "{}: {} FirstHaloInFOFgroup value(s) of -1; {}".format(
                    self.path, int(null.sum()), _rows(self.path, self.window.own.perm[null])
                )
            )
        n_target = 0 if target is None else target.n
        bad = ~null & ((old < 0) | (old >= n_target))
        if bad.any():
            raise ConverterError(
                "{}: {} {} value(s) outside [-1, {}) of snapshot {}; {}".format(
                    self.path,
                    int(bad.sum()),
                    name,
                    n_target,
                    self.snap + offset,
                    _rows(self.path, self.window.own.perm[bad]),
                )
            )
        if companion is not None and self.dataset.format_version >= 3:
            held = _int64(self.dataset, self.snap, companion)[self.window.own.perm]
            expected = np.where(null, -1, self.snap + offset)
            bad = held != expected
            if bad.any():
                raise ConverterError(
                    "{}: {} {} value(s) that are not -1 iff {} is -1 and otherwise snapshot "
                    "{}; {}".format(
                        self.path,
                        int(bad.sum()),
                        companion,
                        name,
                        self.snap + offset,
                        _rows(self.path, self.window.own.perm[bad]),
                    )
                )
            del held, expected
        new = np.full(self.n, -1, dtype=np.int64)
        if target is not None and (~null).any():
            inverse = target.inverse()
            new[~null] = inverse[old[~null]]
            del inverse
        self.null[name] = null
        self.n_links[name] = int(np.count_nonzero(~null))
        return new

    def companion(self, name: str) -> np.ndarray:
        link = _COMPANIONS[name]
        if link not in self.null:
            self.link(link)
        offset = _LINK_INFO[link][1]
        return np.where(self.null[link], -1, self.snap + offset).astype(np.int32)

    def payload(self, name: str) -> np.ndarray:
        dtype, _is_vec = self.table[name]
        values = self.dataset.read_column(self.snap, name)
        if name == "SnapNum":
            bad = np.asarray(values, dtype=np.int64) != self.snap
            if bad.any():
                raise ConverterError(
                    "{}: {} SnapNum value(s) other than {}; {}".format(
                        self.path, int(bad.sum()), self.snap, _rows(self.path, np.flatnonzero(bad))
                    )
                )
        return np.ascontiguousarray(values[self.window.own.perm], dtype=dtype)

    def identity(self, name: str) -> np.ndarray:
        if self.identity_from_input:
            return _int64(self.dataset, self.snap, name)
        return {
            "SourceHaloID": self.window.own.sid,
            "ForestIndex": self.window.own.forest,
            "HaloRankInForest": self.window.own.rank,
        }[name]

    def __call__(self, name: str) -> np.ndarray:
        if name in _LINK_NAMES:
            return self.link(name)
        if name in _COMPANIONS:
            return self.companion(name)
        if name in _IDENTITY:
            return self.identity(name)
        return self.payload(name)


def write_slab(
    columns: SlabColumns,
    path,
    header: Mapping[str, object],
    declarations: Sequence[PayloadField],
    block_rows: int,
) -> Dict[str, int]:
    """Write one output slab to ``path`` (a temporary name; the caller renames
    it after :func:`verify_slab`). Returns the non-null count per link."""
    write_v3_snapshot_columns(
        path,
        header,
        columns.table,
        declarations,
        columns.n,
        columns,
        block_rows=block_rows,
        track_times=False,
    )
    return dict(columns.n_links)


def _require_hard_links(group, path) -> None:
    for name in group:
        link = group.get(name, getlink=True)
        if not isinstance(link, h5py.HardLink):
            raise ConverterError(
                "{}: {}/{} is a {}, not a physically present object".format(
                    path, group.name.rstrip("/"), name, type(link).__name__
                )
            )


def _check_layout(dataset, dtype: np.dtype, is_vec: bool, n_rows: int, path) -> None:
    shape = (n_rows, 3) if is_vec else (n_rows,)
    problems = []
    if dataset.dtype.str != dtype.str:
        problems.append("dtype {} != {}".format(dataset.dtype.str, dtype.str))
    if dataset.shape != shape:
        problems.append("shape {} != {}".format(dataset.shape, shape))
    chunks = dataset.chunks
    if (
        chunks is None
        or len(chunks) != len(shape)
        or not 1 <= chunks[0] <= CHUNK_1D[0]
        or (is_vec and chunks[1] != 3)
    ):
        problems.append("chunks {} (rows must be in [1, {}])".format(chunks, CHUNK_1D[0]))
    if dataset.compression is not None or dataset.shuffle or dataset.fletcher32:
        problems.append("filtered")
    if dataset.scaleoffset is not None:
        problems.append("scale-offset")
    if dataset.external or dataset.is_virtual:
        problems.append("not stored in this file")
    if dataset.attrs.keys():
        problems.append("carries attributes {}".format(sorted(dataset.attrs.keys())))
    if problems:
        raise ConverterError("{}: {}: {}".format(path, dataset.name, "; ".join(problems)))


def _check_structure(
    handle,
    path,
    header: Mapping[str, object],
    table: Mapping[str, Tuple[np.dtype, bool]],
    declarations: Sequence[PayloadField],
    n_halos: int,
) -> None:
    if set(handle.keys()) != {"header", "halos", "schema"} or handle.attrs.keys():
        raise ConverterError(
            "{}: root holds {} (attributes {}), not exactly /header, /halos, /schema".format(
                path, sorted(handle.keys()), sorted(handle.attrs.keys())
            )
        )
    _require_hard_links(handle, path)
    attrs = handle["header"].attrs
    expected_names = set(HEADER_ATTRS) | set(V3_STRING_ATTRS)
    if set(attrs.keys()) != expected_names:
        raise ConverterError(
            "{}: header attributes {} != {}".format(
                path, sorted(attrs.keys()), sorted(expected_names)
            )
        )
    for name, dtype in HEADER_ATTRS.items():
        stored = np.asarray(attrs[name])
        if stored.dtype != np.dtype(dtype) or stored.shape != () or stored != header[name]:
            raise ConverterError(
                "{}: header {} is {!r} ({}), expected {!r}".format(
                    path, name, stored, stored.dtype, header[name]
                )
            )
    for name, size in V3_STRING_ATTRS.items():
        kind = attrs.get_id(name).get_type()
        if (
            not isinstance(kind, h5py.h5t.TypeStringID)
            or kind.is_variable_str()
            or kind.get_size() != size
            or kind.get_cset() != h5py.h5t.CSET_ASCII
        ):
            raise ConverterError(
                "{}: header {} is not {}-byte fixed ASCII".format(path, name, size)
            )
        if attrs[name] != str(header[name]).encode("ascii"):
            raise ConverterError(
                "{}: header {} is {!r}, expected {!r}".format(path, name, attrs[name], header[name])
            )
    halos = handle["halos"]
    if set(halos.keys()) != set(table) or halos.attrs.keys():
        raise ConverterError(
            "{}: /halos holds {} (attributes {}), expected exactly {}".format(
                path, sorted(halos.keys()), sorted(halos.attrs.keys()), sorted(table)
            )
        )
    _require_hard_links(halos, path)
    for name, (dtype, is_vec) in table.items():
        _check_layout(halos[name], dtype, is_vec, n_halos, path)
    schema_group = handle["schema"]
    wanted = {declaration.name: declaration for declaration in declarations}
    if set(schema_group.keys()) != set(wanted) or schema_group.attrs.keys():
        raise ConverterError(
            "{}: /schema declares {}, expected exactly {}".format(
                path, sorted(schema_group.keys()), sorted(wanted)
            )
        )
    _require_hard_links(schema_group, path)
    for name, declaration in wanted.items():
        group = schema_group[name]
        if (
            not isinstance(group, h5py.Group)
            or len(group)
            or set(group.attrs.keys()) != set(V3_SCHEMA_ATTRS)
        ):
            raise ConverterError(
                "{}: /schema/{} must be a childless group with exactly {}".format(
                    path, name, V3_SCHEMA_ATTRS
                )
            )
        for key in V3_SCHEMA_ATTRS:
            kind = group.attrs.get_id(key).get_type()
            if (
                not isinstance(kind, h5py.h5t.TypeStringID)
                or not kind.is_variable_str()
                or kind.get_cset() != h5py.h5t.CSET_UTF8
                or group.attrs[key] != getattr(declaration, key)
            ):
                raise ConverterError(
                    "{}: /schema/{} {} is {!r}, expected UTF-8 {!r}".format(
                        path, name, key, group.attrs[key], getattr(declaration, key)
                    )
                )


def _read_blocks(dataset, n_rows: int, block_rows: int):
    for start in range(0, n_rows, block_rows):
        yield start, dataset[start : min(start + block_rows, n_rows)]


def _compare(written, expected: np.ndarray, block_rows: int, path, name: str) -> None:
    """A written dataset against its expected column, block by block, as bytes."""
    if written.shape != expected.shape:
        raise ConverterError(
            "{}: /halos/{} has shape {}, expected {}".format(
                path, name, written.shape, expected.shape
            )
        )
    for start, block in _read_blocks(written, expected.shape[0], block_rows):
        stop = start + block.shape[0]
        wanted = np.ascontiguousarray(expected[start:stop], dtype=expected.dtype)
        if block.tobytes() != wanted.tobytes():
            raise ConverterError(
                "{}: /halos/{} rows [{}, {}) differ from the input under the permutation".format(
                    path, name, start, stop
                )
            )


def verify_slab(
    path,
    columns: SlabColumns,
    header: Mapping[str, object],
    declarations: Sequence[PayloadField],
    sid_base: np.ndarray,
    block_rows: int,
) -> None:
    """Re-read one written slab and check everything the module docstring
    lists; raise on the first difference. The input ``MostBoundID`` of one
    target slab at a time is read for the link check (the links are taken in
    target order, so each slab's column is read once)."""
    snap = columns.snap
    own = columns.window.own
    n = own.n
    dataset = columns.dataset
    with h5py.File(path, "r") as handle:
        _check_structure(handle, path, header, columns.table, declarations, n)
        halos = handle["halos"]
        for name in columns.table:
            expected = columns(name)
            _compare(halos[name], expected, block_rows, path, name)
            del expected
        if columns.identity_from_input:
            for name in _IDENTITY:
                if not np.array_equal(
                    np.asarray(halos[name][...], dtype=np.int64), _int64(dataset, snap, name)
                ):
                    raise ConverterError(
                        "{}: /halos/{} differs from the input's stored column".format(path, name)
                    )
        # links: the target row of every written link carries the MostBoundID
        # of the input's target row, through the target slab's permutation
        mbid_snap, mbid_target = None, None
        for name, companion, offset in sorted(LINKS, key=lambda link: link[2]):
            target = columns.window.target(offset)
            written = np.asarray(halos[name][...], dtype=np.int64)
            old = columns.old_link(name)
            null = old == -1
            if not np.array_equal(written == -1, null):
                raise ConverterError(
                    "{}: /halos/{} null rows differ from the input's".format(path, name)
                )
            if target is not None and (~null).any():
                if mbid_snap != target.snap:
                    mbid_target = _int64(dataset, target.snap, "MostBoundID")
                    mbid_snap = target.snap
                actual = mbid_target[target.perm[written[~null]]]
                wanted = mbid_target[old[~null]]
                if not np.array_equal(actual, wanted):
                    bad = np.flatnonzero(~null)[actual != wanted]
                    raise ConverterError(
                        "{}: {} /halos/{} link(s) whose target row does not carry the input "
                        "target's MostBoundID; {}".format(path, bad.size, name, _rows(path, bad))
                    )
                del actual, wanted
            if name == "FirstHaloInFOFgroup":
                rows = np.arange(n, dtype=np.int64)
                central_in = old == own.perm
                if not np.array_equal(written == rows, central_in):
                    raise ConverterError(
                        "{}: /halos/FirstHaloInFOFgroup self-references differ from the "
                        "input's".format(path)
                    )
            if companion is not None:
                held = np.asarray(halos[companion][...], dtype=np.int64)
                expected = np.where(null, -1, snap + offset)
                if not np.array_equal(held, expected):
                    raise ConverterError(
                        "{}: /halos/{} is not -1 iff {} is -1 and otherwise {}".format(
                            path, companion, name, snap + offset
                        )
                    )
            del written, old, null
        del mbid_target
        # identities and SnapNum
        snapnum = np.asarray(halos["SnapNum"][...], dtype=np.int64)
        if np.any(snapnum != snap):
            raise ConverterError("{}: /halos/SnapNum holds values other than {}".format(path, snap))
        forest = np.asarray(halos["ForestIndex"][...], dtype=np.int64)
        rank = np.asarray(halos["HaloRankInForest"][...], dtype=np.int64)
        sid = np.asarray(halos["SourceHaloID"][...], dtype=np.int64)
        if n > 1:
            if np.any(forest[1:] < forest[:-1]):
                raise ConverterError("{}: /halos/ForestIndex is not non-decreasing".format(path))
            same = forest[1:] == forest[:-1]
            if np.any(rank[1:][same] != rank[:-1][same] + 1):
                raise ConverterError(
                    "{}: /halos/HaloRankInForest is not dense within a forest's run".format(path)
                )
            if np.any(sid[1:] <= sid[:-1]):
                raise ConverterError(
                    "{}: /halos/SourceHaloID is not strictly ascending".format(path)
                )
        if np.any(sid != sid_base[forest] + rank):
            raise ConverterError(
                "{}: /halos/SourceHaloID is not the forest base plus the rank".format(path)
            )
        if (
            not np.array_equal(forest, own.forest)
            or not np.array_equal(rank, own.rank)
            or not np.array_equal(sid, own.sid)
        ):
            raise ConverterError("{}: identity columns differ from the remap store's".format(path))


# ---------------------------------------------------------------------------
# The sidecar
# ---------------------------------------------------------------------------


def _sidecar_blocks(sidecar: Mapping[str, np.ndarray], rows: int):
    n = int(sidecar["ForestID"].size)
    for start in range(0, n, rows):
        yield {
            name: np.ascontiguousarray(sidecar[name][start : start + rows], dtype="<i8")
            for name in V3_SIDECAR_DATASETS
        }


def write_sidecar(path, sidecar: Mapping[str, np.ndarray]) -> int:
    """``forests.h5`` through the writer's sidecar helper, in its block size,
    so the chunk shape is the converter's own."""
    return write_v3_sidecar(path, _sidecar_blocks(sidecar, V3_WRITE_BLOCK_ROWS))


def verify_sidecar(path, sidecar: Mapping[str, np.ndarray]) -> None:
    """Re-read ``forests.h5``: exactly the three int64 root datasets, the
    format's layout, and the expected values."""
    n = int(sidecar["ForestID"].size)
    with h5py.File(path, "r") as handle:
        if list(handle.keys()) != list(V3_SIDECAR_DATASETS) or handle.attrs.keys():
            raise ConverterError(
                "{}: holds {} (attributes {}), not exactly {}".format(
                    path, list(handle.keys()), sorted(handle.attrs.keys()), V3_SIDECAR_DATASETS
                )
            )
        _require_hard_links(handle, path)
        for name in V3_SIDECAR_DATASETS:
            _check_layout(handle[name], np.dtype("<i8"), False, n, path)
            if (
                handle[name][...].tobytes()
                != np.ascontiguousarray(sidecar[name], dtype="<i8").tobytes()
            ):
                raise ConverterError("{}: /{} differs from the expected values".format(path, name))
