"""Tests for ctrees_to_lhalo: sage-model's Consistent-Trees to L-Halo conversion (9218e11).

Every expected record is a literal derived by hand from sage-model's code at 9218e11
(``src/io/ctrees_utils.c``, ``src/io/read_tree_consistentrees_ascii.c``,
``src/io/parse_ctrees.h``), never computed by the module under test. The derivations are
in the comments beside each control. The float literals were also checked against
sage-model's own arithmetic compiled from that commit, and every forest control's
records against sage-model's ``load_forest_ctrees`` compiled from that commit with only
the module's two differences patched in (development evidence kept outside the
repository).

The forest controls live in ``FOREST_CONTROLS`` (name -> trees, particle mass, expected
records in output order), so the same literals can be fed to that compiled oracle.

Record tuples ``R(...)`` list, in order: MostBoundID, SnapNum, Descendant,
FirstProgenitor, NextProgenitor, FirstHaloInFOFgroup, NextHaloInFOFgroup, Len, Mvir
(1e10 Msun/h), SubhaloIndex. Unless a control says otherwise, a halo's other fields are
the defaults of ``halo()``, which convert exactly: Pos (1.5, 2.5, 3.5), Vel (-1.25, 0.5,
4.0), VelDisp 100.5, Vmax 200.25, Spin 0 (J = 0), M_Mean200 = M_TopHat = Mvir (M200b =
M200c = mvir), FileNr -1, SubHalfMass -1.

Raw masses: strtof("1e10"), ("2e10"), ("4e10"), ("8e10") are exact (2**k * 9765625 *
2**10, 9765625 < 2**24), and times the double 1e-10 they round to 1.0, 2.0, 4.0, 8.0
in double and float32. strtof("3e10") is the tie 14648437.5 * 2048, rounded to even
30000001024; times 1e-10 that is 3.0000001024, within half a float32 ulp (1.19e-7) of
3.0, so 3.0. With PartMass 1.0, Len = roundf(Mvir) = Mvir for these.
"""

import contextlib
import hashlib
import io
import json
import os
import subprocess
import sys
import tempfile
import unittest
import warnings
from collections import namedtuple
from pathlib import Path
from unittest import mock

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import ctrees_to_lhalo as c2l  # noqa: E402

# A header in a non-catalogue order, with mixed case ("Mvir", "Snap_num") and a column
# the conversion does not request (num_prog): columns are found by name, ignoring case.
HEADER = (
    b"#scale(0) id(1) desc_scale(2) desc_id(3) num_prog(4) pid(5) upid(6) Mvir(7) vrms(8)"
    b" vmax(9) x(10) y(11) z(12) vx(13) vy(14) vz(15) Jx(16) Jy(17) Jz(18) Snap_num(19)"
    b" M200b(20) M200c(21)\n"
)
CMAP = c2l.column_map_from_header(HEADER, "<test header>")

R = namedtuple("R", "mbid snap desc fprog nprog ffof nfof length mvir sub")


def halo(
    hid,
    scale,
    snap,
    mvir="1e10",
    pid=-1,
    upid=-1,
    desc_id=-1,
    desc_scale="0.00000",
    J=("0", "0", "0"),
    m200b=None,
    m200c=None,
    x="1.5",
):
    """One ASCII row under ``HEADER``."""
    fields = [
        scale,
        str(hid),
        desc_scale,
        str(desc_id),
        "0",
        str(pid),
        str(upid),
        mvir,
        "100.5",
        "200.25",
        x,
        "2.5",
        "3.5",
        "-1.25",
        "0.5",
        "4",
        *J,
        str(snap),
        m200b or mvir,
        m200c or mvir,
    ]
    return (" " + " ".join(fields) + "\n").encode()


def forest_rows(trees):
    """Rows and per-tree counts of a forest given as lists of ``halo()`` rows."""
    text = b"".join(b"".join(tree) for tree in trees)
    return c2l.parse_rows(text, CMAP, "<test forest>"), [len(tree) for tree in trees]


def convert(trees, part_mass=1.0):
    rows, tree_nhalos = forest_rows(trees)
    return c2l.convert_forest(rows, tree_nhalos, part_mass, source="<test forest>")


def expected_records(entries, **overrides):
    """A RECORD_DTYPE array from literal ``R`` tuples and the ``halo()`` defaults.

    ``overrides`` maps a field to a list of per-halo values (or a list of
    ``uint32`` bit patterns, for ``*_bits`` keys).
    """
    out = np.zeros(len(entries), dtype=c2l.RECORD_DTYPE)
    for i, e in enumerate(entries):
        out["MostBoundID"][i] = e.mbid
        out["SnapNum"][i] = e.snap
        out["Descendant"][i] = e.desc
        out["FirstProgenitor"][i] = e.fprog
        out["NextProgenitor"][i] = e.nprog
        out["FirstHaloInFOFgroup"][i] = e.ffof
        out["NextHaloInFOFgroup"][i] = e.nfof
        out["Len"][i] = e.length
        out["Mvir"][i] = e.mvir
        out["M_Mean200"][i] = e.mvir
        out["M_TopHat"][i] = e.mvir
        out["SubhaloIndex"][i] = e.sub
        out["Pos"][i] = (1.5, 2.5, 3.5)
        out["Vel"][i] = (-1.25, 0.5, 4.0)
        out["VelDisp"][i] = 100.5
        out["Vmax"][i] = 200.25
        out["Spin"][i] = (0.0, 0.0, 0.0)
        out["FileNr"][i] = -1
        out["SubHalfMass"][i] = -1.0
    for field, values in overrides.items():
        if field.endswith("_bits"):
            name = field[: -len("_bits")]
            out[name] = np.array(values, dtype=np.uint32).view(np.float32).reshape(out[name].shape)
        else:
            out[field] = values
    return out


# --------------------------------------------------------------------------------------
# The forest controls
# --------------------------------------------------------------------------------------
# Output order is fix_upid's then assign_mergertree_indices' sort: scale descending, then
# post-fix upid (a FoF's own id; a subhalo's FoF id), pid (-1 for a FoF, so the FoF comes
# first), id. FoF links: a FoF is its own FirstHaloInFOFgroup, its members follow it in
# that order. Descendant: index of desc_id at the highest snapshot within 1e-4 of
# desc_scale. Progenitors: met in index order; a later one goes to the front only if
# strictly more massive than the current front, else to the back.
# SubhaloIndex: rows of the forest before the tree plus the tree's own rows.

FOREST_CONTROLS = {}

# One ASCII tree, file order A, C, B, PartMass 0.5 (1/PartMass = 2.0, so Len = 2 Mvir).
# Order: scale 1.0: A (FoF 100), C (sub of 100); scale 0.5: B (FoF 90).
# A: Spin = J / 4e10: 2e10 -> 0.5, -1e10 -> -0.25 (exact after float32 narrowing).
FOREST_CONTROLS["single_tree"] = (
    [
        [
            halo(100, "1.00000", 2, mvir="4e10", J=("2e10", "-1e10", "0")),
            halo(101, "1.00000", 2, mvir="1e10", pid=100, upid=100),
            halo(90, "0.50000", 1, mvir="2e10", desc_id=100, desc_scale="1.00000"),
        ]
    ],
    0.5,
    expected_records(
        [
            R(100, 2, -1, 2, -1, 0, 1, 8, 4.0, 3),
            R(101, 2, -1, -1, -1, 0, -1, 2, 1.0, 3),
            R(90, 1, 0, -1, -1, 2, -1, 4, 2.0, 3),
        ],
        Spin=[(0.5, -0.25, 0.0), (0.0, 0.0, 0.0), (0.0, 0.0, 0.0)],
    ),
)

# Two ASCII trees. Order: scale 1.0: 200 (FoF), 300 (sub of 200); scale 0.5: 150, 250.
# SubhaloIndex: tree 1 -> 0 + 2 = 2, tree 2 -> 2 + 2 = 4. MostBoundID: own ids (sage-model
# gave 300 and 250, the later tree's rows 0 and 1, the forest's rows 0 and 1: 200, 150).
FOREST_CONTROLS["multi_tree"] = (
    [
        [
            halo(200, "1.00000", 1, mvir="4e10"),
            halo(150, "0.50000", 0, mvir="2e10", desc_id=200, desc_scale="1.00000"),
        ],
        [
            halo(300, "1.00000", 1, mvir="1e10", pid=200, upid=200),
            halo(250, "0.50000", 0, mvir="1e10", desc_id=300, desc_scale="1.00000"),
        ],
    ],
    1.0,
    expected_records(
        [
            R(200, 1, -1, 2, -1, 0, 1, 4, 4.0, 2),
            R(300, 1, -1, 3, -1, 0, -1, 1, 1.0, 4),
            R(150, 0, 0, -1, -1, 2, -1, 2, 2.0, 2),
            R(250, 0, 1, -1, -1, 3, -1, 1, 1.0, 4),
        ]
    ),
)

# Three FoF centrals at the last snapshot (masses 2, 4, 1). sage-model demoted 400 and 600
# into 500's group with MostBoundID -400, -600; here each stays a central of its own group.
# Order: scale 1.0: 400, 500, 600 (upid = own id); scale 0.5: 350, 450.
FOREST_CONTROLS["several_fofs"] = (
    [
        [
            halo(400, "1.00000", 1, mvir="2e10"),
            halo(350, "0.50000", 0, mvir="1e10", desc_id=400, desc_scale="1.00000"),
        ],
        [
            halo(500, "1.00000", 1, mvir="4e10"),
            halo(450, "0.50000", 0, mvir="2e10", desc_id=500, desc_scale="1.00000"),
        ],
        [halo(600, "1.00000", 1, mvir="1e10")],
    ],
    1.0,
    expected_records(
        [
            R(400, 1, -1, 3, -1, 0, -1, 2, 2.0, 2),
            R(500, 1, -1, 4, -1, 1, -1, 4, 4.0, 4),
            R(600, 1, -1, -1, -1, 2, -1, 1, 1.0, 5),
            R(350, 0, 0, -1, -1, 3, -1, 1, 1.0, 2),
            R(450, 0, 1, -1, -1, 4, -1, 2, 2.0, 4),
        ]
    ),
)

