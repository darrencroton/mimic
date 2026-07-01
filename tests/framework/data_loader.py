#!/usr/bin/env python3
"""
Data Loader for Mimic Binary Output

Provides utilities for loading and validating Mimic binary output files.
Used by scientific tests to validate halo properties.

"""

import sys
from pathlib import Path

import numpy as np

_mimic_root = Path(__file__).resolve().parent.parent.parent
_plot_path = _mimic_root / "plot" / "mimic-plot"
if str(_plot_path) not in sys.path:
    sys.path.insert(0, str(_plot_path))

from output_schema import dtype_from_schema, load_schema


def get_halo_dtype(output_path):
    """
    Return the NumPy dtype for Mimic halo data from run-local schema metadata.

    Binary outputs must be kept with their metadata/output_schema.json file.
    """
    return dtype_from_schema(load_schema(output_path), binary=True)


def load_binary_halos(file_path):
    """
    Load halos from a Mimic binary output file.

    Args:
        file_path (str or Path): Path to binary output file

    Returns:
        tuple: (halos, metadata)
            halos: NumPy recarray containing halo data
            metadata: Dictionary with file metadata (Ntrees, TotHalos, etc.)

    Raises:
        FileNotFoundError: If file doesn't exist
        ValueError: If file format is invalid
    """
    file_path = Path(file_path)

    if not file_path.exists():
        raise FileNotFoundError(f"Binary file not found: {file_path}")

    if file_path.stat().st_size == 0:
        raise ValueError(f"Binary file is empty: {file_path}")

    # Get halo dtype
    dtype = get_halo_dtype(file_path)

    # Read file
    with open(file_path, "rb") as f:
        # Read header
        Ntrees = np.fromfile(f, np.int32, 1)[0]
        TotHalos = np.fromfile(f, np.int32, 1)[0]

        # Validate header
        if Ntrees < 0 or Ntrees > 1000000:
            raise ValueError(f"Invalid Ntrees value: {Ntrees}")
        if TotHalos < 0 or TotHalos > 100000000:
            raise ValueError(f"Invalid TotHalos value: {TotHalos}")

        # Read halos per tree array
        halos_per_tree = np.fromfile(f, np.int32, Ntrees)

        # Validate consistency
        sum_halos = np.sum(halos_per_tree)
        if sum_halos != TotHalos:
            raise ValueError(
                f"Inconsistent header: sum of halos per tree ({sum_halos}) "
                f"!= TotHalos ({TotHalos})"
            )

        # Read halo data
        halos = np.fromfile(f, dtype, TotHalos)

        # Verify we read the expected number
        if len(halos) != TotHalos:
            raise ValueError(f"Expected {TotHalos} halos, but read {len(halos)}")

    # Convert to recarray for attribute access
    halos = halos.view(np.recarray)

    # Create metadata dictionary
    metadata = {
        "Ntrees": Ntrees,
        "TotHalos": TotHalos,
        "halos_per_tree": halos_per_tree,
        "file_path": str(file_path),
    }

    return halos, metadata


def _count_matching(halos, predicate):
    """Count predicate hits per float field (scalar and vector fields alike)."""
    counts = {}
    for field in halos.dtype.names:
        if np.issubdtype(halos.dtype[field], np.floating):
            count = int(np.sum(predicate(halos[field])))
            if count > 0:
                counts[field] = count
    return counts


def validate_no_nans(halos):
    """
    Check that no halo properties contain NaN values.

    Args:
        halos: NumPy recarray of halo data

    Returns:
        dict: {field_name: count_of_nans} for fields with NaNs, empty if all clean
    """
    return _count_matching(halos, np.isnan)


def validate_no_infs(halos):
    """
    Check that no halo properties contain infinite values.

    Args:
        halos: NumPy recarray of halo data

    Returns:
        dict: {field_name: count_of_infs} for fields with infs, empty if all clean
    """
    return _count_matching(halos, np.isinf)


