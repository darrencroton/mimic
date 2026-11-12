#!/usr/bin/env python3
"""
sage16 registered figures, rendered from a synthetic catalogue (models/sage16/plots/figures/).

Validates: the generic cases of tests/framework/plot_checks.py for each of the 22 registered
           figures: it renders a non-empty PNG and returns no skip message; every field it reads
           unconditionally is in its PLOT_REQUIREMENTS entry (and, for an evolution figure, in its
           EVOLUTION_PLOT_FIELDS entry) and it runs without any one of its optional fields.
           sage16's figures tag no model artist, so there is no content check of the rendered
           figure beyond the PNG: whether each plots the right quantity is the derivation cases'
           job
           the derivation cases of sage16_plot_derivations.py: a hand-built catalogue per figure
           whose plotted points are known literals (mass-function normalisation by volume, h^3 and
           bin width, the red/blue split, the SFR and stellar-mass densities, the Sb/c selection,
           the metallicity conversion)
Tolerance: none for field sets and selections; plotted points of the derivation cases to a
           relative 1e-5 (float32 input)

sage26's figures are copies of these with a few additions and keep their own copy of the rows.
Figures render with MPLBACKEND=Agg into a temporary directory under $TMPDIR, removed at the end. It
needs no Mimic run.

Run: mimic_venv/bin/python models/sage16/plots/tests/test_sage16_figures.py
"""

import os
import sys
from pathlib import Path

# Set before Matplotlib is imported (the figure package imports it)
os.environ["MPLBACKEND"] = "Agg"

HERE = Path(__file__).resolve().parent
REPO_ROOT = HERE.parents[3]
sys.path.insert(0, str(REPO_ROOT / "tests"))
sys.path.insert(0, str(HERE))

import sage16_plot_fixtures as fixtures
from framework.plot_checks import PlotHarness
from framework.plot_derivations import derivation_cases
from sage16_plot_derivations import DERIVATIONS, INTEGER_FIELDS


def main():
    """Run the shared cases and the derivation cases in a temporary directory."""
    harness = PlotHarness("sage16", fixtures, model_gid=False)
    cases = harness.registry_cases()
    cases += derivation_cases(harness, DERIVATIONS, INTEGER_FIELDS)
    return harness.run(cases, "sage16 figures (test_sage16_figures.py)")


if __name__ == "__main__":
    sys.exit(main())
