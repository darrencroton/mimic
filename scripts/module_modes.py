#!/usr/bin/env python3
"""
Processing Mode Descriptors for Mimic

Single Python source of truth for module processing modes, shared by the module
registry generator (scripts/generate_module_registry.py) and the metadata
validator (scripts/validate_modules.py). It mirrors the C descriptor table in
src/core/module_registry.c: each mode names its configuration string, its C
enumerator and the callback family it dispatches to.

Lookups fail closed. A mode string absent from PROCESSING_MODES is invalid
everywhere, and no mode is assigned to a family by default: adding a mode or a
callback family needs an explicit descriptor here, a matching C table entry and
an implementation.

This file is a generation input. compute_metadata_hash() in the generator and
compute_module_metadata_hash() in scripts/check_generated.py both hash its path
and bytes, and the Makefile module-generation stamp depends on it, so editing it
invalidates generated module registration.

This module is imported, not run.
"""

from dataclasses import dataclass
from typing import Any, Dict, List, Tuple

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
)

MODES_BY_NAME: Dict[str, ProcessingModeDescriptor] = {mode.name: mode for mode in PROCESSING_MODES}
FAMILIES_BY_KEY: Dict[str, CallbackFamily] = {family.key: family for family in CALLBACK_FAMILIES}

# Standalone fallback modules (models/<model>/modules/<name>.c with no metadata)
# advertise exactly the original three FoF modes; they never gain later modes.
STANDALONE_FALLBACK_MODES: Tuple[str, ...] = (
    "process_full_halo",
    "process_per_event",
    "process_by_galaxy",
)

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
