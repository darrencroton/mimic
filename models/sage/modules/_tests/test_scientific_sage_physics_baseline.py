#!/usr/bin/env python3
"""
SAGE full-physics galaxy-output baseline regression.

The repository-level baseline (tests/data/output/baseline/) is intentionally
physics-free: it validates deterministic halo tracking and selected
simulation/catalog fields, and is shared by every model package. It therefore
cannot catch drift in galaxy-formation output.

This model-owned test closes that gap for SAGE. It runs the complete SAGE
physics pipeline (models/sage/input/test_physics_binary.yaml) and compares ALL
output properties -- core halo tracking AND baryonic galaxy properties
(StellarMass, ColdGas, BlackHoleMass, ...) -- against a committed reference
captured from a known-good build. This is the safety net that proves
behaviour-preserving refactors (e.g. the named substep phase pipeline) do not
alter SAGE science output.

Comparison: every common property for every halo, 1e-6 relative tolerance for
floats, exact for integers (reusing the integration-tier comparator).

If this test fails after a DELIBERATE, validated science change, refresh the
reference:
    ./mimic models/sage/input/test_physics_binary.yaml
    cp tests/data/output/physics-binary/model_z0.000_0 \\
       models/sage/modules/_tests/baseline/physics-binary/
    cp tests/data/output/physics-binary/metadata/output_schema.json \\
       models/sage/modules/_tests/baseline/physics-binary/metadata/
and document why in the commit message.
"""

import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[4]
sys.path.insert(0, str(REPO_ROOT / "tests"))
sys.path.insert(0, str(REPO_ROOT / "tests" / "integration"))

from framework import load_binary_halos, MIMIC_EXE, compiled_model, run_mimic_fresh
from test_output_formats import compare_halos_comprehensive

GREEN = "\033[0;32m"
RED = "\033[0;31m"
NC = "\033[0m"

INPUT = REPO_ROOT / "models" / "sage" / "input" / "test_physics_binary.yaml"
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
        halos_now, halos_ref, label1="current", label2="baseline", rtol=1e-6
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
    try:
        test_sage_physics_baseline()
    except AssertionError as exc:
        print(f"{RED}✗ FAIL: {exc}{NC}")
        return 1
    print(f"{GREEN}✓ All tests passed!{NC}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
