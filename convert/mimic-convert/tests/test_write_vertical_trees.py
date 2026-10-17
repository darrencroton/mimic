"""The vertical converter's command line (write_vertical_trees.py, vertical/writer.py).

The dataset is :mod:`fixtures`' correspondence forests 1100, 1200, 1300 and 1400 (``ForestIndex``
0 to 3, 8, 6, 2 and 3 halos) converted to version 3 and planned at 8 halos per file, which cuts
three files: forest 0 alone, forests 1 and 2, and forest 3. Every written file is re-read here
independently, with numpy and the derived record dtype, and compared with the forests' literal
oracles (MostBoundID, the five forest-local links and SnapNum by rank) and with the assembler's
records byte for byte. Interruptions are simulated by patched failures: a kill after a file's
``.partial`` is written, a kill between a file's rename and its manifest entry, and a file
corrupted between its write and its verification by a patched re-read.
"""

import contextlib
import hashlib
import io
import json
import os
import shutil
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

import h5py
import numpy as np

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
sys.path.insert(0, str(HERE))

import fixtures  # noqa: E402
import write_vertical_trees  # noqa: E402
from errors import ConverterError  # noqa: E402
from horizontal_dataset import HorizontalDataset  # noqa: E402
from test_vertical_assemble import (  # noqa: E402
    FOREST_1100,
    FOREST_1200,
    FOREST_1300,
    FOREST_1400,
    SHIN_UCHUU_RECORD,
    WORKED_ASCII,
    convert,
    rows,
)
from test_vertical_plan import edit_column  # noqa: E402
from vertical import writer  # noqa: E402
from vertical.assemble import AssembledForest, ForestAssembler  # noqa: E402
from vertical.record import record_digest, record_dtype, record_layout  # noqa: E402

TREE = "trees"
RECORD = record_dtype(SHIN_UCHUU_RECORD)
ORACLES = [FOREST_1100, FOREST_1200, FOREST_1300, FOREST_1400]

#: The plan at 8 halos per file: (first forest, last forest, the forests' halo counts).
FILES = [(0, 0, [8]), (1, 2, [6, 2]), (3, 3, [3])]


def run(*argv):
    """``write_vertical_trees.main(argv)``: its exit status, stdout and stderr."""
    out, err = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        code = write_vertical_trees.main([str(arg) for arg in argv])
    return code, out.getvalue(), err.getvalue()


def read_file(path):
    """An L-Halo file as (ntrees, totNHalos, counts, records), read independently."""
    data = Path(path).read_bytes()
    ntrees, total = np.frombuffer(data[:8], dtype="<i4")
    head = 8 + 4 * int(ntrees)
    counts = np.frombuffer(data[8:head], dtype="<i4")
    return int(ntrees), int(total), counts.tolist(), np.frombuffer(data[head:], dtype=RECORD)


def split(records, counts):
    """A file's records as one record array per forest."""
    bounds = np.concatenate(([0], np.cumsum(counts)))
    return [records[bounds[i] : bounds[i + 1]] for i in range(len(counts))]


