# SAGE Satellite Stripping Module

Environmental gas stripping from satellite galaxies in the SAGE (Semi-Analytic Galaxy Evolution) model.

## Physics Overview

When a galaxy becomes a satellite within a larger dark matter halo, it experiences environmental processes that strip hot gas from its reservoir:

1. **Ram Pressure Stripping**: Hot gas is removed as the satellite moves through the hot halo of the central galaxy
2. **Tidal Stripping**: Gravitational tides from the host halo can remove loosely bound gas
3. **Reionization Suppression**: The characteristic mass for gas retention increases after cosmic reionization (Gnedin 2000)

This module implements these processes as a simple prescription: satellites lose hot gas when their current baryon content exceeds the cosmological expectation (accounting for reionization suppression). The stripped gas is transferred to the central galaxy's hot gas reservoir.

## Implementation

### Stripping Calculation

For each Type 1 satellite galaxy in a FOF group:

```
expected_baryons = BARYON_FRAC × M_vir × reionization_modifier
current_baryons = M_stars + M_cold + M_hot + M_ejected + M_BH + M_ICS
stripped_gas = (expected_baryons - current_baryons) / STEPS
```

Where:
- `BARYON_FRAC`: Cosmic baryon fraction (Ω_b / Ω_m)
- `reionization_modifier`: Suppression factor from Gnedin (2000) model
- `STEPS`: Number of sub-steps for time integration (currently 1)

### Reionization Model

This module uses the shared `reionization.h` utility which implements:
- **Model**: Gnedin (2000) with Kravtsov et al. (2004) fitting formulas
- **Parameters**: Hardcoded in header for easy model swapping
- **Default values**: z₀ = 8.0, z_r = 7.0

To use a different reionization model, simply replace `src/modules/shared/reionization.h` with an alternative implementation.

## Module Architecture

### Vision Principles Applied

1. **Runtime Modularity**: Can be enabled/disabled independently from `sage_infall`
2. **Single Source of Truth**: Reionization physics in shared header, not duplicated
3. **KISS**: Simple, focused module doing one thing well

### Dependencies

**Requires:**
- `HotGas`: Hot gas reservoir in satellites
- `MetalsHotGas`: Metals in hot gas

**Provides:**
- None (modifies existing properties in-place)

### Processing

- **Scope**: Processes Type 1 satellites only (not Type 2)
- **Central Target**: Transfers stripped gas to Type 0 central galaxy
- **Conservation**: Maintains metal mass ratios during transfer

## Usage

### Basic Configuration

```yaml
# Enable satellite stripping (default: enabled)
EnabledModules:
  - sage_satellite_stripping

# Module parameters
sage_satellite_stripping_BaryonFrac: 0.17  # Cosmic baryon fraction
```

### Disable Stripping

```yaml
# Disable satellite stripping while keeping infall
EnabledModules:
  - sage_infall
  # sage_satellite_stripping disabled
```

### Change Reionization Model

To use a different reionization model:

1. Create new header (e.g., `reionization_okamoto2008.h`)
2. Archive current: `mv reionization.h _archive/shared/reionization_gnedin2000.h`
3. Install new: `cp new_model.h reionization.h`
4. Rebuild: `make clean && make`

No changes to module code required!

## Physics Validation

### Expected Behaviors

1. **Satellite Baryon Content**: Satellites should have lower baryon fractions than centrals
2. **Mass Dependence**: Low-mass satellites more strongly affected (reionization suppression)
3. **Redshift Evolution**: Stronger stripping at low redshift (post-reionization)
4. **Metal Transfer**: Metallicity preserved during gas transfer

### Key Tests

- Hot gas decreases in satellites over time
- Central hot gas increases from satellite contributions
- Total baryon mass conserved within FOF group
- Metal mass ratios preserved

## References

- **Croton et al. (2006)**: SAGE galaxy evolution model
  - *MNRAS*, 365, 11
  - Original satellite stripping implementation

- **Gnedin (2000)**: Reionization suppression model
  - *ApJ*, 542, 535
  - Effect of reionization on structure formation

- **Kravtsov et al. (2004)**: Fitting formulas for suppression
  - *ApJ*, 609, 35
  - Appendix B: Numerical fitting formulas

## Development Notes

### Extraction from sage_infall

This module was extracted from `sage_infall` to improve modularity:
- Original: Single module handling infall + stripping
- Refactored: Separate modules with shared reionization utility
- Benefit: Can enable/disable independently, clearer separation of physics

### STEPS Integration

The division by `STEPS` prepares for future multi-step time integration:
- **Current**: STEPS = 1 (single step per snapshot)
- **Future**: STEPS > 1 (sub-stepping within snapshot)
- **Location**: Local `#define` with TODO comment for future integration

### Shared Utilities Pattern

This module demonstrates the shared utilities pattern:
- Physics common to multiple modules → shared header
- Each module includes and calls shared functions
- Benefits: DRY principle, easy model swapping, clear API
