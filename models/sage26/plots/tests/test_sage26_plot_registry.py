#!/usr/bin/env python3
"""
sage26 plot registry contract (models/sage26/plots/figures/__init__.py and profiles/).

Validates: SNAPSHOT_PLOTS and EVOLUTION_PLOTS equal the registry table in order, with no
           duplicate and no name in both; PLOT_FUNCS and PLOT_REQUIREMENTS hold exactly the
           registry's names; every evolution figure's EVOLUTION_PLOT_FIELDS entry includes its
           PLOT_REQUIREMENTS; every field in either table is an output property declared by
           sage26's model_properties.yaml, core_properties.yaml or the mini-Millennium
           halo_properties.yaml (a halo property also by the horizontal package's file);
           profiles/default.yaml lists the registry in order; and the profile stack the engine
           builds for the vertical and the horizontal run file agrees on `plots` and `axes`
Tolerance: none; every comparison is exact (lists, sets and dictionaries)

The registry table below is written from the plan of the c16 plotting node, Slice 2 (rows S1-S30
and E1-E4), not computed from the package. It needs no Mimic run.

Run: mimic_venv/bin/python models/sage26/plots/tests/test_sage26_plot_registry.py
"""

import importlib
import importlib.util
import os
import sys
from pathlib import Path

# Matplotlib is imported when the figure package loads; keep it headless
os.environ["MPLBACKEND"] = "Agg"

import yaml

HERE = Path(__file__).resolve().parent
REPO_ROOT = HERE.parents[3]
MIMIC_PLOT_DIR = REPO_ROOT / "plot" / "mimic-plot"
sys.path.insert(0, str(MIMIC_PLOT_DIR))
sys.path.insert(0, str(REPO_ROOT / "tests"))
sys.path.insert(0, str(HERE))

from framework import run_test_suite
from sage26_plot_fixtures import (
    HALO_PROPERTIES,
    HORIZONTAL_HALO_PROPERTIES,
    output_property_declarations,
)

MODEL_DIR = REPO_ROOT / "models" / "sage26"
PROFILE = MODEL_DIR / "plots" / "profiles" / "default.yaml"
RUN_FILES = {
    "vertical": MODEL_DIR / "input" / "sage26_mini-millennium.yaml",
    "horizontal": MODEL_DIR / "input" / "sage26_mini-millennium-horizontal.yaml",
}

# Registry table of the c16 plan, Slice 2: row order is registry order
SNAPSHOT_TABLE = [
    "halo_mass_function",  # S1
    "halo_occupation",  # S2
    "spin_distribution",  # S3
    "velocity_distribution",  # S4
    "spatial_distribution",  # S5
    "stellar_mass_function",  # S6
    "cold_gas_function",  # S7
    "baryon_fraction",  # S8
    "baryonic_mass_function",  # S9
    "gas_mass_function",  # S10
    "baryonic_tully_fisher",  # S11
    "specific_sfr",  # S12
    "black_hole_bulge_relation",  # S13
    "gas_fraction",  # S14
    "metallicity",  # S15
    "bulge_mass_fraction",  # S16
    "quiescent_fraction",  # S17
    "mass_reservoir_scatter",  # S18
    "bulge_to_total",  # S19
    "spatial_distribution_3d_box",  # S20
    "star_formation_rate",  # S21
    "mass_loading_vs_vvir",  # S22
    "stellar_vs_halo_mass_by_regime",  # S23
    "specific_star_formation_rate",  # S24
    "concentration_vs_mvir",  # S25
    "sfr_vs_halo_accretion_rate",  # S26
    "tcool_vs_mvir",  # S27
    "cgm_fraction_vs_mvir",  # S28
    "ejection_efficiency_vs_vvir",  # S29
    "disk_radius_vs_stellar_mass",  # S30
]
EVOLUTION_TABLE = [
    "hmf_evolution",  # E1
    "smf_evolution",  # E2
    "sfr_density_evolution",  # E3
    "stellar_mass_density_evolution",  # E4
]


