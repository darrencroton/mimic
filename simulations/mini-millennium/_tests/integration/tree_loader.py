#!/usr/bin/env python3
"""
Binary Tree Loader for Mimic

Provides utilities for loading Mimic binary tree input files (lhalo_binary format).
Mirrors the structure of data_loader.py (which loads output files).

This module enables validation of input tree properties against Mimic output,
ensuring the physics-agnostic data pipeline preserves simulation data correctly.

Author: Mimic Testing Team
Date: 2025-11-29
"""

from pathlib import Path

import numpy as np


def get_tree_dtype():
    """
    Return NumPy dtype for binary tree format (struct RawHalo).

    This dtype MUST exactly match the C struct RawHalo definition in
    src/include/types.h (lines 8-33). The struct contains 17 properties
    from the raw simulation merger tree data.

    IMPORTANT: align=True is CRITICAL to match C struct padding!
    Without this, the byte layout will be incorrect and reading will fail.

    Returns:
        np.dtype: NumPy dtype matching C struct RawHalo

    Example:
        >>> dtype = get_tree_dtype()
        >>> print(f"Struct size: {dtype.itemsize} bytes")
        >>> print(f"Fields: {dtype.names}")
    """
    return np.dtype(
        [
            # Merger tree pointers (20 bytes)
            ("Descendant", np.int32),
            ("FirstProgenitor", np.int32),
            ("NextProgenitor", np.int32),
            ("FirstHaloInFOFgroup", np.int32),
            ("NextHaloInFOFgroup", np.int32),
            # Properties of halo (68 bytes)
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
            # Original position in simulation tree files (16 bytes)
            ("SnapNum", np.int32),
            ("FileNr", np.int32),
            ("SubhaloIndex", np.int32),
            ("SubHalfMass", np.float32),
        ],
        align=True,
    )  # ← CRITICAL: C struct alignment!


def load_binary_tree(file_path):
    """
    Load halos from a Mimic binary tree file (lhalo_binary format).

    This function reads raw simulation merger tree data in the lhalo_binary
    format used by Mimic. The file format matches the C implementation in
    src/io/tree/binary.c.

    File format:
        Header:
            int32: Ntrees       (number of merger trees)
            int32: totNHalos    (total halos across all trees)
            int32[Ntrees]: NHalos per tree
        Data:
            RawHalo[totNHalos]: Sequential halo data

    Args:
        file_path (str or Path): Path to binary tree file

    Returns:
        tuple: (halos, metadata)
            halos: NumPy recarray containing tree halo data (struct RawHalo)
            metadata: Dictionary with file metadata:
                - 'Ntrees': Number of trees in file
                - 'totNHalos': Total halos across all trees
                - 'NHalos_per_tree': Array of halo counts per tree
                - 'file_path': Path to file (string)

    Raises:
        FileNotFoundError: If file doesn't exist
        ValueError: If file format is invalid or inconsistent

    Example:
        >>> halos, meta = load_binary_tree('tests/data/input/trees_063.0')
        >>> print(f"Loaded {meta['totNHalos']} halos from {meta['Ntrees']} trees")
        Loaded 175869 halos from 3432 trees
        >>> print(f"First halo: M_Crit200={halos[0].M_Crit200:.3f}, Pos={halos[0].Pos}")
        First halo: M_Crit200=12.345, Pos=[10.2 15.3 8.9]
    """
    file_path = Path(file_path)

    # Validate file exists
    if not file_path.exists():
        raise FileNotFoundError(f"Tree file not found: {file_path}")

    if file_path.stat().st_size == 0:
        raise ValueError(f"Tree file is empty: {file_path}")

    # Get dtype
    dtype = get_tree_dtype()

    with open(file_path, "rb") as f:
        # Read header
        Ntrees = np.fromfile(f, np.int32, 1)[0]
        totNHalos = np.fromfile(f, np.int32, 1)[0]

        # Validate header values are reasonable
        if Ntrees <= 0 or Ntrees > 1000000:
            raise ValueError(f"Invalid Ntrees: {Ntrees} (expected 1-1000000)")
        if totNHalos <= 0 or totNHalos > 100000000:
            raise ValueError(f"Invalid totNHalos: {totNHalos} (expected 1-100000000)")

        # Read tree sizes
        NHalos_per_tree = np.fromfile(f, np.int32, Ntrees)

        # Validate consistency
        sum_halos = np.sum(NHalos_per_tree)
        if sum_halos != totNHalos:
            raise ValueError(
                f"Inconsistent header: sum(NHalos_per_tree)={sum_halos} "
                f"!= totNHalos={totNHalos}"
            )

        # Read all halos
        halos = np.fromfile(f, dtype, totNHalos)

        # Verify we read the expected number
        if len(halos) != totNHalos:
            raise ValueError(
                f"Expected {totNHalos} halos, but read {len(halos)}. "
                f"This may indicate struct size mismatch (expected {dtype.itemsize} bytes per halo)."
            )

    # Convert to recarray for attribute access (halos.M_Crit200 instead of halos['M_Crit200'])
    halos = halos.view(np.recarray)

    # Create metadata dictionary
    metadata = {
        "Ntrees": Ntrees,
        "totNHalos": totNHalos,
        "NHalos_per_tree": NHalos_per_tree,
        "file_path": str(file_path),
    }

    return halos, metadata


def get_halos_by_snapshot(halos):
    """
    Group halos by snapshot number.

    This is useful for analyzing or validating halos at specific redshifts
    or for matching input tree halos to output halos by snapshot.

    Args:
        halos: NumPy recarray of tree halos (from load_binary_tree)

    Returns:
        dict: {SnapNum: np.array(indices)}
            Keys are snapshot numbers (int)
            Values are NumPy arrays of indices into the halos array

    Example:
        >>> halos, _ = load_binary_tree('trees_063.0')
        >>> by_snap = get_halos_by_snapshot(halos)
        >>> print(f"Snapshot 63 has {len(by_snap[63])} halos")
        Snapshot 63 has 52341 halos
        >>> # Access halos at snapshot 63
        >>> snap63_halos = halos[by_snap[63]]
    """
    snapshots = {}

    for idx, halo in enumerate(halos):
        snap = halo.SnapNum
        if snap not in snapshots:
            snapshots[snap] = []
        snapshots[snap].append(idx)

    # Convert lists to numpy arrays for efficient indexing
    return {snap: np.array(indices) for snap, indices in snapshots.items()}
