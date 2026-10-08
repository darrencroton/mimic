"""CLI tests: the generic ``convert_trees.py`` and the legacy
``convert_ctrees.py`` driven as subprocesses.

Every conversion here runs from a fresh temporary working directory, never
the repository root, with every path passed absolute, so nothing depends on
the repository being the current directory. Sources are the committed package
fixtures (read-only; their bytes are compared before and after) or literal
L-Halo trees written into the test's own temporary directory.

Coverage:

- every generic stage (inspect, ingest, transpose, write, validate, report) on
  each adapter -- L-Halo binary (micro-Uchuu, mini-Uchuu fixtures, and a
  literal gapped source under the mini-Millennium and Millennium packages),
  forests-HDF5 (full-Uchuu external-link fixture, micro-Uchuu fixture) and
  ASCII (micro-Uchuu fixture);
- the output contract: version 3 by default, the runtime-support notice on
  every stage, and the nonadjacent/wide identification;
- early failures: unknown formats, mismatched or broken profiles, foreign or
  missing inventory options, out-of-range and missing files, and conflicting
  resume inputs -- each before the workdir is mutated;
- resume from the embedded configuration after the profile file is gone;
- the legacy ASCII-to-v2 commands, still emitting format version 2.
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

import h5py

HERE = Path(__file__).resolve().parent
CONVERT_DIR = HERE.parent
REPO_ROOT = HERE.parents[2]
SIMULATIONS = REPO_ROOT / "simulations"
CONVERT_TREES = CONVERT_DIR / "convert_trees.py"
CONVERT_CTREES = CONVERT_DIR / "convert_ctrees.py"

sys.path.insert(0, str(CONVERT_DIR))
sys.path.insert(0, str(HERE))

import convert_trees  # noqa: E402
import runtime_routes  # noqa: E402
import test_lhalo_adapter as lhalo  # noqa: E402
import test_pipeline as literal  # noqa: E402

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
        """test_pipeline's literal trees: a three- and a two-snapshot
        descendant gap, in two files named like the Millennium packages'."""
        source = self.tmp / "literal"
        source.mkdir()
        lhalo.write_lhalo_file(str(source / "trees_063.0"), [literal.TREE_A, literal.TREE_B])
        lhalo.write_lhalo_file(str(source / "trees_063.1"), [literal.TREE_C])
        return source


# ==========================================================================
# Help and defaults
# ==========================================================================


