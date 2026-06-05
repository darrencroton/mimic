#!/usr/bin/env python3
"""
Property Code Generator for Mimic

Generates C structures, initialization code, output code, and schema writers from
YAML property metadata definitions. This eliminates manual synchronization across
8+ files and enables rapid property addition (<2 minutes vs 30 minutes).

Usage:
    MODEL=sage python3 scripts/generate_properties.py

Reads:
    src/core/core_properties.yaml
    simulations/*/halo_properties.yaml
    models/<MODEL>/model_properties.yaml

Generates:
    src/include/generated/property_defs.h
    src/include/generated/init_halo_properties.inc
    src/include/generated/init_galaxy_properties.inc
    src/include/generated/copy_to_output.inc
    src/include/generated/hdf5_field_count.inc
    src/include/generated/hdf5_field_definitions.inc
    src/include/generated/output_schema_writer.inc

Author: Property Metadata System (Phase 1)
Date: 2025-11-07
"""

import hashlib
import json
import os
import sys
from pathlib import Path
from typing import Any, Dict, List

try:
    import yaml
except ImportError:
    print("ERROR: PyYAML not installed. Run: pip install PyYAML", file=sys.stderr)
    sys.exit(1)

from discovery import (
    REPO_ROOT,
    core_property_files,
    halo_property_files,
    model_property_files,
    rel,
    test_property_files,
)

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
VALID_OUTPUT_TRANSFORMS = [
    "log10",
]

# ==============================================================================
# PATHS
# ==============================================================================

# Input YAML files are discovered from package roots. Legacy paths remain
# supported through scripts/discovery.py during migration.

# Output directories
GENERATED_DIR = REPO_ROOT / "src" / "include" / "generated"
TESTS_GENERATED_DIR = REPO_ROOT / "tests" / "generated"
BUILD_GENERATED_DIR = REPO_ROOT / "build" / "generated"

