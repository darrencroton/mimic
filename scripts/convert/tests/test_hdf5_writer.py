"""Slice 7 unit tests: horizontal-HDF5 emission against the frozen contract
(docs/dev/HORIZONTAL-HDF5-FORMAT.md), the forests.h5 sidecar, writer resume/refuse
semantics, and the conversion report.

Converter generalisation Slice 8 adds the format version 3 writer
(``HorizontalV3Writer``) at the end. Its oracle is ``test_pipeline``'s literal
gapped L-Halo source and the transposed arrays hand-derived from it
(``test_pipeline.EXPECTED``), extended here with payload and extra values
that are literal functions of each halo's SourceHaloID -- never the writer's
own table or the transpose's output."""

import dataclasses
import os
import shutil
import sys
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest import mock

import h5py
import numpy as np

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import convert_ctrees  # noqa: E402
import fixtures  # noqa: E402
import pipeline  # noqa: E402
import test_ctrees_hdf5_adapter as h5fixtures  # noqa: E402
import test_lhalo_adapter as lhalo  # noqa: E402
import test_pipeline as literal  # noqa: E402
from column_schema import build_schema, load_column_map  # noqa: E402
from conversion_manifest import ConversionManifest  # noqa: E402
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
from hdf5_writer_v3 import (  # noqa: E402
    V3_FORMAT_VERSION,
    V3_SIDECAR_DATASETS,
    HorizontalV3Writer,
    v3_chunk_shape,
    write_v3_sidecar,
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
from validate_v3 import run_battery_v3  # noqa: E402


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

    def test_negative_mostboundid_aborts(self):
        """fix_flybys used to negate MostBoundID as a demotion marker; a
        negative value reaching emission must now abort rather than pass
        through, even though it would still satisfy the ascending-|value|
        slab-order check on its own."""
        fixed = self._fixed([10, -20, 30])
        links = np.zeros(3, dtype=LINKS_RECORD_DTYPE)
        with self.assertRaisesRegex(ConverterError, "non-positive MostBoundID"):
            build_halo_arrays(fixed, links, 5, "test")

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

    def test_mostboundid_emitted_positive(self):
        """fix_flybys used to emit a negated MostBoundID for every demoted
        central (1020 here); with it removed (decision D1) every emitted value
        must be strictly positive."""
        with h5py.File(self.hdf5_dir / snapshot_h5_name(5), "r") as handle:
            mostbound = handle["halos"]["MostBoundID"][...]
        self.assertTrue((mostbound > 0).all(), mostbound.tolist())
        self.assertIn(1020, mostbound.tolist())

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
        # Cross-check against the two conditions horizontal_identity_bounds_valid()
        # applies at run time (src/io/horizontal/interface.c), re-derived here
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
        # required, always-zero field (decision D9(b)): no stage can demote
        self.assertEqual(report["totals"]["flyby_demotions"], 0)
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


class TestExtendedLayoutRefusal(unittest.TestCase):
    """Converter generalisation Slice 5: extended scratch is never written as v2."""

    def test_extended_workdir_is_refused_before_any_file_is_written(self):
        from column_schema import build_schema, load_column_map
        from test_fixups import ALL_TYPES_PROFILE, run_both_layouts

        for profile in (
            ALL_TYPES_PROFILE,
            Path(__file__).resolve().parents[1] / "profiles" / "consistent_trees_ascii.yaml",
        ):
            with self.subTest(profile=profile.name), tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp)
                schema = build_schema(load_column_map(profile))
                legacy, extended = run_both_layouts(root, schema)
                with self.assertRaisesRegex(ConverterError, "format_version 2 cannot carry"):
                    run_write(extended.workdir, root / "test.a_list", root / "simulation_info.yaml")
                self.assertFalse((extended.workdir / "hdf5").exists())
                self.assertNotIn("outputs", Manifest.load_or_create(extended.workdir).data)
                with capture_stderr():
                    run_write(legacy.workdir, root / "test.a_list", root / "simulation_info.yaml")
                self.assertTrue((legacy.workdir / "hdf5" / "forests.h5").exists())

    def test_legacy_cli_write_refuses_an_extended_workdir(self):
        from column_schema import build_schema, load_column_map
        from test_fixups import ALL_TYPES_PROFILE, run_both_layouts

        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            _legacy, extended = run_both_layouts(
                root, build_schema(load_column_map(ALL_TYPES_PROFILE))
            )
            with capture_stderr() as err:
                code = convert_ctrees.main(
                    [
                        "write",
                        "--workdir",
                        str(extended.workdir),
                        "--a-list",
                        str(root / "test.a_list"),
                        "--simulation-info",
                        str(root / "simulation_info.yaml"),
                    ]
                )
            self.assertEqual(code, 1)
            self.assertIn("cannot carry", err.text)
            self.assertFalse((extended.workdir / "hdf5").exists())


