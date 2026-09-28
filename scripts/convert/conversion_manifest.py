"""Schema-bound, adapter-neutral stage state for generic conversions
(contract C4 of docs/dev/MIMIC-CONVERTER-GENERALISATION-IMPLEMENTATION-PLAN.md).

**What this owns, and what it does not.** One JSON manifest per generic
workdir, ``manifest.json``, at ``manifest_version = 3``. It freezes the whole
conversion's configuration -- adapter identity and parameters, the complete
canonical schema and its ``column_mapping_sha256``, the binary source layout
identity, every record dtype with its shapes, the snapshot list and the
source inventory -- pins every source dependency, and records the explicit
``ingest`` -> ``transpose`` -> ``write`` stage state with a SHA-256 content
checksum for every artifact a stage produced. Performance-only settings that
cannot change any output (the ASCII preparation's worker count and parser
chunk size) are recorded beside the configuration, under ``tuning``, and are
not part of its identity or digest. It does not run a stage:
``pipeline.py`` does, through the transitions below. ``scatter.Manifest``
remains the owner of the legacy ASCII-to-v2 state; nothing here reads, writes
or upgrades one.

**The file name is shared with the legacy manifest on purpose.** A legacy
loader pointed at a generic workdir reads ``manifest_version = 3`` and refuses
it (``scatter.Manifest.load_or_create`` accepts exactly 2), and this module
pointed at a legacy workdir recognises version 2 and hands it back untouched
(:func:`open_manifest`). A distinct name would let either tool treat the
other's workdir as empty and start writing into it. Version 1 and every
unknown version fail in both.

**Resume derives everything from the manifest.** :meth:`ConversionManifest.load`
rebuilds the schema from its embedded canonical record through the real
profile parser and ``build_schema`` (:func:`schema_from_record`) and requires
the rebuilt digest to equal the recorded one, so a resume never needs the
profile file -- or, for L-Halo, the ``halo_properties.yaml`` -- that created
the workdir, and a code change that would re-read the same profile
differently is refused instead of silently mixing two schemas in one
workdir. A caller that *does* name a schema (a CLI given ``--column-map``
again) must name the recorded one exactly: same width is not enough, since
two schemas of equal record size can differ in type, units, source component
or h convention (:meth:`ConversionManifest.require_schema`).

**Source content evidence.** A source dependency is pinned by its resolved
path, size, ``mtime_ns`` and ``(st_dev, st_ino)`` -- the evidence the legacy
scatter and the forests-HDF5 adapter already use -- and metadata files the
conversion embeds (the a_list) additionally by SHA-256. Bulk source files are
not content-hashed: a full-Uchuu data file is ~44 GB and is re-checked on
every stage entry. :meth:`ConversionManifest.verify_dependencies` re-pins every
recorded path before a stage mutates anything and names the first one that
moved.

**Shape.** :meth:`ConversionManifest.load` checks the shape of every record
outside the digest-protected configuration that the stages, the validator
and the report index -- stage records, artifact entries, dependency records,
the inventory and ``tuning`` -- and that every artifact a stage lists is
registered to that stage, so a hand-edited or partially corrupted manifest is
refused naming the manifest path and the offending key, never with a bare
``KeyError`` or ``TypeError`` from whichever reader touched it first.

**Containment.** Every artifact path is recorded relative to the workdir and
resolved back strictly inside it; symlinks are refused rather than followed.
Deleting an artifact happens only through :meth:`consume_stage`, which is
verify-then-delete (its single unlink is ``_unlink_artifact``), and a
stage's abandoned attempt directory -- created only after the manifest has
recorded it, so everything under it is that attempt's own unverified output --
is the only directory this module ever removes (:meth:`discard_attempt`).
Source files are never opened for writing, and a workdir that contains a
source dependency is refused, so no cleanup path can reach one. The
manifest's own temporary file is opened without following a symlink.
"""

import hashlib
import json
import math
import os
import reprlib
import shutil
import stat
from pathlib import Path
from typing import Dict, Iterable, List, Mapping, Optional, Sequence, Tuple

import numpy as np
from column_schema import (
    CanonicalSchema,
    ConverterError,
    SourceProperty,
    build_schema,
    parse_column_map,
)
from scatter import MANIFEST_NAME
from scatter import MANIFEST_VERSION as LEGACY_MANIFEST_VERSION
from scatter import Manifest as LegacyManifest

__all__ = [
    "ARTIFACT_PRESENT",
    "ARTIFACT_REMOVED",
    "LEGACY_MANIFEST_VERSION",
    "MANIFEST_KIND",
    "MANIFEST_NAME",
    "MANIFEST_VERSION",
    "MANIFEST_ABSENT",
    "MANIFEST_GENERIC",
    "MANIFEST_LEGACY",
    "SOURCE_ROLE",
    "STAGES",
    "STAGE_COMPLETE",
    "STAGE_FAILED",
    "STAGE_PENDING",
    "STAGE_RUNNING",
    "ConversionManifest",
    "canonical_json",
    "canonical_sha256",
    "classify_manifest",
    "dtype_descriptor",
    "dtype_from_descriptor",
    "finite_float",
    "inventory_record",
    "merge_dependencies",
    "open_manifest",
    "pin_dependency",
    "schema_from_record",
    "schema_record",
    "sha256_file",
]

#: The generic manifest's version. 1 was the legacy fix_flybys-era scatter
#: manifest and 2 is the current legacy one (``scatter.MANIFEST_VERSION``), so
#: the generic format starts at 3 and never shares a number with either.
MANIFEST_VERSION = 3

#: Distinguishes a generic manifest from any other JSON that might claim
#: version 3; a version-3 document without it is unknown, not generic.
MANIFEST_KIND = "mimic-generic-conversion"

MANIFEST_ABSENT = "absent"
MANIFEST_GENERIC = "generic"
MANIFEST_LEGACY = "legacy"

#: The explicit stages, in the only order they may complete.
STAGES = ("ingest", "transpose", "write")

