"""CLI for the ctrees ASCII -> snapshot-HDF5 converter.

Per-phase subcommands over a user-supplied ``--workdir``; canonical metadata
comes from explicit ``--simulation-info`` and ``--a-list`` paths so the
converter stays simulation-agnostic. Later plan slices add the write,
validate, and cross-check stages.

Usage (micro-Uchuu example):
    mimic_venv/bin/python scripts/convert/convert_ctrees.py scatter \\
        --workdir output/convert/micro-uchuu \\
        --forests-list simulations/micro-uchuu-ascii/snapshots/forests.list \\
        --a-list simulations/micro-uchuu-ascii/micro-uchuu.a_list \\
        --simulation-info simulations/micro-uchuu-ascii/simulation_info.yaml \\
        simulations/micro-uchuu-ascii/snapshots/tree_0_0_0.dat
    mimic_venv/bin/python scripts/convert/convert_ctrees.py sort \\
        --workdir output/convert/micro-uchuu
    mimic_venv/bin/python scripts/convert/convert_ctrees.py fixups \\
        --workdir output/convert/micro-uchuu \\
        --a-list simulations/micro-uchuu-ascii/micro-uchuu.a_list \\
        --simulation-info simulations/micro-uchuu-ascii/simulation_info.yaml
    mimic_venv/bin/python scripts/convert/convert_ctrees.py links \\
        --workdir output/convert/micro-uchuu
    mimic_venv/bin/python scripts/convert/convert_ctrees.py write \\
        --workdir output/convert/micro-uchuu \\
        --a-list simulations/micro-uchuu-ascii/micro-uchuu.a_list \\
        --simulation-info simulations/micro-uchuu-ascii/simulation_info.yaml
    mimic_venv/bin/python scripts/convert/convert_ctrees.py report \\
        --workdir output/convert/micro-uchuu \\
        --a-list simulations/micro-uchuu-ascii/micro-uchuu.a_list

The producer validation battery is a standalone CLI (see validate.py):
    mimic_venv/bin/python scripts/convert/validate.py output/convert/micro-uchuu/hdf5 \\
        --a-list simulations/micro-uchuu-ascii/micro-uchuu.a_list \\
        --manifest output/convert/micro-uchuu/manifest.json
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

    fixups = sub.add_parser(
        "fixups",
        help="Phase 3 steps 1-5: adjacency validation, spin/Len conventions, "
        "fix_flybys/fix_upid equivalents",
    )
    _add_workdir(fixups)
    fixups.add_argument("--a-list", required=True, help="canonical a_list (one scale per line)")
    fixups.add_argument(
        "--simulation-info", required=True, help="simulation_info.yaml (particle mass for Len)"
    )
    fixups.add_argument(
        "--snapshot",
        type=int,
        action="append",
        default=None,
        help="fix only this snapshot (repeatable; default: all)",
    )

    links = sub.add_parser(
        "links",
        help="Phase 3 steps 6-9: FoF chains, descendant/progenitor links, ranks, "
        "identity fields (always all snapshots — FirstProgenitor flows forward)",
    )
    _add_workdir(links)

    write = sub.add_parser(
        "write",
        help="emit snapshot_NNN.h5 + forests.h5 per docs/dev/SNAPSHOT-HDF5-FORMAT.md "
        "(one file per a_list snapshot, including empty ones)",
    )
    _add_workdir(write)
    write.add_argument("--a-list", required=True, help="canonical a_list (one scale per line)")
    write.add_argument(
        "--simulation-info", required=True, help="simulation_info.yaml (header attributes)"
    )
    write.add_argument(
        "--output-dir",
        default=None,
        help="dataset output directory (default: <workdir>/hdf5)",
    )

    report = sub.add_parser(
        "report",
        help="run the producer validation battery over the emitted dataset and write "
        "the conversion report (exit 1 if validation fails)",
    )
    _add_workdir(report)
    report.add_argument("--a-list", required=True, help="canonical a_list (one scale per line)")
    report.add_argument(
        "--multiplier",
        type=int,
        default=None,
        help="UniqueGalaxyID multiplier for the header bound checks (default 1e9)",
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
        elif args.command == "fixups":
            from fixups import run_fixups

            run_fixups(
                args.workdir,
                a_list_path=args.a_list,
                simulation_info_path=args.simulation_info,
                snapshots=args.snapshot,
            )
        elif args.command == "links":
            from links import run_links

            run_links(args.workdir)
        elif args.command == "write":
            from hdf5_writer import run_write

            run_write(
                args.workdir,
                a_list_path=args.a_list,
                simulation_info_path=args.simulation_info,
                output_dir=args.output_dir,
            )
        elif args.command == "report":
            from report import run_report
            from validate import DEFAULT_MULTIPLIER

            multiplier = args.multiplier if args.multiplier is not None else DEFAULT_MULTIPLIER
            report = run_report(args.workdir, a_list_path=args.a_list, multiplier=multiplier)
            if not report["validation_passed"]:
                return 1
    except ConverterError as exc:
        print("ERROR: {}".format(exc), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
