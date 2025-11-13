#!/usr/bin/env python3
"""
Property Code Generator for Mimic

Generates C structures, initialization code, output code, and Python dtypes from
YAML property metadata definitions. This eliminates manual synchronization across
8+ files and enables rapid property addition (<2 minutes vs 30 minutes).

Usage:
    python3 scripts/generate_properties.py

Reads:
    metadata/properties/halo_properties.yaml
    metadata/properties/galaxy_properties.yaml

Generates:
    src/include/generated/property_defs.h
    src/include/generated/property_metadata.c
    src/include/generated/init_halo_properties.inc
    src/include/generated/init_galaxy_properties.inc
    src/include/generated/copy_to_output.inc
    src/include/generated/hdf5_field_count.inc
    src/include/generated/hdf5_field_definitions.inc
    output/mimic-plot/generated/dtype.py
    output/mimic-plot/generated/__init__.py

Author: Property Metadata System (Phase 1)
Date: 2025-11-07
"""

import hashlib
import json
import os
import sys
from datetime import datetime
from pathlib import Path
from typing import Any, Dict, List

try:
    import yaml
except ImportError:
    print("ERROR: PyYAML not installed. Run: pip install PyYAML", file=sys.stderr)
    sys.exit(1)

# ==============================================================================
# TYPE MAPPINGS
# ==============================================================================

TYPE_MAP = {
    "int": {
        "c_type": "int",
        "numpy_type": "np.int32",
        "h5_type": "H5T_NATIVE_INT",
        "is_array": False,
    },
    "float": {
        "c_type": "float",
        "numpy_type": "np.float32",
        "h5_type": "H5T_NATIVE_FLOAT",
        "is_array": False,
    },
    "double": {
        "c_type": "double",
        "numpy_type": "np.float64",
        "h5_type": "H5T_NATIVE_DOUBLE",
        "is_array": False,
    },
    "long long": {
        "c_type": "long long",
        "numpy_type": "np.int64",
        "h5_type": "H5T_NATIVE_LLONG",
        "is_array": False,
    },
    "vec3_float": {
        "c_type": "float",
        "c_array": "[3]",
        "numpy_type": "(np.float32, 3)",
        "h5_type": "array3f_tid",
        "is_array": True,
        "array_size": 3,
    },
    "vec3_int": {
        "c_type": "int",
        "c_array": "[3]",
        "numpy_type": "(np.int32, 3)",
        "h5_type": "array3i_tid",
        "is_array": True,
        "array_size": 3,
    },
}

VALID_INIT_SOURCES = [
    "default",
    "copy_from_tree",
    "copy_from_tree_array",
    "calculate",
    "skip",
]
VALID_OUTPUT_SOURCES = [
    "copy_direct",
    "copy_direct_array",
    "copy_from_tree",
    "copy_from_tree_array",
    "recalculate",
    "conditional",
    "custom",
    "galaxy_property",
]

# ==============================================================================
# PATHS
# ==============================================================================

# Repository root (parent of scripts/)
REPO_ROOT = Path(__file__).parent.parent

# Input YAML files
HALO_PROPERTIES_YAML = REPO_ROOT / "metadata" / "properties" / "halo_properties.yaml"
GALAXY_PROPERTIES_YAML = (
    REPO_ROOT / "metadata" / "properties" / "galaxy_properties.yaml"
)

# Output directories
GENERATED_DIR = REPO_ROOT / "src" / "include" / "generated"
PLOT_GENERATED_DIR = REPO_ROOT / "output" / "mimic-plot" / "generated"
TESTS_GENERATED_DIR = REPO_ROOT / "tests" / "generated"

# ==============================================================================
# VALIDATION
# ==============================================================================


