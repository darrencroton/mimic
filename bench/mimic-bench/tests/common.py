"""Shared helpers of the bench's unittest suite: paths, the shipped spec, the Mimic checkout.

Helper module, not a test module. Test modules put the package directory on ``sys.path`` and
import what they need from here. It holds what a shared fixture module would:

* :data:`BENCH_DIR` is ``bench/mimic-bench`` (where the shipped spec and constraint table
  live); :data:`REPO_ROOT` is the Mimic repository the bench lives in.
* :func:`shipped_spec` loads the shipped spec once; :func:`raw_spec` gives a fresh mutable copy of
  its parsed YAML.
* :func:`require_mimic_executable` skips a test, naming the reason, when ``./mimic`` is not built
  or the simulation's tree files are absent; :func:`fake_mimic_root` builds a stand-in Mimic root
  for the tests that must run without them.
* :func:`load_template` and :func:`load_constraint_table` read a run-file template or a constraint
  table from a path, as the package parses them from bytes.
* :class:`BenchTestCase` switches the ``analyse`` stage's figure rendering off for every test
  except one decorated with :func:`renders_figures`.
* :func:`assert_approx` compares numbers to a relative and an absolute tolerance, whichever is
  larger.
* :func:`tmp_path` and :func:`module_tmp_path` give a temporary directory removed at the end of
  the test, the class or the module.
"""

import copy
import functools
import os
import shutil
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

import numpy as np
import yaml

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from mimic_bench.capture import CaptureError, tree_files  # noqa: E402
from mimic_bench.constraints import ConstraintTable, parse_constraint_table  # noqa: E402
from mimic_bench.runfile import parse_template  # noqa: E402
from mimic_bench.spec import Spec, load_spec  # noqa: E402
from mimic_bench.statistics import HELPER_DIRECTORY, HELPER_MODULES  # noqa: E402

BENCH_DIR = Path(__file__).resolve().parents[1]
"""``bench/mimic-bench``: the bench's own directory."""

REPO_ROOT = Path(__file__).resolve().parents[3]
"""The Mimic repository the bench lives in (``bench/mimic-bench/tests/common.py``)."""

SHIPPED_SPEC = BENCH_DIR / "specs" / "sage16-smf.yaml"
"""The spec the bench ships."""


@functools.cache
def shipped_spec() -> Spec:
    """The shipped spec, loaded and validated once per process."""
    return load_spec(SHIPPED_SPEC)


def raw_spec() -> dict:
    """A fresh, mutable copy of the shipped spec as parsed YAML."""
    return copy.deepcopy(yaml.safe_load(SHIPPED_SPEC.read_text()))


def mimic_root() -> Path:
    """The Mimic checkout the bench lives in, for tests that read its files but run nothing."""
    return REPO_ROOT


def require_mimic_executable() -> Path:
    """Return the Mimic root, or skip the calling test when it cannot run ``./mimic`` here.

    Skips, with the reason, when ``<root>/mimic`` is not an executable file or when a tree file
    of the shipped spec's simulation (:func:`mimic_bench.capture.tree_files` of its
    ``simulation_info.yaml``) is absent.

    Raises:
        unittest.SkipTest: Always, when the executable or the tree files are missing.
    """
    executable = REPO_ROOT / "mimic"
    if not (executable.is_file() and os.access(executable, os.X_OK)):
        raise unittest.SkipTest(f"{executable} is not built (run make): this test runs it")
    info_path = REPO_ROOT / "simulations" / shipped_spec().simulation / "simulation_info.yaml"
    try:
        info = yaml.load(info_path.read_text(), Loader=yaml.BaseLoader)["input"]
        directory, names = tree_files(REPO_ROOT, info)
    except (OSError, yaml.YAMLError, KeyError, TypeError, CaptureError) as exc:
        raise unittest.SkipTest(
            f"{info_path}: cannot read the simulation's file range ({exc})"
        ) from exc
    trees = [directory / name for name in names]
    missing = [path for path in trees if not path.is_file()]
    if missing:
        raise unittest.SkipTest(
            f"the simulation's tree files are absent ({len(missing)} of {len(trees)} missing, "
            f"first {missing[0]}): this test runs ./mimic on them"
        )
    return REPO_ROOT


