"""Unit tests for the cross-check: it must catch every deliberately injected
violation, and the reference-run plumbing helpers + CLI must behave as
frozen. One shared pristine mock reference (built from the fixture pipeline)
is reused; per-violation tests deep-copy the galaxies dict, mutate, and write a
fresh reference directory."""

import os
import stat
import sys
import tempfile
import tracemalloc
import unittest
from pathlib import Path
from typing import Dict, List
from unittest import mock

import numpy as np
import yaml

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import crosscheck  # noqa: E402
import validate  # noqa: E402
from crosscheck import (  # noqa: E402
    ConverterError,
    SnapMatch,
    check_flyby_signs,
    check_identity_creation,
    check_values,
    run_crosscheck,
    run_reference,
    write_reference_run_file,
)
from fixtures import write_simulation_info  # noqa: E402
from fixups import load_particle_mass  # noqa: E402
from mock_reference import GALAXY_DTYPE, build_mock_galaxies, write_mock_reference  # noqa: E402
from test_hdf5_writer import make_written_workdir  # noqa: E402
from test_validate import write_synthetic_dataset  # noqa: E402

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
        cls.part_mass = load_particle_mass(cls.sim_info)
        cls.pristine = build_mock_galaxies(cls.hdf5_dir, cls.n_snapshots, cls.sim_info)
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
            run_crosscheck(
                self.hdf5_dir,
                reference_dir,
                self.a_list_path,
                self.sim_info,
                multiplier=multiplier,
            )
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
        halo_mass = np.float64(conv["M_Crit200"][halo_index]) * 1e-10
        is_central = int(conv["FirstHaloInFOFgroup"][halo_index]) == halo_index
        row["Mvir"] = (
            halo_mass
            if (is_central and halo_mass >= 0.0)
            else np.float64(conv["Len"][halo_index]) * self.part_mass
        )
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

    def test_wrong_multiplier_fails_with_the_frozen_report_detail(self):
        """A wrong multiplier must fail, and the reported detail is pinned
        verbatim against the whole-dataset formulation's own text: the same
        message, the same counts and examples, and the same ascending-snapshot
        order. Snapshot 2 is ABSENT from identity-creation because that
        snapshot's only galaxy carries an identity first seen at snapshot 1 —
        the suppression rule, visible in the report itself."""
        outcomes = self._run(self.reference_dir, multiplier=M * 10)
        self.assertEqual(
            outcomes["identity-forest"].detail,
            "snapshot 1: 1 matched galaxy(ies) whose decoded forest != halo ForestIndex; "
            "example ctrees ids: 4011; "
            "snapshot 2: 1 matched galaxy(ies) whose decoded forest != halo ForestIndex; "
            "example ctrees ids: 4010; "
            "snapshot 3: 1 matched galaxy(ies) whose decoded forest != halo ForestIndex; "
            "example ctrees ids: 1013; "
            "snapshot 4: 4 matched galaxy(ies) whose decoded forest != halo ForestIndex; "
            "example ctrees ids: 1011, 1012, 1021, 2012; "
            "snapshot 5: 5 matched galaxy(ies) whose decoded forest != halo ForestIndex; "
            "example ctrees ids: 1010, 1020, 2010, 5010, 6010",
        )
        self.assertEqual(
            outcomes["identity-creation"].detail,
            "snapshot 1: 1 newly-created galaxy(ies) whose decoded (forest, rank) != halo "
            "(ForestIndex, HaloRankInForest); example ctrees ids: 4011; "
            "snapshot 3: 1 newly-created galaxy(ies) whose decoded (forest, rank) != halo "
            "(ForestIndex, HaloRankInForest); example ctrees ids: 1013; "
            "snapshot 4: 3 newly-created galaxy(ies) whose decoded (forest, rank) != halo "
            "(ForestIndex, HaloRankInForest); example ctrees ids: 1012, 1021, 2012; "
            "snapshot 5: 2 newly-created galaxy(ies) whose decoded (forest, rank) != halo "
            "(ForestIndex, HaloRankInForest); example ctrees ids: 5010, 6010",
        )

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

    def test_flyby_signs_ignores_unmatched_negated_halo(self):
        # A correctly flyby-demoted converter halo that seeds no galaxy has no
        # reference counterpart; the check compares only the matched population,
        # so its negated sign must NOT register as a mismatch.
        conv = np.zeros(3, dtype=[("MostBoundID", np.int64)])
        conv["MostBoundID"] = [10, -20, 30]  # halo 1 is flyby-negated and unmatched
        ref = np.zeros(2, dtype=[("MostBoundID", np.int64), ("Type", np.int32)])
        ref["MostBoundID"] = [10, 30]
        match = SnapMatch(0, ref, np.array([0, 1]), conv, np.array([0, 2]))
        self.assertEqual(check_flyby_signs(match), [])
        # but a sign disagreement on a matched halo still fails
        conv["MostBoundID"] = [-10, -20, 30]  # matched halo 0 negated, reference positive
        self.assertTrue(check_flyby_signs(match))

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

    def test_values_mvir_is_type_dependent(self):
        # Reference Mvir follows get_virial_mass: FoF central with a valid
        # catalog mass -> M_Crit200*1e-10; every other halo -> Len*PartMass.
        # A central (halo 0) and a satellite (halo 1, in halo 0's FoF group)
        # with M_Crit200 chosen so Len*PartMass differs from M_Crit200*1e-10.
        part_mass = 0.0325
        conv = np.zeros(
            2,
            dtype=[
                ("Pos", np.float32, (3,)),
                ("Vel", np.float32, (3,)),
                ("Spin", np.float32, (3,)),
                ("VelDisp", np.float32),
                ("Vmax", np.float32),
                ("Len", np.int32),
                ("M_Crit200", np.float32),
                ("MostBoundID", np.int64),
                ("FirstHaloInFOFgroup", np.int32),
            ],
        )
        conv["FirstHaloInFOFgroup"] = [0, 0]  # halo 0 central, halo 1 satellite
        conv["Len"] = [100, 7]
        conv["M_Crit200"] = [np.float32(3.25e9), np.float32(9.99e9)]
        conv["MostBoundID"] = [10, 20]
        ref = np.zeros(2, dtype=GALAXY_DTYPE)
        ref["MostBoundID"] = [10, 20]
        ref["Type"] = [0, 1]
        ref["Len"] = [100, 7]  # match conv Len so only Mvir is under test
        central_ok = np.float64(np.float32(3.25e9)) * 1e-10
        sat_ok = np.float64(7) * part_mass
        match = SnapMatch(0, ref, np.array([0, 1]), conv, np.array([0, 1]))

        ref["Mvir"] = [central_ok, sat_ok]
        self.assertEqual(check_values(match, part_mass), [])
        # satellite must NOT take the central (M_Crit200) rule
        ref["Mvir"] = [central_ok, np.float64(np.float32(9.99e9)) * 1e-10]
        self.assertTrue(check_values(match, part_mass))
        # central must NOT take the satellite (Len*PartMass) rule
        ref["Mvir"] = [np.float64(100) * part_mass, sat_ok]
        self.assertTrue(check_values(match, part_mass))

    def test_values_central_negative_mass_falls_back_to_len_partmass(self):
        # A FoF central whose catalog mass is negative (invalid) takes the
        # Len*PartMass branch, matching get_virial_mass's halo_mass >= 0.0 guard.
        part_mass = 0.0325
        conv = np.zeros(
            1,
            dtype=[
                ("Pos", np.float32, (3,)),
                ("Vel", np.float32, (3,)),
                ("Spin", np.float32, (3,)),
                ("VelDisp", np.float32),
                ("Vmax", np.float32),
                ("Len", np.int32),
                ("M_Crit200", np.float32),
                ("MostBoundID", np.int64),
                ("FirstHaloInFOFgroup", np.int32),
            ],
        )
        conv["FirstHaloInFOFgroup"] = [0]  # central (self-referencing)
        conv["Len"] = [11]
        conv["M_Crit200"] = [np.float32(-5.0)]  # invalid catalog mass
        conv["MostBoundID"] = [10]
        ref = np.zeros(1, dtype=GALAXY_DTYPE)
        ref["MostBoundID"] = [10]
        ref["Type"] = [0]
        ref["Len"] = [11]
        match = SnapMatch(0, ref, np.array([0]), conv, np.array([0]))

        ref["Mvir"] = [np.float64(11) * part_mass]  # fallback, not negative M_Crit200*1e-10
        self.assertEqual(check_values(match, part_mass), [])
        ref["Mvir"] = [np.float64(np.float32(-5.0)) * 1e-10]  # the would-be central rule
        self.assertTrue(check_values(match, part_mass))

    def test_values_rejects_nonpositive_part_mass(self):
        ref = np.zeros(0, dtype=GALAXY_DTYPE)
        conv = np.zeros(0, dtype=[("MostBoundID", np.int64)])
        match = SnapMatch(0, ref, np.array([], dtype=np.int64), conv, np.array([], dtype=np.int64))
        with self.assertRaisesRegex(ConverterError, "particle mass must be positive"):
            check_values(match, 0.0)

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
        # rejected in build_match BEFORE any magnitude is taken, so the
        # overflowed value can never influence matching (defense-in-depth: the
        # reference-sanity check carries the same rejection)
        g = self._copy()
        i = find_row(g[5], 1010)
        g[5]["MostBoundID"][i] = np.iinfo(np.int64).min
        directory = self._write_ref(g)
        with self.assertRaisesRegex(ConverterError, "INT64_MIN"):
            run_crosscheck(self.hdf5_dir, directory, self.a_list_path, self.sim_info)

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
            run_crosscheck(corrupted, self.reference_dir, self.a_list_path, self.sim_info)

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
            run_crosscheck(self.hdf5_dir, directory, self.a_list_path, self.sim_info)

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
            run_crosscheck(self.hdf5_dir, directory, self.a_list_path, self.sim_info)

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
            run_crosscheck(self.hdf5_dir, directory, self.a_list_path, self.sim_info)

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
                "--simulation-info",
                str(self.sim_info),
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
                "--simulation-info",
                str(self.sim_info),
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


