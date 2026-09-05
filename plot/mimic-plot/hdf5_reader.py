"""
HDF5 data reader for Mimic output files.

Reads one snapshot from Mimic HDF5 output (individual per-file or master file).
The HDF5 format is self-describing, so h5py reads the structure directly from
the file.
"""

import h5py
import numpy as np


def _file_group_sort_key(name):
    suffix = name[4:] if name.startswith("File") else ""
    return (0, int(suffix)) if suffix.isdigit() else (1, name)


def _read_galaxies(dataset, fields):
    """Read a Galaxies dataset, restricted to ``fields`` when they are all present.

    h5py reads only the named members of a compound type, so a caller that needs a
    few fields pays for a few fields instead of the whole record. A field list that
    the dataset does not carry (a stale caller-side field list, or output written by
    a model with different properties) falls back to the full record: reading too
    much is a performance cost, reading too little would be a correctness one.

    Args:
        dataset (h5py.Dataset): Compound-type Galaxies dataset
        fields (list[str] or None): Field subset to read, or None for the full record

    Returns:
        np.ndarray: Structured array copy of the requested records
    """
    if fields:
        present = dataset.dtype.names or ()
        missing = [name for name in fields if name not in present]
        if missing:
            print(
                f"Warning: field(s) {', '.join(missing)} not in {dataset.name}; "
                "reading full galaxy records"
            )
        else:
            return np.array(dataset.fields(list(fields))[:])

    return np.array(dataset[:])


def read_hdf5_snapshot(filename, snapshot_num, fields=None):
    """
    Read halos from a specific snapshot in an HDF5 file.

    Args:
        filename (str or Path): Path to HDF5 file (individual or master)
        snapshot_num (int): Snapshot number to read
        fields (list[str], optional): Galaxy fields to read. Defaults to None,
            which reads every field of the record.

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
                return _read_galaxies(dataset, fields)
            else:
                # Master file format - need to read from all File subgroups
                # Iterate over actual File subgroups instead of assuming File000, File001, etc.
                halos_list = []
                for key in sorted(snap_group.keys(), key=_file_group_sort_key):
                    if key.startswith("File"):
                        file_group = snap_group[key]
                        if "Galaxies" in file_group:
                            dataset = file_group["Galaxies"]
                            halos_list.append(_read_galaxies(dataset, fields))

                if not halos_list:
                    return None

                return np.concatenate(halos_list)

    except (OSError, KeyError, ValueError) as e:
        print(f"Warning: Could not read snapshot {snapshot_num} from {filename}: {e}")
        return None
