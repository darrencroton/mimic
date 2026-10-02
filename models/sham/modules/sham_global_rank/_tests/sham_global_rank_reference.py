#!/usr/bin/env python3
"""
Independent 60-digit Decimal reference for sham_global_rank's rank masses.

Evaluates M_r = M0 * [(r + 0.5) / (n0 * BoxSize^3)]^(-1/alpha), the inverse of
n(>M) = n0 (M / M0)^(-alpha) at n = (r + 0.5) / BoxSize^3, in 60-digit Decimal
arithmetic from the exact binary values of the double parameters, and rounds the
result correctly to float32. It shares no code with the module (which works in
double logarithms) and is the reference behind the numerical edge table in
test_unit_sham_global_rank.c; test_integration_sham_global_rank.py recomputes the
table from here and requires the C constants to match.

Usage:
    python3 sham_global_rank_reference.py    # print the edge table
"""

import struct
from decimal import Decimal, localcontext

#: Precision of every Decimal evaluation.
DIGITS = 60

#: Largest assignable stellar mass, internal units (1e10 Msun/h).
MAX_STELLAR_MASS = Decimal(100000)

#: Bits of the largest finite float32.
FLOAT32_MAX_BITS = 0x7F7FFFFF

#: Numerical edge cases (name, M0, n0, alpha, BoxSize, rank) whose outcome the unit
#: test fixes; their parameter values are the acceptance contract's own.
EDGE_CASES = (
    ("endpoint", 1.0e5, 0.5, 1.0, 1.0, 0),
    ("endpoint_density_above", 1.0e5, 0.5000000000005, 1.0, 1.0, 0),
    ("tiny_volume", 1.0e5, 1.0e-12, 10.0, 1.0e-105, 0),
    ("huge_volume_too_massive", 1.0, 1.0e3, 0.1, 5.6e102, 0),
    ("huge_volume", 1.0e-30, 1.0e3, 10.0, 5.6e102, 0),
    ("subnormal", 1.0e-40, 1.0e-12, 10.0, 1.0, 0),
    ("rounds_to_zero", 1.0e-46, 1.0e3, 10.0, 1.0, 0),
)


def float32_from_bits(bits):
    return struct.unpack("<f", struct.pack("<I", bits))[0]


def reference_mass(mass_scale, number_density, slope, box_size, rank):
    """The exact-real rank mass as a 60-digit Decimal."""
    with localcontext() as ctx:
        ctx.prec = DIGITS
        m0 = Decimal(mass_scale)
        n0 = Decimal(number_density)
        alpha = Decimal(slope)
        box = Decimal(box_size)
        rank_density_ratio = (Decimal(rank) + Decimal("0.5")) / (n0 * box * box * box)
        return (m0.ln() - rank_density_ratio.ln() / alpha).exp()


def nearest_float32_bits(value):
    """Bits of the float32 nearest to a positive Decimal (ties to even); None above the range."""
    with localcontext() as ctx:
        ctx.prec = DIGITS
        largest = Decimal(float32_from_bits(FLOAT32_MAX_BITS))
        if value > largest:
            return None
        guess = struct.unpack("<I", struct.pack("<f", min(float(value), float(largest))))[0]
        candidates = [
            bits for bits in (guess - 1, guess, guess + 1) if 0 <= bits <= FLOAT32_MAX_BITS
        ]
        return min(
            candidates,
            key=lambda bits: (abs(Decimal(float32_from_bits(bits)) - value), bits & 1),
        )


def edge_outcome(mass_scale, number_density, slope, box_size, rank):
    """("fail", None) or ("pass", float32 bits) for one case under the exact-real contract.

    Exact-real classification: a mass above 100000 or one rounding to float32 zero
    fails. The module decides its upper bound on computed double logarithms, so the
    unit test fixes cases far from that boundary (or exactly on it) only.
    """
    value = reference_mass(mass_scale, number_density, slope, box_size, rank)
    if value > MAX_STELLAR_MASS:
        return "fail", None
    bits = nearest_float32_bits(value)
    if bits is None or bits == 0:
        return "fail", None
    return "pass", bits


def edge_table():
    """{case name: (outcome, bits)} for every EDGE_CASES entry."""
    return {name: edge_outcome(*params) for name, *params in EDGE_CASES}


def main():
    for name, *params in EDGE_CASES:
        outcome, bits = edge_outcome(*params)
        value = reference_mass(*params)
        shown = "-" if bits is None else f"0x{bits:08X} ({float32_from_bits(bits)!r})"
        print(f"{name:24s} {outcome:4s} {shown:36s} exact {value:.12E}")


if __name__ == "__main__":
    main()
