#!/usr/bin/env python3
"""
Model Parameter Code Generator for Mimic

Generates C code for model parameter validation and access from module metadata.
NO default values - all parameters must be explicitly specified in input file.

Architecture:
- Each module defines its own parameters in module_info.yaml under parameter_definitions:
- Multiple modules CAN define the same parameter (for shared parameters)
- First module wins, subsequent definitions must match type or generation fails
- Description/units from first definition used (documentation only)

Usage:
    python3 scripts/generate_model_parameters.py

Reads:
    src/modules/*/module_info.yaml  (for parameter definitions and dependencies)

Generates:
    src/include/generated/model_parameters.h  (parameter list, types, minimal metadata)
    src/include/generated/model_parameters.c  (validation functions + smart lookup)
"""

import hashlib
import sys
from pathlib import Path
from typing import Any, Dict, List

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

# Input: Module directories
MODULES_DIR = REPO_ROOT / "src" / "modules"

# Output directory
GENERATED_DIR = REPO_ROOT / "src" / "include" / "generated"
BUILD_DIR = REPO_ROOT / "build"

# Hash tracking file
PARAM_HASH_FILE = BUILD_DIR / "model_param_hash.txt"

# ==============================================================================
# TYPE MAPPINGS
# ==============================================================================

TYPE_MAP = {
    "int": {"c_type": "int", "format": "%d"},
    "double": {"c_type": "double", "format": "%g"},
    "float": {"c_type": "float", "format": "%g"},
    "string": {"c_type": "char*", "format": "%s"},
}

# ==============================================================================
# PARAMETER COLLECTION FROM MODULES
# ==============================================================================


def collect_parameters_from_modules() -> Dict[str, Dict]:
    """
    Collect parameter definitions from all module_info.yaml files.

    Scans all modules for parameter_definitions: sections.
    Handles duplicates: first module wins, type must match.

    Returns dict: param_name -> {name, type, description, units, source_module}
    """
    all_params = {}

    # Find all module_info.yaml files (sorted for deterministic order)
    # Use ** for recursive search to catch nested modules like _system/test_fixture
    module_info_files = sorted(MODULES_DIR.glob("**/module_info.yaml"))

    for info_file in module_info_files:
        try:
            with open(info_file) as f:
                data = yaml.safe_load(f)

            if not data or "module" not in data:
                continue

            module_data = data["module"]
            module_name = module_data.get("name")

            if not module_name:
                continue

            # Extract parameter definitions (new decentralized location)
            param_defs = module_data.get("parameter_definitions", [])

            if not param_defs:
                continue

            for param_def in param_defs:
                param_name = param_def.get("name")

                if not param_name:
                    print(
                        f"Warning: {module_name} has parameter without name",
                        file=sys.stderr,
                    )
                    continue

                if param_name in all_params:
                    # Duplicate found - validate type consistency
                    existing = all_params[param_name]
                    if existing["type"] != param_def["type"]:
                        raise ValueError(
                            f"Type mismatch for parameter '{param_name}':\n"
                            f"  {existing['source_module']} defines type '{existing['type']}'\n"
                            f"  {module_name} defines type '{param_def['type']}'\n"
                            f"  All modules must use the same type for shared parameters."
                        )
                    # Type matches - first definition wins (log for visibility)
                    print(
                        f"  Note: {param_name} also defined by {module_name} (type matches '{param_def['type']}')"
                    )
                else:
                    # First definition - validate and store
                    validate_parameter_definition(param_def, module_name)

                    all_params[param_name] = {
                        "name": param_name,
                        "type": param_def["type"],
                        "description": param_def["description"],
                        "units": param_def.get("units", "dimensionless"),
                        "source_module": module_name,
                    }

        except Exception as e:
            print(f"ERROR: Failed to read {info_file}: {e}", file=sys.stderr)
            raise

    return all_params


