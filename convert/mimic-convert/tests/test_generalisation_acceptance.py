"""Self-tests of the generalisation acceptance harness.

A comparator that cannot detect a planted defect is not evidence, so every
check the harness makes is shown here to fire on a defect planted for it, and
to stay silent on the clean input.

**The oracle is this file, not the converter.** :class:`SyntheticLHalo`
declares a small two-file L-Halo catalogue as literal trees -- a forward gap,
mixed-age sibling progenitors, a three-member FoF group, an empty tree,
duplicate and negative particle identifiers, a negative mass sentinel and a
signed zero -- and writes both the binary source (hand-typed ``struct``
layout) and the ``mimic-source-dump v1`` text the C harness
(``tests/unit/tools/dump_ctrees_topology.c --source-payload``) must produce
for it, from the same declarations. The real converter CLI converts the
source once; the harness compares; the tests then corrupt copies of the
converted dataset (or of the dump) one defect at a time.

**64-bit bounds** are exercised without billion-row allocations: the external
sort and merge join run on keys above 2^53 and row indices above 2^31 under
budgets small enough to force many spill runs and several merge passes, and
the whole comparison runs over *virtual* converted blocks whose snapshot-local
row indices start above 2^31 and whose forest numbers exceed 2^53.

**Extras** are compared against the harness's independent source extraction
for all three formats (the synthetic binary, and the committed forests-HDF5
and ASCII fixtures), and a changed extra bit fails.

**Measurement**: the harness's own measured-run path records times, peak RSS,
exit codes, code and source identities and storage widths.

No test here reads real (non-fixture) simulation data.
"""

import contextlib
import io
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import tracemalloc
import unittest
from pathlib import Path
from unittest import mock

import h5py
import numpy as np
import yaml

HERE = Path(__file__).resolve().parent
REPO_ROOT = HERE.parents[2]
SIMULATIONS = REPO_ROOT / "simulations"
MICRO = SIMULATIONS / "micro-uchuu"

sys.path.insert(0, str(HERE))

import run_generalisation_acceptance as acc  # noqa: E402

# ==========================================================================
# The synthetic L-Halo source and its expected reference dump
# ==========================================================================

#: The 104-byte L-Halo record, restated by hand in file order.
RECORD = struct.Struct("<iiiiiifff3f3fff3fqiiif")
assert RECORD.size == 104


def halo(snap, desc=-1, fp=-1, np_=-1, ffof=None, nfof=-1, **values):
    """One literal halo; links are within-tree ranks, ``ffof`` defaults to self."""
    return dict(snap=snap, desc=desc, fp=fp, np=np_, ffof=ffof, nfof=nfof, **values)


def _values(rank, overrides):
    base = float(rank + 1)
    values = {
        "len": 100 + rank,
        "mbid": 900000 + rank,
        "m_mean": base * 1.25,
        "m_crit": base * 2.5,
        "m_tophat": base * 3.75,
        "pos": (base, base + 0.5, base + 0.25),
        "vel": (base * 10.0, base * 11.0, base * 12.0),
        "veldisp": base * 7.0,
        "vmax": base * 9.0,
        "spin": (base * 0.01, base * 0.02, base * 0.03),
        "filenr": 0,
        "subhalo": rank,
        "subhalf": base * 0.5,
    }
    values.update(overrides)
    return values


#: File 0 tree 0 -- the interesting forest (ranks 0..7):
#:   snap 49: A(0) central, B(1) and G(7) satellites: FoF chain A -> B -> G
#:   A's progenitors: C(2) at 48 (first), then D(3) at 46 -- a forward gap of
#:   3 and a mixed-age sibling chain; B's progenitor E(4) at 47 (gap 2), whose
#:   progenitor is F(5) at 45 (gap 2); H(6) at 20, isolated.
#:   A and E share MostBoundID; C's is negative; D carries a negative mass
#:   sentinel; B has a signed-zero position component.
#: File 0 tree 1 -- empty.  File 0 tree 2 -- one halo at snap 30.
#: File 1 tree 0 -- an adjacent chain at snaps 10, 11, 12.
SYNTHETIC_TREES = [
    [
        [
            halo(49, fp=2, ffof=0, nfof=1, mbid=777),
            halo(49, fp=4, ffof=0, nfof=7, pos=(0.0, -0.0, 3.0)),
            halo(48, desc=0, np_=3, mbid=-5),
            halo(46, desc=0, m_crit=-1.0),
            halo(47, desc=1, fp=5, mbid=777),
            halo(45, desc=4),
            halo(20),
            halo(49, ffof=0),
        ],
        [],
        [halo(30)],
    ],
    [
        [halo(10, desc=1), halo(11, desc=2, fp=0), halo(12, fp=1)],
    ],
]


def _float_bits(value):
    return "{:08x}".format(struct.unpack("<I", struct.pack("<f", value))[0])


class SyntheticLHalo:
    """Writes :data:`SYNTHETIC_TREES` as L-Halo binary and as its expected dump."""

    TREE_NAME = "synthetic_trees"

    def __init__(self, trees=SYNTHETIC_TREES):
        self.trees = trees
        self.halos = []
        forest = 0
        for file_number, file_trees in enumerate(trees):
            for unit, tree in enumerate(file_trees):
                for rank, entry in enumerate(tree):
                    values = _values(rank, {k: v for k, v in entry.items() if k not in _LINK_KEYS})
                    ffof = rank if entry["ffof"] is None else entry["ffof"]
                    links = (entry["desc"], entry["fp"], entry["np"], ffof, entry["nfof"])
                    self.halos.append(
                        (forest, rank, file_number, unit, entry["snap"], links, values, tree)
                    )
                forest += 1
        self.n_forests = forest

    def write_source(self, directory):
        directory = Path(directory)
        directory.mkdir(parents=True, exist_ok=True)
        for file_number, file_trees in enumerate(self.trees):
            counts = [len(tree) for tree in file_trees]
            payload = bytearray(struct.pack("<ii", len(file_trees), sum(counts)))
            payload += struct.pack("<{}i".format(len(counts)), *counts)
            for tree in file_trees:
                for rank, entry in enumerate(tree):
                    v = _values(rank, {k: x for k, x in entry.items() if k not in _LINK_KEYS})
                    ffof = rank if entry["ffof"] is None else entry["ffof"]
                    payload += RECORD.pack(
                        entry["desc"],
                        entry["fp"],
                        entry["np"],
                        ffof,
                        entry["nfof"],
                        v["len"],
                        v["m_mean"],
                        v["m_crit"],
                        v["m_tophat"],
                        *v["pos"],
                        *v["vel"],
                        v["veldisp"],
                        v["vmax"],
                        *v["spin"],
                        v["mbid"],
                        entry["snap"],
                        v["filenr"],
                        v["subhalo"],
                        v["subhalf"],
                    )
            (directory / "{}.{}".format(self.TREE_NAME, file_number)).write_bytes(bytes(payload))
        return directory

    def dump_text(self):
        lines = [
            "# mimic-source-dump v1",
            "# reader lhalo_binary partition_model per_file",
            "# columns forest_index rank partition unit snapnum descendant descendant_snap "
            "first_progenitor first_progenitor_snap next_progenitor next_progenitor_snap "
            "first_fof next_fof len most_bound_id m_crit200 pos_x pos_y pos_z vel_x vel_y "
            "vel_z spin_x spin_y spin_z vel_disp vmax",
            "# links are within-forest ranks, -1 = no link; *_snap is the target's snapnum, "
            "-1 = no link; m_crit200..vmax are binary32 bit patterns in hex",
        ]
        for forest, rank, file_number, unit, snap, links, v, tree in self.halos:
            desc, fp, np_, ffof, nfof = links

            def target(index, tree=tree):
                return tree[index]["snap"] if index >= 0 else -1

            fields = [
                forest,
                rank,
                file_number,
                unit,
                snap,
                desc,
                target(desc),
                fp,
                target(fp),
                np_,
                target(np_),
                ffof,
                nfof,
                v["len"],
                v["mbid"],
            ]
            floats = [v["m_crit"], *v["pos"], *v["vel"], *v["spin"], v["veldisp"], v["vmax"]]
            lines.append(" ".join([str(f) for f in fields] + [_float_bits(f) for f in floats]))
        lines.append("# end rows {} forests {}".format(len(self.halos), self.n_forests))
        return "\n".join(lines) + "\n"


_LINK_KEYS = ("snap", "desc", "fp", "np", "ffof", "nfof")


def lhalo_ingest_args(source_dir, column_map=None):
    args = [
        "--source-format",
        "lhalo_binary",
        "--simulation-info",
        MICRO / "simulation_info.yaml",
        "--a-list",
        MICRO / "micro-uchuu.a_list",
        "--halo-properties",
        MICRO / "halo_properties.yaml",
        "--source-dir",
        source_dir,
        "--tree-name",
        SyntheticLHalo.TREE_NAME,
        "--first-file",
        "0",
        "--last-file",
        "1",
    ]
    if column_map is not None:
        args += ["--column-map", column_map]
    return [str(arg) for arg in args]


def run_harness(argv):
    """``acc.main`` with captured output; returns (exit code, stdout, stderr)."""
    out, err = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        code = acc.main([str(arg) for arg in argv])
    return code, out.getvalue(), err.getvalue()


def convert_with_harness(tmp, ingest_args, record, simulation_info=MICRO / "simulation_info.yaml"):
    workdir = Path(tmp) / "work"
    code, out, err = run_harness(
        [
            "convert",
            "--record",
            record,
            "--workdir",
            workdir,
            "--simulation-info",
            simulation_info,
            "--",
        ]
        + list(ingest_args)
    )
    if code != 0:
        stages = json.loads(Path(record).read_text())["runs"][-1].get("stages", [])
        detail = [Path(stage["stderr"]).read_text()[-2000:] for stage in stages[-1:]]
        raise AssertionError("conversion failed:\n{}\n{}\n{}".format(out, err, "".join(detail)))
    attempts = sorted((workdir / "write").glob("attempt_*"))
    assert len(attempts) == 1, attempts
    return attempts[0]


# ==========================================================================
# Dataset mutation helpers (each on a private copy)
# ==========================================================================


def rewrite_rows(path, transform):
    """Replace every /halos dataset of one snapshot file with ``transform(array)``."""
    with h5py.File(path, "r+") as handle:
        halos = handle["halos"]
        for name in list(halos.keys()):
            data = halos[name][...]
            dtype = halos[name].dtype
            del halos[name]
            halos.create_dataset(name, data=transform(data), dtype=dtype)


def locate(dataset_dir, forest, rank):
    """(snapshot file, row) of one converted halo, found by its identity."""
    for path in sorted(Path(dataset_dir).glob("snapshot_*.h5")):
        with h5py.File(path, "r") as handle:
            halos = handle["halos"]
            hit = np.flatnonzero(
                (halos["ForestIndex"][...] == forest) & (halos["HaloRankInForest"][...] == rank)
            )
            if len(hit):
                return path, int(hit[0])
    raise KeyError((forest, rank))


def set_value(dataset_dir, forest, rank, name, value, component=None):
    path, row = locate(dataset_dir, forest, rank)
    with h5py.File(path, "r+") as handle:
        dataset = handle["halos"][name]
        if component is None:
            dataset[row] = value
        else:
            current = dataset[row]
            current[component] = value
            dataset[row] = current


def get_value(dataset_dir, forest, rank, name):
    path, row = locate(dataset_dir, forest, rank)
    with h5py.File(path, "r") as handle:
        return handle["halos"][name][row]


def retype(path, name, dtype):
    """Rewrite one /halos dataset of one snapshot file with another storage dtype."""
    with h5py.File(path, "r+") as handle:
        data = handle["halos"][name][...].astype(dtype)
        del handle["halos"][name]
        handle["halos"].create_dataset(name, data=data)


