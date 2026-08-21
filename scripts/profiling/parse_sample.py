#!/usr/bin/env python3
"""Parser for /usr/bin/sample "Call graph:" output.

    parse_sample.py <sample.txt>

Run directly, it prints a self-check of one report: frame count, total samples, and the
number of frames whose self-time came out negative.

Builds the sample call tree and computes per-frame SELF samples (count minus the sum of
the direct children's counts).  Callers aggregate those; see attribute.py.

Line grammar observed on macOS 26 / sample report version 7:

    <gutter><count> <symbol>  (in <binary>) [+ <off>]  [<addr>]  [<abs source path>:<line>]

The gutter is built from ' ', '+', '!', ':', '|' characters; the column at which the count
starts encodes tree depth, so depth is recovered with a monotonic column stack rather than
by assuming a fixed indent step.

API:
    parse_file(path) -> (roots, total_samples)
    walk(nodes) -> iterator over every node, depth first
    Node: count, self_, symbol, binary, srcfile, line, children, parent, col
"""

import argparse
import re

FRAME_RE = re.compile(r"^(?P<pre>[ +!:|]*?)(?P<count>\d+) (?P<rest>.*?)\s*$")
# symbol  (in binary) + off  [addr]  path:line
REST_RE = re.compile(
    r"^(?P<sym>.*?)\s+\(in (?P<bin>[^)]*)\)"
    r"(?:\s*\+\s*(?P<off>[0-9][0-9,]*(?:\.\.\.)?))?"
    r"(?:\s*\[(?P<addr>[^\]]*)\])?"
    r"(?:\s+(?P<src>\S+):(?P<line>\d+))?\s*$"
)

THREAD_BINARY = "<thread>"


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


def _section_bounds(lines, path):
    """First and last line indices of the call-graph body, or raise ValueError."""
    try:
        start = next(i for i, l in enumerate(lines) if l.startswith("Call graph:")) + 1
    except StopIteration as exc:
        raise ValueError(f"{path}: no 'Call graph:' section; not a sample report") from exc
    try:
        end = next(
            i for i, l in enumerate(lines) if i > start and l.startswith("Total number in stack")
        )
    except StopIteration as exc:
        raise ValueError(f"{path}: call graph never ends; report is truncated") from exc
    return start, end


def parse_file(path):
    """Parse one sample report.

    Args:
        path: a `sample` text report.

    Returns:
        (roots, total_samples), where roots are the per-thread top frames and
        total_samples is the sum of their counts.

    Raises:
        ValueError: the file is not a complete sample report, or a frame line
            does not match the expected grammar.
    """
    with open(path, "r", errors="replace") as fh:
        lines = fh.read().split("\n")
    start, end = _section_bounds(lines, path)

    stack = []  # (column, node), strictly increasing in column
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
        # Thread headers ("Thread_2158546   DispatchQueue_1: ...") carry no binary.
        rm = None if rest.startswith("Thread_") else REST_RE.match(rest)
        if rm:
            symbol = rm.group("sym").strip()
            binary = rm.group("bin").strip()
            srcfile = rm.group("src")
            line = int(rm.group("line")) if rm.group("line") else None
        else:
            symbol = rest.strip()
            binary = THREAD_BINARY
            srcfile = None
            line = None
        node = Node(count, symbol, binary, srcfile, line, col)
        while stack and stack[-1][0] >= col:
            stack.pop()
        if stack:
            node.parent = stack[-1][1]
            node.parent.children.append(node)
        else:
            roots.append(node)
            total += count
        stack.append((col, node))

    for root in roots:
        _assign_self_samples(root)
    return roots, total


def _assign_self_samples(node):
    """Set self_ on node and its descendants: own count minus the direct children's."""
    remaining = node.count
    for child in node.children:
        remaining -= child.count
        _assign_self_samples(child)
    node.self_ = remaining


def walk(nodes):
    """Yield every node in the given subtrees, depth first."""
    for node in nodes:
        yield node
        yield from walk(node.children)


def main():
    parser = argparse.ArgumentParser(description="Self-check one /usr/bin/sample report.")
    parser.add_argument("report", help="a sample text report to parse")
    args = parser.parse_args()

    roots, total = parse_file(args.report)
    nodes = list(walk(roots))
    negative = [n for n in nodes if n.self_ < 0]
    print(f"total {total} frames {len(nodes)}")
    print(f"negative-self frames: {len(negative)}")
    print(f"sum of self: {sum(n.self_ for n in nodes)}")


if __name__ == "__main__":
    main()
