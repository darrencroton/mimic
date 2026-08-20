#!/usr/bin/env python3
"""Parser for /usr/bin/sample "Call graph:" output.

Builds the sample call tree, computes per-frame SELF samples
(count - sum of direct children's counts) and INCLUSIVE samples for
outermost occurrences (recursion-safe), and aggregates by
(source file, symbol) and by (source file, line, symbol).

Line grammar observed on macOS 26 / sample report version 7:

    <gutter><count> <symbol>  (in <binary>) [+ <off>]  [<addr>]  [<abs source path>:<line>]

The gutter is built from ' ', '+', '!', ':', '|' characters; the column at
which the count starts encodes tree depth, so depth is recovered with a
monotonic column stack rather than by assuming a fixed indent step.

API:
    parse_file(path) -> Sample(root_nodes, total)
    Node: count, self, symbol, binary, srcfile, line, children, parent
"""

import re
import sys

FRAME_RE = re.compile(r"^(?P<pre>[ +!:|]*?)(?P<count>\d+) (?P<rest>.*?)\s*$")
# symbol  (in binary) + off  [addr]  path:line
REST_RE = re.compile(
    r"^(?P<sym>.*?)\s+\(in (?P<bin>[^)]*)\)"
    r"(?:\s*\+\s*(?P<off>[0-9][0-9,]*(?:\.\.\.)?))?"
    r"(?:\s*\[(?P<addr>[^\]]*)\])?"
    r"(?:\s+(?P<src>\S+):(?P<line>\d+))?\s*$"
)


class Node:
    __slots__ = (
        "count",
        "self_",
        "symbol",
        "binary",
        "srcfile",
        "line",
        "children",
        "parent",
        "col",
    )

    def __init__(self, count, symbol, binary, srcfile, line, col):
        self.count = count
        self.self_ = count
        self.symbol = symbol
        self.binary = binary
        self.srcfile = srcfile
        self.line = line
        self.children = []
        self.parent = None
        self.col = col

    def key(self):
        return (self.srcfile, self.symbol, self.binary)


def parse_file(path):
    """Return (roots, total_samples, thread_roots)."""
    with open(path, "r", errors="replace") as fh:
        lines = fh.read().split("\n")
    try:
        start = next(i for i, l in enumerate(lines) if l.startswith("Call graph:")) + 1
    except StopIteration as exc:
        raise ValueError(f"{path}: no Call graph section") from exc
    end = next(
        i for i, l in enumerate(lines) if i > start and l.startswith("Total number in stack")
    )
    stack = []  # list of (col, Node)
    roots = []
    total = 0
    for raw in lines[start:end]:
        if not raw.strip():
            continue
        m = FRAME_RE.match(raw)
        if not m:
            raise ValueError(f"{path}: unparsed frame line: {raw!r}")
        col = len(m.group("pre"))
        count = int(m.group("count"))
        rest = m.group("rest")
        rm = None if rest.startswith("Thread_") else REST_RE.match(rest)
        if False:
            rm = None
        if rm:
            sym = rm.group("sym").strip()
            binary = rm.group("bin").strip()
            src = rm.group("src")
            line = int(rm.group("line")) if rm.group("line") else None
        else:
            # thread header line, e.g. "Thread_2158546   DispatchQueue_1: ..."
            sym = rest.strip()
            binary = "<thread>"
            src = None
            line = None
        node = Node(count, sym, binary, src, line, col)
        while stack and stack[-1][0] >= col:
            stack.pop()
        if stack:
            parent = stack[-1][1]
            parent.children.append(node)
            node.parent = parent
        else:
            roots.append(node)
            total += count
        stack.append((col, node))

    # self samples
    def fix(n):
        s = n.count
        for c in n.children:
            s -= c.count
            fix(c)
        n.self_ = s

    for r in roots:
        fix(r)
    return roots, total


def walk(nodes):
    for n in nodes:
        yield n
        yield from walk(n.children)


def ancestors(n):
    p = n.parent
    while p is not None:
        yield p
        p = p.parent


if __name__ == "__main__":
    roots, total = parse_file(sys.argv[1])
    nodes = list(walk(roots))
    print("total", total, "frames", len(nodes))
    neg = [n for n in nodes if n.self_ < 0]
    print("negative-self frames:", len(neg))
    ssum = sum(n.self_ for n in nodes)
    print("sum of self:", ssum)
