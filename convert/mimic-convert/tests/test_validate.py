"""Unit tests for the format version 3 producer validation battery: it must
catch every deliberately corrupted dataset -- each format invariant and
battery check is violated once and the named check must FAIL -- tested
against a real conversion of a hand-derived gapped graph and against literal
datasets written here with plain h5py rather than by the writer. Its bounded
mode (``validate_bounded``) must reach the same verdicts as the external-sort
battery on every adjacent dataset, within the ceiling it states."""

import contextlib
import io
import json
import os
import re
import shutil
import sys
import tempfile
import tracemalloc
import unittest
from pathlib import Path
from types import SimpleNamespace
from typing import Tuple
from unittest import mock

import h5py
import numpy as np

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import convert_trees  # noqa: E402
import fixtures  # noqa: E402
import pipeline  # noqa: E402
import test_pipeline as literal  # noqa: E402
import validate_bounded  # noqa: E402
import validate_v3  # noqa: E402
from column_schema import build_schema, load_column_map  # noqa: E402
from conversion_manifest import ConversionManifest  # noqa: E402
from ctrees_parser import ConverterError  # noqa: E402
from hdf5_writer_v3 import HorizontalV3Writer, snapshot_h5_name  # noqa: E402
from test_fixups import capture_stderr  # noqa: E402
from test_hdf5_writer_v3 import make_v3_conversion  # noqa: E402


def outcome_map(outcomes):
    return {outcome.name: outcome for outcome in outcomes}


# ==========================================================================
# The external-sort battery
# ==========================================================================
#
# Two independent oracles:
#
# - a real conversion of test_pipeline's literal gapped L-Halo source
#   (make_v3_conversion), whose pristine dataset must pass and each of whose
#   contract components is corrupted independently, with the named check
#   required to FAIL;
# - LITERAL datasets written here with plain h5py from hand-specified arrays
#   (write_literal_v3), not by the writer, for graphs a valid source cannot
#   produce: pure NextProgenitor and FoF cycles that satisfy every per-link
#   and coverage rule, and a halo its progenitor chain misses.

V3_BUDGET = 1 << 20

