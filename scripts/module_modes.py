#!/usr/bin/env python3
"""
Processing Mode Descriptors for Mimic

Single Python source of truth for module processing modes, shared by the module
registry generator (scripts/generate_module_registry.py) and the metadata
validator (scripts/validate_modules.py). It mirrors the C descriptor table in
src/core/processing_modes.c: each mode names its configuration string, its C
enumerator and the callback family it dispatches to.

Lookups fail closed. A mode string absent from PROCESSING_MODES is invalid
everywhere, and no mode is assigned to a family by default: adding a mode or a
callback family needs an explicit descriptor here, a matching C table entry and
an implementation.

It also owns the one module_info.yaml key that qualifies a mode:
``snapshot_distribution`` (``serial_only`` or ``collective``, default
``serial_only``), valid only for a module whose supported_processing_modes
include ``process_snapshot`` and never for a utility module (a utility
collection runs no process_snapshot callback, even if its metadata lists the
mode). It mirrors enum SnapshotDistribution in
src/core/module_interface.h; the generator emits it into every module's
registration as ``.snapshot_distribution`` and the validator checks it through
snapshot_distribution_errors(). ``collective`` declares that the module's
process_snapshot reaches every whole-population quantity through the snapshot
collectives (src/core/snapshot_collectives.h); ``serial_only`` means it assumes
the complete population is resident, so startup refuses it under
modules.post_snapshot when NTask > 1.

It also owns the ``rates:`` key of a ``process_rates`` module: the transfers its
law returns coefficients for, the observers that accumulate a transfer's moved
amount, and ``reads_central``. rate_metadata_errors() holds every rule both tools
apply to that key, and global_transfer_index() the canonical order of every
compiled rate module's transfers (module name, then declaration order), which the
generator emits into each transfer's descriptor. The rules read the ``reservoir:``
declarations of the galaxy properties (scripts/generate_properties.py validates
those on their own), so a transfer end is checked against the property metadata of
the build being generated.

This file is a generation input. compute_metadata_hash() in the generator and
compute_module_metadata_hash() in scripts/check_generated.py both hash its path
and bytes, and the Makefile module-generation stamp depends on it, so editing it
invalidates generated module registration.

This module is imported, not run.
"""

import re
from dataclasses import dataclass, field
from typing import Any, Dict, List, Optional, Set, Tuple

# ==============================================================================
# CALLBACK FAMILIES
# ==============================================================================


@dataclass(frozen=True)
class CallbackFamily:
    """One typed struct Module callback and how generated registration binds it.

    Attributes:
        key: Short family identifier used by mode descriptors.
        field: struct Module member that holds the callback.
        symbol_suffix: Suffix appended to the module name for the C symbol.
        parameters: C parameter list of the callback, for the extern declaration.
    """

    key: str
    field: str
    symbol_suffix: str
    parameters: str


FAMILY_FOF = "fof"
FAMILY_SNAPSHOT = "snapshot"
FAMILY_RATE = "rate"

# Ordered as the callbacks appear in struct Module (src/core/module_interface.h).
CALLBACK_FAMILIES: Tuple[CallbackFamily, ...] = (
    CallbackFamily(
        key=FAMILY_FOF,
        field="process",
        symbol_suffix="_process",
        parameters="struct ModuleContext *ctx, struct Halo *halos, int ngal",
    ),
    CallbackFamily(
        key=FAMILY_SNAPSHOT,
        field="process_snapshot",
        symbol_suffix="_process_snapshot",
        parameters="const struct SnapshotContext *ctx, const struct Halo *halos, int64_t count",
    ),
    CallbackFamily(
        key=FAMILY_RATE,
        field="rates",
        symbol_suffix="_rates",
        parameters="const struct RateContext *ctx, const struct Halo *halo, double *coefficients",
    ),
)

# ==============================================================================
# PROCESSING MODES
# ==============================================================================


@dataclass(frozen=True)
class ProcessingModeDescriptor:
    """One processing mode.

    Attributes:
        name: Configuration string used in module_info.yaml and run YAML.
        enum: C enumerator in enum ProcessingMode.
        family: Key of the CallbackFamily this mode dispatches to.
    """

    name: str
    enum: str
    family: str


# Ordered as enum ProcessingMode (src/core/module_interface.h).
PROCESSING_MODES: Tuple[ProcessingModeDescriptor, ...] = (
    ProcessingModeDescriptor("process_full_halo", "PROCESSING_MODE_FULL_HALO", FAMILY_FOF),
    ProcessingModeDescriptor("process_per_event", "PROCESSING_MODE_PER_EVENT", FAMILY_FOF),
    ProcessingModeDescriptor("process_by_galaxy", "PROCESSING_MODE_BY_GALAXY", FAMILY_FOF),
    ProcessingModeDescriptor("process_snapshot", "PROCESSING_MODE_SNAPSHOT", FAMILY_SNAPSHOT),
    ProcessingModeDescriptor("process_rates", "PROCESSING_MODE_RATES", FAMILY_RATE),
)

MODES_BY_NAME: Dict[str, ProcessingModeDescriptor] = {mode.name: mode for mode in PROCESSING_MODES}
FAMILIES_BY_KEY: Dict[str, CallbackFamily] = {family.key: family for family in CALLBACK_FAMILIES}

# Standalone fallback modules (models/<model>/modules/<name>.c with no metadata)
# advertise exactly the original three FoF modes; they never gain later modes, so a
# standalone .c module cannot advertise process_snapshot or process_rates.
STANDALONE_FALLBACK_MODES: Tuple[str, ...] = (
    "process_full_halo",
    "process_per_event",
    "process_by_galaxy",
)

