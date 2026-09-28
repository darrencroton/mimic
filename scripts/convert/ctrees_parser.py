"""Phase 1 ctrees ASCII parsing for the ctrees -> horizontal-HDF5 converter.

Owns the frozen scratch-record dtype, both Consistent-Trees header dialects
(indexed ``#scale(0) id(1) ...`` primary, ``#fields:`` secondary), ``#tree``
block-marker tracking, chunked pandas reads, and the independent row pre-count.

Reference semantics mirrored here (the converter implementation plan is archived
under archive/dev-plans/; the reference sources cited below are authoritative):
- column names are truncated at the first ``(`` and matched case-insensitively
  (src/io/vertical/ctrees/parse_ctrees.h); ``snap_idx``/``snap_num`` are equivalent
  spellings of the snapshot column (src/io/vertical/read_ctrees_ascii.c
  setup_column_info);
- floats are parsed as float64 and cast to float32 at record assembly, matching
  the reference strtod-then-cast parse path;
- duplicate or missing required columns abort; malformed rows abort.

**Declarative selection** (contracts C1/C2 of
docs/dev/MIMIC-CONVERTER-GENERALISATION-IMPLEMENTATION-PLAN.md). Passing
a ``consistent_trees_ascii`` :class:`column_schema.CanonicalSchema` selects the
required columns through the profile's aliases and carries its declared extra
fields in an *extended* scratch layout (:class:`ScratchLayout`), which also
records every row's canonical source coordinate. Without a schema nothing
changes: the frozen 108-byte ``RECORD_DTYPE``, ``DTYPE_TAG`` and the legacy
column resolution are exactly what the ASCII-to-v2 workflow has always used.

Extra-field parse rules, stated here because an ASCII column has no declared
type until it is parsed (``adapters/base.py`` notes):

- an ``int``/``long long``/``vec3_int`` extra is parsed from its text, never
  through floating point: every token must be a base-10 integer literal (a
  fraction, an exponent or an NA token fails), values above 2**53 stay exact,
  and int64 overflow or an ``int`` value outside int32 fails;
- a ``float``/``double``/``vec3_float`` extra follows the core path: float64
  parse, a non-finite value fails, and ``float`` is then cast to float32 with a
  float32 overflow failing. Finite underflow follows the NumPy cast and signed
  zero is preserved;
- every failure names the file, the column, the extra and data-row ordinals;
- a column that fills a required role keeps that role's parse type: an integer
  extra may not read a floating role column and a floating extra may not read
  an integer role column. A column that fills no role is typed by its
  declaration alone -- real ctrees files print some floating columns as integer
  literals (``Mvir_all`` in micro-Uchuu), so the text cannot type it -- and two
  extras may not declare the same such column with different types;
- ASCII columns are scalars, so a ``{field, component}`` source is rejected.
"""

import hashlib
import re
from dataclasses import dataclass, field
from pathlib import Path
from typing import Dict, Iterator, List, Optional, Set, Tuple

import numpy as np
import pandas as pd
from column_schema import EXTRA_TYPES, resolve_extra_sources, resolve_required_columns
from errors import ConverterError

#: Frozen scratch-record dtype: little-endian, packed, itemsize 108 bytes.
#: Field order and widths never change; every legacy scratch-file manifest
#: entry records DTYPE_TAG against it.
RECORD_DTYPE = np.dtype(
    [
        ("id", "<i8"),
        ("desc_id", "<i8"),
        ("desc_scale", "<f8"),
        ("pid", "<i8"),
        ("upid", "<i8"),
        ("snap", "<i4"),
        ("Mvir", "<f4"),
        ("X", "<f4"),
        ("Y", "<f4"),
        ("Z", "<f4"),
        ("VX", "<f4"),
        ("VY", "<f4"),
        ("VZ", "<f4"),
        ("Jx", "<f4"),
        ("Jy", "<f4"),
        ("Jz", "<f4"),
        ("vrms", "<f4"),
        ("vmax", "<f4"),
        ("tree_root_id", "<i8"),
        ("forest_id", "<i8"),
    ],
    align=False,
)

#: Human-readable dtype identity recorded in every manifest.
DTYPE_TAG = "ctrees-scratch-v1/itemsize=108/" + ",".join(
    "{}:{}".format(name, RECORD_DTYPE.fields[name][0].str) for name in RECORD_DTYPE.names
)

#: Canonical source-coordinate fields an extended scratch record carries after
#: the frozen 20 (C1: ``(source_file_ordinal, unit_ordinal, row_ordinal)``).
SOURCE_KEY_FIELDS = (
    ("src_file_ordinal", "<i8"),
    ("src_unit_ordinal", "<i8"),
    ("src_row_ordinal", "<i8"),
)

#: Scratch field-name prefix for a selected extra, so an output name such as
#: ``X`` or ``id`` can never collide with a frozen scratch field.
EXTRA_FIELD_PREFIX = "extra_"

#: Version prefix of an extended scratch dtype tag. Deliberately different from
#: the frozen ``ctrees-scratch-v1`` prefix: a converter that predates the
#: extended layout compares tags for equality and so refuses such a workdir.
EXTENDED_DTYPE_TAG_PREFIX = "ctrees-scratch-v2"

