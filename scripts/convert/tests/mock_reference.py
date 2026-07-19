"""Mock halos-only reference output builder for the Slice 8 cross-check tests.

Builds a semantically correct reference galaxy output directly from a converted
snapshot-HDF5 dataset, replicating the reference inheritance semantics
(src/core/inheritance.c) without running Mimic:

- occupancy by forward induction (``occupied(H) = FoF-central(H) OR any
  occupied progenitor``);
- per occupied halo, its galaxy's ``UniqueGalaxyID`` is inherited from the
  first OCCUPIED progenitor in chain order (FirstProgenitor then siblings via
  NextProgenitor at N-1), or, if none, CREATED from the frozen encoding
  ``rank + multiplier * (ForestIndex + 1)`` (only FoF centrals create);
- Type 0 for FoF centrals (self-referencing FirstHaloInFOFgroup), else Type 1;
- UniqueCentralGalaxyID = the UniqueGalaxyID of the halo at
  FirstHaloInFOFgroup; MostBoundID keeps the halo's signed value;
- values copied bit-for-bit from the converter arrays, with
  ``Mvir = float64(M_Crit200) * 1e-10``.

One Type 2 orphan row is appended at the final populated snapshot (a merged
progenitor galaxy's UniqueGalaxyID with the MostBoundID of a halo that no
longer exists) to prove Type-2 rows are ignored by the cross-check. The
Galaxies dtype also carries one extra field (``dT``) to prove extras are
tolerated. ``write_mock_reference`` writes a single ``<base>_000.hdf5`` chunk
plus an empty master ``<base>.hdf5`` to prove the master is ignored.
"""

import os
import sys
from pathlib import Path
from typing import Dict, Optional

import h5py
import numpy as np

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import validate  # noqa: E402

#: Reference Galaxies structured dtype: exactly the fields the cross-check
#: consumes, plus an extra ``dT`` field to prove extras are tolerated.
GALAXY_DTYPE = np.dtype(
    [
        ("SnapNum", np.int32),
        ("Type", np.int32),
        ("UniqueGalaxyID", np.int64),
        ("UniqueCentralGalaxyID", np.int64),
        ("Len", np.int32),
        ("Mvir", np.float64),
        ("Pos", np.float32, (3,)),
        ("Vel", np.float32, (3,)),
        ("Spin", np.float32, (3,)),
        ("VelDisp", np.float32),
        ("Vmax", np.float32),
        ("MostBoundID", np.int64),
        ("dT", np.float64),
    ]
)


def _fill_from_halo(row, conv, h) -> None:
    """Copy the bit-for-bit value fields from converter halo ``h`` into a
    Galaxies row (Mvir derived per the frozen arithmetic)."""
    row["Pos"] = conv["Pos"][h]
    row["Vel"] = conv["Vel"][h]
    row["Spin"] = conv["Spin"][h]
    row["VelDisp"] = conv["VelDisp"][h]
    row["Vmax"] = conv["Vmax"][h]
    row["Len"] = conv["Len"][h]
    row["Mvir"] = np.float64(conv["M_Crit200"][h]) * 1e-10
    row["MostBoundID"] = conv["MostBoundID"][h]


