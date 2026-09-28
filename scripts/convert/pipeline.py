"""Generic, resumable stage orchestration for canonical adapter conversions
(contract C4 of docs/dev/MIMIC-CONVERTER-GENERALISATION-IMPLEMENTATION-PLAN.md).

**Stages.** A generic conversion is :func:`initialize` followed by three
explicit stages, each resumable on its own and each refusing to run before its
predecessor is complete:

1. :func:`run_ingest` streams the source adapter's canonical batches into
   ``ingest/chunk_NNNNNN.bin`` -- flat little-endian records of
   :func:`ingest_dtype`, one file per batch -- each written under a temporary
   name, fsynced, renamed, re-read and registered with its SHA-256.
2. :func:`run_transpose` feeds those chunks, checksum-verified as they are
   read, to ``transpose.transpose`` and registers every per-snapshot output.
3. :func:`run_write` hands the verified transposed snapshots to a
   :class:`StageWriter` and registers what it produced. The concrete v3 HDF5
   writer is ``hdf5_writer_v3.HorizontalV3Writer``; this module owns the
   stage, its state transitions and the inputs it hands a writer --
   including the forest enumeration for the ``forests.h5`` sidecar, which
   ingest does not persist and :func:`run_write` re-derives read-only from the
   recorded adapter configuration, bound to the recorded inventory
   (:attr:`WriteInputs.forests`).

State lives in ``conversion_manifest.ConversionManifest``. Nothing here keeps
state between calls: every stage entry reloads the manifest, rebuilds the
schema from its embedded record, re-derives every record dtype and compares
it with the recorded descriptor, re-pins every source dependency, and -- for
stages that read the source -- re-derives the inventory and requires it to
match, all *before* the first mutation. A stage call that names a schema must
name the recorded one exactly.

**Resume.**

- *Ingest* resumes at chunk granularity. Registered chunks are verified on
  disk first; the adapter is then re-read from the start with the recorded
  ``ingest_max_rows``, and every batch that corresponds to a registered chunk
  must serialize to exactly that chunk's recorded SHA-256 and row count
  before it is skipped. So a resumed ingest can neither duplicate nor drop a
  halo, nor assign a different ``SourceHaloID`` or forest enumeration, without
  failing: the new chunks are appended only after the old ones were proven to
  be what the source still yields. Chunk files an interrupted run wrote but
  never registered are removed -- only names of this stage's own pattern at
  or beyond the registered count; anything else in the directory is refused.
- *Resume cost.* That proof is paid in full on every ingest resume: each
  registered chunk is re-hashed from disk, and the adapter is re-read from
  the start of the source with every batch re-serialized and hashed, the
  registered ones included. A crash late in ingest therefore costs roughly
  two passes over the chunks already written plus a full re-read of the
  source before the first new chunk is appended.
  :data:`DEFAULT_SAVE_EVERY_CHUNKS` bounds only the loss of chunks written
  but not yet registered, never this verification cost.
- *Transpose* and *write* are all-or-nothing per attempt. Each attempt's
  directory (``transpose/attempt_NNN``, ``write/attempt_NNN``) is recorded in
  the manifest before it is created, and an interrupted attempt's directory is
  discarded whole before the next begins. A stage is marked complete only
  after every artifact it produced has been checked against independent
  expectations (row counts from ingest, file sizes, the exact directory
  listing) and registered with its checksum, in the same manifest save.
- *Tuning.* A format's performance-only parameters -- for ASCII the
  preparation's ``pool_size`` and ``chunksize`` -- change no output, so they
  are recorded under the manifest's ``tuning`` rather than in the frozen
  configuration, and :func:`initialize` compares the configuration without
  them. Re-initializing with different tuning is a resume, not a different
  conversion: while ingest is incomplete the new values are recorded and the
  next ingest attempt uses them; once it is complete they are accepted and
  nothing is written, so ``tuning`` keeps the values the completed ingest
  used. Every other parameter change is still refused.

**Skip-trust.** Re-running a complete stage verifies every one of its
artifacts' SHA-256 before reporting it done; an artifact a later complete
stage deliberately consumed is accepted as consumed, never re-stat-ed.

**Cleanup** is opt-in (``consume_ingest``, ``consume_transposed``), happens
only after the consuming stage is complete and its own artifacts re-verify,
and deletes only verified, manifest-registered artifacts inside the workdir
(``ConversionManifest.consume_stage``).

**Sources are read-only.** Adapters open source files for reading only, the
manifest records them as pinned dependencies, and a workdir that contains a
source file is refused at :func:`initialize`. There is no release or
transfer step for these adapters.

**Failure.** A failing stage records ``failed`` with the error text (best
effort), never ``complete``, and re-raises; the error names the offending
source file, chunk or artifact. Every stage attempt runs inside
:func:`_stage_attempt`, the only place a stage is completed.

**Memory.** Ingest holds one adapter batch and its serialized records (at
most ``ingest_max_rows`` rows); transpose reads chunks in slices of the
transpose's own planned batch size, so its ``transpose_budget_bytes`` bound is
unchanged by persistence. The ASCII preparation refuses a snapshot whose
ingest batch cannot fit the adapter budget as soon as scatter has counted it,
before sort, fix-ups and links run.
"""

import abc
import functools
import hashlib
import json
import os
import re
from contextlib import contextmanager
from dataclasses import dataclass
from pathlib import Path
from typing import Callable, Dict, Iterator, List, Mapping, Optional, Sequence, Tuple

import numpy as np
import transpose as transpose_module
from adapters.base import LINK_FIELDS, CanonicalBatch, SourceAdapter, require_integer
from column_schema import (
    EXTRA_TYPES,
    IDENTITY_FIELDS,
    SOURCE_FORMATS,
    CanonicalSchema,
    ConverterError,
)
from conversion_manifest import (
    ARTIFACT_PRESENT,
    ARTIFACT_REMOVED,
    MANIFEST_ABSENT,
    MANIFEST_GENERIC,
    SOURCE_ROLE,
    STAGE_FAILED,
    STAGE_RUNNING,
    STAGES,
    ConversionManifest,
    canonical_json,
    classify_manifest,
    dtype_descriptor,
    finite_float,
    inventory_record,
    pin_dependency,
    schema_record,
)
from rank_sort import read_into
from scatter import SCRATCH_BUFFER_BYTES, load_a_list

__all__ = [
    "ASCII_PREPARATION_DIR",
    "DEFAULT_INGEST_MAX_ROWS",
    "DEFAULT_SAVE_EVERY_CHUNKS",
    "DEFAULT_TRANSPOSE_BUDGET_BYTES",
    "INGEST_DIR",
    "TRANSPOSE_DIR",
    "WRITE_DIR",
    "StageWriter",
    "TransposedSnapshot",
    "WriteInputs",
    "build_adapter",
    "canonical_parameters",
    "chunk_name",
    "ingest_dtype",
    "initialize",
    "run_ingest",
    "run_transpose",
    "run_write",
]

INGEST_DIR = "ingest"
TRANSPOSE_DIR = "transpose"
WRITE_DIR = "write"
#: The ASCII adapter's own topology-preparation workdir, inside the generic
#: one. It holds the ``scatter.Manifest`` extended-layout state that
#: ``ctrees_ascii.prepare_workdir`` owns and resumes by its own rules.
ASCII_PREPARATION_DIR = "ascii_preparation"

