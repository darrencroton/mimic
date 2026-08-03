#!/usr/bin/env python
"""Regenerate the committed snapshot-HDF5 contract fixtures for this package.

The fixture is produced by the real converter, never hand-written. In a scratch
temporary workdir this generator:

1. synthesises a tiny Consistent-Trees ASCII tree plus the ``forests.list`` and
   a_list the converter pipeline requires (header metadata comes from this
   package's own ``simulation_info.yaml``, so the fixture is self-consistent
   with the package that ships it);
2. runs the full ``scripts/convert/`` pipeline over it in production layout
   (scatter, sort, fixups, links, write);
3. runs ``scripts/convert/validate.py`` over that output and aborts unless the
   producer battery exits 0;
4. rewrites the validated datasets into ``_tests/data/`` with small chunk
   shapes, preserving the object set, dataset names, dtypes, shapes and every
   header attribute value exactly; and
5. re-opens the committed copy and asserts value equality against the validated
   production-layout dataset — every dataset element, every header attribute,
   and every ``/ForestID`` value — aborting on any difference.

Chunk shape is the only permitted difference between the two. The production
contract chunk shape is ``(65536,)``/``(65536, 3)``, which allocates 6.25 MiB
per populated snapshot file; the frozen spec makes chunk layout a storage
detail consumers must not depend on
(docs/dev/SNAPSHOT-HDF5-FORMAT.md, Storage Layout), so the committed copy is
re-chunked small and the conformance checker deliberately ignores chunk shape.

The generator also writes a canonical ``fixture_manifest.json``. The converter's
own ``manifest.json`` is unsuitable to commit verbatim: it records absolute
paths and source ``mtime_ns`` (scripts/convert/scatter.py, hdf5_writer.py), so
it is neither path-independent nor reproducible.

Usage:
    mimic_venv/bin/python \\
        simulations/micro-uchuu-snapshot/_tests/input/create_snapshot_fixture.py
    mimic_venv/bin/python \\
        simulations/micro-uchuu-snapshot/_tests/input/create_snapshot_fixture.py \\
        --compare-against <dir>   # value-equality assertion only, nothing written

Re-running regenerates byte-identical ``.h5`` files and a byte-identical
``fixture_manifest.json``.
"""

import argparse
import hashlib
import json
import subprocess
import sys
import tempfile
from pathlib import Path

import h5py
import numpy as np

PACKAGE_DIR = Path(__file__).resolve().parents[2]
REPO_ROOT = PACKAGE_DIR.parents[1]
DATA_DIR = PACKAGE_DIR / "_tests" / "data"
CONVERT_DIR = REPO_ROOT / "scripts" / "convert"
SIMULATION_INFO = PACKAGE_DIR / "simulation_info.yaml"

#: fixture snapshot list; index = SnapNum, ascending scale factor
A_LIST = [0.25000, 0.40000, 0.50000, 0.65000, 0.80000, 1.00000]

#: fixture a_list filename inside _tests/data/
A_LIST_NAME = "micro-uchuu-fixture.a_list"

#: committed-copy chunk shapes (small; the production contract is 65536)
CHUNK_1D_SMALL = (8,)
CHUNK_VEC_SMALL = (8, 3)

#: generated ctrees column layout (indexed header dialect), including columns
#: the converter ignores (num_prog, phantom, Rvir, Tree_root_ID)
COLUMNS = [
    "scale",
    "id",
    "desc_scale",
    "desc_id",
    "num_prog",
    "pid",
    "upid",
    "phantom",
    "Mvir",
    "Rvir",
    "vrms",
    "vmax",
    "x",
    "y",
    "z",
    "vx",
    "vy",
    "vz",
    "Jx",
    "Jy",
    "Jz",
    "Snap_num",
    "Tree_root_ID",
]

#: /halos datasets: name -> is_vec3 (docs/dev/SNAPSHOT-HDF5-FORMAT.md)
HALO_DATASET_IS_VEC = {
    "Descendant": False,
    "FirstProgenitor": False,
    "NextProgenitor": False,
    "FirstHaloInFOFgroup": False,
    "NextHaloInFOFgroup": False,
    "Len": False,
    "SnapNum": False,
    "M_Crit200": False,
    "Pos": True,
    "Vel": True,
    "Spin": True,
    "VelDisp": False,
    "Vmax": False,
    "MostBoundID": False,
    "ForestIndex": False,
    "HaloRankInForest": False,
}


