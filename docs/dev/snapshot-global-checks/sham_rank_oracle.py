#!/usr/bin/env python3
"""Independent numerical oracle for the Slice 3 global-rank SHAM contract.

The plan freezes this prescription (Slice 3, Acceptance Criteria, revision 3):

- eligible objects are every Type 0/1/2 entry with updated ``ShamVpeak > 0``;
- Type 0/1 update ``ShamVpeak = max(prev, Vmax)`` and ``ShamMpeak = max(prev, Mvir)``,
  Type 2 keeps inherited peaks; all consumed values must be finite and nonnegative
  (for Type 2 that is the inherited peaks only: its current ``Vmax``/``Mvir`` are
  neither consumed nor validated), and the updated double peak must be ``<= FLT_MAX``
  before it is cast to float storage;
- rank by descending ``ShamVpeak`` then ascending positive ``UniqueGalaxyID``;
- for zero-based rank ``r`` the expanded logarithmic form
  ``ln M_r = ln M0 - [ln(r + 0.5) - 3 ln BoxSize - ln n0] / alpha`` is evaluated in
  double, bracket first; ``n_r``, ``BoxSize^3`` and ``n_r / n0`` are never materialised;
- the upper bound is checked in log space, ``ln M_r <= log(100000.0)``, before
  exponentiation; the float rounding of ``exp(ln M_r)`` must then be finite, nonzero
  and ``<= 100000``; a nonzero float32 subnormal is accepted;
- every entry's assigned fields are reset to zero, eligible entries then receive
  the float-rounded ``M_r``; empty/all-ineligible populations succeed.

This module implements that prescription in pure Python (``assign``), an exact
rational oracle for ``alpha = 1`` (``Fraction``), a 60-digit ``Decimal`` reference
for the general case, and a set of deliberately wrong *mutants*. Each mutant must
be killed by at least one test, which is what proves the tests can detect wrong
tie ordering, rank offsets, a materialised ``n_r`` that overflows on admitted tiny
volumes, an exponentiate-then-compare bound that rejects the exact endpoint, an
unchecked ``ShamMpeak`` cast, and the other errors the plan says the module tests
must catch. Parameter/output domain extremes (subnormal volumes, volumes whose
cube is near ``DBL_MAX``, the exact mass upper boundary, float32 subnormal output,
``FLT_MAX`` peaks), malformed parameter strings, empty and zero-candidate
populations and float ULP bounds are exercised too.

Usage::

    python3 docs/dev/snapshot-global-checks/sham_rank_oracle.py [--json OUT] [--seed N]

Nothing here touches Mimic's C code; it is a specification check, not a module test.
The libm behaviour it measures (``exp(log(100000.0))``) is that of the Python
interpreter's platform libm, which is the same library a C build on this machine
links; a Linux build must be measured separately.
"""

from __future__ import annotations

import argparse
import itertools
import json
import math
import random
import struct
import sys
from dataclasses import dataclass
from decimal import Decimal, localcontext
from fractions import Fraction
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from checklib import Checker  # noqa: E402

MASS_MIN_EXCLUSIVE = 0.0
MASS_MAX_INCLUSIVE = 100000.0
# Frozen log-space upper bound: the platform libm's log of the boundary, evaluated once.
LN_MASS_MAX = math.log(MASS_MAX_INCLUSIVE)
DENSITY_MIN, DENSITY_MAX = 1e-12, 1e3
SLOPE_MIN, SLOPE_MAX = 0.1, 10.0
FLOAT32_MAX = 3.4028234663852886e38
FLOAT32_MIN_NORMAL = 1.1754943508222875e-38
DECIMAL_DIGITS = 60
# Measured on the planning platform (Apple clang libm through Python's math module) and
# frozen as the expected float32 bit pattern of the plan's subnormal-output example.
SUBNORMAL_EXAMPLE_BITS = 0x000012DA


class RangeFailure(Exception):
    """The snapshot must fail: a mass left the accepted domain or float storage."""


class InputFailure(Exception):
    """The snapshot must fail: the borrowed population violates the contract."""


@dataclass
class Entry:
    """One borrowed population entry as the module sees it."""

    uid: int
    type: int
    vmax: float
    mvir: float
    vpeak: float = 0.0  # previous ShamVpeak (float storage)
    mpeak: float = 0.0  # previous ShamMpeak (float storage)
    orphan_age: float = 7.0  # must be left unchanged
    # assigned fields (all start non-zero to prove the reset)
    stellar: float = 5.0
    no_scatter: float = 5.0
    scatter_dex: float = 5.0
    bulge: float = 5.0
    metals_stellar: float = 5.0
    metals_bulge: float = 5.0
    sfr: float = 5.0


@dataclass(frozen=True)
class Params:
    mass_scale: float  # M0, internal 1e10 Msun/h
    density: float  # n0, (Mpc/h)^-3
    slope: float  # alpha
    box_size: float  # Mpc/h


def f32(value: float) -> float:
    """Round to float32 and back, as storage in a ``float`` field would.

    A double beyond ``FLT_MAX`` becomes ``inf``, which is what Clang and GCC produce
    for the out-of-range cast (C11 leaves it undefined); NaN stays NaN.
    """
    try:
        return struct.unpack("f", struct.pack("f", value))[0]
    except OverflowError:
        return math.copysign(math.inf, value)


def f32_bits(value: float) -> int:
    return struct.unpack("I", struct.pack("f", value))[0]


def f64_bits(value: float) -> int:
    return struct.unpack("Q", struct.pack("d", value))[0]


def ulp_distance(a: float, b: float) -> int:
    """Distance in float32 ULPs between two finite positive floats."""
    return abs(f32_bits(f32(a)) - f32_bits(f32(b)))


def cube(box_size: float) -> float:
    """``BoxSize^3`` as C would compute it: overflow yields inf, never an exception."""
    try:
        return box_size**3
    except OverflowError:
        return math.inf


def validate_params(p: Params) -> None:
    """The init-time contract: finite, then in range; the cube is evaluated once here only."""
    for name, value in (
        ("mass_scale", p.mass_scale),
        ("density", p.density),
        ("slope", p.slope),
        ("box_size", p.box_size),
    ):
        if not isinstance(value, float) or not math.isfinite(value):
            raise ValueError(f"{name} must be finite, got {value!r}")
    if not (MASS_MIN_EXCLUSIVE < p.mass_scale <= MASS_MAX_INCLUSIVE):
        raise ValueError("ShamGlobalMassScale outside (0, 100000]")
    if not (DENSITY_MIN <= p.density <= DENSITY_MAX):
        raise ValueError("ShamGlobalNumberDensity outside [1e-12, 1e3]")
    if not (SLOPE_MIN <= p.slope <= SLOPE_MAX):
        raise ValueError("ShamGlobalSlope outside [0.1, 10]")
    if p.box_size <= 0.0 or not math.isfinite(cube(p.box_size)) or cube(p.box_size) <= 0.0:
        raise ValueError("BoxSize must be positive with a finite positive cube")


def parse_parameter_text(text: str, low: float, high: float, *, low_inclusive: bool) -> float:
    """Model of the plan's parameter contract for one string: strict parse, finite, range.

    Mirrors ``parse_double_strict`` (full consumption, no trailing characters) followed
    by the explicit ``isfinite`` check and the range macro. Python's ``float`` accepts
    ``nan``/``inf`` like ``strtod`` does, so those reach the finiteness check exactly as
    they would in C. It does not model ``strtod``'s ``ERANGE`` on subnormal results,
    which the plan says no implementation may rely on.
    """
    if text != text.strip() or not text:
        raise ValueError(f"{text!r}: trailing or leading characters")
    try:
        value = float(text)
    except ValueError as exc:
        raise ValueError(f"{text!r}: not a decimal number") from exc
    if not math.isfinite(value):
        raise ValueError(f"{text!r}: not finite")
    below = value < low if low_inclusive else value <= low
    if below or value > high:
        raise ValueError(f"{text!r}: outside range")
    return value


