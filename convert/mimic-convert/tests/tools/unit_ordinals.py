"""Per-forest source-unit check: an independent C source-payload dump vs ``forests.h5``.

Compares each non-empty forest's rank-0 halo (read via ``awk`` over the
plain-text ``mimic-source-dump v1`` dump) against the converter's own
``forests.h5``: the dump's file number and within-file unit index against
``forests.h5``'s ``SourceFileOrdinal``/``SourceUnitOrdinal``, both indexed by
``ForestIndex``. Raw h5py and a plain-text ``awk`` scan only; imports nothing
from the converter (``convert/mimic-convert/``) or the acceptance harness
(``convert/mimic-convert/tests/run_generalisation_acceptance.py``) — only the
standard library, h5py and numpy.

A zero mismatch count is the independent evidence that the converter's source
ordinals name the same units the C reader enumerates.

Usage:
    mimic_venv/bin/python convert/mimic-convert/tests/tools/unit_ordinals.py <dump> <dataset-dir>

``<dump>`` is a ``mimic-source-dump v1`` file, as written by
``dump_ctrees_topology --source-payload``. ``<dataset-dir>`` holds the
converter's ``forests.h5``. Prints one JSON line: sidecar forest count, dump
non-empty forest count, their difference, the mismatch count and the
observed file-ordinal range.
"""

import json
import subprocess
import sys

import h5py
import numpy as np


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: unit_ordinals.py <dump> <dataset-dir>", file=sys.stderr)
        return 2

    dump, dataset = sys.argv[1], sys.argv[2]
    awk = subprocess.run(
        ["awk", "!/^#/ && $2 == 0 {print $1, $3, $4}", dump],
        check=True,
        capture_output=True,
        text=True,
    ).stdout
    ref = (
        np.loadtxt(awk.splitlines(), dtype=np.int64, ndmin=2) if awk else np.empty((0, 3), np.int64)
    )
    with h5py.File(dataset + "/forests.h5", "r") as f:
        file_ord = f["SourceFileOrdinal"][...]
        unit_ord = f["SourceUnitOrdinal"][...]
        n = f["ForestID"].shape[0]
    forest = ref[:, 0]
    ok_range = (forest >= 0) & (forest < n)
    mism = int((~ok_range).sum())
    sel = forest[ok_range]
    mism += int(((file_ord[sel] != ref[ok_range, 1]) | (unit_ord[sel] != ref[ok_range, 2])).sum())
    print(
        json.dumps(
            {
                "dump": dump,
                "sidecar_forests": int(n),
                "dump_nonempty_forests": int(len(ref)),
                "sidecar_minus_dump": int(n - len(ref)),
                "unit_ordinal_mismatches": mism,
                "min_file": int(file_ord.min()) if n else None,
                "max_file": int(file_ord.max()) if n else None,
            }
        )
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