def find_nonfinite(halos, max_examples=5):
    """
    Locate NaN and Inf values in all float fields, with examples for scalars.

    Richer sibling of validate_no_nans/validate_no_infs for diagnostic
    reporting (used by the metadata-driven scientific tier).

    Args:
        halos: NumPy recarray of halo data
        max_examples: Example (index, value) pairs to collect per scalar field

    Returns:
        dict: {"nan": {field: {"count": int, "examples": [(idx, val), ...]}},
               "inf": {...}} -- inner dicts are empty when the data is clean
    """

    def scan(predicate):
        fields = {}
        for field in halos.dtype.names:
            if not np.issubdtype(halos.dtype[field], np.floating):
                continue
            data = halos[field]
            mask = predicate(data)
            count = int(np.sum(mask))
            if count == 0:
                continue
            examples = []
            if data.ndim == 1 and max_examples:
                indices = np.where(mask)[0][:max_examples]
                examples = [(int(i), float(data[i])) for i in indices]
            fields[field] = {"count": count, "examples": examples}
        return fields

    return {"nan": scan(np.isnan), "inf": scan(np.isinf)}


def assert_no_nans(halos):
    """Assert that no float field contains NaN values."""
    bad = validate_no_nans(halos)
    assert not bad, f"NaN values found: {bad}"


def assert_no_infs(halos):
    """Assert that no float field contains infinite values."""
    bad = validate_no_infs(halos)
    assert not bad, f"Inf values found: {bad}"


def assert_range(halos, field, min_val, max_val):
    """Assert that a field's values lie within [min_val, max_val] inclusive."""
    result = validate_range(halos, field, min_val, max_val)
    assert result["passed"], (
        f"{field} outside [{min_val}, {max_val}]: "
        f"{result['count_below']} below (examples {result['examples_below']}), "
        f"{result['count_above']} above (examples {result['examples_above']})"
    )


def validate_range(halos, field, min_val, max_val):
    """
    Validate that a field's values are within expected range.

    Args:
        halos: NumPy recarray of halo data
        field: Field name to check
        min_val: Minimum allowed value (inclusive)
        max_val: Maximum allowed value (inclusive)

    Returns:
        dict: Validation results with keys:
            - 'passed': bool
            - 'count_below': number of values below min_val
            - 'count_above': number of values above max_val
            - 'min_value': actual minimum value in data
            - 'max_value': actual maximum value in data
            - 'examples_below' / 'examples_above': (index, value) tuples for
              the first few violations of each kind
    """
    data = halos[field]

    # Handle vector fields by checking magnitude
    if data.ndim > 1:
        data = np.linalg.norm(data, axis=1)

    # Count violations
    below_mask = data < min_val
    above_mask = data > max_val

    count_below = np.sum(below_mask)
    count_above = np.sum(above_mask)

    # Get example violations (first 5 of each type)
    examples_below = []
    examples_above = []

    if count_below > 0:
        indices = np.where(below_mask)[0][:5]
        examples_below = [(int(i), float(data[i])) for i in indices]

    if count_above > 0:
        indices = np.where(above_mask)[0][:5]
        examples_above = [(int(i), float(data[i])) for i in indices]

    return {
        "passed": (count_below == 0 and count_above == 0),
        "count_below": int(count_below),
        "count_above": int(count_above),
        "min_value": float(np.min(data)),
        "max_value": float(np.max(data)),
        "examples_below": examples_below,
        "examples_above": examples_above,
    }


# ---------------------------------------------------------------------------
# HDF5 output loading and schema-layout validation
# ---------------------------------------------------------------------------


