"""Consumer cross-check for the ctrees -> snapshot-HDF5 converter.

Cross-checks a converted snapshot-HDF5 dataset (a directory of
``snapshot_NNN.h5`` files produced by the converter) against a Mimic
halos-only *reference run* galaxy output. The reference run is the ground
truth: Mimic reads the same trees through its own inheritance service
(src/core/inheritance.c) and emits one galaxy per occupied (sub)halo carrying
the frozen ``UniqueGalaxyID`` encoding. If the converter's links, identity
fields, flyby signs, and copied values are correct, the reference run's
galaxies must reproduce exactly from the converter's own halo arrays.

Reference identity encoding (frozen — src/include/galaxy_id.h:31,
TREE_MUL_FAC = 10**9)::

    UniqueGalaxyID = HaloRankInForest + multiplier * (ForestIndex + 1)

so ``forest = ugid // multiplier - 1`` and ``rank = ugid % multiplier``.

Reference galaxy output layout: per-chunk HDF5 files ``<base>_NNN.hdf5``
(e.g. ``halos_000.hdf5``), each with groups ``Snap%03d`` holding a structured
``Galaxies`` dataset. A master ``<base>.hdf5`` (no ``_<digits>`` suffix) may
carry external links to the chunks; it is IGNORED here — only the
``<base>_<digits>.hdf5`` chunks are read, sorted by numeric suffix, and
galaxies at a snapshot are concatenated across chunks in that order. A missing
``Snap%03d`` group means zero galaxies at that snapshot. Only Type 0/1
galaxies participate in matching; Type 2+ orphans are ignored entirely.

The six checks (frozen comparison rules — integers/links/signs exact,
float32 fields bit-exact, ``Mvir`` exact after documented arithmetic, never a
widened tolerance):

  1. identity-forest    every matched galaxy decodes to its halo's ForestIndex
  2. identity-creation  first-appearance galaxies decode to (ForestIndex, rank)
  3. fof-central        the ``UniqueCentralGalaxyID`` galaxy's |MostBoundID|
                        equals the converter FirstHaloInFOFgroup target's id
  4. flyby-signs        over matched Type 0/1 halos, the negative-MostBoundID
                        sets match both directions
  5. values             Pos/Vel/Spin/VelDisp/Vmax bit-exact, Len exact, Mvir
                        reconstructed via the reference get_virial_mass rule
                        (central+valid catalog mass -> float64(M_Crit200)*1e-10,
                        else Len*PartMass) and compared bit-for-bit
  6. occupancy          the matched-halo set equals the reference occupancy
                        predicate computed on the converter links by forward
                        induction, with zero unmatched reference galaxies

Matching is by ``|MostBoundID|``: the converter emits ascending-unique
|MostBoundID| per file (the slab order), so a galaxy is matched to a halo by
searchsorted. The ascending-unique invariant is re-asserted and any violation
aborts (ConverterError) — matching cannot be trusted otherwise.

Memory is bounded by the per-snapshot window rather than by the dataset
(converter scale pass, plan Slice 7). ``compare`` walks the snapshots in
ascending order, holding one snapshot's converter arrays and one snapshot's
reference galaxies at a time, and the two pieces of genuinely cross-snapshot
state are held exactly but outside memory: the set of ``UniqueGalaxyID``
values already seen (:class:`_SeenIdentities`, a disk-backed sorted union
merged one block at a time) and the reference-topology dump
(:class:`TopologyDumpPartition`, partitioned by snapshot on disk as it is
parsed). Neither is approximate and
neither is probabilistic. The whole-dataset formulation this replaced loaded
the emitted dataset, the whole reference output and the whole dump at once and
measured 251.32 GB on a 1.8% subset of Shin-Uchuu.

**This is a micro-Uchuu-scale gate, not a production-scale instrument.** The
reference side is a tree-ordered ``halos-only`` run over the same data, and the
ctrees reader preallocates per tree for a whole forest before reading a halo
(``src/io/tree/read_ctrees_ascii.c``), so the reference artifact cannot be
produced at Shin-Uchuu scale however bounded this comparator is. The binding
cross-check gate is micro-Uchuu, with the subset rehearsal as the largest scale
it is ever run at; no cross-check artifact belongs in a production conversion's
storage envelope.

Also provides reference-run plumbing (``write_reference_run_file`` /
``run_reference``) and a CLI (``compare`` / ``prepare`` / ``run-reference``).

An optional seventh check, ``topology-chains``, runs when ``compare`` is given
``--reference-topology <dump>``: it compares the converter against a reference
dump produced by an independent implementation reading the same source data
(see tests/unit/tools/dump_ctrees_topology.c and TopologyDumpPartition
below for the dump format). Per halo it checks the five links
(Descendant/FirstProgenitor/NextProgenitor/FirstHaloInFOFgroup/
NextHaloInFOFgroup), the two identity fields (ForestIndex/HaloRankInForest),
and the halo's own signed MostBoundID — after first asserting that the dump
names every converter halo exactly once at every snapshot, which is what makes
it a proof rather than a sample. The six checks above establish identity, rank
(on the lineage-creation subset), and central resolution; this seventh check is
the direct proof that chain ORDER — not just membership — matches, and it
extends rank conformance to every halo. Without ``--reference-topology`` the
six-check gate still runs on its own; chain order is then unproven, not merely
unreported.
"""

import argparse
import itertools
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import warnings
from pathlib import Path
from typing import BinaryIO, Dict, List, Tuple

import h5py
import numpy as np
import yaml

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from ctrees_parser import ConverterError  # noqa: E402
from fixups import NATIVE_TO_REF_MASS, REF_TO_NATIVE_MASS, load_particle_mass  # noqa: E402
from hdf5_writer import snapshot_h5_name  # noqa: E402
from scatter import load_a_list  # noqa: E402

# _Snapshots is validate.py's bounded reader for the emitted dataset: one file
# opened per access, only the named /halos datasets read. The cross-check reads
# the same files the battery does, so it reads them through the same
# implementation rather than a second copy of it; publishing a non-underscore
# alias for it would mean editing validate.py, which is outside this slice's
# authorized surface (converter scale pass, plan Slice 7).
from validate import DEFAULT_MULTIPLIER, Outcome, _Snapshots  # noqa: E402

#: Reference Galaxies fields the cross-check consumes: name -> (exact dtype,
#: subshape). All must be present with these EXACT base dtypes (extras are
#: tolerated/ignored). Widths are enforced, not just kinds: a float64 field
#: where float32 is required would let a wrong reference value round back to
#: the converter's float32 bits and defeat the bit-exact comparison.
REQUIRED_FIELDS = {
    "SnapNum": (np.dtype(np.int32), ()),
    "Type": (np.dtype(np.int32), ()),
    "UniqueGalaxyID": (np.dtype(np.int64), ()),
    "UniqueCentralGalaxyID": (np.dtype(np.int64), ()),
    "Len": (np.dtype(np.int32), ()),
    "Mvir": (np.dtype(np.float64), ()),
    "Pos": (np.dtype(np.float32), (3,)),
    "Vel": (np.dtype(np.float32), (3,)),
    "Spin": (np.dtype(np.float32), (3,)),
    "VelDisp": (np.dtype(np.float32), ()),
    "Vmax": (np.dtype(np.float32), ()),
    "MostBoundID": (np.dtype(np.int64), ()),
}

_INT64_MIN = np.iinfo(np.int64).min

#: Bit-exact float32 value fields compared over matched pairs.
_VEC3_FIELDS = ("Pos", "Vel", "Spin")
_SCALAR_F32_FIELDS = ("VelDisp", "Vmax")

#: Converter /halos datasets the seven per-snapshot checks read. Naming them
#: keeps the window to what the checks actually consume rather than to every
#: emitted field.
_MATCH_FIELDS = (
    "MostBoundID",
    "ForestIndex",
    "HaloRankInForest",
    "FirstHaloInFOFgroup",
    "Descendant",
    "Len",
    "M_Crit200",
    "Pos",
    "Vel",
    "Spin",
    "VelDisp",
    "Vmax",
)

