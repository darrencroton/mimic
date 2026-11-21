"""Tests for lhalo_len: checking and setting the ``Len`` of L-Halo files.

Every expected ``Len`` is a literal derived by hand, never computed by the module under test
(the oracle test below is the one comparison against ``ctrees_to_lhalo``'s own conversion).
All files are tiny synthetic L-Halo files in a temporary directory; the command line is run
in a subprocess so exit statuses and messages are those a user sees.

The control file has two trees (3 and 2 records). Its ``M_Crit200`` masses and ``Len`` at the
two particle masses (1/0.0327 = 30.581..., 1/0.0325 = 30.769...):

====================  =====================  ==========  ==========
mass (float32)        float32 product 0.0327 Len 0.0327  Len 0.0325
====================  =====================  ==========  ==========
0.08174999803304672   2.5 (see below)        3           3
0.0                   0                      0           0
0.0163                0.4985                 0           1 (0.5015)
160000.0              4892966.5              4892967     4923077
12.3456               377.54                 378         380 (379.86)
====================  =====================  ==========  ==========

The first mass is the half-integer case: in float64 its product at 0.0327 is
2.4999999398485238, strictly below 2.5, and narrowing to float32 gives exactly 2.5 = k + 0.5
with k = 2 (even). Round half away from zero gives 3; rounding the float64 product, or
round-half-even of the float32 product, would give 2. At 160000.0 the float64 product,
4892966.3608562695, is above 2**22 where float32 spacing is 0.5: narrowing gives 4892966.5,
which rounds away from zero to 4892967, where the float64 product would give 4892966.
"""

import hashlib
import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

import numpy as np

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))

import ctrees_to_lhalo as c2l  # noqa: E402
import lhalo_len  # noqa: E402

TOOL = HERE.parent / "lhalo_len.py"

HALF_MASS = 0.08174999803304672  # float32 product at 0.0327 is exactly 2.5 (see above)
CONTROL_MASSES = [HALF_MASS, 0.0, 0.0163, 160000.0, 12.3456]
CONTROL_COUNTS = [3, 2]
LEN_AT_0325 = [3, 0, 1, 4923077, 380]
LEN_AT_0327 = [3, 0, 0, 4892967, 378]
MEAN200_MASSES = [1.0, 2.0, 3.0, 4.0, 5.0]
MEAN200_LEN_AT_0327 = [31, 61, 92, 122, 153]  # 30.58, 61.16, 91.74, 122.32, 152.91

SIMULATION_INFO_0327 = "simulation:\n  particle_mass:\n    value: 0.0327\n    units: 1e10 Msun/h\n"


def write_lhalo(path, counts, masses, lengths, mean200=None, tophat=None, seed=1):
    """Write an L-Halo file; every byte outside the four fields set here is random."""
    n = int(sum(counts))
    raw = np.random.default_rng(seed).integers(0, 256, size=(n, 104), dtype=np.uint8)
    records = raw.view(lhalo_len.RECORD_DTYPE).reshape(n)
    records["M_Crit200"] = np.asarray(masses, dtype=np.float32)
    records["Len"] = np.asarray(lengths, dtype=np.int32)
    records["M_Mean200"] = np.float32(7.0) if mean200 is None else np.asarray(mean200, np.float32)
    records["M_TopHat"] = np.float32(9.0) if tophat is None else np.asarray(tophat, np.float32)
    with open(path, "wb") as handle:
        handle.write(np.array([len(counts), n], dtype="<i4").tobytes())
        handle.write(np.asarray(counts, dtype="<i4").tobytes())
        handle.write(raw.tobytes())
    return Path(path)


def masked(path):
    """A file's bytes with every record's ``Len`` zeroed."""
    data = bytearray(Path(path).read_bytes())
    n_trees, n_halos = np.frombuffer(bytes(data[:8]), dtype="<i4")
    offset = 8 + 4 * int(n_trees)
    for i in range(int(n_halos)):
        start = offset + 104 * i + 20
        data[start : start + 4] = bytes(4)
    return bytes(data)


def len_column(path):
    data = Path(path).read_bytes()
    n_trees, n_halos = np.frombuffer(data[:8], dtype="<i4")
    offset = 8 + 4 * int(n_trees)
    records = np.frombuffer(data, dtype=lhalo_len.RECORD_DTYPE, offset=offset, count=int(n_halos))
    return [int(v) for v in records["Len"]]


def sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


