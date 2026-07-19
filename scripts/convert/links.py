"""Phase 3 links, ranks, and identity fields for the ctrees -> snapshot-HDF5
converter (plan Slice 6).

Implements the conversion plan's Phase 3 steps 6-9 on the fixed per-snapshot
arrays produced by the Slice 5 fix-up stage. Reference semantics replicated
exactly against ``assign_mergertree_indices`` (src/io/tree/ctrees/ctrees_utils.c):

- **Reference order** (ctrees_utils.c:524-547): the reference sorts each forest
  by descending scale, then ascending upid, pid, id, over POST-fix values.
  Within one snapshot the scale is constant, so the within-snapshot reference
  ("encounter") order reduces to ascending ``(upid, pid, id)`` — NOT the
  ascending-id slab order (the conversion plan's IDENTITY TRAP note).
- **FoF chains** (ctrees_utils.c:549-623): walking the reference order, a
  ``pid == -1`` central opens its group (``FirstHaloInFOFgroup`` self-reference,
  ``NextHaloInFOFgroup`` starts at -1) and every satellite is appended at the
  chain tail in reference order. A group whose first member is not its central
  is the reference's "sort did not place the FOF before the subs" hard error.
- **Descendants** (ctrees_utils.c:628-665): resolved by id at snapshot N+1
  (adjacency was validated in the fix-up stage); a missing target aborts with
  examples — never rewritten to -1, which would silently drop a merger link.
- **Progenitor chains** (ctrees_utils.c:667-706): replicated LITERALLY. The
  first progenitor encountered becomes the chain; each later progenitor is
  promoted to the chain FRONT if strictly more massive (native float32 Mvir,
  the units the reference compares — struct halo_data carries native Msun/h
  through assign_mergertree_indices) than the CURRENT chain head, otherwise
  appended at the tail. The final chain is NOT "max-Mvir first with the rest
  in encounter order" in general: an ascending-mass sequence promotes every
  progenitor and yields a fully reversed chain. The literal insertion loop is
  the parity-bearing semantics; see ``build_progenitor_links``.
- **Identity** (conversion plan Phase 3 step 9): ``HaloRankInForest`` is the
  within-forest index in reference tree-driver order — (snapshot descending,
  upid, pid, id ascending) over post-fix values, equivalent to the reference's
  scale-descending sort because the a_list is strictly increasing (asserted
  from the manifest's observed pairs). ``ForestIndex`` is the dense run-scoped
  forest number carried from the Phase 0 table.

``FirstProgenitor`` values for snapshot N+1 are computed while snapshot N is
resident and carried in a persistent pending buffer (one ``.bin`` intermediate
per target snapshot, so the stage resumes per snapshot). Link fields are int32
snapshot-local indices into the ascending-id slab order, which the emission
slice writes out unchanged (ascending id == ascending |MostBoundID|, the
Slice 5 invariant). All aborts carry counts and concrete examples — never
repair.

Out of scope (plan Slice 6 non-goals): HDF5 emission (Slice 7) and the chunked
external-merge rank sort — the rank pass groups all snapshots in memory, which
is sufficient for micro-Uchuu and fixtures; the external-sort fallback is a
Shin-Uchuu production concern (see README).
"""

import os
import sys
from pathlib import Path
from typing import Dict, Tuple

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from ctrees_parser import ConverterError  # noqa: E402
from fixups import FIXED_DTYPE_TAG, FIXED_RECORD_DTYPE  # noqa: E402
from scatter import Manifest  # noqa: E402

#: Per-snapshot link/identity record, row-aligned with the fixed scratch file
#: (ascending-id slab order). Link fields are int32 snapshot-local indices
#: (format invariant 2); identity fields are int64 per the frozen contract.
LINKS_RECORD_DTYPE = np.dtype(
    [
        ("Descendant", "<i4"),
        ("FirstProgenitor", "<i4"),
        ("NextProgenitor", "<i4"),
        ("FirstHaloInFOFgroup", "<i4"),
        ("NextHaloInFOFgroup", "<i4"),
        ("ForestIndex", "<i8"),
        ("HaloRankInForest", "<i8"),
    ],
    align=False,
)