class TestTopologyChains(unittest.TestCase):
    """The optional seventh check: comparing an independent reference-topology
    dump against the converter's own link fields by stable ctrees id."""

    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        root = Path(cls.tmp.name)
        cls.workdir, cls.a_list_path, cls.sim_info, cls.hdf5_dir = make_written_workdir(root)
        cls.n_snapshots = 6
        _, cls.arrays = validate.load_dataset(cls.hdf5_dir, cls.n_snapshots)
        pristine = build_mock_galaxies(cls.hdf5_dir, cls.n_snapshots, cls.sim_info)
        cls.reference_dir = Path(tempfile.mkdtemp(dir=cls.tmp.name)) / "reference"
        write_mock_reference(pristine, cls.reference_dir, n_snapshots=cls.n_snapshots)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    # -- helpers -------------------------------------------------------------

    def _snapshots(self, directory=None):
        return validate._Snapshots(directory or self.hdf5_dir, self.n_snapshots)

    def _dump_rows(self):
        """Transcribe the converter's own link fields into dump rows: a
        faithful independent reader reading the same source data would
        produce exactly this, so this is the pristine-passes fixture; tests
        below mutate a copy to inject a disagreement."""
        NA = crosscheck._INT64_MIN

        def resolve(target_snap, local_index):
            if local_index < 0:
                return NA
            return int(self.arrays[target_snap]["MostBoundID"][local_index])

        rows = []
        for snap in range(self.n_snapshots):
            conv = self.arrays[snap]
            for i in range(conv["MostBoundID"].size):
                rows.append(
                    [
                        int(conv["ForestIndex"][i]),
                        int(conv["HaloRankInForest"][i]),
                        int(conv["MostBoundID"][i]),
                        snap,
                        resolve(snap + 1, int(conv["Descendant"][i])),
                        resolve(snap - 1, int(conv["FirstProgenitor"][i])),
                        resolve(snap, int(conv["NextProgenitor"][i])),
                        resolve(snap, int(conv["FirstHaloInFOFgroup"][i])),
                        resolve(snap, int(conv["NextHaloInFOFgroup"][i])),
                    ]
                )
        return rows

    def _write_dump_file(self, rows, path):
        lines = [
            crosscheck._TOPOLOGY_DUMP_HEADER,
            "# forestnr rank id snapnum desc_id first_prog_id next_prog_id first_fof_id "
            "next_fof_id",
            "# NA sentinel = {} (no link)".format(crosscheck._INT64_MIN),
        ]
        lines.extend(" ".join(str(v) for v in row) for row in rows)
        Path(path).write_text("\n".join(lines) + "\n")

    def _dump_path(self, rows):
        path = Path(tempfile.mkdtemp(dir=self.tmp.name)) / "topology.dump"
        self._write_dump_file(rows, path)
        return path

    def _partition(self, rows):
        """A partitioned dump the caller must close; every test that opens one
        does so through a ``with`` block."""
        return crosscheck.TopologyDumpPartition(self._dump_path(rows), self.n_snapshots)

    def _check(self, rows, directory=None):
        """Run the check the way ``run_crosscheck`` does: the dump written to a
        file, parsed and partitioned, and the converter side read one snapshot
        at a time. This calls the check on its own, as the whole-dump helper it
        replaced did; the run_crosscheck path that re-asserts the converter's
        |MostBoundID| order first is covered by
        test_run_crosscheck_with_dump_end_to_end."""
        with self._partition(rows) as partition:
            return crosscheck.check_topology_chains(self._snapshots(directory), partition)

    # -- pristine and CLI wiring ----------------------------------------------

    def test_pristine_dump_passes(self):
        self.assertEqual(self._check(self._dump_rows()), [])

    def test_run_crosscheck_without_dump_omits_check(self):
        outcomes = outcome_map(
            run_crosscheck(
                self.hdf5_dir, self.reference_dir, self.a_list_path, self.sim_info, multiplier=M
            )
        )
        self.assertNotIn("topology-chains", outcomes)

    def test_run_crosscheck_with_dump_end_to_end(self):
        dump_path = Path(tempfile.mkdtemp(dir=self.tmp.name)) / "topology.dump"
        self._write_dump_file(self._dump_rows(), dump_path)
        outcomes = outcome_map(
            run_crosscheck(
                self.hdf5_dir,
                self.reference_dir,
                self.a_list_path,
                self.sim_info,
                multiplier=M,
                topology_dump_path=dump_path,
            )
        )
        self.assertIn("topology-chains", outcomes)
        self.assertEqual(
            outcomes["topology-chains"].status, "PASS", outcomes["topology-chains"].line()
        )

    # -- injected violations --------------------------------------------------

    def _wrong_value_for(self, rows, row_index, column):
        """Any halo id that differs from both the row's own id and its
        current (correct) value in ``column`` — guaranteed to be a genuine
        mismatch regardless of the fixture's actual topology."""
        own_id = rows[row_index][2]
        current = rows[row_index][column]
        return next(row[2] for row in rows if row[2] not in (own_id, current))

    def test_wrong_first_progenitor_fails(self):
        rows = self._dump_rows()
        rows[0][5] = self._wrong_value_for(rows, 0, 5)  # first_prog_id column
        failures = self._check(rows)
        self.assertTrue(failures)
        self.assertTrue(any("FirstProgenitor" in f for f in failures), failures)

    def test_wrong_next_fof_fails(self):
        rows = self._dump_rows()
        rows[0][8] = self._wrong_value_for(rows, 0, 8)  # next_fof_id column
        failures = self._check(rows)
        self.assertTrue(failures)
        self.assertTrue(any("NextHaloInFOFgroup" in f for f in failures), failures)

    def test_wrong_next_progenitor_fails(self):
        rows = self._dump_rows()
        rows[0][6] = self._wrong_value_for(rows, 0, 6)  # next_prog_id column
        failures = self._check(rows)
        self.assertTrue(failures)
        self.assertTrue(any("NextProgenitor" in f for f in failures), failures)

    def test_wrong_first_fof_fails(self):
        rows = self._dump_rows()
        rows[0][7] = self._wrong_value_for(rows, 0, 7)  # first_fof_id column
        failures = self._check(rows)
        self.assertTrue(failures)
        self.assertTrue(any("FirstHaloInFOFgroup" in f for f in failures), failures)

    def test_wrong_descendant_fails(self):
        rows = self._dump_rows()
        rows[0][4] = self._wrong_value_for(rows, 0, 4)  # desc_id column
        failures = self._check(rows)
        self.assertTrue(failures)
        self.assertTrue(any("Descendant" in f for f in failures), failures)

    def test_unmatched_reference_id_fails(self):
        rows = self._dump_rows()
        rows[0][2] = 987654321  # an id not present anywhere in the converter dataset
        failures = self._check(rows)
        self.assertTrue(any("no matching converter halo" in f for f in failures), failures)

    def test_out_of_range_snapshot_fails(self):
        rows = self._dump_rows()
        rows[0][3] = self.n_snapshots  # one past the dataset
        failures = self._check(rows)
        self.assertTrue(any("outside the dataset" in f for f in failures), failures)

    def test_converter_out_of_range_link_target_fails(self):
        """A malformed converter link index (in range for its own snapshot's
        dtype, but past that snapshot's actual halo count) must be reported
        as a graded failure, not raise an uncaught IndexError. Uses
        FirstHaloInFOFgroup on row 0 (always a same-snapshot, non-NA self- or
        central-index) so no search for a populated link is needed; row 0's
        snapshot is looked up rather than assumed 0, since snapshot 0 may be
        empty in this fixture."""
        import shutil

        import h5py

        rows = self._dump_rows()
        snap0 = rows[0][3]
        corrupted = Path(tempfile.mkdtemp(dir=self.tmp.name)) / "hdf5"
        shutil.copytree(self.hdf5_dir, corrupted)
        with h5py.File(corrupted / "snapshot_{:03d}.h5".format(snap0), "r+") as handle:
            dataset = handle["halos"]["FirstHaloInFOFgroup"]
            dataset[0] = dataset.shape[0] + 100
        failures = self._check(rows, directory=corrupted)
        self.assertTrue(any("outside snapshot" in f for f in failures), failures)

    # -- coverage: the dump must name every converter halo exactly once --------

    def test_truncated_dump_fails_coverage(self):
        """A dump missing even one halo must fail, not silently compare over
        the subset it happens to contain: a killed harness run or a full disk
        is the likeliest real failure, and it must never read as conformance."""
        rows = self._dump_rows()
        dropped_snap = rows[0][3]
        rows.pop(0)
        failures = self._check(rows)
        self.assertTrue(any("every converter halo exactly once" in f for f in failures), failures)
        self.assertTrue(any("snapshot {}:".format(dropped_snap) in f for f in failures), failures)

    def test_empty_dump_fails_coverage(self):
        """A header-only dump parses fine (that is the loader's job) but must
        fail the check against a non-empty dataset — the vacuous-pass case."""
        failures = self._check([])
        self.assertTrue(any("every converter halo exactly once" in f for f in failures), failures)

    def test_duplicate_dump_id_fails(self):
        """Matching is by |MostBoundID|, so a duplicated id would let one
        dumped halo stand in for another while keeping the coverage count
        balanced; duplicates must be rejected outright."""
        rows = self._dump_rows()
        by_snap: Dict[int, List[int]] = {}
        for i, row in enumerate(rows):
            by_snap.setdefault(row[3], []).append(i)
        crowded = [indices for indices in by_snap.values() if len(indices) >= 2]
        self.assertTrue(crowded, "fixture must have a snapshot with two halos to duplicate")
        first, second = crowded[0][0], crowded[0][1]
        rows[second][2] = rows[first][2]
        failures = self._check(rows)
        self.assertTrue(any("duplicate |MostBoundID|" in f for f in failures), failures)

    # -- identity and sign, compared for every halo ---------------------------

    def test_wrong_forest_index_fails(self):
        rows = self._dump_rows()
        rows[0][0] += 1  # forestnr column
        failures = self._check(rows)
        self.assertTrue(any("mismatched ForestIndex" in f for f in failures), failures)

    def test_wrong_rank_fails(self):
        """Rank is compared for EVERY dumped halo here, not only the
        first-appearance subset that identity-creation covers."""
        rows = self._dump_rows()
        rows[0][1] += 1  # rank column
        failures = self._check(rows)
        self.assertTrue(any("mismatched HaloRankInForest" in f for f in failures), failures)

    def test_own_mostboundid_sign_mismatch_fails(self):
        """The halo's own signed id must agree, not just its magnitude: the
        flyby-signs check only compares signs over the matched Type 0/1
        population, so galaxy-less demoted halos rely on this comparison."""
        rows = self._dump_rows()
        self.assertNotEqual(rows[0][2], 0, "negating a zero id would not be a mismatch")
        rows[0][2] = -rows[0][2]
        failures = self._check(rows)
        self.assertTrue(any("MostBoundID sign mismatch" in f for f in failures), failures)

    # -- dump parsing and partitioning -----------------------------------------

    def test_dump_partition_roundtrip(self):
        """Every dumped row must survive the parse and land in its own
        snapshot's partition, in dump order."""
        rows = self._dump_rows()
        expected = np.array([tuple(row) for row in rows], dtype=crosscheck._TOPOLOGY_DUMP_DTYPE)
        with self._partition(rows) as partition:
            self.assertEqual(partition.total_rows, len(rows))
            self.assertEqual(partition.out_of_range, {})
            recovered = [partition.rows(snap) for snap in range(self.n_snapshots)]
        recovered = np.concatenate(recovered)
        # the dump is written snapshot-major, so concatenating the partitions
        # in snapshot order reproduces it exactly
        self.assertEqual(recovered.dtype, expected.dtype)
        np.testing.assert_array_equal(recovered, expected)

    def test_dump_partition_spans_blocks(self):
        """The parse is blocked, so the fixture must cross a block boundary:
        with a one-row block every row is its own parse, and the partition must
        still hold every row in dump order."""
        rows = self._dump_rows()
        self.assertGreater(len(rows), 1, "the fixture must have rows to block")
        with mock.patch.object(crosscheck, "_DUMP_BLOCK_ROWS", 1):
            with self._partition(rows) as blocked:
                blocked_rows = [blocked.rows(snap) for snap in range(self.n_snapshots)]
                blocked_counts = list(blocked.counts)
        with self._partition(rows) as whole:
            whole_rows = [whole.rows(snap) for snap in range(self.n_snapshots)]
            whole_counts = list(whole.counts)
        self.assertEqual(blocked_counts, whole_counts)
        for blocked_snap, whole_snap in zip(blocked_rows, whole_rows):
            np.testing.assert_array_equal(blocked_snap, whole_snap)

    def test_dump_partition_rejects_malformed_row_in_a_later_block(self):
        """A malformed row past the first block must still abort, and the
        message must say what its row number is relative to rather than quietly
        renumbering the dump."""
        rows = self._dump_rows()
        path = self._dump_path(rows)
        with path.open("a") as handle:
            handle.write("1 2 3\n")
        with mock.patch.object(crosscheck, "_DUMP_BLOCK_ROWS", 1):
            with self.assertRaises(ConverterError) as caught:
                crosscheck.TopologyDumpPartition(path, self.n_snapshots)
        self.assertIn("malformed data row", str(caught.exception))
        self.assertIn("relative to the block", str(caught.exception))

    def test_dump_partition_bad_header(self):
        path = Path(tempfile.mkdtemp(dir=self.tmp.name)) / "topology.dump"
        path.write_text("not a dump\nline2\nline3\n")
        with self.assertRaises(ConverterError):
            crosscheck.TopologyDumpPartition(path, self.n_snapshots)

    def test_dump_partition_bad_sentinel(self):
        path = Path(tempfile.mkdtemp(dir=self.tmp.name)) / "topology.dump"
        path.write_text(
            "{}\ncolumn names\n# NA sentinel = 0 (no link)\n".format(
                crosscheck._TOPOLOGY_DUMP_HEADER
            )
        )
        with self.assertRaises(ConverterError):
            crosscheck.TopologyDumpPartition(path, self.n_snapshots)

    def test_dump_partition_empty_parses(self):
        """A header-only dump is a well-formed parse; rejecting it against the
        dataset is check_topology_chains' coverage assertion, not the reader's
        (see test_empty_dump_fails_coverage)."""
        with self._partition([]) as partition:
            self.assertEqual(partition.total_rows, 0)
            self.assertEqual(partition.peak_bytes, 0)

    def test_dump_partition_rejects_stray_comment(self):
        """The format is exactly three header lines then data rows, so a "#"
        line after the header means a malformed dump (two runs concatenated, a
        harness re-run appended with ">>"). Skipping it would silently splice
        unrelated dumps into one comparison."""
        rows = self._dump_rows()
        path = self._dump_path(rows)
        with path.open("a") as handle:
            handle.write(crosscheck._TOPOLOGY_DUMP_HEADER + "\n")
            handle.write(" ".join(str(v) for v in rows[0]) + "\n")
        with self.assertRaises(ConverterError):
            crosscheck.TopologyDumpPartition(path, self.n_snapshots)

    def test_dump_partition_rejects_ragged_row(self):
        rows = self._dump_rows()
        path = self._dump_path(rows)
        with path.open("a") as handle:
            handle.write("1 2 3\n")  # too few columns
        with self.assertRaises(ConverterError):
            crosscheck.TopologyDumpPartition(path, self.n_snapshots)

    def test_dump_partition_rejects_non_integer(self):
        rows = self._dump_rows()
        path = self._dump_path(rows)
        with path.open("a") as handle:
            handle.write(" ".join(["nope"] * len(crosscheck._TOPOLOGY_DUMP_DTYPE)) + "\n")
        with self.assertRaises(ConverterError):
            crosscheck.TopologyDumpPartition(path, self.n_snapshots)

    # -- partition lifetime ----------------------------------------------------

    def test_partition_directory_is_removed_on_success(self):
        rows = self._dump_rows()
        with self._partition(rows) as partition:
            directory = partition._dir
            self.assertTrue(directory.is_dir())
            self.assertGreater(partition.peak_bytes, 0)
        self.assertFalse(directory.exists())

    def test_partition_directory_is_removed_when_the_check_raises(self):
        rows = self._dump_rows()
        held = {}
        with self.assertRaisesRegex(RuntimeError, "injected"):
            with self._partition(rows) as partition:
                held["dir"] = partition._dir
                raise RuntimeError("injected")
        self.assertFalse(held["dir"].exists())

    def test_partition_directory_is_removed_when_the_parse_raises(self):
        """The constructor owns the directory until it returns, so a malformed
        dump must not leave one behind — there is no ``with`` to fall back on."""
        rows = self._dump_rows()
        path = self._dump_path(rows)
        with path.open("a") as handle:
            handle.write("1 2 3\n")
        before = set(Path(tempfile.gettempdir()).glob(crosscheck.TOPOLOGY_DIR_PREFIX + "*"))
        with self.assertRaises(ConverterError):
            crosscheck.TopologyDumpPartition(path, self.n_snapshots)
        after = set(Path(tempfile.gettempdir()).glob(crosscheck.TOPOLOGY_DIR_PREFIX + "*"))
        self.assertEqual(after - before, set())


