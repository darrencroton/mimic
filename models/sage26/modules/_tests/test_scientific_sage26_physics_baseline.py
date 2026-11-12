#!/usr/bin/env python3
"""
sage26 full-physics galaxy-output baseline, and the validity of full-physics output.

Validates: the package's complete default pipeline, run on the committed single-file test trees,
           reproduces the committed galaxy-physics baseline: every property of every row at each
           output snapshot (63, and 16, the snapshot holding the most FFBRegime 1 rows); the
           input's SubSteps and modules block equal the package's vertical run file's, value for
           value and in order; the baseline covers the default pipeline's CGM-regime, FFB,
           concentration, FIRE-feedback and merger-path physics (FFBRegime 1 rows at snapshot 16;
           Regime 1 rows, non-zero Concentration, MassLoading and H2gas, satellites, merger bulges
           and quasar-mode accretion at 63); and the fresh output holds no NaN or Inf and no value
           outside the `range:` its property metadata declares
Tolerance: integers exact; floats at baseline_rtol() (1e-6 relative unless MIMIC_BASELINE_RTOL
           overrides it) with the absolute floor BASELINE_ATOL_DEFAULT (1e-10). Under a relaxed
           override, float differences beyond 1e-6 are reported as warnings. The baseline
           reproduces bit for bit on the platform it was captured on. Finite values and declared
           ranges are checked exactly (inclusive bounds, declared sentinels excluded)
Cross-platform floor: CI applies MIMIC_BASELINE_RTOL=1e-3 across platforms; locally on the
           capture platform the strict 1e-6 applies, and a difference beyond it there is a finding,
           not noise

The repository-level baseline (tests/data/output/baseline/) is physics-free and belongs to the
default package pair, so it cannot catch drift in sage26 galaxy formation. The core scientific
tier checks finite values and declared ranges, but on a run file whose module pipeline is empty.
This model-owned test closes both gaps for sage26. It guards itself with its own model and
simulation check rather than the framework's default-pair guard: the committed baseline belongs to
MODEL=sage26 SIMULATION=mini-millennium, which is not the Makefile's default pair, so that guard
would report NA under every sage26 pair.

Test cases:
  - test_input_matches_run_file: the input's SubSteps and modules block equal the run file's, so a
    deliberate run-file change fails here until the baseline is refreshed with it
  - test_sage26_physics_baseline: a fresh run of the input matches the committed baseline at
    every output snapshot, and the baseline covers the default pipeline's physics; the output
    field names are the baseline's followed by the test fixtures' output fields (the tier runs a
    test build, which appends them; see fixture_output_field_names in tests/framework/harness.py)
  - test_output_names_reject_doctored_production_fields: that name check fails when a
    production field is added, removed or renamed, shown on doctored name lists
  - test_physics_output_finite_and_in_range: the same fresh run (it is not repeated) holds no
    NaN or Inf and no value outside its declared range, in any output property at any output
    snapshot; properties with no declared range are skipped with a printed notice. A violation
    is a finding about the physics or its metadata, to be explained, never absorbed by widening
    a range or exempting a property

Run: make MODEL=sage26 SIMULATION=mini-millennium TEST_BUILD=yes && \\
     MODEL=sage26 SIMULATION=mini-millennium \\
     python3 models/sage26/modules/_tests/test_scientific_sage26_physics_baseline.py

If the baseline case fails after a DELIBERATE, validated change to sage26 output, refresh the
baseline. If the run file changed, first copy its SubSteps and modules block into the input
(models/sage26/modules/_tests/input/test_physics_binary.yaml). Capture the baseline with an
ordinary build (make MODEL=sage26 SIMULATION=mini-millennium), not a test build, then rerun the
input twice into different output directories and require the two outputs byte-identical before
copying one:
    ./mimic models/sage26/modules/_tests/input/test_physics_binary.yaml
    cp output/sage26-physics-binary/model_z0.000_0 \\
       output/sage26-physics-binary/model_z7.272_0 \\
       models/sage26/modules/_tests/baseline/physics-binary/
    cp output/sage26-physics-binary/metadata/output_schema.json \\
       models/sage26/modules/_tests/baseline/physics-binary/metadata/
and give the reason, the build's commit, compiler and platform, and each file's SHA-256 in the
commit message. Never edit a baseline file by hand, and never refresh it to silence a failure
that has not been explained.
"""

