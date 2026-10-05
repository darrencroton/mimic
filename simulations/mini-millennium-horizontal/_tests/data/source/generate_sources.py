#!/usr/bin/env python3
"""Regenerate the L-Halo binary sources behind the gap-retention fixtures.

Run from the repository root:

    mimic_venv/bin/python simulations/mini-millennium-horizontal/_tests/data/source/generate_sources.py

Deterministic and idempotent: re-running reproduces byte-identical source files.
The record layout is taken from
``convert/mimic-convert/adapters/source_inventory.LHALO_FIELDS`` (the shipped 104-byte
L-Halo record), not hand-counted. ``../regenerate.sh`` converts each source with
``convert/mimic-convert/convert_trees.py`` into the dataset directory of the same name.

The first three sources are one L-Halo file holding one tree, and each tree is
one retention case the horizontal driver must handle:

- ``trees_worked_graph.0`` -- the worked five-halo mixed-gap graph, the
  reference case the retention tests pin by hand. Snapshots 0-4; E(4) <- D(2) <-
  {A(0), B(1), C(1)}; A -> D and D -> E are gaps of span 2, D -> E crossing the
  empty snapshot 3; at snapshot 1 B is the FoF central and C its satellite. D's
  chain spans two snapshots, A's NextProgenitor points forward to B, B's
  NextProgenitor stays inside snapshot 1, and snapshot 1 holds no halo with a
  progenitor while snapshot 0 is retained.
- ``trees_three_snapshot_chain.0`` -- snapshots 0-3; D(3) has progenitors P0(0),
  P1(1) and P2(2), chained in that order, so one progenitor chain spans three
  snapshots and every generation is retained until snapshot 3. Every progenitor
  sits at row 0 of its own slab, so a lookup that named a progenitor by row alone
  would mistake P1 and P2 for the main branch. P0 is the least massive
  progenitor but occupied, so it is the main branch (the vertical path's pin).
- ``trees_adjacent.0`` -- snapshots 0-2 with every link adjacent, so the
  converter stamps ``links_adjacent = 1``: Z(0) -> Y(1) -> X(2) and W(1) -> X(2),
  with Y's NextProgenitor W in the same snapshot as Y.

Masses (Len and M_Crit200) follow the order the design review gives, so the
most-massive-progenitor rule and the FoF structure are the only things that
decide the expected inheritance.

The fourth source, ``trees_forest_blocks.0``, is the fixture of the distributed
identity gate (``make tests-distributed``): one L-Halo file holding six trees of
unequal size, so six forests and six ``ForestIndex`` blocks per slab, over
snapshots 0-6 with snapshot 3 empty. Every link that crosses snapshot 3 is a gap
of span 2, and one branch also skips snapshot 5. Tree 2 holds at least 40% of
the widest snapshot's halos (a dominant forest the partition cannot split); FoF
satellites merge into their centrals' branches and so become orphans; the final
snapshot holds more than three FoF groups. The trees are written as branches
(``FOREST_BLOCKS``) and expanded into L-Halo links by ``expand_branches``;
``check_forest_blocks`` asserts every property the gate relies on, so an edit
that loses one fails here rather than in the gate. Masses span 1e11 to 2.5e13
Msun/h and Vmax grows with mass, so ``sage16`` forms galaxies, ``sham`` has
candidates above its Vpeak floor and ``hod`` draws satellites in the heaviest
host.

The fifth source, ``trees_wide_slab.0``, is the multi-block reader fixture: one L-Halo
file holding one tree over two snapshots, where snapshot 0 holds ``WIDE_SLAB_ROWS`` halos
(more than one ``HORIZONTAL_HDF5_SCAN_BLOCK`` of 8,192 rows, so range reads and the
``ForestIndex`` scan cross a block boundary) and snapshot 1 holds one halo. Snapshot 0 is one
FoF group chained through ``NextHaloInFOFgroup`` and every halo descends to the snapshot-1
halo, whose progenitors are chained in index order with masses falling along the chain.
"""

import os
import struct
import sys
from pathlib import Path

_REPO = Path(__file__).resolve().parents[5]
sys.path.insert(0, str(_REPO / "convert" / "mimic-convert"))
from adapters.source_inventory import LHALO_FIELDS, LHALO_RECORD_BYTES  # noqa: E402

SOURCE_DIR = Path(__file__).parent

_KIND_TO_STRUCT = {"i4": "i", "f4": "f", "i8": "q"}

_FIELD_FORMAT = "<" + "".join(
    _KIND_TO_STRUCT[kind] * (shape[0] if shape else 1) for _name, kind, shape in LHALO_FIELDS
)

