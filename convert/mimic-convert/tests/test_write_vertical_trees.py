"""The vertical converter's command line (write_vertical_trees.py, vertical/writer.py).

The dataset is :mod:`fixtures`' correspondence forests 1100, 1200, 1300 and 1400 (``ForestIndex``
0 to 3, 8, 6, 2 and 3 halos) converted to version 3 and planned at 8 halos per file, which cuts
three files: forest 0 alone, forests 1 and 2, and forest 3. Every written file is re-read here
independently, with numpy and the derived record dtype, and compared with the forests' literal
oracles (MostBoundID, the five forest-local links and SnapNum by rank) and with the assembler's
records byte for byte. Interruptions are simulated by patched failures: a kill after a file's
``.partial`` is written, a kill between a file's rename and its manifest entry, a file
corrupted between its write and its verification by a patched re-read, a worker pool that
breaks, and interrupts raised in the parent's wait for its workers or by a real SIGTERM.
"""

import contextlib
import errno
import fcntl
import hashlib
import io
import json
import os
import shutil
import signal
import sys
import tempfile
import types
import unittest
from concurrent.futures import Future
from concurrent.futures.process import BrokenProcessPool
from pathlib import Path
from unittest import mock

import h5py
import numpy as np

HERE = Path(__file__).resolve().parent
REPO_ROOT = HERE.parents[2]
sys.path.insert(0, str(HERE.parent))
sys.path.insert(0, str(HERE))

import fixtures  # noqa: E402
import write_vertical_trees  # noqa: E402
from errors import ConverterError  # noqa: E402
from fixtures import (  # noqa: E402
    FOREST_1100,
    FOREST_1200,
    FOREST_1300,
    FOREST_1400,
    SHIN_UCHUU_RECORD,
    WORKED_ASCII,
    assembled_rows,
    convert_one_file_dataset,
    edit_column,
)
from horizontal_dataset import HorizontalDataset  # noqa: E402
from vertical import writer  # noqa: E402
from vertical.assemble import AssembledForest, ForestAssembler  # noqa: E402
from vertical.record import record_digest, record_dtype, record_layout  # noqa: E402

TREE = "trees"
RECORD = record_dtype(SHIN_UCHUU_RECORD)
ORACLES = [FOREST_1100, FOREST_1200, FOREST_1300, FOREST_1400]

#: The plan at 8 halos per file: (first forest, last forest, the forests' halo counts).
FILES = [(0, 0, [8]), (1, 2, [6, 2]), (3, 3, [3])]


def file_size(number):
    """The bytes of planned file ``number``: its header and its records."""
    counts = FILES[number][2]
    return writer.header_bytes(len(counts)) + sum(counts) * RECORD.itemsize


class Killed(BaseException):
    """A simulated kill: not an ``Exception``, so no per-file handler catches it."""


def fake_workers(count=2):
    """Stand-ins for a pool's worker processes: alive until terminated, never alive after."""
    return [mock.Mock(**{"is_alive.return_value": False}) for _ in range(count)]


def fake_pool(
    running=None, submitting=None, submitted=None, workers=None, shutdowns=None, on_submit=None
):
    """A ``ProcessPoolExecutor`` stand-in that runs each job at submission and returns its
    completed future, recording the file numbers submitted and, for each ``shutdown``, its
    arguments and whether each of ``workers`` (its ``_processes``) had been joined by then.
    ``on_submit(job)`` runs at the start of each submission.

    With ``running`` or ``submitting`` it follows the pool's contract when a worker dies: the
    future of file ``running`` fails with ``BrokenProcessPool``, and so does the submission of
    file ``submitting`` and every later one.
    """

    class FakePool:
        def __init__(self, max_workers, mp_context, initializer=None):
            self.broken = False
            self._processes = dict(enumerate(workers or []))

        def submit(self, function, job):
            if on_submit is not None:
                on_submit(job)
            if self.broken or job.number == submitting:
                self.broken = True
                raise BrokenProcessPool("the pool is broken")
            if submitted is not None:
                submitted.append(job.number)
            future = Future()
            if job.number == running:
                self.broken = True
                future.set_exception(BrokenProcessPool("a child process terminated"))
            else:
                future.set_result(function(job))
            return future

        def shutdown(self, wait, cancel_futures):
            if shutdowns is not None:
                shutdowns.append((wait, cancel_futures, [p.join.called for p in workers or []]))

    return FakePool