class ConverterCase(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = Path(tempfile.mkdtemp(prefix="write_vertical_"))
        cls.dataset_dir = convert(
            cls.tmp / "source", fixtures.early_ending_correspondence_forests()
        )

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def setUp(self):
        self.work = Path(tempfile.mkdtemp(prefix="write_vertical_case_"))
        self.addCleanup(shutil.rmtree, self.work, ignore_errors=True)
        self.output = self.work / "out"

    def plan(self, output=None, dataset=None, *extra):
        code, out, err = run(
            "plan",
            "--input",
            dataset or self.dataset_dir,
            "--halo-properties",
            SHIN_UCHUU_RECORD,
            "--output",
            output or self.output,
            "--max-halos-per-file",
            8,
            *extra,
        )
        self.assertEqual(code, 0, err)
        return out

    def convert(self, *extra, output=None, dataset=None, tree=TREE):
        return run(
            "convert",
            "--input",
            dataset or self.dataset_dir,
            "--halo-properties",
            SHIN_UCHUU_RECORD,
            "--output",
            output or self.output,
            "--tree-name",
            tree,
            *extra,
        )

    def manifest(self, output=None):
        return json.loads(((output or self.output) / writer.MANIFEST_NAME).read_text())

    def entries(self, output=None):
        return {entry["number"]: entry for entry in self.manifest(output)["files"]}

    def path(self, number, output=None):
        return (output or self.output) / "{}.{}".format(TREE, number)


class RoundTripTests(ConverterCase):
    def test_plan_then_convert_reproduces_every_forest(self):
        out = self.plan()
        self.assertIn("dataset: 19 halos in 4 forests over 6 slabs", out)
        self.assertIn("files: 3", out)
        self.assertIn("largest file: 0, 8 halos in 1 forests", out)

        code, out, err = self.convert()
        self.assertEqual(code, 0, err + out)
        assembler = ForestAssembler(HorizontalDataset(self.dataset_dir), SHIN_UCHUU_RECORD)
        bound = writer.worker_memory_bound(assembler, 8, 2)
        self.assertEqual(
            bound["total"], assembler.memory_bound(8) + (64 << 20) + (8 << 20) + 12 * 2
        )
        self.assertIn("memory bound per worker: {} bytes".format(bound["total"]), out)

        plan = json.loads((self.output / "file_plan.json").read_text())
        assembled = list(assembler.forests(0, 4))
        forests = []
        for number, (first, last, counts) in enumerate(FILES):
            ntrees, total, header_counts, records = read_file(self.path(number))
            planned = plan["files"][number]
            self.assertEqual((ntrees, total), (planned["n_forests"], planned["n_halos"]))
            self.assertEqual((first, last), (planned["first_forest"], planned["last_forest"]))
            self.assertEqual(header_counts, counts)
            forests += split(records, counts)
        self.assertEqual([rows(AssembledForest(0, forest)) for forest in forests], ORACLES)
        for written, forest in zip(forests, assembled):
            self.assertEqual(written.tobytes(), forest.records.tobytes())

        manifest = self.manifest()
        self.assertEqual(manifest["tree_name"], TREE)
        self.assertEqual(manifest["dataset"]["n_forests_total"], 4)
        self.assertEqual(manifest["dataset"]["total_halos"], 19)
        self.assertEqual(manifest["dataset"]["links_adjacent"], 1)
        self.assertEqual(manifest["record"]["digest"], record_digest(record_layout(RECORD)))
        self.assertEqual(
            manifest["plan"]["md5"],
            hashlib.md5((self.output / "file_plan.json").read_bytes()).hexdigest(),
        )
        self.assertEqual(
            manifest["runs"][0]["command_line"][:2], ["write_vertical_trees.py", "convert"]
        )
        for number, entry in self.entries().items():
            data = self.path(number).read_bytes()
            self.assertEqual(entry["sha256"], hashlib.sha256(data).hexdigest())
            self.assertEqual(entry["bytes"], len(data))
            self.assertEqual(entry["bytes"], 8 + 4 * entry["n_forests"] + 88 * entry["n_halos"])
            self.assertEqual(entry["run"], 0)
        self.assertEqual(sorted(self.entries()), [0, 1, 2])

    def test_two_workers_write_the_same_bytes_as_one(self):
        one, two = self.work / "one", self.work / "two"
        self.plan(one)
        self.plan(two)
        self.assertEqual(self.convert(output=one)[0], 0)
        code, out, err = self.convert("--workers", 2, output=two)
        self.assertEqual(code, 0, err + out)
        for number in range(3):
            self.assertEqual(
                self.path(number, one).read_bytes(), self.path(number, two).read_bytes()
            )
            self.assertEqual(
                self.entries(one)[number]["sha256"], self.entries(two)[number]["sha256"]
            )
        self.assertNotIn(os.getpid(), {entry["worker"] for entry in self.entries(two).values()})

    def test_only_files(self):
        self.plan()
        self.assertEqual(self.convert("--only-files", "0,2")[0], 0)
        self.assertEqual(sorted(self.entries()), [0, 2])
        self.assertFalse(self.path(1).exists())
        before = self.entries()[2]
        code, out, _ = self.convert("--only-files", "1-2")
        self.assertEqual(code, 0)
        self.assertIn("{}.2 matches its manifest entry; skipped".format(TREE), out)
        self.assertEqual(sorted(self.entries()), [0, 1, 2])
        self.assertEqual(self.entries()[2], before)
        for selection, message in (("3", r"the plan's files are 0 to 2"), ("1,x", "'x'")):
            code, _, err = self.convert("--only-files", selection)
            self.assertEqual(code, 2)
            self.assertRegex(err, message)


class ResumeTests(ConverterCase):
    def test_an_interrupted_run_resumes(self):
        """A kill after file 1's ``.partial`` is written: the rerun keeps file 0's entry as it
        was, moves the ``.partial`` aside and rebuilds file 1."""
        self.plan()
        real = writer._verify
        calls = []

        def killed_on_the_second_file(*args):
            calls.append(args)
            if len(calls) == 2:
                raise RuntimeError("killed")
            return real(*args)

        with mock.patch.object(writer, "_verify", killed_on_the_second_file):
            with self.assertRaisesRegex(RuntimeError, "killed"):
                self.convert()
        self.assertEqual(sorted(self.entries()), [0])
        first = self.entries()[0]
        partial = self.output / "{}.1.partial".format(TREE)
        self.assertTrue(partial.exists())

        code, out, err = self.convert()
        self.assertEqual(code, 0, err + out)
        self.assertEqual(self.entries()[0], first)
        self.assertIn("{}.0 matches its manifest entry; skipped".format(TREE), out)
        self.assertRegex(
            out, r"moved aside \S+\.1\.partial \(720 bytes\) to trees\.1\.partial\.stale-"
        )
        (stale,) = self.output.glob("{}.1.partial.stale-*".format(TREE))
        self.assertEqual(stale.read_bytes(), self.path(1).read_bytes())
        self.assertEqual(sorted(self.entries()), [0, 1, 2])
        self.assertEqual([entry["run"] for entry in self.manifest()["files"]], [0, 1, 1])

    def test_a_kill_between_the_rename_and_the_manifest_entry(self):
        """File 1 renamed but never recorded: the rerun moves it aside and rebuilds it, byte
        for byte the same."""
        self.plan()
        real = writer.write_json_atomic
        calls = []

        def killed_on_the_third_save(*args):
            calls.append(args)
            if len(calls) == 3:  # the run's start, file 0, then file 1
                raise RuntimeError("killed")
            return real(*args)

        with mock.patch.object(writer, "write_json_atomic", killed_on_the_third_save):
            with self.assertRaisesRegex(RuntimeError, "killed"):
                self.convert()
        self.assertTrue(self.path(1).exists())
        self.assertEqual(sorted(self.entries()), [0])
        orphan = self.path(1).read_bytes()

        code, out, err = self.convert()
        self.assertEqual(code, 0, err + out)
        self.assertRegex(out, r"moved aside \S+/trees\.1 \(720 bytes\) to trees\.1\.stale-")
        (stale,) = self.output.glob("{}.1.stale-*".format(TREE))
        self.assertEqual(stale.read_bytes(), orphan)
        self.assertEqual(self.path(1).read_bytes(), orphan)
        self.assertEqual(sorted(self.entries()), [0, 1, 2])


class RefusalTests(ConverterCase):
    def test_refusals_before_any_row_is_read(self):
        planned = self.work / "planned"
        self.plan(planned)
        other = self.work / "other"
        self.plan(other, WORKED_ASCII)
        version_2 = self.work / "version_2"
        shutil.copytree(self.dataset_dir, version_2)
        for path in version_2.glob("snapshot_*.h5"):
            with h5py.File(path, "r+") as handle:
                handle["header"].attrs["format_version"] = 2
        renamed = self.work / "renamed"
        self.plan(renamed)
        self.assertEqual(self.convert("--only-files", "2", output=renamed, tree="old")[0], 0)

        cases = [
            ("a missing plan", {"output": self.work / "empty"}, (), r"cannot read the file plan"),
            (
                "a plan of another dataset",
                {"output": other},
                (),
                r"planned for a dataset whose forest_id_sha256",
            ),
            (
                "a tree name with a path separator",
                {"output": planned, "tree": "a/b"},
                (),
                r"without a path separator",
            ),
            (
                "a dataset that is not version 3",
                {"output": planned, "dataset": version_2},
                (),
                r"format_version 2 is not supported",
            ),
            (
                "a manifest of another conversion",
                {"output": renamed},
                (),
                r"records a conversion whose tree_name is 'old'",
            ),
        ]
        refuse = mock.patch.multiple(
            HorizontalDataset,
            read_rows=mock.DEFAULT,
            iter_columns=mock.DEFAULT,
        )
        for name, where, extra, message in cases:
            with self.subTest(name), refuse as reads:
                code, out, err = self.convert(*extra, **where)
                self.assertEqual(code, 2, out)
                self.assertRegex(err, message)
                self.assertFalse(reads["read_rows"].called or reads["iter_columns"].called)
        for option in ("--workers", "--window-rows"):
            with self.subTest(option), refuse as reads:
                with self.assertRaises(SystemExit) as raised:
                    self.convert(option, 0, output=planned)
                self.assertEqual(raised.exception.code, 2)
                self.assertFalse(reads["read_rows"].called or reads["iter_columns"].called)
            with self.assertRaisesRegex(ConverterError, "must be a positive integer"):
                writer.convert(
                    self.dataset_dir,
                    SHIN_UCHUU_RECORD,
                    planned,
                    TREE,
                    **{option[2:].replace("-", "_"): 0},
                )

    def test_a_forest_whose_halos_differ_from_its_planned_total(self):
        """Forest 1's first SourceHaloID raised from 9 to 10: the totals then give forest 0 nine
        halos and forest 1 five, which its assembly refuses."""
        dataset = self.work / "dataset"
        shutil.copytree(self.dataset_dir, dataset)
        edit_column(dataset, 5, "SourceHaloID", lambda values: values.__setitem__(values == 9, 10))
        self.plan(dataset=dataset)
        code, out, err = self.convert(dataset=dataset)
        self.assertEqual(code, 2)
        self.assertIn("forest 0 has 8 halos, but the plan expects 9", out)
        self.assertFalse(self.path(0).exists())


class VerificationTests(ConverterCase):
    def test_a_file_corrupted_before_its_verification(self):
        """File 1's re-read sees one payload byte flipped: it fails by name and stays
        ``.partial``, while files 0 and 2 complete and are recorded."""
        self.plan()
        real = writer._reread

        def corrupted(path, *args):
            for block in real(path, *args):
                if Path(path).name == "{}.1.partial".format(TREE):
                    block = block[:-1] + bytes([block[-1] ^ 0xFF])
                yield block

        with mock.patch.object(writer, "_reread", corrupted):
            code, out, err = self.convert()
        self.assertEqual(code, 1, err)
        self.assertRegex(out, r"trees\.1\.partial: verification failed \(sha256\)")
        self.assertFalse(self.path(1).exists())
        self.assertTrue((self.output / "{}.1.partial".format(TREE)).exists())
        self.assertEqual(sorted(self.entries()), [0, 2])


class HelpTests(unittest.TestCase):
    def test_help_states_the_bound_the_exit_codes_and_the_lone_forest(self):
        for command in ("plan", "convert"):
            with self.subTest(command):
                out = io.StringIO()
                with contextlib.redirect_stdout(out), self.assertRaises(SystemExit):
                    write_vertical_trees.main([command, "--help"])
                text = " ".join(out.getvalue().split())
                self.assertIn("(2*S*R + L)*B + max(R, L)*(B + b + 176) bytes", text)
                self.assertIn("a forest larger than the halo cap occupies its file alone", text)
                self.assertIn("Exit status: 0 when every selected file is complete; 1 when", text)
                self.assertIn("2 on a refusal", text)


if __name__ == "__main__":
    unittest.main()
