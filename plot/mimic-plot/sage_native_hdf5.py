#!/usr/bin/env python

"""
==============================================================================
SAGE-NATIVE-HDF5 reader (isolated add-on)
==============================================================================

Reads native sage-model (https://github.com/sage-home/sage-model) HDF5
output and returns it in the same Mimic-shaped recarray that the rest of
mimic-plot expects. Activated via `--input-format=sage-hdf5` on
mimic-plot.py.

Purpose
-------
Allow direct comparison between native SAGE output and the Mimic SAGE port
by running the same plotting code on both. Helpful for debugging the Mimic
SAGE module set.

Native SAGE HDF5 layout reference
---------------------------------
- Per-core files: `<base>_<file_num>.hdf5`
- Snapshot groups: `Snap_<snap_num>` (no zero padding, e.g. `Snap_63`)
- Each property is stored as a separate 1-D dataset within a snapshot group
- Position/Velocity/Spin are stored as separate scalar components
  (Posx/Posy/Posz, Velx/Vely/Velz, Spinx/Spiny/Spinz) rather than as
  (N, 3) arrays
- Galaxy SFR is split into `SfrDisk` + `SfrBulge`; Mimic uses a single
  combined `StarFormationRate`

This module is intentionally self-contained. To remove SAGE-native support
later:
  1. Delete this file.
  2. Remove the `>>> SAGE-NATIVE-HDF5 >>>` ... `<<< SAGE-NATIVE-HDF5 <<<`
     blocks in `mimic-plot.py` (the `--input-format` argument, the import,
     and the dispatch inside `read_data`).
==============================================================================
"""

import glob
import os
import sys

import numpy as np

try:
    import h5py
    H5PY_AVAILABLE = True
except ImportError:
    H5PY_AVAILABLE = False

# Use the run-local Mimic output schema so the recarray returned here is shaped
# like the Mimic output being compared against.
from output_schema import dtype_from_schema, load_schema


# Renamed-only properties. Field names that are identical in SAGE and Mimic
# do not need an entry here.
SAGE_TO_MIMIC_FIELD = {
    "IntraClusterStars":         "ICS",
    "EjectedMass":               "EjectedGas",
    "MetalsIntraClusterStars":   "MetalsICS",
    "MetalsEjectedMass":         "MetalsEjectedGas",
    "OutflowRate":               "SupernovaOutflowRate",
    "DiskRadius":                "DiskScaleRadius",
    "GalaxyIndex":               "UniqueGalaxyID",
    "CentralGalaxyIndex":        "UniqueCentralGalaxyID",
    "SimulationHaloIndex":       "MostBoundID",
}

# Vector fields that SAGE stores as scalar x/y/z components and Mimic
# stores as a single (3,) sub-field.
SAGE_VECTOR_FIELDS = {
    "Pos":  ("Posx",  "Posy",  "Posz"),
    "Vel":  ("Velx",  "Vely",  "Velz"),
    "Spin": ("Spinx", "Spiny", "Spinz"),
}


def _mimic_to_sage_lookup(mimic_dtype):
    """Build a {mimic_field_name -> sage_source} lookup.

    `sage_source` is either a string (single dataset name) or a 3-tuple of
    component dataset names for vector fields.
    """
    rename_inv = {mimic: sage for sage, mimic in SAGE_TO_MIMIC_FIELD.items()}
    lookup = {}
    for name in mimic_dtype.names:
        if name in SAGE_VECTOR_FIELDS:
            lookup[name] = SAGE_VECTOR_FIELDS[name]
        elif name in rename_inv:
            lookup[name] = rename_inv[name]
        else:
            lookup[name] = name
    return lookup


