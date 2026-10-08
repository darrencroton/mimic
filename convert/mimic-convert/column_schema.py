"""Canonical source schema, mapping profiles and schema identity.

A profile is a closed-key declaration of the required-role aliases and typed
extras for one source format; it is serialized canonically (sorted-key compact
JSON, no NaN) and hashed with SHA-256, so the digest a version 3 file records
changes exactly when a field's type, source, units or convention does.

Pure and side-effect free. This module parses a declarative mapping profile,
validates it, freezes it into an immutable :class:`CanonicalSchema`, and
derives that schema's deterministic SHA-256 identity. It reads a YAML profile
(and, for binary sources, an ordered ``halo_properties.yaml``) when asked to;
it never writes, never opens a source catalog and never converts data. The
source adapters, the transpose, the conversion manifest and the v3 writer all
consume what this module produces.

Three identities must not be confused, and this module keeps them apart by
construction:

- ``SourceHaloID`` is the converter's own positive, globally unique int64 row
  key, assigned by prefix-summing source halo counts over the adapter's
  declared total order (see ``adapters/base.py``). It is the only valid
  remapping key.
- ``MostBoundID`` is the *source catalog's* particle/halo identifier. It is
  carried through as signed int64 data with no uniqueness or positivity
  requirement, and is never a remapping key in the general format.
- ``(ForestIndex, HaloRankInForest)`` is the source-relative forest/rank
  identity pair, whose meaning is defined by the selected source
  representation and recorded in :data:`SOURCE_IDENTITY_CONVENTIONS`.

The canonical serialization rules are the reason this module exists:
comments, mapping key order, alias order and extra-definition order are
presentation and must not change the digest; field types, source components,
units and h conventions are semantics and must.

Profile input is untrusted YAML, and a YAML value may be a list or a mapping
wherever a scalar is expected. Every dict- or set-membership test on a profile
value therefore runs only after an ``isinstance(str)`` check, so a list or
mapping value produces :class:`ConverterError` rather than a bare
``TypeError`` from hashing.
"""

import hashlib
import json
import re
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, Iterable, List, Mapping, Optional, Sequence, Tuple

import yaml
from errors import ConverterError

__all__ = [
    "ConverterError",
    "PROFILE_SCHEMA_VERSION",
    "SOURCE_FORMATS",
    "EXTRA_TYPES",
    "H_CONVENTIONS",
    "BYTE_ORDERS",
    "TOPOLOGY_FIELDS",
    "IDENTITY_FIELDS",
    "FOREST_SIDECAR_FIELDS",
    "RESERVED_OUTPUT_NAMES",
    "REQUIRED_ROLES",
    "ROLE_SOURCE_TYPES",
    "PAYLOAD_FIELDS",
    "SOURCE_IDENTITY_CONVENTIONS",
    "MAX_OUTPUT_NAME_BYTES",
    "INT64_MAX",
    "SourceComponent",
    "ExtraField",
    "BinaryLayout",
    "SourceProperty",
    "LayoutEntry",
    "SourceLayout",
    "PayloadField",
    "ColumnMap",
    "CanonicalSchema",
    "normalize_alias",
    "load_column_map",
    "parse_column_map",
    "load_source_properties",
    "build_schema",
    "resolve_required_columns",
    "resolve_extra_sources",
]


# ==========================================================================
# Profile vocabulary
# ==========================================================================

#: The only accepted profile ``schema_version``. A profile is not
#: version-tolerant: an unknown version fails rather than being read on a
#: best-effort basis.
PROFILE_SCHEMA_VERSION = 1

#: Selected source formats. L-Halo HDF5 is deliberately absent: no named
#: package uses it, so it is new scope, not an omission.
SOURCE_FORMATS = ("consistent_trees_ascii", "consistent_trees_hdf5", "lhalo_binary")

#: Profile top-level keys. ``binary_layout`` is required for ``lhalo_binary``
#: and forbidden otherwise; the rest are required for every format. The key
#: set is closed in both directions -- an unknown key is
#: rejected, and a missing key is not defaulted.
_COMMON_PROFILE_KEYS = ("schema_version", "source_format", "required_columns", "extra_fields")
_BINARY_PROFILE_KEY = "binary_layout"

#: The exact extra-entry keys. All six are required; there are no
#: optional keys and no inferred values.
_EXTRA_ENTRY_KEYS = ("name", "sources", "type", "units", "h_convention", "description")

_SOURCE_COMPONENT_KEYS = ("field", "component")

_BINARY_LAYOUT_KEYS = ("byte_order", "itemsize", "offsets")


@dataclass(frozen=True)
class _TypeSpec:
    """Storage facts for one declarable numeric type.

    ``numpy_dtype`` is written without a byte-order prefix here; the emitted
    v3 datasets are explicit little-endian and the source side takes its
    byte order from the binary layout, so neither is a property of the type
    itself.
    """

    numpy_dtype: str
    n_components: int
    itemsize: int
    is_integer: bool


#: Declarable extra/payload types, matching the property generator's
#: numeric ``TYPE_MAP`` entries in scripts/generate_properties.py exactly --
#: any type this converter can emit must be a type the consuming property
#: system can declare. There is no expression language and no other type.
EXTRA_TYPES: Dict[str, _TypeSpec] = {
    "int": _TypeSpec("int32", 1, 4, True),
    "long long": _TypeSpec("int64", 1, 8, True),
    "float": _TypeSpec("float32", 1, 4, False),
    "double": _TypeSpec("float64", 1, 8, False),
    "vec3_int": _TypeSpec("int32", 3, 12, True),
    "vec3_float": _TypeSpec("float32", 3, 12, False),
}

#: Accepted ``h_convention`` values, matching generate_properties.py's
#: ``H_CONVENTIONS``.
H_CONVENTIONS = ("carried", "free", "none")

#: Accepted ``binary_layout.byte_order`` values, and their numpy prefix.
#: Host-native ('=') is deliberately not accepted: a source file's endianness
#: is a property of the file, never of the machine reading it.
BYTE_ORDERS = {"little": "<", "big": ">"}

#: Largest signed 64-bit integer. It bounds a binary record's itemsize and any
#: field offset within it: a record extent is a byte count that a reader must
#: be able to seek to, so int64 is the ceiling that matters; nothing here
#: describes a real record anywhere near it, and refusing a value beyond it
#: keeps an absurd literal from reaching arithmetic that assumes a machine word.
INT64_MAX = 2**63 - 1

#: Output names are ASCII ``[A-Za-z][A-Za-z0-9_]*``, at most 63 bytes.
#: Matched with ``fullmatch``: ``$`` also matches immediately *before* a
#: trailing newline, so ``re.match(r"^...$", "Rvir\n")`` succeeds and a quoted
#: YAML scalar like ``name: "Rvir\n"`` would pass a grammar it plainly
#: violates -- and then become an HDF5 object name.
_OUTPUT_NAME_RE = re.compile(r"[A-Za-z][A-Za-z0-9_]*")
MAX_OUTPUT_NAME_BYTES = 63


# ==========================================================================
# Reserved v3 names
# ==========================================================================


@dataclass(frozen=True)
class _FixedField:
    """A v3 field whose name, type and meaning are fixed by the format table
    rather than declared by a profile."""

    name: str
    type: str
    description: str


#: The five int64 snapshot-local link columns plus the three int32
#: target-snapshot columns. The snapshot columns are -1 if and only if
#: the corresponding row index is -1; that biconditional is a v3 validator
#: obligation (validate.py), stated here so the name table and the rule live
#: together.
TOPOLOGY_FIELDS: Tuple[_FixedField, ...] = (
    _FixedField(
        "Descendant", "long long", "Snapshot-local row index of the descendant, -1 if none"
    ),
    _FixedField(
        "FirstProgenitor",
        "long long",
        "Snapshot-local row index of the main progenitor, -1 if none",
    ),
    _FixedField(
        "NextProgenitor",
        "long long",
        "Snapshot-local row index of the next sibling progenitor, -1 if none",
    ),
    _FixedField(
        "FirstHaloInFOFgroup",
        "long long",
        "Snapshot-local row index of the FoF central; self-index for a central, never -1",
    ),
    _FixedField(
        "NextHaloInFOFgroup",
        "long long",
        "Snapshot-local row index of the next FoF member, -1 if last",
    ),
    _FixedField(
        "DescendantSnapshot", "int", "Snapshot of the Descendant target, -1 iff Descendant is -1"
    ),
    _FixedField(
        "FirstProgenitorSnapshot",
        "int",
        "Snapshot of the FirstProgenitor target, -1 iff FirstProgenitor is -1",
    ),
    _FixedField(
        "NextProgenitorSnapshot",
        "int",
        "Snapshot of the NextProgenitor target, -1 iff NextProgenitor is -1",
    ),
)

