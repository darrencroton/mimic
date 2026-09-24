"""Horizontal-HDF5 emission for the ctrees -> horizontal-HDF5 converter (plan Slice 7).

Emits ``snapshot_NNN.h5`` files and the ``forests.h5`` sidecar exactly per the
frozen contract in docs/dev/HORIZONTAL-HDF5-FORMAT.md (format_version = 2). The
contract is consumed, never modified — any mismatch discovered here is a
converter bug or a spec erratum to raise to the user.

Emission facts:

- One file per a_list snapshot, ``snapshot_NNN.h5`` for NNN in
  ``[0, len(a_list))`` — INCLUDING empty files (zero-length chunked datasets)
  for snapshots with no halos, as the contract requires.
- Slab order is the fixed/links scratch row order unchanged: the scratch files
  are ascending in ctrees id, and the Slice 5 ``|MostBoundID| == id``
  invariant makes that identical to the contract's ascending-|MostBoundID|
  order. The writer re-asserts the order on the emitted array.
- Datasets are chunked ``(65536,)`` / ``(65536, 3)``, uncompressed, written
  with ``libver="latest"``. HDF5 only permits a chunk shape exceeding the
  current extent on resizable datasets, so every dataset is created with an
  unlimited first dimension (``maxshape``); the contract makes chunk layout a
  storage detail consumers must not depend on.
- Header attributes are stamped with explicit dtypes; ``particle_mass_msun_h``
  is converted explicitly from the simulation_info 1e10 Msun/h value
  (``x 1e10``) — units are validated, never assumed.
- ``forests.h5`` carries the single ``/ForestID`` dataset from the Phase 0
  table (this writer is the single HDF5 owner; scatter only produced the data).

Every emitted file is re-opened and verified bit-for-bit against the source
arrays before being recorded in the manifest's ``outputs`` map (md5 +
row count). Re-running skips files whose recorded md5 still matches
(refuse-not-repair: a recorded file with different content aborts).

**Only the legacy scratch layout is emitted.** A workdir whose scratch records
use the extended layout of the canonical ASCII bridge (source coordinates and
declared extras, converter generalisation Slice 5) is refused before any file
is written: format v2 has no place for either, and dropping them to emit v2
anyway would silently lose data a caller asked to keep. That data leaves
through the v3 writer, never through this one.

**Format version 3** (converter generalisation Slice 8,
docs/dev/HORIZONTAL-HDF5-FORMAT-V3-DRAFT.md, approved at Gate G1) is written by
:class:`HorizontalV3Writer`, a separate section at the end of this module that
shares nothing with the v2 path above except the header attribute table and
the chunk shapes, which v3 keeps unchanged. It is the generic pipeline's write
stage (``pipeline.run_write``); nothing in the v2 functions above calls it, and
it never stamps anything but ``format_version = 3``.
"""

import os
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, Iterable, Iterator, List, Mapping, Sequence, Tuple

import h5py
import numpy as np
import yaml

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
from fixups import (  # noqa: E402
    FIXED_DTYPE_TAG,
    FIXED_RECORD_DTYPE,
    PARTICLE_MASS_UNITS,
    REF_TO_NATIVE_MASS,
)
from links import LINKS_DTYPE_TAG, LINKS_RECORD_DTYPE  # noqa: E402
from pipeline import StageWriter, WriteInputs  # noqa: E402
from scatter import Manifest, file_md5, load_a_list  # noqa: E402

#: format_version this writer implements (the frozen contract's ratchet).
#: Bumped 1 -> 2 when fix_flybys was removed (MostBoundID is always positive
#: now; docs/dev/SHIN-UCHUU-FLYBY-DEFECT-ADDENDUM.md, decision D3). Version 1
#: data is no longer conforming and must be rejected by the reader, not
#: silently re-read.
FORMAT_VERSION = 2

#: Contract chunk shapes (docs/dev/HORIZONTAL-HDF5-FORMAT.md Storage Layout).
CHUNK_1D = (65536,)
CHUNK_VEC = (65536, 3)

#: Expected box-size units string in simulation_info.yaml (header attribute
#: box_size_mpc_h is Mpc/h comoving; any other units would corrupt it).
BOX_SIZE_UNITS = "Mpc/h"

