#!/usr/bin/env python3
"""
mini-millennium-horizontal version 3 schema conformance.

Validates that this package's halo_properties.yaml declares exactly the `/schema`
the converter writes for its route (mini-millennium's converter profile,
`lhalo_binary`), in the YAML, in the compiled catalog_field_metadata.inc, in the
real converted dataset when present, and in every committed version 3 fixture
named below, each of which must exist. The four tests live in
tests/framework/schema_conformance.py; this file only pins the route. Every test
skips unless SIMULATION=mini-millennium-horizontal.

Run with:
  make MODEL=halos-only SIMULATION=mini-millennium-horizontal generate
  MODEL=halos-only SIMULATION=mini-millennium-horizontal \\
      python3 simulations/mini-millennium-horizontal/_tests/integration/test_schema_conformance.py
or (registered):
  make MODEL=halos-only SIMULATION=mini-millennium-horizontal tests-integration
"""

import sys
from pathlib import Path

REPO_ROOT = next(
    p for p in Path(__file__).resolve().parents if (p / "tests" / "framework").is_dir()
)
sys.path.insert(0, str(REPO_ROOT / "tests"))

from framework.schema_conformance import SchemaPackage, run_schema_conformance  # noqa: E402

SPEC = SchemaPackage(
    package="mini-millennium-horizontal",
    vertical_package="mini-millennium",
    source_format="lhalo_binary",
    column_mapping_sha256="5a74a2e07eca5f3a5fef5e02b75e15c821f94370606d7608b25f082f9f5654b1",
    mass_units="1e10 Msun/h",
    fixture_datasets=(
        "tests/data/horizontal_v3/dataset",
        "simulations/mini-millennium-horizontal/_tests/data/worked_graph",
        "simulations/mini-millennium-horizontal/_tests/data/three_snapshot_chain",
        "simulations/mini-millennium-horizontal/_tests/data/adjacent",
    ),
)


if __name__ == "__main__":
    sys.exit(
        run_schema_conformance(
            SPEC, "mini-millennium-horizontal v3 schema conformance (test_schema_conformance.py)"
        )
    )