def load_figure_package(model):
    """Import models/<model>/plots/figures as `figures`, replacing any earlier package."""
    for name in [m for m in sys.modules if m == "figures" or m.startswith("figures.")]:
        del sys.modules[name]
    plots_dir = str(REPO_ROOT / "models" / model / "plots")
    sys.path.insert(0, plots_dir)
    try:
        return importlib.import_module("figures")
    finally:
        sys.path.remove(plots_dir)


FIGURES = load_figure_package("sage26")


def load_engine():
    """Import plot/mimic-plot/mimic-plot.py, whose filename is not importable by name."""
    spec = importlib.util.spec_from_file_location(
        "mimic_plot_registry_test", MIMIC_PLOT_DIR / "mimic-plot.py"
    )
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def profile_stack(engine, run_file):
    """Return the merged plot profile the engine builds for a run file.

    The layers and their order follow configure_plot_profile() (mimic-plot.py:308-371): the
    global profile, the model's default profile, the simulation's plot_profile.yaml, then the
    model's <simulation>_plot_profile.yaml, each included only if it exists. The sage26 run
    files set neither `plotting.profile` nor overrides.
    """
    run = yaml.safe_load(run_file.read_text())
    assert "plotting" not in run, f"{run_file.name} sets plotting keys this stack does not model"
    simulation = run["simulation"]["name"]
    layers = [
        MIMIC_PLOT_DIR / "profiles" / "default.yaml",
        PROFILE,
        REPO_ROOT / "simulations" / simulation / "plot_profile.yaml",
        MODEL_DIR / "plots" / "profiles" / f"{simulation}_plot_profile.yaml",
    ]
    merged = {}
    for layer in layers:
        if layer.exists():
            merged = engine.merge_profile(merged, engine.read_profile_file(layer))
    return merged


def test_registry_lists_equal_the_plan_table_in_order():
    """SNAPSHOT_PLOTS and EVOLUTION_PLOTS are exactly the plan's rows S1-S30 and E1-E4."""
    assert (
        FIGURES.SNAPSHOT_PLOTS == SNAPSHOT_TABLE
    ), f"SNAPSHOT_PLOTS differs from the table: {FIGURES.SNAPSHOT_PLOTS}"
    assert (
        FIGURES.EVOLUTION_PLOTS == EVOLUTION_TABLE
    ), f"EVOLUTION_PLOTS differs from the table: {FIGURES.EVOLUTION_PLOTS}"


def test_registry_lists_have_no_duplicates_and_no_name_in_both():
    """A name appears once, and a figure is either a snapshot or an evolution figure."""
    snapshot, evolution = FIGURES.SNAPSHOT_PLOTS, FIGURES.EVOLUTION_PLOTS
    assert len(snapshot) == len(set(snapshot)), "duplicate name in SNAPSHOT_PLOTS"
    assert len(evolution) == len(set(evolution)), "duplicate name in EVOLUTION_PLOTS"
    both = set(snapshot) & set(evolution)
    assert not both, f"in both lists: {sorted(both)}"


def test_plot_funcs_and_requirements_have_exactly_the_registry_names():
    """PLOT_FUNCS and PLOT_REQUIREMENTS hold one entry per registered figure and no other."""
    registry = set(SNAPSHOT_TABLE) | set(EVOLUTION_TABLE)
    assert set(FIGURES.PLOT_FUNCS) == registry, (
        f"PLOT_FUNCS: extra {sorted(set(FIGURES.PLOT_FUNCS) - registry)}, "
        f"missing {sorted(registry - set(FIGURES.PLOT_FUNCS))}"
    )
    assert set(FIGURES.PLOT_REQUIREMENTS) == registry, (
        f"PLOT_REQUIREMENTS: extra {sorted(set(FIGURES.PLOT_REQUIREMENTS) - registry)}, "
        f"missing {sorted(registry - set(FIGURES.PLOT_REQUIREMENTS))}"
    )
    assert all(callable(func) for func in FIGURES.PLOT_FUNCS.values()), "a PLOT_FUNCS entry"