def empty_snapshot(dataset_dir):
    """The first snapshot file of the dataset that holds no halos."""
    for path in sorted(Path(dataset_dir).glob("snapshot_*.h5")):
        with h5py.File(path, "r") as handle:
            if handle["halos"]["SourceHaloID"].shape[0] == 0:
                return path
    raise AssertionError("the synthetic dataset has no empty snapshot")


def compare(dataset, dump, budget_bytes=1 << 20, block_rows=3):
    return acc.compare_dataset(dataset, dump, "lhalo_binary", budget_bytes, None, block_rows)


# ==========================================================================
# The comparator against planted defects
# ==========================================================================


class ComparatorDefectTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = Path(tempfile.mkdtemp(prefix="acceptance_selftest_"))
        cls.synthetic = SyntheticLHalo()
        cls.source = cls.synthetic.write_source(cls.tmp / "source")
        cls.dump = cls.tmp / "reference.dump"
        cls.dump.write_text(cls.synthetic.dump_text())
        cls.record = cls.tmp / "record.json"
        cls.dataset = convert_with_harness(cls.tmp, lhalo_ingest_args(cls.source), cls.record)

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def setUp(self):
        self.work = Path(tempfile.mkdtemp(prefix="acceptance_defect_"))
        self.addCleanup(shutil.rmtree, self.work, ignore_errors=True)

    def copy_dataset(self):
        target = self.work / "dataset"
        shutil.copytree(self.dataset, target)
        return target

    def copy_dump(self, transform):
        target = self.work / "reference.dump"
        target.write_text(transform(self.dump.read_text()))
        return target

    def assertFailsExactly(self, report, *checks):
        self.assertEqual(report["verdict"], "FAIL", report["failed_checks"])
        self.assertEqual(sorted(report["failed_checks"]), sorted(checks))

    # -- the clean input -----------------------------------------------------

    def test_clean_conversion_passes_every_check(self):
        report = compare(self.dataset, self.dump)
        self.assertEqual(report["verdict"], "PASS", json.dumps(report["checks"], indent=1))
        self.assertEqual(report["matched_rows"], len(self.synthetic.halos))
        self.assertEqual(report["reference_rows"], report["converted_rows"])
        for name, check in report["checks"].items():
            self.assertNotIn("not_applicable", check, name)
            if not name.startswith("duplicate_"):
                self.assertGreater(check["compared"], 0, name)

    def test_the_synthetic_graph_carries_what_it_claims(self):
        # the literal source really contains the cases the defects rely on
        desc_span = [
            snap_t - snap
            for _f, _r, _file, _u, snap, links, _v, tree in self.synthetic.halos
            for snap_t in [tree[links[0]]["snap"] if links[0] >= 0 else None]
            if snap_t is not None
        ]
        self.assertIn(3, desc_span)
        ids = [v["mbid"] for *_rest, v, _tree in self.synthetic.halos]
        self.assertLess(min(ids), 0)
        self.assertGreater(len(ids), len(set(ids)))
        self.assertIn(-1.0, [v["m_crit"] for *_rest, v, _tree in self.synthetic.halos])
        # the converter preserved the sentinel and the signed zero as data
        self.assertEqual(float(get_value(self.dataset, 0, 3, "M_Crit200")), -1.0)
        self.assertTrue(np.signbit(get_value(self.dataset, 0, 1, "Pos")[1]))

    def test_every_run_is_recorded_with_its_measurements(self):
        document = json.loads(self.record.read_text())
        self.assertEqual(document["record_format"], acc.RECORD_FORMAT)
        (entry,) = [run for run in document["runs"] if run["label"] == "convert"]
        self.assertEqual(
            [s["label"] for s in entry["stages"]],
            ["convert-" + stage for stage in acc.CONVERT_STAGES],
        )
        for stage in entry["stages"]:
            self.assertEqual(stage["exit_code"], 0)
            self.assertGreater(stage["wall_seconds"], 0)
            self.assertGreater(stage["peak_rss_bytes"], 1 << 20)
            self.assertEqual(len(stage["executable"]["sha256"]), 64)
            self.assertGreater(stage["workdir_storage"]["apparent_bytes"], 0)
            self.assertTrue(Path(stage["stdout"]).is_file())
        storage = entry["dataset_storage"]
        self.assertEqual(storage["halos"], len(self.synthetic.halos))
        # the default L-Halo v3 row: 5 int64 links, 3 int32 target snapshots,
        # 3 int64 identities, Len/SnapNum int32, MostBoundID int64 and 4+3*12+4+4
        # bytes of float payload
        self.assertEqual(storage["logical_bytes_per_halo"], 140)
        self.assertEqual(len(entry["code"]["git_commit"]), 40)
        self.assertEqual(len(entry["code"]["harness_sha256"]), 64)
        self.assertEqual(entry["exit_status"], 0)

    def test_compare_cli_records_spill_widths_and_verdict(self):
        record = self.work / "record.json"
        report = self.work / "report.json"
        code, out, _err = run_harness(
            [
                "compare",
                "--record",
                record,
                "--dataset",
                self.dataset,
                "--dump",
                self.dump,
                "--source-format",
                "lhalo_binary",
                "--budget-mb",
                "0.01",
                "--block-rows",
                "2",
                "--report",
                report,
                "--hash-sources",
                "--spill-dir",
                self.work,
            ]
        )
        self.assertEqual(code, 0, out)
        (entry,) = json.loads(record.read_text())["runs"]
        self.assertEqual(entry["result"]["verdict"], "PASS")
        self.assertGreater(entry["wall_seconds"], 0)
        self.assertGreater(entry["peak_rss_bytes"], 1 << 20)
        self.assertIn("sha256", entry["inputs"][0])
        sorts = entry["resources"]["sorts"]
        self.assertEqual(len(sorts), 4)
        for sort in sorts:
            self.assertGreater(sort["record_bytes"], 0)
        halo_sort = sorts[0]
        self.assertEqual(halo_sort["record_bytes"], acc.HALO_DTYPE.itemsize)
        self.assertGreater(halo_sort["runs"], 1, "the tiny budget must force spill runs")
        self.assertGreater(halo_sort["spilled_bytes"], 0)
        self.assertEqual(json.loads(report.read_text())["verdict"], "PASS")

    def test_unusable_input_is_still_recorded(self):
        record = self.work / "record.json"
        truncated = self.copy_dump(lambda text: text.rsplit("# end", 1)[0])
        missing = self.work / "no-such.dump"
        for dump in (truncated, missing):
            code, _out, err = run_harness(
                [
                    "compare",
                    "--record",
                    record,
                    "--dataset",
                    self.dataset,
                    "--dump",
                    dump,
                    "--source-format",
                    "lhalo_binary",
                    "--source",
                    self.dataset,
                ]
            )
            self.assertEqual(code, acc.EXIT_ERROR, err)
        truncated_entry, missing_entry = json.loads(record.read_text())["runs"]
        for entry, dump, error in (
            (truncated_entry, truncated, "AcceptanceError"),
            (missing_entry, missing, "FileNotFoundError"),
        ):
            self.assertEqual(entry["label"], "compare")
            self.assertEqual(entry["kind"], "error")
            self.assertEqual(entry["exit_code"], acc.EXIT_ERROR)
            self.assertEqual(entry["exit_status"], acc.EXIT_ERROR)
            self.assertTrue(entry["error"].startswith(error), entry["error"])
            self.assertIn("compare", entry["command"])
            self.assertIn(str(dump), entry["command"])
            self.assertGreaterEqual(entry["wall_seconds"], 0)
            self.assertGreater(entry["peak_rss_bytes"], 0)
            self.assertEqual(len(entry["code"]["git_commit"]), 40)
            self.assertTrue(entry["recorded_sources"])
        self.assertIn("truncated", truncated_entry["error"])
        code, _out, _err = run_harness(
            [
                "compare-extras",
                "--record",
                record,
                "--dataset",
                self.dataset,
                "--source-format",
                "lhalo_binary",
                "--column-map",
                self.work / "absent.yaml",
                "--source-dir",
                self.source,
                "--tree-name",
                SyntheticLHalo.TREE_NAME,
                "--first-file",
                "0",
                "--last-file",
                "1",
                "--halo-properties",
                MICRO / "halo_properties.yaml",
            ]
        )
        self.assertEqual(code, acc.EXIT_ERROR)
        entry = json.loads(record.read_text())["runs"][-1]
        self.assertEqual((entry["label"], entry["kind"]), ("compare-extras", "error"))
        self.assertIn("absent.yaml", entry["error"])

    def test_a_failure_never_counts_zero(self):
        findings = acc.Findings()
        findings.declare("check", "test")
        findings.fail("check", 0, ["an empty file's defect"])
        self.assertEqual(findings.checks["check"]["failures"], 1)
        self.assertEqual(findings.failed, ["check"])

    def test_cli_exit_codes(self):
        record = self.work / "record.json"
        dataset = self.copy_dataset()
        set_value(dataset, 0, 6, "Vmax", 123.0)
        code, _out, _err = run_harness(
            [
                "compare",
                "--record",
                record,
                "--dataset",
                dataset,
                "--dump",
                self.dump,
                "--source-format",
                "lhalo_binary",
            ]
        )
        self.assertEqual(code, acc.EXIT_FAIL)
        truncated = self.copy_dump(lambda text: text.rsplit("# end", 1)[0])
        code, _out, err = run_harness(
            [
                "compare",
                "--record",
                record,
                "--dataset",
                self.dataset,
                "--dump",
                truncated,
                "--source-format",
                "lhalo_binary",
            ]
        )
        self.assertEqual(code, acc.EXIT_ERROR)
        self.assertIn("truncated", err)

    # -- dropped, duplicated and extra rows -----------------------------------

    def test_dropped_row_is_detected(self):
        dataset = self.copy_dataset()
        path, row = locate(dataset, 2, 0)  # the lone halo at snapshot 30
        rewrite_rows(path, lambda data: np.delete(data, row, axis=0))
        report = compare(dataset, self.dump)
        self.assertFailsExactly(report, "row_coverage")
        self.assertIn(
            "(ForestIndex=2, HaloRankInForest=0) is absent from the conversion",
            report["checks"]["row_coverage"]["samples"],
        )

    def test_duplicated_converted_row_is_detected(self):
        dataset = self.copy_dataset()
        path, row = locate(dataset, 2, 0)
        rewrite_rows(path, lambda data: np.concatenate([data, data[row : row + 1]]))
        self.assertFailsExactly(compare(dataset, self.dump), "duplicate_converted_rows")

    def test_duplicated_reference_row_is_detected(self):
        def duplicate(text):
            lines = text.splitlines()
            data = [line for line in lines if not line.startswith("#")]
            body = lines[:4] + data + [data[0]]
            return "\n".join(body + ["# end rows {} forests 4".format(len(data) + 1)]) + "\n"

        report = compare(self.dataset, self.copy_dump(duplicate))
        self.assertFailsExactly(report, "duplicate_reference_rows")

    def test_extra_converted_row_is_detected(self):
        dataset = self.copy_dataset()
        path, row = locate(dataset, 2, 0)

        def add_phantom(data):
            extra = data[row : row + 1].copy()
            return np.concatenate([data, extra])

        with h5py.File(path, "r") as handle:
            names = list(handle["halos"].keys())
        with h5py.File(path, "r+") as handle:
            for name in names:
                data = add_phantom(handle["halos"][name][...])
                if name == "HaloRankInForest":
                    data[-1] = 1  # a phantom second halo in forest 2
                if name == "SourceHaloID":
                    data[-1] = 10**6
                if name == "FirstHaloInFOFgroup":
                    data[-1] = len(data) - 1
                dtype = handle["halos"][name].dtype
                del handle["halos"][name]
                handle["halos"].create_dataset(name, data=data, dtype=dtype)
        report = compare(dataset, self.dump)
        self.assertFailsExactly(report, "row_coverage")
        self.assertIn("absent from the reference", report["checks"]["row_coverage"]["samples"][0])

    # -- topology ------------------------------------------------------------

    def test_wrong_target_snapshot_is_detected(self):
        dataset = self.copy_dataset()
        # D (rank 3, snap 46) descends into A at 49 across a gap; claim 48 instead
        set_value(dataset, 0, 3, "DescendantSnapshot", 48)
        report = compare(dataset, self.dump)
        self.assertIn("target_snapshot_Descendant", report["failed_checks"])
        self.assertIn("link_Descendant", report["failed_checks"])
        self.assertEqual(report["checks"]["target_snapshot_Descendant"]["failures"], 1)

    def test_reordered_progenitor_chain_is_detected(self):
        dataset = self.copy_dataset()
        _c_path, c_row = locate(dataset, 0, 2)
        _d_path, d_row = locate(dataset, 0, 3)
        # A: FirstProgenitor C(48) -> D(46); D.NextProgenitor -> C; C.NextProgenitor -> none
        set_value(dataset, 0, 0, "FirstProgenitor", d_row)
        set_value(dataset, 0, 0, "FirstProgenitorSnapshot", 46)
        set_value(dataset, 0, 3, "NextProgenitor", c_row)
        set_value(dataset, 0, 3, "NextProgenitorSnapshot", 48)
        set_value(dataset, 0, 2, "NextProgenitor", -1)
        set_value(dataset, 0, 2, "NextProgenitorSnapshot", -1)
        report = compare(dataset, self.dump)
        for check in (
            "link_FirstProgenitor",
            "link_NextProgenitor",
            "target_snapshot_FirstProgenitor",
            "target_snapshot_NextProgenitor",
        ):
            self.assertIn(check, report["failed_checks"])
        self.assertEqual(report["checks"]["link_NextProgenitor"]["failures"], 2)

    def test_reordered_fof_chain_is_detected(self):
        dataset = self.copy_dataset()
        _b_path, b_row = locate(dataset, 0, 1)
        _g_path, g_row = locate(dataset, 0, 7)
        # FoF chain A -> B -> G becomes A -> G -> B
        set_value(dataset, 0, 0, "NextHaloInFOFgroup", g_row)
        set_value(dataset, 0, 7, "NextHaloInFOFgroup", b_row)
        set_value(dataset, 0, 1, "NextHaloInFOFgroup", -1)
        report = compare(dataset, self.dump)
        self.assertFailsExactly(report, "link_NextHaloInFOFgroup")
        self.assertEqual(report["checks"]["link_NextHaloInFOFgroup"]["failures"], 3)

    def test_link_to_a_missing_row_is_detected(self):
        dataset = self.copy_dataset()
        set_value(dataset, 0, 4, "Descendant", 999)
        report = compare(dataset, self.dump)
        self.assertFailsExactly(report, "converter_link_targets", "link_Descendant")

    def test_malformed_null_link_is_detected(self):
        dataset = self.copy_dataset()
        set_value(dataset, 0, 6, "FirstProgenitor", -7)
        set_value(dataset, 0, 6, "FirstProgenitorSnapshot", 3)
        report = compare(dataset, self.dump)
        self.assertIn("converter_link_encoding", report["failed_checks"])
        self.assertIn("link_FirstProgenitor", report["failed_checks"])

    # -- payload -------------------------------------------------------------

    def test_changed_field_values_are_detected_bit_exactly(self):
        cases = [
            ("Pos", 1, None, "payload_Pos"),  # flip one ulp below
            ("M_Crit200", None, 1.0, "payload_M_Crit200"),  # the negative sentinel's sign
            ("MostBoundID", None, 778, "payload_MostBoundID"),
            ("Len", None, 0, "payload_Len"),
            ("Spin", 2, 0.5, "payload_Spin"),
            ("SnapNum", None, 48, "snapnum"),
        ]
        for name, component, value, check in cases:
            with self.subTest(name=name):
                dataset = self.work / ("dataset_" + name)
                shutil.copytree(self.dataset, dataset)
                forest, rank = (0, 3) if name == "M_Crit200" else (0, 1)
                if value is None:
                    current = get_value(dataset, forest, rank, name)
                    value = np.nextafter(current[component], np.float32(-np.inf))
                set_value(dataset, forest, rank, name, value, component)
                report = compare(dataset, self.dump)
                self.assertFailsExactly(report, check)
                self.assertEqual(report["checks"][check]["failures"], 1)

    def test_signed_zero_change_is_detected(self):
        dataset = self.copy_dataset()
        set_value(dataset, 0, 1, "Pos", 0.0, component=1)  # -0.0 -> +0.0
        self.assertFailsExactly(compare(dataset, self.dump), "payload_Pos")

    def test_wrong_source_halo_id_is_detected(self):
        dataset = self.copy_dataset()
        set_value(dataset, 0, 6, "SourceHaloID", 10**9)
        self.assertFailsExactly(compare(dataset, self.dump), "source_halo_id")

    def test_non_binary32_payload_storage_fails_even_with_exact_values(self):
        dataset = self.copy_dataset()
        paths = sorted(dataset.glob("snapshot_*.h5"))
        for path in paths:
            retype(path, "Vmax", "<f8")
        report = compare(dataset, self.dump)
        self.assertFailsExactly(report, "payload_storage")
        self.assertEqual(report["checks"]["payload_storage"]["failures"], len(paths))
        # the values themselves, cast back to binary32, were still compared
        self.assertGreater(report["checks"]["payload_Vmax"]["compared"], 0)

    def test_empty_snapshot_storage_defect_alone_fails(self):
        dataset = self.copy_dataset()
        empty = empty_snapshot(dataset)
        retype(empty, "Vmax", "<f8")
        report = compare(dataset, self.dump)
        self.assertFailsExactly(report, "payload_storage")
        self.assertEqual(report["checks"]["payload_storage"]["failures"], 1)
        self.assertIn(empty.name, report["checks"]["payload_storage"]["samples"][0])
        self.assertIn("(0 rows)", report["checks"]["payload_storage"]["samples"][0])

    def test_empty_snapshot_storage_defect_does_not_mask_populated_corruption(self):
        """A past false PASS: one empty file's wrong dtype must not
        silence the value comparison of that field in the populated files."""
        dataset = self.copy_dataset()
        retype(empty_snapshot(dataset), "Vmax", "<f8")
        set_value(dataset, 0, 0, "Vmax", 123.0)
        report = compare(dataset, self.dump)
        self.assertFailsExactly(report, "payload_storage", "payload_Vmax")
        self.assertEqual(report["checks"]["payload_Vmax"]["failures"], 1)
        self.assertIn(
            "(ForestIndex=0, HaloRankInForest=0)", report["checks"]["payload_Vmax"]["samples"][0]
        )

    # -- columns the comparison must ignore ------------------------------------

    def test_extra_columns_are_ignored(self):
        dataset = self.copy_dataset()
        for path in sorted(dataset.glob("snapshot_*.h5")):
            with h5py.File(path, "r+") as handle:
                rows = handle["halos"]["SourceHaloID"].shape[0]
                handle["halos"].create_dataset("UnrelatedExtra", data=np.full(rows, 7.5))

        def add_column(text):
            lines = text.splitlines()
            lines[2] += " future_column"
            lines = [line if line.startswith("#") else line + " 42" for line in lines]
            return "\n".join(lines) + "\n"

        dump = self.copy_dump(add_column)
        report = compare(dataset, dump)
        self.assertEqual(report["verdict"], "PASS", report["failed_checks"])
        self.assertEqual(report["dump"]["ignored_columns"], ["future_column"])

    # -- a broken reference is unusable, not a pass -----------------------------

    def test_malformed_dumps_are_refused(self):
        cases = {
            "truncated": lambda t: t.rsplit("# end", 1)[0],
            "version": lambda t: t.replace("mimic-source-dump v1", "mimic-source-dump v2", 1),
            "ragged": lambda t: t.replace("\n0 1 ", "\n0 1 5 ", 1),
            "trailer count": lambda t: t.replace("# end rows 12", "# end rows 11"),
            "missing column": lambda t: t.replace(" vmax\n", "\n", 1),
            "comment in data": lambda t: t.replace("\n0 1 ", "\n# note\n0 1 ", 1),
            "non-hex float": lambda t: t.replace(_float_bits(9.0), "zzzzzzzz", 1),
            "reader": lambda t: t.replace("reader lhalo_binary", "reader consistent_trees_hdf5"),
        }
        for label, transform in cases.items():
            with self.subTest(label):
                dump = self.copy_dump(transform)
                with self.assertRaises(acc.AcceptanceError):
                    compare(self.dataset, dump)

    def test_broken_rank_density_in_the_reference_fails(self):
        dump = self.copy_dump(lambda t: t.replace("\n0 7 ", "\n0 8 ", 1))
        report = compare(self.dataset, dump)
        self.assertIn("dump_integrity", report["failed_checks"])

    def test_wrong_format_version_is_a_dataset_failure(self):
        dataset = self.copy_dataset()
        path = sorted(dataset.glob("snapshot_*.h5"))[0]
        with h5py.File(path, "r+") as handle:
            handle["header"].attrs["format_version"] = np.int32(2)
        report = compare(dataset, self.dump)
        self.assertFailsExactly(report, "dataset_integrity")

    def test_nothing_compared_is_not_a_pass(self):
        report = acc.compare_streams(
            iter(()), lambda: iter(()), lambda: iter(()), "lhalo_binary", 1 << 16
        )
        self.assertEqual(report["verdict"], "FAIL")
        self.assertIn("nothing_compared", report["failed_checks"])

    def test_non_positive_block_rows_is_a_recorded_usage_error(self):
        # a negative size made every read range empty: nothing converted was read
        record = self.work / "record.json"
        for value in ("-3", "0"):
            with self.subTest(block_rows=value):
                code, _out, err = run_harness(
                    [
                        "compare",
                        "--record",
                        record,
                        "--dataset",
                        self.dataset,
                        "--dump",
                        self.dump,
                        "--source-format",
                        "lhalo_binary",
                        "--block-rows",
                        value,
                    ]
                )
                self.assertEqual(code, acc.EXIT_ERROR, err)
                self.assertIn("--block-rows must be positive", err)
                entry = json.loads(record.read_text())["runs"][-1]
                self.assertEqual((entry["kind"], entry["exit_status"]), ("error", acc.EXIT_ERROR))
                self.assertIn("--block-rows", entry["error"])

    def test_snapshot_files_are_read_in_numeric_not_lexicographic_order(self):
        dataset = self.copy_dataset()
        # "snapshot_10.h5" sorts after "snapshot_049.h5" as text, before it as a number
        (dataset / "snapshot_010.h5").rename(dataset / "snapshot_10.h5")
        numbers = [number for number, _path, _rows in acc.V3Dataset(dataset).files]
        self.assertEqual(numbers, sorted(numbers))
        self.assertIn(10, numbers)
        report = compare(dataset, self.dump)
        self.assertEqual(report["verdict"], "PASS", report["failed_checks"])
        self.assertEqual(report["matched_rows"], len(self.synthetic.halos))

    def test_two_files_of_one_snapshot_are_a_dataset_failure(self):
        dataset = self.copy_dataset()
        shutil.copy2(dataset / "snapshot_010.h5", dataset / "snapshot_10.h5")
        report = compare(dataset, self.dump)
        self.assertFailsExactly(report, "dataset_integrity")
        self.assertIn("both hold snapshot 10", report["checks"]["dataset_integrity"]["samples"][0])

    def test_a_comparison_declares_its_checks_once(self):
        with mock.patch.object(acc, "_declare_checks", wraps=acc._declare_checks) as declare:
            report = compare(self.dataset, self.dump)
        self.assertEqual(report["verdict"], "PASS", report["failed_checks"])
        self.assertEqual(declare.call_count, 1)

    def test_both_comparisons_report_an_uncomparable_dataset_alike(self):
        dataset = self.copy_dataset()
        with h5py.File(sorted(dataset.glob("snapshot_*.h5"))[0], "r+") as handle:
            handle["header"].attrs["format_version"] = np.int32(2)
        topology = compare(dataset, self.dump)
        extras = acc.compare_extras(
            dataset,
            "lhalo_binary",
            REPO_ROOT / "convert" / "mimic-convert" / "profiles" / "lhalo_binary.yaml",
            {},
            1 << 20,
            None,
            3,
        )
        for report in (topology, extras):
            self.assertEqual(
                set(report),
                {"report_format", "verdict", "failed_checks", "source_format", "checks"},
            )
            self.assertFailsExactly(report, "dataset_integrity")
        self.assertEqual(
            topology["checks"]["dataset_integrity"], extras["checks"]["dataset_integrity"]
        )


