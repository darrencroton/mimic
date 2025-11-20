# SAGE Figure Import Summary

## Overview
Successfully imported 12 new plotting figures from sage-plot to mimic-plot, following the established mimic-plot design patterns.

## New Figures Added

### Snapshot Plots (9 new):
1. **baryonic_mass_function.py** - Baryonic (stars + cold gas) mass function
2. **gas_mass_function.py** - Cold gas mass function with observational data
3. **baryonic_tully_fisher.py** - Baryonic Tully-Fisher relation
4. **specific_sfr.py** - Specific star formation rate vs stellar mass
5. **black_hole_bulge_relation.py** - Black hole mass vs bulge mass relation
6. **gas_fraction.py** - Gas fraction vs stellar mass
7. **metallicity.py** - Gas-phase metallicity vs stellar mass
8. **bulge_mass_fraction.py** - Bulge mass fraction vs total stellar mass
9. **quiescent_fraction.py** - Fraction of quiescent galaxies vs stellar mass
10. **mass_reservoir_scatter.py** - Scatter plot of different mass reservoirs

### Evolution Plots (2 new):
1. **sfr_density_evolution.py** - Star formation rate density evolution with redshift
2. **stellar_mass_density_evolution.py** - Stellar mass density evolution with redshift

## Changes Made

### 1. Added Utility Functions to `figures/__init__.py`:
- `get_baryonic_mass_label()` - Labels for baryonic mass plots
- `get_gas_mass_label()` - Labels for gas mass plots
- `get_sfr_density_label()` - Labels for SFR density plots
- `get_ssfr_label()` - Labels for specific SFR plots
- `get_black_hole_mass_label()` - Labels for black hole mass plots
- `get_bulge_mass_label()` - Labels for bulge mass plots

### 2. Updated `figures/__init__.py` Registration:
- Added imports for all 12 new figure modules
- Added new figures to `SNAPSHOT_PLOTS` list (now 18 total)
- Added new figures to `EVOLUTION_PLOTS` list (now 4 total)
- Added property requirements to `PLOT_REQUIREMENTS` dictionary
- Added plot functions to `PLOT_FUNCS` dictionary

### 3. Property Requirements:
The new figures require various galaxy properties:
- **StellarMass** - already in galaxy_properties.yaml ✓
- **ColdGas** - already in galaxy_properties.yaml ✓
- **HotGas** - already in galaxy_properties.yaml ✓
- **MetalsColdGas** - already in galaxy_properties.yaml ✓
- **BulgeMass** - already in galaxy_properties.yaml ✓
- **BlackHoleMass** - already in galaxy_properties.yaml ✓
- **SfrDisk, SfrBulge** - NOT YET in galaxy_properties.yaml (will be added later)
- **DiskScaleRadius** - already in galaxy_properties.yaml ✓

As per the design, figures will automatically skip if required properties don't exist in the data file.

## Design Compliance

All imported figures follow mimic-plot design patterns:
- ✓ Use standard utility functions from `figures` package
- ✓ Consistent font sizing (AXIS_LABEL_SIZE, TICK_LABEL_SIZE, etc.)
- ✓ Proper error handling for empty selections
- ✓ Support for verbose mode debugging
- ✓ Graceful fallback for missing properties
- ✓ Consistent output directory handling

## File Counts

- **Total figure modules**: 22 (10 original + 12 new)
- **Total snapshot plots**: 18
- **Total evolution plots**: 4
- **Total plot functions**: 22

## Validation

- ✓ All Python files pass syntax validation
- ✓ All figures properly registered in `__init__.py`
- ✓ Property requirements documented
- ✓ Utility functions added for consistent labeling

## Notes

The figures are ready to use. Those requiring SfrDisk/SfrBulge properties will be automatically skipped until those properties are added to the galaxy_properties.yaml file and computed by appropriate physics modules.
