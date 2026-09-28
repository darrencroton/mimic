"""Producer validation battery for horizontal-HDF5 format version 3.

Implements the producer side of Validation Requirements in
docs/dev/HORIZONTAL-HDF5-FORMAT-V3-DRAFT.md over one dataset directory:
:func:`run_battery_v3` settles every check named in :data:`V3_CHECKS` and
returns the outcomes together with the residency and spill high-water marks
it measured. Version 2's adjacency, MostBoundID ordering and fixed-unit rules
are replaced only for files that declare ``format_version`` 3, and only here:
the version 2 battery in :mod:`validate` is unchanged by this module, which
reuses its outcome type, failure formatting, per-dataset filter check and
header identity bound. The version dispatch and the CLI stay in
:mod:`validate`.

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

from conversion_manifest import ConversionManifest, sha256_file  # noqa: E402
from ctrees_parser import ConverterError  # noqa: E402
from hdf5_writer import CHUNK_1D, CHUNK_VEC, HEADER_ATTRS, snapshot_h5_name  # noqa: E402
from rank_sort import KeyedSorter, RankSortError, ResidencyMeter, SpillLedger  # noqa: E402
from scatter import load_a_list  # noqa: E402
from source_keys import (  # noqa: E402
    CHAIN_DTYPE,
    CHAIN_KEY,
    SnapshotLayout,
    verify_chains_acyclic,
)
from validate import (  # noqa: E402
    DEFAULT_MULTIPLIER,
    DEFAULT_V3_BUDGET_BYTES,
    RUN_SCOPED_ATTRS,
    V3_FORMAT_VERSION,
    Outcome,
    _examples,
    _filter_failures,
    battery_failed,
    check_header_bounds,
)

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

#: Formats whose forest IS one inventory unit (an L-Halo tree, a forests-HDF5
#: ForestInfo row), so a forest's halos are one contiguous SourceHaloID range
#: in within-forest row order (C1).
_V3_UNIT_FORESTS = ("lhalo_binary", "consistent_trees_hdf5")

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

_INT32_MAX = int(np.iinfo(np.int32).max)


class V3BatteryResult:
    """A v3 battery's named outcomes and what it measured on the way."""

    def __init__(self, outcomes: List[Outcome], measurements: Dict[str, object]):
        self.outcomes = outcomes
        self.measurements = measurements

    @property
    def failed(self) -> bool:
        return battery_failed(self.outcomes)


class _Failures:
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


def _share(budget_bytes: int, name: str) -> int:
    return budget_bytes * _V3_SHARES[name] // _V3_SHARE_DENOMINATOR


# ---------------------------------------------------------------------------
# Structure
# ---------------------------------------------------------------------------


def _v3_check_file_set(directory: Path, n_snapshots: int) -> List[str]:
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
        expected_chunks = CHUNK_VEC
    else:
        if dataset.ndim != 1:
            failures.append("{} shape {} is not [n_halos]".format(name, dataset.shape))
        expected_chunks = CHUNK_1D
    if dataset.chunks != expected_chunks:
        failures.append("{} chunks {} != {}".format(name, dataset.chunks, expected_chunks))
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


def _v3_snapshot_structure(path: Path) -> Tuple[List[str], Optional[Dict[str, Tuple[str, ...]]]]:
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


def _v3_sidecar_structure(path: Path) -> List[str]:
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


def _v3_load_manifest(manifest_path: Path):
    """The generic conversion manifest the dataset claims, loaded with its
    own integrity checks (configuration digest, embedded schema identity)."""
    return ConversionManifest.load(Path(manifest_path).parent)


def _v3_manifest_binding(directory: Path, a_list_path, a_list: np.ndarray, manifest) -> List[str]:
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


def _v3_read_header(path: Path) -> Dict[str, object]:
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


