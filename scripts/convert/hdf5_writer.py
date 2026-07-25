"""Snapshot-HDF5 emission for the ctrees -> snapshot-HDF5 converter (plan Slice 7).

Emits ``snapshot_NNN.h5`` files and the ``forests.h5`` sidecar exactly per the
frozen contract in docs/dev/SNAPSHOT-HDF5-FORMAT.md (format_version = 1). The
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
"""

import os
import sys
from pathlib import Path
from typing import Dict, Optional, Tuple

import h5py
import numpy as np
import yaml

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from ctrees_parser import ConverterError  # noqa: E402
from fixups import (  # noqa: E402
    FIXED_DTYPE_TAG,
    FIXED_RECORD_DTYPE,
    PARTICLE_MASS_UNITS,
    REF_TO_NATIVE_MASS,
)
from links import LINKS_DTYPE_TAG, LINKS_RECORD_DTYPE  # noqa: E402
from scatter import Manifest, file_md5, load_a_list  # noqa: E402

#: format_version this writer implements (the frozen contract's ratchet).
FORMAT_VERSION = 1

#: Contract chunk shapes (docs/dev/SNAPSHOT-HDF5-FORMAT.md Storage Layout).
CHUNK_1D = (65536,)
CHUNK_VEC = (65536, 3)

#: Expected box-size units string in simulation_info.yaml (header attribute
#: box_size_mpc_h is Mpc/h comoving; any other units would corrupt it).
BOX_SIZE_UNITS = "Mpc/h"

#: Header attributes: name -> numpy dtype (docs/dev/SNAPSHOT-HDF5-FORMAT.md
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

#: /halos datasets: name -> (dtype, is_vec3) (docs/dev/SNAPSHOT-HDF5-FORMAT.md
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
            "int64 — no valid negated source-catalog id can take this value; example rows: "
            "{}".format(context, int(sentinel.sum()), ", ".join(str(int(r)) for r in rows))
        )
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


def run_write(workdir, a_list_path, simulation_info_path, output_dir=None) -> Manifest:
    """Emit the full snapshot-HDF5 dataset from the linked scratch files.

    Every a_list snapshot gets a file, including snapshots with zero halos.
    The a_list and simulation_info must be the manifest-recorded ones (same
    identity binding as the fix-up stage).
    """
    manifest = Manifest.load_or_create(workdir)
    if not manifest.path.exists():
        raise ConverterError("{}: no manifest found; run scatter first".format(workdir))

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
    for snap in range(len(a_list)):
        path = output_dir / snapshot_h5_name(snap)
        if _skip_trust_output(manifest, path):
            n_skipped += 1
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
        _record_output(manifest, path, int(rows), "snapshot-hdf5")
        n_written += 1

    write_forests_sidecar(manifest, output_dir, n_forests_total)
    manifest.data["outputs_dir"] = str(output_dir.resolve())
    manifest.save()
    _log(
        "write: {} snapshot file(s) written, {} skipped (already recorded), {} empty, "
        "output dir {}".format(n_written, n_skipped, len(a_list) - len(populated), output_dir)
    )
    return manifest