#: Human-readable dtype identity recorded in every links-file manifest entry.
LINKS_DTYPE_TAG = "ctrees-links-v1/itemsize=36/" + ",".join(
    "{}:{}".format(name, LINKS_RECORD_DTYPE.fields[name][0].str)
    for name in LINKS_RECORD_DTYPE.names
)

_INT32_MAX = np.iinfo(np.int32).max


def links_scratch_name(snap: int) -> str:
    return "snap_{:03d}_links.bin".format(snap)


def pending_fp_name(snap: int) -> str:
    """Pending FirstProgenitor buffer FOR snapshot ``snap`` (int32, one value
    per snap slab row), written while snapshot ``snap - 1`` is resident."""
    return "snap_{:03d}_pending_fp.bin".format(snap)


def _log(message: str) -> None:
    print(message, file=sys.stderr)


def reference_order(records: np.ndarray) -> np.ndarray:
    """Encounter permutation: ascending (upid, pid, id) over post-fix values —
    the assign_mergertree_indices sort key (ctrees_utils.c:530-547) restricted
    to one snapshot, where the scale comparison is constant."""
    return np.lexsort((records["id"], records["pid"], records["upid"]))


def validate_slab(records: np.ndarray, snap: int, context: str) -> None:
    """Guards the link stage relies on: int32-indexable slab (format invariant
    2), strictly ascending ids (slab-order contract from the sort stage), and
    the post-fix central invariant ``upid == id`` where ``pid == -1``."""
    n = records.size
    if n > _INT32_MAX:
        raise ConverterError(
            "{}: snapshot {} has {} halos, exceeding the int32 link-field bound {}".format(
                context, snap, n, _INT32_MAX
            )
        )
    ids = records["id"]
    if n > 1:
        unordered = np.nonzero(ids[1:] <= ids[:-1])[0]
        if unordered.size:
            examples = [
                "(row={}, id={}, next id={})".format(int(r), int(ids[r]), int(ids[r + 1]))
                for r in unordered[:5]
            ]
            raise ConverterError(
                "{}: snapshot {} slab is not strictly ascending in id at {} position(s); "
                "examples: {}".format(context, snap, unordered.size, ", ".join(examples))
            )
    central = records["pid"] == -1
    bad = central & (records["upid"] != ids)
    if bad.any():
        rows = np.nonzero(bad)[0][:5]
        examples = ["(id={}, upid={})".format(int(ids[r]), int(records["upid"][r])) for r in rows]
        raise ConverterError(
            "{}: snapshot {} has {} central(s) with upid != id after fix-ups; "
            "examples: {}".format(context, snap, int(bad.sum()), ", ".join(examples))
        )


