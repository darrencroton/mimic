"""The L-Halo record a simulation package declares, laid out as the C compiler lays it out.

Mimic's ``lhalo_binary`` reader reads each forest's halos wholesale into the generated
``struct RawHalo`` (``scripts/generate_properties.py``), whose members are the package's
``halo_properties`` entries in declaration order with the C type of each entry's ``type``. The
file record is therefore that struct's memory image: every member at its natural alignment and
the record padded to the struct's alignment, so an array of records has the compiler's stride.
:func:`record_dtype` builds the same layout as a numpy structured dtype (``align=True``
reproduces the C rules for these types), and :func:`record_layout` and :func:`record_digest`
describe it stably, so a file plan or manifest can bind the files it describes to the record
they were written in.

For ``simulations/shin-uchuu-ascii/halo_properties.yaml`` the record is 88 bytes: thirteen
members fill 80, ``MostBoundID`` (``long long``) sits at offset 72, ``SnapNum`` at 80, and four
bytes of trailing padding round the record up to the 8-byte alignment ``MostBoundID`` imposes.
"""

import hashlib
import json
import os
import sys
from pathlib import Path
from typing import Dict, List, Sequence, Union

import numpy as np
import yaml

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from errors import ConverterError  # noqa: E402

#: The generator's catalog types and the numpy field each becomes: (base dtype, shape).
RECORD_TYPES = {
    "int": ("<i4", ()),
    "float": ("<f4", ()),
    "long long": ("<i8", ()),
    "double": ("<f8", ()),
    "vec3_float": ("<f4", (3,)),
    "vec3_int": ("<i4", (3,)),
}


def _halo_properties(source: Union[str, Path, Sequence[dict]]) -> List[dict]:
    """The ``halo_properties`` list of a YAML path, or the list itself."""
    if isinstance(source, (str, Path)):
        path = Path(source)
        try:
            with open(path, "r", encoding="utf-8") as handle:
                data = yaml.safe_load(handle)
        except (OSError, yaml.YAMLError) as error:
            raise ConverterError(
                "{}: cannot read halo properties: {}".format(path, error)
            ) from error
        if not isinstance(data, dict) or not isinstance(data.get("halo_properties"), list):
            raise ConverterError("{}: no halo_properties list".format(path))
        return data["halo_properties"]
    return list(source)


def record_dtype(source: Union[str, Path, Sequence[dict]]) -> np.dtype:
    """The package's L-Halo record as a C-aligned numpy structured dtype.

    Args:
        source: a ``halo_properties.yaml`` path, or its parsed ``halo_properties`` list.

    Returns:
        One field per entry, named by its ``name``, in declaration order; ``itemsize`` includes
        the trailing padding, so it is ``sizeof(struct RawHalo)``.

    Raises:
        ConverterError: for an empty list, an entry without a name, a repeated name, or a
            ``type`` outside :data:`RECORD_TYPES`, naming the entry.
    """
    entries = _halo_properties(source)
    if not entries:
        raise ConverterError("halo_properties is empty: a record needs at least one field")
    fields = []
    seen = set()
    for index, entry in enumerate(entries):
        name = entry.get("name") if isinstance(entry, dict) else None
        if not isinstance(name, str) or not name:
            raise ConverterError("halo_properties entry {} has no name".format(index))
        if name in seen:
            raise ConverterError("halo_properties entry {!r} is declared twice".format(name))
        seen.add(name)
        kind = entry.get("type")
        if not isinstance(kind, str) or kind not in RECORD_TYPES:
            raise ConverterError(
                "halo_properties entry {!r} has type {!r}; a record field is one of {}".format(
                    name, kind, sorted(RECORD_TYPES)
                )
            )
        base, shape = RECORD_TYPES[kind]
        fields.append((name, base, shape))
    return np.dtype(fields, align=True)


def record_layout(dtype: np.dtype) -> Dict[str, object]:
    """A stable, JSON-serialisable description of a record dtype.

    Returns:
        ``{"itemsize": n, "fields": [{"name", "offset", "size", "dtype", "shape"}, ...]}`` with
        the fields in offset order; ``dtype`` is the field's little-endian base type and
        ``shape`` its element shape (``[]`` for a scalar).
    """
    fields = []
    for name in dtype.names:
        field, offset = dtype.fields[name][:2]
        fields.append(
            {
                "name": name,
                "offset": int(offset),
                "size": int(field.itemsize),
                "dtype": field.base.newbyteorder("<").str,
                "shape": list(field.shape),
            }
        )
    fields.sort(key=lambda entry: entry["offset"])
    return {"itemsize": int(dtype.itemsize), "fields": fields}


def record_digest(layout: Dict[str, object]) -> str:
    """The SHA-256 of a :func:`record_layout`, over its canonical JSON (sorted keys, no
    whitespace)."""
    canonical = json.dumps(layout, sort_keys=True, separators=(",", ":"))
    return hashlib.sha256(canonical.encode("ascii")).hexdigest()
