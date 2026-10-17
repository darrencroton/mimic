"""Producer validation battery for horizontal-HDF5 format version 3.

Implements the producer side of Validation Requirements in
convert/mimic-convert/HORIZONTAL-HDF5-FORMAT.md (section "Version 3") over one dataset directory:
:func:`run_battery_v3` settles every check named in :data:`V3_CHECKS` and
returns the outcomes together with the residency and spill high-water marks
it measured. A file that does not declare ``format_version`` 3 is refused by
the header checks. The battery is reached through ``convert_trees.py
validate|report``.

**One battery, two modes.** This module is the external-sort mode, the
default, which validates every version 3 dataset, gapped ``Descendant`` links
included. The bounded mode (:func:`validate_bounded.run_battery_bounded`,
``validate|report --bounded``) validates adjacent datasets (``links_adjacent``
1) without any sort or spill, within a ceiling it prints at start, and is the
practical validator at the largest scales. Both modes settle the same checks,
names and verdicts; counts, examples and wording can differ. They share this
module's public interface: the outcome types, :func:`settle_front` (every
check settled before a halo row is read), :class:`RowScan` (the per-row block
check every scanner of either mode constructs and extends),
:func:`settle_scan` (every check recorded from a finished scan), the
structure, header, binding, sidecar and conservation checks, and the failure
kinds both modes word alike.

**Independence.** The fixed format tables below are restated literally from
the specification rather than imported from the writer or the schema module,
so a writer-side table drift is caught instead of being mirrored. The
per-dataset payload types come from each file's own ``/schema`` and are then
bound to the conversion manifest's embedded schema.

**Boundedness.** No check holds a whole snapshot. One scan reads every file
in row blocks, checks every per-row rule there, and feeds four bounded
external sorts (``rank_sort.KeyedSorter``) whose merges settle everything
that crosses rows, snapshots or files:

- a *topology join* keyed by each link's target global position: every halo
  contributes its own record and one request per chain link, so each target
  is judged against every link into it -- progenitor round-trip closure,
  descendant-relative NextProgenitor membership, FoF central identity,
  forest membership, and coverage (every halo with a descendant is reached
  exactly once, every non-central exactly once by its FoF chain);
- a *pointer-jumping* proof that no NextProgenitor or NextHaloInFOFgroup
  chain cycles (``source_keys.verify_chains_acyclic``, a generic primitive
  over ``(chain, node, successor)`` records);
- an *identity* ordering of ``(ForestIndex, HaloRankInForest, SourceHaloID)``;
- a *source-key* ordering of ``SourceHaloID``.

``budget_bytes`` bounds the scan block and every sorter buffer, all reported
to one shared meter whose high-water mark is returned as a measurement, as
is the spill high-water mark. What the meter does not see: O(snapshots)
header arrays, per-block numpy temporaries of the scan (bounded by the block
row count), and interpreter churn.
"""

import math
import os
import shutil
import sys
import tempfile
from pathlib import Path
from typing import Dict, List, Mapping, Optional, Tuple

import h5py
import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from conversion_manifest import ConversionManifest, IdentitySchemeError, sha256_file  # noqa: E402
from ctrees_parser import ConverterError  # noqa: E402
from hdf5_writer_v3 import (  # noqa: E402
    CHUNK_1D,
    HEADER_ATTRS,
    V3_FORMAT_VERSION,
    snapshot_h5_name,
)
from rank_sort import KeyedSorter, RankSortError, ResidencyMeter, SpillLedger  # noqa: E402
from scatter import load_a_list  # noqa: E402
from source_keys import (  # noqa: E402
    CHAIN_DTYPE,
    CHAIN_KEY,
    SnapshotLayout,
    verify_chains_acyclic,
)

#: Default UniqueGalaxyID multiplier (TREE_MUL_FAC, src/include/constants.h).
DEFAULT_MULTIPLIER = 10**9

#: Default working-buffer budget of one v3 battery.
DEFAULT_V3_BUDGET_BYTES = 256 << 20

_INT64_MAX = np.iinfo(np.int64).max

#: Header attributes that are run-scoped or physical and must be identical in
#: every file of the dataset.
RUN_SCOPED_ATTRS = (
    "n_forests_total",
    "max_halo_rank_in_forest",
    "box_size_mpc_h",
    "particle_mass_msun_h",
    "omega_matter",
    "omega_lambda",
    "hubble_h",
)


class Outcome:
    """One named battery check outcome."""

    def __init__(self, name: str, status: str, detail: str = ""):
        self.name = name
        self.status = status  # PASS | FAIL | SKIP
        self.detail = detail

    def as_dict(self) -> dict:
        return {"name": self.name, "status": self.status, "detail": self.detail}

    def line(self) -> str:
        text = "{}: {}".format(self.name, self.status)
        if self.detail:
            text += " — {}".format(self.detail)
        return text


def _examples(values, limit: int = 5) -> str:
    return ", ".join(str(v) for v in list(values)[:limit])


def _filter_failures(dataset, name: str) -> List[str]:
    """The storage contract is chunked and UNFILTERED: compression of any
    kind, scale-offset, shuffle, and fletcher32 are all prohibited."""
    failures = []
    if dataset.compression is not None:
        failures.append(
            "{} is compressed ({}); the contract forbids compression".format(
                name, dataset.compression
            )
        )
    if dataset.scaleoffset is not None:
        failures.append(
            "{} uses the scale-offset filter; the contract forbids filters".format(name)
        )
    if dataset.shuffle:
        failures.append("{} uses the shuffle filter; the contract forbids filters".format(name))
    if dataset.fletcher32:
        failures.append("{} uses the fletcher32 filter; the contract forbids filters".format(name))
    return failures


def check_header_bounds(n_forests_total: int, max_rank: int, multiplier: int) -> List[str]:
    """Galaxy-identity bound checks from the spec: the multiplier must exceed
    the dataset's max rank, and multiplier x (n_forests_total + 1) must fit in
    int64 (the startup checks the reader performs against these headers)."""
    failures = []
    if multiplier <= max_rank:
        failures.append(
            "identity multiplier {} does not exceed max_halo_rank_in_forest {}".format(
                multiplier, max_rank
            )
        )
    if multiplier * (n_forests_total + 1) > _INT64_MAX:
        failures.append(
            "multiplier {} x (n_forests_total {} + 1) overflows int64".format(
                multiplier, n_forests_total
            )
        )
    return failures


#: The fixed /halos table (topology, then identity), restated from the spec.
_V3_FIXED_DATASETS = (
    ("Descendant", "<i8"),
    ("FirstProgenitor", "<i8"),
    ("NextProgenitor", "<i8"),
    ("FirstHaloInFOFgroup", "<i8"),
    ("NextHaloInFOFgroup", "<i8"),
    ("DescendantSnapshot", "<i4"),
    ("FirstProgenitorSnapshot", "<i4"),
    ("NextProgenitorSnapshot", "<i4"),
    ("SourceHaloID", "<i8"),
    ("ForestIndex", "<i8"),
    ("HaloRankInForest", "<i8"),
)

#: The declarable ``/schema`` types and the storage each fixes: dtype, vec3.
_V3_TYPES = {
    "int": ("<i4", False),
    "long long": ("<i8", False),
    "float": ("<f4", False),
    "double": ("<f8", False),
    "vec3_int": ("<i4", True),
    "vec3_float": ("<f4", True),
}

#: Payload fields every v3 file declares, and the three whose type the format
#: table pins rather than the producer.
_V3_CORE_PAYLOAD = ("Len", "SnapNum", "M_Crit200", "Pos", "Vel", "Spin", "VelDisp", "Vmax")
_V3_PINNED_TYPES = {"SnapNum": "int", "Len": "int", "MostBoundID": "long long"}

_V3_STRING_ATTRS = {"source_format": 32, "column_mapping_sha256": 64}
_V3_SCHEMA_KEYS = ("type", "units", "h_convention", "description")
_V3_H_CONVENTIONS = ("carried", "free", "none")
_V3_SOURCE_FORMATS = ("consistent_trees_ascii", "consistent_trees_hdf5", "lhalo_binary")
_V3_SIDECAR = ("ForestID", "SourceFileOrdinal", "SourceUnitOrdinal")

#: Formats whose SourceHaloID is the halo's 1-based position in (ForestIndex,
#: HaloRankInForest) order -- every route: an L-Halo tree, a forests-HDF5
#: ForestInfo row and, under the forest-rank identity scheme, a whole
#: Consistent-Trees ASCII forest is one inventory unit, so a forest's halos are
#: one contiguous SourceHaloID range in rank order.
POSITION_IDENTITY_FORMATS = ("consistent_trees_ascii", "consistent_trees_hdf5", "lhalo_binary")

#: Formats whose inventory records per-file unit counts (``files``), so the
#: sidecar's (SourceFileOrdinal, SourceUnitOrdinal) pairs are checked against
#: them. The ASCII inventory has no per-file summary (one unit per forest).
_V3_PER_FILE_INVENTORY = ("lhalo_binary", "consistent_trees_hdf5")

#: Each snapshot-qualified link and its target-snapshot column.
_V3_QUALIFIED = (
    ("Descendant", "DescendantSnapshot"),
    ("FirstProgenitor", "FirstProgenitorSnapshot"),
    ("NextProgenitor", "NextProgenitorSnapshot"),
)

#: Every named v3 check, in report order.
V3_CHECKS = (
    "file-set",
    "object-set",
    "sidecar-object-set",
    "schema-binding",
    "manifest-binding",
    "header-values",
    "run-scoped-headers",
    "row-values",
    "len-nonnegative",
    "field-finiteness",
    "position-bounds",
    "link-targets",
    "links-adjacent",
    "topology-closure",
    "chain-cycles",
    "source-key-coverage",
    "identity",
    "header-bounds",
    "sidecar-content",
    "count-conservation",
)

#: Smallest budget accepted: every sorter share must hold its minimum buffers.
V3_MIN_BUDGET_BYTES = 1 << 20

#: Most rows one scan block reads, whatever the budget.
V3_MAX_SCAN_ROWS = 4 * CHUNK_1D[0]

#: Prefix of the battery's private spill directory.
V3_SPILL_PREFIX = "validate_v3_"

