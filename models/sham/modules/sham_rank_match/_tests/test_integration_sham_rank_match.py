#!/usr/bin/env python3
"""
sham_rank_match - Integration Test

Checks that the rank-density reference table embedded in
test_unit_sham_rank_match.c (between its SHAM_DECIMAL_REFERENCE markers) is exactly
what the independent double-precision reference sham_rank_match_reference.py
computes, so the C table cannot drift from its oracle. The case is pure Python: it
needs no executable and runs under every MODEL/SIMULATION pair.

The fixture-driven end-to-end cases (assigned masses per UniqueGalaxyID, masking,
retirement, determinism and startup rejections on the committed micro-Uchuu
horizontal fixture) are added with the package's run files and battery group.
"""

import json
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[5]
sys.path.insert(0, str(REPO_ROOT / "tests"))
sys.path.insert(0, str(Path(__file__).resolve().parent))

from framework import run_test_suite  # noqa: E402
from sham_rank_match_reference import CASES, LOG_MASS_DECIMALS, case_table  # noqa: E402

UNIT_TEST_SOURCE = Path(__file__).resolve().parent / "test_unit_sham_rank_match.c"

#: One C table row: {"name", h_sim, density, assigned, log10 M*}.
ROW_PATTERN = re.compile(r'\{"([\w-]+)", ([^,]+), ([^,]+), ([01]), ([^}]+)\}')


def test_reference_matches_unit_table():
    """
    Test that the unit test's reference table is the reference script's output.

    Expected: every row of the C table between its SHAM_DECIMAL_REFERENCE markers has
              the reference's case name, h_sim and rank density (C literals parsed with
              float()), the reference's mask/assign outcome, and, when assigned, the
              reference's log10 M* formatted to LOG_MASS_DECIMALS places; masked rows
              carry 0.0. The table has at least eight cases.
    """
    source = UNIT_TEST_SOURCE.read_text()
    block = source.split("SHAM_DECIMAL_REFERENCE_BEGIN")[1].split("SHAM_DECIMAL_REFERENCE_END")[0]
    rows = ROW_PATTERN.findall(block)
    table = {
        name: {
            "params": (float(hubble), float(density)),
            "outcome": "assign" if flag == "1" else "mask",
            "log_mass": log_mass.strip(),
        }
        for name, hubble, density, flag, log_mass in rows
    }
    assert len(rows) == len(table), "case names in the C table must be unique"
    assert len(table) >= 8, f"the C table has {len(table)} cases; at least eight are required"

    reference = case_table()
    reference_params = {name: (hubble, density) for name, hubble, density in CASES}
    assert list(table) == list(reference), f"C cases {list(table)} != reference {list(reference)}"
    for name, (outcome, log_mass) in reference.items():
        row = table[name]
        params_message = f"{name}: C (h, density) {row['params']} != {reference_params[name]}"
        assert row["params"] == reference_params[name], params_message
        assert row["outcome"] == outcome, f"{name}: C table says {row['outcome']}, not {outcome}"
        if outcome == "assign":
            mass_message = f"{name}: C log10 M* {row['log_mass']} != reference {log_mass}"
            assert row["log_mass"] == log_mass, mass_message
            assert len(log_mass.split(".")[1]) == LOG_MASS_DECIMALS
        else:
            assert row["log_mass"] == "0.0", f"{name}: a masked row carries 0.0"
    print(json.dumps({name: f"{o} {m}" for name, (o, m) in reference.items()}))


def main():
    tests = [test_reference_matches_unit_table]
    return run_test_suite(tests, "sham_rank_match (test_integration_sham_rank_match.py)")


if __name__ == "__main__":
    sys.exit(main())