# ==========================================================================
# 64-bit bounded sort, join and comparison
# ==========================================================================


class BoundedSixtyFourBitTests(unittest.TestCase):
    def setUp(self):
        self.spill = Path(tempfile.mkdtemp(prefix="acceptance_spill_"))
        self.addCleanup(shutil.rmtree, self.spill, ignore_errors=True)

    def test_external_sort_is_exact_above_2_53_over_many_merge_passes(self):
        rng = np.random.default_rng(20260924)
        dtype = np.dtype([("a", "<i8"), ("b", "<i8"), ("payload", "<i8")])
        rows = np.zeros(5000, dtype=dtype)
        # values straddling 2^53 and 2^63 and negative, where float64 would collide
        rows["a"] = rng.integers(0, 4, size=len(rows)) + (1 << 53) - 2
        rows["b"] = rng.integers(-(1 << 62), 1 << 62, size=len(rows)) | 1
        rows["b"][:10] = [np.iinfo(np.int64).max - i for i in range(10)]
        rows["payload"] = np.arange(len(rows))
        budget = 20 * (dtype.itemsize + 24) * 2  # 20-record runs
        with acc.ExternalSorter(dtype, ("a", "b"), budget, self.spill, "test") as sorter:
            for start in range(0, len(rows), 333):
                sorter.add(rows[start : start + 333])
            out = np.concatenate(list(sorter.sorted_blocks()))
            stats = sorter.stats
        expected = rows[np.lexsort((rows["b"], rows["a"]))]
        np.testing.assert_array_equal(out[["a", "b"]], expected[["a", "b"]])
        self.assertEqual(sorted(out["payload"].tolist()), list(range(len(rows))))
        self.assertGreaterEqual(stats["runs"], 200)
        self.assertGreaterEqual(stats["merge_passes"], 2, stats)
        self.assertEqual(stats["fan_in"], 5)

    def test_lookup_join_matches_row_indices_above_2_31_in_tiny_windows(self):
        rng = np.random.default_rng(7)
        big = (1 << 31) + 11
        one = np.zeros(300, dtype=[("snap", "<i8"), ("row", "<i8"), ("value", "<i8")])
        one["snap"] = np.repeat([3, 9, 40], 100)
        one["row"] = np.tile(np.arange(big, big + 100), 3)
        one["value"] = (1 << 60) + np.arange(300)
        picks = rng.integers(0, 300, size=900)
        many = np.zeros(900, dtype=[("s", "<i8"), ("r", "<i8"), ("want", "<i8")])
        many["s"], many["r"], many["want"] = one["snap"][picks], one["row"][picks], picks
        many = many[np.lexsort((many["r"], many["s"]))]
        # also a request for a row that does not exist
        orphan = np.array([(9, big + 1000, -1)], dtype=many.dtype)
        many = np.concatenate([many[many["s"] <= 9], orphan, many[many["s"] > 9]])
        stats = {}
        got = {}
        unmatched = 0
        for m, match, o, _matched in acc.lookup_join(
            (many[i : i + 7] for i in range(0, len(many), 7)),
            ("s", "r"),
            many.dtype,
            (one[i : i + 5] for i in range(0, len(one), 5)),
            ("snap", "row"),
            one.dtype,
            stats,
        ):
            for row, index in zip(m, match):
                if index < 0:
                    unmatched += 1
                    self.assertEqual(int(row["want"]), -1)
                else:
                    self.assertEqual(int(o["value"][index]), (1 << 60) + int(row["want"]))
                    got[int(row["want"])] = got.get(int(row["want"]), 0) + 1
        self.assertEqual(unmatched, 1)
        self.assertEqual(sum(got.values()), 900)
        self.assertLess(stats["max_window_rows"], 200)

    def test_whole_comparison_over_virtual_wide_indices(self):
        """Forest numbers above 2^53 and snapshot rows above 2^31, no files."""
        forest0 = (1 << 53) + 1
        row0 = (1 << 31) + 5
        n_forests, per_snap = 40, 40
        snaps = (10, 12)
        # forest f: halo rank 0 at snap 10 (row0+f), rank 1 at snap 12 (row0+f),
        # a gapped descendant link 0 -> 1 and its reverse progenitor link
        reference = np.zeros(2 * n_forests, dtype=acc.HALO_DTYPE)
        reference["ForestIndex"] = np.repeat(forest0 + np.arange(n_forests), 2)
        reference["HaloRankInForest"] = np.tile([0, 1], n_forests)
        reference["SnapNum"] = np.tile(snaps, n_forests)
        reference["FileSnapshot"] = reference["SnapNum"]
        for link in acc.LINKS:
            reference[link + "_forest"] = acc.NULL
            reference[link + "_rank"] = acc.NULL
        for link in acc.QUALIFIED:
            reference[link + "_snap"] = acc.NULL
        early = reference["HaloRankInForest"] == 0
        reference["Descendant_forest"][early] = reference["ForestIndex"][early]
        reference["Descendant_rank"][early] = 1
        reference["Descendant_snap"][early] = 12
        reference["FirstProgenitor_forest"][~early] = reference["ForestIndex"][~early]
        reference["FirstProgenitor_rank"][~early] = 0
        reference["FirstProgenitor_snap"][~early] = 10
        reference["FirstHaloInFOFgroup_forest"] = reference["ForestIndex"]
        reference["FirstHaloInFOFgroup_rank"] = reference["HaloRankInForest"]
        reference["Len"] = 5
        reference["MostBoundID"] = -3
        reference["M_Crit200"] = 0x3F800000

        def converted_raw(corrupt=False):
            for snap_index, snap in enumerate(snaps):
                rows = row0 + np.arange(per_snap, dtype=np.int64)
                forest = forest0 + np.arange(n_forests, dtype=np.int64)
                zeros = np.zeros(per_snap, dtype=np.int64)
                null = np.full(per_snap, -1, dtype=np.int64)
                columns = {
                    "ForestIndex": forest,
                    "HaloRankInForest": zeros + snap_index,
                    "SnapNum": zeros + snap,
                    "SourceHaloID": (1 << 40) + 2 * np.arange(per_snap) + snap_index,
                    "Len": zeros + 5,
                    "MostBoundID": zeros - 3,
                    "Descendant": rows.copy() if snap == 10 else null,
                    "DescendantSnapshot": zeros + 12 if snap == 10 else null,
                    "FirstProgenitor": rows.copy() if snap == 12 else null,
                    "FirstProgenitorSnapshot": zeros + 10 if snap == 12 else null,
                    "NextProgenitor": null,
                    "NextProgenitorSnapshot": null,
                    "FirstHaloInFOFgroup": rows.copy(),
                    "NextHaloInFOFgroup": null,
                    "M_Crit200": np.full(per_snap, 0x3F800000, dtype=np.uint32),
                }
                for name, components in acc.FLOAT_PAYLOAD:
                    if name != "M_Crit200":
                        shape = per_snap if components == 1 else (per_snap, components)
                        columns[name] = np.zeros(shape, dtype=np.uint32)
                if corrupt and snap == 10:
                    columns["Descendant"][17] += 1  # a neighbouring row, same snapshot
                yield snap, rows, columns

        def identities():
            for snap_index, snap in enumerate(snaps):
                out = np.zeros(per_snap, dtype=acc.IDENTITY_DTYPE)
                out["snap"] = snap
                out["row"] = row0 + np.arange(per_snap)
                out["forest"] = forest0 + np.arange(n_forests)
                out["rank"] = snap_index
                yield out

        def run(corrupt):
            blocks = (reference[i : i + 9] for i in range(0, len(reference), 9))
            return acc.compare_streams(
                blocks,
                lambda: converted_raw(corrupt),
                identities,
                "consistent_trees_ascii",
                budget_bytes=16 * 1024,
                spill_dir=self.spill,
            )

        clean = run(False)
        self.assertEqual(clean["verdict"], "PASS", clean["failed_checks"])
        self.assertEqual(clean["matched_rows"], 2 * n_forests)
        for sort in clean["resources"]["sorts"]:
            self.assertGreater(sort["runs"], 1, sort["label"])
        broken = run(True)
        self.assertEqual(broken["failed_checks"], ["link_Descendant"])
        self.assertIn(
            "(ForestIndex={}, HaloRankInForest=0)".format(forest0 + 17),
            broken["checks"]["link_Descendant"]["samples"][0],
        )

    def test_dedupe_catches_a_repeat_across_a_block_boundary(self):
        dtype = np.dtype([("k", "<i8")])
        blocks = [
            np.array([(1,), (1 << 62,)], dtype=dtype),
            np.array([(1 << 62,), (5 << 60)], dtype=dtype),
        ]
        findings = acc.Findings()
        findings.declare("dup", "test")
        out = list(acc.dedupe(blocks, ("k",), findings, "dup", lambda r: int(r["k"])))
        self.assertEqual(findings.checks["dup"]["failures"], 1)
        self.assertEqual(sum(len(b) for b in out), 3)

    def test_hex_bit_patterns_parse_exactly(self):
        tokens = np.array(["00000000", "80000000", "7fc00001", "ffffffff", "3F800000"])
        np.testing.assert_array_equal(
            acc.parse_hex32(tokens, "t"),
            np.array([0, 0x80000000, 0x7FC00001, 0xFFFFFFFF, 0x3F800000], dtype=np.uint32),
        )