#: The topology join's record. ``tag`` 0 is a halo's own record: ``key`` and
#: ``other`` its global position, ``a`` its descendant's global position (or
#: -1), ``b`` its FoF central's. Tags 1-4 are links INTO ``key`` from the
#: halo at ``other``: 1 FirstProgenitor, 2 NextProgenitor, 3
#: FirstHaloInFOFgroup, 4 NextHaloInFOFgroup; ``a`` is the value the target's
#: own ``a`` (tags 1, 2) or ``b`` (tag 4) must equal. ``forest`` is always the
#: record owner's ForestIndex. ``(key, tag, other)`` is a total order.
_V3_JOIN_DTYPE = np.dtype(
    [
        ("key", "<i8"),
        ("tag", "<i8"),
        ("other", "<i8"),
        ("a", "<i8"),
        ("b", "<i8"),
        ("forest", "<i8"),
    ]
)
_V3_JOIN_KEY = ("key", "tag", "other")
_V3_IDENTITY_DTYPE = np.dtype([("forest", "<i8"), ("rank", "<i8"), ("sid", "<i8")])
_V3_IDENTITY_KEY = ("forest", "rank", "sid")
_V3_SID_DTYPE = np.dtype([("sid", "<i8")])
_V3_SID_KEY = ("sid",)

#: Scratch a merge consumer holds per record of the block it was handed: for
#: the join, the group index, gathered group fields, masks and counters; for
#: the two identity orderings, the shifted predecessor columns and masks.
_V3_JOIN_CONSUMER_BYTES = 12 * 8 + 8
_V3_ORDER_CONSUMER_BYTES = 6 * 8 + 4

#: Shares of the budget held concurrently while the scan feeds the sorters:
#: join, chain, identity and source-key generation buffers, then the scan
#: block itself. They sum to exactly the denominator, so the budget is
#: partitioned between them with nothing held back.
_V3_SHARES = {"join": 6, "chain": 4, "identity": 2, "sid": 1, "scan": 3}
_V3_SHARE_DENOMINATOR = 16

# ---------------------------------------------------------------------------
# Failure kinds both modes word alike
# ---------------------------------------------------------------------------

KIND_PROGENITOR_REACH = (
    "halo(s) not reached exactly once by their descendant's progenitor chain "
    "(reached k times, expected once iff the halo has a Descendant)"
)
KIND_FOF_REACH = (
    "halo(s) not reached exactly once by their FoF chain (reached k times, expected "
    "once iff the halo is not its group's central)"
)
KIND_FIRST_PROGENITOR = "FirstProgenitor(s) whose target's Descendant is not the owner"
KIND_NEXT_PROGENITOR = "NextProgenitor(s) whose target names a different descendant than the owner"
KIND_FOF_CENTRAL = "FirstHaloInFOFgroup target(s) that are not self-referencing centrals"
KIND_FOF_MEMBER = "NextHaloInFOFgroup target(s) in a different FoF group than the owner"
KIND_CROSSING_FORESTS = "link(s) crossing forests"
KIND_RANK_DENSITY = (
    "(ForestIndex, HaloRankInForest) pair(s) breaking per-forest rank density or uniqueness"
)
KIND_ASCII_FOREST_SKIPPED = "ForestIndex value(s) skipping a forest (ASCII forests all hold halos)"
KIND_POSITION_IDENTITY = (
    "halo(s) whose SourceHaloID is not its (ForestIndex, rank) position in the source order"
)
KIND_DUPLICATED_SOURCE_KEY = "duplicated SourceHaloID value(s)"
#: Formatted with the inventory's total_halos.
KIND_SOURCE_KEY_OUTSIDE = "SourceHaloID value(s) outside the inventory's [1, {}]"
KIND_SOURCE_KEY_GAP = "gap(s) in SourceHaloID coverage of the unsampled inventory"


class V3BatteryResult:
    """A v3 battery's named outcomes and what it measured on the way."""

    def __init__(self, outcomes: List[Outcome], measurements: Dict[str, object]):
        self.outcomes = outcomes
        self.measurements = measurements

    @property
    def failed(self) -> bool:
        return any(outcome.status == "FAIL" for outcome in self.outcomes)


class Verdicts:
    """The outcomes one battery run settles, in the order it settles them."""

    def __init__(self):
        self.outcomes: List[Outcome] = []

    def record(self, name: str, failures: List[str], detail: str = "") -> bool:
        """FAIL with the failures joined, or PASS with ``detail``; True on PASS."""
        if failures:
            self.outcomes.append(Outcome(name, "FAIL", "; ".join(failures)))
            return False
        self.outcomes.append(Outcome(name, "PASS", detail))
        return True

    def skip(self, names, reason: str) -> None:
        for name in names:
            self.outcomes.append(Outcome(name, "SKIP", reason))

    def result(self, measurements: Dict[str, object]) -> "V3BatteryResult":
        return V3BatteryResult(ordered_outcomes(self.outcomes), measurements)


class FailureTally:
    """Counted failures by kind, keeping five examples each."""

    def __init__(self):
        self._kinds: Dict[str, List] = {}

    def add(self, kind: str, count: int, examples) -> None:
        if count <= 0:
            return
        entry = self._kinds.setdefault(kind, [0, []])
        entry[0] += int(count)
        room = 5 - len(entry[1])
        if room > 0:
            entry[1].extend(list(examples)[:room])

    def __bool__(self) -> bool:
        return bool(self._kinds)

    def messages(self) -> List[str]:
        return [
            "{} {}; examples: {}".format(count, kind, _examples(examples))
            for kind, (count, examples) in self._kinds.items()
        ]


def _v3_rows(snap: int, rows: np.ndarray) -> List[str]:
    return ["{} row {}".format(snapshot_h5_name(snap), int(row)) for row in rows[:5]]


def pair_examples(forest: np.ndarray, rank: np.ndarray) -> List[str]:
    """Up to five ``(ForestIndex, rank)`` pairs."""
    return ["(ForestIndex={}, rank={})".format(f, r) for f, r in zip(forest[:5], rank[:5])]


def position_examples(forest, rank, sid, expected) -> List[str]:
    """Up to five halos whose SourceHaloID is not their identity position."""
    return [
        "(ForestIndex={}, rank={}) has SourceHaloID {}, expected {}".format(f, r, s, e)
        for f, r, s, e in zip(forest[:5], rank[:5], sid[:5], expected[:5])
    ]


def is_unsampled(inventory: Optional[Mapping]) -> bool:
    """The inventory selects every halo it counts."""
    return inventory is not None and int(inventory["selected_halos"]) == int(
        inventory["total_halos"]
    )


def position_identity_applies(source_format: str, inventory: Optional[Mapping]) -> bool:
    """SourceHaloID must equal the halo's 1-based (ForestIndex, rank)
    position: the format makes it one, and nothing was sampled away."""
    return source_format in POSITION_IDENTITY_FORMATS and is_unsampled(inventory)


def identity_summary(
    source_format: str,
    inventory: Optional[Mapping],
    last_forest: int,
    n_forests_total: int,
    measured_max: int,
    max_rank: int,
) -> List[str]:
    """The identity failures settled once every halo is seen: a sampled
    ASCII inventory, an ASCII enumeration whose last ForestIndex
    (``last_forest``, -1 for none) is not ``n_forests_total - 1``, and a
    measured maximum rank other than the header's."""
    messages = []
    ascii_route = source_format == "consistent_trees_ascii"
    if ascii_route and inventory is not None and not is_unsampled(inventory):
        messages.append(
            "consistent_trees_ascii inventory selects {} of {} halos; the ASCII route does "
            "not sample".format(inventory["selected_halos"], inventory["total_halos"])
        )
    if ascii_route and last_forest != n_forests_total - 1:
        messages.append(
            "ForestIndex values end at {} but n_forests_total is {}".format(
                last_forest, n_forests_total
            )
        )
    if measured_max != max_rank:
        messages.append(
            "measured max HaloRankInForest {} != header max_halo_rank_in_forest {}".format(
                measured_max, max_rank
            )
        )
    return messages


def emitted_count_failures(emitted: int, inventory: Optional[Mapping]) -> List[str]:
    """The SourceHaloID values emitted against the inventory's selection."""
    if inventory is None or emitted == int(inventory["selected_halos"]):
        return []
    return [
        "{} SourceHaloID value(s) emitted; the inventory selects {}".format(
            emitted, inventory["selected_halos"]
        )
    ]


def _share(budget_bytes: int, name: str) -> int:
    return budget_bytes * _V3_SHARES[name] // _V3_SHARE_DENOMINATOR


# ---------------------------------------------------------------------------
# Structure
# ---------------------------------------------------------------------------


def check_file_set(directory: Path, n_snapshots: int) -> List[str]:
    """Exactly the a_list's snapshot files plus forests.h5, as regular files;
    no other entry of any kind."""
    expected = {snapshot_h5_name(snap) for snap in range(n_snapshots)} | {"forests.h5"}
    present = {}
    for entry in directory.iterdir():
        present[entry.name] = entry
    failures = []
    missing = sorted(expected - set(present))
    extra = sorted(set(present) - expected)
    if missing:
        failures.append("{} missing file(s): {}".format(len(missing), _examples(missing)))
    if extra:
        failures.append("{} unexpected entr(ies): {}".format(len(extra), _examples(extra)))
    odd = sorted(
        name
        for name in expected & set(present)
        if present[name].is_symlink() or not present[name].is_file()
    )
    if odd:
        failures.append("{} entr(ies) not regular files: {}".format(len(odd), _examples(odd)))
    return failures


def _link_kind_failures(group, where: str) -> List[str]:
    failures = []
    for name in group:
        link = group.get(name, getlink=True)
        if not isinstance(link, h5py.HardLink):
            failures.append(
                "{}/{} is a {}; every object must be physically present".format(
                    where, name, type(link).__name__
                )
            )
    return failures