def ln_mass_for_rank(rank: int, p: Params, *, rank_offset: float = 0.5) -> float:
    """``ln M_r`` in the frozen expanded form and grouping: bracket first, then ``/ alpha``.

    Every log operand is finite and positive on the admitted domain, so the result is
    always finite. At the exact endpoint (``M0 = 100000``, ``n0 = 0.5``, ``BoxSize = 1``,
    ``alpha = 1``, rank 0) the bracket is exactly ``0.0`` and the result is exactly
    ``log(100000.0)`` from the same libm call the bound uses.
    """
    bracket = math.log(rank + rank_offset) - 3.0 * math.log(p.box_size) - math.log(p.density)
    return math.log(p.mass_scale) - bracket / p.slope


def mass_for_rank(rank: int, p: Params) -> float:
    """``M_r`` from the expanded form; ``exp`` overflow yields inf like C."""
    try:
        return math.exp(ln_mass_for_rank(rank, p))
    except OverflowError:
        return math.inf


def mass_for_rank_pow_form(rank: int, p: Params) -> float:
    """The superseded form that materialises ``n_r``; kept as a mutant and for ULP comparison.

    For ``BoxSize`` below about 3.5e-103 the division overflows to inf and the mass
    collapses to zero although the true mass is in range; for cubes near ``DBL_MAX``
    ``n_r`` is subnormal and the power can overflow. Both are defects the expanded form
    removes.
    """
    n_r = (rank + 0.5) / cube(p.box_size)
    try:
        return p.mass_scale * (n_r / p.density) ** (-1.0 / p.slope)
    except OverflowError:
        return math.inf
    except ZeroDivisionError:
        return math.inf


def decimal_mass_for_rank(rank: int, p: Params) -> Decimal:
    """60-digit reference value of ``M_r`` from the same expanded expression."""
    with localcontext() as ctx:
        ctx.prec = DECIMAL_DIGITS
        r = Decimal(rank) + Decimal("0.5")
        bracket = r.ln() - 3 * Decimal(p.box_size).ln() - Decimal(p.density).ln()
        return (Decimal(p.mass_scale).ln() - bracket / Decimal(p.slope)).exp()


def store_mass(rank: int, uid: int, ln_m: float, *, bound: str = "log_space") -> float:
    """Frozen acceptance order: log-space upper bound, ``exp``, float rounding, float checks.

    ``bound`` exists only so a mutant can express the exponentiate-then-compare defect.
    """
    if bound == "log_space":
        if not ln_m <= LN_MASS_MAX:  # also rejects NaN
            raise RangeFailure(f"rank {rank} uid {uid}: ln M_r {ln_m!r} exceeds log(100000)")
        try:
            mass = math.exp(ln_m)
        except OverflowError:
            mass = math.inf
    elif bound == "after_exp":
        try:
            mass = math.exp(ln_m)
        except OverflowError:
            mass = math.inf
        if not (math.isfinite(mass) and MASS_MIN_EXCLUSIVE < mass <= MASS_MAX_INCLUSIVE):
            raise RangeFailure(f"rank {rank} uid {uid}: mass {mass!r} outside (0, 100000]")
    else:
        raise ValueError(bound)
    stored = f32(mass)
    if not (math.isfinite(stored) and 0.0 < stored <= MASS_MAX_INCLUSIVE):
        raise RangeFailure(
            f"rank {rank} uid {uid}: mass {mass!r} rounds to {stored!r} in float storage"
        )
    return stored


def update_peaks(entries: list[Entry], *, check_flt_max: bool = True) -> None:
    """Peak tracking as the plan specifies it, validating only what each Type consumes.

    Every Type is ranked on its peaks, so the inherited ``ShamVpeak``/``ShamMpeak`` are
    validated for all entries. Type 0/1 additionally consume the current ``Vmax``/``Mvir``;
    Type 2 keeps its inherited peaks and reads nothing else, so its current proxies are
    neither validated nor folded in (``make_orphan`` zeroes an orphan's ``Mvir``, and a
    constructed non-finite value there must not fail the snapshot).
    """
    for e in entries:
        consumed = [("vpeak", e.vpeak), ("mpeak", e.mpeak)]
        if e.type in (0, 1):
            consumed += [("vmax", e.vmax), ("mvir", e.mvir)]
        for label, value in consumed:
            if not math.isfinite(value) or value < 0.0:
                raise InputFailure(f"uid {e.uid}: {label}={value!r} is not finite and nonnegative")
        if e.type in (0, 1):
            new_mpeak = max(e.mpeak, e.mvir)  # double, as max_double(prev, Mvir) is
            if check_flt_max and new_mpeak > FLOAT32_MAX:
                raise InputFailure(f"uid {e.uid}: ShamMpeak {new_mpeak!r} exceeds FLT_MAX")
            e.vpeak = f32(max(e.vpeak, e.vmax))
            e.mpeak = f32(new_mpeak)
        # Type 2 keeps inherited peaks.
        # ``check_flt_max=False`` models an implementation with neither the pre-cast bound
        # nor the post-cast finiteness check, which is what the plan's Mvir=1e39 test targets.
        if check_flt_max and not (math.isfinite(e.vpeak) and math.isfinite(e.mpeak)):
            raise InputFailure(f"uid {e.uid}: stored peak is not finite")


def check_population(entries: list[Entry]) -> None:
    seen: set[int] = set()
    for e in entries:
        if e.type not in (0, 1, 2):
            raise InputFailure(f"uid {e.uid}: Type {e.type} is not 0/1/2")
        if e.uid <= 0:
            raise InputFailure(f"uid {e.uid} is not positive")
        if e.uid in seen:
            raise InputFailure(f"uid {e.uid} is duplicated")
        seen.add(e.uid)


def assign(
    entries: list[Entry],
    p: Params,
    *,
    sort_key=None,
    rank_offset: float = 0.5,
    evaluation: str = "expanded",
    bound: str = "log_space",
    check_flt_max: bool = True,
    flush_subnormal: bool = False,
) -> dict[int, float]:
    """Reference implementation of the frozen prescription.

    Returns ``{uid: float32-rounded StellarMass}`` for every entry (zero for the
    ineligible). The keyword arguments exist only so the mutants below can be
    expressed as parameter changes to this one function; the defaults are the contract.
    """
    validate_params(p)
    check_population(entries)
    update_peaks(entries, check_flt_max=check_flt_max)
    for e in entries:
        e.stellar = e.no_scatter = e.scatter_dex = 0.0
        e.bulge = e.metals_stellar = e.metals_bulge = e.sfr = 0.0
    eligible = [e for e in entries if e.vpeak > 0.0]
    if not eligible:
        return {e.uid: 0.0 for e in entries}
    key = sort_key or (lambda e: (-e.vpeak, e.uid))
    scratch = sorted(eligible, key=key)  # never the borrowed list itself
    for rank, e in enumerate(scratch):
        if evaluation == "expanded":
            ln_m = ln_mass_for_rank(rank, p, rank_offset=rank_offset)
        elif evaluation == "pow_form":
            mass = mass_for_rank_pow_form(rank, p)
            ln_m = math.log(mass) if mass > 0.0 else -math.inf
        else:
            raise ValueError(evaluation)
        stored = store_mass(rank, e.uid, ln_m, bound=bound)
        if flush_subnormal and stored < FLOAT32_MIN_NORMAL:
            stored = 0.0
        e.stellar = e.no_scatter = stored
    return {e.uid: e.stellar for e in entries}


