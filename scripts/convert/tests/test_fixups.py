"""Slice 5 unit tests: adjacency validation, spin/Len conventions, fix_flybys,
fix_upid, and the fix-up pipeline stage with a hand-computed golden fixture."""

import contextlib
import io
import os
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import convert_ctrees  # noqa: E402
import fixtures  # noqa: E402
from ctrees_parser import RECORD_DTYPE, ConverterError  # noqa: E402
from fixups import (  # noqa: E402
    FIXED_DTYPE_TAG,
    FIXED_RECORD_DTYPE,
    derive_len,
    fix_flybys_snapshot,
    fix_upid_snapshot,
    fixed_scratch_name,
    load_particle_mass,
    normalise_spin,
    round_half_away_from_zero,
    run_fixups,
    validate_adjacency,
)
from scatter import Manifest, run_finalize, run_scatter  # noqa: E402
from sort_index import run_sort, sorted_scratch_name  # noqa: E402

#: fixture a_list as float64 (index = SnapNum)
A_LIST = np.asarray(fixtures.A_LIST, dtype=np.float64)


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

    Every stage reports its consumptions and its consumed-input skips through
    ``_log``, and the plan Slice 8 acceptance criteria are about those messages
    as much as about the bytes, so the tests read them rather than inferring
    them.
    """
    buffer = io.StringIO()
    with contextlib.redirect_stderr(buffer):
        yield _Captured(buffer)


def make_fixed(rows):
    """Build an id-sorted FIXED_RECORD_DTYPE array from per-halo dicts."""
    records = np.zeros(len(rows), dtype=FIXED_RECORD_DTYPE)
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


class TestParticleMass(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def test_loads_fixture_value(self):
        info = fixtures.write_simulation_info(self.root / "simulation_info.yaml")
        self.assertEqual(load_particle_mass(info), 0.0325)

    def _write(self, body: str) -> Path:
        path = self.root / "info.yaml"
        path.write_text(body)
        return path

    def test_nonpositive_mass_aborts(self):
        for value in ("0.0", "-1.0", ".nan"):
            path = self._write(
                "simulation:\n  particle_mass: {value: %s, units: 1e10 Msun/h}\n" % value
            )
            with self.assertRaisesRegex(ConverterError, "positive and finite|malformed"):
                load_particle_mass(path)

    def test_missing_key_aborts(self):
        path = self._write("simulation:\n  cosmology: {hubble_h: 0.7}\n")
        with self.assertRaisesRegex(ConverterError, "particle_mass"):
            load_particle_mass(path)

    def test_wrong_units_abort(self):
        path = self._write("simulation:\n  particle_mass: {value: 0.03, units: Msun/h}\n")
        with self.assertRaisesRegex(ConverterError, "units"):
            load_particle_mass(path)


class TestRounding(unittest.TestCase):
    def test_half_ties_round_away_from_zero(self):
        # NumPy banker's rounding would give [0, 2, 2, 4, 4]
        ties = np.asarray([0.5, 1.5, 2.5, 3.5, 4.5])
        np.testing.assert_array_equal(round_half_away_from_zero(ties), [1, 2, 3, 4, 5])

    def test_non_ties_match_nearest(self):
        values = np.asarray([0.0, 0.4999, 1.0001, 2.9, 153.84615069538464])
        np.testing.assert_array_equal(round_half_away_from_zero(values), [0, 0, 1, 3, 154])

    def test_nextafter_around_ties(self):
        # one float64 ulp below/above an exact tie must round to nearest
        below = np.nextafter(2.5, -np.inf)
        above = np.nextafter(2.5, np.inf)
        np.testing.assert_array_equal(
            round_half_away_from_zero(np.asarray([below, 2.5, above])), [2, 3, 3]
        )


class TestDeriveLen(unittest.TestCase):
    def test_exact_half_tie_rounds_up(self):
        # 2e-10 is exactly 2 * float(1e-10) (power-of-two scaling), so the
        # derived count is exactly 0.5: C round() gives 1, banker's gives 0
        mvir = np.asarray([1.0], dtype=np.float32)
        len32, zero_count = derive_len(mvir, 2e-10, "test")
        self.assertEqual(float(mvir[0].astype(np.float64) * 1e-10 / 2e-10), 0.5)
        self.assertEqual(len32.tolist(), [1])
        self.assertEqual(zero_count, 0)

    def test_zero_len_preserved_and_counted(self):
        len32, zero_count = derive_len(np.asarray([0.0], dtype=np.float32), 0.0325, "test")
        self.assertEqual(len32.tolist(), [0])
        self.assertEqual(zero_count, 1)

    def test_negative_mass_aborts(self):
        with self.assertRaisesRegex(ConverterError, "invalid derived particle count"):
            derive_len(np.asarray([-1.0e11], dtype=np.float32), 0.0325, "test")

    def test_overflow_aborts(self):
        with self.assertRaisesRegex(ConverterError, "invalid derived particle count"):
            derive_len(np.asarray([1.0e30], dtype=np.float32), 1e-12, "test")

    def test_non_finite_aborts(self):
        with self.assertRaisesRegex(ConverterError, "invalid derived particle count"):
            derive_len(np.asarray([3.4e38], dtype=np.float32), 1e-300, "test")

    def test_int32_dtype(self):
        len32, _ = derive_len(np.asarray([1.0e12], dtype=np.float32), 0.0325, "test")
        self.assertEqual(len32.dtype, np.int32)
        self.assertEqual(len32.tolist(), [3077])

    def test_int32_boundary_just_under_passes(self):
        # (2^24 - 1) * 128 = 2147483520 exactly (power-of-two scaling), the
        # largest representable derived count below INT32_MAX in this family
        mvir = np.asarray([16777215.0], dtype=np.float32)
        pm = 1e-10 / 128.0
        self.assertEqual(float(mvir[0].astype(np.float64) * 1e-10 / pm), 2147483520.0)
        len32, _ = derive_len(mvir, pm, "test")
        self.assertEqual(len32.tolist(), [2147483520])

    def test_int32_boundary_just_over_aborts(self):
        # 2^31 * s / s = exactly 2147483648.0 > INT32_MAX (strict > gate)
        mvir = np.asarray([2.0**31], dtype=np.float32)
        self.assertEqual(float(mvir[0].astype(np.float64) * 1e-10 / 1e-10), 2147483648.0)
        with self.assertRaisesRegex(ConverterError, "invalid derived particle count"):
            derive_len(mvir, 1e-10, "test")


class TestNormaliseSpin(unittest.TestCase):
    def test_reference_formula_multiply_by_reciprocal(self):
        records = make_fixed([{"id": 1, "Mvir": 1.3e12, "Jx": 1.0e10, "Jy": -2.0e10, "Jz": 3.0e10}])
        raw = {k: records[k][0].copy() for k in ("Jx", "Jy", "Jz")}
        normalise_spin(records)
        inv = 1.0 / np.float64(records["Mvir"][0])
        for k in ("Jx", "Jy", "Jz"):
            expected = np.float32(np.float64(raw[k]) * inv)
            self.assertEqual(records[k][0], expected)

    def test_zero_mass_carve_out(self):
        records = make_fixed(
            [
                {"id": 1, "Mvir": 0.0, "Jx": 1.0e10, "Jy": 2.0e10, "Jz": 3.0e10},
                {"id": 2, "Mvir": 2.0e11, "Jx": 1.0e10, "Jy": 2.0e10, "Jz": 3.0e10},
            ]
        )
        normalise_spin(records)
        self.assertEqual(records["Jx"][0], np.float32(1.0e10))  # untouched raw J
        self.assertEqual(records["Jy"][0], np.float32(2.0e10))
        self.assertEqual(records["Jz"][0], np.float32(3.0e10))
        self.assertNotEqual(records["Jx"][1], np.float32(1.0e10))  # normalised


class TestValidateAdjacency(unittest.TestCase):
    def test_adjacent_links_pass(self):
        records = make_fixed(
            [
                {"id": 1, "snap": 3, "desc_id": 9, "desc_scale": A_LIST[4]},
                {"id": 2, "snap": 3},  # dying branch: desc_id == -1 is fine
            ]
        )
        validate_adjacency(records, 3, A_LIST, "test")

    def test_unknown_desc_scale_aborts_with_count_and_examples(self):
        records = make_fixed(
            [
                {"id": 1, "snap": 3, "desc_id": 9, "desc_scale": 0.8437},
                {"id": 2, "snap": 3, "desc_id": 10, "desc_scale": 0.8437},
                {"id": 3, "snap": 3, "desc_id": 11, "desc_scale": A_LIST[4]},
            ]
        )
        with self.assertRaisesRegex(
            ConverterError, r"2 descendant link\(s\).*no a_list entry.*id=1.*id=2"
        ):
            validate_adjacency(records, 3, A_LIST, "test")

    def test_non_adjacent_link_aborts(self):
        # desc_scale maps to snapshot 5, but the halo is at snapshot 3
        records = make_fixed([{"id": 1, "snap": 3, "desc_id": 9, "desc_scale": A_LIST[5]}])
        with self.assertRaisesRegex(ConverterError, "non-adjacent"):
            validate_adjacency(records, 3, A_LIST, "test")

    def test_final_snapshot_must_terminate(self):
        final = len(A_LIST) - 1
        records = make_fixed([{"id": 1, "snap": final, "desc_id": 9, "desc_scale": 1.0}])
        with self.assertRaisesRegex(ConverterError, "final snapshot"):
            validate_adjacency(records, final, A_LIST, "test")
        validate_adjacency(make_fixed([{"id": 1, "snap": final}]), final, A_LIST, "test")


class TestFixFlybys(unittest.TestCase):
    def test_multiple_centrals_demote_to_strict_max(self):
        records = make_fixed(
            [
                {"id": 10, "Mvir": 1.0e12, "forest_id": 7, "snap": 5},
                {"id": 11, "Mvir": 8.0e11, "forest_id": 7, "snap": 5},
                {"id": 12, "Mvir": 2.0e11, "forest_id": 7, "snap": 5, "pid": 11, "upid": 11},
            ]
        )
        demoted = fix_flybys_snapshot(records, 5, np.asarray([7]))
        self.assertEqual(demoted, 1)
        # survivor untouched
        self.assertEqual(records["pid"][0], -1)
        self.assertEqual(records["MostBoundID"][0], 10)
        # demoted central: upid and pid rewritten, MostBoundID negated
        self.assertEqual(records["upid"][1], 10)
        self.assertEqual(records["pid"][1], 10)
        self.assertEqual(records["MostBoundID"][1], -11)
        # satellite member: upid rewritten, pid and MostBoundID untouched
        self.assertEqual(records["upid"][2], 10)
        self.assertEqual(records["pid"][2], 11)
        self.assertEqual(records["MostBoundID"][2], 12)

    def test_mass_tie_first_ascending_id_wins(self):
        # strict > semantics: the later equal-mass central does NOT displace
        records = make_fixed(
            [
                {"id": 20, "Mvir": 5.0e11, "forest_id": 8, "snap": 5},
                {"id": 21, "Mvir": 5.0e11, "forest_id": 8, "snap": 5},
            ]
        )
        fix_flybys_snapshot(records, 5, np.asarray([8]))
        self.assertEqual(records["pid"][0], -1)  # id 20 survives
        self.assertEqual(records["MostBoundID"][1], -21)

    def test_single_central_unchanged(self):
        records = make_fixed(
            [
                {"id": 30, "Mvir": 5.0e11, "forest_id": 9, "snap": 5},
                {"id": 31, "Mvir": 1.0e11, "forest_id": 9, "snap": 5, "pid": 30, "upid": 30},
            ]
        )
        before = records.copy()
        self.assertEqual(fix_flybys_snapshot(records, 5, np.asarray([9])), 0)
        self.assertEqual(records.tobytes(), before.tobytes())

    def test_zero_centrals_abort(self):
        records = make_fixed(
            [{"id": 40, "Mvir": 2.0e11, "forest_id": 3, "snap": 5, "pid": 41, "upid": 41}]
        )
        with self.assertRaisesRegex(ConverterError, "zero pid == -1 centrals.*\\[3\\]"):
            fix_flybys_snapshot(records, 5, np.asarray([3]))

    def test_forest_absent_from_snapshot_aborts(self):
        records = make_fixed([{"id": 50, "Mvir": 2.0e11, "forest_id": 1, "snap": 5}])
        with self.assertRaisesRegex(ConverterError, "no halos here"):
            fix_flybys_snapshot(records, 5, np.asarray([1, 2]))

    def test_other_forests_untouched(self):
        records = make_fixed(
            [
                {"id": 60, "Mvir": 1.0e12, "forest_id": 4, "snap": 5},
                {"id": 61, "Mvir": 9.0e11, "forest_id": 4, "snap": 5},
                {"id": 62, "Mvir": 8.0e11, "forest_id": 5, "snap": 5},
                {"id": 63, "Mvir": 7.0e11, "forest_id": 5, "snap": 5},
            ]
        )
        fix_flybys_snapshot(records, 5, np.asarray([4]))  # forest 5 maxes elsewhere
        self.assertEqual(records["MostBoundID"][1], -61)
        self.assertEqual(records["pid"][2], -1)
        self.assertEqual(records["pid"][3], -1)
        self.assertEqual(records["MostBoundID"][3], 63)


class TestFixUpid(unittest.TestCase):
    def test_centrals_get_upid_id_and_keep_pid(self):
        records = make_fixed([{"id": 1, "snap": 4}, {"id": 2, "snap": 4}])
        fix_upid_snapshot(records, 4)
        np.testing.assert_array_equal(records["upid"], [1, 2])
        np.testing.assert_array_equal(records["pid"], [-1, -1])

    def test_sub_subhalo_gets_both_fields(self):
        # pid differs from the ultimate host; both must end at the central
        records = make_fixed(
            [
                {"id": 10, "snap": 5},
                {"id": 11, "snap": 5, "pid": 10, "upid": 10},
                {"id": 12, "snap": 5, "pid": 11, "upid": 10},
            ]
        )
        fix_upid_snapshot(records, 5)
        np.testing.assert_array_equal(records["upid"], [10, 10, 10])
        np.testing.assert_array_equal(records["pid"], [-1, 10, 10])

    def test_deep_chain_resolves(self):
        records = make_fixed(
            [
                {"id": 10, "snap": 5, "pid": 11, "upid": 11},
                {"id": 11, "snap": 5, "pid": 12, "upid": 12},
                {"id": 12, "snap": 5, "pid": 13, "upid": 13},
                {"id": 13, "snap": 5},
            ]
        )
        fix_upid_snapshot(records, 5)
        np.testing.assert_array_equal(records["upid"], [13, 13, 13, 13])
        np.testing.assert_array_equal(records["pid"], [13, 13, 13, -1])

    def test_pid_fallback_when_upid_target_missing(self):
        records = make_fixed(
            [
                {"id": 20, "snap": 5, "pid": 21, "upid": 999},
                {"id": 21, "snap": 5},
            ]
        )
        fix_upid_snapshot(records, 5)
        self.assertEqual(records["upid"][0], 21)
        self.assertEqual(records["pid"][0], 21)

    def test_pid_fallback_mid_chain(self):
        records = make_fixed(
            [
                {"id": 30, "snap": 5, "pid": 31, "upid": 31},
                {"id": 31, "snap": 5, "pid": 32, "upid": 888},
                {"id": 32, "snap": 5},
            ]
        )
        fix_upid_snapshot(records, 5)
        np.testing.assert_array_equal(records["upid"], [32, 32, 32])

    def test_unresolved_target_aborts(self):
        records = make_fixed(
            [{"id": 70, "snap": 5, "pid": 888, "upid": 999}, {"id": 71, "snap": 5}]
        )
        with self.assertRaisesRegex(
            ConverterError, r"1 satellite.*origin id=70, at id=70, upid=999, pid=888"
        ):
            fix_upid_snapshot(records, 5)

    def test_mid_chain_unresolved_reports_failing_hop(self):
        # origin 30 fails at hop 31, whose own upid/pid targets are absent;
        # the message must name both the origin and the failing hop
        records = make_fixed(
            [
                {"id": 30, "snap": 5, "pid": 31, "upid": 31},
                {"id": 31, "snap": 5, "pid": 999, "upid": 888},
            ]
        )
        with self.assertRaisesRegex(
            ConverterError, r"2 satellite.*origin id=30, at id=31, upid=888, pid=999"
        ):
            fix_upid_snapshot(records, 5)

    def test_descending_id_chain_uses_reference_path_compression(self):
        # 40 -> 39 -> ... -> 1 -> central: ascending-id processing rewrites
        # low-id satellites first, so each later chain resolves in two
        # lookups — the reference accepts this even though the original
        # chain is longer than the per-satellite lookup limit
        rows = [{"id": 1000, "snap": 5}, {"id": 1, "snap": 5, "pid": 1000, "upid": 1000}]
        for k in range(2, 41):
            rows.append({"id": k, "snap": 5, "pid": k - 1, "upid": k - 1})
        records = make_fixed(rows)
        fix_upid_snapshot(records, 5)
        satellites = records["id"] != 1000
        np.testing.assert_array_equal(records["upid"][satellites], 1000)
        np.testing.assert_array_equal(records["pid"][satellites], 1000)

    def test_cyclic_chain_hits_depth_limit(self):
        records = make_fixed(
            [
                {"id": 40, "snap": 4, "pid": 41, "upid": 41},
                {"id": 41, "snap": 4, "pid": 40, "upid": 40},
            ]
        )
        with self.assertRaisesRegex(ConverterError, "exceeds depth 30"):
            fix_upid_snapshot(records, 4)

    @staticmethod
    def _chain(n_satellites: int):
        """s_1 -> s_2 -> ... -> s_N -> central 1000; s_1 needs N lookups."""
        rows = [{"id": 1000, "snap": 5}]
        for k in range(1, n_satellites + 1):
            target = k + 1 if k < n_satellites else 1000
            rows.append({"id": k, "snap": 5, "pid": target, "upid": target})
        return make_fixed(rows)

    def test_depth_limit_boundary_matches_reference(self):
        # find_fof_halo enters with calldepth 0..30 inclusive: 31 lookups pass
        records = self._chain(31)
        fix_upid_snapshot(records, 5)
        satellites = records["id"] != 1000
        np.testing.assert_array_equal(records["upid"][satellites], 1000)
        np.testing.assert_array_equal(records["pid"][satellites], 1000)
        with self.assertRaisesRegex(ConverterError, "exceeds depth 30"):
            fix_upid_snapshot(self._chain(32), 5)

    def test_cross_forest_upid_falls_back_to_pid(self):
        # upid points at a same-id halo in ANOTHER forest — invisible to the
        # reference per-forest resolution, so the pid fallback must apply
        records = make_fixed(
            [
                {"id": 50, "snap": 5, "pid": 51, "upid": 60, "forest_id": 1},
                {"id": 51, "snap": 5, "forest_id": 1},
                {"id": 60, "snap": 5, "forest_id": 2},
            ]
        )
        fix_upid_snapshot(records, 5)
        self.assertEqual(records["upid"][0], 51)
        self.assertEqual(records["pid"][0], 51)
        self.assertEqual(records["pid"][2], -1)  # other forest untouched

    def test_cross_forest_upid_and_pid_abort(self):
        records = make_fixed(
            [
                {"id": 50, "snap": 5, "pid": 60, "upid": 60, "forest_id": 1},
                {"id": 60, "snap": 5, "forest_id": 2},
            ]
        )
        with self.assertRaisesRegex(ConverterError, "neither target present within the forest"):
            fix_upid_snapshot(records, 5)


def make_sorted_workdir(root: Path, forests=None):
    """scatter + sort on the synthetic fixtures; returns (workdir, paths)."""
    forests = forests if forests is not None else fixtures.standard_forests()
    tree_file = fixtures.write_ctrees_file(root / "tree_0.dat", fixtures.all_trees(forests))
    forests_list = fixtures.write_forests_list(root / "forests.list", forests)
    a_list = fixtures.write_a_list(root / "test.a_list")
    sim_info = fixtures.write_simulation_info(root / "simulation_info.yaml")
    workdir = root / "workdir"
    run_scatter(
        tree_files=[tree_file],
        forests_list_path=forests_list,
        a_list_path=a_list,
        workdir=workdir,
        simulation_info_path=sim_info,
    )
    run_sort(workdir)
    return workdir, a_list, sim_info


#: hand-computed golden post-fix values for standard_forests():
#: id -> (snap, upid, pid, MostBoundID, Len). Len literals were derived
#: independently ("%.5e" text -> float64 -> float32, then Decimal arithmetic
#: for round-half-away of Mvir*1e-10/0.0325). Forest 100 has two pid==-1
#: centrals at its max snapshot 5: survivor 1010 (1e12 > 8e11), demoted 1020.
GOLDEN = {
    1010: (5, 1010, -1, 1010, 3077),
    1020: (5, 1010, 1010, -1020, 2462),
    2010: (5, 2010, -1, 2010, 1538),
    2011: (5, 2010, 2010, 2011, 308),
    5010: (5, 5010, -1, 5010, 769),
    5011: (5, 5010, 5010, 5011, 0),
    6010: (5, 6010, -1, 6010, 6154),
    6011: (5, 6010, 6010, 6011, 615),
    6012: (5, 6010, 6010, 6012, 154),
    1011: (4, 1011, -1, 1011, 1846),
    1012: (4, 1012, -1, 1012, 1846),
    1021: (4, 1021, -1, 1021, 2154),
    2012: (4, 2012, -1, 2012, 1231),
    2013: (4, 2012, 2012, 2013, 277),
    1013: (3, 1013, -1, 1013, 1538),
    4010: (2, 4010, -1, 4010, 923),
    4011: (1, 4011, -1, 4011, 615),
}


class TestFixupsPipeline(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def test_golden_fixture_end_to_end(self):
        workdir, a_list, sim_info = make_sorted_workdir(self.root)
        scratch = workdir / "scratch"
        # raw sorted values, captured before fixups for the spin comparison
        raw = {
            int(s): np.fromfile(scratch / sorted_scratch_name(int(s)), dtype=RECORD_DTYPE)
            for s in Manifest.load_or_create(workdir).data["snapshots"]
        }
        manifest = run_fixups(workdir, a_list_path=a_list, simulation_info_path=sim_info)

        seen = {}
        for snap_str, entry in manifest.data["snapshots"].items():
            snap = int(snap_str)
            self.assertEqual(entry["status"], "fixed")
            fixed = np.fromfile(entry["fixed_file"], dtype=FIXED_RECORD_DTYPE)
            self.assertEqual(len(fixed), entry["rows"])
            tag = manifest.data["intermediates"][entry["fixed_file"]]["dtype_tag"]
            self.assertEqual(tag, FIXED_DTYPE_TAG)
            for i in range(len(fixed)):
                seen[int(fixed["id"][i])] = (
                    snap,
                    int(fixed["upid"][i]),
                    int(fixed["pid"][i]),
                    int(fixed["MostBoundID"][i]),
                    int(fixed["Len"][i]),
                )
            # spin: reference formula applied to the raw sorted values
            raw_snap = raw[snap]
            self.assertTrue(np.array_equal(raw_snap["id"], fixed["id"]))
            nonzero = raw_snap["Mvir"] != np.float32(0.0)
            inv = 1.0 / raw_snap["Mvir"][nonzero].astype(np.float64)
            for k in ("Jx", "Jy", "Jz"):
                expected = (raw_snap[k][nonzero].astype(np.float64) * inv).astype(np.float32)
                np.testing.assert_array_equal(fixed[k][nonzero], expected)
                np.testing.assert_array_equal(fixed[k][~nonzero], raw_snap[k][~nonzero])
        self.assertEqual(seen, GOLDEN)
        self.assertEqual(manifest.data["snapshots"]["5"]["flyby_demotions"], 1)
        self.assertEqual(manifest.data["snapshots"]["5"]["len_zero_count"], 1)
        self.assertEqual(manifest.data["snapshots"]["4"]["flyby_demotions"], 0)

    def test_fixed_dtype_is_frozen_superset(self):
        self.assertEqual(FIXED_RECORD_DTYPE.itemsize, 120)
        self.assertEqual(FIXED_RECORD_DTYPE.names, RECORD_DTYPE.names + ("Len", "MostBoundID"))
        for name in RECORD_DTYPE.names:
            self.assertEqual(FIXED_RECORD_DTYPE.fields[name][0], RECORD_DTYPE.fields[name][0])

    def test_early_dying_flyby_demoted_at_forest_max(self):
        # forest 700 dies at snapshot 2 with two centrals there: fix_flybys
        # must demote at the FOREST'S max snapshot, not the global final one
        forests = fixtures.standard_forests() + [fixtures.early_dying_flyby_forest()]
        workdir, a_list, sim_info = make_sorted_workdir(self.root, forests=forests)
        manifest = run_fixups(workdir, a_list_path=a_list, simulation_info_path=sim_info)
        self.assertEqual(manifest.data["snapshots"]["2"]["flyby_demotions"], 1)
        fixed = np.fromfile(manifest.data["snapshots"]["2"]["fixed_file"], dtype=FIXED_RECORD_DTYPE)
        by_id = {int(fixed["id"][i]): i for i in range(len(fixed))}
        survivor, demoted = by_id[7010], by_id[7011]
        self.assertEqual(fixed["pid"][survivor], -1)
        self.assertEqual(fixed["upid"][survivor], 7010)
        self.assertEqual(fixed["MostBoundID"][survivor], 7010)
        self.assertEqual(fixed["upid"][demoted], 7010)
        self.assertEqual(fixed["pid"][demoted], 7010)
        self.assertEqual(fixed["MostBoundID"][demoted], -7011)

    def test_zero_central_forest_aborts(self):
        forests = fixtures.standard_forests() + [fixtures.zero_central_forest()]
        workdir, a_list, sim_info = make_sorted_workdir(self.root, forests=forests)
        with self.assertRaisesRegex(ConverterError, "zero pid == -1 centrals"):
            run_fixups(workdir, a_list_path=a_list, simulation_info_path=sim_info)

    def test_rerun_is_idempotent(self):
        workdir, a_list, sim_info = make_sorted_workdir(self.root)
        run_fixups(workdir, a_list_path=a_list, simulation_info_path=sim_info)
        manifest = run_fixups(workdir, a_list_path=a_list, simulation_info_path=sim_info)
        for entry in manifest.data["snapshots"].values():
            self.assertEqual(entry["status"], "fixed")

    def test_tampered_fixed_file_detected_on_rerun(self):
        workdir, a_list, sim_info = make_sorted_workdir(self.root)
        manifest = run_fixups(workdir, a_list_path=a_list, simulation_info_path=sim_info)
        snap = sorted(manifest.data["snapshots"], key=int)[0]
        target = Path(manifest.data["snapshots"][snap]["fixed_file"])
        data = bytearray(target.read_bytes())
        data[-1] ^= 0xFF
        target.write_bytes(bytes(data))
        with self.assertRaisesRegex(ConverterError, "checksum"):
            run_fixups(workdir, a_list_path=a_list, simulation_info_path=sim_info)

    def test_wrong_a_list_content_aborts(self):
        workdir, _, sim_info = make_sorted_workdir(self.root)
        other = fixtures.write_a_list(self.root / "other.a_list", [0.1, 0.2, 0.3])
        with self.assertRaisesRegex(ConverterError, "a_list content md5"):
            run_fixups(workdir, a_list_path=other, simulation_info_path=sim_info)

    def test_changed_simulation_info_aborts(self):
        workdir, a_list, sim_info = make_sorted_workdir(self.root)
        run_fixups(workdir, a_list_path=a_list, simulation_info_path=sim_info)
        changed = self.root / "changed_info.yaml"
        changed.write_text("simulation:\n  particle_mass: {value: 0.05, units: 1e10 Msun/h}\n")
        with self.assertRaisesRegex(ConverterError, "simulation_info content md5"):
            run_fixups(workdir, a_list_path=a_list, simulation_info_path=changed)

    def test_fixups_before_sort_aborts(self):
        forests = fixtures.standard_forests()
        tree_file = fixtures.write_ctrees_file(
            self.root / "tree_0.dat", fixtures.all_trees(forests)
        )
        forests_list = fixtures.write_forests_list(self.root / "forests.list", forests)
        a_list = fixtures.write_a_list(self.root / "test.a_list")
        sim_info = fixtures.write_simulation_info(self.root / "simulation_info.yaml")
        workdir = self.root / "workdir"
        run_scatter(
            tree_files=[tree_file],
            forests_list_path=forests_list,
            a_list_path=a_list,
            workdir=workdir,
            simulation_info_path=sim_info,
        )
        with self.assertRaisesRegex(ConverterError, "run sort first"):
            run_fixups(workdir, a_list_path=a_list, simulation_info_path=sim_info)

    def test_fixups_without_manifest_aborts(self):
        workdir = self.root / "empty"
        workdir.mkdir()
        a_list = fixtures.write_a_list(self.root / "test.a_list")
        sim_info = fixtures.write_simulation_info(self.root / "simulation_info.yaml")
        with self.assertRaisesRegex(ConverterError, "no manifest"):
            run_fixups(workdir, a_list_path=a_list, simulation_info_path=sim_info)

    def test_scatter_rerun_after_fixups_is_idempotent(self):
        # the 'fixed' snapshot status must survive an upstream re-run
        workdir, a_list, sim_info = make_sorted_workdir(self.root)
        run_fixups(workdir, a_list_path=a_list, simulation_info_path=sim_info)
        run_scatter(
            tree_files=[self.root / "tree_0.dat"],
            forests_list_path=self.root / "forests.list",
            a_list_path=a_list,
            workdir=workdir,
            simulation_info_path=sim_info,
        )
        run_sort(workdir)
        manifest = run_fixups(workdir, a_list_path=a_list, simulation_info_path=sim_info)
        for entry in manifest.data["snapshots"].values():
            self.assertEqual(entry["status"], "fixed")

    def test_scatter_rerun_with_changed_simulation_info_aborts(self):
        # simulation_info identity is immutable once recorded: a metadata
        # swap after fix-ups must refuse to resume, never mix particle masses
        workdir, a_list, sim_info = make_sorted_workdir(self.root)
        run_fixups(workdir, a_list_path=a_list, simulation_info_path=sim_info)
        changed = self.root / "changed_info.yaml"
        changed.write_text("simulation:\n  particle_mass: {value: 0.05, units: 1e10 Msun/h}\n")
        with self.assertRaisesRegex(ConverterError, "simulation_info content changed"):
            run_scatter(
                tree_files=[self.root / "tree_0.dat"],
                forests_list_path=self.root / "forests.list",
                a_list_path=a_list,
                workdir=workdir,
                simulation_info_path=changed,
            )

    def test_mostboundid_invariant_reports_count_and_examples(self):
        records = make_fixed(
            [
                {"id": 1, "snap": 5},
                {"id": 2, "snap": 5, "MostBoundID": 99},
                {"id": 3, "snap": 5, "MostBoundID": -98},
            ]
        )
        from fixups import verify_mostboundid_invariant

        verify_mostboundid_invariant(make_fixed([{"id": 7, "snap": 5, "MostBoundID": -7}]), "ok")
        with self.assertRaisesRegex(
            ConverterError, r"2 halo\(s\) violate.*id=2, MostBoundID=99.*id=3, MostBoundID=-98"
        ):
            verify_mostboundid_invariant(records, "test")

    def test_cli_fixups_stage(self):
        workdir, a_list, sim_info = make_sorted_workdir(self.root)
        rc = convert_ctrees.main(
            [
                "fixups",
                "--workdir",
                str(workdir),
                "--a-list",
                str(a_list),
                "--simulation-info",
                str(sim_info),
            ]
        )
        self.assertEqual(rc, 0)
        manifest = Manifest.load_or_create(workdir)
        for snap_str, entry in manifest.data["snapshots"].items():
            self.assertEqual(entry["status"], "fixed")
            self.assertTrue((workdir / "scratch" / fixed_scratch_name(int(snap_str))).exists())

    def test_cli_failure_exit_code(self):
        workdir, _, sim_info = make_sorted_workdir(self.root)
        other = fixtures.write_a_list(self.root / "other.a_list", [0.1, 0.2])
        rc = convert_ctrees.main(
            [
                "fixups",
                "--workdir",
                str(workdir),
                "--a-list",
                str(other),
                "--simulation-info",
                str(sim_info),
            ]
        )
        self.assertEqual(rc, 1)


class TestFixupsConsumesSortedScratch(unittest.TestCase):
    """Plan Slice 8 deletion table: ``sorted_N`` goes once snapshot N's
    ``fixed`` output is verified and registered — and only then, and only when
    the operator asked for it."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.addCleanup(self.tmp.cleanup)

    def _sorted_paths(self, workdir):
        manifest = Manifest.load_or_create(workdir)
        return {
            int(snap): Path(entry["sorted_file"])
            for snap, entry in manifest.data["snapshots"].items()
        }

    def test_flag_off_retains_every_sorted_file(self):
        workdir, a_list, sim_info = make_sorted_workdir(self.root)
        before = self._sorted_paths(workdir)
        run_fixups(workdir, a_list_path=a_list, simulation_info_path=sim_info)
        manifest = Manifest.load_or_create(workdir)
        for snap, path in before.items():
            self.assertTrue(path.exists(), "snapshot {} sorted file was deleted".format(snap))
            self.assertEqual(
                "present", manifest.data["intermediates"][str(path.resolve())]["status"]
            )

    def test_flag_on_consumes_every_sorted_file(self):
        workdir, a_list, sim_info = make_sorted_workdir(self.root)
        before = self._sorted_paths(workdir)
        with capture_stderr() as captured:
            run_fixups(
                workdir,
                a_list_path=a_list,
                simulation_info_path=sim_info,
                consume_intermediates=True,
            )
        manifest = Manifest.load_or_create(workdir)
        for snap, path in before.items():
            self.assertFalse(path.exists(), "snapshot {} sorted file survived".format(snap))
            self.assertEqual(
                "removed", manifest.data["intermediates"][str(path.resolve())]["status"]
            )
            self.assertIn("fixups: snapshot {} — consumed {}".format(snap, path), captured.text)

    def test_fixed_output_is_registered_before_its_predecessor_goes(self):
        """The protocol's ordering, asserted rather than assumed: at the
        instant the sorted file is unlinked the fixed successor must already be
        registered present in the manifest ON DISK."""
        workdir, a_list, sim_info = make_sorted_workdir(self.root)
        manifest_path = Path(workdir) / "manifest.json"
        observed = {}
        real_remove = Manifest.remove_intermediate

        def spy(self, path):
            import json

            saved = json.loads(manifest_path.read_text())
            snap = str(Path(path).name).split("_")[1]
            entry = saved["snapshots"][str(int(snap))]
            observed[int(snap)] = (
                entry.get("status"),
                saved["intermediates"].get(entry.get("fixed_file", ""), {}).get("status"),
            )
            return real_remove(self, path)

        with mock.patch.object(Manifest, "remove_intermediate", spy):
            run_fixups(
                workdir,
                a_list_path=a_list,
                simulation_info_path=sim_info,
                consume_intermediates=True,
            )
        self.assertTrue(observed)
        for snap, (status, fixed_status) in observed.items():
            self.assertEqual("fixed", status, "snapshot {}".format(snap))
            self.assertEqual("present", fixed_status, "snapshot {}".format(snap))

    def test_deletion_goes_through_remove_intermediate(self):
        """Never a bare unlink: with the manifest's guarded removal disabled,
        no sorted file may disappear."""
        workdir, a_list, sim_info = make_sorted_workdir(self.root)
        before = self._sorted_paths(workdir)
        with mock.patch.object(Manifest, "remove_intermediate", lambda self, path: None):
            run_fixups(
                workdir,
                a_list_path=a_list,
                simulation_info_path=sim_info,
                consume_intermediates=True,
            )
        for path in before.values():
            self.assertTrue(path.exists())

    def test_crash_between_unlink_and_save_converges_to_removed(self):
        """A crash after the unlink and before the manifest save leaves the
        entry recorded ``present`` with no file. The next run must converge on
        ``removed`` — with the flag in EITHER state, because the bytes are
        already gone — and must not raise."""
        for delete in (False, True):
            with self.subTest(consume_intermediates=delete):
                root = self.root / "crash-{}".format(int(delete))
                root.mkdir()
                workdir, a_list, sim_info = make_sorted_workdir(root)
                run_fixups(workdir, a_list_path=a_list, simulation_info_path=sim_info)
                manifest = Manifest.load_or_create(workdir)
                victim = Path(manifest.data["snapshots"]["5"]["sorted_file"])
                victim.unlink()  # the unlink landed; the save did not
                self.assertEqual(
                    "present", manifest.data["intermediates"][str(victim.resolve())]["status"]
                )
                run_fixups(
                    workdir,
                    a_list_path=a_list,
                    simulation_info_path=sim_info,
                    consume_intermediates=delete,
                )
                reloaded = Manifest.load_or_create(workdir)
                self.assertEqual(
                    "removed", reloaded.data["intermediates"][str(victim.resolve())]["status"]
                )

    def test_finalize_and_scatter_reruns_skip_a_consumed_sorted_file(self):
        """Deletion is bounded by re-run reachability, and ``sort`` is not the
        only path that skip-trusts the sorted file and the index.

        ``_finalize_scatter`` verifies both when it meets a snapshot already
        past ``concatenated``, so once ``fixups`` has consumed the sorted files
        a re-run of ``finalize`` — or of a non-batch ``scatter``, which
        finalizes at the end — must skip and name what was consumed, exactly as
        sort does, rather than refuse on a file the pipeline deliberately
        deleted. Both entry points are exercised because they reach the same
        two branches by different routes.
        """
        root = self.root / "finalize-rerun"
        root.mkdir()
        forests = fixtures.standard_forests()
        tree_file = fixtures.write_ctrees_file(root / "tree_0.dat", fixtures.all_trees(forests))
        forests_list = fixtures.write_forests_list(root / "forests.list", forests)
        a_list = fixtures.write_a_list(root / "test.a_list")
        sim_info = fixtures.write_simulation_info(root / "simulation_info.yaml")
        workdir = root / "workdir"
        run_scatter(
            tree_files=[tree_file],
            forests_list_path=forests_list,
            a_list_path=a_list,
            workdir=workdir,
            simulation_info_path=sim_info,
        )
        run_sort(workdir)
        run_fixups(
            workdir,
            a_list_path=a_list,
            simulation_info_path=sim_info,
            consume_intermediates=True,
        )
        sorted_paths = self._sorted_paths(workdir)
        self.assertTrue(sorted_paths)
        for path in sorted_paths.values():
            self.assertFalse(path.exists())

        with capture_stderr() as captured:
            run_finalize(workdir, forests_list)
        for snap, path in sorted_paths.items():
            self.assertIn(
                "finalize: snapshot {} is already past concat".format(snap), captured.text
            )
            self.assertIn(str(path), captured.text)

        # the same two branches through the other entry point: a non-batch
        # scatter re-run finalizes at the end
        with capture_stderr() as captured:
            run_scatter(
                tree_files=[tree_file],
                forests_list_path=forests_list,
                a_list_path=a_list,
                workdir=workdir,
                simulation_info_path=sim_info,
            )
        for snap in sorted_paths:
            self.assertIn(
                "finalize: snapshot {} is already past concat".format(snap), captured.text
            )

        # neither re-run resurrected or re-registered anything
        manifest = Manifest.load_or_create(workdir)
        for path in sorted_paths.values():
            self.assertFalse(path.exists())
            self.assertEqual(
                "removed", manifest.data["intermediates"][str(path.resolve())]["status"]
            )

    def test_finalize_rerun_still_refuses_a_merely_missing_sorted_file(self):
        """The skip is granted only to a RECORDED consumption here too: a
        sorted file that simply vanished is still refused by finalize."""
        root = self.root / "finalize-missing"
        root.mkdir()
        forests = fixtures.standard_forests()
        tree_file = fixtures.write_ctrees_file(root / "tree_0.dat", fixtures.all_trees(forests))
        forests_list = fixtures.write_forests_list(root / "forests.list", forests)
        a_list = fixtures.write_a_list(root / "test.a_list")
        sim_info = fixtures.write_simulation_info(root / "simulation_info.yaml")
        workdir = root / "workdir"
        run_scatter(
            tree_files=[tree_file],
            forests_list_path=forests_list,
            a_list_path=a_list,
            workdir=workdir,
            simulation_info_path=sim_info,
        )
        run_sort(workdir)
        run_fixups(workdir, a_list_path=a_list, simulation_info_path=sim_info)
        next(iter(self._sorted_paths(workdir).values())).unlink()
        with self.assertRaisesRegex(ConverterError, "missing on disk"):
            run_finalize(workdir, forests_list)

    def test_cli_flag_is_off_by_default(self):
        parser = convert_ctrees.build_arg_parser()
        args = parser.parse_args(
            ["fixups", "--workdir", "w", "--a-list", "a", "--simulation-info", "s"]
        )
        self.assertFalse(args.consume_intermediates)
        args = parser.parse_args(
            [
                "fixups",
                "--workdir",
                "w",
                "--a-list",
                "a",
                "--simulation-info",
                "s",
                "--consume-intermediates",
            ]
        )
        self.assertTrue(args.consume_intermediates)


if __name__ == "__main__":
    unittest.main()
