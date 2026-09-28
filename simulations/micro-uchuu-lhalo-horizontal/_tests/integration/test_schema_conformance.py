#!/usr/bin/env python3
"""
Horizontal v3 Package Schema Conformance Test

Validates: this package's halo_properties.yaml declares exactly the `/schema`
the converter writes for its route, checked three ways:

1. against the converter's own `/schema` derivation for the vertical package's
   converter profile (scripts/convert/column_schema.py, the code that writes
   every file's `/schema` and stamps its column_mapping_sha256), which needs no
   dataset -- and that derivation's digest is the one the package's data and
   parity gate pin;
2. against the generated catalog_field_metadata.inc this package's own build
   compiled, when present;
3. against the real converted dataset's `/schema` behind this package's
   `snapshots/` symlink, when present (skipped, never faked, when it is not).

The five link roles are declared `long long` (Gate R0-2(a)) and excluded from
the `/schema` comparison because `/schema` never declares them -- they are
checked against the v3 format's fixed table instead. SourceHaloID, the three
target-snapshot columns and the ForestIndex/HaloRankInForest identity arrays
must not appear as declared catalog properties at all (Gate R0-3(a) and the
identity-array precedent -- they are reader-owned format-table arrays).

This file is one of four near-identical Slice 7 package tests; only the
"Package parameters" block differs between them.

Skips automatically when:
  - SIMULATION != this package (wrong compiled package)

Run with:
  SIMULATION=<package> python3 simulations/<package>/_tests/integration/test_schema_conformance.py
or (registered):
  make MODEL=halos-only SIMULATION=<package> tests-integration
"""

import re
import sys
from pathlib import Path

# ==========================================================================
# Package parameters -- the only block that differs between the Slice 7 tests
# ==========================================================================

PACKAGE = "micro-uchuu-lhalo-horizontal"
VERTICAL_PACKAGE = "micro-uchuu"
SOURCE_FORMAT = "lhalo_binary"
EXPECTED_COLUMN_MAPPING_SHA256 = "5a74a2e07eca5f3a5fef5e02b75e15c821f94370606d7608b25f082f9f5654b1"
#: The mass role's native storage for this route, asserted explicitly because a
#: wrong-by-1e10 mass is the defect a per-source-format package exists to stop.
EXPECTED_MASS_UNITS = "1e10 Msun/h"

# ==========================================================================
# End of package parameters
# ==========================================================================


def find_repo_root(start):
    for parent in [start, *start.parents]:
        if (parent / "tests").is_dir() and (parent / "src").is_dir():
            return parent
    raise RuntimeError(f"Could not find repository root from {start}")


REPO_ROOT = find_repo_root(Path(__file__).resolve())
PACKAGE_ROOT = REPO_ROOT / "simulations" / PACKAGE
VERTICAL_ROOT = REPO_ROOT / "simulations" / VERTICAL_PACKAGE
GENERATED_METADATA = REPO_ROOT / "src" / "include" / "generated" / "catalog_field_metadata.inc"
PACKAGE_HALO_PROPERTIES = f"simulations/{PACKAGE}/halo_properties.yaml"

sys.path.insert(0, str(REPO_ROOT / "tests"))
sys.path.insert(0, str(REPO_ROOT / "scripts"))
sys.path.insert(0, str(REPO_ROOT / "scripts" / "convert"))

from framework import TestSkipped, compiled_simulation, run_test_suite  # noqa: E402

LINK_ROLES = {
    "Descendant",
    "FirstProgenitor",
    "NextProgenitor",
    "FirstHaloInFOFgroup",
    "NextHaloInFOFgroup",
}

# Reader-owned under R0-3(a) and the ForestIndex/HaloRankInForest identity-array
# precedent: format-table metadata, never declared catalog properties.
READER_OWNED_FIELDS = {
    "SourceHaloID",
    "DescendantSnapshot",
    "FirstProgenitorSnapshot",
    "NextProgenitorSnapshot",
    "ForestIndex",
    "HaloRankInForest",
}

