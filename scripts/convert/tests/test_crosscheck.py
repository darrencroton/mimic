"""Slice 8 unit tests: the six-check cross-check must catch every deliberately
injected violation, and the reference-run plumbing helpers + CLI must behave
as frozen. One shared pristine mock reference (built from the fixture pipeline)
is reused; per-violation tests deep-copy the galaxies dict, mutate, and write a
fresh reference directory."""

import os
import stat
import sys
import tempfile
import unittest
from pathlib import Path

import numpy as np
import yaml

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import crosscheck  # noqa: E402
import validate  # noqa: E402
from crosscheck import (  # noqa: E402
    ConverterError,
    run_crosscheck,
    run_reference,
    write_reference_run_file,
)
from mock_reference import GALAXY_DTYPE, build_mock_galaxies, write_mock_reference  # noqa: E402
from test_hdf5_writer import make_written_workdir  # noqa: E402

M = 10**9


def outcome_map(outcomes):
    return {outcome.name: outcome for outcome in outcomes}


def golden_table(galaxies_by_snap):
    """abs(MostBoundID) -> (UniqueGalaxyID, Type) over Type 0/1 per snapshot."""
    table = {}
    for snap, gals in galaxies_by_snap.items():
        entry = {}
        for g in gals:
            if int(g["Type"]) in (0, 1):
                entry[int(abs(g["MostBoundID"]))] = (int(g["UniqueGalaxyID"]), int(g["Type"]))
        if entry:
            table[snap] = entry
    return table


def find_row(gals, mostboundid, types=(0, 1)):
    """Index of the row whose |MostBoundID| matches and whose Type is allowed."""
    for i in range(gals.size):
        if int(abs(gals[i]["MostBoundID"])) == mostboundid and int(gals[i]["Type"]) in types:
            return i
    raise AssertionError("no row with |MostBoundID| {} type {}".format(mostboundid, types))


