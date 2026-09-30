"""Generalisation tests: the package-local converter profiles and
user-defined extra fields end to end.

**Package profiles.** Every requested simulation has a declared route through
its own ``simulations/<package>/converter_columns.yaml``, and every one of the
seven profiles is checked against that package's source metadata -- the
ordered ``halo_properties.yaml`` for L-Halo binary (field order, types and
offsets recomputed here from the declared types), the committed fixture's
``Forests/`` datasets for forests-HDF5 and its header for ASCII.

**User-defined extras.** For each adapter, a profile written by the test with
freshly generated output names (so no stage can know them in advance) is run
through the generic CLI as a subprocess, and every emitted extra is compared
with values extracted from the source *independently* of the converter: a
hand-typed ``struct`` layout for L-Halo binary, raw ``h5py`` reads walked in
``ForestInfo`` order for forests-HDF5, and a plain text parse for ASCII.
"""

import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
import uuid
from pathlib import Path

import h5py
import numpy as np
import yaml

HERE = Path(__file__).resolve().parent
CONVERT_DIR = HERE.parent
REPO_ROOT = HERE.parents[2]
SIMULATIONS = REPO_ROOT / "simulations"
CONVERT_TREES = CONVERT_DIR / "convert_trees.py"

sys.path.insert(0, str(CONVERT_DIR))

import column_schema as cs  # noqa: E402
from ctrees_parser import parse_header_line, prescan_file, resolve_selection  # noqa: E402

#: The five supported simulations and their declared routes, plus the
#: retained ASCII route. Restated here, not imported.
REQUESTED_ROUTES = {
    "mini-millennium": "lhalo_binary",
    "millennium": "lhalo_binary",
    "micro-uchuu": "lhalo_binary",
    "mini-uchuu": "lhalo_binary",
    "uchuu": "consistent_trees_hdf5",
}
ADDITIONAL_ROUTES = {
    "micro-uchuu-hdf5": "consistent_trees_hdf5",
    "micro-uchuu-ascii": "consistent_trees_ascii",
}
PACKAGE_ROUTES = dict(REQUESTED_ROUTES, **ADDITIONAL_ROUTES)

#: halo_properties.yaml type -> (bytes per component, components).
TYPE_WIDTHS = {
    "int": (4, 1),
    "long long": (8, 1),
    "float": (4, 1),
    "double": (8, 1),
    "vec3_int": (4, 3),
    "vec3_float": (4, 3),
}

#: The five links: format-table topology in v3, not /schema payload, but
#: declared in a package's halo_properties.yaml all the same.
TOPOLOGY_LINKS = (
    "Descendant",
    "FirstProgenitor",
    "NextProgenitor",
    "FirstHaloInFOFgroup",
    "NextHaloInFOFgroup",
)

HDF5_FIXTURES = {
    "uchuu": SIMULATIONS / "uchuu" / "_tests" / "data" / "mergertree_info.h5",
    "micro-uchuu-hdf5": SIMULATIONS
    / "micro-uchuu-hdf5"
    / "_tests"
    / "data"
    / "MicroUchuu_test_mergertree_info.h5",
}
ASCII_DATA = SIMULATIONS / "micro-uchuu-ascii" / "_tests" / "data"


def profile_path(package):
    return SIMULATIONS / package / "converter_columns.yaml"


def declared_properties(package):
    with open(SIMULATIONS / package / "halo_properties.yaml") as handle:
        return yaml.safe_load(handle)["halo_properties"]


def unused(prop):
    return "Unused catalog field" in str(prop.get("notes", ""))


def fresh_name(prefix):
    """An output name no converter module can contain."""
    return "{}{}".format(prefix, uuid.uuid4().hex[:12])


def run_cli(args, cwd):
    return subprocess.run(
        [sys.executable, str(CONVERT_TREES)] + [str(arg) for arg in args],
        cwd=str(cwd),
        capture_output=True,
        text=True,
    )


# ==========================================================================
# The package profiles against their source metadata
# ==========================================================================


