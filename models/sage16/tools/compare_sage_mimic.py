#!/usr/bin/env python3
"""
compare_sage_mimic.py
=====================

Standalone galaxy-by-galaxy comparison of native sage-model HDF5 output against
Mimic SAGE HDF5 output for one snapshot.

Purpose
-------
sage-model and Mimic's `sage16` model are intended to implement identical SAGE
physics.  This script checks that claim after both codes have been run on the
same dark-matter halo merger trees.  It matches individual output galaxies,
compares all mapped physical properties, reports every material discrepancy, and
identifies the galaxies that concentrate the largest number of differences.

This is a reporting tool for human investigation.  It prints an overall verdict,
but deliberately exits with normal process status after producing the report.

Usage
-----
    python3 compare_sage_mimic.py

Edit the CONFIGURATION section below to change the snapshot number, tolerances,
input directories, skipped fields, field mappings, unit transforms, or explicit
sentinel-equivalence rules.  There is intentionally no command-line interface;
the script is meant to be copied or edited as a standalone analysis artifact.

Inputs
------
SAGE_DIR should point at native sage-model HDF5 output.  The loader accepts:
  - shard files named `model_<N>.hdf5` containing `Snap_<snapshot>/<field>`
  - a master file named `model.hdf5` containing `Core_<N>/Snap_<snapshot>/<field>`

MIMIC_DIR should point at Mimic HDF5 output.  The loader accepts:
  - shard files named `model_<NNN>.hdf5` containing `Snap<snapshot>/Galaxies`
  - a master file named `model.hdf5` containing
    `Snap<snapshot>/File<NNN>/Galaxies`

When both shard and master files exist, shard files are preferred so the same
galaxies are not double-counted.

Matching
--------
Matched galaxies are keyed by:
  - SAGE `SimulationHaloIndex`
  - Mimic `MostBoundID`

Both keys must be unique.  If duplicate keys are present, the script aborts with
example duplicate IDs because a duplicate would make the comparison ambiguous.
Unmatched galaxies are reported with IDs and basic halo position/mass context.

Property Comparison
-------------------
Properties are compared after applying the mapping rules below:
  - direct name matches
  - simple renames in PROPERTY_RENAMES
  - vector component mappings such as `Posx` -> `Pos[0]`
  - composite mappings such as `SfrDisk + SfrBulge` -> `StarFormationRate`
  - SAGE-only unit transforms in SAGE_VALUE_TRANSFORMS

Floating-point properties use relative difference:
    |sage - mimic| / max(|sage|, |mimic|, eps)

Values below NEAR_ZERO_THRESHOLD are treated as zero before computing the
relative difference.  Integer properties require exact equality.

Two tolerances are reported:
  - HIGH_TOLERANCE: strict parity threshold
  - LOW_TOLERANCE: relaxed threshold for larger, investigation-grade differences

Sentinels and Repeated Values
-----------------------------
Some sage-model fields can keep an internal unset sentinel and then convert that
sentinel to physical units during output, while Mimic may preserve a cleaner
output sentinel.  Those equivalent "both unset" cases should not be counted as
physics differences.

Only explicit rules, currently implemented in `explicit_sentinel_masks()`, can
exclude a mismatch from pass/fail.  Frequency-based repeated-value detection is
diagnostic only; it is never used to suppress failures.  This protects real
categorical or common physical values, such as `Type = 0/1` or zero gas mass,
from being accidentally treated as sentinels.

Output
------
  1. Galaxy count and matching check
  2. Property coverage: direct, renamed, vector, composite, unmatched, skipped
  3. Per-property high/low tolerance comparison with top discrepant galaxies
  4. Most problematic galaxies ranked by discrepancy count, including SAGE and
     Mimic IDs so they can be found in the output data
  5. Comprehensive summary with count, coverage, tolerance, repeated-value,
     problematic-galaxy, and overall verdict sections

Unmatched fields are reported but do not fail the overall verdict; they may be
intentional approved schema differences between sage-model and Mimic.  Review
the unmatched-field list whenever mappings or output schemas change.
"""

import glob
import os
import sys
from collections import defaultdict

import h5py
import numpy as np

# =============================================================================
# CONFIGURATION — edit these values as needed
# =============================================================================

SNAPSHOT = 63  # Snapshot number to compare [63, 37, 32, 27, 23, 20, 18, 16]
HIGH_TOLERANCE = 1e-6  # Strict: report count + top-10 worst violators per property
LOW_TOLERANCE = 1e-3  # Relaxed: report count only

# Values with |x| < NEAR_ZERO_THRESHOLD are treated as exactly zero before
# computing relative differences.  This prevents floating-point noise around
# zero (e.g. ±1e-22) from producing spurious max rel-diff ≈ 2.0 failures.
NEAR_ZERO_THRESHOLD = 1e-15

# Repeated-value diagnostics.
# A value that appears in ≥ SENTINEL_MIN_FRACTION of galaxies AND at least
# SENTINEL_MIN_COUNT times is reported as a repeated value.  This helps reveal
# possible sentinel / uninitialised defaults, but does not affect pass/fail.
SENTINEL_MIN_FRACTION = 0.01  # 1 % of galaxies
SENTINEL_MIN_COUNT = 100  # absolute floor

# Properties where repeated-value diagnostics are disabled.  These fields always
# carry meaningful values for every galaxy, or are categorical by construction,
# so a dominant repeated value is not evidence of an uninitialised default.
REPEATED_VALUE_EXCLUDE_FIELDS = {
    "Mvir",
    "CentralMvir",
    "Vvir",
    "Rvir",
    "Len",
    "SnapNum",
    "Type",
    "dT",
}

# Explicit sentinel equivalence rules.  These rules are the only way a mismatch
# can be excluded from pass/fail.  Repeated-value detection is diagnostic only.
#
# SAGE initialises merger times to -1 in internal time units and converts that
# value on output; Mimic preserves its cleaner output sentinel of 0.  If both
# sides are in those unset states, the property was not touched by either code
# and should not count as a physics difference.  If only one side is unset, it
# remains a violation.
MERGER_TIME_FIELDS = {
    "TimeOfLastMajorMerger",
    "TimeOfLastMinorMerger",
}

SAGE_DIR = "sage-model/output/millennium"
MIMIC_DIR = "mimic/output/sage16-mini-millennium"

# ---------------------------------------------------------------------------
# Fields to skip entirely (ID/index fields that differ by design)
# ---------------------------------------------------------------------------
SAGE_SKIP_FIELDS = {
    "GalaxyIndex",
    "SAGEHaloIndex",
    "SAGETreeIndex",
    "CentralGalaxyIndex",
    "SimulationHaloIndex",  # used as the matching key — not compared as a value
}

MIMIC_SKIP_FIELDS = {
    "UniqueGalaxyID",
    "UniqueCentralGalaxyID",
    "MostBoundID",  # used as the matching key — not compared as a value
}

# ---------------------------------------------------------------------------
# Simple property renames:  sage_name → mimic_name
# ---------------------------------------------------------------------------
PROPERTY_RENAMES = {
    "DiskRadius": "DiskScaleRadius",
    "EjectedMass": "EjectedGas",
    "IntraClusterStars": "ICS",
    "MetalsEjectedMass": "MetalsEjectedGas",
    "MetalsIntraClusterStars": "MetalsICS",
    "OutflowRate": "SupernovaOutflowRate",
}

# Field-specific unit transforms applied before comparison.  Native SAGE HDF5
# writes merger times in Myr, while Mimic writes the same output fields in Gyr.
SAGE_VALUE_TRANSFORMS = {
    "TimeOfLastMajorMerger": (lambda values: values.astype(np.float64) / 1000.0, "SAGE Myr → Gyr"),
    "TimeOfLastMinorMerger": (lambda values: values.astype(np.float64) / 1000.0, "SAGE Myr → Gyr"),
}