class TestCrosscheck(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        root = Path(cls.tmp.name)
        cls.workdir, cls.a_list_path, cls.sim_info, cls.hdf5_dir = make_written_workdir(root)
        cls.n_snapshots = 6
        _, cls.arrays = validate.load_dataset(cls.hdf5_dir, cls.n_snapshots)
        cls.pristine = build_mock_galaxies(cls.hdf5_dir, cls.n_snapshots)
        cls.reference_dir = Path(tempfile.mkdtemp(dir=cls.tmp.name)) / "reference"
        write_mock_reference(cls.pristine, cls.reference_dir, n_snapshots=cls.n_snapshots)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    # -- helpers -------------------------------------------------------------

    def _copy(self):
        return {k: v.copy() for k, v in self.pristine.items()}

    def _write_ref(self, galaxies):
        directory = Path(tempfile.mkdtemp(dir=self.tmp.name)) / "reference"
        write_mock_reference(galaxies, directory, n_snapshots=self.n_snapshots)
        return directory

    def _run(self, reference_dir, multiplier=M):
        return outcome_map(
            run_crosscheck(self.hdf5_dir, reference_dir, self.a_list_path, multiplier=multiplier)
        )

    def _run_mutated(self, galaxies):
        return self._run(self._write_ref(galaxies))

    def assert_fails(self, outcomes, name):
        self.assertEqual(outcomes[name].status, "FAIL", outcomes[name].line())

    def _append(self, galaxies, snap, row):
        galaxies[snap] = np.concatenate([galaxies[snap], row.reshape(1)])

    def _new_row(self, snap, halo_index, gtype, ugid, central_ugid, mostboundid):
        conv = self.arrays[snap]
        row = np.zeros((), dtype=GALAXY_DTYPE)
        row["SnapNum"] = snap
        row["Type"] = gtype
        row["UniqueGalaxyID"] = ugid
        row["UniqueCentralGalaxyID"] = central_ugid
        row["Pos"] = conv["Pos"][halo_index]
        row["Vel"] = conv["Vel"][halo_index]
        row["Spin"] = conv["Spin"][halo_index]
        row["VelDisp"] = conv["VelDisp"][halo_index]
        row["Vmax"] = conv["Vmax"][halo_index]
        row["Len"] = conv["Len"][halo_index]
        row["Mvir"] = np.float64(conv["M_Crit200"][halo_index]) * 1e-10
        row["MostBoundID"] = mostboundid
        return row

    # -- pristine + golden facts ---------------------------------------------

    def test_pristine_passes(self):
        outcomes = self._run(self.reference_dir)
        failed = [o.line() for o in outcomes.values() if o.status != "PASS"]
        self.assertEqual(failed, [])

    def test_golden_ugid_table(self):
        self.assertEqual(
            golden_table(self.pristine),
            {
                1: {4011: (1 + 3 * M, 0)},
                2: {4010: (1 + 3 * M, 0)},
                3: {1013: (5 + M, 0)},
                4: {
                    1011: (5 + M, 0),
                    1012: (3 + M, 0),
                    1021: (4 + M, 0),
                    2012: (2 + 2 * M, 0),
                },
                5: {
                    1010: (5 + M, 0),
                    1020: (4 + M, 1),
                    2010: (2 + 2 * M, 0),
                    5010: (4 * M, 0),
                    6010: (5 * M, 0),
                },
            },
        )

    def test_golden_flyby_satellite_details(self):
        snap5 = self.pristine[5]
        i = find_row(snap5, 1020, types=(1,))
        self.assertEqual(int(snap5[i]["MostBoundID"]), -1020)
        self.assertEqual(int(snap5[i]["UniqueCentralGalaxyID"]), 5 + M)

    def test_orphan_row_present_and_ignored(self):
        snap5 = self.pristine[5]
        orphans = snap5[snap5["Type"] == 2]
        self.assertEqual(orphans.size, 1)
        self.assertEqual(int(orphans[0]["UniqueGalaxyID"]), 3 + M)  # merged 1012 galaxy
        self.assertEqual(int(orphans[0]["MostBoundID"]), 1012)
        # central-ID propagation: 1012 merged into 1010, whose galaxy is 5 + M
        self.assertEqual(int(orphans[0]["UniqueCentralGalaxyID"]), 5 + M)

    # -- 1. identity-forest --------------------------------------------------

    def test_identity_forest_violation(self):
        g = self._copy()
        i = find_row(g[5], 2010)
        g[5]["UniqueGalaxyID"][i] += M
        self.assert_fails(self._run_mutated(g), "identity-forest")

    # -- 2. identity-creation ------------------------------------------------

    def test_identity_creation_violation(self):
        g = self._copy()
        i = find_row(g[5], 5010)
        g[5]["UniqueGalaxyID"][i] += 1
        self.assert_fails(self._run_mutated(g), "identity-creation")

    # -- 3. fof-central ------------------------------------------------------

    def test_fof_central_violation(self):
        g = self._copy()
        i = find_row(g[5], 1020)
        g[5]["UniqueCentralGalaxyID"][i] = 2 + 2 * M  # 2010's ugid, not 1010's
        self.assert_fails(self._run_mutated(g), "fof-central")

    # -- 4. flyby-signs ------------------------------------------------------

    def test_flyby_signs_violation(self):
        g = self._copy()
        i = find_row(g[5], 1020)
        g[5]["MostBoundID"][i] = 1020  # drop the negative flyby marker
        self.assert_fails(self._run_mutated(g), "flyby-signs")

    # -- 5. values -----------------------------------------------------------

    def test_values_violation_vmax(self):
        g = self._copy()
        i = find_row(g[5], 1010)
        g[5]["Vmax"][i] += np.float32(1.0)
        self.assert_fails(self._run_mutated(g), "values")

    def test_values_violation_pos(self):
        g = self._copy()
        i = find_row(g[5], 1010)
        g[5]["Pos"][i][1] += np.float32(0.5)
        self.assert_fails(self._run_mutated(g), "values")

    def test_values_violation_mvir(self):
        g = self._copy()
        i = find_row(g[5], 1010)
        g[5]["Mvir"][i] *= 1.0000001
        self.assert_fails(self._run_mutated(g), "values")

    def test_values_violation_len(self):
        g = self._copy()
        i = find_row(g[5], 1010)
        g[5]["Len"][i] += 1
        self.assert_fails(self._run_mutated(g), "values")

    # -- 6. occupancy --------------------------------------------------------

    def test_occupancy_missing_galaxy(self):
        g = self._copy()
        i = find_row(g[4], 1021)
        g[4] = np.delete(g[4], i)
        self.assert_fails(self._run_mutated(g), "occupancy")

    def test_occupancy_spurious_galaxy(self):
        # converter halo 2013 (snap4 index 4) is predicted unoccupied; a
        # reference galaxy matched to it is a matched-but-unoccupied violation
        g = self._copy()
        row = self._new_row(4, 4, gtype=1, ugid=3 + 2 * M, central_ugid=2 + 2 * M, mostboundid=2013)
        self._append(g, 4, row)
        self.assert_fails(self._run_mutated(g), "occupancy")

    def test_occupancy_unmatched_galaxy(self):
        g = self._copy()
        row = self._new_row(
            5, 0, gtype=0, ugid=7 * M + 7, central_ugid=7 * M + 7, mostboundid=99999
        )
        self._append(g, 5, row)
        self.assert_fails(self._run_mutated(g), "occupancy")

    # -- reference sanity ----------------------------------------------------

    def test_reference_sanity_duplicate_mostboundid(self):
        g = self._copy()
        i = find_row(g[5], 2010)
        self._append(g, 5, g[5][i])
        self.assert_fails(self._run_mutated(g), "reference-sanity")

    def test_reference_sanity_wrong_snapnum(self):
        g = self._copy()
        g[5]["SnapNum"][0] = 4
        self.assert_fails(self._run_mutated(g), "reference-sanity")

    def test_reference_sanity_duplicate_type1_ugid(self):
        # a Type 1 galaxy reusing another live galaxy's persistent identity
        # must be rejected: identity-creation alone would skip an already-seen
        # id, so uniqueness is enforced over ALL Type 0/1 galaxies
        g = self._copy()
        i = find_row(g[5], 1020, types=(1,))
        g[5]["UniqueGalaxyID"][i] = 2 + 2 * M  # 2010's live identity
        self.assert_fails(self._run_mutated(g), "reference-sanity")

    def test_reference_int64_min_mostboundid_aborts(self):
        # rejected in build_matches BEFORE any magnitude is taken, so the
        # overflowed value can never influence matching (defense-in-depth: the
        # reference-sanity check carries the same rejection)
        g = self._copy()
        i = find_row(g[5], 1010)
        g[5]["MostBoundID"][i] = np.iinfo(np.int64).min
        directory = self._write_ref(g)
        with self.assertRaisesRegex(ConverterError, "INT64_MIN"):
            run_crosscheck(self.hdf5_dir, directory, self.a_list_path)

    def test_converter_int64_min_aborts(self):
        import shutil

        import h5py

        corrupted = Path(tempfile.mkdtemp(dir=self.tmp.name)) / "hdf5"
        shutil.copytree(self.hdf5_dir, corrupted)
        with h5py.File(corrupted / "snapshot_003.h5", "r+") as handle:
            handle["halos"]["MostBoundID"][...] = np.asarray(
                [np.iinfo(np.int64).min], dtype=np.int64
            )
        with self.assertRaisesRegex(ConverterError, "INT64_MIN"):
            run_crosscheck(corrupted, self.reference_dir, self.a_list_path)

    # -- Type-2 filter -------------------------------------------------------

    def test_type2_orphan_ignored(self):
        g = self._copy()
        i = find_row(g[5], 1012, types=(2,))
        g[5]["Vmax"][i] = np.float32(1e30)
        g[5]["MostBoundID"][i] = 424242
        outcomes = self._run_mutated(g)
        failed = [o.line() for o in outcomes.values() if o.status != "PASS"]
        self.assertEqual(failed, [])

    # -- reference loading edge cases ----------------------------------------

    def test_master_file_ignored(self):
        # the pristine reference dir carries a master halos.hdf5 that must be
        # skipped; the pristine run already passed, proving it is not read
        self.assertTrue((self.reference_dir / "halos.hdf5").exists())
        self.assertTrue((self.reference_dir / "halos_000.hdf5").exists())

    def test_missing_required_field_aborts(self):
        directory = Path(tempfile.mkdtemp(dir=self.tmp.name)) / "reference"
        directory.mkdir(parents=True)
        import h5py

        trimmed = np.dtype([field for field in GALAXY_DTYPE.descr if field[0] != "Mvir"])
        with h5py.File(directory / "halos_000.hdf5", "w") as handle:
            handle.create_group("Snap005").create_dataset(
                "Galaxies", data=np.zeros(1, dtype=trimmed)
            )
        with self.assertRaisesRegex(ConverterError, "missing field"):
            run_crosscheck(self.hdf5_dir, directory, self.a_list_path)

    def test_wrong_field_width_aborts(self):
        # float64 Pos would let a wrong reference value round back onto the
        # converter's float32 bits; exact widths are enforced, never coerced
        directory = Path(tempfile.mkdtemp(dir=self.tmp.name)) / "reference"
        directory.mkdir(parents=True)
        import h5py

        fields = []
        for field in GALAXY_DTYPE.descr:
            name = field[0]
            base = np.float64 if name == "Pos" else field[1]
            fields.append((name, base, field[2]) if len(field) == 3 else (name, base))
        widened = np.dtype(fields)
        with h5py.File(directory / "halos_000.hdf5", "w") as handle:
            handle.create_group("Snap005").create_dataset(
                "Galaxies", data=np.zeros(1, dtype=widened)
            )
        with self.assertRaisesRegex(ConverterError, "mistypes"):
            run_crosscheck(self.hdf5_dir, directory, self.a_list_path)

    def test_mixed_chunk_dtype_aborts(self):
        # a second chunk with a different structured dtype would promote the
        # concatenated array; chunks must share one dtype
        import h5py

        directory = Path(tempfile.mkdtemp(dir=self.tmp.name)) / "reference"
        write_mock_reference(self._copy(), directory, n_snapshots=self.n_snapshots)
        extended = np.dtype(GALAXY_DTYPE.descr + [("Extra", np.float64)])
        with h5py.File(directory / "halos_001.hdf5", "w") as handle:
            handle.create_group("Snap004").create_dataset(
                "Galaxies", data=np.zeros(1, dtype=extended)
            )
        with self.assertRaisesRegex(ConverterError, "must share one structured dtype"):
            run_crosscheck(self.hdf5_dir, directory, self.a_list_path)

    # -- plumbing: write_reference_run_file ----------------------------------

    def _source_run_file(self):
        root = Path(tempfile.mkdtemp(dir=self.tmp.name))
        path = root / "source_run.yaml"
        data = {
            "model": {"name": "halos-only"},
            "simulation": {"name": "micro-uchuu"},
            "output": {
                "output_filename": "halos",
                "output_directory": "/repo/original/output",
                "output_format": "hdf5",
                "snapshot_list": [0, 1],
            },
            "SubSteps": 1,
            "modules": {"phases": []},
        }
        with open(path, "w") as handle:
            yaml.safe_dump(data, handle)
        return path

    def test_write_reference_run_file(self):
        source = self._source_run_file()
        source_bytes = source.read_bytes()
        target = source.parent / "reference_run.yaml"
        write_reference_run_file(source, target, range(6), "/work/reference-output")
        with open(target) as handle:
            data = yaml.safe_load(handle)
        self.assertEqual(data["output"]["snapshot_list"], [0, 1, 2, 3, 4, 5])
        self.assertEqual(data["output"]["output_directory"], "/work/reference-output")
        self.assertEqual(data["output"]["output_filename"], "halos")
        self.assertEqual(data["output"]["output_format"], "hdf5")
        self.assertEqual(data["model"], {"name": "halos-only"})
        self.assertEqual(data["simulation"], {"name": "micro-uchuu"})
        self.assertEqual(data["SubSteps"], 1)
        self.assertEqual(data["modules"], {"phases": []})
        self.assertEqual(source.read_bytes(), source_bytes)  # source untouched

    def test_write_reference_run_file_refuses_source(self):
        source = self._source_run_file()
        with self.assertRaisesRegex(ConverterError, "refusing to overwrite the source"):
            write_reference_run_file(source, source, range(6), "/work/out")

    # -- plumbing: run_reference ---------------------------------------------

    def _fake_executable(self, rc):
        root = Path(tempfile.mkdtemp(dir=self.tmp.name))
        script = root / "fake_mimic.sh"
        script.write_text('#!/bin/sh\necho "ran $1"\nexit {}\n'.format(rc))
        script.chmod(script.stat().st_mode | stat.S_IEXEC | stat.S_IRUSR)
        return script

    def test_run_reference_captures_rc_and_log(self):
        script = self._fake_executable(0)
        log = script.parent / "run.log"
        rc = run_reference(script, "some_run.yaml", log)
        self.assertEqual(rc, 0)
        text = log.read_text()
        self.assertIn("ran some_run.yaml", text)
        self.assertIn("exit code: 0", text)

    def test_run_reference_nonzero_rc(self):
        script = self._fake_executable(3)
        log = script.parent / "run.log"
        rc = run_reference(script, "some_run.yaml", log)
        self.assertEqual(rc, 3)
        self.assertIn("exit code: 3", log.read_text())

    # -- CLI -----------------------------------------------------------------

    def test_cli_compare_pass_and_fail(self):
        rc = crosscheck.main(
            [
                "compare",
                str(self.hdf5_dir),
                str(self.reference_dir),
                "--a-list",
                str(self.a_list_path),
            ]
        )
        self.assertEqual(rc, 0)

        g = self._copy()
        i = find_row(g[5], 1010)
        g[5]["Vmax"][i] += np.float32(1.0)
        violated = self._write_ref(g)
        report_path = Path(tempfile.mkdtemp(dir=self.tmp.name)) / "report.json"
        rc = crosscheck.main(
            [
                "compare",
                str(self.hdf5_dir),
                str(violated),
                "--a-list",
                str(self.a_list_path),
                "--report",
                str(report_path),
            ]
        )
        self.assertEqual(rc, 1)
        self.assertTrue(report_path.exists())

    def test_cli_prepare(self):
        source = self._source_run_file()
        workdir = Path(tempfile.mkdtemp(dir=self.tmp.name))
        rc = crosscheck.main(
            [
                "prepare",
                "--run-file",
                str(source),
                "--workdir",
                str(workdir),
                "--a-list",
                str(self.a_list_path),
            ]
        )
        self.assertEqual(rc, 0)
        target = workdir / "reference_run.yaml"
        self.assertTrue(target.exists())
        with open(target) as handle:
            data = yaml.safe_load(handle)
        self.assertEqual(data["output"]["snapshot_list"], [0, 1, 2, 3, 4, 5])
        self.assertEqual(data["output"]["output_directory"], str(workdir / "reference-output"))

    def test_cli_run_reference(self):
        script = self._fake_executable(3)
        log = script.parent / "run.log"
        rc = crosscheck.main(
            [
                "run-reference",
                "--mimic",
                str(script),
                "--run-file",
                "some_run.yaml",
                "--log",
                str(log),
            ]
        )
        self.assertEqual(rc, 3)


if __name__ == "__main__":
    unittest.main()