class FixtureError(RuntimeError):
    """Fatal generator failure: abort, never write a half-made fixture."""


def log(message):
    print("fixture: {}".format(message), flush=True)


# ---------------------------------------------------------------------------
# Synthetic ctrees source
# ---------------------------------------------------------------------------


def _halo(halo_id, snap, mvir, desc_id=-1, pid=-1, upid=-1, num_prog=0):
    """One ctrees row; phase-space values derive deterministically from the id."""
    base = float(halo_id % 89)
    return {
        "id": halo_id,
        "snap": snap,
        "mvir": mvir,
        "desc_id": desc_id,
        "pid": pid,
        "upid": upid,
        "num_prog": num_prog,
        "x": 0.25 + base * 0.5,
        "y": 0.50 + base * 0.5,
        "z": 0.75 + base * 0.5,
        "vx": -50.0 + base,
        "vy": 25.0 + base,
        "vz": -12.5 + base,
        "jx": 1.0e10 + base * 1.0e8,
        "jy": -2.0e10 + base * 1.0e8,
        "jz": 3.0e10 + base * 1.0e8,
        "vrms": 80.0 + base,
        "vmax": 160.0 + base,
    }


def fixture_forests():
    """The fixture topology, chosen to exercise every contract feature the
    reader must handle:

    - snapshot 0 is empty and snapshots 1-5 are populated (six snapshots);
    - halo 1010 has three progenitors, so the NextProgenitor chain exercises
      the reference incremental-insertion loop's mid-chain head replacement;
    - forest 20 carries a two-member FoF group (2010 central, 2011 satellite);
    - forest 10 spans two trees both alive at its max snapshot, so fix_flybys
      demotes tree 102's root and negates its MostBoundID;
    - forest 30 dies at snapshot 2, well before the final snapshot.

    Returns (forests, trees) where forests maps forest id -> [tree root ids]
    and trees maps tree root id -> [halo rows].
    """
    trees = {
        101: [
            _halo(1010, 5, 1.0e12, num_prog=3),
            _halo(1011, 4, 6.0e11, desc_id=1010, num_prog=1),
            _halo(1012, 4, 7.0e11, desc_id=1010),
            _halo(1013, 4, 5.0e11, desc_id=1010),
            _halo(1014, 3, 5.5e11, desc_id=1011),
        ],
        102: [
            _halo(1020, 5, 8.0e11, num_prog=1),
            _halo(1021, 4, 7.5e11, desc_id=1020),
        ],
        201: [
            _halo(2010, 5, 5.0e11, num_prog=1),
            _halo(2011, 5, 1.0e11, pid=2010, upid=2010, num_prog=1),
            _halo(2012, 4, 4.0e11, desc_id=2010),
            _halo(2013, 4, 0.9e11, desc_id=2011, pid=2012, upid=2012),
        ],
        301: [
            _halo(3010, 2, 3.0e11, num_prog=1),
            _halo(3011, 1, 2.0e11, desc_id=3010),
        ],
    }
    forests = {10: [101, 102], 20: [201], 30: [301]}
    return forests, trees


