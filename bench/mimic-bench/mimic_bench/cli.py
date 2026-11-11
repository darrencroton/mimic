"""Command-line entry point of the bench: ``mimic-bench.py <stage>``.

Stages:
    design          Write the design table of a spec: ``design --spec <spec> --out <file.csv>``.
    capture         Run the evaluations of a design with the ``./mimic`` of the repository the
                    bench lives in, verify each and record its counts:
                    ``capture --spec <spec> --sets <names> --out <dir> [--workers 8]
                    [--timeout-s 120] [--design <csv>]``. ``--sets`` is comma-separated and
                    required for the spec's design; with ``--design`` it filters the CSV's rows
                    by set label, and every row runs when it is omitted.
    select-outputs  Print the core and exploratory outputs the spec's rule gives for a pilot
                    capture, with the per-bin counts behind them: ``select-outputs --spec <spec>
                    --capture <dir>``. It writes nothing.
    analyse         Fit the candidate emulators to a capture, judge them on its holdout and
                    write the verdict and tables: ``analyse --spec <spec> --capture <dir>
                    --out <dir>``; exits 0, 1 or 2 for a verdict of pass, fail or error.

Exit status is 0 on success, 1 when a stage's verdict is a fail (stages that give one) and 2
on an error: an invalid spec, design or constraint table, an unreadable or altered file, a
usage mistake, an interrupted capture, or any unexpected exception (``error: internal error:``
and the traceback on stderr).
"""

from __future__ import annotations

import argparse
import sys
import traceback
from pathlib import Path
from typing import Sequence

from mimic_bench import __version__
from mimic_bench.analyse import AnalyseError, run_analyse
from mimic_bench.capture import (
    CaptureError,
    format_selection,
    run_capture,
    select_from_capture,
)
from mimic_bench.constraints import ConstraintError
from mimic_bench.design import DesignError, build_design, write_design
from mimic_bench.manifest import ManifestError
from mimic_bench.provenance import read_spec_inputs
from mimic_bench.runfile import RunFileError
from mimic_bench.spec import SpecError, load_spec
from mimic_bench.statistics import StatisticError

REPO_ROOT = Path(__file__).resolve().parents[3]
"""The Mimic repository this package lives in (``bench/mimic-bench/mimic_bench/cli.py`` is three
levels down); never the working directory."""

EXIT_OK = 0
EXIT_FAIL = 1
EXIT_ERROR = 2


def _run_design(args: argparse.Namespace) -> int:
    """Run the ``design`` stage: load and validate the spec, then write its design table."""
    spec = load_spec(args.spec)
    rows = build_design(spec)
    write_design(rows, spec, args.out)
    print(f"wrote {len(rows)} rows to {args.out}")
    return EXIT_OK


STAGE_ERRORS = (
    SpecError,
    ConstraintError,
    DesignError,
    ManifestError,
    RunFileError,
    StatisticError,
    CaptureError,
    AnalyseError,
)
"""Errors a stage reports as ``error: <message>`` and exit status 2."""


def _run_capture(args: argparse.Namespace) -> int:
    """Run the ``capture`` stage; a capture that cannot start exits 2 having written nothing."""
    sets = None
    if args.sets is not None:
        sets = [name.strip() for name in args.sets.split(",") if name.strip()]
    status = run_capture(
        args.spec,
        REPO_ROOT,
        sets,
        args.out,
        workers=args.workers,
        timeout_s=args.timeout_s,
        design_path=args.design,
    )
    verdict = "complete" if status == EXIT_OK else "FAILED (see runs.csv)"
    print(f"capture {verdict}: {args.out}")
    return status


def _run_select_outputs(args: argparse.Namespace) -> int:
    """Run the ``select-outputs`` stage: print the rule's result for a pilot capture."""
    selections = select_from_capture(read_spec_inputs(args.spec), args.capture)
    print(format_selection(selections), end="")
    return EXIT_OK


def _run_analyse(args: argparse.Namespace) -> int:
    """Run the ``analyse`` stage; an output directory that exists is refused, writing nothing."""
    return run_analyse(args.spec, args.capture, args.out)


def build_parser() -> argparse.ArgumentParser:
    """Build the argument parser, with one sub-command per stage."""
    parser = argparse.ArgumentParser(prog="mimic-bench.py", description=__doc__.split("\n")[0])
    parser.add_argument("--version", action="version", version=f"%(prog)s {__version__}")
    stages = parser.add_subparsers(dest="stage", required=True, metavar="<stage>")
    design = stages.add_parser("design", help="write the design table of a spec")
    design.add_argument("--spec", type=Path, required=True, help="campaign spec (YAML)")
    design.add_argument("--out", type=Path, required=True, help="design table to write (CSV)")
    design.set_defaults(handler=_run_design)
    capture = stages.add_parser("capture", help="run, verify and count a spec's evaluations")
    capture.add_argument("--spec", type=Path, required=True, help="campaign spec (YAML)")
    capture.add_argument(
        "--sets",
        help="comma-separated design sets, e.g. 'pilot' or 'train,holdout' (with --design: set "
        "labels to keep, all rows when omitted)",
    )
    capture.add_argument(
        "--out", type=Path, required=True, help="capture directory (must not exist)"
    )
    capture.add_argument("--workers", type=int, default=8, help="runs at once (default 8)")
    capture.add_argument(
        "--timeout-s", type=float, default=120.0, help="seconds one run may take (default 120)"
    )
    capture.add_argument(
        "--design", type=Path, help="design CSV to run instead of the spec's design"
    )
    capture.set_defaults(handler=_run_capture)
    select = stages.add_parser(
        "select-outputs", help="print the core and exploratory outputs of a pilot capture"
    )
    select.add_argument("--spec", type=Path, required=True, help="campaign spec (YAML)")
    select.add_argument("--capture", type=Path, required=True, help="pilot capture directory")
    select.set_defaults(handler=_run_select_outputs)
    analyse = stages.add_parser(
        "analyse", help="fit, judge and write the verdict for a train and holdout capture"
    )
    analyse.add_argument("--spec", type=Path, required=True, help="campaign spec (YAML)")
    analyse.add_argument("--capture", type=Path, required=True, help="capture directory")
    analyse.add_argument(
        "--out", type=Path, required=True, help="analysis directory (must not exist)"
    )
    analyse.set_defaults(handler=_run_analyse)
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    """Run a stage and return its exit status.

    Args:
        argv: The command line without the program name; ``sys.argv[1:]`` when None.

    Returns:
        The stage's status: 0 on success, 1 for a verdict of fail, 2 on any error, an
        unexpected exception included. A usage mistake exits 2 through argparse.
    """
    args = build_parser().parse_args(argv)
    try:
        return args.handler(args)
    except STAGE_ERRORS as exc:
        print(f"error: {exc}", file=sys.stderr)
    except OSError as exc:
        print(f"error: {exc.filename or ''}: {exc.strerror}", file=sys.stderr)
    except Exception:  # never 1, which means "fail" in this contract
        print(f"error: internal error:\n{traceback.format_exc()}", file=sys.stderr, end="")
    return EXIT_ERROR


if __name__ == "__main__":
    sys.exit(main())