# ==========================================================================
# Extras against independent source extraction
# ==========================================================================


def write_profile(path, base, extras):
    document = yaml.safe_load(Path(base).read_text())
    document["extra_fields"] = extras
    path.write_text(yaml.safe_dump(document, sort_keys=False))
    return path


def extra(name, sources, type_name, units="dimensionless"):
    return {
        "name": name,
        "sources": sources,
        "type": type_name,
        "units": units,
        "h_convention": "none",
        "description": "acceptance self-test extra",
    }


def rows_in(path):
    with h5py.File(path, "r") as handle:
        return handle["halos"]["SourceHaloID"].shape[0]


def edit_ids(path, edit):
    """Apply ``edit`` in place to one snapshot file's SourceHaloID array."""
    with h5py.File(path, "r+") as handle:
        ids = handle["halos"]["SourceHaloID"][...]
        edit(ids)
        handle["halos"]["SourceHaloID"][...] = ids


def flip_extra(dataset, name):
    """Change the first converted value of ``name`` by one unit in its last place."""
    path = sorted(Path(dataset).glob("snapshot_*.h5"))[-1]
    with h5py.File(path, "r+") as handle:
        data = handle["halos"][name]
        value = data[0]
        if data.dtype.kind == "f":
            flat = np.atleast_1d(value).copy()
            flat[0] = np.nextafter(flat[0], np.inf)
            data[0] = flat if data.ndim > 1 else flat[0]
        else:
            flat = np.atleast_1d(value).copy()
            flat[0] += 1
            data[0] = flat if data.ndim > 1 else flat[0]