#: The minimal v3 payload declarations of a literal dataset.
LITERAL_DECLARATIONS = {
    "Len": ("int", "particles", "none", "Particle count"),
    "SnapNum": ("int", "dimensionless", "none", "Snapshot index"),
    "M_Crit200": ("float", "1e10 Msun/h", "carried", "Halo mass"),
    "Pos": ("vec3_float", "Mpc/h", "carried", "Position"),
    "Vel": ("vec3_float", "km/s", "none", "Velocity"),
    "Spin": ("vec3_float", "Mpc/h km/s", "carried", "Specific angular momentum"),
    "VelDisp": ("float", "km/s", "none", "Velocity dispersion"),
    "Vmax": ("float", "km/s", "none", "Maximum circular velocity"),
    "MostBoundID": ("long long", "dimensionless", "none", "Catalog identifier"),
}
_LITERAL_FIXED = {
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


def write_literal_v3(directory, snapshots, sidecar, *, links_adjacent, max_rank, scales=None):
    """Write a v3 dataset from hand-specified columns with plain h5py.

    ``snapshots`` is one mapping per snapshot of column name -> list; the
    link columns are ``(row, snapshot)`` pairs or ``None`` (or, for a large
    dataset, one tuple of a row array and a snapshot array, -1 for none), the
    rest plain values, and unspecified payload is a literal default.
    ``sidecar`` maps the three sidecar names to lists.
    """
    directory = Path(directory)
    directory.mkdir(parents=True, exist_ok=True)
    scales = scales or [0.25 * (i + 1) for i in range(len(snapshots))]
    n_forests = len(sidecar["ForestID"])
    for snap, spec in enumerate(snapshots):
        n = len(spec["SourceHaloID"])
        columns = {name: np.full(n, -1, dtype=dtype) for name, dtype in _LITERAL_FIXED.items()}
        for link, column in (
            ("Descendant", "DescendantSnapshot"),
            ("FirstProgenitor", "FirstProgenitorSnapshot"),
            ("NextProgenitor", "NextProgenitorSnapshot"),
        ):
            if isinstance(spec.get(link), tuple):
                columns[link][:], columns[column][:] = spec[link]
                continue
            for row, target in enumerate(spec.get(link, [None] * n)):
                if target is not None:
                    columns[link][row], columns[column][row] = target
        for name in (
            "FirstHaloInFOFgroup",
            "NextHaloInFOFgroup",
            "SourceHaloID",
            "ForestIndex",
            "HaloRankInForest",
        ):
            if name in spec:
                columns[name][:] = spec[name]
        if "FirstHaloInFOFgroup" not in spec:
            columns["FirstHaloInFOFgroup"][:] = np.arange(n)
        payload = {
            "Len": np.full(n, 10, dtype="<i4"),
            "SnapNum": np.full(n, snap, dtype="<i4"),
            "M_Crit200": np.full(n, 1.5, dtype="<f4"),
            "Pos": np.ones((n, 3), dtype="<f4"),
            "Vel": np.ones((n, 3), dtype="<f4"),
            "Spin": np.ones((n, 3), dtype="<f4"),
            "VelDisp": np.full(n, 2.0, dtype="<f4"),
            "Vmax": np.full(n, 3.0, dtype="<f4"),
            "MostBoundID": np.arange(n, dtype="<i8") - 1,
        }
        for name in payload:
            if name in spec:
                payload[name] = np.asarray(spec[name], dtype=payload[name].dtype)
        with h5py.File(
            directory / "snapshot_{:03d}.h5".format(snap), "w-", libver="latest"
        ) as handle:
            header = handle.create_group("header")
            for name, value, dtype in (
                ("format_version", 3, np.int32),
                ("links_adjacent", links_adjacent, np.int32),
                ("scale_factor", scales[snap], np.float64),
                ("snapshot_number", snap, np.int32),
                ("n_halos", n, np.int64),
                ("n_forests_total", n_forests, np.int64),
                ("max_halo_rank_in_forest", max_rank, np.int64),
                ("box_size_mpc_h", 100.0, np.float64),
                ("particle_mass_msun_h", 3.25e8, np.float64),
                ("omega_matter", 0.3, np.float64),
                ("omega_lambda", 0.7, np.float64),
                ("hubble_h", 0.7, np.float64),
            ):
                header.attrs.create(name, value, dtype=dtype)
            header.attrs.create(
                "source_format", np.bytes_(b"lhalo_binary"), dtype=h5py.string_dtype("ascii", 32)
            )
            header.attrs.create(
                "column_mapping_sha256", np.bytes_(b"0" * 64), dtype=h5py.string_dtype("ascii", 64)
            )
            halos = handle.create_group("halos")
            for name, values in list(columns.items()) + list(payload.items()):
                vec = values.ndim == 2
                halos.create_dataset(
                    name,
                    data=values,
                    chunks=(65536, 3) if vec else (65536,),
                    maxshape=(None, 3) if vec else (None,),
                )
            schema = handle.create_group("schema")
            for name, values in LITERAL_DECLARATIONS.items():
                group = schema.create_group(name)
                for key, value in zip(("type", "units", "h_convention", "description"), values):
                    group.attrs.create(key, value, dtype=h5py.string_dtype("utf-8"))
    with h5py.File(directory / "forests.h5", "w-", libver="latest") as handle:
        for name in ("ForestID", "SourceFileOrdinal", "SourceUnitOrdinal"):
            handle.create_dataset(
                name, data=np.asarray(sidecar[name], dtype="<i8"), chunks=(65536,), maxshape=(None,)
            )
    a_list = directory.parent / (directory.name + ".a_list")
    a_list.write_text("".join("{!r}\n".format(scale) for scale in scales))
    return a_list


def literal_graph():
    """One forest over three snapshots, hand-specified.

    c0 (snap 2) has four progenitors in one chain -- b0 (snap 1), a0 (snap 0,
    a gap), a2 (snap 0, a gap), b1 (snap 1) -- so NextProgenitor steps both
    backwards and forwards in time; c0 heads a three-member FoF group. Ranks
    follow SourceHaloID, which is the unit-forest order.
    """
    snap0 = {
        "SourceHaloID": [3, 5],
        "ForestIndex": [0, 0],
        "HaloRankInForest": [2, 4],
        "Descendant": [(0, 2), (0, 2)],
        "NextProgenitor": [(1, 0), (1, 1)],
    }
    snap1 = {
        "SourceHaloID": [2, 4],
        "ForestIndex": [0, 0],
        "HaloRankInForest": [1, 3],
        "Descendant": [(0, 2), (0, 2)],
        "NextProgenitor": [(0, 0), None],
    }
    snap2 = {
        "SourceHaloID": [1, 6, 7],
        "ForestIndex": [0, 0, 0],
        "HaloRankInForest": [0, 5, 6],
        "FirstProgenitor": [(0, 1), None, None],
        "FirstHaloInFOFgroup": [0, 0, 0],
        "NextHaloInFOFgroup": [1, 2, -1],
    }
    return [snap0, snap1, snap2], {
        "ForestID": [0],
        "SourceFileOrdinal": [0],
        "SourceUnitOrdinal": [0],
    }


class V3BatteryCase(unittest.TestCase):
    """One pristine conversion; every corruption works on its own copy."""

    @classmethod
    def setUpClass(cls):
        cls._tmp = tempfile.TemporaryDirectory(prefix="v3_battery_")
        cls.conv = make_v3_conversion(cls._tmp.name)

    @classmethod
    def tearDownClass(cls):
        cls._tmp.cleanup()

    def copy(self) -> Path:
        target = Path(tempfile.mkdtemp(dir=self._tmp.name)) / "dataset"
        shutil.copytree(self.conv.dataset, target)
        return target

    def copy_workdir(self) -> Tuple[Path, Path]:
        """A copy of the whole conversion workdir and its dataset directory,
        for ``convert_trees.py validate --workdir``."""
        work = Path(tempfile.mkdtemp(dir=self._tmp.name)) / "work"
        shutil.copytree(self.conv.work, work)
        manifest = ConversionManifest.load(work)
        return work, manifest.artifact_path(manifest.stage("write")["directory"])

    def validate_cli(self, work) -> Tuple[int, str]:
        """``convert_trees.py validate --workdir work``: exit code and stdout."""
        out = io.StringIO()
        with contextlib.redirect_stdout(out), capture_stderr():
            code = convert_trees.main(
                ["validate", "--workdir", str(work), "--memory-budget-mb", "1"]
            )
        return code, out.getvalue()

    def run_v3(self, directory, manifest=True, **kwargs):
        kwargs.setdefault("budget_bytes", V3_BUDGET)
        result = validate_v3.run_battery_v3(
            directory,
            kwargs.pop("a_list", self.conv.a_list),
            manifest_path=self.conv.manifest.path if manifest else None,
            **kwargs,
        )
        return result, outcome_map(result.outcomes)

    def assert_fails(self, directory, check, fragment=None, **kwargs):
        result, outcomes = self.run_v3(directory, **kwargs)
        self.assertEqual(outcomes[check].status, "FAIL", outcomes[check].line())
        if fragment is not None:
            self.assertIn(fragment, outcomes[check].detail)
        self.assertTrue(result.failed)
        return outcomes

    def edit(self, directory, name, dataset, row, value):
        with h5py.File(directory / name, "r+") as handle:
            handle["halos"][dataset][row] = value

    def edit_header(self, directory, name, attr, value, dtype=None):
        names = [name] if name else sorted(p.name for p in directory.glob("snapshot_*.h5"))
        for file_name in names:
            with h5py.File(directory / file_name, "r+") as handle:
                attrs = handle["header"].attrs
                attrs.create(attr, value, dtype=dtype or attrs[attr].dtype)

    def replace_dataset(self, directory, name, dataset, dtype=None, **options):
        with h5py.File(directory / name, "r+") as handle:
            halos = handle["halos"]
            values = halos[dataset][...]
            del halos[dataset]
            vec = values.ndim == 2
            options.setdefault("chunks", (65536, 3) if vec else (65536,))
            options.setdefault("maxshape", (None, 3) if vec else (None,))
            halos.create_dataset(dataset, data=values.astype(dtype or values.dtype), **options)


class TestV3BatteryPristine(V3BatteryCase):
    def test_every_check_passes_on_the_written_dataset(self):
        result, outcomes = self.run_v3(self.conv.dataset)
        self.assertEqual(list(outcomes), list(validate_v3.V3_CHECKS))
        self.assertEqual(
            [o.line() for o in result.outcomes if o.status != "PASS"], [], "pristine dataset"
        )

    def test_measurements_match_the_hand_derived_graph(self):
        result, _ = self.run_v3(self.conv.dataset)
        m = result.measurements
        self.assertEqual(m["format_versions"], [3])
        self.assertEqual(m["snapshot_counts"], [2, 0, 3, 3])
        # test_pipeline.EXPECTED: snap-0 halos descend to snapshots 3 and 2
        self.assertEqual(m["gapped_descendants"], 2)
        self.assertEqual(m["max_descendant_span"], 3)
        self.assertEqual(m["links_adjacent_measured"], 0)
        self.assertEqual(
            m["non_null_links"],
            {
                "Descendant": 4,
                "FirstProgenitor": 3,
                "NextProgenitor": 1,
                "FirstHaloInFOFgroup": 0,
                "NextHaloInFOFgroup": 0,
            },
        )
        # tree C's r1 (snapshot 2) has its progenitor at snapshot 0
        self.assertEqual(m["gapped_first_progenitors"], 1)
        self.assertEqual(m["next_progenitors_off_owner_snapshot"], 1)
        self.assertEqual((m["min_source_halo_id"], m["max_source_halo_id"]), (1, 8))
        self.assertEqual((m["max_forest_index"], m["max_halo_rank_in_forest"]), (2, 3))
        self.assertLessEqual(m["peak_resident_bytes"], V3_BUDGET)

    def test_a_copy_with_identical_bytes_is_still_bound_to_its_conversion(self):
        _result, outcomes = self.run_v3(self.copy())
        self.assertEqual(outcomes["manifest-binding"].status, "PASS")

    def test_the_cli_validates_the_dataset_and_exits_zero(self):
        work, _dataset = self.copy_workdir()
        code, out = self.validate_cli(work)
        self.assertEqual(code, 0, out)
        self.assertIn("validation: PASS", out)


class TestV3BatteryStructure(V3BatteryCase):
    def test_an_extra_root_object_fails_object_set(self):
        directory = self.copy()
        with h5py.File(directory / "snapshot_002.h5", "r+") as handle:
            handle.create_group("extra")
        self.assert_fails(directory, "object-set", "root object set")

    def test_a_soft_link_in_halos_fails_object_set(self):
        directory = self.copy()
        with h5py.File(directory / "snapshot_003.h5", "r+") as handle:
            del handle["halos"]["Vmax"]
            handle["halos"]["Vmax"] = h5py.SoftLink("/halos/VelDisp")
        self.assert_fails(directory, "object-set", "SoftLink")

    def test_an_external_link_in_schema_fails_object_set(self):
        directory = self.copy()
        with h5py.File(directory / "snapshot_000.h5", "r+") as handle:
            del handle["schema"]["PosX"]
            handle["schema"]["PosX"] = h5py.ExternalLink("snapshot_002.h5", "/schema/PosX")
        self.assert_fails(directory, "object-set", "ExternalLink")

    def test_a_narrowed_link_dtype_fails_object_set(self):
        directory = self.copy()
        self.replace_dataset(directory, "snapshot_002.h5", "Descendant", dtype="<i4")
        self.assert_fails(directory, "object-set", "Descendant dtype")

    def test_a_big_endian_payload_fails_object_set(self):
        directory = self.copy()
        self.replace_dataset(directory, "snapshot_002.h5", "M_Crit200", dtype=">f4")
        self.assert_fails(directory, "object-set", "little-endian")

    def test_a_compressed_extra_fails_object_set(self):
        directory = self.copy()
        self.replace_dataset(directory, "snapshot_003.h5", "SubHalfMass", compression="gzip")
        self.assert_fails(directory, "object-set", "compressed")

    def test_a_chunk_row_count_within_the_ceiling_passes_object_set(self):
        directory = self.copy()
        self.replace_dataset(directory, "snapshot_002.h5", "Vmax", chunks=(2,))
        self.replace_dataset(directory, "snapshot_002.h5", "Pos", chunks=(1, 3))
        _, outcomes = self.run_v3(directory)
        self.assertEqual(outcomes["object-set"].status, "PASS", outcomes["object-set"].line())

    def test_a_scalar_chunk_above_the_ceiling_fails_object_set(self):
        directory = self.copy()
        self.replace_dataset(directory, "snapshot_002.h5", "Vmax", chunks=(65537,))
        self.assert_fails(directory, "object-set", "Vmax chunks (65537,) violate the rule")

    def test_a_vector_chunk_of_the_wrong_width_fails_object_set(self):
        directory = self.copy()
        self.replace_dataset(directory, "snapshot_002.h5", "Pos", chunks=(2, 2))
        self.assert_fails(directory, "object-set", "Pos chunks (2, 2) violate the rule")

    def test_a_declaration_with_the_wrong_type_for_its_dataset_fails_object_set(self):
        directory = self.copy()
        with h5py.File(directory / "snapshot_003.h5", "r+") as handle:
            handle["schema"]["SubhaloIndex"].attrs.create(
                "type", "long long", dtype=h5py.string_dtype("utf-8")
            )
        self.assert_fails(directory, "object-set", "SubhaloIndex dtype")

    def test_an_undeclared_extra_dataset_fails_object_set(self):
        directory = self.copy()
        with h5py.File(directory / "snapshot_002.h5", "r+") as handle:
            handle["halos"].create_dataset(
                "Surprise", data=np.zeros(3, dtype="<f4"), chunks=(65536,), maxshape=(None,)
            )
        self.assert_fails(directory, "object-set", "Surprise")

    def test_a_schema_attribute_that_is_missing_fails_object_set(self):
        directory = self.copy()
        with h5py.File(directory / "snapshot_000.h5", "r+") as handle:
            del handle["schema"]["Len"].attrs["description"]
        self.assert_fails(directory, "object-set", "/schema/Len")

    def test_an_extra_sidecar_dataset_fails_sidecar_object_set(self):
        directory = self.copy()
        with h5py.File(directory / "forests.h5", "r+") as handle:
            handle.create_dataset("ForestNhalos", data=np.zeros(3, dtype="<i8"))
        self.assert_fails(directory, "sidecar-object-set")

    def test_a_missing_or_stray_file_fails_file_set(self):
        directory = self.copy()
        (directory / "snapshot_001.h5").unlink()
        self.assert_fails(directory, "file-set", "missing")
        directory = self.copy()
        (directory / "notes.txt").write_text("not part of the dataset")
        self.assert_fails(directory, "file-set", "unexpected")

    def test_a_missing_sidecar_fails_file_set(self):
        directory = self.copy()
        (directory / "forests.h5").unlink()
        outcomes = self.assert_fails(directory, "file-set", "forests.h5")
        self.assertEqual(outcomes["sidecar-object-set"].status, "SKIP")

    def test_a_missing_header_attribute_fails_object_set(self):
        directory = self.copy()
        with h5py.File(directory / "snapshot_002.h5", "r+") as handle:
            del handle["header"].attrs["hubble_h"]
        self.assert_fails(directory, "object-set", "missing ['hubble_h']")

    def test_a_header_attribute_of_the_wrong_type_fails_object_set(self):
        directory = self.copy()
        self.edit_header(directory, "snapshot_002.h5", "n_halos", 3, dtype=np.int32)
        self.assert_fails(directory, "object-set", "attribute n_halos has dtype int32")

    def test_a_structural_failure_skips_every_semantic_check(self):
        directory = self.copy()
        with h5py.File(directory / "snapshot_002.h5", "r+") as handle:
            del handle["header"].attrs["hubble_h"]
        outcomes = self.assert_fails(directory, "object-set")
        semantic = validate_v3.V3_CHECKS[validate_v3.V3_CHECKS.index("header-values") :]
        for name in ("schema-binding",) + semantic:
            self.assertEqual(outcomes[name].status, "SKIP", name)
            self.assertEqual(
                outcomes[name].detail, "structural conformance failed; semantics not trusted"
            )

    def test_an_unreadable_file_fails_object_set_without_raising(self):
        directory = self.copy()
        (directory / "snapshot_002.h5").write_bytes(b"not an HDF5 file")
        self.assert_fails(directory, "object-set", "snapshot_002.h5: unreadable as HDF5")

    def test_a_scale_offset_filter_fails_object_set(self):
        directory = self.copy()
        self.replace_dataset(directory, "snapshot_002.h5", "Len", scaleoffset=0)
        self.assert_fails(directory, "object-set", "scale-offset")

    def test_a_contiguous_sidecar_dataset_fails_sidecar_object_set(self):
        directory = self.copy()
        with h5py.File(directory / "forests.h5", "r+") as handle:
            values = handle["ForestID"][...]
            del handle["ForestID"]
            handle.create_dataset("ForestID", data=values)
        self.assert_fails(directory, "sidecar-object-set", "/ForestID chunks None")


class TestV3BatteryEmptySnapshotSchema(V3BatteryCase):
    def test_changed_units_in_the_empty_snapshot_fail_schema_binding(self):
        directory = self.copy()
        with h5py.File(directory / "snapshot_001.h5", "r+") as handle:
            handle["schema"]["M_Crit200"].attrs.create(
                "units", "Msun/h", dtype=h5py.string_dtype("utf-8")
            )
        outcomes = self.assert_fails(directory, "schema-binding", "snapshot_001.h5")
        self.assertEqual(outcomes["object-set"].status, "PASS")

    def test_a_dropped_extra_in_the_empty_snapshot_fails_schema_binding(self):
        directory = self.copy()
        with h5py.File(directory / "snapshot_001.h5", "r+") as handle:
            del handle["schema"]["PosX"]
            del handle["halos"]["PosX"]
        outcomes = self.assert_fails(directory, "schema-binding", "PosX")
        self.assertEqual(outcomes["object-set"].status, "PASS")
        self.assertEqual(outcomes["row-values"].status, "SKIP")

    def test_a_schema_that_differs_only_from_the_manifest_fails_schema_binding(self):
        directory = self.copy()
        for name in sorted(p.name for p in directory.glob("snapshot_*.h5")):
            with h5py.File(directory / name, "r+") as handle:
                handle["schema"]["Vmax"].attrs.create(
                    "description", "edited", dtype=h5py.string_dtype("utf-8")
                )
        self.assert_fails(directory, "schema-binding", "Vmax")


class TestV3BatteryHeaders(V3BatteryCase):
    def test_a_version_2_file_among_version_3_files_is_refused(self):
        directory = self.copy()
        self.edit_header(directory, "snapshot_002.h5", "format_version", 2)
        self.assert_fails(directory, "header-values", "format_version 2")
        work, dataset = self.copy_workdir()
        self.edit_header(dataset, "snapshot_002.h5", "format_version", 2)
        code, out = self.validate_cli(work)
        self.assertEqual(code, 1, out)
        self.assertIn("header-values: FAIL", out)
        self.assertIn("format_version 2", out)
        self.assertIn("validation: FAIL", out)

    def test_every_version_but_3_fails_header_values(self):
        for version in (1, 2, 4):
            directory = self.copy()
            self.edit_header(directory, None, "format_version", version)
            self.assert_fails(directory, "header-values", "format_version {}".format(version))

    def test_a_wrong_snapshot_number_fails_header_values(self):
        directory = self.copy()
        self.edit_header(directory, "snapshot_002.h5", "snapshot_number", 3)
        self.assert_fails(directory, "header-values", "snapshot_number 3 != filename index 2")

    def test_a_scale_factor_other_than_the_a_list_fails_header_values(self):
        directory = self.copy()
        self.edit_header(directory, "snapshot_002.h5", "scale_factor", 0.123)
        self.assert_fails(directory, "header-values", "scale_factor 0.123 != a_list[2]")

    def test_an_n_halos_other_than_the_rows_fails_header_values(self):
        directory = self.copy()
        self.edit_header(directory, "snapshot_002.h5", "n_halos", 4)
        self.assert_fails(directory, "header-values", "has 3 rows, header n_halos is 4")

    def test_a_stamped_links_adjacent_fails_the_measurement(self):
        directory = self.copy()
        self.edit_header(directory, None, "links_adjacent", 1)
        self.assert_fails(directory, "links-adjacent", "measures 0 (2 gapped")

    def test_links_adjacent_differing_between_files_fails_run_scoped_headers(self):
        directory = self.copy()
        self.edit_header(directory, "snapshot_003.h5", "links_adjacent", 1)
        self.assert_fails(directory, "run-scoped-headers", "links_adjacent")

    def test_a_source_format_other_than_the_conversion_fails_header_values(self):
        directory = self.copy()
        self.edit_header(
            directory,
            None,
            "source_format",
            np.bytes_(b"consistent_trees_hdf5"),
            dtype=h5py.string_dtype("ascii", 32),
        )
        self.assert_fails(directory, "header-values", "source_format")

    def test_a_digest_other_than_the_conversion_fails_header_values(self):
        directory = self.copy()
        self.edit_header(
            directory,
            None,
            "column_mapping_sha256",
            np.bytes_(b"f" * 64),
            dtype=h5py.string_dtype("ascii", 64),
        )
        self.assert_fails(directory, "header-values", "column_mapping_sha256")

    def test_a_variable_length_source_format_fails_object_set(self):
        directory = self.copy()
        self.edit_header(
            directory, "snapshot_000.h5", "source_format", "lhalo_binary", dtype=h5py.string_dtype()
        )
        self.assert_fails(directory, "object-set", "fixed ASCII")

    def test_physical_headers_other_than_the_writer_recorded_fail(self):
        directory = self.copy()
        self.edit_header(directory, None, "box_size_mpc_h", 62.5)
        self.assert_fails(directory, "run-scoped-headers", "box_size_mpc_h")

    def test_a_wrong_max_rank_fails_identity(self):
        directory = self.copy()
        self.edit_header(directory, None, "max_halo_rank_in_forest", 4)
        self.assert_fails(directory, "identity", "max_halo_rank_in_forest 4")


class TestV3BatteryRows(V3BatteryCase):
    def test_a_wrong_snapnum_fails_row_values(self):
        directory = self.copy()
        self.edit(directory, "snapshot_002.h5", "SnapNum", 1, 3)
        self.assert_fails(directory, "row-values", "SnapNum")

    def test_rows_out_of_sourcehaloid_order_fail_row_values(self):
        directory = self.copy()
        self.edit(directory, "snapshot_003.h5", "SourceHaloID", 1, 7)
        self.assert_fails(directory, "row-values", "ascending")

    def test_a_negative_len_fails_len_nonnegative(self):
        directory = self.copy()
        self.edit(directory, "snapshot_000.h5", "Len", 1, -1)
        self.assert_fails(directory, "len-nonnegative", "negative Len")

    def test_len_zero_halos_are_counted_not_failed(self):
        directory = self.copy()
        self.edit(directory, "snapshot_002.h5", "Len", 0, 0)
        zeros = 0
        for path in sorted(directory.glob("snapshot_*.h5")):
            with h5py.File(path, "r") as handle:
                zeros += int(np.count_nonzero(handle["halos"]["Len"][...] == 0))
        self.assertGreater(zeros, 0)
        _result, outcomes = self.run_v3(directory)
        self.assertEqual(outcomes["len-nonnegative"].status, "PASS")
        self.assertEqual(outcomes["len-nonnegative"].detail, "{} Len==0 halo(s)".format(zeros))

    def test_a_nan_in_an_extra_fails_field_finiteness(self):
        directory = self.copy()
        self.edit(directory, "snapshot_003.h5", "M_TopHat", 2, np.nan)
        self.assert_fails(directory, "field-finiteness", "M_TopHat")

    def test_an_infinity_in_a_vector_component_fails_field_finiteness(self):
        directory = self.copy()
        with h5py.File(directory / "snapshot_002.h5", "r+") as handle:
            handle["halos"]["Vel"][0, 1] = np.inf
        self.assert_fails(directory, "field-finiteness", "Vel")

    def test_a_duplicated_sourcehaloid_fails_source_key_coverage(self):
        directory = self.copy()
        # snapshot 3 holds ids 1, 5, 6; 4 already belongs to snapshot 2
        self.edit(directory, "snapshot_003.h5", "SourceHaloID", 1, 4)
        self.assert_fails(directory, "source-key-coverage", "duplicated SourceHaloID")

    def test_an_id_outside_the_inventory_fails_source_key_coverage(self):
        directory = self.copy()
        self.edit(directory, "snapshot_003.h5", "SourceHaloID", 2, 9)
        self.assert_fails(directory, "source-key-coverage", "[1, 8]")


class TestV3BatteryLinks(V3BatteryCase):
    def test_a_null_index_with_a_target_snapshot_fails_link_targets(self):
        directory = self.copy()
        self.edit(directory, "snapshot_002.h5", "DescendantSnapshot", 1, 3)
        self.assert_fails(directory, "link-targets", "null-ness")

    def test_a_target_snapshot_mismatch_fails_topology_closure(self):
        # snapshot_000 row 0 descends to snapshot 3 row 0; pointing it at
        # snapshot 2 row 0 keeps every range valid but breaks the chain
        directory = self.copy()
        self.edit(directory, "snapshot_000.h5", "DescendantSnapshot", 0, 2)
        outcomes = self.assert_fails(directory, "topology-closure", "NextProgenitor")
        self.assertEqual(outcomes["link-targets"].status, "PASS")

    def test_a_row_beyond_its_target_snapshot_fails_link_targets(self):
        directory = self.copy()
        self.edit(directory, "snapshot_000.h5", "Descendant", 1, 3)
        self.assert_fails(directory, "link-targets", "outside their target")

    def test_a_backward_descendant_fails_link_targets(self):
        directory = self.copy()
        self.edit(directory, "snapshot_002.h5", "DescendantSnapshot", 0, 0)
        self.assert_fails(directory, "link-targets", "not strictly later")

    def test_a_target_snapshot_outside_the_dataset_fails_link_targets(self):
        directory = self.copy()
        self.edit(directory, "snapshot_003.h5", "FirstProgenitorSnapshot", 0, 9)
        self.assert_fails(directory, "link-targets", "outside the dataset")

    def test_a_next_progenitor_on_a_halo_without_a_descendant_fails_link_targets(self):
        directory = self.copy()
        # snapshot 3 is the last: none of its halos has a descendant
        self.edit(directory, "snapshot_003.h5", "NextProgenitor", 0, 1)
        self.edit(directory, "snapshot_003.h5", "NextProgenitorSnapshot", 0, 3)
        self.assert_fails(
            directory, "link-targets", "NextProgenitor(s) on a halo with no Descendant"
        )

    def test_a_first_progenitor_naming_the_wrong_halo_fails_topology_closure(self):
        directory = self.copy()
        # snapshot 3 row 0's main progenitor is snapshot 2 row 0; row 1 is not
        # its progenitor at all
        self.edit(directory, "snapshot_003.h5", "FirstProgenitor", 0, 1)
        self.assert_fails(directory, "topology-closure", "FirstProgenitor")

    def test_a_fof_member_of_another_group_fails_topology_closure(self):
        directory = self.copy()
        # snapshot 2 row 1 is row 0's FoF member; declare row 2 its central
        self.edit(directory, "snapshot_002.h5", "FirstHaloInFOFgroup", 1, 2)
        self.assert_fails(directory, "topology-closure", "different FoF group")

    def test_a_cross_forest_link_fails_topology_closure(self):
        directory = self.copy()
        self.edit(directory, "snapshot_002.h5", "ForestIndex", 1, 1)
        outcomes = self.assert_fails(directory, "topology-closure", "crossing forests")
        self.assertEqual(outcomes["identity"].status, "FAIL")

    def test_link_failures_skip_the_walk_but_still_report_identity(self):
        directory = self.copy()
        self.edit(directory, "snapshot_002.h5", "FirstHaloInFOFgroup", 0, -1)
        _result, outcomes = self.run_v3(directory)
        self.assertEqual(outcomes["link-targets"].status, "FAIL")
        self.assertEqual(outcomes["topology-closure"].status, "SKIP")
        self.assertEqual(outcomes["chain-cycles"].status, "SKIP")
        self.assertEqual(outcomes["identity"].status, "PASS")


class TestV3BatteryIdentityAndSidecar(V3BatteryCase):
    def test_a_multiplier_not_above_the_max_rank_fails_header_bounds(self):
        # the pristine dataset's max_halo_rank_in_forest is 3
        self.assert_fails(
            self.conv.dataset,
            "header-bounds",
            "multiplier 3 does not exceed max_halo_rank_in_forest 3",
            multiplier=3,
        )

    def test_a_multiplier_overflowing_int64_fails_header_bounds(self):
        # three forests: 2**62 x (3 + 1) is past INT64_MAX
        self.assert_fails(
            self.conv.dataset,
            "header-bounds",
            "multiplier {} x (n_forests_total 3 + 1) overflows int64".format(2**62),
            multiplier=2**62,
        )

    def test_a_rank_gap_fails_identity(self):
        directory = self.copy()
        # tree C (forest 2) holds ranks 0, 1, 2 at SourceHaloID 6, 7, 8
        self.edit(directory, "snapshot_002.h5", "HaloRankInForest", 2, 5)
        self.assert_fails(directory, "identity", "rank density")

    def test_a_forestindex_outside_n_forests_total_fails_identity(self):
        directory = self.copy()
        self.edit(directory, "snapshot_003.h5", "ForestIndex", 1, 3)
        self.assert_fails(directory, "identity", "outside [0, n_forests_total)")

    def test_ids_out_of_source_order_fail_identity(self):
        directory = self.copy()
        # swap the ranks of tree A's rows 1 and 3 (both at snapshot 2)
        self.edit(directory, "snapshot_002.h5", "HaloRankInForest", 0, 3)
        self.edit(directory, "snapshot_002.h5", "HaloRankInForest", 1, 1)
        self.assert_fails(directory, "identity", "source order")

    def test_a_sidecar_forest_id_that_is_not_dense_fails_sidecar_content(self):
        directory = self.copy()
        with h5py.File(directory / "forests.h5", "r+") as handle:
            handle["ForestID"][1] = 7
        self.assert_fails(directory, "sidecar-content", "dense run forest number")

    def test_sidecar_ordinals_out_of_inventory_order_fail_sidecar_content(self):
        directory = self.copy()
        with h5py.File(directory / "forests.h5", "r+") as handle:
            handle["SourceUnitOrdinal"][1] = 2
        self.assert_fails(directory, "sidecar-content", "inventory order")

    def test_a_sidecar_shorter_than_n_forests_total_fails_sidecar_content(self):
        directory = self.copy()
        with h5py.File(directory / "forests.h5", "r+") as handle:
            for name in ("ForestID", "SourceFileOrdinal", "SourceUnitOrdinal"):
                handle[name].resize((2,))
        self.assert_fails(directory, "sidecar-content", "holds 2 forests")


class TestV3BatteryBinding(V3BatteryCase):
    def test_tampered_content_fails_manifest_binding(self):
        directory = self.copy()
        self.edit(directory, "snapshot_003.h5", "Vmax", 0, 99.0)
        self.assert_fails(directory, "manifest-binding", "differs from the registered")

    def test_another_a_list_fails_binding_and_header_values(self):
        directory = self.copy()
        other = Path(self._tmp.name) / "other.a_list"
        other.write_text("0.25\n0.5\n0.75\n0.99\n")
        outcomes = self.assert_fails(directory, "manifest-binding", "a_list", a_list=other)
        self.assertEqual(outcomes["header-values"].status, "FAIL")

    def edited_manifest(self, edit):
        work = Path(tempfile.mkdtemp(dir=self._tmp.name)) / "work"
        shutil.copytree(self.conv.work, work)
        path = work / "manifest.json"
        data = json.loads(path.read_text())
        edit(data)
        path.write_text(json.dumps(data))
        manifest = ConversionManifest.load(work)
        return work, manifest

    def test_ingest_counts_that_differ_fail_count_conservation(self):
        work, manifest = self.edited_manifest(
            lambda data: data["stages"]["ingest"]["result"].update(snapshot_counts=[2, 1, 2, 3])
        )
        directory = manifest.artifact_path(manifest.stage("write")["directory"])
        outcomes = outcome_map(
            validate_v3.run_battery_v3(
                directory, self.conv.a_list, manifest_path=manifest.path, budget_bytes=V3_BUDGET
            ).outcomes
        )
        self.assertEqual(outcomes["count-conservation"].status, "FAIL")
        self.assertIn("ingest", outcomes["count-conservation"].detail)

    def test_an_inventory_selecting_more_halos_fails_conservation_and_coverage(self):
        def edit(data):
            data["sources"]["inventory"]["selected_halos"] = 9
            data["sources"]["inventory"]["total_halos"] = 9

        work, manifest = self.edited_manifest(edit)
        directory = manifest.artifact_path(manifest.stage("write")["directory"])
        outcomes = outcome_map(
            validate_v3.run_battery_v3(
                directory, self.conv.a_list, manifest_path=manifest.path, budget_bytes=V3_BUDGET
            ).outcomes
        )
        self.assertEqual(outcomes["count-conservation"].status, "FAIL")
        self.assertEqual(outcomes["source-key-coverage"].status, "FAIL")

    def test_a_manifest_whose_write_is_incomplete_fails_binding(self):
        work, manifest = self.edited_manifest(
            lambda data: data["stages"]["write"].update(status="failed")
        )
        directory = self.copy()
        outcomes = outcome_map(
            validate_v3.run_battery_v3(
                directory, self.conv.a_list, manifest_path=manifest.path, budget_bytes=V3_BUDGET
            ).outcomes
        )
        self.assertEqual(outcomes["manifest-binding"].status, "FAIL")

    def test_without_a_manifest_binding_and_conservation_are_skipped_not_passed(self):
        _result, outcomes = self.run_v3(self.copy(), manifest=False)
        self.assertEqual(outcomes["manifest-binding"].status, "SKIP")
        self.assertEqual(outcomes["count-conservation"].status, "SKIP")


#: The single-snapshot links and the link/target-snapshot pairs of a v3 slab.
_SAME_SNAPSHOT_LINKS = ("FirstHaloInFOFgroup", "NextHaloInFOFgroup")
_QUALIFIED_LINKS = (
    ("Descendant", "DescendantSnapshot"),
    ("FirstProgenitor", "FirstProgenitorSnapshot"),
    ("NextProgenitor", "NextProgenitorSnapshot"),
)


def source_file_forests():
    """The canned ASCII forests in descending forest id, so the source file's
    forest order is the reverse of the ascending-id ForestIndex enumeration."""
    return list(reversed(fixtures.standard_forests()))


def make_v3_ascii_conversion(root, forests):
    """``forests`` written in the given order to one Consistent-Trees ASCII
    file, then initialize, ingest, transpose and the v3 write stage."""
    root = Path(root)
    src = root / "src"
    src.mkdir(parents=True)
    tree = fixtures.write_ctrees_file(src / "tree_0_0_0.dat", fixtures.all_trees(forests))
    sim_info = fixtures.write_simulation_info(src / "simulation_info.yaml")
    parameters = {
        "tree_files": [str(tree)],
        "forests_list": str(fixtures.write_forests_list(src / "forests.list", forests)),
        "simulation_info": str(sim_info),
    }
    a_list = fixtures.write_a_list(src / "fixture.a_list")
    schema = build_schema(load_column_map(literal.PROFILE_DIR / "consistent_trees_ascii.yaml"))
    work = root / "work"
    pipeline.initialize(
        work, schema, parameters, a_list, ingest_max_rows=4, transpose_budget_bytes=literal.BUDGET
    )
    pipeline.run_ingest(work)
    pipeline.run_transpose(work)
    manifest = pipeline.run_write(work, HorizontalV3Writer(sim_info))
    return SimpleNamespace(
        work=work,
        a_list=a_list,
        manifest=manifest,
        dataset=manifest.artifact_path(manifest.stage("write")["directory"]),
    )


def reorder_to_source_file_order(directory, forests):
    """Rewrite an ASCII v3 dataset in the physical order a one-file source has
    without the forest-rank identity scheme: SourceHaloID is the halo's 1-based data
    row in the source file and every slab is in ascending SourceHaloID. Rows
    are permuted with every column, and each link's row index is remapped, so
    every link still names the same halo and only the identity order changes.
    The catalogue id (MostBoundID, the ctrees id) locates each halo's file row.
    """
    file_row = {
        halo.halo_id: row + 1
        for row, halo in enumerate(
            halo for tree in fixtures.all_trees(forests) for halo in tree.halos
        )
    }
    names = sorted(p.name for p in Path(directory).glob("snapshot_*.h5"))
    orders, new_row = [], []
    for name in names:
        with h5py.File(directory / name, "r") as handle:
            ids = handle["halos"]["MostBoundID"][...]
        sids = np.array([file_row[int(i)] for i in ids], dtype=np.int64)
        order = np.argsort(sids, kind="stable")
        inverse = np.empty_like(order)
        inverse[order] = np.arange(order.size)
        orders.append((order, sids))
        new_row.append(inverse)
    for snap, name in enumerate(names):
        order, sids = orders[snap]
        with h5py.File(directory / name, "r+") as handle:
            halos = handle["halos"]
            old = {key: halos[key][...] for key in halos}
            for key, values in old.items():
                halos[key][...] = values[order]
            for key in _SAME_SNAPSHOT_LINKS:
                values = old[key][order]
                set_ = values >= 0
                values[set_] = new_row[snap][values[set_]]
                halos[key][...] = values
            for key, target_key in _QUALIFIED_LINKS:
                values, targets = old[key][order], old[target_key][order]
                for row in np.flatnonzero(values >= 0):
                    values[row] = new_row[int(targets[row])][values[row]]
                halos[key][...] = values
            halos["SourceHaloID"][...] = sids[order]


class TestV3BatteryAsciiPosition(unittest.TestCase):
    """The (ForestIndex, rank) position check on the Consistent-Trees ASCII
    route: it applies to a complete dataset, it fails a physically ordered
    one, and a sampled ASCII inventory is refused so it can never be skipped."""

    @classmethod
    def setUpClass(cls):
        cls._tmp = tempfile.TemporaryDirectory(prefix="v3_ascii_battery_")
        cls.forests = source_file_forests()
        cls.conv = make_v3_ascii_conversion(cls._tmp.name, cls.forests)

    @classmethod
    def tearDownClass(cls):
        cls._tmp.cleanup()

    def run_v3(self, directory, manifest_path=None):
        result = validate_v3.run_battery_v3(
            directory,
            self.conv.a_list,
            manifest_path=manifest_path or self.conv.manifest.path,
            budget_bytes=V3_BUDGET,
        )
        return result, outcome_map(result.outcomes)

    def copy(self) -> Path:
        target = Path(tempfile.mkdtemp(dir=self._tmp.name)) / "dataset"
        shutil.copytree(self.conv.dataset, target)
        return target

    def identity_order(self, directory):
        rows = []
        for path in sorted(Path(directory).glob("snapshot_*.h5")):
            with h5py.File(path, "r") as handle:
                halos = handle["halos"]
                rows.extend(
                    zip(
                        halos["ForestIndex"][...].tolist(),
                        halos["HaloRankInForest"][...].tolist(),
                        halos["SourceHaloID"][...].tolist(),
                    )
                )
        return sorted(rows)

    def test_the_inventory_is_complete_so_the_position_check_applies(self):
        inventory = self.conv.manifest.inventory
        self.assertEqual(inventory["selected_halos"], inventory["total_halos"])
        with h5py.File(self.conv.dataset / "snapshot_000.h5", "r") as handle:
            self.assertEqual(
                handle["header"].attrs["source_format"].decode(), "consistent_trees_ascii"
            )

    def test_a_forest_blocked_ascii_dataset_passes_the_position_check(self):
        result, outcomes = self.run_v3(self.conv.dataset)
        self.assertEqual([o.line() for o in result.outcomes if o.status != "PASS"], [])
        self.assertEqual(outcomes["identity"].status, "PASS")
        ids = [sid for _forest, _rank, sid in self.identity_order(self.conv.dataset)]
        self.assertEqual(ids, list(range(1, len(ids) + 1)))

    def test_a_physically_ordered_ascii_dataset_fails_the_position_check(self):
        directory = self.copy()
        reorder_to_source_file_order(directory, self.forests)
        # the rewrite is a real physical order: forest 0 (the lowest forest id)
        # comes last in the source file, so its rank 0 is not SourceHaloID 1
        first = self.identity_order(directory)[0]
        self.assertEqual(first[:2], (0, 0))
        self.assertNotEqual(first[2], 1)
        _result, outcomes = self.run_v3(directory)
        self.assertEqual(outcomes["identity"].status, "FAIL", outcomes["identity"].line())
        self.assertIn("not its (ForestIndex, rank) position", outcomes["identity"].detail)
        self.assertIn(
            "(ForestIndex=0, rank=0) has SourceHaloID {}, expected 1".format(first[2]),
            outcomes["identity"].detail,
        )
        # the rewrite changes the files' bytes, so manifest-binding fails too;
        # every other content check still passes: the rewrite is otherwise a
        # valid dataset, so of the content checks only the position check
        # catches the physical order
        self.assertEqual(outcomes["manifest-binding"].status, "FAIL")
        for check in (
            "row-values",
            "link-targets",
            "topology-closure",
            "chain-cycles",
            "source-key-coverage",
            "sidecar-content",
            "count-conservation",
        ):
            self.assertEqual(outcomes[check].status, "PASS", outcomes[check].line())

    def test_a_sampled_ascii_inventory_is_refused(self):
        work = Path(tempfile.mkdtemp(dir=self._tmp.name)) / "work"
        shutil.copytree(self.conv.work, work)
        path = work / "manifest.json"
        data = json.loads(path.read_text())
        inventory = data["sources"]["inventory"]
        selected = int(inventory["selected_halos"])
        inventory["total_halos"] = selected + 1
        path.write_text(json.dumps(data))
        manifest = ConversionManifest.load(work)
        directory = manifest.artifact_path(manifest.stage("write")["directory"])
        _result, outcomes = self.run_v3(directory, manifest_path=manifest.path)
        self.assertEqual(outcomes["identity"].status, "FAIL", outcomes["identity"].line())
        self.assertIn(
            "consistent_trees_ascii inventory selects {} of {} halos; the ASCII route does "
            "not sample".format(selected, selected + 1),
            outcomes["identity"].detail,
        )
        # the selected count still matches the dataset, so the refusal is the
        # identity check's alone
        self.assertEqual(outcomes["source-key-coverage"].status, "PASS")
        self.assertEqual(outcomes["count-conservation"].status, "PASS")


class TestV3LiteralGraphs(unittest.TestCase):
    """Hand-specified graphs written with plain h5py; no manifest (API mode)."""

    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="v3_literal_"))
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)

    def run_literal(self, snapshots, sidecar, **header):
        header.setdefault("links_adjacent", 0)
        header.setdefault("max_rank", 6)
        directory = self.tmp / "dataset_{}".format(len(list(self.tmp.iterdir())))
        a_list = write_literal_v3(directory, snapshots, sidecar, **header)
        result = validate_v3.run_battery_v3(directory, a_list, budget_bytes=V3_BUDGET)
        return result, outcome_map(result.outcomes)

    def test_the_literal_graph_passes(self):
        snapshots, sidecar = literal_graph()
        result, outcomes = self.run_literal(snapshots, sidecar)
        failing = [o.line() for o in result.outcomes if o.status == "FAIL"]
        self.assertEqual(failing, [])
        self.assertEqual(outcomes["topology-closure"].status, "PASS")
        self.assertEqual(result.measurements["gapped_descendants"], 2)
        self.assertEqual(result.measurements["chain_edges"], 5)

    def test_a_pure_next_progenitor_cycle_is_caught_only_by_chain_cycles(self):
        snapshots, sidecar = literal_graph()
        # c0 <- b0 <- a0 ends there; a2 and b1 point at each other: each is
        # reached exactly once and names c0 as descendant, but no chain from
        # c0 ever reaches them
        snapshots[0]["NextProgenitor"] = [None, (1, 1)]
        snapshots[1]["NextProgenitor"] = [(0, 0), (1, 0)]
        _result, outcomes = self.run_literal(snapshots, sidecar)
        self.assertEqual(outcomes["topology-closure"].status, "PASS")
        self.assertEqual(outcomes["chain-cycles"].status, "FAIL")
        self.assertIn("NextProgenitor", outcomes["chain-cycles"].detail)

    def test_a_pure_fof_cycle_is_caught_only_by_chain_cycles(self):
        snapshots, sidecar = literal_graph()
        snapshots[2]["NextHaloInFOFgroup"] = [-1, 2, 1]
        _result, outcomes = self.run_literal(snapshots, sidecar)
        self.assertEqual(outcomes["topology-closure"].status, "PASS")
        self.assertEqual(outcomes["chain-cycles"].status, "FAIL")
        self.assertIn("NextHaloInFOFgroup", outcomes["chain-cycles"].detail)

    def test_a_progenitor_missing_from_its_chain_fails_topology_closure(self):
        snapshots, sidecar = literal_graph()
        # the chain now stops at a0, so b1 and a2 are never reached
        snapshots[0]["NextProgenitor"] = [None, None]
        _result, outcomes = self.run_literal(snapshots, sidecar)
        self.assertEqual(outcomes["topology-closure"].status, "FAIL")
        self.assertIn("not reached exactly once", outcomes["topology-closure"].detail)
        self.assertEqual(outcomes["chain-cycles"].status, "PASS")

    def test_a_halo_reached_twice_fails_topology_closure(self):
        snapshots, sidecar = literal_graph()
        # b1 is also made c0's main progenitor's sibling a second time
        snapshots[1]["NextProgenitor"] = [(1, 1), None]
        _result, outcomes = self.run_literal(snapshots, sidecar)
        self.assertEqual(outcomes["topology-closure"].status, "FAIL")

    def test_a_position_beyond_the_header_box_fails_position_bounds(self):
        snapshots, sidecar = literal_graph()
        # the literal header states a 100 Mpc/h box; x = 250 belongs to a larger one
        snapshots[2]["Pos"] = [[1.0, 1.0, 1.0], [250.0, 2.0, 3.0], [4.0, 5.0, 6.0]]
        result, outcomes = self.run_literal(snapshots, sidecar)
        self.assertEqual(
            [o.name for o in result.outcomes if o.status == "FAIL"], ["position-bounds"]
        )
        self.assertIn(
            "snapshot_002.h5 row 1 Pos [250.0, 2.0, 3.0]", outcomes["position-bounds"].detail
        )
        self.assertIn("box_size_mpc_h = 100.0", outcomes["position-bounds"].detail)

    def test_a_negative_position_component_fails_position_bounds(self):
        snapshots, sidecar = literal_graph()
        snapshots[0]["Pos"] = [[1.0, 1.0, 1.0], [1.0, -0.5, 1.0]]
        _result, outcomes = self.run_literal(snapshots, sidecar)
        self.assertEqual(outcomes["position-bounds"].status, "FAIL")
        self.assertIn(
            "snapshot_000.h5 row 1 Pos [1.0, -0.5, 1.0]", outcomes["position-bounds"].detail
        )

    def test_positions_on_the_box_faces_pass_position_bounds(self):
        snapshots, sidecar = literal_graph()
        snapshots[2]["Pos"] = [[0.0, 0.0, 0.0], [100.0, 100.0, 100.0], [0.0, 50.0, 100.0]]
        _result, outcomes = self.run_literal(snapshots, sidecar)
        self.assertEqual(outcomes["position-bounds"].status, "PASS")

    def test_a_forest_with_zero_halos_is_legal_for_lhalo(self):
        snapshots, _sidecar = literal_graph()
        # forest 0 is an empty tree; the graph's halos belong to forest 1
        for snap in snapshots:
            snap["ForestIndex"] = [1] * len(snap["SourceHaloID"])
        sidecar = {"ForestID": [0, 1], "SourceFileOrdinal": [0, 0], "SourceUnitOrdinal": [0, 1]}
        result, _outcomes = self.run_literal(snapshots, sidecar)
        self.assertEqual([o.line() for o in result.outcomes if o.status == "FAIL"], [])


