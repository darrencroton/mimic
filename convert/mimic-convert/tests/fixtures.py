"""Shared fixtures for the converter tests.

The one module every test module imports its builders from; no test module
imports another. It holds, in order:

* the shipped-profile and repository paths;
* the synthetic Consistent-Trees ASCII catalogue builders: hand-specified tiny
  forests with known topology (multi-tree forests, multi-progenitor halos,
  mass ties, multiple independent FoF groups at a forest's maximum snapshot,
  unresolvable-satellite forests, early-dying forests, zero-mass halos,
  sub-subhalos whose pid differs from their ultimate host, and the adversarial
  cut fixtures with their decided tables), which the fix-up, link and rewriter
  tests reuse;
* the L-Halo binary builders (the hand-packed oracle layout and file writer);
* the forests-HDF5 builders (hand-laid forests and the source writer);
* the hand-specified literal gapped graph (three trees, its expected
  per-snapshot columns, and its L-Halo schema loader);
* the workdir and conversion builders that run a fixture through the pipeline
  stages (scatter, fix-ups, ingest, transpose, write, rewrite) and the
  vertical-assembly oracle forests;
* small assertion and capture helpers.

Snapshot numbering follows the a_list convention: SnapNum indexes A_LIST.
"""

import contextlib
import functools
import hashlib
import io
import os
import struct
import sys
from dataclasses import dataclass, field
from pathlib import Path
from types import SimpleNamespace
from typing import Callable, List, Optional, Sequence, Tuple

import h5py
import numpy as np

CONVERT_DIR = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(CONVERT_DIR))

import column_schema as cs  # noqa: E402
import pipeline  # noqa: E402
import scatter  # noqa: E402
from adapters.ctrees_ascii import prepare_workdir  # noqa: E402
from column_schema import build_schema, load_column_map  # noqa: E402
from ctrees_parser import (  # noqa: E402
    DEFAULT_CHUNKSIZE,
    CtreesFileParser,
    ParseResult,
    PreScan,
    ScratchLayout,
)
from fixups import fixed_layout  # noqa: E402
from hdf5_writer_v3 import V3_SIDECAR_DATASETS, HorizontalV3Writer  # noqa: E402
from rewriter.inputs import RewriteRequest  # noqa: E402
from rewriter.run import run_rewrite  # noqa: E402
from sort_index import run_sort  # noqa: E402
from vertical.assemble import LINK_FIELDS, AssembledForest  # noqa: E402

# ---------------------------------------------------------------------------
# Paths and shipped profiles
# ---------------------------------------------------------------------------

HERE = Path(__file__).resolve().parent
REPO_ROOT = HERE.parents[2]
PROFILE_DIR = CONVERT_DIR / "profiles"
#: the shipped Consistent-Trees ASCII profile every ASCII-route test parses under
DEFAULT_PROFILE = PROFILE_DIR / "consistent_trees_ascii.yaml"
ALL_TYPES_PROFILE = HERE / "data" / "column_maps" / "ascii_all_extra_types.yaml"
MINI_MILLENNIUM_PROPERTIES = REPO_ROOT / "simulations" / "mini-millennium" / "halo_properties.yaml"

# ---------------------------------------------------------------------------
# Synthetic Consistent-Trees ASCII catalogue builders
# ---------------------------------------------------------------------------

#: canonical fixture a_list: scale factor per snapshot number
A_LIST = [0.5, 0.6, 0.7, 0.8, 0.9, 1.0]

#: generated-file column layout (indexed header order); includes columns the
#: converter must ignore (num_prog, phantom, Rvir, Tree_root_ID)
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


@dataclass
class HaloSpec:
    """One ctrees halo row; pos/vel/J default deterministically from the id."""

    halo_id: int
    snap: int
    mvir: float
    desc_id: int = -1
    pid: int = -1
    upid: int = -1
    num_prog: int = 0
    x: Optional[float] = None
    y: Optional[float] = None
    z: Optional[float] = None
    vx: Optional[float] = None
    vy: Optional[float] = None
    vz: Optional[float] = None
    jx: Optional[float] = None
    jy: Optional[float] = None
    jz: Optional[float] = None
    vrms: Optional[float] = None
    vmax: Optional[float] = None

    def __post_init__(self):
        base = float(self.halo_id % 89)
        if self.x is None:
            self.x = 0.25 + base * 0.5
        if self.y is None:
            self.y = 0.50 + base * 0.5
        if self.z is None:
            self.z = 0.75 + base * 0.5
        if self.vx is None:
            self.vx = -50.0 + base
        if self.vy is None:
            self.vy = 25.0 + base
        if self.vz is None:
            self.vz = -12.5 + base
        if self.jx is None:
            self.jx = 1.0e10 + base * 1.0e8
        if self.jy is None:
            self.jy = -2.0e10 + base * 1.0e8
        if self.jz is None:
            self.jz = 3.0e10 + base * 1.0e8
        if self.vrms is None:
            self.vrms = 80.0 + base
        if self.vmax is None:
            self.vmax = 160.0 + base


@dataclass
class TreeSpec:
    root_id: int
    halos: List[HaloSpec] = field(default_factory=list)


@dataclass
class ForestSpec:
    forest_id: int
    trees: List[TreeSpec] = field(default_factory=list)


#: An appended fixture column: header name, and the token each halo row carries.
ExtraColumn = Tuple[str, Callable[["HaloSpec"], str]]


def _row_text(
    halo: HaloSpec,
    tree: TreeSpec,
    a_list: Sequence[float],
    extra_columns: Sequence[ExtraColumn] = (),
) -> str:
    scale = a_list[halo.snap]
    if halo.desc_id == -1:
        desc_scale = -1.0
    else:
        desc_scale = a_list[halo.snap + 1]
    values = {
        "scale": "{:.5f}".format(scale),
        "id": str(halo.halo_id),
        "desc_scale": "{:.5f}".format(desc_scale),
        "desc_id": str(halo.desc_id),
        "num_prog": str(halo.num_prog),
        "pid": str(halo.pid),
        "upid": str(halo.upid),
        "phantom": "0",
        "Mvir": "{:.5e}".format(halo.mvir),
        "Rvir": "150.0",
        "vrms": "{:.4f}".format(halo.vrms),
        "vmax": "{:.4f}".format(halo.vmax),
        "x": "{:.5f}".format(halo.x),
        "y": "{:.5f}".format(halo.y),
        "z": "{:.5f}".format(halo.z),
        "vx": "{:.4f}".format(halo.vx),
        "vy": "{:.4f}".format(halo.vy),
        "vz": "{:.4f}".format(halo.vz),
        "Jx": "{:.5e}".format(halo.jx),
        "Jy": "{:.5e}".format(halo.jy),
        "Jz": "{:.5e}".format(halo.jz),
        "Snap_num": str(halo.snap),
        "Tree_root_ID": str(tree.root_id),
    }
    tokens = [values[c] for c in COLUMNS]
    tokens.extend(render(halo) for _name, render in extra_columns)
    return " ".join(tokens)


def header_line(
    dialect: str = "indexed",
    snapshot_column: str = "Snap_num",
    extra_names: Sequence[str] = (),
) -> str:
    names = [snapshot_column if c == "Snap_num" else c for c in COLUMNS] + list(extra_names)
    if dialect == "indexed":
        return "#" + " ".join("{}({})".format(name, i) for i, name in enumerate(names))
    if dialect == "fields":
        return "#fields: " + " ".join(names)
    raise ValueError("unknown header dialect: {}".format(dialect))


def write_ctrees_file(
    path,
    trees: Sequence[TreeSpec],
    a_list: Sequence[float] = A_LIST,
    dialect: str = "indexed",
    snapshot_column: str = "Snap_num",
    header_override: Optional[str] = None,
    tree_count: Optional[int] = None,
    include_tree_count: bool = True,
    extra_columns: Sequence[ExtraColumn] = (),
) -> Path:
    """Write a synthetic ctrees file. Real files carry a bare tree-count line
    before the first '#tree' marker, so the generator writes one by default;
    pass ``tree_count`` to write a deliberately wrong value.

    ``extra_columns`` appends columns after the standard set, each a header
    name and a function rendering one halo's token -- the source text a
    declared extra field is then parsed from, verbatim."""
    path = Path(path)
    lines = [
        (
            header_override
            if header_override is not None
            else header_line(dialect, snapshot_column, [name for name, _render in extra_columns])
        )
    ]
    lines.append("#Synthetic converter test fixture")
    if include_tree_count:
        lines.append(str(tree_count if tree_count is not None else len(trees)))
    for tree in trees:
        lines.append("#tree {}".format(tree.root_id))
        for halo in tree.halos:
            lines.append(_row_text(halo, tree, a_list, extra_columns))
    path.write_text("\n".join(lines) + "\n")
    return path


def write_forests_list(path, forests: Sequence[ForestSpec]) -> Path:
    path = Path(path)
    lines = ["#TreeRootID ForestID"]
    for forest in forests:
        for tree in forest.trees:
            lines.append("{} {}".format(tree.root_id, forest.forest_id))
    path.write_text("\n".join(lines) + "\n")
    return path


