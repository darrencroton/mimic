#!/usr/bin/env python3
"""
Compile and run the rate integrator's manual cost driver, ``tests/manual/rate_cost.c``.

Usage::

    python3 tests/manual/run_rate_cost.py

Standard library only, so it runs under a plain ``python3`` as well as under ``mimic_venv``. It
needs a built tree: the driver includes the generated headers ``src/include/generated/
property_defs.h`` and ``raw_halo_defs.h``, which ``make`` (or ``make generate``) writes; without
them it prints which are missing and exits 2 before compiling anything.

The driver times hand-built stars of 10, 100, 1,000 and 10,000 satellites through the integrator
alone under each scheme (``rate_cost.c`` has the design and the bounds). This script compiles it
against an explicit source list (the integrator and the error module; no Makefile rule) with the
Makefile's optimisation and the house warning set into ``build/rate_cost``, and runs it from the
repository root with its output on the terminal. It exits with the driver's status: 0 when every
cost bound held, 1 when one missed, a segment failed, or the compile or the run failed or timed
out, 2 when the generated headers are missing. It builds and restores nothing else, and its
timings mean something only on a machine doing nothing else.
"""

from __future__ import annotations

import os
import shlex
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
SOURCE = Path("tests") / "manual" / "rate_cost.c"
BINARY = REPO_ROOT / "build" / "rate_cost"
# The units the driver links: the integrator under test and the error module it reports through;
# the driver itself defines the few globals main.c and allvars.c would.
SOURCES = (Path("src") / "core" / "rate_integrator.c", Path("src") / "util" / "error.c")
INCLUDES = ("src", "src/include", "src/include/generated", "src/util", "src/core")
GENERATED_HEADERS = (
    Path("src") / "include" / "generated" / "property_defs.h",
    Path("src") / "include" / "generated" / "raw_halo_defs.h",
)
# The house warning set (AGENTS.md: compile clean under these on Clang and GCC).
HOUSE_WARNINGS = ("-Wall", "-Wextra", "-Wshadow", "-Wformat-security", "-Wundef")
COMPILE_TIMEOUT = 300
RUN_TIMEOUT = 600


def main() -> int:
    missing = [str(path) for path in GENERATED_HEADERS if not (REPO_ROOT / path).is_file()]
    if missing:
        print(
            "The cost driver needs a built tree: the generated header(s) "
            f"{', '.join(missing)} are missing. Run `make` (or `make generate`) from the "
            "repository root first.",
            file=sys.stderr,
        )
        return 2
    BINARY.parent.mkdir(parents=True, exist_ok=True)
    cmd = [os.environ.get("CC", "cc"), "-O2", *HOUSE_WARNINGS]
    cmd += [f"-I{path}" for path in INCLUDES]
    cmd += [str(SOURCE), *map(str, SOURCES), "-lm", "-o", str(BINARY)]
    print(f"$ {shlex.join(cmd)}", flush=True)
    try:
        status = subprocess.run(cmd, cwd=REPO_ROOT, timeout=COMPILE_TIMEOUT).returncode
    except (OSError, subprocess.TimeoutExpired) as error:
        print(f"FAIL: could not compile the cost driver: {error}")
        return 1
    if status != 0:
        print(f"FAIL: compiling the cost driver exited {status}")
        return 1
    print(f"$ {BINARY}", flush=True)
    try:
        return subprocess.run([str(BINARY)], cwd=REPO_ROOT, timeout=RUN_TIMEOUT).returncode
    except subprocess.TimeoutExpired:
        print(f"FAIL: the cost driver timed out after {RUN_TIMEOUT} s")
        return 1


if __name__ == "__main__":
    sys.exit(main())