# ==========================================================================
# Format version 3 (converter generalisation Slice 8)
# ==========================================================================

#: Literal fixed-table storage of every v3 /halos topology and identity
#: column, restated from docs/dev/HORIZONTAL-HDF5-FORMAT.md (section "Version 3").
V3_FIXED = {
    "Descendant": "<i8",
    "FirstProgenitor": "<i8",
    "NextProgenitor": "<i8",
    "FirstHaloInFOFgroup": "<i8",
    "NextHaloInFOFgroup": "<i8",
    "DescendantSnapshot": "<i4",
    "FirstProgenitorSnapshot": "<i4",
    "NextProgenitorSnapshot": "<i4",
    "SourceHaloID": "<i8",
    "ForestIndex": "<i8",
    "HaloRankInForest": "<i8",
}

#: The L-Halo extras-example profile's payload and extras as the file must
#: declare and store them: name -> (type, units, dtype, vec3).
V3_LHALO_EXTRAS_PAYLOAD = {
    "Len": ("int", "particles", "<i4", False),
    "SnapNum": ("int", "dimensionless", "<i4", False),
    "M_Crit200": ("float", "1e10 Msun/h", "<f4", False),
    "Pos": ("vec3_float", "Mpc/h", "<f4", True),
    "Vel": ("vec3_float", "km/s", "<f4", True),
    "Spin": ("vec3_float", "Mpc/h km/s", "<f4", True),
    "VelDisp": ("float", "km/s", "<f4", False),
    "Vmax": ("float", "km/s", "<f4", False),
    "MostBoundID": ("long long", "dimensionless", "<i8", False),
    "M_Mean200": ("float", "1e10 Msun/h", "<f4", False),
    "M_TopHat": ("float", "1e10 Msun/h", "<f4", False),
    "SubHalfMass": ("float", "1e10 Msun/h", "<f4", False),
    "SourceFileNr": ("int", "dimensionless", "<i4", False),
    "SubhaloIndex": ("int", "dimensionless", "<i4", False),
    "PosX": ("float", "Mpc/h", "<f4", False),
}


def v3_source_values(sid: int) -> dict:
    """Every non-topology L-Halo field of the literal halo with this
    SourceHaloID, as a literal function of it. Spin's last component is a
    signed zero, which must survive."""
    return {
        "M_Mean200": sid + 0.5,
        "M_TopHat": -0.25 * sid,
        "SubHalfMass": 1024.0 * sid + 0.125,
        "FileNr": -sid,
        "SubhaloIndex": 100 + sid,
        "Pos": (sid + 0.25, 2.0 * sid, 3.0 * sid),
        "Vel": (float(sid), -float(sid), 0.5),
        "Spin": (0.125 * sid, 1.0, -0.0),
        "VelDisp": 10.0 + sid,
        "Vmax": 20.0 + sid,
    }


def v3_expected_extras(sid: int) -> dict:
    """The v3 dataset value of every payload/extra column for one halo:
    the literal source value, renamed and component-selected exactly as the
    extras profile declares."""
    values = v3_source_values(sid)
    return {
        "Pos": values["Pos"],
        "Vel": values["Vel"],
        "Spin": values["Spin"],
        "VelDisp": values["VelDisp"],
        "Vmax": values["Vmax"],
        "M_Mean200": values["M_Mean200"],
        "M_TopHat": values["M_TopHat"],
        "SubHalfMass": values["SubHalfMass"],
        "SourceFileNr": values["FileNr"],
        "SubhaloIndex": values["SubhaloIndex"],
        "PosX": values["Pos"][0],
    }


