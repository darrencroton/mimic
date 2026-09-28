#!/usr/bin/env python3
"""
mini-Millennium Horizontal v3 Schema Conformance Test

Validates: this package's compiled halo_properties.yaml declarations agree
with a real version 3 dataset's `/schema` -- checked two ways, the YAML-via-
generator view (the same effective-h_convention lookup
scripts/generate_properties.py applies) and, when present, the generated
catalog_field_metadata.inc this package's own build compiled. The five link
roles are declared `long long` (Gate R0-2(a)) and excluded from the /schema
comparison because /schema never declares them -- they are checked against
the v3 format's fixed table instead. SourceHaloID, the three target-snapshot
columns and the ForestIndex/HaloRankInForest identity arrays must not appear
as declared catalog properties at all (Gate R0-3(a) and the identity-array
precedent -- they are reader-owned format-table arrays).

The dataset compared against is the committed version 3 reader fixture
(tests/data/horizontal_v3/dataset/, tests/data/horizontal_v3/regenerate.sh),
not the real converted mini-Millennium dataset -- the latter is a machine-
local, gitignored, multi-hundred-megabyte artifact this test cannot depend
on being present.

Skips automatically when:
  - SIMULATION != mini-millennium-horizontal (wrong compiled package)

Run with:
  python3 simulations/mini-millennium-horizontal/_tests/integration/test_schema_conformance.py
or (registered):
  make MODEL=halos-only SIMULATION=mini-millennium-horizontal tests-integration
"""

import re
import sys
from pathlib import Path


def find_repo_root(start):
    for parent in [start, *start.parents]:
        if (parent / "tests").is_dir() and (parent / "src").is_dir():
            return parent
    raise RuntimeError(f"Could not find repository root from {start}")


REPO_ROOT = find_repo_root(Path(__file__).resolve())
PACKAGE_ROOT = REPO_ROOT / "simulations" / "mini-millennium-horizontal"
FIXTURE_DIR = REPO_ROOT / "tests" / "data" / "horizontal_v3" / "dataset"
GENERATED_METADATA = REPO_ROOT / "src" / "include" / "generated" / "catalog_field_metadata.inc"
PACKAGE_HALO_PROPERTIES = "simulations/mini-millennium-horizontal/halo_properties.yaml"
CONVERTER_PROFILE = REPO_ROOT / "simulations" / "mini-millennium" / "converter_columns.yaml"

sys.path.insert(0, str(REPO_ROOT / "tests"))
sys.path.insert(0, str(REPO_ROOT / "scripts"))

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

# The one deliberate R0-8(a) extra in the committed v3 fixture: an undeclared
# /schema field that must be validated for internal consistency and never
# materialised (tests/data/horizontal_v3/source/profile.yaml). It is not part
# of this package's own declarations.
FIXTURE_ONLY_EXTRA_FIELDS = {"SubHalfMass"}

CATALOG_FIELD_RE = re.compile(
    r'CATALOG_FIELD\(\s*\w+\s*,\s*"([^"]*)"\s*,\s*"([^"]*)"\s*,\s*"([^"]*)"\s*,'
    r'\s*"([^"]*)"\s*,\s*"([^"]*)"\s*,\s*"([^"]*)"\s*\)'
)


def _expected_payload_fields():
    """Non-link field names the mini-Millennium converter profile selects.

    Source of the expected declared-name set:
    simulations/mini-millennium/converter_columns.yaml's required_columns
    keys, minus the five link roles (the profile selects those too, but
    /schema never declares them).
    """
    import yaml

    with open(CONVERTER_PROFILE, encoding="utf-8") as f:
        profile = yaml.safe_load(f)
    return set(profile["required_columns"]) - LINK_ROLES


def _require_simulation():
    sim = compiled_simulation()
    if sim != "mini-millennium-horizontal":
        raise TestSkipped(f"compiled simulation is {sim!r}, not mini-millennium-horizontal")


def _declared_properties():
    import generate_properties as gp

    return gp.load_property_package(PACKAGE_ROOT / "halo_properties.yaml", "halo_properties")


def _fixture_schema_file():
    import h5py

    for candidate in sorted(FIXTURE_DIR.glob("snapshot_*.h5")):
        with h5py.File(candidate, "r") as f:
            if int(f["header"].attrs.get("n_halos", 0)) > 0:
                return candidate
    raise AssertionError(f"no populated snapshot file under {FIXTURE_DIR}")


def _read_fixture_schema():
    import h5py

    schema = {}
    with h5py.File(_fixture_schema_file(), "r") as f:
        for name, group in f["schema"].items():
            schema[name] = {
                "type": group.attrs["type"],
                "units": group.attrs["units"],
                "h_convention": group.attrs["h_convention"],
            }
    return schema


