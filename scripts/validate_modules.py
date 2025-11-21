#!/usr/bin/env python3
"""
Module Metadata Validator for Mimic

Validates module_info.yaml files against the schema and performs comprehensive
consistency checks. Used to catch errors before code generation.

Usage:
    python3 scripts/validate_modules.py                    # Validate all modules
    python3 scripts/validate_modules.py path/to/module/    # Validate specific module
    python3 scripts/validate_modules.py --verbose          # Verbose output

Exit codes:
    0 - All validations passed
    1 - Schema error (missing fields, wrong types)
    2 - File not found (source, header, test, doc)
    3 - Dependency error (circular, unresolved)
    4 - Naming convention violation
    5 - Parameter validation error
    6 - Code verification error (register function not found)

Author: Module Metadata System (Phase 4.2.5)
Date: 2025-11-12
"""

import argparse
import re
import sys
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

# Property metadata files (for dependency validation)
GALAXY_PROPERTIES_YAML = REPO_ROOT / "src" / "modules" / "galaxy_properties.yaml"
HALO_PROPERTIES_YAML = REPO_ROOT / "src" / "core" / "halo_properties.yaml"

# ==============================================================================
# SCHEMA DEFINITIONS
# ==============================================================================

VALID_PARAMETER_TYPES = ["double", "int", "string"]

VALID_COMPILATION_FEATURES = ["HDF5", "MPI", "GSL"]

# Semantic versioning pattern
VERSION_PATTERN = re.compile(r"^\d+\.\d+\.\d+$")

# C identifier pattern
C_IDENTIFIER_PATTERN = re.compile(r"^[a-zA-Z_][a-zA-Z0-9_]*$")

# ==============================================================================
# ERROR TRACKING
# ==============================================================================


class ValidationError:
    """Track validation errors with severity and exit code."""

    def __init__(self, module_name: str, severity: str, exit_code: int, message: str):
        self.module_name = module_name
        self.severity = severity  # 'ERROR', 'WARNING'
        self.exit_code = exit_code
        self.message = message

    def __str__(self):
        return f"[{self.severity}] {self.module_name}: {self.message}"


class ValidationResults:
    """Collect and report validation results."""

    def __init__(self):
        self.errors: List[ValidationError] = []
        self.warnings: List[ValidationError] = []

    def add_error(self, module_name: str, exit_code: int, message: str):
        """Add an error (validation failure)."""
        self.errors.append(ValidationError(module_name, "ERROR", exit_code, message))

    def add_warning(self, module_name: str, message: str):
        """Add a warning (non-critical issue)."""
        self.warnings.append(ValidationError(module_name, "WARNING", 0, message))

    def has_errors(self) -> bool:
        """Check if any errors were recorded."""
        return len(self.errors) > 0

    def get_exit_code(self) -> int:
        """Get appropriate exit code (first error's code, or 0 if no errors)."""
        if self.errors:
            return self.errors[0].exit_code
        return 0

    def print_summary(self):
        """Print validation summary."""
        if self.warnings:
            print("\n" + "=" * 70)
            print(f"WARNINGS ({len(self.warnings)})")
            print("=" * 70)
            for warning in self.warnings:
                print(f"  {warning}")

        if self.errors:
            print("\n" + "=" * 70)
            print(f"ERRORS ({len(self.errors)})")
            print("=" * 70)
            for error in self.errors:
                print(f"  {error}")
            print("\n" + "=" * 70)
            print(f"✗ VALIDATION FAILED - {len(self.errors)} error(s) found")
            print("=" * 70)
        else:
            print("\n" + "=" * 70)
            print("✓ VALIDATION PASSED")
            print("=" * 70)


# ==============================================================================
# YAML LOADING
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
            return data.get("module", None)
    except yaml.YAMLError as e:
        print(f"ERROR: Failed to parse {yaml_path}: {e}", file=sys.stderr)
        return None