#: Version of the ``scratch_layout`` manifest record.
SCRATCH_LAYOUT_RECORD_VERSION = 1


def describe_dtype(prefix: str, dtype: np.dtype) -> str:
    """Human-readable dtype identity: prefix, itemsize, then every field's
    name and little-endian element type, with a ``[n]`` suffix for a subarray."""
    parts = []
    for name in dtype.names:
        sub = dtype.fields[name][0]
        text = "{}:{}".format(name, sub.base.str)
        if sub.shape:
            text += "[{}]".format(",".join(str(n) for n in sub.shape))
        parts.append(text)
    return "{}/itemsize={}/{}".format(prefix, dtype.itemsize, ",".join(parts))


def _little_endian_element(spec) -> str:
    """Scratch element dtype of a declared extra type: the
    ``column_schema.EXTRA_TYPES`` element, always little-endian (``<i4`` etc.)."""
    return np.dtype(spec.numpy_dtype).newbyteorder("<").str


@dataclass(frozen=True)
class ScratchLayout:
    """Which scratch record a workdir holds: the frozen legacy one, or the
    extended one of the canonical ASCII bridge.

    The legacy layout is ``RECORD_DTYPE`` itself, byte for byte, under the
    frozen ``DTYPE_TAG``. The extended layout appends the three source
    coordinates and every selected extra (in the schema's sorted order) to
    those 20 fields; it is identified by its own tag *and* by the digest of the
    schema that selected it, so two same-width schemas that differ only in
    units or descriptions still refuse to share a workdir.
    """

    extras: Tuple[Tuple[str, str], ...] = ()
    schema_digest: Optional[str] = None

    @property
    def is_extended(self) -> bool:
        return self.schema_digest is not None

    @property
    def dtype(self) -> np.dtype:
        if not self.is_extended:
            return RECORD_DTYPE
        fields: List[tuple] = [
            (name, RECORD_DTYPE.fields[name][0].str) for name in RECORD_DTYPE.names
        ]
        fields.extend(SOURCE_KEY_FIELDS)
        for name, type_name in self.extras:
            spec = EXTRA_TYPES[type_name]
            element = _little_endian_element(spec)
            if spec.n_components == 1:
                fields.append((EXTRA_FIELD_PREFIX + name, element))
            else:
                fields.append((EXTRA_FIELD_PREFIX + name, element, (spec.n_components,)))
        return np.dtype(fields, align=False)

    @property
    def dtype_tag(self) -> str:
        if not self.is_extended:
            return DTYPE_TAG
        return describe_dtype(EXTENDED_DTYPE_TAG_PREFIX, self.dtype)

    @classmethod
    def from_schema(cls, schema) -> "ScratchLayout":
        """The extended layout a ``consistent_trees_ascii`` schema selects."""
        if schema.source_format != "consistent_trees_ascii":
            raise ConverterError(
                "the ctrees ASCII scratch layout needs a consistent_trees_ascii schema, "
                "got {!r}".format(schema.source_format)
            )
        extras = tuple(
            (extra.name, extra.type) for extra in sorted(schema.extra_fields, key=lambda e: e.name)
        )
        for _name, type_name in extras:
            if type_name not in EXTRA_TYPES:  # pragma: no cover - column_schema rejects it
                raise ConverterError("unsupported extra type {!r}".format(type_name))
        return cls(extras=extras, schema_digest=schema.digest)

    def to_record(self) -> Dict[str, object]:
        """Manifest record of an extended layout; the legacy layout has none."""
        return {
            "version": SCRATCH_LAYOUT_RECORD_VERSION,
            "extras": [[name, type_name] for name, type_name in self.extras],
            "schema_digest": self.schema_digest,
            "dtype_tag": self.dtype_tag,
        }

    @classmethod
    def from_record(cls, record, context: str) -> "ScratchLayout":
        """Rebuild an extended layout from its manifest record, refusing a
        record this code would not have written: the tag is recomputed from
        the recorded extras and must match the recorded one exactly."""
        try:
            if record["version"] != SCRATCH_LAYOUT_RECORD_VERSION:
                raise ConverterError(
                    "{}: scratch layout record version {!r} != supported {}".format(
                        context, record["version"], SCRATCH_LAYOUT_RECORD_VERSION
                    )
                )
            extras = tuple((str(name), str(type_name)) for name, type_name in record["extras"])
            digest = record["schema_digest"]
            recorded_tag = record["dtype_tag"]
        except (KeyError, TypeError, ValueError) as exc:
            raise ConverterError(
                "{}: malformed scratch layout record ({!r})".format(context, exc)
            ) from exc
        if not isinstance(digest, str) or not re.fullmatch(r"[0-9a-f]{64}", digest):
            raise ConverterError(
                "{}: scratch layout schema digest {!r} is not 64 lowercase hex".format(
                    context, digest
                )
            )
        unknown = [type_name for _name, type_name in extras if type_name not in EXTRA_TYPES]
        if unknown or list(extras) != sorted(extras):
            raise ConverterError(
                "{}: scratch layout record lists unsupported or unsorted extras {!r}".format(
                    context, list(extras)
                )
            )
        layout = cls(extras=extras, schema_digest=digest)
        if layout.dtype_tag != recorded_tag:
            raise ConverterError(
                "{}: scratch layout dtype tag {!r} does not match the tag its recorded extras "
                "describe ({!r}) -- refusing to reinterpret the scratch records".format(
                    context, recorded_tag, layout.dtype_tag
                )
            )
        return layout


