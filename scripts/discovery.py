#!/usr/bin/env python3
"""Repository discovery helpers for the model/simulation package layout."""

from __future__ import annotations

import os
from pathlib import Path
from typing import Iterable, List


REPO_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_MODEL = ""


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
    """Return live simulation package roots under simulations/."""
    simulations_dir = REPO_ROOT / "simulations"
    if not simulations_dir.exists():
        return []
    return [
        path
        for path in sorted(simulations_dir.iterdir())
        if path.is_dir() and not path.name.startswith("_") and path.name != "archive"
    ]


def core_property_files() -> List[Path]:
    """Core property metadata."""
    return existing([REPO_ROOT / "src" / "core" / "core_properties.yaml"])


def model_property_files() -> List[Path]:
    """Galaxy/model property metadata from the selected model package."""
    model_files = [root / "model_properties.yaml" for root in live_model_roots()]
    return existing(model_files)


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
    """Framework test modules that are registered as runtime modules for tests."""
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