class ExtrasTests(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="acceptance_extras_"))
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)
        self.record = self.tmp / "record.json"

    def extras_cli(self, dataset, source_format, profile, inventory):
        report = self.tmp / "extras.json"
        code, out, err = run_harness(
            [
                "compare-extras",
                "--record",
                self.record,
                "--dataset",
                dataset,
                "--source-format",
                source_format,
                "--column-map",
                profile,
                "--budget-mb",
                "0.02",
                "--block-rows",
                "2",
                "--report",
                report,
            ]
            + inventory
        )
        return code, json.loads(report.read_text()) if report.exists() else None, out + err

    def check_route(self, source_format, profile, ingest, inventory, flip, sim_info):
        dataset = convert_with_harness(self.tmp, ingest, self.record, sim_info)
        code, report, text = self.extras_cli(dataset, source_format, profile, inventory)
        self.assertEqual(code, 0, text + json.dumps(report and report["checks"], indent=1))
        self.assertGreater(report["matched_rows"], 0)
        for name in report["extras"]:
            self.assertGreater(report["checks"]["extra_" + name]["compared"], 0, name)
        broken = self.tmp / "broken"
        shutil.copytree(dataset, broken)
        flip_extra(broken, flip)
        code, report, text = self.extras_cli(broken, source_format, profile, inventory)
        self.assertEqual(code, acc.EXIT_FAIL, text)
        self.assertEqual(report["failed_checks"], ["extra_" + flip])
        self.assertEqual(report["checks"]["extra_" + flip]["failures"], 1)
        return dataset

    def test_lhalo_binary_extras_including_signed_duplicate_ids_and_sentinels(self):
        source = SyntheticLHalo().write_source(self.tmp / "source")
        profile = write_profile(
            self.tmp / "profile.yaml",
            REPO_ROOT / "convert" / "mimic-convert" / "profiles" / "lhalo_binary.yaml",
            [
                extra("RawParticleID", [{"field": "MostBoundID"}], "long long"),
                extra("RawMassSentinel", [{"field": "M_Crit200"}], "float", "1e10 Msun/h"),
                extra("MeanMass", [{"field": "M_Mean200"}], "float", "1e10 Msun/h"),
                extra("Subhalo", [{"field": "SubhaloIndex"}], "int"),
                extra(
                    "SpinCopy",
                    [{"field": "Spin", "component": c} for c in (2, 1, 0)],
                    "vec3_float",
                    "Mpc/h km/s",
                ),
            ],
        )
        dataset = self.check_route(
            "lhalo_binary",
            profile,
            lhalo_ingest_args(source, profile),
            [
                "--source-dir",
                source,
                "--tree-name",
                SyntheticLHalo.TREE_NAME,
                "--first-file",
                "0",
                "--last-file",
                "1",
                "--halo-properties",
                MICRO / "halo_properties.yaml",
            ],
            "RawParticleID",
            MICRO / "simulation_info.yaml",
        )
        # an extra stored differently in one empty snapshot is a dataset failure
        inconsistent = self.tmp / "inconsistent"
        shutil.copytree(dataset, inconsistent)
        retype(empty_snapshot(inconsistent), "MeanMass", "<f8")
        code, report, text = self.extras_cli(
            inconsistent,
            "lhalo_binary",
            profile,
            [
                "--source-dir",
                source,
                "--tree-name",
                SyntheticLHalo.TREE_NAME,
                "--first-file",
                "0",
                "--last-file",
                "1",
                "--halo-properties",
                MICRO / "halo_properties.yaml",
            ],
        )
        self.assertEqual(code, acc.EXIT_FAIL, text)
        self.assertEqual(report["failed_checks"], ["dataset_integrity"])

    def test_forests_hdf5_extras(self):
        package = SIMULATIONS / "micro-uchuu-hdf5"
        fixture = package / "_tests" / "data" / "MicroUchuu_test_mergertree_info.h5"
        profile = write_profile(
            self.tmp / "profile.yaml",
            package / "converter_columns.yaml",
            [
                extra("RawMvir", [{"field": "Mvir"}], "double", "Msun/h"),
                extra("CatalogID", [{"field": "id"}], "long long"),
                extra("RawJz", [{"field": "Jz"}], "double", "Msun/h Mpc/h km/s"),
            ],
        )
        ingest = [
            "--source-format",
            "consistent_trees_hdf5",
            "--simulation-info",
            package / "simulation_info.yaml",
            "--a-list",
            package / "micro-uchuu.a_list",
            "--column-map",
            profile,
            "--info-file",
            fixture,
            "--first-file",
            "0",
            "--last-file",
            "0",
        ]
        self.check_route(
            "consistent_trees_hdf5",
            profile,
            [str(a) for a in ingest],
            ["--info-file", fixture, "--first-file", "0", "--last-file", "0"],
            "RawMvir",
            package / "simulation_info.yaml",
        )

    def test_ascii_extras(self):
        package = SIMULATIONS / "micro-uchuu-ascii"
        data = package / "_tests" / "data"
        profile = write_profile(
            self.tmp / "profile.yaml",
            package / "converter_columns.yaml",
            [
                extra("Radius", [{"field": "Rvir"}], "float", "kpc/h"),
                extra("Progenitors", [{"field": "num_prog"}], "int"),
                extra("RootID", [{"field": "Tree_root_ID"}], "long long"),
                extra("RawJ", [{"field": f} for f in ("Jx", "Jy", "Jz")], "vec3_float", "raw J"),
            ],
        )
        ingest = [
            "--source-format",
            "consistent_trees_ascii",
            "--simulation-info",
            package / "simulation_info.yaml",
            "--a-list",
            package / "micro-uchuu.a_list",
            "--column-map",
            profile,
            "--forests-list",
            data / "forests.list",
            "--tree-file",
            data / "tree_0_0_0.dat",
            "--ingest-max-rows",
            "4096",
        ]
        self.check_route(
            "consistent_trees_ascii",
            profile,
            [str(a) for a in ingest],
            ["--forests-list", data / "forests.list", "--tree-file", data / "tree_0_0_0.dat"],
            "RootID",
            package / "simulation_info.yaml",
        )

    def test_ascii_extras_with_a_forest_spanning_two_files(self):
        """Forest 100's two trees lie in different files, between other
        forests: the catalogue-key join still matches every row and every
        extra, whatever file a forest's halos come from."""
        import fixtures

        source = self.tmp / "spanning"
        source.mkdir()
        multi, satellite, early, zero_mass, sub = fixtures.standard_forests()
        tree_files = [
            fixtures.write_ctrees_file(
                source / "tree_0.dat", [multi.trees[0], satellite.trees[0], early.trees[0]]
            ),
            fixtures.write_ctrees_file(
                source / "tree_1.dat", [zero_mass.trees[0], multi.trees[1], sub.trees[0]]
            ),
        ]
        forests_list = fixtures.write_forests_list(
            source / "forests.list", [multi, satellite, early, zero_mass, sub]
        )
        # the package's metadata names the route (input.tree_type); the
        # fixture's own a_list numbers its snapshots
        sim_info = SIMULATIONS / "micro-uchuu-ascii" / "simulation_info.yaml"
        profile = write_profile(
            self.tmp / "profile.yaml",
            REPO_ROOT / "convert" / "mimic-convert" / "profiles" / "consistent_trees_ascii.yaml",
            [
                extra("Radius", [{"field": "Rvir"}], "float", "kpc/h"),
                extra("Progenitors", [{"field": "num_prog"}], "int"),
                extra("RootID", [{"field": "Tree_root_ID"}], "long long"),
            ],
        )
        ingest = [
            "--source-format",
            "consistent_trees_ascii",
            "--simulation-info",
            sim_info,
            "--a-list",
            fixtures.write_a_list(source / "fixture.a_list"),
            "--column-map",
            profile,
            "--forests-list",
            forests_list,
            "--tree-file",
            tree_files[0],
            "--tree-file",
            tree_files[1],
            "--ingest-max-rows",
            "4",
        ]
        dataset = self.check_route(
            "consistent_trees_ascii",
            profile,
            [str(a) for a in ingest],
            [
                "--forests-list",
                forests_list,
                "--tree-file",
                tree_files[0],
                "--tree-file",
                tree_files[1],
            ],
            "RootID",
            sim_info,
        )
        code, report, text = self.extras_cli(
            dataset,
            "consistent_trees_ascii",
            profile,
            [
                "--forests-list",
                forests_list,
                "--tree-file",
                tree_files[0],
                "--tree-file",
                tree_files[1],
            ],
        )
        self.assertEqual(code, 0, text)
        self.assertEqual(report["matched_rows"], 17)
        self.assertEqual(report["checks"]["row_coverage"]["compared"], 17)
        with h5py.File(dataset / "forests.h5", "r") as handle:
            spans = handle["SourceFileOrdinal"][...]
            self.assertEqual(int(spans[0]), -1)

    def test_profile_loader_accepts_no_extras_and_refuses_malformed_profiles(self):
        shipped = REPO_ROOT / "convert" / "mimic-convert" / "profiles" / "lhalo_binary.yaml"
        self.assertEqual(
            acc.load_profile_declarations(shipped, "lhalo_binary"), ([], ["MostBoundID"])
        )
        ascii_profile = SIMULATIONS / "micro-uchuu-ascii" / "converter_columns.yaml"
        self.assertEqual(
            acc.load_profile_declarations(ascii_profile, "consistent_trees_ascii"), ([], ["id"])
        )
        base = yaml.safe_load(shipped.read_text())
        malformed = {
            "not a mapping": "- extra_fields\n",
            "invalid YAML": "extra_fields: [\n",
            "no extra_fields": yaml.safe_dump(
                {k: v for k, v in base.items() if k != "extra_fields"}
            ),
            "extra_fields not a list": yaml.safe_dump(dict(base, extra_fields={})),
            "extra not a mapping": yaml.safe_dump(dict(base, extra_fields=["Len"])),
            "source without field": yaml.safe_dump(
                dict(base, extra_fields=[extra("X", [{"name": "Len"}], "int")])
            ),
            "bad component": yaml.safe_dump(
                dict(base, extra_fields=[extra("X", [{"field": "Pos", "component": 5}], "float")])
            ),
            "no identity role": yaml.safe_dump(dict(base, required_columns={"Len": ["Len"]})),
            "repeated extra name": yaml.safe_dump(
                dict(
                    base,
                    extra_fields=[
                        extra("X", [{"field": "Len"}], "int"),
                        extra("X", [{"field": "SnapNum"}], "int"),
                    ],
                )
            ),
            "extra named SourceHaloID": yaml.safe_dump(
                dict(base, extra_fields=[extra("SourceHaloID", [{"field": "Len"}], "int")])
            ),
            "extra named like the identity field": yaml.safe_dump(
                dict(base, extra_fields=[extra(acc.IDENTITY_FIELD, [{"field": "Len"}], "int")])
            ),
        }
        for label, text in malformed.items():
            with self.subTest(label):
                path = self.tmp / "malformed.yaml"
                path.write_text(text)
                with self.assertRaises(acc.AcceptanceError):
                    acc.load_profile_declarations(path, "lhalo_binary")

    # -- SourceHaloID identity without any declared extra ----------------------

    def check_identity_only(self, dataset, source_format, profile, inventory):
        """PASS on the clean dataset; a wrong id and a swapped pair each fail."""
        code, report, text = self.extras_cli(dataset, source_format, profile, inventory)
        self.assertEqual(code, 0, text + json.dumps(report and report["checks"], indent=1))
        self.assertEqual(report["extras"], [])
        self.assertGreater(report["checks"]["source_identity"]["compared"], 0)
        self.assertGreater(report["checks"]["row_coverage"]["compared"], 0)
        paths = [p for p in sorted(Path(dataset).glob("snapshot_*.h5")) if rows_in(p) > 1]
        self.assertTrue(paths, "need a snapshot with two halos to swap")

        wrong = self.tmp / "wrong_id"
        shutil.copytree(dataset, wrong)
        edit_ids(wrong / paths[0].name, lambda ids: ids.__setitem__(0, 10**12))
        code, report, text = self.extras_cli(wrong, source_format, profile, inventory)
        self.assertEqual(code, acc.EXIT_FAIL, text)
        self.assertEqual(report["failed_checks"], ["row_coverage"])
        self.assertEqual(report["checks"]["row_coverage"]["failures"], 2)

        swapped = self.tmp / "swapped_id"
        shutil.copytree(dataset, swapped)
        edit_ids(swapped / paths[0].name, lambda ids: ids.__setitem__(slice(0, 2), ids[1::-1]))
        code, report, text = self.extras_cli(swapped, source_format, profile, inventory)
        self.assertEqual(code, acc.EXIT_FAIL, text)
        self.assertEqual(report["failed_checks"], ["source_identity"])
        self.assertEqual(report["checks"]["source_identity"]["failures"], 2)

    def check_ascii_catalogue_key_only(self, dataset, profile, inventory):
        """The ASCII leg of :meth:`check_identity_only` under the catalogue-key
        join: PASS on the clean dataset with ``source_identity`` not
        applicable; a wrong catalogue id fails ``row_coverage`` exactly as a
        wrong SourceHaloID does on the other routes; a swapped SourceHaloID
        pair passes, because compare-extras no longer reads ASCII ids."""
        source_format = "consistent_trees_ascii"
        code, report, text = self.extras_cli(dataset, source_format, profile, inventory)
        self.assertEqual(code, 0, text + json.dumps(report and report["checks"], indent=1))
        self.assertEqual(report["extras"], [])
        self.assertIn("not_applicable", report["checks"]["source_identity"])
        self.assertEqual(report["checks"]["source_identity"]["compared"], 0)
        self.assertGreater(report["checks"]["row_coverage"]["compared"], 0)
        paths = [p for p in sorted(Path(dataset).glob("snapshot_*.h5")) if rows_in(p) > 1]
        self.assertTrue(paths, "need a snapshot with two halos to swap")

        wrong = self.tmp / "wrong_catalogue_id"
        shutil.copytree(dataset, wrong)
        with h5py.File(wrong / paths[0].name, "r+") as handle:
            catalogue = handle["halos"]["MostBoundID"][...]
            catalogue[0] = 10**12
            handle["halos"]["MostBoundID"][...] = catalogue
        code, report, text = self.extras_cli(wrong, source_format, profile, inventory)
        self.assertEqual(code, acc.EXIT_FAIL, text)
        self.assertEqual(report["failed_checks"], ["row_coverage"])
        self.assertEqual(report["checks"]["row_coverage"]["failures"], 2)

        # ASCII SourceHaloID binding is not this comparator's to prove: the
        # adapter's literal ids (test_ascii_adapter TestCanonicalBridge) and
        # the C-dump compare leg's source_halo_id finding prove it
        swapped = self.tmp / "swapped_id"
        shutil.copytree(dataset, swapped)
        edit_ids(swapped / paths[0].name, lambda ids: ids.__setitem__(slice(0, 2), ids[1::-1]))
        code, report, text = self.extras_cli(swapped, source_format, profile, inventory)
        self.assertEqual(code, 0, text)
        self.assertEqual(report["failed_checks"], [])

    def test_ascii_identity_with_the_shipped_zero_extras_profile(self):
        package = SIMULATIONS / "micro-uchuu-ascii"
        data = package / "_tests" / "data"
        profile = package / "converter_columns.yaml"
        ingest = [
            "--source-format",
            "consistent_trees_ascii",
            "--simulation-info",
            package / "simulation_info.yaml",
            "--a-list",
            package / "micro-uchuu.a_list",
            "--column-map",
            profile,
            "--forests-list",
            data / "forests.list",
            "--tree-file",
            data / "tree_0_0_0.dat",
            "--ingest-max-rows",
            "4096",
        ]
        dataset = convert_with_harness(
            self.tmp, [str(a) for a in ingest], self.record, package / "simulation_info.yaml"
        )
        self.check_ascii_catalogue_key_only(
            dataset,
            profile,
            ["--forests-list", data / "forests.list", "--tree-file", data / "tree_0_0_0.dat"],
        )

    def test_lhalo_and_hdf5_identity_with_the_shipped_zero_extras_profiles(self):
        source = SyntheticLHalo().write_source(self.tmp / "source")
        profile = REPO_ROOT / "convert" / "mimic-convert" / "profiles" / "lhalo_binary.yaml"
        dataset = convert_with_harness(self.tmp / "lhalo", lhalo_ingest_args(source), self.record)
        self.check_identity_only(
            dataset,
            "lhalo_binary",
            profile,
            [
                "--source-dir",
                source,
                "--tree-name",
                SyntheticLHalo.TREE_NAME,
                "--first-file",
                "0",
                "--last-file",
                "1",
                "--halo-properties",
                MICRO / "halo_properties.yaml",
            ],
        )
        package = SIMULATIONS / "micro-uchuu-hdf5"
        fixture = package / "_tests" / "data" / "MicroUchuu_test_mergertree_info.h5"
        profile = package / "converter_columns.yaml"
        ingest = [
            "--source-format",
            "consistent_trees_hdf5",
            "--simulation-info",
            package / "simulation_info.yaml",
            "--a-list",
            package / "micro-uchuu.a_list",
            "--column-map",
            profile,
            "--info-file",
            fixture,
            "--first-file",
            "0",
            "--last-file",
            "0",
        ]
        dataset = convert_with_harness(
            self.tmp / "hdf5",
            [str(a) for a in ingest],
            self.record,
            package / "simulation_info.yaml",
        )
        shutil.rmtree(self.tmp / "wrong_id")
        shutil.rmtree(self.tmp / "swapped_id")
        self.check_identity_only(
            dataset,
            "consistent_trees_hdf5",
            profile,
            ["--info-file", fixture, "--first-file", "0", "--last-file", "0"],
        )

    def test_malformed_profile_is_a_recorded_usage_error(self):
        profile = self.tmp / "broken.yaml"
        profile.write_text("extra_fields: [\n")
        code, _report, text = self.extras_cli(
            self.tmp,
            "lhalo_binary",
            profile,
            [
                "--source-dir",
                self.tmp,
                "--tree-name",
                "x",
                "--first-file",
                "0",
                "--last-file",
                "0",
                "--halo-properties",
                MICRO / "halo_properties.yaml",
            ],
        )
        self.assertEqual(code, acc.EXIT_ERROR, text)
        entry = json.loads(self.record.read_text())["runs"][-1]
        self.assertEqual(entry["kind"], "error")

    def test_colliding_extra_names_are_recorded_usage_errors(self):
        # each would otherwise reach np.dtype() as a duplicate field: a bare ValueError
        shipped = REPO_ROOT / "convert" / "mimic-convert" / "profiles" / "lhalo_binary.yaml"
        colliding = {
            "repeated": [
                extra("RawLen", [{"field": "Len"}], "int"),
                extra("RawLen", [{"field": "SnapNum"}], "int"),
            ],
            "identity": [extra("SourceHaloID", [{"field": "Len"}], "int")],
        }
        for label, extras in colliding.items():
            with self.subTest(label):
                profile = write_profile(self.tmp / "{}.yaml".format(label), shipped, extras)
                code, _report, text = self.extras_cli(
                    self.tmp,
                    "lhalo_binary",
                    profile,
                    [
                        "--source-dir",
                        self.tmp,
                        "--tree-name",
                        "x",
                        "--first-file",
                        "0",
                        "--last-file",
                        "0",
                        "--halo-properties",
                        MICRO / "halo_properties.yaml",
                    ],
                )
                self.assertEqual(code, acc.EXIT_ERROR, text)
                entry = json.loads(self.record.read_text())["runs"][-1]
                self.assertEqual((entry["kind"], entry["exit_status"]), ("error", acc.EXIT_ERROR))
                self.assertTrue(entry["error"].startswith("AcceptanceError"), entry["error"])

    def test_forests_hdf5_inputs_include_the_external_link_targets(self):
        package = SIMULATIONS / "micro-uchuu-hdf5"
        fixture = package / "_tests" / "data" / "MicroUchuu_test_mergertree_info.h5"
        forests = self.tmp / "forests_0.h5"
        with h5py.File(fixture, "r") as source, h5py.File(forests, "w") as target:
            source.copy(source["File0"], target, name="File0")
        linked = self.tmp / "linked_info.h5"
        with h5py.File(linked, "w") as handle:
            handle["File0"] = h5py.ExternalLink(forests.name, "/File0")  # relative, as shipped
        self.assertEqual(
            [path.resolve() for path in acc.hdf5_link_targets(linked, 0, 0)], [forests.resolve()]
        )
        self.assertEqual(acc.hdf5_link_targets(fixture, 0, 0), [], "a hard link has no target")
        # the extraction reads the same rows through the link as from the fixture itself
        _declared, aliases = acc.load_profile_declarations(
            package / "converter_columns.yaml", "consistent_trees_hdf5"
        )

        def extracted(info):
            blocks = acc.iter_hdf5_source(info, 0, 0, ["Mvir"], aliases, 4, 1 << 20)
            return [
                (first, rows, {k: v.tolist() for k, v in c.items()}) for first, rows, c in blocks
            ]

        self.assertEqual(extracted(linked), extracted(fixture))
        # the recorded inputs name the file the halos were read from, not just the info file
        empty = self.tmp / "empty_dataset"
        empty.mkdir()
        code, report, text = self.extras_cli(
            empty,
            "consistent_trees_hdf5",
            package / "converter_columns.yaml",
            ["--info-file", linked, "--first-file", "0", "--last-file", "0"],
        )
        self.assertEqual(code, acc.EXIT_FAIL, text)
        self.assertEqual(report["failed_checks"], ["dataset_integrity"])
        entry = json.loads(self.record.read_text())["runs"][-1]
        recorded = [identity["path"] for identity in entry["inputs"]]
        self.assertIn(str(linked.resolve()), recorded)
        self.assertIn(str(forests.resolve()), recorded)
        self.assertEqual(entry["result"]["report"], str(self.tmp / "extras.json"))
        self.assertIn("resources", entry)

    def test_opposite_endian_extraction_reads_the_declared_values(self):
        synthetic = SyntheticLHalo([[[halo(5, mbid=-(1 << 40), m_crit=-1.0)]]])
        little = synthetic.write_source(self.tmp / "le")
        raw = (little / "synthetic_trees.0").read_bytes()
        # byte-swap every 4/8-byte field of the header and the record
        header = struct.unpack("<iii", raw[:12])
        record = RECORD.unpack(raw[12:])
        big = struct.pack(">iii", *header) + struct.Struct(">" + RECORD.format[1:]).pack(*record)
        (self.tmp / "be").mkdir()
        (self.tmp / "be" / "synthetic_trees.0").write_bytes(big)
        layout = acc.lhalo_layout(MICRO / "halo_properties.yaml", "big")
        self.assertEqual(layout.itemsize, 104)
        ((first, records),) = list(
            acc.iter_lhalo_source(self.tmp / "be", "synthetic_trees", 0, 0, layout, "big", 10)
        )
        self.assertEqual(first, 1)
        self.assertEqual(int(records["MostBoundID"][0]), -(1 << 40))
        self.assertEqual(float(records["M_Crit200"][0]), -1.0)
        self.assertEqual(int(records["SnapNum"][0]), 5)

    def test_truncated_binary_source_is_refused(self):
        source = SyntheticLHalo().write_source(self.tmp / "source")
        path = source / "synthetic_trees.1"
        path.write_bytes(path.read_bytes()[:-1])
        layout = acc.lhalo_layout(MICRO / "halo_properties.yaml", "little")
        with self.assertRaises(acc.AcceptanceError):
            list(acc.iter_lhalo_source(source, "synthetic_trees", 0, 1, layout, "little", 10))
        with self.assertRaises(acc.AcceptanceError):
            list(acc.iter_lhalo_source(source, "synthetic_trees", 0, 2, layout, "little", 10))


