#!/usr/bin/env python3
"""
Rate metadata: the `rates:` module key, the global transfer index and the generated rate tables.

Validates:
  - test_fixture_declarations_accepted: the four rate fixtures' declarations, against
    the fixture properties, pass both the registry generator's and the metadata
    validator's rate validation
  - test_rate_refusals: every refusal of the `rates:` key that can be decided from
    metadata is raised by both tools, called in-process on doctored copies of the
    fixture declarations, with a message naming the module and the transfer (the
    one row with no transfer to name, process_rates without rates:, names the
    module); each row doctors the accepted set, so it fails for its own reason
  - test_unknown_keys_refused: an unknown key inside rates:, inside a transfer and
    inside an observer is refused by both tools
  - test_validator_exit_codes: a malformed declaration is a schema error (1) and a
    reference to an undeclared property or transfer a dependency error (3)
  - test_global_index_order: the global transfer index orders the fixtures'
    transfers by module name, then declaration order, and the emitted tables and
    header are byte-identical when the modules are discovered in reverse or another
    order, or alongside a module without rates:
  - test_removed_property_fails_generation: removing a property a transfer names
    makes the generator's validation fail, naming the transfer
  - test_tables_name_reservoirs_by_symbol: every transfer end and observer property
    in the emitted tables is a generated index symbol (RESERVOIR_INDEX_<Property>)
    or RATE_TRANSFER_EXTERNAL, never an integer
  - test_production_generation_has_no_rate_table: generation without
    MIMIC_TEST_BUILD (a dry run, writing nothing) registers no fixture and emits
    no rate table, and its rate header declares a count of zero
  - test_rate_module_linkage_follows_the_build: a rate module's struct Module has
    external linkage only when generating a test build
  - test_validator_resolves_master_for_one_module_path: validating one module path
    still resolves a slaved master another module declares
  - test_changes_totals_accepted_and_emitted: test_rate_jump's changes_totals passes both
    tools, and generation emits its table of total names and its struct Module members
  - test_changes_totals_refusals: changes_totals that is not a non-empty list, lists a
    non-string, lists a total twice, names an undeclared total, or sits on a module with no
    discrete FoF mode is refused by both tools, naming the module, with exit code 1 (3 for
    an undeclared total)

The generated tables' contents in a test build, and the fixture laws, are checked
by tests/unit/test_rate_transfer_tables.c.
"""

import contextlib
import copy
import io
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

import yaml

REPO_ROOT = Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(REPO_ROOT / "scripts"))
sys.path.insert(0, str(REPO_ROOT / "tests"))

import validate_modules  # noqa: E402  (path set up above)
from discovery import model_property_files, rel  # noqa: E402
from framework import run_test_suite  # noqa: E402
from generate_module_registry import (  # noqa: E402
    event_name_to_enum_constant,
    generate_changes_totals_definitions,
    generate_module_struct_definitions,
    generate_rate_table_definitions,
    load_galaxy_property_metadata,
    load_module_metadata,
    module_name_to_id_macro,
    render_rate_contracts_h,
    validate_rate_declarations,
)
from module_modes import (  # noqa: E402
    RESERVED_RATE_IDENTIFIERS,
    RESERVED_RATE_TAGS,
    event_identifiers,
    global_transfer_index,
)

MODULE_SYSTEM = REPO_ROOT / "src" / "module_system"
FIXTURES = ("test_rate_supply", "test_rate_formation", "test_rate_feedback", "test_rate_stripping")
TEST_PROPERTIES = MODULE_SYSTEM / "test_fixture" / "test_properties.yaml"

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------


def fixture_properties():
    """Return the test-only galaxy properties by name, as test_properties.yaml declares them."""
    with open(TEST_PROPERTIES, "r", encoding="utf-8") as f:
        data = yaml.safe_load(f)
    return {prop["name"]: prop for prop in data["galaxy_properties"]}


def fixture_modules():
    """Return the four rate fixtures' metadata, keyed by module name, as the generator loads it."""
    return {name: load_module_metadata(MODULE_SYSTEM / name) for name in FIXTURES}


def find_transfer(modules, module, transfer):
    """Return one declared transfer mapping of a module dict keyed by name."""
    for entry in modules[module]["rates"]["transfers"]:
        if entry["name"] == transfer:
            return entry
    raise KeyError(f"{module}.{transfer}")


def reservoir(name, total, units="1e10 Msun/h"):
    """Return a synthetic double galaxy property declaring a reservoir in a total."""
    return {
        "name": name,
        "type": "double",
        "units": units,
        "description": f"synthetic reservoir {name}",
        "output": False,
        "reservoir": {"kind": "reservoir", "total": total},
    }


def generator_errors(modules, props):
    """Run the generator's rate validation; return its error strings."""
    return validate_rate_declarations(list(modules.values()), props)


def validator_errors(modules, props):
    """Run the validator's rate validation; return (module, exit code, message) triples."""
    results = validate_modules.ValidationResults()
    pairs = [(MODULE_SYSTEM / name, module) for name, module in modules.items()]
    validate_modules.validate_rate_declarations(pairs, props, results)
    return [(error.module_name, error.exit_code, error.message) for error in results.errors]


# ---------------------------------------------------------------------------
# Refusal table: (case, doctor, module, transfer, message fragment)
#
# Each doctor edits deep copies of the fixture modules (keyed by name) and of the
# fixture properties in place. `transfer` is the transfer the message must name, or
# None for the one refusal that concerns no transfer.
# ---------------------------------------------------------------------------


def _both_external(m, p):
    find_transfer(m, "test_rate_supply", "infall")["to"] = "external"


def _reads_central_not_bool(m, p):
    m["test_rate_supply"]["rates"]["reads_central"] = "yes"


def _from_tracer(m, p):
    find_transfer(m, "test_rate_supply", "cooling")["from"] = "RateFixtureHotTracer"


def _from_observer(m, p):
    find_transfer(m, "test_rate_formation", "star_formation")["from"] = "RateFixtureFormed"