# --- mutants -----------------------------------------------------------------


def mutant_descending_id_ties(entries, p):
    return assign(entries, p, sort_key=lambda e: (-e.vpeak, -e.uid))


def mutant_rank_plus_one(entries, p):
    return assign(entries, p, rank_offset=1.0)


def mutant_rank_plus_zero(entries, p):
    return assign(entries, p, rank_offset=0.0)


def mutant_ascending_proxy(entries, p):
    return assign(entries, p, sort_key=lambda e: (e.vpeak, e.uid))


def mutant_rank_by_mpeak(entries, p):
    return assign(entries, p, sort_key=lambda e: (-e.mpeak, e.uid))


def mutant_materialise_n_r_first(entries, p):
    """Forms ``n_r = (r + 0.5) / BoxSize^3`` before taking logs (the superseded form)."""
    return assign(entries, p, evaluation="pow_form")


def mutant_compare_after_exp(entries, p):
    """Checks ``exp(ln M_r) <= 100000`` in double instead of ``ln M_r <= log(100000)``."""
    return assign(entries, p, bound="after_exp")


def mutant_mpeak_cast_without_check(entries, p):
    """Casts ``max(prev, Mvir)`` to float without the ``FLT_MAX`` check."""
    return assign(entries, p, check_flt_max=False)


def mutant_flush_subnormal_to_zero(entries, p):
    """Stores a subnormal float32 mass as zero instead of the subnormal."""
    return assign(entries, p, flush_subnormal=True)


def mutant_positive_slope_sign(entries, p):
    """Uses (n_r/n0)^(+1/alpha) instead of the inverse."""
    validate_params(p)
    check_population(entries)
    update_peaks(entries)
    eligible = sorted((e for e in entries if e.vpeak > 0.0), key=lambda e: (-e.vpeak, e.uid))
    out = {e.uid: 0.0 for e in entries}
    for rank, e in enumerate(eligible):
        n_r = (rank + 0.5) / cube(p.box_size)
        out[e.uid] = f32(p.mass_scale * (n_r / p.density) ** (1.0 / p.slope))
    return out


def mutant_normalize_to_count(entries, p):
    """Divides ranks by the eligible count instead of the box volume."""
    validate_params(p)
    check_population(entries)
    update_peaks(entries)
    eligible = sorted((e for e in entries if e.vpeak > 0.0), key=lambda e: (-e.vpeak, e.uid))
    out = {e.uid: 0.0 for e in entries}
    for rank, e in enumerate(eligible):
        n_r = (rank + 0.5) / max(len(eligible), 1)
        out[e.uid] = f32(p.mass_scale * (n_r / p.density) ** (-1.0 / p.slope))
    return out


def mutant_include_zero_peak(entries, p):
    validate_params(p)
    check_population(entries)
    update_peaks(entries)
    eligible = sorted(entries, key=lambda e: (-e.vpeak, e.uid))
    out = {}
    for rank, e in enumerate(eligible):
        out[e.uid] = f32(mass_for_rank(rank, p))
    return out


def mutant_exclude_type2(entries, p):
    validate_params(p)
    check_population(entries)
    update_peaks(entries)
    eligible = sorted(
        (e for e in entries if e.vpeak > 0.0 and e.type != 2), key=lambda e: (-e.vpeak, e.uid)
    )
    out = {e.uid: 0.0 for e in entries}
    for rank, e in enumerate(eligible):
        out[e.uid] = f32(mass_for_rank(rank, p))
    return out


def mutant_positional_result(entries, p):
    """Assigns by list position instead of by identity (a sorted-borrowed-population bug)."""
    validate_params(p)
    check_population(entries)
    update_peaks(entries)
    ranked = sorted((e for e in entries if e.vpeak > 0.0), key=lambda e: (-e.vpeak, e.uid))
    masses = [f32(mass_for_rank(r, p)) for r in range(len(ranked))]
    out = {}
    eligible_positions = [e for e in entries if e.vpeak > 0.0]
    for e, mass in zip(eligible_positions, masses):
        out[e.uid] = mass
    for e in entries:
        out.setdefault(e.uid, 0.0)
    return out


def mutant_clip_instead_of_fail(entries, p):
    validate_params(p)
    check_population(entries)
    update_peaks(entries)
    eligible = sorted((e for e in entries if e.vpeak > 0.0), key=lambda e: (-e.vpeak, e.uid))
    out = {e.uid: 0.0 for e in entries}
    for rank, e in enumerate(eligible):
        out[e.uid] = f32(min(mass_for_rank(rank, p), MASS_MAX_INCLUSIVE))
    return out


def mutant_type2_updates_peaks(entries, p):
    validate_params(p)
    check_population(entries)
    for e in entries:
        e.vpeak = max(e.vpeak, e.vmax)
        e.mpeak = max(e.mpeak, e.mvir)
    eligible = sorted((e for e in entries if e.vpeak > 0.0), key=lambda e: (-e.vpeak, e.uid))
    out = {e.uid: 0.0 for e in entries}
    for rank, e in enumerate(eligible):
        out[e.uid] = f32(mass_for_rank(rank, p))
    return out


def mutant_float32_evaluation(entries, p):
    """Evaluates the power law in float32 arithmetic instead of double."""
    validate_params(p)
    check_population(entries)
    update_peaks(entries)
    eligible = sorted((e for e in entries if e.vpeak > 0.0), key=lambda e: (-e.vpeak, e.uid))
    out = {e.uid: 0.0 for e in entries}
    for rank, e in enumerate(eligible):
        n_r = f32(f32(rank + 0.5) / f32(cube(p.box_size)))
        ratio = f32(n_r / f32(p.density))
        out[e.uid] = f32(f32(p.mass_scale) * f32(ratio ** f32(-1.0 / p.slope)))
    return out


def mutant_skip_malformed_ineligible(entries, p):
    """Silently drops a NaN-peak object that would be ineligible anyway."""
    clean = [e for e in entries if math.isfinite(e.vpeak) and math.isfinite(e.vmax)]
    return assign(clean, p)


def mutant_invent_fallback_ids(entries, p):
    """Replaces a zero/duplicate ID with a synthetic one instead of failing."""
    fixed = []
    used = set()
    for e in entries:
        uid = e.uid
        if uid <= 0 or uid in used:
            uid = max(used, default=0) + 10_000_000
        used.add(uid)
        fixed.append(Entry(uid, e.type, e.vmax, e.mvir, e.vpeak, e.mpeak))
    return assign(fixed, p)


MUTANTS = {
    "descending_id_ties": mutant_descending_id_ties,
    "rank_plus_one": mutant_rank_plus_one,
    "rank_plus_zero": mutant_rank_plus_zero,
    "ascending_proxy": mutant_ascending_proxy,
    "rank_by_mpeak_not_vpeak": mutant_rank_by_mpeak,
    "materialise_n_r_first": mutant_materialise_n_r_first,
    "compare_after_exp": mutant_compare_after_exp,
    "mpeak_cast_without_check": mutant_mpeak_cast_without_check,
    "flush_subnormal_to_zero": mutant_flush_subnormal_to_zero,
    "positive_slope_sign": mutant_positive_slope_sign,
    "normalize_to_eligible_count": mutant_normalize_to_count,
    "include_zero_peak_objects": mutant_include_zero_peak,
    "exclude_type2_orphans": mutant_exclude_type2,
    "positional_not_per_id": mutant_positional_result,
    "clip_instead_of_fail": mutant_clip_instead_of_fail,
    "type2_updates_peaks": mutant_type2_updates_peaks,
    "float32_evaluation": mutant_float32_evaluation,
    "skip_malformed_ineligible": mutant_skip_malformed_ineligible,
    "invent_fallback_ids": mutant_invent_fallback_ids,
}


