"""
Snapshot selection for the SAGE26 evolution figures.

The plotting engine hands an evolution figure ``{snapshot: (galaxies, volume, metadata)}`` ordered
by increasing redshift, with the snapshot's redshift in ``metadata["redshift"]``. SAGE26's
history script indexes its snapshots by number and scans them in increasing snapshot number. These
helpers first order the written snapshots by increasing snapshot number, so a figure that selects
through them sees the snapshots in SAGE26's order whatever order the engine supplies, and return
snapshot numbers in that order.

Difference from SAGE26: SAGE26 searches all 64 snapshots because it writes all 64; the candidates here
are the snapshots the run wrote (for example eight for the mini-Millennium run files).
"""

import numpy as np

# The fifteen redshift bins of SAGE26's history script. A snapshot at redshift 0 falls in none of
# them.
Z_BINS = [
    (0.2, 0.5),
    (0.5, 0.8),
    (0.8, 1.1),
    (1.1, 1.5),
    (1.5, 2.0),
    (2.0, 2.5),
    (2.5, 3.0),
    (3.0, 3.5),
    (3.5, 4.5),
    (4.5, 5.5),
    (5.5, 6.5),
    (6.5, 7.5),
    (7.5, 8.5),
    (8.5, 10.0),
    (10.0, 12.0),
]


def _ordered(snapshots):
    """Return the snapshot numbers in increasing order, as SAGE26 scans them."""
    return sorted(snapshots)


def _redshift(snapshots, snap):
    """Return the redshift the engine recorded for one snapshot."""
    return snapshots[snap][2]["redshift"]


def snapshots_in_z_range(snapshots, z_min, z_max):
    """
    Return the snapshots with ``z_min <= z <= z_max``, both ends inclusive.

    As SAGE26's `find_snapshots_in_z_range`: `if z_min <= z <= z_max: snapshot_indices.append(i)`,
    scanning in snapshot-number order.

    Args:
        snapshots: Mapping of snapshot number to (galaxies, volume, metadata).
        z_min: Lower redshift bound, inclusive.
        z_max: Upper redshift bound, inclusive.

    Returns:
        List of snapshot numbers in increasing snapshot-number order.
    """
    return [snap for snap in _ordered(snapshots) if z_min <= _redshift(snapshots, snap) <= z_max]


def nearest_snapshot(snapshots, z_target):
    """
    Return the snapshot whose redshift is nearest ``z_target``.

    SAGE26 takes `np.argmin(np.abs(np.array(redshifts) - target_z))`, which returns the first
    minimum in snapshot-number order, so a tie goes to the lower snapshot number.

    Args:
        snapshots: Mapping of snapshot number to (galaxies, volume, metadata).
        z_target: Target redshift.

    Returns:
        The snapshot number, or None when no snapshot is written.
    """
    ordered = _ordered(snapshots)
    if not ordered:
        return None
    redshifts = np.array([_redshift(snapshots, snap) for snap in ordered])
    return ordered[int(np.argmin(np.abs(redshifts - z_target)))]


def all_snapshots(snapshots):
    """
    Return every written snapshot in increasing snapshot-number order.

    SAGE26 loops `for snap in range(FirstSnap, LastSnap + 1)`.

    Args:
        snapshots: Mapping of snapshot number to (galaxies, volume, metadata).

    Returns:
        List of snapshot numbers in increasing snapshot-number order.
    """
    return _ordered(snapshots)
