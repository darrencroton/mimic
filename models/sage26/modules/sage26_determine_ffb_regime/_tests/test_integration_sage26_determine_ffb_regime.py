#!/usr/bin/env python3
"""
sage26_determine_ffb_regime - Integration Test

Validates: SAGE26's feedback-free-burst classifier through real runs of mimic on the committed
test trees: it loads and runs, writes nothing with EnhancedStarFormationOn 0, writes only its
domain with EnhancedStarFormationOn 1, and refuses a missing required key, a misplacement and,
at EnhancedStarFormationOn 1, a configuration without the FFB transaction, at the executable's
exit status.

Every configuration sets every key its modules read, explicitly. The keys
come from each configured module's module_info.yaml `parameters` list, which
`make lint-parameters` holds to the code, with the vertical run file's values
(models/sage26/input/sage26_mini-millennium.yaml), except CGMrecipeOn, set to 0 because no CGM
module is configured, so the star-formation modules take the classic hot-gas routing.
Runs use the run file's SubSteps (10) and write snapshots 63 and 16 (the FFB snapshot S).

Minimal pipeline (Loads and runs, Off, On; EnhancedStarFormationOn 1 for Loads and runs and On),
in the run file's order:
  pre_timestep:   sage26_determine_ffb_regime
  galaxy_physics: sage26_ffb_star_formation, sage26_calculate_star_formation,
                  sage26_apply_star_formation_supernova
  - sage26_determine_ffb_regime: the module under test. Its reads Type, Mvir and MostBoundID are
    core and simulation halo properties.
  - sage26_ffb_star_formation: at EnhancedStarFormationOn 1 the module's guard requires it in a
    substep phase, by galaxy.
  - sage26_calculate_star_formation: the transaction's guard requires it immediately after.
  - sage26_apply_star_formation_supernova: sage26_calculate_star_formation refuses to run without
    it (sage26_calculate_star_formation.c, its init's dependency check).

Off is a no-op: with EnhancedStarFormationOn 0, the minimal pipeline with the module and the same
pipeline without it give byte-identical output in every field of every row of both snapshots.

On writes its domain (the observable list):
  - FFBRegime: in {0, 1} on every output row of both snapshots (range [0, 1], creation value 0,
    models/sage26/model_properties.yaml:496-504; README.md, "Properties": "Writes: FFBRegime").
  - Witness: at least one row with FFBRegime 1 at snapshot S = 16.
  - Exceptions: none. FFBRegime is the only property the module writes.

Required-key refusal, isolated (a real run; mimic exits non-zero with "Required model parameter
'EnhancedStarFormationOn' not found" and "Module 'sage26_determine_ffb_regime' initialization
failed", and no other module's initialization failure):
  - EnhancedStarFormationOn. Configuration: the classifier alone in pre_timestep, with
    sage26_ffb_star_formation (the key's other reader) not configured; the classifier fails reading
    the key before any cross-module guard applies (asserted from the modules' parameters lists).
    Control: the same configuration with EnhancedStarFormationOn 0 exits 0. At 1 the classifier
    alone is refused by its cross-module guard, which is the next case's subject.

Placement refusal: with EnhancedStarFormationOn 0 (so no cross-module guard applies), the
classifier in a substep phase (galaxy_physics, process_full_halo), every key present, is refused
naming the module (check_placement).

Cross-module guard, at EnhancedStarFormationOn 1: the classifier
configured and sage26_ffb_star_formation absent is refused by the classifier, naming
sage26_ffb_star_formation. This configuration breaks only that guard. The transaction's two guards
(adjacency to sage26_calculate_star_formation, and the classifier's presence) are each exercised by
sage26_ffb_star_formation's integration test, beside the guard they test.

These run-level cases repeat, at the executable's exit status, refusals the module's C unit tests
assert; they test no new behaviour.

Run (from the repository root, after `make MODEL=sage26 SIMULATION=mini-millennium`):
  MODEL=sage26 SIMULATION=mini-millennium python3 models/sage26/modules/\\
sage26_determine_ffb_regime/_tests/test_integration_sage26_determine_ffb_regime.py
"""