#: The three int64 identity arrays. Governed by the fixed format table,
#: never redeclared in ``/schema``.
IDENTITY_FIELDS: Tuple[_FixedField, ...] = (
    _FixedField(
        "SourceHaloID",
        "long long",
        "Positive, globally unique converter row key from the adapter's declared source order",
    ),
    _FixedField(
        "ForestIndex",
        "long long",
        "Source-relative forest identity (see SOURCE_IDENTITY_CONVENTIONS)",
    ),
    _FixedField(
        "HaloRankInForest",
        "long long",
        "Source-relative within-forest row identity (see SOURCE_IDENTITY_CONVENTIONS)",
    ),
)

#: ``forests.h5`` sidecar arrays, all int64 and all length
#: ``n_forests_total``.
FOREST_SIDECAR_FIELDS: Tuple[_FixedField, ...] = (
    _FixedField("ForestID", "long long", "Source forest id (see SOURCE_IDENTITY_CONVENTIONS)"),
    _FixedField(
        "SourceFileOrdinal", "long long", "Inventory file ordinal owning the forest, -1 if it spans"
    ),
    _FixedField(
        "SourceUnitOrdinal", "long long", "Within-file unit ordinal of the forest, -1 if it spans"
    ),
)

#: Core physical/catalog payload names. Same nine names for every source
#: format; only their declared types and units differ (see PAYLOAD_FIELDS).
_CORE_PAYLOAD_NAMES = (
    "Len",
    "SnapNum",
    "M_Crit200",
    "Pos",
    "Vel",
    "Spin",
    "VelDisp",
    "Vmax",
    "MostBoundID",
)

#: Every name an extra may not take ("reject ... collisions with reserved
#: topology/identity/core names"). The sidecar names are included because a
#: v3 dataset carries them as identity provenance.
RESERVED_OUTPUT_NAMES = frozenset(
    [f.name for f in TOPOLOGY_FIELDS]
    + [f.name for f in IDENTITY_FIELDS]
    + [f.name for f in FOREST_SIDECAR_FIELDS]
    + list(_CORE_PAYLOAD_NAMES)
)


# ==========================================================================
# Per-format required roles
# ==========================================================================

#: Consistent-Trees ASCII roles: the current parser's required column names
#: (convert/mimic-convert/ctrees_parser.py ``_INT_COLUMNS``/``_FLOAT_COLUMNS``) with
#: ``snap`` as the snapshot role. ``scale`` is parsed but not retained in
#: the scratch record, and is a required role all the same -- the reference
#: parse path reads it.
_ASCII_ROLES = (
    "desc_id",
    "desc_scale",
    "id",
    "jx",
    "jy",
    "jz",
    "mvir",
    "pid",
    "scale",
    "snap",
    "upid",
    "vmax",
    "vrms",
    "vx",
    "vy",
    "vz",
    "x",
    "y",
    "z",
)

#: Consistent-Trees forests-HDF5 roles: the five stored link names plus
#: the value columns, with exact spellings taken from the C reader's own field
#: table (src/io/vertical/read_ctrees_hdf5.c ``CTREES_H5_FIXED_FIELD_NAMES``).
_CTREES_HDF5_ROLES = (
    "Descendant",
    "FirstHaloInFOFgroup",
    "FirstProgenitor",
    "Jx",
    "Jy",
    "Jz",
    "Mvir",
    "NextHaloInFOFgroup",
    "NextProgenitor",
    "id",
    "snap",
    "vmax",
    "vrms",
    "vx",
    "vy",
    "vz",
    "x",
    "y",
    "z",
)

#: L-Halo binary roles: the five stored link names plus the stored value
#: columns, with spellings from the shipped record
#: (src/include/generated/raw_halo_defs.h / struct RawHalo).
_LHALO_ROLES = (
    "Descendant",
    "FirstHaloInFOFgroup",
    "FirstProgenitor",
    "Len",
    "M_Crit200",
    "MostBoundID",
    "NextHaloInFOFgroup",
    "NextProgenitor",
    "Pos",
    "SnapNum",
    "Spin",
    "Vel",
    "VelDisp",
    "Vmax",
)

#: source format -> the exact role set its profile must map. Structural
#: metadata (tree headers, ForestInfo) is adapter-owned and deliberately
#: absent: it cannot be remapped or omitted.
REQUIRED_ROLES: Dict[str, Tuple[str, ...]] = {
    "consistent_trees_ascii": _ASCII_ROLES,
    "consistent_trees_hdf5": _CTREES_HDF5_ROLES,
    "lhalo_binary": _LHALO_ROLES,
}

#: The type each ``lhalo_binary`` role's source field must actually have, as
#: the shipped record stores it (struct RawHalo). This is the *source* side,
#: not the emitted side: the five links are int32 on disk and are widened to
#: int64 only in v3's output.
#:
#: There is no equivalent table for the two named-object formats. A
#: Consistent-Trees ASCII column has no declared type at all until it is
#: parsed, and a forests-HDF5 dataset's dtype is a property of the file rather
#: than of any profile, so for those two the check is an adapter-read-time
#: obligation (see ``adapters/base.py``) and cannot be made at freeze time.
_LHALO_ROLE_SOURCE_TYPES: Dict[str, str] = {
    "Descendant": "int",
    "FirstHaloInFOFgroup": "int",
    "FirstProgenitor": "int",
    "Len": "int",
    "M_Crit200": "float",
    "MostBoundID": "long long",
    "NextHaloInFOFgroup": "int",
    "NextProgenitor": "int",
    "Pos": "vec3_float",
    "SnapNum": "int",
    "Spin": "vec3_float",
    "Vel": "vec3_float",
    "VelDisp": "float",
    "Vmax": "float",
}

#: source format -> role type table, where one can exist at freeze time.
ROLE_SOURCE_TYPES: Dict[str, Dict[str, str]] = {"lhalo_binary": _LHALO_ROLE_SOURCE_TYPES}


# ==========================================================================
# Per-format payload declarations
# ==========================================================================


@dataclass(frozen=True)
class PayloadField:
    """One physical/catalog payload field as the emitted file declares it.

    These are the adapter's *native* storage precision and units: the
    converter records what the source actually carries and never converts a
    value into a different basis to make two formats look alike. Each field
    becomes one ``/schema`` subgroup in a v3 file.
    """

    name: str
    type: str
    units: str
    h_convention: str
    description: str


#: Consistent-Trees payload (ASCII and forests-HDF5 share the ctrees value
#: conventions): mass is the native ctrees ``Mvir`` in Msun/h, Spin is the
#: producer-applied J/Mvir specific angular momentum, and Len is derived as
#: round(Mvir * 1e-10 / particle_mass).
_CTREES_PAYLOAD: Tuple[PayloadField, ...] = (
    PayloadField(
        "Len",
        "int",
        "particles",
        "none",
        "Particle count derived as round(Mvir * 1e-10 / particle_mass); never negative",
    ),
    PayloadField("SnapNum", "int", "dimensionless", "none", "Snapshot index of this halo"),
    PayloadField(
        "M_Crit200", "float", "Msun/h", "carried", "Consistent-Trees Mvir in native Msun/h"
    ),
    PayloadField("Pos", "vec3_float", "Mpc/h", "carried", "Comoving position"),
    PayloadField("Vel", "vec3_float", "km/s", "none", "Peculiar velocity"),
    PayloadField(
        "Spin",
        "vec3_float",
        "Mpc/h km/s",
        "carried",
        "Specific angular momentum J/Mvir; zero-mass halos carry unnormalised J",
    ),
    PayloadField("VelDisp", "float", "km/s", "none", "Velocity dispersion"),
    PayloadField("Vmax", "float", "km/s", "none", "Maximum circular velocity"),
    PayloadField(
        "MostBoundID",
        "long long",
        "dimensionless",
        "none",
        "Source catalog identifier carried as signed data; not unique in the general format",
    ),
)

