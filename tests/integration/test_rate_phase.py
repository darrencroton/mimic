#!/usr/bin/env python3
"""
Rate phase: the modules.rate_integration run-file section and the rate fixtures through real runs.

Validates, on the selected package's committed fixture in a test build (the default pair runs the
vertical driver; MODEL=halos-only SIMULATION=mini-millennium-horizontal the horizontal driver):
  - test_run_file_section_refusals: a modules.rate_integration section with no rate phase (as
    a mapping or as null), with an unknown or non-scalar key, or with a value out of range (each
    of the six keys), a section that is not a mapping, and a substep phase named
    rate_integration, each end the run before any module's init() with a message naming the key
  - test_explicit_scheme: the four rate fixtures in one rate phase, with a null section (every
    default), complete under the default (explicit) scheme; every one of the seven fixture
    properties is non-zero on some row, and no reservoir, tracer or observer value is negative
    or non-finite; no leak is reported
  - test_implicit_scheme: the same under scheme: implicit, with all five keys set
  - test_dynamic_timestepping: the same under TimestepScheme: dynamic
  - test_stiff_rates: the same fixtures with their rate coefficients scaled by 1e3 under the
    explicit scheme: RunProperties/RateIntegration records rejected steps and a largest K_max
    above the positivity cap, and every fixture output value is finite and not negative
  - test_created_records_join: with test_fixture creating records in pre_timestep, the run
    completes and every created row's fixture values are finite and non-negative (that a created
    record joins its component at zero content is shown by tests/unit/test_rate_dispatch.c)
  - test_failing_laws_write_a_failure_file: test_rate_faults selecting the diverging coefficient,
    NaN and the negative coefficient in turn ends each run non-zero, naming the module, its
    transfer and the cause, and leaves rate_failure_task0.txt in the output directory with the
    segment-start state, each row's Delta and the failing evaluation's coefficients as
    hexadecimal floats; the well-behaved law completes and writes no file
  - test_purity_probe_catches_the_impure_law: the impure law with check_purity true ends the run
    non-zero naming the module and transfer; with check_purity false the same run completes
  - test_purity_probe_leaves_output_unchanged: with the four well-behaved fixtures, check_purity
    true gives Galaxies datasets byte-identical to the same run with it false, under both schemes
  - check_purity with any value but true or false is refused at startup (in
    test_run_file_section_refusals)
  - test_lossless_jump_records_no_change: test_rate_jump merging satellites without loss in a
    discrete phase after the rate phase completes, merges some satellites, and the master file's
    RunProperties/RateIntegration/DeclaredDiscreteChanges counts are zero for both totals
  - test_lossy_jump_is_counted: the same with a declared loss completes, and both totals'
    declared-change counts are non-zero
  - test_undeclared_change_is_fatal: test_rate_faults in its full-halo mode, adding mass in a
    discrete phase without declaring it, ends the run naming the phase, the module and the total
  - test_undeclared_change_beside_a_declaring_module: test_rate_jump (declaring both totals) and
    test_rate_faults in its full-halo mode in one discrete phase end the run naming
    test_rate_faults and the total: a declaration covers only its own dispatch unit

The rate modules and their parameters are read from the rate battery's run file (see
tests/framework/rate_fixture.py). The seven fixture properties
(src/module_system/test_fixture/test_properties.yaml) are written to
output in every test build, so the checks read them from the run's HDF5 galaxies.
"""

import math
import re
import shutil
import sys
from pathlib import Path

import h5py
import numpy as np
import yaml

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "tests"))

from framework import (  # noqa: E402
    MIMIC_EXE,
    TestSkipped,
    check_no_memory_leaks,
    core_input_file,
    create_test_param_file,
    run_mimic,
    run_test_suite,
)
from framework.rate_fixture import (  # noqa: E402
    POSITIVITY_CAP,
    STIFF_FACTOR,
    load_rate_fixture,
    read_step_extremes,
    scale_coefficients,
)

