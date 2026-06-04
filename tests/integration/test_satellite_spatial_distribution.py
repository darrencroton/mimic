#!/usr/bin/env python3
"""
Satellite Galaxy Spatial Distribution Integration Test

Validates: Type 1 satellites are within physically reasonable distances from their centrals
Phase: Core Property Validation
Scope: Physics-free mode (no modules), snapshot 63

Physical Motivation:
  - Type 1 satellites have dark matter halos and orbit within their host halo
  - Satellite positions should be close to their central galaxy position
  - Large separations indicate potential tracking bugs or merger issues
  - Type 2 orphans have lost their halos and are expected to drift away

Test Strategy:
  1. Load HDF5 output and parameter file to get box_size
  2. Filter halos: only process those > 10x Rvir from box boundaries
  3. For each satellite galaxy:
     - Find its central galaxy using UniqueCentralGalaxyID
     - Calculate 3D distance between satellite and central (with periodic boundaries)
     - Type 1 (true satellites):
       * WARNING if distance > 3x central Rvir (unusual but possible)
       * FAIL if distance > 10x central Rvir (unphysical)
     - Type 2 (orphans):
       * Count for informational purposes only (no pass/fail)
       * Orphans are expected to drift away as they've lost their halos

Periodic Boundaries:
  - Simulation box uses periodic boundary conditions
  - Distance calculation uses minimum image convention (shortest path)
  - Filter excludes halos near boundaries for additional safety

Test Data:
  - Uses test_hdf5.yaml (snapshot 63, z=0)
  - Box size: 62.5 Mpc/h
  - Processes only halos well inside box boundaries

Author: Mimic Testing Team
Date: 2025-12-09
"""

import sys
from pathlib import Path
import numpy as np
from collections import defaultdict

# Add framework to path
REPO_ROOT = Path(__file__).parent.parent.parent
sys.path.insert(0, str(REPO_ROOT / "tests"))

from framework import (
    TEST_DATA_DIR,
    MIMIC_EXE,
    ensure_output_dirs,
    model_input_file,
    run_mimic,
    read_param_file,
)

# Ensure output directories exist
ensure_output_dirs()

# ANSI color codes (module-level constants)
BLUE = '\033[1;34m'
GREEN = '\033[0;32m'
RED = '\033[0;31m'
YELLOW = '\033[1;33m'
NC = '\033[0m'


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
        raise ImportError(f"{RED}h5py not available - cannot load HDF5 output{NC}")

    with h5py.File(output_file, 'r') as f:
        # Mimic HDF5 structure: Root contains snapshot groups (e.g., 'Snap063')
        # Each snapshot group contains 'Galaxies' dataset (structured array)

        # Get snapshot groups (e.g., 'Snap063')
        snap_groups = [key for key in f.keys() if key.startswith('Snap')]

        if not snap_groups:
            raise ValueError(f"{RED}No snapshot groups found in HDF5 file: {output_file}{NC}")

        # For testing, we expect one snapshot (Snap063 for z=0)
        # Use the first snapshot group found
        snap_name = snap_groups[0]
        snap_group = f[snap_name]

        # Read halo data from 'Galaxies' dataset
        if 'Galaxies' not in snap_group:
            raise ValueError(f"{RED}No 'Galaxies' dataset found in {snap_name}{NC}")

        # Load the structured array directly
        halos = snap_group['Galaxies'][:]

        # Get metadata from group attributes
        attrs = dict(snap_group.attrs) if hasattr(snap_group, 'attrs') else {}

        # Also check for TreeHalosPerSnap to get tree count
        ntrees = len(snap_group['TreeHalosPerSnap'][:]) if 'TreeHalosPerSnap' in snap_group else 1

        # Create metadata
        metadata = {
            'TotHalos': len(halos),
            'Ntrees': ntrees,
            'NoutputSnaps': 1,
            'SnapshotName': snap_name,
        }
        metadata.update(attrs)

    # Convert to recarray for attribute access (outside the 'with' block)
    halos = halos.view(np.recarray)

    return halos, metadata


