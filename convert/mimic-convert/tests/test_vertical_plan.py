"""The file plan (vertical/plan.py): forest totals, the file cut and ``file_plan.json``.

Oracles, on :mod:`fixtures`' correspondence-valid forests 1100, 1200, 1300 and 1400 converted to
a version 3 dataset (forest 1300's one tree ends at snapshot 2, so that forest has no halo in the
final slab): the forests' halo totals, 8, 6, 2 and 3, typed literally and read at block sizes that
split slabs between a forest's rows; the committed ``shin-uchuu`` ``worked_ascii`` fixture's
totals, 7, 4 and 2 (its forest 30 ends at snapshot 2); literal file cuts for small caps; every
refusal of :func:`forest_totals`, each from a copy of the dataset mutated in one way; the caps'
and totals' refusals of :func:`cut_files`; and the plan file's round trip and refusals.
"""

import json
import shutil
import sys
import tempfile
import unittest
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
REPO_ROOT = HERE.parents[2]
sys.path.insert(0, str(HERE.parent))
sys.path.insert(0, str(HERE))

import fixtures  # noqa: E402
from errors import ConverterError  # noqa: E402
from fixtures import (  # noqa: E402
    convert_ascii,
    edit_column,
)
from horizontal_dataset import HorizontalDataset  # noqa: E402
from vertical.plan import (  # noqa: E402
    FILE_PLAN_NAME,
    INT32_MAX,
    cut_files,
    forest_totals,
    make_file_plan,
    read_file_plan,
    write_file_plan,
)
from vertical.record import record_digest, record_dtype, record_layout  # noqa: E402

SHIN_UCHUU_RECORD = REPO_ROOT / "simulations" / "shin-uchuu-ascii" / "halo_properties.yaml"
MINI_MILLENNIUM_RECORD = REPO_ROOT / "simulations" / "mini-millennium" / "halo_properties.yaml"
WORKED_ASCII = REPO_ROOT / "simulations" / "shin-uchuu" / "_tests" / "data" / "worked_ascii"

#: The fixture's forests by ForestIndex: 1100, 1200, 1300 (ends at snapshot 2) and 1400.
TOTALS = [8, 6, 2, 3]


def cuts(files):
    return [(f.number, f.first_forest, f.last_forest, f.n_forests, f.n_halos) for f in files]


