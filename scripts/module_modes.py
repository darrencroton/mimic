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
from dataclasses import dataclass
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
    galaxy_properties: Dict[str, Dict[str, Any]], name: str
) -> Optional[Dict[str, Any]]:
    """Return a property's reservoir: mapping, or None when it declares none."""
    prop = galaxy_properties.get(name)
    if not isinstance(prop, dict):
        return None
    mapping = prop.get("reservoir")
    return mapping if isinstance(mapping, dict) else None


def _describe_end(galaxy_properties: Dict[str, Dict[str, Any]], name: str) -> str:
    """Describe what a named transfer end is, for a refusal message."""
    if name not in galaxy_properties:
        return f"'{name}', which is not a declared property"
    mapping = _reservoir_mapping(galaxy_properties, name)
    if mapping is None:
        return f"'{name}', a property that declares no reservoir:"
    return f"'{name}', a property of reservoir kind '{mapping.get('kind')}'"


def _end_errors(
    transfer: Dict[str, Any], label: str, galaxy_properties: Dict[str, Dict[str, Any]]
) -> List[Tuple[str, bool]]:
    """Return the refusals of one transfer's from and to, as (message, dependency) pairs.

    An empty list means both ends are valid and, for an internal transfer, that
    its two reservoirs share one conserved total.
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
    if target != EXTERNAL_END:
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
    if errors or EXTERNAL_END in (source, target):
        return errors

    source_total = galaxy_properties[source]["reservoir"].get("total")
    target_total = galaxy_properties[target]["reservoir"].get("total")
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


def _module_rate_errors(
    module: Dict[str, Any],
    galaxy_properties: Dict[str, Dict[str, Any]],
    declared: Dict[Tuple[str, str], Dict[str, Any]],
    resolved: Set[Tuple[str, str]],
    slaved: List[Tuple[str, Dict[str, Any]]],
) -> List[RateMetadataError]:
    """Return the refusals decidable from one module's own metadata.

    Records, for the cross-module checks, every named transfer in ``declared``,
    those whose ends are valid in ``resolved``, and the slaved sources with valid
    ends in ``slaved``.
    """
    name = str(module.get("name", "<unnamed>"))
    errors: List[RateMetadataError] = []

    def refuse(message: str, dependency: bool = False) -> None:
        errors.append(RateMetadataError(name, message, dependency))

    modes = module.get("supported_processing_modes")
    utility = module.get("is_utility", False)
    advertises = not utility and isinstance(modes, list) and RATES_MODE in modes

    if RATES_KEY not in module:
        if advertises:
            refuse(
                f"advertises {RATES_MODE} but declares no {RATES_KEY}:; a rate module declares "
                "the transfers its law returns coefficients for"
            )
        return errors

    rates = module[RATES_KEY]
    raw_transfers = rates.get("transfers") if isinstance(rates, dict) else None
    named = (
        [t.get("name") for t in raw_transfers if isinstance(t, dict) and "name" in t]
        if isinstance(raw_transfers, list)
        else []
    )
    if utility:
        refuse(
            f"declares {RATES_KEY}: (transfers {named}) on a utility module; utility modules "
            "compile no runtime module and run no rate law"
        )
        return errors
    if not advertises:
        refuse(
            f"declares {RATES_KEY}: (transfers {named}) but its supported_processing_modes do not "
            f"include {RATES_MODE}"
        )
        return errors

    if not isinstance(rates, dict):
        refuse(f"{RATES_KEY}: must be a mapping with keys {list(RATES_KEYS)}")
        return errors
    unknown = [key for key in rates if key not in RATES_KEYS]
    if unknown:
        refuse(
            f"has unknown {RATES_KEY}: key(s) {unknown} (transfers {named}); allowed keys are "
            f"{list(RATES_KEYS)}"
        )
    if "reads_central" in rates and not isinstance(rates["reads_central"], bool):
        refuse(
            f"{RATES_KEY}.reads_central is {rates['reads_central']!r} (transfers {named}); "
            "it must be true or false (default false)"
        )

    transfers = raw_transfers if isinstance(raw_transfers, list) else []
    if not transfers:
        refuse(f"{RATES_KEY}.transfers must be a non-empty list of transfers")

    seen: Set[str] = set()
    for position, transfer in enumerate(transfers):
        label = f"{RATES_KEY}.transfers[{position}]"
        if not isinstance(transfer, dict):
            refuse(f"{label} must be a mapping with keys {list(TRANSFER_KEYS)}")
            continue
        transfer_name = transfer.get("name")
        valid_name = isinstance(transfer_name, str) and bool(TRANSFER_NAME.fullmatch(transfer_name))
        if valid_name:
            label = f"{RATES_KEY}.transfers '{transfer_name}'"
        else:
            refuse(f"{label} has name {transfer_name!r}; a transfer name is a C identifier")

        unknown = [key for key in transfer if key not in TRANSFER_KEYS]
        if unknown:
            refuse(f"{label} has unknown key(s) {unknown}; allowed keys are {list(TRANSFER_KEYS)}")

        if valid_name:
            if transfer_name in seen:
                refuse(f"{label} is declared twice; a transfer name is unique within its module")
                continue
            seen.add(transfer_name)
            declared[(name, transfer_name)] = transfer

        end_errors = _end_errors(transfer, label, galaxy_properties)
        for message, dependency in end_errors:
            refuse(message, dependency)
        ends_valid = not end_errors

        record = transfer.get("to_record", DEFAULT_TRANSFER_RECORD)
        if record not in TRANSFER_RECORDS:
            refuse(
                f"{label} has to_record {record!r}; it must be one of {list(TRANSFER_RECORDS)} "
                f"(default {DEFAULT_TRANSFER_RECORD})"
            )
        elif record == "central" and transfer.get("to") == EXTERNAL_END:
            refuse(
                f"{label} has to_record: central with to: external; only a transfer into a "
                "reservoir or tracer has a recipient record"
            )

        if "slaved_to" in transfer:
            target = transfer["slaved_to"]
            if not isinstance(target, str) or not SLAVED_TO.fullmatch(target):
                refuse(
                    f"{label} has slaved_to {target!r}; it names its master transfer as "
                    "<module>.<transfer>"
                )
            elif transfer.get("from") != EXTERNAL_END:
                refuse(
                    f"{label} has slaved_to but takes from {transfer.get('from')!r}; only a source "
                    "(from: external) is slaved to a master transfer"
                )
            elif ends_valid and valid_name:
                slaved.append((name, transfer))

        if ends_valid and valid_name:
            resolved.add((name, transfer_name))

    observers = rates.get("observers", [])
    if not isinstance(observers, list):
        refuse(f"{RATES_KEY}.observers must be a list of {{property, transfer}} mappings")
        observers = []
    for position, observer in enumerate(observers):
        label = f"{RATES_KEY}.observers[{position}]"
        if not isinstance(observer, dict):
            refuse(f"{label} must be a mapping with keys {list(OBSERVER_KEYS)}")
            continue
        observed = observer.get("transfer")
        prop = observer.get("property")
        label = f"{label} (transfer {observed!r}, property {prop!r})"
        unknown = [key for key in observer if key not in OBSERVER_KEYS]
        if unknown:
            refuse(f"{label} has unknown key(s) {unknown}; allowed keys are {list(OBSERVER_KEYS)}")
        if not isinstance(observed, str) or observed not in seen:
            refuse(
                f"{label} observes a transfer this module does not declare (declared: "
                f"{sorted(seen)})",
                True,
            )
        mapping = _reservoir_mapping(galaxy_properties, prop) if isinstance(prop, str) else None
        if mapping is None or mapping.get("kind") != "observer":
            described = (
                _describe_end(galaxy_properties, prop) if isinstance(prop, str) else repr(prop)
            )
            refuse(
                f"{label} accumulates into {described}; an observer's property is of reservoir "
                "kind 'observer'",
                isinstance(prop, str) and prop not in galaxy_properties,
            )

    return errors


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


def rate_metadata_errors(
    modules: List[Dict[str, Any]], galaxy_properties: Dict[str, Dict[str, Any]]
) -> List[RateMetadataError]:
    """Return every refusal of the rates: declarations of a set of modules.

    The registry generator and the metadata validator both report these, so the
    two accept the same metadata. Every module's own rules are checked first; the
    cross-module rules (a slaved source's master, and the order of the conserved
    totals) then run over every module passed, which for generation is every
    compiled module.

    Rules: rates: needs process_rates and the reverse; rates: has no unknown key,
    reads_central is a boolean, transfers is a non-empty list; a transfer has no
    unknown key and a C-identifier name unique within its module; its from is a
    reservoir or external and its to a reservoir, external, or a tracer when the
    from is external, never both external; an internal transfer's reservoirs share
    one total; to_record is self or central, never central with to: external;
    slaved_to appears only on a source and names, as <module>.<transfer>, a transfer
    out of a reservoir whose total differs from the recipient's; an observer names
    a property of kind observer and a transfer of its own module. Finally a
    slaved source makes its recipient's total depend on its master donor's total,
    and a tracer total on its tracers' parents' totals; a cycle among totals is
    refused, naming the totals and the slaved sources on it.

    Args:
        modules: Module metadata dicts (module_info.yaml contents).
        galaxy_properties: Galaxy property metadata by name, each as its property
            YAML declares it (the ``reservoir:`` mapping is read from it).

    Returns:
        The refusals, in module then rule order; empty when every declaration is valid.
    """
    errors: List[RateMetadataError] = []
    declared: Dict[Tuple[str, str], Dict[str, Any]] = {}
    resolved: Set[Tuple[str, str]] = set()
    slaved: List[Tuple[str, Dict[str, Any]]] = []

    for module in modules:
        errors.extend(_module_rate_errors(module, galaxy_properties, declared, resolved, slaved))

    def total_of(prop: str) -> str:
        return galaxy_properties[prop]["reservoir"]["total"]

    # Dependencies between totals: donor total -> recipient total, with the slaved
    # sources that create each one (tracer dependencies carry none).
    edges: Dict[str, Set[str]] = {}
    via: Dict[Tuple[str, str], List[Tuple[str, str]]] = {}
    for module_name, transfer in slaved:
        label = f"{RATES_KEY}.transfers '{transfer['name']}'"
        master_module, master_name = SLAVED_TO.fullmatch(transfer["slaved_to"]).groups()
        master = declared.get((master_module, master_name))
        if master is None:
            errors.append(
                RateMetadataError(
                    module_name,
                    f"{label} is slaved to '{transfer['slaved_to']}', which names no transfer "
                    "declared by a rate module",
                    True,
                )
            )
            continue
        if (master_module, master_name) not in resolved:
            continue  # The master's own refusal names its defect.
        if master["from"] == EXTERNAL_END:
            errors.append(
                RateMetadataError(
                    module_name,
                    f"{label} is slaved to '{transfer['slaved_to']}', a source; a master is a "
                    "transfer out of a reservoir (slaved sources are one level deep)",
                )
            )
            continue
        donor_total = total_of(master["from"])
        recipient_total = total_of(transfer["to"])
        if donor_total == recipient_total:
            errors.append(
                RateMetadataError(
                    module_name,
                    f"{label} is slaved to '{transfer['slaved_to']}', whose donor "
                    f"'{master['from']}' is in total '{donor_total}', the recipient's total; a "
                    "slaved source feeds a total other than its master's donor's",
                )
            )
            continue
        edges.setdefault(donor_total, set()).add(recipient_total)
        via.setdefault((donor_total, recipient_total), []).append(
            (
                module_name,
                f"{module_name} {RATES_KEY}.transfers '{transfer['name']}' (slaved to "
                f"{transfer['slaved_to']})",
            )
        )

    for prop in galaxy_properties.values():
        mapping = prop.get("reservoir") if isinstance(prop, dict) else None
        if isinstance(mapping, dict) and mapping.get("kind") == "tracer":
            parent = _reservoir_mapping(galaxy_properties, mapping.get("parent"))
            if parent and parent.get("total") and mapping.get("total"):
                edges.setdefault(parent["total"], set()).add(mapping["total"])

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
        errors.append(
            RateMetadataError(
                links[0][0],
                f"{RATES_KEY}: conserved totals {component} depend on each other in a cycle "
                f"through the slaved sources {'; '.join(text for _, text in links)}; a slaved "
                "source makes its recipient's total depend on its master donor's total, and those "
                "dependencies must not form a cycle",
            )
        )

    return errors
