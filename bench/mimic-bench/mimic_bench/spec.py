"""Load and validate a campaign spec.

A campaign spec is a YAML file that declares everything a campaign varies or judges by: the
run-file template and its overrides, the design parameters with a range and a scale each, the
design sets and seed, the statistic, the rule that picks the judged outputs, the constraint and
its conversion, the discrepancy budgets, the rejection rule, the pass criteria and the
candidate emulators. Nothing in this package names a model, a parameter or a property; the spec
is the only place they appear. ``constraint.table`` is resolved against the spec file's directory
(:attr:`Spec.constraint_path`) and ``run_file.template`` against the Mimic root.

Scalars resolve by the YAML 1.2 core schema, not PyYAML's YAML 1.1 default: ``063`` is the
integer 63 (not octal), ``1e-2`` and ``1.0e1`` are floats, only ``true``/``false`` (in any of the
cases ``true``, ``True``, ``TRUE``) are booleans, and ``yes``, ``on`` or a date stay strings.

Every problem raises :class:`SpecError` with a message that starts with the dotted path of the
offending key (for example ``parameters[2].low``), so a caller can print it and exit with
status 2. The loader is strict: an unknown key at any level, a missing item and an invalid
value are all errors, and a repeated YAML key is an error rather than a silent override.

Usage:
    spec = load_spec(Path("specs/example.yaml"))
"""

from __future__ import annotations

import math
import re
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Mapping, Sequence

import numpy as np
import yaml

SCALES = ("log", "linear")
"""The scales a parameter may be sampled in."""

SET_NAMES = ("pilot", "train", "holdout")
"""The design sets, in the order that fixes each one's random stream."""

NOISE_KINDS = ("poisson_log10",)
"""The noise models a spec may declare."""

EMULATOR_NAMES = ("gaussian_process", "bayes_linear")
"""The emulators a spec may list under ``emulators``; :data:`mimic_bench.analyse.EMULATORS` maps
each name to its class."""

LOG10_E_SQUARED = math.log10(math.e) ** 2
"""``(log10 e)^2``, the variance of ``log10 N`` per unit of ``1 / N`` for a Poisson count N."""


class SpecError(ValueError):
    """A spec that is missing an item or holds an invalid one; the message names the key."""


@dataclass(frozen=True)
class Parameter:
    """One design dimension: a range, the template's default and the scale it is sampled in.

    Attributes:
        name: The key the value is written under (a run-file parameter, or the inert control).
        low: Lower bound of the range.
        high: Upper bound of the range.
        default: The template's value, inside the range.
        scale: ``"log"`` (uniform in log10 of the value) or ``"linear"``.
        inert: True for a control the model never sees, which no run file carries.
    """

    name: str
    low: float
    high: float
    default: float
    scale: str
    inert: bool = False

    def to_unit(self, values: Any) -> np.ndarray:
        """Map values in the range to the unit interval, linearly in this parameter's scale."""
        x = np.asarray(values, dtype=float)
        if self.scale == "log":
            lo, hi = math.log10(self.low), math.log10(self.high)
            return (np.log10(x) - lo) / (hi - lo)
        return (x - self.low) / (self.high - self.low)

    def from_unit(self, unit: Any) -> np.ndarray:
        """Map unit-interval values to the range, linearly in this parameter's scale."""
        u = np.asarray(unit, dtype=float)
        if self.scale == "log":
            lo, hi = math.log10(self.low), math.log10(self.high)
            return 10.0 ** (lo + u * (hi - lo))
        return self.low + u * (self.high - self.low)


@dataclass(frozen=True)
class RunFileSpec:
    """The run-file template (relative to the Mimic root) and the dotted-path overrides."""

    template: str
    overrides: Mapping[str, Any]


@dataclass(frozen=True)
class SetSpec:
    """One design set: optional default and corner rows, then Latin-hypercube points."""

    latin_hypercube: int
    include_defaults: bool
    include_corners: bool