# One tree per source. Each halo is
# (snap, (Descendant, FirstProgenitor, NextProgenitor, FirstFoF, NextFoF), mass)
# with within-tree indices, in the tree's stored (descendant-first) order.
TREES = {
    "trees_worked_graph": [
        (4, (-1, 1, -1, 0, -1), 60),  # E
        (2, (0, 2, -1, 1, -1), 50),  # D
        (0, (1, -1, 3, 2, -1), 30),  # A
        (1, (1, -1, 4, 3, 4), 20),  # B, FoF central at snapshot 1
        (1, (1, -1, -1, 3, -1), 10),  # C, satellite of B
    ],
    "trees_three_snapshot_chain": [
        (3, (-1, 1, -1, 0, -1), 100),  # D
        (0, (0, -1, 2, 1, -1), 20),  # P0
        (1, (0, -1, 3, 2, -1), 50),  # P1
        (2, (0, -1, -1, 3, -1), 30),  # P2
    ],
    "trees_adjacent": [
        (2, (-1, 1, -1, 0, -1), 90),  # X
        (1, (0, 2, 3, 1, -1), 60),  # Y
        (0, (1, -1, -1, 2, -1), 40),  # Z
        (1, (0, -1, -1, 3, -1), 25),  # W
    ],
}


# The forest_blocks fixture: snapshots 0-6, snapshot 3 empty (no branch lists it).
FOREST_BLOCKS_SNAPSHOTS = 7
FOREST_BLOCKS_EMPTY_SNAPSHOT = 3
FOREST_BLOCKS_PARTICLE_MASS = 0.0860657  # mini-Millennium particle mass [1e10 Msun/h]
FOREST_BLOCKS_BOX = 62.5  # mini-Millennium box size [Mpc/h]

# Six trees, each a list of branches
# (name, snapshots, first mass, last mass, {snapshot: FoF central branch}, merges into).
# A branch is one halo per listed snapshot. Each halo's Descendant is the branch's
# next halo; the last halo's is the merge target's first halo after it, or -1 on a
# branch that reaches snapshot 6. A branch is its own FoF central at every snapshot
# missing from its host map. Masses are M_Crit200 in 1e10 Msun/h, interpolated
# geometrically along the branch.
FOREST_BLOCKS = [
    [  # tree 0: a central and a satellite born at snapshot 1
        ("H", (0, 1, 2, 4, 5, 6), 300.0, 800.0, {}, None),
        ("I", (1, 2, 4, 5, 6), 50.0, 70.0, {5: "H", 6: "H"}, None),
    ],
    [  # tree 1: the smallest, born after the empty snapshot
        ("N", (4, 5, 6), 25.0, 35.0, {}, None),
    ],
    [  # tree 2: the dominant forest; C merges into A, D skips snapshot 5
        ("A", (0, 1, 2, 4, 5, 6), 400.0, 1500.0, {}, None),
        ("B", (0, 1, 2, 4, 5, 6), 100.0, 250.0, {4: "A", 5: "A", 6: "A"}, None),
        ("C", (1, 2, 4, 5), 80.0, 95.0, {2: "A", 4: "A", 5: "A"}, "A"),
        ("D", (2, 4, 6), 60.0, 90.0, {}, None),
        ("E", (4, 5, 6), 30.0, 40.0, {6: "D"}, None),
        ("F", (5, 6), 20.0, 24.0, {5: "A", 6: "A"}, None),
        ("G", (4, 5, 6), 18.0, 21.0, {4: "A", 5: "A", 6: "A"}, None),
        ("R", (6,), 15.0, 15.0, {6: "A"}, None),
        ("S", (5, 6), 12.0, 13.0, {5: "E", 6: "D"}, None),
    ],
    [  # tree 3: a satellite that merges into its central at snapshot 6
        ("J", (2, 4, 5, 6), 150.0, 260.0, {}, None),
        ("K", (4, 5), 40.0, 45.0, {5: "J"}, "J"),
    ],
    [  # tree 4: the heaviest host, for hod satellites
        ("L", (0, 1, 2, 4, 5, 6), 1000.0, 2500.0, {}, None),
        ("M", (5, 6), 90.0, 110.0, {5: "L", 6: "L"}, None),
    ],
    [  # tree 5: an early merger and a second group born at snapshot 5
        ("O", (0, 1, 2, 4, 5, 6), 70.0, 120.0, {}, None),
        ("P", (0, 1), 30.0, 32.0, {0: "O", 1: "O"}, "O"),
        ("Q", (5, 6), 45.0, 55.0, {}, None),
        ("T", (4, 5, 6), 22.0, 26.0, {6: "Q"}, None),
    ],
]