# A second FoF group (800, with subhalo 810) beside the survivor 700, one snapshot.
# sage-model set 810's upid (and 800's) to 700, merging both into 700's group; here 810
# keeps upid 800. Order: 700 (upid 700), 800 (upid 800, pid -1), 810 (upid 800, pid 800).
FOREST_CONTROLS["second_fof_subhalo"] = (
    [
        [halo(700, "1.00000", 0, mvir="4e10")],
        [
            halo(800, "1.00000", 0, mvir="2e10"),
            halo(810, "1.00000", 0, mvir="1e10", pid=800, upid=800),
        ],
    ],
    1.0,
    expected_records(
        [
            R(700, 0, -1, -1, -1, 0, -1, 4, 4.0, 1),
            R(800, 0, -1, -1, -1, 1, 2, 2, 2.0, 3),
            R(810, 0, -1, -1, -1, 1, -1, 1, 1.0, 3),
        ]
    ),
)

# A host chain 1030 -> 1020 -> 1010 -> 1000 (FoF) in upid, rows in reverse. fix_upid gives
# every subhalo upid = pid = 1000. Order: 1000, then 1010, 1020, 1030 by id.
FOREST_CONTROLS["host_chain"] = (
    [
        [
            halo(1030, "1.00000", 0, mvir="1e10", pid=1020, upid=1020),
            halo(1020, "1.00000", 0, mvir="2e10", pid=1010, upid=1010),
            halo(1010, "1.00000", 0, mvir="4e10", pid=1000, upid=1000),
            halo(1000, "1.00000", 0, mvir="8e10"),
        ]
    ],
    1.0,
    expected_records(
        [
            R(1000, 0, -1, -1, -1, 0, 1, 8, 8.0, 4),
            R(1010, 0, -1, -1, -1, 0, 2, 4, 4.0, 4),
            R(1020, 0, -1, -1, -1, 0, 3, 2, 2.0, 4),
            R(1030, 0, -1, -1, -1, 0, -1, 1, 1.0, 4),
        ]
    ),
)

# Two progenitors of equal mass 2: met in index order 1900, 1901; 1901 is not strictly
# more massive than the front 1900, so it is appended: chain 1900 -> 1901.
FOREST_CONTROLS["tied_progenitors"] = (
    [
        [
            halo(2000, "1.00000", 1, mvir="8e10"),
            halo(1901, "0.50000", 0, mvir="2e10", desc_id=2000, desc_scale="1.00000"),
            halo(1900, "0.50000", 0, mvir="2e10", desc_id=2000, desc_scale="1.00000"),
        ]
    ],
    1.0,
    expected_records(
        [
            R(2000, 1, -1, 1, -1, 0, -1, 8, 8.0, 3),
            R(1900, 0, 0, -1, 2, 1, -1, 2, 2.0, 3),
            R(1901, 0, 0, -1, -1, 2, -1, 2, 2.0, 3),
        ]
    ),
)

# Progenitors met in mass order 1, 3, 2 (ids 1910, 1911, 1912, rows in reverse) give the
# chain 3, 1, 2: 1911 (3 > 1) goes to the front, 1912 (2 < 3) to the back. Masses 1, 2, 4
# (ids 1920, 1921, 1922) each go to the front: chain 4, 2, 1. Two FoFs at the last
# snapshot (2100, 2200). Order: 2100, 2200, then 1910, 1911, 1912, 1920, 1921, 1922.
FOREST_CONTROLS["non_monotone_progenitors"] = (
    [
        [
            halo(2100, "1.00000", 1, mvir="8e10"),
            halo(1912, "0.50000", 0, mvir="2e10", desc_id=2100, desc_scale="1.00000"),
            halo(1911, "0.50000", 0, mvir="3e10", desc_id=2100, desc_scale="1.00000"),
            halo(1910, "0.50000", 0, mvir="1e10", desc_id=2100, desc_scale="1.00000"),
        ],
        [
            halo(2200, "1.00000", 1, mvir="8e10"),
            halo(1920, "0.50000", 0, mvir="1e10", desc_id=2200, desc_scale="1.00000"),
            halo(1921, "0.50000", 0, mvir="2e10", desc_id=2200, desc_scale="1.00000"),
            halo(1922, "0.50000", 0, mvir="4e10", desc_id=2200, desc_scale="1.00000"),
        ],
    ],
    1.0,
    expected_records(
        [
            R(2100, 1, -1, 3, -1, 0, -1, 8, 8.0, 4),
            R(2200, 1, -1, 7, -1, 1, -1, 8, 8.0, 8),
            R(1910, 0, 0, -1, 4, 2, -1, 1, 1.0, 4),
            R(1911, 0, 0, -1, 2, 3, -1, 3, 3.0, 4),
            R(1912, 0, 0, -1, -1, 4, -1, 2, 2.0, 4),
            R(1920, 0, 1, -1, -1, 5, -1, 1, 1.0, 8),
            R(1921, 0, 1, -1, 5, 6, -1, 2, 2.0, 8),
            R(1922, 0, 1, -1, 6, 7, -1, 4, 4.0, 8),
        ]
    ),
)

# Snapshot scales 0.40000 (0), 0.50000 (1), 0.50005 (2). desc_scale 0.50003 lies within
# 1e-4 of snapshots 1 (3e-5) and 2 (2e-5); the search from the top takes snapshot 2,
# where 3000 is. Order: 3000, 2900, 2800.
FOREST_CONTROLS["descendant_two_snapshots"] = (
    [
        [
            halo(3000, "0.50005", 2, mvir="4e10"),
            halo(2900, "0.50000", 1, mvir="2e10"),
            halo(2800, "0.40000", 0, mvir="1e10", desc_id=3000, desc_scale="0.50003"),
        ]
    ],
    1.0,
    expected_records(
        [
            R(3000, 2, -1, 2, -1, 0, -1, 4, 4.0, 3),
            R(2900, 1, -1, -1, -1, 1, -1, 2, 2.0, 3),
            R(2800, 0, 0, -1, -1, 2, -1, 1, 1.0, 3),
        ]
    ),
)

# sage-model's arithmetic on catalogue-like values, PartMass 0.0327 (the bits checked
# against sage-model's convert_ctrees_conventions_to_lht compiled at 9218e11):
# Mvir = float32(strtof("1.77500e+14") * 1e-10) = 17750.0; Len = roundf(float32(17750.0 /
# 0.0327 = 542813.45...)) = 542813; Spin[k] = float32(J * (1.0 / 1.775e14)).
FOREST_CONTROLS["arithmetic"] = (
    [
        [
            halo(
                5000,
                "1.00000",
                0,
                mvir="1.77500e+14",
                J=("1.696e+15", "-1.072e+14", "-1.364e+15"),
                m200b="191950000000000",
                m200c="156190000000000",
            )
        ]
    ],
    0.0327,
    expected_records(
        [R(5000, 0, -1, -1, -1, 0, -1, 542813, 17750.0, 1)],
        Mvir_bits=[0x468AAC00],
        M_Mean200_bits=[0x4695F600],
        M_TopHat_bits=[0x46740BFF],
        Spin_bits=[0x4118E0FF, 0xBF1A9C0E, 0xC0F5E77C],
    ),
)

# Len = (int) roundf(float32(Mvir * (1.0 / PartMass))), Mvir = 2.5f (strtof("2.5e10") =
# 24999999488, times 1e-10 = 2.4999999488, within 1.19e-7 of 2.5).
# PartMass 1.0: the product is exactly 2.5, rounded half away from zero: 3 (not 2).
# PartMass 1.000000004: the double product 2.49999999 narrows to exactly 2.5: 3 (rounding
# the double directly would give 2). PartMass 1.0000001: the product 2.49999975 narrows
# to 2.4999998 (the float32 below 2.5): 2.
for _name, _part_mass, _length in (
    ("len_half_way", 1.0, 3),
    ("len_narrows_to_half", 1.000000004, 3),
    ("len_narrows_below_half", 1.0000001, 2),
):
    FOREST_CONTROLS[_name] = (
        [[halo(6000, "1.00000", 0, mvir="2.5e10")]],
        _part_mass,
        expected_records([R(6000, 0, -1, -1, -1, 0, -1, _length, 2.5, 1)]),
    )


def assert_records_equal(test, got, expected):
    """Field by field and bit for bit, then the whole byte image."""
    test.assertEqual(got.dtype, c2l.RECORD_DTYPE)
    test.assertEqual(len(got), len(expected))
    for field in c2l.RECORD_DTYPE.names:
        a = np.ascontiguousarray(got[field])
        b = np.ascontiguousarray(expected[field])
        test.assertEqual(a.tobytes(), b.tobytes(), f"field {field}: {a!r} != {b!r}")
    test.assertEqual(got.tobytes(), expected.tobytes())


