"""Independent census of a version 3 horizontal-HDF5 dataset.

Counts snapshot files and rows, and measures forward descendant-link gaps
(a non-null ``Descendant`` whose target snapshot is more than one snapshot
ahead). Reads only raw h5py arrays, one snapshot file at a time, and imports
nothing from the converter (``scripts/convert/``) or the acceptance harness
(``scripts/convert/tests/run_generalisation_acceptance.py``) — only the
standard library, h5py and numpy.

Reproduces the census reported in
``docs/dev/MIMIC-CONVERTER-GENERALISATION-ACCEPTANCE.md`` section 2.1.

Usage:
    mimic_venv/bin/python scripts/convert/tests/tools/gap_census.py <dataset-dir>

``<dataset-dir>`` holds ``snapshot_*.h5`` files (e.g. a converter ``write``
attempt directory such as ``<workdir>/write/attempt_NNN``). Prints one JSON
line: snapshot-file count, empty-snapshot-file count, total halos, non-null
descendant links, forward gaps (span > 1) and the maximum span.
"""

import json
import sys
from pathlib import Path

import h5py
import numpy as np


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: gap_census.py <dataset-dir>", file=sys.stderr)
        return 2

    dataset = Path(sys.argv[1])
    rows = files = links = gaps = 0
    max_span = 0
    empty = 0
    for path in sorted(dataset.glob("snapshot_*.h5")):
        files += 1
        with h5py.File(path, "r") as handle:
            halos = handle["halos"]
            n = halos["SourceHaloID"].shape[0]
            rows += n
            empty += n == 0
            if n == 0:
                continue
            snap = halos["SnapNum"][...].astype(np.int64)
            dsnap = halos["DescendantSnapshot"][...].astype(np.int64)
            desc = halos["Descendant"][...]
            linked = desc >= 0
            assert np.array_equal(linked, dsnap >= 0)
            span = dsnap[linked] - snap[linked]
            assert (span >= 1).all()
            links += int(linked.sum())
            gaps += int((span > 1).sum())
            if span.size:
                max_span = max(max_span, int(span.max()))
    print(
        json.dumps(
            {
                "dataset": str(dataset),
                "snapshot_files": files,
                "empty_snapshot_files": empty,
                "halos": rows,
                "non_null_descendants": links,
                "forward_gaps": gaps,
                "max_span": max_span,
            }
        )
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
