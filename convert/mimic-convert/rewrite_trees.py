"""Rewrite a horizontal-HDF5 dataset of a Consistent-Trees ASCII catalogue.

Three operations over one input dataset directory, each writing a version 3
dataset into a converter-shaped workdir (``convert/mimic-convert/rewriter``):

  migrate   a version 2 input becomes version 3: identities computed from the
            version 2 columns, rows at their (ForestIndex, HaloRankInForest)
            position, int64 links with target-snapshot companions, /schema
            from the package's converter profile, sidecar ordinals from the
            index files and the version 2 conversion's recorded tree-file order
  noop      a version 3 input is reproduced exactly (every dataset value,
            dtype, header attribute, /schema attribute and sidecar value) under
            the identity table: the passes' permanent self-test
  cut       a version 3 input is rewritten under a declared cut forest table
            (--cut-table, forests.list-shaped: tree root to forest id): every
            tree joins the table's forest, a cut forest's largest piece keeps
            its id and the fresh pieces take ids above the catalogue's maximum,
            contiguous from it, enumerated after every original forest; every
            halo whose FoF central lands in another forest becomes a central;
            FoF chains, ranks and progenitor chain order are recomputed for the
            forests the table changes and every other forest is reproduced
            exactly; the table's md5 is bound into the manifest and the resume
            state, and --cut-table-record (the census's JSON record of the
            table) supplies the rule text and the dataset lineage

Every location is explicit: ``--input`` (the dataset directory), ``--output``
(the workdir to create: manifest.json, dataset/, tree_inventory.npy,
rewrite_state.json, rewrite_summary.json) and ``--scratch`` (pass 1's remap
entries and checkpoints); nothing defaults into the repository. The a_list,
the simulation metadata and the two index files (``forests.list`` and
``locations.dat``) are always named. ``--column-map`` is required for migrate
and refused for noop and cut; ``--conversion-manifest`` (migrate only) is the
version 2 conversion's ``manifest.json``, required when ``locations.dat`` names
more than one tree file, since the sidecar's file ordinals follow the order the
converter was given the files, which ``locations.dat``'s FileID does not record.

Refusals happen before any write and exit 2 naming the offending value: an
input that is not version 2 (migrate) or 3 (noop, cut), ``links_adjacent`` 0,
a version 3 input of another source format, missing or inconsistent index
files, an a_list that is not the input's, a multiplier that cannot encode the
output's bounds (the installed table's for cut), a missing or ambiguous
tree-file order, terminal halos that do not correspond to the index files'
tree roots, a cut table violating any of its invariants, a cut piece held in
one file whose FileID the input sidecar gives no conversion ordinal for, and a
record that is not the given table's or index files'. Two refusals of a cut
are made per slab once the workdir exists: a halo whose FoF central lies in
another forest of the input, and stored progenitor chains that are not the
reference insertion loop's over the input's own keys.

``--resume`` continues an interrupted run after its last verified slab; it
refuses a changed input or configuration, and re-verifies a completed run.
The written workdir validates with
``convert_trees.py validate --workdir <output>`` and ``report``.

Usage (micro-Uchuu, one tree file, so no conversion manifest is needed):

    mimic_venv/bin/python convert/mimic-convert/rewrite_trees.py migrate \\
        --input /data/micro-uchuu-ascii-horizontal-v2 \\
        --output /data/micro-uchuu-v3-migrated --scratch /data/scratch/migrate \\
        --simulation-info simulations/micro-uchuu-ascii-horizontal/simulation_info.yaml \\
        --a-list simulations/micro-uchuu-ascii-horizontal/micro-uchuu.a_list \\
        --forests-list simulations/micro-uchuu-ascii/snapshots/forests.list \\
        --locations simulations/micro-uchuu-ascii/snapshots/locations.dat \\
        --column-map simulations/micro-uchuu-ascii/converter_columns.yaml \\
        --multiplier 1000000000
"""

import argparse
import os
import sys
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from errors import ConverterError  # noqa: E402
from rewriter import OPERATION_CUT, OPERATION_MIGRATE, OPERATION_NOOP  # noqa: E402
from rewriter.inputs import RewriteRequest  # noqa: E402
from subset import SubsetError  # noqa: E402
from validate import DEFAULT_MULTIPLIER  # noqa: E402

