#!/usr/bin/env python3
"""Unit tests for the plotting engine's snapshot and evolution stages against a stub registry.

Covers: the evolution stage returns at once, without reading any snapshot or exiting, when no
registered evolution figure is selected (an empty EVOLUTION_PLOTS, or --plots naming only other
figures), still reads snapshots when an evolution figure is selected, and the snapshot stage
hands each figure the snapshot's redshift in metadata. Needs no Mimic output.
"""

import contextlib
import importlib.util
import io
import sys
from pathlib import Path
from types import SimpleNamespace

import numpy as np

HERE = Path(__file__).resolve().parent
MIMIC_PLOT_DIR = HERE.parent
REPO_ROOT = HERE.parent.parent.parent
sys.path.insert(0, str(MIMIC_PLOT_DIR))
sys.path.insert(0, str(REPO_ROOT / "tests"))

from framework import run_test_suite


class SnapshotsRead(Exception):
    """Raised by the stub mapper: the stage went on to read snapshots."""


class StubParams:
    """The slice of the engine's parameter object the stages use."""

    def __init__(self, values):
        self.params = dict(values)

    def __getitem__(self, key):
        return self.params[key]

    def get(self, key, default=None):
        return self.params.get(key, default)


class StubMapper:
    """Snapshot/redshift mapper for one snapshot (49) at z = 0.25."""

    def __init__(self, *_args):
        self.snapshots = [49]

    def get_redshift(self, snapshot):
        assert snapshot == 49
        return 0.25

    def get_redshift_str(self, snapshot):
        return "_z0.250"


class RaisingMapper:
    """Mapper whose construction fails the test: the stage reached its snapshot reads."""

    def __init__(self, *_args):
        raise SnapshotsRead()


def load_engine():
    """Import mimic-plot.py afresh, whose filename is not importable by name."""
    spec = importlib.util.spec_from_file_location(
        "mimic_plot_stages", MIMIC_PLOT_DIR / "mimic-plot.py"
    )
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def make_args(**overrides):
    values = dict(
        quiet=False,
        verbose=False,
        all_snapshots=False,
        snapshot=None,
        first_file=None,
        last_file=None,
        param_file="stub.yaml",
        evolution_plots=True,
        format=".png",
    )
    values.update(overrides)
    return SimpleNamespace(**values)


def run_evolution_stage(engine, selected_plots=None, **args):
    out = io.StringIO()
    with contextlib.redirect_stdout(out):
        result = engine.generate_evolution_plots(
            StubParams({"OutputDir": "unused"}), make_args(**args), "unused", selected_plots
        )
    return result, out.getvalue()


def test_empty_evolution_registry_returns_without_reading_or_exiting():
    """A registry with no evolution figures (a one-epoch run) is not an error."""
    engine = load_engine()
    engine.EVOLUTION_PLOTS = []
    engine.SnapshotRedshiftMapper = RaisingMapper
    result, text = run_evolution_stage(engine)
    assert result == ([], {}), f"Expected empty results, got {result}"
    assert "No evolution plots" in text, f"Expected a one-line notice, got {text!r}"


def test_quiet_run_prints_no_notice():
    """--quiet suppresses the skip notice."""
    engine = load_engine()
    engine.EVOLUTION_PLOTS = []
    engine.SnapshotRedshiftMapper = RaisingMapper
    result, text = run_evolution_stage(engine, quiet=True)
    assert result == ([], {})
    assert text == "", f"Quiet output should be empty, got {text!r}"


def test_selection_naming_only_snapshot_figures_skips_the_evolution_stage():
    """--plots=<snapshot figure> must not read every snapshot to draw nothing."""
    engine = load_engine()
    engine.EVOLUTION_PLOTS = ["stub_evolution"]
    engine.PROFILE_PLOTS = {"snapshot": None, "evolution": None}
    engine.importlib = SimpleNamespace(
        import_module=lambda _name: SimpleNamespace(plot=lambda **_kwargs: (None, "stub"))
    )
    engine.SnapshotRedshiftMapper = RaisingMapper
    result, _text = run_evolution_stage(engine, selected_plots=["halo_mass_function"])
    assert result == ([], {})


def test_selected_evolution_figure_still_reads_snapshots():
    """Positive control: a registered, selected evolution figure reaches the snapshot reads."""
    engine = load_engine()
    engine.EVOLUTION_PLOTS = ["stub_evolution"]
    engine.PROFILE_PLOTS = {"snapshot": None, "evolution": None}
    engine.importlib = SimpleNamespace(
        import_module=lambda _name: SimpleNamespace(plot=lambda **_kwargs: (None, "stub"))
    )
    engine.SnapshotRedshiftMapper = RaisingMapper
    for selected in (None, ["stub_evolution"]):
        try:
            run_evolution_stage(engine, selected_plots=selected)
        except SnapshotsRead:
            continue
        raise AssertionError(f"selected_plots={selected}: the stage did not reach the reads")


def test_snapshot_stage_passes_the_redshift_to_figures():
    """Snapshot figures receive metadata['redshift'] mapped from the snapshot, as evolution does."""
    engine = load_engine()
    seen = {}

    def stub_plot(galaxies, volume, metadata, **_kwargs):
        seen.update(metadata)
        return "stub.png", None

    engine.SNAPSHOT_PLOTS = ["stub_snapshot"]
    engine.PLOT_REQUIREMENTS = {}
    engine.SnapshotRedshiftMapper = StubMapper
    engine.get_available_plot_modules = lambda *_args: {"stub_snapshot": stub_plot}
    engine.read_data = lambda **_kwargs: (np.zeros(1), 1.0e6, {"hubble_h": 0.7})
    params = StubParams(
        {
            "OutputDir": "unused",
            "OutputFileBaseName": "stub",
            "LastSnapshotNr": 49,
            "FirstFile": 0,
            "LastFile": 0,
        }
    )
    with contextlib.redirect_stdout(io.StringIO()):
        created, skipped, available = engine.generate_snapshot_plots(
            params, make_args(), "unused", None
        )
    assert available and created == ["stub.png"] and skipped == {}
    assert seen.get("redshift") == 0.25, f"metadata redshift was {seen.get('redshift')}"
    assert seen.get("hubble_h") == 0.7, "The reader's own metadata must be kept"


def main():
    """Run this file's tests via the shared framework runner."""
    return run_test_suite(
        [
            test_empty_evolution_registry_returns_without_reading_or_exiting,
            test_quiet_run_prints_no_notice,
            test_selection_naming_only_snapshot_figures_skips_the_evolution_stage,
            test_selected_evolution_figure_still_reads_snapshots,
            test_snapshot_stage_passes_the_redshift_to_figures,
        ],
        "Engine snapshot and evolution stages (test_engine_stages.py)",
    )


if __name__ == "__main__":
    sys.exit(main())
