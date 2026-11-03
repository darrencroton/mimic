#!/usr/bin/env python3
"""Known-bad (and one known-good) controls derived from G1's Mimic-layout copy.

Each control starts from ``to_mimic_layout.load`` (the T1 reference's eight snapshots
in Mimic's layout, rows in reference row order), alters the candidate side only, and is
written with the same documented partition shuffle as G1. "The first n galaxies" always
means by row order of the reference's ``Snap_<N>`` group. Each count the plan states
(7307, 62, 359, 4093, 3348, 2000) is recomputed from the reference and checked against
the plan's literal before anything is written.

Pass-or-fail controls carry the control's own expected tables (``control_lib.evaluate``)
and the facts their criterion names; error controls carry the message fragment that
identifies the rule which stops the comparator.
"""

import argparse
import os
import sys

import control_lib as cl
import numpy as np
import to_mimic_layout as tml
from numpy.lib.recfunctions import repack_fields

T1_REFERENCE_SHA256 = "19977093c0497339b954cb4fc0417ff249fde0de794319c224a8b9ea9e001d18"


def ceil_fraction(n, percent_times_10):
    """ceil(n * p / 1000) in integers, for p given in tenths of a percent."""
    return -(-n * percent_times_10 // 1000)


def require(value, literal, what):
    if value != literal:
        raise AssertionError("%s is %r, the plan states %r" % (what, value, literal))
    return value


def fresh(base):
    return {s: [ref, cand.copy(), origin.copy()] for s, (ref, cand, origin) in base.items()}


def prop(snap, ds, key):
    return ["snapshot_tables", "snap=%d" % snap, "properties", ds, key]


def threshold(snap, name):
    return ["snapshot_tables", "snap=%d" % snap, "thresholds", "threshold=%s" % name, "passed"]


def fact(label, path, equals, of=None):
    out = {"label": label, "path": path, "equals": equals}
    if of is not None:
        out["of"] = of
    return out


def scale_float32(cand, name, rows, factor):
    cand[name][rows] = (cand[name][rows].astype(np.float64) * factor).astype(np.float32)


def nonzero_rows(ref, ds):
    return np.nonzero(ref[ds] != 0)[0]


# ---------------------------------------------------------------------------
# Pass-or-fail controls: each returns (description, designed outcome, facts).
# ---------------------------------------------------------------------------


def b1(fx):
    ref, cand, _ = fx[63]
    n = require(ceil_fraction(ref["SnapNum"].size, 200), 7307, "20% of Snap_63, rounded up")
    rows = nonzero_rows(ref, "StellarMass")[:n]
    scale_float32(cand, "StellarMass", rows, 1 + 1e-5)
    within = ref["SnapNum"].size - n
    return (
        "StellarMass x (1 + 1e-5) in the first %d galaxies with non-zero StellarMass at 63" % n,
        ("fail", "Snap 63: float:StellarMass:"),
        [
            fact(
                "StellarMass within at 63",
                prop(63, "StellarMass", "within_tolerance"),
                within,
                ref["SnapNum"].size,
            ),
            fact("StellarMass over_M gate", threshold(63, "float:StellarMass:over_M"), False),
        ],
    )


def b2(fx):
    ref, cand, origin = fx[27]
    n_ref = ref["SnapNum"].size
    n = require(ceil_fraction(n_ref, 2), 62, "0.2% of Snap_27, rounded up")
    fx[27][1], fx[27][2] = cand[n:], origin[n:]
    t = ["snapshot_tables", "snap=27"]
    return (
        "the first %d rows removed from snapshot 27" % n,
        ("fail", ["Snap 27: count", "Snap 27: matching_vs_n_ref"]),
        [
            fact("N_ref at 27", t + ["n_ref"], n_ref),
            fact("N_cand at 27", t + ["n_cand"], n_ref - n),
            fact("M at 27", t + ["matched"], n_ref - n),
            fact("count threshold at 27", threshold(27, "count"), False),
            fact("matching vs N_ref at 27", threshold(27, "matching_vs_n_ref"), False),
            fact("matching vs N_cand at 27", threshold(27, "matching_vs_n_cand"), True),
        ],
    )


def b3(fx):
    ref, cand, _ = fx[32]
    n_ref = ref["SnapNum"].size
    n = require(ceil_fraction(n_ref, 10), 359, "1% of Snap_32, rounded up")
    if not np.all(np.isin(cand["Regime"][:n], (0, 1))):
        raise AssertionError("B3: Regime is not 0 or 1 in the first %d galaxies" % n)
    cand["Regime"][:n] = 1 - cand["Regime"][:n]
    return (
        "Regime changed 0 <-> 1 in the first %d galaxies at snapshot 32" % n,
        ("fail", ["Snap 32: int:Regime"]),
        [fact("Regime exactly equal at 32", prop(32, "Regime", "exact_equal"), n_ref - n, n_ref)],
    )


def _gate37(fx, n, literal_within, verdict):
    ref, cand, _ = fx[37]
    n_ref = require(ref["SnapNum"].size, 37203, "N_ref at 37")
    rows = nonzero_rows(ref, "StellarMass")[:n]
    scale_float32(cand, "StellarMass", rows, 1 + 1e-4)
    within = require(n_ref - n, literal_within, "pairs within at 37")
    design = ("fail", "Snap 37: float:StellarMass:") if verdict == "fail" else ("pass", [])
    return (
        "StellarMass x (1 + 1e-4) in the first %d galaxies with non-zero StellarMass at 37" % n,
        design,
        [
            fact(
                "StellarMass within at 37",
                prop(37, "StellarMass", "within_tolerance"),
                within,
                n_ref,
            ),
            fact(
                "StellarMass over_M gate at 37",
                threshold(37, "float:StellarMass:over_M"),
                verdict == "pass",
            ),
        ],
    )


def b4(fx):
    n = require(ceil_fraction(fx[37][0]["SnapNum"].size, 110), 4093, "11% of 37203, up")
    return _gate37(fx, n, 33110, "fail")


def g2(fx):
    n = require(fx[37][0]["SnapNum"].size * 90 // 1000, 3348, "9% of 37203, down")
    return _gate37(fx, n, 33855, "pass")


def b7(fx):
    ref, cand, origin = fx[16]
    n_ref = ref["SnapNum"].size
    a, b, c = np.nonzero(ref["Type"] == 0)[0][:3]
    cand["Pos"][b] = cand["Pos"][a]
    cand["Pos"][c] = cand["Pos"][a]
    origin[[a, b, c]] = -1
    t = ["snapshot_tables", "snap=16"]
    sides = t + ["sides"]
    return (
        "at snapshot 16, the positions of Type 0 galaxies (rows %d, %d) set to row %d's"
        % (b, c, a),
        ("pass", []),
        [
            fact("M at 16", t + ["matched"], require(n_ref - 3, 3905, "M at 16")),
            fact("reference unmatched", sides + ["reference", "unmatched"], 3),
            fact("candidate unmatched", sides + ["candidate", "unmatched"], 3),
            fact("candidate duplicate-key", sides + ["candidate", "duplicate_key"], 3),
            fact("reference duplicate-key", sides + ["reference", "duplicate_key"], 0),
        ],
    )


def _b10_facts(within, m_sig, within_sig, n_ref):
    return [
        fact("HotGas within at 63 (over M)", prop(63, "HotGas", "within_tolerance"), within, n_ref),
        fact("HotGas over_M gate", threshold(63, "float:HotGas:over_M"), True),
        fact("HotGas M_sig at 63", prop(63, "HotGas", "M_sig"), m_sig),
        fact(
            "HotGas within over M_sig",
            prop(63, "HotGas", "within_tolerance_sig"),
            within_sig,
            m_sig,
        ),
        fact("HotGas over_M_sig gate", threshold(63, "float:HotGas:over_M_sig"), False),
    ]


B10_FAIL = ("fail", ["Snap 63: float:HotGas:over_M_sig"])


def b10a(fx):
    ref, cand, _ = fx[63]
    zero = require(int((ref["HotGas"] == 0).sum()), 34452, "zero HotGas references at 63")
    cand["HotGas"] = 0.0
    n_ref = ref["SnapNum"].size
    return (
        "HotGas set to 0 for every galaxy at snapshot 63",
        B10_FAIL,
        _b10_facts(zero, n_ref - zero, 0, n_ref),
    )


def b10b(fx):
    ref, cand, _ = fx[63]
    zero = require(int((ref["HotGas"] == 0).sum()), 34452, "zero HotGas references at 63")
    cand["HotGas"] = np.float32(5e-11)
    n_ref = ref["SnapNum"].size
    return (
        "HotGas set to 5e-11 for every galaxy at snapshot 63",
        B10_FAIL,
        _b10_facts(zero, n_ref - zero, 0, n_ref),
    )


def b10c(fx):
    ref, cand, _ = fx[63]
    n_ref = ref["SnapNum"].size
    rows = np.nonzero(ref["HotGas"] == 0)[0][:2000]
    require(rows.size, 2000, "zero-reference HotGas galaxies at 63")
    cand["HotGas"][rows] = np.float32(1e-3)
    sig = require(n_ref - 34452 + 2000, 4082, "M_sig at 63")
    return (
        "HotGas set to 1e-3 in the first 2000 galaxies whose reference HotGas is zero, at 63",
        B10_FAIL,
        _b10_facts(require(n_ref - 2000, 34534, "within at 63"), sig, sig - 2000, n_ref),
    )


PASS_FAIL = (
    ("B1", b1),
    ("B2", b2),
    ("B3", b3),
    ("B4", b4),
    ("G2", g2),
    ("B7", b7),
    ("B10a", b10a),
    ("B10b", b10b),
    ("B10c", b10c),
)


def check_design(name, design, own):
    verdict, failures = design
    if own["verdict"] != verdict:
        raise AssertionError("%s: own evaluation gives %s" % (name, own["verdict"]))
    if isinstance(failures, str):
        # Only this property at this snapshot fails, its over-M gate among them.
        bad = [f for f in own["failures"] if not f.startswith(failures)]
        if bad or failures + "over_M" not in own["failures"]:
            raise AssertionError("%s: own evaluation fails %s" % (name, own["failures"]))
    elif sorted(own["failures"]) != sorted(failures):
        raise AssertionError("%s: own evaluation fails %s" % (name, own["failures"]))


def build_pass_fail(out, comparator, reference, base, mappings, uncompared):
    for name, make in PASS_FAIL:
        fx = fresh(base)
        description, design, facts = make(fx)
        own = cl.evaluate(tml.snapshot_list(fx), mappings, uncompared)
        check_design(name, design, own)
        cdir = os.path.join(out, name)
        os.makedirs(cdir)
        candidate = os.path.join(cdir, "candidate")
        cl.write_candidate(candidate, {s: fx[s][1] for s in cl.T1_SNAPSHOTS}, tml.PARTITIONS)
        verdict = own["verdict"]
        expect = {
            "verdict": verdict,
            "exit": 0 if verdict == "pass" else 1,
            "printed": "verdict: %s" % verdict,
            "t1_eligible": True,
            "failures": own["failures"],
            "tables": own["tables"],
            "facts": facts,
        }
        doc = cl.expectation(
            name,
            description,
            comparator,
            reference,
            T1_REFERENCE_SHA256,
            candidate,
            partitions=tml.PARTITIONS,
            expect=expect,
        )
        cl.write_expectation(cdir, doc)
        print("built %-5s %s" % (name, description))


# ---------------------------------------------------------------------------
# Error controls
# ---------------------------------------------------------------------------


def without_field(table, field):
    return repack_fields(table[[n for n in table.dtype.names if n != field]])


def build_errors(out, comparator, reference, base):
    tables = {s: base[s][1] for s in cl.T1_SNAPSHOTS}
    mixed = dict(tables)
    mixed[37] = tables[32]
    controls = (
        (
            "B5",
            "ColdGas removed from every partition",
            {s: without_field(t, "ColdGas") for s, t in tables.items()},
            {},
            "field-map property ColdGas (for ColdGas) absent",
        ),
        (
            "B8a",
            "G1's candidate with partition file model_005.hdf5 missing",
            tables,
            {"skip_partition": 5},
            "missing ['model_005.hdf5']",
        ),
        (
            "B8b",
            "G1's candidate with an extra model_008.hdf5",
            tables,
            {"extra_partition": True},
            "unexpected ['model_008.hdf5']",
        ),
        (
            "B8c",
            "G1's candidate with HotGas missing from model_003.hdf5's Snap063/Galaxies",
            tables,
            {"drop_field": (3, 63, "HotGas")},
            "compound field names, types or shapes differ from the first partition",
        ),
        (
            "B9",
            "the candidate's Snap037 group holding snapshot 32's rows",
            mixed,
            {},
            "Snap 37: 35882 rows carry another SnapNum (snapshot mix-up)",
        ),
    )
    for name, description, data, options, fragment in controls:
        cdir = os.path.join(out, name)
        os.makedirs(cdir)
        candidate = os.path.join(cdir, "candidate")
        cl.write_candidate(candidate, data, tml.PARTITIONS, **options)
        doc = cl.expectation(
            name,
            description,
            comparator,
            reference,
            T1_REFERENCE_SHA256,
            candidate,
            partitions=tml.PARTITIONS,
            expect={"verdict": "error", "exit": 2, "error_contains": fragment},
        )
        cl.write_expectation(cdir, doc)
        print("built %-5s %s" % (name, description))


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--reference", required=True, help="the T1 reference (read only)")
    parser.add_argument("--out", required=True, help="directory for the control directories")
    parser.add_argument("--comparator", required=True, help="compare_sage26.py to run")
    args = parser.parse_args(argv)
    mappings, uncompared, base = tml.load(args.reference, args.comparator)
    build_pass_fail(args.out, args.comparator, args.reference, base, mappings, uncompared)
    build_errors(args.out, args.comparator, args.reference, base)
    return 0


if __name__ == "__main__":
    sys.exit(main())