def test_every_evolution_figure_has_fields_that_include_its_requirements():
    """EVOLUTION_PLOT_FIELDS has an entry per evolution figure, a superset of its requirements."""
    assert set(FIGURES.EVOLUTION_PLOT_FIELDS) == set(
        EVOLUTION_TABLE
    ), f"EVOLUTION_PLOT_FIELDS names: {sorted(FIGURES.EVOLUTION_PLOT_FIELDS)}"
    for name in EVOLUTION_TABLE:
        missing = set(FIGURES.PLOT_REQUIREMENTS[name]) - set(FIGURES.EVOLUTION_PLOT_FIELDS[name])
        assert not missing, f"{name}: EVOLUTION_PLOT_FIELDS lacks required {sorted(missing)}"


def test_every_registry_field_is_a_declared_output_property():
    """Each field a requirement names is declared with `output: true` in a property file.

    A halo property (declared by the mini-Millennium halo catalogue) must also be an output
    property of the horizontal package, which sage26's horizontal run reads
    (docs/DEVELOPER-GUIDE.md:482, "register only figures whose required properties the model
    declares").
    """
    declared = set(output_property_declarations())
    halo = set(output_property_declarations([(HALO_PROPERTIES, "halo_properties")]))
    horizontal = set(
        output_property_declarations([(HORIZONTAL_HALO_PROPERTIES, "halo_properties")])
    )
    named = set()
    for fields in FIGURES.PLOT_REQUIREMENTS.values():
        named |= set(fields)
    for fields in FIGURES.EVOLUTION_PLOT_FIELDS.values():
        named |= set(fields)
    undeclared = named - declared
    assert not undeclared, f"not declared output properties: {sorted(undeclared)}"
    not_horizontal = (named & halo) - horizontal
    assert not not_horizontal, f"halo fields missing in horizontal: {sorted(not_horizontal)}"


def test_default_profile_lists_equal_the_registry_in_order():
    """profiles/default.yaml runs exactly the registered figures, in registry order."""
    plots = yaml.safe_load(PROFILE.read_text())["plots"]
    assert plots["snapshot"] == SNAPSHOT_TABLE, f"plots.snapshot: {plots['snapshot']}"
    assert plots["evolution"] == EVOLUTION_TABLE, f"plots.evolution: {plots['evolution']}"


def test_vertical_and_horizontal_profile_stacks_agree_on_plots_and_axes():
    """The stack for each run file lists the registry and carries the same axes."""
    engine = load_engine()
    stacks = {driver: profile_stack(engine, path) for driver, path in RUN_FILES.items()}
    for driver, stack in stacks.items():
        assert stack["plots"]["snapshot"] == SNAPSHOT_TABLE, f"{driver}: plots.snapshot"
        assert stack["plots"]["evolution"] == EVOLUTION_TABLE, f"{driver}: plots.evolution"
    assert stacks["vertical"]["plots"] == stacks["horizontal"]["plots"], "plots differ"
    assert stacks["vertical"].get("axes") == stacks["horizontal"].get("axes"), "axes differ"
    assert stacks["vertical"].get("axes"), "the stack carries no axes at all"


def main():
    """Run this file's tests via the shared framework runner."""
    return run_test_suite(
        [
            test_registry_lists_equal_the_plan_table_in_order,
            test_registry_lists_have_no_duplicates_and_no_name_in_both,
            test_plot_funcs_and_requirements_have_exactly_the_registry_names,
            test_every_evolution_figure_has_fields_that_include_its_requirements,
            test_every_registry_field_is_a_declared_output_property,
            test_default_profile_lists_equal_the_registry_in_order,
            test_vertical_and_horizontal_profile_stacks_agree_on_plots_and_axes,
        ],
        "sage26 plot registry (test_sage26_plot_registry.py)",
    )


if __name__ == "__main__":
    sys.exit(main())