def _read_generated_catalog_metadata():
    """Parse this build's catalog_field_metadata.inc, keyed by dataset name.

    Returns None if the file is missing or was not generated from this
    package's halo_properties.yaml (e.g. a stale build for another
    SIMULATION) -- callers fall back to the YAML-via-generator view alone.
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


def test_yaml_declarations_match_fixture_schema():
    """The package's YAML declarations match the fixture's /schema exactly."""
    _require_simulation()

    import generate_properties as gp

    props = _declared_properties()
    schema = _read_fixture_schema()

    names = [p["name"] for p in props]
    assert len(names) == len(set(names)), f"duplicate declarations: {names}"

    for forbidden in sorted(READER_OWNED_FIELDS):
        msg = (
            f"{forbidden} is reader-owned under R0-3(a) and must not be a declared catalog property"
        )
        assert forbidden not in names, msg

    payload_names = set(names) - LINK_ROLES
    expected_payload = _expected_payload_fields()
    msg = (
        f"declared non-link fields {sorted(payload_names)} != the converter "
        f"profile's selection {sorted(expected_payload)}"
    )
    assert payload_names == expected_payload, msg
    msg = (
        f"fixture /schema {sorted(schema)} != declared payload fields "
        f"{sorted(payload_names)} plus the deliberate R0-8(a) extra "
        f"{sorted(FIXTURE_ONLY_EXTRA_FIELDS)}"
    )
    assert set(schema) == payload_names | FIXTURE_ONLY_EXTRA_FIELDS, msg

    checked = 0
    for prop in props:
        name = prop["name"]
        if name in LINK_ROLES:
            msg = f"{name}: link role must be 'long long' under R0-2(a), got {prop['type']!r}"
            assert prop["type"] == "long long", msg
            continue
        entry = schema[name]
        msg = f"{name}: type {prop['type']!r} != /schema {entry['type']!r}"
        assert prop["type"] == entry["type"], msg
        msg = f"{name}: units {prop['units']!r} != /schema {entry['units']!r}"
        assert prop["units"] == entry["units"], msg
        effective_h = gp._effective_h_convention(prop)
        msg = f"{name}: h_convention {effective_h!r} != /schema {entry['h_convention']!r}"
        assert effective_h == entry["h_convention"], msg
        checked += 1

    msg = f"expected {len(expected_payload)} non-link payload fields checked, got {checked}"
    assert checked == len(expected_payload), msg


def test_compiled_catalog_metadata_matches_fixture_schema():
    """The build's own compiled catalog_field_metadata.inc matches /schema too.

    This is a stricter check than the YAML view: it reads exactly what the
    reader's generated code was compiled against, not a re-derivation of it.
    """
    _require_simulation()

    generated = _read_generated_catalog_metadata()
    if generated is None:
        return (
            f"{GENERATED_METADATA} is missing or was not generated from "
            f"{PACKAGE_HALO_PROPERTIES} -- run 'make MODEL=<model> "
            "SIMULATION=mini-millennium-horizontal generate' first; "
            "falling back to the YAML-via-generator view alone "
            "(test_yaml_declarations_match_fixture_schema), which uses the "
            "same effective-h_convention lookup the generator applies"
        )

    schema = _read_fixture_schema()
    expected_payload = _expected_payload_fields()

    compiled_names = set(generated)
    compiled_payload_names = compiled_names - LINK_ROLES
    msg = (
        f"compiled non-link fields {sorted(compiled_payload_names)} != the "
        f"converter profile's selection {sorted(expected_payload)}"
    )
    assert compiled_payload_names == expected_payload, msg

    checked = 0
    for dataset, entry in generated.items():
        if entry["role_kind"] == "tree_link":
            assert dataset in LINK_ROLES, f"{dataset}: unexpected tree_link role"
            msg = f"{dataset}: link role must be 'long long' under R0-2(a), got {entry['type']!r}"
            assert entry["type"] == "long long", msg
            continue
        msg = f"{dataset}: compiled but the fixture's /schema does not include it"
        assert dataset in schema, msg
        fixture_entry = schema[dataset]
        msg = f"{dataset}: compiled type {entry['type']!r} != /schema {fixture_entry['type']!r}"
        assert entry["type"] == fixture_entry["type"], msg
        msg = f"{dataset}: compiled units {entry['units']!r} != /schema {fixture_entry['units']!r}"
        assert entry["units"] == fixture_entry["units"], msg
        msg = (
            f"{dataset}: compiled h_convention {entry['h_convention']!r} != "
            f"/schema {fixture_entry['h_convention']!r}"
        )
        assert entry["h_convention"] == fixture_entry["h_convention"], msg
        checked += 1

    msg = f"expected {len(expected_payload)} non-link payload fields checked, got {checked}"
    assert checked == len(expected_payload), msg


if __name__ == "__main__":
    sys.exit(
        run_test_suite(
            [
                test_yaml_declarations_match_fixture_schema,
                test_compiled_catalog_metadata_matches_fixture_schema,
            ],
            "mini-Millennium Horizontal v3 schema conformance (test_schema_conformance.py)",
        )
    )
