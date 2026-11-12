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
  - test_validator_resolves_master_for_one_module_path: validating one module path
    still resolves a slaved master another module declares

The generated tables' contents in a test build, and the fixture laws, are checked
by tests/unit/test_rate_transfer_tables.c.
"""

import copy
import os
import re
import subprocess
import sys
from pathlib import Path

import yaml

REPO_ROOT = Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(REPO_ROOT / "scripts"))
sys.path.insert(0, str(REPO_ROOT / "tests"))

import validate_modules  # noqa: E402  (path set up above)
from framework import run_test_suite  # noqa: E402
from generate_module_registry import (  # noqa: E402
    generate_rate_table_definitions,
    load_module_metadata,
    render_rate_contracts_h,
    validate_rate_declarations,
)
from module_modes import global_transfer_index  # noqa: E402

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


REFUSALS = [
    ("both ends external", _both_external, "test_rate_supply", "infall", "both ends 'external'"),
    (
        "reads_central not a boolean",
        _reads_central_not_bool,
        "test_rate_supply",
        "cooling",
        "reads_central is 'yes'",
    ),
    (
        "from is a tracer",
        _from_tracer,
        "test_rate_supply",
        "cooling",
        "takes from 'RateFixtureHotTracer', a property of reservoir kind 'tracer'",
    ),
    (
        "from is an observer",
        _from_observer,
        "test_rate_formation",
        "star_formation",
        "takes from 'RateFixtureFormed', a property of reservoir kind 'observer'",
    ),
    (
        "to is not a declared property",
        _to_undeclared,
        "test_rate_supply",
        "cooling",
        "'NoSuchProperty', which is not a declared property",
    ),
    (
        "to declares no reservoir:",
        _to_not_reservoir,
        "test_rate_supply",
        "cooling",
        "'TestDummyProperty', a property that declares no reservoir:",
    ),
    (
        "to is an observer",
        _to_observer,
        "test_rate_supply",
        "cooling",
        "moves into 'RateFixtureFormed', a property of reservoir kind 'observer'",
    ),
    (
        "tracer as to from a reservoir",
        _to_tracer_from_reservoir,
        "test_rate_supply",
        "cooling",
        "a tracer may be a transfer's to only when its from is 'external'",
    ),
    (
        "internal transfer across totals",
        _internal_across_totals,
        "test_rate_supply",
        "cooling",
        "an internal transfer stays within one conserved total",
    ),
    (
        "to_record central with to external",
        _central_to_external,
        "test_rate_stripping",
        "ejection",
        "to_record: central with to: external",
    ),
    (
        "to_record not self or central",
        _bad_to_record,
        "test_rate_stripping",
        "ejection",
        "has to_record 'neighbour'",
    ),
    (
        "duplicate transfer name",
        _duplicate_name,
        "test_rate_supply",
        "infall",
        "is declared twice",
    ),
    ("transfer name not an identifier", _bad_name, "test_rate_supply", "2cool", "C identifier"),
    (
        "slaved_to names no transfer",
        _slaved_unknown,
        "test_rate_feedback",
        "yield",
        "names no transfer declared by a rate module",
    ),
    (
        "slaved_to not <module>.<transfer>",
        _slaved_malformed,
        "test_rate_feedback",
        "yield",
        "<module>.<transfer>",
    ),
    (
        "slaved_to names a source",
        _slaved_to_source,
        "test_rate_feedback",
        "yield",
        "'test_rate_supply.infall', a source",
    ),
    (
        "slaved_to names a slaved source",
        _slaved_to_slaved,
        "test_rate_feedback",
        "second_yield",
        "'test_rate_feedback.yield', a source",
    ),
    (
        "slaved_to whose donor's total is the recipient's",
        _slaved_same_total,
        "test_rate_feedback",
        "yield",
        "the recipient's total",
    ),
    (
        "slaved_to on a transfer out of a reservoir",
        _slaved_on_internal,
        "test_rate_feedback",
        "reheating",
        "only a source (from: external) is slaved",
    ),
    (
        "dependency cycle among totals",
        _total_cycle,
        "test_cycle_a",
        "feed_a",
        "conserved totals ['total_a', 'total_b'] depend on each other in a cycle",
    ),
    (
        "observer property not of kind observer",
        _observer_not_observer,
        "test_rate_formation",
        "star_formation",
        "an observer's property is of reservoir kind 'observer'",
    ),
    (
        "observer names an undeclared transfer",
        _observer_undeclared_transfer,
        "test_rate_formation",
        "cooling",
        "observes a transfer this module does not declare",
    ),
    (
        "rates: without process_rates",
        _rates_without_mode,
        "test_rate_supply",
        "infall",
        "do not include process_rates",
    ),
    (
        "process_rates without rates:",
        _mode_without_rates,
        "test_rate_supply",
        None,
        "advertises process_rates but declares no rates:",
    ),
    (
        "empty transfer list",
        _empty_transfers,
        "test_rate_feedback",
        None,
        "rates.transfers must be a non-empty list",
    ),
]


def run_both_tools(doctor):
    """Doctor fresh copies of the accepted set; return (generator errors, validator errors)."""
    modules = copy.deepcopy(fixture_modules())
    props = copy.deepcopy(fixture_properties())
    doctor(modules, props)
    return generator_errors(modules, props), validator_errors(modules, props)


def assert_refused(case, doctor, module, transfer, fragment):
    """Assert both tools refuse a doctored set, naming the module, the transfer and the reason."""
    gen_errors, val_errors = run_both_tools(doctor)

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
    for case, doctor, module, transfer, fragment in REFUSALS:
        assert_refused(case, doctor, module, transfer, fragment)
    print(f"  ✓ {len(REFUSALS)} refusals raised by both tools")


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
        assert_refused(case, doctor, "test_rate_formation", "star_formation", fragment)
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
        and "'RateFixtureStars', which is not a declared property" in e
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


# ---------------------------------------------------------------------------
# Runner
# ---------------------------------------------------------------------------


def main():
    """Run this file's tests via the shared framework runner."""
    return run_test_suite(
        [
            test_fixture_declarations_accepted,
            test_rate_refusals,
            test_unknown_keys_refused,
            test_validator_exit_codes,
            test_global_index_order,
            test_removed_property_fails_generation,
            test_tables_name_reservoirs_by_symbol,
            test_production_generation_has_no_rate_table,
            test_validator_resolves_master_for_one_module_path,
        ],
        "Rate Metadata (test_rate_metadata.py)",
    )


if __name__ == "__main__":
    sys.exit(main())
