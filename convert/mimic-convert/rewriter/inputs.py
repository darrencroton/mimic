"""Every argument check and refusal the rewriter makes before it writes.

:func:`prepare` turns a :class:`RewriteRequest` into a :class:`Prepared` run:
it opens the input, binds the a_list, the physical metadata, the schema, the
index files and the conversion's tree-file order to it, and refuses -- with
a message naming the offending value -- anything the passes could not carry
through faithfully. Nothing here writes.

The refusals, in the order they are made:

- paths: the output and scratch directories are explicit and lie outside the
  input, and neither contains the other;
- the operation's options: ``migrate`` needs ``--column-map``; ``noop`` and
  ``cut`` refuse it (``/schema`` is carried through) and
  ``--conversion-manifest``; ``cut`` needs ``--cut-table``, which the other
  two refuse, and takes ``--cut-table-record``;
- the input: ``format_version`` 2 for ``migrate`` and 3 for ``noop`` and
  ``cut`` (every other version is refused by the reader), ``links_adjacent``
  1, and for a version 3 input ``source_format`` ``consistent_trees_ascii``;
- the a_list: exactly the input's scale factors;
- the simulation metadata: exactly the input's five physical header values;
- the schema: for ``migrate`` the profile's, which must be a
  ``consistent_trees_ascii`` profile; for ``noop`` and ``cut`` the shipped
  default profile, whose digest must be the input's ``column_mapping_sha256``,
  since the manifest the battery binds to embeds the schema and no other
  profile is given; every input slab's ``/schema`` must then declare exactly
  it;
- the columns: every ``/halos`` column the output table needs, at the table's
  dtype and shape;
- the multiplier: it must encode the output's identity bounds -- the
  input's for ``migrate`` and ``noop``; for ``cut`` the installed table's,
  checked when the table is installed (``rewriter.cut``), since a cut lowers
  ``max_halo_rank_in_forest`` and raises ``n_forests_total``;
- the index files: both present and consistent with each other, listing
  exactly the input's forests in the sidecar's order;
- the tree-file order: from the version 2 conversion manifest's ordered
  source list (``migrate``), required when ``locations.dat`` names more than
  one file, the one file itself otherwise, and refused when ambiguous or
  naming another file; for ``noop`` and ``cut`` the mapping the input sidecar
  itself implies, refused when inconsistent;
- the cut table's record (``cut``, when given): a readable JSON record whose
  ``table.md5`` is the given table's md5 and whose ``index_files`` md5 values
  are the given index files', each refused otherwise; the record's dataset
  identity is carried into the manifest as lineage, not compared, since a
  migration changes the identity while preserving the forest enumeration.
  The table's own invariants are checked once pass 0 has given the trees'
  halo totals (``rewriter.cut``), still before any write, as are a piece held
  in one file whose ``FileID`` the input sidecar gives no ordinal for (the
  cut cannot infer the converter's file order) and the multiplier against
  the output's bounds.

Two further refusals of a ``cut`` are made per slab in pass 1
(``rewriter.passes.rank_slab``), after the workdir exists: a halo whose FoF
central lies in another forest of the input (a version 3 invariant hosts are
resolved under), and stored progenitor chains that are not the reference
insertion loop's over the input's own keys (the recomputed chains and the
progenitor-order counts rest on it; every converter and rewriter output
satisfies it).
"""

import json
import os
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, List, Mapping, Optional, Tuple

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from census.cut_table import md5_file  # noqa: E402
from census.trees import ParsedCounts, load_parsed_counts  # noqa: E402
from column_schema import (  # noqa: E402
    CanonicalSchema,
    PayloadField,
    build_schema,
    load_column_map,
)
from conversion_manifest import (  # noqa: E402
    canonical_sha256,
    dtype_descriptor,
    pin_dependency,
    schema_record,
    sha256_file,
)
from errors import ConverterError  # noqa: E402
from hdf5_writer_v3 import (  # noqa: E402
    V3_SCHEMA_ATTRS,
    V3_SIDECAR_DATASETS,
    load_header_metadata,
    v3_halo_datasets,
)
from horizontal_dataset import DEFAULT_BLOCK_ROWS, HorizontalDataset  # noqa: E402
from rewriter import (  # noqa: E402
    OPERATION_CUT,
    OPERATION_MIGRATE,
    OPERATION_NOOP,
    OPERATIONS,
    REWRITER_VERSION,
    SOURCE_FORMAT,
)
from scatter import load_a_list  # noqa: E402
from source_index import ForestTable, SourceIndex  # noqa: E402
from transpose import output_dtype  # noqa: E402
from validate_v3 import check_header_bounds  # noqa: E402