# The four fixtures and their law parameters (each fixture's README) come from the run file the
# rate battery uses, simulations/mini-millennium-horizontal/_tests/input/forest_blocks_rate.yaml.
# The rates are in inverse internal time units and a snapshot interval is about 3e-4 of them, so
# these defaults give a largest K_max near 0.0045, far below the positivity cap; the threshold is
# low enough to let stars form even on the two-galaxy horizontal fixture, whose cold content
# stays near 1e-5. test_stiff_rates scales the rates up to reach the cap.
RATE_FIXTURES, RATE_PARAMS = load_rate_fixture()

RESERVOIR_FIELDS = (
    "RateFixtureHot",
    "RateFixtureCold",
    "RateFixtureStars",
    "RateFixtureHotTracer",
    "RateFixtureColdTracer",
    "RateFixtureStarsTracer",
)
FIXTURE_FIELDS = RESERVOIR_FIELDS + ("RateFixtureFormed",)

# The fault injector (src/module_system/test_rate_faults) rides in the rate phase beside the
# supply fixture, which feeds the cold reservoir its sink drains. TestRateFaultsLaw selects the
# law; the rate is its scale.
FAULTS_MODULE = "test_rate_faults"
FAULTS_RATE = 1.0

# Logged by test_rate_supply's init(); absent when startup refused the run before any init().
SUPPLY_INIT_LINE = "Test rate supply fixture initialized"

# The discrete jump (src/module_system/test_rate_jump): a full-halo module in a discrete phase
# after the rate phase, merging satellites whose stars exceed the threshold into the central.
# The threshold is low enough that satellites of the committed fixture merge.
JUMP_MODULE = "test_rate_jump"
JUMP_PHASE = "jumps"
JUMP_THRESHOLD = 1.0e-9
JUMP_MERGES = re.compile(r"Test rate jump fixture: (\d+) satellite\(s\) merged")
FIXTURE_TOTALS = ("rate_fixture_mass", "rate_fixture_tracer")

_temp_dirs = []


def _rate_phase():
    return {"rates": [(name, "process_rates") for name in RATE_FIXTURES]}


# _run()'s edit_section default: leave the section as create_test_param_file() wrote it.
_KEEP = object()


def _run(name, phase_config, model_params=None, edit_section=_KEEP, **overrides):
    """Write a run file on the committed HDF5 fixture, run it, return (rc, log, output_dir).

    edit_section, when given, replaces modules.rate_integration in the written file with that
    value verbatim, None included (written as null), for spellings the harness does not emit.
    """
    if not MIMIC_EXE.exists():
        raise TestSkipped("Mimic not built")
    ref = core_input_file("test_hdf5.yaml")  # skips for a package with no committed fixture
    param_file, output_dir, temp_dir = create_test_param_file(
        f"rate_phase_{name}",
        phase_config=phase_config,
        model_params=model_params,
        ref_param_file=ref,
        output_format="hdf5",
        **overrides,
    )
    _temp_dirs.append(temp_dir)
    if edit_section is not _KEEP:
        with open(param_file) as handle:
            config = yaml.safe_load(handle)
        config["modules"]["rate_integration"] = edit_section
        with open(param_file, "w") as handle:
            yaml.dump(config, handle, default_flow_style=False, sort_keys=False)
    returncode, stdout, stderr = run_mimic(param_file)
    return returncode, stdout + "\n" + stderr, output_dir


def _galaxies(output_dir):
    """Every Galaxies row of every per-file HDF5 output, concatenated."""
    files = sorted(Path(output_dir).glob("model_*.hdf5"))
    assert files, f"no per-file HDF5 output in {output_dir}"
    parts = []
    for path in files:
        with h5py.File(path, "r") as handle:
            for key in sorted(handle.keys()):
                if key.startswith("Snap") and "Galaxies" in handle[key]:
                    parts.append(handle[key]["Galaxies"][:])
    assert parts, f"no Galaxies dataset in {output_dir}"
    return np.concatenate(parts)


