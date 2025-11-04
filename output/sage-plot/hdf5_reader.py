"""
HDF5 data reader for SAGE output files.

This module provides functions to read SAGE output in HDF5 format,
matching the structure defined in io_save_hdf5.c.
"""

import h5py
import numpy as np
from pathlib import Path


def get_hdf5_dtype():
    """
    Return the NumPy dtype matching SAGE HDF5 HaloOutput structure.

    Must match the 24-field structure defined in types.h and io_save_hdf5.c:line87-203.

    Returns:
        np.dtype: NumPy structured array dtype for HDF5 halos
    """
    return np.dtype([
        ("SnapNum", np.int32),
        ("Type", np.int32),
        ("HaloIndex", np.int64),
        ("CentralHaloIndex", np.int64),
        ("SAGEHaloIndex", np.int32),
        ("SAGETreeIndex", np.int32),
        ("SimulationHaloIndex", np.int64),
        ("MergeStatus", np.int32),
        ("mergeIntoID", np.int32),
        ("mergeIntoSnapNum", np.int32),
        ("dT", np.float32),
        ("Pos", np.float32, (3,)),
        ("Vel", np.float32, (3,)),
        ("Spin", np.float32, (3,)),
        ("Len", np.int32),
        ("Mvir", np.float32),
        ("CentralMvir", np.float32),
        ("Rvir", np.float32),
        ("Vvir", np.float32),
        ("Vmax", np.float32),
        ("VelDisp", np.float32),
        ("infallMvir", np.float32),
        ("infallVvir", np.float32),
        ("infallVmax", np.float32),
    ])


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
        with h5py.File(filename, 'r') as f:
            # Format: Snap063, Snap037, etc.
            group_name = f"Snap{snapshot_num:03d}"

            if group_name not in f:
                return None

            # Check if this is a master file with external links
            snap_group = f[group_name]

            # Master files have File000, File001, etc. subgroups
            # Individual files have Galaxies dataset directly
            if 'Galaxies' in snap_group:
                # Individual file format
                dataset = snap_group['Galaxies']
                return np.array(dataset[:])
            else:
                # Master file format - need to read from all File subgroups
                halos_list = []
                file_idx = 0
                while True:
                    file_group_name = f"File{file_idx:03d}"
                    if file_group_name not in snap_group:
                        break

                    file_group = snap_group[file_group_name]
                    if 'Galaxies' in file_group:
                        dataset = file_group['Galaxies']
                        halos_list.append(np.array(dataset[:]))

                    file_idx += 1

                if not halos_list:
                    return None

                # Concatenate all files
                return np.concatenate(halos_list)

    except (OSError, KeyError, ValueError) as e:
        print(f"Warning: Could not read snapshot {snapshot_num} from {filename}: {e}")
        return None


def count_halos_in_file(filename, snapshot_num):
    """
    Count total halos in a specific snapshot.

    Args:
        filename (str or Path): Path to HDF5 file
        snapshot_num (int): Snapshot number

    Returns:
        int: Number of halos in the snapshot, or 0 if not found
    """
    try:
        with h5py.File(filename, 'r') as f:
            group_name = f"Snap{snapshot_num:03d}"

            if group_name not in f:
                return 0

            snap_group = f[group_name]

            if 'Galaxies' in snap_group:
                # Individual file
                return snap_group['Galaxies'].shape[0]
            else:
                # Master file - sum across all File subgroups
                total = 0
                file_idx = 0
                while True:
                    file_group_name = f"File{file_idx:03d}"
                    if file_group_name not in snap_group:
                        break

                    file_group = snap_group[file_group_name]
                    if 'Galaxies' in file_group:
                        total += file_group['Galaxies'].shape[0]

                    file_idx += 1

                return total

    except (OSError, KeyError, ValueError):
        return 0


def read_hdf5_data(output_dir, file_base, first_file, last_file, snapshot_num):
    """
    Read halos from multiple HDF5 files for a given snapshot.

    Matches the interface of the binary read_data() function in sage-plot.py.

    Args:
        output_dir (str): Directory containing output files
        file_base (str): Base name for output files
        first_file (int): First file number to read
        last_file (int): Last file number to read
        snapshot_num (int): Snapshot number to extract

    Returns:
        tuple: (halos_array, total_halos) where halos_array is np.recarray
               and total_halos is int, or (None, 0) on error
    """
    output_path = Path(output_dir)
    halos_list = []
    total_halos = 0

    # Try reading from master file first
    master_file = output_path / f"{file_base}.hdf5"
    if master_file.exists():
        halos = read_hdf5_snapshot(master_file, snapshot_num)
        if halos is not None:
            return halos, len(halos)

    # Fall back to individual files
    for file_num in range(first_file, last_file + 1):
        filename = output_path / f"{file_base}_{file_num:03d}.hdf5"

        if not filename.exists():
            continue

        halos = read_hdf5_snapshot(filename, snapshot_num)
        if halos is not None:
            halos_list.append(halos)
            total_halos += len(halos)

    if not halos_list:
        return None, 0

    # Concatenate all halos from all files
    all_halos = np.concatenate(halos_list)
    return all_halos, total_halos


def get_hdf5_metadata(filename):
    """
    Read metadata attributes from HDF5 file.

    Args:
        filename (str or Path): Path to HDF5 file

    Returns:
        dict: Metadata attributes (Ntrees, Ntothalos, etc.)
    """
    metadata = {}

    try:
        with h5py.File(filename, 'r') as f:
            # Read root-level attributes
            for attr_name in f.attrs:
                metadata[attr_name] = f.attrs[attr_name]

            # Read snapshot-level attributes if needed
            for group_name in f.keys():
                if group_name.startswith('Snap'):
                    snap_group = f[group_name]
                    snap_attrs = {f"{group_name}_{k}": v for k, v in snap_group.attrs.items()}
                    metadata.update(snap_attrs)

    except (OSError, KeyError, ValueError) as e:
        print(f"Warning: Could not read metadata from {filename}: {e}")

    return metadata