def discover_modules() -> List[Tuple[Path, Optional[Dict[str, Any]]]]:
    """Discover all modules in src/modules/ directory."""
    modules = []

    if not MODULES_DIR.exists():
        print(f"ERROR: Module directory not found: {MODULES_DIR}", file=sys.stderr)
        return []

    for item in sorted(MODULES_DIR.iterdir()):
        if not item.is_dir():
            continue

        # Skip template directory
        if item.name.startswith("_"):
            continue

        metadata = load_module_metadata(item)
        modules.append((item, metadata))

    return modules


def load_property_metadata() -> Dict[str, Dict[str, Any]]:
    """
    Load both galaxy and halo property metadata.

    Returns:
        Dict mapping property name to full metadata:
        {
            "ColdGas": {
                "name": "ColdGas",
                "type": "float",
                "units": "1e10 Msun/h",
                "description": "...",
                "source": "galaxy_properties.yaml"
            },
            "Mvir": {
                "name": "Mvir",
                "type": "float",
                "units": "1e10 Msun/h",
                "description": "...",
                "source": "halo_properties.yaml"
            },
            ...
        }
    """
    properties = {}

    # Load galaxy properties
    if GALAXY_PROPERTIES_YAML.exists():
        try:
            with open(GALAXY_PROPERTIES_YAML, "r", encoding="utf-8") as f:
                data = yaml.safe_load(f)
                if data and "galaxy_properties" in data:
                    for prop in data["galaxy_properties"]:
                        if "name" in prop:
                            properties[prop["name"]] = {
                                **prop,
                                "source": "galaxy_properties.yaml",
                            }
        except Exception as e:
            print(f"WARNING: Failed to load galaxy properties: {e}", file=sys.stderr)

    # Load halo properties
    if HALO_PROPERTIES_YAML.exists():
        try:
            with open(HALO_PROPERTIES_YAML, "r", encoding="utf-8") as f:
                data = yaml.safe_load(f)
                if data and "halo_properties" in data:
                    for prop in data["halo_properties"]:
                        if "name" in prop:
                            properties[prop["name"]] = {
                                **prop,
                                "source": "halo_properties.yaml",
                            }
        except Exception as e:
            print(f"WARNING: Failed to load halo properties: {e}", file=sys.stderr)

    return properties


# ==============================================================================
# SCHEMA VALIDATION
# ==============================================================================


def validate_required_fields(
    module: Dict[str, Any], module_name: str, results: ValidationResults
) -> bool:
    """Validate that all required fields are present."""

    required_core = [
        "name",
        "display_name",
        "description",
        "version",
        "author",
    ]
    required_sources = ["sources", "headers", "register_function"]
    required_deps = ["dependencies"]
    required_params = ["parameters"]

    all_required = required_core + required_sources + required_deps + required_params

    missing = [field for field in all_required if field not in module]

    if missing:
        results.add_error(
            module_name, 1, f"Missing required fields: {', '.join(missing)}"
        )
        return False

    # Check dependencies subfields
    deps = module.get("dependencies", {})
    if "requires" not in deps or "provides" not in deps:
        results.add_error(
            module_name,
            1,
            "dependencies must have both 'requires' and 'provides' fields",
        )
        return False

    return True