STAGE_PENDING = "pending"
STAGE_RUNNING = "running"
STAGE_FAILED = "failed"
STAGE_COMPLETE = "complete"
_STAGE_STATUSES = (STAGE_PENDING, STAGE_RUNNING, STAGE_FAILED, STAGE_COMPLETE)

#: The dependency role of a bulk source file the adapter reads (as opposed to
#: metadata such as the a_list or simulation_info).
SOURCE_ROLE = "source"

ARTIFACT_PRESENT = "present"
ARTIFACT_REMOVED = "removed"

_TOP_LEVEL_KEYS = (
    "artifacts",
    "configuration",
    "configuration_sha256",
    "manifest_kind",
    "manifest_version",
    "sources",
    "stages",
    "tuning",
)

_STAGE_KEYS = ("artifacts", "attempt", "directory", "error", "result", "status")
_SOURCES_KEYS = ("dependencies", "inventory")

_HASH_BLOCK_BYTES = 8 * 1024 * 1024


# ==========================================================================
# Hashing and canonical JSON
# ==========================================================================


def sha256_file(path, blocksize: int = _HASH_BLOCK_BYTES) -> str:
    """Streamed SHA-256 of a file's contents."""
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        while True:
            block = handle.read(blocksize)
            if not block:
                break
            digest.update(block)
    return digest.hexdigest()


def canonical_json(document) -> str:
    """Sorted-key, compact JSON with no NaN -- the C2 serialization rule,
    applied to every digest this module records."""
    return json.dumps(document, sort_keys=True, separators=(",", ":"), allow_nan=False)


def canonical_sha256(document) -> str:
    return hashlib.sha256(canonical_json(document).encode("utf-8")).hexdigest()


# ==========================================================================
# Dtype descriptors
# ==========================================================================


def dtype_descriptor(dtype) -> Dict[str, object]:
    """A structured dtype as explicit fields: name, byte-ordered base type,
    shape and offset, plus the itemsize. Round-trips through
    :func:`dtype_from_descriptor` to an equal dtype."""
    dtype = np.dtype(dtype)
    if dtype.names is None:
        raise ConverterError("dtype_descriptor needs a structured dtype, got {}".format(dtype))
    fields = []
    for name in dtype.names:
        field_dtype, offset = dtype.fields[name][:2]
        base = field_dtype.base
        fields.append(
            {
                "name": name,
                "base": base.str,
                "shape": list(field_dtype.shape),
                "offset": int(offset),
            }
        )
    return {"fields": fields, "itemsize": int(dtype.itemsize)}


def dtype_from_descriptor(descriptor: Mapping, what: str = "dtype") -> np.dtype:
    try:
        fields = descriptor["fields"]
        formats = [
            (field["base"], tuple(field["shape"])) if field["shape"] else field["base"]
            for field in fields
        ]
        return np.dtype(
            {
                "names": [field["name"] for field in fields],
                "formats": formats,
                "offsets": [field["offset"] for field in fields],
                "itemsize": descriptor["itemsize"],
            }
        )
    except (KeyError, TypeError, ValueError) as exc:
        raise ConverterError("{}: malformed dtype descriptor ({})".format(what, exc)) from exc


# ==========================================================================
# Schema records
# ==========================================================================


def schema_record(schema: CanonicalSchema) -> Dict[str, object]:
    """The schema as the manifest embeds it: its canonical form and digest,
    plus the separate source-layout identity a binary conversion carries."""
    canonical = schema.as_canonical()
    layout = canonical.get("source_layout")
    return {
        "canonical": canonical,
        "column_mapping_sha256": schema.digest,
        "source_layout_sha256": None if layout is None else canonical_sha256(layout),
    }


def schema_from_record(record: Mapping, origin: str) -> CanonicalSchema:
    """Rebuild a recorded schema through the real profile parser.

    The canonical form is re-expressed as the profile it canonicalises --
    required columns, extras, and for a binary schema the byte order,
    itemsize, offsets and the ordered source properties its layout entries
    carry -- and frozen again by ``build_schema``. So the rebuilt schema passes
    every validation a fresh profile would, and must re-hash to the recorded
    ``column_mapping_sha256``: a manifest whose embedded schema was edited, or
    whose schema the current code would freeze differently, is refused.
    """
    try:
        canonical = record["canonical"]
        recorded_digest = record["column_mapping_sha256"]
        document = {
            "schema_version": canonical["schema_version"],
            "source_format": canonical["source_format"],
            "required_columns": {
                role: list(aliases) for role, aliases in canonical["required_columns"].items()
            },
            "extra_fields": [dict(extra) for extra in canonical["extra_fields"]],
        }
        properties: Optional[List[SourceProperty]] = None
        layout = canonical.get("source_layout")
        if layout is not None:
            entries = layout["entries"]
            document["binary_layout"] = {
                "byte_order": layout["byte_order"],
                "itemsize": layout["itemsize"],
                "offsets": {entry["name"]: entry["offset"] for entry in entries},
            }
            properties = [
                SourceProperty(name=entry["name"], type=entry["type"], units=entry["units"])
                for entry in entries
            ]
    except (KeyError, TypeError, AttributeError) as exc:
        raise ConverterError("{}: malformed embedded schema ({!r})".format(origin, exc)) from exc
    try:
        schema = build_schema(parse_column_map(document, origin=origin), properties)
    except ConverterError as exc:
        raise ConverterError(
            "{}: the embedded schema no longer validates: {}".format(origin, exc)
        ) from exc
    if schema.digest != recorded_digest:
        raise ConverterError(
            "{}: the embedded schema re-hashes to {} but the manifest records {} -- the manifest "
            "was edited, or this code freezes the recorded mapping differently; refusing to "
            "resume".format(origin, schema.digest, recorded_digest)
        )
    if canonical_json(schema_record(schema)) != canonical_json(record):
        raise ConverterError(
            "{}: the embedded schema record disagrees with its own rebuilt form".format(origin)
        )
    return schema