def write_ctrees_file(path, trees):
    """Write the synthetic ctrees ASCII file (indexed header dialect, bare
    tree-count line, one '#tree' block per tree)."""
    header = "#" + " ".join("{}({})".format(name, i) for i, name in enumerate(COLUMNS))
    lines = [header, "#Synthetic snapshot-HDF5 fixture source", str(len(trees))]
    for root_id in sorted(trees):
        lines.append("#tree {}".format(root_id))
        for halo in trees[root_id]:
            scale = A_LIST[halo["snap"]]
            desc_scale = -1.0 if halo["desc_id"] == -1 else A_LIST[halo["snap"] + 1]
            values = {
                "scale": "{:.5f}".format(scale),
                "id": str(halo["id"]),
                "desc_scale": "{:.5f}".format(desc_scale),
                "desc_id": str(halo["desc_id"]),
                "num_prog": str(halo["num_prog"]),
                "pid": str(halo["pid"]),
                "upid": str(halo["upid"]),
                "phantom": "0",
                "Mvir": "{:.5e}".format(halo["mvir"]),
                "Rvir": "150.0",
                "vrms": "{:.4f}".format(halo["vrms"]),
                "vmax": "{:.4f}".format(halo["vmax"]),
                "x": "{:.5f}".format(halo["x"]),
                "y": "{:.5f}".format(halo["y"]),
                "z": "{:.5f}".format(halo["z"]),
                "vx": "{:.4f}".format(halo["vx"]),
                "vy": "{:.4f}".format(halo["vy"]),
                "vz": "{:.4f}".format(halo["vz"]),
                "Jx": "{:.5e}".format(halo["jx"]),
                "Jy": "{:.5e}".format(halo["jy"]),
                "Jz": "{:.5e}".format(halo["jz"]),
                "Snap_num": str(halo["snap"]),
                "Tree_root_ID": str(root_id),
            }
            lines.append(" ".join(values[c] for c in COLUMNS))
    Path(path).write_text("\n".join(lines) + "\n")


def write_forests_list(path, forests):
    lines = ["#TreeRootID ForestID"]
    for forest_id in sorted(forests):
        for root_id in forests[forest_id]:
            lines.append("{} {}".format(root_id, forest_id))
    Path(path).write_text("\n".join(lines) + "\n")


def write_a_list(path):
    Path(path).write_text("\n".join("{:.5f}".format(a) for a in A_LIST) + "\n")


# ---------------------------------------------------------------------------
# Converter pipeline
# ---------------------------------------------------------------------------


def run_step(argv, what):
    """Run one converter step, aborting with its output on a non-zero exit."""
    log("running {}".format(what))
    completed = subprocess.run(argv, capture_output=True, text=True)
    if completed.returncode != 0:
        raise FixtureError(
            "{} failed with exit code {}\n--- stdout ---\n{}\n--- stderr ---\n{}".format(
                what, completed.returncode, completed.stdout, completed.stderr
            )
        )
    log("{} exit code 0".format(what))
    return completed


def produce_production_dataset(workdir):
    """Synthesise the source and run the full converter pipeline plus the
    producer validation battery. Returns the production-layout dataset dir."""
    workdir = Path(workdir)
    source_dir = workdir / "source"
    source_dir.mkdir(parents=True)
    convert_dir = workdir / "convert"

    forests, trees = fixture_forests()
    tree_file = source_dir / "tree_0_0_0.dat"
    forests_list = source_dir / "forests.list"
    a_list = source_dir / A_LIST_NAME
    write_ctrees_file(tree_file, trees)
    write_forests_list(forests_list, forests)
    write_a_list(a_list)
    log("synthesised ctrees source: {} tree(s), {} forest(s)".format(len(trees), len(forests)))

    cli = [sys.executable, str(CONVERT_DIR / "convert_ctrees.py")]
    common = ["--workdir", str(convert_dir)]
    run_step(
        cli
        + ["scatter"]
        + common
        + [
            "--forests-list",
            str(forests_list),
            "--a-list",
            str(a_list),
            "--simulation-info",
            str(SIMULATION_INFO),
            str(tree_file),
        ],
        "converter scatter",
    )
    run_step(cli + ["sort"] + common, "converter sort")
    run_step(
        cli
        + ["fixups"]
        + common
        + ["--a-list", str(a_list), "--simulation-info", str(SIMULATION_INFO)],
        "converter fixups",
    )
    run_step(cli + ["links"] + common, "converter links")
    run_step(
        cli
        + ["write"]
        + common
        + ["--a-list", str(a_list), "--simulation-info", str(SIMULATION_INFO)],
        "converter write",
    )

    dataset_dir = convert_dir / "hdf5"
    run_step(
        [
            sys.executable,
            str(CONVERT_DIR / "validate.py"),
            str(dataset_dir),
            "--a-list",
            str(a_list),
            "--manifest",
            str(convert_dir / "manifest.json"),
        ],
        "producer validation battery (scripts/convert/validate.py)",
    )
    log("production-layout output passed scripts/convert/validate.py with exit code 0")
    return dataset_dir


