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
# Read from DEFAULT_SIMULATION in the Makefile. The Makefile always exports
# SIMULATION when it invokes these helpers, so this default only applies to
# standalone script runs.
DEFAULT_SIMULATION = makefile_default("DEFAULT_SIMULATION", "mini-millennium")


def rel(path: Path) -> str:
    """Return a repository-relative path string."""
    return str(path.relative_to(REPO_ROOT))


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
    selected = os.environ.get("SIMULATION") or os.environ.get("SIM") or DEFAULT_SIMULATION
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
