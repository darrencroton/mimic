"""CLI tests: the generic ``convert_trees.py`` driven as a subprocess.

Every conversion here runs from a fresh temporary working directory, never
the repository root, with every path passed absolute, so nothing depends on
the repository being the current directory. Sources are the committed package
fixtures (read-only; their bytes are compared before and after) or literal
L-Halo trees written into the test's own temporary directory.

Coverage:

- every generic stage (inspect, ingest, transpose, write, validate, report) on
  each adapter -- L-Halo binary (micro-Uchuu fixture, and a literal gapped
  source under the mini-Millennium package), forests-HDF5 (full-Uchuu
  external-link fixture) and ASCII (micro-Uchuu fixture, also under a cut
  table);
- the output contract: version 3 by default, the runtime-support notice on
  every stage, and the nonadjacent/wide identification;
- early failures: mismatched or broken profiles, foreign or missing inventory
  options, out-of-range and missing files, and conflicting resume inputs --
  each before the workdir is mutated;
- resume from the embedded configuration after the profile file is gone;
- the battery's bounded mode (``validate``/``report --bounded``): its verdict
  and printed ceiling on an adjacent conversion, and its refusal of a gapped
  dataset.
"""

import contextlib
import hashlib
import io
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from typing import NamedTuple

import h5py

HERE = Path(__file__).resolve().parent
CONVERT_DIR = HERE.parent
REPO_ROOT = HERE.parents[2]
SIMULATIONS = REPO_ROOT / "simulations"
CONVERT_TREES = CONVERT_DIR / "convert_trees.py"

sys.path.insert(0, str(CONVERT_DIR))
sys.path.insert(0, str(HERE))

import convert_trees  # noqa: E402
import fixtures  # noqa: E402
from fixtures import (  # noqa: E402
    make_scatter_workdir,
)

RUNTIME_MARK = "runtime support: format version 3 is consumed"
BUDGET_MB = "64"


def run(script, args, cwd):
    """Run one CLI invocation; returns the CompletedProcess."""
    return subprocess.run(
        [sys.executable, str(script)] + [str(arg) for arg in args],
        cwd=str(cwd),
        capture_output=True,
        text=True,
    )


def tree_state(root: Path):
    """(relative path, sha256) for every regular file, plus symlink targets."""
    state = []
    for base, dirs, files in os.walk(root, followlinks=False):
        for name in sorted(dirs + files):
            path = Path(base) / name
            rel = str(path.relative_to(root))
            if path.is_symlink():
                state.append((rel, "->" + os.readlink(path)))
            elif path.is_file():
                state.append((rel, hashlib.sha256(path.read_bytes()).hexdigest()))
    return sorted(state)


def package_files(package: str):
    """Every package file a conversion might be tempted to touch, including
    the snapshots symlink itself (not what it points at)."""
    root = SIMULATIONS / package
    state = []
    for path in sorted(root.iterdir()):
        if path.is_symlink():
            state.append((path.name, "->" + os.readlink(path)))
        elif path.is_file():
            state.append((path.name, hashlib.sha256(path.read_bytes()).hexdigest()))
    return state + [("_tests/data/" + name, digest) for name, digest in tree_state(root / "_tests")]


# ==========================================================================
# Routes: one explicit configuration per package fixture
# ==========================================================================


def lhalo_route(package, source_dir, tree_name, last_file=0):
    root = SIMULATIONS / package
    a_list = next(root.glob("*.a_list"))
    return [
        "--source-format",
        "lhalo_binary",
        "--simulation-info",
        root / "simulation_info.yaml",
        "--a-list",
        a_list,
        "--column-map",
        root / "converter_columns.yaml",
        "--halo-properties",
        root / "halo_properties.yaml",
        "--source-dir",
        source_dir,
        "--tree-name",
        tree_name,
        "--first-file",
        "0",
        "--last-file",
        str(last_file),
    ]


def hdf5_route(package, info_name):
    root = SIMULATIONS / package
    return [
        "--source-format",
        "consistent_trees_hdf5",
        "--simulation-info",
        root / "simulation_info.yaml",
        "--a-list",
        next(root.glob("*.a_list")),
        "--column-map",
        root / "converter_columns.yaml",
        "--info-file",
        root / "_tests" / "data" / info_name,
        "--first-file",
        "0",
        "--last-file",
        "0",
    ]