def v3_literal_trees():
    """test_pipeline's three literal trees (SourceHaloID A -> 1-4, B -> 5,
    C -> 6-8) with every other field set by :func:`v3_source_values`."""
    trees = []
    sid = 1
    for tree in (literal.TREE_A, literal.TREE_B, literal.TREE_C):
        rows = []
        for row in tree:
            values = dict(row)
            values.update(v3_source_values(sid))
            rows.append(values)
            sid += 1
        trees.append(rows)
    return trees


def write_simulation_info(path, particle_mass: float = 0.0325, box: float = 100.0) -> Path:
    path = Path(path)
    path.write_text(
        "simulation:\n"
        "  cosmology: {omega_matter: 0.3089, omega_lambda: 0.6911, hubble_h: 0.6774}\n"
        "  box_size: {value: %r, units: Mpc/h}\n"
        "  particle_mass: {value: %r, units: 1e10 Msun/h}\n" % (box, particle_mass)
    )
    return path


def make_v3_conversion(
    root, profile="lhalo_binary_extras_example.yaml", *, write=True, block_rows=2
):
    """The literal gapped L-Halo source through initialize, ingest,
    transpose and (unless ``write`` is False) the v3 write stage, streaming in
    ``block_rows``-row blocks so even eight halos cross block boundaries."""
    root = Path(root)
    src = root / "src"
    src.mkdir(parents=True)
    tree_a, tree_b, tree_c = v3_literal_trees()
    file0 = lhalo.write_lhalo_file(str(src / "trees.0"), [tree_a, tree_b])
    file1 = lhalo.write_lhalo_file(str(src / "trees.1"), [tree_c])
    a_list = src / "a.list"
    a_list.write_text(literal.A_LIST_TEXT)
    sim_info = write_simulation_info(src / "simulation_info.yaml")
    work = root / "work"
    schema = literal.lhalo_schema(profile)
    pipeline.initialize(
        work,
        schema,
        {"sources": [[0, file0], [1, file1]]},
        a_list,
        ingest_max_rows=3,
        transpose_budget_bytes=literal.BUDGET,
    )
    pipeline.run_ingest(work)
    pipeline.run_transpose(work)
    conversion = SimpleNamespace(
        work=work, a_list=a_list, sim_info=sim_info, schema=schema, manifest=None, dataset=None
    )
    if write:
        manifest = pipeline.run_write(work, HorizontalV3Writer(sim_info, block_rows=block_rows))
        conversion.manifest = manifest
        conversion.dataset = manifest.artifact_path(manifest.stage("write")["directory"])
    return conversion


class V3Case(unittest.TestCase):
    """One written extras conversion shared by the read-only tests."""

    @classmethod
    def setUpClass(cls):
        cls._tmp = tempfile.TemporaryDirectory(prefix="v3_writer_")
        cls.conv = make_v3_conversion(cls._tmp.name)

    @classmethod
    def tearDownClass(cls):
        cls._tmp.cleanup()

    def open(self, name):
        handle = h5py.File(self.conv.dataset / name, "r")
        self.addCleanup(handle.close)
        return handle