def _assert_fixture_output(galaxies, label):
    """Every fixture property non-zero somewhere; none negative or non-finite anywhere."""
    for field in FIXTURE_FIELDS:
        values = np.asarray(galaxies[field], dtype=np.float64)
        bad = ~np.isfinite(values)
        assert not bad.any(), f"{label}: {field} has {int(bad.sum())} non-finite value(s)"
        assert (values >= 0.0).all(), f"{label}: {field} has a negative value ({values.min()!r})"
        assert np.count_nonzero(values) > 0, f"{label}: {field} is zero on every row"


def _assert_completed(returncode, log, label):
    assert returncode == 0, f"{label}: the run failed (exit {returncode})\n{log[-4000:]}"
    assert check_no_memory_leaks(log), f"{label}: the run reported a memory leak"


def test_run_file_section_refusals():
    """Each refusal of modules.rate_integration ends the run before any init(), naming the key."""
    fixtures = _rate_phase()
    cases = [
        (
            "no_rate_phase",
            {},
            {"scheme": "explicit"},
            "modules.rate_integration is set, but no phase under modules.phases runs "
            "process_rates",
        ),
        (
            "unknown_key",
            fixtures,
            {"method": "explicit"},
            "Unknown key 'modules.rate_integration.method'",
        ),
        (
            "bad_scheme",
            fixtures,
            {"scheme": "rk4"},
            "Unknown modules.rate_integration.scheme 'rk4'",
        ),
        (
            "relative_zero",
            fixtures,
            {"relative_tolerance": 0},
            "modules.rate_integration.relative_tolerance must lie in (0, 1), got 0",
        ),
        (
            "relative_one",
            fixtures,
            {"relative_tolerance": 1},
            "modules.rate_integration.relative_tolerance must lie in (0, 1), got 1",
        ),
        (
            "absolute_negative",
            fixtures,
            {"absolute_tolerance": -0.001},
            "modules.rate_integration.absolute_tolerance must lie in (0, 1), got -0.001",
        ),
        (
            "absolute_not_a_number",
            fixtures,
            {"absolute_tolerance": "tiny"},
            "modules.rate_integration.absolute_tolerance must be a valid finite number",
        ),
        (
            "steps_zero",
            fixtures,
            {"max_steps_per_segment": 0},
            "modules.rate_integration.max_steps_per_segment must be >= 1, got 0",
        ),
        (
            "rejections_zero",
            fixtures,
            {"max_rejections_per_step": 0},
            "modules.rate_integration.max_rejections_per_step must be >= 20, got 0",
        ),
        (
            "rejections_fraction",
            fixtures,
            {"max_rejections_per_step": 2.5},
            "modules.rate_integration.max_rejections_per_step must be a valid integer",
        ),
        (
            "purity_word",
            fixtures,
            {"check_purity": "yes"},
            "modules.rate_integration.check_purity must be true or false, got 'yes'",
        ),
        (
            "purity_capitalised",
            fixtures,
            {"check_purity": "True"},
            "modules.rate_integration.check_purity must be true or false, got 'True'",
        ),
        (
            "purity_number",
            fixtures,
            {"check_purity": 1},
            "modules.rate_integration.check_purity must be true or false, got '1'",
        ),
        (
            "purity_mapping",
            fixtures,
            {"check_purity": {"value": True}},
            "modules.rate_integration.check_purity must be a scalar, true or false",
        ),
        (
            "purity_without_rate_phase",
            {},
            {"check_purity": True},
            "modules.rate_integration is set, but no phase under modules.phases runs "
            "process_rates",
        ),
        ("not_a_mapping", fixtures, ["explicit"], "modules.rate_integration must be a mapping"),
        (
            "reserved_phase_name",
            {"rate_integration": [(name, "process_rates") for name in RATE_FIXTURES]},
            None,
            "Substep phase name 'rate_integration' is reserved",
        ),
    ]
    runs = [
        (name, phase_config, fragment, {"rate_integration": section})
        for name, phase_config, section, fragment in cases
    ]
    # Spellings written into the run file directly: a null section with no rate phase, and a
    # key that is not a scalar (a YAML sequence key, which PyYAML writes for a tuple).
    runs += [
        (
            "null_without_rate_phase",
            {},
            "modules.rate_integration is set, but no phase under modules.phases runs "
            "process_rates",
            {"edit_section": None},
        ),
        (
            "non_scalar_key",
            fixtures,
            "modules.rate_integration has a key that is not a scalar",
            {"edit_section": {("scheme",): 7}},
        ),
    ]
    for name, phase_config, fragment, section_args in runs:
        returncode, log, _ = _run(
            f"refuse_{name}", phase_config, model_params=RATE_PARAMS, **section_args
        )
        assert returncode != 0, f"{name}: the run was accepted"
        assert fragment in log, f"{name}: the refusal does not say {fragment!r}\n{log[-3000:]}"
        assert SUPPLY_INIT_LINE not in log, f"{name}: a module init() ran before the refusal"


