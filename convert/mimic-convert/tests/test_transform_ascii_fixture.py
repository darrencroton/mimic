"""The cut-fixture transformation for the reference reader
(convert/mimic-convert/tests/tools/transform_ascii_fixture.py).

On :mod:`fixtures`' adversarial cut forests, each beside the two untouched
forests the cut tests use:

- the transformed files parse under ``ctrees_parser`` with the expected hosts:
  every halo the cut promotes (stated literally per fixture) and every central
  in the raw convention, ``pid`` -1 and ``upid`` -1, every other satellite on
  its resolved central, and every other column as in the input;
- the cut table is the new ``forests.list`` and ``locations.dat`` points at
  each block's first data row in the rewritten files;
- the converter's route over the transformed files, with no cut table, equals
  its route over the original files under the table, in every ``/halos``
  column, header attribute and sidecar value: the transformed source carries
  exactly the cut topology to a reader that knows nothing of a cut;
- a source whose hosts cannot be resolved under the original partition, and a
  used output directory, are refused.
"""

import io
import os
import shutil
import sys
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "tools"))

import fixtures  # noqa: E402
import transform_ascii_fixture  # noqa: E402
from ctrees_parser import parse_file  # noqa: E402
from errors import ConverterError  # noqa: E402
from test_ascii_adapter import CUT_FIXTURES, convert_under_cut  # noqa: E402
from test_rewriter import assert_datasets_equal, companions, cut_layout  # noqa: E402

#: The halos each fixture's decided table promotes (its docstring's account).
PROMOTED = {
    "severed": {2111},
    "no_central": {2211, 2212},
    "leaves": {2311, 2321},
    "tie": {2452},
    "three": {2501, 2521, 2522},
    "z0": {2621},
    "head": {2753},
}


def write_source(root: Path, forest, layout, table) -> Path:
    """A fixture forest beside the companions, in ``layout``'s files, with its
    ``forests.list``, the run metadata and the cut table."""
    root.mkdir(parents=True)
    forests = [companions()[0], forest, companions()[1]]
    for index, trees in enumerate(layout):
        fixtures.write_ctrees_file(root / "tree_{}.dat".format(index), trees)
    fixtures.write_forests_list(root / "forests.list", forests)
    fixtures.write_simulation_info(root / "simulation_info.yaml")
    fixtures.write_a_list(root / "fixture.a_list")
    return fixtures.write_cut_table(root / "cut.list", table, forests)


def transform(source: Path, out: Path):
    with redirect_stdout(io.StringIO()):
        return transform_ascii_fixture.transform(
            sorted(source.glob("tree_*.dat")), source / "forests.list", source / "cut.list", out
        )


def parsed(directory: Path):
    """Every record of a directory's tree files, keyed by halo id."""
    by_id = {}
    for path in sorted(directory.glob("tree_*.dat")):
        records, _result, _prescan = parse_file(path)
        for record in records:
            by_id[int(record["id"])] = record
    return by_id


class TransformTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.root = Path(tempfile.mkdtemp(prefix="transform_ascii_"))

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.root, ignore_errors=True)

    def test_the_transformed_files_carry_the_cut_hosts(self):
        for name, forest_of, table_of, split_after in CUT_FIXTURES:
            with self.subTest(fixture=name):
                root = self.root / "hosts" / name
                forest = forest_of()
                write_source(root / "src", forest, cut_layout(forest, split_after), table_of())
                summary = transform(root / "src", root / "out")
                before, after = parsed(root / "src"), parsed(root / "out")
                self.assertEqual(sorted(before), sorted(after))
                promoted = set()
                for halo_id, record in after.items():
                    original = before[halo_id]
                    for column in original.dtype.names:
                        if column not in ("pid", "upid"):
                            self.assertEqual(record[column], original[column], (halo_id, column))
                    hosts = (int(record["pid"]), int(record["upid"]))
                    if int(original["pid"]) == -1:
                        self.assertEqual(hosts, (-1, -1), halo_id)
                    elif hosts == (-1, -1):
                        promoted.add(halo_id)
                    else:
                        # the fixtures' satellites already name their ultimate central
                        self.assertEqual(hosts, (int(original["upid"]),) * 2, halo_id)
                self.assertEqual(promoted, PROMOTED[name])
                self.assertEqual(
                    sum(summary["promoted_per_snapshot"].values()), len(PROMOTED[name])
                )
                self.assertEqual(
                    (root / "out" / "forests.list").read_bytes(),
                    (root / "src" / "cut.list").read_bytes(),
                )
                self.assert_locations(root / "out")

    def assert_locations(self, directory: Path):
        names = sorted(path.name for path in directory.glob("tree_*.dat"))
        entries = (directory / "locations.dat").read_text().splitlines()
        self.assertEqual(entries[0], "#TreeRootID FileID Offset Filename")
        roots = []
        for entry in entries[1:]:
            root, file_id, offset, name = entry.split()
            self.assertEqual(name, names[int(file_id)])
            text = (directory / name).read_bytes()
            marker = "#tree {}\n".format(root).encode("ascii")
            self.assertEqual(text[: int(offset)][-len(marker) :], marker, entry)
            roots.append(int(root))
        listed = [
            int(line.split()[0])
            for line in (directory / "forests.list").read_text().splitlines()[1:]
        ]
        self.assertEqual(sorted(roots), sorted(listed))

    def test_the_route_reads_the_transformed_files_as_the_cut_catalogue(self):
        for name, forest_of, table_of, split_after in CUT_FIXTURES:
            with self.subTest(fixture=name):
                root = self.root / "route" / name
                forest = forest_of()
                table = write_source(
                    root / "src", forest, cut_layout(forest, split_after), table_of()
                )
                transform(root / "src", root / "out")
                for metadata in ("simulation_info.yaml", "fixture.a_list"):
                    shutil.copy(root / "src" / metadata, root / "out" / metadata)
                under_table = convert_under_cut({"root": root / "src"}, root / "cut_route", table)
                plain = convert_under_cut({"root": root / "out"}, root / "plain_route", None)
                assert_datasets_equal(
                    self,
                    under_table.artifact_path(under_table.stage("write")["directory"]),
                    plain.artifact_path(plain.stage("write")["directory"]),
                )

    def test_unresolvable_hosts_are_refused(self):
        table = fixtures.severed_satellite_cut_table()
        for name in ("missing", "foreign"):
            with self.subTest(case=name):
                root = self.root / "unresolved" / name
                forest = fixtures.severed_satellite_forest()
                if name == "missing":
                    # the satellite's host and its fallback name no halo of the snapshot
                    satellite = forest.trees[1].halos[1]
                    satellite.pid = satellite.upid = 9999
                write_source(root / "src", forest, cut_layout(forest, 1), table)
                if name == "foreign":
                    # tree 2110 listed as a forest of its own, so the host of its
                    # halo 2111 lies in another forest; the table is the identity
                    listed = (root / "src" / "forests.list").read_text()
                    listed = listed.replace("2110 2100", "2110 2150")
                    (root / "src" / "forests.list").write_text(listed)
                    (root / "src" / "cut.list").write_text(listed)
                with self.assertRaisesRegex(ConverterError, "cannot be resolved"):
                    transform(root / "src", root / "out")
                self.assertFalse((root / "out").exists(), "a refusal wrote its output")

    def test_a_used_output_directory_and_the_exit_status(self):
        forest = fixtures.severed_satellite_forest()
        root = self.root / "cli"
        write_source(
            root / "src", forest, cut_layout(forest, 1), fixtures.severed_satellite_cut_table()
        )
        (root / "out").mkdir()
        (root / "out" / "kept").write_text("x")
        argv = ["--forests-list", str(root / "src" / "forests.list")]
        argv += ["--cut-table", str(root / "src" / "cut.list"), "--output-dir", str(root / "out")]
        argv += [str(path) for path in sorted((root / "src").glob("tree_*.dat"))]
        stderr = io.StringIO()
        with redirect_stderr(stderr):
            self.assertEqual(transform_ascii_fixture.main(argv), 2)
        self.assertIn("the output directory is not empty", stderr.getvalue())
        self.assertEqual(sorted(path.name for path in (root / "out").iterdir()), ["kept"])
        argv[argv.index(str(root / "out"))] = str(root / "fresh")
        with redirect_stdout(io.StringIO()) as stdout:
            self.assertEqual(transform_ascii_fixture.main(argv), 0)
        self.assertIn('"promoted_per_snapshot": {"4": 1}', stdout.getvalue())
        self.assertTrue((root / "fresh" / "locations.dat").is_file())


if __name__ == "__main__":
    unittest.main()
