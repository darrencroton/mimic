#!/usr/bin/env python3
"""
sage16 plot registry contract (models/sage16/plots/figures/__init__.py and profiles/).

Validates: SNAPSHOT_PLOTS and EVOLUTION_PLOTS hold no duplicate and no name in both; PLOT_FUNCS
           and PLOT_REQUIREMENTS hold exactly the registry's names; every evolution figure's
           EVOLUTION_PLOT_FIELDS entry includes its PLOT_REQUIREMENTS; every field in either table
           is an output property declared by sage16's model_properties.yaml,
           core_properties.yaml or the mini-Millennium halo_properties.yaml (a halo property also
           by the horizontal package's file); profiles/default.yaml lists the registry in order;
           and the profile stack the engine builds for the vertical and the horizontal run file
           lists the registry and agrees on `plots` and `axes`
Tolerance: none; every comparison is exact (lists, sets and dictionaries)

The cases live in tests/framework/plot_checks.py (PlotHarness.structure_cases) and read the
registry itself, so a figure added to the package is checked without a change here. This file
names the package, its horizontal halo properties and its two run files. It needs no Mimic run.

Run: mimic_venv/bin/python models/sage16/plots/tests/test_sage16_plot_registry.py
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

import sage16_plot_fixtures as fixtures
from framework.plot_checks import PlotHarness

INPUT_DIR = REPO_ROOT / "models" / "sage16" / "input"
RUN_FILES = {
    "vertical": INPUT_DIR / "sage16_mini-millennium.yaml",
    "horizontal": INPUT_DIR / "sage16_mini-millennium-horizontal.yaml",
}


def main():
    """Run the registry-contract cases through the shared runner."""
    harness = PlotHarness("sage16", fixtures, model_gid=False)
    # sage16's profile stack sets no axes of its own: each figure's built-in window applies
    cases = harness.structure_cases(
        RUN_FILES, fixtures.HORIZONTAL_HALO_PROPERTIES, require_axes=False
    )
    return harness.run(cases, "sage16 plot registry (test_sage16_plot_registry.py)")


if __name__ == "__main__":
    sys.exit(main())
