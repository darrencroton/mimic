"""Consumer cross-check for the ctrees -> snapshot-HDF5 converter (plan Slice 8).

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
  4. flyby-signs        negative-MostBoundID sets match exactly both directions
  5. values             Pos/Vel/Spin/VelDisp/Vmax bit-exact, Len exact, Mvir
                        equal to ``float64(M_Crit200) * 1e-10`` bit-for-bit
  6. occupancy          the matched-halo set equals the reference occupancy
                        predicate computed on the converter links by forward
                        induction, with zero unmatched reference galaxies

Matching is by ``|MostBoundID|``: the converter emits ascending-unique
|MostBoundID| per file (the slab order), so a galaxy is matched to a halo by
searchsorted. The ascending-unique invariant is re-asserted and any violation
aborts (ConverterError) — matching cannot be trusted otherwise.

Also provides reference-run plumbing (``write_reference_run_file`` /
``run_reference``) and a CLI (``compare`` / ``prepare`` / ``run-reference``).

The plan's optional ``--reference-topology`` mode (Slice 10, direct
FirstProgenitor/NextProgenitor/NextHaloInFOFgroup chain-order comparison by
stable id) is NOT implemented here: Slice 10 has not been approved/landed, so
this module is the partial six-check gate the plan describes for Slice 8.
"""

import argparse
import json
import os
import re
import subprocess
import sys
from pathlib import Path
from typing import Dict, List

import h5py
import numpy as np
import yaml

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from ctrees_parser import ConverterError  # noqa: E402
from hdf5_writer import snapshot_h5_name  # noqa: E402
from scatter import load_a_list  # noqa: E402
from validate import DEFAULT_MULTIPLIER, Outcome, load_dataset  # noqa: E402

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


def load_reference_galaxies(reference_dir, base: str, n_snapshots: int) -> Dict[int, np.ndarray]:
    """Load reference galaxies per snapshot from the ``<base>_<digits>.hdf5``
    chunk files (the master ``<base>.hdf5`` is ignored). Galaxies at a
    snapshot are concatenated across chunks in ascending numeric-suffix order;
    a missing ``Snap%03d`` group contributes zero galaxies."""
    reference_dir = Path(reference_dir)
    if not reference_dir.is_dir():
        raise ConverterError("{}: not a directory".format(reference_dir))
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

    by_snap: Dict[int, List[np.ndarray]] = {snap: [] for snap in range(n_snapshots)}
    ref_dtype = None
    for _, path in chunks:
        with h5py.File(path, "r") as handle:
            for snap in range(n_snapshots):
                group_name = "Snap{:03d}".format(snap)
                if group_name not in handle:
                    continue
                group = handle[group_name]
                if "Galaxies" not in group:
                    continue
                data = group["Galaxies"][...]
                # every dataset in every chunk is validated (a later chunk
                # with a wider dtype would silently promote the concatenated
                # array and defeat bit-exact comparison)
                _validate_reference_dtype(data.dtype, path)
                if ref_dtype is None:
                    ref_dtype = data.dtype
                elif data.dtype != ref_dtype:
                    raise ConverterError(
                        "{}: Snap{:03d}/Galaxies dtype {} differs from the first chunk's "
                        "dtype {} — reference chunks must share one structured dtype".format(
                            path, snap, data.dtype, ref_dtype
                        )
                    )
                by_snap[snap].append(data)
    if ref_dtype is None:
        raise ConverterError(
            "{}: reference chunks carry no Snap###/Galaxies datasets".format(reference_dir)
        )

    result: Dict[int, np.ndarray] = {}
    for snap in range(n_snapshots):
        parts = by_snap[snap]
        result[snap] = np.concatenate(parts) if parts else np.empty(0, dtype=ref_dtype)
    return result


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


