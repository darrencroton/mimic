"""Horizontal-HDF5 format version 3 emission for the generic converter pipeline.

Writes ``snapshot_NNN.h5`` and ``forests.h5`` per
docs/dev/HORIZONTAL-HDF5-FORMAT.md, section "Version 3" (File Set and Naming, Header
Attributes, Halo Datasets, The Schema Group, Forest Sidecar, Storage Layout;
contract C3 of docs/dev/MIMIC-CONVERTER-GENERALISATION-IMPLEMENTATION-PLAN.md).
:class:`HorizontalV3Writer` is the generic pipeline's write stage
(``pipeline.run_write``), and it never stamps anything but
``format_version = 3``. It shares only the header attribute table, the chunk
shapes and the header-metadata loader with the version 2 writer in
:mod:`hdf5_writer`, which never calls into this module.

The writer consumes the generic pipeline's verified transposed snapshots --
flat little-endian records whose fields ARE the v3 /halos columns
(``transpose.output_dtype``) -- and changes no value: it moves bytes from a
record column into an HDF5 dataset of the same declared type.

What it adds over the records is only what a whole-dataset view decides:

- ``links_adjacent``, measured here over Descendant links and required to
  agree with the transpose's own independent count;
- the run-scoped identity headers (``n_forests_total`` from the forest
  enumeration, ``max_halo_rank_in_forest`` measured over the records);
- the physical headers from simulation_info.yaml, validated with the same
  unit rules as v2 (``load_header_metadata``);
- ``/schema``, one subgroup per payload/extra declaration of the embedded
  schema, and the two v3 string headers.

Memory is one block of ``block_rows`` records and one block of forests at a
time; nothing is O(halos) or O(forests). Every emitted file is re-opened and
verified against its source records before the stage reports it.
"""

import os
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, Iterable, Iterator, List, Mapping, Sequence, Tuple

import h5py
import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from column_schema import (  # noqa: E402
    EXTRA_TYPES,
    IDENTITY_FIELDS,
    TOPOLOGY_FIELDS,
    CanonicalSchema,
    PayloadField,
)
from conversion_manifest import sha256_file  # noqa: E402
from ctrees_parser import ConverterError  # noqa: E402
from fixups import REF_TO_NATIVE_MASS  # noqa: E402
from hdf5_writer import (  # noqa: E402
    CHUNK_1D,
    CHUNK_VEC,
    HEADER_ATTRS,
    _log,
    load_header_metadata,
    snapshot_h5_name,
)
from pipeline import StageWriter, WriteInputs  # noqa: E402

#: The format version this module writes. Never 2: a v3 dataset is not a v2
#: dataset with more columns, and stamping it 2 is not a rollback path.
V3_FORMAT_VERSION = 3

#: The two v3 header strings: fixed-length ASCII, byte length (C3).
V3_STRING_ATTRS = {"source_format": 32, "column_mapping_sha256": 64}

#: The attributes every /schema subgroup carries, exactly (C3).
V3_SCHEMA_ATTRS = ("type", "units", "h_convention", "description")

#: The forests.h5 datasets, all int64 at the file root, in this order (C3).
V3_SIDECAR_DATASETS = ("ForestID", "SourceFileOrdinal", "SourceUnitOrdinal")

#: Records the writer holds at once when streaming a transposed snapshot, and
#: forests when streaming the sidecar. Four HDF5 chunks: writes stay
#: chunk-aligned, and at ~150 B/record the block is ~40 MB.
V3_WRITE_BLOCK_ROWS = 4 * CHUNK_1D[0]

SIDECAR_NAME = "forests.h5"

#: How each route's refusal names the conversion that recorded a
#: ``simulation_info`` other than the writer's.
_RECORDED_METADATA = {
    "consistent_trees_ascii": "the ASCII conversion was prepared",
    "consistent_trees_hdf5": "the forests-HDF5 conversion was recorded",
    "lhalo_binary": "the L-Halo conversion was recorded",
}


def _v3_storage(type_name: str) -> Tuple[np.dtype, bool]:
    spec = EXTRA_TYPES[type_name]
    return np.dtype(spec.numpy_dtype).newbyteorder("<"), spec.n_components == 3


