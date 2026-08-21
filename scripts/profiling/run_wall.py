#!/usr/bin/env python3
"""Serial wall-clock timer for mimic runs.

    run_wall.py <run.yaml> <repeats> <label> [-o OUT]

Runs `./mimic <run.yaml> -q` the requested number of times, strictly one at a time, timing
each with time.perf_counter.  Emits JSON on stdout, and to OUT when given:

    {label, cmd, repeats, peak_child_rss_bytes, runs: [{i, wall, rc, stdout_tail}]}

A non-zero return code aborts: a crashed run would otherwise contribute a spuriously fast
wall time to the mean that attribute.py uses to convert percentages into milliseconds.

The run whose tree data failed to load is the harder case, because it still exits 0 -- see
precondition 1 in README.md.  Each run's galaxy-pool high-water is checked against mimic's
own run memory profile, and a zero or absent high-water is reported as a warning rather
than an error, because a model package that creates no galaxies is legitimate.

The first repeat is normally discarded by the caller as warm-up, since it reads the tree
files before the OS file cache is warm.
"""

import argparse
import json
import os
import re
import resource
import subprocess
import sys
import time

REPO = os.path.realpath(
    os.environ.get(
        "MIMIC_REPO",
        os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))),
    )
)

GALAXY_POOL_RE = re.compile(r"Galaxy pool high-water G:\s*(\d+)\s+galaxies")


def galaxy_pool_high_water(stdout):
    """Galaxies reported by mimic's run memory profile, or None when absent."""
    match = GALAXY_POOL_RE.search(stdout)
    return int(match.group(1)) if match else None


def time_runs(config, repeats, label):
    """Time `repeats` serial mimic runs, aborting on the first failure.

    Returns:
        The result record described in the module docstring.

    Raises:
        SystemExit: a run exited non-zero.
    """
    cmd = ["./mimic", config, "-q"]
    runs = []
    for i in range(repeats):
        start = time.perf_counter()
        completed = subprocess.run(cmd, cwd=REPO, capture_output=True, text=True)
        wall = time.perf_counter() - start
        if completed.returncode != 0:
            sys.stderr.write(completed.stdout[-2000:])
            sys.stderr.write(completed.stderr[-2000:])
            raise SystemExit(
                f"run_wall.py: repeat {i} of {label} exited {completed.returncode}; "
                "timings from a failed run would corrupt the mean"
            )
        galaxies = galaxy_pool_high_water(completed.stdout)
        if not galaxies:
            print(
                f"run_wall.py: warning: repeat {i} reported no galaxy-pool high-water; "
                "check the simulation snapshots symlink before trusting these timings",
                file=sys.stderr,
            )
        runs.append(
            {
                "i": i,
                "wall": wall,
                "rc": completed.returncode,
                "stdout_tail": completed.stdout[-2000:],
            }
        )
        print(f"  {label} run {i}: {wall:.3f}s rc={completed.returncode}", file=sys.stderr)

    # ru_maxrss is a high-water across all waited-for children and never decreases, so it
    # is a session-level ceiling rather than a per-repeat figure.  For per-run memory use
    # mimic's own run memory profile, which reports peak process RSS directly.
    return {
        "label": label,
        "cmd": " ".join(cmd),
        "repeats": repeats,
        "peak_child_rss_bytes": resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss,
        "runs": runs,
    }


def main():
    parser = argparse.ArgumentParser(description="Time serial mimic runs with a warm cache.")
    parser.add_argument("config", help="run YAML to execute")
    parser.add_argument("repeats", type=int, help="number of serial repeats")
    parser.add_argument("label", help="label recorded in the output JSON")
    parser.add_argument("-o", "--output", help="also write the JSON result to this file")
    args = parser.parse_args()

    if args.repeats < 1:
        parser.error(f"repeats must be at least 1, got {args.repeats}")

    result = time_runs(args.config, args.repeats, args.label)
    if args.output:
        with open(args.output, "w") as fh:
            json.dump(result, fh, indent=1)
    print(json.dumps(result))


if __name__ == "__main__":
    main()