def calculate_distance_3d(pos1, pos2, box_size):
    """
    Calculate 3D Euclidean distance between two positions with periodic boundaries.

    Uses the minimum image convention: for each dimension, calculates the shortest
    distance accounting for periodic boundary conditions.

    Args:
        pos1: 3D position array [x, y, z] in Mpc/h
        pos2: 3D position array [x, y, z] in Mpc/h
        box_size: Simulation box size in Mpc/h

    Returns:
        float: Distance in Mpc/h (accounting for periodic boundaries)
    """
    # Calculate distance in each dimension using minimum image convention
    dx = abs(pos1[0] - pos2[0])
    dy = abs(pos1[1] - pos2[1])
    dz = abs(pos1[2] - pos2[2])

    # Apply periodic boundaries: if distance > box_size/2, use wrapped distance
    if dx > box_size / 2.0:
        dx = box_size - dx
    if dy > box_size / 2.0:
        dy = box_size - dy
    if dz > box_size / 2.0:
        dz = box_size - dz

    return np.sqrt(dx*dx + dy*dy + dz*dz)


def is_halo_inside_safe_region(pos, rvir, box_size, safety_factor=10.0):
    """
    Check if a halo is sufficiently far from box boundaries.

    To avoid issues with periodic boundary conditions, we only process halos
    that are at least safety_factor * Rvir away from all box edges.

    Args:
        pos: 3D position array [x, y, z] in Mpc/h
        rvir: Virial radius in Mpc/h
        box_size: Simulation box size in Mpc/h
        safety_factor: Minimum distance from boundaries in units of Rvir (default: 10)

    Returns:
        bool: True if halo is in safe region, False if too close to boundaries
    """
    safety_distance = safety_factor * rvir

    for coord in pos:
        # Check distance from lower boundary (0)
        if coord < safety_distance:
            return False
        # Check distance from upper boundary (box_size)
        if coord > (box_size - safety_distance):
            return False

    return True


