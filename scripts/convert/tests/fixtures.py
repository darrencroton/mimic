"""Synthetic ctrees ASCII fixture generator for converter tests.

Hand-specified tiny forests with known topology (plan Slice 2): multi-tree
forests, multi-progenitor halos, mass ties, flyby configurations,
zero-central-at-max-scale forests, early-dying forests, zero-mass halos, and
sub-subhalos whose pid differs from their ultimate host. Slices 5-6 reuse
these topologies for fix-up and link tests.

Snapshot numbering follows the a_list convention: SnapNum indexes A_LIST.
"""

import os
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import List, Optional, Sequence

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


def _row_text(halo: HaloSpec, tree: TreeSpec, a_list: Sequence[float]) -> str:
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
    return " ".join(values[c] for c in COLUMNS)


def header_line(dialect: str = "indexed", snapshot_column: str = "Snap_num") -> str:
    names = [snapshot_column if c == "Snap_num" else c for c in COLUMNS]
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
) -> Path:
    """Write a synthetic ctrees file. Real files carry a bare tree-count line
    before the first '#tree' marker, so the generator writes one by default;
    pass ``tree_count`` to write a deliberately wrong value."""
    path = Path(path)
    lines = [
        header_override if header_override is not None else header_line(dialect, snapshot_column)
    ]
    lines.append("#Synthetic converter test fixture")
    if include_tree_count:
        lines.append(str(tree_count if tree_count is not None else len(trees)))
    for tree in trees:
        lines.append("#tree {}".format(tree.root_id))
        for halo in tree.halos:
            lines.append(_row_text(halo, tree, a_list))
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
# Canned forests (topology cases from the plan)
# ---------------------------------------------------------------------------


def multi_tree_forest() -> ForestSpec:
    """Forest 100: two trees; multi-progenitor halo with a mass tie; two
    pid==-1 centrals at the forest max snapshot (flyby-demotion input)."""
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
    """Forest 300: zero pid==-1 centrals at the forest max snapshot (corrupt
    input; the Slice 5 fix_flybys equivalent must abort on it)."""
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


def zero_mass_forest() -> ForestSpec:
    """Forest 500: contains a zero-mass halo (Spin carve-out input, Slice 5)."""
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
    (6010); the Slice 5 fix_upid equivalent must set both to 6010."""
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
    forest is deliberately excluded: it exists to test the Slice 5 abort."""
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