def write_a_list(path, a_list: Sequence[float] = A_LIST) -> Path:
    path = Path(path)
    path.write_text("\n".join("{:.5f}".format(a) for a in a_list) + "\n")
    return path


def write_simulation_info(path, particle_mass: float = 0.0325, box: float = 100.0) -> Path:
    path = Path(path)
    path.write_text(
        "simulation:\n"
        "  cosmology: {omega_matter: 0.3089, omega_lambda: 0.6911, hubble_h: 0.6774}\n"
        "  box_size: {value: %r, units: Mpc/h}\n"
        "  particle_mass: {value: %r, units: 1e10 Msun/h}\n" % (box, particle_mass)
    )
    return path


# ---------------------------------------------------------------------------
# Canned forests (one topology case each)
# ---------------------------------------------------------------------------


def multi_tree_forest() -> ForestSpec:
    """Forest 100: two trees; multi-progenitor halo with a mass tie; two
    independent pid==-1 centrals (1010, 1020) at the forest max snapshot 5.

    Both centrals must SURVIVE as self-central (``pid == -1``, ``upid == id``,
    positive ``MostBoundID``): ``fix_flybys``, which used to demote all but the
    most massive of them, was removed as scientifically wrong. Neither
    central has a subhalo at snapshot 5, so this forest alone does not cover
    the per-group membership half of that property —
    :func:`multi_fof_survivor_forest` does.
    """
    tree_a = TreeSpec(
        root_id=101,
        halos=[
            HaloSpec(halo_id=1010, snap=5, mvir=1.0e12),
            HaloSpec(halo_id=1011, snap=4, mvir=6.0e11, desc_id=1010, num_prog=0),
            HaloSpec(halo_id=1012, snap=4, mvir=6.0e11, desc_id=1010, num_prog=0),
            HaloSpec(halo_id=1013, snap=3, mvir=5.0e11, desc_id=1011),
        ],
    )
    tree_a.halos[0].num_prog = 2
    tree_b = TreeSpec(
        root_id=102,
        halos=[
            HaloSpec(halo_id=1020, snap=5, mvir=8.0e11),
            HaloSpec(halo_id=1021, snap=4, mvir=7.0e11, desc_id=1020),
        ],
    )
    tree_b.halos[0].num_prog = 1
    return ForestSpec(forest_id=100, trees=[tree_a, tree_b])


def satellite_forest() -> ForestSpec:
    """Forest 200: one central with a satellite at each of two snapshots."""
    tree = TreeSpec(
        root_id=201,
        halos=[
            HaloSpec(halo_id=2010, snap=5, mvir=5.0e11, num_prog=1),
            HaloSpec(halo_id=2011, snap=5, mvir=1.0e11, pid=2010, upid=2010, num_prog=1),
            HaloSpec(halo_id=2012, snap=4, mvir=4.0e11, desc_id=2010),
            HaloSpec(halo_id=2013, snap=4, mvir=0.9e11, desc_id=2011, pid=2012, upid=2012),
        ],
    )
    return ForestSpec(forest_id=200, trees=[tree])


def zero_central_forest() -> ForestSpec:
    """Forest 300: zero pid==-1 centrals at the forest max snapshot.

    Corrupt input: 3010 is the forest's only halo at snapshot 5 and claims to
    be a satellite of 3011, which lives at snapshot 4. This is exactly the
    case :func:`fixups.verify_fof_centrals_present` rejects directly, before
    :func:`fixups.fix_upid_snapshot` ever runs, so the guard ``fix_flybys``
    used to provide does not depend on fix_upid's unrelated chain-resolution
    failure.
    """
    tree = TreeSpec(
        root_id=301,
        halos=[
            HaloSpec(halo_id=3010, snap=5, mvir=2.0e11, pid=3011, upid=3011),
            HaloSpec(halo_id=3011, snap=4, mvir=3.0e11, desc_id=3010),
        ],
    )
    return ForestSpec(forest_id=300, trees=[tree])


def early_dying_forest() -> ForestSpec:
    """Forest 400: dies at snapshot 2, well before the global final snapshot."""
    tree = TreeSpec(
        root_id=401,
        halos=[
            HaloSpec(halo_id=4010, snap=2, mvir=3.0e11, num_prog=1),
            HaloSpec(halo_id=4011, snap=1, mvir=2.0e11, desc_id=4010),
        ],
    )
    return ForestSpec(forest_id=400, trees=[tree])


def early_dying_flyby_forest() -> ForestSpec:
    """Forest 700: dies at snapshot 2 with TWO pid==-1 centrals there.

    A forest whose maximum snapshot (2) is well before the global final one
    (5). Both 7010 and 7011 must survive as self-central there: nothing demotes
    an independent FoF group at a forest's max snapshot any more.
    Complements :func:`multi_fof_survivor_forest`, which covers the same
    property at the global final snapshot and with subhalos attached.
    """
    tree_a = TreeSpec(
        root_id=701,
        halos=[
            HaloSpec(halo_id=7010, snap=2, mvir=4.0e11, num_prog=1),
            HaloSpec(halo_id=7012, snap=1, mvir=3.5e11, desc_id=7010),
        ],
    )
    tree_b = TreeSpec(
        root_id=702,
        halos=[HaloSpec(halo_id=7011, snap=2, mvir=3.0e11)],
    )
    return ForestSpec(forest_id=700, trees=[tree_a, tree_b])


def multi_fof_survivor_forest() -> ForestSpec:
    """Forest 800: two independent FoF groups at the forest max snapshot 5,
    EACH with its own subhalo there.

    The regression fixture for the removal of ``fix_flybys``: 8010 (1e12) and 8020 (8e11) are
    both ``pid == -1`` at snapshot 5, and 8011 / 8021 are their respective
    subhalos. After the fix-ups both centrals must still be self-central and
    each subhalo must still resolve to ITS OWN central. ``fix_flybys`` would
    have demoted 8020 under 8010 and pulled 8021 into 8010's group with it,
    so any reintroduction of that collapse fails this fixture's test.
    """
    tree_a = TreeSpec(
        root_id=801,
        halos=[
            HaloSpec(halo_id=8010, snap=5, mvir=1.0e12),
            HaloSpec(halo_id=8011, snap=5, mvir=2.0e11, pid=8010, upid=8010),
        ],
    )
    tree_b = TreeSpec(
        root_id=802,
        halos=[
            HaloSpec(halo_id=8020, snap=5, mvir=8.0e11),
            HaloSpec(halo_id=8021, snap=5, mvir=1.5e11, pid=8020, upid=8020),
        ],
    )
    return ForestSpec(forest_id=800, trees=[tree_a, tree_b])


def zero_mass_forest() -> ForestSpec:
    """Forest 500: contains a zero-mass halo (Spin carve-out input)."""
    tree = TreeSpec(
        root_id=501,
        halos=[
            HaloSpec(halo_id=5010, snap=5, mvir=2.5e11),
            HaloSpec(halo_id=5011, snap=5, mvir=0.0, pid=5010, upid=5010),
        ],
    )
    return ForestSpec(forest_id=500, trees=[tree])


def sub_subhalo_forest() -> ForestSpec:
    """Forest 600: sub-subhalo whose pid (6011) differs from its ultimate host
    (6010); the fix_upid equivalent must set both to 6010."""
    tree = TreeSpec(
        root_id=601,
        halos=[
            HaloSpec(halo_id=6010, snap=5, mvir=2.0e12),
            HaloSpec(halo_id=6011, snap=5, mvir=2.0e11, pid=6010, upid=6010),
            HaloSpec(halo_id=6012, snap=5, mvir=5.0e10, pid=6011, upid=6010),
        ],
    )
    return ForestSpec(forest_id=600, trees=[tree])


def standard_forests() -> List[ForestSpec]:
    """The benign canned forests (parse/scatter/sort-safe). The zero-central
    forest is deliberately excluded: it exists to test the fix-up abort."""
    return [
        multi_tree_forest(),
        satellite_forest(),
        early_dying_forest(),
        zero_mass_forest(),
        sub_subhalo_forest(),
    ]


def all_trees(forests: Sequence[ForestSpec]) -> List[TreeSpec]:
    trees: List[TreeSpec] = []
    for forest in forests:
        trees.extend(forest.trees)
    return trees


# ---------------------------------------------------------------------------
# Correspondence-valid forests: each ``#tree`` root id is its terminal halo's id
# ---------------------------------------------------------------------------
#
# In Consistent-Trees output a ``#tree`` block holds the history of one root
# halo and is keyed by that halo's id. The canned forests above do not follow
# this (tree 101's terminal halo is 1010), which the ASCII adapter does not care
# about; the forest census does, because it recovers trees from terminal halos
# and compares their ids with the index files' tree roots. These forests keep
# exactly one terminal halo per tree, with the tree's root id, and give the
# census cross-tree FoF membership and a forest spanning files; the forest
# whose tree's root is not at the final snapshot is a separate negative fixture.


