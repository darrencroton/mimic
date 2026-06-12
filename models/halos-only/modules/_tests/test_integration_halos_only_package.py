#!/usr/bin/env python3
"""Integration checks for the halos-only model package."""

import shutil
import sys
from pathlib import Path

import yaml

REPO_ROOT = Path(__file__).resolve().parents[4]
sys.path.insert(0, str(REPO_ROOT / "tests"))

from framework import (
    MIMIC_EXE,
    TestSkipped,
    create_test_param_file,
    result_error,
    result_fail,
    result_pass,
    result_skip,
    run_mimic,
)

MODEL_ROOT = REPO_ROOT / "models" / "halos-only"


def test_model_properties_are_empty():
    data = yaml.safe_load((MODEL_ROOT / "model_properties.yaml").read_text(encoding="utf-8"))
    assert data == {"galaxy_properties": []}


def test_user_run_file_has_empty_pipeline():
    data = yaml.safe_load(
        (MODEL_ROOT / "input" / "halos-only_mini-millennium.yaml").read_text(encoding="utf-8")
    )
    assert data["model"]["name"] == "halos-only"
    assert data["model"]["path"] == "models/halos-only"

    modules = data.get("modules") or {}
    active = {
        key: modules.get(key)
        for key in ("pre_timestep", "phases", "post_timestep")
        if modules.get(key)
    }
    assert active == {}


def test_runtime_reports_physics_free_mode():
    if not MIMIC_EXE.exists():
        raise TestSkipped("Mimic not built")

    param_file, output_dir, temp_dir = create_test_param_file(
        output_name="halos_only_physics_free",
        first_file=0,
        last_file=0,
    )

    try:
        returncode, stdout, stderr = run_mimic(param_file)
        assert returncode == 0, stderr
        assert "No modules configured (physics-free mode)" in stdout
        assert output_dir.exists()
    finally:
        shutil.rmtree(temp_dir)


def main():
    tests = [
        test_model_properties_are_empty,
        test_user_run_file_has_empty_pipeline,
        test_runtime_reports_physics_free_mode,
    ]

    failed = 0
    skipped = 0
    for test in tests:
        try:
            test()
            result_pass(test.__name__)
        except TestSkipped as exc:
            result_skip(test.__name__, str(exc))
            skipped += 1
        except AssertionError as exc:
            result_fail(test.__name__, str(exc).splitlines()[0])
            failed += 1
        except Exception as exc:
            result_error(test.__name__, str(exc).splitlines()[0])
            failed += 1

    print(f"Passed: {len(tests) - failed - skipped}")
    if skipped:
        print(f"Skipped: {skipped}")
    print(f"Failed: {failed}")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
