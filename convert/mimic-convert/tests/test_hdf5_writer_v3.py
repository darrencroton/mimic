"""Unit tests: horizontal-HDF5 format version 3 emission
(``hdf5_writer_v3.HorizontalV3Writer``) against
convert/mimic-convert/HORIZONTAL-HDF5-FORMAT.md, the forests.h5 sidecar, the
header metadata loader, and the identity multiplier the conversion report
recommends.

The writer's oracle is the literal gapped L-Halo source of ``fixtures`` and the
transposed arrays hand-derived from it (``fixtures.LITERAL_EXPECTED``), extended
here with payload and extra values that are literal functions of each halo's
SourceHaloID -- never the writer's own table or the transpose's output."""

import dataclasses
import os
import shutil
import sys
import tempfile
import unittest
from pathlib import Path

import h5py
import numpy as np

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import fixtures  # noqa: E402
import pipeline  # noqa: E402
from column_schema import build_schema, load_column_map  # noqa: E402
from conversion_manifest import ConversionManifest  # noqa: E402
from ctrees_parser import ConverterError  # noqa: E402
from fixtures import (  # noqa: E402
    make_v3_conversion,
    v3_source_values,
)
from hdf5_writer_v3 import (  # noqa: E402
    HEADER_ATTRS,
    V3_FORMAT_VERSION,
    V3_SIDECAR_DATASETS,
    HorizontalV3Writer,
    load_header_metadata,
    snapshot_h5_name,
    v3_chunk_shape,
    v3_halo_datasets,
    write_v3_sidecar,
    write_v3_snapshot_columns,
)
from report import identity_multiplier_window, recommended_multiplier  # noqa: E402
from validate_v3 import run_battery_v3  # noqa: E402

#: Literal fixed-table storage of every v3 /halos topology and identity
#: column, restated from convert/mimic-convert/HORIZONTAL-HDF5-FORMAT.md (section "Version 3").
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


def write_simulation_info(path, particle_mass: float = 0.0325, box: float = 100.0) -> Path:
    path = Path(path)
    path.write_text(
        "simulation:\n"
        "  cosmology: {omega_matter: 0.3089, omega_lambda: 0.6911, hubble_h: 0.6774}\n"
        "  box_size: {value: %r, units: Mpc/h}\n"
        "  particle_mass: {value: %r, units: 1e10 Msun/h}\n" % (box, particle_mass)
    )
    return path


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

    def test_every_file_holds_exactly_the_header_halos_and_schema_groups(self):
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

    def test_the_column_writer_writes_the_record_writers_bytes(self):
        # the dataset rewriter writes through write_v3_snapshot_columns: the
        # same columns in the same block size give byte-identical files
        table = v3_halo_datasets(self.conv.schema)
        declarations = self.conv.schema.output_field_declarations()
        for snap in range(4):
            name = snapshot_h5_name(snap)
            with h5py.File(self.conv.dataset / name, "r") as handle:
                header = {
                    key: value.decode("ascii") if isinstance(value, bytes) else value
                    for key, value in handle["header"].attrs.items()
                }
                columns = {key: handle["halos"][key][...] for key in table}
            copy = Path(self._tmp.name) / ("columns_" + name)
            write_v3_snapshot_columns(
                copy,
                header,
                table,
                declarations,
                int(header["n_halos"]),
                columns.__getitem__,
                block_rows=2,
            )
            self.assertEqual(copy.read_bytes(), (self.conv.dataset / name).read_bytes(), name)

    def test_topology_and_identity_are_the_hand_derived_gapped_graph(self):
        for snap, columns in fixtures.LITERAL_EXPECTED.items():
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

    def test_payload_and_extras_keep_native_units_precision_and_values(self):
        for snap, columns in fixtures.LITERAL_EXPECTED.items():
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
            self.assertEqual(attrs["n_halos"], len(fixtures.LITERAL_EXPECTED[snap]["SourceHaloID"]))
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

    def test_the_battery_passes_the_dataset(self):
        result = run_battery_v3(
            self.conv.dataset,
            self.conv.a_list,
            manifest_path=self.conv.manifest.path,
            budget_bytes=1 << 20,
        )
        self.assertEqual(
            [o.name for o in result.outcomes if o.status != "PASS"], [], result.outcomes
        )


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

    def test_malformed_simulation_metadata_aborts(self):
        """Wrong box or particle-mass units and a missing cosmology block are
        each refused rather than stamped into a header."""
        cosmology = "  cosmology: {omega_matter: 0.3, omega_lambda: 0.7, hubble_h: 0.7}\n"
        box = "  box_size: {value: 100.0, units: Mpc/h}\n"
        mass = "  particle_mass: {value: 0.0325, units: 1e10 Msun/h}\n"
        cases = (
            (
                "box units",
                cosmology + "  box_size: {value: 100.0, units: kpc/h}\n" + mass,
                "box_size units",
            ),
            (
                "particle mass units",
                cosmology + box + "  particle_mass: {value: 3.25e8, units: Msun/h}\n",
                "particle_mass units",
            ),
            ("cosmology", box + mass, "malformed simulation metadata"),
        )
        for label, body, message in cases:
            with self.subTest(case=label):
                path = self._write_info("simulation:\n" + body)
                with self.assertRaisesRegex(ConverterError, message):
                    load_header_metadata(path)