def correspondence_multi_tree_forest() -> ForestSpec:
    """Forest 1100: three trees; 1110 is a satellite of 1100 at snapshot 5 and
    1121 of 1101 at snapshot 4, so FoF groups join halos of different trees."""
    tree_a = TreeSpec(
        root_id=1100,
        halos=[
            HaloSpec(halo_id=1100, snap=5, mvir=2.0e12, num_prog=2),
            HaloSpec(halo_id=1101, snap=4, mvir=1.2e12, desc_id=1100, num_prog=1),
            HaloSpec(halo_id=1102, snap=4, mvir=5.0e11, desc_id=1100),
            HaloSpec(halo_id=1103, snap=3, mvir=9.0e11, desc_id=1101),
        ],
    )
    tree_b = TreeSpec(
        root_id=1110,
        halos=[
            HaloSpec(halo_id=1110, snap=5, mvir=3.0e11, pid=1100, upid=1100, num_prog=1),
            HaloSpec(halo_id=1111, snap=4, mvir=3.5e11, desc_id=1110),
        ],
    )
    tree_c = TreeSpec(
        root_id=1120,
        halos=[
            HaloSpec(halo_id=1120, snap=5, mvir=4.0e11, num_prog=1),
            HaloSpec(halo_id=1121, snap=4, mvir=2.0e11, desc_id=1120, pid=1101, upid=1101),
        ],
    )
    return ForestSpec(forest_id=1100, trees=[tree_a, tree_b, tree_c])


def correspondence_spanning_forest() -> ForestSpec:
    """Forest 1200: two trees meant for different files (a forest spanning
    files); 1210 is 1200's satellite at snapshot 5 and 1211 is 1201's at 4."""
    tree_a = TreeSpec(
        root_id=1200,
        halos=[
            HaloSpec(halo_id=1200, snap=5, mvir=1.0e12, num_prog=1),
            HaloSpec(halo_id=1201, snap=4, mvir=8.0e11, desc_id=1200, num_prog=1),
            HaloSpec(halo_id=1202, snap=3, mvir=6.0e11, desc_id=1201),
        ],
    )
    tree_b = TreeSpec(
        root_id=1210,
        halos=[
            HaloSpec(halo_id=1210, snap=5, mvir=2.0e11, pid=1200, upid=1200, num_prog=1),
            HaloSpec(halo_id=1211, snap=4, mvir=1.5e11, desc_id=1210, pid=1201, upid=1201),
            HaloSpec(halo_id=1212, snap=3, mvir=1.0e11, desc_id=1211),
        ],
    )
    return ForestSpec(forest_id=1200, trees=[tree_a, tree_b])


def correspondence_early_dying_forest() -> ForestSpec:
    """Forest 1300: one tree whose root halo is at snapshot 2, not the final one."""
    tree = TreeSpec(
        root_id=1300,
        halos=[
            HaloSpec(halo_id=1300, snap=2, mvir=3.0e11, num_prog=1),
            HaloSpec(halo_id=1301, snap=1, mvir=2.0e11, desc_id=1300),
        ],
    )
    return ForestSpec(forest_id=1300, trees=[tree])


def correspondence_single_tree_forest() -> ForestSpec:
    """Forest 1400: one tree, a main branch over three snapshots."""
    tree = TreeSpec(
        root_id=1400,
        halos=[
            HaloSpec(halo_id=1400, snap=5, mvir=6.0e11, num_prog=1),
            HaloSpec(halo_id=1401, snap=4, mvir=5.0e11, desc_id=1400, num_prog=1),
            HaloSpec(halo_id=1402, snap=3, mvir=4.0e11, desc_id=1401),
        ],
    )
    return ForestSpec(forest_id=1400, trees=[tree])


def correspondence_forests() -> List[ForestSpec]:
    """The correspondence-valid forests, in ascending forest id: every tree
    reaches the final snapshot."""
    return [
        correspondence_multi_tree_forest(),
        correspondence_spanning_forest(),
        correspondence_single_tree_forest(),
    ]


def early_ending_correspondence_forests() -> List[ForestSpec]:
    """A negative fixture: the correspondence-valid forests with forest 1300,
    whose one tree ends at snapshot 2, in ascending forest id. The census
    reports the early root; the decided cut table refuses the catalogue."""
    multi, spanning, single = correspondence_forests()
    return [multi, spanning, correspondence_early_dying_forest(), single]


# ---------------------------------------------------------------------------
# A forest enumerated last whose halo ids are not the highest
# ---------------------------------------------------------------------------
#
# Forest 8000 has a forest id above every other fixture forest's, so beside any
# of them it is enumerated last, while its halo ids (2000-2002) lie below those
# of the cut fixtures' forests (2100 to 2700): halo-id order and the
# (ForestIndex, HaloRankInForest) row order disagree.


def interleaved_forest_8000() -> ForestSpec:
    """Forest 8000: one tree (halo ids 2000-2002) with two progenitors at
    snapshot 4."""
    tree = TreeSpec(
        root_id=2000,
        halos=[
            HaloSpec(halo_id=2000, snap=5, mvir=7.0e11, num_prog=2),
            HaloSpec(halo_id=2001, snap=4, mvir=3.0e11, desc_id=2000),
            HaloSpec(halo_id=2002, snap=4, mvir=3.5e11, desc_id=2000),
        ],
    )
    return ForestSpec(forest_id=8000, trees=[tree])


# ---------------------------------------------------------------------------
# Adversarial cut fixtures: correspondence-valid forests with a declared cut table
# ---------------------------------------------------------------------------
#
# Each forest below is cut by the table its ``*_cut_table`` function states: a
# map from every tree root of the forest to the forest id its piece takes
# after the cut. Every tree reaches the final snapshot 5 and every ``#tree``
# root is its terminal halo, so the reference reader can read the forests
# vertically, and every table is the decided one for its forest -- each tree
# joins the forest of its terminal halo's FoF group at snapshot 5 -- so that
# no halo is promoted at the final snapshot. Fresh forest ids are assigned
# over the catalogue the tests build (forests 1400, the fixture forest and
# 8000, in that order), so they start at 8001 and follow descending piece
# size; the largest piece keeps the forest's id (ties by the smallest root,
# which no fixture relies on). Each table is written in ``forests.list`` shape
# (:func:`write_cut_table`).


def severed_satellite_forest() -> ForestSpec:
    """Forest 2100: tree 2110's halo at snapshot 4 is a satellite of tree 2100's;
    at snapshot 5 both roots are centrals of their own groups. The decided
    table severs the satellite: 2111 is promoted at snapshot 4, {2100} (3 halos)
    keeps the id and {2110} (2 halos) takes the fresh id."""
    tree_a = TreeSpec(
        root_id=2100,
        halos=[
            HaloSpec(halo_id=2100, snap=5, mvir=2.0e12, num_prog=1),
            HaloSpec(halo_id=2101, snap=4, mvir=1.5e12, desc_id=2100, num_prog=1),
            HaloSpec(halo_id=2102, snap=3, mvir=1.0e12, desc_id=2101),
        ],
    )
    tree_b = TreeSpec(
        root_id=2110,
        halos=[
            HaloSpec(halo_id=2110, snap=5, mvir=5.0e11, num_prog=1),
            HaloSpec(halo_id=2111, snap=4, mvir=4.0e11, desc_id=2110, pid=2101, upid=2101),
        ],
    )
    return ForestSpec(forest_id=2100, trees=[tree_a, tree_b])


def severed_satellite_cut_table(fresh: int = 8001) -> dict:
    return {2100: 2100, 2110: fresh}


def no_initial_central_forest() -> ForestSpec:
    """Forest 2200: tree 2210 is a satellite of tree 2200's halos at snapshots
    3 and 4 and a central only at snapshot 5, so its piece holds no central
    before the cut at those snapshots; 2211 and 2212 are promoted. {2200}
    (4 halos) keeps the id, {2210} (3 halos) is fresh."""
    tree_a = TreeSpec(
        root_id=2200,
        halos=[
            HaloSpec(halo_id=2200, snap=5, mvir=2.0e12, num_prog=1),
            HaloSpec(halo_id=2201, snap=4, mvir=1.5e12, desc_id=2200, num_prog=1),
            HaloSpec(halo_id=2202, snap=3, mvir=1.0e12, desc_id=2201, num_prog=1),
            HaloSpec(halo_id=2203, snap=2, mvir=8.0e11, desc_id=2202),
        ],
    )
    tree_b = TreeSpec(
        root_id=2210,
        halos=[
            HaloSpec(halo_id=2210, snap=5, mvir=5.0e11, num_prog=1),
            HaloSpec(
                halo_id=2211, snap=4, mvir=4.0e11, desc_id=2210, pid=2201, upid=2201, num_prog=1
            ),
            HaloSpec(halo_id=2212, snap=3, mvir=3.0e11, desc_id=2211, pid=2202, upid=2202),
        ],
    )
    return ForestSpec(forest_id=2200, trees=[tree_a, tree_b])


def no_initial_central_cut_table(fresh: int = 8001) -> dict:
    return {2200: 2200, 2210: fresh}


