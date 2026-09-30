"""Canonical record/topology contracts shared by every source adapter.

Pure contract module: it defines the inventory, the source-coordinate
identity, the deterministic ``SourceHaloID`` assignment and the canonical
batch an adapter must emit, validates one, and holds the scalar and budget
checks every adapter applies. It opens no source file and implements no
adapter; ``lhalo_binary``, ``ctrees_hdf5`` and ``ctrees_ascii`` subclass
:class:`SourceAdapter`.

**Links are carried as target ``SourceHaloID``, not as output row indices.**
An adapter cannot know a target's snapshot-local row: that mapping is built by
the bounded external sort/merge in ``transpose.py``. Emitting the
target's ``SourceHaloID`` keeps each batch flat and bounded (one int64 per
link), keeps the whole relationship spoolable, and keeps the source coordinate
recoverable -- the inventory's prefix sums invert an id back to
``(source_file_ordinal, unit_ordinal, row_ordinal)`` exactly.

**Adapter-author contract.**

- Derive source-overwrite protection from the adapter's *complete* real
  read/write set -- every path the run actually touches -- as one principle,
  never from the route under review.
- A failed *read* may be reported as data; a failed *write* must propagate.
  Swallowing a write failure behind a bare-``Exception`` boundary can leave a
  partial artifact that looks complete.
- **Check each selected extra's source component against the source field's
  real shape *and type*, at read time.** A profile's ``{field}`` means a stored
  scalar and ``{field, component}`` means one element of a stored vector, and
  the two are not interchangeable; likewise an extra declared ``float`` may not
  be filled from an int32 source field, and a required role may not be filled
  from a field of the wrong type or arity.
  ``column_schema.build_schema`` enforces all of this at freeze time for
  ``lhalo_binary``, because a fixed-record profile declares its shapes and
  types; it *cannot* for ``consistent_trees_ascii`` or
  ``consistent_trees_hdf5``. An ASCII column has no declared type until it is
  parsed, and a forests-HDF5 dataset's dtype is a property of the file rather
  than of any profile, so **those two adapters own both checks at read time**
  and fail rather than guess which of three values a component-less vector
  reference meant, or silently cast an integer column into a float output.
- **Reject two required roles that resolve to the same source field.**
  ``Len: [SnapNum]`` passes every per-role check -- both are integers -- and
  would emit snapshot numbers as particle counts. ``build_schema`` rejects it
  at freeze time for ``lhalo_binary``; the other two adapters own the same
  check at read time, once their columns are resolved. Role-to-*extra* reuse
  is a different thing and stays legal (the shipped ASCII example depends
  on it), so only role-to-role collisions are
  errors.
- **Byte-swap into native order before building a batch.**
  ``CanonicalBatch``'s dtype checks compare against native-endian dtypes, while
  ``SourceLayout.numpy_dtype_spec()`` deliberately carries the *source's*
  declared byte order. Reading a big-endian source therefore yields
  big-endian arrays that ``validate()`` rejects on dtype. That fails safe --
  a mismatched dtype is refused, never silently misread -- but it is a step a
  big-endian adapter has to take, not a bug to report.
- Type-check every integral scalar with :func:`require_integer` rather than
  coercing it, and refuse every budgeted allocation with
  :func:`check_budget` *before* making it.
"""

import abc
from dataclasses import dataclass
from typing import Dict, Iterator, Mapping, Optional, Sequence, Tuple

import numpy as np
from column_schema import (
    EXTRA_TYPES,
    IDENTITY_FIELDS,
    INT64_MAX,
    SOURCE_FORMATS,
    TOPOLOGY_FIELDS,
    CanonicalSchema,
    ConverterError,
)

__all__ = [
    "ConverterError",
    "NULL_LINK",
    "INT64_MAX",
    "LINK_FIELDS",
    "SNAPSHOT_LINK_FIELDS",
    "require_integer",
    "check_budget",
    "SourceCoordinate",
    "SourceUnit",
    "SourceInventory",
    "CanonicalBatch",
    "identity_columns",
    "SourceAdapter",
]