def v3_halo_datasets(schema: CanonicalSchema) -> Dict[str, Tuple[np.dtype, bool]]:
    """The /halos datasets of a v3 file for ``schema``, in format-table order:
    name -> (explicit little-endian dtype, is_vec3).

    Five int64 links, three int32 target snapshots, three int64 identities
    (fixed by the format table), then every payload field and selected extra in
    its declared type.
    """
    table: Dict[str, Tuple[np.dtype, bool]] = {}
    for field in TOPOLOGY_FIELDS + IDENTITY_FIELDS:
        table[field.name] = _v3_storage(field.type)
    for declaration in schema.output_field_declarations():
        table[declaration.name] = _v3_storage(declaration.type)
    return table


def _fixed_ascii(value: str, size: int, what: str) -> np.ndarray:
    try:
        encoded = str(value).encode("ascii")
    except UnicodeEncodeError:
        raise ConverterError("header {} {!r} is not ASCII".format(what, value)) from None
    if len(encoded) > size:
        raise ConverterError(
            "header {} {!r} is {} bytes; the format fixes it at {}".format(
                what, value, len(encoded), size
            )
        )
    return np.array(encoded, dtype="S{}".format(size))


@dataclass(frozen=True)
class V3Measurement:
    """Whole-dataset facts the writer measures over the transposed records
    before stamping any header, and re-measures when it verifies."""

    counts: Tuple[int, ...]
    n_gapped_descendants: int
    max_halo_rank_in_forest: int
    max_forest_index: int

    @property
    def links_adjacent(self) -> int:
        return int(self.n_gapped_descendants == 0)

    @property
    def total_halos(self) -> int:
        return int(sum(self.counts))


def _transposed_blocks(path: Path, dtype: np.dtype, n_halos: int, rows: int) -> Iterator:
    """One transposed snapshot in blocks of at most ``rows`` records, read
    with plain file reads (never a whole-file map), after checking that the
    file holds exactly ``n_halos`` records."""
    expected = n_halos * dtype.itemsize
    size = os.path.getsize(path)
    if size != expected:
        raise ConverterError(
            "{}: {} bytes, but {} records of {} bytes require {}".format(
                path, size, n_halos, dtype.itemsize, expected
            )
        )
    with open(path, "rb") as handle:
        remaining = n_halos
        while remaining:
            count = min(rows, remaining)
            block = np.fromfile(handle, dtype=dtype, count=count)
            if block.size != count:
                raise ConverterError("{}: read {} of {} records".format(path, block.size, count))
            yield block
            remaining -= count


def _forest_blocks(records: Iterable, rows: int) -> Iterator[Dict[str, np.ndarray]]:
    """The forest enumeration as sidecar column blocks, requiring every
    record's ``forest_index`` to be its position: sidecar row ``f`` must be
    the forest whose halos carry ``ForestIndex == f``."""
    columns: Dict[str, List[int]] = {name: [] for name in V3_SIDECAR_DATASETS}
    position = 0

    def flush() -> Dict[str, np.ndarray]:
        block = {name: np.asarray(values, dtype="<i8") for name, values in columns.items()}
        for values in columns.values():
            values.clear()
        return block

    for record in records:
        if int(record.forest_index) != position:
            raise ConverterError(
                "forest enumeration yields ForestIndex {} at sidecar row {}; the sidecar must be "
                "in dense ForestIndex order".format(record.forest_index, position)
            )
        columns["ForestID"].append(int(record.forest_id))
        columns["SourceFileOrdinal"].append(int(record.source_file_ordinal))
        columns["SourceUnitOrdinal"].append(int(record.unit_ordinal))
        position += 1
        if len(columns["ForestID"]) >= rows:
            yield flush()
    if columns["ForestID"]:
        yield flush()


