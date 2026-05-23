#!/usr/bin/env python

"""
Unit tests for the SAGE-native HDF5 reader (output/mimic-plot/sage_native_hdf5.py).

Covers:
  - SAGE -> Mimic field renames (IntraClusterStars -> ICS, etc.)
  - Vector field reconstruction from Posx/Posy/Posz and friends
  - SfrDisk + SfrBulge -> StarFormationRate aggregation
  - SnapNum always populated even if SAGE did not export it
  - Mimic-only fields stay zero-filled when SAGE omits them
  - Per-rank file iteration <base>_<N>.hdf5
  - Optional master file <base>.hdf5 with Core_<N>/Snap_<M>/... layout

Skipped when h5py is unavailable.
"""

import os
import sys
import tempfile
import unittest

# Make the mimic-plot package importable from this tests/ subdirectory
HERE = os.path.dirname(os.path.abspath(__file__))
MIMIC_PLOT_DIR = os.path.dirname(HERE)
sys.path.insert(0, MIMIC_PLOT_DIR)

import numpy as np

try:
    import h5py
    HAVE_H5PY = True
except ImportError:
    HAVE_H5PY = False


SAGE_FIELDS_TEMPLATE = {
    # Identically-named scalar fields
    "Type": np.int32,
    "Mvir": np.float32,
    "CentralMvir": np.float32,
    "Rvir": np.float32,
    "Vvir": np.float32,
    "Vmax": np.float32,
    "ColdGas": np.float32,
    "HotGas": np.float32,
    "StellarMass": np.float32,
    "BulgeMass": np.float32,
    "BlackHoleMass": np.float32,
    # Renamed fields
    "IntraClusterStars": np.float32,   # -> ICS
    "EjectedMass": np.float32,         # -> EjectedGas
    "MetalsIntraClusterStars": np.float32,  # -> MetalsICS
    "MetalsEjectedMass": np.float32,   # -> MetalsEjectedGas
    "OutflowRate": np.float32,         # -> SupernovaOutflowRate
    "DiskRadius": np.float32,          # -> DiskScaleRadius
    "GalaxyIndex": np.int64,           # -> UniqueGalaxyID
    "CentralGalaxyIndex": np.int64,    # -> UniqueCentralGalaxyID
    "SimulationHaloIndex": np.int64,   # -> MostBoundID
    # SFR split
    "SfrDisk": np.float32,
    "SfrBulge": np.float32,
    # Vector components
    "Posx": np.float32, "Posy": np.float32, "Posz": np.float32,
    "Velx": np.float32, "Vely": np.float32, "Velz": np.float32,
    "Spinx": np.float32, "Spiny": np.float32, "Spinz": np.float32,
}


def _populate_snap_group(group, n, snap, fnr, include_snapnum=True):
    """Write a Snap_<snap> group's worth of SAGE-style datasets."""
    rng = np.random.default_rng(snap * 1000 + fnr)
    for name, dtype in SAGE_FIELDS_TEMPLATE.items():
        if np.issubdtype(dtype, np.integer):
            group.create_dataset(name, data=np.arange(n, dtype=dtype) + fnr * n)
        else:
            group.create_dataset(name, data=rng.uniform(0.1, 5.0, n).astype(dtype))
    # Make SfrDisk/SfrBulge deterministic so the assertion is exact
    group["SfrDisk"][:] = np.full(n, 1.25, dtype=np.float32)
    group["SfrBulge"][:] = np.full(n, 0.75, dtype=np.float32)
    if include_snapnum:
        group.create_dataset("SnapNum", data=np.full(n, snap, dtype=np.int32))


def _build_params(tmpdir, num_simulation_tree_files=4):
    """Build a minimal params dict + a_list file for SnapshotRedshiftMapper."""
    a_list = os.path.join(tmpdir, "fake.a_list")
    with open(a_list, "w") as f:
        for i in range(64):
            a = 1.0 / (1.0 + (63 - i) * 0.1)
            f.write(f"{a}\n")
    return {
        "Hubble_h": 0.73,
        "BoxSize": 62.5,
        "FirstFile": 0,
        "LastFile": 1,
        "NumSimulationTreeFiles": num_simulation_tree_files,
        "OutputFileBaseName": "model",
        "FileWithSnapList": a_list,
        "LastSnapshotNr": 63,
        "_input_format": "sage-hdf5",
    }