#: The shipped default profile of the one accepted source format.
DEFAULT_PROFILE = (
    Path(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    / "profiles"
    / "{}.yaml".format(SOURCE_FORMAT)
)

#: The physical header attributes the simulation metadata supplies.
PHYSICAL_ATTRS = (
    "box_size_mpc_h",
    "particle_mass_msun_h",
    "omega_matter",
    "omega_lambda",
    "hubble_h",
)

#: Dependency roles, as the manifest pins them.
ROLE_INPUT = "input"
ROLE_A_LIST = "a_list"
ROLE_SIMULATION_INFO = "simulation_info"
ROLE_COLUMN_MAP = "column_map"
ROLE_FORESTS_LIST = "forests_list"
ROLE_LOCATIONS = "locations"
ROLE_CONVERSION_MANIFEST = "conversion_manifest"
ROLE_CUT_TABLE = "cut_table"
ROLE_CUT_TABLE_RECORD = "cut_table_record"

#: ``/halos`` columns of a version 2 input that the output table reads under
#: another width (int32 links become int64).
_V2_LINKS = (
    "Descendant",
    "FirstProgenitor",
    "NextProgenitor",
    "FirstHaloInFOFgroup",
    "NextHaloInFOFgroup",
)


def _examples(values, limit: int = 5) -> list:
    return [v for v in list(values)[:limit]]


@dataclass(frozen=True)
class RewriteRequest:
    """One invocation's explicit arguments, every path as given."""

    operation: str
    input_dir: Path
    output_dir: Path
    scratch_dir: Path
    simulation_info: Path
    a_list: Path
    forests_list: Path
    locations: Path
    column_map: Optional[Path]
    conversion_manifest: Optional[Path]
    multiplier: int
    resume: bool = False
    block_rows: int = DEFAULT_BLOCK_ROWS
    #: the cut forest table (``cut`` only) and, optionally, its JSON record
    cut_table: Optional[Path] = None
    cut_table_record: Optional[Path] = None


@dataclass
class Prepared:
    """A request every refusal has passed, with everything the passes read."""

    request: RewriteRequest
    dataset: HorizontalDataset
    schema: CanonicalSchema
    declarations: Tuple[PayloadField, ...]
    table: Dict[str, Tuple[np.dtype, bool]]
    metadata: Dict[str, float]
    scale_factors: Tuple[float, ...]
    index: Optional[SourceIndex]
    forest_table: Optional[ForestTable]
    #: ``locations.dat`` ``FileID`` -> the conversion's tree-file ordinal
    #: (-1 where a ``noop`` input's sidecar does not determine it).
    file_ordinals: Dict[int, int]
    #: tree-file names in the conversion's order; empty when not recorded.
    tree_file_order: List[str]
    parsed_counts: Optional[ParsedCounts]
    #: the output sidecar's three columns, in ``ForestIndex`` order.
    sidecar: Optional[Dict[str, np.ndarray]]
    pins: List[Dict[str, object]]
    configuration: Dict[str, object]
    #: ``cut``: the table's md5 and the header of its record (``None`` without one)
    cut_table_md5: Optional[str] = None
    cut_record: Optional[Dict[str, object]] = None
    #: ``cut``: the output's forest count once the table is installed
    n_forests_out: Optional[int] = None

    @property
    def operation(self) -> str:
        return self.request.operation

    @property
    def n_snapshots(self) -> int:
        return len(self.scale_factors)

    @property
    def n_forests(self) -> int:
        """The output's forest count: the input's, or the cut table's."""
        if self.n_forests_out is not None:
            return int(self.n_forests_out)
        return self.dataset.n_forests_total

    def release_tables(self) -> None:
        """Drop the index, its forest table and the sidecar columns once pass
        0, the sidecar and the inventory no longer need them."""
        self.index = None
        self.forest_table = None
        self.sidecar = None


# ---------------------------------------------------------------------------
# Paths and options
# ---------------------------------------------------------------------------


def _resolved(path, what: str) -> Path:
    if path is None:
        raise ConverterError("{} is required".format(what))
    return Path(os.path.realpath(os.fspath(path)))


def _inside(inner: Path, outer: Path) -> bool:
    return inner == outer or outer in inner.parents


def check_paths(request: RewriteRequest) -> Tuple[Path, Path, Path]:
    """The resolved input, output and scratch directories: all explicit, the
    output and scratch outside the input and apart from each other."""
    input_dir = _resolved(request.input_dir, "--input")
    output_dir = _resolved(request.output_dir, "--output")
    scratch_dir = _resolved(request.scratch_dir, "--scratch")
    if not input_dir.is_dir():
        raise ConverterError("--input {}: not a directory".format(input_dir))
    for name, directory in (("--output", output_dir), ("--scratch", scratch_dir)):
        if _inside(directory, input_dir) or _inside(input_dir, directory):
            raise ConverterError(
                "{} {} and --input {} contain one another; the output and scratch must lie "
                "outside the input".format(name, directory, input_dir)
            )
    if _inside(output_dir, scratch_dir) or _inside(scratch_dir, output_dir):
        raise ConverterError(
            "--output {} and --scratch {} contain one another".format(output_dir, scratch_dir)
        )
    return input_dir, output_dir, scratch_dir


def check_options(request: RewriteRequest) -> None:
    """The option set each operation accepts."""
    if request.operation not in OPERATIONS:
        raise ConverterError(
            "operation {!r} is not one of {}".format(request.operation, list(OPERATIONS))
        )
    if request.operation == OPERATION_MIGRATE and request.column_map is None:
        raise ConverterError(
            "migrate needs --column-map (the package's consistent_trees_ascii profile), from "
            "which the version 3 /schema is built"
        )
    if request.operation in (OPERATION_NOOP, OPERATION_CUT):
        if request.column_map is not None:
            raise ConverterError(
                "{} refuses --column-map {}: the input's /schema is carried through".format(
                    request.operation, request.column_map
                )
            )
        if request.conversion_manifest is not None:
            raise ConverterError(
                "{} refuses --conversion-manifest {}: the sidecar ordinals follow the input "
                "sidecar".format(request.operation, request.conversion_manifest)
            )
    if request.operation == OPERATION_CUT:
        if request.cut_table is None:
            raise ConverterError(
                "cut needs --cut-table (the cut forest table, forests.list-shaped)"
            )
    else:
        for name, value in (
            ("--cut-table", request.cut_table),
            ("--cut-table-record", request.cut_table_record),
        ):
            if value is not None:
                raise ConverterError(
                    "{} refuses {} {}: only cut takes a cut table".format(
                        request.operation, name, value
                    )
                )
    if isinstance(request.multiplier, bool) or not isinstance(
        request.multiplier, (int, np.integer)
    ):
        raise ConverterError("--multiplier must be an integer, got {!r}".format(request.multiplier))
    if int(request.multiplier) < 1:
        raise ConverterError("--multiplier must be positive, got {}".format(request.multiplier))
    block_rows = request.block_rows
    if (
        isinstance(block_rows, bool)
        or not isinstance(block_rows, (int, np.integer))
        or block_rows < 1
    ):
        raise ConverterError("block_rows must be a positive integer, got {!r}".format(block_rows))


# ---------------------------------------------------------------------------
# The input dataset
# ---------------------------------------------------------------------------


def check_dataset(request: RewriteRequest, dataset: HorizontalDataset) -> None:
    """The input is the operation's version, adjacent, and of this format."""
    wanted = 2 if request.operation == OPERATION_MIGRATE else 3
    if dataset.format_version != wanted:
        raise ConverterError(
            "{}: format_version {} but {} rewrites a version {} input".format(
                dataset.directory, dataset.format_version, request.operation, wanted
            )
        )
    if dataset.links_adjacent != 1:
        raise ConverterError(
            "{}: links_adjacent {}; only a dataset whose every Descendant link is adjacent "
            "(links_adjacent 1) is rewritten -- the gapped case is out of scope".format(
                dataset.directory, dataset.links_adjacent
            )
        )
    if dataset.format_version >= 3 and dataset.source_format != SOURCE_FORMAT:
        raise ConverterError(
            "{}: source_format {!r}; the rewriter handles {!r} datasets only".format(
                dataset.directory, dataset.source_format, SOURCE_FORMAT
            )
        )


def check_a_list(request: RewriteRequest, dataset: HorizontalDataset) -> Tuple[float, ...]:
    """The a_list must be exactly the input's scale factors."""
    values, _md5 = load_a_list(request.a_list)
    scales = [float(v) for v in values]
    recorded = [float(v) for v in dataset.scale_factors]
    if scales != recorded:
        differing = [
            snap for snap, (given, held) in enumerate(zip(scales, recorded)) if given != held
        ]
        raise ConverterError(
            "--a-list {}: {} scale factor(s) but the input holds {} snapshots; differing at "
            "snapshots {}".format(
                request.a_list,
                len(scales),
                len(recorded),
                _examples(differing) if differing else "none, lengths differ",
            )
        )
    return tuple(scales)


def check_metadata(request: RewriteRequest, dataset: HorizontalDataset) -> Dict[str, float]:
    """The simulation metadata must give exactly every input slab's physical
    headers (the reader's run-scoped checks do not cover them)."""
    metadata = load_header_metadata(request.simulation_info)
    for snap in dataset.snapshots:
        header = dataset.header(snap)
        for name in PHYSICAL_ATTRS:
            if header.get(name) is None:
                raise ConverterError(
                    "{}: header without {}".format(dataset.snapshot_path(snap), name)
                )
            if float(header[name]) != float(metadata[name]):
                raise ConverterError(
                    "--simulation-info {}: {} is {!r} but {} records {!r}; refusing to write a "
                    "header that differs from the input's".format(
                        request.simulation_info,
                        name,
                        metadata[name],
                        dataset.snapshot_path(snap),
                        header[name],
                    )
                )
    return metadata


def check_multiplier(request: RewriteRequest, n_forests: int, max_rank: int) -> int:
    failures = check_header_bounds(n_forests, max_rank, int(request.multiplier))
    if failures:
        raise ConverterError(
            "--multiplier {}: cannot encode the output's bounds: {}".format(
                request.multiplier, "; ".join(failures)
            )
        )
    return int(request.multiplier)


# ---------------------------------------------------------------------------
# The schema
# ---------------------------------------------------------------------------


def _declared(declarations) -> Dict[str, Dict[str, str]]:
    return {
        field.name: {key: str(getattr(field, key)) for key in V3_SCHEMA_ATTRS}
        for field in declarations
    }


def load_schema(request: RewriteRequest, dataset: HorizontalDataset) -> CanonicalSchema:
    """The output's schema: the profile's for ``migrate``, the shipped default
    profile's for ``noop`` (whose digest must be the input's)."""
    if request.operation == OPERATION_MIGRATE:
        schema = build_schema(load_column_map(request.column_map))
        if schema.source_format != SOURCE_FORMAT:
            raise ConverterError(
                "--column-map {}: a {!r} profile; migrate needs a {!r} profile".format(
                    request.column_map, schema.source_format, SOURCE_FORMAT
                )
            )
        return schema
    schema = build_schema(load_column_map(DEFAULT_PROFILE))
    for snap in dataset.snapshots:
        recorded = dataset.header(snap).get("column_mapping_sha256")
        if schema.digest != recorded:
            raise ConverterError(
                "{}: column_mapping_sha256 {} is not the shipped {} profile's ({}); the "
                "manifest noop writes embeds that profile's schema, so an input converted with "
                "another profile cannot be bound -- its own profile is not given (noop refuses "
                "--column-map)".format(
                    dataset.snapshot_path(snap), recorded, SOURCE_FORMAT, schema.digest
                )
            )
    expected = _declared(schema.output_field_declarations())
    for snap in dataset.snapshots:
        declared = dataset.schema_declarations(snap)
        if declared != expected:
            raise ConverterError(
                "{}: /schema differs from the profile's declarations (missing {}, extra {}, "
                "changed {})".format(
                    dataset.snapshot_path(snap),
                    sorted(set(expected) - set(declared)),
                    sorted(set(declared) - set(expected)),
                    sorted(
                        name
                        for name in set(expected) & set(declared)
                        if expected[name] != declared[name]
                    ),
                )
            )
    return schema


def check_columns(dataset: HorizontalDataset, table: Mapping[str, Tuple[np.dtype, bool]]) -> None:
    """Every slab holds the columns the output reads, at the table's dtype
    and row shape (a version 2 input's five links at int32 instead)."""
    identity_from_input = dataset.format_version >= 3
    for snap in dataset.snapshots:
        held = dataset.halo_dtypes(snap)
        for name, (dtype, is_vec) in table.items():
            if name.endswith("Snapshot") and dataset.format_version < 3:
                continue  # version 2 implies the target snapshots
            if name in ("SourceHaloID",) and not identity_from_input:
                continue  # computed from the version 2 columns
            if name not in held:
                raise ConverterError(
                    "{}: no /halos/{} column, which the output needs".format(
                        dataset.snapshot_path(snap), name
                    )
                )
            held_dtype, shape = held[name]
            wanted = np.dtype(dtype)
            if name in _V2_LINKS and dataset.format_version < 3:
                wanted = np.dtype("<i4")
            if np.dtype(held_dtype).newbyteorder("<").str != wanted.str:
                raise ConverterError(
                    "{}: /halos/{} is {}, the output table needs {}".format(
                        dataset.snapshot_path(snap), name, held_dtype.str, wanted.str
                    )
                )
            if shape != ((3,) if is_vec else ()):
                raise ConverterError(
                    "{}: /halos/{} rows have shape {}, the output table needs {}".format(
                        dataset.snapshot_path(snap), name, shape, (3,) if is_vec else ()
                    )
                )


# ---------------------------------------------------------------------------
# The index files and the tree-file order
# ---------------------------------------------------------------------------


def load_index(request: RewriteRequest) -> SourceIndex:
    for name, path in (
        ("--forests-list", request.forests_list),
        ("--locations", request.locations),
    ):
        if path is None or not Path(path).is_file():
            raise ConverterError("{} {}: no such file".format(name, path))
    try:
        return SourceIndex.load(request.forests_list, request.locations)
    except OverflowError as exc:
        raise ConverterError(
            "{} or {}: an id outside int64 ({})".format(
                request.forests_list, request.locations, exc
            )
        ) from exc


def check_forest_ids(
    dataset: HorizontalDataset, table: ForestTable, forest_ids: np.ndarray
) -> None:
    """The index files list exactly the input's forests, in its sidecar's
    (ascending) order: ``ForestIndex`` f is the f-th forest id of the index."""
    if table.forest_ids.size != forest_ids.size or not np.array_equal(table.forest_ids, forest_ids):
        only_index = np.setdiff1d(table.forest_ids, forest_ids)
        only_sidecar = np.setdiff1d(forest_ids, table.forest_ids)
        raise ConverterError(
            "index files inconsistent with {}: they list {} forest(s), the sidecar {}; {} only "
            "in the index (e.g. {}), {} only in the sidecar (e.g. {})".format(
                dataset.sidecar_path,
                table.forest_ids.size,
                forest_ids.size,
                only_index.size,
                _examples(only_index.tolist()),
                only_sidecar.size,
                _examples(only_sidecar.tolist()),
            )
        )


def _basenames(index: SourceIndex) -> Dict[int, str]:
    names = {file_id: os.path.basename(name) for file_id, name in index.filenames.items()}
    seen: Dict[str, int] = {}
    for file_id, name in sorted(names.items()):
        if name in seen:
            raise ConverterError(
                "locations.dat: FileID {} and {} both name {!r}; the tree-file mapping is "
                "ambiguous".format(seen[name], file_id, name)
            )
        seen[name] = file_id
    return names


def tree_file_ordinals(
    index: SourceIndex, conversion_manifest: Optional[Path]
) -> Tuple[Dict[int, int], List[str]]:
    """``locations.dat`` ``FileID`` -> the conversion's tree-file ordinal, by
    file name, from the version 2 conversion manifest's ordered
    ``provenance.source_files``; for a one-file catalogue without a manifest
    the file itself is ordinal 0. Returns the mapping and the ordered names.

    Raises:
        ConverterError: when ``locations.dat`` names several files and no
            manifest is given, when the manifest's list is missing, empty,
            repeats a name, or names a file the index does not (or omits one
            it does), and when two ``FileID`` values share a name.
    """
    names = _basenames(index)
    if conversion_manifest is None:
        if len(names) != 1:
            raise ConverterError(
                "locations.dat names {} tree files ({}); --conversion-manifest (the version 2 "
                "conversion's manifest.json, whose provenance.source_files records the "
                "converter's tree-file order) is required".format(
                    len(names), _examples(sorted(names.values()))
                )
            )
        ((file_id, name),) = names.items()
        return {file_id: 0}, [name]
    path = Path(conversion_manifest)
    try:
        with open(path) as handle:
            document = json.load(handle)
    except (OSError, ValueError) as exc:
        raise ConverterError(
            "--conversion-manifest {}: not a readable JSON manifest ({})".format(path, exc)
        ) from exc
    recorded = (
        (document.get("provenance") or {}).get("source_files")
        if isinstance(document, dict)
        else None
    )
    if (
        not isinstance(recorded, list)
        or not recorded
        or not all(isinstance(entry, str) for entry in recorded)
    ):
        raise ConverterError(
            "--conversion-manifest {}: no ordered provenance.source_files list; the tree-file "
            "order is missing".format(path)
        )
    order = [os.path.basename(entry) for entry in recorded]
    repeated = sorted({name for name in order if order.count(name) > 1})
    if repeated:
        raise ConverterError(
            "--conversion-manifest {}: source_files names {} more than once; the tree-file "
            "order is ambiguous".format(path, _examples(repeated))
        )
    listed = set(order)
    indexed = set(names.values())
    if listed != indexed:
        raise ConverterError(
            "--conversion-manifest {}: its source files are not locations.dat's: {} named only "
            "by the manifest (e.g. {}), {} only by locations.dat (e.g. {})".format(
                path,
                len(listed - indexed),
                _examples(sorted(listed - indexed)),
                len(indexed - listed),
                _examples(sorted(indexed - listed)),
            )
        )
    position = {name: ordinal for ordinal, name in enumerate(order)}
    return {file_id: position[name] for file_id, name in names.items()}, order


def sidecar_file_ordinals(
    index: SourceIndex, table: ForestTable, sidecar: Mapping[str, np.ndarray]
) -> Dict[int, int]:
    """For a version 3 input, the ``FileID`` -> ``SourceFileOrdinal`` mapping
    its own sidecar implies through the forests held in one file, refused
    when it is not one-to-one; a ``FileID`` whose every forest spans files
    maps to -1. The unit ordinals and the spanning forests must agree too."""
    _basenames(index)
    files = np.asarray(sidecar["SourceFileOrdinal"], dtype=np.int64)
    units = np.asarray(sidecar["SourceUnitOrdinal"], dtype=np.int64)
    single = table.file_ordinal >= 0
    bad_units = units != table.unit_ordinal
    bad_span = (~single) & ((files != -1) | (units != -1))
    if bad_units.any() or bad_span.any():
        rows = np.flatnonzero(bad_units | bad_span)
        raise ConverterError(
            "index files inconsistent with the input sidecar: {} forest(s) whose unit ordinal "
            "or spanning status differs; ForestIndex examples: {}".format(
                rows.size, _examples(rows.tolist())
            )
        )
    pairs = np.unique(np.stack((table.file_ordinal[single], files[single]), axis=1), axis=0)
    if pairs.size:
        if (
            np.unique(pairs[:, 0]).size != pairs.shape[0]
            or np.unique(pairs[:, 1]).size != pairs.shape[0]
        ):
            raise ConverterError(
                "index files inconsistent with the input sidecar: FileID to SourceFileOrdinal is "
                "not one-to-one; (FileID, ordinal) pairs: {}".format(_examples(pairs.tolist()))
            )
        if (pairs[:, 1] < 0).any():
            raise ConverterError(
                "input sidecar: a forest held in one file carries SourceFileOrdinal -1; "
                "(FileID, ordinal) pairs: {}".format(_examples(pairs.tolist()))
            )
    mapping = {int(file_id): int(ordinal) for file_id, ordinal in pairs.tolist()}
    return {file_id: mapping.get(file_id, -1) for file_id in index.filenames}


def output_sidecar(
    table: ForestTable, file_ordinals: Mapping[int, int], forest_ids: np.ndarray
) -> Dict[str, np.ndarray]:
    """The three sidecar columns: the input's forest ids, each single-file
    forest's conversion file ordinal and unit ordinal, -1/-1 for a forest
    spanning files."""
    single = table.file_ordinal >= 0
    keys = np.asarray(sorted(file_ordinals), dtype=np.int64)
    values = np.asarray([file_ordinals[key] for key in keys.tolist()], dtype=np.int64)
    files = np.full(table.forest_ids.size, -1, dtype=np.int64)
    if single.any():
        position = np.searchsorted(keys, table.file_ordinal[single])
        position = np.minimum(position, max(keys.size - 1, 0))
        found = keys.size > 0 and keys[position] == table.file_ordinal[single]
        held = np.where(found, values[position], -1)
        files[single] = held
    unknown = single & (files < 0)
    if unknown.any():
        raise ConverterError(
            "{} forest(s) held in one file whose FileID has no conversion ordinal; ForestIndex "
            "examples: {}".format(int(unknown.sum()), _examples(np.flatnonzero(unknown).tolist()))
        )
    units = np.where(single, table.unit_ordinal, -1).astype(np.int64)
    return {
        "ForestID": np.ascontiguousarray(forest_ids, dtype=np.int64),
        "SourceFileOrdinal": files,
        "SourceUnitOrdinal": units,
    }


# ---------------------------------------------------------------------------
# The planned working set
# ---------------------------------------------------------------------------

#: The concurrent peak per phase (package docstring): n the widest slab's
#: rows, F the forests, T the index's trees, P its (file, forest) pairs, b the
#: block rows read at a time. The per-row and per-tree coefficients marked
#: measured were measured with tracemalloc on a synthetic 400,000-row slab.
MEMORY_FORMULAS = {
    "pass0": "133 B x T + 80 B x F + 8 B x P + 24 B x b at the inventory step, holding the "
    "index (32 B x T), its forest table (40 B x F + 8 B x P), the sidecar columns (24 B x F), "
    "the roots and their slabs (12 B x T), the totals and bases (16 B x F) and the inventory "
    "(56 B x T) with the marker ordinals' transient (33 B x T, measured); earlier transients "
    "are smaller: the index load (79 B x T), the forest table's build (57 B x T at Shin-Uchuu's "
    "ratio, 81 B x T when forests equal trees) and the roots' gathering (42 B x T)",
    "baseline": "16 B x F (forest bases, running counts) + 16 B x T (roots, tree counts) + "
    "4 B x n (the previous slab's labels) through passes 1 and 2, once pass 0's tables are "
    "released",
    "remap_store": "32 B per row of each resident slab, at most three slabs: 96 B x n",
    "pass1": "the working set of one slab beside the baseline and the two held remap entries "
    "(64 B x n): 68 B x n, and 79 B x n for a last slab whose every halo is terminal "
    "(measured; the sort's four key columns and workspace, then the identities, the labels "
    "and the root lookup's transients)",
    "pass2": "the working set of one slab beside the baseline and the three held remap "
    "entries: 45 B x n while writing (one input column and its gathered output, or a link's "
    "temporaries and the target's inverse permutation) and 79 B x n while verifying (the "
    "written and input link, the compared target ids, one target slab's MostBoundID, then the "
    "identity columns), measured",
    "peak": "the larger of pass 0 and the slab passes' 16 B x F + 16 B x T + 179 B x n; "
    "Shin-Uchuu (n 519,342,987, F 166,547,771, T 315,004,242, P about 320 million): about "
    "58 GB for pass 0 and about 101 GB for pass 2's verification, the run's peak",
    "cut": "a cut adds the labelling pre-pass (8 B x n + 16 B x b + about 20 B x n beside "
    "pass 0's arrays), the table's installation (84 B x T + 82 B x F' + 8 B x P + 56 B x p "
    "after a 128 B x T read, about 84 GB concurrent at Shin-Uchuu), 24 B x T to the baseline, "
    "16 B per row per resident remap entry (144 B x n over three), a pass 1 working set of "
    "298 B x n in place of 68 B x n (measured; 219 GB concurrent, the run's peak) and a "
    "pass 2 working set of 56 B x n writing and 117 B x n verifying (measured; 150 GB "
    "concurrent); the planned peak below uses the input's forests for F'",
}


def planned_peak_bytes(
    widest: int, n_forests: int, n_trees: int, n_pairs: int, block_rows: int, cut: bool = False
) -> int:
    """The concurrent peak the formulas above plan for this input: the larger
    of pass 0's inventory step and the slab passes' verification peak, or for
    a cut of the installation and pass 1 (with the input's forest count
    standing in for the output's, unknown before the table is installed)."""
    pass0 = 133 * n_trees + 80 * n_forests + 8 * n_pairs + 24 * block_rows
    if cut:
        install = 52 * n_trees + 80 * n_forests + 8 * n_pairs + 84 * n_trees + 82 * n_forests
        install += 8 * n_pairs + 56 * n_trees
        slabs = 26 * n_forests + 40 * n_trees + (4 + 96 + 298) * widest
        return int(max(pass0, install, slabs))
    slabs = 16 * n_forests + 16 * n_trees + 179 * widest
    return int(max(pass0, slabs))


# ---------------------------------------------------------------------------
# Pins and configuration
# ---------------------------------------------------------------------------


def _pins(request: RewriteRequest, dataset: HorizontalDataset) -> List[Dict[str, object]]:
    pins = [
        pin_dependency(request.a_list, (ROLE_A_LIST,), content_sha256=True),
        pin_dependency(request.simulation_info, (ROLE_SIMULATION_INFO,), content_sha256=True),
        pin_dependency(request.forests_list, (ROLE_FORESTS_LIST,), content_sha256=True),
        pin_dependency(request.locations, (ROLE_LOCATIONS,), content_sha256=True),
    ]
    if request.column_map is not None:
        pins.append(pin_dependency(request.column_map, (ROLE_COLUMN_MAP,), content_sha256=True))
    if request.conversion_manifest is not None:
        pins.append(
            pin_dependency(
                request.conversion_manifest, (ROLE_CONVERSION_MANIFEST,), content_sha256=True
            )
        )
    if request.cut_table is not None:
        pins.append(pin_dependency(request.cut_table, (ROLE_CUT_TABLE,), content_sha256=True))
    if request.cut_table_record is not None:
        pins.append(
            pin_dependency(request.cut_table_record, (ROLE_CUT_TABLE_RECORD,), content_sha256=True)
        )
    for snap in dataset.snapshots:
        pins.append(pin_dependency(dataset.snapshot_path(snap), (ROLE_INPUT,)))
    pins.append(pin_dependency(dataset.sidecar_path, (ROLE_INPUT,)))
    return pins


def _sha256_or_none(path) -> Optional[str]:
    return None if path is None else sha256_file(path)


def _configuration(
    request: RewriteRequest,
    dataset: HorizontalDataset,
    schema: CanonicalSchema,
    scale_factors: Tuple[float, ...],
    tree_file_order: List[str],
    input_dir: Path,
    scratch_dir: Path,
    index_trees: int,
    index_pairs: int,
    cut_table_md5: Optional[str],
    cut_record: Optional[Dict[str, object]],
    pinned: Mapping[str, str],
) -> Dict[str, object]:
    """The manifest's frozen configuration: everything that decides an output
    byte, under the keys the report reads, with the rewriter's own values.
    The cut table is bound by its md5 (the census's digest of it) and its
    SHA-256, the latter taken from its pin (``pinned``: role -> content
    digest) so the table is read once for each; the record contributes the
    rule text and its dataset lineage."""
    input_dtype = np.dtype(
        [
            (name, dtype.str, shape) if shape else (name, dtype.str)
            for name, (dtype, shape) in dataset.halo_dtypes(0).items()
        ]
    )
    parameters = {
        "rewriter_version": REWRITER_VERSION,
        "operation": request.operation,
        "input": str(input_dir),
        "input_format_version": dataset.format_version,
        "scratch": str(scratch_dir),
        "simulation_info": str(_resolved(request.simulation_info, "--simulation-info")),
        "forests_list": str(_resolved(request.forests_list, "--forests-list")),
        "locations": str(_resolved(request.locations, "--locations")),
        "column_map": (
            None
            if request.column_map is None
            else str(_resolved(request.column_map, "--column-map"))
        ),
        "conversion_manifest": (
            None
            if request.conversion_manifest is None
            else str(_resolved(request.conversion_manifest, "--conversion-manifest"))
        ),
        "tree_file_order": list(tree_file_order),
        "tree_file_order_sha256": canonical_sha256(list(tree_file_order)),
        "multiplier": int(request.multiplier),
        "cut_table": (
            None if request.cut_table is None else str(_resolved(request.cut_table, "--cut-table"))
        ),
        "cut_table_md5": cut_table_md5,
        "cut_table_sha256": pinned.get(ROLE_CUT_TABLE),
        "cut_table_record": (
            None
            if request.cut_table_record is None
            else str(_resolved(request.cut_table_record, "--cut-table-record"))
        ),
        "cut_table_record_sha256": pinned.get(ROLE_CUT_TABLE_RECORD),
        "cut_rule": None if cut_record is None else cut_record["rule"],
        "cut_table_record_dataset": None if cut_record is None else cut_record["dataset"],
        "a_list_sha256": sha256_file(request.a_list),
        "simulation_info_sha256": sha256_file(request.simulation_info),
        "column_map_sha256": _sha256_or_none(request.column_map),
        "conversion_manifest_sha256": _sha256_or_none(request.conversion_manifest),
        "forests_list_sha256": sha256_file(request.forests_list),
        "locations_sha256": sha256_file(request.locations),
    }
    widest = max(dataset.n_halos) if dataset.n_halos else 0
    parameters["planned_peak_bytes"] = planned_peak_bytes(
        widest,
        dataset.n_forests_total,
        index_trees,
        index_pairs,
        int(request.block_rows),
        request.operation == OPERATION_CUT,
    )
    return {
        "adapter": {"source_format": SOURCE_FORMAT, "parameters": parameters},
        "schema": schema_record(schema),
        "record_dtypes": {
            # the input row as it is read, and the output row as it is written
            "ingest": dtype_descriptor(input_dtype),
            "transposed": dtype_descriptor(output_dtype(schema)),
        },
        "snapshots": {
            "a_list_path": str(_resolved(request.a_list, "--a-list")),
            "numbers": list(range(len(scale_factors))),
            "scale_factors": [float(value) for value in scale_factors],
        },
        # the report's working-set keys: the rewriter reads in blocks of this
        # many rows, and the budget it plans for is the concurrent peak of
        # the package docstring's formulas over this input (planned_peak_bytes)
        "ingest_max_rows": int(request.block_rows),
        "transpose_budget_bytes": int(parameters["planned_peak_bytes"]),
    }


# ---------------------------------------------------------------------------
# The whole preparation
# ---------------------------------------------------------------------------


def prepare(request: RewriteRequest) -> Prepared:
    """Every refusal of the module docstring, in order; returns the prepared
    run. Reads the input headers, the sidecar, the metadata files and the
    index files; writes nothing."""
    check_options(request)
    input_dir, _output_dir, scratch_dir = check_paths(request)
    dataset = HorizontalDataset(input_dir)
    check_dataset(request, dataset)
    scale_factors = check_a_list(request, dataset)
    metadata = check_metadata(request, dataset)
    schema = load_schema(request, dataset)
    declarations = schema.output_field_declarations()
    table = v3_halo_datasets(schema)
    check_columns(dataset, table)
    if request.operation != OPERATION_CUT:
        check_multiplier(request, dataset.n_forests_total, dataset.max_halo_rank_in_forest)
    index = load_index(request)
    forest_table = index.forest_table()
    sidecar_in = dataset.sidecar_columns()
    forest_ids = np.ascontiguousarray(sidecar_in["ForestID"], dtype=np.int64)
    check_forest_ids(dataset, forest_table, forest_ids)
    parsed_counts = None
    if request.operation == OPERATION_MIGRATE:
        file_ordinals, order = tree_file_ordinals(index, request.conversion_manifest)
        if request.conversion_manifest is not None:
            parsed_counts = load_parsed_counts(request.conversion_manifest)
    else:
        missing = sorted(set(V3_SIDECAR_DATASETS) - set(sidecar_in))
        if missing:
            raise ConverterError(
                "{}: a version 3 sidecar without {}".format(dataset.sidecar_path, missing)
            )
        file_ordinals = sidecar_file_ordinals(index, forest_table, sidecar_in)
        order = []
    sidecar = output_sidecar(forest_table, file_ordinals, forest_ids)
    if request.operation == OPERATION_NOOP:
        for name in V3_SIDECAR_DATASETS:
            if not np.array_equal(sidecar[name], np.asarray(sidecar_in[name], dtype=np.int64)):
                raise ConverterError(
                    "index files inconsistent with the input sidecar: /{} differs".format(name)
                )
    cut_table_md5, cut_record = None, None
    if request.operation == OPERATION_CUT:
        cut_table_md5, cut_record = check_cut_table_files(request)
    pins = _pins(request, dataset)
    pinned = {role: pin["sha256"] for pin in pins for role in pin["roles"] if "sha256" in pin}
    configuration = _configuration(
        request,
        dataset,
        schema,
        scale_factors,
        order,
        input_dir,
        scratch_dir,
        index.n_trees,
        int(forest_table.files.size),
        cut_table_md5,
        cut_record,
        pinned,
    )
    return Prepared(
        request=request,
        dataset=dataset,
        schema=schema,
        declarations=declarations,
        table=table,
        metadata=metadata,
        scale_factors=scale_factors,
        index=index,
        forest_table=forest_table,
        file_ordinals=file_ordinals,
        tree_file_order=order,
        parsed_counts=parsed_counts,
        sidecar=sidecar,
        pins=pins,
        configuration=configuration,
        cut_table_md5=cut_table_md5,
        cut_record=cut_record,
    )


# ---------------------------------------------------------------------------
# The cut table's files
# ---------------------------------------------------------------------------


def read_record_header(path) -> Dict[str, object]:
    """The header entries of a cut table's JSON record, without its per-piece
    columns. The census writes the record with one header entry per line
    before a ``"pieces"`` line (``census.cut_table.write_table_record``), so
    the header is read line by line up to that line and parsed alone, which
    bounds the read at a few kilobytes however many pieces follow; a record
    without that line is parsed whole."""
    path = Path(path)
    lines: List[str] = []
    try:
        with open(path) as handle:
            for line in handle:
                if line.startswith('  "pieces":'):
                    text = "".join(lines).rstrip().rstrip(",") + "\n}\n"
                    break
                lines.append(line)
            else:
                text = "".join(lines)
        document = json.loads(text)
    except (OSError, ValueError) as exc:
        raise ConverterError(
            "--cut-table-record {}: not a readable JSON record ({})".format(path, exc)
        ) from exc
    if not isinstance(document, dict):
        raise ConverterError("--cut-table-record {}: not a JSON object".format(path))
    return document


def check_cut_table_files(request: RewriteRequest) -> Tuple[str, Optional[Dict[str, object]]]:
    """The cut table must exist (its invariants are checked once the trees'
    totals are known); its record, when given, must name the table's md5 and
    the index files' md5 values and carry a rule text. Returns the table's
    md5 and the record's header (``None`` without a record)."""
    table = request.cut_table
    if table is None or not Path(table).is_file():
        raise ConverterError("--cut-table {}: no such file".format(table))
    table_md5 = md5_file(table)
    if request.cut_table_record is None:
        return table_md5, None
    record = read_record_header(request.cut_table_record)
    held = record.get("table") if isinstance(record.get("table"), dict) else {}
    if held.get("md5") != table_md5:
        raise ConverterError(
            "--cut-table-record {}: records table md5 {!r} but --cut-table {} has md5 {}; the "
            "record is not this table's".format(
                request.cut_table_record, held.get("md5"), table, table_md5
            )
        )
    files = record.get("index_files") if isinstance(record.get("index_files"), dict) else {}
    for name, option, path in (
        ("forests_list", "--forests-list", request.forests_list),
        ("locations", "--locations", request.locations),
    ):
        entry = files.get(name) if isinstance(files.get(name), dict) else {}
        actual = md5_file(path)
        if entry.get("md5") != actual:
            raise ConverterError(
                "--cut-table-record {}: records {} md5 {!r} but {} {} has md5 {}; the table was "
                "built for other index files".format(
                    request.cut_table_record, name, entry.get("md5"), option, path, actual
                )
            )
    rule = record.get("rule") if isinstance(record.get("rule"), dict) else {}
    if not isinstance(rule.get("text"), str) or not rule["text"]:
        raise ConverterError(
            "--cut-table-record {}: no rule text (rule.text)".format(request.cut_table_record)
        )
    return table_md5, {"rule": rule["text"], "dataset": record.get("dataset")}


def check_header_reproduced(
    dataset: HorizontalDataset, snap: int, header: Mapping[str, object]
) -> None:
    """For the no-op: the header about to be written must equal the input
    slab's own, attribute by attribute (the format's string attributes
    compared as text)."""
    held = dataset.header(snap)
    for name, value in header.items():
        if name not in held or held[name] != value:
            raise ConverterError(
                "{}: header {} would be written as {!r} but the input holds {!r}; refusing to "
                "reproduce it".format(dataset.snapshot_path(snap), name, value, held.get(name))
            )


def output_header(prepared: Prepared, snap: int, n_halos: int, max_rank: int) -> Dict[str, object]:
    """One output slab's ``/header`` attributes, in the writer's table."""
    header: Dict[str, object] = {
        "format_version": 3,
        "links_adjacent": 1,
        "scale_factor": float(prepared.scale_factors[snap]),
        "snapshot_number": int(snap),
        "n_halos": int(n_halos),
        "n_forests_total": int(prepared.n_forests),
        "max_halo_rank_in_forest": int(max_rank),
        "source_format": SOURCE_FORMAT,
        "column_mapping_sha256": prepared.schema.digest,
    }
    header.update(prepared.metadata)
    return header
