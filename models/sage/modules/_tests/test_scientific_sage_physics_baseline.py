#!/usr/bin/env python3
"""
SAGE full-physics galaxy-output baseline regression.

The repository-level baseline (tests/data/output/baseline/) is intentionally
physics-free: it validates deterministic halo tracking and selected
simulation/catalog fields, and is shared by every model package. It therefore
cannot catch drift in galaxy-formation output.

This model-owned test closes that gap for SAGE. It runs the complete SAGE
physics pipeline (models/sage/modules/_tests/input/test_physics_binary.yaml) and compares ALL
output properties -- core halo tracking AND baryonic galaxy properties
(StellarMass, ColdGas, BlackHoleMass, ...) -- against a committed reference
captured from a known-good build. This is the safety net that proves
behaviour-preserving refactors (e.g. the named substep phase pipeline) do not
alter SAGE science output.

Comparison: every common property for every halo, 1e-6 relative tolerance for
floats, exact for integers (reusing the integration-tier comparator).

If this test fails after a DELIBERATE, validated science change, refresh the
reference:
    ./mimic models/sage/modules/_tests/input/test_physics_binary.yaml
    cp tests/data/output/physics-binary/model_z0.000_0 \\
       models/sage/modules/_tests/baseline/physics-binary/
    cp tests/data/output/physics-binary/metadata/output_schema.json \\
       models/sage/modules/_tests/baseline/physics-binary/metadata/
and document why in the commit message.
"""

import io
import sys
from contextlib import redirect_stdout
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[4]
sys.path.insert(0, str(REPO_ROOT / "tests"))
sys.path.insert(0, str(REPO_ROOT / "tests" / "integration"))

from framework import (
    BASELINE_ATOL_DEFAULT,
    BASELINE_RTOL_DEFAULT,
    MIMIC_EXE,
    baseline_rtol,
    compiled_model,
    load_binary_halos,
    run_mimic_fresh,
)
from test_output_formats import compare_halos_comprehensive

BLUE = "\033[1;34m"
GREEN = "\033[0;32m"
RED = "\033[0;31m"
YELLOW = "\033[1;33m"
NC = "\033[0m"

INPUT = REPO_ROOT / "models" / "sage" / "modules" / "_tests" / "input" / "test_physics_binary.yaml"
CURRENT = REPO_ROOT / "tests" / "data" / "output" / "physics-binary" / "model_z0.000_0"
BASELINE = (
    REPO_ROOT
    / "models"
    / "sage"
    / "modules"
    / "_tests"
    / "baseline"
    / "physics-binary"
    / "model_z0.000_0"
)


def test_sage_physics_baseline():
    """Current full-physics SAGE output matches the committed baryonic baseline."""
    print("Testing SAGE full-physics galaxy-output baseline...")

    if compiled_model() != "sage":
        print(f"  Skipping (MODEL={compiled_model()}, this baseline is SAGE-specific)")
        return

    if not MIMIC_EXE.exists():
        print("  Skipping (Mimic not built)")
        return

    assert INPUT.exists(), f"Full-physics input not found: {INPUT}"
    assert BASELINE.exists(), (
        f"{RED}Committed baseline not found: {BASELINE}\n"
        f"Generate it once and commit it (see this file's docstring).{NC}"
    )

    # Always regenerate so a stale file cannot satisfy the comparison.
    run_mimic_fresh(INPUT, CURRENT)

    halos_now, meta_now = load_binary_halos(CURRENT)
    halos_ref, meta_ref = load_binary_halos(BASELINE)
    print(f"  current:  {meta_now['TotHalos']} halos")
    print(f"  baseline: {meta_ref['TotHalos']} halos")

    assert meta_now["TotHalos"] == meta_ref["TotHalos"], (
        f"{RED}Halo count mismatch: current={meta_now['TotHalos']}, "
        f"baseline={meta_ref['TotHalos']}{NC}"
    )

    assert halos_now.dtype.names == halos_ref.dtype.names, (
        f"{RED}Output property schema mismatch.\n"
        f"Only in current: {sorted(set(halos_now.dtype.names) - set(halos_ref.dtype.names))}\n"
        f"Only in baseline: {sorted(set(halos_ref.dtype.names) - set(halos_now.dtype.names))}{NC}"
    )

    # Compare every property (core + baryonic) for every halo.
    passed, report = compare_halos_comprehensive(
        halos_now,
        halos_ref,
        label1="current",
        label2="baseline",
        rtol=baseline_rtol(),
        warn_rtol=BASELINE_RTOL_DEFAULT,
        atol=BASELINE_ATOL_DEFAULT,
    )
    print(report, end="")

    assert passed, (
        f"{RED}SAGE full-physics output does not match the committed baseline.\n"
        f"A behaviour-preserving change must reproduce galaxy output exactly.\n"
        f"If this was a deliberate science change, refresh the baseline "
        f"(see this file's docstring).{NC}"
    )
    print(f"{GREEN}  ✓ SAGE galaxy-physics output matches committed baseline{NC}")


def main():
    print(f"{BLUE}{'=' * 60}{NC}")
    print(
        f"{BLUE}Test Suite: SAGE Full-Physics Baseline Tests (test_scientific_sage_physics_baseline.py){NC}"
    )
    print(f"{BLUE}{'=' * 60}{NC}")
    print()
    print(f"Repository root: {REPO_ROOT}")
    print(f"Mimic executable: {MIMIC_EXE}")

    if not MIMIC_EXE.exists():
        print(f"{RED}ERROR: Mimic executable not found: {MIMIC_EXE}{NC}")
        print("Build it first with: make")
        return 1

    tests = [
        test_sage_physics_baseline,
    ]

    passed = 0
    failed = 0
    skipped = 0

    for test in tests:
        print()
        try:
            output_buffer = io.StringIO()
            with redirect_stdout(output_buffer):
                test()

            output = output_buffer.getvalue()
            print(output, end="")

            if "Skipping" in output:
                print(f"{YELLOW}⊘ SKIP: {test.__name__}{NC}")
                skipped += 1
            else:
                passed += 1
        except AssertionError as e:
            output = output_buffer.getvalue()
            if output:
                print(output, end="")
            print(f"{RED}✗ FAIL: {test.__name__}{NC}")
            print(f"  {e}")
            failed += 1
        except Exception as e:
            output = output_buffer.getvalue()
            if output:
                print(output, end="")
            print(f"{RED}✗ ERROR: {test.__name__}{NC}")
            print(f"  {e}")
            failed += 1

    print()
    print(f"{BLUE}{'=' * 60}{NC}")
    print(f"{BLUE}Test Summary{NC}")
    print(f"{BLUE}{'=' * 60}{NC}")
    print(f"Passed:  {passed}")
    print(f"Failed:  {failed}")
    print(f"Skipped: {skipped}")
    print(f"Total:   {passed + failed + skipped}")
    print(f"{BLUE}{'=' * 60}{NC}")
    print()

    if failed == 0:
        print(f"{GREEN}✓ All tests passed!{NC}")
        if skipped > 0:
            print(f"{YELLOW}⊘ {skipped} test(s) skipped{NC}")
        return 0
    else:
        print(f"{RED}✗ {failed} test(s) failed{NC}")
        return 1


if __name__ == "__main__":
    sys.exit(main())
