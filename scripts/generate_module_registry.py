#!/usr/bin/env python3
"""
Module Registry Generator for Mimic

Generates module registration code and test configurations from module_info.yaml
metadata files. This eliminates manual synchronization and implements the
metadata-driven module architecture.

Usage:
    python3 scripts/generate_module_registry.py [--dry-run] [--verbose]

Reads:
    src/modules/*/module_info.yaml

Generates:
    src/modules/_system/generated/module_init.c    - Module registration code
    tests/generated/module_sources.mk              - Test build configuration
    build/module_registry_hash.txt                 - Validation hash

Exit codes:
    0 - Success
    1 - Generation failed (validation errors, I/O errors)

Author: Module Metadata System (Phase 4.2.5)
Date: 2025-11-12
"""

import argparse
import hashlib
import sys
from datetime import datetime
from graphlib import CycleError, TopologicalSorter
from pathlib import Path
from typing import Any, Dict, List, Optional, Tuple

try:
    import yaml
except ImportError:
    print("ERROR: PyYAML not installed. Run: pip install PyYAML", file=sys.stderr)
    sys.exit(1)

# ==============================================================================
# PATHS
# ==============================================================================

# Repository root (parent of scripts/)
REPO_ROOT = Path(__file__).parent.parent

# Module directory
MODULES_DIR = REPO_ROOT / "src" / "modules"

# Output files
MODULE_INIT_C = (
    REPO_ROOT / "src" / "modules" / "_system" / "generated" / "module_init.c"
)
MODULE_SOURCES_MK = REPO_ROOT / "tests" / "generated" / "module_sources.mk"
MODULE_HASH_FILE = REPO_ROOT / "build" / "module_registry_hash.txt"

# ==============================================================================
# MODULE DISCOVERY
# ==============================================================================


def load_module_metadata(module_dir: Path) -> Optional[Dict[str, Any]]:
    """Load module_info.yaml from module directory."""
    yaml_path = module_dir / "module_info.yaml"

    if not yaml_path.exists():
        return None

    try:
        with open(yaml_path, "r", encoding="utf-8") as f:
            data = yaml.safe_load(f)
            if data is None:
                return None
            module = data.get("module", None)
            if module:
                # Add module directory path for reference
                module["_module_dir"] = module_dir
            return module
    except yaml.YAMLError as e:
        print(f"ERROR: Failed to parse {yaml_path}: {e}", file=sys.stderr)
        return None


def discover_modules() -> List[Dict[str, Any]]:
    """Discover all modules with module_info.yaml files."""
    modules = []

    if not MODULES_DIR.exists():
        print(f"ERROR: Module directory not found: {MODULES_DIR}", file=sys.stderr)
        return []

    for item in sorted(MODULES_DIR.iterdir()):
        if not item.is_dir():
            continue

        # Skip directories starting with underscore, except _system
        if item.name.startswith("_") and item.name != "_system":
            continue

        # Handle _system directory specially - only include test_fixture
        if item.name == "_system":
            test_fixture_dir = item / "test_fixture"
            if test_fixture_dir.exists() and test_fixture_dir.is_dir():
                metadata = load_module_metadata(test_fixture_dir)
                if metadata:
                    modules.append(metadata)
            continue

        # Regular module directory
        metadata = load_module_metadata(item)
        if metadata:
            modules.append(metadata)

    return modules


# ==============================================================================
# DEPENDENCY RESOLUTION
# ==============================================================================


def build_dependency_graph(modules: List[Dict[str, Any]]) -> Dict[str, List[str]]:
    """Build module dependency graph for topological sort."""

    graph = {}
    module_names = [m["name"] for m in modules]

    # Build module name -> provides mapping
    provides_map = {}
    for module in modules:
        name = module["name"]
        provides = module.get("dependencies", {}).get("provides", [])
        for prop in provides:
            if prop not in provides_map:
                provides_map[prop] = []
            provides_map[prop].append(name)

    # Build dependency edges
    for module in modules:
        name = module["name"]
        requires = module.get("dependencies", {}).get("requires", [])
        dependencies = []

        for req_prop in requires:
            if req_prop in provides_map:
                # This module depends on modules that provide req_prop
                for provider in provides_map[req_prop]:
                    if provider != name:  # Don't depend on self
                        dependencies.append(provider)

        graph[name] = list(set(dependencies))  # Remove duplicates

    return graph