def _to_undeclared(m, p):
    find_transfer(m, "test_rate_supply", "cooling")["to"] = "NoSuchProperty"


def _to_not_reservoir(m, p):
    find_transfer(m, "test_rate_supply", "cooling")["to"] = "TestDummyProperty"


def _to_observer(m, p):
    find_transfer(m, "test_rate_supply", "cooling")["to"] = "RateFixtureFormed"


def _to_tracer_from_reservoir(m, p):
    find_transfer(m, "test_rate_supply", "cooling")["to"] = "RateFixtureColdTracer"


def _internal_across_totals(m, p):
    p["RateFixtureOther"] = reservoir("RateFixtureOther", "other_mass")
    find_transfer(m, "test_rate_supply", "cooling")["to"] = "RateFixtureOther"


def _central_to_external(m, p):
    find_transfer(m, "test_rate_stripping", "ejection")["to_record"] = "central"


def _bad_to_record(m, p):
    find_transfer(m, "test_rate_stripping", "ejection")["to_record"] = "neighbour"


def _duplicate_name(m, p):
    find_transfer(m, "test_rate_supply", "cooling")["name"] = "infall"


def _bad_name(m, p):
    find_transfer(m, "test_rate_supply", "cooling")["name"] = "2cool"


def _slaved_unknown(m, p):
    find_transfer(m, "test_rate_feedback", "yield")["slaved_to"] = "test_rate_formation.nothing"


def _slaved_malformed(m, p):
    find_transfer(m, "test_rate_feedback", "yield")["slaved_to"] = "star_formation"


def _slaved_to_source(m, p):
    find_transfer(m, "test_rate_feedback", "yield")["slaved_to"] = "test_rate_supply.infall"


def _slaved_to_slaved(m, p):
    m["test_rate_feedback"]["rates"]["transfers"].append(
        {
            "name": "second_yield",
            "from": "external",
            "to": "RateFixtureHotTracer",
            "slaved_to": "test_rate_feedback.yield",
        }
    )


def _slaved_same_total(m, p):
    find_transfer(m, "test_rate_feedback", "yield")["to"] = "RateFixtureHot"


def _slaved_on_internal(m, p):
    find_transfer(m, "test_rate_feedback", "reheating")[
        "slaved_to"
    ] = "test_rate_formation.star_formation"


def _total_cycle(m, p):
    # Two totals, each fed by a source slaved to a sink of the other.
    p["CycleA"] = reservoir("CycleA", "total_a")
    p["CycleB"] = reservoir("CycleB", "total_b")
    for own, other in (("a", "b"), ("b", "a")):
        m[f"test_cycle_{own}"] = {
            "name": f"test_cycle_{own}",
            "supported_processing_modes": ["process_rates"],
            "rates": {
                "transfers": [
                    {"name": f"drain_{own}", "from": f"Cycle{own.upper()}", "to": "external"},
                    {
                        "name": f"feed_{own}",
                        "from": "external",
                        "to": f"Cycle{own.upper()}",
                        "slaved_to": f"test_cycle_{other}.drain_{other}",
                    },
                ]
            },
        }


def _observer_not_observer(m, p):
    m["test_rate_formation"]["rates"]["observers"][0]["property"] = "RateFixtureStars"


def _observer_undeclared_transfer(m, p):
    m["test_rate_formation"]["rates"]["observers"][0]["transfer"] = "cooling"


def _rates_without_mode(m, p):
    m["test_rate_supply"]["supported_processing_modes"] = ["process_by_galaxy"]


def _mode_without_rates(m, p):
    del m["test_rate_supply"]["rates"]


def _empty_transfers(m, p):
    m["test_rate_feedback"]["rates"]["transfers"] = []
    m["test_rate_feedback"]["rates"].pop("observers", None)
    # The yield's master no longer exists in this module, so nothing else is slaved.


def _to_record_list(m, p):
    find_transfer(m, "test_rate_stripping", "ejection")["to_record"] = ["central"]


def _to_record_mapping(m, p):
    find_transfer(m, "test_rate_stripping", "ejection")["to_record"] = {"central": True}


def _missing_total(m, p):
    del p["RateFixtureStars"]["reservoir"]["total"]


def _case_duplicate(m, p):
    find_transfer(m, "test_rate_supply", "cooling")["name"] = "Infall"


def _sink_module(name, transfer, extra=None):
    """Return a synthetic rate module with one sink out of RateFixtureCold."""
    module = {
        "name": name,
        "supported_processing_modes": ["process_rates"],
        "rates": {"transfers": [{"name": transfer, "from": "RateFixtureCold", "to": "external"}]},
    }
    module.update(extra or {})
    return module


def _constant_collision(m, p):
    # TEST_CROSS + _TRANSFER_ + TRANSFER_DRAIN == TEST_CROSS_TRANSFER + _TRANSFER_ + DRAIN
    m["test_cross"] = _sink_module("test_cross", "transfer_drain")
    m["test_cross_transfer"] = _sink_module("test_cross_transfer", "drain")


def _count_macro_collision(m, p):
    # test_n's constant TEST_N_TRANSFER_NUM_TRANSFERS is test_n_transfer's count macro
    m["test_n"] = _sink_module("test_n", "num_transfers")
    m["test_n_transfer"] = _sink_module("test_n_transfer", "drain")


def _enum_tag_collision(m, p):
    # test__tag and test_tag both generate the enum tag TestTagTransfer
    m["test__tag"] = _sink_module("test__tag", "drain")
    m["test_tag"] = _sink_module("test_tag", "drain")


def _core_tag_collision(m, p):
    # A module named rate generates enum RateTransfer, the tag of struct RateTransfer
    m["rate"] = _sink_module("rate", "drain")


def _core_identifier_collision(m, p):
    m["mimic_rate"] = _sink_module("mimic_rate", "count")