def _v3_dataset_failures(dataset, name: str, dtype: str, is_vec: bool) -> List[str]:
    failures = []
    kind = dataset.id.get_type()
    if dataset.dtype.str != np.dtype(dtype).str or kind.get_order() != h5py.h5t.ORDER_LE:
        failures.append(
            "{} dtype {} is not explicit little-endian {}".format(name, dataset.dtype, dtype)
        )
    if is_vec:
        if dataset.ndim != 2 or dataset.shape[1] != 3:
            failures.append("{} shape {} is not [n_halos, 3]".format(name, dataset.shape))
    else:
        if dataset.ndim != 1:
            failures.append("{} shape {} is not [n_halos]".format(name, dataset.shape))
    chunks = dataset.chunks
    rank = 2 if is_vec else 1
    width_ok = not is_vec or (chunks is not None and len(chunks) == 2 and chunks[1] == 3)
    if chunks is None or len(chunks) != rank or not 1 <= chunks[0] <= CHUNK_1D[0] or not width_ok:
        failures.append(
            "{} chunks {} violate the rule: chunked, rank {}{}, 1 <= chunk rows <= {}".format(
                name,
                chunks,
                rank,
                ", vector width 3" if is_vec else "",
                CHUNK_1D[0],
            )
        )
    failures += _filter_failures(dataset, name)
    if dataset.external or dataset.is_virtual:
        failures.append("{} is stored outside its file".format(name))
    if dataset.attrs.keys():
        failures.append("{} carries attributes {}".format(name, sorted(dataset.attrs.keys())))
    return failures


def _vlen_utf8_scalar(attrs, key: str) -> bool:
    attr = attrs.get_id(key)
    kind = attr.get_type()
    return (
        isinstance(kind, h5py.h5t.TypeStringID)
        and kind.is_variable_str()
        and kind.get_cset() == h5py.h5t.CSET_UTF8
        and attr.shape == ()
    )


def _v3_schema_structure(schema_group) -> Tuple[List[str], Dict[str, Tuple[str, ...]]]:
    """The file's own /schema declarations, and what is wrong with them as
    declarations (the dataset agreement is checked by the caller)."""
    failures = _link_kind_failures(schema_group, "/schema")
    if schema_group.attrs.keys():
        failures.append("/schema carries attributes {}".format(sorted(schema_group.attrs.keys())))
    declared: Dict[str, Tuple[str, ...]] = {}
    fixed = {name for name, _dtype in _V3_FIXED_DATASETS}
    for name in sorted(schema_group.keys()):
        group = schema_group.get(name)
        where = "/schema/{}".format(name)
        if not isinstance(group, h5py.Group):
            failures.append("{} is not a group".format(where))
            continue
        if len(group):
            failures.append("{} has children {}".format(where, sorted(group.keys())))
        keys = set(group.attrs.keys())
        if keys != set(_V3_SCHEMA_KEYS):
            failures.append(
                "{} attributes {} != exactly {}".format(where, sorted(keys), list(_V3_SCHEMA_KEYS))
            )
            continue
        bad = [key for key in _V3_SCHEMA_KEYS if not _vlen_utf8_scalar(group.attrs, key)]
        if bad:
            failures.append("{} attribute(s) {} are not scalar UTF-8 strings".format(where, bad))
            continue
        values = tuple(str(group.attrs[key]) for key in _V3_SCHEMA_KEYS)
        type_name, units, h_convention, description = values
        if name in fixed:
            failures.append("{} redeclares a fixed-table dataset".format(where))
        if type_name not in _V3_TYPES:
            failures.append("{} type {!r} is not declarable".format(where, type_name))
        if h_convention not in _V3_H_CONVENTIONS:
            failures.append(
                "{} h_convention {!r} is not one of {}".format(
                    where, h_convention, _V3_H_CONVENTIONS
                )
            )
        if not units or not description:
            failures.append("{} has an empty units or description".format(where))
        pinned = _V3_PINNED_TYPES.get(name)
        if pinned is not None and type_name != pinned:
            failures.append(
                "{} type {!r}; the format table pins {!r}".format(where, type_name, pinned)
            )
        declared[name] = values
    missing = sorted(set(_V3_CORE_PAYLOAD + tuple(_V3_PINNED_TYPES)) - set(declared))
    if missing:
        failures.append("/schema lacks core payload declaration(s) {}".format(missing))
    return failures, declared


def snapshot_structure(path: Path) -> Tuple[List[str], Optional[Dict[str, Tuple[str, ...]]]]:
    """Exact object set, header attribute set and types, /schema, and every
    /halos dataset against the fixed table or its own declaration."""
    failures: List[str] = []
    with h5py.File(path, "r") as handle:
        root = set(handle.keys())
        if root != {"header", "halos", "schema"}:
            return [
                "root object set {} != {{'halos', 'header', 'schema'}}".format(sorted(root))
            ], None
        failures += _link_kind_failures(handle, "")
        if handle.attrs.keys():
            failures.append("root carries attributes {}".format(sorted(handle.attrs.keys())))
        groups = [
            name
            for name in ("header", "halos", "schema")
            if not isinstance(handle.get(name), h5py.Group)
        ]
        if groups:
            return failures + ["/{} must be groups".format(", /".join(groups))], None

        header = handle["header"]
        if len(header):
            failures.append("/header has children {}".format(sorted(header.keys())))
        attrs = header.attrs
        expected_attrs = set(HEADER_ATTRS) | set(_V3_STRING_ATTRS)
        names = set(attrs.keys())
        if names != expected_attrs:
            failures.append(
                "header attribute set mismatch: missing {}, extra {}".format(
                    sorted(expected_attrs - names), sorted(names - expected_attrs)
                )
            )
        for name in sorted(names & set(HEADER_ATTRS)):
            actual = np.asarray(attrs[name])
            if actual.dtype != np.dtype(HEADER_ATTRS[name]) or actual.shape != ():
                failures.append(
                    "attribute {} has dtype {} shape {}, the format requires scalar {}".format(
                        name, actual.dtype, actual.shape, np.dtype(HEADER_ATTRS[name])
                    )
                )
        for name in sorted(names & set(_V3_STRING_ATTRS)):
            attr = attrs.get_id(name)
            kind = attr.get_type()
            if (
                not isinstance(kind, h5py.h5t.TypeStringID)
                or kind.is_variable_str()
                or kind.get_size() != _V3_STRING_ATTRS[name]
                or kind.get_cset() != h5py.h5t.CSET_ASCII
                or attr.shape != ()
            ):
                failures.append(
                    "attribute {} is not a scalar {}-byte fixed ASCII string".format(
                        name, _V3_STRING_ATTRS[name]
                    )
                )

        schema_failures, declared = _v3_schema_structure(handle["schema"])
        failures += schema_failures

        halos = handle["halos"]
        failures += _link_kind_failures(halos, "/halos")
        if halos.attrs.keys():
            failures.append("/halos carries attributes {}".format(sorted(halos.attrs.keys())))
        table = {name: (dtype, False) for name, dtype in _V3_FIXED_DATASETS}
        for name, values in declared.items():
            if values[0] in _V3_TYPES and name not in table:
                table[name] = _V3_TYPES[values[0]]
        present = set(halos.keys())
        if present != set(table):
            failures.append(
                "/halos dataset set mismatch against the fixed table plus /schema: missing {}, "
                "extra {}".format(sorted(set(table) - present), sorted(present - set(table)))
            )
        lengths = set()
        for name in sorted(present & set(table)):
            dataset = halos.get(name)
            if not isinstance(dataset, h5py.Dataset):
                failures.append("/halos/{} is not a dataset".format(name))
                continue
            dtype, is_vec = table[name]
            failures += _v3_dataset_failures(dataset, "/halos/{}".format(name), dtype, is_vec)
            if dataset.ndim:
                lengths.add(int(dataset.shape[0]))
        if len(lengths) > 1:
            failures.append("/halos datasets differ in length: {}".format(sorted(lengths)))
    return failures, declared


def sidecar_structure(path: Path) -> List[str]:
    """forests.h5: exactly three int64 little-endian 1-D root datasets of one
    length, chunked and unfiltered, with no attributes anywhere."""
    failures: List[str] = []
    with h5py.File(path, "r") as handle:
        names = set(handle.keys())
        if names != set(_V3_SIDECAR):
            return ["object set {} != {}".format(sorted(names), sorted(_V3_SIDECAR))]
        failures += _link_kind_failures(handle, "")
        if handle.attrs.keys():
            failures.append("root carries attributes {}".format(sorted(handle.attrs.keys())))
        lengths = set()
        for name in _V3_SIDECAR:
            dataset = handle.get(name)
            if not isinstance(dataset, h5py.Dataset):
                failures.append("/{} is not a dataset".format(name))
                continue
            failures += _v3_dataset_failures(dataset, "/{}".format(name), "<i8", False)
            lengths.add(int(dataset.shape[0]) if dataset.ndim else -1)
        if len(lengths) > 1:
            failures.append("sidecar datasets differ in length: {}".format(sorted(lengths)))
    return failures


# ---------------------------------------------------------------------------
# Manifest binding
# ---------------------------------------------------------------------------


def load_manifest(manifest_path: Path):
    """The generic conversion manifest the dataset claims, loaded with its
    own integrity checks (configuration digest, embedded schema identity).
    Every path that validates or reports a conversion loads it here, so an
    ASCII conversion that predates the forest-rank identity scheme is
    refused (:class:`IdentitySchemeError`) before anything else is read."""
    manifest = ConversionManifest.load(Path(manifest_path).parent)
    manifest.require_current_identity_scheme()
    return manifest


def try_load_manifest(manifest_path: Path):
    """``(manifest, None)``, or ``(None, error)`` when the manifest does not
    load; the error is a manifest-binding failure, never a refusal. An
    outdated identity scheme is refused, not returned."""
    try:
        return load_manifest(manifest_path), None
    except IdentitySchemeError:
        raise
    except (ConverterError, OSError, ValueError, KeyError) as exc:
        return None, exc