# The wide_slab fixture: snapshot 0 is wider than one reader scan block (8,192 rows).
WIDE_SLAB_ROWS = 8600
WIDE_SLAB_SNAPSHOTS = 2


def wide_slab_tree(rows=WIDE_SLAB_ROWS):
    """Build the wide_slab tree: ``rows`` snapshot-0 halos descending to one snapshot-1 halo.

    Stored descendant-first, so index 0 is the snapshot-1 halo and indices 1..``rows`` are
    the snapshot-0 halos. They form one FoF group (central index 1, members chained in index
    order) and one progenitor chain, with the mass falling along the chain so the central is
    the most massive halo and the most massive progenitor. Masses are M_Crit200 in
    1e10 Msun/h, well inside the range every model's thresholds accept.
    """
    tree = [(1, (-1, 1, -1, 0, -1), 250.0)]
    for index in range(1, rows + 1):
        links = (
            0,  # Descendant: the snapshot-1 halo
            -1,  # FirstProgenitor
            index + 1 if index < rows else -1,  # NextProgenitor
            1,  # FirstHaloInFOFgroup: the central
            index + 1 if index < rows else -1,  # NextHaloInFOFgroup
        )
        tree.append((0, links, 200.0 - 0.01 * index))
    return tree


def expand_branches(branches):
    """Expand one tree's branches into ``(snap, links, mass)`` halos in stored order.

    Halos are stored by descending snapshot; within a snapshot each FoF group is
    contiguous, groups by descending central mass, the central first and its
    satellites by descending mass. A halo's progenitors are chained by
    descending mass, so FirstProgenitor is the most massive one.
    """
    by_name = {branch[0]: branch for branch in branches}
    halos = {}  # (branch, snap) -> (mass, descendant key or None, central key)
    for name, snaps, first, last, hosts, merge in branches:
        for i, snap in enumerate(snaps):
            fraction = i / (len(snaps) - 1) if len(snaps) > 1 else 0.0
            mass = first * (last / first) ** fraction
            if i + 1 < len(snaps):
                descendant = (name, snaps[i + 1])
            elif merge is not None:
                descendant = (merge, min(t for t in by_name[merge][1] if t > snap))
            else:
                descendant = None
            halos[(name, snap)] = (mass, descendant, (hosts.get(snap, name), snap))

    def stored_order(key):
        mass, _descendant, central = halos[key]
        return (-key[1], -halos[central][0], central != key, -mass)

    keys = sorted(halos, key=stored_order)
    index = {key: i for i, key in enumerate(keys)}
    progenitors = {key: [] for key in keys}
    members = {key: [] for key in keys}
    for key in keys:  # stored order, so each member list is already in FoF order
        _mass, descendant, central = halos[key]
        assert central in halos and halos[central][2] == central, f"{key}: bad host {central}"
        if descendant is not None:
            progenitors[descendant].append(key)
        members[central].append(key)

    def following(sequence, key):
        position = sequence.index(key)
        return index[sequence[position + 1]] if position + 1 < len(sequence) else -1

    tree = []
    for key in keys:
        mass, descendant, central = halos[key]
        progs = sorted(progenitors[key], key=lambda k: -halos[k][0])
        siblings = sorted(progenitors[descendant], key=lambda k: -halos[k][0]) if descendant else []
        links = (
            index[descendant] if descendant else -1,
            index[progs[0]] if progs else -1,
            following(siblings, key) if siblings else -1,
            index[central],
            following(members[central], key),
        )
        tree.append((key[1], links, mass))
    return tree


def was_central(tree, index: int) -> bool:
    """Whether a halo's main branch (its FirstProgenitor chain) holds an earlier FoF central."""
    progenitor = tree[index][1][1]
    while progenitor >= 0:
        if tree[progenitor][1][3] == progenitor:
            return True
        progenitor = tree[progenitor][1][1]
    return False


