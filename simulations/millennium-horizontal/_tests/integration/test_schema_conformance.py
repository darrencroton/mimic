#!/usr/bin/env python3
"""
millennium-horizontal version 3 schema conformance.

Validates that this package's halo_properties.yaml declares exactly the `/schema`
the converter writes for its route (millennium's converter profile, `lhalo_binary`),
in the YAML, in the compiled catalog_field_metadata.inc and in the real converted
dataset behind `snapshots/` when present. The four tests live in
tests/framework/schema_conformance.py; this file only pins the route. Every test
skips unless SIMULATION=millennium-horizontal.

Run with:
  make MODEL=halos-only SIMULATION=millennium-horizontal generate
  MODEL=halos-only SIMULATION=millennium-horizontal \\
      python3 simulations/millennium-horizontal/_tests/integration/test_schema_conformance.py
or (registered):
  make MODEL=halos-only SIMULATION=millennium-horizontal tests-integration
"""

import sys
from pathlib import Path

REPO_ROOT = next(
    p for p in Path(__file__).resolve().parents if (p / "tests" / "framework").is_dir()
)
sys.path.insert(0, str(REPO_ROOT / "tests"))

from framework.schema_conformance import SchemaPackage, run_schema_conformance  # noqa: E402

SPEC = SchemaPackage(
    package="millennium-horizontal",
    vertical_package="millennium",
    source_format="lhalo_binary",
    column_mapping_sha256="5a74a2e07eca5f3a5fef5e02b75e15c821f94370606d7608b25f082f9f5654b1",
    mass_units="1e10 Msun/h",
)


if __name__ == "__main__":
    sys.exit(
        run_schema_conformance(
            SPEC, "millennium-horizontal v3 schema conformance (test_schema_conformance.py)"
        )
    )