DEFAULT_INGEST_MAX_ROWS = 1 << 20
DEFAULT_TRANSPOSE_BUDGET_BYTES = 2 * 1024**3
#: Registered chunks are saved into the manifest at least this often. A crash
#: loses at most this many chunks' worth of re-ingest; saving every chunk
#: would rewrite an O(chunks) manifest O(chunks) times.
DEFAULT_SAVE_EVERY_CHUNKS = 32

#: ``chunk_name`` zero-pads to six digits and grows past them at index
#: 1,000,000, so the pattern accepts six or more.
_CHUNK_RE = re.compile(r"^chunk_(\d{6,})\.bin(\.partial)?$")
_PARTIAL_SUFFIX = ".partial"

#: Ingest-record fields carrying the source coordinate. The leading
#: underscore cannot collide with any declared name (C2 names start with a
#: letter).
_COORDINATE_FIELDS = (
    ("_file", "source_file_ordinal"),
    ("_unit", "unit_ordinal"),
    ("_row", "row_ordinal"),
)
_IDENTITY_NAMES = tuple(field.name for field in IDENTITY_FIELDS)


# ==========================================================================
# Record layouts
# ==========================================================================


def _field_spec(name: str, type_name: str) -> Tuple:
    spec = EXTRA_TYPES[type_name]
    base = np.dtype(spec.numpy_dtype).newbyteorder("<")
    if spec.n_components == 1:
        return (name, base)
    return (name, base, (spec.n_components,))


def ingest_dtype(schema: CanonicalSchema) -> np.dtype:
    """One canonical halo as the ingest stage persists it: source coordinate,
    identity, the five links as target ``SourceHaloID``, payload and extras,
    explicitly little-endian and in declared precision."""
    specs: List[Tuple] = [(name, "<i8") for name, _canonical in _COORDINATE_FIELDS]
    specs.extend((name, "<i8") for name in _IDENTITY_NAMES)
    specs.extend((name, "<i8") for name in LINK_FIELDS)
    specs.extend(_field_spec(field.name, field.type) for field in schema.payload_fields)
    specs.extend(_field_spec(extra.name, extra.type) for extra in schema.extra_fields)
    try:
        return np.dtype(specs)
    except ValueError as exc:  # pragma: no cover - C2 name rules make collisions impossible
        raise ConverterError(
            "schema {} has colliding field names: {}".format(schema.digest, exc)
        ) from exc


def chunk_name(index: int) -> str:
    return "chunk_{:06d}.bin".format(int(index))


def _batch_records(batch: CanonicalBatch, dtype: np.dtype) -> np.ndarray:
    records = np.empty(batch.n_rows, dtype=dtype)
    for name, canonical in _COORDINATE_FIELDS:
        records[name] = batch.coordinates[canonical]
    for group in (batch.identity, batch.links, batch.payload, batch.extras):
        for name, values in group.items():
            records[name] = values
    return records


def _native(values: np.ndarray, numpy_dtype: str) -> np.ndarray:
    target = np.dtype(numpy_dtype)
    return values if values.dtype == target else values.astype(target)


def _batch_from_records(schema: CanonicalSchema, records: np.ndarray) -> CanonicalBatch:
    """Views into ``records`` in native byte order: no copy on a
    little-endian host, a byte swap on a big-endian one."""
    return CanonicalBatch(
        schema=schema,
        identity={name: _native(records[name], "int64") for name in _IDENTITY_NAMES},
        coordinates={
            canonical: _native(records[name], "int64") for name, canonical in _COORDINATE_FIELDS
        },
        links={name: _native(records[name], "int64") for name in LINK_FIELDS},
        payload={
            field.name: _native(records[field.name], EXTRA_TYPES[field.type].numpy_dtype)
            for field in schema.payload_fields
        },
        extras={
            extra.name: _native(records[extra.name], extra.spec.numpy_dtype)
            for extra in schema.extra_fields
        },
    )


def _record_dtypes(schema: CanonicalSchema) -> Dict[str, object]:
    return {
        "ingest": dtype_descriptor(ingest_dtype(schema)),
        "transposed": dtype_descriptor(transpose_module.output_dtype(schema)),
    }


# ==========================================================================
# Adapter parameters and routes
# ==========================================================================


#: Why ``require_integer`` refuses to coerce a recorded configuration scalar.
_RECORDED = "it is recorded configuration and is never coerced"


def _path_string(value, what: str) -> str:
    if not isinstance(value, (str, os.PathLike)):
        raise ConverterError("{} must be a filesystem path, got {!r}".format(what, value))
    return os.path.realpath(os.fspath(value))


def _check_keys(parameters: Mapping, required: Sequence[str], optional: Mapping, what: str):
    if not isinstance(parameters, Mapping):
        raise ConverterError("{} parameters must be a mapping".format(what))
    allowed = set(required) | set(optional)
    unknown = sorted(set(parameters) - allowed)
    missing = sorted(set(required) - set(parameters))
    if unknown or missing:
        raise ConverterError(
            "{} parameters: unknown {}, missing {} (allowed: {})".format(
                what, unknown, missing, sorted(allowed)
            )
        )
    merged = dict(optional)
    merged.update(parameters)
    return merged


def _default_adapter_budget() -> int:
    from adapters.lhalo_binary import DEFAULT_MEMORY_BUDGET_BYTES

    return DEFAULT_MEMORY_BUDGET_BYTES


def _optional_simulation_info(recorded: Dict[str, object], merged: Mapping) -> Dict[str, object]:
    """Add ``simulation_info`` to the two prelinked formats' recorded
    parameters when it was given, and only then.

    Neither prelinked adapter reads physical metadata itself (and
    ``particle_mass`` alone does not identify the metadata a header is written
    from), but a caller that names the file (``convert_trees.py`` always
    does) binds the conversion to its content, as the ASCII route does, so the
    write stage can refuse a header from different metadata.
    """
    if merged["simulation_info"] is not None:
        recorded["simulation_info"] = _path_string(merged["simulation_info"], "simulation_info")
    return recorded


def _lhalo_parameters(parameters: Mapping) -> Dict[str, object]:
    merged = _check_keys(
        parameters,
        ("sources",),
        {"memory_budget_bytes": _default_adapter_budget(), "simulation_info": None},
        "lhalo_binary",
    )
    sources = merged["sources"]
    if isinstance(sources, (str, bytes)) or not isinstance(sources, Sequence) or not sources:
        raise ConverterError("lhalo_binary sources must be a nonempty list of (ordinal, path)")
    canonical_sources = []
    for position, entry in enumerate(sources):
        if isinstance(entry, (str, bytes)) or not isinstance(entry, Sequence) or len(entry) != 2:
            raise ConverterError(
                "lhalo_binary sources[{}] must be an (ordinal, path) pair, got {!r}".format(
                    position, entry
                )
            )
        canonical_sources.append(
            [
                require_integer(
                    entry[0], "sources[{}] ordinal".format(position), _RECORDED, minimum=0
                ),
                _path_string(entry[1], "sources[{}] path".format(position)),
            ]
        )
    recorded = {
        "sources": canonical_sources,
        "memory_budget_bytes": require_integer(
            merged["memory_budget_bytes"], "memory_budget_bytes", _RECORDED, minimum=1
        ),
    }
    return _optional_simulation_info(recorded, merged)