# ---------------------------------------------------------------------------
# Re-chunked committed copy
# ---------------------------------------------------------------------------


def rechunk_file(source_path, dest_path):
    """Copy one dataset file, preserving everything except chunk shape.

    ``track_times=False`` keeps the output byte-reproducible: HDF5 otherwise
    stamps object modification times into the object headers.
    """
    tmp = Path(str(dest_path) + ".tmp")
    with h5py.File(source_path, "r") as src, h5py.File(tmp, "w", libver="latest") as dst:
        if "header" in src:
            header = dst.create_group("header", track_order=False)
            for name in sorted(src["header"].attrs):
                value = np.asarray(src["header"].attrs[name])
                header.attrs.create(name, value, dtype=value.dtype)
            halos = dst.create_group("halos")
            for name in sorted(HALO_DATASET_IS_VEC):
                data = src["halos"][name][...]
                is_vec = HALO_DATASET_IS_VEC[name]
                halos.create_dataset(
                    name,
                    data=data,
                    chunks=CHUNK_VEC_SMALL if is_vec else CHUNK_1D_SMALL,
                    maxshape=(None, 3) if is_vec else (None,),
                    compression=None,
                    track_times=False,
                )
        else:
            data = src["ForestID"][...]
            dst.create_dataset(
                "ForestID",
                data=data,
                chunks=CHUNK_1D_SMALL,
                maxshape=(None,),
                compression=None,
                track_times=False,
            )
    tmp.replace(dest_path)


# ---------------------------------------------------------------------------
# Value equality between production layout and the committed copy
# ---------------------------------------------------------------------------


def snapshot_files(directory):
    directory = Path(directory)
    return sorted(p.name for p in directory.glob("snapshot_*.h5"))


def assert_value_equality(production_dir, copy_dir):
    """Assert every dataset element, every header attribute, and every
    /ForestID value is identical between the two directories. Chunk shape is
    the only permitted difference; nothing else may vary."""
    production_dir = Path(production_dir)
    copy_dir = Path(copy_dir)
    prod_names = snapshot_files(production_dir)
    copy_names = snapshot_files(copy_dir)
    if prod_names != copy_names:
        raise FixtureError(
            "snapshot file set differs: production {} vs copy {}".format(prod_names, copy_names)
        )
    if not (copy_dir / "forests.h5").is_file():
        raise FixtureError("{}: forests.h5 is missing".format(copy_dir))

    for name in prod_names:
        with h5py.File(production_dir / name, "r") as prod, h5py.File(copy_dir / name, "r") as copy:
            prod_attrs = set(prod["header"].attrs)
            copy_attrs = set(copy["header"].attrs)
            if prod_attrs != copy_attrs:
                raise FixtureError(
                    "{}: header attribute set differs: missing {}, extra {}".format(
                        name,
                        sorted(prod_attrs - copy_attrs),
                        sorted(copy_attrs - prod_attrs),
                    )
                )
            for attr in sorted(prod_attrs):
                want = np.asarray(prod["header"].attrs[attr])
                got = np.asarray(copy["header"].attrs[attr])
                if want.dtype != got.dtype or want.shape != got.shape:
                    raise FixtureError(
                        "{}: header attribute {} is {} {} in the copy, {} {} in production".format(
                            name, attr, got.dtype, got.shape, want.dtype, want.shape
                        )
                    )
                if want.tobytes() != got.tobytes():
                    raise FixtureError(
                        "{}: header attribute {} value {!r} != production {!r}".format(
                            name, attr, got.item(), want.item()
                        )
                    )
            prod_sets = set(prod["halos"].keys())
            copy_sets = set(copy["halos"].keys())
            if prod_sets != copy_sets:
                raise FixtureError(
                    "{}: /halos dataset set differs: missing {}, extra {}".format(
                        name, sorted(prod_sets - copy_sets), sorted(copy_sets - prod_sets)
                    )
                )
            for dataset in sorted(prod_sets):
                want = prod["halos"][dataset][...]
                got = copy["halos"][dataset][...]
                if want.dtype != got.dtype:
                    raise FixtureError(
                        "{}: /halos/{} dtype {} != production {}".format(
                            name, dataset, got.dtype, want.dtype
                        )
                    )
                if want.shape != got.shape:
                    raise FixtureError(
                        "{}: /halos/{} shape {} != production {}".format(
                            name, dataset, got.shape, want.shape
                        )
                    )
                if want.tobytes() != got.tobytes():
                    bad = np.nonzero(np.asarray(want).reshape(-1) != np.asarray(got).reshape(-1))[
                        0
                    ][:5]
                    raise FixtureError(
                        "{}: /halos/{} values differ from production at flat index/indices "
                        "{}".format(name, dataset, bad.tolist())
                    )

    with h5py.File(production_dir / "forests.h5", "r") as prod, h5py.File(
        copy_dir / "forests.h5", "r"
    ) as copy:
        want = prod["ForestID"][...]
        got = copy["ForestID"][...]
        if want.dtype != got.dtype or want.shape != got.shape:
            raise FixtureError(
                "forests.h5: /ForestID is {} {} in the copy, {} {} in production".format(
                    got.dtype, got.shape, want.dtype, want.shape
                )
            )
        if want.tobytes() != got.tobytes():
            bad = np.nonzero(want != got)[0][:5]
            raise FixtureError(
                "forests.h5: /ForestID values differ from production at index/indices "
                "{}".format(bad.tolist())
            )
    log("value equality vs the validated production-layout dataset: OK")