def validate_field_types(
    module: Dict[str, Any], module_name: str, results: ValidationResults
) -> bool:
    """Validate field types match schema."""

    valid = True

    # String fields
    for field in [
        "name",
        "display_name",
        "description",
        "version",
        "author",
        "register_function",
    ]:
        if field in module and not isinstance(module[field], str):
            results.add_error(module_name, 1, f"Field '{field}' must be a string")
            valid = False

    # List fields
    for field in ["sources", "headers"]:
        if field in module:
            if not isinstance(module[field], list):
                results.add_error(module_name, 1, f"Field '{field}' must be a list")
                valid = False
            elif not all(isinstance(item, str) for item in module[field]):
                results.add_error(
                    module_name, 1, f"All items in '{field}' must be strings"
                )
                valid = False

    # Dependencies
    if "dependencies" in module:
        deps = module["dependencies"]
        if not isinstance(deps, dict):
            results.add_error(module_name, 1, "Field 'dependencies' must be a dict")
            valid = False
        else:
            for field in ["requires", "provides"]:
                if field in deps:
                    if not isinstance(deps[field], list):
                        results.add_error(
                            module_name, 1, f"dependencies.{field} must be a list"
                        )
                        valid = False
                    elif not all(isinstance(item, str) for item in deps[field]):
                        results.add_error(
                            module_name,
                            1,
                            f"All items in dependencies.{field} must be strings",
                        )
                        valid = False

    # Parameters
    if "parameters" in module:
        params = module["parameters"]
        if not isinstance(params, list):
            results.add_error(module_name, 1, "Field 'parameters' must be a list")
            valid = False
        elif params:  # If not empty, check structure
            for i, param in enumerate(params):
                if not isinstance(param, dict):
                    results.add_error(module_name, 1, f"parameters[{i}] must be a dict")
                    valid = False
                else:
                    # Check required parameter fields
                    required_param_fields = ["name", "type", "default", "description"]
                    missing = [f for f in required_param_fields if f not in param]
                    if missing:
                        results.add_error(
                            module_name,
                            1,
                            f"parameters[{i}] missing fields: {', '.join(missing)}",
                        )
                        valid = False

    # Optional boolean fields
    if "default_enabled" in module and not isinstance(module["default_enabled"], bool):
        results.add_error(module_name, 1, "Field 'default_enabled' must be a boolean")
        valid = False

    return valid


def validate_version(
    module: Dict[str, Any], module_name: str, results: ValidationResults
) -> bool:
    """Validate version follows semantic versioning."""

    version = module.get("version", "")
    if not VERSION_PATTERN.match(version):
        results.add_error(
            module_name,
            1,
            f"Invalid version '{version}'. Must follow semantic versioning (e.g., '1.0.0')",
        )
        return False

    return True


def validate_name(
    module: Dict[str, Any],
    module_name: str,
    module_dir: Path,
    results: ValidationResults,
) -> bool:
    """Validate module name is valid C identifier and matches directory."""

    name = module.get("name", "")

    # Check C identifier
    if not C_IDENTIFIER_PATTERN.match(name):
        results.add_error(
            module_name, 4, f"Module name '{name}' is not a valid C identifier"
        )
        return False

    # Check lowercase with underscores convention
    if not name.islower() or not all(c.isalnum() or c == "_" for c in name):
        results.add_warning(
            module_name, f"Module name '{name}' should be lowercase_with_underscores"
        )

    # Check matches directory name
    if name != module_dir.name:
        results.add_error(
            module_name,
            4,
            f"Module name '{name}' doesn't match directory name '{module_dir.name}'",
        )
        return False

    return True


def validate_register_function(
    module: Dict[str, Any], module_name: str, results: ValidationResults
) -> bool:
    """Validate register function follows naming convention."""

    name = module.get("name", "")
    register_func = module.get("register_function", "")

    expected = f"{name}_register"
    if register_func != expected:
        results.add_error(
            module_name,
            4,
            f"Register function '{register_func}' should be '{expected}'",
        )
        return False

    return True