class HelpTests(CliCase):
    def test_top_level_help_names_v3_and_the_runtime_boundary(self):
        text = " ".join(self.cli("--help").stdout.split())
        for command in ("inspect", "ingest", "transpose", "write", "validate", "report"):
            self.assertIn(command, text)
        self.assertIn("format version 3", text)
        self.assertIn("validated route only where a recorded parity gate passed", text)
        self.assertIn("full Uchuu is not claimed", text)
        self.assertIn("convert_ctrees.py", text)

    def test_runtime_notice_names_only_the_evidenced_routes(self):
        notice = convert_trees.RUNTIME_NOTICE
        self.assertTrue(notice.startswith(RUNTIME_MARK))
        self.assertIn("a conversion is not a validated route", notice)
        for route in runtime_routes.ROUTES:
            self.assertIn(route.simulation, notice)
        self.assertIn(runtime_routes.SPEC_ANCHOR, notice)
        self.assertIn(runtime_routes.FULL_UCHUU_NOT_CLAIMED, notice)

    def test_every_subcommand_has_help(self):
        for command in ("inspect", "ingest", "transpose", "write", "validate", "report"):
            self.cli(command, "--help")

    def test_write_help_states_format_version_3_and_offers_no_format_version_flag(self):
        text = " ".join(self.cli("write", "--help").stdout.split())
        self.assertIn("format version 3", text)
        self.assertNotIn("--format-version", text)
        result = self.cli(
            "write", "--workdir", "w", "--simulation-info", "x", "--format-version", "2", expect=2
        )
        self.assertIn("unrecognized arguments", result.stderr)

    def test_memory_budget_help_does_not_claim_an_rss_bound(self):
        text = " ".join(self.cli("ingest", "--help").stdout.split())
        self.assertIn("each checked before it is allocated", text)
        self.assertIn("not a bound on total interpreter RSS", text)

    def test_source_format_is_required_for_inspect(self):
        result = self.cli("inspect", "--simulation-info", "s", "--a-list", "a", expect=2)
        self.assertIn("--source-format", result.stderr)


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
        # Format capability ("format consumed"), not route validation.
        self.assertIs(report["runtime_compatibility"]["runnable_by_current_mimic"], True)
        if n_halos is not None:
            self.assertEqual(report["totals"]["halos"], n_halos)
        self.assertEqual(os.listdir(self.cwd), [], "a stage wrote into its working directory")
        self.assertEqual(package_files(package), before, "the conversion touched the package")
        return work

    def test_micro_uchuu_lhalo_binary(self):
        data = SIMULATIONS / "micro-uchuu" / "_tests" / "data"
        self.run_chain(
            lhalo_route("micro-uchuu", data, "Uchuu100_test_lhalo_binary"), "micro-uchuu", n_halos=6
        )

    def test_mini_uchuu_lhalo_binary(self):
        data = SIMULATIONS / "mini-uchuu" / "_tests" / "data"
        self.run_chain(
            lhalo_route("mini-uchuu", data, "Uchuu400_test_lhalo_binary"), "mini-uchuu", n_halos=6
        )

    def test_mini_millennium_package_on_a_gapped_literal_source(self):
        work = self.run_chain(
            lhalo_route("mini-millennium", self.literal_gapped_source(), "trees_063", last_file=1),
            "mini-millennium",
            expect_adjacent=False,
            n_halos=literal.TOTAL_HALOS,
        )
        stdout = self.cli("transpose", "--workdir", work).stdout
        # Tree A's snap-0 halo descends to snap 3; tree C's to snap 2.
        self.assertIn("2 Descendant link(s) skip snapshots (longest span 3)", stdout)

    def test_millennium_package_on_a_gapped_literal_source(self):
        self.run_chain(
            lhalo_route("millennium", self.literal_gapped_source(), "trees_063", last_file=1),
            "millennium",
            expect_adjacent=False,
            n_halos=literal.TOTAL_HALOS,
        )

    def test_full_uchuu_forests_hdf5_external_link_fixture(self):
        self.run_chain(hdf5_route("uchuu", "mergertree_info.h5"), "uchuu", n_halos=6)

    def test_micro_uchuu_forests_hdf5_fixture(self):
        self.run_chain(
            hdf5_route("micro-uchuu-hdf5", "MicroUchuu_test_mergertree_info.h5"),
            "micro-uchuu-hdf5",
            n_halos=6,
        )

    def test_micro_uchuu_ascii_fixture(self):
        # Runs at the default batch size: the ASCII adapter charges its batch
        # buffer per snapshot at min(max_rows, n_rows), so a small budget no
        # longer needs a matching --ingest-max-rows.
        self.run_chain(ascii_route(), "micro-uchuu-ascii", n_halos=4)

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

    def ingest_fails(self, route, message, expect=1):
        result = self.cli("ingest", "--workdir", self.work, *route, expect=expect)
        self.assertIn(message, result.stderr)
        self.assertFalse(self.work.exists(), "a refused ingest created its workdir")
        return result

    def test_an_unknown_source_format_is_rejected_by_the_parser(self):
        route = self.with_option("--source-format", "lhalo_hdf5")
        self.ingest_fails(route, "invalid choice", expect=2)

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
        route = self.route + ["--forests-list", self.data / "x.list"]
        self.ingest_fails(route, "do not apply to --source-format lhalo_binary")

    def test_a_missing_inventory_option_is_named(self):
        index = self.route.index("--tree-name")
        route = self.route[:index] + self.route[index + 2 :]
        self.ingest_fails(route, "missing --tree-name")

    def test_a_range_outside_the_declared_package_is_refused(self):
        self.ingest_fails(self.with_option("--last-file", "4"), "lie outside the 0-3")

    def test_a_missing_requested_file_fails_instead_of_narrowing(self):
        self.ingest_fails(self.with_option("--last-file", "1"), "Uchuu100_test_lhalo_binary.1")

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
            expect=1,
        )
        self.assertIn("transpose", result.stderr)
        self.cli("validate", "--workdir", self.work, expect=1)

    def test_conflicting_resume_inputs_fail_before_mutation(self):
        self.cli("ingest", "--workdir", self.work, *self.route)
        before = tree_state(self.work)
        extras = CONVERT_DIR / "profiles" / "lhalo_binary_extras_example.yaml"
        for route in (
            self.with_option("--column-map", extras),
            self.route + ["--memory-budget-mb", "1024"],
            self.route + ["--ingest-max-rows", "2"],
        ):
            result = self.cli("ingest", "--workdir", self.work, *route, expect=1)
            self.assertIn("different conversion", result.stderr)
            self.assertEqual(tree_state(self.work), before)
        # The identical configuration is a verified no-op.
        self.cli("ingest", "--workdir", self.work, *self.route)
        self.assertEqual(tree_state(self.work), before)

    def test_resume_needs_either_the_whole_configuration_or_none_of_it(self):
        self.cli("ingest", "--workdir", self.work, *self.route)
        result = self.cli(
            "ingest",
            "--workdir",
            self.work,
            "--a-list",
            route_value(self.route, "--a-list"),
            expect=1,
        )
        self.assertIn("given without --source-format", result.stderr)
        result = self.cli("ingest", "--workdir", self.work, "--memory-budget-mb", "8", expect=1)
        self.assertIn("--memory-budget-mb given without --source-format", result.stderr)

    def test_resume_with_nothing_recorded_is_refused(self):
        result = self.cli("ingest", "--workdir", self.work, expect=1)
        self.assertIn("no generic conversion to resume", result.stderr)

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

    def test_a_legacy_workdir_is_never_adopted(self):
        legacy = HERE / "data" / "legacy_manifest_v2" / "workdir"
        copy = self.tmp / "legacy"
        shutil.copytree(legacy, copy)
        before = tree_state(copy)
        result = self.cli("ingest", "--workdir", copy, *self.route, expect=1)
        self.assertIn("legacy ASCII-to-v2 workdir", result.stderr)
        self.assertEqual(tree_state(copy), before)


