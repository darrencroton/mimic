"""Self-tests of the generalisation acceptance harness (Slice 10).

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
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

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

    def test_non_binary32_payload_fails_its_field(self):
        dataset = self.copy_dataset()
        for path in sorted(dataset.glob("snapshot_*.h5")):
            with h5py.File(path, "r+") as handle:
                data = handle["halos"]["Vmax"][...].astype("<f8")
                del handle["halos"]["Vmax"]
                handle["halos"].create_dataset("Vmax", data=data)
        self.assertFailsExactly(compare(dataset, self.dump), "payload_Vmax")

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

    def test_lhalo_binary_extras_including_signed_duplicate_ids_and_sentinels(self):
        source = SyntheticLHalo().write_source(self.tmp / "source")
        profile = write_profile(
            self.tmp / "profile.yaml",
            REPO_ROOT / "scripts" / "convert" / "profiles" / "lhalo_binary.yaml",
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
        self.check_route(
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

    def test_a_profile_without_extras_is_refused(self):
        with self.assertRaises(acc.AcceptanceError):
            acc.load_extra_declarations(
                REPO_ROOT / "scripts" / "convert" / "profiles" / "lhalo_binary.yaml"
            )

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
                "import sys; b = bytearray(64 << 20); b[::4096] = b'x' * len(b[::4096]); sys.exit(3)",
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

    def test_the_harness_imports_nothing_from_the_converter(self):
        converter_modules = {
            path.stem for path in (REPO_ROOT / "scripts" / "convert").glob("*.py")
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