def validate_property(prop: Dict[str, Any], category: str) -> None:
    """Validate a property definition according to schema."""

    # Required fields
    required = ["name", "type", "units", "description", "output"]
    for field in required:
        if field not in prop:
            raise ValueError(
                f"{category} property missing required field '{field}': {prop}"
            )

    # Type validation
    if prop["type"] not in TYPE_MAP:
        raise ValueError(
            f"Invalid type '{prop['type']}' for property '{prop['name']}'. "
            f"Must be one of: {list(TYPE_MAP.keys())}"
        )

    # Name validation (basic C identifier check)
    name = prop["name"]
    if not name.isidentifier():
        raise ValueError(
            f"Invalid property name '{name}': must be a valid C identifier"
        )

    # Check init_source for halo properties
    if category == "halo" and "init_source" in prop:
        if prop["init_source"] not in VALID_INIT_SOURCES:
            raise ValueError(
                f"Invalid init_source '{prop['init_source']}' for '{name}'. "
                f"Must be one of: {VALID_INIT_SOURCES}"
            )

    # Check output_source if output=true
    if prop["output"] and "output_source" in prop:
        if prop["output_source"] not in VALID_OUTPUT_SOURCES:
            raise ValueError(
                f"Invalid output_source '{prop['output_source']}' for '{name}'. "
                f"Must be one of: {VALID_OUTPUT_SOURCES}"
            )

    # Dependency checks
    if prop.get("init_source") == "default" and "init_value" not in prop:
        raise ValueError(
            f"Property '{name}' with init_source=default must have init_value"
        )

    if prop.get("init_source") == "calculate" and "init_function" not in prop:
        raise ValueError(
            f"Property '{name}' with init_source=calculate must have init_function"
        )

    if (
        prop.get("output_source") == "copy_from_tree"
        and "output_tree_field" not in prop
    ):
        raise ValueError(
            f"Property '{name}' with output_source=copy_from_tree must have output_tree_field"
        )

    if prop.get("output_source") == "recalculate":
        if "output_function" not in prop or "output_function_arg" not in prop:
            raise ValueError(
                f"Property '{name}' with output_source=recalculate must have "
                "output_function and output_function_arg"
            )

    if prop.get("output_source") == "conditional":
        required_cond = ["output_condition", "output_true_value", "output_false_value"]
        for field in required_cond:
            if field not in prop:
                raise ValueError(
                    f"Property '{name}' with output_source=conditional must have {field}"
                )


def validate_properties(halo_props: List[Dict], galaxy_props: List[Dict]) -> None:
    """Validate all properties and check for duplicates."""

    # Validate each property
    for prop in halo_props:
        validate_property(prop, "halo")

    for prop in galaxy_props:
        validate_property(prop, "galaxy")

    # Check for duplicate names
    all_names = [p["name"] for p in halo_props] + [p["name"] for p in galaxy_props]
    duplicates = [name for name in set(all_names) if all_names.count(name) > 1]
    if duplicates:
        raise ValueError(f"Duplicate property names found: {duplicates}")

    print(f"✓ Validated {len(halo_props)} halo properties")
    print(f"✓ Validated {len(galaxy_props)} galaxy properties")


# ==============================================================================
# C CODE GENERATION
# ==============================================================================


def compute_yaml_hash() -> str:
    """Compute MD5 hash of YAML input files for validation."""
    md5 = hashlib.md5()

    # Hash both YAML files in order (halo, then galaxy)
    for yaml_file in [HALO_PROPERTIES_YAML, GALAXY_PROPERTIES_YAML]:
        with open(yaml_file, "rb") as f:
            md5.update(f.read())

    return md5.hexdigest()


def generate_header(yaml_hash: str):
    """Generate common header for all generated files."""
    return f"""/* AUTO-GENERATED CODE - DO NOT EDIT
 *
 * Generated by: scripts/generate_properties.py
 * Generated on: {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}
 *
 * Source files:
 *   - metadata/properties/halo_properties.yaml
 *   - metadata/properties/galaxy_properties.yaml
 *
 * Source MD5: {yaml_hash}
 * To regenerate: make generate
 */

"""


