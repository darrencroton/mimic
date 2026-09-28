#!/usr/bin/env python3
"""Generator-level tests for the Mimic unit contract.

These exercise the catalog/parameter/output conversion-expression generator
directly so the non-identity paths are covered without an Uchuu fixture.
"""

import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent.parent.parent / "scripts"))
sys.path.insert(0, str(Path(__file__).parent.parent))

from framework import TestSkipped, run_test_suite
from generate_properties import (
    _effective_h_convention,
    _linear_conversion_expr,
    _unit_info,
    core_property_files,
    generate_catalog_field_metadata_inc,
    generate_raw_halo_defs_h,
    generate_tree_property_accessors_h,
    load_core_metadata,
    merge_property_packages,
    normalize_catalog_contract,
    reference_units_from_core,
)


def test_non_identity_catalog_mass_to_reference_factor():
    expr = _linear_conversion_expr(
        "Msun",
        "free",
        "1e10 Msun/h",
        "carried",
        "test non-identity catalog mass",
    )
    assert expr != "1.0", "non-reference catalog mass must not generate identity conversion"
    assert "MimicConfig.Hubble_h" in expr, "h-free mass must be converted to h-carried reference"
    assert "1e-10" in expr, "Msun to 1e10 Msun/h must include a 1e-10 scale factor"


def test_h_free_length_to_carried_reference_multiplies_by_h():
    # A physical (h-free) Mpc length converted into the h-carried Mpc/h reference
    # is a pure factor of little-h, with no scale change.
    expr = _linear_conversion_expr("Mpc", "free", "Mpc/h", "carried", "test h-free length")
    assert expr == "MimicConfig.Hubble_h", expr


def test_identity_when_label_equals_reference():
    expr = _linear_conversion_expr(
        "1e10 Msun/h", "carried", "1e10 Msun/h", "carried", "test identity mass"
    )
    assert expr == "1.0", expr


def test_velocity_is_h_independent():
    expr = _linear_conversion_expr("km/s", "none", "km/s", "none", "test velocity")
    assert expr == "1.0", expr


def test_specific_angular_momentum_is_identity_when_carried():
    # Pins the Spin units-label contract: the registered "Mpc/h km/s" label
    # converts to itself as a literal "1.0", not merely a numerically-equal
    # expression. That identity rests on two registry-derived facts, pinned
    # here the same way the generator itself resolves them: the label's
    # effective h_convention derives to "carried" (via _effective_h_convention,
    # with no explicit override), and its dimension is
    # "specific_angular_momentum" (via _unit_info). Either drifting would
    # silently break the identity below without failing any other test.
    assert _effective_h_convention({"units": "Mpc/h km/s"}) == "carried"
    assert _unit_info("Mpc/h km/s")["dimension"] == "specific_angular_momentum"

    # The target side of the real generator path is resolved from
    # reference_units, not hardcoded -- pin that resolution too, so an edit to
    # core_properties.yaml's specific_angular_momentum entry (e.g. changing its
    # h_convention) fails this test rather than silently changing Spin values.
    reference_units = reference_units_from_core(load_core_metadata())
    sam_reference = reference_units["specific_angular_momentum"]
    assert sam_reference["label"] == "Mpc/h km/s"
    assert sam_reference["h_convention"] == "carried"

    expr = _linear_conversion_expr(
        "Mpc/h km/s",
        "carried",
        sam_reference["label"],
        sam_reference["h_convention"],
        "test specific angular momentum",
    )
    assert expr == "1.0", expr


def test_time_conversion_is_rejected():
    # The reference time unit is derived (length/velocity); registry-driven time
    # conversion must fail loudly rather than emit a silently wrong factor.
    try:
        _linear_conversion_expr("Gyr", "free", "Myr/h", "carried", "test time")
    except ValueError as exc:
        assert "time" in str(exc).lower(), str(exc)
        return
    raise AssertionError("time conversion must raise ValueError")


def _core_halo_props_and_reference_units():
    """Return (merged halo property map, reference unit dict) from core metadata YAML files."""
    core_meta = load_core_metadata()
    return (
        merge_property_packages(core_property_files(), "halo_properties"),
        reference_units_from_core(core_meta),
    )