# ---------------------------------------------------------------------------
# Vector component mappings:  sage_name → (mimic_field, component_index)
# mimic stores Pos/Vel/Spin as shape-(N,3) arrays; sage uses separate x/y/z.
# ---------------------------------------------------------------------------
VECTOR_COMPONENTS = {
    "Posx": ("Pos", 0),
    "Posy": ("Pos", 1),
    "Posz": ("Pos", 2),
    "Velx": ("Vel", 0),
    "Vely": ("Vel", 1),
    "Velz": ("Vel", 2),
    "Spinx": ("Spin", 0),
    "Spiny": ("Spin", 1),
    "Spinz": ("Spin", 2),
}

# ---------------------------------------------------------------------------
# Composite sage → mimic:  tuple of sage fields (summed) → single mimic field
# ---------------------------------------------------------------------------
COMPOSITE_MAPPINGS = {
    ("SfrDisk", "SfrBulge"): "StarFormationRate",
}


# =============================================================================
# COLOUR HELPERS
# =============================================================================

_USE_COLOUR = sys.stdout.isatty()


def _c(text, code):
    return f"\033[{code}m{text}\033[0m" if _USE_COLOUR else str(text)


def green(t):
    return _c(t, "32")


def red(t):
    return _c(t, "31")


def yellow(t):
    return _c(t, "33")


def bold(t):
    return _c(t, "1")


def cyan(t):
    return _c(t, "36")


def magenta(t):
    return _c(t, "35")


PASS_STR = green("✓ PASS")
FAIL_STR = red("✗ FAIL")
WARN_STR = yellow("⚠ WARN")


# =============================================================================
# DATA LOADING
# =============================================================================


def _sort_hdf5_files(paths):
    """Sort HDF5 shards by filename, preserving numeric shard order."""

    def key(path):
        base = os.path.basename(path)
        stem, _ = os.path.splitext(base)
        parts = stem.rsplit("_", 1)
        if len(parts) == 2 and parts[1].isdigit():
            return (parts[0], int(parts[1]))
        return (stem, -1)

    return sorted(paths, key=key)


def _sage_output_files(directory):
    """Return SAGE shard files, or the master file if no shards are present."""
    shard_files = _sort_hdf5_files(glob.glob(os.path.join(directory, "model_[0-9]*.hdf5")))
    master_file = os.path.join(directory, "model.hdf5")
    return shard_files if shard_files else ([master_file] if os.path.exists(master_file) else [])


def _mimic_output_files(directory):
    """Return Mimic shard files, or the master file if no shards are present."""
    shard_files = _sort_hdf5_files(glob.glob(os.path.join(directory, "model_[0-9]*.hdf5")))
    master_file = os.path.join(directory, "model.hdf5")
    return shard_files if shard_files else ([master_file] if os.path.exists(master_file) else [])


def available_sage_snapshots(directory):
    """Return sorted snapshot numbers available in SAGE HDF5 output."""
    snapshots = set()
    for fpath in _sage_output_files(directory):
        with h5py.File(fpath, "r") as f:
            for key in f.keys():
                if key.startswith("Snap_"):
                    snapshots.add(int(key.split("_", 1)[1]))
                elif key.startswith("Core_"):
                    for subkey in f[key].keys():
                        if subkey.startswith("Snap_"):
                            snapshots.add(int(subkey.split("_", 1)[1]))
    return sorted(snapshots)


def available_mimic_snapshots(directory):
    """Return sorted snapshot numbers available in Mimic HDF5 output."""
    snapshots = set()
    for fpath in _mimic_output_files(directory):
        with h5py.File(fpath, "r") as f:
            for key in f.keys():
                if key.startswith("Snap") and key[4:].isdigit():
                    snapshots.add(int(key[4:]))
    return sorted(snapshots)


def _format_snapshot_list(snapshots):
    if not snapshots:
        return "(none)"
    return ", ".join(str(s) for s in snapshots)


def _mimic_metadata_config(directory):
    """Return the copied Mimic run config from an output directory, if present."""
    metadata_dir = os.path.join(directory, "metadata")
    if not os.path.isdir(metadata_dir):
        return None

    configs = sorted(glob.glob(os.path.join(metadata_dir, "*.yaml")))
    for path in configs:
        if os.path.basename(path).startswith("sage16_"):
            return path
    return configs[0] if configs else None


def _snapshot_list_line(config_path):
    """Return the snapshot_list line from a Mimic YAML config, if present."""
    if config_path is None:
        return None
    try:
        with open(config_path, "r", encoding="utf-8") as handle:
            for line in handle:
                stripped = line.strip()
                if stripped.startswith("snapshot_list:"):
                    return stripped
    except OSError:
        return None
    return None


def _validate_field_group(grp, source):
    """Return dataset field names and row count for a SAGE snapshot group."""
    fields = [name for name in grp.keys() if isinstance(grp[name], h5py.Dataset)]
    if not fields:
        raise ValueError(f"No datasets found in SAGE snapshot group {source}")

    lengths = {name: int(grp[name].shape[0]) for name in fields}
    unique_lengths = set(lengths.values())
    if len(unique_lengths) != 1:
        bad = ", ".join(f"{name}={length}" for name, length in sorted(lengths.items()))
        raise ValueError(f"Inconsistent dataset lengths in {source}: {bad}")

    return fields, unique_lengths.pop()


def load_sage(directory, snapshot):
    """Load sage-model data for *snapshot* from shard or master HDF5 output."""
    snap_key = f"Snap_{snapshot}"

    files = _sage_output_files(directory)
    if not files:
        raise FileNotFoundError(f"No SAGE model_*.hdf5 or model.hdf5 files in {directory}")

    arrays = defaultdict(list)
    files_loaded = []
    expected_fields = None

    def append_group(grp, source):
        nonlocal expected_fields
        fields, n_rows = _validate_field_group(grp, source)
        field_set = set(fields)
        if expected_fields is None:
            expected_fields = field_set
        elif field_set != expected_fields:
            missing = sorted(expected_fields - field_set)
            extra = sorted(field_set - expected_fields)
            raise ValueError(f"SAGE schema mismatch in {source}; missing={missing}, extra={extra}")

        if n_rows == 0:
            return False

        for field in sorted(fields):
            arrays[field].append(grp[field][...])
        return True

    for fpath in files:
        with h5py.File(fpath, "r") as f:
            loaded = False
            if snap_key in f:
                loaded = append_group(f[snap_key], f"{os.path.basename(fpath)}:{snap_key}")
            else:
                for core_name in sorted(k for k in f.keys() if k.startswith("Core_")):
                    core = f[core_name]
                    if snap_key in core:
                        loaded = (
                            append_group(
                                core[snap_key],
                                f"{os.path.basename(fpath)}:{core_name}/{snap_key}",
                            )
                            or loaded
                        )
            if loaded:
                files_loaded.append(os.path.basename(fpath))

    if not arrays:
        raise ValueError(f"Snapshot '{snap_key}' not found in any file under {directory}")

    data = {k: np.concatenate(v) for k, v in arrays.items()}
    return data, files_loaded