# ==========================================================================
# Source dependencies and inventory
# ==========================================================================


def pin_dependency(
    path, roles: Iterable[str], objects: Iterable[str] = (), content_sha256: bool = False
) -> Dict[str, object]:
    """Stat one source dependency into its manifest record.

    Read-only: it stats, and with ``content_sha256`` reads. ``path`` is the
    resolved real path. Raises naming the path when it is absent, unreadable
    or not a regular file.
    """
    resolved = Path(os.path.realpath(path))
    try:
        status = os.stat(resolved)
    except OSError as exc:
        raise ConverterError(
            "source dependency {} cannot be pinned: {}".format(resolved, exc)
        ) from exc
    if not stat.S_ISREG(status.st_mode):
        raise ConverterError("source dependency {} is not a regular file".format(resolved))
    record: Dict[str, object] = {
        "path": str(resolved),
        "roles": sorted(set(roles)),
        "objects": sorted(set(objects)),
        "size_bytes": int(status.st_size),
        "mtime_ns": int(status.st_mtime_ns),
        "device": int(status.st_dev),
        "inode": int(status.st_ino),
        "sha256": None,
    }
    if content_sha256:
        try:
            record["sha256"] = sha256_file(resolved)
        except OSError as exc:
            raise ConverterError(
                "source dependency {} cannot be read: {}".format(resolved, exc)
            ) from exc
    return record


def merge_dependencies(records: Iterable[Mapping]) -> List[Dict[str, object]]:
    """One record per physical path, roles and objects merged, sorted by path.

    Two records for the same path must agree on every piece of content
    evidence, including two recorded SHA-256s; a disagreement means the file
    changed while it was being pinned.
    """
    merged: Dict[str, Dict[str, object]] = {}
    for record in records:
        path = record["path"]
        if path not in merged:
            merged[path] = dict(record)
            continue
        existing = merged[path]
        for key in ("size_bytes", "mtime_ns", "device", "inode"):
            if existing[key] != record[key]:
                change = "{} {} -> {}".format(key, existing[key], record[key])
                raise ConverterError(
                    "source dependency {} changed while it was being pinned ({})".format(
                        path, change
                    )
                )
        existing["roles"] = sorted(set(existing["roles"]) | set(record["roles"]))
        existing["objects"] = sorted(set(existing["objects"]) | set(record["objects"]))
        if existing.get("sha256") is None:
            existing["sha256"] = record.get("sha256")
        elif record.get("sha256") is not None and record["sha256"] != existing["sha256"]:
            change = "sha256 {} -> {}".format(existing["sha256"], record["sha256"])
            raise ConverterError(
                "source dependency {} changed while it was being pinned ({})".format(path, change)
            )
    return [merged[path] for path in sorted(merged)]


def inventory_record(inventory) -> Dict[str, object]:
    """A source inventory as durable evidence of its enumeration.

    Not the unit list itself -- a forest-level inventory can hold hundreds of
    millions of units -- but its exact digest: SHA-256 over every unit's
    ``(source_file_ordinal, unit_ordinal, n_halos)`` as little-endian int64 in
    inventory order, plus the selected keys'. Two inventories with equal
    records assign every ``SourceHaloID`` and enumerate every forest
    identically; a per-file summary keeps the record readable.
    """
    digest = hashlib.sha256()
    files: Dict[int, List[int]] = {}
    block: List[Tuple[int, int, int]] = []

    def flush() -> None:
        if block:
            digest.update(np.asarray(block, dtype="<i8").tobytes())
            block.clear()

    for unit in inventory.units:
        block.append((unit.source_file_ordinal, unit.unit_ordinal, unit.n_halos))
        summary = files.setdefault(unit.source_file_ordinal, [0, 0])
        summary[0] += 1
        summary[1] += unit.n_halos
        if len(block) >= 65536:
            flush()
    flush()
    selected = hashlib.sha256()
    selected_halos = 0
    for start in range(0, len(inventory.selected), 65536):
        keys = inventory.selected[start : start + 65536]
        selected.update(np.asarray(keys, dtype="<i8").reshape(-1).tobytes())
    # Both sequences ascend in (file, unit) (SourceInventory enforces it), so
    # one merge walk sums the selected units without an O(units) lookup table.
    position = 0
    for unit in inventory.units:
        if position == len(inventory.selected):
            break
        if (unit.source_file_ordinal, unit.unit_ordinal) == tuple(inventory.selected[position]):
            selected_halos += unit.n_halos
            position += 1
    if position != len(inventory.selected):  # pragma: no cover - SourceInventory guarantees it
        raise ConverterError("inventory selects units it does not contain")
    return {
        "n_units": len(inventory.units),
        "total_halos": int(inventory.total_halos),
        "units_sha256": digest.hexdigest(),
        "n_selected_units": len(inventory.selected),
        "selected_halos": int(selected_halos),
        "selected_sha256": selected.hexdigest(),
        "files": [
            {"source_file_ordinal": ordinal, "n_units": counts[0], "n_halos": counts[1]}
            for ordinal, counts in sorted(files.items())
        ],
    }


# ==========================================================================
# Version dispatch
# ==========================================================================


def _read_json(path: Path) -> object:
    try:
        with open(path) as handle:
            return json.load(handle)
    except (OSError, ValueError) as exc:
        raise ConverterError("{}: not a readable JSON manifest ({})".format(path, exc)) from exc


def _version_of(document, path: Path) -> int:
    if not isinstance(document, dict):
        raise ConverterError("{}: a manifest must be a JSON object".format(path))
    version = document.get("manifest_version")
    if isinstance(version, bool) or not isinstance(version, int):
        raise ConverterError(
            "{}: manifest_version {!r} is not an integer version; refusing it".format(path, version)
        )
    return version


