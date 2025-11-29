#!/usr/bin/env python3
"""
Tree Preservation Integration Test

Validates: Simulation core properties are correctly preserved from input tree to output
Phase: Input-Output Validation
Scope: Physics-free mode (no modules), snapshot 63 only, Type 0+1 halos

This test validates the physics-agnostic data pipeline by comparing:
  - Input: Binary tree files (struct RawHalo) at snapshot 63
  - Output: Mimic output files (struct HaloOutput) at snapshot 63, Type 0+1 only

Test cases:
  - test_tree_preservation_coverage: Validates halo counts and matching coverage
  - test_tree_preservation_properties: Validates all directly-copied properties

Properties validated (directly copied from input):
  - Pos[3], Vel[3], Spin[3]: Vector properties (component-wise)
  - Len, SnapNum: Integer properties
  - Vmax, VelDisp: Float properties
  - MostBoundID → SimulationHaloIndex: ID mapping
  - Mvir: Conditionally validated for Type=0 centrals (when input Mvir >= 0.0)

Properties NOT validated:
  - Mvir: For satellites (always calculated) and centrals with invalid input Mvir
  - All other properties: Not directly copied from input

Matching strategy:
  - Filter input to SnapNum=63 only
  - Filter output to SnapNum=63 AND Type in [0,1] (skip orphans)
  - Match by SimulationHaloIndex (output) = MostBoundID (input)
  - Categorize: perfect matches, imperfect matches, unmatched input, unmatched output

Tolerance: 1e-6 relative for floats, exact for integers

Author: Mimic Testing Team
Date: 2025-11-30
"""

import sys
from pathlib import Path
import numpy as np
from io import StringIO
from collections import defaultdict

# Add framework to path
REPO_ROOT = Path(__file__).parent.parent.parent
sys.path.insert(0, str(REPO_ROOT / "tests"))

from framework import (
    load_binary_halos,
    TEST_DATA_DIR,
    MIMIC_EXE,
    ensure_output_dirs,
    run_mimic,
)

# Import tree loader
from framework.tree_loader import load_binary_tree, get_halos_by_snapshot

# Ensure output directories exist
ensure_output_dirs()


def match_halos_snapshot63(input_halos, output_halos, snapshot=63):
    """
    Match snapshot 63 input halos to Type 0+1 output halos.

    Matching strategy:
    1. Filter input to SnapNum=63 only
    2. Filter output to SnapNum=63 AND Type in [0,1] (skip orphans)
    3. Match by SimulationHaloIndex (output) = MostBoundID (input)
    4. Categorize ALL matches (no filtering to 1:1)

    Args:
        input_halos: Tree input halos (recarray from load_binary_tree)
        output_halos: Mimic output halos (recarray from load_binary_halos)
        snapshot: Snapshot to validate (default 63)

    Returns:
        dict: {
            'input_snap63': np.array of input indices at snapshot 63,
            'output_type01': np.array of output indices (Type 0+1 at snapshot),
            'matched_pairs': List of (input_idx, output_idx),
            'unmatched_input': List of input indices not matched,
            'unmatched_output': List of output indices not matched,
            'input_count': Total input halos at snapshot,
            'output_count': Total Type 0+1 output halos at snapshot,
        }
    """
    # Filter input to snapshot 63
    input_snap_mask = input_halos.SnapNum == snapshot
    input_snap63_indices = np.where(input_snap_mask)[0]

    # Filter output to snapshot 63 AND Type 0+1 (skip orphans)
    output_mask = (output_halos.SnapNum == snapshot) & (output_halos.Type != 2)
    output_type01_indices = np.where(output_mask)[0]

    # Build lookup: MostBoundID → input index (for snap 63 halos)
    input_lookup = {}
    for in_idx in input_snap63_indices:
        mbid = input_halos[in_idx].MostBoundID
        # Store ALL input halos with same MostBoundID (should be unique, but check)
        if mbid in input_lookup:
            input_lookup[mbid].append(in_idx)
        else:
            input_lookup[mbid] = [in_idx]

    # Match output Type 0+1 halos to input
    matched_pairs = []
    matched_input_set = set()
    matched_output_set = set()

    for out_idx in output_type01_indices:
        sim_halo_idx = output_halos[out_idx].SimulationHaloIndex

        # Find corresponding input halo(s)
        if sim_halo_idx in input_lookup:
            # Should be exactly one, but handle multiple if they exist
            input_matches = input_lookup[sim_halo_idx]
            for in_idx in input_matches:
                matched_pairs.append((in_idx, out_idx))
                matched_input_set.add(in_idx)
                matched_output_set.add(out_idx)

    # Find unmatched
    unmatched_input = [idx for idx in input_snap63_indices if idx not in matched_input_set]
    unmatched_output = [idx for idx in output_type01_indices if idx not in matched_output_set]

    return {
        'input_snap63': input_snap63_indices,
        'output_type01': output_type01_indices,
        'matched_pairs': matched_pairs,
        'unmatched_input': unmatched_input,
        'unmatched_output': unmatched_output,
        'input_count': len(input_snap63_indices),
        'output_count': len(output_type01_indices),
    }