def write_v3_snapshot_file(
    path,
    header: Mapping[str, object],
    table: Mapping[str, Tuple[np.dtype, bool]],
    declarations: Sequence[PayloadField],
    n_halos: int,
    blocks: Iterable[np.ndarray],
) -> None:
    """Create one v3 ``snapshot_NNN.h5`` holding exactly /header, /halos and
    /schema, streaming ``blocks`` (structured records carrying every
    ``table`` column) into datasets pre-sized to ``n_halos`` rows.

    The file is created exclusively (``w-``): the write stage's attempt
    directory is fresh, and nothing here ever overwrites an existing file.
    """
    with h5py.File(path, "w-", libver="latest") as handle:
        header_group = handle.create_group("header")
        for name, dtype in HEADER_ATTRS.items():
            header_group.attrs.create(name, header[name], dtype=dtype)
        for name, size in V3_STRING_ATTRS.items():
            header_group.attrs.create(
                name, _fixed_ascii(header[name], size, name), dtype=h5py.string_dtype("ascii", size)
            )
        halos = handle.create_group("halos")
        datasets = {}
        for name, (dtype, is_vec) in table.items():
            datasets[name] = halos.create_dataset(
                name,
                shape=(n_halos, 3) if is_vec else (n_halos,),
                dtype=dtype,
                chunks=CHUNK_VEC if is_vec else CHUNK_1D,
                maxshape=(None, 3) if is_vec else (None,),
                compression=None,
            )
        written = 0
        for block in blocks:
            stop = written + int(block.size)
            if stop > n_halos:
                raise ConverterError(
                    "{}: more than the {} declared rows were streamed".format(path, n_halos)
                )
            for name, dataset in datasets.items():
                dataset[written:stop] = np.ascontiguousarray(block[name], dtype=table[name][0])
            written = stop
        if written != n_halos:
            raise ConverterError("{}: {} rows streamed, {} declared".format(path, written, n_halos))
        schema_group = handle.create_group("schema")
        utf8 = h5py.string_dtype("utf-8")
        for declaration in declarations:
            group = schema_group.create_group(declaration.name)
            for key in V3_SCHEMA_ATTRS:
                group.attrs.create(key, getattr(declaration, key), dtype=utf8)


def write_v3_sidecar(path, blocks: Iterable[Mapping[str, np.ndarray]]) -> int:
    """Create ``forests.h5``: exactly the three int64 root datasets, grown
    block by block. Returns the forest count."""
    total = 0
    with h5py.File(path, "w-", libver="latest") as handle:
        datasets = {
            name: handle.create_dataset(
                name, shape=(0,), dtype="<i8", chunks=CHUNK_1D, maxshape=(None,), compression=None
            )
            for name in V3_SIDECAR_DATASETS
        }
        for block in blocks:
            count = int(block["ForestID"].size)
            for name, dataset in datasets.items():
                dataset.resize((total + count,))
                dataset[total : total + count] = block[name]
            total += count
    return total


def _require_block_rows(value) -> int:
    if isinstance(value, bool) or not isinstance(value, (int, np.integer)) or int(value) < 1:
        raise ConverterError("block_rows must be a positive integer, got {!r}".format(value))
    return int(value)