class TestIdentityMultiplier(unittest.TestCase):
    """The identity multiplier window and recommendation the conversion
    report states (``report.identity_multiplier_window``,
    ``report.recommended_multiplier``)."""

    @staticmethod
    def reader_accepts(multiplier, max_rank, n_forests_total):
        """The two conditions horizontal_identity_bounds_valid() applies at run
        time (src/io/horizontal/interface.c), re-derived here rather than
        reusing the implementation under test."""
        return multiplier > max_rank and n_forests_total <= (2**63 - 1) // multiplier - 1

    def test_identity_multiplier_window_matches_the_reader_bounds(self):
        # Figures are the projected Shin-Uchuu production dataset.
        max_rank, n_forests_total = 12_834_657_129, 166_547_771

        lower, upper = identity_multiplier_window(max_rank, n_forests_total)
        self.assertEqual((lower, upper), (12_834_657_130, 55_379_738_354))
        self.assertTrue(self.reader_accepts(lower, max_rank, n_forests_total))
        self.assertTrue(self.reader_accepts(upper, max_rank, n_forests_total))
        self.assertFalse(self.reader_accepts(lower - 1, max_rank, n_forests_total))
        self.assertFalse(self.reader_accepts(upper + 1, max_rank, n_forests_total))

    def test_recommended_multiplier_picks_a_ladder_value_the_reader_accepts(self):
        """Each recommendation lies in the window and is accepted by the reader;
        a window holding no power of ten (Shin-Uchuu: 4.3e10 wide) still yields
        one, and a window that is empty is refused."""
        cases = (
            ("rank far below the reader default", 5, 5, 10**9),
            ("rank just below the default", 10**9 - 1, 5, 10**9),
            ("rank at the default", 10**9, 5, 10**10),
            ("no power of ten in the window", 12_834_657_129, 166_547_771, 2 * 10**10),
        )
        for label, max_rank, n_forests_total, expected in cases:
            with self.subTest(case=label):
                self.assertEqual(recommended_multiplier(max_rank, n_forests_total), expected)
                self.assertTrue(self.reader_accepts(expected, max_rank, n_forests_total))
        with self.assertRaisesRegex(ConverterError, "no valid identity multiplier"):
            recommended_multiplier(10**17, 10**3)

    def test_the_window_stays_usable_at_its_extremes(self):
        with self.subTest("empty-dataset sentinel"):
            # An all-empty dataset carries (n_forests_total 0, max_rank -1); the
            # reader requires multiplier > 0, so the window must not offer 0.
            lower, upper = identity_multiplier_window(max_rank=-1, n_forests_total=0)
            self.assertEqual(lower, 1)
            self.assertLessEqual(lower, upper)

        with self.subTest("ceiling below the reader default"):
            # Very many small forests put the ceiling under TREE_MUL_FAC (1e9).
            # Flooring the window at the default would report it empty and abort a
            # report the reader would have accepted.
            max_rank, n_forests_total = 5_000_000, 10**10
            lower, upper = identity_multiplier_window(max_rank, n_forests_total)
            self.assertLess(upper, 10**9)
            self.assertLessEqual(lower, upper)
            multiplier = recommended_multiplier(max_rank, n_forests_total)
            self.assertGreater(multiplier, max_rank)
            self.assertTrue(self.reader_accepts(multiplier, max_rank, n_forests_total))

        with self.subTest("window too narrow for a 1/2/5 value"):
            max_rank, n_forests_total = 12 * 10**17, 4
            lower, upper = identity_multiplier_window(max_rank, n_forests_total)
            multiplier = recommended_multiplier(max_rank, n_forests_total)
            self.assertEqual(multiplier, lower)
            self.assertLessEqual(multiplier, upper)


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

    def test_corruption_in_a_written_column_fails_the_stage(self):
        """A flipped value in an extra field and in a target-snapshot column is
        each caught by name."""
        cases = (
            (snapshot_h5_name(2), "SubHalfMass", 1),
            (snapshot_h5_name(0), "DescendantSnapshot", 0),
        )
        for file_name, dataset, row in cases:
            with self.subTest(dataset=dataset):
                writer = _CorruptingWriter(self.conv.sim_info, file_name, dataset, row)
                self.assert_refused_before_success_or_cleanup(writer, dataset)

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

    def test_a_wrong_chunk_shape_in_the_zero_halo_snapshot_fails_the_stage(self):
        """A zero-row scalar and a zero-row vec3 dataset re-created with other
        chunks are each refused."""
        cases = (
            ("Vmax", (65537,), r"snapshot_001\.h5: /halos/Vmax: chunks \(65537,\)"),
            ("Pos", (2, 2), r"snapshot_001\.h5: /halos/Pos: chunks \(2, 2\)"),
        )
        for dataset, chunks, message in cases:
            with self.subTest(dataset=dataset):
                mutate = _rechunk_empty(snapshot_h5_name(1), dataset, chunks)
                self.assert_refused_before_success_or_cleanup(
                    _MutatingWriter(self.conv.sim_info, mutate), message
                )

    def test_a_retry_after_refusal_writes_a_verified_dataset(self):
        writer = _CorruptingWriter(self.conv.sim_info, snapshot_h5_name(3), "Pos")
        self.assert_refused_before_success_or_cleanup(writer, "Pos")
        manifest = pipeline.run_write(self.conv.work, HorizontalV3Writer(self.conv.sim_info))
        self.assertTrue(manifest.is_complete("write"))

    def test_a_completed_write_is_not_claimed_by_different_physical_metadata(self):
        manifest = pipeline.run_write(self.conv.work, HorizontalV3Writer(self.conv.sim_info))
        writer = manifest.stage("write")["result"]["writer"]
        self.assertEqual(writer["format_version"], V3_FORMAT_VERSION)
        self.assertEqual(writer["header"]["particle_mass_msun_h"], 0.0325e10)
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
        info = fixtures.write_hdf5_source(
            str(directory),
            [
                {"forests": [fixtures.HDF5_MERGER_FOREST]},
                {"forests": [fixtures.hdf5_lone_forest(9, snap=5)]},
            ],
        )
        a_list = self.tmp / "h5.a_list"
        a_list.write_text("".join("{}\n".format(0.5 + 0.1 * i) for i in range(6)))
        work = self.tmp / "h5work"
        pipeline.initialize(
            work,
            fixtures.load_hdf5_schema(),
            {
                "info_path": info,
                "first_file": 0,
                "last_file": 1,
                "particle_mass": fixtures.UCHUU_PARTICLE_MASS,
                **({} if simulation_info is None else {"simulation_info": str(simulation_info)}),
            },
            a_list,
            ingest_max_rows=2,
            transpose_budget_bytes=fixtures.LITERAL_BUDGET,
        )
        pipeline.run_ingest(work)
        pipeline.run_transpose(work)
        return work, a_list

    def test_forests_hdf5_route_writes_an_adjacent_dataset_with_source_forest_ids(self):
        work, a_list = self.hdf5_conversion()
        sim_info = write_simulation_info(self.tmp / "sim.yaml", fixtures.UCHUU_PARTICLE_MASS)
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
                handle["ForestID"][...], [fixtures.HDF5_MERGER_FOREST["id"], 9]
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
        recorded = write_simulation_info(
            self.tmp / "sim.yaml", fixtures.UCHUU_PARTICLE_MASS, box=100.0
        )
        other = write_simulation_info(
            self.tmp / "other.yaml", fixtures.UCHUU_PARTICLE_MASS, box=62.5
        )
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
        schema = build_schema(load_column_map(fixtures.PROFILE_DIR / "consistent_trees_ascii.yaml"))
        work = self.tmp / "ascii_work"
        pipeline.initialize(
            work,
            schema,
            parameters,
            a_list,
            ingest_max_rows=4,
            transpose_budget_bytes=fixtures.LITERAL_BUDGET,
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
        # every a_list snapshot, the empty snapshot 0 included, carries the
        # run-scoped identity bounds of the five standard forests and the
        # physical headers of the fixture's simulation_info
        for snap, scale in enumerate(fixtures.A_LIST):
            with h5py.File(dataset / snapshot_h5_name(snap), "r") as handle:
                attrs = handle["header"].attrs
                self.assertEqual(int(attrs["format_version"]), V3_FORMAT_VERSION)
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
        result = run_battery_v3(dataset, a_list, manifest_path=manifest.path, budget_bytes=1 << 20)
        self.assertEqual([o.name for o in result.outcomes if o.status != "PASS"], [])

    def test_ascii_route_refuses_simulation_info_it_was_not_prepared_against(self):
        work, _a_list, _sim_info, _forests = self.ascii_conversion()
        other = write_simulation_info(self.tmp / "other.yaml", particle_mass=0.0325, box=50.0)
        with self.assertRaisesRegex(ConverterError, "different simulation_info"):
            pipeline.run_write(work, HorizontalV3Writer(other))


if __name__ == "__main__":
    unittest.main()
