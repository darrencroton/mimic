#!/usr/bin/env python3
"""
sage26 plot registry contract (models/sage26/plots/figures/__init__.py and profiles/).

Validates: SNAPSHOT_PLOTS and EVOLUTION_PLOTS hold no duplicate and no name in both; PLOT_FUNCS
           and PLOT_REQUIREMENTS hold exactly the registry's names; every evolution figure's
           EVOLUTION_PLOT_FIELDS entry includes its PLOT_REQUIREMENTS; every field in either table
           is an output property declared by sage26's model_properties.yaml,
           core_properties.yaml or the mini-Millennium halo_properties.yaml (a halo property also
           by the horizontal package's file); profiles/default.yaml lists the registry in order;
           and the profile stack the engine builds for the vertical and the horizontal run file
           lists the registry and agrees on `plots` and `axes`
Tolerance: none; every comparison is exact (lists, sets and dictionaries)

The cases live in tests/framework/plot_checks.py (PlotHarness.structure_cases) and read the
registry itself, so a figure added to the package is checked without a change here. This file
names the package, its horizontal halo properties and its two run files. It needs no Mimic run.

Run: mimic_venv/bin/python models/sage26/plots/tests/test_sage26_plot_registry.py
"""

import os
import sys
from pathlib import Path

# Matplotlib is imported when the figure package loads; keep it headless
os.environ["MPLBACKEND"] = "Agg"

HERE = Path(__file__).resolve().parent
REPO_ROOT = HERE.parents[3]
sys.path.insert(0, str(REPO_ROOT / "tests"))
sys.path.insert(0, str(HERE))

import sage26_plot_fixtures as fixtures
from framework.plot_checks import PlotHarness

INPUT_DIR = REPO_ROOT / "models" / "sage26" / "input"
RUN_FILES = {
    "vertical": INPUT_DIR / "sage26_mini-millennium.yaml",
    "horizontal": INPUT_DIR / "sage26_mini-millennium-horizontal.yaml",
}


def main():
    """Run the registry-contract cases through the shared runner."""
    harness = PlotHarness("sage26", fixtures, model_gid=True)
    cases = harness.structure_cases(RUN_FILES, fixtures.HORIZONTAL_HALO_PROPERTIES)
    return harness.run(cases, "sage26 plot registry (test_sage26_plot_registry.py)")


if __name__ == "__main__":
    sys.exit(main())