@dataclass(frozen=True)
class DesignSpec:
    """The design seed and the three sets, keyed by name."""

    seed: int
    sets: Mapping[str, SetSpec]


@dataclass(frozen=True)
class StatisticSpec:
    """A mass function of one galaxy property at one snapshot."""

    kind: str
    property: str
    snapshot: int
    selection: str
    bin_width_dex: float
    bin_centres: str


@dataclass(frozen=True)
class NoiseModel:
    """The declared noise of the emulated statistic, as a variance in its units squared.

    Kind ``poisson_log10``: a bin holding N galaxies has a Poisson count, whose noise carried to
    ``log10 N`` (and so to ``log10`` of the density, which differs by a constant) has variance
    ``scale (log10 e)^2 / max(N, 1)`` in dex squared; the floor of one galaxy matches the
    statistic's own single-galaxy floor. The model is declared, not fitted.

    Attributes:
        kind: The noise model's kind, one of :data:`NOISE_KINDS`.
        scale: Positive multiplier of the Poisson variance (1 for Poisson noise).
    """

    kind: str
    scale: float

    def __post_init__(self) -> None:
        """Refuse a kind this class does not compute, or a scale the loader would refuse.

        Raises:
            ValueError: If ``kind`` is not one of :data:`NOISE_KINDS`, or ``scale`` is not a
                finite positive number (a boolean is not a number here, as in the loader).
        """
        if self.kind not in NOISE_KINDS:
            raise ValueError(
                f"noise_model.kind: {self.kind!r} is not supported (supported: {NOISE_KINDS})"
            )
        scale = self.scale
        if (
            isinstance(scale, bool)
            or not isinstance(scale, (int, float))
            or not math.isfinite(scale)
            or scale <= 0
        ):
            raise ValueError(
                f"noise_model.scale: must be a finite positive number, found {scale!r}"
            )

    def variance(self, counts: Any) -> np.ndarray:
        """Return the declared variance, in dex squared, of each count's ``log10``.

        Args:
            counts: Galaxy counts (any shape), finite and not negative; need not be integers,
                so that a predicted count may be passed.

        Returns:
            ``scale (log10 e)^2 / max(N, 1)`` element by element, with the shape of ``counts``.

        Raises:
            ValueError: If a count is negative or not finite.
        """
        n = np.asarray(counts, dtype=float)
        if not np.all(np.isfinite(n)):
            raise ValueError("noise_model.variance: a count is not finite")
        if np.any(n < 0.0):
            raise ValueError("noise_model.variance: a count is negative")
        return self.scale * LOG10_E_SQUARED / np.maximum(n, 1.0)


@dataclass(frozen=True)
class SelectionRule:
    """The rule that picks the judged (core) and exploratory outputs from a pilot capture."""

    min_bin_centre: float
    require_error_below_value: bool
    min_galaxies: int
    min_runs: int


@dataclass(frozen=True)
class OutputsSpec:
    """The selection rule and the outputs it chose (as constraint-table log10 M values)."""

    selection_rule: SelectionRule
    core: tuple[float, ...]
    exploratory: tuple[float, ...]


@dataclass(frozen=True)
class ConversionSpec:
    """How the unconverted constraint table is brought to the run's conventions.

    Converted mass is ``x + mass_h_power * log10(h) + mass_shift_dex`` and converted density is
    ``phi * h ** density_h_power``, with h taken from the run.
    """

    mass_h_power: float
    mass_shift_dex: float
    density_h_power: float


@dataclass(frozen=True)
class ConstraintSpec:
    """The constraint table (relative to the spec file's directory) and its conversion."""

    table: str
    conversion: ConversionSpec


@dataclass(frozen=True)
class DiscrepancySpec:
    """The scanned discrepancy budgets and the gate budget, in dex."""

    budgets_dex: tuple[float, ...]
    gate_budget_dex: float


@dataclass(frozen=True)
class ImplausibilitySpec:
    """Reject when the ``rank``-th largest implausibility exceeds ``threshold``."""

    rank: int
    threshold: float