def write_chain_dataset(directory, n_forests: int, n_snapshots: int = 5) -> Path:
    """A literal dataset of ``n_forests`` main-branch chains; odd forests skip
    snapshot 1, so half their descendants are gapped. Ranks run from the last
    snapshot back, and SourceHaloID is (forest, rank) order."""
    present = [
        [s for s in range(n_snapshots) if forest % 2 == 0 or s != 1] for forest in range(n_forests)
    ]
    sizes = [len(snaps) for snaps in present]
    bases = np.concatenate([[0], np.cumsum(sizes)])
    rows = {s: [] for s in range(n_snapshots)}  # forest ids present, ascending
    for forest, snaps in enumerate(present):
        for s in snaps:
            rows[s].append(forest)
    position = {s: {forest: row for row, forest in enumerate(rows[s])} for s in rows}
    snapshots = []
    for s in range(n_snapshots):
        spec = {name: [] for name in ("SourceHaloID", "ForestIndex", "HaloRankInForest")}
        spec["Descendant"], spec["FirstProgenitor"] = [], []
        for forest in rows[s]:
            snaps = present[forest]
            index = snaps.index(s)
            rank = len(snaps) - 1 - index
            spec["SourceHaloID"].append(int(bases[forest]) + rank + 1)
            spec["ForestIndex"].append(forest)
            spec["HaloRankInForest"].append(rank)
            later = snaps[index + 1] if index + 1 < len(snaps) else None
            earlier = snaps[index - 1] if index > 0 else None
            spec["Descendant"].append(None if later is None else (position[later][forest], later))
            spec["FirstProgenitor"].append(
                None if earlier is None else (position[earlier][forest], earlier)
            )
        snapshots.append(spec)
    sidecar = {
        "ForestID": list(range(n_forests)),
        "SourceFileOrdinal": [0] * n_forests,
        "SourceUnitOrdinal": list(range(n_forests)),
    }
    return write_literal_v3(
        directory, snapshots, sidecar, links_adjacent=0, max_rank=n_snapshots - 1
    )