class TestForestControls(unittest.TestCase):
    """Each control's records equal its hand-derived literals, bit for bit."""

    def check(self, name):
        trees, part_mass, expected = FOREST_CONTROLS[name]
        assert_records_equal(self, convert(trees, part_mass), expected)

    def test_single_tree(self):
        self.check("single_tree")

    def test_multi_tree_own_ids_and_per_tree_subhalo_index(self):
        self.check("multi_tree")
        got = convert(*FOREST_CONTROLS["multi_tree"][:2])
        self.assertEqual(got["MostBoundID"].tolist(), [200, 300, 150, 250])
        self.assertEqual(got["SubhaloIndex"].tolist(), [2, 4, 2, 4])

    def test_several_fofs_at_last_snapshot_are_not_demoted(self):
        self.check("several_fofs")
        got = convert(*FOREST_CONTROLS["several_fofs"][:2])
        last = got["SnapNum"] == 1
        self.assertEqual(got["MostBoundID"][last].tolist(), [400, 500, 600])
        self.assertEqual(got["FirstHaloInFOFgroup"][last].tolist(), [0, 1, 2])

    def test_second_fof_subhalo_keeps_its_upid(self):
        self.check("second_fof_subhalo")
        rows, _ = forest_rows(FOREST_CONTROLS["second_fof_subhalo"][0])
        upid, pid, _ = c2l._fix_upid(rows, rows["snap_num"], "<test>")
        pairs = zip(upid.tolist(), pid.tolist(), strict=True)
        by_id = dict(zip(rows["id"].tolist(), pairs, strict=True))
        self.assertEqual(by_id[810], (800, 800))
        self.assertEqual(by_id[800], (800, -1))

    def test_host_chain_needing_fix_upid(self):
        self.check("host_chain")

    def test_tied_progenitor_masses(self):
        self.check("tied_progenitors")

    def test_non_monotone_progenitor_masses(self):
        self.check("non_monotone_progenitors")
        got = convert(*FOREST_CONTROLS["non_monotone_progenitors"][:2])
        chain, link = [], int(got["FirstProgenitor"][0])
        while link != -1:
            chain.append(float(got["Mvir"][link]))
            link = int(got["NextProgenitor"][link])
        self.assertEqual(chain, [3.0, 1.0, 2.0])

    def test_descendant_takes_highest_snapshot_within_tolerance(self):
        self.check("descendant_two_snapshots")

    def test_arithmetic_of_spin_mass_and_len(self):
        self.check("arithmetic")

    def test_len_half_way_rounds_away_from_zero(self):
        self.check("len_half_way")

    def test_len_product_narrowing_to_n_plus_half(self):
        self.check("len_narrows_to_half")

    def test_len_product_narrowing_below_half(self):
        self.check("len_narrows_below_half")

    def test_spin_of_zero_mass(self):
        # 1.0 / 0.0 = inf: J * inf = +-inf, and 0 * inf = NaN (the platform's default NaN,
        # as sage-model's C on the same hardware). Len = roundf(0) = 0.
        got = convert([[halo(7000, "1.00000", 0, mvir="0", J=("1e10", "-1e10", "0"))]])
        self.assertEqual(got["Spin"][0, 0], np.float32(np.inf))
        self.assertEqual(got["Spin"][0, 1], np.float32(-np.inf))
        self.assertTrue(np.isnan(got["Spin"][0, 2]))
        self.assertEqual(int(got["Len"][0]), 0)


class TestForestRefusals(unittest.TestCase):
    """sage-model's error exits become CtreesToLhaloError, exit status 2."""

    def assertRefused(self, trees, *fragments):
        with self.assertRaises(c2l.CtreesToLhaloError) as caught:
            convert(trees)
        self.assertEqual(caught.exception.exit_status, 2)
        self.assertEqual(caught.exception.source, "<test forest>")
        for fragment in fragments:
            self.assertIn(fragment, str(caught.exception))
        return caught.exception

    def test_no_fof_at_last_scale(self):
        # fix_flybys, ctrees_utils.c lines 321-341.
        self.assertRefused(
            [
                [
                    halo(900, "1.00000", 1, pid=950, upid=950),
                    halo(950, "0.50000", 0, mvir="2e10"),
                ]
            ],
            "no FoF halo",
        )

    def test_missing_descendant_id(self):
        # assign_mergertree_indices, ctrees_utils.c lines 600-610.
        self.assertRefused(
            [
                [
                    halo(3000, "1.00000", 1),
                    halo(2800, "0.50000", 0, desc_id=9999, desc_scale="1.00000"),
                ]
            ],
            "descendant id 9999",
        )

    def test_descendant_only_in_the_lower_of_two_matching_snapshots(self):
        # desc_scale 0.50003 matches snapshots 1 and 2; sage-model searches snapshot 2
        # only, so a descendant at snapshot 1 is not found.
        self.assertRefused(
            [
                [
                    halo(3000, "0.50005", 2, mvir="4e10"),
                    halo(2900, "0.50000", 1, mvir="2e10"),
                    halo(2800, "0.40000", 0, desc_id=2900, desc_scale="0.50003"),
                ]
            ],
            "descendant id 2900",
            "snapshot 2",
        )

    def test_descendant_scale_matching_no_snapshot(self):
        self.assertRefused(
            [
                [
                    halo(3000, "1.00000", 1),
                    halo(2800, "0.50000", 0, desc_id=3000, desc_scale="0.90000"),
                ]
            ],
            "could not locate desc_snapnum",
        )

    def test_host_missing(self):
        self.assertRefused(
            [[halo(1000, "1.00000", 0), halo(1010, "1.00000", 0, pid=1, upid=1)]],
            "no halo has id 1",
        )

    def test_host_cycle(self):
        # sage-model would loop forever; refused.
        self.assertRefused(
            [
                [
                    halo(1000, "1.00000", 0),
                    halo(1010, "1.00000", 0, pid=1020, upid=1020),
                    halo(1020, "1.00000", 0, pid=1010, upid=1010),
                ]
            ],
            "never reaches a FOF halo",
        )

    def test_repeated_halo_id(self):
        self.assertRefused(
            [[halo(1000, "1.00000", 1), halo(1000, "0.50000", 0)]], "appears more than once"
        )

    def test_every_snapnum_negative(self):
        # fix_upid returns max_snapnum = -1 (ctrees_utils.c lines 387 and 402-407), which
        # load_forest_ctrees refuses (read_tree_consistentrees_ascii.c lines 407-410).
        self.assertRefused([[halo(1000, "1.00000", -1)]], "every SnapNum is negative")

    def test_empty_forest(self):
        with self.assertRaises(c2l.CtreesToLhaloError) as caught:
            c2l.convert_forest(np.zeros(0, dtype=c2l.ROW_DTYPE), [0], 1.0, "<empty>")
        self.assertEqual(caught.exception.exit_status, 2)

    def test_one_fof_at_last_scale_returns_before_the_survivor_search(self):
        # fix_flybys returns at its single-FoF check (lines 340-342) even when the FoF's
        # mass is NaN, which the survivor search (lines 344-358) would refuse.
        got = convert([[halo(1000, "1.00000", 0, mvir="nan")]])
        self.assertEqual(got["MostBoundID"].tolist(), [1000])