#: The frozen legacy layout: the default ASCII-to-v2 route.
LEGACY_LAYOUT = ScratchLayout()

DEFAULT_CHUNKSIZE = 1_000_000

#: Required ctrees columns (normalized lowercase) -> parse dtype.
#: The snapshot column is handled separately because it has two spellings.
_INT_COLUMNS = ("id", "desc_id", "pid", "upid")
_FLOAT_COLUMNS = (
    "scale",
    "desc_scale",
    "mvir",
    "vrms",
    "vmax",
    "x",
    "y",
    "z",
    "vx",
    "vy",
    "vz",
    "jx",
    "jy",
    "jz",
)
#: Older ctrees files use snap_num, newer use snap_idx (read_ctrees_ascii.c).
SNAPSHOT_SPELLINGS = ("snap_idx", "snap_num")

#: Required roles parsed as int64; every other role is parsed as float64.
_INT_ROLES = frozenset(_INT_COLUMNS + ("snap",))

#: record field <- ctrees column (both normalized) for direct copies.
_RECORD_FROM_COLUMN = {
    "id": "id",
    "desc_id": "desc_id",
    "desc_scale": "desc_scale",
    "pid": "pid",
    "upid": "upid",
    "Mvir": "mvir",
    "X": "x",
    "Y": "y",
    "Z": "z",
    "VX": "vx",
    "VY": "vy",
    "VZ": "vz",
    "Jx": "jx",
    "Jy": "jy",
    "Jz": "jz",
    "vrms": "vrms",
    "vmax": "vmax",
}


@dataclass
class PreScan:
    """Marker/count evidence from the pandas-independent pre-scan of one file."""

    path: str
    header_line: str
    n_rows: int
    #: data-row ordinal at which each ``#tree`` block starts (ascending)
    tree_start_rows: np.ndarray
    #: tree root id for each block, aligned with tree_start_rows
    tree_root_ids: np.ndarray
    md5: str
    size: int
    #: st_mtime_ns captured before the scan; the scatter stage re-stats after
    #: the pandas pass and aborts if the source changed between the two reads
    mtime_ns: int
    #: ctrees files carry a bare integer tree-count line between the header
    #: comments and the first '#tree' marker (verified against tree_0_0_0.dat);
    #: None when absent. The scatter stage validates it against the marker count.
    declared_tree_count: Optional[int] = None
    #: physical 0-based line index of the count line (for pandas skiprows)
    count_line_index: Optional[int] = None


def normalize_column_name(token: str) -> str:
    """Truncate at the first '(' — reference parse_ctrees.h suffix stripping."""
    return token.split("(", 1)[0]


def parse_header_line(header_line: str) -> List[str]:
    """Return normalized (suffix-stripped, original-case) column names.

    Accepts the indexed primary dialect (``#scale(0) id(1) ...``) and the
    ``#fields:`` secondary dialect. Tokens are split on whitespace and commas,
    matching the reference delimiter set (space, comma, newline, '#').
    """
    if not header_line.startswith("#"):
        raise ConverterError(
            "ctrees header line must start with '#', got: {!r}".format(header_line[:80])
        )
    body = header_line.lstrip("#").strip()
    if body.lower().startswith("fields:"):
        body = body[len("fields:") :]
    tokens = [t for t in re.split(r"[,\s]+", body) if t]
    names = [normalize_column_name(t) for t in tokens]
    names = [n for n in names if n]
    if not names:
        raise ConverterError(
            "ctrees header line contains no column names: {!r}".format(header_line[:80])
        )
    return names


@dataclass(frozen=True)
class ExtraColumns:
    """One selected extra resolved against one file's header."""

    #: the extra's output name, and its scratch field (EXTRA_FIELD_PREFIX + name)
    name: str
    field: str
    #: the declared type (a column_schema.EXTRA_TYPES key)
    type: str
    #: one entry per component: (column index, normalized-lowercase spelling)
    columns: Tuple[Tuple[int, str], ...]

    @property
    def spec(self):
        return EXTRA_TYPES[self.type]


@dataclass
class ColumnLayout:
    """Resolved required-column indices for one file's header."""

    all_names: List[str]
    #: normalized-lowercase required column -> column index in the file
    indices: Dict[str, int]
    #: which snapshot spelling the file uses (normalized lowercase), under
    #: either resolution
    snapshot_column: str
    #: key of the snapshot column in ``indices``. Both fields exist because the
    #: resolutions key ``indices`` differently: the legacy one by spelling (so
    #: this equals ``snapshot_column``), the profile one by the role name
    #: ``snap`` (so the spelling is only in ``snapshot_column``)
    snapshot_key: str = ""
    #: selected extras (schema-driven resolution only)
    extras: Tuple[ExtraColumns, ...] = ()
    #: column indices read as text for an integer extra (no role reads them)
    text_columns: frozenset = frozenset()

    def __post_init__(self):
        if not self.snapshot_key:
            self.snapshot_key = self.snapshot_column

    def is_integer_role(self, key: str) -> bool:
        """Whether the ``indices`` entry ``key`` fills an int64 role (``_INT_ROLES``),
        with the snapshot column recognised under either resolution's key."""
        return ("snap" if key == self.snapshot_key else key) in _INT_ROLES