def validate_parameter_definition(param: Dict[str, Any], module_name: str) -> None:
    """Validate a parameter definition from module_info.yaml."""

    # Required fields (SIMPLIFIED: no range, no recommended, no references)
    required = ["name", "type", "description"]
    for field in required:
        if field not in param:
            raise ValueError(
                f"Module {module_name}: parameter missing required field '{field}': {param.get('name', 'unknown')}"
            )

    # Type validation
    if param["type"] not in TYPE_MAP:
        raise ValueError(
            f"Module {module_name}: invalid type '{param['type']}' for parameter '{param['name']}'. "
            f"Must be one of: {list(TYPE_MAP.keys())}"
        )

    # Name validation (C identifier)
    name = param["name"]
    if not name.replace("_", "").isalnum():
        raise ValueError(
            f"Module {module_name}: invalid parameter name '{name}': must be a valid C identifier"
        )


# ==============================================================================
# MODULE DEPENDENCY EXTRACTION
# ==============================================================================


def extract_module_dependencies() -> Dict[str, List[str]]:
    """
    Extract parameter dependencies from all module_info.yaml files.

    Returns dict: module_name -> list of required parameter names
    """
    dependencies = {}

    # Find all module_info.yaml files (recursive for nested modules)
    module_info_files = list(MODULES_DIR.glob("**/module_info.yaml"))

    for info_file in module_info_files:
        try:
            with open(info_file) as f:
                data = yaml.safe_load(f)

            if not data or "module" not in data:
                continue

            module_data = data["module"]
            module_name = module_data.get("name")

            if not module_name:
                continue

            # Extract parameter dependencies (existing field)
            deps = module_data.get("dependencies", {})
            params = deps.get("parameters", [])

            if params:
                dependencies[module_name] = params

        except Exception as e:
            print(f"Warning: Failed to read {info_file}: {e}", file=sys.stderr)
            continue

    return dependencies


# ==============================================================================
# HASH CHECKING
# ==============================================================================


def compute_module_metadata_hash() -> str:
    """Compute MD5 hash of all module_info.yaml files (for change detection)."""
    md5 = hashlib.md5()

    # Sort files for deterministic hash (recursive for nested modules)
    module_files = sorted(MODULES_DIR.glob("**/module_info.yaml"))

    for module_file in module_files:
        with open(module_file, "rb") as f:
            md5.update(f.read())

    return md5.hexdigest()


def load_saved_hash() -> str:
    """Load previously saved hash."""
    if PARAM_HASH_FILE.exists():
        return PARAM_HASH_FILE.read_text().strip()
    return ""


def save_hash(metadata_hash: str) -> None:
    """Save hash to disk."""
    ensure_dir(BUILD_DIR)
    PARAM_HASH_FILE.write_text(metadata_hash + "\n")


# ==============================================================================
# CODE GENERATION
# ==============================================================================


def generate_header(metadata_hash: str) -> str:
    """Generate common header for generated files."""
    return f"""/* AUTO-GENERATED CODE - DO NOT EDIT
 *
 * Generated by: scripts/generate_model_parameters.py
 * Source: src/modules/.../module_info.yaml (parameter_definitions sections)
 * Metadata MD5: {metadata_hash}
 *
 * To regenerate: make generate
 *
 * Architecture:
 *   - Each module defines its own parameters in module_info.yaml
 *   - Multiple modules can define same parameter (for shared parameters)
 *   - First module wins, type must match or generation fails
 *   - All parameters are REQUIRED in input file (no defaults)
 */

"""


