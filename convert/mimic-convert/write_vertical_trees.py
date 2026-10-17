"""Write a forest-blocked version 3 horizontal-HDF5 dataset as L-Halo binary merger trees.

The output is the file set Mimic's ``lhalo_binary`` reader reads: ``<tree_name>.<k>`` for
``k`` from 0, each the L-Halo header (``int32`` ``ntrees``, ``int32`` ``totNHalos``, one
``int32`` halo count per forest) followed by every forest's records, in the record the named
simulation package's ``halo_properties.yaml`` declares (``vertical/record.py``). Forests are
written in ascending ``ForestIndex`` and each forest's halos in ``HaloRankInForest`` order with
forest-local links, so the vertical driver's identities equal the horizontal driver's on the
same dataset. Two subcommands, run in order into one output directory:

  plan      derive every forest's halo total from the dataset and cut the forests into files
            of contiguous ``ForestIndex`` ranges; writes ``file_plan.json``

  convert   write, verify and rename every planned file (or the selected ones), in parallel
            across files, recording each in ``manifest.json``; a rerun resumes

Usage:

    mimic_venv/bin/python convert/mimic-convert/write_vertical_trees.py plan \\
        --input /data/<dataset> \\
        --halo-properties simulations/<package>/halo_properties.yaml \\
        --output /data/<package-trees>

    mimic_venv/bin/python convert/mimic-convert/write_vertical_trees.py convert \\
        --input /data/<dataset> \\
        --halo-properties simulations/<package>/halo_properties.yaml \\
        --output /data/<package-trees> --tree-name <tree_name> --workers 8
"""

import argparse
import os
import sys
from typing import Optional, Sequence

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from errors import ConverterError  # noqa: E402
from horizontal_dataset import HorizontalDataset  # noqa: E402
from vertical.assemble import DEFAULT_WINDOW_ROWS, WORKING_BYTES_PER_ROW  # noqa: E402
from vertical.plan import (  # noqa: E402
    DEFAULT_MAX_FORESTS_PER_FILE,
    DEFAULT_MAX_HALOS_PER_FILE,
    make_file_plan,
    write_file_plan,
)
from vertical.writer import (  # noqa: E402
    EXIT_OK,
    EXIT_REFUSED,
    HEADER_BYTES_PER_FOREST,
    VERIFY_BLOCK_BYTES,
    WRITE_BUFFER_BYTES,
    convert,
    parse_file_selection,
)

EPILOG = """\
Files: forests are cut, in ascending ForestIndex, into contiguous ranges; a new file starts
when adding the next forest would exceed --max-halos-per-file halos or --max-forests-per-file
forests, and a forest larger than the halo cap occupies its file alone. No file may hold more
than 2,147,483,647 halos or forests (the reader's int counts).

Memory: convert prints its bound before it starts. Per worker it is
  (2*S*R + L)*B + max(R, L)*(B + b + {working}) bytes
for S slabs, R = --window-rows, L the plan's largest forest in halos, B the bytes per halo of
the dataset columns read and b the record's bytes (the forest assembler's windows, the forest
in flight and its batch), plus the {reread} MiB re-read buffer, the {buffer} MiB write buffer
and {header} B per forest of the largest file's header. The parent process holds 8 B per
forest of the forest totals while the files are converted, and about 17 B per forest while it
derives them; plan holds up to 24 B per forest while it cuts the files.

Exit status: 0 when every selected file is complete; 1 when a file fails its verification
(the header against the plan, the size against the header plus totNHalos records, or the
SHA-256 of the re-read file against the one computed while writing): that file stays
<name>.<k>.partial and is never renamed, while every other file is still written and
recorded; 2 on a refusal: an invalid option, unreadable input, a dataset that is not version
3, a missing or mismatching file_plan.json, a malformed forest, any other error in a
file, or a worker that died.
""".format(
    working=WORKING_BYTES_PER_ROW,
    reread=VERIFY_BLOCK_BYTES >> 20,
    buffer=WRITE_BUFFER_BYTES >> 20,
    header=HEADER_BYTES_PER_FOREST,
)


def _positive(text: str) -> int:
    try:
        value = int(text)
    except ValueError:
        raise argparse.ArgumentTypeError("{!r} is not an integer".format(text)) from None
    if value < 1:
        raise argparse.ArgumentTypeError("must be at least 1, got {}".format(value))
    return value


