#!/usr/bin/env python3
"""
sage26 full-physics galaxy-output baseline regression.

Validates: the package's complete default pipeline (R6), run on the committed single-file test
           trees, reproduces the committed galaxy-physics baseline: every property of every row at
           each output snapshot (63, and 16, the snapshot holding the most FFBRegime 1 rows);
           the input's SubSteps and modules block equal the package's vertical run file's, value
           for value and in order; and the baseline covers R6's new physics (FFBRegime 1 rows at
           snapshot 16; Regime 1 rows and non-zero Concentration, MassLoading and H2gas at 63)
Tolerance: integers exact; floats at baseline_rtol() (1e-6 relative unless MIMIC_BASELINE_RTOL
           overrides it) with the absolute floor BASELINE_ATOL_DEFAULT (1e-10). Under a relaxed
           override, float differences beyond 1e-6 are reported as warnings. The baseline was
           captured on macOS arm64 (Apple clang) and reproduces there bit for bit; another
           compiler or libm may need the override, as CI uses for sage16's baseline

This is a regression guard on the package's own output on the test trees, not a parity
measurement against SAGE26: the package's parity with SAGE26 is measured by the instrument under
models/sage26/instrument. A pass
says the output has not moved since the baseline was captured; it says nothing about whether
that output matches SAGE26.

The repository-level baseline (tests/data/output/baseline/) is physics-free and belongs to the
default package pair, so it cannot catch drift in sage26 galaxy formation. This model-owned
test closes that gap for sage26. It guards itself with its own model and simulation check
rather than the framework's default-pair guard: the committed baseline belongs to MODEL=sage26
SIMULATION=mini-millennium, which is not the Makefile's default pair, so that guard would report
NA under every sage26 pair.

Test cases:
  - test_pair_guard_outcomes: the guard raises nothing under (sage26, mini-millennium),
    TestNotApplicable under (sage26, micro-uchuu) and (sage26, mini-millennium-horizontal), and
    TestSkipped under (sage16, mini-millennium)
  - test_input_matches_run_file: the input's SubSteps and modules block equal the run file's,
    so a deliberate run-file change fails here until the baseline is refreshed with it
  - test_sage26_physics_baseline: a fresh run of the input matches the committed baseline at
    every output snapshot, and the baseline covers R6's new physics

Run: make MODEL=sage26 SIMULATION=mini-millennium && \\
     MODEL=sage26 SIMULATION=mini-millennium \\
     python3 models/sage26/modules/_tests/test_scientific_sage26_physics_baseline.py

If the baseline case fails after a DELIBERATE, validated change to sage26 output, refresh the
baseline. If the run file changed, first copy its SubSteps and modules block into the input
(models/sage26/modules/_tests/input/test_physics_binary.yaml). Then rerun the input and copy
each output file and the schema:
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

import os
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
    TestNotApplicable,
    TestSkipped,
    baseline_rtol,
    compare_halos_comprehensive,
    compiled_model,
    compiled_simulation,
    load_binary_halos,
    run_mimic_fresh,
    run_test_suite,
)

# The package pair the committed baseline belongs to
BASELINE_MODEL = "sage26"
BASELINE_SIMULATION = "mini-millennium"

TESTS_DIR = REPO_ROOT / "models" / "sage26" / "modules" / "_tests"
INPUT = TESTS_DIR / "input" / "test_physics_binary.yaml"
RUN_FILE = REPO_ROOT / "models" / "sage26" / "input" / "sage26_mini-millennium.yaml"
BASELINE_DIR = TESTS_DIR / "baseline" / "physics-binary"
CURRENT_DIR = REPO_ROOT / "output" / "sage26-physics-binary"

# The input's output snapshots and the binary file Mimic writes for each (model_z<z>_0, z from
# tests/data/input/mini-millennium.a_list). FFB_SNAPSHOT is the snapshot among the output snapshots
# 16, 18, 20, 23 and 27 whose output on the test trees holds the most FFBRegime 1 rows
FFB_SNAPSHOT = 16
OUTPUT_FILES = {63: "model_z0.000_0", FFB_SNAPSHOT: "model_z7.272_0"}
SCHEMA = Path("metadata") / "output_schema.json"


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


def _guard_outcome(model, simulation):
    """Apply the guard under the given MODEL and SIMULATION, restoring the environment after."""
    saved = {key: os.environ.get(key) for key in ("MODEL", "SIMULATION")}
    os.environ["MODEL"] = model
    os.environ["SIMULATION"] = simulation
    try:
        check_baseline_pair()
        return None
    except (TestSkipped, TestNotApplicable) as exc:
        return type(exc)
    finally:
        for key, value in saved.items():
            if value is None:
                os.environ.pop(key, None)
            else:
                os.environ[key] = value


def _outcome_name(outcome):
    return "nothing" if outcome is None else outcome.__name__


def test_pair_guard_outcomes():
    """The guard runs the baseline under its pair, NA under other simulations, SKIP otherwise."""
    before = {key: os.environ.get(key) for key in ("MODEL", "SIMULATION")}
    expected = [
        (("sage26", "mini-millennium"), None),
        (("sage26", "micro-uchuu"), TestNotApplicable),
        (("sage26", "mini-millennium-horizontal"), TestNotApplicable),
        (("sage16", "mini-millennium"), TestSkipped),
    ]
    for (model, simulation), want in expected:
        got = _guard_outcome(model, simulation)
        print(f"  ({model}, {simulation}) raises {_outcome_name(got)}")
        assert got is want, (
            f"{RED}guard under ({model}, {simulation}) raised {_outcome_name(got)}, "
            f"expected {_outcome_name(want)}{NC}"
        )
    after = {key: os.environ.get(key) for key in ("MODEL", "SIMULATION")}
    assert after == before, f"guard probe left MODEL/SIMULATION changed: {before} -> {after}"


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
    parameters = run_file["modules"]["parameters"]
    print(
        f"  SubSteps={run_file['SubSteps']}; modules block equal "
        f"({len(parameters)} parameters, phases {list(run_file['modules']['phases'])})"
    )


def _row_counts(halos):
    """Counts of the rows that show R6's new physics in one output snapshot."""
    return {
        "FFBRegime == 1": int(np.sum(halos["FFBRegime"] == 1)),
        "Regime == 1": int(np.sum(halos["Regime"] == 1)),
        "Concentration != 0": int(np.sum(halos["Concentration"] != 0)),
        "MassLoading != 0": int(np.sum(halos["MassLoading"] != 0)),
        "H2gas != 0": int(np.sum(halos["H2gas"] != 0)),
    }