class LHaloMetadataBindingTests(CliCase):
    """An L-Halo conversion is bound to the simulation_info it was ingested
    with: the header's box size and cosmology come from that file at write
    time, and nothing in the binary source would catch a different one.

    mini-Millennium (62.5 Mpc/h) and Millennium (500 Mpc/h) share cosmology
    and particle mass, so only the file's content identity tells them apart.
    """

    def setUp(self):
        super().setUp()
        self.work = self.tmp / "work"
        self.mini = SIMULATIONS / "mini-millennium" / "simulation_info.yaml"
        self.full = SIMULATIONS / "millennium" / "simulation_info.yaml"

    def ingest_and_transpose(self, sim_info=None):
        route = lhalo_route(
            "mini-millennium", self.literal_gapped_source(), "trees_063", last_file=1
        )
        if sim_info is not None:
            route[route.index("--simulation-info") + 1] = sim_info
        self.cli("ingest", "--workdir", self.work, *route)
        self.cli("transpose", "--workdir", self.work)
        return route

    def assert_nothing_written(self):
        written = sorted(str(p) for p in (self.work / "write").rglob("*") if p.is_file())
        self.assertEqual(written, [], "a refused write created output files")
        manifest = json.loads((self.work / "manifest.json").read_text())
        self.assertNotEqual(manifest["stages"]["write"]["status"], "complete")

    def test_write_refuses_another_packages_simulation_info(self):
        self.ingest_and_transpose()
        result = self.cli("write", "--workdir", self.work, "--simulation-info", self.full, expect=1)
        self.assertIn("different simulation_info", result.stderr)
        self.assert_nothing_written()
        # The recorded metadata still writes, and its box size is the header's.
        self.cli("write", "--workdir", self.work, "--simulation-info", self.mini)
        manifest = json.loads((self.work / "manifest.json").read_text())
        dataset = self.work / manifest["stages"]["write"]["directory"]
        with h5py.File(dataset / "snapshot_000.h5", "r") as handle:
            self.assertEqual(float(handle["header"].attrs["box_size_mpc_h"]), 62.5)

    def test_metadata_edited_after_ingest_is_refused_at_write(self):
        edited = self.tmp / "simulation_info.yaml"
        shutil.copy(self.mini, edited)
        self.ingest_and_transpose(sim_info=edited)
        edited.write_text(edited.read_text().replace("value: 62.5", "value: 500.0"))
        result = self.cli("write", "--workdir", self.work, "--simulation-info", edited, expect=1)
        self.assertIn(str(edited.resolve()), result.stderr)
        self.assert_nothing_written()

    def test_resume_with_another_packages_simulation_info_is_refused(self):
        route = self.ingest_and_transpose()
        before = tree_state(self.work)
        other = list(route)
        other[other.index("--simulation-info") + 1] = self.full
        result = self.cli("ingest", "--workdir", self.work, *other, expect=1)
        self.assertIn("different conversion", result.stderr)
        self.assertEqual(tree_state(self.work), before)