#: Header attributes: name -> numpy dtype (docs/dev/HORIZONTAL-HDF5-FORMAT.md
#: Header Attributes table; names and types are normative).
HEADER_ATTRS = {
    "format_version": np.int32,
    "links_adjacent": np.int32,
    "scale_factor": np.float64,
    "snapshot_number": np.int32,
    "n_halos": np.int64,
    "n_forests_total": np.int64,
    "max_halo_rank_in_forest": np.int64,
    "box_size_mpc_h": np.float64,
    "particle_mass_msun_h": np.float64,
    "omega_matter": np.float64,
    "omega_lambda": np.float64,
    "hubble_h": np.float64,
}

#: /halos datasets: name -> (dtype, is_vec3) (docs/dev/HORIZONTAL-HDF5-FORMAT.md
#: Halo Datasets table; names and types are normative).
HALO_DATASETS = {
    "Descendant": (np.int32, False),
    "FirstProgenitor": (np.int32, False),
    "NextProgenitor": (np.int32, False),
    "FirstHaloInFOFgroup": (np.int32, False),
    "NextHaloInFOFgroup": (np.int32, False),
    "Len": (np.int32, False),
    "SnapNum": (np.int32, False),
    "M_Crit200": (np.float32, False),
    "Pos": (np.float32, True),
    "Vel": (np.float32, True),
    "Spin": (np.float32, True),
    "VelDisp": (np.float32, False),
    "Vmax": (np.float32, False),
    "MostBoundID": (np.int64, False),
    "ForestIndex": (np.int64, False),
    "HaloRankInForest": (np.int64, False),
}


def snapshot_h5_name(snap: int) -> str:
    return "snapshot_{:03d}.h5".format(snap)


def _log(message: str) -> None:
    print(message, file=sys.stderr)


def load_header_metadata(path) -> Dict[str, float]:
    """Load the physical header attributes from simulation_info.yaml with
    explicit unit validation and conversion (plan review finding 6):
    ``particle_mass_msun_h = particle_mass[1e10 Msun/h] x 1e10``."""
    path = Path(path)
    with open(path) as handle:
        data = yaml.safe_load(handle)
    try:
        sim = data["simulation"]
        cosmology = sim["cosmology"]
        box = sim["box_size"]
        pmass = sim["particle_mass"]
        values = {
            "box_size_mpc_h": float(box["value"]),
            "particle_mass_msun_h": float(pmass["value"]) * REF_TO_NATIVE_MASS,
            "omega_matter": float(cosmology["omega_matter"]),
            "omega_lambda": float(cosmology["omega_lambda"]),
            "hubble_h": float(cosmology["hubble_h"]),
        }
    except (KeyError, TypeError, ValueError):
        raise ConverterError(
            "{}: missing or malformed simulation metadata (need simulation.cosmology "
            "omega_matter/omega_lambda/hubble_h, box_size.value, particle_mass.value)".format(path)
        )
    if box.get("units") != BOX_SIZE_UNITS:
        raise ConverterError(
            "{}: box_size units {!r} != required {!r}".format(
                path, box.get("units"), BOX_SIZE_UNITS
            )
        )
    if pmass.get("units") != PARTICLE_MASS_UNITS:
        raise ConverterError(
            "{}: particle_mass units {!r} != required {!r} — the header attribute "
            "particle_mass_msun_h is converted explicitly from 1e10 Msun/h".format(
                path, pmass.get("units"), PARTICLE_MASS_UNITS
            )
        )
    for name, value in values.items():
        if not np.isfinite(value):
            raise ConverterError("{}: {} is not finite ({})".format(path, name, value))
    return values


def reject_extended_layout(manifest: Manifest) -> None:
    """Refuse a workdir holding extended scratch records (module docstring):
    format v2 cannot carry source coordinates or declared extras, and this
    writer emits nothing rather than emit a v2 file that silently drops them."""
    layout = manifest.layout
    if layout.is_extended:
        raise ConverterError(
            "{}: this workdir holds extended scratch records (tag {!r}; schema {}; extras {}) "
            "— horizontal-HDF5 format_version {} cannot carry source coordinates or declared "
            "extra fields, and this writer will not drop them to emit v2; they are written "
            "only by the v3 writer".format(
                manifest.path,
                layout.dtype_tag,
                layout.schema_digest,
                [name for name, _type in layout.extras] or "none",
                FORMAT_VERSION,
            )
        )


