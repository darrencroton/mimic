#!/usr/bin/env python

"""
Mimic Plot Output Utilities

Simple shared utilities for consistent output formatting across mimic-plot
and all figure modules. Provides colored warnings and errors when writing
to a TTY, and centralized field checking for adaptive plotting.
"""

import random
import sys

import numpy as np


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


def check_required_fields(galaxies, required_fields, optional_fields=None, plot_name="Plot"):
    """
    Check if required fields exist in galaxy data.

    This function supports Mimic's adaptive plotting architecture by gracefully
    handling missing physics properties when modules are disabled.

    Args:
        galaxies: Galaxy data as numpy recarray
        required_fields: List of field names that MUST exist for this plot
        optional_fields: List of field names that enhance the plot but aren't required
        plot_name: Name of plot for error messages

    Returns:
        Tuple of (success, available_optional, message):
            - success (bool): True if all required fields present
            - available_optional (dict): {field_name: True/False} for optional fields
            - message (str): Empty string on success, error message on failure

    Example:
        >>> success, opts, msg = check_required_fields(
        ...     galaxies,
        ...     required_fields=['StellarMass', 'ColdGas'],
        ...     optional_fields=['Sfr'],
        ...     plot_name='Gas Fraction'
        ... )
        >>> if not success:
        ...     warn(msg)
        ...     return create_empty_plot(msg)
        >>> if opts.get('Sfr'):
        ...     plot_red_blue_separation()
    """
    available = set(galaxies.dtype.names)

    # Check required fields
    missing_required = [f for f in required_fields if f not in available]
    if missing_required:
        msg = f"{plot_name} requires missing field(s): {', '.join(missing_required)}"
        return False, {}, msg

    # Check optional fields
    available_optional = {}
    if optional_fields:
        for field in optional_fields:
            available_optional[field] = field in available

    return True, available_optional, ""


def validate_filtered_data(indices, plot_name, verbose=False):
    """
    Validate that filtered data has results for snapshot plots.

    This function checks if filtering produced non-empty results and returns
    a standardized skip message if not. Unlike the old pattern, this does NOT
    create empty plot files - the caller should skip the plot entirely.

    Args:
        indices: Array of filtered indices (from np.where)
        plot_name: Name of plot for error messages
        verbose: Whether to print warnings

    Returns:
        Tuple of (is_valid, skip_message):
            - is_valid (bool): True if len(indices) > 0, False otherwise
            - skip_message (str or None): Skip reason if invalid, None if valid

    Example (snapshot plot):
        >>> w = np.where(galaxies.StellarMass > 0.0)[0]
        >>> is_valid, skip_msg = validate_filtered_data(w, "Stellar Mass Function", verbose)
        >>> if not is_valid:
        >>>     return None, skip_msg  # Skip plot entirely
    """
    if len(indices) == 0:
        msg = f"No data found for {plot_name} after filtering"
        if verbose:
            warn(msg)
        return False, msg
    return True, None


def validate_evolution_snapshot(indices, redshift, plot_name, verbose=False):
    """
    Validate that filtered data has results for evolution plots.

    This function is used inside snapshot loops in evolution plots to validate
    individual snapshots. Returns False to signal the caller should skip (continue)
    to the next snapshot.

    Args:
        indices: Array of filtered indices (from np.where)
        redshift: Redshift of current snapshot
        plot_name: Name of plot for context
        verbose: Whether to print warnings

    Returns:
        Tuple of (is_valid, skip_message):
            - is_valid (bool): True if len(indices) > 0, False otherwise
            - skip_message (str or None): Skip reason if invalid, None if valid

    Example (evolution plot):
        >>> for snap, (galaxies, volume, metadata) in snapshots.items():
        >>>     w = np.where(galaxies.Mvir > 0.0)[0]
        >>>     is_valid, skip_msg = validate_evolution_snapshot(
        >>>         w, metadata['redshift'], "HMF Evolution", verbose
        >>>     )
        >>>     if not is_valid:
        >>>         continue  # Skip this snapshot
    """
    if len(indices) == 0:
        msg = f"{plot_name}: No data found for z={redshift:.1f}"
        if verbose:
            warn(msg)
        return False, msg
    return True, None


