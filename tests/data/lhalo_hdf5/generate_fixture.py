#!/usr/bin/env python3
"""Write the tiny L-Halo-tree HDF5 fixture read by tests/unit/test_lhalo_hdf5_reader.c.

The `lhalo_hdf5` reader (src/io/vertical/hdf5.c) expects one HDF5 file per partition,
named by `input.tree_name` with its `%d` placeholder replaced by the file number, each
laid out as:

    /Header                 group carrying three native-int attributes:
        Ntrees              number of trees in this file (one element)
        totNHalos           total halos over those trees (one element)
        InputTreeNHalos     halos per tree, exactly Ntrees elements
    /tree_NNN/<field>       one group per tree (NNN = tree index, zero-padded to three
                            digits); one dataset per catalog field, InputTreeNHalos[NNN]
                            rows long, (rows, 3) for Pos, Vel and Spin

The <field> names are the `source` names of the compiled simulation package's
halo_properties.yaml (generated into read_tree_hdf5_properties.inc). This fixture carries
mini-Millennium's L-Halo record, so a unit is loadable only under a package with that
catalog; the header hooks are catalog-independent.

Files written beside this script:

    trees_fixture.0.hdf5    three trees of 2, 5 and 1 halos (largest in the middle)
    trees_fixture.1.hdf5    two trees of 3 and 6 halos (largest last)
    trees_mismatch.0.hdf5   header only: Ntrees = 3 but InputTreeNHalos has 2 elements,
                            which the extent-checked header reader must reject

Each tree is one main-branch chain: halo 0 is the root at the last snapshot, halo i
descends to halo i - 1, and every halo is its own FoF group. `Len` encodes the halo's
position as 1000 * file + 100 * tree + halo + 20, so a test can tell which rows loaded.

Usage (from anywhere, with the project's mimic_venv Python):
    mimic_venv/bin/python tests/data/lhalo_hdf5/generate_fixture.py
"""

from __future__ import annotations

from pathlib import Path

import h5py
import numpy as np

FIXTURE_DIR = Path(__file__).resolve().parent

# Halos per tree, per file number.
FILE_TREE_NHALOS = {0: [2, 5, 1], 1: [3, 6]}

LAST_SNAPSHOT = 63

# mini-Millennium's L-Halo catalog, in record order: (dataset name, dtype, components).
CATALOG_FIELDS = [
    ("Descendant", "<i4", 1),
    ("FirstProgenitor", "<i4", 1),
    ("NextProgenitor", "<i4", 1),
    ("FirstHaloInFOFgroup", "<i4", 1),
    ("NextHaloInFOFgroup", "<i4", 1),
    ("Len", "<i4", 1),
    ("M_mean200", "<f4", 1),
    ("Mvir", "<f4", 1),
    ("M_TopHat", "<f4", 1),
    ("Pos", "<f4", 3),
    ("Vel", "<f4", 3),
    ("VelDisp", "<f4", 1),
    ("Vmax", "<f4", 1),
    ("Spin", "<f4", 3),
    ("MostBoundID", "<i8", 1),
    ("SnapNum", "<i4", 1),
    ("Filenr", "<i4", 1),
    ("SubHaloIndex", "<i4", 1),
    ("SubHalfMass", "<f4", 1),
]


def tree_fields(file_nr: int, tree: int, nhalos: int) -> dict[str, np.ndarray]:
    """Return every catalog field of one main-branch chain of `nhalos` halos."""
    halo = np.arange(nhalos)
    lengths = 1000 * file_nr + 100 * tree + halo + 20
    mass = lengths.astype(np.float64) * 0.0860657
    position = np.stack([halo + 1.0, halo + 2.0, halo + 3.0], axis=1) + 10.0 * tree
    return {
        "Descendant": halo - 1,
        "FirstProgenitor": np.where(halo + 1 < nhalos, halo + 1, -1),
        "NextProgenitor": np.full(nhalos, -1),
        "FirstHaloInFOFgroup": halo,
        "NextHaloInFOFgroup": np.full(nhalos, -1),
        "Len": lengths,
        "M_mean200": mass * 1.1,
        "Mvir": mass,
        "M_TopHat": mass * 1.05,
        "Pos": position,
        "Vel": position * 10.0,
        "VelDisp": 50.0 + halo,
        "Vmax": 100.0 + halo,
        "Spin": np.full((nhalos, 3), 0.01),
        "MostBoundID": 10_000_000 * (file_nr + 1) + 1000 * tree + halo,
        "SnapNum": LAST_SNAPSHOT - halo,
        "Filenr": np.full(nhalos, file_nr),
        "SubHaloIndex": np.zeros(nhalos),
        "SubHalfMass": mass * 0.5,
    }


def write_header(h5: h5py.File, ntrees: int, tree_nhalos: list[int]) -> None:
    """Write the /Header attributes; `tree_nhalos` may disagree with `ntrees` on purpose."""
    header = h5.create_group("Header")
    header.attrs["Ntrees"] = np.int32(ntrees)
    header.attrs["totNHalos"] = np.int32(sum(tree_nhalos))
    header.attrs["InputTreeNHalos"] = np.asarray(tree_nhalos, dtype=np.int32)


def write_partition(path: Path, file_nr: int, tree_nhalos: list[int]) -> None:
    """Write one well-formed partition file."""
    with h5py.File(path, "w") as h5:
        write_header(h5, len(tree_nhalos), tree_nhalos)
        for tree, nhalos in enumerate(tree_nhalos):
            group = h5.create_group(f"tree_{tree:03d}")
            fields = tree_fields(file_nr, tree, nhalos)
            for name, dtype, components in CATALOG_FIELDS:
                values = np.asarray(fields[name], dtype=dtype)
                expected_shape = (nhalos, components) if components > 1 else (nhalos,)
                assert values.shape == expected_shape, (name, values.shape)
                group.create_dataset(name, data=values)


def main() -> None:
    for file_nr, tree_nhalos in FILE_TREE_NHALOS.items():
        path = FIXTURE_DIR / f"trees_fixture.{file_nr}.hdf5"
        write_partition(path, file_nr, tree_nhalos)
        print(path)

    mismatch = FIXTURE_DIR / "trees_mismatch.0.hdf5"
    with h5py.File(mismatch, "w") as h5:
        write_header(h5, 3, [2, 5])
    print(mismatch)


if __name__ == "__main__":
    main()