def build_fof_chains(
    records: np.ndarray, order: np.ndarray, snap: int, context: str
) -> Tuple[np.ndarray, np.ndarray]:
    """FoF chain construction in reference order (ctrees_utils.c:549-623).

    Groups are contiguous runs of equal resolved ``upid`` in the reference
    order; each group's first member must be its central (``pid == -1`` and
    ``id == upid`` — otherwise the reference's "sort did not place the FOF
    before the subs" error), every member must share the central's forest,
    and satellites chain in reference order, ending at -1.
    """
    n = records.size
    first_fof = np.full(n, -1, dtype=np.int32)
    next_fof = np.full(n, -1, dtype=np.int32)
    if n == 0:
        return first_fof, next_fof
    upid_sorted = records["upid"][order]
    new_group = np.r_[True, upid_sorted[1:] != upid_sorted[:-1]]
    group_id = np.cumsum(new_group) - 1
    starts = np.nonzero(new_group)[0]
    heads = order[starts]

    bad_head = (records["pid"][heads] != -1) | (records["id"][heads] != upid_sorted[starts])
    if bad_head.any():
        idx = np.nonzero(bad_head)[0][:5]
        examples = [
            "(group upid={}, first member id={}, pid={})".format(
                int(upid_sorted[starts[g]]),
                int(records["id"][heads[g]]),
                int(records["pid"][heads[g]]),
            )
            for g in idx
        ]
        raise ConverterError(
            "{}: snapshot {} has {} FoF group(s) whose first member in reference order "
            "is not the group's central; examples: {}".format(
                context, snap, int(bad_head.sum()), ", ".join(examples)
            )
        )

    member_forest = records["forest_id"][order]
    central_forest = records["forest_id"][heads][group_id]
    bad_forest = member_forest != central_forest
    if bad_forest.any():
        idx = np.nonzero(bad_forest)[0][:5]
        examples = [
            "(id={}, forest={}, central id={}, central forest={})".format(
                int(records["id"][order[i]]),
                int(member_forest[i]),
                int(records["id"][heads[group_id[i]]]),
                int(central_forest[i]),
            )
            for i in idx
        ]
        raise ConverterError(
            "{}: snapshot {} has {} FoF member(s) in a different forest than their "
            "central; examples: {}".format(
                context, snap, int(bad_forest.sum()), ", ".join(examples)
            )
        )

    first_fof[order] = heads[group_id].astype(np.int32)
    same_group = ~new_group[1:]
    next_fof[order[:-1][same_group]] = order[1:][same_group].astype(np.int32)
    return first_fof, next_fof


def build_descendants(
    records: np.ndarray, next_ids: np.ndarray, snap: int, context: str
) -> np.ndarray:
    """Descendant merge-join on sorted ids (conversion plan Phase 3 step 7).

    ``next_ids`` is snapshot N+1's sorted id index; positions in it ARE the
    N+1 slab indices. A ``desc_id`` with no target is corrupt input under the
    adjacency invariant — abort with examples, never rewrite to -1.
    """
    n = records.size
    desc = np.full(n, -1, dtype=np.int32)
    has = np.nonzero(records["desc_id"] != -1)[0]
    if has.size == 0:
        return desc
    targets = records["desc_id"][has]
    pos = np.searchsorted(next_ids, targets)
    if next_ids.size:
        clipped = np.minimum(pos, next_ids.size - 1)
        missing = (pos >= next_ids.size) | (next_ids[clipped] != targets)
    else:
        missing = np.ones(targets.size, dtype=bool)
    if missing.any():
        rows = has[missing][:5]
        examples = [
            "(id={}, desc_id={})".format(int(records["id"][r]), int(records["desc_id"][r]))
            for r in rows
        ]
        raise ConverterError(
            "{}: snapshot {} has {} descendant link(s) with no target halo at snapshot {}; "
            "examples: {}".format(context, snap, int(missing.sum()), snap + 1, ", ".join(examples))
        )
    desc[has] = pos.astype(np.int32)
    return desc


def build_progenitor_links(
    records: np.ndarray, order: np.ndarray, desc: np.ndarray, n_next: int
) -> Tuple[np.ndarray, np.ndarray]:
    """Progenitor chains, replicating the reference insertion loop LITERALLY
    (ctrees_utils.c:667-706).

    Progenitors of each descendant are enumerated in reference encounter order
    (the ``order`` permutation — (upid, pid, id), NOT slab order). The first
    becomes the chain; each later progenitor is promoted to the chain front if
    its native float32 Mvir is strictly greater than the CURRENT chain head's
    (``forest[first_prog].Mvir < forest[i].Mvir``), otherwise appended at the
    tail. Ties therefore keep the first-encountered halo in front, and an
    ascending-mass sequence yields a fully reversed chain — the literal
    reference behaviour, deliberately not simplified to "max first, remainder
    in encounter order".

    Returns ``(next_prog, pending_fp)``: NextProgenitor chains within this
    snapshot's slab, and the FirstProgenitor values for snapshot N+1 (indexed
    by N+1 slab position, -1 where a halo has no progenitor).
    """
    next_prog = np.full(records.size, -1, dtype=np.int32)
    pending_fp = np.full(n_next, -1, dtype=np.int32)
    enc = order[desc[order] != -1]
    if enc.size == 0:
        return next_prog, pending_fp
    d = desc[enc]
    by_desc = np.argsort(d, kind="stable")  # stable: keeps encounter order per descendant
    rows = enc[by_desc]
    d_sorted = d[by_desc]
    new_d = np.r_[True, d_sorted[1:] != d_sorted[:-1]]
    starts = np.nonzero(new_d)[0]
    ends = np.r_[starts[1:], d_sorted.size]
    single = (ends - starts) == 1
    pending_fp[d_sorted[starts[single]]] = rows[starts[single]].astype(np.int32)

    mvir = records["Mvir"]
    for s, e in zip(starts[~single], ends[~single]):
        progs = rows[s:e]
        head = int(progs[0])
        tail = head
        for p in progs[1:]:
            p = int(p)
            if mvir[head] < mvir[p]:
                next_prog[p] = head
                head = p
            else:
                next_prog[tail] = p
                tail = p
        pending_fp[d_sorted[s]] = head
    return next_prog, pending_fp


