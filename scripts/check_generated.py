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

def compute_yaml_hash() -> str:
    """Compute MD5 hash of YAML input files (same logic as generator)."""
    md5 = hashlib.md5()

    # Hash both YAML files in order (halo, then galaxy)
    for yaml_file in YAML_FILES:
        if not yaml_file.exists():
            return ""
        with open(yaml_file, 'rb') as f:
            md5.update(f.read())

    return md5.hexdigest()

def extract_yaml_hash_from_file(path: Path) -> str:
    """Extract 'Source MD5:' hash from generated file header."""
    if not path.exists():
        return ""

    try:
        with open(path, 'r', encoding='utf-8') as f:
            # Read first 20 lines (hash should be in header)
            for _ in range(20):
                line = f.readline()
                if not line:
                    break
                # Look for "Source MD5: <hash>"
                if 'Source MD5:' in line:
                    # Extract hash (32 hex chars)
                    parts = line.split('Source MD5:')
                    if len(parts) >= 2:
                        hash_str = parts[1].strip().strip('*/').strip()
                        if len(hash_str) == 32:  # MD5 is 32 hex chars
                            return hash_str
    except Exception:
        return ""

    return ""

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

def check_yaml_hashes() -> bool:
    """Check if generated files match current YAML content (via MD5 hash)."""

    # Compute current YAML hash
    current_yaml_hash = compute_yaml_hash()
    if not current_yaml_hash:
        print("✗ ERROR: Cannot compute YAML hash (files missing or unreadable)")
        return False

    # Check each generated file's embedded hash
    mismatches = []
    missing_hash = []

    for gen_file in GENERATED_FILES:
        if not gen_file.exists():
            mismatches.append(gen_file.name)
            continue

        embedded_hash = extract_yaml_hash_from_file(gen_file)
        if not embedded_hash:
            missing_hash.append(gen_file.name)
            continue

        if embedded_hash != current_yaml_hash:
            mismatches.append(gen_file.name)

    if missing_hash:
        print("✗ WARNING: Some files missing embedded hash (may be old format)")
        for filename in missing_hash:
            print(f"    - {filename}")
        print()

    if mismatches:
        print("✗ OUT OF DATE: YAML metadata changed, generated files need updating")
        print()
        print(f"  Current YAML hash:  {current_yaml_hash}")
        print(f"  Embedded hash:      {embedded_hash if embedded_hash else '(missing)'}")
        print()
        print("  Files need regeneration:")
        for filename in mismatches:
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

    # Check 3: YAML hash consistency
    print("[3/4] Checking YAML hash consistency...")
    if check_yaml_hashes():
        print("✓ Generated files match current YAML metadata (hash verified)")
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