def _read_sage_snapshot_group(snap, snapshot_num, mimic_dtype):
    """Convert an open Snap_<N> h5py group into a Mimic-shaped ndarray.

    Returns None when the group has no usable datasets.
    """
    present = set(snap.keys())

    # Determine galaxy count from any scalar dataset
    num_gals = 0
    for k in snap.keys():
        obj = snap[k]
        if isinstance(obj, h5py.Dataset) and obj.ndim >= 1:
            num_gals = int(obj.shape[0])
            break
    if num_gals == 0:
        return None

    galaxies = np.zeros(num_gals, dtype=mimic_dtype)
    lookup = _mimic_to_sage_lookup(mimic_dtype)

    for mimic_name in mimic_dtype.names:
        src = lookup[mimic_name]
        if isinstance(src, tuple):
            cx, cy, cz = src
            if cx in present and cy in present and cz in present:
                galaxies[mimic_name][:, 0] = np.asarray(snap[cx][:])
                galaxies[mimic_name][:, 1] = np.asarray(snap[cy][:])
                galaxies[mimic_name][:, 2] = np.asarray(snap[cz][:])
        elif src in present:
            galaxies[mimic_name] = np.asarray(snap[src][:])
        # Unmatched fields stay zero-filled (e.g. Mimic-only fields).

    # SAGE stores SFR split between disc and bulge; Mimic uses a single
    # combined StarFormationRate. Aggregate here so SFR-dependent plots
    # see SAGE data the same way they would see Mimic data.
    if "StarFormationRate" in mimic_dtype.names and "SfrDisk" in present:
        disk = np.asarray(snap["SfrDisk"][:])
        bulge = (np.asarray(snap["SfrBulge"][:])
                 if "SfrBulge" in present else 0.0)
        galaxies["StarFormationRate"] = disk + bulge

    # Stamp the snapshot number on every row regardless of whether the SAGE
    # file exported a per-galaxy SnapNum dataset (it sometimes does not).
    if "SnapNum" in mimic_dtype.names:
        galaxies["SnapNum"] = snapshot_num

    return galaxies


def _read_sage_snapshot_file(filename, snapshot_num, mimic_dtype, verbose=False):
    """Read one SAGE per-rank file's snapshot into a Mimic-shaped ndarray."""
    snap_key = f"Snap_{snapshot_num}"

    try:
        with h5py.File(filename, "r") as f:
            if snap_key not in f:
                return None
            return _read_sage_snapshot_group(f[snap_key], snapshot_num, mimic_dtype)
    except (OSError, KeyError, ValueError) as e:
        if verbose:
            print(f"Warning: could not read SAGE file {filename} ({snap_key}): {e}")
        return None


def _read_sage_master_file(filename, snapshot_num, mimic_dtype, verbose=False):
    """Read SAGE's optional master file with ``Core_<N>/Snap_<M>/...`` layout.

    Returns a list of per-core arrays, or None if the file does not look
    like a master file (no Core_* groups).
    """
    snap_key = f"Snap_{snapshot_num}"
    try:
        with h5py.File(filename, "r") as f:
            core_keys = sorted(k for k in f.keys() if k.startswith("Core_"))
            if not core_keys:
                return None
            chunks = []
            for ck in core_keys:
                core = f[ck]
                if snap_key not in core:
                    continue
                chunk = _read_sage_snapshot_group(core[snap_key], snapshot_num, mimic_dtype)
                if chunk is not None and len(chunk) > 0:
                    chunks.append(chunk)
                    if verbose:
                        print(f"  Read {len(chunk)} galaxies from {filename}:{ck}/{snap_key}")
            return chunks
    except (OSError, KeyError, ValueError) as e:
        if verbose:
            print(f"Warning: could not read SAGE master file {filename}: {e}")
        return None


def _resolve_snapshot_number(model_path, params, output_dir, verbose=False, quiet=False):
    """Extract snapshot number from a Mimic-style model_path.

    mimic-plot constructs model paths like '<dir>/<base>_z<redshift>'. SAGE
    HDF5 files store all snapshots in one file per core, so we only need
    the integer snapshot number. Map the embedded redshift back via the
    standard mimic-plot SnapshotRedshiftMapper.
    """
    from snapshot_redshift_mapper import SnapshotRedshiftMapper

    base_name = os.path.basename(model_path)
    if "_z" in base_name:
        redshift_str = "_z" + base_name.split("_z", 1)[1]
    else:
        redshift_str = None

    mapper_params = params.copy()
    mapper_params["quiet"] = quiet
    mapper_params["verbose"] = verbose
    mapper = SnapshotRedshiftMapper(None, mapper_params, output_dir)

    if redshift_str is None:
        # No redshift in path; fall back to first OutputSnapshot if present
        out_snaps = params.get("OutputSnapshots") or []
        if out_snaps:
            return int(out_snaps[0])
        print("ERROR: Cannot infer SAGE snapshot number from model path "
              f"'{model_path}' and no OutputSnapshots in parameter file.")
        sys.exit(1)

    try:
        idx = mapper.redshift_strs.index(redshift_str)
    except ValueError:
        print(f"ERROR: redshift string {redshift_str} not found in snapshot mapping.")
        sys.exit(1)
    return int(mapper.snapshots[idx])