def write_adjacent_chain_dataset(directory, n_forests: int, n_snapshots: int = 3) -> Path:
    """A literal adjacent dataset of ``n_forests`` main-branch chains present
    at every snapshot: forest f is row f of every snapshot and descends to row
    f of the next. Ranks run from the last snapshot back, and SourceHaloID is
    (forest, rank) order. Written column by column, so it can be large."""
    forest = np.arange(n_forests, dtype=np.int64)
    none = np.full(n_forests, -1, dtype=np.int64)
    snapshots = []
    for snap in range(n_snapshots):
        rank = n_snapshots - 1 - snap
        later = np.full(n_forests, snap + 1, dtype=np.int64)
        earlier = np.full(n_forests, snap - 1, dtype=np.int64)
        snapshots.append(
            {
                "SourceHaloID": forest * n_snapshots + rank + 1,
                "ForestIndex": forest,
                "HaloRankInForest": np.full(n_forests, rank, dtype=np.int64),
                "Descendant": (forest, later) if snap + 1 < n_snapshots else (none, none),
                "FirstProgenitor": (forest, earlier) if snap > 0 else (none, none),
            }
        )
    sidecar = {
        "ForestID": forest,
        "SourceFileOrdinal": np.zeros(n_forests, dtype=np.int64),
        "SourceUnitOrdinal": forest,
    }
    return write_literal_v3(
        directory, snapshots, sidecar, links_adjacent=1, max_rank=n_snapshots - 1
    )