def check_manifest_binding(directory: Path, a_list_path, a_list: np.ndarray, manifest) -> List[str]:
    """The manifest must describe THIS dataset: a completed version-3 write
    whose registered outputs are exactly these files by name and SHA-256,
    produced from the supplied a_list."""
    failures = []
    write = manifest.stage("write")
    if write.get("status") != "complete":
        return ["the manifest's write stage is {!r}, not complete".format(write.get("status"))]
    writer = (write.get("result") or {}).get("writer") or {}
    if writer.get("format_version") != V3_FORMAT_VERSION:
        failures.append(
            "the manifest's write stage was completed by writer {} rather than a version {} "
            "writer".format(writer, V3_FORMAT_VERSION)
        )
    recorded = {}
    for relpath in write.get("artifacts", []):
        entry = manifest.artifact(relpath) or {}
        name = Path(relpath).name
        if name in recorded:
            failures.append("output basename {} is registered twice".format(name))
        recorded[name] = entry
    present = {entry.name for entry in directory.iterdir()}
    unrecorded = sorted(present - set(recorded))
    absent = sorted(set(recorded) - present)
    if unrecorded:
        failures.append(
            "{} file(s) not registered as write outputs: {}".format(
                len(unrecorded), _examples(unrecorded)
            )
        )
    if absent:
        failures.append(
            "{} registered write output(s) missing: {}".format(len(absent), _examples(absent))
        )
    mismatched = []
    for name in sorted(present & set(recorded)):
        entry = recorded[name]
        if entry.get("status") != "present" or sha256_file(directory / name) != entry.get("sha256"):
            mismatched.append(name)
    if mismatched:
        failures.append(
            "{} file(s) whose content differs from the registered write output: {}".format(
                len(mismatched), _examples(mismatched)
            )
        )
    pinned = [record for record in manifest.dependencies if "a_list" in record.get("roles", ())]
    supplied = sha256_file(a_list_path)
    if not pinned or any(record.get("sha256") != supplied for record in pinned):
        failures.append(
            "supplied a_list content sha256 {} is not the a_list this conversion pinned".format(
                supplied
            )
        )
    recorded_scales = [
        float(value) for value in manifest.configuration["snapshots"]["scale_factors"]
    ]
    if recorded_scales != [float(value) for value in a_list]:
        failures.append("supplied a_list scale factors differ from the conversion's")
    return failures


# ---------------------------------------------------------------------------
# Headers
# ---------------------------------------------------------------------------


def read_header(path: Path) -> Dict[str, object]:
    with h5py.File(path, "r") as handle:
        attrs = handle["header"].attrs
        values: Dict[str, object] = {
            name: np.asarray(attrs[name])[()].item() for name in HEADER_ATTRS
        }
        for name in _V3_STRING_ATTRS:
            raw = attrs[name]
            values[name] = raw.decode("ascii") if isinstance(raw, bytes) else str(raw)
        values["_rows"] = int(handle["halos"]["SourceHaloID"].shape[0])
        values["_field_rows"] = {
            name: int(handle["halos"][name].shape[0]) for name in handle["halos"]
        }
        values["_bytes"] = int(path.stat().st_size)
    return values


_V3_RUN_SCOPED = RUN_SCOPED_ATTRS + ("links_adjacent", "source_format", "column_mapping_sha256")


def check_headers(
    headers: List[Dict[str, object]], a_list: np.ndarray, manifest
) -> Tuple[List[str], List[str]]:
    failures = []
    run_scoped = []
    observed = {name: set() for name in _V3_RUN_SCOPED}
    for snap, header in enumerate(headers):
        name = snapshot_h5_name(snap)
        for attr in _V3_RUN_SCOPED:
            observed[attr].add(repr(header[attr]))
        if header["format_version"] != V3_FORMAT_VERSION:
            failures.append(
                "{}: format_version {} != {}".format(
                    name, header["format_version"], V3_FORMAT_VERSION
                )
            )
        if header["links_adjacent"] not in (0, 1):
            failures.append(
                "{}: links_adjacent {} is not 0 or 1".format(name, header["links_adjacent"])
            )
        if header["snapshot_number"] != snap:
            failures.append(
                "{}: snapshot_number {} != filename index {}".format(
                    name, header["snapshot_number"], snap
                )
            )
        if float(header["scale_factor"]) != float(a_list[snap]):
            failures.append(
                "{}: scale_factor {!r} != a_list[{}] = {!r}".format(
                    name, header["scale_factor"], snap, float(a_list[snap])
                )
            )
        for field, rows in header["_field_rows"].items():
            if rows != header["n_halos"]:
                failures.append(
                    "{}: dataset {} has {} rows, header n_halos is {}".format(
                        name, field, rows, header["n_halos"]
                    )
                )
        if header["source_format"] not in _V3_SOURCE_FORMATS:
            failures.append(
                "{}: source_format {!r} is not a known adapter".format(
                    name, header["source_format"]
                )
            )
        digest = header["column_mapping_sha256"]
        if len(digest) != 64 or any(c not in "0123456789abcdef" for c in digest):
            failures.append(
                "{}: column_mapping_sha256 {!r} is not lowercase hex".format(name, digest)
            )
        if manifest is not None:
            if header["source_format"] != manifest.schema.source_format:
                failures.append(
                    "{}: source_format {!r} != the conversion's {!r}".format(
                        name, header["source_format"], manifest.schema.source_format
                    )
                )
            if digest != manifest.schema.digest:
                failures.append(
                    "{}: column_mapping_sha256 {} != the conversion's schema {}".format(
                        name, digest, manifest.schema.digest
                    )
                )
    for attr in _V3_RUN_SCOPED:
        if len(observed[attr]) > 1:
            run_scoped.append(
                "{} differs across files: {}".format(attr, _examples(sorted(observed[attr])))
            )
    if manifest is not None and headers:
        writer = ((manifest.stage("write").get("result") or {}).get("writer") or {}).get("header")
        if writer is not None:
            for attr, value in writer.items():
                if any(header.get(attr) != value for header in headers):
                    run_scoped.append(
                        "{} differs from the value the conversion's writer recorded ({!r})".format(
                            attr, value
                        )
                    )
    return failures, run_scoped


def check_schema_binding(declarations: List[Dict[str, Tuple[str, ...]]], manifest) -> List[str]:
    """Every file declares the same /schema, and it is the conversion's."""
    failures = []
    reference = declarations[0] if declarations else {}
    differing = [
        snapshot_h5_name(snap)
        for snap, declared in enumerate(declarations)
        if declared != reference
    ]
    if differing:
        failures.append(
            "/schema differs from {}'s in {} file(s): {}".format(
                snapshot_h5_name(0), len(differing), _examples(differing)
            )
        )
    if manifest is not None:
        expected = {
            field.name: tuple(getattr(field, key) for key in _V3_SCHEMA_KEYS)
            for field in manifest.schema.output_field_declarations()
        }
        for snap, declared in enumerate(declarations):
            if declared != expected:
                failures.append(
                    "{}: /schema differs from the conversion's embedded schema (missing {}, "
                    "extra {}, changed {})".format(
                        snapshot_h5_name(snap),
                        sorted(set(expected) - set(declared)),
                        sorted(set(declared) - set(expected)),
                        sorted(
                            name
                            for name in set(expected) & set(declared)
                            if expected[name] != declared[name]
                        ),
                    )
                )
    return failures


# ---------------------------------------------------------------------------
# The row scan
# ---------------------------------------------------------------------------


def float_columns(declarations: Dict[str, Tuple[str, ...]]) -> List[str]:
    """The ``/schema`` fields declared with a floating type, in name order:
    the columns whose finiteness the row check judges."""
    return [
        name
        for name, values in sorted(declarations.items())
        if _V3_TYPES.get(values[0], ("", False))[0].startswith("<f")
    ]