def classify_manifest(workdir) -> str:
    """Which state owner a workdir's manifest belongs to, without loading it.

    Returns :data:`MANIFEST_ABSENT`, :data:`MANIFEST_GENERIC` or
    :data:`MANIFEST_LEGACY`. Version 1 (the fix_flybys-era scatter manifest)
    and every unknown version raise: neither owner may resume them.
    """
    path = Path(workdir) / MANIFEST_NAME
    if not path.exists():
        return MANIFEST_ABSENT
    document = _read_json(path)
    version = _version_of(document, path)
    if version == LEGACY_MANIFEST_VERSION:
        return MANIFEST_LEGACY
    if version == MANIFEST_VERSION and document.get("manifest_kind") == MANIFEST_KIND:
        return MANIFEST_GENERIC
    if version == 1:
        raise ConverterError(
            "{}: manifest version 1 predates the fix_flybys removal and is permanently "
            "rejected; convert again into a fresh workdir".format(path)
        )
    raise ConverterError(
        "{}: unknown manifest (version {!r}, kind {!r}); only legacy version {} and generic "
        "version {} are supported".format(
            path, version, document.get("manifest_kind"), LEGACY_MANIFEST_VERSION, MANIFEST_VERSION
        )
    )


def open_manifest(workdir):
    """Load a workdir's manifest with the owner its version names.

    A legacy version-2 manifest is loaded by ``scatter.Manifest`` exactly as
    the legacy route loads it -- nothing is added, converted or written -- so
    it stays resumable in its original representation, and no generic
    provenance is ever invented for it. A generic manifest is loaded by
    :meth:`ConversionManifest.load`. Anything else raises.
    """
    kind = classify_manifest(workdir)
    if kind == MANIFEST_ABSENT:
        raise ConverterError("{}: no manifest".format(Path(workdir) / MANIFEST_NAME))
    if kind == MANIFEST_LEGACY:
        return LegacyManifest.load_or_create(workdir)
    return ConversionManifest.load(workdir)


# ==========================================================================
# Manifest shape
# ==========================================================================


def _malformed(path: Path, key: str, expected: str, value) -> ConverterError:
    return ConverterError(
        "{}: malformed generic manifest: {} must be {}, got {} {}".format(
            path, key, expected, type(value).__name__, reprlib.repr(value)
        )
    )


def _require_mapping(
    value,
    path: Path,
    key: str,
    exact: Optional[Sequence[str]] = None,
    required: Sequence[str] = (),
) -> Mapping:
    """``value`` as a JSON object; with ``exact``, holding exactly those keys,
    and in any case holding every ``required`` key."""
    if not isinstance(value, dict):
        raise _malformed(path, key, "a JSON object", value)
    missing = sorted(set(exact if exact is not None else required) - set(value))
    unknown = sorted(set(value) - set(exact)) if exact is not None else []
    if missing or unknown:
        raise ConverterError(
            "{}: malformed generic manifest: {} (missing {}, unknown {})".format(
                path, key, missing, unknown
            )
        )
    return value


def _require_list(value, path: Path, key: str) -> list:
    if not isinstance(value, list):
        raise _malformed(path, key, "a JSON array", value)
    return value


def _require_int(value, path: Path, key: str, minimum: Optional[int] = 0) -> int:
    if isinstance(value, bool) or not isinstance(value, int):
        raise _malformed(path, key, "an integer", value)
    if minimum is not None and value < minimum:
        raise _malformed(path, key, "at least {}".format(minimum), value)
    return value


def _require_str(value, path: Path, key: str, nullable: bool = False) -> Optional[str]:
    if value is None and nullable:
        return None
    if not isinstance(value, str):
        raise _malformed(path, key, "a string or null" if nullable else "a string", value)
    return value


def _check_artifacts(artifacts, path: Path) -> Mapping:
    _require_mapping(artifacts, path, "artifacts")
    for relpath, entry in artifacts.items():
        key = "artifacts[{!r}]".format(relpath)
        _require_mapping(entry, path, key, required=("bytes", "kind", "sha256", "stage", "status"))
        if entry["stage"] not in STAGES:
            raise _malformed(path, key + ".stage", "one of {}".format(STAGES), entry["stage"])
        if entry["status"] not in (ARTIFACT_PRESENT, ARTIFACT_REMOVED):
            raise _malformed(
                path,
                key + ".status",
                "{!r} or {!r}".format(ARTIFACT_PRESENT, ARTIFACT_REMOVED),
                entry["status"],
            )
        _require_str(entry["kind"], path, key + ".kind")
        _require_int(entry["bytes"], path, key + ".bytes")
        _require_str(entry["sha256"], path, key + ".sha256")
    return artifacts


def _check_stages(stages, artifacts: Mapping, path: Path) -> None:
    if not isinstance(stages, dict) or sorted(stages) != sorted(STAGES):
        raise ConverterError("{}: stage records must be exactly {}".format(path, STAGES))
    for name in STAGES:
        key = "stages.{}".format(name)
        record = _require_mapping(stages[name], path, key, exact=_STAGE_KEYS)
        if record["status"] not in _STAGE_STATUSES:
            raise ConverterError(
                "{}: stage {!r} has unknown status {!r}".format(path, name, record["status"])
            )
        _require_int(record["attempt"], path, key + ".attempt")
        _require_str(record["directory"], path, key + ".directory", nullable=True)
        _require_str(record["error"], path, key + ".error", nullable=True)
        if record["result"] is not None:
            _require_mapping(record["result"], path, key + ".result")
        listed = _require_list(record["artifacts"], path, key + ".artifacts")
        for position, relpath in enumerate(listed):
            item = "{}.artifacts[{}]".format(key, position)
            _require_str(relpath, path, item)
            if (artifacts.get(relpath) or {}).get("stage") != name:
                raise ConverterError(
                    "{}: malformed generic manifest: {} names {!r}, which has no artifacts "
                    "entry of stage {!r}".format(path, item, relpath, name)
                )