def _hdf5_parameters(parameters: Mapping) -> Dict[str, object]:
    merged = _check_keys(
        parameters,
        ("info_path", "first_file", "last_file", "particle_mass"),
        {"memory_budget_bytes": _default_adapter_budget(), "simulation_info": None},
        "consistent_trees_hdf5",
    )
    particle_mass = finite_float(merged["particle_mass"], "particle_mass")
    if particle_mass <= 0.0:
        raise ConverterError("particle_mass must be positive, got {!r}".format(particle_mass))
    recorded = {
        "info_path": _path_string(merged["info_path"], "info_path"),
        "first_file": require_integer(merged["first_file"], "first_file", _RECORDED, minimum=0),
        "last_file": require_integer(merged["last_file"], "last_file", _RECORDED, minimum=0),
        "particle_mass": particle_mass,
        "memory_budget_bytes": require_integer(
            merged["memory_budget_bytes"], "memory_budget_bytes", _RECORDED, minimum=1
        ),
    }
    return _optional_simulation_info(recorded, merged)


def _ascii_parameters(parameters: Mapping) -> Dict[str, object]:
    from ctrees_parser import DEFAULT_CHUNKSIZE
    from links import DEFAULT_RANK_BUDGET_BYTES

    merged = _check_keys(
        parameters,
        ("tree_files", "forests_list", "simulation_info"),
        {
            "memory_budget_bytes": _default_adapter_budget(),
            "pool_size": 1,
            "chunksize": DEFAULT_CHUNKSIZE,
            "rank_budget_bytes": DEFAULT_RANK_BUDGET_BYTES,
        },
        "consistent_trees_ascii",
    )
    tree_files = merged["tree_files"]
    if (
        isinstance(tree_files, (str, bytes))
        or not isinstance(tree_files, Sequence)
        or not tree_files
    ):
        raise ConverterError("consistent_trees_ascii tree_files must be a nonempty list")
    return {
        "tree_files": [
            _path_string(path, "tree_files[{}]".format(i)) for i, path in enumerate(tree_files)
        ],
        "forests_list": _path_string(merged["forests_list"], "forests_list"),
        "simulation_info": _path_string(merged["simulation_info"], "simulation_info"),
        "pool_size": require_integer(merged["pool_size"], "pool_size", _RECORDED, minimum=1),
        "chunksize": require_integer(merged["chunksize"], "chunksize", _RECORDED, minimum=1),
        "rank_budget_bytes": require_integer(
            merged["rank_budget_bytes"], "rank_budget_bytes", _RECORDED, minimum=1
        ),
        "memory_budget_bytes": require_integer(
            merged["memory_budget_bytes"], "memory_budget_bytes", _RECORDED, minimum=1
        ),
    }


def _lhalo_dependencies(parameters: Mapping):
    dependencies = [(path, (SOURCE_ROLE,), False) for _ordinal, path in parameters["sources"]]
    if "simulation_info" in parameters:
        dependencies.append((parameters["simulation_info"], ("simulation_info",), True))
    return dependencies


def _hdf5_dependencies(parameters: Mapping):
    dependencies = [(parameters["info_path"], (SOURCE_ROLE,), False)]
    if "simulation_info" in parameters:
        dependencies.append((parameters["simulation_info"], ("simulation_info",), True))
    return dependencies


def _ascii_dependencies(parameters: Mapping):
    dependencies = [(path, (SOURCE_ROLE,), False) for path in parameters["tree_files"]]
    dependencies.append((parameters["forests_list"], ("forests_list",), False))
    dependencies.append((parameters["simulation_info"], ("simulation_info",), True))
    return dependencies


def _build_lhalo(schema, parameters, max_snapshot, prepared_dir) -> SourceAdapter:
    from adapters.lhalo_binary import LHaloBinaryAdapter

    return LHaloBinaryAdapter(
        schema,
        [(ordinal, path) for ordinal, path in parameters["sources"]],
        max_snapshot=max_snapshot,
        memory_budget_bytes=parameters["memory_budget_bytes"],
    )


def _build_hdf5(schema, parameters, max_snapshot, prepared_dir) -> SourceAdapter:
    from adapters.ctrees_hdf5 import CTreesHDF5Adapter

    return CTreesHDF5Adapter(
        schema,
        parameters["info_path"],
        first_file=parameters["first_file"],
        last_file=parameters["last_file"],
        particle_mass=parameters["particle_mass"],
        max_snapshot=max_snapshot,
        memory_budget_bytes=parameters["memory_budget_bytes"],
    )


def _build_ascii(schema, parameters, max_snapshot, prepared_dir) -> SourceAdapter:
    from adapters.ctrees_ascii import CTreesAsciiAdapter

    if prepared_dir is None:
        raise ConverterError("the ASCII adapter reads a prepared workdir; prepare it first")
    return CTreesAsciiAdapter(
        schema, prepared_dir, memory_budget_bytes=parameters["memory_budget_bytes"]
    )


def _lhalo_source_paths(adapter, parameters: Mapping) -> List[str]:
    return [path for _ordinal, path in parameters["sources"]]


def _hdf5_source_paths(adapter, parameters: Mapping) -> List[str]:
    paths = {dependency.identity.path for dependency in adapter.dependencies()}
    paths.add(parameters["info_path"])
    return sorted(paths)


def _ascii_source_paths(adapter, parameters: Mapping) -> List[str]:
    return list(parameters["tree_files"])


def _hdf5_inventory_dependencies(adapter):
    return [
        (dependency.identity.path, (SOURCE_ROLE,), dependency.objects)
        for dependency in adapter.dependencies()
    ]


def _no_inventory_dependencies(adapter):
    return []


def _lhalo_forests(adapter) -> Iterator:
    """L-Halo's sidecar rows: one per tree, in inventory order. Its
    ``ForestIndex`` is the file-prefix tree number (C1), which is the tree's
    position in that order, and its ``ForestID`` is that same dense run forest
    number (C3), disambiguated by the file and tree ordinals. Zero-halo trees
    are forests too: they occupy a tree number."""
    from adapters.ctrees_hdf5 import ForestRecord

    for position, unit in enumerate(adapter.inventory().units):
        yield ForestRecord(
            forest_index=position,
            forest_id=position,
            source_file_ordinal=unit.source_file_ordinal,
            unit_ordinal=unit.unit_ordinal,
            n_halos=unit.n_halos,
        )


def _adapter_forests(adapter) -> Iterator:
    return adapter.iter_forests()


@dataclass(frozen=True)
class _Route:
    """Everything the pipeline does differently per source format; a new
    format is one more entry in :data:`_ROUTES`."""

    #: validates caller parameters into their recorded form, tuning included
    parameters: Callable[[Mapping], Dict[str, object]]
    #: pre-inventory dependencies: [(path, roles, content_sha256)]
    static_dependencies: Callable[[Mapping], List[Tuple[str, Tuple[str, ...], bool]]]
    #: (schema, parameters, max_snapshot, prepared_dir) -> the adapter; read-only
    build: Callable[[CanonicalSchema, Mapping, int, Optional[Path]], SourceAdapter]
    #: (adapter, parameters) -> the bulk source files the adapter reads now,
    #: for the pinned-set check
    source_paths: Callable[[SourceAdapter, Mapping], List[str]]
    #: adapter -> dependencies only its inventory reveals: [(path, roles, objects)]
    inventory_dependencies: Callable[[SourceAdapter], List[Tuple[str, Tuple[str, ...], object]]]
    #: adapter -> the ``forests.h5`` sidecar rows (:attr:`WriteInputs.forests`)
    forests: Callable[[SourceAdapter], Iterator]
    #: True when the adapter needs a preparation (a mutation) before it can
    #: report its inventory
    prepared: bool
    #: parameter names that change no output: recorded under the manifest's
    #: ``tuning``, outside the configuration identity
    tuning: Tuple[str, ...] = ()