def generate_model_parameters_h(params: List[Dict], metadata_hash: str) -> str:
    """Generate model_parameters.h header file."""

    code = generate_header(metadata_hash)
    code += "#ifndef GENERATED_MODEL_PARAMETERS_H\n"
    code += "#define GENERATED_MODEL_PARAMETERS_H\n\n"
    code += "#include <stddef.h>\n\n"

    # Parameter metadata structure (SIMPLIFIED: removed range fields)
    code += "/**\n"
    code += " * @brief Model parameter metadata (simplified)\n"
    code += " *\n"
    code += " * Defines structure and type for model parameters.\n"
    code += " * NO default values - all must be specified in input file.\n"
    code += " * NO range validation - trust the user (in-code checks catch issues).\n"
    code += " */\n"
    code += "struct ModelParameterMetadata {\n"
    code += "    const char *name;          /* Parameter name */\n"
    code += "    const char *type;          /* Type: int, double, string */\n"
    code += "    const char *description;   /* Human-readable description */\n"
    code += "    const char *units;         /* Physical units */\n"
    code += "    const char *source_module; /* Module that defines this parameter */\n"
    code += "};\n\n"

    # Constants
    code += f"/* Number of required model parameters */\n"
    code += f"#define NUM_REQUIRED_MODEL_PARAMETERS {len(params)}\n\n"

    # Parameter names array (for validation)
    code += "/* Required parameter names (for validation) */\n"
    code += "extern const char *REQUIRED_MODEL_PARAMETERS[NUM_REQUIRED_MODEL_PARAMETERS];\n\n"

    # Metadata array
    code += "/* Parameter metadata (for validation and documentation) */\n"
    code += "extern const struct ModelParameterMetadata MODEL_PARAMETER_METADATA[NUM_REQUIRED_MODEL_PARAMETERS];\n\n"

    # Function declarations (SIMPLIFIED: no range validation, just existence check)
    code += "/**\n"
    code += " * @brief Validate parameter exists in metadata (no range checking)\n"
    code += " *\n"
    code += " * @param param_name  Parameter name\n"
    code += " * @param value       Value (unused - no validation)\n"
    code += " * @return 0 if parameter exists, -1 if not found\n"
    code += " */\n"
    code += "int validate_model_param_double(const char *param_name, double value);\n"
    code += "int validate_model_param_int(const char *param_name, int value);\n\n"

    code += "/**\n"
    code += " * @brief Get parameter metadata by name\n"
    code += " *\n"
    code += " * @param param_name  Parameter name\n"
    code += " * @return Pointer to metadata, or NULL if not found\n"
    code += " */\n"
    code += "const struct ModelParameterMetadata *get_model_param_metadata(const char *param_name);\n\n"

    # Smart parameter lookup (unchanged - still uses dependencies.parameters)
    code += "/**\n"
    code += " * @brief Get required parameters for a set of enabled modules\n"
    code += " *\n"
    code += " * Determines which model parameters are needed by the specified\n"
    code += " * enabled modules by consulting module dependency metadata.\n"
    code += " *\n"
    code += " * @param enabled_modules Array of enabled module names\n"
    code += " * @param num_enabled Number of enabled modules\n"
    code += " * @param required_params_out Output array for required parameter names (must have space for NUM_REQUIRED_MODEL_PARAMETERS)\n"
    code += " * @param num_required_out Output count of required parameters\n"
    code += " * @return 0 on success, -1 on error\n"
    code += " */\n"
    code += "int get_required_params_for_modules(\n"
    code += "    const char **enabled_modules,\n"
    code += "    int num_enabled,\n"
    code += "    const char **required_params_out,\n"
    code += "    int *num_required_out\n"
    code += ");\n\n"

    code += "#endif /* GENERATED_MODEL_PARAMETERS_H */\n"

    return code


