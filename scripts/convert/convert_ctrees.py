"""CLI for the ctrees ASCII -> snapshot-HDF5 converter.

Per-phase subcommands over a user-supplied ``--workdir``; canonical metadata
comes from explicit ``--simulation-info`` and ``--a-list`` paths so the
converter stays simulation-agnostic. Later plan slices add the fix-up, link,
write, validate, and cross-check stages.

Usage (micro-Uchuu example):
    mimic_venv/bin/python scripts/convert/convert_ctrees.py scatter \\
        --workdir output/convert/micro-uchuu \\
        --forests-list simulations/micro-uchuu-ascii/snapshots/forests.list \\
        --a-list simulations/micro-uchuu-ascii/micro-uchuu.a_list \\
        --simulation-info simulations/micro-uchuu-ascii/simulation_info.yaml \\
        simulations/micro-uchuu-ascii/snapshots/tree_0_0_0.dat
    mimic_venv/bin/python scripts/convert/convert_ctrees.py sort \\
        --workdir output/convert/micro-uchuu
"""

import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from ctrees_parser import ConverterError  # noqa: E402


def _add_workdir(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--workdir", required=True, help="scratch/output directory for this run")


def build_arg_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="convert_ctrees",
        description="Convert Consistent-Trees ASCII output to Mimic snapshot-ordered HDF5",
    )
    sub = parser.add_subparsers(dest="command", required=True)

    scatter = sub.add_parser(
        "scatter", help="Phase 0 + 1: forest map, scatter to per-snapshot scratch binaries"
    )
    _add_workdir(scatter)
    scatter.add_argument("--forests-list", required=True, help="path to forests.list")
    scatter.add_argument("--a-list", required=True, help="canonical a_list (one scale per line)")
    scatter.add_argument(
        "--simulation-info", required=True, help="simulation_info.yaml (recorded for provenance)"
    )
    scatter.add_argument("--pool-size", type=int, default=1, help="worker processes (default 1)")
    scatter.add_argument(
        "--chunksize", type=int, default=1_000_000, help="parser rows per chunk (default 1e6)"
    )
    scatter.add_argument("tree_files", nargs="+", help="ctrees ASCII tree files")

    sort = sub.add_parser("sort", help="Phase 2: per-snapshot sort by id + id index")
    _add_workdir(sort)
    sort.add_argument(
        "--snapshot",
        type=int,
        action="append",
        default=None,
        help="sort only this snapshot (repeatable; default: all)",
    )
    return parser


def main(argv=None) -> int:
    args = build_arg_parser().parse_args(argv)
    try:
        if args.command == "scatter":
            from scatter import run_scatter

            run_scatter(
                tree_files=args.tree_files,
                forests_list_path=args.forests_list,
                a_list_path=args.a_list,
                workdir=args.workdir,
                pool_size=args.pool_size,
                chunksize=args.chunksize,
                simulation_info_path=args.simulation_info,
            )
        elif args.command == "sort":
            from sort_index import run_sort

            run_sort(args.workdir, snapshots=args.snapshot)
    except ConverterError as exc:
        print("ERROR: {}".format(exc), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