def load_mimic(directory, snapshot):
    """Load mimic data for *snapshot* from shard or master HDF5 output."""
    snap_key = f"Snap{snapshot:03d}"

    files = _mimic_output_files(directory)
    if not files:
        raise FileNotFoundError(f"No Mimic model_*.hdf5 or model.hdf5 files in {directory}")

    records = []
    files_loaded = []
    expected_dtype = None

    def append_records(dataset, source):
        nonlocal expected_dtype
        data = dataset[...]
        if expected_dtype is None:
            expected_dtype = data.dtype
        elif data.dtype != expected_dtype:
            raise ValueError(f"Mimic schema mismatch in {source}")
        if len(data) == 0:
            return False
        records.append(data)
        return True

    for fpath in files:
        with h5py.File(fpath, "r") as f:
            if snap_key not in f:
                continue
            grp = f[snap_key]
            loaded = False
            if "Galaxies" in grp:
                loaded = append_records(
                    grp["Galaxies"],
                    f"{os.path.basename(fpath)}:{snap_key}/Galaxies",
                )
            else:
                for file_name in sorted(k for k in grp.keys() if k.startswith("File")):
                    file_grp = grp[file_name]
                    if "Galaxies" in file_grp:
                        loaded = (
                            append_records(
                                file_grp["Galaxies"],
                                f"{os.path.basename(fpath)}:{snap_key}/{file_name}/Galaxies",
                            )
                            or loaded
                        )
            if loaded:
                files_loaded.append(os.path.basename(fpath))

    if not records:
        raise ValueError(f"Snapshot '{snap_key}' not found in any file under {directory}")

    data = np.concatenate(records)
    return data, files_loaded


# =============================================================================
# GALAXY MATCHING
# =============================================================================


def _duplicate_id_summary(ids, label):
    unique, counts = np.unique(ids, return_counts=True)
    duplicate_ids = unique[counts > 1]
    if len(duplicate_ids) == 0:
        return None

    examples = []
    for dup_id in duplicate_ids[:10]:
        positions = np.where(ids == dup_id)[0][:5]
        examples.append(f"{int(dup_id)} at rows {positions.tolist()}")
    return (
        f"{label} matching key contains {len(duplicate_ids)} duplicate IDs; "
        f"examples: {', '.join(examples)}"
    )


def match_galaxies(sage_data, mimic_data):
    """
    Match galaxies using SimulationHaloIndex (sage) ↔ MostBoundID (mimic).

    Returns
    -------
    sage_idx       : np.ndarray   — indices into sage_data for matched galaxies
    mimic_idx      : np.ndarray   — indices into mimic_data for matched galaxies
    common_ids     : list[int]    — shared halo IDs (same order as sage/mimic_idx)
    sage_only_ids  : list[int]    — halo IDs in sage but not mimic
    mimic_only_ids : list[int]    — halo IDs in mimic but not sage
    """
    sage_ids = sage_data["SimulationHaloIndex"].astype(np.int64)
    mimic_ids = mimic_data["MostBoundID"].astype(np.int64)

    duplicate_errors = [
        msg
        for msg in (
            _duplicate_id_summary(sage_ids, "SAGE SimulationHaloIndex"),
            _duplicate_id_summary(mimic_ids, "Mimic MostBoundID"),
        )
        if msg is not None
    ]
    if duplicate_errors:
        raise ValueError("\n".join(duplicate_errors))

    sage_id_map = {int(v): i for i, v in enumerate(sage_ids)}
    mimic_id_map = {int(v): i for i, v in enumerate(mimic_ids)}

    sage_id_set = set(sage_id_map)
    mimic_id_set = set(mimic_id_map)

    common_ids = sorted(sage_id_set & mimic_id_set)
    sage_only_ids = sorted(sage_id_set - mimic_id_set)
    mimic_only_ids = sorted(mimic_id_set - sage_id_set)

    sage_idx = np.array([sage_id_map[gid] for gid in common_ids], dtype=np.intp)
    mimic_idx = np.array([mimic_id_map[gid] for gid in common_ids], dtype=np.intp)

    return sage_idx, mimic_idx, common_ids, sage_only_ids, mimic_only_ids


# =============================================================================
# BUILD COMPARISON TASKS
# =============================================================================


def apply_sage_transform(label, values):
    """Apply any SAGE-to-Mimic output-unit transform for a comparison label."""
    transform = SAGE_VALUE_TRANSFORMS.get(label)
    if transform is None:
        return values, None

    func, note = transform
    return func(values), note


def explicit_sentinel_masks(label, sage_vals, mimic_vals):
    """Return explicit sentinel masks for fields with known equivalent sentinels."""
    if label in MERGER_TIME_FIELDS:
        sage_f = np.asarray(sage_vals, dtype=np.float64)
        mimic_f = np.asarray(mimic_vals, dtype=np.float64)
        return sage_f < 0.0, np.isclose(mimic_f, 0.0, rtol=0.0, atol=NEAR_ZERO_THRESHOLD)

    n = len(sage_vals)
    return np.zeros(n, bool), np.zeros(n, bool)


def build_comparison_tasks(sage_data, mimic_data, sage_idx, mimic_idx):
    """
    Construct the list of property comparisons to run.

    Each task is a dict:
        label            : str           display name
        sage_vals        : np.ndarray    sage values for matched galaxies
        mimic_vals       : np.ndarray    mimic values for matched galaxies
        mapping          : str           how fields were matched
        sage_sentinel_counts : dict      {value: count} dominant sage values
        mimic_sentinel_counts: dict      dominant mimic values
        sage_explicit_sentinel_mask : np.ndarray[bool]
        mimic_explicit_sentinel_mask: np.ndarray[bool]

    Repeated-value diagnostics run on the FULL arrays (all galaxies, not just
    matched) for maximum statistical power.  Pass/fail sentinel exclusions only
    use explicit per-field rules.

    Also returns lists of unmatched field names for reporting.
    """
    tasks = []
    sage_accounted = set(SAGE_SKIP_FIELDS)
    mimic_accounted = set(MIMIC_SKIP_FIELDS)

    mimic_field_names = set(mimic_data.dtype.names)
    sage_field_names = set(sage_data.keys())

    def _sent(label, sage_arr, mimic_arr):
        """Return repeated-value diagnostic counts for SAGE and Mimic.
        Returns empty dicts for fields excluded from repeated-value diagnostics."""
        if label in REPEATED_VALUE_EXCLUDE_FIELDS:
            return {}, {}
        sc, _ = detect_sentinels(sage_arr)
        mc, _ = detect_sentinels(mimic_arr)
        return sc, mc

    def _append_task(label, sage_full, mimic_full, mapping):
        sage_full, transform_note = apply_sage_transform(label, sage_full)
        if transform_note:
            mapping = f"{mapping}; {transform_note}"

        sc, mc = _sent(label, sage_full, mimic_full)
        sage_vals = sage_full[sage_idx]
        mimic_vals = mimic_full[mimic_idx]
        sage_mask, mimic_mask = explicit_sentinel_masks(label, sage_vals, mimic_vals)
        tasks.append(
            {
                "label": label,
                "sage_vals": sage_vals,
                "mimic_vals": mimic_vals,
                "mapping": mapping,
                "sage_sentinel_counts": sc,
                "mimic_sentinel_counts": mc,
                "sage_explicit_sentinel_mask": sage_mask,
                "mimic_explicit_sentinel_mask": mimic_mask,
            }
        )

    # --- Composite mappings (e.g. SfrDisk+SfrBulge → StarFormationRate) ---
    for sage_tuple, mimic_name in COMPOSITE_MAPPINGS.items():
        if all(s in sage_data for s in sage_tuple) and mimic_name in mimic_field_names:
            sage_full = sum(sage_data[s].astype(np.float64) for s in sage_tuple)
            mimic_full = mimic_data[mimic_name].astype(np.float64)
            label = " + ".join(sage_tuple)
            _append_task(label, sage_full, mimic_full, f"composite → {mimic_name}")
            sage_accounted.update(sage_tuple)
            mimic_accounted.add(mimic_name)

    # --- Vector component mappings (Posx → Pos[0], etc.) ---
    for sage_name, (mimic_field, comp) in VECTOR_COMPONENTS.items():
        if sage_name in sage_data and mimic_field in mimic_field_names:
            sage_full = sage_data[sage_name]
            mimic_full = mimic_data[mimic_field][:, comp]
            _append_task(sage_name, sage_full, mimic_full, f"vector → {mimic_field}[{comp}]")
            sage_accounted.add(sage_name)
            mimic_accounted.add(mimic_field)

    # --- Simple renames ---
    for sage_name, mimic_name in PROPERTY_RENAMES.items():
        if sage_name in sage_data and mimic_name in mimic_field_names:
            sage_full = sage_data[sage_name]
            mimic_full = mimic_data[mimic_name]
            _append_task(sage_name, sage_full, mimic_full, f"rename → {mimic_name}")
            sage_accounted.add(sage_name)
            mimic_accounted.add(mimic_name)

    # --- Direct name matches ---
    for field in sorted(sage_field_names - sage_accounted):
        if field in mimic_field_names:
            sage_full = sage_data[field]
            mimic_full = mimic_data[field]
            _append_task(field, sage_full, mimic_full, "direct")
            sage_accounted.add(field)
            mimic_accounted.add(field)

    sage_unmatched = sorted(sage_field_names - sage_accounted)
    mimic_unmatched = sorted(mimic_field_names - mimic_accounted)

    return tasks, sage_unmatched, mimic_unmatched