def _event_identifier_collision(m, p):
    # test_ev's event constant TEST_EV_EVENT_Y_TRANSFER_Z is test_ev_event_y's z constant
    m["test_ev"] = {
        "name": "test_ev",
        "supported_processing_modes": ["process_full_halo"],
        "events": {"emits": [{"name": "y_transfer_z", "description": "probe"}]},
    }
    m["test_ev_event_y"] = _sink_module("test_ev_event_y", "z")


def _reservoir_index_collision(m, p):
    # A property A_TRANSFER_B gets RESERVOIR_INDEX_A_TRANSFER_B in reservoir_table.h, the
    # coefficient constant of module reservoir_index_a's transfer b
    p["A_TRANSFER_B"] = {
        **reservoir("A_TRANSFER_B", "unused"),
        "reservoir": {"kind": "observer"},
    }
    m["reservoir_index_a"] = _sink_module("reservoir_index_a", "b")


def _references_to_refused_transfer(m, p):
    # STAR_FORMATION is refused as a case-only duplicate of star_formation; the observer
    # and the slaved source naming it add no refusal of their own
    m["test_rate_formation"]["rates"]["transfers"].append(
        {"name": "STAR_FORMATION", "from": "RateFixtureCold", "to": "RateFixtureStars"}
    )
    m["test_rate_formation"]["rates"]["observers"][0]["transfer"] = "STAR_FORMATION"
    find_transfer(m, "test_rate_feedback", "yield")[
        "slaved_to"
    ] = "test_rate_formation.STAR_FORMATION"


def _observer_transfer_list(m, p):
    m["test_rate_formation"]["rates"]["observers"][0]["transfer"] = ["star_formation"]


def _observer_transfer_mapping(m, p):
    m["test_rate_formation"]["rates"]["observers"][0]["transfer"] = {"star_formation": 1}


def _duplicate_observer(m, p):
    observers = m["test_rate_formation"]["rates"]["observers"]
    observers.append(copy.deepcopy(observers[0]))


def _noop_transfer(m, p):
    find_transfer(m, "test_rate_supply", "cooling")["to"] = "RateFixtureHot"


