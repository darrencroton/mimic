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
    3 - Dependency error (unresolved property)
    4 - Naming convention violation
"""

import argparse
import re
import sys
from pathlib import Path
from typing import Any, Dict, List, Optional, Tuple

try:
    import yaml
except ImportError:
    print("ERROR: PyYAML not installed. Run: pip install PyYAML", file=sys.stderr)
    sys.exit(1)

from console import BLUE, GREEN, NC, RED, YELLOW
from discovery import (
    REPO_ROOT,
    halo_property_files,
    model_property_files,
    module_metadata_files,
    rel,
    standalone_module_files,
)
from generate_properties import load_parameter_units

# ==============================================================================
# PATHS
# ==============================================================================

# Module and property roots are discovered from model/simulation packages.

# ==============================================================================
# SCHEMA DEFINITIONS
# ==============================================================================


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
            print()
            for warning in self.warnings:
                print(f"{YELLOW}{warning}{NC}")

        if self.errors:
            print()
            print(f"{RED}ERRORS ({len(self.errors)}){NC}")
            for error in self.errors:
                print(f"{RED}  {error}{NC}")
            print()
            print(f"{RED}✗ Validation failed - {len(self.errors)} error(s) found{NC}")
        else:
            print()
            print(f"{GREEN}✓ Validation PASSED{NC}")


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
    """Discover all modules in configured package roots."""
    modules = []

    for yaml_path in module_metadata_files():
        modules.append((yaml_path.parent, load_module_metadata(yaml_path.parent)))

    for c_file in standalone_module_files():
        module_name = c_file.stem
        modules.append(
            (
                c_file.parent,
                {
                    "name": module_name,
                    "display_name": module_name.replace("_", " ").title(),
                    "version": "1.0.0",
                    "supported_processing_modes": [
                        "process_full_halo",
                        "process_per_event",
                        "process_by_galaxy",
                    ],
                    "dependencies": {"properties": [], "parameters": []},
                    "_pattern": "standalone",
                    "_standalone_file": c_file,
                },
            )
        )

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
                "source": "models/<model>/model_properties.yaml"
            },
            "Mvir": {
                "name": "Mvir",
                "type": "float",
                "units": "1e10 Msun/h",
                "description": "...",
                "source": "src/core/core_properties.yaml or simulations/<simulation>/halo_properties.yaml"
            },
            ...
        }
    """
    properties = {}

    # Load galaxy properties
    for path in model_property_files():
        try:
            with open(path, "r", encoding="utf-8") as f:
                data = yaml.safe_load(f)
                if data and "galaxy_properties" in data:
                    for prop in data["galaxy_properties"]:
                        if "name" in prop:
                            properties[prop["name"]] = {
                                **prop,
                                "source": rel(path),
                            }
        except Exception as e:
            print(
                f"WARNING: Failed to load galaxy properties from {rel(path)}: {e}", file=sys.stderr
            )

    # Load halo properties
    for path in halo_property_files():
        try:
            with open(path, "r", encoding="utf-8") as f:
                data = yaml.safe_load(f)
                if data and "halo_properties" in data:
                    for prop in data["halo_properties"]:
                        if "name" in prop:
                            properties[prop["name"]] = {
                                **prop,
                                "source": rel(path),
                            }
        except Exception as e:
            print(f"WARNING: Failed to load halo properties from {rel(path)}: {e}", file=sys.stderr)

    return properties


# ==============================================================================
# SCHEMA VALIDATION
# ==============================================================================