def central_leaves_forest() -> ForestSpec:
    """Forest 2300: at snapshot 4 the central 2301 (tree 2300) hosts 2311 (tree
    2310) and 2321 (tree 2320); at snapshot 5 tree 2300's root is alone and
    2320 is 2310's satellite. The decided table puts {2310, 2320} (4 halos)
    under the forest's id 2300 and {2300} (2 halos) under the fresh id: the
    central leaves while its two members stay together, each promoted to its
    own central at snapshot 4 (one remnant of two members)."""
    tree_a = TreeSpec(
        root_id=2300,
        halos=[
            HaloSpec(halo_id=2300, snap=5, mvir=3.0e12, num_prog=1),
            HaloSpec(halo_id=2301, snap=4, mvir=2.5e12, desc_id=2300),
        ],
    )
    tree_b = TreeSpec(
        root_id=2310,
        halos=[
            HaloSpec(halo_id=2310, snap=5, mvir=8.0e11, num_prog=1),
            HaloSpec(halo_id=2311, snap=4, mvir=6.0e11, desc_id=2310, pid=2301, upid=2301),
        ],
    )
    tree_c = TreeSpec(
        root_id=2320,
        halos=[
            HaloSpec(halo_id=2320, snap=5, mvir=3.0e11, pid=2310, upid=2310, num_prog=1),
            HaloSpec(halo_id=2321, snap=4, mvir=2.5e11, desc_id=2320, pid=2301, upid=2301),
        ],
    )
    return ForestSpec(forest_id=2300, trees=[tree_a, tree_b, tree_c])


def central_leaves_cut_table(fresh: int = 8001) -> dict:
    return {2300: fresh, 2310: 2300, 2320: 2300}


def progenitor_tie_forest() -> ForestSpec:
    """Forest 2400: 2450 (tree 2450) has two progenitors of equal mass at
    snapshot 4, the central 2451 and 2452, a satellite of tree 2410's 2411.
    Before the cut 2452's key (2411, 2411, 2452) is encountered before 2451's
    (2451, -1, 2451), so the insertion loop keeps 2452 in front of the equally
    massive 2451: FirstProgenitor 2452. The decided table severs 2452 (its
    key becomes (2452, -1, 2452), after 2451's), so the loop keeps 2451 in
    front: FirstProgenitor 2451. {2450} (3 halos) keeps the id 2400, {2410}
    (2 halos) is fresh."""
    tree_b = TreeSpec(
        root_id=2410,
        halos=[
            HaloSpec(halo_id=2410, snap=5, mvir=9.0e11, num_prog=1),
            HaloSpec(halo_id=2411, snap=4, mvir=8.0e11, desc_id=2410),
        ],
    )
    tree_a = TreeSpec(
        root_id=2450,
        halos=[
            HaloSpec(halo_id=2450, snap=5, mvir=2.0e12, num_prog=2),
            HaloSpec(halo_id=2451, snap=4, mvir=1.0e12, desc_id=2450),
            HaloSpec(halo_id=2452, snap=4, mvir=1.0e12, desc_id=2450, pid=2411, upid=2411),
        ],
    )
    return ForestSpec(forest_id=2400, trees=[tree_b, tree_a])


def progenitor_tie_cut_table(fresh: int = 8001) -> dict:
    return {2410: fresh, 2450: 2400}


def three_piece_forest() -> ForestSpec:
    """Forest 2500 in three pieces: tree 2510 (5 halos) is the largest and
    keeps the id; {2520, 2530} (4 halos: 2530 is 2520's satellite at snapshot
    5) takes the first fresh id and {2500} (2 halos) the second. At snapshot
    4 the central 2511 hosts 2501 and 2521, and at snapshot 3 2512 hosts 2522:
    three promotions over two snapshots. Laid out with tree 2530 in a second
    file, the piece {2520, 2530} spans files."""
    tree_a = TreeSpec(
        root_id=2500,
        halos=[
            HaloSpec(halo_id=2500, snap=5, mvir=1.0e12, num_prog=1),
            HaloSpec(halo_id=2501, snap=4, mvir=9.0e11, desc_id=2500, pid=2511, upid=2511),
        ],
    )
    tree_b = TreeSpec(
        root_id=2510,
        halos=[
            HaloSpec(halo_id=2510, snap=5, mvir=3.0e12, num_prog=1),
            HaloSpec(halo_id=2511, snap=4, mvir=2.5e12, desc_id=2510, num_prog=1),
            HaloSpec(halo_id=2512, snap=3, mvir=2.0e12, desc_id=2511, num_prog=1),
            HaloSpec(halo_id=2513, snap=2, mvir=1.5e12, desc_id=2512, num_prog=1),
            HaloSpec(halo_id=2514, snap=1, mvir=1.2e12, desc_id=2513),
        ],
    )
    tree_c = TreeSpec(
        root_id=2520,
        halos=[
            HaloSpec(halo_id=2520, snap=5, mvir=6.0e11, num_prog=1),
            HaloSpec(
                halo_id=2521, snap=4, mvir=5.0e11, desc_id=2520, pid=2511, upid=2511, num_prog=1
            ),
            HaloSpec(halo_id=2522, snap=3, mvir=4.0e11, desc_id=2521, pid=2512, upid=2512),
        ],
    )
    tree_d = TreeSpec(
        root_id=2530,
        halos=[HaloSpec(halo_id=2530, snap=5, mvir=2.0e11, pid=2520, upid=2520)],
    )
    return ForestSpec(forest_id=2500, trees=[tree_a, tree_b, tree_c, tree_d])


def three_piece_cut_table(fresh: int = 8001) -> dict:
    return {2500: fresh + 1, 2510: 2500, 2520: fresh, 2530: fresh}


def z0_groups_forest() -> ForestSpec:
    """Forest 2600 split into its two snapshot-5 FoF groups by the decided
    table: {2600} and {2610, 2620} (2620 is 2610's satellite at snapshot 5).
    At snapshot 4, 2621 (tree 2620) is a satellite of 2601 (tree 2600) and is
    promoted; nothing is promoted at snapshot 5, so the output's final FoF
    groups equal the input's. {2610, 2620} (4 halos) keeps the id 2600,
    {2600} (3 halos) is fresh."""
    tree_a = TreeSpec(
        root_id=2600,
        halos=[
            HaloSpec(halo_id=2600, snap=5, mvir=2.0e12, num_prog=1),
            HaloSpec(halo_id=2601, snap=4, mvir=1.8e12, desc_id=2600, num_prog=1),
            HaloSpec(halo_id=2602, snap=3, mvir=1.5e12, desc_id=2601),
        ],
    )
    tree_b = TreeSpec(
        root_id=2610,
        halos=[
            HaloSpec(halo_id=2610, snap=5, mvir=7.0e11, num_prog=1),
            HaloSpec(halo_id=2611, snap=4, mvir=6.0e11, desc_id=2610),
        ],
    )
    tree_c = TreeSpec(
        root_id=2620,
        halos=[
            HaloSpec(halo_id=2620, snap=5, mvir=2.0e11, pid=2610, upid=2610, num_prog=1),
            HaloSpec(halo_id=2621, snap=4, mvir=1.5e11, desc_id=2620, pid=2601, upid=2601),
        ],
    )
    return ForestSpec(forest_id=2600, trees=[tree_a, tree_b, tree_c])


def z0_groups_cut_table(fresh: int = 8001) -> dict:
    return {2600: fresh, 2610: 2600, 2620: 2600}


def progenitor_head_forest() -> ForestSpec:
    """Forest 2700: 2750 (tree 2750) has three progenitors at snapshot 4, the
    centrals 2751 (7e11) and 2752 (5e11) and 2753 (7e11), a satellite of tree
    2710's 2711. Before the cut 2753 is encountered first ((2711, 2711, 2753)),
    2751 ties its mass and joins the tail, 2752 the tail: chain 2753, 2751,
    2752. The decided table severs 2753 ((2753, -1, 2753), encountered last):
    2751 starts, 2752 joins the tail, 2753 ties and joins the tail: chain
    2751, 2752, 2753, so the head is replaced and the old head ends the
    chain. {2750} (4 halos) keeps the id 2700, {2710} (2 halos) is fresh."""
    tree_b = TreeSpec(
        root_id=2710,
        halos=[
            HaloSpec(halo_id=2710, snap=5, mvir=9.0e11, num_prog=1),
            HaloSpec(halo_id=2711, snap=4, mvir=8.0e11, desc_id=2710),
        ],
    )
    tree_a = TreeSpec(
        root_id=2750,
        halos=[
            HaloSpec(halo_id=2750, snap=5, mvir=3.0e12, num_prog=3),
            HaloSpec(halo_id=2751, snap=4, mvir=7.0e11, desc_id=2750),
            HaloSpec(halo_id=2752, snap=4, mvir=5.0e11, desc_id=2750),
            HaloSpec(halo_id=2753, snap=4, mvir=7.0e11, desc_id=2750, pid=2711, upid=2711),
        ],
    )
    return ForestSpec(forest_id=2700, trees=[tree_b, tree_a])


def progenitor_head_cut_table(fresh: int = 8001) -> dict:
    return {2710: fresh, 2750: 2700}


