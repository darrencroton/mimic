#!/usr/bin/env python3
"""
sage26_set_halo_concentration - Integration Test

Validates: SAGE26's halo-concentration assignment at progenitor joining through real runs of
mimic on the committed test trees: it loads and runs, writes nothing with ConcentrationOn 0,
writes only its domain with ConcentrationOn 3, and refuses a missing required key and a
misplacement at the executable's exit status.

Every configuration sets every key its modules read, explicitly (campaign decision D33). The keys
come from each configured module's module_info.yaml `parameters` list, which
`make lint-parameters` holds to the code, with the vertical run file's values
(models/sage26/input/sage26_mini-millennium.yaml). Runs use the run file's SubSteps (10) and write
snapshots 63 and 16.

Minimal pipeline (Loads and runs, Off, On; ConcentrationOn 3 for Loads and runs and On), in the
run file's order:
  pre_timestep:  sage26_set_halo_concentration
  post_timestep: sage26_record_transition_state
  - sage26_set_halo_concentration: the module under test. Its reads Type, Vmax, Vvir, infallVmax
    and infallVvir are core properties.
  - sage26_record_transition_state: with ConcentrationOn 3 the module's guard requires it in
    post_timestep (check_recorder); it writes PreviousType, which selects the velocity pair
    (without it every galaxy holds -1 and no central is assigned).

Off is a no-op: with ConcentrationOn 0, the minimal pipeline with the module and the same pipeline
without it (sage26_record_transition_state alone) give byte-identical output in every field of
every row of both snapshots.

On writes its domain (frozen observable list):
  - Concentration: finite and non-negative on every output row of both snapshots. Domain: the
    NFW concentration of SAGE26's Vmax/Vvir inversion, 0 if (Vmax/Vvir)^2 < 1, with creation value
    0, and every other row keeps its value (models/sage26/model_properties.yaml:1371-1378; README.md,
    "Processing Contract", the assignment table).
  - Witness: at least one Type 0 row with Concentration > 0 at snapshot 63.
  - Exceptions: none. Concentration is the only property the module writes.

Required-key refusal, isolated (a real run; mimic exits non-zero with "Required model parameter
'ConcentrationOn' not found" and "Module 'sage26_set_halo_concentration' initialization failed",
and no other module's initialization failure):
  - ConcentrationOn. Configuration: the minimal pipeline, in which only this module reads the key
    (asserted from the modules' parameters lists). Control: the same configuration with
    ConcentrationOn 3 exits 0.

Placement refusal: the module in a substep phase (galaxy_physics, process_full_halo) with
sage26_record_transition_state in post_timestep, ConcentrationOn 3, every key present, is refused
naming the module (check_placement: exactly one pre_timestep entry and none elsewhere).

These run-level cases repeat, at the executable's exit status, refusals the module's C unit tests
assert; they test no new behaviour.

Run (from the repository root, after `make MODEL=sage26 SIMULATION=mini-millennium`):
  MODEL=sage26 SIMULATION=mini-millennium python3 models/sage26/modules/\\
sage26_set_halo_concentration/_tests/test_integration_sage26_set_halo_concentration.py
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

MODULE = "sage26_set_halo_concentration"
RECORDER = "sage26_record_transition_state"
MODULES_DIR = Path(__file__).resolve().parents[2]
FH = "process_full_halo"

# The vertical run file's SubSteps and the snapshots written, with their binary output files on
# mini-millennium's committed test trees (16 is Slice 1's FFB snapshot S, z = 7.272)
SUBSTEPS = 10
OUTPUT_FILES = {63: "model_z0.000_0", 16: "model_z7.272_0"}

# The vertical run file's values for every key the configurations below read (D33)
PARAMETER_VALUES = {
    "ConcentrationOn": 3,
}

MINIMAL = {
    "pre_timestep": [(MODULE, FH)],
    "post_timestep": [(RECORDER, FH)],
}
MINIMAL_WITHOUT = {"post_timestep": [(RECORDER, FH)]}
MISPLACED = {
    "galaxy_physics": [(MODULE, FH)],
    "post_timestep": [(RECORDER, FH)],
}

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
    """Every key the configured modules read, with the run file's values (D33)."""
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
    """The minimal pipeline with ConcentrationOn 3 runs to completion, exit 0, no leak report."""
    require_pair()
    require_mimic()
    with mimic_run(f"{MODULE}_loads", MINIMAL, parameters_for(MINIMAL)) as run:
        returncode, stdout, stderr, output_dir = run
        assert_runs(returncode, stdout, stderr)
        assert "SAGE set halo concentration module initialized" in stdout, "no initialization log"
        assert "SAGE set halo concentration module cleaned up" in stdout, "no cleanup log"
        for snapshot in OUTPUT_FILES:
            load_snapshot(output_dir, snapshot)
    print(f"  exit status 0, no leak report, snapshots {list(OUTPUT_FILES)} written")