def ascii_route():
    root = SIMULATIONS / "micro-uchuu-ascii"
    data = root / "_tests" / "data"
    return [
        "--source-format",
        "consistent_trees_ascii",
        "--simulation-info",
        root / "simulation_info.yaml",
        "--a-list",
        root / "micro-uchuu.a_list",
        "--column-map",
        root / "converter_columns.yaml",
        "--forests-list",
        data / "forests.list",
        "--tree-file",
        data / "tree_0_0_0.dat",
    ]


def route_value(route, flag):
    return route[route.index(flag) + 1]


class CliCase(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="cli_"))
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)
        self.cwd = self.tmp / "cwd"
        self.cwd.mkdir()
        # Never the repository root or a converter directory, so no relative
        # path can resolve against them. (A harness may place TMPDIR inside
        # the checkout; the directory is still a fresh, empty one.)
        self.assertNotIn(self.cwd.resolve(), (REPO_ROOT, CONVERT_DIR, HERE))

    def cli(self, *args, expect=0):
        result = run(CONVERT_TREES, args, self.cwd)
        self.assertEqual(
            result.returncode,
            expect,
            "convert_trees {} exited {}\nstdout:\n{}\nstderr:\n{}".format(
                args[0], result.returncode, result.stdout, result.stderr
            ),
        )
        return result

    def literal_gapped_source(self):
        """The literal trees of ``fixtures``: a three- and a two-snapshot
        descendant gap, in two files named like the Millennium packages'."""
        source = self.tmp / "literal"
        source.mkdir()
        fixtures.write_lhalo_file(
            str(source / "trees_063.0"), [fixtures.LITERAL_TREE_A, fixtures.LITERAL_TREE_B]
        )
        fixtures.write_lhalo_file(str(source / "trees_063.1"), [fixtures.LITERAL_TREE_C])
        return source


# ==========================================================================
# Every stage, every adapter
# ==========================================================================