import re
import shutil
import sys
from contextlib import contextmanager
from pathlib import Path

import numpy as np
import yaml

REPO_ROOT = Path(__file__).resolve().parents[5]
sys.path.insert(0, str(REPO_ROOT / "tests"))

from framework import (  # noqa: E402
    MIMIC_EXE,
    TestNotApplicable,
    TestSkipped,
    check_no_memory_leaks,
    compiled_model,
    compiled_simulation,
    create_test_param_file,
    load_binary_halos,
    run_mimic,
    run_test_suite,
)

MODULE = "sage26_determine_ffb_regime"
TRANSACTION = "sage26_ffb_star_formation"
MODULES_DIR = Path(__file__).resolve().parents[2]
FH = "process_full_halo"
BG = "process_by_galaxy"

# The vertical run file's SubSteps and the snapshots written, with their binary output files on
# mini-millennium's committed test trees (16 is the FFB snapshot S, z = 7.272)
SUBSTEPS = 10
FFB_SNAPSHOT = 16
OUTPUT_FILES = {63: "model_z0.000_0", FFB_SNAPSHOT: "model_z7.272_0"}

# The vertical run file's values for every key the configurations below read, except CGMrecipeOn: 0,
# as no CGM module is configured
PARAMETER_VALUES = {
    "EnhancedStarFormationOn": 1,
    "EnhancedSFEfficiency": 0.2,
    "StarFormingDiskFactor": 3.0,
    "SFprescription": 1,
    "RecycleFraction": 0.43,
    "CGMrecipeOn": 0,
    "FIREmodeOn": 1,
    "FeedbackReheatingEpsilon": 2.9,
    "FeedbackEjectionEfficiency": 0.3,
    "Yield": 0.025,
    "FracZleaveDisk": 0.0,
    "SfrEfficiency": 0.05,
    "H2RadialIntegrationOn": 1,
    "H2RadialNBins": 25,
    "H2RadialRMaxFactor": 5.0,
}

STAR_FORMATION = [
    (TRANSACTION, BG),
    ("sage26_calculate_star_formation", BG),
    ("sage26_apply_star_formation_supernova", BG),
]
MINIMAL = {
    "pre_timestep": [(MODULE, FH)],
    "galaxy_physics": STAR_FORMATION,
}
MINIMAL_WITHOUT = {"galaxy_physics": STAR_FORMATION}
CLASSIFIER_ALONE = {"pre_timestep": [(MODULE, FH)]}
MISPLACED = {"galaxy_physics": [(MODULE, FH)]}

INIT_FAILED = re.compile(r"Module '([^']+)' initialization failed")


# ============================================================================
# HELPERS
# ============================================================================


def require_pair():
    """SKIP outside sage26 (the registry never runs it there); NA off mini-millennium."""
    if compiled_model() != "sage26":
        raise TestSkipped(f"model-local sage26 test run under MODEL={compiled_model()}")
    if compiled_simulation() != "mini-millennium":
        raise TestNotApplicable(
            f"selected pair (sage26, {compiled_simulation()}): the output snapshots and witnesses "
            "are those of mini-millennium's committed test trees (pair sage26, mini-millennium)"
        )


def require_mimic():
    if not MIMIC_EXE.exists():
        raise TestSkipped("Mimic not built")


def module_parameters(name):
    """The `parameters` list of a module's module_info.yaml."""
    with open(MODULES_DIR / name / "module_info.yaml", encoding="utf-8") as handle:
        info = yaml.safe_load(handle)
    return list(info["module"]["dependencies"]["parameters"])


def configured_modules(phases):
    return sorted({name for entries in phases.values() for name, _mode in entries})


def parameters_for(phases, overrides=None, omit=()):
    """Every key the configured modules read, with the run file's values."""
    keys = sorted({key for name in configured_modules(phases) for key in module_parameters(name)})
    missing = [key for key in keys if key not in PARAMETER_VALUES]
    assert not missing, f"no value for keys the configured modules read: {missing}"
    params = {key: PARAMETER_VALUES[key] for key in keys if key not in omit}
    params.update(overrides or {})
    return params


