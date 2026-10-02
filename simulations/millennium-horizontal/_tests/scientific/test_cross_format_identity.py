#!/usr/bin/env python3
"""
Version 3 route parity gate: millennium against its own horizontal conversion.

Certifies that the same real merger trees -- the whole simulation, trees_063.0-.511,
all 512 files the package declares -- read through the vertical driver
(``millennium``, the ``lhalo_binary`` reader) and through the horizontal driver
(``millennium-horizontal``, a version 3 conversion of exactly those files) produce,
for every output snapshot, identical ``UniqueGalaxyID`` sets and per-id byte-
identical fields under ``halos-only``, with fixed and dynamic timesteps. Parity is
promised against the package's own source format only.

The shipped vertical run file ``halos-only_millennium.yaml`` reads only files 0-15 so
that it stays quick to run, so the gate runs a scratch copy of it with the input range
set to 0-511 and nothing else changed, and checks that both runs record that range.

The stages and every check live in ``tests/framework/parity_gate.py``; this file
only pins what the package's data must be. It is a manual, dataset-present
operation, registered only when this package is selected::

    make MODEL=halos-only SIMULATION=millennium-horizontal tests-scientific

A missing dataset, a dataset that is not the pinned conversion, or a leg that
does not run fails the gate: it never skips.
"""

import sys
from pathlib import Path

REPO_ROOT = next(
    p for p in Path(__file__).resolve().parents if (p / "tests" / "framework").is_dir()
)
sys.path.insert(0, str(REPO_ROOT / "tests"))

from framework.parity_gate import GatePackage, ParityGate  # noqa: E402

PACKAGE = GatePackage(
    vertical="millennium",
    horizontal="millennium-horizontal",
    alist="millennium.a_list",
    evidence=(
        "the whole simulation (trees_063.0-.511, all 512 files simulations/millennium declares)"
    ),
    file_range=(0, 511),
    override_vertical_range=True,
    format_version=3,
    source_format="lhalo_binary",
    column_mapping_sha256="5a74a2e07eca5f3a5fef5e02b75e15c821f94370606d7608b25f082f9f5654b1",
    links_adjacent=0,
    halos=760_667_000,
    forests=14_329_882,
    gapped_descendants=15_026_757,
    max_descendant_span=2,
    required_free_bytes=150 * 1024**3,
)


def main() -> int:
    return ParityGate(PACKAGE).run(
        "millennium-horizontal version 3 parity gate (test_cross_format_identity.py)"
    )


if __name__ == "__main__":
    sys.exit(main())
