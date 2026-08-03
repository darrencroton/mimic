"""Slice 6 unit tests: reference order, FoF chains, descendant merge-join,
progenitor insertion semantics, rank pass, identity assertions, and the link
pipeline stage with a hand-computed golden fixture."""

import os
import sys
import tempfile
import unittest
from pathlib import Path

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import convert_ctrees  # noqa: E402
import fixtures  # noqa: E402
from ctrees_parser import ConverterError  # noqa: E402
from fixups import run_fixups  # noqa: E402
from links import (  # noqa: E402
    LINKS_DTYPE_TAG,
    LINKS_RECORD_DTYPE,
    build_descendants,
    build_fof_chains,
    build_progenitor_links,
    pending_fp_name,
    reference_order,
    run_links,
    validate_slab,
    verify_identity,
)
from scatter import Manifest, run_scatter  # noqa: E402
from sort_index import run_sort  # noqa: E402
from test_fixups import make_fixed  # noqa: E402


def make_linkable(rows):
    """Build an id-sorted FIXED_RECORD_DTYPE array for link-stage unit tests.

    Post-fix invariants the link stage expects: centrals (``pid == -1``) carry
    ``upid == id``, which ``make_fixed`` does not apply — apply it here.
    """
    records = make_fixed(rows)
    central = records["pid"] == -1
    records["upid"][central] = records["id"][central]
    return records


class TestLinksDtype(unittest.TestCase):
    def test_frozen_layout(self):
        self.assertEqual(LINKS_RECORD_DTYPE.itemsize, 36)
        self.assertEqual(
            LINKS_RECORD_DTYPE.names,
            (
                "Descendant",
                "FirstProgenitor",
                "NextProgenitor",
                "FirstHaloInFOFgroup",
                "NextHaloInFOFgroup",
                "ForestIndex",
                "HaloRankInForest",
            ),
        )
        for name in ("Descendant", "NextHaloInFOFgroup"):
            self.assertEqual(LINKS_RECORD_DTYPE.fields[name][0].str, "<i4")
        for name in ("ForestIndex", "HaloRankInForest"):
            self.assertEqual(LINKS_RECORD_DTYPE.fields[name][0].str, "<i8")
        self.assertIn("itemsize=36", LINKS_DTYPE_TAG)


class TestValidateSlab(unittest.TestCase):
    def test_accepts_post_fix_records(self):
        records = make_linkable(
            [
                {"id": 10, "snap": 5, "forest_id": 1},
                {"id": 20, "snap": 5, "pid": 10, "upid": 10, "forest_id": 1},
            ]
        )
        validate_slab(records, 5, "test")

    def test_non_ascending_ids_abort(self):
        records = make_linkable(
            [{"id": 10, "snap": 5, "forest_id": 1}, {"id": 20, "snap": 5, "forest_id": 1}]
        )
        records = records[::-1].copy()
        with self.assertRaisesRegex(ConverterError, "not strictly ascending"):
            validate_slab(records, 5, "test")

    def test_central_upid_mismatch_aborts(self):
        records = make_fixed([{"id": 10, "snap": 5, "upid": 99, "forest_id": 1}])
        with self.assertRaisesRegex(ConverterError, "upid != id"):
            validate_slab(records, 5, "test")


class TestReferenceOrder(unittest.TestCase):
    def test_encounter_order_is_upid_pid_id_not_slab_order(self):
        # slab (ascending id): 8010 (satellite of 8050), 8020, 8050
        # encounter (upid, pid, id): 8020 group, then 8050's central, then sat
        records = make_linkable(
            [
                {"id": 8010, "snap": 4, "pid": 8050, "upid": 8050, "forest_id": 8},
                {"id": 8020, "snap": 4, "forest_id": 8},
                {"id": 8050, "snap": 4, "forest_id": 8},
            ]
        )
        order = reference_order(records)
        self.assertEqual(records["id"][order].tolist(), [8020, 8050, 8010])