class StageChainTests(CliCase):
    def run_chain(
        self, route, package, sim_info=None, expect_adjacent=True, n_halos=None, ingest=()
    ):
        """inspect -> ingest -> transpose -> write -> validate -> report, from
        a directory outside the repository. Returns the workdir."""
        sim_info = sim_info or route_value(route, "--simulation-info")
        before = package_files(package)
        work = self.tmp / "work"

        inspected = self.cli("inspect", *route, "--memory-budget-mb", BUDGET_MB)
        self.assertFalse(work.exists())
        self.assertEqual(os.listdir(self.cwd), [], "inspect wrote into its working directory")
        document = json.loads(
            inspected.stdout[inspected.stdout.index("{") : inspected.stdout.rindex("}") + 1]
        )
        self.assertEqual(document["output"]["format_version"], 3)
        self.assertIn(RUNTIME_MARK, inspected.stdout)

        outputs = {
            "ingest": self.cli(
                "ingest", "--workdir", work, *route, "--memory-budget-mb", BUDGET_MB, *ingest
            ),
            "transpose": self.cli("transpose", "--workdir", work),
            "write": self.cli("write", "--workdir", work, "--simulation-info", sim_info),
            "validate": self.cli("validate", "--workdir", work),
            "report": self.cli("report", "--workdir", work),
        }
        for stage, result in outputs.items():
            self.assertIn(RUNTIME_MARK, result.stdout, stage)
        self.assertIn("ingest: COMPLETE", outputs["ingest"].stdout)
        self.assertIn("validation: PASS", outputs["validate"].stdout)
        for stage in ("transpose", "write", "validate", "report"):
            marker = "links_adjacent=1" if expect_adjacent else "NONADJACENT output"
            self.assertIn(marker, outputs[stage].stdout, stage)
            self.assertIn("within INT32_MAX", outputs[stage].stdout, stage)

        manifest = json.loads((work / "manifest.json").read_text())
        dataset = work / manifest["stages"]["write"]["directory"]
        n_snapshots = len(manifest["configuration"]["snapshots"]["numbers"])
        with h5py.File(dataset / "snapshot_000.h5", "r") as handle:
            self.assertEqual(int(handle["header"].attrs["format_version"]), 3)
        self.assertEqual(len(list(dataset.glob("snapshot_*.h5"))), n_snapshots)
        report = json.loads((work / "conversion_report.json").read_text())
        self.assertTrue(report["validation_passed"])
        # the text report names the battery that settled the verdicts
        self.assertIn(", external-sort battery\n", (work / "conversion_report.txt").read_text())
        # Format capability ("format consumed"), not route validation.
        self.assertIs(report["runtime_compatibility"]["runnable_by_current_mimic"], True)
        if n_halos is not None:
            self.assertEqual(report["totals"]["halos"], n_halos)
        self.assertEqual(os.listdir(self.cwd), [], "a stage wrote into its working directory")
        self.assertEqual(package_files(package), before, "the conversion touched the package")
        if expect_adjacent:
            self.assert_bounded_mode_agrees(work)
        return work

    def assert_bounded_mode_agrees(self, work):
        """The bounded mode gives the default battery's 20 check statuses on
        an adjacent conversion, at its default block and at blocks of one
        and two rows, so every chain and run crosses block boundaries."""
        from conversion_manifest import ConversionManifest
        from validate_bounded import DEFAULT_BLOCK_ROWS, run_battery_bounded
        from validate_v3 import V3_CHECKS, run_battery_v3

        manifest = ConversionManifest.load(work)
        dataset = manifest.artifact_path(manifest.stage("write")["directory"])
        a_list = manifest.configuration["snapshots"]["a_list_path"]
        budget = int(BUDGET_MB) << 20
        default = run_battery_v3(dataset, a_list, manifest_path=manifest.path, budget_bytes=budget)
        expected = {outcome.name: outcome.status for outcome in default.outcomes}
        self.assertEqual(list(expected), list(V3_CHECKS))
        for block_rows in (DEFAULT_BLOCK_ROWS, 1, 2):
            with contextlib.redirect_stderr(io.StringIO()):
                bounded = run_battery_bounded(
                    dataset, a_list, manifest.path, budget_bytes=budget, block_rows=block_rows
                )
            self.assertEqual(
                {outcome.name: outcome.status for outcome in bounded.outcomes},
                expected,
                "block_rows {}".format(block_rows),
            )

    def test_micro_uchuu_lhalo_binary(self):
        data = SIMULATIONS / "micro-uchuu" / "_tests" / "data"
        self.run_chain(
            lhalo_route("micro-uchuu", data, "Uchuu100_test_lhalo_binary"), "micro-uchuu", n_halos=6
        )

    def test_mini_millennium_package_on_a_gapped_literal_source(self):
        work = self.run_chain(
            lhalo_route("mini-millennium", self.literal_gapped_source(), "trees_063", last_file=1),
            "mini-millennium",
            expect_adjacent=False,
            n_halos=fixtures.LITERAL_TOTAL_HALOS,
        )
        stdout = self.cli("transpose", "--workdir", work).stdout
        # Tree A's snap-0 halo descends to snap 3; tree C's to snap 2.
        self.assertIn("2 Descendant link(s) skip snapshots (longest span 3)", stdout)

    def test_full_uchuu_forests_hdf5_external_link_fixture(self):
        self.run_chain(hdf5_route("uchuu", "mergertree_info.h5"), "uchuu", n_halos=6)

    def test_micro_uchuu_ascii_fixture(self):
        # Runs at the default batch size: the ASCII adapter charges its batch
        # buffer per snapshot at min(max_rows, n_rows), so a small budget no
        # longer needs a matching --ingest-max-rows.
        self.run_chain(ascii_route(), "micro-uchuu-ascii", n_halos=4)

    def test_micro_uchuu_ascii_fixture_under_a_cut_table(self):
        # the catalogue's own forests.list is the identity cut table
        table = route_value(ascii_route(), "--forests-list")
        work = self.run_chain(
            ascii_route(), "micro-uchuu-ascii", n_halos=4, ingest=("--cut-table", table)
        )
        manifest = json.loads((work / "manifest.json").read_text())
        parameters = manifest["configuration"]["adapter"]["parameters"]
        self.assertEqual(parameters["cut_table"], os.path.realpath(table))
        self.assertEqual(
            parameters["cut_table_md5"], hashlib.md5(Path(table).read_bytes()).hexdigest()
        )

    def test_the_shipped_default_profile_is_announced_when_no_profile_is_named(self):
        route = ascii_route()
        index = route.index("--column-map")
        del route[index : index + 2]
        result = self.cli("inspect", *route)
        self.assertIn("profile: shipped default", result.stderr)
        self.assertIn("consistent_trees_ascii.yaml", result.stderr)

    def test_inspect_stdout_is_pure_json_with_the_runtime_notice_on_stderr(self):
        data = SIMULATIONS / "micro-uchuu" / "_tests" / "data"
        route = lhalo_route("micro-uchuu", data, "Uchuu100_test_lhalo_binary")
        result = self.cli("inspect", *route, "--memory-budget-mb", BUDGET_MB)
        document = json.loads(result.stdout)
        self.assertEqual(document["output"]["format_version"], 3)
        self.assertIn(RUNTIME_MARK, result.stderr)
        self.assertIn("profile: named", result.stderr)