#: The null sentinel for every link, in the canonical records and in v3 alike.
#: Only -1 is null; any other negative value is structurally invalid, matching
#: the C reader's own ``CT_ASSIGN_LINK`` bounds.
NULL_LINK = -1

#: The five stored topology links, in the fixed v3 order.
LINK_FIELDS: Tuple[str, ...] = tuple(
    field.name for field in TOPOLOGY_FIELDS if field.type == "long long"
)

#: The three target-snapshot columns. ``transpose.py`` *derives* them from a
#: resolved target's SnapNum; an adapter never carries them. They are named
#: here so the contract lives in one place.
SNAPSHOT_LINK_FIELDS: Tuple[str, ...] = tuple(
    field.name for field in TOPOLOGY_FIELDS if field.type == "int"
)

_IDENTITY_FIELD_NAMES = tuple(field.name for field in IDENTITY_FIELDS)

#: The FoF central link is never null: a central self-references.
_NEVER_NULL_LINKS = ("FirstHaloInFOFgroup",)


# ==========================================================================
# Scalar and budget checks
# ==========================================================================


def require_integer(value, what: str, because: str, minimum: Optional[int] = None) -> int:
    """Type-check an integral scalar instead of coercing it.

    ``int()`` would quietly truncate ``1.5`` and quietly accept ``True`` as 1,
    and every scalar an adapter or the pipeline takes is one a later stage
    depends on -- a recorded identity, a memory ceiling, a snapshot bound.
    ``bool`` is excluded explicitly because it is a subclass of ``int``.
    ``numpy`` integers are accepted: a caller computing a value from array
    data should not have to convert it back first.

    Args:
        value: The scalar to check.
        what: Names the scalar in the error.
        because: Why coercion is refused, appended to the type error.
        minimum: When given, the smallest accepted value.

    Returns:
        The value as a Python ``int``.

    Raises:
        ConverterError: ``value`` is not an integer, or is below ``minimum``.
    """
    if isinstance(value, bool) or not isinstance(value, (int, np.integer)):
        raise ConverterError("{} must be an integer, got {!r}; {}".format(what, value, because))
    value = int(value)
    if minimum is not None and value < minimum:
        raise ConverterError("{} must be at least {}, got {}".format(what, minimum, value))
    return value


def check_budget(required_bytes: int, memory_budget_bytes: int, what: str, remedy: str) -> None:
    """Refuse an over-budget allocation *before* making it.

    Each budgeted term is compared against the whole ``memory_budget_bytes``
    ceiling on its own, not as a running total: the ceiling bounds working
    buffers term by term, never interpreter RSS. ``what`` names the term and
    how its figure was built; ``remedy`` tells the operator what to change.

    Raises:
        ConverterError: ``required_bytes`` exceeds ``memory_budget_bytes``.
    """
    if required_bytes > memory_budget_bytes:
        raise ConverterError(
            "{} needs {} bytes, above the configured memory budget of {} bytes; {}".format(
                what, required_bytes, memory_budget_bytes, remedy
            )
        )


# ==========================================================================
# Source coordinates and inventory
# ==========================================================================


@dataclass(frozen=True, order=True)
class SourceCoordinate:
    """A canonical halo's source identity ``(file, unit, row)``.

    A *unit* is the source's own grouping object: an L-Halo tree, a
    forests-HDF5 ``ForestInfo`` row, or an ASCII forest. ``row_ordinal`` is the
    original within-unit row index, never a re-sorted one.
    """

    source_file_ordinal: int
    unit_ordinal: int
    row_ordinal: int


@dataclass(frozen=True)
class SourceUnit:
    """One inventory unit and its halo count."""

    source_file_ordinal: int
    unit_ordinal: int
    n_halos: int