def verify_descendant_forests(
    desc: np.ndarray,
    forest_index: np.ndarray,
    next_forest_index: np.ndarray,
    records: np.ndarray,
    snap: int,
    context: str,
) -> None:
    """A descendant must live in the same forest as its progenitor — the
    reference resolves descendants inside one forest's array, so a cross-forest
    match is unrepresentable there. The global merge-join could silently
    accept one if forests.list mis-grouped linked trees; abort instead."""
    has = np.nonzero(desc != -1)[0]
    if has.size == 0:
        return
    bad = forest_index[has] != next_forest_index[desc[has]]
    if bad.any():
        rows = has[bad][:5]
        examples = [
            "(id={}, desc_id={}, ForestIndex={}, descendant ForestIndex={})".format(
                int(records["id"][r]),
                int(records["desc_id"][r]),
                int(forest_index[r]),
                int(next_forest_index[desc[r]]),
            )
            for r in rows
        ]
        raise ConverterError(
            "{}: snapshot {} has {} descendant link(s) crossing forest boundaries; "
            "examples: {}".format(context, snap, int(bad.sum()), ", ".join(examples))
        )


def verify_identity(
    forest_index: np.ndarray, ranks: np.ndarray, n_forests: int, context: str
) -> None:
    """The plan's global identity assertion, checked on the OUTPUT arrays
    independently of how they were constructed: (ForestIndex, HaloRankInForest)
    pairs unique, ForestIndex dense over [0, n_forests), and ranks dense per
    forest (format invariant 4)."""
    order = np.lexsort((ranks, forest_index))
    fi = forest_index[order]
    rk = ranks[order]
    new_forest = np.r_[True, fi[1:] != fi[:-1]]
    starts = np.nonzero(new_forest)[0]
    if not np.array_equal(fi[starts], np.arange(n_forests, dtype=fi.dtype)):
        raise ConverterError(
            "{}: observed ForestIndex values are not dense over [0, {})".format(context, n_forests)
        )
    group_id = np.cumsum(new_forest) - 1
    expected = np.arange(fi.size, dtype=rk.dtype) - starts[group_id]
    bad = rk != expected
    if bad.any():
        idx = np.nonzero(bad)[0][:5]
        examples = [
            "(ForestIndex={}, rank={}, expected {})".format(
                int(fi[i]), int(rk[i]), int(expected[i])
            )
            for i in idx
        ]
        raise ConverterError(
            "{}: {} (ForestIndex, HaloRankInForest) pair(s) violate per-forest "
            "density/uniqueness; examples: {}".format(context, int(bad.sum()), ", ".join(examples))
        )