# ==========================================================================
# Early failures and resume
# ==========================================================================


class BoundedBatteryTests(CliCase):
    """``validate`` and ``report`` with ``--bounded``."""

    def converted(self, route, sim_info=None):
        work = self.tmp / "work"
        sim_info = sim_info or route_value(route, "--simulation-info")
        self.cli("ingest", "--workdir", work, *route, "--memory-budget-mb", BUDGET_MB)
        self.cli("transpose", "--workdir", work)
        self.cli("write", "--workdir", work, "--simulation-info", sim_info)
        return work

    def test_an_adjacent_conversion_passes_and_prints_its_ceiling(self):
        work = self.converted(ascii_route())
        spill = self.tmp / "spill"
        spill.mkdir()
        options = ("--bounded", "--memory-budget-mb", BUDGET_MB, "--spill-dir", spill)
        validated = self.cli("validate", "--workdir", work, *options)
        self.assertIn("validation: PASS", validated.stdout)
        self.assertIn("links_adjacent=1", validated.stdout)
        self.assertIn("bounded battery: ceiling", validated.stderr)
        self.assertIn("budget {} B".format(int(BUDGET_MB) << 20), validated.stderr)
        reported = self.cli("report", "--workdir", work, *options)
        self.assertIn("bounded battery: ceiling", reported.stderr)
        report = json.loads((work / "conversion_report.json").read_text())
        self.assertTrue(report["validation_passed"])
        validation = report["resources"]["validation"]
        self.assertEqual(validation["peak_spill_bytes"], 0)
        self.assertEqual(validation["battery_mode"], "bounded")
        self.assertIn(
            "bounded battery: ceiling {} B".format(validation["ceiling_bytes"]), reported.stderr
        )
        self.assertIn(
            "bounded battery (ceiling {} byte(s))".format(validation["ceiling_bytes"]),
            (work / "conversion_report.txt").read_text(),
        )
        self.assertEqual(list(spill.iterdir()), [], "the bounded mode wrote to --spill-dir")
        self.assertEqual(os.listdir(self.cwd), [])

    def test_a_gapped_conversion_is_refused_by_name_and_passes_the_default_battery(self):
        work = self.converted(
            lhalo_route("mini-millennium", self.literal_gapped_source(), "trees_063", last_file=1)
        )
        for command in ("validate", "report"):
            refused = self.cli(
                command, "--workdir", work, "--bounded", "--memory-budget-mb", BUDGET_MB, expect=2
            )
            self.assertIn("ERROR: the bounded battery validates adjacent datasets", refused.stderr)
            self.assertIn("snapshot_000.h5 declares links_adjacent 0", refused.stderr)
            self.assertNotIn("validation:", refused.stdout)
        self.assertFalse((work / "conversion_report.json").exists())
        self.assertIn("validation: PASS", self.cli("validate", "--workdir", work).stdout)