@dataclass(frozen=True)
class PerOutputCriteria:
    """The criteria one output must meet on the holdout's standardised errors."""

    within_sigma: float
    within_min_fraction: float
    beyond_sigma: float
    beyond_max_fraction: float
    min_rms: float


@dataclass(frozen=True)
class Criteria:
    """Every pass criterion of the campaign."""

    per_output: PerOutputCriteria
    calibrated_min_fraction: float
    false_exclusion_max_rate: float
    false_exclusion_min_kept_runs: int
    non_implausible_max_fraction: float


@dataclass(frozen=True)
class Spec:
    """A validated campaign spec.

    Attributes:
        model: Name of the model package.
        simulation: Name of the simulation.
        run_file: The run-file template and overrides.
        parameters: The real design parameters, in the order their columns are written.
        inert_control: The control sampled like a parameter that no run file carries.
        design: The seed and the sets.
        statistic: What is measured.
        noise_model: The statistic's declared noise.
        outputs: The selection rule and the judged and exploratory outputs.
        constraint: The constraint table and its conversion.
        discrepancy: The budgets.
        implausibility: The rejection rule.
        criteria: The pass criteria.
        emulators: The candidate emulators' names in verdict order, the first the primary.
        source: The file the spec was read from, when it was read from one.
    """

    model: str
    simulation: str
    run_file: RunFileSpec
    parameters: tuple[Parameter, ...]
    inert_control: Parameter
    design: DesignSpec
    statistic: StatisticSpec
    noise_model: NoiseModel
    outputs: OutputsSpec
    constraint: ConstraintSpec
    discrepancy: DiscrepancySpec
    implausibility: ImplausibilitySpec
    criteria: Criteria
    emulators: tuple[str, ...]
    source: Path | None = None

    @property
    def all_parameters(self) -> tuple[Parameter, ...]:
        """The design dimensions in column order: the real parameters, then the inert control."""
        return (*self.parameters, self.inert_control)

    @property
    def constraint_path(self) -> Path:
        """The constraint table, resolved against the spec file's directory.

        An absolute ``constraint.table`` is used as written, as is a relative one when the spec
        was not read from a file.
        """
        table = Path(self.constraint.table)
        if table.is_absolute() or self.source is None:
            return table
        return Path(self.source).parent / table


class _CoreSchemaLoader(yaml.SafeLoader):
    """A safe YAML loader with the YAML 1.2 core schema's scalars that rejects a repeated key."""

    yaml_implicit_resolvers: dict = {}

    def construct_yaml_int(self, node: yaml.ScalarNode) -> int:
        """Build a core-schema integer: decimal (leading zeros allowed), ``0o`` octal or ``0x``."""
        text = self.construct_scalar(node)
        if text.startswith(("0o", "0x")):
            return int(text[2:], 8 if text[1] == "o" else 16)
        return int(text, 10)

    def construct_yaml_float(self, node: yaml.ScalarNode) -> float:
        """Build a core-schema float, ``.inf`` and ``.nan`` included."""
        text = self.construct_scalar(node).lower()
        if text.endswith(".inf"):
            return -math.inf if text.startswith("-") else math.inf
        return math.nan if text == ".nan" else float(text)

    def construct_mapping(self, node: yaml.MappingNode, deep: bool = False) -> dict:
        """Build a mapping, raising :class:`SpecError` when a key is not a scalar or repeats."""
        seen = set()
        for key_node, _ in node.value:
            key = self.construct_object(key_node, deep=True)
            if isinstance(key, (list, dict)):
                raise SpecError(
                    f"{key!r}: a mapping key must be a scalar (line {key_node.start_mark.line + 1})"
                )
            if key in seen:
                raise SpecError(
                    f"{key}: key is repeated in one mapping (line {key_node.start_mark.line + 1})"
                )
            seen.add(key)
        return super().construct_mapping(node, deep)