class TestV3Emission(V3Case):
    def test_emits_every_a_list_snapshot_including_the_empty_one_and_the_sidecar(self):
        self.assertEqual(
            sorted(os.listdir(self.conv.dataset)),
            ["forests.h5"] + [snapshot_h5_name(s) for s in range(4)],
        )
        empty = self.open(snapshot_h5_name(1))
        self.assertEqual(int(empty["header"].attrs["n_halos"]), 0)
        self.assertEqual(set(empty["schema"]), set(V3_LHALO_EXTRAS_PAYLOAD))
        for name, dtype in V3_FIXED.items():
            self.assertEqual(empty["halos"][name].shape, (0,), name)
            self.assertEqual(empty["halos"][name].dtype.str, dtype, name)
        for name, (_type, _units, dtype, is_vec) in V3_LHALO_EXTRAS_PAYLOAD.items():
            self.assertEqual(empty["halos"][name].shape, (0, 3) if is_vec else (0,), name)
            self.assertEqual(empty["halos"][name].dtype.str, dtype, name)

    def test_every_file_holds_exactly_the_c3_objects(self):
        header_names = set(HEADER_ATTRS) | {"source_format", "column_mapping_sha256"}
        for snap in range(4):
            handle = self.open(snapshot_h5_name(snap))
            self.assertEqual(set(handle.keys()), {"header", "halos", "schema"})
            self.assertEqual(set(handle.attrs.keys()), set())
            self.assertEqual(set(handle["header"].attrs.keys()), header_names)
            self.assertEqual(
                set(handle["halos"].keys()), set(V3_FIXED) | set(V3_LHALO_EXTRAS_PAYLOAD)
            )
            for name in handle["halos"]:
                self.assertIsInstance(handle["halos"].get(name, getlink=True), h5py.HardLink)
        sidecar = self.open("forests.h5")
        self.assertEqual(
            set(sidecar.keys()), {"ForestID", "SourceFileOrdinal", "SourceUnitOrdinal"}
        )
        self.assertEqual(set(sidecar.attrs.keys()), set())

    def test_topology_and_identity_are_the_hand_derived_gapped_graph(self):
        for snap, columns in literal.EXPECTED.items():
            halos = self.open(snapshot_h5_name(snap))["halos"]
            for name, values in columns.items():
                if name == "M_Crit200":
                    expected = np.asarray(values, dtype="<f4")
                elif name in V3_FIXED:
                    expected = np.asarray(values, dtype=V3_FIXED[name])
                else:
                    expected = np.asarray(values, dtype=V3_LHALO_EXTRAS_PAYLOAD[name][2])
                stored = halos[name][...]
                self.assertEqual(stored.dtype.str, expected.dtype.str, name)
                np.testing.assert_array_equal(stored, expected, err_msg="{} {}".format(snap, name))
        # the two gapped descendants and the mixed-age sibling survive as such
        snap0 = self.open(snapshot_h5_name(0))["halos"]
        np.testing.assert_array_equal(snap0["DescendantSnapshot"][...], [3, 2])
        snap2 = self.open(snapshot_h5_name(2))["halos"]
        np.testing.assert_array_equal(snap2["NextProgenitorSnapshot"][...], [0, -1, -1])

    def test_signed_duplicated_mostboundid_is_carried_as_data(self):
        snap3 = self.open(snapshot_h5_name(3))["halos"]
        np.testing.assert_array_equal(snap3["MostBoundID"][...], [77, 13, 77])
        snap2 = self.open(snapshot_h5_name(2))["halos"]
        np.testing.assert_array_equal(snap2["MostBoundID"][...], [-5, 12, -3])

    def test_payload_and_extras_keep_native_units_precision_and_values(self):
        for snap, columns in literal.EXPECTED.items():
            halos = self.open(snapshot_h5_name(snap))["halos"]
            wanted = [v3_expected_extras(sid) for sid in columns["SourceHaloID"]]
            for name in v3_expected_extras(1):
                _type, _units, dtype, _vec = V3_LHALO_EXTRAS_PAYLOAD[name]
                expected = np.asarray([row[name] for row in wanted], dtype=dtype)
                stored = halos[name][...]
                self.assertEqual(stored.dtype.str, dtype, name)
                # bitwise, so the signed zero in Spin counts
                self.assertEqual(stored.tobytes(), expected.tobytes(), "{} {}".format(snap, name))

    def test_header_records_version_measured_gaps_identity_bounds_and_provenance(self):
        for snap, scale in enumerate((0.25, 0.5, 0.75, 1.0)):
            attrs = self.open(snapshot_h5_name(snap))["header"].attrs
            self.assertEqual(attrs["format_version"], 3)
            self.assertEqual(attrs["format_version"].dtype, np.int32)
            self.assertEqual(attrs["links_adjacent"], 0)
            self.assertEqual(attrs["snapshot_number"], snap)
            self.assertEqual(attrs["scale_factor"], scale)
            self.assertEqual(attrs["n_halos"], len(literal.EXPECTED[snap]["SourceHaloID"]))
            self.assertEqual(attrs["n_forests_total"], 3)
            self.assertEqual(attrs["max_halo_rank_in_forest"], 3)
            self.assertEqual(attrs["particle_mass_msun_h"], 0.0325e10)
            self.assertEqual(attrs["box_size_mpc_h"], 100.0)
            self.assertEqual(attrs["source_format"], b"lhalo_binary")
            self.assertEqual(attrs["column_mapping_sha256"], self.conv.schema.digest.encode())
            for name, size in (("source_format", 32), ("column_mapping_sha256", 64)):
                kind = attrs.get_id(name).get_type()
                self.assertFalse(kind.is_variable_str())
                self.assertEqual(kind.get_size(), size)
                self.assertEqual(kind.get_cset(), h5py.h5t.CSET_ASCII)

    def test_schema_group_declares_every_payload_field_exactly(self):
        for snap in range(4):
            schema = self.open(snapshot_h5_name(snap))["schema"]
            self.assertEqual(set(schema), set(V3_LHALO_EXTRAS_PAYLOAD))
            for name, (type_name, units, _dtype, _vec) in V3_LHALO_EXTRAS_PAYLOAD.items():
                group = schema[name]
                self.assertEqual(len(group), 0)
                self.assertEqual(set(group.attrs), {"type", "units", "h_convention", "description"})
                self.assertEqual(group.attrs["type"], type_name)
                self.assertEqual(group.attrs["units"], units)
                for key in group.attrs:
                    kind = group.attrs.get_id(key).get_type()
                    self.assertTrue(kind.is_variable_str())
                    self.assertEqual(kind.get_cset(), h5py.h5t.CSET_UTF8)

    def test_storage_is_chunked_explicit_little_endian_and_unfiltered(self):
        halos = self.open(snapshot_h5_name(2))["halos"]
        for name in halos:
            dataset = halos[name]
            self.assertEqual(
                dataset.chunks, v3_chunk_shape(dataset.shape[0], dataset.ndim == 2), name
            )
            self.assertIsNone(dataset.compression)
            self.assertFalse(dataset.shuffle)
            self.assertEqual(dataset.id.get_type().get_order(), h5py.h5t.ORDER_LE, name)

    def test_chunk_rows_are_the_row_count_clamped_to_the_ceiling(self):
        self.assertEqual(v3_chunk_shape(0, False), (1,))
        self.assertEqual(v3_chunk_shape(0, True), (1, 3))
        self.assertEqual(v3_chunk_shape(7, False), (7,))
        self.assertEqual(v3_chunk_shape(7, True), (7, 3))
        self.assertEqual(v3_chunk_shape(65536, False), (65536,))
        self.assertEqual(v3_chunk_shape(10**9, True), (65536, 3))

    def test_sidecar_chunks_are_the_forest_count_only_when_it_is_known_up_front(self):
        scratch = tempfile.TemporaryDirectory()
        self.addCleanup(scratch.cleanup)

        def block(n):
            return {name: np.arange(n, dtype="<i8") for name in V3_SIDECAR_DATASETS}

        def chunks(blocks):
            path = Path(scratch.name) / "forests_{}.h5".format(len(blocks))
            write_v3_sidecar(path, blocks)
            with h5py.File(path, "r") as handle:
                return {handle[name].chunks for name in handle}

        self.assertEqual(chunks([block(5)]), {(5,)})
        self.assertEqual(chunks([block(5), block(2)]), {(65536,)})
        self.assertEqual(chunks([]), {(1,)})

    def test_sidecar_carries_the_lhalo_forest_provenance(self):
        sidecar = self.open("forests.h5")
        np.testing.assert_array_equal(sidecar["ForestID"][...], [0, 1, 2])
        np.testing.assert_array_equal(sidecar["SourceFileOrdinal"][...], [0, 0, 1])
        np.testing.assert_array_equal(sidecar["SourceUnitOrdinal"][...], [0, 1, 0])
        for name in sidecar:
            self.assertEqual(sidecar[name].dtype.str, "<i8")

    def test_the_v3_battery_passes_and_the_v2_battery_rejects_the_dataset(self):
        result = run_battery_v3(
            self.conv.dataset,
            self.conv.a_list,
            manifest_path=self.conv.manifest.path,
            budget_bytes=1 << 20,
        )
        self.assertEqual(
            [o.name for o in result.outcomes if o.status != "PASS"], [], result.outcomes
        )
        v2 = {o.name: o for o in run_battery(self.conv.dataset, self.conv.a_list)}
        self.assertEqual(v2["object-set"].status, "FAIL")
        self.assertIn("schema", v2["object-set"].detail)

    def test_the_write_stage_records_the_writer_identity(self):
        writer = self.conv.manifest.stage("write")["result"]["writer"]
        self.assertEqual(writer["format_version"], V3_FORMAT_VERSION)
        self.assertEqual(writer["header"]["particle_mass_msun_h"], 0.0325e10)
        self.assertNotEqual(V3_FORMAT_VERSION, FORMAT_VERSION)