#: Converter /halos datasets the topology-chains check reads: the two identity
#: fields, the five links, and the halo's own signed id.
_TOPOLOGY_FIELDS = (
    "MostBoundID",
    "ForestIndex",
    "HaloRankInForest",
    "Descendant",
    "FirstProgenitor",
    "NextProgenitor",
    "FirstHaloInFOFgroup",
    "NextHaloInFOFgroup",
)

#: Identities read or written at a time while merging the seen-identity store.
#: Bounds a buffer, never a total: three of these (the block read, the window
#: slice merged into it, and the merged output) are live at once, so 1 Mi
#: identities is tens of MB.
_SEEN_BLOCK_ROWS = 1 << 20

#: Dump text lines parsed at a time. Bounds the Python string list and the
#: parsed block, not the dump.
_DUMP_BLOCK_ROWS = 1 << 16

#: Prefixes of the two private spill directories, created under the system
#: temporary directory (so TMPDIR places them) and removed on the success,
#: failure and exception paths alike.
SEEN_DIR_PREFIX = "crosscheck_seen_"
TOPOLOGY_DIR_PREFIX = "crosscheck_topology_"


def _log(message: str) -> None:
    print(message, file=sys.stderr)


def _examples(values, limit: int = 5) -> str:
    return ", ".join(str(v) for v in list(values)[:limit])


def decode_forest(ugid, multiplier: int):
    """Reference forest index encoded in a UniqueGalaxyID."""
    return ugid // multiplier - 1


def decode_rank(ugid, multiplier: int):
    """Reference HaloRankInForest encoded in a UniqueGalaxyID."""
    return ugid % multiplier


# ---------------------------------------------------------------------------
# Reference-run galaxy loading
# ---------------------------------------------------------------------------


def _validate_reference_dtype(dtype: np.dtype, path: Path) -> None:
    """Every required Galaxies field must be present with the EXACT base dtype
    and subarray shape; anything missing or mistyped aborts (width coercion is
    a bit-exactness hazard, never tolerated)."""
    missing = []
    wrong = []
    for name, (expected, subshape) in REQUIRED_FIELDS.items():
        if dtype.names is None or name not in dtype.names:
            missing.append(name)
            continue
        field_dtype = dtype.fields[name][0]
        if field_dtype.subdtype is not None:
            base, shape = field_dtype.subdtype
        else:
            base, shape = field_dtype, ()
        if base != expected or tuple(shape) != subshape:
            wrong.append(
                "{} ({}{} != required {}{})".format(
                    name, base.str, tuple(shape), expected.str, subshape
                )
            )
    if missing or wrong:
        raise ConverterError(
            "{}: reference Galaxies dtype is missing field(s) {} and mistypes {}".format(
                path, missing, wrong
            )
        )


class ReferenceGalaxies:
    """Bounded per-snapshot access to a reference-run galaxy output.

    Galaxies live in the ``<base>_<digits>.hdf5`` chunk files; the master
    ``<base>.hdf5`` is ignored. A snapshot's galaxies are the concatenation of
    its ``Snap%03d/Galaxies`` datasets across chunks in ascending numeric-suffix
    order, and a missing group contributes zero galaxies — exactly what the
    whole-output loader this replaced produced, one snapshot at a time.

    Construction scans the chunks' metadata only (no galaxy is read): it
    validates every ``Snap###/Galaxies`` dtype in the same chunk-major order
    the whole-output loader validated in, so a missing field, a wrong field
    width, a chunk whose dtype differs from the first, or an output carrying no
    Galaxies dataset at all still aborts before any check runs. What the scan
    keeps is one path list per snapshot, which is bounded by the chunk count
    times the snapshot count and not by the galaxies.
    """

    def __init__(self, reference_dir, base: str, n_snapshots: int):
        reference_dir = Path(reference_dir)
        if not reference_dir.is_dir():
            raise ConverterError("{}: not a directory".format(reference_dir))
        self.directory = reference_dir
        self.n_snapshots = int(n_snapshots)
        pattern = re.compile(r"^" + re.escape(base) + r"_(\d+)\.hdf5$")
        chunks = []
        for path in reference_dir.iterdir():
            match = pattern.match(path.name)
            if match:
                chunks.append((int(match.group(1)), path))
        chunks.sort()
        if not chunks:
            raise ConverterError(
                "{}: no reference chunk files {}_<digits>.hdf5 found".format(reference_dir, base)
            )

        self.dtype = None
        self._paths_by_snap: Dict[int, List[Path]] = {snap: [] for snap in range(self.n_snapshots)}
        for _, path in chunks:
            with h5py.File(path, "r") as handle:
                for snap in range(self.n_snapshots):
                    group_name = "Snap{:03d}".format(snap)
                    if group_name not in handle:
                        continue
                    group = handle[group_name]
                    if "Galaxies" not in group:
                        continue
                    # every dataset in every chunk is validated (a later chunk
                    # with a wider dtype would silently promote the
                    # concatenated array and defeat bit-exact comparison)
                    self._require_dtype(group["Galaxies"].dtype, path, snap)
                    self._paths_by_snap[snap].append(path)
        if self.dtype is None:
            raise ConverterError(
                "{}: reference chunks carry no Snap###/Galaxies datasets".format(reference_dir)
            )

    def _require_dtype(self, dtype: np.dtype, path: Path, snap: int) -> None:
        """Validate one Galaxies dtype and bind the run's single dtype to the
        first one seen."""
        _validate_reference_dtype(dtype, path)
        if self.dtype is None:
            self.dtype = dtype
        elif dtype != self.dtype:
            raise ConverterError(
                "{}: Snap{:03d}/Galaxies dtype {} differs from the first chunk's "
                "dtype {} — reference chunks must share one structured dtype".format(
                    path, snap, dtype, self.dtype
                )
            )

    def load(self, snap: int) -> np.ndarray:
        """One snapshot's reference galaxies, concatenated across the chunks
        that carry it in ascending numeric-suffix order."""
        parts = []
        for path in self._paths_by_snap[snap]:
            with h5py.File(path, "r") as handle:
                data = handle["Snap{:03d}".format(snap)]["Galaxies"][...]
            # re-asserted at read time: the scan validated the file's metadata,
            # and this is the array the checks actually compare
            self._require_dtype(data.dtype, path, snap)
            parts.append(data)
        if not parts:
            return np.empty(0, dtype=self.dtype)
        if len(parts) == 1:
            return parts[0]
        return np.concatenate(parts)


# ---------------------------------------------------------------------------
# Per-snapshot matching
# ---------------------------------------------------------------------------


class SnapMatch:
    """One snapshot's reference galaxies matched to converter halos.

    ``matched[k]`` is the converter halo index for the k-th Type 0/1 galaxy
    (``t01_idx[k]`` into ``ref``), or -1 if no converter halo shares its
    ``|MostBoundID|``.
    """

    def __init__(self, snap, ref, t01_idx, conv, matched):
        self.snap = snap
        self.ref = ref
        self.t01_idx = t01_idx
        self.conv = conv
        self.matched = matched


def build_match(conv, ref, snap: int) -> SnapMatch:
    """Match one snapshot's reference Type 0/1 galaxies to converter halos by
    ``|MostBoundID|``, re-asserting the converter's ascending-unique
    |MostBoundID| slab order first (ConverterError if not).

    ``conv`` is one snapshot's converter arrays and ``ref`` that snapshot's
    reference galaxies; the whole-dataset formulation this replaced built the
    same object for every snapshot up front and kept them all.
    """
    sentinel = conv["MostBoundID"] == _INT64_MIN
    if sentinel.any():
        raise ConverterError(
            "{}: {} converter MostBoundID value(s) equal INT64_MIN, whose magnitude "
            "overflows signed int64 — matching by |MostBoundID| cannot be trusted; "
            "example rows: {}".format(
                snapshot_h5_name(snap),
                int(sentinel.sum()),
                _examples(np.nonzero(sentinel)[0].tolist()),
            )
        )
    conv_abs = np.abs(conv["MostBoundID"])
    if conv_abs.size > 1 and not (conv_abs[1:] > conv_abs[:-1]).all():
        rows = np.nonzero(conv_abs[1:] <= conv_abs[:-1])[0][:5]
        raise ConverterError(
            "{}: converter |MostBoundID| is not strictly ascending/unique; example "
            "rows: {}".format(snapshot_h5_name(snap), _examples(rows.tolist()))
        )
    types = ref["Type"]
    t01_idx = np.nonzero((types == 0) | (types == 1))[0]
    ref_sentinel = ref["MostBoundID"][t01_idx] == _INT64_MIN
    if ref_sentinel.any():
        raise ConverterError(
            "snapshot {}: {} reference Type 0/1 MostBoundID value(s) equal INT64_MIN, "
            "whose magnitude overflows signed int64 — matching by |MostBoundID| cannot "
            "be trusted".format(snap, int(ref_sentinel.sum()))
        )
    ref_abs = np.abs(ref["MostBoundID"][t01_idx])
    matched = np.full(ref_abs.shape, -1, dtype=np.int64)
    if conv_abs.size and ref_abs.size:
        pos = np.clip(np.searchsorted(conv_abs, ref_abs), 0, conv_abs.size - 1)
        hit = conv_abs[pos] == ref_abs
        matched[hit] = pos[hit]
    return SnapMatch(snap, ref, t01_idx, conv, matched)