REFUSALS = [
    ("both ends external", _both_external, "test_rate_supply", "infall", "both ends 'external'", 1),
    (
        "reads_central not a boolean",
        _reads_central_not_bool,
        "test_rate_supply",
        "cooling",
        "reads_central is 'yes'",
        1,
    ),
    (
        "from is a tracer",
        _from_tracer,
        "test_rate_supply",
        "cooling",
        "takes from 'RateFixtureHotTracer', a property of reservoir kind 'tracer'",
        1,
    ),
    (
        "from is an observer",
        _from_observer,
        "test_rate_formation",
        "star_formation",
        "takes from 'RateFixtureFormed', a property of reservoir kind 'observer'",
        1,
    ),
    (
        "to is not a declared property",
        _to_undeclared,
        "test_rate_supply",
        "cooling",
        "'NoSuchProperty', which is not a declared galaxy property",
        1,
    ),
    (
        "to declares no reservoir:",
        _to_not_reservoir,
        "test_rate_supply",
        "cooling",
        "'TestDummyProperty', a property that declares no reservoir mapping",
        1,
    ),
    (
        "to is an observer",
        _to_observer,
        "test_rate_supply",
        "cooling",
        "moves into 'RateFixtureFormed', a property of reservoir kind 'observer'",
        1,
    ),
    (
        "tracer as to from a reservoir",
        _to_tracer_from_reservoir,
        "test_rate_supply",
        "cooling",
        "a tracer may be a transfer's to only when its from is 'external'",
        1,
    ),
    (
        "internal transfer across totals",
        _internal_across_totals,
        "test_rate_supply",
        "cooling",
        "an internal transfer stays within one conserved total",
        1,
    ),
    (
        "to_record central with to external",
        _central_to_external,
        "test_rate_stripping",
        "ejection",
        "to_record: central with to: external",
        1,
    ),
    (
        "to_record not self or central",
        _bad_to_record,
        "test_rate_stripping",
        "ejection",
        "has to_record 'neighbour'",
        1,
    ),
    (
        "duplicate transfer name",
        _duplicate_name,
        "test_rate_supply",
        "infall",
        "is declared twice",
        1,
    ),
    ("transfer name not an identifier", _bad_name, "test_rate_supply", "2cool", "C identifier", 1),
    (
        "slaved_to names no transfer",
        _slaved_unknown,
        "test_rate_feedback",
        "yield",
        "names no transfer declared by a rate module",
        1,
    ),
    (
        "slaved_to not <module>.<transfer>",
        _slaved_malformed,
        "test_rate_feedback",
        "yield",
        "<module>.<transfer>",
        1,
    ),
    (
        "slaved_to names a source",
        _slaved_to_source,
        "test_rate_feedback",
        "yield",
        "'test_rate_supply.infall', a source",
        1,
    ),
    (
        "slaved_to names a slaved source",
        _slaved_to_slaved,
        "test_rate_feedback",
        "second_yield",
        "'test_rate_feedback.yield', a source",
        1,
    ),
    (
        "slaved_to whose donor's total is the recipient's",
        _slaved_same_total,
        "test_rate_feedback",
        "yield",
        "the recipient's total",
        1,
    ),
    (
        "slaved_to on a transfer out of a reservoir",
        _slaved_on_internal,
        "test_rate_feedback",
        "reheating",
        "only a source (from: external) is slaved",
        1,
    ),
    (
        "dependency cycle among totals",
        _total_cycle,
        "test_cycle_a",
        "feed_a",
        "conserved totals ['total_a', 'total_b'] depend on each other in a cycle",
        2,  # one refusal against each module on the cycle
    ),
    (
        "observer property not of kind observer",
        _observer_not_observer,
        "test_rate_formation",
        "star_formation",
        "an observer's property is of reservoir kind 'observer'",
        1,
    ),
    (
        "observer names an undeclared transfer",
        _observer_undeclared_transfer,
        "test_rate_formation",
        "cooling",
        "observes a transfer this module does not declare",
        1,
    ),
    (
        "rates: without process_rates",
        _rates_without_mode,
        "test_rate_supply",
        "infall",
        "do not include process_rates",
        1,
    ),
    (
        "process_rates without rates:",
        _mode_without_rates,
        "test_rate_supply",
        None,
        "advertises process_rates but declares no rates:",
        1,
    ),
    (
        "empty transfer list",
        _empty_transfers,
        "test_rate_feedback",
        None,
        "rates.transfers must be a non-empty list",
        1,
    ),
    (
        "to_record a list",
        _to_record_list,
        "test_rate_stripping",
        "ejection",
        "has to_record ['central']",
        1,
    ),
    (
        "to_record a mapping",
        _to_record_mapping,
        "test_rate_stripping",
        "ejection",
        "has to_record {'central': True}",
        1,
    ),
    (
        "reservoir end without a total",
        _missing_total,
        "test_rate_formation",
        "star_formation",
        "names 'RateFixtureStars', whose reservoir: mapping declares no total",
        1,
    ),
    (
        "transfer names differing only in case",
        _case_duplicate,
        "test_rate_supply",
        "Infall",
        "and rates.transfers 'infall' differ only in case",
        1,
    ),
    (
        "coefficient constants collide across modules",
        _constant_collision,
        "test_cross",
        "transfer_drain",
        "identifier TEST_CROSS_TRANSFER_TRANSFER_DRAIN",
        2,  # one refusal against each module
    ),
    (
        "a coefficient constant collides with a count macro",
        _count_macro_collision,
        "test_n_transfer",
        "drain",
        "identifier TEST_N_TRANSFER_NUM_TRANSFERS",
        2,  # one refusal against each module
    ),
    (
        "enum tags collide across modules",
        _enum_tag_collision,
        "test__tag",
        "drain",
        "enum tag TestTagTransfer",
        2,  # one refusal against each module
    ),
    (
        "enum tag collides with a core tag",
        _core_tag_collision,
        "rate",
        "drain",
        "enum tag RateTransfer (the coefficient enum of rate (transfers ['drain']) and the "
        "core tag",
        1,
    ),
    (
        "coefficient constant collides with a core identifier",
        _core_identifier_collision,
        "mimic_rate",
        "count",
        "identifier MIMIC_RATE_TRANSFER_COUNT",
        1,
    ),
    (
        "coefficient constant collides with an event identifier",
        _event_identifier_collision,
        "test_ev_event_y",
        "z",
        "identifier TEST_EV_EVENT_Y_TRANSFER_Z",
        1,
    ),
    (
        "coefficient constant collides with a reservoir index",
        _reservoir_index_collision,
        "reservoir_index_a",
        "b",
        "identifier RESERVOIR_INDEX_A_TRANSFER_B",
        1,
    ),
    (
        "references to a transfer refused as a case-only duplicate",
        _references_to_refused_transfer,
        "test_rate_formation",
        "STAR_FORMATION",
        "and rates.transfers 'star_formation' differ only in case",
        1,  # the duplicate itself; the observer and the slaved source add nothing
    ),
    (
        "observer transfer a list",
        _observer_transfer_list,
        "test_rate_formation",
        "star_formation",
        "(transfer ['star_formation'], property 'RateFixtureFormed') observes a transfer this "
        "module does not declare",
        1,
    ),
    (
        "observer transfer a mapping",
        _observer_transfer_mapping,
        "test_rate_formation",
        "star_formation",
        "(transfer {'star_formation': 1}, property 'RateFixtureFormed') observes a transfer "
        "this module does not declare",
        1,
    ),
    (
        "duplicate observer",
        _duplicate_observer,
        "test_rate_formation",
        "star_formation",
        "repeats an observer of property 'RateFixtureFormed' on transfer 'star_formation'",
        1,
    ),
    (
        "internal transfer into itself on the same record",
        _noop_transfer,
        "test_rate_supply",
        "cooling",
        "moves 'RateFixtureHot' into itself on the same record",
        1,
    ),
]


def run_both_tools(doctor):
    """Doctor fresh copies of the accepted set; return (generator errors, validator errors)."""
    modules = copy.deepcopy(fixture_modules())
    props = copy.deepcopy(fixture_properties())
    doctor(modules, props)
    return generator_errors(modules, props), validator_errors(modules, props)


def assert_refused(case, doctor, module, transfer, fragment, count):
    """Assert both tools refuse a doctored set, naming the module, the transfer and the reason.

    Each tool must report exactly ``count`` refusals, so a row cannot pass on a
    refusal that some other defect of the doctored set causes.
    """
    gen_errors, val_errors = run_both_tools(doctor)
    assert len(gen_errors) == count, (
        f"{case}: the generator reported {len(gen_errors)} refusal(s), expected {count}: "
        f"{gen_errors}"
    )
    assert len(val_errors) == count, (
        f"{case}: validate_modules.py reported {len(val_errors)} refusal(s), expected {count}: "
        f"{val_errors}"
    )

    gen_hits = [
        e
        for e in gen_errors
        if e.startswith(f"{module}/module_info.yaml: ")
        and fragment in e
        and (transfer is None or f"'{transfer}'" in e)
    ]
    assert gen_hits, (
        f"{case}: the generator does not refuse it naming module {module!r}, transfer "
        f"{transfer!r} and {fragment!r}; it reported {gen_errors}"
    )

    val_hits = [
        message
        for name, _code, message in val_errors
        if name == module
        and fragment in message
        and (transfer is None or f"'{transfer}'" in message)
    ]
    assert val_hits, (
        f"{case}: validate_modules.py does not refuse it naming module {module!r}, transfer "
        f"{transfer!r} and {fragment!r}; it reported {val_errors}"
    )


# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------


