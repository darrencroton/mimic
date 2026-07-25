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

Also provides reference-run plumbing (``write_reference_run_file`` /
``run_reference``) and a CLI (``compare`` / ``prepare`` / ``run-reference``).

An optional seventh check, ``topology-chains``, runs when ``compare`` is given
``--reference-topology <dump>``: it compares the converter against a reference
dump produced by an independent implementation reading the same source data
(see tests/unit/tools/dump_ctrees_topology.c and load_reference_topology_dump
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
import json
import os
import re
import subprocess
import sys
import warnings
from pathlib import Path
from typing import Dict, List

import h5py
import numpy as np
import yaml

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from ctrees_parser import ConverterError  # noqa: E402
from fixups import NATIVE_TO_REF_MASS, REF_TO_NATIVE_MASS, load_particle_mass  # noqa: E402
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
    every galaxy of the real micro-Uchuu output, where per-galaxy Python loops
    and a tens-of-millions-entry set are prohibitive.
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
    searchsorted rather than a Python dict (real micro-Uchuu scale; see
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
    for match in matches:
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


def check_values(matches, part_mass) -> List[str]:
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
                "converter M_Crit200 must be float32, got {} — refusing to coerce".format(
                    m200.dtype
                )
            )
        halo_mass = m200.astype(np.float64) * NATIVE_TO_REF_MASS
        len_mass = match.conv["Len"][conv_idx].astype(np.float64) * part_mass
        is_central = match.conv["FirstHaloInFOFgroup"][conv_idx] == conv_idx
        expected = np.where(is_central & (halo_mass >= 0.0), halo_mass, len_mass)
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


def load_reference_topology_dump(path) -> np.ndarray:
    """Load a reference-topology dump: a fixed three-line header (format
    marker, column names, NA-sentinel value) followed by one whitespace-
    separated row per halo, in the column order of ``_TOPOLOGY_DUMP_DTYPE``,
    and nothing else. Raises ConverterError on a header mismatch, a ragged or
    non-integer row, or a comment line after the header — this loader and the
    harness that writes the dump must agree on the format exactly, and silent
    field-order drift or a silently spliced second dump would defeat every
    comparison below without ever failing loudly."""
    path = Path(path)
    # Validate the fixed three-line header, then stream the data rows into the
    # typed array from the SAME open handle: the real micro-Uchuu dump is ~2 GB
    # of text (22.6 M rows), so no intermediate Python list is ever built.
    # ``readline`` past EOF returns "" — a short file therefore fails the
    # header/sentinel checks rather than raising.
    with open(path) as handle:
        header = [handle.readline().rstrip("\n") for _ in range(3)]
        if header[0] != _TOPOLOGY_DUMP_HEADER:
            raise ConverterError(
                "{}: not a recognised reference-topology dump (expected first line {!r})".format(
                    path, _TOPOLOGY_DUMP_HEADER
                )
            )
        expected_sentinel = "# NA sentinel = {} (no link)".format(_INT64_MIN)
        if header[2] != expected_sentinel:
            raise ConverterError(
                "{}: NA sentinel line {!r} != expected {!r}".format(
                    path, header[2], expected_sentinel
                )
            )
        # np.loadtxt (C-accelerated in NumPy >= 1.23) reads the remaining rows
        # with bounded memory. ``comments=None`` is deliberate: the format is
        # exactly three header lines followed by data rows, so a "#" line after
        # the header means a malformed dump (two runs concatenated, a harness
        # re-run appended with ">>"). Letting np.loadtxt skip such lines would
        # silently splice unrelated dumps into one array. A ragged row, a
        # non-integer field, or a stray comment raises ValueError, remapped to
        # ConverterError so the loader keeps a single loud failure mode.
        try:
            with warnings.catch_warnings():
                # A header-only dump parses to zero rows here; it is
                # check_topology_chains' coverage assertion, not the loader,
                # that rejects it against the converter's halo counts.
                warnings.simplefilter("ignore", category=UserWarning)
                parsed = np.loadtxt(handle, dtype=_TOPOLOGY_DUMP_DTYPE, comments=None, ndmin=1)
        except ValueError as exc:
            raise ConverterError("{}: malformed data row ({})".format(path, exc))
    return np.ascontiguousarray(parsed)


def check_topology_chains(matches, arrays, dump: np.ndarray) -> List[str]:
    """Compare the reference dump against the converter, by stable ctrees id,
    for every halo in the dataset: the five link fields
    (Descendant/FirstProgenitor/NextProgenitor/FirstHaloInFOFgroup/
    NextHaloInFOFgroup), the two identity fields (ForestIndex/
    HaloRankInForest), and the halo's own signed MostBoundID.

    The six checks above establish that the right galaxies exist at the right
    rank *on the lineage-creation subset*; this check is the direct proof that
    link ORDER matches, by resolving each converter link (a same-file or
    adjacent-file index) to an id and comparing it against the reference's own
    recorded id for the same link. Matching per snapshot reuses each
    ``matches[i].conv``'s ascending-unique ``|MostBoundID|`` order, already
    re-asserted by ``build_matches`` before this check ever runs.

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
    """
    failures = []
    n_snapshots = len(arrays)
    abs_by_snap = [np.abs(match.conv["MostBoundID"]) for match in matches]
    mostbound_by_snap = [match.conv["MostBoundID"] for match in matches]

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

    dump_snaps = dump["SnapNum"].astype(np.int64)

    # Completeness, per snapshot, before any comparison (see docstring).
    within_dataset = (dump_snaps >= 0) & (dump_snaps < n_snapshots)
    dump_counts = np.bincount(dump_snaps[within_dataset], minlength=n_snapshots)
    for snap in range(n_snapshots):
        converter_n = abs_by_snap[snap].size
        if int(dump_counts[snap]) != converter_n:
            failures.append(
                "snapshot {}: reference dump has {} halo row(s) but the converter has {} "
                "halo(s) — the dump must name every converter halo exactly once".format(
                    snap, int(dump_counts[snap]), converter_n
                )
            )

    if dump_snaps.size == 0:
        return failures

    # Group dump rows by snapshot with a single stable argsort rather than a
    # per-row Python dict, then work one snapshot-slice at a time with fully
    # vectorised lookups. The previous row-at-a-time loop did a searchsorted per
    # halo per field (~5 x 22.6 M interpreter iterations on the real dump); this
    # collapses it to O(snapshots x fields) batched operations.
    order = np.argsort(dump_snaps, kind="stable")
    sorted_snaps = dump_snaps[order]
    uniq_snaps, slice_starts = np.unique(sorted_snaps, return_index=True)
    slice_ends = np.append(slice_starts[1:], sorted_snaps.size)

    for snap_val, start, end in zip(uniq_snaps, slice_starts, slice_ends):
        snap = int(snap_val)
        rows = dump[order[start:end]]
        if snap < 0 or snap >= n_snapshots:
            failures.append(
                "reference dump has {} halo(s) at snapshot {}, outside the dataset's [0, {})".format(
                    rows.size, snap, n_snapshots
                )
            )
            continue

        # Resolve every dumped id to its converter row via one batched
        # searchsorted over this snapshot's ascending-unique |MostBoundID|.
        dump_ids = rows["MostBoundID"].astype(np.int64)
        targets = np.abs(dump_ids)
        # Matching is by magnitude, so a duplicated |id| would let one dumped
        # halo stand in for another and keep the coverage count balanced. The
        # converter side is already strictly ascending-unique (build_matches).
        dup_ids, dup_counts = np.unique(targets, return_counts=True)
        repeated = dup_ids[dup_counts > 1]
        if repeated.size:
            failures.append(
                "snapshot {}: {} duplicate |MostBoundID| value(s) in the reference dump; "
                "examples: {}".format(snap, int(repeated.size), _examples(repeated.tolist()))
            )
        arr = abs_by_snap[snap]
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
        conv_signed = mostbound_by_snap[snap][conv_rows].astype(np.int64)
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
            conv_identity = np.asarray(arrays[snap][field])[conv_rows].astype(np.int64)
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
            conv_target = np.asarray(arrays[snap][field])[conv_rows].astype(np.int64)
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
                tgt_mostbound = mostbound_by_snap[target_snap]
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
    seventh ``topology-chains`` check against that reference-topology dump."""
    converted_dir = Path(converted_dir)
    if not converted_dir.is_dir():
        raise ConverterError("{}: not a directory".format(converted_dir))
    a_list, _ = load_a_list(a_list_path)
    n_snapshots = len(a_list)
    headers, arrays = load_dataset(converted_dir, n_snapshots)
    # Particle mass (1e10 Msun/h) needed to reconstruct satellite Mvir
    # (Len * PartMass). Read it from simulation_info — the same native value the
    # reference model uses (MimicConfig.PartMass) and the converter's own Len
    # derivation — so the reconstruction is bit-for-bit for any particle mass,
    # not only header round-trip-safe ones. Guard that the simulation_info
    # matches the emitted dataset: the header stores particle_mass_msun_h =
    # value * REF_TO_NATIVE_MASS (the same constant the writer used), so
    # recompute it identically and require agreement across all files.
    part_mass = load_particle_mass(simulation_info_path)
    expected_header_mass = part_mass * REF_TO_NATIVE_MASS
    header_masses = {float(np.asarray(header["particle_mass_msun_h"])) for header in headers}
    if header_masses != {expected_header_mass}:
        raise ConverterError(
            "simulation_info particle mass ({} -> {} Msun/h) does not match the dataset header "
            "particle_mass_msun_h {}".format(part_mass, expected_header_mass, sorted(header_masses))
        )
    ref_by_snap = load_reference_galaxies(reference_dir, base, n_snapshots)
    matches = build_matches(arrays, ref_by_snap, n_snapshots)

    def outcome(name, failures):
        if failures:
            return Outcome(name, "FAIL", "; ".join(failures))
        return Outcome(name, "PASS")

    outcomes = [
        outcome("reference-sanity", check_reference_sanity(matches)),
        outcome("identity-forest", check_identity_forest(matches, multiplier)),
        outcome("identity-creation", check_identity_creation(matches, multiplier)),
        outcome("fof-central", check_fof_central(matches)),
        outcome("flyby-signs", check_flyby_signs(matches)),
        outcome("values", check_values(matches, part_mass)),
        outcome("occupancy", check_occupancy(matches, arrays)),
    ]
    if topology_dump_path is not None:
        dump = load_reference_topology_dump(topology_dump_path)
        outcomes.append(outcome("topology-chains", check_topology_chains(matches, arrays, dump)))
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