# ---------------------------------------------------------------------------
# Bounded global state: the suppression set and the dump partition
# ---------------------------------------------------------------------------


class TestSeenIdentities(unittest.TestCase):
    """The suppression set replaces an in-memory ``np.union1d`` array with a
    disk-backed sorted union, so it must answer EXACTLY what ``np.isin``
    against that array answered, at any block size, and leave nothing behind."""

    def _reference(self, snapshots):
        """What the whole-dataset formulation computed: ~isin against the
        union of every earlier snapshot's identities."""
        seen = np.empty(0, dtype=np.int64)
        expected = []
        for ugid in snapshots:
            expected.append(~np.isin(ugid, seen))
            seen = np.union1d(seen, ugid)
        return expected, seen

    def _run(self, snapshots, block_rows):
        with mock.patch.object(crosscheck, "_SEEN_BLOCK_ROWS", block_rows):
            with crosscheck._SeenIdentities() as seen:
                masks = [seen.first_appearance(ugid) for ugid in snapshots]
                store = np.fromfile(seen._path, dtype=np.int64)
        return masks, store

    def test_matches_isin_and_union_at_every_block_size(self):
        rng = np.random.default_rng(20260828)
        snapshots = [
            rng.integers(0, 40, size=rng.integers(0, 30), dtype=np.int64) for _ in range(12)
        ]
        # identities that persist across snapshots, and a snapshot that repeats
        # one within itself, are the cases the suppression rule turns on
        snapshots[3] = np.array([7, 7, 11, 11, 11], dtype=np.int64)
        snapshots[4] = np.array([11, 7, 999], dtype=np.int64)
        snapshots[7] = np.empty(0, dtype=np.int64)
        expected_masks, expected_store = self._reference(snapshots)
        for block_rows in (1, 2, 3, 7, 1 << 20):
            masks, store = self._run(snapshots, block_rows)
            for snap, (got, want) in enumerate(zip(masks, expected_masks)):
                np.testing.assert_array_equal(
                    got, want, "block {} snapshot {}".format(block_rows, snap)
                )
            np.testing.assert_array_equal(store, expected_store, "block {}".format(block_rows))

    def test_duplicates_within_one_call_are_all_unseen(self):
        # the store is updated only once the mask is complete, so a repeated
        # identity does not suppress its own siblings at the same snapshot
        with crosscheck._SeenIdentities() as seen:
            first = seen.first_appearance(np.array([5, 5, 5], dtype=np.int64))
            second = seen.first_appearance(np.array([5, 6], dtype=np.int64))
        np.testing.assert_array_equal(first, [True, True, True])
        np.testing.assert_array_equal(second, [False, True])

    def test_mask_follows_the_callers_order(self):
        with crosscheck._SeenIdentities() as seen:
            seen.first_appearance(np.array([10, 30], dtype=np.int64))
            mask = seen.first_appearance(np.array([30, 20, 10, 40], dtype=np.int64))
        np.testing.assert_array_equal(mask, [False, True, False, True])

    def test_peak_bytes_is_recorded(self):
        with crosscheck._SeenIdentities() as seen:
            self.assertEqual(seen.peak_bytes, 0)
            seen.first_appearance(np.arange(100, dtype=np.int64))
            # the store and its successor coexist while a merge is in flight
            self.assertGreaterEqual(seen.peak_bytes, 100 * 8)

    def test_store_is_removed_on_exit(self):
        with crosscheck._SeenIdentities() as seen:
            directory = seen._dir
            seen.first_appearance(np.arange(4, dtype=np.int64))
            self.assertTrue(directory.is_dir())
        self.assertFalse(directory.exists())

    def test_store_is_removed_when_the_body_raises(self):
        held = {}
        with self.assertRaisesRegex(RuntimeError, "injected"):
            with crosscheck._SeenIdentities() as seen:
                held["dir"] = seen._dir
                raise RuntimeError("injected")
        self.assertFalse(held["dir"].exists())


