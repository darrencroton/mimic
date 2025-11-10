#!/usr/bin/env python3
"""
Scientific Validation Test

Validates: Physical correctness and reasonable ranges of halo properties
Phase: Phase 2 (Testing Framework)

This comprehensive test validates all halo properties:
- No NaN or Inf values (FAIL if found)
- Zero values (WARNING - counted but not failure)
- Physical ranges for all properties
- Positions within simulation box
- Minimum particle counts

Note: Internal units are 10^10 Msun/h for masses

Author: Mimic Testing Team
Date: 2025-11-10
"""

import subprocess
import sys
from pathlib import Path
import numpy as np

# Add framework to path
REPO_ROOT = Path(__file__).parent.parent.parent
sys.path.insert(0, str(REPO_ROOT / "tests"))

from framework import load_binary_halos

# Repository paths
TEST_DATA_DIR = REPO_ROOT / "tests" / "data"
MIMIC_EXE = REPO_ROOT / "mimic"

# Expected ranges
# Mass units: 10^10 Msun/h (internal units)
# So 1e-5 = 10^5 Msun/h, 10000.0 = 10^14 Msun/h
EXPECTED_RANGES = {
    'Mvir': {'min': 1.0e-5, 'max': 10000.0, 'units': '10^10 Msun/h'},
    'Rvir': {'min': 0.001, 'max': 10.0, 'units': 'Mpc/h'},
    'Vvir': {'min': 10.0, 'max': 5000.0, 'units': 'km/s'},
    'Vmax': {'min': 10.0, 'max': 5000.0, 'units': 'km/s'},
    'Len': {'min': 20, 'max': 1e9, 'units': 'particles'},
    'BoxSize': 62.5,  # Mpc/h
    'MaxSnap': 63,
}

# ANSI color codes
RED = '\033[0;31m'
GREEN = '\033[0;32m'
YELLOW = '\033[1;33m'
NC = '\033[0m'  # No Color


def run_mimic_if_needed():
    """
    Run Mimic if output doesn't exist

    Returns:
        Path: Path to output file
    """
    output_dir = TEST_DATA_DIR / "output" / "binary"
    output_file = output_dir / "model_z0.000_0"  # snapshot 63 is z=0

    if not output_file.exists():
        print("  Running Mimic to generate output...")
        param_file = TEST_DATA_DIR / "test_binary.par"
        result = subprocess.run(
            [str(MIMIC_EXE), str(param_file)],
            cwd=str(REPO_ROOT),
            capture_output=True,
            text=True
        )
        if result.returncode != 0:
            print(f"STDOUT:\n{result.stdout}")
            print(f"STDERR:\n{result.stderr}")
            raise RuntimeError(f"Mimic execution failed with code {result.returncode}")

    return output_file


def check_nans_infs(halos):
    """
    Check for NaN and Inf values in all fields

    Returns:
        dict: Results with 'passed', 'nan_fields', 'inf_fields'
    """
    nan_fields = {}
    inf_fields = {}

    # Check scalar fields
    for field in ['Mvir', 'Rvir', 'Vvir', 'Vmax', 'Len', 'SnapNum']:
        data = getattr(halos, field)

        nan_count = np.sum(np.isnan(data))
        if nan_count > 0:
            indices = np.where(np.isnan(data))[0][:5]
            nan_fields[field] = {
                'count': nan_count,
                'examples': [(int(i), float(data[i])) for i in indices]
            }

        inf_count = np.sum(np.isinf(data))
        if inf_count > 0:
            indices = np.where(np.isinf(data))[0][:5]
            inf_fields[field] = {
                'count': inf_count,
                'examples': [(int(i), float(data[i])) for i in indices]
            }

    # Check position vector
    nan_pos = np.sum(np.isnan(halos.Pos))
    if nan_pos > 0:
        nan_fields['Pos'] = {'count': nan_pos, 'examples': []}

    inf_pos = np.sum(np.isinf(halos.Pos))
    if inf_pos > 0:
        inf_fields['Pos'] = {'count': inf_pos, 'examples': []}

    return {
        'passed': len(nan_fields) == 0 and len(inf_fields) == 0,
        'nan_fields': nan_fields,
        'inf_fields': inf_fields
    }


def check_zeros(halos):
    """
    Check for zero values in physical quantities
    These are warnings, not failures

    Returns:
        dict: Field name -> count and examples
    """
    zero_counts = {}

    for field in ['Mvir', 'Rvir', 'Vvir', 'Vmax']:
        data = getattr(halos, field)
        zero_mask = data == 0.0
        count = np.sum(zero_mask)

        if count > 0:
            indices = np.where(zero_mask)[0][:5]
            zero_counts[field] = {
                'count': count,
                'examples': [(int(i), float(data[i])) for i in indices]
            }

    return zero_counts


