#!/usr/bin/env python3
"""
shin-uchuu version 3 schema conformance.

Validates that this package's halo_properties.yaml declares exactly the `/schema`
the converter writes for its route (shin-uchuu-ascii's converter profile,
`consistent_trees_ascii`), in the YAML, in the compiled catalog_field_metadata.inc
and in the real converted dataset behind `snapshots/` when present. The four tests
live in tests/framework/schema_conformance.py; this file pins the route and skips the
real-dataset half while the resolved dataset is version 2, which carries neither
`/schema` nor `column_mapping_sha256`. The committed fixture under _tests/data/ is
version 2 as well and is not named here. Every test skips unless SIMULATION=shin-uchuu.

Run with:
  make MODEL=halos-only SIMULATION=shin-uchuu generate
  MODEL=halos-only SIMULATION=shin-uchuu \\
      python3 simulations/shin-uchuu/_tests/integration/test_schema_conformance.py
or (registered):
  make MODEL=halos-only SIMULATION=shin-uchuu tests-integration
"""

import sys
from pathlib import Path

REPO_ROOT = next(
    p for p in Path(__file__).resolve().parents if (p / "tests" / "framework").is_dir()
)
sys.path.insert(0, str(REPO_ROOT / "tests"))

from framework.markers import TestSkipped  # noqa: E402
from framework.runner import run_test_suite  # noqa: E402
from framework.schema_conformance import SchemaConformance, SchemaPackage  # noqa: E402

SPEC = SchemaPackage(
    package="shin-uchuu",
    vertical_package="shin-uchuu-ascii",
    source_format="consistent_trees_ascii",
    column_mapping_sha256="727d13f529fa80305f261b933612b6087aa52ade5dde7899bb18f8f4450f8f6d",
    mass_units="Msun/h",
)


class _VersionAwareConformance(SchemaConformance):
    """Skips the real-dataset check while the dataset behind `snapshots/` is version 2."""

    def _check_real_dataset(self, directory, files):
        import h5py

        with h5py.File(files[0], "r") as handle:
            version = int(handle["header"].attrs["format_version"])
        if version < 3:
            raise TestSkipped(
                f"{directory} is a version {version} dataset: no /schema or column_mapping_sha256"
            )
        super()._check_real_dataset(directory, files)


if __name__ == "__main__":
    sys.exit(
        run_test_suite(
            _VersionAwareConformance(SPEC).tests(),
            "shin-uchuu v3 schema conformance (test_schema_conformance.py)",
        )
    )