def write_cut_table(path, table: dict, forests: Sequence[ForestSpec] = ()) -> Path:
    """A cut table in ``forests.list`` shape: ``table`` maps tree roots to
    their forest id after the cut; every tree of ``forests`` not in it keeps
    its forest's id."""
    rows = {}
    for forest in forests:
        for tree in forest.trees:
            rows[tree.root_id] = forest.forest_id
    rows.update(table)
    path = Path(path)
    lines = ["#TreeRootID ForestID"] + [
        "{} {}".format(root, forest) for root, forest in sorted(rows.items())
    ]
    path.write_text("\n".join(lines) + "\n")
    return path


# ---------------------------------------------------------------------------
# Many small forests
# ---------------------------------------------------------------------------


def small_forests(count: int, first_id: int = 100000) -> List[ForestSpec]:
    """``count`` forests of one tree each, in ascending forest id from ``first_id`` in steps
    of ten: the even-numbered ones a lone halo at the final snapshot, the odd-numbered ones a
    main branch of two halos (snapshots 5 and 4), so half the forests are absent from every
    slab but the last."""
    forests = []
    for index in range(count):
        root = first_id + 10 * index
        halos = [HaloSpec(halo_id=root, snap=5, mvir=1.0e12, num_prog=index % 2)]
        if index % 2:
            halos.append(HaloSpec(halo_id=root + 1, snap=4, mvir=9.0e11, desc_id=root))
        forests.append(ForestSpec(forest_id=root, trees=[TreeSpec(root_id=root, halos=halos)]))
    return forests


# ---------------------------------------------------------------------------
# L-Halo binary builders (hand-packed from struct RawHalo)
# ---------------------------------------------------------------------------


#: (name, struct code, component count, hand-declared byte offset). Written
#: out by hand from struct RawHalo (src/include/generated/raw_halo_defs.h),
#: not derived from the profile this adapter loads.
LHALO_ORACLE_LAYOUT = (
    ("Descendant", "i", 1, 0),
    ("FirstProgenitor", "i", 1, 4),
    ("NextProgenitor", "i", 1, 8),
    ("FirstHaloInFOFgroup", "i", 1, 12),
    ("NextHaloInFOFgroup", "i", 1, 16),
    ("Len", "i", 1, 20),
    ("M_Mean200", "f", 1, 24),
    ("M_Crit200", "f", 1, 28),
    ("M_TopHat", "f", 1, 32),
    ("Pos", "f", 3, 36),
    ("Vel", "f", 3, 48),
    ("VelDisp", "f", 1, 60),
    ("Vmax", "f", 1, 64),
    ("Spin", "f", 3, 68),
    ("MostBoundID", "q", 1, 80),
    ("SnapNum", "i", 1, 88),
    ("FileNr", "i", 1, 92),
    ("SubhaloIndex", "i", 1, 96),
    ("SubHalfMass", "f", 1, 100),
)


LHALO_ORACLE_RECORD_BYTES = 104


_LHALO_ORACLE_FORMAT = "".join(code * n for _name, code, n, _offset in LHALO_ORACLE_LAYOUT)


#: Every field's default, so a fixture row declares only what it means to say.
#: A lone halo that is its own FoF central with no links is the simplest
#: structurally valid row.
_LHALO_DEFAULTS = {
    "Descendant": -1,
    "FirstProgenitor": -1,
    "NextProgenitor": -1,
    "FirstHaloInFOFgroup": 0,
    "NextHaloInFOFgroup": -1,
    "Len": 1,
    "M_Mean200": 0.0,
    "M_Crit200": 0.0,
    "M_TopHat": 0.0,
    "Pos": (0.0, 0.0, 0.0),
    "Vel": (0.0, 0.0, 0.0),
    "VelDisp": 0.0,
    "Vmax": 0.0,
    "Spin": (0.0, 0.0, 0.0),
    "MostBoundID": 0,
    "SnapNum": 0,
    "FileNr": 0,
    "SubhaloIndex": 0,
    "SubHalfMass": 0.0,
}


def pack_lhalo_record(row, byte_order="<"):
    """Pack one halo by hand, in the oracle's own declared field order."""
    values = dict(_LHALO_DEFAULTS)
    values.update(row)
    flat = []
    for name, _code, n_components, _offset in LHALO_ORACLE_LAYOUT:
        value = values[name]
        if n_components == 1:
            flat.append(value)
        else:
            flat.extend(value)
    packed = struct.pack(byte_order + _LHALO_ORACLE_FORMAT, *flat)
    assert len(packed) == LHALO_ORACLE_RECORD_BYTES, len(packed)
    return packed


def write_lhalo_file(path, trees, byte_order="<", trailing=b"", drop_records=0):
    """Write one L-Halo file: Ntrees, totNHalos, per-tree counts, records.

    ``trailing`` and ``drop_records`` exist so a test can produce a file whose
    payload disagrees with its own header in either direction.
    """
    counts = [len(tree) for tree in trees]
    records = [pack_lhalo_record(row, byte_order) for tree in trees for row in tree]
    if drop_records:
        records = records[:-drop_records]
    with open(path, "wb") as handle:
        handle.write(struct.pack(byte_order + "i", len(trees)))
        handle.write(struct.pack(byte_order + "i", sum(counts)))
        for count in counts:
            handle.write(struct.pack(byte_order + "i", count))
        for record in records:
            handle.write(record)
        handle.write(trailing)
    return path


# ---------------------------------------------------------------------------
# Forests-HDF5 builders (hand-laid forests and the source writer)
# ---------------------------------------------------------------------------


#: The package value (simulations/*uchuu*/simulation_info.yaml), 1e10 Msun/h.
UCHUU_PARTICLE_MASS = 0.0327


HDF5_LINKS = (
    "Descendant",
    "FirstProgenitor",
    "NextProgenitor",
    "FirstHaloInFOFgroup",
    "NextHaloInFOFgroup",
)


HDF5_FLOATS = ("Mvir", "x", "y", "z", "vx", "vy", "vz", "Jx", "Jy", "Jz", "vrms", "vmax")


_HDF5_ROW_DEFAULTS = {
    "Descendant": -1,
    "FirstProgenitor": -1,
    "NextProgenitor": -1,
    "NextHaloInFOFgroup": -1,
    "Mvir": 3.27e10,
    "x": 1.5,
    "y": 2.5,
    "z": 3.5,
    "vx": 10.0,
    "vy": 20.0,
    "vz": 30.0,
    "Jx": 0.0,
    "Jy": 0.0,
    "Jz": 0.0,
    "vrms": 50.0,
    "vmax": 100.0,
    "snap": 0,
}


def hdf5_halo(**values):
    """One forest-local row; FirstHaloInFOFgroup defaults to the row itself."""
    return values


def hdf5_forest(forest_id, *rows):
    return {"id": forest_id, "rows": list(rows)}


def hdf5_lone_forest(forest_id, **values):
    """A one-halo forest: its own FoF central with no links."""
    return hdf5_forest(forest_id, hdf5_halo(**values))


def pack_hdf5_file(forests, order=None, first_id=1):
    """Lay out one file's forests contiguously in ``order`` (storage order).

    Returns the per-halo columns and the ForestInfo rows in *ForestInfo row
    order* (the order of ``forests``), with each forest's offset wherever
    ``order`` put it. Written by hand from the format description, not from
    the adapter.
    """
    order = list(range(len(forests))) if order is None else list(order)
    assert sorted(order) == list(range(len(forests)))
    columns = {name: [] for name in HDF5_LINKS + HDF5_FLOATS + ("id", "snap")}
    offsets = [0] * len(forests)
    cursor = 0
    next_id = first_id
    for index in order:
        offsets[index] = cursor
        for local, row in enumerate(forests[index]["rows"]):
            values = dict(_HDF5_ROW_DEFAULTS)
            values["FirstHaloInFOFgroup"] = local
            values["id"] = next_id
            next_id += 1
            values.update(row)
            for name in columns:
                columns[name].append(values[name])
        cursor += len(forests[index]["rows"])
    info = [
        (entry["id"], offsets[index], len(entry["rows"]), 1) for index, entry in enumerate(forests)
    ]
    return columns, info


FOREST_INFO_DTYPE = np.dtype(
    [
        ("ForestID", "<i8"),
        ("ForestHalosOffset", "<i8"),
        ("ForestNhalos", "<i8"),
        ("ForestNtrees", "<i8"),
    ]
)


def _write_file_group(group, forests, order, snap_name, snap_float, first_id, extra_columns):
    columns, info = pack_hdf5_file(forests, order, first_id)
    group.attrs["Nforests"] = np.int64(len(forests))
    group.attrs["Nhalos"] = np.int64(len(columns["id"]))
    group.attrs["contiguous-halo-props"] = np.int8(1)
    group.create_dataset("ForestInfo", data=np.array(info, dtype=FOREST_INFO_DTYPE))
    params = group.create_group("simulation_params")
    params.attrs["Boxsize"] = 100.0
    data = group.create_group("Forests")
    for name in HDF5_LINKS + ("id",):
        data.create_dataset(name, data=np.array(columns[name], dtype="<i8"))
    for name in HDF5_FLOATS:
        data.create_dataset(name, data=np.array(columns[name], dtype="<f8"))
    snap_dtype = "<f8" if snap_float else "<i8"
    data.create_dataset(snap_name, data=np.array(columns["snap"], dtype=snap_dtype))
    for name, values in (extra_columns or {}).items():
        data.create_dataset(name, data=np.asarray(values))
    return len(columns["id"])