# ==========================================================================
# The ASCII extractor's memory bound
# ==========================================================================

ASCII_HEADER = "#scale(0) id(1) desc_id(2) x(3) Mvir(4) num_prog(5) Tree_root_ID(6) Snap_num(7)\n"


def write_ascii(directory, trees, forest_of_tree):
    """One indexed-header ASCII tree file of ``{tree root: rows}`` and its forests.list.

    Row ``i`` of the file (from 0) has catalog id ``i + 1``, x ``i / 1000`` and
    snapshot ``i % 4``.
    """
    directory.mkdir(parents=True, exist_ok=True)
    lines, row = [ASCII_HEADER, "#Omega_M = 0.3\n", "{}\n".format(len(trees))], 0
    for tree, rows in trees.items():
        lines.append("#tree {}\n".format(tree))
        for _ in range(rows):
            lines.append(
                "0.5 {} -1 {:.3f} 1.0e12 0 {} {}\n".format(row + 1, row / 1000, tree, row % 4)
            )
            row += 1
    path = directory / "tree_0_0_0.dat"
    path.write_text("".join(lines))
    listing = directory / "forests.list"
    listing.write_text(
        "#TreeRootID ForestID\n"
        + "".join("{} {}\n".format(t, f) for t, f in forest_of_tree.items())
    )
    return path, listing


