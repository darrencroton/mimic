"""Slice 6 unit tests: reference order, FoF chains, descendant merge-join,
progenitor insertion semantics, rank pass, identity assertions, and the link
pipeline stage with a hand-computed golden fixture."""

import gc
import os
import shutil
import sys
import tempfile
import tracemalloc
import unittest
from pathlib import Path
from unittest import mock

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import convert_ctrees  # noqa: E402
import fixtures  # noqa: E402
import hdf5_writer  # noqa: E402
import links  # noqa: E402
from ctrees_parser import ConverterError  # noqa: E402
from fixups import run_fixups  # noqa: E402
from hdf5_writer import run_write  # noqa: E402
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
from rank_sort import RankSortError  # noqa: E402
from scatter import Manifest, file_md5, run_scatter  # noqa: E402
from sort_index import run_sort  # noqa: E402
from test_fixups import capture_stderr, make_fixed  # noqa: E402


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


def make_linked_workdir(root: Path, forests=None, a_list_values=None):
    """scatter + sort + fixups on the synthetic fixtures; returns
    (workdir, a_list_path, sim_info_path)."""
    forests = forests if forests is not None else fixtures.standard_forests()
    a_list_values = a_list_values if a_list_values is not None else fixtures.A_LIST
    tree_file = fixtures.write_ctrees_file(
        root / "tree_0.dat", fixtures.all_trees(forests), a_list=a_list_values
    )
    forests_list = fixtures.write_forests_list(root / "forests.list", forests)
    a_list = fixtures.write_a_list(root / "test.a_list", a_list=a_list_values)
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


# ---------------------------------------------------------------------------
# Slice 5: the bounded rank/identity pass (CONVERTER-SCALE-PASS-PLAN.md)
# ---------------------------------------------------------------------------


def in_memory_identity(manifest):
    """The pre-Slice-5 in-memory formulation, transcribed from the shipped
    ``compute_identity`` at 3d52446c: five key columns concatenated over all
    snapshots, one global ``np.lexsort``, ranked within forest groups, with
    ForestIndex from ``np.searchsorted``.

    This is the binding oracle for the bounded pass — the same arithmetic, not a
    paraphrase of it — and it returns exactly what that function returned:
    ``({snap: (forest_index, ranks)}, n_forests_total, max_halo_rank_in_forest)``.
    """
    forest_table = np.load(Path(manifest.workdir) / "forest_index_table.npy")
    snaps = sorted(int(s) for s in manifest.data["snapshots"])
    forests_l, snaps_l, upids_l, pids_l, ids_l, counts = [], [], [], [], [], []
    for snap in snaps:
        records = links._load_fixed(manifest, snap)
        forests_l.append(records["forest_id"].copy())
        snaps_l.append(np.full(records.size, snap, dtype=np.int64))
        upids_l.append(records["upid"].copy())
        pids_l.append(records["pid"].copy())
        ids_l.append(records["id"].copy())
        counts.append(records.size)
    forest = np.concatenate(forests_l)
    neg_snap = -np.concatenate(snaps_l)
    upid = np.concatenate(upids_l)
    pid = np.concatenate(pids_l)
    ids = np.concatenate(ids_l)
    total = forest.size

    order = np.lexsort((ids, pid, upid, neg_snap, forest))
    sorted_forest = forest[order]
    new_forest = np.r_[True, sorted_forest[1:] != sorted_forest[:-1]]
    starts = np.nonzero(new_forest)[0]
    group_id = np.cumsum(new_forest) - 1
    ranks = np.empty(total, dtype=np.int64)
    ranks[order] = np.arange(total, dtype=np.int64) - starts[group_id]
    forest_index = np.searchsorted(forest_table, forest)

    identity = {}
    offset = 0
    for snap, count in zip(snaps, counts):
        identity[snap] = (forest_index[offset : offset + count], ranks[offset : offset + count])
        offset += count
    return identity, int(forest_table.size), (int(ranks.max()) if total else -1)


def scaling_forests(n_forests, snaps, halos_per_snap):
    """``n_forests`` structurally identical forests, each contributing exactly
    ``halos_per_snap`` halos to every snapshot in ``snaps`` (a contiguous
    ascending range): one central per snapshot, chained by descendant, with the
    rest of that snapshot's halos as its satellites.

    The total is ``n_forests * len(snaps) * halos_per_snap`` and the three
    factors are independent, which is what a memory measurement needs: growing
    the total by adding snapshots holds every per-snapshot quantity fixed —
    including the manifest checksum's own read block, which is sized from the
    file it is checksumming and would otherwise grow with the input.
    """
    snaps = list(snaps)
    assert snaps == list(range(snaps[0], snaps[-1] + 1)), "snaps must be contiguous"
    assert halos_per_snap <= 99_999, "id layout allows 99,999 halos per forest per snapshot"
    forests = []
    for index in range(n_forests):
        base = 1_000_000 + index * 2_000_000
        halos = []
        for snap in snaps:
            central = base + snap * 100_000
            halos.append(
                fixtures.HaloSpec(
                    halo_id=central,
                    snap=snap,
                    mvir=1.0e12 + snap,
                    desc_id=(central + 100_000) if snap != snaps[-1] else -1,
                )
            )
            for satellite in range(halos_per_snap - 1):
                halos.append(
                    fixtures.HaloSpec(
                        halo_id=central + 1 + satellite,
                        snap=snap,
                        mvir=1.0e10 + satellite,
                        pid=central,
                        upid=central,
                    )
                )
        forests.append(
            fixtures.ForestSpec(
                forest_id=100 + index,
                trees=[fixtures.TreeSpec(root_id=900_000 + index, halos=halos)],
            )
        )
    return forests


#: A longer a_list than the canned fixtures', so a fixture can spread the same
#: per-snapshot halo count over four times as many snapshots.
LONG_A_LIST = [round(0.08 * (index + 1), 5) for index in range(12)]


def reregister_forest_table(manifest, table):
    """Rewrite the Phase 0 forest index table and refresh its manifest checksum,
    so a reconciliation test fails on the mismatch it is testing rather than on
    the ownership guard."""
    table_path = Path(manifest.workdir) / "forest_index_table.npy"
    np.save(table_path, table)
    manifest.data["intermediates"][str(table_path.resolve())]["md5"] = file_md5(table_path)
    manifest.save()