class TestExternalSortBudget(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="v3_bounded_"))
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)

    def test_many_forests_merge_in_several_passes_within_the_budget(self):
        directory = self.tmp / "chains"
        a_list = write_chain_dataset(directory, 4000)
        spill = self.tmp / "spill"
        spill.mkdir()
        tracemalloc.start()
        try:
            result = validate_v3.run_battery_v3(
                directory, a_list, budget_bytes=V3_BUDGET, spill_dir=spill
            )
            _current, peak = tracemalloc.get_traced_memory()
        finally:
            tracemalloc.stop()
        self.assertEqual([o.line() for o in result.outcomes if o.status == "FAIL"], [])
        m = result.measurements
        self.assertEqual(sum(m["snapshot_counts"]), 18000)
        self.assertEqual(m["gapped_descendants"], 2000)
        self.assertLessEqual(m["peak_resident_bytes"], V3_BUDGET)
        # the join alone spills ~3.5 MB of records through a 1 MiB budget
        self.assertGreater(m["peak_spill_bytes"], V3_BUDGET)
        # everything the battery allocated, metered or not
        self.assertLess(peak, 3 * V3_BUDGET)
        self.assertEqual(list(spill.iterdir()), [])

    def test_a_budget_below_the_minimum_is_refused_before_reading(self):
        directory = self.tmp / "small"
        a_list = write_chain_dataset(directory, 4)
        for bad in (V3_BUDGET - 1, 1.5, True):
            with self.assertRaises(ConverterError):
                validate_v3.run_battery_v3(directory, a_list, budget_bytes=bad)


class TestV3FormatTables(unittest.TestCase):
    """The v3 battery restates the format tables literally instead of
    importing the writer's; this is the drift guard between the two."""

    def test_the_fixed_table_matches_the_schema_module_and_the_writer(self):
        from column_schema import EXTRA_TYPES, IDENTITY_FIELDS, TOPOLOGY_FIELDS
        from hdf5_writer_v3 import V3_FORMAT_VERSION, v3_halo_datasets
        from test_pipeline import lhalo_schema

        restated = [
            (field.name, np.dtype(EXTRA_TYPES[field.type].numpy_dtype).newbyteorder("<").str)
            for field in TOPOLOGY_FIELDS + IDENTITY_FIELDS
        ]
        self.assertEqual(list(validate_v3._V3_FIXED_DATASETS), restated)
        written = v3_halo_datasets(lhalo_schema())
        self.assertEqual(
            [(name, dtype.str) for name, (dtype, _vec) in list(written.items())[:11]], restated
        )
        # the version the writer stamps and the battery requires is the format's
        self.assertEqual(V3_FORMAT_VERSION, 3)

    def test_the_declarable_types_match_the_schema_module(self):
        from column_schema import EXTRA_TYPES

        self.assertEqual(
            validate_v3._V3_TYPES,
            {
                name: (np.dtype(spec.numpy_dtype).newbyteorder("<").str, spec.n_components == 3)
                for name, spec in EXTRA_TYPES.items()
            },
        )


# ==========================================================================
# The bounded mode against the external-sort battery
# ==========================================================================
#
# The bounded mode (validate_bounded) must report what the external-sort
# battery reports, check by check, on every adjacent (links_adjacent 1)
# dataset: the canned ASCII conversion and every corruption of it, the cut
# fixtures' conversions, and graphs built to stress the bounded walks (a long
# progenitor chain and FoF group, cycles, chains that cross block boundaries
# and snapshots). A gapped dataset stays with the external-sort battery and
# is refused by the bounded mode before it reads anything.

#: The bounded mode's budget in these tests: far above any fixture's ceiling.
BOUNDED_BUDGET = 64 << 20

#: Block sizes that put every chain of the fixtures across block boundaries.
BOUNDED_BLOCK_SIZES = (1, 2, 3, 7, validate_bounded.DEFAULT_BLOCK_ROWS)


def verdicts(result):
    return {outcome.name: outcome.status for outcome in result.outcomes}


def long_chain_forest():
    """Forest 9000: 9000 (snapshot 5) has 24 progenitors at snapshot 4, so its
    NextProgenitor chain is 23 links long; 9001 is their central and the other
    23 its satellites, a 23-link FoF chain. Each progenitor has its own
    progenitor at snapshot 3, and 9000 a satellite 9030 at snapshot 5."""
    halos = [
        fixtures.HaloSpec(halo_id=9000, snap=5, mvir=5.0e13, num_prog=24),
        fixtures.HaloSpec(halo_id=9030, snap=5, mvir=1.0e10, pid=9000, upid=9000),
    ]
    for k in range(1, 25):
        central = k == 1
        halos.append(
            fixtures.HaloSpec(
                halo_id=9000 + k,
                snap=4,
                mvir=4.0e13 / k,
                desc_id=9000,
                pid=-1 if central else 9001,
                upid=-1 if central else 9001,
                num_prog=1,
            )
        )
        halos.append(fixtures.HaloSpec(halo_id=9100 + k, snap=3, mvir=3.0e13 / k, desc_id=9000 + k))
    return fixtures.ForestSpec(forest_id=9000, trees=[fixtures.TreeSpec(root_id=9000, halos=halos)])


def row_of(directory, snap, halo_id) -> int:
    """The row of the halo whose catalogue id (MostBoundID) is ``halo_id``."""
    with h5py.File(Path(directory) / snapshot_h5_name(snap), "r") as handle:
        ids = handle["halos"]["MostBoundID"][...]
    return int(np.flatnonzero(ids == halo_id)[0])


def set_values(directory, snap, column, values):
    """``values`` maps row -> value of one /halos column."""
    with h5py.File(Path(directory) / snapshot_h5_name(snap), "r+") as handle:
        for row, value in values.items():
            handle["halos"][column][row] = value


def set_header(directory, attr, value, names=None):
    names = names or sorted(p.name for p in Path(directory).glob("snapshot_*.h5"))
    for name in names:
        with h5py.File(Path(directory) / name, "r+") as handle:
            attrs = handle["header"].attrs
            attrs.create(attr, value, dtype=attrs[attr].dtype)


class BoundedAgreementCase(unittest.TestCase):
    """Runs both modes over a dataset and asserts identical verdict sets."""

    def run_both(self, directory, a_list, manifest_path, block_sizes=(None,), examples=True):
        """Both modes over one dataset: identical verdicts at every block size,
        and identical outcome lines across block sizes. When ``examples`` is
        False a line is compared as the set of its failure counts and kinds,
        without their examples: which five examples, and which kind, a block
        order meets first may differ, while no count or kind may."""
        external = validate_v3.run_battery_v3(
            directory, a_list, manifest_path=manifest_path, budget_bytes=V3_BUDGET
        )
        self.assertEqual(external.measurements["battery_mode"], "external-sort")
        expected = verdicts(external)
        self.assertEqual(list(expected), list(validate_v3.V3_CHECKS))
        lines = None
        for block_rows in block_sizes:
            with capture_stderr() as err:
                bounded = validate_bounded.run_battery_bounded(
                    directory,
                    a_list,
                    manifest_path,
                    budget_bytes=BOUNDED_BUDGET,
                    block_rows=block_rows or validate_bounded.DEFAULT_BLOCK_ROWS,
                )
            self.assertIn("bounded battery: ceiling", err.text)
            self.assertEqual(
                verdicts(bounded),
                expected,
                "block_rows {}:\nexternal:\n{}\nbounded:\n{}".format(
                    block_rows,
                    "\n".join(o.line() for o in external.outcomes if o.status != "PASS"),
                    "\n".join(o.line() for o in bounded.outcomes if o.status != "PASS"),
                ),
            )
            measured = bounded.measurements
            self.assertEqual(measured["battery_mode"], "bounded")
            self.assertEqual(measured.get("peak_spill_bytes", 0), 0)
            # No meter-against-ceiling assertion here: the meter is charged
            # from the ceiling's own terms, so it could not fail. The ceiling
            # is tested against what the battery actually allocates in
            # TestBoundedCeiling.test_the_battery_allocates_within_its_printed_ceiling.
            # the verdict does not depend on the block size, nor do the details
            current = [o.line() for o in bounded.outcomes]
            if not examples:
                current = [
                    (o.name, o.status)
                    + tuple(
                        sorted(p for p in o.detail.split("; ") if not p.startswith("examples: "))
                    )
                    for o in bounded.outcomes
                ]
            if lines is not None:
                self.assertEqual(current, lines, "block_rows {}".format(block_rows))
            lines = current
        return outcome_map(external.outcomes), outcome_map(bounded.outcomes)

    def assert_only_kind(self, outcome, kind):
        """``outcome`` fails with ``kind`` as its one failure kind."""
        pattern = r"^\d+ {}; examples: [^;]*$".format(re.escape(kind))
        self.assertRegex(outcome.detail, pattern, outcome.line())


