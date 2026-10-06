"""The version 3 routes the converter names as evidenced, and every string that names them.

A conversion is not a validated route. Mimic's ``horizontal_hdf5`` reader and horizontal driver
consume format version 3, but a route is supported only where a recorded parity gate passed. The
authoritative list, with models, data and evidence, is the table under
``convert/mimic-convert/HORIZONTAL-HDF5-FORMAT.md#v3-runtime-support``; :data:`ROUTES` mirrors its five rows
and is the one place in the converter that restates them.

Three renderers derive the text the converter shows from that one constant, so the runtime
notice every stage prints, the CLI description and the conversion report's limitations cannot
drift apart: :func:`runtime_notice`, :func:`cli_description` and :func:`standing_limitations`.
When the spec table gains or loses a route, change :data:`ROUTES` and nothing else in code.
"""

from typing import NamedTuple, Tuple

#: Where the evidenced routes, their coverage, models and evidence are recorded.
SPEC_ANCHOR = "convert/mimic-convert/HORIZONTAL-HDF5-FORMAT.md#v3-runtime-support"

#: The coverage label of a route whose source data is the whole simulation.
COMPLETE = "complete"

FULL_UCHUU_NOT_CLAIMED = (
    "Full Uchuu is not claimed: it exceeds whole-slab memory, and although Mimic's chunked "
    "sweeps (input.forest_chunks) now bound a run's memory by a chunk, full Uchuu stays "
    "unclaimed because it is storage-bound before it is memory-bound, and its largest forest "
    "bounds any chunk."
)


class Route(NamedTuple):
    """One parity-gated version 3 route."""

    simulation: str
    source_format: str
    coverage: str
    models: Tuple[str, ...]


#: The five parity-gated routes, in the spec table's order.
ROUTES: Tuple[Route, ...] = (
    Route("mini-Millennium", "lhalo_binary", COMPLETE, ("halos-only", "sage16")),
    Route("micro-Uchuu", "lhalo_binary", COMPLETE, ("halos-only",)),
    Route("micro-Uchuu", "consistent_trees_hdf5", COMPLETE, ("halos-only",)),
    Route("Millennium", "lhalo_binary", COMPLETE, ("halos-only",)),
    Route("mini-Uchuu", "lhalo_binary", COMPLETE, ("halos-only",)),
)


def _route_clause(route: Route) -> str:
    """One route as ``<simulation> <format>, <coverage>, <models>``."""
    return "{} {}, {}, {}".format(
        route.simulation, route.source_format, route.coverage, " and ".join(route.models)
    )


def runtime_notice() -> str:
    """The line every conversion stage prints so a success is never read as a validated route."""
    return (
        "runtime support: format version 3 is consumed by the current Mimic horizontal reader "
        "and driver, but a conversion is not a validated route. The only evidenced routes "
        "(per-UniqueGalaxyID bitwise parity against the same source format's vertical reader, "
        "recorded at {}) are: {}. {}"
    ).format(SPEC_ANCHOR, "; ".join(_route_clause(r) for r in ROUTES), FULL_UCHUU_NOT_CLAIMED)


def cli_description() -> str:
    """The runtime-support sentence of the ``convert_trees`` argparse description."""
    return (
        "The current Mimic reads version 3, but converted output is a validated route only "
        "where a recorded parity gate passed ({}; see each stage's runtime-support line); "
        "full Uchuu is not claimed."
    ).format("; ".join(_route_clause(r) for r in ROUTES))


def standing_limitations() -> Tuple[str, ...]:
    """The route limitations the version 3 conversion report states for every dataset."""
    return (
        "Format consumed, route not validated: Mimic's horizontal_hdf5 reader and horizontal "
        "driver consume format_version 3, but a conversion is an evidenced runtime route only "
        "where a recorded parity gate showed its horizontal output bitwise identical, per "
        "UniqueGalaxyID, to the same source format's vertical reader over the same files "
        "({}).".format(SPEC_ANCHOR),
        "The only evidenced routes are: {}.".format("; ".join(_route_clause(r) for r in ROUTES)),
        FULL_UCHUU_NOT_CLAIMED,
    )