class Slice5Case(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.addCleanup(self.tmp.cleanup)

    def identity_dirs(self, manifest):
        entry = manifest.data["snapshots"][sorted(manifest.data["snapshots"])[0]]
        scratch = Path(entry["fixed_file"]).parent
        return sorted(scratch.glob(links.IDENTITY_DIR_PREFIX + "*"))

    def assert_matches_oracle(self, manifest, budget_bytes):
        """Every value the pass produces, against the in-memory formulation."""
        expected, expected_forests, expected_max = in_memory_identity(manifest)
        identity, n_forests_total, max_rank = links.compute_identity(
            manifest, budget_bytes=budget_bytes
        )
        with identity:
            self.assertEqual(n_forests_total, expected_forests)
            self.assertEqual(max_rank, expected_max)
            for snap, (forest_index, ranks) in expected.items():
                observed_fi, observed_ranks = identity[snap]
                np.testing.assert_array_equal(observed_fi, forest_index)
                np.testing.assert_array_equal(observed_ranks, ranks)
                self.assertEqual(observed_fi.dtype, np.dtype("<i8"))
                self.assertEqual(observed_ranks.dtype, np.dtype("<i8"))
            return identity


class TestBoundedIdentityPass(Slice5Case):
    def test_matches_the_in_memory_formulation_at_every_budget(self):
        workdir, _, _ = make_linked_workdir(self.root)
        manifest = Manifest.load_or_create(workdir)
        for budget in (4096, 1 << 16, links.DEFAULT_RANK_BUDGET_BYTES):
            with self.subTest(budget=budget):
                self.assert_matches_oracle(manifest, budget)

    def test_matches_the_oracle_when_the_budget_forces_spill_and_merge(self):
        # the fixture forests are 17 halos; scale up so a small budget produces
        # many sorted runs and a real reduction pass, which is the path that
        # cannot be exercised at fixture size
        forests = scaling_forests(n_forests=12, snaps=range(4, 6), halos_per_snap=400)
        workdir, _, _ = make_linked_workdir(self.root, forests=forests)
        manifest = Manifest.load_or_create(workdir)
        identity = self.assert_matches_oracle(manifest, 8192)
        self.assertGreater(identity.n_runs, identity.merge_records)
        self.assertGreaterEqual(identity.n_merge_passes, 1)
        self.assertGreater(identity.peak_spill_bytes, 0)

    def test_emitted_links_are_identical_across_budgets(self):
        # the budget must be a memory control and nothing else: two runs of the
        # whole stage over the same input, at budgets three orders of magnitude
        # apart, must emit the same bytes
        emitted = {}
        for label, budget in (("small", 8192), ("default", links.DEFAULT_RANK_BUDGET_BYTES)):
            root = self.root / label
            root.mkdir()
            forests = scaling_forests(n_forests=6, snaps=range(4, 6), halos_per_snap=40)
            workdir, _, _ = make_linked_workdir(root, forests=forests)
            manifest = run_links(workdir, budget_bytes=budget)
            self.assertEqual(
                manifest.data["links"], {"n_forests_total": 6, "max_halo_rank_in_forest": 79}
            )
            emitted[label] = {
                snap: Path(entry["links_file"]).read_bytes()
                for snap, entry in manifest.data["snapshots"].items()
            }
        self.assertEqual(emitted["small"], emitted["default"])

    def test_golden_links_are_unchanged_at_a_tiny_budget(self):
        # the recorded hand-computed expectations, re-asserted through the
        # spilling path rather than only through the single-run path
        workdir, _, _ = make_linked_workdir(self.root)
        manifest = run_links(workdir, budget_bytes=4096)
        self.assertEqual(
            manifest.data["links"], {"n_forests_total": 5, "max_halo_rank_in_forest": 5}
        )
        seen = {}
        for snap_str, entry in manifest.data["snapshots"].items():
            links_records = np.fromfile(entry["links_file"], dtype=LINKS_RECORD_DTYPE)
            seen[int(snap_str)] = {
                name: links_records[name].tolist() for name in LINKS_RECORD_DTYPE.names
            }
        self.assertEqual(seen, GOLDEN_LINKS)

    def test_stores_are_removed_on_success(self):
        workdir, _, _ = make_linked_workdir(self.root)
        manifest = run_links(workdir)
        self.assertEqual(self.identity_dirs(manifest), [])

    def test_stores_are_removed_when_linking_fails(self):
        # the accessor's directory is not a manifest intermediate, so nothing
        # else would ever clean it up: the pass owns it on the failure path too
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
        manifest = Manifest.load_or_create(workdir)
        self.assertEqual(self.identity_dirs(manifest), [])

    def test_stores_are_removed_when_the_rank_pass_itself_fails(self):
        workdir, _, _ = make_linked_workdir(self.root)
        manifest = Manifest.load_or_create(workdir)
        # a table with a forest nothing observed: the reconciliation fires after
        # the core has written both stores, which is the path that leaves them
        # behind if the pass does not own them
        table = np.load(Path(workdir) / "forest_index_table.npy")
        reregister_forest_table(manifest, np.append(table, np.int64(999_999)))
        with self.assertRaisesRegex(ConverterError, "have no halos"):
            links.compute_identity(manifest)
        self.assertEqual(self.identity_dirs(manifest), [])

    def test_accessor_holds_at_most_the_adjacent_pair(self):
        forests = scaling_forests(n_forests=4, snaps=range(2, 6), halos_per_snap=25)
        workdir, _, _ = make_linked_workdir(self.root, forests=forests)
        manifest = Manifest.load_or_create(workdir)
        identity, _, _ = links.compute_identity(manifest, budget_bytes=1 << 16)
        with identity:
            snaps = sorted(int(s) for s in manifest.data["snapshots"])
            for snap in snaps:
                identity[snap]
                if snap + 1 in identity:
                    identity[snap + 1]
                # the same access pattern link_one_snapshot uses, including its
                # second look at snap after the pair check
                identity[snap]
                self.assertLessEqual(len(identity.resident_snapshots()), links.RESIDENT_SNAPSHOTS)
                self.assertIn(snap, identity.resident_snapshots())
            with self.assertRaises(KeyError):
                identity[max(snaps) + 5]
        self.assertFalse(identity.directory.exists())

    def test_stores_are_removed_when_the_success_log_raises(self):
        # the window between constructing the accessor and returning it: the
        # store exists, the expensive pass has already SUCCEEDED, and nothing
        # else in the converter owns those bytes. A bare print is not a safe
        # statement to leave in that window — BrokenPipeError on a closed pipe,
        # ENOSPC on the full volume this pass is bounded for
        workdir, _, _ = make_linked_workdir(self.root)
        manifest = Manifest.load_or_create(workdir)
        with mock.patch.object(links, "_log", side_effect=OSError("No space left on device")):
            with self.assertRaisesRegex(OSError, "No space left on device"):
                links.compute_identity(manifest)
        self.assertEqual(self.identity_dirs(manifest), [])

    def test_a_failed_removal_after_a_late_failure_is_reported_and_masks_nothing(self):
        # the failure branch of the same asymmetry: the stores are written, the
        # pass then fails, and removal fails too. The warning must name the
        # store and its real size, and the ORIGINAL exception must be what
        # reaches the caller — cleaning up after a failure must not replace the
        # traceback that explains the run
        workdir, _, _ = make_linked_workdir(self.root)
        manifest = Manifest.load_or_create(workdir)
        total = sum(entry["rows"] for entry in manifest.data["snapshots"].values())
        with mock.patch.object(links, "shutil") as shutil_module:
            shutil_module.rmtree.return_value = None  # removal silently does nothing
            with mock.patch.object(
                links, "verify_identity", side_effect=ConverterError("injected late failure")
            ):
                with mock.patch.object(links, "_log") as log:
                    with self.assertRaisesRegex(ConverterError, "injected late failure"):
                        links.compute_identity(manifest)
        warnings = [
            call.args[0]
            for call in log.call_args_list
            if "could not remove the identity store" in call.args[0]
        ]
        self.assertEqual(len(warnings), 1)
        self.assertIn("{} byte(s)".format(2 * 8 * total), warnings[0])
        leftover = self.identity_dirs(manifest)
        self.assertEqual(len(leftover), 1)  # it really was left behind
        for path in leftover:
            shutil.rmtree(path)

    def test_a_failure_while_reporting_a_failed_removal_does_not_mask_it(self):
        # cleanup runs while an exception is already propagating, and the
        # reporting it does can itself fail (a closed stderr is the same class
        # of problem it exists to report). What reaches the caller must still be
        # the exception that explains the run
        workdir, _, _ = make_linked_workdir(self.root)
        manifest = Manifest.load_or_create(workdir)
        with mock.patch.object(links, "shutil") as shutil_module:
            shutil_module.rmtree.return_value = None
            with mock.patch.object(
                links, "verify_identity", side_effect=ConverterError("injected late failure")
            ):
                with mock.patch.object(links, "_log", side_effect=OSError("stderr is gone")):
                    with self.assertRaisesRegex(ConverterError, "injected late failure"):
                        links.compute_identity(manifest)
        for path in self.identity_dirs(manifest):
            shutil.rmtree(path)

    def test_stores_are_removed_when_a_setup_statement_raises(self):
        # the statements between creating the store and the guarded block cannot
        # raise today, and the guard's comment claims that of the WHOLE window;
        # this pins the claim instead of trusting it, by making one of them fail
        workdir, _, _ = make_linked_workdir(self.root)
        manifest = Manifest.load_or_create(workdir)
        with mock.patch.object(links, "FOREST_INDEX_STORE_NAME", None):
            with self.assertRaises(TypeError):
                links.compute_identity(manifest)
        self.assertEqual(self.identity_dirs(manifest), [])

    def test_a_failed_removal_before_the_size_is_known_says_unknown(self):
        # an early failure has no byte count to report yet, and a wrong number
        # would be worse than none
        workdir, _, _ = make_linked_workdir(self.root)
        manifest = Manifest.load_or_create(workdir)
        with mock.patch.object(links, "shutil") as shutil_module:
            shutil_module.rmtree.return_value = None
            with mock.patch.object(
                links, "rank_forests", side_effect=RankSortError("injected early failure")
            ):
                with mock.patch.object(links, "_log") as log:
                    with self.assertRaisesRegex(ConverterError, "injected early failure"):
                        links.compute_identity(manifest)
        warnings = [
            call.args[0]
            for call in log.call_args_list
            if "could not remove the identity store" in call.args[0]
        ]
        self.assertEqual(len(warnings), 1)
        self.assertIn("size unknown", warnings[0])
        self.assertNotIn("byte(s)", warnings[0])
        for path in self.identity_dirs(manifest):
            shutil.rmtree(path)

    def test_an_accessor_nobody_closes_still_releases_its_store(self):
        # the window between compute_identity returning and run_links entering
        # ``with identity:`` is two statements, and an asynchronous exception
        # can land between them. Ownership therefore lives on the accessor's
        # lifetime: drop the last reference without closing, and the store goes
        workdir, _, _ = make_linked_workdir(self.root)
        manifest = Manifest.load_or_create(workdir)
        identity, _, _ = links.compute_identity(manifest)
        directory = identity.directory
        self.assertTrue(directory.exists())
        del identity
        gc.collect()
        self.assertFalse(directory.exists())
        self.assertEqual(self.identity_dirs(manifest), [])

    def test_a_store_that_cannot_be_removed_is_reported(self):
        # close() must not raise, so a failed removal can only be surfaced
        # through the log — and it must be surfaced: the storage envelope is
        # written assuming these bytes are gone
        directory = self.root / "stuck"
        directory.mkdir()
        np.asarray([0], dtype=np.int64).tofile(directory / links.FOREST_INDEX_STORE_NAME)
        np.asarray([0], dtype=np.int64).tofile(directory / links.RANKS_STORE_NAME)
        identity = links.SnapshotIdentity(
            directory, {3: (0, 1)}, peak_spill_bytes=0, store_bytes=16
        )
        with mock.patch.object(links.shutil, "rmtree") as rmtree:
            with mock.patch.object(links, "_log") as log:
                identity.close()
        rmtree.assert_called_once()
        self.assertTrue(directory.exists())
        self.assertEqual(len(log.call_args_list), 1)
        message = log.call_args_list[0].args[0]
        self.assertIn("could not remove the identity store", message)
        self.assertIn(str(directory), message)

    def test_a_failed_removal_leaves_the_store_owned_and_a_retry_removes_it(self):
        # weakref.finalize is one-shot and marks itself dead BEFORE it calls, so
        # releasing ownership through it would hand a failed removal to nobody:
        # no later close(), no destruction, no interpreter exit would retry it.
        # Ownership must survive an attempt that did not confirm absence
        directory = self.root / "retry"
        directory.mkdir()
        np.asarray([0], dtype=np.int64).tofile(directory / links.FOREST_INDEX_STORE_NAME)
        identity = links.SnapshotIdentity(
            directory, {3: (0, 1)}, peak_spill_bytes=0, store_bytes=16
        )
        with mock.patch.object(links.shutil, "rmtree"):  # removal does nothing
            with mock.patch.object(links, "_log") as log:
                identity.close()
        self.assertTrue(directory.exists())
        self.assertEqual(len(log.call_args_list), 1)
        self.assertIn("could not remove the identity store", log.call_args_list[0].args[0])
        # still owned...
        self.assertTrue(identity._finalizer.alive)
        # ...so the retry, whoever makes it, actually removes the bytes
        with mock.patch.object(links, "_log") as log:
            identity.close()
        self.assertFalse(directory.exists())
        self.assertFalse(identity._finalizer.alive)
        log.assert_not_called()

    def test_an_interrupted_removal_leaves_the_store_owned(self):
        # the case a narrowed docstring could not have covered: an asynchronous
        # exception inside the removal itself. It gets no warning (the helper
        # deliberately does not catch BaseException), so if ownership were
        # released first the store would be stranded in SILENCE
        directory = self.root / "interrupted"
        directory.mkdir()
        identity = links.SnapshotIdentity(directory, {}, peak_spill_bytes=0, store_bytes=0)
        with mock.patch.object(links.shutil, "rmtree", side_effect=KeyboardInterrupt):
            with self.assertRaises(KeyboardInterrupt):
                identity.close()
        self.assertTrue(directory.exists())
        self.assertTrue(identity._finalizer.alive)
        identity.close()
        self.assertFalse(directory.exists())

    def test_lifetime_release_retries_a_removal_close_could_not_make(self):
        # the same guarantee reached the other way: close() failed, nobody calls
        # it again, and the accessor becomes unreachable
        workdir, _, _ = make_linked_workdir(self.root)
        manifest = Manifest.load_or_create(workdir)
        identity, _, _ = links.compute_identity(manifest)
        directory = identity.directory
        with mock.patch.object(links.shutil, "rmtree"):
            with mock.patch.object(links, "_log"):
                identity.close()
        self.assertTrue(directory.exists())
        del identity
        gc.collect()
        self.assertFalse(directory.exists())
        self.assertEqual(self.identity_dirs(manifest), [])

    def test_close_is_silent_and_raises_nothing_when_removal_works(self):
        directory = self.root / "clean"
        directory.mkdir()
        identity = links.SnapshotIdentity(directory, {}, peak_spill_bytes=0, store_bytes=0)
        with mock.patch.object(links, "_log") as log:
            identity.close()
            identity.close()  # idempotent: the second call has nothing to remove
        self.assertFalse(directory.exists())
        log.assert_not_called()

    def test_accessor_serves_an_empty_snapshot_window(self):
        # a snapshot with no halos owns a zero-length window into the stores,
        # and the link stage must still receive two empty int64 arrays for it
        directory = self.root / "stores"
        directory.mkdir()
        np.asarray([0, 0], dtype=np.int64).tofile(directory / links.FOREST_INDEX_STORE_NAME)
        np.asarray([0, 1], dtype=np.int64).tofile(directory / links.RANKS_STORE_NAME)
        identity = links.SnapshotIdentity(
            directory, {3: (0, 2), 4: (2, 0)}, peak_spill_bytes=0, store_bytes=32
        )
        with identity:
            forest_index, ranks = identity[4]
            self.assertEqual((forest_index.size, ranks.size), (0, 0))
            self.assertEqual(forest_index.dtype, np.dtype("<i8"))
            np.testing.assert_array_equal(identity[3][1], [0, 1])
        self.assertFalse(directory.exists())

    def test_accessor_refuses_a_short_store(self):
        directory = self.root / "short"
        directory.mkdir()
        np.asarray([0], dtype=np.int64).tofile(directory / links.FOREST_INDEX_STORE_NAME)
        np.asarray([0], dtype=np.int64).tofile(directory / links.RANKS_STORE_NAME)
        identity = links.SnapshotIdentity(
            directory, {3: (0, 4)}, peak_spill_bytes=0, store_bytes=16
        )
        with identity:
            with self.assertRaisesRegex(
                ConverterError, "holds 1 of the 4 ForestIndex value\(s\) snapshot 3 needs"
            ):
                identity[3]

    def test_cli_accepts_a_memory_budget(self):
        workdir, _, _ = make_linked_workdir(self.root)
        rc = convert_ctrees.main(["links", "--workdir", str(workdir), "--memory-budget-mb", "1"])
        self.assertEqual(rc, 0)
        manifest = Manifest.load_or_create(workdir)
        self.assertEqual(
            {entry["status"] for entry in manifest.data["snapshots"].values()}, {"linked"}
        )

    def test_cli_refuses_an_unusable_memory_budget(self):
        workdir, _, _ = make_linked_workdir(self.root)
        rc = convert_ctrees.main(["links", "--workdir", str(workdir), "--memory-budget-mb", "0"])
        self.assertEqual(rc, 1)
        manifest = Manifest.load_or_create(workdir)
        self.assertEqual(
            {entry["status"] for entry in manifest.data["snapshots"].values()}, {"fixed"}
        )

    def test_budget_below_the_core_minimum_is_refused_with_both_figures(self):
        workdir, _, _ = make_linked_workdir(self.root)
        manifest = Manifest.load_or_create(workdir)
        with self.assertRaisesRegex(ConverterError, "memory budget of 64 byte\\(s\\) is too small"):
            links.compute_identity(manifest, budget_bytes=64)


class TestForestTableReconciliation(Slice5Case):
    def test_listed_forest_with_no_halos_aborts(self):
        workdir, _, _ = make_linked_workdir(self.root)
        manifest = Manifest.load_or_create(workdir)
        table = np.load(Path(workdir) / "forest_index_table.npy")
        reregister_forest_table(manifest, np.append(table, np.int64(777_777)))
        with self.assertRaisesRegex(
            ConverterError,
            r"1 listed forest\(s\) have no halos \(examples: \[777777\]\), "
            r"0 observed forest\(s\) are unlisted",
        ):
            run_links(workdir)

    def test_observed_forest_unlisted_aborts(self):
        workdir, _, _ = make_linked_workdir(self.root)
        manifest = Manifest.load_or_create(workdir)
        table = np.load(Path(workdir) / "forest_index_table.npy")
        dropped = int(table[1])
        reregister_forest_table(manifest, np.delete(table, 1))
        with self.assertRaisesRegex(
            ConverterError,
            r"0 listed forest\(s\) have no halos \(examples: \[\]\), "
            r"1 observed forest\(s\) are unlisted \(examples: \[{}\]\)".format(dropped),
        ):
            run_links(workdir)

    def test_both_directions_are_reported_together(self):
        workdir, _, _ = make_linked_workdir(self.root)
        manifest = Manifest.load_or_create(workdir)
        table = np.load(Path(workdir) / "forest_index_table.npy")
        mangled = np.sort(np.append(np.delete(table, 0), np.int64(888_888)))
        reregister_forest_table(manifest, mangled)
        with self.assertRaisesRegex(
            ConverterError,
            r"1 listed forest\(s\) have no halos \(examples: \[888888\]\), "
            r"1 observed forest\(s\) are unlisted",
        ):
            run_links(workdir)


class TestVerifyIdentityBounded(unittest.TestCase):
    """The verifier's own conditions, exercised across chunk boundaries.

    Its three conditions are unchanged from the lexsort formulation; what
    changed is that it now reads its inputs in bounded chunks, so every
    condition is re-asserted for a violation that straddles a chunk and one that
    does not.
    """

    def budget_for(self, chunk_rows):
        return links.STREAM_BUDGET_SHARE * chunk_rows * links.VERIFY_STREAM_BYTES_PER_ROW

    def test_dense_identity_passes_one_row_at_a_time(self):
        forest_index = np.asarray([0, 0, 0, 1, 1, 2], dtype=np.int64)
        ranks = np.asarray([2, 0, 1, 1, 0, 0], dtype=np.int64)
        verify_identity(forest_index, ranks, 3, "test", budget_bytes=self.budget_for(1))

    def test_duplicate_pair_inside_one_chunk_aborts(self):
        forest_index = np.asarray([0, 0, 0, 0], dtype=np.int64)
        ranks = np.asarray([1, 1, 2, 3], dtype=np.int64)
        with self.assertRaisesRegex(ConverterError, r"1 \(ForestIndex, HaloRankInForest\) pair"):
            verify_identity(forest_index, ranks, 1, "test", budget_bytes=self.budget_for(4))

    def test_duplicate_pair_across_chunks_aborts(self):
        # the same defect split across two read windows: a per-chunk-only check
        # would pass this
        forest_index = np.asarray([0, 0, 0, 0], dtype=np.int64)
        ranks = np.asarray([1, 2, 3, 1], dtype=np.int64)
        with self.assertRaisesRegex(ConverterError, r"1 \(ForestIndex, HaloRankInForest\) pair"):
            verify_identity(forest_index, ranks, 1, "test", budget_bytes=self.budget_for(2))

    def test_rank_outside_the_forest_range_aborts(self):
        forest_index = np.asarray([0, 0], dtype=np.int64)
        ranks = np.asarray([0, 7], dtype=np.int64)
        with self.assertRaisesRegex(
            ConverterError, r"examples: \(ForestIndex=0, rank=7, expected 1\)"
        ):
            verify_identity(forest_index, ranks, 1, "test", budget_bytes=self.budget_for(1))

    def test_negative_rank_aborts(self):
        forest_index = np.asarray([0, 0], dtype=np.int64)
        ranks = np.asarray([-1, 1], dtype=np.int64)
        with self.assertRaisesRegex(
            ConverterError, r"examples: \(ForestIndex=0, rank=-1, expected 0\)"
        ):
            verify_identity(forest_index, ranks, 1, "test", budget_bytes=self.budget_for(2))

    def test_example_names_the_rank_no_halo_holds(self):
        forest_index = np.asarray([0, 0, 0], dtype=np.int64)
        ranks = np.asarray([0, 0, 2], dtype=np.int64)
        with self.assertRaisesRegex(
            ConverterError,
            r"1 \(ForestIndex, HaloRankInForest\) pair\(s\) violate per-forest "
            r"density/uniqueness; examples: \(ForestIndex=0, rank=0, expected 1\)",
        ):
            verify_identity(forest_index, ranks, 1, "test", budget_bytes=self.budget_for(2))

    def test_aggregate_collision_is_still_caught(self):
        # the counter-example the Slice 4 review found for aggregate rank
        # checks: forest counts [3, 2] with ranks [0,0,2 | 1,1] match dense
        # [0,1,2 | 0,1] in sum, maximum and modular sum-of-squares, and neither
        # forest is dense. The bitset does not admit it.
        forest_index = np.asarray([0, 0, 0, 1, 1], dtype=np.int64)
        ranks = np.asarray([0, 0, 2, 1, 1], dtype=np.int64)
        self.assertEqual(int(ranks.sum()), 4)
        self.assertEqual(int(ranks.max()), 2)
        with self.assertRaisesRegex(ConverterError, r"2 \(ForestIndex, HaloRankInForest\) pair"):
            verify_identity(forest_index, ranks, 2, "test", budget_bytes=self.budget_for(2))

    def test_sparse_forest_index_across_chunks_aborts(self):
        forest_index = np.asarray([0, 0, 2, 2], dtype=np.int64)
        ranks = np.asarray([0, 1, 0, 1], dtype=np.int64)
        with self.assertRaisesRegex(ConverterError, r"not dense over \[0, 3\)"):
            verify_identity(forest_index, ranks, 3, "test", budget_bytes=self.budget_for(2))

    def test_forest_index_above_the_range_aborts(self):
        forest_index = np.asarray([0, 1], dtype=np.int64)
        ranks = np.asarray([0, 0], dtype=np.int64)
        with self.assertRaisesRegex(ConverterError, r"not dense over \[0, 1\)"):
            verify_identity(forest_index, ranks, 1, "test", budget_bytes=self.budget_for(1))

    def test_column_length_mismatch_refused(self):
        with self.assertRaisesRegex(ConverterError, "got 3 ForestIndex value\\(s\\) against 2"):
            verify_identity(np.zeros(3, dtype=np.int64), np.zeros(2, dtype=np.int64), 1, "test")

    def test_empty_run_passes(self):
        verify_identity(np.empty(0, dtype=np.int64), np.empty(0, dtype=np.int64), 0, "test")

    def test_on_disk_columns_verify_identically(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            forest_index = np.asarray([0, 0, 1, 1, 1], dtype=np.int64)
            ranks = np.asarray([1, 0, 2, 0, 1], dtype=np.int64)
            fi_path, rank_path = root / "fi.i64", root / "rank.i64"
            forest_index.tofile(fi_path)
            ranks.tofile(rank_path)
            columns = (
                links._Int64Column(fi_path, forest_index.size),
                links._Int64Column(rank_path, ranks.size),
            )
            verify_identity(columns[0], columns[1], 2, "test", budget_bytes=self.budget_for(2))
            broken = ranks.copy()
            broken[0] = 0
            broken.tofile(rank_path)
            with self.assertRaisesRegex(ConverterError, "density/uniqueness"):
                verify_identity(columns[0], columns[1], 2, "test", budget_bytes=self.budget_for(2))

    def test_truncated_on_disk_column_refused(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "short.i64"
            np.asarray([0, 0], dtype=np.int64).tofile(path)
            column = links._Int64Column(path, 4)
            with self.assertRaisesRegex(ConverterError, "ended after 2 of 4 int64 value"):
                verify_identity(column, column, 1, "test")


class TestIdentityMemoryScaling(Slice5Case):
    """Actual allocation, not an instrument's own counter.

    Slice 4 lost four rounds to allocations that escaped its meter, each
    invisible because the test read the meter. This measures ``tracemalloc``'s
    peak for a whole ``compute_identity`` call at a fixed budget over two
    workdirs differing 4x in total halo count, and the in-memory formulation it
    replaced is measured beside it as the control that proves the measurement
    can see growth at all.

    **What these workdirs are, precisely.** They are real converter workdirs —
    assembled by the real scatter, sort and fix-up stages, with real manifests
    and real fixed scratch files — but their halos come from synthetic fixture
    forests. That is deliberately NOT the plan's primary evidence for "memory
    does not scale with total halo count", which requires two real emitted
    datasets differing by at least 4x and pointedly excludes synthetic fixtures:
    that route is satisfied separately by the fixed-budget peak-RSS pair
    measured on micro-Uchuu against the 406,668,896-halo rehearsal subset. This
    is the criterion's stated ALTERNATIVE route — instrument the resident
    buffers so a total-count-sized allocation cannot hide — which a fixture can
    serve, because it localises growth to a specific buffer in a way that
    comparing two whole-dataset RSS figures cannot.
    """

    #: Allowance for the growth that is NOT per-halo: the sort core's per-run
    #: bookkeeping (O(number of runs), so it shrinks as the budget grows —
    #: rank_sort's own exclusion list, category 3), per-snapshot bookkeeping and
    #: interpreter churn. Small enough that one whole int64 column of the run
    #: escaping into memory (8 B/halo) cannot hide inside it, which is asserted
    #: below rather than assumed.
    ALLOWANCE_BYTES = 1 << 17

    def prepare(self, n_snaps):
        """A workdir whose snapshots are the same size whatever ``n_snaps`` is,
        so only the run's TOTAL halo count differs between two calls."""
        root = self.root / "s{}".format(n_snaps)
        root.mkdir()
        forests = scaling_forests(n_forests=10, snaps=range(11 - n_snaps, 11), halos_per_snap=1000)
        workdir, _, _ = make_linked_workdir(root, forests=forests, a_list_values=LONG_A_LIST)
        return Manifest.load_or_create(workdir)

    def peak_of(self, call):
        """``tracemalloc``'s peak for one call, with the manifest checksum's own
        read block shrunk.

        ``Manifest.verify_intermediate`` checksums through ``file_md5``, whose
        8 MB read block CPython allocates in full before it knows how much the
        file holds. That spike is constant, bounded, pre-existing and in
        ``scatter.py``, which this slice does not touch — but it is larger than
        any leak this test needs to see, and a peak measurement reports the
        maximum, so it would mask one completely. The checksum still runs, and
        still runs for real; only its block size changes.
        """
        gc.collect()
        with mock.patch(
            "scatter.file_md5", lambda path, blocksize=64 * 1024: file_md5(path, blocksize)
        ):
            tracemalloc.start()
            try:
                tracemalloc.reset_peak()
                before = tracemalloc.get_traced_memory()[0]
                result = call()
                peak = tracemalloc.get_traced_memory()[1]
            finally:
                tracemalloc.stop()
        return result, peak - before

    def test_peak_allocation_does_not_scale_with_halo_count(self):
        budget = 1 << 18
        peaks, oracle_peaks, totals = {}, {}, {}
        for label, n_snaps in (("base", 2), ("four_times", 8)):
            manifest = self.prepare(n_snaps)
            totals[label] = sum(entry["rows"] for entry in manifest.data["snapshots"].values())
            identity, n_forests, max_rank = None, None, None

            def bounded(manifest=manifest):
                return links.compute_identity(manifest, budget_bytes=budget)

            (identity, n_forests, max_rank), peaks[label] = self.peak_of(bounded)
            with identity:
                # the measurement must be of a call that did the right thing
                expected, expected_forests, expected_max = in_memory_identity(manifest)
                self.assertEqual((n_forests, max_rank), (expected_forests, expected_max))
                for snap, (forest_index, ranks) in expected.items():
                    observed_fi, observed_ranks = identity[snap]
                    np.testing.assert_array_equal(observed_fi, forest_index)
                    np.testing.assert_array_equal(observed_ranks, ranks)
                self.assertGreater(identity.n_runs, 1)
            _, oracle_peaks[label] = self.peak_of(
                lambda manifest=manifest: in_memory_identity(manifest)
            )
        self.assertGreaterEqual(totals["four_times"], 4 * totals["base"])
        grown = totals["four_times"] - totals["base"]
        # the measurement is only worth making if it could see the defect class
        # it exists for: one int64 column over the whole run held in memory
        self.assertGreater(8 * grown, grown // 8 + self.ALLOWANCE_BYTES)
        # the bounded pass's only halo-count-sized structure is the verifier's
        # bitset, at one bit per halo, and it is the reason an exact per-forest
        # density check is affordable at all
        self.assertLessEqual(peaks["four_times"] - peaks["base"], grown // 8 + self.ALLOWANCE_BYTES)
        # not a vacuous bound: the same measurement, over the same two inputs,
        # sees the formulation this replaced blow the very bound the bounded
        # pass just met — tens of bytes per halo against one bit
        self.assertGreater(
            oracle_peaks["four_times"] - oracle_peaks["base"],
            grown // 8 + self.ALLOWANCE_BYTES,
        )


class TestLinksConsumesIntermediates(unittest.TestCase):
    """Plan Slice 8 deletion table, link-stage half.

    Two entries land here and one deliberately does not. ``pending_fp_N`` goes
    once the snapshot that consumes it is linked; ``idx_N`` goes once snapshot
    N−1 has been linked, which makes ``idx``'s consumer the link BELOW it and
    leaves any index without a recorded predecessor with no consumer at all.
    ``fixed_N`` and ``links_N`` do NOT go here — the writer reads both, and is
    their terminal consumer.

    The fixture's recorded snapshot set is {1, 2, 3, 4, 5}: snapshot 0 has no
    halos, so ``idx_1`` is the no-consumer index this dataset exercises.
    """

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.addCleanup(self.tmp.cleanup)

    def _fixed_workdir(self):
        workdir, a_list, sim_info = make_linked_workdir(self.root)
        return workdir

    @staticmethod
    def _paths(workdir):
        """Every intermediate this stage's deletion table names, plus the two
        it must leave alone, keyed for readable assertions."""
        manifest = Manifest.load_or_create(workdir)
        snaps = sorted(int(s) for s in manifest.data["snapshots"])
        scratch = Path(manifest.data["snapshots"][str(snaps[0])]["fixed_file"]).parent
        paths = {}
        for snap in snaps:
            entry = manifest.data["snapshots"][str(snap)]
            paths["idx_{}".format(snap)] = Path(entry["index_file"])
            paths["fixed_{}".format(snap)] = Path(entry["fixed_file"])
            if snap - 1 in snaps:
                paths["pending_fp_{}".format(snap)] = scratch / pending_fp_name(snap)
        return paths

    def test_flag_off_retains_every_index_and_pending_buffer(self):
        workdir = self._fixed_workdir()
        run_links(workdir)
        manifest = Manifest.load_or_create(workdir)
        for name, path in self._paths(workdir).items():
            self.assertTrue(path.exists(), "{} was deleted with the flag off".format(name))
            self.assertEqual(
                "present",
                manifest.data["intermediates"][str(path.resolve())]["status"],
                name,
            )

    def test_flag_on_consumes_exactly_the_table(self):
        workdir = self._fixed_workdir()
        expected = self._paths(workdir)
        run_links(workdir, consume_intermediates=True)
        manifest = Manifest.load_or_create(workdir)
        for name, path in expected.items():
            entry = manifest.data["intermediates"][str(path.resolve())]
            if name.startswith("fixed_"):
                self.assertTrue(path.exists(), "{} must survive the link stage".format(name))
                self.assertEqual("present", entry["status"], name)
            else:
                self.assertFalse(path.exists(), "{} survived".format(name))
                self.assertEqual("removed", entry["status"], name)
        # the links files this stage produced are the writer's input and must
        # also survive it
        for snap in sorted(int(s) for s in manifest.data["snapshots"]):
            links_path = Path(manifest.data["snapshots"][str(snap)]["links_file"])
            self.assertTrue(links_path.exists())
            self.assertEqual(
                "present", manifest.data["intermediates"][str(links_path.resolve())]["status"]
            )

    def test_no_consumer_index_goes_as_soon_as_linking_starts(self):
        """``idx_1`` has no consumer here — ``link_one_snapshot(0)`` never runs,
        because linking iterates only recorded snapshots — so it is gone before
        the first snapshot is linked, not after some later one."""
        workdir = self._fixed_workdir()
        expected = self._paths(workdir)
        seen = []
        real = links.link_one_snapshot

        def spy(manifest, snap, identity):
            seen.append((snap, expected["idx_1"].exists()))
            return real(manifest, snap, identity)

        with mock.patch.object(links, "link_one_snapshot", spy):
            run_links(workdir, consume_intermediates=True)
        self.assertEqual([(snap, False) for snap, _ in seen], seen)

    def test_each_entry_survives_until_its_own_consumer_has_run(self):
        """Deleting an index or a pending buffer one snapshot early breaks
        linking, so the point of deletion is the acceptance criterion, not just
        the end state. Sampled before every ``link_one_snapshot`` call."""
        workdir = self._fixed_workdir()
        expected = self._paths(workdir)
        before = {}
        real = links.link_one_snapshot

        def spy(manifest, snap, identity):
            before[snap] = {
                name: path.exists() for name, path in expected.items() if not name.startswith("f")
            }
            return real(manifest, snap, identity)

        with mock.patch.object(links, "link_one_snapshot", spy):
            run_links(workdir, consume_intermediates=True)

        snaps = sorted(before)
        for snap in snaps:
            state = before[snap]
            # what this call is about to read must still be there
            if "pending_fp_{}".format(snap) in state:
                self.assertTrue(state["pending_fp_{}".format(snap)], snap)
            if "idx_{}".format(snap + 1) in state:
                self.assertTrue(state["idx_{}".format(snap + 1)], snap)
            # what an earlier call finished with must already be gone
            for earlier in snaps:
                if earlier >= snap:
                    continue
                if "pending_fp_{}".format(earlier) in state:
                    self.assertFalse(state["pending_fp_{}".format(earlier)], (snap, earlier))
                if "idx_{}".format(earlier + 1) in state:
                    self.assertFalse(state["idx_{}".format(earlier + 1)], (snap, earlier))

    def test_consumption_is_resumable_mid_stage(self):
        """A links run that dies part-way with the flag on must still resume:
        every input a later snapshot needs is deleted only after that snapshot
        has been linked."""
        workdir = self._fixed_workdir()
        real = links.link_one_snapshot

        def die_at_four(manifest, snap, identity):
            if snap == 4:
                raise RuntimeError("simulated crash")
            return real(manifest, snap, identity)

        with mock.patch.object(links, "link_one_snapshot", die_at_four):
            with self.assertRaises(RuntimeError):
                run_links(workdir, consume_intermediates=True)
        manifest = Manifest.load_or_create(workdir)
        self.assertEqual("linked", manifest.data["snapshots"]["3"]["status"])
        self.assertEqual("fixed", manifest.data["snapshots"]["4"]["status"])

        run_links(workdir, consume_intermediates=True)
        manifest = Manifest.load_or_create(workdir)
        for snap in sorted(int(s) for s in manifest.data["snapshots"]):
            self.assertEqual("linked", manifest.data["snapshots"][str(snap)]["status"])

    def test_crash_between_unlink_and_save_converges_to_removed(self):
        for delete in (False, True):
            with self.subTest(consume_intermediates=delete):
                root = self.root / "crash-{}".format(int(delete))
                root.mkdir()
                workdir, _, _ = make_linked_workdir(root)
                run_links(workdir)
                expected = self._paths(workdir)
                victim = expected["pending_fp_3"]
                victim.unlink()  # the unlink landed; the save did not
                manifest = Manifest.load_or_create(workdir)
                self.assertEqual(
                    "present", manifest.data["intermediates"][str(victim.resolve())]["status"]
                )
                run_links(workdir, consume_intermediates=delete)
                reloaded = Manifest.load_or_create(workdir)
                self.assertEqual(
                    "removed", reloaded.data["intermediates"][str(victim.resolve())]["status"]
                )

    def test_identity_backing_arrays_are_gone_once_links_completes(self):
        """The last deletion-table entry. The identity stores are per-invocation
        scratch owned by the link stage itself, never manifest intermediates, so
        nothing here registers or removes them — but the storage envelope
        assumes they are gone, so assert it in both flag states."""
        for delete in (False, True):
            with self.subTest(consume_intermediates=delete):
                root = self.root / "identity-{}".format(int(delete))
                root.mkdir()
                workdir, _, _ = make_linked_workdir(root)
                manifest = run_links(workdir, consume_intermediates=delete)
                scratch = Path(manifest.data["snapshots"]["5"]["fixed_file"]).parent
                self.assertEqual([], sorted(scratch.glob(links.IDENTITY_DIR_PREFIX + "*")))
                for key, entry in manifest.data["intermediates"].items():
                    self.assertNotIn(links.IDENTITY_DIR_PREFIX, key)
                    self.assertIsNotNone(entry)

    def test_refuse_not_repair_still_runs_while_the_inputs_are_present(self):
        """The short-circuit is conditioned on the fixed inputs being consumed,
        not on every snapshot merely being linked, precisely so this comparison
        keeps running wherever a links re-run is still reachable."""
        workdir = self._fixed_workdir()
        manifest = run_links(workdir)
        manifest.data["links"]["n_forests_total"] += 1
        manifest.save()
        with self.assertRaisesRegex(ConverterError, "run-scoped identity values changed"):
            run_links(workdir)

    def _pipeline_paths(self, workdir):
        """Just the two entries this stage deletes, keyed for assertions."""
        return {
            name: path
            for name, path in self._paths(workdir).items()
            if name.startswith(("idx_", "pending_fp_"))
        }

    def test_short_circuit_still_drains_a_flag_off_links_run(self):
        """The legal sequence links(off) -> write(on) -> links(on). The re-run
        takes the short-circuit, and must still delete the indexes and pending
        buffers the flag-off run left behind — an operator who opts in late
        gets the whole deletion table, not a subset of it."""
        root = self.root / "off-then-on"
        root.mkdir()
        workdir, a_list, sim_info = make_linked_workdir(root)
        run_links(workdir)
        expected = self._pipeline_paths(workdir)
        self.assertTrue(expected)
        for path in expected.values():
            self.assertTrue(path.exists())

        run_write(
            workdir,
            a_list_path=a_list,
            simulation_info_path=sim_info,
            consume_intermediates=True,
        )
        for path in expected.values():
            self.assertTrue(path.exists(), "the writer must not delete these")

        with capture_stderr() as captured:
            run_links(workdir, consume_intermediates=True)
        self.assertIn("skipping the rank pass", captured.text)
        manifest = Manifest.load_or_create(workdir)
        for name, path in expected.items():
            self.assertFalse(path.exists(), "{} survived the short-circuit".format(name))
            self.assertEqual(
                "removed", manifest.data["intermediates"][str(path.resolve())]["status"], name
            )

    def test_short_circuit_drains_after_an_interrupted_writer(self):
        """Same drain, reached the other way: a writer run that died part-way
        still consumed some fixed files, so a links re-run short-circuits — and
        must finish this stage's own deletions rather than return empty-handed.
        """
        root = self.root / "interrupted-writer"
        root.mkdir()
        workdir, a_list, sim_info = make_linked_workdir(root)
        run_links(workdir)
        expected = self._pipeline_paths(workdir)

        real = hdf5_writer._consume_snapshot_scratch
        calls = {"n": 0}

        def die_after_two(manifest, snap, delete):
            real(manifest, snap, delete)
            calls["n"] += 1
            if calls["n"] == 2:
                raise RuntimeError("simulated writer crash")

        with mock.patch.object(hdf5_writer, "_consume_snapshot_scratch", die_after_two):
            with self.assertRaises(RuntimeError):
                run_write(
                    workdir,
                    a_list_path=a_list,
                    simulation_info_path=sim_info,
                    consume_intermediates=True,
                )
        manifest = Manifest.load_or_create(workdir)
        consumed = links._consumed_fixed_snapshots(
            manifest, sorted(int(s) for s in manifest.data["snapshots"])
        )
        self.assertTrue(consumed, "the interrupted writer must have consumed at least one")
        self.assertLess(len(consumed), len(manifest.data["snapshots"]), "and not all of them")

        with capture_stderr() as captured:
            run_links(workdir, consume_intermediates=True)
        self.assertIn("skipping the rank pass", captured.text)
        reloaded = Manifest.load_or_create(workdir)
        for name, path in expected.items():
            self.assertFalse(path.exists(), "{} survived the short-circuit".format(name))
            self.assertEqual(
                "removed", reloaded.data["intermediates"][str(path.resolve())]["status"], name
            )

    def _interrupted_writer_workdir(self, name):
        """links(off) -> a writer run that consumes some fixed files and dies.
        Every snapshot is linked, some fixed files are consumed, and every idx
        and pending buffer is still on disk — the state the short-circuit is
        reached in."""
        root = self.root / name
        root.mkdir()
        workdir, a_list, sim_info = make_linked_workdir(root)
        run_links(workdir)
        real = hdf5_writer._consume_snapshot_scratch
        calls = {"n": 0}

        def die_after_two(manifest, snap, delete):
            real(manifest, snap, delete)
            calls["n"] += 1
            if calls["n"] == 2:
                raise RuntimeError("simulated writer crash")

        with mock.patch.object(hdf5_writer, "_consume_snapshot_scratch", die_after_two):
            with self.assertRaises(RuntimeError):
                run_write(
                    workdir,
                    a_list_path=a_list,
                    simulation_info_path=sim_info,
                    consume_intermediates=True,
                )
        return workdir

    def _surviving_links_file(self, workdir):
        """A links file the interrupted writer did NOT consume."""
        manifest = Manifest.load_or_create(workdir)
        for snap in sorted(int(x) for x in manifest.data["snapshots"]):
            path = Path(manifest.data["snapshots"][str(snap)]["links_file"])
            if not manifest.is_consumed(path):
                return snap, path
        self.fail("every links file was consumed")

    def test_short_circuit_refuses_a_missing_links_successor_and_deletes_nothing(self):
        """The short-circuit never calls ``link_one_snapshot``, so it has to do
        that path's verification itself: delete-after-VERIFY, not
        delete-after-status-check. A links file gone from disk must stop the
        drain, leaving the index and pending buffer it stands behind intact."""
        workdir = self._interrupted_writer_workdir("missing-successor")
        expected = self._pipeline_paths(workdir)
        snap, links_path = self._surviving_links_file(workdir)
        links_path.unlink()
        with self.assertRaisesRegex(ConverterError, "missing on disk"):
            run_links(workdir, consume_intermediates=True)
        manifest = Manifest.load_or_create(workdir)
        for name, path in expected.items():
            self.assertTrue(path.exists(), "{} was deleted behind an unverified successor")
            self.assertEqual(
                "present", manifest.data["intermediates"][str(path.resolve())]["status"], name
            )

    def test_short_circuit_refuses_a_tampered_links_successor_and_deletes_nothing(self):
        """Same guarantee against silent corruption rather than absence."""
        workdir = self._interrupted_writer_workdir("tampered-successor")
        expected = self._pipeline_paths(workdir)
        snap, links_path = self._surviving_links_file(workdir)
        with open(links_path, "r+b") as handle:
            handle.write(b"\x7f\x7f\x7f\x7f")
        with self.assertRaisesRegex(ConverterError, "content checksum"):
            run_links(workdir, consume_intermediates=True)
        manifest = Manifest.load_or_create(workdir)
        for name, path in expected.items():
            self.assertTrue(
                path.exists(), "{} was deleted behind a tampered successor".format(name)
            )
            self.assertEqual(
                "present", manifest.data["intermediates"][str(path.resolve())]["status"], name
            )

    def test_short_circuit_deletes_nothing_with_the_flag_off(self):
        """And the drain honours the flag, like every other deletion here."""
        root = self.root / "short-circuit-off"
        root.mkdir()
        workdir, a_list, sim_info = make_linked_workdir(root)
        run_links(workdir)
        expected = self._pipeline_paths(workdir)
        run_write(
            workdir,
            a_list_path=a_list,
            simulation_info_path=sim_info,
            consume_intermediates=True,
        )
        run_links(workdir)
        for name, path in expected.items():
            self.assertTrue(path.exists(), "{} was deleted with the flag off".format(name))

    def test_mid_set_gap_in_the_recorded_snapshots(self):
        """The no-consumer rule at the boundary the plan makes a point of.

        The standard fixture only exercises a LEADING gap (snapshot 0 empty).
        Here the recorded set is {1, 2, 4, 5} — forest 100 dies at snapshot 2,
        forest 200 does not appear until snapshot 4 — so snapshot 3 is a gap in
        the middle. ``idx_1`` and ``idx_4`` then both have no consumer, because
        neither snapshot 0 nor snapshot 3 is linked, while ``idx_2`` and
        ``idx_5`` are consumed normally by the links of 1 and 4.
        """
        root = self.root / "mid-gap"
        root.mkdir()
        early = fixtures.ForestSpec(
            forest_id=100,
            trees=[
                fixtures.TreeSpec(
                    root_id=101,
                    halos=[
                        fixtures.HaloSpec(halo_id=1010, snap=2, mvir=4.0e11, num_prog=1),
                        fixtures.HaloSpec(halo_id=1011, snap=1, mvir=3.0e11, desc_id=1010),
                    ],
                )
            ],
        )
        late = fixtures.ForestSpec(
            forest_id=200,
            trees=[
                fixtures.TreeSpec(
                    root_id=201,
                    halos=[
                        fixtures.HaloSpec(halo_id=2010, snap=5, mvir=9.0e11, num_prog=1),
                        fixtures.HaloSpec(halo_id=2011, snap=4, mvir=7.0e11, desc_id=2010),
                    ],
                )
            ],
        )
        workdir, _, _ = make_linked_workdir(root, forests=[early, late])
        manifest = Manifest.load_or_create(workdir)
        self.assertEqual([1, 2, 4, 5], sorted(int(s) for s in manifest.data["snapshots"]))

        snaps = [1, 2, 4, 5]
        self.assertEqual(
            [manifest.data["snapshots"][str(s)]["index_file"] for s in (1, 4)],
            links._no_consumer_indexes(manifest, snaps),
        )

        expected = self._paths(workdir)
        seen = []
        real = links.link_one_snapshot

        def spy(manifest_arg, snap, identity):
            seen.append((snap, expected["idx_1"].exists(), expected["idx_4"].exists()))
            return real(manifest_arg, snap, identity)

        with mock.patch.object(links, "link_one_snapshot", spy):
            run_links(workdir, consume_intermediates=True)

        # both no-consumer indexes are gone before the first link, not after
        # some later one
        self.assertEqual([(snap, False, False) for snap, _, _ in seen], seen)
        reloaded = Manifest.load_or_create(workdir)
        for name, path in expected.items():
            entry = reloaded.data["intermediates"][str(path.resolve())]
            if name.startswith("fixed_"):
                self.assertTrue(path.exists(), name)
            else:
                self.assertFalse(path.exists(), "{} survived".format(name))
                self.assertEqual("removed", entry["status"], name)
        # the pending buffers that exist are exactly those across a non-gap
        self.assertEqual(
            ["pending_fp_2", "pending_fp_5"],
            sorted(n for n in expected if n.startswith("pending_fp_")),
        )

    def test_cli_flag_is_off_by_default(self):
        parser = convert_ctrees.build_arg_parser()
        self.assertFalse(parser.parse_args(["links", "--workdir", "w"]).consume_intermediates)
        self.assertTrue(
            parser.parse_args(
                ["links", "--workdir", "w", "--consume-intermediates"]
            ).consume_intermediates
        )


if __name__ == "__main__":
    unittest.main()