# --- test cases (each returns True when the implementation under test is correct) --


def oracle_population() -> list[Entry]:
    return [Entry(80, 0, 200.0, 5.0), Entry(7, 1, 100.0, 4.0), Entry(42, 0, 100.0, 3.0)]


ORACLE_PARAMS = Params(mass_scale=8.0, density=1.0, slope=1.0, box_size=1.0)  # n0 * V = 1
ENDPOINT_PARAMS = Params(mass_scale=100000.0, density=0.5, slope=1.0, box_size=1.0)
TINY_BOX_PARAMS = Params(mass_scale=1e5, density=1e-12, slope=10.0, box_size=1e-105)
HUGE_BOX_FAILING_PARAMS = Params(mass_scale=1.0, density=1e3, slope=0.1, box_size=5.6e102)
HUGE_BOX_PASSING_PARAMS = Params(mass_scale=1e-30, density=1e3, slope=10.0, box_size=5.6e102)
SUBNORMAL_PARAMS = Params(mass_scale=1e-40, density=1e-12, slope=10.0, box_size=1.0)
UNDERFLOW_PARAMS = Params(mass_scale=1e-46, density=1e3, slope=10.0, box_size=1.0)


def exact_oracle(uids_and_vpeaks: list[tuple[int, float]], m0: Fraction) -> dict[int, Fraction]:
    """Exact rational masses for alpha=1, n0*V=1: M_r = M0 / (r + 1/2)."""
    ranked = sorted(uids_and_vpeaks, key=lambda t: (-t[1], t[0]))
    return {uid: m0 / (Fraction(r) + Fraction(1, 2)) for r, (uid, _) in enumerate(ranked)}


def test_hand_oracle(impl) -> bool:
    out = impl(oracle_population(), ORACLE_PARAMS)
    expected = {80: f32(16.0), 7: f32(16 / 3), 42: f32(16 / 5)}
    return out == expected


def test_all_permutations(impl) -> bool:
    expected = {80: f32(16.0), 7: f32(16 / 3), 42: f32(16 / 5)}
    for perm in itertools.permutations(oracle_population()):
        if impl([Entry(e.uid, e.type, e.vmax, e.mvir) for e in perm], ORACLE_PARAMS) != expected:
            return False
    return True


def test_rank_offset_r_plus_half(impl) -> bool:
    """A single top-ranked object with M0=8, n0*V=1 must get exactly 16 (r+0.5 => 0.5)."""
    out = impl([Entry(1, 0, 50.0, 1.0)], ORACLE_PARAMS)
    return out == {1: f32(16.0)}


def test_tie_break_ascending_id(impl) -> bool:
    out = impl([Entry(9, 0, 100.0, 1.0), Entry(3, 0, 100.0, 1.0)], ORACLE_PARAMS)
    return out[3] == f32(16.0) and out[9] == f32(16 / 3)


def test_type2_included_with_inherited_peak(impl) -> bool:
    """An orphan with vmax=0 but inherited vpeak=300 must outrank a central at 200."""
    out = impl(
        [Entry(1, 0, 200.0, 1.0), Entry(2, 2, 0.0, 0.0, vpeak=300.0, mpeak=1.0)], ORACLE_PARAMS
    )
    return out[2] == f32(16.0) and out[1] == f32(16 / 3)


def test_type2_keeps_peaks(impl) -> bool:
    entries = [Entry(1, 2, 500.0, 9.0, vpeak=100.0, mpeak=1.0), Entry(2, 0, 200.0, 1.0)]
    out = impl(entries, ORACLE_PARAMS)
    return out[2] == f32(16.0) and out[1] == f32(16 / 3) and entries[0].vpeak == 100.0


def test_type2_unused_current_proxies_not_validated(impl) -> bool:
    """A Type 2 entry with non-finite current Vmax/Mvir but valid inherited peaks must succeed.

    Type 2 consumes only its inherited peaks, so the unused current proxies are neither
    validated nor folded into the peaks: the inherited peaks are unchanged and the orphan
    is ranked on them. The revision 4 oracle wrongly rejected this population.
    """
    orphan = Entry(2, 2, math.nan, math.inf, vpeak=300.0, mpeak=1.0)
    entries = [Entry(1, 0, 200.0, 1.0), orphan]
    out = impl(entries, ORACLE_PARAMS)
    return (
        out[2] == f32(16.0)
        and out[1] == f32(16 / 3)
        and orphan.vpeak == 300.0
        and orphan.mpeak == 1.0
    )


def test_zero_peak_ineligible(impl) -> bool:
    entries = [Entry(1, 0, 0.0, 1.0), Entry(2, 0, 100.0, 1.0)]
    out = impl(entries, ORACLE_PARAMS)
    return out == {1: 0.0, 2: f32(16.0)}


def test_empty_population(impl) -> bool:
    return impl([], ORACLE_PARAMS) == {}


def test_all_ineligible(impl) -> bool:
    entries = [Entry(1, 0, 0.0, 0.0), Entry(2, 2, 0.0, 0.0)]
    return impl(entries, ORACLE_PARAMS) == {1: 0.0, 2: 0.0} and entries[0].orphan_age == 7.0


def test_reset_of_assigned_fields(impl) -> bool:
    entries = [Entry(1, 0, 0.0, 0.0), Entry(2, 0, 100.0, 1.0)]
    impl(entries, ORACLE_PARAMS)
    ineligible, eligible = entries
    zeroed = all(
        getattr(ineligible, f) == 0.0
        for f in (
            "stellar",
            "no_scatter",
            "scatter_dex",
            "bulge",
            "metals_stellar",
            "metals_bulge",
            "sfr",
        )
    )
    eligible_ok = (
        eligible.stellar == f32(16.0)
        and eligible.no_scatter == f32(16.0)
        and eligible.scatter_dex == 0.0
        and eligible.bulge == 0.0
    )
    return zeroed and eligible_ok and ineligible.orphan_age == 7.0 and eligible.orphan_age == 7.0


def test_global_not_per_fof(impl) -> bool:
    """Adding a higher-proxy galaxy elsewhere must push every lower rank down."""
    base = impl([Entry(1, 0, 100.0, 1.0), Entry(2, 0, 90.0, 1.0)], ORACLE_PARAMS)
    grown = impl(
        [Entry(1, 0, 100.0, 1.0), Entry(2, 0, 90.0, 1.0), Entry(3, 0, 500.0, 1.0)], ORACLE_PARAMS
    )
    return (
        grown[3] == f32(16.0)
        and grown[1] == base[2]
        and grown[2] == f32(16 / 5)
        and grown[1] < base[1]
    )


def test_shuffle_invariance(impl, seed: int = 901, populations: int = 128) -> bool:
    rng = random.Random(seed)
    params = Params(mass_scale=1.0, density=0.01, slope=1.5, box_size=100.0)
    for count in range(1, populations + 1):
        entries = [
            Entry(
                i + 1,
                rng.choice((0, 1, 2)),
                float(rng.randrange(1, 11)),
                1.0,
                vpeak=float(rng.randrange(0, 11)),
            )
            for i in range(count)
        ]
        reference = impl(
            [Entry(e.uid, e.type, e.vmax, e.mvir, e.vpeak, e.mpeak) for e in entries], params
        )
        rng.shuffle(entries)
        if impl(entries, params) != reference:
            return False
    return True


def test_repeat_identical_bits(impl) -> bool:
    params = Params(mass_scale=1.0, density=1e-6, slope=1.0, box_size=250.0)
    make = lambda: [
        Entry(i + 1, i % 3, float((i * 37) % 101), 1.0) for i in range(1000)
    ]  # noqa: E731
    a = {uid: f32_bits(m) for uid, m in impl(make(), params).items()}
    b = {uid: f32_bits(m) for uid, m in impl(make(), params).items()}
    return a == b


