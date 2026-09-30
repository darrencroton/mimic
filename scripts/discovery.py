#!/usr/bin/env python3
"""Repository discovery helpers for the model/simulation package layout."""

from __future__ import annotations

import os
from pathlib import Path
from typing import Iterable, List

REPO_ROOT = Path(__file__).resolve().parent.parent


def makefile_default(variable: str, fallback: str) -> str:
    """Read a simple DEFAULT_* assignment from the repository Makefile.

    Single source of truth for the default MODEL/SIMULATION selection used by
    generator scripts and the test harness when the environment does not
    select a package explicitly.
    """
    makefile = REPO_ROOT / "Makefile"
    try:
        with makefile.open(encoding="utf-8") as handle:
            for raw_line in handle:
                line = raw_line.split("#", 1)[0].strip()
                prefix = f"{variable} :="
                if line.startswith(prefix):
                    value = line[len(prefix) :].strip()
                    return value or fallback
    except OSError:
        pass
    return fallback


DEFAULT_MODEL = makefile_default("DEFAULT_MODEL", "sage16")
# Simulation selected when neither SIMULATION nor SIM is set in the environment.
# Read from DEFAULT_SIMULATION in the Makefile. The Makefile explicitly prefixes
# MODEL and SIMULATION when invoking these helpers, so this default only applies
# to standalone script runs outside of make.
DEFAULT_SIMULATION = makefile_default("DEFAULT_SIMULATION", "mini-millennium")

# Packages whose selected-model tests are registered with the core and simulation tiers. A
# package belongs here only when every model's registered tests can run on its generated
# inputs. No horizontal package does yet: the sage16 module integration tests read binary
# output (model_z0.000_0), which a horizontal run cannot write (measured 2026-09-30 on
# mini-millennium-horizontal: 26 FAIL across 17 files; halos-only's own test passes).
FULL_MODEL_TEST_SIMULATIONS = frozenset(
    {
        "mini-millennium",
        "micro-uchuu",
        "micro-uchuu-hdf5",
        "micro-uchuu-ascii",
    }
)

# Packages whose generated test inputs run the production simulation_info.yaml instead of a
# committed _tests/input/test_simulation.yaml, so one small real catalogue exercises each shipped
# vertical reader path. A horizontal package is never a member: its generic tier runs only on
# committed fixture data.
PRODUCTION_TEST_CONFIG_SIMULATIONS = frozenset(
    {
        "micro-uchuu",
        "micro-uchuu-hdf5",
        "micro-uchuu-ascii",
    }
)


def rel(path: Path) -> str:
    """Return a repository-relative path string."""
    return str(path.relative_to(REPO_ROOT))


def selected_simulation() -> str:
    """Return the simulation package selected by environment or Makefile default."""
    return os.environ.get("SIMULATION") or os.environ.get("SIM") or DEFAULT_SIMULATION


def full_model_tests_enabled(simulation: str | None = None) -> bool:
    """Whether the selected simulation should run the full selected-model test suite."""
    return (simulation or selected_simulation()) in FULL_MODEL_TEST_SIMULATIONS


def production_test_config_enabled(simulation: str | None = None) -> bool:
    """Whether generated tests should use the package's production simulation_info.yaml."""
    return (simulation or selected_simulation()) in PRODUCTION_TEST_CONFIG_SIMULATIONS


#: Where a simulation package keeps its committed, test-sized simulation config.
PACKAGE_TEST_CONFIG = Path("_tests") / "input" / "test_simulation.yaml"


def simulation_root(simulation: str | None = None) -> Path:
    """Return ``simulations/<simulation>/`` for the given or selected package."""
    return REPO_ROOT / "simulations" / (simulation or selected_simulation())


def package_test_config(simulation: str | None = None) -> Path | None:
    """Return the package's committed ``_tests/input/test_simulation.yaml``, or None."""
    path = simulation_root(simulation) / PACKAGE_TEST_CONFIG
    return path if path.is_file() else None