def resolve_columns(names: List[str]) -> ColumnLayout:
    """Map required columns to file column indices, aborting per the contract.

    Case-insensitive first-match semantics follow the reference
    match_column_name(); unlike the reference, a duplicated required column or
    both snapshot spellings at once abort.
    """
    lowered = [n.lower() for n in names]
    counts: Dict[str, int] = {}
    for name in lowered:
        counts[name] = counts.get(name, 0) + 1

    required = list(_INT_COLUMNS) + list(_FLOAT_COLUMNS)
    duplicates = [name for name in required if counts.get(name, 0) > 1]
    duplicates += [s for s in SNAPSHOT_SPELLINGS if counts.get(s, 0) > 1]
    if duplicates:
        raise ConverterError(
            "duplicate required column(s) in ctrees header: {}".format(
                ", ".join(sorted(duplicates))
            )
        )

    snapshot_present = [s for s in SNAPSHOT_SPELLINGS if s in counts]
    if len(snapshot_present) > 1:
        raise ConverterError(
            "ambiguous snapshot column: both {} present in ctrees header".format(
                " and ".join(snapshot_present)
            )
        )
    if not snapshot_present:
        raise ConverterError(
            "missing required column(s) in ctrees header: one of {}".format(
                "/".join(SNAPSHOT_SPELLINGS)
            )
        )
    snapshot_column = snapshot_present[0]

    indices: Dict[str, int] = {}
    missing: List[str] = []
    for wanted in required + [snapshot_column]:
        try:
            indices[wanted] = lowered.index(wanted)
        except ValueError:
            missing.append(wanted)
    if missing:
        raise ConverterError(
            "missing required column(s) in ctrees header: {}".format(", ".join(sorted(missing)))
        )
    return ColumnLayout(all_names=names, indices=indices, snapshot_column=snapshot_column)


#: The only token shape an integer extra accepts (ASCII digits, optional sign).
_INTEGER_LITERAL = r"[+-]?[0-9]+"


def resolve_selection(names: List[str], schema) -> ColumnLayout:
    """Resolve a ``consistent_trees_ascii`` schema's roles and extras against
    one file's header (C2).

    Matching is suffix-stripped and case-insensitive, as in the legacy
    resolution, and exactly one alias must resolve for each role
    (``column_schema.resolve_required_columns``, which also rejects two roles
    resolving to one column). A selected column that the header carries more
    than once is ambiguous and aborts; an *unselected* duplicate does not, as
    in the legacy resolution -- real headers repeat ``b_to_a`` and ``A[x]`` once
    suffix-stripped. Extra typing rules are the module docstring's.
    """
    if schema.source_format != "consistent_trees_ascii":
        what = schema.source_format
        raise ConverterError(
            "ctrees ASCII column selection needs a consistent_trees_ascii schema, got {!r}".format(
                what
            )
        )
    lowered = [n.lower() for n in names]
    counts: Dict[str, int] = {}
    for name in lowered:
        counts[name] = counts.get(name, 0) + 1
    available = list(dict.fromkeys(lowered))

    roles = resolve_required_columns(schema, available)
    duplicates = sorted(spelling for spelling in roles.values() if counts[spelling] > 1)
    if duplicates:
        raise ConverterError(
            "duplicate required column(s) in ctrees header: {}".format(", ".join(duplicates))
        )
    indices = {role: lowered.index(spelling) for role, spelling in roles.items()}
    role_of_spelling = {spelling: role for role, spelling in roles.items()}

    extras: List[ExtraColumns] = []
    text_declared: Dict[str, str] = {}
    float_declared: Dict[str, str] = {}
    for extra_name, components in sorted(resolve_extra_sources(schema, available).items()):
        extra = next(e for e in schema.extra_fields if e.name == extra_name)
        is_integer = extra.spec.is_integer
        columns = []
        for position, (spelling, component) in enumerate(components):
            what = "extra field {!r} source[{}] (column {!r})".format(
                extra_name, position, spelling
            )
            if component is not None:
                raise ConverterError(
                    "{}: ctrees ASCII columns are scalars, so 'component: {}' names an element "
                    "that does not exist".format(what, component)
                )
            if counts[spelling] > 1:
                raise ConverterError(
                    "{}: the ctrees header carries this column {} times -- which one is meant "
                    "is ambiguous".format(what, counts[spelling])
                )
            role = role_of_spelling.get(spelling)
            if role is not None:
                role_is_integer = role in _INT_ROLES
                if role_is_integer != is_integer:
                    raise ConverterError(
                        "{}: the column fills the {} required role {!r}, but the extra is "
                        "declared {}; integers never pass through floating point, and a floating "
                        "column is never read as an integer".format(
                            what, "integer" if role_is_integer else "floating", role, extra.type
                        )
                    )
            else:
                mine, other = (
                    (text_declared, float_declared)
                    if is_integer
                    else (float_declared, text_declared)
                )
                if spelling in other:
                    raise ConverterError(
                        "{}: extra {!r} declares the same column with a different number "
                        "class; one column has one type".format(what, other[spelling])
                    )
                mine.setdefault(spelling, extra_name)
            columns.append((lowered.index(spelling), spelling))
        extras.append(
            ExtraColumns(
                name=extra_name,
                field=EXTRA_FIELD_PREFIX + extra_name,
                type=extra.type,
                columns=tuple(columns),
            )
        )
    return ColumnLayout(
        all_names=names,
        indices=indices,
        snapshot_column=roles["snap"],
        snapshot_key="snap",
        extras=tuple(extras),
        text_columns=frozenset(lowered.index(spelling) for spelling in text_declared),
    )