class TestIdentityCreationSuppression(unittest.TestCase):
    """A galaxy that persists across snapshots keeps its UniqueGalaxyID, so
    identity-creation must decode it ONCE, at its first appearance, and never
    again — the whole point of the suppression set."""

    CONV_DTYPE = [
        ("MostBoundID", np.int64),
        ("ForestIndex", np.int64),
        ("HaloRankInForest", np.int64),
    ]

    def _match(self, snap, ugid, forest, rank, matched=0):
        """One galaxy at ``snap`` carrying ``ugid``, matched to a halo whose
        identity is (``forest``, ``rank``)."""
        conv = np.zeros(1, dtype=self.CONV_DTYPE)
        conv["MostBoundID"] = [10]
        conv["ForestIndex"] = [forest]
        conv["HaloRankInForest"] = [rank]
        ref = np.zeros(1, dtype=[("UniqueGalaxyID", np.int64), ("MostBoundID", np.int64)])
        ref["UniqueGalaxyID"] = [ugid]
        ref["MostBoundID"] = [10]
        return SnapMatch(snap, ref, np.array([0]), conv, np.array([matched]))

    def test_persisting_galaxy_is_decoded_once_and_never_again(self):
        # created at snapshot 1 on the halo its id encodes; at snapshots 2-4 the
        # same galaxy sits on halos whose rank has advanced, which would fail
        # the decode if it were re-checked
        ugid = 3 * M + 7
        with crosscheck._SeenIdentities() as seen:
            failures = []
            failures += check_identity_creation(self._match(1, ugid, 2, 7), M, seen)
            for snap, rank in ((2, 8), (3, 9), (4, 10)):
                failures += check_identity_creation(self._match(snap, ugid, 2, rank), M, seen)
        self.assertEqual(failures, [])

    def test_the_same_galaxy_appearing_first_at_a_later_snapshot_is_decoded(self):
        # the mutation proof for the test above: without the earlier
        # appearance, snapshot 2's mismatched rank IS a failure, so suppression
        # is what silences it rather than the check being blind
        with crosscheck._SeenIdentities() as seen:
            failures = check_identity_creation(self._match(2, 3 * M + 7, 2, 8), M, seen)
        self.assertEqual(len(failures), 1)
        self.assertIn("snapshot 2", failures[0])

    def test_an_unmatched_first_appearance_still_suppresses_later_snapshots(self):
        # the whole-dataset formulation folded EVERY Type 0/1 identity into
        # ``seen``, matched or not, so an unmatched galaxy at snapshot 1
        # suppresses the same identity at snapshot 2
        ugid = 3 * M + 7
        with crosscheck._SeenIdentities() as seen:
            unmatched = self._match(1, ugid, 2, 7, matched=-1)
            self.assertEqual(check_identity_creation(unmatched, M, seen), [])
            failures = check_identity_creation(self._match(2, ugid, 2, 8), M, seen)
        self.assertEqual(failures, [])