def test_satellite_spatial_distribution():
    """
    Test that satellite galaxies are within reasonable distances from their centrals.

    What: Validates spatial distribution of satellites relative to their central galaxies
    Expected:
      - All satellites within 10x Rvir of their central (FAIL if not)
      - Most satellites within 3x Rvir of their central (WARNING if many exceed this)
    Rationale: Satellites orbit within their host halo, large separations indicate bugs
    """
    print("Testing satellite galaxy spatial distribution...")

    if not MIMIC_EXE.exists():
        print("  Skipping (Mimic not built)")
        return

    # Check if HDF5 is supported
    param_file = model_input_file("test_hdf5.yaml")

    # Try running Mimic to check HDF5 support
    output_dir = TEST_DATA_DIR / "output" / "hdf5"
    output_file = output_dir / "model_000.hdf5"

    if not output_file.exists():
        print(f"  Running Mimic to generate HDF5 output...")
        returncode, stdout, stderr = run_mimic(param_file)
        if returncode != 0:
            output = (stdout + stderr)
            if "requires HDF5" in output or "HDF5 support" in output or "Recompile with" in output:
                print(f"  Skipping (Mimic not compiled with HDF5 support)")
                return
            else:
                assert False, f"Mimic failed with code {returncode}\nSTDERR: {stderr}"

    # Check if h5py is available
    try:
        import h5py  # noqa: F401
    except ImportError:
        print(f"  Skipping (h5py not available)")
        return

    # Load parameter file to get box_size
    print(f"  Loading parameter file: {param_file.relative_to(REPO_ROOT)}")
    params = read_param_file(param_file)
    box_size = float(params['BoxSize'])
    print(f"    Box size: {box_size} Mpc/h")

    # Load HDF5 output
    print(f"  Loading HDF5 output: {output_file.relative_to(REPO_ROOT)}")
    halos, metadata = load_hdf5_halos(output_file)
    print(f"    Total halos: {metadata['TotHalos']}")

    # Count halo types
    n_centrals = np.sum(halos.Type == 0)
    n_satellites = np.sum(halos.Type == 1)
    n_orphans = np.sum(halos.Type == 2)
    print(f"    Centrals: {n_centrals}, Satellites: {n_satellites}, Orphans: {n_orphans}")

    # Build lookup: UniqueGalaxyID -> halo
    print(f"\n  Building galaxy lookup table...")
    galaxy_lookup = {}
    for halo in halos:
        galaxy_lookup[halo.UniqueGalaxyID] = halo

    print(f"    Indexed {len(galaxy_lookup)} galaxies")

    # Filter to halos inside safe region (>= 10x Rvir from boundaries)
    print(f"\n  Filtering halos to safe region (>= 10x Rvir from box boundaries)...")
    safe_halos = []
    boundary_halos = []

    for halo in halos:
        if is_halo_inside_safe_region(halo.Pos, halo.Rvir, box_size, safety_factor=10.0):
            safe_halos.append(halo)
        else:
            boundary_halos.append(halo)

    print(f"    Safe region halos: {len(safe_halos)} ({100.0 * len(safe_halos) / len(halos):.1f}%)")
    print(f"    Near-boundary halos: {len(boundary_halos)} (excluded from test)")

    # Count satellites in safe region
    safe_satellites = [h for h in safe_halos if h.Type in [1, 2]]
    print(f"    Satellites in safe region: {len(safe_satellites)}")

    if len(safe_satellites) == 0:
        print(f"\n{YELLOW}  ⚠ WARNING: No satellites in safe region to test{NC}")
        print(f"{GREEN}  ✓ PASS: Test completed (no satellites to validate){NC}")
        return

    # Validate each satellite's distance from its central
    print(f"\n  Validating satellite distances from centrals...")
    print(f"    Note: Only Type 1 (true satellites) are subject to pass/warn/fail criteria")
    print(f"    Type 2 (orphans) are counted separately for information only")

    # Type 1 satellites (true satellites with halos)
    violations_3x = []  # Type 1 satellites beyond 3x Rvir (WARNING)
    violations_10x = []  # Type 1 satellites beyond 10x Rvir (FAIL)
    missing_centrals = []  # Satellites whose central is not found
    valid_satellites = []  # Type 1 satellites within 3x Rvir

    # Type 2 orphans (informational only - not part of pass/fail)
    orphan_valid = []  # Orphans < 3x Rvir
    orphan_3x = []  # Orphans 3x-10x Rvir
    orphan_10x = []  # Orphans > 10x Rvir

    for sat in safe_satellites:
        central_id = sat.UniqueCentralGalaxyID

        # Find central galaxy
        if central_id not in galaxy_lookup:
            missing_centrals.append({
                'satellite_id': sat.UniqueGalaxyID,
                'central_id': central_id,
                'type': sat.Type,
                'mvir': sat.Mvir,
                'rvir': sat.Rvir,
            })
            continue

        central = galaxy_lookup[central_id]

        # Calculate distance (using periodic boundary conditions)
        distance = calculate_distance_3d(sat.Pos, central.Pos, box_size)

        # Get central Rvir for comparison
        central_rvir = central.Rvir

        # Separate handling for Type 1 (true satellites) vs Type 2 (orphans)
        if sat.Type == 1:
            # Type 1: Apply pass/warn/fail criteria
            if distance > 10.0 * central_rvir:
                violations_10x.append({
                    'satellite_id': sat.UniqueGalaxyID,
                    'central_id': central_id,
                    'type': sat.Type,
                    'distance': distance,
                    'central_rvir': central_rvir,
                    'ratio': distance / central_rvir,
                    'sat_pos': sat.Pos,
                    'central_pos': central.Pos,
                    'sat_mvir': sat.Mvir,
                    'central_mvir': central.Mvir,
                })
            elif distance > 3.0 * central_rvir:
                violations_3x.append({
                    'satellite_id': sat.UniqueGalaxyID,
                    'central_id': central_id,
                    'type': sat.Type,
                    'distance': distance,
                    'central_rvir': central_rvir,
                    'ratio': distance / central_rvir,
                })
            else:
                valid_satellites.append({
                    'satellite_id': sat.UniqueGalaxyID,
                    'distance': distance,
                    'central_rvir': central_rvir,
                    'ratio': distance / central_rvir,
                })
        elif sat.Type == 2:
            # Type 2 (orphans): Count for information only
            if distance > 10.0 * central_rvir:
                orphan_10x.append({
                    'satellite_id': sat.UniqueGalaxyID,
                    'central_id': central_id,
                    'distance': distance,
                    'central_rvir': central_rvir,
                    'ratio': distance / central_rvir,
                })
            elif distance > 3.0 * central_rvir:
                orphan_3x.append({
                    'satellite_id': sat.UniqueGalaxyID,
                    'central_id': central_id,
                    'distance': distance,
                    'central_rvir': central_rvir,
                    'ratio': distance / central_rvir,
                })
            else:
                orphan_valid.append({
                    'satellite_id': sat.UniqueGalaxyID,
                    'distance': distance,
                    'central_rvir': central_rvir,
                    'ratio': distance / central_rvir,
                })

    # Report statistics
    n_tested = len(safe_satellites)
    n_type1 = len([s for s in safe_satellites if s.Type == 1])
    n_type2 = len([s for s in safe_satellites if s.Type == 2])
    n_valid = len(valid_satellites)
    n_warn = len(violations_3x)
    n_fail = len(violations_10x)
    n_missing = len(missing_centrals)

    print(f"\n  Satellite distance validation results:")
    print(f"    Total tested: {n_tested} satellites (Type 1: {n_type1}, Type 2: {n_type2})")
    print(f"\n  Type 1 satellites (true satellites - subject to pass/warn/fail):")
    if n_type1 > 0:
        print(f"    Valid (< 3x Rvir): {n_valid} ({100.0 * n_valid / n_type1:.1f}%)")
        print(f"    Warning (3x-10x Rvir): {n_warn} ({100.0 * n_warn / n_type1:.1f}%)")
        print(f"    Failed (> 10x Rvir): {n_fail} ({100.0 * n_fail / n_type1:.1f}%)")
    else:
        print(f"    No Type 1 satellites found")

    print(f"\n  Type 2 orphans (informational only - not part of pass/fail):")
    if n_type2 > 0:
        print(f"    < 3x Rvir: {len(orphan_valid)} ({100.0 * len(orphan_valid) / n_type2:.1f}%)")
        print(f"    3x-10x Rvir: {len(orphan_3x)} ({100.0 * len(orphan_3x) / n_type2:.1f}%)")
        print(f"    > 10x Rvir: {len(orphan_10x)} ({100.0 * len(orphan_10x) / n_type2:.1f}%)")
        print(f"    Note: Orphans are expected to drift away as they've lost their halos")
    else:
        print(f"    No Type 2 orphans found")

    if n_missing > 0:
        print(f"\n  Missing centrals: {n_missing} ({100.0 * n_missing / n_tested:.1f}%)")

    # Report missing centrals
    if missing_centrals:
        print(f"\n{YELLOW}  ⚠ WARNING: {len(missing_centrals)} satellites have missing central galaxies{NC}")
        print(f"{YELLOW}  This may indicate a bug in central galaxy tracking or UniqueCentralGalaxyID assignment.{NC}")
        print(f"{YELLOW}  Showing first 5 satellites with missing centrals:{NC}")
        for i, info in enumerate(missing_centrals[:5]):
            print(f"{YELLOW}    {i+1}. Satellite {info['satellite_id']}: Type={info['type']}, "
                  f"Mvir={info['mvir']:.3f}, expects central {info['central_id']} (not found){NC}")
        if len(missing_centrals) > 5:
            print(f"{YELLOW}    ... and {len(missing_centrals) - 5} more{NC}")

    # Report violations > 3x Rvir (WARNING) - Type 1 only
    if violations_3x:
        print(f"\n{YELLOW}  ⚠ WARNING: {len(violations_3x)} Type 1 satellites beyond 3x Rvir of their central{NC}")
        print(f"{YELLOW}  This is unusual but can occur for substructure or recent infall.{NC}")
        print(f"{YELLOW}  Showing first 10 Type 1 satellites with large separations:{NC}")

        # Sort by ratio (largest first)
        violations_3x.sort(key=lambda x: x['ratio'], reverse=True)

        for i, info in enumerate(violations_3x[:10]):
            print(f"{YELLOW}    {i+1}. Satellite {info['satellite_id']} → Central {info['central_id']}: "
                  f"distance={info['distance']:.3f} Mpc/h ({info['ratio']:.2f}x Rvir){NC}")
            print(f"{YELLOW}       Type={info['type']}, Central Rvir={info['central_rvir']:.3f} Mpc/h{NC}")

        if len(violations_3x) > 10:
            print(f"{YELLOW}    ... and {len(violations_3x) - 10} more{NC}")

    # Report violations > 10x Rvir (FAIL) - Type 1 only
    if violations_10x:
        print(f"\n{RED}  ✗ FAIL: {len(violations_10x)} Type 1 satellites beyond 10x Rvir of their central!{NC}")
        print(f"{RED}  This is unphysical and indicates a serious bug in halo tracking or merger handling.{NC}")
        print(f"{RED}  Showing all Type 1 satellites with extreme separations:{NC}")

        # Sort by ratio (largest first)
        violations_10x.sort(key=lambda x: x['ratio'], reverse=True)

        for i, info in enumerate(violations_10x):
            print(f"\n{RED}    {i+1}. Satellite {info['satellite_id']} → Central {info['central_id']}:{NC}")
            print(f"{RED}       Distance: {info['distance']:.3f} Mpc/h ({info['ratio']:.2f}x Rvir){NC}")
            print(f"{RED}       Central Rvir: {info['central_rvir']:.3f} Mpc/h{NC}")
            print(f"{RED}       Satellite Type: {info['type']}{NC}")
            print(f"{RED}       Satellite Pos: [{info['sat_pos'][0]:.2f}, {info['sat_pos'][1]:.2f}, {info['sat_pos'][2]:.2f}] Mpc/h{NC}")
            print(f"{RED}       Central Pos:   [{info['central_pos'][0]:.2f}, {info['central_pos'][1]:.2f}, {info['central_pos'][2]:.2f}] Mpc/h{NC}")
            print(f"{RED}       Satellite Mvir: {info['sat_mvir']:.3f} (1e10 Msun/h){NC}")
            print(f"{RED}       Central Mvir:   {info['central_mvir']:.3f} (1e10 Msun/h){NC}")

        assert False, (
            f"Satellite spatial distribution validation FAILED!\n"
            f"Found {len(violations_10x)} Type 1 satellites beyond 10x Rvir of their central.\n"
            f"Type 1 satellites should orbit within their host halo (typically < 1-2x Rvir).\n"
            f"Large separations (> 10x Rvir) indicate bugs in halo tracking or merger processing."
        )

    # Calculate distance statistics for valid satellites
    if valid_satellites:
        distances = np.array([s['ratio'] for s in valid_satellites])
        print(f"\n  Distance ratio statistics (valid satellites):")
        print(f"    Mean: {np.mean(distances):.2f}x Rvir")
        print(f"    Median: {np.median(distances):.2f}x Rvir")
        print(f"    Std dev: {np.std(distances):.2f}x Rvir")
        print(f"    Min: {np.min(distances):.2f}x Rvir")
        print(f"    Max: {np.max(distances):.2f}x Rvir")
        print(f"    95th percentile: {np.percentile(distances, 95):.2f}x Rvir")

    print(f"\n{GREEN}  ✓ PASS: All {n_type1} Type 1 satellites within 10x Rvir of their centrals{NC}")
    if n_warn > 0:
        print(f"{YELLOW}  Note: {n_warn} Type 1 satellites between 3-10x Rvir (see warnings above){NC}")
    if n_missing > 0:
        print(f"{YELLOW}  Note: {n_missing} satellites with missing centrals (see warnings above){NC}")
    if n_type2 > 0:
        print(f"  Info: {n_type2} Type 2 orphans tested (for information only)")


