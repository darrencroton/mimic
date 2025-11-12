#!/usr/bin/env python3
"""
Module Registry Generator for Mimic

Generates module registration code, test configurations, and documentation from
module_info.yaml metadata files. This eliminates manual synchronization and
implements the metadata-driven module architecture.

Usage:
    python3 scripts/generate_module_registry.py [--dry-run] [--verbose]

Reads:
    src/modules/*/module_info.yaml

Generates:
    src/modules/module_init.c              - Module registration code
    tests/unit/module_sources.mk           - Test build configuration
    docs/user/module-reference.md          - Module documentation
    build/module_registry_hash.txt         - Validation hash

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
from graphlib import TopologicalSorter, CycleError
from pathlib import Path
from typing import Dict, List, Any, Tuple, Optional

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
MODULES_DIR = REPO_ROOT / 'src' / 'modules'

# Output files
MODULE_INIT_C = REPO_ROOT / 'src' / 'modules' / 'module_init.c'
MODULE_SOURCES_MK = REPO_ROOT / 'tests' / 'unit' / 'module_sources.mk'
MODULE_REFERENCE_MD = REPO_ROOT / 'docs' / 'user' / 'module-reference.md'
MODULE_HASH_FILE = REPO_ROOT / 'build' / 'module_registry_hash.txt'

# ==============================================================================
# MODULE DISCOVERY
# ==============================================================================

def load_module_metadata(module_dir: Path) -> Optional[Dict[str, Any]]:
    """Load module_info.yaml from module directory."""
    yaml_path = module_dir / 'module_info.yaml'

    if not yaml_path.exists():
        return None

    try:
        with open(yaml_path, 'r', encoding='utf-8') as f:
            data = yaml.safe_load(f)
            if data is None:
                return None
            module = data.get('module', None)
            if module:
                # Add module directory path for reference
                module['_module_dir'] = module_dir
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

        # Skip template directory
        if item.name.startswith('_'):
            continue

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
    module_names = [m['name'] for m in modules]

    # Build module name -> provides mapping
    provides_map = {}
    for module in modules:
        name = module['name']
        provides = module.get('dependencies', {}).get('provides', [])
        for prop in provides:
            if prop not in provides_map:
                provides_map[prop] = []
            provides_map[prop].append(name)

    # Build dependency edges
    for module in modules:
        name = module['name']
        requires = module.get('dependencies', {}).get('requires', [])
        dependencies = []

        for req_prop in requires:
            if req_prop in provides_map:
                # This module depends on modules that provide req_prop
                for provider in provides_map[req_prop]:
                    if provider != name:  # Don't depend on self
                        dependencies.append(provider)

        graph[name] = list(set(dependencies))  # Remove duplicates

    return graph

def resolve_dependencies(modules: List[Dict[str, Any]]) -> Optional[List[Dict[str, Any]]]:
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
    module_map = {m['name']: m for m in modules}

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
    sorted_modules = sorted(modules, key=lambda m: m['name'])

    for module in sorted_modules:
        module_dir = module.get('_module_dir')
        if module_dir:
            yaml_path = module_dir / 'module_info.yaml'
            if yaml_path.exists():
                with open(yaml_path, 'rb') as f:
                    md5.update(f.read())

    return md5.hexdigest()

# ==============================================================================
# CODE GENERATION - module_init.c
# ==============================================================================

def generate_module_init_c(modules: List[Dict[str, Any]], metadata_hash: str,
                          output_path: Path, dry_run: bool = False) -> bool:
    """Generate module registration code."""

    lines = []

    # Header
    lines.append("/* AUTO-GENERATED FILE - DO NOT EDIT MANUALLY */")
    lines.append("/* Generated from module metadata by scripts/generate_module_registry.py */")
    lines.append("/* Source: src/modules/[MODULE]/module_info.yaml */")
    lines.append("/*")
    lines.append(" * To regenerate:")
    lines.append(" *   make generate-modules")
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
    for module in sorted(modules, key=lambda m: m['name']):
        module_dir = module.get('_module_dir')
        if module_dir:
            rel_path = module_dir.relative_to(MODULES_DIR)
            header_files = module.get('headers', [])
            for header in header_files:
                lines.append(f'#include "{rel_path}/{header}"')
    lines.append("")

    # Registration function
    lines.append("/**")
    lines.append(" * @brief Register all available physics modules")
    lines.append(" *")
    lines.append(f" * Modules registered: {len(modules)}")
    lines.append(" *")
    if modules:
        lines.append(" * Dependency order:")
        for i, module in enumerate(modules, 1):
            name = module['name']
            provides = module.get('dependencies', {}).get('provides', [])
            requires = module.get('dependencies', {}).get('requires', [])

            if provides:
                provides_str = ', '.join(provides)
            else:
                provides_str = "(none)"

            if requires:
                requires_str = ', '.join(requires)
                lines.append(f" * {i}. {name}: requires [{requires_str}] → provides [{provides_str}]")
            else:
                lines.append(f" * {i}. {name}: provides [{provides_str}]")
    lines.append(" */")
    lines.append("void register_all_modules(void) {")

    if modules:
        lines.append("    /* Register in dependency-resolved order */")
        for module in modules:
            name = module['name']
            register_func = module.get('register_function', f"{name}_register")
            provides = module.get('dependencies', {}).get('provides', [])
            requires = module.get('dependencies', {}).get('requires', [])

            # Add inline comment
            if provides and requires:
                comment = f"Requires: {', '.join(requires)} → Provides: {', '.join(provides)}"
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
    content = '\n'.join(lines)

    if dry_run:
        print("=== module_init.c (DRY RUN) ===")
        print(content)
        return True

    try:
        output_path.parent.mkdir(parents=True, exist_ok=True)
        with open(output_path, 'w', encoding='utf-8') as f:
            f.write(content)
        print(f"✓ Generated: {output_path.relative_to(REPO_ROOT)}")
        return True
    except Exception as e:
        print(f"ERROR: Failed to write {output_path}: {e}", file=sys.stderr)
        return False

# ==============================================================================
# CODE GENERATION - module_sources.mk
# ==============================================================================

def generate_module_sources_mk(modules: List[Dict[str, Any]], output_path: Path,
                               dry_run: bool = False) -> bool:
    """Generate makefile fragment for test system."""

    lines = []

    # Header
    lines.append("# AUTO-GENERATED FILE - DO NOT EDIT MANUALLY")
    lines.append("# Generated from module metadata by scripts/generate_module_registry.py")
    lines.append(f"# Generated: {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}")
    lines.append("")

    # Module source files
    lines.append("# Module source files for unit testing")
    lines.append("MODULE_SRCS = \\")
    lines.append("    $(SRC_DIR)/core/module_registry.c \\")

    # Add each module's source files
    for module in modules:
        module_dir = module.get('_module_dir')
        if module_dir:
            rel_path = module_dir.relative_to(REPO_ROOT / 'src')
            sources = module.get('sources', [])
            for source in sources:
                lines.append(f"    $(SRC_DIR)/{rel_path}/{source} \\")

    # Add module_init.c last (no backslash)
    lines.append("    $(SRC_DIR)/modules/module_init.c")
    lines.append("")

    # Write file
    content = '\n'.join(lines)

    if dry_run:
        print("\n=== module_sources.mk (DRY RUN) ===")
        print(content)
        return True

    try:
        output_path.parent.mkdir(parents=True, exist_ok=True)
        with open(output_path, 'w', encoding='utf-8') as f:
            f.write(content)
        print(f"✓ Generated: {output_path.relative_to(REPO_ROOT)}")
        return True
    except Exception as e:
        print(f"ERROR: Failed to write {output_path}: {e}", file=sys.stderr)
        return False

# ==============================================================================
# CODE GENERATION - module-reference.md
# ==============================================================================

def generate_module_reference_md(modules: List[Dict[str, Any]], output_path: Path,
                                dry_run: bool = False) -> bool:
    """Generate module reference documentation."""

    lines = []

    # Header
    lines.append("# Module Reference")
    lines.append("")
    lines.append("Auto-generated from module metadata. "
                f"Last updated: {datetime.now().strftime('%Y-%m-%d')}")
    lines.append("")
    lines.append("---")
    lines.append("")

    # Summary
    lines.append(f"## Available Modules ({len(modules)} modules)")
    lines.append("")

    # Group by category
    categories = {}
    for module in modules:
        category = module.get('category', 'miscellaneous')
        if category not in categories:
            categories[category] = []
        categories[category].append(module)

    # Category display names
    category_names = {
        'gas_physics': 'Gas Physics',
        'star_formation': 'Star Formation',
        'stellar_evolution': 'Stellar Evolution',
        'black_holes': 'Black Holes',
        'mergers': 'Mergers',
        'environment': 'Environment',
        'reionization': 'Reionization',
        'miscellaneous': 'Miscellaneous',
    }

    # Output each category
    for category in sorted(categories.keys()):
        category_modules = categories[category]
        display_name = category_names.get(category, category.title())

        lines.append(f"### {display_name} ({len(category_modules)} modules)")
        lines.append("")

        for module in category_modules:
            name = module['name']
            display_name = module.get('display_name', name)
            description = module.get('description', 'No description')
            version = module.get('version', '1.0.0')
            author = module.get('author', 'Unknown')

            lines.append(f"#### {name} - {display_name}")
            lines.append("")
            lines.append(description)
            lines.append("")
            lines.append(f"**Version**: {version}  ")
            lines.append(f"**Author**: {author}")
            lines.append("")

            # Dependencies
            deps = module.get('dependencies', {})
            requires = deps.get('requires', [])
            provides = deps.get('provides', [])

            lines.append("**Dependencies**:")
            if requires:
                lines.append(f"- Requires: {', '.join(requires)}")
            else:
                lines.append("- Requires: (none)")

            if provides:
                lines.append(f"- Provides: {', '.join(provides)}")
            else:
                lines.append("- Provides: (none)")
            lines.append("")

            # Parameters
            parameters = module.get('parameters', [])
            if parameters:
                lines.append("**Parameters**:")
                for param in parameters:
                    param_name = param['name']
                    param_type = param['type']
                    param_default = param.get('default', 'N/A')
                    param_range = param.get('range', None)
                    param_desc = param.get('description', 'No description')

                    # Format parameter line
                    param_line = f"- `{name}_{param_name}` ({param_type}, default: {param_default}"
                    if param_range:
                        param_line += f", range: {param_range}"
                    param_line += ")"

                    lines.append(param_line)
                    lines.append(f"  {param_desc}")
                lines.append("")

            # References
            references = module.get('references', [])
            if references:
                lines.append("**References**:")
                for ref in references:
                    lines.append(f"- {ref}")
                lines.append("")

            # Documentation link
            docs = module.get('docs', {})
            physics_doc = docs.get('physics')
            if physics_doc:
                lines.append(f"**Physics Documentation**: [{physics_doc}](../{physics_doc})")
                lines.append("")

            lines.append("---")
            lines.append("")

    # Write file
    content = '\n'.join(lines)

    if dry_run:
        print("\n=== module-reference.md (DRY RUN) ===")
        print(content[:500] + "\n... (truncated)")
        return True

    try:
        output_path.parent.mkdir(parents=True, exist_ok=True)
        with open(output_path, 'w', encoding='utf-8') as f:
            f.write(content)
        print(f"✓ Generated: {output_path.relative_to(REPO_ROOT)}")
        return True
    except Exception as e:
        print(f"ERROR: Failed to write {output_path}: {e}", file=sys.stderr)
        return False

# ==============================================================================
# CODE GENERATION - Hash File
# ==============================================================================

def generate_hash_file(metadata_hash: str, output_path: Path,
                      dry_run: bool = False) -> bool:
    """Generate hash file for validation."""

    lines = []
    lines.append("# AUTO-GENERATED - DO NOT EDIT")
    lines.append("# Hash of all module metadata files for validation")
    lines.append(f"# Generated: {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}")
    lines.append("")
    lines.append(f"MODULE_HASH={metadata_hash}")
    lines.append("")

    content = '\n'.join(lines)

    if dry_run:
        print("\n=== module_registry_hash.txt (DRY RUN) ===")
        print(content)
        return True

    try:
        output_path.parent.mkdir(parents=True, exist_ok=True)
        with open(output_path, 'w', encoding='utf-8') as f:
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

    parser = argparse.ArgumentParser(description='Generate module registration code')
    parser.add_argument('--dry-run', action='store_true',
                       help='Print generated code without writing files')
    parser.add_argument('--verbose', '-v', action='store_true',
                       help='Verbose output')
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

    # Generate files
    print("Generating files...")

    success = True
    success &= generate_module_init_c(sorted_modules, metadata_hash,
                                     MODULE_INIT_C, args.dry_run)
    success &= generate_module_sources_mk(sorted_modules, MODULE_SOURCES_MK,
                                         args.dry_run)
    success &= generate_module_reference_md(sorted_modules, MODULE_REFERENCE_MD,
                                           args.dry_run)
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


if __name__ == '__main__':
    sys.exit(main())
