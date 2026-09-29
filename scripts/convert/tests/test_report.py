"""Converter generalisation Slice 8 unit tests: the format version 3
conversion report (``report.run_report_v3``).

The report is built from a real conversion of ``test_pipeline``'s literal
gapped L-Halo source with the extras-example profile, so every count it
states is checked against the hand-derived graph (``test_pipeline.EXPECTED``:
two gapped descendants, the longer spanning three snapshots) rather than
against the battery's own arithmetic. The legacy v2 report is covered by
``test_hdf5_writer.TestReport`` and is unchanged.
"""

import json
import os
import shutil
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

import h5py

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import report  # noqa: E402
import runtime_routes  # noqa: E402
from column_schema import ConverterError  # noqa: E402
from report import REPORT_JSON, REPORT_TXT, run_report_v3  # noqa: E402
from test_hdf5_writer import V3_LHALO_EXTRAS_PAYLOAD, make_v3_conversion  # noqa: E402
from validate_v3 import run_battery_v3  # noqa: E402

#: The fixed topology and identity table in format order, with the core
#: role each provides, restated from src/core/core_properties.yaml's
#: required_inputs rather than read from the converter.
FORMAT_TABLE_ROLES = [
    ("Descendant", "long long", "Descendant"),
    ("FirstProgenitor", "long long", "FirstProgenitor"),
    ("NextProgenitor", "long long", "NextProgenitor"),
    ("FirstHaloInFOFgroup", "long long", "FirstHaloInFOFgroup"),
    ("NextHaloInFOFgroup", "long long", "NextHaloInFOFgroup"),
    ("DescendantSnapshot", "int", None),
    ("FirstProgenitorSnapshot", "int", None),
    ("NextProgenitorSnapshot", "int", None),
    ("SourceHaloID", "long long", None),
    ("ForestIndex", "long long", None),
    ("HaloRankInForest", "long long", None),
]

REPO_ROOT = Path(__file__).resolve().parents[3]
BUDGET = 1 << 20


def tree_state(root: Path):
    """Every regular file under ``root`` (symlinks not followed) with its
    size and modification time."""
    state = {}
    for base, _dirs, names in os.walk(root):
        for name in names:
            path = Path(base) / name
            if path.is_symlink():
                continue
            stat = path.stat()
            state[str(path)] = (stat.st_size, stat.st_mtime_ns)
    return state


