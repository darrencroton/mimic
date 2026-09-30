#!/usr/bin/env python3
"""Regenerate the L-Halo binary sources behind the gap-retention fixtures.

Run from the repository root:

    mimic_venv/bin/python simulations/mini-millennium-horizontal/_tests/data/source/generate_sources.py

Deterministic and idempotent: re-running reproduces byte-identical source files.
The record layout is taken from
``convert/mimic-convert/adapters/source_inventory.LHALO_FIELDS`` (the shipped 104-byte
L-Halo record), not hand-counted. ``../regenerate.sh`` converts each source with
``convert/mimic-convert/convert_trees.py`` into the dataset directory of the same name.

Each source is one L-Halo file holding one tree, and each tree is one retention
case the horizontal driver must handle:

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


def record(index: int, snap: int, links, mass: int) -> bytes:
    """One little-endian L-Halo record; every payload value is distinct per halo."""
    serial = index + 1
    values = {
        "Descendant": links[0],
        "FirstProgenitor": links[1],
        "NextProgenitor": links[2],
        "FirstHaloInFOFgroup": links[3],
        "NextHaloInFOFgroup": links[4],
        "Len": mass,
        "M_Mean200": 0.5 * mass,
        "M_Crit200": 1.0 * mass,
        "M_TopHat": 0.75 * mass,
        "Pos": (1.0 * serial, 2.0 * serial, 3.0 * serial),
        "Vel": (-10.0 * serial, 11.0 * serial, 12.5 * serial),
        "VelDisp": 30.0 + serial,
        "Vmax": 40.0 + serial,
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


def main() -> int:
    for name, tree in TREES.items():
        write_source(name, tree)
    return 0


if __name__ == "__main__":
    sys.exit(main())