def signalling_wait(*signals):
    """``writer.wait`` sending ``signals`` to this process on its first call, before it waits:
    the operator's signals arriving while the parent waits for its workers."""
    real = writer.wait
    sent = []

    def wait(*args, **kwargs):
        if not sent:
            sent.append(True)
            for number in signals:
                os.kill(os.getpid(), number)
        return real(*args, **kwargs)

    return wait


@contextlib.contextmanager
def sentinel_handlers():
    """Handlers for SIGINT and SIGTERM that fail the test if a signal reaches them, so a run
    that did not install its own cannot kill the test process; yields them for comparison."""

    def unexpected(*_):
        raise AssertionError("a signal reached the handler convert should have replaced")

    previous = {
        number: signal.signal(number, unexpected) for number in writer._StopRequests.SIGNALS
    }
    try:
        yield unexpected
    finally:
        for number, handler in previous.items():
            signal.signal(number, handler)


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
        cls.dataset_dir = convert_one_file_dataset(
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

    def job(self):
        """A job for file 0 whose output directory is the case's work directory."""
        return writer.FileJob(
            dataset=str(self.dataset_dir),
            halo_properties=str(SHIN_UCHUU_RECORD),
            window_rows=1,
            output=str(self.work),
            tree_name=TREE,
            number=0,
            first_forest=0,
            last_forest=0,
            n_halos=8,
            counts=np.array([8]),
            expected_sha256=None,
            stamp="stamp",
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
        self.assertEqual(bound["assembler"], assembler.memory_bound(8))
        self.assertEqual(bound["total"], sum(bound[name] for name in bound if name != "total"))
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
        self.assertEqual(
            [assembled_rows(AssembledForest(0, forest)) for forest in forests], ORACLES
        )
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
            self.assertEqual(entry["bytes"], file_size(number))
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
                raise Killed()
            return real(*args)

        with mock.patch.object(writer, "_verify", killed_on_the_second_file):
            with self.assertRaises(Killed):
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
            out,
            r"moved aside \S+\.1\.partial \({} bytes\) to trees\.1\.partial\.stale-".format(
                file_size(1)
            ),
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
                raise Killed()
            return real(*args)

        with mock.patch.object(writer, "write_json_atomic", killed_on_the_third_save):
            with self.assertRaises(Killed):
                self.convert()
        self.assertTrue(self.path(1).exists())
        self.assertEqual(sorted(self.entries()), [0])
        orphan = self.path(1).read_bytes()

        code, out, err = self.convert()
        self.assertEqual(code, 0, err + out)
        self.assertRegex(
            out, r"moved aside \S+/trees\.1 \({} bytes\) to trees\.1\.stale-".format(file_size(1))
        )
        (stale,) = self.output.glob("{}.1.stale-*".format(TREE))
        self.assertEqual(stale.read_bytes(), orphan)
        self.assertEqual(self.path(1).read_bytes(), orphan)
        self.assertEqual(sorted(self.entries()), [0, 1, 2])

    def test_a_worker_death_keeps_the_entries_of_files_it_did_not_touch(self):
        """A complete conversion rerun in a pool that breaks running file 1: file 1 fails in
        flight, file 2 is never started, both keep their entries, and the next run skips all
        three by SHA-256. A fresh conversion whose pool breaks idle, at file 2's submission
        after files 0 and 1 completed, names file 2 and exits 2. A fresh conversion whose
        pool breaks running file 0 refuses file 1's submission and never takes file 2."""

        died = "a worker process died (killed, or out of memory)"
        self.plan()
        self.assertEqual(self.convert()[0], 0)
        before = self.manifest()["files"]
        with mock.patch.object(writer, "ProcessPoolExecutor", fake_pool(running=1)):
            code, out, err = self.convert("--workers", 2)
        self.assertEqual(code, 2, err)
        self.assertEqual(out.count(died), 1)
        self.assertIn("file 1: in flight when a worker process died", out)
        self.assertEqual(self.manifest()["files"], before)

        code, out, err = self.convert()
        self.assertEqual(code, 0, err + out)
        self.assertEqual(out.count("matches its manifest entry; skipped"), 3)
        self.assertEqual(self.manifest()["files"], before)

        idle = self.work / "idle"
        self.plan(idle)
        with mock.patch.object(writer, "ProcessPoolExecutor", fake_pool(submitting=2)):
            code, out, err = self.convert("--workers", 2, output=idle)
        self.assertEqual(code, 2, err)
        self.assertEqual(out.count(died), 1)
        self.assertIn("FAILED file 2: file 2: not started: a worker process died", out)
        self.assertIn("2 of 3 selected file(s) complete", out)
        self.assertEqual(sorted(self.entries(idle)), [0, 1])

        early = self.work / "early"
        self.plan(early)
        with mock.patch.object(writer, "ProcessPoolExecutor", fake_pool(running=0)):
            code, out, err = self.convert("--workers", 2, output=early)
        self.assertEqual(code, 2, err)
        self.assertIn("FAILED file 1: file 1: not started: a worker process died", out)
        self.assertIn("1 file(s) never started: 2", out)


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
        real = writer._blocks

        def corrupted(path, *args):
            for block in real(path, *args):
                if Path(path).name == "{}.1.partial".format(TREE):
                    block = bytes(block[:-1]) + bytes([block[-1] ^ 0xFF])
                yield block

        with mock.patch.object(writer, "_blocks", corrupted):
            code, out, err = self.convert()
        self.assertEqual(code, 1, err)
        self.assertRegex(out, r"trees\.1\.partial: verification failed \(sha256\)")
        self.assertFalse(self.path(1).exists())
        self.assertTrue((self.output / "{}.1.partial".format(TREE)).exists())
        self.assertEqual(sorted(self.entries()), [0, 2])


class InterruptTests(ConverterCase):
    """Stop requests: the first signal lets the files in flight finish and records them, the
    second terminates the workers; either way no file is started after the first."""

    def test_the_first_signal_records_the_files_in_flight_and_starts_no_more(self):
        self.plan()
        submitted, shutdowns, workers = [], [], fake_workers()
        pool = fake_pool(submitted=submitted, workers=workers, shutdowns=shutdowns)
        with mock.patch.object(writer, "ProcessPoolExecutor", pool):
            with mock.patch.object(writer, "wait", signalling_wait(signal.SIGTERM)):
                with sentinel_handlers():
                    code, out, err = self.convert("--workers", 2)
        self.assertEqual(code, 2, err)
        self.assertEqual(submitted, [0, 1])
        self.assertIn("stop requested: no further file is started", out)
        self.assertIn("interrupted: the run is incomplete", out)
        self.assertIn("2 of 3 selected file(s) complete", out)
        self.assertIn("1 file(s) never started: 2", out)
        self.assertEqual(sorted(self.entries()), [0, 1])
        self.assertEqual(shutdowns, [(True, True, [False, False])])
        for worker in workers:
            worker.terminate.assert_not_called()
        self.assertFalse(self.path(2).exists())

        code, out, err = self.convert()
        self.assertEqual(code, 0, err + out)
        self.assertEqual(out.count("matches its manifest entry; skipped"), 2)
        self.assertEqual(sorted(self.entries()), [0, 1, 2])

    def test_the_second_signal_terminates_the_workers_and_abandons_their_files(self):
        self.plan()
        shutdowns, workers = [], fake_workers()
        pool = fake_pool(workers=workers, shutdowns=shutdowns)
        with mock.patch.object(writer, "ProcessPoolExecutor", pool):
            with mock.patch.object(writer, "wait", signalling_wait(signal.SIGINT, signal.SIGINT)):
                with sentinel_handlers():
                    code, out, err = self.convert("--workers", 2)
        self.assertEqual(code, 2, err)
        for worker in workers:
            worker.terminate.assert_called_once_with()
            worker.kill.assert_not_called()
        self.assertEqual(shutdowns, [(True, True, [True, True])])
        self.assertIn("0 of 3 selected file(s) complete", out)
        self.assertEqual(self.entries(), {})

        code, out, err = self.convert()
        self.assertEqual(code, 0, err + out)
        self.assertRegex(out, r"moved aside \S+/trees\.0 \(\d+ bytes\) to trees\.0\.stale-")
        self.assertEqual(sorted(self.entries()), [0, 1, 2])

    def test_a_signal_during_a_submission_stops_the_remaining_slots_filling(self):
        self.plan()
        submitted = []

        def signal_while_submitting_file_0(job):
            if job.number == 0:
                os.kill(os.getpid(), signal.SIGTERM)

        pool = fake_pool(submitted=submitted, on_submit=signal_while_submitting_file_0)
        with mock.patch.object(writer, "ProcessPoolExecutor", pool):
            with sentinel_handlers():
                code, out, err = self.convert("--workers", 2)
        self.assertEqual(code, 2, err)
        self.assertEqual(submitted, [0])
        self.assertEqual(sorted(self.entries()), [0])
        self.assertIn("2 file(s) never started: 1, 2", out)

    def test_a_forced_stop_while_a_corrupted_file_is_rebuilt_is_incomplete(self):
        """File 1's old entry would otherwise survive its being moved aside, and the run would
        count it complete and exit 0."""
        self.plan()
        self.assertEqual(self.convert()[0], 0)
        data = self.path(1).read_bytes()
        self.path(1).write_bytes(data[:-1] + bytes([data[-1] ^ 0xFF]))

        def stopped_while_writing_file_1(job, *args):
            os.kill(os.getpid(), signal.SIGINT)
            os.kill(os.getpid(), signal.SIGINT)

        with mock.patch.object(writer, "_write_partial", stopped_while_writing_file_1):
            with sentinel_handlers():
                code, out, err = self.convert()
        self.assertEqual(code, 2, err)
        self.assertFalse(self.path(1).exists())
        self.assertEqual(sorted(self.entries()), [0, 2])
        self.assertIn("2 of 3 selected file(s) complete", out)

    def test_a_signalled_rerun_keeps_the_entries_of_intact_files_and_exits_0_when_complete(self):
        """Every file is already recorded, so the run is complete however it was stopped."""
        self.plan()
        self.assertEqual(self.convert()[0], 0)
        before = self.manifest()["files"]
        for signals in ((signal.SIGTERM,), (signal.SIGTERM, signal.SIGTERM)):
            with self.subTest(signals=len(signals)):
                with mock.patch.object(writer, "ProcessPoolExecutor", fake_pool()):
                    with mock.patch.object(writer, "wait", signalling_wait(*signals)):
                        with sentinel_handlers():
                            code, out, err = self.convert("--workers", 2)
                self.assertEqual(code, 0, err + out)
                self.assertEqual(self.manifest()["files"], before)

    def test_with_one_worker_real_signals_stop_the_run_after_or_inside_the_file(self):
        """Real signals while file 1 is written in the parent. One SIGTERM lets file 1 finish
        and be recorded and starts no file after it; a SIGINT and a SIGTERM stop file 1 itself,
        leaving only file 0 recorded. The previous handlers are back afterwards."""
        cases = [
            ("the first signal", (signal.SIGTERM,), [0, 1], "1 file(s) never started: 2"),
            ("the second signal", (signal.SIGINT, signal.SIGTERM), [0], None),
        ]
        real = writer.convert_file
        for label, signals, recorded, message in cases:
            with self.subTest(label):
                output = self.work / label.replace(" ", "_")
                self.plan(output)

                def terminated_in_file_1(job, signals=signals):
                    if job.number == 1:
                        for number in signals:
                            os.kill(os.getpid(), number)
                    return real(job)

                with sentinel_handlers() as unexpected:
                    with mock.patch.object(writer, "convert_file", terminated_in_file_1):
                        code, out, err = self.convert(output=output)
                    for number in writer._StopRequests.SIGNALS:
                        self.assertIs(signal.getsignal(number), unexpected)
                self.assertEqual(code, 2, err)
                self.assertEqual(sorted(self.entries(output)), recorded)
                self.assertEqual(self.path(1, output).exists(), 1 in recorded)
                if message:
                    self.assertIn(message, out)

    def test_main_turns_an_interrupt_into_exit_2(self):
        self.plan()
        with mock.patch.object(write_vertical_trees, "convert", side_effect=KeyboardInterrupt):
            code, out, err = self.convert()
        self.assertEqual((code, err), (2, "ERROR: interrupted\n"))

    def test_terminated_workers_are_joined_and_a_stubborn_one_is_killed(self):
        quick, stubborn = fake_workers(2)
        stubborn.is_alive.return_value = True
        writer._terminate_workers(types.SimpleNamespace(_processes={7: quick, 8: stubborn}))
        for process in (quick, stubborn):
            process.terminate.assert_called_once_with()
            process.join.assert_any_call(writer.TERMINATE_JOIN_SECONDS)
        quick.kill.assert_not_called()
        stubborn.kill.assert_called_once_with()
        self.assertEqual(stubborn.join.call_count, 2)


class LockTests(ConverterCase):
    def test_a_second_convert_in_the_directory_is_refused_naming_the_lock(self):
        self.plan()
        lock = self.output / writer.LOCK_NAME
        refuse = mock.patch.multiple(
            HorizontalDataset, read_rows=mock.DEFAULT, iter_columns=mock.DEFAULT
        )
        with open(lock, "a") as holder:
            fcntl.flock(holder.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
            with refuse as reads:
                code, out, err = self.convert()
            self.assertFalse(reads["read_rows"].called or reads["iter_columns"].called)
        self.assertEqual(code, 2)
        self.assertIn(str(lock), err)
        self.assertIn("another convert holds this output directory's lock", err)
        self.assertFalse((self.output / writer.MANIFEST_NAME).exists())

        code, out, err = self.convert()
        self.assertEqual(code, 0, err + out)
        self.assertEqual(sorted(path.name for path in self.output.glob("*.lock")), [lock.name])

    def test_a_file_another_process_is_writing_is_refused_and_the_rest_converted(self):
        """A held ``trees.1.lock`` stands for a worker orphaned by a killed parent."""
        self.plan()
        lock = self.output / "{}.1{}".format(TREE, writer.FILE_LOCK_SUFFIX)
        lock.touch()
        with open(lock, "a") as holder:
            fcntl.flock(holder.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
            code, out, err = self.convert()
        self.assertEqual(code, 2, err)
        self.assertIn(
            "FAILED file 1: file 1: trees.1 is being written by another process (trees.1.lock "
            "is locked); wait for it or kill it",
            out,
        )
        self.assertEqual(sorted(self.entries()), [0, 2])
        self.assertEqual(list(self.output.glob("trees.1.partial*")), [])
        self.assertFalse(self.path(1).exists())
        self.assertEqual(
            sorted(path.name for path in self.output.glob("*.lock")), [".convert.lock", lock.name]
        )

        code, out, err = self.convert()
        self.assertEqual(code, 0, err + out)
        self.assertEqual(sorted(self.entries()), [0, 1, 2])
        self.assertEqual(
            sorted(path.name for path in self.output.glob("*.lock")), [".convert.lock"]
        )

    def test_a_filesystem_that_cannot_lock_is_warned_about_once_and_not_refused(self):
        self.plan()

        def unsupported(*_):
            raise OSError(errno.ENOTSUP, "Operation not supported")

        with mock.patch.object(writer.fcntl, "flock", unsupported):
            code, out, err = self.convert("--workers", 2)
        self.assertEqual(code, 0, err + out)
        self.assertEqual(out.count("the filesystem cannot lock"), 1)
        self.assertIn(str(self.output / writer.LOCK_NAME), out)
        self.assertEqual(
            sorted(path.name for path in self.output.glob("*.lock")), [".convert.lock"]
        )


class FileLockLifetimeTests(ConverterCase):
    def test_a_completed_file_leaves_no_lock_and_a_failed_one_leaves_its_lock(self):
        final = self.work / "{}.0".format(TREE)
        lock = self.work / "{}.0{}".format(TREE, writer.FILE_LOCK_SUFFIX)
        job = self.job()
        with writer._file_lock(final, job):
            self.assertTrue(lock.exists())
        self.assertFalse(lock.exists())
        with self.assertRaises(ValueError):
            with writer._file_lock(final, job):
                raise ValueError("the file failed")
        self.assertTrue(lock.exists())
        with writer._file_lock(final, job):
            pass
        self.assertFalse(lock.exists())


class LockFileReplacedTests(ConverterCase):
    """A lock taken on a lock file that was unlinked after it was opened excludes nobody, so
    the lock is retaken on the file the path now names."""

    def replacing_lock(self, times):
        """``writer._lock`` that, ``times`` times, replaces the lock file after locking it."""
        real = writer._lock
        left = [times]

        def lock(handle):
            locked = real(handle)
            if left[0]:
                left[0] -= 1
                path = Path(handle.name)
                path.unlink()
                path.touch()
            return locked

        return mock.patch.object(writer, "_lock", lock)

    def test_a_replaced_lock_file_is_locked_again_until_the_attempts_run_out(self):
        """A lock file replaced twice is locked again; one that keeps being replaced is refused,
        naming the file."""
        final = self.work / "{}.0".format(TREE)
        with self.replacing_lock(2):
            with writer._file_lock(final, self.job()):
                pass
        with self.replacing_lock(writer.FILE_LOCK_ATTEMPTS):
            with self.assertRaisesRegex(ConverterError, r"file 0: trees\.0\.lock keeps being"):
                with writer._file_lock(final, self.job()):
                    self.fail("the lock must not be taken")


class LedgerTests(unittest.TestCase):
    def test_a_write_that_lands_before_the_assignment_is_not_lost_by_the_sweep(self):
        """A signal after ``write_json_atomic`` replaced the manifest but before memory was
        assigned: ``reload`` takes the disk as the truth, and the sweep keeps the entry."""
        directory = Path(tempfile.mkdtemp(prefix="write_vertical_ledger_"))
        self.addCleanup(shutil.rmtree, directory, ignore_errors=True)
        ledger = writer._Ledger({"files": [], "runs": []}, directory, TREE)
        (directory / "{}.1".format(TREE)).write_bytes(b"complete")
        real = writer.write_json_atomic

        def written_then_interrupted(path, document):
            real(path, document)
            raise KeyboardInterrupt

        with mock.patch.object(writer, "write_json_atomic", written_then_interrupted):
            with self.assertRaises(KeyboardInterrupt):
                ledger.record(1, {"number": 1, "sha256": "b"})
        self.assertEqual(ledger.manifest["files"], [])
        ledger.reload()
        ledger.drop_absent([0, 1])
        self.assertEqual([entry["number"] for entry in ledger.manifest["files"]], [1])
        on_disk = json.loads((directory / writer.MANIFEST_NAME).read_text())
        self.assertEqual([entry["number"] for entry in on_disk["files"]], [1])

    def test_a_manifest_write_that_fails_leaves_memory_as_the_disk_is(self):
        directory = Path(tempfile.mkdtemp(prefix="write_vertical_ledger_"))
        self.addCleanup(shutil.rmtree, directory, ignore_errors=True)
        manifest = {"files": [{"number": 0, "sha256": "a"}], "runs": []}
        ledger = writer._Ledger(manifest, directory, TREE)
        ledger.record(1, {"number": 1, "sha256": "b"})
        on_disk = json.loads((directory / writer.MANIFEST_NAME).read_text())
        self.assertEqual(ledger.manifest, on_disk)
        before = json.loads(json.dumps(ledger.manifest))

        def interrupted(*_):
            raise KeyboardInterrupt

        with mock.patch.object(writer, "write_json_atomic", interrupted):
            for change in (
                lambda: ledger.record(2, {"number": 2, "sha256": "c"}),
                lambda: ledger.forget(0),
                lambda: ledger.start_run({"workers": 1}),
                lambda: ledger.drop_absent([0, 1]),
            ):
                with self.assertRaises(KeyboardInterrupt):
                    change()
                self.assertEqual(ledger.manifest, before)
        self.assertEqual(json.loads((directory / writer.MANIFEST_NAME).read_text()), on_disk)


class PlanStrictnessTests(ConverterCase):
    def test_a_hand_edited_plan_is_refused_by_name_before_any_row_is_read(self):
        """The cases of ``_check_files`` are in test_vertical_plan; this is the command line's
        contract for one of them: exit 2 naming the entry, with no row read."""
        self.plan()
        path = self.output / "file_plan.json"
        plan = json.loads(path.read_text())
        plan["files"][1]["first_forest"] = 1.0
        path.write_text(json.dumps(plan))
        refuse = mock.patch.multiple(
            HorizontalDataset, read_rows=mock.DEFAULT, iter_columns=mock.DEFAULT
        )
        with refuse as reads:
            code, out, err = self.convert()
        self.assertEqual(code, 2, out)
        self.assertRegex(err, r"file entry 1 is malformed")
        self.assertFalse(reads["read_rows"].called or reads["iter_columns"].called)

    def test_a_header_value_past_int32_is_refused_before_the_file_is_opened(self):
        partial = self.work / "trees.0.partial"
        job = writer.FileJob(
            dataset=str(self.dataset_dir),
            halo_properties=str(SHIN_UCHUU_RECORD),
            window_rows=1,
            output=str(self.work),
            tree_name=TREE,
            number=0,
            first_forest=0,
            last_forest=0,
            n_halos=2**31,
            counts=np.array([2**31]),
            expected_sha256=None,
            stamp="stamp",
        )
        with self.assertRaisesRegex(
            ConverterError, r"the header holds 2147483648, which does not fit int32"
        ):
            writer._write_partial(job, None, partial)
        self.assertFalse(partial.exists())


class UnexpectedErrorTests(ConverterCase):
    def test_an_uncaught_exception_exits_2_naming_its_type(self):
        """Python's own status for an uncaught exception, 1, is this tool's verification
        failure."""
        self.plan()
        with mock.patch.object(write_vertical_trees, "convert", side_effect=RuntimeError("boom")):
            code, out, err = self.convert()
        self.assertEqual(code, 2)
        self.assertIn("Traceback (most recent call last)", err)
        self.assertTrue(err.endswith("ERROR: unexpected RuntimeError: boom\n"))
        with mock.patch.object(write_vertical_trees, "make_file_plan", side_effect=KeyError("k")):
            code, out, err = run(
                "plan",
                "--input",
                self.dataset_dir,
                "--halo-properties",
                SHIN_UCHUU_RECORD,
                "--output",
                self.work / "again",
            )
        self.assertEqual(code, 2)
        self.assertTrue(err.endswith("ERROR: unexpected KeyError: 'k'\n"))


class ManifestHygieneTests(ConverterCase):
    def converted(self):
        self.plan()
        self.assertEqual(self.convert()[0], 0)
        return self.manifest()["files"]

    def test_a_corrupted_recorded_file_is_moved_aside_and_rebuilt(self):
        """File 1's bytes differ from its entry: it is moved aside and rebuilt byte for byte
        as it was, and every entry ends as it began, the rebuilt file's apart from its run."""
        before = {entry["number"]: entry for entry in self.converted()}
        data = self.path(1).read_bytes()
        corrupted = data[:-1] + bytes([data[-1] ^ 0xFF])
        self.path(1).write_bytes(corrupted)
        code, out, err = self.convert()
        self.assertEqual(code, 0, err + out)
        self.assertRegex(out, r"moved aside \S+/trees\.1 \(\d+ bytes\) to trees\.1\.stale-")
        (stale,) = self.output.glob("{}.1.stale-*".format(TREE))
        self.assertEqual(stale.read_bytes(), corrupted)
        self.assertEqual(self.path(1).read_bytes(), data)
        self.assertEqual(self.entries()[1]["sha256"], before[1]["sha256"])
        self.assertEqual(self.entries()[1]["run"], 1)
        self.assertEqual(self.entries()[0], before[0])
        self.assertEqual(self.entries()[2], before[2])

    def test_a_job_that_fails_before_touching_its_file_keeps_its_entry(self):
        before = {entry["number"]: entry for entry in self.converted()}
        real = writer.convert_file

        def refusing(job):
            if job.number == 0:
                raise ConverterError("refused before touching the file")
            return real(job)

        with mock.patch.object(writer, "convert_file", refusing):
            code, out, err = self.convert()
        self.assertEqual(code, 2, err)
        self.assertIn("FAILED file 0: refused before touching the file", out)
        self.assertEqual(self.entries(), before)

    def test_a_rebuild_that_fails_leaves_no_entry_for_the_file_it_moved_aside(self):
        before = {entry["number"]: entry for entry in self.converted()}
        data = self.path(1).read_bytes()
        self.path(1).write_bytes(data[:-1] + bytes([data[-1] ^ 0xFF]))
        real = writer._verify

        def failing(job, *args):
            if job.number == 1:
                raise ValueError("boom")
            return real(job, *args)

        with mock.patch.object(writer, "_verify", failing):
            code, out, err = self.convert()
        self.assertEqual(code, 2, err)
        self.assertIn("FAILED file 1: file 1: unexpected ValueError: boom", out)
        self.assertFalse(self.path(1).exists())
        self.assertEqual(sorted(self.entries()), [0, 2])
        self.assertEqual(self.entries()[0], before[0])

    def test_the_entries_of_absent_files_are_swept_over_every_planned_file(self):
        self.converted()
        self.path(2).rename(self.output / "trees.2.moved")
        code, out, err = self.convert("--only-files", 0)
        self.assertEqual(code, 0, err + out)
        self.assertEqual(sorted(self.entries()), [0, 1])

    def test_the_dataset_identity_is_hashed_once(self):
        self.plan()
        real = HorizontalDataset.forest_id_sha256
        with mock.patch.object(
            HorizontalDataset, "forest_id_sha256", autospec=True, side_effect=real
        ) as hashed:
            self.assertEqual(self.convert()[0], 0)
        self.assertEqual(hashed.call_count, 1)


class VerifyBranchTests(ConverterCase):
    """The checks a flipped byte cannot reach, which the SHA-256 comparison would otherwise
    mask: file 1's re-read is cut short, so it fails by the check that names the cut."""

    def cut_reread(self, keep):
        real = writer._blocks

        def cut(path, *args):
            if Path(path).name != "{}.1.partial".format(TREE):
                yield from real(path, *args)
                return
            for block in real(path, *args):
                yield bytes(block[:keep])
                return

        return mock.patch.object(writer, "_blocks", cut)

    def test_a_re_read_cut_short_fails_the_check_that_names_the_cut(self):
        """A re-read cut one byte short of the header fails the header check; one cut right
        after the header fails the size check."""
        head = writer.header_bytes(len(FILES[1][2]))
        cases = [
            ("header", head - 1, r"\(header\): {} bytes".format(head - 1)),
            ("size", head, r"\(size\): {} bytes, not".format(head)),
        ]
        for check, keep, message in cases:
            with self.subTest(check):
                output = self.work / check
                self.plan(output)
                with self.cut_reread(keep):
                    code, out, err = self.convert(output=output)
                self.assertEqual(code, 1, err)
                self.assertRegex(out, r"trees\.1\.partial: verification failed " + message)
                self.assertFalse(self.path(1, output).exists())


class ManifestRefusalTests(ConverterCase):
    """A manifest edited in one way is refused, naming what is wrong, before any file is
    attempted."""

    def test_a_malformed_manifest_is_refused_by_name(self):
        self.plan()
        self.assertEqual(self.convert()[0], 0)
        path = self.output / writer.MANIFEST_NAME
        original = path.read_text()

        def edited(edit):
            manifest = json.loads(original)
            edit(manifest)
            return json.dumps(manifest)

        def duplicate_file(manifest):
            manifest["files"].append(dict(manifest["files"][0]))

        cases = [
            ("not JSON", "{", r"malformed conversion manifest"),
            (
                "another format",
                edited(lambda m: m.update(format="other")),
                r"not a mimic-vertical-conversion-manifest version 1 file",
            ),
            (
                "another version",
                edited(lambda m: m.update(version=2)),
                r"not a mimic-vertical-conversion-manifest version 1 file",
            ),
            ("a file recorded twice", edited(duplicate_file), r"a file is recorded twice"),
            (
                "a sha256 that is not a string",
                edited(lambda m: m["files"][0].update(sha256=7)),
                r"a file entry's sha256 is not a string",
            ),
        ]
        for label, text, message in cases:
            with self.subTest(label):
                path.write_text(text)
                code, out, err = self.convert()
                self.assertEqual(code, 2, out)
                self.assertRegex(err, message)
                self.assertIn(str(path), err)
        path.write_text(original)
        self.assertEqual(self.convert()[0], 0)


class CommittedFixtureTests(ConverterCase):
    def test_the_worked_ascii_conversion_equals_the_committed_lhalo_fixture(self):
        """Pins the assembler and writer to a committed artifact: ``worked_ascii`` converted in
        the Shin-Uchuu L-Halo record, two forests to a file, is byte for byte the committed
        ``shin-uchuu-cut.0`` and ``.1``, from a plan equal to the committed one."""
        package = REPO_ROOT / "simulations" / "shin-uchuu-lhalo"
        committed = package / "_tests" / "data" / "worked_ascii_lhalo"
        for window_rows in (None, 1):
            with self.subTest(window_rows=window_rows):
                output = self.work / "window_{}".format(window_rows)
                common = ["--input", WORKED_ASCII, "--output", output]
                common += ["--halo-properties", package / "halo_properties.yaml"]
                code, out, err = run("plan", *common, "--max-forests-per-file", 2)
                self.assertEqual(code, 0, err)
                extra = [] if window_rows is None else ["--window-rows", window_rows]
                code, out, err = run("convert", *common, "--tree-name", "shin-uchuu-cut", *extra)
                self.assertEqual(code, 0, err + out)
                for number in (0, 1):
                    name = "shin-uchuu-cut.{}".format(number)
                    self.assertEqual((output / name).read_bytes(), (committed / name).read_bytes())
                planned = json.loads((output / "file_plan.json").read_text())["files"]
                expected = json.loads((committed / "file_plan.json").read_text())["files"]
                self.assertEqual(planned, expected)


if __name__ == "__main__":
    unittest.main()