class _CorruptingWriter(HorizontalV3Writer):
    """Writes correctly, then flips one stored value before ``verify`` runs."""

    def __init__(self, sim_info, file_name, dataset, row=0):
        super().__init__(sim_info)
        self.target = (file_name, dataset, row)

    def write(self, inputs, out_dir):
        produced = super().write(inputs, out_dir)
        file_name, dataset, row = self.target
        with h5py.File(Path(out_dir) / file_name, "r+") as handle:
            values = handle["halos"][dataset]
            if values.dtype.kind == "f":
                values[row] = values[row] + 1
            else:
                values[row] = values[row] + 7
        return produced


class _MutatingWriter(HorizontalV3Writer):
    """Writes correctly, then applies ``mutate(out_dir)`` before ``verify``
    runs."""

    def __init__(self, sim_info, mutate):
        super().__init__(sim_info)
        self.mutate = mutate

    def write(self, inputs, out_dir):
        produced = super().write(inputs, out_dir)
        self.mutate(Path(out_dir))
        return produced


def _rechunk_empty(file_name, dataset, chunks):
    """A mutation re-creating one zero-row /halos dataset with other chunks."""

    def mutate(out_dir):
        with h5py.File(out_dir / file_name, "r+") as handle:
            halos = handle["halos"]
            dtype, shape = halos[dataset].dtype, halos[dataset].shape
            del halos[dataset]
            halos.create_dataset(
                dataset,
                shape=shape,
                dtype=dtype,
                chunks=chunks,
                maxshape=(None,) + shape[1:],
            )

    return mutate


