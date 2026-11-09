#!/usr/bin/env python3
"""
shin-uchuu-ascii-horizontal version 3 schema conformance.

Validates that this package's halo_properties.yaml declares exactly the `/schema` the
converter writes for its route (the converter's shipped default `consistent_trees_ascii`
profile), in the YAML, in the compiled catalog_field_metadata.inc and in the real
converted dataset behind `snapshots/` when present. The four tests live in
tests/framework/schema_conformance.py; this file only pins the route. Every test skips
unless SIMULATION=shin-uchuu-ascii-horizontal. The committed worked_ascii fixture under
_tests/data/ is not named here: the generic test tiers run on it.

Run with:
  make MODEL=halos-only SIMULATION=shin-uchuu-ascii-horizontal generate
  MODEL=halos-only SIMULATION=shin-uchuu-ascii-horizontal \\
      python3 simulations/shin-uchuu-ascii-horizontal/_tests/integration/test_schema_conformance.py
or (registered):
  make MODEL=halos-only SIMULATION=shin-uchuu-ascii-horizontal tests-integration
"""

import sys
from pathlib import Path

REPO_ROOT = next(
    p for p in Path(__file__).resolve().parents if (p / "tests" / "framework").is_dir()
)
sys.path.insert(0, str(REPO_ROOT / "tests"))

from framework.schema_conformance import SchemaPackage, run_schema_conformance  # noqa: E402

SPEC = SchemaPackage(
    package="shin-uchuu-ascii-horizontal",
    vertical_package=None,
    source_format="consistent_trees_ascii",
    column_mapping_sha256="727d13f529fa80305f261b933612b6087aa52ade5dde7899bb18f8f4450f8f6d",
    mass_units="Msun/h",
    column_map="convert/mimic-convert/profiles/consistent_trees_ascii.yaml",
)


if __name__ == "__main__":
    sys.exit(
        run_schema_conformance(
            SPEC, "shin-uchuu-ascii-horizontal v3 schema conformance (test_schema_conformance.py)"
        )
    )