import sys
from pathlib import Path

import numpy as np
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
    TestNotApplicable,
    TestSkipped,
    baseline_rtol,
    compare_halos_comprehensive,
    compiled_model,
    compiled_simulation,
    find_validity_violations,
    format_validity_finding,
    load_binary_halos,
    load_validation_manifest,
    run_mimic_fresh,
    run_test_suite,
)
from framework.harness import fixture_output_field_names  # noqa: E402

# The package pair the committed baseline belongs to
BASELINE_MODEL = "sage26"
BASELINE_SIMULATION = "mini-millennium"

TESTS_DIR = REPO_ROOT / "models" / "sage26" / "modules" / "_tests"
INPUT = TESTS_DIR / "input" / "test_physics_binary.yaml"
RUN_FILE = REPO_ROOT / "models" / "sage26" / "input" / "sage26_mini-millennium.yaml"
BASELINE_DIR = TESTS_DIR / "baseline" / "physics-binary"
CURRENT_DIR = REPO_ROOT / "output" / "sage26-physics-binary"

# The input's output snapshots and the binary file Mimic writes for each (model_z<z>_0, z from
# tests/data/input/mini-millennium.a_list). FFB_SNAPSHOT is the snapshot, among five of the package
# run file's eight output snapshots (16, 18, 20, 23 and 27), whose output on the test trees holds
# the most FFBRegime 1 rows
FFB_SNAPSHOT = 16
OUTPUT_FILES = {63: "model_z0.000_0", FFB_SNAPSHOT: "model_z7.272_0"}
SCHEMA = Path("metadata") / "output_schema.json"

# Row selections the committed baseline must hold at least one row of, as (snapshot, label,
# selection), so a pass says something about the physics the default pipeline switches on
REQUIRED_ROWS = [
    (FFB_SNAPSHOT, "FFBRegime == 1", lambda halos: halos["FFBRegime"] == 1),
    (63, "Regime == 1", lambda halos: halos["Regime"] == 1),
    (63, "Concentration != 0", lambda halos: halos["Concentration"] != 0),
    (63, "MassLoading != 0", lambda halos: halos["MassLoading"] != 0),
    (63, "H2gas != 0", lambda halos: halos["H2gas"] != 0),
    # The merger path: satellites, merger bulges, and quasar-mode accretion
    (63, "Type == 1", lambda halos: halos["Type"] == 1),
    (63, "MergerBulgeMass != 0", lambda halos: halos["MergerBulgeMass"] != 0),
    (63, "QuasarModeBHaccretionMass != 0", lambda halos: halos["QuasarModeBHaccretionMass"] != 0),
]

# Set once the input has been run in this process, so the cases share one fresh run
_fresh_run_done = False


def check_baseline_pair():
    """Raise unless the selected pair is the one the committed baseline belongs to.

    The registry runs a model-local test only under its own model, so another model is a
    misconfiguration and stays a loud SKIP. The test is registered under MODEL=sage26 with every
    member of FULL_MODEL_TEST_SIMULATIONS, so another simulation is a legitimate, deterministic
    consequence of the selection: NA.
    """
    model = compiled_model()
    if model != BASELINE_MODEL:
        raise TestSkipped(
            f"MODEL={model}; this baseline is {BASELINE_MODEL}-specific and the registry runs it "
            f"only under MODEL={BASELINE_MODEL}"
        )
    simulation = compiled_simulation()
    if simulation != BASELINE_SIMULATION:
        raise TestNotApplicable(
            f"committed baseline is for MODEL={BASELINE_MODEL} SIMULATION={BASELINE_SIMULATION}, "
            f"but this run selected MODEL={model} SIMULATION={simulation}"
        )


