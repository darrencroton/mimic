#!/usr/bin/env python3
"""
Check Generated Code Validator for Mimic

Verifies that tracked generated files are up-to-date with their source metadata.
Used in CI to catch when metadata is modified but `make generate` wasn't run.

Current scope:
- Property metadata outputs generated from halo/model property YAML
- The tracked module-generated event contract header

Usage:
    python3 scripts/check_generated.py

Exit codes:
    0 - All generated files are up-to-date
    1 - Generated files are out of date or missing

Author: Property Metadata System (Phase 1)
Date: 2025-11-07
"""

import hashlib
import json
import sys
from pathlib import Path
from typing import List, Tuple

from discovery import (
    REPO_ROOT,
    generated_module_dir,
    halo_property_files,
    model_property_files,
    module_metadata_files,
    standalone_module_files,
    rel,
)

# ==============================================================================
# PATHS
# ==============================================================================

# Property YAML inputs
PROPERTY_YAML_FILES = halo_property_files() + model_property_files()

# Property-generated files to check
PROPERTY_GENERATED_FILES = [
    REPO_ROOT / "src" / "include" / "generated" / "property_defs.h",
    REPO_ROOT / "src" / "include" / "generated" / "init_halo_properties.inc",
    REPO_ROOT / "src" / "include" / "generated" / "init_galaxy_properties.inc",
    REPO_ROOT / "src" / "include" / "generated" / "reset_galaxy_properties.inc",
    REPO_ROOT / "src" / "include" / "generated" / "copy_to_output.inc",
    REPO_ROOT / "src" / "include" / "generated" / "hdf5_field_count.inc",
    REPO_ROOT / "src" / "include" / "generated" / "hdf5_field_definitions.inc",
    REPO_ROOT / "src" / "include" / "generated" / "hdf5_field_metadata.inc",
    REPO_ROOT / "output" / "mimic-plot" / "generated" / "dtype.py",
    REPO_ROOT / "output" / "mimic-plot" / "generated" / "__init__.py",
    REPO_ROOT / "tests" / "generated" / "property_ranges.json",
]

# Module metadata inputs / tracked generated outputs
MODULE_EVENT_CONTRACTS_H = generated_module_dir() / "event_contracts.h"
MODULE_GENERATED_FILES = [MODULE_EVENT_CONTRACTS_H]

# ==============================================================================
# UTILITIES
# ==============================================================================


def compute_property_yaml_hash() -> str:
    """Compute MD5 hash of property YAML input files (same logic as generator)."""
    md5 = hashlib.md5()

    # Hash every property package in the same order as the generator.
    for yaml_file in PROPERTY_YAML_FILES:
        if not yaml_file.exists():
            return ""
        with open(yaml_file, "rb") as f:
            md5.update(rel(yaml_file).encode("utf-8"))
            md5.update(f.read())

    return md5.hexdigest()


def discover_module_metadata_entries() -> List[Tuple[str, str, Path]]:
    """Discover module metadata inputs using the same file-selection rules as the generator.

    Returns:
        Tuples of (kind, module_name, path).
    """
    entries: List[Tuple[str, str, Path]] = []

    for yaml_path in module_metadata_files():
        entries.append(("yaml", yaml_path.parent.name, yaml_path))

    for c_file in standalone_module_files():
        entries.append(("standalone", c_file.stem, c_file))

    entries.sort(key=lambda entry: entry[1])
    return entries


def compute_module_metadata_hash() -> str:
    """Compute MD5 hash of module metadata."""
    entries = discover_module_metadata_entries()
    if not entries:
        return ""

    md5 = hashlib.md5()
    for kind, _module_name, path in entries:
        if kind == "standalone":
            md5.update(f"standalone:{rel(path)}".encode("utf-8"))
        with open(path, "rb") as f:
            md5.update(f.read())

    return md5.hexdigest()


def extract_yaml_hash_from_file(path: Path) -> str:
    """Extract 'Source MD5:' hash from generated file header."""
    if not path.exists():
        return ""

    try:
        if path.suffix == ".json":
            with open(path, "r", encoding="utf-8") as f:
                data = json.load(f)
            return data.get("_metadata", {}).get("source_md5", "")

        with open(path, "r", encoding="utf-8") as f:
            # Read first 20 lines (hash should be in header)
            for _ in range(20):
                line = f.readline()
                if not line:
                    break
                # Look for "Source MD5: <hash>"
                if "Source MD5:" in line:
                    # Extract hash (32 hex chars)
                    parts = line.split("Source MD5:")
                    if len(parts) >= 2:
                        hash_str = parts[1].strip().strip("*/").strip()
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


def validate_property_yaml_files() -> bool:
    """Check that property YAML metadata files exist."""
    all_exist = True
    for yaml_file in PROPERTY_YAML_FILES:
        if not check_file_exists(yaml_file, f"Property metadata: {yaml_file.name}"):
            all_exist = False
    return all_exist


def validate_module_metadata_inputs() -> bool:
    """Check that module metadata inputs can be discovered."""
    entries = discover_module_metadata_entries()
    if not entries:
        print("✗ MISSING: No module metadata inputs discovered")
        print("  Expected under configured model/module roots")
        return False
    return True