_ROUTES: Dict[str, _Route] = {
    "lhalo_binary": _Route(
        _lhalo_parameters,
        _lhalo_dependencies,
        _build_lhalo,
        _lhalo_source_paths,
        _no_inventory_dependencies,
        _lhalo_forests,
        prepared=False,
    ),
    "consistent_trees_hdf5": _Route(
        _hdf5_parameters,
        _hdf5_dependencies,
        _build_hdf5,
        _hdf5_source_paths,
        _hdf5_inventory_dependencies,
        _adapter_forests,
        prepared=False,
    ),
    "consistent_trees_ascii": _Route(
        _ascii_parameters,
        _ascii_dependencies,
        _build_ascii,
        _ascii_source_paths,
        _no_inventory_dependencies,
        _adapter_forests,
        prepared=True,
        tuning=("pool_size", "chunksize"),
    ),
}
if tuple(sorted(_ROUTES)) != tuple(sorted(SOURCE_FORMATS)):  # pragma: no cover - contract drift
    raise ImportError("pipeline.py's adapter routes disagree with column_schema.SOURCE_FORMATS")


def _route(source_format: str) -> _Route:
    if source_format not in _ROUTES:
        raise ConverterError(
            "unknown source_format {!r}; expected one of {}".format(source_format, sorted(_ROUTES))
        )
    return _ROUTES[source_format]


def canonical_parameters(source_format: str, parameters: Mapping) -> Dict[str, object]:
    """Validate one format's adapter parameters into their recorded form:
    exact key set, defaults filled in, integers type-checked, paths resolved.
    Tuning parameters are included; :func:`initialize` separates them."""
    return _route(source_format).parameters(parameters)


def build_adapter(
    source_format: str,
    schema: CanonicalSchema,
    parameters: Mapping,
    max_snapshot: int,
    prepared_dir: Optional[Path] = None,
) -> SourceAdapter:
    """Construct the adapter a recorded configuration names. Read-only."""
    return _route(source_format).build(schema, parameters, max_snapshot, prepared_dir)


def _prepare_ascii(manifest: ConversionManifest) -> Path:
    """Run (or resume) the ASCII topology preparation inside the workdir,
    with the recorded tuning and the ingest's batch size and adapter budget,
    so an oversized snapshot is refused as soon as scatter has counted it."""
    from adapters.ctrees_ascii import prepare_workdir

    parameters = manifest.configuration["adapter"]["parameters"]
    prepared = manifest.artifact_path(ASCII_PREPARATION_DIR)
    prepare_workdir(
        manifest.schema,
        parameters["tree_files"],
        parameters["forests_list"],
        manifest.configuration["snapshots"]["a_list_path"],
        parameters["simulation_info"],
        prepared,
        pool_size=manifest.tuning["pool_size"],
        chunksize=manifest.tuning["chunksize"],
        rank_budget_bytes=parameters["rank_budget_bytes"],
        max_rows=manifest.configuration["ingest_max_rows"],
        memory_budget_bytes=parameters["memory_budget_bytes"],
    )
    return prepared


def _bind_adapter(manifest: ConversionManifest, adapter) -> None:
    """Require an adapter's live dependency set and inventory to be the
    recorded ones (recording the inventory the first time). Read-only."""
    adapter_config = manifest.configuration["adapter"]
    inventory = adapter.inventory()
    route = _route(adapter_config["source_format"])
    manifest.require_dependency_paths(route.source_paths(adapter, adapter_config["parameters"]))
    manifest.require_inventory(inventory_record(inventory))


# ==========================================================================
# Initialize and stage entry
# ==========================================================================


def initialize(
    workdir,
    schema: CanonicalSchema,
    parameters: Mapping,
    a_list_path,
    *,
    ingest_max_rows: int = DEFAULT_INGEST_MAX_ROWS,
    transpose_budget_bytes: int = DEFAULT_TRANSPOSE_BUDGET_BYTES,
) -> ConversionManifest:
    """Freeze a generic conversion's configuration into ``workdir``.

    The source format is the schema's. Every source dependency is pinned and,
    for the two prelinked formats, the complete inventory is read and
    recorded now (read-only). The format's tuning parameters are split off
    into the manifest's ``tuning``. Re-initializing a workdir with the
    identical configuration returns its manifest unchanged -- tuning apart,
    which is recorded while ingest is incomplete (see the module docstring);
    anything else different, or a legacy workdir, is refused.
    """
    if not isinstance(schema, CanonicalSchema):
        raise ConverterError(
            "initialize needs a CanonicalSchema, got {!r}".format(type(schema).__name__)
        )
    source_format = schema.source_format
    route = _route(source_format)
    recorded_parameters = route.parameters(parameters)
    tuning = {name: recorded_parameters.pop(name) for name in route.tuning}
    ingest_max_rows = require_integer(ingest_max_rows, "ingest_max_rows", _RECORDED, minimum=1)
    transpose_budget_bytes = require_integer(
        transpose_budget_bytes, "transpose_budget_bytes", _RECORDED, minimum=1
    )
    # Refused here, before any source is read, if the budget cannot hold one
    # row in some phase.
    transpose_module.plan_budget(schema, transpose_budget_bytes)

    a_list_resolved = _path_string(a_list_path, "a_list_path")
    scale_factors, _md5 = load_a_list(a_list_resolved)
    snapshots = list(range(len(scale_factors)))
    configuration = {
        "adapter": {"source_format": source_format, "parameters": recorded_parameters},
        "schema": schema_record(schema),
        "record_dtypes": _record_dtypes(schema),
        "snapshots": {
            "a_list_path": a_list_resolved,
            "numbers": snapshots,
            "scale_factors": [float(value) for value in scale_factors],
        },
        "ingest_max_rows": ingest_max_rows,
        "transpose_budget_bytes": transpose_budget_bytes,
    }

    kind = classify_manifest(workdir)
    if kind == MANIFEST_GENERIC:
        manifest = ConversionManifest.load(workdir)
        if canonical_json(manifest.configuration) != canonical_json(configuration):
            raise ConverterError(
                "{}: this workdir holds a different conversion (configuration {}); refusing "
                "to reinitialize it -- use a fresh workdir".format(
                    manifest.path, manifest.data["configuration_sha256"]
                )
            )
        manifest.verify_dependencies()
        tuning_changed = canonical_json(manifest.tuning) != canonical_json(tuning)
        if tuning_changed and not manifest.is_complete("ingest"):
            manifest.record_tuning(tuning)
        return manifest
    if kind != MANIFEST_ABSENT:
        # classify_manifest raised for everything else except legacy
        raise ConverterError(
            "{}: this is a legacy ASCII-to-v2 workdir; the generic pipeline never adopts, "
            "upgrades or adds provenance to it -- use a fresh workdir".format(workdir)
        )

    pins = [pin_dependency(a_list_resolved, ("a_list",), content_sha256=True)]
    for path, roles, content in route.static_dependencies(recorded_parameters):
        pins.append(pin_dependency(path, roles, content_sha256=content))
    inventory = None
    if not route.prepared:
        adapter = build_adapter(source_format, schema, recorded_parameters, len(snapshots) - 1)
        inventory = inventory_record(adapter.inventory())
        for path, roles, objects in route.inventory_dependencies(adapter):
            pins.append(pin_dependency(path, roles, objects))
    manifest = ConversionManifest.create(workdir, configuration, pins, inventory, tuning)
    # A dependency that moved while the inventory was read is caught here,
    # before any stage runs.
    manifest.verify_dependencies()
    return manifest


