"""Bounded-memory mode of the version 3 producer battery, for adjacent datasets.

:func:`run_battery_bounded` settles every check the external-sort battery
(:func:`validate_v3.run_battery_v3`) settles, under the same names, statuses
and failure kinds, without any whole-dataset sort and without spilling
anything. It is reached through ``run_battery_v3(..., bounded=True)`` and
``convert_trees.py validate|report --bounded``; the external-sort battery
stays the default and is unchanged.

**Scope.** Only datasets whose every snapshot file declares
``links_adjacent`` 1 are accepted; a file declaring anything else is refused
by name before any long pass, as are a missing or unloadable conversion
manifest (the source-key bitset is sized from its inventory) and a ceiling
above the budget. Adjacency is what makes the work local: every
``Descendant`` of a conforming dataset lies in the next snapshot, every
``FirstProgenitor`` in the previous one and every ``NextProgenitor`` in the
owner's own, so each link is judged with at most two snapshots resident.

**Obligations and their algorithms** (one row per obligation):

================================  ===============================================================
Obligation (checks)               Bounded algorithm
================================  ===============================================================
Manifest binding and source       Unchanged: the external-sort battery's own functions
conservation (manifest-binding,   (registered names and SHA-256, the pinned a_list, the ingest
count-conservation,               and transpose counts, the sidecar read in blocks).
sidecar-content)

Structure, headers and /schema    Unchanged: the external-sort battery's own functions, one file
agreement (file-set, object-set,  at a time.
sidecar-object-set,
schema-binding, header-values,
run-scoped-headers)

Every per-row invariant           Unchanged and per block: the external-sort battery's own block
(row-values, len-nonnegative,     check on every row; the previous block's last SourceHaloID is
field-finiteness,                 carried into the next.
position-bounds, link-targets,
links-adjacent)

Progenitor closure, chain         Per snapshot pair N-1 and N: N-1's Descendant,
membership and FoF integrity      FirstHaloInFOFgroup, NextProgenitor, NextHaloInFOFgroup and
(topology-closure)                ForestIndex (each link combined with its *Snapshot column into
                                  a global position) and two int32 in-link counters resident,
                                  with N's FirstProgenitor and ForestIndex. Every link into N-1
                                  is judged against its target's own values and counted, as the
                                  external-sort battery's join does, a block of owners at a time.
                                  A link that does not join adjacent snapshots (only in a failing
                                  dataset) is held from the row pass and judged when its target
                                  is resident.

Chain termination (chain-cycles)  Per snapshot, one chain pass per chain kind (NextProgenitor,
                                  NextHaloInFOFgroup) with a visited bitset: walks start at the
                                  heads (no in-link) and a halo is visited once every link into
                                  it was walked; a halo never visited lies on a cycle. The pass's
                                  state (the in-link counters and the bitset) is per snapshot, so
                                  a chain crossing a block boundary is walked whole.
                                  NextProgenitor links leaving a snapshot are contracted to a
                                  graph over those links alone and walked the same way.

SourceHaloID uniqueness and the   A one-bit-per-id bitset over the inventory's [1, total_halos]:
[1, total] density                an id set twice is a duplicate, a clear bit at the end a gap.
(source-key-coverage)

The (ForestIndex, rank) position  Per-forest halo totals (int64 per forest, from the row pass)
check and rank density            become each forest's start position; a second one-bit-per-halo
(identity)                        bitset indexed by start + rank catches a repeated pair, a rank
                                  at or beyond its forest's total breaks density, and
                                  SourceHaloID must be start + rank + 1. The ASCII forest
                                  density, the last ForestIndex and the header's maximum rank
                                  come from the same totals and the row pass.

Identity bounds (header-bounds)   Unchanged.
================================  ===============================================================

**Memory.** The battery reads every column in blocks of at most
``block_rows`` rows (never one read above ``INT32_MAX`` rows) and holds, by
phase (``W`` the widest snapshot's rows, ``N`` the dataset's halos, ``H`` the
inventory's ``total_halos``, ``F`` the headers' ``n_forests_total``, ``B``
``min(block_rows, W)``, ``R`` the bytes of one row of every ``/halos``
column):

- the row pass: ``8 F`` (per-forest totals) + ``B (4 R + 256)`` (the block
  and its temporaries);
- the pair pass: ``64 W`` (five int64 columns and two int32 counters of the
  target snapshot, two int64 columns of the next) + ``ceil(W / 8)`` (the
  visited bitset) + ``ceil(H / 8)`` + ``ceil(N / 8)`` (the two bitsets) +
  ``8 F`` (the forest start positions) + ``256 B`` (block temporaries) +
  :data:`DIAGNOSTIC_WORKSPACE_BYTES` (2 MiB: the windows that count a
  bitset's set bits and find its first clear ones).

The ceiling is the larger of the two, printed at start and refused when it
exceeds the budget. Beyond it, only a dataset that cannot pass holds
anything: each link that does not join adjacent snapshots costs
:data:`FAR_LINK_BYTES` from what the budget leaves above the ceiling, and a
dataset holding more than fit is refused rather than run over budget. Not
counted, as in the external-sort battery: O(snapshots) header arrays,
the per-snapshot structure reads and interpreter churn. ``spill_dir`` is
accepted for a uniform interface and never written: this mode spills nothing.

``python validate_bounded.py ceiling ...`` evaluates the ceiling for given
dataset numbers without a dataset.
"""

import argparse
import math
import os
import sys
from pathlib import Path
from typing import Dict, List, Optional, Tuple

import h5py
import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from ctrees_parser import ConverterError  # noqa: E402
from hdf5_writer import snapshot_h5_name  # noqa: E402
from rank_sort import ResidencyMeter  # noqa: E402
from scatter import load_a_list  # noqa: E402
from validate import DEFAULT_MULTIPLIER, DEFAULT_V3_BUDGET_BYTES, Outcome  # noqa: E402
from validate_v3 import (  # noqa: E402
    _V3_FIXED_DATASETS,
    _V3_POSITION_IDENTITY,
    _V3_TYPES,
    V3_CHECKS,
    V3BatteryResult,
    _describe_gp,
    _Failures,
    _link_examples,
    _ordered,
    _require_budget,
    _v3_check_file_set,
    _v3_check_headers,
    _v3_count_conservation,
    _v3_load_manifest,
    _v3_manifest_binding,
    _v3_position_bounds,
    _v3_read_header,
    _v3_schema_binding,
    _v3_sidecar_content,
    _v3_sidecar_structure,
    _v3_snapshot_structure,
    _V3Scan,
    check_header_bounds,
)

#: Rows per block read, unless the caller names another.
DEFAULT_BLOCK_ROWS = 1 << 20

#: Largest block: no read is ever above INT32_MAX rows.
MAX_BLOCK_ROWS = int(np.iinfo(np.int32).max)