def validate_required_fields(
    module: Dict[str, Any], module_name: str, results: ValidationResults
) -> bool:
    """Validate that all required fields are present."""

    # Minimal required fields - only name and processing mode!
    required_core = [
        "name",
        "supported_processing_modes",  # Required: how to execute the module
    ]
    # Everything else is optional:
    # - description: optional (can be empty)
    # - display_name: auto-generated from name
    # - version: defaults to "1.0.0"
    # - author: optional
    # - additional_files: optional (only needed for multi-file modules)
    # - dependencies: optional (skip validation if absent)

    all_required = required_core

    missing = [field for field in all_required if field not in module]

    if missing:
        results.add_error(module_name, 1, f"Missing required fields: {', '.join(missing)}")
        return False

    # Validate dependencies subfields IF present
    if "dependencies" in module:
        deps = module["dependencies"]
        if "properties" not in deps or "parameters" not in deps:
            results.add_error(
                module_name,
                1,
                "dependencies (if present) must have both 'properties' and 'parameters' fields",
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
    ]:
        if field in module and not isinstance(module[field], str):
            results.add_error(module_name, 1, f"Field '{field}' must be a string")
            valid = False

    # List fields
    if "additional_files" in module:
        if not isinstance(module["additional_files"], list):
            results.add_error(module_name, 1, "Field 'additional_files' must be a list")
            valid = False
        elif not all(isinstance(item, str) for item in module["additional_files"]):
            results.add_error(module_name, 1, "All items in 'additional_files' must be strings")
            valid = False

    # Supported processing modes (optional field)
    if "supported_processing_modes" in module:
        modes = module["supported_processing_modes"]
        if not isinstance(modes, list):
            results.add_error(module_name, 1, "Field 'supported_processing_modes' must be a list")
            valid = False
        elif not all(isinstance(item, str) for item in modes):
            results.add_error(
                module_name,
                1,
                "All items in 'supported_processing_modes' must be strings",
            )
            valid = False

    # Dependencies
    if "dependencies" in module:
        deps = module["dependencies"]
        if not isinstance(deps, dict):
            results.add_error(module_name, 1, "Field 'dependencies' must be a dict")
            valid = False
        else:
            # Validate 'properties' (list of all properties used by module)
            if "properties" in deps:
                if not isinstance(deps["properties"], list):
                    results.add_error(module_name, 1, "dependencies.properties must be a list")
                    valid = False
                elif not all(isinstance(item, str) for item in deps["properties"]):
                    results.add_error(
                        module_name,
                        1,
                        "All items in dependencies.properties must be strings",
                    )
                    valid = False

            # Validate 'parameters' (list of all parameters needed by module)
            if "parameters" in deps:
                if not isinstance(deps["parameters"], list):
                    results.add_error(module_name, 1, "dependencies.parameters must be a list")
                    valid = False
                elif not all(isinstance(item, str) for item in deps["parameters"]):
                    results.add_error(
                        module_name,
                        1,
                        "All items in dependencies.parameters must be strings",
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

    return valid


def validate_version(module: Dict[str, Any], module_name: str, results: ValidationResults) -> bool:
    """Validate version follows semantic versioning (if present)."""

    # Version is optional - defaults to "1.0.0" if not specified
    version = module.get("version", "")
    if not version:
        # No version specified - this is valid
        return True

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
        results.add_error(module_name, 4, f"Module name '{name}' is not a valid C identifier")
        return False

    # Check lowercase with underscores convention
    if not name.islower() or not all(c.isalnum() or c == "_" for c in name):
        results.add_warning(
            module_name, f"Module name '{name}' should be lowercase_with_underscores"
        )

    if module.get("_pattern") == "standalone":
        c_file = module.get("_standalone_file")
        if not c_file or name != Path(c_file).stem:
            results.add_error(
                module_name,
                4,
                f"Standalone module name '{name}' doesn't match source file stem",
            )
            return False
        return True

    # Check matches directory name
    if name != module_dir.name:
        results.add_error(
            module_name,
            4,
            f"Module name '{name}' doesn't match directory name '{module_dir.name}'",
        )
        return False

    return True


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


def validate_supported_processing_modes(
    module: Dict[str, Any], module_name: str, results: ValidationResults
) -> bool:
    """Validate supported_processing_modes field."""

    # Field is optional - if omitted, defaults to
    # [process_full_halo, process_per_event, process_by_galaxy]
    if "supported_processing_modes" not in module:
        return True

    modes = module["supported_processing_modes"]

    # Empty list not allowed
    if len(modes) == 0:
        results.add_error(
            module_name,
            1,
            "supported_processing_modes cannot be empty. "
            "Specify one or more of ['process_full_halo', 'process_per_event', 'process_by_galaxy']",
        )
        return False

    # Check for valid values
    valid_modes = {"process_full_halo", "process_per_event", "process_by_galaxy"}
    invalid = [mode for mode in modes if mode not in valid_modes]
    if invalid:
        results.add_error(
            module_name,
            1,
            f"Invalid processing mode(s): {', '.join(invalid)}. "
            f"Must be from {sorted(valid_modes)}",
        )
        return False

    # Check for duplicates
    if len(modes) != len(set(modes)):
        results.add_error(module_name, 1, "supported_processing_modes contains duplicates")
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
    """Validate that all source files exist.

    Format: {name}.c is always implicit, additional_files lists extras
    """

    valid = True

    main_file = (
        Path(module["_standalone_file"])
        if module.get("_pattern") == "standalone"
        else module_dir / f"{module_name}.c"
    )
    if not main_file.exists():
        results.add_error(module_name, 2, f"Main module file not found: {module_name}.c")
        valid = False

    # Check additional files (if any)
    additional = module.get("additional_files", [])
    for file in additional:
        file_path = module_dir / file
        if not file_path.exists():
            results.add_error(module_name, 2, f"Additional file not found: {file}")
            valid = False

    return valid


def _missing_test_files(
    test_value: Any, module_dir: Path, extra_dirs: Tuple[Path, ...] = ()
) -> List[str]:
    """Return declared test filenames that do not exist in any search directory.

    ``test_value`` may be None (no test declared), a single filename, or a list.
    A file is present if it exists under ``module_dir`` or any of ``extra_dirs``
    (e.g. the centralized tests/unit/ directory for unit tests).
    """
    if test_value is None:
        return []
    test_files = test_value if isinstance(test_value, list) else [test_value]
    search_dirs = (module_dir, *extra_dirs)
    return [tf for tf in test_files if not any((base / tf).exists() for base in search_dirs)]


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

    # Unit tests may be co-located with the module or centralized in tests/unit/;
    # integration and scientific tests are co-located only.
    tiers = [
        ("unit", "Unit", (REPO_ROOT / "tests" / "unit",)),
        ("integration", "Integration", ()),
        ("scientific", "Scientific", ()),
    ]
    for key, label, extra_dirs in tiers:
        if key in tests:
            for test_file in _missing_test_files(tests[key], module_dir, extra_dirs):
                results.add_warning(module_name, f"{label} test file not found: {test_file}")

    return True


def validate_doc_files(
    module_dir: Path, module: Dict[str, Any], module_name: str, results: ValidationResults
) -> bool:
    """Validate that documentation files exist (warnings only)."""

    if "docs" not in module:
        results.add_warning(module_name, "No documentation specified")
        return True

    docs = module["docs"]

    if "physics" in docs:
        physics_doc = docs["physics"]
        # null means "no physics doc" — treat as absent
        if physics_doc is not None:
            physics_doc_path = module_dir / physics_doc
            if not physics_doc_path.exists():
                results.add_warning(module_name, f"Physics documentation not found: {physics_doc}")

    return True


# ==============================================================================
# DEPENDENCY VALIDATION
# ==============================================================================


def validate_module_dependencies(
    module: Dict[str, Any],
    module_name: str,
    property_metadata: Dict[str, Dict[str, Any]],
    results: ValidationResults,
    verbose: bool = False,
) -> bool:
    """Validate module dependencies against property metadata."""

    deps = module.get("dependencies", {})
    properties = deps.get("properties", [])
    # parameters are validated elsewhere (modules.parameters system)

    valid = True

    # Validate all properties exist in metadata
    for prop in properties:
        if prop not in property_metadata:
            results.add_error(
                module_name,
                3,
                f"Property '{prop}' not found in property metadata. "
                f"Check model_properties.yaml and halo_properties.yaml.",
            )
            valid = False
        elif verbose:
            prop_meta = property_metadata[prop]
            print(
                f"  {module_name} uses {prop}: "
                f"type={prop_meta.get('type', 'unknown')}, "
                f"units={prop_meta.get('units', 'unknown')}, "
                f"source={prop_meta.get('source', 'unknown')}"
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

    # Note: Module execution order is now manual (specified in config YAML)
    # No automatic dependency resolution or circular dependency checking

    return valid


def validate_parameter_units(valid_modules, results: ValidationResults) -> None:
    """Validate model-global dimensional parameter metadata against module declarations."""
    declared = set()
    for _, module in valid_modules:
        deps = module.get("dependencies", {})
        for param in deps.get("parameters", []):
            declared.add(param)

    # Structural validation (types, units, h conventions, duplicates) is owned by
    # the generator; reuse it so the two paths cannot drift.
    try:
        entries = load_parameter_units()
    except ValueError as e:
        results.add_error("parameter_units", 1, str(e))
        return

    for entry in entries:
        if entry["name"] not in declared:
            results.add_error(
                "parameter_units",
                3,
                f"parameter '{entry['name']}' is not declared by any selected module",
            )


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

    if not validate_compilation_requires(module, module_name, results):
        return False

    if not validate_supported_processing_modes(module, module_name, results):
        return False

    # File existence validation
    if not validate_source_files(module, module_name, module_dir, results):
        return False

    if module.get("_pattern") == "standalone":
        if verbose:
            print(f"  ✓ {module_name} validated (standalone module)")
        return True

    validate_test_files(module, module_name, module_dir, results)
    validate_doc_files(module_dir, module, module_name, results)

    if verbose:
        print(f"  ✓ {module_name} validated")

    return True


# ==============================================================================
# MAIN
# ==============================================================================


def main():
    """Main entry point."""

    parser = argparse.ArgumentParser(description="Validate Mimic module metadata")
    parser.add_argument("module_path", nargs="?", help="Path to specific module directory")
    parser.add_argument("--verbose", "-v", action="store_true", help="Verbose output")
    args = parser.parse_args()

    print(f"{BLUE}{'=' * 60}{NC}")
    print(f"{BLUE}Module Metadata Validation{NC}")
    print(f"{BLUE}{'=' * 60}{NC}")
    print()

    results = ValidationResults()

    # Load property metadata (both galaxy and halo) for dependency validation
    property_metadata = load_property_metadata()
    if property_metadata:
        galaxy_sources = {rel(path) for path in model_property_files()}
        halo_sources = {rel(path) for path in halo_property_files()}
        galaxy_count = sum(
            1 for p in property_metadata.values() if p.get("source") in galaxy_sources
        )
        halo_count = sum(1 for p in property_metadata.values() if p.get("source") in halo_sources)
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

    seen_names = {}
    for module_dir, metadata in modules:
        if metadata is None:
            continue
        name = metadata.get("name", module_dir.name)
        if name in seen_names:
            results.add_error(
                name,
                4,
                f"Duplicate module name discovered in {seen_names[name]} and {module_dir}",
            )
        else:
            seen_names[name] = module_dir

    # Validate each module
    valid_modules = []
    for module_dir, metadata in modules:
        if metadata is None:
            results.add_error(module_dir.name, 1, "module_info.yaml not found or invalid")
            continue

        validate_module(module_dir, metadata, property_metadata, results, args.verbose)
        valid_modules.append((module_dir, metadata))

    # Global dependency validation (only if we have valid modules)
    if valid_modules:
        print("Validating cross-module dependencies...")
        validate_dependencies(valid_modules, property_metadata, results, args.verbose)
        print("Validating parameter unit metadata...")
        validate_parameter_units(valid_modules, results)

    # Print summary
    results.print_summary()

    return results.get_exit_code()


if __name__ == "__main__":
    sys.exit(main())