def generate_property_defs_h(
    halo_props: List[Dict], galaxy_props: List[Dict], yaml_hash: str
) -> str:
    """Generate property_defs.h with struct definitions."""

    code = generate_header(yaml_hash)
    code += "#ifndef GENERATED_PROPERTY_DEFS_H\n"
    code += "#define GENERATED_PROPERTY_DEFS_H\n\n"

    # Forward declarations
    code += "/* Forward declarations */\n"
    code += "struct GalaxyData;\n\n"

    # struct Halo (include all properties except those ONLY in output)
    # A property is in struct Halo if:
    #   - output: false (internal only), OR
    #   - output: true AND init_source != skip (in both processing and output)
    # Properties with output: true AND init_source: skip are output-only (not in struct Halo)
    code += "/* Halo properties (internal processing) */\n"
    code += "struct Halo {\n"
    code += "  /* Halo properties */\n"
    for prop in halo_props:
        # Include if: internal-only OR (output AND has processing logic)
        is_internal_only = not prop["output"]
        is_in_processing = prop.get("init_source") != "skip"
        if is_internal_only or is_in_processing:
            type_info = TYPE_MAP[prop["type"]]
            c_type = type_info["c_type"]
            array_suffix = type_info.get("c_array", "")
            code += f"  {c_type} {prop['name']}{array_suffix};\n"
    code += "\n  /* Galaxy pointer (physics-agnostic separation) */\n"
    code += "  struct GalaxyData *galaxy;\n"
    code += "};\n\n"

    # struct GalaxyData
    code += "/* Galaxy properties (baryonic physics) */\n"
    code += "struct GalaxyData {\n"
    for prop in galaxy_props:
        type_info = TYPE_MAP[prop["type"]]
        c_type = type_info["c_type"]
        array_suffix = type_info.get("c_array", "")
        code += f"  {c_type} {prop['name']}{array_suffix};\n"
    code += "};\n\n"

    # struct HaloOutput (all properties with output=true)
    code += "/* Output structure (file writing) */\n"
    code += "struct HaloOutput {\n"
    code += "  /* Halo properties */\n"
    for prop in halo_props:
        if prop["output"]:
            type_info = TYPE_MAP[prop["type"]]
            c_type = type_info["c_type"]
            array_suffix = type_info.get("c_array", "")
            code += f"  {c_type} {prop['name']}{array_suffix};\n"
    code += "\n  /* Galaxy properties */\n"
    for prop in galaxy_props:
        if prop["output"]:
            type_info = TYPE_MAP[prop["type"]]
            c_type = type_info["c_type"]
            array_suffix = type_info.get("c_array", "")
            code += f"  {c_type} {prop['name']}{array_suffix};\n"
    code += "};\n\n"

    code += "#endif /* GENERATED_PROPERTY_DEFS_H */\n"
    return code


def generate_init_halo_properties(halo_props: List[Dict], yaml_hash: str) -> str:
    """Generate init_halo_properties.inc initialization code."""

    code = generate_header(yaml_hash)
    code += "/* Initialize halo properties in init_halo(int p, int halonr) */\n\n"

    for prop in halo_props:
        init_source = prop.get("init_source", "skip")
        name = prop["name"]
        type_info = TYPE_MAP[prop["type"]]

        if init_source == "skip":
            # Determine if property is in struct Halo or output-only
            is_internal_only = not prop["output"]
            is_in_processing = prop.get("init_source") != "skip"
            is_in_struct = is_internal_only or is_in_processing

            if is_in_struct:
                # Property is in struct Halo but has custom initialization
                code += f"/* {name}: skip (custom initialization in init_halo) */\n"
            else:
                # Property is output-only, not in struct Halo
                code += f"/* {name}: skip (output-only, not in struct Halo) */\n"

        elif init_source == "default":
            init_value = prop["init_value"]
            code += f"FoFWorkspace[p].{name} = {init_value};\n"

        elif init_source == "copy_from_tree":
            code += f"FoFWorkspace[p].{name} = InputTreeHalos[halonr].{name};\n"

        elif init_source == "copy_from_tree_array":
            if not type_info["is_array"]:
                raise ValueError(
                    f"Property '{name}' uses copy_from_tree_array but type is not array"
                )
            code += f"for (int j = 0; j < {type_info['array_size']}; j++) {{\n"
            code += f"  FoFWorkspace[p].{name}[j] = InputTreeHalos[halonr].{name}[j];\n"
            code += "}\n"

        elif init_source == "calculate":
            func = prop["init_function"]
            code += f"FoFWorkspace[p].{name} = {func}(halonr);\n"

    return code


