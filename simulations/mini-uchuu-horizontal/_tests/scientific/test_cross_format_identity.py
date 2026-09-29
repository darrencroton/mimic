#!/usr/bin/env python3
"""
Version 3 route parity gate: mini-uchuu against its own horizontal conversion.

Certifies that the same real merger trees -- Uchuu400_Planck_lhalo_binary.0-.15, a
sampled subset of the 128 files the package declares -- read through the vertical
driver (``mini-uchuu``, the ``lhalo_binary`` reader) and through the horizontal
driver (``mini-uchuu-horizontal``, a version 3 conversion of exactly those files)
produce, for every output snapshot, identical ``UniqueGalaxyID`` sets and per-id
byte-identical fields under ``halos-only``, with fixed and dynamic timesteps. Parity
is promised against the package's own source format only.

The stages and every check live in ``tests/framework/parity_gate.py``; this file
only pins what the package's data must be. It is a manual, dataset-present
operation, registered only when this package is selected::

    make MODEL=halos-only SIMULATION=mini-uchuu-horizontal tests-scientific

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
    vertical="mini-uchuu",
    horizontal="mini-uchuu-horizontal",
    alist="mini-uchuu.a_list",
    evidence="a sampled subset (Uchuu400_Planck_lhalo_binary.0-.15 of the 128 files simulations/mini-uchuu declares)",
    file_range=(0, 15),
    override_vertical_range=True,
    format_version=3,
    source_format="lhalo_binary",
    column_mapping_sha256="5a74a2e07eca5f3a5fef5e02b75e15c821f94370606d7608b25f082f9f5654b1",
    links_adjacent=1,
    halos=181_188_125,
    forests=3_230_400,
    gapped_descendants=0,
    max_descendant_span=1,
    required_free_bytes=100 * 1024**3,
)


def main() -> int:
    return ParityGate(PACKAGE).run(
        "mini-uchuu-horizontal version 3 parity gate (test_cross_format_identity.py)"
    )


if __name__ == "__main__":
    sys.exit(main())
