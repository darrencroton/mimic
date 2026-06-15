#!/usr/bin/env python3
"""Generator-level tests for the Mimic unit contract.

These exercise the catalog/parameter/output conversion-expression generator
directly so the non-identity paths are covered without an Uchuu fixture.
"""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent.parent.parent / "scripts"))
sys.path.insert(0, str(Path(__file__).parent.parent))

from framework import run_test_suite
from generate_properties import (
    _linear_conversion_expr,
    core_property_files,
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

    assert "mimic_tree_get_FirstProgenitor" in accessors
    assert "InputTreeHalos[halonr].FirstProg" in accessors
    assert "mimic_tree_get_SnapNum" in accessors
    assert "InputTreeHalos[halonr].Snap" in accessors
    assert "mimic_tree_get_Len" in accessors
    assert "InputTreeHalos[halonr].NPart" in accessors
    assert "mimic_tree_get_HaloMass" in accessors
    assert "InputTreeHalos[halonr].Mass200" in accessors


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
        assert "core role 'Descendant' requires an int catalog field" in str(exc)
        return
    raise AssertionError("non-integer tree-link role must raise ValueError")


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
            test_time_conversion_is_rejected,
            test_required_input_roles_generate_accessors_from_inline_bindings,
            test_tree_link_core_roles_reject_non_integer_catalog_fields,
            test_provides_core_role_must_be_non_empty_string,
        ],
        "Unit Contract Generation (test_unit_contract_generation.py)",
    )


if __name__ == "__main__":
    sys.exit(main())