def generate_init_galaxy_properties(galaxy_props: List[Dict], yaml_hash: str) -> str:
    """Generate init_galaxy_properties.inc initialization code."""

    code = generate_header(yaml_hash)
    code += (
        "/* Initialize galaxy properties after allocating FoFWorkspace[p].galaxy */\n\n"
    )

    for prop in galaxy_props:
        init_source = prop.get("init_source", "default")
        name = prop["name"]

        if init_source == "default":
            init_value = prop.get("init_value", "0.0")
            code += f"FoFWorkspace[p].galaxy->{name} = {init_value};\n"

    return code


def generate_copy_to_output(
    halo_props: List[Dict], galaxy_props: List[Dict], yaml_hash: str
) -> str:
    """Generate copy_to_output.inc for prepare_halo_for_output()."""

    code = generate_header(yaml_hash)
    code += "/* Copy properties from struct Halo to struct HaloOutput\n"
    code += " * Used in prepare_halo_for_output(int filenr, int tree, const struct Halo *g, struct HaloOutput *o)\n"
    code += " */\n\n"

    code += "/* Halo properties */\n"
    for prop in halo_props:
        if not prop["output"]:
            continue

        output_source = prop.get("output_source", "copy_direct")
        name = prop["name"]
        type_info = TYPE_MAP[prop["type"]]

        if output_source == "custom":
            code += f"/* CUSTOM: {name} - see prepare_halo_for_output() for hand-written code */\n"

        elif output_source == "copy_direct":
            code += f"o->{name} = g->{name};\n"

        elif output_source == "copy_direct_array":
            if not type_info["is_array"]:
                raise ValueError(
                    f"Property '{name}' uses copy_direct_array but type is not array"
                )
            code += f"for (int j = 0; j < {type_info['array_size']}; j++) {{\n"
            code += f"  o->{name}[j] = g->{name}[j];\n"
            code += "}\n"

        elif output_source == "copy_from_tree":
            tree_field = prop["output_tree_field"]
            code += f"o->{name} = InputTreeHalos[g->HaloNr].{tree_field};\n"

        elif output_source == "copy_from_tree_array":
            tree_field = prop["output_tree_field"]
            if not type_info["is_array"]:
                raise ValueError(
                    f"Property '{name}' uses copy_from_tree_array but type is not array"
                )
            code += f"for (int j = 0; j < {type_info['array_size']}; j++) {{\n"
            code += f"  o->{name}[j] = InputTreeHalos[g->HaloNr].{tree_field}[j];\n"
            code += "}\n"

        elif output_source == "recalculate":
            func = prop["output_function"]
            arg = prop["output_function_arg"]
            code += f"o->{name} = {func}({arg});\n"

        elif output_source == "conditional":
            condition = prop["output_condition"]
            true_val = prop["output_true_value"]
            false_val = prop["output_false_value"]
            code += f"if ({condition}) {{\n"
            code += f"  o->{name} = {true_val};\n"
            code += "} else {\n"
            code += f"  o->{name} = {false_val};\n"
            code += "}\n"

    code += "\n/* Galaxy properties */\n"
    for prop in galaxy_props:
        if not prop["output"]:
            continue

        output_source = prop.get("output_source", "galaxy_property")
        name = prop["name"]

        if output_source == "galaxy_property":
            code += f"o->{name} = g->galaxy->{name};\n"

    return code