def validate_copied_properties(input_halo, output_halo, halonr, input_halos, rtol=1e-6):
    """
    Validate properties directly copied from input tree to output.

    Based on halo_properties.yaml, these properties have init_source or output_source
    with copy_from_tree or copy_from_tree_array:
        - SnapNum: int (exact)
        - Pos[3]: float (rtol)
        - Vel[3]: float (rtol)
        - Spin[3]: float (rtol)
        - Len: int (exact)
        - Vmax: float (rtol)
        - VelDisp: float (rtol)
        - MostBoundID → SimulationHaloIndex: long long (exact)

    Conditional validation:
        - Mvir: For Type=0 centrals where halonr==FirstHaloInFOFgroup AND Mvir>=0.0,
                Mvir is copied from input (see get_virial_mass in virial.c)

    Properties NOT validated:
        - Mvir: For satellites and centrals with invalid input Mvir (calculated value)
        - All other properties: Not directly copied from input

    Args:
        input_halo: Input tree halo (recarray element)
        output_halo: Output halo (recarray element)
        halonr: HaloNr (index in InputTreeHalos)
        input_halos: Full input halos array (needed for FOF check)
        rtol: Relative tolerance for float comparison (default 1e-6)

    Returns:
        dict: {
            'passed': bool,
            'mismatches': List of (property, input_val, output_val, rel_diff)
        }
    """
    mismatches = []

    # Vector properties - check component-wise
    for prop in ['Pos', 'Vel', 'Spin']:
        for i, comp in enumerate(['x', 'y', 'z']):
            in_val = input_halo[prop][i]
            out_val = output_halo[prop][i]
            if not np.isclose(in_val, out_val, rtol=rtol, atol=0):
                rel_diff = abs(in_val - out_val) / (abs(in_val) + 1e-30)
                mismatches.append((f"{prop}[{comp}]", in_val, out_val, rel_diff))

    # Integer properties - exact match required
    for prop in ['Len', 'SnapNum']:
        if input_halo[prop] != output_halo[prop]:
            mismatches.append((prop, input_halo[prop], output_halo[prop], None))

    # Float properties - relative tolerance
    for prop in ['Vmax', 'VelDisp']:
        in_val = input_halo[prop]
        out_val = output_halo[prop]
        if not np.isclose(in_val, out_val, rtol=rtol, atol=0):
            rel_diff = abs(in_val - out_val) / (abs(in_val) + 1e-30)
            mismatches.append((prop, in_val, out_val, rel_diff))

    # MostBoundID → SimulationHaloIndex
    if input_halo['MostBoundID'] != output_halo['SimulationHaloIndex']:
        mismatches.append((
            'MostBoundID→SimulationHaloIndex',
            input_halo['MostBoundID'],
            output_halo['SimulationHaloIndex'],
            None
        ))

    # Mvir: Conditional validation for Type=0 centrals
    # From get_virial_mass() in src/core/halo_properties/virial.c:
    # If halonr == FirstHaloInFOFgroup AND Mvir >= 0.0, then Mvir is copied
    if output_halo.Type == 0:  # Central halo
        is_fof_central = (halonr == input_halos[halonr].FirstHaloInFOFgroup)
        has_valid_mvir = (input_halo['Mvir'] >= 0.0)

        if is_fof_central and has_valid_mvir:
            in_val = input_halo['Mvir']
            out_val = output_halo['Mvir']
            if not np.isclose(in_val, out_val, rtol=rtol, atol=0):
                rel_diff = abs(in_val - out_val) / (abs(in_val) + 1e-30)
                mismatches.append(('Mvir', in_val, out_val, rel_diff))

    return {
        'passed': len(mismatches) == 0,
        'mismatches': mismatches
    }