def test_fixture_declarations_accepted():
    """The four fixtures' declarations pass both tools' rate validation."""
    modules = fixture_modules()
    props = fixture_properties()
    assert generator_errors(modules, props) == [], generator_errors(modules, props)
    assert validator_errors(modules, props) == [], validator_errors(modules, props)
    print(f"  ✓ {len(modules)} fixture declarations accepted by the generator and the validator")


def test_rate_refusals():
    """Every metadata-decidable refusal is raised by both tools, naming module and transfer."""
    for case, doctor, module, transfer, fragment, count in REFUSALS:
        assert_refused(case, doctor, module, transfer, fragment, count)
    print(f"  ✓ {len(REFUSALS)} refusals raised by both tools")


# Every raw value the rates: rules read from module metadata, as (module, path) where a
# path is a key sequence from the module's rates: mapping (integers index lists).
RAW_RATE_FIELDS = [
    ("test_rate_supply", ("reads_central",)),
    ("test_rate_supply", ("transfers",)),
    ("test_rate_supply", ("transfers", 2)),
    ("test_rate_supply", ("transfers", 2, "name")),
    ("test_rate_supply", ("transfers", 2, "from")),
    ("test_rate_supply", ("transfers", 2, "to")),
    ("test_rate_supply", ("transfers", 2, "to_record")),
    ("test_rate_feedback", ("transfers", 1, "slaved_to")),
    ("test_rate_formation", ("observers",)),
    ("test_rate_formation", ("observers", 0)),
    ("test_rate_formation", ("observers", 0, "property")),
    ("test_rate_formation", ("observers", 0, "transfer")),
]


def test_container_values_refused_without_crashing():
    """A list or mapping in any raw rates: field is refused by both tools, never raised.

    Sweeps every field the rules read (RAW_RATE_FIELDS) with a list and a mapping
    value, so a membership test or lookup on an unhashable YAML value cannot crash
    either tool.
    """
    cases = 0
    for module, path in RAW_RATE_FIELDS:
        for value in (["star_formation"], {"star_formation": 1}):

            def doctor(m, p, module=module, path=path, value=value):
                target = m[module]["rates"]
                for key in path[:-1]:
                    target = target[key]
                target[path[-1]] = value

            gen_errors, val_errors = run_both_tools(doctor)
            where = f"{module} rates.{'.'.join(map(str, path))} = {value!r}"
            assert gen_errors, f"{where}: the generator accepted it"
            assert val_errors, f"{where}: validate_modules.py accepted it"
            cases += 1
    print(f"  ✓ {cases} container values refused by both tools without an exception")


def test_unknown_keys_refused():
    """An unknown key inside rates:, a transfer or an observer is refused by both tools."""

    def in_rates(m, p):
        m["test_rate_formation"]["rates"]["transfer"] = []

    def in_transfer(m, p):
        find_transfer(m, "test_rate_formation", "star_formation")["rate"] = 1.0

    def in_observer(m, p):
        m["test_rate_formation"]["rates"]["observers"][0]["scale"] = 2

    cases = [
        ("unknown rates: key", in_rates, "unknown rates: key(s) ['transfer']"),
        ("unknown transfer key", in_transfer, "has unknown key(s) ['rate']"),
        ("unknown observer key", in_observer, "has unknown key(s) ['scale']"),
    ]
    for case, doctor, fragment in cases:
        assert_refused(case, doctor, "test_rate_formation", "star_formation", fragment, 1)
    print("  ✓ unknown keys inside rates:, a transfer and an observer refused by both tools")


def test_validator_exit_codes():
    """A malformed declaration is a schema error (1); an unresolved reference a dependency (3)."""
    _, schema = run_both_tools(_both_external)
    assert [code for name, code, _ in schema if name == "test_rate_supply"] == [1], schema
    _, dependency = run_both_tools(_to_undeclared)
    assert [code for name, code, _ in dependency if name == "test_rate_supply"] == [3], dependency
    _, master = run_both_tools(_slaved_unknown)
    assert [code for name, code, _ in master if name == "test_rate_feedback"] == [3], master
    print("  ✓ schema errors exit 1, unresolved references exit 3")


def test_global_index_order():
    """Module name, then declaration order; emitted text independent of discovery order."""
    modules = list(fixture_modules().values())
    expected = [
        ("test_rate_feedback", "reheating"),
        ("test_rate_feedback", "yield"),
        ("test_rate_formation", "star_formation"),
        ("test_rate_stripping", "stripping"),
        ("test_rate_stripping", "ejection"),
        ("test_rate_supply", "infall"),
        ("test_rate_supply", "infall_tracer"),
        ("test_rate_supply", "cooling"),
    ]
    assert global_transfer_index(modules) == expected, global_transfer_index(modules)

    plain = load_module_metadata(MODULE_SYSTEM / "test_fixture")
    orders = {
        "forward": modules,
        "reversed": list(reversed(modules)),
        "interleaved": [modules[2], modules[0], modules[3], modules[1]],
        "with a module without rates:": [plain] + list(reversed(modules)),
    }
    reference_tables = generate_rate_table_definitions(modules)
    reference_header = render_rate_contracts_h(modules, "0" * 32)
    for label, order in orders.items():
        assert global_transfer_index(order) == expected, f"{label}: global index changed"
        assert (
            generate_rate_table_definitions(order) == reference_tables
        ), f"{label}: emitted transfer tables changed with discovery order"
        assert (
            render_rate_contracts_h(order, "0" * 32) == reference_header
        ), f"{label}: emitted rate_contracts.h changed with discovery order"
    print(f"  ✓ global index and emitted text identical over {len(orders)} discovery orders")


def test_removed_property_fails_generation():
    """Removing a property a transfer names makes the generator's validation fail."""
    modules = fixture_modules()
    props = fixture_properties()
    del props["RateFixtureStars"]
    errors = generator_errors(modules, props)
    hits = [
        e
        for e in errors
        if e.startswith("test_rate_formation/module_info.yaml: ")
        and "'star_formation'" in e
        and "'RateFixtureStars', which is not a declared galaxy property" in e
    ]
    assert (
        hits
    ), f"removing RateFixtureStars did not fail generation naming star_formation: {errors}"
    print("  ✓ removing a transfer end fails generation, naming the transfer")