class TestV3WriteVerification(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="v3_verify_"))
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)
        self.conv = make_v3_conversion(self.tmp, write=False)

    def assert_refused_before_success_or_cleanup(self, writer, message):
        with self.assertRaisesRegex(ConverterError, message):
            pipeline.run_write(self.conv.work, writer, consume_transposed=True)
        manifest = ConversionManifest.load(self.conv.work)
        self.assertEqual(manifest.stage("write")["status"], "failed")
        self.assertEqual(manifest.stage("write")["artifacts"], [])
        # nothing was consumed: every transposed snapshot is still verified present
        manifest.verify_stage_artifacts("transpose")

    def test_corruption_in_an_extra_field_fails_the_stage(self):
        writer = _CorruptingWriter(self.conv.sim_info, snapshot_h5_name(2), "SubHalfMass", 1)
        self.assert_refused_before_success_or_cleanup(writer, "SubHalfMass")

    def test_corruption_in_a_target_snapshot_column_fails_the_stage(self):
        writer = _CorruptingWriter(self.conv.sim_info, snapshot_h5_name(0), "DescendantSnapshot")
        self.assert_refused_before_success_or_cleanup(writer, "DescendantSnapshot")

    def test_a_changed_schema_attribute_fails_the_stage(self):
        def mutate(out_dir):
            with h5py.File(out_dir / snapshot_h5_name(2), "r+") as handle:
                handle["schema"]["Vmax"].attrs.create(
                    "units", "m/s", dtype=h5py.string_dtype("utf-8")
                )

        self.assert_refused_before_success_or_cleanup(
            _MutatingWriter(self.conv.sim_info, mutate), "/schema/Vmax units"
        )

    def test_a_changed_sidecar_row_fails_the_stage(self):
        def mutate(out_dir):
            with h5py.File(out_dir / "forests.h5", "r+") as handle:
                handle["ForestID"][1] = 99

        self.assert_refused_before_success_or_cleanup(
            _MutatingWriter(self.conv.sim_info, mutate),
            "/ForestID rows .* differ from the forest enumeration",
        )

    def test_a_scalar_chunk_shape_in_the_zero_halo_snapshot_fails_the_stage(self):
        mutate = _rechunk_empty(snapshot_h5_name(1), "Vmax", (65537,))
        self.assert_refused_before_success_or_cleanup(
            _MutatingWriter(self.conv.sim_info, mutate),
            r"snapshot_001\.h5: /halos/Vmax: chunks \(65537,\)",
        )

    def test_a_vector_chunk_shape_in_the_zero_halo_snapshot_fails_the_stage(self):
        mutate = _rechunk_empty(snapshot_h5_name(1), "Pos", (2, 2))
        self.assert_refused_before_success_or_cleanup(
            _MutatingWriter(self.conv.sim_info, mutate),
            r"snapshot_001\.h5: /halos/Pos: chunks \(2, 2\)",
        )

    def test_a_retry_after_refusal_writes_a_verified_dataset(self):
        writer = _CorruptingWriter(self.conv.sim_info, snapshot_h5_name(3), "Pos")
        self.assert_refused_before_success_or_cleanup(writer, "Pos")
        manifest = pipeline.run_write(self.conv.work, HorizontalV3Writer(self.conv.sim_info))
        self.assertTrue(manifest.is_complete("write"))

    def test_a_completed_write_is_not_claimed_by_different_physical_metadata(self):
        pipeline.run_write(self.conv.work, HorizontalV3Writer(self.conv.sim_info))
        pipeline.run_write(self.conv.work, HorizontalV3Writer(self.conv.sim_info))
        other = write_simulation_info(self.tmp / "other.yaml", box=62.5)
        with self.assertRaisesRegex(ConverterError, "completed by writer"):
            pipeline.run_write(self.conv.work, HorizontalV3Writer(other))

    def test_inputs_disagreeing_with_the_transpose_gap_count_are_refused(self):
        manifest = ConversionManifest.load(self.conv.work)
        inputs = pipeline._write_inputs(manifest)
        result = dict(inputs.transpose_result, n_gapped_descendants=0, links_adjacent=True)
        out = self.tmp / "out"
        out.mkdir()
        with self.assertRaisesRegex(ConverterError, "refusing to stamp links_adjacent"):
            HorizontalV3Writer(self.conv.sim_info).write(
                dataclasses.replace(inputs, transpose_result=result), out
            )
        self.assertEqual(list(out.iterdir()), [])

    def test_inputs_without_a_forest_enumeration_are_refused(self):
        inputs = pipeline._write_inputs(ConversionManifest.load(self.conv.work))
        with self.assertRaisesRegex(ConverterError, "forest enumeration"):
            HorizontalV3Writer(self.conv.sim_info).write(
                dataclasses.replace(inputs, forests=None), self.tmp
            )

    def test_block_rows_must_be_a_positive_integer(self):
        for bad in (0, -1, 1.5, True, "8"):
            with self.assertRaises(ConverterError):
                HorizontalV3Writer(self.conv.sim_info, block_rows=bad)