#: Bytes of the pair pass per row of the widest snapshot: five int64 columns
#: and two int32 in-link counters of the target snapshot, and two int64
#: columns of the snapshot after it.
PAIR_BYTES_PER_ROW = 5 * 8 + 2 * 4 + 2 * 8

#: Temporaries one block of the pair pass holds per row (gathers, masks,
#: positions, the rank and SourceHaloID block, a chain walk's frontier).
PAIR_BLOCK_BYTES_PER_ROW = 256

#: Temporaries one block of the row pass holds per row beyond four copies of
#: the block's columns.
ROW_BLOCK_EXTRA_BYTES_PER_ROW = 256

#: What one link that does not join adjacent snapshots costs while held: its
#: 40-byte record (sorted in place), and at any one moment at most 48 bytes
#: of index arrays over the held NextProgenitor links (their order by owner,
#: owners and successors, and the contracted graph's in-link counts with the
#: gather that feeds them); 88 bytes, charged as 96. Every other array over
#: these links is built a block of at most ``block_rows`` links at a time.
FAR_LINK_BYTES = 96

#: The pair pass's diagnostic workspace: one fixed allowance, in the
#: ceiling, for the windows the bitset counts and clear-bit searches walk,
#: whatever the dataset's size. Each window below is at most this, and they
#: are never held at once.
DIAGNOSTIC_WORKSPACE_BYTES = 2 << 20

#: Bytes of the popcount table walk per bitset chunk: the gathered counts are
#: one byte per bitset byte.
_BITSET_CHUNK = 1 << 20

#: Bytes searched at a time for a clear bit: the search's temporaries are at
#: most this many bytes and eight times as many index bytes (576 KiB).
_CLEAR_SEARCH_BYTES = 1 << 16

_FAR_DTYPE = np.dtype(
    [
        ("target", "<i8"),
        ("tag", "<i8"),
        ("owner", "<i8"),
        ("expected", "<i8"),
        ("forest", "<i8"),
    ]
)
_FAR_FP, _FAR_NP = 1, 2

_POPCOUNT = np.array([bin(value).count("1") for value in range(256)], dtype=np.uint8)

_CHAIN_KINDS = ("NextProgenitor", "NextHaloInFOFgroup")


# ---------------------------------------------------------------------------
# The ceiling
# ---------------------------------------------------------------------------


class Ceiling:
    """The bounded battery's storage ceiling, term by term (bytes)."""

    def __init__(
        self,
        widest_rows: int,
        n_halos: int,
        total_halos: int,
        n_forests_total: int,
        row_bytes: int,
        block_rows: int = DEFAULT_BLOCK_ROWS,
    ):
        self.widest_rows = int(widest_rows)
        self.n_halos = int(n_halos)
        self.total_halos = int(total_halos)
        self.n_forests_total = int(n_forests_total)
        self.row_bytes = int(row_bytes)
        self.block_rows = int(block_rows)
        block = min(self.block_rows, self.widest_rows)
        self.forest_bytes = 8 * max(self.n_forests_total, 0)
        self.row_pass = self.forest_bytes + block * (
            4 * self.row_bytes + ROW_BLOCK_EXTRA_BYTES_PER_ROW
        )
        self.pair_bytes = PAIR_BYTES_PER_ROW * self.widest_rows
        self.visited_bytes = _bitset_bytes(self.widest_rows)
        self.source_key_bytes = _bitset_bytes(self.total_halos)
        self.position_bytes = _bitset_bytes(self.n_halos)
        self.pair_pass = (
            self.pair_bytes
            + self.visited_bytes
            + self.source_key_bytes
            + self.position_bytes
            + self.forest_bytes
            + block * PAIR_BLOCK_BYTES_PER_ROW
            + DIAGNOSTIC_WORKSPACE_BYTES
        )
        self.total = max(self.row_pass, self.pair_pass)

    def as_dict(self) -> Dict[str, int]:
        return {
            "widest_rows": self.widest_rows,
            "n_halos": self.n_halos,
            "total_halos": self.total_halos,
            "n_forests_total": self.n_forests_total,
            "row_bytes": self.row_bytes,
            "block_rows": self.block_rows,
            "row_pass_bytes": self.row_pass,
            "pair_pass_bytes": self.pair_pass,
            "ceiling_bytes": self.total,
        }

    def describe(self) -> str:
        return (
            "ceiling {} B ({:.2f} GB) = max(row pass {} B, pair pass {} B); pair pass = "
            "{} x {} widest rows + {} visited bitset + {} SourceHaloID bitset ({} ids) + {} "
            "position bitset ({} halos) + 8 x {} forests + {} x {} block rows + {} diagnostic "
            "workspace; row pass = 8 x {} forests + (4 x {} + {}) x {} block rows".format(
                self.total,
                self.total / 1e9,
                self.row_pass,
                self.pair_pass,
                PAIR_BYTES_PER_ROW,
                self.widest_rows,
                self.visited_bytes,
                self.source_key_bytes,
                self.total_halos,
                self.position_bytes,
                self.n_halos,
                self.n_forests_total,
                PAIR_BLOCK_BYTES_PER_ROW,
                min(self.block_rows, self.widest_rows),
                DIAGNOSTIC_WORKSPACE_BYTES,
                self.n_forests_total,
                self.row_bytes,
                ROW_BLOCK_EXTRA_BYTES_PER_ROW,
                min(self.block_rows, self.widest_rows),
            )
        )


def _bitset_bytes(n_bits: int) -> int:
    return (max(int(n_bits), 0) + 7) // 8


def _require_block_rows(block_rows) -> int:
    if isinstance(block_rows, bool) or not isinstance(block_rows, (int, np.integer)):
        raise ConverterError("block_rows must be an integer, got {!r}".format(block_rows))
    block_rows = int(block_rows)
    if not 1 <= block_rows <= MAX_BLOCK_ROWS:
        raise ConverterError("block_rows {} is outside [1, {}]".format(block_rows, MAX_BLOCK_ROWS))
    return block_rows


# ---------------------------------------------------------------------------
# Bitsets
# ---------------------------------------------------------------------------