for _tag, _pattern, _first in (
    ("null", r"~|null|Null|NULL|", ["~", "n", "N", ""]),
    ("bool", r"true|True|TRUE|false|False|FALSE", list("tTfF")),
    ("int", r"[-+]?[0-9]+|0o[0-7]+|0x[0-9a-fA-F]+", list("-+0123456789")),
    (
        "float",
        r"[-+]?(\.[0-9]+|[0-9]+(\.[0-9]*)?)([eE][-+]?[0-9]+)?|[-+]?\.(inf|Inf|INF)|\.(nan|NaN|NAN)",
        list("-+.0123456789"),
    ),
):
    _CoreSchemaLoader.add_implicit_resolver(
        f"tag:yaml.org,2002:{_tag}", re.compile(f"^(?:{_pattern})$"), _first
    )
_CoreSchemaLoader.add_constructor("tag:yaml.org,2002:int", _CoreSchemaLoader.construct_yaml_int)
_CoreSchemaLoader.add_constructor("tag:yaml.org,2002:float", _CoreSchemaLoader.construct_yaml_float)


def load_spec(path: Path) -> Spec:
    """Read and validate a spec file; :func:`parse_spec_text` of its bytes.

    Raises:
        SpecError: If the file is unreadable, is not YAML, or fails validation.
    """
    path = Path(path)
    try:
        data = path.read_bytes()
    except OSError as exc:
        raise SpecError(f"{path}: cannot read spec ({exc.strerror})") from exc
    return parse_spec_text(data, path)


def parse_spec_text(data: bytes, source: Path) -> Spec:
    """Parse and validate a spec from the bytes of its file.

    Args:
        data: The file's bytes (UTF-8 YAML).
        source: The file they were read from, named in messages and recorded on the result.

    Returns:
        The validated spec, with ``source`` set.

    Raises:
        SpecError: If the bytes are not UTF-8 YAML or fail validation. The message starts with
            ``source`` and names the offending key.
    """
    source = Path(source)
    try:
        raw = yaml.load(data.decode("utf-8"), Loader=_CoreSchemaLoader)  # a SafeLoader subclass
    except (UnicodeDecodeError, yaml.YAMLError) as exc:
        raise SpecError(f"{source}: not valid YAML ({exc})") from exc
    except SpecError as exc:
        raise SpecError(f"{source}: {exc}") from exc
    try:
        return parse_spec(raw, source=source)
    except SpecError as exc:
        raise SpecError(f"{source}: {exc}") from exc


def parse_spec(raw: Any, source: Path | None = None) -> Spec:
    """Validate a spec that is already a parsed mapping.

    Args:
        raw: The parsed YAML document.
        source: The file it came from, recorded on the result.

    Returns:
        The validated spec.

    Raises:
        SpecError: If any item is unknown, missing or invalid; the message starts with the
            dotted path of the offending key.
    """
    top = _mapping(raw, "spec")
    _check_keys(
        top,
        "",
        (
            "model",
            "simulation",
            "run_file",
            "parameters",
            "inert_control",
            "design",
            "statistic",
            "noise_model",
            "outputs",
            "constraint",
            "discrepancy",
            "implausibility",
            "criteria",
            "emulators",
        ),
    )
    parameters = _parse_parameters(top["parameters"])
    inert = _parse_parameter(top["inert_control"], "inert_control", inert=True)
    _check_unique_names(parameters, inert)
    design = _parse_design(top["design"])
    outputs = _parse_outputs(top["outputs"])
    discrepancy = _parse_discrepancy(top["discrepancy"])
    if outputs.selection_rule.min_runs > design.sets["pilot"].latin_hypercube:
        raise SpecError(
            "outputs.selection_rule.min_runs: "
            f"{outputs.selection_rule.min_runs} exceeds the pilot's "
            f"{design.sets['pilot'].latin_hypercube} Latin-hypercube runs"
        )
    return Spec(
        model=_string(top["model"], "model"),
        simulation=_string(top["simulation"], "simulation"),
        run_file=_parse_run_file(top["run_file"]),
        parameters=parameters,
        inert_control=inert,
        design=design,
        statistic=_parse_statistic(top["statistic"]),
        noise_model=_parse_noise_model(top["noise_model"]),
        outputs=outputs,
        constraint=_parse_constraint(top["constraint"]),
        discrepancy=discrepancy,
        implausibility=_parse_implausibility(top["implausibility"]),
        criteria=_parse_criteria(top["criteria"]),
        emulators=_parse_emulators(top["emulators"]),
        source=source,
    )