# ==============================================================================
# SNAPSHOT DISTRIBUTION
# ==============================================================================

SNAPSHOT_DISTRIBUTION_KEY = "snapshot_distribution"

# Ordered as enum SnapshotDistribution (src/core/module_interface.h).
SNAPSHOT_DISTRIBUTIONS: Dict[str, str] = {
    "serial_only": "SNAPSHOT_DISTRIBUTION_SERIAL_ONLY",
    "collective": "SNAPSHOT_DISTRIBUTION_COLLECTIVE",
}

DEFAULT_SNAPSHOT_DISTRIBUTION = "serial_only"

# ==============================================================================
# HELPERS
# ==============================================================================


def mode_list_errors(modes: Any) -> List[str]:
    """Return every reason a supported_processing_modes value is invalid.

    The generator and validator both report these messages, so the two agree on
    which mode lists are accepted.

    Args:
        modes: The raw supported_processing_modes value from module metadata.

    Returns:
        Error messages; empty when the list is valid.
    """
    if not isinstance(modes, list) or len(modes) == 0:
        return ["'supported_processing_modes' must be a non-empty list"]

    errors: List[str] = []
    non_strings = [mode for mode in modes if not isinstance(mode, str)]
    if non_strings:
        errors.append(f"'supported_processing_modes' entries must be strings: {non_strings}")

    unknown = [mode for mode in modes if isinstance(mode, str) and mode not in MODES_BY_NAME]
    if unknown:
        errors.append(
            f"Invalid processing mode(s) {unknown} in 'supported_processing_modes' "
            f"(expected subset of {sorted(MODES_BY_NAME)})"
        )

    hashable = [mode for mode in modes if isinstance(mode, str)]
    if len(set(hashable)) != len(hashable):
        errors.append(f"Duplicate entries in 'supported_processing_modes': {modes}")

    return errors


def enum_for_mode(mode: str) -> str:
    """Return the C enumerator for a mode name; raises KeyError for an unknown mode."""
    return MODES_BY_NAME[mode].enum


def callback_families(modes: List[str]) -> List[CallbackFamily]:
    """Return the callback families a mode list advertises, in struct Module order.

    Raises:
        KeyError: if any mode is unknown (fail closed; validate with
            mode_list_errors() first).
    """
    keys = {MODES_BY_NAME[mode].family for mode in modes}
    return [family for family in CALLBACK_FAMILIES if family.key in keys]


def snapshot_distribution_errors(module: Dict[str, Any]) -> List[str]:
    """Return every reason a module's snapshot_distribution value is invalid.

    The key is optional. When present it must be one of SNAPSHOT_DISTRIBUTIONS,
    the module's supported_processing_modes must include process_snapshot, and
    the module must not be a utility (is_utility true): utility modules never
    reach the module registry, so the key would be silently dropped even where
    their metadata lists process_snapshot. The generator and validator both
    report these messages, so the two agree on which metadata is accepted.

    Args:
        module: The module metadata dict (module_info.yaml contents).

    Returns:
        Error messages; empty when the key is absent or valid.
    """
    if SNAPSHOT_DISTRIBUTION_KEY not in module:
        return []

    errors: List[str] = []
    if module.get("is_utility", False):
        errors.append(
            f"'{SNAPSHOT_DISTRIBUTION_KEY}' is not allowed on a utility module: utility "
            f"modules run no process_snapshot callback and may not declare it"
        )

    modes = module.get("supported_processing_modes")
    if not isinstance(modes, list) or "process_snapshot" not in modes:
        errors.append(
            f"'{SNAPSHOT_DISTRIBUTION_KEY}' is only valid for modules whose "
            f"'supported_processing_modes' include process_snapshot"
        )

    value = module[SNAPSHOT_DISTRIBUTION_KEY]
    if not isinstance(value, str) or value not in SNAPSHOT_DISTRIBUTIONS:
        errors.append(
            f"Invalid '{SNAPSHOT_DISTRIBUTION_KEY}' value {value!r} "
            f"(expected one of {sorted(SNAPSHOT_DISTRIBUTIONS)})"
        )

    return errors


def snapshot_distribution_enum(module: Dict[str, Any]) -> str:
    """Return the C enumerator of a module's snapshot_distribution.

    The default applies when the key is absent. Raises KeyError for an invalid
    value (fail closed; validate with snapshot_distribution_errors() first).
    """
    value = module.get(SNAPSHOT_DISTRIBUTION_KEY, DEFAULT_SNAPSHOT_DISTRIBUTION)
    if not isinstance(value, str) or value not in SNAPSHOT_DISTRIBUTIONS:
        raise KeyError(f"invalid {SNAPSHOT_DISTRIBUTION_KEY} value {value!r}")
    return SNAPSHOT_DISTRIBUTIONS[value]


# ==============================================================================
# RATE TRANSFERS
# ==============================================================================

RATES_KEY = "rates"
RATES_MODE = "process_rates"

# Keys of the rates: mapping, of one transfer and of one observer.
RATES_KEYS: Tuple[str, ...] = ("reads_central", "transfers", "observers")
TRANSFER_KEYS: Tuple[str, ...] = ("name", "from", "to", "to_record", "slaved_to")
OBSERVER_KEYS: Tuple[str, ...] = ("property", "transfer")

# The end of a transfer that is no record's reservoir: a source's donor, a sink's recipient.
EXTERNAL_END = "external"

