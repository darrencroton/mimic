#!/usr/bin/env python

"""
SAGE26 Spatial Distribution 3-D Box Plot

Port of the "Plotting the spatial distribution of all galaxies in 3D Box" section of SAGE26's
plotting script (plotting/allresults-local.py:1565-1619): the positions of galaxies with
Mvir > 0 and a stellar mass above 1e9 Msun in the simulation box, coloured by the local density
of the drawn points (a Gaussian kernel density estimate), inside the twelve edges of the box.
The section is the specification; where this port differs it is a permitted deviation below.

Requires: Mvir, StellarMass, Pos

Fields read (units): Mvir and StellarMass (1e10 Msun/h, converted to Msun as SAGE26 converts
them: x 1e10 / hubble_h, allresults-local.py:364-365), Pos (Mpc/h; SAGE26's Posx, Posy and Posz
are Pos[:, 0], Pos[:, 1] and Pos[:, 2]). The box size is metadata["box_size"] (Mpc/h).

Permitted deviations: the sample comes from one random.Random(2222) per figure, not from SAGE26's
script-wide stream (seeded once, allresults-local.py:357), so the drawn subset differs when the
candidates exceed DILUTE.
"""

import random

import matplotlib.pyplot as plt
import numpy as np
from figures import AXIS_LABEL_SIZE, setup_plot_fonts
from figures._sampling import draw_sample
from output_utils import (
    check_required_fields,
    get_profile_axes,
    save_and_close_figure,
)

# Number of galaxies drawn at most: SAGE26's script-level `dilute` (allresults-local.py:73),
# in force here (it is reassigned only at :1735)
DILUTE = 7500


def box_edges(box_size):
    """
    Return the 12 edges of the box as [start, end] pairs of corners.

    SAGE26 allresults-local.py:1583-1588: the eight corners of [0, BoxSize]^3 in SAGE26's
    order, joined bottom square, top square, then the four verticals.
    """
    points = np.array(
        [
            [0, 0, 0],
            [box_size, 0, 0],
            [box_size, box_size, 0],
            [0, box_size, 0],
            [0, 0, box_size],
            [box_size, 0, box_size],
            [box_size, box_size, box_size],
            [0, box_size, box_size],
        ]
    )
    return [
        [points[0], points[1]],
        [points[1], points[2]],
        [points[2], points[3]],
        [points[3], points[0]],
        [points[4], points[5]],
        [points[5], points[6]],
        [points[6], points[7]],
        [points[7], points[4]],
        [points[0], points[4]],
        [points[1], points[5]],
        [points[2], points[6]],
        [points[3], points[7]],
    ]


def plot(
    galaxies,
    volume,
    metadata,
    params,
    output_dir="plots",
    output_format=".png",
    verbose=False,
):
    """
    Create the 3-D box plot.

    Args:
        galaxies: Galaxy data as a numpy recarray
        volume: Simulation volume in (Mpc/h)^3
        metadata: Dictionary with additional metadata
        params: Dictionary with Mimic parameters
        output_dir: Output directory for the plot
        output_format: File format for the output
        verbose: Whether to print verbose output

    Returns:
        Tuple of (plot_path, skip_message):
            - plot_path (str or None): Path to saved plot file if successful
            - skip_message (str or None): Reason for skipping if a field is missing
    """
    success, _, msg = check_required_fields(
        galaxies,
        required_fields=["Mvir", "StellarMass", "Pos"],
        plot_name="Spatial Distribution 3D Box",
    )
    if not success:
        return None, f"Required fields missing: {msg}"

    # Imported here so the figure package loads where SciPy is absent
    from mpl_toolkits.mplot3d.art3d import Line3DCollection
    from scipy.stats import gaussian_kde

    hubble_h = metadata["hubble_h"]
    box_size = metadata["box_size"]
    rng = random.Random(2222)
    x_min, x_max, y_min, y_max = get_profile_axes(
        params, "spatial_distribution_3d_box", (0, box_size), (0, box_size)
    )

    mvir = galaxies.Mvir * 1.0e10 / hubble_h
    stellar_mass = galaxies.StellarMass * 1.0e10 / hubble_h
    pos = galaxies.Pos

    # allresults-local.py:1570-1575: the sample is drawn before the density is computed on it
    w = np.where((mvir > 0.0) & (stellar_mass > 1.0e9))[0]
    w = draw_sample(w, rng, DILUTE)
    xx = pos[w, 0]
    yy = pos[w, 1]
    zz = pos[w, 2]

    if verbose:
        print("Spatial Distribution 3D Box plot debug:")
        print(f"  Number of galaxies plotted: {len(w)}")

    fig = plt.figure(figsize=(8, 8))
    ax = fig.add_subplot(111, projection="3d")
    setup_plot_fonts(ax)

    # allresults-local.py:1577-1580. gaussian_kde needs more points than dimensions, so with
    # three or fewer galaxies there is no density to colour by and nothing is drawn.
    if len(w) > 3:
        xyz = np.vstack([xx, yy, zz])
        density = gaussian_kde(xyz)(xyz)
        # SAGE26 figure parity: allresults-local.py:1580 colours by density with no colour bar.
        # Mirrored as written.
        ax.scatter(xx, yy, zz, s=0.3, c=density, cmap="plasma", alpha=0.8, gid="model")

    # Box edges, allresults-local.py:1582-1591
    ax.add_collection3d(Line3DCollection(box_edges(box_size), colors="k"))

    ax.set_xlabel("X (Mpc/h)", fontsize=AXIS_LABEL_SIZE)
    ax.set_ylabel("Y (Mpc/h)", fontsize=AXIS_LABEL_SIZE)
    ax.set_zlabel("Z (Mpc/h)", fontsize=AXIS_LABEL_SIZE)

    # allresults-local.py:1597-1599
    ax.set_xlim([x_min, x_max])
    ax.set_ylim([y_min, y_max])
    ax.set_zlim([0, box_size])

    # Black panes, no grid and black ticks, as SAGE26 draws them (:1601-1611)
    ax.xaxis.set_pane_color((0.0, 0.0, 0.0, 1.0))
    ax.yaxis.set_pane_color((0.0, 0.0, 0.0, 1.0))
    ax.zaxis.set_pane_color((0.0, 0.0, 0.0, 1.0))
    ax.grid(False)
    ax.tick_params(axis="x", colors="k")
    ax.tick_params(axis="y", colors="k")
    ax.tick_params(axis="z", colors="k")

    # Equal aspect ratio, allresults-local.py:1614
    ax.set_box_aspect([1, 1, 1])

    plot_path = save_and_close_figure(
        fig, output_dir, "SpatialDistribution3D_Box", output_format, verbose
    )
    return plot_path, None