class HorizontalV3Writer(StageWriter):
    """The generic pipeline's v3 write stage (``pipeline.run_write``).

    ``simulation_info_path`` supplies the five physical header values, under
    the same explicit unit validation v2 applies. Its content hash and the
    values themselves are part of the writer :attr:`identity`, which the
    manifest records with the completed stage: re-running the stage with
    different physical metadata is refused rather than silently re-labelled.
    Where the recorded conversion already pinned physical metadata -- a
    ``simulation_info`` file on any route that recorded one (always for
    ASCII, optionally for L-Halo and forests-HDF5), and the forests-HDF5
    route's ``particle_mass`` -- the writer requires agreement before writing.

    ``verify`` re-opens every file and compares every dataset, header,
    declaration and sidecar row against the inputs, so an emitted file that
    differs from its records in any column -- an extra included -- fails the
    stage before it is registered, and before any transposed input can be
    consumed.
    """

    def __init__(self, simulation_info_path, *, block_rows: int = V3_WRITE_BLOCK_ROWS):
        path = Path(simulation_info_path)
        before = sha256_file(path)
        self._metadata = load_header_metadata(path)
        if sha256_file(path) != before:
            raise ConverterError("{}: changed while it was being read".format(path))
        self._simulation_info_sha256 = before
        self._particle_mass = self._metadata["particle_mass_msun_h"]
        self.block_rows = _require_block_rows(block_rows)

    @property
    def identity(self) -> Mapping:
        return {
            "writer": "horizontal-hdf5",
            "format_version": V3_FORMAT_VERSION,
            "simulation_info_sha256": self._simulation_info_sha256,
            "header": dict(self._metadata),
        }

    # ---- input checks ----------------------------------------------------

    def _check_inputs(self, inputs: WriteInputs) -> Dict[str, Tuple[np.dtype, bool]]:
        """Everything about ``inputs`` that must hold before a byte is
        written; returns the /halos table."""
        schema = inputs.schema
        if inputs.source_format != schema.source_format:
            raise ConverterError(
                "write inputs name source format {!r} for a {!r} schema".format(
                    inputs.source_format, schema.source_format
                )
            )
        n_snapshots = len(inputs.scale_factors)
        if tuple(inputs.snapshots) != tuple(range(n_snapshots)):
            raise ConverterError(
                "v3 files are named by a_list position; snapshots {} are not 0..{}".format(
                    list(inputs.snapshots)[:5], n_snapshots - 1
                )
            )
        if tuple(entry.snapshot for entry in inputs.transposed) != tuple(inputs.snapshots):
            raise ConverterError("transposed snapshots do not cover the a_list in order")
        if inputs.forests is None:
            raise ConverterError("write inputs carry no forest enumeration for forests.h5")
        table = v3_halo_datasets(schema)
        record = np.dtype(inputs.record_dtype)
        if tuple(record.names or ()) != tuple(table):
            raise ConverterError(
                "transposed records carry fields {} but a v3 file of schema {} needs {}".format(
                    record.names, schema.digest, tuple(table)
                )
            )
        for name, (dtype, is_vec) in table.items():
            field = record.fields[name][0]
            base = field.base if field.shape else field
            if base.str != dtype.str or field.shape != ((3,) if is_vec else ()):
                raise ConverterError(
                    "transposed field {} is {} {}; the v3 table needs {}{}".format(
                        name, base.str, field.shape, dtype.str, " [3]" if is_vec else ""
                    )
                )
        self._check_configuration(inputs)
        return table

    def _check_configuration(self, inputs: WriteInputs) -> None:
        """Refuse to write physical headers the recorded conversion did not
        use: a recorded ``simulation_info`` must be this writer's file by
        content, on every route that recorded one, and the forests-HDF5
        route's ``particle_mass`` (from which it derived Len) must equal the
        header's."""
        parameters = inputs.configuration["adapter"]["parameters"]
        if inputs.source_format == "consistent_trees_hdf5":
            recorded = float(parameters["particle_mass"]) * REF_TO_NATIVE_MASS
            if recorded != self._particle_mass:
                raise ConverterError(
                    "the conversion derived Len with particle_mass {} (1e10 Msun/h) but the "
                    "simulation_info header would record {} Msun/h; refusing to write a header "
                    "that disagrees with the payload".format(
                        parameters["particle_mass"], self._particle_mass
                    )
                )
        if "simulation_info" in parameters:
            recorded = sha256_file(parameters["simulation_info"])
            if recorded != self._simulation_info_sha256:
                raise ConverterError(
                    "{}: {} against a different simulation_info (sha256 {}) than the writer "
                    "was given ({}); refusing to mix metadata".format(
                        parameters["simulation_info"],
                        _RECORDED_METADATA.get(inputs.source_format, "the conversion was recorded"),
                        recorded,
                        self._simulation_info_sha256,
                    )
                )

    def _measure(self, inputs: WriteInputs) -> V3Measurement:
        """One bounded pass over the transposed records: per-snapshot counts,
        descendant gaps (in a_list positions), maximum rank and ForestIndex.
        The gap count must equal the transpose's own, which it measured from
        source keys rather than from these rows."""
        counts = []
        gapped = 0
        max_rank = -1
        max_forest = -1
        dtype = np.dtype(inputs.record_dtype)
        for entry in inputs.transposed:
            counts.append(int(entry.n_halos))
            snap = int(entry.snapshot)
            for block in _transposed_blocks(entry.path, dtype, entry.n_halos, self.block_rows):
                target = block["DescendantSnapshot"].astype(np.int64)
                linked = target != -1
                span = target[linked] - snap
                # span > 1 is the transpose's definition of a gap (source_keys.py)
                gapped += int(np.count_nonzero(span > 1))
                if block.size:
                    max_rank = max(max_rank, int(block["HaloRankInForest"].max()))
                    max_forest = max(max_forest, int(block["ForestIndex"].max()))
        measured = V3Measurement(tuple(counts), gapped, max_rank, max_forest)
        result = inputs.transpose_result
        if measured.total_halos != int(result["total_halos"]):
            raise ConverterError(
                "the transposed snapshots hold {} halos but the transpose recorded {}".format(
                    measured.total_halos, result["total_halos"]
                )
            )
        if measured.n_gapped_descendants != int(result["n_gapped_descendants"]) or bool(
            measured.links_adjacent
        ) != bool(result["links_adjacent"]):
            raise ConverterError(
                "measured {} gapped descendant link(s) (links_adjacent {}) but the transpose "
                "recorded {} (links_adjacent {}); refusing to stamp links_adjacent".format(
                    measured.n_gapped_descendants,
                    measured.links_adjacent,
                    result["n_gapped_descendants"],
                    result["links_adjacent"],
                )
            )
        return measured

    def _header(
        self, inputs: WriteInputs, measured: V3Measurement, n_forests: int, snap: int
    ) -> Dict[str, object]:
        if measured.max_forest_index >= n_forests:
            raise ConverterError(
                "a halo carries ForestIndex {} but the forest enumeration holds only {} "
                "forest(s)".format(measured.max_forest_index, n_forests)
            )
        header: Dict[str, object] = {
            "format_version": V3_FORMAT_VERSION,
            "links_adjacent": measured.links_adjacent,
            "scale_factor": float(inputs.scale_factors[snap]),
            "snapshot_number": snap,
            "n_halos": measured.counts[snap],
            "n_forests_total": n_forests,
            "max_halo_rank_in_forest": measured.max_halo_rank_in_forest,
            "source_format": inputs.schema.source_format,
            "column_mapping_sha256": inputs.schema.digest,
        }
        header.update(self._metadata)
        return header

    # ---- StageWriter -----------------------------------------------------

    def write(self, inputs: WriteInputs, out_dir: Path) -> Sequence[Path]:
        table = self._check_inputs(inputs)
        measured = self._measure(inputs)
        out_dir = Path(out_dir)
        sidecar = out_dir / SIDECAR_NAME
        n_forests = write_v3_sidecar(sidecar, _forest_blocks(inputs.forests(), self.block_rows))
        declarations = inputs.schema.output_field_declarations()
        dtype = np.dtype(inputs.record_dtype)
        produced = []
        for entry in inputs.transposed:
            snap = int(entry.snapshot)
            path = out_dir / snapshot_h5_name(snap)
            write_v3_snapshot_file(
                path,
                self._header(inputs, measured, n_forests, snap),
                table,
                declarations,
                int(entry.n_halos),
                _transposed_blocks(entry.path, dtype, entry.n_halos, self.block_rows),
            )
            produced.append(path)
        produced.append(sidecar)
        _log(
            "write: {} v3 snapshot file(s) and {} ({} forest(s), {} halo(s), links_adjacent {}) "
            "in {}".format(
                len(inputs.transposed),
                SIDECAR_NAME,
                n_forests,
                measured.total_halos,
                measured.links_adjacent,
                out_dir,
            )
        )
        return produced

    def verify(self, inputs: WriteInputs, produced: Sequence[Path]) -> None:
        table = self._check_inputs(inputs)
        measured = self._measure(inputs)
        by_name = {Path(path).name: Path(path) for path in produced}
        expected = {snapshot_h5_name(s) for s in inputs.snapshots} | {SIDECAR_NAME}
        if set(by_name) != expected or len(by_name) != len(produced):
            raise ConverterError(
                "write produced {} but a v3 dataset is exactly {}".format(
                    sorted(by_name), sorted(expected)
                )
            )
        n_forests = _verify_v3_sidecar(
            by_name[SIDECAR_NAME], _forest_blocks(inputs.forests(), self.block_rows)
        )
        declarations = inputs.schema.output_field_declarations()
        dtype = np.dtype(inputs.record_dtype)
        for entry in inputs.transposed:
            snap = int(entry.snapshot)
            _verify_v3_snapshot_file(
                by_name[snapshot_h5_name(snap)],
                self._header(inputs, measured, n_forests, snap),
                table,
                declarations,
                int(entry.n_halos),
                _transposed_blocks(entry.path, dtype, entry.n_halos, self.block_rows),
            )