def format_validation_failures(failures, halo_type):
    """
    Format validation failures into readable report.

    Groups failures by property and shows example mismatches.

    Args:
        failures: List of (input_idx, output_idx, mismatches)
            where mismatches is list of (property, input_val, output_val, rel_diff)
        halo_type: String like "CENTRALS" or "SATELLITES" for report header

    Returns:
        str: Formatted report string
    """
    report = StringIO()

    report.write(f"\n{'='*60}\n")
    report.write(f"{halo_type} VALIDATION FAILURES\n")
    report.write(f"{'='*60}\n")
    report.write(f"Total failures: {len(failures)}\n\n")

    # Group by property
    property_failures = defaultdict(list)
    for in_idx, out_idx, mismatches in failures:
        for prop, expected, actual, diff in mismatches:
            property_failures[prop].append((in_idx, out_idx, expected, actual, diff))

    # Report by property
    for prop, prop_failures in sorted(property_failures.items()):
        report.write(f"\nProperty: {prop}\n")
        report.write(f"  Failures: {len(prop_failures)}\n")

        # Show first 5 examples
        for i, (in_idx, out_idx, exp, act, diff) in enumerate(prop_failures[:5]):
            if diff is not None:
                report.write(f"    Input[{in_idx}] → Output[{out_idx}]: "
                           f"expected={exp:.6e}, actual={act:.6e}, "
                           f"rel_diff={diff:.2e}\n")
            else:
                report.write(f"    Input[{in_idx}] → Output[{out_idx}]: "
                           f"expected={exp}, actual={act}\n")

        if len(prop_failures) > 5:
            report.write(f"    ... and {len(prop_failures) - 5} more\n")

    return report.getvalue()