class AsciiExtractionTests(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="acceptance_ascii_"))
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)

    def peak(self, path, listing, block_rows):
        """tracemalloc peak while streaming ``x`` of every row; the blocks' sizes and catalog ids."""
        sizes, ids = [], []
        tracemalloc.start()
        try:
            for _snaps, columns in acc.iter_ascii_source(
                [path], listing, ["x"], ["id"], block_rows
            ):
                catalog = columns[acc.IDENTITY_FIELD]
                sizes.append(len(catalog))
                ids.append((int(catalog[0]), int(catalog[-1]), columns["x"][0]))
            _current, peak = tracemalloc.get_traced_memory()
        finally:
            tracemalloc.stop()
        return peak, sizes, ids

    def test_ascii_extraction_memory_follows_block_rows_not_the_tree(self):
        """A 20,000-row tree read 1,000 rows at a time never holds the whole tree's tokens.

        The earlier extractor kept every token of a tree twice (a list of rows
        and a string array), about 21 MB here whatever the block size.
        """
        path, listing = write_ascii(self.tmp, {7: 20000}, {7: 1})
        peak, sizes, ids = self.peak(path, listing, 1000)
        self.assertEqual(sizes, [1000] * 20)
        self.assertEqual(ids[0], (1, 1000, "0.000"))
        self.assertEqual(ids[-1], (19001, 20000, "19.000"))
        whole_peak, whole_sizes, _ids = self.peak(path, listing, 20000)
        self.assertEqual(whole_sizes, [20000])
        # the whole tree's tokens of just the two kept columns, as Python strings
        whole_tokens = 20000 * 2 * sys.getsizeof("19999")
        self.assertLess(peak, whole_tokens / 2, (peak, whole_tokens))
        self.assertLess(4 * peak, whole_peak, (peak, whole_peak))

    def test_ascii_slices_yield_the_same_catalogue_keys_at_any_block_size(self):
        """The extractor assigns no SourceHaloID: every row comes out keyed by
        (SnapNum, catalogue id) in file order, identically at any block size."""
        # forests 1 and 2 interleave across trees; tree 31 is empty
        trees = {11: 3, 21: 5, 12: 4, 31: 0, 22: 2, 13: 1, 32: 3}
        forests = {11: 1, 12: 1, 13: 1, 21: 2, 22: 2, 31: 3, 32: 3}
        path, listing = write_ascii(self.tmp, trees, forests)

        def keys(block_rows):
            blocks = acc.iter_ascii_source([path], listing, ["x"], ["id"], block_rows)
            return [
                (int(snap), int(catalog), x)
                for snaps, columns in blocks
                for snap, catalog, x in zip(snaps, columns[acc.IDENTITY_FIELD], columns["x"])
            ]

        whole = keys(1 << 20)
        self.assertEqual(
            whole, [(row % 4, row + 1, "{:.3f}".format(row / 1000)) for row in range(18)]
        )
        for block_rows in (1, 2, 3, 7):
            with self.subTest(block_rows=block_rows):
                self.assertEqual(keys(block_rows), whole)
        with self.assertRaises(acc.AcceptanceError):
            keys(0)

    def test_a_file_changed_between_the_passes_is_refused(self):
        path, listing = write_ascii(self.tmp, {7: 5, 8: 2}, {7: 1, 8: 2})
        plan = acc._ascii_tree_plan
        for label, edit in (
            ("fewer rows planned", lambda trees: trees[0].__setitem__(2, 4)),
            ("more rows planned", lambda trees: trees[1].__setitem__(2, 3)),
            ("a tree fewer planned", lambda trees: trees.pop()),
            ("tree ids swapped", lambda trees: trees[0].__setitem__(3, trees[1][3])),
        ):

            def changed(*args, edit=edit):
                trees, sizes = plan(*args)
                edit(trees)
                return trees, sizes

            with self.subTest(label), mock.patch.object(acc, "_ascii_tree_plan", changed):
                with self.assertRaisesRegex(acc.AcceptanceError, "changed between"):
                    list(acc.iter_ascii_source([path], listing, ["x"], ["id"], 2))


# ==========================================================================
# The C reference dump itself, built and run
# ==========================================================================

#: The sources the dump build reads, copied so the build's code generation
#: rewrites the copy's generated headers and never this checkout's.
BUILD_TREE = (
    "Makefile",
    "src",
    "scripts",
    "models/halos-only",
    "simulations/micro-uchuu",
    "simulations/micro-uchuu-ascii",
    "tests/unit/tools",
)

#: ``mimic-topology-dump v1`` of the committed micro-uchuu-ascii fixture, as
#: written by the tool's default mode (SHA-256 55bd72a1...), so the default
#: mode cannot drift. The two trailing backslashes are line continuations, not
#: characters of the dump.
ASCII_FIXTURE_V1_DUMP = """# mimic-topology-dump v1
# forestnr rank id snapnum desc_id first_prog_id next_prog_id first_fof_id next_fof_id
# NA sentinel = -9223372036854775808 (no link)
0 0 1000001 49 -9223372036854775808 1000011 -9223372036854775808 1000001 -9223372036854775808
0 1 1000011 48 1000001 -9223372036854775808 -9223372036854775808 1000011 -9223372036854775808
1 0 1000002 49 -9223372036854775808 -9223372036854775808 -9223372036854775808 1000002 \
-9223372036854775808
2 0 1000003 49 -9223372036854775808 -9223372036854775808 -9223372036854775808 1000003 \
-9223372036854775808
"""
V1_HEADER = "\n".join(ASCII_FIXTURE_V1_DUMP.splitlines()[:3]) + "\n"

#: ``mimic-source-dump v1`` of the same fixture through the enumerated
#: consistent_trees_ascii reader, typed from tree_0_0_0.dat and forests.list:
#: forests 1001..1003 are forest numbers 0..2, all in partition 0 as units
#: 0..2; 1000011 (snap 48) is 1000001's first progenitor, and every halo heads
#: its own FoF group. len is round(float32(Mvir) * 1e-10 / 0.0327), the
#: package particle mass (153, 138, 92, 61); m_crit200 is float32(Mvir)
#: (5e10 -> 513a43b7, 4.5e10 -> 5127a358, 3e10 -> 50df8476, 2e10 -> 509502f9);
#: pos/vel are x..z and vx..vz (5.1 -> 40a33333); spin is J/Mvir = 0 (J is
#: zero); vel_disp is vrms and vmax is vmax. Backslashes continue lines.
ASCII_FIXTURE_SOURCE_DUMP = """# mimic-source-dump v1
# reader consistent_trees_ascii partition_model enumerated
# columns forest_index rank partition unit snapnum descendant descendant_snap \
first_progenitor first_progenitor_snap next_progenitor next_progenitor_snap first_fof next_fof \
len most_bound_id m_crit200 pos_x pos_y pos_z vel_x vel_y vel_z spin_x spin_y spin_z vel_disp vmax
# links are within-forest ranks, -1 = no link; *_snap is the target's snapnum, -1 = no link; \
m_crit200..vmax are binary32 bit patterns in hex
0 0 0 0 49 -1 -1 1 48 -1 -1 0 -1 153 1000001 \
513a43b7 40a00000 40c00000 40e00000 41200000 41a00000 41f00000 00000000 00000000 00000000 \
42a00000 42f00000
0 1 0 0 48 0 49 -1 -1 -1 -1 1 -1 138 1000011 \
5127a358 40a33333 40c33333 40e33333 41300000 41a80000 41f80000 00000000 00000000 00000000 \
42960000 42e60000
1 0 0 1 49 -1 -1 -1 -1 -1 -1 0 -1 92 1000002 \
50df8476 41700000 41800000 41880000 41400000 41b00000 42000000 00000000 00000000 00000000 \
42700000 42c80000
2 0 0 2 49 -1 -1 -1 -1 -1 -1 0 -1 61 1000003 \
509502f9 41c80000 41d00000 41d80000 41500000 41b80000 42040000 00000000 00000000 00000000 \
42480000 42b40000
# end rows 4 forests 3
"""


def _toolchain_missing():
    """Why the C dump cannot be built here, or None when it can."""
    compiler = os.environ.get("CC", "gcc")
    if shutil.which(compiler) is None:
        return "C compiler {!r} not found".format(compiler)
    if (
        shutil.which("pkg-config")
        and subprocess.run(["pkg-config", "--exists", "yaml-0.1"], capture_output=True).returncode
    ):
        return "libyaml (pkg-config yaml-0.1) not found"
    return None


TOOLCHAIN_MISSING = _toolchain_missing()


def run_file(path, simulation, simulation_dir, tree_name, last_file, output_dir):
    path.write_text(
        yaml.safe_dump(
            {
                "model": {"name": "halos-only"},
                "simulation": {"name": simulation},
                "input": {
                    "tree_name": tree_name,
                    "simulation_dir": str(simulation_dir),
                    "first_file": 0,
                    "last_file": last_file,
                },
                "output": {
                    "output_filename": "halos",
                    "output_directory": str(output_dir),
                    "output_format": "binary",
                    "snapshot_list": [49],
                },
                "SubSteps": 1,
                "modules": {"parameters": {}},
            },
            sort_keys=False,
        )
    )
    return path