def test_tables_name_reservoirs_by_symbol():
    """Every emitted end and observer property is an index symbol, never an integer."""
    text = "\n".join(generate_rate_table_definitions(list(fixture_modules().values())))
    ends = re.findall(r"\.(from|to|property) = ([^,}]+)", text)
    assert ends, "the emitted tables carry no ends"
    symbol = re.compile(r"RESERVOIR_INDEX_[A-Za-z_][A-Za-z0-9_]*|RATE_TRANSFER_EXTERNAL")
    for field, value in ends:
        value = value.strip()
        assert symbol.fullmatch(value), f".{field} = {value} is not a generated index symbol"

    for name in ("RateFixtureHot", "RateFixtureCold", "RateFixtureStars", "RateFixtureFormed"):
        assert f"RESERVOIR_INDEX_{name}" in text, f"RESERVOIR_INDEX_{name} not emitted"
    print(f"  ✓ {len(ends)} emitted ends and observer properties are index symbols")


def test_production_generation_has_no_rate_table():
    """A production dry run registers no fixture and emits no rate table."""
    env = dict(os.environ)
    env.pop("MIMIC_TEST_BUILD", None)
    proc = subprocess.run(
        [sys.executable, str(REPO_ROOT / "scripts" / "generate_module_registry.py"), "--dry-run"],
        cwd=REPO_ROOT,
        env=env,
        capture_output=True,
        text=True,
        check=False,
    )
    assert proc.returncode == 0, f"production dry run failed:\n{proc.stdout}\n{proc.stderr}"
    init_c = proc.stdout.split("=== module_init.c (DRY RUN) ===", 1)[1].split("\n=== ", 1)[0]
    header = proc.stdout.split("=== rate_contracts.h (DRY RUN) ===", 1)[1].split("\n=== ", 1)[0]

    for forbidden in ("test_rate_", "struct RateTransfer", "RATE TRANSFER TABLES", ".rate_"):
        assert forbidden not in init_c, f"production module_init.c contains {forbidden!r}"
    assert "#define MIMIC_RATE_TRANSFER_COUNT 0" in header, header
    assert "enum " not in header, f"production rate_contracts.h declares an enum:\n{header}"
    print("  ✓ production generation: no fixture, no rate table, rate count 0")


def test_validator_resolves_master_for_one_module_path():
    """Validating one module path resolves a slaved master declared by another module."""
    env = dict(os.environ, MIMIC_TEST_BUILD="1")
    proc = subprocess.run(
        [
            sys.executable,
            str(REPO_ROOT / "scripts" / "validate_modules.py"),
            str(MODULE_SYSTEM / "test_rate_feedback"),
        ],
        cwd=REPO_ROOT,
        env=env,
        capture_output=True,
        text=True,
        check=False,
    )
    assert proc.returncode == 0, f"validating test_rate_feedback alone failed:\n{proc.stdout}"
    print("  ✓ one module path validated with every module as the rate context")


def run_validator_main(modules, extra_props, selected):
    """Run validate_modules.main() on one temporary module path; return (exit code, output).

    Writes each module (keyed by directory name) with a placeholder source to a
    temporary directory, adds them to discovery and ``extra_props`` to the property
    metadata (as galaxy properties, in both loaders the validator reads), then
    validates the module ``selected`` alone, so the single-module report filter applies.
    """
    galaxy_source = rel(model_property_files()[0])
    props = {name: {**prop, "source": galaxy_source} for name, prop in extra_props.items()}
    real_discover = validate_modules.discover_modules
    real_properties = validate_modules.load_property_metadata
    real_galaxy_properties = validate_modules.load_galaxy_property_metadata
    real_argv = sys.argv
    with tempfile.TemporaryDirectory() as tmp:
        dirs = {}
        for name, module in modules.items():
            module_dir = Path(tmp) / name
            module_dir.mkdir()
            (module_dir / "module_info.yaml").write_text(yaml.safe_dump({"module": module}))
            (module_dir / f"{name}.c").write_text("/* placeholder */\n")
            dirs[name] = module_dir
        validate_modules.discover_modules = lambda: real_discover() + [
            (path, validate_modules.load_module_metadata(path)) for path in dirs.values()
        ]
        validate_modules.load_property_metadata = lambda: {**real_properties(), **props}
        validate_modules.load_galaxy_property_metadata = lambda: {
            **real_galaxy_properties(),
            **extra_props,
        }
        try:
            sys.argv = ["validate_modules.py", str(dirs[selected])]
            output = io.StringIO()
            with contextlib.redirect_stdout(output):
                code = validate_modules.main()
            return code, output.getvalue()
        finally:
            validate_modules.discover_modules = real_discover
            validate_modules.load_property_metadata = real_properties
            validate_modules.load_galaxy_property_metadata = real_galaxy_properties
            sys.argv = real_argv


def test_single_module_validation_reports_cycle_from_both_sides():
    """Validating either module of a two-total cycle alone exits non-zero, naming the totals.

    Runs validate_modules.main() on one module path, so its report filter applies,
    with two cycle modules written to a temporary directory and added to discovery,
    and their two reservoirs added to the property metadata.
    """
    cycle_props = {
        name: reservoir(name, total)
        for name, total in (("CycleA", "total_a"), ("CycleB", "total_b"))
    }
    cycle = {}
    _total_cycle(cycle, {})
    for name in sorted(cycle):
        code, text = run_validator_main(cycle, cycle_props, name)
        assert code != 0, f"validating {name} alone exited 0:\n{text}"
        assert (
            f"[ERROR] {name}: rates: conserved totals ['total_a', 'total_b']" in text
        ), f"validating {name} alone did not report the cycle naming the totals:\n{text}"
    print("  ✓ the cycle is reported when either of its modules is validated alone")


