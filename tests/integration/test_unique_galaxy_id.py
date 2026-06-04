#!/usr/bin/env python3
"""
UniqueGalaxyID Validation Integration Test

Validates: UniqueGalaxyID uniqueness and persistence across snapshots
Phase: Core Property Validation
Scope: Physics-free mode (no modules), snapshots 62 and 63

UniqueGalaxyID Design (from halo_properties.yaml):
  - Every new galaxy gets a unique UniqueGalaxyID
  - UniqueGalaxyID is carried forward across all snapshots
  - Formula: file*10^12 + tree*10^6 + creation_halonr
  - Without mergers, every galaxy should exist in all future snapshots

Test Strategy:
  1. Check all UniqueGalaxyID values are unique in snapshot 62 (FAIL if duplicates)
  2. Check all galaxies in snap 62 exist in snap 63 (WARNING if missing)
  3. Check all UniqueGalaxyID values are unique in snapshot 63 (FAIL if duplicates)
  4. Check Vvir evolution is reasonable (WARNING if large changes)

Note: With no modules enabled mergers should not happen, so every galaxy created
      should exist through to the final snapshot. Missing galaxies indicate
      a potential bug (or rare edge case that needs investigation).

Author: Mimic Testing Team
Date: 2025-12-06
"""

import sys
from pathlib import Path
import numpy as np
from collections import defaultdict

# Add framework to path
REPO_ROOT = Path(__file__).parent.parent.parent
sys.path.insert(0, str(REPO_ROOT / "tests"))

from framework import (
    load_binary_halos,
    TEST_DATA_DIR,
    MIMIC_EXE,
    ensure_output_dirs,
    model_input_file,
    run_mimic_fresh,
)

# Ensure output directories exist
ensure_output_dirs()

# ANSI color codes (module-level constants)
BLUE = '\033[1;34m'
GREEN = '\033[0;32m'
RED = '\033[0;31m'
YELLOW = '\033[1;33m'
NC = '\033[0m'


def load_uniquegalid_test_data():
    """
    Load test data for UniqueGalaxyID validation.

    Runs Mimic with test_uniquegalid.yaml (outputs snapshots 62 and 63) and
    loads all output halos into a single combined array.

    Returns:
        np.ndarray: Combined halos array from all snapshot output files
    """
    output_dir = TEST_DATA_DIR / "output" / "binary"
    param_file = model_input_file("test_uniquegalid.yaml")

    # Always regenerate output for the selected model so stale files from a
    # previous run (possibly a different MODEL writing the same shared path)
    # cannot satisfy this test. Clear any existing matches first.
    for stale in output_dir.glob("model_uniquegalid_*"):
        stale.unlink()
    print(f"  Running Mimic to generate output...")
    run_mimic_fresh(param_file)
    uniquegalid_files = list(output_dir.glob("model_uniquegalid_*"))

    # The run is expected to emit one output file per snapshot (62 and 63).
    # Fail loudly if it produced fewer, otherwise the per-snapshot uniqueness
    # and persistence checks below could pass vacuously on missing snapshots.
    assert len(uniquegalid_files) >= 2, (
        f"Expected at least 2 snapshot output files (snapshots 62 and 63), "
        f"found {len(uniquegalid_files)}: {[f.name for f in sorted(uniquegalid_files)]}"
    )

    # Load both snapshot files and combine
    print(f"  Loading output files:")
    all_halos = []
    for output_file in sorted(uniquegalid_files):
        print(f"    {output_file.relative_to(REPO_ROOT)}")
        halos, _ = load_binary_halos(output_file)
        all_halos.append(halos)

    # Combine all halos into single array and convert to recarray
    import numpy as np
    output_halos = np.concatenate(all_halos)
    output_halos = output_halos.view(np.recarray)

    # Guard against vacuous success: both expected snapshots must be present and
    # non-empty before any per-snapshot uniqueness/persistence check runs.
    expected_snapshots = {62, 63}
    present_snapshots = {int(s) for s in np.unique(output_halos.SnapNum)}
    missing = expected_snapshots - present_snapshots
    assert not missing, (
        f"Expected galaxies for snapshots {sorted(expected_snapshots)} but "
        f"snapshot(s) {sorted(missing)} are absent (present: {sorted(present_snapshots)}). "
        f"Files: {[f.name for f in sorted(uniquegalid_files)]}"
    )
    for snap in sorted(expected_snapshots):
        n_snap = int(np.sum(output_halos.SnapNum == snap))
        assert n_snap > 0, (
            f"Snapshot {snap} produced no galaxies; uniqueness and persistence "
            f"checks for it would be vacuous"
        )

    return output_halos


