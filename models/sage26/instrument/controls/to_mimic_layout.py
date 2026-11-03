#!/usr/bin/env python3
"""Known-good control G1: the T1 reference rewritten in Mimic's layout.

Reads T1's eight snapshots from the SAGE26 reference and writes them as a Mimic-layout
candidate using ``field_map.yaml``: each mapped dataset goes to its Mimic property with
the map's Mimic storage type, divided by the map's factor (the factor inverted), and
``Pos``, ``Vel`` and ``Spin`` become 3-element float32 arrays. The candidate holds only
the mapped properties, and only T1's eight snapshots (as a Mimic mini-Millennium run
does: it writes ``Snap016`` ... ``Snap063``).

Rows go to 8 partition files in the documented shuffled order of
``control_lib.partition_rows``: per snapshot N, the rows are permuted with
``numpy.random.default_rng(20261103 + N).permutation(n)`` and the permutation is cut
into 8 contiguous near-equal chunks (``numpy.array_split``); partition p holds chunk p.

Controls written: G1 (the reference against its copy), G1s63 (the same with
``--snapshots 63``) and B6 (G1 with a wrong ``--reference-sha256``). ``perturb.py``
imports this module to derive the known-bad candidates from the same copy.
"""

import argparse
import os
import sys

import control_lib as cl
import h5py
import numpy as np

PARTITIONS = 8
WRONG_SHA256 = "0" * 64


def load_pairing(comparator):
    """The pairing from the ``field_map.yaml`` beside ``comparator``.

    The map is read with the comparator's own parser, so the copy is built from exactly
    the entries the comparator will use. Returns ``(mappings, uncompared)``.
    """
    compare_sage26 = cl.import_comparator(comparator)
    entries, _ = compare_sage26.load_field_map(
        os.path.join(os.path.dirname(os.path.abspath(comparator)), "field_map.yaml")
    )
    mappings, uncompared = [], []
    for ds, e in entries.items():
        if "uncompared" in e:
            uncompared.append(ds)
            continue
        mappings.append(
            cl.Mapping(
                ds,
                e["kind"],
                e["sage26_type"],
                e["mimic"],
                e["component"],
                e["mimic_type"],
                e["factor"],
            )
        )
    return mappings, uncompared


def read_reference(path, snapshots):
    """``{snap: {dataset: array}}`` for the requested snapshots, in native byte order."""
    out = {}
    with h5py.File(path, "r") as f:
        for snap in snapshots:
            group = f["Snap_%d" % snap]
            data = {}
            for ds in group:
                arr = group[ds][()]
                data[ds] = arr.astype(arr.dtype.newbyteorder("="))
            out[snap] = data
    return out


def to_mimic(ref, mappings):
    """The Mimic-layout table holding one snapshot's reference values, in row order."""
    n = ref["SnapNum"].shape[0]
    cand = np.zeros(n, dtype=cl.mimic_dtype(mappings))
    for mp in mappings:
        r = ref[mp.ds]
        if mp.kind == "float":
            values = (r.astype(np.float64) / np.float64(mp.factor)).astype(mp.mimic_type)
        else:
            values = r.astype(mp.mimic_type)
            if not np.array_equal(values.astype(np.int64), r.astype(np.int64)):
                raise AssertionError("%s does not fit Mimic's %s" % (mp.ds, mp.mimic_type))
        if mp.component is None:
            cand[mp.mimic] = values
        else:
            cand[mp.mimic][:, mp.component] = values
    return cand


def load(reference, comparator):
    """Everything a reference-derived control starts from.

    Returns ``(mappings, uncompared, fixture)`` where ``fixture[snap]`` is
    ``[ref, cand, origin]``: the reference datasets, the Mimic-layout copy in reference
    row order, and each copy row's reference row.
    """
    mappings, uncompared = load_pairing(comparator)
    refs = read_reference(reference, cl.T1_SNAPSHOTS)
    fixture = {}
    for snap in cl.T1_SNAPSHOTS:
        ref = refs[snap]
        fixture[snap] = [ref, to_mimic(ref, mappings), np.arange(ref["SnapNum"].shape[0])]
    return mappings, uncompared, fixture


def snapshot_list(fixture, snapshots=cl.T1_SNAPSHOTS):
    return [(s, fixture[s][0], fixture[s][1], fixture[s][2]) for s in snapshots]


