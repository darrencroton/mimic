#!/usr/bin/env python3
"""Regenerate the L-Halo binary source fixture behind tests/data/horizontal_v3.

Run from the repository root:

    mimic_venv/bin/python tests/data/horizontal_v3/source/generate_source.py

Deterministic and idempotent: re-running reproduces a byte-identical
``trees_fixture.0``. The record layout is taken from
``scripts/convert/adapters/source_inventory.LHALO_FIELDS`` (the shipped 104-byte
L-Halo record), not hand-counted.

The fixture is one L-Halo file of two trees over four snapshots (0-3), the
fewest that carry every topology case the v3 reader must accept:

- tree 0, halo 0 (snapshot 0) -> halo 2 (snapshot 3): a forward descendant gap
  of span 3, across the empty snapshot 2, which no halo of either tree occupies;
- tree 0, halo 1 (snapshot 1) -> halo 2 (snapshot 3): a gap of span 2;
- tree 0, halo 0 (snapshot 0) has NextProgenitor halo 1 (snapshot 1): a
  cross-snapshot sibling that names the same descendant (halo 2), whose
  FirstProgenitor (halo 0) is itself three snapshots back;
- tree 0, halo 3 (snapshot 3) is a FoF satellite of halo 2;
- tree 1 is an adjacent snapshot 0 -> 1 pair plus a root at snapshot 3.

Only three snapshots hold halos: the empty snapshot 2 is what carries the
span-3 descendant gap, a topology choice rather than a size constraint.

Every payload value is distinct per halo so field-by-field slab checks are
meaningful, and SubHalfMass (the one selected extra in ../profile.yaml) is
non-zero. MostBoundID repeats and goes negative on purpose: version 3 carries
it as signed, non-unique source data.
"""

import os
import struct
import sys
from pathlib import Path

_REPO = Path(__file__).resolve().parents[4]
sys.path.insert(0, str(_REPO / "scripts" / "convert"))
from adapters.source_inventory import LHALO_FIELDS, LHALO_RECORD_BYTES  # noqa: E402

OUT_PATH = Path(__file__).parent / "trees_fixture.0"

_KIND_TO_STRUCT = {"i4": "i", "f4": "f", "i8": "q"}


def _field_format() -> str:
    return "".join(
        _KIND_TO_STRUCT[kind] * (shape[0] if shape else 1) for _name, kind, shape in LHALO_FIELDS
    )


_FIELD_FORMAT = "<" + _field_format()


def record(tree: int, index: int, snap: int, links, most_bound_id: int) -> bytes:
    """One little-endian L-Halo record. ``links`` is (Descendant,
    FirstProgenitor, NextProgenitor, FirstHaloInFOFgroup, NextHaloInFOFgroup)
    as within-tree indices."""
    serial = 10 * tree + index + 1  # distinct, positive, per halo
    values = {
        "Descendant": links[0],
        "FirstProgenitor": links[1],
        "NextProgenitor": links[2],
        "FirstHaloInFOFgroup": links[3],
        "NextHaloInFOFgroup": links[4],
        "Len": 20 + serial,
        "M_Mean200": 0.5 * serial,
        "M_Crit200": 1.25 * serial,
        "M_TopHat": 0.75 * serial,
        "Pos": (1.0 * serial, 2.0 * serial, 3.0 * serial),
        "Vel": (-10.0 * serial, 11.0 * serial, 12.5 * serial),
        "VelDisp": 30.0 + serial,
        "Vmax": 40.0 + serial,
        "Spin": (0.125 * serial, -0.25 * serial, 0.5 * serial),
        "MostBoundID": most_bound_id,
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


def main() -> int:
    # (snap, (Desc, FirstProg, NextProg, FirstFoF, NextFoF), MostBoundID)
    tree0 = [
        (0, (2, -1, 1, 0, -1), 700),
        (1, (2, -1, -1, 1, -1), -701),
        (3, (-1, 0, -1, 2, 3), 702),
        (3, (-1, -1, -1, 2, -1), 702),
    ]
    tree1 = [
        (0, (1, -1, -1, 0, -1), 800),
        (1, (-1, 0, -1, 1, -1), 801),
        (3, (-1, -1, -1, 2, -1), 802),
    ]
    trees = [tree0, tree1]

    with open(OUT_PATH, "wb") as handle:
        handle.write(struct.pack("<ii", len(trees), sum(len(t) for t in trees)))
        for tree in trees:
            handle.write(struct.pack("<i", len(tree)))
        for tree_number, tree in enumerate(trees):
            for index, (snap, links, most_bound_id) in enumerate(tree):
                handle.write(record(tree_number, index, snap, links, most_bound_id))
    print("wrote {}".format(os.path.relpath(OUT_PATH, _REPO)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
