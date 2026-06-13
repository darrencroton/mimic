"""
HDF5 data reader for Mimic output files.

Reads one snapshot from Mimic HDF5 output (individual per-file or master file).
The HDF5 format is self-describing, so h5py reads the structure directly from
the file.
"""

import h5py
import numpy as np


def read_hdf5_snapshot(filename, snapshot_num):
    """
    Read halos from a specific snapshot in an HDF5 file.

    Args:
        filename (str or Path): Path to HDF5 file (individual or master)
        snapshot_num (int): Snapshot number to read

    Returns:
        np.recarray: Structured array of halos, or None if snapshot not found
    """
    try:
        with h5py.File(filename, "r") as f:
            # Format: Snap063, Snap037, etc.
            group_name = f"Snap{snapshot_num:03d}"

            if group_name not in f:
                return None

            # Check if this is a master file with external links
            snap_group = f[group_name]

            # Master files have File000, File001, etc. subgroups
            # Individual files have Galaxies dataset directly
            if "Galaxies" in snap_group:
                # Individual file format
                dataset = snap_group["Galaxies"]
                return np.array(dataset[:])
            else:
                # Master file format - need to read from all File subgroups
                # Iterate over actual File subgroups instead of assuming File000, File001, etc.
                halos_list = []
                for key in sorted(snap_group.keys()):
                    if key.startswith("File"):
                        file_group = snap_group[key]
                        if "Galaxies" in file_group:
                            dataset = file_group["Galaxies"]
                            halos_list.append(np.array(dataset[:]))

                if not halos_list:
                    return None

                # Concatenate all files
                return np.concatenate(halos_list)

    except (OSError, KeyError, ValueError) as e:
        print(f"Warning: Could not read snapshot {snapshot_num} from {filename}: {e}")
        return None