# Ordered as enum RateTransferRecord (src/core/module_interface.h).
TRANSFER_RECORDS: Dict[str, str] = {
    "self": "RATE_TRANSFER_TO_SELF",
    "central": "RATE_TRANSFER_TO_CENTRAL",
}
DEFAULT_TRANSFER_RECORD = "self"

# A transfer's kind follows from its ends; ordered as enum RateTransferKind.
TRANSFER_KIND_INTERNAL = "internal"
TRANSFER_KIND_SINK = "sink"
TRANSFER_KIND_SOURCE = "source"
TRANSFER_KIND_SLAVED_SOURCE = "slaved_source"
TRANSFER_KINDS: Dict[str, str] = {
    TRANSFER_KIND_INTERNAL: "RATE_TRANSFER_INTERNAL",
    TRANSFER_KIND_SINK: "RATE_TRANSFER_SINK",
    TRANSFER_KIND_SOURCE: "RATE_TRANSFER_SOURCE",
    TRANSFER_KIND_SLAVED_SOURCE: "RATE_TRANSFER_SLAVED_SOURCE",
}

# A transfer name is a C identifier; slaved_to names a master as <module>.<transfer>.
TRANSFER_NAME = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")
SLAVED_TO = re.compile(r"([A-Za-z_][A-Za-z0-9_]*)\.([A-Za-z_][A-Za-z0-9_]*)")

# ------------------------------------------------------------------------------
# Generated names. The generator emits these per rate module, and the rules below
# refuse metadata whose generated names would collide, so a collision is reported
# at validation instead of as a C redefinition.
# ------------------------------------------------------------------------------


def rate_transfer_enum_type(module_name: str) -> str:
    """Return the C enum tag of a rate module's coefficient slots (PascalCase + Transfer)."""
    return "".join(part.capitalize() for part in module_name.split("_")) + "Transfer"


def rate_coefficient_constant(module_name: str, transfer_name: str) -> str:
    """Return the coefficient index constant of one transfer (the slot its law writes)."""
    return f"{module_name.upper()}_TRANSFER_{transfer_name.upper()}"


def rate_transfer_count_macro(module_name: str) -> str:
    """Return the macro holding a rate module's number of transfers (its coefficient slots)."""
    return f"{module_name.upper()}_NUM_TRANSFERS"


# Core C tags of the generated enum-tag form <Pascal>Transfer (struct, enum and union
# tags share one C namespace), and core identifiers of the generated constant forms
# <MODULE>_TRANSFER_<NAME> and <MODULE>_NUM_TRANSFERS, from module_interface.h and
# rate_contracts.h. tests/integration/test_rate_metadata.py checks these lists
# against the sources.
RESERVED_RATE_TAGS: Tuple[str, ...] = ("RateTransfer",)
RESERVED_RATE_IDENTIFIERS: Tuple[str, ...] = (
    "MIMIC_RATE_TRANSFER_COUNT",
    "RATE_TRANSFER_EXTERNAL",
    *TRANSFER_KINDS.values(),
    *TRANSFER_RECORDS.values(),
)


def event_identifiers(module: Dict[str, Any]) -> List[str]:
    """Return the event macros and constants the generator emits for a producer module.

    These mirror module_name_to_id_macro() and event_name_to_enum_constant() in
    scripts/generate_module_registry.py (tests/integration/test_rate_metadata.py
    checks that they agree), so a rate constant that would redefine one is refused.
    """
    events = module.get("events")
    emits = events.get("emits") if isinstance(events, dict) else None
    name = module.get("name")
    if not isinstance(emits, list) or not emits or not isinstance(name, str):
        return []
    identifiers = [f"MODULE_ID_{name.upper()}"]
    for event in emits:
        if isinstance(event, dict) and isinstance(event.get("name"), str):
            identifiers.append(f"{name.upper()}_EVENT_{event['name'].upper()}")
    return identifiers


@dataclass(frozen=True)
class RateMetadataError:
    """One refusal of a module's rates: declaration.

    Attributes:
        module: Name of the module the refusal is reported against.
        message: The reason, naming the transfer (or the transfers) concerned.
        dependency: True when the refusal is an unresolved reference to a
            property or another module's transfer, rather than malformed metadata.
    """

    module: str
    message: str
    dependency: bool = False


def is_rate_module(module: Dict[str, Any]) -> bool:
    """Return True for a runtime module whose rates: is a mapping.

    Generation selects these; call it on metadata that passed
    rate_metadata_errors(), where every such module also advertises
    process_rates.
    """
    return not module.get("is_utility", False) and isinstance(module.get(RATES_KEY), dict)


def rate_transfers(module: Dict[str, Any]) -> List[Dict[str, Any]]:
    """Return a rate module's transfers in declaration order (the coefficient slots)."""
    return list(module[RATES_KEY].get("transfers") or [])


def rate_observers(module: Dict[str, Any]) -> List[Dict[str, Any]]:
    """Return a rate module's observers in declaration order."""
    return list(module[RATES_KEY].get("observers") or [])


def rate_reads_central(module: Dict[str, Any]) -> bool:
    """Return a rate module's reads_central flag (false when the key is omitted)."""
    return module[RATES_KEY].get("reads_central", False) is True


def transfer_kind(transfer: Dict[str, Any]) -> str:
    """Return a validated transfer's kind (a key of TRANSFER_KINDS), decided by its ends."""
    if transfer["from"] == EXTERNAL_END:
        return TRANSFER_KIND_SLAVED_SOURCE if "slaved_to" in transfer else TRANSFER_KIND_SOURCE
    if transfer["to"] == EXTERNAL_END:
        return TRANSFER_KIND_SINK
    return TRANSFER_KIND_INTERNAL