def resolve_dependencies(
    modules: List[Dict[str, Any]],
) -> Optional[List[Dict[str, Any]]]:
    """Topologically sort modules by dependencies."""

    if not modules:
        return []

    try:
        graph = build_dependency_graph(modules)
    except Exception as e:
        print(f"ERROR: Failed to build dependency graph: {e}", file=sys.stderr)
        return None

    # Topological sort
    try:
        ts = TopologicalSorter(graph)
        sorted_names = list(ts.static_order())
    except CycleError as e:
        print(f"ERROR: Circular dependency detected: {e}", file=sys.stderr)
        return None

    # Create name -> module mapping
    module_map = {m["name"]: m for m in modules}

    # Return modules in dependency order
    sorted_modules = []
    for name in sorted_names:
        if name in module_map:
            sorted_modules.append(module_map[name])

    return sorted_modules


# ==============================================================================
# HASH COMPUTATION
# ==============================================================================


def compute_metadata_hash(modules: List[Dict[str, Any]]) -> str:
    """Compute MD5 hash of all module metadata files."""
    md5 = hashlib.md5()

    # Sort by module name for consistent ordering
    sorted_modules = sorted(modules, key=lambda m: m["name"])

    for module in sorted_modules:
        module_dir = module.get("_module_dir")
        if module_dir:
            yaml_path = module_dir / "module_info.yaml"
            if yaml_path.exists():
                with open(yaml_path, "rb") as f:
                    md5.update(f.read())

    return md5.hexdigest()


def load_saved_hash() -> str:
    """Load the previously saved module metadata hash.

    Returns:
        The saved hash, or empty string if not found.
    """
    if MODULE_HASH_FILE.exists():
        try:
            with open(MODULE_HASH_FILE, "r", encoding="utf-8") as f:
                for line in f:
                    if line.startswith("MODULE_HASH="):
                        return line.split("=", 1)[1].strip()
        except Exception:
            pass
    return ""


# ==============================================================================
# CODE GENERATION - module_init.c
# ==============================================================================