def _require_runnable():
    """Skip unless the baseline pair is selected and Mimic is built."""
    check_baseline_pair()
    if not MIMIC_EXE.exists():
        raise TestSkipped("Mimic not built")


def _fresh_outputs():
    """Run the input once per process, always regenerating; return {snapshot: output path}."""
    global _fresh_run_done
    if not _fresh_run_done:
        assert INPUT.exists(), f"Full-physics input not found: {INPUT}"
        # Remove every expected output first, so a stale file cannot satisfy a later check
        for name in [*OUTPUT_FILES.values(), SCHEMA]:
            stale = CURRENT_DIR / name
            if stale.exists():
                stale.unlink()
        run_mimic_fresh(INPUT, CURRENT_DIR / OUTPUT_FILES[63])
        _fresh_run_done = True
    return {snapshot: CURRENT_DIR / name for snapshot, name in OUTPUT_FILES.items()}


def _check_output_names(names_now, names_ref, fixture_names, snapshot):
    """Assert one snapshot's output names are the baseline's followed by the fixture outputs.

    A test build appends the test fixtures' output fields after every production
    field, so any other difference (a production field added, removed, renamed or
    moved) is a schema change the baseline does not cover.
    """
    expected = tuple(names_ref) + tuple(fixture_names)
    assert tuple(names_now) == expected, (
        f"{RED}snapshot {snapshot}: output property names differ from the baseline's followed "
        f"by the test fixtures' output fields {list(fixture_names)}.\n"
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


def _typed(node):
    """Return a YAML node in a form whose equality checks order, keys, values and value types.

    Plain dict equality ignores key order and Python equates 1, 1.0 and True, so each mapping
    becomes an ordered list of pairs and each scalar carries its type name.
    """
    if isinstance(node, dict):
        return ("map", [(key, _typed(value)) for key, value in node.items()])
    if isinstance(node, list):
        return ("list", [_typed(item) for item in node])
    return (type(node).__name__, node)


def _load_yaml(path):
    with open(path, "r", encoding="utf-8") as handle:
        return yaml.safe_load(handle)


def test_input_matches_run_file():
    """The input's SubSteps and modules block equal the vertical run file's, in order."""
    assert INPUT.exists(), f"Full-physics input not found: {INPUT}"
    assert RUN_FILE.exists(), f"Package run file not found: {RUN_FILE}"
    test_input = _load_yaml(INPUT)
    run_file = _load_yaml(RUN_FILE)

    for key in ("SubSteps", "modules"):
        assert key in run_file, f"run file {RUN_FILE.name} has no '{key}'"
        assert key in test_input, f"input {INPUT.name} has no '{key}'"
        assert _typed(test_input[key]) == _typed(run_file[key]), (
            f"{RED}input '{key}' differs from {RUN_FILE.name}: refresh the input and the baseline "
            f"together (see this file's docstring){NC}"
        )
    print(
        f"  SubSteps={run_file['SubSteps']}; modules block equal "
        f"({len(run_file['modules']['parameters'])} parameters, "
        f"phases {list(run_file['modules']['phases'])})"
    )


def test_sage26_physics_baseline():
    """A fresh run of the input matches the committed baseline at every output snapshot."""
    _require_runnable()

    assert INPUT.exists(), f"Full-physics input not found: {INPUT}"
    for name in [*OUTPUT_FILES.values(), SCHEMA]:
        assert (BASELINE_DIR / name).exists(), (
            f"{RED}Committed baseline file not found: {BASELINE_DIR / name}\n"
            f"Capture it from a run of the input and commit it (see this file's docstring).{NC}"
        )
    snapshot_list = _load_yaml(INPUT)["output"]["snapshot_list"]
    assert sorted(snapshot_list) == sorted(
        OUTPUT_FILES
    ), f"input snapshot_list {snapshot_list} is not the baseline's {sorted(OUTPUT_FILES)}"

    # The committed baseline must exercise the default pipeline's physics, or a pass would say
    # nothing about it
    baseline_halos = {
        snapshot: load_binary_halos(BASELINE_DIR / name)[0]
        for snapshot, name in OUTPUT_FILES.items()
    }
    for snapshot, label, selection in REQUIRED_ROWS:
        count = int(np.sum(selection(baseline_halos[snapshot])))
        print(f"  baseline snapshot {snapshot}: rows with {label}: {count}")
        assert count >= 1, (
            f"{RED}baseline snapshot {snapshot} holds no row with {label}, so it does not "
            f"cover the default pipeline's physics{NC}"
        )

    failures = []
    for snapshot, current in _fresh_outputs().items():
        name = OUTPUT_FILES[snapshot]
        assert current.exists(), f"Mimic wrote no output for snapshot {snapshot}: {current}"
        halos_now, meta_now = load_binary_halos(current)
        halos_ref, meta_ref = load_binary_halos(BASELINE_DIR / name)
        print(
            f"  snapshot {snapshot} ({name}): current {meta_now['TotHalos']} rows, "
            f"baseline {meta_ref['TotHalos']} rows"
        )

        assert meta_now["TotHalos"] == meta_ref["TotHalos"], (
            f"{RED}snapshot {snapshot}: row count mismatch, current={meta_now['TotHalos']}, "
            f"baseline={meta_ref['TotHalos']}{NC}"
        )
        _check_output_names(
            halos_now.dtype.names, halos_ref.dtype.names, fixture_output_field_names(), snapshot
        )

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
        if not passed:
            failures.append(snapshot)

    assert not failures, (
        f"{RED}sage26 full-physics output does not match the committed baseline at "
        f"snapshot(s) {failures}. If this was a deliberate, validated change, refresh the "
        f"baseline (see this file's docstring).{NC}"
    )
    print(f"{GREEN}  ✓ sage26 galaxy-physics output matches the committed baseline{NC}")


def test_output_names_reject_doctored_production_fields():
    """The name check fails when a production field is added, removed, renamed or moved."""
    baseline = BASELINE_DIR / OUTPUT_FILES[63]
    assert baseline.exists(), f"Committed baseline not found: {baseline}"
    names_ref = tuple(load_binary_halos(baseline)[0].dtype.names)
    fixture = fixture_output_field_names()
    assert len(names_ref) >= 2 and fixture, "need production and fixture names to doctor"

    _check_output_names(names_ref + fixture, names_ref, fixture, 63)

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
            _check_output_names(names_now, names_ref, fixture, 63)
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

    violations = []
    skipped_fields = []
    for snapshot, current in _fresh_outputs().items():
        halos, meta = load_binary_halos(current)
        findings, skipped = find_validity_violations(halos, properties)
        skipped_fields.extend(field for field in skipped if field not in skipped_fields)
        print(
            f"  snapshot {snapshot}: {meta['TotHalos']} rows, "
            f"{len(halos.dtype.names) - len(skipped)} of {len(halos.dtype.names)} properties "
            f"range-checked, {len(findings)} violation(s)"
        )
        for finding in findings:
            text = format_validity_finding(finding)
            print(f"{RED}  ✗ snapshot {snapshot}: {text}{NC}")
            violations.append(f"snapshot {snapshot} {text}")
    for field in skipped_fields:
        print(f"{YELLOW}  Skipping {field}: no range specified in metadata{NC}")

    assert not violations, (
        f"{len(violations)} non-finite or out-of-range finding(s) in sage26 full-physics output; "
        f"explain each before changing a range or a module:\n" + "\n".join(violations)
    )
    print(f"{GREEN}  ✓ no NaN, Inf or out-of-range value in any output property{NC}")


def main():
    print(f"Repository root: {REPO_ROOT}")
    print(f"Mimic executable: {MIMIC_EXE}")
    return run_test_suite(
        [
            test_input_matches_run_file,
            test_sage26_physics_baseline,
            test_output_names_reject_doctored_production_fields,
            test_physics_output_finite_and_in_range,
        ],
        "sage26 Full-Physics Baseline (test_scientific_sage26_physics_baseline.py)",
    )


if __name__ == "__main__":
    raise SystemExit(main())