@dataclass(frozen=True)
class SourceUnitPlan:
    """How one file's ``#tree`` blocks group into canonical source units (C1).

    A unit is the part of one ASCII forest that one file carries: every
    ``#tree`` block of that file whose root belongs to the forest. Units are
    numbered in order of their forest's first ``#tree`` marker in the file, and
    a row's ``row_ordinal`` is its position among its unit's rows in file order
    -- the source's own order, never a re-sorted one. A forest whose trees lie
    in several files is several units, one per file; the sidecar convention
    (C3: -1 ordinals for a forest spanning files) follows from that.
    """

    source_file_ordinal: int
    #: per ``#tree`` marker, in file order
    unit_of_tree: np.ndarray
    unit_row_base: np.ndarray
    #: per unit, in unit order
    unit_forest_ids: np.ndarray
    unit_counts: np.ndarray

    def table(self) -> np.ndarray:
        """``[n_units, 2]`` int64 (forest id, halo count), the scatter sidecar."""
        return np.column_stack((self.unit_forest_ids, self.unit_counts)).astype(np.int64)


def plan_source_units(
    prescan: "PreScan", tree_forest_ids: np.ndarray, source_file_ordinal: int
) -> SourceUnitPlan:
    """Group one file's trees into source units from the pre-scan alone.

    Pure arithmetic over the marker table (O(trees in the file)); no data row
    is read, so the plan is independent of the pandas pass whose attribution
    it later checks.
    """
    starts = np.asarray(prescan.tree_start_rows, dtype=np.int64)
    forests = np.asarray(tree_forest_ids, dtype=np.int64)
    if starts.shape != forests.shape:
        raise ConverterError(
            "{}: {} tree marker(s) but {} forest id(s)".format(
                prescan.path, starts.size, forests.size
            )
        )
    n_trees = starts.size
    if n_trees == 0:
        empty = np.zeros(0, dtype=np.int64)
        return SourceUnitPlan(int(source_file_ordinal), empty, empty, empty, empty)
    sizes = np.diff(np.r_[starts, np.int64(prescan.n_rows)])
    unique_forests, first_marker, inverse = np.unique(
        forests, return_index=True, return_inverse=True
    )
    # unit ordinal = rank of the forest's first marker in file order
    by_first = np.argsort(first_marker, kind="stable")
    unit_of_unique = np.empty(unique_forests.size, dtype=np.int64)
    unit_of_unique[by_first] = np.arange(unique_forests.size, dtype=np.int64)
    unit_of_tree = unit_of_unique[inverse.reshape(-1)]
    unit_forest_ids = unique_forests[by_first]
    unit_counts = np.zeros(unique_forests.size, dtype=np.int64)
    np.add.at(unit_counts, unit_of_tree, sizes)
    # exclusive running size of earlier trees of the same unit, in file order
    order = np.argsort(unit_of_tree, kind="stable")
    sorted_sizes = sizes[order]
    running = np.cumsum(sorted_sizes) - sorted_sizes
    sorted_units = unit_of_tree[order]
    group_start = np.r_[True, sorted_units[1:] != sorted_units[:-1]]
    group_base = np.maximum.accumulate(np.where(group_start, running, 0))
    unit_row_base = np.empty(n_trees, dtype=np.int64)
    unit_row_base[order] = running - group_base
    return SourceUnitPlan(
        source_file_ordinal=int(source_file_ordinal),
        unit_of_tree=unit_of_tree,
        unit_row_base=unit_row_base,
        unit_forest_ids=unit_forest_ids,
        unit_counts=unit_counts,
    )


