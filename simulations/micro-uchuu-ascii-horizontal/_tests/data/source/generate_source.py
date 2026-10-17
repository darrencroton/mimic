#!/usr/bin/env python3
"""Regenerate the Consistent-Trees ASCII source behind this package's worked_ascii fixture.

Run from the repository root:

    mimic_venv/bin/python \\
        simulations/micro-uchuu-ascii-horizontal/_tests/data/source/generate_source.py

Writes three files into this directory: ``tree_0_0_0.dat`` (one Consistent-Trees
ASCII tree file, indexed header dialect, a bare tree-count line and one ``#tree``
block per tree), ``forests.list`` (every tree root to its forest) and
``worked_ascii.a_list`` (the fixture's six scale factors, one per line).
``../regenerate.sh`` converts them with ``convert/mimic-convert/convert_trees.py``
into ``../worked_ascii/`` (via ``convert_worked_ascii.sh`` beside this script).

The scale factors are the last six of a vertical Consistent-Trees ASCII package's
own snapshot list (its ``simulation_info.yaml`` ``input.snapshot_list_file``),
renumbered from 0, so the fixture's timesteps have that package's real spacing.
``--package`` names the package (default ``micro-uchuu-ascii``, the source of
this horizontal package); ``--output-dir`` writes elsewhere, which is how
``convert_worked_ascii.sh`` builds the same forests at each package's scale factors in a
temporary directory.

The fixture is three forests holding four trees in all (forest 10 has two, forests 20 and 30
one each) over six snapshots, chosen to exercise
every case the horizontal reader and driver must handle:

- snapshot 0 is empty and snapshots 1-5 are populated;
- halo 1010 has three progenitors, so its NextProgenitor chain has three members;
- forest 20 carries a two-member FoF group (2010 central, 2011 satellite), and
  2013 is a satellite of 2012 one snapshot earlier;
- forest 10 spans two trees both alive at its last snapshot (1010 and 1020 are
  independent ``pid == -1`` centrals there), so one forest holds two
  self-central halos in the same snapshot;
- forest 30 ends at snapshot 2, well before the final snapshot.

Every phase-space value derives deterministically from the halo id, and the
columns the converter does not select (num_prog, phantom, Rvir, Tree_root_ID)
are written too. Deterministic and idempotent: re-running reproduces
byte-identical files.
"""

import argparse
import os
import sys
from pathlib import Path

import yaml

_REPO = Path(__file__).resolve().parents[5]
OUT_DIR = Path(__file__).resolve().parent

#: number of snapshots in the fixture (the last this many of the package's list)
N_SNAPSHOTS = 6

TREE_FILE_NAME = "tree_0_0_0.dat"
FORESTS_LIST_NAME = "forests.list"
A_LIST_NAME = "worked_ascii.a_list"

#: written ctrees column layout (indexed header dialect)
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


def package_scale_factors(package):
    """The last N_SNAPSHOTS scale factors of ``simulations/<package>``'s snapshot list."""
    info_path = _REPO / "simulations" / package / "simulation_info.yaml"
    with open(info_path, "r") as handle:
        a_list_rel = (yaml.safe_load(handle).get("input") or {}).get("snapshot_list_file")
    if not a_list_rel:
        raise SystemExit("{} declares no input.snapshot_list_file".format(info_path))
    values = [
        float(line.split("#", 1)[0])
        for line in (_REPO / a_list_rel).read_text().splitlines()
        if line.split("#", 1)[0].strip()
    ]
    if len(values) < N_SNAPSHOTS:
        raise SystemExit("{} holds fewer than {} scale factors".format(a_list_rel, N_SNAPSHOTS))
    return values[-N_SNAPSHOTS:]


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
    """Return (forests, trees): forest id -> [tree root ids], tree root id -> [halo rows]."""
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


def write_ctrees_file(path, trees, a_list):
    """Write the ctrees ASCII tree file, one '#tree' block per tree root in id order."""
    header = "#" + " ".join("{}({})".format(name, i) for i, name in enumerate(COLUMNS))
    lines = [header, "#Synthetic horizontal-HDF5 fixture source", str(len(trees))]
    for root_id in sorted(trees):
        lines.append("#tree {}".format(root_id))
        for halo in trees[root_id]:
            scale = a_list[halo["snap"]]
            desc_scale = -1.0 if halo["desc_id"] == -1 else a_list[halo["snap"] + 1]
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
    """Write ``forests.list``: one ``TreeRootID ForestID`` line per tree root."""
    lines = ["#TreeRootID ForestID"]
    for forest_id in sorted(forests):
        for root_id in forests[forest_id]:
            lines.append("{} {}".format(root_id, forest_id))
    Path(path).write_text("\n".join(lines) + "\n")


def write_a_list(path, a_list):
    """Write the fixture snapshot list, one scale factor per line."""
    Path(path).write_text("\n".join("{:.5f}".format(a) for a in a_list) + "\n")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument(
        "--package",
        default="micro-uchuu-ascii",
        help="vertical package whose last six scale factors the fixture uses "
        "(default: micro-uchuu-ascii)",
    )
    parser.add_argument(
        "--output-dir",
        type=Path,
        default=OUT_DIR,
        help="directory to write the three files into (default: this directory)",
    )
    args = parser.parse_args(argv)

    a_list = package_scale_factors(args.package)
    forests, trees = fixture_forests()
    out_dir = args.output_dir
    out_dir.mkdir(parents=True, exist_ok=True)
    write_ctrees_file(out_dir / TREE_FILE_NAME, trees, a_list)
    write_forests_list(out_dir / FORESTS_LIST_NAME, forests)
    write_a_list(out_dir / A_LIST_NAME, a_list)
    for name in (TREE_FILE_NAME, FORESTS_LIST_NAME, A_LIST_NAME):
        print("wrote {}".format(os.path.relpath(out_dir / name)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