def _open_for_stage(workdir, schema: Optional[CanonicalSchema]) -> ConversionManifest:
    """Load and bind a manifest for a stage; every check here is read-only,
    so a refused resume leaves the workdir exactly as it was."""
    manifest = ConversionManifest.load(workdir)
    manifest.require_schema(schema)
    recomputed = _record_dtypes(manifest.schema)
    recorded = manifest.configuration["record_dtypes"]
    if canonical_json(recomputed) != canonical_json(recorded):
        raise ConverterError(
            "{}: the record layouts this code derives from the embedded schema differ from the "
            "recorded ones; refusing to resume with a different converter layout".format(
                manifest.path
            )
        )
    _check_tuning(manifest)
    _check_stage_results(manifest)
    manifest.verify_dependencies()
    return manifest


def _check_tuning(manifest: ConversionManifest) -> None:
    """The recorded tuning must name exactly the route's tuning parameters,
    each a positive integer. Read-only."""
    expected = _route(manifest.configuration["adapter"]["source_format"]).tuning
    tuning = manifest.tuning
    if sorted(tuning) != sorted(expected):
        raise ConverterError(
            "{}: tuning records {} but this source format takes exactly {}".format(
                manifest.path, sorted(tuning), sorted(expected)
            )
        )
    for name in expected:
        value = tuning[name]
        if isinstance(value, bool) or not isinstance(value, int) or value < 1:
            raise ConverterError(
                "{}: tuning.{} must be a positive integer, got {!r}".format(
                    manifest.path, name, value
                )
            )


#: The result keys this module, the CLI, the validator and the report index
#: for a completed stage. The manifest records a stage's result as an opaque
#: mapping; what it must hold is this module's contract, checked here so a
#: hand-edited manifest is refused by name rather than with a ``KeyError``
#: from the first consumer.
_STAGE_RESULT_KEYS = {
    "ingest": ("n_chunks", "n_rows", "snapshot_counts"),
    "transpose": (
        "total_halos",
        "n_links",
        "n_gapped_descendants",
        "max_descendant_span",
        "links_adjacent",
    ),
    "write": ("writer", "n_files"),
}


def _check_stage_results(manifest: ConversionManifest) -> None:
    """Every completed stage's result holds the keys its consumers index
    (:data:`_STAGE_RESULT_KEYS`), and ``write``'s ``writer`` is a mapping.
    Read-only."""
    for stage, keys in _STAGE_RESULT_KEYS.items():
        if not manifest.is_complete(stage):
            continue
        result = manifest.stage(stage)["result"]
        missing = [key for key in keys if not isinstance(result, Mapping) or key not in result]
        if missing:
            raise ConverterError(
                "{}: the completed {} stage's result is missing {}; the manifest was edited "
                "outside the converter".format(manifest.path, stage, missing)
            )
        if stage == "write" and not isinstance(result["writer"], Mapping):
            raise ConverterError(
                "{}: the completed write stage's result.writer must be a mapping, got {!r}".format(
                    manifest.path, type(result["writer"]).__name__
                )
            )


def _snapshots(manifest: ConversionManifest) -> List[int]:
    return list(manifest.configuration["snapshots"]["numbers"])


def _max_snapshot(manifest: ConversionManifest) -> int:
    return len(_snapshots(manifest)) - 1


class _Attempt:
    """One running stage attempt, as :func:`_stage_attempt` yields it: the
    attempt directory it created (``None`` for ingest) and the result the
    body sets for the stage to be completed with."""

    def __init__(self, directory: Optional[Path]):
        self.directory = directory
        self.result: Optional[Mapping] = None


@contextmanager
def _stage_attempt(
    manifest: ConversionManifest, stage: str, directory: Optional[str] = None
) -> Iterator[_Attempt]:
    """Run one attempt of ``stage``; the only place a stage is completed.

    The attempt is recorded as running, and saved, before anything is
    created; ``directory``, a workdir-relative attempt directory, is then
    created. The stage is completed with ``attempt.result`` only when the
    body returns normally having set one. Any exception -- ``KeyboardInterrupt``
    included, and a failure to complete -- records the attempt ``failed``
    (best effort) and propagates, so a failure never completes a stage.
    """
    manifest.begin_attempt(stage, directory)
    try:
        attempt = _Attempt(None if directory is None else manifest.artifact_path(directory))
        if attempt.directory is not None:
            attempt.directory.mkdir(parents=True)
        yield attempt
        if attempt.result is None:
            raise ConverterError(
                "{}: the {} attempt produced no result; refusing to complete it".format(
                    manifest.path, stage
                )
            )
        manifest.complete_stage(stage, attempt.result)
    except BaseException as exc:
        manifest.fail_stage(stage, exc)
        raise


def _skip_if_complete(
    manifest: ConversionManifest, stage: str, consume: Optional[str] = None
) -> bool:
    """Skip-trust for a complete stage; ``False``, doing nothing, otherwise.

    Every artifact of ``stage`` is verified by SHA-256 -- one the next stage
    deliberately consumed is accepted as consumed once that stage is
    complete -- and then the opt-in consumption of ``consume``, the stage's
    predecessor, runs.
    """
    if not manifest.is_complete(stage):
        return False
    position = STAGES.index(stage)
    later = STAGES[position + 1] if position + 1 < len(STAGES) else None
    manifest.verify_stage_artifacts(
        stage, allow_removed=later is not None and manifest.is_complete(later)
    )
    if consume is not None:
        manifest.consume_stage(consume, stage)
    return True


# ==========================================================================
# Ingest
# ==========================================================================


def _ingest_leftovers(manifest: ConversionManifest, n_registered: int) -> List[Path]:
    """Unregistered chunk files an interrupted ingest left behind.

    Only this stage's own names at or beyond the registered count qualify;
    any other entry in the manifest-owned ingest directory is refused rather
    than guessed about.
    """
    directory = manifest.artifact_path(INGEST_DIR)
    if not directory.exists():
        return []
    registered = set(manifest.stage("ingest")["artifacts"])
    leftovers = []
    for entry in sorted(directory.iterdir()):
        relpath = "{}/{}".format(INGEST_DIR, entry.name)
        if relpath in registered:
            continue
        match = _CHUNK_RE.match(entry.name)
        if (
            match is None
            or int(match.group(1)) < n_registered
            or entry.is_symlink()
            or not entry.is_file()
        ):
            raise ConverterError(
                "{}: unexpected entry in the manifest-owned ingest directory; refusing to "
                "touch it".format(entry)
            )
        leftovers.append(entry)
    return leftovers