class SourceInventory:
    """The adapter's deterministic total order over the whole source.

    ``SourceHaloID`` is a prefix sum of halo counts in this order, starting at
    1 so every id is positive. Two properties matter and are enforced
    here:

    - **The order is total and explicit.** Units are held in ascending
      ``(source_file_ordinal, unit_ordinal)``; duplicates are rejected.
    - **Sampling never compacts identity.** ``selected`` narrows which units a
      run converts, but the prefix sums are always computed over *every* unit
      of the parent inventory, so a sampled run's ids match the unsampled
      run's exactly. Comparing compacted sample identities against an
      unsampled reference is precisely the mistake this prevents.

    Identity is relative to the selected source representation: an L-Halo
    packaging and a forests-HDF5 packaging of the same simulation can
    enumerate different forest sets, so equal ``SourceHaloID`` or
    ``UniqueGalaxyID`` values across formats are never promised.
    """

    def __init__(
        self,
        units: Sequence[SourceUnit],
        selected: Optional[Sequence[Tuple[int, int]]] = None,
    ):
        ordered = tuple(units)
        seen = set()
        previous: Optional[Tuple[int, int]] = None
        total = 0
        bases = []
        for unit in ordered:
            key = (unit.source_file_ordinal, unit.unit_ordinal)
            if unit.source_file_ordinal < 0 or unit.unit_ordinal < 0:
                raise ConverterError("inventory ordinals must be non-negative: {!r}".format(unit))
            if unit.n_halos < 0:
                raise ConverterError("inventory unit {!r} has a negative halo count".format(unit))
            if key in seen:
                raise ConverterError("inventory lists unit {} twice".format(key))
            if previous is not None and key <= previous:
                raise ConverterError(
                    "inventory is not in ascending (file, unit) order: {} follows {}".format(
                        key, previous
                    )
                )
            seen.add(key)
            previous = key
            bases.append(total + 1)
            # Checked before the addition, so the abort happens instead of the
            # overflow, not after it ("Abort on int64 overflow").
            if total > INT64_MAX - unit.n_halos:
                raise ConverterError(
                    "SourceHaloID assignment overflows int64 at unit {} ({} halos already "
                    "counted)".format(key, total)
                )
            total += unit.n_halos

        self.units: Tuple[SourceUnit, ...] = ordered
        self.total_halos: int = total
        self._bases: Tuple[int, ...] = tuple(bases)
        self._index: Dict[Tuple[int, int], int] = {
            (unit.source_file_ordinal, unit.unit_ordinal): position
            for position, unit in enumerate(ordered)
        }
        if selected is None:
            self.selected: Tuple[Tuple[int, int], ...] = tuple(sorted(self._index))
        else:
            chosen = tuple(sorted(set(tuple(key) for key in selected)))
            unknown = [key for key in chosen if key not in self._index]
            if unknown:
                raise ConverterError("selected units {} are not in the inventory".format(unknown))
            self.selected = chosen

    def base_id(self, source_file_ordinal: int, unit_ordinal: int) -> int:
        """The ``SourceHaloID`` of row 0 of one unit."""
        key = (source_file_ordinal, unit_ordinal)
        try:
            return self._bases[self._index[key]]
        except KeyError:
            raise ConverterError("unit {} is not in the inventory".format(key)) from None

    def coordinate(self, source_halo_id: int) -> SourceCoordinate:
        """Invert a ``SourceHaloID`` back to its source coordinate.

        Exact by construction: ids are a contiguous prefix sum, so the search
        is a bisection over unit bases with no stored id table.
        """
        if not 1 <= source_halo_id <= self.total_halos:
            raise ConverterError(
                "SourceHaloID {} is outside [1, {}]".format(source_halo_id, self.total_halos)
            )
        low, high = 0, len(self._bases) - 1
        while low < high:
            middle = (low + high + 1) // 2
            if self._bases[middle] <= source_halo_id:
                low = middle
            else:
                high = middle - 1
        unit = self.units[low]
        return SourceCoordinate(
            source_file_ordinal=unit.source_file_ordinal,
            unit_ordinal=unit.unit_ordinal,
            row_ordinal=source_halo_id - self._bases[low],
        )


# ==========================================================================
# Canonical batch
# ==========================================================================


