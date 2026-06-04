#!/usr/bin/env python3
"""
Generate test registry from module metadata.

This script scans all module_info.yaml files and generates test manifests
that allow the test runners to automatically discover and run module tests.

Usage:
    python scripts/generate_test_registry.py           # Warn on missing tests
    python scripts/generate_test_registry.py --strict  # Fail on missing tests

Generates:
    build/generated/unit_tests.txt        - Unit test paths
    build/generated/integration_tests.txt - Integration test paths
    build/generated/scientific_tests.txt  - Scientific test paths
    build/generated/test_registry_hash.txt - Validation hash

Author: Mimic Development Team
Date: 2025-11-12
Phase: Phase 4.2 (Test Architecture Refactor)
"""

import argparse
import hashlib
import sys
from pathlib import Path

import yaml

from discovery import REPO_ROOT, live_simulation_roots, module_metadata_files

# ==============================================================================
# COLOR OUTPUT
# ==============================================================================

# ANSI color codes (disabled if not a TTY)
if sys.stdout.isatty():
    COLOR_RED = "\033[91m"
    COLOR_YELLOW = "\033[93m"
    COLOR_RESET = "\033[0m"
else:
    COLOR_RED = ""
    COLOR_YELLOW = ""
    COLOR_RESET = ""


def print_error(msg: str) -> None:
    """Print error message in red."""
    print(f"{COLOR_RED}ERROR: {msg}{COLOR_RESET}", file=sys.stderr)


def print_warning(msg: str) -> None:
    """Print warning message in yellow."""
    print(f"{COLOR_YELLOW}WARNING: {msg}{COLOR_RESET}")


# Track missing tests globally
missing_tests = []


def core_unit_tests(repo_root):
    """Return model-neutral C unit tests under tests/unit."""
    test_dir = repo_root / "tests" / "unit"
    return [
        str(path.relative_to(repo_root))
        for path in sorted(test_dir.glob("test_*.c"))
        if path.name != "test_stubs.c"
    ]


def core_integration_tests(repo_root):
    """Return model-neutral Python integration tests under tests/integration."""
    test_dir = repo_root / "tests" / "integration"
    return [str(path.relative_to(repo_root)) for path in sorted(test_dir.glob("test_*.py"))]


def core_scientific_tests(repo_root):
    """Return model-neutral Python scientific tests under tests/scientific."""
    test_dir = repo_root / "tests" / "scientific"
    return [str(path.relative_to(repo_root)) for path in sorted(test_dir.glob("test_*.py"))]


def simulation_tests(repo_root, test_type, pattern):
    """Return tests owned by the selected simulation package."""
    tests = []
    for simulation_root in live_simulation_roots():
        test_dir = simulation_root / "_tests" / test_type
        if not test_dir.exists():
            continue
        tests.extend(
            str(path.relative_to(repo_root))
            for path in sorted(test_dir.glob(pattern))
        )
    return tests


def process_test_entries(test_value, module_path, repo_root, test_type, module_name):
    """
    Process test entries from module_info.yaml.

    Handles either one test file string or a list of test files.

    Returns list of relative test paths.
    """
    test_paths = []

    if test_value is None:
        # No tests declared (allowed for test fixtures)
        return test_paths

    # Support both string (single test) and list (multiple tests)
    test_files = test_value if isinstance(test_value, list) else [test_value]

    for test_file in test_files:
        test_path = module_path / test_file
        if test_path.exists():
            rel_path = test_path.relative_to(repo_root)
            test_paths.append(str(rel_path))
        else:
            # Collect missing tests for later reporting
            missing_tests.append(
                f"{module_name}: {test_type} test '{test_file}' not found"
            )

    return test_paths