def global_transfer_index(modules: List[Dict[str, Any]]) -> List[Tuple[str, str]]:
    """Return every compiled rate module's transfers in the canonical global order.

    The order is (module name, declaration order): rate modules sorted by name,
    each module's transfers in the order its rates.transfers lists them. It
    depends on neither discovery order nor run-file order, so a transfer's
    position is its global index. Call it on metadata that passed
    rate_metadata_errors().

    Returns:
        (module name, transfer name) pairs; a pair's position is its global index.
    """
    order: List[Tuple[str, str]] = []
    for module in sorted((m for m in modules if is_rate_module(m)), key=lambda m: m["name"]):
        for transfer in rate_transfers(module):
            order.append((module["name"], transfer["name"]))
    return order


def _reservoir_mapping(
    galaxy_properties: Dict[str, Dict[str, Any]], name: Any
) -> Optional[Dict[str, Any]]:
    """Return a property's reservoir: mapping, or None when it declares none."""
    if not isinstance(name, str):
        return None
    prop = galaxy_properties.get(name)
    if not isinstance(prop, dict):
        return None
    mapping = prop.get("reservoir")
    return mapping if isinstance(mapping, dict) else None


def _total_of(galaxy_properties: Dict[str, Dict[str, Any]], name: Any) -> Optional[str]:
    """Return the conserved total a reservoir or tracer names, or None when it names none."""
    mapping = _reservoir_mapping(galaxy_properties, name)
    total = mapping.get("total") if mapping else None
    return total if isinstance(total, str) and total else None


def _describe_end(galaxy_properties: Dict[str, Dict[str, Any]], name: str) -> str:
    """Describe what a named transfer end is, for a refusal message."""
    if name not in galaxy_properties:
        return f"'{name}', which is not a declared galaxy property"
    mapping = _reservoir_mapping(galaxy_properties, name)
    if mapping is None:
        return f"'{name}', a property that declares no reservoir mapping"
    return f"'{name}', a property of reservoir kind '{mapping.get('kind')}'"


@dataclass
class _RateScan:
    """What the per-module rules collect for the cross-module rules.

    Attributes:
        galaxy_properties: Galaxy property metadata by name.
        errors: Refusals found so far.
        declared: Every transfer with a valid, unique name, by (module, transfer).
        resolved: Those transfers whose ends are valid.
        slaved: Slaved sources with valid ends, as (module, transfer mapping).
        names: Each rate module's uniquely named transfers, in declaration order.
        refused: Transfers refused as case-only duplicates, by (module, transfer); a
            slaved_to or observer naming one is not refused again.
    """

    galaxy_properties: Dict[str, Dict[str, Any]]
    errors: List[RateMetadataError] = field(default_factory=list)
    declared: Dict[Tuple[str, str], Dict[str, Any]] = field(default_factory=dict)
    resolved: Set[Tuple[str, str]] = field(default_factory=set)
    slaved: List[Tuple[str, Dict[str, Any]]] = field(default_factory=list)
    names: Dict[str, List[str]] = field(default_factory=dict)
    refused: Set[Tuple[str, str]] = field(default_factory=set)

    def refuse(self, module: str, message: str, dependency: bool = False) -> None:
        """Record one refusal against a module."""
        self.errors.append(RateMetadataError(module, message, dependency))


def _end_kind_errors(
    source: str, target: str, label: str, galaxy_properties: Dict[str, Dict[str, Any]]
) -> List[Tuple[str, bool]]:
    """Return the refusals of a transfer's ends by reservoir kind, as (message, dependency)."""
    errors: List[Tuple[str, bool]] = []
    if source != EXTERNAL_END:
        mapping = _reservoir_mapping(galaxy_properties, source)
        if mapping is None or mapping.get("kind") != "reservoir":
            errors.append(
                (
                    f"{label} takes from {_describe_end(galaxy_properties, source)}; a transfer's "
                    "from is a property of reservoir kind 'reservoir' or 'external' (never a "
                    "tracer or an observer)",
                    source not in galaxy_properties,
                )
            )
    if target == EXTERNAL_END:
        return errors
    mapping = _reservoir_mapping(galaxy_properties, target)
    kind = mapping.get("kind") if mapping else None
    if kind == "tracer" and source != EXTERNAL_END:
        errors.append(
            (
                f"{label} moves from '{source}' into the tracer '{target}'; a tracer may be a "
                "transfer's to only when its from is 'external' (tracers otherwise move with "
                "their parent without declaration)",
                False,
            )
        )
    elif kind not in ("reservoir", "tracer"):
        errors.append(
            (
                f"{label} moves into {_describe_end(galaxy_properties, target)}; a transfer's "
                "to is a property of reservoir kind 'reservoir', 'external', or a tracer when "
                "the from is 'external'",
                target not in galaxy_properties,
            )
        )
    return errors


def _end_total_errors(
    source: str, target: str, label: str, galaxy_properties: Dict[str, Dict[str, Any]]
) -> List[Tuple[str, bool]]:
    """Return the refusals of a transfer's totals, for ends whose kinds are valid.

    Every reservoir or tracer end names a total, and an internal transfer's two
    reservoirs share one.
    """
    errors: List[Tuple[str, bool]] = []
    for end in (source, target):
        if end != EXTERNAL_END and _total_of(galaxy_properties, end) is None:
            errors.append(
                (
                    f"{label} names '{end}', whose reservoir: mapping declares no total; every "
                    "reservoir and tracer belongs to a conserved total",
                    False,
                )
            )
    if errors or EXTERNAL_END in (source, target):
        return errors

    source_total = _total_of(galaxy_properties, source)
    target_total = _total_of(galaxy_properties, target)
    if source_total != target_total:
        errors.append(
            (
                f"{label} moves from '{source}' in total '{source_total}' to '{target}' in total "
                f"'{target_total}'; an internal transfer stays within one conserved total "
                "(movement between totals is a sink in one and a slaved source in the other)",
                False,
            )
        )
    return errors