def _join(path: str, key: Any) -> str:
    """Join a dotted path and a key."""
    return f"{path}.{key}" if path else str(key)


def _mapping(value: Any, path: str) -> dict:
    """Return ``value`` if it is a mapping, else raise naming ``path``."""
    if not isinstance(value, dict):
        raise SpecError(f"{path}: expected a mapping, found {type(value).__name__}")
    return value


def _check_keys(
    mapping: dict, path: str, required: Sequence[str], optional: Sequence[str] = ()
) -> None:
    """Raise on a key that is not allowed, then on a required key that is absent."""
    allowed = (*required, *optional)
    for key in mapping:
        if key not in allowed:
            raise SpecError(f"{_join(path, key)}: unknown key (allowed here: {', '.join(allowed)})")
    for key in required:
        if key not in mapping:
            raise SpecError(f"{_join(path, key)}: required key is missing")


def _string(value: Any, path: str) -> str:
    """Return a non-empty string."""
    if not isinstance(value, str) or not value.strip():
        raise SpecError(f"{path}: expected a non-empty string, found {value!r}")
    return value


def _boolean(value: Any, path: str) -> bool:
    """Return a boolean."""
    if not isinstance(value, bool):
        raise SpecError(f"{path}: expected true or false, found {value!r}")
    return value


def _number(value: Any, path: str, *, positive: bool = False) -> float:
    """Return a finite number (not a boolean), positive when asked."""
    if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value):
        raise SpecError(f"{path}: expected a finite number, found {value!r}")
    if positive and value <= 0:
        raise SpecError(f"{path}: must be positive, found {value!r}")
    return float(value)


def _integer(value: Any, path: str, *, minimum: int = 1) -> int:
    """Return an integer (not a boolean, not a float) of at least ``minimum``."""
    if isinstance(value, bool) or not isinstance(value, int) or value < minimum:
        kind = "a positive integer" if minimum == 1 else f"an integer of at least {minimum}"
        raise SpecError(f"{path}: must be {kind}, found {value!r}")
    return value


def _fraction(value: Any, path: str) -> float:
    """Return a fraction in (0, 1]."""
    number = _number(value, path)
    if not 0.0 < number <= 1.0:
        raise SpecError(f"{path}: must be a fraction in (0, 1], found {value!r}")
    return number


def _parse_run_file(raw: Any) -> RunFileSpec:
    """Validate ``run_file``: a template path and a mapping of dotted-path overrides."""
    raw = _mapping(raw, "run_file")
    _check_keys(raw, "run_file", ("template", "overrides"))
    overrides = _mapping(raw["overrides"], "run_file.overrides")
    for key, value in overrides.items():
        if not isinstance(key, str) or not all(key.split(".")):
            raise SpecError(
                f"{_join('run_file.overrides', key)}: expected a dotted run-file path "
                "such as 'section.key'"
            )
        _check_finite(value, _join("run_file.overrides", key))
    return RunFileSpec(_string(raw["template"], "run_file.template"), dict(overrides))


def _check_finite(value: Any, path: str) -> None:
    """Raise naming ``path`` (``[i]`` for a list item) on a non-finite number inside ``value``."""
    if isinstance(value, float) and not math.isfinite(value):
        raise SpecError(f"{path}: expected a finite number, found {value!r}")
    if isinstance(value, list):
        for index, item in enumerate(value):
            _check_finite(item, f"{path}[{index}]")
    elif isinstance(value, dict):
        for key, item in value.items():
            _check_finite(item, _join(path, key))


