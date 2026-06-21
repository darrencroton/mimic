#!/usr/bin/env python3
"""Create the tiny micro-Uchuu HDF5 reader fixture used by integration tests."""

from __future__ import annotations

from pathlib import Path

import h5py
import numpy as np

ROOT = Path(__file__).resolve().parents[1]
DATA_DIR = ROOT / "data"
FIXTURE = DATA_DIR / "MicroUchuu_test_mergertree_info.h5"


def write_dataset(group: h5py.Group, name: str, values, dtype) -> None:
    group.create_dataset(name, data=np.asarray(values, dtype=dtype))


def main() -> None:
    DATA_DIR.mkdir(parents=True, exist_ok=True)

    # Three forests, six halos total. The first forest has a two-snapshot
    # central+satellite pattern so process_full_halo sees a real multi-galaxy
    # workspace at z=0; the other forests keep simple single-central coverage.
    forest_info = np.array(
        [
            (1000, 0, 4, 1),
            (1001, 4, 1, 1),
            (1002, 5, 1, 1),
        ],
        dtype=np.dtype(
            [
                ("ForestID", "<i8"),
                ("ForestHalosOffset", "<i8"),
                ("ForestNhalos", "<i8"),
                ("ForestNtrees", "<i8"),
            ]
        ),
    )

    descendant = [-1, -1, 0, 1, -1, -1]
    first_progenitor = [2, 3, -1, -1, -1, -1]
    no_next_progenitors = [-1, -1, -1, -1, -1, -1]
    first_fof = [0, 0, 2, 3, 0, 0]
    next_fof = [1, -1, -1, -1, -1, -1]

    with h5py.File(FIXTURE, "w") as h5:
        h5.attrs["Nfiles"] = np.int64(1)
        h5.attrs["TotNforests"] = np.int64(len(forest_info))

        file0 = h5.create_group("File0")
        file0.attrs["Nforests"] = np.int64(len(forest_info))
        file0.attrs["Nhalos"] = np.int64(6)
        file0.attrs["contiguous-halo-props"] = np.int8(1)

        params = file0.create_group("simulation_params")
        params.attrs["Omega_M"] = np.float64(0.3089)
        params.attrs["Omega_L"] = np.float64(0.6911)
        params.attrs["hubble"] = np.float64(0.6774)
        params.attrs["Boxsize"] = np.float64(100.0)

        file0.create_dataset("ForestInfo", data=forest_info)
        forests = file0.create_group("Forests")

        for name, values in {
            "Descendant": descendant,
            "FirstProgenitor": first_progenitor,
            "NextProgenitor": no_next_progenitors,
            "FirstHaloInFOFgroup": first_fof,
            "NextHaloInFOFgroup": next_fof,
            "id": [900001, 900002, 900003, 900004, 900005, 900006],
            "Snap_num": [49, 49, 48, 48, 49, 49],
        }.items():
            write_dataset(forests, name, values, "<i8")

        write_dataset(
            forests, "Mvir", [3.25e10, 4.00e10, 5.50e10, 2.75e10, 6.00e10, 1.50e10], "<f8"
        )
        write_dataset(forests, "x", [5.0, 15.0, 25.0, 26.0, 35.0, 36.0], "<f8")
        write_dataset(forests, "y", [6.0, 16.0, 26.0, 27.0, 36.0, 37.0], "<f8")
        write_dataset(forests, "z", [7.0, 17.0, 27.0, 28.0, 37.0, 38.0], "<f8")
        write_dataset(forests, "vrms", [80.0, 85.0, 90.0, 75.0, 95.0, 70.0], "<f8")
        write_dataset(forests, "vmax", [120.0, 125.0, 130.0, 115.0, 135.0, 110.0], "<f8")
        write_dataset(forests, "vx", [10.0, 11.0, 12.0, 13.0, 14.0, 15.0], "<f8")
        write_dataset(forests, "vy", [20.0, 21.0, 22.0, 23.0, 24.0, 25.0], "<f8")
        write_dataset(forests, "vz", [30.0, 31.0, 32.0, 33.0, 34.0, 35.0], "<f8")
        write_dataset(forests, "Jx", [0.0, 0.0, 0.0, 0.0, 0.0, 0.0], "<f8")
        write_dataset(forests, "Jy", [0.0, 0.0, 0.0, 0.0, 0.0, 0.0], "<f8")
        write_dataset(forests, "Jz", [0.0, 0.0, 0.0, 0.0, 0.0, 0.0], "<f8")

    print(FIXTURE)


if __name__ == "__main__":
    main()