class PackageProfileTests(unittest.TestCase):
    def test_every_requested_simulation_has_a_declared_route(self):
        self.assertEqual(
            sorted(format_ for format_ in REQUESTED_ROUTES.values()),
            ["consistent_trees_hdf5"] + ["lhalo_binary"] * 4,
        )
        for package, source_format in PACKAGE_ROUTES.items():
            with self.subTest(package=package):
                column_map = cs.load_column_map(profile_path(package))
                self.assertEqual(column_map.source_format, source_format)
                with open(SIMULATIONS / package / "simulation_info.yaml") as handle:
                    info = yaml.safe_load(handle)
                self.assertEqual(info["input"]["tree_type"], source_format)

    def test_binary_layouts_match_the_ordered_halo_properties(self):
        for package in (p for p, f in PACKAGE_ROUTES.items() if f == "lhalo_binary"):
            with self.subTest(package=package):
                declared = declared_properties(package)
                schema = cs.build_schema(
                    cs.load_column_map(profile_path(package)),
                    cs.load_source_properties(SIMULATIONS / package / "halo_properties.yaml"),
                )
                layout = schema.source_layout
                self.assertEqual(layout.byte_order, "little")
                offset = 0
                entries = {entry.name: entry for entry in layout.entries}
                self.assertEqual(
                    [entry.name for entry in layout.entries], [p["name"] for p in declared]
                )
                for prop in declared:
                    width, components = TYPE_WIDTHS[prop["type"]]
                    self.assertEqual(entries[prop["name"]].offset, offset, prop["name"])
                    offset += width * components
                self.assertEqual(layout.itemsize, offset)
                self.assertEqual(offset, 104)
                # Exactly the fields the vertical run consumes are selected.
                consumed = {p["name"] for p in declared if not unused(p)}
                selected = {entry.name for entry in layout.entries if entry.selected}
                self.assertEqual(selected, consumed)
                self.assertEqual(schema.extra_fields, ())

    def test_hdf5_profiles_resolve_against_the_committed_fixtures(self):
        for package, fixture in HDF5_FIXTURES.items():
            with self.subTest(package=package):
                schema = cs.build_schema(cs.load_column_map(profile_path(package)))
                with h5py.File(fixture, "r") as handle:
                    available = list(handle["File0"]["Forests"].keys())
                resolved = cs.resolve_required_columns(schema, available)
                self.assertEqual(set(resolved), set(schema.roles))
                self.assertEqual(
                    {field.name for field in schema.payload_fields} | set(TOPOLOGY_LINKS),
                    {p["name"] for p in declared_properties(package)},
                )
                self.assertEqual(schema.extra_fields, ())

    def test_ascii_profile_resolves_against_the_committed_fixture(self):
        schema = cs.build_schema(cs.load_column_map(profile_path("micro-uchuu-ascii")))
        scan = prescan_file(ASCII_DATA / "tree_0_0_0.dat")
        resolve_selection(parse_header_line(scan.header_line), schema)
        self.assertEqual(
            {field.name for field in schema.payload_fields} | set(TOPOLOGY_LINKS),
            {p["name"] for p in declared_properties("micro-uchuu-ascii")},
        )
        self.assertEqual(schema.extra_fields, ())

    def test_package_profiles_are_not_wired_into_production_configuration(self):
        """A profile selects source fields; no run YAML, halo_properties.yaml
        or simulation_info.yaml refers to it."""
        for package in PACKAGE_ROUTES:
            root = SIMULATIONS / package
            candidates = [root / "halo_properties.yaml", root / "simulation_info.yaml"]
            candidates += sorted((root / "_tests").rglob("*.yaml"))
            candidates += sorted(REPO_ROOT.glob("models/*/input/*{}*.yaml".format(package)))
            for path in candidates:
                with self.subTest(path=str(path)):
                    self.assertNotIn("converter_columns", path.read_text())


# ==========================================================================
# User-defined extras, end to end, against independent extraction
# ==========================================================================


def write_profile(path, base_profile, extras):
    """The package profile with ``extras`` in place of its empty list."""
    document = yaml.safe_load(Path(base_profile).read_text())
    document["extra_fields"] = extras
    path.write_text(yaml.safe_dump(document, sort_keys=False))
    return path


def extra(name, sources, type_name, units, description):
    return {
        "name": name,
        "sources": sources,
        "type": type_name,
        "units": units,
        "h_convention": "none",
        "description": description,
    }


def read_output(dataset_dir, names):
    """Every snapshot file's SourceHaloID, MostBoundID and the named extras,
    concatenated; plus each extra's /schema declaration."""
    columns = {name: [] for name in ("SourceHaloID", "MostBoundID") + tuple(names)}
    declarations = {}
    for path in sorted(Path(dataset_dir).glob("snapshot_*.h5")):
        with h5py.File(path, "r") as handle:
            for name in columns:
                columns[name].append(handle["halos"][name][...])
            for name in names:
                attrs = handle["schema"][name].attrs
                declarations[name] = {key: attrs[key] for key in ("type", "units")}
                dtype = handle["halos"][name].dtype
                declarations[name]["dtype"] = dtype.str
    return {name: np.concatenate(parts) for name, parts in columns.items()}, declarations