def generate_hdf5_field_count(
    halo_props: List[Dict], galaxy_props: List[Dict], yaml_hash: str
) -> str:
    """Generate hdf5_field_count.inc for HDF5 output."""

    n_output = sum(1 for p in halo_props if p["output"]) + sum(
        1 for p in galaxy_props if p["output"]
    )

    code = generate_header(yaml_hash)
    code += f"/* HDF5 field count and counter initialization */\n\n"
    code += f"HDF5_n_props = {n_output};\n"
    code += "int i = 0;\n"

    return code


def generate_hdf5_field_definitions(
    halo_props: List[Dict], galaxy_props: List[Dict], yaml_hash: str
) -> str:
    """Generate hdf5_field_definitions.inc for HDF5 output."""

    code = generate_header(yaml_hash)
    code += "/* HDF5 field definitions for calc_hdf5_props() */\n"
    code += "/* Requires: struct HaloOutput galout; */\n\n"

    for prop in halo_props:
        if not prop["output"]:
            continue

        name = prop["name"]
        type_info = TYPE_MAP[prop["type"]]
        h5_type = type_info["h5_type"]

        code += f"/* {name} */\n"
        code += f"HDF5_dst_offsets[i] = HOFFSET(struct HaloOutput, {name});\n"
        code += f"HDF5_dst_sizes[i] = sizeof(galout.{name});\n"
        code += f'HDF5_field_names[i] = "{name}";\n'
        code += f"HDF5_field_types[i++] = {h5_type};\n\n"

    for prop in galaxy_props:
        if not prop["output"]:
            continue

        name = prop["name"]
        type_info = TYPE_MAP[prop["type"]]
        h5_type = type_info["h5_type"]

        code += f"/* {name} */\n"
        code += f"HDF5_dst_offsets[i] = HOFFSET(struct HaloOutput, {name});\n"
        code += f"HDF5_dst_sizes[i] = sizeof(galout.{name});\n"
        code += f'HDF5_field_names[i] = "{name}";\n'
        code += f"HDF5_field_types[i++] = {h5_type};\n\n"

    return code


# ==============================================================================
# PYTHON CODE GENERATION
# ==============================================================================


def _generate_dtype_fields(halo_props: List[Dict], galaxy_props: List[Dict]) -> str:
    """Helper: Generate dtype field tuples for output properties."""
    fields = ""

    # Add all output properties (halo then galaxy)
    for prop in halo_props:
        if prop["output"]:
            numpy_type = TYPE_MAP[prop["type"]]["numpy_type"]
            fields += f'        ("{prop["name"]}", {numpy_type}),\n'

    for prop in galaxy_props:
        if prop["output"]:
            numpy_type = TYPE_MAP[prop["type"]]["numpy_type"]
            fields += f'        ("{prop["name"]}", {numpy_type}),\n'

    return fields


def generate_python_dtype(
    halo_props: List[Dict], galaxy_props: List[Dict], yaml_hash: str
) -> str:
    """Generate generated_dtype.py for Python plotting tools."""

    code = f'''"""AUTO-GENERATED CODE - DO NOT EDIT

Generated by: scripts/generate_properties.py
Generated on: {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}

Source files:
  - metadata/properties/halo_properties.yaml
  - metadata/properties/galaxy_properties.yaml

Source MD5: {yaml_hash}
To regenerate: make generate
"""

import numpy as np

def get_binary_dtype():
    """Return NumPy dtype for binary output format (with struct alignment)."""
    return np.dtype([
'''

    # Add dtype fields using helper
    code += _generate_dtype_fields(halo_props, galaxy_props)

    code += '''    ], align=True)

def get_hdf5_dtype():
    """Return NumPy dtype for HDF5 output format (no alignment)."""
    return np.dtype([
'''

    # Add dtype fields using helper (same fields as binary)
    code += _generate_dtype_fields(halo_props, galaxy_props)

    code += "    ])\n"

    return code


# ==============================================================================
# FILE I/O
# ==============================================================================


def ensure_dir(path: Path) -> None:
    """Ensure directory exists."""
    path.mkdir(parents=True, exist_ok=True)


def write_file(path: Path, content: str) -> None:
    """Write content to file."""
    with open(path, "w") as f:
        f.write(content)
    print(f"  Generated: {path.relative_to(REPO_ROOT)}")


