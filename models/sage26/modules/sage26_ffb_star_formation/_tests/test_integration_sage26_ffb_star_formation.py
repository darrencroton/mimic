#!/usr/bin/env python3
"""
sage26_ffb_star_formation - Integration Test

Validates: SAGE26's feedback-free-burst star-formation transaction through real runs of mimic on
the committed test trees: it loads and runs, writes nothing with EnhancedStarFormationOn 0,
writes only its domain on the FFBRegime 1 rows with EnhancedStarFormationOn 1, and refuses a
missing required key, a misplacement and, at EnhancedStarFormationOn 1, a placement not
immediately before sage26_calculate_star_formation and a configuration without the classifier, at
the executable's exit status.

Every configuration sets every key its modules read, explicitly. The keys
come from each configured module's module_info.yaml `parameters` list, which
`make lint-parameters` holds to the code, with the vertical run file's values
(models/sage26/input/sage26_mini-millennium.yaml), except CGMrecipeOn, set to 0 because no CGM
module is configured, so infall, cooling and feedback take the classic hot-gas routing.
Runs use the run file's SubSteps (10) and write snapshots 63 and 16 (the FFB snapshot S).

Minimal pipeline (Loads and runs, Off, On; EnhancedStarFormationOn 1 for Loads and runs and On),
in the run file's order:
  pre_timestep:   sage26_set_disk_scale_radius, sage26_prepare_infall_budget,
                  sage26_determine_ffb_regime
  galaxy_physics: sage26_apply_infall, sage26_calculate_cooling_budget, sage26_apply_cooling,
                  sage26_ffb_star_formation, sage26_calculate_star_formation,
                  sage26_apply_star_formation_supernova
  - sage26_ffb_star_formation: the module under test.
  - sage26_determine_ffb_regime: at EnhancedStarFormationOn 1 the module's guard requires it in
    pre_timestep; it writes FFBRegime, which selects the rows the transaction acts on.
  - sage26_calculate_star_formation: the module's guard requires it immediately after.
  - sage26_apply_star_formation_supernova: sage26_calculate_star_formation refuses to run without
    it (sage26_calculate_star_formation.c, its init's dependency check).
  - sage26_set_disk_scale_radius: writes DiskScaleRadius, which the transaction reads for its
    dynamical time; without it the time is 0 and no star forms.
  - sage26_prepare_infall_budget, sage26_apply_infall, sage26_calculate_cooling_budget and
    sage26_apply_cooling: they supply the ColdGas the transaction forms stars from (each guard
    requires the module before it: the infall budget for the infall, the cooling budget for the
    cooling). sage26_reionization is optional to the budget and is left out.

Off is a no-op: with EnhancedStarFormationOn 0, the minimal pipeline with the module and the same
pipeline without it give byte-identical output in every field of every row of both snapshots.

On writes its domain (the observable list), with EnhancedStarFormationOn 1:
  - On the FFBRegime 1 rows at snapshot S = 16, each finite and non-negative: ColdGas,
    MetalsColdGas, StellarMass, MetalsStellarMass, H1gas, StarFormationRate and
    SupernovaOutflowRate (the last written by the shared feedback commit the transaction calls).
    Domains: models/sage26/model_properties.yaml:58-67, :180-189, :95-104, :158-167, :619-627,
    :140-152 and :351-363 (each a mass or rate, range [0.0, ...], creation value 0).
  - Witnesses: at least one FFBRegime 1 row with StarFormationRate > 0, and at least one with
    SupernovaOutflowRate > 0 (the feedback commit's outflow accumulation ran).
  - H2gas: exactly 0 on every FFBRegime 1 row at S. The transaction writes 0.0f, its creation value
    (model_properties.yaml:609-617), and sage26_calculate_star_formation
    returns at once on an FFBRegime 1 row.
  - MassLoading: finite and non-negative on the FFBRegime 1 rows at S, with no witness. It is
    written only with a valid FIRE scaling and otherwise left stale (model_properties.yaml:
    1246-1253).
  - The FoF central's reservoirs that feedback credits, HotGas, MetalsHotGas, CGMgas, MetalsCGMgas,
    EjectedGas and MetalsEjectedGas: finite and non-negative on every row at S, since other modules
    write them too (model_properties.yaml:69-78, :191-200, :506-514, :516-524, :80-89, :213-222).
  - Excluded: SfrDisk and SfrDiskZ, which sage26_finalize_sfr_histories writes from the non-output
    history bins; that module is not in the pipeline. The transaction's carried values (the stars
    formed, the pre-star-formation metallicity, the renormalised stars, the reheated and ejected
    masses) are locals of the call and never output.

Required-key refusal, isolated (a real run; mimic exits non-zero with "Required model parameter
'EnhancedSFEfficiency' not found" and "Module 'sage26_ffb_star_formation' initialization failed",
and no other module's initialization failure):
  - EnhancedSFEfficiency, at EnhancedStarFormationOn 0 so that no cross-module guard applies.
    Configuration: the minimal pipeline, in which only this module reads the key (asserted from
    the modules' parameters lists). Control: the same configuration with the key present exits 0.

Placement refusal: at EnhancedStarFormationOn 0 (so no cross-module guard applies), the module
configured twice, by galaxy in galaxy_physics and in satellite_mergers, every key present, is
refused naming the module (check_placement: exactly one substep entry, by galaxy).

Cross-module guards, at EnhancedStarFormationOn 1. Each configuration
breaks exactly one guard, and the case requires that guard's diagnostic:
  - Adjacency: pre_timestep sage26_determine_ffb_regime; galaxy_physics
    sage26_calculate_star_formation, sage26_ffb_star_formation,
    sage26_apply_star_formation_supernova (the transaction after, not immediately before,
    sage26_calculate_star_formation). Refused by the transaction, naming both modules.
  - Classifier: galaxy_physics sage26_ffb_star_formation, sage26_calculate_star_formation,
    sage26_apply_star_formation_supernova (correctly placed), with no sage26_determine_ffb_regime
    in pre_timestep. Refused by the transaction, naming sage26_determine_ffb_regime.
  The classifier's own guard, which requires this transaction, is exercised by
  sage26_determine_ffb_regime's integration test, beside the guard it tests.

These run-level cases repeat, at the executable's exit status, refusals the module's C unit tests
assert; they test no new behaviour.

Run (from the repository root, after `make MODEL=sage26 SIMULATION=mini-millennium`):
  MODEL=sage26 SIMULATION=mini-millennium python3 models/sage26/modules/\\
sage26_ffb_star_formation/_tests/test_integration_sage26_ffb_star_formation.py
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

MODULE = "sage26_ffb_star_formation"
CLASSIFIER = "sage26_determine_ffb_regime"
ORDINARY_SF = "sage26_calculate_star_formation"
APPLY_SF = "sage26_apply_star_formation_supernova"
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
    "GlobalBaryonFraction": 0.17,
    "BulgeSizeOn": 3,
}

SUPPLY_PRE = [
    ("sage26_set_disk_scale_radius", FH),
    ("sage26_prepare_infall_budget", FH),
]
SUPPLY_PHYSICS = [
    ("sage26_apply_infall", FH),
    ("sage26_calculate_cooling_budget", BG),
    ("sage26_apply_cooling", BG),
]
MINIMAL = {
    "pre_timestep": SUPPLY_PRE + [(CLASSIFIER, FH)],
    "galaxy_physics": SUPPLY_PHYSICS + [(MODULE, BG), (ORDINARY_SF, BG), (APPLY_SF, BG)],
}
MINIMAL_WITHOUT = {
    "pre_timestep": MINIMAL["pre_timestep"],
    "galaxy_physics": SUPPLY_PHYSICS + [(ORDINARY_SF, BG), (APPLY_SF, BG)],
}
TWICE = {
    "galaxy_physics": [(MODULE, BG)],
    "satellite_mergers": [(MODULE, BG)],
}
NOT_ADJACENT = {
    "pre_timestep": [(CLASSIFIER, FH)],
    "galaxy_physics": [(ORDINARY_SF, BG), (MODULE, BG), (APPLY_SF, BG)],
}
NO_CLASSIFIER = {
    "galaxy_physics": [(MODULE, BG), (ORDINARY_SF, BG), (APPLY_SF, BG)],
}

FFB_ROW_NON_NEGATIVE = (
    "ColdGas",
    "MetalsColdGas",
    "StellarMass",
    "MetalsStellarMass",
    "H1gas",
    "StarFormationRate",
    "SupernovaOutflowRate",
    "MassLoading",
)
CENTRAL_RESERVOIRS = (
    "HotGas",
    "MetalsHotGas",
    "CGMgas",
    "MetalsCGMgas",
    "EjectedGas",
    "MetalsEjectedGas",
)

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


def assert_finite_non_negative(rows, props, where):
    for prop in props:
        values = rows[prop]
        bad = int((~np.isfinite(values) | (values < 0)).sum())
        assert bad == 0, f"{where}: {bad} rows with {prop} not finite and >= 0"


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
        assert "SAGE FFB star formation module initialized" in stdout, "no initialization log"
        assert "SAGE FFB star formation module cleaned up" in stdout, "no cleanup log"
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
    """At EnhancedStarFormationOn 1 the FFB rows at S hold their domains and the witnesses."""
    require_pair()
    require_mimic()
    with mimic_run(f"{MODULE}_on", MINIMAL, parameters_for(MINIMAL)) as run:
        returncode, stdout, stderr, output_dir = run
        assert_runs(returncode, stdout, stderr)
        halos = load_snapshot(output_dir, FFB_SNAPSHOT)
        ffb = halos[halos["FFBRegime"] == 1]
        where = f"snapshot {FFB_SNAPSHOT}"
        assert len(ffb) > 0, f"{where}: no FFBRegime 1 row, so the transaction never acted"
        assert_finite_non_negative(ffb, FFB_ROW_NON_NEGATIVE, f"{where}, FFBRegime 1 rows")
        h2_nonzero = int((ffb["H2gas"] != 0).sum())
        assert h2_nonzero == 0, f"{where}: {h2_nonzero} FFBRegime 1 rows with H2gas not 0"
        assert_finite_non_negative(halos, CENTRAL_RESERVOIRS, f"{where}, every row")
        forming = int((ffb["StarFormationRate"] > 0).sum())
        outflowing = int((ffb["SupernovaOutflowRate"] > 0).sum())
        print(
            f"  {where}: {len(halos)} rows, FFBRegime 1: {len(ffb)}, with StarFormationRate > 0: "
            f"{forming}, with SupernovaOutflowRate > 0: {outflowing}, H2gas 0 on all"
        )
        assert forming > 0, f"witness: no FFBRegime 1 row with StarFormationRate > 0 at {where}"
        assert (
            outflowing > 0
        ), f"witness: no FFBRegime 1 row with SupernovaOutflowRate > 0 at {where}"


def test_required_key_enhancedsfefficiency_refused():
    """A missing EnhancedSFEfficiency is refused by this module alone; the control exits 0."""
    require_pair()
    require_mimic()
    key = "EnhancedSFEfficiency"
    readers = [name for name in configured_modules(MINIMAL) if key in module_parameters(name)]
    assert readers == [MODULE], f"configured readers of {key}: {readers}"
    off = {"EnhancedStarFormationOn": 0}

    with mimic_run(f"{MODULE}_key_control", MINIMAL, parameters_for(MINIMAL, off)) as run:
        returncode, stdout, stderr, _ = run
        assert_runs(returncode, stdout, stderr)
        print(f"  control ({key} present): exit status {returncode}")

    params = parameters_for(MINIMAL, off, omit=(key,))
    with mimic_run(f"{MODULE}_key_missing", MINIMAL, params) as run:
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
    """At EnhancedStarFormationOn 0, a second substep entry is refused, naming the module."""
    require_pair()
    require_mimic()
    params = parameters_for(TWICE, {"EnhancedStarFormationOn": 0})
    with mimic_run(f"{MODULE}_twice", TWICE, params) as run:
        returncode, stdout, stderr, _ = run
        assert_refused(
            returncode,
            stdout,
            stderr,
            [f"{MODULE} must be configured exactly once in a substep phase"],
        )


def test_not_adjacent_refused():
    """At EnhancedStarFormationOn 1, the transaction after the ordinary SF is refused."""
    require_pair()
    require_mimic()
    params = parameters_for(NOT_ADJACENT)
    assert params["EnhancedStarFormationOn"] == 1
    with mimic_run(f"{MODULE}_not_adjacent", NOT_ADJACENT, params) as run:
        returncode, stdout, stderr, _ = run
        assert_refused(
            returncode,
            stdout,
            stderr,
            [f"{MODULE} must run immediately before {ORDINARY_SF}"],
        )


def test_classifier_required_refused():
    """At EnhancedStarFormationOn 1, a placed transaction without the classifier is refused."""
    require_pair()
    require_mimic()
    params = parameters_for(NO_CLASSIFIER)
    assert params["EnhancedStarFormationOn"] == 1
    with mimic_run(f"{MODULE}_no_classifier", NO_CLASSIFIER, params) as run:
        returncode, stdout, stderr, _ = run
        assert_refused(
            returncode,
            stdout,
            stderr,
            [f"{MODULE} requires {CLASSIFIER} in modules.pre_timestep"],
        )


def main():
    tests = [
        test_loads_and_runs,
        test_off_is_noop,
        test_on_writes_domain,
        test_required_key_enhancedsfefficiency_refused,
        test_placement_refused,
        test_not_adjacent_refused,
        test_classifier_required_refused,
    ]
    return run_test_suite(tests, f"{MODULE} Integration Tests")


if __name__ == "__main__":
    sys.exit(main())
