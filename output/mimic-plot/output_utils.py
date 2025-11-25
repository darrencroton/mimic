#!/usr/bin/env python

"""
Mimic Plot Output Utilities

Simple shared utilities for consistent output formatting across mimic-plot
and all figure modules. Provides colored warnings and errors when writing
to a TTY.
"""

import sys


def colour_enabled():
    """Return True if stdout is a TTY and colours should be used."""
    return sys.stdout.isatty()


def warn(msg: str):
    """Print a warning message, coloured yellow when writing to a TTY."""
    if colour_enabled():
        print(f"\x1b[33mWARNING: {msg}\x1b[0m")
    else:
        print(f"WARNING: {msg}")


def error(msg: str):
    """Print an error message, coloured red when writing to a TTY."""
    if colour_enabled():
        print(f"\x1b[31mERROR: {msg}\x1b[0m")
    else:
        print(f"ERROR: {msg}")