class UserExtrasEndToEndTests(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="generalisation_"))
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)

    def convert(self, route, sim_info, ingest=()):
        work = self.tmp / "work"
        for args in (
            ["ingest", "--workdir", work] + route + list(ingest),
            ["transpose", "--workdir", work],
            ["write", "--workdir", work, "--simulation-info", sim_info],
            ["validate", "--workdir", work],
        ):
            result = run_cli(args, self.tmp)
            self.assertEqual(
                result.returncode, 0, "{}:\n{}\n{}".format(args[0], result.stdout, result.stderr)
            )
        dataset = sorted((work / "write").glob("attempt_*"))
        self.assertEqual(len(dataset), 1)
        return dataset[0]

    def test_lhalo_binary_extras_match_direct_record_reads(self):
        package = SIMULATIONS / "micro-uchuu"
        source = package / "_tests" / "data" / "Uchuu100_test_lhalo_binary.0"
        mass, index, spin, ident, pos_y = (fresh_name(p) for p in ("Bm", "Bi", "Bs", "Bl", "By"))
        profile = write_profile(
            self.tmp / "binary.yaml",
            profile_path("micro-uchuu"),
            [
                extra(mass, [{"field": "M_Mean200"}], "float", "1e10 Msun/h", "user mass"),
                extra(index, [{"field": "SubhaloIndex"}], "int", "dimensionless", "user index"),
                extra(
                    spin,
                    [{"field": "Spin", "component": c} for c in (0, 1, 2)],
                    "vec3_float",
                    "Mpc/h km/s",
                    "user vector",
                ),
                extra(ident, [{"field": "MostBoundID"}], "long long", "dimensionless", "user id"),
                extra(
                    pos_y, [{"field": "Pos", "component": 1}], "float", "Mpc/h", "user component"
                ),
            ],
        )
        dataset = self.convert(
            [
                "--source-format",
                "lhalo_binary",
                "--simulation-info",
                package / "simulation_info.yaml",
                "--a-list",
                package / "micro-uchuu.a_list",
                "--column-map",
                profile,
                "--halo-properties",
                package / "halo_properties.yaml",
                "--source-dir",
                source.parent,
                "--tree-name",
                "Uchuu100_test_lhalo_binary",
                "--first-file",
                "0",
                "--last-file",
                "0",
            ],
            package / "simulation_info.yaml",
        )
        got, declarations = read_output(dataset, (mass, index, spin, ident, pos_y))

        # Independent extraction: the 104-byte record, offsets hand-typed.
        raw = source.read_bytes()
        ntrees, nhalos = struct.unpack_from("<ii", raw, 0)
        start = 8 + 4 * ntrees
        expected = {mass: [], index: [], spin: [], ident: [], pos_y: []}
        for row in range(nhalos):
            base = start + 104 * row
            expected[mass].append(struct.unpack_from("<f", raw, base + 24)[0])
            expected[index].append(struct.unpack_from("<i", raw, base + 96)[0])
            expected[spin].append(struct.unpack_from("<3f", raw, base + 68))
            expected[ident].append(struct.unpack_from("<q", raw, base + 80)[0])
            expected[pos_y].append(struct.unpack_from("<f", raw, base + 36 + 4)[0])
        self.assertEqual(len(raw), start + 104 * nhalos)
        # One file: SourceHaloID is 1 + the record's position in the file.
        order = got["SourceHaloID"] - 1
        self.assertEqual(sorted(order.tolist()), list(range(nhalos)))
        for name, dtype in (
            (mass, "<f4"),
            (index, "<i4"),
            (spin, "<f4"),
            (ident, "<i8"),
            (pos_y, "<f4"),
        ):
            want = np.asarray(expected[name], dtype=dtype)[order]
            np.testing.assert_array_equal(got[name], want, err_msg=name)
            self.assertEqual(declarations[name]["dtype"], dtype, name)
        np.testing.assert_array_equal(got[ident], got["MostBoundID"])
        self.assertEqual(got[spin].shape, (nhalos, 3))
        self.assertEqual(declarations[spin]["type"], "vec3_float")
        self.assertEqual(declarations[mass]["units"], "1e10 Msun/h")

    def test_forests_hdf5_extras_match_raw_forest_reads(self):
        package = SIMULATIONS / "micro-uchuu-hdf5"
        fixture = HDF5_FIXTURES["micro-uchuu-hdf5"]
        mass, catalog, jz = (fresh_name(p) for p in ("Hm", "Hc", "Hj"))
        profile = write_profile(
            self.tmp / "hdf5.yaml",
            profile_path("micro-uchuu-hdf5"),
            [
                extra(mass, [{"field": "Mvir"}], "double", "Msun/h", "raw catalog mass"),
                extra(catalog, [{"field": "id"}], "long long", "dimensionless", "catalog id"),
                extra(jz, [{"field": "Jz"}], "double", "Msun/h Mpc/h km/s", "raw Jz"),
            ],
        )
        dataset = self.convert(
            [
                "--source-format",
                "consistent_trees_hdf5",
                "--simulation-info",
                package / "simulation_info.yaml",
                "--a-list",
                package / "micro-uchuu.a_list",
                "--column-map",
                profile,
                "--info-file",
                fixture,
                "--first-file",
                "0",
                "--last-file",
                "0",
            ],
            package / "simulation_info.yaml",
        )
        got, declarations = read_output(dataset, (mass, catalog, jz))

        # Independent extraction: ForestInfo row order, then within-forest
        # row order, numbering from 1.
        expected = {}
        with h5py.File(fixture, "r") as handle:
            group = handle["File0"]
            info = group["ForestInfo"][...]
            forests = group["Forests"]
            columns = {field: forests[field][...] for field in ("Mvir", "id", "Jz")}
        source_halo_id = 1
        for forest in info:
            first = int(forest["ForestHalosOffset"])
            for row in range(first, first + int(forest["ForestNhalos"])):
                expected[source_halo_id] = {
                    mass: columns["Mvir"][row],
                    catalog: columns["id"][row],
                    jz: columns["Jz"][row],
                }
                source_halo_id += 1
        self.assertEqual(sorted(got["SourceHaloID"].tolist()), sorted(expected))
        for name, dtype in ((mass, "<f8"), (catalog, "<i8"), (jz, "<f8")):
            want = np.asarray([expected[int(s)][name] for s in got["SourceHaloID"]], dtype=dtype)
            np.testing.assert_array_equal(got[name], want, err_msg=name)
            self.assertEqual(declarations[name]["dtype"], dtype, name)

    def test_ascii_extras_match_a_plain_text_parse(self):
        package = SIMULATIONS / "micro-uchuu-ascii"
        tree = ASCII_DATA / "tree_0_0_0.dat"
        rvir, nprog, root, jvec = (fresh_name(p) for p in ("Ar", "An", "At", "Aj"))
        profile = write_profile(
            self.tmp / "ascii.yaml",
            profile_path("micro-uchuu-ascii"),
            [
                extra(rvir, [{"field": "Rvir"}], "float", "kpc/h", "catalog radius"),
                extra(nprog, [{"field": "num_prog"}], "int", "dimensionless", "progenitors"),
                extra(root, [{"field": "Tree_root_ID"}], "long long", "dimensionless", "root id"),
                extra(
                    jvec,
                    [{"field": "Jx"}, {"field": "Jy"}, {"field": "Jz"}],
                    "vec3_float",
                    "Msun/h Mpc/h km/s",
                    "raw J",
                ),
            ],
        )
        dataset = self.convert(
            [
                "--source-format",
                "consistent_trees_ascii",
                "--simulation-info",
                package / "simulation_info.yaml",
                "--a-list",
                package / "micro-uchuu.a_list",
                "--column-map",
                profile,
                "--forests-list",
                ASCII_DATA / "forests.list",
                "--tree-file",
                tree,
            ],
            package / "simulation_info.yaml",
            ingest=("--ingest-max-rows", "4096"),
        )
        got, declarations = read_output(dataset, (rvir, nprog, root, jvec))

        # Independent extraction: header names with the "(n)" suffix removed;
        # data rows are the lines carrying the header's full token count.
        lines = tree.read_text().splitlines()
        header = [token.split("(")[0].lower() for token in lines[0].lstrip("#").split()]
        rows = [line.split() for line in lines[1:] if not line.startswith("#")]
        rows = [row for row in rows if len(row) == len(header)]
        column = {name: position for position, name in enumerate(header)}
        by_id = {int(row[column["id"]]): row for row in rows}
        # The ctrees route carries the catalog id as MostBoundID.
        self.assertEqual(sorted(got["MostBoundID"].tolist()), sorted(by_id))
        halos = [by_id[int(i)] for i in got["MostBoundID"]]
        np.testing.assert_array_equal(
            got[rvir], np.asarray([float(r[column["rvir"]]) for r in halos], dtype="<f4")
        )
        np.testing.assert_array_equal(
            got[nprog], np.asarray([int(r[column["num_prog"]]) for r in halos], dtype="<i4")
        )
        np.testing.assert_array_equal(
            got[root], np.asarray([int(r[column["tree_root_id"]]) for r in halos], dtype="<i8")
        )
        raw_j = [[float(r[column[axis]]) for axis in ("jx", "jy", "jz")] for r in halos]
        np.testing.assert_array_equal(got[jvec], np.asarray(raw_j, dtype="<f4"))
        self.assertEqual(declarations[jvec]["type"], "vec3_float")
        for name, dtype in ((rvir, "<f4"), (nprog, "<i4"), (root, "<i8"), (jvec, "<f4")):
            self.assertEqual(declarations[name]["dtype"], dtype, name)


if __name__ == "__main__":
    unittest.main()
