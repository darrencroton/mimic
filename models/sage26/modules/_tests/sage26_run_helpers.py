"""
Shared helpers of the sage26 module integration tests that drive real mimic runs.

The integration tests of sage26_determine_ffb_regime, sage26_determine_regime,
sage26_ffb_star_formation, sage26_regime_aware_cooling and sage26_set_halo_concentration each
configure minimal pipelines from the modules' module_info.yaml `parameters` lists, run mimic on
the committed mini-millennium test trees, and compare the written snapshots or assert a refusal.
This module holds the machinery they share; each test keeps its own pipelines, parameter values
and output snapshots and binds them with `PackageRuns`:

    RUNS = PackageRuns(MODULE, PARAMETER_VALUES, OUTPUT_FILES, SUBSTEPS)

A test imports it after putting this directory on `sys.path`, beside the framework's.
"""

import re
import shutil
from contextlib import contextmanager
from pathlib import Path

import numpy as np
import yaml
from framework import (
    MIMIC_EXE,
    TestNotApplicable,
    TestSkipped,
    check_no_memory_leaks,
    compiled_model,
    compiled_simulation,
    create_test_param_file,
    load_binary_halos,
    run_mimic,
)

MODULES_DIR = Path(__file__).resolve().parents[1]
INIT_FAILED = re.compile(r"Module '([^']+)' initialization failed")


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


def assert_runs(returncode, stdout, stderr):
    assert returncode == 0, f"mimic exited {returncode}\nSTDERR:\n{stderr[-2000:]}"
    assert check_no_memory_leaks(stdout, stderr), "mimic reported a memory leak"


class PackageRuns:
    """
    Runs of one module's test: its parameter values, written snapshots and substep count.

    Args:
        module: the module under test, the default subject of `assert_refused`
        parameter_values: the value of every key the test's configurations read
        output_files: written snapshot -> its binary output file
        substeps: the runs' SubSteps
    """

    def __init__(self, module, parameter_values, output_files, substeps):
        self.module = module
        self.parameter_values = parameter_values
        self.output_files = output_files
        self.substeps = substeps

    def parameters_for(self, phases, overrides=None, omit=()):
        """Every key the configured modules read, with the run file's values."""
        keys = sorted(
            {key for name in configured_modules(phases) for key in module_parameters(name)}
        )
        missing = [key for key in keys if key not in self.parameter_values]
        assert not missing, f"no value for keys the configured modules read: {missing}"
        params = {key: self.parameter_values[key] for key in keys if key not in omit}
        params.update(overrides or {})
        return params

    @contextmanager
    def mimic_run(self, name, phases, params):
        """Run mimic on a generated run file; yield (returncode, stdout, stderr, output_dir)."""
        param_file, output_dir, temp_dir = create_test_param_file(
            output_name=name,
            phase_config=phases,
            model_params=params,
            substeps=self.substeps,
            snapshot_list=list(self.output_files),
        )
        try:
            returncode, stdout, stderr = run_mimic(param_file)
            yield returncode, stdout, stderr, output_dir
        finally:
            shutil.rmtree(temp_dir, ignore_errors=True)

    def load_snapshot(self, output_dir, snapshot):
        halos, _metadata = load_binary_halos(output_dir / self.output_files[snapshot])
        assert len(halos) > 0, f"snapshot {snapshot} has no output rows"
        return halos

    def assert_outputs_identical(self, dir_a, dir_b):
        """Every field of every row of every written snapshot is byte-identical."""
        for snapshot in self.output_files:
            a = self.load_snapshot(dir_a, snapshot)
            b = self.load_snapshot(dir_b, snapshot)
            assert a.dtype.names == b.dtype.names, f"snapshot {snapshot}: property lists differ"
            assert len(a) == len(b), f"snapshot {snapshot}: {len(a)} rows against {len(b)}"
            for field in a.dtype.names:
                bytes_a = np.ascontiguousarray(a[field]).view(np.uint8).reshape(len(a), -1)
                bytes_b = np.ascontiguousarray(b[field]).view(np.uint8).reshape(len(b), -1)
                differing = int(np.any(bytes_a != bytes_b, axis=1).sum())
                assert differing == 0, f"snapshot {snapshot}: {field} differs on {differing} rows"
            print(f"  snapshot {snapshot}: {len(a)} rows, {len(a.dtype.names)} fields identical")

    def assert_refused(self, returncode, stdout, stderr, needles, module=None):
        """Exit non-zero, every needle present, and only `module`'s initialization failure."""
        module = module or self.module
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