def _check_sources(sources, path: Path) -> None:
    _require_mapping(sources, path, "sources", exact=_SOURCES_KEYS)
    dependencies = _require_list(sources["dependencies"], path, "sources.dependencies")
    for position, record in enumerate(dependencies):
        key = "sources.dependencies[{}]".format(position)
        _require_mapping(
            record,
            path,
            key,
            required=("path", "roles", "objects", "size_bytes", "mtime_ns", "device", "inode"),
        )
        _require_str(record["path"], path, key + ".path")
        for name in ("roles", "objects"):
            for index, value in enumerate(_require_list(record[name], path, key + "." + name)):
                _require_str(value, path, "{}.{}[{}]".format(key, name, index))
        for name in ("size_bytes", "device", "inode"):
            _require_int(record[name], path, key + "." + name)
        _require_int(record["mtime_ns"], path, key + ".mtime_ns", minimum=None)
        _require_str(record.get("sha256"), path, key + ".sha256", nullable=True)
    inventory = sources["inventory"]
    if inventory is None:
        return
    key = "sources.inventory"
    _require_mapping(
        inventory,
        path,
        key,
        required=(
            "files",
            "n_selected_units",
            "n_units",
            "selected_halos",
            "selected_sha256",
            "total_halos",
            "units_sha256",
        ),
    )
    for name in ("n_units", "total_halos", "n_selected_units", "selected_halos"):
        _require_int(inventory[name], path, key + "." + name)
    for name in ("units_sha256", "selected_sha256"):
        _require_str(inventory[name], path, key + "." + name)
    for position, entry in enumerate(_require_list(inventory["files"], path, key + ".files")):
        item = "{}.files[{}]".format(key, position)
        _require_mapping(entry, path, item, required=("n_halos", "n_units", "source_file_ordinal"))
        for name in ("source_file_ordinal", "n_units", "n_halos"):
            _require_int(entry[name], path, "{}.{}".format(item, name))


def _check_shape(data: Mapping, path: Path) -> None:
    """Every record type the stages, the validator and the report index, so
    a malformed manifest is refused here naming its path and key."""
    if (
        isinstance(data, dict)
        and "tuning" not in data
        and set(data) == set(_TOP_LEVEL_KEYS) - {"tuning"}
    ):
        raise ConverterError(
            "{}: this manifest was written before tuning was recorded separately from the "
            "configuration (a version 3 manifest without a tuning record); it is not resumed. "
            "Start the conversion again in a fresh workdir".format(path)
        )
    _require_mapping(data, path, "the top level", exact=_TOP_LEVEL_KEYS)
    _require_mapping(data["configuration"], path, "configuration", required=("schema",))
    _require_str(data["configuration_sha256"], path, "configuration_sha256")
    _require_mapping(data["tuning"], path, "tuning")
    artifacts = _check_artifacts(data["artifacts"], path)
    _check_stages(data["stages"], artifacts, path)
    _check_sources(data["sources"], path)


# ==========================================================================
# The generic manifest
# ==========================================================================


def _empty_stage() -> Dict[str, object]:
    return {
        "status": STAGE_PENDING,
        "attempt": 0,
        "directory": None,
        "error": None,
        "artifacts": [],
        "result": None,
    }