# ---------------------------------------------------------------------------
# Bounded memory, on the synthetic dataset the battery's memory tests use
# ---------------------------------------------------------------------------


def build_synthetic_reference(n_snapshots: int, n_halos: int, multiplier: int = M):
    """Reference galaxies for a ``write_synthetic_dataset`` dataset.

    Every halo there is its own FoF central and descends into the same row of
    the next snapshot, so every halo is occupied and Type 0, and each forest's
    galaxy is CREATED at snapshot 0 and then PERSISTS: its UniqueGalaxyID keeps
    the rank-0 encoding while its halo's HaloRankInForest advances with the
    snapshot. That makes this fixture a suppression fixture as well as a memory
    one — identity-creation passes on it only if each galaxy is decoded once,
    at snapshot 0, and never again.
    """
    ids = np.arange(1, n_halos + 1, dtype=np.int64)
    ugid = multiplier * (np.arange(n_halos, dtype=np.int64) + 1)
    galaxies = {}
    for snap in range(n_snapshots):
        rows = np.zeros(n_halos, dtype=GALAXY_DTYPE)
        rows["SnapNum"] = snap
        rows["Type"] = 0
        rows["UniqueGalaxyID"] = ugid
        rows["UniqueCentralGalaxyID"] = ugid
        rows["Len"] = 100  # write_synthetic_dataset's Len
        rows["Mvir"] = 0.0  # float64(M_Crit200 = 0) * NATIVE_TO_REF_MASS
        rows["MostBoundID"] = ids
        galaxies[snap] = rows
    return galaxies