def test_uniquegalid_uniqueness_snapshot62():
    """
    Test 1: Check that all UniqueGalaxyID values are unique in snapshot 62.

    What: Validates every galaxy in snapshot 62 has a unique identifier
    Expected: No duplicates - FAIL if any found
    Rationale: UniqueGalaxyID must be unique within a snapshot to identify galaxies
    """
    print("Test 1: Checking UniqueGalaxyID uniqueness in snapshot 62...")

    if not MIMIC_EXE.exists():
        print("  Skipping (Mimic not built)")
        return

    # Load test data
    output_halos = load_uniquegalid_test_data()

    # Filter to snapshot 62
    snap62_mask = output_halos.SnapNum == 62
    snap62_halos = output_halos[snap62_mask]
    n_snap62 = len(snap62_halos)

    print(f"  Snapshot 62: {n_snap62} galaxies")

    # Check for duplicates
    halo_indices = snap62_halos.UniqueGalaxyID
    unique_indices = np.unique(halo_indices)
    n_unique = len(unique_indices)

    if n_unique < n_snap62:
        # DUPLICATES FOUND - THIS IS A FAILURE
        n_duplicates = n_snap62 - n_unique
        print(f"\n{RED}  ✗ FAIL: Found {n_duplicates} duplicate UniqueGalaxyID values in snapshot 62!{NC}")

        # Identify which values are duplicated
        index_counts = defaultdict(int)
        for idx in halo_indices:
            index_counts[idx] += 1

        duplicated_indices = {idx: count for idx, count in index_counts.items() if count > 1}
        print(f"\n{RED}  Duplicated UniqueGalaxyID values (showing first 10):{NC}")
        for i, (idx, count) in enumerate(sorted(duplicated_indices.items())[:10]):
            print(f"{RED}    UniqueGalaxyID {idx}: appears {count} times{NC}")
            # Show which halos have this index
            dup_mask = snap62_halos.UniqueGalaxyID == idx
            dup_halos = snap62_halos[dup_mask]
            for j, halo in enumerate(dup_halos[:5]):  # Show first 5
                print(f"{RED}      Galaxy {j+1}: Type={halo.Type}, Mvir={halo.Mvir:.3f}, "
                      f"MostBoundID={halo.MostBoundID}{NC}")

        if len(duplicated_indices) > 10:
            print(f"{RED}    ... and {len(duplicated_indices) - 10} more duplicated values{NC}")

        assert False, (
            f"UniqueGalaxyID uniqueness FAILED in snapshot 62! "
            f"Found {n_duplicates} duplicates across {len(duplicated_indices)} distinct values. "
            f"Every galaxy must have a unique UniqueGalaxyID."
        )

    print(f"{GREEN}  ✓ PASS: All {n_snap62} UniqueGalaxyID values are unique in snapshot 62{NC}")