def check_forest_blocks(trees) -> None:
    """Assert every property of the forest_blocks fixture the distributed gate relies on."""
    last = FOREST_BLOCKS_SNAPSHOTS - 1
    sizes = [len(tree) for tree in trees]
    assert len(trees) == 6 and len(set(sizes)) == 6, f"six trees of unequal size: {sizes}"
    counts = [[0] * FOREST_BLOCKS_SNAPSHOTS for _ in trees]
    gapped = orphans = final_groups = 0
    for t, tree in enumerate(trees):
        for i, (snap, (desc, _first_prog, _next_prog, fof, _next_fof), _mass) in enumerate(tree):
            counts[t][snap] += 1
            if desc >= 0 and tree[desc][0] > snap + 1:
                gapped += 1
            # A FoF satellite that is not its descendant's main progenitor and was
            # its own FoF central earlier on its main branch, so it carries a
            # galaxy: its halo ends here and that galaxy continues as an orphan.
            if fof != i and desc >= 0 and tree[desc][1][1] != i and was_central(tree, i):
                orphans += 1
            if snap == last and fof == i:
                final_groups += 1
    totals = [sum(row[s] for row in counts) for s in range(FOREST_BLOCKS_SNAPSHOTS)]
    assert totals[FOREST_BLOCKS_EMPTY_SNAPSHOT] == 0, totals
    assert all(n > 0 for s, n in enumerate(totals) if s != FOREST_BLOCKS_EMPTY_SNAPSHOT), totals
    widest = totals.index(max(totals))  # ties to the lowest snapshot, as the partition's
    share = max(row[widest] for row in counts) / totals[widest]
    assert share >= 0.4, f"largest forest holds {share:.0%} of snapshot {widest}"
    assert gapped >= 1 and orphans >= 1, (gapped, orphans)
    assert final_groups >= 3, final_groups


def record(index: int, snap: int, links, mass: int, serial=None) -> bytes:
    """One little-endian L-Halo record; every payload value is distinct per halo.

    The single-tree fixtures key their payload on the within-tree index and use
    the mass as the particle count. The forest_blocks fixture passes ``serial``,
    the halo's position in the file, so payloads stay distinct across trees, and
    derives a physical payload from the mass instead: Len from the particle mass,
    Vmax growing with mass, and positions wrapped into the box.
    """
    if serial is None:
        serial = index + 1
        length, vmax = mass, 40.0 + serial
        pos = (1.0 * serial, 2.0 * serial, 3.0 * serial)
    else:
        length = int(round(mass / FOREST_BLOCKS_PARTICLE_MASS))
        vmax = 40.0 * (mass / 10.0) ** (1.0 / 3.0) + 0.01 * serial
        pos = tuple((k * 7.3 * serial) % FOREST_BLOCKS_BOX for k in (1.0, 2.0, 3.0))
    values = {
        "Descendant": links[0],
        "FirstProgenitor": links[1],
        "NextProgenitor": links[2],
        "FirstHaloInFOFgroup": links[3],
        "NextHaloInFOFgroup": links[4],
        "Len": length,
        "M_Mean200": 0.5 * mass,
        "M_Crit200": 1.0 * mass,
        "M_TopHat": 0.75 * mass,
        "Pos": pos,
        "Vel": (-10.0 * serial, 11.0 * serial, 12.5 * serial),
        "VelDisp": 30.0 + serial,
        "Vmax": vmax,
        "Spin": (0.125 * serial, -0.25 * serial, 0.5 * serial),
        "MostBoundID": 1000 + serial,
        "SnapNum": snap,
        "FileNr": 0,
        "SubhaloIndex": index,
        "SubHalfMass": 0.0625 * serial,
    }
    packed = []
    for name, _kind, shape in LHALO_FIELDS:
        value = values[name]
        if shape:
            packed.extend(value)
        else:
            packed.append(value)
    data = struct.pack(_FIELD_FORMAT, *packed)
    assert len(data) == LHALO_RECORD_BYTES, len(data)
    return data


def write_source(name: str, tree) -> None:
    path = SOURCE_DIR / f"{name}.0"
    with open(path, "wb") as handle:
        handle.write(struct.pack("<ii", 1, len(tree)))
        handle.write(struct.pack("<i", len(tree)))
        for index, (snap, links, mass) in enumerate(tree):
            handle.write(record(index, snap, links, mass))
    print("wrote {}".format(os.path.relpath(path, _REPO)))


def write_forest_blocks(name: str, trees) -> None:
    """Write several trees into one L-Halo file, numbering halos across the file."""
    path = SOURCE_DIR / f"{name}.0"
    with open(path, "wb") as handle:
        handle.write(struct.pack("<ii", len(trees), sum(len(tree) for tree in trees)))
        handle.write(struct.pack(f"<{len(trees)}i", *(len(tree) for tree in trees)))
        serial = 0
        for tree in trees:
            for index, (snap, links, mass) in enumerate(tree):
                serial += 1
                handle.write(record(index, snap, links, mass, serial=serial))
    print("wrote {}".format(os.path.relpath(path, _REPO)))


def main() -> int:
    for name, tree in TREES.items():
        write_source(name, tree)
    forest_blocks = [expand_branches(branches) for branches in FOREST_BLOCKS]
    check_forest_blocks(forest_blocks)
    write_forest_blocks("trees_forest_blocks", forest_blocks)
    write_forest_blocks("trees_wide_slab", [wide_slab_tree()])
    return 0


if __name__ == "__main__":
    sys.exit(main())
