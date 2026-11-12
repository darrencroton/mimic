#!/usr/bin/env python3
"""
Reservoir metadata: the `reservoir:` property key and the generated reservoir table.

Validates:
  - test_reservoir_refusals: every refusal of the `reservoir:` mapping is raised by
    the generator's own validation, called in-process on synthetic property sets,
    with a message naming the offending property; the valid set every row doctors
    is itself accepted, so each row fails for its own reason
  - test_reservoir_table_compiles_clean: the generated reservoir_table.h compiles
    under the project warnings with -Werror, both for a property set with no
    `reservoir:` mapping (counts of zero, NULL accessors) and for one with a
    reservoir, a tracer and an observer
  - test_fixture_fields_in_binary_output / test_fixture_fields_in_hdf5_output: in a
    test build, a pipeline that does not touch the rate fixture writes its seven
    properties last in the output schema, with units and descriptions, as doubles
    holding 0 in every row, and the core scientific tier's range and zero-value
    checks report nothing for them

The table's contents in a test build (names, kinds, parent and total indices,
offsets) are checked by tests/unit/test_reservoir_table.c.
"""

import importlib.util
import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(REPO_ROOT / "scripts"))
sys.path.insert(0, str(REPO_ROOT / "tests"))

from framework import (  # noqa: E402  (path set up above)
    MIMIC_EXE,
    VALIDATION_MANIFEST_PATH,
    SharedRuns,
    TestSkipped,
    check_declared_ranges,
    load_binary_halos,
    load_hdf5_halos,
    load_validation_manifest,
    not_applicable_if_selected_package_is_horizontal,
    run_test_suite,
)
from framework.harness import fixture_output_field_names  # noqa: E402
from generate_properties import (  # noqa: E402
    core_property_files,
    generate_property_defs_h,
    generate_reservoir_table_h,
    load_core_metadata,
    load_simulation_catalog_contract,
    merge_property_lists,
    merge_property_packages,
    normalize_catalog_contract,
    reference_units_from_core,
    validate_properties,
)

# The project's warning set (Makefile CFLAGS), made fatal
PROJECT_WARNINGS = ["-Wall", "-Wextra", "-Wshadow", "-Wformat-security", "-Wundef", "-Werror"]

# The rate fixture's seven properties, in test_properties.yaml declaration order
FIXTURE_FIELDS = (
    "RateFixtureHot",
    "RateFixtureCold",
    "RateFixtureStars",
    "RateFixtureHotTracer",
    "RateFixtureColdTracer",
    "RateFixtureStarsTracer",
    "RateFixtureFormed",
)

RUNS = SharedRuns()


# ==============================================================================
# Refusals
# ==============================================================================


def _galaxy(name, reservoir=None, **overrides):
    """A valid double galaxy property, optionally with a `reservoir:` mapping."""
    prop = {
        "name": name,
        "type": "double",
        "units": "1e10 Msun/h",
        "description": f"synthetic {name}",
        "output": True,
        "init_source": "default",
        "init_value": 0.0,
        "output_source": "galaxy_property",
    }
    if reservoir is not None:
        prop["reservoir"] = reservoir
    prop.update(overrides)
    return prop


def _valid_set():
    """Two reservoirs, a tracer of each in its own total, and an observer."""
    return [
        _galaxy("Hot", {"kind": "reservoir", "total": "mass"}),
        _galaxy("Cold", {"kind": "reservoir", "total": "mass"}),
        _galaxy("HotMetals", {"kind": "tracer", "total": "metals", "parent": "Hot"}),
        _galaxy("ColdMetals", {"kind": "tracer", "total": "metals", "parent": "Cold"}),
        _galaxy("Formed", {"kind": "observer"}),
    ]


def _with(name, prop):
    """The valid set with property `name` replaced by `prop` (appended if absent)."""
    props = [p for p in _valid_set() if p["name"] != name]
    return props + [prop]


def _core_halo_property():
    """A real core halo property, as validate_properties() receives it."""
    return dict(merge_property_packages(core_property_files(), "halo_properties")[0])


def _selected_halo_properties():
    """The selected package's halo properties, assembled as the generator's main() does."""
    core = merge_property_packages(core_property_files(), "halo_properties")
    catalog = load_simulation_catalog_contract()["catalog_fields"]
    copied = [
        prop
        for prop in catalog
        if prop.get("init_source") in ("copy_from_tree", "copy_from_tree_array")
    ]
    return merge_property_lists(core, copied)