class TestParsing(unittest.TestCase):
    def test_columns_found_by_name_ignoring_case(self):
        expected = (
            (0, "scale", "<f8"),
            (1, "id", "<i8"),
            (2, "desc_scale", "<f8"),
            (3, "desc_id", "<i8"),
            (5, "pid", "<i8"),
            (6, "upid", "<i8"),
            (7, "mvir", "<f4"),
            (8, "vrms", "<f4"),
            (9, "vmax", "<f4"),
            (10, "x", "<f4"),
            (11, "y", "<f4"),
            (12, "z", "<f4"),
            (13, "vx", "<f4"),
            (14, "vy", "<f4"),
            (15, "vz", "<f4"),
            (16, "Jx", "<f4"),
            (17, "Jy", "<f4"),
            (18, "Jz", "<f4"),
            (19, "snap_num", "<i4"),
            (20, "M200b", "<f4"),
            (21, "M200c", "<f4"),
        )
        self.assertEqual(CMAP.columns, expected)

    def test_snap_idx_and_the_later_of_two_snapshot_columns(self):
        idx = HEADER.replace(b"Snap_num(19)", b"SNAP_IDX(19)")
        self.assertIn((19, "snap_num", "<i4"), c2l.column_map_from_header(idx, "h").columns)
        both = HEADER.replace(b"num_prog(4)", b"snap_idx(4)")
        self.assertIn((19, "snap_num", "<i4"), c2l.column_map_from_header(both, "h").columns)
        self.assertNotIn((4, "snap_num", "<i4"), c2l.column_map_from_header(both, "h").columns)

    def test_types(self):
        self.assertEqual(
            [c2l.ROW_DTYPE[f].str for f in ("scale", "desc_scale", "id", "desc_id", "pid")],
            ["<f8", "<f8", "<i8", "<i8", "<i8"],
        )
        self.assertEqual(c2l.ROW_DTYPE["upid"].str, "<i8")
        self.assertEqual(c2l.ROW_DTYPE["snap_num"].str, "<i4")
        row = halo(9007199254740993, "0.123456789012345", 7, desc_scale="0.987654321098765")
        rows = c2l.parse_rows(row, CMAP)
        # float64 (strtod) keeps digits a float32 would lose; int64 (strtoll) is exact.
        self.assertEqual(rows["scale"][0].view(np.uint64), np.uint64(0x3FBF9ADD3746F62E))
        self.assertEqual(rows["desc_scale"][0].view(np.uint64), np.uint64(0x3FEF9ADD3C1BE9B5))
        self.assertEqual(int(rows["id"][0]), 9007199254740993)
        self.assertEqual(int(rows["snap_num"][0]), 7)

    def test_float32_columns_equal_strtof_including_double_rounding(self):
        # Decimals a hair above the midpoint 1 + 2**-24 and below 1 + 3 * 2**-24: strtod
        # lands on the midpoint, whose cast to float32 ties to even (0x3f800000,
        # 0x3f800002); strtof rounds once (0x3f800001 both). 3.27000e+09 is itself a
        # float32 midpoint (25546875 * 128), tie to even 0x4f42e83e both ways.
        above = "1.00000005960464477539062500000000000000001"
        below = "1.00000017881393432617187499999999999"
        rows = c2l.parse_rows(halo(1, "1.0", 0, mvir="3.27000e+09", J=(above, below, "0")), CMAP)
        self.assertEqual(int(rows["Jx"][0].view(np.uint32)), 0x3F800001)
        self.assertEqual(int(rows["Jy"][0].view(np.uint32)), 0x3F800001)
        self.assertEqual(int(rows["mvir"][0].view(np.uint32)), 0x4F42E83E)
        self.assertEqual(int(np.float32(float(above)).view(np.uint32)), 0x3F800000)
        self.assertEqual(int(np.float32(float(below)).view(np.uint32)), 0x3F800002)
        self.assertEqual(int(c2l.strtof_exact(above.encode()).view(np.uint32)), 0x3F800001)

    def test_float32_overflow_emits_no_warning(self):
        # strtof gives HUGE_VALF (+inf) beyond float32's range; neither the vectorised
        # narrowing nor strtof_exact's neighbour search may emit a RuntimeWarning.
        with warnings.catch_warnings():
            warnings.simplefilter("error")
            rows = c2l.parse_rows(halo(1, "1.0", 0, J=("1e39", "-1e39", "0")), CMAP)
            self.assertEqual(int(rows["Jx"][0].view(np.uint32)), 0x7F800000)
            self.assertEqual(int(rows["Jy"][0].view(np.uint32)), 0xFF800000)
            self.assertEqual(int(c2l.strtof_exact(b"1e39").view(np.uint32)), 0x7F800000)
            flt_max = c2l.strtof_exact(b"3.4028234663852886e38")
            self.assertEqual(int(flt_max.view(np.uint32)), 0x7F7FFFFF)

    def test_strtof_exact_overflow_threshold(self):
        # float32 overflows from 2**128 - 2**103 on (ties to the even 2**128): below it,
        # FLT_MAX 0x7f7fffff.
        threshold = str(2**128 - 2**103)
        self.assertEqual(int(c2l.strtof_exact(threshold.encode()).view(np.uint32)), 0x7F800000)
        below = str(2**128 - 2**103 - 1)
        self.assertEqual(int(c2l.strtof_exact(below.encode()).view(np.uint32)), 0x7F7FFFFF)

    def test_row_refusals(self):
        good = halo(1, "1.0", 0)
        cases = {
            "blank": good + b"\n",
            "tab": good.replace(b" 1.0 ", b"\t1.0 ", 1),
            "comma": good.replace(b" 1 ", b",1 ", 1),
            "short": b" 1.0 1\n",
            "non-numeric": good.replace(b" 1 ", b" one ", 1),
            "over-long": good[:-1] + b" " * 1300 + b"\n",
            "unterminated": good[:-1],
        }
        for name, text in cases.items():
            with self.subTest(name):
                with self.assertRaises(c2l.CtreesToLhaloError) as caught:
                    c2l.parse_rows(text, CMAP, "<rows>")
                self.assertEqual(caught.exception.exit_status, 2)
                self.assertEqual(caught.exception.source, "<rows>")

    def test_header_refusals(self):
        cases = {
            "no hash": HEADER[1:],
            "double space": HEADER.replace(b" id(1)", b"  id(1)"),
            "long token": HEADER.replace(b"num_prog(4)", b"n" * 64),
            "missing column": HEADER.replace(b"M200c", b"M500c"),
            "empty": b"",
        }
        for name, line in cases.items():
            with self.subTest(name):
                with self.assertRaises(c2l.CtreesToLhaloError) as caught:
                    c2l.column_map_from_header(line, "tree_9.dat")
                self.assertEqual(caught.exception.exit_status, 2)
                self.assertIn("tree_9.dat", str(caught.exception))


class TestTreeText(unittest.TestCase):
    BUFFER = b"#h\n#tree 1\n 1 2\n 3 4\n#tree 2\n 5 6\n 7"

    def test_rows_from_offset_to_next_hash_line(self):
        self.assertEqual(c2l.tree_text(self.BUFFER, 11), b" 1 2\n 3 4\n")

    def test_last_tree_drops_an_unterminated_line(self):
        self.assertEqual(c2l.tree_text(self.BUFFER, 29), b" 5 6\n")

    def test_offset_at_a_hash_line_or_end_gives_no_rows(self):
        self.assertEqual(c2l.tree_text(self.BUFFER, 21), b"")
        self.assertEqual(c2l.tree_text(self.BUFFER, len(self.BUFFER)), b"")


