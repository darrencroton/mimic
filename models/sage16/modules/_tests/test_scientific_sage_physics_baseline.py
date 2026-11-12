#!/usr/bin/env python3
"""
SAGE full-physics galaxy-output baseline regression, and the validity of full-physics output.

The repository-level baseline (tests/data/output/baseline/) is intentionally
physics-free: it validates deterministic halo tracking and selected
simulation/catalog fields, and is shared by every model package. It therefore
cannot catch drift in galaxy-formation output. The core scientific tier checks
finite values and declared ranges, but on a run file whose module pipeline is
empty.

This model-owned test closes both gaps for SAGE. It runs the complete SAGE
physics pipeline (models/sage16/modules/_tests/input/test_physics_binary.yaml)
once and:
  - compares ALL output properties -- core halo tracking AND baryonic galaxy
    properties (StellarMass, ColdGas, BlackHoleMass, ...) -- against a committed
    reference captured from a known-good build. This is the safety net that proves
    behaviour-preserving refactors (e.g. the named substep phase pipeline) do not
    alter SAGE science output
  - checks that the same output holds no NaN or Inf and no value outside the
    `range:` its property metadata declares, for every output property

Test cases:
  - test_input_matches_run_file: the input's SubSteps and modules block equal the
    package's vertical run file's, value for value and in order, so the baseline
    pins the pipeline the package ships and a run-file change fails here until the
    input and the baseline are refreshed with it
  - test_sage_physics_baseline: the fresh run matches the committed baseline; its
    output field names are the baseline's followed by the test fixtures' output
    fields (the tier runs a test build, which appends them; see
    fixture_output_field_names in tests/framework/harness.py)
  - test_output_names_reject_doctored_production_fields: that name check fails when
    a production field is added, removed or renamed, shown on doctored name lists
  - test_physics_output_finite_and_in_range: the same fresh run (it is not repeated)
    holds no NaN or Inf and no value outside its declared range, sentinels
    excluded exactly as the core scientific tier does; properties with no declared
    range are skipped with a printed notice. A violation is a finding about the
    physics or its metadata, to be explained, never absorbed by widening a range
    or exempting a property

Comparison: every common property for every halo, 1e-6 relative tolerance for
floats, exact for integers (reusing the integration-tier comparator).

Range tolerance: declared ranges are inclusive and checked with an absolute slack
of RANGE_ATOL (1e-15 in each property's own units). SAGE's single-precision
reservoir updates subtract metallicity * mass from a metal reservoir, which can
leave a rounding residue just below zero as the reservoir empties. Measured on
this run: MetalsHotGas in 601 of 4195 rows at snapshot 63, the most negative
-2.7e-21 (1e10 Msun/h), against a declared lower bound of 0; no other property is
affected. The slack is set from that residue, a few orders above it, so it absorbs
rounding of this kind and nothing a module could form: 1e-15 is 1e-5 Msun/h.

If the baseline case fails after a DELIBERATE, validated science change, refresh
the reference. If the run file changed, first copy its SubSteps and modules block
into the input (models/sage16/modules/_tests/input/test_physics_binary.yaml).
Capture it with an ordinary build (make), not a test build, which appends the test
fixtures' output fields. Then:
    ./mimic models/sage16/modules/_tests/input/test_physics_binary.yaml
    cp tests/data/output/physics-binary/model_z0.000_0 \\
       models/sage16/modules/_tests/baseline/physics-binary/
    cp tests/data/output/physics-binary/metadata/output_schema.json \\
       models/sage16/modules/_tests/baseline/physics-binary/metadata/
and document why in the commit message.
"""

import sys
from pathlib import Path

import yaml

REPO_ROOT = Path(__file__).resolve().parents[4]
sys.path.insert(0, str(REPO_ROOT / "tests"))

from framework import (  # noqa: E402  (path set up above)
    BASELINE_ATOL_DEFAULT,
    BASELINE_RTOL_DEFAULT,
    GREEN,
    MIMIC_EXE,
    NC,
    RED,
    VALIDATION_MANIFEST_PATH,
    YELLOW,
    TestSkipped,
    baseline_rtol,
    compare_halos_comprehensive,
    compiled_model,
    find_validity_violations,
    format_validity_finding,
    is_default_baseline_combo,
    load_binary_halos,
    load_validation_manifest,
    not_applicable_to_non_default_baseline,
    run_mimic_fresh,
    run_test_suite,
)
from framework.harness import fixture_output_field_names  # noqa: E402

