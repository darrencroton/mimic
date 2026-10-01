#!/usr/bin/env python3
"""Run every planning-only check in this directory and summarize the outcome.

Runs, in order and sequentially:

1. ``check_plan_contract.py`` -- the frozen plan through the real PM parser,
   prompt renderers, surface matcher, anchor checks and corrupted variants;
2. ``c_const_view_probes.py`` -- positive/negative C compile probes against the
   real headers for the proposed const snapshot view;
3. ``sham_rank_oracle.py`` -- the independent numerical oracle and mutation
   kill matrix for the global-rank SHAM prescription.

Each script's stdout is captured to ``results/<name>.log`` and its JSON report to
``results/<name>.json``; the counts below are derived from the ``MIMIC_RESULT:``
markers, never from prose. The exit status is nonzero when any suite fails or
cannot run.

Usage::

    python3 docs/dev/snapshot-global-checks/run_checks.py [--results-dir DIR]
        [--pm-scripts DIR] [--skills-root DIR] [--cc CC]

These are planning experiments. A green run proves properties of the plan text,
of the installed PM tooling, of the C type system against the current headers,
and of the specified mathematics. It proves nothing about a driver integration,
which does not exist at the planning baseline.
"""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from checklib import DEFAULT_PM_SCRIPTS, DEFAULT_SKILLS_ROOT, HERE, REPO_ROOT  # noqa: E402

SUITES = (
    (
        "plan_contract",
        "check_plan_contract.py",
        ("--pm-scripts", "{pm_scripts}", "--skills-root", "{skills_root}"),
    ),
    ("const_view", "c_const_view_probes.py", ("--cc", "{cc}")),
    ("sham_rank", "sham_rank_oracle.py", ()),
)


def count_markers(text: str) -> dict[str, int]:
    counts = {"PASS": 0, "FAIL": 0, "SKIP": 0, "ERROR": 0, "WARN": 0}
    for line in text.splitlines():
        if line.startswith("MIMIC_RESULT: "):
            kind = line.split()[1]
            counts[kind] = counts.get(kind, 0) + 1
    return counts


def main() -> int:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument("--results-dir", type=Path, default=HERE / "results")
    parser.add_argument("--pm-scripts", type=Path, default=DEFAULT_PM_SCRIPTS)
    parser.add_argument("--skills-root", type=Path, default=DEFAULT_SKILLS_ROOT)
    parser.add_argument("--cc", default="cc")
    args = parser.parse_args()
    args.results_dir.mkdir(parents=True, exist_ok=True)
    substitutions = {
        "pm_scripts": str(args.pm_scripts),
        "skills_root": str(args.skills_root),
        "cc": args.cc,
    }

    overall = 0
    table: list[dict] = []
    for name, script, extra in SUITES:
        log_path = args.results_dir / f"{name}.log"
        json_path = args.results_dir / f"{name}.json"
        cmd = [
            sys.executable,
            str(HERE / script),
            "--json",
            str(json_path),
            *[a.format(**substitutions) for a in extra],
        ]
        started = time.monotonic()
        run = subprocess.run(cmd, cwd=REPO_ROOT, text=True, capture_output=True, check=False)
        elapsed = time.monotonic() - started
        log_path.write_text(
            run.stdout + ("\n--- stderr ---\n" + run.stderr if run.stderr else ""), encoding="utf-8"
        )
        counts = count_markers(run.stdout)
        row = {
            "suite": name,
            "exit": run.returncode,
            "elapsed_s": round(elapsed, 2),
            **counts,
            "log": str(log_path),
            "json": str(json_path),
        }
        table.append(row)
        overall |= run.returncode != 0
        print(
            f"{name:14s} exit={run.returncode} pass={counts['PASS']} fail={counts['FAIL']} skip={counts['SKIP']} error={counts['ERROR']} ({elapsed:.1f}s)"
        )
        if run.returncode != 0:
            for line in run.stdout.splitlines():
                if line.startswith(("MIMIC_RESULT: FAIL", "MIMIC_RESULT: ERROR")):
                    print("   ", line)
            if run.stderr.strip():
                print("    stderr:", run.stderr.strip().splitlines()[-1])
    (args.results_dir / "summary.json").write_text(
        json.dumps({"python": sys.version.split()[0], "suites": table}, indent=2), encoding="utf-8"
    )
    total_pass = sum(r["PASS"] for r in table)
    total_fail = sum(r["FAIL"] for r in table)
    total_error = sum(r["ERROR"] for r in table)
    total_skip = sum(r["SKIP"] for r in table)
    print(
        f"TOTAL pass={total_pass} fail={total_fail} skip={total_skip} error={total_error} -> {'OK' if not overall else 'FAILURES'}"
    )
    return 1 if overall else 0


if __name__ == "__main__":
    sys.exit(main())