class _Bitset:
    """One bit per index of ``[0, n)``, little-endian within each byte."""

    def __init__(self, n: int):
        self.n = int(n)
        self.bits = np.zeros(_bitset_bytes(self.n), dtype=np.uint8)

    @property
    def nbytes(self) -> int:
        return int(self.bits.nbytes)

    def reset(self, n: int) -> None:
        """Clear every bit and cover ``[0, n)``, within the allocated size."""
        if _bitset_bytes(n) > self.bits.size:
            raise ConverterError("a bitset of {} bits cannot cover {}".format(self.n, n))
        self.bits[:] = 0
        self.n = int(n)

    def test(self, index: np.ndarray) -> np.ndarray:
        shift = (index & 7).astype(np.uint8)
        return ((self.bits[index >> 3] >> shift) & 1).astype(bool)

    def set_sorted(self, index: np.ndarray) -> None:
        """Set every bit of ``index``, which is sorted and unique."""
        if not index.size:
            return
        byte = index >> 3
        masks = np.left_shift(np.uint8(1), (index & 7).astype(np.uint8))
        starts = np.flatnonzero(np.concatenate(([True], byte[1:] != byte[:-1])))
        self.bits[byte[starts]] |= np.bitwise_or.reduceat(masks, starts)

    def first_and_seen(self, index: np.ndarray) -> Tuple[np.ndarray, np.ndarray]:
        """Record ``index`` (any order, repeats allowed). Returns the indices
        that repeat within ``index`` or were already set, one entry per extra
        occurrence, and sets every bit."""
        if not index.size:
            return index, index
        ordered = np.sort(index)
        repeat = np.zeros(ordered.size, dtype=bool)
        repeat[1:] = ordered[1:] == ordered[:-1]
        within = ordered[repeat]
        unique = ordered[~repeat]
        seen = unique[self.test(unique)]
        self.set_sorted(unique)
        return within, seen

    def count(self) -> int:
        total = 0
        for start in range(0, self.bits.size, _BITSET_CHUNK):
            total += int(_POPCOUNT[self.bits[start : start + _BITSET_CHUNK]].sum(dtype=np.int64))
        return total

    def clear_indices(self, limit: int = 5) -> List[int]:
        """The first ``limit`` indices of ``[0, n)`` whose bit is clear.

        Only bytes with a clear bit are unpacked, ``limit`` of them at most,
        found :data:`_CLEAR_SEARCH_BYTES` at a time, so the search holds a
        small fixed amount whatever the bitset's size or how many bits are
        clear."""
        found: List[int] = []
        for start in range(0, self.bits.size, _CLEAR_SEARCH_BYTES):
            chunk = self.bits[start : start + _CLEAR_SEARCH_BYTES]
            for byte in np.flatnonzero(chunk != 0xFF)[:limit]:
                position = 8 * (start + int(byte))
                bits = np.unpackbits(chunk[byte : byte + 1], bitorder="little")
                for bit in np.flatnonzero(bits == 0):
                    if position + int(bit) < self.n and len(found) < limit:
                        found.append(position + int(bit))
            if len(found) >= limit or 8 * (start + _CLEAR_SEARCH_BYTES) >= self.n:
                break
        return found


def _walk_chains(successors, in_links: np.ndarray, block_rows: int, visited: _Bitset) -> int:
    """One chain pass over a snapshot's chain graph; returns the halos never
    visited, which are exactly those on a cycle.

    ``successors(rows)`` gives each row's successor in the snapshot (-1 for
    none) and ``in_links`` each row's count of links into it, which the pass
    consumes. Walks start at every head (no in-link), a block of rows at a
    time; a halo joins a walk once every link into it has been walked, so a
    halo whose predecessors include a cycle never does, and each halo is
    entered once. The bitset is the only state carried from one block's walks
    to the next."""
    n = in_links.size
    for start in range(0, n, block_rows):
        rows = np.arange(start, min(start + block_rows, n), dtype=np.int64)
        frontier = rows[(in_links[rows] == 0) & ~visited.test(rows)]
        visited.set_sorted(frontier)
        while frontier.size:
            after = successors(frontier)
            after = after[after >= 0]
            if not after.size:
                break
            np.subtract.at(in_links, after, 1)
            after = np.unique(after)
            frontier = after[in_links[after] == 0]
            visited.set_sorted(frontier)
    return n - visited.count()


def _cycle_rows(visited: _Bitset) -> List[int]:
    return visited.clear_indices()


# ---------------------------------------------------------------------------
# The row pass
# ---------------------------------------------------------------------------


class _ForestTotals:
    """Halos per forest over ``[0, n_forests_total)``, one int64 each,
    accumulated block by block from the row pass's identity records."""

    def __init__(self, n_forests_total: Optional[int]):
        # a negative header count holds no forest: every ForestIndex is then
        # out of range, which the identity check reports
        self.counts = (
            None if n_forests_total is None else np.zeros(max(n_forests_total, 0), np.int64)
        )

    def add(self, records: np.ndarray) -> None:
        if self.counts is None or not records.size:
            return
        forest = records["forest"]
        forest = forest[(forest >= 0) & (forest < self.counts.size)]
        values, counts = np.unique(forest, return_counts=True)
        self.counts[values] += counts


class _Discard:
    """A record sink that keeps nothing; the source keys are settled in the
    pair pass."""

    def add(self, records: np.ndarray) -> None:
        return None


