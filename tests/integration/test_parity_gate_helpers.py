#!/usr/bin/env python3
"""
Parity-gate run-file helpers: the pure functions of tests/framework/parity_gate.py
every cross-format gate uses to prove its two runs share one input. Needs no
dataset or build. Run with: python3 tests/integration/test_parity_gate_helpers.py
"""

import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from framework import run_test_suite  # noqa: E402
from framework.parity_gate import (  # noqa: E402
    assert_horizontal_run_file_diff,
    dynamic_variant,
    functional_lines,
    range_override_variant,
)

VERTICAL = """# vertical run file
simulation:
  name: vert
input:
  first_file: 0
  last_file: 3
SubSteps: 1
output:
  output_directory: output/run-vert
"""
HORIZONTAL = VERTICAL.replace("name: vert", "name: horiz  # package").replace("-vert", "-horiz")


def _write(directory: Path, name: str, text: str) -> Path:
    path = directory / name
    path.write_text(text)
    return path


def _refuses(call) -> bool:
    """True when ``call`` raises AssertionError (the helpers' refusal contract)."""
    try:
        call()
    except AssertionError:
        return True
    return False


def test_dynamic_variant_adds_one_line_and_refuses_existing_scheme():
    with tempfile.TemporaryDirectory() as tmp:
        scratch = Path(tmp)
        base = _write(scratch, "base.yaml", VERTICAL)
        added = dynamic_variant(base, scratch).read_text().splitlines()
        assert len(added) == len(VERTICAL.splitlines()) + 1, "expected exactly one added line"
        assert "TimestepScheme: dynamic" in added, "the added line is not the dynamic scheme"
        carrying = _write(scratch, "carrying.yaml", VERTICAL + "TimestepScheme: fixed\n")
        assert _refuses(lambda: dynamic_variant(carrying, scratch)), "accepted a second scheme"


def test_range_override_rewrites_only_the_range_keys():
    with tempfile.TemporaryDirectory() as tmp:
        scratch = Path(tmp)
        base = _write(scratch, "base.yaml", VERTICAL)
        variant = range_override_variant(base, scratch, (1, 15))
        left, right = functional_lines(VERTICAL), functional_lines(variant.read_text())
        changed = [(a, b) for a, b in zip(left, right) if a != b]
        assert len(left) == len(right), "the override changed the line count"
        expected = [("  first_file: 0", "  first_file: 1"), ("  last_file: 3", "  last_file: 15")]
        assert changed == expected, f"changed lines: {changed}"
        bare = _write(
            scratch, "bare.yaml", VERTICAL.replace("  first_file: 0\n  last_file: 3\n", "")
        )
        assert _refuses(lambda: range_override_variant(bare, scratch, (0, 15))), "no range refused"


def test_horizontal_diff_accepts_two_keys_and_rejects_a_third():
    with tempfile.TemporaryDirectory() as tmp:
        scratch = Path(tmp)
        vertical = _write(scratch, "vertical.yaml", VERTICAL)
        assert_horizontal_run_file_diff(vertical, _write(scratch, "h.yaml", HORIZONTAL), "horiz")
        third = _write(scratch, "third.yaml", HORIZONTAL.replace("SubSteps: 1", "SubSteps: 2"))
        msg = "a third functional change was accepted"
        assert _refuses(lambda: assert_horizontal_run_file_diff(vertical, third, "horiz")), msg


if __name__ == "__main__":
    sys.exit(
        run_test_suite(
            [
                test_dynamic_variant_adds_one_line_and_refuses_existing_scheme,
                test_range_override_rewrites_only_the_range_keys,
                test_horizontal_diff_accepts_two_keys_and_rejects_a_third,
            ],
            "Parity-gate run-file helpers (test_parity_gate_helpers.py)",
        )
    )