def check_field_has_values(data_array, field_name, threshold=0.0):
    """
    Check if a field has meaningful non-zero values (field-level validation).

    This function validates that a field contains values above a threshold,
    catching cases where all values are zero before any filtering occurs.
    Use this BEFORE filtering to detect all-zero fields early.

    Args:
        data_array: NumPy array to check
        field_name: Name of field for error messages
        threshold: Minimum value to consider meaningful (default: 0.0)

    Returns:
        Tuple of (has_values, count_valid, message):
            - has_values (bool): True if any values > threshold
            - count_valid (int): Number of values > threshold
            - message (str): Error message if no values, empty string otherwise

    Example:
        >>> has_metals, count, msg = check_field_has_values(
        >>>     galaxies.MetalsColdGas, 'MetalsColdGas', threshold=0.0
        >>> )
        >>> if not has_metals:
        >>>     return None, f"Field validation failed: {msg}"
    """
    count_valid = np.sum(data_array > threshold)
    has_values = count_valid > 0

    if not has_values:
        msg = f"All values in '{field_name}' are <= {threshold}"
    else:
        msg = ""

    return has_values, count_valid, msg


def check_field_has_values_any_snapshot(snapshots, field_name, threshold=0.0):
    """
    Check if a field has meaningful non-zero values in ANY snapshot (evolution-plot gate).

    Evolution plots hold multiple snapshots spanning very different epochs. A field can be
    legitimately all-zero at one epoch (e.g. StellarMass before the first star has formed)
    while genuinely populated at others. Checking a single sample snapshot - as
    check_field_has_values() does for single-snapshot plots - produces a false negative if
    that sample happens to be the empty epoch. Use this instead to gate an evolution plot on
    whether the field is populated anywhere across the full snapshot set.

    Args:
        snapshots: Dict mapping snapshot number -> (galaxies, volume, metadata)
        field_name: Name of the field to check (attribute of the galaxies recarray)
        threshold: Minimum value to consider meaningful (default: 0.0)

    Returns:
        Tuple of (has_values, message):
            - has_values (bool): True if any snapshot has values > threshold
            - message (str): Error message if no snapshot has values, empty string otherwise

    Example:
        >>> has_mass, msg = check_field_has_values_any_snapshot(snapshots, 'StellarMass')
        >>> if not has_mass:
        >>>     return None, f"Field validation failed: {msg}"
    """
    for galaxies, _volume, _metadata in snapshots.values():
        if np.sum(getattr(galaxies, field_name) > threshold) > 0:
            return True, ""

    return False, f"All values in '{field_name}' are <= {threshold} in every snapshot"


def setup_figure(figsize=(8, 6)):
    """
    Create and set up a matplotlib figure with consistent styling.

    Args:
        figsize: Tuple of (width, height) in inches

    Returns:
        Tuple of (fig, ax) with fonts already configured

    Example:
        >>> fig, ax = setup_figure()
        >>> ax.plot(x, y)
    """
    import matplotlib.pyplot as plt
    from figures import setup_plot_fonts

    fig, ax = plt.subplots(figsize=figsize)
    setup_plot_fonts(ax)
    return fig, ax


def save_and_close_figure(fig, output_dir, filename, output_format=".png", verbose=False):
    """
    Save and close a matplotlib figure with standardized error handling.

    This centralizes the save/close pattern shared by every figure module.

    Args:
        fig: Matplotlib figure object
        output_dir: Directory to save the figure
        filename: Base filename (without extension)
        output_format: File extension (default: ".png")
        verbose: Print save location if True

    Returns:
        str: Full path to saved file

    Example:
        >>> fig, ax = setup_figure()
        >>> # ... plotting code ...
        >>> return save_and_close_figure(fig, output_dir, "StellarMassFunction", output_format, verbose)
    """
    import os

    import matplotlib.pyplot as plt

    # Ensure output directory exists
    try:
        os.makedirs(output_dir, exist_ok=True)
    except Exception as e:
        warn(f"Could not create output directory {output_dir}: {e}")
        # Fallback to current directory
        output_dir = "./plots"
        os.makedirs(output_dir, exist_ok=True)

    # Construct full path
    output_path = os.path.join(output_dir, f"{filename}{output_format}")

    # Save and close
    if verbose:
        print(f"Saving {filename} to: {output_path}")
    plt.savefig(output_path)
    plt.close(fig)

    return output_path


