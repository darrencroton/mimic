#!/usr/bin/env python3
"""Create the tiny micro-Uchuu L-Halo binary fixture used by integration tests."""

from __future__ import annotations

from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parents[1]
DATA_DIR = ROOT / "data"
FIXTURE = DATA_DIR / "Uchuu100_test_lhalo_binary.0"


def raw_halo_dtype() -> np.dtype:
    return np.dtype(
        [
            ("Descendant", np.int32),
            ("FirstProgenitor", np.int32),
            ("NextProgenitor", np.int32),
            ("FirstHaloInFOFgroup", np.int32),
            ("NextHaloInFOFgroup", np.int32),
            ("Len", np.int32),
            ("M_Mean200", np.float32),
            ("M_Crit200", np.float32),
            ("M_TopHat", np.float32),
            ("Pos", (np.float32, 3)),
            ("Vel", (np.float32, 3)),
            ("VelDisp", np.float32),
            ("Vmax", np.float32),
            ("Spin", (np.float32, 3)),
            ("MostBoundID", np.int64),
            ("SnapNum", np.int32),
            ("FileNr", np.int32),
            ("SubhaloIndex", np.int32),
            ("SubHalfMass", np.float32),
        ],
        align=True,
    )


def fixture_halos() -> np.recarray:
    halos = np.zeros(6, dtype=raw_halo_dtype()).view(np.recarray)

    halos.Descendant = [-1, -1, 0, 1, -1, -1]
    halos.FirstProgenitor = [2, 3, -1, -1, -1, -1]
    halos.NextProgenitor = [-1, -1, -1, -1, -1, -1]
    halos.FirstHaloInFOFgroup = [0, 0, 2, 3, 0, 0]
    halos.NextHaloInFOFgroup = [1, -1, -1, -1, -1, -1]
    halos.Len = [1000, 700, 950, 650, 800, 500]
    halos.M_Mean200 = [3.25, 2.75, 3.10, 2.60, 4.00, 1.50]
    halos.M_Crit200 = [3.25, 2.75, 3.10, 2.60, 4.00, 1.50]
    halos.M_TopHat = [3.25, 2.75, 3.10, 2.60, 4.00, 1.50]
    halos.Pos = [
        (5.0, 6.0, 7.0),
        (5.2, 6.1, 7.1),
        (4.8, 5.9, 6.9),
        (5.1, 6.0, 7.2),
        (15.0, 16.0, 17.0),
        (25.0, 26.0, 27.0),
    ]
    halos.Vel = [
        (10.0, 20.0, 30.0),
        (11.0, 21.0, 31.0),
        (12.0, 22.0, 32.0),
        (13.0, 23.0, 33.0),
        (14.0, 24.0, 34.0),
        (15.0, 25.0, 35.0),
    ]
    halos.VelDisp = [80.0, 75.0, 78.0, 72.0, 85.0, 70.0]
    halos.Vmax = [120.0, 115.0, 118.0, 112.0, 125.0, 110.0]
    halos.Spin = [(0.1, 0.2, 0.3)] * len(halos)
    halos.MostBoundID = [900001, 900002, 900003, 900004, 900005, 900006]
    halos.SnapNum = [49, 49, 48, 48, 49, 49]
    halos.FileNr = 0
    halos.SubhaloIndex = np.arange(len(halos), dtype=np.int32)
    halos.SubHalfMass = [1.6, 1.3, 1.5, 1.2, 2.0, 0.7]

    return halos


def main() -> None:
    DATA_DIR.mkdir(parents=True, exist_ok=True)
    halos = fixture_halos()
    tree_sizes = np.asarray([4, 1, 1], dtype=np.int32)

    with FIXTURE.open("wb") as handle:
        np.asarray([len(tree_sizes), len(halos)], dtype=np.int32).tofile(handle)
        tree_sizes.tofile(handle)
        halos.tofile(handle)

    print(FIXTURE)


if __name__ == "__main__":
    main()