class TestBoundedModeAgreement(BoundedAgreementCase):
    """The canned ASCII conversion (adjacent) and every corruption of it."""

    @classmethod
    def setUpClass(cls):
        cls._tmp = tempfile.TemporaryDirectory(prefix="v3_bounded_agreement_")
        cls.forests = source_file_forests()
        with capture_stderr():
            cls.conv = make_v3_ascii_conversion(cls._tmp.name, cls.forests)

    @classmethod
    def tearDownClass(cls):
        cls._tmp.cleanup()

    def copy(self) -> Path:
        target = Path(tempfile.mkdtemp(dir=self._tmp.name)) / "dataset"
        shutil.copytree(self.conv.dataset, target)
        return target

    def agree(self, directory=None, block_sizes=(None,), **kwargs):
        return self.run_both(
            directory or self.conv.dataset,
            kwargs.get("a_list", self.conv.a_list),
            kwargs.get("manifest_path", self.conv.manifest.path),
            block_sizes,
            examples=kwargs.get("examples", True),
        )

    def edited_manifest(self, edit):
        work = Path(tempfile.mkdtemp(dir=self._tmp.name)) / "work"
        shutil.copytree(self.conv.work, work)
        path = work / "manifest.json"
        data = json.loads(path.read_text())
        edit(data)
        path.write_text(json.dumps(data))
        manifest = ConversionManifest.load(work)
        return manifest, manifest.artifact_path(manifest.stage("write")["directory"])

    def test_the_fixture_is_adjacent_and_passes_in_both_modes(self):
        with h5py.File(self.conv.dataset / "snapshot_005.h5", "r") as handle:
            self.assertEqual(int(handle["header"].attrs["links_adjacent"]), 1)
        external, bounded = self.agree(block_sizes=BOUNDED_BLOCK_SIZES)
        self.assertEqual([o.line() for o in bounded.values() if o.status != "PASS"], [])
        self.assertEqual(bounded["identity"].status, "PASS")

    def test_every_corruption_gets_the_same_verdicts(self):
        """Each corruption of the adjacent dataset, after the external-sort
        mode's own corruption tests, with the check it must fail. A third
        element names the one failure kind both modes must report for a
        corruption that only that kind's detector catches."""
        mutations = {
            # structure
            "extra root object": (lambda d: self.add_root_group(d), "object-set"),
            "narrowed link dtype": (lambda d: self.narrow(d), "object-set"),
            "scalar SourceHaloID": (lambda d: self.scalar_key(d), "object-set"),
            "/halos a dataset, not a group": (lambda d: self.halos_dataset(d), "object-set"),
            "missing file": (lambda d: (d / "snapshot_003.h5").unlink(), "file-set"),
            "extra sidecar dataset": (lambda d: self.add_sidecar_dataset(d), "sidecar-object-set"),
            "schema units of one file": (
                lambda d: self.schema_units(d, "snapshot_004.h5"),
                "schema-binding",
            ),
            # headers
            "format version": (
                lambda d: set_header(d, "format_version", 4, ["snapshot_003.h5"]),
                "header-values",
            ),
            "omega_matter of one file": (
                lambda d: set_header(d, "omega_matter", 0.5, ["snapshot_003.h5"]),
                "run-scoped-headers",
            ),
            "n_forests_total of one file": (
                lambda d: set_header(d, "n_forests_total", 9, ["snapshot_004.h5"]),
                "run-scoped-headers",
            ),
            "negative n_forests_total": (
                lambda d: set_header(d, "n_forests_total", -1),
                "identity",
            ),
            "n_forests_total 0 over nonempty slabs": (
                lambda d: set_header(d, "n_forests_total", 0),
                "identity",
            ),
            "max rank header": (
                lambda d: set_header(d, "max_halo_rank_in_forest", 7),
                "identity",
            ),
            # rows
            "SnapNum": (lambda d: set_values(d, 5, "SnapNum", {0: 4}), "row-values"),
            "SourceHaloID order": (
                lambda d: set_values(d, 5, "SourceHaloID", {0: 2, 1: 1}),
                "row-values",
            ),
            "negative Len": (lambda d: set_values(d, 4, "Len", {0: -1}), "len-nonnegative"),
            "NaN": (lambda d: set_values(d, 5, "Vmax", {3: np.nan}), "field-finiteness"),
            "infinite vector component": (
                lambda d: set_values(d, 5, "Vel", {1: [np.inf, 0.0, 0.0]}),
                "field-finiteness",
            ),
            "position outside the box": (
                lambda d: set_values(d, 5, "Pos", {0: [1.0e6, 1.0, 1.0]}),
                "position-bounds",
            ),
            "duplicated SourceHaloID": (
                lambda d: set_values(d, 5, "SourceHaloID", {8: 16}),
                "source-key-coverage",
            ),
            "SourceHaloID outside the inventory": (
                lambda d: set_values(d, 5, "SourceHaloID", {8: 99}),
                "source-key-coverage",
            ),
            # links
            "null index with a target snapshot": (
                lambda d: set_values(d, 4, "NextProgenitorSnapshot", {1: 4}),
                "link-targets",
            ),
            "row beyond its target snapshot": (
                lambda d: set_values(d, 5, "FirstProgenitor", {0: 99}),
                "link-targets",
            ),
            "backward descendant": (
                lambda d: set_values(d, 4, "DescendantSnapshot", {0: 3}),
                "link-targets",
            ),
            "target snapshot outside the dataset": (
                lambda d: set_values(d, 4, "DescendantSnapshot", {0: 9}),
                "link-targets",
            ),
            "NextProgenitor before the shared descendant's snapshot": (
                lambda d: set_values(d, 4, "NextProgenitorSnapshot", {0: 5}),
                "link-targets",
            ),
            "first progenitor two snapshots back": (
                lambda d: set_values(d, 5, "FirstProgenitorSnapshot", {0: 3}),
                "topology-closure",
            ),
            "first progenitor naming the wrong halo": (
                lambda d: set_values(d, 5, "FirstProgenitor", {1: 0}),
                "topology-closure",
            ),
            "next progenitor in an earlier snapshot": (
                lambda d: set_values(d, 4, "NextProgenitorSnapshot", {0: 3})
                or set_values(d, 4, "NextProgenitor", {0: 0}),
                "topology-closure",
            ),
            "next progenitor of another descendant": (
                lambda d: set_values(d, 4, "NextProgenitor", {0: 2}),
                "topology-closure",
            ),
            "NextProgenitor chain spliced onto another descendant's": (
                lambda d: self.splice_progenitor_chain(d),
                "topology-closure",
                validate_v3.KIND_NEXT_PROGENITOR,
            ),
            "FoF member of another group": (
                lambda d: set_values(d, 5, "NextHaloInFOFgroup", {2: 5}),
                "topology-closure",
            ),
            "FoF target that is not a central": (
                lambda d: set_values(d, 5, "FirstHaloInFOFgroup", {5: 5}),
                "topology-closure",
            ),
            "member left out of its FoF chain": (
                lambda d: set_values(d, 5, "NextHaloInFOFgroup", {7: -1}),
                "topology-closure",
            ),
            "cross-forest link": (
                lambda d: set_values(d, 5, "ForestIndex", {3: 0}),
                "topology-closure",
            ),
            "gapped descendant under links_adjacent 1": (
                lambda d: set_values(d, 3, "DescendantSnapshot", {0: 5}),
                "links-adjacent",
            ),
            "FoF cycle": (
                lambda d: set_values(d, 5, "NextHaloInFOFgroup", {8: 7}),
                "chain-cycles",
            ),
            # identity and sidecar
            "rank gap": (lambda d: set_values(d, 5, "HaloRankInForest", {8: 5}), "identity"),
            "ForestIndex outside n_forests_total": (
                lambda d: set_values(d, 5, "ForestIndex", {8: 99}),
                "identity",
            ),
            "ranks out of source order": (
                lambda d: set_values(d, 4, "HaloRankInForest", {3: 3, 4: 2}),
                "identity",
            ),
            "repeated (ForestIndex, rank) pair": (
                lambda d: set_values(d, 5, "HaloRankInForest", {7: 0}),
                "identity",
            ),
            "skipped forest": (
                lambda d: set_values(d, 2, "ForestIndex", {0: 3})
                or set_values(d, 1, "ForestIndex", {0: 3}),
                "identity",
            ),
            "empty ASCII forest inside the enumeration": (
                lambda d: self.empty_forest(d, 3),
                "identity",
                validate_v3.KIND_ASCII_FOREST_SKIPPED,
            ),
            "physically ordered ASCII dataset": (
                lambda d: reorder_to_source_file_order(d, self.forests),
                "identity",
            ),
            "sidecar ForestID order": (
                lambda d: self.sidecar(d, "ForestID", {1: 0}),
                "sidecar-content",
            ),
            "sidecar shorter than n_forests_total": (
                lambda d: self.shorten_sidecar(d),
                "sidecar-content",
            ),
            # binding
            "tampered content": (
                lambda d: set_values(d, 3, "Vmax", {0: 99.0}),
                "manifest-binding",
            ),
        }
        for name, (mutate, check, *only) in mutations.items():
            with self.subTest(name):
                directory = self.copy()
                mutate(directory)
                external, bounded = self.agree(directory, BOUNDED_BLOCK_SIZES[:3], examples=False)
                self.assertEqual(external[check].status, "FAIL", external[check].line())
                for kind in only:
                    self.assert_only_kind(external[check], kind)
                    self.assert_only_kind(bounded[check], kind)

    def test_a_repeated_pair_fails_identity_where_position_identity_does_not_apply(self):
        """Without the position check (the state of a sampled L-Halo or
        forests-HDF5 conversion) only the per-forest rank order catches a
        repeated (ForestIndex, rank) pair, which in the bounded mode is the
        position bitset. Each repeated pair is listed once among the
        examples."""
        directory = self.copy()
        set_values(directory, 5, "HaloRankInForest", {7: 0})
        with mock.patch.object(validate_v3, "POSITION_IDENTITY_FORMATS", ()):
            external, bounded = self.agree(directory)
        self.assertEqual(external["identity"].status, "FAIL")
        detail = bounded["identity"].detail
        self.assertNotIn(validate_v3.KIND_POSITION_IDENTITY, detail)
        found = re.search(re.escape(validate_v3.KIND_RANK_DENSITY) + "; examples: ([^;]*)", detail)
        self.assertIsNotNone(found, detail)
        listed = re.findall(r"\(ForestIndex=\d+, rank=\d+\)", found.group(1))
        self.assertTrue(listed, detail)
        self.assertEqual(len(listed), len(set(listed)), detail)

    def test_another_a_list_gets_the_same_verdicts(self):
        other = Path(self._tmp.name) / "other.a_list"
        other.write_text("".join("{!r}\n".format(0.1 * (i + 1)) for i in range(6)))
        external, _bounded = self.agree(
            block_sizes=BOUNDED_BLOCK_SIZES[:3], a_list=other, examples=False
        )
        self.assertEqual(external["manifest-binding"].status, "FAIL")
        self.assertEqual(external["header-values"].status, "FAIL")

    def test_manifest_corruptions_get_the_same_verdicts(self):
        selected = int(self.conv.manifest.inventory["selected_halos"])

        def sampled(data):
            data["sources"]["inventory"]["total_halos"] = selected + 1

        def more(data):
            data["sources"]["inventory"]["selected_halos"] = selected + 2
            data["sources"]["inventory"]["total_halos"] = selected + 2

        def counts(data):
            recorded = data["stages"]["ingest"]["result"]["snapshot_counts"]
            data["stages"]["ingest"]["result"]["snapshot_counts"] = recorded[:-1] + [0]

        for name, edit, check in (
            ("a sampled ASCII inventory", sampled, "identity"),
            ("an inventory selecting more halos", more, "count-conservation"),
            ("ingest counts that differ", counts, "count-conservation"),
            ("an incomplete write", lambda d: d["stages"]["write"].update(status="failed"), None),
        ):
            with self.subTest(name):
                manifest, directory = self.edited_manifest(edit)
                external, _bounded = self.agree(
                    directory, BOUNDED_BLOCK_SIZES[:3], manifest_path=manifest.path, examples=False
                )
                check = check or "manifest-binding"
                self.assertEqual(external[check].status, "FAIL", external[check].line())

    # ---- corruption helpers ------------------------------------------------

    def splice_progenitor_chain(self, directory):
        """Cut 1010's two-member progenitor chain at snapshot 4 after its head
        and hang the second member on 1020's one-member chain instead: every
        halo is still reached once and no link crosses a forest, but that
        NextProgenitor names a halo of another descendant."""
        with h5py.File(Path(directory) / "snapshot_005.h5", "r") as handle:
            first_progenitor = handle["halos"]["FirstProgenitor"][...]
        with h5py.File(Path(directory) / "snapshot_004.h5", "r") as handle:
            next_progenitor = handle["halos"]["NextProgenitor"][...]
        head = int(first_progenitor[row_of(directory, 5, 1010)])
        second = int(next_progenitor[head])
        other = int(first_progenitor[row_of(directory, 5, 1020)])
        self.assertEqual((next_progenitor[second], next_progenitor[other]), (-1, -1))
        set_values(directory, 4, "NextProgenitor", {head: -1, other: second})
        set_values(directory, 4, "NextProgenitorSnapshot", {head: -1, other: 4})

    def empty_forest(self, directory, forest):
        """Relabel ``forest``'s halos as ``forest - 1``'s, ranked after its own:
        ``forest`` holds no halo while every SourceHaloID position, link and
        the maximum rank stay as they were."""
        paths = sorted(Path(directory).glob("snapshot_*.h5"))
        indices = []
        for path in paths:
            with h5py.File(path, "r") as handle:
                indices.append(handle["halos"]["ForestIndex"][...])
        offset = sum(int(np.count_nonzero(values == forest - 1)) for values in indices)
        for path, values in zip(paths, indices):
            rows = np.flatnonzero(values == forest)
            if not rows.size:
                continue
            with h5py.File(path, "r+") as handle:
                halos = handle["halos"]
                rank = halos["HaloRankInForest"][...]
                for row in rows:
                    halos["ForestIndex"][row] = forest - 1
                    halos["HaloRankInForest"][row] = rank[row] + offset

    def add_root_group(self, directory):
        with h5py.File(directory / "snapshot_002.h5", "r+") as handle:
            handle.create_group("extra")

    def add_sidecar_dataset(self, directory):
        with h5py.File(directory / "forests.h5", "r+") as handle:
            handle.create_dataset("Extra", data=np.zeros(5, "<i8"), chunks=(5,))

    def halos_dataset(self, directory):
        with h5py.File(directory / "snapshot_004.h5", "r+") as handle:
            del handle["halos"]
            handle.create_dataset("halos", data=np.zeros(5, "<i8"))

    def scalar_key(self, directory):
        with h5py.File(directory / "snapshot_004.h5", "r+") as handle:
            del handle["halos"]["SourceHaloID"]
            handle["halos"].create_dataset("SourceHaloID", data=np.int64(3))

    def narrow(self, directory):
        with h5py.File(directory / "snapshot_004.h5", "r+") as handle:
            halos = handle["halos"]
            values = halos["Descendant"][...]
            del halos["Descendant"]
            halos.create_dataset(
                "Descendant", data=values.astype("<i4"), chunks=(65536,), maxshape=(None,)
            )

    def schema_units(self, directory, name):
        with h5py.File(directory / name, "r+") as handle:
            handle["schema"]["Vmax"].attrs.create("units", "m/s", dtype=h5py.string_dtype("utf-8"))

    def sidecar(self, directory, column, values):
        with h5py.File(directory / "forests.h5", "r+") as handle:
            for row, value in values.items():
                handle[column][row] = value

    def shorten_sidecar(self, directory):
        with h5py.File(directory / "forests.h5", "r+") as handle:
            for name in ("ForestID", "SourceFileOrdinal", "SourceUnitOrdinal"):
                handle[name].resize((3,))