def write_hdf5_source(
    directory,
    files,
    layout="external",
    snap_name="Snap_num",
    snap_float=False,
    extra_columns=None,
    root_totals=True,
):
    """Write a forests-HDF5 source by hand. ``files`` is a list of dicts with
    ``forests`` and optional ``order``/``extra_columns``.

    ``layout="external"`` is the full-Uchuu organisation (``info.h5`` with one
    ``ExternalLink`` per ``File<N>`` to ``forests_<N>.h5``); ``"internal"`` is
    the micro-Uchuu fixture's (``File<N>`` groups inside ``info.h5``).
    """
    info_path = os.path.join(directory, "info.h5")
    total_forests = 0
    total_halos = 0
    next_id = 1
    with h5py.File(info_path, "w") as info:
        info.attrs["Nfiles"] = np.int64(len(files))
        for ordinal, spec in enumerate(files):
            extras = spec.get("extra_columns", extra_columns)
            if layout == "external":
                name = "forests_{}.h5".format(ordinal)
                with h5py.File(os.path.join(directory, name), "w") as data:
                    n = _write_file_group(
                        data,
                        spec["forests"],
                        spec.get("order"),
                        snap_name,
                        snap_float,
                        next_id,
                        extras,
                    )
                info["File{}".format(ordinal)] = h5py.ExternalLink(name, "/")
            else:
                group = info.create_group("File{}".format(ordinal))
                n = _write_file_group(
                    group,
                    spec["forests"],
                    spec.get("order"),
                    snap_name,
                    snap_float,
                    next_id,
                    extras,
                )
            next_id += n
            total_halos += n
            total_forests += len(spec["forests"])
        if root_totals:
            info.attrs["TotNforests"] = np.int64(total_forests)
            info.attrs["TotNhalos"] = np.int64(total_halos)
    return info_path


def load_hdf5_schema(name="consistent_trees_hdf5.yaml"):
    return cs.build_schema(cs.load_column_map(os.path.join(PROFILE_DIR, name)))


#: A four-halo forest with a real merger: two z=0 halos (rows 0, 1) in one
#: FoF group, each with one progenitor at the previous snapshot (rows 2, 3).
HDF5_MERGER_FOREST = hdf5_forest(
    7,
    hdf5_halo(snap=5, FirstProgenitor=2, NextHaloInFOFgroup=1),
    hdf5_halo(snap=5, FirstProgenitor=3, FirstHaloInFOFgroup=0),
    hdf5_halo(snap=4, Descendant=0),
    hdf5_halo(snap=4, Descendant=1),
)


# ---------------------------------------------------------------------------
# The hand-specified literal gapped graph (three trees, eight halos)
# ---------------------------------------------------------------------------


LITERAL_BUDGET = 4 << 20


#: Tree A (file 0, tree 0): r0 at snap 3 has progenitors r1 (snap 2) and r2
#: (snap 0, a three-snapshot gap) as mixed-age siblings; r3 shares r1's FoF
#: group and ends early.
LITERAL_TREE_A = [
    dict(
        SnapNum=3, FirstProgenitor=1, FirstHaloInFOFgroup=0, MostBoundID=77, Len=10, M_Crit200=1.5
    ),
    dict(
        SnapNum=2,
        Descendant=0,
        NextProgenitor=2,
        FirstHaloInFOFgroup=1,
        NextHaloInFOFgroup=3,
        MostBoundID=-5,
        Len=9,
        M_Crit200=0.75,
    ),
    dict(SnapNum=0, Descendant=0, FirstHaloInFOFgroup=2, MostBoundID=11, Len=8, M_Crit200=0.25),
    dict(SnapNum=2, FirstHaloInFOFgroup=1, MostBoundID=12, Len=7, M_Crit200=0.125),
]


#: Tree B (file 0, tree 1): a lone halo.
LITERAL_TREE_B = [dict(SnapNum=3, FirstHaloInFOFgroup=0, MostBoundID=13, Len=6, M_Crit200=2.0)]


#: Tree C (file 1, tree 0): a chain whose root progenitor skips snapshot 1.
#: r0 duplicates tree A's MostBoundID, as real catalogs may.
LITERAL_TREE_C = [
    dict(SnapNum=3, FirstProgenitor=1, FirstHaloInFOFgroup=0, MostBoundID=77, Len=5, M_Crit200=3.0),
    dict(
        SnapNum=2,
        Descendant=0,
        FirstProgenitor=2,
        FirstHaloInFOFgroup=1,
        MostBoundID=-3,
        Len=4,
        M_Crit200=4.0,
    ),
    dict(SnapNum=0, Descendant=1, FirstHaloInFOFgroup=2, MostBoundID=14, Len=3, M_Crit200=5.0),
]


LITERAL_A_LIST_TEXT = "0.25\n0.5\n0.75\n1.0\n"


#: SourceHaloID = 1 + prefix position over (file, tree, row): A -> 1-4,
#: B -> 5, C -> 6-8. Rows per snapshot ascend in SourceHaloID. Links are
#: (row, target snapshot); -1 null.
_LITERAL_COLUMNS = (
    "SourceHaloID",
    "ForestIndex",
    "HaloRankInForest",
    "Descendant",
    "DescendantSnapshot",
    "FirstProgenitor",
    "FirstProgenitorSnapshot",
    "NextProgenitor",
    "NextProgenitorSnapshot",
    "FirstHaloInFOFgroup",
    "NextHaloInFOFgroup",
    "SnapNum",
    "Len",
    "MostBoundID",
    "M_Crit200",
)


_LITERAL_ROWS = {
    0: [
        (3, 0, 2, 0, 3, -1, -1, -1, -1, 0, -1, 0, 8, 11, 0.25),
        (8, 2, 2, 2, 2, -1, -1, -1, -1, 1, -1, 0, 3, 14, 5.0),
    ],
    1: [],
    2: [
        (2, 0, 1, 0, 3, -1, -1, 0, 0, 0, 1, 2, 9, -5, 0.75),
        (4, 0, 3, -1, -1, -1, -1, -1, -1, 0, -1, 2, 7, 12, 0.125),
        (7, 2, 1, 2, 3, 1, 0, -1, -1, 2, -1, 2, 4, -3, 4.0),
    ],
    3: [
        (1, 0, 0, -1, -1, 0, 2, -1, -1, 0, -1, 3, 10, 77, 1.5),
        (5, 1, 0, -1, -1, -1, -1, -1, -1, 1, -1, 3, 6, 13, 2.0),
        (6, 2, 0, -1, -1, 2, 2, -1, -1, 2, -1, 3, 5, 77, 3.0),
    ],
}


LITERAL_EXPECTED = {
    snap: {name: [row[i] for row in rows] for i, name in enumerate(_LITERAL_COLUMNS)}
    for snap, rows in _LITERAL_ROWS.items()
}


LITERAL_TOTAL_HALOS = 8


def lhalo_schema(profile="lhalo_binary.yaml", directory=PROFILE_DIR, properties=None):
    return cs.build_schema(
        cs.load_column_map(Path(directory) / profile),
        cs.load_source_properties(properties or MINI_MILLENNIUM_PROPERTIES),
    )


# ---------------------------------------------------------------------------
# Workdir and conversion builders
# ---------------------------------------------------------------------------


# Shipped-profile schemas, the scatter/fix-up workdirs and the ASCII route's conversions


def load_profile(name):
    column_map = cs.load_column_map(PROFILE_DIR / name)
    properties = None
    if column_map.source_format == "lhalo_binary":
        properties = cs.load_source_properties(MINI_MILLENNIUM_PROPERTIES)
    return cs.build_schema(column_map, properties)


def schema_of(path=ALL_TYPES_PROFILE):
    return build_schema(load_column_map(path))


@functools.lru_cache(maxsize=None)
def default_schema():
    """The shipped default Consistent-Trees ASCII profile's schema, built once:
    every stage test runs on the layout it selects unless it names another."""
    return build_schema(load_column_map(DEFAULT_PROFILE))


def default_layout():
    return ScratchLayout.from_schema(default_schema())


def make_fixed(rows):
    """Build an id-sorted fixed-record array (the default layout's) from per-halo dicts."""
    records = np.zeros(len(rows), dtype=fixed_layout(default_layout())[0])
    records["desc_id"] = -1
    records["desc_scale"] = -1.0
    records["pid"] = -1
    records["upid"] = -1
    for i, row in enumerate(rows):
        for key, value in row.items():
            records[key][i] = value
        if "MostBoundID" not in row:
            records["MostBoundID"][i] = row["id"]
    order = np.argsort(records["id"], kind="stable")
    return records[order]