# =============================================================================
# PROPERTY COMPARISON
# =============================================================================


def relative_diff(a, b, eps=1e-30):
    """
    Element-wise relative difference:  |a - b| / max(|a|, |b|, eps).

    Values with |x| < NEAR_ZERO_THRESHOLD are zeroed first so that
    floating-point noise around zero (e.g. ±1e-22) does not produce
    spurious relative differences of ≈ 2.0.
    Works for any numeric dtype; promotes to float64 internally.
    """
    a = np.asarray(a, dtype=np.float64)
    b = np.asarray(b, dtype=np.float64)
    if NEAR_ZERO_THRESHOLD > 0:
        a = np.where(np.abs(a) < NEAR_ZERO_THRESHOLD, 0.0, a)
        b = np.where(np.abs(b) < NEAR_ZERO_THRESHOLD, 0.0, b)
    denom = np.maximum(np.abs(a), np.abs(b))
    denom = np.maximum(denom, eps)
    return np.abs(a - b) / denom


def detect_sentinels(arr):
    """
    Find values that appear suspiciously often in *arr*.

    These repeated values are diagnostics only.  They can reveal sentinel /
    uninitialised defaults, but they are not used for pass/fail suppression.

    Returns
    -------
    counts : dict  {float_value: int_count}   — dominant values and how often
    value_set : set                           — the same values as a set for O(1) lookup
    """
    n = len(arr)
    if n == 0:
        return {}, set()

    arr_f = np.asarray(arr, dtype=np.float64).ravel()
    finite = arr_f[np.isfinite(arr_f)]
    if len(finite) == 0:
        return {}, set()

    threshold = max(SENTINEL_MIN_COUNT, SENTINEL_MIN_FRACTION * n)
    unique, counts = np.unique(finite, return_counts=True)

    sent_counts = {float(v): int(c) for v, c in zip(unique, counts) if c >= threshold}
    return sent_counts, set(sent_counts.keys())


def compare_property(
    sage_vals,
    mimic_vals,
    common_ids,
    high_tol,
    low_tol,
    sage_explicit_sentinel_mask=None,
    mimic_explicit_sentinel_mask=None,
):
    """
    Compare two matched property arrays and return a statistics dict.

    Sentinel classification (three categories):
      sent-sent  : both sides are sentinel values → EXCLUDED from violation counts.
                   The assumption is both codes left this at the initialisation
                   default; the values are equivalent even if numerically different.
      sent-real  : one side is a sentinel, the other is a real physics value →
                   COUNTED as a violation; one code ran physics, the other didn't.
      genuine    : neither side is a sentinel → COUNTED as a violation.

    pass/fail is based on (sent-real + genuine) violations only.
    """
    n = len(sage_vals)
    is_int = np.issubdtype(sage_vals.dtype, np.integer) and np.issubdtype(
        mimic_vals.dtype, np.integer
    )

    if is_int:
        rdiff = (sage_vals != mimic_vals).astype(np.float64)
    else:
        rdiff = relative_diff(sage_vals, mimic_vals)

    # Build explicit sentinel membership masks.  Frequency-based repeated-value
    # detection is diagnostic only and is deliberately not used here.
    is_sage_sent = (
        np.asarray(sage_explicit_sentinel_mask, dtype=bool)
        if sage_explicit_sentinel_mask is not None
        else np.zeros(n, bool)
    )
    is_mimic_sent = (
        np.asarray(mimic_explicit_sentinel_mask, dtype=bool)
        if mimic_explicit_sentinel_mask is not None
        else np.zeros(n, bool)
    )

    is_both_sent = is_sage_sent & is_mimic_sent  # excluded
    is_sent_real = (is_sage_sent | is_mimic_sent) & ~is_both_sent  # kept, annotated
    # is_genuine    = ~is_sage_sent & ~is_mimic_sent        # kept, no annotation

    reported_rdiff = rdiff[~is_both_sent]
    max_rdiff = float(reported_rdiff.max()) if len(reported_rdiff) else 0.0
    mean_rdiff = float(reported_rdiff.mean()) if len(reported_rdiff) else 0.0

    viol_h = rdiff > high_tol
    viol_l = rdiff > low_tol

    n_high_sent_sent = int(np.sum(viol_h & is_both_sent))
    n_high_sent_real = int(np.sum(viol_h & is_sent_real))
    n_high_genuine = int(np.sum(viol_h & ~is_sage_sent & ~is_mimic_sent))
    n_high_real = n_high_sent_real + n_high_genuine  # what counts for pass/fail

    n_low_sent_sent = int(np.sum(viol_l & is_both_sent))
    n_low_sent_real = int(np.sum(viol_l & is_sent_real))
    n_low_genuine = int(np.sum(viol_l & ~is_sage_sent & ~is_mimic_sent))
    n_low_real = n_low_sent_real + n_low_genuine

    # Top-10 worst violators at high tolerance — exclude sent-sent rows
    top_10 = []
    if n_high_real > 0:
        sage_f = np.asarray(sage_vals, dtype=np.float64)
        mimic_f = np.asarray(mimic_vals, dtype=np.float64)
        # Only consider non-sent-sent violations for the ranked list
        eligible = viol_h & ~is_both_sent
        eligible_idx = np.where(eligible)[0]
        eligible_idx = eligible_idx[np.argsort(rdiff[eligible_idx])[::-1]]
        for i in eligible_idx[:10]:
            if is_sent_real[i]:
                sent_kind = "sage_sent" if is_sage_sent[i] else "mimic_sent"
            else:
                sent_kind = None
            top_10.append(
                {
                    "halo_id": common_ids[i],
                    "sage_val": float(sage_vals[i]),
                    "mimic_val": float(mimic_vals[i]),
                    "rel_diff": float(rdiff[i]),
                    "sent_kind": sent_kind,  # None, "sage_sent", or "mimic_sent"
                }
            )

    return {
        "n_total": n,
        # Real violation counts (sent-sent excluded)
        "n_high_real": n_high_real,
        "n_high_sent_real": n_high_sent_real,
        "n_high_genuine": n_high_genuine,
        "n_high_sent_sent": n_high_sent_sent,
        "n_low_real": n_low_real,
        "n_low_sent_real": n_low_sent_real,
        "n_low_genuine": n_low_genuine,
        "n_low_sent_sent": n_low_sent_sent,
        "max_rdiff": max_rdiff,
        "mean_rdiff": mean_rdiff,
        "top_10": top_10,
        "is_int": is_int,
    }


# =============================================================================
# PER-GALAXY DISCREPANCY AGGREGATION
# =============================================================================