def build_halo_arrays(
    fixed: np.ndarray, links: np.ndarray, snap: int, context: str
) -> Dict[str, np.ndarray]:
    """Assemble the /halos dataset arrays from row-aligned fixed + links
    records, asserting the contract's slab-order invariant (ascending unique
    |MostBoundID|) on the emitted values themselves."""
    if fixed.size != links.size:
        raise ConverterError(
            "{}: fixed file has {} rows but links file has {} — row alignment is the "
            "Slice 6 contract".format(context, fixed.size, links.size)
        )
    sentinel = fixed["MostBoundID"] == np.iinfo(np.int64).min
    if sentinel.any():
        rows = np.nonzero(sentinel)[0][:5]
        raise ConverterError(
            "{}: {} MostBoundID value(s) equal INT64_MIN, whose magnitude overflows signed "
            "int64 — no valid source-catalog id can take this value; example rows: "
            "{}".format(context, int(sentinel.sum()), ", ".join(str(int(r)) for r in rows))
        )
    # Last line of defence before a format_version = 2 stamp goes on disk: a
    # workdir resumed from before fix_flybys was removed could carry
    # fixed/linked snapshots with negated MostBoundID (its demotion marker)
    # without ever being re-verified (fix_one_snapshot trusts recorded
    # checksums on resume, not content semantics). MANIFEST_VERSION now
    # refuses that resume outright, but assert the invariant here too rather
    # than rely on that alone.
    non_positive = fixed["MostBoundID"] <= 0
    if non_positive.any():
        rows = np.nonzero(non_positive)[0][:5]
        raise ConverterError(
            "{}: snapshot {} has {} record(s) with non-positive MostBoundID — the "
            "fix_flybys demotion marker must not survive to emission; example rows: "
            "{}".format(
                context,
                snap,
                int(non_positive.sum()),
                ", ".join(
                    "(row={}, MostBoundID={})".format(int(r), int(fixed["MostBoundID"][r]))
                    for r in rows
                ),
            )
        )
    id_mismatch = fixed["MostBoundID"] != fixed["id"]
    if id_mismatch.any():
        rows = np.nonzero(id_mismatch)[0][:5]
        raise ConverterError(
            "{}: snapshot {} has {} record(s) where MostBoundID != id; example rows: "
            "{}".format(
                context,
                snap,
                int(id_mismatch.sum()),
                ", ".join(
                    "(row={}, id={}, MostBoundID={})".format(
                        int(r), int(fixed["id"][r]), int(fixed["MostBoundID"][r])
                    )
                    for r in rows
                ),
            )
        )
    # MostBoundID is always positive now (fix_flybys, which used to negate it
    # as a demotion marker, was removed), so |MostBoundID| == MostBoundID; the
    # abs() is retained only because it is what the contract's ordering
    # invariant is stated in terms of.
    mb_abs = np.abs(fixed["MostBoundID"])
    if fixed.size > 1 and not (mb_abs[1:] > mb_abs[:-1]).all():
        rows = np.nonzero(mb_abs[1:] <= mb_abs[:-1])[0][:5]
        examples = [
            "(row={}, |MostBoundID|={}, next {})".format(int(r), int(mb_abs[r]), int(mb_abs[r + 1]))
            for r in rows
        ]
        raise ConverterError(
            "{}: slab is not strictly ascending in |MostBoundID|; examples: {}".format(
                context, ", ".join(examples)
            )
        )
    arrays = {
        "Descendant": links["Descendant"],
        "FirstProgenitor": links["FirstProgenitor"],
        "NextProgenitor": links["NextProgenitor"],
        "FirstHaloInFOFgroup": links["FirstHaloInFOFgroup"],
        "NextHaloInFOFgroup": links["NextHaloInFOFgroup"],
        "Len": fixed["Len"],
        "SnapNum": np.full(fixed.size, snap, dtype=np.int32),
        "M_Crit200": fixed["Mvir"],
        "Pos": np.column_stack((fixed["X"], fixed["Y"], fixed["Z"])),
        "Vel": np.column_stack((fixed["VX"], fixed["VY"], fixed["VZ"])),
        "Spin": np.column_stack((fixed["Jx"], fixed["Jy"], fixed["Jz"])),
        "VelDisp": fixed["vrms"],
        "Vmax": fixed["vmax"],
        "MostBoundID": fixed["MostBoundID"],
        "ForestIndex": links["ForestIndex"],
        "HaloRankInForest": links["HaloRankInForest"],
    }
    return {
        name: np.ascontiguousarray(a, dtype=HALO_DATASETS[name][0]) for name, a in arrays.items()
    }