def _validate_monotonic_pairs(manifest: Manifest) -> None:
    """Snapshot-descending rank order equals the reference's scale-descending
    order only if snapshot number and scale factor are in strict one-to-one
    ascending correspondence; assert both halves from the manifest's
    a_list-validated observed pairs. A snapshot observed with more than one
    scale value would let the reference's scale sort split what the converter
    treats as one slab, so it aborts even though each value is within the
    a_list tolerance."""
    pairs = sorted((int(s), float(a)) for s, a in manifest.data["observed_pairs"])
    for (snap_a, scale_a), (snap_b, scale_b) in zip(pairs, pairs[1:]):
        if snap_b == snap_a:
            raise ConverterError(
                "snapshot {} was observed with multiple scale factors ({} and {}) — the "
                "reference scale-descending sort could order these halos differently from "
                "one snapshot-number slab".format(snap_a, scale_a, scale_b)
            )
        if scale_b <= scale_a:
            raise ConverterError(
                "observed scale factors are not strictly increasing with snapshot number: "
                "snapshot {} has scale {} but snapshot {} has scale {} — snapshot-descending "
                "rank order would not match the reference scale-descending sort".format(
                    snap_a, scale_a, snap_b, scale_b
                )
            )


def _load_fixed(manifest: Manifest, snap: int) -> np.ndarray:
    """Verify and load one snapshot's fixed scratch file."""
    entry = manifest.data["snapshots"][str(snap)]
    tag = manifest.data["intermediates"][entry["fixed_file"]].get("dtype_tag")
    if tag != FIXED_DTYPE_TAG:
        raise ConverterError(
            "{}: fixed-file dtype tag {!r} != expected {!r} — refusing to link".format(
                entry["fixed_file"], tag, FIXED_DTYPE_TAG
            )
        )
    manifest.verify_intermediate(entry["fixed_file"], "fixed snapshot scratch")
    records = np.fromfile(entry["fixed_file"], dtype=FIXED_RECORD_DTYPE)
    if len(records) != entry["rows"]:
        raise ConverterError(
            "{}: has {} rows, manifest records {}".format(
                entry["fixed_file"], len(records), entry["rows"]
            )
        )
    return records


def compute_identity(
    manifest: Manifest,
) -> Tuple[Dict[int, Tuple[np.ndarray, np.ndarray]], int, int]:
    """Rank pass (conversion plan Phase 3 step 9): HaloRankInForest per forest
    in reference tree-driver order over all snapshots — (snapshot descending,
    upid, pid, id ascending) on post-fix values — plus the dense ForestIndex
    from the Phase 0 table.

    In-memory grouping over all snapshots' key columns; sufficient for
    micro-Uchuu and fixtures (the external-merge sort is a production
    concern). Returns ``({snap: (forest_index, ranks)}, n_forests_total,
    max_halo_rank_in_forest)`` with per-snapshot arrays in slab order.
    """
    _validate_monotonic_pairs(manifest)
    table_path = Path(manifest.workdir) / "forest_index_table.npy"
    manifest.verify_intermediate(table_path, "forest index table")
    forest_table = np.load(table_path)

    snaps = sorted(int(s) for s in manifest.data["snapshots"])
    forests_l, snaps_l, upids_l, pids_l, ids_l = [], [], [], [], []
    counts = []
    for snap in snaps:
        records = _load_fixed(manifest, snap)
        forests_l.append(records["forest_id"].copy())
        snaps_l.append(np.full(records.size, snap, dtype=np.int64))
        upids_l.append(records["upid"].copy())
        pids_l.append(records["pid"].copy())
        ids_l.append(records["id"].copy())
        counts.append(records.size)

    forest = np.concatenate(forests_l)
    neg_snap = -np.concatenate(snaps_l)
    upid = np.concatenate(upids_l)
    pid = np.concatenate(pids_l)
    ids = np.concatenate(ids_l)
    total = forest.size

    order = np.lexsort((ids, pid, upid, neg_snap, forest))
    sorted_forest = forest[order]
    new_forest = np.r_[True, sorted_forest[1:] != sorted_forest[:-1]]
    starts = np.nonzero(new_forest)[0]
    observed_forests = sorted_forest[starts]
    if not np.array_equal(observed_forests, forest_table):
        missing = np.setdiff1d(forest_table, observed_forests)
        extra = np.setdiff1d(observed_forests, forest_table)
        raise ConverterError(
            "observed forests do not match the Phase 0 forest index table: {} listed forest(s) "
            "have no halos (examples: {}), {} observed forest(s) are unlisted (examples: "
            "{})".format(missing.size, missing[:5].tolist(), extra.size, extra[:5].tolist())
        )
    group_id = np.cumsum(new_forest) - 1
    ranks = np.empty(total, dtype=np.int64)
    ranks[order] = np.arange(total, dtype=np.int64) - starts[group_id]
    forest_index = np.searchsorted(forest_table, forest)

    n_forests_total = int(forest_table.size)
    verify_identity(forest_index, ranks, n_forests_total, "identity pass")
    max_rank = int(ranks.max()) if total else -1

    identity: Dict[int, Tuple[np.ndarray, np.ndarray]] = {}
    offset = 0
    for snap, count in zip(snaps, counts):
        identity[snap] = (forest_index[offset : offset + count], ranks[offset : offset + count])
        offset += count
    return identity, n_forests_total, max_rank