class FixtureDataset(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = Path(tempfile.mkdtemp(prefix="vertical_plan_"))
        forests = fixtures.early_ending_correspondence_forests()
        cls.dataset_dir = convert_ascii(cls.tmp / "source", [fixtures.all_trees(forests)], forests)[
            "dataset"
        ]

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def setUp(self):
        self.work = Path(tempfile.mkdtemp(prefix="vertical_plan_case_"))
        self.addCleanup(shutil.rmtree, self.work, ignore_errors=True)

    def mutated(self, *edits):
        """A copy of the dataset with each ``(snap, column, edit)`` applied."""
        target = Path(tempfile.mkdtemp(dir=self.work)) / "dataset"
        shutil.copytree(self.dataset_dir, target)
        for snap, name, edit in edits:
            edit_column(target, snap, name, edit)
        return HorizontalDataset(target)


class ForestTotalsTests(FixtureDataset):
    def test_totals_at_every_block_size(self):
        dataset = HorizontalDataset(self.dataset_dir)
        self.assertEqual(dataset.forest_ids().tolist(), [1100, 1200, 1300, 1400])
        self.assertEqual(dataset.total_halos, 19)
        for block_rows in (1, 2, 3, 1 << 22):
            with self.subTest(block_rows=block_rows):
                totals = forest_totals(dataset, block_rows)
                self.assertEqual(totals.dtype, np.int64)
                self.assertEqual(totals.tolist(), TOTALS)

    def test_the_committed_worked_ascii_fixture(self):
        dataset = HorizontalDataset(WORKED_ASCII)
        self.assertEqual(dataset.forest_ids().tolist(), [10, 20, 30])
        self.assertEqual(forest_totals(dataset).tolist(), [7, 4, 2])

    def test_a_slab_out_of_forest_order_is_refused(self):
        def swap(values):
            values[1] = 1  # snapshot 5's ForestIndex [0, 0, 0, 1, 1, 3] -> [0, 1, 0, ...]

        dataset = self.mutated((5, "ForestIndex", swap))
        for block_rows in (2, 1 << 22):  # row 2 starts the second block of two rows
            with self.subTest(block_rows=block_rows):
                with self.assertRaisesRegex(
                    ConverterError,
                    r"snapshot_005\.h5: ForestIndex is not non-decreasing: row 2 holds 0 after 1",
                ):
                    forest_totals(dataset, block_rows)

    def test_a_forest_index_outside_the_forest_count_is_refused(self):
        def beyond(values):
            values[-1] = 4

        with self.assertRaisesRegex(
            ConverterError, r"snapshot_003\.h5: row 3 has ForestIndex 4, outside \[0, 4\)"
        ):
            forest_totals(self.mutated((3, "ForestIndex", beyond)))

    def test_a_forest_present_in_no_slab_is_refused(self):
        def relabel(values):
            values[values == 2] = 1

        dataset = self.mutated((1, "ForestIndex", relabel), (2, "ForestIndex", relabel))
        with self.assertRaisesRegex(ConverterError, r"forest 2 \(ForestIndex\) is present in no"):
            forest_totals(dataset)

    def test_minima_that_do_not_increase_are_refused(self):
        def lower(values):
            values[0] = 9  # forest 2's first row at snapshot 2, lowered from 15 to forest 1's 9

        with self.assertRaisesRegex(
            ConverterError, r"forest 2's first SourceHaloID 9 does not exceed forest 1's 9"
        ):
            forest_totals(self.mutated((2, "SourceHaloID", lower)))

    def test_totals_that_are_not_those_of_dense_ids_are_refused(self):
        """The first SourceHaloID of forest 0 must be 1, and the totals must sum to the dataset's
        halo count."""

        def raise_first(values):
            values[0] = 2  # forest 0's rank 0 at snapshot 5

        def shift_last(values):
            values[-1] += 100  # forest 3's row in each slab it occupies

        cases = [
            (
                "a first minimum other than one",
                [(5, "SourceHaloID", raise_first)],
                r"forest 0's first SourceHaloID is 2, not 1 \(the totals need .* dense",
            ),
            (
                "totals that do not sum to the halo total",
                [(snap, "SourceHaloID", shift_last) for snap in (3, 4, 5)],
                r"forest totals do not sum to the dataset's 19 halos: forests 0 to 2 hold 116, "
                r"leaving -97 for forest 3 \(the totals need SourceHaloID to be the dense 1-based",
            ),
        ]
        for label, edits, message in cases:
            with self.subTest(label):
                with self.assertRaisesRegex(ConverterError, message):
                    forest_totals(self.mutated(*edits))


class CutFilesTests(unittest.TestCase):
    def test_literal_cuts(self):
        cases = {
            # a forest above the halo cap alone in its file, then 6 + 2 > 7
            (7, 10): [(0, 0, 0, 1, 8), (1, 1, 1, 1, 6), (2, 2, 3, 2, 5)],
            # the forest cap binds before the halo cap
            (100, 3): [(0, 0, 2, 3, 16), (1, 3, 3, 1, 3)],
            # a file filled exactly to the halo cap
            (14, 10): [(0, 0, 1, 2, 14), (1, 2, 3, 2, 5)],
            (100, 100): [(0, 0, 3, 4, 19)],
            (1, 1): [(0, 0, 0, 1, 8), (1, 1, 1, 1, 6), (2, 2, 2, 1, 2), (3, 3, 3, 1, 3)],
        }
        for (halo_cap, forest_cap), expected in cases.items():
            with self.subTest(halo_cap=halo_cap, forest_cap=forest_cap):
                self.assertEqual(cuts(cut_files(np.array(TOTALS), halo_cap, forest_cap)), expected)
        self.assertEqual(cut_files([], 10, 10), [])

    def test_caps_and_totals_are_accepted_at_the_int_limit_and_refused_beyond_it(self):
        """INT32_MAX itself is a valid cap and total; a zero, boolean or over-limit cap, an empty
        or over-limit forest, and a non-integer array are refused by name."""
        self.assertEqual(
            cuts(cut_files([INT32_MAX, 1], INT32_MAX, INT32_MAX)),
            [(0, 0, 0, 1, INT32_MAX), (1, 1, 1, 1, 1)],
        )
        refusals = [
            ((TOTALS, INT32_MAX + 1, 10), r"max_halos_per_file must be at most 2147483647, got"),
            ((TOTALS, 10, INT32_MAX + 1), r"max_forests_per_file must be at most 2147483647"),
            ((TOTALS, 0, 10), r"max_halos_per_file must be a positive integer, got 0"),
            ((TOTALS, 10, True), r"max_forests_per_file must be a positive integer, got True"),
            (([1, INT32_MAX + 1], INT32_MAX, 10), r"forest 1 has 2147483648 halos, more than"),
            (([3, 0, 2], 10, 10), r"forest 1 has 0 halos"),
            (([1.5], 10, 10), r"one-dimensional integer array"),
        ]
        for args, message in refusals:
            with self.subTest(message=message):
                with self.assertRaisesRegex(ConverterError, message):
                    cut_files(*args)


class FilePlanTests(FixtureDataset):
    def plan(self, halo_properties=SHIN_UCHUU_RECORD):
        dataset = HorizontalDataset(self.dataset_dir)
        return dataset, make_file_plan(dataset, halo_properties, 7, 10, block_rows=2)

    def test_round_trip(self):
        dataset, plan = self.plan()
        path = write_file_plan(self.work, plan)
        self.assertEqual(path, self.work / FILE_PLAN_NAME)
        self.assertEqual(read_file_plan(self.work, dataset, SHIN_UCHUU_RECORD), plan)
        self.assertEqual(
            plan["dataset"],
            {
                "forest_id_sha256": dataset.forest_id_sha256(),
                "n_forests_total": 4,
                "total_halos": 19,
                "column_mapping_sha256": dataset.header(0)["column_mapping_sha256"],
            },
        )
        self.assertEqual(plan["caps"], {"max_halos_per_file": 7, "max_forests_per_file": 10})
        self.assertEqual(
            plan["record"]["digest"], record_digest(record_layout(record_dtype(SHIN_UCHUU_RECORD)))
        )
        self.assertEqual(plan["record"]["halo_properties"], str(SHIN_UCHUU_RECORD))
        self.assertEqual(len(plan["record"]["halo_properties_md5"]), 32)
        self.assertEqual(
            plan["files"],
            [
                {"number": 0, "first_forest": 0, "last_forest": 0, "n_forests": 1, "n_halos": 8},
                {"number": 1, "first_forest": 1, "last_forest": 1, "n_forests": 1, "n_halos": 6},
                {"number": 2, "first_forest": 2, "last_forest": 3, "n_forests": 2, "n_halos": 5},
            ],
        )

    def test_the_yaml_path_and_md5_are_information_only(self):
        dataset, plan = self.plan()
        write_file_plan(self.work, plan)
        copy = self.work / "elsewhere.yaml"
        copy.write_text(
            "# a comment changes the md5, not the record\n" + SHIN_UCHUU_RECORD.read_text()
        )
        self.assertEqual(read_file_plan(self.work, dataset, copy), plan)

    def test_refusals(self):
        dataset, plan = self.plan()

        def edited(edit):
            changed = json.loads(json.dumps(plan))
            edit(changed)
            return changed

        def set_mapping(changed):
            changed["dataset"]["column_mapping_sha256"] = "0" * 64

        def stretch_file(changed):
            changed["files"][1]["last_forest"] = 2

        def bump_version(changed):
            changed["version"] = 2

        cases = [
            (
                "another dataset",
                plan,
                HorizontalDataset(WORKED_ASCII),
                SHIN_UCHUU_RECORD,
                r"whose forest_id_sha256 is",
            ),
            (
                "another record",
                plan,
                dataset,
                MINI_MILLENNIUM_RECORD,
                r"planned for the record with digest [0-9a-f]{64}; the record derived now has [0-9a-f]{64}",
            ),
            (
                "another mapping",
                edited(set_mapping),
                dataset,
                SHIN_UCHUU_RECORD,
                r"whose column_mapping_sha256 is '0000",
            ),
            (
                "inconsistent files",
                edited(stretch_file),
                dataset,
                SHIN_UCHUU_RECORD,
                r"file entry 1 is inconsistent",
            ),
            (
                "another version",
                edited(bump_version),
                dataset,
                SHIN_UCHUU_RECORD,
                r"not a mimic-vertical-file-plan version 1 file",
            ),
        ]
        for label, written, open_dataset, record, message in cases:
            with self.subTest(label):
                write_file_plan(self.work, written)
                with self.assertRaisesRegex(ConverterError, message):
                    read_file_plan(self.work, open_dataset, record)
        with self.assertRaisesRegex(ConverterError, r"cannot read the file plan"):
            read_file_plan(self.work / "absent", dataset, SHIN_UCHUU_RECORD)

    def test_malformed_and_over_limit_entries_are_refused_by_name(self):
        """Every entry field must be a plain integer, and no entry may pass the reader's int
        limit or the plan's recorded caps (7 halos, 10 forests); a lone forest above the halo
        cap is the plan's file 0 and stays valid."""
        dataset, plan = self.plan()

        def edited(edit):
            changed = json.loads(json.dumps(plan))
            edit(changed)
            return changed

        def entry(index, **fields):
            return lambda changed: changed["files"][index].update(fields)

        cases = [
            ("a float field", entry(1, n_halos=6.0), r"file entry 1 is malformed"),
            ("a bool field", entry(0, number=False), r"file entry 0 is malformed"),
            ("a string field", entry(2, last_forest="3"), r"file entry 2 is malformed"),
            ("a missing field", lambda c: c["files"][1].pop("n_forests"), r"file entry 1 is malf"),
            ("a halo count past int32", entry(0, n_halos=INT32_MAX + 1), r"file entry 0 exceeds"),
            (
                "more forests than the cap",
                lambda c: c["caps"].update(max_forests_per_file=1),
                r"file entry 2 exceeds a limit \(at most 1 forests per file, 7 halos",
            ),
            (
                "more halos than the cap in several forests",
                lambda c: c["caps"].update(max_halos_per_file=4),
                r"file entry 2 exceeds a limit \(at most 10 forests per file, 4 halos",
            ),
            (
                "a float cap",
                lambda c: c["caps"].update(max_halos_per_file=7.0),
                r"caps max_halos_per_file must be a positive integer, got 7.0",
            ),
        ]
        for label, edit, message in cases:
            with self.subTest(label):
                write_file_plan(self.work, edited(edit))
                with self.assertRaisesRegex(ConverterError, message):
                    read_file_plan(self.work, dataset, SHIN_UCHUU_RECORD)
        write_file_plan(self.work, plan)
        self.assertEqual(read_file_plan(self.work, dataset, SHIN_UCHUU_RECORD), plan)

    def test_make_file_plan_refuses_a_bad_cap_before_reading_the_dataset(self):
        """A dataset that cannot be touched at all is never reached: the cap is checked first."""
        with self.assertRaisesRegex(ConverterError, r"max_forests_per_file must be a positive"):
            make_file_plan(object(), SHIN_UCHUU_RECORD, 10, 0)


if __name__ == "__main__":
    unittest.main()