def _validate(galaxy_props, halo_props=()):
    return lambda: validate_properties(list(halo_props), galaxy_props)


def _catalog_only_with_reservoir():
    """Refuse a reservoir mapping on a catalog-only field of the selected simulation."""
    contract = load_simulation_catalog_contract()
    fields = [dict(field) for field in contract["catalog_fields"]]
    target = next(
        field
        for field in fields
        if field.get("init_source") not in ("copy_from_tree", "copy_from_tree_array")
    )
    target["reservoir"] = {"kind": "reservoir", "total": "mass"}
    reference_units = reference_units_from_core(load_core_metadata())
    catalog = {"path": contract["path"], "catalog_fields": fields}
    return target["name"], lambda: normalize_catalog_contract([], catalog, reference_units)


def _bad_total_names():
    """(label, property, total, mapping) for each total name that is not identifier-like."""
    rows = []
    for label, total in [
        ("with a trailing space", "mass "),
        ("with an inner space", "gas mass"),
        ("with a control character", "mass\x01"),
        ("with a non-BMP character", "mass\U0001f600"),
        ("starting with a digit", "2mass"),
        ("with a hyphen", "gas-mass"),
        ("ending in a newline", "mass\n"),
    ]:
        rows.append(
            (f"{label} on a reservoir", "Cold", total, {"kind": "reservoir", "total": total})
        )
    rows.append(
        (
            "with a space on a tracer",
            "HotMetals",
            "metal mass",
            {"kind": "tracer", "total": "metal mass", "parent": "Hot"},
        )
    )
    return rows


def _refusal_rows():
    """(label, property the message must name, message fragment, call) for each refusal."""
    core = _core_halo_property()
    core["reservoir"] = {"kind": "reservoir", "total": "mass"}
    catalog_name, catalog_call = _catalog_only_with_reservoir()
    tracer = {"kind": "tracer", "total": "metals", "parent": "Hot"}
    return [
        (
            "unknown key in the mapping",
            "Hot",
            "unknown reservoir: key(s) ['rate']",
            _validate(
                _with("Hot", _galaxy("Hot", {"kind": "reservoir", "total": "mass", "rate": 1}))
            ),
        ),
        (
            "the mapping on a core halo property",
            core["name"],
            "is a halo property",
            _validate(_valid_set(), [core]),
        ),
        (
            "the mapping on a simulation halo property",
            "SimHalo",
            "is a halo property",
            _validate(_valid_set(), [_galaxy("SimHalo", {"kind": "observer"})]),
        ),
        (
            "the mapping on a catalog-only simulation field",
            catalog_name,
            "is a halo property",
            catalog_call,
        ),
        (
            "type float",
            "Hot",
            "must be a scalar double",
            _validate(
                _with("Hot", _galaxy("Hot", {"kind": "reservoir", "total": "mass"}, type="float"))
            ),
        ),
        (
            "type vec3_float",
            "Formed",
            "must be a scalar double",
            _validate(_with("Formed", _galaxy("Formed", {"kind": "observer"}, type="vec3_float"))),
        ),
        (
            "init_repeat on a reservoir",
            "Cold",
            "init_repeat: true",
            _validate(
                _with(
                    "Cold",
                    _galaxy("Cold", {"kind": "reservoir", "total": "mass"}, init_repeat=True),
                )
            ),
        ),
        (
            "init_repeat on a tracer",
            "HotMetals",
            "init_repeat: true",
            _validate(_with("HotMetals", _galaxy("HotMetals", tracer, init_repeat=True))),
        ),
        (
            "a tracer whose parent is missing",
            "HotMetals",
            "which is not a galaxy property",
            _validate(
                _with("HotMetals", _galaxy("HotMetals", {**tracer, "parent": "Warm"})),
            ),
        ),
        (
            "a tracer whose parent is an observer",
            "HotMetals",
            "which is not a reservoir",
            _validate(_with("HotMetals", _galaxy("HotMetals", {**tracer, "parent": "Formed"}))),
        ),
        (
            "a tracer whose parent declares no mapping",
            "HotMetals",
            "which is not a reservoir",
            _validate(
                _with("HotMetals", _galaxy("HotMetals", {**tracer, "parent": "Plain"}))
                + [_galaxy("Plain")]
            ),
        ),
        (
            "a tracer that shares its parent's total",
            "HotMetals",
            "the total of its parent 'Hot'",
            _validate(_with("HotMetals", _galaxy("HotMetals", {**tracer, "total": "mass"}))),
        ),
        (
            "two tracers of one parent in one total",
            "HotMetalsAgain",
            "second tracer of 'Hot'",
            _validate(_valid_set() + [_galaxy("HotMetalsAgain", tracer)]),
        ),
        (
            "a total holding reservoirs and tracers",
            "Stars",
            "holds reservoirs or tracers, not both",
            _validate(_valid_set() + [_galaxy("Stars", {"kind": "reservoir", "total": "metals"})]),
        ),
        (
            "two members of one total with different units",
            "Cold",
            "has units '1e10 Msun'",
            _validate(
                _with(
                    "Cold",
                    _galaxy("Cold", {"kind": "reservoir", "total": "mass"}, units="1e10 Msun"),
                )
            ),
        ),
        (
            "a mapping that is not a mapping",
            "Hot",
            "not a mapping",
            _validate(_with("Hot", _galaxy("Hot", "reservoir"))),
        ),
        (
            "an unknown kind",
            "Hot",
            "reservoir kind 'store'",
            _validate(_with("Hot", _galaxy("Hot", {"kind": "store", "total": "mass"}))),
        ),
        (
            "a reservoir with no total",
            "Hot",
            "needs a non-empty string total",
            _validate(_with("Hot", _galaxy("Hot", {"kind": "reservoir"}))),
        ),
        *[
            (
                f"a total name {label}",
                property_name,
                f"names total {total!r}",
                _validate(_with(property_name, _galaxy(property_name, mapping))),
            )
            for label, property_name, total, mapping in _bad_total_names()
        ],
        (
            "an observer with a total",
            "Formed",
            "is an observer and declares a total",
            _validate(_with("Formed", _galaxy("Formed", {"kind": "observer", "total": "mass"}))),
        ),
        (
            "a tracer with no parent",
            "HotMetals",
            "needs a non-empty string parent",
            _validate(
                _with("HotMetals", _galaxy("HotMetals", {"kind": "tracer", "total": "metals"}))
            ),
        ),
        (
            "a reservoir with a parent",
            "Cold",
            "only a tracer has one",
            _validate(
                _with(
                    "Cold",
                    _galaxy("Cold", {"kind": "reservoir", "total": "mass", "parent": "Hot"}),
                )
            ),
        ),
    ]