def check_range(halos, field, min_val, max_val, exclude_zeros=False):
    """
    Check if field values are within expected range

    Args:
        halos: Halo data
        field: Field name
        min_val: Minimum expected value
        max_val: Maximum expected value
        exclude_zeros: If True, don't count zeros as below minimum

    Returns:
        dict: Results with 'passed', counts, examples
    """
    data = getattr(halos, field)

    if exclude_zeros:
        # For range checking, exclude zeros (they're already warned about)
        valid_mask = data != 0.0
        data_to_check = data[valid_mask]
    else:
        data_to_check = data

    below_min = (data_to_check < min_val)
    above_max = (data_to_check > max_val)

    count_below = np.sum(below_min)
    count_above = np.sum(above_max)

    result = {
        'passed': count_below == 0 and count_above == 0,
        'min_value': float(np.min(data_to_check)) if len(data_to_check) > 0 else 0.0,
        'max_value': float(np.max(data_to_check)) if len(data_to_check) > 0 else 0.0,
        'count_below': count_below,
        'count_above': count_above,
        'examples_below': [],
        'examples_above': []
    }

    if count_below > 0:
        if exclude_zeros:
            # Find original indices
            original_indices = np.where(valid_mask)[0]
            below_in_valid = np.where(below_min)[0][:5]
            actual_indices = original_indices[below_in_valid]
        else:
            actual_indices = np.where(below_min)[0][:5]

        result['examples_below'] = [(int(i), float(data[i])) for i in actual_indices]

    if count_above > 0:
        if exclude_zeros:
            original_indices = np.where(valid_mask)[0]
            above_in_valid = np.where(above_max)[0][:5]
            actual_indices = original_indices[above_in_valid]
        else:
            actual_indices = np.where(above_max)[0][:5]

        result['examples_above'] = [(int(i), float(data[i])) for i in actual_indices]

    return result


def test_numerical_validity():
    """
    Test for NaN and Inf values (critical failures)
    """
    print("\n" + "="*60)
    print("NUMERICAL VALIDITY (NaN/Inf checks)")
    print("="*60)

    if not MIMIC_EXE.exists():
        print(f"  Skipping (Mimic not built)")
        return True, 0

    output_file = run_mimic_if_needed()
    halos, metadata = load_binary_halos(output_file)
    print(f"Loaded {metadata['TotHalos']} halos from {metadata['Ntrees']} trees\n")

    result = check_nans_infs(halos)

    if result['nan_fields']:
        print(f"{RED}✗ FAIL: Found NaN values in {len(result['nan_fields'])} field(s):{NC}")
        for field, info in result['nan_fields'].items():
            print(f"  {field}: {info['count']} NaN values")
            if info['examples']:
                for idx, val in info['examples']:
                    print(f"    Halo {idx}: {field} = {val}")
        return False, 1

    if result['inf_fields']:
        print(f"{RED}✗ FAIL: Found Inf values in {len(result['inf_fields'])} field(s):{NC}")
        for field, info in result['inf_fields'].items():
            print(f"  {field}: {info['count']} Inf values")
            if info['examples']:
                for idx, val in info['examples']:
                    print(f"    Halo {idx}: {field} = {val}")
        return False, 1

    print(f"{GREEN}✓ PASS: No NaN or Inf values found{NC}")
    return True, 0


def test_zero_values():
    """
    Check for zero values (warnings, not failures)
    """
    print("\n" + "="*60)
    print("ZERO VALUE CHECKS (warnings)")
    print("="*60)

    if not MIMIC_EXE.exists():
        print(f"  Skipping (Mimic not built)")
        return True, 0

    output_file = run_mimic_if_needed()
    halos, metadata = load_binary_halos(output_file)
    total_halos = metadata['TotHalos']

    zero_counts = check_zeros(halos)

    if zero_counts:
        print(f"{YELLOW}⚠ WARNING: Found zero values in {len(zero_counts)} field(s):{NC}")
        for field, info in zero_counts.items():
            print(f"  {field}: {info['count']} zero values out of {total_halos} total")
            for idx, val in info['examples']:
                print(f"    Halo {idx}: {field} = {val}")
        print()
        return True, len(zero_counts)  # Warnings, not failures
    else:
        print(f"{GREEN}✓ No zero values found{NC}")
        return True, 0


