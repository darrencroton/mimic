"""Candidate cut rules over the co-membership graph, and union-find components.

A **candidate rule** is three thresholds on a co-membership pair's merged
record (``census/graph.py``): it keeps the edge between two trees iff

- its distinct-snapshot count is at least ``d``,
- its halo count is at least ``h``, and
- its ``Mvir`` sum is at least ``m``,

where any of the three may be unbounded (``None``, written ``any``). A rule is
named by its thresholds, ``d=<d>,h=<h>,m=<m>``, in every output: the name is
the directory its results are written under and the key every summary uses.
``m`` is written as the shortest decimal that reads back as the same float,
so the name is lossless (``parse_rule(rule.name) == rule``).
The rule with all three unbounded keeps every edge; its components are the
complete effective graph's (:data:`COMPLETE`).

Components are found by **union-find** over the kept edges
(:func:`union_find`): a disjoint-set forest in which every node's parent is a
node of smaller or equal index, built by hooking the larger of two roots under
the smaller and compressing every path between batches. Edges are streamed in
batches (vectorised hooking with ``np.minimum.at``), and rounds over the edge
stream repeat until no kept edge joins two roots, so the result does not
depend on the batch size. Each node's final parent is its component's
smallest node index; with nodes numbered in ascending tree root id, that is
the component's smallest tree root.

Resident memory (measured with ``tracemalloc``): the parent array, 4 B per
node (int32), and one parent-sized temporary while a batch's paths are
compressed, 8 B per node in all (about 0.84 GB for the Shin-Uchuu
super-forest's 104,845,278 trees), plus about 40 B per edge of the batch (0.17
GB for a batch of 2^22 edges).
"""

import os
import sys
from dataclasses import dataclass
from typing import Callable, Iterable, Optional, Tuple

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from errors import ConverterError  # noqa: E402

#: The threshold keys of a rule specification, in naming order.
RULE_KEYS = ("d", "h", "m")

#: How an unbounded threshold is written.
UNBOUNDED = "any"


def _format_mass(value: float) -> str:
    """The shortest decimal that reads back as exactly ``value`` (``repr``), so
    distinct thresholds never share a name and ``parse_rule(rule.name) == rule``."""
    return repr(float(value))


@dataclass(frozen=True)
class Rule:
    """Edge-keeping thresholds; ``None`` is unbounded (module docstring)."""

    min_snapshots: Optional[int] = None
    min_halos: Optional[int] = None
    min_mass: Optional[float] = None

    @property
    def name(self) -> str:
        """``d=<d>,h=<h>,m=<m>`` with ``any`` for an unbounded threshold."""
        parts = (
            UNBOUNDED if self.min_snapshots is None else str(self.min_snapshots),
            UNBOUNDED if self.min_halos is None else str(self.min_halos),
            UNBOUNDED if self.min_mass is None else _format_mass(self.min_mass),
        )
        return ",".join("{}={}".format(key, part) for key, part in zip(RULE_KEYS, parts))

    @property
    def keeps_every_edge(self) -> bool:
        return self.min_snapshots is None and self.min_halos is None and self.min_mass is None

    def record(self) -> dict:
        """The rule as a JSON record: its name and its three thresholds."""
        return {
            "name": self.name,
            "min_snapshots": self.min_snapshots,
            "min_halos": self.min_halos,
            "min_mass": self.min_mass,
        }

    def keep(self, snapshots: np.ndarray, halos: np.ndarray, mass: np.ndarray) -> np.ndarray:
        """Whether each pair, given its merged distinct-snapshot count, halo
        count and mass sum, is kept."""
        kept = np.ones(np.shape(snapshots), dtype=bool)
        if self.min_snapshots is not None:
            kept &= np.asarray(snapshots) >= self.min_snapshots
        if self.min_halos is not None:
            kept &= np.asarray(halos) >= self.min_halos
        if self.min_mass is not None:
            kept &= np.asarray(mass) >= self.min_mass
        return kept


#: The rule keeping every edge: its components are the complete graph's.
COMPLETE = Rule()


def parse_rule(text: str) -> Rule:
    """A rule from ``d=<int>,h=<int>,m=<float>``; an omitted key, or the value
    ``any``, is unbounded. ``d`` and ``h`` are non-negative integers, ``m`` a
    finite number.

    Raises:
        ConverterError: on an empty specification, an unknown or repeated key,
            or a value of the wrong kind.
    """
    values = {}
    parts = [part.strip() for part in str(text).split(",")]
    if not any(parts):
        raise ConverterError("rule {!r}: expected d=<int>,h=<int>,m=<float>".format(text))
    for part in parts:
        key, sep, value = part.partition("=")
        key, value = key.strip(), value.strip()
        if not sep or key not in RULE_KEYS or not value:
            raise ConverterError(
                "rule {!r}: {!r} is not one of d=..., h=..., m=...".format(text, part)
            )
        if key in values:
            raise ConverterError("rule {!r}: {} given twice".format(text, key))
        if value == UNBOUNDED:
            values[key] = None
            continue
        try:
            number = int(value) if key in ("d", "h") else float(value)
        except ValueError:
            number = None
        if number is None or (key in ("d", "h") and number < 0) or not np.isfinite(number):
            kind = "a non-negative integer" if key in ("d", "h") else "a finite number"
            raise ConverterError(
                "rule {!r}: {}={!r} is not {} or {}".format(text, key, value, kind, UNBOUNDED)
            )
        values[key] = number
    return Rule(values.get("d"), values.get("h"), values.get("m"))


# ---- union-find ----------------------------------------------------------------

EdgeBatches = Callable[[], Iterable[Tuple[np.ndarray, np.ndarray]]]


def _compress(parent: np.ndarray) -> None:
    """Point every node at its root (pointer jumping until flat), in place."""
    while True:
        grand = parent[parent]
        if np.array_equal(grand, parent):
            return
        parent[:] = grand


def union_find(n_nodes: int, batches: EdgeBatches) -> np.ndarray:
    """Components of the graph on ``n_nodes`` nodes whose edges ``batches()``
    streams as ``(u, v)`` index arrays; called once per round, so it must
    restart the stream on every call.

    Returns:
        int32 (int64 beyond 2^31 nodes) array: each node's component root, the
        component's smallest node index.

    Raises:
        ConverterError: on an edge endpoint outside ``[0, n_nodes)``.
    """
    dtype = np.int32 if n_nodes <= np.iinfo(np.int32).max else np.int64
    parent = np.arange(n_nodes, dtype=dtype)
    while True:
        hooked = False
        for u, v in batches():
            u = np.asarray(u, dtype=np.int64)
            v = np.asarray(v, dtype=np.int64)
            if u.size == 0:
                continue
            low = min(int(u.min()), int(v.min()))
            high = max(int(u.max()), int(v.max()))
            if low < 0 or high >= n_nodes:
                raise ConverterError(
                    "union-find edge endpoint outside [0, {}) (min {}, max {})".format(
                        n_nodes, low, high
                    )
                )
            # parent is flat at the start of every batch, so parent[x] is x's root
            root_u, root_v = parent[u], parent[v]
            differ = root_u != root_v
            if not differ.any():
                continue
            root_u, root_v = root_u[differ], root_v[differ]
            np.minimum.at(parent, np.maximum(root_u, root_v), np.minimum(root_u, root_v))
            _compress(parent)
            hooked = True
        if not hooked:
            return parent


def component_count(roots: np.ndarray) -> int:
    """The number of components in a :func:`union_find` result."""
    return int(np.count_nonzero(roots == np.arange(roots.size, dtype=roots.dtype)))