def _matched_pairs(match: SnapMatch):
    """(galaxy indices into ref, converter halo indices) for matched pairs."""
    mask = match.matched >= 0
    return match.t01_idx[mask], match.matched[mask]


# ---------------------------------------------------------------------------
# The six checks (+ reference sanity)
# ---------------------------------------------------------------------------


def check_reference_sanity(match) -> List[str]:
    """Reference data must be internally consistent before it can be a ground
    truth: SnapNum matches the group, no INT64_MIN MostBoundID (its magnitude
    overflows signed int64), |MostBoundID| is unique over Type 0/1, and
    UniqueGalaxyID is unique over ALL Type 0/1 galaxies (it is a run-scoped
    persistent identity — a duplicate on a satellite would otherwise slip
    past identity-creation once the id had been seen)."""
    failures = []
    ref = match.ref
    bad_snap = ref["SnapNum"] != match.snap
    if bad_snap.any():
        failures.append(
            "snapshot {}: {} galaxy(ies) with SnapNum != {}; examples: {}".format(
                match.snap,
                int(bad_snap.sum()),
                match.snap,
                _examples(ref["SnapNum"][bad_snap].tolist()),
            )
        )
    t01_mb = ref["MostBoundID"][match.t01_idx]
    sentinel = t01_mb == _INT64_MIN
    if sentinel.any():
        failures.append(
            "snapshot {}: {} Type 0/1 galaxy(ies) with MostBoundID == INT64_MIN, whose "
            "magnitude overflows signed int64".format(match.snap, int(sentinel.sum()))
        )
    t01_abs = np.abs(t01_mb)
    unique, counts = np.unique(t01_abs, return_counts=True)
    dup = unique[counts > 1]
    if dup.size:
        failures.append(
            "snapshot {}: duplicate |MostBoundID| among Type 0/1 galaxies; examples: "
            "{}".format(match.snap, _examples(dup.tolist()))
        )
    unique, counts = np.unique(ref["UniqueGalaxyID"][match.t01_idx], return_counts=True)
    dup = unique[counts > 1]
    if dup.size:
        failures.append(
            "snapshot {}: duplicate UniqueGalaxyID among Type 0/1 galaxies; examples: "
            "{}".format(match.snap, _examples(dup.tolist()))
        )
    return failures


def check_identity_forest(match, multiplier) -> List[str]:
    """Every matched galaxy's decoded forest equals its halo's ForestIndex."""
    failures = []
    gal_idx, conv_idx = _matched_pairs(match)
    if gal_idx.size == 0:
        return failures
    ugid = match.ref["UniqueGalaxyID"][gal_idx]
    bad = decode_forest(ugid, multiplier) != match.conv["ForestIndex"][conv_idx]
    if bad.any():
        ids = np.abs(match.ref["MostBoundID"][gal_idx[bad]])
        failures.append(
            "snapshot {}: {} matched galaxy(ies) whose decoded forest != halo "
            "ForestIndex; example ctrees ids: {}".format(
                match.snap, int(bad.sum()), _examples(ids.tolist())
            )
        )
    return failures


class _SeenIdentities:
    """Exact, bounded membership over the UniqueGalaxyIDs seen so far.

    ``check_identity_creation`` decodes only identities appearing for the FIRST
    time, because a galaxy that persists across snapshots legitimately keeps
    its UniqueGalaxyID and re-checking it would be noise. The whole-dataset
    formulation held that suppression set as an in-memory sorted int64 array
    grown with ``np.union1d`` — one entry per distinct identity in the run,
    which a one-snapshot window does not bound.

    This is the same union, externalised. The store is a single binary file of
    sorted, unique int64 identities; a snapshot's membership test streams the
    file one block at a time, merge-joins each block against the slice of the
    snapshot's own sorted identities that falls inside it, and writes the
    merged block straight out to the successor file. Nothing probabilistic and
    nothing approximate is involved: the answer is the same set membership
    ``np.isin`` computed, and the successor file is the same set ``np.union1d``
    produced.

    Resident bytes are one block, the window's own identities, and the merge's
    temporaries. On-disk bytes are the store plus its successor while a merge
    is in flight; ``peak_bytes`` reports the largest that pair ever reached.
    """

    def __init__(self, directory=None):
        self._dir = Path(tempfile.mkdtemp(prefix=SEEN_DIR_PREFIX, dir=directory))
        self._path = self._dir / "seen.i8"
        self._next_path = self._dir / "seen.next.i8"
        self.peak_bytes = 0
        try:
            self._path.write_bytes(b"")
        except BaseException:
            # the constructor owns the directory until it returns; there is no
            # ``with`` to fall back on if it raises here
            self.close()
            raise

    def __enter__(self) -> "_SeenIdentities":
        return self

    def __exit__(self, exc_type, exc, tb) -> bool:
        self.close()
        return False

    def close(self) -> None:
        """Remove the store. Three paths reach it: the constructor's own
        failure above, ``__exit__`` on the success path, and ``__exit__`` when
        a check — or anything else inside the ``with`` — raises.
        ``ignore_errors`` keeps a cleanup problem from masking an error that is
        already propagating."""
        shutil.rmtree(self._dir, ignore_errors=True)

    def first_appearance(self, ugid: np.ndarray) -> np.ndarray:
        """Which of ``ugid``'s entries have NOT been seen at an earlier call,
        as a boolean mask in ``ugid``'s own order, folding ``ugid`` into the
        store afterwards.

        The mask answers the same question as ``~np.isin(ugid, seen)`` against
        the union of every earlier call's identities: identities repeated
        WITHIN this call are all reported unseen, exactly as the whole-dataset
        formulation did, because the store is only updated once the mask is
        complete.
        """
        order = np.argsort(ugid, kind="stable")
        sorted_ugid = ugid[order]
        unseen_sorted = np.ones(sorted_ugid.size, dtype=bool)
        cursor = 0
        with open(self._path, "rb") as source, open(self._next_path, "wb") as target:
            while True:
                block = np.fromfile(source, dtype=np.int64, count=_SEEN_BLOCK_ROWS)
                if block.size == 0:
                    break
                # every identity at or below this block's last value is decided
                # by this block: the store is sorted, so no later block can
                # hold one of them
                stop = int(np.searchsorted(sorted_ugid, block[-1], side="right"))
                chunk = sorted_ugid[cursor:stop]
                if chunk.size:
                    pos = np.searchsorted(block, chunk)
                    unseen_sorted[cursor:stop] = block[np.minimum(pos, block.size - 1)] != chunk
                np.union1d(block, chunk).tofile(target)
                cursor = stop
            remainder = sorted_ugid[cursor:]
            if remainder.size:
                # past the store's last value, so unseen, and appended in sorted
                # order after the merged blocks
                np.unique(remainder).tofile(target)
        self.peak_bytes = max(
            self.peak_bytes, self._path.stat().st_size + self._next_path.stat().st_size
        )
        os.replace(self._next_path, self._path)
        unseen = np.empty(ugid.size, dtype=bool)
        unseen[order] = unseen_sorted
        return unseen