def _write_chunk(path: Path, records: np.ndarray) -> None:
    """Write one chunk under a temporary name, fsync it and rename it into
    place. The caller registers (and so re-reads and hashes) it afterwards."""
    partial = path.with_name(path.name + _PARTIAL_SUFFIX)
    with open(partial, "xb", buffering=SCRATCH_BUFFER_BYTES) as handle:
        handle.write(records.view(np.uint8).reshape(-1))
        handle.flush()
        os.fsync(handle.fileno())
    os.replace(partial, path)


def run_ingest(
    workdir,
    *,
    schema: Optional[CanonicalSchema] = None,
    save_every_chunks: int = DEFAULT_SAVE_EVERY_CHUNKS,
) -> ConversionManifest:
    """Stream the source into verified canonical chunks, resuming from the
    last registered chunk. See the module docstring."""
    save_every_chunks = require_integer(
        save_every_chunks, "save_every_chunks", _RECORDED, minimum=1
    )
    manifest = _open_for_stage(workdir, schema)
    if _skip_if_complete(manifest, "ingest"):
        return manifest

    adapter_config = manifest.configuration["adapter"]
    source_format = adapter_config["source_format"]
    route = _route(source_format)
    adapter = None
    if not route.prepared:
        adapter = build_adapter(
            source_format, manifest.schema, adapter_config["parameters"], _max_snapshot(manifest)
        )
        _bind_adapter(manifest, adapter)
    registered = list(manifest.stage("ingest")["artifacts"])
    for relpath in registered:
        manifest.verify_artifact(relpath, "ingest chunk")
    leftovers = _ingest_leftovers(manifest, len(registered))

    with _stage_attempt(manifest, "ingest") as attempt:
        for path in leftovers:
            path.unlink()
        manifest.artifact_path(INGEST_DIR).mkdir(exist_ok=True)
        if route.prepared:
            prepared = _prepare_ascii(manifest)
            adapter = build_adapter(
                source_format,
                manifest.schema,
                adapter_config["parameters"],
                _max_snapshot(manifest),
                prepared_dir=prepared,
            )
            _bind_adapter(manifest, adapter)
            manifest.save()
        attempt.result = _stream_chunks(manifest, adapter, registered, save_every_chunks)
    return manifest


def _stream_chunks(
    manifest: ConversionManifest, adapter, registered: Sequence[str], save_every: int
) -> Dict[str, object]:
    schema = manifest.schema
    dtype = ingest_dtype(schema)
    n_snapshots = len(_snapshots(manifest))
    counts = np.zeros(n_snapshots, dtype=np.int64)
    max_rows = manifest.configuration["ingest_max_rows"]
    total = 0
    index = 0
    unsaved = 0
    source = "{} source ({})".format(
        manifest.configuration["adapter"]["source_format"],
        ", ".join(_adapter_source_paths_for_message(manifest)),
    )
    for batch in adapter.iter_batches(max_rows):
        if batch.schema.digest != schema.digest:
            raise ConverterError(
                "{} yielded a batch of schema {}, not {}".format(
                    source, batch.schema.digest, schema.digest
                )
            )
        batch.validate()
        n_rows = batch.n_rows
        if n_rows > max_rows:
            raise ConverterError(
                "{} yielded {} rows after being asked for at most {}".format(
                    source, n_rows, max_rows
                )
            )
        snaps = np.asarray(batch.payload["SnapNum"])
        if n_rows and int(snaps.max()) >= n_snapshots:
            raise ConverterError(
                "{}: batch {} carries SnapNum {} outside the {}-entry a_list".format(
                    source, index, int(snaps.max()), n_snapshots
                )
            )
        counts += np.bincount(snaps, minlength=n_snapshots)
        records = _batch_records(batch, dtype)
        digest = hashlib.sha256(records.view(np.uint8).reshape(-1)).hexdigest()
        if index < len(registered):
            entry = manifest.artifact(registered[index])
            if entry["sha256"] != digest or entry["n_rows"] != n_rows:
                raise ConverterError(
                    "{}: the {} now yields different rows for chunk {} ({} rows, sha256 {}) than "
                    "were ingested ({} rows, sha256 {}); resuming would duplicate or drop halos -- "
                    "refusing".format(
                        manifest.artifact_path(registered[index]),
                        source,
                        index,
                        n_rows,
                        digest,
                        entry["n_rows"],
                        entry["sha256"],
                    )
                )
        else:
            relpath = "{}/{}".format(INGEST_DIR, chunk_name(index))
            path = manifest.artifact_path(relpath)
            _write_chunk(path, records)
            ids = batch.identity["SourceHaloID"]
            entry = manifest.register_artifact(
                relpath,
                "ingest",
                "canonical-chunk",
                chunk=index,
                n_rows=n_rows,
                first_source_halo_id=int(ids[0]) if n_rows else None,
                last_source_halo_id=int(ids[-1]) if n_rows else None,
            )
            if entry["sha256"] != digest or entry["bytes"] != records.nbytes:
                raise ConverterError(
                    "{}: re-read as {} bytes with sha256 {} after writing {} bytes with sha256 "
                    "{}".format(path, entry["bytes"], entry["sha256"], records.nbytes, digest)
                )
            unsaved += 1
            if unsaved >= save_every:
                manifest.save()
                unsaved = 0
        total += n_rows
        index += 1
        del records, batch
    if index < len(registered):
        raise ConverterError(
            "{} now yields {} chunk(s) but {} were ingested; refusing to resume".format(
                source, index, len(registered)
            )
        )
    expected = manifest.inventory["selected_halos"]
    if total != expected:
        raise ConverterError(
            "{} yielded {} halos but its inventory selects {}; refusing to complete the "
            "ingest stage".format(source, total, expected)
        )
    if unsaved:
        manifest.save()
    return {"n_chunks": index, "n_rows": total, "snapshot_counts": counts.tolist()}


def _adapter_source_paths_for_message(manifest: ConversionManifest) -> List[str]:
    paths = [record["path"] for record in manifest.dependencies if SOURCE_ROLE in record["roles"]]
    return paths[:3] + (["..."] if len(paths) > 3 else [])


# ==========================================================================
# Transpose
# ==========================================================================


def _chunk_batches(manifest: ConversionManifest, max_rows: int) -> Iterator[CanonicalBatch]:
    """Every ingested chunk, in order, as batches of at most ``max_rows``
    rows. Each chunk is hashed as it is read and must match its registered
    SHA-256; a mismatch raises inside the transpose, which then removes
    everything it created."""
    schema = manifest.schema
    dtype = ingest_dtype(schema)
    for relpath in manifest.stage("ingest")["artifacts"]:
        entry = manifest.artifact(relpath)
        path = manifest.artifact_path(relpath)
        if entry is None or entry.get("status") != ARTIFACT_PRESENT:
            raise ConverterError(
                "{}: ingest chunk was consumed; the transpose cannot be re-run from this "
                "workdir".format(path)
            )
        n_rows = int(entry["n_rows"])
        expected_bytes = n_rows * dtype.itemsize
        try:
            size = path.stat().st_size
        except OSError as exc:
            raise ConverterError("{}: ingest chunk unreadable: {}".format(path, exc)) from exc
        if size != expected_bytes or size != entry["bytes"]:
            raise ConverterError(
                "{}: ingest chunk is {} bytes; {} rows of {} bytes require {}".format(
                    path, size, n_rows, dtype.itemsize, expected_bytes
                )
            )
        digest = hashlib.sha256()
        with open(path, "rb", buffering=0) as handle:
            remaining = n_rows
            while remaining:
                count = min(max_rows, remaining)
                records = np.empty(count, dtype=dtype)
                raw = memoryview(records.view(np.uint8).reshape(-1))
                got = read_into(handle, raw)
                if got != count * dtype.itemsize:
                    raise ConverterError(
                        "{}: ingest chunk yielded {} of {} bytes".format(
                            path, got, count * dtype.itemsize
                        )
                    )
                digest.update(raw)
                yield _batch_from_records(schema, records)
                remaining -= count
        if digest.hexdigest() != entry["sha256"]:
            raise ConverterError(
                "{}: ingest chunk read back with sha256 {} but was registered with {}; refusing "
                "to use it".format(path, digest.hexdigest(), entry["sha256"])
            )