# Hash tracking file
PROPERTY_HASH_FILE = BUILD_GENERATED_DIR / "property_hash.txt"

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

    # Check output_transform if specified
    if "output_transform" in prop:
        if prop["output_transform"] not in VALID_OUTPUT_TRANSFORMS:
            raise ValueError(
                f"Invalid output_transform '{prop['output_transform']}' for '{name}'. "
                f"Must be one of: {VALID_OUTPUT_TRANSFORMS}"
            )

    # Check init_repeat (only for galaxy properties)
    if "init_repeat" in prop:
        if category != "galaxy":
            raise ValueError(
                f"Property '{name}' has init_repeat but is not a galaxy property. "
                "init_repeat is only supported for galaxy properties."
            )

        if not isinstance(prop["init_repeat"], bool):
            raise ValueError(
                f"Property '{name}' has invalid init_repeat value '{prop['init_repeat']}'. "
                "Must be a boolean (true/false)."
            )

        if prop.get("init_source") != "default":
            raise ValueError(
                f"Property '{name}' has init_repeat but init_source is not 'default'. "
                "init_repeat only applies to properties with init_source: default."
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
    """Compute MD5 hash of property-generation inputs for validation."""
    md5 = hashlib.md5()

    generator_path = Path(__file__)
    md5.update(rel(generator_path).encode("utf-8"))
    md5.update(generator_path.read_bytes())

    # Hash all YAML files in stable generation order.
    for yaml_file in halo_property_files() + model_property_files() + test_property_files():
        with open(yaml_file, "rb") as f:
            md5.update(rel(yaml_file).encode("utf-8"))
            md5.update(f.read())

    return md5.hexdigest()


def load_saved_hash() -> str:
    """Load the previously saved hash from disk.

    Returns:
        The saved hash, or empty string if not found.
    """
    if PROPERTY_HASH_FILE.exists():
        return PROPERTY_HASH_FILE.read_text().strip()
    return ""


def save_hash(yaml_hash: str) -> None:
    """Save the current hash to disk for future comparison.

    Args:
        yaml_hash: The MD5 hash to save.
    """
    ensure_dir(BUILD_GENERATED_DIR)
    PROPERTY_HASH_FILE.write_text(yaml_hash + "\n")


def generate_header(yaml_hash: str):
    """Generate common header for all generated files."""
    selected_model = os.environ.get("MODEL") or "<MODEL>"
    source_lines = "\n".join(
        f" *   - {rel(path)}" for path in halo_property_files() + model_property_files() + test_property_files()
    )
    return f"""/* AUTO-GENERATED CODE - DO NOT EDIT
 *
 * Generated by: scripts/generate_properties.py
 *
 * Source files:
{source_lines}
 *
 * Source MD5: {yaml_hash}
 * To regenerate: make MODEL={selected_model} generate
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

    code += "/* Driver-neutral payload for initializing halo properties */\n"
    code += "struct HaloInitPayload {\n"
    for prop in halo_props:
        init_source = prop.get("init_source", "skip")
        if init_source not in ("copy_from_tree", "copy_from_tree_array", "calculate"):
            continue
        type_info = TYPE_MAP[prop["type"]]
        c_type = type_info["c_type"]
        array_suffix = type_info.get("c_array", "")
        code += f"  {c_type} {prop['name']}{array_suffix};\n"
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

    code += (
        "static inline void init_halo_from_payload(struct Halo *halo, "
        "const struct HaloInitPayload *payload) {\n"
    )
    for prop in halo_props:
        init_source = prop.get("init_source", "skip")
        name = prop["name"]
        type_info = TYPE_MAP[prop["type"]]

        if init_source == "skip":
            continue
        if init_source == "default":
            init_value = prop["init_value"]
            code += f"  halo->{name} = {init_value};\n"
        elif init_source in ("copy_from_tree", "calculate"):
            code += f"  halo->{name} = payload->{name};\n"
        elif init_source == "copy_from_tree_array":
            if not type_info["is_array"]:
                raise ValueError(
                    f"Property '{name}' uses copy_from_tree_array but type is not array"
                )
            code += f"  for (int j = 0; j < {type_info['array_size']}; j++) {{\n"
            code += f"    halo->{name}[j] = payload->{name}[j];\n"
            code += "  }\n"
    code += "}\n\n"

    code += "static inline void init_galaxy_defaults(struct GalaxyData *galaxy) {\n"
    for prop in galaxy_props:
        init_source = prop.get("init_source", "default")
        name = prop["name"]

        if init_source == "default":
            init_value = prop.get("init_value", "0.0")
            code += f"  galaxy->{name} = {init_value};\n"
    code += "}\n\n"

    repeated_props = [
        prop for prop in galaxy_props if prop.get("init_repeat", False)
    ]
    code += "static inline void reset_galaxy_snapshot_accumulators(struct GalaxyData *galaxy) {\n"
    for prop in repeated_props:
        name = prop["name"]
        init_value = prop.get("init_value", "0.0")
        code += f"  galaxy->{name} = {init_value};\n"
    code += "}\n\n"

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


def generate_populate_halo_payload_from_tree(
    halo_props: List[Dict], yaml_hash: str
) -> str:
    """Generate populate_halo_payload_from_tree.inc.

    Fills a local `struct HaloInitPayload payload` for descendant `halonr` from
    the tree-ordered input catalog. Included inside the tree driver's
    make_halo_init_payload() (build_model.c).

    This is the tree-driver-specific half of the gather/inherit split: it is the
    only place where tree-index coupling (InputTreeHalos, virial helpers) touches
    halo initialization. It is generated so that the payload is populated directly
    from metadata and can never silently desync from struct HaloInitPayload --
    both this populator and the struct iterate the identical init_source filter
    (copy_from_tree / copy_from_tree_array / calculate), so adding a new
    tree-sourced halo property updates both at once. The format-neutral
    consumer (init_halo_from_payload in property_defs.h) carries no tree coupling.
    """

    code = generate_header(yaml_hash)
    code += "/* Populate `payload` for descendant `halonr` from the tree input.\n"
    code += " *\n"
    code += " * Mirrors struct HaloInitPayload (init_source in copy_from_tree /\n"
    code += " * copy_from_tree_array / calculate). Included in a function body that\n"
    code += " * declares `struct HaloInitPayload payload;`. */\n\n"

    for prop in halo_props:
        init_source = prop.get("init_source", "skip")
        name = prop["name"]
        type_info = TYPE_MAP[prop["type"]]

        if init_source == "copy_from_tree":
            code += f"payload.{name} = InputTreeHalos[halonr].{name};\n"

        elif init_source == "copy_from_tree_array":
            if not type_info["is_array"]:
                raise ValueError(
                    f"Property '{name}' uses copy_from_tree_array but type is not array"
                )
            code += f"for (int j = 0; j < {type_info['array_size']}; j++) {{\n"
            code += f"  payload.{name}[j] = InputTreeHalos[halonr].{name}[j];\n"
            code += "}\n"

        elif init_source == "calculate":
            func = prop["init_function"]
            code += f"payload.{name} = {func}(halonr);\n"

    return code


def _test_seed_value(prop: Dict[str, Any]) -> str:
    """Return a non-init value for generated property tests."""
    init_value = prop.get("init_value", "0")
    prop_type = prop["type"]
    if prop_type in {"float", "vec3_float"}:
        return f"((float)({init_value}) + 123.25f)"
    if prop_type == "double":
        return f"((double)({init_value}) + 123.25)"
    if prop_type == "long long":
        return f"((long long)({init_value}) + 123LL)"
    return f"((int)({init_value}) + 123)"


def _test_init_compare_expr(accessor: str, prop: Dict[str, Any]) -> str:
    """Return a C expression comparing a generated field with its init value."""
    init_value = prop.get("init_value", "0")
    prop_type = prop["type"]
    if prop_type in {"float", "vec3_float"}:
        return f"fabsf({accessor} - (float)({init_value})) <= 1.0e-6f"
    if prop_type == "double":
        return f"fabs({accessor} - (double)({init_value})) <= 1.0e-12"
    if prop_type == "long long":
        return f"{accessor} == (long long)({init_value})"
    return f"{accessor} == (int)({init_value})"


def generate_property_test_helpers(galaxy_props: List[Dict], yaml_hash: str) -> str:
    """Generate generic helpers for hand-written property unit tests."""
    default_props = [
        prop for prop in galaxy_props if prop.get("init_source", "default") == "default"
    ]
    reset_props = [
        prop for prop in galaxy_props if prop.get("init_repeat", False) is True
    ]

    code = generate_header(yaml_hash)
    code += "#ifndef GENERATED_PROPERTY_TEST_HELPERS_H\n"
    code += "#define GENERATED_PROPERTY_TEST_HELPERS_H\n\n"
    code += "#include <math.h>\n"
    code += '#include "property_defs.h"\n\n'
    code += f"#define GENERATED_GALAXY_PROPERTY_COUNT {len(galaxy_props)}\n"
    code += f"#define GENERATED_DEFAULT_GALAXY_PROPERTY_COUNT {len(default_props)}\n"
    code += f"#define GENERATED_INIT_REPEAT_PROPERTY_COUNT {len(reset_props)}\n\n"

    code += "static inline void generated_test_seed_default_galaxy_properties(struct GalaxyData *galaxy) {\n"
    code += "  (void)galaxy;\n"
    for prop in default_props:
        name = prop["name"]
        type_info = TYPE_MAP[prop["type"]]
        seed = _test_seed_value(prop)
        if type_info["is_array"]:
            code += f"  for (int j = 0; j < {type_info['array_size']}; j++) {{\n"
            code += f"    galaxy->{name}[j] = {seed};\n"
            code += "  }\n"
        else:
            code += f"  galaxy->{name} = {seed};\n"
    code += "}\n\n"

    code += "static inline int generated_test_default_galaxy_properties_equal_init(const struct GalaxyData *galaxy) {\n"
    code += "  (void)galaxy;\n"
    code += "  int ok = 1;\n"
    for prop in default_props:
        name = prop["name"]
        type_info = TYPE_MAP[prop["type"]]
        if type_info["is_array"]:
            code += f"  for (int j = 0; j < {type_info['array_size']}; j++) {{\n"
            code += f"    ok = ok && ({_test_init_compare_expr(f'galaxy->{name}[j]', prop)});\n"
            code += "  }\n"
        else:
            code += f"  ok = ok && ({_test_init_compare_expr(f'galaxy->{name}', prop)});\n"
    code += "  return ok;\n"
    code += "}\n\n"

    code += "static inline void generated_test_seed_init_repeat_properties(struct GalaxyData *galaxy) {\n"
    code += "  (void)galaxy;\n"
    for prop in reset_props:
        name = prop["name"]
        type_info = TYPE_MAP[prop["type"]]
        seed = _test_seed_value(prop)
        if type_info["is_array"]:
            code += f"  for (int j = 0; j < {type_info['array_size']}; j++) {{\n"
            code += f"    galaxy->{name}[j] = {seed};\n"
            code += "  }\n"
        else:
            code += f"  galaxy->{name} = {seed};\n"
    code += "}\n\n"

    code += "static inline int generated_test_init_repeat_properties_equal_init(const struct GalaxyData *galaxy) {\n"
    code += "  (void)galaxy;\n"
    code += "  int ok = 1;\n"
    for prop in reset_props:
        name = prop["name"]
        type_info = TYPE_MAP[prop["type"]]
        if type_info["is_array"]:
            code += f"  for (int j = 0; j < {type_info['array_size']}; j++) {{\n"
            code += f"    ok = ok && ({_test_init_compare_expr(f'galaxy->{name}[j]', prop)});\n"
            code += "  }\n"
        else:
            code += f"  ok = ok && ({_test_init_compare_expr(f'galaxy->{name}', prop)});\n"
    code += "  return ok;\n"
    code += "}\n\n"
    code += "#endif /* GENERATED_PROPERTY_TEST_HELPERS_H */\n"
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

        # Apply unit conversion if output_convert is specified
        # Skip if output_source is custom (custom code handles its own conversions)
        if output_source != "custom" and "output_convert" in prop:
            conversion_expr = prop["output_convert"]
            sentinels = prop.get("sentinels", [])
            c_type = type_info["c_type"]

            # Determine type suffix for sentinel comparisons (f for float, nothing for double/int)
            if prop["type"] == "float":
                type_suffix = "f"
            else:
                type_suffix = ""

            if sentinels:
                # Generate conditional conversion (skip sentinels)
                # Build condition: value != sentinel1 && value != sentinel2 && ...
                conditions = [f"o->{name} != {s}{type_suffix}" for s in sentinels]
                condition_str = " && ".join(conditions)
                code += f"if ({condition_str}) {{\n"
                code += f"  o->{name} *= {conversion_expr};\n"
                code += "}\n"
            else:
                # Unconditional conversion
                code += f"o->{name} *= {conversion_expr};\n"

        # Apply output transform if specified (e.g., log10)
        # Transform is applied AFTER unit conversion
        if output_source != "custom" and "output_transform" in prop:
            transform = prop["output_transform"]
            sentinels = prop.get("sentinels", [])

            # Determine type suffix for sentinel comparisons
            if prop["type"] == "float":
                type_suffix = "f"
            else:
                type_suffix = ""

            if transform == "log10":
                if sentinels:
                    # Generate conditional transform (skip sentinels like 0.0 to avoid log10(0) = -inf)
                    conditions = [f"o->{name} != {s}{type_suffix}" for s in sentinels]
                    condition_str = " && ".join(conditions)
                    code += f"if ({condition_str}) {{\n"
                    code += f"  o->{name} = log10(o->{name});\n"
                    code += "}\n"
                else:
                    # Unconditional transform
                    code += f"o->{name} = log10(o->{name});\n"

    code += "\n/* Galaxy properties */\n"
    for prop in galaxy_props:
        if not prop["output"]:
            continue

        output_source = prop.get("output_source", "galaxy_property")
        name = prop["name"]
        type_info = TYPE_MAP[prop["type"]]

        if output_source == "galaxy_property":
            code += f"o->{name} = g->galaxy->{name};\n"

            # Apply unit conversion if output_convert is specified
            if "output_convert" in prop:
                conversion_expr = prop["output_convert"]
                sentinels = prop.get("sentinels", [])
                c_type = type_info["c_type"]

                # Determine type suffix for sentinel comparisons
                if prop["type"] == "float":
                    type_suffix = "f"
                else:
                    type_suffix = ""

                if sentinels:
                    # Generate conditional conversion (skip sentinels)
                    conditions = [f"o->{name} != {s}{type_suffix}" for s in sentinels]
                    condition_str = " && ".join(conditions)
                    code += f"if ({condition_str}) {{\n"
                    code += f"  o->{name} *= {conversion_expr};\n"
                    code += "}\n"
                else:
                    # Unconditional conversion
                    code += f"o->{name} *= {conversion_expr};\n"

            # Apply output transform if specified (e.g., log10)
            # Transform is applied AFTER unit conversion
            if "output_transform" in prop:
                transform = prop["output_transform"]
                sentinels = prop.get("sentinels", [])

                # Determine type suffix for sentinel comparisons
                if prop["type"] == "float":
                    type_suffix = "f"
                else:
                    type_suffix = ""

                if transform == "log10":
                    if sentinels:
                        # Generate conditional transform (skip sentinels like 0.0 to avoid log10(0) = -inf)
                        conditions = [
                            f"o->{name} != {s}{type_suffix}" for s in sentinels
                        ]
                        condition_str = " && ".join(conditions)
                        code += f"if ({condition_str}) {{\n"
                        code += f"  o->{name} = log10(o->{name});\n"
                        code += "}\n"
                    else:
                        # Unconditional transform
                        code += f"o->{name} = log10(o->{name});\n"

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


def generate_hdf5_field_metadata(
    halo_props: List[Dict], galaxy_props: List[Dict], yaml_hash: str
) -> str:
    """Generate hdf5_field_metadata.inc for writing FieldMetadata dataset.

    This generates C code that creates a separate HDF5 dataset containing
    field names, units, and descriptions as a structured table. This makes
    property metadata discoverable and easily queryable.
    """

    code = generate_header(yaml_hash)
    code += "/* HDF5 FieldMetadata dataset - structured table of field names, units, and descriptions */\n"
    code += "/* Provides discoverable, queryable metadata for all output fields */\n\n"

    # Collect all output properties
    all_props = []
    for prop in halo_props:
        if prop["output"]:
            all_props.append(
                (prop["name"], prop.get("units", ""), prop.get("description", ""))
            )
    for prop in galaxy_props:
        if prop["output"]:
            all_props.append(
                (prop["name"], prop.get("units", ""), prop.get("description", ""))
            )

    num_fields = len(all_props)

    code += f"/* Total number of output fields */\n"
    code += f"#define NUM_FIELDS {num_fields}\n\n"

    # Define the metadata structure
    code += "/* Define metadata structure */\n"
    code += "struct FieldMetadata {\n"
    code += "  char field_name[64];\n"
    code += "  char units[128];\n"
    code += "  char description[256];\n"
    code += "};\n\n"

    # Create and populate the metadata array
    code += "/* Create metadata array */\n"
    code += "struct FieldMetadata field_metadata[NUM_FIELDS] = {\n"
    for name, units, description in all_props:
        # Escape quotes in strings
        units_escaped = units.replace('"', '\\"')
        desc_escaped = description.replace('"', '\\"')
        code += f'  {{"{name}", "{units_escaped}", "{desc_escaped}"}},\n'
    code += "};\n\n"

    # Create HDF5 compound datatype
    code += "/* Create HDF5 compound datatype for metadata */\n"
    code += "hid_t string_type_field = H5Tcopy(H5T_C_S1);\n"
    code += "H5Tset_size(string_type_field, 64);\n"
    code += "hid_t string_type_units = H5Tcopy(H5T_C_S1);\n"
    code += "H5Tset_size(string_type_units, 128);\n"
    code += "hid_t string_type_desc = H5Tcopy(H5T_C_S1);\n"
    code += "H5Tset_size(string_type_desc, 256);\n\n"
    code += (
        "hid_t metadata_tid = H5Tcreate(H5T_COMPOUND, sizeof(struct FieldMetadata));\n"
    )
    code += 'H5Tinsert(metadata_tid, "field_name", HOFFSET(struct FieldMetadata, field_name), string_type_field);\n'
    code += 'H5Tinsert(metadata_tid, "units", HOFFSET(struct FieldMetadata, units), string_type_units);\n'
    code += 'H5Tinsert(metadata_tid, "description", HOFFSET(struct FieldMetadata, description), string_type_desc);\n\n'

    # Create field name array for Table API
    code += "/* Create field name and offset arrays for Table API */\n"
    code += 'const char *metadata_field_names[3] = {"field_name", "units", "description"};\n'
    code += "size_t metadata_field_offsets[3] = {\n"
    code += "  HOFFSET(struct FieldMetadata, field_name),\n"
    code += "  HOFFSET(struct FieldMetadata, units),\n"
    code += "  HOFFSET(struct FieldMetadata, description)\n"
    code += "};\n"
    code += "hid_t metadata_field_types[3] = {string_type_field, string_type_units, string_type_desc};\n\n"

    # Create and write dataset using Table API (adds CLASS and FIELD_N_NAME attributes)
    code += "/* Create and write FieldMetadata as HDF5 Table for discoverability */\n"
    code += '/* This adds CLASS="TABLE" and FIELD_N_NAME attributes for better tool support */\n'
    code += "herr_t metadata_status = H5TBmake_table(\n"
    code += '    "Field Metadata",           /* Table title */\n'
    code += "    group_id,                    /* Parent group */\n"
    code += '    "FieldMetadata",             /* Dataset name */\n'
    code += "    3,                           /* Number of fields */\n"
    code += "    NUM_FIELDS,                  /* Number of records */\n"
    code += "    sizeof(struct FieldMetadata), /* Record size */\n"
    code += "    metadata_field_names,        /* Field names */\n"
    code += "    metadata_field_offsets,      /* Field offsets */\n"
    code += "    metadata_field_types,        /* Field types */\n"
    code += "    1000,                        /* Chunk size */\n"
    code += "    NULL,                        /* Fill data */\n"
    code += "    0,                           /* Compress */\n"
    code += "    field_metadata               /* Data */\n"
    code += ");\n"
    code += "if (metadata_status < 0) {\n"
    code += '  FATAL_ERROR("Failed to create FieldMetadata table for HDF5 output");\n'
    code += "}\n\n"

    # Cleanup (no need to close dataset - H5TBmake_table handles it)
    code += "/* Cleanup metadata resources */\n"
    code += "H5Tclose(metadata_tid);\n"
    code += "H5Tclose(string_type_field);\n"
    code += "H5Tclose(string_type_units);\n"
    code += "H5Tclose(string_type_desc);\n"

    return code


# ==============================================================================
# OUTPUT SCHEMA CODE GENERATION
# ==============================================================================


NUMPY_TYPE_NAMES = {
    "int": "int32",
    "float": "float32",
    "double": "float64",
    "long long": "int64",
    "vec3_float": "float32",
    "vec3_int": "int32",
}


def _json_string(value: Any) -> str:
    """Return a JSON string literal suitable for embedding in generated C."""
    json_literal = json.dumps("" if value is None else str(value))
    return json.dumps(json_literal)[1:-1]


def _schema_shape(prop: Dict[str, Any]) -> str:
    type_info = TYPE_MAP[prop["type"]]
    if type_info.get("is_array"):
        return f"[{type_info['array_size']}]"
    return "[]"


def generate_output_schema_writer(
    halo_props: List[Dict], galaxy_props: List[Dict], yaml_hash: str
) -> str:
    """Generate C statements that write the run-local output schema JSON."""

    code = generate_header(yaml_hash)
    code += "/* Writes metadata/output_schema.json for binary readers and plotting. */\n"
    code += "fprintf(schema_file, \"{\\n\");\n"
    code += "fprintf(schema_file, \"  \\\"schema_version\\\": 1,\\n\");\n"
    code += (
        "fprintf(schema_file, \"  \\\"generated_by\\\": "
        "\\\"scripts/generate_properties.py\\\",\\n\");\n"
    )
    code += (
        f"fprintf(schema_file, \"  \\\"source_md5\\\": "
        f"\\\"{yaml_hash}\\\",\\n\");\n"
    )
    code += (
        "fprintf(schema_file, \"  \\\"model\\\": \\\"%s\\\",\\n\", "
        "MIMIC_COMPILED_MODEL);\n"
    )
    code += (
        "fprintf(schema_file, \"  \\\"model_path\\\": \\\"%s\\\",\\n\", "
        "MIMIC_COMPILED_MODEL_PATH);\n"
    )
    code += "fprintf(schema_file, \"  \\\"record\\\": {\\n\");\n"
    code += "fprintf(schema_file, \"    \\\"c_struct\\\": \\\"HaloOutput\\\",\\n\");\n"
    code += (
        "fprintf(schema_file, \"    \\\"binary_record_size\\\": %zu,\\n\", "
        "sizeof(struct HaloOutput));\n"
    )
    code += "fprintf(schema_file, \"    \\\"byte_order\\\": \\\"native\\\",\\n\");\n"
    code += (
        "fprintf(schema_file, \"    \\\"alignment\\\": "
        "\\\"native_c_compiler\\\"\\n\");\n"
    )
    code += "fprintf(schema_file, \"  },\\n\");\n"
    code += "fprintf(schema_file, \"  \\\"fields\\\": [\\n\");\n"

    fields = [("halo", prop) for prop in halo_props if prop["output"]]
    fields += [("galaxy", prop) for prop in galaxy_props if prop["output"]]

    for idx, (category, prop) in enumerate(fields):
        comma = "," if idx < len(fields) - 1 else ""
        name = prop["name"]
        code += "fprintf(schema_file, \"    {\\n\");\n"
        code += (
            f"fprintf(schema_file, \"      \\\"name\\\": "
            f"{_json_string(name)},\\n\");\n"
        )
        code += (
            f"fprintf(schema_file, \"      \\\"category\\\": "
            f"{_json_string(category)},\\n\");\n"
        )
        code += (
            f"fprintf(schema_file, \"      \\\"c_type\\\": "
            f"{_json_string(TYPE_MAP[prop['type']]['c_type'])},\\n\");\n"
        )
        code += (
            f"fprintf(schema_file, \"      \\\"metadata_type\\\": "
            f"{_json_string(prop['type'])},\\n\");\n"
        )
        code += (
            f"fprintf(schema_file, \"      \\\"numpy_type\\\": "
            f"{_json_string(NUMPY_TYPE_NAMES[prop['type']])},\\n\");\n"
        )
        code += (
            f"fprintf(schema_file, \"      \\\"shape\\\": {_schema_shape(prop)},\\n\");\n"
        )
        code += (
            "fprintf(schema_file, \"      \\\"offset\\\": %zu,\\n\", "
            f"offsetof(struct HaloOutput, {name}));\n"
        )
        code += (
            "fprintf(schema_file, \"      \\\"size\\\": %zu,\\n\", "
            f"sizeof(((struct HaloOutput *)0)->{name}));\n"
        )
        code += (
            f"fprintf(schema_file, \"      \\\"units\\\": "
            f"{_json_string(prop.get('units', ''))},\\n\");\n"
        )
        code += (
            f"fprintf(schema_file, \"      \\\"description\\\": "
            f"{_json_string(prop.get('description', ''))}\\n\");\n"
        )
        code += f"fprintf(schema_file, \"    }}{comma}\\n\");\n"

    code += "fprintf(schema_file, \"  ]\\n\");\n"
    code += "fprintf(schema_file, \"}\\n\");\n"
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


def _prop_to_validation_entry(prop: Dict[str, Any], category: str) -> Dict[str, Any]:
    """Convert a YAML property dict to a validation manifest entry.

    Supported optional fields in YAML:
      - range: [min, max] (inclusive)
      - sentinels: list of values to ignore for range checking (e.g., -1.0, 0.0)
    """
    prop_name = prop["name"]
    prop_type = prop["type"]

    entry: Dict[str, Any] = {
        "name": prop_name,
        "category": category,
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
    halo_props: List[Dict], galaxy_props: List[Dict], yaml_hash: str
) -> str:
    """Generate a JSON manifest consumed by scientific tests for range checks.

    Only includes properties with output=true so tests validate exactly what is written.
    """
    props: Dict[str, Any] = {}

    # Halo output properties
    for prop in halo_props:
        if prop.get("output", False):
            props[prop["name"]] = _prop_to_validation_entry(prop, "halo")

    # Galaxy output properties
    for prop in galaxy_props:
        if prop.get("output", False):
            props[prop["name"]] = _prop_to_validation_entry(prop, "galaxy")

    manifest = {
        "_metadata": {
            "auto_generated": True,
            "generated_by": "scripts/generate_properties.py",
            "source_files": [
                rel(path) for path in halo_property_files() + model_property_files() + test_property_files()
            ],
            "source_md5": yaml_hash,
            "regenerate": f"make MODEL={os.environ.get('MODEL', 'sage')} generate",
        },
        "schema_version": 1,
        "properties": props,
        "notes": "Auto-generated from package property metadata. Range is inclusive; sentinels are exempt.",
    }

    return json.dumps(manifest, indent=2)


def load_property_package(path: Path, key: str) -> List[Dict[str, Any]]:
    """Load one property YAML package."""
    with open(path, encoding="utf-8") as f:
        data = yaml.safe_load(f) or {}
    props = data.get(key, [])
    if not isinstance(props, list):
        raise ValueError(f"{rel(path)}: '{key}' must be a list")
    return props


def merge_property_packages(paths: List[Path], key: str) -> List[Dict[str, Any]]:
    """Merge property packages, allowing only exact duplicate definitions."""
    merged: List[Dict[str, Any]] = []
    by_name: Dict[str, Dict[str, Any]] = {}

    for path in paths:
        for prop in load_property_package(path, key):
            name = prop.get("name")
            if not name:
                raise ValueError(f"{rel(path)}: property missing required field 'name'")

            existing_prop = by_name.get(name)
            if existing_prop is None:
                by_name[name] = prop
                merged.append(prop)
                continue

            if existing_prop != prop:
                raise ValueError(
                    f"Incompatible duplicate property '{name}' in {rel(path)}"
                )

    return merged


# ==============================================================================
# MAIN
# ==============================================================================


def main():
    """Main entry point."""

    print("=" * 70)
    print("Property Code Generator for Mimic")
    print("=" * 70)
    print()

    halo_yaml_files = halo_property_files()
    # Test builds append fixture-owned galaxy properties (e.g. TestDummyProperty);
    # test_property_files() is empty for production builds.
    galaxy_yaml_files = model_property_files() + test_property_files()
    core_yaml_files = core_property_files()

    if not core_yaml_files:
        print("ERROR: no core property metadata found", file=sys.stderr)
        print("Expected location: src/core/core_properties.yaml", file=sys.stderr)
        sys.exit(1)

    if not galaxy_yaml_files:
        print("ERROR: no model property metadata found", file=sys.stderr)
        print("Expected model package file: models/<model>/model_properties.yaml", file=sys.stderr)
        sys.exit(1)

    print("Reading property metadata...")
    for path in halo_yaml_files:
        print(f"  Halo properties: {rel(path)}")
    for path in galaxy_yaml_files:
        print(f"  Galaxy properties: {rel(path)}")
    print()

    # Compute YAML hash for validation
    yaml_hash = compute_yaml_hash()

    # Check if regeneration is needed
    saved_hash = load_saved_hash()
    if saved_hash == yaml_hash:
        print("✓ Property metadata unchanged - skipping regeneration")
        print(f"  Hash: {yaml_hash}")
        print()
        return 0

    # Hash mismatch or missing - regeneration needed
    if saved_hash:
        print(f"Property metadata changed - regenerating...")
        print(f"  Old hash: {saved_hash}")
        print(f"  New hash: {yaml_hash}")
    else:
        print("No previous hash found - generating for first time...")
        print(f"  Hash: {yaml_hash}")
    print()

    # Load YAML
    try:
        halo_props = merge_property_packages(halo_yaml_files, "halo_properties")
        galaxy_props = merge_property_packages(galaxy_yaml_files, "galaxy_properties")
    except ValueError as e:
        print(f"VALIDATION ERROR: {e}", file=sys.stderr)
        sys.exit(1)

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
    ensure_dir(TESTS_GENERATED_DIR)
    ensure_dir(BUILD_GENERATED_DIR)

    # C header files
    write_file(
        GENERATED_DIR / "property_defs.h",
        generate_property_defs_h(halo_props, galaxy_props, yaml_hash),
    )

    # C initialization files
    #
    # Halo/galaxy initialization and snapshot-accumulator reset are emitted as
    # typed inline functions in property_defs.h (init_halo_from_payload,
    # init_galaxy_defaults, reset_galaxy_snapshot_accumulators). The only
    # initialization .inc that remains is the tree-driver payload populator,
    # which must be tree-coupled and therefore cannot be a driver-neutral header
    # function.
    write_file(
        GENERATED_DIR / "populate_halo_payload_from_tree.inc",
        generate_populate_halo_payload_from_tree(halo_props, yaml_hash),
    )
    write_file(
        GENERATED_DIR / "property_test_helpers.h",
        generate_property_test_helpers(galaxy_props, yaml_hash),
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
    write_file(
        GENERATED_DIR / "hdf5_field_metadata.inc",
        generate_hdf5_field_metadata(halo_props, galaxy_props, yaml_hash),
    )
    write_file(
        GENERATED_DIR / "output_schema_writer.inc",
        generate_output_schema_writer(halo_props, galaxy_props, yaml_hash),
    )

    # Validation manifest for tests
    write_file(
        TESTS_GENERATED_DIR / "property_ranges.json",
        generate_validation_manifest(halo_props, galaxy_props, yaml_hash),
    )

    # Save hash for future comparison
    save_hash(yaml_hash)

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
    print("  Schema writer:   src/include/generated/output_schema_writer.inc")
    print()
    print("Next steps:")
    print("  1. Review generated files")
    print("  2. Update source files to include generated code")
    print("  3. Test compilation: make")
    print(f"  4. Run validation: make MODEL={os.environ.get('MODEL', 'sage')} check-generated")
    print()


if __name__ == "__main__":
    main()