def _require_hard_links(group, path: Path) -> None:
    for name in group:
        link = group.get(name, getlink=True)
        if not isinstance(link, h5py.HardLink):
            raise ConverterError(
                "{}: {}/{} is a {}, not a physically present object".format(
                    path, group.name.rstrip("/"), name, type(link).__name__
                )
            )


def _verify_v3_dataset_layout(dataset, dtype: np.dtype, is_vec: bool, n_rows: int, path) -> None:
    shape = (n_rows, 3) if is_vec else (n_rows,)
    problems = []
    if dataset.dtype.str != dtype.str:
        problems.append("dtype {} != {}".format(dataset.dtype.str, dtype.str))
    if dataset.shape != shape:
        problems.append("shape {} != {}".format(dataset.shape, shape))
    if dataset.chunks != (CHUNK_VEC if is_vec else CHUNK_1D):
        problems.append("chunks {}".format(dataset.chunks))
    if dataset.compression is not None or dataset.shuffle or dataset.fletcher32:
        problems.append("filtered")
    if dataset.scaleoffset is not None:
        problems.append("scale-offset")
    if dataset.external or dataset.is_virtual:
        problems.append("not stored in this file")
    if dataset.attrs.keys():
        problems.append("carries attributes {}".format(sorted(dataset.attrs.keys())))
    if problems:
        raise ConverterError("{}: {}: {}".format(path, dataset.name, "; ".join(problems)))