def test_range_failure_not_clipped(impl) -> bool:
    """alpha=0.1 with a tiny n_r/n0 ratio drives M_r far above 100000: must fail, not clip."""
    params = Params(mass_scale=1.0, density=1e3, slope=0.1, box_size=1000.0)
    try:
        impl([Entry(1, 0, 10.0, 1.0)], params)
    except RangeFailure:
        return True
    return False


def test_float_underflow_fails(impl) -> bool:
    """A mass below float32's smallest subnormal must fail rather than store 0."""
    params = Params(mass_scale=1e-300, density=1e-12, slope=10.0, box_size=1.0)
    try:
        impl([Entry(1, 0, 10.0, 1.0)], params)
    except RangeFailure:
        return True
    return False


def test_malformed_ineligible_fails(impl) -> bool:
    entries = [Entry(1, 0, 100.0, 1.0), Entry(2, 2, 0.0, 0.0, vpeak=math.nan)]
    try:
        impl(entries, ORACLE_PARAMS)
    except InputFailure:
        return True
    return False


def test_negative_proxy_fails(impl) -> bool:
    try:
        impl([Entry(1, 0, -5.0, 1.0)], ORACLE_PARAMS)
    except InputFailure:
        return True
    return False


def test_duplicate_or_nonpositive_ids_fail(impl) -> bool:
    for entries in (
        [Entry(5, 0, 10.0, 1.0), Entry(5, 0, 9.0, 1.0)],
        [Entry(0, 0, 10.0, 1.0)],
        [Entry(-3, 0, 10.0, 1.0)],
    ):
        try:
            impl(entries, ORACLE_PARAMS)
        except InputFailure:
            continue
        return False
    return True


def test_double_precision_evaluation(impl) -> bool:
    """A float32 evaluation of the power law differs from the double value by more than 2 ULPs somewhere."""
    params = Params(mass_scale=0.0137, density=3.1e-4, slope=1.73, box_size=400.0)
    entries = [Entry(i + 1, 0, float(4000 - i), 1.0) for i in range(4000)]
    out = impl(entries, params)
    for rank in range(4000):
        uid = rank + 1
        reference = f32(mass_for_rank(rank, params))
        if ulp_distance(out[uid], reference) > 2:
            return False
    return True


def test_tiny_box_expanded_form(impl) -> bool:
    """BoxSize=1e-105 is admitted (cube 1e-315 is finite and positive) and the mass is in range.

    Forming ``n_r`` first overflows to inf and collapses the mass to zero; the expanded
    form must succeed with the float within 2 ULPs of the 60-digit reference.
    """
    out = impl([Entry(1, 0, 10.0, 1.0)], TINY_BOX_PARAMS)
    reference = float(decimal_mass_for_rank(0, TINY_BOX_PARAMS))
    return out[1] > 0.0 and ulp_distance(out[1], reference) <= 2


def test_exact_endpoint_accepted(impl) -> bool:
    """M0=100000, n0=0.5, BoxSize=1, alpha=1, rank 0: exact mass 100000, must be accepted."""
    out = impl([Entry(1, 0, 10.0, 1.0)], ENDPOINT_PARAMS)
    return out == {1: 100000.0}


def test_just_outside_endpoint_rejected(impl) -> bool:
    """n0 = 0.5000000000005 puts the exact mass 1e-7 above 100000: must fail at rank 0."""
    params = Params(mass_scale=100000.0, density=0.5000000000005, slope=1.0, box_size=1.0)
    try:
        impl([Entry(1, 0, 10.0, 1.0)], params)
    except RangeFailure:
        return True
    return False


def test_huge_box_out_of_range_fails(impl) -> bool:
    """BoxSize=5.6e102 (cube 1.76e308, finite) with M0=1, n0=1e3, alpha=0.1: ln M_r ~ 7174, fails."""
    try:
        impl([Entry(1, 0, 10.0, 1.0)], HUGE_BOX_FAILING_PARAMS)
    except RangeFailure:
        return True
    return False


def test_huge_box_in_range_succeeds(impl) -> bool:
    """BoxSize=5.6e102 with M0=1e-30, n0=1e3, alpha=10: mass ~14.27, n_r is a subnormal double."""
    out = impl([Entry(1, 0, 10.0, 1.0)], HUGE_BOX_PASSING_PARAMS)
    reference = float(decimal_mass_for_rank(0, HUGE_BOX_PASSING_PARAMS))
    return out[1] > 0.0 and ulp_distance(out[1], reference) <= 2


def test_subnormal_output_bits(impl) -> bool:
    """M0=1e-40, n0=1e-12, alpha=10, BoxSize=1 stores the measured float32 subnormal bit pattern."""
    out = impl([Entry(1, 0, 10.0, 1.0)], SUBNORMAL_PARAMS)
    return f32_bits(out[1]) == SUBNORMAL_EXAMPLE_BITS


def test_subnormal_underflow_fails(impl) -> bool:
    """M0=1e-46, n0=1e3, alpha=10, BoxSize=1 rounds to float zero: must fail."""
    try:
        impl([Entry(1, 0, 10.0, 1.0)], UNDERFLOW_PARAMS)
    except RangeFailure:
        return True
    return False


def test_mpeak_flt_max_accepted_exactly(impl) -> bool:
    entries = [Entry(1, 0, 10.0, FLOAT32_MAX)]
    out = impl(entries, ORACLE_PARAMS)
    return out == {1: f32(16.0)} and entries[0].mpeak == FLOAT32_MAX


def test_mpeak_above_flt_max_fails(impl) -> bool:
    try:
        impl([Entry(1, 0, 10.0, 1e39)], ORACLE_PARAMS)
    except InputFailure:
        return True
    return False


TESTS = {
    "hand_oracle_16_16over3_16over5": test_hand_oracle,
    "all_six_permutations": test_all_permutations,
    "rank_offset_is_r_plus_half": test_rank_offset_r_plus_half,
    "tie_break_ascending_id": test_tie_break_ascending_id,
    "type2_included_with_inherited_peak": test_type2_included_with_inherited_peak,
    "type2_keeps_peaks": test_type2_keeps_peaks,
    "type2_unused_current_proxies_not_validated": test_type2_unused_current_proxies_not_validated,
    "zero_peak_ineligible": test_zero_peak_ineligible,
    "empty_population": test_empty_population,
    "all_ineligible_no_rank_evaluation": test_all_ineligible,
    "assigned_fields_reset_orphan_age_kept": test_reset_of_assigned_fields,
    "global_not_per_fof": test_global_not_per_fof,
    "shuffle_invariance_128_populations": test_shuffle_invariance,
    "repeat_identical_float_bits": test_repeat_identical_bits,
    "range_failure_not_clipped": test_range_failure_not_clipped,
    "float32_underflow_fails": test_float_underflow_fails,
    "malformed_ineligible_fails": test_malformed_ineligible_fails,
    "negative_proxy_fails": test_negative_proxy_fails,
    "duplicate_or_nonpositive_ids_fail": test_duplicate_or_nonpositive_ids_fail,
    "double_precision_within_2_ulp": test_double_precision_evaluation,
    "tiny_box_expanded_form": test_tiny_box_expanded_form,
    "exact_endpoint_accepted": test_exact_endpoint_accepted,
    "just_outside_endpoint_rejected": test_just_outside_endpoint_rejected,
    "huge_box_out_of_range_fails": test_huge_box_out_of_range_fails,
    "huge_box_in_range_succeeds": test_huge_box_in_range_succeeds,
    "subnormal_output_bits_pinned": test_subnormal_output_bits,
    "subnormal_underflow_fails": test_subnormal_underflow_fails,
    "mpeak_flt_max_accepted_exactly": test_mpeak_flt_max_accepted_exactly,
    "mpeak_above_flt_max_fails": test_mpeak_above_flt_max_fails,
}