def link_one_snapshot(
    manifest: Manifest,
    snap: int,
    identity: Dict[int, Tuple[np.ndarray, np.ndarray]],
) -> None:
    """Link one snapshot: FoF chains, descendant merge-join, progenitor
    chains, pending FirstProgenitor hand-off, and identity carry-through."""
    entry = manifest.data["snapshots"][str(snap)]
    if entry.get("status") == "linked":
        meta = manifest.verify_intermediate(entry["links_file"], "snapshot links scratch")
        if meta.get("dtype_tag") != LINKS_DTYPE_TAG:
            raise ConverterError(
                "{}: links-file dtype tag {!r} != current {!r} — refusing to skip-trust an "
                "artifact from a different converter revision".format(
                    entry["links_file"], meta.get("dtype_tag"), LINKS_DTYPE_TAG
                )
            )
        if meta.get("rows") != entry["rows"]:
            raise ConverterError(
                "{}: links file records {} rows, snapshot manifest records {}".format(
                    entry["links_file"], meta.get("rows"), entry["rows"]
                )
            )
        expected_bytes = entry["rows"] * LINKS_RECORD_DTYPE.itemsize
        actual_bytes = Path(entry["links_file"]).stat().st_size
        if actual_bytes != expected_bytes:
            raise ConverterError(
                "{}: links file is {} bytes, expected {} ({} rows x {} bytes)".format(
                    entry["links_file"],
                    actual_bytes,
                    expected_bytes,
                    entry["rows"],
                    LINKS_RECORD_DTYPE.itemsize,
                )
            )
        return

    records = _load_fixed(manifest, snap)
    context = entry["fixed_file"]
    scratch_dir = Path(entry["fixed_file"]).parent
    validate_slab(records, snap, context)
    order = reference_order(records)
    first_fof, next_fof = build_fof_chains(records, order, snap, context)

    next_entry = manifest.data["snapshots"].get(str(snap + 1))
    if next_entry is not None:
        manifest.verify_intermediate(next_entry["index_file"], "snapshot id index")
        next_ids = np.fromfile(next_entry["index_file"], dtype=np.int64)
        if len(next_ids) != next_entry["rows"]:
            raise ConverterError(
                "{}: has {} ids, manifest records {}".format(
                    next_entry["index_file"], len(next_ids), next_entry["rows"]
                )
            )
    else:
        next_ids = np.empty(0, dtype=np.int64)

    desc = build_descendants(records, next_ids, snap, context)
    if next_entry is not None:
        verify_descendant_forests(
            desc, identity[snap][0], identity[snap + 1][0], records, snap, context
        )
    next_prog, pending_fp = build_progenitor_links(records, order, desc, next_ids.size)

    prev_entry = manifest.data["snapshots"].get(str(snap - 1))
    if prev_entry is not None:
        pending_path = scratch_dir / pending_fp_name(snap)
        manifest.verify_intermediate(pending_path, "pending first-progenitor buffer")
        first_prog = np.fromfile(pending_path, dtype=np.int32)
        if len(first_prog) != records.size:
            raise ConverterError(
                "{}: pending buffer has {} values, snapshot {} has {} halos".format(
                    pending_path, len(first_prog), snap, records.size
                )
            )
    else:
        # no data at snapshot snap-1 (start of the run or an empty snapshot):
        # nothing can have progenitors here
        first_prog = np.full(records.size, -1, dtype=np.int32)

    forest_index, ranks = identity[snap]
    if forest_index.size != records.size:
        raise ConverterError(
            "{}: identity pass produced {} rows for snapshot {}, expected {}".format(
                context, forest_index.size, snap, records.size
            )
        )

    links = np.zeros(records.size, dtype=LINKS_RECORD_DTYPE)
    links["Descendant"] = desc
    links["FirstProgenitor"] = first_prog
    links["NextProgenitor"] = next_prog
    links["FirstHaloInFOFgroup"] = first_fof
    links["NextHaloInFOFgroup"] = next_fof
    links["ForestIndex"] = forest_index
    links["HaloRankInForest"] = ranks

    if next_entry is not None:
        pending_next = scratch_dir / pending_fp_name(snap + 1)
        pending_fp.tofile(pending_next)
        reread_pending = np.fromfile(pending_next, dtype=np.int32)
        if not np.array_equal(reread_pending, pending_fp):
            raise ConverterError(
                "{}: pending buffer re-read does not match what was written".format(pending_next)
            )
        manifest.register_intermediate(
            pending_next, "pending-first-progenitor", rows=int(pending_fp.size), dtype_tag="<i4"
        )

    links_path = scratch_dir / links_scratch_name(snap)
    links.tofile(links_path)
    reread = np.fromfile(links_path, dtype=LINKS_RECORD_DTYPE)
    if not np.array_equal(reread, links):
        raise ConverterError(
            "{}: links file re-read does not match what was written".format(links_path)
        )
    manifest.register_intermediate(
        links_path, "snapshot-links", rows=int(links.size), dtype_tag=LINKS_DTYPE_TAG
    )
    entry["links_file"] = str(links_path.resolve())
    entry["status"] = "linked"
    manifest.save()
    _log(
        "links: snapshot {} — {} rows, {} FoF group(s), {} descendant link(s), "
        "{} progenitor sibling link(s)".format(
            snap,
            records.size,
            int((first_fof == np.arange(records.size, dtype=np.int32)).sum()),
            int((desc != -1).sum()),
            int((next_prog != -1).sum()),
        )
    )