def validate_generated_files(files: List[Path], description: str) -> bool:
    """Check that all generated files in a set exist."""
    all_exist = True
    for gen_file in files:
        if not check_file_exists(gen_file, f"{description}: {gen_file.name}"):
            all_exist = False
    return all_exist


def check_hashes(files: List[Path], current_hash: str, scope_label: str) -> bool:
    """Check if generated files match current metadata via embedded MD5 hash."""
    if not current_hash:
        print(f"✗ ERROR: Cannot compute {scope_label} hash (inputs missing or unreadable)")
        return False

    mismatches = []
    missing_hash = []

    for gen_file in files:
        if not gen_file.exists():
            mismatches.append(gen_file.name)
            continue

        embedded_hash = extract_yaml_hash_from_file(gen_file)
        if not embedded_hash:
            missing_hash.append(gen_file.name)
            continue

        if embedded_hash != current_hash:
            mismatches.append(gen_file.name)

    if missing_hash:
        print(f"✗ WARNING: Some {scope_label} files are missing embedded hash")
        for filename in missing_hash:
            print(f"    - {filename}")
        print()

    if mismatches:
        print(f"✗ OUT OF DATE: {scope_label} changed, generated files need updating")
        print()
        print(f"  Current {scope_label} hash:  {current_hash}")
        print(
            f"  Embedded hash:      {embedded_hash if embedded_hash else '(missing)'}"
        )
        print()
        print("  Files need regeneration:")
        for filename in mismatches:
            print(f"    - {filename}")
        return False

    return True


def check_file_markers(files: List[Path], scope_label: str) -> bool:
    """Check that generated files have the AUTO-GENERATED marker."""

    marker = b"AUTO-GENERATED"
    missing_marker = []

    for gen_file in files:
        if not gen_file.exists():
            continue

        if gen_file.suffix == ".json":
            with open(gen_file, "r", encoding="utf-8") as f:
                data = json.load(f)
            if data.get("_metadata", {}).get("auto_generated") is True:
                continue
            missing_marker.append(gen_file.name)
            continue

        with open(gen_file, "rb") as f:
            first_100_bytes = f.read(100)
            if marker not in first_100_bytes:
                missing_marker.append(gen_file.name)

    if missing_marker:
        print(f"✗ WARNING: {scope_label} files missing AUTO-GENERATED marker")
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
    checks_total = 6

    # Check 1: Property YAML files exist
    print("[1/6] Checking property YAML metadata files...")
    if validate_property_yaml_files():
        print("✓ All property YAML metadata files present")
        checks_passed += 1
    print()

    # Check 2: Module metadata inputs exist
    print("[2/6] Checking module metadata inputs...")
    if validate_module_metadata_inputs():
        print("✓ Module metadata inputs discovered successfully")
        checks_passed += 1
    print()

    # Check 3: Generated files exist
    print("[3/6] Checking generated files...")
    property_files_ok = validate_generated_files(
        PROPERTY_GENERATED_FILES, "Property-generated file"
    )
    module_files_ok = validate_generated_files(
        MODULE_GENERATED_FILES, "Module-generated tracked file"
    )
    if property_files_ok and module_files_ok:
        total_files = len(PROPERTY_GENERATED_FILES) + len(MODULE_GENERATED_FILES)
        print(f"✓ All {total_files} tracked generated files present")
        checks_passed += 1
    print()

    # Check 4: Property hash consistency
    print("[4/6] Checking property metadata hash consistency...")
    if check_hashes(
        PROPERTY_GENERATED_FILES,
        compute_property_yaml_hash(),
        "property metadata",
    ):
        print("✓ Property-generated files match current property metadata")
        checks_passed += 1
    print()

    # Check 5: Module hash consistency
    print("[5/6] Checking module metadata hash consistency...")
    if check_hashes(
        MODULE_GENERATED_FILES,
        compute_module_metadata_hash(),
        "module metadata",
    ):
        print("✓ Tracked module-generated files match current module metadata")
        checks_passed += 1
    print()

    # Check 6: AUTO-GENERATED marker
    print("[6/6] Checking AUTO-GENERATED markers...")
    all_generated_files = PROPERTY_GENERATED_FILES + MODULE_GENERATED_FILES
    if check_file_markers(all_generated_files, "Generated"):
        print("✓ All generated files have proper markers")
        checks_passed += 1
    print()

    # Summary
    print("=" * 70)
    if checks_passed == checks_total:
        print("✓ PASS: All checks passed")
        print("=" * 70)
        print()
        print("Tracked generated code is up-to-date with property and module metadata.")
        return 0
    else:
        print(f"✗ FAIL: {checks_total - checks_passed} check(s) failed")
        print("=" * 70)
        print()
        print("ACTION REQUIRED:")
        print("  Run: make generate")
        print()
        print("This will regenerate tracked code from property and module metadata.")
        print("Then commit the updated generated files to git.")
        print()
        return 1


if __name__ == "__main__":
    sys.exit(main())