class _BoundedRowScan(_V3Scan):
    """The external-sort battery's per-row block check, run unchanged, with
    its four sorters replaced: the identity records feed the per-forest
    totals, and the topology emission keeps only the links that do not join
    adjacent snapshots (none in a conforming dataset)."""

    def __init__(
        self,
        directory: Path,
        counts: List[int],
        box_sizes: List[float],
        declarations: Dict[str, Tuple[str, ...]],
        n_forests_total: Optional[int],
        block_rows: int,
        far_capacity: int,
        meter: ResidencyMeter,
    ):
        # The parent's constructor builds the four external sorters; this
        # mode builds none, so the state the block check reads is set here.
        self.directory = directory
        self.counts = np.asarray(counts, dtype=np.int64)
        self.n_snapshots = len(counts)
        self.offsets = np.zeros(self.n_snapshots + 1, dtype=np.int64)
        np.cumsum(self.counts, out=self.offsets[1:])
        self.n_forests_total = n_forests_total
        self.box_sizes = [float(box) for box in box_sizes]
        self.floats = [
            name
            for name, values in sorted(declarations.items())
            if _V3_TYPES.get(values[0], ("", False))[0].startswith("<f")
        ]
        self.position_only_columns = [] if "Pos" in self.floats else ["Pos"]
        self.meter = meter
        self.row_values = _Failures()
        self.lengths = _Failures()
        self.finiteness = _Failures()
        self.positions = _Failures()
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
        self.forest_totals = _ForestTotals(n_forests_total)
        self.identity = self.forest_totals
        self.sids = _Discard()
        self.n_chain_edges = 0
        self.scan_rows = block_rows
        self.far_parts: List[np.ndarray] = []
        self.n_far = 0
        self.far_capacity = far_capacity

    def close(self) -> None:
        return None

    def run(self) -> None:
        for snap in range(self.n_snapshots):
            self._scan_snapshot(snap)

    def far_links(self) -> np.ndarray:
        """Every held link, ordered by target position; the parts are
        released."""
        links = np.empty(sum(part.size for part in self.far_parts), dtype=_FAR_DTYPE)
        start = 0
        while self.far_parts:
            part = self.far_parts.pop(0)
            links[start : start + part.size] = part
            start += part.size
            del part
        links.sort(order=("target", "tag", "owner"))
        return links

    def _emit_topology(self, snap, rows, targets, first_fof, next_fof, forest) -> None:
        """Count the chain links as the external-sort battery does, and hold
        every progenitor link that does not join adjacent snapshots with its
        owner's values, for the pair pass to judge at its target."""
        offsets = self.offsets
        gp = offsets[snap] + rows
        d_index, d_target, d_linked, _ = targets["Descendant"]
        desc_gp = np.where(d_linked, offsets[np.where(d_linked, d_target, 0)] + d_index, -1)
        fp_index, fp_target, fp_linked, _ = targets["FirstProgenitor"]
        np_index, np_target, np_linked, _ = targets["NextProgenitor"]
        self.n_chain_edges += int(np.count_nonzero(np_linked)) + int(
            np.count_nonzero(next_fof != -1)
        )
        for tag, index, target, mask, expected in (
            (_FAR_FP, fp_index, fp_target, fp_linked & (fp_target != snap - 1), gp),
            (_FAR_NP, np_index, np_target, np_linked & (np_target != snap), desc_gp),
        ):
            count = int(np.count_nonzero(mask))
            if not count:
                continue
            self.n_far += count
            if self.n_far * FAR_LINK_BYTES > self.far_capacity:
                raise ConverterError(
                    "the dataset declares links_adjacent 1 but holds more than {} progenitor "
                    "link(s) that do not join adjacent snapshots (from {}); a dataset with any "
                    "such link fails links-adjacent or topology-closure, and holding more "
                    "would exceed the budget's {} B above the ceiling. Run the battery without "
                    "--bounded for its full verdict".format(
                        self.far_capacity // FAR_LINK_BYTES,
                        snapshot_h5_name(snap),
                        self.far_capacity,
                    )
                )
            records = np.empty(count, dtype=_FAR_DTYPE)
            records["target"] = offsets[target[mask]] + index[mask]
            records["tag"] = tag
            records["owner"] = gp[mask]
            records["expected"] = expected[mask]
            records["forest"] = forest[mask]
            self.far_parts.append(records)


# ---------------------------------------------------------------------------
# The pair pass
# ---------------------------------------------------------------------------


def _read_int64(dataset, n: int, block_rows: int) -> np.ndarray:
    """A whole column as int64, read in blocks of at most ``block_rows``."""
    out = np.empty(n, dtype=np.int64)
    for start in range(0, n, block_rows):
        stop = min(start + block_rows, n)
        dataset.read_direct(out, np.s_[start:stop], np.s_[start:stop])
    return out


def _read_positions(halos, link: str, column: str, offsets, n: int, block_rows: int):
    """A snapshot-qualified link as global positions (-1 for none)."""
    out = _read_int64(halos[link], n, block_rows)
    snapshots = halos[column]
    for start in range(0, n, block_rows):
        stop = min(start + block_rows, n)
        part = out[start:stop]
        linked = part >= 0
        part[linked] += offsets[snapshots[start:stop][linked]]
    return out


