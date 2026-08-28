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

For a source too large to hold locally all at once, scatter runs in batch mode:
every invocation is handed the *complete* frozen inventory, scatters whatever
subset has arrived, and stops without finalizing. Once a batch is scattered,
``release`` verifies its intermediates and records its entries as consumed, at
which point the operator may delete those source bytes and transfer the next
batch. ``finalize`` runs once nothing is deferred:

    # ... transfer batch 1 ...
    convert_ctrees.py scatter --batch --workdir W --forests-list F --a-list A \\
        --simulation-info S <every file of the full inventory, in frozen order>
    convert_ctrees.py release --workdir W <the batch-1 files>
    # ... delete batch 1, transfer batch 2, re-run scatter --batch, release ...
    convert_ctrees.py finalize --workdir W --forests-list F

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


def _add_consume_flag(parser: argparse.ArgumentParser) -> None:
    """The opt-in consumptive-deletion flag (plan Slice 8).

    Off by default and deliberately per-invocation: destroying intermediates on
    a multi-day conversion with no way back is an operator decision, not a
    default. With it off the stage deletes nothing it does not delete today.
    """
    parser.add_argument(
        "--consume-intermediates",
        action="store_true",
        help="delete each intermediate this stage consumes, once the successor artifact "
        "has been verified and recorded in the manifest (fixups: the sorted scratch; "
        "links: the pending first-progenitor buffers and id indexes; write: the fixed "
        "and links scratch). IRREVERSIBLE: the workdir can then only be resumed from "
        "the last surviving stage. Off by default; emitted output is identical either "
        "way.",
    )


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
    scatter.add_argument(
        "--batch",
        action="store_true",
        help="batch mode for the interleaved consumptive transfer: tree_files must "
        "still be the complete frozen inventory, entries whose bytes have not "
        "arrived yet are deferred instead of fatal, and the run does not finalize "
        "(use the finalize subcommand). Off by default.",
    )
    scatter.add_argument("tree_files", nargs="+", help="ctrees ASCII tree files")

    release = sub.add_parser(
        "release",
        help="batch mode: record completed source files as consumed, after verifying "
        "every intermediate their scatter produced, so their bytes may be deleted "
        "(the converter never deletes source data itself)",
    )
    _add_workdir(release)
    release.add_argument(
        "tree_files", nargs="+", help="completed source files whose bytes are to be released"
    )

    finalize = sub.add_parser(
        "finalize",
        help="batch mode: explicit Phase 1 finalize (root coverage, per-snapshot concat, "
        "aggregate merge, sidecar tables); refuses to run while any inventory entry "
        "is still deferred",
    )
    _add_workdir(finalize)
    finalize.add_argument("--forests-list", required=True, help="path to forests.list")

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
    _add_consume_flag(fixups)

    links = sub.add_parser(
        "links",
        help="Phase 3 steps 6-9: FoF chains, descendant/progenitor links, ranks, "
        "identity fields (always all snapshots — FirstProgenitor flows forward)",
    )
    _add_workdir(links)
    links.add_argument(
        "--memory-budget-mb",
        type=int,
        default=None,
        help="working-memory budget for the rank/identity pass, in MiB (default 2048). "
        "It bounds the external merge sort's resident records and the identity "
        "stream's read windows; the values written are identical at any budget, so "
        "this trades memory against spill I/O and nothing else",
    )
    _add_consume_flag(links)

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
    _add_consume_flag(write)

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
                batch_mode=args.batch,
            )
        elif args.command == "release":
            from scatter import run_release

            run_release(args.workdir, args.tree_files)
        elif args.command == "finalize":
            from scatter import run_finalize

            run_finalize(args.workdir, args.forests_list)
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
                consume_intermediates=args.consume_intermediates,
            )
        elif args.command == "links":
            from links import DEFAULT_RANK_BUDGET_BYTES, run_links

            budget_bytes = (
                args.memory_budget_mb * 1024**2
                if args.memory_budget_mb is not None
                else DEFAULT_RANK_BUDGET_BYTES
            )
            run_links(
                args.workdir,
                budget_bytes=budget_bytes,
                consume_intermediates=args.consume_intermediates,
            )
        elif args.command == "write":
            from hdf5_writer import run_write

            run_write(
                args.workdir,
                a_list_path=args.a_list,
                simulation_info_path=args.simulation_info,
                output_dir=args.output_dir,
                consume_intermediates=args.consume_intermediates,
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