@contextmanager
def mimic_run(name, phases, params):
    """Run mimic on a generated run file; yield (returncode, stdout, stderr, output_dir)."""
    param_file, output_dir, temp_dir = create_test_param_file(
        output_name=name, phase_config=phases, model_params=params, substeps=SUBSTEPS
    )
    try:
        with open(param_file, encoding="utf-8") as handle:
            config = yaml.safe_load(handle)
        config["output"]["snapshot_list"] = list(OUTPUT_FILES)
        with open(param_file, "w", encoding="utf-8") as handle:
            yaml.safe_dump(config, handle, sort_keys=False)
        returncode, stdout, stderr = run_mimic(param_file)
        yield returncode, stdout, stderr, output_dir
    finally:
        shutil.rmtree(temp_dir, ignore_errors=True)


def load_snapshot(output_dir, snapshot):
    halos, _metadata = load_binary_halos(output_dir / OUTPUT_FILES[snapshot])
    assert len(halos) > 0, f"snapshot {snapshot} has no output rows"
    return halos


def assert_runs(returncode, stdout, stderr):
    assert returncode == 0, f"mimic exited {returncode}\nSTDERR:\n{stderr[-2000:]}"
    assert check_no_memory_leaks(stdout, stderr), "mimic reported a memory leak"


def assert_outputs_identical(dir_a, dir_b):
    """Every field of every row of every written snapshot is byte-identical."""
    for snapshot in OUTPUT_FILES:
        a = load_snapshot(dir_a, snapshot)
        b = load_snapshot(dir_b, snapshot)
        assert a.dtype.names == b.dtype.names, f"snapshot {snapshot}: property lists differ"
        assert len(a) == len(b), f"snapshot {snapshot}: {len(a)} rows against {len(b)}"
        for field in a.dtype.names:
            bytes_a = np.ascontiguousarray(a[field]).view(np.uint8).reshape(len(a), -1)
            bytes_b = np.ascontiguousarray(b[field]).view(np.uint8).reshape(len(b), -1)
            differing = int(np.any(bytes_a != bytes_b, axis=1).sum())
            assert differing == 0, f"snapshot {snapshot}: {field} differs on {differing} rows"
        print(f"  snapshot {snapshot}: {len(a)} rows, {len(a.dtype.names)} fields identical")


def assert_refused(returncode, stdout, stderr, needles, module=MODULE):
    """Exit non-zero, every needle present, and only `module`'s initialization failure."""
    text = stdout + "\n" + stderr
    assert returncode != 0, f"mimic exited 0; expected a refusal naming {module}"
    failures = INIT_FAILED.findall(text)
    assert failures == [module], f"initialization failures logged: {failures}"
    for needle in needles:
        assert needle in text, f"missing diagnostic: {needle!r}"
    for line in text.splitlines():
        if "ERROR" in line:
            print(f"  refusal: {line.strip()}")
    print(f"  refusal: exit status {returncode}")


# ============================================================================
# TEST CASES
# ============================================================================


def test_loads_and_runs():
    """The minimal pipeline at EnhancedStarFormationOn 1 runs to completion, exit 0, no leak."""
    require_pair()
    require_mimic()
    with mimic_run(f"{MODULE}_loads", MINIMAL, parameters_for(MINIMAL)) as run:
        returncode, stdout, stderr, output_dir = run
        assert_runs(returncode, stdout, stderr)
        assert "SAGE determine FFB regime module initialized" in stdout, "no initialization log"
        assert "SAGE determine FFB regime module cleaned up" in stdout, "no cleanup log"
        for snapshot in OUTPUT_FILES:
            load_snapshot(output_dir, snapshot)
    print(f"  exit status 0, no leak report, snapshots {list(OUTPUT_FILES)} written")


def test_off_is_noop():
    """At EnhancedStarFormationOn 0 the output equals the same pipeline's without the module."""
    require_pair()
    require_mimic()
    off = {"EnhancedStarFormationOn": 0}
    with mimic_run(f"{MODULE}_off_with", MINIMAL, parameters_for(MINIMAL, off)) as with_module:
        returncode, stdout, stderr, dir_with = with_module
        assert_runs(returncode, stdout, stderr)
        params = parameters_for(MINIMAL_WITHOUT, off)
        with mimic_run(f"{MODULE}_off_without", MINIMAL_WITHOUT, params) as without:
            returncode, stdout, stderr, dir_without = without
            assert_runs(returncode, stdout, stderr)
            assert_outputs_identical(dir_with, dir_without)