class RowScan:
    """The per-row rules both modes share: one pass over every snapshot
    file in row blocks of ``scan_rows``, judging every row as it is read.

    The block check settles the per-row invariants (row-values,
    len-nonnegative, field-finiteness, position-bounds, link-targets) into the
    :class:`FailureTally` attributes, accumulates the measurements in
    ``stats``, and hands what crosses rows to the scan's sinks: each block's
    ``(forest, rank, sid)`` identity records to ``identity_sink.add``, its
    ``sid`` records to ``source_key_sink.add`` unless that sink is None (no
    record is then built), and, while every link seen so far is in range, its
    links to :meth:`emit_topology`, which a mode implements. ``meter`` is the
    mode's residency meter.
    """

    def __init__(
        self,
        directory: Path,
        counts: List[int],
        box_sizes: List[float],
        declarations: Dict[str, Tuple[str, ...]],
        n_forests_total: Optional[int],
        *,
        scan_rows: int,
        meter: ResidencyMeter,
        identity_sink,
        source_key_sink,
    ):
        self.directory = directory
        self.counts = np.asarray(counts, dtype=np.int64)
        self.n_snapshots = len(counts)
        self.offsets = np.zeros(self.n_snapshots + 1, dtype=np.int64)
        np.cumsum(self.counts, out=self.offsets[1:])
        self.n_forests_total = n_forests_total
        self.box_sizes = [float(box) for box in box_sizes]
        self.floats = float_columns(declarations)
        self.position_only_columns = [] if "Pos" in self.floats else ["Pos"]
        self.scan_rows = scan_rows
        self.meter = meter
        self.identity = identity_sink
        self.sids = source_key_sink
        self.row_values = FailureTally()
        self.lengths = FailureTally()
        self.finiteness = FailureTally()
        self.positions = FailureTally()
        self.links = FailureTally()
        self.identity_ranges = FailureTally()
        self.len_zero = 0
        self.links_ok = True
        self.n_chain_edges = 0
        self.stats = {
            "non_null_links": {name: 0 for name, _dtype in _V3_FIXED_DATASETS[:5]},
            "gapped_descendants": 0,
            "max_descendant_span": 0,
            "gapped_first_progenitors": 0,
            "max_first_progenitor_span": 0,
            "next_progenitors_off_owner_snapshot": 0,
            "max_link_row": -1,
            "min_source_halo_id": None,
            "max_source_halo_id": None,
            "max_forest_index": -1,
            "max_halo_rank_in_forest": -1,
        }

    def close(self) -> None:
        """Release whatever the sinks hold; nothing by default."""
        return None

    def run(self) -> None:
        for snap in range(self.n_snapshots):
            self._scan_snapshot(snap)

    def emit_topology(self, snap, rows, targets, first_fof, next_fof, forest) -> None:
        """Hand one block's links on. ``rows`` are the block's row numbers in
        snapshot ``snap``; ``targets`` maps each snapshot-qualified link to its
        ``(index, target snapshot, linked mask, null mask)``; ``first_fof``,
        ``next_fof`` and ``forest`` are the block's FoF links and ForestIndex.
        Only reached while every link seen so far is in range."""
        raise NotImplementedError

    def _scan_snapshot(self, snap: int) -> None:
        names = (
            [name for name, _dtype in _V3_FIXED_DATASETS]
            + ["SnapNum", "Len"]
            + self.floats
            + self.position_only_columns
        )
        previous_sid = None
        with h5py.File(self.directory / snapshot_h5_name(snap), "r") as handle:
            halos = handle["halos"]
            n = int(self.counts[snap])
            for start in range(0, n, self.scan_rows):
                stop = min(start + self.scan_rows, n)
                block = {name: halos[name][start:stop] for name in names}
                previous_sid = self._check_block(snap, start, block, previous_sid)

    def _check_block(self, snap: int, start: int, block: Dict[str, np.ndarray], previous_sid):
        size = int(block["SourceHaloID"].size)
        rows = np.arange(start, start + size, dtype=np.int64)
        n = int(self.counts[snap])
        stats = self.stats

        # -- row values: SnapNum, SourceHaloID order, Len, finiteness ------
        bad = block["SnapNum"] != snap
        self.row_values.add(
            "SnapNum value(s) != the file's snapshot", int(bad.sum()), _v3_rows(snap, rows[bad])
        )
        sid = block["SourceHaloID"].astype(np.int64)
        bad = sid <= 0
        self.row_values.add(
            "non-positive SourceHaloID value(s)", int(bad.sum()), _v3_rows(snap, rows[bad])
        )
        before = np.empty(size, dtype=np.int64)
        before[0] = sid[0] - 1 if previous_sid is None else previous_sid
        before[1:] = sid[:-1]
        bad = sid <= before
        self.row_values.add(
            "row(s) not strictly ascending in SourceHaloID",
            int(bad.sum()),
            _v3_rows(snap, rows[bad]),
        )
        stats["min_source_halo_id"] = _min_none(stats["min_source_halo_id"], int(sid.min()))
        stats["max_source_halo_id"] = _max_none(stats["max_source_halo_id"], int(sid.max()))
        length = block["Len"]
        bad = length < 0
        self.lengths.add("negative Len value(s)", int(bad.sum()), _v3_rows(snap, rows[bad]))
        self.len_zero += int(np.count_nonzero(length == 0))
        for name in self.floats:
            values = block[name]
            bad = ~np.isfinite(values)
            if bad.ndim == 2:
                bad = bad.any(axis=1)
            self.finiteness.add(
                "non-finite {} value(s)".format(name), int(bad.sum()), _v3_rows(snap, rows[bad])
            )

        # -- positions inside the header's box (see check_position_bounds) ----
        box = self.box_sizes[snap]
        position = block["Pos"].reshape(size, -1)
        bad = ((position < 0) | (position > box)).any(axis=1)
        self.positions.add(
            "halo(s) with a Pos component outside [0, box_size_mpc_h = {!r}]".format(box),
            int(bad.sum()),
            [
                "{} row {} Pos {}".format(
                    snapshot_h5_name(snap), int(row), [float(value) for value in values]
                )
                for row, values in zip(rows[bad][:5], position[bad][:5])
            ],
        )

        # -- identity ranges -----------------------------------------------
        forest = block["ForestIndex"].astype(np.int64)
        rank = block["HaloRankInForest"].astype(np.int64)
        stats["max_forest_index"] = max(stats["max_forest_index"], int(forest.max()))
        stats["max_halo_rank_in_forest"] = max(stats["max_halo_rank_in_forest"], int(rank.max()))
        records = np.empty(size, dtype=_V3_IDENTITY_DTYPE)
        records["forest"] = forest
        records["rank"] = rank
        records["sid"] = sid
        self.identity.add(records)
        if self.sids is not None:
            records = np.empty(size, dtype=_V3_SID_DTYPE)
            records["sid"] = sid
            self.sids.add(records)

        # -- snapshot-qualified links --------------------------------------
        before_links = bool(self.links)
        targets = {}
        for link, column in _V3_QUALIFIED:
            index = block[link].astype(np.int64)
            target = block[column].astype(np.int64)
            null = index == -1
            mismatch = null != (target == -1)
            self.links.add(
                "{} / {} null-ness disagreements".format(link, column),
                int(mismatch.sum()),
                _v3_rows(snap, rows[mismatch]),
            )
            linked = ~null & ~mismatch
            bad = index < -1
            self.links.add(
                "{} value(s) below -1".format(link), int(bad.sum()), _v3_rows(snap, rows[bad])
            )
            bad = linked & ((target < 0) | (target >= self.n_snapshots))
            self.links.add(
                "{} value(s) naming a snapshot outside the dataset".format(column),
                int(bad.sum()),
                _v3_rows(snap, rows[bad]),
            )
            linked &= ~bad & (index >= 0)
            safe_target = np.where(linked, target, 0)
            bad = linked & (index >= self.counts[safe_target])
            self.links.add(
                "{} row(s) outside their target snapshot's n_halos".format(link),
                int(bad.sum()),
                _v3_rows(snap, rows[bad]),
            )
            linked &= ~bad
            stats["non_null_links"][link] += int(np.count_nonzero(~null))
            if linked.any():
                stats["max_link_row"] = max(stats["max_link_row"], int(index[linked].max()))
            targets[link] = (index, target, linked, null)

        index, target, linked, null = targets["Descendant"]
        bad = linked & (target <= snap)
        self.links.add(
            "Descendant(s) not strictly later", int(bad.sum()), _v3_rows(snap, rows[bad])
        )
        span = target[linked] - snap
        # span > 1 is the transpose's definition of a gap (source_keys.py)
        stats["gapped_descendants"] += int(np.count_nonzero(span > 1))
        if span.size:
            stats["max_descendant_span"] = max(stats["max_descendant_span"], int(span.max()))
        _, fp_target, fp_linked, _ = targets["FirstProgenitor"]
        bad = fp_linked & (fp_target >= snap)
        self.links.add(
            "FirstProgenitor(s) not strictly earlier", int(bad.sum()), _v3_rows(snap, rows[bad])
        )
        fp_span = snap - fp_target[fp_linked]
        stats["gapped_first_progenitors"] += int(np.count_nonzero(fp_span != 1))
        if fp_span.size:
            stats["max_first_progenitor_span"] = max(
                stats["max_first_progenitor_span"], int(fp_span.max())
            )
        _, np_target, np_linked, np_null = targets["NextProgenitor"]
        bad = ~np_null & null
        self.links.add(
            "NextProgenitor(s) on a halo with no Descendant",
            int(bad.sum()),
            _v3_rows(snap, rows[bad]),
        )
        bad = np_linked & linked & (np_target >= target)
        self.links.add(
            "NextProgenitor(s) not earlier than the shared DescendantSnapshot",
            int(bad.sum()),
            _v3_rows(snap, rows[bad]),
        )
        stats["next_progenitors_off_owner_snapshot"] += int(
            np.count_nonzero(np_linked & (np_target != snap))
        )

        first_fof = block["FirstHaloInFOFgroup"].astype(np.int64)
        next_fof = block["NextHaloInFOFgroup"].astype(np.int64)
        bad = (first_fof < 0) | (first_fof >= n)
        self.links.add(
            "FirstHaloInFOFgroup value(s) outside [0, n_halos)",
            int(bad.sum()),
            _v3_rows(snap, rows[bad]),
        )
        bad = (next_fof < -1) | (next_fof >= n)
        self.links.add(
            "NextHaloInFOFgroup value(s) outside [-1, n_halos)",
            int(bad.sum()),
            _v3_rows(snap, rows[bad]),
        )
        for values in (first_fof, next_fof):
            if values.size:
                stats["max_link_row"] = max(stats["max_link_row"], int(values.max()))
        if self.n_forests_total is not None:
            bad = (forest < 0) | (forest >= self.n_forests_total)
            self.identity_ranges.add(
                "ForestIndex value(s) outside [0, n_forests_total)",
                int(bad.sum()),
                _v3_rows(snap, rows[bad]),
            )
        bad = rank < 0
        self.identity_ranges.add(
            "negative HaloRankInForest value(s)", int(bad.sum()), _v3_rows(snap, rows[bad])
        )

        if bool(self.links) and not before_links:
            self.links_ok = False
        if self.links_ok:
            self.emit_topology(snap, rows, targets, first_fof, next_fof, forest)
        return int(sid[-1])