class LhaloLenTestCase(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.tmp = Path(self._tmp.name)
        self.src = self.tmp / "src"
        self.src.mkdir()
        self.out = self.tmp / "out"

    def control(self, name="control.0", directory=None, **kwargs):
        return write_lhalo(
            (directory or self.src) / name,
            CONTROL_COUNTS,
            CONTROL_MASSES,
            LEN_AT_0325,
            **kwargs,
        )

    def run_tool(self, *args):
        done = subprocess.run(
            [sys.executable, str(TOOL), *[str(a) for a in args]],
            capture_output=True,
            text=True,
            cwd=self.tmp,
        )
        return done.returncode, done.stdout, done.stderr

    def rewrite(self, paths, mass="0.0327", *extra, out=None, expect=None):
        paths = list(paths)
        return self.run_tool(
            "rewrite",
            "--particle-mass",
            mass,
            *extra,
            "--out",
            out or self.out,
            "--expect-files",
            len(paths) if expect is None else expect,
            *paths,
        )

    def check(self, paths, mass="0.0325", *extra):
        paths = list(paths)
        return self.run_tool(
            "check", "--particle-mass", mass, *extra, "--expect-files", len(paths), *paths
        )

    def sidecar(self, name="control.0", directory=None):
        path = (directory or self.out) / f"{name}.lhalo-len.json"
        return json.loads(path.read_text())

    def assertNothingWritten(self, directory=None):
        directory = directory or self.out
        self.assertFalse(directory.exists(), f"{directory} was created")


class ComputeLenTests(LhaloLenTestCase):
    def test_control_values_at_both_particle_masses(self):
        masses = np.array(CONTROL_MASSES, dtype=np.float32)
        self.assertEqual(lhalo_len.compute_len(masses, 0.0327).tolist(), LEN_AT_0327)
        self.assertEqual(lhalo_len.compute_len(masses, 0.0325).tolist(), LEN_AT_0325)

    def test_half_integer_rounds_away_from_zero(self):
        mass = np.array([HALF_MASS], dtype=np.float32)
        product64 = float(mass[0]) * (1.0 / 0.0327)
        self.assertLess(product64, 2.5)
        self.assertEqual(float(np.float32(product64)), 2.5)
        self.assertEqual(lhalo_len.compute_len(mass, 0.0327).tolist(), [3])  # not 2

    def test_negative_mass_allowed_only_on_request(self):
        mass = np.array([-1.0], dtype=np.float32)
        with self.assertRaisesRegex(lhalo_len.LhaloLenError, "record 7.*negative"):
            lhalo_len.compute_len(mass, 1.0, source="f.0", first_index=7)
        self.assertEqual(lhalo_len.compute_len(mass, 1.0, allow_negative=True).tolist(), [-1])

    def test_non_finite_and_out_of_range_name_file_and_index(self):
        for bad, text in ((np.nan, "non-finite"), (np.inf, "non-finite"), (1e30, ".*int32 range")):
            masses = np.array([1.0, bad], dtype=np.float32)
            with self.assertRaisesRegex(lhalo_len.LhaloLenError, f"f.0: record 11: {text}"):
                lhalo_len.compute_len(masses, 0.0327, source="f.0", first_index=10)

    def test_bad_particle_mass_is_refused(self):
        masses = np.zeros(1, dtype=np.float32)
        for bad in (0.0, -1.0, math_nan(), float("inf")):
            with self.assertRaises(ValueError):
                lhalo_len.compute_len(masses, bad)


def math_nan():
    return float("nan")


class OracleTests(LhaloLenTestCase):
    def test_agrees_with_ctrees_to_lhalo_conversion(self):
        n = 100_000
        raw = np.random.default_rng(20261121).uniform(0.0, 2e15, size=n)  # Msun/h
        rows = np.zeros(n, dtype=c2l.ROW_DTYPE)
        rows["mvir"] = raw
        rows["M200b"] = raw
        rows["M200c"] = raw
        rows["id"] = np.arange(n)
        for part_mass in (0.0327, 0.0325):
            records = c2l._convert_conventions(rows, np.array([n]), part_mass)
            got = lhalo_len.compute_len(np.ascontiguousarray(records["Mvir"]), part_mass)
            self.assertEqual(int(np.count_nonzero(got != records["Len"])), 0, part_mass)
            self.assertGreater(int(records["Len"].max()), 1_000_000)


class RewriteTests(LhaloLenTestCase):
    def test_control_values_and_only_len_differs(self):
        src = self.control()
        code, out, err = self.rewrite([src])
        self.assertEqual((code, err), (0, ""), out)
        dest = self.out / src.name
        self.assertEqual(len_column(dest), LEN_AT_0327)
        self.assertEqual(masked(dest), masked(src))
        self.assertEqual(self.sidecar()["n_len_changed"], 3)
        self.assertEqual(
            sorted(p.name for p in self.out.iterdir()), [src.name, f"{src.name}.lhalo-len.json"]
        )

    def test_same_particle_mass_changes_nothing(self):
        src = self.control()
        self.assertEqual(self.rewrite([src], "0.0325")[0], 0)
        self.assertEqual((self.out / src.name).read_bytes(), src.read_bytes())
        self.assertEqual(self.sidecar()["n_len_changed"], 0)

    def test_round_trip_reproduces_the_original(self):
        src = self.control()
        self.assertEqual(self.rewrite([src], "0.0327")[0], 0)
        self.assertEqual(self.rewrite([self.out / src.name], "0.0325", out=self.tmp / "back")[0], 0)
        self.assertEqual((self.tmp / "back" / src.name).read_bytes(), src.read_bytes())

    def test_mass_field_selects_the_mass(self):
        src = self.control(mean200=MEAN200_MASSES)
        self.assertEqual(self.rewrite([src], "0.0327", "--mass-field", "M_Mean200")[0], 0)
        self.assertEqual(len_column(self.out / src.name), MEAN200_LEN_AT_0327)
        self.assertEqual(self.sidecar()["mass_field"], "M_Mean200")
        self.assertEqual(self.sidecar()["n_len_changed"], 5)  # none equals LEN_AT_0325

    def test_simulation_info_matches_particle_mass(self):
        src = self.control()
        info = self.tmp / "simulation_info.yaml"
        info.write_text(SIMULATION_INFO_0327)
        code, _, err = self.run_tool(
            "rewrite", "--simulation-info", info, "--out", self.tmp / "by_info",
            "--expect-files", 1, src,
        )  # fmt: skip
        self.assertEqual((code, err), (0, ""))
        self.assertEqual(self.rewrite([src], "0.0327")[0], 0)
        self.assertEqual(
            (self.tmp / "by_info" / src.name).read_bytes(), (self.out / src.name).read_bytes()
        )
        side = self.sidecar(directory=self.tmp / "by_info")
        self.assertEqual(side["particle_mass"], 0.0327)
        self.assertEqual(side["particle_mass_source"], f"{info}:simulation.particle_mass")
        self.assertEqual(self.sidecar()["particle_mass_source"], "0.0327")

    def test_empty_dataset(self):
        src = write_lhalo(self.src / "empty.0", [], [], [])
        self.assertEqual(self.rewrite([src])[0], 0)
        self.assertEqual((self.out / "empty.0").read_bytes(), src.read_bytes())
        self.assertEqual(self.sidecar("empty.0")["n_records"], 0)


class SidecarTests(LhaloLenTestCase):
    def test_keys_hashes_and_counts_are_independent_values(self):
        src = self.control()
        self.assertEqual(self.rewrite([src], "0.0327")[0], 0)
        side = self.sidecar()
        self.assertEqual(
            list(side),
            [
                "input", "output", "particle_mass", "particle_mass_source", "mass_field",
                "n_records", "n_len_changed", "source_particle_mass", "n_len_mismatch_source",
                "sha256_input", "sha256_output", "tool_sha256", "started_utc", "finished_utc",
            ],
        )  # fmt: skip
        self.assertEqual(set(side), set(lhalo_len.SIDECAR_KEYS))
        self.assertEqual(side["input"], str(src))
        self.assertEqual(side["output"], str(self.out / src.name))
        self.assertEqual(side["particle_mass"], 0.0327)
        self.assertEqual(side["mass_field"], "M_Crit200")
        self.assertEqual(side["n_records"], 5)
        self.assertEqual(side["n_len_changed"], 3)
        self.assertIsNone(side["source_particle_mass"])
        self.assertIsNone(side["n_len_mismatch_source"])
        self.assertEqual(side["sha256_input"], hashlib.sha256(src.read_bytes()).hexdigest())
        self.assertEqual(side["sha256_output"], sha256(self.out / src.name))
        self.assertEqual(side["tool_sha256"], sha256(TOOL))
        stamp = r"\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z"
        self.assertRegex(side["started_utc"], f"^{stamp}$")
        self.assertRegex(side["finished_utc"], f"^{stamp}$")

    def test_source_particle_mass_premise_check(self):
        src = self.control()
        self.assertEqual(self.rewrite([src], "0.0327", "--source-particle-mass", "0.0325")[0], 0)
        side = self.sidecar()
        self.assertEqual((side["source_particle_mass"], side["n_len_mismatch_source"]), (0.0325, 0))
        # One Len altered: the premise fails for exactly that record, reported, not an error.
        altered = write_lhalo(
            self.src / "altered.0", CONTROL_COUNTS, CONTROL_MASSES, LEN_AT_0325[:4] + [381]
        )
        code, out, _ = self.rewrite(
            [altered], "0.0327", "--source-particle-mass", "0.0325", out=self.tmp / "o2"
        )
        self.assertEqual(code, 0)
        self.assertEqual(self.sidecar("altered.0", self.tmp / "o2")["n_len_mismatch_source"], 1)
        self.assertIn("1 input Len differ", out)

    def test_sidecar_only_after_a_complete_output(self):
        src = self.control()
        self.out.mkdir()
        real_rename = os.rename

        def failing_rename(source, target):
            if str(target).endswith(".lhalo-len.json"):
                raise OSError("simulated failure between the output rename and the sidecar")
            return real_rename(source, target)

        with mock.patch.object(lhalo_len.os, "rename", failing_rename):
            with self.assertRaises(lhalo_len.LhaloLenError):
                lhalo_len.rewrite_file(str(src), str(self.out), 0.0327, "0.0327")
        names = sorted(p.name for p in self.out.iterdir())
        self.assertEqual(names, [src.name])  # no sidecar, no sidecar temporary file


class ChunkingTests(LhaloLenTestCase):
    def test_two_record_chunk_equals_single_chunk(self):
        src = self.control()
        self.assertEqual(self.rewrite([src], "0.0327", "--source-particle-mass", "0.0325")[0], 0)
        whole = self.sidecar()
        self.assertEqual(
            self.rewrite(
                [src], "0.0327", "--source-particle-mass", "0.0325",
                "--chunk-records", 2, out=self.tmp / "chunked",
            )[0],
            0,
        )  # fmt: skip
        chunked = self.sidecar(directory=self.tmp / "chunked")
        self.assertEqual(
            (self.tmp / "chunked" / src.name).read_bytes(), (self.out / src.name).read_bytes()
        )
        for key in (
            "sha256_input",
            "sha256_output",
            "n_records",
            "n_len_changed",
            "n_len_mismatch_source",
        ):
            self.assertEqual(chunked[key], whole[key], key)

    def test_check_in_two_record_chunks_equals_single_chunk(self):
        src = self.control()
        one = self.tmp / "one.json"
        two = self.tmp / "two.json"
        self.assertEqual(self.check([src], "0.0327", "--json", one)[0], 1)
        self.assertEqual(self.check([src], "0.0327", "--json", two, "--chunk-records", 2)[0], 1)
        self.assertEqual(json.loads(one.read_text()), json.loads(two.read_text()))

    def test_error_in_second_chunk_keeps_earlier_file_and_leaves_no_trace(self):
        good = self.control("a.0")
        masses = list(CONTROL_MASSES)
        masses[3] = float("nan")  # record 3: in the second chunk of two records
        bad = write_lhalo(self.src / "b.0", CONTROL_COUNTS, masses, LEN_AT_0325)
        code, _, err = self.rewrite([good, bad], "0.0327", "--chunk-records", 2)
        self.assertEqual(code, 2)
        self.assertIn(str(bad), err)
        self.assertIn("record 3", err)
        self.assertEqual(sorted(p.name for p in self.out.iterdir()), ["a.0", "a.0.lhalo-len.json"])
        self.assertEqual(len_column(self.out / "a.0"), LEN_AT_0327)
        self.assertEqual(self.sidecar("a.0")["n_records"], 5)


class CheckTests(LhaloLenTestCase):
    def test_matching_particle_mass_exit_0_with_interval(self):
        src = self.control()
        result = self.tmp / "result.json"
        code, out, err = self.check([src], "0.0325", "--json", result)
        self.assertEqual((code, err), (0, ""), out)
        data = json.loads(result.read_text())
        self.assertEqual(data["n_records"], 5)
        self.assertEqual(data["n_len_mismatch"], 0)
        self.assertEqual(data["n_negative_mass"], 0)
        self.assertEqual(data["interval_status"], "found")
        self.assertLessEqual(data["interval"]["low"], 0.0325)
        self.assertGreaterEqual(data["interval"]["high"], 0.0325)
        self.assertIn("interval", out)
        self.assertEqual(sorted(p.name for p in self.src.iterdir()), ["control.0"])

    def test_other_particle_mass_exit_1_with_literal_count_and_first_example(self):
        result = self.tmp / "result.json"
        code, out, _ = self.check([self.control()], "0.0327", "--json", result)
        self.assertEqual(code, 1)
        data = json.loads(result.read_text())
        self.assertEqual(data["n_len_mismatch"], 3)  # records 2, 3 and 4
        first = data["examples"][0]
        self.assertEqual((first["record"], first["len"], first["expected_len"]), (2, 1, 0))
        self.assertAlmostEqual(first["mass"], 0.0163, places=6)
        self.assertEqual([e["record"] for e in data["examples"]], [2, 3, 4])
        self.assertIn("Len mismatches: 3", out)

    def test_negative_mass_is_counted_and_left_out(self):
        masses = list(CONTROL_MASSES)
        masses[1] = -1.0  # its Len (0 here) is a sentinel, not mass-derived
        src = write_lhalo(self.src / "neg.0", CONTROL_COUNTS, masses, LEN_AT_0325)
        result = self.tmp / "result.json"
        code, _, _ = self.check([src], "0.0325", "--json", result)
        data = json.loads(result.read_text())
        self.assertEqual((code, data["n_records"], data["n_negative_mass"]), (0, 5, 1))
        self.assertEqual(data["n_len_mismatch"], 0)
        self.assertEqual(data["interval_status"], "found")

    def test_zero_halos_gives_the_no_interval_statement(self):
        src = write_lhalo(self.src / "empty.0", [], [], [])
        result = self.tmp / "result.json"
        code, out, _ = self.check([src], "0.0325", "--json", result)
        data = json.loads(result.read_text())
        self.assertEqual(code, 0)
        self.assertEqual((data["n_records"], data["n_len_mismatch"]), (0, 0))
        self.assertEqual(data["interval_status"], "none_comparable")
        self.assertIsNone(data["interval"])
        self.assertIn("no interval", out)

    def test_all_masses_negative_is_not_an_error(self):
        src = write_lhalo(self.src / "allneg.0", [2], [-1.0, -2.0], [5, 6])
        code, out, _ = self.check([src], "0.0325")
        self.assertEqual(code, 0)
        self.assertIn("no interval", out)

    def test_no_single_particle_mass_reproduces_the_lens(self):
        # The same mass cannot have two Len values.
        src = write_lhalo(self.src / "inconsistent.0", [2], [1.0, 1.0], [30, 40])
        result = self.tmp / "result.json"
        code, out, _ = self.check([src], "0.0325", "--json", result)
        data = json.loads(result.read_text())
        self.assertEqual(code, 1)
        self.assertEqual(data["interval_status"], "none_reproduces")
        self.assertIsNone(data["interval"])
        self.assertIn("no particle mass reproduces every Len", out)

    def test_several_files_are_reported_together(self):
        a = self.control("a.0")
        b = self.control("b.0")
        result = self.tmp / "result.json"
        self.assertEqual(self.check([a, b], "0.0325", "--json", result)[0], 0)
        self.assertEqual(json.loads(result.read_text())["n_records"], 10)

    def test_mass_field_option(self):
        src = self.control(mean200=MEAN200_MASSES)
        lens = [31, 61, 92, 122, 153]
        other = write_lhalo(
            self.src / "mean.0", CONTROL_COUNTS, CONTROL_MASSES, lens, mean200=MEAN200_MASSES
        )
        self.assertEqual(self.check([other], "0.0327", "--mass-field", "M_Mean200")[0], 0)
        self.assertEqual(self.check([src], "0.0327", "--mass-field", "M_Mean200")[0], 1)

    def test_json_that_is_an_input_is_refused_and_the_input_survives(self):
        a = self.control("a.0")
        b = self.control("b.0")
        before = a.read_bytes()
        link = self.tmp / "alias.json"
        link.symlink_to(a)
        for target in (a, link):
            with self.subTest(target=target):
                code, _, err = self.check([a, b], "0.0325", "--json", target)
                self.assertEqual(code, 3, err)
                self.assertIn(str(target), err)
                self.assertIn(str(a), err)
                self.assertEqual(a.read_bytes(), before)

    def test_json_in_a_missing_directory_is_refused_before_any_scan(self):
        target = self.tmp / "nodir" / "result.json"
        code, out, err = self.check([self.control()], "0.0325", "--json", target)
        self.assertEqual(code, 3, err)
        self.assertIn(str(target), err)
        self.assertNotIn("checked", out)

    def test_json_that_is_a_directory_is_refused(self):
        target = self.tmp / "adir"
        target.mkdir()
        code, _, err = self.check([self.control()], "0.0325", "--json", target)
        self.assertEqual(code, 3, err)
        self.assertIn(str(target), err)

    def test_stale_json_temporary_file_is_refused_and_left_alone(self):
        target = self.tmp / "result.json"
        stale = self.tmp / "result.json.tmp"
        stale.write_bytes(b"stale")
        code, _, err = self.check([self.control()], "0.0325", "--json", target)
        self.assertEqual(code, 3, err)
        self.assertIn(str(target), err)
        self.assertEqual(stale.read_bytes(), b"stale")
        self.assertFalse(target.exists())
        # The writer itself also refuses (mode "x") and removes nothing it did not create.
        with self.assertRaises(lhalo_len.LhaloLenError):
            lhalo_len._write_json_atomic(str(target), {})
        self.assertEqual(stale.read_bytes(), b"stale")

    def test_check_refusals(self):
        src = self.control()
        code, _, err = self.check([src], "0.0325", "--chunk-records", 0)
        self.assertEqual(code, 3)
        bad = write_lhalo(self.src / "nan.0", [1], [float("nan")], [1])
        code, _, err = self.check([bad])
        self.assertEqual(code, 2)
        self.assertIn(str(bad), err)
        self.assertIn("record 0", err)
        code, _, err = self.run_tool("check", "--expect-files", 2, src)
        self.assertEqual(code, 3)
        code, _, err = self.run_tool("check", "--particle-mass", "0.0325", "--expect-files", 2, src)
        self.assertEqual(code, 3)
        self.assertIn("2", err)
        self.assertIn("1", err)


class LayoutErrorTests(LhaloLenTestCase):
    """Exit 2: a file the tool refuses to read, named in the message."""

    def assertRefused(self, path, text, extra_out=None):
        code, _, err = self.rewrite([path])
        self.assertEqual(code, 2, err)
        self.assertIn(str(path), err)
        self.assertRegex(err, text)
        self.assertNothingWritten(extra_out)
        code, _, err = self.check([path])
        self.assertEqual(code, 2, err)
        self.assertIn(str(path), err)

    def test_file_shorter_than_header(self):
        path = self.src / "short.0"
        path.write_bytes(b"\x01\x00\x00")
        self.assertRefused(path, "shorter than the 8-byte header")

    def test_negative_ntrees(self):
        path = self.src / "negtrees.0"
        path.write_bytes(np.array([-1, 0], dtype="<i4").tobytes())
        self.assertRefused(path, "negative header count")

    def test_negative_total(self):
        path = self.src / "negtotal.0"
        path.write_bytes(np.array([0, -5], dtype="<i4").tobytes())
        self.assertRefused(path, "negative header count")

    def test_tree_counts_do_not_fit_in_the_file(self):
        path = self.src / "nocounts.0"
        path.write_bytes(np.array([1000, 0], dtype="<i4").tobytes())
        self.assertRefused(path, "too short for the 1000 per-tree counts")

    def test_negative_tree_count(self):
        path = self.src / "negcount.0"
        path.write_bytes(np.array([2, 1, 3, -2], dtype="<i4").tobytes() + bytes(104))
        self.assertRefused(path, "negative per-tree halo count")

    def test_header_total_disagrees_with_tree_counts(self):
        path = self.src / "sum.0"
        raw = bytearray(self.control("good.0").read_bytes())
        raw[4:8] = np.array([6], dtype="<i4").tobytes()  # totNHalos 6; the trees sum to 5
        path.write_bytes(bytes(raw) + bytes(104))
        self.assertRefused(path, "sums to 5, header totNHalos is 6")

    def test_truncated_file(self):
        path = self.src / "trunc.0"
        path.write_bytes(self.control("good.0").read_bytes()[:-1])
        self.assertRefused(path, "bytes, not")

    def test_truncated_second_input_is_refused_before_anything_is_written(self):
        good = self.control("a.0")
        bad = self.src / "b.0"
        bad.write_bytes(good.read_bytes()[:-104])
        code, _, err = self.rewrite([good, bad])
        self.assertEqual(code, 2)
        self.assertIn(str(bad), err)
        self.assertNothingWritten()

    def test_source_particle_mass_range_error_names_the_option(self):
        path = self.control()
        code, _, err = self.rewrite([path], "0.0327", "--source-particle-mass", "1e-6")
        self.assertEqual(code, 2)
        self.assertIn(str(path), err)
        self.assertIn("--source-particle-mass", err)
        self.assertIn("1e-06", err)
        self.assertIn("int32 range", err)
        self.assertEqual([p.name for p in self.out.iterdir()], [])

    def test_negative_mass_is_refused_by_rewrite(self):
        masses = list(CONTROL_MASSES)
        masses[4] = -0.5
        path = write_lhalo(self.src / "neg.0", CONTROL_COUNTS, masses, LEN_AT_0325)
        code, _, err = self.rewrite([path])
        self.assertEqual(code, 2)
        self.assertIn(str(path), err)
        self.assertIn("record 4", err)
        self.assertIn("negative", err)
        self.assertEqual([p.name for p in self.out.iterdir()], [])

    def test_nan_mass(self):
        masses = list(CONTROL_MASSES)
        masses[2] = float("nan")
        path = write_lhalo(self.src / "nan.0", CONTROL_COUNTS, masses, LEN_AT_0325)
        code, _, err = self.rewrite([path])
        self.assertEqual(code, 2)
        self.assertIn(str(path), err)
        self.assertIn("record 2", err)
        self.assertIn("non-finite", err)
        self.assertEqual([p.name for p in self.out.iterdir()], [])

    def test_huge_finite_mass_exceeds_int32(self):
        masses = list(CONTROL_MASSES)
        masses[3] = 1e30
        path = write_lhalo(self.src / "huge.0", CONTROL_COUNTS, masses, LEN_AT_0325)
        code, _, err = self.rewrite([path])
        self.assertEqual(code, 2)
        self.assertIn(str(path), err)
        self.assertIn("record 3", err)
        self.assertIn("int32 range", err)
        self.assertEqual([p.name for p in self.out.iterdir()], [])


class UsageRefusalTests(LhaloLenTestCase):
    """Exit 3, each naming what the contract says, with nothing written."""

    def refused(self, code_err, *names):
        code, _, err = code_err
        self.assertEqual(code, 3, err)
        for name in names:
            self.assertIn(str(name), err)
        self.assertNothingWritten()

    def test_invalid_particle_masses(self):
        src = self.control()
        for value in ("0", "-1", "nan", "inf", "-inf", "abc", "1e-400"):
            with self.subTest(value=value):
                self.refused(self.rewrite([src], value), "--particle-mass", repr(value))

    def test_invalid_source_particle_masses(self):
        src = self.control()
        for value in ("0", "-0.5", "nan", "inf", "abc"):
            with self.subTest(value=value):
                self.refused(
                    self.rewrite([src], "0.0327", "--source-particle-mass", value),
                    "--source-particle-mass",
                    repr(value),
                )

    def test_both_or_neither_particle_mass_option(self):
        src = self.control()
        info = self.tmp / "info.yaml"
        info.write_text(SIMULATION_INFO_0327)
        self.refused(
            self.run_tool(
                "rewrite", "--particle-mass", "0.0327", "--simulation-info", info,
                "--out", self.out, "--expect-files", 1, src,
            ),
            "--particle-mass",
            "--simulation-info",
        )  # fmt: skip
        self.refused(
            self.run_tool("rewrite", "--out", self.out, "--expect-files", 1, src),
            "--particle-mass",
            "--simulation-info",
        )

    def test_expect_files_differs_from_the_path_count(self):
        src = self.control()
        code, _, err = self.rewrite([src], "0.0327", expect=3)
        self.assertEqual(code, 3)
        self.assertRegex(err, r"3.*1|1.*3")
        self.assertIn("--expect-files", err)
        self.assertNothingWritten()

    def test_missing_path(self):
        missing = self.src / "absent.0"
        self.refused(self.rewrite([self.control(), missing]), missing)

    def test_path_that_is_a_directory(self):
        self.refused(self.rewrite([self.src]), self.src)

    def test_unreadable_path(self):
        path = self.control()
        path.chmod(0)
        self.addCleanup(path.chmod, 0o644)
        if os.access(path, os.R_OK):
            self.skipTest("permissions are not enforced for this user")
        self.refused(self.rewrite([path]), path)

    def test_same_file_twice_by_link(self):
        src = self.control()
        link = self.src / "link.0"
        link.symlink_to(src)
        self.refused(self.rewrite([src, link]), src, link)

    def test_same_output_name_from_two_directories(self):
        other = self.tmp / "other"
        other.mkdir()
        a = self.control()
        b = self.control(directory=other)
        self.refused(self.rewrite([a, b]), a, b)

    def test_out_is_the_inputs_directory(self):
        src = self.control()
        before = sorted(p.name for p in self.src.iterdir())
        code, _, err = self.rewrite([src], out=self.src)
        self.assertEqual(code, 3)
        self.assertIn(str(self.src.resolve()), err)
        self.assertEqual(sorted(p.name for p in self.src.iterdir()), before)

    def test_out_is_a_link_to_the_inputs_directory(self):
        src = self.control()
        link = self.tmp / "snapshots"
        link.symlink_to(self.src)
        code, _, err = self.rewrite([src], out=link)
        self.assertEqual(code, 3)
        self.assertIn(str(self.src.resolve()), err)
        self.assertEqual([p.name for p in self.src.iterdir()], ["control.0"])

    def test_out_is_the_directory_of_a_linked_inputs_resolved_file(self):
        data = self.tmp / "data"
        data.mkdir()
        real = self.control(directory=data)
        link = self.src / "control.0"
        link.symlink_to(real)
        code, _, err = self.rewrite([link], out=data)
        self.assertEqual(code, 3)
        self.assertIn(str(data.resolve()), err)
        self.assertEqual([p.name for p in data.iterdir()], ["control.0"])

    def test_existing_output_sidecar_or_temporary_files_are_refused(self):
        src = self.control()
        for name in (
            "control.0",
            "control.0.lhalo-len.json",
            ".control.0.lhalo-len-tmp",
            ".control.0.lhalo-len.json.tmp",
        ):
            with self.subTest(name=name):
                out = self.tmp / f"out_{abs(hash(name))}"
                out.mkdir()
                (out / name).write_bytes(b"stale")
                code, _, err = self.rewrite([src], out=out)
                self.assertEqual(code, 3, err)
                self.assertIn(str(out / name), err)
                self.assertEqual([p.name for p in out.iterdir()], [name])
                self.assertEqual((out / name).read_bytes(), b"stale")

    def test_bad_simulation_info(self):
        src = self.control()
        cases = {
            "missing.yaml": None,
            "novalue.yaml": "simulation:\n  particle_mass:\n    units: 1e10 Msun/h\n",
            "zero.yaml": "simulation:\n  particle_mass:\n    value: 0\n    units: 1e10 Msun/h\n",
            "negative.yaml": "simulation:\n  particle_mass:\n    value: -1\n    units: 1e10 Msun/h\n",
            "nan.yaml": "simulation:\n  particle_mass:\n    value: .nan\n    units: 1e10 Msun/h\n",
            "text.yaml": "simulation:\n  particle_mass:\n    value: abc\n    units: 1e10 Msun/h\n",
            "units.yaml": "simulation:\n  particle_mass:\n    value: 0.0327\n    units: Msun/h\n",
            "nounits.yaml": "simulation:\n  particle_mass:\n    value: 0.0327\n",
            "nosim.yaml": "box:\n  value: 1\n",
            "broken.yaml": "simulation: [unclosed\n",
        }
        for name, text in cases.items():
            with self.subTest(name=name):
                path = self.tmp / name
                if text is not None:
                    path.write_text(text)
                code, _, err = self.run_tool(
                    "rewrite", "--simulation-info", path, "--out", self.out,
                    "--expect-files", 1, src,
                )  # fmt: skip
                self.assertEqual(code, 3, err)
                self.assertIn(str(path), err)
                self.assertNothingWritten()

    def test_other_usage_errors(self):
        src = self.control()
        cases = {
            "no path": ["rewrite", "--particle-mass", "0.0327", "--out", self.out, "--expect-files", 0],
            "unknown option": ["rewrite", "--particle-mass", "0.0327", "--bogus", "--out", self.out,
                               "--expect-files", 1, src],
            "bad mass field": ["rewrite", "--particle-mass", "0.0327", "--mass-field", "Mvir",
                               "--out", self.out, "--expect-files", 1, src],
            "no out": ["rewrite", "--particle-mass", "0.0327", "--expect-files", 1, src],
            "no expect-files": ["rewrite", "--particle-mass", "0.0327", "--out", self.out, src],
            "non-integer expect-files": ["rewrite", "--particle-mass", "0.0327", "--out", self.out,
                                         "--expect-files", "x", src],
            "no command": [],
        }  # fmt: skip
        for name, args in cases.items():
            with self.subTest(name=name):
                code, _, err = self.run_tool(*args)
                self.assertEqual(code, 3, err)
                self.assertNothingWritten()

    def test_help_exits_zero(self):
        self.assertEqual(self.run_tool("--help")[0], 0)

    def test_new_output_directory_is_created_with_parents(self):
        src = self.control()
        out = self.tmp / "a" / "b" / "c"
        self.assertEqual(self.rewrite([src], out=out)[0], 0)
        self.assertTrue((out / "control.0").is_file())

    def test_second_run_into_the_same_directory_is_refused(self):
        src = self.control()
        self.assertEqual(self.rewrite([src])[0], 0)
        first = (self.out / src.name).read_bytes()
        code, _, err = self.rewrite([src], "0.0325")
        self.assertEqual(code, 3, err)
        self.assertEqual((self.out / src.name).read_bytes(), first)


if __name__ == "__main__":
    unittest.main()
