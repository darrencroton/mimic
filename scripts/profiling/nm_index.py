#!/usr/bin/env python3
"""Build a symbol -> defining source file index from the mimic object tree.

    nm_index.py [-o OUT]

`build/obj` mirrors the `src/` and `models/` layout, so an object path maps back to a
source path by stripping the `build/obj` prefix and swapping `.o` for `.c`.  The index
resolves stack frames that `sample` leaves without a `file:line` annotation: static
helpers, cold-split fragments, and compiler-outlined functions.

Only Mach-O text symbols are indexed -- type `T` (external) or `t` (local, meaning a
static function), carrying the leading underscore that marks a C symbol on Mach-O.  That
excludes assembler-local labels such as `l_.str`, `lCPI0_0`, and `ltmp0`, which repeat in
nearly every translation unit and can never appear as a stack frame.

A symbol defined in more than one translation unit is ambiguous: a static helper of the
same name in two modules, or a compiler-generated `OUTLINED_FUNCTION_N`.  Such symbols are
omitted rather than resolved by walk order, so their frames land in attribute.py's explicit
Unattributed bucket instead of being credited to whichever object happened to be indexed
first.  Ambiguity is reported on stderr so the omission is visible.
"""

import argparse
import collections
import json
import os
import subprocess
import sys

REPO = os.path.realpath(
    os.environ.get(
        "MIMIC_REPO",
        os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))),
    )
)
DEFAULT_OUTPUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "nm_index.json")

# Mach-O nm type letters for defined text (code) symbols.
TEXT_SYMBOL_TYPES = frozenset({"T", "t"})


def object_sources(obj_root):
    """Yield (object path, repository-relative source path) in a deterministic order.

    Sorting matters: an unsorted walk makes the reported ambiguity set depend on
    filesystem order, which differs between machines and checkouts.
    """
    for root, dirs, files in os.walk(obj_root):
        dirs.sort()
        for name in sorted(files):
            if name.endswith(".o"):
                obj = os.path.join(root, name)
                yield obj, os.path.relpath(obj, obj_root)[:-2] + ".c"


def text_symbols(obj):
    """Defined text symbols in one object file, with the Mach-O underscore stripped."""
    result = subprocess.run(["nm", "-U", obj], capture_output=True, text=True, check=True)
    symbols = []
    for line in result.stdout.splitlines():
        fields = line.split()
        if len(fields) != 3:
            continue
        _address, kind, name = fields
        if kind in TEXT_SYMBOL_TYPES and name.startswith("_"):
            symbols.append(name[1:])
    return symbols


def build_index(obj_root):
    """Return (index, ambiguous): symbols owned by exactly one source, and the rest.

    Args:
        obj_root: the `build/obj` tree to index.

    Returns:
        A (dict, dict) pair mapping symbol -> source path and symbol -> sorted owner list.
    """
    owners = collections.defaultdict(set)
    for obj, src in object_sources(obj_root):
        for symbol in text_symbols(obj):
            owners[symbol].add(src)
    index = {s: next(iter(o)) for s, o in owners.items() if len(o) == 1}
    ambiguous = {s: sorted(o) for s, o in owners.items() if len(o) > 1}
    return index, ambiguous


def main():
    parser = argparse.ArgumentParser(description="Index mimic text symbols by source file.")
    parser.add_argument(
        "-o",
        "--output",
        default=DEFAULT_OUTPUT,
        help="where to write the index (default: %(default)s, where attribute.py looks)",
    )
    args = parser.parse_args()

    obj_root = os.path.join(REPO, "build", "obj")
    if not os.path.isdir(obj_root):
        parser.error(f"no object tree at {obj_root}: build mimic before indexing its symbols")
    index, ambiguous = build_index(obj_root)
    if not index:
        parser.error(f"no defined text symbols under {obj_root}: is this a Mach-O build?")

    with open(args.output, "w") as fh:
        json.dump(index, fh, indent=0, sort_keys=True)
    print(f"indexed {len(index)} symbols -> {args.output}", file=sys.stderr)
    if ambiguous:
        examples = ", ".join(sorted(ambiguous)[:3])
        print(
            f"omitted {len(ambiguous)} symbols defined in more than one translation unit "
            f"({examples}...); their frames stay Unattributed",
            file=sys.stderr,
        )


if __name__ == "__main__":
    main()
