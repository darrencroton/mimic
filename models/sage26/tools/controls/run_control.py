#!/usr/bin/env python3
"""Run the comparator on one control and check the outcome against its expect.json.

Usage: ``run_control.py CONTROL_DIR``. The comparator named in ``expect.json`` is run
with the same Python interpreter, writing ``report.json``, ``outliers.csv``,
``stdout.txt`` and ``stderr.txt`` into CONTROL_DIR (none of which may exist yet). The
script prints the control's expected and actual verdict and exit status and each fact
its criterion names, and exits 0 when every expectation is met, 1 otherwise.

Expectation keys (all under ``expect``):

- ``verdict``: ``pass``, ``fail`` or ``error``; ``exit``: the exit status (both required); ``printed``: the exact last line of standard output.
- ``t1_eligible``: the JSON's ``t1_eligible``.
- ``failures``: the exact list of failed thresholds (compared as a multiset).
- ``error_contains``: a fragment of the JSON's ``error`` message. An ``error`` must also
  have no ``snapshot_tables`` or ``failures`` in its JSON and leave no outliers CSV.
- ``tables``: the control's own expected per-snapshot counts (``control_lib.evaluate``):
  every one must equal the comparator's, and the compared datasets must be the same.
- ``facts``: ``{label, path, equals | at_least, of?}``, printed one per line.
- ``groups``: ``{label, items: [{path, equals}]}``, printed as one line each.
- ``outlier_rows``: ``{"Snap <N>: <dataset>": rows}``, the exact number of rows the
  outliers CSV holds for each snapshot and dataset; the CSV must hold no other row.
- ``notes``: lines printed as they are.

A ``path`` is a list of JSON keys; ``snap=<N>`` selects the snapshot table of snapshot N
and ``threshold=<name>`` the threshold of that name.
"""

import csv
import json
import os
import subprocess
import sys
from collections import Counter

OUTPUTS = ("report.json", "outliers.csv", "stdout.txt", "stderr.txt")


class Missing(Exception):
    pass


def resolve(doc, path):
    node = doc
    for key in path:
        if isinstance(key, str) and key.startswith("snap="):
            hits = [t for t in node if t.get("snapshot") == int(key[5:])]
        elif isinstance(key, str) and key.startswith("threshold="):
            hits = [t for t in node if t.get("name") == key[10:]]
        elif isinstance(node, dict) and key in node:
            node = node[key]
            continue
        else:
            raise Missing("/".join(str(k) for k in path))
        if len(hits) != 1:
            raise Missing("/".join(str(k) for k in path))
        node = hits[0]
    return node


def same(expected, actual):
    """Equality for JSON values; a boolean equals only a boolean (``True != 1`` here)."""
    if isinstance(expected, bool) or isinstance(actual, bool):
        return isinstance(expected, bool) and isinstance(actual, bool) and expected == actual
    return expected == actual


class Checker:
    def __init__(self):
        self.missed = []

    def line(self, ok, text):
        print("   [%s] %s" % ("ok" if ok else "MISSED", text))
        if not ok:
            self.missed.append(text)

    def fact(self, doc, f):
        try:
            actual = resolve(doc, f["path"])
        except Missing as exc:
            self.line(False, "%s: absent from the JSON (%s)" % (f["label"], exc))
            return
        if "at_least" in f:
            ok = isinstance(actual, int) and actual >= f["at_least"]
            want = ">= %s" % f["at_least"]
        else:
            ok = same(f["equals"], actual)
            want = json.dumps(f["equals"])
        shown = json.dumps(actual)
        if "of" in f and isinstance(actual, int) and f["of"]:
            shown = "%d/%d (%.6f)" % (actual, f["of"], actual / f["of"])
        self.line(ok, "%s: expected %s, actual %s" % (f["label"], want, shown))

    def group(self, doc, g):
        bad = []
        for item in g["items"]:
            try:
                actual = resolve(doc, item["path"])
            except Missing:
                actual = "<absent>"
            if not same(item["equals"], actual):
                bad.append(
                    "%s = %s (expected %s)" % ("/".join(item["path"]), actual, item["equals"])
                )
        n = len(g["items"])
        self.line(not bad and n > 0, "%s: %d of %d hold" % (g["label"], n - len(bad), n))
        for b in bad[:10]:
            print("          " + b)

    def outlier_rows(self, path, expected):
        """The outliers CSV's rows, counted per snapshot and dataset, against ``expected``."""
        try:
            with open(path, newline="", encoding="utf-8") as fh:
                rows = list(csv.DictReader(fh))
        except OSError as exc:
            self.line(False, "outliers CSV rows: cannot read %s (%s)" % (path, exc))
            return
        got = Counter("Snap %s: %s" % (row["snapshot"], row["property"]) for row in rows)
        self.line(
            got == Counter(expected),
            "outliers CSV rows: expected %s, actual %s" % (dict(expected), dict(got)),
        )

    def tables(self, doc, expected):
        tables = doc.get("snapshot_tables", [])
        got = [t["snapshot"] for t in tables]
        want = [int(s) for s in expected]
        if got != want:
            self.line(False, "compared snapshots %s, expected %s" % (got, want))
            return
        checked, bad, layout = 0, [], []
        for t in tables:
            e = expected[str(t["snapshot"])]
            pairs = [
                ("n_ref", e["n_ref"], t["n_ref"]),
                ("n_cand", e["n_cand"], t["n_cand"]),
                ("matched", e["matched"], t["matched"]),
                (
                    "reference unmatched",
                    e["unmatched_reference"],
                    t["sides"]["reference"]["unmatched"],
                ),
                (
                    "candidate unmatched",
                    e["unmatched_candidate"],
                    t["sides"]["candidate"]["unmatched"],
                ),
            ]
            if set(t["properties"]) != set(e["properties"]):
                layout.append(
                    "Snap %d compared datasets differ: only comparator %s, only control %s"
                    % (
                        t["snapshot"],
                        sorted(set(t["properties"]) - set(e["properties"])),
                        sorted(set(e["properties"]) - set(t["properties"])),
                    )
                )
            for ds, counts in e["properties"].items():
                for key, value in counts.items():
                    actual = t["properties"].get(ds, {}).get(key, "<absent>")
                    pairs.append(("%s %s" % (ds, key), value, actual))
            if set(t["uncompared"]) != set(e["uncompared"]):
                layout.append("Snap %d uncompared datasets differ" % t["snapshot"])
            for ds, summary in e["uncompared"].items():
                for key, value in summary.items():
                    actual = t["uncompared"].get(ds, {}).get(key, "<absent>")
                    pairs.append(("uncompared %s %s" % (ds, key), value, actual))
            for what, want_value, actual in pairs:
                checked += 1
                if not same(want_value, actual):
                    bad.append(
                        "Snap %d %s: expected %s, actual %s"
                        % (t["snapshot"], what, want_value, actual)
                    )
        self.line(
            not bad,
            "control's own evaluation: %d of %d per-snapshot values agree with the JSON"
            % (checked - len(bad), checked),
        )
        for b in bad[:10]:
            print("          " + b)
        if layout:
            # Dataset-set mismatches are not values, so they are tallied apart from them.
            self.line(False, "compared datasets differ at %d snapshot(s)" % len(layout))
            for b in layout[:10]:
                print("          " + b)