def validate_parameters(
    module: Dict[str, Any], module_name: str, results: ValidationResults
) -> bool:
    """Validate parameter definitions."""

    params = module.get("parameters", [])
    if not params:
        return True  # Empty list is valid

    valid = True
    param_names = []

    for i, param in enumerate(params):
        param_name = param.get("name", f"<parameter {i}>")

        # Check type
        param_type = param.get("type", "")
        if param_type not in VALID_PARAMETER_TYPES:
            results.add_error(
                module_name,
                5,
                f"Parameter '{param_name}' has invalid type '{param_type}'. "
                f"Must be one of: {', '.join(VALID_PARAMETER_TYPES)}",
            )
            valid = False

        # Check default value type matches
        default = param.get("default")
        if default is not None:
            if param_type == "int" and not isinstance(default, int):
                results.add_error(
                    module_name,
                    5,
                    f"Parameter '{param_name}' type is int but default is {type(default).__name__}",
                )
                valid = False
            elif param_type == "double" and not isinstance(default, (int, float)):
                results.add_error(
                    module_name,
                    5,
                    f"Parameter '{param_name}' type is double but default is {type(default).__name__}",
                )
                valid = False
            elif param_type == "string" and not isinstance(default, str):
                results.add_error(
                    module_name,
                    5,
                    f"Parameter '{param_name}' type is string but default is {type(default).__name__}",
                )
                valid = False

        # Check range for numeric types
        if "range" in param:
            if param_type not in ["int", "double"]:
                results.add_warning(
                    module_name,
                    f"Parameter '{param_name}' has range but type is '{param_type}'",
                )
            else:
                range_val = param["range"]
                if not isinstance(range_val, list) or len(range_val) != 2:
                    results.add_error(
                        module_name,
                        5,
                        f"Parameter '{param_name}' range must be [min, max]",
                    )
                    valid = False
                elif range_val[0] > range_val[1]:
                    results.add_error(
                        module_name, 5, f"Parameter '{param_name}' range min > max"
                    )
                    valid = False

        # Check for duplicates
        if param_name in param_names:
            results.add_error(
                module_name, 5, f"Duplicate parameter name '{param_name}'"
            )
            valid = False
        param_names.append(param_name)

        # Validate naming convention (PascalCase recommended)
        if not C_IDENTIFIER_PATTERN.match(param_name):
            results.add_error(
                module_name,
                4,
                f"Parameter name '{param_name}' is not a valid C identifier",
            )
            valid = False

    return valid


def validate_compilation_requires(
    module: Dict[str, Any], module_name: str, results: ValidationResults
) -> bool:
    """Validate compilation requirements are recognized features."""

    if "compilation_requires" not in module:
        return True

    reqs = module["compilation_requires"]
    if not isinstance(reqs, list):
        results.add_error(module_name, 1, "compilation_requires must be a list")
        return False

    invalid = [req for req in reqs if req not in VALID_COMPILATION_FEATURES]
    if invalid:
        results.add_error(
            module_name,
            1,
            f"Invalid compilation requirements: {', '.join(invalid)}. "
            f"Must be one of: {', '.join(VALID_COMPILATION_FEATURES)}",
        )
        return False

    return True


# ==============================================================================
# FILE EXISTENCE VALIDATION
# ==============================================================================


def validate_source_files(
    module: Dict[str, Any],
    module_name: str,
    module_dir: Path,
    results: ValidationResults,
) -> bool:
    """Validate that all source files exist."""

    valid = True

    for source in module.get("sources", []):
        source_path = module_dir / source
        if not source_path.exists():
            results.add_error(module_name, 2, f"Source file not found: {source}")
            valid = False

    for header in module.get("headers", []):
        header_path = module_dir / header
        if not header_path.exists():
            results.add_error(module_name, 2, f"Header file not found: {header}")
            valid = False

    return valid