def _create_contract_dataset(group, name: str, data: np.ndarray, is_vec: bool) -> None:
    """Chunked, uncompressed, unlimited first dimension (see module docstring
    for why maxshape is required by the fixed contract chunk shape)."""
    if is_vec:
        group.create_dataset(
            name, data=data, chunks=CHUNK_VEC, maxshape=(None, 3), compression=None
        )
    else:
        group.create_dataset(name, data=data, chunks=CHUNK_1D, maxshape=(None,), compression=None)


def write_snapshot_file(
    path,
    snap: int,
    arrays: Dict[str, np.ndarray],
    scale_factor: float,
    metadata: Dict[str, float],
    n_forests_total: int,
    max_halo_rank_in_forest: int,
) -> None:
    """Write one snapshot_NNN.h5 with exactly the contract's object set."""
    n_halos = arrays["MostBoundID"].size if arrays else 0
    values = {
        "format_version": FORMAT_VERSION,
        "links_adjacent": 1,
        "scale_factor": scale_factor,
        "snapshot_number": snap,
        "n_halos": n_halos,
        "n_forests_total": n_forests_total,
        "max_halo_rank_in_forest": max_halo_rank_in_forest,
    }
    values.update(metadata)
    path = Path(path)
    tmp = path.with_suffix(".h5.tmp")
    with h5py.File(tmp, "w", libver="latest") as handle:
        header = handle.create_group("header")
        for name, dtype in HEADER_ATTRS.items():
            header.attrs.create(name, values[name], dtype=dtype)
        halos = handle.create_group("halos")
        for name, (dtype, is_vec) in HALO_DATASETS.items():
            if arrays:
                data = arrays[name]
            else:
                shape = (0, 3) if is_vec else (0,)
                data = np.empty(shape, dtype=dtype)
            _create_contract_dataset(halos, name, data, is_vec)
    os.replace(tmp, path)


def verify_snapshot_file(path, snap: int, arrays: Dict[str, np.ndarray], context: str) -> None:
    """Re-open an emitted file and verify every dataset byte-for-byte against
    the source arrays (bit-exactness is a frozen comparison rule; float
    comparison goes through raw bytes so NaN payloads and signed zeros count)."""
    with h5py.File(path, "r") as handle:
        halos = handle["halos"]
        n_halos = int(handle["header"].attrs["n_halos"])
        expected_n = arrays["MostBoundID"].size if arrays else 0
        if n_halos != expected_n:
            raise ConverterError(
                "{}: n_halos attribute {} != source rows {}".format(context, n_halos, expected_n)
            )
        for name, (dtype, is_vec) in HALO_DATASETS.items():
            stored = halos[name][...]
            if arrays:
                expected = arrays[name]
            else:
                expected = np.empty((0, 3) if is_vec else (0,), dtype=dtype)
            if stored.shape != expected.shape or stored.tobytes() != expected.tobytes():
                raise ConverterError(
                    "{}: dataset {} re-read does not match what was written".format(context, name)
                )


def _record_output(manifest: Manifest, path: Path, rows: int, kind: str) -> None:
    outputs = manifest.data.setdefault("outputs", {})
    outputs[str(path.resolve())] = {"kind": kind, "md5": file_md5(path), "rows": rows}
    manifest.save()