def _declared_input(config_path: Path, key: str):
    """Return ``input.<key>`` from a simulation config, or None when it is not declared."""
    import yaml

    with config_path.open(encoding="utf-8") as handle:
        config = yaml.safe_load(handle) or {}
    return (config.get("input") or {}).get(key)


def package_processing_order(simulation: str | None = None) -> str:
    """Return the processing order a simulation package declares (``vertical`` when unset).

    The package's metadata is the source of this fact, never its name: the production
    ``simulation_info.yaml`` and, when present, the committed test config are both read,
    and a package whose two configs disagree fails here rather than generating run files
    for the wrong driver. The runtime default for an unset key is ``vertical``.
    """
    simulation = simulation or selected_simulation()
    declared = {}
    production = simulation_root(simulation) / "simulation_info.yaml"
    for path in (production, package_test_config(simulation)):
        if path is not None and path.is_file():
            # An omitted key means ``vertical`` at run time, so it is compared as such: a
            # horizontal production config beside a test config that omits the key is a
            # real conflict (the run would load the test config as vertical), not agreement.
            value = _declared_input(path, "processing_order")
            declared[rel(path)] = "vertical" if value is None else str(value)
    orders = set(declared.values())
    if len(orders) > 1:
        raise ValueError(
            f"simulation package {simulation} declares conflicting input.processing_order "
            f"values: {declared}"
        )
    return orders.pop() if orders else "vertical"


def package_is_horizontal(simulation: str | None = None) -> bool:
    """Whether a simulation package declares ``input.processing_order: horizontal``."""
    return package_processing_order(simulation) == "horizontal"


def generic_tier_skip_reason(simulation: str | None = None) -> str | None:
    """Why the generic run-file-driven tests cannot run for a package, or None if they can.

    A horizontal package runs them only on committed fixture data, declared by its
    ``_tests/input/test_simulation.yaml``. Without one, the only dataset its configuration
    names is the machine-local production conversion, which a test must never open, so
    every test that needs a generated run file skips with this reason instead.
    """
    simulation = simulation or selected_simulation()
    if not package_is_horizontal(simulation) or package_test_config(simulation) is not None:
        return None
    return (
        f"horizontal package {simulation} ships no committed test fixture "
        f"(simulations/{simulation}/{PACKAGE_TEST_CONFIG.as_posix()}), so the generic tier "
        f"does not run for it; run its parity gate on a machine holding the dataset"
    )


def existing(paths: Iterable[Path]) -> List[Path]:
    """Return existing paths in declaration order without duplicates."""
    seen = set()
    result: List[Path] = []
    for path in paths:
        resolved = path.resolve()
        if resolved in seen or not path.exists():
            continue
        seen.add(resolved)
        result.append(path)
    return result


def live_model_roots() -> List[Path]:
    """Return the selected model package root under models/.

    Mimic is built against one model set at a time. Select it with
    ``make MODEL=<name>`` or by setting the ``MODEL`` environment variable when
    invoking helper scripts directly.
    """
    selected_model = os.environ.get("MODEL", DEFAULT_MODEL)
    if not selected_model:
        return []
    models_dir = REPO_ROOT / "models"
    if not models_dir.exists():
        return []
    path = models_dir / selected_model
    if not path.is_dir() or path.name.startswith("_") or path.name == "archive":
        return []
    return [path]


def live_simulation_roots() -> List[Path]:
    """Return the selected simulation package root under simulations/.

    Mimic is built against one simulation/catalog property package at a time.
    Select it with ``make SIMULATION=<name>`` (or the ``SIM=<name>`` shorthand),
    or by setting the ``SIMULATION``/``SIM`` environment variable when invoking
    helper scripts directly. Defaults to :data:`DEFAULT_SIMULATION`.
    """
    selected = selected_simulation()
    if not selected:
        return []
    simulations_dir = REPO_ROOT / "simulations"
    if not simulations_dir.exists():
        return []
    path = simulations_dir / selected
    if not path.is_dir() or path.name.startswith("_") or path.name == "archive":
        return []
    return [path]