def real_violation_masks(task, high_tol, low_tol):
    """Return high/low real-violation masks and relative differences for a task."""
    sage_vals = task["sage_vals"]
    mimic_vals = task["mimic_vals"]

    is_int = np.issubdtype(sage_vals.dtype, np.integer) and np.issubdtype(
        mimic_vals.dtype, np.integer
    )
    if is_int:
        rdiff = (sage_vals != mimic_vals).astype(np.float64)
    else:
        rdiff = relative_diff(sage_vals, mimic_vals)

    n = len(sage_vals)
    is_sage_sent = np.asarray(
        task.get("sage_explicit_sentinel_mask", np.zeros(n, bool)),
        dtype=bool,
    )
    is_mimic_sent = np.asarray(
        task.get("mimic_explicit_sentinel_mask", np.zeros(n, bool)),
        dtype=bool,
    )
    is_both_sent = is_sage_sent & is_mimic_sent

    high_real = (rdiff > high_tol) & ~is_both_sent
    low_real = (rdiff > low_tol) & ~is_both_sent
    return high_real, low_real, rdiff


def _maybe_int(value):
    """Convert scalar values to int strings where possible; return '-' if absent."""
    if value is None:
        return "-"
    try:
        return str(int(value))
    except (TypeError, ValueError):
        return str(value)


def _array_value(data, field, idx):
    if field not in data:
        return None
    return data[field][idx]


def _record_value(records, field, idx):
    if field not in records.dtype.names:
        return None
    return records[field][idx]


def build_problem_galaxy_summary(
    tasks, common_ids, sage_idx, mimic_idx, sage_data, mimic_data, high_tol, low_tol, limit=10
):
    """Find matched galaxies with the largest cross-property discrepancy footprint."""
    n = len(common_ids)
    high_counts = np.zeros(n, dtype=np.int32)
    low_counts = np.zeros(n, dtype=np.int32)
    max_rdiff = np.zeros(n, dtype=np.float64)
    high_props = defaultdict(list)
    low_props = defaultdict(list)

    for task in tasks:
        high_real, low_real, rdiff = real_violation_masks(task, high_tol, low_tol)
        high_idx = np.where(high_real)[0]
        low_idx = np.where(low_real)[0]

        high_counts[high_idx] += 1
        low_counts[low_idx] += 1
        np.maximum.at(max_rdiff, high_idx, rdiff[high_idx])

        label = task["label"]
        for i in high_idx:
            high_props[int(i)].append(label)
        for i in low_idx:
            low_props[int(i)].append(label)

    problem_idx = np.where(high_counts > 0)[0]
    if len(problem_idx) == 0:
        return [], 0

    # Low-tolerance failures are the largest discrepancies, so rank those first;
    # high-tolerance failures then capture broad but smaller drift.
    ordered = sorted(
        problem_idx,
        key=lambda i: (
            -int(low_counts[i]),
            -int(high_counts[i]),
            -float(max_rdiff[i]),
            int(common_ids[i]),
        ),
    )

    rows = []
    for i in ordered[:limit]:
        si = int(sage_idx[i])
        mi = int(mimic_idx[i])
        low_set = set(low_props.get(int(i), []))
        high_only = [p for p in high_props.get(int(i), []) if p not in low_set]
        rows.append(
            {
                "match_id": int(common_ids[i]),
                "sage_row": si,
                "mimic_row": mi,
                "sage_galaxy_index": _array_value(sage_data, "GalaxyIndex", si),
                "sage_tree_index": _array_value(sage_data, "SAGETreeIndex", si),
                "sage_halo_index": _array_value(sage_data, "SAGEHaloIndex", si),
                "mimic_unique_id": _record_value(mimic_data, "UniqueGalaxyID", mi),
                "mimic_most_bound_id": _record_value(mimic_data, "MostBoundID", mi),
                "high_count": int(high_counts[i]),
                "low_count": int(low_counts[i]),
                "max_rdiff": float(max_rdiff[i]),
                "low_props": sorted(low_set),
                "high_only_props": sorted(high_only),
            }
        )

    return rows, int(len(problem_idx))


# =============================================================================
# OUTPUT HELPERS
# =============================================================================

SEP_MAJOR = "=" * 80
SEP_MINOR = "-" * 80


def pct(n, total):
    """Format as 'n (x.xx%)'"""
    if total == 0:
        return f"{n} (n/a)"
    return f"{n} ({100.0 * n / total:.2f}%)"


def print_property_result(label, mapping, stats, high_tol, low_tol):
    """Print one block of per-property comparison output."""
    n = stats["n_total"]
    n_high_real = stats["n_high_real"]
    n_low_real = stats["n_low_real"]
    n_high_sr = stats["n_high_sent_real"]
    n_high_gen = stats["n_high_genuine"]
    n_high_ss = stats["n_high_sent_sent"]
    n_low_sr = stats["n_low_sent_real"]
    n_low_gen = stats["n_low_genuine"]
    n_low_ss = stats["n_low_sent_sent"]
    sage_sc = stats.get("sage_sentinel_counts", {})
    mimic_sc = stats.get("mimic_sentinel_counts", {})

    # Pass/fail based on real violations only (sent-sent excluded)
    high_pass = n_high_real == 0
    low_pass = n_low_real == 0

    high_status = PASS_STR if high_pass else FAIL_STR
    low_status = PASS_STR if low_pass else WARN_STR

    mapping_note = f"  [{mapping}]" if mapping != "direct" else ""
    print(f"\n  {bold(label)}{cyan(mapping_note)}")

    # High tolerance line
    print(f"    High tol ({high_tol:.0e}):  {pct(n_high_real, n):>18s}  {high_status}", end="")
    parts = []
    if n_high_gen > 0:
        parts.append(f"{n_high_gen} genuine")
    if n_high_sr > 0:
        parts.append(yellow(f"{n_high_sr} sent-real"))
    if n_high_ss > 0:
        parts.append(f"{n_high_ss} sent-sent excl.")
    if parts:
        print(f"  ({', '.join(parts)})", end="")
    print()

    # Low tolerance line
    print(f"    Low  tol ({low_tol:.0e}):  {pct(n_low_real,  n):>18s}  {low_status}", end="")
    parts = []
    if n_low_gen > 0:
        parts.append(f"{n_low_gen} genuine")
    if n_low_sr > 0:
        parts.append(yellow(f"{n_low_sr} sent-real"))
    if n_low_ss > 0:
        parts.append(f"{n_low_ss} sent-sent excl.")
    if parts:
        print(f"  ({', '.join(parts)})", end="")
    print()

    print(f"    Max rel-diff: {stats['max_rdiff']:.3e}   Mean: {stats['mean_rdiff']:.3e}")

    # Repeated-value diagnostic info
    if sage_sc or mimic_sc:
        print(f"    {yellow('Repeated values')} (diagnostic only; not used for pass/fail):")
        for val, cnt in sorted(sage_sc.items(), key=lambda x: -x[1]):
            print(f"      SAGE  : {val:<+20.6g}  {cnt:>6,} galaxies  ({100.0*cnt/n:.1f}%)")
        for val, cnt in sorted(mimic_sc.items(), key=lambda x: -x[1]):
            print(f"      Mimic : {val:<+20.6g}  {cnt:>6,} galaxies  ({100.0*cnt/n:.1f}%)")

    if stats["top_10"]:
        print(
            f"    Top-{len(stats['top_10'])} worst non-sentinel-sentinel violators (high tolerance):"
        )
        print(f"      {'HaloID':>12s}  {'sage':>15s}  {'mimic':>15s}  {'rel_diff':>12s}")
        for row in stats["top_10"]:
            sk = row.get("sent_kind")
            if sk == "sage_sent":
                note = yellow("  ⚠ sage sentinel vs real")
            elif sk == "mimic_sent":
                note = yellow("  ⚠ real vs mimic sentinel")
            else:
                note = ""
            print(
                f"      {row['halo_id']:>12d}  "
                f"{row['sage_val']:>15.6e}  "
                f"{row['mimic_val']:>15.6e}  "
                f"{row['rel_diff']:>12.3e}"
                f"{note}"
            )