@unittest.skipIf(TOOLCHAIN_MISSING, TOOLCHAIN_MISSING or "")
class CDumpToolTests(unittest.TestCase):
    """Build ``dump_ctrees_topology`` with the committed build script and check
    its ``--source-payload`` output byte for byte against the literal oracle.

    The build script regenerates property and module-registry code for its
    MODEL/SIMULATION pair, so it runs from a temporary copy of the sources it
    reads (:data:`BUILD_TREE`); this checkout's generated state is untouched.
    A build failure with a toolchain present fails the test; only an absent
    toolchain skips it.
    """

    @classmethod
    def setUpClass(cls):
        cls.tmp = Path(tempfile.mkdtemp(prefix="acceptance_cdump_"))
        cls.copy = cls.tmp / "repo"
        ignore = shutil.ignore_patterns("__pycache__", "*.o", "snapshots", "generated", "build")
        for rel in BUILD_TREE:
            source, target = REPO_ROOT / rel, cls.copy / rel
            if source.is_dir():
                shutil.copytree(source, target, ignore=ignore, symlinks=True)
            else:
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(source, target)
        cls.tools = {}
        for simulation in ("micro-uchuu", "micro-uchuu-ascii"):
            build_dir = cls.tmp / ("tool-" + simulation)
            env = dict(
                os.environ,
                MODEL="halos-only",
                SIMULATION=simulation,
                TOPOLOGY_DUMP_BUILD_DIR=str(build_dir),
            )
            if (Path(sys.prefix) / "bin" / "python3").exists():
                env["VIRTUAL_ENV"] = sys.prefix  # the copy has no mimic_venv of its own
            entry = acc.measured_run(
                ["bash", cls.copy / "tests" / "unit" / "tools" / "build_topology_dump.sh"],
                "build-" + simulation,
                cls.tmp / "logs",
                cwd=cls.copy,
                env=env,
            )
            tool = build_dir / "dump_ctrees_topology"
            if entry["exit_code"] != 0 or not tool.is_file():
                log = build_dir / "compile.log"
                detail = log.read_text()[-3000:] if log.exists() else ""
                raise AssertionError(
                    "dump build failed for {}:\n{}\n{}".format(
                        simulation, Path(entry["stdout"]).read_text()[-2000:], detail
                    )
                )
            cls.tools[simulation] = tool
        cls.source = SyntheticLHalo().write_source(cls.tmp / "source")

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def setUp(self):
        self.work = Path(tempfile.mkdtemp(prefix="acceptance_cdump_run_", dir=self.tmp))

    def lhalo_run_file(self, last_file=1):
        return run_file(
            self.work / "run.yaml",
            "micro-uchuu",
            self.source,
            SyntheticLHalo.TREE_NAME,
            last_file,
            self.work / "out",
        )

    def test_source_payload_matches_the_literal_oracle_byte_for_byte(self):
        record = self.work / "record.json"
        dump = self.work / "synthetic.dump"
        code, out, err = run_harness(
            [
                "dump",
                "--record",
                record,
                "--tool",
                self.tools["micro-uchuu"],
                "--run-file",
                self.lhalo_run_file(),
                "--out",
                dump,
            ]
        )
        self.assertEqual(code, 0, out + err)
        self.assertEqual(dump.read_text(), SyntheticLHalo().dump_text())
        (entry,) = json.loads(record.read_text())["runs"]
        self.assertEqual(entry["exit_code"], 0)
        self.assertEqual(entry["dump"]["sha256"], acc.sha256_file(dump))

    def test_default_mode_still_refuses_per_file_readers_unchanged(self):
        dump = self.work / "v1.dump"
        result = subprocess.run(
            [str(self.tools["micro-uchuu"]), str(self.lhalo_run_file()), str(dump)],
            cwd=str(REPO_ROOT),
            capture_output=True,
            text=True,
        )
        self.assertEqual(result.returncode, 1)
        self.assertIn("global_forest_offset", result.stderr)
        self.assertEqual(dump.read_text(), V1_HEADER)

    def test_source_payload_of_the_enumerated_ascii_reader_matches_its_literal(self):
        # the only --source-payload run through global_forest_offset (enumerated readers)
        record = self.work / "record.json"
        dump = self.work / "ascii.dump"
        data = SIMULATIONS / "micro-uchuu-ascii" / "_tests" / "data"
        run = run_file(
            self.work / "ascii.yaml", "micro-uchuu-ascii", data, "tree_0_0_0.dat", 0, self.work
        )
        code, out, err = run_harness(
            [
                "dump",
                "--record",
                record,
                "--tool",
                self.tools["micro-uchuu-ascii"],
                "--run-file",
                run,
                "--out",
                dump,
            ]
        )
        self.assertEqual(code, 0, out + err)
        self.assertEqual(dump.read_text(), ASCII_FIXTURE_SOURCE_DUMP)
        parsed = acc.SourceDump(dump)
        self.assertEqual(
            (parsed.reader, parsed.partition_model), ("consistent_trees_ascii", "enumerated")
        )

    def test_default_mode_output_is_byte_identical_to_the_pre_change_tool(self):
        dump = self.work / "v1.dump"
        data = SIMULATIONS / "micro-uchuu-ascii" / "_tests" / "data"
        run = run_file(
            self.work / "ascii.yaml",
            "micro-uchuu-ascii",
            data,
            "tree_0_0_0.dat",
            0,
            self.work / "out",
        )
        result = subprocess.run(
            [str(self.tools["micro-uchuu-ascii"]), str(run), str(dump)],
            cwd=str(REPO_ROOT),
            capture_output=True,
            text=True,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(dump.read_text(), ASCII_FIXTURE_V1_DUMP)

    def test_a_missing_requested_file_is_fatal(self):
        dump = self.work / "missing.dump"
        result = subprocess.run(
            [
                str(self.tools["micro-uchuu"]),
                "--source-payload",
                str(self.lhalo_run_file(last_file=2)),
                str(dump),
            ],
            cwd=str(REPO_ROOT),
            capture_output=True,
            text=True,
        )
        self.assertEqual(result.returncode, 1)
        self.assertIn("Requested input file 2 is missing", result.stderr)
        # files 0 and 1 were already dumped when file 2 was found missing
        self.assertFalse(dump.exists(), "a failed source dump must leave no partial file")


# ==========================================================================
# The measured-run path
# ==========================================================================


class MeasurementTests(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="acceptance_measure_"))
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)

    def test_exec_records_exit_code_peak_rss_and_identities(self):
        record = self.tmp / "record.json"
        source = self.tmp / "input.bin"
        source.write_bytes(b"source bytes")
        code, _out, _err = run_harness(
            [
                "exec",
                "--record",
                record,
                "--label",
                "allocate",
                "--source",
                source,
                "--hash-sources",
                "--",
                sys.executable,
                "-c",
                "import sys; b = bytearray(64 << 20); "
                "b[::4096] = b'x' * len(b[::4096]); sys.exit(3)",
            ]
        )
        self.assertEqual(code, acc.EXIT_FAIL)
        (entry,) = json.loads(record.read_text())["runs"]
        self.assertEqual(entry["exit_code"], 3)
        self.assertEqual(entry["exit_status"], acc.EXIT_FAIL)
        self.assertGreater(entry["peak_rss_bytes"], 64 << 20)
        self.assertGreater(entry["wall_seconds"], 0)
        self.assertIn("user_seconds", entry)
        self.assertEqual(entry["command"][0], sys.executable)
        self.assertEqual(len(entry["executable"]["sha256"]), 64)
        (identity,) = entry["recorded_sources"]
        self.assertEqual(identity["bytes"], len(b"source bytes"))
        self.assertEqual(identity["sha256"], acc.sha256_file(source))
        self.assertIn("git_commit", entry["code"])

    def test_record_accumulates_runs_and_rejects_foreign_files(self):
        record = self.tmp / "record.json"
        for label in ("one", "two"):
            run_harness(
                ["exec", "--record", record, "--label", label, "--", sys.executable, "-c", ""]
            )
        self.assertEqual(
            [r["label"] for r in json.loads(record.read_text())["runs"]], ["one", "two"]
        )
        foreign = self.tmp / "foreign.json"
        foreign.write_text('{"runs": []}')
        code, _out, err = run_harness(
            ["exec", "--record", foreign, "--label", "x", "--", sys.executable, "-c", ""]
        )
        self.assertEqual(code, acc.EXIT_ERROR)
        self.assertIn("not a", err)

    def test_convert_stops_at_the_first_failing_stage(self):
        record = self.tmp / "record.json"
        code, _out, _err = run_harness(
            [
                "convert",
                "--record",
                record,
                "--workdir",
                self.tmp / "work",
                "--simulation-info",
                MICRO / "simulation_info.yaml",
                "--",
                "--source-format",
                "lhalo_binary",
            ]
        )
        self.assertEqual(code, acc.EXIT_FAIL)
        (entry,) = json.loads(record.read_text())["runs"]
        self.assertEqual([s["label"] for s in entry["stages"]], ["convert-ingest"])
        self.assertNotEqual(entry["stages"][0]["exit_code"], 0)
        code, _out, _err = run_harness(
            [
                "convert",
                "--record",
                record,
                "--workdir",
                self.tmp,
                "--simulation-info",
                MICRO / "simulation_info.yaml",
                "--",
            ]
        )
        self.assertEqual(code, acc.EXIT_ERROR, "an existing workdir is refused")

    def test_same_label_runs_within_one_second_keep_separate_logs(self):
        entries = []
        with mock.patch.object(acc.time, "strftime", return_value="20260101T000000"):
            for text in ("first", "second"):
                entries.append(
                    acc.measured_run(
                        [sys.executable, "-c", "print({!r})".format(text)], "retry", self.tmp
                    )
                )
        self.assertNotEqual(entries[0]["stdout"], entries[1]["stdout"])
        self.assertNotEqual(entries[0]["stderr"], entries[1]["stderr"])
        for entry, text in zip(entries, ("first", "second")):
            self.assertEqual(entry["exit_code"], 0)
            self.assertEqual(Path(entry["stdout"]).read_text().strip(), text)
            self.assertTrue(Path(entry["stderr"]).is_file())
        # the stem collision leaves no stray log behind: exactly two pairs exist
        self.assertEqual(len(list(Path(self.tmp).glob("*.out"))), 2)
        self.assertEqual(len(list(Path(self.tmp).glob("*.err"))), 2)

    def test_an_interrupted_wait_terminates_the_child(self):
        waited = []

        def interrupted(pid, _options):
            waited.append(pid)
            raise KeyboardInterrupt

        with mock.patch.object(acc.os, "wait4", side_effect=interrupted):
            with self.assertRaises(KeyboardInterrupt):
                acc.measured_run(
                    [sys.executable, "-c", "import time; time.sleep(60)"], "sleep", self.tmp
                )
        (pid,) = waited
        # terminated and reaped: the pid no longer names a process of ours
        with self.assertRaises(ProcessLookupError):
            os.kill(pid, 0)

    def test_the_harness_imports_nothing_from_the_converter(self):
        converter_modules = {
            path.stem for path in (REPO_ROOT / "convert" / "mimic-convert").glob("*.py")
        } | {"adapters"}
        source = Path(acc.__file__).read_text()
        imported = {
            line.split()[1].split(".")[0]
            for line in source.splitlines()
            if line.startswith(("import ", "from "))
        }
        self.assertFalse(imported & converter_modules, imported & converter_modules)
        result = subprocess.run(
            [
                sys.executable,
                "-c",
                "import sys; sys.path.insert(0, {!r}); import run_generalisation_acceptance; "
                "print(sorted(m for m in sys.modules if m in {!r}))".format(
                    str(HERE), sorted(converter_modules)
                ),
            ],
            capture_output=True,
            text=True,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.strip(), "[]")


if __name__ == "__main__":
    unittest.main()