def test_explicit_scheme():
    """The four rate fixtures run under the default (explicit) scheme with sound output.

    The section is written as null, which with a rate phase keeps every default.
    """
    returncode, log, output_dir = _run(
        "explicit", _rate_phase(), model_params=RATE_PARAMS, edit_section=None
    )
    _assert_completed(returncode, log, "explicit")
    _assert_fixture_output(_galaxies(output_dir), "explicit")


def test_implicit_scheme():
    """The four rate fixtures run under scheme: implicit, every key set, with sound output."""
    section = {
        "scheme": "implicit",
        "relative_tolerance": 1.0e-3,
        "absolute_tolerance": 1.0e-6,
        "max_steps_per_segment": 50000,
        "max_rejections_per_step": 20,
    }
    returncode, log, output_dir = _run(
        "implicit", _rate_phase(), model_params=RATE_PARAMS, rate_integration=section
    )
    _assert_completed(returncode, log, "implicit")
    _assert_fixture_output(_galaxies(output_dir), "implicit")


def test_dynamic_timestepping():
    """The four rate fixtures run under TimestepScheme: dynamic with sound output."""
    returncode, log, output_dir = _run(
        "dynamic",
        _rate_phase(),
        model_params=RATE_PARAMS,
        timestep_scheme="dynamic",
        substeps=3,
    )
    _assert_completed(returncode, log, "dynamic")
    _assert_fixture_output(_galaxies(output_dir), "dynamic")


def test_stiff_rates():
    """Rate coefficients scaled by 1e3 drive the explicit scheme to its positivity cap.

    The scaling is an explicit override of the run file's parameters. With the default rates no
    segment's largest K_max comes within a factor of 100 of the cap, so the cap, the retry rule
    and repeated rejections never run through dispatch; here the master file's
    RunProperties/RateIntegration must record rejected steps and a largest K_max above the cap,
    and every fixture output value must still be finite and not negative, with each fixture
    property non-zero on some row.
    """
    stiff = scale_coefficients(RATE_PARAMS, STIFF_FACTOR)
    assert stiff != RATE_PARAMS, "the override scaled no parameter"
    returncode, log, output_dir = _run(
        "stiff",
        _rate_phase(),
        model_params=stiff,
        rate_integration={"scheme": "explicit"},
    )
    _assert_completed(returncode, log, "stiff")
    rejected, k_max = read_step_extremes(Path(output_dir) / "model.hdf5")
    assert rejected > 0, "no step was rejected, so the retry rule never ran"
    assert (
        k_max > POSITIVITY_CAP
    ), f"largest K_max {k_max:.3g} is not above the cap {POSITIVITY_CAP}"
    _assert_fixture_output(_galaxies(output_dir), "stiff")


def test_created_records_join():
    """Records test_fixture creates in pre_timestep run through the rate phase with sound values."""
    phase_config = {"pre_timestep": [("test_fixture", "process_full_halo")], **_rate_phase()}
    params = dict(RATE_PARAMS)
    params.update(
        {
            "TestFixtureDummyParameter": 0.25,
            "TestFixtureEnableLogging": 0,
            "TestFixtureCreateRecords": 1,
        }
    )
    returncode, log, output_dir = _run("created", phase_config, model_params=params)
    _assert_completed(returncode, log, "created")
    galaxies = _galaxies(output_dir)
    _assert_fixture_output(galaxies, "created")
    created = galaxies[galaxies["UniqueGalaxyID"] < 0]
    assert len(created) > 0, "test_fixture created no record"
    for field in FIXTURE_FIELDS:
        values = np.asarray(created[field], dtype=np.float64)
        assert (
            np.isfinite(values).all() and (values >= 0.0).all()
        ), f"a created record has a negative or non-finite {field}"