class _PairPass:
    """Everything that crosses rows: topology closure and chain termination
    per snapshot pair, the source-key and position bitsets, the identity."""

    def __init__(
        self,
        scan: _BoundedRowScan,
        far: np.ndarray,
        *,
        topology: bool,
        identity: bool,
        source_format: str,
        inventory,
        block_rows: int,
        meter: ResidencyMeter,
    ):
        self.scan = scan
        self.directory = scan.directory
        self.counts = scan.counts
        self.offsets = scan.offsets
        self.n_snapshots = scan.n_snapshots
        self.far = far
        self.topology = topology
        self.identity = identity
        self.source_format = source_format
        self.inventory = inventory
        self.block_rows = block_rows
        self.meter = meter
        self.closure = _Failures()
        self.cycles = _Failures()
        self.keys = _Failures()
        self.positions = _Failures()
        self.total = int(inventory["total_halos"])
        self.unsampled = int(inventory["selected_halos"]) == self.total
        self.emitted = 0
        far_np = np.flatnonzero(far["tag"] == _FAR_NP)
        owners = far["owner"][far_np]
        order = np.argsort(owners, kind="stable")
        self.far_np = far_np[order]  # far-link index, by owner position
        del far_np
        self.far_np_owner = owners[order]
        del owners, order
        self.far_np_next = np.full(self.far_np.size, -1, dtype=np.int64)

    # ---- driver ------------------------------------------------------------

    def run(self) -> None:
        source_keys = _Bitset(self.total)
        self.meter.acquire(source_keys.nbytes)
        position_bits = None
        starts = None
        try:
            if self.identity:
                starts = self.scan.forest_totals.counts
                self._identity_totals(starts)
                np.cumsum(starts, out=starts)  # each forest's end position
                position_bits = _Bitset(int(starts[-1]) if starts.size else 0)
                self.meter.acquire(position_bits.nbytes)
            visited = _Bitset(int(self.counts.max()) if self.counts.size else 0)
            self.meter.acquire(visited.nbytes)
            next_forest = None
            for snap in range(self.n_snapshots):
                next_forest = self._snapshot(
                    snap, next_forest, source_keys, position_bits, starts, visited
                )
            self.meter.release(visited.nbytes)
            self._source_key_totals(source_keys)
            if self.topology:
                self._far_chain_cycles()
        finally:
            self.meter.release(source_keys.nbytes)
            if position_bits is not None:
                self.meter.release(position_bits.nbytes)

    def _snapshot(self, snap, current_forest, source_keys, position_bits, ends, visited):
        """One snapshot as the target of every link into it; returns the next
        snapshot's ForestIndex when it was read as this pair's owner."""
        n = int(self.counts[snap])
        if not n:
            return None
        block = self.block_rows
        path = self.directory / snapshot_h5_name(snap)
        with h5py.File(path, "r") as handle:
            halos = handle["halos"]
            if current_forest is None and (self.topology or self.identity):
                current_forest = _read_int64(halos["ForestIndex"], n, block)
                self.meter.acquire(current_forest.nbytes)
            next_forest = None
            if self.topology:
                next_forest = self._topology(snap, halos, current_forest, visited)
            self._keys_and_positions(snap, halos, current_forest, source_keys, position_bits, ends)
        if current_forest is not None:
            self.meter.release(current_forest.nbytes)
        return next_forest

    # ---- topology ------------------------------------------------------------

    def _topology(self, snap, halos, forest, visited):
        n = int(self.counts[snap])
        block = self.block_rows
        offsets = self.offsets
        low, high = int(offsets[snap]), int(offsets[snap + 1])
        columns = {
            "a": _read_positions(halos, "Descendant", "DescendantSnapshot", offsets, n, block),
            "central": _read_int64(halos["FirstHaloInFOFgroup"], n, block),
            "np": _read_positions(
                halos, "NextProgenitor", "NextProgenitorSnapshot", offsets, n, block
            ),
            "nfof": _read_int64(halos["NextHaloInFOFgroup"], n, block),
            "prog_in": np.zeros(n, dtype=np.int32),
            "fof_in": np.zeros(n, dtype=np.int32),
        }
        held = sum(values.nbytes for values in columns.values())
        self.meter.acquire(held)
        next_forest = None
        try:
            a, central = columns["a"], columns["central"]
            np_gp, nfof = columns["np"], columns["nfof"]
            prog_in, fof_in = columns["prog_in"], columns["fof_in"]
            self._judge_own_links(snap, a, central, np_gp, nfof, forest, prog_in, fof_in)
            if snap + 1 < self.n_snapshots and int(self.counts[snap + 1]):
                m = int(self.counts[snap + 1])
                with h5py.File(self.directory / snapshot_h5_name(snap + 1), "r") as owner:
                    owner_halos = owner["halos"]
                    first_progenitor = _read_positions(
                        owner_halos,
                        "FirstProgenitor",
                        "FirstProgenitorSnapshot",
                        offsets,
                        m,
                        block,
                    )
                    next_forest = _read_int64(owner_halos["ForestIndex"], m, block)
                self.meter.acquire(first_progenitor.nbytes + next_forest.nbytes)
                self._judge_first_progenitors(
                    snap, first_progenitor, next_forest, a, forest, prog_in
                )
                self.meter.release(first_progenitor.nbytes)
                del first_progenitor
            self._judge_far_links(snap, a, forest, prog_in)
            self._judge_in_links(snap, a, central, prog_in, fof_in)

            # chain termination: NextProgenitor inside the snapshot, then FoF
            prog_in[:] = 0
            for start in range(0, n, block):
                part = np_gp[start : start + block]
                local = part[(part >= low) & (part < high)] - low
                np.add.at(prog_in, local, 1)

            def progenitor_successor(rows):
                after = np_gp[rows]
                return np.where((after >= low) & (after < high), after - low, -1)

            visited.reset(n)
            stuck = _walk_chains(progenitor_successor, prog_in, block, visited)
            self._record_cycles(snap, "NextProgenitor", stuck, visited)
            self._far_path_ends(snap, np_gp, visited, low, high)
            visited.reset(n)
            stuck = _walk_chains(lambda rows: nfof[rows], fof_in, block, visited)
            self._record_cycles(snap, "NextHaloInFOFgroup", stuck, visited)
        finally:
            self.meter.release(held)
        return next_forest

    def _judge_own_links(self, snap, a, central, np_gp, nfof, forest, prog_in, fof_in):
        """The links a snapshot's halos hold into their own snapshot:
        FirstHaloInFOFgroup, NextHaloInFOFgroup and adjacent NextProgenitor."""
        offsets = self.offsets
        low, high = int(offsets[snap]), int(offsets[snap + 1])
        n = a.size
        failures = self.closure
        for start in range(0, n, self.block_rows):
            stop = min(start + self.block_rows, n)
            rows = np.arange(start, stop, dtype=np.int64)
            owner_gp = low + rows
            owner_forest = forest[start:stop]

            # FirstHaloInFOFgroup: the target is a self-referencing central
            target = central[start:stop]
            bad = central[target] != target
            failures.add(
                "FirstHaloInFOFgroup target(s) that are not self-referencing centrals",
                int(bad.sum()),
                _link_examples(offsets, owner_gp, low + target, bad),
            )
            bad = forest[target] != owner_forest
            failures.add(
                "link(s) crossing forests",
                int(bad.sum()),
                _link_examples(offsets, owner_gp, low + target, bad),
            )

            # NextHaloInFOFgroup: the target shares the owner's central
            values = nfof[start:stop]
            linked = values >= 0
            target = values[linked]
            owners = owner_gp[linked]
            bad = central[target] != central[start:stop][linked]
            failures.add(
                "NextHaloInFOFgroup target(s) in a different FoF group than the owner",
                int(bad.sum()),
                _link_examples(offsets, owners, low + target, bad),
            )
            bad = forest[target] != owner_forest[linked]
            failures.add(
                "link(s) crossing forests",
                int(bad.sum()),
                _link_examples(offsets, owners, low + target, bad),
            )
            np.add.at(fof_in, target, 1)

            # NextProgenitor into the owner's own snapshot: the target names
            # the owner's descendant
            values = np_gp[start:stop]
            linked = (values >= low) & (values < high)
            target = values[linked] - low
            owners = owner_gp[linked]
            bad = a[target] != a[start:stop][linked]
            failures.add(
                "NextProgenitor(s) whose target names a different descendant than the owner",
                int(bad.sum()),
                _link_examples(offsets, owners, low + target, bad),
            )
            bad = forest[target] != owner_forest[linked]
            failures.add(
                "link(s) crossing forests",
                int(bad.sum()),
                _link_examples(offsets, owners, low + target, bad),
            )
            np.add.at(prog_in, target, 1)

    def _judge_first_progenitors(self, snap, first_progenitor, owner_forest, a, forest, prog_in):
        """FirstProgenitor links of the next snapshot's halos into this one:
        the target's Descendant is the owner."""
        offsets = self.offsets
        low, high = int(offsets[snap]), int(offsets[snap + 1])
        owner_low = int(offsets[snap + 1])
        m = first_progenitor.size
        for start in range(0, m, self.block_rows):
            stop = min(start + self.block_rows, m)
            values = first_progenitor[start:stop]
            linked = (values >= low) & (values < high)
            target = values[linked] - low
            owners = owner_low + np.arange(start, stop, dtype=np.int64)[linked]
            bad = a[target] != owners
            self.closure.add(
                "FirstProgenitor(s) whose target's Descendant is not the owner",
                int(bad.sum()),
                _link_examples(offsets, owners, low + target, bad),
            )
            bad = forest[target] != owner_forest[start:stop][linked]
            self.closure.add(
                "link(s) crossing forests",
                int(bad.sum()),
                _link_examples(offsets, owners, low + target, bad),
            )
            np.add.at(prog_in, target, 1)

    def _judge_far_links(self, snap, a, forest, prog_in):
        """The held links whose target lies in this snapshot."""
        low, high = int(self.offsets[snap]), int(self.offsets[snap + 1])
        first = int(np.searchsorted(self.far["target"], low, side="left"))
        last = int(np.searchsorted(self.far["target"], high, side="left"))
        for start in range(first, last, self.block_rows):
            links = self.far[start : min(start + self.block_rows, last)]
            target = links["target"] - low
            owners, targets = links["owner"], links["target"]
            wrong = a[target] != links["expected"]
            for tag, kind in (
                (_FAR_FP, "FirstProgenitor(s) whose target's Descendant is not the owner"),
                (
                    _FAR_NP,
                    "NextProgenitor(s) whose target names a different descendant than the owner",
                ),
            ):
                bad = wrong & (links["tag"] == tag)
                self.closure.add(
                    kind, int(bad.sum()), _link_examples(self.offsets, owners, targets, bad)
                )
            bad = forest[target] != links["forest"]
            self.closure.add(
                "link(s) crossing forests",
                int(bad.sum()),
                _link_examples(self.offsets, owners, targets, bad),
            )
            np.add.at(prog_in, target, 1)

    def _judge_in_links(self, snap, a, central, prog_in, fof_in):
        """Every halo is reached exactly once by its descendant's progenitor
        chain iff it has a Descendant, and by its FoF chain iff it is not its
        group's central."""
        low = int(self.offsets[snap])
        n = a.size
        for start in range(0, n, self.block_rows):
            stop = min(start + self.block_rows, n)
            rows = np.arange(start, stop, dtype=np.int64)
            expected = (a[start:stop] != -1).astype(np.int64)
            reached = prog_in[start:stop]
            bad = reached != expected
            self.closure.add(
                "halo(s) not reached exactly once by their descendant's progenitor chain "
                "(reached k times, expected once iff the halo has a Descendant)",
                int(bad.sum()),
                [
                    "{} (k={})".format(_describe_gp(self.offsets, low + row), int(k))
                    for row, k in zip(rows[bad][:5], reached[bad][:5])
                ],
            )
            expected = (central[start:stop] != rows).astype(np.int64)
            reached = fof_in[start:stop]
            bad = reached != expected
            self.closure.add(
                "halo(s) not reached exactly once by their FoF chain (reached k times, expected "
                "once iff the halo is not its group's central)",
                int(bad.sum()),
                [
                    "{} (k={})".format(_describe_gp(self.offsets, low + row), int(k))
                    for row, k in zip(rows[bad][:5], reached[bad][:5])
                ],
            )

    def _record_cycles(self, snap, chain, stuck, visited) -> None:
        if not stuck:
            return
        self.cycles.add(
            "halo(s) on a {} chain that never terminates (the chain is cyclic)".format(chain),
            stuck,
            ["{} row {}".format(snapshot_h5_name(snap), row) for row in _cycle_rows(visited)],
        )

    def _far_path_ends(self, snap, np_gp, visited, low, high) -> None:
        """For each held NextProgenitor link into this snapshot, where its
        chain leaves the snapshot again: the held link owned by the halo the
        in-snapshot walk ends at, -1 if the chain ends here, -2 if it runs
        into a cycle (already reported)."""
        far = self.far
        first = int(np.searchsorted(far["target"], low, side="left"))
        last = int(np.searchsorted(far["target"], high, side="left"))
        for start in range(first, last, self.block_rows):
            stop = min(start + self.block_rows, last)
            links = np.flatnonzero(far["tag"][start:stop] == _FAR_NP) + start
            if links.size:
                self._far_path_ends_of(links, np_gp, visited, low, high)

    def _far_path_ends_of(self, links, np_gp, visited, low, high) -> None:
        far = self.far
        position = far["target"][links] - low
        result = np.full(links.size, -1, dtype=np.int64)
        active = np.arange(links.size)
        while active.size:
            rows = position[active]
            on_cycle = ~visited.test(rows)
            result[active[on_cycle]] = -2
            active, rows = active[~on_cycle], rows[~on_cycle]
            after = np_gp[rows]
            inside = (after >= low) & (after < high)
            ended = active[~inside]
            leaving = after[~inside] >= 0
            if leaving.any():
                owners = low + rows[~inside][leaving]
                index = np.searchsorted(self.far_np_owner, owners)
                result[ended[leaving]] = index
            position[active[inside]] = after[inside] - low
            active = active[inside]
        # each held NextProgenitor link, by owner order, names the next one
        slot = np.searchsorted(self.far_np_owner, far["owner"][links])
        self.far_np_next[slot] = result

    def _far_chain_cycles(self) -> None:
        """The held NextProgenitor links as one contracted chain graph: a
        cycle there is a chain that crosses snapshots and never ends."""
        n = self.far_np_next.size
        if not n:
            return
        successor = self.far_np_next  # consumed: -2 (runs into a cycle) ends here too
        successor[successor < 0] = -1
        in_links = np.bincount(successor[successor >= 0], minlength=n)
        visited = _Bitset(n)
        stuck = _walk_chains(lambda rows: successor[rows], in_links, self.block_rows, visited)
        if stuck:
            self.cycles.add(
                "halo(s) on a NextProgenitor chain that never terminates (the chain is cyclic)",
                stuck,
                [
                    "{} (crossing snapshots)".format(
                        _describe_gp(self.offsets, int(self.far_np_owner[row]))
                    )
                    for row in visited.clear_indices()
                ],
            )

    # ---- source keys and the identity ------------------------------------

    def _identity_totals(self, totals: np.ndarray) -> None:
        """Before the totals become positions: ASCII forests all hold halos."""
        if self.source_format != "consistent_trees_ascii" or not totals.size:
            return
        top = int(self.scan.stats["max_forest_index"])
        top = min(top, totals.size - 1)
        if top < 0:
            return
        # a window of at most min(block_rows, W) forests (one byte of mask and
        # eight of index each, inside the pair pass's block term), or a single
        # forest when no snapshot holds a halo
        window = max(1, min(self.block_rows, int(self.counts.max()) if self.counts.size else 0))
        count, examples = 0, []
        for start in range(0, top + 1, window):
            empty = np.flatnonzero(totals[start : min(start + window, top + 1)] == 0) + start
            count += int(empty.size)
            examples.extend(int(f) for f in empty[: 5 - len(examples)])
        self.positions.add(
            "ForestIndex value(s) skipping a forest (ASCII forests all hold halos)",
            count,
            ["ForestIndex {} holds no halo".format(f) for f in examples],
        )

    def _keys_and_positions(self, snap, halos, forest, source_keys, position_bits, ends) -> None:
        n = int(self.counts[snap])
        position_identity = (
            self.source_format in _V3_POSITION_IDENTITY
            and int(self.inventory["selected_halos"]) == self.total
        )
        for start in range(0, n, self.block_rows):
            stop = min(start + self.block_rows, n)
            sid = halos["SourceHaloID"][start:stop].astype(np.int64)
            self.emitted += int(sid.size)
            inside = (sid >= 1) & (sid <= self.total)
            outside = sid[~inside]
            self.keys.add(
                "SourceHaloID value(s) outside the inventory's [1, {}]".format(self.total),
                int(outside.size),
                outside.tolist(),
            )
            within, seen = source_keys.first_and_seen(sid[inside] - 1)
            repeats = np.sort(np.concatenate((within, seen))) + 1
            self.keys.add("duplicated SourceHaloID value(s)", int(repeats.size), repeats.tolist())
            if position_bits is None or not ends.size:
                # no forest to place a halo in: every ForestIndex is out of
                # range, which the identity check reports by name
                continue
            block_forest = forest[start:stop]
            rank = halos["HaloRankInForest"][start:stop].astype(np.int64)
            known = (block_forest >= 0) & (block_forest < ends.size)
            safe = np.where(known, block_forest, 0)
            end = ends[safe]
            begin = np.where(safe > 0, ends[np.maximum(safe - 1, 0)], 0)
            ranked = known & (rank >= 0) & (rank < end - begin)
            bad = known & ~ranked
            position = begin + rank
            within, seen = position_bits.first_and_seen(position[ranked])
            repeats = np.concatenate((within, seen))
            count = int(bad.sum()) + int(repeats.size)
            bad |= ranked & np.isin(position, repeats)
            self.positions.add(
                "(ForestIndex, HaloRankInForest) pair(s) breaking per-forest rank density or "
                "uniqueness",
                count,
                [
                    "(ForestIndex={}, rank={})".format(f, r)
                    for f, r in zip(block_forest[bad][:5], rank[bad][:5])
                ],
            )
            if position_identity:
                expected = position + 1
                bad = ranked & (sid != expected)
                self.positions.add(
                    "halo(s) whose SourceHaloID is not its (ForestIndex, rank) position in the "
                    "source order",
                    int(bad.sum()),
                    [
                        "(ForestIndex={}, rank={}) has SourceHaloID {}, expected {}".format(
                            f, r, s, e
                        )
                        for f, r, s, e in zip(
                            block_forest[bad][:5], rank[bad][:5], sid[bad][:5], expected[bad][:5]
                        )
                    ],
                )

    def _source_key_totals(self, source_keys: _Bitset) -> None:
        if not self.unsampled:
            return
        missing = self.total - source_keys.count()
        if missing > 0:
            self.keys.add(
                "gap(s) in SourceHaloID coverage of the unsampled inventory",
                missing,
                ["{} is missing".format(index + 1) for index in source_keys.clear_indices()],
            )

    def source_key_messages(self) -> List[str]:
        messages = self.keys.messages()
        if self.emitted != int(self.inventory["selected_halos"]):
            messages.append(
                "{} SourceHaloID value(s) emitted; the inventory selects {}".format(
                    self.emitted, self.inventory["selected_halos"]
                )
            )
        return messages

    def identity_messages(self, n_forests_total: int, max_rank: int) -> List[str]:
        messages = self.positions.messages()
        inventory = self.inventory
        unsampled = int(inventory["selected_halos"]) == int(inventory["total_halos"])
        if self.source_format == "consistent_trees_ascii" and not unsampled:
            messages.append(
                "consistent_trees_ascii inventory selects {} of {} halos; the ASCII route does "
                "not sample".format(inventory["selected_halos"], inventory["total_halos"])
            )
        last = int(self.scan.stats["max_forest_index"])
        if self.source_format == "consistent_trees_ascii" and last != n_forests_total - 1:
            messages.append(
                "ForestIndex values end at {} but n_forests_total is {}".format(
                    last, n_forests_total
                )
            )
        measured_max = int(self.scan.stats["max_halo_rank_in_forest"])
        if measured_max != max_rank:
            messages.append(
                "measured max HaloRankInForest {} != header max_halo_rank_in_forest {}".format(
                    measured_max, max_rank
                )
            )
        return messages