class TestBoundedModeChains(BoundedAgreementCase):
    """A long progenitor chain and FoF group, walked at every block size,
    and cycles in them: inside one snapshot, across a block boundary, and
    across snapshots."""

    @classmethod
    def setUpClass(cls):
        cls._tmp = tempfile.TemporaryDirectory(prefix="v3_bounded_chains_")
        cls.forests = [fixtures.multi_tree_forest(), long_chain_forest()]
        with capture_stderr():
            cls.conv = make_v3_ascii_conversion(cls._tmp.name, cls.forests)

    @classmethod
    def tearDownClass(cls):
        cls._tmp.cleanup()

    def copy(self) -> Path:
        target = Path(tempfile.mkdtemp(dir=self._tmp.name)) / "dataset"
        shutil.copytree(self.conv.dataset, target)
        return target

    def agree(self, directory=None):
        return self.run_both(
            directory or self.conv.dataset,
            self.conv.a_list,
            self.conv.manifest.path,
            BOUNDED_BLOCK_SIZES,
        )

    def chain(self, directory, snap, column, head):
        """The rows of one chain, from ``head`` along ``column``."""
        with h5py.File(Path(directory) / snapshot_h5_name(snap), "r") as handle:
            successor = handle["halos"][column][...]
        rows = [head]
        while successor[rows[-1]] != -1 and len(rows) <= successor.size:
            rows.append(int(successor[rows[-1]]))
        return rows

    def progenitors(self, directory):
        """9000's progenitor chain at snapshot 4, head first."""
        root = row_of(directory, 5, 9000)
        with h5py.File(Path(directory) / "snapshot_005.h5", "r") as handle:
            head = int(handle["halos"]["FirstProgenitor"][root])
        return self.chain(directory, 4, "NextProgenitor", head)

    def members(self, directory):
        """9001's FoF chain at snapshot 4, central first."""
        return self.chain(directory, 4, "NextHaloInFOFgroup", row_of(directory, 4, 9001))

    def test_the_long_chains_pass_in_both_modes_at_every_block_size(self):
        self.assertEqual(len(self.progenitors(self.conv.dataset)), 24)
        self.assertEqual(len(self.members(self.conv.dataset)), 24)
        _external, bounded = self.agree()
        self.assertEqual([o.line() for o in bounded.values() if o.status != "PASS"], [])

    def test_a_pure_next_progenitor_cycle_is_caught_by_chain_cycles_alone(self):
        directory = self.copy()
        chain = self.progenitors(directory)
        # the chain now ends at its 10th member, and the 11th to 24th form a
        # cycle whose members are each still reached exactly once
        set_values(directory, 4, "NextProgenitor", {chain[9]: -1, chain[-1]: chain[10]})
        set_values(directory, 4, "NextProgenitorSnapshot", {chain[9]: -1, chain[-1]: 4})
        external, bounded = self.agree(directory)
        self.assertEqual(external["topology-closure"].status, "PASS")
        self.assertEqual(external["chain-cycles"].status, "FAIL")
        self.assertIn("NextProgenitor", bounded["chain-cycles"].detail)
        self.assertIn("14 halo(s)", bounded["chain-cycles"].detail)

    def test_a_next_progenitor_chain_that_loops_back_is_caught_by_both_checks(self):
        directory = self.copy()
        chain = self.progenitors(directory)
        set_values(directory, 4, "NextProgenitor", {chain[-1]: chain[5]})
        set_values(directory, 4, "NextProgenitorSnapshot", {chain[-1]: 4})
        external, _bounded = self.agree(directory)
        self.assertEqual(external["topology-closure"].status, "FAIL")
        self.assertEqual(external["chain-cycles"].status, "FAIL")

    def test_a_pure_fof_cycle_across_block_boundaries_is_caught_by_chain_cycles_alone(self):
        directory = self.copy()
        chain = self.members(directory)
        set_values(directory, 4, "NextHaloInFOFgroup", {chain[2]: -1, chain[-1]: chain[3]})
        external, bounded = self.agree(directory)
        self.assertEqual(external["topology-closure"].status, "PASS")
        self.assertEqual(external["chain-cycles"].status, "FAIL")
        self.assertIn("NextHaloInFOFgroup", bounded["chain-cycles"].detail)

    def test_a_fof_member_reached_twice_is_not_a_cycle(self):
        directory = self.copy()
        chain = self.members(directory)
        # two members name the same successor: a merge, not a cycle
        set_values(directory, 4, "NextHaloInFOFgroup", {chain[4]: chain[9]})
        external, _bounded = self.agree(directory)
        self.assertEqual(external["topology-closure"].status, "FAIL")
        self.assertEqual(external["chain-cycles"].status, "PASS")

    def test_a_next_progenitor_cycle_across_snapshots_is_caught(self):
        directory = self.copy()
        tail = self.progenitors(directory)[-1]
        early = row_of(directory, 3, 9124)
        root = row_of(directory, 5, 9000)
        # the tail at snapshot 4 names 9124 at snapshot 3, given 9000 as its
        # descendant two snapshots on, which names the tail again
        set_values(directory, 4, "NextProgenitor", {tail: early})
        set_values(directory, 4, "NextProgenitorSnapshot", {tail: 3})
        set_values(directory, 3, "Descendant", {early: root})
        set_values(directory, 3, "DescendantSnapshot", {early: 5})
        set_values(directory, 3, "NextProgenitor", {early: tail})
        set_values(directory, 3, "NextProgenitorSnapshot", {early: 4})
        external, bounded = self.agree(directory)
        self.assertEqual(external["links-adjacent"].status, "FAIL")
        self.assertEqual(external["chain-cycles"].status, "FAIL")
        self.assertIn("crossing snapshots", bounded["chain-cycles"].detail)


class TestBoundedModeCutFixtures(BoundedAgreementCase):
    """Every adversarial cut fixture, converted by the ASCII route with and
    without its cut table."""

    def test_every_cut_fixture_gets_the_same_verdicts_cut_and_uncut(self):
        from test_ascii_adapter import CUT_FIXTURES, convert_under_cut
        from test_rewriter import cut_layout, cut_source, write_table

        root = Path(tempfile.mkdtemp(prefix="v3_bounded_cut_"))
        self.addCleanup(shutil.rmtree, root, ignore_errors=True)
        for name, forest_of, table_of, split in CUT_FIXTURES:
            with self.subTest(name):
                forest = forest_of()
                source = cut_source(root / name, forest, cut_layout(forest, split))
                table = write_table(root / name, "cut", table_of(), source)
                manifest = convert_under_cut(source, root / name / "cut_work", table)
                a_list = source["root"] / "fixture.a_list"
                for work in (root / name / "work", manifest.workdir):
                    loaded = ConversionManifest.load(work)
                    dataset = loaded.artifact_path(loaded.stage("write")["directory"])
                    with h5py.File(dataset / "snapshot_005.h5", "r") as handle:
                        self.assertEqual(int(handle["header"].attrs["links_adjacent"]), 1)
                    external, _bounded = self.run_both(
                        dataset, a_list, loaded.path, BOUNDED_BLOCK_SIZES[:3]
                    )
                    self.assertFalse(
                        [o.line() for o in external.values() if o.status != "PASS"], work
                    )


