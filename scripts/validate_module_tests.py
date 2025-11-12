#!/usr/bin/env python3
"""
Validate module test declarations.

This script checks that all tests declared in module_info.yaml files
actually exist in the module directories.

Usage:
    python scripts/validate_module_tests.py

Exit codes:
    0 - All declared tests exist
    1 - One or more declared tests missing

Author: Mimic Development Team
Date: 2025-11-12
Phase: Phase 4.2 (Test Architecture Refactor)
"""

import sys
import yaml
from pathlib import Path


def validate_module_tests():
    """Validate that declared module tests exist."""

    print("Validating module test declarations...")
    print("=" * 70)

    # Paths
    repo_root = Path(__file__).parent.parent
    module_dir = repo_root / "src" / "modules"

    # Track validation results
    all_valid = True
    modules_checked = 0
    tests_validated = 0
    missing_tests = []

    # Scan for module metadata
    for module_info_file in sorted(module_dir.glob("*/module_info.yaml")):
        module_path = module_info_file.parent
        module_name = module_path.name

        # Skip template module
        if module_name == "_template":
            continue

        # Load metadata
        try:
            with open(module_info_file) as f:
                metadata = yaml.safe_load(f)
        except Exception as e:
            print(f"ERROR: Failed to load {module_info_file}: {e}")
            all_valid = False
            continue

        modules_checked += 1

        # Get test declarations
        tests = metadata.get('module', {}).get('tests', {})

        # Validate each test type
        for test_type in ['unit', 'integration', 'scientific']:
            if test_type in tests:
                test_file = tests[test_type]
                test_path = module_path / test_file

                if test_path.exists():
                    print(f"✓ {module_name:20s} {test_type:12s} test: {test_file}")
                    tests_validated += 1
                else:
                    print(f"✗ {module_name:20s} {test_type:12s} test: {test_file} NOT FOUND")
                    missing_tests.append({
                        'module': module_name,
                        'type': test_type,
                        'file': test_file,
                        'expected_path': test_path
                    })
                    all_valid = False

    # Print summary
    print()
    print("=" * 70)
    print("Validation Summary")
    print("=" * 70)
    print(f"Modules checked:      {modules_checked}")
    print(f"Tests validated:      {tests_validated}")
    print(f"Missing tests:        {len(missing_tests)}")

    if missing_tests:
        print()
        print("Missing test files:")
        for missing in missing_tests:
            print(f"  • {missing['module']}/{missing['file']}")
            print(f"    Expected at: {missing['expected_path']}")

    print()
    if all_valid:
        print("✓ ALL MODULE TESTS VALIDATED")
        return 0
    else:
        print("✗ VALIDATION FAILED - Missing test files")
        return 1


if __name__ == "__main__":
    sys.exit(validate_module_tests())
