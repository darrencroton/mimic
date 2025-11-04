#!/usr/bin/env python

"""
Setup script for the Mimic Plotting Tool package.

This file defines the package metadata, dependencies, and entry points
for the Mimic Plotting Tool, which provides a centralized system for
generating plots from Mimic galaxy evolution framework outputs.
"""

from setuptools import find_packages, setup

setup(
    name="mimic-plot",
    version="0.1.0",
    description="Centralized plotting tool for the Mimic galaxy evolution framework",
    author="Mimic Development Team",
    packages=find_packages(),
    entry_points={
        "console_scripts": [
            "mimic-plot=mimic_plot:main",
        ],
    },
    install_requires=[
        "numpy",
        "matplotlib",
        "tqdm",
    ],
    python_requires=">=3.6",
)