def test_required_input_roles_generate_accessors_from_inline_bindings():
    halo_props, reference_units = _core_halo_props_and_reference_units()
    catalog_contract = {
        "path": Path("synthetic/halo_properties.yaml"),
        "catalog_fields": [
            {
                "name": "Desc",
                "type": "int",
                "units": "dimensionless",
                "provides_core_role": "Descendant",
            },
            {
                "name": "FirstProg",
                "type": "int",
                "units": "dimensionless",
                "provides_core_role": "FirstProgenitor",
            },
            {
                "name": "NextProg",
                "type": "int",
                "units": "dimensionless",
                "provides_core_role": "NextProgenitor",
            },
            {
                "name": "FirstFOF",
                "type": "int",
                "units": "dimensionless",
                "provides_core_role": "FirstHaloInFOFgroup",
            },
            {
                "name": "NextFOF",
                "type": "int",
                "units": "dimensionless",
                "provides_core_role": "NextHaloInFOFgroup",
            },
            {
                "name": "Snap",
                "type": "int",
                "units": "dimensionless",
                "provides_core_role": "SnapNum",
            },
            {
                "name": "NPart",
                "type": "int",
                "units": "particles",
                "provides_core_role": "Len",
            },
            {
                "name": "Mass200",
                "type": "float",
                "units": "1e10 Msun/h",
                "h_convention": "carried",
                "provides_core_role": "HaloMass",
            },
        ],
    }

    catalog_info = normalize_catalog_contract(halo_props, catalog_contract, reference_units)
    accessors = generate_tree_property_accessors_h(halo_props, catalog_info, "0" * 32)

    # Pins the calling convention as well as the array source: without the
    # signature assertion, reverting the accessors to (int halonr) over a
    # file-scope `view` object would still satisfy every check below.
    assert (
        "int64_t mimic_tree_get_FirstProgenitor(struct HaloInputView view, int64_t halonr)"
        in accessors
    )
    assert "view.halos[halonr].FirstProg" in accessors
    assert "mimic_tree_get_SnapNum" in accessors
    assert "view.halos[halonr].Snap" in accessors
    assert "mimic_tree_get_Len" in accessors
    assert "view.halos[halonr].NPart" in accessors
    assert "mimic_tree_get_HaloMass" in accessors
    assert "view.halos[halonr].Mass200" in accessors


def test_tree_link_core_roles_reject_non_integer_catalog_fields():
    halo_props, reference_units = _core_halo_props_and_reference_units()
    catalog_contract = {
        "path": Path("synthetic/halo_properties.yaml"),
        "catalog_fields": [
            {
                "name": "Desc",
                "type": "float",
                "units": "dimensionless",
                "provides_core_role": "Descendant",
            },
            {
                "name": "FirstProgenitor",
                "type": "int",
                "units": "dimensionless",
                "provides_core_role": "FirstProgenitor",
            },
            {
                "name": "NextProgenitor",
                "type": "int",
                "units": "dimensionless",
                "provides_core_role": "NextProgenitor",
            },
            {
                "name": "FirstHaloInFOFgroup",
                "type": "int",
                "units": "dimensionless",
                "provides_core_role": "FirstHaloInFOFgroup",
            },
            {
                "name": "NextHaloInFOFgroup",
                "type": "int",
                "units": "dimensionless",
                "provides_core_role": "NextHaloInFOFgroup",
            },
            {
                "name": "SnapNum",
                "type": "int",
                "units": "dimensionless",
                "provides_core_role": "SnapNum",
            },
            {"name": "Len", "type": "int", "units": "particles", "provides_core_role": "Len"},
            {
                "name": "M_Crit200",
                "type": "float",
                "units": "1e10 Msun/h",
                "h_convention": "carried",
                "provides_core_role": "HaloMass",
            },
        ],
    }

    try:
        normalize_catalog_contract(halo_props, catalog_contract, reference_units)
    except ValueError as exc:
        assert "core role 'Descendant' requires an int or long long catalog field" in str(exc)
        return
    raise AssertionError("non-integer tree-link role must raise ValueError")