def prepare_standard_forests(root: Path, schema, chunksize: int = 2):
    """Prepare the standard fixture forests over two files in ``schema``'s
    layout, up to and including links; returns the workdir manifest."""
    forests = standard_forests()
    files = [
        write_ctrees_file(root / "tree_0.dat", all_trees(forests[:3])),
        write_ctrees_file(root / "tree_1.dat", all_trees(forests[3:])),
    ]
    forests_list = write_forests_list(root / "forests.list", forests)
    a_list = write_a_list(root / "test.a_list")
    sim_info = write_simulation_info(root / "simulation_info.yaml")
    with capture_stderr():
        return prepare_workdir(
            schema, files, forests_list, a_list, sim_info, root / "workdir", chunksize=chunksize
        )


def make_scatter_workdir(root):
    """A directory whose top-level manifest is a current scatter manifest.

    The Consistent-Trees ASCII route keeps exactly this state in its
    preparation directory (``pipeline.ASCII_PREPARATION_DIR``): the scatter
    and sort stages run here in the layout of the shipped profile, over the
    canned fixture forests written under ``root/inputs``.
    Returns ``root/workdir``, every snapshot recorded ``sorted``.
    """
    root = Path(root)
    inputs = root / "inputs"
    inputs.mkdir(parents=True)
    forests = standard_forests()
    tree = write_ctrees_file(inputs / "tree_0_0_0.dat", all_trees(forests))
    workdir = root / "workdir"
    scatter.run_scatter(
        tree_files=[tree],
        forests_list_path=write_forests_list(inputs / "forests.list", forests),
        a_list_path=write_a_list(inputs / "fixture.a_list"),
        workdir=workdir,
        simulation_info_path=write_simulation_info(inputs / "simulation_info.yaml"),
        schema=load_profile("consistent_trees_ascii.yaml"),
    )
    run_sort(workdir)
    return workdir


def parse_file(
    path, schema, chunksize: int = DEFAULT_CHUNKSIZE
) -> Tuple[np.ndarray, ParseResult, PreScan]:
    """Parse a whole file into one array of ``schema``'s scratch layout
    (small-file convenience)."""
    parser = CtreesFileParser(path, schema, chunksize=chunksize)
    parts = list(parser.chunks())
    if parts:
        records = np.concatenate(parts)
    else:
        records = np.zeros(0, dtype=parser.dtype)
    return records, parser.result, parser.prescan


def write_locations(path, tree_files):
    """``locations.dat`` for ``tree_files`` (``FileID`` = list position): each
    ``#tree`` marker's root with the byte offset of its block's first data row."""
    lines = ["#TreeRootID FileID Offset Filename"]
    for file_id, tree_file in enumerate(tree_files):
        position = 0
        for line in Path(tree_file).read_bytes().splitlines(keepends=True):
            position += len(line)
            if line.startswith(b"#tree "):
                root = int(line.split()[1])
                lines.append("{} {} {} {}".format(root, file_id, position, Path(tree_file).name))
    Path(path).write_text("\n".join(lines) + "\n")
    return Path(path)


def convert_ascii(root: Path, file_trees, forests):
    """Convert fixture trees, ``file_trees[i]`` written to ``tree_<i>.dat``, to a
    version 3 dataset; returns the paths the census needs."""
    root.mkdir(parents=True)
    tree_files = [
        write_ctrees_file(root / "tree_{}.dat".format(i), trees_of_file)
        for i, trees_of_file in enumerate(file_trees)
    ]
    forests_list = write_forests_list(root / "forests.list", forests)
    locations = write_locations(root / "locations.dat", tree_files)
    sim_info = write_simulation_info(root / "simulation_info.yaml")
    a_list = write_a_list(root / "fixture.a_list")
    work = root / "work"
    pipeline.initialize(
        work,
        build_schema(load_column_map(DEFAULT_PROFILE)),
        {
            "tree_files": [str(path) for path in tree_files],
            "forests_list": str(forests_list),
            "simulation_info": str(sim_info),
        },
        a_list,
        ingest_max_rows=4,
        transpose_budget_bytes=4 << 20,
    )
    with capture_stderr():
        pipeline.run_ingest(work)
        pipeline.run_transpose(work)
        manifest = pipeline.run_write(work, HorizontalV3Writer(sim_info))
    return {
        "dataset": manifest.artifact_path(manifest.stage("write")["directory"]),
        "forests_list": forests_list,
        "locations": locations,
        "preparation_manifest": manifest.artifact_path(pipeline.ASCII_PREPARATION_DIR)
        / "manifest.json",
    }


#: Each adversarial cut fixture: its forest, its decided table and, for the
#: two laid over two files, the tree from which the second file starts (the
#: three-piece fixture's piece {2520, 2530} then spans both files).
CUT_FIXTURES = (
    ("severed", severed_satellite_forest, severed_satellite_cut_table, 1),
    ("no_central", no_initial_central_forest, no_initial_central_cut_table, None),
    ("leaves", central_leaves_forest, central_leaves_cut_table, None),
    ("tie", progenitor_tie_forest, progenitor_tie_cut_table, None),
    ("three", three_piece_forest, three_piece_cut_table, 3),
    ("z0", z0_groups_forest, z0_groups_cut_table, None),
    ("head", progenitor_head_forest, progenitor_head_cut_table, None),
)


def route_parameters(source, cut_table=None):
    """The route's parameters over ``source``'s files (as ``cut_source`` wrote
    them), with a cut table when one is given."""
    root = source["root"]
    parameters = {
        "tree_files": [str(path) for path in sorted(root.glob("tree_*.dat"))],
        "forests_list": str(root / "forests.list"),
        "simulation_info": str(root / "simulation_info.yaml"),
    }
    if cut_table is not None:
        parameters["cut_table"] = str(cut_table)
    return parameters


def convert_under_cut(source, work, cut_table):
    """The route's conversion of ``source``'s files under ``cut_table``, with
    the batch and budget ``cut_source``'s uncut conversion used."""
    pipeline.initialize(
        work,
        schema_of(DEFAULT_PROFILE),
        route_parameters(source, cut_table),
        source["root"] / "fixture.a_list",
        ingest_max_rows=4,
        transpose_budget_bytes=4 << 20,
    )
    with capture_stderr():
        pipeline.run_ingest(work)
        pipeline.run_transpose(work)
        manifest = pipeline.run_write(
            work, HorizontalV3Writer(source["root"] / "simulation_info.yaml")
        )
    return manifest


# The literal gapped L-Halo source converted to version 3


def v3_source_values(sid: int) -> dict:
    """Every non-topology L-Halo field of the literal halo with this
    SourceHaloID, as a literal function of it. Spin's last component is a
    signed zero, which must survive."""
    return {
        "M_Mean200": sid + 0.5,
        "M_TopHat": -0.25 * sid,
        "SubHalfMass": 1024.0 * sid + 0.125,
        "FileNr": -sid,
        "SubhaloIndex": 100 + sid,
        "Pos": (sid + 0.25, 2.0 * sid, 3.0 * sid),
        "Vel": (float(sid), -float(sid), 0.5),
        "Spin": (0.125 * sid, 1.0, -0.0),
        "VelDisp": 10.0 + sid,
        "Vmax": 20.0 + sid,
    }


def v3_literal_trees():
    """The literal graph's three trees (SourceHaloID A -> 1-4, B -> 5,
    C -> 6-8) with every other field set by :func:`v3_source_values`."""
    trees = []
    sid = 1
    for tree in (LITERAL_TREE_A, LITERAL_TREE_B, LITERAL_TREE_C):
        rows = []
        for row in tree:
            values = dict(row)
            values.update(v3_source_values(sid))
            rows.append(values)
            sid += 1
        trees.append(rows)
    return trees


def make_v3_conversion(
    root, profile="lhalo_binary_extras_example.yaml", *, write=True, block_rows=2
):
    """The literal gapped L-Halo source through initialize, ingest,
    transpose and (unless ``write`` is False) the v3 write stage, streaming in
    ``block_rows``-row blocks so even eight halos cross block boundaries."""
    root = Path(root)
    src = root / "src"
    src.mkdir(parents=True)
    tree_a, tree_b, tree_c = v3_literal_trees()
    file0 = write_lhalo_file(str(src / "trees.0"), [tree_a, tree_b])
    file1 = write_lhalo_file(str(src / "trees.1"), [tree_c])
    a_list = src / "a.list"
    a_list.write_text(LITERAL_A_LIST_TEXT)
    sim_info = write_simulation_info(src / "simulation_info.yaml")
    work = root / "work"
    schema = lhalo_schema(profile)
    pipeline.initialize(
        work,
        schema,
        {"sources": [[0, file0], [1, file1]]},
        a_list,
        ingest_max_rows=3,
        transpose_budget_bytes=LITERAL_BUDGET,
    )
    pipeline.run_ingest(work)
    pipeline.run_transpose(work)
    conversion = SimpleNamespace(
        work=work, a_list=a_list, sim_info=sim_info, schema=schema, manifest=None, dataset=None
    )
    if write:
        manifest = pipeline.run_write(work, HorizontalV3Writer(sim_info, block_rows=block_rows))
        conversion.manifest = manifest
        conversion.dataset = manifest.artifact_path(manifest.stage("write")["directory"])
    return conversion