def write_synthetic_dump(path, n_snapshots: int, n_halos: int) -> Path:
    """The reference-topology dump a faithful independent reader would produce
    for a ``write_synthetic_dataset`` dataset."""
    path = Path(path)
    na = crosscheck._INT64_MIN
    ids = np.arange(1, n_halos + 1, dtype=np.int64)
    block = np.zeros((n_halos, 9), dtype=np.int64)
    block[:, 0] = np.arange(n_halos, dtype=np.int64)  # ForestIndex
    block[:, 2] = ids
    block[:, 6] = na  # NextProgenitor
    block[:, 7] = ids  # FirstHaloInFOFgroup: every halo is its own central
    block[:, 8] = na  # NextHaloInFOFgroup
    with open(path, "w") as handle:
        handle.write(crosscheck._TOPOLOGY_DUMP_HEADER + "\n")
        handle.write(
            "# forestnr rank id snapnum desc_id first_prog_id next_prog_id first_fof_id "
            "next_fof_id\n"
        )
        handle.write("# NA sentinel = {} (no link)\n".format(na))
        for snap in range(n_snapshots):
            block[:, 1] = snap  # HaloRankInForest
            block[:, 3] = snap
            block[:, 4] = ids if snap < n_snapshots - 1 else na
            block[:, 5] = ids if snap > 0 else na
            np.savetxt(handle, block, fmt="%d")
    return path