def _end_errors(
    transfer: Dict[str, Any], label: str, galaxy_properties: Dict[str, Dict[str, Any]]
) -> List[Tuple[str, bool]]:
    """Return the refusals of one transfer's from and to, as (message, dependency) pairs.

    An empty list means both ends are valid, each reservoir or tracer end names a
    total, and an internal transfer's two reservoirs share one conserved total.
    """
    errors: List[Tuple[str, bool]] = []
    ends = {key: transfer.get(key) for key in ("from", "to")}
    for key, value in ends.items():
        if not isinstance(value, str) or not value:
            errors.append(
                (f"{label} needs a non-empty string {key}: naming a reservoir or 'external'", False)
            )
    if errors:
        return errors

    source, target = ends["from"], ends["to"]
    if source == EXTERNAL_END and target == EXTERNAL_END:
        return [
            (f"{label} has both ends 'external'; a transfer moves into or out of a record", False)
        ]
    return _end_kind_errors(source, target, label, galaxy_properties) or _end_total_errors(
        source, target, label, galaxy_properties
    )


def _rates_header(module: Dict[str, Any], name: str, scan: _RateScan) -> Optional[Dict[str, Any]]:
    """Check that rates: and process_rates come together; return rates: to check further.

    Returns None when the module declares no rates: or is refused here, in which
    case its transfers take no part in the cross-module rules.
    """
    modes = module.get("supported_processing_modes")
    utility = module.get("is_utility", False)
    advertises = not utility and isinstance(modes, list) and RATES_MODE in modes

    if RATES_KEY not in module:
        if advertises:
            scan.refuse(
                name,
                f"advertises {RATES_MODE} but declares no {RATES_KEY}:; a rate module declares "
                "the transfers its law returns coefficients for",
            )
        return None

    rates = module[RATES_KEY]
    raw = rates.get("transfers") if isinstance(rates, dict) else None
    named = (
        [t.get("name") for t in raw if isinstance(t, dict) and "name" in t]
        if isinstance(raw, list)
        else []
    )
    if utility:
        scan.refuse(
            name,
            f"declares {RATES_KEY}: (transfers {named}) on a utility module; utility modules "
            "compile no runtime module and run no rate law",
        )
        return None
    if not advertises:
        scan.refuse(
            name,
            f"declares {RATES_KEY}: (transfers {named}) but its supported_processing_modes do not "
            f"include {RATES_MODE}",
        )
        return None
    if not isinstance(rates, dict):
        scan.refuse(name, f"{RATES_KEY}: must be a mapping with keys {list(RATES_KEYS)}")
        return None

    unknown = [key for key in rates if key not in RATES_KEYS]
    if unknown:
        scan.refuse(
            name,
            f"has unknown {RATES_KEY}: key(s) {unknown} (transfers {named}); allowed keys are "
            f"{list(RATES_KEYS)}",
        )
    if "reads_central" in rates and not isinstance(rates["reads_central"], bool):
        scan.refuse(
            name,
            f"{RATES_KEY}.reads_central is {rates['reads_central']!r} (transfers {named}); "
            "it must be true or false (default false)",
        )
    return rates


def _claim_transfer_name(
    name: str, label: str, transfer: Dict[str, Any], by_constant: Dict[str, str], scan: _RateScan
) -> bool:
    """Record a validly named transfer unless its name, or its generated constant, is taken.

    ``by_constant`` maps each coefficient constant already generated in the module to
    the transfer that took it, so a name differing from another only in case, which
    generates the same constant, is refused. Returns True when the transfer was recorded.
    """
    transfer_name = transfer["name"]
    constant = rate_coefficient_constant(name, transfer_name)
    first = by_constant.get(constant)
    if first == transfer_name:
        scan.refuse(name, f"{label} is declared twice; a transfer name is unique within its module")
        return False
    if first is not None:
        scan.refuse(
            name,
            f"{label} and {RATES_KEY}.transfers '{first}' differ only in case and would both "
            f"generate the coefficient constant {constant}; transfer names in a module must "
            "differ other than in case",
        )
        scan.refused.add((name, transfer_name))
        return False
    by_constant[constant] = transfer_name
    scan.declared[(name, transfer_name)] = transfer
    scan.names.setdefault(name, []).append(transfer_name)
    return True


def _record_errors(transfer: Dict[str, Any], label: str) -> List[str]:
    """Return the refusals of a transfer's to_record."""
    record = transfer.get("to_record", DEFAULT_TRANSFER_RECORD)
    if not isinstance(record, str) or record not in TRANSFER_RECORDS:
        return [
            f"{label} has to_record {record!r}; it must be one of {list(TRANSFER_RECORDS)} "
            f"(default {DEFAULT_TRANSFER_RECORD})"
        ]
    if record == "central" and transfer.get("to") == EXTERNAL_END:
        return [
            f"{label} has to_record: central with to: external; only a transfer into a "
            "reservoir or tracer has a recipient record"
        ]
    return []