def _attempt_directory(manifest: ConversionManifest, stage: str, root: str) -> str:
    return "{}/attempt_{:03d}".format(root, int(manifest.stage(stage)["attempt"]) + 1)


def _prepare_attempt(manifest: ConversionManifest, stage: str, root: str) -> str:
    """Discard an interrupted attempt and name the next attempt directory,
    which must not exist yet. The discard is the first mutation of a
    re-entered stage and runs only after every read-only check passed."""
    if manifest.stage(stage)["status"] in (STAGE_RUNNING, STAGE_FAILED):
        manifest.discard_attempt(stage)
    directory = _attempt_directory(manifest, stage, root)
    path = manifest.artifact_path(directory)
    if os.path.lexists(str(path)):
        raise ConverterError(
            "{}: an attempt directory the manifest never recorded already exists; refusing to "
            "touch it".format(path)
        )
    return directory


def run_transpose(
    workdir, *, schema: Optional[CanonicalSchema] = None, consume_ingest: bool = False
) -> ConversionManifest:
    """Transpose the verified ingest chunks into per-snapshot records.

    ``consume_ingest`` deletes the ingest chunks once the transpose is
    complete and its outputs re-verify; it is off by default.
    """
    consume = "ingest" if consume_ingest else None
    manifest = _open_for_stage(workdir, schema)
    manifest.require_complete("ingest")
    if _skip_if_complete(manifest, "transpose", consume):
        return manifest
    consumed = [
        relpath
        for relpath in manifest.stage("ingest")["artifacts"]
        if manifest.artifact(relpath).get("status") == ARTIFACT_REMOVED
    ]
    if consumed:
        raise ConverterError(
            "{}: ingest chunk(s) {} were consumed; the transpose cannot run from this workdir "
            "-- start a fresh one".format(manifest.path, consumed[:3])
        )

    directory = _prepare_attempt(manifest, "transpose", TRANSPOSE_DIR)
    with _stage_attempt(manifest, "transpose", directory) as attempt:
        result = transpose_module.transpose(
            manifest.schema,
            lambda max_rows: _chunk_batches(manifest, max_rows),
            _snapshots(manifest),
            attempt.directory,
            budget_bytes=manifest.configuration["transpose_budget_bytes"],
            spill_dir=attempt.directory,
        )
        attempt.result = _register_transposed(manifest, result, directory)
    if consume is not None:
        manifest.consume_stage(consume, "transpose")
    return manifest


def _register_transposed(manifest: ConversionManifest, result, directory: str) -> Dict:
    """Check a transpose result against expectations the transpose did not
    produce, then register every output. Raises naming the artifact."""
    schema = manifest.schema
    if result.schema_digest != schema.digest:
        raise ConverterError(
            "transpose ran for schema {}, not {}".format(result.schema_digest, schema.digest)
        )
    recorded_dtype = manifest.configuration["record_dtypes"]["transposed"]
    if canonical_json(dtype_descriptor(result.record_dtype)) != canonical_json(recorded_dtype):
        raise ConverterError("transpose output dtype differs from the recorded descriptor")
    ingest = manifest.stage("ingest")["result"]
    snapshots = _snapshots(manifest)
    if [entry.snapshot for entry in result.snapshots] != snapshots:
        raise ConverterError("transpose emitted snapshots other than the recorded a_list")
    if result.total_halos != ingest["n_rows"]:
        raise ConverterError(
            "transpose emitted {} halos but ingest registered {}".format(
                result.total_halos, ingest["n_rows"]
            )
        )
    out_dir = manifest.artifact_path(directory)
    expected_names = {transpose_module.output_name(s) for s in snapshots}
    present = {entry.name for entry in out_dir.iterdir()}
    if present != expected_names:
        raise ConverterError(
            "{}: attempt directory holds {} beyond/instead of the expected outputs".format(
                out_dir, sorted(present ^ expected_names)
            )
        )
    itemsize = result.record_dtype.itemsize
    for entry, expected in zip(result.snapshots, ingest["snapshot_counts"]):
        relpath = "{}/{}".format(directory, transpose_module.output_name(entry.snapshot))
        path = manifest.artifact_path(relpath)
        if Path(entry.path).resolve() != path.resolve():
            raise ConverterError("{}: transpose reported output {}".format(path, entry.path))
        if entry.n_halos != expected:
            raise ConverterError(
                "{}: snapshot {} holds {} halos but ingest counted {}".format(
                    path, entry.snapshot, entry.n_halos, expected
                )
            )
        size = path.stat().st_size
        if size != entry.n_halos * itemsize:
            raise ConverterError(
                "{}: {} bytes, but {} rows of {} bytes require {}".format(
                    path, size, entry.n_halos, itemsize, entry.n_halos * itemsize
                )
            )
        manifest.register_artifact(
            relpath,
            "transpose",
            "transposed-snapshot",
            snapshot=entry.snapshot,
            n_halos=entry.n_halos,
            first_global_position=entry.first_global_position,
        )
    return {
        "total_halos": result.total_halos,
        "n_links": dict(result.n_links),
        "n_gapped_descendants": result.n_gapped_descendants,
        "max_descendant_span": result.max_descendant_span,
        "links_adjacent": result.links_adjacent,
        "chain_rounds": result.chain_rounds,
        "peak_resident_bytes": result.peak_resident_bytes,
        "peak_spill_bytes": result.peak_spill_bytes,
        "sort_runs": dict(result.sort_runs),
        "sort_merge_passes": dict(result.sort_merge_passes),
    }


# ==========================================================================
# Write
# ==========================================================================


@dataclass(frozen=True)
class TransposedSnapshot:
    snapshot: int
    path: Path
    n_halos: int


@dataclass(frozen=True)
class WriteInputs:
    """Everything a writer needs, all verified before it is handed over."""

    schema: CanonicalSchema
    source_format: str
    snapshots: Tuple[int, ...]
    scale_factors: Tuple[float, ...]
    record_dtype: np.dtype
    transposed: Tuple[TransposedSnapshot, ...]
    transpose_result: Mapping
    configuration: Mapping
    #: Zero-argument callable returning a fresh iterator over every forest of
    #: the recorded inventory in ``ForestIndex`` order, as
    #: ``adapters.ctrees_hdf5.ForestRecord`` values -- the ``forests.h5``
    #: sidecar's rows. Lazy: the adapter is rebuilt read-only and bound to the
    #: recorded inventory only when a writer first calls it, so a writer that
    #: emits no sidecar never touches the source. ``None`` only for inputs a
    #: caller assembled by hand.
    forests: Optional[Callable[[], Iterator]] = None