class _ExternalSortScan(RowScan):
    """The external-sort mode's scan: the shared row check, feeding four
    bounded external sorts whose merges settle everything that crosses rows,
    snapshots or files."""

    def __init__(
        self,
        directory: Path,
        counts: List[int],
        box_sizes: List[float],
        declarations: Dict[str, Tuple[str, ...]],
        n_forests_total: Optional[int],
        budget_bytes: int,
        spill_root: Path,
    ):
        meter = ResidencyMeter()
        self.budget_bytes = budget_bytes
        self.ledger = SpillLedger()
        self.spill_root = spill_root

        def sorter(dtype, key, share, tag):
            return KeyedSorter(
                dtype,
                key,
                budget_bytes=_share(budget_bytes, share),
                spill_dir=spill_root,
                residency=meter,
                ledger=self.ledger,
                tag=tag,
            )

        self._sorter = sorter
        self.join = sorter(_V3_JOIN_DTYPE, _V3_JOIN_KEY, "join", "v3join")
        self.chain = sorter(CHAIN_DTYPE, CHAIN_KEY, "chain", "v3chain")
        identity = sorter(_V3_IDENTITY_DTYPE, _V3_IDENTITY_KEY, "identity", "v3identity")
        sids = sorter(_V3_SID_DTYPE, _V3_SID_KEY, "sid", "v3sid")
        floats = float_columns(declarations)
        position_only_columns = [] if "Pos" in floats else ["Pos"]
        row_bytes = 13 * 8 + 12 * (len(floats) + len(position_only_columns))
        per_row = (
            4 * row_bytes + 5 * _V3_JOIN_DTYPE.itemsize + 4 * CHAIN_DTYPE.itemsize + 32 + 24 * 8
        )
        scan_rows = int(max(1, min(V3_MAX_SCAN_ROWS, _share(budget_bytes, "scan") // per_row)))
        super().__init__(
            directory,
            counts,
            box_sizes,
            declarations,
            n_forests_total,
            scan_rows=scan_rows,
            meter=meter,
            identity_sink=identity,
            source_key_sink=sids,
        )
        self.scan_bytes = self.scan_rows * per_row

    def close(self) -> None:
        for sorter in (self.join, self.chain, self.identity, self.sids):
            sorter.close()

    def run(self) -> None:
        self.meter.acquire(self.scan_bytes)
        try:
            super().run()
        finally:
            self.meter.release(self.scan_bytes)
        for sorter in (self.join, self.chain, self.identity, self.sids):
            sorter.seal()

    def emit_topology(self, snap, rows, targets, first_fof, next_fof, forest) -> None:
        """Join requests and pointer-jumping edges for one block, in global
        positions."""
        offsets = self.offsets
        gp = offsets[snap] + rows
        d_index, d_target, d_linked, _ = targets["Descendant"]
        desc_gp = np.where(d_linked, offsets[np.where(d_linked, d_target, 0)] + d_index, -1)
        central_gp = offsets[snap] + first_fof
        parts = []

        def part(mask, key, tag, a):
            count = int(np.count_nonzero(mask)) if mask is not None else int(gp.size)
            records = np.empty(count, dtype=_V3_JOIN_DTYPE)
            pick = slice(None) if mask is None else mask
            records["key"] = key if mask is None else key[mask]
            records["tag"] = tag
            records["other"] = gp[pick]
            records["a"] = a[pick]
            records["b"] = central_gp[pick]
            records["forest"] = forest[pick]
            parts.append(records)

        part(None, gp, 0, desc_gp)
        fp_index, fp_target, fp_linked, _ = targets["FirstProgenitor"]
        fp_gp = offsets[np.where(fp_linked, fp_target, 0)] + fp_index
        part(fp_linked, fp_gp, 1, gp)
        np_index, np_target, np_linked, _ = targets["NextProgenitor"]
        np_gp = offsets[np.where(np_linked, np_target, 0)] + np_index
        part(np_linked, np_gp, 2, desc_gp)
        part(np.ones(gp.size, dtype=bool), central_gp, 3, central_gp)
        nf_linked = next_fof != -1
        nf_gp = offsets[snap] + next_fof
        part(nf_linked, nf_gp, 4, central_gp)
        for records in parts:
            self.join.add(records)

        for chain, mask, successor in ((0, np_linked, np_gp), (1, nf_linked, nf_gp)):
            count = int(np.count_nonzero(mask))
            if not count:
                continue
            edges = np.empty(2 * count, dtype=CHAIN_DTYPE)
            nodes, asks = edges[:count], edges[count:]
            nodes["chain"] = chain
            nodes["key"] = gp[mask]
            nodes["tag"] = 0
            nodes["other"] = successor[mask]
            asks["chain"] = chain
            asks["key"] = successor[mask]
            asks["tag"] = 1
            asks["other"] = gp[mask]
            self.chain.add(edges)
            self.n_chain_edges += count


def check_position_bounds(
    scan: RowScan, headers: List[Dict[str, object]], declared: Dict[str, Tuple[str, ...]]
) -> List[str]:
    """Every Pos component lies in [0, box_size_mpc_h] of its own file's
    header, as the scan measured it row by row.

    This binds the physical header to the data it labels: a dataset of a
    500 Mpc/h box stamped with a 62.5 Mpc/h box's simulation_info (the only
    field in which mini-Millennium and Millennium metadata differ) places
    halos outside the stated box and fails here. The reverse -- a smaller
    box's halos stamped with a larger box -- leaves every position inside
    the stated box and is not detectable this way. The bound is in Mpc/h, so
    a Pos declared in other units, or a box that is not a positive finite
    length, fails rather than being compared.
    """
    failures = []
    units = declared["Pos"][1]
    if units != "Mpc/h":
        failures.append(
            "Pos is declared in {!r}, but box_size_mpc_h bounds it in Mpc/h".format(units)
        )
    for snap, header in enumerate(headers):
        box = float(header["box_size_mpc_h"])
        if not (math.isfinite(box) and box > 0.0):
            failures.append(
                "{}: box_size_mpc_h {!r} is not a positive finite length".format(
                    snapshot_h5_name(snap), box
                )
            )
    return failures + scan.positions.messages()


def _min_none(current, value):
    return value if current is None else min(current, value)


def _max_none(current, value):
    return value if current is None else max(current, value)


# ---------------------------------------------------------------------------
# Merge consumers
# ---------------------------------------------------------------------------


def describe_position(offsets: np.ndarray, gp: int) -> str:
    position = int(np.searchsorted(offsets, gp, side="right")) - 1
    return "{} row {}".format(snapshot_h5_name(position), int(gp - offsets[position]))


def link_examples(offsets: np.ndarray, owners, targets, mask) -> List[str]:
    """Up to five ``owner -> target`` links, located by file and row."""
    return [
        "{} -> {}".format(describe_position(offsets, owner), describe_position(offsets, target))
        for owner, target in zip(owners[mask][:5], targets[mask][:5])
    ]


def reach_examples(offsets: np.ndarray, positions, reached) -> List[str]:
    """Up to five halos, by global position, with how often they were reached."""
    return [
        "{} (k={})".format(describe_position(offsets, g), int(k))
        for g, k in zip(positions[:5], reached[:5])
    ]


def _v3_topology_closure(scan: _ExternalSortScan) -> List[str]:
    """Consume the sorted topology join: every link into a halo is judged
    against that halo's own record, and every halo's in-links are counted.

    A group is a halo's own record followed by the links into it. The last
    group of a block may continue into the next, so its record and running
    counts are carried; a group is judged once the next own-record appears.
    """
    failures = FailureTally()
    offsets = scan.offsets
    carry = None  # (key, a, b, forest, progenitor_in, fof_in)

    def judge(key, a, b, prog_in, fof_in):
        expected_prog = (a != -1).astype(np.int64)
        expected_fof = (b != key).astype(np.int64)
        bad = prog_in != expected_prog
        failures.add(
            KIND_PROGENITOR_REACH, int(bad.sum()), reach_examples(offsets, key[bad], prog_in[bad])
        )
        bad = fof_in != expected_fof
        failures.add(KIND_FOF_REACH, int(bad.sum()), reach_examples(offsets, key[bad], fof_in[bad]))

    blocks = scan.join.sorted_blocks(
        budget_bytes=scan.budget_bytes, consumer_bytes_per_record=_V3_JOIN_CONSUMER_BYTES
    )
    try:
        for block in blocks:
            size = int(block.size)
            tag = block["tag"]
            key = block["key"]
            is_self = tag == 0
            last = np.where(is_self, np.arange(size), -1)
            np.maximum.accumulate(last, out=last)
            requests = np.flatnonzero(~is_self)
            source = last[requests]
            inside = source >= 0
            safe = np.maximum(source, 0)
            if carry is None:
                carry_values = (-2, -1, -1, -1)
            else:
                carry_values = carry[:4]
            g_key = np.where(inside, key[safe], carry_values[0])
            g_a = np.where(inside, block["a"][safe], carry_values[1])
            g_b = np.where(inside, block["b"][safe], carry_values[2])
            g_forest = np.where(inside, block["forest"][safe], carry_values[3])
            r_key = key[requests]
            r_tag = tag[requests]
            r_a = block["a"][requests]
            r_other = block["other"][requests]

            def where(mask, owners=r_other, targets=r_key):
                return link_examples(offsets, owners, targets, mask)

            orphan = g_key != r_key
            failures.add("link(s) naming a halo that has no row", int(orphan.sum()), where(orphan))
            matched = ~orphan
            prog = matched & ((r_tag == 1) | (r_tag == 2))
            bad = prog & (r_tag == 1) & (g_a != r_a)
            failures.add(KIND_FIRST_PROGENITOR, int(bad.sum()), where(bad))
            bad = prog & (r_tag == 2) & (g_a != r_a)
            failures.add(KIND_NEXT_PROGENITOR, int(bad.sum()), where(bad))
            bad = matched & (r_tag == 3) & (g_b != g_key)
            failures.add(KIND_FOF_CENTRAL, int(bad.sum()), where(bad))
            fof = matched & (r_tag == 4)
            bad = fof & (g_b != r_a)
            failures.add(KIND_FOF_MEMBER, int(bad.sum()), where(bad))
            bad = matched & (g_forest != block["forest"][requests])
            failures.add(KIND_CROSSING_FORESTS, int(bad.sum()), where(bad))

            # in-link counts per own-record of this block, plus the carry's
            prog_in = np.bincount(source[prog & inside], minlength=size)
            fof_in = np.bincount(source[fof & inside], minlength=size)
            selves = np.flatnonzero(is_self)
            if carry is not None:
                carry = carry[:4] + (
                    carry[4] + int(np.count_nonzero(prog & ~inside)),
                    carry[5] + int(np.count_nonzero(fof & ~inside)),
                )
            if selves.size:
                if carry is not None:
                    judge(
                        np.asarray([carry[0]]),
                        np.asarray([carry[1]]),
                        np.asarray([carry[2]]),
                        np.asarray([carry[4]]),
                        np.asarray([carry[5]]),
                    )
                done = selves[:-1]
                judge(key[done], block["a"][done], block["b"][done], prog_in[done], fof_in[done])
                tail = int(selves[-1])
                carry = (
                    int(key[tail]),
                    int(block["a"][tail]),
                    int(block["b"][tail]),
                    int(block["forest"][tail]),
                    int(prog_in[tail]),
                    int(fof_in[tail]),
                )
    finally:
        blocks.close()
    if carry is not None:
        judge(
            np.asarray([carry[0]]),
            np.asarray([carry[1]]),
            np.asarray([carry[2]]),
            np.asarray([carry[4]]),
            np.asarray([carry[5]]),
        )
    return failures.messages()


def _v3_chain_cycles(scan: _ExternalSortScan) -> Tuple[List[str], int]:
    """Prove every NextProgenitor and NextHaloInFOFgroup chain terminates."""
    half = scan.budget_bytes // 2
    layout = SnapshotLayout(list(range(scan.n_snapshots)), scan.counts.tolist())

    def new_round():
        return KeyedSorter(
            CHAIN_DTYPE,
            CHAIN_KEY,
            budget_bytes=half,
            spill_dir=scan.spill_root,
            residency=scan.meter,
            ledger=scan.ledger,
            tag="v3chain",
        )

    try:
        rounds = verify_chains_acyclic(
            scan.chain, scan.n_chain_edges, layout, merge_budget_bytes=half, new_round=new_round
        )
    except ConverterError as exc:
        return [str(exc)], -1
    return [], rounds


def _v3_identity(
    scan: _ExternalSortScan,
    source_format: str,
    n_forests_total: int,
    max_rank: int,
    inventory: Optional[Mapping],
) -> List[str]:
    """Consume the (ForestIndex, HaloRankInForest, SourceHaloID) ordering.

    Every forest's ranks are exactly 0 .. count-1 (dense and unique). For
    Consistent-Trees ASCII every forest holds halos, so ForestIndex is dense
    over [0, n_forests_total); for the two prelinked formats a forest may be a
    zero-halo tree. On every route a forest's halos are one contiguous
    SourceHaloID range in rank order, so in (ForestIndex, rank) order the ids of
    an unsampled conversion are exactly 1, 2, 3, ... -- which, for a complete
    dataset, also proves every slab is grouped by forest. The ASCII route does
    not sample, so a sampled ASCII inventory fails here and the position check
    always applies to it.
    """
    failures = FailureTally()
    carry_forest, carry_rank, position = -1, -1, 0
    measured_max = -1
    position_identity = position_identity_applies(source_format, inventory)
    blocks = scan.identity.sorted_blocks(
        budget_bytes=scan.budget_bytes, consumer_bytes_per_record=_V3_ORDER_CONSUMER_BYTES
    )
    try:
        for block in blocks:
            size = int(block.size)
            forest, rank, sid = block["forest"], block["rank"], block["sid"]
            prev_forest = np.empty(size, dtype=np.int64)
            prev_rank = np.empty(size, dtype=np.int64)
            prev_forest[0], prev_rank[0] = carry_forest, carry_rank
            prev_forest[1:], prev_rank[1:] = forest[:-1], rank[:-1]
            same = forest == prev_forest
            bad = np.where(same, rank != prev_rank + 1, rank != 0)
            failures.add(KIND_RANK_DENSITY, int(bad.sum()), pair_examples(forest[bad], rank[bad]))
            if source_format == "consistent_trees_ascii":
                bad = ~same & (forest != prev_forest + 1)
                failures.add(
                    KIND_ASCII_FOREST_SKIPPED,
                    int(bad.sum()),
                    [
                        "ForestIndex={} after {}".format(f, p)
                        for f, p in zip(forest[bad][:5], prev_forest[bad][:5])
                    ],
                )
            if position_identity:
                expected = position + 1 + np.arange(size, dtype=np.int64)
                bad = sid != expected
                failures.add(
                    KIND_POSITION_IDENTITY,
                    int(bad.sum()),
                    position_examples(forest[bad], rank[bad], sid[bad], expected[bad]),
                )
            measured_max = max(measured_max, int(rank.max()))
            carry_forest, carry_rank = int(forest[-1]), int(rank[-1])
            position += size
    finally:
        blocks.close()
    return failures.messages() + identity_summary(
        source_format, inventory, carry_forest, n_forests_total, measured_max, max_rank
    )


def _v3_source_keys(scan: _ExternalSortScan, inventory: Optional[Mapping]) -> List[str]:
    """Consume the SourceHaloID ordering: globally unique, and -- against the
    conversion's inventory -- within [1, total_halos] and exactly as many as
    the inventory selects, which for an unsampled inventory is exactly
    {1 .. total_halos}."""
    failures = FailureTally()
    previous = 0
    position = 0
    total = None if inventory is None else int(inventory["total_halos"])
    unsampled = is_unsampled(inventory)
    blocks = scan.sids.sorted_blocks(
        budget_bytes=scan.budget_bytes, consumer_bytes_per_record=_V3_ORDER_CONSUMER_BYTES
    )
    try:
        for block in blocks:
            sid = block["sid"]
            before = np.empty(sid.size, dtype=np.int64)
            before[0] = previous
            before[1:] = sid[:-1]
            bad = sid == before
            failures.add(KIND_DUPLICATED_SOURCE_KEY, int(bad.sum()), sid[bad].tolist())
            if total is not None:
                bad = (sid < 1) | (sid > total)
                failures.add(
                    KIND_SOURCE_KEY_OUTSIDE.format(total), int(bad.sum()), sid[bad].tolist()
                )
            if unsampled:
                expected = position + 1 + np.arange(sid.size, dtype=np.int64)
                bad = sid != expected
                failures.add(
                    KIND_SOURCE_KEY_GAP,
                    int(bad.sum()),
                    [
                        "{} where {} was expected".format(s, e)
                        for s, e in zip(sid[bad][:5], expected[bad][:5])
                    ],
                )
            previous = int(sid[-1])
            position += int(sid.size)
    finally:
        blocks.close()
    return failures.messages() + emitted_count_failures(position, inventory)


# ---------------------------------------------------------------------------
# Sidecar content and conservation
# ---------------------------------------------------------------------------


def check_sidecar_content(
    path: Path, source_format: str, n_forests_total: int, inventory, block_rows: int
) -> List[str]:
    failures = FailureTally()
    per_file: Dict[int, int] = {}
    with h5py.File(path, "r") as handle:
        n = int(handle["ForestID"].shape[0])
        if n != n_forests_total:
            return ["forests.h5 holds {} forests, n_forests_total is {}".format(n, n_forests_total)]
        prev = (-1, -1, None)  # (file, unit, forest_id)
        for start in range(0, n, block_rows):
            stop = min(start + block_rows, n)
            fid = handle["ForestID"][start:stop]
            files = handle["SourceFileOrdinal"][start:stop]
            units = handle["SourceUnitOrdinal"][start:stop]
            rows = np.arange(start, stop)
            if source_format == "consistent_trees_ascii":
                spans = (files == -1) & (units == -1)
                bad = ~spans & ((files < 0) | (units < 0))
                failures.add(
                    "forest(s) with one -1 ordinal (spanning forests carry -1 in both)",
                    int(bad.sum()),
                    rows[bad].tolist(),
                )
                before = np.empty(fid.size, dtype=np.int64)
                before[0] = fid[0] - 1 if prev[2] is None else prev[2]
                before[1:] = fid[:-1]
                bad = fid <= before
                failures.add(
                    "ForestID value(s) not strictly ascending (the ASCII forest enumeration)",
                    int(bad.sum()),
                    rows[bad].tolist(),
                )
                prev = (-1, -1, int(fid[-1]))
                continue
            if source_format == "lhalo_binary":
                bad = fid != rows
                failures.add(
                    "ForestID value(s) that are not the dense run forest number",
                    int(bad.sum()),
                    rows[bad].tolist(),
                )
            prev_file = np.empty(fid.size, dtype=np.int64)
            prev_unit = np.empty(fid.size, dtype=np.int64)
            prev_file[0], prev_unit[0] = prev[0], prev[1]
            prev_file[1:], prev_unit[1:] = files[:-1], units[:-1]
            same = files == prev_file
            bad = (files < 0) | np.where(
                same, units != prev_unit + 1, (files <= prev_file) | (units != 0)
            )
            failures.add(
                "forest(s) out of dense (file, unit) inventory order",
                int(bad.sum()),
                rows[bad].tolist(),
            )
            for ordinal, count in zip(*np.unique(files, return_counts=True)):
                per_file[int(ordinal)] = per_file.get(int(ordinal), 0) + int(count)
            prev = (int(files[-1]), int(units[-1]), None)
    messages = failures.messages()
    if inventory is not None and source_format in _V3_PER_FILE_INVENTORY:
        recorded = {
            int(entry["source_file_ordinal"]): int(entry["n_units"]) for entry in inventory["files"]
        }
        if per_file != {k: v for k, v in recorded.items() if v}:
            messages.append(
                "per-file forest counts {} differ from the inventory's units {}".format(
                    dict(sorted(per_file.items())), dict(sorted(recorded.items()))
                )
            )
        if n_forests_total != int(inventory["n_units"]):
            messages.append(
                "n_forests_total {} != the inventory's {} units".format(
                    n_forests_total, inventory["n_units"]
                )
            )
    return messages


def check_count_conservation(counts: List[int], manifest) -> List[str]:
    failures = []
    total = int(sum(counts))
    inventory = manifest.inventory or {}
    if total != int(inventory.get("selected_halos", -1)):
        failures.append(
            "emitted halo total {} != the source inventory's selected {}".format(
                total, inventory.get("selected_halos")
            )
        )
    ingest = manifest.stage("ingest").get("result") or {}
    recorded = ingest.get("snapshot_counts")
    if recorded is None or [int(v) for v in recorded] != [int(v) for v in counts]:
        differing = [
            snap
            for snap, (got, want) in enumerate(zip(counts, recorded or []))
            if int(got) != int(want)
        ]
        failures.append(
            "per-snapshot halo counts differ from the ingest stage's source counts (snapshots {})".format(
                _examples(differing) if differing else "all"
            )
        )
    transpose_total = (manifest.stage("transpose").get("result") or {}).get("total_halos")
    if transpose_total is not None and int(transpose_total) != total:
        failures.append(
            "emitted halo total {} != the transpose's {}".format(total, transpose_total)
        )
    return failures


# ---------------------------------------------------------------------------
# Battery driver
# ---------------------------------------------------------------------------


def require_budget(budget_bytes) -> int:
    if isinstance(budget_bytes, bool) or not isinstance(budget_bytes, (int, np.integer)):
        raise ConverterError("budget_bytes must be an integer, got {!r}".format(budget_bytes))
    budget_bytes = int(budget_bytes)
    if budget_bytes < V3_MIN_BUDGET_BYTES:
        raise ConverterError(
            "budget_bytes {} is below the v3 battery's minimum of {}".format(
                budget_bytes, V3_MIN_BUDGET_BYTES
            )
        )
    return budget_bytes


class DatasetFront:
    """What the checks settled before any halo row is read hand to the row
    phase: the dataset, its manifest and headers, and the values the
    run-scoped headers fix (``n_forests_total`` is None unless they agree)."""

    def __init__(self, directory, manifest, manifest_error, declared, headers, run_scoped_ok):
        self.directory = directory
        self.manifest = manifest
        self.manifest_error = manifest_error
        self.inventory = manifest.inventory if manifest is not None else None
        self.declared = declared
        self.headers = headers
        self.counts = [int(header["_rows"]) for header in headers]
        self.box_sizes = [header["box_size_mpc_h"] for header in headers]
        self.run_scoped_ok = run_scoped_ok
        first = headers[0]
        self.n_forests_total = int(first["n_forests_total"]) if run_scoped_ok else None
        self.max_rank = int(first["max_halo_rank_in_forest"])
        self.source_format = first["source_format"]


def settle_front(
    directory: Path,
    a_list_path,
    a_list: np.ndarray,
    manifest,
    manifest_error,
    verdicts: Verdicts,
    measurements: Dict[str, object],
) -> Optional[DatasetFront]:
    """Settle every check decided before a halo row is read, alike in both
    modes: file-set, object-set, sidecar-object-set, manifest-binding,
    schema-binding, header-values and run-scoped-headers, and record the
    header measurements.

    ``manifest`` is the loaded conversion manifest, or None with
    ``manifest_error`` the reason it did not load (None when no manifest was
    given). Returns None, with every later check SKIP, when the structure or
    a differing ``/schema`` leaves the rows unreadable as one table.
    """
    n_snapshots = len(a_list)
    structural_ok = verdicts.record("file-set", check_file_set(directory, n_snapshots))
    declarations: List[Dict[str, Tuple[str, ...]]] = []
    if structural_ok:
        failures = []
        for snap in range(n_snapshots):
            path = directory / snapshot_h5_name(snap)
            try:
                file_failures, declared = snapshot_structure(path)
            except OSError as exc:
                file_failures, declared = ["unreadable as HDF5 ({})".format(exc)], None
            failures += ["{}: {}".format(path.name, f) for f in file_failures]
            declarations.append(declared or {})
        structural_ok = verdicts.record("object-set", failures)
        try:
            sidecar_failures = sidecar_structure(directory / "forests.h5")
        except OSError as exc:
            sidecar_failures = ["forests.h5: unreadable as HDF5 ({})".format(exc)]
        structural_ok = verdicts.record("sidecar-object-set", sidecar_failures) and structural_ok
    else:
        verdicts.skip(("object-set", "sidecar-object-set"), "file set invalid")

    if manifest is not None:
        verdicts.record(
            "manifest-binding", check_manifest_binding(directory, a_list_path, a_list, manifest)
        )
    elif manifest_error is not None:
        verdicts.record(
            "manifest-binding", ["cannot load the conversion manifest: {}".format(manifest_error)]
        )
    else:
        verdicts.skip(
            ("manifest-binding",), "no manifest given (API mode; unreachable from the CLI)"
        )

    semantic = V3_CHECKS[V3_CHECKS.index("header-values") :]
    if not structural_ok:
        verdicts.skip(
            ("schema-binding",) + semantic, "structural conformance failed; semantics not trusted"
        )
        return None
    verdicts.record("schema-binding", check_schema_binding(declarations, manifest))
    if any(declared != declarations[0] for declared in declarations):
        verdicts.skip(semantic, "files declare different /schema; their columns are not comparable")
        return None

    headers = [read_header(directory / snapshot_h5_name(snap)) for snap in range(n_snapshots)]
    measurements["format_versions"] = sorted({int(h["format_version"]) for h in headers})
    measurements["snapshot_counts"] = [int(h["_rows"]) for h in headers]
    measurements["snapshot_bytes"] = [int(h["_bytes"]) for h in headers]
    measurements["sidecar_bytes"] = int((directory / "forests.h5").stat().st_size)
    header_failures, run_scoped_failures = check_headers(headers, a_list, manifest)
    verdicts.record("header-values", header_failures)
    run_scoped_ok = verdicts.record("run-scoped-headers", run_scoped_failures)
    front = DatasetFront(
        directory, manifest, manifest_error, declarations[0], headers, run_scoped_ok
    )
    first = headers[0]
    measurements.update(
        n_forests_total=int(first["n_forests_total"]),
        header_max_halo_rank_in_forest=front.max_rank,
        header_links_adjacent=int(first["links_adjacent"]),
        source_format=front.source_format,
        column_mapping_sha256=first["column_mapping_sha256"],
    )
    return front


def settle_scan(
    verdicts: Verdicts,
    front: DatasetFront,
    scan: RowScan,
    multiplier: int,
    measurements: Dict[str, object],
    *,
    topology,
    source_keys,
    identity,
) -> None:
    """Settle every check that follows a finished row scan, alike in both
    modes, and record the scan's measurements.

    The mode supplies its cross-row results through three callables, each
    called at most once and in this order: ``topology()`` returns the
    topology-closure failures, the chain-cycles failures and the chain-cycles
    PASS detail, and is called only when every link target is valid;
    ``source_keys()`` returns the source-key-coverage failures; ``identity()``
    returns the identity failures beyond the scan's range checks, and is
    called only when the run-scoped headers agree.
    """
    record = verdicts.record
    record("row-values", scan.row_values.messages())
    record(
        "len-nonnegative",
        scan.lengths.messages(),
        detail="{} Len==0 halo(s)".format(scan.len_zero),
    )
    record(
        "field-finiteness",
        scan.finiteness.messages(),
        detail="{} float field(s)".format(len(scan.floats)),
    )
    record(
        "position-bounds",
        check_position_bounds(scan, front.headers, front.declared),
        detail="every Pos component within [0, box_size_mpc_h]",
    )
    links_ok = record("link-targets", scan.links.messages())
    gapped = scan.stats["gapped_descendants"]
    measured_adjacent = int(gapped == 0)
    if links_ok:
        record(
            "links-adjacent",
            [
                "{}: links_adjacent {} but the dataset measures {} ({} gapped descendant "
                "link(s))".format(
                    snapshot_h5_name(snap), header["links_adjacent"], measured_adjacent, gapped
                )
                for snap, header in enumerate(front.headers)
                if header["links_adjacent"] != measured_adjacent
            ],
        )
        closure, cycles, cycle_detail = topology()
        record("topology-closure", closure)
        record("chain-cycles", cycles, detail=cycle_detail)
    else:
        verdicts.skip(
            ("links-adjacent", "topology-closure", "chain-cycles"),
            "link targets invalid; topology not walked",
        )
    record("source-key-coverage", source_keys())
    if front.run_scoped_ok:
        record("identity", scan.identity_ranges.messages() + identity())
        record(
            "header-bounds", check_header_bounds(front.n_forests_total, front.max_rank, multiplier)
        )
        record(
            "sidecar-content",
            check_sidecar_content(
                front.directory / "forests.h5",
                front.source_format,
                front.n_forests_total,
                front.inventory,
                scan.scan_rows,
            ),
        )
    else:
        verdicts.skip(
            ("identity", "header-bounds", "sidecar-content"), "run-scoped headers inconsistent"
        )
    if front.manifest is not None:
        record("count-conservation", check_count_conservation(front.counts, front.manifest))
    elif front.manifest_error is not None:
        verdicts.skip(
            ("count-conservation",),
            "the conversion manifest failed to load; source counts unavailable",
        )
    else:
        verdicts.skip(("count-conservation",), "no manifest given; source counts unavailable")
    measurements.update(scan.stats)
    measurements.update(
        len_zero=scan.len_zero,
        links_adjacent_measured=measured_adjacent,
        chain_edges=scan.n_chain_edges,
        scan_rows=scan.scan_rows,
    )


def run_battery_v3(
    directory,
    a_list_path,
    manifest_path=None,
    multiplier: int = DEFAULT_MULTIPLIER,
    *,
    budget_bytes: int = DEFAULT_V3_BUDGET_BYTES,
    spill_dir=None,
) -> V3BatteryResult:
    """Run the version 3 producer battery's external-sort mode over one
    dataset directory.

    ``manifest_path`` names the generic conversion's ``manifest.json``; it
    binds the dataset to its conversion (schema, inventory, stage counts,
    registered outputs). Without it (API mode, unreachable from the CLI) the
    binding and conservation checks are SKIP and coverage is judged for
    uniqueness only. ``budget_bytes`` bounds the scan and sort buffers;
    ``spill_dir`` hosts the private sort directory (default: the system
    temporary directory), which is removed on every path.

    The bounded mode, :func:`validate_bounded.run_battery_bounded`, takes the
    same arguments.
    """
    budget_bytes = require_budget(budget_bytes)
    directory = Path(directory)
    if not directory.is_dir():
        raise ConverterError("{}: not a directory".format(directory))
    a_list, _ = load_a_list(a_list_path)
    manifest, manifest_error = None, None
    if manifest_path is not None:
        manifest, manifest_error = try_load_manifest(Path(manifest_path))
    verdicts = Verdicts()
    measurements: Dict[str, object] = {
        "budget_bytes": budget_bytes,
        "battery_mode": "external-sort",
    }
    front = settle_front(
        directory, a_list_path, a_list, manifest, manifest_error, verdicts, measurements
    )
    if front is None:
        return verdicts.result(measurements)

    spill_root = Path(tempfile.mkdtemp(prefix=V3_SPILL_PREFIX, dir=spill_dir))
    scan = None
    try:
        scan = _ExternalSortScan(
            directory,
            front.counts,
            front.box_sizes,
            front.declared,
            front.n_forests_total,
            budget_bytes,
            spill_root,
        )
        scan.run()
        if scan.links:
            # nothing walks the topology: release its sorters before the merges below
            scan.join.close()
            scan.chain.close()

        def topology():
            closure = _v3_topology_closure(scan)
            cycles, rounds = _v3_chain_cycles(scan)
            return closure, cycles, "{} pointer-jumping round(s)".format(rounds)

        settle_scan(
            verdicts,
            front,
            scan,
            multiplier,
            measurements,
            topology=topology,
            source_keys=lambda: _v3_source_keys(scan, front.inventory),
            identity=lambda: _v3_identity(
                scan, front.source_format, front.n_forests_total, front.max_rank, front.inventory
            ),
        )
    except RankSortError as exc:
        raise ConverterError("v3 battery sort failed: {}".format(exc)) from exc
    finally:
        if scan is not None:
            scan.close()
            measurements["peak_resident_bytes"] = int(scan.meter.bytes_peak)
            measurements["peak_spill_bytes"] = int(scan.ledger.peak_bytes)
        shutil.rmtree(str(spill_root), ignore_errors=True)
    if spill_root.exists():
        raise ConverterError(
            "{}: the battery's spill directory could not be removed".format(spill_root)
        )
    return verdicts.result(measurements)


def ordered_outcomes(outcomes: List[Outcome]) -> List[Outcome]:
    """Outcomes in :data:`V3_CHECKS` order, whatever order they were settled
    in."""
    rank = {name: position for position, name in enumerate(V3_CHECKS)}
    return sorted(outcomes, key=lambda outcome: rank[outcome.name])