class TestTables(unittest.TestCase):
    """forests.list / locations.dat: pairing, file resolution, and every refusal."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        root = Path(self.tmp.name)
        self.trees = root / "trees"
        self.lists = root / "lists"
        self.trees.mkdir()
        self.lists.mkdir()
        tree_a = [halo(11, "1.00000", 1, mvir="2e10"), halo(5, "0.50000", 0)]
        tree_b = [halo(12, "1.00000", 1)]
        tree_c = [halo(13, "1.00000", 1, mvir="4e10", pid=11, upid=11)]
        content = HEADER
        self.offsets = {}
        for root_id, rows in ((11, tree_a), (12, tree_b), (13, tree_c)):
            content += b"#tree %d\n" % root_id
            self.offsets[root_id] = len(content)
            content += b"".join(rows)
        (self.trees / "tree_0_0_0.dat").write_bytes(content)
        self.forests_list = self.lists / "forests.list"
        self.locations = self.trees / "locations.dat"
        self.write_forests([(13, 7), (11, 7), (12, 5)])
        self.write_locations([(12, 0), (11, 0), (13, 0)])

    def tearDown(self):
        self.tmp.cleanup()

    def write_forests(self, pairs):
        body = "".join(f"{t} {f}\n" for t, f in pairs)
        self.forests_list.write_text("#TreeRootID ForestID\n" + body)

    def write_locations(self, entries, filename="tree_0_0_0.dat", offsets=None):
        offsets = offsets or {}
        body = "".join(
            f"{t} {fid} {offsets.get(t, self.offsets.get(t, 0))} {filename}\n" for t, fid in entries
        )
        self.locations.write_text("#TreeRootID FileID Offset Filename\n" + body)

    def read(self):
        return c2l.read_forest_table(self.forests_list, self.locations)

    def test_pairing_and_file_resolution(self):
        table = self.read()
        # (ForestID, FileID, Offset): forest 5 = [12]; forest 7 = [11, 13] by offset.
        self.assertEqual(table.tree_root_id.tolist(), [12, 11, 13])
        self.assertEqual(table.forest_id.tolist(), [5, 7, 7])
        self.assertEqual(table.forest_start.tolist(), [0, 1, 3])
        self.assertEqual(
            table.offset.tolist(), [self.offsets[12], self.offsets[11], self.offsets[13]]
        )
        # tree files resolve against the directory of locations.dat, not forests.list
        self.assertEqual(table.tree_files, (f"{self.trees}/tree_0_0_0.dat",))
        cmap = c2l.read_column_map(table.tree_files[0])
        with c2l.TreeFiles(table.tree_files) as files:
            rows, tree_nhalos = c2l.read_forest(table, 1, files, cmap)
            both, all_trees, per_forest = c2l.read_forests(table, [1, 0], files, cmap)
        self.assertEqual(rows["id"].tolist(), [11, 5, 13])
        self.assertEqual(tree_nhalos.tolist(), [2, 1])
        self.assertEqual(both["id"].tolist(), [11, 5, 13, 12])
        self.assertEqual(all_trees.tolist(), [2, 1, 1])
        self.assertEqual(per_forest.tolist(), [3, 1])
        # The two trees of forest 7 become one L-Halo tree: 13 is a subhalo of 11.
        records = c2l.convert_forest(rows, tree_nhalos, 1.0)
        self.assertEqual(records["MostBoundID"].tolist(), [11, 13, 5])
        self.assertEqual(records["FirstHaloInFOFgroup"].tolist(), [0, 0, 2])
        self.assertEqual(records["SubhaloIndex"].tolist(), [2, 3, 2])

    def test_forest_across_tree_files_in_file_id_order(self):
        # Eight tree files (a perfect cube). Forest 7 holds tree 21 in file 3 at a small
        # offset and tree 22 in file 1 at a larger one: sage-model orders a forest's
        # trees by (ForestID, FileID, Offset), so 22 (file 1) comes before 21 (file 3),
        # against the offset order. The other files each hold a one-tree forest.
        trees = {
            22: (1, [halo(22, "1.00000", 1, mvir="2e10"), halo(2, "0.50000", 0)]),
            21: (3, [halo(21, "1.00000", 1, pid=22, upid=22)]),
        }
        for fid in (0, 2, 4, 5, 6, 7):
            trees[30 + fid] = (fid, [halo(30 + fid, "1.00000", 1)])
        offsets = {}
        for fid in range(8):
            content = HEADER + (b"#padding line\n" * 8 if fid == 1 else b"")
            for root_id, (tree_fid, rows) in trees.items():
                if tree_fid == fid:
                    content += b"#tree %d\n" % root_id
                    offsets[root_id] = len(content)
                    content += b"".join(rows)
            (self.trees / f"tree_{fid}.dat").write_bytes(content)
        self.assertLess(offsets[21], offsets[22])
        roots = [37, 21, 30, 22, 32, 34, 35, 36]
        self.write_forests([(r, 7 if r in (21, 22) else 100 + r) for r in roots])
        body = "".join(f"{r} {trees[r][0]} {offsets[r]} tree_{trees[r][0]}.dat\n" for r in roots)
        self.locations.write_text("#TreeRootID FileID Offset Filename\n" + body)
        table = self.read()
        self.assertEqual(table.tree_root_id.tolist(), [22, 21, 30, 32, 34, 35, 36, 37])
        self.assertEqual(table.file_id.tolist(), [1, 3, 0, 2, 4, 5, 6, 7])
        self.assertEqual(table.forest_start.tolist(), [0, 2, 3, 4, 5, 6, 7, 8])
        cmap = c2l.read_column_map(table.tree_files[0])
        with c2l.TreeFiles(table.tree_files) as files:
            rows, tree_nhalos = c2l.read_forest(table, 0, files, cmap)
        self.assertEqual(rows["id"].tolist(), [22, 2, 21])
        self.assertEqual(tree_nhalos.tolist(), [2, 1])

    def test_leading_forest_id_minus_one_is_dropped(self):
        # sage-model counts forests from a previous id of -1, so leading trees of
        # ForestID -1 open no forest and are skipped (read_tree_consistentrees_ascii.c
        # lines 68-75 and 105-112).
        self.write_forests([(13, 7), (11, 7), (12, -1)])
        table = self.read()
        self.assertEqual(table.tree_root_id.tolist(), [11, 13])
        self.assertEqual(table.forest_start.tolist(), [0, 2])

    def test_locations_dirname_is_cut_at_the_last_slash(self):
        self.assertEqual(c2l._locations_dirname("/a/b/locations.dat"), "/a/b")
        # with no '/', sage-model keeps the whole name as the directory
        self.assertEqual(c2l._locations_dirname("locations.dat"), "locations.dat")

    def assertRefused(self, fragment, source=None):
        with self.assertRaises(c2l.CtreesToLhaloError) as caught:
            self.read()
        self.assertEqual(caught.exception.exit_status, 2)
        self.assertIn(fragment, str(caught.exception))
        if source is not None:
            self.assertEqual(caught.exception.source, str(source))
        return caught.exception

    def test_disagreeing_tree_ids(self):
        self.write_locations([(12, 0), (11, 0), (14, 0)])
        self.assertRefused("does not equal", self.locations)

    def test_tree_counts_differ(self):
        self.write_locations([(12, 0), (11, 0)])
        self.assertRefused("forests.list holds 3", self.locations)

    def test_negative_offset(self):
        self.write_locations([(12, 0), (11, 0), (13, 0)], offsets={13: -5})
        self.assertRefused("offset -5 is negative", self.locations)

    def test_negative_file_id(self):
        self.write_locations([(12, 0), (11, -1), (13, 0)])
        self.assertRefused("file id -1 is negative", self.locations)

    def test_missing_tree_file(self):
        self.write_locations([(12, 0), (11, 0), (13, 0)], filename="tree_9_9_9.dat")
        self.assertRefused("could not open the tree file", f"{self.trees}/tree_9_9_9.dat")

    def test_file_ids_not_contiguous(self):
        self.write_locations([(12, 1), (11, 1), (13, 1)])
        self.assertRefused("not contiguous", self.locations)

    def test_file_count_not_a_cube(self):
        (self.trees / "tree_0_0_1.dat").write_bytes(HEADER)
        body = (
            f"12 0 {self.offsets[12]} tree_0_0_0.dat\n11 0 {self.offsets[11]} tree_0_0_0.dat\n"
            f"13 1 0 tree_0_0_1.dat\n"
        )
        self.locations.write_text("#TreeRootID FileID Offset Filename\n" + body)
        self.assertRefused("not a perfect cube", self.locations)

    def test_file_id_equal_to_the_file_table_size(self):
        # sage-model's table holds 2000 file ids and grows only when a file id exceeds
        # 2000, so file id 2000 is refused (ctrees_utils.c lines 130-151).
        self.write_locations([(12, 2000), (11, 0), (13, 0)])
        self.assertRefused("beyond sage-model's file table", self.locations)

    def test_missing_tables(self):
        self.forests_list.unlink()
        self.assertRefused("can't open the file", self.forests_list)

    def test_missing_locations(self):
        self.locations.unlink()
        self.assertRefused("can't open the file", self.locations)

    def test_blank_line_in_forests_list(self):
        self.forests_list.write_text("#TreeRootID ForestID\n13 7\n\n11 7\n12 5\n")
        self.assertRefused("blank", self.forests_list)

    def test_unparsable_line(self):
        self.forests_list.write_text("#TreeRootID ForestID\n13 7\n11\n12 5\n")
        self.assertRefused("could not parse", self.forests_list)

    def test_blank_line_in_locations(self):
        body = self.locations.read_text().replace("\n12 0", "\n\n12 0", 1)
        self.locations.write_text(body)
        self.assertRefused("blank", self.locations)

    def test_unparsable_line_in_locations(self):
        body = self.locations.read_text().replace(" tree_0_0_0.dat\n", "\n", 1)
        self.locations.write_text(body)
        self.assertRefused("could not parse", self.locations)

    def test_missing_header_file(self):
        absent = self.trees / "tree_9_9_9.dat"
        with self.assertRaises(c2l.CtreesToLhaloError) as caught:
            c2l.read_column_map(absent)
        self.assertEqual(caught.exception.exit_status, 2)
        self.assertEqual(caught.exception.source, str(absent))
        self.assertIn("could not open", str(caught.exception))

    def test_unreadable_tree_file(self):
        absent = str(self.trees / "tree_9_9_9.dat")
        with c2l.TreeFiles((absent,)) as files:
            with self.assertRaises(c2l.CtreesToLhaloError) as caught:
                files.tree_text(0, 0)
        self.assertEqual(caught.exception.exit_status, 2)
        self.assertEqual(caught.exception.source, absent)
        self.assertIn("could not read", str(caught.exception))

    def test_file_table_grows_once_per_line(self):
        # A line whose id exceeds the table's size doubles it once, for that line
        # (ctrees_utils.c lines 129-151); the id must then be below the size. 2001
        # doubles 2000 to 4000, so 2001 and 3999 fit; 4000 equals the size then and is
        # refused, whether met first (2000 doubles to 4000) or after 2001. A later line
        # with 4001 doubles the table again, to 8000.
        c2l._check_file_table_growth(np.array([0, 2001, 3999], dtype=np.int32), "loc")
        c2l._check_file_table_growth(np.array([0, 2001, 4001], dtype=np.int32), "loc")
        for file_ids in ([0, 4000], [0, 2001, 4000]):
            with self.subTest(file_ids=file_ids):
                with self.assertRaises(c2l.CtreesToLhaloError) as caught:
                    c2l._check_file_table_growth(np.array(file_ids, dtype=np.int32), "loc")
                self.assertEqual(caught.exception.exit_status, 2)
                self.assertEqual(caught.exception.source, "loc")
                self.assertIn("file id 4000", str(caught.exception))

    def test_forest_count_limit(self):
        # sage-model needs totnforests < INT_MAX = 2147483647 (read_tree_consistentrees_ascii.c
        # lines 76-79); the check is called with literal counts, not 2**31 forests.
        c2l._check_forest_count(2147483646, "forests.list")
        with self.assertRaises(c2l.CtreesToLhaloError) as caught:
            c2l._check_forest_count(2147483647, "forests.list")
        self.assertEqual(caught.exception.exit_status, 2)
        self.assertEqual(caught.exception.source, "forests.list")

    def test_empty_batch_of_forests(self):
        table = self.read()
        cmap = c2l.read_column_map(table.tree_files[0])
        with c2l.TreeFiles(table.tree_files) as files:
            rows, tree_nhalos, forest_nhalos = c2l.read_forests(table, [], files, cmap)
        self.assertEqual((rows.dtype, rows.size), (c2l.ROW_DTYPE, 0))
        self.assertEqual((tree_nhalos.dtype, tree_nhalos.size), (np.dtype(np.int64), 0))
        self.assertEqual((forest_nhalos.dtype, forest_nhalos.size), (np.dtype(np.int64), 0))

    def test_repeated_tree_id(self):
        self.write_forests([(13, 7), (11, 7), (11, 5)])
        self.write_locations([(11, 0), (11, 0), (13, 0)])
        self.assertRefused("appears more than once", self.forests_list)


# --------------------------------------------------------------------------------------
# Output files and the command line
# --------------------------------------------------------------------------------------


class TestFileSplit(unittest.TestCase):
    """setup_forests_io_ctrees' split (read_tree_consistentrees_ascii.c lines 83-124)."""

    def ranges(self, nforests, n_files):
        return [c2l.file_forest_range(nforests, n_files, t, "f.list") for t in range(n_files)]

    def test_split_with_a_remainder(self):
        # floor(10/3) = 3, remainder 1: file 0 takes 4; starts 3t + min(t, 1).
        self.assertEqual(self.ranges(10, 3), [(0, 4), (4, 7), (7, 10)])
        # floor(7/4) = 1, remainder 3: files 0-2 take 2, file 3 takes 1.
        self.assertEqual(self.ranges(7, 4), [(0, 2), (2, 4), (4, 6), (6, 7)])
        # floor(128/5) = 25, remainder 3.
        self.assertEqual(self.ranges(128, 5), [(0, 26), (26, 52), (52, 78), (78, 103), (103, 128)])

    def test_split_without_a_remainder_and_one_forest_per_file(self):
        self.assertEqual(self.ranges(6, 3), [(0, 2), (2, 4), (4, 6)])
        self.assertEqual(self.ranges(3, 3), [(0, 1), (1, 2), (2, 3)])
        self.assertEqual(self.ranges(5, 1), [(0, 5)])

    def test_more_files_than_forests_is_refused(self):
        # sage-model refuses a task with no forests (lines 121-124); the module refuses
        # every file of such a run, including those below F.
        for file_number in (0, 3):
            with self.subTest(file_number=file_number):
                with self.assertRaises(c2l.CtreesToLhaloError) as caught:
                    c2l.file_forest_range(3, 4, file_number, "f.list")
                self.assertEqual(caught.exception.exit_status, 2)
                self.assertEqual(caught.exception.source, "f.list")
                self.assertIn("4 output files but 3 forests", str(caught.exception))

    def test_int_max_count_check(self):
        # PWRITE_64BIT_TO_32BIT (src/sage.c lines 421-444) refuses a count above
        # INT_MAX = 2147483647; the check is called with literals, not 2**31 forests.
        c2l.check_int32_count(2147483647, "the number of halos in the file", "out.0")
        with self.assertRaises(c2l.CtreesToLhaloError) as caught:
            c2l.check_int32_count(2147483648, "the number of halos in the file", "out.0")
        self.assertEqual(caught.exception.exit_status, 2)
        self.assertEqual(caught.exception.source, "out.0")
        self.assertIn("2147483648 does not fit inside 32 bits", str(caught.exception))

    def test_file_and_forest_counts_above_int_max_are_refused(self):
        # A file's forest count of 2147483648 (one file, 2**31 forests) ...
        with self.assertRaises(c2l.CtreesToLhaloError) as caught:
            c2l.file_forest_range(2147483648, 1, 0, "f.list")
        self.assertEqual(caught.exception.exit_status, 2)
        self.assertIn("forests in a file = 2147483648", str(caught.exception))
        # ... and a file count of 2147483648.
        with self.assertRaises(c2l.CtreesToLhaloError) as caught:
            c2l.file_forest_range(4294967296, 2147483648, 0, "f.list")
        self.assertIn("output files = 2147483648", str(caught.exception))
        # 2147483647 forests in one file fit.
        self.assertEqual(c2l.file_forest_range(2147483647, 1, 0, "f.list"), (0, 2147483647))

    def test_bad_file_number_is_a_programming_error(self):
        for n_files, file_number in ((0, 0), (3, 3), (3, -1)):
            with self.subTest(n_files=n_files, file_number=file_number):
                with self.assertRaises(ValueError):
                    c2l.file_forest_range(10, n_files, file_number, "f.list")