def _parse_parameter(raw: Any, path: str, *, inert: bool = False) -> Parameter:
    """Validate one parameter or the inert control."""
    raw = _mapping(raw, path)
    _check_keys(raw, path, ("name", "low", "high", "default", "scale"))
    name = _string(raw["name"], f"{path}.name")
    scale = raw["scale"]
    if not isinstance(scale, str) or scale not in SCALES:
        raise SpecError(f"{path}.scale: must be 'log' or 'linear', found {scale!r}")
    low = _number(raw["low"], f"{path}.low")
    high = _number(raw["high"], f"{path}.high")
    if scale == "log":
        for key, bound in (("low", low), ("high", high)):
            if bound <= 0:
                raise SpecError(f"{path}.{key}: must be positive on a log scale, found {bound!r}")
    if low >= high:
        raise SpecError(f"{path}.low: must be below high, found low={low!r} and high={high!r}")
    default = _number(raw["default"], f"{path}.default")
    if not low <= default <= high:
        raise SpecError(f"{path}.default: {default!r} is outside the range [{low!r}, {high!r}]")
    return Parameter(name, low, high, default, scale, inert)


def _parse_parameters(raw: Any) -> tuple[Parameter, ...]:
    """Validate ``parameters``: a non-empty list of parameters."""
    if not isinstance(raw, list) or not raw:
        raise SpecError("parameters: expected a non-empty list of parameters")
    return tuple(_parse_parameter(item, f"parameters[{i}]") for i, item in enumerate(raw))


def _check_unique_names(parameters: tuple[Parameter, ...], inert: Parameter) -> None:
    """Raise when two design dimensions share a name."""
    seen: set[str] = set()
    for i, parameter in enumerate(parameters):
        if parameter.name in seen:
            raise SpecError(f"parameters[{i}].name: duplicate parameter name {parameter.name!r}")
        seen.add(parameter.name)
    if inert.name in seen:
        raise SpecError(f"inert_control.name: duplicate parameter name {inert.name!r}")


def _parse_design(raw: Any) -> DesignSpec:
    """Validate ``design``: a seed and exactly the three named sets."""
    raw = _mapping(raw, "design")
    _check_keys(raw, "design", ("seed", "sets"))
    seed = _integer(raw["seed"], "design.seed", minimum=0)
    sets_raw = _mapping(raw["sets"], "design.sets")
    _check_keys(sets_raw, "design.sets", SET_NAMES)
    sets = {}
    for name in SET_NAMES:
        path = f"design.sets.{name}"
        entry = _mapping(sets_raw[name], path)
        _check_keys(entry, path, ("latin_hypercube",), ("include_defaults", "include_corners"))
        sets[name] = SetSpec(
            latin_hypercube=_integer(entry["latin_hypercube"], f"{path}.latin_hypercube"),
            include_defaults=_boolean(
                entry.get("include_defaults", False), f"{path}.include_defaults"
            ),
            include_corners=_boolean(
                entry.get("include_corners", False), f"{path}.include_corners"
            ),
        )
    return DesignSpec(seed, sets)


def _parse_statistic(raw: Any) -> StatisticSpec:
    """Validate ``statistic``: the one supported kind, with its property, snapshot and bins."""
    raw = _mapping(raw, "statistic")
    _check_keys(
        raw,
        "statistic",
        ("kind", "property", "snapshot", "selection", "bin_width_dex", "bin_centres"),
    )
    for key, allowed in (
        ("kind", "mass_function"),
        ("selection", "positive"),
        ("bin_centres", "constraint"),
    ):
        if raw[key] != allowed:
            raise SpecError(f"statistic.{key}: only {allowed!r} is supported, found {raw[key]!r}")
    return StatisticSpec(
        kind=raw["kind"],
        property=_string(raw["property"], "statistic.property"),
        snapshot=_integer(raw["snapshot"], "statistic.snapshot", minimum=0),
        selection=raw["selection"],
        bin_width_dex=_number(raw["bin_width_dex"], "statistic.bin_width_dex", positive=True),
        bin_centres=raw["bin_centres"],
    )