def check_identity_creation(match, multiplier, seen: _SeenIdentities) -> List[str]:
    """Each galaxy whose UniqueGalaxyID first appears at this snapshot must
    decode to its matched halo's (ForestIndex, rank).

    Snapshots must be presented in ascending order, as the whole-dataset
    formulation processed them: ``seen`` carries the suppression set across
    calls (see :class:`_SeenIdentities`), and an identity already in it is
    skipped — a galaxy that persists across snapshots keeps its identity, and
    re-checking it would be noise rather than signal. A galaxy that is
    unmatched is skipped too: that is an occupancy failure, not an identity
    failure.
    """
    failures = []
    t01_ugid = np.asarray(match.ref["UniqueGalaxyID"][match.t01_idx], dtype=np.int64)
    if t01_ugid.size == 0:
        return failures
    candidate = seen.first_appearance(t01_ugid) & (match.matched >= 0)
    conv_i = match.matched[candidate]
    ugids = t01_ugid[candidate]
    bad = (decode_forest(ugids, multiplier) != match.conv["ForestIndex"][conv_i]) | (
        decode_rank(ugids, multiplier) != match.conv["HaloRankInForest"][conv_i]
    )
    if bad.any():
        ids = np.abs(match.ref["MostBoundID"][match.t01_idx[candidate][bad]])
        failures.append(
            "snapshot {}: {} newly-created galaxy(ies) whose decoded (forest, rank) != "
            "halo (ForestIndex, HaloRankInForest); example ctrees ids: {}".format(
                match.snap, int(bad.sum()), _examples(ids.tolist())
            )
        )
    return failures


def check_fof_central(match) -> List[str]:
    """The Type 0 galaxy named by each matched galaxy's UniqueCentralGalaxyID
    must exist and share |MostBoundID| with the converter FoF-central target.

    The Type 0 UniqueGalaxyID -> |MostBoundID| lookup is a sorted-array
    searchsorted rather than a Python dict (real micro-Uchuu scale; see
    check_identity_creation). Duplicate Type 0 UniqueGalaxyIDs are a
    reference-sanity failure and resolve here to their first occurrence.
    """
    failures = []
    ref = match.ref
    gal_idx, conv_idx = _matched_pairs(match)
    if gal_idx.size == 0:
        return failures
    t0_idx = np.nonzero(ref["Type"] == 0)[0]
    t0_ugid = np.asarray(ref["UniqueGalaxyID"][t0_idx], dtype=np.int64)
    order = np.argsort(t0_ugid, kind="stable")
    t0_ugid_sorted = t0_ugid[order]
    t0_absmb_sorted = np.abs(ref["MostBoundID"][t0_idx[order]])
    ids = np.abs(ref["MostBoundID"][gal_idx])

    central_ugid = np.asarray(ref["UniqueCentralGalaxyID"][gal_idx], dtype=np.int64)
    pos = np.searchsorted(t0_ugid_sorted, central_ugid)
    if t0_ugid_sorted.size:
        clipped = np.minimum(pos, t0_ugid_sorted.size - 1)
        found = t0_ugid_sorted[clipped] == central_ugid
    else:
        clipped = np.zeros(central_ugid.size, dtype=np.int64)
        found = np.zeros(central_ugid.size, dtype=bool)
    if (~found).any():
        failures.append(
            "snapshot {}: {} galaxy(ies) whose UniqueCentralGalaxyID names no Type 0 "
            "galaxy; example ctrees ids: {}".format(
                match.snap, int((~found).sum()), _examples(ids[~found].tolist())
            )
        )
    fof_target = match.conv["FirstHaloInFOFgroup"][conv_idx[found]]
    expected = np.abs(match.conv["MostBoundID"][fof_target])
    mismatch = t0_absmb_sorted[clipped[found]] != expected
    if mismatch.any():
        failures.append(
            "snapshot {}: {} galaxy(ies) whose FoF-central |MostBoundID| != converter "
            "FirstHaloInFOFgroup target; example ctrees ids: {}".format(
                match.snap, int(mismatch.sum()), _examples(ids[found][mismatch].tolist())
            )
        )
    return failures


def check_flyby_signs(match) -> List[str]:
    """Over the matched Type 0/1 population, the set of negative MostBoundID
    values from the reference galaxies must equal the set from their matched
    converter halos (both ways). Only matched halos are compared: a correctly
    flyby-demoted halo that seeds no galaxy — a would-be FoF central demoted to
    a satellite whose lineage was never occupied, so the reference model never
    creates a galaxy for it — has no reference counterpart to compare against.
    Such a halo's flyby sign is instead re-covered by the topology-chains check
    (which resolves signed link targets against the reference dump) when a
    reference-topology dump is supplied; without that dump an unmatched halo's
    sign is not independently re-verified here (occupancy matches on
    ``|MostBoundID|`` and does not inspect sign)."""
    failures = []
    gal_idx, conv_idx = _matched_pairs(match)
    ref_neg = {int(v) for v in match.ref["MostBoundID"][gal_idx] if v < 0}
    conv_neg = {int(v) for v in match.conv["MostBoundID"][conv_idx] if v < 0}
    only_ref = ref_neg - conv_neg
    only_conv = conv_neg - ref_neg
    if only_ref or only_conv:
        failures.append(
            "snapshot {}: negative-MostBoundID set mismatch ({} only in reference, {} "
            "only in converter); examples: {}".format(
                match.snap,
                len(only_ref),
                len(only_conv),
                _examples(sorted(only_ref | only_conv)),
            )
        )
    return failures


def _u32(values) -> np.ndarray:
    """Raw uint32 view of a float32 array. The input dtype is ASSERTED, never
    coerced: silently casting a float64 operand to float32 here would round a
    wrong reference value back onto the converter's bits and defeat the
    bit-exact comparison."""
    arr = np.ascontiguousarray(values)
    if arr.dtype != np.float32:
        raise ConverterError(
            "bit-exact comparison expected float32 data, got {} — refusing to coerce".format(
                arr.dtype
            )
        )
    return arr.view(np.uint32)


def _u64(values) -> np.ndarray:
    """Raw uint64 view of a float64 array (dtype asserted, never coerced)."""
    arr = np.ascontiguousarray(values)
    if arr.dtype != np.float64:
        raise ConverterError(
            "bit-exact comparison expected float64 data, got {} — refusing to coerce".format(
                arr.dtype
            )
        )
    return arr.view(np.uint64)


def check_values(match, part_mass) -> List[str]:
    """Bit-exact value comparison over matched pairs (frozen rules): float32
    fields via uint32 views (NaN payloads and signed zeros count), Len exact,
    and Mvir reconstructed via the reference get_virial_mass rule (a FoF central
    with a valid catalog mass -> ``float64(M_Crit200) * 1e-10``; every other
    halo -> ``Len * part_mass``) compared bit-for-bit."""
    if not part_mass > 0.0:
        raise ConverterError(
            "particle mass must be positive to reconstruct satellite Mvir "
            "(Len * PartMass); got {}".format(part_mass)
        )
    failures = []
    gal_idx, conv_idx = _matched_pairs(match)
    if gal_idx.size == 0:
        return failures
    ref_sub = match.ref[gal_idx]
    ids = np.abs(ref_sub["MostBoundID"])

    def report(field, bad):
        if bad.any():
            failures.append(
                "snapshot {}: {} matched pair(s) with mismatched {}; example ctrees ids: "
                "{}".format(match.snap, int(bad.sum()), field, _examples(ids[bad].tolist()))
            )

    for field in _VEC3_FIELDS:
        bad = (_u32(ref_sub[field]) != _u32(match.conv[field][conv_idx])).any(axis=1)
        report(field, bad)
    for field in _SCALAR_F32_FIELDS:
        report(field, _u32(ref_sub[field]) != _u32(match.conv[field][conv_idx]))
    report("Len", ref_sub["Len"].astype(np.int64) != match.conv["Len"][conv_idx].astype(np.int64))
    # Reference galaxy Mvir is model-derived, not a raw catalog copy
    # (src/core/virial.c get_virial_mass): a FoF central with a valid
    # (non-negative) spherical-overdensity mass takes M_Crit200 widened to
    # float64 and scaled to the reference mass unit in float64; every other
    # halo — satellites, and any central without a valid catalog mass —
    # takes Len * PartMass. The converter carries the raw catalog M_Crit200,
    # so the expected galaxy Mvir is reconstructed here. The scale factor is
    # NATIVE_TO_REF_MASS — the single converter-side definition shared with
    # the Len derivation and matching the generated accessor's baked-in
    # conversion — not a re-typed literal. The operand dtype is asserted
    # before the deliberate widening.
    m200 = np.ascontiguousarray(match.conv["M_Crit200"][conv_idx])
    if m200.dtype != np.float32:
        raise ConverterError(
            "converter M_Crit200 must be float32, got {} — refusing to coerce".format(m200.dtype)
        )
    halo_mass = m200.astype(np.float64) * NATIVE_TO_REF_MASS
    len_mass = match.conv["Len"][conv_idx].astype(np.float64) * part_mass
    is_central = match.conv["FirstHaloInFOFgroup"][conv_idx] == conv_idx
    expected = np.where(is_central & (halo_mass >= 0.0), halo_mass, len_mass)
    report("Mvir", _u64(ref_sub["Mvir"]) != _u64(expected))
    return failures