# Four ASCII trees in one tree file, three forests. In sage-model's order: forest 0 is
# ForestID 5 (tree 12), forest 1 is ForestID 7 (trees 11 then 13, by offset), forest 2 is
# ForestID 9 (tree 14). PartMass 0.5, so 1/PartMass = 2.0 and Len = 2 Mvir.
CLI_TREES = {
    12: [halo(12, "1.00000", 1)],
    11: [
        halo(11, "1.00000", 1, mvir="2e10"),
        halo(5, "0.50000", 0, desc_id=11, desc_scale="1.00000"),
    ],
    13: [halo(13, "1.00000", 1, mvir="4e10", pid=11, upid=11)],
    14: [halo(14, "1.00000", 1)],
}
CLI_FORESTS = [(14, 9), (13, 7), (12, 5), (11, 7)]

# Forest ForestID 5: one halo; SubhaloIndex 0 + 1 = 1.
FOREST_5 = expected_records([R(12, 1, -1, -1, -1, 0, -1, 2, 1.0, 1)])
# Forest ForestID 7, output order (scale desc, upid, pid, id): 11 (FoF), 13 (its
# subhalo, upid 11), then 5 at scale 0.5, whose descendant is 11 (index 0), so 11's
# FirstProgenitor is 2. SubhaloIndex: tree 11 -> 0 + 2 = 2, tree 13 -> 2 + 1 = 3.
FOREST_7 = expected_records(
    [
        R(11, 1, -1, 2, -1, 0, 1, 4, 2.0, 2),
        R(13, 1, -1, -1, -1, 0, -1, 8, 4.0, 3),
        R(5, 0, 0, -1, -1, 2, -1, 2, 1.0, 2),
    ]
)
# Forest ForestID 9: halo 14, written out by hand, field by field in struct halo_data's
# order, little-endian: Descendant, FirstProgenitor, NextProgenitor -1; FirstHaloInFOFgroup
# 0; NextHaloInFOFgroup -1; Len 2; M_Mean200, Mvir, M_TopHat 1.0f (0x3f800000); Pos 1.5f,
# 2.5f, 3.5f; Vel -1.25f, 0.5f, 4.0f; VelDisp 100.5f (0x42c90000); Vmax 200.25f
# (0x43484000); Spin 0, 0, 0; MostBoundID 14 (int64); SnapNum 1; FileNr -1;
# SubhaloIndex 1; SubHalfMass -1.0f (0xbf800000). 104 bytes.
FOREST_9_BYTES = bytes.fromhex(
    "ffffffff ffffffff ffffffff"  # Descendant, FirstProgenitor, NextProgenitor
    " 00000000 ffffffff 02000000"  # FirstHaloInFOFgroup, NextHaloInFOFgroup, Len
    " 0000803f 0000803f 0000803f"  # M_Mean200, Mvir, M_TopHat
    " 0000c03f 00002040 00006040"  # Pos
    " 0000a0bf 0000003f 00008040"  # Vel
    " 0000c942 00404843"  # VelDisp, Vmax
    " 00000000 00000000 00000000"  # Spin
    " 0e00000000000000"  # MostBoundID
    " 01000000 ffffffff 01000000 000080bf"  # SnapNum, FileNr, SubhaloIndex, SubHalfMass
)


def int32s(*values):
    """Little-endian int32 bytes, written out byte by byte for the small values used."""
    table = {
        1: b"\x01\x00\x00\x00",
        2: b"\x02\x00\x00\x00",
        3: b"\x03\x00\x00\x00",
        4: b"\x04\x00\x00\x00",
        5: b"\x05\x00\x00\x00",
    }
    return b"".join(table[v] for v in values)


class CliFixture(unittest.TestCase):
    """A synthetic tree directory, tables and output directory for the command line."""

    TREE_NAME = "Synthetic_lhalo_binary"

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        root = Path(self.tmp.name)
        self.trees = root / "trees"
        self.lists = root / "lists"
        self.out = root / "out"
        for directory in (self.trees, self.lists, self.out):
            directory.mkdir()
        self.write_trees(CLI_TREES)
        self.forests_list = self.lists / "forests.list"
        body = "".join(f"{t} {f}\n" for t, f in CLI_FORESTS)
        self.forests_list.write_text("#TreeRootID ForestID\n" + body)
        self.locations = self.trees / "locations.dat"
        body = "".join(f"{t} 0 {self.offsets[t]} tree_0_0_0.dat\n" for t, _ in CLI_FORESTS)
        self.locations.write_text("#TreeRootID FileID Offset Filename\n" + body)

    def tearDown(self):
        self.tmp.cleanup()

    def write_trees(self, trees, name="tree_0_0_0.dat"):
        content = HEADER
        self.offsets = {}
        for root_id, rows in trees.items():
            content += b"#tree %d\n" % root_id
            self.offsets[root_id] = len(content)
            content += b"".join(rows)
        (self.trees / name).write_bytes(content)

    def argv(self, n_files=3, files="0-2", **changes):
        options = {
            "--forests-list": str(self.forests_list),
            "--locations": str(self.locations),
            "--n-files": str(n_files),
            "--files": files,
            "--particle-mass": "0.5",
            "--tree-name": self.TREE_NAME,
            "--out": str(self.out),
        }
        options.update(changes)
        return [
            item for key, value in options.items() if value is not None for item in (key, value)
        ]

    def run_main(self, argv):
        stderr = io.StringIO()
        with contextlib.redirect_stderr(stderr):
            status = c2l.main(argv)
        return status, stderr.getvalue()

    def output(self, t):
        return self.out / f"{self.TREE_NAME}.{t}"

    def listing(self):
        return sorted(p.name for p in self.out.iterdir())


class TestWriteFile(CliFixture):
    """write_lhalo_forests: header and records equal literal bytes."""

    def write(self, start, stop):
        table = c2l.read_forest_table(self.forests_list, self.locations)
        cmap = c2l.read_column_map(c2l.header_tree_file(self.locations))
        handle = io.BytesIO()
        with c2l.TreeFiles(table.tree_files) as files:
            nhalos = c2l.write_lhalo_forests(handle, table, start, stop, files, cmap, 0.5, "x")
        return handle.getvalue(), nhalos

    def test_header_and_records_of_two_forests(self):
        data, nhalos = self.write(0, 2)
        # Ntrees 2, totNHalos 1 + 3 = 4, TreeNHalos [1, 3], then the forests in turn.
        header = int32s(2, 4, 1, 3)
        self.assertEqual(data[:16], header)
        self.assertEqual(data, header + FOREST_5.tobytes() + FOREST_7.tobytes())
        self.assertEqual(len(data), 16 + 4 * 104)
        self.assertEqual(nhalos.tolist(), [1, 3])

    def test_header_and_record_bytes_of_one_forest(self):
        data, _ = self.write(2, 3)
        self.assertEqual(data, int32s(1, 1, 1) + FOREST_9_BYTES)

    def test_header_tree_file_is_tree_0_0_0_beside_locations(self):
        self.assertEqual(c2l.header_tree_file("/a/b/locations.dat"), "/a/b/tree_0_0_0.dat")
        self.assertEqual(c2l.header_tree_file(self.locations), f"{self.trees}/tree_0_0_0.dat")