def prescan_file(path) -> PreScan:
    """Stream the file once, independent of pandas: header, ``#tree`` markers,
    valid data-row count, size, and md5.

    This is the independent row pre-count of plan review finding 7: the parsed
    row count must equal ``n_rows`` exactly before the file's result is
    accepted. Every data row must also have exactly as many whitespace-separated
    tokens as the header declares columns — an explicit structural invariant
    independent of the pandas column projection.
    """
    path = Path(path)
    stat_before = path.stat()
    md5 = hashlib.md5()
    tree_start_rows: List[int] = []
    tree_root_ids: List[int] = []
    n_rows = 0
    header_line: Optional[str] = None
    expected_tokens: Optional[int] = None
    declared_tree_count: Optional[int] = None
    count_line_index: Optional[int] = None
    with open(path, "rb") as handle:
        for lineno, raw in enumerate(handle, start=1):
            md5.update(raw)
            line = raw.strip()
            if header_line is None:
                if not line.startswith(b"#"):
                    raise ConverterError(
                        "{}: first line must be a '#' header, got: {!r}".format(
                            path, line[:80].decode("utf-8", "replace")
                        )
                    )
                header_line = line.decode("utf-8", "replace")
                expected_tokens = len(parse_header_line(header_line))
                continue
            if not line:
                continue
            if line.startswith(b"#"):
                # a marker's first token must be exactly '#tree' — prefixes
                # like '#treejunk' are ordinary comments, and a real '#tree'
                # marker with wrong arity or a non-integer id aborts
                parts = line.split()
                if parts[0] == b"#tree":
                    if len(parts) != 2:
                        raise ConverterError(
                            "{}:{}: malformed '#tree' marker: {!r}".format(
                                path, lineno, line[:80].decode("utf-8", "replace")
                            )
                        )
                    try:
                        root_id = int(parts[1])
                    except ValueError:
                        raise ConverterError(
                            "{}:{}: non-integer '#tree' root id: {!r}".format(
                                path, lineno, parts[1][:40].decode("utf-8", "replace")
                            )
                        )
                    tree_start_rows.append(n_rows)
                    tree_root_ids.append(root_id)
                continue
            if b"#" in line:
                # pandas comment='#' would silently drop the row tail, hiding
                # it from the structural invariant — never legal in ctrees data
                raise ConverterError(
                    "{}:{}: inline '#' in data row: {!r}".format(
                        path, lineno, line[:80].decode("utf-8", "replace")
                    )
                )
            if not tree_start_rows:
                # before the first '#tree' marker only the bare tree-count
                # line is legal (real ctrees layout, e.g. '561266')
                tokens = line.split()
                if len(tokens) == 1 and declared_tree_count is None:
                    try:
                        declared_tree_count = int(tokens[0])
                    except ValueError:
                        raise ConverterError(
                            "{}:{}: data row before the first '#tree' marker".format(path, lineno)
                        )
                    count_line_index = lineno - 1
                    continue
                raise ConverterError(
                    "{}:{}: data row before the first '#tree' marker".format(path, lineno)
                )
            ntokens = len(line.split())
            if ntokens != expected_tokens:
                raise ConverterError(
                    "{}:{}: malformed data row: {} token(s), header declares {} columns".format(
                        path, lineno, ntokens, expected_tokens
                    )
                )
            n_rows += 1
    if header_line is None:
        raise ConverterError("{}: empty file (no header line)".format(path))
    return PreScan(
        path=str(path),
        header_line=header_line,
        n_rows=n_rows,
        tree_start_rows=np.asarray(tree_start_rows, dtype=np.int64),
        tree_root_ids=np.asarray(tree_root_ids, dtype=np.int64),
        md5=md5.hexdigest(),
        size=stat_before.st_size,
        mtime_ns=stat_before.st_mtime_ns,
        declared_tree_count=declared_tree_count,
        count_line_index=count_line_index,
    )


@dataclass
class ParseResult:
    """Totals accumulated by CtreesFileParser.chunks(); valid once complete."""

    n_rows_parsed: int = 0
    #: observed (snapshot, scale) pairs across the whole file
    observed_pairs: Set[Tuple[int, float]] = field(default_factory=set)
    complete: bool = False