def generate_model_parameters_c(
    params: List[Dict], module_deps: Dict[str, List[str]], metadata_hash: str
) -> str:
    """Generate model_parameters.c implementation file."""

    code = generate_header(metadata_hash)
    code += '#include "generated/model_parameters.h"\n'
    code += '#include "error.h"\n'
    code += '#include <string.h>\n\n'

    # Required parameter names array
    code += "/* Required parameter names (for startup validation) */\n"
    code += "const char *REQUIRED_MODEL_PARAMETERS[NUM_REQUIRED_MODEL_PARAMETERS] = {\n"
    for param in params:
        code += f'    "{param["name"]}",\n'
    code += "};\n\n"

    # Parameter metadata array (SIMPLIFIED: no range fields)
    code += "/* Parameter metadata (minimal schema - no range validation) */\n"
    code += "const struct ModelParameterMetadata MODEL_PARAMETER_METADATA[NUM_REQUIRED_MODEL_PARAMETERS] = {\n"

    for param in params:
        name = param["name"]
        ptype = param["type"]
        description = param["description"].replace('"', '\\"').replace("\n", " ")
        units = param["units"]
        source_module = param["source_module"]

        code += f'    {{ "{name}", "{ptype}", "{description}", "{units}", "{source_module}" }},\n'

    code += "};\n\n"

    # Simplified validation functions (NO range checking - just metadata lookup)
    code += "/* Simplified validation: check parameter exists (no range checking) */\n"
    code += "int validate_model_param_double(const char *param_name, double value) {\n"
    code += "    (void)value;  /* Unused - no range validation */\n"
    code += "    const struct ModelParameterMetadata *meta = get_model_param_metadata(param_name);\n"
    code += "    if (meta == NULL) {\n"
    code += '        ERROR_LOG("Model parameter \'%s\' not found in metadata", param_name);\n'
    code += "        return -1;\n"
    code += "    }\n"
    code += "    return 0;  /* No range validation - trust the user */\n"
    code += "}\n\n"

    code += "int validate_model_param_int(const char *param_name, int value) {\n"
    code += "    (void)value;  /* Unused - no range validation */\n"
    code += "    const struct ModelParameterMetadata *meta = get_model_param_metadata(param_name);\n"
    code += "    if (meta == NULL) {\n"
    code += '        ERROR_LOG("Model parameter \'%s\' not found in metadata", param_name);\n'
    code += "        return -1;\n"
    code += "    }\n"
    code += "    return 0;  /* No range validation - trust the user */\n"
    code += "}\n\n"

    # Metadata lookup function
    code += "const struct ModelParameterMetadata *get_model_param_metadata(const char *param_name) {\n"
    code += "    for (int i = 0; i < NUM_REQUIRED_MODEL_PARAMETERS; i++) {\n"
    code += "        if (strcmp(MODEL_PARAMETER_METADATA[i].name, param_name) == 0) {\n"
    code += "            return &MODEL_PARAMETER_METADATA[i];\n"
    code += "        }\n"
    code += "    }\n"
    code += "    return NULL;\n"
    code += "}\n\n"

    # Smart parameter lookup based on module dependencies
    code += generate_smart_lookup_function(module_deps)

    return code


def generate_smart_lookup_function(module_deps: Dict[str, List[str]]) -> str:
    """Generate the smart parameter lookup function (unchanged logic)."""

    code = "/* Smart parameter lookup based on module dependencies */\n"
    code += "int get_required_params_for_modules(\n"
    code += "    const char **enabled_modules,\n"
    code += "    int num_enabled,\n"
    code += "    const char **required_params_out,\n"
    code += "    int *num_required_out\n"
    code += ") {\n"
    code += "    if (num_enabled == 0) {\n"
    code += "        *num_required_out = 0;\n"
    code += "        return 0;\n"
    code += "    }\n\n"

    code += "    /* Build union of all required parameters */\n"
    code += "    int num_required = 0;\n\n"

    code += "    for (int i = 0; i < num_enabled; i++) {\n"
    code += "        const char *module_name = enabled_modules[i];\n\n"

    # Generate if-else chain for each module
    first = True
    for module_name, param_list in sorted(module_deps.items()):
        if not param_list:
            continue

        if_keyword = "if" if first else "} else if"
        first = False

        code += f'        {if_keyword} (strcmp(module_name, "{module_name}") == 0) {{\n'
        code += f"            /* {module_name} requires {len(param_list)} parameters */\n"

        for param_name in param_list:
            code += "            {\n"  # Scope block for each parameter
            code += f'                const char *param = "{param_name}";\n'
            code += "                /* Check if already in list (deduplicate) */\n"
            code += "                int found = 0;\n"
            code += "                for (int j = 0; j < num_required; j++) {\n"
            code += "                    if (strcmp(required_params_out[j], param) == 0) {\n"
            code += "                        found = 1;\n"
            code += "                        break;\n"
            code += "                    }\n"
            code += "                }\n"
            code += "                if (!found) {\n"
            code += "                    required_params_out[num_required++] = param;\n"
            code += "                }\n"
            code += "            }\n"  # End scope block

    if not first:  # If we had any modules
        code += "        }\n"

    code += "    }\n\n"
    code += "    *num_required_out = num_required;\n"
    code += "    return 0;\n"
    code += "}\n"

    return code


