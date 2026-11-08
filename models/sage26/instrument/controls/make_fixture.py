#!/usr/bin/env python3
"""Synthetic fixture controls S0-S6 for the T1 parity instrument.

Writes a small SAGE26-layout reference and a Mimic-layout candidate built directly from
the literal tables below, then one control directory per synthetic control (S0, S0e,
S1a-S1g, S2a-S2b, S3a-S3c, S4a-S4b, S5a-S5c, S6), each holding its inputs and an
``expect.json`` for ``run_control.py``. S7 (altered field maps run on S0's files) is
built by ``alter_map.py``.

Independence from ``field_map.yaml``: this file never reads the map (it imports the
comparator only for its ``T1_REFERENCE_SHA256``, which S0e passes). ``FIELDS`` pairs
each SAGE26 dataset with its Mimic property, component, storage types and unit factor,
written from SAGE26's writer field list (``src/io/save_gals_hdf5_fields.h``, the
``GALAXY_OUTPUT_FIELDS(X)`` list that ``src/io/save_gals_hdf5.c`` expands, in the staged
reference source) and from Mimic's metadata (``models/sage26/model_properties.yaml``,
``src/core/core_properties.yaml``, ``simulations/mini-millennium/halo_properties.yaml``,
as written in a ``sage26`` run's ``Galaxies`` table). Every factor is 1: each SAGE26
unit in the field list equals the Mimic output unit of the paired property.

Values: every float dataset f (its row in ``FIELDS``) holds, for galaxy i at snapshot
s, ``float32((f + 1) * (1000 + i) * (1 + s / 128))``. Two different fields of the same
galaxy differ by the ratio of their (f + 1), at least 1/81 relative, far outside the
1e-6 tolerance, and so do the three components of ``Pos``, ``Vel`` and ``Spin``; a
swapped field, a swapped component or a wrong factor in the field map therefore moves
every affected pair outside tolerance. Each value is stored in the reference as the
float32 nearest the formula evaluated in float64 (not every formula value is a float32),
and the candidate is derived from that stored value, so
the Mimic float32 and float64 properties hold it exactly and the unperturbed fixture is
bit-identical. Integer datasets take the literal per-galaxy rules in ``FIELDS``.

Each control's expected outcome is computed by ``control_lib.evaluate`` from the
control's own tables (the expected pairing is known by construction), and checked
against the outcome the control is designed to give before anything is written.
"""

import argparse
import os
import sys

import control_lib as cl
import numpy as np

#: Galaxies per snapshot in the fixture.
N = 1000

#: Partition files in the fixture's candidate.
PARTITIONS = 2


FLOAT = None  # marks a float dataset: its values follow the formula in the docstring


def _i(_s):
    return np.arange(N, dtype=np.int64)


