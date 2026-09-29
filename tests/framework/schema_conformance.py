"""Shared schema-conformance tests for the version 3 horizontal packages.

A version 3 horizontal package's ``halo_properties.yaml`` must declare exactly
the ``/schema`` the converter writes for its route. Each
``simulations/<package>/_tests/integration/test_schema_conformance.py`` is a thin
file: a ``SchemaPackage`` naming what the package pins, and a ``main()`` calling
``run_schema_conformance(SPEC, title)``, which runs four tests:

1. ``test_converter_schema_is_the_pinned_route`` -- the vertical package's
   converter profile (scripts/convert/column_schema.py, the code that writes
   every file's ``/schema`` and stamps its ``column_mapping_sha256``) derives the
   pinned source format, digest and mass units. Needs no dataset.
2. ``test_yaml_declarations_match_converter_schema`` -- the YAML declarations
   match that derivation exactly: the five link roles are declared
   ``long long`` (Gate R0-2(a)) and excluded from the ``/schema`` comparison;
   the reader-owned format-table arrays (SourceHaloID, the three
   target-snapshot columns, ForestIndex, HaloRankInForest; Gate R0-3(a)) are
   not declared at all; the payload set is equal.
3. ``test_compiled_catalog_metadata_matches_converter_schema`` -- the
   ``catalog_field_metadata.inc`` this package's build compiled matches too. A
   missing file, or one generated for another package, FAILS: the test runs
   only with this package selected, and the registered tier regenerates first.
4. ``test_dataset_schemas_match_converter_schema`` -- every file of the real
   converted dataset behind ``snapshots/`` carries the pinned digest and exactly
   the derived ``/schema``; and every committed fixture dataset the package
   names (FAIL if absent) satisfies the one-way rule on every populated file:
   each declared payload field appears in ``/schema`` with equal type, units and
   h_convention, while undeclared ``/schema`` extras are allowed (the R0-8(a)
   extra ``SubHalfMass`` in tests/data/horizontal_v3/dataset is one). Skipped
   only when there is neither a real dataset nor a fixture to check.

Every test skips unless the compiled simulation is the package. Usage::

    make MODEL=halos-only SIMULATION=<package> generate
    MODEL=halos-only SIMULATION=<package> \\
        python3 simulations/<package>/_tests/integration/test_schema_conformance.py
"""

from __future__ import annotations

import re
import sys
from dataclasses import dataclass
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
for _path in (REPO_ROOT / "scripts", REPO_ROOT / "scripts" / "convert"):
    if str(_path) not in sys.path:
        sys.path.insert(0, str(_path))

from .harness import compiled_simulation  # noqa: E402
from .markers import TestSkipped  # noqa: E402
from .runner import run_test_suite  # noqa: E402

GENERATED_METADATA = REPO_ROOT / "src" / "include" / "generated" / "catalog_field_metadata.inc"

LINK_ROLES = frozenset(
    {"Descendant", "FirstProgenitor", "NextProgenitor", "FirstHaloInFOFgroup", "NextHaloInFOFgroup"}
)

#: Reader-owned under R0-3(a) and the ForestIndex/HaloRankInForest identity-array
#: precedent: format-table metadata, never declared catalog properties.
READER_OWNED_FIELDS = frozenset(
    {
        "SourceHaloID",
        "DescendantSnapshot",
        "FirstProgenitorSnapshot",
        "NextProgenitorSnapshot",
        "ForestIndex",
        "HaloRankInForest",
    }
)

#: The attributes compared per field.
COMPARED_KEYS = ("type", "units", "h_convention")

#: The package halo_properties.yaml a generated file names as its source.
GENERATED_FROM_RE = re.compile(r"simulations/[\w.-]+/halo_properties\.yaml")

CATALOG_FIELD_RE = re.compile(
    r'CATALOG_FIELD\(\s*\w+\s*,\s*"([^"]*)"\s*,\s*"([^"]*)"\s*,\s*"([^"]*)"\s*,'
    r'\s*"([^"]*)"\s*,\s*"([^"]*)"\s*,\s*"([^"]*)"\s*\)'
)


@dataclass(frozen=True)
class SchemaPackage:
    """What one version 3 package's schema-conformance tests pin.

    Args:
        package: The horizontal package under test.
        vertical_package: The package whose converter_columns.yaml is the route.
        source_format: The route's expected ``source_format``.
        column_mapping_sha256: The pinned digest of that route.
        mass_units: The mass role's (M_Crit200) native units, asserted
            explicitly because a wrong-by-1e10 mass is the defect a
            per-source-format package exists to stop.
        fixture_datasets: Repo-relative directories of committed version 3
            datasets that must exist and satisfy the one-way rule.
    """

    package: str
    vertical_package: str
    source_format: str
    column_mapping_sha256: str
    mass_units: str
    fixture_datasets: tuple[str, ...] = ()


def _text(value) -> str:
    return value.decode() if isinstance(value, bytes) else str(value)