class TestCommandLine(CliFixture):
    """main(): exactly files A-B, refusals, per-file temporaries, sidecars, exit statuses."""

    def test_writes_exactly_files_a_to_b(self):
        status, stderr = self.run_main(self.argv(n_files=3, files="1-2"))
        self.assertEqual(status, 0, stderr)
        name = self.TREE_NAME
        self.assertEqual(
            self.listing(), [f"{name}.1", f"{name}.1.json", f"{name}.2", f"{name}.2.json"]
        )
        self.assertEqual(self.output(1).read_bytes(), int32s(1, 3, 3) + FOREST_7.tobytes())
        self.assertEqual(self.output(2).read_bytes(), int32s(1, 1, 1) + FOREST_9_BYTES)

    def test_two_files_with_a_remainder(self):
        # F = 3, N = 2: file 0 holds forests 0-1, file 1 forest 2.
        status, stderr = self.run_main(self.argv(n_files=2, files="0-1"))
        self.assertEqual(status, 0, stderr)
        self.assertEqual(
            self.output(0).read_bytes(),
            int32s(2, 4, 1, 3) + FOREST_5.tobytes() + FOREST_7.tobytes(),
        )
        self.assertEqual(self.output(1).read_bytes(), int32s(1, 1, 1) + FOREST_9_BYTES)

    def test_sidecar_records_counts_inputs_and_provenance(self):
        status, stderr = self.run_main(self.argv(n_files=2, files="0-0"))
        self.assertEqual(status, 0, stderr)
        record = json.loads((self.out / f"{self.TREE_NAME}.0.json").read_text())
        self.assertEqual(record["nforests"], 2)
        self.assertEqual(record["nhalos"], 4)
        self.assertEqual(
            record["forest_range"],
            {
                "start": 0,
                "stop": 2,
                "total_forests": 3,
                "first_forest_id": 5,
                "last_forest_id": 7,
            },
        )
        self.assertEqual(record["particle_mass"], 0.5)
        self.assertEqual(record["particle_mass_argument"], "0.5")
        self.assertEqual(record["sage_model_commit"], "9218e11")
        with open(c2l.__file__, "rb") as handle:
            self.assertEqual(record["script_sha256"], hashlib.sha256(handle.read()).hexdigest())
        tree_file = self.trees / "tree_0_0_0.dat"
        inputs = record["inputs"]
        for key, path in (
            ("forests_list", self.forests_list),
            ("locations", self.locations),
            ("header_tree_file", tree_file),
        ):
            self.assertEqual(inputs[key], {"path": str(path), "size_bytes": path.stat().st_size})
        self.assertEqual(
            inputs["tree_files"],
            [{"file_id": 0, "path": str(tree_file), "size_bytes": tree_file.stat().st_size}],
        )
        self.assertEqual(
            record["output"],
            {
                "path": str(self.output(0)),
                "size_bytes": 16 + 4 * 104,
                "tree_name": self.TREE_NAME,
                "file_number": 0,
                "n_files": 2,
            },
        )
        self.assertLessEqual(record["started_utc"], record["finished_utc"])
        self.assertTrue(record["started_utc"].endswith("+00:00"))

    def test_each_file_is_renamed_complete_from_its_own_temporary(self):
        renames = []
        real_replace = os.replace

        def recording_replace(source, destination):
            # At the rename: the temporary is complete and the final name is free.
            renames.append(
                (
                    Path(source).name,
                    Path(destination).name,
                    Path(source).parent == self.out,
                    Path(source).stat().st_size,
                    os.path.lexists(destination),
                )
            )
            real_replace(source, destination)

        with mock.patch.object(c2l.os, "replace", recording_replace):
            status, stderr = self.run_main(self.argv(n_files=3, files="0-1"))
        self.assertEqual(status, 0, stderr)
        name = self.TREE_NAME
        self.assertEqual(
            [r[1] for r in renames],
            [f"{name}.0", f"{name}.0.json", f"{name}.1", f"{name}.1.json"],
        )
        sizes = {f"{name}.0": 12 + 104, f"{name}.1": 12 + 3 * 104}
        for temporary, final, in_out, size, existed in renames:
            with self.subTest(final=final):
                stem = final[: -len(".json")] if final.endswith(".json") else final
                self.assertTrue(temporary.startswith(f".{stem}."), temporary)
                self.assertTrue(temporary.endswith(".partial"), temporary)
                self.assertTrue(in_out)
                self.assertFalse(existed)
                if final in sizes:
                    self.assertEqual(size, sizes[final])
        self.assertEqual(len({r[0] for r in renames}), 4)
        self.assertEqual(
            self.listing(), [f"{name}.0", f"{name}.0.json", f"{name}.1", f"{name}.1.json"]
        )

    def test_existing_output_or_sidecar_refuses_the_whole_range(self):
        name = self.TREE_NAME
        for existing in (f"{name}.2", f"{name}.2.json"):
            with self.subTest(existing=existing):
                for path in self.out.iterdir():
                    path.unlink()
                (self.out / existing).write_bytes(b"keep")
                status, stderr = self.run_main(self.argv(n_files=3, files="0-2"))
                self.assertEqual(status, 3)
                self.assertIn("refusing to overwrite", stderr)
                self.assertIn(existing, stderr)
                self.assertEqual(self.listing(), [existing])
                self.assertEqual((self.out / existing).read_bytes(), b"keep")

    def test_output_outside_the_range_is_left_alone(self):
        (self.out / f"{self.TREE_NAME}.0").write_bytes(b"keep")
        status, stderr = self.run_main(self.argv(n_files=3, files="1-1"))
        self.assertEqual(status, 0, stderr)
        self.assertEqual(self.output(0).read_bytes(), b"keep")

    def test_a_failing_file_leaves_no_partial_output(self):
        # Forest 2 (tree 14) has no FoF at its last scale: refused, exit 2, naming the
        # forest's tree file. File 1 was already complete and stays; nothing of file 2.
        trees = dict(CLI_TREES)
        trees[14] = [halo(14, "1.00000", 1, pid=99, upid=99)]
        self.write_trees(trees)
        status, stderr = self.run_main(self.argv(n_files=3, files="1-2"))
        self.assertEqual(status, 2)
        self.assertIn(f"{self.trees}/tree_0_0_0.dat (forest 2, ForestID 9)", stderr)
        self.assertIn("no FoF halo", stderr)
        name = self.TREE_NAME
        self.assertEqual(self.listing(), [f"{name}.1", f"{name}.1.json"])

    def test_more_files_than_forests_exits_2_before_writing(self):
        status, stderr = self.run_main(self.argv(n_files=4, files="0-0"))
        self.assertEqual(status, 2)
        self.assertIn(str(self.forests_list), stderr)
        self.assertIn("4 output files but 3 forests", stderr)
        self.assertEqual(self.listing(), [])

    def test_read_errors_exit_2_naming_the_file(self):
        missing = self.lists / "absent.list"
        status, stderr = self.run_main(self.argv(**{"--forests-list": str(missing)}))
        self.assertEqual(status, 2)
        self.assertIn(f"{missing}: can't open the file", stderr)
        # The header comes from tree_0_0_0.dat beside locations.dat, whatever file id 0
        # is called: here the trees live in tree_a.dat and tree_0_0_0.dat is absent.
        (self.trees / "tree_0_0_0.dat").rename(self.trees / "tree_a.dat")
        text = self.locations.read_text().replace("tree_0_0_0.dat", "tree_a.dat")
        self.locations.write_text(text)
        status, stderr = self.run_main(self.argv())
        self.assertEqual(status, 2)
        self.assertIn(f"{self.trees}/tree_0_0_0.dat: could not open the file", stderr)
        self.assertEqual(self.listing(), [])

    def test_usage_errors_exit_3_and_write_nothing(self):
        cases = {
            "missing option": {"--tree-name": None},
            "A greater than B": {"--files": "2-1"},
            "B not below N": {"--files": "0-3"},
            "malformed range": {"--files": "0:2"},
            "single number": {"--files": "1"},
            "zero files": {"--n-files": "0", "--files": "0-0"},
            "non-integer files": {"--n-files": "two"},
            "zero particle mass": {"--particle-mass": "0"},
            "negative particle mass": {"--particle-mass": "-0.5"},
            "nan particle mass": {"--particle-mass": "nan"},
            "non-numeric particle mass": {"--particle-mass": "heavy"},
            "tree name with a slash": {"--tree-name": "a/b"},
            "missing output directory": {"--out": str(self.out / "absent")},
            "unknown option": {"--default-mass": "1"},
        }
        for name, changes in cases.items():
            with self.subTest(name):
                status, stderr = self.run_main(self.argv(**changes))
                self.assertEqual(status, 3, stderr)
                self.assertIn("error:", stderr)
                self.assertEqual(self.listing(), [])

    def test_process_exit_statuses(self):
        # The script's own process: 0 on success, 3 on a usage error, 2 on a data error.
        script = c2l.__file__

        def run(argv):
            return subprocess.run(
                [sys.executable, script, *argv], capture_output=True, text=True
            ).returncode

        self.assertEqual(run(self.argv(files="2-1")), 3)
        self.assertEqual(run(self.argv(n_files=4, files="0-0")), 2)
        self.assertEqual(run(self.argv(files="0-0")), 0)
        self.assertEqual(self.listing(), [f"{self.TREE_NAME}.0", f"{self.TREE_NAME}.0.json"])