class TestV3OtherRoutes(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="v3_routes_"))
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)

    def hdf5_conversion(self, simulation_info=None):
        directory = self.tmp / "h5"
        directory.mkdir()
        info = h5fixtures.write_source(
            str(directory),
            [{"forests": [h5fixtures.MERGER_FOREST]}, {"forests": [h5fixtures.lone(9, snap=5)]}],
        )
        a_list = self.tmp / "h5.a_list"
        a_list.write_text("".join("{}\n".format(0.5 + 0.1 * i) for i in range(6)))
        work = self.tmp / "h5work"
        pipeline.initialize(
            work,
            h5fixtures.load_schema(),
            {
                "info_path": info,
                "first_file": 0,
                "last_file": 1,
                "particle_mass": h5fixtures.PARTICLE_MASS,
                **({} if simulation_info is None else {"simulation_info": str(simulation_info)}),
            },
            a_list,
            ingest_max_rows=2,
            transpose_budget_bytes=literal.BUDGET,
        )
        pipeline.run_ingest(work)
        pipeline.run_transpose(work)
        return work, a_list

    def test_forests_hdf5_route_writes_an_adjacent_dataset_with_source_forest_ids(self):
        work, a_list = self.hdf5_conversion()
        sim_info = write_simulation_info(self.tmp / "sim.yaml", h5fixtures.PARTICLE_MASS)
        manifest = pipeline.run_write(work, HorizontalV3Writer(sim_info))
        dataset = manifest.artifact_path(manifest.stage("write")["directory"])
        with h5py.File(dataset / snapshot_h5_name(5), "r") as handle:
            self.assertEqual(handle["header"].attrs["links_adjacent"], 1)
            self.assertEqual(handle["header"].attrs["source_format"], b"consistent_trees_hdf5")
            self.assertEqual(handle["schema"]["M_Crit200"].attrs["units"], "Msun/h")
            np.testing.assert_array_equal(
                handle["halos"]["FirstProgenitorSnapshot"][...], [4, 4, -1]
            )
        with h5py.File(dataset / "forests.h5", "r") as handle:
            np.testing.assert_array_equal(handle["SourceFileOrdinal"][...], [0, 1])
            np.testing.assert_array_equal(handle["SourceUnitOrdinal"][...], [0, 0])
            np.testing.assert_array_equal(
                handle["ForestID"][...], [h5fixtures.MERGER_FOREST["id"], 9]
            )
        result = run_battery_v3(dataset, a_list, manifest_path=manifest.path, budget_bytes=1 << 20)
        self.assertEqual([o.name for o in result.outcomes if o.status != "PASS"], [])

    def test_forests_hdf5_route_refuses_a_header_particle_mass_its_len_did_not_use(self):
        work, _a_list = self.hdf5_conversion()
        sim_info = write_simulation_info(self.tmp / "sim.yaml", 0.0325)
        with self.assertRaisesRegex(ConverterError, "particle_mass"):
            pipeline.run_write(work, HorizontalV3Writer(sim_info))

    def test_forests_hdf5_route_refuses_simulation_info_it_was_not_recorded_against(self):
        # equal particle_mass, as Uchuu and micro-Uchuu share; only the box differs
        recorded = write_simulation_info(self.tmp / "sim.yaml", h5fixtures.PARTICLE_MASS, box=100.0)
        other = write_simulation_info(self.tmp / "other.yaml", h5fixtures.PARTICLE_MASS, box=62.5)
        work, _a_list = self.hdf5_conversion(simulation_info=recorded)
        with self.assertRaisesRegex(
            ConverterError,
            "forests-HDF5 conversion was recorded against a different simulation_info",
        ):
            pipeline.run_write(work, HorizontalV3Writer(other))
        manifest = pipeline.run_write(work, HorizontalV3Writer(recorded))
        self.assertTrue(manifest.is_complete("write"))

    def ascii_conversion(self):
        src = self.tmp / "ascii"
        src.mkdir()
        forests = fixtures.standard_forests()
        tree = fixtures.write_ctrees_file(src / "tree_0_0_0.dat", fixtures.all_trees(forests))
        parameters = {
            "tree_files": [str(tree)],
            "forests_list": str(fixtures.write_forests_list(src / "forests.list", forests)),
            "simulation_info": str(fixtures.write_simulation_info(src / "simulation_info.yaml")),
        }
        a_list = fixtures.write_a_list(src / "fixture.a_list")
        schema = build_schema(load_column_map(literal.PROFILE_DIR / "consistent_trees_ascii.yaml"))
        work = self.tmp / "ascii_work"
        pipeline.initialize(
            work,
            schema,
            parameters,
            a_list,
            ingest_max_rows=4,
            transpose_budget_bytes=literal.BUDGET,
        )
        pipeline.run_ingest(work)
        pipeline.run_transpose(work)
        return work, a_list, Path(parameters["simulation_info"]), forests

    def test_ascii_route_writes_and_validates(self):
        work, a_list, sim_info, forests = self.ascii_conversion()
        manifest = pipeline.run_write(work, HorizontalV3Writer(sim_info))
        dataset = manifest.artifact_path(manifest.stage("write")["directory"])
        with h5py.File(dataset / "forests.h5", "r") as handle:
            self.assertEqual(
                handle["ForestID"][...].tolist(), sorted(forest.forest_id for forest in forests)
            )
        result = run_battery_v3(dataset, a_list, manifest_path=manifest.path, budget_bytes=1 << 20)
        self.assertEqual([o.name for o in result.outcomes if o.status != "PASS"], [])

    def test_ascii_route_refuses_simulation_info_it_was_not_prepared_against(self):
        work, _a_list, _sim_info, _forests = self.ascii_conversion()
        other = write_simulation_info(self.tmp / "other.yaml", particle_mass=0.0325, box=50.0)
        with self.assertRaisesRegex(ConverterError, "different simulation_info"):
            pipeline.run_write(work, HorizontalV3Writer(other))


if __name__ == "__main__":
    unittest.main()
