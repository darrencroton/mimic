"""Constants and helpers shared by the census subcommands."""

import numpy as np

#: Largest value of the int32 row, ordinal and count columns the aggregates hold.
INT32_MAX = int(np.iinfo(np.int32).max)

#: Largest int64, the bound of the weight sums and fresh forest ids.
INT64_MAX = int(np.iinfo(np.int64).max)

#: Examples quoted per reported count, and entries per list of largest items.
N_EXAMPLES = 5
N_LARGEST = 10


def quiet(_message: str) -> None:
    """The default progress log: discards the message."""


def examples(values, limit: int = N_EXAMPLES) -> list:
    """The first ``limit`` of ``values`` as Python numbers, for refusal messages and records."""
    return np.asarray(values)[:limit].tolist()


def in_sorted(haystack: np.ndarray, needles: np.ndarray) -> np.ndarray:
    """Whether each needle occurs in the ascending ``haystack``."""
    if haystack.size == 0:
        return np.zeros(np.shape(needles), dtype=bool)
    position = np.minimum(np.searchsorted(haystack, needles), haystack.size - 1)
    return haystack[position] == needles