def check_occupancy(match, forwarded_prev) -> Tuple[List[str], np.ndarray]:
    """The matched-halo set at this snapshot must equal the reference
    occupancy predicate on the converter links, computed by forward induction
    (``occupied(H) = FoF-central(H) OR any occupied progenitor``), and no
    reference Type 0/1 galaxy may be unmatched.

    ``forwarded_prev`` is what the previous snapshot's call returned: the
    Descendant indices of its occupied halos, with the no-descendant sentinel
    already dropped. The whole-dataset formulation carried the previous
    snapshot's occupancy mask and re-read its Descendant array here; forwarding
    the resolved indices instead is the same induction with the previous
    snapshot released. ``None`` starts the induction at snapshot 0.

    Returns the failures and the indices to forward to the next snapshot.
    """
    failures = []
    conv = match.conv
    n = conv["MostBoundID"].size
    is_central = conv["FirstHaloInFOFgroup"] == np.arange(n)
    occupied = is_central.copy()
    if forwarded_prev is not None:
        occupied[forwarded_prev] = True

    matched_mask = np.zeros(n, dtype=bool)
    matched_mask[match.matched[match.matched >= 0]] = True
    only_pred = occupied & ~matched_mask
    only_match = matched_mask & ~occupied
    if only_pred.any():
        ids = np.abs(conv["MostBoundID"][only_pred])
        failures.append(
            "snapshot {}: {} predicted-occupied halo(s) with no matching reference "
            "galaxy; example ctrees ids: {}".format(
                match.snap, int(only_pred.sum()), _examples(ids.tolist())
            )
        )
    if only_match.any():
        ids = np.abs(conv["MostBoundID"][only_match])
        failures.append(
            "snapshot {}: {} matched halo(s) predicted unoccupied; example ctrees ids: "
            "{}".format(match.snap, int(only_match.sum()), _examples(ids.tolist()))
        )
    unmatched = match.matched < 0
    if unmatched.any():
        ids = np.abs(match.ref["MostBoundID"][match.t01_idx[unmatched]])
        failures.append(
            "snapshot {}: {} reference Type 0/1 galaxy(ies) with no matching converter "
            "halo; example ctrees ids: {}".format(
                match.snap, int(unmatched.sum()), _examples(ids.tolist())
            )
        )
    forwarded = conv["Descendant"][occupied]
    return failures, forwarded[forwarded != -1]


# ---------------------------------------------------------------------------
# Optional seventh check: direct chain-order comparison against an
# independent reference-topology dump
# ---------------------------------------------------------------------------

#: Dump row layout, in column order (tests/unit/tools/dump_ctrees_topology.c):
#: forestnr rank id snapnum desc_id first_prog_id next_prog_id first_fof_id next_fof_id.
#: All fields are int64 in the dump; ``_INT64_MIN`` marks "no link".
_TOPOLOGY_DUMP_DTYPE = np.dtype(
    [
        ("ForestIndex", np.int64),
        ("HaloRankInForest", np.int64),
        ("MostBoundID", np.int64),
        ("SnapNum", np.int64),
        ("Descendant", np.int64),
        ("FirstProgenitor", np.int64),
        ("NextProgenitor", np.int64),
        ("FirstHaloInFOFgroup", np.int64),
        ("NextHaloInFOFgroup", np.int64),
    ]
)

_TOPOLOGY_DUMP_HEADER = "# mimic-topology-dump v1"


