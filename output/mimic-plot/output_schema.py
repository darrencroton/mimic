"""Read Mimic run-local output schemas."""

import json
import sys
import warnings
from pathlib import Path

import numpy as np


NUMPY_TYPES = {
    "int32": np.int32,
    "int64": np.int64,
    "float32": np.float32,
    "float64": np.float64,
}


def schema_path_for_output(output_path):
    """Return the schema path for a Mimic output file or output directory."""
    path = Path(output_path)
    output_dir = path if path.is_dir() else path.parent
    return output_dir / "metadata" / "output_schema.json"


def load_schema(output_path):
    """Load metadata/output_schema.json for a Mimic output directory."""
    schema_path = schema_path_for_output(output_path)
    if not schema_path.exists():
        raise FileNotFoundError(
            f"Output schema not found: {schema_path}. "
            "Mimic binary outputs must be kept with their metadata directory."
        )

    with schema_path.open("r", encoding="utf-8") as handle:
        schema = json.load(handle)

    if schema.get("schema_version") != 1:
        raise ValueError(
            f"Unsupported output schema version in {schema_path}: "
            f"{schema.get('schema_version')}"
        )

    fields = schema.get("fields")
    record = schema.get("record", {})
    if not isinstance(fields, list) or not fields:
        raise ValueError(f"Output schema has no fields: {schema_path}")
    if "binary_record_size" not in record:
        raise ValueError(f"Output schema has no binary record size: {schema_path}")

    byte_order = record.get("byte_order", "native")
    if byte_order == "native" and sys.byteorder != "little":
        warnings.warn(
            f"Schema at {schema_path} was written on a little-endian host "
            f"but this machine is {sys.byteorder}-endian. "
            "Binary output files may require byte-swapping.",
            stacklevel=3,
        )

    return schema


def dtype_from_schema(schema, *, binary=True):
    """Build a NumPy dtype from a Mimic output schema."""
    names = []
    formats = []
    offsets = []

    for field in schema["fields"]:
        numpy_type = field.get("numpy_type")
        if numpy_type not in NUMPY_TYPES:
            raise ValueError(
                f"Unsupported dtype '{numpy_type}' for field '{field.get('name')}'"
            )

        shape = tuple(field.get("shape", []))
        base_type = NUMPY_TYPES[numpy_type]
        field_dtype = np.dtype((base_type, shape)) if shape else np.dtype(base_type)

        names.append(field["name"])
        formats.append(field_dtype)
        offsets.append(int(field["offset"]))

    if not binary:
        return np.dtype({"names": names, "formats": formats})

    return np.dtype(
        {
            "names": names,
            "formats": formats,
            "offsets": offsets,
            "itemsize": int(schema["record"]["binary_record_size"]),
        }
    )


def units_from_schema(schema):
    """Return field unit labels from a Mimic output schema."""
    return {field["name"]: field.get("units", "") for field in schema["fields"]}


def descriptions_from_schema(schema):
    """Return field descriptions from a Mimic output schema."""
    return {
        field["name"]: field.get("description", "") for field in schema["fields"]
    }