# =============================================================================
# REPEATED-VALUE OVERVIEW
# =============================================================================


def print_repeated_value_overview(all_stats, n_matched):
    """
    Print a ranked table of the top-10 repeated values found across all
    properties, ordered by occurrence count.
    """
    rows = []
    for s in all_stats:
        for val, cnt in s.get("sage_sentinel_counts", {}).items():
            rows.append((s["label"], "SAGE", val, cnt))
        for val, cnt in s.get("mimic_sentinel_counts", {}).items():
            rows.append((s["label"], "Mimic", val, cnt))

    if not rows:
        print("  No repeated values detected.")
        return

    rows.sort(key=lambda r: -r[3])

    print(
        f"  {'#':>3s}  {'Property':<35s}  {'Side':>5s}  {'Value':>18s}  "
        f"{'Count':>8s}  {'Fraction':>8s}"
    )
    print(f"  {'---':>3s}  {'-'*35}  {'-'*5}  {'-'*18}  {'-'*8}  {'-'*8}")
    for rank, (label, side, val, cnt) in enumerate(rows[:10], 1):
        frac = 100.0 * cnt / max(n_matched, 1)
        bar = "█" * int(frac / 5)  # 1 block per 5 %
        print(
            f"  {rank:>3d}  {label:<35s}  {side:>5s}  {val:>+18.6g}  "
            f"{cnt:>8,}  {frac:>7.1f}%  {bar}"
        )


def print_problem_galaxy_summary(problem_galaxies):
    """Print the top matched galaxies by cross-property discrepancy footprint."""
    if not problem_galaxies:
        print(f"  {PASS_STR}  No matched galaxies have property discrepancies.")
        return

    print("  Ranked by low-tolerance discrepancy count, then high-tolerance count.")
    print("  MatchID is SAGE SimulationHaloIndex and Mimic MostBoundID.")
    print()
    for rank, row in enumerate(problem_galaxies, 1):
        sage_loc = (
            f"loaded_row={row['sage_row']}, GalaxyIndex={_maybe_int(row['sage_galaxy_index'])}, "
            f"Tree:Halo={_maybe_int(row['sage_tree_index'])}:{_maybe_int(row['sage_halo_index'])}"
        )
        mimic_loc = (
            f"loaded_row={row['mimic_row']}, UniqueGalaxyID={_maybe_int(row['mimic_unique_id'])}, "
            f"MostBoundID={_maybe_int(row['mimic_most_bound_id'])}"
        )
        print(
            f"  {rank:>2d}. MatchID={row['match_id']}  "
            f"High={row['high_count']}  Low={row['low_count']}  "
            f"MaxRelDiff={row['max_rdiff']:.3e}"
        )
        print(f"      SAGE : {sage_loc}")
        print(f"      Mimic: {mimic_loc}")
        if row["low_props"]:
            print(f"      Low-tol discrepant properties : {', '.join(row['low_props'])}")
        if row["high_only_props"]:
            print(f"      High-only discrepant properties: {', '.join(row['high_only_props'])}")


def print_loading_failure(error):
    """Print an actionable data-loading failure without a Python traceback."""
    print(f"\n{SEP_MAJOR}")
    print(bold("[ DATA LOADING FAILED ]"))
    print(SEP_MAJOR)
    print(f"  Requested snapshot : {SNAPSHOT}")
    print(f"  Error              : {error}")

    try:
        sage_snaps = available_sage_snapshots(SAGE_DIR)
    except (OSError, ValueError) as exc:
        sage_snaps = []
        print(f"  SAGE snapshots     : unavailable ({exc})")
    else:
        print(f"  SAGE snapshots     : {_format_snapshot_list(sage_snaps)}")

    try:
        mimic_snaps = available_mimic_snapshots(MIMIC_DIR)
    except (OSError, ValueError) as exc:
        mimic_snaps = []
        print(f"  Mimic snapshots    : unavailable ({exc})")
    else:
        print(f"  Mimic snapshots    : {_format_snapshot_list(mimic_snaps)}")

    mimic_config = _mimic_metadata_config(MIMIC_DIR)
    snapshot_line = _snapshot_list_line(mimic_config)
    if snapshot_line:
        print(f"  Mimic config       : {mimic_config}")
        print(f"  Mimic snapshot_list: {snapshot_line}")

    common = sorted(set(sage_snaps) & set(mimic_snaps))
    print(f"  Common snapshots   : {_format_snapshot_list(common)}")
    if common:
        print(
            "  Set SNAPSHOT to one of the common snapshots above, or rerun Mimic with this snapshot in snapshot_list."
        )
    else:
        print(
            "  No common snapshots are available; regenerate one or both outputs with matching snapshots."
        )
    print(SEP_MAJOR)


# =============================================================================
# MAIN
# =============================================================================