# ==============================================================================
# VALIDATION MANIFEST GENERATION (for tests)
# ==============================================================================


def _prop_to_validation_entry(prop: Dict[str, Any]) -> Dict[str, Any]:
    """Convert a YAML property dict to a validation manifest entry.

    Supported optional fields in YAML:
      - range: [min, max] (inclusive)
      - sentinels: list of values to ignore for range checking (e.g., -1.0, 0.0)
    """
    prop_name = prop["name"]
    prop_type = prop["type"]

    entry: Dict[str, Any] = {
        "name": prop_name,
        "type": prop_type,
        "units": prop.get("units", ""),
        "is_vector": TYPE_MAP[prop_type].get("is_array", False),
    }

    # Optional inclusive range
    rng = prop.get("range")
    if rng is not None:
        if not isinstance(rng, list) or len(rng) != 2:
            raise ValueError(
                f"Property '{prop_name}' has invalid range; expected [min, max]"
            )

        # Validate that range values are numbers
        if not isinstance(rng[0], (int, float)) or not isinstance(rng[1], (int, float)):
            raise ValueError(
                f"Property '{prop_name}' range values must be numbers, got {rng}"
            )

        # Validate that min <= max
        if rng[0] > rng[1]:
            raise ValueError(
                f"Property '{prop_name}' range invalid: min ({rng[0]}) > max ({rng[1]})"
            )

        entry["range"] = rng

    # Optional sentinel values to be excluded from range checks
    sentinels = prop.get("sentinels")
    if sentinels is not None:
        if not isinstance(sentinels, list):
            raise ValueError(f"Property '{prop_name}' sentinels must be a list")

        # Validate sentinel types match property type
        is_numeric_type = prop_type in ["float", "double", "int", "long long"]
        if is_numeric_type:
            for s in sentinels:
                if not isinstance(s, (int, float)):
                    raise ValueError(
                        f"Property '{prop_name}' (type {prop_type}) has non-numeric sentinel: {s} (type {type(s).__name__})"
                    )

            # For integer types, warn if sentinels contain floats (might be unintended)
            if prop_type in ["int", "long long"]:
                for s in sentinels:
                    if isinstance(s, float) and s != int(s):
                        print(
                            f"WARNING: Property '{prop_name}' (type {prop_type}) has non-integer sentinel: {s}",
                            file=sys.stderr,
                        )

        entry["sentinels"] = sentinels

    return entry


def generate_validation_manifest(
    halo_props: List[Dict], galaxy_props: List[Dict]
) -> str:
    """Generate a JSON manifest consumed by scientific tests for range checks.

    Only includes properties with output=true so tests validate exactly what is written.
    """
    props: Dict[str, Any] = {}

    # Halo output properties
    for prop in halo_props:
        if prop.get("output", False):
            props[prop["name"]] = _prop_to_validation_entry(prop)

    # Galaxy output properties
    for prop in galaxy_props:
        if prop.get("output", False):
            props[prop["name"]] = _prop_to_validation_entry(prop)

    manifest = {
        "schema_version": 1,
        "generated_at": datetime.now().isoformat(timespec="seconds"),
        "properties": props,
        "notes": "Auto-generated from metadata/properties/*.yaml. Range is inclusive; sentinels are exempt.",
    }

    return json.dumps(manifest, indent=2)


# ==============================================================================
# MAIN
# ==============================================================================