class FailureTests(CliCase):
    def setUp(self):
        super().setUp()
        self.data = SIMULATIONS / "micro-uchuu" / "_tests" / "data"
        self.route = lhalo_route("micro-uchuu", self.data, "Uchuu100_test_lhalo_binary")
        self.work = self.tmp / "work"

    def with_option(self, flag, value, route=None):
        route = list(route or self.route)
        route[route.index(flag) + 1] = value
        return route

    def ingest_fails(self, route, message):
        result = self.cli("ingest", "--workdir", self.work, *route, expect=2)
        self.assertIn(message, result.stderr)
        self.assertFalse(self.work.exists(), "a refused ingest created its workdir")
        return result

    def test_a_profile_of_another_format_is_refused_not_reinterpreted(self):
        profile = CONVERT_DIR / "profiles" / "consistent_trees_ascii.yaml"
        self.ingest_fails(self.with_option("--column-map", profile), "not --source-format")

    def test_a_broken_named_profile_never_falls_back_to_the_default(self):
        broken = self.tmp / "broken.yaml"
        text = (SIMULATIONS / "micro-uchuu" / "converter_columns.yaml").read_text()
        broken.write_text(text.replace("extra_fields: []", "extra_fields: []\nsurprise: 1"))
        result = self.ingest_fails(self.with_option("--column-map", broken), str(broken))
        self.assertIn("surprise", result.stderr)
        self.ingest_fails(self.with_option("--column-map", self.tmp / "absent.yaml"), "absent.yaml")

    def test_simulation_metadata_of_another_format_is_refused(self):
        other = SIMULATIONS / "uchuu" / "simulation_info.yaml"
        self.ingest_fails(self.with_option("--simulation-info", other), "input.tree_type")

    def test_an_option_of_another_format_is_refused_rather_than_ignored(self):
        cases = (
            ("--forests-list", "do not apply to --source-format lhalo_binary"),
            ("--cut-table", "--cut-table do not apply to --source-format lhalo_binary"),
        )
        for option, message in cases:
            with self.subTest(option=option):
                self.ingest_fails(self.route + [option, self.data / "x.list"], message)

    def test_a_missing_inventory_option_is_named(self):
        index = self.route.index("--tree-name")
        route = self.route[:index] + self.route[index + 2 :]
        self.ingest_fails(route, "missing --tree-name")

    def test_a_file_range_the_source_cannot_honour_is_refused_instead_of_narrowed(self):
        cases = (
            ("--last-file", "4", "lie outside the 0-3"),
            ("--last-file", "1", "Uchuu100_test_lhalo_binary.1"),
        )
        for flag, value, message in cases:
            with self.subTest(range=value):
                self.ingest_fails(self.with_option(flag, value), message)

    def test_a_nonpositive_memory_budget_is_refused(self):
        self.ingest_fails(self.route + ["--memory-budget-mb", "0"], "must be positive")

    def test_stages_refuse_to_run_out_of_order(self):
        self.cli("ingest", "--workdir", self.work, *self.route)
        result = self.cli(
            "write",
            "--workdir",
            self.work,
            "--simulation-info",
            route_value(self.route, "--simulation-info"),
            expect=2,
        )
        self.assertIn("transpose", result.stderr)
        self.cli("validate", "--workdir", self.work, expect=2)

    def test_conflicting_resume_inputs_fail_before_mutation(self):
        self.cli("ingest", "--workdir", self.work, *self.route)
        before = tree_state(self.work)
        extras = CONVERT_DIR / "profiles" / "lhalo_binary_extras_example.yaml"
        for route in (
            self.with_option("--column-map", extras),
            self.route + ["--memory-budget-mb", "1024"],
            self.route + ["--ingest-max-rows", "2"],
        ):
            result = self.cli("ingest", "--workdir", self.work, *route, expect=2)
            self.assertIn("different conversion", result.stderr)
            self.assertEqual(tree_state(self.work), before)
        # The identical configuration is a verified no-op.
        self.cli("ingest", "--workdir", self.work, *self.route)
        self.assertEqual(tree_state(self.work), before)

    def test_resume_needs_either_the_whole_configuration_or_none_of_it(self):
        nothing_recorded = self.tmp / "nothing_recorded"
        result = self.cli("ingest", "--workdir", nothing_recorded, expect=2)
        self.assertIn("no generic conversion to resume", result.stderr)
        self.cli("ingest", "--workdir", self.work, *self.route)
        result = self.cli(
            "ingest",
            "--workdir",
            self.work,
            "--a-list",
            route_value(self.route, "--a-list"),
            expect=2,
        )
        self.assertIn("given without --source-format", result.stderr)
        result = self.cli("ingest", "--workdir", self.work, "--memory-budget-mb", "8", expect=2)
        self.assertIn("--memory-budget-mb given without --source-format", result.stderr)

    def test_resume_uses_the_embedded_configuration_after_the_profile_is_gone(self):
        profile = self.tmp / "profile.yaml"
        shutil.copy(SIMULATIONS / "micro-uchuu" / "converter_columns.yaml", profile)
        self.cli("ingest", "--workdir", self.work, *self.with_option("--column-map", profile))
        profile.unlink()
        result = self.cli("ingest", "--workdir", self.work)
        self.assertIn("resuming the conversion recorded", result.stdout)
        self.cli("transpose", "--workdir", self.work)
        self.cli(
            "write",
            "--workdir",
            self.work,
            "--simulation-info",
            route_value(self.route, "--simulation-info"),
        )
        self.cli("validate", "--workdir", self.work)

    def test_a_scatter_manifest_workdir_is_never_adopted(self):
        work = make_scatter_workdir(self.tmp / "scattered")
        before = tree_state(work)
        result = self.cli("ingest", "--workdir", work, *self.route, expect=2)
        self.assertIn("holds a scatter manifest", result.stderr)
        self.assertEqual(tree_state(work), before)