def _parse_noise_model(raw: Any) -> NoiseModel:
    """Validate ``noise_model``: a supported kind and a finite positive scale."""
    raw = _mapping(raw, "noise_model")
    _check_keys(raw, "noise_model", ("kind", "scale"))
    kind = raw["kind"]
    if not isinstance(kind, str) or kind not in NOISE_KINDS:
        supported = ", ".join(repr(k) for k in NOISE_KINDS)
        raise SpecError(f"noise_model.kind: only {supported} is supported, found {kind!r}")
    return NoiseModel(kind=kind, scale=_number(raw["scale"], "noise_model.scale", positive=True))


def _parse_output_list(raw: Any, path: str) -> tuple[float, ...]:
    """Validate a list of constraint-table log10 M values (possibly empty)."""
    if not isinstance(raw, list):
        raise SpecError(f"{path}: expected a list, found {type(raw).__name__}")
    values = tuple(_number(item, f"{path}[{i}]") for i, item in enumerate(raw))
    if len(set(values)) != len(values):
        raise SpecError(f"{path}: lists a value more than once")
    return values


def _parse_outputs(raw: Any) -> OutputsSpec:
    """Validate ``outputs``: the selection rule and the two output lists."""
    raw = _mapping(raw, "outputs")
    _check_keys(raw, "outputs", ("selection_rule", "core", "exploratory"))
    rule_raw = _mapping(raw["selection_rule"], "outputs.selection_rule")
    rule_path = "outputs.selection_rule"
    _check_keys(
        rule_raw,
        rule_path,
        ("min_bin_centre", "require_error_below_value", "min_galaxies", "min_runs"),
    )
    rule = SelectionRule(
        min_bin_centre=_number(rule_raw["min_bin_centre"], f"{rule_path}.min_bin_centre"),
        require_error_below_value=_boolean(
            rule_raw["require_error_below_value"], f"{rule_path}.require_error_below_value"
        ),
        min_galaxies=_integer(rule_raw["min_galaxies"], f"{rule_path}.min_galaxies"),
        min_runs=_integer(rule_raw["min_runs"], f"{rule_path}.min_runs"),
    )
    core = _parse_output_list(raw["core"], "outputs.core")
    exploratory = _parse_output_list(raw["exploratory"], "outputs.exploratory")
    shared = set(core) & set(exploratory)
    if shared:
        raise SpecError(f"outputs.exploratory: {sorted(shared)} is also a core output")
    return OutputsSpec(rule, core, exploratory)


def _parse_constraint(raw: Any) -> ConstraintSpec:
    """Validate ``constraint``: the table path and its declared conversion."""
    raw = _mapping(raw, "constraint")
    _check_keys(raw, "constraint", ("table", "conversion"))
    conversion = _mapping(raw["conversion"], "constraint.conversion")
    path = "constraint.conversion"
    _check_keys(conversion, path, ("hubble_h", "mass_h_power", "mass_shift_dex", "density_h_power"))
    if conversion["hubble_h"] != "run":
        raise SpecError(
            f"{path}.hubble_h: only 'run' (h taken from the run) is supported, "
            f"found {conversion['hubble_h']!r}"
        )
    return ConstraintSpec(
        table=_string(raw["table"], "constraint.table"),
        conversion=ConversionSpec(
            mass_h_power=_number(conversion["mass_h_power"], f"{path}.mass_h_power"),
            mass_shift_dex=_number(conversion["mass_shift_dex"], f"{path}.mass_shift_dex"),
            density_h_power=_number(conversion["density_h_power"], f"{path}.density_h_power"),
        ),
    )