def test_reservoir_refusals():
    """Each refusal is raised in-process with a message naming the property."""
    # The baseline every row doctors is accepted, so a row cannot pass by accident
    validate_properties([], _valid_set())
    validate_properties(
        [], _valid_set() + [_galaxy("Formed2", {"kind": "observer"}, init_repeat=True)]
    )

    failures = []
    for label, name, fragment, call in _refusal_rows():
        try:
            call()
        except ValueError as exc:
            message = str(exc)
            if f"'{name}'" not in message or fragment not in message:
                failures.append(
                    f"{label}: message does not name '{name}' with {fragment!r}: {message}"
                )
            continue
        failures.append(f"{label}: no ValueError raised")
    assert not failures, "\n".join(failures)
    print(f"  {len(_refusal_rows())} refusals raised, each naming its property")


# ==============================================================================
# The generated header compiles clean
# ==============================================================================

_EMPTY_PROGRAM = """
#include <stdio.h>
#include "reservoir_table.h"

int main(void) {
  const struct ReservoirEntry *table = mimic_reservoir_table();
  const char *const *totals = mimic_reservoir_total_names();
  printf("%d %d %d %d\\n", MIMIC_RESERVOIR_COUNT, MIMIC_RESERVOIR_TOTAL_COUNT, table == NULL,
         totals == NULL);
  return 0;
}
"""

_DECLARED_PROGRAM = """
#include <stdio.h>
#include "reservoir_table.h"

int main(void) {
  const struct ReservoirEntry *table = mimic_reservoir_table();
  const char *const *totals = mimic_reservoir_total_names();
  printf("%d %d", MIMIC_RESERVOIR_COUNT, MIMIC_RESERVOIR_TOTAL_COUNT);
  for (int i = 0; i < MIMIC_RESERVOIR_COUNT; i++) {
    size_t expected = offsetof(struct GalaxyData, Plain) + sizeof(double) * (size_t)(i + 1);
    printf(" %s:%d:%d:%d:%d", table[i].name, (int)table[i].kind, table[i].parent, table[i].total,
           table[i].offset == expected);
  }
  for (int i = 0; i < MIMIC_RESERVOIR_TOTAL_COUNT; i++) {
    printf(" %s", totals[i]);
  }
  printf(" %d\\n", RESERVOIR_INDEX_Formed);
  return 0;
}
"""