def test_tree_preservation_coverage():
    """
    Validate snapshot 63 halo counts and matching coverage.

    What: Check if Type 0+1 output count matches input count at snapshot 63
    Expected: Counts should match if no halos lost/added
    Validates: Halo processing doesn't incorrectly lose or add halos
    """
    print("Testing tree preservation coverage...")

    if not MIMIC_EXE.exists():
        print("  Skipping (Mimic not built)")
        return

    # Load input tree
    tree_file = TEST_DATA_DIR / "input" / "trees_063.0"
    print(f"  Loading input: {tree_file.relative_to(REPO_ROOT)}")
    input_halos, tree_meta = load_binary_tree(tree_file)
    print(f"    → {tree_meta['totNHalos']} total halos, {tree_meta['Ntrees']} trees")

    # Load output
    output_dir = TEST_DATA_DIR / "output" / "binary"
    output_file = output_dir / "model_z0.000_0"

    # Run Mimic if needed
    if not output_file.exists():
        print(f"  Running Mimic to generate output...")
        param_file = TEST_DATA_DIR / "test_binary.yaml"
        returncode, _, stderr = run_mimic(param_file)
        assert returncode == 0, f"Mimic execution failed: {stderr}"

    print(f"  Loading output: {output_file.relative_to(REPO_ROOT)}")
    output_halos, output_meta = load_binary_halos(output_file)
    print(f"    → {output_meta['TotHalos']} total halos")

    # Match halos at snapshot 63
    print(f"\n  Matching snapshot 63 halos...")
    match_result = match_halos_snapshot63(input_halos, output_halos, snapshot=63)

    input_count = match_result['input_count']
    output_count = match_result['output_count']
    matched_count = len(match_result['matched_pairs'])
    unmatched_input_count = len(match_result['unmatched_input'])
    unmatched_output_count = len(match_result['unmatched_output'])

    # Report counts
    print(f"  Input snapshot 63 halos: {input_count}")
    print(f"  Output Type 0+1 halos: {output_count}")
    print(f"  Matched pairs: {matched_count}")
    print(f"  Unmatched input: {unmatched_input_count}")
    print(f"  Unmatched output: {unmatched_output_count}")

    # Check if counts match
    if input_count == output_count:
        print(f"  ✓ Counts match! No halos lost or added.")
    else:
        diff = output_count - input_count
        if diff > 0:
            print(f"  ⚠ Output has {diff} MORE halos than input (investigate)")
        else:
            print(f"  ⚠ Output has {-diff} FEWER halos than input (investigate)")

    # Validate matching coverage
    match_rate = matched_count / input_count * 100 if input_count > 0 else 0
    print(f"\n  Match rate: {match_rate:.1f}%")

    # Diagnostic information about unmatched input halos
    if unmatched_input_count > 0:
        unmatched_halos = input_halos[match_result['unmatched_input']]
        print(f"\n  Diagnostic: Unmatched input halos ({unmatched_input_count}):")
        print(f"    Len range: {np.min(unmatched_halos.Len)} - {np.max(unmatched_halos.Len)}")
        print(f"    Mvir range: {np.min(unmatched_halos.Mvir):.3f} - {np.max(unmatched_halos.Mvir):.3f} (1e10 Msun/h)")
        print(f"    Vmax range: {np.min(unmatched_halos.Vmax):.2f} - {np.max(unmatched_halos.Vmax):.2f} (km/s)")

        # Check if all have Mvir=0.0
        mvir_zero_count = np.sum(unmatched_halos.Mvir == 0.0)
        if mvir_zero_count == unmatched_input_count:
            print(f"    Note: All {mvir_zero_count} unmatched halos have Mvir=0.0 (invalid mass)")
            # Check if matched halos also have Mvir=0.0
            matched_indices = [idx for idx, _ in match_result['matched_pairs']]
            matched_halos = input_halos[matched_indices]
            matched_mvir_zero = np.sum(matched_halos.Mvir == 0.0)
            print(f"    Note: {matched_mvir_zero} matched halos also have Mvir=0.0")
            print(f"    → Mvir=0.0 alone is NOT the filtering criterion")

    # Expect high match rate (>95%)
    assert matched_count > 0, "No matches found! Matching algorithm broken."
    assert match_rate > 95.0, (
        f"Match rate too low: {match_rate:.1f}% (expected >95%). "
        f"This suggests halos are being lost in processing."
    )

    # Expect no unmatched output (all output should be in input)
    assert unmatched_output_count == 0, (
        f"Found {unmatched_output_count} output halos NOT in input! "
        f"This suggests halos are being incorrectly created."
    )

    print(f"\n  ✓ Coverage validation passed")
    if unmatched_input_count > 0:
        print(f"  Note: {unmatched_input_count} input halos not in output (may be filtered by processing)")