def calculate_mass_function(mass_array, volume, hubble_h, binwidth=0.1, mi=None, ma=None):
    """
    Calculate a mass function histogram with standardized binning.

    This function provides consistent normalization for all mass functions (halo,
    stellar, baryonic, etc.). The volume parameter is automatically scaled by the
    fraction of files read (good_files/total_files) in read_data(), ensuring
    correct normalization regardless of how many simulation files are processed.

    Args:
        mass_array: Array of log10(mass/Msun) values
        volume: Simulation volume in (Mpc/h)^3 (already scaled by file fraction)
        hubble_h: Hubble parameter h
        binwidth: Histogram bin width in dex (default: 0.1)
        mi: Minimum mass bin (if None, auto-determined)
        ma: Maximum mass bin (if None, auto-determined)

    Returns:
        Tuple of (xaxis, yaxis) for plotting
        - xaxis: Bin centers
        - yaxis: Number density (Mpc^-3 h^3 dex^-1) in comoving coordinates

    Normalization formula:
        phi = counts / volume * h^3 / binwidth

        This gives the comoving number density per dex. The h^3 factor converts
        from (Mpc/h)^-3 to physical units when needed.

    Example:
        >>> mass = np.log10(galaxies.StellarMass[w] * 1.0e10 / hubble_h)
        >>> x, y = calculate_mass_function(mass, volume, hubble_h)
        >>> ax.plot(x, y, 'k-')
    """
    if mi is None:
        mi = np.floor(min(mass_array)) - 2
    if ma is None:
        ma = np.floor(max(mass_array)) + 2

    nbins = int((ma - mi) / binwidth)
    counts, binedges = np.histogram(mass_array, range=(mi, ma), bins=nbins)
    xaxis = binedges[:-1] + 0.5 * binwidth

    # Normalize: counts per comoving volume per dex
    # Volume is already scaled by good_files/total_files in read_data()
    yaxis = counts / volume * hubble_h**3 / binwidth

    return xaxis, yaxis


def get_profile_axes(params, plot_key, xlim_default, ylim_default, log_y=False):
    """
    Read axis limits for a named plot from the active plot profile.

    Looks up axes.<plot_key> in the profile and returns (x_min, x_max, y_min, y_max),
    falling back to xlim_default and ylim_default when keys are absent.

    x values (xmin, xmax) are always used directly.

    y values (ymin, ymax) depend on log_y:
      log_y=True  — profile stores log10 of the limit (e.g. -6.0 → 1e-6). Use for
                    plots with a logarithmic y axis (mass functions, number densities).
      log_y=False — profile stores the limit directly. Use for linear y-axis plots.

    Args:
        params: Mimic params dict containing PlotProfile.
        plot_key: Profile key under axes (e.g. "halo_mass_function").
        xlim_default: (x_min, x_max) fallback tuple.
        ylim_default: (y_min, y_max) fallback tuple (linear values, regardless of log_y).
        log_y: Whether y profile values are log10-encoded (default False).

    Returns:
        (x_min, x_max, y_min, y_max) as floats ready for ax.set_xlim / ax.set_ylim.
    """
    axes = (params.get("PlotProfile") or {}).get("axes", {}).get(plot_key, {})
    x_min = axes.get("xmin", xlim_default[0])
    x_max = axes.get("xmax", xlim_default[1])
    if log_y:
        y_min = 10 ** axes.get("ymin", np.log10(ylim_default[0]))
        y_max = 10 ** axes.get("ymax", np.log10(ylim_default[1]))
    else:
        y_min = axes.get("ymin", ylim_default[0])
        y_max = axes.get("ymax", ylim_default[1])
    return x_min, x_max, y_min, y_max