def test_on_writes_domain():
    """At EnhancedStarFormationOn 1 every FFBRegime is 0 or 1; snapshot S has an FFB row."""
    require_pair()
    require_mimic()
    with mimic_run(f"{MODULE}_on", MINIMAL, parameters_for(MINIMAL)) as run:
        returncode, stdout, stderr, output_dir = run
        assert_runs(returncode, stdout, stderr)
        for snapshot in OUTPUT_FILES:
            halos = load_snapshot(output_dir, snapshot)
            outside = int((~np.isin(halos["FFBRegime"], (0, 1))).sum())
            assert outside == 0, f"snapshot {snapshot}: {outside} rows with FFBRegime not 0 or 1"
            ffb = int((halos["FFBRegime"] == 1).sum())
            print(f"  snapshot {snapshot}: {len(halos)} rows in domain, FFBRegime 1: {ffb}")
            if snapshot == FFB_SNAPSHOT:
                assert ffb > 0, f"witness: no FFBRegime 1 row at snapshot {FFB_SNAPSHOT}"


def test_required_key_enhancedstarformationon_refused():
    """A missing EnhancedStarFormationOn is refused by this module alone; the control exits 0."""
    require_pair()
    require_mimic()
    key = "EnhancedStarFormationOn"
    configured = configured_modules(CLASSIFIER_ALONE)
    readers = [name for name in configured if key in module_parameters(name)]
    assert readers == [MODULE], f"configured readers of {key}: {readers}"
    assert TRANSACTION not in configured

    params = parameters_for(CLASSIFIER_ALONE, {key: 0})
    with mimic_run(f"{MODULE}_key_control", CLASSIFIER_ALONE, params) as run:
        returncode, stdout, stderr, _ = run
        assert_runs(returncode, stdout, stderr)
        print(f"  control ({key} present, 0): exit status {returncode}")

    params = parameters_for(CLASSIFIER_ALONE, omit=(key,))
    with mimic_run(f"{MODULE}_key_missing", CLASSIFIER_ALONE, params) as run:
        returncode, stdout, stderr, _ = run
        assert_refused(
            returncode,
            stdout,
            stderr,
            [
                f"Required model parameter '{key}' not found",
                f"Module '{MODULE}' initialization failed",
            ],
        )


def test_placement_refused():
    """At EnhancedStarFormationOn 0, an entry in a substep phase is refused, naming the module."""
    require_pair()
    require_mimic()
    params = parameters_for(MISPLACED, {"EnhancedStarFormationOn": 0})
    with mimic_run(f"{MODULE}_misplaced", MISPLACED, params) as run:
        returncode, stdout, stderr, _ = run
        assert_refused(
            returncode,
            stdout,
            stderr,
            [f"{MODULE} is configured in substep phase 'galaxy_physics'"],
        )


def test_transaction_required_refused():
    """At EnhancedStarFormationOn 1 the classifier alone is refused, naming the transaction."""
    require_pair()
    require_mimic()
    params = parameters_for(CLASSIFIER_ALONE)
    assert params["EnhancedStarFormationOn"] == 1
    with mimic_run(f"{MODULE}_no_transaction", CLASSIFIER_ALONE, params) as run:
        returncode, stdout, stderr, _ = run
        assert_refused(
            returncode,
            stdout,
            stderr,
            [f"{MODULE} requires {TRANSACTION} in a substep phase"],
        )


def main():
    tests = [
        test_loads_and_runs,
        test_off_is_noop,
        test_on_writes_domain,
        test_required_key_enhancedstarformationon_refused,
        test_placement_refused,
        test_transaction_required_refused,
    ]
    return run_test_suite(tests, f"{MODULE} Integration Tests")


if __name__ == "__main__":
    sys.exit(main())