def build_matches(arrays, ref_by_snap, n_snapshots) -> List[SnapMatch]:
    """Match every reference Type 0/1 galaxy to a converter halo by
    ``|MostBoundID|`` for each snapshot, re-asserting the converter's
    ascending-unique |MostBoundID| slab order first (ConverterError if not)."""
    matches = []
    for snap in range(n_snapshots):
        conv = arrays[snap]
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
        ref = ref_by_snap[snap]
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
        matches.append(SnapMatch(snap, ref, t01_idx, conv, matched))
    return matches


def _matched_pairs(match: SnapMatch):
    """(galaxy indices into ref, converter halo indices) for matched pairs."""
    mask = match.matched >= 0
    return match.t01_idx[mask], match.matched[mask]


# ---------------------------------------------------------------------------
# The six checks (+ reference sanity)
# ---------------------------------------------------------------------------


def check_reference_sanity(matches) -> List[str]:
    """Reference data must be internally consistent before it can be a ground
    truth: SnapNum matches the group, no INT64_MIN MostBoundID (its magnitude
    overflows signed int64), |MostBoundID| is unique over Type 0/1, and
    UniqueGalaxyID is unique over ALL Type 0/1 galaxies (it is a run-scoped
    persistent identity — a duplicate on a satellite would otherwise slip
    past identity-creation once the id had been seen)."""
    failures = []
    for match in matches:
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


def check_identity_forest(matches, multiplier) -> List[str]:
    """Every matched galaxy's decoded forest equals its halo's ForestIndex."""
    failures = []
    for match in matches:
        gal_idx, conv_idx = _matched_pairs(match)
        if gal_idx.size == 0:
            continue
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


def check_identity_creation(matches, multiplier) -> List[str]:
    """Process snapshots ascending; each galaxy whose UniqueGalaxyID first
    appears here must decode to its matched halo's (ForestIndex, rank).

    ``seen`` is a sorted int64 array (not a Python set): the check runs over
    every galaxy of the real micro-Uchuu output at the Slice 9 gate, where
    per-galaxy Python loops and a tens-of-millions-entry set are prohibitive.
    A galaxy that is unmatched is skipped here — that is an occupancy failure,
    not an identity failure.
    """
    failures = []
    seen = np.empty(0, dtype=np.int64)
    for match in matches:
        t01_ugid = np.asarray(match.ref["UniqueGalaxyID"][match.t01_idx], dtype=np.int64)
        if t01_ugid.size == 0:
            continue
        candidate = ~np.isin(t01_ugid, seen) & (match.matched >= 0)
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
        seen = np.union1d(seen, t01_ugid)
    return failures


def check_fof_central(matches) -> List[str]:
    """The Type 0 galaxy named by each matched galaxy's UniqueCentralGalaxyID
    must exist and share |MostBoundID| with the converter FoF-central target.

    The Type 0 UniqueGalaxyID -> |MostBoundID| lookup is a sorted-array
    searchsorted rather than a Python dict (Slice 9 scale; see
    check_identity_creation). Duplicate Type 0 UniqueGalaxyIDs are a
    reference-sanity failure and resolve here to their first occurrence.
    """
    failures = []
    for match in matches:
        ref = match.ref
        gal_idx, conv_idx = _matched_pairs(match)
        if gal_idx.size == 0:
            continue
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


def check_flyby_signs(matches) -> List[str]:
    """The set of negative MostBoundID values over reference Type 0/1 galaxies
    must equal the set over ALL converter halos in that file (both ways)."""
    failures = []
    for match in matches:
        ref_neg = {int(v) for v in match.ref["MostBoundID"][match.t01_idx] if v < 0}
        conv_neg = {int(v) for v in match.conv["MostBoundID"] if v < 0}
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


