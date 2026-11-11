#!/usr/bin/env python

"""
Mimic Bench - evaluate a Mimic model over a parameter design, driven by a declared spec;
history matching with emulators is its first method

Usage:
  python bench/mimic-bench/mimic-bench.py <stage> [options]

Stages:
  design          Write the design table of a spec
                  design --spec <spec> --out <file.csv>
  capture         Run, verify and count a design's evaluations with this repository's ./mimic
                  capture --spec <spec> --sets <names> --out <dir> [--workers 8]
                          [--timeout-s 120] [--design <csv>]
  select-outputs  Print the core and exploratory outputs the spec selects for a pilot capture
                  select-outputs --spec <spec> --capture <dir>
  analyse         Fit the candidate emulators, judge them on the holdout, write the verdict
                  analyse --spec <spec> --capture <dir> --out <dir>

Options:
  --spec=<file>     Campaign spec (YAML), for example bench/mimic-bench/specs/sage16-smf.yaml
  --sets=<names>    Comma-separated design sets, for example 'pilot' or 'train,holdout'; with
                    --design, the set labels to keep (every row when omitted)
  --capture=<dir>   Capture directory written by the capture stage
  --out=<path>      Output file or directory (a directory must not exist)
  --workers=<n>     Runs at once during capture [default: 8]
  --timeout-s=<s>   Seconds one run may take during capture [default: 120]
  --design=<csv>    A design table to capture instead of the spec's (the columns `design` writes)
  --version         Show the version
  --help            Show this help message

Exit status: 0 on success or a verdict of pass, 1 on a verdict of fail, 2 on an error (an invalid
spec, design or constraint table, an unreadable or altered file, a usage mistake, an interrupted
capture, or an unexpected exception).
"""

import sys

# The script's own directory is on sys.path, so the package beside it is found.
from mimic_bench.cli import main

if __name__ == "__main__":
    sys.exit(main())
