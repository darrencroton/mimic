#!/usr/bin/env python3
"""
Independent double-precision reference for sham_rank_match's target and inversion.

Converts the Baldry et al. (2012) GAMA double Schechter fit from its published
h_obs = 0.7 to a simulation's h_sim (log10 Ms_sim = log10 Ms + 2 log10(h_obs / h_sim),
phi_i_sim = phi_i (h_sim / h_obs)^3) and evaluates

    n(>M) = integral_M^(120 Ms) exp(-x) [phi1 x^alpha1 + phi2 x^alpha2] dx,  x = M' / Ms,

for each M directly, by composite Simpson in ln M' over 400,000 equal intervals
(400,001 nodes; Simpson's rule needs an even interval count). Its inverse is a
bisection in ln M on that integral. It shares no code and no table with the module,
which tabulates n(>M) once on 4096 nodes and interpolates in log-log; the module must
agree with this reference to |delta log10 M*| <= 1e-4.

CASES is the rank-density table embedded in test_unit_sham_rank_match.c between its
SHAM_DECIMAL_REFERENCE markers; test_integration_sham_rank_match.py recomputes it
from here and requires the C table to match exactly.

Usage:
    python3 sham_rank_match_reference.py    # print the converted targets and the table
"""

import math

import numpy as np

#: Baldry et al. (2012) double Schechter fit: log10 Ms [Msun], phi1, alpha1, phi2,
#: alpha2 [Mpc^-3], physical units at h_obs.
PUBLISHED = (10.66, 3.96e-3, -0.35, 0.79e-3, -1.47)

#: Hubble parameter of the published fit.
H_OBS = 0.7

#: log10 of the target's low-mass floor, physical Msun at h_sim (the run files' value).
LOG_MASS_FLOOR = 8.0

#: Upper integration limit in units of Ms (the module's table top).
MAX_MASS_RATIO = 120.0

#: Composite Simpson intervals in ln M per integral.
SIMPSON_INTERVALS = 400_000

#: Bisection steps in ln M; the bracket (under 40 in ln M) shrinks below 1e-15.
BISECTION_STEPS = 60

#: Decimal places of log10 M* in the C table.
LOG_MASS_DECIMALS = 8

#: Rank-density cases (name, h_sim, rank density [physical Mpc^-3]) the unit test fixes.
#: fixture_rank0/5 are (r + 0.5) / 100^3 * 0.6774^3 for the committed micro-Uchuu
#: fixture; floor_inside/outside sit 1e-4 (relative) either side of n(>10^8) =
#: 3.031993e-2; tail_deep (M ~ 21 Ms) and tail_extreme (M ~ 70 Ms) lie in the
#: exponential cutoff; the h = 0.73 pair checks the conversion on both sides of its floor.
CASES = (
    ("fixture_rank0", 0.6774, 1.554195e-7),
    ("fixture_rank5", 0.6774, 1.709615e-6),
    ("mid_1e-4", 0.6774, 1.0e-4),
    ("mid_1e-3", 0.6774, 1.0e-3),
    ("mid_1e-2", 0.6774, 1.0e-2),
    ("floor_inside", 0.6774, 3.0316e-2),
    ("floor_outside", 0.6774, 3.0323e-2),
    ("tail_deep", 0.6774, 1.0e-12),
    ("tail_extreme", 0.6774, 1.0e-30),
    ("hubble_073", 0.73, 1.0e-4),
    ("hubble_073_outside", 0.73, 5.0e-2),
)


def converted_target(h_sim):
    """(log10 Ms_sim, phi1_sim, alpha1, phi2_sim, alpha2) at the simulation's h."""
    log_ms, phi1, alpha1, phi2, alpha2 = PUBLISHED
    scale = (h_sim / H_OBS) ** 3
    return (log_ms + 2.0 * math.log10(H_OBS / h_sim), phi1 * scale, alpha1, phi2 * scale, alpha2)


def cumulative_density(h_sim, log_mass):
    """n(>M) in physical Mpc^-3 at ln M = log_mass, by composite Simpson in ln M'."""
    log10_ms, phi1, alpha1, phi2, alpha2 = converted_target(h_sim)
    log_ms = log10_ms * math.log(10.0)
    log_top = log_ms + math.log(MAX_MASS_RATIO)
    if log_mass >= log_top:
        return 0.0
    t = np.linspace(log_mass, log_top, SIMPSON_INTERVALS + 1)
    log_x = t - log_ms
    x = np.exp(log_x)
    f = phi1 * np.exp((alpha1 + 1.0) * log_x - x) + phi2 * np.exp((alpha2 + 1.0) * log_x - x)
    weights = np.ones_like(t)
    weights[1:-1:2] = 4.0
    weights[2:-1:2] = 2.0
    step = (log_top - log_mass) / SIMPSON_INTERVALS
    return float(step / 3.0 * np.dot(weights, f))


def inverse_log10_mass(h_sim, density):
    """log10 M [physical Msun] with n(>M) = density, by bisection in ln M."""
    log10_ms = converted_target(h_sim)[0]
    lo = (LOG_MASS_FLOOR - 1.0) * math.log(10.0)
    hi = log10_ms * math.log(10.0) + math.log(MAX_MASS_RATIO)
    for _ in range(BISECTION_STEPS):
        mid = 0.5 * (lo + hi)
        if cumulative_density(h_sim, mid) >= density:
            lo = mid
        else:
            hi = mid
    return 0.5 * (lo + hi) / math.log(10.0)


def case_outcome(h_sim, density):
    """("mask", None) above the floor density, else ("assign", log10 M* formatted)."""
    floor_density = cumulative_density(h_sim, LOG_MASS_FLOOR * math.log(10.0))
    if density > floor_density:
        return "mask", None
    return "assign", f"{inverse_log10_mass(h_sim, density):.{LOG_MASS_DECIMALS}f}"


def case_table():
    """{case name: (outcome, log10 M* string or None)} for every CASES entry."""
    return {name: case_outcome(h_sim, density) for name, h_sim, density in CASES}


def main():
    for h_sim in (0.6774, 0.73):
        log_ms, phi1, _, phi2, _ = converted_target(h_sim)
        print(f"h_sim={h_sim}: log10 Ms={log_ms:.7f} phi1={phi1:.7e} phi2={phi2:.7e}")
    for log_mass in (8.0, 9.0, 10.0, 11.0, 11.5):
        value = cumulative_density(0.6774, log_mass * math.log(10.0))
        print(f"n(>10^{log_mass}) = {value:.7e} Mpc^-3 at h_sim=0.6774")
    for name, (outcome, log_mass) in case_table().items():
        print(f"{name:20s} {outcome:6s} {log_mass or '-'}")


if __name__ == "__main__":
    main()