INPUT = (
    REPO_ROOT / "models" / "sage16" / "modules" / "_tests" / "input" / "test_physics_binary.yaml"
)
CURRENT = REPO_ROOT / "tests" / "data" / "output" / "physics-binary" / "model_z0.000_0"
BASELINE = (
    REPO_ROOT
    / "models"
    / "sage16"
    / "modules"
    / "_tests"
    / "baseline"
    / "physics-binary"
    / "model_z0.000_0"
)

RUN_FILE = REPO_ROOT / "models" / "sage16" / "input" / "sage16_mini-millennium.yaml"

# Absolute slack on declared ranges; see "Range tolerance" in the module docstring
RANGE_ATOL = 1e-15

# Set once the input has been run in this process, so the cases share one fresh run
_fresh_run_done = False


def _require_runnable():
    """Skip or mark not applicable unless this baseline's package pair is built and ready."""
    if compiled_model() != "sage16":
        raise TestSkipped(f"MODEL={compiled_model()}, this baseline is SAGE-specific")
    if not is_default_baseline_combo():
        not_applicable_to_non_default_baseline()

    if not MIMIC_EXE.exists():
        raise TestSkipped("Mimic not built")


def _fresh_output():
    """Run the input once per process, always regenerating, and return the output path."""
    global _fresh_run_done
    if not _fresh_run_done:
        assert INPUT.exists(), f"Full-physics input not found: {INPUT}"
        # Always regenerate so a stale file cannot satisfy a check.
        run_mimic_fresh(INPUT, CURRENT)
        _fresh_run_done = True
    return CURRENT


def _ordered(node):
    """A YAML node with mapping order made significant, so phase and module order compare."""
    if isinstance(node, dict):
        return [(key, _ordered(value)) for key, value in node.items()]
    if isinstance(node, list):
        return [_ordered(value) for value in node]
    return node


def _check_output_names(names_now, names_ref, fixture_names):
    """Assert the current output names are the baseline's followed by the fixture outputs.

    A test build appends the test fixtures' output fields after every production
    field, so any other difference (a production field added, removed, renamed or
    moved) is a schema change the baseline does not cover.
    """
    expected = tuple(names_ref) + tuple(fixture_names)
    assert tuple(names_now) == expected, (
        f"{RED}Output property schema mismatch (expected the baseline's fields followed by "
        f"the test fixtures' output fields {list(fixture_names)}).\n"
        f"{_name_difference(names_now, expected)}{NC}"
    )


def _name_difference(names_now, expected):
    """Describe how two name lists differ: the set differences, or the first differing position.

    When both lists hold the same names, the set differences are empty, so the
    first position where the order differs is named instead.
    """
    only_now = sorted(set(names_now) - set(expected))
    only_expected = sorted(set(expected) - set(names_now))
    if only_now or only_expected:
        return f"Only in current: {only_now}\nOnly in expected: {only_expected}"
    names_now, expected = tuple(names_now), tuple(expected)
    index = next(
        (i for i, (now, want) in enumerate(zip(names_now, expected, strict=False)) if now != want),
        min(len(names_now), len(expected)),
    )
    current = names_now[index] if index < len(names_now) else "<end of list>"
    wanted = expected[index] if index < len(expected) else "<end of list>"
    return (
        f"Same names in a different order: first difference at position {index}, "
        f"current {current!r}, expected {wanted!r}"
    )


def test_input_matches_run_file():
    """The input's SubSteps and modules block equal the package run file's, in order."""
    assert INPUT.exists(), f"Full-physics input not found: {INPUT}"
    assert RUN_FILE.exists(), f"Package run file not found: {RUN_FILE}"
    with open(INPUT, encoding="utf-8") as handle:
        test_input = yaml.safe_load(handle)
    with open(RUN_FILE, encoding="utf-8") as handle:
        run_file = yaml.safe_load(handle)

    for key in ("SubSteps", "modules"):
        assert key in test_input, f"input {INPUT.name} has no '{key}'"
        assert _ordered(test_input[key]) == _ordered(run_file[key]), (
            f"{RED}input '{key}' differs from {RUN_FILE.name}: refresh the input and the "
            f"baseline together (see this file's docstring){NC}"
        )
    print(
        f"  SubSteps={run_file['SubSteps']}; modules block equal "
        f"({len(run_file['modules']['parameters'])} parameters, "
        f"phases {list(run_file['modules']['phases'])})"
    )