# ---------------------------------------------------------------------------
# Canonical fixture manifest
# ---------------------------------------------------------------------------


def build_manifest(data_dir):
    """Only stable, path-independent facts: no absolute paths, no mtimes."""
    data_dir = Path(data_dir)
    files = {}
    for name in snapshot_files(data_dir) + ["forests.h5"]:
        with h5py.File(data_dir / name, "r") as handle:
            entry = {}
            if "header" in handle:
                entry["header_attributes"] = {
                    attr: {
                        "dtype": str(np.asarray(handle["header"].attrs[attr]).dtype),
                        "value": np.asarray(handle["header"].attrs[attr]).item(),
                    }
                    for attr in sorted(handle["header"].attrs)
                }
                entry["datasets"] = {
                    dataset: _dataset_entry(handle["halos"][dataset])
                    for dataset in sorted(handle["halos"].keys())
                }
            else:
                entry["datasets"] = {"ForestID": _dataset_entry(handle["ForestID"])}
            files[name] = entry
    manifest = {
        "generator": "simulations/micro-uchuu-snapshot/_tests/input/create_snapshot_fixture.py",
        "format_specification": "docs/dev/SNAPSHOT-HDF5-FORMAT.md",
        "format_version": 1,
        "a_list": list(A_LIST),
        "a_list_file": A_LIST_NAME,
        "chunk_shape_1d": list(CHUNK_1D_SMALL),
        "chunk_shape_vec3": list(CHUNK_VEC_SMALL),
        "n_snapshots": len(A_LIST),
        "files": files,
    }
    return manifest


def _dataset_entry(dataset):
    values = dataset[...]
    return {
        "dtype": str(values.dtype),
        "shape": list(values.shape),
        "md5": hashlib.md5(values.tobytes()).hexdigest(),
    }


def write_manifest(data_dir, manifest):
    text = json.dumps(manifest, indent=2, sort_keys=True) + "\n"
    (Path(data_dir) / "fixture_manifest.json").write_text(text)


# ---------------------------------------------------------------------------
# Fixture content assertions
# ---------------------------------------------------------------------------