def make_bin_edges(min_val, max_val, width):
    """
    Build evenly spaced bin edges from min_val to max_val inclusive.

    Prefer this over np.arange(min_val, max_val, width): arange excludes the
    stop value, silently dropping up to one bin width of data at the top of
    the requested range (e.g. a histogram never reaches its own axis max).

    Args:
        min_val: Lower edge of the first bin.
        max_val: Upper edge of the last bin.
        width: Requested bin width in dex (or other unit); the actual width is
            adjusted slightly so an integer number of equal bins exactly spans
            [min_val, max_val].

    Returns:
        Array of nbins+1 edges spanning [min_val, max_val] inclusive.
    """
    nbins = max(1, round((max_val - min_val) / width))
    return np.linspace(min_val, max_val, nbins + 1)


def select_scatter_sample(x, y_arrays, x_min, x_max, y_min, y_max, dilute, rng=None):
    """
    Restrict scatter-plot points to the display axis box, then randomly dilute.

    Filtering to the box before dilution matters whenever there are more
    candidates than the dilution budget: sampling from the full candidate
    range first can spend nearly the whole budget on points that fall outside
    a display window narrower than the candidate population, leaving the
    visible plot almost empty even though plenty of in-range data exists.

    Args:
        x: 1D array of x-axis values for each candidate point.
        y_arrays: A single 1D array, or a list of 1D arrays, of y-axis values
            for each candidate point -- pass a list when several series share
            one x axis and one display box (e.g. stellar/cold/hot/ejected/ICS
            mass all plotted against halo mass). A point is kept if x is in
            range and at least one y series is in range.
        x_min, x_max, y_min, y_max: The plot's display axis (get_profile_axes()).
        dilute: Maximum number of points to keep.
        rng: Object exposing .sample(population, k), for reproducible sampling.
            Defaults to the random module, matching the random.seed(2222)
            convention every scatter figure already uses.

    Returns:
        Array of indices into x (and each array in y_arrays), sized
        min(dilute, number of in-box candidates).
    """
    if rng is None:
        rng = random
    y_list = [y_arrays] if isinstance(y_arrays, np.ndarray) else list(y_arrays)
    in_x = (x >= x_min) & (x <= x_max)
    in_box = np.zeros_like(in_x)
    for y in y_list:
        in_box |= in_x & (y >= y_min) & (y <= y_max)
    indices = np.where(in_box)[0]
    if len(indices) > dilute:
        indices = np.array(rng.sample(list(indices), dilute))
    return indices


# Largest number of grid cells per axis used by periodic_pair_counts(); bounds the cell loop
# when r_edges[-1] is tiny compared to the box. Cells never shrink below r_edges[-1].
_MAX_PAIR_COUNT_CELLS_PER_AXIS = 32

# Most point-pair separations periodic_pair_counts() evaluates at once. A tile holds
# 3 float64 values per pair, so the default bounds the temporaries at about 24 MB however
# many points share a cell.
_PAIR_COUNT_BLOCK_PAIRS = 1_000_000


def _separations_sq(points_a, points_b, box_size):
    """Return the (len(a), len(b)) squared minimum-image separations of two (n, 3) point sets."""
    delta = points_a[:, None, :] - points_b[None, :, :]
    delta -= box_size * np.round(delta / box_size)
    return np.einsum("ijk,ijk->ij", delta, delta)