# Rewriter requests, dataset readers and the cut-fixture layouts

N_FIXTURE_SNAPSHOTS = len(A_LIST)


REWRITE_MULTIPLIER = 10**9


def rewrite_request(operation, source: dict, input_dir, output_dir, scratch_dir, **overrides):
    """A request over one route's files (``source`` as the converter helpers
    return it, plus ``root``)."""
    values = dict(
        operation=operation,
        input_dir=Path(input_dir),
        output_dir=Path(output_dir),
        scratch_dir=Path(scratch_dir),
        simulation_info=source["root"] / "simulation_info.yaml",
        a_list=source["root"] / "fixture.a_list",
        forests_list=source["forests_list"],
        locations=source["locations"],
        multiplier=REWRITE_MULTIPLIER,
    )
    values.update(overrides)
    return RewriteRequest(**values)


def rewrite_quietly(operation, source, input_dir, output_dir, scratch_dir, **overrides):
    with quiet_output():
        return run_rewrite(
            rewrite_request(operation, source, input_dir, output_dir, scratch_dir, **overrides)
        )


def read_slab_file(path):
    with h5py.File(path, "r") as handle:
        halos = {name: handle["halos"][name][...] for name in handle["halos"]}
        header = {name: value for name, value in handle["header"].attrs.items()}
        schema = {
            name: {key: group.attrs[key] for key in group.attrs}
            for name, group in handle["schema"].items()
        }
    return halos, header, schema


def read_sidecar(path):
    with h5py.File(path, "r") as handle:
        return {name: handle[name][...] for name in handle}


def dataset_files(directory):
    return sorted(entry.name for entry in Path(directory).iterdir())


def cut_companions():
    """The two untouched forests every cut fixture is laid beside: 1400
    (before the fixture forest) and 8000 (after it, so its SourceHaloID
    prefix moves while its ForestIndex, ranks and links do not)."""
    return correspondence_single_tree_forest(), interleaved_forest_8000()


def cut_source(root: Path, forest, layout):
    first, last = cut_companions()
    source = convert_ascii(root, layout, [first, forest, last])
    source["root"] = root
    source["forests"] = [first, forest, last]
    return source


def cut_layout(forest, split_after=None):
    """One file holding everything, or two files with the fixture forest's
    trees from ``split_after`` on in the second (so a piece can span files)."""
    first, last = cut_companions()
    if split_after is None:
        return [[first.trees[0]] + forest.trees + [last.trees[0]]]
    return [
        [first.trees[0]] + forest.trees[:split_after],
        forest.trees[split_after:] + [last.trees[0]],
    ]


def write_table(root: Path, name: str, table: dict, source) -> Path:
    return write_cut_table(root / (name + ".list"), table, source["forests"])


def rewrite_cut(source, out, scratch, table, input_dir=None, **overrides):
    input_dir = source["dataset"] if input_dir is None else input_dir
    return rewrite_quietly("cut", source, input_dir, out, scratch, cut_table=table, **overrides)


# Vertical-assembly oracles and the dataset-editing helper

SHIN_UCHUU_RECORD = REPO_ROOT / "simulations" / "shin-uchuu-ascii" / "halo_properties.yaml"


WORKED_ASCII = REPO_ROOT / "simulations" / "shin-uchuu" / "_tests" / "data" / "worked_ascii"


# The correspondence dataset's forests by rank: (MostBoundID, Descendant, FirstProgenitor,
# NextProgenitor, FirstHaloInFOFgroup, NextHaloInFOFgroup, SnapNum). Snapshots descend; within
# snapshot 4 of forest 1100 the FoF groups are [1101, 1121], [1102], [1111].
FOREST_1100 = [
    (1100, -1, 3, -1, 0, 1, 5),
    (1110, -1, 6, -1, 0, -1, 5),
    (1120, -1, 4, -1, 2, -1, 5),
    (1101, 0, 7, 5, 3, 4, 4),
    (1121, 2, -1, -1, 3, -1, 4),
    (1102, 0, -1, -1, 5, -1, 4),
    (1111, 1, -1, -1, 6, -1, 4),
    (1103, 3, -1, -1, 7, -1, 3),
]


FOREST_1200 = [
    (1200, -1, 2, -1, 0, 1, 5),
    (1210, -1, 3, -1, 0, -1, 5),
    (1201, 0, 4, -1, 2, 3, 4),
    (1211, 1, 5, -1, 2, -1, 4),
    (1202, 2, -1, -1, 4, -1, 3),
    (1212, 3, -1, -1, 5, -1, 3),
]


FOREST_1300 = [(1300, -1, 1, -1, 0, -1, 2), (1301, 0, -1, -1, 1, -1, 1)]


FOREST_1400 = [(1400, -1, 1, -1, 0, -1, 5), (1401, 0, 2, -1, 1, -1, 4), (1402, 1, -1, -1, 2, -1, 3)]


def assembled_rows(forest: AssembledForest):
    """A forest's records as the oracle tuples."""
    return [
        (
            int(record["MostBoundID"]),
            *(int(record[name]) for name in LINK_FIELDS),
            int(record["SnapNum"]),
        )
        for record in forest.records
    ]


def convert_one_file_dataset(root, forests):
    return convert_ascii(root, [all_trees(forests)], forests)["dataset"]


def edit_column(dataset_dir, snap, name, edit):
    """Rewrite one ``/halos`` column of one slab in place with ``edit(values)``."""
    path = Path(dataset_dir) / "snapshot_{:03d}.h5".format(snap)
    with h5py.File(path, "r+") as handle:
        column = handle["halos"][name]
        values = column[...]
        edit(values)
        column[...] = values


# ---------------------------------------------------------------------------
# Assertion and capture helpers
# ---------------------------------------------------------------------------


def sha256_of(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def tree_state(root):
    """Every file and directory under ``root`` with size, mtime and hash."""
    state = {}
    for base, dirs, files in os.walk(root):
        for name in dirs:
            state[os.path.relpath(os.path.join(base, name), root) + "/"] = None
        for name in files:
            path = os.path.join(base, name)
            status = os.lstat(path)
            state[os.path.relpath(path, root)] = (
                status.st_size,
                status.st_mtime_ns,
                sha256_of(path),
            )
    return state


class _Captured:
    """Everything written to stderr inside a :func:`capture_stderr` block, as
    ``.text``; readable inside the block as well as after it."""

    def __init__(self, buffer):
        self._buffer = buffer

    @property
    def text(self) -> str:
        return self._buffer.getvalue()


@contextlib.contextmanager
def capture_stderr():
    """Capture the converter's stderr log lines for one block.

    Every stage reports its progress, its deletions and its skips through
    ``_log``, and the operator relies on those messages as much as on the
    bytes, so the tests read them rather than inferring them.
    """
    buffer = io.StringIO()
    with contextlib.redirect_stderr(buffer):
        yield _Captured(buffer)


@contextlib.contextmanager
def quiet_output():
    with contextlib.redirect_stderr(io.StringIO()), contextlib.redirect_stdout(io.StringIO()):
        yield


def delete_through_the_guard(manifest, path):
    """Delete ``path`` through the manifest's containment guard and save the
    record, as a stage that consumes an intermediate does."""
    manifest.remove_intermediate(path)
    manifest.save()


def assert_datasets_equal(test, expected_dir, actual_dir):
    """Every /halos column (values and dtype), header attribute, /schema
    attribute and sidecar value of two version 3 datasets."""
    test.assertEqual(dataset_files(expected_dir), dataset_files(actual_dir))
    for snap in range(N_FIXTURE_SNAPSHOTS):
        name = "snapshot_{:03d}.h5".format(snap)
        halos_a, header_a, schema_a = read_slab_file(Path(expected_dir) / name)
        halos_b, header_b, schema_b = read_slab_file(Path(actual_dir) / name)
        test.assertEqual(sorted(halos_a), sorted(halos_b), name)
        for column in halos_a:
            test.assertEqual(halos_a[column].dtype, halos_b[column].dtype, (name, column))
            test.assertEqual(halos_a[column].shape, halos_b[column].shape, (name, column))
            test.assertEqual(halos_a[column].tobytes(), halos_b[column].tobytes(), (name, column))
        test.assertEqual(sorted(header_a), sorted(header_b), name)
        for attr in header_a:
            test.assertEqual(header_a[attr], header_b[attr], (name, attr))
            test.assertEqual(np.asarray(header_a[attr]).dtype, np.asarray(header_b[attr]).dtype)
        test.assertEqual(schema_a, schema_b, name)
    sidecar_a = read_sidecar(Path(expected_dir) / "forests.h5")
    sidecar_b = read_sidecar(Path(actual_dir) / "forests.h5")
    test.assertEqual(sorted(sidecar_a), list(V3_SIDECAR_DATASETS))
    test.assertEqual(sorted(sidecar_b), list(V3_SIDECAR_DATASETS))
    for name in V3_SIDECAR_DATASETS:
        test.assertEqual(sidecar_a[name].dtype, sidecar_b[name].dtype)
        np.testing.assert_array_equal(sidecar_a[name], sidecar_b[name], name)