class TestBoundedModeRefusals(unittest.TestCase):
    """What the bounded mode refuses, each before any long pass."""

    @classmethod
    def setUpClass(cls):
        cls._tmp = tempfile.TemporaryDirectory(prefix="v3_bounded_refusals_")
        root = Path(cls._tmp.name)
        with capture_stderr():
            cls.gapped = make_v3_conversion(root / "gapped")
            cls.adjacent = make_v3_ascii_conversion(root / "adjacent", source_file_forests())

    @classmethod
    def tearDownClass(cls):
        cls._tmp.cleanup()

    def copy(self, conv) -> Path:
        target = Path(tempfile.mkdtemp(dir=self._tmp.name)) / "dataset"
        shutil.copytree(conv.dataset, target)
        return target

    def bounded(self, conv, directory=None, **kwargs):
        kwargs.setdefault("budget_bytes", BOUNDED_BUDGET)
        with capture_stderr():
            return validate_bounded.run_battery_bounded(
                directory or conv.dataset,
                conv.a_list,
                kwargs.pop("manifest_path", conv.manifest.path),
                **kwargs,
            )

    def assert_refused_before_reading(self, pattern, conv, directory=None, **kwargs):
        reads = mock.patch.object(
            validate_v3, "check_manifest_binding", side_effect=AssertionError("read")
        )
        with reads, mock.patch.object(
            validate_bounded._BoundedRowScan, "run", side_effect=AssertionError("scanned")
        ):
            with self.assertRaisesRegex(ConverterError, pattern):
                self.bounded(conv, directory, **kwargs)

    def test_a_gapped_dataset_is_refused_by_name_and_passes_the_general_battery(self):
        self.assert_refused_before_reading(
            r"adjacent datasets \(links_adjacent 1\) only.*snapshot_000\.h5 declares "
            r"links_adjacent 0",
            self.gapped,
        )
        result = validate_v3.run_battery_v3(
            self.gapped.dataset,
            self.gapped.a_list,
            manifest_path=self.gapped.manifest.path,
            budget_bytes=V3_BUDGET,
        )
        self.assertFalse(result.failed)
        self.assertEqual(result.measurements["links_adjacent_measured"], 0)

    def test_one_gapped_file_among_adjacent_files_is_refused_by_name(self):
        directory = self.copy(self.adjacent)
        set_header(directory, "links_adjacent", 0, ["snapshot_003.h5"])
        self.assert_refused_before_reading(
            "1 file.*snapshot_003.h5 declares links_adjacent 0", self.adjacent, directory
        )

    def test_a_ceiling_above_the_budget_is_refused(self):
        # a fixture's ceiling is a few KB; scale the per-row term past 1 MiB
        with mock.patch.object(validate_bounded, "PAIR_BYTES_PER_ROW", 1 << 20):
            self.assert_refused_before_reading(
                r"ceiling \d+ B exceeds the budget of 1048576 B .*pair pass = 1048576 x 9 widest",
                self.adjacent,
                budget_bytes=V3_BUDGET,
            )

    def test_a_missing_or_unloadable_manifest_is_refused(self):
        self.assert_refused_before_reading(
            "needs the conversion manifest", self.adjacent, manifest_path=None
        )
        self.assert_refused_before_reading(
            "cannot load the conversion manifest",
            self.adjacent,
            manifest_path=Path(self._tmp.name) / "absent" / "manifest.json",
        )

    def test_a_snapshot_above_int32_max_rows_is_refused_by_name(self):
        """The mode holds whole-snapshot columns, which the format does not let
        a validator require above INT32_MAX rows. The wide SourceHaloID is
        declared but never written, so nothing of its size is stored."""
        directory = self.copy(self.adjacent)
        with h5py.File(directory / "snapshot_005.h5", "r+") as handle:
            halos = handle["halos"]
            del halos["SourceHaloID"]
            rows = validate_bounded.MAX_WIDEST_ROWS + 1
            halos.create_dataset("SourceHaloID", shape=(rows,), dtype="<i8", chunks=(65536,))
        self.assert_refused_before_reading(
            r"above INT32_MAX \(2147483647\) rows.*snapshot_005\.h5 holds 2147483648 rows",
            self.adjacent,
            directory,
        )

    def test_block_sizes_outside_the_int32_range_are_refused(self):
        for bad in (0, validate_bounded.MAX_BLOCK_ROWS + 1, 2.0, True):
            with self.assertRaises(ConverterError):
                self.bounded(self.adjacent, block_rows=bad)

    def test_a_stamped_gapped_dataset_agrees_or_is_refused_within_its_budget(self):
        directory = self.copy(self.gapped)
        set_header(directory, "links_adjacent", 1)
        result = self.bounded(self.gapped, directory)
        external = validate_v3.run_battery_v3(
            directory,
            self.gapped.a_list,
            manifest_path=self.gapped.manifest.path,
            budget_bytes=V3_BUDGET,
        )
        self.assertEqual(verdicts(result), verdicts(external))
        self.assertEqual(outcome_map(result.outcomes)["links-adjacent"].status, "FAIL")
        self.assertEqual(outcome_map(result.outcomes)["topology-closure"].status, "PASS")
        self.assertGreater(result.measurements["far_links"], 0)
        # when the links it would have to hold do not fit above the ceiling,
        # it stops rather than exceed the budget
        with mock.patch.object(validate_bounded, "FAR_LINK_BYTES", BOUNDED_BUDGET):
            with self.assertRaisesRegex(ConverterError, "do not join adjacent snapshots"):
                self.bounded(self.gapped, directory)

    def test_every_inherited_reader_gets_the_charged_block(self):
        # the battery hands its one effective block to the inherited sidecar
        # reader and row scan: here min(block_rows, W) = W = 9 rows
        calls = []
        real = validate_v3.check_sidecar_content

        def recording(*args):
            calls.append(args[-1])
            return real(*args)

        with mock.patch.object(validate_v3, "check_sidecar_content", side_effect=recording):
            result = self.bounded(self.adjacent)
        ceiling = result.measurements["ceiling"]
        self.assertEqual(ceiling["effective_block_rows"], 9)
        self.assertEqual(calls, [9])
        self.assertEqual(result.measurements["scan_rows"], 9)
        # many empty forests and few halos (256 forests per row of the widest
        # snapshot): the sidecar's forest rows are read within the ceiling's
        # block and forest terms, not in a block_rows window
        n_forests = 1 << 20
        path = Path(self._tmp.name) / "wide_sidecar.h5"
        with h5py.File(path, "w") as handle:
            for name, values in (
                ("ForestID", np.arange(n_forests, dtype="<i8")),
                ("SourceFileOrdinal", np.zeros(n_forests, dtype="<i8")),
                ("SourceUnitOrdinal", np.arange(n_forests, dtype="<i8")),
            ):
                handle.create_dataset(name, data=values, chunks=(65536,))
        wide = validate_bounded.Ceiling(4096, 4096, 4096, n_forests, 140)
        self.assertEqual(wide.block, 4096)
        terms = wide.forest_bytes + validate_bounded.PAIR_BLOCK_BYTES_PER_ROW * wide.block

        def peak_of(block):
            tracemalloc.start()
            try:
                failures = validate_v3.check_sidecar_content(
                    path, "consistent_trees_ascii", n_forests, None, block
                )
                return failures, tracemalloc.get_traced_memory()[1]
            finally:
                tracemalloc.stop()

        failures, peak = peak_of(wide.block)
        self.assertEqual(failures, [])
        self.assertLess(peak, terms)
        # the unrestricted block_rows window this replaces does not fit
        _failures, peak = peak_of(validate_bounded.DEFAULT_BLOCK_ROWS)
        self.assertGreater(peak, terms)

    def test_the_external_sort_battery_stays_the_default(self):
        result = validate_v3.run_battery_v3(
            self.adjacent.dataset,
            self.adjacent.a_list,
            manifest_path=self.adjacent.manifest.path,
            budget_bytes=V3_BUDGET,
        )
        self.assertEqual(result.measurements["battery_mode"], "external-sort")
        self.assertGreater(result.measurements["chain_edges"], 0)


class TestBoundedCeiling(unittest.TestCase):
    def test_the_ceiling_is_the_stated_function(self):
        ceiling = validate_bounded.Ceiling(
            widest_rows=1000,
            n_halos=5000,
            total_halos=6000,
            n_forests_total=70,
            row_bytes=140,
            block_rows=100,
        )
        pair = 64 * 1000 + 125 + 750 + 625 + 8 * 70 + 256 * 100 + (2 << 20)
        row = 8 * 70 + (4 * 140 + 256) * 100
        self.assertEqual(ceiling.pair_pass, pair)
        self.assertEqual(ceiling.row_pass, row)
        self.assertEqual(ceiling.total, max(pair, row))
        # the effective block is at least one row when no snapshot holds a halo
        self.assertEqual(validate_bounded.effective_block_rows(1 << 20, 0), 1)
        self.assertEqual(validate_bounded.Ceiling(0, 0, 0, 5, 140).block, 1)

    def test_shin_uchuu_numbers_fit_below_64_gb(self):
        for n_forests in (166547771, 244953607):
            ceiling = validate_bounded.Ceiling(
                widest_rows=519342987,
                n_halos=22503649037,
                total_halos=22503649037,
                n_forests_total=n_forests,
                row_bytes=140,
            )
            self.assertLess(ceiling.total, 64 * 10**9)
            self.assertGreater(ceiling.total, 38 * 10**9)

    def test_the_command_prints_the_ceiling(self):
        with mock.patch("sys.stdout") as out:
            code = validate_bounded.main(
                [
                    "ceiling",
                    "--widest-rows",
                    "1000",
                    "--n-halos",
                    "5000",
                    "--n-forests-total",
                    "70",
                ]
            )
        self.assertEqual(code, 0)
        text = "".join(call.args[0] for call in out.write.call_args_list)
        expected = validate_bounded.Ceiling(1000, 5000, 5000, 70, 140).total
        self.assertIn("ceiling_bytes {}".format(expected), text)

    def test_the_battery_allocates_within_its_printed_ceiling(self):
        """Every buffer and object the battery allocates through Python and
        numpy, metered or not, stays inside the ceiling it printed, on a
        dataset whose widest-snapshot columns dominate the ceiling. The meter
        is charged from the ceiling's own terms; this measures the battery
        instead. The HDF5 library's own buffers are not traced."""
        n_forests, n_snapshots, block_rows = 400_000, 2, 4096
        n_halos = n_forests * n_snapshots
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp) / "chains"
            a_list = write_adjacent_chain_dataset(directory, n_forests, n_snapshots)
            inventory = {
                "total_halos": n_halos,
                "selected_halos": n_halos,
                "n_units": n_forests,
                "files": [{"source_file_ordinal": 0, "n_units": n_forests}],
            }
            # A literal dataset has no conversion manifest: the checks bound to
            # one are shared with the external-sort mode, hold nothing that
            # grows with the dataset, and are stood in for, so every pass of
            # the bounded mode runs on the real files.
            with mock.patch.object(
                validate_v3, "load_manifest", return_value=SimpleNamespace(inventory=inventory)
            ), mock.patch.object(
                validate_v3, "check_manifest_binding", return_value=[]
            ), mock.patch.object(
                validate_v3, "check_schema_binding", return_value=[]
            ), mock.patch.object(
                validate_v3, "check_headers", return_value=([], [])
            ), mock.patch.object(
                validate_v3, "check_count_conservation", return_value=[]
            ), capture_stderr():
                tracemalloc.start()
                try:
                    result = validate_bounded.run_battery_bounded(
                        directory,
                        a_list,
                        Path(tmp) / "manifest.json",
                        budget_bytes=BOUNDED_BUDGET,
                        block_rows=block_rows,
                    )
                    _current, peak = tracemalloc.get_traced_memory()
                finally:
                    tracemalloc.stop()
        self.assertEqual([o.line() for o in result.outcomes if o.status != "PASS"], [])
        ceiling = validate_bounded.Ceiling(
            widest_rows=n_forests,
            n_halos=n_halos,
            total_halos=n_halos,
            n_forests_total=n_forests,
            row_bytes=result.measurements["ceiling"]["row_bytes"],
            block_rows=block_rows,
        )
        self.assertEqual(result.measurements["ceiling"]["ceiling_bytes"], ceiling.total)
        # the widest snapshot's resident columns dominate the ceiling, and the
        # measurement sees them, so a buffer the ceiling does not charge shows
        self.assertGreater(ceiling.pair_bytes, ceiling.total // 2)
        self.assertGreater(peak, ceiling.pair_bytes)
        self.assertLessEqual(peak, ceiling.total)

    def test_bitsets_record_repeats_and_gaps(self):
        bits = validate_bounded._Bitset(20)
        within, seen = bits.first_and_seen(np.array([3, 9, 3, 17], dtype=np.int64))
        self.assertEqual(within.tolist(), [3])
        self.assertEqual(seen.tolist(), [])
        within, seen = bits.first_and_seen(np.array([9, 0, 8], dtype=np.int64))
        self.assertEqual((within.tolist(), seen.tolist()), ([], [9]))
        self.assertEqual(bits.count(), 5)
        self.assertEqual(bits.clear_indices(3), [1, 2, 4])

    def test_the_first_clear_bits_of_a_large_sparse_bitset_cost_a_small_fixed_amount(self):
        n = 1 << 31  # 256 MiB of bits: the first 64th set, every bit after it clear
        bits = validate_bounded._Bitset(n)
        filled = n // 64
        bits.bits[: filled // 8] = 0xFF
        tracemalloc.start()
        try:
            first = bits.clear_indices()
            _current, peak = tracemalloc.get_traced_memory()
        finally:
            tracemalloc.stop()
        self.assertEqual(first, list(range(filled, filled + 5)))
        # the search holds one 64 KiB window's mask and indices, not the bitset's
        self.assertLess(peak, 2 << 20)
        # a full bitset has no clear bit, and the trailing bits past n are never named
        full = validate_bounded._Bitset(13)
        full.set_sorted(np.arange(13, dtype=np.int64))
        self.assertEqual(full.clear_indices(), [])


if __name__ == "__main__":
    unittest.main()