def _expect_array(values, name: str, dtype: str, n_rows: int, n_components: int) -> np.ndarray:
    array = np.asarray(values)
    if array.dtype != np.dtype(dtype):
        raise ConverterError(
            "{}: expected dtype {}, got {}".format(name, np.dtype(dtype), array.dtype)
        )
    expected_shape = (n_rows,) if n_components == 1 else (n_rows, n_components)
    if array.shape != expected_shape:
        raise ConverterError(
            "{}: expected shape {}, got {}".format(name, expected_shape, array.shape)
        )
    # Float parsing/casts reject non-finite input and overflow and preserve
    # signed zero. An adapter should ideally catch a non-finite value at its
    # own cast boundary, where it can name the source row -- but this is the
    # one place every batch passes through whatever produced it, so it is the
    # contract's last guard. Signed zero is finite and passes untouched: the
    # rule is to preserve it, not to reject it.
    if array.dtype.kind == "f" and array.size and not bool(np.all(np.isfinite(array))):
        bad = np.asarray(~np.isfinite(array)).nonzero()
        first = tuple(int(axis[0]) for axis in bad)
        raise ConverterError(
            "{}: non-finite value {} at index {}; NaN and infinity are not valid payload".format(
                name, array[first], first
            )
        )
    return array


@dataclass(frozen=True)
class CanonicalBatch:
    """One bounded chunk of canonical halo records.

    Column names follow the v3 field table: ``identity`` carries
    ``SourceHaloID``/``ForestIndex``/``HaloRankInForest``, ``links`` the five
    topology links as target ``SourceHaloID`` (``-1`` null), ``payload`` the
    format's declared physical/catalog fields, and ``extras`` the
    declaratively selected ones. ``coordinates`` carries the three source
    ordinals per row, so a comparison never has to reconstruct them.

    Batch size is the adapter's business; the contract only requires that a
    batch fits the configured bound, so a forest larger than one chunk is
    emitted across several batches rather than materialised whole.

    ``frozen=True`` here prevents *rebinding the five fields*, nothing more.
    The mappings and the arrays they hold stay mutable, deliberately -- an
    adapter fills them, and this module's own tests construct invalid cases by
    editing a valid batch. Do not read the freeze as a guarantee that a batch's
    contents cannot change after ``validate()`` has passed.
    """

    schema: CanonicalSchema
    identity: Mapping[str, np.ndarray]
    coordinates: Mapping[str, np.ndarray]
    links: Mapping[str, np.ndarray]
    payload: Mapping[str, np.ndarray]
    extras: Mapping[str, np.ndarray]

    @property
    def n_rows(self) -> int:
        # Coerced, because this runs before _expect_array has seen anything: an
        # adapter handing a plain list would otherwise crash with a raw
        # AttributeError instead of this module's named error. Reachable only
        # by an adapter bug, never by user input, but the contract should
        # report it the same way it reports everything else.
        return int(np.asarray(self.identity["SourceHaloID"]).shape[0])

    def validate(self) -> None:
        """Structural validation of one batch against its schema.

        Deliberately *not* checked: ``MostBoundID`` uniqueness, positivity or
        ordering. It is the source catalog's own signed particle/halo
        identifier, legitimately duplicated and legitimately negative in the
        general format, and treating it as a key is the v2 assumption v3
        exists to drop. ``SourceHaloID`` is the unique key, and it is
        checked here.
        """
        if "SourceHaloID" not in self.identity:
            raise ConverterError("batch is missing the SourceHaloID identity column")
        n_rows = self.n_rows

        self._validate_identity(n_rows)
        self._validate_coordinates(n_rows)
        self._validate_links(n_rows)
        self._validate_payload(n_rows)
        self._validate_extras(n_rows)

    # ---- helpers -------------------------------------------------------

    def _validate_identity(self, n_rows: int) -> None:
        missing = [name for name in _IDENTITY_FIELD_NAMES if name not in self.identity]
        if missing:
            raise ConverterError("batch is missing identity column(s) {}".format(missing))
        unknown = sorted(set(self.identity) - set(_IDENTITY_FIELD_NAMES))
        if unknown:
            raise ConverterError("batch declares unknown identity column(s) {}".format(unknown))
        # The coerced arrays are kept and used below, rather than re-reading
        # self.identity: _expect_array is what turns whatever an adapter handed
        # over into an ndarray, and reaching past it would reintroduce the raw
        # AttributeError it exists to prevent.
        checked = {
            name: _expect_array(
                self.identity[name], "identity {!r}".format(name), "int64", n_rows, 1
            )
            for name in _IDENTITY_FIELD_NAMES
        }

        ids = checked["SourceHaloID"]
        if n_rows and int(ids.min()) <= 0:
            raise ConverterError("SourceHaloID must be positive; found {}".format(int(ids.min())))
        if n_rows > 1 and not bool(np.all(np.diff(ids) > 0)):
            raise ConverterError(
                "SourceHaloID must be strictly increasing within a batch (the adapter's declared "
                "source order); found a duplicate or an out-of-order row"
            )
        for name in ("ForestIndex", "HaloRankInForest"):
            values = checked[name]
            if n_rows and int(values.min()) < 0:
                raise ConverterError("{} must be non-negative".format(name))

    def _validate_coordinates(self, n_rows: int) -> None:
        expected = ("source_file_ordinal", "unit_ordinal", "row_ordinal")
        missing = [name for name in expected if name not in self.coordinates]
        if missing:
            raise ConverterError("batch is missing source coordinate(s) {}".format(missing))
        unknown = sorted(set(self.coordinates) - set(expected))
        if unknown:
            raise ConverterError("batch declares unknown source coordinate(s) {}".format(unknown))
        for name in expected:
            values = _expect_array(
                self.coordinates[name], "coordinate {!r}".format(name), "int64", n_rows, 1
            )
            if n_rows and int(values.min()) < 0:
                raise ConverterError("coordinate {!r} must be non-negative".format(name))

    def _validate_links(self, n_rows: int) -> None:
        missing = [name for name in LINK_FIELDS if name not in self.links]
        if missing:
            raise ConverterError("batch is missing link column(s) {}".format(missing))
        unknown = sorted(set(self.links) - set(LINK_FIELDS))
        if unknown:
            raise ConverterError("batch declares unknown link column(s) {}".format(unknown))
        for name in LINK_FIELDS:
            values = _expect_array(self.links[name], "link {!r}".format(name), "int64", n_rows, 1)
            if not n_rows:
                continue
            if int(values.min()) < NULL_LINK:
                raise ConverterError(
                    "link {!r} has a value below the -1 null sentinel; only -1 is null".format(name)
                )
            if name in _NEVER_NULL_LINKS and bool(np.any(values == NULL_LINK)):
                raise ConverterError(
                    "link {!r} is never null: a central self-references".format(name)
                )
            if bool(np.any(values == 0)):
                raise ConverterError(
                    "link {!r} targets SourceHaloID 0, which is never assigned (ids start "
                    "at 1)".format(name)
                )

    def _validate_payload(self, n_rows: int) -> None:
        declared = {field.name: field for field in self.schema.payload_fields}
        missing = sorted(set(declared) - set(self.payload))
        if missing:
            raise ConverterError("batch is missing payload column(s) {}".format(missing))
        unknown = sorted(set(self.payload) - set(declared))
        if unknown:
            raise ConverterError("batch declares unknown payload column(s) {}".format(unknown))
        for name, field in declared.items():
            spec = EXTRA_TYPES[field.type]
            values = _expect_array(
                self.payload[name],
                "payload {!r}".format(name),
                spec.numpy_dtype,
                n_rows,
                spec.n_components,
            )
            if name == "Len" and n_rows and int(values.min()) < 0:
                raise ConverterError("Len must never be negative")
            if name == "SnapNum" and n_rows and int(values.min()) < 0:
                raise ConverterError("SnapNum must be non-negative")

    def _validate_extras(self, n_rows: int) -> None:
        declared = {extra.name: extra for extra in self.schema.extra_fields}
        missing = sorted(set(declared) - set(self.extras))
        if missing:
            raise ConverterError("batch is missing selected extra column(s) {}".format(missing))
        unknown = sorted(set(self.extras) - set(declared))
        if unknown:
            raise ConverterError("batch declares unselected extra column(s) {}".format(unknown))
        for name, extra in declared.items():
            spec = extra.spec
            _expect_array(
                self.extras[name],
                "extra {!r}".format(name),
                spec.numpy_dtype,
                n_rows,
                spec.n_components,
            )


