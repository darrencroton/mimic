#!/usr/bin/env python3
"""Shared console output helpers for Mimic's scripts/ tooling.

Single source of truth for ANSI colour and the ERROR:/WARNING: console
conventions that the generator and validator scripts previously each
re-implemented. Importable as a flat sibling of ``discovery.py``::

    from console import RED, GREEN, YELLOW, BLUE, NC
    from console import print_error, print_warning, print_ok

Colour is emitted only when stdout is a TTY and ``NO_COLOR`` is unset, so piped
output (CI logs, ``make ... > file``) stays free of escape codes.
"""

from __future__ import annotations

import os
import sys

# Honour the de-facto NO_COLOR standard (https://no-color.org) and suppress
# colour for non-interactive stdout (pipes, redirected logs, CI).
_COLOR_ENABLED = sys.stdout.isatty() and "NO_COLOR" not in os.environ


def _code(value: str) -> str:
    return value if _COLOR_ENABLED else ""


RED = _code("\033[0;31m")
GREEN = _code("\033[0;32m")
YELLOW = _code("\033[1;33m")
BLUE = _code("\033[1;34m")
NC = _code("\033[0m")  # reset


def print_error(msg: str) -> None:
    """Print an error to stderr in red with an ``ERROR:`` prefix."""
    print(f"{RED}ERROR: {msg}{NC}", file=sys.stderr)


def print_warning(msg: str) -> None:
    """Print a warning to stdout in yellow with a ``WARNING:`` prefix."""
    print(f"{YELLOW}WARNING: {msg}{NC}")


def print_ok(msg: str) -> None:
    """Print a success line to stdout in green."""
    print(f"{GREEN}{msg}{NC}")
