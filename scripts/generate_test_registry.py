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

"""

import argparse
import hashlib
import sys
from pathlib import Path

import yaml
from console import NC, RED, YELLOW, print_error, print_warning
from discovery import (
    REPO_ROOT,
    full_model_tests_enabled,
    live_simulation_roots,
    module_metadata_files,
)


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
        tests.extend(str(path.relative_to(repo_root)) for path in sorted(test_dir.glob(pattern)))
    return tests


def process_test_entries(test_value, module_path, repo_root, test_type, module_name, missing):
    """
    Process test entries from module_info.yaml.

    Handles either one test file string or a list of test files. Declared
    tests that do not exist are recorded in ``missing`` for later reporting.

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
            missing.append(f"{module_name}: {test_type} test '{test_file}' not found")

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
    tiers = {
        "unit": core_unit_tests(repo_root),
        "integration": core_integration_tests(repo_root),
        "scientific": core_scientific_tests(repo_root),
    }
    tiers["unit"].extend(simulation_tests(repo_root, "unit", "test_*.c"))
    tiers["integration"].extend(simulation_tests(repo_root, "integration", "test_*.py"))
    tiers["scientific"].extend(simulation_tests(repo_root, "scientific", "test_*.py"))

    missing_tests = []

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

        if (
            module_info_file.relative_to(repo_root).parts[0] == "models"
            and not full_model_tests_enabled()
        ):
            continue

        # Get test declarations
        tests = metadata.get("module", {}).get("tests", {})

        has_tests = False

        # Each tier supports both string (single test) and list formats
        for tier, collected in tiers.items():
            if tier in tests:
                found_tests = process_test_entries(
                    tests[tier], module_path, repo_root, tier, module_name, missing_tests
                )
                collected.extend(found_tests)
                if found_tests:
                    has_tests = True

        if has_tests:
            modules_with_tests.append(module_name)

    # Write one manifest per tier
    manifest_files = []
    for tier, collected in tiers.items():
        manifest = output_dir / f"{tier}_tests.txt"
        manifest_files.append(manifest)
        with open(manifest, "w") as f:
            f.write(f"# Auto-generated {tier} test registry\n")
            f.write("# DO NOT EDIT - Generated by scripts/generate_test_registry.py\n")
            f.write(f"# Found {len(collected)} {tier} test(s)\n\n")
            for test_path in collected:
                f.write(f"{test_path}\n")

    # Generate validation hash
    all_tests = [path for collected in tiers.values() for path in collected]
    hash_content = "\n".join(sorted(all_tests))
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
    print(f"  Unit tests:        {len(tiers['unit'])}")
    print(f"  Integration tests: {len(tiers['integration'])}")
    print(f"  Scientific tests:  {len(tiers['scientific'])}")

    # Report missing tests
    if missing_tests:
        print()
        if strict:
            print_error("Declared tests not found:")
            for msg in missing_tests:
                print(f"  {RED}- {msg}{NC}", file=sys.stderr)
            print()
            print("=" * 70)
            print(f"{RED}✗ TEST REGISTRY GENERATION FAILED{NC}")
            print("=" * 70)
            return 1
        else:
            print_warning("Some declared tests not found:")
            for msg in missing_tests:
                print(f"  {YELLOW}- {msg}{NC}")
            print(f"  {YELLOW}(Tests may be planned but not yet implemented){NC}")

    print()
    print("Generated files:")
    for manifest in manifest_files:
        print(f"  ✓ {manifest.relative_to(repo_root)}")
    print(f"  ✓ {hash_file.relative_to(repo_root)}")

    print()
    print("=" * 70)
    print("✓ TEST REGISTRY GENERATION COMPLETED")
    print("=" * 70)

    return 0


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Generate test registry from module metadata")
    parser.add_argument(
        "--strict",
        action="store_true",
        help="Fail if declared tests are missing (use for test runs)",
    )
    args = parser.parse_args()

    sys.exit(generate_test_registry(strict=args.strict))
