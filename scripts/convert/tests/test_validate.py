"""Slice 7 unit tests: the producer validation battery must catch every
deliberately corrupted dataset — each format invariant and battery check is
violated once and the named check must FAIL (plan Slice 7 validation plan)."""

import json
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

import validate  # noqa: E402
from hdf5_writer import CHUNK_1D, snapshot_h5_name  # noqa: E402
from test_hdf5_writer import make_written_workdir  # noqa: E402
from validate import DEFAULT_MULTIPLIER, run_battery  # noqa: E402


def outcome_map(outcomes):
    return {outcome.name: outcome for outcome in outcomes}


class TestBattery(unittest.TestCase):
    """One shared pristine dataset; every corruption test works on a copy."""

    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        root = Path(cls.tmp.name)
        cls.workdir, cls.a_list_path, cls.sim_info, cls.hdf5_dir = make_written_workdir(root)
        cls.manifest_path = cls.workdir / "manifest.json"

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def _copy_dataset(self) -> Path:
        target = Path(tempfile.mkdtemp(dir=self.tmp.name)) / "hdf5"
        shutil.copytree(self.hdf5_dir, target)
        return target

    def _run(self, directory, manifest_path=None, multiplier=DEFAULT_MULTIPLIER):
        return outcome_map(
            run_battery(
                directory, self.a_list_path, manifest_path=manifest_path, multiplier=multiplier
            )
        )

    def assert_fails(self, outcomes, name, fragment=None):
        self.assertEqual(outcomes[name].status, "FAIL", outcomes[name].line())
        if fragment is not None:
            self.assertIn(fragment, outcomes[name].detail)

    # -- pristine ------------------------------------------------------------

    def test_pristine_dataset_passes(self):
        outcomes = self._run(self.hdf5_dir, manifest_path=self.manifest_path)
        failed = [o.line() for o in outcomes.values() if o.status != "PASS"]
        self.assertEqual(failed, [])

    def test_len_zero_count_logged(self):
        outcomes = self._run(self.hdf5_dir, manifest_path=self.manifest_path)
        self.assertIn("Len==0 halo(s)", outcomes["len-nonnegative"].detail)

    def test_count_conservation_skips_only_at_api_level(self):
        # run_battery(manifest_path=None) exists for targeted unit tests of the
        # other checks; the SKIP it records must never be reachable from the CLI
        outcomes = self._run(self.hdf5_dir)
        self.assertEqual(outcomes["count-conservation"].status, "SKIP")

    def test_cli_requires_manifest(self):
        with self.assertRaises(SystemExit) as ctx:
            validate.main([str(self.hdf5_dir), "--a-list", str(self.a_list_path)])
        self.assertEqual(ctx.exception.code, 2)

    def test_cli_pass_and_fail(self):
        rc = validate.main(
            [
                str(self.hdf5_dir),
                "--a-list",
                str(self.a_list_path),
                "--manifest",
                str(self.manifest_path),
            ]
        )
        self.assertEqual(rc, 0)
        corrupted = self._copy_dataset()
        (corrupted / snapshot_h5_name(2)).unlink()
        rc = validate.main(
            [
                str(corrupted),
                "--a-list",
                str(self.a_list_path),
                "--manifest",
                str(self.manifest_path),
            ]
        )
        self.assertEqual(rc, 1)

    # -- file set ------------------------------------------------------------

    def test_missing_file(self):
        corrupted = self._copy_dataset()
        (corrupted / snapshot_h5_name(2)).unlink()
        self.assert_fails(self._run(corrupted), "file-set", "missing")

    def test_extra_file(self):
        corrupted = self._copy_dataset()
        with h5py.File(corrupted / "rogue.h5", "w"):
            pass
        self.assert_fails(self._run(corrupted), "file-set", "unexpected")

    def test_missing_sidecar(self):
        corrupted = self._copy_dataset()
        (corrupted / "forests.h5").unlink()
        self.assert_fails(self._run(corrupted), "file-set", "forests.h5")

    # -- structural conformance ----------------------------------------------

    def test_structural_failure_skips_semantics(self):
        corrupted = self._copy_dataset()
        with h5py.File(corrupted / snapshot_h5_name(5), "r+") as handle:
            del handle["halos"]["Vmax"]
        outcomes = self._run(corrupted)
        self.assert_fails(outcomes, "object-set", "Vmax")
        self.assertEqual(outcomes["identity"].status, "SKIP")
        self.assertEqual(outcomes["fof-chains"].status, "SKIP")

    def test_extra_dataset(self):
        corrupted = self._copy_dataset()
        with h5py.File(corrupted / snapshot_h5_name(5), "r+") as handle:
            handle["halos"].create_dataset("Bonus", data=np.zeros(9, dtype=np.int32))
        self.assert_fails(self._run(corrupted), "object-set", "Bonus")

    def test_wrong_dataset_dtype(self):
        corrupted = self._copy_dataset()
        with h5py.File(corrupted / snapshot_h5_name(5), "r+") as handle:
            values = handle["halos"]["Len"][...]
            del handle["halos"]["Len"]
            handle["halos"].create_dataset(
                "Len",
                data=values.astype(np.int64),
                chunks=CHUNK_1D,
                maxshape=(None,),
            )
        self.assert_fails(self._run(corrupted), "object-set", "dtype")

    def test_wrong_chunk_shape(self):
        corrupted = self._copy_dataset()
        with h5py.File(corrupted / snapshot_h5_name(5), "r+") as handle:
            values = handle["halos"]["Len"][...]
            del handle["halos"]["Len"]
            handle["halos"].create_dataset("Len", data=values, chunks=(1024,), maxshape=(None,))
        self.assert_fails(self._run(corrupted), "object-set", "chunks")

    def test_compressed_dataset(self):
        corrupted = self._copy_dataset()
        with h5py.File(corrupted / snapshot_h5_name(5), "r+") as handle:
            values = handle["halos"]["Vmax"][...]
            del handle["halos"]["Vmax"]
            handle["halos"].create_dataset(
                "Vmax", data=values, chunks=CHUNK_1D, maxshape=(None,), compression="gzip"
            )
        self.assert_fails(self._run(corrupted), "object-set", "compressed")

    def test_missing_header_attribute(self):
        corrupted = self._copy_dataset()
        with h5py.File(corrupted / snapshot_h5_name(4), "r+") as handle:
            del handle["header"].attrs["hubble_h"]
        self.assert_fails(self._run(corrupted), "object-set", "hubble_h")

    def test_wrong_attribute_dtype(self):
        corrupted = self._copy_dataset()
        with h5py.File(corrupted / snapshot_h5_name(4), "r+") as handle:
            del handle["header"].attrs["scale_factor"]
            handle["header"].attrs.create("scale_factor", 0.9, dtype=np.float32)
        self.assert_fails(self._run(corrupted), "object-set", "scale_factor")

    def test_unreadable_file_reported_not_crashed(self):
        corrupted = self._copy_dataset()
        (corrupted / snapshot_h5_name(3)).write_bytes(b"not an hdf5 file")
        self.assert_fails(self._run(corrupted), "object-set", "unreadable")

    def test_sidecar_extra_object(self):
        corrupted = self._copy_dataset()
        with h5py.File(corrupted / "forests.h5", "r+") as handle:
            handle.create_dataset("Extra", data=np.zeros(2, dtype=np.int64))
        self.assert_fails(self._run(corrupted), "sidecar-object-set", "Extra")

    # -- header values ---------------------------------------------------------

    def _corrupt_attr(self, name, value, snap=3):
        corrupted = self._copy_dataset()
        with h5py.File(corrupted / snapshot_h5_name(snap), "r+") as handle:
            handle["header"].attrs.modify(name, value)
        return corrupted

    def test_wrong_format_version(self):
        outcomes = self._run(self._corrupt_attr("format_version", 2))
        self.assert_fails(outcomes, "header-values", "format_version")

    def test_wrong_links_adjacent(self):
        outcomes = self._run(self._corrupt_attr("links_adjacent", 0))
        self.assert_fails(outcomes, "header-values", "links_adjacent")

    def test_wrong_snapshot_number(self):
        outcomes = self._run(self._corrupt_attr("snapshot_number", 4))
        self.assert_fails(outcomes, "header-values", "snapshot_number")

    def test_wrong_scale_factor(self):
        outcomes = self._run(self._corrupt_attr("scale_factor", 0.81))
        self.assert_fails(outcomes, "header-values", "scale_factor")

    def test_wrong_n_halos(self):
        outcomes = self._run(self._corrupt_attr("n_halos", 5))
        self.assert_fails(outcomes, "header-values", "n_halos")

    def test_wrong_snapnum_value(self):
        corrupted = self._copy_dataset()
        with h5py.File(corrupted / snapshot_h5_name(3), "r+") as handle:
            handle["halos"]["SnapNum"][...] = np.asarray([2], dtype=np.int32)
        self.assert_fails(self._run(corrupted), "header-values", "SnapNum")

    def test_run_scoped_identity_header_differs(self):
        outcomes = self._run(self._corrupt_attr("n_forests_total", 6))
        self.assert_fails(outcomes, "run-scoped-headers", "n_forests_total")

    def test_physical_header_differs(self):
        outcomes = self._run(self._corrupt_attr("box_size_mpc_h", 99.0))
        self.assert_fails(outcomes, "run-scoped-headers", "box_size_mpc_h")

    # -- manifest binding ------------------------------------------------------

    def test_manifest_binding_wrong_a_list(self):
        # same parsed values, different bytes: the battery's own header checks
        # pass, but the manifest was bound to a different a_list content
        other = Path(tempfile.mkdtemp(dir=self.tmp.name)) / "other.a_list"
        other.write_text("# reformatted copy\n" + Path(self.a_list_path).read_text())
        outcomes = outcome_map(run_battery(self.hdf5_dir, other, manifest_path=self.manifest_path))
        self.assert_fails(outcomes, "manifest-binding", "does not describe the supplied a_list")

    def test_manifest_binding_unrelated_manifest(self):
        with open(self.manifest_path) as handle:
            manifest = json.load(handle)
        for entry in manifest["outputs"].values():
            entry["md5"] = "0" * 32
        tampered = Path(tempfile.mkdtemp(dir=self.tmp.name)) / "manifest.json"
        with open(tampered, "w") as handle:
            json.dump(manifest, handle)
        outcomes = self._run(self.hdf5_dir, manifest_path=tampered)
        self.assert_fails(outcomes, "manifest-binding", "differs from the manifest-recorded")

    def test_manifest_binding_duplicate_basename_refused(self):
        # a workdir written to two output directories records the same
        # basenames under two paths; binding is ambiguous and must refuse
        with open(self.manifest_path) as handle:
            manifest = json.load(handle)
        first_path, first_entry = sorted(manifest["outputs"].items())[0]
        stale = dict(first_entry)
        stale["md5"] = "f" * 32
        manifest["outputs"]["/stale-dir/" + Path(first_path).name] = stale
        tampered = Path(tempfile.mkdtemp(dir=self.tmp.name)) / "manifest.json"
        with open(tampered, "w") as handle:
            json.dump(manifest, handle)
        outcomes = self._run(self.hdf5_dir, manifest_path=tampered)
        self.assert_fails(outcomes, "manifest-binding", "more than one manifest path")

    def test_manifest_binding_uniform_physical_tamper(self):
        # the same wrong box size in EVERY file defeats run-scoped-headers
        # (cross-file equality only); the emission checksums catch it
        corrupted = self._copy_dataset()
        for snap in range(6):
            with h5py.File(corrupted / snapshot_h5_name(snap), "r+") as handle:
                handle["header"].attrs.modify("box_size_mpc_h", 99.0)
        outcomes = self._run(corrupted, manifest_path=self.manifest_path)
        self.assertEqual(outcomes["run-scoped-headers"].status, "PASS")
        self.assert_fails(outcomes, "manifest-binding", "differs from the manifest-recorded")

    # -- slab order ------------------------------------------------------------

    def test_int64_min_mostboundid_caught_in_single_row_slab(self):
        # a one-halo slab has no adjacent-order comparison; the explicit
        # INT64_MIN rejection must catch the overflowing magnitude anyway
        corrupted = self._copy_dataset()
        with h5py.File(corrupted / snapshot_h5_name(3), "r+") as handle:
            handle["halos"]["MostBoundID"][...] = np.asarray(
                [np.iinfo(np.int64).min], dtype=np.int64
            )
        self.assert_fails(self._run(corrupted), "slab-order", "INT64_MIN")

    def test_slab_order_violation(self):
        corrupted = self._copy_dataset()
        with h5py.File(corrupted / snapshot_h5_name(5), "r+") as handle:
            values = handle["halos"]["MostBoundID"][...]
            values[[0, 1]] = values[[1, 0]]
            handle["halos"]["MostBoundID"][...] = values
        self.assert_fails(self._run(corrupted), "slab-order")

    # -- link ranges -------------------------------------------------------------

    def test_descendant_out_of_range(self):
        corrupted = self._copy_dataset()
        with h5py.File(corrupted / snapshot_h5_name(4), "r+") as handle:
            values = handle["halos"]["Descendant"][...]
            values[0] = 9
            handle["halos"]["Descendant"][...] = values
        outcomes = self._run(corrupted)
        self.assert_fails(outcomes, "link-ranges", "Descendant")
        self.assertEqual(outcomes["fof-chains"].status, "SKIP")

    def test_null_first_fof_rejected(self):
        corrupted = self._copy_dataset()
        with h5py.File(corrupted / snapshot_h5_name(5), "r+") as handle:
            values = handle["halos"]["FirstHaloInFOFgroup"][...]
            values[1] = -1
            handle["halos"]["FirstHaloInFOFgroup"][...] = values
        self.assert_fails(self._run(corrupted), "link-ranges", "FirstHaloInFOFgroup")

    # -- FoF chains ----------------------------------------------------------------

    def _corrupt_links(self, snap, field, row, value):
        corrupted = self._copy_dataset()
        with h5py.File(corrupted / snapshot_h5_name(snap), "r+") as handle:
            values = handle["halos"][field][...]
            values[row] = value
            handle["halos"][field][...] = values
        return corrupted

    def test_fof_member_wrong_central(self):
        # snap 5 slab: first_fof [0,0,2,2,4,4,6,6,6]; halo 1 claiming central 2
        # is a chain member whose FirstHaloInFOFgroup is not its chain's central
        outcomes = self._run(self._corrupt_links(5, "FirstHaloInFOFgroup", 1, 2))
        self.assert_fails(outcomes, "fof-chains")

    def test_fof_cycle(self):
        outcomes = self._run(self._corrupt_links(5, "NextHaloInFOFgroup", 1, 0))
        self.assert_fails(outcomes, "fof-chains")

    def test_fof_orphaned_member(self):
        outcomes = self._run(self._corrupt_links(5, "NextHaloInFOFgroup", 0, -1))
        self.assert_fails(outcomes, "fof-chains", "not reachable")

    def test_fof_target_not_central(self):
        # halo 8 names halo 7 (a satellite) as its FoF central
        outcomes = self._run(self._corrupt_links(5, "FirstHaloInFOFgroup", 8, 7))
        self.assert_fails(outcomes, "fof-chains", "self-referencing")

    # -- progenitor closure -------------------------------------------------------

    def test_stray_next_progenitor(self):
        outcomes = self._run(self._corrupt_links(5, "NextProgenitor", 0, 1))
        self.assert_fails(outcomes, "progenitor-closure", "no Descendant")

    def test_sibling_descendant_mismatch(self):
        # snap 4 desc [0,0,1,2,3]; halo 2 (desc 1) claiming sibling 3 (desc 2)
        outcomes = self._run(self._corrupt_links(4, "NextProgenitor", 2, 3))
        self.assert_fails(outcomes, "progenitor-closure")

    def test_duplicate_first_progenitor_claim(self):
        # snap 5 FirstProgenitor [0,2,3,4,...]; halo 1 also claiming progenitor 0
        outcomes = self._run(self._corrupt_links(5, "FirstProgenitor", 1, 0))
        self.assert_fails(outcomes, "progenitor-closure")

    def test_unclaimed_progenitor(self):
        outcomes = self._run(self._corrupt_links(5, "FirstProgenitor", 0, -1))
        self.assert_fails(outcomes, "progenitor-closure", "no progenitor chain")

    def test_descendant_not_pointing_back(self):
        outcomes = self._run(self._corrupt_links(4, "Descendant", 0, 1))
        self.assert_fails(outcomes, "progenitor-closure")

    # -- identity --------------------------------------------------------------------

    def test_duplicate_identity_pair(self):
        outcomes = self._run(self._corrupt_links(5, "HaloRankInForest", 1, 0))
        self.assert_fails(outcomes, "identity", "density/uniqueness")

    def test_forest_index_not_dense(self):
        corrupted = self._copy_dataset()
        with h5py.File(corrupted / snapshot_h5_name(5), "r+") as handle:
            values = handle["halos"]["ForestIndex"][...]
            values[values == 4] = 5
            handle["halos"]["ForestIndex"][...] = values
        self.assert_fails(self._run(corrupted), "identity", "not dense")

    def test_max_rank_header_mismatch(self):
        corrupted = self._copy_dataset()
        for snap in range(6):
            with h5py.File(corrupted / snapshot_h5_name(snap), "r+") as handle:
                handle["header"].attrs.modify("max_halo_rank_in_forest", 7)
        self.assert_fails(self._run(corrupted), "identity", "max_halo_rank_in_forest")

    # -- header bounds ------------------------------------------------------------------

    def test_multiplier_below_max_rank(self):
        outcomes = self._run(self.hdf5_dir, multiplier=4)
        self.assert_fails(outcomes, "header-bounds", "does not exceed")

    def test_multiplier_overflow(self):
        outcomes = self._run(self.hdf5_dir, multiplier=2**62)
        self.assert_fails(outcomes, "header-bounds", "overflows int64")

    # -- Len ----------------------------------------------------------------------------

    def test_negative_len(self):
        outcomes = self._run(self._corrupt_links(5, "Len", 0, -3))
        self.assert_fails(outcomes, "len-nonnegative", "negative Len")

    # -- storage filters ------------------------------------------------------------------

    def test_scaleoffset_filter_rejected(self):
        corrupted = self._copy_dataset()
        with h5py.File(corrupted / snapshot_h5_name(5), "r+") as handle:
            values = handle["halos"]["Len"][...]
            del handle["halos"]["Len"]
            handle["halos"].create_dataset(
                "Len", data=values, chunks=CHUNK_1D, maxshape=(None,), scaleoffset=16
            )
        self.assert_fails(self._run(corrupted), "object-set", "scale-offset")

    def test_sidecar_wrong_chunk_shape(self):
        corrupted = self._copy_dataset()
        with h5py.File(corrupted / "forests.h5", "r+") as handle:
            values = handle["ForestID"][...]
            del handle["ForestID"]
            handle.create_dataset("ForestID", data=values, chunks=(1024,), maxshape=(None,))
        self.assert_fails(self._run(corrupted), "sidecar-object-set", "chunks")

    # -- sidecar content ------------------------------------------------------------------

    def test_sidecar_wrong_length(self):
        corrupted = self._copy_dataset()
        with h5py.File(corrupted / "forests.h5", "r+") as handle:
            values = handle["ForestID"][...]
            del handle["ForestID"]
            handle.create_dataset("ForestID", data=values[:-1], chunks=CHUNK_1D, maxshape=(None,))
        self.assert_fails(self._run(corrupted), "sidecar-content", "n_forests_total")

    # -- count conservation ----------------------------------------------------------------

    def test_count_conservation_mismatch(self):
        with open(self.manifest_path) as handle:
            manifest = json.load(handle)
        for entry in manifest["source_files"].values():
            entry["pre_count"] += 1
        tampered = Path(tempfile.mkdtemp(dir=self.tmp.name)) / "manifest.json"
        with open(tampered, "w") as handle:
            json.dump(manifest, handle)
        outcomes = self._run(self.hdf5_dir, manifest_path=tampered)
        self.assert_fails(outcomes, "count-conservation", "pre-count")


if __name__ == "__main__":
    unittest.main()
