"""Unit tests for the producer validation battery: it must catch every
deliberately corrupted dataset — each format invariant and battery check is
violated once and the named check must FAIL — and, since the converter scale
pass (plan Slice 6), it must do so from a bounded window rather than from the
whole dataset, reporting exactly what the whole-dataset battery reported.

The format version 3 battery (converter generalisation Slice 8) is tested at
the end, against a real conversion of a hand-derived gapped graph and against
literal datasets written here with plain h5py rather than by the writer."""

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
import validate_v3  # noqa: E402
from conversion_manifest import ConversionManifest  # noqa: E402
from ctrees_parser import ConverterError  # noqa: E402
from hdf5_writer import (  # noqa: E402
    CHUNK_1D,
    FORMAT_VERSION,
    snapshot_h5_name,
    write_snapshot_file,
)
from scatter import load_a_list  # noqa: E402
from test_fixups import capture_stderr  # noqa: E402
from test_hdf5_writer import make_v3_conversion, make_written_workdir  # noqa: E402
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
        # any value other than the writer's own version; derived from
        # FORMAT_VERSION so the test survives the next ratchet bump
        outcomes = self._run(self._corrupt_attr("format_version", FORMAT_VERSION + 1))
        self.assert_fails(outcomes, "header-values", "format_version")

    def test_format_version_1_rejected(self):
        # Dedicated regression for the pre-flyby-removal v1 contract
        # (docs/dev/SHIN-UCHUU-FLYBY-DEFECT-ADDENDUM.md, D3): there is no
        # legacy-read path, so version 1 must be rejected outright, by name
        # rather than only as "any value other than the current one". A future
        # legacy-read branch that special-cased version 1 as acceptable would
        # pass test_wrong_format_version (which never tries 1) but must fail
        # this one.
        name = snapshot_h5_name(3)
        outcomes = self._run(self._corrupt_attr("format_version", 1))
        outcome = outcomes["header-values"]
        self.assertEqual(outcome.status, "FAIL", outcome.line())
        self.assertIn(name, outcome.detail)
        self.assertIn("format_version 1 != {}".format(FORMAT_VERSION), outcome.detail)

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
        # snap 5 slab: first_fof [0,1,2,2,4,4,6,6,6] (1010 and 1020 are both
        # self-central since fix_flybys was removed); halo 3 sits in halo 2's
        # chain, so claiming central 4 makes it a chain member whose
        # FirstHaloInFOFgroup is not its chain's central
        outcomes = self._run(self._corrupt_links(5, "FirstHaloInFOFgroup", 3, 4))
        self.assert_fails(outcomes, "fof-chains")

    def test_fof_cycle(self):
        outcomes = self._run(self._corrupt_links(5, "NextHaloInFOFgroup", 1, 0))
        self.assert_fails(outcomes, "fof-chains")

    def test_fof_orphaned_member(self):
        # halo 2 is the central of the chain {2, 3}; cutting its next-link
        # leaves member 3 unreachable from its own central
        outcomes = self._run(self._corrupt_links(5, "NextHaloInFOFgroup", 2, -1))
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
            "header-values": (
                "FAIL",
                "snapshot_003.h5: format_version {} != {}".format(
                    FORMAT_VERSION + 1, FORMAT_VERSION
                ),
            ),
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
                "not the chain's central; example rows: 3",
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
                "(orphaned or cyclic chain); example rows: 3",
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
            directory = self._attr("format_version", FORMAT_VERSION + 1)
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
            directory = self._field(5, "FirstHaloInFOFgroup", 3, 4)
        elif case == "fof_cycle":
            directory = self._field(5, "NextHaloInFOFgroup", 1, 0)
        elif case == "fof_orphaned_member":
            directory = self._field(5, "NextHaloInFOFgroup", 2, -1)
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

    # large enough that the per-halo identity-bitset signal stays well above
    # the per-file h5py bookkeeping 60 extra snapshot opens legitimately cost
    HALOS_PER_SNAPSHOT = 16384
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
        # the identity bitset grows by one bit per extra halo; everything else
        # is identical between runs except h5py's own per-file bookkeeping
        # from opening 60 more snapshots, charged as PER_SNAPSHOT_FILE_ALLOWANCE.
        extra_halos = (self.LARGE_SNAPSHOTS - self.SMALL_SNAPSHOTS) * self.HALOS_PER_SNAPSHOT
        extra_snapshots = self.LARGE_SNAPSHOTS - self.SMALL_SNAPSHOTS
        PER_SNAPSHOT_FILE_ALLOWANCE = 20 * 1024
        allowance = extra_halos // 8 + extra_snapshots * PER_SNAPSHOT_FILE_ALLOWANCE + 64 * 1024
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
        failures, peak_bytes, ordering_bytes = validate.check_identity(self.snapshots, 8, 2)
        self.assertEqual(failures, [])
        self.assertEqual(len(made), 1)
        self.assertEqual(peak_bytes, (3 * 8 + 7) // 8)
        self.assertEqual(peak_bytes, made[0].peak_bytes)
        # a conformant dataset spills nothing at all
        self.assertEqual(ordering_bytes, 0)

    def test_structure_released_on_success(self):
        made = self._tracked()
        failures, _, _ = validate.check_identity(self.snapshots, 8, 2)
        self.assertEqual(failures, [])
        self.assertIsNone(made[0].bits)

    def test_structure_released_on_failure(self):
        made = self._tracked()
        # a wrong header max rank makes the check FAIL after the bitset pass
        failures, _, _ = validate.check_identity(self.snapshots, 8, 99)
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


# ---------------------------------------------------------------------------
# Out-of-range ForestIndex: the external ordering (plan Slice 6)
# ---------------------------------------------------------------------------


def write_stray_forest_dataset(directory, n_snapshots: int, n_forests: int) -> Path:
    """A structurally conformant dataset in which EVERY halo carries a
    DISTINCT out-of-range ForestIndex.

    The structural checks validate dtype, shape and chunks, not range, so this
    reaches the semantic checks on the ordinary CLI path. It is the input shape
    that makes any per-value table grow with the dataset: the number of
    distinct out-of-range values here is the halo count.
    """
    a_list_path = write_synthetic_dataset(directory, n_snapshots, n_forests)
    for snap in range(n_snapshots):
        with h5py.File(Path(directory) / snapshot_h5_name(snap), "r+") as handle:
            first = -1 - snap * n_forests
            handle["halos"]["ForestIndex"][...] = first - np.arange(n_forests, dtype=np.int64)
    return a_list_path


def tiny_ordering_limits(case):
    """Shrink the ordering's buffers so a small dataset still spills many runs
    and merges them over several bounded rounds — the fan-in, and with it the
    number of concurrent read buffers, is what must stay constant."""
    for name, value in (
        ("_ORDERING_RUN_ROWS", 512),
        ("_ORDERING_READ_ROWS", 512),
        ("_ORDERING_MERGE_ROWS", 256),
        ("_ORDERING_MERGE_READ_ROWS", 128),
    ):
        patcher = mock.patch.object(validate, name, value)
        patcher.start()
        case.addCleanup(patcher.stop)


def ordering_spill_directories():
    return sorted(
        path
        for path in Path(tempfile.gettempdir()).glob(validate.ORDERING_DIR_PREFIX + "*")
        if path.is_dir()
    )


class TestPairOrdering(unittest.TestCase):
    """The external ordering must be an exact sort, not an approximation, and
    must clean up after itself."""

    def setUp(self):
        tiny_ordering_limits(self)

    def test_merged_output_equals_an_in_memory_sort(self):
        rng = np.random.default_rng(20260827)
        forest = rng.integers(-500, 500, size=7777, dtype=np.int64)
        rank = rng.integers(-3, 40, size=7777, dtype=np.int64)
        ordering = validate._PairOrdering()
        try:
            for start in range(0, forest.size, 333):
                ordering.add(forest[start : start + 333], rank[start : start + 333])
            path = ordering.finish()
            merged = np.concatenate(list(validate._PairOrdering.blocks(path)))
            expected = np.empty(forest.size, dtype=validate._PAIR_DTYPE)
            expected["forest"] = forest
            expected["rank"] = rank
            expected.sort(order=("forest", "rank"))
            self.assertEqual(merged.size, expected.size)
            self.assertTrue(np.array_equal(merged["forest"], expected["forest"]))
            self.assertTrue(np.array_equal(merged["rank"], expected["rank"]))
            # the run size above forces several bounded merge rounds
            self.assertGreater(ordering.peak_bytes, 0)
        finally:
            ordering.close()

    def test_nothing_is_spilled_when_no_pair_is_added(self):
        before = ordering_spill_directories()
        ordering = validate._PairOrdering()
        try:
            self.assertIsNone(ordering.finish())
            self.assertEqual(ordering.peak_bytes, 0)
            self.assertEqual(ordering_spill_directories(), before)
        finally:
            ordering.close()

    def test_close_removes_the_spill_directory(self):
        before = ordering_spill_directories()
        ordering = validate._PairOrdering()
        ordering.add(np.arange(-2000, 0, dtype=np.int64), np.zeros(2000, dtype=np.int64))
        ordering.finish()
        self.assertNotEqual(ordering_spill_directories(), before)
        ordering.close()
        self.assertEqual(ordering_spill_directories(), before)
        # and calling it twice is safe
        ordering.close()
        self.assertEqual(ordering_spill_directories(), before)


class TestStrayForestIndex(unittest.TestCase):
    """ForestIndex values outside [0, n_forests_total) keep the whole-dataset
    battery's semantics — their own group per distinct value, judged dense on
    its own — without any per-value state in memory."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)

    def _dataset(self, name, n_snapshots, n_forests, forest_values):
        directory = self.root / name
        a_list_path = write_synthetic_dataset(directory, n_snapshots, n_forests)
        for snap, values in enumerate(forest_values):
            with h5py.File(directory / snapshot_h5_name(snap), "r+") as handle:
                handle["halos"]["ForestIndex"][...] = np.asarray(values, dtype=np.int64)
        return directory, a_list_path

    def test_a_dense_stray_group_fails_density_only(self):
        # two halos share ForestIndex -1 and hold ranks 0 and 1: the
        # whole-dataset battery sorted on ForestIndex alone, so that group was
        # dense and only the density condition failed
        directory, a_list_path = self._dataset("dense-stray", 2, 2, ([0, -1], [0, -1]))
        detail = outcome_map(run_battery(directory, a_list_path))["identity"].detail
        self.assertIn("not dense over [0, 2)", detail)
        self.assertNotIn("density/uniqueness", detail)

    def test_a_non_dense_stray_group_fails_both_conditions(self):
        # ForestIndex -1 now holds ranks 0 and 0: still its own group, and now
        # that group is NOT dense
        directory, a_list_path = self._dataset("broken-stray", 2, 2, ([0, -1], [0, -1]))
        with h5py.File(directory / snapshot_h5_name(1), "r+") as handle:
            handle["halos"]["HaloRankInForest"][...] = np.asarray([1, 0], dtype=np.int64)
        detail = outcome_map(run_battery(directory, a_list_path))["identity"].detail
        self.assertIn("not dense over [0, 2)", detail)
        self.assertIn(
            "1 (ForestIndex, HaloRankInForest) pair(s) violate per-forest density/uniqueness; "
            "examples: (ForestIndex=-1, rank=0, expected 1)",
            detail,
        )

    def test_stray_values_sort_below_in_range_ones_in_the_examples(self):
        # condition (a) reports the five LOWEST distinct values observed, and a
        # negative stray sorts below every in-range forest
        directory, a_list_path = self._dataset("lowest", 2, 3, ([0, 1, -7], [0, 1, -7]))
        detail = outcome_map(run_battery(directory, a_list_path))["identity"].detail
        self.assertIn("3 distinct value(s) observed, examples: -7, 0, 1", detail)

    def test_spill_directory_is_removed_on_success_failure_and_exception(self):
        before = ordering_spill_directories()
        clean, clean_a_list = self._dataset("clean", 2, 2, ([0, 1], [0, 1]))
        stray, stray_a_list = self._dataset("stray", 2, 2, ([0, -1], [0, -1]))
        self.assertEqual(outcome_map(run_battery(clean, clean_a_list))["identity"].status, "PASS")
        self.assertEqual(ordering_spill_directories(), before)
        self.assertEqual(outcome_map(run_battery(stray, stray_a_list))["identity"].status, "FAIL")
        self.assertEqual(ordering_spill_directories(), before)

        def explode(path):
            raise RuntimeError("boom")

        with mock.patch.object(validate, "_scan_ordering", explode):
            with self.assertRaises(RuntimeError):
                run_battery(stray, stray_a_list)
        self.assertEqual(ordering_spill_directories(), before)

    def test_ordering_peak_bytes_are_reported(self):
        stray, stray_a_list = self._dataset("reported", 2, 2, ([0, -1], [0, -1]))
        snapshots = validate._Snapshots(stray, 2)
        failures, bitset_bytes, ordering_bytes = validate.check_identity(snapshots, 2, 1)
        self.assertTrue(failures)
        # two out-of-range halos, 16 bytes per ordered pair
        self.assertEqual(ordering_bytes, 2 * validate._PAIR_DTYPE.itemsize)
        self.assertEqual(bitset_bytes, (2 + 7) // 8)


class TestBoundedMemoryOnMalformedInput(unittest.TestCase):
    """The bounded-memory claim has to hold on the corrupt datasets the battery
    exists to diagnose, not only on conformant ones.

    Every halo here carries a DISTINCT out-of-range ForestIndex, so the number
    of distinct out-of-range values scales with the dataset. Any per-value
    table in memory grows with it; the external ordering does not.
    """

    # large enough that the per-halo regression this test exists to catch
    # stays well above the per-file h5py bookkeeping 48 extra opens cost
    HALOS_PER_SNAPSHOT = 8192
    # both sizes must spill more runs than the merge fan-in, so that BOTH runs
    # pay the same maximum number of concurrent merge read buffers and the only
    # quantity left differing is retained per-value state
    SMALL_SNAPSHOTS = 16
    LARGE_SNAPSHOTS = 64

    def setUp(self):
        tiny_ordering_limits(self)
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        root = Path(self.tmp.name)
        self.small = root / "small"
        self.large = root / "large"
        self.small_a_list = write_stray_forest_dataset(
            self.small, self.SMALL_SNAPSHOTS, self.HALOS_PER_SNAPSHOT
        )
        self.large_a_list = write_stray_forest_dataset(
            self.large, self.LARGE_SNAPSHOTS, self.HALOS_PER_SNAPSHOT
        )

    def test_the_defect_is_actually_reached(self):
        # if these datasets ever stopped being structurally conformant, or
        # stopped failing on density alone, the memory test below would be
        # measuring the wrong path
        outcomes = outcome_map(run_battery(self.small, self.small_a_list))
        self.assertEqual(outcomes["object-set"].status, "PASS")
        self.assertEqual(outcomes["identity"].status, "FAIL")
        self.assertIn("not dense over [0,", outcomes["identity"].detail)

    def test_peak_allocation_does_not_scale_with_distinct_stray_forests(self):
        run_battery(self.small, self.small_a_list)  # warm up
        small_peak = self._peak(self.small, self.small_a_list)
        large_peak = self._peak(self.large, self.large_a_list)
        extra_snapshots = self.LARGE_SNAPSHOTS - self.SMALL_SNAPSHOTS
        extra_halos = extra_snapshots * self.HALOS_PER_SNAPSHOT
        # retained per-value state must be nothing; the only real cost is
        # h5py's own per-file bookkeeping from 48 more snapshot opens
        PER_SNAPSHOT_FILE_ALLOWANCE = 16 * 1024
        allowance = extra_snapshots * PER_SNAPSHOT_FILE_ALLOWANCE + 64 * 1024
        self.assertLess(large_peak - small_peak, allowance)
        # and the allowance is far below what even 8 bytes per distinct value
        # would cost, which is what an in-memory table would have charged
        self.assertLess(allowance, extra_halos * 8)

    @staticmethod
    def _peak(directory, a_list_path):
        tracemalloc.start()
        try:
            tracemalloc.reset_peak()
            run_battery(directory, a_list_path)
            return tracemalloc.get_traced_memory()[1]
        finally:
            tracemalloc.stop()


# ---------------------------------------------------------------------------
# The ordering's accounting, lifetime and per-chunk cost (plan Slice 6)
# ---------------------------------------------------------------------------


def failing_rmtree(*_args, **_kwargs):
    raise OSError(13, "Permission denied")


class TestOrderingAccounting(unittest.TestCase):
    """Nothing in the ordering may scale with the NUMBER OF RUNS — neither the
    list of live runs nor the work done to account for their bytes."""

    def setUp(self):
        tiny_ordering_limits(self)

    def _ordering_for(self, runs):
        rows = runs * validate._ORDERING_RUN_ROWS
        ordering = validate._PairOrdering()
        ordering.add(-1 - np.arange(rows, dtype=np.int64), np.zeros(rows, dtype=np.int64))
        return ordering

    def test_live_runs_are_bounded_by_the_fan_in_not_the_run_count(self):
        ordering = self._ordering_for(128)
        try:
            live = sum(len(level) for level in ordering._levels)
            # 128 runs were written...
            self.assertGreaterEqual(ordering._serial, 128)
            # ...but a full level is merged away as it fills, so what is held
            # is the fan-in times the level count, not the run count
            self.assertLess(live, validate._ORDERING_FANIN * 4)
        finally:
            ordering.close()

    def _stat_calls(self, runs):
        calls = []
        original = Path.stat

        def counting(path, *args, **kwargs):
            calls.append(path)
            return original(path, *args, **kwargs)

        with mock.patch.object(Path, "stat", counting):
            ordering = self._ordering_for(runs)
            try:
                ordering.finish()
            finally:
                ordering.close()
        return len(calls)

    def test_disk_accounting_is_linear_in_the_number_of_runs(self):
        small = self._stat_calls(64)
        large = self._stat_calls(256)
        # each run's bytes are charged when it is written and discharged when
        # it is unlinked, so the accounting is O(1) per run
        self.assertLess(small, 8 * 64 + 64)
        self.assertLess(large, 8 * 256 + 64)
        # a directory rescan after every spill would be R(R+1)/2 stats — 2,080
        # for the small case and 32,896 for the large one — so this bound is
        # what separates linear accounting from quadratic
        self.assertLess(large, 4 * small + 512)

    def test_reported_peak_matches_the_bytes_actually_written(self):
        ordering = self._ordering_for(4)
        try:
            path = ordering.finish()
            merged = sum(int(block.size) for block in validate._PairOrdering.blocks(path))
            self.assertEqual(merged, ordering.rows)
            # the merge holds its inputs and its output at once, so the peak is
            # above the ordered file alone and below twice it
            ordered_bytes = merged * validate._PAIR_DTYPE.itemsize
            self.assertGreater(ordering.peak_bytes, ordered_bytes)
            self.assertLessEqual(ordering.peak_bytes, 2 * ordered_bytes)
        finally:
            ordering.close()


class TestOrderingCleanupFailure(unittest.TestCase):
    """A removal that cannot be confirmed is reported, never swallowed — and
    never at the cost of an exception already in flight."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)

    def _stray_dataset(self, name="stray"):
        directory = self.root / name
        a_list_path = write_synthetic_dataset(directory, 2, 2)
        for snap in range(2):
            with h5py.File(directory / snapshot_h5_name(snap), "r+") as handle:
                handle["halos"]["ForestIndex"][...] = np.asarray([0, -1], dtype=np.int64)
        return directory, a_list_path

    def test_close_reports_a_failed_removal_instead_of_swallowing_it(self):
        ordering = validate._PairOrdering()
        ordering.add(np.asarray([-1], dtype=np.int64), np.asarray([0], dtype=np.int64))
        ordering.finish()
        directory = ordering._directory
        self.addCleanup(shutil.rmtree, str(directory), True)
        with mock.patch.object(validate.shutil, "rmtree", failing_rmtree):
            ordering.close()
        # ownership is NOT released: the directory is still named, and the
        # failure is available to the caller rather than lost
        self.assertEqual(ordering.unremoved, directory)
        self.assertTrue(directory.exists())
        with self.assertRaises(ConverterError) as ctx:
            ordering.raise_if_unremoved()
        self.assertIn(str(directory), str(ctx.exception))
        # a later close that succeeds clears it, on confirmed absence
        ordering.close()
        self.assertIsNone(ordering.unremoved)
        self.assertFalse(directory.exists())
        ordering.raise_if_unremoved()

    def test_the_battery_surfaces_a_cleanup_failure(self):
        directory, a_list_path = self._stray_dataset()
        before = ordering_spill_directories()
        with mock.patch.object(validate.shutil, "rmtree", failing_rmtree):
            with self.assertRaises(ConverterError) as ctx:
                run_battery(directory, a_list_path)
        self.assertIn("could not be removed", str(ctx.exception))
        for leftover in set(ordering_spill_directories()) - set(before):
            shutil.rmtree(str(leftover), ignore_errors=True)

    def test_a_cleanup_failure_does_not_replace_an_exception_in_flight(self):
        directory, a_list_path = self._stray_dataset("in-flight")
        before = ordering_spill_directories()

        def explode(path):
            raise RuntimeError("boom")

        with mock.patch.object(validate, "_scan_ordering", explode):
            with mock.patch.object(validate.shutil, "rmtree", failing_rmtree):
                # the RuntimeError must survive: close() runs in a finally and
                # must not raise a ConverterError over the top of it
                with self.assertRaises(RuntimeError):
                    run_battery(directory, a_list_path)
        for leftover in set(ordering_spill_directories()) - set(before):
            shutil.rmtree(str(leftover), ignore_errors=True)


class TestIdentityChunkCost(unittest.TestCase):
    """IDENTITY_CHUNK_BYTES_PER_ROW is a ceiling the identity passes claim to
    stay within, so it is measured rather than asserted in a comment."""

    class OneChunk:
        """The smallest thing check_identity consumes: one full-size chunk of
        conformant in-range identity data, regenerated per pass as a real
        reader would."""

        n_snapshots = 1

        def __init__(self, rows, per_forest):
            self.rows = int(rows)
            self.per_forest = int(per_forest)

        def chunks(self, snap, fields, chunk_rows):
            index = np.arange(self.rows, dtype=np.int64)
            yield index // self.per_forest, index % self.per_forest

    def test_identity_chunk_cost_is_within_the_declared_bound(self):
        rows = validate.IDENTITY_CHUNK_ROWS
        per_forest = 1024
        n_forests = rows // per_forest
        source = self.OneChunk(rows, per_forest)
        tracemalloc.start()
        try:
            tracemalloc.reset_peak()
            base = tracemalloc.get_traced_memory()[0]
            failures, bitset_bytes, ordering_bytes = validate.check_identity(
                source, n_forests, per_forest - 1
            )
            peak = tracemalloc.get_traced_memory()[1]
        finally:
            tracemalloc.stop()
        self.assertEqual(failures, [])
        self.assertEqual(ordering_bytes, 0)
        # everything the passes hold that is NOT per-chunk: the bitset and the
        # two forest-count-sized tables
        overhead = bitset_bytes + 2 * 8 * n_forests
        self.assertLessEqual(peak - base - overhead, rows * validate.IDENTITY_CHUNK_BYTES_PER_ROW)


# ==========================================================================
# Format version 3 (converter generalisation Slice 8)
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
_LITERAL_STORAGE = {"int": "<i4", "long long": "<i8", "float": "<f4", "vec3_float": "<f4"}
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
    link columns are ``(row, snapshot)`` pairs or ``None``, the rest plain
    values, and unspecified payload is a literal default. ``sidecar`` maps the
    three sidecar names to lists.
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

    def test_cli_dispatches_version_3_and_exits_zero(self):
        code = validate.main(
            [
                str(self.conv.dataset),
                "--a-list",
                str(self.conv.a_list),
                "--manifest",
                str(self.conv.manifest.path),
                "--memory-budget-mb",
                "1",
            ]
        )
        self.assertEqual(code, 0)


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
        with self.assertRaisesRegex(ConverterError, "mixes format versions"):
            validate.detect_dataset_version(directory, 4)
        with capture_stderr() as err:
            code = validate.main(
                [
                    str(directory),
                    "--a-list",
                    str(self.conv.a_list),
                    "--manifest",
                    str(self.conv.manifest.path),
                ]
            )
        self.assertEqual(code, 1)
        self.assertIn("mixes format versions", err.text)

    def test_version_1_and_unknown_versions_fail(self):
        for version in (1, 4):
            directory = self.copy()
            self.edit_header(directory, None, "format_version", version)
            self.assertEqual(validate.detect_dataset_version(directory, 4), FORMAT_VERSION)
            outcomes = outcome_map(
                validate.run_producer_battery(directory, self.conv.a_list, manifest_path=None)
            )
            self.assertEqual(outcomes["object-set"].status, "FAIL", version)
            self.assert_fails(directory, "header-values", "format_version {}".format(version))

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


class TestV3BoundedBattery(unittest.TestCase):
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


class TestProducerDispatch(unittest.TestCase):
    def test_a_version_2_dataset_runs_the_unchanged_v2_battery(self):
        with tempfile.TemporaryDirectory() as tmp:
            workdir, a_list, _sim_info, hdf5_dir = make_written_workdir(Path(tmp))
            manifest = workdir / "manifest.json"
            n_snapshots = len(load_a_list(a_list)[0])
            self.assertEqual(validate.detect_dataset_version(hdf5_dir, n_snapshots), FORMAT_VERSION)
            dispatched = [
                o.as_dict()
                for o in validate.run_producer_battery(hdf5_dir, a_list, manifest_path=manifest)
            ]
            direct = [o.as_dict() for o in run_battery(hdf5_dir, a_list, manifest_path=manifest)]
            self.assertEqual(dispatched, direct)
            self.assertTrue(all(o["status"] == "PASS" for o in direct))


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
        self.assertEqual(validate.V3_FORMAT_VERSION, V3_FORMAT_VERSION)

    def test_the_declarable_types_match_the_schema_module(self):
        from column_schema import EXTRA_TYPES

        self.assertEqual(
            validate_v3._V3_TYPES,
            {
                name: (np.dtype(spec.numpy_dtype).newbyteorder("<").str, spec.n_components == 3)
                for name, spec in EXTRA_TYPES.items()
            },
        )


if __name__ == "__main__":
    unittest.main()