class TopologyDumpPartition:
    """A reference-topology dump, parsed once and partitioned by snapshot.

    The dump is a fixed three-line header (format marker, column names,
    NA-sentinel value) followed by one whitespace-separated row per halo, in the
    column order of ``_TOPOLOGY_DUMP_DTYPE``, and nothing else. Raises
    ConverterError on a header mismatch, a ragged or non-integer row, or a
    comment line after the header — this reader and the harness that writes the
    dump must agree on the format exactly, and silent field-order drift or a
    silently spliced second dump would defeat every comparison below without
    ever failing loudly.

    Rows arrive in the harness's own order — forest by forest, snapshots
    interleaved — while the check consumes them one snapshot at a time, so the
    rows are written into one binary file per snapshot as they are parsed,
    keeping each snapshot's rows in dump order. The whole-dump formulation this
    replaced read the file with a single ``np.loadtxt`` and then built a global
    sort permutation over it: 42 GB of text became a 229.5 GB transient at
    rehearsal scale. Here the resident cost is one block of text lines and its
    parsed block; the partition itself lives on disk and is removed by
    :meth:`close`.

    ``counts`` is the row count per in-dataset snapshot, ``out_of_range`` maps
    each snapshot value outside ``[0, n_snapshots)`` to its row count (empty for
    any conformant dump, and never larger than the number of failure lines the
    check must emit for those values), and ``peak_bytes`` is the partition's
    size on disk.
    """

    def __init__(self, dump_path, n_snapshots: int, directory=None):
        self.path = Path(dump_path)
        self.n_snapshots = int(n_snapshots)
        self.counts = np.zeros(self.n_snapshots, dtype=np.int64)
        self.out_of_range: Dict[int, int] = {}
        self.total_rows = 0
        self.peak_bytes = 0
        self._dir = Path(tempfile.mkdtemp(prefix=TOPOLOGY_DIR_PREFIX, dir=directory))
        self._handles: Dict[int, BinaryIO] = {}
        try:
            self._load()
        except BaseException:
            # the constructor owns the directory until it returns, so a
            # malformed dump — or anything else raised while parsing — must not
            # leave it behind
            self.close()
            raise

    def __enter__(self) -> "TopologyDumpPartition":
        return self

    def __exit__(self, exc_type, exc, tb) -> bool:
        self.close()
        return False

    def close(self) -> None:
        """Close any open partition file and remove the partition directory.
        Reached from the constructor's own failure path, from ``__exit__`` on
        the success path, and from ``__exit__`` when the check or anything else
        inside the ``with`` raises; ``ignore_errors`` keeps a cleanup problem
        from masking an error already propagating."""
        for handle in self._handles.values():
            handle.close()
        self._handles = {}
        shutil.rmtree(self._dir, ignore_errors=True)

    def _partition_path(self, snap: int) -> Path:
        return self._dir / "snap_{:04d}.bin".format(snap)

    def _handle(self, snap: int):
        """The open writer for one snapshot's partition file. At most one per
        in-dataset snapshot is open, so this is bounded by the snapshot count
        and not by the dump."""
        if snap not in self._handles:
            self._handles[snap] = open(self._partition_path(snap), "wb")
        return self._handles[snap]

    def _load(self) -> None:
        with open(self.path) as handle:
            header = [handle.readline().rstrip("\n") for _ in range(3)]
            if header[0] != _TOPOLOGY_DUMP_HEADER:
                raise ConverterError(
                    "{}: not a recognised reference-topology dump (expected first line "
                    "{!r})".format(self.path, _TOPOLOGY_DUMP_HEADER)
                )
            expected_sentinel = "# NA sentinel = {} (no link)".format(_INT64_MIN)
            if header[2] != expected_sentinel:
                raise ConverterError(
                    "{}: NA sentinel line {!r} != expected {!r}".format(
                        self.path, header[2], expected_sentinel
                    )
                )
            # ``readline`` past EOF returns "" — a short file therefore fails
            # the header/sentinel checks above rather than raising.
            offset = 0
            while True:
                lines = list(itertools.islice(handle, _DUMP_BLOCK_ROWS))
                if not lines:
                    break
                self._append(self._parse(lines, offset))
                offset += len(lines)
        for target in self._handles.values():
            target.close()
        self._handles = {}
        self.peak_bytes = sum(
            self._partition_path(snap).stat().st_size
            for snap in range(self.n_snapshots)
            if self.counts[snap]
        )

    def _parse(self, lines, offset: int) -> np.ndarray:
        """One block of dump text lines as typed rows.

        ``comments=None`` is deliberate: the format is exactly three header
        lines followed by data rows, so a "#" line after the header means a
        malformed dump (two runs concatenated, a harness re-run appended with
        ">>"). Letting np.loadtxt skip such lines would silently splice
        unrelated dumps into one array. A ragged row, a non-integer field, or a
        stray comment raises ValueError, remapped to ConverterError so the
        reader keeps a single loud failure mode.
        """
        try:
            with warnings.catch_warnings():
                # a block of blank lines parses to zero rows here; it is
                # check_topology_chains' coverage assertion, not this reader,
                # that rejects a dump with no rows against the halo counts
                warnings.simplefilter("ignore", category=UserWarning)
                return np.loadtxt(lines, dtype=_TOPOLOGY_DUMP_DTYPE, comments=None, ndmin=1)
        except ValueError as exc:
            # numpy counts rows within the block it was handed, so the number
            # it reports is absolute only for the first block; later blocks say
            # where their numbering starts rather than quietly shifting it
            detail = "{}: malformed data row ({})".format(self.path, exc)
            if offset:
                detail += (
                    " — the row number is relative to the block of data rows "
                    "starting at row {}".format(offset)
                )
            raise ConverterError(detail)

    def _append(self, block: np.ndarray) -> None:
        """Write one parsed block into its snapshots' partition files, keeping
        dump order within each snapshot."""
        if block.size == 0:
            return
        self.total_rows += int(block.size)
        snaps = block["SnapNum"]
        in_range = (snaps >= 0) & (snaps < self.n_snapshots)
        if not in_range.all():
            values, counts = np.unique(snaps[~in_range], return_counts=True)
            for value, count in zip(values.tolist(), counts.tolist()):
                self.out_of_range[value] = self.out_of_range.get(value, 0) + count
        rows = block[in_range]
        if rows.size == 0:
            return
        # a stable sort keeps each snapshot's rows in the order the dump gave
        # them, which is the order the whole-dump formulation's single stable
        # argsort produced and the order the failure examples are drawn in
        rows = rows[np.argsort(rows["SnapNum"], kind="stable")]
        values, starts = np.unique(rows["SnapNum"], return_index=True)
        ends = np.append(starts[1:], rows.size)
        for value, start, end in zip(values.tolist(), starts.tolist(), ends.tolist()):
            self._handle(value).write(rows[start:end].tobytes())
            self.counts[value] += end - start

    def snapshot_values(self) -> List[int]:
        """Every snapshot value the dump carried, ascending — the order the
        whole-dump formulation's single stable argsort presented them in, with
        out-of-range values interleaved at their numeric position (below zero
        first, past the dataset last)."""
        below = sorted(value for value in self.out_of_range if value < 0)
        above = sorted(value for value in self.out_of_range if value >= self.n_snapshots)
        present = [snap for snap in range(self.n_snapshots) if self.counts[snap]]
        return below + present + above

    def count(self, snap: int) -> int:
        """Rows the dump carried at one snapshot value, in range or not."""
        if 0 <= snap < self.n_snapshots:
            return int(self.counts[snap])
        return int(self.out_of_range.get(snap, 0))

    def rows(self, snap: int) -> np.ndarray:
        """One in-dataset snapshot's dump rows, in dump order."""
        if not self.counts[snap]:
            return np.empty(0, dtype=_TOPOLOGY_DUMP_DTYPE)
        return np.fromfile(self._partition_path(snap), dtype=_TOPOLOGY_DUMP_DTYPE)


class _MostBoundWindow:
    """The MostBoundID column of at most three adjacent snapshots.

    Every converter link reaches at most one snapshot either way (descendants
    advance one, first-progenitors retreat one, the rest are same-snapshot), so
    resolving snapshot ``s``'s links needs ``s-1``, ``s`` and ``s+1`` and
    nothing else. Snapshots below the predecessor are dropped as the walk
    advances, so the window holds three columns rather than the dataset's.
    """

    def __init__(self, snapshots: _Snapshots):
        self._snapshots = snapshots
        self._cache: Dict[int, np.ndarray] = {}

    def advance(self, snap: int) -> None:
        for held in [key for key in self._cache if key < snap - 1]:
            del self._cache[held]

    def put(self, snap: int, values: np.ndarray) -> None:
        self._cache[snap] = values

    def get(self, snap: int) -> np.ndarray:
        if snap not in self._cache:
            self._cache[snap] = self._snapshots.load(snap, ("MostBoundID",))["MostBoundID"]
        return self._cache[snap]