def _assert_matches(name: str, found: dict, expected: dict, what: str) -> None:
    for key in COMPARED_KEYS:
        msg = f"{name}: {what} {key} {found[key]!r} != /schema {expected[key]!r}"
        assert found[key] == expected[key], msg


def _file_schema(handle) -> dict[str, dict[str, str]]:
    """One open v3 file's ``/schema`` as name -> {type, units, h_convention}."""
    return {
        name: {key: _text(value) for key, value in group.attrs.items() if key in COMPARED_KEYS}
        for name, group in handle["schema"].items()
    }


class SchemaConformance:
    """The four conformance tests for one package, as bound test callables."""

    def __init__(self, spec: SchemaPackage):
        self.spec = spec
        self.package_root = REPO_ROOT / "simulations" / spec.package
        self.vertical_root = REPO_ROOT / "simulations" / spec.vertical_package
        self.halo_properties = f"simulations/{spec.package}/halo_properties.yaml"

    def tests(self):
        return [
            self.test_converter_schema_is_the_pinned_route,
            self.test_yaml_declarations_match_converter_schema,
            self.test_compiled_catalog_metadata_matches_converter_schema,
            self.test_dataset_schemas_match_converter_schema,
        ]

    # ---- sources of truth ------------------------------------------------

    def _require_simulation(self) -> None:
        simulation = compiled_simulation()
        if simulation != self.spec.package:
            raise TestSkipped(f"compiled simulation is {simulation!r}, not {self.spec.package}")

    def _converter_schema(self):
        """The converter's canonical schema for the vertical package's profile."""
        import column_schema as cs

        column_map = cs.load_column_map(self.vertical_root / "converter_columns.yaml")
        source_properties = None
        if column_map.source_format == "lhalo_binary":
            source_properties = cs.load_source_properties(
                self.vertical_root / "halo_properties.yaml"
            )
        return cs.build_schema(column_map, source_properties)

    def _expected_declarations(self) -> dict[str, dict[str, str]]:
        """name -> {type, units, h_convention}, exactly as the converter writes /schema."""
        return {
            field.name: {
                "type": field.type,
                "units": field.units,
                "h_convention": field.h_convention,
            }
            for field in self._converter_schema().output_field_declarations()
        }

    def _declared_payload(self) -> dict[str, dict[str, str]]:
        """The package YAML's non-link declarations, with their effective h_convention."""
        import generate_properties as gp

        props = gp.load_property_package(
            self.package_root / "halo_properties.yaml", "halo_properties"
        )
        return {
            prop["name"]: {
                "type": prop["type"],
                "units": prop["units"],
                "h_convention": gp._effective_h_convention(prop),
            }
            for prop in props
            if prop["name"] not in LINK_ROLES
        }

    def _read_generated_catalog_metadata(self) -> dict[str, dict[str, str]]:
        """Parse this build's catalog_field_metadata.inc, keyed by dataset name.

        Fails if the file is missing or was not generated from this package's
        halo_properties.yaml. Either is a real defect here: the test runs only
        with this package selected, and the registered tier regenerates first.
        """
        regenerate = f"run 'make MODEL=<model> SIMULATION={self.spec.package} generate'"
        assert GENERATED_METADATA.exists(), f"{GENERATED_METADATA} is missing; {regenerate}"
        text = GENERATED_METADATA.read_text()
        if self.halo_properties not in text:
            sources = sorted(set(GENERATED_FROM_RE.findall(text))) or ["(no package named)"]
            raise AssertionError(
                f"{GENERATED_METADATA} was generated from {', '.join(sources)}, not "
                f"{self.halo_properties}; {regenerate}"
            )
        fields = {}
        for dataset, ftype, units, h_convention, _role, role_kind in CATALOG_FIELD_RE.findall(text):
            fields[dataset] = {
                "type": ftype,
                "units": units,
                "h_convention": h_convention,
                "role_kind": role_kind,
            }
        return fields

    # ---- tests -----------------------------------------------------------

    def test_converter_schema_is_the_pinned_route(self):
        """The vertical profile derives the pinned route, digest and mass units."""
        self._require_simulation()
        spec = self.spec
        schema = self._converter_schema()
        msg = f"profile source_format {schema.source_format!r} != {spec.source_format!r}"
        assert schema.source_format == spec.source_format, msg
        msg = f"column_mapping_sha256 {schema.digest} != pinned {spec.column_mapping_sha256}"
        assert schema.digest == spec.column_mapping_sha256, msg
        mass = self._expected_declarations()["M_Crit200"]
        msg = f"converter M_Crit200 units {mass['units']!r} != {spec.mass_units!r}"
        assert mass["units"] == spec.mass_units, msg

    def test_yaml_declarations_match_converter_schema(self):
        """The package's YAML declarations match the converter's /schema exactly."""
        self._require_simulation()
        import generate_properties as gp

        props = gp.load_property_package(
            self.package_root / "halo_properties.yaml", "halo_properties"
        )
        expected = self._expected_declarations()
        names = [prop["name"] for prop in props]
        assert len(names) == len(set(names)), f"duplicate declarations: {names}"
        for forbidden in sorted(READER_OWNED_FIELDS):
            msg = f"{forbidden} is reader-owned under R0-3(a) and must not be a declared property"
            assert forbidden not in names, msg
        missing_links = LINK_ROLES - set(names)
        assert not missing_links, f"link roles not declared: {sorted(missing_links)}"
        for prop in props:
            if prop["name"] in LINK_ROLES:
                msg = (
                    f"{prop['name']}: link role must be 'long long' under R0-2(a), "
                    f"got {prop['type']!r}"
                )
                assert prop["type"] == "long long", msg

        declared = self._declared_payload()
        msg = f"declared non-link fields {sorted(declared)} != /schema {sorted(expected)}"
        assert set(declared) == set(expected), msg
        for name, found in declared.items():
            _assert_matches(name, found, expected[name], "declared")

    def test_compiled_catalog_metadata_matches_converter_schema(self):
        """The build's own compiled catalog_field_metadata.inc matches /schema too.

        Stricter than the YAML view: it reads exactly what the reader's
        generated code was compiled against, not a re-derivation of it.
        """
        self._require_simulation()
        generated = self._read_generated_catalog_metadata()
        expected = self._expected_declarations()
        payload = set(generated) - LINK_ROLES
        msg = f"compiled non-link fields {sorted(payload)} != {sorted(expected)}"
        assert payload == set(expected), msg
        for dataset, entry in generated.items():
            if entry["role_kind"] == "tree_link":
                assert dataset in LINK_ROLES, f"{dataset}: unexpected tree_link role"
                msg = (
                    f"{dataset}: link role must be 'long long' under R0-2(a), got {entry['type']!r}"
                )
                assert entry["type"] == "long long", msg
                continue
            _assert_matches(dataset, entry, expected[dataset], "compiled")

    def _check_real_dataset(self, directory: Path, files: list[Path]) -> None:
        """Every real file carries the pinned digest and exactly the derived /schema."""
        import h5py

        expected = self._expected_declarations()
        populated = 0
        for path in files:
            with h5py.File(path, "r") as handle:
                header = handle["header"].attrs
                digest = _text(header["column_mapping_sha256"])
                msg = f"{path}: column_mapping_sha256 {digest} != {self.spec.column_mapping_sha256}"
                assert digest == self.spec.column_mapping_sha256, msg
                schema = _file_schema(handle)
                populated += int(int(header["n_halos"]) > 0)
            msg = f"{path}: /schema {sorted(schema)} != converter {sorted(expected)}"
            assert set(schema) == set(expected), msg
            for name, entry in schema.items():
                _assert_matches(name, entry, expected[name], path.name)
        assert populated > 0, f"{directory}: no populated snapshot file"
        print(f"  real dataset {directory}: {len(files)} file(s) match the converter /schema")

    def _check_fixture(self, relative: str, declared: dict[str, dict[str, str]]) -> None:
        """One committed fixture satisfies the one-way rule on every populated file."""
        import h5py

        directory = REPO_ROOT / relative
        files = sorted(directory.glob("snapshot_*.h5")) if directory.is_dir() else []
        assert files, f"committed fixture dataset {directory} is missing or holds no snapshot files"
        populated = 0
        for path in files:
            with h5py.File(path, "r") as handle:
                if int(handle["header"].attrs["n_halos"]) <= 0:
                    continue
                schema = _file_schema(handle)
            populated += 1
            missing = sorted(set(declared) - set(schema))
            assert not missing, f"{path}: declared payload field(s) {missing} absent from /schema"
            for name, found in declared.items():
                _assert_matches(name, found, schema[name], f"{relative}/{path.name} declared")
        assert populated > 0, f"{directory}: no populated snapshot file"
        print(f"  fixture {relative}: {populated} populated file(s) carry every declaration")

    def test_dataset_schemas_match_converter_schema(self):
        """The real dataset's /schema is the converter's; every fixture carries the declarations.

        The real data is machine-local and gitignored, so its absence alone is
        not a failure (the package's parity gate fails rather than skipping in
        that situation); a named fixture's absence is.
        """
        self._require_simulation()
        declared = self._declared_payload()
        for relative in self.spec.fixture_datasets:
            self._check_fixture(relative, declared)

        directory = self.package_root / "snapshots"
        files = sorted(directory.glob("snapshot_*.h5")) if directory.is_dir() else []
        if files:
            self._check_real_dataset(directory, files)
        elif self.spec.fixture_datasets:
            print(f"  no converted dataset behind {directory}; fixtures checked only")
        else:
            raise TestSkipped(f"no converted dataset behind {directory}")


def run_schema_conformance(spec: SchemaPackage, title: str) -> int:
    """Run the four conformance tests for ``spec``; return the suite exit code."""
    return run_test_suite(SchemaConformance(spec).tests(), title)