CATALOG_FIELD_RE = re.compile(
    r'CATALOG_FIELD\(\s*\w+\s*,\s*"([^"]*)"\s*,\s*"([^"]*)"\s*,\s*"([^"]*)"\s*,'
    r'\s*"([^"]*)"\s*,\s*"([^"]*)"\s*,\s*"([^"]*)"\s*\)'
)


def _require_simulation():
    sim = compiled_simulation()
    if sim != PACKAGE:
        raise TestSkipped(f"compiled simulation is {sim!r}, not {PACKAGE}")


def _declared_properties():
    import generate_properties as gp

    return gp.load_property_package(PACKAGE_ROOT / "halo_properties.yaml", "halo_properties")


def _converter_schema():
    """The converter's canonical schema for the vertical package's profile."""
    import column_schema as cs

    column_map = cs.load_column_map(VERTICAL_ROOT / "converter_columns.yaml")
    source_properties = None
    if column_map.source_format == "lhalo_binary":
        source_properties = cs.load_source_properties(VERTICAL_ROOT / "halo_properties.yaml")
    return cs.build_schema(column_map, source_properties)


def _expected_declarations():
    """name -> {type, units, h_convention}, exactly as the converter writes /schema."""
    return {
        field.name: {
            "type": field.type,
            "units": field.units,
            "h_convention": field.h_convention,
        }
        for field in _converter_schema().output_field_declarations()
    }


def _read_generated_catalog_metadata():
    """Parse this build's catalog_field_metadata.inc, keyed by dataset name.

    Returns None if the file is missing or was not generated from this
    package's halo_properties.yaml (e.g. a stale build for another
    SIMULATION).
    """
    if not GENERATED_METADATA.exists():
        return None
    text = GENERATED_METADATA.read_text()
    if PACKAGE_HALO_PROPERTIES not in text:
        return None
    fields = {}
    for dataset, ftype, units, h_convention, _core_role, role_kind in CATALOG_FIELD_RE.findall(
        text
    ):
        fields[dataset] = {
            "type": ftype,
            "units": units,
            "h_convention": h_convention,
            "role_kind": role_kind,
        }
    return fields


def _assert_matches(name, found, expected, what):
    for key in ("type", "units", "h_convention"):
        msg = f"{name}: {what} {key} {found[key]!r} != /schema {expected[key]!r}"
        assert found[key] == expected[key], msg


def test_converter_schema_is_the_pinned_route():
    """The vertical profile derives the pinned route, digest and mass units."""
    _require_simulation()

    schema = _converter_schema()
    msg = f"profile source_format {schema.source_format!r} != {SOURCE_FORMAT!r}"
    assert schema.source_format == SOURCE_FORMAT, msg
    msg = f"column_mapping_sha256 {schema.digest} != pinned {EXPECTED_COLUMN_MAPPING_SHA256}"
    assert schema.digest == EXPECTED_COLUMN_MAPPING_SHA256, msg
    mass = _expected_declarations()["M_Crit200"]
    msg = f"converter M_Crit200 units {mass['units']!r} != {EXPECTED_MASS_UNITS!r}"
    assert mass["units"] == EXPECTED_MASS_UNITS, msg


def test_yaml_declarations_match_converter_schema():
    """The package's YAML declarations match the converter's /schema exactly."""
    _require_simulation()

    import generate_properties as gp

    props = _declared_properties()
    expected = _expected_declarations()

    names = [p["name"] for p in props]
    assert len(names) == len(set(names)), f"duplicate declarations: {names}"
    for forbidden in sorted(READER_OWNED_FIELDS):
        msg = (
            f"{forbidden} is reader-owned under R0-3(a) and must not be a declared catalog property"
        )
        assert forbidden not in names, msg
    missing_links = LINK_ROLES - set(names)
    assert not missing_links, f"link roles not declared: {sorted(missing_links)}"

    payload_names = set(names) - LINK_ROLES
    msg = f"declared non-link fields {sorted(payload_names)} != /schema {sorted(expected)}"
    assert payload_names == set(expected), msg

    for prop in props:
        name = prop["name"]
        if name in LINK_ROLES:
            msg = f"{name}: link role must be 'long long' under R0-2(a), got {prop['type']!r}"
            assert prop["type"] == "long long", msg
            continue
        found = {
            "type": prop["type"],
            "units": prop["units"],
            "h_convention": gp._effective_h_convention(prop),
        }
        _assert_matches(name, found, expected[name], "declared")