def read_data_sage_native(model_path, first_file, last_file, params,
                          verbose=False, quiet=False):
    """Read SAGE-native HDF5 output for one snapshot.

    Designed to be a drop-in alternative to `read_data()` in mimic-plot.py
    when `--input-format=sage-hdf5` is set. Returns the same
    `(galaxies, volume, metadata)` tuple as the Mimic readers.

    Args:
        model_path: Mimic-style model file base, e.g. '<dir>/model_z0.000'.
            The redshift suffix selects which snapshot is read; the actual
            on-disk file pattern is '<dir>/model_<N>.hdf5'.
        first_file, last_file: SAGE per-core file index range to read.
        params: parsed Mimic parameter dictionary.
        verbose, quiet: output-level flags.
    """
    if not H5PY_AVAILABLE:
        print("ERROR: h5py is required to read SAGE-native HDF5 output. "
              "Install with: pip install h5py")
        sys.exit(1)

    hubble_h = params["Hubble_h"]
    box_size = params["BoxSize"]
    output_dir = os.path.dirname(model_path)
    mimic_dtype = dtype_from_schema(load_schema(output_dir), binary=False)

    base_name = os.path.basename(model_path)
    # Strip mimic-plot's redshift suffix (the SAGE files do not carry one).
    file_base = base_name.split("_z", 1)[0] if "_z" in base_name else base_name

    snapshot_num = _resolve_snapshot_number(
        model_path, params, output_dir, verbose=verbose, quiet=quiet
    )
    if verbose:
        print(f"SAGE-native reader: Snap_{snapshot_num} from "
              f"{file_base}_<N>.hdf5 in {output_dir}")

    chunks = []
    total = 0
    good_files = 0
    used_master = False

    # SAGE produces per-rank files <base>_<N>.hdf5 with Snap_<n> groups at the
    # root (what allresults-*.py and sage-plot.py read). Optionally task 0
    # also writes a master file <base>.hdf5 with Core_<N>/Snap_<n>/... links.
    # Prefer the master file when present; fall back to per-rank files.
    master_path = os.path.join(output_dir, f"{file_base}.hdf5")
    if os.path.isfile(master_path):
        master_chunks = _read_sage_master_file(
            master_path, snapshot_num, mimic_dtype, verbose
        )
        if master_chunks:
            chunks = master_chunks
            total = sum(len(c) for c in chunks)
            good_files = len(chunks)
            used_master = True
            if verbose:
                print(f"  Master file {master_path}: aggregated {good_files} cores, "
                      f"{total} galaxies")

    if not chunks:
        for fnr in range(first_file, last_file + 1):
            fname = os.path.join(output_dir, f"{file_base}_{fnr}.hdf5")
            if not os.path.isfile(fname):
                continue
            chunk = _read_sage_snapshot_file(fname, snapshot_num, mimic_dtype, verbose)
            if chunk is None or len(chunk) == 0:
                continue
            chunks.append(chunk)
            total += len(chunk)
            good_files += 1
            if verbose:
                print(f"  Read {len(chunk)} galaxies from {fname} (Snap_{snapshot_num})")

    if not chunks:
        raise FileNotFoundError(
            f"No SAGE HDF5 data found for snapshot {snapshot_num} in {output_dir} "
            f"(tried master file {file_base}.hdf5 and per-rank "
            f"{file_base}_<{first_file}..{last_file}>.hdf5)"
        )

    galaxies = np.concatenate(chunks).view(np.recarray)

    # Determine total output files from the data itself — not from
    # NumSimulationTreeFiles, which in a SAGE .par file counts input merger
    # tree files and is unrelated to how many output rank files SAGE wrote.
    if used_master:
        # Master file is self-describing: count Core_ groups directly.
        try:
            with h5py.File(master_path, "r") as _f:
                total_output_files = len([k for k in _f.keys()
                                          if k.startswith("Core_")])
        except (OSError, KeyError):
            total_output_files = good_files
    else:
        # Per-rank files: count all rank files that exist on disk.
        total_output_files = len(glob.glob(
            os.path.join(output_dir, f"{file_base}_*.hdf5")
        ))

    volume = box_size ** 3.0
    if total_output_files > 0 and good_files > 0:
        volume = volume * good_files / total_output_files
        if verbose:
            print(f"  Volume fraction: {good_files}/{total_output_files} = "
                  f"{good_files / total_output_files:.4f}")

    metadata = {
        "hubble_h": hubble_h,
        "box_size": box_size,
        "volume": volume,
        "ntrees": 0,         # SAGE HDF5 output does not expose Ntrees at this level
        "ngals": total,
        "good_files": good_files,
        "snapshot": snapshot_num,
    }
    return galaxies, volume, metadata
