#!/usr/bin/env python3
"""
Version 3 route parity gate: micro-uchuu-hdf5 against its own horizontal conversion.

Certifies that the same real merger trees -- the whole MicroUchuu_mergertree
forests-HDF5 catalogue -- read through the vertical driver (``micro-uchuu-hdf5``,
the ``consistent_trees_hdf5`` reader) and through the horizontal driver (``micro-
uchuu-hdf5-horizontal``, a version 3 conversion of exactly those files) produce, for
every output snapshot, identical ``UniqueGalaxyID`` sets and per-id byte-identical
fields under ``halos-only``, with fixed and dynamic timesteps. Parity is promised
against the package's own source format only.

The stages and every check live in ``tests/framework/parity_gate.py``; this file
only pins what the package's data must be. It is a manual, dataset-present
operation, registered only when this package is selected::

    make MODEL=halos-only SIMULATION=micro-uchuu-hdf5-horizontal tests-scientific

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
    vertical="micro-uchuu-hdf5",
    horizontal="micro-uchuu-hdf5-horizontal",
    alist="micro-uchuu.a_list",
    evidence="complete real data (the whole MicroUchuu_mergertree forests-HDF5 catalogue, FileN 0-0)",
    file_range=(0, 0),
    override_vertical_range=False,
    format_version=3,
    source_format="consistent_trees_hdf5",
    column_mapping_sha256="241f277cf059378077c19c16c4584ff171a8f2b9fba55e395a7808e6033bcbce",
    links_adjacent=1,
    halos=22_580_924,
    forests=440_651,
    gapped_descendants=0,
    max_descendant_span=1,
    required_free_bytes=20 * 1024**3,
)


def main() -> int:
    return ParityGate(PACKAGE).run(
        "micro-uchuu-hdf5-horizontal version 3 parity gate (test_cross_format_identity.py)"
    )


if __name__ == "__main__":
    sys.exit(main())
