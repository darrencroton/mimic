#!/usr/bin/env python3
"""Sanity-inspect a Mimic binary run output: schema, finiteness, and per-field ranges.

Usage (from the repo root, venv active for numpy):

    python3 .agents/skills/mimic-diagnostics-and-tooling/scripts/inspect_run_output.py \
        <output_dir_or_binary_file> [--ranges tests/generated/property_ranges.json]

Reads the run's OWN metadata/output_schema.json (never the current checkout's metadata),
loads every binary partition file (or the single file given), and prints: one row per
field (type, units, min, max, NaN/Inf counts) plus range-violation counts when a
property_ranges.json manifest is supplied. Exit code 1 if any NaN/Inf or range
violation is found, else 0.
"""

import argparse
import json
import re
import sys
from pathlib import Path

import numpy as np

REPO_ROOT = Path(__file__).resolve()
while not (REPO_ROOT / "Makefile").exists() and REPO_ROOT != REPO_ROOT.parent:
    REPO_ROOT = REPO_ROOT.parent
sys.path.insert(0, str(REPO_ROOT / "plot" / "mimic-plot"))

from output_schema import dtype_from_schema, load_schema, units_from_schema  # noqa: E402


def read_binary(path, dtype):
    """Read one binary partition; return None (with a warning) on schema mismatch.

    Guard: header-implied size must equal the actual file size. A mismatch means the
    file was written by a build with a different record layout than this schema —
    reading it would produce garbage, the classic wrong-schema failure mode.
    """
    with open(path, "rb") as f:
        ntrees = int(np.fromfile(f, dtype=np.int32, count=1)[0])
        ngals = int(np.fromfile(f, dtype=np.int32, count=1)[0])
        expected = 8 + 4 * ntrees + dtype.itemsize * ngals
        actual = path.stat().st_size
        if expected != actual:
            print(
                f"SCHEMA MISMATCH: {path.name}: header implies {expected} bytes "
                f"({ngals} x {dtype.itemsize}B records), file is {actual} bytes — "
                f"written by a different schema; skipping (read it with ITS OWN "
                f"metadata/output_schema.json)"
            )
            return None
        np.fromfile(f, dtype=np.int32, count=ntrees)  # per-tree counts
        return np.fromfile(f, dtype=dtype, count=ngals)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("target", help="run output directory, or one binary output file")
    ap.add_argument("--ranges", help="optional property_ranges.json manifest to check against")
    args = ap.parse_args()

    target = Path(args.target)
    outdir = target if target.is_dir() else target.parent
    schema = load_schema(outdir)
    dtype = dtype_from_schema(schema, binary=True)
    units = units_from_schema(schema)

    files = (
        [target]
        if target.is_file()
        else sorted(
            p for p in outdir.iterdir() if p.is_file() and re.search(r"_z\d+\.\d+_\d+$", p.name)
        )
    )
    if not files:
        sys.exit(f"No binary output files found in {outdir}")

    parts = [(p, read_binary(p, dtype)) for p in files]
    loaded = [h for _, h in parts if h is not None]
    if not loaded:
        sys.exit("No files matched the directory's schema.")
    halos = np.concatenate(loaded)
    print(f"Loaded {len(halos)} records from {len(loaded)}/{len(files)} file(s) in {outdir}")

    ranges = {}
    if args.ranges:
        manifest = json.loads(Path(args.ranges).read_text())
        # properties is a dict: name -> {name, category, type, units, range?, sentinels?}
        for name, prop in manifest.get("properties", {}).items():
            if prop.get("range"):
                ranges[name] = (prop["range"], prop.get("sentinels") or [])

    bad = 0
    print(
        f"{'field':28s} {'unit':16s} {'min':>12s} {'max':>12s} {'NaN':>6s} {'Inf':>6s} {'range!':>7s}"
    )
    for name in halos.dtype.names:
        col = halos[name]
        if col.dtype.kind not in "fiu":
            continue
        flat = col.astype(np.float64).ravel()
        nan = int(np.isnan(flat).sum())
        inf = int(np.isinf(flat).sum())
        finite = flat[np.isfinite(flat)]
        lo = finite.min() if finite.size else float("nan")
        hi = finite.max() if finite.size else float("nan")
        viol = ""
        if name in ranges:
            (rmin, rmax), sentinels = ranges[name]
            mask = np.isfinite(flat)
            for s in sentinels:
                mask &= flat != s
            n_viol = int(((flat < rmin) | (flat > rmax))[mask].sum())
            viol = str(n_viol)
            bad += n_viol
        bad += nan + inf
        print(
            f"{name:28s} {units.get(name, ''):16s} {lo:12.5g} {hi:12.5g} {nan:6d} {inf:6d} {viol:>7s}"
        )

    print(f"\nRESULT: {'FAIL' if bad else 'OK'} ({bad} NaN/Inf/range findings)")
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