#: L-Halo binary payload: Len is supplied by the source rather than
#: derived, mass stays float32 in 1e10 Msun/h (a round trip through Msun/h
#: would destroy bit parity), Spin is already specific angular momentum, and
#: MostBoundID is the signed particle identifier.
_LHALO_PAYLOAD: Tuple[PayloadField, ...] = (
    PayloadField(
        "Len", "int", "particles", "none", "Particle count supplied by the source; never negative"
    ),
    PayloadField("SnapNum", "int", "dimensionless", "none", "Snapshot index of this halo"),
    PayloadField(
        "M_Crit200",
        "float",
        "1e10 Msun/h",
        "carried",
        "Native L-Halo M_Crit200; kept float32 in 1e10 Msun/h with no unit round trip",
    ),
    PayloadField("Pos", "vec3_float", "Mpc/h", "carried", "Comoving position"),
    PayloadField("Vel", "vec3_float", "km/s", "none", "Peculiar velocity"),
    PayloadField(
        "Spin",
        "vec3_float",
        "Mpc/h km/s",
        "carried",
        "Specific angular momentum as already stored by the source; not renormalised",
    ),
    PayloadField("VelDisp", "float", "km/s", "none", "Velocity dispersion"),
    PayloadField("Vmax", "float", "km/s", "none", "Maximum circular velocity"),
    PayloadField(
        "MostBoundID",
        "long long",
        "dimensionless",
        "none",
        "Signed most-bound particle identifier carried as data; duplicates are legal",
    ),
)

#: source format -> its native payload declarations.
PAYLOAD_FIELDS: Dict[str, Tuple[PayloadField, ...]] = {
    "consistent_trees_ascii": _CTREES_PAYLOAD,
    "consistent_trees_hdf5": _CTREES_PAYLOAD,
    "lhalo_binary": _LHALO_PAYLOAD,
}

#: How ``ForestIndex``, ``HaloRankInForest`` and the sidecar ``ForestID`` are
#: defined for each source format, and ``SourceHaloID`` where the route defines
#: it by those identities rather than by its physical inventory. Identity is
#: relative to the selected source representation: L-Halo and forests-HDF5
#: packagings of the same simulation can enumerate different forest sets, so
#: equal identities across formats are never promised.
SOURCE_IDENTITY_CONVENTIONS: Dict[str, Dict[str, str]] = {
    "consistent_trees_ascii": {
        "forest_index": "dense forest-id enumeration in ascending source forest id",
        "halo_rank_in_forest": "post-fixup reference vertical traversal order",
        "forest_id": "original source forest id",
        "ordinals": "-1/-1 for a forest spanning files; the manifest retains full membership",
        "source_halo_id": "1-based position in (ForestIndex, HaloRankInForest) order",
    },
    "consistent_trees_hdf5": {
        "forest_index": "file-prefix ForestInfo row number",
        "halo_rank_in_forest": "original within-forest row index",
        "forest_id": "source ForestID, retained even when only file-local unique",
        "ordinals": "file ordinal and ForestInfo row ordinal",
    },
    "lhalo_binary": {
        "forest_index": "file-prefix tree number",
        "halo_rank_in_forest": "original within-tree row index",
        "forest_id": "dense run forest number, disambiguated by the two ordinals",
        "ordinals": "file ordinal and within-file tree ordinal",
    },
}


# ==========================================================================
# Name and alias helpers
# ==========================================================================


def _check_output_name(name: str, what: str) -> None:
    if not isinstance(name, str):
        raise ConverterError("{}: name must be a string, got {!r}".format(what, name))
    if not _OUTPUT_NAME_RE.fullmatch(name):
        raise ConverterError("{}: name {!r} is not ASCII [A-Za-z][A-Za-z0-9_]*".format(what, name))
    if len(name.encode("ascii")) > MAX_OUTPUT_NAME_BYTES:
        raise ConverterError(
            "{}: name {!r} is {} bytes, above the {}-byte limit".format(
                what, name, len(name.encode("ascii")), MAX_OUTPUT_NAME_BYTES
            )
        )


def normalize_alias(source_format: str, alias: str) -> str:
    """Canonical form of one source column alias.

    Consistent-Trees ASCII headers are matched suffix-stripped (truncated at
    the first ``(``) and case-insensitively, exactly as the reference parser
    does (``parse_ctrees.h``; ``ctrees_parser.normalize_column_name``). HDF5
    dataset names and binary record field names are matched exactly -- they
    are stored object names, not a printed header dialect, and a
    case-insensitive match there would invent an equivalence the source does
    not have.
    """
    if not isinstance(alias, str):
        raise ConverterError("alias must be a string, got {!r}".format(alias))
    if source_format == "consistent_trees_ascii":
        canonical = alias.split("(", 1)[0].strip().lower()
    else:
        canonical = alias
    if not canonical:
        raise ConverterError("alias {!r} normalizes to an empty name".format(alias))
    return _require_utf8_encodable(canonical, "alias {!r}".format(alias))


# ==========================================================================
# Profile data model
# ==========================================================================


@dataclass(frozen=True, order=True)
class SourceComponent:
    """One source component of an extra field.

    ``component`` is ``None`` for a scalar stored field and 0/1/2 for an
    element of a stored vector. That distinction is what lets a native binary
    vector (``Pos``) be selected without pretending its elements are
    independent columns.
    """

    field: str
    component: Optional[int] = None

    def as_canonical(self) -> Dict[str, object]:
        entry: Dict[str, object] = {"field": self.field}
        if self.component is not None:
            entry["component"] = self.component
        return entry


@dataclass(frozen=True)
class ExtraField:
    """One declaratively selected extra output field."""

    name: str
    sources: Tuple[SourceComponent, ...]
    type: str
    units: str
    h_convention: str
    description: str

    @property
    def spec(self) -> _TypeSpec:
        return EXTRA_TYPES[self.type]

    def as_canonical(self) -> Dict[str, object]:
        return {
            "description": self.description,
            "h_convention": self.h_convention,
            "name": self.name,
            "sources": [component.as_canonical() for component in self.sources],
            "type": self.type,
            "units": self.units,
        }


@dataclass(frozen=True)
class BinaryLayout:
    """Declared on-disk layout of a fixed-record binary source."""

    byte_order: str
    itemsize: int
    offsets: Tuple[Tuple[str, int], ...]

    @property
    def numpy_byte_order(self) -> str:
        return BYTE_ORDERS[self.byte_order]


@dataclass(frozen=True)
class SourceProperty:
    """One entry of an ordered source ``halo_properties.yaml``."""

    name: str
    type: str
    units: str


@dataclass(frozen=True)
class LayoutEntry:
    """One field of a frozen binary source layout, in declaration order."""

    name: str
    type: str
    units: str
    offset: int
    itemsize: int
    n_components: int
    numpy_dtype: str
    selected: bool

    def as_canonical(self) -> Dict[str, object]:
        return {
            "itemsize": self.itemsize,
            "n_components": self.n_components,
            "name": self.name,
            "numpy_dtype": self.numpy_dtype,
            "offset": self.offset,
            "selected": self.selected,
            "type": self.type,
            "units": self.units,
        }