def identity_columns(
    base_id: int,
    forest_index: int,
    source_file_ordinal: int,
    unit_ordinal: int,
    start: int,
    n_rows: int,
) -> Tuple[Dict[str, np.ndarray], Dict[str, np.ndarray]]:
    """The ``identity`` and ``coordinates`` groups for one contiguous row range
    of one unit.

    Covers rows ``[start, start + n_rows)`` of the unit whose row 0 has
    ``SourceHaloID`` ``base_id`` (``SourceInventory.base_id``). Each row's
    ``SourceHaloID`` is ``base_id + row``, ``HaloRankInForest`` and
    ``row_ordinal`` are the original within-unit row, and ``ForestIndex`` and
    the file/unit ordinals are constant. Every column is int64, one element
    per row.

    Returns:
        ``(identity, coordinates)``, keyed as :class:`CanonicalBatch` expects.
    """
    rows = np.arange(start, start + n_rows, dtype=np.int64)
    identity = {
        "SourceHaloID": base_id + rows,
        "ForestIndex": np.full(n_rows, forest_index, dtype=np.int64),
        "HaloRankInForest": rows.copy(),
    }
    coordinates = {
        "source_file_ordinal": np.full(n_rows, source_file_ordinal, dtype=np.int64),
        "unit_ordinal": np.full(n_rows, unit_ordinal, dtype=np.int64),
        "row_ordinal": rows.copy(),
    }
    return identity, coordinates