def test_sage_physics_baseline():
    """Current full-physics SAGE output matches the committed baryonic baseline."""
    print("Testing SAGE full-physics galaxy-output baseline...")
    _require_runnable()

    assert INPUT.exists(), f"Full-physics input not found: {INPUT}"
    assert BASELINE.exists(), (
        f"{RED}Committed baseline not found: {BASELINE}\n"
        f"Generate it once and commit it (see this file's docstring).{NC}"
    )

    current = _fresh_output()
    halos_now, meta_now = load_binary_halos(current)
    halos_ref, meta_ref = load_binary_halos(BASELINE)
    print(f"  current:  {meta_now['TotHalos']} halos")
    print(f"  baseline: {meta_ref['TotHalos']} halos")

    assert meta_now["TotHalos"] == meta_ref["TotHalos"], (
        f"{RED}Halo count mismatch: current={meta_now['TotHalos']}, "
        f"baseline={meta_ref['TotHalos']}{NC}"
    )

    _check_output_names(halos_now.dtype.names, halos_ref.dtype.names, fixture_output_field_names())

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


def test_output_names_reject_doctored_production_fields():
    """The name check fails when a production field is added, removed, renamed or moved."""
    assert BASELINE.exists(), f"Committed baseline not found: {BASELINE}"
    names_ref = tuple(load_binary_halos(BASELINE)[0].dtype.names)
    fixture = fixture_output_field_names()
    assert len(names_ref) >= 2 and fixture, "need production and fixture names to doctor"

    _check_output_names(names_ref + fixture, names_ref, fixture)

    middle = len(names_ref) // 2
    renamed = names_ref[:middle] + (names_ref[middle] + "Renamed",) + names_ref[middle + 1 :]
    swapped = (
        names_ref[: middle - 1]
        + (names_ref[middle], names_ref[middle - 1])
        + names_ref[middle + 1 :]
    )
    position = (
        f"first difference at position {middle - 1}, current {names_ref[middle]!r}, "
        f"expected {names_ref[middle - 1]!r}"
    )
    # (label, doctored current names, a fragment the failure message must hold)
    doctored = [
        ("production field added", names_ref + ("DoctoredField",) + fixture, "DoctoredField"),
        (
            "production field removed",
            names_ref[:middle] + names_ref[middle + 1 :] + fixture,
            repr(names_ref[middle]),
        ),
        ("production field renamed", renamed + fixture, names_ref[middle] + "Renamed"),
        ("production fields reordered", swapped + fixture, position),
        # The message's first line already lists the fixture names, so check the
        # difference part, which lists the missing names sorted.
        ("fixture fields missing", names_ref, f"Only in expected: {sorted(fixture)}"),
    ]
    for label, names_now, fragment in doctored:
        try:
            _check_output_names(names_now, names_ref, fixture)
        except AssertionError as exc:
            assert fragment in str(exc), f"{label}: failure does not name {fragment}: {exc}"
            continue
        raise AssertionError(f"{label}: the name check accepted a doctored name list")
    print(f"  {len(doctored)} doctored name lists rejected; the true list accepted")


def test_physics_output_finite_and_in_range():
    """Fresh full-physics output has no NaN or Inf and no value outside its declared range."""
    _require_runnable()
    if not VALIDATION_MANIFEST_PATH.exists():
        raise TestSkipped("validation manifest not found: run 'make generate'")
    properties = load_validation_manifest().get("properties", {})

    halos, meta = load_binary_halos(_fresh_output())
    findings, skipped = find_validity_violations(halos, properties, range_atol=RANGE_ATOL)
    print(
        f"  {meta['TotHalos']} halos, {len(halos.dtype.names) - len(skipped)} of "
        f"{len(halos.dtype.names)} properties range-checked, {len(findings)} violation(s)"
    )
    for field in skipped:
        print(f"{YELLOW}  Skipping {field}: no range specified in metadata{NC}")
    for finding in findings:
        print(f"{RED}  ✗ {format_validity_finding(finding)}{NC}")

    assert not findings, (
        f"{len(findings)} non-finite or out-of-range finding(s) in SAGE full-physics output; "
        f"explain each before changing a range or a module:\n"
        + "\n".join(format_validity_finding(finding) for finding in findings)
    )
    print(f"{GREEN}  ✓ no NaN, Inf or out-of-range value in any output property{NC}")


def main():
    print(f"Repository root: {REPO_ROOT}")
    print(f"Mimic executable: {MIMIC_EXE}")
    return run_test_suite(
        [
            test_input_matches_run_file,
            test_sage_physics_baseline,
            test_output_names_reject_doctored_production_fields,
            test_physics_output_finite_and_in_range,
        ],
        "SAGE Full-Physics Baseline (test_scientific_sage_physics_baseline.py)",
    )


if __name__ == "__main__":
    raise SystemExit(main())