def test_tree_preservation_properties():
    """
    Validate copied properties for ALL matched snapshot 63 halos.

    What: Compares all directly-copied properties (Pos, Vel, Spin, Len, Vmax, VelDisp, etc.)
    Expected: Perfect matches within tolerance (1e-6)
    Validates: Physics-free data pipeline preserves input exactly
    Categorizes: Perfect matches, imperfect matches (with breakdown)
    """
    print("Testing tree preservation of copied properties...")

    if not MIMIC_EXE.exists():
        print("  Skipping (Mimic not built)")
        return

    # Load data
    tree_file = TEST_DATA_DIR / "input" / "trees_063.0"
    input_halos, _ = load_binary_tree(tree_file)

    output_file = TEST_DATA_DIR / "output" / "binary" / "model_z0.000_0"
    if not output_file.exists():
        param_file = TEST_DATA_DIR / "test_binary.yaml"
        returncode, _, stderr = run_mimic(param_file)
        assert returncode == 0, f"Mimic execution failed: {stderr}"

    output_halos, _ = load_binary_halos(output_file)

    # Match halos at snapshot 63
    match_result = match_halos_snapshot63(input_halos, output_halos, snapshot=63)
    matched_pairs = match_result['matched_pairs']

    print(f"  Validating {len(matched_pairs)} matched halos...")

    # Validate each matched pair
    perfect_matches = []
    imperfect_matches = []

    for in_idx, out_idx in matched_pairs:
        halonr = output_halos[out_idx].MimicHaloIndex
        result = validate_copied_properties(
            input_halos[in_idx],
            output_halos[out_idx],
            halonr,
            input_halos,
            rtol=1e-6
        )
        if result['passed']:
            perfect_matches.append((in_idx, out_idx))
        else:
            imperfect_matches.append((in_idx, out_idx, result['mismatches']))

    # Calculate statistics
    total = len(matched_pairs)
    perfect_count = len(perfect_matches)
    imperfect_count = len(imperfect_matches)
    perfect_pct = perfect_count / total * 100 if total > 0 else 0
    imperfect_pct = imperfect_count / total * 100 if total > 0 else 0

    # Report results
    print(f"\n  Validation Results:")
    print(f"    Total matched: {total}")
    print(f"    Perfect matches: {perfect_count} ({perfect_pct:.1f}%)")
    print(f"    Imperfect matches: {imperfect_count} ({imperfect_pct:.1f}%)")

    # Break down by halo type
    central_perfect = sum(1 for in_idx, out_idx in perfect_matches if output_halos[out_idx].Type == 0)
    central_imperfect = sum(1 for in_idx, out_idx, _ in imperfect_matches if output_halos[out_idx].Type == 0)
    satellite_perfect = sum(1 for in_idx, out_idx in perfect_matches if output_halos[out_idx].Type == 1)
    satellite_imperfect = sum(1 for in_idx, out_idx, _ in imperfect_matches if output_halos[out_idx].Type == 1)

    print(f"\n  By halo type:")
    print(f"    Centrals (Type=0): {central_perfect} perfect, {central_imperfect} imperfect")
    print(f"    Satellites (Type=1): {satellite_perfect} perfect, {satellite_imperfect} imperfect")

    # If there are imperfect matches, report details
    if imperfect_matches:
        report = format_validation_failures(imperfect_matches, "PROPERTY VALIDATION")
        print(f"\n{report}")
        assert False, f"Found {imperfect_count} halos with property mismatches (see details above)"

    print(f"  ✓ All {perfect_count} halos have perfectly preserved properties")
    print(f"    Properties validated: Pos[3], Vel[3], Spin[3], Len, Vmax, VelDisp, SnapNum, MostBoundID")
    print(f"    Mvir validated conditionally for Type=0 centrals (when input Mvir >= 0.0)")


def main():
    """Main test runner."""
    print("Integration Test: Tree Preservation (Input → Output)")
    print(f"Repository root: {REPO_ROOT}")
    print(f"Mimic executable: {MIMIC_EXE}")

    # ANSI color codes
    RED = '\033[0;31m'
    GREEN = '\033[0;32m'
    YELLOW = '\033[0;33m'
    NC = '\033[0m'  # No Color

    if not MIMIC_EXE.exists():
        print(f"{RED}ERROR: Mimic executable not found: {MIMIC_EXE}{NC}")
        print("Build it first with: make")
        return 1

    tests = [
        test_tree_preservation_coverage,
        test_tree_preservation_properties,
    ]

    passed = 0
    failed = 0
    skipped = 0

    for test in tests:
        print()
        try:
            test()
            print(f"{GREEN}✓ PASS: {test.__name__}{NC}")
            passed += 1
        except AssertionError as e:
            print(f"{RED}✗ FAIL: {test.__name__}{NC}")
            print(f"  {e}")
            failed += 1
        except Exception as e:
            print(f"{RED}✗ ERROR: {test.__name__}{NC}")
            print(f"  {e}")
            failed += 1

    print()
    print("=" * 60)
    print("Test Summary: Tree Preservation")
    print("=" * 60)
    print(f"Passed:  {passed}")
    print(f"Failed:  {failed}")
    print(f"Skipped: {skipped}")
    print(f"Total:   {passed + failed + skipped}")
    print("=" * 60)

    if failed == 0:
        print(f"{GREEN}✓ All tests passed!{NC}")
        return 0
    else:
        print(f"{RED}✗ {failed} test(s) failed{NC}")
        return 1


if __name__ == "__main__":
    sys.exit(main())