def _skip_trust_output(manifest: Manifest, path: Path) -> bool:
    """True if this output file was already recorded and its content still
    matches; a recorded file with different content aborts (never repaired)."""
    entry = manifest.data.get("outputs", {}).get(str(Path(path).resolve()))
    if entry is None:
        return False
    if not Path(path).exists():
        return False
    checksum = file_md5(path)
    if checksum != entry.get("md5"):
        raise ConverterError(
            "{}: output file content md5 {} != recorded {} — refusing to overwrite a "
            "recorded output (remove it and re-run, or use a fresh output directory)".format(
                path, checksum, entry.get("md5")
            )
        )
    return True


def _load_snapshot_scratch(manifest: Manifest, snap: int) -> Tuple[np.ndarray, np.ndarray]:
    """Verify and load one snapshot's fixed + links scratch files."""
    entry = manifest.data["snapshots"][str(snap)]
    fixed_meta = manifest.verify_intermediate(entry["fixed_file"], "fixed snapshot scratch")
    if fixed_meta.get("dtype_tag") != FIXED_DTYPE_TAG:
        raise ConverterError(
            "{}: fixed-file dtype tag {!r} != expected {!r} — refusing to emit".format(
                entry["fixed_file"], fixed_meta.get("dtype_tag"), FIXED_DTYPE_TAG
            )
        )
    links_meta = manifest.verify_intermediate(entry["links_file"], "snapshot links scratch")
    if links_meta.get("dtype_tag") != LINKS_DTYPE_TAG:
        raise ConverterError(
            "{}: links-file dtype tag {!r} != expected {!r} — refusing to emit".format(
                entry["links_file"], links_meta.get("dtype_tag"), LINKS_DTYPE_TAG
            )
        )
    fixed = np.fromfile(entry["fixed_file"], dtype=FIXED_RECORD_DTYPE)
    links = np.fromfile(entry["links_file"], dtype=LINKS_RECORD_DTYPE)
    for what, count in (("fixed", fixed.size), ("links", links.size)):
        if count != entry["rows"]:
            raise ConverterError(
                "snapshot {}: {} file has {} rows, manifest records {}".format(
                    snap, what, count, entry["rows"]
                )
            )
    return fixed, links


def _consume_snapshot_scratch(manifest: Manifest, snap: int, delete: bool) -> None:
    """Delete-after-verify for one snapshot's fixed and links scratch files
    (plan Slice 8 deletion table).

    **The writer is the terminal consumer of both.** ``links`` reads the fixed
    file through ``_load_fixed`` and the writer reads it again here through
    ``_load_snapshot_scratch``, so neither file may be deleted inside the link
    stage. Callers reach this only once snapshot ``snap``'s emitted HDF5 is on
    the record: either just written, re-opened and verified dataset-by-dataset
    against the source arrays, or skip-trusted, which re-checks its recorded
    md5. The successor is durable before the predecessors go.

    ``delete`` is the run's opt-in flag; with it clear both files are retained,
    and only a removal a crash interrupted between the unlink and the manifest
    save converges.
    """
    entry = manifest.data["snapshots"][str(snap)]
    for path in manifest.consume_intermediates(
        [entry["fixed_file"], entry["links_file"]], delete=delete
    ):
        _log("write: snapshot {} — consumed {}".format(snap, path))


def write_forests_sidecar(manifest: Manifest, output_dir: Path, n_forests_total: int) -> None:
    """Emit forests.h5 (single dataset /ForestID) from the Phase 0 table."""
    table_path = Path(manifest.workdir) / "forest_index_table.npy"
    manifest.verify_intermediate(table_path, "forest index table")
    table = np.ascontiguousarray(np.load(table_path), dtype=np.int64)
    if table.size != n_forests_total:
        raise ConverterError(
            "forest index table has {} entries, run-scoped n_forests_total is {}".format(
                table.size, n_forests_total
            )
        )
    path = output_dir / "forests.h5"
    if _skip_trust_output(manifest, path):
        _log("write: forests.h5 already recorded and unchanged — skipping")
        return
    tmp = path.with_suffix(".h5.tmp")
    with h5py.File(tmp, "w", libver="latest") as handle:
        handle.create_dataset(
            "ForestID", data=table, chunks=CHUNK_1D, maxshape=(None,), compression=None
        )
    os.replace(tmp, path)
    with h5py.File(path, "r") as handle:
        stored = handle["ForestID"][...]
        if stored.tobytes() != table.tobytes():
            raise ConverterError("{}: /ForestID re-read does not match the table".format(path))
    _record_output(manifest, path, int(table.size), "forests-sidecar")
    _log("write: forests.h5 — {} forest(s)".format(table.size))