def generate_module_init_c(
    modules: List[Dict[str, Any]],
    metadata_hash: str,
    output_path: Path,
    dry_run: bool = False,
) -> bool:
    """Generate module registration code."""

    # Filter out utilities (is_utility: true) - they're test-only, not runtime modules
    runtime_modules = [m for m in modules if not m.get("is_utility", False)]

    lines = []

    # Header
    lines.append("/* AUTO-GENERATED FILE - DO NOT EDIT MANUALLY */")
    lines.append(
        "/* Generated from module metadata by scripts/generate_module_registry.py */"
    )
    lines.append("/* Source: src/modules/[MODULE]/module_info.yaml */")
    lines.append("/*")
    lines.append(" * To regenerate:")
    lines.append(" *   make generate")
    lines.append(" *")
    lines.append(" * To validate:")
    lines.append(" *   make validate-modules")
    lines.append(" *")
    lines.append(f" * Generated: {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}")
    lines.append(f" * Source MD5: {metadata_hash}")
    lines.append(" */")
    lines.append("")

    # Includes
    lines.append('#include "module_registry.h"')
    lines.append("")

    # Auto-generated module includes (sorted alphabetically)
    lines.append("/* Auto-generated module includes (sorted alphabetically) */")
    for module in sorted(runtime_modules, key=lambda m: m["name"]):
        module_dir = module.get("_module_dir")
        if module_dir:
            rel_path = module_dir.relative_to(MODULES_DIR)
            header_files = module.get("headers", [])
            for header in header_files:
                lines.append(f'#include "{rel_path}/{header}"')
    lines.append("")

    # Registration function
    lines.append("/**")
    lines.append(" * @brief Register all available physics modules")
    lines.append(" *")
    lines.append(f" * Modules registered: {len(runtime_modules)}")
    lines.append(" *")
    if runtime_modules:
        lines.append(" * Dependency order:")
        for i, module in enumerate(runtime_modules, 1):
            name = module["name"]
            provides = module.get("dependencies", {}).get("provides", [])
            requires = module.get("dependencies", {}).get("requires", [])

            if provides:
                provides_str = ", ".join(provides)
            else:
                provides_str = "(none)"

            if requires:
                requires_str = ", ".join(requires)
                lines.append(
                    f" * {i}. {name}: requires [{requires_str}] → provides [{provides_str}]"
                )
            else:
                lines.append(f" * {i}. {name}: provides [{provides_str}]")
    lines.append(" */")
    lines.append("void register_all_modules(void) {")

    if runtime_modules:
        lines.append("    /* Register in dependency-resolved order */")
        for module in runtime_modules:
            name = module["name"]
            register_func = module.get("register_function", f"{name}_register")
            provides = module.get("dependencies", {}).get("provides", [])
            requires = module.get("dependencies", {}).get("requires", [])

            # Add inline comment
            if provides and requires:
                comment = (
                    f"Requires: {', '.join(requires)} → Provides: {', '.join(provides)}"
                )
            elif provides:
                comment = f"Provides: {', '.join(provides)}"
            elif requires:
                comment = f"Requires: {', '.join(requires)}"
            else:
                comment = "No dependencies"

            lines.append(f"    {register_func}();  /* {comment} */")
    else:
        lines.append("    /* No modules to register */")

    lines.append("}")
    lines.append("")

    # Write file
    content = "\n".join(lines)

    if dry_run:
        print("=== module_init.c (DRY RUN) ===")
        print(content)
        return True

    try:
        output_path.parent.mkdir(parents=True, exist_ok=True)
        with open(output_path, "w", encoding="utf-8") as f:
            f.write(content)
        print(f"✓ Generated: {output_path.relative_to(REPO_ROOT)}")
        return True
    except Exception as e:
        print(f"ERROR: Failed to write {output_path}: {e}", file=sys.stderr)
        return False


# ==============================================================================
# CODE GENERATION - module_sources.mk
# ==============================================================================


def generate_module_sources_mk(
    modules: List[Dict[str, Any]], output_path: Path, dry_run: bool = False
) -> bool:
    """Generate makefile fragment for test system."""

    # Filter out utilities (is_utility: true) - they have no runtime source files
    runtime_modules = [m for m in modules if not m.get("is_utility", False)]

    lines = []

    # Header
    lines.append("# AUTO-GENERATED FILE - DO NOT EDIT MANUALLY")
    lines.append(
        "# Generated from module metadata by scripts/generate_module_registry.py"
    )
    lines.append(f"# Generated: {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}")
    lines.append("")

    # Module source files
    lines.append("# Module source files for unit testing")
    lines.append("MODULE_SRCS = \\")
    lines.append("    $(SRC_DIR)/core/module_registry.c \\")

    # Add each module's source files
    for module in runtime_modules:
        module_dir = module.get("_module_dir")
        if module_dir:
            rel_path = module_dir.relative_to(REPO_ROOT / "src")
            sources = module.get("sources", [])
            for source in sources:
                lines.append(f"    $(SRC_DIR)/{rel_path}/{source} \\")

    # Add module_init.c last (no backslash)
    lines.append("    $(SRC_DIR)/modules/_system/generated/module_init.c")
    lines.append("")

    # Write file
    content = "\n".join(lines)

    if dry_run:
        print("\n=== module_sources.mk (DRY RUN) ===")
        print(content)
        return True

    try:
        output_path.parent.mkdir(parents=True, exist_ok=True)
        with open(output_path, "w", encoding="utf-8") as f:
            f.write(content)
        print(f"✓ Generated: {output_path.relative_to(REPO_ROOT)}")
        return True
    except Exception as e:
        print(f"ERROR: Failed to write {output_path}: {e}", file=sys.stderr)
        return False