# ---------------------------------------------------------------------------
# Battery driver
# ---------------------------------------------------------------------------


def _preflight(directory: Path, n_snapshots: int) -> List[Dict[str, object]]:
    """What the ceiling and the refusals need, from each readable snapshot
    file's header and dataset shapes: nothing is read beyond them. A file
    that is missing or does not open, and a file whose SourceHaloID is not a
    one-dimensional dataset, contribute no rows: they are left to the
    structural checks (a declared links_adjacent is still read)."""
    found = []
    for snap in range(n_snapshots):
        path = directory / snapshot_h5_name(snap)
        if not path.is_file():
            continue
        try:
            with h5py.File(path, "r") as handle:
                attrs = handle["header"].attrs
                halos = handle["halos"]
                entry: Dict[str, object] = {"name": path.name}
                if "links_adjacent" in attrs:
                    entry["links_adjacent"] = np.asarray(attrs["links_adjacent"])[()].item()
                if "n_forests_total" in attrs:
                    entry["n_forests_total"] = int(np.asarray(attrs["n_forests_total"])[()])
                key = halos.get("SourceHaloID")
                if isinstance(key, h5py.Dataset) and key.ndim == 1:
                    entry["rows"] = int(key.shape[0])
                entry["row_bytes"] = sum(
                    int(halos[name].dtype.itemsize) * int(np.prod(halos[name].shape[1:]))
                    for name in halos
                    if isinstance(halos.get(name), h5py.Dataset)
                )
        except (OSError, KeyError, TypeError, ValueError):
            continue
        found.append(entry)
    return found


