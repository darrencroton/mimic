#!/usr/bin/env python3
"""Regenerate the L-Halo binary fixtures in this directory.

Run from the repository root:

    mimic_venv/bin/python scripts/convert/tests/data/source_formats/generate_fixtures.py

Deterministic and idempotent: re-running reproduces byte-identical files.
Field layout is taken from adapters.source_inventory.LHALO_FIELDS (the
shipped 104-byte record), not hand-counted, so a future field-order change
there regenerates fixtures that still match it.
"""

import os
import struct
import sys
from pathlib import Path

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", ".."))
from adapters.source_inventory import LHALO_FIELDS, LHALO_RECORD_BYTES  # noqa: E402

OUT_DIR = Path(__file__).parent

_KIND_TO_STRUCT = {"i4": "i", "f4": "f", "i8": "q"}

_DEFAULTS = {
    "Descendant": -1,
    "FirstProgenitor": -1,
    "NextProgenitor": -1,
    "FirstHaloInFOFgroup": 0,
    "NextHaloInFOFgroup": -1,
    "Len": 10,
    "M_Mean200": 1.0,
    "M_Crit200": 1.0,
    "M_TopHat": 1.0,
    "Pos": (0.0, 0.0, 0.0),
    "Vel": (0.0, 0.0, 0.0),
    "VelDisp": 1.0,
    "Vmax": 1.0,
    "Spin": (0.0, 0.0, 0.0),
    "MostBoundID": 1,
    "SnapNum": 0,
    "FileNr": 0,
    "SubhaloIndex": 0,
    "SubHalfMass": 0.0,
}


def _field_format() -> str:
    chars = []
    for _name, kind, shape in LHALO_FIELDS:
        n = shape[0] if shape else 1
        chars.append(_KIND_TO_STRUCT[kind] * n)
    return "".join(chars)


_FIELD_FORMAT = _field_format()


def record(overrides=None, byte_order="<"):
    values = dict(_DEFAULTS)
    if overrides:
        values.update(overrides)
    packed_values = []
    for name, _kind, shape in LHALO_FIELDS:
        v = values[name]
        if shape:
            packed_values.extend(v)
        else:
            packed_values.append(v)
    packed = struct.pack(byte_order + _FIELD_FORMAT, *packed_values)
    assert len(packed) == LHALO_RECORD_BYTES, len(packed)
    return packed


def write_file(path, ntrees, tree_halo_counts, records, byte_order="<"):
    int_fmt = byte_order + "i"
    with open(path, "wb") as f:
        f.write(struct.pack(int_fmt, ntrees))
        f.write(struct.pack(int_fmt, sum(tree_halo_counts)))
        for c in tree_halo_counts:
            f.write(struct.pack(int_fmt, c))
        for r in records:
            f.write(r)


def main():
    # tree0: halo0(snap0) --Descendant--> halo1(snap2): span 2 (forward gap);
    #        halo1(snap2) --Descendant--> halo2(snap3): span 1 (adjacent).
    # tree1: two roots, no links.
    tree0 = [
        record({"Descendant": 1, "SnapNum": 0, "MostBoundID": 100}),
        record({"Descendant": 2, "SnapNum": 2, "MostBoundID": 101}),
        record({"Descendant": -1, "SnapNum": 3, "MostBoundID": 102}),
    ]
    tree1 = [
        record({"Descendant": -1, "SnapNum": 1, "MostBoundID": 200}),
        record({"Descendant": -1, "SnapNum": 1, "MostBoundID": 201}),
    ]
    write_file(OUT_DIR / "valid_two_trees.bin", 2, [3, 2], tree0 + tree1)

    tree0_be = [
        record({"Descendant": 1, "SnapNum": 0, "MostBoundID": 100}, byte_order=">"),
        record({"Descendant": 2, "SnapNum": 2, "MostBoundID": 101}, byte_order=">"),
        record({"Descendant": -1, "SnapNum": 3, "MostBoundID": 102}, byte_order=">"),
    ]
    tree1_be = [
        record({"Descendant": -1, "SnapNum": 1, "MostBoundID": 200}, byte_order=">"),
        record({"Descendant": -1, "SnapNum": 1, "MostBoundID": 201}, byte_order=">"),
    ]
    write_file(OUT_DIR / "valid_big_endian.bin", 2, [3, 2], tree0_be + tree1_be, byte_order=">")

    with open(OUT_DIR / "truncated_header.bin", "wb") as f:
        f.write(struct.pack("<i", 2))  # only Ntrees, missing totNHalos

    with open(OUT_DIR / "truncated_counts.bin", "wb") as f:
        f.write(struct.pack("<ii", 3, 5))
        f.write(struct.pack("<i", 2))  # only one of three counts

    with open(OUT_DIR / "bad_count_total.bin", "wb") as f:
        f.write(struct.pack("<ii", 2, 99))  # wrong total
        f.write(struct.pack("<ii", 3, 2))
        for r in tree0 + tree1:
            f.write(r)

    with open(OUT_DIR / "truncated_payload.bin", "wb") as f:
        f.write(struct.pack("<ii", 2, 5))
        f.write(struct.pack("<ii", 3, 2))
        for r in (tree0 + tree1)[:-1]:
            f.write(r)

    with open(OUT_DIR / "negative_counts.bin", "wb") as f:
        f.write(struct.pack("<ii", -1, 5))

    print("wrote fixtures:", sorted(p.name for p in OUT_DIR.glob("*.bin")))


if __name__ == "__main__":
    main()