#: The pairing table: (SAGE26 dataset, SAGE26 storage type, Mimic property or None for
#: an uncompared dataset, component, Mimic storage type, factor, integer value rule).
#: SAGE26 order and types: save_gals_hdf5_fields.h lines 21-261 (int32_t and
#: H5T_NATIVE_INT are int32, long long is int64, float is float32, double is float64).
FIELDS = (
    ("SnapNum", "int32", "SnapNum", None, "int32", 1, lambda s: np.full(N, s)),
    ("Type", "int32", "Type", None, "int32", 1, lambda s: _i(s) % 3),
    ("GalaxyIndex", "int64", None, None, None, None, lambda s: s * 10**9 + _i(s)),
    ("CentralGalaxyIndex", "int64", None, None, None, None, lambda s: s * 10**9 + _i(s) // 5),
    ("SAGEHaloIndex", "int32", None, None, None, None, lambda s: _i(s)),
    ("SAGETreeIndex", "int32", None, None, None, None, lambda s: _i(s) // 10),
    (
        "SimulationHaloIndex",
        "int64",
        "MostBoundID",
        None,
        "int64",
        1,
        lambda s: 10**12 + 10**4 * s + _i(s),
    ),
    ("mergeType", "int32", None, None, None, None, lambda s: np.zeros(N)),
    ("mergeIntoID", "int32", None, None, None, None, lambda s: np.full(N, -1)),
    ("mergeIntoSnapNum", "int32", None, None, None, None, lambda s: np.full(N, -1)),
    ("dT", "float32", "dT", None, "float64", 1, FLOAT),
    ("Posx", "float32", "Pos", 0, "float32", 1, FLOAT),
    ("Posy", "float32", "Pos", 1, "float32", 1, FLOAT),
    ("Posz", "float32", "Pos", 2, "float32", 1, FLOAT),
    ("Velx", "float32", "Vel", 0, "float32", 1, FLOAT),
    ("Vely", "float32", "Vel", 1, "float32", 1, FLOAT),
    ("Velz", "float32", "Vel", 2, "float32", 1, FLOAT),
    ("Spinx", "float32", "Spin", 0, "float32", 1, FLOAT),
    ("Spiny", "float32", "Spin", 1, "float32", 1, FLOAT),
    ("Spinz", "float32", "Spin", 2, "float32", 1, FLOAT),
    ("Len", "int32", "Len", None, "int32", 1, lambda s: 20 + 7 * _i(s) + s),
    ("Mvir", "float32", "Mvir", None, "float64", 1, FLOAT),
    ("CentralMvir", "float32", "CentralMvir", None, "float64", 1, FLOAT),
    ("Rvir", "float32", "Rvir", None, "float64", 1, FLOAT),
    ("Vvir", "float32", "Vvir", None, "float64", 1, FLOAT),
    ("Vmax", "float32", "Vmax", None, "float32", 1, FLOAT),
    ("VelDisp", "float32", "VelDisp", None, "float32", 1, FLOAT),
    ("ColdGas", "float32", "ColdGas", None, "float32", 1, FLOAT),
    ("StellarMass", "float32", "StellarMass", None, "float32", 1, FLOAT),
    ("BulgeMass", "float32", "BulgeMass", None, "float32", 1, FLOAT),
    ("HotGas", "float32", "HotGas", None, "float32", 1, FLOAT),
    ("EjectedMass", "float32", "EjectedGas", None, "float32", 1, FLOAT),
    ("BlackHoleMass", "float32", "BlackHoleMass", None, "float32", 1, FLOAT),
    ("IntraClusterStars", "float32", "ICS", None, "float32", 1, FLOAT),
    ("MetalsColdGas", "float32", "MetalsColdGas", None, "float32", 1, FLOAT),
    ("MetalsStellarMass", "float32", "MetalsStellarMass", None, "float32", 1, FLOAT),
    ("MetalsBulgeMass", "float32", "MetalsBulgeMass", None, "float32", 1, FLOAT),
    ("MetalsHotGas", "float32", "MetalsHotGas", None, "float32", 1, FLOAT),
    ("MetalsEjectedMass", "float32", "MetalsEjectedGas", None, "float32", 1, FLOAT),
    ("MetalsIntraClusterStars", "float32", "MetalsICS", None, "float32", 1, FLOAT),
    ("SfrDisk", "float32", "SfrDisk", None, "float32", 1, FLOAT),
    ("SfrBulge", "float32", "SfrBulge", None, "float32", 1, FLOAT),
    ("SfrDiskZ", "float32", "SfrDiskZ", None, "float32", 1, FLOAT),
    ("SfrBulgeZ", "float32", "SfrBulgeZ", None, "float32", 1, FLOAT),
    ("DiskRadius", "float32", "DiskScaleRadius", None, "float32", 1, FLOAT),
    ("BulgeRadius", "float32", "BulgeRadius", None, "float32", 1, FLOAT),
    ("MergerBulgeRadius", "float32", "MergerBulgeRadius", None, "float32", 1, FLOAT),
    ("InstabilityBulgeRadius", "float32", "InstabilityBulgeRadius", None, "float32", 1, FLOAT),
    ("MergerBulgeMass", "float32", "MergerBulgeMass", None, "float32", 1, FLOAT),
    ("InstabilityBulgeMass", "float32", "InstabilityBulgeMass", None, "float32", 1, FLOAT),
    ("Cooling", "float32", "Cooling", None, "float64", 1, FLOAT),
    ("Heating", "float32", "Heating", None, "float64", 1, FLOAT),
    (
        "QuasarModeBHaccretionMass",
        "float32",
        "QuasarModeBHaccretionMass",
        None,
        "float32",
        1,
        FLOAT,
    ),
    ("TimeOfLastMajorMerger", "float32", "TimeOfLastMajorMerger", None, "float32", 1, FLOAT),
    ("TimeOfLastMinorMerger", "float32", "TimeOfLastMinorMerger", None, "float32", 1, FLOAT),
    ("OutflowRate", "float32", "SupernovaOutflowRate", None, "float32", 1, FLOAT),
    ("infallMvir", "float32", "infallMvir", None, "float64", 1, FLOAT),
    ("infallVvir", "float32", "infallVvir", None, "float64", 1, FLOAT),
    ("infallVmax", "float32", "infallVmax", None, "float64", 1, FLOAT),
    ("infallStellarMass", "float32", "infallStellarMass", None, "float32", 1, FLOAT),
    ("Regime", "int32", "Regime", None, "int32", 1, lambda s: _i(s) % 2),
    ("CGMgas", "float32", "CGMgas", None, "float32", 1, FLOAT),
    ("MetalsCGMgas", "float32", "MetalsCGMgas", None, "float32", 1, FLOAT),
    ("MassLoading", "float32", "MassLoading", None, "float32", 1, FLOAT),
    ("H2gas", "float32", "H2gas", None, "float32", 1, FLOAT),
    ("H1gas", "float32", "H1gas", None, "float32", 1, FLOAT),
    ("tcool", "float32", "tcool", None, "float32", 1, FLOAT),
    ("tff", "float32", "tff", None, "float32", 1, FLOAT),
    ("H2DepletionTime_Gyr", "float32", "H2DepletionTime_Gyr", None, "float32", 1, FLOAT),
    ("RcoolToRvir", "float32", "RcoolToRvir", None, "float32", 1, FLOAT),
    ("TimeOfInfall", "float32", "TimeOfInfall", None, "float32", 1, FLOAT),
    ("FFBRegime", "int32", "FFBRegime", None, "int32", 1, lambda s: (_i(s) + 1) % 2),
    ("Concentration", "float32", "Concentration", None, "float32", 1, FLOAT),
    ("mdot_cool", "float32", "mdot_cool", None, "float32", 1, FLOAT),
    ("mdot_stream", "float32", "mdot_stream", None, "float32", 1, FLOAT),
    ("ICS_disrupt", "float32", "ICS_disrupt", None, "float32", 1, FLOAT),
    ("ICS_accrete", "float32", "ICS_accrete", None, "float32", 1, FLOAT),
    ("ICS_sum_mt", "float32", "ICS_sum_mt", None, "float32", 1, FLOAT),
    ("g_max", "float64", None, None, None, None, lambda s: np.zeros(N)),
    ("r_heat", "float32", "Rheat", None, "float32", 1, FLOAT),
    ("CoolingRate", "float32", "CoolingRate", None, "float32", 1, FLOAT),
)

ORDER = [row[0] for row in FIELDS]
MAPPINGS = [
    cl.Mapping(ds, "int" if st.startswith("int") else "float", st, mimic, comp, mt, factor)
    for ds, st, mimic, comp, mt, factor, _ in FIELDS
    if mimic is not None
]
UNCOMPARED = [row[0] for row in FIELDS if row[2] is None]
BY_DS = {mp.ds: mp for mp in MAPPINGS}
MIMIC_DTYPE = cl.mimic_dtype(MAPPINGS)
SNAP = 63  # the snapshot every perturbation acts on; the other seven stay as written


def reference_snapshot(s):
    """The fixture's SAGE26-layout datasets at snapshot s, from the literal table."""
    data = {}
    galaxy = np.arange(N, dtype=np.float64)
    for f, (ds, st, _, _, _, _, rule) in enumerate(FIELDS):
        if rule is FLOAT:
            values = (f + 1) * (1000 + galaxy) * (1 + s / 128.0)
        else:
            values = rule(s)
        data[ds] = np.asarray(values).astype(st)
    return data


def candidate_from(ref):
    """The Mimic-layout table holding ``ref``'s values (each factor inverted)."""
    cand = np.zeros(N, dtype=MIMIC_DTYPE)
    for mp in MAPPINGS:
        values = (ref[mp.ds].astype(np.float64) / mp.factor) if mp.kind == "float" else ref[mp.ds]
        values = values.astype(mp.mimic_type)
        if mp.component is None:
            cand[mp.mimic] = values
        else:
            cand[mp.mimic][:, mp.component] = values
    return cand


def base():
    """``{snap: [ref, cand, origin]}`` for the unperturbed fixture."""
    out = {}
    for s in cl.T1_SNAPSHOTS:
        ref = reference_snapshot(s)
        out[s] = [ref, candidate_from(ref), np.arange(N, dtype=np.int64)]
    return out


def path_prop(ds, key, snap=SNAP):
    return ["snapshot_tables", "snap=%d" % snap, "properties", ds, key]


def path_threshold(name, snap=SNAP):
    return ["snapshot_tables", "snap=%d" % snap, "thresholds", "threshold=%s" % name, "passed"]


def fact(label, path, equals, of=None):
    out = {"label": label, "path": path, "equals": equals}
    if of is not None:
        out["of"] = of
    return out


def _scale(cand, prop, rows, factor):
    """Multiply a float32 candidate property by ``factor`` (in float64) on ``rows``."""
    cand[prop][rows] = (cand[prop][rows].astype(np.float64) * factor).astype(cand.dtype[prop])


# ---------------------------------------------------------------------------
# The synthetic controls: each perturbs snapshot 63 of a fresh base fixture and
# returns (description, designed verdict, designed failures, facts).
# ---------------------------------------------------------------------------


def s0(fx):
    facts = []
    for s in cl.T1_SNAPSHOTS:
        for mp in MAPPINGS:
            key = "exact_equal" if mp.kind == "int" else "bit_identical"
            facts.append({"path": path_prop(mp.ds, key, s), "equals": N})
    group = {
        "label": "every mapped property 100%% bit-identical (%d mappings x 8 snapshots)"
        % len(MAPPINGS),
        "items": facts,
    }
    return ("the fixture as written", "pass", [], [], [group])


def s1a(fx):
    _scale(fx[SNAP][1], "StellarMass", slice(0, 100), 1 + 1e-3)
    return (
        "float boundary: StellarMass outside tolerance in 100 of 1000 pairs (900 within)",
        "pass",
        [],
        [
            fact("StellarMass within", path_prop("StellarMass", "within_tolerance"), 900, N),
            fact("over_M gate passed", path_threshold("float:StellarMass:over_M"), True),
        ],
    )


def s1b(fx):
    _scale(fx[SNAP][1], "StellarMass", slice(0, 101), 1 + 1e-3)
    return (
        "float boundary: StellarMass outside tolerance in 101 of 1000 pairs (899 within)",
        "fail",
        ["Snap 63: float:StellarMass:over_M", "Snap 63: float:StellarMass:over_M_sig"],
        [
            fact("StellarMass within", path_prop("StellarMass", "within_tolerance"), 899, N),
            fact("over_M gate passed", path_threshold("float:StellarMass:over_M"), False),
        ],
    )


def s1c(fx):
    fx[SNAP][1]["Len"][0:5] += 1
    return (
        "integer boundary: Len unequal in 5 of 1000 pairs (995 equal)",
        "pass",
        [],
        [
            fact("Len exactly equal", path_prop("Len", "exact_equal"), 995, N),
            fact("int gate passed", path_threshold("int:Len"), True),
        ],
    )


def s1d(fx):
    fx[SNAP][1]["Len"][0:6] += 1
    return (
        "integer boundary: Len unequal in 6 of 1000 pairs (994 equal)",
        "fail",
        ["Snap 63: int:Len"],
        [
            fact("Len exactly equal", path_prop("Len", "exact_equal"), 994, N),
            fact("int gate passed", path_threshold("int:Len"), False),
        ],
    )


def _drop_rows(fx, rows):
    ref, cand, origin = fx[SNAP]
    keep = np.setdiff1d(np.arange(cand.shape[0]), rows)
    fx[SNAP][1], fx[SNAP][2] = cand[keep], origin[keep]


def _count_facts(n_cand, matched, count_ok, ref_ok, cand_ok):
    t = ["snapshot_tables", "snap=63"]
    return [
        fact("N_ref", t + ["n_ref"], N),
        fact("N_cand", t + ["n_cand"], n_cand),
        fact("M", t + ["matched"], matched),
        fact("count threshold passed", path_threshold("count"), count_ok),
        fact("matching vs N_ref passed", path_threshold("matching_vs_n_ref"), ref_ok),
        fact("matching vs N_cand passed", path_threshold("matching_vs_n_cand"), cand_ok),
    ]


def s1e(fx):
    _drop_rows(fx, [0])
    return (
        "count and matching at exactly 0.1%: one candidate galaxy removed (N_cand = M = 999)",
        "pass",
        [],
        _count_facts(999, 999, True, True, True),
    )


def s1f(fx):
    _drop_rows(fx, [0, 1])
    return (
        "count and matching just beyond 0.1%: two candidate galaxies removed (N_cand = M = 998)",
        "fail",
        ["Snap 63: count", "Snap 63: matching_vs_n_ref"],
        _count_facts(998, 998, False, False, True),
    )


def s1g(fx):
    ref, cand, origin = fx[SNAP]
    cand = cand.copy()
    cand["Pos"][0, 0] = -1.0  # no fixture position is negative: this galaxy is unmatched
    extra = cand[1:2].copy()
    extra["Pos"][0, 0] = -2.0
    fx[SNAP][1] = np.concatenate([cand, extra])
    origin = origin.copy()
    origin[0] = -1
    fx[SNAP][2] = np.concatenate([origin, [-1]])
    return (
        "candidate-side matching alone: N_ref = 1000, N_cand = 1001, M = 999",
        "fail",
        ["Snap 63: matching_vs_n_cand"],
        _count_facts(1001, 999, True, True, False),
    )


def s2a(fx):
    _scale(fx[SNAP][1], "StellarMass", slice(None), 1 + 5e-7)
    return (
        "tolerance arithmetic: every StellarMass candidate 5e-7 relative from its reference",
        "pass",
        [],
        [
            fact("StellarMass within", path_prop("StellarMass", "within_tolerance"), N, N),
            fact("StellarMass bit-identical", path_prop("StellarMass", "bit_identical"), 0, N),
        ],
    )


def s2b(fx):
    _scale(fx[SNAP][1], "StellarMass", slice(0, 110), 1 + 2e-6)
    return (
        "tolerance arithmetic: StellarMass 2e-6 relative from its reference in 11% of galaxies",
        "fail",
        ["Snap 63: float:StellarMass:over_M", "Snap 63: float:StellarMass:over_M_sig"],
        [fact("StellarMass within", path_prop("StellarMass", "within_tolerance"), 890, N)],
    )


def _set(fx, side, prop, rows, value):
    """Set a float32 property on one side (0 = reference, 1 = candidate) at snapshot 63."""
    fx[SNAP][side][prop][rows] = value


def s3a(fx):
    _set(fx, 0, "HotGas", slice(None), 0.0)
    _set(fx, 1, "HotGas", slice(None), 5e-11)
    return (
        "zero references: HotGas reference 0 and candidate 5e-11 for every galaxy",
        "pass",
        [],
        [
            fact("HotGas within", path_prop("HotGas", "within_tolerance"), N, N),
            fact("HotGas M_sig", path_prop("HotGas", "M_sig"), 0),
        ],
    )


def s3b(fx):
    _set(fx, 0, "HotGas", slice(None), 0.0)
    _set(fx, 1, "HotGas", slice(None), 0.0)
    _set(fx, 1, "HotGas", slice(0, 110), 2e-10)
    return (
        "zero references: HotGas reference 0, candidate 2e-10 in 11% of galaxies (else 0)",
        "fail",
        ["Snap 63: float:HotGas:over_M", "Snap 63: float:HotGas:over_M_sig"],
        [
            fact("HotGas within", path_prop("HotGas", "within_tolerance"), 890, N),
            fact("HotGas M_sig", path_prop("HotGas", "M_sig"), 110),
            fact("HotGas within over M_sig", path_prop("HotGas", "within_tolerance_sig"), 0, 110),
        ],
    )


def s3c(fx):
    _set(fx, 0, "HotGas", slice(None), 1e-12)
    _set(fx, 1, "HotGas", slice(None), 1e-12 * (1 + 1e-5))
    return (
        "tiny non-zero references: HotGas reference 1e-12, candidate 1e-12 (1 + 1e-5)",
        "fail",
        ["Snap 63: float:HotGas:over_M"],
        [
            fact("HotGas within (relative rule)", path_prop("HotGas", "within_tolerance"), 0, N),
            fact("HotGas M_sig (inside the band)", path_prop("HotGas", "M_sig"), 0),
        ],
    )


def _float32_spacing(ref32):
    """Distance from each float32 value to the next float32 above it, in float64."""
    return (np.nextafter(ref32, np.float32(np.inf)) - ref32).astype(np.float64)


def s4a(fx):
    ref, cand, _ = fx[SNAP]
    r = ref["Mvir"].astype(np.float64)
    cand["Mvir"] = r + 0.4 * _float32_spacing(ref["Mvir"])
    if not np.all(cand["Mvir"] != r):
        raise AssertionError("S4a: a candidate equals its reference before rounding")
    return (
        "precision: Mimic float64 Mvir 0.4 float32 ulp above the reference",
        "pass",
        [],
        [
            fact("Mvir bit-identical after rounding", path_prop("Mvir", "bit_identical"), N, N),
            fact(
                "Mvir within_tolerance_unrounded",
                path_prop("Mvir", "within_tolerance_unrounded"),
                N,
                N,
            ),
        ],
    )


#: S4b's design: galaxies outside 1e-6 before rounding but inside after (OUT_IN) and
#: inside before but outside after (IN_OUT).
OUT_IN, IN_OUT = 150, 50


def s4b(fx):
    ref, cand, _ = fx[SNAP]
    r32 = ref["Rvir"]
    r = r32.astype(np.float64)
    u = _float32_spacing(r32)
    t = cl.REL_TOL * r / u  # the 1e-6 boundary, in float32 ulps above r
    frac = t - np.floor(t)
    out_in = np.nonzero((frac > 0.1) & (frac < 0.4))[0][:OUT_IN]
    in_out = np.nonzero((frac > 0.6) & (frac < 0.9))[0][:IN_OUT]
    if out_in.size != OUT_IN or in_out.size != IN_OUT:
        raise AssertionError("S4b: the fixture lacks galaxies for the design")
    m = r.copy()
    # Just outside 1e-6 before rounding, and nearer the float32 inside the boundary.
    m[out_in] = r[out_in] + (t[out_in] + (0.5 - frac[out_in]) / 2) * u[out_in]
    # Just inside 1e-6 before rounding, and nearer the float32 outside the boundary.
    m[in_out] = r[in_out] + (t[in_out] - (frac[in_out] - 0.5) / 2) * u[in_out]
    cand["Rvir"] = m
    return (
        "precision: Mimic float64 Rvir straddling 1e-6 before rounding "
        "(%d just outside that round inside, %d just inside that round outside)" % (OUT_IN, IN_OUT),
        "pass",
        [],
        [
            fact(
                "Rvir within_tolerance_unrounded (would fail the gate)",
                path_prop("Rvir", "within_tolerance_unrounded"),
                N - OUT_IN,
                N,
            ),
            fact(
                "Rvir within_tolerance (gated, after rounding)",
                path_prop("Rvir", "within_tolerance"),
                N - IN_OUT,
                N,
            ),
        ],
    )


def s5a(fx):
    _set(fx, 0, "ColdGas", slice(0, 200), np.nan)
    _set(fx, 1, "ColdGas", slice(0, 200), np.nan)
    return (
        "non-finite: ColdGas NaN on both sides in 200 galaxies",
        "pass",
        [],
        [fact("ColdGas within", path_prop("ColdGas", "within_tolerance"), N, N)],
    )


def s5b(fx):
    _set(fx, 1, "ColdGas", slice(0, 55), np.nan)
    _set(fx, 1, "ColdGas", slice(55, 110), np.inf)
    return (
        "non-finite: ColdGas NaN (55) or +inf (55) in the candidate only, 11% of galaxies",
        "fail",
        ["Snap 63: float:ColdGas:over_M", "Snap 63: float:ColdGas:over_M_sig"],
        [fact("ColdGas within", path_prop("ColdGas", "within_tolerance"), 890, N)],
    )


def s5c(fx):
    for side in (0, 1):
        _set(fx, side, "ColdGas", slice(0, 100), np.inf)
        _set(fx, side, "ColdGas", slice(100, 200), -np.inf)
    return (
        "non-finite: the same infinity on both sides (+inf in 100, -inf in 100 galaxies)",
        "pass",
        [],
        [fact("ColdGas within", path_prop("ColdGas", "within_tolerance"), N, N)],
    )


def s6(fx):
    ref, cand, _ = fx[SNAP]
    big = 2**60 + 1024 * np.arange(N, dtype=np.int64)
    ref["SimulationHaloIndex"] = big
    cand["MostBoundID"] = big
    cand["MostBoundID"][0:6] ^= 1
    as_float = cand["MostBoundID"][0:6].astype(np.float64) == big[0:6].astype(np.float64)
    if not np.all(as_float):
        raise AssertionError("S6: the IDs must be indistinguishable in float64")
    return (
        "64-bit integers: MostBoundID above 2^60 differing only in the lowest bit in 6 galaxies",
        "fail",
        ["Snap 63: int:SimulationHaloIndex"],
        [
            fact(
                "SimulationHaloIndex exactly equal",
                path_prop("SimulationHaloIndex", "exact_equal"),
                N - 6,
                N,
            )
        ],
    )


CONTROLS = (
    ("S0", s0),
    ("S1a", s1a),
    ("S1b", s1b),
    ("S1c", s1c),
    ("S1d", s1d),
    ("S1e", s1e),
    ("S1f", s1f),
    ("S1g", s1g),
    ("S2a", s2a),
    ("S2b", s2b),
    ("S3a", s3a),
    ("S3b", s3b),
    ("S3c", s3c),
    ("S4a", s4a),
    ("S4b", s4b),
    ("S5a", s5a),
    ("S5b", s5b),
    ("S5c", s5c),
    ("S6", s6),
)


def build(out, comparator):
    for name, make in CONTROLS:
        fx = base()
        result = make(fx)
        description, verdict, failures, facts = result[:4]
        groups = result[4] if len(result) > 4 else []
        snapshots = [(s, fx[s][0], fx[s][1], fx[s][2]) for s in cl.T1_SNAPSHOTS]
        own = cl.evaluate(snapshots, MAPPINGS, UNCOMPARED)
        if own["verdict"] != verdict or sorted(own["failures"]) != sorted(failures):
            raise AssertionError(
                "%s: the control's own evaluation (%s, %s) is not its design (%s, %s)"
                % (name, own["verdict"], own["failures"], verdict, failures)
            )
        cdir = os.path.join(out, name)
        os.makedirs(cdir)
        ref_path = os.path.join(cdir, "reference.hdf5")
        cl.write_reference(ref_path, {s: fx[s][0] for s in cl.T1_SNAPSHOTS}, ORDER)
        cl.write_candidate(
            os.path.join(cdir, "candidate"), {s: fx[s][1] for s in cl.T1_SNAPSHOTS}, PARTITIONS
        )
        digest = cl.sha256_file(ref_path)
        expect = {
            "verdict": verdict,
            "exit": 0 if verdict == "pass" else 1,
            "printed": "verdict: pass (not T1-eligible)" if verdict == "pass" else "verdict: fail",
            "t1_eligible": False,
            "failures": failures,
            "tables": own["tables"],
            "facts": facts,
            "groups": groups,
        }
        doc = cl.expectation(
            name,
            description,
            comparator,
            ref_path,
            digest,
            os.path.join(cdir, "candidate"),
            partitions=PARTITIONS,
            expect=expect,
        )
        cl.write_expectation(cdir, doc)
        print("built %-4s %s" % (name, description))
        if name == "S0":
            build_s0e(out, comparator, ref_path, os.path.join(cdir, "candidate"))


def build_s0e(out, comparator, ref_path, candidate):
    cdir = os.path.join(out, "S0e")
    os.makedirs(cdir)
    doc = cl.expectation(
        "S0e",
        "S0's files with the T1 reference's digest as --reference-sha256 (rule 1)",
        comparator,
        ref_path,
        cl.import_comparator(comparator).T1_REFERENCE_SHA256,
        candidate,
        partitions=PARTITIONS,
        expect={
            "verdict": "error",
            "exit": 2,
            "error_contains": "does not equal --reference-sha256",
        },
    )
    cl.write_expectation(cdir, doc)
    print("built S0e  S0's files with the T1 reference's digest")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--out", required=True, help="new directory for the control directories")
    parser.add_argument("--comparator", required=True, help="compare_sage26.py to run")
    args = parser.parse_args(argv)
    os.makedirs(args.out, exist_ok=True)
    build(args.out, args.comparator)
    return 0


if __name__ == "__main__":
    sys.exit(main())