def main():
    """Main entry point."""

    print("=" * 70)
    print("Property Code Generator for Mimic")
    print("=" * 70)
    print()

    # Check input files exist
    if not HALO_PROPERTIES_YAML.exists():
        print(f"ERROR: {HALO_PROPERTIES_YAML} not found", file=sys.stderr)
        print(
            "Expected location: metadata/properties/halo_properties.yaml",
            file=sys.stderr,
        )
        sys.exit(1)

    if not GALAXY_PROPERTIES_YAML.exists():
        print(f"ERROR: {GALAXY_PROPERTIES_YAML} not found", file=sys.stderr)
        print(
            "Expected location: metadata/properties/galaxy_properties.yaml",
            file=sys.stderr,
        )
        sys.exit(1)

    print("Reading property metadata...")
    print(f"  Halo properties: {HALO_PROPERTIES_YAML.relative_to(REPO_ROOT)}")
    print(f"  Galaxy properties: {GALAXY_PROPERTIES_YAML.relative_to(REPO_ROOT)}")
    print()

    # Compute YAML hash for validation
    yaml_hash = compute_yaml_hash()

    # Load YAML
    with open(HALO_PROPERTIES_YAML) as f:
        halo_data = yaml.safe_load(f)
    with open(GALAXY_PROPERTIES_YAML) as f:
        galaxy_data = yaml.safe_load(f)

    halo_props = halo_data.get("halo_properties", [])
    galaxy_props = galaxy_data.get("galaxy_properties", [])

    # Validate
    print("Validating property definitions...")
    try:
        validate_properties(halo_props, galaxy_props)
    except ValueError as e:
        print(f"VALIDATION ERROR: {e}", file=sys.stderr)
        sys.exit(1)
    print()

    # Generate code
    print("Generating code...")

    # Ensure output directories exist
    ensure_dir(GENERATED_DIR)
    ensure_dir(PLOT_GENERATED_DIR)
    ensure_dir(TESTS_GENERATED_DIR)

    # C header files
    write_file(
        GENERATED_DIR / "property_defs.h",
        generate_property_defs_h(halo_props, galaxy_props, yaml_hash),
    )

    # C initialization files
    write_file(
        GENERATED_DIR / "init_halo_properties.inc",
        generate_init_halo_properties(halo_props, yaml_hash),
    )
    write_file(
        GENERATED_DIR / "init_galaxy_properties.inc",
        generate_init_galaxy_properties(galaxy_props, yaml_hash),
    )

    # C output files
    write_file(
        GENERATED_DIR / "copy_to_output.inc",
        generate_copy_to_output(halo_props, galaxy_props, yaml_hash),
    )
    write_file(
        GENERATED_DIR / "hdf5_field_count.inc",
        generate_hdf5_field_count(halo_props, galaxy_props, yaml_hash),
    )
    write_file(
        GENERATED_DIR / "hdf5_field_definitions.inc",
        generate_hdf5_field_definitions(halo_props, galaxy_props, yaml_hash),
    )

    # Python dtype
    write_file(
        PLOT_GENERATED_DIR / "dtype.py",
        generate_python_dtype(halo_props, galaxy_props, yaml_hash),
    )

    # Python package init file
    init_py_content = f'''"""AUTO-GENERATED CODE - DO NOT EDIT

Generated by: scripts/generate_properties.py
Generated on: {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}

Source files:
  - metadata/properties/halo_properties.yaml
  - metadata/properties/galaxy_properties.yaml

Source MD5: {yaml_hash}

This package provides generated data types for reading Mimic output files.
To regenerate: make generate
"""
'''
    write_file(PLOT_GENERATED_DIR / "__init__.py", init_py_content)

    # Validation manifest for tests
    write_file(
        TESTS_GENERATED_DIR / "property_ranges.json",
        generate_validation_manifest(halo_props, galaxy_props),
    )

    print()
    print("=" * 70)
    print("✓ Code generation complete!")
    print("=" * 70)
    print()
    print("Generated files:")
    print("  C headers:       src/include/generated/property_defs.h")
    print("  C init code:     src/include/generated/init_*_properties.inc")
    print("  C output code:   src/include/generated/copy_to_output.inc")
    print("  HDF5 code:       src/include/generated/hdf5_*.inc")
    print("  Python dtype:    output/mimic-plot/generated/dtype.py")
    print()
    print("Next steps:")
    print("  1. Review generated files")
    print("  2. Update source files to include generated code")
    print("  3. Test compilation: make")
    print("  4. Run validation: make check-generated")
    print()


if __name__ == "__main__":
    main()