def run_test(test, impl) -> bool:
    try:
        return bool(test(impl))
    except (RangeFailure, InputFailure, ValueError, KeyError, OverflowError, ZeroDivisionError):
        return False


# --- domain sweeps -----------------------------------------------------------


# 2e-108 is close to the smallest admitted box (its cube rounds to the subnormal 1e-323);
# 1e-108 cubes to zero and is rejected at init. 5.6e102 cubes to 1.76e308, just under DBL_MAX.
CORNER_BOX_SIZES = (2e-108, 1e-107, 1e-105, 2.2e-103, 1.0, 62.5, 2000.0, 5.6e102)
CORNER_MASS_SCALES = (5e-324, 1e-30, 1e-3, 1.0, 100000.0)


def parameter_corner_sweep(counts=(1, 2, 1000)) -> list[dict]:
    """Evaluate the accepted parameter corners and report which fail at rank 0 or rank N-1.

    Every corner also records whether ``ln M_r`` stayed finite at each rank (it must)
    and whether the superseded ``n_r``-first form would have reached the same verdict.
    """
    corners = []
    for m0 in CORNER_MASS_SCALES:
        for n0 in (DENSITY_MIN, 1e-3, DENSITY_MAX):
            for alpha in (SLOPE_MIN, 1.0, SLOPE_MAX):
                for box in CORNER_BOX_SIZES:
                    params = Params(m0, n0, alpha, box)
                    for count in counts:
                        population = [Entry(i + 1, 0, float(count - i), 1.0) for i in range(count)]
                        ln_finite = all(
                            math.isfinite(ln_mass_for_rank(r, params)) for r in range(count)
                        )
                        outcome = "ok"
                        try:
                            assign(population, params)
                        except RangeFailure as exc:
                            outcome = "range-failure: " + str(exc)[:60]
                        pow_outcome = "ok"
                        try:
                            assign(
                                [Entry(i + 1, 0, float(count - i), 1.0) for i in range(count)],
                                params,
                                evaluation="pow_form",
                            )
                        except RangeFailure as exc:
                            pow_outcome = "range-failure: " + str(exc)[:60]
                        corners.append(
                            {
                                "M0": m0,
                                "n0": n0,
                                "alpha": alpha,
                                "box": box,
                                "count": count,
                                "outcome": outcome,
                                "ln_finite": ln_finite,
                                "pow_form_outcome": pow_outcome,
                            }
                        )
    return corners


def decimal_ulp_sweep(seed: int, samples: int = 10000) -> dict:
    """Compare the expanded double evaluation with the 60-digit reference after float32 rounding.

    Samples the whole admitted box: ``M0`` log-uniform over ``[1e-40, 1e5]``, ``n0`` over
    ``[1e-12, 1e3]``, ``alpha`` uniform over ``[0.1, 10]``, ``BoxSize`` log-uniform over
    ``[2e-108, 5e102]`` (cube positive and finite), rank uniform below one million. Only
    accepted samples are compared; failures are counted by cause.
    """
    rng = random.Random(seed)
    worst = 0
    over = 0
    accepted = 0
    failed_bound = 0
    failed_zero = 0
    ln_non_finite = 0
    for _ in range(samples):
        params = Params(
            10 ** rng.uniform(-40, 5),
            10 ** rng.uniform(-12, 3),
            rng.uniform(0.1, 10.0),
            10 ** rng.uniform(-107.7, 102.7),
        )
        validate_params(params)  # every sample must be in the admitted domain
        rank = rng.randrange(0, 10**6)
        ln_m = ln_mass_for_rank(rank, params)
        if not math.isfinite(ln_m):
            ln_non_finite += 1
            continue
        try:
            stored = store_mass(rank, 0, ln_m)
        except RangeFailure as exc:
            if "exceeds" in str(exc):
                failed_bound += 1
            else:
                failed_zero += 1
            continue
        accepted += 1
        reference = float(decimal_mass_for_rank(rank, params))
        d = ulp_distance(stored, reference)
        worst = max(worst, d)
        over += d > 2
    return {
        "samples": samples,
        "accepted": accepted,
        "failed_upper_bound": failed_bound,
        "failed_float_zero": failed_zero,
        "ln_non_finite": ln_non_finite,
        "worst_ulp": worst,
        "over_two_ulp": over,
    }


def pow_form_ulp_sweep(seed: int, samples: int = 2000) -> dict:
    """Compare the superseded pow form with the expanded form where the pow form is finite."""
    rng = random.Random(seed)
    worst = 0
    over = 0
    compared = 0
    for _ in range(samples):
        params = Params(
            10 ** rng.uniform(-3, 5),
            10 ** rng.uniform(-12, 3),
            rng.uniform(0.1, 10.0),
            rng.uniform(1.0, 2000.0),
        )
        rank = rng.randrange(0, 10**6)
        a = mass_for_rank(rank, params)
        b = mass_for_rank_pow_form(rank, params)
        if (
            not (math.isfinite(a) and math.isfinite(b))
            or a <= 0
            or b <= 0
            or a > FLOAT32_MAX
            or b > FLOAT32_MAX
        ):
            continue
        if f32(a) == 0.0 or f32(b) == 0.0:
            continue
        compared += 1
        d = ulp_distance(a, b)
        worst = max(worst, d)
        over += d > 2
    return {"samples": samples, "compared": compared, "worst_ulp": worst, "over_two_ulp": over}


def endpoint_evidence() -> dict:
    """Measured facts behind the frozen log-space bound, for the evidence record."""
    exp_of_log = math.exp(LN_MASS_MAX)
    ln_endpoint = ln_mass_for_rank(0, ENDPOINT_PARAMS)
    band_density = 0.5 * (1.0 + 2.0**-52)
    band_params = Params(100000.0, band_density, 1.0, 1.0)
    band_ln = ln_mass_for_rank(0, band_params)
    with localcontext() as ctx:
        ctx.prec = DECIMAL_DIGITS
        band_exact_excess = decimal_mass_for_rank(0, band_params) - Decimal(100000)
    return {
        "log_100000": repr(LN_MASS_MAX),
        "exp_of_log_100000": repr(exp_of_log),
        "exp_of_log_exceeds_100000": exp_of_log > MASS_MAX_INCLUSIVE,
        "float32_of_exp_of_log": repr(f32(exp_of_log)),
        "endpoint_ln_mass_bits_equal_bound": f64_bits(ln_endpoint) == f64_bits(LN_MASS_MAX),
        "double_ulp_of_bound": repr(math.ulp(LN_MASS_MAX)),
        "mass_band_from_one_ulp_of_bound": repr(MASS_MAX_INCLUSIVE * math.ulp(LN_MASS_MAX)),
        "float32_spacing_at_100000": repr(
            struct.unpack("f", struct.pack("I", f32_bits(MASS_MAX_INCLUSIVE) + 1))[0]
            - MASS_MAX_INCLUSIVE
        ),
        "roundoff_band_example": {
            "density": repr(band_density),
            "ln_mass_minus_bound": repr(band_ln - LN_MASS_MAX),
            "accepted": band_ln <= LN_MASS_MAX,
            "exact_mass_minus_100000": str(band_exact_excess),
        },
    }