def check_topology_chains(snapshots: _Snapshots, partition: TopologyDumpPartition) -> List[str]:
    """Compare the reference dump against the converter, by stable ctrees id,
    for every halo in the dataset: the five link fields
    (Descendant/FirstProgenitor/NextProgenitor/FirstHaloInFOFgroup/
    NextHaloInFOFgroup), the two identity fields (ForestIndex/
    HaloRankInForest), and the halo's own signed MostBoundID.

    The six checks above establish that the right galaxies exist at the right
    rank *on the lineage-creation subset*; this check is the direct proof that
    link ORDER matches, by resolving each converter link (a same-file or
    adjacent-file index) to an id and comparing it against the reference's own
    recorded id for the same link. Matching per snapshot uses that snapshot's
    ascending-unique ``|MostBoundID|`` order, already re-asserted by
    ``build_match`` for every snapshot before this check ever runs.

    Coverage is asserted first, and it is what makes the rest a proof rather
    than a sample: the dump must name every converter halo exactly once at
    every snapshot. A truncated, empty, or duplicated dump would otherwise
    compare cleanly over whatever subset it happened to contain and report
    PASS — a vacuous result, and the likeliest real-world failure (a killed
    harness run, a full disk) rather than an exotic one.

    Failures are reported as one counted summary line per (snapshot, field)
    with example ids, never one line per halo: a systematic converter error at
    micro-Uchuu scale would otherwise build a 22.6 M-element list and a
    multi-gigabyte joined string before anyone could read it.

    The dump arrives already partitioned by snapshot on disk, and the converter
    side is read one snapshot at a time, so what is resident is one snapshot's
    dump rows, one snapshot's link and identity arrays, and the three-snapshot
    MostBoundID window the link resolution needs.
    """
    failures = []
    n_snapshots = snapshots.n_snapshots

    #: (dump field, snapshot offset the converter link points into), matching
    #: the HDF5 contract: descendants advance one snapshot, first-progenitors
    #: retreat one, the rest are same-snapshot links.
    link_fields = (
        ("Descendant", 1),
        ("FirstProgenitor", -1),
        ("NextProgenitor", 0),
        ("FirstHaloInFOFgroup", 0),
        ("NextHaloInFOFgroup", 0),
    )
    #: Identity fields the dump also carries, compared per halo against the
    #: converter's own arrays. The reference reader's within-forest rank is the
    #: halo's position in its final per-forest InputTreeHalos order, which is
    #: exactly what HaloRankInForest must reproduce; comparing it here extends
    #: rank conformance from identity-creation's first-appearance subset to
    #: every halo, including halos that never seed a galaxy.
    identity_fields = ("ForestIndex", "HaloRankInForest")

    # Completeness, per snapshot, before any comparison (see docstring). Only
    # the row counts are needed, so no halo is read here.
    for snap in range(n_snapshots):
        converter_n = snapshots.rows(snap)
        if int(partition.counts[snap]) != converter_n:
            failures.append(
                "snapshot {}: reference dump has {} halo row(s) but the converter has {} "
                "halo(s) — the dump must name every converter halo exactly once".format(
                    snap, int(partition.counts[snap]), converter_n
                )
            )

    if partition.total_rows == 0:
        return failures

    window = _MostBoundWindow(snapshots)
    for snap in partition.snapshot_values():
        if snap < 0 or snap >= n_snapshots:
            failures.append(
                "reference dump has {} halo(s) at snapshot {}, outside the dataset's "
                "[0, {})".format(partition.count(snap), snap, n_snapshots)
            )
            continue
        window.advance(snap)
        rows = partition.rows(snap)
        arrays = snapshots.load(snap, _TOPOLOGY_FIELDS)
        window.put(snap, arrays["MostBoundID"])

        # Resolve every dumped id to its converter row via one batched
        # searchsorted over this snapshot's ascending-unique |MostBoundID|.
        dump_ids = rows["MostBoundID"].astype(np.int64)
        targets = np.abs(dump_ids)
        # Matching is by magnitude, so a duplicated |id| would let one dumped
        # halo stand in for another and keep the coverage count balanced. The
        # converter side is already strictly ascending-unique (build_match).
        dup_ids, dup_counts = np.unique(targets, return_counts=True)
        repeated = dup_ids[dup_counts > 1]
        if repeated.size:
            failures.append(
                "snapshot {}: {} duplicate |MostBoundID| value(s) in the reference dump; "
                "examples: {}".format(snap, int(repeated.size), _examples(repeated.tolist()))
            )
        arr = np.abs(arrays["MostBoundID"])
        if arr.size == 0:
            matched = np.zeros(targets.shape, dtype=bool)
            conv_rows = np.empty(0, dtype=np.intp)
        else:
            pos = np.searchsorted(arr, targets)
            matched = (pos < arr.size) & (arr[np.minimum(pos, arr.size - 1)] == targets)
            conv_rows = pos[matched]
        if not matched.all():
            failures.append(
                "snapshot {}: {} reference halo(s) with no matching converter halo; example "
                "ctrees ids: {}".format(
                    snap,
                    int((~matched).sum()),
                    _examples(dump_ids[~matched].tolist()),
                )
            )
        if not matched.any():
            continue
        m_ids = dump_ids[matched]

        # The halo's own signed id, not just its magnitude. Matching is by
        # |MostBoundID|, so a wrong flyby sign on the halo itself would
        # otherwise only be caught indirectly, via some other halo's link
        # resolving to it — and check_flyby_signs compares signs only over the
        # matched Type 0/1 population, so galaxy-less demoted halos depend on
        # this comparison being direct.
        conv_signed = arrays["MostBoundID"][conv_rows].astype(np.int64)
        sign_bad = conv_signed != m_ids
        if sign_bad.any():
            first = int(np.flatnonzero(sign_bad)[0])
            failures.append(
                "snapshot {}: {} halo(s) with a MostBoundID sign mismatch; first at ctrees id "
                "{} (reference {}, converter {}); example ctrees ids: {}".format(
                    snap,
                    int(sign_bad.sum()),
                    abs(int(m_ids[first])),
                    int(m_ids[first]),
                    int(conv_signed[first]),
                    _examples(np.abs(m_ids[sign_bad]).tolist()),
                )
            )

        for field in identity_fields:
            expected_identity = rows[field][matched].astype(np.int64)
            conv_identity = np.asarray(arrays[field])[conv_rows].astype(np.int64)
            bad = conv_identity != expected_identity
            if bad.any():
                first = int(np.flatnonzero(bad)[0])
                failures.append(
                    "snapshot {}: {} halo(s) with mismatched {}; first at ctrees id {} "
                    "(reference {}, converter {}); example ctrees ids: {}".format(
                        snap,
                        int(bad.sum()),
                        field,
                        abs(int(m_ids[first])),
                        int(expected_identity[first]),
                        int(conv_identity[first]),
                        _examples(np.abs(m_ids[bad]).tolist()),
                    )
                )

        for field, delta in link_fields:
            target_snap = snap + delta
            # Reference-recorded link id (NA sentinel where the dump had no link).
            expected = rows[field][matched].astype(np.int64)
            # Converter's own link: a local index into target_snap's array, or a
            # negative "no link" sentinel.
            conv_target = np.asarray(arrays[field])[conv_rows].astype(np.int64)
            no_link = conv_target < 0
            # conv_id defaults to the NA sentinel (covers the no-link rows); a
            # valid in-range target overwrites it with the resolved id below.
            conv_id = np.full(conv_target.shape, _INT64_MIN, dtype=np.int64)

            if target_snap < 0 or target_snap >= n_snapshots:
                # A non-negative link into a non-existent snapshot is malformed;
                # no-link rows still fall through to the comparison (as the
                # row-at-a-time code did, short-circuiting on conv_target < 0
                # before this branch), where NA == NA passes.
                if (~no_link).any():
                    failures.append(
                        "snapshot {}: {} converter halo(s) with {} pointing to snapshot {}, "
                        "outside the dataset; example ctrees ids: {}".format(
                            snap,
                            int((~no_link).sum()),
                            field,
                            target_snap,
                            _examples(np.abs(m_ids[~no_link]).tolist()),
                        )
                    )
                compare = no_link
            else:
                tgt_mostbound = window.get(target_snap)
                out_of_range = ~no_link & (conv_target >= tgt_mostbound.size)
                if out_of_range.any():
                    failures.append(
                        "snapshot {}: {} converter halo(s) with a {} index outside snapshot "
                        "{}'s {} halo(s); example ctrees ids: {}".format(
                            snap,
                            int(out_of_range.sum()),
                            field,
                            target_snap,
                            tgt_mostbound.size,
                            _examples(np.abs(m_ids[out_of_range]).tolist()),
                        )
                    )
                in_range = ~no_link & (conv_target < tgt_mostbound.size)
                conv_id[in_range] = tgt_mostbound[conv_target[in_range]].astype(np.int64)
                # Out-of-range rows are already reported and skipped; compare
                # the no-link and in-range rows.
                compare = no_link | in_range

            mismatched = compare & (conv_id != expected)
            if mismatched.any():
                # Keep the actual disagreeing ids for the first mismatch: a
                # count alone would not say which side is wrong. The NA
                # sentinel displays as None, as "no link" reads better than
                # INT64_MIN.
                first = int(np.flatnonzero(mismatched)[0])
                exp_first = int(expected[first])
                conv_first = int(conv_id[first])
                failures.append(
                    "snapshot {}: {} halo(s) with a {} mismatch; first at ctrees id {} "
                    "(reference {}, converter {}); example ctrees ids: {}".format(
                        snap,
                        int(mismatched.sum()),
                        field,
                        abs(int(m_ids[first])),
                        None if exp_first == _INT64_MIN else exp_first,
                        None if conv_first == _INT64_MIN else conv_first,
                        _examples(np.abs(m_ids[mismatched]).tolist()),
                    )
                )
    return failures


# ---------------------------------------------------------------------------
# Cross-check driver
# ---------------------------------------------------------------------------


#: The seven always-run checks, in report order. ``topology-chains`` is
#: appended after them when a reference-topology dump is supplied.
CHECK_NAMES = (
    "reference-sanity",
    "identity-forest",
    "identity-creation",
    "fof-central",
    "flyby-signs",
    "values",
    "occupancy",
)