def _slaved_to_errors(transfer: Dict[str, Any], label: str) -> List[str]:
    """Return the refusals of a transfer's slaved_to that need no other module."""
    if "slaved_to" not in transfer:
        return []
    target = transfer["slaved_to"]
    if not isinstance(target, str) or not SLAVED_TO.fullmatch(target):
        return [
            f"{label} has slaved_to {target!r}; it names its master transfer as "
            "<module>.<transfer>"
        ]
    if transfer.get("from") != EXTERNAL_END:
        return [
            f"{label} has slaved_to but takes from {transfer.get('from')!r}; only a source "
            "(from: external) is slaved to a master transfer"
        ]
    return []


def _transfer_errors(
    name: str,
    position: int,
    transfer: Any,
    by_constant: Dict[str, str],
    scan: _RateScan,
) -> None:
    """Check one transfer of a rate module, recording it for the cross-module rules."""
    label = f"{RATES_KEY}.transfers[{position}]"
    if not isinstance(transfer, dict):
        scan.refuse(name, f"{label} must be a mapping with keys {list(TRANSFER_KEYS)}")
        return
    transfer_name = transfer.get("name")
    valid_name = isinstance(transfer_name, str) and bool(TRANSFER_NAME.fullmatch(transfer_name))
    if valid_name:
        label = f"{RATES_KEY}.transfers '{transfer_name}'"
    else:
        scan.refuse(name, f"{label} has name {transfer_name!r}; a transfer name is a C identifier")

    unknown = [key for key in transfer if key not in TRANSFER_KEYS]
    if unknown:
        scan.refuse(
            name, f"{label} has unknown key(s) {unknown}; allowed keys are {list(TRANSFER_KEYS)}"
        )
    if valid_name and not _claim_transfer_name(name, label, transfer, by_constant, scan):
        return

    end_errors = _end_errors(transfer, label, scan.galaxy_properties)
    for message, dependency in end_errors:
        scan.refuse(name, message, dependency)
    slaved_errors = _slaved_to_errors(transfer, label)
    for message in _record_errors(transfer, label) + slaved_errors:
        scan.refuse(name, message)

    if valid_name and not end_errors:
        scan.resolved.add((name, transfer_name))
        if "slaved_to" in transfer and not slaved_errors:
            scan.slaved.append((name, transfer))


def _observer_errors(name: str, observers: Any, scan: _RateScan) -> None:
    """Check a rate module's observers against its own transfers and the property kinds."""
    if not isinstance(observers, list):
        scan.refuse(
            name, f"{RATES_KEY}.observers must be a list of {{property, transfer}} mappings"
        )
        return
    declared = scan.names.get(name, [])
    for position, observer in enumerate(observers):
        label = f"{RATES_KEY}.observers[{position}]"
        if not isinstance(observer, dict):
            scan.refuse(name, f"{label} must be a mapping with keys {list(OBSERVER_KEYS)}")
            continue
        observed = observer.get("transfer")
        prop = observer.get("property")
        label = f"{label} (transfer {observed!r}, property {prop!r})"
        unknown = [key for key in observer if key not in OBSERVER_KEYS]
        if unknown:
            scan.refuse(
                name,
                f"{label} has unknown key(s) {unknown}; allowed keys are {list(OBSERVER_KEYS)}",
            )
        if isinstance(observed, str) and (name, observed) in scan.refused:
            pass  # The transfer's own refusal names its defect.
        elif not isinstance(observed, str) or observed not in declared:
            scan.refuse(
                name,
                f"{label} observes a transfer this module does not declare (declared: "
                f"{sorted(declared)})",
                True,
            )
        mapping = _reservoir_mapping(scan.galaxy_properties, prop)
        if mapping is None or mapping.get("kind") != "observer":
            described = (
                _describe_end(scan.galaxy_properties, prop) if isinstance(prop, str) else repr(prop)
            )
            scan.refuse(
                name,
                f"{label} accumulates into {described}; an observer's property is of reservoir "
                "kind 'observer'",
                isinstance(prop, str) and prop not in scan.galaxy_properties,
            )


UNNAMED_MODULE = "<unnamed>"


def rate_error_module(module: Dict[str, Any]) -> str:
    """Return the module name rate refusals are reported against.

    A module whose metadata has no string name is reported as UNNAMED_MODULE; the
    validator filters single-module reports by this same name.
    """
    name = module.get("name")
    return name if isinstance(name, str) and name else UNNAMED_MODULE


def _module_rate_errors(module: Dict[str, Any], scan: _RateScan) -> None:
    """Check the rules decidable from one module's own metadata."""
    name = rate_error_module(module)
    rates = _rates_header(module, name, scan)
    if rates is None:
        return
    scan.names.setdefault(name, [])

    raw = rates.get("transfers")
    transfers = raw if isinstance(raw, list) else []
    if not transfers:
        scan.refuse(name, f"{RATES_KEY}.transfers must be a non-empty list of transfers")
    by_constant: Dict[str, str] = {}
    for position, transfer in enumerate(transfers):
        _transfer_errors(name, position, transfer, by_constant, scan)

    _observer_errors(name, rates.get("observers", []), scan)