HEX_FLOAT = r"[-+]?0x[0-9a-f.]+p[-+]\d+"


def _fault_phase():
    """A rate phase of test_rate_supply (it feeds Cold) and the fault injector."""
    return {"rates": [("test_rate_supply", "process_rates"), (FAULTS_MODULE, "process_rates")]}


def _fault_params(law):
    params = dict(RATE_PARAMS)
    params.update({"TestRateFaultsLaw": law, "TestRateFaultsRate": FAULTS_RATE})
    return params


def _parse_failure_file(path):
    """Return ({key: value} header lines, [state rows], [coefficient rows], [row clock lines])."""
    header, states, coefficients, rows = {}, [], [], []
    for line in Path(path).read_text().splitlines():
        if line.startswith("#") or not line.strip():
            continue
        if line.startswith("state "):
            states.append(line.split())
        elif line.startswith("coefficient "):
            coefficients.append(line.split())
        elif line.startswith("row "):
            rows.append(line.split())
        else:
            key, _, value = line.partition(": ")
            header[key] = value
    return header, states, coefficients, rows


def test_failing_laws_write_a_failure_file():
    """Each failing fault law ends the run naming module, transfer and cause, with a file.

    The failure file is <output_directory>/rate_failure_task0.txt in a serial run; it holds the
    component's segment-start state, each row's Delta and the failing evaluation's coefficients
    as hexadecimal floats. The well-behaved law completes and leaves no file.
    """
    cases = (
        ("diverging", "step_underflow"),
        ("not_a_number", "bad_coefficient"),
        ("negative", "bad_coefficient"),
    )
    for law, cause in cases:
        returncode, log, output_dir = _run(
            f"fault_{law}", _fault_phase(), model_params=_fault_params(law)
        )
        assert returncode != 0, f"{law}: the run was accepted"
        for fragment in (
            f"cause: {cause}",
            "module 'test_rate_faults', transfer 'fault'",
            f"Rate segment failed: cause {cause}, module 'test_rate_faults', transfer 'fault'",
        ):
            assert fragment in log, f"{law}: the message lacks {fragment!r}\n{log[-3000:]}"
        path = Path(output_dir) / "rate_failure_task0.txt"
        assert path.is_file(), f"{law}: no failure file at {path}"
        assert f"failure file: {path}" in log, f"{law}: the message does not name the file"

        header, states, coefficients, rows = _parse_failure_file(path)
        assert header["cause"] == cause, f"{law}: file cause {header['cause']!r}"
        assert header["module"] == "test_rate_faults" and header["transfer"] == "fault"
        assert header["task"] == "0" and header["check_purity"] == "false"
        assert len(rows) == int(header["rows"]) >= 1, f"{law}: no row clock lines"
        # row <r> workspace_row <w> unique_galaxy_id <id> type <t> dT <h> delta <h>
        for row in rows:
            assert row[8] == "dT" and row[10] == "delta", f"{law}: row line {row}"
            for value in (row[9], row[11]):
                assert re.fullmatch(HEX_FLOAT, value), f"{law}: dT or delta not a hex float: {row}"
            assert float.fromhex(row[11]) > 0.0, f"{law}: a failing row has a zero Delta: {row}"
        per_row = int(header["entries_per_row"])
        assert len(states) == len(rows) * per_row, f"{law}: {len(states)} state lines"
        names = {state[3] for state in states}
        assert set(RESERVOIR_FIELDS) <= names, f"{law}: state lacks reservoirs: {names}"
        for state in states:  # state <r> <slot> <property> <hex>
            assert re.fullmatch(HEX_FLOAT, state[4]), f"{law}: state not a hex float: {state}"
            assert float.fromhex(state[4]) >= 0.0, f"{law}: negative start state {state}"
        assert len(coefficients) == len(rows) * int(header["transfers_per_row"])

        fault = [c for c in coefficients if c[2] == "test_rate_faults.fault"]
        assert len(fault) == len(rows), f"{law}: the fault coefficient is not listed per row"
        values = [float.fromhex(c[3]) if c[3] != "nan" else math.nan for c in fault]
        if law == "not_a_number":
            assert all(c[3] == "nan" and c[4].startswith("bits=0x7ff8") for c in fault), fault
        elif law == "negative":
            assert -1.0 in values, f"negative: the coefficient -1 is not in {fault}"
        else:
            assert max(values) > 1.0e3 * FAULTS_RATE, f"diverging: no large rate in {fault}"
        for coefficient in coefficients:  # every other coefficient is a hex float or NaN
            assert re.fullmatch(HEX_FLOAT, coefficient[3]) or coefficient[3] == "nan", coefficient

    returncode, log, output_dir = _run(
        "fault_well_behaved", _fault_phase(), model_params=_fault_params("well_behaved")
    )
    _assert_completed(returncode, log, "well_behaved")
    assert not list(Path(output_dir).glob("rate_failure_task*.txt")), "a clean run wrote a file"


