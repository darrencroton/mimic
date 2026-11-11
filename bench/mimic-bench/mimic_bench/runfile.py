"""Build Mimic run files from a template, the spec's overrides and one design row.

A run file is the template with three kinds of change and no other:

* ``output.output_directory`` set to an absolute per-run directory;
* each of the spec's ``run_file.overrides`` (dotted paths such as ``output.snapshot_list``);
* each real design parameter's value under ``modules.parameters``, written with ``repr`` so
  that it parses back to the design's float. The inert control is never written.

The template is read with PyYAML's ``BaseLoader``, which keeps every scalar as its written
text, and the result is emitted by a dumper with no implicit typing, so every scalar this module
does not set is written back exactly as the template wrote it (``6.0e11`` stays ``6.0e11`` and an
empty value stays empty) and key order is kept. Comments are not kept; Mimic does not read them.

Before any run, :func:`check_template` checks the template against the spec: every real
parameter must be present under ``modules.parameters`` with the spec's default as its value;
every override must name an existing key and none may set what the bench owns (a design
parameter, the output directory, file name or format, or the tree-file range, :data:`OWNED_PATHS`);
the output must be HDF5, ``SubSteps`` an integer, and ``model.name`` and ``simulation.name`` the
spec's, after the overrides. A problem raises :class:`RunFileError`.

Usage:
    template = parse_template(template_bytes, template_path)
    check_template(template, spec)
    text = dump_run_file(build_run_file(template, spec, row_values, output_directory))
"""

from __future__ import annotations

import copy
from pathlib import Path
from typing import Any, Mapping, Sequence

import yaml
from mimic_bench.spec import Spec

PARAMETERS_PATH = ("modules", "parameters")
"""Where a template keeps the model's parameters."""

OUTPUT_DIRECTORY_PATH = ("output", "output_directory")
OUTPUT_FILENAME_PATH = ("output", "output_filename")
OUTPUT_FORMAT_PATH = ("output", "output_format")
SUBSTEPS_PATH = ("SubSteps",)
MODEL_PATH = ("model", "name")
SIMULATION_PATH = ("simulation", "name")
FIRST_FILE_PATH = ("input", "first_file")
LAST_FILE_PATH = ("input", "last_file")
SUPPORTED_OUTPUT_FORMAT = "hdf5"

OWNED_PATHS = (
    OUTPUT_DIRECTORY_PATH,
    OUTPUT_FILENAME_PATH,
    OUTPUT_FORMAT_PATH,
    FIRST_FILE_PATH,
    LAST_FILE_PATH,
)
"""Run-file keys an override may not set (nor any mapping that holds one), besides the design
parameters: the bench writes or verifies them."""


class RunFileError(ValueError):
    """A template that cannot carry the spec's design, or a value that cannot be written."""


class _TextDumper(yaml.BaseDumper):
    """A dumper with no implicit typing, so a plain scalar is written as plain text.

    ``BaseDumper`` resolves every plain scalar to a string, so a string such as ``0.17`` or
    ``6.0e11`` needs no quotes to read back as the same text, and is written without them.
    """


def _represent_str(dumper: yaml.BaseDumper, value: str) -> yaml.ScalarNode:
    """Represent a string as a scalar in the default (plain where possible) style."""
    return dumper.represent_scalar("tag:yaml.org,2002:str", value)


def _represent_dict(dumper: yaml.BaseDumper, value: dict) -> yaml.MappingNode:
    """Represent a mapping in its insertion order."""
    return dumper.represent_mapping("tag:yaml.org,2002:map", value.items())


def _represent_list(dumper: yaml.BaseDumper, value: list) -> yaml.SequenceNode:
    """Represent a list as a sequence."""
    return dumper.represent_sequence("tag:yaml.org,2002:seq", value)


_TextDumper.add_representer(str, _represent_str)
_TextDumper.add_representer(dict, _represent_dict)
_TextDumper.add_representer(list, _represent_list)


def parse_template(data: bytes, path: Path) -> dict:
    """Parse a run-file template from its bytes, keeping every scalar as its written text.

    Args:
        data: The template's bytes (UTF-8 YAML).
        path: The file they were read from, named in messages.

    Returns:
        The template as nested dicts and lists of strings; an empty value is ``""``.

    Raises:
        RunFileError: If the bytes are not UTF-8 YAML or not a mapping.
    """
    try:
        document = yaml.load(data.decode("utf-8"), Loader=yaml.BaseLoader)
    except (UnicodeDecodeError, yaml.YAMLError) as exc:
        raise RunFileError(f"{path}: run-file template is not valid YAML ({exc})") from exc
    if not isinstance(document, dict):
        raise RunFileError(f"{path}: run-file template is not a mapping")
    return document


def get_path(document: Mapping[str, Any], path: Sequence[str]) -> Any:
    """Return the value at a key path; raise :class:`RunFileError` naming the first missing key."""
    node: Any = document
    for depth, key in enumerate(path):
        if not isinstance(node, dict) or key not in node:
            raise RunFileError(f"{'.'.join(path[: depth + 1])}: not in the run-file template")
        node = node[key]
    return node


def _set_path(document: dict, path: Sequence[str], value: Any) -> None:
    """Replace the value at an existing key path."""
    get_path(document, path)
    node = document
    for key in path[:-1]:
        node = node[key]
    node[path[-1]] = value


