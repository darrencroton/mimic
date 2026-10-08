"""The forest census of a horizontal-HDF5 dataset (version 2 or 3).

Measures how a dataset's halos are distributed over forests and over effective
descendant trees, slab by slab, and simulates the horizontal driver's two-level
forest partition, without ever holding a whole column of a slab: every read is a
bounded block (``horizontal_dataset.py``). Results go to an **aggregate
directory** bound to the dataset it was produced from (``census/aggregate.py``);
a subcommand given another dataset refuses the directory. Each subcommand writes
its own subdirectory and a ``summary.json`` last; aggregate sizes and their
formulas are in every summary. The directory is deleted by hand when the
census is done.

Subcommands::

    forest_census.py occupancy --dataset DIR --aggregate DIR [--block-rows N]
        Per slab, the (ForestIndex, count) pairs of the forests present; per
        forest, its total, maximum occupancy and the slab of the maximum; the
        widest slab and its top-10 and top-100 shares (census/occupancy.py).

    forest_census.py trees --dataset DIR --aggregate DIR
            [--forests-list FILE --locations FILE [--conversion-report JSON]]
            [--block-rows N]
        Terminal-root labels for every halo (int32 root ordinals per slab, kept
        for the graph and the cut), per-tree totals and per-slab occupancy, the
        root correspondence with the index files and the per-file conservation
        check (census/trees.py).

    forest_census.py partition --aggregate DIR [--ntask 1,2,4,8]
            [--nchunk 1,2,4,8,16,32]
        The driver's task and chunk partition with the widest slab's weights,
        for every grid point: every range's rows in every slab and the widest
        range, with the heaviest forest as the floor (census/partition.py).
        Needs a completed ``occupancy``.

Exit status: 0 when the subcommand completed (a failed correspondence or
conservation verdict is a result, recorded and printed, not an error); 2 on a
refusal or unreadable input.
"""

import argparse
import os
import sys
from typing import List, Optional, Sequence

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from census.occupancy import run_occupancy  # noqa: E402
from census.partition import DEFAULT_NCHUNKS, DEFAULT_NTASKS, run_partition  # noqa: E402
from census.trees import run_trees  # noqa: E402
from errors import ConverterError  # noqa: E402
from horizontal_dataset import DEFAULT_BLOCK_ROWS, HorizontalDataset  # noqa: E402
from source_index import SourceIndex  # noqa: E402
from subset import SubsetError  # noqa: E402


def _log(message: str) -> None:
    print(message, file=sys.stderr, flush=True)


def _counts(text: str) -> List[int]:
    try:
        values = [int(part) for part in text.split(",") if part.strip()]
    except ValueError as exc:
        raise argparse.ArgumentTypeError(
            "expected comma-separated integers, got {!r}".format(text)
        ) from exc
    if not values or min(values) < 1:
        raise argparse.ArgumentTypeError("expected positive integers, got {!r}".format(text))
    return values


def _positive(text: str) -> int:
    value = int(text)
    if value < 1:
        raise argparse.ArgumentTypeError("expected a positive integer, got {!r}".format(text))
    return value


def _print_occupancy(summary: dict) -> None:
    print(
        "dataset: format_version {}, n_forests_total {}".format(
            summary["dataset"]["format_version"], summary["n_forests_total"]
        )
    )
    for row in summary["per_snapshot"]:
        print(
            "snapshot {:3d}: {} halos, {} forests present".format(
                row["snapshot"], row["halos"], row["forests_present"]
            )
        )
    print("total halos: {}".format(summary["total_halos"]))
    widest = summary["widest_slab"]
    if widest is not None:
        print(
            "widest slab: snapshot {}, {} halos, top-10 share {}, top-100 share {}".format(
                widest["snapshot"],
                widest["halos"],
                widest["top_shares"]["10"]["share"],
                widest["top_shares"]["100"]["share"],
            )
        )
    largest = summary["largest_forest"]
    if largest is not None:
        print(
            "largest forest: ForestIndex {} (ForestID {}), {} halos, peak {} at snapshot {}".format(
                largest["forest_index"],
                largest["forest_id"],
                largest["total_halos"],
                largest["peak_occupancy"],
                largest["peak_snapshot"],
            )
        )
    print("forests exceeding (any slab): {}".format(summary["forests_exceeding"]))