def run_write(
    workdir, a_list_path, simulation_info_path, output_dir=None, consume_intermediates=False
) -> Manifest:
    """Emit the full horizontal-HDF5 dataset from the linked scratch files.

    Every a_list snapshot gets a file, including snapshots with zero halos.
    The a_list and simulation_info must be the manifest-recorded ones (same
    identity binding as the fix-up stage).

    ``consume_intermediates`` (CLI: ``--consume-intermediates``) turns on the
    plan Slice 8 deletion of each snapshot's fixed and links scratch once that
    snapshot's emitted file is verified and recorded. It is off by default and
    changes no emitted byte.
    """
    manifest = Manifest.load_or_create(workdir)
    if not manifest.path.exists():
        raise ConverterError("{}: no manifest found; run scatter first".format(workdir))
    reject_extended_layout(manifest)

    a_list, a_list_md5 = load_a_list(a_list_path)
    provenance = manifest.data["provenance"]
    recorded = provenance.get("a_list", {}).get("md5")
    if recorded != a_list_md5:
        raise ConverterError(
            "{}: a_list content md5 {} != manifest-recorded {} — the write stage must use "
            "the a_list the scatter stage validated against".format(
                a_list_path, a_list_md5, recorded
            )
        )
    sim_md5 = file_md5(simulation_info_path)
    recorded_info = provenance.get("simulation_info", {}).get("md5")
    if recorded_info != sim_md5:
        raise ConverterError(
            "{}: simulation_info content md5 {} != manifest-recorded {} — refusing to mix "
            "metadata across runs".format(simulation_info_path, sim_md5, recorded_info)
        )
    metadata = load_header_metadata(simulation_info_path)

    links_values = manifest.data.get("links")
    if links_values is None:
        raise ConverterError("{}: no run-scoped links values; run links first".format(workdir))
    n_forests_total = int(links_values["n_forests_total"])
    max_rank = int(links_values["max_halo_rank_in_forest"])

    snaps = sorted(int(s) for s in manifest.data["snapshots"])
    if not snaps:
        raise ConverterError("{}: manifest records no snapshots".format(workdir))
    for snap in snaps:
        status = manifest.data["snapshots"][str(snap)].get("status")
        if status != "linked":
            raise ConverterError(
                "snapshot {}: unexpected status {!r}; run links first".format(snap, status)
            )
        if snap < 0 or snap >= len(a_list):
            raise ConverterError(
                "snapshot {} is outside the a_list range [0, {})".format(snap, len(a_list))
            )

    output_dir = Path(output_dir) if output_dir is not None else Path(manifest.workdir) / "hdf5"
    output_dir.mkdir(parents=True, exist_ok=True)

    populated = set(snaps)
    n_written = 0
    n_skipped = 0
    n_consumed_inputs = 0
    for snap in range(len(a_list)):
        path = output_dir / snapshot_h5_name(snap)
        if _skip_trust_output(manifest, path):
            n_skipped += 1
            if snap in populated:
                if manifest.is_consumed(manifest.data["snapshots"][str(snap)]["fixed_file"]):
                    n_consumed_inputs += 1
                # an emitted file that is already recorded and unchanged is
                # this snapshot's terminal consumption, whether it happened on
                # this run or an earlier one
                _consume_snapshot_scratch(manifest, snap, consume_intermediates)
            continue
        if snap in populated:
            fixed, links = _load_snapshot_scratch(manifest, snap)
            entry = manifest.data["snapshots"][str(snap)]
            arrays = build_halo_arrays(fixed, links, snap, entry["fixed_file"])
        else:
            arrays = {}
        write_snapshot_file(
            path,
            snap,
            arrays,
            float(a_list[snap]),
            metadata,
            n_forests_total,
            max_rank,
        )
        verify_snapshot_file(path, snap, arrays, str(path))
        rows = arrays["MostBoundID"].size if arrays else 0
        _record_output(manifest, path, int(rows), "horizontal-hdf5")
        if snap in populated:
            _consume_snapshot_scratch(manifest, snap, consume_intermediates)
        n_written += 1

    write_forests_sidecar(manifest, output_dir, n_forests_total)
    manifest.data["outputs_dir"] = str(output_dir.resolve())
    manifest.save()
    _log(
        "write: {} snapshot file(s) written, {} skipped (already recorded), {} empty, "
        "output dir {}".format(n_written, n_skipped, len(a_list) - len(populated), output_dir)
    )
    if n_consumed_inputs:
        _log(
            "write: {} skipped snapshot(s) had their fixed and links scratch consumed by an "
            "earlier verified emission — nothing to re-read".format(n_consumed_inputs)
        )
    return manifest


