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
from generate_properties import _linear_conversion_expr


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


def main():
    return run_test_suite(
        [
            test_non_identity_catalog_mass_to_reference_factor,
            test_h_free_length_to_carried_reference_multiplies_by_h,
            test_identity_when_label_equals_reference,
            test_velocity_is_h_independent,
            test_time_conversion_is_rejected,
        ],
        "Unit Contract Generation (test_unit_contract_generation.py)",
    )


if __name__ == "__main__":
    sys.exit(main())