def validate_test_files(
    module: Dict[str, Any],
    module_name: str,
    module_dir: Path,
    results: ValidationResults,
) -> bool:
    """Validate that test files exist (warnings only)."""

    if "tests" not in module:
        results.add_warning(module_name, "No test files specified")
        return True

    tests = module["tests"]

    # Unit tests can be in tests/unit/ OR co-located with module
    if "unit" in tests:
        unit_test = tests["unit"]
        # Handle list format (e.g., shared utilities with multiple tests)
        if isinstance(unit_test, list):
            for test_file in unit_test:
                # Check module directory first (co-located)
                module_test_path = module_dir / test_file
                # Check tests/unit/ directory second (centralized)
                central_test_path = REPO_ROOT / "tests" / "unit" / test_file
                if not module_test_path.exists() and not central_test_path.exists():
                    results.add_warning(
                        module_name, f"Unit test file not found: {test_file}"
                    )
        else:
            # Check module directory first (co-located)
            module_test_path = module_dir / unit_test
            # Check tests/unit/ directory second (centralized)
            central_test_path = REPO_ROOT / "tests" / "unit" / unit_test
            if not module_test_path.exists() and not central_test_path.exists():
                results.add_warning(
                    module_name, f"Unit test file not found: {unit_test}"
                )

    # Integration tests are co-located with module
    if "integration" in tests:
        integration_test = tests["integration"]
        # Handle list format
        if isinstance(integration_test, list):
            for test_file in integration_test:
                int_test_path = module_dir / test_file
                if not int_test_path.exists():
                    results.add_warning(
                        module_name, f"Integration test file not found: {test_file}"
                    )
        else:
            int_test_path = module_dir / integration_test
            if not int_test_path.exists():
                results.add_warning(
                    module_name, f"Integration test file not found: {integration_test}"
                )

    # Scientific tests are co-located with module
    if "scientific" in tests:
        scientific_test = tests["scientific"]
        # Handle list format
        if isinstance(scientific_test, list):
            for test_file in scientific_test:
                sci_test_path = module_dir / test_file
                if not sci_test_path.exists():
                    results.add_warning(
                        module_name, f"Scientific test file not found: {test_file}"
                    )
        else:
            sci_test_path = module_dir / scientific_test
            if not sci_test_path.exists():
                results.add_warning(
                    module_name, f"Scientific test file not found: {scientific_test}"
                )

    return True


def validate_doc_files(
    module: Dict[str, Any], module_name: str, results: ValidationResults
) -> bool:
    """Validate that documentation files exist (warnings only)."""

    if "docs" not in module:
        results.add_warning(module_name, "No documentation specified")
        return True

    docs = module["docs"]

    if "physics" in docs:
        physics_doc_path = REPO_ROOT / docs["physics"]
        if not physics_doc_path.exists():
            results.add_warning(
                module_name, f"Physics documentation not found: {docs['physics']}"
            )

    return True


# ==============================================================================
# CODE VERIFICATION
# ==============================================================================


def validate_register_function_exists(
    module: Dict[str, Any],
    module_name: str,
    module_dir: Path,
    results: ValidationResults,
) -> bool:
    """Verify register function exists in source code."""

    register_func = module.get("register_function", "")
    if not register_func:
        return False

    # Search all source files for function definition
    found = False

    for source in module.get("sources", []):
        source_path = module_dir / source
        if not source_path.exists():
            continue

        try:
            with open(source_path, "r", encoding="utf-8") as f:
                content = f.read()
                # Look for function definition (basic pattern match)
                pattern = rf"\bvoid\s+{re.escape(register_func)}\s*\("
                if re.search(pattern, content):
                    found = True
                    break
        except Exception:
            pass

    if not found:
        results.add_error(
            module_name,
            6,
            f"Register function '{register_func}' not found in source files",
        )
        return False

    return True


# ==============================================================================
# DEPENDENCY VALIDATION
# ==============================================================================


def build_dependency_graph(
    modules: List[Tuple[str, Dict[str, Any]]],
) -> Dict[str, List[str]]:
    """Build module dependency graph for topological sort."""

    graph = {}

    # Build module name -> provides mapping
    provides_map = {}
    for name, module in modules:
        provides = module.get("dependencies", {}).get("provides", [])
        for prop in provides:
            if prop not in provides_map:
                provides_map[prop] = []
            provides_map[prop].append(name)

    # Build dependency edges
    for name, module in modules:
        requires = module.get("dependencies", {}).get("requires", [])
        dependencies = []

        for req_prop in requires:
            if req_prop in provides_map:
                # This module depends on modules that provide req_prop
                dependencies.extend(provides_map[req_prop])

        graph[name] = dependencies

    return graph