class StageWriter(abc.ABC):
    """What the write stage asks of a writer (``hdf5_writer_v3.HorizontalV3Writer``
    is the v3 one).

    ``write`` creates its outputs only under ``out_dir`` -- a fresh,
    manifest-recorded attempt directory -- and returns every file it
    created. ``verify`` must re-read those files and check them against
    ``inputs`` before the stage may complete; the pipeline additionally
    requires the returned list to be exactly the directory's contents.
    """

    @property
    @abc.abstractmethod
    def identity(self) -> Mapping:
        """JSON-serialisable writer name/version, recorded with the stage."""

    @abc.abstractmethod
    def write(self, inputs: WriteInputs, out_dir: Path) -> Sequence[Path]:
        """Produce the stage outputs; return every file created."""

    @abc.abstractmethod
    def verify(self, inputs: WriteInputs, produced: Sequence[Path]) -> None:
        """Re-read and check every produced file; raise on any defect."""


def _bound_adapter(manifest: ConversionManifest) -> SourceAdapter:
    """The recorded adapter, rebuilt read-only and required to enumerate the
    recorded dependency set and inventory -- so every forest it reports is one
    this conversion's ``SourceHaloID`` and ``ForestIndex`` values were
    assigned against. The ASCII adapter reads its completed preparation."""
    adapter_config = manifest.configuration["adapter"]
    source_format = adapter_config["source_format"]
    prepared = None
    if _route(source_format).prepared:
        prepared = manifest.artifact_path(ASCII_PREPARATION_DIR)
        if not prepared.is_dir():
            raise ConverterError(
                "{}: the ASCII preparation directory is missing; the forest sidecar cannot be "
                "enumerated".format(prepared)
            )
    adapter = build_adapter(
        source_format,
        manifest.schema,
        adapter_config["parameters"],
        _max_snapshot(manifest),
        prepared_dir=prepared,
    )
    _bind_adapter(manifest, adapter)
    return adapter


def _forest_provider(manifest: ConversionManifest) -> Callable[[], Iterator]:
    """:attr:`WriteInputs.forests` for one manifest: the bound adapter is
    built on first call and reused, so a writer that enumerates the forests
    twice (once to write, once to verify) reads the inventory once. A build
    that fails is not cached; the next call tries again."""
    route = _route(manifest.configuration["adapter"]["source_format"])

    @functools.lru_cache(maxsize=None)
    def bound() -> SourceAdapter:
        return _bound_adapter(manifest)

    def forests() -> Iterator:
        return route.forests(bound())

    return forests


def _write_inputs(manifest: ConversionManifest) -> WriteInputs:
    snapshots = []
    for relpath in manifest.stage("transpose")["artifacts"]:
        entry = manifest.artifact(relpath)
        snapshots.append(
            TransposedSnapshot(
                snapshot=int(entry["snapshot"]),
                path=manifest.artifact_path(relpath),
                n_halos=int(entry["n_halos"]),
            )
        )
    configuration = manifest.configuration
    return WriteInputs(
        schema=manifest.schema,
        source_format=configuration["adapter"]["source_format"],
        snapshots=tuple(_snapshots(manifest)),
        scale_factors=tuple(configuration["snapshots"]["scale_factors"]),
        record_dtype=transpose_module.output_dtype(manifest.schema),
        transposed=tuple(snapshots),
        transpose_result=dict(manifest.stage("transpose")["result"]),
        configuration=configuration,
        forests=_forest_provider(manifest),
    )


def run_write(
    workdir,
    writer: StageWriter,
    *,
    schema: Optional[CanonicalSchema] = None,
    consume_transposed: bool = False,
) -> ConversionManifest:
    """Run a writer over the verified transposed snapshots.

    A complete write stage is re-verified and returned only for the writer
    identity that completed it; any other writer is refused.
    """
    if not isinstance(writer, StageWriter):
        raise ConverterError(
            "run_write needs a StageWriter, got {!r}".format(type(writer).__name__)
        )
    identity = _writer_identity(writer)
    consume = "transpose" if consume_transposed else None
    manifest = _open_for_stage(workdir, schema)
    manifest.require_complete("transpose")
    if manifest.is_complete("write"):
        completed_by = manifest.stage("write")["result"]["writer"]
        if canonical_json(completed_by) != canonical_json(identity):
            raise ConverterError(
                "{}: the write stage was completed by writer {}, not {}; refusing".format(
                    manifest.path, completed_by, identity
                )
            )
    if _skip_if_complete(manifest, "write", consume):
        return manifest
    # verify-before-consume: every transposed snapshot, in full, first
    manifest.verify_stage_artifacts("transpose")
    inputs = _write_inputs(manifest)

    directory = _prepare_attempt(manifest, "write", WRITE_DIR)
    with _stage_attempt(manifest, "write", directory) as attempt:
        out_dir = attempt.directory
        produced = _check_produced(out_dir, writer.write(inputs, out_dir))
        writer.verify(inputs, produced)
        for path in produced:
            manifest.register_artifact(manifest.relative(path), "write", "write-output")
        attempt.result = {"writer": identity, "n_files": len(produced)}
    if consume is not None:
        manifest.consume_stage(consume, "write")
    return manifest


def _writer_identity(writer: StageWriter) -> Dict[str, object]:
    identity = writer.identity
    if not isinstance(identity, Mapping) or not identity:
        raise ConverterError("a writer identity must be a nonempty mapping")
    try:
        return json.loads(canonical_json(dict(identity)))
    except (TypeError, ValueError) as exc:
        raise ConverterError("writer identity is not JSON-serialisable: {}".format(exc)) from exc


def _check_produced(out_dir: Path, produced) -> List[Path]:
    """The writer's reported outputs must be regular files strictly inside
    ``out_dir`` and exactly what the directory now holds. A symlink or an
    empty subdirectory anywhere under ``out_dir`` is a failed production."""
    resolved_dir = out_dir.resolve()
    reported = []
    for item in produced:
        path = Path(item)
        if path.is_symlink() or not path.is_file():
            raise ConverterError("{}: writer output is not a regular file".format(path))
        resolved = path.resolve()
        if resolved_dir not in resolved.parents:
            raise ConverterError(
                "{}: writer output lies outside its attempt directory {}".format(path, out_dir)
            )
        reported.append(resolved)
    if len(set(reported)) != len(reported):
        raise ConverterError("{}: writer reported an output twice".format(out_dir))
    on_disk = []
    for root, dirs, files in os.walk(resolved_dir):
        for name in dirs + files:
            candidate = Path(root) / name
            if candidate.is_symlink():
                raise ConverterError("{}: writer created a symlink".format(candidate))
        for name in dirs:
            candidate = Path(root) / name
            if not any(candidate.iterdir()):
                raise ConverterError("{}: writer left an empty directory".format(candidate))
        on_disk.extend(Path(root) / name for name in files)
    if sorted(on_disk) != sorted(reported):
        raise ConverterError(
            "{}: writer left undeclared or missing outputs: {}".format(
                out_dir, sorted(str(p) for p in set(on_disk) ^ set(reported))
            )
        )
    return sorted(reported)