def _v3_check_headers(
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


def _v3_schema_binding(declarations: List[Dict[str, Tuple[str, ...]]], manifest) -> List[str]:
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
# The bounded scan
# ---------------------------------------------------------------------------


class _V3Scan:
    """One pass over every snapshot file in row blocks: every per-row rule,
    the measurements, and the four sorters' input."""

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
        self.directory = directory
        self.counts = np.asarray(counts, dtype=np.int64)
        self.n_snapshots = len(counts)
        self.offsets = np.zeros(self.n_snapshots + 1, dtype=np.int64)
        np.cumsum(self.counts, out=self.offsets[1:])
        self.n_forests_total = n_forests_total
        self.box_sizes = [float(box) for box in box_sizes]
        self.floats = [
            name
            for name, values in sorted(declarations.items())
            if _V3_TYPES.get(values[0], ("", False))[0].startswith("<f")
        ]
        self.position_only_columns = [] if "Pos" in self.floats else ["Pos"]
        self.budget_bytes = budget_bytes
        self.meter = ResidencyMeter()
        self.ledger = SpillLedger()
        self.spill_root = spill_root
        self.row_values = _Failures()
        self.lengths = _Failures()
        self.finiteness = _Failures()
        self.positions = _Failures()
        self.links = _Failures()
        self.identity_ranges = _Failures()
        self.len_zero = 0
        self.links_ok = True
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

        def sorter(dtype, key, share, tag):
            return KeyedSorter(
                dtype,
                key,
                budget_bytes=_share(budget_bytes, share),
                spill_dir=spill_root,
                residency=self.meter,
                ledger=self.ledger,
                tag=tag,
            )

        self._sorter = sorter
        self.join = sorter(_V3_JOIN_DTYPE, _V3_JOIN_KEY, "join", "v3join")
        self.chain = sorter(CHAIN_DTYPE, CHAIN_KEY, "chain", "v3chain")
        self.identity = sorter(_V3_IDENTITY_DTYPE, _V3_IDENTITY_KEY, "identity", "v3identity")
        self.sids = sorter(_V3_SID_DTYPE, _V3_SID_KEY, "sid", "v3sid")
        self.n_chain_edges = 0
        row_bytes = 13 * 8 + 12 * (len(self.floats) + len(self.position_only_columns))
        per_row = (
            4 * row_bytes + 5 * _V3_JOIN_DTYPE.itemsize + 4 * CHAIN_DTYPE.itemsize + 32 + 24 * 8
        )
        self.scan_rows = int(max(1, min(V3_MAX_SCAN_ROWS, _share(budget_bytes, "scan") // per_row)))
        self.scan_bytes = self.scan_rows * per_row

    def close(self) -> None:
        for sorter in (self.join, self.chain, self.identity, self.sids):
            sorter.close()

    # ---- the pass --------------------------------------------------------

    def run(self) -> None:
        self.meter.acquire(self.scan_bytes)
        try:
            for snap in range(self.n_snapshots):
                self._scan_snapshot(snap)
        finally:
            self.meter.release(self.scan_bytes)
        for sorter in (self.join, self.chain, self.identity, self.sids):
            sorter.seal()

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

        # -- positions inside the header's box (see _v3_position_bounds) ----
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
            self._emit_topology(snap, rows, targets, first_fof, next_fof, forest)
        return int(sid[-1])

    def _emit_topology(self, snap, rows, targets, first_fof, next_fof, forest) -> None:
        """Join requests and pointer-jumping edges for one block, in global
        positions. Only reached while every link seen so far is in range."""
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


def _v3_position_bounds(
    scan: _V3Scan, headers: List[Dict[str, object]], declared: Dict[str, Tuple[str, ...]]
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


def _describe_gp(offsets: np.ndarray, gp: int) -> str:
    position = int(np.searchsorted(offsets, gp, side="right")) - 1
    return "{} row {}".format(snapshot_h5_name(position), int(gp - offsets[position]))


def _link_examples(offsets: np.ndarray, owners, targets, mask) -> List[str]:
    """Up to five ``owner -> target`` links, located by file and row."""
    return [
        "{} -> {}".format(_describe_gp(offsets, owner), _describe_gp(offsets, target))
        for owner, target in zip(owners[mask][:5], targets[mask][:5])
    ]


def _v3_topology_closure(scan: _V3Scan) -> List[str]:
    """Consume the sorted topology join: every link into a halo is judged
    against that halo's own record, and every halo's in-links are counted.

    A group is a halo's own record followed by the links into it. The last
    group of a block may continue into the next, so its record and running
    counts are carried; a group is judged once the next own-record appears.
    """
    failures = _Failures()
    offsets = scan.offsets
    carry = None  # (key, a, b, forest, progenitor_in, fof_in)

    def judge(key, a, b, prog_in, fof_in):
        expected_prog = (a != -1).astype(np.int64)
        expected_fof = (b != key).astype(np.int64)
        bad = prog_in != expected_prog
        failures.add(
            "halo(s) not reached exactly once by their descendant's progenitor chain "
            "(reached k times, expected once iff the halo has a Descendant)",
            int(bad.sum()),
            [
                "{} (k={})".format(_describe_gp(offsets, g), int(k))
                for g, k in zip(key[bad][:5], prog_in[bad][:5])
            ],
        )
        bad = fof_in != expected_fof
        failures.add(
            "halo(s) not reached exactly once by their FoF chain (reached k times, expected "
            "once iff the halo is not its group's central)",
            int(bad.sum()),
            [
                "{} (k={})".format(_describe_gp(offsets, g), int(k))
                for g, k in zip(key[bad][:5], fof_in[bad][:5])
            ],
        )

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
                return _link_examples(offsets, owners, targets, mask)

            orphan = g_key != r_key
            failures.add("link(s) naming a halo that has no row", int(orphan.sum()), where(orphan))
            matched = ~orphan
            prog = matched & ((r_tag == 1) | (r_tag == 2))
            bad = prog & (r_tag == 1) & (g_a != r_a)
            failures.add(
                "FirstProgenitor(s) whose target's Descendant is not the owner",
                int(bad.sum()),
                where(bad),
            )
            bad = prog & (r_tag == 2) & (g_a != r_a)
            failures.add(
                "NextProgenitor(s) whose target names a different descendant than the owner",
                int(bad.sum()),
                where(bad),
            )
            bad = matched & (r_tag == 3) & (g_b != g_key)
            failures.add(
                "FirstHaloInFOFgroup target(s) that are not self-referencing centrals",
                int(bad.sum()),
                where(bad),
            )
            fof = matched & (r_tag == 4)
            bad = fof & (g_b != r_a)
            failures.add(
                "NextHaloInFOFgroup target(s) in a different FoF group than the owner",
                int(bad.sum()),
                where(bad),
            )
            bad = matched & (g_forest != block["forest"][requests])
            failures.add("link(s) crossing forests", int(bad.sum()), where(bad))

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


def _v3_chain_cycles(scan: _V3Scan) -> Tuple[List[str], int]:
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
    scan: _V3Scan, source_format: str, n_forests_total: int, max_rank: int, unsampled: bool
) -> List[str]:
    """Consume the (ForestIndex, HaloRankInForest, SourceHaloID) ordering.

    Every forest's ranks are exactly 0 .. count-1 (dense and unique). For
    Consistent-Trees ASCII every forest holds halos, so ForestIndex is dense
    over [0, n_forests_total). For the unit-forest formats a forest may be a
    zero-halo tree, but its halos are one contiguous SourceHaloID range in rank
    order, so in (ForestIndex, rank) order the ids of an unsampled conversion
    are exactly 1, 2, 3, ...
    """
    failures = _Failures()
    carry_forest, carry_rank, position = -1, -1, 0
    measured_max = -1
    unit_forests = source_format in _V3_UNIT_FORESTS
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
            failures.add(
                "(ForestIndex, HaloRankInForest) pair(s) breaking per-forest rank density or "
                "uniqueness",
                int(bad.sum()),
                [
                    "(ForestIndex={}, rank={})".format(f, r)
                    for f, r in zip(forest[bad][:5], rank[bad][:5])
                ],
            )
            if source_format == "consistent_trees_ascii":
                bad = ~same & (forest != prev_forest + 1)
                failures.add(
                    "ForestIndex value(s) skipping a forest (ASCII forests all hold halos)",
                    int(bad.sum()),
                    [
                        "ForestIndex={} after {}".format(f, p)
                        for f, p in zip(forest[bad][:5], prev_forest[bad][:5])
                    ],
                )
            if unit_forests and unsampled:
                expected = position + 1 + np.arange(size, dtype=np.int64)
                bad = sid != expected
                failures.add(
                    "halo(s) whose SourceHaloID is not its (ForestIndex, rank) position in the "
                    "source order",
                    int(bad.sum()),
                    [
                        "(ForestIndex={}, rank={}) has SourceHaloID {}, expected {}".format(
                            f, r, s, e
                        )
                        for f, r, s, e in zip(
                            forest[bad][:5], rank[bad][:5], sid[bad][:5], expected[bad][:5]
                        )
                    ],
                )
            measured_max = max(measured_max, int(rank.max()))
            carry_forest, carry_rank = int(forest[-1]), int(rank[-1])
            position += size
    finally:
        blocks.close()
    messages = failures.messages()
    if source_format == "consistent_trees_ascii" and carry_forest != n_forests_total - 1:
        messages.append(
            "ForestIndex values end at {} but n_forests_total is {}".format(
                carry_forest, n_forests_total
            )
        )
    if measured_max != max_rank:
        messages.append(
            "measured max HaloRankInForest {} != header max_halo_rank_in_forest {}".format(
                measured_max, max_rank
            )
        )
    return messages


def _v3_source_keys(scan: _V3Scan, inventory: Optional[Mapping]) -> List[str]:
    """Consume the SourceHaloID ordering: globally unique, and -- against the
    conversion's inventory -- within [1, total_halos] and exactly as many as
    the inventory selects, which for an unsampled inventory is exactly
    {1 .. total_halos}."""
    failures = _Failures()
    previous = 0
    position = 0
    total = None if inventory is None else int(inventory["total_halos"])
    unsampled = inventory is not None and int(inventory["selected_halos"]) == total
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
            failures.add("duplicated SourceHaloID value(s)", int(bad.sum()), sid[bad].tolist())
            if total is not None:
                bad = (sid < 1) | (sid > total)
                failures.add(
                    "SourceHaloID value(s) outside the inventory's [1, {}]".format(total),
                    int(bad.sum()),
                    sid[bad].tolist(),
                )
            if unsampled:
                expected = position + 1 + np.arange(sid.size, dtype=np.int64)
                bad = sid != expected
                failures.add(
                    "gap(s) in SourceHaloID coverage of the unsampled inventory",
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
    messages = failures.messages()
    if inventory is not None and position != int(inventory["selected_halos"]):
        messages.append(
            "{} SourceHaloID value(s) emitted; the inventory selects {}".format(
                position, inventory["selected_halos"]
            )
        )
    return messages


# ---------------------------------------------------------------------------
# Sidecar content and conservation
# ---------------------------------------------------------------------------


def _v3_sidecar_content(
    path: Path, source_format: str, n_forests_total: int, inventory, block_rows: int
) -> List[str]:
    failures = _Failures()
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
    if inventory is not None and source_format in _V3_UNIT_FORESTS:
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


def _v3_count_conservation(counts: List[int], manifest) -> List[str]:
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


def _require_budget(budget_bytes) -> int:
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


def run_battery_v3(
    directory,
    a_list_path,
    manifest_path=None,
    multiplier: int = DEFAULT_MULTIPLIER,
    *,
    budget_bytes: int = DEFAULT_V3_BUDGET_BYTES,
    spill_dir=None,
) -> V3BatteryResult:
    """Run the version 3 producer battery over one dataset directory.

    ``manifest_path`` names the generic conversion's ``manifest.json``; it
    binds the dataset to its conversion (schema, inventory, stage counts,
    registered outputs). Without it (API mode, unreachable from the CLI) the
    binding and conservation checks are SKIP and coverage is judged for
    uniqueness only. ``budget_bytes`` bounds the scan and sort buffers;
    ``spill_dir`` hosts the private sort directory (default: the system
    temporary directory), which is removed on every path.
    """
    budget_bytes = _require_budget(budget_bytes)
    directory = Path(directory)
    if not directory.is_dir():
        raise ConverterError("{}: not a directory".format(directory))
    a_list, _ = load_a_list(a_list_path)
    n_snapshots = len(a_list)
    outcomes: List[Outcome] = []
    measurements: Dict[str, object] = {"budget_bytes": budget_bytes}

    def record(name: str, failures: List[str], detail: str = "") -> bool:
        if failures:
            outcomes.append(Outcome(name, "FAIL", "; ".join(failures)))
            return False
        outcomes.append(Outcome(name, "PASS", detail))
        return True

    def skip(names, reason: str) -> None:
        for name in names:
            outcomes.append(Outcome(name, "SKIP", reason))

    structural_ok = record("file-set", _v3_check_file_set(directory, n_snapshots))
    declarations: List[Dict[str, Tuple[str, ...]]] = []
    if structural_ok:
        failures = []
        for snap in range(n_snapshots):
            path = directory / snapshot_h5_name(snap)
            try:
                file_failures, declared = _v3_snapshot_structure(path)
            except OSError as exc:
                file_failures, declared = ["unreadable as HDF5 ({})".format(exc)], None
            failures += ["{}: {}".format(path.name, f) for f in file_failures]
            declarations.append(declared or {})
        structural_ok = record("object-set", failures)
        try:
            sidecar_failures = _v3_sidecar_structure(directory / "forests.h5")
        except OSError as exc:
            sidecar_failures = ["forests.h5: unreadable as HDF5 ({})".format(exc)]
        structural_ok = record("sidecar-object-set", sidecar_failures) and structural_ok
    else:
        skip(("object-set", "sidecar-object-set"), "file set invalid")

    manifest = None
    if manifest_path is not None:
        try:
            manifest = _v3_load_manifest(Path(manifest_path))
        except (ConverterError, OSError, ValueError, KeyError) as exc:
            record("manifest-binding", ["cannot load the conversion manifest: {}".format(exc)])
        else:
            record(
                "manifest-binding", _v3_manifest_binding(directory, a_list_path, a_list, manifest)
            )
    else:
        skip(("manifest-binding",), "no manifest given (API mode; unreachable from the CLI)")

    semantic = V3_CHECKS[V3_CHECKS.index("header-values") :]
    if not structural_ok:
        skip(("schema-binding",) + semantic, "structural conformance failed; semantics not trusted")
        return V3BatteryResult(_ordered(outcomes), measurements)
    record("schema-binding", _v3_schema_binding(declarations, manifest))
    if any(declared != declarations[0] for declared in declarations):
        skip(semantic, "files declare different /schema; their columns are not comparable")
        return V3BatteryResult(_ordered(outcomes), measurements)

    headers = [_v3_read_header(directory / snapshot_h5_name(snap)) for snap in range(n_snapshots)]
    counts = [int(header["_rows"]) for header in headers]
    measurements["format_versions"] = sorted({int(h["format_version"]) for h in headers})
    measurements["snapshot_counts"] = counts
    measurements["snapshot_bytes"] = [int(h["_bytes"]) for h in headers]
    measurements["sidecar_bytes"] = int((directory / "forests.h5").stat().st_size)
    header_failures, run_scoped_failures = _v3_check_headers(headers, a_list, manifest)
    record("header-values", header_failures)
    run_scoped_ok = record("run-scoped-headers", run_scoped_failures)
    first = headers[0]
    n_forests_total = int(first["n_forests_total"]) if run_scoped_ok else None
    max_rank = int(first["max_halo_rank_in_forest"])
    source_format = first["source_format"]
    measurements.update(
        n_forests_total=int(first["n_forests_total"]),
        header_max_halo_rank_in_forest=max_rank,
        header_links_adjacent=int(first["links_adjacent"]),
        source_format=source_format,
        column_mapping_sha256=first["column_mapping_sha256"],
    )

    spill_root = Path(tempfile.mkdtemp(prefix=V3_SPILL_PREFIX, dir=spill_dir))
    scan = None
    try:
        scan = _V3Scan(
            directory,
            counts,
            [header["box_size_mpc_h"] for header in headers],
            declarations[0],
            n_forests_total,
            budget_bytes,
            spill_root,
        )
        scan.run()
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
            _v3_position_bounds(scan, headers, declarations[0]),
            detail="every Pos component within [0, box_size_mpc_h]",
        )
        links_ok = record("link-targets", scan.links.messages())
        measured_adjacent = int(scan.stats["gapped_descendants"] == 0)
        adjacency = [
            "{}: links_adjacent {} but the dataset measures {} ({} gapped descendant link(s))".format(
                snapshot_h5_name(snap),
                header["links_adjacent"],
                measured_adjacent,
                scan.stats["gapped_descendants"],
            )
            for snap, header in enumerate(headers)
            if header["links_adjacent"] != measured_adjacent
        ]
        if links_ok:
            record("links-adjacent", adjacency)
            record("topology-closure", _v3_topology_closure(scan))
            cycle_failures, rounds = _v3_chain_cycles(scan)
            record(
                "chain-cycles", cycle_failures, detail="{} pointer-jumping round(s)".format(rounds)
            )
        else:
            skip(
                ("links-adjacent", "topology-closure", "chain-cycles"),
                "link targets invalid; topology not walked",
            )
            scan.join.close()
            scan.chain.close()
        inventory = manifest.inventory if manifest is not None else None
        record("source-key-coverage", _v3_source_keys(scan, inventory))
        if run_scoped_ok:
            unsampled = inventory is not None and int(inventory["selected_halos"]) == int(
                inventory["total_halos"]
            )
            record(
                "identity",
                scan.identity_ranges.messages()
                + _v3_identity(scan, source_format, n_forests_total, max_rank, unsampled),
            )
            record("header-bounds", check_header_bounds(n_forests_total, max_rank, multiplier))
            record(
                "sidecar-content",
                _v3_sidecar_content(
                    directory / "forests.h5",
                    source_format,
                    n_forests_total,
                    inventory,
                    scan.scan_rows,
                ),
            )
        else:
            skip(
                ("identity", "header-bounds", "sidecar-content"), "run-scoped headers inconsistent"
            )
        if manifest is not None:
            record("count-conservation", _v3_count_conservation(counts, manifest))
        else:
            skip(("count-conservation",), "no manifest given; source counts unavailable")
        measurements.update(scan.stats)
        measurements.update(
            len_zero=scan.len_zero,
            links_adjacent_measured=measured_adjacent,
            chain_edges=scan.n_chain_edges,
            scan_rows=scan.scan_rows,
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
    return V3BatteryResult(_ordered(outcomes), measurements)


def _ordered(outcomes: List[Outcome]) -> List[Outcome]:
    """Outcomes in :data:`V3_CHECKS` order, whatever order they were settled
    in."""
    rank = {name: position for position, name in enumerate(V3_CHECKS)}
    return sorted(outcomes, key=lambda outcome: rank[outcome.name])