@dataclass(frozen=True)
class SourceLayout:
    """The complete ordered binary source record.

    Every declared source field appears here with its offset and width,
    whether or not the profile selects it: an unselected field still occupies
    its bytes, and the stride a reader must step is a property of the source,
    not of the selection ("Extra selection does not alter the on-disk
    source stride").
    """

    byte_order: str
    itemsize: int
    entries: Tuple[LayoutEntry, ...]

    @property
    def numpy_byte_order(self) -> str:
        return BYTE_ORDERS[self.byte_order]

    def numpy_dtype_spec(self) -> Dict[str, object]:
        """Structured-dtype specification ready for ``np.dtype(...)``.

        Returned in the names/formats/offsets/itemsize form, which is the only
        one that carries explicit offsets and a total itemsize -- a record with
        padding cannot be expressed as a plain list of (name, format) tuples,
        and a reader that stepped a packed stride over a padded record would
        silently misread every row after the first.

        Byte order is always the declared one; numpy's native '=' is never
        used, so reading a big-endian source on a little-endian host cannot
        silently succeed.
        """
        prefix = self.numpy_byte_order
        formats: List[object] = []
        for entry in self.entries:
            code = prefix + _numpy_short_code(entry.numpy_dtype)
            formats.append(code if entry.n_components == 1 else (code, (entry.n_components,)))
        return {
            "names": [entry.name for entry in self.entries],
            "formats": formats,
            "offsets": [entry.offset for entry in self.entries],
            "itemsize": self.itemsize,
        }

    def as_canonical(self) -> Dict[str, object]:
        return {
            "byte_order": self.byte_order,
            "entries": [entry.as_canonical() for entry in self.entries],
            "itemsize": self.itemsize,
        }


_NUMPY_SHORT_CODES = {
    "int32": "i4",
    "int64": "i8",
    "float32": "f4",
    "float64": "f8",
}


def _numpy_short_code(numpy_dtype: str) -> str:
    return _NUMPY_SHORT_CODES[numpy_dtype]


@dataclass(frozen=True)
class ColumnMap:
    """A parsed, validated mapping profile.

    Immutable by construction: ``required_columns`` is a tuple of
    (role, alias tuple) pairs rather than a dict so the whole object can be
    frozen and hashed without a caller being able to mutate it afterwards.
    """

    schema_version: int
    source_format: str
    required_columns: Tuple[Tuple[str, Tuple[str, ...]], ...]
    extra_fields: Tuple[ExtraField, ...]
    binary_layout: Optional[BinaryLayout]
    origin: str

    @property
    def roles(self) -> Tuple[str, ...]:
        return tuple(role for role, _aliases in self.required_columns)


# ==========================================================================
# Strict YAML loading
# ==========================================================================


class _StrictLoader(yaml.SafeLoader):
    """SafeLoader that rejects duplicate mapping keys.

    PyYAML's default behaviour is last-wins, which would let a profile that
    declares ``type: float`` twice with different values load silently and
    produce a schema nobody wrote. A converter that must fail rather than
    repair (VISION.md) cannot accept that.
    """


def _construct_mapping_no_duplicates(loader, node, deep=False):
    loader.flatten_mapping(node)
    mapping: Dict = {}
    for key_node, value_node in node.value:
        key = loader.construct_object(key_node, deep=True)
        try:
            duplicate = key in mapping
        except TypeError as exc:
            # A YAML mapping may legally use a sequence or a mapping as a key.
            # Python cannot hash one, and letting that escape as a raw
            # TypeError would be the one malformed-profile path that does not
            # produce this module's named error.
            raise ConverterError(
                "unhashable mapping key at line {}: a profile key must be a scalar ({})".format(
                    key_node.start_mark.line + 1, exc
                )
            ) from exc
        if duplicate:
            raise ConverterError(
                "duplicate key {!r} at line {}".format(key, key_node.start_mark.line + 1)
            )
        mapping[key] = loader.construct_object(value_node, deep=True)
    return mapping


_StrictLoader.add_constructor(
    yaml.resolver.BaseResolver.DEFAULT_MAPPING_TAG, _construct_mapping_no_duplicates
)


def _load_yaml_mapping(path: Path, what: str) -> Dict:
    try:
        # Explicit UTF-8, not the platform default: a profile is repository
        # content read on every platform, and a locale-dependent decode would
        # make the same bytes valid on one machine and not another.
        with open(path, "r", encoding="utf-8") as handle:
            data = yaml.load(handle, Loader=_StrictLoader)
    except UnicodeDecodeError as exc:
        raise ConverterError("{}: {} is not valid UTF-8: {}".format(path, what, exc)) from exc
    except OSError as exc:
        raise ConverterError("{}: cannot read {}: {}".format(path, what, exc)) from exc
    except yaml.YAMLError as exc:
        raise ConverterError("{}: invalid YAML: {}".format(path, exc)) from exc
    except ConverterError as exc:
        raise ConverterError("{}: {}".format(path, exc)) from exc
    except ValueError as exc:
        # PyYAML constructs scalars itself, and some scalars it cannot: an
        # integer literal above CPython's string-to-int digit limit raises a
        # raw ValueError from inside the loader, before any check in this
        # module runs. UnicodeDecodeError is a ValueError subclass and is
        # caught above, so it keeps its own more specific message.
        raise ConverterError("{}: unreadable scalar value: {}".format(path, exc)) from exc
    if not isinstance(data, dict):
        raise ConverterError("{}: {} must be a YAML mapping, got {}".format(path, what, type(data)))
    return data


# ==========================================================================
# Profile parsing
# ==========================================================================


def _require_int(value, what: str) -> int:
    """Accept a YAML integer only.

    ``bool`` is excluded explicitly because it is an ``int`` subclass in
    Python, and a float is excluded because an integer must never arrive
    through floating point.
    """
    if isinstance(value, bool) or not isinstance(value, int):
        raise ConverterError("{}: must be an integer, got {!r}".format(what, value))
    return value


def _require_nonempty_str(value, what: str) -> str:
    if not isinstance(value, str) or not value.strip():
        raise ConverterError("{}: must be a nonempty string, got {!r}".format(what, value))
    return _require_utf8_encodable(value, what)


def _require_utf8_encodable(value: str, what: str) -> str:
    """Reject a string that cannot be encoded as UTF-8.

    A lone surrogate is a perfectly valid YAML scalar that PyYAML parses
    happily, but it cannot be encoded -- so without this the failure surfaces
    as a raw ``UnicodeEncodeError`` from ``canonical_json().encode("utf-8")``,
    at digest time rather than at parse time, and from a call site that has no
    idea which field was at fault. Caught here, where the field is still
    named.
    """
    try:
        value.encode("utf-8")
    except UnicodeEncodeError as exc:
        raise ConverterError(
            "{}: is not encodable as UTF-8 (a lone surrogate or similar): {}".format(what, exc)
        ) from exc
    return value


def _check_key_set(mapping: Mapping, allowed: Sequence[str], what: str) -> None:
    keys = set(mapping.keys())
    unknown = sorted(str(k) for k in keys - set(allowed))
    if unknown:
        raise ConverterError(
            "{}: unknown key(s) {} (allowed: {})".format(what, unknown, sorted(allowed))
        )
    missing = sorted(set(allowed) - keys)
    if missing:
        raise ConverterError("{}: missing required key(s) {}".format(what, missing))


def _parse_required_columns(
    raw, source_format: str, origin: str
) -> Tuple[Tuple[str, Tuple[str, ...]], ...]:
    if not isinstance(raw, dict):
        raise ConverterError("{}: required_columns must be a mapping".format(origin))
    expected = REQUIRED_ROLES[source_format]
    _check_key_set(raw, expected, "{}: required_columns".format(origin))

    parsed: List[Tuple[str, Tuple[str, ...]]] = []
    for role in expected:
        aliases = raw[role]
        if isinstance(aliases, str) or not isinstance(aliases, (list, tuple)):
            raise ConverterError(
                "{}: required_columns.{} must be a list of aliases, got {!r}".format(
                    origin, role, aliases
                )
            )
        if not aliases:
            raise ConverterError(
                "{}: required_columns.{} has an empty alias list".format(origin, role)
            )
        normalized: List[str] = []
        for alias in aliases:
            try:
                canonical = normalize_alias(source_format, alias)
            except ConverterError as exc:
                raise ConverterError(
                    "{}: required_columns.{}: {}".format(origin, role, exc)
                ) from exc
            if canonical in normalized:
                raise ConverterError(
                    "{}: required_columns.{} lists {!r} twice after normalization".format(
                        origin, role, canonical
                    )
                )
            normalized.append(canonical)
        # Aliases are sorted in canonical form: exactly one must resolve per
        # file, so their order carries no meaning and reordering them is a
        # presentation change that must not move the digest.
        parsed.append((role, tuple(sorted(normalized))))
    return tuple(parsed)


