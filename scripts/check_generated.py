#!/usr/bin/env python3
"""
Check Generated Code Validator for Mimic

Verifies that generated files are up-to-date with property metadata.
Used in CI to catch when YAML is modified but `make generate` wasn't run.

Usage:
    python3 scripts/check_generated.py

Exit codes:
    0 - All generated files are up-to-date
    1 - Generated files are out of date or missing

Author: Property Metadata System (Phase 1)
Date: 2025-11-07
"""

import hashlib
import sys
from pathlib import Path
from typing import List

# ==============================================================================
# PATHS
# ==============================================================================

# Repository root (parent of scripts/)
REPO_ROOT = Path(__file__).parent.parent

# Input YAML files
YAML_FILES = [
    REPO_ROOT / 'metadata' / 'properties' / 'halo_properties.yaml',
    REPO_ROOT / 'metadata' / 'properties' / 'galaxy_properties.yaml',
]

# Generated files to check
GENERATED_FILES = [
    REPO_ROOT / 'src' / 'include' / 'generated' / 'property_defs.h',
    REPO_ROOT / 'src' / 'include' / 'generated' / 'init_halo_properties.inc',
    REPO_ROOT / 'src' / 'include' / 'generated' / 'init_galaxy_properties.inc',
    REPO_ROOT / 'src' / 'include' / 'generated' / 'copy_to_output.inc',
    REPO_ROOT / 'src' / 'include' / 'generated' / 'hdf5_field_count.inc',
    REPO_ROOT / 'src' / 'include' / 'generated' / 'hdf5_field_definitions.inc',
    REPO_ROOT / 'output' / 'mimic-plot' / 'generated_dtype.py',
]

# ==============================================================================
# UTILITIES
# ==============================================================================

def file_md5(path: Path) -> str:
    """Calculate MD5 hash of file contents (excluding timestamp lines)."""
    if not path.exists():
        return ""

    md5 = hashlib.md5()
    with open(path, 'rb') as f:
        for line in f:
            # Skip timestamp lines in generated files
            if b'Generated on:' in line:
                continue
            md5.update(line)
    return md5.hexdigest()

def check_file_exists(path: Path, description: str) -> bool:
    """Check if file exists and report."""
    if not path.exists():
        print(f"✗ MISSING: {description}")
        print(f"  Expected: {path.relative_to(REPO_ROOT)}")
        return False
    return True

# ==============================================================================
# VALIDATION
# ==============================================================================

def validate_yaml_files() -> bool:
    """Check that YAML metadata files exist."""
    all_exist = True
    for yaml_file in YAML_FILES:
        if not check_file_exists(yaml_file, f"Property metadata: {yaml_file.name}"):
            all_exist = False
    return all_exist

def validate_generated_files() -> bool:
    """Check that all generated files exist."""
    all_exist = True
    for gen_file in GENERATED_FILES:
        if not check_file_exists(gen_file, f"Generated file: {gen_file.name}"):
            all_exist = False
    return all_exist

def check_timestamps() -> bool:
    """Check if generated files are newer than YAML files."""

    # Get newest YAML timestamp
    yaml_times = [f.stat().st_mtime for f in YAML_FILES if f.exists()]
    if not yaml_times:
        print("✗ ERROR: No YAML metadata files found")
        return False
    newest_yaml_time = max(yaml_times)

    # Check each generated file
    out_of_date = []
    for gen_file in GENERATED_FILES:
        if not gen_file.exists():
            out_of_date.append(gen_file.name)
            continue

        gen_time = gen_file.stat().st_mtime
        if gen_time < newest_yaml_time:
            out_of_date.append(gen_file.name)

    if out_of_date:
        print("✗ OUT OF DATE: Generated files are older than YAML metadata")
        print()
        print("  Files need regeneration:")
        for filename in out_of_date:
            print(f"    - {filename}")
        return False

    return True

def check_file_marker() -> bool:
    """Check that generated files have the AUTO-GENERATED marker."""

    marker = b'AUTO-GENERATED'
    missing_marker = []

    for gen_file in GENERATED_FILES:
        if not gen_file.exists():
            continue

        with open(gen_file, 'rb') as f:
            first_100_bytes = f.read(100)
            if marker not in first_100_bytes:
                missing_marker.append(gen_file.name)

    if missing_marker:
        print("✗ WARNING: Files missing AUTO-GENERATED marker")
        print("  (May be hand-written files, not generated)")
        print()
        for filename in missing_marker:
            print(f"    - {filename}")
        return False

    return True

# ==============================================================================
# MAIN
# ==============================================================================

def main():
    """Main entry point."""

    print("=" * 70)
    print("Checking Generated Code Status")
    print("=" * 70)
    print()

    checks_passed = 0
    checks_total = 4

    # Check 1: YAML files exist
    print("[1/4] Checking YAML metadata files...")
    if validate_yaml_files():
        print("✓ All YAML metadata files present")
        checks_passed += 1
    print()

    # Check 2: Generated files exist
    print("[2/4] Checking generated files...")
    if validate_generated_files():
        print(f"✓ All {len(GENERATED_FILES)} generated files present")
        checks_passed += 1
    print()

    # Check 3: Timestamps
    print("[3/4] Checking file timestamps...")
    if check_timestamps():
        print("✓ Generated files are up-to-date (newer than YAML)")
        checks_passed += 1
    print()

    # Check 4: AUTO-GENERATED marker
    print("[4/4] Checking AUTO-GENERATED markers...")
    if check_file_marker():
        print("✓ All generated files have proper markers")
        checks_passed += 1
    print()

    # Summary
    print("=" * 70)
    if checks_passed == checks_total:
        print("✓ PASS: All checks passed")
        print("=" * 70)
        print()
        print("Generated code is up-to-date with property metadata.")
        return 0
    else:
        print(f"✗ FAIL: {checks_total - checks_passed} check(s) failed")
        print("=" * 70)
        print()
        print("ACTION REQUIRED:")
        print("  Run: make generate")
        print()
        print("This will regenerate all code from property metadata.")
        print("Then commit the updated generated files to git.")
        print()
        return 1

if __name__ == '__main__':
    sys.exit(main())