def test_purity_probe_catches_the_impure_law():
    """The impure law ends a check_purity run naming module and transfer; off, it completes."""
    returncode, log, output_dir = _run(
        "purity_impure_on",
        _fault_phase(),
        model_params=_fault_params("impure"),
        rate_integration={"check_purity": True},
    )
    assert returncode != 0, "the impure law was accepted under check_purity: true"
    for fragment in (
        "Rate law is impure: module 'test_rate_faults', transfer 'fault'",
        "on the first evaluation and",
    ):
        assert fragment in log, f"the message lacks {fragment!r}\n{log[-3000:]}"
    path = Path(output_dir) / "rate_failure_task0.txt"
    assert path.is_file(), "the purity failure left no failure file"
    header, _, _, _ = _parse_failure_file(path)
    assert header["cause"] == "impure_law" and header["check_purity"] == "true"

    returncode, log, _ = _run(
        "purity_impure_off",
        _fault_phase(),
        model_params=_fault_params("impure"),
        rate_integration={"check_purity": False},
    )
    _assert_completed(returncode, log, "impure law, check_purity false")


def test_purity_probe_leaves_output_unchanged():
    """With the four well-behaved fixtures, check_purity true gives byte-identical Galaxies."""
    for scheme in ("explicit", "implicit"):
        outputs = {}
        for purity in (False, True):
            section = {"scheme": scheme, "check_purity": purity}
            returncode, log, output_dir = _run(
                f"purity_equal_{scheme}_{purity}",
                _rate_phase(),
                model_params=RATE_PARAMS,
                rate_integration=section,
            )
            _assert_completed(returncode, log, f"{scheme}, check_purity {purity}")
            outputs[purity] = _galaxies(output_dir)
        _assert_fixture_output(outputs[True], f"{scheme} with the probe")
        assert outputs[True].dtype == outputs[False].dtype
        assert outputs[True].shape == outputs[False].shape
        assert (
            outputs[True].tobytes() == outputs[False].tobytes()
        ), f"{scheme}: check_purity true changed the Galaxies datasets"


def _jump_params(loss, faults=False):
    params = dict(RATE_PARAMS)
    params.update({"TestRateJumpStarsThreshold": JUMP_THRESHOLD, "TestRateJumpLossFraction": loss})
    if faults:
        params.update(_fault_params("well_behaved"))
        params["TestRateFaultsAddedMass"] = 1.0e-3
    return params


def _declared_changes(output_dir):
    """Return {total: count} from the master file's RunProperties/RateIntegration."""
    with h5py.File(Path(output_dir) / "model.hdf5", "r") as handle:
        rows = handle["RunProperties/RateIntegration/DeclaredDiscreteChanges"][:]
    return {row["total"].decode(): int(row["count"]) for row in rows}