def test_uniquegalid_persistence():
    """
    Test 2: Check that all galaxies in snapshot 62 exist in snapshot 63.

    What: Validates every galaxy persists to the next snapshot
    Expected: All galaxies found - WARNING if any missing
    Rationale: Without mergers, every galaxy should continue to exist
              Missing galaxies indicate potential bug or rare edge case
    """
    print("\nTest 2: Checking galaxy persistence from snapshot 62 to 63...")

    if not MIMIC_EXE.exists():
        print("  Skipping (Mimic not built)")
        return

    # Load test data
    output_halos = load_uniquegalid_test_data()

    # Filter to snapshots 62 and 63
    snap62_mask = output_halos.SnapNum == 62
    snap63_mask = output_halos.SnapNum == 63
    snap62_halos = output_halos[snap62_mask]
    snap63_halos = output_halos[snap63_mask]

    print(f"  Snapshot 62: {len(snap62_halos)} galaxies")
    print(f"  Snapshot 63: {len(snap63_halos)} galaxies")

    # Build lookup: UniqueGalaxyID -> halo for snapshot 63
    snap63_lookup = {}
    for halo in snap63_halos:
        halo_idx = halo.UniqueGalaxyID
        if halo_idx in snap63_lookup:
            # This should never happen if Test 1 and Test 3 pass
            print(f"{RED}  ✗ ERROR: Duplicate UniqueGalaxyID {halo_idx} in snapshot 63!{NC}")
        snap63_lookup[halo_idx] = halo

    # Check EVERY galaxy in snapshot 62 (not just active ones)
    print(f"\n  Checking all {len(snap62_halos)} galaxies from snapshot 62...")

    matched = 0
    missing = []

    for halo_62 in snap62_halos:
        halo_idx = halo_62.UniqueGalaxyID

        # Find corresponding galaxy in snapshot 63
        if halo_idx in snap63_lookup:
            matched += 1
        else:
            missing.append(halo_62)

    match_pct = matched / len(snap62_halos) * 100 if len(snap62_halos) > 0 else 0

    print(f"  Matched: {matched} / {len(snap62_halos)} ({match_pct:.1f}%)")
    print(f"  Missing: {len(missing)}")

    # Report missing galaxies as WARNING (should be rare/none without mergers)
    if missing:
        print(f"\n{YELLOW}  ⚠ WARNING: {len(missing)} galaxies from snapshot 62 not found in snapshot 63{NC}")
        print(f"{YELLOW}  Note: No mergers should happen without modules, so this may indicate a bug or rare edge case.{NC}")
        print(f"{YELLOW}  Showing first 10 missing galaxies:{NC}")
        for i, halo in enumerate(missing[:10]):
            print(f"{YELLOW}    UniqueGalaxyID {halo.UniqueGalaxyID}: Type={halo.Type}, Mvir={halo.Mvir:.3f}, "
                  f"Vvir={halo.Vvir:.2f}, Len={halo.Len}{NC}")

        if len(missing) > 10:
            print(f"{YELLOW}    ... and {len(missing) - 10} more missing galaxies{NC}")
    else:
        print(f"{GREEN}  ✓ Perfect persistence: All galaxies continue to snapshot 63{NC}")

    # Calculate new galaxies in snapshot 63
    snap62_index_set = set(snap62_halos.UniqueGalaxyID)
    new_in_63 = [h for h in snap63_halos if h.UniqueGalaxyID not in snap62_index_set]

    print(f"\n  New galaxies in snapshot 63: {len(new_in_63)}")
    print(f"    (These should have unique UniqueGalaxyID values - validated in Test 3)")

    print(f"{GREEN}  ✓ PASS: Persistence check complete ({match_pct:.1f}% matched){NC}")