def run_control(control_dir):
    with open(os.path.join(control_dir, "expect.json"), encoding="utf-8") as fh:
        spec = json.load(fh)
    expect, args = spec["expect"], spec["args"]
    paths = {name: os.path.join(control_dir, name) for name in OUTPUTS}
    clash = [p for p in paths.values() if os.path.lexists(p)]
    if clash:
        print("== %s: refusing to run, outputs already exist: %s" % (spec["control"], clash))
        return 1
    argv = [
        sys.executable,
        spec["comparator"],
        "--reference",
        args["reference"],
        "--reference-sha256",
        args["reference_sha256"],
        "--candidate",
        args["candidate"],
        "--partitions",
        str(args["partitions"]),
        "--json",
        paths["report.json"],
        "--outliers",
        paths["outliers.csv"],
    ]
    if args.get("snapshots"):
        argv += ["--snapshots", args["snapshots"]]
    with open(paths["stdout.txt"], "w") as out, open(paths["stderr.txt"], "w") as err:
        status = subprocess.call(argv, stdout=out, stderr=err)
    with open(paths["stdout.txt"], encoding="utf-8") as fh:
        lines = fh.read().splitlines()
    last = lines[-1] if lines else ""
    try:
        with open(paths["report.json"], encoding="utf-8") as fh:
            doc = json.load(fh)
    except (OSError, ValueError) as exc:
        doc = {"verdict": "<no JSON: %s>" % exc}

    print("== %s: %s" % (spec["control"], spec["description"]))
    verdict = doc.get("verdict")
    want_exit = expect["exit"]
    print(
        "   expected: verdict %s, exit %d | actual: verdict %s, exit %d"
        % (expect["verdict"], want_exit, verdict, status)
    )
    c = Checker()
    c.line(verdict == expect["verdict"] and status == want_exit, "verdict and exit status")
    if "printed" in expect:
        c.line(last == expect["printed"], "last line printed: %r" % last)
    elif expect["verdict"] == "error":
        c.line(last.startswith("verdict: error: "), "last line printed: %r" % last)
    if "t1_eligible" in expect:
        c.line(
            same(expect["t1_eligible"], doc.get("t1_eligible")),
            "t1_eligible: expected %s, actual %s" % (expect["t1_eligible"], doc.get("t1_eligible")),
        )
    if expect["verdict"] == "error":
        message = doc.get("error") or ""
        c.line(expect["error_contains"] in message, "error message: %s" % message)
        c.line(
            "snapshot_tables" not in doc and "failures" not in doc,
            "no verdict tables in the JSON",
        )
        c.line(not os.path.lexists(paths["outliers.csv"]), "no outliers CSV written")
    if "failures" in expect:
        got = doc.get("failures")
        ok = got is not None and Counter(got) == Counter(expect["failures"])
        c.line(ok, "failed thresholds: expected %s, actual %s" % (expect["failures"], got))
        c.line(os.path.isfile(paths["outliers.csv"]), "outliers CSV written")
    if "outlier_rows" in expect:
        c.outlier_rows(paths["outliers.csv"], expect["outlier_rows"])
    for f in expect.get("facts", []):
        c.fact(doc, f)
    for g in expect.get("groups", []):
        c.group(doc, g)
    if "tables" in expect:
        c.tables(doc, expect["tables"])
    for note in expect.get("notes", []):
        print("   note: %s" % note)
    print("   result: %s" % ("MET" if not c.missed else "MISSED (%d)" % len(c.missed)))
    return 0 if not c.missed else 1


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    if len(argv) != 1:
        print("usage: run_control.py CONTROL_DIR", file=sys.stderr)
        return 2
    return run_control(argv[0])


if __name__ == "__main__":
    sys.exit(main())