def _parse_discrepancy(raw: Any) -> DiscrepancySpec:
    """Validate ``discrepancy``: positive scanned budgets and a gate budget among them."""
    raw = _mapping(raw, "discrepancy")
    _check_keys(raw, "discrepancy", ("budgets_dex", "gate_budget_dex"))
    budgets_raw = raw["budgets_dex"]
    if not isinstance(budgets_raw, list) or not budgets_raw:
        raise SpecError("discrepancy.budgets_dex: expected a non-empty list of budgets")
    budgets = tuple(
        _number(item, f"discrepancy.budgets_dex[{i}]", positive=True)
        for i, item in enumerate(budgets_raw)
    )
    if len(set(budgets)) != len(budgets):
        raise SpecError("discrepancy.budgets_dex: lists a budget more than once")
    gate = _number(raw["gate_budget_dex"], "discrepancy.gate_budget_dex", positive=True)
    if gate not in budgets:
        raise SpecError(
            f"discrepancy.gate_budget_dex: {gate!r} is not one of the scanned budgets "
            f"{list(budgets)}"
        )
    return DiscrepancySpec(budgets, gate)


def _parse_implausibility(raw: Any) -> ImplausibilitySpec:
    """Validate ``implausibility``: which largest value is cut on, and the cut."""
    raw = _mapping(raw, "implausibility")
    _check_keys(raw, "implausibility", ("rank", "threshold"))
    return ImplausibilitySpec(
        rank=_integer(raw["rank"], "implausibility.rank"),
        threshold=_number(raw["threshold"], "implausibility.threshold", positive=True),
    )


def _parse_criteria(raw: Any) -> Criteria:
    """Validate ``criteria``: the per-output criteria and the campaign-level ones."""
    raw = _mapping(raw, "criteria")
    _check_keys(
        raw,
        "criteria",
        (
            "per_output",
            "calibrated_min_fraction",
            "false_exclusion_max_rate",
            "false_exclusion_min_kept_runs",
            "non_implausible_max_fraction",
        ),
    )
    per_raw = _mapping(raw["per_output"], "criteria.per_output")
    path = "criteria.per_output"
    _check_keys(
        per_raw,
        path,
        (
            "within_sigma",
            "within_min_fraction",
            "beyond_sigma",
            "beyond_max_fraction",
            "min_rms",
        ),
    )
    per_output = PerOutputCriteria(
        within_sigma=_number(per_raw["within_sigma"], f"{path}.within_sigma", positive=True),
        within_min_fraction=_fraction(
            per_raw["within_min_fraction"], f"{path}.within_min_fraction"
        ),
        beyond_sigma=_number(per_raw["beyond_sigma"], f"{path}.beyond_sigma", positive=True),
        beyond_max_fraction=_fraction(
            per_raw["beyond_max_fraction"], f"{path}.beyond_max_fraction"
        ),
        min_rms=_number(per_raw["min_rms"], f"{path}.min_rms", positive=True),
    )
    return Criteria(
        per_output=per_output,
        calibrated_min_fraction=_fraction(
            raw["calibrated_min_fraction"], "criteria.calibrated_min_fraction"
        ),
        false_exclusion_max_rate=_fraction(
            raw["false_exclusion_max_rate"], "criteria.false_exclusion_max_rate"
        ),
        false_exclusion_min_kept_runs=_integer(
            raw["false_exclusion_min_kept_runs"], "criteria.false_exclusion_min_kept_runs"
        ),
        non_implausible_max_fraction=_fraction(
            raw["non_implausible_max_fraction"], "criteria.non_implausible_max_fraction"
        ),
    )


def _parse_emulators(raw: Any) -> tuple[str, ...]:
    """Validate ``emulators``: a non-empty list of distinct names from :data:`EMULATOR_NAMES`."""
    if not isinstance(raw, list) or not raw:
        raise SpecError("emulators: expected a non-empty list of emulator names")
    for i, name in enumerate(raw):
        if name not in EMULATOR_NAMES:
            raise SpecError(
                f"emulators[{i}]: {name!r} is not an emulator (known: {', '.join(EMULATOR_NAMES)})"
            )
    if len(set(raw)) != len(raw):
        raise SpecError("emulators: lists an emulator more than once")
    return tuple(raw)