def main():
    """Main test runner."""
    # Print test suite header
    print(f"{BLUE}{'=' * 60}{NC}")
    print(f"{BLUE}Test Suite: Satellite Spatial Distribution (test_satellite_spatial_distribution.py){NC}")
    print(f"{BLUE}{'=' * 60}{NC}")
    print()
    print(f"Repository root: {REPO_ROOT}")
    print(f"Mimic executable: {MIMIC_EXE}")
    print()
    print("Testing satellite galaxy spatial distribution:")
    print("  - Satellites should be close to their central galaxy")
    print("  - Distance > 3x Rvir: WARNING (unusual)")
    print("  - Distance > 10x Rvir: FAIL (unphysical)")
    print("  - Only tests halos > 10x Rvir from box boundaries")
    print()

    if not MIMIC_EXE.exists():
        print(f"{RED}ERROR: Mimic executable not found: {MIMIC_EXE}{NC}")
        print("Build it first with: make")
        return 1

    tests = [
        test_satellite_spatial_distribution,
    ]

    passed = 0
    failed = 0

    for test in tests:
        try:
            test()
            print(f"{GREEN}✓ PASS: {test.__name__}{NC}")
            passed += 1
        except AssertionError as e:
            print(f"{RED}✗ FAIL: {test.__name__}{NC}")
            print(f"{RED}  {e}{NC}")
            failed += 1
        except Exception as e:
            print(f"{RED}✗ ERROR: {test.__name__}{NC}")
            print(f"{RED}  {e}{NC}")
            failed += 1

    # Print summary
    print()
    print(f"{BLUE}{'=' * 60}{NC}")
    print(f"{BLUE}Test Summary{NC}")
    print(f"{BLUE}{'=' * 60}{NC}")
    print(f"Passed:  {passed}")
    print(f"Failed:  {failed}")
    print(f"Total:   {passed + failed}")
    print(f"{BLUE}{'=' * 60}{NC}")
    print()

    if failed == 0:
        print(f"{GREEN}✓ All tests passed!{NC}")
        return 0
    else:
        print(f"{RED}✗ {failed} test(s) failed{NC}")
        return 1


if __name__ == "__main__":
    sys.exit(main())