class ConversionManifest:
    """One generic workdir's frozen configuration, sources and stage state.

    Construct with :meth:`create` or :meth:`load`; both return a manifest
    whose schema has been rebuilt from the embedded record and whose
    configuration digest has been checked. Nothing is written except by
    :meth:`save` and the transition methods that call it.
    """

    def __init__(self, workdir, data: Dict[str, object], schema: CanonicalSchema):
        self.workdir = Path(workdir).resolve()
        self.path = self.workdir / MANIFEST_NAME
        self.data = data
        self.schema = schema

    # ---- construction ---------------------------------------------------

    @classmethod
    def create(
        cls,
        workdir,
        configuration: Mapping,
        dependencies: Sequence[Mapping],
        inventory: Optional[Mapping] = None,
        tuning: Optional[Mapping] = None,
    ) -> "ConversionManifest":
        """Write a new manifest into an empty or absent ``workdir``.

        ``configuration`` must carry ``schema`` as :func:`schema_record`
        produced it. ``tuning`` holds the performance-only settings recorded
        outside the configuration's identity (:meth:`record_tuning`). Refuses
        a workdir that already holds anything -- except a lone
        ``manifest.json.tmp`` left by an interrupted first save, which is
        discarded -- and one that contains any of the ``dependencies``.
        """
        workdir = Path(workdir)
        resolved = workdir.resolve()
        for dependency in dependencies:
            source = Path(dependency["path"])
            if source == resolved or resolved in source.parents:
                raise ConverterError(
                    "{}: the workdir contains source dependency {}; generic cleanup must never "
                    "be able to reach a source file".format(workdir, source)
                )
        if workdir.exists():
            if not workdir.is_dir():
                raise ConverterError("{} is not a directory".format(workdir))
            entries = list(workdir.iterdir())
            stale = workdir / (MANIFEST_NAME + ".tmp")
            if entries == [stale] and stale.is_file() and not stale.is_symlink():
                # A crash inside the very first save() left only its
                # temporary file: no manifest was ever published, so nothing
                # was recorded or created. Recover by discarding it.
                stale.unlink()
                entries = []
            if entries:
                raise ConverterError(
                    "{}: refusing to start a conversion in a non-empty directory without a "
                    "generic manifest".format(workdir)
                )
        configuration = json.loads(canonical_json(configuration))
        schema = schema_from_record(configuration["schema"], str(workdir / MANIFEST_NAME))
        data: Dict[str, object] = {
            "manifest_version": MANIFEST_VERSION,
            "manifest_kind": MANIFEST_KIND,
            "configuration": configuration,
            "configuration_sha256": canonical_sha256(configuration),
            "sources": {
                "dependencies": merge_dependencies(dependencies),
                "inventory": None if inventory is None else dict(inventory),
            },
            "stages": {stage: _empty_stage() for stage in STAGES},
            "artifacts": {},
            "tuning": json.loads(canonical_json(dict(tuning or {}))),
        }
        workdir.mkdir(parents=True, exist_ok=True)
        manifest = cls(workdir, data, schema)
        manifest.save()
        return manifest

    @classmethod
    def load(cls, workdir) -> "ConversionManifest":
        """Load and validate a generic manifest. Writes nothing.

        Refuses a legacy, version-1 or unknown manifest, one whose records do
        not have the shapes the stages index (named by key), one whose
        configuration digest does not match its configuration, and one whose
        embedded schema does not rebuild to its recorded digest. Every refusal
        is a ``ConverterError`` naming the manifest path.
        """
        path = Path(workdir) / MANIFEST_NAME
        kind = classify_manifest(workdir)
        if kind == MANIFEST_ABSENT:
            raise ConverterError("{}: no manifest; start the conversion first".format(path))
        if kind == MANIFEST_LEGACY:
            raise ConverterError(
                "{}: this is a legacy version-{} ASCII workdir; resume it with "
                "convert_ctrees.py. The generic pipeline never upgrades, relabels or adds "
                "provenance to it".format(path, LEGACY_MANIFEST_VERSION)
            )
        data = _read_json(path)
        _check_shape(data, path)
        recomputed = canonical_sha256(data["configuration"])
        if recomputed != data["configuration_sha256"]:
            raise ConverterError(
                "{}: configuration digest {} != recorded {} -- the embedded configuration was "
                "edited; refusing to resume".format(path, recomputed, data["configuration_sha256"])
            )
        schema = schema_from_record(data["configuration"]["schema"], str(path))
        manifest = cls(workdir, data, schema)
        manifest._check_stage_order()
        return manifest

    def _check_stage_order(self) -> None:
        """A stage may be complete only if every earlier stage is."""
        seen_incomplete = None
        for name in STAGES:
            complete = self.stage(name)["status"] == STAGE_COMPLETE
            if complete and seen_incomplete is not None:
                raise ConverterError(
                    "{}: stage {!r} is complete but earlier stage {!r} is not".format(
                        self.path, name, seen_incomplete
                    )
                )
            if not complete and seen_incomplete is None:
                seen_incomplete = name

    # ---- views ------------------------------------------------------------

    @property
    def configuration(self) -> Mapping:
        return self.data["configuration"]

    @property
    def dependencies(self) -> List[Mapping]:
        return self.data["sources"]["dependencies"]

    @property
    def inventory(self) -> Optional[Mapping]:
        return self.data["sources"]["inventory"]

    @property
    def tuning(self) -> Mapping:
        return self.data["tuning"]

    def stage(self, name: str) -> Dict[str, object]:
        if name not in STAGES:
            raise ConverterError("unknown stage {!r}".format(name))
        return self.data["stages"][name]

    def is_complete(self, name: str) -> bool:
        return self.stage(name)["status"] == STAGE_COMPLETE

    # ---- persistence ------------------------------------------------------

    def save(self) -> None:
        """Atomic, durable replace: write a sibling temporary file, fsync it,
        rename it over the manifest, fsync the directory. A crash leaves
        either the previous manifest or this one, never a torn file.

        The temporary file is opened with ``O_NOFOLLOW``: a symlink planted at
        its name is refused, never written through. A regular file left there
        by an interrupted save is truncated and reused.
        """
        document = json.dumps(self.data, indent=2, sort_keys=True, allow_nan=False) + "\n"
        tmp = self.path.with_name(MANIFEST_NAME + ".tmp")
        try:
            descriptor = os.open(
                str(tmp), os.O_WRONLY | os.O_CREAT | os.O_TRUNC | os.O_NOFOLLOW, 0o644
            )
        except OSError as exc:
            raise ConverterError(
                "{}: cannot open the manifest's temporary file for writing ({}); a symlink there "
                "is never followed".format(tmp, exc)
            ) from exc
        with os.fdopen(descriptor, "w") as handle:
            handle.write(document)
            handle.flush()
            os.fsync(handle.fileno())
        os.replace(tmp, self.path)
        _fsync_directory(self.workdir)

    def record_tuning(self, tuning: Mapping) -> None:
        """Replace the recorded performance-only settings and save.

        ``tuning`` is outside the configuration and its digest: it may differ
        between attempts because nothing it holds can change an output. What
        is recorded is what the next attempt that reads it uses.
        """
        self.data["tuning"] = json.loads(canonical_json(dict(tuning)))
        self.save()

    # ---- binding checks (all read-only) -----------------------------------

    def require_schema(self, schema: Optional[CanonicalSchema]) -> None:
        """Refuse a caller-named schema that is not the recorded one.

        Compared by digest, never by width: a same-width schema that changes
        a type, unit, component or h convention is a different conversion.
        """
        if schema is None:
            return
        if not isinstance(schema, CanonicalSchema):
            raise ConverterError(
                "expected a CanonicalSchema, got {!r}".format(type(schema).__name__)
            )
        if schema.digest != self.schema.digest:
            raise ConverterError(
                "{}: this workdir was created with schema {} ({}); the requested schema is {} "
                "({}) -- a different mapping is a different conversion, even at the same record "
                "width; refusing before anything is changed".format(
                    self.path,
                    self.schema.digest,
                    self.schema.source_format,
                    schema.digest,
                    schema.source_format,
                )
            )

    def verify_dependencies(self) -> None:
        """Re-pin every recorded source dependency; raise on the first that
        is missing or whose content evidence moved."""
        for recorded in self.dependencies:
            path = recorded["path"]
            current = pin_dependency(
                path,
                recorded["roles"],
                recorded["objects"],
                content_sha256=recorded.get("sha256") is not None,
            )
            for key in ("size_bytes", "mtime_ns", "device", "inode", "sha256"):
                if current[key] != recorded.get(key):
                    raise ConverterError(
                        "source dependency {} changed since this conversion pinned it ({}: {} -> "
                        "{}); refusing to continue -- restore it or start a fresh "
                        "workdir".format(path, key, recorded.get(key), current[key])
                    )

    def require_dependency_paths(self, paths: Iterable[str]) -> None:
        """The set of physical files an adapter now reads must be exactly the
        pinned set: an added external file is a changed dependency graph."""
        current = sorted({os.path.realpath(path) for path in paths})
        recorded = sorted(
            record["path"] for record in self.dependencies if SOURCE_ROLE in record["roles"]
        )
        if current != recorded:
            raise ConverterError(
                "{}: the source now resolves through {} but this conversion pinned {}; "
                "refusing to continue".format(self.path, current, recorded)
            )

    def require_inventory(self, record: Mapping) -> None:
        """Record the inventory the first time, and require it thereafter.

        Must be followed by :meth:`save` when it records; it never writes.
        """
        if self.inventory is None:
            self.data["sources"]["inventory"] = dict(record)
            return
        if dict(record) != dict(self.inventory):
            raise ConverterError(
                "{}: the source inventory changed (recorded {} units / {} halos, digest {}; now "
                "{} units / {} halos, digest {}) -- SourceHaloID and forest enumeration would "
                "differ; refusing to continue".format(
                    self.path,
                    self.inventory["n_units"],
                    self.inventory["total_halos"],
                    self.inventory["units_sha256"],
                    record["n_units"],
                    record["total_halos"],
                    record["units_sha256"],
                )
            )

    # ---- artifacts --------------------------------------------------------

    def artifact_path(self, relpath: str) -> Path:
        """Resolve a recorded relative path strictly inside the workdir,
        refusing absolute paths, ``..`` escapes and symlinked components."""
        if not isinstance(relpath, str) or not relpath or os.path.isabs(relpath):
            raise ConverterError(
                "artifact path {!r} must be a nonempty workdir-relative path".format(relpath)
            )
        parts = Path(relpath).parts
        if any(part in ("..", ".") for part in parts):
            raise ConverterError("artifact path {!r} may not contain '.' or '..'".format(relpath))
        candidate = self.workdir
        for part in parts:
            candidate = candidate / part
            if candidate.is_symlink():
                raise ConverterError(
                    "refusing {}: {} is a symlink, and artifacts are never followed out of "
                    "the workdir".format(relpath, candidate)
                )
        if self.workdir not in candidate.resolve().parents:
            raise ConverterError(
                "refusing {}: resolves outside the workdir {}".format(relpath, self.workdir)
            )
        return candidate

    def relative(self, path) -> str:
        resolved = Path(path).resolve()
        if self.workdir not in resolved.parents:
            raise ConverterError("{} is outside the workdir {}".format(resolved, self.workdir))
        return resolved.relative_to(self.workdir).as_posix()

    def artifact(self, relpath: str) -> Optional[Dict[str, object]]:
        return self.data["artifacts"].get(relpath)

    def register_artifact(self, relpath: str, stage: str, kind: str, **facts) -> Dict[str, object]:
        """Record one artifact's content checksum and size as it is on disk
        now. Must be followed by :meth:`save`."""
        if stage not in STAGES:
            raise ConverterError("unknown stage {!r}".format(stage))
        path = self.artifact_path(relpath)
        if not path.is_file():
            raise ConverterError("cannot register {}: not a regular file".format(path))
        entry: Dict[str, object] = {
            "stage": stage,
            "kind": kind,
            "status": ARTIFACT_PRESENT,
            "bytes": int(path.stat().st_size),
            "sha256": sha256_file(path),
        }
        entry.update(facts)
        self.data["artifacts"][relpath] = entry
        artifacts = self.stage(stage)["artifacts"]
        if relpath not in artifacts:
            artifacts.append(relpath)
        return entry

    def verify_artifact(self, relpath: str, what: str) -> Dict[str, object]:
        """Verify a registered artifact before trusting or consuming it:
        recorded present, on disk, the recorded size and SHA-256."""
        entry = self.artifact(relpath)
        path = self.artifact_path(relpath)
        if entry is None or entry.get("status") != ARTIFACT_PRESENT:
            raise ConverterError(
                "{}: {} is not a present manifest-owned artifact".format(path, what)
            )
        if not path.is_file():
            raise ConverterError(
                "{}: {} is recorded in the manifest but missing on disk".format(path, what)
            )
        size = path.stat().st_size
        if size != entry["bytes"]:
            raise ConverterError(
                "{}: {} is {} bytes, registered as {} -- refusing to use it".format(
                    path, what, size, entry["bytes"]
                )
            )
        checksum = sha256_file(path)
        if checksum != entry["sha256"]:
            raise ConverterError(
                "{}: {} content checksum {} != registered {} -- refusing to use it".format(
                    path, what, checksum, entry["sha256"]
                )
            )
        return entry

    def verify_stage_artifacts(self, stage: str, allow_removed: bool = False) -> List[str]:
        """Verify every artifact of a stage. ``allow_removed`` accepts those a
        later, complete stage deliberately consumed; returns their paths."""
        removed = []
        for relpath in self.stage(stage)["artifacts"]:
            entry = self.artifact(relpath)
            if allow_removed and entry is not None and entry.get("status") == ARTIFACT_REMOVED:
                removed.append(relpath)
                continue
            self.verify_artifact(relpath, "{} artifact".format(stage))
        return removed

    def _unlink_artifact(self, relpath: str) -> None:
        """The one unlink of a registered artifact. Only
        :meth:`consume_stage` calls it, after verifying the successor stage and
        every artifact it is about to delete; it must be followed by
        :meth:`save`."""
        self.artifact_path(relpath).unlink()
        self.data["artifacts"][relpath]["status"] = ARTIFACT_REMOVED

    def consume_stage(self, stage: str, successor: str) -> List[str]:
        """Opt-in cleanup of one stage's artifacts once ``successor`` is done.

        The successor must be complete and every one of its artifacts must
        verify *now*, before any predecessor byte is deleted; each predecessor
        artifact is then verified and deleted. A registered artifact already
        absent on disk (a crash after an unlink, before its save) converges on
        ``removed``. Saves once at the end if anything changed.
        """
        if STAGES.index(successor) != STAGES.index(stage) + 1:
            raise ConverterError("{} does not consume {}".format(successor, stage))
        if not self.is_complete(successor):
            raise ConverterError(
                "{}: refusing to consume {} artifacts before {} is complete".format(
                    self.path, stage, successor
                )
            )
        candidates = [
            relpath
            for relpath in self.stage(stage)["artifacts"]
            if (self.artifact(relpath) or {}).get("status") == ARTIFACT_PRESENT
        ]
        if not candidates:
            # Nothing left to consume: an idempotent no-op, which must not
            # re-verify a successor whose own artifacts a later complete stage
            # may have consumed in turn.
            return []
        # A successor artifact that the stage after it deliberately consumed
        # is accepted as consumed -- the same transitive rule every skip-trust
        # path uses -- so deferring this cleanup until after the next one is
        # not refused. Everything the successor still holds is verified.
        index = STAGES.index(successor)
        later = STAGES[index + 1] if index + 1 < len(STAGES) else None
        later_complete = later is not None and self.is_complete(later)
        consumed = self.verify_stage_artifacts(successor, allow_removed=later_complete)
        if consumed:
            # Those rows now live only in the later stage's artifacts, so they
            # are what must verify before any predecessor byte goes.
            self.verify_stage_artifacts(later)
        # Every predecessor is verified before the first one is deleted, so a
        # single corrupt artifact refuses the whole consumption cleanly.
        present = []
        removed = []
        for relpath in candidates:
            entry = self.artifact(relpath)
            if not self.artifact_path(relpath).exists():
                entry["status"] = ARTIFACT_REMOVED
                removed.append(relpath)
                continue
            self.verify_artifact(relpath, "artifact to consume")
            present.append(relpath)
        try:
            for relpath in present:
                self._unlink_artifact(relpath)
                removed.append(relpath)
        finally:
            if removed:
                self.save()
        return sorted(removed)

    # ---- stage transitions ------------------------------------------------

    def require_complete(self, stage: str) -> None:
        if not self.is_complete(stage):
            raise ConverterError(
                "{}: stage {!r} is {!r}, not complete; run it first".format(
                    self.path, stage, self.stage(stage)["status"]
                )
            )

    def begin_attempt(self, stage: str, directory: Optional[str] = None) -> int:
        """Record a new attempt of ``stage`` as running and save, *before*
        the attempt creates anything. Returns the attempt number."""
        record = self.stage(stage)
        if record["status"] == STAGE_COMPLETE:
            raise ConverterError("{}: stage {!r} is already complete".format(self.path, stage))
        index = STAGES.index(stage)
        for earlier in STAGES[:index]:
            self.require_complete(earlier)
        record["attempt"] = int(record["attempt"]) + 1
        record["status"] = STAGE_RUNNING
        record["error"] = None
        record["directory"] = directory
        self.save()
        return record["attempt"]

    def complete_stage(self, stage: str, result: Mapping) -> None:
        record = self.stage(stage)
        if record["status"] != STAGE_RUNNING:
            raise ConverterError(
                "{}: cannot complete stage {!r} from status {!r}".format(
                    self.path, stage, record["status"]
                )
            )
        record["status"] = STAGE_COMPLETE
        record["result"] = json.loads(canonical_json(result))
        record["error"] = None
        self.save()

    def fail_stage(self, stage: str, error: BaseException) -> None:
        """Best-effort record of a failed attempt, never marking it complete.

        Called from an exception handler: a failure to save here is
        swallowed so the original error is what propagates, and the stage
        simply stays ``running`` -- which resumes identically.
        """
        record = self.stage(stage)
        if record["status"] != STAGE_RUNNING:
            return
        record["status"] = STAGE_FAILED
        record["error"] = "{}: {}".format(type(error).__name__, error)
        try:
            self.save()
        except Exception:  # noqa: BLE001 - secondary failure must not mask the first
            record["status"] = STAGE_RUNNING

    def discard_attempt(self, stage: str) -> None:
        """Remove a running or failed attempt's directory, if it exists.

        Contained by construction: the directory was recorded by
        :meth:`begin_attempt` before it was created, lies strictly inside the
        workdir, is not a symlink, and holds only that attempt's unverified
        output. It is never an artifact another stage consumed. Nothing
        outside it is touched, and any artifact registered under it is
        dropped from the record.
        """
        record = self.stage(stage)
        if record["status"] not in (STAGE_RUNNING, STAGE_FAILED):
            raise ConverterError(
                "{}: stage {!r} is {!r}; only an interrupted attempt is discarded".format(
                    self.path, stage, record["status"]
                )
            )
        directory = record["directory"]
        if directory is None:
            return
        path = self.artifact_path(directory)
        prefix = directory.rstrip("/") + "/"
        for relpath in list(record["artifacts"]):
            if relpath.startswith(prefix):
                record["artifacts"].remove(relpath)
                self.data["artifacts"].pop(relpath, None)
        if path.exists():
            if not path.is_dir() or path.is_symlink():
                raise ConverterError(
                    "{}: recorded attempt directory {} is not a real directory".format(
                        self.path, path
                    )
                )
            shutil.rmtree(path)


def _fsync_directory(path: Path) -> None:
    """fsync a directory so a rename inside it is durable (POSIX)."""
    try:
        descriptor = os.open(str(path), os.O_RDONLY)
    except OSError:  # pragma: no cover - platforms without directory fds
        return
    try:
        os.fsync(descriptor)
    except OSError:  # pragma: no cover - some filesystems refuse directory fsync
        pass
    finally:
        os.close(descriptor)


def finite_float(value, what: str) -> float:
    """A JSON-safe finite float, type-checked rather than coerced."""
    if isinstance(value, bool) or not isinstance(value, (int, float, np.integer, np.floating)):
        raise ConverterError("{} must be a number, got {!r}".format(what, value))
    value = float(value)
    if not math.isfinite(value):
        raise ConverterError("{} must be finite, got {!r}".format(what, value))
    return value