def _print_trees(summary: dict) -> None:
    print(
        "trees: {} effective trees over {} halos".format(summary["n_trees"], summary["total_halos"])
    )
    largest = summary["largest_tree"]
    if largest is not None:
        print(
            "largest tree: root {} (ForestIndex {}), {} halos, peak {} at snapshot {}".format(
                largest["root_id"],
                largest["forest_index"],
                largest["total_halos"],
                largest["peak_occupancy"],
                largest["peak_snapshot"],
            )
        )
    correspondence = summary["root_correspondence"]
    print(
        "root correspondence: {} ({} roots outside the last snapshot)".format(
            correspondence["verdict"], correspondence["roots_not_in_last_snapshot"]["count"]
        )
    )
    if summary["conservation"] is not None:
        print("conservation: {}".format(summary["conservation"]["verdict"]))


def _print_partition(summary: dict) -> None:
    print(
        "weights: snapshot {} ({} halos); floor {} rows at snapshot {}".format(
            summary["weights"]["snapshot"],
            summary["weights"]["halos"],
            summary["floor"]["max_rows"],
            summary["floor"]["snapshot"],
        )
    )
    for point in summary["grid"]:
        widest = point["widest"]
        print(
            "ntask {:3d} nchunk {:3d}: widest range {} rows (snapshot {}, task {}, chunk {})".format(
                point["ntask"],
                point["nchunk"],
                widest["rows"],
                widest["snapshot"],
                widest["task"],
                widest["chunk"],
            )
        )


def cmd_occupancy(args: argparse.Namespace) -> int:
    dataset = HorizontalDataset(args.dataset)
    _print_occupancy(run_occupancy(dataset, args.aggregate, args.block_rows, _log))
    return 0


def cmd_trees(args: argparse.Namespace) -> int:
    if (args.forests_list is None) != (args.locations is None):
        raise ConverterError("--forests-list and --locations are given together or not at all")
    dataset = HorizontalDataset(args.dataset)
    index = None
    if args.forests_list is not None:
        _log("trees: loading {} and {}".format(args.forests_list, args.locations))
        index = SourceIndex.load(args.forests_list, args.locations)
    summary = run_trees(
        dataset, args.aggregate, index, args.conversion_report, args.block_rows, _log
    )
    _print_trees(summary)
    return 0


def cmd_partition(args: argparse.Namespace) -> int:
    _print_partition(run_partition(args.aggregate, args.ntask, args.nchunk, _log))
    return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="forest_census.py",
        description="Forest and effective-tree census of a horizontal-HDF5 dataset.",
    )
    sub = parser.add_subparsers(dest="command", required=True)

    def dataset_arguments(command):
        command.add_argument("--dataset", required=True, help="horizontal-HDF5 dataset directory")
        command.add_argument("--aggregate", required=True, help="census aggregate directory")
        command.add_argument(
            "--block-rows",
            type=_positive,
            default=DEFAULT_BLOCK_ROWS,
            help="rows per column block read (default 2^22)",
        )

    occupancy = sub.add_parser("occupancy", help="per-slab and per-forest occupancy")
    dataset_arguments(occupancy)
    occupancy.set_defaults(func=cmd_occupancy)

    trees = sub.add_parser("trees", help="terminal-root labels and effective-tree occupancy")
    dataset_arguments(trees)
    trees.add_argument("--forests-list", default=None, help="the catalogue's forests.list")
    trees.add_argument("--locations", default=None, help="the catalogue's locations.dat")
    trees.add_argument(
        "--conversion-report",
        default=None,
        help="JSON with per-file parsed_count under source_files, for the conservation check",
    )
    trees.set_defaults(func=cmd_trees)

    partition = sub.add_parser("partition", help="simulate the driver's task/chunk partition")
    partition.add_argument("--aggregate", required=True, help="census aggregate directory")
    partition.add_argument(
        "--ntask", type=_counts, default=list(DEFAULT_NTASKS), help="comma-separated task counts"
    )
    partition.add_argument(
        "--nchunk",
        type=_counts,
        default=list(DEFAULT_NCHUNKS),
        help="comma-separated chunk counts per task",
    )
    partition.set_defaults(func=cmd_partition)
    return parser


def main(argv: Optional[Sequence[str]] = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        return args.func(args)
    except (ConverterError, SubsetError, OSError) as exc:
        _log("FATAL: {}".format(exc))
        return 2


if __name__ == "__main__":
    sys.exit(main())