def test_off_is_noop():
    """With ConcentrationOn 0 the output equals the same pipeline's without the module."""
    require_pair()
    require_mimic()
    off = {"ConcentrationOn": 0}
    with mimic_run(f"{MODULE}_off_with", MINIMAL, parameters_for(MINIMAL, off)) as with_module:
        returncode, stdout, stderr, dir_with = with_module
        assert_runs(returncode, stdout, stderr)
        params = parameters_for(MINIMAL_WITHOUT)
        with mimic_run(f"{MODULE}_off_without", MINIMAL_WITHOUT, params) as without:
            returncode, stdout, stderr, dir_without = without
            assert_runs(returncode, stdout, stderr)
            assert_outputs_identical(dir_with, dir_without)


def test_on_writes_domain():
    """With ConcentrationOn 3 every Concentration is finite and >= 0; a Type 0 row has > 0."""
    require_pair()
    require_mimic()
    with mimic_run(f"{MODULE}_on", MINIMAL, parameters_for(MINIMAL)) as run:
        returncode, stdout, stderr, output_dir = run
        assert_runs(returncode, stdout, stderr)
        for snapshot in OUTPUT_FILES:
            halos = load_snapshot(output_dir, snapshot)
            values = halos["Concentration"]
            bad = int((~np.isfinite(values) | (values < 0)).sum())
            assert bad == 0, f"snapshot {snapshot}: {bad} rows with Concentration not finite, >= 0"
            centrals = halos["Type"] == 0
            positive = int((centrals & (values > 0)).sum())
            print(
                f"  snapshot {snapshot}: {len(halos)} rows in domain; Type 0 rows: "
                f"{int(centrals.sum())}, with Concentration > 0: {positive}"
            )
            if snapshot == 63:
                assert positive > 0, "witness: no Type 0 row with Concentration > 0 at 63"


def test_required_key_concentrationon_refused():
    """A missing ConcentrationOn is refused by this module alone; the control exits 0."""
    require_pair()
    require_mimic()
    key = "ConcentrationOn"
    readers = [name for name in configured_modules(MINIMAL) if key in module_parameters(name)]
    assert readers == [MODULE], f"configured readers of {key}: {readers}"

    with mimic_run(f"{MODULE}_key_control", MINIMAL, parameters_for(MINIMAL)) as run:
        returncode, stdout, stderr, _ = run
        assert_runs(returncode, stdout, stderr)
        print(f"  control ({key} present): exit status {returncode}")

    with mimic_run(f"{MODULE}_key_missing", MINIMAL, parameters_for(MINIMAL, omit=(key,))) as run:
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
    """An entry in a substep phase is refused, naming the module."""
    require_pair()
    require_mimic()
    with mimic_run(f"{MODULE}_misplaced", MISPLACED, parameters_for(MISPLACED)) as run:
        returncode, stdout, stderr, _ = run
        assert_refused(
            returncode,
            stdout,
            stderr,
            [f"{MODULE} is configured in substep phase 'galaxy_physics'"],
        )


def main():
    tests = [
        test_loads_and_runs,
        test_off_is_noop,
        test_on_writes_domain,
        test_required_key_concentrationon_refused,
        test_placement_refused,
    ]
    return run_test_suite(tests, f"{MODULE} Integration Tests")


if __name__ == "__main__":
    sys.exit(main())
