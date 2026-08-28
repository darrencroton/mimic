"""Slice 7 unit tests: snapshot-HDF5 emission against the frozen contract
(docs/dev/SNAPSHOT-HDF5-FORMAT.md), the forests.h5 sidecar, writer resume/refuse
semantics, and the conversion report."""

import os
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

import h5py
import numpy as np

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import convert_ctrees  # noqa: E402
import fixtures  # noqa: E402
from ctrees_parser import ConverterError  # noqa: E402
from fixups import FIXED_RECORD_DTYPE, run_fixups  # noqa: E402
from hdf5_writer import (  # noqa: E402
    CHUNK_1D,
    CHUNK_VEC,
    FORMAT_VERSION,
    HALO_DATASETS,
    HEADER_ATTRS,
    build_halo_arrays,
    load_header_metadata,
    run_write,
    snapshot_h5_name,
)
from links import LINKS_RECORD_DTYPE, run_links  # noqa: E402
from report import (  # noqa: E402
    build_report,
    identity_multiplier_window,
    recommended_multiplier,
    run_report,
)
from scatter import Manifest, run_scatter  # noqa: E402
from sort_index import run_sort  # noqa: E402
from test_fixups import capture_stderr  # noqa: E402
from test_links import GOLDEN_LINKS, make_linked_workdir  # noqa: E402
from validate import run_battery  # noqa: E402


def make_written_workdir(root: Path):
    """Full fixture pipeline scatter -> sort -> fixups -> links -> write;
    returns (workdir, a_list_path, sim_info_path, hdf5_dir)."""
    workdir, a_list, sim_info = make_linked_workdir(root)
    run_links(workdir)
    manifest = run_write(workdir, a_list_path=a_list, simulation_info_path=sim_info)
    return workdir, a_list, sim_info, Path(manifest.data["outputs_dir"])