def run_links(workdir) -> Manifest:
    """Run the link stage over every fixed snapshot, in ascending order.

    Snapshot subsets are deliberately not supported: FirstProgenitor values
    flow forward through the pending buffer, so snapshots must be linked in
    order. Snapshots already linked are verified and skipped; the run-scoped
    identity values are recomputed and must match what a previous run
    recorded (refuse-not-repair).
    """
    manifest = Manifest.load_or_create(workdir)
    if not manifest.path.exists():
        raise ConverterError("{}: no manifest found; run scatter first".format(workdir))
    snaps = sorted(int(s) for s in manifest.data["snapshots"])
    if not snaps:
        raise ConverterError("{}: manifest records no snapshots".format(workdir))
    for snap in snaps:
        status = manifest.data["snapshots"][str(snap)].get("status")
        if status not in ("fixed", "linked"):
            raise ConverterError(
                "snapshot {}: unexpected status {!r}; run fixups first".format(snap, status)
            )

    identity, n_forests_total, max_rank = compute_identity(manifest)
    recorded = manifest.data.get("links")
    computed = {"n_forests_total": n_forests_total, "max_halo_rank_in_forest": max_rank}
    if recorded is not None and recorded != computed:
        raise ConverterError(
            "run-scoped identity values changed across runs: manifest records {}, "
            "recomputed {} — refusing to mix link outputs".format(recorded, computed)
        )
    manifest.data["links"] = computed
    manifest.save()

    for snap in snaps:
        link_one_snapshot(manifest, snap, identity)
    _log(
        "links: {} snapshot(s) linked — n_forests_total={}, max_halo_rank_in_forest={}".format(
            len(snaps), n_forests_total, max_rank
        )
    )
    return manifest