class TestV3Report(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls._tmp = tempfile.TemporaryDirectory(prefix="v3_report_")
        cls.conv = make_v3_conversion(cls._tmp.name)
        cls.simulations_before = tree_state(REPO_ROOT / "simulations")
        cls.workdir_before = set(os.listdir(cls.conv.work))
        cls.report = run_report_v3(cls.conv.work, cls.conv.a_list, budget_bytes=BUDGET)

    @classmethod
    def tearDownClass(cls):
        cls._tmp.cleanup()

    def test_report_identifies_source_format_version_mapping_and_layout(self):
        report = self.report
        self.assertEqual(report["source"]["source_format"], "lhalo_binary")
        self.assertEqual(report["format"]["declared_format_versions"], [3])
        self.assertEqual(report["format"]["writer"]["format_version"], 3)
        self.assertEqual(report["mapping"]["column_mapping_sha256"], self.conv.schema.digest)
        layout = report["mapping"]["source_layout"]
        self.assertEqual((layout["byte_order"], layout["itemsize"]), ("little", 104))
        selected = {entry["name"] for entry in layout["entries"] if entry["selected"]}
        self.assertIn("SubHalfMass", selected)
        self.assertEqual(report["source"]["inventory"]["total_halos"], 8)

    def test_report_counts_link_gaps_from_the_hand_derived_graph(self):
        links = self.report["links"]
        self.assertEqual(links["gapped_descendants"], 2)
        self.assertEqual(links["max_descendant_span"], 3)
        self.assertEqual(links["transpose_gapped_descendants"], 2)
        self.assertEqual(links["gapped_first_progenitors"], 1)
        self.assertEqual(links["non_null"]["Descendant"], 4)
        self.assertEqual(self.report["format"]["links_adjacent"], 0)
        self.assertEqual(self.report["format"]["links_adjacent_measured"], 0)

    def test_report_states_totals_and_index_bounds(self):
        report = self.report
        self.assertEqual(report["totals"]["halos"], 8)
        self.assertEqual(report["totals"]["snapshots"], 4)
        self.assertEqual(report["totals"]["snapshots_with_halos"], 3)
        self.assertEqual(report["totals"]["n_forests_total"], 3)
        self.assertEqual([entry["halos"] for entry in report["per_snapshot"]], [2, 0, 3, 3])
        bounds = report["index_bounds"]
        self.assertEqual((bounds["source_halo_id_min"], bounds["source_halo_id_max"]), (1, 8))
        self.assertEqual(bounds["max_forest_index"], 2)
        self.assertEqual(bounds["max_halo_rank_in_forest"], 3)
        self.assertEqual(bounds["max_snapshot_halos"], 3)
        self.assertFalse(bounds["any_index_above_int32"])
        self.assertEqual(report["identity_multiplier"]["recommended"], 10**9)

    def test_report_records_measured_resources(self):
        resources = self.report["resources"]
        dataset = Path(self.report["dataset_dir"])
        on_disk = sum(path.stat().st_size for path in dataset.iterdir())
        self.assertEqual(resources["emitted_bytes"], on_disk)
        self.assertAlmostEqual(resources["emitted_bytes_per_halo"], on_disk / 8)
        transpose = self.conv.manifest.stage("transpose")["result"]
        self.assertEqual(
            resources["transpose"]["peak_resident_bytes"], transpose["peak_resident_bytes"]
        )
        self.assertEqual(resources["transpose"]["peak_spill_bytes"], transpose["peak_spill_bytes"])
        self.assertEqual(resources["validation"]["budget_bytes"], BUDGET)
        self.assertLessEqual(resources["validation"]["peak_resident_bytes"], BUDGET)
        self.assertGreater(resources["record_itemsize"]["transposed"], 0)

    def test_report_states_format_capability_and_only_the_evidenced_routes(self):
        runtime = self.report["runtime_compatibility"]
        # The flag means "format consumed", never "route validated".
        self.assertIs(runtime["runnable_by_current_mimic"], True)
        joined = " ".join(runtime["limitations"])
        self.assertIn("Format consumed, route not validated", joined)
        for sentence in runtime_routes.standing_limitations():
            self.assertIn(sentence, joined)
        self.assertNotIn("accepts only format_version 2", joined)
        self.assertIn("2 Descendant link(s) skip snapshots", joined)
        self.assertIn("SubHalfMass", joined)
        text = (self.conv.work / REPORT_TXT).read_text()
        self.assertIn("FORMAT CONSUMED BY THE CURRENT MIMIC; ROUTE NOT VALIDATED", text)
        self.assertNotIn("NOT RUNNABLE BY THE CURRENT MIMIC", text)
        self.assertIn("format consumed by the current Mimic: YES", text)
        self.assertIn("INSUFFICIENT for runtime execution", text)

    def test_consumer_fragment_has_native_units_types_and_core_roles(self):
        section = self.report["consumer_metadata_fragment"]
        self.assertIs(section["sufficient_for_runtime_execution"], False)
        self.assertIs(section["simulation_packages_written"], False)
        self.assertIn("Insufficient to enable runtime execution", section["label"])
        self.assertIn("reader-owned target-snapshot and SourceHaloID arrays", section["label"])
        self.assertNotIn("a version 3 reader", section["label"])
        text = (self.conv.work / REPORT_TXT).read_text()
        self.assertIn(section["label"], text)
        fragment = section["fragment"]
        self.assertIs(fragment["complete"], False)
        self.assertEqual(fragment["column_mapping_sha256"], self.conv.schema.digest)
        entries = {entry["name"]: entry for entry in fragment["halo_properties"]}
        self.assertEqual(set(entries), set(V3_LHALO_EXTRAS_PAYLOAD))
        for name, (type_name, units, _dtype, _vec) in V3_LHALO_EXTRAS_PAYLOAD.items():
            self.assertEqual((entries[name]["type"], entries[name]["units"]), (type_name, units))
        self.assertEqual(entries["M_Crit200"]["provides_core_role"], "HaloMass")
        self.assertEqual(entries["Len"]["provides_core_role"], "Len")
        self.assertEqual(entries["SnapNum"]["provides_core_role"], "SnapNum")
        self.assertIsNone(entries["SubHalfMass"]["provides_core_role"])
        self.assertIsNone(entries["MostBoundID"]["provides_core_role"])

    def test_consumer_fragment_lists_the_format_table_fields_with_their_core_roles(self):
        fragment = self.report["consumer_metadata_fragment"]["fragment"]
        self.assertEqual(
            [
                (entry["name"], entry["type"], entry["provides_core_role"])
                for entry in fragment["format_table_fields"]
            ],
            FORMAT_TABLE_ROLES,
        )
        text = (self.conv.work / REPORT_TXT).read_text()
        self.assertRegex(
            text,
            r"NextHaloInFOFgroup +long long +\(format table\) +core role: " r"NextHaloInFOFgroup",
        )

    def test_report_is_written_only_into_the_workdir(self):
        self.assertEqual(
            set(os.listdir(self.conv.work)) - self.workdir_before, {REPORT_JSON, REPORT_TXT}
        )
        self.assertEqual(json.loads((self.conv.work / REPORT_JSON).read_text()), self.report)
        self.assertEqual(tree_state(REPO_ROOT / "simulations"), self.simulations_before)

    def test_every_battery_outcome_is_reported_and_passes(self):
        self.assertTrue(self.report["validation_passed"])
        self.assertEqual(len(self.report["validation"]), 20)
        self.assertEqual({o["status"] for o in self.report["validation"]}, {"PASS"})


class TestV3ReportFailures(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="v3_report_fail_"))
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)

    def test_a_failing_dataset_gets_a_failing_report(self):
        conv = make_v3_conversion(self.tmp)
        with h5py.File(conv.dataset / "snapshot_002.h5", "r+") as handle:
            handle["halos"]["Len"][0] = -3
        report = run_report_v3(conv.work, conv.a_list, budget_bytes=BUDGET)
        self.assertFalse(report["validation_passed"])
        failing = {o["name"] for o in report["validation"] if o["status"] == "FAIL"}
        self.assertEqual(failing, {"len-nonnegative", "manifest-binding"})
        self.assertIn("validation: FAIL", (conv.work / REPORT_TXT).read_text())

    def test_a_structurally_failed_dataset_still_gets_a_failing_report(self):
        conv = make_v3_conversion(self.tmp)
        with h5py.File(conv.dataset / "snapshot_002.h5", "r+") as handle:
            handle.create_group("extra")
        report_v3 = run_report_v3(conv.work, conv.a_list, budget_bytes=BUDGET)
        self.assertFalse(report_v3["validation_passed"])
        statuses = {o["name"]: o["status"] for o in report_v3["validation"]}
        self.assertEqual(statuses["object-set"], "FAIL")
        self.assertEqual(statuses["row-values"], "SKIP")
        self.assertEqual(statuses["position-bounds"], "SKIP")
        self.assertIn("validation: FAIL", (conv.work / REPORT_TXT).read_text())
        self.assertEqual(json.loads((conv.work / REPORT_JSON).read_text()), report_v3)

    def test_an_unmeasured_format_is_not_reported_as_consumed(self):
        conv = make_v3_conversion(self.tmp)
        with h5py.File(conv.dataset / "snapshot_002.h5", "r+") as handle:
            handle.create_group("extra")
        report_v3 = run_report_v3(conv.work, conv.a_list, budget_bytes=BUDGET)
        self.assertIsNone(report_v3["format"]["declared_format_versions"])
        self.assertIs(report_v3["runtime_compatibility"]["runnable_by_current_mimic"], False)
        text = (conv.work / REPORT_TXT).read_text()
        self.assertIn("FORMAT VERSION UNMEASURED; NOT CONFIRMED AS CONSUMED", text)
        self.assertNotIn("FORMAT CONSUMED BY THE CURRENT MIMIC", text)
        self.assertIn("format consumed by the current Mimic: NO", text)

    def test_a_format_other_than_3_is_reported_as_not_consumed(self):
        conv = make_v3_conversion(self.tmp)
        battery = run_battery_v3(
            conv.dataset, conv.a_list, manifest_path=conv.manifest.path, budget_bytes=BUDGET
        )
        battery.measurements["format_versions"] = [2]
        report_v3 = run_report_v3(conv.work, conv.a_list, battery=battery)
        self.assertIs(report_v3["runtime_compatibility"]["runnable_by_current_mimic"], False)
        text = (conv.work / REPORT_TXT).read_text()
        self.assertIn(
            "FORMAT NOT CONSUMED BY THE CURRENT MIMIC (declared format version(s) [2])", text
        )
        self.assertNotIn("FORMAT CONSUMED BY THE CURRENT MIMIC;", text)
        self.assertIn("format consumed by the current Mimic: NO", text)

    def test_a_supplied_battery_result_is_reported_without_running_the_battery_again(self):
        conv = make_v3_conversion(self.tmp)
        with h5py.File(conv.dataset / "snapshot_002.h5", "r+") as handle:
            handle["halos"]["Len"][0] = -3
        battery = run_battery_v3(
            conv.dataset, conv.a_list, manifest_path=conv.manifest.path, budget_bytes=BUDGET
        )
        with mock.patch.object(report, "run_battery_v3") as spy:
            report_v3 = run_report_v3(conv.work, conv.a_list, battery=battery)
        spy.assert_not_called()
        self.assertEqual(report_v3["validation"], [o.as_dict() for o in battery.outcomes])
        self.assertFalse(report_v3["validation_passed"])
        self.assertEqual(report_v3["resources"]["validation"]["budget_bytes"], BUDGET)

    def test_an_incomplete_conversion_is_refused(self):
        conv = make_v3_conversion(self.tmp, write=False)
        with self.assertRaisesRegex(ConverterError, "write"):
            run_report_v3(conv.work, conv.a_list, budget_bytes=BUDGET)
        self.assertFalse((conv.work / REPORT_JSON).exists())


if __name__ == "__main__":
    unittest.main()
