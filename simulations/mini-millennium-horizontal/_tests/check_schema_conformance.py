#!/usr/bin/env python3
"""Package-local conformance check for simulations/mini-millennium-horizontal.

Compares this package's compiled halo_properties.yaml declarations against a
committed version 3 fixture's /schema (tests/data/horizontal_v3/dataset/,
tests/data/horizontal_v3/regenerate.sh). Every non-link payload field this
package declares must match the fixture's /schema type, units and effective
h_convention exactly (the same lookup scripts/generate_properties.py applies
when a field omits an explicit h_convention). The five link roles are
excluded from that comparison because /schema never declares them (Gate
R0-2); SourceHaloID and the three target-snapshot columns must not appear as
declared catalog properties at all (Gate R0-3(a) -- they are reader-owned
slab arrays).

Usage: mimic_venv/bin/python simulations/mini-millennium-horizontal/_tests/check_schema_conformance.py
Exit code: 0 on success, 1 on any mismatch.
"""

import sys
from pathlib import Path

import h5py

REPO_ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO_ROOT / "scripts"))

import generate_properties as gp  # noqa: E402

PACKAGE_ROOT = Path(__file__).resolve().parents[1]
FIXTURE_DIR = REPO_ROOT / "tests" / "data" / "horizontal_v3" / "dataset"

LINK_ROLES = {
    "Descendant",
    "FirstProgenitor",
    "NextProgenitor",
    "FirstHaloInFOFgroup",
    "NextHaloInFOFgroup",
}

# Reader-owned under R0-3(a): format metadata, never declared catalog properties.
READER_OWNED_FIELDS = {
    "SourceHaloID",
    "DescendantSnapshot",
    "FirstProgenitorSnapshot",
    "NextProgenitorSnapshot",
}


def declared_properties():
    return gp.load_property_package(PACKAGE_ROOT / "halo_properties.yaml", "halo_properties")


def fixture_schema_file():
    for candidate in sorted(FIXTURE_DIR.glob("snapshot_*.h5")):
        with h5py.File(candidate, "r") as f:
            if int(f["header"].attrs.get("n_halos", 0)) > 0:
                return candidate
    raise AssertionError(f"no populated snapshot file under {FIXTURE_DIR}")


def read_schema(path):
    schema = {}
    with h5py.File(path, "r") as f:
        for name, group in f["schema"].items():
            schema[name] = {
                "type": group.attrs["type"],
                "units": group.attrs["units"],
                "h_convention": group.attrs["h_convention"],
            }
    return schema


def main():
    props = declared_properties()
    schema = read_schema(fixture_schema_file())

    names = [p["name"] for p in props]
    if len(names) != len(set(names)):
        raise AssertionError(f"duplicate declarations: {names}")

    for forbidden in sorted(READER_OWNED_FIELDS):
        if forbidden in names:
            raise AssertionError(
                f"{forbidden} is reader-owned under R0-3(a) and must not be a "
                "declared catalog property"
            )

    checked = 0
    for prop in props:
        name = prop["name"]
        if name in LINK_ROLES:
            if prop["type"] != "long long":
                raise AssertionError(
                    f"{name}: link role must be 'long long' under R0-2(a), got {prop['type']!r}"
                )
            continue
        if name not in schema:
            raise AssertionError(f"{name}: declared but the fixture's /schema does not include it")
        entry = schema[name]
        if prop["type"] != entry["type"]:
            raise AssertionError(f"{name}: type {prop['type']!r} != /schema {entry['type']!r}")
        if prop["units"] != entry["units"]:
            raise AssertionError(f"{name}: units {prop['units']!r} != /schema {entry['units']!r}")
        effective_h = gp._effective_h_convention(prop)
        if effective_h != entry["h_convention"]:
            raise AssertionError(
                f"{name}: h_convention {effective_h!r} != /schema {entry['h_convention']!r}"
            )
        checked += 1

    print(
        f"OK: {checked} payload field(s) match the fixture's /schema; "
        f"{len(LINK_ROLES)} link role(s) declared long long; "
        f"{len(READER_OWNED_FIELDS)} reader-owned field(s) confirmed undeclared"
    )


if __name__ == "__main__":
    main()