def _compile_and_run(galaxy_props, program, tmp_path):
    """Generate property_defs.h and reservoir_table.h, compile `program` with -Werror, run it."""
    halo_props = _selected_halo_properties()
    (tmp_path / "property_defs.h").write_text(
        generate_property_defs_h(halo_props, galaxy_props, "0" * 32)
    )
    (tmp_path / "reservoir_table.h").write_text(generate_reservoir_table_h(galaxy_props, "0" * 32))
    (tmp_path / "main.c").write_text(program)
    exe = tmp_path / "reservoir_table"
    compiler = os.environ.get("CC", "cc")
    command = [
        compiler,
        *PROJECT_WARNINGS,
        f"-I{tmp_path}",
        str(tmp_path / "main.c"),
        "-o",
        str(exe),
    ]
    built = subprocess.run(command, capture_output=True, text=True)
    assert built.returncode == 0, f"{' '.join(command)}\n{built.stdout}{built.stderr}"
    assert not built.stderr.strip(), f"compiler diagnostics:\n{built.stderr}"
    ran = subprocess.run([str(exe)], capture_output=True, text=True)
    assert ran.returncode == 0, f"exit {ran.returncode}: {ran.stdout}{ran.stderr}"
    return ran.stdout.split()


def test_reservoir_table_compiles_clean():
    """The header compiles warning-free with no declaration and with every kind declared."""
    compiler = os.environ.get("CC", "cc")
    if shutil.which(compiler) is None:
        raise TestSkipped(f"C compiler '{compiler}' not found")

    # Plain first, then the declared properties: each double follows the previous
    # one with no padding, so entry i sits (i + 1) doubles after Plain.
    declared = [
        _galaxy("Plain"),
        _galaxy("Hot", {"kind": "reservoir", "total": "mass"}),
        _galaxy("HotMetals", {"kind": "tracer", "total": "metals", "parent": "Hot"}),
        _galaxy("Formed", {"kind": "observer"}),
    ]
    rows = [
        (
            "no reservoir: mapping",
            [_galaxy("Plain"), _galaxy("Other")],
            _EMPTY_PROGRAM,
            ["0", "0", "1", "1"],
        ),
        (
            "a reservoir, a tracer and an observer",
            declared,
            _DECLARED_PROGRAM,
            [
                "3",
                "2",
                "Hot:0:-1:0:1",
                "HotMetals:1:0:1:1",
                "Formed:2:-1:-1:1",
                "mass",
                "metals",
                "2",
            ],
        ),
    ]
    with tempfile.TemporaryDirectory(prefix="mimic_reservoir_table_") as tmp:
        for label, props, program, expected in rows:
            row_dir = Path(tmp) / label.replace(" ", "_").replace(":", "").replace(",", "")
            row_dir.mkdir()
            validate_properties([], props)
            got = _compile_and_run(props, program, row_dir)
            assert got == expected, f"{label}: printed {got}, expected {expected}"
            print(f"  {label}: compiled clean under {' '.join(PROJECT_WARNINGS)}")


# ==============================================================================
# The fixture properties in test-build output
# ==============================================================================


def _require_test_build():
    """Skip unless ./mimic is a test build: only a test build carries the fixture."""
    if not MIMIC_EXE.exists():
        raise TestSkipped("Mimic not built")
    marker = REPO_ROOT / "build" / ".last_exec_mode"
    if not marker.exists() or "TEST_BUILD=yes" not in marker.read_text().splitlines():
        raise TestSkipped("./mimic is not a test build: build it with make TEST_BUILD=yes")


