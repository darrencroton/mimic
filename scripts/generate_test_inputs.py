#!/usr/bin/env python3
"""Generate concrete Mimic run files for tests.

The test suite owns small fixture *recipes* by scope, but Mimic runtime
validation requires concrete model and simulation package names in every run
file. This script materializes those run files for the selected MODEL and
SIMULATION under build/generated/test_inputs/.

Usage:
    MODEL=sage16 SIMULATION=mini-millennium python3 scripts/generate_test_inputs.py

Generates, for a vertical package:
    build/generated/test_inputs/<MODEL>/<SIMULATION>/core/test_binary.yaml
    build/generated/test_inputs/<MODEL>/<SIMULATION>/core/test_hdf5.yaml
    build/generated/test_inputs/<MODEL>/<SIMULATION>/simulations/<SIMULATION>/test_binary.yaml
    build/generated/test_inputs/<MODEL>/<SIMULATION>/simulations/<SIMULATION>/test_hdf5.yaml
    build/generated/test_inputs/<MODEL>/<SIMULATION>/simulations/<SIMULATION>/test_uniquegalid.yaml
    build/generated/test_inputs/<MODEL>/<SIMULATION>/manifest.json

The run files are shaped by the package's own declared processing order
(discovery.package_processing_order), never by its name. A horizontal package
is HDF5-only, has no input file range and one reader, so it gets the two HDF5
run files, each with no input overrides and requesting the last snapshot of its
committed fixture's own snapshot list, plus core/test_binary.yaml, which only
the C unit tier reads: that tier parses it after coercing the reader to a
vertical one, and cannot parse an HDF5 run file (see test_cosmology_param_file()
in tests/framework/core_test_fixtures.h), so it is kept binary and is never run.
A horizontal package without a committed fixture gets that parse fixture only,
and its manifest records why the run-file-driven tests skip
(discovery.generic_tier_skip_reason); no generated file then names a runnable
production dataset.
"""

from __future__ import annotations

import json
import os
from pathlib import Path
from typing import Any

import yaml
from discovery import (
    REPO_ROOT,
    generic_tier_skip_reason,
    makefile_default,
    package_processing_order,
    package_test_config,
    production_test_config_enabled,
    rel,
)

OUTPUT_ROOT = REPO_ROOT / "build" / "generated" / "test_inputs"


def selected_model() -> str:
    return os.environ.get("MODEL") or makefile_default("DEFAULT_MODEL", "sage16")


def selected_simulation() -> str:
    return (
        os.environ.get("SIMULATION")
        or os.environ.get("SIM")
        or makefile_default("DEFAULT_SIMULATION", "mini-millennium")
    )


def require_file(path: Path, label: str) -> Path:
    if not path.is_file():
        raise FileNotFoundError(f"{label} not found: {path}")
    return path


def require_dir(path: Path, label: str) -> Path:
    if not path.is_dir():
        raise FileNotFoundError(f"{label} not found: {path}")
    return path


def test_simulation_config(simulation_root: Path, simulation: str) -> str:
    """Return a test-sized simulation config path for fast test runs.

    Full-validation micro-Uchuu packages use their production simulation
    metadata so the model test suite exercises the shipped reader paths. Other
    simulation packages may provide a test-sized simulation metadata file at
    simulations/<simulation>/_tests/input/test_simulation.yaml. This keeps fast
    core/model tests independent of production catalog size while still compiling
    against the selected simulation package.

    mini-Millennium has a shared single-file mini-catalog under tests/data/.
    Simulations without package-local fixtures fall back to production
    simulation_info.yaml; generated run YAML then applies input overrides to
    cap the file range. Production-scale packages should provide a fixture here.
    A horizontal package without one falls back too, but only its C-unit-tier
    parse fixture is then written (see the module docstring).
    """
    production_config = simulation_root / "simulation_info.yaml"
    if production_test_config_enabled(simulation):
        require_file(production_config, "simulation config")
        return rel(production_config)

    committed_test_config = package_test_config(simulation)
    if committed_test_config is not None:
        return rel(committed_test_config)

    shared_mini_millennium_config = REPO_ROOT / "tests" / "data" / "test_simulation.yaml"
    if simulation == "mini-millennium" and shared_mini_millennium_config.is_file():
        return rel(shared_mini_millennium_config)

    require_file(production_config, "simulation config")
    return rel(production_config)