def generate_test_registry(strict: bool = False):
    """Generate test registry from module metadata.

    Args:
        strict: If True, fail on missing tests (for test runs).
                If False, warn only (for normal builds).
    """

    print("Generating test registry...")
    print("=" * 70)

    # Paths
    repo_root = REPO_ROOT
    output_dir = repo_root / "build" / "generated"

    # Create output directory
    output_dir.mkdir(parents=True, exist_ok=True)

    # Collect model-neutral core tests first, then append tests from the
    # selected simulation, model package, and framework test modules.
    unit_tests = core_unit_tests(repo_root)
    integration_tests = core_integration_tests(repo_root)
    scientific_tests = core_scientific_tests(repo_root)

    unit_tests.extend(simulation_tests(repo_root, "unit", "test_*.c"))
    integration_tests.extend(simulation_tests(repo_root, "integration", "test_*.py"))
    scientific_tests.extend(simulation_tests(repo_root, "scientific", "test_*.py"))

    # Track modules for summary
    modules_found = []
    modules_with_tests = []

    # Scan for module metadata from configured roots.
    module_info_files = module_metadata_files()

    for module_info_file in sorted(module_info_files):
        module_path = module_info_file.parent
        module_name = module_path.name

        # Skip template and system directories (but test_fixture from _system is included above)
        if module_name in ["_template", "_system", "_archive", "template", "generated"]:
            continue

        # Load metadata
        try:
            with open(module_info_file) as f:
                metadata = yaml.safe_load(f)
        except (OSError, yaml.YAMLError) as e:
            print_warning(f"Failed to load {module_info_file}: {e}")
            continue

        modules_found.append(module_name)

        # Get test declarations
        tests = metadata.get("module", {}).get("tests", {})

        has_tests = False

        # Process unit tests (supports both string and list formats)
        if "unit" in tests:
            found_tests = process_test_entries(
                tests["unit"], module_path, repo_root, "unit", module_name
            )
            unit_tests.extend(found_tests)
            if found_tests:
                has_tests = True

        # Process integration tests (supports both string and list formats)
        if "integration" in tests:
            found_tests = process_test_entries(
                tests["integration"], module_path, repo_root, "integration", module_name
            )
            integration_tests.extend(found_tests)
            if found_tests:
                has_tests = True

        # Process scientific tests (supports both string and list formats)
        if "scientific" in tests:
            found_tests = process_test_entries(
                tests["scientific"], module_path, repo_root, "scientific", module_name
            )
            scientific_tests.extend(found_tests)
            if found_tests:
                has_tests = True

        if has_tests:
            modules_with_tests.append(module_name)

    # Write test manifests
    unit_tests_file = output_dir / "unit_tests.txt"
    with open(unit_tests_file, "w") as f:
        f.write("# Auto-generated unit test registry\n")
        f.write("# DO NOT EDIT - Generated by scripts/generate_test_registry.py\n")
        f.write(f"# Found {len(unit_tests)} unit test(s)\n\n")
        for test_path in unit_tests:
            f.write(f"{test_path}\n")

    integration_tests_file = output_dir / "integration_tests.txt"
    with open(integration_tests_file, "w") as f:
        f.write("# Auto-generated integration test registry\n")
        f.write("# DO NOT EDIT - Generated by scripts/generate_test_registry.py\n")
        f.write(f"# Found {len(integration_tests)} integration test(s)\n\n")
        for test_path in integration_tests:
            f.write(f"{test_path}\n")

    scientific_tests_file = output_dir / "scientific_tests.txt"
    with open(scientific_tests_file, "w") as f:
        f.write("# Auto-generated scientific test registry\n")
        f.write("# DO NOT EDIT - Generated by scripts/generate_test_registry.py\n")
        f.write(f"# Found {len(scientific_tests)} scientific test(s)\n\n")
        for test_path in scientific_tests:
            f.write(f"{test_path}\n")

    # Generate validation hash
    hash_content = "\n".join(sorted(unit_tests + integration_tests + scientific_tests))
    registry_hash = hashlib.md5(hash_content.encode()).hexdigest()

    hash_file = output_dir / "test_registry_hash.txt"
    with open(hash_file, "w") as f:
        f.write(f"{registry_hash}\n")

    # Print summary
    print()
    print(f"Found {len(modules_found)} module(s):")
    for module in modules_found:
        marker = "✓" if module in modules_with_tests else " "
        print(f"  [{marker}] {module}")

    print()
    print("Test registry summary:")
    print(f"  Unit tests:        {len(unit_tests)}")
    print(f"  Integration tests: {len(integration_tests)}")
    print(f"  Scientific tests:  {len(scientific_tests)}")

    # Report missing tests
    if missing_tests:
        print()
        if strict:
            print_error("Declared tests not found:")
            for msg in missing_tests:
                print(f"  {COLOR_RED}- {msg}{COLOR_RESET}", file=sys.stderr)
            print()
            print("=" * 70)
            print(f"{COLOR_RED}✗ TEST REGISTRY GENERATION FAILED{COLOR_RESET}")
            print("=" * 70)
            return 1
        else:
            print_warning("Some declared tests not found:")
            for msg in missing_tests:
                print(f"  {COLOR_YELLOW}- {msg}{COLOR_RESET}")
            print(
                f"  {COLOR_YELLOW}(Tests may be planned but not yet implemented){COLOR_RESET}"
            )

    print()
    print("Generated files:")
    print(f"  ✓ {unit_tests_file.relative_to(repo_root)}")
    print(f"  ✓ {integration_tests_file.relative_to(repo_root)}")
    print(f"  ✓ {scientific_tests_file.relative_to(repo_root)}")
    print(f"  ✓ {hash_file.relative_to(repo_root)}")

    print()
    print("=" * 70)
    print("✓ TEST REGISTRY GENERATION COMPLETED")
    print("=" * 70)

    return 0


if __name__ == "__main__":
    parser = argparse.ArgumentParser(
        description="Generate test registry from module metadata"
    )
    parser.add_argument(
        "--strict",
        action="store_true",
        help="Fail if declared tests are missing (use for test runs)",
    )
    args = parser.parse_args()

    # Clear global missing_tests list
    missing_tests.clear()

    sys.exit(generate_test_registry(strict=args.strict))