def _parse_source_component(raw, source_format: str, what: str) -> SourceComponent:
    if not isinstance(raw, dict):
        raise ConverterError("{}: each source must be a mapping, got {!r}".format(what, raw))
    unknown = sorted(str(k) for k in set(raw.keys()) - set(_SOURCE_COMPONENT_KEYS))
    if unknown:
        raise ConverterError(
            "{}: unknown source key(s) {} (allowed: {})".format(
                what, unknown, list(_SOURCE_COMPONENT_KEYS)
            )
        )
    if "field" not in raw:
        raise ConverterError("{}: source is missing 'field'".format(what))
    field_name = normalize_alias(
        source_format, _require_nonempty_str(raw["field"], what + ".field")
    )
    component: Optional[int] = None
    if "component" in raw:
        component = _require_int(raw["component"], what + ".component")
        if component not in (0, 1, 2):
            raise ConverterError("{}.component must be 0, 1 or 2, got {}".format(what, component))
    return SourceComponent(field=field_name, component=component)


def _parse_extra_field(raw, source_format: str, origin: str, index: int) -> ExtraField:
    what = "{}: extra_fields[{}]".format(origin, index)
    if not isinstance(raw, dict):
        raise ConverterError("{}: must be a mapping, got {!r}".format(what, raw))
    _check_key_set(raw, _EXTRA_ENTRY_KEYS, what)

    name = raw["name"]
    _check_output_name(name, what)
    if name in RESERVED_OUTPUT_NAMES:
        raise ConverterError(
            "{}: name {!r} collides with a reserved topology/identity/core name".format(what, name)
        )

    type_name = raw["type"]
    if not isinstance(type_name, str) or type_name not in EXTRA_TYPES:
        raise ConverterError(
            "{}: unsupported type {!r} (supported: {})".format(what, type_name, sorted(EXTRA_TYPES))
        )
    spec = EXTRA_TYPES[type_name]

    sources_raw = raw["sources"]
    if isinstance(sources_raw, (str, dict)) or not isinstance(sources_raw, (list, tuple)):
        raise ConverterError("{}: sources must be a list, got {!r}".format(what, sources_raw))
    if len(sources_raw) != spec.n_components:
        raise ConverterError(
            "{}: type {!r} needs exactly {} source component(s), got {}".format(
                what, type_name, spec.n_components, len(sources_raw)
            )
        )
    components: List[SourceComponent] = []
    for position, entry in enumerate(sources_raw):
        component = _parse_source_component(
            entry, source_format, "{}.sources[{}]".format(what, position)
        )
        if component in components:
            # A repeated (field, component) pair inside one output field can
            # only be a typo: it would emit the same source value into two or
            # three elements of a vector. Rejected as a malformed
            # definition rather than silently emitted.
            raise ConverterError(
                "{}: source component {!r} appears more than once in one field".format(
                    what, component
                )
            )
        components.append(component)

    h_convention = raw["h_convention"]
    if h_convention not in H_CONVENTIONS:
        raise ConverterError(
            "{}: h_convention must be one of {}, got {!r}".format(
                what, list(H_CONVENTIONS), h_convention
            )
        )

    return ExtraField(
        name=name,
        sources=tuple(components),
        type=type_name,
        units=_require_nonempty_str(raw["units"], what + ".units"),
        h_convention=h_convention,
        description=_require_nonempty_str(raw["description"], what + ".description"),
    )


def _parse_extra_fields(raw, source_format: str, origin: str) -> Tuple[ExtraField, ...]:
    if raw is None:
        raise ConverterError(
            "{}: extra_fields must be a list (use [] for none), not null".format(origin)
        )
    if isinstance(raw, (str, dict)) or not isinstance(raw, (list, tuple)):
        raise ConverterError("{}: extra_fields must be a list, got {!r}".format(origin, raw))
    fields: List[ExtraField] = []
    seen: Dict[str, int] = {}
    for index, entry in enumerate(raw):
        extra = _parse_extra_field(entry, source_format, origin, index)
        if extra.name in seen:
            raise ConverterError(
                "{}: extra_fields[{}] duplicates the output name {!r} of extra_fields[{}]".format(
                    origin, index, extra.name, seen[extra.name]
                )
            )
        seen[extra.name] = index
        fields.append(extra)
    # Sorted by output name: extra-definition ordering is presentation, not
    # semantics, so it must not change the digest.
    return tuple(sorted(fields, key=lambda f: f.name))


def _parse_binary_layout(raw, origin: str) -> BinaryLayout:
    if not isinstance(raw, dict):
        raise ConverterError("{}: binary_layout must be a mapping".format(origin))
    _check_key_set(raw, _BINARY_LAYOUT_KEYS, "{}: binary_layout".format(origin))

    byte_order = raw["byte_order"]
    if not isinstance(byte_order, str) or byte_order not in BYTE_ORDERS:
        raise ConverterError(
            "{}: binary_layout.byte_order must be one of {}, got {!r}".format(
                origin, sorted(BYTE_ORDERS), byte_order
            )
        )
    itemsize = _require_int(raw["itemsize"], "{}: binary_layout.itemsize".format(origin))
    if not 0 < itemsize <= INT64_MAX:
        raise ConverterError(
            "{}: binary_layout.itemsize must be in [1, {}], got {}".format(
                origin, INT64_MAX, itemsize
            )
        )

    offsets_raw = raw["offsets"]
    if not isinstance(offsets_raw, dict):
        raise ConverterError("{}: binary_layout.offsets must be a mapping".format(origin))
    if not offsets_raw:
        raise ConverterError("{}: binary_layout.offsets is empty".format(origin))
    # Every key is checked before anything sorts them: `sorted()` over a mixed
    # string/int key set raises a bare TypeError from the comparison, which
    # would run before a per-key check placed inside the loop.
    non_string = [key for key in offsets_raw if not isinstance(key, str)]
    if non_string:
        raise ConverterError(
            "{}: binary_layout.offsets has non-string key(s) {!r}".format(origin, non_string)
        )
    offsets: List[Tuple[str, int]] = []
    for field_name in sorted(offsets_raw):
        offset = _require_int(
            offsets_raw[field_name], "{}: binary_layout.offsets.{}".format(origin, field_name)
        )
        if not 0 <= offset <= INT64_MAX:
            raise ConverterError(
                "{}: binary_layout.offsets.{} must be in [0, {}], got {}".format(
                    origin, field_name, INT64_MAX, offset
                )
            )
        offsets.append((field_name, offset))
    return BinaryLayout(byte_order=byte_order, itemsize=itemsize, offsets=tuple(offsets))


def parse_column_map(data: Mapping, origin: str = "<profile>") -> ColumnMap:
    """Validate an already-loaded profile mapping into a frozen ``ColumnMap``.

    Every rejection is fatal: there is no lenient mode, no defaulting of a
    missing key and no guessing of an unknown one.
    """
    if not isinstance(data, Mapping):
        raise ConverterError("{}: profile must be a mapping, got {!r}".format(origin, type(data)))

    source_format = data.get("source_format")
    if source_format not in SOURCE_FORMATS:
        raise ConverterError(
            "{}: source_format must be one of {}, got {!r}".format(
                origin, list(SOURCE_FORMATS), source_format
            )
        )

    allowed = list(_COMMON_PROFILE_KEYS)
    if source_format == "lhalo_binary":
        allowed.append(_BINARY_PROFILE_KEY)
    _check_key_set(data, allowed, origin)

    schema_version = _require_int(data["schema_version"], "{}: schema_version".format(origin))
    if schema_version != PROFILE_SCHEMA_VERSION:
        raise ConverterError(
            "{}: schema_version must be {}, got {}".format(
                origin, PROFILE_SCHEMA_VERSION, schema_version
            )
        )

    binary_layout = None
    if source_format == "lhalo_binary":
        binary_layout = _parse_binary_layout(data[_BINARY_PROFILE_KEY], origin)

    return ColumnMap(
        schema_version=schema_version,
        source_format=source_format,
        required_columns=_parse_required_columns(data["required_columns"], source_format, origin),
        extra_fields=_parse_extra_fields(data["extra_fields"], source_format, origin),
        binary_layout=binary_layout,
        origin=origin,
    )