class TestBuildFofChains(unittest.TestCase):
    def test_chains_follow_reference_order(self):
        records = make_linkable(
            [
                {"id": 10, "snap": 5, "forest_id": 1},
                {"id": 20, "snap": 5, "pid": 10, "upid": 10, "forest_id": 1},
                {"id": 30, "snap": 5, "pid": 10, "upid": 10, "forest_id": 1},
                {"id": 40, "snap": 5, "forest_id": 2},
            ]
        )
        first_fof, next_fof = build_fof_chains(records, reference_order(records), 5, "test")
        # slab: 10->0, 20->1, 30->2, 40->3; group 10 chains 0->1->2
        self.assertEqual(first_fof.tolist(), [0, 0, 0, 3])
        self.assertEqual(next_fof.tolist(), [1, 2, -1, -1])

    def test_group_without_central_aborts(self):
        # satellite resolved to upid 30, but no halo 30 in this snapshot
        records = make_linkable(
            [
                {"id": 10, "snap": 5, "forest_id": 1},
                {"id": 20, "snap": 5, "pid": 30, "upid": 30, "forest_id": 1},
            ]
        )
        with self.assertRaisesRegex(ConverterError, "is not the group's central"):
            build_fof_chains(records, reference_order(records), 5, "test")

    def test_cross_forest_member_aborts(self):
        records = make_linkable(
            [
                {"id": 10, "snap": 5, "forest_id": 1},
                {"id": 20, "snap": 5, "pid": 10, "upid": 10, "forest_id": 2},
            ]
        )
        with self.assertRaisesRegex(ConverterError, "different forest"):
            build_fof_chains(records, reference_order(records), 5, "test")


class TestBuildDescendants(unittest.TestCase):
    def test_merge_join_positions(self):
        records = make_linkable(
            [
                {"id": 10, "snap": 4, "desc_id": 100, "forest_id": 1},
                {"id": 20, "snap": 4, "forest_id": 1},
                {"id": 30, "snap": 4, "desc_id": 300, "forest_id": 1},
            ]
        )
        desc = build_descendants(records, np.asarray([100, 300], dtype=np.int64), 4, "test")
        self.assertEqual(desc.tolist(), [0, -1, 1])

    def test_dangling_desc_id_aborts(self):
        records = make_linkable([{"id": 10, "snap": 4, "desc_id": 999, "forest_id": 1}])
        with self.assertRaisesRegex(ConverterError, "no target halo at snapshot 5"):
            build_descendants(records, np.asarray([100, 300], dtype=np.int64), 4, "test")

    def test_descendant_into_empty_snapshot_aborts(self):
        records = make_linkable([{"id": 10, "snap": 4, "desc_id": 100, "forest_id": 1}])
        with self.assertRaisesRegex(ConverterError, "no target halo"):
            build_descendants(records, np.empty(0, dtype=np.int64), 4, "test")


def _progenitor_case(specs):
    """Run build_progenitor_links on centrals with given (id, Mvir) merging
    into one descendant; returns (first_prog_id, chain_ids_from_first)."""
    records = make_linkable(
        [{"id": i, "snap": 4, "desc_id": 100, "Mvir": m, "forest_id": 1} for i, m in specs]
    )
    order = reference_order(records)
    desc = build_descendants(records, np.asarray([100], dtype=np.int64), 4, "test")
    next_prog, pending_fp = build_progenitor_links(records, order, desc, 1)
    chain = []
    cursor = int(pending_fp[0])
    while cursor != -1:
        chain.append(int(records["id"][cursor]))
        cursor = int(next_prog[cursor])
    return chain