# ==========================================================================
# Horizontal-HDF5 format version 3
# ==========================================================================
#
# docs/dev/HORIZONTAL-HDF5-FORMAT-V3-DRAFT.md (contract C3 of
# docs/dev/MIMIC-CONVERTER-GENERALISATION-IMPLEMENTATION-PLAN.md, approved at
# Gate G1). The writer consumes the generic pipeline's verified transposed
# snapshots -- flat little-endian records whose fields ARE the v3 /halos
# columns (transpose.output_dtype) -- and changes no value: it moves bytes
# from a record column into an HDF5 dataset of the same declared type.
#
# What it adds over the records is only what a whole-dataset view decides:
#
# - ``links_adjacent``, measured here over Descendant links and required to
#   agree with the transpose's own independent count;
# - the run-scoped identity headers (``n_forests_total`` from the forest
#   enumeration, ``max_halo_rank_in_forest`` measured over the records);
# - the physical headers from simulation_info.yaml, validated with the same
#   unit rules as v2 (``load_header_metadata``);
# - ``/schema``, one subgroup per payload/extra declaration of the embedded
#   schema, and the two v3 string headers.
#
# Memory is one block of ``block_rows`` records and one block of forests at a
# time; nothing is O(halos) or O(forests).

#: The format version this section writes. Never 2: a v3 dataset is not a v2
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

#: The ``type`` of the three /schema declarations whose storage the fixed
#: format table pins rather than the producer (C3: SnapNum int32, Len int32,
#: MostBoundID int64).
_V3_PINNED_PAYLOAD_TYPES = {"SnapNum": "int", "Len": "int", "MostBoundID": "long long"}

SIDECAR_NAME = "forests.h5"


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
    max_descendant_span: int
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
    Where the recorded conversion already pinned physical metadata -- the
    forests-HDF5 route's ``particle_mass``, the ASCII route's
    ``simulation_info`` file -- the writer requires agreement before writing.

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
        elif inputs.source_format == "consistent_trees_ascii":
            recorded = sha256_file(parameters["simulation_info"])
            if recorded != self._simulation_info_sha256:
                raise ConverterError(
                    "{}: the ASCII conversion was prepared against a different simulation_info "
                    "(sha256 {}) than the writer was given ({}); refusing to mix metadata".format(
                        parameters["simulation_info"], recorded, self._simulation_info_sha256
                    )
                )
        elif inputs.source_format == "lhalo_binary" and "simulation_info" in parameters:
            recorded = sha256_file(parameters["simulation_info"])
            if recorded != self._simulation_info_sha256:
                raise ConverterError(
                    "{}: the L-Halo conversion was recorded against a different simulation_info "
                    "(sha256 {}) than the writer was given ({}); refusing to mix metadata".format(
                        parameters["simulation_info"], recorded, self._simulation_info_sha256
                    )
                )

    def _measure(self, inputs: WriteInputs) -> V3Measurement:
        """One bounded pass over the transposed records: per-snapshot counts,
        descendant gaps (in a_list positions), maximum rank and ForestIndex.
        The gap count must equal the transpose's own, which it measured from
        source keys rather than from these rows."""
        counts = []
        gapped = 0
        max_span = 0
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
                gapped += int(np.count_nonzero(span != 1))
                if span.size:
                    max_span = max(max_span, int(span.max()))
                if block.size:
                    max_rank = max(max_rank, int(block["HaloRankInForest"].max()))
                    max_forest = max(max_forest, int(block["ForestIndex"].max()))
        measured = V3Measurement(tuple(counts), gapped, max_span, max_rank, max_forest)
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