def assert_fixture_features(data_dir):
    """The fixture must actually exercise the cases the reader has to handle."""
    data_dir = Path(data_dir)
    names = snapshot_files(data_dir)
    if len(names) < 4:
        raise FixtureError("fixture has {} snapshot(s), at least 4 required".format(len(names)))
    empty = 0
    populated = 0
    max_progenitors = 0
    max_fof_members = 0
    negative_ids = 0
    # FirstProgenitor indexes snapshot N-1 and the NextProgenitor chain lives
    # in that same earlier file, so the walk needs the previous file's arrays
    previous_next_prog = np.zeros(0, dtype=np.int32)
    for name in names:
        with h5py.File(data_dir / name, "r") as handle:
            halos = handle["halos"]
            n_halos = int(handle["header"].attrs["n_halos"])
            first_prog = halos["FirstProgenitor"][...]
            for head in first_prog[first_prog >= 0]:
                length = 1
                node = int(head)
                while True:
                    node = int(previous_next_prog[node])
                    if node < 0:
                        break
                    length += 1
                max_progenitors = max(max_progenitors, length)
            previous_next_prog = halos["NextProgenitor"][...]
            if n_halos == 0:
                empty += 1
                continue
            populated += 1
            negative_ids += int((halos["MostBoundID"][...] < 0).sum())
            first_fof = halos["FirstHaloInFOFgroup"][...]
            next_fof = halos["NextHaloInFOFgroup"][...]
            for index in range(n_halos):
                if int(first_fof[index]) != index:
                    continue
                length = 1
                node = index
                while True:
                    node = int(next_fof[node])
                    if node < 0:
                        break
                    length += 1
                max_fof_members = max(max_fof_members, length)
    failures = []
    if empty < 1:
        failures.append("no snapshot with zero halos")
    if populated < 1:
        failures.append("no populated snapshot")
    if max_progenitors < 3:
        failures.append(
            "longest progenitor chain is {}, need a descendant with 3 or more".format(
                max_progenitors
            )
        )
    if max_fof_members < 2:
        failures.append(
            "largest FoF group has {} member(s), need 2 or more".format(max_fof_members)
        )
    if negative_ids < 1:
        failures.append("no halo with a negative MostBoundID")
    if failures:
        raise FixtureError("fixture content requirements unmet: {}".format("; ".join(failures)))
    log(
        "fixture content: {} snapshot(s) ({} empty, {} populated), longest progenitor chain {}, "
        "largest FoF group {}, {} negative MostBoundID".format(
            len(names), empty, populated, max_progenitors, max_fof_members, negative_ids
        )
    )


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------


def regenerate(data_dir):
    with tempfile.TemporaryDirectory(prefix="mimic-snapshot-fixture-") as workdir:
        production_dir = produce_production_dataset(workdir)
        data_dir = Path(data_dir)
        data_dir.mkdir(parents=True, exist_ok=True)
        for stale in list(data_dir.glob("*.h5")):
            stale.unlink()
        for name in snapshot_files(production_dir) + ["forests.h5"]:
            rechunk_file(production_dir / name, data_dir / name)
        write_a_list(data_dir / A_LIST_NAME)
        log("wrote re-chunked copy to {}".format(data_dir))
        assert_value_equality(production_dir, data_dir)
        assert_fixture_features(data_dir)
        write_manifest(data_dir, build_manifest(data_dir))
        log("wrote fixture_manifest.json")


def compare_only(candidate_dir):
    with tempfile.TemporaryDirectory(prefix="mimic-snapshot-fixture-") as workdir:
        production_dir = produce_production_dataset(workdir)
        assert_value_equality(production_dir, candidate_dir)


def main(argv=None):
    parser = argparse.ArgumentParser(
        prog="create_snapshot_fixture",
        description="Regenerate the committed snapshot-HDF5 contract fixtures for "
        "simulations/micro-uchuu-snapshot",
    )
    parser.add_argument(
        "--compare-against",
        metavar="DIR",
        default=None,
        help="run only the value-equality assertion against DIR (writes nothing)",
    )
    args = parser.parse_args(argv)
    try:
        if args.compare_against is not None:
            compare_only(Path(args.compare_against))
        else:
            regenerate(DATA_DIR)
    except FixtureError as exc:
        print("ERROR: {}".format(exc), file=sys.stderr)
        return 1
    log("done")
    return 0


if __name__ == "__main__":
    sys.exit(main())