def build_mock_galaxies(hdf5_dir, n_snapshots, multiplier=10**9) -> Dict[int, np.ndarray]:
    """Build reference galaxies per snapshot from a converted dataset,
    replicating the reference inheritance semantics."""
    _, arrays = validate.load_dataset(Path(hdf5_dir), n_snapshots)
    galaxies_by_snap: Dict[int, np.ndarray] = {}
    ugid_by_snap: Dict[int, np.ndarray] = {}
    occupied_prev = None
    ugid_prev = None

    for snap in range(n_snapshots):
        conv = arrays[snap]
        n = conv["MostBoundID"].size
        first_fof = conv["FirstHaloInFOFgroup"]
        is_central = first_fof == np.arange(n)

        # occupancy: FoF-central, or a descendant of an occupied progenitor
        occupied = is_central.copy()
        if occupied_prev is not None:
            desc = arrays[snap - 1]["Descendant"]
            forwarded = desc[occupied_prev]
            occupied[forwarded[forwarded != -1]] = True

        # UniqueGalaxyID: inherit from the first occupied progenitor in chain
        # order, else create (FoF centrals only)
        ugid = np.full(n, -1, dtype=np.int64)
        first_prog = conv["FirstProgenitor"]
        next_prog_prev = arrays[snap - 1]["NextProgenitor"] if snap > 0 else None
        for h in range(n):
            if not occupied[h]:
                continue
            donor = -1
            if snap > 0 and first_prog[h] != -1:
                cursor = int(first_prog[h])
                while cursor != -1:
                    if occupied_prev[cursor]:
                        donor = int(ugid_prev[cursor])
                        break
                    cursor = int(next_prog_prev[cursor])
            if donor != -1:
                ugid[h] = donor
            else:
                assert is_central[h], "non-central halo created a galaxy at snap {}".format(snap)
                ugid[h] = int(conv["HaloRankInForest"][h]) + multiplier * (
                    int(conv["ForestIndex"][h]) + 1
                )

        rows = []
        for h in range(n):
            if not occupied[h]:
                continue
            row = np.zeros((), dtype=GALAXY_DTYPE)
            row["SnapNum"] = snap
            row["Type"] = 0 if is_central[h] else 1
            row["UniqueGalaxyID"] = ugid[h]
            row["UniqueCentralGalaxyID"] = ugid[int(first_fof[h])]
            _fill_from_halo(row, conv, h)
            rows.append(row)
        galaxies_by_snap[snap] = (
            np.array(rows, dtype=GALAXY_DTYPE) if rows else np.empty(0, dtype=GALAXY_DTYPE)
        )
        occupied_prev = occupied
        ugid_prev = ugid
        ugid_by_snap[snap] = ugid

    _append_orphan(galaxies_by_snap, arrays, ugid_by_snap)
    return galaxies_by_snap


def _append_orphan(galaxies_by_snap, arrays, ugid_by_snap) -> None:
    """Append one Type 2 orphan at the final populated snapshot: a merged
    progenitor galaxy's UniqueGalaxyID with the MostBoundID of a halo that no
    longer exists there. Its UniqueCentralGalaxyID names the FoF central of
    the halo the donor merged into (reference central-ID propagation,
    src/core/build_model.c). Ignored by every cross-check."""
    populated = [snap for snap, gals in galaxies_by_snap.items() if gals.size]
    if not populated:
        return
    final_snap = max(populated)
    prev = [snap for snap in populated if snap < final_snap]
    if not prev:
        return
    prev_snap = max(prev)
    if prev_snap + 1 != final_snap:
        return  # the donor's merge target must live at the final snapshot
    final_ugids = set(galaxies_by_snap[final_snap]["UniqueGalaxyID"].tolist())
    merged = [g for g in galaxies_by_snap[prev_snap] if int(g["UniqueGalaxyID"]) not in final_ugids]
    if not merged:
        return
    donor = merged[0]
    donor_halo = np.nonzero(ugid_by_snap[prev_snap] == int(donor["UniqueGalaxyID"]))[0]
    if donor_halo.size == 0:
        return
    target = int(arrays[prev_snap]["Descendant"][donor_halo[0]])
    if target < 0:
        return
    host_central = int(arrays[final_snap]["FirstHaloInFOFgroup"][target])
    orphan = np.zeros((), dtype=GALAXY_DTYPE)
    orphan["SnapNum"] = final_snap
    orphan["Type"] = 2
    orphan["UniqueGalaxyID"] = donor["UniqueGalaxyID"]
    orphan["UniqueCentralGalaxyID"] = ugid_by_snap[final_snap][host_central]
    orphan["MostBoundID"] = donor["MostBoundID"]
    galaxies_by_snap[final_snap] = np.concatenate([galaxies_by_snap[final_snap], orphan.reshape(1)])


def write_mock_reference(galaxies_by_snap, directory, base="halos", n_snapshots=None) -> Path:
    """Write the reference output: one ``<base>_000.hdf5`` chunk with a
    ``Snap%03d/Galaxies`` dataset for every snapshot (empty snapshots get a
    zero-length dataset), plus an empty master ``<base>.hdf5`` (RunProperties
    group only) that the cross-check must ignore."""
    directory = Path(directory)
    directory.mkdir(parents=True, exist_ok=True)
    if n_snapshots is None:
        n_snapshots = max(galaxies_by_snap) + 1

    chunk = directory / "{}_000.hdf5".format(base)
    with h5py.File(chunk, "w") as handle:
        for snap in range(n_snapshots):
            gals = galaxies_by_snap.get(snap)
            if gals is None:
                gals = np.empty(0, dtype=GALAXY_DTYPE)
            handle.create_group("Snap{:03d}".format(snap)).create_dataset("Galaxies", data=gals)

    master = directory / "{}.hdf5".format(base)
    with h5py.File(master, "w") as handle:
        handle.create_group("RunProperties")
    return chunk