class BindingRoute(NamedTuple):
    """One route of a metadata-binding case: its explicit configuration, the simulation_info
    of another package on the same route, and the recorded box size with the text that edits it."""

    label: str
    route: list
    other: Path
    box_size: float
    edit_from: str
    edit_to: str


class MetadataBindingTests(CliCase):
    """A conversion is bound to the simulation_info it was ingested with: the header's box
    size and cosmology come from that file at write time, and nothing in the source would
    catch a different one.

    L-Halo: mini-Millennium (62.5 Mpc/h) and Millennium (500 Mpc/h) share cosmology and
    particle mass, so only the file's content identity tells them apart. Forests-HDF5: full
    Uchuu (2000 Mpc/h) and micro-Uchuu (100 Mpc/h) do too, so the writer's particle_mass
    agreement check alone cannot tell them apart.
    """

    def routes(self):
        return (
            BindingRoute(
                "lhalo_binary",
                lhalo_route(
                    "mini-millennium", self.literal_gapped_source(), "trees_063", last_file=1
                ),
                SIMULATIONS / "millennium" / "simulation_info.yaml",
                62.5,
                "value: 62.5",
                "value: 500.0",
            ),
            BindingRoute(
                "consistent_trees_hdf5",
                hdf5_route("uchuu", "mergertree_info.h5"),
                SIMULATIONS / "micro-uchuu-hdf5" / "simulation_info.yaml",
                2000.0,
                "value: 2000.0",
                "value: 100.0",
            ),
        )

    def ingest_and_transpose(self, case, sim_info=None):
        route = list(case.route)
        if sim_info is not None:
            route[route.index("--simulation-info") + 1] = sim_info
        work = self.tmp / "work_{}".format(case.label)
        self.cli("ingest", "--workdir", work, *route)
        self.cli("transpose", "--workdir", work)
        return work, route

    def assert_nothing_written(self, work):
        written = sorted(str(p) for p in (work / "write").rglob("*") if p.is_file())
        self.assertEqual(written, [], "a refused write created output files")
        manifest = json.loads((work / "manifest.json").read_text())
        self.assertNotEqual(manifest["stages"]["write"]["status"], "complete")

    def test_write_refuses_another_packages_simulation_info(self):
        for case in self.routes():
            with self.subTest(route=case.label):
                work, route = self.ingest_and_transpose(case)
                recorded = route_value(route, "--simulation-info")
                result = self.cli(
                    "write", "--workdir", work, "--simulation-info", case.other, expect=2
                )
                self.assertIn("different simulation_info", result.stderr)
                self.assert_nothing_written(work)
                # The recorded metadata still writes, and its box size is the header's.
                self.cli("write", "--workdir", work, "--simulation-info", recorded)
                manifest = json.loads((work / "manifest.json").read_text())
                dataset = work / manifest["stages"]["write"]["directory"]
                with h5py.File(dataset / "snapshot_000.h5", "r") as handle:
                    self.assertEqual(float(handle["header"].attrs["box_size_mpc_h"]), case.box_size)

    def test_metadata_edited_after_ingest_is_refused_at_write(self):
        for case in self.routes():
            with self.subTest(route=case.label):
                edited = self.tmp / "simulation_info_{}.yaml".format(case.label)
                shutil.copy(route_value(case.route, "--simulation-info"), edited)
                work, _route = self.ingest_and_transpose(case, sim_info=edited)
                edited.write_text(edited.read_text().replace(case.edit_from, case.edit_to))
                result = self.cli("write", "--workdir", work, "--simulation-info", edited, expect=2)
                self.assertIn(str(edited.resolve()), result.stderr)
                self.assert_nothing_written(work)

    def test_resume_with_another_packages_simulation_info_is_refused(self):
        """The recorded simulation_info is part of the conversion's identity on every route,
        so the refusal is exercised on the L-Halo route."""
        case = self.routes()[0]
        work, route = self.ingest_and_transpose(case)
        before = tree_state(work)
        other = list(route)
        other[other.index("--simulation-info") + 1] = case.other
        result = self.cli("ingest", "--workdir", work, *other, expect=2)
        self.assertIn("different conversion", result.stderr)
        self.assertEqual(tree_state(work), before)