def main():
    print(SEP_MAJOR)
    print(bold("  SAGE vs Mimic Galaxy Comparison"))
    print(f"  Snapshot : {SNAPSHOT}")
    print(f"  High tol : {HIGH_TOLERANCE:.0e}   Low tol : {LOW_TOLERANCE:.0e}")
    print(f"  SAGE dir : {SAGE_DIR}")
    print(f"  Mimic dir: {MIMIC_DIR}")
    print(SEP_MAJOR)

    # -------------------------------------------------------------------------
    # 1. Load data
    # -------------------------------------------------------------------------
    print(f"\n{bold('[ Loading data ]')}")
    try:
        sage_data, sage_files = load_sage(SAGE_DIR, SNAPSHOT)
        mimic_data, mimic_files = load_mimic(MIMIC_DIR, SNAPSHOT)
    except (FileNotFoundError, KeyError, OSError, ValueError) as exc:
        print_loading_failure(exc)
        return

    sage_total = len(sage_data[next(iter(sage_data))])
    mimic_total = len(mimic_data)

    print(f"  SAGE  files: {sage_files}")
    print(f"  Mimic files: {mimic_files}")
    print(f"  SAGE  total galaxies : {sage_total:,}")
    print(f"  Mimic total galaxies : {mimic_total:,}")

    # -------------------------------------------------------------------------
    # 2. Galaxy count check
    # -------------------------------------------------------------------------
    print(f"\n{SEP_MINOR}")
    print(bold("[ 1. GALAXY COUNT CHECK ]"))
    print(SEP_MINOR)

    count_diff = sage_total - mimic_total
    if count_diff == 0:
        print(f"  {PASS_STR}  Counts match: {sage_total:,}")
        count_ok = True
    else:
        print(
            f"  {yellow('⚠ DISCREPANCY')}  SAGE={sage_total:,}  Mimic={mimic_total:,}  diff={count_diff:+d}"
        )
        count_ok = False

    # -------------------------------------------------------------------------
    # 3. Match galaxies
    # -------------------------------------------------------------------------
    sage_idx, mimic_idx, common_ids, sage_only, mimic_only = match_galaxies(sage_data, mimic_data)
    n_matched = len(common_ids)

    print(f"\n  Matched via SimulationHaloIndex ↔ MostBoundID")
    print(f"    Matched galaxies : {n_matched:,}")

    if sage_only:
        print(f"    {yellow('⚠ IN SAGE ONLY')} ({len(sage_only)}):")
        sage_id_map = {int(v): i for i, v in enumerate(sage_data["SimulationHaloIndex"])}
        print(f"      {'HaloID':>12s}  {'Mvir':>10s}  {'x':>10s}  {'y':>10s}  {'z':>10s}")
        for hid in sage_only[:20]:
            idx = sage_id_map[hid]
            mvir = float(sage_data["Mvir"][idx])
            x = float(sage_data["Posx"][idx])
            y = float(sage_data["Posy"][idx])
            z = float(sage_data["Posz"][idx])
            print(f"      {hid:>12d}  {mvir:>10.4f}  {x:>10.4f}  {y:>10.4f}  {z:>10.4f}")
        if len(sage_only) > 20:
            print(f"      ... and {len(sage_only) - 20} more")
    if mimic_only:
        print(f"    {yellow('⚠ IN Mimic ONLY')} ({len(mimic_only)}):")
        mimic_id_map = {int(v): i for i, v in enumerate(mimic_data["MostBoundID"])}
        print(f"      {'HaloID':>12s}  {'Mvir':>10s}  {'x':>10s}  {'y':>10s}  {'z':>10s}")
        for hid in mimic_only[:20]:
            idx = mimic_id_map[hid]
            mvir = float(mimic_data["Mvir"][idx])
            x = float(mimic_data["Pos"][idx, 0])
            y = float(mimic_data["Pos"][idx, 1])
            z = float(mimic_data["Pos"][idx, 2])
            print(f"      {hid:>12d}  {mvir:>10.4f}  {x:>10.4f}  {y:>10.4f}  {z:>10.4f}")
        if len(mimic_only) > 20:
            print(f"      ... and {len(mimic_only) - 20} more")

    if not sage_only and not mimic_only:
        print(f"    {PASS_STR}  All galaxies matched perfectly.")

    # -------------------------------------------------------------------------
    # 4. Property coverage
    # -------------------------------------------------------------------------
    print(f"\n{SEP_MINOR}")
    print(bold("[ 2. PROPERTY COVERAGE ]"))
    print(SEP_MINOR)

    tasks, sage_unmatched, mimic_unmatched = build_comparison_tasks(
        sage_data, mimic_data, sage_idx, mimic_idx
    )

    print(f"  Comparable property pairs  : {len(tasks)}")

    # Summarise mappings used
    by_mapping = defaultdict(list)
    for t in tasks:
        by_mapping[t["mapping"]].append(t["label"])

    for mapping_type, labels in sorted(by_mapping.items()):
        print(
            f"    {mapping_type:30s}: {len(labels):3d}  "
            f"({', '.join(labels[:4])}" + (" ..." if len(labels) > 4 else "") + ")"
        )

    if sage_unmatched:
        print(f"\n  {yellow('SAGE fields with no Mimic equivalent')} ({len(sage_unmatched)}):")
        for f in sage_unmatched:
            print(f"    - {f}")

    if mimic_unmatched:
        print(f"\n  {yellow('Mimic fields with no SAGE equivalent')} ({len(mimic_unmatched)}):")
        for f in mimic_unmatched:
            print(f"    - {f}")

    print(
        f"\n  Skipped SAGE fields  ({len(SAGE_SKIP_FIELDS)}): "
        f"{', '.join(sorted(SAGE_SKIP_FIELDS))}"
    )
    print(
        f"  Skipped Mimic fields ({len(MIMIC_SKIP_FIELDS)}): "
        f"{', '.join(sorted(MIMIC_SKIP_FIELDS))}"
    )

    # -------------------------------------------------------------------------
    # 5. Per-property comparison
    # -------------------------------------------------------------------------
    print(f"\n{SEP_MINOR}")
    print(bold("[ 3. PER-PROPERTY COMPARISON ]"))
    print(f"     Comparing {n_matched:,} matched galaxies across {len(tasks)} properties")
    print(SEP_MINOR)

    all_stats = []
    for task in tasks:
        stats = compare_property(
            task["sage_vals"],
            task["mimic_vals"],
            common_ids,
            HIGH_TOLERANCE,
            LOW_TOLERANCE,
            sage_explicit_sentinel_mask=task.get("sage_explicit_sentinel_mask"),
            mimic_explicit_sentinel_mask=task.get("mimic_explicit_sentinel_mask"),
        )
        stats["label"] = task["label"]
        stats["mapping"] = task["mapping"]
        stats["sage_sentinel_counts"] = task.get("sage_sentinel_counts", {})
        stats["mimic_sentinel_counts"] = task.get("mimic_sentinel_counts", {})
        all_stats.append(stats)

    # Sort by max_rdiff descending so worst offenders appear first
    all_stats.sort(key=lambda s: s["max_rdiff"], reverse=True)

    for stats in all_stats:
        print_property_result(
            stats["label"], stats["mapping"], stats, HIGH_TOLERANCE, LOW_TOLERANCE
        )

    # -------------------------------------------------------------------------
    # 6. Most problematic galaxies
    # -------------------------------------------------------------------------
    problem_galaxies, n_problem_galaxies = build_problem_galaxy_summary(
        tasks,
        common_ids,
        sage_idx,
        mimic_idx,
        sage_data,
        mimic_data,
        HIGH_TOLERANCE,
        LOW_TOLERANCE,
    )

    print(f"\n{SEP_MINOR}")
    print(bold("[ 4. MOST PROBLEMATIC GALAXIES ]"))
    print(SEP_MINOR)
    print_problem_galaxy_summary(problem_galaxies)

    # -------------------------------------------------------------------------
    # 7. Comprehensive summary
    # -------------------------------------------------------------------------
    print(f"\n{SEP_MAJOR}")
    print(bold("[ 5. COMPREHENSIVE SUMMARY ]"))
    print(SEP_MAJOR)

    n_props = len(all_stats)

    # Aggregate counts using the new per-property fields
    n_high_fail = sum(1 for s in all_stats if s["n_high_real"] > 0)
    n_low_fail = sum(1 for s in all_stats if s["n_low_real"] > 0)
    n_high_pass = n_props - n_high_fail
    n_low_pass = n_props - n_low_fail

    total_high_real = sum(s["n_high_real"] for s in all_stats)
    total_high_genuine = sum(s["n_high_genuine"] for s in all_stats)
    total_high_sent_real = sum(s["n_high_sent_real"] for s in all_stats)
    total_high_sent_sent = sum(s["n_high_sent_sent"] for s in all_stats)

    total_low_real = sum(s["n_low_real"] for s in all_stats)
    total_low_genuine = sum(s["n_low_genuine"] for s in all_stats)
    total_low_sent_real = sum(s["n_low_sent_real"] for s in all_stats)
    total_low_sent_sent = sum(s["n_low_sent_sent"] for s in all_stats)

    print(f"\n  Galaxy counts")
    print(f"    SAGE total            : {sage_total:,}")
    print(f"    Mimic total           : {mimic_total:,}")
    print(
        f"    Matched               : {n_matched:,}/{sage_total:,}  ({100.0*n_matched/max(sage_total,1):.4f}% of SAGE)"
    )
    print(f"    SAGE-only (unmatched) : {len(sage_only)}")
    print(f"    Mimic-only (unmatched): {len(mimic_only)}")
    count_status = (
        PASS_STR
        if count_ok and not sage_only and not mimic_only
        else (yellow("⚠ WARN") if not count_ok or sage_only or mimic_only else PASS_STR)
    )
    print(f"    Count check           : {count_status}")

    print(f"\n  Property coverage")
    print(f"    Comparable pairs      : {n_props}")
    print(
        f"    Near-zero threshold   : {NEAR_ZERO_THRESHOLD:.0e}  (|value| below this treated as 0)"
    )
    print(
        f"    Repeated-value flag   : >{SENTINEL_MIN_FRACTION*100:.0f}% of galaxies and >{SENTINEL_MIN_COUNT} occurrences"
    )
    print(
        f"    SAGE unmatched fields : {len(sage_unmatched)}"
        + (f"  ({', '.join(sage_unmatched)})" if sage_unmatched else "")
    )
    print(
        f"    Mimic unmatched fields: {len(mimic_unmatched)}"
        + (f"  ({', '.join(mimic_unmatched)})" if mimic_unmatched else "")
    )

    print(f"\n  Tolerance results")
    print(f"    sent-sent = both sides are a sentinel (excluded from pass/fail)")
    print(f"    sent-real = one side is a sentinel, the other is physics (counts as violation)")
    print(f"    genuine   = neither side is a sentinel")

    print(f"\n    High tolerance ({HIGH_TOLERANCE:.0e}):")
    print(f"      Properties passing : {n_high_pass}/{n_props}  ({100.0*n_high_pass/n_props:.1f}%)")
    print(f"      Properties failing : {n_high_fail}/{n_props}  ({100.0*n_high_fail/n_props:.1f}%)")
    print(
        f"      Real violations    : {total_high_real:,}  ({100.0*total_high_real/max(n_matched*n_props,1):.4f}% of all comparisons)"
    )
    print(f"        genuine          : {total_high_genuine:,}")
    if total_high_sent_real > 0:
        print(
            f"        sent-real        : {total_high_sent_real:,}  {yellow('(one code ran physics, other did not)')}"
        )
    if total_high_sent_sent > 0:
        print(
            f"      Excluded (sent-sent): {total_high_sent_sent:,}  {yellow('(explicit equivalent unset values)')}"
        )

    print(f"\n    Low tolerance ({LOW_TOLERANCE:.0e}):")
    print(f"      Properties passing : {n_low_pass}/{n_props}  ({100.0*n_low_pass/n_props:.1f}%)")
    print(f"      Properties failing : {n_low_fail}/{n_props}  ({100.0*n_low_fail/n_props:.1f}%)")
    print(
        f"      Real violations    : {total_low_real:,}  ({100.0*total_low_real/max(n_matched*n_props,1):.4f}% of all comparisons)"
    )
    print(f"        genuine          : {total_low_genuine:,}")
    if total_low_sent_real > 0:
        print(
            f"        sent-real        : {total_low_sent_real:,}  {yellow('(one code ran physics, other did not)')}"
        )
    if total_low_sent_sent > 0:
        print(
            f"      Excluded (sent-sent): {total_low_sent_sent:,}  {yellow('(explicit equivalent unset values)')}"
        )

    print(f"\n  Most problematic galaxies")
    if problem_galaxies:
        worst = problem_galaxies[0]
        print(f"    Galaxies with diffs    : {n_problem_galaxies:,}/{n_matched:,}")
        print(
            f"      Worst MatchID       : {worst['match_id']}  "
            f"(High={worst['high_count']}, Low={worst['low_count']}, "
            f"MaxRelDiff={worst['max_rdiff']:.3e})"
        )
        print(
            f"      Worst SAGE IDs      : GalaxyIndex={_maybe_int(worst['sage_galaxy_index'])}, "
            f"Tree:Halo={_maybe_int(worst['sage_tree_index'])}:{_maybe_int(worst['sage_halo_index'])}"
        )
        print(
            f"      Worst Mimic IDs     : UniqueGalaxyID={_maybe_int(worst['mimic_unique_id'])}, "
            f"MostBoundID={_maybe_int(worst['mimic_most_bound_id'])}"
        )
    else:
        print(f"    {PASS_STR}  No matched galaxies have property discrepancies.")

    # Repeated-value overview — top 10 by count
    print(f"\n{SEP_MINOR}")
    print(bold("  Top-10 repeated values (diagnostic only)"))
    print(SEP_MINOR)
    print_repeated_value_overview(all_stats, n_matched)

    # All failing properties, ordered by n_high_real descending (badness)
    failing = [s for s in all_stats if s["n_high_real"] > 0 or s["n_low_real"] > 0]
    failing.sort(key=lambda s: (-s["n_high_real"], -s["n_low_real"], -s["max_rdiff"]))
    passing = [s for s in all_stats if s["n_high_real"] == 0 and s["n_low_real"] == 0]

    print(f"\n{SEP_MINOR}")
    print(bold(f"  All failing properties ({len(failing)}) — ordered by high-tol real violations"))
    print(SEP_MINOR)
    if failing:
        print(f"  Column key:")
        print(f"    Hi/Lo real : violations at high/low tolerance that count toward pass/fail")
        print(f"                 (= genuine + sent-real combined)")
        print(
            f"    Hi/Lo s-r  : subset of real where one side is a sentinel, the other is a physics value"
        )
        print(f"    Hi/Lo excl : sent-sent pairs excluded entirely (both sides at init default)")
        print()
        hdr = (
            f"  {'Property':<35s}  {'MaxRdiff':>10s}  "
            f"{'Hi real (%)':>14s}  {'Hi s-r':>8s}  {'Hi excl':>8s}  "
            f"{'Lo real (%)':>14s}  {'Lo s-r':>8s}  {'Lo excl':>8s}"
        )
        print(hdr)
        print(f"  {'-'*35}  {'-'*10}  {'-'*14}  {'-'*8}  {'-'*8}  {'-'*14}  {'-'*8}  {'-'*8}")
        for s in failing:
            hr = s["n_high_real"]
            hsr = s["n_high_sent_real"]
            hss = s["n_high_sent_sent"]
            lr = s["n_low_real"]
            lsr = s["n_low_sent_real"]
            lss = s["n_low_sent_sent"]
            n = s["n_total"]
            hr_str = f"{hr:,} ({100.0*hr/n:.1f}%)"
            lr_str = f"{lr:,} ({100.0*lr/n:.1f}%)"
            hr_col = red(f"{hr_str:>14s}") if s["n_high_genuine"] > 0 else yellow(f"{hr_str:>14s}")
            lr_col = yellow(f"{lr_str:>14s}") if lr > 0 else f"{lr_str:>14s}"
            print(
                f"  {s['label']:<35s}  {s['max_rdiff']:>10.3e}  "
                f"{hr_col}  {hsr:>8,}  {hss:>8,}  "
                f"{lr_col}  {lsr:>8,}  {lss:>8,}"
            )
    else:
        print(f"  {PASS_STR}  No failing properties!")

    if passing:
        print(
            f"\n  {PASS_STR}  Passing properties ({len(passing)}): "
            + ", ".join(s["label"] for s in passing)
        )

    # Overall verdict
    high_overall_pass = (n_high_fail == 0) and count_ok and not sage_only and not mimic_only
    low_overall_pass = (n_low_fail == 0) and count_ok and not sage_only and not mimic_only

    print(f"\n{SEP_MAJOR}")
    print(bold("  OVERALL VERDICT"))
    print(SEP_MAJOR)
    if high_overall_pass:
        print(
            f"  {PASS_STR}  All galaxies matched; all properties agree within high tolerance ({HIGH_TOLERANCE:.0e})."
        )
        if total_high_sent_sent > 0:
            print(f"       ({total_high_sent_sent:,} explicit sent-sent exclusions noted.)")
    elif low_overall_pass:
        print(
            f"  {yellow('PARTIAL PASS')}  All galaxies matched; all properties agree within low tolerance ({LOW_TOLERANCE:.0e}),"
        )
        print(
            f"               but {n_high_fail} propert{'y' if n_high_fail==1 else 'ies'} "
            f"have real violations exceeding the high tolerance ({HIGH_TOLERANCE:.0e})."
        )
    else:
        print(
            f"  {FAIL_STR}  {n_low_fail} propert{'y' if n_low_fail==1 else 'ies'} "
            f"have real violations exceeding even the low tolerance ({LOW_TOLERANCE:.0e})."
        )
        if not count_ok or sage_only or mimic_only:
            print(f"       Galaxy count discrepancy also present — investigate unmatched galaxies.")
    print(SEP_MAJOR)
    print()


if __name__ == "__main__":
    main()