def _verify_v3_snapshot_file(
    path: Path,
    header: Mapping[str, object],
    table: Mapping[str, Tuple[np.dtype, bool]],
    declarations: Sequence[PayloadField],
    n_halos: int,
    blocks: Iterable[np.ndarray],
) -> None:
    """Re-read one written snapshot and compare everything with what it
    should hold; raise on the first difference."""
    with h5py.File(path, "r") as handle:
        if set(handle.keys()) != {"header", "halos", "schema"} or handle.attrs.keys():
            raise ConverterError(
                "{}: root holds {} (attributes {}), not exactly /header, /halos, /schema".format(
                    path, sorted(handle.keys()), sorted(handle.attrs.keys())
                )
            )
        _require_hard_links(handle, path)
        attrs = handle["header"].attrs
        expected_names = set(HEADER_ATTRS) | set(V3_STRING_ATTRS)
        if set(attrs.keys()) != expected_names:
            raise ConverterError(
                "{}: header attributes {} != {}".format(
                    path, sorted(attrs.keys()), sorted(expected_names)
                )
            )
        for name, dtype in HEADER_ATTRS.items():
            stored = np.asarray(attrs[name])
            if stored.dtype != np.dtype(dtype) or stored.shape != () or stored != header[name]:
                raise ConverterError(
                    "{}: header {} is {!r} ({}), expected {!r}".format(
                        path, name, stored, stored.dtype, header[name]
                    )
                )
        for name, size in V3_STRING_ATTRS.items():
            kind = attrs.get_id(name).get_type()
            if (
                not isinstance(kind, h5py.h5t.TypeStringID)
                or kind.is_variable_str()
                or kind.get_size() != size
                or kind.get_cset() != h5py.h5t.CSET_ASCII
            ):
                raise ConverterError(
                    "{}: header {} is not {}-byte fixed ASCII".format(path, name, size)
                )
            if attrs[name] != str(header[name]).encode("ascii"):
                raise ConverterError(
                    "{}: header {} is {!r}, expected {!r}".format(
                        path, name, attrs[name], header[name]
                    )
                )

        halos = handle["halos"]
        if set(halos.keys()) != set(table) or halos.attrs.keys():
            raise ConverterError(
                "{}: /halos holds {} (attributes {}), expected exactly {}".format(
                    path, sorted(halos.keys()), sorted(halos.attrs.keys()), sorted(table)
                )
            )
        _require_hard_links(halos, path)
        for name, (dtype, is_vec) in table.items():
            _verify_v3_dataset_layout(halos[name], dtype, is_vec, n_halos, path)
        start = 0
        for block in blocks:
            stop = start + int(block.size)
            for name, (dtype, _is_vec) in table.items():
                stored = halos[name][start:stop]
                expected = np.ascontiguousarray(block[name], dtype=dtype)
                if stored.shape != expected.shape or stored.tobytes() != expected.tobytes():
                    raise ConverterError(
                        "{}: /halos/{} rows [{}, {}) differ from the transposed records".format(
                            path, name, start, stop
                        )
                    )
            start = stop
        if start != n_halos:
            raise ConverterError(
                "{}: compared {} rows, the snapshot holds {}".format(path, start, n_halos)
            )

        schema_group = handle["schema"]
        wanted = {declaration.name: declaration for declaration in declarations}
        if set(schema_group.keys()) != set(wanted) or schema_group.attrs.keys():
            raise ConverterError(
                "{}: /schema declares {}, expected exactly {}".format(
                    path, sorted(schema_group.keys()), sorted(wanted)
                )
            )
        _require_hard_links(schema_group, path)
        for name, declaration in wanted.items():
            group = schema_group[name]
            if (
                not isinstance(group, h5py.Group)
                or len(group)
                or (set(group.attrs.keys()) != set(V3_SCHEMA_ATTRS))
            ):
                raise ConverterError(
                    "{}: /schema/{} must be a childless group with exactly {}".format(
                        path, name, V3_SCHEMA_ATTRS
                    )
                )
            for key in V3_SCHEMA_ATTRS:
                kind = group.attrs.get_id(key).get_type()
                if (
                    not isinstance(kind, h5py.h5t.TypeStringID)
                    or not kind.is_variable_str()
                    or kind.get_cset() != h5py.h5t.CSET_UTF8
                    or group.attrs[key] != getattr(declaration, key)
                ):
                    raise ConverterError(
                        "{}: /schema/{} {} is {!r}, expected UTF-8 {!r}".format(
                            path, name, key, group.attrs[key], getattr(declaration, key)
                        )
                    )