def test_single_module_validation_of_an_unnamed_module():
    """A module whose metadata has no name still has its rate refusals reported when
    validated alone, under the name the rules attribute them to."""
    unnamed = {
        "supported_processing_modes": ["process_rates"],
        "rates": {"transfers": [{"name": "drift", "from": "external", "to": "external"}]},
    }
    code, text = run_validator_main({"test_unnamed_rate": unnamed}, {}, "test_unnamed_rate")
    assert code != 0, text
    expected = "[ERROR] <unnamed>: rates.transfers 'drift' has both ends 'external'"
    assert expected in text, f"the unnamed module's rate refusal was dropped:\n{text}"
    print("  ✓ an unnamed module's rate refusal is reported when it is validated alone")


def test_tools_share_the_galaxy_property_view():
    """Both tools load the rate rules' galaxy properties through one function.

    The galaxy-only view excludes halo properties, so a transfer end naming one
    (Mvir) gets one identical refusal from each tool.
    """
    assert (
        validate_modules.load_galaxy_property_metadata is load_galaxy_property_metadata
    ), "the validator does not load galaxy properties through the generator's function"
    saved = os.environ.get("MIMIC_TEST_BUILD")
    os.environ["MIMIC_TEST_BUILD"] = "1"
    try:
        view = load_galaxy_property_metadata()
    finally:
        if saved is None:
            os.environ.pop("MIMIC_TEST_BUILD")
        else:
            os.environ["MIMIC_TEST_BUILD"] = saved
    assert "Mvir" not in view and "RateFixtureCold" in view

    modules = copy.deepcopy(fixture_modules())
    find_transfer(modules, "test_rate_supply", "cooling")["to"] = "Mvir"
    gen = generator_errors(modules, view)
    val = validator_errors(modules, view)
    assert len(gen) == 1 and len(val) == 1, (gen, val)
    assert "'Mvir', which is not a declared galaxy property" in val[0][2], val
    assert gen[0] == f"{val[0][0]}/module_info.yaml: {val[0][2]}", (gen, val)
    print("  ✓ one galaxy property loader and an identical refusal for a halo-property end")


def struct_block(text, module):
    """Return the generated struct Module definition of one module."""
    match = re.search(rf"struct Module {module}_module = \{{\n(.*?)\n\}};", text, re.S)
    assert match, f"no struct Module for {module}"
    return match.group(1)


def test_reads_central_emitted():
    """reads_central: true is emitted as .reads_central = true, and false otherwise."""
    no_events = {"producer_ids": {}, "emits": {}, "consumes": {}}
    modules = copy.deepcopy(fixture_modules())
    text = "\n".join(generate_module_struct_definitions(list(modules.values()), no_events))
    for name in FIXTURES:
        assert ".reads_central = false" in struct_block(text, name), f"{name}: not false"

    modules["test_rate_formation"]["rates"]["reads_central"] = True
    assert generator_errors(modules, fixture_properties()) == []
    text = "\n".join(generate_module_struct_definitions(list(modules.values()), no_events))
    for name in FIXTURES:
        want = "true" if name == "test_rate_formation" else "false"
        assert f".reads_central = {want}" in struct_block(text, name), f"{name}: not {want}"
    print("  ✓ .reads_central = true only for the module declaring it")


def test_rate_module_linkage_follows_the_build():
    """A rate module's struct Module is external in a test build and static in production.

    The unit tests read the fixtures' generated rate members through the external
    symbol; a production build keeps every registration static.
    """
    no_events = {"producer_ids": {}, "emits": {}, "consumes": {}}
    modules = list(fixture_modules().values())
    saved = os.environ.get("MIMIC_TEST_BUILD")
    try:
        os.environ["MIMIC_TEST_BUILD"] = "1"
        test_text = "\n".join(generate_module_struct_definitions(modules, no_events))
        os.environ.pop("MIMIC_TEST_BUILD")
        production_text = "\n".join(generate_module_struct_definitions(modules, no_events))
    finally:
        if saved is None:
            os.environ.pop("MIMIC_TEST_BUILD", None)
        else:
            os.environ["MIMIC_TEST_BUILD"] = saved
    for name in FIXTURES:
        external = re.compile(rf"^struct Module {name}_module = \{{", re.M)
        static = re.compile(rf"^static struct Module {name}_module = \{{", re.M)
        assert external.search(test_text), f"{name}: not external in a test build"
        assert not static.search(test_text), f"{name}: static in a test build"
        assert static.search(production_text), f"{name}: not static in a production build"
        assert not external.search(production_text), f"{name}: external in a production build"
    print(f"  ✓ {len(FIXTURES)} rate modules external in a test build, static in production")


def test_reserved_names_match_sources():
    """The reserved name lists cover the core's names of the generated forms, and the
    event identifiers mirror the generator's event naming."""
    # Every header a rate module's code can reach: the core, the generated headers
    # under src/include/generated and src/module_system/generated, and the model
    # packages' shared helpers. rate_contracts.h holds the per-module generated names
    # themselves, so it is represented by its empty form.
    rate_contracts = REPO_ROOT / "src" / "module_system" / "generated" / "rate_contracts.h"
    roots = [REPO_ROOT / d for d in ("src/core", "src/include", "src/util", "src/module_system")]
    roots += sorted((REPO_ROOT / "models").glob("*/shared"))
    sources = sorted(
        path for root in roots for path in root.rglob("*.h") if path.resolve() != rate_contracts
    )
    assert any("generated" in path.parts for path in sources), "no generated header scanned"
    text = "\n".join(path.read_text(encoding="utf-8") for path in sources)
    text += render_rate_contracts_h([], "0" * 32)
    identifiers = set(re.findall(r"\b[A-Z0-9_]+_(?:TRANSFER_[A-Z0-9_]+|NUM_TRANSFERS)\b", text))
    tags = set(re.findall(r"\b(?:struct|enum|union)\s+([A-Za-z_][A-Za-z0-9_]*Transfer)\b", text))
    assert identifiers <= set(RESERVED_RATE_IDENTIFIERS), sorted(
        identifiers - set(RESERVED_RATE_IDENTIFIERS)
    )
    assert tags <= set(RESERVED_RATE_TAGS), sorted(tags - set(RESERVED_RATE_TAGS))
    assert "RateTransfer" in tags and "RATE_TRANSFER_EXTERNAL" in identifiers

    producer = {"name": "probe_producer", "events": {"emits": [{"name": "a"}, {"name": "b_c"}]}}
    assert event_identifiers(producer) == [
        module_name_to_id_macro("probe_producer"),
        event_name_to_enum_constant("probe_producer", "a"),
        event_name_to_enum_constant("probe_producer", "b_c"),
    ]
    print(f"  ✓ {len(identifiers)} core identifiers and {len(tags)} core tag(s) reserved")


