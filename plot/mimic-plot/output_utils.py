#!/usr/bin/env python

"""
Mimic Plot Output Utilities

Simple shared utilities for consistent output formatting across mimic-plot
and all figure modules. Provides colored warnings and errors when writing
to a TTY, and centralized field checking for adaptive plotting.
"""

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
