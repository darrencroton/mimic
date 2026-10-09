"""Synthetic ctrees ASCII fixture generator for converter tests.

Hand-specified tiny forests with known topology: multi-tree
forests, multi-progenitor halos, mass ties, multiple independent FoF groups at
a forest's maximum snapshot, unresolvable-satellite forests, early-dying
forests, zero-mass halos, and sub-subhalos whose pid differs from their
ultimate host. The fix-up and link tests reuse these topologies.

Snapshot numbering follows the a_list convention: SnapNum indexes A_LIST.
"""

import os
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Callable, List, Optional, Sequence, Tuple

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

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


def write_simulation_info(path) -> Path:
    path = Path(path)
    path.write_text(
        "simulation:\n"
        "  cosmology: {omega_matter: 0.3089, omega_lambda: 0.6911, hubble_h: 0.6774}\n"
        "  box_size: {value: 100.0, units: Mpc/h}\n"
        "  particle_mass: {value: 0.0325, units: 1e10 Msun/h}\n"
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
# Correspondence-valid forests whose forest-id order disagrees with halo-id order
# ---------------------------------------------------------------------------
#
# A version 2 slab is sorted by halo id while the version 3 row order is by
# (ForestIndex, HaloRankInForest), ForestIndex being the rank of the forest id.
# In the forests above the two orders agree, so a row permutation between them
# moves only neighbours. Here forest 3000 holds halo ids 3000-3011 and 7100-7102,
# forest 6000 holds 1500-1502 and 5500-5502, and forest 8000 holds 2000-2002, so
# in every slab the version 2 order interleaves the three forests
# (1500, 2000, 3000, ..., 5500, 7100 at the final snapshot) and every link kind
# -- Descendant, FirstProgenitor, NextProgenitor and both FoF links -- crosses a
# permutation that moves whole forest runs past one another.


def interleaved_forest_3000() -> ForestSpec:
    """Forest 3000: trees 3000 and 3010 (3010 is 3000's satellite at snapshot
    5, 3011 is 3001's at 4) and tree 7100, with ids above every other forest's."""
    tree_a = TreeSpec(
        root_id=3000,
        halos=[
            HaloSpec(halo_id=3000, snap=5, mvir=2.5e12, num_prog=2),
            HaloSpec(halo_id=3001, snap=4, mvir=1.5e12, desc_id=3000, num_prog=1),
            HaloSpec(halo_id=3002, snap=4, mvir=4.0e11, desc_id=3000),
            HaloSpec(halo_id=3003, snap=3, mvir=1.0e12, desc_id=3001),
        ],
    )
    tree_b = TreeSpec(
        root_id=3010,
        halos=[
            HaloSpec(halo_id=3010, snap=5, mvir=3.0e11, pid=3000, upid=3000, num_prog=1),
            HaloSpec(halo_id=3011, snap=4, mvir=3.2e11, desc_id=3010, pid=3001, upid=3001),
        ],
    )
    tree_c = TreeSpec(
        root_id=7100,
        halos=[
            HaloSpec(halo_id=7100, snap=5, mvir=9.0e11, num_prog=2),
            HaloSpec(halo_id=7101, snap=4, mvir=5.0e11, desc_id=7100),
            HaloSpec(halo_id=7102, snap=4, mvir=6.0e11, desc_id=7100),
        ],
    )
    return ForestSpec(forest_id=3000, trees=[tree_a, tree_b, tree_c])


def interleaved_forest_6000() -> ForestSpec:
    """Forest 6000: tree 1500 (ids below every other forest's) and tree 5500,
    whose root is 1500's satellite at snapshot 5 with two progenitors."""
    tree_a = TreeSpec(
        root_id=1500,
        halos=[
            HaloSpec(halo_id=1500, snap=5, mvir=1.8e12, num_prog=1),
            HaloSpec(halo_id=1501, snap=4, mvir=1.2e12, desc_id=1500, num_prog=1),
            HaloSpec(halo_id=1502, snap=3, mvir=8.0e11, desc_id=1501),
        ],
    )
    tree_b = TreeSpec(
        root_id=5500,
        halos=[
            HaloSpec(halo_id=5500, snap=5, mvir=4.0e11, pid=1500, upid=1500, num_prog=2),
            HaloSpec(halo_id=5501, snap=4, mvir=2.0e11, desc_id=5500, pid=1501, upid=1501),
            HaloSpec(halo_id=5502, snap=4, mvir=2.5e11, desc_id=5500),
        ],
    )
    return ForestSpec(forest_id=6000, trees=[tree_a, tree_b])


def interleaved_forest_8000() -> ForestSpec:
    """Forest 8000: one tree whose ids (2000-2002) fall between forest 6000's
    and forest 3000's, with two progenitors at snapshot 4."""
    tree = TreeSpec(
        root_id=2000,
        halos=[
            HaloSpec(halo_id=2000, snap=5, mvir=7.0e11, num_prog=2),
            HaloSpec(halo_id=2001, snap=4, mvir=3.0e11, desc_id=2000),
            HaloSpec(halo_id=2002, snap=4, mvir=3.5e11, desc_id=2000),
        ],
    )
    return ForestSpec(forest_id=8000, trees=[tree])


def interleaved_correspondence_forests() -> List[ForestSpec]:
    """The interleaved forests in ascending forest id: every tree reaches the
    final snapshot and every ``#tree`` root is its terminal halo."""
    return [interleaved_forest_3000(), interleaved_forest_6000(), interleaved_forest_8000()]


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