class LHaloWriterBindingTests(unittest.TestCase):
    """The v3 writer's own L-Halo agreement check, reached through the
    pipeline API rather than the CLI (whose earlier check would fire first):
    a conversion that recorded its simulation_info refuses a writer built
    from different metadata, before any output file exists."""

    def test_the_writer_refuses_metadata_the_conversion_did_not_record(self):
        import pipeline
        from column_schema import ConverterError
        from hdf5_writer_v3 import HorizontalV3Writer

        tmp = Path(tempfile.mkdtemp(prefix="cli_writer_"))
        self.addCleanup(shutil.rmtree, tmp, ignore_errors=True)
        source = tmp / "src"
        source.mkdir()
        fixtures.write_lhalo_file(
            str(source / "trees.0"), [fixtures.LITERAL_TREE_A, fixtures.LITERAL_TREE_B]
        )
        a_list = source / "a.list"
        a_list.write_text(fixtures.LITERAL_A_LIST_TEXT)
        mini = SIMULATIONS / "mini-millennium" / "simulation_info.yaml"
        work = tmp / "work"
        pipeline.initialize(
            work,
            fixtures.lhalo_schema(),
            {"sources": [[0, str(source / "trees.0")]], "simulation_info": str(mini)},
            a_list,
            transpose_budget_bytes=fixtures.LITERAL_BUDGET,
        )
        pipeline.run_ingest(work)
        pipeline.run_transpose(work)
        other = SIMULATIONS / "millennium" / "simulation_info.yaml"
        with self.assertRaisesRegex(ConverterError, "L-Halo conversion was recorded against"):
            pipeline.run_write(work, HorizontalV3Writer(other))
        self.assertEqual([p for p in (work / "write").rglob("*") if p.is_file()], [])
        # HorizontalV3Writer's completion line goes through hdf5_writer_v3._log (stderr);
        # capture both streams so this in-process call cannot leak either into the
        # test runner's own output, regardless of which one a future change uses.
        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            manifest = pipeline.run_write(work, HorizontalV3Writer(mini))
        self.assertTrue(manifest.is_complete("write"))


# ==========================================================================
# Output identification (in process: a wide slab cannot be manufactured)
# ==========================================================================


class OutputIdentificationTests(unittest.TestCase):
    def topology(self, *args):
        buffer = io.StringIO()
        with contextlib.redirect_stdout(buffer):
            convert_trees._print_topology(*args)
        return buffer.getvalue()

    def test_a_snapshot_above_int32_is_identified_as_wide(self):
        text = self.topology(0, 0, True, [5, 2**31, 7])
        self.assertIn("WIDE output", text)
        self.assertIn("only when that snapshot is a requested output snapshot", text)
        self.assertIn("warns above 1e9 rows", text)
        self.assertIn(str(2**31), text)
        self.assertIn("links_adjacent=1", text)
        # every route's output is forest-blocked, ASCII included, so chunked
        # sweeps apply to all three
        for route in ("lhalo_binary", "consistent_trees_hdf5", "consistent_trees_ascii"):
            self.assertIn(route, text)
        self.assertIn("forest-blocked", text)
        self.assertIn("chunked sweeps", text)
        self.assertNotIn("not forest-blocked", text)
        self.assertNotIn("cannot be chunked", text)

    def test_a_snapshot_at_int32_max_is_not_wide(self):
        text = self.topology(0, 0, True, [2**31 - 1])
        self.assertNotIn("WIDE", text)
        self.assertIn("within INT32_MAX", text)

    def test_gapped_links_are_identified_as_nonadjacent(self):
        text = self.topology(29291, 2, False, [10])
        self.assertIn("NONADJACENT output: 29291 Descendant link(s) skip snapshots", text)
        self.assertIn("longest span 2", text)


if __name__ == "__main__":
    unittest.main()
