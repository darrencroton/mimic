#!/usr/bin/env python3
"""Map-error controls S7a-S7c for the T1 parity instrument.

Each control is a copy of ``compare_sage26.py`` beside an altered copy of
``field_map.yaml`` (the comparator always reads the map beside itself), run on S0's
fixture files, which ``make_fixture.py`` built from its own literal pairing:

- S7a: two float fields swapped (``ColdGas`` reads Mimic's ``StellarMass`` and
  ``StellarMass`` reads ``ColdGas``).
- S7b: two ``Pos`` components swapped (``Posx`` reads component 1, ``Posy`` component 0).
- S7c: a factor changed (``Mvir``, a core property, gets factor 1.001, with the
  ``factor_citation`` the map's structure requires, so the comparison runs).

None may give ``pass``. Each control is pinned to the outcome its alteration gives,
not merely "not pass", so a map the comparator rejects on structure before comparing
(an ``error`` of another kind) misses its expectation:

- S7a: ``fail``, failing exactly both float gates of ``ColdGas`` and ``StellarMass`` at
  every snapshot (the fixture's fields differ by at least 1/81 relative).
- S7b: ``error``, ``no galaxy matched (M = 0)``: the swapped components are the
  matching key, so no candidate key equals a reference key.
- S7c: ``fail``, failing exactly both float gates of ``Mvir`` at every snapshot.
"""

import argparse
import json
import os
import shutil
import sys

import control_lib as cl

ALTERATIONS = (
    (
        "S7a",
        "field_map.yaml with two float fields swapped (ColdGas <-> StellarMass)",
        (
            ("ColdGas", "    mimic: ColdGas", "    mimic: StellarMass"),
            ("StellarMass", "    mimic: StellarMass", "    mimic: ColdGas"),
        ),
    ),
    (
        "S7b",
        "field_map.yaml with two Pos components swapped (Posx <-> Posy)",
        (
            ("Posx", "    component: 0", "    component: 1"),
            ("Posy", "    component: 1", "    component: 0"),
        ),
    ),
    (
        "S7c",
        "field_map.yaml with a factor changed (Mvir factor 1 -> 1.001)",
        (
            (
                "Mvir",
                "    factor: 1",
                '    factor: 1.001\n    factor_citation: "control S7c: a deliberately wrong factor"',
            ),
        ),
    ),
)


def gate_failures(*datasets):
    """Both float gates of each dataset, at every T1 snapshot, as the JSON lists them."""
    return [
        "Snap %d: float:%s:%s" % (snap, ds, gate)
        for snap in cl.T1_SNAPSHOTS
        for ds in datasets
        for gate in ("over_M", "over_M_sig")
    ]


def fail_expectation(failures):
    return {"verdict": "fail", "exit": 1, "printed": "verdict: fail", "failures": failures}


EXPECTED = {
    "S7a": fail_expectation(gate_failures("ColdGas", "StellarMass")),
    "S7b": {"verdict": "error", "exit": 2, "error_contains": "no galaxy matched (M = 0)"},
    "S7c": fail_expectation(gate_failures("Mvir")),
}


def alter(text, edits):
    """Apply ``(dataset, old line, new text)`` edits, each inside its dataset's block."""
    lines = text.split("\n")
    for ds, old, new in edits:
        start = lines.index("  %s:" % ds)
        end = start + 1
        while end < len(lines) and lines[end].startswith("    "):
            end += 1
        hits = [i for i in range(start + 1, end) if lines[i] == old]
        if len(hits) != 1:
            raise AssertionError("%s: %r occurs %d times in its block" % (ds, old, len(hits)))
        lines[hits[0]] = new
    return "\n".join(lines)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--out", required=True, help="directory holding the S0 control")
    parser.add_argument("--comparator", required=True, help="compare_sage26.py to copy")
    args = parser.parse_args(argv)
    with open(os.path.join(args.out, "S0", "expect.json"), encoding="utf-8") as fh:
        s0 = json.load(fh)
    map_path = os.path.join(os.path.dirname(os.path.abspath(args.comparator)), "field_map.yaml")
    with open(map_path, encoding="utf-8") as fh:
        text = fh.read()
    for name, description, edits in ALTERATIONS:
        cdir = os.path.join(args.out, name)
        os.makedirs(cdir)
        altered = alter(text, edits)
        if altered == text:
            raise AssertionError("%s: the map is unchanged" % name)
        with open(os.path.join(cdir, "field_map.yaml"), "w", encoding="utf-8") as fh:
            fh.write(altered)
        comparator = os.path.join(cdir, "compare_sage26.py")
        shutil.copyfile(args.comparator, comparator)
        doc = cl.expectation(
            name,
            description + ", run on S0's files",
            comparator,
            s0["args"]["reference"],
            s0["args"]["reference_sha256"],
            s0["args"]["candidate"],
            partitions=s0["args"]["partitions"],
            expect=EXPECTED[name],
        )
        cl.write_expectation(cdir, doc)
        print("built %-4s %s" % (name, description))
    return 0


if __name__ == "__main__":
    sys.exit(main())