class TestProgenitorInsertion(unittest.TestCase):
    """The reference insertion loop (ctrees_utils.c:667-706), literally."""

    def test_single_progenitor(self):
        self.assertEqual(_progenitor_case([(10, 5.0e11)]), [10])

    def test_mass_tie_keeps_first_encountered_in_front(self):
        # strict > only: the tie never promotes
        self.assertEqual(_progenitor_case([(10, 5.0e11), (20, 5.0e11)]), [10, 20])

    def test_promote_then_append(self):
        # masses 3,7,5: 7 promotes over 3; 5 appends at the tail (after 3)
        self.assertEqual(_progenitor_case([(10, 3.0e11), (20, 7.0e11), (30, 5.0e11)]), [20, 10, 30])

    def test_ascending_masses_reverse_the_chain(self):
        # every progenitor promotes: the final chain is fully reversed —
        # NOT "max-Mvir first with the remainder in encounter order",
        # which would give [30, 10, 20]
        self.assertEqual(_progenitor_case([(10, 3.0e11), (20, 4.0e11), (30, 5.0e11)]), [30, 20, 10])

    def test_encounter_order_is_not_slab_order(self):
        # equal masses, so the chain is pure encounter order. Slab order is
        # [8010, 8020, 8050]; encounter order is (upid, pid, id):
        # 8020 (its own group), 8050 (central of group 8050), 8010 (satellite)
        records = make_linkable(
            [
                {
                    "id": 8010,
                    "snap": 4,
                    "pid": 8050,
                    "upid": 8050,
                    "desc_id": 100,
                    "Mvir": 5.0e11,
                    "forest_id": 8,
                },
                {"id": 8020, "snap": 4, "desc_id": 100, "Mvir": 5.0e11, "forest_id": 8},
                {"id": 8050, "snap": 4, "desc_id": 100, "Mvir": 5.0e11, "forest_id": 8},
            ]
        )
        order = reference_order(records)
        desc = build_descendants(records, np.asarray([100], dtype=np.int64), 4, "test")
        next_prog, pending_fp = build_progenitor_links(records, order, desc, 1)
        chain = []
        cursor = int(pending_fp[0])
        while cursor != -1:
            chain.append(int(records["id"][cursor]))
            cursor = int(next_prog[cursor])
        self.assertEqual(chain, [8020, 8050, 8010])


class TestVerifyIdentity(unittest.TestCase):
    def test_valid_identity_passes(self):
        verify_identity(
            np.asarray([0, 0, 1], dtype=np.int64), np.asarray([0, 1, 0], dtype=np.int64), 2, "test"
        )

    def test_duplicate_pair_aborts(self):
        with self.assertRaisesRegex(ConverterError, "density/uniqueness"):
            verify_identity(
                np.asarray([0, 0], dtype=np.int64), np.asarray([0, 0], dtype=np.int64), 1, "test"
            )

    def test_sparse_forest_index_aborts(self):
        with self.assertRaisesRegex(ConverterError, "not dense"):
            verify_identity(
                np.asarray([0, 2], dtype=np.int64), np.asarray([0, 0], dtype=np.int64), 3, "test"
            )

    def test_non_dense_ranks_abort(self):
        with self.assertRaisesRegex(ConverterError, "density/uniqueness"):
            verify_identity(
                np.asarray([0, 0], dtype=np.int64), np.asarray([0, 2], dtype=np.int64), 1, "test"
            )


def make_linked_workdir(root: Path, forests=None):
    """scatter + sort + fixups on the synthetic fixtures; returns
    (workdir, a_list_path, sim_info_path)."""
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
    run_fixups(workdir, a_list_path=a_list, simulation_info_path=sim_info)
    return workdir, a_list, sim_info


