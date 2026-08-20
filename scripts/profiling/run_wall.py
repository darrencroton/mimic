#!/usr/bin/env python3
"""Serial wall-clock timer for mimic runs.

Usage: run_wall.py <run.yaml> <repeats> <label> [outdir]

Runs `./mimic <run.yaml> -q` `repeats` times, strictly one at a time, timing each
with time.perf_counter and capturing peak RSS via resource.getrusage(RUSAGE_CHILDREN)
deltas.  Emits JSON on stdout: {label, cmd, repeats, runs:[{wall,rc,rss_bytes}], ...}

The first repeat is normally discarded by the caller as warm-up (OS file cache).
"""

import json
import os
import resource
import subprocess
import sys
import time

REPO = os.environ.get(
    "MIMIC_REPO", os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
)


def main():
    cfg, repeats, label = sys.argv[1], int(sys.argv[2]), sys.argv[3]
    out = sys.argv[4] if len(sys.argv) > 4 else None
    cmd = ["./mimic", cfg, "-q"]
    runs = []
    for i in range(repeats):
        t0 = time.perf_counter()
        p = subprocess.run(cmd, cwd=REPO, capture_output=True, text=True)
        t1 = time.perf_counter()
        ru = resource.getrusage(resource.RUSAGE_CHILDREN)
        # ru_maxrss is a cumulative high-water across ALL children (bytes on macOS),
        # not a per-run figure — it is monotonic across repeats. Use it as the run's
        # RSS ceiling only because every repeat runs the same workload serially.
        rss = ru.ru_maxrss
        runs.append(
            {
                "i": i,
                "wall": t1 - t0,
                "rc": p.returncode,
                "rss_highwater": rss,
                "stdout_tail": p.stdout[-2000:],
            }
        )
        print(f"  {label} run {i}: {t1-t0:.3f}s rc={p.returncode}", file=sys.stderr)
    res = {"label": label, "cmd": " ".join(cmd), "repeats": repeats, "runs": runs}
    if out:
        with open(out, "w") as fh:
            json.dump(res, fh, indent=1)
    print(json.dumps(res))


if __name__ == "__main__":
    main()
