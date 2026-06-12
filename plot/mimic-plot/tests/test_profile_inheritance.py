#!/usr/bin/env python3
"""Unit tests for plot profile inheritance."""

import os
import runpy
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
MIMIC_PLOT_DIR = HERE.parent
REPO_ROOT = HERE.parent.parent.parent
sys.path.insert(0, str(MIMIC_PLOT_DIR))
sys.path.insert(0, str(REPO_ROOT / "tests"))

from framework import run_test_suite


def load_profile_helpers():
    """Load helpers from mimic-plot.py, whose filename is not importable."""

    return runpy.run_path(MIMIC_PLOT_DIR / "mimic-plot.py", run_name="__profile_test__")


def test_profile_local_inheritance():
    """Relative inherited profile paths resolve from the declaring profile."""

    helpers = load_profile_helpers()
    read_profile_file = helpers["read_profile_file"]

    previous_cwd = Path.cwd()
    with tempfile.TemporaryDirectory(prefix="mimic_profile_") as tmp:
        profile_dir = Path(tmp)
        (profile_dir / "base.yaml").write_text(
            "\n".join(
                [
                    "mode: base",
                    "plots:",
                    "  snapshot:",
                    "    - halo_mass_function",
                    "  evolution:",
                    "    - hmf_evolution",
                    "style:",
                    "  marker: circle",
                    "",
                ]
            )
        )
        (profile_dir / "child.yaml").write_text(
            "\n".join(
                [
                    "inherits:",
                    "  - base.yaml",
                    "mode: validation",
                    "plots:",
                    "  snapshot:",
                    "    - spin_distribution",
                    "",
                ]
            )
        )

        os.chdir("/")
        try:
            profile = read_profile_file(profile_dir / "child.yaml")
        finally:
            os.chdir(previous_cwd)

    assert profile["mode"] == "validation"
    assert profile["plots"]["snapshot"] == ["spin_distribution"]
    assert profile["plots"]["evolution"] == ["hmf_evolution"]
    assert profile["style"]["marker"] == "circle"


def main():
    """Run this file's tests via the shared framework runner."""
    return run_test_suite(
        [test_profile_local_inheritance],
        "Plot Profile Inheritance (test_profile_inheritance.py)",
    )


if __name__ == "__main__":
    sys.exit(main())