class CtreesFileParser:
    """Chunked parser for one ctrees ASCII file.

    Yields structured arrays in RECORD_DTYPE with ``tree_root_id`` attributed
    from ``#tree`` markers and ``forest_id`` set to -1 (joined by the scatter
    stage). The generator raises ConverterError unless the parsed row count
    equals the independent pre-count exactly.

    With a ``schema`` the columns are selected through its profile and the
    records are in the extended ``ScratchLayout.from_schema(schema).dtype``:
    the same 20 frozen fields, filled by exactly the same code, followed by
    the source coordinate (from ``unit_plan``, which must then be supplied
    before iterating) and every selected extra.
    """

    def __init__(
        self,
        path,
        chunksize: int = DEFAULT_CHUNKSIZE,
        prescan: Optional[PreScan] = None,
        schema=None,
        unit_plan: Optional[SourceUnitPlan] = None,
    ):
        self.path = Path(path)
        if chunksize < 1:
            raise ConverterError("chunksize must be >= 1, got {}".format(chunksize))
        self.chunksize = chunksize
        self.prescan = prescan if prescan is not None else prescan_file(self.path)
        names = parse_header_line(self.prescan.header_line)
        if schema is None:
            self.scratch_layout = LEGACY_LAYOUT
            self.layout = resolve_columns(names)
        else:
            self.scratch_layout = ScratchLayout.from_schema(schema)
            try:
                self.layout = resolve_selection(names, schema)
            except ConverterError as exc:
                raise ConverterError("{}: {}".format(self.path, exc)) from exc
        self.unit_plan = unit_plan
        self.result = ParseResult()

    @property
    def dtype(self) -> np.dtype:
        return self.scratch_layout.dtype

    def _read_csv_chunks(self):
        ncols = len(self.layout.all_names)
        pandas_names = ["c{}".format(i) for i in range(ncols)]
        used = {"c{}".format(idx): col for col, idx in self.layout.indices.items()}
        dtype_map = {}
        for pname, col in used.items():
            if self.layout.is_integer_role(col):
                dtype_map[pname] = np.int64
            else:
                dtype_map[pname] = np.float64
        # extra-only columns: text for an integer extra (never through float),
        # float64 otherwise; a column already read for a role keeps that parse
        for extra in self.layout.extras:
            for idx, _spelling in extra.columns:
                pname = "c{}".format(idx)
                if pname in used:
                    continue
                used[pname] = extra.name
                dtype_map[pname] = object if idx in self.layout.text_columns else np.float64
        skiprows = None
        if self.prescan.count_line_index is not None:
            skiprows = [self.prescan.count_line_index]
        return pd.read_csv(
            self.path,
            sep=r"\s+",
            comment="#",
            header=None,
            names=pandas_names,
            usecols=sorted(used, key=lambda n: int(n[1:])),
            dtype=dtype_map,
            chunksize=self.chunksize,
            skiprows=skiprows,
        )

    def _column(self, chunk: pd.DataFrame, name: str) -> np.ndarray:
        return chunk["c{}".format(self.layout.indices[name])].to_numpy()

    def chunks(self) -> Iterator[np.ndarray]:
        prescan = self.prescan
        if self.scratch_layout.is_extended:
            plan = self.unit_plan
            if plan is None:
                raise ConverterError(
                    "{}: an extended scratch layout needs a source-unit plan".format(self.path)
                )
            if plan.unit_of_tree.shape != prescan.tree_start_rows.shape:
                raise ConverterError(
                    "{}: source-unit plan covers {} tree(s), the pre-scan found {}".format(
                        self.path, plan.unit_of_tree.size, prescan.tree_start_rows.size
                    )
                )
        row_offset = 0
        try:
            reader = self._read_csv_chunks()
        except (pd.errors.ParserError, ValueError) as exc:
            raise ConverterError("{}: malformed ctrees data: {}".format(self.path, exc))
        with reader:
            while True:
                try:
                    chunk = reader.get_chunk()
                except StopIteration:
                    break
                except (pd.errors.ParserError, ValueError) as exc:
                    raise ConverterError("{}: malformed ctrees data: {}".format(self.path, exc))
                # structurally short rows are impossible here (the pre-scan
                # enforces exact per-row token arity); float NaN can only be a
                # literal nan token, which _assemble rejects as non-finite
                n = len(chunk)
                if row_offset + n > prescan.n_rows:
                    raise ConverterError(
                        "{}: parsed more rows ({}) than the independent pre-count ({})".format(
                            self.path, row_offset + n, prescan.n_rows
                        )
                    )
                records = self._assemble(chunk, row_offset)
                row_offset += n
                yield records
        if row_offset != prescan.n_rows:
            raise ConverterError(
                "{}: parsed row count {} != independent pre-count {}".format(
                    self.path, row_offset, prescan.n_rows
                )
            )
        self.result.n_rows_parsed = row_offset
        self.result.complete = True

    def _abort_non_finite(self, what: str, col: str, bad: np.ndarray, row_offset: int) -> None:
        ordinals = (np.nonzero(bad)[0][:5] + row_offset).tolist()
        raise ConverterError(
            "{}: {} value(s) in column '{}' at data-row ordinal(s) {} — "
            "aborting, never repairing".format(self.path, what, col, ordinals)
        )

    def _assemble(self, chunk: pd.DataFrame, row_offset: int) -> np.ndarray:
        n = len(chunk)
        records = np.zeros(n, dtype=self.scratch_layout.dtype)
        for rec_field, col in _RECORD_FROM_COLUMN.items():
            values = self._column(chunk, col)
            if values.dtype.kind == "f":
                bad = ~np.isfinite(values)
                if bad.any():
                    self._abort_non_finite("non-finite", col, bad, row_offset)
            # float64 parse -> float32 cast at record assembly (reference path);
            # the reference parser rejects non-finite values, and a finite
            # float64 overflowing float32 becomes inf — both abort here (the
            # overflow warning is suppressed because the inf check below is
            # the deliberate detector)
            with np.errstate(over="ignore"):
                cast = values.astype(RECORD_DTYPE[rec_field], copy=False)
            if cast.dtype.kind == "f":
                bad = ~np.isfinite(cast)
                if bad.any():
                    self._abort_non_finite("float32-overflowing", col, bad, row_offset)
            records[rec_field] = cast

        snap64 = self._column(chunk, self.layout.snapshot_key)
        if snap64.size and (
            snap64.min() < np.iinfo(np.int32).min or snap64.max() > np.iinfo(np.int32).max
        ):
            raise ConverterError(
                "{}: snapshot value outside int32 range (min {}, max {})".format(
                    self.path, snap64.min(), snap64.max()
                )
            )
        records["snap"] = snap64.astype(np.int32)

        # #tree attribution: interval lookup over pre-scan marker start rows.
        row_ordinals = np.arange(row_offset, row_offset + n, dtype=np.int64)
        marker_idx = np.searchsorted(self.prescan.tree_start_rows, row_ordinals, side="right") - 1
        records["tree_root_id"] = self.prescan.tree_root_ids[marker_idx]
        records["forest_id"] = -1

        scale64 = self._column(chunk, "scale")
        bad = ~np.isfinite(scale64)
        if bad.any():
            self._abort_non_finite("non-finite", "scale", bad, row_offset)
        pairs = np.unique(np.column_stack((snap64.astype(np.float64), scale64)), axis=0)
        self.result.observed_pairs.update((int(s), float(a)) for s, a in pairs)

        if self.scratch_layout.is_extended:
            plan = self.unit_plan
            records["src_file_ordinal"] = plan.source_file_ordinal
            records["src_unit_ordinal"] = plan.unit_of_tree[marker_idx]
            records["src_row_ordinal"] = plan.unit_row_base[marker_idx] + (
                row_ordinals - self.prescan.tree_start_rows[marker_idx]
            )
            for extra in self.layout.extras:
                for position, (idx, spelling) in enumerate(extra.columns):
                    values = self._extra_values(chunk, extra, idx, spelling, row_offset)
                    if len(extra.columns) == 1:
                        records[extra.field] = values
                    else:
                        records[extra.field][:, position] = values
        return records

    def _extra_values(
        self, chunk: pd.DataFrame, extra: ExtraColumns, idx: int, spelling: str, row_offset: int
    ) -> np.ndarray:
        """One extra component, converted under the module docstring's rules."""
        raw = chunk["c{}".format(idx)].to_numpy()
        label = "{} [extra {}, {}]".format(spelling, extra.name, extra.type)
        what = "column '{}'".format(label)
        element = np.dtype(_little_endian_element(extra.spec))
        if extra.spec.is_integer:
            if raw.dtype == object:
                values = self._parse_integer_text(raw, what, row_offset)
            else:
                # a required integer role column, already parsed exactly as int64
                values = raw.astype(np.int64, copy=False)
            if element.itemsize == 4:
                info = np.iinfo(np.int32)
                bad = (values < info.min) | (values > info.max)
                if bad.any():
                    rows = np.nonzero(bad)[0][:5]
                    raise ConverterError(
                        "{}: {} {} value(s) outside the int32 range at data-row ordinal(s) {} "
                        "(values {}) -- aborting, never narrowing".format(
                            self.path,
                            what,
                            int(bad.sum()),
                            (rows + row_offset).tolist(),
                            values[rows].tolist(),
                        )
                    )
            return values.astype(element)
        bad = ~np.isfinite(raw)
        if bad.any():
            self._abort_non_finite("non-finite", label, bad, row_offset)
        if element.itemsize == 8:
            return raw.astype(element)
        with np.errstate(over="ignore"):
            cast = raw.astype(element)
        bad = ~np.isfinite(cast)
        if bad.any():
            self._abort_non_finite("float32-overflowing", label, bad, row_offset)
        return cast

    def _parse_integer_text(self, raw: np.ndarray, what: str, row_offset: int) -> np.ndarray:
        """Exact int64 from integer-literal tokens; nothing passes through float.

        pandas hands an NA token (``nan``, ``NA``, ...) over as a float NaN and
        every other token as ``str``; only ``[+-]?[0-9]+`` is accepted, so a
        fraction, an exponent, an underscore-grouped literal and an NA token all
        fail here, before any conversion.
        """
        text = pd.Series(raw, dtype=object)
        literal = text.str.fullmatch(_INTEGER_LITERAL, na=False).to_numpy(dtype=bool)
        if not literal.all():
            rows = np.nonzero(~literal)[0][:5]
            raise ConverterError(
                "{}: {} {} token(s) that are not integer literals at data-row ordinal(s) {} "
                "(tokens {}) -- a fraction, exponent or NA token is never read as an "
                "integer".format(
                    self.path,
                    what,
                    int((~literal).sum()),
                    (rows + row_offset).tolist(),
                    [str(raw[r]) for r in rows],
                )
            )
        try:
            return raw.astype(np.int64)
        except OverflowError:
            limits = np.iinfo(np.int64)
            rows = [r for r in range(raw.size) if not limits.min <= int(raw[r]) <= limits.max][:5]
            raise ConverterError(
                "{}: {} integer token(s) overflowing int64 at data-row ordinal(s) {} "
                "(tokens {})".format(
                    self.path, what, [r + row_offset for r in rows], [str(raw[r]) for r in rows]
                )
            ) from None


def parse_file(path, chunksize: int = DEFAULT_CHUNKSIZE) -> Tuple[np.ndarray, ParseResult, PreScan]:
    """Parse a whole file into one array (tests / small-file convenience)."""
    parser = CtreesFileParser(path, chunksize=chunksize)
    parts = list(parser.chunks())
    if parts:
        records = np.concatenate(parts)
    else:
        records = np.zeros(0, dtype=RECORD_DTYPE)
    return records, parser.result, parser.prescan