def validate_module_dependencies(
    module: Dict[str, Any],
    module_name: str,
    property_metadata: Dict[str, Dict[str, Any]],
    results: ValidationResults,
    verbose: bool = False,
) -> bool:
    """Validate module dependencies against property metadata."""

    deps = module.get("dependencies", {})
    requires = deps.get("requires", [])
    provides = deps.get("provides", [])

    valid = True

    # Validate required properties
    for req in requires:
        if req not in property_metadata:
            results.add_error(
                module_name,
                3,
                f"Required property '{req}' not found in property metadata. "
                f"Check galaxy_properties.yaml and halo_properties.yaml.",
            )
            valid = False
        elif verbose:
            prop_meta = property_metadata[req]
            print(
                f"  {module_name} requires {req}: "
                f"type={prop_meta.get('type', 'unknown')}, "
                f"units={prop_meta.get('units', 'unknown')}, "
                f"source={prop_meta.get('source', 'unknown')}"
            )

    # Validate provided properties
    for prov in provides:
        if prov not in property_metadata:
            results.add_error(
                module_name,
                3,
                f"Provided property '{prov}' not found in property metadata. "
                f"Add to galaxy_properties.yaml or check spelling.",
            )
            valid = False
        elif verbose:
            prop_meta = property_metadata[prov]
            print(
                f"  {module_name} provides {prov}: "
                f"type={prop_meta.get('type', 'unknown')}, "
                f"units={prop_meta.get('units', 'unknown')}, "
                f"source={prop_meta.get('source', 'unknown')}"
            )

    # Check for property modification pattern (requires AND provides same property)
    modifies = set(requires) & set(provides)
    for prop_name in modifies:
        if prop_name in property_metadata:
            prop_meta = property_metadata[prop_name]
            if verbose:
                results.add_warning(
                    module_name,
                    f"Module both requires and provides '{prop_name}' "
                    f"(modification pattern). Type: {prop_meta.get('type', 'unknown')}, "
                    f"Units: {prop_meta.get('units', 'unknown')}",
                )

    return valid


def validate_dependencies(
    modules: List[Tuple[Path, Dict[str, Any]]],
    property_metadata: Dict[str, Dict[str, Any]],
    results: ValidationResults,
    verbose: bool = False,
) -> bool:
    """Validate module dependencies and check for cycles."""

    if not modules:
        return True

    # Extract module metadata
    module_list = [(m[0].name, m[1]) for m in modules]

    # Check that required/provided properties exist
    valid = True
    for module_dir, module in modules:
        module_name = module.get("name", module_dir.name)

        if not validate_module_dependencies(
            module, module_name, property_metadata, results, verbose
        ):
            valid = False

    # Build dependency graph
    try:
        graph = build_dependency_graph(module_list)
    except Exception as e:
        results.add_error("GLOBAL", 3, f"Failed to build dependency graph: {e}")
        return False

    # Check for circular dependencies using topological sort
    try:
        ts = TopologicalSorter(graph)
        _ = list(ts.static_order())  # This will raise CycleError if circular
    except CycleError as e:
        results.add_error("GLOBAL", 3, f"Circular dependency detected: {e}")
        return False

    return valid


# ==============================================================================
# MAIN VALIDATION FUNCTION
# ==============================================================================