def test_uniquegalid_uniqueness_snapshot63():
    """
    Test 3: Check that all UniqueGalaxyID values are unique in snapshot 63.

    What: Validates every galaxy in snapshot 63 has a unique identifier
    Expected: No duplicates - FAIL if any found
    Rationale: This catches new galaxies with duplicate IDs
              Must be unique to properly identify all galaxies
    """
    print("\nTest 3: Checking UniqueGalaxyID uniqueness in snapshot 63...")

    if not MIMIC_EXE.exists():
        print("  Skipping (Mimic not built)")
        return

    # Load test data
    output_halos = load_uniquegalid_test_data()

    # Filter to snapshot 63
    snap63_mask = output_halos.SnapNum == 63
    snap63_halos = output_halos[snap63_mask]
    n_snap63 = len(snap63_halos)

    print(f"  Snapshot 63: {n_snap63} galaxies")

    # Check for duplicates
    halo_indices = snap63_halos.UniqueGalaxyID
    unique_indices = np.unique(halo_indices)
    n_unique = len(unique_indices)

    if n_unique < n_snap63:
        # DUPLICATES FOUND - THIS IS A FAILURE
        n_duplicates = n_snap63 - n_unique
        print(f"\n{RED}  ✗ FAIL: Found {n_duplicates} duplicate UniqueGalaxyID values in snapshot 63!{NC}")

        # Identify which values are duplicated
        index_counts = defaultdict(int)
        for idx in halo_indices:
            index_counts[idx] += 1

        duplicated_indices = {idx: count for idx, count in index_counts.items() if count > 1}
        print(f"\n{RED}  Duplicated UniqueGalaxyID values (showing first 10):{NC}")
        for i, (idx, count) in enumerate(sorted(duplicated_indices.items())[:10]):
            print(f"{RED}    UniqueGalaxyID {idx}: appears {count} times{NC}")
            # Show which halos have this index
            dup_mask = snap63_halos.UniqueGalaxyID == idx
            dup_halos = snap63_halos[dup_mask]
            for j, halo in enumerate(dup_halos[:5]):  # Show first 5
                print(f"{RED}      Galaxy {j+1}: Type={halo.Type}, Mvir={halo.Mvir:.3f}, "
                      f"MostBoundID={halo.MostBoundID}{NC}")

        if len(duplicated_indices) > 10:
            print(f"{RED}    ... and {len(duplicated_indices) - 10} more duplicated values{NC}")

        assert False, (
            f"UniqueGalaxyID uniqueness FAILED in snapshot 63! "
            f"Found {n_duplicates} duplicates across {len(duplicated_indices)} distinct values. "
            f"Every galaxy must have a unique UniqueGalaxyID."
        )

    print(f"{GREEN}  ✓ PASS: All {n_snap63} UniqueGalaxyID values are unique in snapshot 63{NC}")