#: hand-computed golden links for standard_forests(), per snapshot and slab row.
#: Slab orders (ascending id): snap 1 [4011]; snap 2 [4010]; snap 3 [1013];
#: snap 4 [1011, 1012, 1021, 2012, 2013];
#: snap 5 [1010, 1020, 2010, 2011, 5010, 5011, 6010, 6011, 6012].
#: ForestIndex (ascending forest id 100,200,400,500,600 -> 0,1,2,3,4).
#: Covers: the 1011/1012 mass tie (first encountered stays FirstProgenitor),
#: the flyby-demoted 1020 chained behind 1010, and the pending buffer flowing
#: across three consecutive snapshots (1013@3 -> 1011@4 -> 1010@5).
GOLDEN_LINKS = {
    1: {
        "Descendant": [0],
        "FirstProgenitor": [-1],
        "NextProgenitor": [-1],
        "FirstHaloInFOFgroup": [0],
        "NextHaloInFOFgroup": [-1],
        "ForestIndex": [2],
        "HaloRankInForest": [1],
    },
    2: {
        "Descendant": [-1],
        "FirstProgenitor": [0],
        "NextProgenitor": [-1],
        "FirstHaloInFOFgroup": [0],
        "NextHaloInFOFgroup": [-1],
        "ForestIndex": [2],
        "HaloRankInForest": [0],
    },
    3: {
        "Descendant": [0],
        "FirstProgenitor": [-1],
        "NextProgenitor": [-1],
        "FirstHaloInFOFgroup": [0],
        "NextHaloInFOFgroup": [-1],
        "ForestIndex": [0],
        "HaloRankInForest": [5],
    },
    4: {
        "Descendant": [0, 0, 1, 2, 3],
        "FirstProgenitor": [0, -1, -1, -1, -1],
        "NextProgenitor": [1, -1, -1, -1, -1],
        "FirstHaloInFOFgroup": [0, 1, 2, 3, 3],
        "NextHaloInFOFgroup": [-1, -1, -1, 4, -1],
        "ForestIndex": [0, 0, 0, 1, 1],
        "HaloRankInForest": [2, 3, 4, 2, 3],
    },
    5: {
        "Descendant": [-1] * 9,
        "FirstProgenitor": [0, 2, 3, 4, -1, -1, -1, -1, -1],
        "NextProgenitor": [-1] * 9,
        "FirstHaloInFOFgroup": [0, 0, 2, 2, 4, 4, 6, 6, 6],
        "NextHaloInFOFgroup": [1, -1, 3, -1, 5, -1, 7, 8, -1],
        "ForestIndex": [0, 0, 1, 1, 3, 3, 4, 4, 4],
        "HaloRankInForest": [0, 1, 0, 1, 0, 1, 0, 1, 2],
    },
}