def _verify_v3_sidecar(path: Path, blocks: Iterable[Mapping[str, np.ndarray]]) -> int:
    """Re-read forests.h5 against a fresh forest enumeration; returns the
    forest count."""
    with h5py.File(path, "r") as handle:
        if set(handle.keys()) != set(V3_SIDECAR_DATASETS) or handle.attrs.keys():
            raise ConverterError(
                "{}: holds {} (attributes {}), not exactly {}".format(
                    path, sorted(handle.keys()), sorted(handle.attrs.keys()), V3_SIDECAR_DATASETS
                )
            )
        _require_hard_links(handle, path)
        lengths = {int(handle[name].shape[0]) for name in V3_SIDECAR_DATASETS}
        if len(lengths) != 1:
            raise ConverterError("{}: sidecar datasets differ in length".format(path))
        (n_forests,) = lengths
        for name in V3_SIDECAR_DATASETS:
            _verify_v3_dataset_layout(handle[name], np.dtype("<i8"), False, n_forests, path)
        start = 0
        for block in blocks:
            stop = start + int(block["ForestID"].size)
            for name in V3_SIDECAR_DATASETS:
                if handle[name][start:stop].tobytes() != block[name].tobytes():
                    raise ConverterError(
                        "{}: /{} rows [{}, {}) differ from the forest enumeration".format(
                            path, name, start, stop
                        )
                    )
            start = stop
        if start != n_forests:
            raise ConverterError(
                "{}: holds {} forests, the enumeration {}".format(path, n_forests, start)
            )
    return n_forests