def _slaved_master_edges(
    scan: _RateScan,
) -> Tuple[Dict[str, Set[str]], Dict[Tuple[str, str], List[Tuple[str, str]]]]:
    """Resolve each slaved source's master; return the total dependencies they create.

    Returns:
        (edges, via): donor total -> recipient totals, and for each dependency the
        (module, description) of the slaved sources that create it.
    """
    edges: Dict[str, Set[str]] = {}
    via: Dict[Tuple[str, str], List[Tuple[str, str]]] = {}
    gp = scan.galaxy_properties
    for module_name, transfer in scan.slaved:
        label = f"{RATES_KEY}.transfers '{transfer['name']}'"
        target = transfer["slaved_to"]
        master_key = tuple(SLAVED_TO.fullmatch(target).groups())
        master = scan.declared.get(master_key)
        if master_key in scan.refused:
            continue  # The master's own refusal names its defect.
        if master is None:
            scan.refuse(
                module_name,
                f"{label} is slaved to '{target}', which names no transfer declared by a rate "
                "module",
                True,
            )
            continue
        if master_key not in scan.resolved:
            continue  # The master's own refusal names its defect.
        if master["from"] == EXTERNAL_END:
            scan.refuse(
                module_name,
                f"{label} is slaved to '{target}', a source; a master is a transfer out of a "
                "reservoir (slaved sources are one level deep)",
            )
            continue
        donor_total = _total_of(gp, master["from"])
        recipient_total = _total_of(gp, transfer["to"])
        if donor_total == recipient_total:
            scan.refuse(
                module_name,
                f"{label} is slaved to '{target}', whose donor '{master['from']}' is in total "
                f"'{donor_total}', the recipient's total; a slaved source feeds a total other "
                "than its master's donor's",
            )
            continue
        edges.setdefault(donor_total, set()).add(recipient_total)
        via.setdefault((donor_total, recipient_total), []).append(
            (module_name, f"{module_name} {label} (slaved to {target})")
        )
    return edges, via


def _add_tracer_edges(
    galaxy_properties: Dict[str, Dict[str, Any]], edges: Dict[str, Set[str]]
) -> None:
    """Add each tracer total's dependency on its tracers' parents' totals."""
    for name in galaxy_properties:
        mapping = _reservoir_mapping(galaxy_properties, name)
        if mapping is None or mapping.get("kind") != "tracer":
            continue
        parent_total = _total_of(galaxy_properties, mapping.get("parent"))
        tracer_total = _total_of(galaxy_properties, name)
        if parent_total and tracer_total:
            edges.setdefault(parent_total, set()).add(tracer_total)


def _cyclic_components(nodes: List[str], edges: Dict[str, Set[str]]) -> List[List[str]]:
    """Return each set of nodes that lie on a common cycle, sorted, in sorted order."""

    def reachable(start: str) -> Set[str]:
        found: Set[str] = set()
        stack = sorted(edges.get(start, ()))
        while stack:
            node = stack.pop()
            if node not in found:
                found.add(node)
                stack.extend(sorted(edges.get(node, ())))
        return found

    reach = {node: reachable(node) for node in nodes}
    components: List[List[str]] = []
    placed: Set[str] = set()
    for node in sorted(nodes):
        if node in placed or node not in reach[node]:
            continue
        component = sorted(
            other for other in nodes if other in reach[node] and node in reach[other]
        )
        placed.update(component)
        components.append(component)
    return components


def _cycle_errors(
    edges: Dict[str, Set[str]],
    via: Dict[Tuple[str, str], List[Tuple[str, str]]],
    scan: _RateScan,
) -> None:
    """Refuse each cycle among totals, against every module whose slaved source is on it.

    Every participating module gets the refusal, so validating any one of them
    alone reports the cycle.
    """
    nodes = sorted(set(edges) | {total for targets in edges.values() for total in targets})
    for component in _cyclic_components(nodes, edges):
        members = set(component)
        links = sorted(
            link
            for (donor, recipient), entries in via.items()
            if donor in members and recipient in members
            for link in entries
        )
        if not links:
            continue
        message = (
            f"{RATES_KEY}: conserved totals {component} depend on each other in a cycle through "
            f"the slaved sources {'; '.join(text for _, text in links)}; a slaved source makes "
            "its recipient's total depend on its master donor's total, and those dependencies "
            "must not form a cycle"
        )
        for module_name in sorted({module_name for module_name, _ in links}):
            scan.refuse(module_name, message)


# A claim on a generated name: (C namespace, name) -> [(rate module or None, description)].
# Ordinary identifiers ("id") and struct/enum/union tags ("tag") live in separate namespaces.
_NameClaims = Dict[Tuple[str, str], List[Tuple[Optional[str], str]]]


# A property name that no metadata uses, rendered through the reservoir-table generator to
# learn the form of the index symbol it emits for a property.
_RESERVOIR_NAME_PROBE = "MimicRateNameProbe"


def _reservoir_table_names(
    galaxy_properties: Dict[str, Dict[str, Any]],
) -> Tuple[Dict[str, str], Set[str]]:
    """Return the identifiers and tags reservoir_table.h declares, which module_init.c includes.

    The forms come from scripts/generate_properties.py itself: a one-entry table is
    rendered for a probe property, its index symbol gives the per-property form,
    and its other macros, enum constants and tags are the table's fixed names. Every
    galaxy property with a reservoir: mapping then claims its index symbol.

    Returns:
        (identifier -> description, tags).
    """
    from generate_properties import generate_reservoir_table_h  # Owner of these names.

    text = generate_reservoir_table_h(
        [{"name": _RESERVOIR_NAME_PROBE, "reservoir": {"kind": "observer"}}], ""
    )
    defined = set(re.findall(r"^#define\s+([A-Za-z_][A-Za-z0-9_]*)", text, re.M))
    defined |= set(re.findall(r"^\s+([A-Za-z_][A-Za-z0-9_]*)\s*=", text, re.M))
    tags = set(re.findall(r"\b(?:enum|struct|union)\s+([A-Za-z_][A-Za-z0-9_]*)\s*\{", text))

    identifiers: Dict[str, str] = {}
    for identifier in sorted(defined):
        if _RESERVOIR_NAME_PROBE in identifier:
            template = identifier.replace(_RESERVOIR_NAME_PROBE, "{}")
            for name in sorted(galaxy_properties):
                if _reservoir_mapping(galaxy_properties, name) is not None:
                    identifiers[template.format(name)] = f"the reservoir index of '{name}'"
        else:
            identifiers[identifier] = f"the reservoir-table identifier {identifier}"
    return identifiers, tags