def _merges(log):
    found = JUMP_MERGES.findall(log)
    assert found, f"test_rate_jump logged no merge count\n{log[-3000:]}"
    return sum(int(count) for count in found)


def test_lossless_jump_records_no_change():
    """Merging satellites without loss completes, and the ledger records no change."""
    phase_config = {**_rate_phase(), JUMP_PHASE: [(JUMP_MODULE, "process_full_halo")]}
    returncode, log, output_dir = _run(
        "jump_lossless", phase_config, model_params=_jump_params(0.0)
    )
    _assert_completed(returncode, log, "lossless jump")
    assert _merges(log) > 0, "test_rate_jump merged no satellite, so nothing was exercised"
    changes = _declared_changes(output_dir)
    assert set(changes) == set(FIXTURE_TOTALS), f"declared-change totals {sorted(changes)}"
    assert all(count == 0 for count in changes.values()), f"lossless merges counted: {changes}"


def test_lossy_jump_is_counted():
    """A declared loss completes, and every total's declared-change count is non-zero."""
    phase_config = {**_rate_phase(), JUMP_PHASE: [(JUMP_MODULE, "process_full_halo")]}
    returncode, log, output_dir = _run("jump_lossy", phase_config, model_params=_jump_params(0.25))
    _assert_completed(returncode, log, "lossy jump")
    merges = _merges(log)
    changes = _declared_changes(output_dir)
    assert set(changes) == set(FIXTURE_TOTALS), f"declared-change totals {sorted(changes)}"
    for total, count in changes.items():
        assert 0 < count <= merges, f"{total}: {count} declared change(s) for {merges} merge(s)"


def test_undeclared_change_is_fatal():
    """test_rate_faults adding mass undeclared in a discrete phase ends the run, naming both."""
    phase_config = {**_rate_phase(), "kicks": [(FAULTS_MODULE, "process_full_halo")]}
    params = _jump_params(0.0, faults=True)
    returncode, log, _ = _run("undeclared", phase_config, model_params=params)
    assert returncode != 0, "an undeclared change to a conserved total was accepted"
    fragment = (
        "Undeclared change to conserved total 'rate_fixture_mass' in phase 'kicks' by module "
        "'test_rate_faults'"
    )
    assert fragment in log, f"the message lacks {fragment!r}\n{log[-3000:]}"


def test_undeclared_change_beside_a_declaring_module():
    """A declaring module in the same phase does not cover test_rate_faults' own unit."""
    phase_config = {
        **_rate_phase(),
        JUMP_PHASE: [(JUMP_MODULE, "process_full_halo"), (FAULTS_MODULE, "process_full_halo")],
    }
    returncode, log, _ = _run(
        "undeclared_mixed", phase_config, model_params=_jump_params(0.25, faults=True)
    )
    assert returncode != 0, "an undeclared change beside a declaring module was accepted"
    fragment = (
        f"Undeclared change to conserved total 'rate_fixture_mass' in phase '{JUMP_PHASE}' by "
        "module 'test_rate_faults'"
    )
    assert fragment in log, f"the message lacks {fragment!r}\n{log[-3000:]}"


def _cleanup():
    for temp_dir in _temp_dirs:
        shutil.rmtree(temp_dir, ignore_errors=True)


def main():
    try:
        return run_test_suite(
            [
                test_run_file_section_refusals,
                test_explicit_scheme,
                test_implicit_scheme,
                test_dynamic_timestepping,
                test_stiff_rates,
                test_created_records_join,
                test_failing_laws_write_a_failure_file,
                test_purity_probe_catches_the_impure_law,
                test_purity_probe_leaves_output_unchanged,
                test_lossless_jump_records_no_change,
                test_lossy_jump_is_counted,
                test_undeclared_change_is_fatal,
                test_undeclared_change_beside_a_declaring_module,
            ],
            "Rate Phase (test_rate_phase.py)",
        )
    finally:
        _cleanup()


if __name__ == "__main__":
    sys.exit(main())