# ==========================================================================
# Adapter contract
# ==========================================================================


class SourceAdapter(abc.ABC):
    """What every source adapter owes the generic converter.

    An adapter owns its format's scientific semantics and nothing else:
    it reads its own on-disk representation, preserves that representation's
    values, units, chain order and identity conventions, and emits canonical
    batches. It never applies another format's conventions, never repairs a
    source, never inserts a phantom halo and never sorts the source to derive
    identity.
    """

    #: The ``source_format`` key this adapter answers to; set by a subclass.
    source_format: str = ""

    def __new__(cls, *args, **kwargs):
        """Reject an unknown ``source_format`` when an adapter is instantiated.

        Checked here rather than at class definition: at ``__init_subclass__``
        time ABCMeta has not yet computed ``__abstractmethods__``, so an
        abstract *intermediate* base -- a shared binary base that a concrete
        adapter subclasses again -- is indistinguishable from a concrete leaf
        and would be forced to invent a format key it does not answer to.
        ``object.__new__`` runs first so an incomplete adapter still reports
        the standard abstract-method ``TypeError``, which is the more useful
        error for a class nobody could instantiate anyway.
        """
        instance = super().__new__(cls)
        if cls.source_format not in SOURCE_FORMATS:
            raise ConverterError(
                "adapter {} declares source_format {!r}, which is not one of {}".format(
                    cls.__name__, cls.source_format, sorted(SOURCE_FORMATS)
                )
            )
        return instance

    @abc.abstractmethod
    def inventory(self) -> SourceInventory:
        """The complete, deterministically ordered source inventory.

        Must cover every unit of the selected file range, and must fail rather
        than narrow silently when a requested file is missing.
        """

    @abc.abstractmethod
    def iter_batches(self, max_rows: int) -> Iterator[CanonicalBatch]:
        """Stream canonical batches of at most ``max_rows`` rows.

        Bounded by contract: a forest or tree larger than ``max_rows`` is
        emitted across several batches, never materialised as one whole-forest
        object graph.
        """