class ForestsHDF5MetadataBindingTests(LHaloMetadataBindingTests):
    """The same binding on the forests-HDF5 route. Full Uchuu (2000 Mpc/h)
    and micro-Uchuu (100 Mpc/h) share cosmology and particle mass, so the
    writer's particle_mass agreement check alone cannot tell them apart."""

    def setUp(self):
        CliCase.setUp(self)
        self.work = self.tmp / "work"
        self.mini = SIMULATIONS / "uchuu" / "simulation_info.yaml"
        self.full = SIMULATIONS / "micro-uchuu-hdf5" / "simulation_info.yaml"

    def ingest_and_transpose(self, sim_info=None):
        route = hdf5_route("uchuu", "mergertree_info.h5")
        if sim_info is not None:
            route[route.index("--simulation-info") + 1] = sim_info
        self.cli("ingest", "--workdir", self.work, *route)
        self.cli("transpose", "--workdir", self.work)
        return route

    def test_write_refuses_another_packages_simulation_info(self):
        self.ingest_and_transpose()
        result = self.cli("write", "--workdir", self.work, "--simulation-info", self.full, expect=1)
        self.assertIn("different simulation_info", result.stderr)
        self.assert_nothing_written()
        self.cli("write", "--workdir", self.work, "--simulation-info", self.mini)
        manifest = json.loads((self.work / "manifest.json").read_text())
        dataset = self.work / manifest["stages"]["write"]["directory"]
        with h5py.File(dataset / "snapshot_000.h5", "r") as handle:
            self.assertEqual(float(handle["header"].attrs["box_size_mpc_h"]), 2000.0)

    def test_metadata_edited_after_ingest_is_refused_at_write(self):
        edited = self.tmp / "simulation_info.yaml"
        shutil.copy(self.mini, edited)
        self.ingest_and_transpose(sim_info=edited)
        edited.write_text(edited.read_text().replace("value: 2000.0", "value: 100.0"))
        result = self.cli("write", "--workdir", self.work, "--simulation-info", edited, expect=1)
        self.assertIn(str(edited.resolve()), result.stderr)
        self.assert_nothing_written()


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
        lhalo.write_lhalo_file(str(source / "trees.0"), [literal.TREE_A, literal.TREE_B])
        a_list = source / "a.list"
        a_list.write_text(literal.A_LIST_TEXT)
        mini = SIMULATIONS / "mini-millennium" / "simulation_info.yaml"
        work = tmp / "work"
        pipeline.initialize(
            work,
            literal.lhalo_schema(),
            {"sources": [[0, str(source / "trees.0")]], "simulation_info": str(mini)},
            a_list,
            transpose_budget_bytes=literal.BUDGET,
        )
        pipeline.run_ingest(work)
        pipeline.run_transpose(work)
        other = SIMULATIONS / "millennium" / "simulation_info.yaml"
        with self.assertRaisesRegex(ConverterError, "L-Halo conversion was recorded against"):
            pipeline.run_write(work, HorizontalV3Writer(other))
        self.assertEqual([p for p in (work / "write").rglob("*") if p.is_file()], [])
        # HorizontalV3Writer's completion line goes through hdf5_writer._log (stderr);
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
        self.assertIn(
            "output from every route (lhalo_binary, consistent_trees_hdf5 and "
            "consistent_trees_ascii sources) is forest-blocked, so chunked sweeps "
            "(input.forest_chunks) bound a run's memory by a chunk",
            text,
        )
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


# ==========================================================================
# The legacy ASCII-to-v2 commands
# ==========================================================================


class LegacyCliTests(CliCase):
    def test_convert_ctrees_still_emits_format_version_2(self):
        root = SIMULATIONS / "micro-uchuu-ascii"
        data = root / "_tests" / "data"
        sim_info = root / "simulation_info.yaml"
        a_list = root / "micro-uchuu.a_list"
        before = package_files("micro-uchuu-ascii")
        work = self.tmp / "legacy"
        steps = [
            [
                "scatter",
                "--workdir",
                work,
                "--forests-list",
                data / "forests.list",
                "--a-list",
                a_list,
                "--simulation-info",
                sim_info,
                data / "tree_0_0_0.dat",
            ],
            ["sort", "--workdir", work],
            ["fixups", "--workdir", work, "--a-list", a_list, "--simulation-info", sim_info],
            ["links", "--workdir", work],
            ["write", "--workdir", work, "--a-list", a_list, "--simulation-info", sim_info],
            ["report", "--workdir", work, "--a-list", a_list],
        ]
        for step in steps:
            result = run(CONVERT_CTREES, step, self.cwd)
            self.assertEqual(result.returncode, 0, "{}: {}".format(step[0], result.stderr))
        files = sorted((work / "hdf5").glob("snapshot_*.h5"))
        self.assertEqual(len(files), 50)
        for path in files:
            with h5py.File(path, "r") as handle:
                self.assertEqual(int(handle["header"].attrs["format_version"]), 2)
                self.assertEqual(set(handle.keys()), {"header", "halos"})
        self.assertEqual(os.listdir(self.cwd), [])
        self.assertEqual(package_files("micro-uchuu-ascii"), before)

    def test_convert_ctrees_help_offers_no_version_3_route(self):
        result = run(CONVERT_CTREES, ["write", "--help"], self.cwd)
        self.assertEqual(result.returncode, 0)
        self.assertNotIn("--format-version", result.stdout)
        top = run(CONVERT_CTREES, ["--help"], self.cwd)
        self.assertIn("convert_trees.py", " ".join(top.stdout.split()))


if __name__ == "__main__":
    unittest.main()