def _tiles(nrows, ncols):
    """
    Yield (row_lo, row_hi, col_lo, col_hi) tiles covering an nrows x ncols grid.

    Both dimensions are tiled, so no tile has more than _PAIR_COUNT_BLOCK_PAIRS entries
    however large either dimension is.
    """
    budget = max(1, _PAIR_COUNT_BLOCK_PAIRS)
    tile_cols = min(ncols, budget)
    tile_rows = max(1, budget // max(tile_cols, 1))
    for row_lo in range(0, nrows, tile_rows):
        for col_lo in range(0, ncols, tile_cols):
            yield row_lo, min(row_lo + tile_rows, nrows), col_lo, min(col_lo + tile_cols, ncols)


def _validate_pair_count_inputs(positions, box_size, r_edges):
    """
    Validate the input domain shared by periodic_pair_counts() and correlation_function().

    Returns:
        (positions, box_size, r_edges) as float64 arrays/scalar.

    Raises:
        ValueError: with an explicit message for each domain violation.
    """
    try:
        box_size = float(box_size)
    except (TypeError, ValueError) as exc:
        raise ValueError(f"box_size must be a number, got {box_size!r}") from exc
    if not np.isfinite(box_size) or box_size <= 0.0:
        raise ValueError(f"box_size must be finite and > 0, got {box_size}")

    positions = np.asarray(positions, dtype=np.float64)
    if positions.ndim != 2 or positions.shape[1] != 3:
        raise ValueError(f"positions must have shape (N, 3), got {positions.shape}")
    if not np.all(np.isfinite(positions)):
        raise ValueError("positions must be finite (found NaN or Inf)")
    if positions.size and (positions.min() < 0.0 or positions.max() >= box_size):
        raise ValueError(
            f"positions must lie inside [0, box_size) = [0, {box_size}); "
            f"found range [{positions.min()}, {positions.max()}]"
        )

    r_edges = np.asarray(r_edges, dtype=np.float64)
    if r_edges.ndim != 1 or r_edges.size < 2:
        raise ValueError("r_edges must be a 1-D array of at least two edges")
    if not np.all(np.isfinite(r_edges)):
        raise ValueError("r_edges must be finite")
    if r_edges[0] < 0.0:
        raise ValueError(f"r_edges must be non-negative, got first edge {r_edges[0]}")
    if np.any(np.diff(r_edges) <= 0.0):
        raise ValueError("r_edges must be strictly increasing")
    if r_edges[-1] > 0.5 * box_size:
        raise ValueError(
            f"r_edges[-1] = {r_edges[-1]} exceeds box_size / 2 = {0.5 * box_size}: "
            "the minimum-image separation is not defined beyond half the box"
        )
    return positions, box_size, r_edges


def _neighbour_offsets(ncell):
    """Return the unique cell offsets {-1, 0, 1} along one axis for a grid of ncell cells."""
    return sorted({(offset % ncell) for offset in (-1, 0, 1)})


def periodic_pair_counts(positions, box_size, r_edges):
    """
    Count distinct pairs of points per separation bin in a periodic cubic box.

    Uses a cell grid whose cells are at least r_edges[-1] wide, so every pair closer than
    r_edges[-1] lies in the same or a neighbouring cell, and the minimum-image convention
    for the separation. Numpy only. Cost scales with the number of near-neighbour pairs
    rather than N^2, and evaluates pairs in tiles of at most _PAIR_COUNT_BLOCK_PAIRS separations
    so memory does not grow with the occupancy of a cell.

    Each unordered pair is counted once and no point is paired with itself (two distinct
    points at the same position are a pair at separation 0). Bins are half-open,
    [r_edges[i], r_edges[i+1]); a pair exactly at r_edges[-1] is not counted.

    Args:
        positions: (N, 3) array of finite coordinates inside [0, box_size).
        box_size: Box side length, finite and > 0, in the same units as positions.
        r_edges: 1-D strictly increasing, non-negative bin edges with
            r_edges[-1] <= box_size / 2.

    Returns:
        Integer array of len(r_edges) - 1 pair counts. All zeros when N < 2.

    Raises:
        ValueError: if any input violates the domain above.
    """
    positions, box_size, r_edges = _validate_pair_count_inputs(positions, box_size, r_edges)
    nbins = len(r_edges) - 1
    counts = np.zeros(nbins, dtype=np.int64)
    npoints = len(positions)
    if npoints < 2:
        return counts

    edges_sq = r_edges**2
    ncell = int(min(max(np.floor(box_size / r_edges[-1]), 1), _MAX_PAIR_COUNT_CELLS_PER_AXIS))
    cell_width = box_size / ncell
    cell_xyz = np.minimum((positions / cell_width).astype(np.int64), ncell - 1)
    cell_id = (cell_xyz[:, 0] * ncell + cell_xyz[:, 1]) * ncell + cell_xyz[:, 2]

    order = np.argsort(cell_id, kind="stable")
    sorted_pos = positions[order]
    sorted_cell = cell_id[order]
    occupied, starts, sizes = np.unique(sorted_cell, return_index=True, return_counts=True)
    extent = {int(cid): (int(s), int(s + n)) for cid, s, n in zip(occupied, starts, sizes)}

    offsets = _neighbour_offsets(ncell)

    def accumulate(r_sq):
        idx = np.searchsorted(edges_sq, r_sq, side="right") - 1
        idx = idx[(idx >= 0) & (idx < nbins)]
        np.add(counts, np.bincount(idx, minlength=nbins), out=counts)

    for cid, (lo, hi) in extent.items():
        cx, rem = divmod(cid, ncell * ncell)
        cy, cz = divmod(rem, ncell)
        block = sorted_pos[lo:hi]
        npts = hi - lo

        # Pairs inside the cell, each once: tile the upper triangle (column index > row index),
        # skipping tiles that lie wholly on or below the diagonal.
        for r_lo, r_hi, c_lo, c_hi in _tiles(npts - 1, npts - 1):
            if c_hi <= r_lo:  # largest column point index c_hi, smallest row index r_lo
                continue
            r_sq = _separations_sq(block[r_lo:r_hi], block[c_lo + 1 : c_hi + 1], box_size)
            later = (np.arange(c_lo + 1, c_hi + 1)[None, :]) > np.arange(r_lo, r_hi)[:, None]
            accumulate(r_sq[later])

        # Pairs with each distinct neighbouring cell of larger id, so a cell pair is visited
        # once (per-axis offsets that wrap onto each other on a coarse grid are deduplicated).
        for dx in offsets:
            for dy in offsets:
                for dz in offsets:
                    nid = (((cx + dx) % ncell) * ncell + (cy + dy) % ncell) * ncell + (
                        (cz + dz) % ncell
                    )
                    if nid <= cid or nid not in extent:
                        continue
                    nlo, nhi = extent[nid]
                    other = sorted_pos[nlo:nhi]
                    for r_lo, r_hi, c_lo, c_hi in _tiles(npts, len(other)):
                        accumulate(_separations_sq(block[r_lo:r_hi], other[c_lo:c_hi], box_size))

    return counts


def correlation_function(positions, box_size, r_edges):
    """
    Real-space two-point correlation function of a point set in a periodic cubic box.

    xi = DD / RR_analytic - 1, where DD is periodic_pair_counts() and the random-pair
    expectation in a periodic box is analytic: RR = N (N - 1) / 2 * V_shell / box_size^3
    with V_shell = 4 pi / 3 (r_hi^3 - r_lo^3). No random catalogue is needed.

    The error bar is the Poisson error on DD propagated through the estimator,
    sqrt(DD) / RR = (1 + xi) / sqrt(DD). It is zero for a bin with no pairs; such a bin has
    xi = -1 and carries no information. It treats pair counts as independent, so it is
    approximate for strongly clustered samples.

    Args:
        positions: (N, 3) array of finite coordinates inside [0, box_size).
        box_size: Box side length, finite and > 0.
        r_edges: 1-D strictly increasing, non-negative bin edges with
            r_edges[-1] <= box_size / 2.

    Returns:
        (xi, xi_err, dd, rr): float arrays xi and xi_err, integer pair counts dd, and the
        analytic expected random pair counts rr, each of len(r_edges) - 1. With N < 2,
        dd = 0, xi = NaN, xi_err = 0 and rr = 0 (no division by zero).

    Raises:
        ValueError: if any input violates the domain (see periodic_pair_counts).
    """
    positions, box_size, r_edges = _validate_pair_count_inputs(positions, box_size, r_edges)
    dd = periodic_pair_counts(positions, box_size, r_edges)
    npoints = len(positions)
    nbins = len(r_edges) - 1
    if npoints < 2:
        return np.full(nbins, np.nan), np.zeros(nbins), dd, np.zeros(nbins)

    shell_volume = 4.0 * np.pi / 3.0 * (r_edges[1:] ** 3 - r_edges[:-1] ** 3)
    rr = 0.5 * npoints * (npoints - 1) * shell_volume / box_size**3
    xi = dd / rr - 1.0
    xi_err = np.sqrt(dd) / rr
    return xi, xi_err, dd, rr