def to_text(value: Any, path: str) -> Any:
    """Convert a spec value to the text form a run file carries, recursively for lists and maps.

    Floats are written with ``repr`` (so they parse back to the same float), integers with
    ``str`` and booleans as ``true``/``false``; strings are kept.

    Raises:
        RunFileError: If the value is of a type a run file cannot carry, or is ``None``.
    """
    if isinstance(value, bool):
        return "true" if value else "false"
    if isinstance(value, int):
        return str(value)
    if isinstance(value, float):
        return repr(value)
    if isinstance(value, str):
        return value
    if isinstance(value, list):
        return [to_text(item, f"{path}[{i}]") for i, item in enumerate(value)]
    if isinstance(value, dict):
        return {str(key): to_text(item, f"{path}.{key}") for key, item in value.items()}
    raise RunFileError(f"{path}: cannot write a value of type {type(value).__name__} to a run file")


def parse_float(text: Any, path: str) -> float:
    """Parse a run-file scalar as a float, raising :class:`RunFileError` naming ``path``."""
    try:
        return float(text)
    except (TypeError, ValueError) as exc:
        raise RunFileError(f"{path}: {text!r} is not a number") from exc


def apply_overrides(template: Mapping[str, Any], spec: Spec) -> dict:
    """Return a copy of ``template`` with each of the spec's ``run_file.overrides`` set."""
    document = copy.deepcopy(dict(template))
    for key, value in spec.run_file.overrides.items():
        _set_path(document, key.split("."), to_text(value, f"run_file.overrides.{key}"))
    return document


def check_template(template: Mapping[str, Any], spec: Spec) -> None:
    """Check, before any run, that a template can carry the spec's design.

    Args:
        template: The template as read by :func:`parse_template`.
        spec: The campaign spec.

    Raises:
        RunFileError: If ``modules.parameters`` lacks a real design parameter, a spec default
            differs from the template's value, an override sets a key the bench owns or names a
            key the template lacks, the output directory, file name or format is missing, the
            output format is not HDF5, or, after the overrides, ``SubSteps`` is missing or not an
            integer or ``model.name`` or ``simulation.name`` is not the spec's.
    """
    parameters = get_path(template, PARAMETERS_PATH)
    if not isinstance(parameters, dict):
        raise RunFileError(f"{'.'.join(PARAMETERS_PATH)}: expected a mapping in the template")
    for parameter in spec.parameters:
        if parameter.name not in parameters:
            raise RunFileError(
                f"{'.'.join(PARAMETERS_PATH)}.{parameter.name}: design parameter is not in the "
                "template"
            )
        path = f"{'.'.join(PARAMETERS_PATH)}.{parameter.name}"
        value = parse_float(parameters[parameter.name], path)
        if value != parameter.default:
            raise RunFileError(
                f"{path}: the spec's default {parameter.default!r} differs from the template's "
                f"value {parameters[parameter.name]!r}"
            )
    owned = [*OWNED_PATHS, *((*PARAMETERS_PATH, p.name) for p in spec.parameters)]
    for key in spec.run_file.overrides:
        parts = tuple(key.split("."))
        for path in owned:
            if path[: len(parts)] == parts:
                raise RunFileError(
                    f"run_file.overrides.{key}: may not set {'.'.join(path)}, which the bench "
                    "writes or verifies"
                )
        get_path(template, parts)
    get_path(template, OUTPUT_DIRECTORY_PATH)
    get_path(template, OUTPUT_FILENAME_PATH)
    output_format = get_path(template, OUTPUT_FORMAT_PATH)
    if output_format != SUPPORTED_OUTPUT_FORMAT:
        raise RunFileError(
            f"{'.'.join(OUTPUT_FORMAT_PATH)}: only {SUPPORTED_OUTPUT_FORMAT!r} output is "
            f"supported, found {output_format!r}"
        )
    document = apply_overrides(template, spec)
    substeps = get_path(document, SUBSTEPS_PATH)
    if not isinstance(substeps, str) or not substeps.strip().lstrip("+-").isdigit():
        raise RunFileError(f"SubSteps: {substeps!r} is not an integer")
    for path, wanted in ((MODEL_PATH, spec.model), (SIMULATION_PATH, spec.simulation)):
        found = get_path(document, path)
        if found != wanted:
            raise RunFileError(
                f"{'.'.join(path)}: the run file names {found!r}, the spec {wanted!r}"
            )


def build_run_file(
    template: Mapping[str, Any],
    spec: Spec,
    values: Sequence[float],
    output_directory: Path,
) -> dict:
    """Build a run file's document for one design row.

    Args:
        template: The template as read by :func:`parse_template`; it is not modified.
        spec: The campaign spec.
        values: The row's values in :attr:`Spec.all_parameters` order (the inert control's
            value is accepted and ignored).
        output_directory: The run's output directory; written as an absolute path.

    Returns:
        The run file as nested dicts and lists of strings, in the template's key order.

    Raises:
        RunFileError: If ``values`` has the wrong length or a key path is missing.
    """
    if len(values) != len(spec.all_parameters):
        raise RunFileError(
            f"design row has {len(values)} values, expected {len(spec.all_parameters)}"
        )
    document = apply_overrides(template, spec)
    _set_path(document, OUTPUT_DIRECTORY_PATH, str(Path(output_directory).resolve()))
    for parameter, value in zip(spec.all_parameters, values, strict=True):
        if parameter.inert:
            continue
        _set_path(document, (*PARAMETERS_PATH, parameter.name), repr(float(value)))
    return document


def dump_run_file(document: Mapping[str, Any]) -> str:
    """Emit a run-file document as YAML text, keeping key order and every scalar's text."""
    return yaml.dump(
        dict(document), Dumper=_TextDumper, sort_keys=False, default_flow_style=False, width=4096
    )