# --------------------------------------------------------------------------------------
# The particle-mass interval
# --------------------------------------------------------------------------------------


def write_lhalo_file(path, forests):
    """An L-Halo file whose forests are lists of (Mvir, Len); every other field zero."""
    counts = [len(forest) for forest in forests]
    records = np.zeros(sum(counts), dtype=c2l.RECORD_DTYPE)
    pairs = [pair for forest in forests for pair in forest]
    records["Mvir"] = [mvir for mvir, _ in pairs]
    records["Len"] = [length for _, length in pairs]
    with open(path, "wb") as handle:
        handle.write(np.array([len(forests), len(records)], dtype="<i4").tobytes())
        handle.write(np.array(counts, dtype="<i4").tobytes())
        handle.write(records.tobytes())


# Two files built with PartMass 0.5 (1.0 / 0.5 = 2.0 exactly), Len = roundf(Mvir * 2):
#   A: Mvir 1.25 -> product 2.5 exactly -> Len 3 (half away from zero);
#   B: Mvir 1.0  -> product 2.0         -> Len 2;
#   Z: Mvir 0.0  -> Len 0 under every mass.
# The interval, by hand, with r = fl64(1.0 / PartMass) (ulps: double 2**-52 in [1, 2),
# 2**-51 in [2, 4), 2**-53 in [0.5, 1), 2**-54 in [0.25, 0.5); float32 2**-22 in [2, 4)):
# * A gives the lower bound on r. Len >= 3 needs float32(fl64(1.25 r)) >= 2.5, i.e.
#   fl64(1.25 r) >= t = 2.5 - 2**-23 (the midpoint below 2.5, which ties to 2.5: even).
#   t is even in double, so the midpoint below it ties to it: 1.25 r >= t - 2**-52, r >= 2
#   - 0.8 * 2**-23 - 0.8 * 2**-52 = 2 - 429496730.4 * 2**-52, so (r a multiple of 2**-52)
#   r_lo = 2 - 429496730 * 2**-52.
#   (There 1.25 r = t - 2**-53 rounds to t, whose float32 is exactly 2.5: Len 3. One ulp
#   lower, 1.25 r = t - 1.5 * 2**-52 rounds below t: float32 2.5 - 2**-22, Len 2.)
# * B gives the upper bound on r. Len <= 2 needs float32(r) < 2.5, i.e. r < 2.5 - 2**-23
#   (the midpoint ties to 2.5), so r_hi = 2.5 - 2**-23 - 2**-51 (odd in double).
# * PartMass_hi: the largest P with fl64(1/P) >= r_lo. r_lo is even, so the midpoint
#   r_lo - 2**-53 rounds to it: 1/P >= r_lo - 2**-53, P <= 2**53 / (2**54 - 858993461),
#   P in [0.5, 1): P_hi = floor(2**106 / (2**54 - 858993461)) * 2**-53
#   = 0x1.000000cccccd7p-1 (0.500000023841859).
# * PartMass_lo: the smallest P with fl64(1/P) <= r_hi. r_hi is odd, so the midpoint
#   r_hi + 2**-52 rounds up past it: 1/P < 2.5 - 2**-23 - 2**-52, P > 2**52 /
#   11258998531555327, P in [0.25, 0.5): P_lo = (floor(2**106 / 11258998531555327) + 1) *
#   2**-54 = 0x1.99999ae147af3p-2 (0.4000000190734873).
# At P_hi, fl64(1/P_hi) = r_lo and A's product 2.5 - 2**-23 - 2**-53 (double) narrows to
# exactly 2.5 (n + 0.5): Len 3. One ulp above P_hi, A's Len is 2; one ulp below P_lo,
# B's Len is 3. The shortest decimal in [P_lo, P_hi] is 0.5 (0.4 lies below P_lo).
PM_LOW = float.fromhex("0x1.99999ae147af3p-2")
PM_HIGH = float.fromhex("0x1.000000cccccd7p-1")
PM_FILES = (
    [[(1.25, 3), (1.0, 2)], [(0.0, 0)]],  # two forests
    [[(1.0, 2), (1.25, 3), (1.25, 3)]],
)


class TestParticleMassInterval(unittest.TestCase):
    """The particle masses that reproduce a set of Len values, against literals."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.paths = []
        for k, forests in enumerate(PM_FILES):
            path = os.path.join(self.tmp.name, f"lhalo.{k}")
            write_lhalo_file(path, forests)
            self.paths.append(path)

    def test_interval_of_files_built_with_half(self):
        got = c2l.particle_mass_interval_of_files(self.paths)
        self.assertEqual(got.low.hex(), PM_LOW.hex())
        self.assertEqual(got.high.hex(), PM_HIGH.hex())
        self.assertEqual(got.value, "0.5")
        self.assertEqual((got.n_checked, got.n_reproduced), (6, 6))
        self.assertTrue(got.contains(0.5))
        self.assertFalse(got.contains(0.0325))
        self.assertFalse(got.contains(0.4))

    def test_endpoints_reproduce_and_their_neighbours_change_a_len(self):
        mvir = np.array([1.25, 1.0, 0.0], dtype=np.float32)
        length = np.array([3, 2, 0])
        for part_mass in (PM_LOW, PM_HIGH, 0.5):
            self.assertEqual(c2l.count_reproduced(mvir, length, part_mass), 3, part_mass)
        # The conversion itself (convert_forest) gives the same Len at and beyond each end.
        # strtof("1.25e10") = 12499999744 (12207031 * 1024), times 1e-10 = 1.2499999744,
        # within half a float32 ulp (5.96e-8) of 1.25: Mvir 1.25; "1e10" gives 1.0.
        lens = {}
        for part_mass in (np.nextafter(PM_LOW, 0), PM_LOW, PM_HIGH, np.nextafter(PM_HIGH, 1)):
            rows = [halo(1, "1.00000", 0, mvir="1.25e10"), halo(2, "1.00000", 0, mvir="1e10")]
            got = convert([rows], float(part_mass))
            by_mvir = dict(zip(got["Mvir"].tolist(), got["Len"].tolist(), strict=True))
            lens[float(part_mass)] = (by_mvir[1.25], by_mvir[1.0])
        self.assertEqual(
            list(lens.values()),
            [(3, 3), (3, 2), (3, 2), (2, 2)],  # B changes below P_lo, A above P_hi
        )

    def test_product_narrowing_to_n_plus_half_at_the_upper_end(self):
        inv = np.float64(1.0) / np.float64(PM_HIGH)
        self.assertEqual(inv.hex(), (2 - 429496730 * 2.0**-52).hex())
        product = np.float64(np.float32(1.25)) * inv
        self.assertEqual(product.hex(), (2.5 - 2.0**-23).hex())
        self.assertEqual(np.float32(product), np.float32(2.5))
        self.assertEqual(c2l.sage_len(np.array([1.25], np.float32), inv).tolist(), [3.0])

    def test_len_values_no_mass_reproduces(self):
        got = c2l.particle_mass_interval(np.array([1.0, 1.0], np.float32), np.array([2, 3]))
        self.assertTrue(got.empty)
        self.assertEqual((got.low, got.high, got.value), (None, None, None))
        self.assertEqual((got.n_checked, got.n_reproduced), (2, 0))
        self.assertFalse(got.contains(0.5))

    def test_a_nonzero_len_of_zero_mass_is_never_reproduced(self):
        got = c2l.particle_mass_interval(np.array([0.0], np.float32), np.array([1]))
        self.assertTrue(got.empty)

    def test_refusals(self):
        with self.assertRaises(ValueError):
            c2l.particle_mass_interval(np.array([], np.float32), np.array([], np.int32))
        with self.assertRaises(ValueError):
            c2l.particle_mass_interval(np.array([-1.0], np.float32), np.array([2]))
        with self.assertRaises(ValueError):
            c2l.particle_mass_interval(np.array([np.nan], np.float32), np.array([2]))
        with self.assertRaises(ValueError):
            c2l.particle_mass_interval(np.array([1.0]), np.array([2]))  # float64 Mvir
        truncated = os.path.join(self.tmp.name, "truncated")
        with open(self.paths[0], "rb") as source, open(truncated, "wb") as target:
            target.write(source.read()[:-1])
        with self.assertRaisesRegex(ValueError, "truncated"):
            c2l.read_lhalo_mvir_and_len(truncated)

    def test_shortest_decimal(self):
        self.assertEqual(c2l.shortest_decimal(PM_LOW, PM_HIGH), "0.5")
        self.assertEqual(c2l.shortest_decimal(1.0, 1.0), "1")
        self.assertEqual(c2l.shortest_decimal(0.099, 0.101), "0.1")  # a power of ten
        self.assertEqual(c2l.shortest_decimal(0.03265, 0.03275), "0.0327")
        # 0.2 and 0.3 are equally near the midpoint 0.25: the smaller.
        self.assertEqual(c2l.shortest_decimal(0.125, 0.375), "0.2")
        # Fewest digits first, then the one nearest the midpoint 555.2.
        self.assertEqual(c2l.shortest_decimal(123.4, 987.0), "600")


if __name__ == "__main__":
    unittest.main()