# ---------------------------------------------------------------------------
# changes_totals
# ---------------------------------------------------------------------------

JUMP = "test_rate_jump"


def _with_jump(modules):
    """Add test_rate_jump's metadata, which declares changes_totals, to a module dict."""
    modules[JUMP] = load_module_metadata(MODULE_SYSTEM / JUMP)
    return modules[JUMP]


def _totals_not_a_list(m, p):
    _with_jump(m)["changes_totals"] = "rate_fixture_mass"


def _totals_empty(m, p):
    _with_jump(m)["changes_totals"] = []


def _totals_non_string(m, p):
    _with_jump(m)["changes_totals"] = ["rate_fixture_mass", 7]


def _totals_duplicate(m, p):
    _with_jump(m)["changes_totals"] = ["rate_fixture_mass", "rate_fixture_mass"]


def _totals_undeclared(m, p):
    _with_jump(m)["changes_totals"] = ["rate_fixture_mass", "rate_fixture_energy"]


def _totals_on_rate_only_module(m, p):
    _with_jump(m)
    m["test_rate_supply"]["changes_totals"] = ["rate_fixture_mass"]


# (case, doctor, module, message fragment, refusals per tool, validator exit code)
CHANGES_TOTALS_REFUSALS = [
    ("not a list", _totals_not_a_list, JUMP, "it must be a non-empty list of total names", 1, 1),
    ("empty list", _totals_empty, JUMP, "it must be a non-empty list of total names", 1, 1),
    ("non-string entry", _totals_non_string, JUMP, "entries must be total names: [7]", 1, 1),
    ("listed twice", _totals_duplicate, JUMP, "lists ['rate_fixture_mass'] more than once", 1, 1),
    (
        "undeclared total",
        _totals_undeclared,
        JUMP,
        "names ['rate_fixture_energy'], which no reservoir or tracer property declares",
        1,
        3,
    ),
    (
        "no discrete FoF mode",
        _totals_on_rate_only_module,
        "test_rate_supply",
        "on a module that advertises none of process_full_halo, process_per_event, "
        "process_by_galaxy",
        1,
        1,
    ),
]


def test_changes_totals_accepted_and_emitted():
    """test_rate_jump's changes_totals passes both tools and is emitted as names."""
    gen_errors, val_errors = run_both_tools(lambda m, p: _with_jump(m))
    assert gen_errors == [], f"the generator refused test_rate_jump: {gen_errors}"
    assert val_errors == [], f"validate_modules.py refused test_rate_jump: {val_errors}"

    jump = load_module_metadata(MODULE_SYSTEM / JUMP)
    table = "\n".join(generate_changes_totals_definitions([jump]))
    assert (
        'static const char *const test_rate_jump_changes_totals[] = {"rate_fixture_mass", '
        '"rate_fixture_tracer"};'
    ) in table, f"the emitted table is not the declared names:\n{table}"
    assert (
        generate_changes_totals_definitions(list(fixture_modules().values())) == []
    ), "modules without changes_totals emit a table"

    event_info = {"producer_ids": {}, "consumes": {}, "emits": {}}
    members = "\n".join(generate_module_struct_definitions([jump], event_info))
    assert "    .changes_totals = test_rate_jump_changes_totals," in members, members
    assert members.rstrip().endswith("    .num_changes_totals = 2\n};"), members
    print("  ✓ test_rate_jump's changes_totals accepted and emitted as total names")


def test_changes_totals_refusals():
    """Each changes_totals refusal is raised by both tools, naming the module."""
    for case, doctor, module, fragment, count, code in CHANGES_TOTALS_REFUSALS:
        assert_refused(case, doctor, module, None, fragment, count)
        _, val_errors = run_both_tools(doctor)
        codes = {exit_code for name, exit_code, _ in val_errors if name == module}
        assert codes == {code}, f"{case}: validator exit code(s) {codes}, expected {code}"
    print(f"  ✓ {len(CHANGES_TOTALS_REFUSALS)} changes_totals refusals raised by both tools")


# ---------------------------------------------------------------------------
# Runner
# ---------------------------------------------------------------------------


def main():
    """Run this file's tests via the shared framework runner."""
    return run_test_suite(
        [
            test_fixture_declarations_accepted,
            test_rate_refusals,
            test_container_values_refused_without_crashing,
            test_unknown_keys_refused,
            test_validator_exit_codes,
            test_global_index_order,
            test_removed_property_fails_generation,
            test_tables_name_reservoirs_by_symbol,
            test_production_generation_has_no_rate_table,
            test_validator_resolves_master_for_one_module_path,
            test_single_module_validation_reports_cycle_from_both_sides,
            test_single_module_validation_of_an_unnamed_module,
            test_tools_share_the_galaxy_property_view,
            test_reads_central_emitted,
            test_rate_module_linkage_follows_the_build,
            test_reserved_names_match_sources,
            test_changes_totals_accepted_and_emitted,
            test_changes_totals_refusals,
        ],
        "Rate Metadata (test_rate_metadata.py)",
    )


if __name__ == "__main__":
    sys.exit(main())
