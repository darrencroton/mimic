#!/usr/bin/env python3
"""Unit tests for plot profile inheritance."""

import os
import runpy
import sys
import tempfile
from pathlib import Path


HERE = Path(__file__).resolve().parent
MIMIC_PLOT_DIR = HERE.parent
sys.path.insert(0, str(MIMIC_PLOT_DIR))


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


def run_all_tests():
    """Run all profile inheritance tests."""

    tests = [test_profile_local_inheritance]
    passed = 0
    failed = 0

    print("=" * 60)
    print("Running plot profile inheritance unit tests")
    print("=" * 60)

    for test in tests:
        try:
            test()
            print(f"PASS: {test.__name__}")
            passed += 1
        except Exception as exc:
            print(f"FAIL: {test.__name__}: {exc}")
            failed += 1

    print(f"Results: {passed} passed, {failed} failed out of {len(tests)} tests")
    return failed == 0


if __name__ == "__main__":
    sys.exit(0 if run_all_tests() else 1)