class TestLinksPipeline(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def test_golden_fixture_end_to_end(self):
        workdir, _, _ = make_linked_workdir(self.root)
        manifest = run_links(workdir)
        self.assertEqual(
            manifest.data["links"], {"n_forests_total": 5, "max_halo_rank_in_forest": 5}
        )
        seen = {}
        for snap_str, entry in manifest.data["snapshots"].items():
            snap = int(snap_str)
            self.assertEqual(entry["status"], "linked")
            links = np.fromfile(entry["links_file"], dtype=LINKS_RECORD_DTYPE)
            self.assertEqual(len(links), entry["rows"])
            tag = manifest.data["intermediates"][entry["links_file"]]["dtype_tag"]
            self.assertEqual(tag, LINKS_DTYPE_TAG)
            seen[snap] = {name: links[name].tolist() for name in LINKS_RECORD_DTYPE.names}
        self.assertEqual(seen, GOLDEN_LINKS)

    def test_pending_buffers_written_and_sized(self):
        workdir, _, _ = make_linked_workdir(self.root)
        manifest = run_links(workdir)
        scratch = Path(manifest.data["snapshots"]["1"]["fixed_file"]).parent
        for snap, rows in ((2, 1), (3, 1), (4, 5), (5, 9)):
            pending = np.fromfile(scratch / pending_fp_name(snap), dtype=np.int32)
            self.assertEqual(len(pending), rows)
        self.assertFalse((scratch / pending_fp_name(1)).exists())
        # the three-snapshot chain: 1013@3 -> 1011@4 (slab 0) -> 1010@5 (slab 0)
        self.assertEqual(
            np.fromfile(scratch / pending_fp_name(4), dtype=np.int32).tolist(), [0, -1, -1, -1, -1]
        )
        self.assertEqual(
            np.fromfile(scratch / pending_fp_name(5), dtype=np.int32).tolist()[:2], [0, 2]
        )

    def test_rerun_verifies_and_skips(self):
        workdir, _, _ = make_linked_workdir(self.root)
        first = run_links(workdir)
        before = {
            s: Path(e["links_file"]).stat().st_mtime_ns for s, e in first.data["snapshots"].items()
        }
        again = run_links(workdir)
        after = {
            s: Path(e["links_file"]).stat().st_mtime_ns for s, e in again.data["snapshots"].items()
        }
        self.assertEqual(before, after)

    def test_cross_forest_descendant_refused(self):
        # forests.list mis-grouping: 9020's descendant 9010 exists at snap 5
        # but belongs to another forest; the reference resolves descendants
        # inside one forest's array, so this is unrepresentable there and the
        # merge-join must abort rather than silently link across forests
        tree_a = fixtures.TreeSpec(
            root_id=901, halos=[fixtures.HaloSpec(halo_id=9010, snap=5, mvir=1.0e12)]
        )
        tree_b = fixtures.TreeSpec(
            root_id=902,
            halos=[fixtures.HaloSpec(halo_id=9020, snap=4, mvir=5.0e11, desc_id=9010)],
        )
        forests = [
            fixtures.ForestSpec(forest_id=910, trees=[tree_a]),
            fixtures.ForestSpec(forest_id=920, trees=[tree_b]),
        ]
        workdir, _, _ = make_linked_workdir(self.root, forests=forests)
        with self.assertRaisesRegex(ConverterError, "crossing forest boundaries"):
            run_links(workdir)

    def test_stale_links_dtype_tag_refused_on_rerun(self):
        # a checksummed, registered links file from an older converter revision
        # must not be skip-trusted: the recorded dtype tag is part of the deal
        workdir, _, _ = make_linked_workdir(self.root)
        manifest = run_links(workdir)
        links_file = manifest.data["snapshots"]["5"]["links_file"]
        manifest.data["intermediates"][links_file]["dtype_tag"] = "ctrees-links-v0/itemsize=36/old"
        manifest.save()
        with self.assertRaisesRegex(ConverterError, "dtype tag"):
            run_links(workdir)

    def test_links_rows_metadata_mismatch_refused_on_rerun(self):
        workdir, _, _ = make_linked_workdir(self.root)
        manifest = run_links(workdir)
        links_file = manifest.data["snapshots"]["5"]["links_file"]
        manifest.data["intermediates"][links_file]["rows"] = 999
        manifest.save()
        with self.assertRaisesRegex(ConverterError, "records 999 rows"):
            run_links(workdir)

    def test_tampered_links_file_refused_on_rerun(self):
        workdir, _, _ = make_linked_workdir(self.root)
        manifest = run_links(workdir)
        links_path = Path(manifest.data["snapshots"]["5"]["links_file"])
        data = bytearray(links_path.read_bytes())
        data[0] ^= 0xFF
        links_path.write_bytes(bytes(data))
        with self.assertRaisesRegex(ConverterError, "content checksum"):
            run_links(workdir)

    def test_run_scoped_value_change_refused(self):
        workdir, _, _ = make_linked_workdir(self.root)
        run_links(workdir)
        manifest = Manifest.load_or_create(workdir)
        manifest.data["links"]["n_forests_total"] = 99
        manifest.save()
        with self.assertRaisesRegex(ConverterError, "run-scoped identity values changed"):
            run_links(workdir)

    def test_requires_fixups_first(self):
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
        run_sort(workdir)
        with self.assertRaisesRegex(ConverterError, "run fixups first"):
            run_links(workdir)

    def test_non_monotonic_observed_pairs_refused(self):
        workdir, _, _ = make_linked_workdir(self.root)
        manifest = Manifest.load_or_create(workdir)
        manifest.data["observed_pairs"] = [[1, 0.6], [2, 0.5]]
        manifest.save()
        with self.assertRaisesRegex(ConverterError, "not strictly increasing"):
            run_links(workdir)

    def test_multiple_scales_for_one_snapshot_refused(self):
        # both scales lie within the a_list tolerance of snapshot 1, but the
        # reference's scale-descending sort could split them; the rank pass
        # treats a snapshot as one slab and must refuse
        workdir, _, _ = make_linked_workdir(self.root)
        manifest = Manifest.load_or_create(workdir)
        manifest.data["observed_pairs"] = [[1, 0.59995], [1, 0.60005], [2, 0.7]]
        manifest.save()
        with self.assertRaisesRegex(ConverterError, "multiple scale factors"):
            run_links(workdir)

    def test_encounter_order_regression_end_to_end(self):
        # forest 800: three snap-4 halos merge into 8000@5; slab order
        # [8010, 8020, 8050] but encounter order [8020, 8050, 8010]; equal
        # masses keep pure encounter order in the chain
        tree = fixtures.TreeSpec(
            root_id=801,
            halos=[
                fixtures.HaloSpec(halo_id=8000, snap=5, mvir=2.0e12, num_prog=3),
                fixtures.HaloSpec(halo_id=8050, snap=4, mvir=5.0e11, desc_id=8000),
                fixtures.HaloSpec(
                    halo_id=8010, snap=4, mvir=5.0e11, desc_id=8000, pid=8050, upid=8050
                ),
                fixtures.HaloSpec(halo_id=8020, snap=4, mvir=5.0e11, desc_id=8000),
            ],
        )
        forest = fixtures.ForestSpec(forest_id=800, trees=[tree])
        workdir, _, _ = make_linked_workdir(self.root, forests=[forest])
        manifest = run_links(workdir)
        snap4 = np.fromfile(manifest.data["snapshots"]["4"]["links_file"], dtype=LINKS_RECORD_DTYPE)
        snap5 = np.fromfile(manifest.data["snapshots"]["5"]["links_file"], dtype=LINKS_RECORD_DTYPE)
        # slab: 8010->0, 8020->1, 8050->2; chain must start at 8020, not 8010
        self.assertEqual(snap5["FirstProgenitor"].tolist(), [1])
        self.assertEqual(snap4["NextProgenitor"].tolist(), [-1, 2, 0])
        # FoF at snap 4: 8020 and 8050 self-centrals, 8010 behind 8050
        self.assertEqual(snap4["FirstHaloInFOFgroup"].tolist(), [2, 1, 2])
        self.assertEqual(snap4["NextHaloInFOFgroup"].tolist(), [-1, -1, 0])
        # ranks in reference order: 8000@5 r0; then 8020, 8050, 8010 -> r1, r2, r3
        self.assertEqual(snap4["HaloRankInForest"].tolist(), [3, 1, 2])
        self.assertEqual(snap5["HaloRankInForest"].tolist(), [0])

    def test_cli_links_subcommand(self):
        workdir, _, _ = make_linked_workdir(self.root)
        rc = convert_ctrees.main(["links", "--workdir", str(workdir)])
        self.assertEqual(rc, 0)
        manifest = Manifest.load_or_create(workdir)
        statuses = {e["status"] for e in manifest.data["snapshots"].values()}
        self.assertEqual(statuses, {"linked"})

    def test_cli_links_failure_exit_code(self):
        rc = convert_ctrees.main(["links", "--workdir", str(self.root / "missing")])
        self.assertEqual(rc, 1)


if __name__ == "__main__":
    unittest.main()