def fake_mimic_root(directory: Path, script: str, template_extra: str = "") -> Path:
    """A stand-in Mimic root under ``directory``, with ``script`` as ``./mimic``.

    It holds copies of the files ``prepare`` reads (the shipped spec's template, with
    ``template_extra`` appended, the simulation's ``simulation_info.yaml`` and Mimic's helper
    modules) and an empty file for each tree file of the simulation's range.
    """
    spec = shipped_spec()
    root = directory / "root"
    helpers = [(HELPER_DIRECTORY / f"{name}.py").as_posix() for name in HELPER_MODULES]
    simulation_info = f"simulations/{spec.simulation}/simulation_info.yaml"
    for relative in (spec.run_file.template, simulation_info, *helpers):
        (root / relative).parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(REPO_ROOT / relative, root / relative)
    template = root / spec.run_file.template
    template.write_text(template.read_text() + template_extra)
    info = yaml.load((root / simulation_info).read_text(), Loader=yaml.BaseLoader)["input"]
    trees, names = tree_files(root, info)
    trees.mkdir(parents=True)
    for name in names:
        (trees / name).write_bytes(b"")
    executable = root / "mimic"
    executable.write_text(script)
    executable.chmod(0o755)
    return root


def load_template(path: Path) -> dict:
    """A run-file template read from ``path`` (``parse_template`` of its bytes)."""
    return parse_template(Path(path).read_bytes(), Path(path))


def load_constraint_table(path: Path) -> ConstraintTable:
    """A constraint table read from ``path`` (``parse_constraint_table`` of its bytes)."""
    return parse_constraint_table(Path(path).read_bytes(), Path(path))


def renders_figures(test):
    """Mark a test method as one that draws the ``analyse`` stage's figures for real."""
    test.renders_figures = True
    return test


class BenchTestCase(unittest.TestCase):
    """A TestCase that skips the ``analyse`` stage's figures unless the test asks for them.

    Drawing the figures starts a subprocess that builds matplotlib's font cache in a fresh
    configuration directory, which takes seconds a run; the stage's tests that are not about the
    figures would pay it each time. A method decorated with :func:`renders_figures` gets the real
    rendering. Fixtures built in ``setUpClass`` or ``setUpModule`` run before this patch and so
    render for real, which is where their tests expect it.
    """

    def setUp(self) -> None:
        """Replace ``mimic_bench.figures.render_figures`` with a no-op unless the test is marked."""
        super().setUp()
        method = getattr(self, self._testMethodName)
        if not getattr(method, "renders_figures", False):
            import mimic_bench.figures as figures

            patcher = mock.patch.object(figures, "render_figures", lambda tables, directory: [])
            patcher.start()
            self.addCleanup(patcher.stop)


def _new_directory() -> tuple[tempfile.TemporaryDirectory, Path]:
    """A new temporary directory and its resolved path."""
    directory = tempfile.TemporaryDirectory(prefix="mimic-bench-test-")
    return directory, Path(directory.name).resolve()


def tmp_path(owner: unittest.TestCase) -> Path:
    """A temporary directory removed when the test ``owner`` finishes (a ``tmp_path`` fixture)."""
    directory, path = _new_directory()
    owner.addCleanup(directory.cleanup)
    return path


def class_tmp_path(owner: type[unittest.TestCase]) -> Path:
    """A temporary directory removed when the test class ``owner`` finishes (in setUpClass)."""
    directory, path = _new_directory()
    owner.addClassCleanup(directory.cleanup)
    return path


def module_tmp_path() -> Path:
    """A temporary directory removed when the test module finishes (in setUpModule)."""
    directory, path = _new_directory()
    unittest.addModuleCleanup(directory.cleanup)
    return path


def assert_approx(actual, expected, *, rel_tol=None, abs_tol=None) -> None:
    """Assert ``actual`` equals ``expected`` within a relative or an absolute tolerance.

    Element by element (scalars or arrays; a scalar ``expected`` applies to every element), each
    difference must not exceed the larger of ``rel_tol * |expected|`` and ``abs_tol``. The
    defaults are ``rel_tol`` 1e-6 and ``abs_tol`` 1e-12; giving only
    ``abs_tol`` switches the relative tolerance off.

    Raises:
        AssertionError: If any element differs by more than the tolerance (the message shows the
            first offender).
    """
    actual_array = np.asarray(actual, dtype=float)
    expected_array = np.asarray(expected, dtype=float)
    if rel_tol is None and abs_tol is not None:
        tolerance = np.full(expected_array.shape, float(abs_tol))
    else:
        relative = 1e-6 if rel_tol is None else rel_tol
        absolute = 1e-12 if abs_tol is None else abs_tol
        tolerance = np.maximum(relative * np.abs(expected_array), absolute)
    difference = np.abs(actual_array - expected_array)
    bad = ~(difference <= tolerance)
    if np.any(bad):
        first = np.argwhere(bad)[0]
        index = tuple(first) if actual_array.ndim == expected_array.ndim else ()
        raise AssertionError(
            f"{actual_array[index] if actual_array.ndim else actual_array} != "
            f"{expected_array[index] if expected_array.ndim else expected_array} "
            f"(difference {difference[index] if difference.ndim else difference}, "
            f"tolerance {tolerance[index] if tolerance.ndim else tolerance}; "
            f"{int(bad.sum())} of {bad.size} elements differ)"
        )