def check_values(matches) -> List[str]:
    """Bit-exact value comparison over matched pairs (frozen rules): float32
    fields via uint32 views (NaN payloads and signed zeros count), Len exact,
    Mvir equal to ``float64(M_Crit200) * 1e-10`` bit-for-bit."""
    failures = []
    for match in matches:
        gal_idx, conv_idx = _matched_pairs(match)
        if gal_idx.size == 0:
            continue
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
        report(
            "Len", ref_sub["Len"].astype(np.int64) != match.conv["Len"][conv_idx].astype(np.int64)
        )
        # the documented reference arithmetic: float32 M_Crit200 widened to
        # float64, scaled by 1e-10 in float64; the operand dtype is asserted
        # before the deliberate widening
        m200 = np.ascontiguousarray(match.conv["M_Crit200"][conv_idx])
        if m200.dtype != np.float32:
            raise ConverterError(
                "converter M_Crit200 must be float32, got {} — refusing to coerce".format(
                    m200.dtype
                )
            )
        expected = m200.astype(np.float64) * 1e-10
        report("Mvir", _u64(ref_sub["Mvir"]) != _u64(expected))
    return failures


def check_occupancy(matches, arrays) -> List[str]:
    """The matched-halo set at each snapshot must equal the reference
    occupancy predicate on the converter links, computed by forward induction
    (``occupied(H) = FoF-central(H) OR any occupied progenitor``), and no
    reference Type 0/1 galaxy may be unmatched."""
    failures = []
    occupied_prev = None
    for match in matches:
        conv = match.conv
        n = conv["MostBoundID"].size
        is_central = conv["FirstHaloInFOFgroup"] == np.arange(n)
        occupied = is_central.copy()
        if occupied_prev is not None:
            desc = arrays[match.snap - 1]["Descendant"]
            forwarded = desc[occupied_prev]
            forwarded = forwarded[forwarded != -1]
            occupied[forwarded] = True

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
        occupied_prev = occupied
    return failures


# ---------------------------------------------------------------------------
# Cross-check driver
# ---------------------------------------------------------------------------


def run_crosscheck(
    converted_dir,
    reference_dir,
    a_list_path,
    base: str = "halos",
    multiplier: int = DEFAULT_MULTIPLIER,
) -> List[Outcome]:
    """Run the six cross-checks (plus reference sanity) and return one Outcome
    per named check."""
    converted_dir = Path(converted_dir)
    if not converted_dir.is_dir():
        raise ConverterError("{}: not a directory".format(converted_dir))
    a_list, _ = load_a_list(a_list_path)
    n_snapshots = len(a_list)
    _, arrays = load_dataset(converted_dir, n_snapshots)
    ref_by_snap = load_reference_galaxies(reference_dir, base, n_snapshots)
    matches = build_matches(arrays, ref_by_snap, n_snapshots)

    def outcome(name, failures):
        if failures:
            return Outcome(name, "FAIL", "; ".join(failures))
        return Outcome(name, "PASS")

    return [
        outcome("reference-sanity", check_reference_sanity(matches)),
        outcome("identity-forest", check_identity_forest(matches, multiplier)),
        outcome("identity-creation", check_identity_creation(matches, multiplier)),
        outcome("fof-central", check_fof_central(matches)),
        outcome("flyby-signs", check_flyby_signs(matches)),
        outcome("values", check_values(matches)),
        outcome("occupancy", check_occupancy(matches, arrays)),
    ]


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
        base=args.reference_base,
        multiplier=args.multiplier,
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
        "reference-run galaxy output (plan Slice 8 six-check gate)",
    )
    sub = parser.add_subparsers(dest="command", required=True)

    compare = sub.add_parser("compare", help="run the six-check cross-check")
    compare.add_argument("converted_dir", help="converter output directory of snapshot_NNN.h5")
    compare.add_argument("reference_dir", help="reference-run galaxy output directory")
    compare.add_argument("--a-list", required=True, help="canonical a_list (one scale per line)")
    compare.add_argument(
        "--reference-base", default="halos", help="reference chunk file base (default halos)"
    )
    compare.add_argument(
        "--multiplier",
        type=int,
        default=DEFAULT_MULTIPLIER,
        help="UniqueGalaxyID multiplier (default {})".format(DEFAULT_MULTIPLIER),
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