def _refuse_gapped(preflight: List[Dict[str, object]]) -> None:
    declared = [
        "{} declares links_adjacent {!r}".format(entry["name"], entry["links_adjacent"])
        for entry in preflight
        if "links_adjacent" in entry and entry["links_adjacent"] != 1
    ]
    if declared:
        raise ConverterError(
            "the bounded battery validates adjacent datasets (links_adjacent 1) only, and {} "
            "file(s) are not: {}. Run the battery without --bounded, whose external sorts "
            "handle gapped links".format(len(declared), "; ".join(declared[:5]))
        )


def run_battery_bounded(
    directory,
    a_list_path,
    manifest_path=None,
    multiplier: int = DEFAULT_MULTIPLIER,
    *,
    budget_bytes: int = DEFAULT_V3_BUDGET_BYTES,
    spill_dir=None,
    block_rows: int = DEFAULT_BLOCK_ROWS,
) -> V3BatteryResult:
    """Run the version 3 producer battery over one adjacent dataset within
    the ceiling the module docstring states.

    Refused with :class:`ConverterError`, before any long pass: no
    ``manifest_path`` or a manifest that does not load, an inventory without
    halo totals, a file declaring ``links_adjacent`` other than 1, and a
    ceiling above ``budget_bytes``. ``spill_dir`` is accepted for the
    external-sort battery's interface and never written.
    """
    del spill_dir  # nothing is spilled
    budget_bytes = _require_budget(budget_bytes)
    block_rows = _require_block_rows(block_rows)
    directory = Path(directory)
    if not directory.is_dir():
        raise ConverterError("{}: not a directory".format(directory))
    a_list, _ = load_a_list(a_list_path)
    n_snapshots = len(a_list)
    if manifest_path is None:
        raise ConverterError(
            "the bounded battery needs the conversion manifest: its inventory sizes the "
            "SourceHaloID bitset"
        )
    try:
        manifest = _v3_load_manifest(Path(manifest_path))
    except (ConverterError, OSError, ValueError, KeyError) as exc:
        raise ConverterError(
            "cannot load the conversion manifest ({}); the bounded battery sizes its "
            "SourceHaloID bitset from its inventory".format(exc)
        ) from exc
    inventory = manifest.inventory or {}
    try:
        total_halos = int(inventory["total_halos"])
        int(inventory["selected_halos"])
    except (KeyError, TypeError, ValueError):
        raise ConverterError(
            "the conversion manifest's inventory records no total_halos and selected_halos; "
            "the bounded battery sizes its SourceHaloID bitset from them"
        ) from None

    preflight = _preflight(directory, n_snapshots)
    _refuse_gapped(preflight)
    rows = [int(entry["rows"]) for entry in preflight if "rows" in entry]
    ceiling = Ceiling(
        widest_rows=max(rows, default=0),
        n_halos=sum(rows),
        total_halos=total_halos,
        n_forests_total=max((int(e.get("n_forests_total", 0)) for e in preflight), default=0),
        row_bytes=max((int(entry["row_bytes"]) for entry in preflight), default=0),
        block_rows=block_rows,
    )
    print(
        "bounded battery: {}; budget {} B".format(ceiling.describe(), budget_bytes), file=sys.stderr
    )
    if ceiling.total > budget_bytes:
        raise ConverterError(
            "the bounded battery's ceiling {} B exceeds the budget of {} B (--memory-budget-mb "
            "{}): {}".format(ceiling.total, budget_bytes, budget_bytes >> 20, ceiling.describe())
        )

    outcomes: List[Outcome] = []
    measurements: Dict[str, object] = {
        "budget_bytes": budget_bytes,
        "battery_mode": "bounded",
        "ceiling": ceiling.as_dict(),
        "block_rows": block_rows,
    }

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

    record("manifest-binding", _v3_manifest_binding(directory, a_list_path, a_list, manifest))

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

    meter = ResidencyMeter()
    scan = _BoundedRowScan(
        directory,
        counts,
        [header["box_size_mpc_h"] for header in headers],
        declarations[0],
        n_forests_total,
        block_rows,
        budget_bytes - ceiling.total,
        meter,
    )
    totals = scan.forest_totals.counts
    totals_bytes = 0 if totals is None else int(totals.nbytes)
    block = min(block_rows, max(counts, default=0))
    row_block_bytes = block * (4 * ceiling.row_bytes + ROW_BLOCK_EXTRA_BYTES_PER_ROW)
    meter.acquire(totals_bytes + row_block_bytes)
    scan.run()
    meter.release(row_block_bytes)
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
    record(
        "position-bounds",
        _v3_position_bounds(scan, headers, declarations[0]),
        detail="every Pos component within [0, box_size_mpc_h]",
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

    far = scan.far_links() if links_ok else np.empty(0, dtype=_FAR_DTYPE)
    meter.acquire(far.size * FAR_LINK_BYTES)
    pair = _PairPass(
        scan,
        far,
        topology=links_ok,
        identity=run_scoped_ok,
        source_format=source_format,
        inventory=inventory,
        block_rows=block_rows,
        meter=meter,
    )
    pair_block_bytes = block * PAIR_BLOCK_BYTES_PER_ROW
    meter.acquire(pair_block_bytes)
    pair.run()
    meter.release(pair_block_bytes + totals_bytes + far.size * FAR_LINK_BYTES)

    if links_ok:
        record("links-adjacent", adjacency)
        record("topology-closure", pair.closure.messages())
        record(
            "chain-cycles",
            pair.cycles.messages(),
            detail="{} chain link(s) walked from their heads".format(scan.n_chain_edges),
        )
    else:
        skip(
            ("links-adjacent", "topology-closure", "chain-cycles"),
            "link targets invalid; topology not walked",
        )
    record("source-key-coverage", pair.source_key_messages())
    if run_scoped_ok:
        record(
            "identity",
            scan.identity_ranges.messages() + pair.identity_messages(n_forests_total, max_rank),
        )
        record("header-bounds", check_header_bounds(n_forests_total, max_rank, multiplier))
        record(
            "sidecar-content",
            _v3_sidecar_content(
                directory / "forests.h5", source_format, n_forests_total, inventory, block_rows
            ),
        )
    else:
        skip(("identity", "header-bounds", "sidecar-content"), "run-scoped headers inconsistent")
    record("count-conservation", _v3_count_conservation(counts, manifest))
    measurements.update(scan.stats)
    measurements.update(
        len_zero=scan.len_zero,
        links_adjacent_measured=measured_adjacent,
        chain_edges=scan.n_chain_edges,
        scan_rows=block_rows,
        far_links=int(far.size),
        peak_resident_bytes=int(meter.bytes_peak),
        peak_spill_bytes=0,
    )
    return V3BatteryResult(_ordered(outcomes), measurements)


# ---------------------------------------------------------------------------
# The ceiling, from the command line
# ---------------------------------------------------------------------------


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(
        prog="validate_bounded",
        description="Evaluate the bounded battery's storage ceiling for a dataset's numbers "
        "(the battery itself prints the same figure from the dataset it is given).",
    )
    sub = parser.add_subparsers(dest="command", required=True)
    ceiling = sub.add_parser("ceiling", help="print the ceiling, term by term")
    ceiling.add_argument("--widest-rows", type=int, required=True, help="rows of the widest slab")
    ceiling.add_argument("--n-halos", type=int, required=True, help="the dataset's halos")
    ceiling.add_argument(
        "--total-halos",
        type=int,
        default=None,
        help="the inventory's total_halos (default: --n-halos)",
    )
    ceiling.add_argument("--n-forests-total", type=int, required=True)
    ceiling.add_argument(
        "--row-bytes",
        type=int,
        default=140,
        help="bytes of one row of every /halos column (default 140, the default schema's)",
    )
    ceiling.add_argument("--block-rows", type=int, default=DEFAULT_BLOCK_ROWS)
    args = parser.parse_args(argv)
    result = Ceiling(
        widest_rows=args.widest_rows,
        n_halos=args.n_halos,
        total_halos=args.n_halos if args.total_halos is None else args.total_halos,
        n_forests_total=args.n_forests_total,
        row_bytes=args.row_bytes,
        block_rows=_require_block_rows(args.block_rows),
    )
    print(result.describe())
    print(
        "ceiling_bytes {} ({:.2f} GB, {:.2f} GiB; --memory-budget-mb {} or more)".format(
            result.total,
            result.total / 1e9,
            result.total / (1 << 30),
            math.ceil(result.total / (1 << 20)),
        )
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
