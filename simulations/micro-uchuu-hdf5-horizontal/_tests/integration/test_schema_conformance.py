#!/usr/bin/env python3
"""
micro-uchuu-hdf5-horizontal version 3 schema conformance.

Validates that this package's halo_properties.yaml declares exactly the `/schema`
the converter writes for its route (micro-uchuu-hdf5's converter profile,
`consistent_trees_hdf5`), in the YAML, in the compiled catalog_field_metadata.inc
and in the real converted dataset behind `snapshots/` when present. The four tests
live in tests/framework/schema_conformance.py; this file only pins the route. Every
test skips unless SIMULATION=micro-uchuu-hdf5-horizontal.

Run with:
  make MODEL=halos-only SIMULATION=micro-uchuu-hdf5-horizontal generate
  MODEL=halos-only SIMULATION=micro-uchuu-hdf5-horizontal \\
      python3 simulations/micro-uchuu-hdf5-horizontal/_tests/integration/test_schema_conformance.py
or (registered):
  make MODEL=halos-only SIMULATION=micro-uchuu-hdf5-horizontal tests-integration
"""

import sys
from pathlib import Path

REPO_ROOT = next(
    p for p in Path(__file__).resolve().parents if (p / "tests" / "framework").is_dir()
)
sys.path.insert(0, str(REPO_ROOT / "tests"))

from framework.schema_conformance import SchemaPackage, run_schema_conformance  # noqa: E402

SPEC = SchemaPackage(
    package="micro-uchuu-hdf5-horizontal",
    vertical_package="micro-uchuu-hdf5",
    source_format="consistent_trees_hdf5",
    column_mapping_sha256="241f277cf059378077c19c16c4584ff171a8f2b9fba55e395a7808e6033bcbce",
    mass_units="Msun/h",
)


if __name__ == "__main__":
    sys.exit(
        run_schema_conformance(
            SPEC, "micro-uchuu-hdf5-horizontal v3 schema conformance (test_schema_conformance.py)"
        )
    )
