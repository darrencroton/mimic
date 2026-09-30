#!/usr/bin/env python3
"""
Gapped mini-Millennium parity gate: vertical L-Halo against horizontal version 3.

Certifies that the same real mini-Millennium merger trees, read through the
vertical driver (``mini-millennium``, L-Halo binary ``trees_063.0``-``trees_063.7``)
and through the horizontal driver (``mini-millennium-horizontal``, a version 3
conversion of those same eight files carrying 29,291 gapped descendant links),
produce, for every output snapshot, identical ``UniqueGalaxyID`` sets and per-id
byte-identical fields. Four legs: ``halos-only`` and ``sage16``, each with fixed
and dynamic timesteps. It is the real-data evidence that the horizontal driver
handles gapped (non-adjacent) links.

The stages and every check live in ``tests/framework/parity_gate.py``; this file
only pins what the package's data must be. It is a manual, dataset-present
operation, registered only when this package is selected (``MODEL=`` selects
only the ambient tier build; the gate builds both models itself)::

    make MODEL=halos-only SIMULATION=mini-millennium-horizontal tests-scientific

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
    vertical="mini-millennium",
    horizontal="mini-millennium-horizontal",
    alist="mini-millennium.a_list",
    evidence="complete real data (all 8 of 8 trees_063 files, 29,291 gapped descendant links)",
    # The committed run files declare no range; simulation_info.yaml gives 0-7.
    file_range=(0, 7),
    override_vertical_range=False,
    format_version=3,
    source_format="lhalo_binary",
    column_mapping_sha256="5a74a2e07eca5f3a5fef5e02b75e15c821f94370606d7608b25f082f9f5654b1",
    links_adjacent=0,
    halos=1_533_122,
    forests=29_585,
    gapped_descendants=29_291,
    max_descendant_span=2,
    required_free_bytes=10 * 1024**3,
    models=("halos-only", "sage16"),
)


def main() -> int:
    return ParityGate(PACKAGE).run(
        "mini-millennium-horizontal version 3 parity gate (test_cross_format_identity.py)"
    )


if __name__ == "__main__":
    sys.exit(main())