def test_build_enabled() -> bool:
    """Whether this is a test build that includes framework test fixtures.

    Set by the Makefile for ``TEST_BUILD=yes`` (exported as MIMIC_TEST_BUILD)
    and unconditionally by tests/unit/run_tests.sh. Production builds leave it
    unset so the executable carries neither the test fixture modules nor their
    test-only properties.
    """
    return bool(os.environ.get("MIMIC_TEST_BUILD"))


def test_property_files() -> List[Path]:
    """Test-only galaxy property metadata, included only in test builds.

    These properties (e.g. TestDummyProperty) are owned by the framework test
    fixture modules under src/module_system/test_*, not by any production model
    package, so they must never appear in models/<model>/model_properties.yaml.
    """
    if not test_build_enabled():
        return []
    return existing([module_system_dir() / "test_fixture" / "test_properties.yaml"])


def core_property_files() -> List[Path]:
    """Core property metadata."""
    return existing([REPO_ROOT / "src" / "core" / "core_properties.yaml"])


def model_property_files() -> List[Path]:
    """Galaxy/model property metadata from the selected model package."""
    model_files = [root / "model_properties.yaml" for root in live_model_roots()]
    return existing(model_files)


def parameter_unit_files() -> List[Path]:
    """Optional model-global dimensional parameter metadata."""
    return existing([root / "parameter_units.yaml" for root in live_model_roots()])


def simulation_halo_property_files() -> List[Path]:
    """Simulation/catalog halo property metadata from all live simulations."""
    return existing([root / "halo_properties.yaml" for root in live_simulation_roots()])


def halo_property_files() -> List[Path]:
    """All halo property metadata roots in generation order."""
    return core_property_files() + simulation_halo_property_files()


def module_system_dir() -> Path:
    """Framework-owned module-system directory."""
    return REPO_ROOT / "src" / "module_system"


def generated_module_dir() -> Path:
    return module_system_dir() / "generated"


def module_roots() -> List[Path]:
    """Module discovery roots for the selected production model package."""
    roots: List[Path] = []
    roots.extend(root / "modules" for root in live_model_roots())
    return existing(roots)


def standalone_module_files() -> List[Path]:
    """Package-local standalone module source files.

    Standalone modules are supported only inside model package module roots,
    for example ``models/<model>/modules/my_module.c``. The old ``src/modules``
    root is intentionally not searched.
    """
    files: List[Path] = []
    for root in module_roots():
        for source_file in sorted(root.glob("*.c")):
            if source_file.name.startswith("test_"):
                continue
            files.append(source_file)
    return existing(files)


def framework_test_module_roots() -> List[Path]:
    """Framework test modules registered as runtime modules for test builds.

    Empty for production builds so the production executable does not carry the
    test fixture/event modules. Gated on :func:`test_build_enabled`.
    """
    if not test_build_enabled():
        return []
    system = module_system_dir()
    return existing(
        [
            path
            for path in sorted(system.glob("test_*"))
            if path.is_dir() and (path / "module_info.yaml").exists()
        ]
    )


def module_metadata_files() -> List[Path]:
    """All module_info.yaml files from live discovery roots."""
    files: List[Path] = []
    seen = set()

    utility_candidates = [root / "shared" / "module_info.yaml" for root in live_model_roots()]
    for yaml_file in existing(utility_candidates):
        resolved = yaml_file.resolve()
        if resolved not in seen:
            seen.add(resolved)
            files.append(yaml_file)

    for root in module_roots():
        if not root.exists():
            continue
        for yaml_file in sorted(root.glob("*/module_info.yaml")):
            parts = yaml_file.relative_to(REPO_ROOT).parts
            if any(part in {"_archive", "archive", "generated", "template"} for part in parts):
                continue
            if "_system" in parts:
                continue
            resolved = yaml_file.resolve()
            if resolved not in seen:
                seen.add(resolved)
                files.append(yaml_file)

    for test_root in framework_test_module_roots():
        yaml_file = test_root / "module_info.yaml"
        resolved = yaml_file.resolve()
        if resolved not in seen:
            seen.add(resolved)
            files.append(yaml_file)

    return files