def _scientific_check_zeros():
    """The core scientific tier's zero-value check (tests/scientific/test_scientific.py)."""
    path = REPO_ROOT / "tests" / "scientific" / "test_scientific.py"
    spec = importlib.util.spec_from_file_location("core_scientific_checks", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module.check_zeros


def _check_fixture_output(output_dir, files, loader):
    """Assert the fixture fields are last in the schema and zero, in range and zero-exempt."""
    assert (
        fixture_output_field_names() == FIXTURE_FIELDS
    ), f"harness fixture names {fixture_output_field_names()} differ from {FIXTURE_FIELDS}"

    schema = json.loads((output_dir / "metadata" / "output_schema.json").read_text())
    tail = schema["fields"][-len(FIXTURE_FIELDS) :]
    assert (
        tuple(field["name"] for field in tail) == FIXTURE_FIELDS
    ), f"the schema's last fields are {[field['name'] for field in tail]}"
    for field in tail:
        assert field["c_type"] == "double", f"{field['name']}: c_type {field['c_type']}"
        assert field["units"] == "1e10 Msun/h", f"{field['name']}: units {field['units']}"
        assert field["description"].startswith(
            "Rate fixture"
        ), f"{field['name']}: description {field['description']!r}"

    if not VALIDATION_MANIFEST_PATH.exists():
        raise TestSkipped("validation manifest not found: run 'make TEST_BUILD=yes generate'")
    properties = load_validation_manifest().get("properties", {})
    fixture_specs = {name: properties.get(name) for name in FIXTURE_FIELDS}
    missing = [name for name, spec in fixture_specs.items() if spec is None]
    assert (
        not missing
    ), f"validation manifest lacks {missing}: it was not generated for a test build"

    check_zeros = _scientific_check_zeros()
    assert files, f"no output partition in {output_dir}"
    rows = 0
    for path in files:
        halos, _metadata = loader(path)
        assert (
            halos.dtype.names[-len(FIXTURE_FIELDS) :] == FIXTURE_FIELDS
        ), f"{path.name}: last fields {halos.dtype.names[-len(FIXTURE_FIELDS):]}"
        rows += len(halos)
        for name in FIXTURE_FIELDS:
            values = halos[name]
            assert (
                values.dtype.kind == "f" and values.dtype.itemsize == 8
            ), f"{path.name}: {name} is {values.dtype}, not a double"
            assert (values == 0.0).all(), f"{path.name}: {name} holds a non-zero value"

        results = {
            result["field"]: result
            for result in check_declared_ranges(halos, properties)
            if result["field"] in FIXTURE_FIELDS
        }
        not_passed = {name: results.get(name, {}).get("status") for name in FIXTURE_FIELDS}
        not_passed = {name: status for name, status in not_passed.items() if status != "pass"}
        assert not not_passed, f"{path.name}: core range check did not pass {not_passed}"

        zeros = check_zeros(halos, {"properties": properties})
        flagged = sorted(set(zeros) & set(FIXTURE_FIELDS))
        assert not flagged, f"{path.name}: core zero-value check flags {flagged}"
    assert rows > 0, f"no rows in {output_dir}: the checks above saw nothing"
    print(f"  {len(files)} file(s), {rows} rows: the seven fixture fields are last, 0 and in range")


def test_fixture_fields_in_binary_output():
    """Binary output of an empty pipeline carries the seven fixture fields, all zero."""
    _require_test_build()
    not_applicable_if_selected_package_is_horizontal("binary galaxy output")
    run = RUNS.run("reservoir_fixture_binary", output_format="binary")
    assert run.returncode == 0, f"Mimic failed ({run.returncode}):\n{run.stdout}{run.stderr}"
    files = sorted(path for path in run.output_dir.glob("model_z*") if path.is_file())
    _check_fixture_output(run.output_dir, files, load_binary_halos)


def test_fixture_fields_in_hdf5_output():
    """HDF5 output of an empty pipeline carries the seven fixture fields, all zero."""
    _require_test_build()
    run = RUNS.run("reservoir_fixture_hdf5", output_format="hdf5")
    if run.returncode != 0:
        text = (run.stdout + run.stderr).lower()
        if "hdf5" in text and any(
            phrase in text for phrase in ("not compiled", "not supported", "unknown")
        ):
            raise TestSkipped("Mimic was built without HDF5")
    assert run.returncode == 0, f"Mimic failed ({run.returncode}):\n{run.stdout}{run.stderr}"
    files = sorted(run.output_dir.glob("model_[0-9][0-9][0-9].hdf5"))
    _check_fixture_output(run.output_dir, files, load_hdf5_halos)


def main():
    try:
        return run_test_suite(
            [
                test_reservoir_refusals,
                test_reservoir_table_compiles_clean,
                test_fixture_fields_in_binary_output,
                test_fixture_fields_in_hdf5_output,
            ],
            "Reservoir Metadata (test_reservoir_metadata.py)",
        )
    finally:
        RUNS.cleanup()


if __name__ == "__main__":
    raise SystemExit(main())