# ==============================================================================
# FILE WRITING
# ==============================================================================


def ensure_dir(path: Path) -> None:
    """Ensure directory exists."""
    path.mkdir(parents=True, exist_ok=True)


def write_file_if_changed(path: Path, content: str) -> bool:
    """Write file only if content changed. Returns True if written."""
    if path.exists() and path.read_text() == content:
        return False

    ensure_dir(path.parent)
    path.write_text(content)
    return True


# ==============================================================================
# MAIN
# ==============================================================================


def main():
    """Main code generation function."""

    print("=" * 70)
    print("Model Parameter Code Generator (Decentralized Architecture)")
    print("=" * 70)

    # Collect parameter definitions from all modules
    print("Collecting parameter definitions from module_info.yaml files...")
    params_dict = collect_parameters_from_modules()

    if not params_dict:
        print("ERROR: No parameter definitions found in any module_info.yaml files")
        sys.exit(1)

    # Convert to list and sort by name for deterministic output
    params = sorted(params_dict.values(), key=lambda p: p["name"])
    print(f"✓ Collected {len(params)} unique parameters from modules")

    # Show parameter sources
    print("\nParameter definitions by module:")
    by_module = {}
    for param in params:
        source = param["source_module"]
        if source not in by_module:
            by_module[source] = []
        by_module[source].append(param["name"])

    for module in sorted(by_module.keys()):
        print(f"  {module}: {len(by_module[module])} parameters")

    # Extract module dependencies (for smart lookup)
    print("\nReading module dependencies...")
    module_deps = extract_module_dependencies()
    print(f"✓ Found {len(module_deps)} modules with parameter dependencies")

    # Compute hash of all module metadata
    metadata_hash = compute_module_metadata_hash()
    saved_hash = load_saved_hash()

    if metadata_hash == saved_hash:
        print(f"\n✓ Module metadata unchanged (MD5: {metadata_hash[:8]}...), skipping generation")
        sys.exit(0)

    print(f"\nModule metadata changed, generating code (MD5: {metadata_hash[:8]}...)")

    # Generate files
    files_written = []

    # Generate model_parameters.h
    h_file = GENERATED_DIR / "model_parameters.h"
    h_content = generate_model_parameters_h(params, metadata_hash)
    if write_file_if_changed(h_file, h_content):
        files_written.append(str(h_file.relative_to(REPO_ROOT)))

    # Generate model_parameters.c
    c_file = GENERATED_DIR / "model_parameters.c"
    c_content = generate_model_parameters_c(params, module_deps, metadata_hash)
    if write_file_if_changed(c_file, c_content):
        files_written.append(str(c_file.relative_to(REPO_ROOT)))

    # Save hash
    save_hash(metadata_hash)

    # Summary
    print(f"\n✓ Generated {len(files_written)} files:")
    for f in files_written:
        print(f"  - {f}")

    print(f"\n✓ Model parameter code generation complete")
    print(f"  Total parameters: {len(params)}")
    print(f"  Modules with definitions: {len(by_module)}")
    print(f"  Modules with dependencies: {len(module_deps)}")
    print(f"  All parameters REQUIRED in input file (no defaults)")
    print(f"  No range validation (trust the user)")


if __name__ == "__main__":
    main()