def _generated_name_claims(modules: List[Dict[str, Any]], scan: _RateScan) -> _NameClaims:
    """Return every name the generator would emit per rate module, plus the names they must avoid.

    Rate modules claim their enum tag, count macro and coefficient constants; the
    core's reserved names, every module's event identifiers and the names
    reservoir_table.h declares (module_init.c includes it) are claimed by no rate
    module. Transfers already refused as case-only duplicates are not claimed.
    """
    claims: _NameClaims = {}

    def claim(space: str, identifier: str, module: Optional[str], what: str) -> None:
        claims.setdefault((space, identifier), []).append((module, what))

    for name, transfer_names in sorted(scan.names.items()):
        listed = f"{name} (transfers {transfer_names})"
        claim("tag", rate_transfer_enum_type(name), name, f"the coefficient enum of {listed}")
        claim("id", rate_transfer_count_macro(name), name, f"the transfer count of {listed}")
        for transfer_name in transfer_names:
            claim(
                "id",
                rate_coefficient_constant(name, transfer_name),
                name,
                f"{name} {RATES_KEY}.transfers '{transfer_name}'",
            )
    for tag in RESERVED_RATE_TAGS:
        claim("tag", tag, None, f"the core tag {tag}")
    for identifier in RESERVED_RATE_IDENTIFIERS:
        claim("id", identifier, None, f"the core identifier {identifier}")
    for module in modules:
        for identifier in event_identifiers(module):
            claim("id", identifier, None, f"an event identifier of {module.get('name')}")
    reservoir_identifiers, reservoir_tags = _reservoir_table_names(scan.galaxy_properties)
    for identifier, what in reservoir_identifiers.items():
        claim("id", identifier, None, what)
    for tag in sorted(reservoir_tags):
        claim("tag", tag, None, f"the reservoir-table tag {tag}")
    return claims


def _identifier_collision_errors(modules: List[Dict[str, Any]], scan: _RateScan) -> None:
    """Refuse rate modules whose generated names collide with any other generated or core name.

    Checks, over every rate module, the coefficient constants, the count macros and
    the enum tags the generator emits, against each other, against the core's own
    names of those forms, against the event identifiers and against the names
    reservoir_table.h declares. Each rate module
    involved gets one refusal listing its colliding names, so validating any one of
    them alone reports the collision.
    """
    clashes: Dict[str, List[str]] = {}
    for (space, identifier), claimants in sorted(_generated_name_claims(modules, scan).items()):
        if len(claimants) < 2:
            continue
        described = " and ".join(what for _, what in claimants)
        kind = "enum tag" if space == "tag" else "identifier"
        for module_name in sorted({module for module, _ in claimants if module is not None}):
            clashes.setdefault(module_name, []).append(f"{kind} {identifier} ({described})")

    for module_name, entries in sorted(clashes.items()):
        scan.refuse(
            module_name,
            f"{RATES_KEY}: generated names would collide: {'; '.join(entries)}; rename a module "
            "or transfer so every generated name is distinct",
        )


def rate_metadata_errors(
    modules: List[Dict[str, Any]], galaxy_properties: Dict[str, Dict[str, Any]]
) -> List[RateMetadataError]:
    """Return every refusal of the rates: declarations of a set of modules.

    The registry generator and the metadata validator both report these, so the
    two accept the same metadata. Every module's own rules are checked first; the
    cross-module rules (a slaved source's master, the order of the conserved
    totals and collisions of generated names) then run over every module passed,
    which for generation is every compiled module.

    Rules: rates: needs process_rates and the reverse; rates: has no unknown key,
    reads_central is a boolean, transfers is a non-empty list; a transfer has no
    unknown key and a C-identifier name unique within its module, also when
    upper-cased; its from is a reservoir or external and its to a reservoir,
    external, or a tracer when the from is external, never both external; every
    reservoir or tracer end names a total, and an internal transfer's reservoirs
    share one; to_record is self or central, never central with to: external;
    slaved_to appears only on a source and names, as <module>.<transfer>, a transfer
    out of a reservoir whose total differs from the recipient's; an observer names
    a property of kind observer and a transfer of its own module. A slaved source
    makes its recipient's total depend on its master donor's total, and a tracer
    total on its tracers' parents' totals; a cycle among totals is refused against
    every module whose slaved source is on it, naming the totals. Finally no
    generated name may collide with another, with a core name of the same form,
    with an event identifier or with a name reservoir_table.h declares.

    Args:
        modules: Module metadata dicts (module_info.yaml contents).
        galaxy_properties: Galaxy property metadata by name, each as its property
            YAML declares it (the ``reservoir:`` mapping is read from it). Both tools
            pass galaxy properties only.

    Returns:
        The refusals, in module then rule order; empty when every declaration is valid.
    """
    scan = _RateScan(galaxy_properties)
    for module in modules:
        _module_rate_errors(module, scan)

    edges, via = _slaved_master_edges(scan)
    _add_tracer_edges(galaxy_properties, edges)
    _cycle_errors(edges, via, scan)
    _identifier_collision_errors(modules, scan)
    return scan.errors