# Core role -> synthetic catalog field name. The names deliberately differ from the
# role names so the generated accessors are seen to read the bound field.
_SYNTHETIC_ROLE_FIELDS = {
    "Descendant": "Desc",
    "FirstProgenitor": "FirstProg",
    "NextProgenitor": "NextProg",
    "FirstHaloInFOFgroup": "FirstFOF",
    "NextHaloInFOFgroup": "NextFOF",
    "SnapNum": "Snap",
    "Len": "NPart",
    "HaloMass": "Mass200",
}
_TREE_LINK_ROLES = (
    "Descendant",
    "FirstProgenitor",
    "NextProgenitor",
    "FirstHaloInFOFgroup",
    "NextHaloInFOFgroup",
)


def _synthetic_role_catalog(link_type="int", role_types=None):
    """Return a catalog contract binding every core role, links stored as `link_type`.

    `role_types` overrides the storage type of individual roles by role name.
    """
    role_types = role_types or {}
    fields = []
    for role, name in _SYNTHETIC_ROLE_FIELDS.items():
        if role == "HaloMass":
            field = {"type": "float", "units": "1e10 Msun/h", "h_convention": "carried"}
        elif role == "Len":
            field = {"type": "int", "units": "particles"}
        elif role == "SnapNum":
            field = {"type": "int", "units": "dimensionless"}
        else:
            field = {"type": link_type, "units": "dimensionless"}
        field["type"] = role_types.get(role, field["type"])
        fields.append({"name": name, **field, "provides_core_role": role})
    return {"path": Path("synthetic/halo_properties.yaml"), "catalog_fields": fields}


def _expect_role_type_error(catalog_contract, expected_message):
    halo_props, reference_units = _core_halo_props_and_reference_units()
    try:
        normalize_catalog_contract(halo_props, catalog_contract, reference_units)
    except ValueError as exc:
        assert expected_message in str(exc), str(exc)
        return
    raise AssertionError(f"expected ValueError containing {expected_message!r}")


def test_tree_link_core_roles_accept_long_long_catalog_fields():
    # R0-2(a): a wide horizontal package stores its links as long long; the raw
    # record keeps that storage and every link accessor returns int64_t.
    halo_props, reference_units = _core_halo_props_and_reference_units()
    catalog_info = normalize_catalog_contract(
        halo_props, _synthetic_role_catalog("long long"), reference_units
    )
    accessors = generate_tree_property_accessors_h(halo_props, catalog_info, "0" * 32)
    raw_halo = generate_raw_halo_defs_h(catalog_info, "0" * 32)

    for role in _TREE_LINK_ROLES:
        field = _SYNTHETIC_ROLE_FIELDS[role]
        assert f"  long long {field};" in raw_halo, raw_halo
        assert (
            f"static inline int64_t mimic_tree_get_{role}"
            "(struct HaloInputView view, int64_t halonr)" in accessors
        ), accessors
        assert f"return (int64_t)(view.halos[halonr].{field});" in accessors, accessors
    # Index and count roles keep their int return type; only the index widens.
    assert (
        "static inline int mimic_tree_get_SnapNum(struct HaloInputView view, int64_t halonr)"
        in (accessors)
    )
    assert "static inline int mimic_tree_get_Len(struct HaloInputView view, int64_t halonr)" in (
        accessors
    )


