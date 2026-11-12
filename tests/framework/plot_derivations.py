"""Model-neutral machinery for hand-built derivation cases of a package's figures.

A derivation case calls one registered figure on a tiny catalogue whose plotted answer can be
stated as literals, then compares the plotted points of the artist it names with those literals.
It catches a figure that plots the wrong quantity: a mass function not divided by the volume, by
``h^3`` or by the bin width, the wrong selection or split, a missing unit conversion. The generic
cases of ``plot_checks`` only prove that a figure runs and draws something.

The rows (``Derivation``) and the names of the integer-typed fields belong to the package, in its
``plots/tests/``; this module holds only the row type and the code that runs one. A row whose
figure the package does not register is skipped.

Units every row runs in: the Hubble parameter ``H = 0.5`` and a volume of ``ROW_VOLUME = 1000``
(Mpc/h)^3; a package states its own mass unit and conversions in its rows.
"""

from dataclasses import dataclass

import numpy as np

H = 0.5  # the Hubble parameter every row runs at
ROW_VOLUME = 1000.0  # (Mpc/h)^3
RTOL = 1e-5  # float32 inputs: plotted values agree with the literals to well under this


@dataclass(frozen=True)
class Derivation:
    """One hand-built case.

    Attributes:
        name: Case name suffix (``test_derivation__<name>``).
        figure: Registered figure to call.
        data: {field: values} for a snapshot figure; {snapshot: (redshift, {field: values})}
            for an evolution figure.
        expected: {artist label: [(x, y) or (x, y, colour), ...]}. The label is the artist's legend
            label; ``None`` names the artists tagged ``gid="model"`` that carry no label (only a
            package that tags them can use it). The points are compared sorted.
        drop_zero_y: Compare only the points with y > 0 (a mass function is zero in every bin
            that holds no galaxy, so its peaks are the answer).
    """

    name: str
    figure: str
    data: dict
    expected: dict
    drop_zero_y: bool = False


def typed(arrays, integer_fields):
    """Return {field: array}: int32 for a field named in ``integer_fields``, else float32.

    A hand-built record types its fields as the output does: floats are float32, and the package
    names the fields that hold integers.
    """
    return {
        name: np.asarray(values, dtype=np.int32 if name in integer_fields else np.float32)
        for name, values in arrays.items()
    }


def artist_points(artist, drop_zero_y=False):
    """Return the sorted finite points of a line or scatter, with the colour value if it has one.

    Each point is (x, y), or (x, y, c) for a scatter coloured by an array. A point whose x or y
    is not finite is not drawn, so it is left out.
    """
    from matplotlib.collections import PathCollection

    if isinstance(artist, PathCollection):
        offsets = np.ma.filled(np.ma.asarray(artist.get_offsets(), dtype=float), np.nan)
        colours = artist.get_array()
        columns = [offsets[:, 0], offsets[:, 1]]
        if colours is not None:
            columns.append(np.ma.filled(np.ma.asarray(colours, dtype=float), np.nan))
        table = np.column_stack(columns)
    else:
        table = np.asarray(artist.get_xydata(), dtype=float)
    table = table[np.isfinite(table).all(axis=1)]
    if drop_zero_y:
        table = table[table[:, 1] > 0.0]
    return sorted(tuple(float(v) for v in row) for row in table)


def artists_labelled(fig, label):
    """Return the artists of a figure with a legend label, or tagged gid="model" for None."""
    found = []
    for ax in fig.axes:
        for artist in ax.get_children():
            if label is None:
                # Matplotlib names an unlabelled artist "_child<n>"
                if artist.get_gid() == "model" and artist.get_label().startswith("_"):
                    found.append(artist)
            elif artist.get_label() == label:
                found.append(artist)
    return found


def derivation_case(harness, row, integer_fields):
    """Return the test callable of one Derivation for a PlotHarness."""

    def case():
        if harness.is_evolution(row.figure):
            data = {
                snap: (z, typed(arrays, integer_fields)) for snap, (z, arrays) in row.data.items()
            }
            snapshots = harness.hand_built(data, volume=ROW_VOLUME, hubble_h=H)
            galaxies, kwargs = snapshots, {}
        else:
            arrays = typed(row.data, integer_fields)
            n = len(next(iter(arrays.values())))
            galaxies = np.zeros(n, dtype=[(k, v.dtype) for k, v in arrays.items()]).view(
                np.recarray
            )
            for key, values in arrays.items():
                galaxies[key] = values
            metadata = harness.fixtures.snapshot_metadata(0.0, n)
            metadata["hubble_h"] = H
            kwargs = {"volume": ROW_VOLUME, "metadata": metadata}
        params = harness.fixtures.run_parameters()
        params["Hubble_h"] = H
        run = harness.call_figure(
            row.figure, galaxies, f"derivation-{row.name}", params=params, **kwargs
        )
        assert run.skip is None, f"{row.figure} returned a skip message: {run.skip}"
        assert run.fig is not None, f"{row.figure}: no figure was captured"
        for label, expected in row.expected.items():
            artists = artists_labelled(run.fig, label)
            assert len(artists) == 1, f"{row.figure}: {len(artists)} artists labelled {label!r}"
            got = artist_points(artists[0], row.drop_zero_y)
            assert len(got) == len(
                expected
            ), f"{row.figure} {label!r}: points {got}, want {expected}"
            for point, want in zip(got, sorted(expected), strict=True):
                assert np.allclose(point, want, rtol=RTOL, atol=1e-9), (
                    f"{row.figure} {label!r}: plotted {point}, expected {want} "
                    f"(all points {got})"
                )

    case.__name__ = f"test_derivation__{row.name}"
    return case


def derivation_cases(harness, rows, integer_fields):
    """Return the cases of the rows whose figure the harness's package registers.

    ``integer_fields`` names the fields of a hand-built record that hold integers.
    """
    return [
        derivation_case(harness, row, integer_fields) for row in rows if row.figure in harness.names
    ]