def base_run_config(model: str, simulation: str) -> dict[str, Any]:
    model_root = require_dir(REPO_ROOT / "models" / model, "model package")
    simulation_root = require_dir(REPO_ROOT / "simulations" / simulation, "simulation package")

    require_file(model_root / "model_properties.yaml", "model properties")
    require_file(simulation_root / "halo_properties.yaml", "simulation halo properties")
    sim_config = test_simulation_config(simulation_root, simulation)

    config: dict[str, Any] = {
        "model": {
            "name": model,
        },
        "simulation": {
            "name": simulation,
            "config": sim_config,
        },
        "output": {},
        "SubSteps": 1,
        "modules": {
            "parameters": {},
        },
    }

    return config


def write_yaml(path: Path, config: dict[str, Any], title: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8") as handle:
        handle.write("#" + "=" * 77 + "\n")
        handle.write(f"# {title}\n")
        handle.write("# Auto-generated by scripts/generate_test_inputs.py; do not edit.\n")
        handle.write("#" + "=" * 77 + "\n\n")
        yaml.safe_dump(config, handle, default_flow_style=False, sort_keys=False)


def write_run(
    path: Path,
    base: dict[str, Any],
    *,
    title: str,
    output_format: str,
    output_directory: str,
    output_filename: str,
    snapshot_list: list[int],
    input_overrides: dict[str, Any] | None = None,
    output_overrides: dict[str, Any] | None = None,
) -> Path:
    config = dict(base)
    if input_overrides:
        config["input"] = input_overrides
    config["output"] = {
        "output_filename": output_filename,
        "output_directory": output_directory,
        "output_format": output_format,
        "snapshot_list": snapshot_list,
    }
    if output_overrides:
        config["output"].update(output_overrides)
    write_yaml(path, config, title)
    return path


def generation_root(model: str, simulation: str) -> Path:
    return OUTPUT_ROOT / model / simulation


def last_snapshot_index(sim_config_rel: str) -> int:
    """Return the last snapshot index from the simulation's scale-factor list.

    Test inputs must request snapshots that exist for the selected simulation
    package, whatever its snapshot count -- nothing here assumes the
    64-snapshot Millennium convention.
    """
    config = yaml.safe_load((REPO_ROOT / sim_config_rel).read_text(encoding="utf-8"))
    a_list_rel = (config.get("input") or {}).get("snapshot_list_file")
    if not a_list_rel:
        raise ValueError(f"simulation config {sim_config_rel} lacks input.snapshot_list_file")
    a_list = REPO_ROOT / a_list_rel
    count = sum(
        1 for raw in a_list.read_text(encoding="utf-8").splitlines() if raw.split("#", 1)[0].strip()
    )
    if count < 2:
        raise ValueError(f"snapshot list {a_list} has fewer than 2 snapshots")
    return count - 1


def write_vertical_runs(output_root: Path, simulation: str, base: dict[str, Any]) -> list[Path]:
    """Write the run files of a vertical package and return their paths."""
    # All generated test runs cap the file range to a single file so tests stay
    # fast regardless of how many files the production catalogue has. For
    # full-validation micro-Uchuu packages, the simulation_info.yaml provides
    # the production paths and tree format; the single-file cap keeps run time
    # bounded. For other packages, this overrides the test fixture or production
    # range to the same single-file limit.
    test_input = {"first_file": 0, "last_file": 0}
    written: list[Path] = []

    # Snapshot indices derive from the selected simulation's a_list (last and
    # second-last snapshots) rather than hardcoding the mini-Millennium 63.
    last_snap = last_snapshot_index(base["simulation"]["config"])

    written.append(
        write_run(
            output_root / "core" / "test_binary.yaml",
            base,
            title="Mimic Core Test Run - Binary",
            output_format="binary",
            output_directory="./tests/data/output/binary/",
            output_filename="model",
            snapshot_list=[last_snap],
            input_overrides=test_input,
        )
    )
    written.append(
        write_run(
            output_root / "core" / "test_hdf5.yaml",
            base,
            title="Mimic Core Test Run - HDF5",
            output_format="hdf5",
            output_directory="./tests/data/output/hdf5/",
            output_filename="model",
            snapshot_list=[last_snap],
            input_overrides=test_input,
        )
    )

    simulation_dir = output_root / "simulations" / simulation
    written.append(
        write_run(
            simulation_dir / "test_binary.yaml",
            base,
            title=f"Mimic {simulation} Simulation Test Run - Binary",
            output_format="binary",
            output_directory="./tests/data/output/binary/",
            output_filename="model",
            snapshot_list=[last_snap],
            input_overrides=test_input,
        )
    )
    written.append(
        write_run(
            simulation_dir / "test_hdf5.yaml",
            base,
            title=f"Mimic {simulation} Simulation Test Run - HDF5",
            output_format="hdf5",
            output_directory="./tests/data/output/hdf5/",
            output_filename="model",
            snapshot_list=[last_snap],
            input_overrides=test_input,
        )
    )
    written.append(
        write_run(
            simulation_dir / "test_uniquegalid.yaml",
            base,
            title=f"Mimic {simulation} UniqueGalaxyID Test Run",
            output_format="binary",
            output_directory="./tests/data/output/binary/",
            output_filename="model_uniquegalid",
            snapshot_list=[last_snap - 1, last_snap],
            input_overrides=test_input,
        )
    )
    return written


def write_horizontal_runs(
    output_root: Path, simulation: str, base: dict[str, Any], skip_reason: str | None
) -> list[Path]:
    """Write the run files of a horizontal package and return their paths.

    See the module docstring for which files a horizontal package gets.

    No input overrides: the horizontal reader derives its file set from the
    snapshot list, so a file range means nothing to it, and its dataset comes
    from the committed test config the base run points at.
    """
    last_snap = last_snapshot_index(base["simulation"]["config"])
    written: list[Path] = []

    written.append(
        write_run(
            output_root / "core" / "test_binary.yaml",
            base,
            title="Mimic Core Test Run - Binary (C unit-tier parse fixture; never run horizontally)",
            output_format="binary",
            output_directory="./tests/data/output/binary/",
            output_filename="model",
            snapshot_list=[last_snap],
        )
    )
    if skip_reason is not None:
        return written

    written.append(
        write_run(
            output_root / "core" / "test_hdf5.yaml",
            base,
            title="Mimic Core Test Run - HDF5",
            output_format="hdf5",
            output_directory="./tests/data/output/hdf5/",
            output_filename="model",
            snapshot_list=[last_snap],
        )
    )
    written.append(
        write_run(
            output_root / "simulations" / simulation / "test_hdf5.yaml",
            base,
            title=f"Mimic {simulation} Simulation Test Run - HDF5",
            output_format="hdf5",
            output_directory="./tests/data/output/hdf5/",
            output_filename="model",
            snapshot_list=[last_snap],
        )
    )
    return written


def generate() -> None:
    model = selected_model()
    simulation = selected_simulation()
    output_root = generation_root(model, simulation)
    base = base_run_config(model, simulation)
    processing_order = package_processing_order(simulation)
    skip_reason = generic_tier_skip_reason(simulation)

    if processing_order == "horizontal":
        written = write_horizontal_runs(output_root, simulation, base, skip_reason)
    else:
        written = write_vertical_runs(output_root, simulation, base)

    manifest: dict[str, Any] = {
        "generated_by": "scripts/generate_test_inputs.py",
        "model": model,
        "simulation": simulation,
        "processing_order": processing_order,
        # The harness trusts only the files listed here, so a file an earlier
        # generation left for this pair can never be run by mistake.
        "run_files": sorted(path.relative_to(output_root).as_posix() for path in written),
    }
    if skip_reason is not None:
        manifest["skip_reason"] = skip_reason
    with (output_root / "manifest.json").open("w", encoding="utf-8") as handle:
        json.dump(manifest, handle, indent=2)
        handle.write("\n")

    print(f"Generated test inputs under {rel(output_root)}")
    if skip_reason is not None:
        print(f"Generic run-file-driven tests will skip: {skip_reason}")


if __name__ == "__main__":
    generate()