def g1_facts(fixture, mappings, own, snapshots):
    """G1's facts: sizes, bit-identity, factors other than 1, uncompared summaries."""
    sizes, bits, other = [], [], []
    for s in snapshots:
        n = int(fixture[s][0]["SnapNum"].shape[0])
        t = ["snapshot_tables", "snap=%d" % s]
        for key in ("n_ref", "n_cand", "matched"):
            sizes.append({"path": t + [key], "equals": n})
        for mp in mappings:
            p = t + ["properties", mp.ds]
            if mp.factor == 1.0:
                key = "exact_equal" if mp.kind == "int" else "bit_identical"
                bits.append({"path": p + [key], "equals": n})
            else:
                other.append({"path": p + ["within_tolerance"], "equals": n})
                b = own["tables"][str(s)]["properties"][mp.ds]["bit_identical"]
                other.append({"path": p + ["bit_identical"], "equals": b})
    n_factor1 = sum(1 for mp in mappings if mp.factor == 1.0)
    groups = [
        {"label": "M = N_ref = N_cand at every snapshot", "items": sizes},
        {
            "label": "every factor-1 mapping 100%% bit-identical (%d mappings x %d snapshots)"
            % (n_factor1, len(snapshots)),
            "items": bits,
        },
    ]
    notes = []
    if other:
        groups.append(
            {
                "label": "mappings with a factor other than 1: within tolerance for 100% of "
                "pairs, bit-identical fraction as the control computes it",
                "items": other,
            }
        )
    else:
        notes.append("field_map.yaml has no mapping with a factor other than 1")
    return groups, notes


def uncompared_facts(snapshots):
    """Rule 7's summaries on the T1 reference: g_max zero everywhere; mergeType and
    mergeIntoID equal to their creation values at snapshot 63 and not before it."""
    facts, gmax = [], []
    for s in snapshots:
        u = ["snapshot_tables", "snap=%d" % s, "uncompared"]
        gmax.append({"path": u + ["g_max", "nonzero"], "equals": 0})
        for ds in ("mergeType", "mergeIntoID"):
            path = u + [ds, "differ_from_creation"]
            if s == 63:
                facts.append({"label": "%s differs from creation at 63" % ds, "path": path})
                facts[-1]["equals"] = 0
            else:
                facts.append({"label": "%s differs from creation at %d" % (ds, s), "path": path})
                facts[-1]["at_least"] = 1
    return facts, [{"label": "g_max non-zero count 0 at every snapshot", "items": gmax}]


def pass_expectation(fixture, mappings, uncompared, snapshots, t1):
    own = cl.evaluate(snapshot_list(fixture, snapshots), mappings, uncompared)
    if own["verdict"] != "pass":
        raise AssertionError("G1: the control's own evaluation fails: %s" % own["failures"])
    groups, notes = g1_facts(fixture, mappings, own, snapshots)
    facts, gmax = uncompared_facts(snapshots)
    return {
        "verdict": "pass",
        "exit": 0,
        "printed": "verdict: pass" if t1 else "verdict: pass (not T1-eligible)",
        "t1_eligible": t1,
        "failures": [],
        "tables": own["tables"],
        "facts": facts,
        "groups": groups + gmax,
        "notes": notes,
    }


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--reference", required=True, help="the T1 reference (read only)")
    parser.add_argument("--reference-sha256", required=True, help="its SHA-256")
    parser.add_argument("--out", required=True, help="directory for the control directories")
    parser.add_argument("--comparator", required=True, help="compare_sage26.py to run")
    args = parser.parse_args(argv)

    mappings, uncompared, fixture = load(args.reference, args.comparator)
    g1 = os.path.join(args.out, "G1")
    candidate = os.path.join(g1, "candidate")
    os.makedirs(g1)
    cl.write_candidate(candidate, {s: fixture[s][1] for s in cl.T1_SNAPSHOTS}, PARTITIONS)

    common = dict(partitions=PARTITIONS)
    controls = (
        (
            "G1",
            "the T1 reference against its Mimic-layout copy, all eight snapshots",
            args.reference_sha256,
            None,
            pass_expectation(fixture, mappings, uncompared, cl.T1_SNAPSHOTS, True),
        ),
        (
            "G1s63",
            "G1 with --snapshots 63 (eligibility needs all eight)",
            args.reference_sha256,
            "63",
            pass_expectation(fixture, mappings, uncompared, (63,), False),
        ),
        (
            "B6",
            "G1 with a wrong --reference-sha256 (rule 1)",
            WRONG_SHA256,
            None,
            {"verdict": "error", "exit": 2, "error_contains": "does not equal --reference-sha256"},
        ),
    )
    for name, description, digest, snaps, expect in controls:
        cdir = os.path.join(args.out, name)
        os.makedirs(cdir, exist_ok=name == "G1")
        doc = cl.expectation(
            name,
            description,
            args.comparator,
            args.reference,
            digest,
            candidate,
            snapshots=snaps,
            expect=expect,
            **common,
        )
        cl.write_expectation(cdir, doc)
        print("built %-5s %s" % (name, description))
    return 0


if __name__ == "__main__":
    sys.exit(main())
