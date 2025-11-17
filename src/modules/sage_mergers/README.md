# SAGE Mergers Module

**Status**: Partially Complete (Core physics implemented, see Deferred Features below)
**Version**: 1.0.0
**Author**: Mimic Team (ported from SAGE)
**Date**: November 2025

---

## Overview

This module implements galaxy merger physics from the SAGE (Semi-Analytic Galaxy Evolution) model. It handles the complex processes that occur when galaxies merge, including dynamical friction, merger-induced starbursts, black hole growth, and morphological transformations.

**Key Physics Processes:**
1. **Dynamical Friction**: Calculates merger timescales for satellites (Binney & Tremaine 1987)
2. **Galaxy Mergers**: Combines properties of merging galaxies
3. **Black Hole Growth**: Merger-driven accretion (Kauffmann & Haehnelt 2000)
4. **Quasar Feedback**: Powerful AGN winds from BH accretion
5. **Starbursts**: Merger-induced star formation (Somerville et al. 2001, Cox thesis)
6. **Morphology**: Disk-to-bulge transformations for major mergers
7. **Disruption**: Satellite tidal stripping to intracluster stars

---

## Implementation Status

### ✅ Fully Implemented

**1. Merger Timescale Calculation** (`estimate_merging_time`)
- Binney & Tremaine (1987) dynamical friction formula
- Accounts for particle counts, masses, and orbital parameters
- Returns merger time in Gyr

**2. Galaxy Combination** (`add_galaxies_together`)
- Transfers all baryonic components (gas, stars, BH)
- Adds stellar mass to bulge component
- Conserves metals in all reservoirs
- **Partial**: SFH arrays deferred (see below)

**3. Black Hole Growth** (`grow_black_hole`)
- Kauffmann & Haehnelt (2000) accretion model
- Depends on mass ratio, Vvir, and cold gas availability
- Tracks quasar-mode accretion mass

**4. Quasar-Mode Feedback** (`quasar_mode_wind`)
- Energy-driven gas ejection
- Compares quasar energy to gas binding energies
- Can eject both cold and hot gas

**5. Morphological Transformations** (`make_bulge_from_burst`)
- Converts disk to bulge for major mergers
- Implements violent relaxation
- **Partial**: SFH arrays deferred (see below)

**6. Merger-Induced Starbursts** (`collisional_starburst_recipe`)
- Somerville et al. (2001) model with Cox thesis coefficients
- Calculates burst efficiency: ε = 0.56 * mass_ratio^0.7
- Implements star formation and feedback inline
- Metal production and distribution (Krumholz & Dekel 2011)
- **Partial**: Disk instability deferred (see below)

**7. Satellite Disruption** (`disrupt_satellite_to_ICS`)
- Handles complete tidal disruption
- Transfers stars to intracluster component
- Transfers gas to central hot reservoir

**8. Update Helper Functions**
- `update_from_star_formation`: Implements instantaneous recycling
- `update_from_feedback`: Two-stage feedback (reheating + ejection)

---

### ⏸️ Deferred Features

#### **1. Star Formation History (SFH) Arrays**

**What's Missing:**
- `SfrDisk[]`, `SfrBulge[]` arrays (STEPS-sized time bins)
- `SfrDiskColdGas[]`, `SfrBulgeColdGas[]` tracking arrays
- `SfrDiskColdGasMetals[]`, `SfrBulgeColdGasMetals[]` tracking arrays

**Why Deferred:**
- Requires design decision on STEPS constant and array management
- Need to determine: fixed vs. dynamic array sizes
- Need to determine: storage in GalaxyData vs. separate structure
- These arrays are used for post-processing (magnitudes, colors), not core physics

**Impact:**
- Core merger physics works without SFH arrays
- Cannot calculate photometric properties (requires separate module anyway)
- Mass and metal budgets are fully tracked

**Future Work:**
1. Design SFH array system (see `docs/architecture/next-task.md`)
2. Add SFH properties to `galaxy_properties.yaml` (array type support needed)
3. Implement SFH tracking in `add_galaxies_together()` and `make_bulge_from_burst()`
4. Update transfer logic in merger functions

**Estimated Effort**: 1-2 weeks (including design, implementation, testing)

#### **2. Disk Instability Checking**

**What's Missing:**
- `check_disk_instability()` function calls after minor mergers
- Disk stability criterion (Efstathiou, Lake & Negroponte 1982)
- Triggered bulge formation from unstable disks

**Why Deferred:**
- Requires separate `disk_instability` module (SAGE Priority 6)
- Currently disabled via `DiskInstabilityOn = 0` parameter
- Minor mergers still work correctly (disk preserved, bulge grows)

**Impact:**
- Minor mergers don't check for post-merger disk instability
- Misses some disk-to-bulge transformations that should occur
- Major mergers still correctly transform to spheroids

**Future Work:**
1. Implement `sage_disk_instability` module (separate effort)
2. Call `check_disk_instability()` after minor mergers in `collisional_starburst_recipe()`
3. Enable `DiskInstabilityOn` parameter when module is ready

**Estimated Effort**: 1-2 weeks for separate disk_instability module

---

## Integration with Core

**Important**: This module provides physics functions but **does not handle merger detection**. Full merger processing requires integration with core tree traversal logic.

**Current Implementation:**
- Module registers and initializes successfully
- Physics functions are available for core to call
- `process_halos()` is a placeholder

**Full Implementation Would Require:**
1. Core tracks `MergTime` for satellites
2. Core calls `deal_with_galaxy_merger()` when `MergTime` expires
3. Core calls `disrupt_satellite_to_ICS()` for tidally stripped satellites
4. Core updates merger timescales using `estimate_merging_time()`

**This is by design**: Merger triggering logic belongs in core tree processing, not in physics modules (Vision Principle #1: Physics-Agnostic Core).

---

## Usage

### Parameter File Configuration

```
# Enable the mergers module
EnabledModules  sage_infall,sage_cooling,sage_mergers

# Merger physics parameters
SageMergers_BlackHoleGrowthRate       0.01    # BH growth efficiency
SageMergers_QuasarModeEfficiency      0.001   # Quasar feedback efficiency
SageMergers_ThreshMajorMerger         0.3     # Major merger threshold
SageMergers_RecycleFraction           0.43    # Stellar recycling
SageMergers_Yield                     0.03    # Metal yield
SageMergers_FracZleaveDisk            0.3     # Metal distribution
SageMergers_FeedbackReheatingEpsilon  3.0     # Feedback reheating
SageMergers_FeedbackEjectionEfficiency 0.3    # Feedback ejection

# Feedback toggles
SageMergers_AGNrecipeOn               1       # Enable AGN feedback
SageMergers_SupernovaRecipeOn         1       # Enable SN feedback
SageMergers_DiskInstabilityOn         0       # DEFERRED - keep at 0
```

---

## References

**Primary References:**
- Binney & Tremaine (1987) - *Galactic Dynamics* - Dynamical friction
- Somerville et al. (2001) - ApJ, 547, 521 - Merger-induced starbursts
- Kauffmann & Haehnelt (2000) - MNRAS, 311, 576 - BH growth during mergers
- Cox (PhD thesis) - Updated starburst efficiency coefficients
- Krumholz & Dekel (2011) - ApJ, 729, 36 - Metal distribution

**SAGE Implementation:**
- Croton et al. (2016) - ApJS, 222, 22 - SAGE model description
- SAGE source code: `sage-code/model_mergers.c`