def test_catalog_field_metadata_carries_compiled_declarations():
    # The horizontal-HDF5 v3 reader compares a file's /schema against this
    # table, so each entry must carry the on-disk dataset name, the declared
    # type and units, the *effective* h_convention (explicit, else derived from
    # the unit label, exactly as every conversion resolves it) and the core
    # role with its kind -- in catalog declaration order.
    halo_props, reference_units = _core_halo_props_and_reference_units()
    catalog = _synthetic_role_catalog("long long")
    catalog["catalog_fields"].append(
        {"name": "Extra", "source": "ExtraOnDisk", "type": "vec3_float", "units": "Mpc/h"}
    )
    catalog_info = normalize_catalog_contract(halo_props, catalog, reference_units)
    table = generate_catalog_field_metadata_inc(catalog_info, "0" * 32)
    entries = [line for line in table.splitlines() if line.startswith("CATALOG_FIELD(")]

    assert len(entries) == len(catalog["catalog_fields"]), table
    assert entries[0] == (
        'CATALOG_FIELD(Desc, "Desc", "long long", "dimensionless", "none", "Descendant", '
        '"tree_link")'
    ), entries[0]
    assert (
        'CATALOG_FIELD(Mass200, "Mass200", "float", "1e10 Msun/h", "carried", "HaloMass", '
        '"mass")' in entries
    ), table
    npart = 'CATALOG_FIELD(NPart, "NPart", "int", "particles", "none", "Len", "count")'
    assert npart in entries, table
    # No explicit h_convention: Mpc/h derives "carried" from the unit registry;
    # the dataset name is the entry's `source`, the member its `name`.
    assert entries[-1] == (
        'CATALOG_FIELD(Extra, "ExtraOnDisk", "vec3_float", "Mpc/h", "carried", "", "")'
    ), entries[-1]


def test_int_tree_links_still_generate_int64_accessors():
    # Vertical packages keep int storage (R0-2(a)); the accessor widens it.
    halo_props, reference_units = _core_halo_props_and_reference_units()
    catalog_info = normalize_catalog_contract(
        halo_props, _synthetic_role_catalog("int"), reference_units
    )
    accessors = generate_tree_property_accessors_h(halo_props, catalog_info, "0" * 32)
    raw_halo = generate_raw_halo_defs_h(catalog_info, "0" * 32)

    assert "  int Desc;" in raw_halo, raw_halo
    for role in _TREE_LINK_ROLES:
        assert (
            f"static inline int64_t mimic_tree_get_{role}"
            "(struct HaloInputView view, int64_t halonr)" in accessors
        ), accessors


def test_tree_link_core_roles_reject_every_other_type():
    for bad_type in ("float", "double", "vec3_int", "vec3_float"):
        _expect_role_type_error(
            _synthetic_role_catalog("int", {"NextProgenitor": bad_type}),
            "core role 'NextProgenitor' requires an int or long long catalog field, "
            f"but 'NextProg' has type '{bad_type}'",
        )


def test_index_and_count_core_roles_reject_long_long():
    # Only tree links may widen their storage; SnapNum (index) and Len (count)
    # stay int, so long long -- and every other non-int type -- is still rejected.
    for bad_type in ("long long", "float", "double", "vec3_int"):
        _expect_role_type_error(
            _synthetic_role_catalog("long long", {"SnapNum": bad_type}),
            f"core role 'SnapNum' requires an int catalog field, but 'Snap' has type '{bad_type}'",
        )
        _expect_role_type_error(
            _synthetic_role_catalog("long long", {"Len": bad_type}),
            f"core role 'Len' requires an int catalog field, but 'NPart' has type '{bad_type}'",
        )


# A C driver for the generated accessors over long long link storage. Every link
# value sits above INT32_MAX (or is the -1 sentinel), so a narrowing anywhere
# between the stored field and the returned value changes what is printed.
_WIDE_LINK_PROGRAM = r"""
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "tree_property_accessors.h"

#define IS_INT64(expr) _Generic((expr), int64_t: 1, default: 0)

int main(void) {
  struct RawHalo halos[2];
  memset(halos, 0, sizeof(halos));
  halos[1].Desc = 3000000000LL;
  halos[1].FirstProg = -1LL;
  halos[1].NextProg = 4294967296LL + 5;
  halos[1].FirstFOF = 9007199254740993LL;
  halos[1].NextFOF = 2147483648LL;

  const struct HaloInputView view = {halos, 2};
  const int64_t index = 1;
  if (!IS_INT64(mimic_tree_get_Descendant(view, index))) {
    return 2;
  }
  printf("%" PRId64 " %" PRId64 " %" PRId64 " %" PRId64 " %" PRId64 "\n",
         mimic_tree_get_Descendant(view, index), mimic_tree_get_FirstProgenitor(view, index),
         mimic_tree_get_NextProgenitor(view, index),
         mimic_tree_get_FirstHaloInFOFgroup(view, index),
         mimic_tree_get_NextHaloInFOFgroup(view, index));
  return 0;
}
"""