@unittest.skipUnless(HAVE_H5PY, "h5py not installed")
class TestSagePerRankRead(unittest.TestCase):
    def test_rename_vector_sfr_snapnum_and_defaults(self):
        import sage_native_hdf5
        from generated.dtype import get_hdf5_dtype

        snapshot = 63
        with tempfile.TemporaryDirectory() as tmp:
            # 2 per-rank SAGE files
            for fnr in (0, 1):
                with h5py.File(os.path.join(tmp, f"model_{fnr}.hdf5"), "w") as f:
                    _populate_snap_group(f.create_group(f"Snap_{snapshot}"),
                                         n=4, snap=snapshot, fnr=fnr,
                                         include_snapnum=False)

            params = _build_params(tmp, num_simulation_tree_files=8)
            model_path = os.path.join(tmp, "model_z0.000")  # z=0 -> snap 63

            galaxies, volume, metadata = sage_native_hdf5.read_data_sage_native(
                model_path, first_file=0, last_file=1, params=params
            )

            self.assertEqual(galaxies.dtype, get_hdf5_dtype())
            self.assertEqual(len(galaxies), 8)
            # Renamed fields are populated (non-zero)
            self.assertTrue(np.all(galaxies.ICS > 0.0))
            self.assertTrue(np.all(galaxies.EjectedGas > 0.0))
            self.assertTrue(np.all(galaxies.DiskScaleRadius > 0.0))
            self.assertTrue(np.all(galaxies.SupernovaOutflowRate > 0.0))
            # Vector reconstruction
            self.assertEqual(galaxies.Pos.shape, (8, 3))
            self.assertEqual(galaxies.Vel.shape, (8, 3))
            self.assertEqual(galaxies.Spin.shape, (8, 3))
            # SFR combination is exact
            np.testing.assert_allclose(galaxies.StarFormationRate, 2.0)
            # SnapNum is populated even though SAGE did not write it
            self.assertTrue(np.all(galaxies.SnapNum == snapshot))
            # Mimic-only fields default to zero
            self.assertTrue(np.all(galaxies.Len == 0))
            self.assertTrue(np.all(galaxies.deltaMvir == 0.0))
            self.assertTrue(np.all(galaxies.HaloBaryonFraction == 0.0))
            # Volume scaled by good_files / NumSimulationTreeFiles
            self.assertAlmostEqual(volume, 62.5 ** 3 * 2 / 8, places=2)
            self.assertEqual(metadata["snapshot"], snapshot)
            self.assertEqual(metadata["good_files"], 2)


@unittest.skipUnless(HAVE_H5PY, "h5py not installed")
class TestSageMasterFileRead(unittest.TestCase):
    def test_master_file_with_core_subgroups(self):
        import sage_native_hdf5
        from generated.dtype import get_hdf5_dtype

        snapshot = 63
        with tempfile.TemporaryDirectory() as tmp:
            # Build a master file with two Core_<N>/Snap_<snapshot> subgroups.
            # SAGE's real master file uses external links; for testing the
            # consumer logic the equivalent direct subgroup layout suffices.
            master_path = os.path.join(tmp, "model.hdf5")
            with h5py.File(master_path, "w") as f:
                for core_idx in (0, 1, 2):
                    core_g = f.create_group(f"Core_{core_idx}")
                    snap_g = core_g.create_group(f"Snap_{snapshot}")
                    _populate_snap_group(snap_g, n=5, snap=snapshot, fnr=core_idx)

            params = _build_params(tmp, num_simulation_tree_files=3)
            model_path = os.path.join(tmp, "model_z0.000")

            galaxies, volume, metadata = sage_native_hdf5.read_data_sage_native(
                model_path, first_file=0, last_file=2, params=params
            )

            # 3 cores * 5 galaxies = 15
            self.assertEqual(len(galaxies), 15)
            self.assertEqual(galaxies.dtype, get_hdf5_dtype())
            self.assertTrue(np.all(galaxies.SnapNum == snapshot))
            self.assertEqual(metadata["good_files"], 3)


if __name__ == "__main__":
    unittest.main(verbosity=2)