def validate_module(
    module_dir: Path,
    module: Dict[str, Any],
    property_metadata: Dict[str, Dict[str, Any]],
    results: ValidationResults,
    verbose: bool = False,
) -> bool:
    """Validate a single module (all checks)."""

    module_name = module.get("name", module_dir.name)

    if verbose:
        print(f"Validating module: {module_name}")

    # Check if this is a utility module (different validation rules)
    is_utility = module.get("is_utility", False)

    # Utility modules have relaxed validation (only tests need to be specified)
    if is_utility:
        if verbose:
            print(f"  {module_name} is a utility module (relaxed validation)")

        # Validate only basic fields and tests
        if "name" not in module:
            results.add_error(module_name, 1, "Missing required field: name")
            return False

        # Validate name matches directory
        if not validate_name(module, module_name, module_dir, results):
            return False

        # Validate test files if specified
        validate_test_files(module, module_name, module_dir, results)

        if verbose:
            print(f"  ✓ {module_name} validated (utility module)")

        return True

    # Schema validation (for regular modules)
    if not validate_required_fields(module, module_name, results):
        return False

    if not validate_field_types(module, module_name, results):
        return False

    if not validate_version(module, module_name, results):
        return False

    if not validate_name(module, module_name, module_dir, results):
        return False

    if not validate_register_function(module, module_name, results):
        return False

    if not validate_parameters(module, module_name, results):
        return False

    if not validate_compilation_requires(module, module_name, results):
        return False

    # File existence validation
    if not validate_source_files(module, module_name, module_dir, results):
        return False

    validate_test_files(module, module_name, module_dir, results)
    validate_doc_files(module, module_name, results)

    # Code verification
    validate_register_function_exists(module, module_name, module_dir, results)

    if verbose:
        print(f"  ✓ {module_name} validated")

    return True


# ==============================================================================
# MAIN
# ==============================================================================


def main():
    """Main entry point."""

    parser = argparse.ArgumentParser(description="Validate Mimic module metadata")
    parser.add_argument(
        "module_path", nargs="?", help="Path to specific module directory"
    )
    parser.add_argument("--verbose", "-v", action="store_true", help="Verbose output")
    args = parser.parse_args()

    print("=" * 70)
    print("Module Metadata Validation")
    print("=" * 70)
    print()

    results = ValidationResults()

    # Load property metadata (both galaxy and halo) for dependency validation
    property_metadata = load_property_metadata()
    if property_metadata:
        galaxy_count = sum(
            1
            for p in property_metadata.values()
            if p.get("source") == "galaxy_properties.yaml"
        )
        halo_count = sum(
            1
            for p in property_metadata.values()
            if p.get("source") == "halo_properties.yaml"
        )
        print(
            f"Loaded {len(property_metadata)} properties "
            f"({galaxy_count} galaxy, {halo_count} halo)"
        )
        if args.verbose:
            print()

    # Discover modules
    if args.module_path:
        # Validate specific module
        module_dir = Path(args.module_path)
        if not module_dir.is_dir():
            print(f"ERROR: Not a directory: {module_dir}", file=sys.stderr)
            return 1

        metadata = load_module_metadata(module_dir)
        if metadata is None:
            print(f"ERROR: No module_info.yaml found in {module_dir}", file=sys.stderr)
            return 1

        modules = [(module_dir, metadata)]
    else:
        # Validate all modules
        modules = discover_modules()

    if not modules:
        print("No modules found to validate.")
        return 0

    print(f"Found {len(modules)} module(s) to validate")
    print()

    # Validate each module
    valid_modules = []
    for module_dir, metadata in modules:
        if metadata is None:
            results.add_error(
                module_dir.name, 1, "module_info.yaml not found or invalid"
            )
            continue

        validate_module(module_dir, metadata, property_metadata, results, args.verbose)
        valid_modules.append((module_dir, metadata))

    # Global dependency validation (only if we have valid modules)
    if valid_modules:
        print("\nValidating cross-module dependencies...")
        validate_dependencies(valid_modules, property_metadata, results, args.verbose)

    # Print summary
    results.print_summary()

    return results.get_exit_code()


if __name__ == "__main__":
    sys.exit(main())