class TestHeaderMetadata(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.addCleanup(self.tmp.cleanup)

    def test_loads_and_converts_particle_mass(self):
        path = fixtures.write_simulation_info(self.root / "simulation_info.yaml")
        metadata = load_header_metadata(path)
        self.assertEqual(metadata["particle_mass_msun_h"], 0.0325 * 1e10)
        self.assertEqual(metadata["box_size_mpc_h"], 100.0)
        self.assertEqual(metadata["omega_matter"], 0.3089)
        self.assertEqual(metadata["omega_lambda"], 0.6911)
        self.assertEqual(metadata["hubble_h"], 0.6774)

    def _write_info(self, text: str) -> Path:
        path = self.root / "info.yaml"
        path.write_text(text)
        return path

    def test_wrong_box_units_abort(self):
        path = self._write_info(
            "simulation:\n"
            "  cosmology: {omega_matter: 0.3, omega_lambda: 0.7, hubble_h: 0.7}\n"
            "  box_size: {value: 100.0, units: kpc/h}\n"
            "  particle_mass: {value: 0.0325, units: 1e10 Msun/h}\n"
        )
        with self.assertRaisesRegex(ConverterError, "box_size units"):
            load_header_metadata(path)

    def test_wrong_particle_mass_units_abort(self):
        path = self._write_info(
            "simulation:\n"
            "  cosmology: {omega_matter: 0.3, omega_lambda: 0.7, hubble_h: 0.7}\n"
            "  box_size: {value: 100.0, units: Mpc/h}\n"
            "  particle_mass: {value: 3.25e8, units: Msun/h}\n"
        )
        with self.assertRaisesRegex(ConverterError, "particle_mass units"):
            load_header_metadata(path)

    def test_missing_cosmology_aborts(self):
        path = self._write_info(
            "simulation:\n"
            "  box_size: {value: 100.0, units: Mpc/h}\n"
            "  particle_mass: {value: 0.0325, units: 1e10 Msun/h}\n"
        )
        with self.assertRaisesRegex(ConverterError, "malformed simulation metadata"):
            load_header_metadata(path)


class TestBuildHaloArrays(unittest.TestCase):
    def _fixed(self, mostboundids):
        fixed = np.zeros(len(mostboundids), dtype=FIXED_RECORD_DTYPE)
        fixed["MostBoundID"] = mostboundids
        fixed["id"] = np.abs(np.asarray(mostboundids, dtype=np.int64))
        return fixed

    def test_row_misalignment_aborts(self):
        fixed = self._fixed([10, 20])
        links = np.zeros(1, dtype=LINKS_RECORD_DTYPE)
        with self.assertRaisesRegex(ConverterError, "row alignment"):
            build_halo_arrays(fixed, links, 5, "test")

    def test_slab_order_violation_aborts(self):
        fixed = self._fixed([20, 10])
        links = np.zeros(2, dtype=LINKS_RECORD_DTYPE)
        with self.assertRaisesRegex(ConverterError, "ascending in \\|MostBoundID\\|"):
            build_halo_arrays(fixed, links, 5, "test")

    def test_negated_ids_keep_slab_order(self):
        fixed = self._fixed([10, -20, 30])
        links = np.zeros(3, dtype=LINKS_RECORD_DTYPE)
        arrays = build_halo_arrays(fixed, links, 5, "test")
        self.assertEqual(arrays["MostBoundID"].tolist(), [10, -20, 30])

    def test_int64_min_mostboundid_aborts(self):
        fixed = np.zeros(1, dtype=FIXED_RECORD_DTYPE)
        fixed["MostBoundID"] = np.iinfo(np.int64).min
        links = np.zeros(1, dtype=LINKS_RECORD_DTYPE)
        with self.assertRaisesRegex(ConverterError, "INT64_MIN"):
            build_halo_arrays(fixed, links, 5, "test")


class TestEmission(unittest.TestCase):
    """Contract conformance of the emitted fixture dataset (read-only tests
    over one shared pipeline run)."""

    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        root = Path(cls.tmp.name)
        cls.workdir, cls.a_list_path, cls.sim_info, cls.hdf5_dir = make_written_workdir(root)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def test_file_set(self):
        names = sorted(p.name for p in self.hdf5_dir.glob("*.h5"))
        expected = sorted(
            [snapshot_h5_name(snap) for snap in range(len(fixtures.A_LIST))] + ["forests.h5"]
        )
        self.assertEqual(names, expected)

    def test_exact_object_set_dtypes_chunks_compression(self):
        for snap in range(len(fixtures.A_LIST)):
            with h5py.File(self.hdf5_dir / snapshot_h5_name(snap), "r") as handle:
                self.assertEqual(set(handle.keys()), {"header", "halos"})
                self.assertEqual(set(handle["header"].attrs.keys()), set(HEADER_ATTRS))
                for name, dtype in HEADER_ATTRS.items():
                    value = np.asarray(handle["header"].attrs[name])
                    self.assertEqual(value.dtype, np.dtype(dtype), name)
                    self.assertEqual(value.shape, (), name)
                self.assertEqual(set(handle["halos"].keys()), set(HALO_DATASETS))
                n_halos = int(handle["header"].attrs["n_halos"])
                for name, (dtype, is_vec) in HALO_DATASETS.items():
                    dataset = handle["halos"][name]
                    self.assertEqual(dataset.dtype, np.dtype(dtype), name)
                    self.assertIsNone(dataset.compression, name)
                    if is_vec:
                        self.assertEqual(dataset.shape, (n_halos, 3), name)
                        self.assertEqual(dataset.chunks, CHUNK_VEC, name)
                    else:
                        self.assertEqual(dataset.shape, (n_halos,), name)
                        self.assertEqual(dataset.chunks, CHUNK_1D, name)

    def test_header_attribute_values(self):
        for snap, scale in enumerate(fixtures.A_LIST):
            with h5py.File(self.hdf5_dir / snapshot_h5_name(snap), "r") as handle:
                attrs = handle["header"].attrs
                self.assertEqual(int(attrs["format_version"]), FORMAT_VERSION)
                self.assertEqual(int(attrs["links_adjacent"]), 1)
                self.assertEqual(int(attrs["snapshot_number"]), snap)
                self.assertEqual(float(attrs["scale_factor"]), scale)
                self.assertEqual(int(attrs["n_forests_total"]), 5)
                self.assertEqual(int(attrs["max_halo_rank_in_forest"]), 5)
                self.assertEqual(float(attrs["box_size_mpc_h"]), 100.0)
                self.assertEqual(float(attrs["particle_mass_msun_h"]), 0.0325 * 1e10)
                self.assertEqual(float(attrs["omega_matter"]), 0.3089)
                self.assertEqual(float(attrs["omega_lambda"]), 0.6911)
                self.assertEqual(float(attrs["hubble_h"]), 0.6774)

    def test_empty_snapshot_file(self):
        with h5py.File(self.hdf5_dir / snapshot_h5_name(0), "r") as handle:
            self.assertEqual(int(handle["header"].attrs["n_halos"]), 0)
            for name, (dtype, is_vec) in HALO_DATASETS.items():
                dataset = handle["halos"][name]
                self.assertEqual(dataset.shape, (0, 3) if is_vec else (0,), name)
                self.assertEqual(dataset.chunks, CHUNK_VEC if is_vec else CHUNK_1D, name)
                self.assertIsNone(dataset.compression, name)

    def test_link_datasets_match_golden(self):
        for snap, golden in GOLDEN_LINKS.items():
            with h5py.File(self.hdf5_dir / snapshot_h5_name(snap), "r") as handle:
                for field in golden:
                    self.assertEqual(
                        handle["halos"][field][...].tolist(),
                        golden[field],
                        "snapshot {} {}".format(snap, field),
                    )

    def test_value_datasets_match_fixed_records(self):
        manifest = Manifest.load_or_create(self.workdir)
        for snap_str, entry in manifest.data["snapshots"].items():
            fixed = np.fromfile(entry["fixed_file"], dtype=FIXED_RECORD_DTYPE)
            with h5py.File(self.hdf5_dir / snapshot_h5_name(int(snap_str)), "r") as handle:
                halos = handle["halos"]
                self.assertEqual(
                    halos["M_Crit200"][...].tobytes(), fixed["Mvir"].tobytes(), snap_str
                )
                self.assertEqual(
                    halos["Pos"][...].tobytes(),
                    np.column_stack((fixed["X"], fixed["Y"], fixed["Z"])).tobytes(),
                )
                self.assertEqual(
                    halos["Vel"][...].tobytes(),
                    np.column_stack((fixed["VX"], fixed["VY"], fixed["VZ"])).tobytes(),
                )
                self.assertEqual(
                    halos["Spin"][...].tobytes(),
                    np.column_stack((fixed["Jx"], fixed["Jy"], fixed["Jz"])).tobytes(),
                )
                self.assertEqual(halos["VelDisp"][...].tobytes(), fixed["vrms"].tobytes())
                self.assertEqual(halos["Vmax"][...].tobytes(), fixed["vmax"].tobytes())
                self.assertEqual(halos["Len"][...].tobytes(), fixed["Len"].tobytes())
                self.assertEqual(
                    halos["MostBoundID"][...].tobytes(), fixed["MostBoundID"].tobytes()
                )
                self.assertTrue((halos["SnapNum"][...] == int(snap_str)).all())

    def test_flyby_sign_emitted(self):
        with h5py.File(self.hdf5_dir / snapshot_h5_name(5), "r") as handle:
            mostbound = handle["halos"]["MostBoundID"][...]
        self.assertIn(-1020, mostbound.tolist())

    def test_forests_sidecar(self):
        with h5py.File(self.hdf5_dir / "forests.h5", "r") as handle:
            self.assertEqual(set(handle.keys()), {"ForestID"})
            dataset = handle["ForestID"]
            self.assertEqual(dataset.dtype, np.dtype(np.int64))
            self.assertEqual(dataset.chunks, CHUNK_1D)
            self.assertIsNone(dataset.compression)
            self.assertEqual(dataset[...].tolist(), [100, 200, 400, 500, 600])

    def test_battery_passes(self):
        manifest = Manifest.load_or_create(self.workdir)
        outcomes = run_battery(self.hdf5_dir, self.a_list_path, manifest_path=manifest.path)
        failed = [o.line() for o in outcomes if o.status != "PASS"]
        self.assertEqual(failed, [])


class TestWriterLifecycle(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.addCleanup(self.tmp.cleanup)

    def test_rerun_skips_recorded_files(self):
        workdir, a_list, sim_info, hdf5_dir = make_written_workdir(self.root)
        before = {p.name: p.stat().st_mtime_ns for p in hdf5_dir.glob("*.h5")}
        run_write(workdir, a_list_path=a_list, simulation_info_path=sim_info)
        after = {p.name: p.stat().st_mtime_ns for p in hdf5_dir.glob("*.h5")}
        self.assertEqual(before, after)

    def test_tampered_output_refused(self):
        workdir, a_list, sim_info, hdf5_dir = make_written_workdir(self.root)
        with h5py.File(hdf5_dir / snapshot_h5_name(5), "r+") as handle:
            values = handle["halos"]["Vmax"][...]
            values[0] += np.float32(1.0)
            handle["halos"]["Vmax"][...] = values
        with self.assertRaisesRegex(ConverterError, "refusing to overwrite a recorded output"):
            run_write(workdir, a_list_path=a_list, simulation_info_path=sim_info)

    def test_missing_output_rewritten(self):
        workdir, a_list, sim_info, hdf5_dir = make_written_workdir(self.root)
        (hdf5_dir / snapshot_h5_name(3)).unlink()
        run_write(workdir, a_list_path=a_list, simulation_info_path=sim_info)
        self.assertTrue((hdf5_dir / snapshot_h5_name(3)).exists())

    def test_requires_linked_snapshots(self):
        workdir, a_list, sim_info = make_linked_workdir(self.root)
        with self.assertRaisesRegex(ConverterError, "run links first"):
            run_write(workdir, a_list_path=a_list, simulation_info_path=sim_info)

    def test_wrong_a_list_refused(self):
        workdir, a_list, sim_info = make_linked_workdir(self.root)
        run_links(workdir)
        other = fixtures.write_a_list(self.root / "other.a_list", fixtures.A_LIST + [1.1])
        with self.assertRaisesRegex(ConverterError, "a_list content md5"):
            run_write(workdir, a_list_path=other, simulation_info_path=sim_info)

    def test_wrong_simulation_info_refused(self):
        workdir, a_list, sim_info = make_linked_workdir(self.root)
        run_links(workdir)
        other = self.root / "other_info.yaml"
        other.write_text(Path(sim_info).read_text() + "# changed\n")
        with self.assertRaisesRegex(ConverterError, "simulation_info content md5"):
            run_write(workdir, a_list_path=a_list, simulation_info_path=other)

    def test_write_cli(self):
        workdir, a_list, sim_info = make_linked_workdir(self.root)
        run_links(workdir)
        rc = convert_ctrees.main(
            [
                "write",
                "--workdir",
                str(workdir),
                "--a-list",
                str(a_list),
                "--simulation-info",
                str(sim_info),
            ]
        )
        self.assertEqual(rc, 0)
        self.assertTrue((workdir / "hdf5" / snapshot_h5_name(0)).exists())


class TestReport(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.addCleanup(self.tmp.cleanup)

    def test_recommended_multiplier(self):
        self.assertEqual(recommended_multiplier(5, 5), 10**9)
        self.assertEqual(recommended_multiplier(10**9 - 1, 5), 10**9)
        self.assertEqual(recommended_multiplier(10**9, 5), 10**10)
        with self.assertRaisesRegex(ConverterError, "no valid identity multiplier"):
            recommended_multiplier(10**17, 10**3)

    def test_identity_multiplier_window_matches_the_reader_bounds(self):
        # Cross-check against the two conditions snapshot_identity_bounds_valid()
        # applies at run time (src/io/snapshot/interface.c), re-derived here
        # rather than reusing the implementation under test. Figures are the
        # projected Shin-Uchuu production dataset.
        max_rank, n_forests_total = 12_834_657_129, 166_547_771

        def reader_accepts(multiplier):
            return multiplier > max_rank and n_forests_total <= (2**63 - 1) // multiplier - 1

        lower, upper = identity_multiplier_window(max_rank, n_forests_total)
        self.assertEqual((lower, upper), (12_834_657_130, 55_379_738_354))
        self.assertTrue(reader_accepts(lower))
        self.assertTrue(reader_accepts(upper))
        self.assertFalse(reader_accepts(lower - 1))
        self.assertFalse(reader_accepts(upper + 1))

    def test_recommended_multiplier_searches_past_the_decade_ladder(self):
        # Shin-Uchuu production: the window is 4.3e10 wide but holds no power of
        # ten, so a decade-only search wrongly reports no valid multiplier.
        self.assertEqual(
            recommended_multiplier(max_rank=12_834_657_129, n_forests_total=166_547_771),
            2 * 10**10,
        )

    def test_window_floor_stays_positive_for_the_empty_dataset_sentinel(self):
        # An all-empty dataset carries (n_forests_total 0, max_rank -1); the
        # reader requires multiplier > 0, so the window must not offer 0.
        lower, upper = identity_multiplier_window(max_rank=-1, n_forests_total=0)
        self.assertEqual(lower, 1)
        self.assertLessEqual(lower, upper)

    def test_window_below_the_reader_default_still_yields_a_multiplier(self):
        # Very many small forests put the ceiling under TREE_MUL_FAC (1e9).
        # Flooring the window at the default would report it empty and abort a
        # report the reader would have accepted -- the same defect on new input.
        max_rank, n_forests_total = 5_000_000, 10**10
        lower, upper = identity_multiplier_window(max_rank, n_forests_total)
        self.assertLess(upper, 10**9)
        self.assertLessEqual(lower, upper)
        multiplier = recommended_multiplier(max_rank, n_forests_total)
        self.assertGreater(multiplier, max_rank)
        self.assertLessEqual(n_forests_total, (2**63 - 1) // multiplier - 1)

    def test_recommended_multiplier_falls_back_to_the_window_floor(self):
        # A window too narrow to hold any 1/2/5 x 10**k value still yields one.
        max_rank, n_forests_total = 12 * 10**17, 4
        lower, upper = identity_multiplier_window(max_rank, n_forests_total)
        multiplier = recommended_multiplier(max_rank, n_forests_total)
        self.assertEqual(multiplier, lower)
        self.assertLessEqual(multiplier, upper)

    def test_run_report_writes_artifacts(self):
        workdir, a_list, _, _ = make_written_workdir(self.root)
        report = run_report(workdir, a_list_path=a_list)
        self.assertTrue(report["validation_passed"])
        self.assertEqual(report["totals"]["halos"], 17)
        self.assertEqual(report["n_forests_total"], 5)
        self.assertEqual(report["max_halo_rank_in_forest"], 5)
        self.assertEqual(report["recommended_identity_multiplier"], 10**9)
        window_min, window_max = report["identity_multiplier_window"]
        self.assertLessEqual(window_min, report["recommended_identity_multiplier"])
        self.assertLessEqual(report["recommended_identity_multiplier"], window_max)
        self.assertEqual(report["totals"]["flyby_demotions"], 1)
        self.assertEqual(report["totals"]["snapshots_with_halos"], 5)
        # every a_list snapshot appears, with explicit zeros for empty ones
        self.assertEqual(sorted(report["per_snapshot"], key=int), [str(s) for s in range(6)])
        self.assertEqual(
            report["per_snapshot"]["0"],
            {"rows": 0, "flyby_demotions": 0, "len_zero_count": 0},
        )
        self.assertEqual(len(report["observed_pairs"]), 5)
        statuses = {entry["name"]: entry["status"] for entry in report["validation"]}
        self.assertEqual(statuses["count-conservation"], "PASS")
        self.assertTrue((workdir / "conversion_report.json").exists())
        text = (workdir / "conversion_report.txt").read_text()
        self.assertIn("validation: PASS", text)
        self.assertIn("recommended identity multiplier=1000000000", text)

    def test_report_requires_write(self):
        workdir, a_list, _ = make_linked_workdir(self.root)
        run_links(workdir)
        with self.assertRaisesRegex(ConverterError, "run write first"):
            run_report(workdir, a_list_path=a_list)

    def test_report_cli_fails_on_bad_dataset(self):
        workdir, a_list, sim_info, hdf5_dir = make_written_workdir(self.root)
        with h5py.File(hdf5_dir / snapshot_h5_name(5), "r+") as handle:
            values = handle["halos"]["Len"][...]
            values[0] = -1
            handle["halos"]["Len"][...] = values
        rc = convert_ctrees.main(["report", "--workdir", str(workdir), "--a-list", str(a_list)])
        self.assertEqual(rc, 1)
        report_text = (workdir / "conversion_report.txt").read_text()
        self.assertIn("validation: FAIL", report_text)

    def test_report_cli_passes_on_good_dataset(self):
        workdir, a_list, _, _ = make_written_workdir(self.root)
        rc = convert_ctrees.main(["report", "--workdir", str(workdir), "--a-list", str(a_list)])
        self.assertEqual(rc, 0)

    def test_build_report_requires_links(self):
        workdir, _, _ = make_linked_workdir(self.root)
        manifest = Manifest.load_or_create(workdir)
        with self.assertRaisesRegex(ConverterError, "run links first"):
            build_report(manifest, [], n_snapshots=6)


class TestWriterConsumesScratch(unittest.TestCase):
    """Plan Slice 8 deletion table, writer half: ``fixed_N`` and ``links_N`` go
    once snapshot N's emitted HDF5 is verified and recorded.

    The writer is their terminal consumer — ``links`` reads the fixed file too
    (``_load_fixed``), which is why neither may be deleted there — so this is
    the last stage that can drop them, and the point at which the workdir's
    peak footprint is decided.
    """

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.addCleanup(self.tmp.cleanup)

    @staticmethod
    def _scratch(workdir):
        manifest = Manifest.load_or_create(workdir)
        paths = {}
        for snap, entry in manifest.data["snapshots"].items():
            paths["fixed_{}".format(snap)] = Path(entry["fixed_file"])
            paths["links_{}".format(snap)] = Path(entry["links_file"])
        return paths

    def _linked(self, name):
        root = self.root / name
        root.mkdir()
        workdir, a_list, sim_info = make_linked_workdir(root)
        run_links(workdir)
        return workdir, a_list, sim_info

    def test_flag_off_retains_every_fixed_and_links_file(self):
        workdir, a_list, sim_info = self._linked("off")
        run_write(workdir, a_list_path=a_list, simulation_info_path=sim_info)
        manifest = Manifest.load_or_create(workdir)
        for name, path in self._scratch(workdir).items():
            self.assertTrue(path.exists(), "{} was deleted with the flag off".format(name))
            self.assertEqual(
                "present", manifest.data["intermediates"][str(path.resolve())]["status"], name
            )

    def test_flag_on_consumes_every_fixed_and_links_file(self):
        workdir, a_list, sim_info = self._linked("on")
        expected = self._scratch(workdir)
        with capture_stderr() as captured:
            run_write(
                workdir,
                a_list_path=a_list,
                simulation_info_path=sim_info,
                consume_intermediates=True,
            )
        manifest = Manifest.load_or_create(workdir)
        for name, path in expected.items():
            self.assertFalse(path.exists(), "{} survived".format(name))
            self.assertEqual(
                "removed", manifest.data["intermediates"][str(path.resolve())]["status"], name
            )
            self.assertIn(str(path), captured.text)

    def test_output_is_recorded_before_its_inputs_go(self):
        """The protocol's ordering: at the instant a fixed or links file is
        unlinked, that snapshot's emitted file must already be recorded in the
        manifest ON DISK."""
        workdir, a_list, sim_info = self._linked("order")
        manifest_path = Path(workdir) / "manifest.json"
        observed = []
        real_remove = Manifest.remove_intermediate

        def spy(self, path):
            import json

            snap = int(Path(path).name.split("_")[1])
            saved = json.loads(manifest_path.read_text())
            recorded = [
                key for key in saved.get("outputs", {}) if key.endswith(snapshot_h5_name(snap))
            ]
            observed.append((snap, recorded))
            return real_remove(self, path)

        with mock.patch.object(Manifest, "remove_intermediate", spy):
            run_write(
                workdir,
                a_list_path=a_list,
                simulation_info_path=sim_info,
                consume_intermediates=True,
            )
        self.assertTrue(observed)
        for snap, recorded in observed:
            self.assertEqual(1, len(recorded), "snapshot {} output not recorded yet".format(snap))

    def test_emitted_dataset_is_bitwise_identical_with_the_flag_on_and_off(self):
        off_workdir, off_a_list, off_info = self._linked("dataset-off")
        on_workdir, on_a_list, on_info = self._linked("dataset-on")
        off = run_write(off_workdir, a_list_path=off_a_list, simulation_info_path=off_info)
        on = run_write(
            on_workdir,
            a_list_path=on_a_list,
            simulation_info_path=on_info,
            consume_intermediates=True,
        )
        off_dir = Path(off.data["outputs_dir"])
        on_dir = Path(on.data["outputs_dir"])
        off_files = sorted(path.name for path in off_dir.iterdir())
        self.assertEqual(off_files, sorted(path.name for path in on_dir.iterdir()))
        self.assertIn("forests.h5", off_files)
        for name in off_files:
            self.assertEqual(
                (off_dir / name).read_bytes(),
                (on_dir / name).read_bytes(),
                "{} differs between the two flag states".format(name),
            )

    def test_rerunning_the_writer_after_consumption_is_a_skip(self):
        workdir, a_list, sim_info = self._linked("rerun")
        run_write(
            workdir,
            a_list_path=a_list,
            simulation_info_path=sim_info,
            consume_intermediates=True,
        )
        with capture_stderr() as captured:
            manifest = run_write(
                workdir,
                a_list_path=a_list,
                simulation_info_path=sim_info,
                consume_intermediates=True,
            )
        self.assertIn("fixed and links scratch consumed", captured.text)
        self.assertIn("0 snapshot file(s) written", captured.text)
        self.assertEqual(6, len(manifest.data["outputs"]) - 1)

    def test_rerunning_links_after_the_writer_consumed_its_inputs_is_a_skip(self):
        """``run_links`` streams every snapshot's fixed file, so once the writer
        has consumed them the rank pass cannot run again. A fully linked stage
        in that state skips and names what was consumed."""
        workdir, a_list, sim_info = self._linked("links-rerun")
        run_write(
            workdir,
            a_list_path=a_list,
            simulation_info_path=sim_info,
            consume_intermediates=True,
        )
        with capture_stderr() as captured:
            run_links(workdir)
        self.assertIn("skipping the rank pass", captured.text)
        self.assertIn("consumed by the write stage", captured.text)

    def test_crash_between_unlink_and_save_converges_to_removed(self):
        for delete in (False, True):
            with self.subTest(consume_intermediates=delete):
                workdir, a_list, sim_info = self._linked("writer-crash-{}".format(int(delete)))
                run_write(workdir, a_list_path=a_list, simulation_info_path=sim_info)
                victim = self._scratch(workdir)["links_5"]
                victim.unlink()  # the unlink landed; the save did not
                manifest = Manifest.load_or_create(workdir)
                self.assertEqual(
                    "present", manifest.data["intermediates"][str(victim.resolve())]["status"]
                )
                run_write(
                    workdir,
                    a_list_path=a_list,
                    simulation_info_path=sim_info,
                    consume_intermediates=delete,
                )
                reloaded = Manifest.load_or_create(workdir)
                self.assertEqual(
                    "removed", reloaded.data["intermediates"][str(victim.resolve())]["status"]
                )

    def test_full_pipeline_with_consumption_leaves_only_the_run_scoped_tables(self):
        """The end state the storage envelope is measured against: with the
        flag on through fixups, links and write, every per-snapshot
        intermediate is gone and only the two run-scoped sidecar tables and the
        emitted dataset remain."""
        root = self.root / "envelope"
        root.mkdir()
        tree_file = fixtures.write_ctrees_file(
            root / "tree_0.dat", fixtures.all_trees(fixtures.standard_forests())
        )
        forests_list = fixtures.write_forests_list(
            root / "forests.list", fixtures.standard_forests()
        )
        a_list = fixtures.write_a_list(root / "test.a_list")
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
        run_fixups(
            workdir,
            a_list_path=a_list,
            simulation_info_path=sim_info,
            consume_intermediates=True,
        )
        run_links(workdir, consume_intermediates=True)
        manifest = run_write(
            workdir,
            a_list_path=a_list,
            simulation_info_path=sim_info,
            consume_intermediates=True,
        )
        present = sorted(
            Path(key).name
            for key, entry in manifest.data["intermediates"].items()
            if entry["status"] == "present"
        )
        # what survives is the two run-scoped sidecar tables plus the two
        # per-source sidecars the ``release`` verification path owns — which
        # this slice deliberately does not touch, because release refuses a
        # source whose own intermediates are recorded removed. Nothing
        # per-snapshot is left.
        self.assertEqual(
            [
                "forest_index_table.npy",
                "forest_max_snap.npy",
                "forest_max_src_0.npy",
                "roots_src_0.npy",
            ],
            present,
        )
        for key, entry in manifest.data["intermediates"].items():
            self.assertEqual(entry["status"] == "present", Path(key).exists(), key)
        outcomes = run_battery(
            Path(manifest.data["outputs_dir"]),
            a_list,
            manifest_path=manifest.path,
        )
        self.assertTrue(all(outcome.status == "PASS" for outcome in outcomes), outcomes)

    def test_cli_flag_is_off_by_default(self):
        parser = convert_ctrees.build_arg_parser()
        base = ["write", "--workdir", "w", "--a-list", "a", "--simulation-info", "s"]
        self.assertFalse(parser.parse_args(base).consume_intermediates)
        self.assertTrue(parser.parse_args(base + ["--consume-intermediates"]).consume_intermediates)


if __name__ == "__main__":
    unittest.main()