def main() -> int:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument("--json", type=Path, default=None)
    parser.add_argument("--seed", type=int, default=901)
    args = parser.parse_args()
    chk = Checker("sham_rank")
    report: dict = {"seed": args.seed}

    # 1. Exact oracle agreement (Fraction arithmetic, before any float rounding).
    exact = exact_oracle([(80, 200.0), (7, 100.0), (42, 100.0)], Fraction(8))
    chk.check(
        "exact_oracle_rational",
        exact == {80: Fraction(16), 7: Fraction(16, 3), 42: Fraction(16, 5)},
        str(exact),
    )
    chk.check(
        "exact_oracle_rank_order_80_7_42",
        list(exact) == [80, 7, 42],
        f"rank order {list(exact)}",
    )
    chk.check(
        "float_rounding_of_oracle",
        assign(oracle_population(), ORACLE_PARAMS)
        == {uid: f32(float(v)) for uid, v in exact.items()},
        "float32 rounding of the exact values disagrees",
    )

    # 2. Reference implementation passes every test.
    for name, test in TESTS.items():
        chk.check(
            f"reference.{name}",
            run_test(test, assign),
            "reference implementation failed its own test",
        )

    # 3. The platform fact behind the frozen bound order.
    evidence = endpoint_evidence()
    report["endpoint_evidence"] = evidence
    chk.note(
        f"exp(log(100000.0)) = {evidence['exp_of_log_100000']} on this platform; exceeds 100000: "
        f"{evidence['exp_of_log_exceeds_100000']}; float32 rounding {evidence['float32_of_exp_of_log']}"
    )
    chk.check(
        "endpoint.ln_mass_bit_identical_to_bound",
        evidence["endpoint_ln_mass_bits_equal_bound"],
        "the exact endpoint's ln M_r is not bit-identical to log(100000.0)",
    )
    chk.note(
        "roundoff band: n0 = 0.5*(1+2^-52) gives ln M_r - log(100000) = "
        f"{evidence['roundoff_band_example']['ln_mass_minus_bound']} (accepted = "
        f"{evidence['roundoff_band_example']['accepted']}) although the exact mass exceeds 100000 by "
        f"{evidence['roundoff_band_example']['exact_mass_minus_100000']}; the stored float is 100000.0 either way"
    )
    hazard_present = evidence["exp_of_log_exceeds_100000"]
    if not hazard_present:
        chk.skip(
            "endpoint.exp_then_compare_hazard_reproducible",
            "exp(log(100000.0)) does not exceed 100000 on this platform; the compare_after_exp mutant may survive here",
        )

    # 4. Mutation kill matrix: every mutant must be caught by at least one test.
    matrix: dict[str, list[str]] = {}
    for mutant_name, mutant in MUTANTS.items():
        killers = [tname for tname, test in TESTS.items() if not run_test(test, mutant)]
        matrix[mutant_name] = killers
        if mutant_name == "compare_after_exp" and not hazard_present:
            chk.skip(f"mutant_killed.{mutant_name}", "hazard not reproducible on this platform")
            continue
        chk.check(f"mutant_killed.{mutant_name}", bool(killers), "no test detects this mutant")
    report["mutation_matrix"] = matrix
    # Tests the plan names explicitly must kill their target mutants.
    chk.check(
        "plan_named.wrong_tie_order_detected",
        "descending_id_ties" in matrix and "tie_break_ascending_id" in matrix["descending_id_ties"],
        "tie test does not kill the descending-ID mutant",
    )
    chk.check(
        "plan_named.r_plus_one_detected",
        "rank_offset_is_r_plus_half" in matrix["rank_plus_one"],
        "rank-offset test does not kill r+1",
    )
    chk.check(
        "plan_named.hand_oracle_kills_r_plus_one",
        "hand_oracle_16_16over3_16over5" in matrix["rank_plus_one"],
        "oracle does not kill r+1",
    )
    chk.check(
        "plan_named.hand_oracle_kills_wrong_ties",
        "hand_oracle_16_16over3_16over5" in matrix["descending_id_ties"],
        "oracle does not kill wrong ties",
    )
    chk.check(
        "plan_named.permutation_kills_positional_bug",
        "shuffle_invariance_128_populations" in matrix["positional_not_per_id"],
        "shuffle test does not kill the positional mutant",
    )
    chk.check(
        "plan_named.tiny_box_kills_materialised_n_r",
        "tiny_box_expanded_form" in matrix["materialise_n_r_first"],
        "tiny-box test does not kill the n_r-first mutant",
    )
    if hazard_present:
        chk.check(
            "plan_named.endpoint_kills_compare_after_exp",
            "exact_endpoint_accepted" in matrix["compare_after_exp"],
            "endpoint test does not kill the exponentiate-then-compare mutant",
        )
    chk.check(
        "plan_named.mvir_1e39_kills_unchecked_cast",
        "mpeak_above_flt_max_fails" in matrix["mpeak_cast_without_check"],
        "Mvir=1e39 test does not kill the unchecked cast mutant",
    )
    chk.check(
        "plan_named.subnormal_bits_kill_flush_to_zero",
        "subnormal_output_bits_pinned" in matrix["flush_subnormal_to_zero"],
        "subnormal bit test does not kill the flush-to-zero mutant",
    )

    # 5. Parameter/output domain extremes.
    corners = parameter_corner_sweep()
    failing = [c for c in corners if c["outcome"] != "ok"]
    # Verdict disagreements only: both forms may fail with different diagnostics.
    disagreements = [
        c for c in corners if (c["outcome"] == "ok") != (c["pow_form_outcome"] == "ok")
    ]
    report["corner_sweep"] = {
        "total": len(corners),
        "range_failures": len(failing),
        "pow_form_disagreements": len(disagreements),
        "box_sizes": CORNER_BOX_SIZES,
        "mass_scales": CORNER_MASS_SCALES,
    }
    report["corner_failures_sample"] = failing[:12]
    report["pow_form_disagreement_sample"] = disagreements[:12]
    chk.note(
        f"parameter corner sweep: {len(failing)} of {len(corners)} accepted-domain corners fail the snapshot by specification (mass above 100000 or float underflow)"
    )
    chk.check(
        "corners.ln_mass_finite_everywhere",
        all(c["ln_finite"] for c in corners),
        "ln M_r was non-finite on an admitted corner",
    )
    chk.check(
        "corners.some_accepted_params_fail_by_spec",
        bool(failing),
        "expected some accepted parameter corners to fail by specification",
    )
    chk.check(
        "corners.some_accepted_params_succeed",
        any(c["outcome"] == "ok" for c in corners),
        "no corner succeeded",
    )
    chk.check(
        "corners.expanded_form_accepts_where_pow_form_fails",
        bool(disagreements)
        and all(c["outcome"] == "ok" and c["pow_form_outcome"] != "ok" for c in disagreements),
        f"{len(disagreements)} disagreements, not all of them pow-form false failures",
    )
    chk.note(
        f"the superseded n_r-first form disagrees with the expanded form on {len(disagreements)} corners, every one a false failure of an in-range mass: n_r overflows to inf on tiny admitted volumes and the subnormal n_r on volumes near DBL_MAX overflows the power"
    )
    top_rank_failures = [c for c in failing if "rank 0 " in c["outcome"]]
    chk.note(
        f"of those, {len(top_rank_failures)} fail at rank 0 (the most massive object), i.e. the whole snapshot fails at its first assignment"
    )
    # The specified boundaries themselves.
    chk.expect_raises(
        "params.mass_scale_zero_rejected",
        lambda: assign([], Params(0.0, 1.0, 1.0, 1.0)),
        ValueError,
        "M0=0 accepted",
    )
    chk.expect_raises(
        "params.mass_scale_above_max_rejected",
        lambda: assign([], Params(100000.0000001, 1.0, 1.0, 1.0)),
        ValueError,
        "M0>100000 accepted",
    )
    chk.expect_raises(
        "params.density_below_min_rejected",
        lambda: assign([], Params(1.0, 9e-13, 1.0, 1.0)),
        ValueError,
        "n0<1e-12 accepted",
    )
    chk.expect_raises(
        "params.slope_below_min_rejected",
        lambda: assign([], Params(1.0, 1.0, 0.0999, 1.0)),
        ValueError,
        "alpha<0.1 accepted",
    )
    chk.expect_raises(
        "params.nan_rejected",
        lambda: assign([], Params(math.nan, 1.0, 1.0, 1.0)),
        ValueError,
        "NaN M0 accepted",
    )
    chk.expect_raises(
        "params.box_cube_overflow_rejected",
        lambda: assign([], Params(1.0, 1.0, 1.0, 1e103)),
        ValueError,
        "BoxSize^3 overflow accepted",
    )
    chk.check(
        "params.boundaries_inclusive_exclusive_as_specified",
        assign([], Params(100000.0, 1e-12, 0.1, 1.0)) == {}
        and assign([], Params(1e-300, 1e3, 10.0, 1.0)) == {},
        "inclusive boundary rejected",
    )
    # Malformed parameter strings under the plan's parse-then-isfinite-then-range contract.
    rejected_strings = ("nan", "inf", "infinity", "1e400", "1,0", "abc", "1e5 ", "", "0")
    for text in rejected_strings:
        chk.expect_raises(
            f"params.string_rejected.{text!r}",
            lambda text=text: parse_parameter_text(text, 0.0, 100000.0, low_inclusive=False),
            ValueError,
            f"{text!r} accepted as ShamGlobalMassScale",
        )
    accepted_1e5 = parse_parameter_text("1e5", 0.0, 100000.0, low_inclusive=False)
    accepted_100000 = parse_parameter_text("100000", 0.0, 100000.0, low_inclusive=False)
    chk.check(
        "params.strings_1e5_and_100000_identical_bits",
        f64_bits(accepted_1e5) == f64_bits(accepted_100000) == f64_bits(100000.0),
        f"{accepted_1e5!r} vs {accepted_100000!r}",
    )
    chk.note(
        "subnormal parameter strings such as '1e-320' parse to a subnormal double in Python; C strtod on macOS reports ERANGE and the strict parser rejects them, glibc likewise, but the plan forbids relying on that: a subnormal M0 that does parse is in-domain and ln M0 is finite (log(5e-324) = -744.44)"
    )
    # Subnormal storage: accepted and pinned by bits (measured, not assumed).
    subnormal_out = assign([Entry(1, 0, 10.0, 1.0)], SUBNORMAL_PARAMS)
    report["subnormal_example"] = {
        "params": SUBNORMAL_PARAMS.__dict__,
        "double_mass": repr(mass_for_rank(0, SUBNORMAL_PARAMS)),
        "float32_mass": repr(subnormal_out[1]),
        "float32_bits": f"0x{f32_bits(subnormal_out[1]):08X}",
        "decimal_reference": str(decimal_mass_for_rank(0, SUBNORMAL_PARAMS)),
    }
    chk.check(
        "storage.subnormal_float_is_neither_zero_nor_nonfinite",
        0.0 < subnormal_out[1] < FLOAT32_MIN_NORMAL and math.isfinite(subnormal_out[1]),
        "float32 subnormal behaviour unexpected",
    )
    chk.note(
        f"subnormal example stores float32 bits {report['subnormal_example']['float32_bits']} "
        f"({report['subnormal_example']['float32_mass']}) for double {report['subnormal_example']['double_mass']}"
    )
    # Huge-box evidence for the record: the failing case is out of range, not a success.
    report["huge_box"] = {
        "failing": {
            "params": HUGE_BOX_FAILING_PARAMS.__dict__,
            "ln_mass": repr(ln_mass_for_rank(0, HUGE_BOX_FAILING_PARAMS)),
            "decimal_reference": f"{decimal_mass_for_rank(0, HUGE_BOX_FAILING_PARAMS):.6E}",
        },
        "passing": {
            "params": HUGE_BOX_PASSING_PARAMS.__dict__,
            "float32_mass": repr(assign([Entry(1, 0, 10.0, 1.0)], HUGE_BOX_PASSING_PARAMS)[1]),
            "decimal_reference": str(decimal_mass_for_rank(0, HUGE_BOX_PASSING_PARAMS)),
            "n_r_if_materialised": repr(0.5 / cube(5.6e102)),
        },
    }
    chk.note(
        f"huge box 5.6e102 with M0=1, n0=1e3, alpha=0.1 gives ln M_r = {report['huge_box']['failing']['ln_mass']} (exact mass {report['huge_box']['failing']['decimal_reference']}): a range failure, not a success; M0=1e-30, alpha=10 succeeds with {report['huge_box']['passing']['float32_mass']}"
    )
    # Largest population that still succeeds at a generous corner, to show scale does not break the math.
    big = [
        Entry(i + 1, i % 3, float(rng_v), 1.0)
        for i, rng_v in enumerate(random.Random(args.seed).choices(range(1, 500), k=100_000))
    ]
    out = assign(big, Params(1.0, 1e-7, 1.0, 500.0))
    # Type 2 entries carry no inherited peak here, so only Type 0/1 are eligible.
    expected_eligible = sum(1 for e in big if e.type in (0, 1))
    assigned = sum(1 for v in out.values() if v > 0)
    chk.check(
        "scale.100k_population_assigns_exactly_the_eligible",
        assigned == expected_eligible,
        f"{assigned} assigned, {expected_eligible} eligible",
    )
    chk.check(
        "scale.100k_type2_without_peak_stay_zero",
        all(out[e.uid] == 0.0 for e in big if e.type == 2),
        "a peak-less Type 2 received mass",
    )
    ranks_desc = sorted(out.values(), reverse=True)
    chk.check(
        "scale.100k_masses_monotone_nonincreasing_by_rank",
        all(a >= b for a, b in zip(ranks_desc, ranks_desc[1:])),
        "not monotone",
    )

    # 6. Float ULP bounds: expanded form against the 60-digit reference over the admitted
    #    domain, and against the superseded pow form where that form is finite.
    ulps = decimal_ulp_sweep(args.seed)
    report["decimal_ulp_sweep"] = ulps
    chk.check(
        "ulp.expanded_vs_decimal_within_2_ulp",
        ulps["over_two_ulp"] == 0 and ulps["accepted"] > 0,
        f"{ulps}",
    )
    chk.check(
        "ulp.expanded_ln_mass_finite_on_all_samples",
        ulps["ln_non_finite"] == 0,
        f"{ulps['ln_non_finite']} non-finite ln M_r samples",
    )
    chk.note(
        f"Decimal sweep: {ulps['accepted']} of {ulps['samples']} random admitted samples accepted, worst float32 distance from the 60-digit reference {ulps['worst_ulp']} ULP; {ulps['failed_upper_bound']} failed the log-space bound, {ulps['failed_float_zero']} rounded to float zero"
    )
    pow_ulps = pow_form_ulp_sweep(args.seed)
    report["pow_form_ulp_sweep"] = pow_ulps
    chk.check("ulp.pow_form_vs_expanded_within_2_ulp", pow_ulps["over_two_ulp"] == 0, f"{pow_ulps}")
    chk.note(
        f"pow-form sweep: worst float32 distance between the superseded pow form and the expanded form was {pow_ulps['worst_ulp']} over {pow_ulps['compared']} finite samples"
    )

    report["summary"] = {
        "passed": len(chk.passed),
        "failed": len(chk.failed),
        "skipped": len(chk.skipped),
        "errors": len(chk.errored),
        "notes": chk.notes,
    }
    report["failures"] = chk.failed
    if args.json:
        args.json.parent.mkdir(parents=True, exist_ok=True)
        args.json.write_text(json.dumps(report, indent=2, default=str), encoding="utf-8")
    return chk.finish()


if __name__ == "__main__":
    sys.exit(main())