def test_physical_ranges():
    """
    Test that all properties are within physically reasonable ranges
    """
    print("\n" + "="*60)
    print("PHYSICAL RANGE VALIDATION")
    print("="*60)

    # Print expected ranges
    print("\nExpected ranges:")
    for field, spec in EXPECTED_RANGES.items():
        if isinstance(spec, dict):
            print(f"  {field}: {spec['min']:.2e} to {spec['max']:.2e} {spec['units']}")
    print()

    if not MIMIC_EXE.exists():
        print(f"  Skipping (Mimic not built)")
        return True, 0

    output_file = run_mimic_if_needed()
    halos, metadata = load_binary_halos(output_file)

    failures = 0
    total_tests = 0

    # Test each field
    for field in ['Mvir', 'Rvir', 'Vvir', 'Vmax']:
        total_tests += 1
        spec = EXPECTED_RANGES[field]

        print(f"Testing {field} range (non-zero values only)...")
        result = check_range(halos, field, spec['min'], spec['max'], exclude_zeros=True)

        if not result['passed']:
            failures += 1
            print(f"{RED}✗ FAIL: {field} non-zero values outside expected range:{NC}")
            print(f"  Expected: {spec['min']:.2e} to {spec['max']:.2e} {spec['units']}")
            print(f"  Actual range: {result['min_value']:.2e} to {result['max_value']:.2e} {spec['units']}")

            if result['count_below'] > 0:
                print(f"  {result['count_below']} halos below minimum (showing 5):")
                for idx, val in result['examples_below']:
                    print(f"    Halo {idx}: {field} = {val:.2e}")

            if result['count_above'] > 0:
                print(f"  {result['count_above']} halos above maximum (showing 5):")
                for idx, val in result['examples_above']:
                    print(f"    Halo {idx}: {field} = {val:.2e}")
        else:
            print(f"{GREEN}✓ PASS: All non-zero halos in expected range{NC}")
            print(f"  Actual range: {result['min_value']:.2e} to {result['max_value']:.2e} {spec['units']}")
        print()

    # Test particle counts
    total_tests += 1
    print(f"Testing particle count range...")
    len_result = check_range(halos, 'Len', EXPECTED_RANGES['Len']['min'],
                             EXPECTED_RANGES['Len']['max'], exclude_zeros=False)

    if not len_result['passed']:
        failures += 1
        print(f"{RED}✗ FAIL: Particle counts outside expected range:{NC}")
        if len_result['count_below'] > 0:
            print(f"  {len_result['count_below']} halos below minimum (showing 5):")
            for idx, val in len_result['examples_below']:
                print(f"    Halo {idx}: Len = {int(val)}")
    else:
        print(f"{GREEN}✓ PASS: All halos meet particle count requirement{NC}")
        print(f"  Range: {int(len_result['min_value'])} to {int(len_result['max_value'])} particles")
    print()

    # Test positions
    total_tests += 1
    print(f"Testing position ranges...")
    BoxSize = EXPECTED_RANGES['BoxSize']
    violations = []

    for i, axis in enumerate(['x', 'y', 'z']):
        pos_component = halos.Pos[:, i]
        below_zero = np.sum(pos_component < 0)
        above_box = np.sum(pos_component >= BoxSize)

        if below_zero > 0:
            violations.append(f"{axis}: {below_zero} halos < 0")
        if above_box > 0:
            violations.append(f"{axis}: {above_box} halos >= {BoxSize}")

    if violations:
        failures += 1
        print(f"{RED}✗ FAIL: Positions outside box:{NC}")
        for v in violations:
            print(f"  {v}")
    else:
        print(f"{GREEN}✓ PASS: All positions inside simulation box{NC}")
        print(f"  Box size: {BoxSize} Mpc/h")
        for i, axis in enumerate(['x', 'y', 'z']):
            pos_min = np.min(halos.Pos[:, i])
            pos_max = np.max(halos.Pos[:, i])
            print(f"  {axis} range: {pos_min:.4f} to {pos_max:.4f} Mpc/h")
    print()

    return failures == 0, failures


def main():
    """
    Main test runner
    """
    print("=" * 60)
    print("SCIENTIFIC VALIDATION TEST")
    print("=" * 60)
    print(f"Repository root: {REPO_ROOT}")
    print(f"Mimic executable: {MIMIC_EXE}")

    # Check prerequisites
    if not MIMIC_EXE.exists():
        print(f"\n{RED}ERROR: Mimic executable not found: {MIMIC_EXE}{NC}")
        print("Build it first with: make")
        return 1

    # Run test sections
    passed_sections = 0
    failed_sections = 0
    warning_count = 0

    # 1. Numerical validity (critical)
    passed, failures = test_numerical_validity()
    if passed:
        passed_sections += 1
    else:
        failed_sections += failures

    # 2. Zero values (warnings)
    passed, warnings = test_zero_values()
    warning_count = warnings
    if passed:
        passed_sections += 1

    # 3. Physical ranges
    passed, failures = test_physical_ranges()
    if passed:
        passed_sections += 1
    else:
        failed_sections += failures

    # Summary
    print("\n" + "=" * 60)
    print("TEST SUMMARY")
    print("=" * 60)
    print(f"Sections passed: {passed_sections}")
    print(f"Sections failed: {failed_sections}")
    if warning_count > 0:
        print(f"{YELLOW}Warnings: {warning_count} field(s) with zero values{NC}")
    print("=" * 60)

    if failed_sections == 0:
        if warning_count > 0:
            print(f"{YELLOW}✓ All tests passed (with warnings){NC}")
        else:
            print(f"{GREEN}✓ All tests passed!{NC}")
        return 0
    else:
        print(f"{RED}✗ {failed_sections} test(s) failed{NC}")
        return 1


if __name__ == "__main__":
    sys.exit(main())
