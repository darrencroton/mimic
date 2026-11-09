"""
Binned statistics shared by the SAGE26 figures.

Each helper is the expression SAGE26's plotting scripts repeat in several sections.
"""

import warnings

import numpy as np


def binned_median_and_std(x, values, bin_edges):
    """
    Return the median and standard deviation of ``values`` in each bin of ``x``.

    As in SAGE26: np.digitize of x on the edges, then the median and np.std of the values in
    bin i for i = 1 .. len(edges) - 1. An empty bin has no median (NaN).
    """
    digitized = np.digitize(x, bin_edges)
    with warnings.catch_warnings():
        warnings.simplefilter("ignore", RuntimeWarning)
        medians = [np.median(values[digitized == i]) for i in range(1, len(bin_edges))]
        stds = [np.std(values[digitized == i]) for i in range(1, len(bin_edges))]
    return medians, stds


def binned_mean_and_std(masses, values, bins):
    """
    Return the mean and standard deviation of ``values`` in each stellar-mass bin.

    As in SAGE26, bins hold NaN until a bin has a galaxy, and a galaxy is in bin j when
    `masses >= mass_bins[j]` and `masses < mass_bins[j+1]`.
    """
    n_bins = len(bins) - 1
    means = np.full(n_bins, np.nan)
    stds = np.full(n_bins, np.nan)
    for j in range(n_bins):
        mask = (masses >= bins[j]) & (masses < bins[j + 1])
        if np.sum(mask) > 0:
            means[j] = np.mean(values[mask])
            stds[j] = np.std(values[mask])
    return means, stds


def mean_and_error(means_stack):
    """
    Return the mean over snapshots, its standard error, and the bins where both are defined.

    As in SAGE26: `np.nanmean(stack, axis=0)`, `np.nanstd(stack, axis=0) / np.sqrt(len(stack))`
    and `~np.isnan(mean) & ~np.isnan(err)`.
    """
    with warnings.catch_warnings():
        # SAGE26 silences these: an empty bin has no mean
        warnings.simplefilter("ignore", RuntimeWarning)
        mean = np.nanmean(means_stack, axis=0)
        err = np.nanstd(means_stack, axis=0) / np.sqrt(len(means_stack))
    valid = ~np.isnan(mean) & ~np.isnan(err)
    return mean, err, valid