def load_column_map(path) -> ColumnMap:
    """Load and validate a mapping profile from a YAML file."""
    path = Path(path)
    return parse_column_map(_load_yaml_mapping(path, "mapping profile"), origin=str(path))


def load_source_properties(path) -> Tuple[SourceProperty, ...]:
    """Load an ordered source ``halo_properties.yaml`` declaration list.

    Order is the file's declaration order and is load-bearing: it is the
    record's field order for a fixed-record binary source. Only the
    subset of keys this converter needs is read; unknown property keys are the
    property system's business, not this module's.
    """
    path = Path(path)
    data = _load_yaml_mapping(path, "halo properties")
    raw = data.get("halo_properties")
    if not isinstance(raw, list) or not raw:
        raise ConverterError("{}: halo_properties must be a nonempty list".format(path))
    properties: List[SourceProperty] = []
    seen = set()
    for index, entry in enumerate(raw):
        what = "{}: halo_properties[{}]".format(path, index)
        if not isinstance(entry, dict):
            raise ConverterError("{}: must be a mapping".format(what))
        name = _require_nonempty_str(entry.get("name"), what + ".name")
        if name in seen:
            raise ConverterError("{}: duplicate property name {!r}".format(what, name))
        seen.add(name)
        type_name = entry.get("type")
        if not isinstance(type_name, str) or type_name not in EXTRA_TYPES:
            raise ConverterError(
                "{}: unsupported type {!r} (supported: {})".format(
                    what, type_name, sorted(EXTRA_TYPES)
                )
            )
        properties.append(
            SourceProperty(
                name=name,
                type=type_name,
                units=_require_nonempty_str(entry.get("units"), what + ".units"),
            )
        )
    return tuple(properties)


# ==========================================================================
# Canonical schema
# ==========================================================================


def _build_source_layout(
    layout: BinaryLayout, properties: Sequence[SourceProperty], selected: Iterable[str], origin: str
) -> SourceLayout:
    """Freeze the complete ordered binary record, selected or not.

    Validates that every declared property has an offset, that no offset
    names a property the source does not declare, that no field extends past
    ``itemsize`` and that no two fields overlap. Padding between fields is
    permitted -- a record may legitimately contain bytes this converter never
    reads -- but an unreadable or double-claimed byte range is not.
    """
    offsets = dict(layout.offsets)
    declared = [prop.name for prop in properties]
    missing = sorted(set(declared) - set(offsets))
    if missing:
        raise ConverterError(
            "{}: binary_layout.offsets does not cover source field(s) {}".format(origin, missing)
        )
    unknown = sorted(set(offsets) - set(declared))
    if unknown:
        raise ConverterError(
            "{}: binary_layout.offsets names field(s) {} that the source properties do not "
            "declare".format(origin, unknown)
        )

    selected_names = set(selected)
    entries: List[LayoutEntry] = []
    for prop in properties:
        spec = EXTRA_TYPES[prop.type]
        offset = offsets[prop.name]
        if offset + spec.itemsize > layout.itemsize:
            raise ConverterError(
                "{}: source field {!r} spans bytes [{}, {}) but the record is only {} bytes".format(
                    origin, prop.name, offset, offset + spec.itemsize, layout.itemsize
                )
            )
        entries.append(
            LayoutEntry(
                name=prop.name,
                type=prop.type,
                units=prop.units,
                offset=offset,
                itemsize=spec.itemsize,
                n_components=spec.n_components,
                numpy_dtype=spec.numpy_dtype,
                selected=prop.name in selected_names,
            )
        )

    overlap = _first_overlap(entries)
    if overlap is not None:
        first, second = overlap
        raise ConverterError(
            "{}: source fields {!r} [{}, {}) and {!r} [{}, {}) overlap".format(
                origin,
                first.name,
                first.offset,
                first.offset + first.itemsize,
                second.name,
                second.offset,
                second.offset + second.itemsize,
            )
        )
    _check_declaration_order(entries, origin)
    return SourceLayout(
        byte_order=layout.byte_order, itemsize=layout.itemsize, entries=tuple(entries)
    )


def _check_declaration_order(entries: Sequence[LayoutEntry], origin: str) -> None:
    """Offsets must ascend in the source's declaration order.

    The on-disk record is generated *in* that order, so a profile whose
    offsets disagree with it describes a different record. Coverage, extent
    and overlap all pass when two same-width neighbours are swapped -- the
    bytes are all claimed exactly once -- and an adapter reading through such
    a layout would silently file each field's bytes under the other's name.
    Padding between fields is fine; ordering is not negotiable.
    """
    previous: Optional[LayoutEntry] = None
    for entry in entries:
        if previous is not None and entry.offset < previous.offset:
            raise ConverterError(
                "{}: source field {!r} is declared after {!r} but lies earlier in the record "
                "(offset {} < {}); offsets must follow the declaration order the record is "
                "generated in".format(
                    origin, entry.name, previous.name, entry.offset, previous.offset
                )
            )
        previous = entry


def _check_role_collisions(resolved_roles: Mapping[str, str], origin: Optional[str] = None) -> None:
    """No two required roles may claim the same source field.

    Roles resolve independently, so a copy-paste typo such as
    ``Len: [SnapNum]`` passes every per-role check -- both are ``int``, so the
    type and arity comparison sees nothing wrong -- and would emit snapshot
    numbers as particle counts under a perfectly valid digest.

    Role-to-*extra* reuse is a different thing and stays legal, and
    ``consistent_trees_ascii_extras_example.yaml`` depends on it to carry the
    raw catalog J alongside the normalised Spin. Only role-to-role collisions
    are rejected here.
    """
    claimed: Dict[str, str] = {}
    for role in sorted(resolved_roles):
        spelling = resolved_roles[role]
        if spelling in claimed:
            raise ConverterError(
                _context(
                    origin,
                    "required columns {!r} and {!r} both resolve to the same source field "
                    "{!r}; each role needs its own field".format(claimed[spelling], role, spelling),
                )
            )
        claimed[spelling] = role


def _first_overlap(
    entries: Sequence[LayoutEntry],
) -> Optional[Tuple[LayoutEntry, LayoutEntry]]:
    """The first overlapping pair in ascending offset order, or None.

    Comparing each field only against its immediate successor is sufficient:
    in offset order, any overlap at all forces one adjacent pair to overlap.
    """
    ordered = sorted(entries, key=lambda e: (e.offset, e.name))
    for previous, current in zip(ordered, ordered[1:]):
        if previous.offset + previous.itemsize > current.offset:
            return previous, current
    return None