# ==============================================================================
# CODE GENERATION - Hash File
# ==============================================================================


def generate_hash_file(
    metadata_hash: str, output_path: Path, dry_run: bool = False
) -> bool:
    """Generate hash file for validation."""

    lines = []
    lines.append("# AUTO-GENERATED - DO NOT EDIT")
    lines.append("# Hash of all module metadata files for validation")
    lines.append(f"# Generated: {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}")
    lines.append("")
    lines.append(f"MODULE_HASH={metadata_hash}")
    lines.append("")

    content = "\n".join(lines)

    if dry_run:
        print("\n=== module_registry_hash.txt (DRY RUN) ===")
        print(content)
        return True

    try:
        output_path.parent.mkdir(parents=True, exist_ok=True)
        with open(output_path, "w", encoding="utf-8") as f:
            f.write(content)
        print(f"✓ Generated: {output_path.relative_to(REPO_ROOT)}")
        return True
    except Exception as e:
        print(f"ERROR: Failed to write {output_path}: {e}", file=sys.stderr)
        return False


# ==============================================================================
# MAIN
# ==============================================================================


def main():
    """Main entry point."""

    parser = argparse.ArgumentParser(description="Generate module registration code")
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="Print generated code without writing files",
    )
    parser.add_argument("--verbose", "-v", action="store_true", help="Verbose output")
    args = parser.parse_args()

    print("=" * 70)
    print("Module Registry Generation")
    print("=" * 70)
    print()

    # Discover modules
    modules = discover_modules()

    if not modules:
        print("WARNING: No modules found with module_info.yaml files")
        print("         Empty registration code will be generated")
        # Continue with empty list to generate valid (but empty) code

    if modules:
        print(f"Found {len(modules)} module(s):")
        for module in modules:
            print(f"  - {module['name']} ({module.get('version', '1.0.0')})")
        print()

    # Resolve dependencies
    if modules:
        print("Resolving dependencies...")
        sorted_modules = resolve_dependencies(modules)
        if sorted_modules is None:
            print("ERROR: Dependency resolution failed", file=sys.stderr)
            return 1

        if args.verbose:
            print("Dependency order:")
            for i, module in enumerate(sorted_modules, 1):
                print(f"  {i}. {module['name']}")
        print()
    else:
        sorted_modules = []

    # Compute metadata hash
    metadata_hash = compute_metadata_hash(modules)
    if args.verbose:
        print(f"Metadata hash: {metadata_hash}")
        print()

    # Check if regeneration is needed (skip in dry-run mode)
    if not args.dry_run:
        saved_hash = load_saved_hash()
        if saved_hash == metadata_hash:
            print("✓ Module metadata unchanged - skipping regeneration")
            print(f"  Hash: {metadata_hash}")
            print()
            print("=" * 70)
            print("✓ SKIPPED (no changes)")
            print("=" * 70)
            return 0

        # Hash mismatch or missing - regeneration needed
        if saved_hash:
            print(f"Module metadata changed - regenerating...")
            print(f"  Old hash: {saved_hash}")
            print(f"  New hash: {metadata_hash}")
        else:
            print("No previous hash found - generating for first time...")
            print(f"  Hash: {metadata_hash}")
        print()

    # Generate files
    print("Generating files...")

    success = True
    success &= generate_module_init_c(
        sorted_modules, metadata_hash, MODULE_INIT_C, args.dry_run
    )
    success &= generate_module_sources_mk(
        sorted_modules, MODULE_SOURCES_MK, args.dry_run
    )
    success &= generate_hash_file(metadata_hash, MODULE_HASH_FILE, args.dry_run)

    print()
    print("=" * 70)
    if success:
        if args.dry_run:
            print("✓ DRY RUN COMPLETED")
        else:
            print("✓ GENERATION COMPLETED")
        print("=" * 70)
        return 0
    else:
        print("✗ GENERATION FAILED")
        print("=" * 70)
        return 1


if __name__ == "__main__":
    sys.exit(main())