def load_hdf5_halos(output_file):
    """
    Load halo data from HDF5 output file

    Args:
        output_file (Path): Path to HDF5 output file

    Returns:
        tuple: (halos, metadata) where halos is structured array
    """

    try:
        import h5py
    except ImportError:
        raise ImportError("h5py not available - cannot load HDF5 output")

    with h5py.File(output_file, "r") as f:
        # Mimic HDF5 structure: Root contains snapshot groups (e.g., 'Snap063')
        # Each snapshot group contains 'Galaxies' dataset (structured array)

        # Get snapshot groups (e.g., 'Snap063')
        snap_groups = [key for key in f.keys() if key.startswith("Snap")]

        if not snap_groups:
            raise ValueError(f"No snapshot groups found in HDF5 file: {output_file}")

        # For testing, we expect one snapshot (Snap063 for z=0)
        # Use the first snapshot group found
        snap_name = snap_groups[0]
        snap_group = f[snap_name]

        # Read halo data from 'Galaxies' dataset
        if "Galaxies" not in snap_group:
            raise ValueError(f"No 'Galaxies' dataset found in {snap_name}")

        # Load the structured array directly
        halos = snap_group["Galaxies"][:]

        # Get metadata from group attributes
        attrs = dict(snap_group.attrs) if hasattr(snap_group, "attrs") else {}

        # Also check for TreeHalosPerSnap to get tree count
        ntrees = len(snap_group["TreeHalosPerSnap"][:]) if "TreeHalosPerSnap" in snap_group else 1

        # Create metadata
        metadata = {
            "TotHalos": len(halos),
            "Ntrees": ntrees,
            "NoutputSnaps": 1,
            "SnapshotName": snap_name,
        }
        metadata.update(attrs)

    # Convert to recarray for attribute access (outside the 'with' block)
    halos = halos.view(np.recarray)

    return halos, metadata


def decode_hdf5_string(value):
    """Decode scalar or one-element HDF5 string attributes."""
    if isinstance(value, np.ndarray):
        value = value[0]
    if isinstance(value, bytes):
        return value.decode()
    return str(value)


def _decode_hdf5_attr(value):
    """Return ordinary Python scalars for HDF5 attributes."""
    if isinstance(value, np.ndarray):
        if value.shape in ((), (1,)):
            value = value.item()
        else:
            return value
    if isinstance(value, bytes):
        return value.decode()
    if isinstance(value, np.generic):
        return value.item()
    return value


def load_hdf5_run_properties(output_file):
    """Load master-file RunProperties attributes from a Mimic HDF5 output."""
    try:
        import h5py
    except ImportError:
        raise ImportError("h5py not available - cannot load HDF5 run properties")

    with h5py.File(output_file, "r") as f:
        if "RunProperties" not in f:
            raise ValueError(f"No RunProperties group found in HDF5 file: {output_file}")
        return {key: _decode_hdf5_attr(value) for key, value in f["RunProperties"].attrs.items()}


def assert_hdf5_schema_layout(output_file, expected_format_version="1.1"):
    """
    Validate the current Mimic HDF5 schema layout.

    FieldMetadata is intentionally written once per file under RunProperties.
    Snapshot-local copies are stale duplication and should not be reintroduced.
    """
    try:
        import h5py
    except ImportError:
        raise ImportError("h5py not available - cannot validate HDF5 schema")

    with h5py.File(output_file, "r") as f:
        assert "RunProperties" in f, "Missing RunProperties group"
        assert "FieldMetadata" in f["RunProperties"], "Missing RunProperties/FieldMetadata"
        assert "Version" in f["RunProperties"], "Missing RunProperties/Version"

        version_attrs = f["RunProperties/Version"].attrs
        assert "hdf5_format_version" in version_attrs, "Missing HDF5 format version attribute"
        actual_version = decode_hdf5_string(version_attrs["hdf5_format_version"])
        assert actual_version == expected_format_version, (
            f"HDF5 format version mismatch: expected {expected_format_version}, "
            f"got {actual_version}"
        )

        field_metadata = f["RunProperties/FieldMetadata"]
        assert field_metadata.shape[0] > 0, "FieldMetadata table is empty"
        assert {"field_name", "units", "description"}.issubset(
            field_metadata.dtype.fields
        ), "FieldMetadata table missing required fields"

        snap_groups = [key for key in f.keys() if key.startswith("Snap")]
        assert snap_groups, "No snapshot groups found"
        for snap_name in snap_groups:
            assert "FieldMetadata" not in f[snap_name], (
                f"{snap_name}/FieldMetadata should not exist; metadata belongs under "
                "RunProperties/FieldMetadata"
            )