@dataclass(frozen=True)
class CanonicalSchema:
    """The immutable, hashable identity of one conversion's field mapping.

    ``digest`` is the ``column_mapping_sha256`` a v3 file records: 64
    lowercase hex characters over the canonical JSON below. Anything that
    changes how a value is read, typed, named or labelled changes the digest;
    comments, YAML key order, alias order and extra-definition order do not.
    """

    #: The profile's *own* parsed version, not the module constant. The digest
    #: is durable on-disk provenance: if a version 2 profile grammar is ever
    #: accepted, a previously frozen version 1 mapping must still re-hash to
    #: the value stamped in the files it already produced, which serializing
    #: the constant instead of the parsed value would silently break.
    schema_version: int
    source_format: str
    required_columns: Tuple[Tuple[str, Tuple[str, ...]], ...]
    extra_fields: Tuple[ExtraField, ...]
    payload_fields: Tuple[PayloadField, ...]
    source_layout: Optional[SourceLayout]

    # ---- serialization -------------------------------------------------

    def as_canonical(self) -> Dict[str, object]:
        """Canonical, JSON-ready form. Key order here is irrelevant: the
        serializer sorts keys."""
        document: Dict[str, object] = {
            "extra_fields": [extra.as_canonical() for extra in self.extra_fields],
            "payload_fields": [
                {
                    "description": field.description,
                    "h_convention": field.h_convention,
                    "name": field.name,
                    "type": field.type,
                    "units": field.units,
                }
                for field in self.payload_fields
            ],
            "required_columns": {role: list(aliases) for role, aliases in self.required_columns},
            "schema_version": self.schema_version,
            "source_format": self.source_format,
        }
        if self.source_layout is not None:
            document["source_layout"] = self.source_layout.as_canonical()
        return document

    def canonical_json(self) -> str:
        """Sorted-key, compact, UTF-8-encodable JSON with no NaN."""
        return json.dumps(
            self.as_canonical(),
            sort_keys=True,
            separators=(",", ":"),
            ensure_ascii=False,
            allow_nan=False,
        )

    @property
    def digest(self) -> str:
        try:
            encoded = self.canonical_json().encode("utf-8")
        except UnicodeEncodeError as exc:  # pragma: no cover - validation catches this first
            # Every string is checked for encodability where it is validated,
            # so reaching here means a schema was constructed without going
            # through the parser. Still named rather than raw: the digest is
            # what a v3 file records, and it may not fail anonymously.
            raise ConverterError(
                "schema contains a string that is not encodable as UTF-8: {}".format(exc)
            ) from exc
        return hashlib.sha256(encoded).hexdigest()

    # ---- derived views -------------------------------------------------

    @property
    def roles(self) -> Tuple[str, ...]:
        return tuple(role for role, _aliases in self.required_columns)

    def output_field_declarations(self) -> Tuple[PayloadField, ...]:
        """The ``/schema`` subgroups a v3 file declares.

        One entry per physical/catalog payload field, including ``Len``,
        ``SnapNum`` and ``MostBoundID``, plus every selected extra. Topology
        and the three identity arrays are governed by the fixed format table
        and are deliberately absent -- redeclaring them would create two
        sources of truth for the same contract.
        """
        declared = list(self.payload_fields)
        declared.extend(
            PayloadField(
                name=extra.name,
                type=extra.type,
                units=extra.units,
                h_convention=extra.h_convention,
                description=extra.description,
            )
            for extra in self.extra_fields
        )
        return tuple(declared)

    def consumer_metadata_fragment(self) -> Dict[str, object]:
        """Payload types/units/core-role bindings a future consumer needs.

        ``halo_properties`` lists the payload a ``halo_properties.yaml`` would
        declare; ``format_table_fields`` lists the format-owned topology and
        identity fields with the core role each provides (the five links bind
        the tree-link roles; the target-snapshot and identity fields bind
        none). Deliberately incomplete, and says so: it does *not* describe the
        runtime topology support v3 needs (retained gap state, wide slab
        access), which belongs to the Mimic reader and driver that execute
        the data, not to the converter that produces it.
        """
        return {
            "source_format": self.source_format,
            "column_mapping_sha256": self.digest,
            "complete": False,
            "incomplete_because": (
                "runtime topology support for gapped links and int64 slab indices is not "
                "described here: it belongs to the Mimic horizontal reader and driver that "
                "execute the dataset, not to the converter schema that produced it"
            ),
            "identity_conventions": dict(SOURCE_IDENTITY_CONVENTIONS[self.source_format]),
            "halo_properties": [
                {
                    "name": field.name,
                    "type": field.type,
                    "units": field.units,
                    "h_convention": field.h_convention,
                    "description": field.description,
                    "provides_core_role": _CORE_ROLE_BINDINGS.get(field.name),
                }
                for field in self.output_field_declarations()
            ],
            "format_table_fields": [
                {
                    "name": field.name,
                    "type": field.type,
                    "description": field.description,
                    "provides_core_role": _CORE_ROLE_BINDINGS.get(field.name),
                }
                for field in TOPOLOGY_FIELDS + IDENTITY_FIELDS
            ],
        }


#: Core-role bindings a consumer needs for the required inputs in
#: src/core/core_properties.yaml: the five links are format-table fields, the
#: other three payload. Extras never bind a core role: a declaratively
#: selected field is payload, not a pipeline input.
_CORE_ROLE_BINDINGS = {
    "Descendant": "Descendant",
    "FirstProgenitor": "FirstProgenitor",
    "NextProgenitor": "NextProgenitor",
    "FirstHaloInFOFgroup": "FirstHaloInFOFgroup",
    "NextHaloInFOFgroup": "NextHaloInFOFgroup",
    "SnapNum": "SnapNum",
    "Len": "Len",
    "M_Crit200": "HaloMass",
}


def build_schema(
    column_map: ColumnMap, source_properties: Optional[Sequence[SourceProperty]] = None
) -> CanonicalSchema:
    """Freeze a validated profile into its canonical schema.

    ``source_properties`` is the ordered source ``halo_properties.yaml``
    declaration list and is required for (and only accepted for) a
    ``lhalo_binary`` profile: a fixed-record binary source has an on-disk
    layout to validate, and the other two formats read named objects.
    """
    source_layout = None
    if column_map.source_format == "lhalo_binary":
        if source_properties is None:
            raise ConverterError(
                "{}: a lhalo_binary profile needs the ordered source halo_properties.yaml "
                "to validate its binary_layout".format(column_map.origin)
            )
        if column_map.binary_layout is None:  # pragma: no cover - parse_column_map guarantees it
            raise ConverterError(
                "{}: a lhalo_binary profile has no binary_layout".format(column_map.origin)
            )
        # A fixed-record binary source is the one case where the complete set
        # of source field names is known at freeze time, so resolution happens
        # here rather than being deferred to the adapter. Freezing a mapping
        # whose aliases no source can resolve would mint a
        # column_mapping_sha256 that is durable provenance for a conversion
        # that can never run -- and would silently mark the unresolvable role's
        # layout entry unselected instead of failing.
        property_names = [prop.name for prop in source_properties]
        resolved_roles = _resolve_roles(
            column_map.source_format,
            column_map.required_columns,
            property_names,
            column_map.origin,
            # Deferred, not skipped: see the ordering note at the call below.
            check_collisions=False,
        )
        resolved_extras = _resolve_extras(
            column_map.source_format, column_map.extra_fields, property_names, column_map.origin
        )
        # Type before collision, deliberately: a role pointed at a field of
        # the wrong type is the more specific defect, and reporting "two roles
        # share a field" for `M_Crit200: [Pos]` would send an author looking
        # for the wrong mistake.
        _check_role_types(
            resolved_roles, source_properties, column_map.source_format, column_map.origin
        )
        _check_role_collisions(resolved_roles, column_map.origin)
        _check_component_arity(
            column_map.extra_fields, resolved_extras, source_properties, column_map.origin
        )
        _check_extra_types(
            column_map.extra_fields, resolved_extras, source_properties, column_map.origin
        )
        selected = set(resolved_roles.values())
        for components in resolved_extras.values():
            selected.update(spelling for spelling, _component in components)
        source_layout = _build_source_layout(
            column_map.binary_layout, source_properties, selected, column_map.origin
        )
    elif source_properties is not None:
        raise ConverterError(
            "{}: source_properties apply to lhalo_binary profiles only, not {}".format(
                column_map.origin, column_map.source_format
            )
        )

    return CanonicalSchema(
        schema_version=column_map.schema_version,
        source_format=column_map.source_format,
        required_columns=column_map.required_columns,
        extra_fields=column_map.extra_fields,
        payload_fields=PAYLOAD_FIELDS[column_map.source_format],
        source_layout=source_layout,
    )


