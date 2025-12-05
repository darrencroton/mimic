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


def create_empty_plot_with_message(ax, message, fontsize=14):
    """
    Create a standard empty plot with informative message.

    Args:
        ax: Matplotlib axis object
        message: Message to display
        fontsize: Font size for message text

    Returns:
        The axis object for chaining
    """
    ax.text(
        0.5,
        0.5,
        message,
        horizontalalignment="center",
        verticalalignment="center",
        transform=ax.transAxes,
        fontsize=fontsize,
    )
    return ax


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

    This eliminates the duplicate save/close pattern appearing in all 21 figure files.

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

    Args:
        mass_array: Array of log10(mass/Msun) values
        volume: Simulation volume in (Mpc/h)^3
        hubble_h: Hubble parameter h
        binwidth: Histogram bin width in dex (default: 0.1)
        mi: Minimum mass bin (if None, auto-determined)
        ma: Maximum mass bin (if None, auto-determined)

    Returns:
        Tuple of (xaxis, yaxis) for plotting
        - xaxis: Bin centers
        - yaxis: Number density (Mpc^-3 dex^-1)

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
    yaxis = counts / volume * hubble_h**3 / binwidth

    return xaxis, yaxis