def test_sage26_physics_baseline():
    """A fresh run of the input matches the committed baseline at every output snapshot."""
    check_baseline_pair()
    if not MIMIC_EXE.exists():
        raise TestSkipped("Mimic not built")

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

    # The committed baseline must exercise R6's new physics, or a pass would say nothing about it
    ffb_counts = _row_counts(load_binary_halos(BASELINE_DIR / OUTPUT_FILES[FFB_SNAPSHOT])[0])
    z0_counts = _row_counts(load_binary_halos(BASELINE_DIR / OUTPUT_FILES[63])[0])
    print(f"  baseline coverage, snapshot {FFB_SNAPSHOT}: {ffb_counts}")
    print(f"  baseline coverage, snapshot 63: {z0_counts}")
    assert (
        ffb_counts["FFBRegime == 1"] >= 1
    ), f"baseline holds no FFBRegime 1 row at snapshot {FFB_SNAPSHOT}"
    for key in ("Regime == 1", "Concentration != 0", "MassLoading != 0", "H2gas != 0"):
        assert z0_counts[key] >= 1, f"baseline holds no row with {key} at snapshot 63"

    # Remove every expected output first, so a stale file cannot satisfy the comparison
    for name in [*OUTPUT_FILES.values(), SCHEMA]:
        stale = CURRENT_DIR / name
        if stale.exists():
            stale.unlink()
    run_mimic_fresh(INPUT, CURRENT_DIR / OUTPUT_FILES[63])

    failures = []
    for snapshot, name in OUTPUT_FILES.items():
        current = CURRENT_DIR / name
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
        names_now = halos_now.dtype.names
        names_ref = halos_ref.dtype.names
        assert names_now == names_ref, (
            f"{RED}snapshot {snapshot}: output property names differ. "
            f"Only in current: {sorted(set(names_now) - set(names_ref))}; "
            f"only in baseline: {sorted(set(names_ref) - set(names_now))}{NC}"
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
        f"{RED}sage26 full-physics output does not match the committed baseline at snapshot(s) "
        f"{failures}. If this was a deliberate, validated change, refresh the baseline (see this "
        f"file's docstring).{NC}"
    )
    print(f"{GREEN}  ✓ sage26 galaxy-physics output matches the committed baseline{NC}")


def main():
    print(f"Repository root: {REPO_ROOT}")
    print(f"Mimic executable: {MIMIC_EXE}")
    return run_test_suite(
        [
            test_pair_guard_outcomes,
            test_input_matches_run_file,
            test_sage26_physics_baseline,
        ],
        "sage26 Full-Physics Baseline (test_scientific_sage26_physics_baseline.py)",
    )


if __name__ == "__main__":
    raise SystemExit(main())