def _common(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--input", required=True, help="the version 3 dataset directory")
    parser.add_argument(
        "--halo-properties",
        required=True,
        help="the target simulation package's halo_properties.yaml, which declares the record",
    )
    parser.add_argument(
        "--output",
        required=True,
        help="the output directory: file_plan.json, the files and manifest.json",
    )


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="write_vertical_trees",
        description="Write a version 3 horizontal-HDF5 dataset as L-Halo binary merger trees "
        "in a simulation package's record: plan, then convert.",
        epilog=EPILOG,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    sub = parser.add_subparsers(dest="command", required=True)

    plan = sub.add_parser(
        "plan",
        help="derive the forest totals and cut the files; writes file_plan.json",
        description="Derive every forest's halo total from one bounded pass over every slab's "
        "ForestIndex and SourceHaloID, cut the forests into files and write "
        "<output>/file_plan.json, bound to the dataset and the record.",
        epilog=EPILOG,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    _common(plan)
    plan.add_argument(
        "--max-halos-per-file",
        type=_positive,
        default=DEFAULT_MAX_HALOS_PER_FILE,
        help="halo cap of a file (default {:,}); a larger forest occupies its file alone".format(
            DEFAULT_MAX_HALOS_PER_FILE
        ),
    )
    plan.add_argument(
        "--max-forests-per-file",
        type=_positive,
        default=DEFAULT_MAX_FORESTS_PER_FILE,
        help="forest cap of a file (default {:,})".format(DEFAULT_MAX_FORESTS_PER_FILE),
    )

    conv = sub.add_parser(
        "convert",
        help="write, verify and rename the planned files; writes manifest.json",
        description="Write every planned file (or the selected ones) as <output>/<tree-name>.<k>: "
        "written to <name>.<k>.partial, re-read whole and checked, then renamed, the largest "
        "files first, each worker one file at a time; every completed file is recorded in "
        "<output>/manifest.json. A rerun with the same inputs skips every file whose SHA-256 "
        "equals its manifest entry's and redoes the rest; a file the manifest does not record "
        "as complete, and any .partial, is moved aside to <name>.stale-<timestamp> (never "
        "deleted) and rebuilt. Run one convert per output directory at a time.",
        epilog=EPILOG,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    _common(conv)
    conv.add_argument(
        "--tree-name",
        required=True,
        help="the files' name prefix, the package's tree_name (no path separator)",
    )
    conv.add_argument(
        "--workers",
        type=_positive,
        default=1,
        help="files converted concurrently, one process each (default 1)",
    )
    conv.add_argument(
        "--window-rows",
        type=_positive,
        default=DEFAULT_WINDOW_ROWS,
        help="rows read per window from each slab (default {:,})".format(DEFAULT_WINDOW_ROWS),
    )
    conv.add_argument(
        "--only-files",
        default=None,
        help="convert only these file numbers: a comma-separated list of numbers and "
        "inclusive ranges, e.g. 0,3-5",
    )
    return parser


def cmd_plan(args) -> int:
    dataset = HorizontalDataset(args.input)
    plan = make_file_plan(
        dataset, args.halo_properties, args.max_halos_per_file, args.max_forests_per_file
    )
    path = write_file_plan(args.output, plan)
    files = plan["files"]
    print("wrote {}".format(path))
    print(
        "dataset: {} halos in {} forests over {} slabs".format(
            plan["dataset"]["total_halos"],
            plan["dataset"]["n_forests_total"],
            len(dataset.snapshots),
        )
    )
    print(
        "caps: {} halos and {} forests per file".format(
            plan["caps"]["max_halos_per_file"], plan["caps"]["max_forests_per_file"]
        )
    )
    print("files: {}".format(len(files)))
    if files:
        largest = max(files, key=lambda entry: (entry["n_halos"], -entry["number"]))
        print(
            "largest file: {}, {} halos in {} forests".format(
                largest["number"], largest["n_halos"], largest["n_forests"]
            )
        )
    return EXIT_OK


def cmd_convert(args, argv: Sequence[str]) -> int:
    only_files = None if args.only_files is None else parse_file_selection(args.only_files)
    return convert(
        args.input,
        args.halo_properties,
        args.output,
        args.tree_name,
        workers=args.workers,
        window_rows=args.window_rows,
        only_files=only_files,
        command_line=["write_vertical_trees.py"] + list(argv),
    )


def main(argv: Optional[Sequence[str]] = None) -> int:
    argv = list(sys.argv[1:] if argv is None else argv)
    args = build_parser().parse_args(argv)
    try:
        if args.command == "plan":
            return cmd_plan(args)
        return cmd_convert(args, argv)
    except (ConverterError, OSError) as exc:
        print("ERROR: {}".format(exc), file=sys.stderr)
        return EXIT_REFUSED


if __name__ == "__main__":
    sys.exit(main())