def test_compiled_catalog_metadata_matches_converter_schema():
    """The build's own compiled catalog_field_metadata.inc matches /schema too.

    Stricter than the YAML view: it reads exactly what the reader's generated
    code was compiled against, not a re-derivation of it.
    """
    _require_simulation()

    generated = _read_generated_catalog_metadata()
    if generated is None:
        return (
            f"{GENERATED_METADATA} is missing or was not generated from "
            f"{PACKAGE_HALO_PROPERTIES} -- run 'make MODEL=<model> SIMULATION={PACKAGE} "
            "generate' first; the YAML-via-generator view "
            "(test_yaml_declarations_match_converter_schema) still ran"
        )

    expected = _expected_declarations()
    msg = f"compiled non-link fields {sorted(set(generated) - LINK_ROLES)} != {sorted(expected)}"
    assert set(generated) - LINK_ROLES == set(expected), msg
    for dataset, entry in generated.items():
        if entry["role_kind"] == "tree_link":
            assert dataset in LINK_ROLES, f"{dataset}: unexpected tree_link role"
            msg = f"{dataset}: link role must be 'long long' under R0-2(a), got {entry['type']!r}"
            assert entry["type"] == "long long", msg
            continue
        _assert_matches(dataset, entry, expected[dataset], "compiled")


def test_real_dataset_schema_matches_converter_schema():
    """The real converted dataset's /schema is the converter's, in every file.

    Skipped when this machine has no dataset behind `snapshots/`: the real data
    is machine-local and gitignored. The package's parity gate fails rather
    than skipping in the same situation.
    """
    _require_simulation()

    import h5py

    directory = PACKAGE_ROOT / "snapshots"
    files = sorted(directory.glob("snapshot_*.h5")) if directory.is_dir() else []
    if not files:
        raise TestSkipped(f"no converted dataset behind {directory}")

    expected = _expected_declarations()
    populated = 0
    for path in files:
        with h5py.File(path, "r") as handle:
            header = handle["header"].attrs
            digest = header["column_mapping_sha256"]
            digest = digest.decode() if isinstance(digest, bytes) else str(digest)
            msg = f"{path}: column_mapping_sha256 {digest} != {EXPECTED_COLUMN_MAPPING_SHA256}"
            assert digest == EXPECTED_COLUMN_MAPPING_SHA256, msg
            schema = {}
            for name, group in handle["schema"].items():
                schema[name] = {
                    key: (value.decode() if isinstance(value, bytes) else str(value))
                    for key, value in group.attrs.items()
                    if key in ("type", "units", "h_convention")
                }
            populated += int(int(header["n_halos"]) > 0)
        msg = f"{path}: /schema {sorted(schema)} != converter {sorted(expected)}"
        assert set(schema) == set(expected), msg
        for name, entry in schema.items():
            _assert_matches(name, entry, expected[name], f"{path.name}")
    assert populated > 0, f"{directory}: no populated snapshot file"


if __name__ == "__main__":
    sys.exit(
        run_test_suite(
            [
                test_converter_schema_is_the_pinned_route,
                test_yaml_declarations_match_converter_schema,
                test_compiled_catalog_metadata_matches_converter_schema,
                test_real_dataset_schema_matches_converter_schema,
            ],
            f"{PACKAGE} v3 schema conformance (test_schema_conformance.py)",
        )
    )