def test_uniquegalid_physical_evolution():
    """
    Test 4: Check that Vvir evolution is physically reasonable.

    What: Validates Vvir changes between snapshots 62 and 63
    Expected: Most changes moderate - WARNING if large changes
    Rationale: Tracks physical consistency - large changes may indicate
              tracking issues or edge cases needing investigation

    Tolerance: 50 (meaning |Vvir_63 - Vvir_62| / Vvir_62 < 50.0)
               This is generous to accommodate halo growth/stripping
    """
    print("\nTest 4: Checking physical evolution (Vvir changes)...")

    if not MIMIC_EXE.exists():
        print("  Skipping (Mimic not built)")
        return

    # Load test data
    output_halos = load_uniquegalid_test_data()

    # Filter to snapshots 62 and 63
    snap62_mask = output_halos.SnapNum == 62
    snap63_mask = output_halos.SnapNum == 63
    snap62_halos = output_halos[snap62_mask]
    snap63_halos = output_halos[snap63_mask]

    # Build lookup: UniqueGalaxyID -> halo for both snapshots
    snap62_lookup = {halo.UniqueGalaxyID: halo for halo in snap62_halos}
    snap63_lookup = {halo.UniqueGalaxyID: halo for halo in snap63_halos}

    # Find galaxies present in both snapshots
    common_indices = set(snap62_lookup.keys()) & set(snap63_lookup.keys())
    n_common = len(common_indices)

    print(f"  Galaxies in both snapshots: {n_common}")

    # Tolerance for Vvir relative change
    tolerance = 50.0  # Very generous - allows 5000% change
    print(f"  Tolerance for relative Vvir change: {tolerance}")

    # Analyze Vvir changes
    vvir_changes = []
    large_changes = []

    for halo_idx in common_indices:
        halo_62 = snap62_lookup[halo_idx]
        halo_63 = snap63_lookup[halo_idx]

        vvir_62 = halo_62.Vvir
        vvir_63 = halo_63.Vvir

        # Calculate relative change
        if vvir_62 > 0:
            rel_change = abs(vvir_63 - vvir_62) / vvir_62
        else:
            # Handle case where Vvir_62 is zero (edge case)
            rel_change = float('inf') if vvir_63 != 0 else 0.0

        vvir_changes.append(rel_change)

        if rel_change > tolerance:
            large_changes.append({
                'UniqueGalaxyID': halo_idx,
                'Type_62': halo_62.Type,
                'Type_63': halo_63.Type,
                'Vvir_62': vvir_62,
                'Vvir_63': vvir_63,
                'rel_change': rel_change,
                'Mvir_62': halo_62.Mvir,
                'Mvir_63': halo_63.Mvir,
            })

    # Statistics
    vvir_changes = np.array(vvir_changes)
    finite_changes = vvir_changes[np.isfinite(vvir_changes)]

    if len(finite_changes) > 0:
        median_change = np.median(finite_changes)
        mean_change = np.mean(finite_changes)
        max_finite = np.max(finite_changes)
        p95_change = np.percentile(finite_changes, 95)
    else:
        median_change = mean_change = max_finite = p95_change = 0.0

    print(f"\n  Vvir relative change statistics (finite values only):")
    print(f"    Median: {median_change:.3f}")
    print(f"    Mean: {mean_change:.3f}")
    print(f"    95th percentile: {p95_change:.3f}")
    print(f"    Maximum (finite): {max_finite:.3f}")

    print(f"\n  Galaxies exceeding tolerance ({tolerance}): {len(large_changes)}")

    # Count infinite changes (Vvir=0 edge case)
    infinite_changes = sum(1 for c in large_changes if c['rel_change'] == float('inf'))

    if large_changes:
        print(f"{YELLOW}\n  ⚠ WARNING: {len(large_changes)} galaxies have Vvir changes exceeding tolerance{NC}")
        if infinite_changes > 0:
            print(f"{YELLOW}  Note: {infinite_changes} galaxies have infinite changes due to Vvir=0 in snapshot 62 (edge case){NC}")
        print(f"{YELLOW}  Showing first 10 galaxies with large Vvir changes:{NC}")

        # Sort by relative change (largest first)
        large_changes.sort(key=lambda x: x['rel_change'] if x['rel_change'] != float('inf') else 1e10, reverse=True)

        for i, change_info in enumerate(large_changes[:10]):
            rel_change_str = "inf" if change_info['rel_change'] == float('inf') else f"{change_info['rel_change']:.2f}"
            rel_change_pct = "inf%" if change_info['rel_change'] == float('inf') else f"{change_info['rel_change']*100:.1f}%"

            print(f"{YELLOW}\n    {i+1}. UniqueGalaxyID {change_info['UniqueGalaxyID']}:{NC}")
            print(f"{YELLOW}       Type: {change_info['Type_62']} → {change_info['Type_63']}{NC}")
            print(f"{YELLOW}       Vvir: {change_info['Vvir_62']:.2f} → {change_info['Vvir_63']:.2f} km/s{NC}")
            print(f"{YELLOW}       Mvir: {change_info['Mvir_62']:.3f} → {change_info['Mvir_63']:.3f} (1e10 Msun/h){NC}")
            print(f"{YELLOW}       Relative change: {rel_change_str} ({rel_change_pct}){NC}")

        if len(large_changes) > 10:
            print(f"{YELLOW}    ... and {len(large_changes) - 10} more{NC}")

        print(f"\n{YELLOW}  Note: Large Vvir changes may be physical (growth, stripping) or indicate tracking issues.{NC}")

    print(f"\n{GREEN}  ✓ PASS: Physical evolution analyzed for {n_common} galaxies{NC}")
    if not large_changes:
        print(f"{GREEN}    All galaxies have Vvir changes within tolerance{NC}")


def main():
    """Main test runner."""
    # Print test suite header
    print(f"{BLUE}{'=' * 60}{NC}")
    print(f"{BLUE}Test Suite: UniqueGalaxyID Validation (test_unique_galaxy_id.py){NC}")
    print(f"{BLUE}{'=' * 60}{NC}")
    print()
    print(f"Repository root: {REPO_ROOT}")
    print(f"Mimic executable: {MIMIC_EXE}")
    print()
    print("Testing UniqueGalaxyID design:")
    print("  - Every new galaxy gets a unique UniqueGalaxyID")
    print("  - UniqueGalaxyID persists across all snapshots")
    print("  - Without mergers, all galaxies exist through final snapshot")
    print()

    if not MIMIC_EXE.exists():
        print(f"{RED}ERROR: Mimic executable not found: {MIMIC_EXE}{NC}")
        print("Build it first with: make")
        return 1

    tests = [
        test_uniquegalid_uniqueness_snapshot62,
        test_uniquegalid_persistence,
        test_uniquegalid_uniqueness_snapshot63,
        test_uniquegalid_physical_evolution,
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
