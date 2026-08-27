"""Unit tests for the producer validation battery: it must catch every
deliberately corrupted dataset — each format invariant and battery check is
violated once and the named check must FAIL — and, since the converter scale
pass (plan Slice 6), it must do so from a bounded window rather than from the
whole dataset, reporting exactly what the whole-dataset battery reported."""

import json
import os
import shutil
import sys
import tempfile
import tracemalloc
import unittest
from pathlib import Path
from unittest import mock

import h5py
import numpy as np

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import validate  # noqa: E402
from hdf5_writer import CHUNK_1D, snapshot_h5_name, write_snapshot_file  # noqa: E402
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

    def test_cli_input_error_exit_code(self):
        # the third exit code the contract fixes: an input error is 1, not a
        # traceback and not the argparse 2
        rc = validate.main(
            [
                str(self.hdf5_dir / "not-a-directory"),
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


# ---------------------------------------------------------------------------
# Streaming-equivalence corpus (plan Slice 6)
# ---------------------------------------------------------------------------
#
# The battery below is the STREAMING one. RECORDED holds what the whole-dataset
# battery it replaced (commit c5573d0c, before load_dataset was removed)
# produced for a deliberately injected defect of every detectable class,
# including one per check_identity condition. Every case is compared outcome by
# outcome — name, status AND detail — so a weakened, reordered or reworded check
# fails here, not just a missing one.
#
# Cases run without a manifest unless they are the manifest-binding or
# count-conservation cases themselves; that keeps the recorded details free of
# per-file emission checksums without losing a check, since check_manifest_binding
# has its own recorded cases and is untouched by the streaming rewrite.

STRUCTURAL_NAMES = ("file-set", "object-set", "sidecar-object-set", "manifest-binding")
#: Semantic checks in the order the driver RECORDS them when it runs them...
SEMANTIC_NAMES = (
    "header-values",
    "run-scoped-headers",
    "slab-order",
    "link-ranges",
    "fof-chains",
    "progenitor-closure",
    "identity",
    "header-bounds",
    "sidecar-content",
    "len-nonnegative",
    "count-conservation",
)
#: ...and in the (different, pre-existing) order it records them as SKIP when
#: structural conformance failed.
SEMANTIC_NAMES_SKIPPED = (
    "header-values",
    "run-scoped-headers",
    "slab-order",
    "link-ranges",
    "fof-chains",
    "progenitor-closure",
    "identity",
    "header-bounds",
    "len-nonnegative",
    "sidecar-content",
    "count-conservation",
)
STRUCTURAL_SKIP = "structural conformance failed; semantics not trusted"
NO_MANIFEST_BINDING_SKIP = "no manifest given (API mode; unreachable from the CLI)"
NO_MANIFEST_COUNT_SKIP = "no --manifest given; independent pre-counts unavailable"
LEN_PASS_DETAIL = "1 Len==0 halo(s)"

RECORDED = {
    "pristine": {},
    "missing_file": {
        "file_set_failed": True,
        "semantics_skipped": True,
        "no_manifest": True,
        "outcomes": {
            "file-set": ("FAIL", "1 missing file(s): snapshot_002.h5"),
        },
    },
    "extra_file": {
        "file_set_failed": True,
        "semantics_skipped": True,
        "no_manifest": True,
        "outcomes": {
            "file-set": ("FAIL", "1 unexpected .h5 file(s): rogue.h5"),
        },
    },
    "missing_sidecar": {
        "file_set_failed": True,
        "semantics_skipped": True,
        "no_manifest": True,
        "outcomes": {
            "file-set": ("FAIL", "1 missing file(s): forests.h5"),
        },
    },
    "missing_dataset": {
        "semantics_skipped": True,
        "no_manifest": True,
        "outcomes": {
            "object-set": (
                "FAIL",
                "snapshot_005.h5: /halos dataset set mismatch: missing ['Vmax'], " "extra []",
            ),
        },
    },
    "extra_dataset": {
        "semantics_skipped": True,
        "no_manifest": True,
        "outcomes": {
            "object-set": (
                "FAIL",
                "snapshot_005.h5: /halos dataset set mismatch: missing [], extra " "['Bonus']",
            ),
        },
    },
    "wrong_dataset_dtype": {
        "semantics_skipped": True,
        "no_manifest": True,
        "outcomes": {
            "object-set": ("FAIL", "snapshot_005.h5: /halos/Len dtype int64 != contract int32"),
        },
    },
    "wrong_chunk_shape": {
        "semantics_skipped": True,
        "no_manifest": True,
        "outcomes": {
            "object-set": (
                "FAIL",
                "snapshot_005.h5: /halos/Len chunks (1024,) != contract (65536,)",
            ),
        },
    },
    "compressed_dataset": {
        "semantics_skipped": True,
        "no_manifest": True,
        "outcomes": {
            "object-set": (
                "FAIL",
                "snapshot_005.h5: /halos/Vmax is compressed (gzip); the contract "
                "forbids compression",
            ),
        },
    },
    "scaleoffset_filter": {
        "semantics_skipped": True,
        "no_manifest": True,
        "outcomes": {
            "object-set": (
                "FAIL",
                "snapshot_005.h5: /halos/Len uses the scale-offset filter; the "
                "contract forbids filters",
            ),
        },
    },
    "missing_header_attribute": {
        "semantics_skipped": True,
        "no_manifest": True,
        "outcomes": {
            "object-set": (
                "FAIL",
                "snapshot_004.h5: header attribute set mismatch: missing " "['hubble_h'], extra []",
            ),
        },
    },
    "wrong_attribute_dtype": {
        "semantics_skipped": True,
        "no_manifest": True,
        "outcomes": {
            "object-set": (
                "FAIL",
                "snapshot_004.h5: attribute scale_factor has dtype float32 shape "
                "(), contract requires scalar float64",
            ),
        },
    },
    "unreadable_file": {
        "semantics_skipped": True,
        "no_manifest": True,
        "outcomes": {
            "object-set": (
                "FAIL",
                "snapshot_003.h5: unreadable as HDF5 (Unable to synchronously open "
                "file (file signature not found))",
            ),
        },
    },
    "sidecar_extra_object": {
        "semantics_skipped": True,
        "no_manifest": True,
        "outcomes": {
            "sidecar-object-set": ("FAIL", "object set ['Extra', 'ForestID'] != {'ForestID'}"),
        },
    },
    "sidecar_wrong_chunks": {
        "semantics_skipped": True,
        "no_manifest": True,
        "outcomes": {
            "sidecar-object-set": ("FAIL", "/ForestID chunks (1024,) != contract (65536,)"),
        },
    },
    "sidecar_wrong_length": {
        "no_manifest": True,
        "outcomes": {
            "sidecar-content": ("FAIL", "forests.h5 /ForestID has 4 entries, n_forests_total is 5"),
        },
    },
    "header_format_version": {
        "no_manifest": True,
        "outcomes": {
            "header-values": ("FAIL", "snapshot_003.h5: format_version 2 != 1"),
        },
    },
    "header_links_adjacent": {
        "no_manifest": True,
        "outcomes": {
            "header-values": ("FAIL", "snapshot_003.h5: links_adjacent 0 != 1"),
        },
    },
    "header_snapshot_number": {
        "no_manifest": True,
        "outcomes": {
            "header-values": ("FAIL", "snapshot_003.h5: snapshot_number 4 != filename index 3"),
        },
    },
    "header_scale_factor": {
        "no_manifest": True,
        "outcomes": {
            "header-values": ("FAIL", "snapshot_003.h5: scale_factor 0.81 != a_list[3] = 0.8"),
        },
    },
    "header_n_halos": {
        "no_manifest": True,
        "outcomes": {
            "header-values": (
                "FAIL",
                "snapshot_003.h5: dataset Descendant has 1 rows, header n_halos is "
                "5; snapshot_003.h5: dataset FirstProgenitor has 1 rows, header "
                "n_halos is 5; snapshot_003.h5: dataset NextProgenitor has 1 rows, "
                "header n_halos is 5; snapshot_003.h5: dataset FirstHaloInFOFgroup "
                "has 1 rows, header n_halos is 5; snapshot_003.h5: dataset "
                "NextHaloInFOFgroup has 1 rows, header n_halos is 5; "
                "snapshot_003.h5: dataset Len has 1 rows, header n_halos is 5; "
                "snapshot_003.h5: dataset SnapNum has 1 rows, header n_halos is 5; "
                "snapshot_003.h5: dataset M_Crit200 has 1 rows, header n_halos is "
                "5; snapshot_003.h5: dataset Pos has 1 rows, header n_halos is 5; "
                "snapshot_003.h5: dataset Vel has 1 rows, header n_halos is 5; "
                "snapshot_003.h5: dataset Spin has 1 rows, header n_halos is 5; "
                "snapshot_003.h5: dataset VelDisp has 1 rows, header n_halos is 5; "
                "snapshot_003.h5: dataset Vmax has 1 rows, header n_halos is 5; "
                "snapshot_003.h5: dataset MostBoundID has 1 rows, header n_halos is "
                "5; snapshot_003.h5: dataset ForestIndex has 1 rows, header n_halos "
                "is 5; snapshot_003.h5: dataset HaloRankInForest has 1 rows, header "
                "n_halos is 5",
            ),
        },
    },
    "snapnum_value": {
        "no_manifest": True,
        "outcomes": {
            "header-values": (
                "FAIL",
                "snapshot_003.h5: 1 SnapNum value(s) != snapshot_number 3; " "examples: 2",
            ),
        },
    },
    "run_scoped_n_forests_total": {
        "no_manifest": True,
        "outcomes": {
            "run-scoped-headers": ("FAIL", "n_forests_total differs across files: 5, 6"),
            "identity": ("SKIP", "run-scoped headers inconsistent"),
            "header-bounds": ("SKIP", "run-scoped headers inconsistent"),
            "sidecar-content": ("SKIP", "run-scoped headers inconsistent"),
        },
    },
    "run_scoped_box_size": {
        "no_manifest": True,
        "outcomes": {
            "run-scoped-headers": ("FAIL", "box_size_mpc_h differs across files: 100.0, 99.0"),
            "identity": ("SKIP", "run-scoped headers inconsistent"),
            "header-bounds": ("SKIP", "run-scoped headers inconsistent"),
            "sidecar-content": ("SKIP", "run-scoped headers inconsistent"),
        },
    },
    "int64_min_mostboundid": {
        "no_manifest": True,
        "outcomes": {
            "slab-order": (
                "FAIL",
                "snapshot_003.h5: 1 MostBoundID value(s) equal INT64_MIN, whose "
                "magnitude overflows signed int64; example rows: 0",
            ),
        },
    },
    "slab_order_swap": {
        "no_manifest": True,
        "outcomes": {
            "slab-order": (
                "FAIL",
                "snapshot_005.h5: not strictly ascending in |MostBoundID| at 1 "
                "position(s); examples: (row=0, |MostBoundID|=1020, next 1010)",
            ),
        },
    },
    "descendant_out_of_range": {
        "no_manifest": True,
        "outcomes": {
            "link-ranges": (
                "FAIL",
                "snapshot_004.h5: 1 Descendant value(s) outside [-1, 9); examples: " "9",
            ),
            "fof-chains": ("SKIP", "link ranges invalid; chains not walked"),
            "progenitor-closure": ("SKIP", "link ranges invalid; chains not walked"),
        },
    },
    "null_first_fof": {
        "no_manifest": True,
        "outcomes": {
            "link-ranges": (
                "FAIL",
                "snapshot_005.h5: 1 FirstHaloInFOFgroup value(s) outside [0, 9); " "examples: -1",
            ),
            "fof-chains": ("SKIP", "link ranges invalid; chains not walked"),
            "progenitor-closure": ("SKIP", "link ranges invalid; chains not walked"),
        },
    },
    "fof_member_wrong_central": {
        "no_manifest": True,
        "outcomes": {
            "fof-chains": (
                "FAIL",
                "snapshot_005.h5: 1 chain member(s) whose FirstHaloInFOFgroup is "
                "not the chain's central; example rows: 1",
            ),
        },
    },
    "fof_cycle": {
        "no_manifest": True,
        "outcomes": {
            "fof-chains": (
                "FAIL",
                "snapshot_005.h5: FoF chain cycle or duplicate membership at row(s) " "0",
            ),
        },
    },
    "fof_orphaned_member": {
        "no_manifest": True,
        "outcomes": {
            "fof-chains": (
                "FAIL",
                "snapshot_005.h5: 1 halo(s) not reachable from any FoF central "
                "(orphaned or cyclic chain); example rows: 1",
            ),
        },
    },
    "fof_target_not_central": {
        "no_manifest": True,
        "outcomes": {
            "fof-chains": (
                "FAIL",
                "snapshot_005.h5: 1 FirstHaloInFOFgroup target(s) are not "
                "self-referencing centrals; example rows: 8",
            ),
        },
    },
    "stray_next_progenitor": {
        "no_manifest": True,
        "outcomes": {
            "progenitor-closure": (
                "FAIL",
                "snapshot_005.h5: 1 halo(s) carry NextProgenitor but no Descendant; "
                "example rows: 0",
            ),
        },
    },
    "sibling_descendant_mismatch": {
        "no_manifest": True,
        "outcomes": {
            "progenitor-closure": (
                "FAIL",
                "snapshot_004.h5: 1 NextProgenitor sibling(s) with a different "
                "Descendant; example rows: 2; snapshot_004.h5: progenitor chain "
                "cycle or duplicate membership at row(s) 3",
            ),
        },
    },
    "duplicate_first_progenitor": {
        "no_manifest": True,
        "outcomes": {
            "progenitor-closure": (
                "FAIL",
                "snapshot_004.h5: progenitor chain(s) converge on the same halo; "
                "example rows: 0",
            ),
        },
    },
    "unclaimed_progenitor": {
        "no_manifest": True,
        "outcomes": {
            "progenitor-closure": (
                "FAIL",
                "snapshot_004.h5: 2 halo(s) with a Descendant appear in no "
                "progenitor chain; example rows: 0, 1",
            ),
        },
    },
    "descendant_not_pointing_back": {
        "no_manifest": True,
        "outcomes": {
            "progenitor-closure": (
                "FAIL",
                "snapshot_004.h5: 1 NextProgenitor sibling(s) with a different "
                "Descendant; example rows: 0; snapshot_004.h5: 1 progenitor chain "
                "member(s) whose Descendant is not the chain owner; example rows: 0",
            ),
        },
    },
    "identity_a_forest_not_dense": {
        "no_manifest": True,
        "outcomes": {
            "identity": (
                "FAIL",
                "ForestIndex values are not dense over [0, 5); 5 distinct value(s) "
                "observed, examples: 0, 1, 2, 3, 5",
            ),
        },
    },
    "identity_b_duplicate_pair": {
        "no_manifest": True,
        "outcomes": {
            "identity": (
                "FAIL",
                "1 (ForestIndex, HaloRankInForest) pair(s) violate per-forest "
                "density/uniqueness; examples: (ForestIndex=0, rank=0, expected 1)",
            ),
        },
    },
    "identity_c_max_rank_header": {
        "no_manifest": True,
        "outcomes": {
            "identity": (
                "FAIL",
                "measured max HaloRankInForest 5 != header max_halo_rank_in_forest " "7",
            ),
        },
    },
    "negative_len": {
        "no_manifest": True,
        "outcomes": {
            "len-nonnegative": ("FAIL", "snapshot_005.h5: 1 negative Len value(s); examples: -3"),
        },
    },
    "multiplier_below_max_rank": {
        "outcomes": {
            "header-bounds": (
                "FAIL",
                "identity multiplier 4 does not exceed max_halo_rank_in_forest 5",
            ),
        },
    },
    "multiplier_overflow": {
        "outcomes": {
            "header-bounds": (
                "FAIL",
                "multiplier 4611686018427387904 x (n_forests_total 5 + 1) overflows " "int64",
            ),
        },
    },
    "manifest_wrong_a_list": {
        "outcomes": {
            "manifest-binding": (
                "FAIL",
                "supplied a_list content md5 0cc4f0e9388061a8eeb2562b2f95f862 != "
                "manifest-recorded eb1d4c6c5d32b275bdec18c6db6764aa — this manifest "
                "does not describe the supplied a_list",
            ),
        },
    },
    "manifest_bad_md5": {
        "outcomes": {
            "manifest-binding": (
                "FAIL",
                "7 file(s) whose content differs from the manifest-recorded "
                "emission checksum: forests.h5, snapshot_000.h5, snapshot_001.h5, "
                "snapshot_002.h5, snapshot_003.h5",
            ),
        },
    },
    "manifest_duplicate_basename": {
        "outcomes": {
            "manifest-binding": (
                "FAIL",
                "1 output basename(s) recorded under more than one manifest path "
                "(the workdir was written to multiple output directories?) — "
                "binding is ambiguous, refusing to validate: forests.h5",
            ),
        },
    },
    "manifest_uniform_physical_tamper": {
        "outcomes": {
            "manifest-binding": (
                "FAIL",
                "6 file(s) whose content differs from the manifest-recorded "
                "emission checksum: snapshot_000.h5, snapshot_001.h5, "
                "snapshot_002.h5, snapshot_003.h5, snapshot_004.h5",
            ),
        },
    },
    "count_conservation": {
        "outcomes": {
            "count-conservation": (
                "FAIL",
                "emitted halo total 17 != independent source pre-count total 18",
            ),
        },
    },
    "api_mode_no_manifest": {
        "no_manifest": True,
    },
}


def recorded_outcomes(case):
    """Expand one RECORDED case into the full ordered (name, status, detail)
    list the whole-dataset battery produced, filling in the defaults the table
    leaves implicit: a PASS with an empty detail, the Len==0 count on
    len-nonnegative, and the two SKIPs an API-mode run without a manifest
    records."""
    entry = RECORDED[case]
    explicit = entry.get("outcomes", {})
    names = list(STRUCTURAL_NAMES)
    if entry.get("file_set_failed"):
        # the driver never reaches the per-file structural checks
        names = [name for name in names if name not in ("object-set", "sidecar-object-set")]
    semantic = SEMANTIC_NAMES_SKIPPED if entry.get("semantics_skipped") else SEMANTIC_NAMES
    expanded = []
    for name in names + list(semantic):
        if name in explicit:
            status, detail = explicit[name]
        elif name in semantic and entry.get("semantics_skipped"):
            status, detail = "SKIP", STRUCTURAL_SKIP
        elif name == "manifest-binding" and entry.get("no_manifest"):
            status, detail = "SKIP", NO_MANIFEST_BINDING_SKIP
        elif name == "count-conservation" and entry.get("no_manifest"):
            status, detail = "SKIP", NO_MANIFEST_COUNT_SKIP
        elif name == "len-nonnegative":
            status, detail = "PASS", LEN_PASS_DETAIL
        else:
            status, detail = "PASS", ""
        expanded.append((name, status, detail))
    return expanded


class TestStreamingEquivalence(unittest.TestCase):
    """Every RECORDED defect must produce, from the streaming battery, exactly
    the outcome list the whole-dataset battery produced."""

    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        root = Path(cls.tmp.name)
        cls.workdir, cls.a_list_path, cls.sim_info, cls.hdf5_dir = make_written_workdir(root)
        cls.manifest_path = cls.workdir / "manifest.json"

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    # -- dataset and manifest mutations --------------------------------------

    def _copy_dataset(self):
        target = Path(tempfile.mkdtemp(dir=self.tmp.name)) / "hdf5"
        shutil.copytree(self.hdf5_dir, target)
        return target

    def _attr(self, name, value, snap=3):
        corrupted = self._copy_dataset()
        with h5py.File(corrupted / snapshot_h5_name(snap), "r+") as handle:
            handle["header"].attrs.modify(name, value)
        return corrupted

    def _all_attrs(self, name, value):
        corrupted = self._copy_dataset()
        for snap in range(6):
            with h5py.File(corrupted / snapshot_h5_name(snap), "r+") as handle:
                handle["header"].attrs.modify(name, value)
        return corrupted

    def _field(self, snap, field, row, value):
        corrupted = self._copy_dataset()
        with h5py.File(corrupted / snapshot_h5_name(snap), "r+") as handle:
            values = handle["halos"][field][...]
            values[row] = value
            handle["halos"][field][...] = values
        return corrupted

    def _replace_dataset(self, group, name, values, snap=5, **kwargs):
        corrupted = self._copy_dataset()
        path = corrupted / ("forests.h5" if group is None else snapshot_h5_name(snap))
        with h5py.File(path, "r+") as handle:
            target = handle if group is None else handle[group]
            data = values(target[name][...])
            del target[name]
            options = dict(chunks=CHUNK_1D, maxshape=(None,))
            options.update(kwargs)
            target.create_dataset(name, data=data, **options)
        return corrupted

    def _tampered_manifest(self, mutate):
        with open(self.manifest_path) as handle:
            manifest = json.load(handle)
        mutate(manifest)
        path = Path(tempfile.mkdtemp(dir=self.tmp.name)) / "manifest.json"
        with open(path, "w") as handle:
            json.dump(manifest, handle)
        return path

    def _build(self, case):
        """Return the (directory, a_list, manifest_path, multiplier) arguments
        for one RECORDED case."""
        directory = self.hdf5_dir
        a_list = self.a_list_path
        manifest = None
        multiplier = DEFAULT_MULTIPLIER
        if case == "pristine":
            manifest = self.manifest_path
        elif case == "missing_file":
            directory = self._copy_dataset()
            (directory / snapshot_h5_name(2)).unlink()
        elif case == "extra_file":
            directory = self._copy_dataset()
            with h5py.File(directory / "rogue.h5", "w"):
                pass
        elif case == "missing_sidecar":
            directory = self._copy_dataset()
            (directory / "forests.h5").unlink()
        elif case == "missing_dataset":
            directory = self._copy_dataset()
            with h5py.File(directory / snapshot_h5_name(5), "r+") as handle:
                del handle["halos"]["Vmax"]
        elif case == "extra_dataset":
            directory = self._copy_dataset()
            with h5py.File(directory / snapshot_h5_name(5), "r+") as handle:
                handle["halos"].create_dataset("Bonus", data=np.zeros(9, dtype=np.int32))
        elif case == "wrong_dataset_dtype":
            directory = self._replace_dataset("halos", "Len", lambda v: v.astype(np.int64))
        elif case == "wrong_chunk_shape":
            directory = self._replace_dataset("halos", "Len", lambda v: v, chunks=(1024,))
        elif case == "compressed_dataset":
            directory = self._replace_dataset("halos", "Vmax", lambda v: v, compression="gzip")
        elif case == "scaleoffset_filter":
            directory = self._replace_dataset("halos", "Len", lambda v: v, scaleoffset=16)
        elif case == "missing_header_attribute":
            directory = self._copy_dataset()
            with h5py.File(directory / snapshot_h5_name(4), "r+") as handle:
                del handle["header"].attrs["hubble_h"]
        elif case == "wrong_attribute_dtype":
            directory = self._copy_dataset()
            with h5py.File(directory / snapshot_h5_name(4), "r+") as handle:
                del handle["header"].attrs["scale_factor"]
                handle["header"].attrs.create("scale_factor", 0.9, dtype=np.float32)
        elif case == "unreadable_file":
            directory = self._copy_dataset()
            (directory / snapshot_h5_name(3)).write_bytes(b"not an hdf5 file")
        elif case == "sidecar_extra_object":
            directory = self._copy_dataset()
            with h5py.File(directory / "forests.h5", "r+") as handle:
                handle.create_dataset("Extra", data=np.zeros(2, dtype=np.int64))
        elif case == "sidecar_wrong_chunks":
            directory = self._replace_dataset(None, "ForestID", lambda v: v, chunks=(1024,))
        elif case == "sidecar_wrong_length":
            directory = self._replace_dataset(None, "ForestID", lambda v: v[:-1])
        elif case == "header_format_version":
            directory = self._attr("format_version", 2)
        elif case == "header_links_adjacent":
            directory = self._attr("links_adjacent", 0)
        elif case == "header_snapshot_number":
            directory = self._attr("snapshot_number", 4)
        elif case == "header_scale_factor":
            directory = self._attr("scale_factor", 0.81)
        elif case == "header_n_halos":
            directory = self._attr("n_halos", 5)
        elif case == "snapnum_value":
            directory = self._copy_dataset()
            with h5py.File(directory / snapshot_h5_name(3), "r+") as handle:
                handle["halos"]["SnapNum"][...] = np.asarray([2], dtype=np.int32)
        elif case == "run_scoped_n_forests_total":
            directory = self._attr("n_forests_total", 6)
        elif case == "run_scoped_box_size":
            directory = self._attr("box_size_mpc_h", 99.0)
        elif case == "int64_min_mostboundid":
            directory = self._copy_dataset()
            with h5py.File(directory / snapshot_h5_name(3), "r+") as handle:
                handle["halos"]["MostBoundID"][...] = np.asarray(
                    [np.iinfo(np.int64).min], dtype=np.int64
                )
        elif case == "slab_order_swap":
            directory = self._copy_dataset()
            with h5py.File(directory / snapshot_h5_name(5), "r+") as handle:
                values = handle["halos"]["MostBoundID"][...]
                values[[0, 1]] = values[[1, 0]]
                handle["halos"]["MostBoundID"][...] = values
        elif case == "descendant_out_of_range":
            directory = self._field(4, "Descendant", 0, 9)
        elif case == "null_first_fof":
            directory = self._field(5, "FirstHaloInFOFgroup", 1, -1)
        elif case == "fof_member_wrong_central":
            directory = self._field(5, "FirstHaloInFOFgroup", 1, 2)
        elif case == "fof_cycle":
            directory = self._field(5, "NextHaloInFOFgroup", 1, 0)
        elif case == "fof_orphaned_member":
            directory = self._field(5, "NextHaloInFOFgroup", 0, -1)
        elif case == "fof_target_not_central":
            directory = self._field(5, "FirstHaloInFOFgroup", 8, 7)
        elif case == "stray_next_progenitor":
            directory = self._field(5, "NextProgenitor", 0, 1)
        elif case == "sibling_descendant_mismatch":
            directory = self._field(4, "NextProgenitor", 2, 3)
        elif case == "duplicate_first_progenitor":
            directory = self._field(5, "FirstProgenitor", 1, 0)
        elif case == "unclaimed_progenitor":
            directory = self._field(5, "FirstProgenitor", 0, -1)
        elif case == "descendant_not_pointing_back":
            directory = self._field(4, "Descendant", 0, 1)
        elif case == "identity_a_forest_not_dense":
            directory = self._copy_dataset()
            with h5py.File(directory / snapshot_h5_name(5), "r+") as handle:
                values = handle["halos"]["ForestIndex"][...]
                values[values == 4] = 5
                handle["halos"]["ForestIndex"][...] = values
        elif case == "identity_b_duplicate_pair":
            directory = self._field(5, "HaloRankInForest", 1, 0)
        elif case == "identity_c_max_rank_header":
            directory = self._all_attrs("max_halo_rank_in_forest", 7)
        elif case == "negative_len":
            directory = self._field(5, "Len", 0, -3)
        elif case == "multiplier_below_max_rank":
            manifest, multiplier = self.manifest_path, 4
        elif case == "multiplier_overflow":
            manifest, multiplier = self.manifest_path, 2**62
        elif case == "manifest_wrong_a_list":
            manifest = self.manifest_path
            a_list = Path(tempfile.mkdtemp(dir=self.tmp.name)) / "other.a_list"
            a_list.write_text("# reformatted copy\n" + Path(self.a_list_path).read_text())
        elif case == "manifest_bad_md5":

            def blank_md5(manifest_data):
                for entry in manifest_data["outputs"].values():
                    entry["md5"] = "0" * 32

            manifest = self._tampered_manifest(blank_md5)
        elif case == "manifest_duplicate_basename":

            def duplicate_basename(manifest_data):
                first_path, first_entry = sorted(manifest_data["outputs"].items())[0]
                stale = dict(first_entry)
                stale["md5"] = "f" * 32
                manifest_data["outputs"]["/stale-dir/" + Path(first_path).name] = stale

            manifest = self._tampered_manifest(duplicate_basename)
        elif case == "manifest_uniform_physical_tamper":
            manifest = self.manifest_path
            directory = self._all_attrs("box_size_mpc_h", 99.0)
        elif case == "count_conservation":

            def bump_pre_count(manifest_data):
                for entry in manifest_data["source_files"].values():
                    entry["pre_count"] += 1

            manifest = self._tampered_manifest(bump_pre_count)
        elif case == "api_mode_no_manifest":
            pass
        else:
            raise AssertionError("no builder for recorded case {!r}".format(case))
        return directory, a_list, manifest, multiplier

    def test_every_recorded_case_has_a_builder(self):
        # a case whose builder is missing must fail loudly, not silently vanish
        for case in RECORDED:
            self.assertIsNotNone(self._build(case)[0], case)

    def test_streaming_battery_matches_recorded_outcomes(self):
        for case in RECORDED:
            with self.subTest(case=case):
                directory, a_list, manifest, multiplier = self._build(case)
                outcomes = run_battery(
                    directory, a_list, manifest_path=manifest, multiplier=multiplier
                )
                actual = [(o.name, o.status, o.detail) for o in outcomes]
                self.assertEqual(actual, recorded_outcomes(case))

    def test_corpus_covers_every_check_and_identity_condition(self):
        # the corpus is only evidence if every named check actually FAILs
        # somewhere in it, and each identity condition on its own
        failing = set()
        for entry in RECORDED.values():
            for name, (status, _) in entry.get("outcomes", {}).items():
                if status == "FAIL":
                    failing.add(name)
        self.assertEqual(failing, set(STRUCTURAL_NAMES) | set(SEMANTIC_NAMES))
        self.assertEqual(set(SEMANTIC_NAMES), set(SEMANTIC_NAMES_SKIPPED))
        conditions = {
            "identity_a_forest_not_dense": "not dense",
            "identity_b_duplicate_pair": "density/uniqueness",
            "identity_c_max_rank_header": "max_halo_rank_in_forest",
        }
        for case, fragment in conditions.items():
            status, detail = RECORDED[case]["outcomes"]["identity"]
            self.assertEqual(status, "FAIL", case)
            self.assertIn(fragment, detail)


# ---------------------------------------------------------------------------
# Bounded memory (plan Slice 6)
# ---------------------------------------------------------------------------


def write_synthetic_dataset(directory, n_snapshots: int, n_forests: int) -> Path:
    """Emit a fully conformant dataset of ``n_forests`` halos per snapshot, one
    halo per forest, and return the a_list path beside it.

    Every halo is its own FoF central, descends into the same row of the next
    snapshot, and holds ``rank == snap`` within its forest, so the battery
    passes every check. Halo count grows with ``n_snapshots`` while the forest
    count and the per-snapshot window stay FIXED, which is what lets the memory
    test attribute any growth in peak allocation to the dataset rather than to
    the window.
    """
    directory = Path(directory)
    directory.mkdir(parents=True, exist_ok=True)
    a_list = [round(0.1 + 0.8 * snap / max(n_snapshots - 1, 1), 12) for snap in range(n_snapshots)]
    metadata = {
        "box_size_mpc_h": 100.0,
        "particle_mass_msun_h": 3.25e8,
        "omega_matter": 0.3089,
        "omega_lambda": 0.6911,
        "hubble_h": 0.6774,
    }
    rows32 = np.arange(n_forests, dtype=np.int32)
    null32 = np.full(n_forests, -1, dtype=np.int32)
    for snap in range(n_snapshots):
        arrays = {
            "Descendant": rows32 if snap < n_snapshots - 1 else null32,
            "FirstProgenitor": rows32 if snap > 0 else null32,
            "NextProgenitor": null32,
            "FirstHaloInFOFgroup": rows32,
            "NextHaloInFOFgroup": null32,
            "Len": np.full(n_forests, 100, dtype=np.int32),
            "SnapNum": np.full(n_forests, snap, dtype=np.int32),
            "M_Crit200": np.zeros(n_forests, dtype=np.float32),
            "Pos": np.zeros((n_forests, 3), dtype=np.float32),
            "Vel": np.zeros((n_forests, 3), dtype=np.float32),
            "Spin": np.zeros((n_forests, 3), dtype=np.float32),
            "VelDisp": np.zeros(n_forests, dtype=np.float32),
            "Vmax": np.zeros(n_forests, dtype=np.float32),
            "MostBoundID": np.arange(1, n_forests + 1, dtype=np.int64),
            "ForestIndex": np.arange(n_forests, dtype=np.int64),
            "HaloRankInForest": np.full(n_forests, snap, dtype=np.int64),
        }
        write_snapshot_file(
            directory / snapshot_h5_name(snap),
            snap,
            arrays,
            a_list[snap],
            metadata,
            n_forests,
            n_snapshots - 1,
        )
    with h5py.File(directory / "forests.h5", "w") as handle:
        handle.create_dataset(
            "ForestID",
            data=np.arange(n_forests, dtype=np.int64),
            chunks=CHUNK_1D,
            maxshape=(None,),
            compression=None,
        )
    a_list_path = directory.parent / "{}.a_list".format(directory.name)
    a_list_path.write_text("".join("{!r}\n".format(value) for value in a_list))
    return a_list_path


class TestBoundedMemory(unittest.TestCase):
    """Resident bytes must be bounded by the two-snapshot window plus the
    forest-count-sized metadata plus the identity structure — NOT by the
    dataset. Measured with tracemalloc, which traces numpy's and h5py's own
    allocations, so this asserts ACTUAL peak allocation rather than any counter
    the battery keeps about itself."""

    HALOS_PER_SNAPSHOT = 4096
    SMALL_SNAPSHOTS = 4
    LARGE_SNAPSHOTS = 64

    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        root = Path(cls.tmp.name)
        cls.small = root / "small"
        cls.large = root / "large"
        cls.small_a_list = write_synthetic_dataset(
            cls.small, cls.SMALL_SNAPSHOTS, cls.HALOS_PER_SNAPSHOT
        )
        cls.large_a_list = write_synthetic_dataset(
            cls.large, cls.LARGE_SNAPSHOTS, cls.HALOS_PER_SNAPSHOT
        )

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    @staticmethod
    def _peak_bytes(directory, a_list_path):
        tracemalloc.start()
        try:
            tracemalloc.reset_peak()
            outcomes = run_battery(directory, a_list_path)
            peak = tracemalloc.get_traced_memory()[1]
        finally:
            tracemalloc.stop()
        return peak, outcomes

    def test_synthetic_datasets_are_conformant(self):
        # the memory numbers below only mean anything if every check actually
        # ran to completion on both datasets
        for directory, a_list_path in (
            (self.small, self.small_a_list),
            (self.large, self.large_a_list),
        ):
            outcomes = run_battery(directory, a_list_path)
            unexpected = [
                o.line()
                for o in outcomes
                if o.status != "PASS" and o.name not in ("manifest-binding", "count-conservation")
            ]
            self.assertEqual(unexpected, [], str(directory))

    def test_peak_allocation_does_not_scale_with_the_dataset(self):
        # warm up: first-call imports and h5py's own caches are not the subject
        self._peak_bytes(self.small, self.small_a_list)
        small_peak, _ = self._peak_bytes(self.small, self.small_a_list)
        large_peak, _ = self._peak_bytes(self.large, self.large_a_list)

        def dataset_bytes(directory):
            return sum(path.stat().st_size for path in Path(directory).glob("*.h5"))

        grew_by = dataset_bytes(self.large) - dataset_bytes(self.small)
        self.assertGreater(grew_by, 10 * 1024**2, "the two datasets must differ materially")
        # the window, the forest tables and the chunk buffers are identical
        # between the two runs; only the identity bitset grows, by one bit per
        # extra halo, so the whole allowance below is dominated by slack
        extra_halos = (self.LARGE_SNAPSHOTS - self.SMALL_SNAPSHOTS) * self.HALOS_PER_SNAPSHOT
        allowance = extra_halos // 8 + 64 * 1024
        self.assertLess(large_peak - small_peak, allowance)
        # the allowance is tight enough to catch a regression that made even
        # ONE four-byte column whole-dataset-resident again
        self.assertLess(allowance, extra_halos * 4)

    def test_peak_allocation_is_within_the_declared_bound(self):
        peak, _ = self._peak_bytes(self.large, self.large_a_list)
        halos = self.LARGE_SNAPSHOTS * self.HALOS_PER_SNAPSHOT
        # 100 bytes per halo is the emitted dataset's own per-halo footprint
        # across all 16 /halos datasets, so this is a generous whole-window term
        window = 2 * self.HALOS_PER_SNAPSHOT * 100
        forest_tables = 4 * 8 * self.HALOS_PER_SNAPSHOT
        identity_structure = (halos + 7) // 8
        chunk = min(validate.IDENTITY_CHUNK_ROWS, self.HALOS_PER_SNAPSHOT) * (
            validate.IDENTITY_CHUNK_BYTES_PER_ROW
        )
        bound = window + forest_tables + identity_structure + chunk + 1024**2
        self.assertLess(peak, bound)
        # ...and the pre-streaming battery, which held every file's /halos
        # arrays at once, could not have met it
        self.assertGreater(halos * 100, bound)


class TestIdentityStructure(unittest.TestCase):
    """The halo-count-sized identity structure is exact, bit-packed, and
    released on the success, failure and exception paths."""

    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        root = Path(cls.tmp.name)
        cls.directory = root / "dataset"
        cls.a_list_path = write_synthetic_dataset(cls.directory, 3, 8)
        cls.snapshots = validate._Snapshots(cls.directory, 3)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def _tracked(self):
        """Patch in a _IdentityBits that records every instance made."""
        made = []
        original = validate._IdentityBits

        class Tracked(original):
            def __init__(self, n_slots):
                super().__init__(n_slots)
                made.append(self)

        validate._IdentityBits = Tracked
        self.addCleanup(setattr, validate, "_IdentityBits", original)
        return made

    def test_structure_is_bit_packed_and_reported(self):
        made = self._tracked()
        failures, peak_bytes = validate.check_identity(self.snapshots, 8, 2)
        self.assertEqual(failures, [])
        self.assertEqual(len(made), 1)
        self.assertEqual(peak_bytes, (3 * 8 + 7) // 8)
        self.assertEqual(peak_bytes, made[0].peak_bytes)

    def test_structure_released_on_success(self):
        made = self._tracked()
        failures, _ = validate.check_identity(self.snapshots, 8, 2)
        self.assertEqual(failures, [])
        self.assertIsNone(made[0].bits)

    def test_structure_released_on_failure(self):
        made = self._tracked()
        # a wrong header max rank makes the check FAIL after the bitset pass
        failures, _ = validate.check_identity(self.snapshots, 8, 99)
        self.assertTrue(failures)
        self.assertIsNone(made[0].bits)

    def test_structure_released_when_the_pass_raises(self):
        made = self._tracked()

        def explode(self, slots):
            raise RuntimeError("boom")

        with mock.patch.object(validate._IdentityBits, "claim", explode):
            with self.assertRaises(RuntimeError):
                validate.check_identity(self.snapshots, 8, 2)
        self.assertIsNone(made[0].bits)

    def test_aggregates_would_miss_what_the_bitset_catches(self):
        """An aggregate-proof corruption: two forests of three halos holding
        ranks [0,0,2] and [1,1,2] have the same total (6), the same maximum (2)
        and the same sum of squares (10) as the dense [0,1,2] and [0,1,2], and
        neither forest is dense. Sums, maxima and moments cannot separate them;
        the bitset does."""
        directory = Path(tempfile.mkdtemp(dir=self.tmp.name)) / "collide"
        a_list_path = write_synthetic_dataset(directory, 3, 2)
        with h5py.File(directory / snapshot_h5_name(0), "r+") as handle:
            handle["halos"]["ForestIndex"][...] = np.asarray([0, 0], dtype=np.int64)
            handle["halos"]["HaloRankInForest"][...] = np.asarray([0, 0], dtype=np.int64)
        with h5py.File(directory / snapshot_h5_name(1), "r+") as handle:
            handle["halos"]["ForestIndex"][...] = np.asarray([0, 1], dtype=np.int64)
            handle["halos"]["HaloRankInForest"][...] = np.asarray([2, 1], dtype=np.int64)
        with h5py.File(directory / snapshot_h5_name(2), "r+") as handle:
            handle["halos"]["ForestIndex"][...] = np.asarray([1, 1], dtype=np.int64)
            handle["halos"]["HaloRankInForest"][...] = np.asarray([1, 2], dtype=np.int64)
        outcomes = outcome_map(run_battery(directory, a_list_path))
        self.assertEqual(outcomes["identity"].status, "FAIL", outcomes["identity"].line())
        self.assertIn("density/uniqueness", outcomes["identity"].detail)

    def test_out_of_range_forest_index_is_grouped_not_double_reported(self):
        """A ForestIndex outside [0, n_forests_total) failed only the DENSITY
        condition in the whole-dataset battery, because that formulation sorted
        on ForestIndex and such halos formed their own dense group. The
        streaming formulation gives them their own bitset group for the same
        reason, so the uniqueness condition still passes on them."""
        directory = Path(tempfile.mkdtemp(dir=self.tmp.name)) / "stray"
        a_list_path = write_synthetic_dataset(directory, 2, 2)
        for snap, values in ((0, [0, -1]), (1, [0, -1])):
            with h5py.File(directory / snapshot_h5_name(snap), "r+") as handle:
                handle["halos"]["ForestIndex"][...] = np.asarray(values, dtype=np.int64)
        outcomes = outcome_map(run_battery(directory, a_list_path))
        detail = outcomes["identity"].detail
        self.assertEqual(outcomes["identity"].status, "FAIL", detail)
        self.assertIn("not dense over [0, 2)", detail)
        self.assertNotIn("density/uniqueness", detail)


if __name__ == "__main__":
    unittest.main()