def run_crosscheck(
    converted_dir,
    reference_dir,
    a_list_path,
    simulation_info_path,
    base: str = "halos",
    multiplier: int = DEFAULT_MULTIPLIER,
    topology_dump_path=None,
) -> List[Outcome]:
    """Run the six cross-checks (plus reference sanity) and return one Outcome
    per named check. When ``topology_dump_path`` is given, also runs the
    seventh ``topology-chains`` check against that reference-topology dump.

    Snapshots are walked in ascending order and every check is applied to one
    snapshot's window before it is released, so each check's failures still
    accumulate in ascending snapshot order and every Outcome reports exactly
    what the whole-dataset formulation reported. The topology check gets its
    own second walk, after the seven, so that a malformed dump is still
    reported after — never instead of — an abort the seven would have raised.
    """
    converted_dir = Path(converted_dir)
    if not converted_dir.is_dir():
        raise ConverterError("{}: not a directory".format(converted_dir))
    a_list, _ = load_a_list(a_list_path)
    n_snapshots = len(a_list)
    snapshots = _Snapshots(converted_dir, n_snapshots)
    # Particle mass (1e10 Msun/h) needed to reconstruct satellite Mvir
    # (Len * PartMass). Read it from simulation_info — the same native value the
    # reference model uses (MimicConfig.PartMass) and the converter's own Len
    # derivation — so the reconstruction is bit-for-bit for any particle mass,
    # not only header round-trip-safe ones. Guard that the simulation_info
    # matches the emitted dataset: the header stores particle_mass_msun_h =
    # value * REF_TO_NATIVE_MASS (the same constant the writer used), so
    # recompute it identically and require agreement across all files. The
    # distinct values are carried as a set, so this opens every file (as the
    # whole-dataset load did) without holding any of them.
    part_mass = load_particle_mass(simulation_info_path)
    expected_header_mass = part_mass * REF_TO_NATIVE_MASS
    header_masses = {
        float(np.asarray(snapshots.header(snap)["particle_mass_msun_h"]))
        for snap in range(n_snapshots)
    }
    if header_masses != {expected_header_mass}:
        raise ConverterError(
            "simulation_info particle mass ({} -> {} Msun/h) does not match the dataset header "
            "particle_mass_msun_h {}".format(part_mass, expected_header_mass, sorted(header_masses))
        )
    reference = ReferenceGalaxies(reference_dir, base, n_snapshots)

    failures: Dict[str, List[str]] = {name: [] for name in CHECK_NAMES}
    with _SeenIdentities() as seen:
        forwarded = None
        for snap in range(n_snapshots):
            match = build_match(snapshots.load(snap, _MATCH_FIELDS), reference.load(snap), snap)
            failures["reference-sanity"].extend(check_reference_sanity(match))
            failures["identity-forest"].extend(check_identity_forest(match, multiplier))
            failures["identity-creation"].extend(check_identity_creation(match, multiplier, seen))
            failures["fof-central"].extend(check_fof_central(match))
            failures["flyby-signs"].extend(check_flyby_signs(match))
            failures["values"].extend(check_values(match, part_mass))
            occupancy_failures, forwarded = check_occupancy(match, forwarded)
            failures["occupancy"].extend(occupancy_failures)
        _log(
            "crosscheck: identity suppression set peaked at {} byte(s) on disk".format(
                seen.peak_bytes
            )
        )

    def outcome(name, check_failures):
        if check_failures:
            return Outcome(name, "FAIL", "; ".join(check_failures))
        return Outcome(name, "PASS")

    outcomes = [outcome(name, failures[name]) for name in CHECK_NAMES]
    if topology_dump_path is not None:
        with TopologyDumpPartition(topology_dump_path, n_snapshots) as partition:
            chain_failures = check_topology_chains(snapshots, partition)
            _log(
                "crosscheck: topology dump partition held {} byte(s) on disk".format(
                    partition.peak_bytes
                )
            )
        outcomes.append(outcome("topology-chains", chain_failures))
    return outcomes


def crosscheck_failed(outcomes) -> bool:
    return any(outcome.status == "FAIL" for outcome in outcomes)


# ---------------------------------------------------------------------------
# Reference-run plumbing
# ---------------------------------------------------------------------------


def write_reference_run_file(source_run_file, target_path, snapshot_list, output_directory) -> Path:
    """Copy a halos-only run YAML to ``target_path`` with only the output
    snapshot list and output directory replaced; everything else is preserved.
    Refuses to overwrite the source (repo run files are read-only inputs)."""
    source = Path(source_run_file)
    target = Path(target_path)
    if source.resolve() == target.resolve():
        raise ConverterError(
            "{}: refusing to overwrite the source run file — write the reference run file to "
            "a separate path".format(source)
        )
    with open(source) as handle:
        data = yaml.safe_load(handle)
    data.setdefault("output", {})
    data["output"]["snapshot_list"] = list(snapshot_list)
    data["output"]["output_directory"] = str(output_directory)
    with open(target, "w") as handle:
        yaml.safe_dump(data, handle)
    return target


def run_reference(executable, run_file, log_path) -> int:
    """Run ``[executable, run_file]`` with stdout+stderr appended to a durable
    log at ``log_path``; append a final ``exit code: <rc>`` line and return
    the exit code."""
    log_path = Path(log_path)
    with open(log_path, "a") as log:
        completed = subprocess.run(
            [str(executable), str(run_file)], stdout=log, stderr=subprocess.STDOUT
        )
        log.write("exit code: {}\n".format(completed.returncode))
    return completed.returncode


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------


def _cmd_compare(args) -> int:
    outcomes = run_crosscheck(
        args.converted_dir,
        args.reference_dir,
        args.a_list,
        args.simulation_info,
        base=args.reference_base,
        multiplier=args.multiplier,
        topology_dump_path=args.reference_topology,
    )
    for outcome in outcomes:
        print(outcome.line())
    if args.report is not None:
        report = {
            "checks": [outcome.as_dict() for outcome in outcomes],
            "status": {outcome.name: outcome.status for outcome in outcomes},
            "passed": not crosscheck_failed(outcomes),
        }
        with open(args.report, "w") as handle:
            json.dump(report, handle, indent=2)
    if crosscheck_failed(outcomes):
        print("crosscheck: FAIL", file=sys.stderr)
        return 1
    print("crosscheck: PASS", file=sys.stderr)
    return 0


def _cmd_prepare(args) -> int:
    a_list, _ = load_a_list(args.a_list)
    workdir = Path(args.workdir)
    output_dir = Path(args.output_dir) if args.output_dir else workdir / "reference-output"
    target = write_reference_run_file(
        args.run_file, workdir / "reference_run.yaml", range(len(a_list)), output_dir
    )
    print(str(target))
    return 0


def _cmd_run_reference(args) -> int:
    return run_reference(args.mimic, args.run_file, args.log)


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(
        prog="crosscheck",
        description="Cross-check a converted snapshot-HDF5 dataset against a Mimic halos-only "
        "reference-run galaxy output",
    )
    sub = parser.add_subparsers(dest="command", required=True)

    compare = sub.add_parser("compare", help="run the cross-check")
    compare.add_argument("converted_dir", help="converter output directory of snapshot_NNN.h5")
    compare.add_argument("reference_dir", help="reference-run galaxy output directory")
    compare.add_argument("--a-list", required=True, help="canonical a_list (one scale per line)")
    compare.add_argument(
        "--simulation-info",
        required=True,
        help="simulation_info.yaml providing the native particle mass (1e10 Msun/h) used to "
        "reconstruct satellite Mvir; must match the dataset the reference run consumed",
    )
    compare.add_argument(
        "--reference-base", default="halos", help="reference chunk file base (default halos)"
    )
    compare.add_argument(
        "--multiplier",
        type=int,
        default=DEFAULT_MULTIPLIER,
        help="UniqueGalaxyID multiplier (default {})".format(DEFAULT_MULTIPLIER),
    )
    compare.add_argument(
        "--reference-topology",
        default=None,
        help="optional reference-topology dump (tests/unit/tools/dump_ctrees_topology.c "
        "output) for a direct chain-order check",
    )
    compare.add_argument("--report", default=None, help="optional JSON report path")
    compare.set_defaults(func=_cmd_compare)

    prepare = sub.add_parser("prepare", help="write a reference run file listing all snapshots")
    prepare.add_argument("--run-file", required=True, help="source halos-only run YAML")
    prepare.add_argument("--workdir", required=True, help="workdir for reference_run.yaml")
    prepare.add_argument(
        "--a-list", required=True, help="canonical a_list (fixes the snapshot list)"
    )
    prepare.add_argument(
        "--output-dir",
        default=None,
        help="reference output directory (default <workdir>/reference-output)",
    )
    prepare.set_defaults(func=_cmd_prepare)

    run_ref = sub.add_parser("run-reference", help="run Mimic on a reference run file")
    run_ref.add_argument("--mimic", required=True, help="Mimic executable")
    run_ref.add_argument("--run-file", required=True, help="reference run YAML")
    run_ref.add_argument("--log", required=True, help="durable log path (appended)")
    run_ref.set_defaults(func=_cmd_run_reference)

    args = parser.parse_args(argv)
    try:
        return args.func(args)
    except ConverterError as exc:
        print("ERROR: {}".format(exc), file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