# Stand-in for globals.h: only the two types the generated accessors need.
_WIDE_LINK_GLOBALS = """
#include <stdint.h>
#include "raw_halo_defs.h"
struct HaloInputView {
  const struct RawHalo *halos;
  int64_t count;
};
"""


def test_long_long_link_accessors_round_trip_values_above_int32():
    # No committed package stores long long links yet, so a C unit test cannot put
    # a link above 2^31 into a real RawHalo. Generate the synthetic package's
    # struct and accessors, compile them warning-free with conversion warnings as
    # errors, and check every link value above INT32_MAX comes back exactly.
    compiler = os.environ.get("CC", "cc")
    if shutil.which(compiler) is None:
        raise TestSkipped(f"C compiler '{compiler}' not found")

    halo_props, reference_units = _core_halo_props_and_reference_units()
    catalog_info = normalize_catalog_contract(
        halo_props, _synthetic_role_catalog("long long"), reference_units
    )
    with tempfile.TemporaryDirectory(prefix="mimic_wide_links_") as tmp:
        tmp_path = Path(tmp)
        (tmp_path / "raw_halo_defs.h").write_text(generate_raw_halo_defs_h(catalog_info, "0" * 32))
        (tmp_path / "tree_property_accessors.h").write_text(
            generate_tree_property_accessors_h(halo_props, catalog_info, "0" * 32)
        )
        (tmp_path / "globals.h").write_text(_WIDE_LINK_GLOBALS)
        (tmp_path / "main.c").write_text(_WIDE_LINK_PROGRAM)
        exe = tmp_path / "wide_links"
        compile_cmd = [
            compiler,
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-Wconversion",
            "-Werror",
            f"-I{tmp_path}",
            str(tmp_path / "main.c"),
            "-o",
            str(exe),
        ]
        built = subprocess.run(compile_cmd, capture_output=True, text=True)
        assert built.returncode == 0, built.stdout + built.stderr
        ran = subprocess.run([str(exe)], capture_output=True, text=True)
        assert ran.returncode == 0, f"exit {ran.returncode}: {ran.stdout}{ran.stderr}"
        assert ran.stdout.split() == [
            "3000000000",
            "-1",
            "4294967301",
            "9007199254740993",
            "2147483648",
        ], ran.stdout


def test_provides_core_role_must_be_non_empty_string():
    halo_props, reference_units = _core_halo_props_and_reference_units()
    catalog_contract = {
        "path": Path("synthetic/halo_properties.yaml"),
        "catalog_fields": [
            {
                "name": "Descendant",
                "type": "int",
                "units": "dimensionless",
                "provides_core_role": ["Descendant"],
            }
        ],
    }

    try:
        normalize_catalog_contract(halo_props, catalog_contract, reference_units)
    except ValueError as exc:
        assert "invalid provides_core_role" in str(exc)
        return
    raise AssertionError("non-string provides_core_role must raise ValueError")


def main():
    return run_test_suite(
        [
            test_non_identity_catalog_mass_to_reference_factor,
            test_h_free_length_to_carried_reference_multiplies_by_h,
            test_identity_when_label_equals_reference,
            test_velocity_is_h_independent,
            test_specific_angular_momentum_is_identity_when_carried,
            test_time_conversion_is_rejected,
            test_required_input_roles_generate_accessors_from_inline_bindings,
            test_tree_link_core_roles_reject_non_integer_catalog_fields,
            test_tree_link_core_roles_accept_long_long_catalog_fields,
            test_catalog_field_metadata_carries_compiled_declarations,
            test_int_tree_links_still_generate_int64_accessors,
            test_tree_link_core_roles_reject_every_other_type,
            test_index_and_count_core_roles_reject_long_long,
            test_long_long_link_accessors_round_trip_values_above_int32,
            test_provides_core_role_must_be_non_empty_string,
        ],
        "Unit Contract Generation (test_unit_contract_generation.py)",
    )


if __name__ == "__main__":
    sys.exit(main())