def _check_role_types(
    resolved_roles: Mapping[str, str],
    source_properties: Sequence[SourceProperty],
    source_format: str,
    origin: str,
) -> None:
    """Check each resolved required role against its source field's real type.

    Compared as the **whole** type, arity included, not merely the element
    dtype: a scalar role filled from a stored vector (``M_Crit200: [Pos]``)
    would pass an element-only comparison, because a ``vec3_float``'s elements
    are ``float32`` exactly like a ``float``'s. It is still wrong -- the role
    wants one value and the source offers three.

    A mismatch here is not a tolerance to widen. Freezing it would mint a
    ``column_mapping_sha256`` -- durable on-disk provenance -- for a mapping no
    adapter can honestly satisfy, and would have to be resolved later by either
    a silent fill or a silent cast, both of which are forbidden.
    """
    expected_by_role = ROLE_SOURCE_TYPES[source_format]
    by_name = {prop.name: prop for prop in source_properties}
    for role, spelling in sorted(resolved_roles.items()):
        expected = expected_by_role[role]
        actual = by_name[spelling].type
        if actual != expected:
            raise ConverterError(
                "{}: required column {!r} resolves to source field {!r}, which is {}, but the "
                "role is {}".format(origin, role, spelling, actual, expected)
            )


def _check_extra_types(
    extra_fields: Sequence[ExtraField],
    resolved: Mapping[str, Tuple[Tuple[str, Optional[int]], ...]],
    source_properties: Sequence[SourceProperty],
    origin: str,
) -> None:
    """Check each extra's declared type against its source field's real type.

    Compared as the **element** dtype, because arity is legitimately free here
    and is already checked separately: one component of a stored vector may
    fill a scalar output (``PosX <- Pos component 0``), and three scalar
    sources may fill a vector output. What may not differ is the stored
    element type -- selecting the int32 ``FileNr`` as a ``float`` would push an
    integer through floating point, which is forbidden outright, and selecting a
    ``long long`` as an ``int`` would narrow it silently.
    """
    by_name = {prop.name: prop for prop in source_properties}
    for extra in extra_fields:
        declared_element = extra.spec.numpy_dtype
        for position, (spelling, _component) in enumerate(resolved[extra.name]):
            source_element = EXTRA_TYPES[by_name[spelling].type].numpy_dtype
            if source_element != declared_element:
                raise ConverterError(
                    "{}: extra field {!r} source[{}]: source field {!r} stores {} ({}), but the "
                    "extra is declared {} ({})".format(
                        origin,
                        extra.name,
                        position,
                        spelling,
                        by_name[spelling].type,
                        source_element,
                        extra.type,
                        declared_element,
                    )
                )


def _check_component_arity(
    extra_fields: Sequence[ExtraField],
    resolved: Mapping[str, Tuple[Tuple[str, Optional[int]], ...]],
    source_properties: Sequence[SourceProperty],
    origin: str,
) -> None:
    """Check each source component against the declared property's real shape.

    The grammar is ``{field}`` for a stored scalar and ``{field, component}``
    for one element of a stored vector, so the two are not interchangeable: a
    ``component`` on a scalar names an element that does not exist, and a
    missing ``component`` on a vector does not say which of three values to
    read. Only a fixed-record binary source declares its shapes in a profile;
    for the two named-object formats the equivalent check is an
    adapter-read-time obligation (see ``adapters/base.py``).
    """
    by_name = {prop.name: prop for prop in source_properties}
    for extra in extra_fields:
        for position, (spelling, component) in enumerate(resolved[extra.name]):
            prop = by_name[spelling]
            what = "{}: extra field {!r} source[{}]".format(origin, extra.name, position)
            is_vector = EXTRA_TYPES[prop.type].n_components == 3
            if is_vector and component is None:
                raise ConverterError(
                    "{}: source field {!r} is {}, a 3-component vector, so it needs an explicit "
                    "'component: 0|1|2'".format(what, spelling, prop.type)
                )
            if not is_vector and component is not None:
                raise ConverterError(
                    "{}: source field {!r} is the scalar type {}, so it must not carry a "
                    "'component'".format(what, spelling, prop.type)
                )


# ==========================================================================
# Alias resolution against a concrete source
# ==========================================================================


def _normalized_available(source_format: str, available: Iterable[str]) -> Dict[str, str]:
    """Map normalized source column name -> the source's own spelling.

    Two *distinct* source columns that normalize to the same name are
    rejected, because picking either one would be a silent guess. An exact
    repeat of the same spelling is not: ``available`` is a caller-supplied
    iterable, and a repeated entry in it says nothing about whether the file
    really carries the column twice. Detecting a genuinely duplicated column
    is the reading adapter's job, at the point it enumerates the file's own
    header or dataset list -- this function never sees the file.
    """
    resolved: Dict[str, str] = {}
    for name in available:
        canonical = normalize_alias(source_format, name)
        if canonical in resolved and resolved[canonical] != name:
            raise ConverterError(
                "source declares both {!r} and {!r}, which normalize to the same column "
                "{!r}".format(resolved[canonical], name, canonical)
            )
        resolved[canonical] = name
    return resolved


def _resolve_one(aliases: Sequence[str], lookup: Mapping[str, str], what: str) -> str:
    matches = [lookup[alias] for alias in aliases if alias in lookup]
    if not matches:
        raise ConverterError(
            "{}: none of the aliases {} is present in the source".format(what, list(aliases))
        )
    if len(matches) > 1:
        raise ConverterError(
            "{}: aliases {} resolve ambiguously to {} -- exactly one must match".format(
                what, list(aliases), sorted(matches)
            )
        )
    return matches[0]


def _context(origin: Optional[str], what: str) -> str:
    return what if origin is None else "{}: {}".format(origin, what)


def _resolve_roles(
    source_format: str,
    required_columns: Sequence[Tuple[str, Tuple[str, ...]]],
    available: Iterable[str],
    origin: Optional[str] = None,
    check_collisions: bool = True,
) -> Dict[str, str]:
    """Resolve every role, rejecting a role-to-role collision by default.

    The default is on so that a caller gets the protection without having to
    remember a second call -- the ASCII and forests-HDF5 adapters resolve
    through :func:`resolve_required_columns`, and that is where the check has
    to bite for them. ``build_schema`` opts out and runs the check itself a
    little later, purely to keep a more specific error ahead of a more general
    one; see its call site.
    """
    lookup = _normalized_available(source_format, available)
    resolved = {
        role: _resolve_one(aliases, lookup, _context(origin, "required column {!r}".format(role)))
        for role, aliases in required_columns
    }
    if check_collisions:
        _check_role_collisions(resolved, origin)
    return resolved


def _resolve_extras(
    source_format: str,
    extra_fields: Sequence[ExtraField],
    available: Iterable[str],
    origin: Optional[str] = None,
) -> Dict[str, Tuple[Tuple[str, Optional[int]], ...]]:
    lookup = _normalized_available(source_format, available)
    resolved: Dict[str, Tuple[Tuple[str, Optional[int]], ...]] = {}
    for extra in extra_fields:
        components: List[Tuple[str, Optional[int]]] = []
        for position, component in enumerate(extra.sources):
            what = _context(origin, "extra field {!r} source[{}]".format(extra.name, position))
            if component.field not in lookup:
                raise ConverterError(
                    "{}: the source does not carry field {!r}".format(what, component.field)
                )
            components.append((lookup[component.field], component.component))
        resolved[extra.name] = tuple(components)
    return resolved


def resolve_required_columns(schema: CanonicalSchema, available: Iterable[str]) -> Dict[str, str]:
    """Resolve every required role against one concrete source's column names.

    Returns role -> the source's own spelling. Exactly one alias must resolve
    for each role in each file; zero matches and two matches are both
    fatal, and nothing is defaulted or filled.
    """
    return _resolve_roles(schema.source_format, schema.required_columns, available)


def resolve_extra_sources(
    schema: CanonicalSchema, available: Iterable[str]
) -> Dict[str, Tuple[Tuple[str, Optional[int]], ...]]:
    """Resolve every extra field's source components against one source.

    Returns output name -> one ``(source spelling, component)`` pair per
    component, in the declared order, where ``component`` is ``None`` for a
    stored scalar and 0/1/2 for an element of a stored vector. The component
    index is part of the resolution, not a detail to be recovered separately:
    ``PosX <- Pos component 0`` and "read the whole Pos vector" resolve to the
    same spelling and must stay distinguishable from this return value alone.

    A selected field that the source does not carry is fatal: there is no
    guessed default and no silent fill.
    """
    return _resolve_extras(schema.source_format, schema.extra_fields, available)