#: The exit status of every refusal and verification failure.
EXIT_REFUSED = 2


def _add_common(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--input", required=True, help="the input dataset directory")
    parser.add_argument(
        "--output", required=True, help="the workdir to create (manifest.json, dataset/, ...)"
    )
    parser.add_argument(
        "--scratch", required=True, help="an empty directory for the remap store and checkpoints"
    )
    parser.add_argument(
        "--simulation-info", required=True, help="the package's simulation_info.yaml"
    )
    parser.add_argument("--a-list", required=True, help="the package's a_list")
    parser.add_argument("--forests-list", required=True, help="the catalogue's forests.list")
    parser.add_argument("--locations", required=True, help="the catalogue's locations.dat")
    parser.add_argument(
        "--multiplier",
        type=int,
        default=DEFAULT_MULTIPLIER,
        help="UniqueGalaxyID multiplier the output must satisfy (default {})".format(
            DEFAULT_MULTIPLIER
        ),
    )
    parser.add_argument(
        "--resume",
        action="store_true",
        help="continue the run in --output after its last verified slab",
    )
    parser.add_argument(
        "--column-map",
        default=None,
        help="the package's consistent_trees_ascii converter profile (migrate: required; noop: refused)",
    )
    parser.add_argument(
        "--conversion-manifest",
        default=None,
        help="the version 2 conversion's manifest.json recording the tree-file order (migrate "
        "only; required when locations.dat names several files)",
    )


def build_arg_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="rewrite_trees",
        description="Rewrite a horizontal-HDF5 dataset of a Consistent-Trees ASCII catalogue "
        "into a version 3 dataset: migrate (version 2 in), noop (version 3 in, the "
        "identity) or cut (version 3 in, under a cut forest table). Every location is "
        "explicit; refusals exit 2.",
    )
    sub = parser.add_subparsers(dest="operation", required=True)
    migrate = sub.add_parser(OPERATION_MIGRATE, help="version 2 in, version 3 out")
    _add_common(migrate)
    noop = sub.add_parser(
        OPERATION_NOOP, help="version 3 in, version 3 out under the identity table"
    )
    _add_common(noop)
    cut = sub.add_parser(OPERATION_CUT, help="version 3 in, version 3 out under a cut table")
    _add_common(cut)
    cut.add_argument(
        "--cut-table",
        required=True,
        help="the cut forest table (forests.list-shaped: tree root id, forest id per tree)",
    )
    cut.add_argument(
        "--cut-table-record",
        default=None,
        help="the table's JSON record (the census's record.json): its rule text and dataset "
        "lineage go into the manifest; it must name this table's md5 and the index files'",
    )
    return parser


def request_from_args(args) -> RewriteRequest:
    return RewriteRequest(
        operation=args.operation,
        input_dir=Path(args.input),
        output_dir=Path(args.output),
        scratch_dir=Path(args.scratch),
        simulation_info=Path(args.simulation_info),
        a_list=Path(args.a_list),
        forests_list=Path(args.forests_list),
        locations=Path(args.locations),
        column_map=None if args.column_map is None else Path(args.column_map),
        conversion_manifest=(
            None if args.conversion_manifest is None else Path(args.conversion_manifest)
        ),
        multiplier=args.multiplier,
        resume=bool(args.resume),
        cut_table=None if getattr(args, "cut_table", None) is None else Path(args.cut_table),
        cut_table_record=(
            None if getattr(args, "cut_table_record", None) is None else Path(args.cut_table_record)
        ),
    )


def main(argv=None) -> int:
    args = build_arg_parser().parse_args(argv)
    from rewriter.run import run_rewrite

    try:
        summary = run_rewrite(request_from_args(args))
    except (ConverterError, SubsetError, OSError) as exc:
        print("ERROR: {}".format(exc), file=sys.stderr)
        return EXIT_REFUSED
    print(
        "{}: COMPLETE -- {} snapshot file(s), {} halos, {} forests in {}/dataset".format(
            args.operation,
            summary["n_snapshots"],
            summary["n_halos"],
            summary["n_forests_total"],
            summary["output"],
        )
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