class TestBoundedMemory(unittest.TestCase):
    """Resident bytes must be bounded by the per-snapshot window plus the two
    bounded global structures — NOT by the dataset, the reference output or the
    dump. Measured with tracemalloc, which traces numpy's and h5py's own
    allocations, so this asserts ACTUAL peak allocation rather than any counter
    the cross-check keeps about itself."""

    HALOS_PER_SNAPSHOT = 2048
    SMALL_SNAPSHOTS = 4
    LARGE_SNAPSHOTS = 40

    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        root = Path(cls.tmp.name)
        cls.sim_info = write_simulation_info(root / "simulation_info.yaml")
        cls.small = cls._build(root, "small", cls.SMALL_SNAPSHOTS)
        cls.large = cls._build(root, "large", cls.LARGE_SNAPSHOTS)

    @classmethod
    def _build(cls, root, name, n_snapshots):
        """Dataset, matching reference output and matching dump, all of the
        same shape and differing only in snapshot count."""
        directory = root / name
        a_list_path = write_synthetic_dataset(directory, n_snapshots, cls.HALOS_PER_SNAPSHOT)
        reference_dir = root / "{}-reference".format(name)
        write_mock_reference(
            build_synthetic_reference(n_snapshots, cls.HALOS_PER_SNAPSHOT),
            reference_dir,
            n_snapshots=n_snapshots,
        )
        dump_path = write_synthetic_dump(
            root / "{}.dump".format(name), n_snapshots, cls.HALOS_PER_SNAPSHOT
        )
        return {
            "dir": directory,
            "a_list": a_list_path,
            "reference": reference_dir,
            "dump": dump_path,
            "n_snapshots": n_snapshots,
        }

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def _run(self, fixture):
        return run_crosscheck(
            fixture["dir"],
            fixture["reference"],
            fixture["a_list"],
            self.sim_info,
            multiplier=M,
            topology_dump_path=fixture["dump"],
        )

    def _peak_bytes(self, fixture):
        tracemalloc.start()
        try:
            tracemalloc.reset_peak()
            outcomes = self._run(fixture)
            peak = tracemalloc.get_traced_memory()[1]
        finally:
            tracemalloc.stop()
        return peak, outcomes

    @staticmethod
    def _bytes(fixture):
        return (
            sum(path.stat().st_size for path in Path(fixture["dir"]).glob("*.h5"))
            + sum(path.stat().st_size for path in Path(fixture["reference"]).glob("*.hdf5"))
            + Path(fixture["dump"]).stat().st_size
        )

    def test_synthetic_fixtures_are_conformant(self):
        # the memory numbers below only mean anything if every check actually
        # ran to completion on both fixtures; identity-creation passing here is
        # itself the suppression proof at fixture scale, since every galaxy
        # persists onto a halo whose rank has advanced
        for fixture in (self.small, self.large):
            outcomes = self._run(fixture)
            self.assertEqual(len(outcomes), 8)
            failed = [o.line() for o in outcomes if o.status != "PASS"]
            self.assertEqual(failed, [], str(fixture["dir"]))

    def test_peak_allocation_does_not_scale_with_the_dataset(self):
        # Both runs must fill the same fixed buffers, or the comparison
        # measures how full a buffer got rather than what stayed resident: at
        # the shipped block sizes the small fixture never fills one dump-parse
        # block while the large one fills several, which is a difference in the
        # BUFFER, not in the dataset. Small blocks make both runs fill many,
        # leaving dataset-sized residency as the only thing that could differ.
        # The shipped constants are asserted by the declared-bound test below.
        with mock.patch.object(crosscheck, "_DUMP_BLOCK_ROWS", 512), mock.patch.object(
            crosscheck, "_SEEN_BLOCK_ROWS", 512
        ):
            # warm up: first-call imports and h5py's own caches are not the subject
            self._peak_bytes(self.small)
            small_peak, _ = self._peak_bytes(self.small)
            large_peak, _ = self._peak_bytes(self.large)

        grew_by = self._bytes(self.large) - self._bytes(self.small)
        self.assertGreater(grew_by, 10 * 1024**2, "the two fixtures must differ materially")

        extra_snapshots = self.LARGE_SNAPSHOTS - self.SMALL_SNAPSHOTS
        extra_halos = extra_snapshots * self.HALOS_PER_SNAPSHOT
        # what legitimately grows with the SNAPSHOT count: one open partition
        # writer per snapshot while the dump is parsed, the cached halo counts,
        # and the per-snapshot row counts
        allowance = extra_snapshots * 32 * 1024 + 512 * 1024
        self.assertLess(large_peak - small_peak, allowance)
        # and the allowance is far below what even ONE whole-dataset-resident
        # column would have cost, let alone the three whole-dataset arrays the
        # formulation this replaced held
        self.assertLess(allowance, extra_halos * 100)

    def test_peak_allocation_is_within_the_declared_bound(self):
        peak, _ = self._peak_bytes(self.large)
        # the window: one snapshot's converter arrays, that snapshot's
        # reference galaxies, its dump rows and the comparison temporaries.
        # 512 bytes per halo is a generous ceiling on all of them together
        window = 4 * self.HALOS_PER_SNAPSHOT * 512
        # the dump parse holds one block of text lines and its parsed rows
        parse_buffer = crosscheck._DUMP_BLOCK_ROWS * 512
        # the suppression merge holds one block of the store, the slice of the
        # window merged into it, and the merged output
        merge_buffer = 3 * crosscheck._SEEN_BLOCK_ROWS * 8
        bound = window + parse_buffer + merge_buffer + 8 * 1024**2
        self.assertLess(peak, bound)

    def test_on_disk_structures_are_reported_and_removed(self):
        temp_root = Path(tempfile.gettempdir())
        before = set(temp_root.glob("crosscheck_*"))
        with mock.patch.object(crosscheck, "_log") as logged:
            outcomes = self._run(self.large)
        self.assertEqual(len(outcomes), 8)
        lines = [call.args[0] for call in logged.call_args_list]
        self.assertTrue(any("identity suppression set peaked at" in line for line in lines), lines)
        self.assertTrue(any("topology dump partition held" in line for line in lines), lines)
        self.assertEqual(set(temp_root.glob("crosscheck_*")) - before, set())

    def test_the_on_disk_structures_are_removed_when_a_run_aborts(self):
        temp_root = Path(tempfile.gettempdir())
        before = set(temp_root.glob("crosscheck_*"))
        with self.assertRaises(ConverterError):
            run_crosscheck(
                self.large["dir"],
                self.large["reference"],
                self.large["a_list"],
                self.sim_info,
                multiplier=M,
                # a dump whose header is wrong aborts inside the partition's
                # own constructor, after the seven checks have run
                topology_dump_path=self.large["a_list"],
            )
        self.assertEqual(set(temp_root.glob("crosscheck_*")) - before, set())

    def test_truncated_dump_fails_end_to_end(self):
        truncated = Path(self.tmp.name) / "truncated.dump"
        lines = Path(self.large["dump"]).read_text().splitlines()
        truncated.write_text("\n".join(lines[:-1]) + "\n")
        outcomes = outcome_map(
            run_crosscheck(
                self.large["dir"],
                self.large["reference"],
                self.large["a_list"],
                self.sim_info,
                multiplier=M,
                topology_dump_path=truncated,
            )
        )
        self.assertEqual(outcomes["topology-chains"].status, "FAIL")
        self.assertIn("every converter halo exactly once", outcomes["topology-chains"].detail)

    def test_wrong_multiplier_fails_end_to_end(self):
        outcomes = outcome_map(
            run_crosscheck(
                self.large["dir"],
                self.large["reference"],
                self.large["a_list"],
                self.sim_info,
                multiplier=M // 10,
            )
        )
        self.assertEqual(outcomes["identity-forest"].status, "FAIL")
        self.assertEqual(outcomes["identity-creation"].status, "FAIL")


if __name__ == "__main__":
    unittest.main()
